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

// Per-strip button LEDs — monochrome on/off path (legacy, unused for
// SOLO/CUT/SEL because it lights LEDs uncoloured/white).
//   FF 3B 03 <led_id> 00 <state> CKSUM    (7 bytes)
// Kept around for any LED class that doesn't have a colour-pair mapping.
std::vector<uint8_t> buildLedCommand(uint8_t ledId, bool on);

// Per-strip button LEDs — full-colour path (cap31, 2026-04-26).
// SSL 360° drives SOLO yellow / CUT orange / SEL white via this pair-write
// for every state change in DAW Layer. Replaces buildLedCommand for
// SOLO/CUT/SEL.
//
//   FF 38 04 <cell> 00 <a> <b> CKSUM   (8 bytes)
//   FF 39 04 <cell> 00 <a> <b> CKSUM   (8 bytes)
//
// Cell formula (24 LEDs, contiguous descending in id space):
//   cell = 0x17 - 3*strip - led_offset
//     strip:       0..7
//     led_offset:  SOLO=0, MUTE/CUT=1, SEL=2
//
// ON state:  FF38 = bright colour bytes, FF39 = base colour bytes.
// OFF state: FF38 == FF39 = dim colour bytes.
enum class LedClass : uint8_t {
    Solo = 0,
    Cut  = 1,
    Sel  = 2,
};

// 4 bytes drive a coloured LED in DAW-Colour mode: bright pair (when lit)
// + dim pair (when un-lit). All four come from cap31/cap33 byte tables.
struct LedColour {
    uint8_t aBright;
    uint8_t bBright;
    uint8_t aDim;
    uint8_t bDim;
};

// Class defaults — yellow for SOLO, red for CUT, white for SEL.
LedColour ledColourYellow();   // SOLO
LedColour ledColourRed();      // CUT
LedColour ledColourWhite();    // SEL when track has no custom colour
LedColour ledColourOrange();   // SSL360 default for CUT (cap31), still available

// Map a REAPER track-colour (0x00RRGGBB) onto SSL360's SEL DAW-Colour
// palette by Euclidean nearest-match against the 10 distinct anchors
// captured in cap33. A track with no custom colour (rgb == 0) returns
// `ledColourWhite()`.
LedColour ledColourForTrackRgb(uint32_t rgb);

// Default colour for a class when no track-colour override is requested.
LedColour ledColourClassDefault(LedClass cls);

// Build the FF 38 + FF 39 frame pair as TWO separate 8-byte frames.
// SSL360's captures show these always traverse the bus as independent USB
// transfers — combining them into one transfer makes the UF8 firmware
// treat the second frame as garbage and stall subsequent commands (e.g.
// fader-motor writes go silent).
struct LedColourFrames {
    std::vector<uint8_t> ff38;
    std::vector<uint8_t> ff39;
};

LedColourFrames buildLedColourPair(uint8_t strip, LedClass cls, bool on,
                                   LedColour colour);

// Convenience overload — uses ledColourClassDefault(cls) for the colour.
LedColourFrames buildLedColourPair(uint8_t strip, LedClass cls, bool on);

// Per-strip top-soft-key LED (cap41, 2026-04-28). Cell range `0x18..0x1F`
// — formula `cell = 0x1F - strip` (strip 0 leftmost = 0x1F, strip 7 = 0x18).
// Same FF 38/39 04 pair-write family as SEL/CUT/SOLO. Three visible
// states for the SSL-mode soft-key indicator:
//   On:  FF38 = bright bytes, FF39 = `00 F0`         — focused param
//   Dim: FF38/FF39 both carry the colour's dim bytes — slot available
//   Off: FF38/FF39 both `00 F0`                      — kNoSlot / dark
enum class TopSoftKeyState : uint8_t { Off, Dim, On };
LedColourFrames buildTopSoftKeyLed(uint8_t strip, TopSoftKeyState state,
                                   LedColour colour);

// Global button LEDs (cap35/36, 2026-04-26). Same FF 38/39 04 pair-write
// frame family, but the cell ranges 0x18..0x60 are the 30+ LEDs around
// the buttons in the upper section of the UF8 (Layer/Send/Plugin row,
// Soft Keys, modifiers, Zoom, etc.). Off-state = `11 F1` (dim white)
// for plain-white buttons; coloured ones reuse their bright pair as the
// dim value with both FF38/FF39 set to a "dim variant".
enum class Uf8GlobalLed : uint8_t {
    Layer1, Layer2,
    SendPlugin1, SendPlugin2, SendPlugin3, SendPlugin4,
    SendPlugin5, SendPlugin6, SendPlugin7, SendPlugin8,
    Plugin,
    PageLeft, PageRight, Flip,
    AutoRead, AutoWrite, AutoTrim, AutoLatch, AutoTouch,
    VPotBank, Soft1, Soft2, Soft3, Soft4, Soft5,
    Pan, Fine, Norm, Rec, Auto, Nav, Nudge, Focus,
    BankLeft, BankRight,
    ZoomUp, ZoomLeft, ZoomCenter, ZoomRight, ZoomDown,
};

LedColourFrames buildUf8GlobalLed(Uf8GlobalLed btn, bool on);

// Selected-strip bitmask. cap33 shows SSL360 sending this on every
// selection change in PM Layer + DAW Colour mode — it's what tells the
// firmware which SEL LED is "the lit one", so the stored colour bytes
// actually render as track colour. Without it the firmware falls back
// to white for any newly-selected SEL.
//   FF 66 03 06 <mask_lo> <mask_hi> CKSUM    (7 bytes)
// 16-bit LE bitmask, bit `s` = strip s selected (0-indexed, leftmost = 0).
std::vector<uint8_t> buildSelectedStripBitmask(uint16_t mask);

// Channel Number Zone — the small numeric digit top-left of each scribble
// strip's color bar (REAPER track index rendered as ASCII digits).
//   FF 66 <len> 14 <strip> <N ASCII chars> CKSUM    (len = N + 2)
// Variable length: 1..9 = single digit, 10..99 = two digits, 100+ = three.
// Decoded from cap21_chan_no (2026-04-21) by capturing BANK ←/→ events.
std::vector<uint8_t> buildChannelNumber(uint8_t strip, std::string_view digits);

// Value Line — combined parameter name + value on one row, 19 chars wide.
// Left-justified name, right-justified value, spaces in between.
// Example: "In Trim       0.0dB", "HF Freq     8.00kHz".
//   FF 66 15 0E <strip> <19 ASCII chars> CKSUM      (24 bytes)
std::vector<uint8_t> buildValueLine(uint8_t strip, std::string_view nineteenChars);

// V-Pot Readout Bar — the horizontal bar under each scribble strip that
// shows the V-Pot's current position.
//
// SSL 360° uses `FF 66 11 0F <16 bytes> CKSUM` (20 B broadcast). Two
// bytes per strip, little-endian, 8 strips. First byte = position 0..255,
// second byte = mode flag (0x00 during V-Pot motion; various values seen
// during PM-layer init). Confirmed by cap20 capture on 2026-04-21 — SSL
// 360° fires this command *only* when the V-Pot rotates, and updates
// just the active strip's 2 bytes while zeroing the rest.
//
// Earlier prototype used `FF 66 09 0D <8 bytes>` which turned out to be
// something else entirely (sent on soft-key mode changes, not V-Pot motion).
std::vector<uint8_t> buildVPotReadoutBar(const std::array<uint16_t, kStripCount>& positions);

// O/PdB Fader Readout — the big fader-dB number.
// Format reserves 6 ASCII bytes for the value followed by the literal "dB"
// suffix. SSL 360° captures (cap16) only fill the first 4 bytes and leave
// the trailing two as NUL — the LCD renders whatever ASCII is present, so
// we use the full 6 bytes when needed (e.g. "-12.5") and NUL-pad otherwise.
//   FF 66 0A 0C <strip> <6 bytes> "dB" CKSUM  (14 bytes)
std::vector<uint8_t> buildFaderDbReadout(uint8_t strip, std::string_view value);

// ---- Decoded 2026-04-24 ----

// LED master brightness (all non-LCD LEDs on the UF8):
//   FF 2D 08 00 00 <b> 00 <b> 00 <b> 00 CKSUM    (12 bytes)
// The triplet carries identical brightness bytes in SSL 360° captures —
// likely per-RGB-channel driver level. We mirror that (all 3 equal).
// Per-step values: dark=0x05, dim=0x0A, half=0x10, bright=0x13, full=0x20.
std::vector<uint8_t> buildLedBrightness(uint8_t level);

// LCD / scribble backlight brightness:
//   FF 4F 02 <b> 00 CKSUM     (6 bytes)
// Per-step values: dark=0x18, dim=0x30, half=0x50, bright=0x60, full=0xA0.
std::vector<uint8_t> buildLcdBrightness(uint8_t level);

// VU meter — 8 strips × (input, output) per frame.
// Previously classified as idle heartbeat `FF 66 21 09`. 37-byte frame:
//   FF 66 21 09 00 00 <s0_in> <s0_out> <s1_in> <s1_out> … <s7_in> <s7_out> <14 × 00> CKSUM
// Each level ∈ 0x00..0x1F. Sibling `FF 66 21 0A` has the same layout
// (probably output-side or confirmation); we send both.
// `levels` is [s0_in, s0_out, s1_in, s1_out, …] = 16 bytes.
std::array<std::vector<uint8_t>, 2> buildVuMeter(const std::array<uint8_t, 16>& levels);

// GR meter (single byte, focused-strip's CS dynamics):
//   FF 66 11 0F <gr_byte> 00x15 CKSUM    (21 bytes)
// Observed range 0x22..0x64 during a CS compressor ramp; larger = more
// GR. UF8 renders the on-screen GR arc from this byte.
std::vector<uint8_t> buildGrByte(uint8_t grByte);

// Selected-strip 16-bit bitmask. Fires on selection change.
//   FF 66 03 06 <lo> <hi> CKSUM    (7 bytes)
// bit N = strip N selected (strip 1 = bit 1 = 0x0002, …, strip 8 = 0x0100).
std::vector<uint8_t> buildSelectedStripMask(uint16_t mask);

// SEL LED per-strip colour (bank 0x00 of the FF 38/39 family, NOT the
// button-LED bank). Two coupled frames per strip — bytes encode a 2-byte
// colour+brightness pair (FF38 = primary, FF39 = mirror/confirmation).
//   FF 38 04 <cell> 00 <a> <b> CKSUM    (8 bytes)
//   FF 39 04 <cell> 00 <a> <b> CKSUM    (8 bytes)
// Cell → strip map (from cap30 decode): strip 1=0x12, 2=0x0F, 3=0x0C,
// 4=0x09, 5=0x06, 6=0x03, 7=0x00, 8=0x15.
// Byte semantics still partially TBD; we send white values (a=0x11,
// b=0xF1 dim / a=0xFF, b=0xFF bright) as a safe default while the
// track-colour-to-bytes mapping is refined.
uint8_t selCellForStrip(uint8_t strip);
std::array<std::vector<uint8_t>, 2> buildSelColour(uint8_t strip, uint8_t byteA, uint8_t byteB);

// Convenience: build a pair of {FF38, FF39} for the "white dim"
// (unselected) / "white bright" (selected) colour states — no track
// colour lookup needed.
std::array<std::vector<uint8_t>, 2> buildSelWhite(uint8_t strip, bool bright);

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
