#pragma once
//
// HidDevice — reads the UF8 HID controller (VID 0x31E9 / PID 0x0022).
//
// The vendor-USB device (PID 0x0021) carries display/color output only.
// Physical input from faders, V-pots and buttons comes through a separate
// HID device on the same USB hub (PID 0x0022). We open it with hidapi,
// run a dedicated reader thread, and dispatch raw reports via a callback.
//
// Parsing to MCU happens in main.cpp once we've characterized the report
// format (test with logRawHid first, then map).
//

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>
#include <vector>

struct hid_device_;
typedef struct hid_device_ hid_device;

namespace uf8 {

class HidDevice {
public:
    using ReportHandler = std::function<void(const uint8_t* data, size_t len)>;

    HidDevice() = default;
    ~HidDevice();
    HidDevice(const HidDevice&) = delete;
    HidDevice& operator=(const HidDevice&) = delete;

    bool open(uint16_t vid, uint16_t pid);
    void close();
    bool isOpen() const { return handle_ != nullptr; }

    void setHandler(ReportHandler h) { handler_ = std::move(h); }

    const std::string& lastError() const { return lastError_; }

private:
    void readerLoop_();

    hid_device*       handle_ = nullptr;
    std::thread       thread_;
    std::atomic<bool> stop_{false};
    ReportHandler     handler_;
    std::string       lastError_;
};

} // namespace uf8
