#include "UF8Device.h"

#include <libusb.h>

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <queue>
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
    std::queue<std::vector<uint8_t>> q;
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

    // Replay the SSL-360° wakeup/init sequence captured on 2026-04-20.
    // Without this the UF8 stays in "SSL 360 connection lost" state — any
    // frames we send are ignored until it sees this handshake.
    for (const auto& f : kInitSequence) {
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
        // 2 ms pacing matches SSL 360°'s observed inter-frame gap.
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
        pending_->q.push(std::move(frame));
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
    uint8_t hb3[13] = {0xff, 0x66, 0x09, 0x15, 0, 0, 0, 0, 0, 0, 0, 0, 0x84};
    uint8_t hb4[13] = {0xff, 0x66, 0x09, 0x16, 0, 0, 0, 0, 0, 0, 0, 0, 0x85};
    auto lastHeartbeat = std::chrono::steady_clock::now();

    while (!shuttingDown_) {
        // Drain pending sends.
        std::vector<uint8_t> frame;
        {
            std::unique_lock<std::mutex> lk(pending_->mu);
            pending_->cv.wait_for(lk, std::chrono::milliseconds(20),
                [&] { return !pending_->q.empty() || shuttingDown_; });
            if (shuttingDown_) break;
            if (!pending_->q.empty()) {
                frame = std::move(pending_->q.front());
                pending_->q.pop();
            }
        }

        // Send heartbeat pair every ~20 ms regardless of pending sends.
        auto now = std::chrono::steady_clock::now();
        if (now - lastHeartbeat >= std::chrono::milliseconds(20)) {
            int t = 0;
            libusb_bulk_transfer(handle_, kEpOut, hb1, sizeof(hb1), &t, 100);
            libusb_bulk_transfer(handle_, kEpOut, hb2, sizeof(hb2), &t, 100);
            libusb_bulk_transfer(handle_, kEpOut, hb3, sizeof(hb3), &t, 100);
            libusb_bulk_transfer(handle_, kEpOut, hb4, sizeof(hb4), &t, 100);
            lastHeartbeat = now;
        }

        if (!frame.empty()) {
            int transferred = 0;
            int rc = libusb_bulk_transfer(handle_, kEpOut,
                                          frame.data(),
                                          static_cast<int>(frame.size()),
                                          &transferred, 500);
            if (rc < 0) {
                lastError_ = std::string("bulk OUT failed: ") + libusb_error_name(rc);
                // Keep running; transient errors shouldn't kill the extension.
            }
        }

        // Pump libusb event handling so the IN callback fires.
        timeval tv{0, 1000};  // 1 ms
        libusb_handle_events_timeout_completed(ctx_, &tv, nullptr);
    }
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
