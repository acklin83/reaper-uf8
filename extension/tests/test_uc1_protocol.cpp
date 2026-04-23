//
// Unit tests for UC1 frame construction + parsing. No libusb, no REAPER —
// just byte math. Test vectors come from real captures (uc1_02 idle,
// uc1_04 Threshold, uc1_05 Ratio, uc1_11 GR, uc1_13 VU, uc1_19 buttons,
// uc1_21 LEDs).
//

#include "UC1Protocol.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#define EXPECT(cond) do {                                              \
    if (!(cond)) {                                                     \
        std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__,   \
                     #cond);                                           \
        std::exit(1);                                                  \
    }                                                                  \
} while(0)

static std::string hex(const std::vector<uint8_t>& v) {
    std::string s; s.reserve(v.size() * 2);
    static const char* d = "0123456789abcdef";
    for (auto b : v) { s.push_back(d[b >> 4]); s.push_back(d[b & 0xF]); }
    return s;
}

int main()
{
    using namespace uc1;

    // --- Keepalive. From uc1_02 idle baseline: counter cycles 0..3.
    EXPECT(hex(buildKeepalive(0)) == "ff1b01001c");
    EXPECT(hex(buildKeepalive(1)) == "ff1b01011d");
    EXPECT(hex(buildKeepalive(2)) == "ff1b01021e");
    EXPECT(hex(buildKeepalive(3)) == "ff1b01031f");

    // Counter wraps on the low 2 bits
    EXPECT(hex(buildKeepalive(4)) == hex(buildKeepalive(0)));
    EXPECT(hex(buildKeepalive(7)) == hex(buildKeepalive(3)));

    // --- GR meter (FF 5B 02 <BE-16 in 1/10 dB>).
    // uc1_02 idle:            ff 5b 02 00 00 5d   (0 dB)
    // uc1_11 static ~12 dB:   ff 5b 02 00 79 d6
    // uc1_12 dynamic peak:    ff 5b 02 00 98 f5   (15.2 dB)
    EXPECT(hex(buildGrMeter(0.0f))  == "ff5b0200005d");
    EXPECT(hex(buildGrMeter(12.1f)) == "ff5b020079d6");
    EXPECT(hex(buildGrMeter(15.2f)) == "ff5b020098f5");
    EXPECT(hex(buildGrMeter(-3.0f)) == "ff5b0200005d");   // negative clamps to 0
    EXPECT(hex(buildZeroGr())       == "ff5b0200005d");

    // --- VU meter (FF 13 04 01 <level> 01 <in/out>). uc1_13 pair:
    //     ff 13 04 01 1a 01 00 33   (input, level 0x1a)
    //     ff 13 04 01 1a 01 01 34   (output, level 0x1a)
    EXPECT(hex(buildVuMeter(0, 0x1A)) == "ff1304011a010033");
    EXPECT(hex(buildVuMeter(1, 0x1A)) == "ff1304011a010134");

    // --- Per-button LED writes (FF 13 04 <bank> <cell> 01 <state>).
    // uc1_21 captured these exact payloads:
    //   Polarity  press  → ff 13 04 02 98 01 ff b1
    //   HF Bell   press  → ff 13 04 02 89 01 ff a2
    //   Bus Comp  active → ff 13 04 02 01 01 ff 1a
    //   EQ In     off    → ff 13 04 02 50 01 00 6a
    EXPECT(hex(buildLedWrite(led::kPolarity.bank, led::kPolarity.cell, led::kStateOn))
           == "ff1304029801ffb1");
    EXPECT(hex(buildLedWrite(led::kHfBell.bank, led::kHfBell.cell, led::kStateOn))
           == "ff1304028901ffa2");
    EXPECT(hex(buildLedWrite(led::kBusCompIn.bank, led::kBusCompIn.cell, led::kStateOn))
           == "ff1304020101ff1a");
    EXPECT(hex(buildLedWrite(led::kEqIn.bank, led::kEqIn.cell, led::kStateOff))
           == "ff1304025001006a");

    // Dim state — plugin-bypass brightness (0x33).
    // Fast Att (Comp) dim: 0x13+0x04+0x02+0x38+0x01+0x33 = 0x85
    EXPECT(hex(buildLedWrite(led::kFastAttComp.bank,
                             led::kFastAttComp.cell,
                             led::kStateDim))
           == "ff13040238013385");

    // --- Display text write (FF 66 <len> <zone> <ASCII> <chk>).
    // uc1_04 Threshold:  FF 66 17 05 "Threshold       12.1dB" <chk>
    {
        auto f = buildDisplayText(zone::kBusCompReadout, "Threshold       12.1dB");
        EXPECT(f.size() == 5 + 22);    // FF 66 len zone ascii×22 chk
        EXPECT(f[0] == 0xFF);
        EXPECT(f[1] == 0x66);
        EXPECT(f[2] == 0x17);          // len = 1 (zone) + 22 (ASCII) = 23
        EXPECT(f[3] == 0x05);
        const char* expected = "Threshold       12.1dB";
        for (size_t i = 0; i < 22; ++i) EXPECT(f[4 + i] == static_cast<uint8_t>(expected[i]));
        EXPECT(verifyFrame(f));
    }

    // Short string is space-padded.
    {
        auto f = buildDisplayText(zone::kChannelStripReadout, "Solo");
        EXPECT(f.size() == 5 + 22);
        EXPECT(f[3] == 0x03);
        EXPECT(f[4] == 'S' && f[5] == 'o' && f[6] == 'l' && f[7] == 'o');
        for (size_t i = 8; i < f.size() - 1; ++i) EXPECT(f[i] == 0x20);
        EXPECT(verifyFrame(f));
    }

    // Over-length truncates.
    {
        auto f = buildDisplayText(zone::kBusCompReadout,
                                  "This string is far longer than twenty-two chars");
        EXPECT(f.size() == 5 + 22);
        EXPECT(verifyFrame(f));
    }

    // --- verifyFrame spot checks
    {
        const std::vector<uint8_t> good = {0xff, 0x5b, 0x02, 0x00, 0x79, 0xd6};
        const std::vector<uint8_t> bad  = {0xff, 0x5b, 0x02, 0x00, 0x79, 0xd7};
        const std::vector<uint8_t> noMagic = {0xfe, 0x5b, 0x02, 0x00, 0x79, 0xd6};
        EXPECT(verifyFrame(good));
        EXPECT(!verifyFrame(bad));
        EXPECT(!verifyFrame(noMagic));
    }

    // --- Button event parser. From uc1_19:
    //   31 60 ff 22 03 08 00 01 2e   (HF Bell press with poll wrap)
    //   ff 22 03 08 00 00 2d         (HF Bell release bare)
    {
        const std::vector<uint8_t> wrapped = {0x31, 0x60, 0xff, 0x22, 0x03, 0x08, 0x00, 0x01, 0x2e};
        auto ev = parseButtonEvent(wrapped);
        EXPECT(ev.has_value());
        EXPECT(ev->id == button::kHfBell);
        EXPECT(ev->pressed);
    }
    {
        const std::vector<uint8_t> bare = {0xff, 0x22, 0x03, 0x08, 0x00, 0x00, 0x2d};
        auto ev = parseButtonEvent(bare);
        EXPECT(ev.has_value());
        EXPECT(ev->id == button::kHfBell);
        EXPECT(!ev->pressed);
    }
    {
        // Not a button event — should return nullopt
        const std::vector<uint8_t> gr = {0xff, 0x5b, 0x02, 0x00, 0x79, 0xd6};
        EXPECT(!parseButtonEvent(gr).has_value());
    }
    {
        // Bad checksum — should return nullopt
        const std::vector<uint8_t> bad = {0xff, 0x22, 0x03, 0x08, 0x00, 0x01, 0x2f};
        EXPECT(!parseButtonEvent(bad).has_value());
    }

    // --- Knob event parser. From uc1_05 Ratio:
    //   ff 24 02 0e 01 35  (CW, +1 step)
    //   ff 24 02 0e 3f 73  (CCW, -1 step — 0x3f = 6-bit -1)
    {
        const std::vector<uint8_t> cw = {0xff, 0x24, 0x02, 0x0e, 0x01, 0x35};
        auto ev = parseKnobEvent(cw);
        EXPECT(ev.has_value());
        EXPECT(ev->id == knob::kBCRatio);
        EXPECT(ev->delta == +1);
    }
    {
        const std::vector<uint8_t> ccw = {0xff, 0x24, 0x02, 0x0e, 0x3f, 0x73};
        auto ev = parseKnobEvent(ccw);
        EXPECT(ev.has_value());
        EXPECT(ev->id == knob::kBCRatio);
        EXPECT(ev->delta == -1);
    }
    {
        // uc1_04 CCW-2 burst: ff 24 02 12 3e 76
        const std::vector<uint8_t> ccw2 = {0xff, 0x24, 0x02, 0x12, 0x3e, 0x76};
        auto ev = parseKnobEvent(ccw2);
        EXPECT(ev.has_value());
        EXPECT(ev->id == knob::kBCThreshold);
        EXPECT(ev->delta == -2);
    }
    {
        // With USB poll wrap
        const std::vector<uint8_t> wrapped = {0x31, 0x60, 0xff, 0x24, 0x02, 0x0e, 0x01, 0x35};
        auto ev = parseKnobEvent(wrapped);
        EXPECT(ev.has_value());
        EXPECT(ev->delta == +1);
    }

    std::printf("all UC1 protocol tests passed\n");
    return 0;
}
