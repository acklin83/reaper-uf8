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
    // Top-center V-Pots — Bus Comp 2 mapping
    constexpr uint8_t kBCRatio      = 0x0E;
    constexpr uint8_t kBCMakeup     = 0x0F;
    constexpr uint8_t kBCRelease    = 0x11;
    constexpr uint8_t kBCThreshold  = 0x12;
    constexpr uint8_t kBCMix        = 0x14;
    constexpr uint8_t kBCScHpf      = 0x16;
    // kBCAttack probably 0x10 — not yet directly verified

    // Top-center V-Pots — Channel Strip repurposing when no Bus Comp 2
    constexpr uint8_t kCSInputTrim  = 0x0C;
    constexpr uint8_t kCSFaderLevel = 0x16;  // collides with kBCScHpf by design — repurposes

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

// Gain Reduction meter (Bus Comp center section). 16-bit big-endian value
// in units of 1/10 dB (0 dB = 0x0000, 12.1 dB = 0x0079).
//   FF 5B 02 <hi> <lo> <chk>         (6 bytes)
std::vector<uint8_t> buildGrMeter(float dB);

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
