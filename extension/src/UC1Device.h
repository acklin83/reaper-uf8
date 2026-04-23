#pragma once
//
// UC1Device — libusb wrapper for the SSL UC1 (VID 0x31E9 / PID 0x0023).
// Parallel to UF8Device, but much simpler: UC1 needs no custom init
// sequence and only one keepalive command (FF 1B 01 <counter 0-3>).
//
// Threading: libusb calls happen on a dedicated worker. send() is
// producer-side (thread-safe, buffered). Event callbacks fire on the
// worker thread — hop to REAPER's main thread before touching REAPER
// state.
//

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include "UC1Protocol.h"

struct libusb_context;
struct libusb_device_handle;
struct libusb_transfer;

namespace uc1 {

// Diagnostic hooks — useful when the UC1 falls back to "Attempting to
// reconnect to SSL 360°". Counters are lifetime totals on the current
// process; poll them from the main thread and print deltas once per
// second to see what's actually on the wire.
uint64_t debugOutFrames();
uint64_t debugOutBytes();
uint64_t debugOutErrors();
uint64_t debugInCallbacks();
uint64_t debugInBytes();

class UC1Device {
public:
    using ButtonHandler   = std::function<void(const ButtonEvent&)>;
    using KnobHandler     = std::function<void(const KnobEvent&)>;
    using RawInputHandler = std::function<void(const uint8_t* data, size_t len)>;

    UC1Device();
    ~UC1Device();

    UC1Device(const UC1Device&) = delete;
    UC1Device& operator=(const UC1Device&) = delete;

    // Try to open the UC1. Returns true on success. Most common failure
    // mode is SSL360Core holding an exclusive claim — we report that
    // specifically in lastError().
    bool open();
    void close();
    bool isOpen() const { return handle_ != nullptr; }

    // Fire-and-forget send. Thread-safe; the worker drains the queue
    // over EP 0x02 OUT.
    void send(std::vector<uint8_t> frame);

    // Set the current GR meter value (positive dB, 0 = no reduction).
    // The worker thread streams this at ~50 Hz continuously — UC1
    // firmware uses the FF 5B GR stream as its primary liveness
    // heartbeat and will disconnect if it stops. Thread-safe.
    void setGainReduction(float dB);

    // Event handlers fire on the worker thread.
    void setButtonHandler(ButtonHandler h)   { buttonHandler_   = std::move(h); }
    void setKnobHandler(KnobHandler h)       { knobHandler_     = std::move(h); }
    void setRawInputHandler(RawInputHandler h) { rawInputHandler_ = std::move(h); }

    const std::string& lastError() const { return lastError_; }

private:
    void workerLoop_();
    void startBulkRead_();
    static void readCallback_(libusb_transfer* xfer);

    libusb_context*       ctx_    = nullptr;
    libusb_device_handle* handle_ = nullptr;
    std::atomic<bool>     shuttingDown_{false};
    std::thread           worker_;
    std::string           lastError_;
    ButtonHandler         buttonHandler_;
    KnobHandler           knobHandler_;
    RawInputHandler       rawInputHandler_;

    struct PendingSend;
    std::unique_ptr<PendingSend> pending_;

    std::atomic<float> grDb_{0.0f};
};

} // namespace uc1
