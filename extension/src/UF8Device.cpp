#include "UF8Device.h"

#include <libusb.h>

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <queue>

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

    shuttingDown_ = false;
    worker_ = std::thread([this]{ workerLoop_(); });
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
    // Kick off an async read for button events.
    startBulkRead_();

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
