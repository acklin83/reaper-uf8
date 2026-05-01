#include "UC1Protocol.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace uc1 {

uint8_t checksum(std::span<const uint8_t> payload)
{
    uint32_t sum = 0;
    for (uint8_t b : payload) sum += b;
    return static_cast<uint8_t>(sum & 0xFF);
}

bool verifyFrame(std::span<const uint8_t> frame)
{
    if (frame.size() < 3)          return false;
    if (frame[0] != kFrameMagic)   return false;
    const auto payload = frame.subspan(1, frame.size() - 2);
    return checksum(payload) == frame.back();
}

namespace {

// Build `FF <cmd> <len> <data...> <chk>` from a data buffer.
std::vector<uint8_t> buildFrame(uint8_t cmd, std::span<const uint8_t> data)
{
    std::vector<uint8_t> f;
    f.reserve(3 + data.size() + 1);
    f.push_back(kFrameMagic);
    f.push_back(cmd);
    f.push_back(static_cast<uint8_t>(data.size()));
    for (uint8_t b : data) f.push_back(b);

    // checksum covers cmd + len + data (everything between the leading FF
    // and the trailing checksum byte).
    f.push_back(checksum(std::span<const uint8_t>(f).subspan(1)));
    return f;
}

} // namespace

std::vector<uint8_t> buildKeepalive(uint8_t counter)
{
    const uint8_t data[1] = { static_cast<uint8_t>(counter & 0x03) };
    return buildFrame(0x1B, data);
}

std::vector<uint8_t> buildGrMeter(float dB)
{
    // Encode as 16-bit big-endian, units of 1/10 dB. Clamp to positive
    // GR values — negative "GR" is nonsensical on the wire.
    int32_t scaled = static_cast<int32_t>(std::lround(std::max(0.0f, dB) * 10.0f));
    if (scaled > 0xFFFF) scaled = 0xFFFF;
    const uint8_t data[2] = {
        static_cast<uint8_t>((scaled >> 8) & 0xFF),
        static_cast<uint8_t>(scaled & 0xFF),
    };
    return buildFrame(0x5B, data);
}

std::vector<uint8_t> buildBcBypassPose(bool entering)
{
    // cap45 (2026-04-28): SSL 360° fires exactly one FF 5C frame per BC
    // bypass-toggle press, with fixed positions independent of current GR.
    // Same frame shape + checksum formula as FF 5B; opcode 0x5C distinguishes
    // it as cosmetic-only.
    const uint8_t pos = entering ? 0x0A : 0x32;
    const uint8_t data[2] = { 0x00, pos };
    return buildFrame(0x5C, data);
}

std::vector<uint8_t> buildVuMeter(uint8_t meter, uint8_t level)
{
    // bank 0x01 is reserved for I/O VU strips; byte 4 picks input/output.
    const uint8_t data[4] = {
        0x01,                                     // bank
        level,                                    // meter level
        0x01,                                     // fixed role byte
        static_cast<uint8_t>(meter ? 0x01 : 0x00) // 0=in, 1=out
    };
    return buildFrame(0x13, data);
}

std::vector<uint8_t> buildLedWrite(uint8_t bank, uint8_t cell, uint8_t state)
{
    const uint8_t data[4] = { bank, cell, 0x01, state };
    return buildFrame(0x13, data);
}

std::vector<uint8_t> buildChannelStripContext(std::string_view name)
{
    // 36-byte content field (+ zone byte = 37 total data). Name at
    // position 12, up to ~12 chars (uc1_20 saw a 'b' marker at pos 24
    // when the slot held a placeholder — treat 12..23 as the usable
    // text range).
    constexpr size_t kFieldWidth = 36;
    constexpr size_t kNamePos    = 12;
    constexpr size_t kNameLen    = 12;

    std::vector<uint8_t> data(1 + kFieldWidth, 0x00);
    data[0] = 0x02;

    const size_t n = std::min(name.size(), kNameLen);
    for (size_t i = 0; i < n; ++i) data[1 + kNamePos + i] = static_cast<uint8_t>(name[i]);

    return buildFrame(0x66, data);
}

std::vector<uint8_t> buildBusCompContext(std::string_view name)
{
    // 42-byte content field (+ zone byte = 43 total). Name at position
    // 14, up to 14 chars (uc1_24b: "TESTBUS" landed at 14..20).
    constexpr size_t kFieldWidth = 42;
    constexpr size_t kNamePos    = 14;
    constexpr size_t kNameLen    = 14;

    std::vector<uint8_t> data(1 + kFieldWidth, 0x00);
    data[0] = 0x04;

    const size_t n = std::min(name.size(), kNameLen);
    for (size_t i = 0; i < n; ++i) data[1 + kNamePos + i] = static_cast<uint8_t>(name[i]);

    return buildFrame(0x66, data);
}

namespace {

// Segments lit for each decimal digit (standard 7-segment, a..g).
// Each bit represents one segment; segments a,b,c,d,e,f,g indexed 0..6.
//   bit 0 = a (top horizontal)
//   bit 1 = b (top-right)
//   bit 2 = c (bottom-right)
//   bit 3 = d (bottom horizontal)
//   bit 4 = e (bottom-left)
//   bit 5 = f (top-left)
//   bit 6 = g (middle)
constexpr uint8_t kDigitSegments[10] = {
    0b0111111,  // 0: a,b,c,d,e,f
    0b0000110,  // 1: b,c
    0b1011011,  // 2: a,b,d,e,g
    0b1001111,  // 3: a,b,c,d,g
    0b1100110,  // 4: b,c,f,g
    0b1101101,  // 5: a,c,d,f,g
    0b1111101,  // 6: a,c,d,e,f,g
    0b0000111,  // 7: a,b,c
    0b1111111,  // 8: all
    0b1101111,  // 9: a,b,c,d,f,g
};

// Push 7 FF 13 04 01 <cell> 00 <state> frames for one digit position.
void appendDigit(std::vector<std::vector<uint8_t>>& out,
                 uint8_t baseCell,
                 bool blank,
                 uint8_t digit)
{
    const uint8_t segs = blank ? 0 : kDigitSegments[digit];
    for (int s = 0; s < 7; ++s) {
        // 7-seg segments use state=0x01 for on, 0x00 for off (unlike
        // button LEDs which use 0xFF / 0x00). The "mode" byte (byte3)
        // is 0x00 for 7-seg vs 0x01 for buttons.
        const uint8_t state = (segs & (1 << s)) ? 0x01 : 0x00;
        out.push_back(buildLedWrite(0x01, baseCell + s, state));
        // buildLedWrite uses byte3=0x01 (VU flag) — but 7-seg needs
        // byte3=0x00. Rewrite that byte.
        auto& f = out.back();
        // Frame layout: FF 13 04 01 <cell> 01 <state> <chk>
        //                        idx: 3   4    5    6     7
        f[5] = 0x00;  // switch from VU-mode flag to 7-seg-mode flag
        // Recompute checksum: sum(cmd + len + data) mod 256.
        uint32_t sum = 0;
        for (size_t i = 1; i < f.size() - 1; ++i) sum += f[i];
        f.back() = static_cast<uint8_t>(sum & 0xFF);
    }
}

} // namespace

std::vector<std::vector<uint8_t>> buildSevenSeg(unsigned int value)
{
    if (value > 999) value = 999;
    std::vector<std::vector<uint8_t>> frames;
    frames.reserve(21);

    const uint8_t ones    = static_cast<uint8_t>(value % 10);
    const uint8_t tens    = static_cast<uint8_t>((value / 10) % 10);
    const uint8_t hundreds= static_cast<uint8_t>((value / 100) % 10);

    // Always render all 3 digits with leading zeros — the user wants
    // "001", "002" etc. rather than SSL's blank-leading convention.
    appendDigit(frames, 0x10, false, ones);
    appendDigit(frames, 0x08, false, tens);

    // Hundreds: alphabetical a..g at baseCell 0x00, same layout as
    // ones/tens. Decoded fully via uc1_33b (CHANNEL-encoder scroll
    // through the 0..900 range): "0"={0x00..0x05}, "1"={0x01,0x02},
    // "2"={0x00,0x01,0x03,0x04,0x06}, "3"={0x00..0x03,0x06},
    // "4"={0x01,0x02,0x05,0x06}, "5"={0x00,0x02,0x03,0x05,0x06},
    // "8"={0x00..0x06}. Render via the same appendDigit helper as the
    // other two digits.
    appendDigit(frames, 0x00, false, hundreds);

    return frames;
}

std::vector<uint8_t> buildDisplayText(uint8_t zone, std::string_view text, size_t width)
{
    std::vector<uint8_t> data;
    data.reserve(1 + width);
    data.push_back(zone);

    // Truncate or space-pad to the zone width. Null bytes outside the
    // printable text are replaced with spaces so the display shows a
    // clean field (SSL 360° captures use 0x20 padding, not nulls).
    const size_t take = std::min<size_t>(text.size(), width);
    for (size_t i = 0; i < take; ++i) data.push_back(static_cast<uint8_t>(text[i]));
    for (size_t i = take; i < width; ++i) data.push_back(0x20);

    return buildFrame(0x66, data);
}

// ---- parsers ----

namespace {

// Strip the optional "31 60" USB poll wrapper so parsers see a raw FF-frame.
std::span<const uint8_t> stripPollWrap(std::span<const uint8_t> bytes)
{
    if (bytes.size() >= 3 && bytes[0] == 0x31 && bytes[1] == 0x60 && bytes[2] == kFrameMagic) {
        return bytes.subspan(2);
    }
    return bytes;
}

} // namespace

std::optional<ButtonEvent> parseButtonEvent(std::span<const uint8_t> bytes)
{
    const auto f = stripPollWrap(bytes);
    // Expected form: FF 22 03 <id> 00 <state> <chk>  (7 bytes)
    if (f.size() < 7)          return std::nullopt;
    if (f[0] != kFrameMagic)   return std::nullopt;
    if (f[1] != 0x22)          return std::nullopt;
    if (f[2] != 0x03)          return std::nullopt;
    if (f[4] != 0x00)          return std::nullopt;

    if (!verifyFrame(f.first(7))) return std::nullopt;

    ButtonEvent ev;
    ev.id      = f[3];
    ev.pressed = (f[5] == 0x01);
    return ev;
}

std::optional<KnobEvent> parseKnobEvent(std::span<const uint8_t> bytes)
{
    const auto f = stripPollWrap(bytes);
    // Expected form: FF 24 02 <id> <delta> <chk>  (6 bytes)
    if (f.size() < 6)          return std::nullopt;
    if (f[0] != kFrameMagic)   return std::nullopt;
    if (f[1] != 0x24)          return std::nullopt;
    if (f[2] != 0x02)          return std::nullopt;

    if (!verifyFrame(f.first(6))) return std::nullopt;

    // Delta is a 6-bit signed value packed in the low 6 bits of the byte.
    //   0x01 = +1, 0x02 = +2, … 0x1F = +31
    //   0x3F = -1, 0x3E = -2, … 0x20 = -32
    const uint8_t raw = f[4] & 0x3F;
    const int8_t delta = (raw & 0x20) ? static_cast<int8_t>(raw | 0xC0)  // sign-extend
                                      : static_cast<int8_t>(raw);

    KnobEvent ev;
    ev.id    = f[3];
    ev.delta = delta;
    return ev;
}

// --- Decoded 2026-04-24 ---

std::vector<uint8_t> buildLedBrightness(uint8_t level)
{
    // FF 14 02 <b> 00 CKSUM
    const uint8_t data[2] = {level, 0x00};
    return buildFrame(0x14, data);
}

std::vector<uint8_t> buildLcdBrightness(uint8_t level)
{
    // FF 4F 02 <b> 00 CKSUM
    const uint8_t data[2] = {level, 0x00};
    return buildFrame(0x4F, data);
}

std::vector<uint8_t> buildStatusBrightness(uint8_t level)
{
    // FF 5C 02 00 <b> CKSUM — byte order inverted vs the other two
    const uint8_t data[2] = {0x00, level};
    return buildFrame(0x5C, data);
}

std::vector<uint8_t> buildFocusedColour(uint8_t paletteIdx)
{
    // FF 66 02 11 <palette> CKSUM
    const uint8_t data[2] = {0x11, paletteIdx};
    return buildFrame(0x66, data);
}

std::vector<uint8_t> buildColourBarEnable(bool on)
{
    // FF 66 03 00 01 <flag> CKSUM
    const uint8_t data[3] = {0x00, 0x01, static_cast<uint8_t>(on ? 0x01 : 0x00)};
    return buildFrame(0x66, data);
}

std::vector<uint8_t> buildLcdHeader(std::string_view text)
{
    // FF 66 <len> 01 <text> CKSUM. Length byte = text.size() + 1 (the
    // 01 prefix consumes one payload byte).
    std::vector<uint8_t> data;
    data.reserve(1 + text.size());
    data.push_back(0x01);
    for (char c : text) data.push_back(static_cast<uint8_t>(c));
    return buildFrame(0x66, data);
}

std::vector<uint8_t> buildLcdSubHeader(std::string_view text)
{
    // FF 66 <len> 07 <text> CKSUM. Same as buildLcdHeader but with the
    // 0x07 prefix byte instead of 0x01.
    std::vector<uint8_t> data;
    data.reserve(1 + text.size());
    data.push_back(0x07);
    for (char c : text) data.push_back(static_cast<uint8_t>(c));
    return buildFrame(0x66, data);
}

std::vector<uint8_t> buildLcdValue(std::string_view text)
{
    // FF 66 <len> 0E <text> CKSUM. Used in EXT_FUNCS scroll to show
    // each item's current parameter value left of the name.
    std::vector<uint8_t> data;
    data.reserve(1 + text.size());
    data.push_back(0x0E);
    for (char c : text) data.push_back(static_cast<uint8_t>(c));
    return buildFrame(0x66, data);
}

std::vector<uint8_t> buildLcdUnit(std::string_view text)
{
    // FF 66 <len> 0F <text> CKSUM. Empty text → FF 66 01 0F (1-byte
    // payload = single 0x0F prefix, no chars). The unit frame must
    // be sent between the value and the round-indicator for the
    // firmware to paint the yellow arc.
    std::vector<uint8_t> data;
    data.reserve(1 + text.size());
    data.push_back(0x0F);
    for (char c : text) data.push_back(static_cast<uint8_t>(c));
    return buildFrame(0x66, data);
}

std::vector<uint8_t> buildLcdRoundIndicator(double norm)
{
    // Re-decoded uc1_40 (2026-05-01) — every distinct frame in the
    // sweep enumerated. Unipolar fill is 24 positions (n = 0..23):
    //   n  0..8  → b0 = (1<<n)-1,        b1 = 0,    b2 = 0xC0
    //   n  9..16 → b0 = 0xFF, b1 = (1<<(n-8))-1,    b2 = 0xC0
    //   n 17..20 → b0 = 0xFF, b1 = 0xFF,
    //              b2 = 0xC0 | ((1<<(n-16))-1)      i.e. C1 C3 C7 CF
    //   n 21..23 → b0 = 0xFF, b1 = 0xFF,
    //              b2 = 0xCF + (n-20)*0x10          i.e. DF EF FF
    // The upper nibble of b2 acts as a 4-bit counter starting at
    // 0xC; once the lower nibble fills (0xCF), each further step
    // adds 0x10 (DF, EF, FF). The 0xC0 colour-C marker is therefore
    // baked into the lowest counter value, not a separate OR mask.
    //
    // Bipolar params (Pan, Out Trim) fill from CENTRE outward —
    // separate code path documented but not yet wired (TODO).
    if (norm < 0.0) norm = 0.0;
    if (norm > 1.0) norm = 1.0;
    int n = static_cast<int>(norm * 23.0 + 0.5);
    if (n < 0)  n = 0;
    if (n > 23) n = 23;

    uint8_t b0, b1, b2;
    if (n <= 8) {
        b0 = static_cast<uint8_t>((1u << n) - 1u);   // 0 → 0x00
        b1 = 0x00;
        b2 = 0xC0;
    } else if (n <= 16) {
        b0 = 0xFF;
        b1 = static_cast<uint8_t>((1u << (n - 8)) - 1u);
        b2 = 0xC0;
    } else if (n <= 20) {
        b0 = 0xFF;
        b1 = 0xFF;
        b2 = static_cast<uint8_t>(0xC0 | ((1u << (n - 16)) - 1u));
    } else {
        b0 = 0xFF;
        b1 = 0xFF;
        b2 = static_cast<uint8_t>(0xCF + (n - 20) * 0x10);
    }
    const uint8_t data[4] = {0x0D, b0, b1, b2};
    return buildFrame(0x66, data);
}

std::vector<uint8_t> buildPresetListScroll(std::string_view prev2,
                                           std::string_view prev1,
                                           std::string_view curr,
                                           std::string_view next1,
                                           std::string_view next2)
{
    // FF 66 4C 06 + 5 × (14-char slot + \0) + CKSUM. Each slot is 15
    // bytes total (14 ASCII + 1 null terminator). String shorter than
    // 14 chars zero-pads to slot width; longer truncates.
    constexpr size_t kSlotChars = 14;
    constexpr size_t kSlotBytes = kSlotChars + 1;
    std::vector<uint8_t> data(1 + 5 * kSlotBytes, 0x00);
    data[0] = 0x06;
    auto writeSlot_ = [&](size_t idx, std::string_view s) {
        const size_t base = 1 + idx * kSlotBytes;
        const size_t n = std::min(s.size(), kSlotChars);
        for (size_t i = 0; i < n; ++i) {
            data[base + i] = static_cast<uint8_t>(s[i]);
        }
        // Slot is zero-initialised; the trailing kSlotChars-n bytes
        // plus the null terminator are already 0x00.
    };
    writeSlot_(0, prev2);
    writeSlot_(1, prev1);
    writeSlot_(2, curr);
    writeSlot_(3, next1);
    writeSlot_(4, next2);
    return buildFrame(0x66, data);
}

std::vector<uint8_t> buildMenuCommit(bool active)
{
    // FF 66 02 09 <flag> CKSUM. flag=0x01 lights the current item's
    // name green (EXT_FUNCS Adjust mode); flag=0x00 = normal.
    const uint8_t data[2] = {0x09, static_cast<uint8_t>(active ? 0x01 : 0x00)};
    return buildFrame(0x66, data);
}

std::vector<uint8_t> buildMenuIndicator08()
{
    // FF 66 02 08 00 CKSUM.
    const uint8_t data[2] = {0x08, 0x00};
    return buildFrame(0x66, data);
}

std::vector<uint8_t> buildCentralLabel(std::string_view fourChars)
{
    // FF 66 05 01 <4 ASCII> CKSUM
    uint8_t data[5];
    data[0] = 0x01;
    for (int i = 0; i < 4; ++i) {
        data[1 + i] = (i < static_cast<int>(fourChars.size()))
                          ? static_cast<uint8_t>(fourChars[i])
                          : 0x20;
    }
    return buildFrame(0x66, data);
}

std::vector<uint8_t> buildCentralMode(CentralMode m)
{
    // FF 66 03 00 <mode> 00 CKSUM
    const uint8_t data[3] = {0x00, static_cast<uint8_t>(m), 0x00};
    return buildFrame(0x66, data);
}

std::vector<uint8_t> buildRoutingOrderIndicator(uint8_t orderByte)
{
    // FF 66 02 0A <byte> CKSUM
    const uint8_t data[2] = {0x0A, orderByte};
    return buildFrame(0x66, data);
}

namespace {
std::vector<uint8_t> buildLedDotFrame(uint8_t bank, uint8_t cell,
                                      uint8_t state) {
    std::vector<uint8_t> f{0xFF, 0x13, 0x04, bank, cell, 0x01, state};
    uint32_t sum = 0;
    for (size_t i = 1; i < f.size(); ++i) sum += f[i];
    f.push_back(static_cast<uint8_t>(sum & 0xFF));
    return f;
}
} // namespace

std::vector<uint8_t> buildBcModeDot(bool on)
{
    // FF 13 04 02 9E 01 <0xFF/0x00> CKSUM. uc1_37 confirmed SSL360
    // only writes bank=0x02 for the BC dot — no selection-bit pair.
    return buildLedDotFrame(0x02, kCellBcModeDot, on ? 0xFF : 0x00);
}

std::array<std::vector<uint8_t>, 2> buildMenuDot(uint8_t cell, bool on)
{
    // Dual-bank pair (uc1_38 entry/exit sequence). Order:
    //   1. bank=0x02 brightness (0xFF on / 0x00 off)
    //   2. bank=0x01 selection  (0x04 on / 0x01 off)
    return {
        buildLedDotFrame(0x02, cell, on ? 0xFF : 0x00),
        buildLedDotFrame(0x01, cell, on ? 0x04 : 0x01),
    };
}

namespace {
void writeSlot(uint8_t* dst, size_t slotWidth, std::string_view text)
{
    const size_t n = std::min(text.size(), slotWidth);
    for (size_t i = 0; i < n; ++i) dst[i] = static_cast<uint8_t>(text[i]);
    for (size_t i = n; i < slotWidth; ++i) dst[i] = 0x00;
}
}

std::vector<uint8_t> buildTrackNameTripleSmall(std::string_view prev,
                                               std::string_view curr,
                                               std::string_view next)
{
    // FF 66 25 02 + 3 × 12-byte slots + CKSUM
    constexpr size_t kSlot = 12;
    std::vector<uint8_t> data(1 + 3 * kSlot, 0x00);
    data[0] = 0x02;
    writeSlot(&data[1 + 0 * kSlot], kSlot, prev);
    writeSlot(&data[1 + 1 * kSlot], kSlot, curr);
    writeSlot(&data[1 + 2 * kSlot], kSlot, next);
    return buildFrame(0x66, data);
}

std::vector<uint8_t> buildTrackNameTripleLarge(std::string_view prev,
                                               std::string_view curr,
                                               std::string_view next)
{
    // FF 66 2B 04 + 3 × 14-byte slots + CKSUM
    constexpr size_t kSlot = 14;
    std::vector<uint8_t> data(1 + 3 * kSlot, 0x00);
    data[0] = 0x04;
    writeSlot(&data[1 + 0 * kSlot], kSlot, prev);
    writeSlot(&data[1 + 1 * kSlot], kSlot, curr);
    writeSlot(&data[1 + 2 * kSlot], kSlot, next);
    return buildFrame(0x66, data);
}

} // namespace uc1
