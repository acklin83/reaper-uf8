//
// uf8_color_test — drive UF8 without SSL 360°. Spawns an async IN reader
// so flow-control doesn't stall the OUT pipeline, sends the full captured
// init sequence, then a heartbeat loop alongside color cycles.
//

#include <libusb.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#include "Protocol.h"
#include "init_sequence.inc"

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
        // Intentionally discard — we only care that the endpoint keeps draining.
    }
}

void heartbeatSender(libusb_device_handle* h)
{
    // FF 66 21 09 00x59 90 and FF 66 21 0A 00x59 91 observed as the
    // continuous keepalive pair at ~20 ms cadence. Without these the
    // UF8 firmware declares "connection lost".
    std::array<uint8_t, 64> hb1{0xff, 0x66, 0x21, 0x09};
    std::array<uint8_t, 64> hb2{0xff, 0x66, 0x21, 0x0a};
    hb1[63] = 0x90;  // checksum = 0x66 + 0x21 + 0x09
    hb2[63] = 0x91;
    while (!g_stop) {
        int t = 0;
        libusb_bulk_transfer(h, kEpOut, hb1.data(), hb1.size(), &t, 100);
        libusb_bulk_transfer(h, kEpOut, hb2.data(), hb2.size(), &t, 100);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

int sendAll(libusb_device_handle* h, uint8_t index)
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
} // anonymous

int main(int argc, char** argv)
{
    libusb_context* ctx = nullptr;
    if (libusb_init(&ctx) < 0) return 1;

    libusb_device_handle* h = libusb_open_device_with_vid_pid(ctx, kVid, kPid);
    if (!h) {
        std::fprintf(stderr, "UF8 not found. Quit SSL 360° and retry.\n");
        libusb_exit(ctx);
        return 2;
    }
    if (libusb_kernel_driver_active(h, kInterface) == 1) {
        libusb_detach_kernel_driver(h, kInterface);
    }
    int rc = libusb_claim_interface(h, kInterface);
    if (rc < 0) {
        std::fprintf(stderr, "claim_interface: %s\n", libusb_error_name(rc));
        libusb_close(h);
        libusb_exit(ctx);
        return 3;
    }

    libusb_clear_halt(h, kEpOut);
    libusb_clear_halt(h, kEpIn);

    // Start IN drainer so OUT flow control doesn't stall.
    std::thread inThread(inReader, h);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    std::printf("replaying %zu init frames...\n", uf8::kInitSequence.size());
    size_t sent = 0;
    for (const auto& f : uf8::kInitSequence) {
        int transferred = 0;
        int ircode = libusb_bulk_transfer(
            h, kEpOut, const_cast<uint8_t*>(f.bytes),
            static_cast<int>(f.size), &transferred, 1000);
        if (ircode < 0) {
            std::fprintf(stderr, "frame %zu (size=%zu) failed: %s\n",
                         sent, f.size, libusb_error_name(ircode));
            goto cleanup;
        }
        ++sent;
        // 2 ms pacing — captures show SSL 360° ~1 ms, but we're single-threaded.
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    std::printf("init done (%zu frames). starting heartbeat + color cycle.\n", sent);

    {
        std::thread hbThread(heartbeatSender, h);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        for (int cycle = 0; cycle < 3; ++cycle) {
            sendAll(h, 0x02); std::this_thread::sleep_for(std::chrono::milliseconds(1500));
            sendAll(h, 0x03); std::this_thread::sleep_for(std::chrono::milliseconds(1500));
            sendAll(h, 0x04); std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        }

        g_stop = true;
        hbThread.join();
    }

cleanup:
    g_stop = true;
    inThread.join();
    libusb_release_interface(h, kInterface);
    libusb_close(h);
    libusb_exit(ctx);
    return 0;
}
