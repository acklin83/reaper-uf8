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

// Set the motor fader position on one strip:
//   FF 1E 03 <strip> <LSB> <MSB> CKSUM     (7 bytes)
// LSB/MSB are the 7-bit halves of the MCU pitch-bend value (0..0x3FFF).
// We pass them through verbatim — SSL 360° does the same in captures.
std::vector<uint8_t> buildFaderPosition(uint8_t strip, uint8_t lsb, uint8_t msb);

// Enable / disable the fader motor on one strip:
//   FF 1D 02 <strip> <enable> CKSUM        (6 bytes)
// enable = 0x01 → motor active (tracks host position)
// enable = 0x00 → motor limp (user can move fader freely)
// SSL 360° toggles this in response to the UF8's FF 20 02 capacitive
// touch events, so the motor releases under the user's finger.
std::vector<uint8_t> buildMotorEnable(uint8_t strip, bool enable);

// Drive one strip's VU meter.
//   FF 38 04 <strip*3> 00 00 <0xF0 | level> CKSUM     (8 bytes)
// Best-guess from cap10 frame layout. level is the MCU meter nibble
// (0..0xE full scale, 0xF clip). Actual byte semantics still partially
// TBD — refine once we have captures with known audio levels to verify.
std::vector<uint8_t> buildMeter(uint8_t strip, uint8_t level);

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
