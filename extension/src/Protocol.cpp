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
