#include "UC1Device.h"

#include <libusb.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <queue>
#include <span>

namespace uc1 {

// Diagnostic counters — bumped from the worker + IN callback, read from
// the main thread to log bytes-per-second and error rates. Useful for
// "is anything actually happening on the wire?" triage.
struct IoStats {
    std::atomic<uint64_t> outFramesSent{0};
    std::atomic<uint64_t> outBytesSent{0};
    std::atomic<uint64_t> outErrors{0};
    std::atomic<uint64_t> inCallbacks{0};
    std::atomic<uint64_t> inBytes{0};
};
static IoStats g_stats;
// Exposed so main.cpp can poll and ShowConsoleMsg once per second.
uint64_t debugOutFrames()   { return g_stats.outFramesSent.load();   }
uint64_t debugOutBytes()    { return g_stats.outBytesSent.load();    }
uint64_t debugOutErrors()   { return g_stats.outErrors.load();       }
uint64_t debugInCallbacks() { return g_stats.inCallbacks.load();     }
uint64_t debugInBytes()     { return g_stats.inBytes.load();         }

namespace {
constexpr uint8_t  kInterface = 0;
constexpr size_t   kInBufSize = 64;

// Cadence measured from uc1_02 idle baseline:
//   FF 1B 01 <counter>   every 150 ms (4-phase cycle, ~6.8 Hz total)
//   FF 5B 02 <GR-16BE>   every 20 ms  (~50 Hz — primary liveness heartbeat)
// Both streams run continuously. Stop either and UC1 decides the host
// has died, drops back to "Attempting to reconnect to SSL 360°".
constexpr auto kKeepaliveInterval = std::chrono::milliseconds(150);
constexpr auto kGrStreamInterval  = std::chrono::milliseconds(20);
}

struct UC1Device::PendingSend {
    std::mutex              mu;
    std::condition_variable cv;
    std::queue<std::vector<uint8_t>> q;
};

UC1Device::UC1Device()
: pending_(std::make_unique<PendingSend>())
{
}

UC1Device::~UC1Device()
{
    close();
}

bool UC1Device::open()
{
    if (handle_) return true;

    if (libusb_init(&ctx_) < 0) {
        lastError_ = "libusb_init failed";
        return false;
    }
    handle_ = libusb_open_device_with_vid_pid(ctx_, kVid, kPid);
    if (!handle_) {
        lastError_ = "UC1 not found or not accessible — is SSL360Core running? Close it first.";
        libusb_exit(ctx_);
        ctx_ = nullptr;
        return false;
    }

    // macOS/Linux: detach the kernel driver if one is attached. Windows
    // handles this via WinUSB — no-op there.
    if (libusb_kernel_driver_active(handle_, kInterface) == 1) {
        if (libusb_detach_kernel_driver(handle_, kInterface) < 0) {
            lastError_ = "Could not detach kernel driver (likely SSL360Core owns the device)";
            libusb_close(handle_);
            libusb_exit(ctx_);
            handle_ = nullptr;
            ctx_    = nullptr;
            return false;
        }
    }

    const int rc = libusb_claim_interface(handle_, kInterface);
    if (rc < 0) {
        lastError_ = std::string("libusb_claim_interface failed: ") + libusb_error_name(rc);
        libusb_close(handle_);
        libusb_exit(ctx_);
        handle_ = nullptr;
        ctx_    = nullptr;
        return false;
    }

    libusb_clear_halt(handle_, kEpOut);
    libusb_clear_halt(handle_, kEpIn);

    // Handshake. Captured from uc1_23 (SSL 360° cold-start with UC1
    // already plugged in) — these 5 empty-data commands transition UC1
    // from "Attempting to reconnect to SSL 360°" into the fully
    // controllable state. Without this, UC1 shows a half-connected UI
    // (plugin context visible but LEDs dark, GR bar stuck, controls
    // drop the connection on use).
    //
    // Pacing mirrors SSL 360°'s observed inter-frame gaps (~15 / 10 /
    // 70 / 17 / 12 ms). The 70 ms gap between FF 05 and FF 4B is
    // respected — earlier attempts with tighter pacing left UC1 in
    // partial state, so we keep the original timing.
    struct HandshakeStep { uint8_t bytes[4]; int delayMsBefore; };
    static constexpr HandshakeStep kHandshake[] = {
        { {0xFF, 0x01, 0x00, 0x01}, 0   },
        { {0xFF, 0x02, 0x00, 0x02}, 15  },
        { {0xFF, 0x05, 0x00, 0x05}, 10  },
        { {0xFF, 0x4B, 0x00, 0x4B}, 70  },
        { {0xFF, 0x4E, 0x00, 0x4E}, 17  },
    };
    for (const auto& step : kHandshake) {
        if (step.delayMsBefore > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(step.delayMsBefore));
        }
        int t = 0;
        const int rc = libusb_bulk_transfer(handle_, kEpOut,
                                            const_cast<uint8_t*>(step.bytes),
                                            4, &t, 500);
        if (rc < 0) {
            lastError_ = std::string("handshake OUT failed: ") + libusb_error_name(rc);
            libusb_release_interface(handle_, kInterface);
            libusb_close(handle_);
            libusb_exit(ctx_);
            handle_ = nullptr;
            ctx_    = nullptr;
            return false;
        }
    }
    // Short settle before the GR / keepalive stream starts — in the
    // captured reference SSL 360° waits ~12 ms after FF 4E before any
    // further traffic.
    std::this_thread::sleep_for(std::chrono::milliseconds(12));

    shuttingDown_ = false;
    worker_ = std::thread([this]{ workerLoop_(); });

    return true;
}

void UC1Device::close()
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

void UC1Device::send(std::vector<uint8_t> frame)
{
    {
        std::lock_guard<std::mutex> lk(pending_->mu);
        pending_->q.push(std::move(frame));
    }
    pending_->cv.notify_one();
}

void UC1Device::setGainReduction(float dB)
{
    grDb_.store(dB, std::memory_order_relaxed);
}

void UC1Device::workerLoop_()
{
    // Post the first async IN transfer so UC1 events flow immediately,
    // and so the IN endpoint stays drained (OUT flow-control otherwise
    // stalls just like on UF8).
    startBulkRead_();

    // Prime the device: one zero-GR frame so the Bus Comp meter reads
    // 0 dB from the start (rather than whatever garbage the firmware
    // has in memory from the previous session). The worker loop below
    // keeps the GR stream flowing at 50 Hz — UC1's liveness watchdog
    // depends on it.
    {
        auto f = buildZeroGr();
        int t = 0;
        libusb_bulk_transfer(handle_, kEpOut, f.data(), static_cast<int>(f.size()), &t, 500);
    }

    uint8_t keepaliveCounter = 0;
    auto    lastKeepalive    = std::chrono::steady_clock::now();
    auto    lastGr           = std::chrono::steady_clock::now();

    while (!shuttingDown_) {
        // Drain any pending sends; wake for keepalive / GR on timeout.
        // Short sleep so we don't drift past the 20 ms GR cadence.
        std::vector<uint8_t> frame;
        {
            std::unique_lock<std::mutex> lk(pending_->mu);
            pending_->cv.wait_for(lk, std::chrono::milliseconds(10),
                [&] { return !pending_->q.empty() || shuttingDown_; });
            if (shuttingDown_) break;
            if (!pending_->q.empty()) {
                frame = std::move(pending_->q.front());
                pending_->q.pop();
            }
        }

        const auto now = std::chrono::steady_clock::now();

        // GR stream — the primary 50 Hz liveness heartbeat. Always send
        // SOMETHING every ~20 ms or UC1's firmware watchdog drops the
        // connection.
        if (now - lastGr >= kGrStreamInterval) {
            auto gr = buildGrMeter(grDb_.load(std::memory_order_relaxed));
            int t = 0;
            const int rc = libusb_bulk_transfer(handle_, kEpOut, gr.data(),
                                                static_cast<int>(gr.size()), &t, 100);
            if (rc < 0) { g_stats.outErrors.fetch_add(1); lastError_ = libusb_error_name(rc); }
            else         { g_stats.outFramesSent.fetch_add(1); g_stats.outBytesSent.fetch_add(gr.size()); }
            lastGr = now;
        }

        // FF 1B keepalive at ~150 ms per counter advance.
        if (now - lastKeepalive >= kKeepaliveInterval) {
            auto ka = buildKeepalive(keepaliveCounter);
            int t = 0;
            const int rc = libusb_bulk_transfer(handle_, kEpOut, ka.data(),
                                                static_cast<int>(ka.size()), &t, 100);
            if (rc < 0) { g_stats.outErrors.fetch_add(1); lastError_ = libusb_error_name(rc); }
            else         { g_stats.outFramesSent.fetch_add(1); g_stats.outBytesSent.fetch_add(ka.size()); }
            keepaliveCounter = (keepaliveCounter + 1) & 0x03;
            lastKeepalive    = now;
        }

        if (!frame.empty()) {
            int transferred = 0;
            const int rc = libusb_bulk_transfer(handle_, kEpOut,
                                                frame.data(),
                                                static_cast<int>(frame.size()),
                                                &transferred, 500);
            if (rc < 0) {
                g_stats.outErrors.fetch_add(1);
                lastError_ = std::string("bulk OUT failed: ") + libusb_error_name(rc);
            } else {
                g_stats.outFramesSent.fetch_add(1);
                g_stats.outBytesSent.fetch_add(frame.size());
            }
        }

        // Pump libusb event loop so the IN callback fires.
        timeval tv{0, 1000};
        libusb_handle_events_timeout_completed(ctx_, &tv, nullptr);
    }
}

void UC1Device::startBulkRead_()
{
    auto* xfer = libusb_alloc_transfer(0);
    auto* buf  = new uint8_t[kInBufSize];

    libusb_fill_bulk_transfer(
        xfer, handle_, kEpIn,
        buf, kInBufSize,
        &UC1Device::readCallback_,
        this,
        0  // unlimited timeout
    );

    libusb_submit_transfer(xfer);
}

void UC1Device::readCallback_(libusb_transfer* xfer)
{
    auto* self = static_cast<UC1Device*>(xfer->user_data);

    if (xfer->status == LIBUSB_TRANSFER_COMPLETED && xfer->actual_length > 0) {
        std::span<const uint8_t> data{xfer->buffer,
                                      static_cast<size_t>(xfer->actual_length)};

        g_stats.inCallbacks.fetch_add(1);
        g_stats.inBytes.fetch_add(static_cast<size_t>(xfer->actual_length));

        if (self->rawInputHandler_) {
            self->rawInputHandler_(xfer->buffer, static_cast<size_t>(xfer->actual_length));
        }

        if (auto ev = parseButtonEvent(data); ev && self->buttonHandler_) {
            self->buttonHandler_(*ev);
        } else if (auto kn = parseKnobEvent(data); kn && self->knobHandler_) {
            self->knobHandler_(*kn);
        }
    }

    // Resubmit unless shutting down.
    if (!self->shuttingDown_ && xfer->status != LIBUSB_TRANSFER_CANCELLED) {
        libusb_submit_transfer(xfer);
    } else {
        delete[] xfer->buffer;
        libusb_free_transfer(xfer);
    }
}

} // namespace uc1
