//
// uf8_hid_probe — minimal standalone tool to verify we can open and read
// from the UF8 HID controller (VID 0x31E9 / PID 0x0022) via hidapi.
// Logs all HID reports for 15 seconds, then exits.
//

#include <hidapi.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

int main()
{
    if (hid_init() != 0) {
        std::fprintf(stderr, "hid_init failed\n");
        return 1;
    }

    std::printf("=== enumerating HID devices (VID 0x31E9) ===\n");
    hid_device_info* all = hid_enumerate(0x31E9, 0);
    for (auto* d = all; d; d = d->next) {
        std::printf("  path=%s  pid=0x%04x  usage_page=0x%04x  usage=0x%04x\n",
                    d->path ? d->path : "(null)",
                    d->product_id, d->usage_page, d->usage);
    }
    hid_free_enumeration(all);

    std::printf("\n=== opening VID 0x31E9 PID 0x0022 ===\n");
    hid_device* h = hid_open(0x31E9, 0x0022, nullptr);
    if (!h) {
        std::fprintf(stderr, "hid_open failed: %ls\n",
                     (wchar_t*)hid_error(nullptr));
        return 2;
    }
    std::printf("opened. reading for 15 s, move faders/v-pots/buttons now.\n");

    uint8_t buf[128];
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    int total = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        int n = hid_read_timeout(h, buf, sizeof(buf), 200);
        if (n > 0) {
            ++total;
            std::printf("[%3d] len=%d: ", total, n);
            for (int i = 0; i < n; ++i) std::printf("%02x ", buf[i]);
            std::printf("\n");
        } else if (n < 0) {
            std::fprintf(stderr, "hid_read_timeout error\n");
            break;
        }
    }

    std::printf("\n=== done, %d reports received ===\n", total);
    hid_close(h);
    hid_exit();
    return 0;
}
