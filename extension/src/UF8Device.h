#pragma once
//
// UF8Device — libusb wrapper. Claims UF8 over VID 0x31E9 / PID 0x0021,
// interface 0, and exposes send() for EP 0x02 OUT and an async read loop
// for EP 0x81 IN (button events).
//
// Threading: all libusb calls happen on a dedicated worker thread. send()
// is producer-side (thread-safe, enqueues into a lock-free ring buffer).
//

#include <array>
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

    // Priority send — inserts at the front of the queue so latency-
    // sensitive frames (motor-limp on touch press) don't wait behind
    // a burst of state pushes (VU, SEL colour, zones).
    void sendPriority(std::vector<uint8_t> frame);

    // Register a callback for incoming button events. Called on the worker
    // thread — keep it short, hop to REAPER's thread via main_OnCommand or
    // similar if you need to touch REAPER state.
    void setButtonHandler(ButtonHandler h) { buttonHandler_ = std::move(h); }
    void setRawInputHandler(RawInputHandler h) { rawInputHandler_ = std::move(h); }

    // Per-strip GR bytes carried in the FF 66 09 15 heartbeat frame.
    // 13-byte heartbeat: FF 66 09 15 <s1>..<s8> <chk>. Worker stamps
    // these into hb3 every 20 ms cycle.
    void setGrBytes(const std::array<uint8_t, 8>& bytes) {
        uint64_t packed = 0;
        for (int i = 0; i < 8; ++i)
            packed |= static_cast<uint64_t>(bytes[i]) << (i * 8);
        grBytes_.store(packed, std::memory_order_relaxed);
    }

    const std::string& lastError() const { return lastError_; }

    // USB iSerialNumber string descriptor, populated on successful open().
    // Empty when no descriptor was advertised or the read failed. Read on
    // the main thread (UI accessor); never mutated after open() returns.
    const std::string& serial() const { return serial_; }

    // Diagnostic — when on, every IN + OUT frame is appended to
    // /tmp/reaper_uf8_frames.log with a timestamp. Used to compare
    // Rea-Sixty's frame stream against an SSL360 baseline when the
    // motor-lock symptom recurs.
    void setFrameTrace(bool on) {
        frameTrace_.store(on, std::memory_order_relaxed);
    }
    bool frameTrace() const {
        return frameTrace_.load(std::memory_order_relaxed);
    }

private:
    void workerLoop_();
    void runInit_();
    void startBulkRead_();
    static void readCallback_(libusb_transfer* xfer);

    // Diagnostic — append a single line per frame to the trace log
    // when frameTrace_ is on. dir: 'O' (out → device) / 'I' (in ← device).
    void traceFrame_(char dir, const uint8_t* data, size_t len,
                     int rc) const;

    libusb_context*       ctx_      = nullptr;
    libusb_device_handle* handle_   = nullptr;
    std::string           serial_;
    std::atomic<bool>     shuttingDown_{false};
    std::atomic<bool>     frameTrace_{false};
    std::atomic<uint64_t> grBytes_{0};
    // True while runInitOnInitThread_() is replaying the boot sequence
    // (handshake → 96-cell zero-fill → fader-tanz → LCD/colour init →
    // motor re-engage). The worker thread skips draining the user-send
    // queue while this is set so REAPER's onTimer pushes don't interleave
    // with init frames; heartbeats + IN reads continue normally.
    std::atomic<bool>     initInProgress_{false};
    std::thread           worker_;
    std::thread           initThread_;
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
