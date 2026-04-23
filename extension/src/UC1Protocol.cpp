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

std::vector<uint8_t> buildTrackNameContext(std::string_view csName,
                                           std::string_view bcName)
{
    // 42-byte data field after the zone byte. Slot layout derived
    // empirically from uc1_24b.
    constexpr size_t kFieldWidth = 42;
    constexpr size_t kCsStart    = 0;
    constexpr size_t kCsLen      = 14;   // first slot: Channel Strip
    constexpr size_t kBcStart    = 16;
    constexpr size_t kBcLen      = 14;   // second slot: Bus Compressor

    std::vector<uint8_t> data(1 + kFieldWidth, 0x00);
    data[0] = 0x04;  // zone

    const size_t nCs = std::min(csName.size(), kCsLen);
    for (size_t i = 0; i < nCs; ++i) data[1 + kCsStart + i] = static_cast<uint8_t>(csName[i]);

    const size_t nBc = std::min(bcName.size(), kBcLen);
    for (size_t i = 0; i < nBc; ++i) data[1 + kBcStart + i] = static_cast<uint8_t>(bcName[i]);

    return buildFrame(0x66, data);
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

} // namespace uc1
