#include "UC1Device.h"

#include <libusb.h>

#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <queue>
#include <span>

namespace uc1 {

namespace {
constexpr uint8_t  kInterface = 0;
constexpr size_t   kInBufSize = 64;

// Keepalive cadence from uc1_02 baseline: 4 distinct FF 1B 01 tokens
// cycling at ~1 Hz. We pace at ~4 Hz (one counter advance every 250 ms)
// which is well inside the envelope SSL 360° uses — comfortably safe
// against the firmware's "connection lost" timeout without flooding.
constexpr auto kKeepaliveInterval = std::chrono::milliseconds(250);
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

    // UC1 init is trivial — no custom sequence needed. The worker thread
    // starts pushing the keepalive stream and zero-GR baseline, and UC1
    // is ready to receive display/LED writes immediately.
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

void UC1Device::workerLoop_()
{
    // Post the first async IN transfer so UC1 events flow immediately,
    // and so the IN endpoint stays drained (OUT flow-control otherwise
    // stalls just like on UF8).
    startBulkRead_();

    // Prime the device: one zero-GR frame so the Bus Comp meter reads
    // 0 dB from the start (rather than whatever garbage the firmware
    // has in memory from the previous session).
    {
        auto f = buildZeroGr();
        int t = 0;
        libusb_bulk_transfer(handle_, kEpOut, f.data(), static_cast<int>(f.size()), &t, 500);
    }

    uint8_t keepaliveCounter = 0;
    auto    lastKeepalive    = std::chrono::steady_clock::now();

    while (!shuttingDown_) {
        // Drain any pending sends; wake for keepalive on timeout.
        std::vector<uint8_t> frame;
        {
            std::unique_lock<std::mutex> lk(pending_->mu);
            pending_->cv.wait_for(lk, std::chrono::milliseconds(50),
                [&] { return !pending_->q.empty() || shuttingDown_; });
            if (shuttingDown_) break;
            if (!pending_->q.empty()) {
                frame = std::move(pending_->q.front());
                pending_->q.pop();
            }
        }

        // Keepalive tick.
        const auto now = std::chrono::steady_clock::now();
        if (now - lastKeepalive >= kKeepaliveInterval) {
            auto ka = buildKeepalive(keepaliveCounter);
            int t = 0;
            libusb_bulk_transfer(handle_, kEpOut, ka.data(),
                                 static_cast<int>(ka.size()), &t, 100);
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
                lastError_ = std::string("bulk OUT failed: ") + libusb_error_name(rc);
                // Keep running — transient errors shouldn't kill the surface.
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
