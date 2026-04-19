//
// uf8_color_test — CLI tool that opens the UF8 via libusb and sends one
// color-set command with all 8 strips to the same palette index. Use this
// to confirm the vendor-specific USB path accepts color commands with
// SSL 360° not running (only CSI / MCU MIDI active).
//
// Usage:
//   uf8_color_test <palette_index>        e.g. 0x02 for red
//   uf8_color_test                        cycles red -> green -> blue
//
// Build:
//   c++ -std=c++20 -I../src $(pkg-config --cflags libusb-1.0) \
//       ../src/Protocol.cpp uf8_color_test.cpp \
//       $(pkg-config --libs libusb-1.0) -o uf8_color_test
//

#include <libusb.h>

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

#include "Protocol.h"

namespace {
constexpr uint16_t kVid       = 0x31E9;
constexpr uint16_t kPid       = 0x0021;
constexpr uint8_t  kInterface = 0;
constexpr uint8_t  kEpOut     = 0x02;
}

static int sendAll(libusb_device_handle* h, uint8_t index)
{
    std::array<uint8_t, 8> strips{};
    strips.fill(index);
    auto frame = uf8::buildColorCommand(strips);

    int transferred = 0;
    int rc = libusb_bulk_transfer(h, kEpOut,
                                  frame.data(),
                                  static_cast<int>(frame.size()),
                                  &transferred, 500);
    std::printf("send idx=0x%02x  rc=%d  transferred=%d/%zu\n",
                index, rc, transferred, frame.size());
    return rc;
}

int main(int argc, char** argv)
{
    libusb_context* ctx = nullptr;
    if (libusb_init(&ctx) < 0) {
        std::fprintf(stderr, "libusb_init failed\n");
        return 1;
    }

    libusb_device_handle* h = libusb_open_device_with_vid_pid(ctx, kVid, kPid);
    if (!h) {
        std::fprintf(stderr, "UF8 not found OR claimed by another process.\n"
                             "Quit SSL 360° first and try again.\n");
        libusb_exit(ctx);
        return 2;
    }

    if (libusb_kernel_driver_active(h, kInterface) == 1) {
        int rc = libusb_detach_kernel_driver(h, kInterface);
        std::printf("detach kernel driver rc=%d\n", rc);
    }

    int rc = libusb_claim_interface(h, kInterface);
    if (rc < 0) {
        std::fprintf(stderr, "claim_interface: %s\n", libusb_error_name(rc));
        libusb_close(h);
        libusb_exit(ctx);
        return 3;
    }

    if (argc >= 2) {
        uint8_t idx = static_cast<uint8_t>(std::strtol(argv[1], nullptr, 0));
        sendAll(h, idx);
    } else {
        std::printf("cycling red -> green -> blue (1.5 s each)\n");
        sendAll(h, 0x02);
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        sendAll(h, 0x03);
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        sendAll(h, 0x04);
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    }

    libusb_release_interface(h, kInterface);
    libusb_close(h);
    libusb_exit(ctx);
    return 0;
}
