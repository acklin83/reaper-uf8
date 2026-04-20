#pragma once
//
// SSL UF8 wire protocol — frame construction and parsing.
//
// Every frame (both directions):
//   FF <payload bytes> <checksum>
// checksum = sum(payload bytes) mod 256
//
// The commands we actually build/parse are documented in docs/protocol-notes.md.
//

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace uf8 {

constexpr uint8_t kFrameMagic = 0xFF;
constexpr size_t  kStripCount = 8;

struct ButtonEvent {
    uint8_t id;       // 0x78 = BANK->, 0x79 = BANK<-, others TBD
    bool    pressed;  // true on press, false on release
};

// Build "set colors" command:
//   FF 66 09 18 <8 palette indices> CKSUM
// Returns 14 bytes. Safe to call from any thread; pure function.
std::vector<uint8_t> buildColorCommand(const std::array<uint8_t, kStripCount>& paletteIndices);

// Build the plugin-mixer heartbeat pair (13 B each):
//   FF 66 09 15 00x8 84
//   FF 66 09 16 00x8 85
std::array<std::vector<uint8_t>, 2> buildPluginMixerHeartbeat();

// Build a scribble-strip-text command for a single strip.
//
// There are two physical rows on the UF8 display. SSL 360° uses two
// different command types:
//
//   Upper row (where track names go):
//     FF 66 <len> 0B <strip> <N chars ASCII> CKSUM    (variable length, N chars 0..7)
//     len = N + 2
//
//   Lower row (where v-pot / value text goes):
//     FF 66 09 0E <strip> <7 ASCII chars, space-padded> CKSUM  (fixed 13 bytes)
//
// `text` longer than 7 chars is truncated. Upper-row is not space-padded
// (sent at natural length); lower-row is always 7 chars.
std::vector<uint8_t> buildStripTextUpper(uint8_t strip, std::string_view text);
std::vector<uint8_t> buildStripTextLower(uint8_t strip, std::string_view text);

// Verify a frame's checksum. Returns true if frame starts with FF and the
// last byte matches sum(middle bytes) mod 256.
bool verifyFrame(std::span<const uint8_t> frame);

// Parse an incoming button event. Accepts either the raw 9-byte frame
// (FF 22 03 ..) or the 11-byte UF8 wire form including the 31 60 prefix.
// Returns std::nullopt if the bytes don't parse as a button event.
std::optional<ButtonEvent> parseButtonEvent(std::span<const uint8_t> bytes);

// Compute the checksum byte for a payload (exposed mainly for tests).
uint8_t checksum(std::span<const uint8_t> payload);

} // namespace uf8
