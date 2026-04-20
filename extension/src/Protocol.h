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

// Switch UF8 display mode:
//   Plugin-Mixer Layer: FF 66 11 0F 10 00 40 00 <20 00 × 6> 96   (shows color bar)
//   DAW Layer:          FF 66 11 0F <00 × 16> 86                  (no color bar)
// Decoded from a capture where the user pressed the physical layer
// button on the UF8 three times; this command arrived from SSL 360°
// within ~30 ms of each button press.
std::vector<uint8_t> buildLayerPluginMixer();
std::vector<uint8_t> buildLayerDaw();

// Plugin-Mixer "slot populate" frames. SSL 360° gates the color bar
// rendering on the plugin slot being marked as occupied. An empty slot
// (`FF 66 02 04 <strip>`) leaves the bar dark even when a color command
// arrives; a populated slot (`FF 66 <len> 04 <strip> <text>`) makes the
// bar respond to `FF 66 09 18`.
//
// buildPluginSlotActive(): one-shot "all 8 slots populated" flag —
//   FF 66 0A 00 03 00 00 00 00 00 00 00 00 CKSUM       (13 bytes)
//   (our replay captured the "empty" 0x02 variant; 0x03 is the populated
//    variant seen in cap13 right before colors became visible).
// buildPluginSlotName(strip, text): per-strip slot-occupant label —
//   FF 66 <len> 04 <strip> <N ASCII chars> CKSUM       (len = N + 2)
//   Text longer than ~12 chars is truncated.
std::vector<uint8_t> buildPluginSlotActive();
std::vector<uint8_t> buildPluginSlotName(uint8_t strip, std::string_view text);

// Plug-in Mixer / Channel Strip Mode LCD zones — per-strip addressable.
// All decoded from cap14a–cap18 on 2026-04-20.
//
// Channel Strip Type ("CS 2" / "4K B" / "4K E" / "BusComp"):
//   FF 66 06 17 <strip> <4 ASCII chars> CKSUM       (9 bytes)
std::vector<uint8_t> buildChannelStripType(uint8_t strip, std::string_view fourChars);

// Value Line — combined parameter name + value on one row, 19 chars wide.
// Left-justified name, right-justified value, spaces in between.
// Example: "In Trim       0.0dB", "HF Freq     8.00kHz".
//   FF 66 15 0E <strip> <19 ASCII chars> CKSUM      (24 bytes)
std::vector<uint8_t> buildValueLine(uint8_t strip, std::string_view nineteenChars);

// O/PdB Fader Readout — the big fader-dB number.
// Format places a 4-char value field (e.g. "-0.1", "0.0", "12.0") followed
// by two NUL bytes and the literal "dB" suffix. Caller passes the value
// as a string (shorter strings right-padded with NUL inside the frame).
//   FF 66 0A 0C <strip> <4 bytes> 00 00 "dB" CKSUM  (14 bytes)
std::vector<uint8_t> buildFaderDbReadout(uint8_t strip, std::string_view fourChars);

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
