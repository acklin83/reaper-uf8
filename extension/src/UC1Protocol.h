#pragma once
//
// SSL UC1 wire protocol — frame construction and parsing.
//
// Every frame (both directions):
//   FF <cmd> <len> <payload×len> <checksum>
// checksum = sum(cmd + len + payload bytes) mod 256
//
// Full decoded protocol in docs/protocol-notes-uc1.md (as of 2026-04-23).
//
// The UC1 has no mode-switching: its hardware is a fixed layout where the
// top-center 7 V-Pots map to Bus Comp 2 params and the surrounding pots
// drive Channel Strip 2 params (4K E, 4K G, 4K B, CS 2 — SSL 360° adapts).
// Display zone 0x05 is the Bus Comp readout, zone 0x03 is the Channel Strip
// readout, time-multiplexed to whichever knob is currently being turned.
//

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace uc1 {

constexpr uint8_t  kFrameMagic = 0xFF;
constexpr uint16_t kVid        = 0x31E9;
constexpr uint16_t kPid        = 0x0023;  // UF8 is 0x0021, UC UF8-HID is 0x0022
constexpr uint8_t  kEpOut      = 0x02;
constexpr uint8_t  kEpIn       = 0x81;

// Knob IDs (from docs/protocol-notes-uc1.md — direct-display evidence).
//
// The top-center V-Pots (0x0C-0x16 range) repurpose per plugin; with Bus
// Comp 2 loaded they drive Bus Comp params, without it SSL 360° maps some
// of them (0x0C, 0x16) to Channel Strip params instead.
namespace knob {
    // Top 7 Bus Comp V-Pots — IDs 0x0E..0x14, confirmed from uc1_XX
    // hardware session 2026-04-23 via the per-ID param-name diagnostic.
    // Each physical knob has a unique ID; no V-Pot repurposing of the
    // Bus Comp row (the earlier hypothesis that 0x0C/0x16 repurpose
    // was wrong — those IDs belong to Channel-Strip-only pots).
    constexpr uint8_t kBCRatio      = 0x0E;
    constexpr uint8_t kBCScHpf      = 0x0F;
    constexpr uint8_t kBCAttack     = 0x10;
    constexpr uint8_t kBCRelease    = 0x11;
    constexpr uint8_t kBCThreshold  = 0x12;
    constexpr uint8_t kBCMakeup     = 0x13;
    constexpr uint8_t kBCMix        = 0x14;

    // Channel-Strip-only V-Pot IDs (no Bus Comp collision)
    constexpr uint8_t kCSInputTrim  = 0x0C;
    constexpr uint8_t kCSFaderLevel = 0x16;

    // Central control panel — CHANNEL encoder. SSL uses this to scroll
    // through the Plug-in Mixer; Rea-Sixty uses it to scroll through
    // REAPER's track selection.
    constexpr uint8_t kChannelEncoder = 0x0D;

    // Secondary encoder right of the central screen. SSL uses it to
    // select a Bus Compressor 2 instance; Rea-Sixty uses it to jump
    // between tracks that have the BC-section-target plugin loaded
    // (Bus Compressor 2 today, any user-mapped plugin once the
    //  Link-System config page lands).
    constexpr uint8_t kBcEncoder      = 0x15;

    // Dedicated Channel Strip left-side pots (EQ + filters)
    constexpr uint8_t kCSLowPass    = 0x00;
    constexpr uint8_t kCSHighPass   = 0x01;
    constexpr uint8_t kCSHfGain     = 0x02;
    constexpr uint8_t kCSHfFreq     = 0x03;
    constexpr uint8_t kCSHmfGain    = 0x04;
    constexpr uint8_t kCSHmfFreq    = 0x05;
    constexpr uint8_t kCSHmfQ       = 0x06;
    constexpr uint8_t kCSLmfGain    = 0x07;
    constexpr uint8_t kCSLmfFreq    = 0x08;
    constexpr uint8_t kCSLmfQ       = 0x09;
    constexpr uint8_t kCSLfFreq     = 0x0A;
    constexpr uint8_t kCSLfGain     = 0x0B;

    // Dedicated Channel Strip right-side pots (Dyn + Gate)
    constexpr uint8_t kCSGateRelease   = 0x17;
    constexpr uint8_t kCSGateHold      = 0x18;
    constexpr uint8_t kCSGateThreshold = 0x19;
    constexpr uint8_t kCSGateRange     = 0x1A;
    constexpr uint8_t kCSCompRelease   = 0x1B;
    constexpr uint8_t kCSCompThreshold = 0x1C;
    constexpr uint8_t kCSCompRatio     = 0x1D;
}

namespace button {
    // EQ section
    constexpr uint8_t kHfBell     = 0x08;
    constexpr uint8_t kEqType     = 0x09;
    constexpr uint8_t kEqIn       = 0x0A;
    constexpr uint8_t kLfBell     = 0x0B;

    // Bus Comp section (single enable button — rest of Bus Comp is V-Pots)
    constexpr uint8_t kBusCompIn  = 0x0C;

    // Menu / navigation row (decoded uc1_35_menu_buttons 2026-05-01).
    constexpr uint8_t kBack       = 0x0E;
    constexpr uint8_t kConfirm    = 0x0F;
    constexpr uint8_t kRouting    = 0x10;
    constexpr uint8_t kPresets    = 0x11;
    constexpr uint8_t k360        = 0x12;  // 360 knob press
    constexpr uint8_t kMagnifier  = 0x13;

    // Dyn section
    constexpr uint8_t kFastAttComp = 0x14;
    constexpr uint8_t kPeak        = 0x15;
    constexpr uint8_t kDynIn       = 0x16;
    constexpr uint8_t kExpand      = 0x17;
    constexpr uint8_t kFastAttGate = 0x18;

    // Channel section
    constexpr uint8_t kPolarity    = 0x19;
    constexpr uint8_t kScListen    = 0x1A;
    constexpr uint8_t kSoloClear   = 0x1B;
    constexpr uint8_t kSolo        = 0x1C;
    constexpr uint8_t kCut         = 0x1D;
    constexpr uint8_t kChannelIn   = 0x1E;
    constexpr uint8_t kFine        = 0x1F;
}

// LED cells for each button's feedback LED (FF 13 04 <bank> <cell> 01 <state>).
// bank is 0x02 for every button except Solo Clear which uses bank 0x01.
// state byte: 0x00 = off, 0x33 = dim (plugin bypassed), 0xFF = on.
namespace led {
    struct Cell { uint8_t bank; uint8_t cell; };

    constexpr Cell kHfBell      {0x02, 0x89};
    constexpr Cell kEqType      {0x02, 0x51};
    constexpr Cell kEqIn        {0x02, 0x50};
    constexpr Cell kLfBell      {0x02, 0x23};
    constexpr Cell kBusCompIn   {0x02, 0x01};
    constexpr Cell kFastAttComp {0x02, 0x38};
    constexpr Cell kPeak        {0x02, 0x39};
    constexpr Cell kDynIn       {0x02, 0x5B};
    constexpr Cell kExpand      {0x02, 0x92};
    constexpr Cell kFastAttGate {0x02, 0x93};
    constexpr Cell kPolarity    {0x02, 0x98};
    constexpr Cell kScListen    {0x02, 0x99};
    constexpr Cell kSolo        {0x02, 0x97};
    constexpr Cell kSoloClear   {0x01, 0x9A};  // cycles 01 → 03 → 00 on state transitions
    constexpr Cell kCut         {0x02, 0x96};
    constexpr Cell kChannelIn   {0x02, 0x94};
    constexpr Cell kFine        {0x02, 0x95};

    constexpr uint8_t kStateOff  = 0x00;
    constexpr uint8_t kStateDim  = 0x33;   // plugin-bypassed dim level
    constexpr uint8_t kStateOn   = 0xFF;
}

// Display zones. 22-char fixed-width label+value fields, padded with
// spaces. Each zone is a distinct UI element on the UC1.
namespace zone {
    constexpr uint8_t kChannelStripContext = 0x02;  // 37 B, strip-layout markers
    constexpr uint8_t kChannelStripReadout = 0x03;  // 22 B, "<label>   <value>"
    constexpr uint8_t kGlobalStatus        = 0x04;  // 43 B, "No Plug-ins" / track context
    constexpr uint8_t kBusCompReadout      = 0x05;  // 22 B, same format as 0x03
    constexpr uint8_t kPluginStateTag      = 0x0E;  // "Off" / "N/A" / "On"
    constexpr uint8_t kPluginNameTag       = 0x10;  // "4K E" / "CS 2" / ...
}

struct ButtonEvent {
    uint8_t id;       // see namespace button
    bool    pressed;  // true on press, false on release
};

struct KnobEvent {
    uint8_t id;      // see namespace knob
    int8_t  delta;   // signed step count, +1 = CW click, -1 = CCW click
};

// ---- Frame builders (host → UC1, EP 0x02) ----

// 4-phase keepalive / watchdog token. SSL 360° sends these at ~1 Hz with
// counter cycling 0x00 → 0x01 → 0x02 → 0x03. UC1 declares itself lost if
// the stream stops.
//   FF 1B 01 <counter> <chk>         (5 bytes)
std::vector<uint8_t> buildKeepalive(uint8_t counter);

// Gain Reduction meter (Bus Comp center section). Drives the BC mechanical
// analog needle. 16-bit big-endian value in units of 1/10 dB (0 dB = 0x0000,
// 20 dB = 0x00C8). cap43 confirmed linear position = round(dB × 10), range
// 0..200 mapping to 0..20 dB GR. Streamed at ~50 Hz by UC1Device's worker.
//   FF 5B 02 <hi> <lo> <chk>         (6 bytes)
std::vector<uint8_t> buildGrMeter(float dB);

// Cosmetic single-shot "needle-pose" sent on every BC-bypass-state-change
// press, mirroring SSL 360°'s behaviour. Not streamed — fire once per
// transition. Positions are fixed (cap45):
//   entering=true  → FF 5C 02 00 0A 68    (pos 10 ≈ 1 dB on the 0..20 scale)
//   entering=false → FF 5C 02 00 32 90    (pos 50 ≈ 5 dB on the 0..20 scale)
// Skipping these is safe — the meter still works without the cosmetic blip.
std::vector<uint8_t> buildBcBypassPose(bool entering);

// VU meter (Channel Strip I/O meter strip). Bank is fixed 0x01 for VU.
// `meter`: 0 = input, 1 = output. `level`: byte value 0..0xFF.
//   FF 13 04 01 <level> 01 <meter> <chk>     (8 bytes)
std::vector<uint8_t> buildVuMeter(uint8_t meter, uint8_t level);

// Per-LED cell write (the command family used for all per-button LED
// feedback plus VU strips — see docs/protocol-notes-uc1.md).
//   FF 13 04 <bank> <cell> 01 <state> <chk>      (8 bytes)
// state: led::kStateOff / kStateDim / kStateOn.
std::vector<uint8_t> buildLedWrite(uint8_t bank, uint8_t cell, uint8_t state);

// Display text write for a given zone.
//   FF 66 <len> <zone> <ASCII bytes> <chk>
// len = (ASCII length) + 1. Caller ensures the string fits the zone's
// width (22 for 0x03/0x05). Strings shorter than the zone width are
// space-padded inside this helper.
std::vector<uint8_t> buildDisplayText(uint8_t zone, std::string_view text, size_t width = 22);

// Zero-GR convenience: FF 5B 02 00 00.
inline std::vector<uint8_t> buildZeroGr() { return buildGrMeter(0.0f); }

// Build the set of OUT frames that together drive the UC1's central
// 7-segment display to show `value` (0..999). Each digit's segments
// are individual LED cells on bank 0x01:
//
//   Ones digit:  cells 0x10..0x16 = segments a..g alphabetical
//   Tens digit:  cells 0x08..0x0E = segments a..g alphabetical
//   Hundreds:    cells 0x00,0x03,0x04,0x05 — only partially decoded
//                from uc1_27 (99→100 transition only lit "1"-segments);
//                values ≥ 100 currently render the ones+tens faithfully
//                and leave the hundreds cells at whatever state they
//                inherit from the prior display.
//
// Each segment gets FF 13 04 01 <cell> 00 <state> with state=0xFF (on)
// or 0x00 (off). Caller should send ALL returned frames in order.
std::vector<std::vector<uint8_t>> buildSevenSeg(unsigned int value);

// Build the zone 0x02 Channel-Strip context frame with the DAW track
// name for the Channel Strip section. 36-byte content field; the track
// name is written at position 12 (from uc1_25: "TESTCS" landed there).
// Empty name leaves the field zero-filled (UC1 default is "-------"
// dashes or "No Plug-ins" depending on init state).
//
//   FF 66 25 02 <12 zeros> <name 0..12 chars> <zeros to fill 36> <chk>
std::vector<uint8_t> buildChannelStripContext(std::string_view name);

// Build the zone 0x04 Bus-Compressor context frame. 42-byte content
// field; track name at position 14 (from uc1_24b).
//
//   FF 66 2B 04 <14 zeros> <name 0..14 chars> <zeros to fill 42> <chk>
std::vector<uint8_t> buildBusCompContext(std::string_view name);

// ---- Decoded 2026-04-24 ----

// LED master brightness (non-LCD LEDs on UC1):
//   FF 14 02 <b> 00 CKSUM    (6 bytes)
// Per-step: dark=0x0A, dim=0x13, half=0x20, bright=0x26, full=0x40.
std::vector<uint8_t> buildLedBrightness(uint8_t level);

// LCD / display backlight brightness. Same encoding as UF8's.
//   FF 4F 02 <b> 00 CKSUM    (6 bytes)
// Per-step: dark=0x18, dim=0x30, half=0x50, bright=0x60, full=0xA0.
std::vector<uint8_t> buildLcdBrightness(uint8_t level);

// Status / GR-area auxiliary brightness (role TBD; observed alongside
// the main brightness changes).
//   FF 5C 02 00 <b> CKSUM    (6 bytes)
// Per-step: dark=0x08, dim=0x0F, half=0x19, bright=0x1E, full=0x32.
std::vector<uint8_t> buildStatusBrightness(uint8_t level);

// Focused-track colour bar. Single palette byte (same indices as UF8's
// palette 0x01..0x0B, plus 0x00/0x0C-0x0F render OFF).
//   FF 66 02 11 <palette_idx> CKSUM    (6 bytes)
std::vector<uint8_t> buildFocusedColour(uint8_t paletteIdx);

// Colour-bar enable flag (and "MAIN" mode toggle). flag=0x01 shows the
// plugin-context colour bar; flag=0x00 puts UC1 in MAIN/idle mode.
//   FF 66 03 00 01 <flag> CKSUM    (7 bytes)
std::vector<uint8_t> buildColourBarEnable(bool on);

// Central label — 4-character plugin-type tag shown in the central LCD
// area. "MAIN" when no SSL plugin is focused; "CS 2", "4K E", "BC 2",
// etc. when a plugin is loaded. Observed analog to UF8's Channel Strip
// Type zone but UC1-specific framing.
//   FF 66 05 01 <4 ASCII> CKSUM    (9 bytes)
std::vector<uint8_t> buildCentralLabel(std::string_view fourChars);

// Central Control Panel mode banner (decoded uc1_37 2026-05-01).
// Frame: FF 66 03 00 <mode> 00 <chk>. Selects which top-of-LCD layout
// the firmware renders.
//   0x01 = MAIN (default carousel + plug-in name)
//   0x04 = ROUTING (graph + order indicator)
//   0x06 = EXTENDED FUNCTIONS (param scroll list)
//   PRESETS / TRANSPORT bytes still TBD (separate capture).
enum class CentralMode : uint8_t {
    Main      = 0x01,
    Routing   = 0x04,
    ExtFuncs  = 0x06,
    // Presets / Transport are placeholders; real bytes pending.
    Presets   = 0x05,
    Transport = 0x03,
};
std::vector<uint8_t> buildCentralMode(CentralMode m);

// Routing-order indicator on the ROUTING-mode LCD graph (decoded
// uc1_37 2026-05-01). Frame: FF 66 02 0A <byte> <chk>.
//   byte 0x01..0x0A → orders 1..10 (main path)
//   byte 0x81..0x8A → b-variants (bit 7 = external S/C marker)
std::vector<uint8_t> buildRoutingOrderIndicator(uint8_t orderByte);

// Central Control Panel status indicator LEDs above the menu-row
// buttons (decoded uc1_37 2026-05-01). Each is a single bank=0x02
// brightness write at byte5=0x01.
//   BC mode dot   → cell 0x9E (lit in MAIN, dark in any menu mode)
//   Routing dot   → cell 0x9F (lit only in ROUTING)
//   Presets dot   → cell 0xA0 (lit only in PRESETS — TBD capture)
constexpr uint8_t kCellBcModeDot   = 0x9E;
constexpr uint8_t kCellRoutingDot  = 0x9F;
constexpr uint8_t kCellPresetsDot  = 0xA0;
std::vector<uint8_t> buildMenuStatusDot(uint8_t cell, bool on);

// Track-name carousel — 3-slot version for both small (0x02) and large
// (0x04) zones. Each slot is left-aligned, zero-padded to its slot
// width; slots are [prev, current, next] in the frame.
//   FF 66 25 02 <3 × 12-byte slots> CKSUM    (41 bytes)
//   FF 66 2B 04 <3 × 14-byte slots> CKSUM    (47 bytes)
std::vector<uint8_t> buildTrackNameTripleSmall(std::string_view prev,
                                               std::string_view curr,
                                               std::string_view next);
std::vector<uint8_t> buildTrackNameTripleLarge(std::string_view prev,
                                               std::string_view curr,
                                               std::string_view next);

// Convenience constants for brightness levels (matches decoded table).
namespace brightness {
    constexpr uint8_t kDark   = 0x0A;
    constexpr uint8_t kDim    = 0x13;
    constexpr uint8_t kHalf   = 0x20;
    constexpr uint8_t kBright = 0x26;
    constexpr uint8_t kFull   = 0x40;

    constexpr uint8_t kLcdDark   = 0x18;
    constexpr uint8_t kLcdDim    = 0x30;
    constexpr uint8_t kLcdHalf   = 0x50;
    constexpr uint8_t kLcdBright = 0x60;
    constexpr uint8_t kLcdFull   = 0xA0;

    constexpr uint8_t kStatusDark   = 0x08;
    constexpr uint8_t kStatusDim    = 0x0F;
    constexpr uint8_t kStatusHalf   = 0x19;
    constexpr uint8_t kStatusBright = 0x1E;
    constexpr uint8_t kStatusFull   = 0x32;
}

// ---- Frame parser (UC1 → host, EP 0x81) ----
//
// EP 0x81 IN frames arrive as a 2-byte USB poll token `31 XX` followed
// by an optional event frame:
//   31 00            → empty poll (no event)
//   31 60 FF 22 ...  → button event
//   31 60 FF 24 ...  → knob event
// Both parsers accept either the raw FF-prefixed frame or the full
// 31 60-prefixed poll form.
std::optional<ButtonEvent> parseButtonEvent(std::span<const uint8_t> bytes);
std::optional<KnobEvent>   parseKnobEvent  (std::span<const uint8_t> bytes);

// Verify a frame's checksum. Returns true if bytes[0] == 0xFF and the
// last byte equals the sum of the middle bytes mod 256.
bool verifyFrame(std::span<const uint8_t> frame);

// Compute checksum byte for a payload slice (cmd + len + data bytes).
uint8_t checksum(std::span<const uint8_t> payload);

} // namespace uc1
