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
