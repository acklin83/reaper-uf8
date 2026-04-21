//
// uf8_palette_probe — iterate 0x00..0x0F, show each for 3 s on all 8
// strips. User reads off the color per index → drops into Palette.cpp.
//

#include <libusb.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

#include "Protocol.h"
#include "init_sequence.inc"
#include "layer_plugin_mixer.inc"

namespace {
constexpr uint16_t kVid       = 0x31E9;
constexpr uint16_t kPid       = 0x0021;
constexpr uint8_t  kInterface = 0;
constexpr uint8_t  kEpOut     = 0x02;
constexpr uint8_t  kEpIn      = 0x81;

std::atomic<bool> g_stop{false};

void inReader(libusb_device_handle* h)
{
    uint8_t buf[64];
    while (!g_stop) {
        int transferred = 0;
        libusb_bulk_transfer(h, kEpIn, buf, sizeof(buf), &transferred, 50);
    }
}

void heartbeatSender(libusb_device_handle* h)
{
    std::array<uint8_t, 64> hb1{0xff, 0x66, 0x21, 0x09};
    std::array<uint8_t, 64> hb2{0xff, 0x66, 0x21, 0x0a};
    hb1[63] = 0x90;
    hb2[63] = 0x91;
    uint8_t hb3[13] = {0xff, 0x66, 0x09, 0x15, 0, 0, 0, 0, 0, 0, 0, 0, 0x84};
    uint8_t hb4[13] = {0xff, 0x66, 0x09, 0x16, 0, 0, 0, 0, 0, 0, 0, 0, 0x85};
    while (!g_stop) {
        int t = 0;
        libusb_bulk_transfer(h, kEpOut, hb1.data(), hb1.size(), &t, 100);
        libusb_bulk_transfer(h, kEpOut, hb2.data(), hb2.size(), &t, 100);
        libusb_bulk_transfer(h, kEpOut, hb3, sizeof(hb3), &t, 100);
        libusb_bulk_transfer(h, kEpOut, hb4, sizeof(hb4), &t, 100);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

void sendFrame(libusb_device_handle* h, const uint8_t* data, size_t size)
{
    int transferred = 0;
    libusb_bulk_transfer(h, kEpOut, const_cast<uint8_t*>(data),
                         static_cast<int>(size), &transferred, 500);
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
}

void sendVec(libusb_device_handle* h, const std::vector<uint8_t>& v)
{
    sendFrame(h, v.data(), v.size());
}

} // anonymous

int main()
{
    libusb_context* ctx = nullptr;
    if (libusb_init(&ctx) < 0) return 1;

    libusb_device_handle* h = libusb_open_device_with_vid_pid(ctx, kVid, kPid);
    if (!h) {
        std::fprintf(stderr, "UF8 not found. Quit REAPER/SSL360 first.\n");
        libusb_exit(ctx);
        return 2;
    }
    if (libusb_kernel_driver_active(h, kInterface) == 1) {
        libusb_detach_kernel_driver(h, kInterface);
    }
    if (libusb_claim_interface(h, kInterface) < 0) {
        std::fprintf(stderr, "claim_interface failed\n");
        libusb_close(h);
        libusb_exit(ctx);
        return 3;
    }
    libusb_clear_halt(h, kEpOut);
    libusb_clear_halt(h, kEpIn);

    std::thread inThread(inReader, h);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Init + layer replay + slot activation — mirror what UF8Device::open does.
    for (const auto& f : uf8::kInitSequence) sendFrame(h, f.bytes, f.size);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    for (const auto& f : uf8::kLayerPluginMixerSequence) sendFrame(h, f.bytes, f.size);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    sendVec(h, uf8::buildPluginSlotActive());
    for (uint8_t s = 0; s < 8; ++s) {
        char name[8];
        std::snprintf(name, sizeof(name), "IDX %X", s);
        sendVec(h, uf8::buildPluginSlotName(s, name));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    std::thread hbThread(heartbeatSender, h);

    std::printf("\n=== Palette Probe ===\n");
    std::printf("Each index shown for 3s on ALL 8 strips.\n");
    std::printf("Note the color on the UF8 for each index.\n\n");

    for (uint8_t idx = 0x00; idx < 0x10; ++idx) {
        std::array<uint8_t, 8> strips{};
        strips.fill(idx);
        auto colorFrame = uf8::buildColorCommand(strips);

        // Push CS-Type label so user sees which index is active.
        for (uint8_t s = 0; s < 8; ++s) {
            char txt[5];
            std::snprintf(txt, sizeof(txt), "0x%02X", idx);
            sendVec(h, uf8::buildChannelStripType(s, txt));
        }
        sendVec(h, colorFrame);

        std::printf(">>> idx=0x%02X — observe color on UF8 (5 s)\n", idx);
        std::fflush(stdout);
        std::this_thread::sleep_for(std::chrono::seconds(5));
    }

    g_stop = true;
    hbThread.join();
    inThread.join();

    libusb_release_interface(h, kInterface);
    libusb_close(h);
    libusb_exit(ctx);
    return 0;
}
