#include "HidDevice.h"

#include <hidapi.h>

#include <array>
#include <chrono>
#include <thread>

namespace uf8 {

bool HidDevice::open(uint16_t vid, uint16_t pid)
{
    if (handle_) return true;
    if (hid_init() != 0) { lastError_ = "hid_init failed"; return false; }

    handle_ = hid_open(vid, pid, nullptr);
    if (!handle_) {
        lastError_ = "hid_open failed (device not found or no permission)";
        return false;
    }
    hid_set_nonblocking(handle_, 0);  // blocking reads with timeout in loop

    stop_ = false;
    thread_ = std::thread([this]{ readerLoop_(); });
    return true;
}

void HidDevice::close()
{
    stop_ = true;
    if (thread_.joinable()) thread_.join();
    if (handle_) { hid_close(handle_); handle_ = nullptr; }
}

HidDevice::~HidDevice() { close(); }

void HidDevice::readerLoop_()
{
    // UF8's HID reports appear to be relatively small; 64 bytes is a safe
    // upper bound. If longer packets appear in the log we'll grow.
    std::array<uint8_t, 128> buf{};
    while (!stop_) {
        int n = hid_read_timeout(handle_, buf.data(), buf.size(), 50);
        if (n > 0 && handler_) {
            handler_(buf.data(), static_cast<size_t>(n));
        }
        if (n < 0) {
            // Device vanished — bail out so the extension can report.
            lastError_ = "hid_read_timeout returned error";
            break;
        }
    }
}

} // namespace uf8
