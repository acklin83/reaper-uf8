#include "UF8Device.h"

#include <libusb.h>

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <deque>
#include <thread>

#include "Protocol.h"
#include "init_sequence.inc"
#include "layer_plugin_mixer.inc"

#include <cstdio>

namespace uf8 {

namespace {
constexpr uint16_t kVid       = 0x31E9;
constexpr uint16_t kPid       = 0x0021;
constexpr uint8_t  kInterface = 0;
constexpr uint8_t  kEpOut     = 0x02;
constexpr uint8_t  kEpIn      = 0x81;
constexpr size_t   kInBufSize = 64;
}

struct UF8Device::PendingSend {
    std::mutex              mu;
    std::condition_variable cv;
    std::deque<std::vector<uint8_t>> q;
};

UF8Device::UF8Device()
: pending_(std::make_unique<PendingSend>())
{
}

UF8Device::~UF8Device()
{
    close();
}

bool UF8Device::open()
{
    if (handle_) return true;

    if (libusb_init(&ctx_) < 0) {
        lastError_ = "libusb_init failed";
        return false;
    }
    handle_ = libusb_open_device_with_vid_pid(ctx_, kVid, kPid);
    if (!handle_) {
        lastError_ = "UF8 not found or not accessible — is SSL360Core running? Close it first.";
        libusb_exit(ctx_);
        ctx_ = nullptr;
        return false;
    }

    // macOS/Linux: detach kernel driver if one is attached. On Windows this
    // is a no-op since WinUSB etc. handle that.
    if (libusb_kernel_driver_active(handle_, kInterface) == 1) {
        if (libusb_detach_kernel_driver(handle_, kInterface) < 0) {
            lastError_ = "Could not detach kernel driver (likely SSL360Core owns the device)";
            libusb_close(handle_);
            libusb_exit(ctx_);
            handle_ = nullptr;
            ctx_ = nullptr;
            return false;
        }
    }

    int rc = libusb_claim_interface(handle_, kInterface);
    if (rc < 0) {
        lastError_ = std::string("libusb_claim_interface failed: ") + libusb_error_name(rc);
        libusb_close(handle_);
        libusb_exit(ctx_);
        handle_ = nullptr;
        ctx_ = nullptr;
        return false;
    }

    // Read the iSerialNumber descriptor for Settings → Device display.
    // Best-effort: if the device doesn't advertise one or the string read
    // fails, leave serial_ empty rather than failing open(). Done before
    // the init replay so it lands even if a later step has issues.
    {
        libusb_device_descriptor desc{};
        if (libusb_device* d = libusb_get_device(handle_)) {
            if (libusb_get_device_descriptor(d, &desc) >= 0
                && desc.iSerialNumber != 0)
            {
                unsigned char sbuf[256] = {0};
                const int n = libusb_get_string_descriptor_ascii(
                    handle_, desc.iSerialNumber, sbuf, sizeof(sbuf));
                if (n > 0) serial_.assign(reinterpret_cast<char*>(sbuf), n);
            }
        }
    }

    // Clear any stall left by the prior owner (typically SSL360Core).
    libusb_clear_halt(handle_, kEpOut);
    libusb_clear_halt(handle_, kEpIn);

    // Start the worker thread BEFORE the init replay — the worker pumps
    // async bulk IN reads, and without a drained IN endpoint the OUT
    // pipeline stalls around frame 82 of the init sequence.
    shuttingDown_ = false;
    worker_ = std::thread([this]{ workerLoop_(); });

    // Give the worker a beat to post its first IN transfer.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Replay the SSL-360° wakeup/init sequence captured on 2026-05-07.
    // Frames carry an explicit delay_ms_before for the load-bearing
    // inter-phase pauses in the fader-tanz (SSL waits ~750 ms..3 s between
    // motor-target phases for the motor to physically move). Default 2 ms
    // pacing for everything else matches SSL's observed inter-frame gap.
    for (const auto& f : kInitSequence) {
        if (f.delay_ms_before > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(f.delay_ms_before));
        }
        int transferred = 0;
        int ircode = libusb_bulk_transfer(
            handle_, kEpOut,
            const_cast<uint8_t*>(f.bytes),
            static_cast<int>(f.size),
            &transferred, 1000);
        if (ircode < 0) {
            lastError_ = std::string("init sequence bulk OUT failed: ")
                       + libusb_error_name(ircode);
            shuttingDown_ = true;
            if (worker_.joinable()) worker_.join();
            libusb_release_interface(handle_, kInterface);
            libusb_close(handle_);
            libusb_exit(ctx_);
            handle_ = nullptr;
            ctx_ = nullptr;
            return false;
        }
        // 2 ms baseline pacing matches SSL 360°'s observed inter-frame gap.
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Replay the full state-flood captured when SSL 360° handles a
    // physical Plugin-Mixer-Layer button press. A single FF 66 11 0F
    // command alone doesn't switch modes — UF8 firmware needs the
    // whole fader-position/motor/label/color sequence.
    //
    // We filter out the text-zone frames though: the capture was taken on
    // an SSL session that had "Kick", "Snarer", "OH" etc. on the scribble
    // strips and those demo strings briefly flash before the timer
    // overwrites them. Structural frames (layer mode, color, fader pos,
    // motor, slot-active) stay — only text content is skipped.
    auto isTextFrame = [](const uint8_t* b, size_t sz) {
        if (sz < 5 || b[0] != 0xFF || b[1] != 0x66) return false;
        const uint8_t cmd = b[3];
        return cmd == 0x04   // parameter label / plugin slot name
            || cmd == 0x0B   // upper scribble strip
            || cmd == 0x0C   // O/PdB readout
            || cmd == 0x0E   // lower scribble / value line
            || cmd == 0x17;  // CS Type
    };
    for (const auto& f : kLayerPluginMixerSequence) {
        if (isTextFrame(f.bytes, f.size)) continue;
        int transferred = 0;
        libusb_bulk_transfer(
            handle_, kEpOut,
            const_cast<uint8_t*>(f.bytes),
            static_cast<int>(f.size),
            &transferred, 500);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Plugin-Mixer color bar only renders when each strip's plugin slot is
    // marked "populated". The layer-switch replay left every slot empty
    // (FF 66 02 04 <strip>), which is why color pushes went through at the
    // USB layer but nothing lit up. Send the "populated" 0x03 flag + 8
    // non-empty slot-name frames to activate the bar. The strip names are
    // placeholders — REAPER track names arrive on the scribble-strip via
    // the MCU pipe, and ColorSync drives the bar via FF 66 09 18.
    auto sendFrame = [&](const std::vector<uint8_t>& f) {
        int t = 0;
        libusb_bulk_transfer(handle_, kEpOut,
                             const_cast<uint8_t*>(f.data()),
                             static_cast<int>(f.size()),
                             &t, 500);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    };
    sendFrame(buildPluginSlotActive());
    for (uint8_t s = 0; s < 8; ++s) {
        char name[8];
        std::snprintf(name, sizeof(name), "TRK %u", static_cast<unsigned>(s + 1));
        sendFrame(buildPluginSlotName(s, name));
    }

    // The captured layer-switch replay writes the session text that SSL
    // 360° had on screen at capture time ("Kick", "Snarer", "OH", "-1.0",
    // etc.). Blank every text zone on every strip so the user doesn't see
    // those ghost strings in the ~33 ms before the 30 Hz timer overwrites
    // them with REAPER-derived content.
    for (uint8_t s = 0; s < 8; ++s) {
        sendFrame(buildStripTextUpper(s, ""));
        sendFrame(buildStripTextLower(s, ""));
        sendFrame(buildChannelStripType(s, "    "));
        sendFrame(buildFaderDbReadout(s, "    "));
        sendFrame(buildValueLine(s, ""));
    }

    // LED + LCD brightness is set by main.cpp after open() using the
    // brightness level persisted in REAPER's ExtState. We don't push
    // here so the user's choice survives device re-open.

    // Re-engage all 8 motors after the init sequence. The captured init
    // ends with FF 1D 02 strip 00 (motor DISABLE) for all strips at t=20.76
    // — SSL 360° follows up at t=22.57+ with a per-strip echo+enable+target
    // sweep that drives each fader to its current REAPER track volume.
    // Without this, our subsequent REAPER-volume pushes via buildFaderPosition
    // (FF 1E target) hit limp motors and the faders stay wherever the tanz
    // left them — the user has to physically "abholen" each fader to engage
    // it. Enabling here lets the existing onTimer push path drive the
    // motors normally.
    for (uint8_t s = 0; s < 8; ++s) {
        sendFrame(buildMotorEnable(s, true));
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    return true;
}

void UF8Device::close()
{
    if (!handle_) return;
    shuttingDown_ = true;
    pending_->cv.notify_all();
    if (worker_.joinable()) worker_.join();

    libusb_release_interface(handle_, kInterface);
    libusb_close(handle_);
    handle_ = nullptr;

    libusb_exit(ctx_);
    ctx_ = nullptr;
}

void UF8Device::send(std::vector<uint8_t> frame)
{
    {
        std::lock_guard<std::mutex> lk(pending_->mu);
        pending_->q.push_back(std::move(frame));
    }
    pending_->cv.notify_one();
}

void UF8Device::sendPriority(std::vector<uint8_t> frame)
{
    {
        std::lock_guard<std::mutex> lk(pending_->mu);
        pending_->q.push_front(std::move(frame));
    }
    pending_->cv.notify_one();
}

void UF8Device::workerLoop_()
{
    // Kick off an async read for button events — also drains the IN
    // endpoint so OUT flow-control doesn't stall.
    startBulkRead_();

    // Heartbeat frames: SSL 360° sends these at ~50 Hz as a keepalive.
    // If we stop sending them the UF8 firmware declares "connection lost"
    // and goes back to splash screen.
    //   hb1/hb2: 64-byte keepalive pair (always present)
    //   hb3/hb4: 13-byte Plugin-Mixer-Layer-specific heartbeat pair —
    //            in cap01 (PM mode steady state) these were sent alongside
    //            the 64-byte pair at the same cadence. They may be what
    //            keeps UF8 in Plugin-Mixer-Layer display mode.
    uint8_t hb1[64] = {0xff, 0x66, 0x21, 0x09};  hb1[63] = 0x90;
    uint8_t hb2[64] = {0xff, 0x66, 0x21, 0x0a};  hb2[63] = 0x91;
    // hb3 carries live per-strip GR bytes. Without this the static all-
    // zeros heartbeat raced our buildGrByte writes (same FF 66 09 15
    // opcode), flickering the bottom GR LED.
    uint8_t hb3[13] = {0xff, 0x66, 0x09, 0x15, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    uint8_t hb4[13] = {0xff, 0x66, 0x09, 0x16, 0, 0, 0, 0, 0, 0, 0, 0, 0x85};
    // FF 5B 02 00 00 5D = primary liveness, 50 Hz in cap32 (the most-
    // frequent OUT frame). UC1 uses the same family as its liveness;
    // the f73201c motor-fix worked without it but firmware appears to
    // have degraded behaviour without this stream — adding it 2026-04-28.
    uint8_t hb5[6]  = {0xff, 0x5b, 0x02, 0x00, 0x00, 0x5d};
    auto lastHeartbeat = std::chrono::steady_clock::now();
    auto lastHb5      = std::chrono::steady_clock::now();

    // Plugin-Mixer keepalive: FF 1B 01 <counter 0-3> <chk>. Captured in
    // cap32 (2026-04-25) firing every ~150 ms while SSL 360° is in PM
    // mode. UC1 already does this; UF8 was missing it. Without this
    // keepalive the firmware appears to ignore motor-limp commands —
    // i.e. our FF 1D 02 strip 00 frames make it onto the wire (verified
    // via /tmp/reaper_uf8_motor.log) but the motor stays engaged. Same
    // 4-phase counter pattern as UC1.
    constexpr auto kPmKeepaliveInterval = std::chrono::milliseconds(150);
    auto lastPmKeepalive = std::chrono::steady_clock::now();
    uint8_t pmCounter = 0;

    while (!shuttingDown_) {
        // Drain pending sends — pull up to N frames per iteration so a
        // burst of state pushes (VU, SEL colour, zones) doesn't stall
        // latency-sensitive frames like motor-limp behind them. The
        // worker still wakes on cv.notify_one() from send().
        std::vector<std::vector<uint8_t>> batch;
        {
            std::unique_lock<std::mutex> lk(pending_->mu);
            pending_->cv.wait_for(lk, std::chrono::milliseconds(20),
                [&] { return !pending_->q.empty() || shuttingDown_; });
            if (shuttingDown_) break;
            constexpr size_t kMaxBatch = 16;
            while (!pending_->q.empty() && batch.size() < kMaxBatch) {
                batch.push_back(std::move(pending_->q.front()));
                pending_->q.pop_front();
            }
        }

        // Send heartbeat pair every ~20 ms regardless of pending sends.
        auto now = std::chrono::steady_clock::now();
        if (now - lastHeartbeat >= std::chrono::milliseconds(20)) {
            int t = 0;
            int rc = libusb_bulk_transfer(handle_, kEpOut, hb1, sizeof(hb1), &t, 100);
            traceFrame_('O', hb1, sizeof(hb1), rc);
            rc = libusb_bulk_transfer(handle_, kEpOut, hb2, sizeof(hb2), &t, 100);
            traceFrame_('O', hb2, sizeof(hb2), rc);
            // Stamp live per-strip GR bytes into hb3 + recompute checksum.
            const uint64_t packed = grBytes_.load(std::memory_order_relaxed);
            for (int i = 0; i < 8; ++i)
                hb3[4 + i] = static_cast<uint8_t>((packed >> (i * 8)) & 0xFF);
            uint32_t sum = 0;
            for (int k = 1; k < 12; ++k) sum += hb3[k];
            hb3[12] = static_cast<uint8_t>(sum & 0xFF);
            rc = libusb_bulk_transfer(handle_, kEpOut, hb3, sizeof(hb3), &t, 100);
            traceFrame_('O', hb3, sizeof(hb3), rc);
            rc = libusb_bulk_transfer(handle_, kEpOut, hb4, sizeof(hb4), &t, 100);
            traceFrame_('O', hb4, sizeof(hb4), rc);
            lastHeartbeat = now;
        }
        // FF 5B liveness — cap32 sends this at 50 Hz throughout. UC1
        // uses the same family as primary-liveness; UF8 firmware appears
        // to also expect it.
        if (now - lastHb5 >= std::chrono::milliseconds(20)) {
            int t = 0;
            int rc = libusb_bulk_transfer(handle_, kEpOut, hb5, sizeof(hb5), &t, 100);
            traceFrame_('O', hb5, sizeof(hb5), rc);
            lastHb5 = now;
        }

        // PM keepalive every 150 ms (counter cycles 0..3).
        if (now - lastPmKeepalive >= kPmKeepaliveInterval) {
            uint8_t ka[5] = {0xff, 0x1b, 0x01, pmCounter,
                             static_cast<uint8_t>(0x1b + 0x01 + pmCounter)};
            int t = 0;
            int rc = libusb_bulk_transfer(handle_, kEpOut, ka, sizeof(ka), &t, 100);
            traceFrame_('O', ka, sizeof(ka), rc);
            pmCounter = (pmCounter + 1) & 0x03;
            lastPmKeepalive = now;
        }

        for (auto& frame : batch) {
            int transferred = 0;
            int rc = libusb_bulk_transfer(handle_, kEpOut,
                                          frame.data(),
                                          static_cast<int>(frame.size()),
                                          &transferred, 500);
            if (rc < 0) {
                lastError_ = std::string("bulk OUT failed: ") + libusb_error_name(rc);
                // Keep running; transient errors shouldn't kill the extension.
            }
            traceFrame_('O', frame.data(), frame.size(), rc);
            // Diag: log motor (FF 1D) and fader-pos (FF 1E) frames.
            if (frame.size() >= 5 && frame[0] == 0xFF
                && (frame[1] == 0x1D || frame[1] == 0x1E)) {
                if (FILE* lg = std::fopen("/tmp/reaper_uf8_motor.log", "a")) {
                    const auto t = std::chrono::system_clock::now().time_since_epoch();
                    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t).count();
                    if (frame[1] == 0x1D) {
                        std::fprintf(lg, "[%lld] FF 1D 02 strip=%u en=%u rc=%d t=%d\n",
                                     static_cast<long long>(ms), frame[3], frame[4], rc, transferred);
                    } else {
                        const bool flag = (frame[4] & 0x80) != 0;
                        std::fprintf(lg, "[%lld] FF 1E 03 strip=%u lsb=%02x msb=%02x echo=%d rc=%d t=%d\n",
                                     static_cast<long long>(ms), frame[3], frame[4], frame[5],
                                     flag ? 1 : 0, rc, transferred);
                    }
                    std::fclose(lg);
                }
            }
        }

        // Pump libusb event handling so the IN callback fires.
        timeval tv{0, 1000};  // 1 ms
        libusb_handle_events_timeout_completed(ctx_, &tv, nullptr);
    }
}

void UF8Device::traceFrame_(char dir, const uint8_t* data, size_t len,
                            int rc) const
{
    if (!frameTrace_.load(std::memory_order_relaxed)) return;
    FILE* lg = std::fopen("/tmp/reaper_uf8_frames.log", "a");
    if (!lg) return;
    const auto t  = std::chrono::system_clock::now().time_since_epoch();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t).count();
    std::fprintf(lg, "[%lld] %c rc=%d len=%zu ",
                 static_cast<long long>(ms), dir, rc, len);
    for (size_t i = 0; i < len; ++i) std::fprintf(lg, "%02x", data[i]);
    std::fputc('\n', lg);
    std::fclose(lg);
}

void UF8Device::startBulkRead_()
{
    auto* xfer = libusb_alloc_transfer(0);
    auto* buf = new uint8_t[kInBufSize];

    libusb_fill_bulk_transfer(
        xfer, handle_, kEpIn,
        buf, kInBufSize,
        &UF8Device::readCallback_,
        this,
        0  // unlimited timeout
    );

    libusb_submit_transfer(xfer);
}

void UF8Device::readCallback_(libusb_transfer* xfer)
{
    auto* self = static_cast<UF8Device*>(xfer->user_data);

    if (xfer->status == LIBUSB_TRANSFER_COMPLETED && xfer->actual_length > 0) {
        std::span<const uint8_t> data{xfer->buffer, static_cast<size_t>(xfer->actual_length)};

        self->traceFrame_('I', xfer->buffer,
                          static_cast<size_t>(xfer->actual_length), 0);

        if (self->rawInputHandler_) {
            self->rawInputHandler_(xfer->buffer, static_cast<size_t>(xfer->actual_length));
        }

        if (auto ev = parseButtonEvent(data); ev && self->buttonHandler_) {
            self->buttonHandler_(*ev);
        }
    }

    // Resubmit unless we're shutting down.
    if (!self->shuttingDown_ && xfer->status != LIBUSB_TRANSFER_CANCELLED) {
        libusb_submit_transfer(xfer);
    } else {
        delete[] xfer->buffer;
        libusb_free_transfer(xfer);
    }
}

} // namespace uf8
