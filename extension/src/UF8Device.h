#pragma once
//
// UF8Device — libusb wrapper. Claims UF8 over VID 0x31E9 / PID 0x0021,
// interface 0, and exposes send() for EP 0x02 OUT and an async read loop
// for EP 0x81 IN (button events).
//
// Threading: all libusb calls happen on a dedicated worker thread. send()
// is producer-side (thread-safe, enqueues into a lock-free ring buffer).
//

#include <atomic>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "Protocol.h"

struct libusb_context;
struct libusb_device_handle;
struct libusb_transfer;

namespace uf8 {

class UF8Device {
public:
    using ButtonHandler    = std::function<void(const ButtonEvent&)>;
    using RawInputHandler  = std::function<void(const uint8_t* data, size_t len)>;

    UF8Device();
    ~UF8Device();

    UF8Device(const UF8Device&) = delete;
    UF8Device& operator=(const UF8Device&) = delete;

    // Try to open the UF8. Returns true on success. On failure `lastError()`
    // gives a diagnostic. The most common failure is exclusive claim by
    // SSL360Core — we report that specifically.
    bool open();
    void close();
    bool isOpen() const { return handle_ != nullptr; }

    // Fire-and-forget send. Thread-safe. Buffers the frame and lets the
    // worker thread push it over bulk OUT.
    void send(std::vector<uint8_t> frame);

    // Register a callback for incoming button events. Called on the worker
    // thread — keep it short, hop to REAPER's thread via main_OnCommand or
    // similar if you need to touch REAPER state.
    void setButtonHandler(ButtonHandler h) { buttonHandler_ = std::move(h); }
    void setRawInputHandler(RawInputHandler h) { rawInputHandler_ = std::move(h); }

    const std::string& lastError() const { return lastError_; }

private:
    void workerLoop_();
    void startBulkRead_();
    static void readCallback_(libusb_transfer* xfer);

    libusb_context*       ctx_      = nullptr;
    libusb_device_handle* handle_   = nullptr;
    std::atomic<bool>     shuttingDown_{false};
    std::thread           worker_;
    std::string           lastError_;
    ButtonHandler         buttonHandler_;
    RawInputHandler       rawInputHandler_;

    // Simple protected send queue — replace with lock-free ringbuf if
    // REAPER calls into send() at audio-thread rates. For color pushes at
    // ~human interaction rates a mutex is fine.
    struct PendingSend;
    std::unique_ptr<PendingSend> pending_;
};

} // namespace uf8
