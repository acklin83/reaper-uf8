#include "Protocol.h"

#include <cstring>
#include <numeric>

namespace uf8 {

uint8_t checksum(std::span<const uint8_t> payload)
{
    uint32_t sum = 0;
    for (auto b : payload) sum += b;
    return static_cast<uint8_t>(sum & 0xFFu);
}

std::vector<uint8_t> buildColorCommand(const std::array<uint8_t, kStripCount>& indices)
{
    // Frame: FF 66 09 18 <8 indices> CKSUM
    std::vector<uint8_t> frame;
    frame.reserve(14);
    frame.push_back(kFrameMagic);

    // Payload (bytes that go into the checksum)
    const std::array<uint8_t, 3> header{0x66, 0x09, 0x18};
    for (auto b : header) frame.push_back(b);
    for (auto b : indices) frame.push_back(b);

    // Checksum = sum of everything after FF, excluding the checksum byte itself
    std::span<const uint8_t> payload{frame.data() + 1, frame.size() - 1};
    frame.push_back(checksum(payload));
    return frame;
}

std::vector<uint8_t> buildStripTextUpper(uint8_t strip, std::string_view text)
{
    // Frame: FF 66 <len> 0B <strip> <N chars> CKSUM
    // where len = N + 2 (strip byte + N text bytes... wait: len = count of bytes between
    // len and cksum, which is 0B + strip + N chars = 2 + N).
    const size_t N = std::min(text.size(), size_t{7});
    const uint8_t len = static_cast<uint8_t>(N + 2);

    std::vector<uint8_t> frame;
    frame.reserve(5 + N);
    frame.push_back(kFrameMagic);
    frame.push_back(0x66);
    frame.push_back(len);
    frame.push_back(0x0B);
    frame.push_back(strip);
    for (size_t i = 0; i < N; ++i) frame.push_back(static_cast<uint8_t>(text[i]));

    std::span<const uint8_t> payload{frame.data() + 1, frame.size() - 1};
    frame.push_back(checksum(payload));
    return frame;
}

std::vector<uint8_t> buildFaderPosition(uint8_t strip, uint8_t lsb, uint8_t msb)
{
    std::vector<uint8_t> frame;
    frame.reserve(7);
    frame.push_back(kFrameMagic);
    frame.push_back(0x1E);
    frame.push_back(0x03);
    frame.push_back(strip);
    frame.push_back(lsb);
    frame.push_back(msb);
    std::span<const uint8_t> payload{frame.data() + 1, frame.size() - 1};
    frame.push_back(checksum(payload));
    return frame;
}

std::vector<uint8_t> buildMotorEnable(uint8_t strip, bool enable)
{
    std::vector<uint8_t> frame;
    frame.reserve(6);
    frame.push_back(kFrameMagic);
    frame.push_back(0x1D);
    frame.push_back(0x02);
    frame.push_back(strip);
    frame.push_back(enable ? 0x01 : 0x00);
    std::span<const uint8_t> payload{frame.data() + 1, frame.size() - 1};
    frame.push_back(checksum(payload));
    return frame;
}

std::vector<uint8_t> buildMeter(uint8_t strip, uint8_t level)
{
    std::vector<uint8_t> frame;
    frame.reserve(8);
    frame.push_back(kFrameMagic);
    frame.push_back(0x38);
    frame.push_back(0x04);
    frame.push_back(static_cast<uint8_t>(strip * 3));
    frame.push_back(0x00);
    frame.push_back(0x00);
    frame.push_back(static_cast<uint8_t>(0xF0 | (level & 0x0F)));
    std::span<const uint8_t> payload{frame.data() + 1, frame.size() - 1};
    frame.push_back(checksum(payload));
    return frame;
}

std::vector<uint8_t> buildLayerPluginMixer()
{
    // Captured byte-for-byte: FF 66 11 0F 10 00 40 00 <20 00 × 6> 96
    std::vector<uint8_t> frame{
        0xFF, 0x66, 0x11, 0x0F,
        0x10, 0x00, 0x40, 0x00,
        0x20, 0x00, 0x20, 0x00,
        0x20, 0x00, 0x20, 0x00,
        0x20, 0x00, 0x20, 0x00
    };
    std::span<const uint8_t> payload{frame.data() + 1, frame.size() - 1};
    frame.push_back(checksum(payload));
    return frame;
}

std::vector<uint8_t> buildLayerDaw()
{
    std::vector<uint8_t> frame{0xFF, 0x66, 0x11, 0x0F};
    for (int i = 0; i < 16; ++i) frame.push_back(0x00);
    std::span<const uint8_t> payload{frame.data() + 1, frame.size() - 1};
    frame.push_back(checksum(payload));
    return frame;
}

std::vector<uint8_t> buildPluginSlotActive()
{
    // FF 66 0A 00 03 00 00 00 00 00 00 00 00 CKSUM
    std::vector<uint8_t> frame{0xFF, 0x66, 0x0A, 0x00, 0x03};
    for (int i = 0; i < 8; ++i) frame.push_back(0x00);
    std::span<const uint8_t> payload{frame.data() + 1, frame.size() - 1};
    frame.push_back(checksum(payload));
    return frame;
}

std::vector<uint8_t> buildPluginSlotName(uint8_t strip, std::string_view text)
{
    // FF 66 <len> 04 <strip> <N text bytes> CKSUM   (len = N + 2)
    const size_t N = std::min(text.size(), size_t{12});
    const uint8_t len = static_cast<uint8_t>(N + 2);

    std::vector<uint8_t> frame;
    frame.reserve(5 + N);
    frame.push_back(kFrameMagic);
    frame.push_back(0x66);
    frame.push_back(len);
    frame.push_back(0x04);
    frame.push_back(strip);
    for (size_t i = 0; i < N; ++i) frame.push_back(static_cast<uint8_t>(text[i]));

    std::span<const uint8_t> payload{frame.data() + 1, frame.size() - 1};
    frame.push_back(checksum(payload));
    return frame;
}

std::vector<uint8_t> buildChannelStripType(uint8_t strip, std::string_view fourChars)
{
    // FF 66 06 17 <strip> <4 chars, space-padded> CKSUM
    std::vector<uint8_t> frame{0xFF, 0x66, 0x06, 0x17, strip};
    for (size_t i = 0; i < 4; ++i) {
        frame.push_back(i < fourChars.size() ? static_cast<uint8_t>(fourChars[i]) : 0x20);
    }
    std::span<const uint8_t> payload{frame.data() + 1, frame.size() - 1};
    frame.push_back(checksum(payload));
    return frame;
}

std::vector<uint8_t> buildValueLine(uint8_t strip, std::string_view text)
{
    // FF 66 15 0E <strip> <19 chars, space-padded> CKSUM    (24 bytes)
    std::vector<uint8_t> frame{0xFF, 0x66, 0x15, 0x0E, strip};
    for (size_t i = 0; i < 19; ++i) {
        frame.push_back(i < text.size() ? static_cast<uint8_t>(text[i]) : 0x20);
    }
    std::span<const uint8_t> payload{frame.data() + 1, frame.size() - 1};
    frame.push_back(checksum(payload));
    return frame;
}

std::vector<uint8_t> buildLedCommand(uint8_t ledId, bool on)
{
    // FF 3B 03 <id> 00 <state> CKSUM
    std::vector<uint8_t> frame{0xFF, 0x3B, 0x03, ledId, 0x00, uint8_t(on ? 0x01 : 0x00)};
    std::span<const uint8_t> payload{frame.data() + 1, frame.size() - 1};
    frame.push_back(checksum(payload));
    return frame;
}

std::vector<uint8_t> buildChannelNumber(uint8_t strip, std::string_view digits)
{
    // FF 66 <len> 14 <strip> <N ASCII> CKSUM  where len = N + 2
    const size_t N = digits.size();
    const uint8_t len = static_cast<uint8_t>(N + 2);
    std::vector<uint8_t> frame;
    frame.reserve(5 + N);
    frame.push_back(kFrameMagic);
    frame.push_back(0x66);
    frame.push_back(len);
    frame.push_back(0x14);
    frame.push_back(strip);
    for (char c : digits) frame.push_back(static_cast<uint8_t>(c));
    std::span<const uint8_t> payload{frame.data() + 1, frame.size() - 1};
    frame.push_back(checksum(payload));
    return frame;
}

std::vector<uint8_t> buildVPotReadoutBar(const std::array<uint16_t, kStripCount>& positions)
{
    // Frame: FF 66 11 0F <16 bytes: 8 × 2-byte LE per strip> CKSUM (20 bytes)
    std::vector<uint8_t> frame;
    frame.reserve(20);
    frame.push_back(kFrameMagic);
    const std::array<uint8_t, 3> head{0x66, 0x11, 0x0F};
    for (auto b : head) frame.push_back(b);
    for (auto p : positions) {
        frame.push_back(static_cast<uint8_t>(p & 0xFF));
        frame.push_back(static_cast<uint8_t>((p >> 8) & 0xFF));
    }
    std::span<const uint8_t> payload{frame.data() + 1, frame.size() - 1};
    frame.push_back(checksum(payload));
    return frame;
}

std::vector<uint8_t> buildFaderDbReadout(uint8_t strip, std::string_view fourChars)
{
    // FF 66 0A 0C <strip> <4 bytes, NUL-padded> 00 00 "dB" CKSUM    (14 bytes)
    // The 4-byte value slot is NUL-padded (not space-padded) to match SSL 360°.
    std::vector<uint8_t> frame{0xFF, 0x66, 0x0A, 0x0C, strip};
    for (size_t i = 0; i < 4; ++i) {
        frame.push_back(i < fourChars.size() ? static_cast<uint8_t>(fourChars[i]) : 0x00);
    }
    frame.push_back(0x00);
    frame.push_back(0x00);
    frame.push_back('d');
    frame.push_back('B');
    std::span<const uint8_t> payload{frame.data() + 1, frame.size() - 1};
    frame.push_back(checksum(payload));
    return frame;
}

std::vector<uint8_t> buildStripTextLower(uint8_t strip, std::string_view text)
{
    // Frame: FF 66 09 0E <strip> <7 chars, space-padded> CKSUM   (total 13 bytes)
    std::vector<uint8_t> frame;
    frame.reserve(13);
    frame.push_back(kFrameMagic);
    const std::array<uint8_t, 3> head{0x66, 0x09, 0x0E};
    for (auto b : head) frame.push_back(b);
    frame.push_back(strip);

    for (size_t i = 0; i < 7; ++i) {
        frame.push_back(i < text.size() ? static_cast<uint8_t>(text[i]) : 0x20);
    }
    std::span<const uint8_t> payload{frame.data() + 1, frame.size() - 1};
    frame.push_back(checksum(payload));
    return frame;
}

std::array<std::vector<uint8_t>, 2> buildPluginMixerHeartbeat()
{
    auto build = [](uint8_t counter) {
        std::vector<uint8_t> f;
        f.reserve(13);
        f.push_back(kFrameMagic);
        const std::array<uint8_t, 3> head{0x66, 0x09, counter};
        for (auto b : head) f.push_back(b);
        for (int i = 0; i < 8; ++i) f.push_back(0x00);
        std::span<const uint8_t> payload{f.data() + 1, f.size() - 1};
        f.push_back(checksum(payload));
        return f;
    };
    return {build(0x15), build(0x16)};
}

// --- Decoded 2026-04-24 ---

std::vector<uint8_t> buildLedBrightness(uint8_t level)
{
    // FF 2D 08 00 00 <b> 00 <b> 00 <b> 00 CKSUM
    std::vector<uint8_t> f;
    f.reserve(12);
    f.push_back(kFrameMagic);
    const std::array<uint8_t, 11> body{0x2D, 0x08, 0x00, 0x00,
                                        level, 0x00, level, 0x00,
                                        level, 0x00, 0x00};
    (void)body;
    // Actually the payload is 10 bytes after FF: 2D 08 00 00 <b> 00 <b> 00 <b> 00.
    // Let me rebuild without the trailing 0x00.
    f.clear();
    f.push_back(kFrameMagic);
    f.push_back(0x2D);
    f.push_back(0x08);
    f.push_back(0x00);
    f.push_back(0x00);
    f.push_back(level);
    f.push_back(0x00);
    f.push_back(level);
    f.push_back(0x00);
    f.push_back(level);
    f.push_back(0x00);
    std::span<const uint8_t> payload{f.data() + 1, f.size() - 1};
    f.push_back(checksum(payload));
    return f;
}

std::vector<uint8_t> buildLcdBrightness(uint8_t level)
{
    // FF 4F 02 <b> 00 CKSUM
    std::vector<uint8_t> f;
    f.reserve(6);
    f.push_back(kFrameMagic);
    f.push_back(0x4F);
    f.push_back(0x02);
    f.push_back(level);
    f.push_back(0x00);
    std::span<const uint8_t> payload{f.data() + 1, f.size() - 1};
    f.push_back(checksum(payload));
    return f;
}

std::array<std::vector<uint8_t>, 2> buildVuMeter(const std::array<uint8_t, 16>& levels)
{
    // Frame: FF 66 21 <subcmd> <strip0_in><strip0_out> … <strip7_in><strip7_out>
    //   <16 × 00> CKSUM
    // Strip 0 starts at payload byte 1 (right after sub-cmd), no leading
    // padding. The two "00 00" bytes visible in baseline captures were
    // from empty strip 0 + strip 1 inputs, not an intrinsic header.
    auto build = [&](uint8_t subcmd) {
        std::vector<uint8_t> f;
        f.reserve(37);
        f.push_back(kFrameMagic);
        f.push_back(0x66);
        f.push_back(0x21);
        f.push_back(subcmd);
        for (auto b : levels) f.push_back(b);        // 16 bytes: 8 × (in, out)
        for (int i = 0; i < 16; ++i) f.push_back(0x00);
        std::span<const uint8_t> payload{f.data() + 1, f.size() - 1};
        f.push_back(checksum(payload));
        return f;
    };
    return {build(0x09), build(0x0A)};
}

std::vector<uint8_t> buildGrByte(uint8_t grByte)
{
    // FF 66 11 0F <gr> 00×15 CKSUM  (21 bytes: 3 hdr + 17 data + 1 chk = 21)
    std::vector<uint8_t> f;
    f.reserve(21);
    f.push_back(kFrameMagic);
    f.push_back(0x66);
    f.push_back(0x11);
    f.push_back(0x0F);
    f.push_back(grByte);
    for (int i = 0; i < 15; ++i) f.push_back(0x00);
    std::span<const uint8_t> payload{f.data() + 1, f.size() - 1};
    f.push_back(checksum(payload));
    return f;
}

std::vector<uint8_t> buildSelectedStripMask(uint16_t mask)
{
    // FF 66 03 06 <lo> <hi> CKSUM
    std::vector<uint8_t> f;
    f.reserve(7);
    f.push_back(kFrameMagic);
    f.push_back(0x66);
    f.push_back(0x03);
    f.push_back(0x06);
    f.push_back(static_cast<uint8_t>(mask & 0xFF));
    f.push_back(static_cast<uint8_t>((mask >> 8) & 0xFF));
    std::span<const uint8_t> payload{f.data() + 1, f.size() - 1};
    f.push_back(checksum(payload));
    return f;
}

uint8_t selCellForStrip(uint8_t strip)
{
    // From cap30 decode (re-interpreted after 2026-04-24 test): UF8's
    // leftmost strip 0 = cell 0x15; each strip right decrements cell
    // by 3. Original table had strips misread as 1-indexed because the
    // capture's REAPER project had a "TESTCS" track at index 0 before
    // the numbered tracks, so "T1 click" landed on UF8 strip 1 (not 0).
    static constexpr uint8_t kMap[8] = {0x15, 0x12, 0x0F, 0x0C, 0x09, 0x06, 0x03, 0x00};
    return (strip < 8) ? kMap[strip] : 0x00;
}

static std::vector<uint8_t> buildSelFrame(uint8_t cmd, uint8_t cell, uint8_t a, uint8_t b)
{
    std::vector<uint8_t> f;
    f.reserve(8);
    f.push_back(kFrameMagic);
    f.push_back(cmd);
    f.push_back(0x04);
    f.push_back(cell);
    f.push_back(0x00);
    f.push_back(a);
    f.push_back(b);
    std::span<const uint8_t> payload{f.data() + 1, f.size() - 1};
    f.push_back(checksum(payload));
    return f;
}

std::array<std::vector<uint8_t>, 2> buildSelColour(uint8_t strip, uint8_t byteA, uint8_t byteB)
{
    const uint8_t cell = selCellForStrip(strip);
    return {buildSelFrame(0x38, cell, byteA, byteB),
            buildSelFrame(0x39, cell, byteA, byteB)};
}

std::array<std::vector<uint8_t>, 2> buildSelWhite(uint8_t strip, bool bright)
{
    // From cap30: white-dim = 00 11 F1 (both FF38 and FF39 symmetric).
    // white-bright: only FF 38 fires with 00 FF FF; FF 39 is omitted by SSL.
    // We mirror that behaviour — for `bright` the FF 39 entry is an empty
    // vector the caller should skip-if-empty.
    if (bright) {
        const uint8_t cell = selCellForStrip(strip);
        return {buildSelFrame(0x38, cell, 0xFF, 0xFF), std::vector<uint8_t>{}};
    }
    return buildSelColour(strip, 0x11, 0xF1);
}

bool verifyFrame(std::span<const uint8_t> frame)
{
    if (frame.size() < 3) return false;
    if (frame.front() != kFrameMagic) return false;
    std::span<const uint8_t> payload{frame.data() + 1, frame.size() - 2};
    return checksum(payload) == frame.back();
}

std::optional<ButtonEvent> parseButtonEvent(std::span<const uint8_t> bytes)
{
    // Strip the 31 60 prefix if present (incoming UF8 wire form)
    if (bytes.size() >= 2 && bytes[0] == 0x31 && bytes[1] == 0x60) {
        bytes = bytes.subspan(2);
    }
    // Need exactly: FF 22 03 <id> 00 <state> CKSUM   (7 bytes)
    if (bytes.size() < 7) return std::nullopt;
    if (bytes[0] != kFrameMagic || bytes[1] != 0x22 || bytes[2] != 0x03) return std::nullopt;
    if (bytes[4] != 0x00) return std::nullopt;

    std::span<const uint8_t> payload{bytes.data() + 1, 5};  // 22 03 id 00 state
    if (checksum(payload) != bytes[6]) return std::nullopt;

    return ButtonEvent{bytes[3], bytes[5] == 0x01};
}

} // namespace uf8
