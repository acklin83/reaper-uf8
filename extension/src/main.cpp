//
// REAPER extension entry point. Glues ColorSync + UF8Device to REAPER's
// API. The visible-track resolution is the one piece that's REAPER-specific
// and deserves comment:
//
//   REAPER exposes the mixer view via TCP/MCP scroll state. Our extension
//   polls on a timer (33 ms) and resolves the 8 "currently visible" tracks
//   in the TCP order. For the first milestone we simply use CountTracks()
//   and show tracks 1..8 regardless of scroll — bank-shift hookup is a
//   follow-up once we confirm the pipe actually drives the hardware.
//

#define REAPERAPI_IMPLEMENT
#include "reaper_plugin.h"
#include "reaper_plugin_functions.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <vector>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>

#ifdef __APPLE__
#include <ApplicationServices/ApplicationServices.h>
#endif

#include "ColorSync.h"
#include "FocusedParam.h"
#include "HidDevice.h"
#include "MidiBridge.h"
#include "MixerWindow.h"
#include "PluginChunkPatch.h"
#include "PluginMap.h"
#include "Protocol.h"
#include "UC1Device.h"
#include "UC1Surface.h"
#include "UF8Device.h"

namespace {

std::unique_ptr<uf8::UF8Device>   g_dev;
std::unique_ptr<uf8::ColorSync>   g_sync;
std::unique_ptr<uf8::MidiBridge>  g_midi;
std::unique_ptr<uf8::HidDevice>   g_hid;

// UC1 — optional. If the device isn't present on the bus we just skip
// it; UF8 continues to work independently. Opening UC1 as a separate
// libusb context keeps the two devices isolated on the bulk-transfer
// side.
std::unique_ptr<uc1::UC1Device>   g_uc1_dev;
std::unique_ptr<uc1::UC1Surface>  g_uc1_surface;

// Plugin Mixer / Settings window (Phase 2.6 + 2.7). Toggled via custom
// action; rendered from onTimer() so REAPER-API reads stay main-thread.
uf8::MixerWindow g_mixerWindow;

// IReaperControlSurface subclass registered as a full control surface
// class ("Rea-Sixty") so users see and add it like any other surface.
// GetTouchState lets Touch-mode automation see the user is holding the
// fader; SetSurfaceVolume is how REAPER notifies us of the current
// track volume during automation playback.
class ReaSixtySurface : public IReaperControlSurface {
public:
    ReaSixtySurface();
    ~ReaSixtySurface() override;

    const char* GetTypeString()   override { return "REASIXTY"; }
    const char* GetDescString()   override { return "Rea-Sixty"; }
    const char* GetConfigString() override { return "0 0"; }

    bool GetTouchState(MediaTrack* tr, int isPan) override;

    // LED feedback. REAPER calls these whenever solo/mute/select/arm
    // state changes for any track. We translate to MCU note-on/off and
    // send to the UF8's MCU-MIDI port so the per-strip LEDs follow.
    void SetSurfaceSolo(MediaTrack* tr, bool solo)        override;
    void SetSurfaceMute(MediaTrack* tr, bool mute)        override;
    void SetSurfaceSelected(MediaTrack* tr, bool sel)     override;
    void SetSurfaceRecArm(MediaTrack* tr, bool arm)       override;
};

// Per-slot MediaTrack cache so GetTouchState can map REAPER's track
// pointer back to a strip index without a linear scan each call. Filled
// by the timer; main-thread-only.
std::array<MediaTrack*, 8> g_slotTrack{};

// Bank offset in 8-strip windows. Bank Left/Right shift by 8; scroll
// wheel (once implemented) will shift by 1. Clamped in [0, max track
// count − 1] so the last bank can end partially empty. Written from
// the USB-input thread on Bank-button press and consumed on the main
// thread by the timer.
std::atomic<int> g_bankOffset{0};

// When the bank offset changes the motor-echo dedup and ColorSync
// dedup both need a full re-push on the next timer tick, since every
// slot now points at a different track. Set by bank-shift, consumed
// by the timer.
std::atomic<bool> g_bankDirty{false};

// Re-render trigger for the timer when the focused-param slot changes.
// The actual focused-param state lives in FocusedParam.h
// (uf8::g_focusedParam, uf8::g_focusedDirty). This flag is the existing
// UF8-side "the labels/values need a forced re-push next tick" signal and
// is set in tandem with g_focusedDirty by anything that mutates the
// focused param. Kept as a separate flag because pushZonesForVisibleSlots
// also fires it on bank shifts, which don't touch g_focusedParam.
std::atomic<bool> g_pageDirty{false};

// When the user hits the PAN button, we globally override every strip's
// V-Pot to act as pan control regardless of whether the track hosts an
// SSL plug-in. Any V-Pot assignment soft key (0x68–0x6D) returns to
// automatic plug-in-param mode. Same invalidation path as a page change.
std::atomic<bool> g_forcePan{false};

// FLIP mode (button 0x54): swap fader and V-Pot. Fader drives the
// focused plug-in parameter; V-Pot drives track volume. Display zones
// follow the swap so the user sees param value above the fader and
// "Vol  -X.YdB" in the Value Line. Persisted across REAPER sessions.
std::atomic<bool> g_flip{false};

// Plugin-fader-mode toggle. Press of the global Plugin button (0x50)
// flips this. When true, UF8 faders should drive plug-in faders directly
// instead of REAPER track volume. Routing wireup TBD; this state +
// the Plugin LED feedback land in this commit.
std::atomic<bool> g_pluginFaderMode{false};

// V-Pot has dedicated Pan UX (Plugin button → plug-in Pan; PAN button →
// REAPER track pan; default → REAPER track pan). Clicking the Pan knob
// in the SSL plug-in GUI fires GetLastTouchedFX, which chaseLastTouchedFx
// turns into setFocus({ChannelStrip, 3}) — and that focus persists across
// Plugin/PAN button toggles, hijacking the V-Pot's Pan-mode tree. Fader
// + V-Pot drive/display sites treat Pan-focus as "no focus" so the
// Plugin/forcePan/REAPER tree retains authority over Pan.
inline bool isVPotPanFocus(const uf8::FocusedParam& f) {
    return f.domain == uf8::Domain::ChannelStrip && f.slotIdx == 3;
}

// Soft-Key Bank — which page of params is currently shown across the 8
// top-soft-key labels (and selectable via the per-strip 0x18..0x1F
// keys). Range is 0..5 (V-POT + Bank 1..5) for both domains; in BC
// mode banks 2..5 are present-but-empty per SSL UF8 User Guide.
// Layout from SSL UF8 User Guide p.180-181. Persisted across sessions.
std::atomic<int>  g_softKeyBank{0};
std::atomic<bool> g_softKeyDirty{false};

namespace softkey {
    constexpr int kNoSlot = -1;
    constexpr size_t kStrips = 8;

    // CS-mode banks (6 × 8). Values are SSL 360 Link slot indices
    // (linkIdx); uf8::ext::* refers to extension-defined synthetic IDs in
    // PluginMap.h for params not in the SSL 360 Link table.
    //   - Phase / A/B / HQ Mode: synthetic linkIdx — soft-key press sets
    //     focus to the synthetic, render path reads state per-strip
    //     (REAPER B_PHASE for Phase; SSL plug-in chunk for A/B + HQ),
    //     V-Pot push triggers the per-strip toggle. Same UX as any
    //     other CS param.
    //   - Pre / Mic-Drive / Imp In / Imp: only on 4K-series; CS2 strips
    //     render blank when soft-key pressed (graceful no-op).
    constexpr int kCsBanks[6][kStrips] = {
        // V-POT: BYPASS, IN TRIM, Ø, PRE, MIC/DRIVE, _, IMPEDANCE IN, IMPEDANCE
        // BYPASS uses linkIdx 0 — the plug-in's own Bypass param (NOT
        // REAPER's TrackFX_Enabled).
        { 0,  4, uf8::ext::TrackPhase, uf8::ext::Pre, uf8::ext::MicDrive, kNoSlot, uf8::ext::ImpedanceIn, uf8::ext::Impedance },
        // Bank 1: WIDTH, _, _, A/B, HIGH PASS, LOW PASS, EQ, EQ TYPE
        { 2, kNoSlot, kNoSlot, uf8::ext::PluginAB,  7,  6, 15, 14 },
        // Bank 2: LF FREQ, LF GAIN, LF TYPE, _, LMF FREQ, LMF GAIN, LMF Q, _
        { 19, 20, 21, kNoSlot, 17, 16, 18, kNoSlot },
        // Bank 3: HMF FREQ, HMF GAIN, HMF Q, _, _, HF FREQ, HF GAIN, HF TYPE
        { 12, 11, 13, kNoSlot, kNoSlot, 10,  9,  8 },
        // Bank 4: DYNAMICS, COMP MIX, COMP RATIO, COMP THR, COMP REL, COMP ATK, PEAK/RMS, _
        { 22, 23, 26, 27, 28, 24, 25, kNoSlot },
        // Bank 5: GATE REL, GATE THR, GATE RNG, GATE HLD, GATE ATK, GATE/EXP, HQ MODE, OUT TRIM
        { 31, 30, 29, 32, 34, 33, uf8::ext::PluginHQ, 37 },
    };
    constexpr const char* kCsLabels[6][kStrips] = {
        { "BYPASS",  "IN TRIM",   "PHASE",      "PRE",      "MIC/DRV",   "",          "IMP IN",  "IMP" },
        { "WIDTH",   "",          "",           "A/B",      "HPF",       "LPF",       "EQ",      "EQ TYPE" },
        { "LF FREQ", "LF GAIN",   "LF TYPE",    "",         "LMF FREQ",  "LMF GAIN",  "LMF Q",   "" },
        { "HMF FREQ","HMF GAIN",  "HMF Q",      "",         "",          "HF FREQ",   "HF GAIN", "HF TYPE" },
        { "DYNAMICS","COMP MIX",  "COMP RATIO", "COMP THR", "COMP REL",  "COMP F.ATK","PEAK/RMS","" },
        { "GATE REL","GATE THR",  "GATE RANGE", "GATE HOLD","GATE F.ATK","GATE/EXP",  "HQ MODE", "OUT TRIM" },
    };

    // BC-mode banks (6 × 8). Banks 2..5 are intentionally empty per SSL
    // UF8 User Guide — bank navigation cycles through them symmetrically
    // with CS mode even though BC has only two pages of params.
    constexpr int kBcBanks[6][kStrips] = {
        // V-POT: THRESHOLD, ATTACK, RELEASE, RATIO, S/C HPF, MIX, EXTERNAL S/C, BUS COMP
        // BUS COMP at pos 7 = the BC plug-in's own CompBypass param
        // (linkIdx 0 in the BC 360 Link layout). External S/C uses
        // uf8::ext::ExternalSC — only the Native BC2 plug-in exposes it (the
        // 360 Link wrapper does not), so other BC variants will no-op.
        { 1, 3, 4, 5, 6, 7, uf8::ext::ExternalSC, 0 },
        // Bank 1: OUTPUT GAIN (= MakeupGain in BC2 map), rest empty
        { 2, kNoSlot, kNoSlot, kNoSlot, kNoSlot, kNoSlot, kNoSlot, kNoSlot },
        // Banks 2..5: empty per SSL spec
        { kNoSlot, kNoSlot, kNoSlot, kNoSlot, kNoSlot, kNoSlot, kNoSlot, kNoSlot },
        { kNoSlot, kNoSlot, kNoSlot, kNoSlot, kNoSlot, kNoSlot, kNoSlot, kNoSlot },
        { kNoSlot, kNoSlot, kNoSlot, kNoSlot, kNoSlot, kNoSlot, kNoSlot, kNoSlot },
        { kNoSlot, kNoSlot, kNoSlot, kNoSlot, kNoSlot, kNoSlot, kNoSlot, kNoSlot },
    };
    constexpr const char* kBcLabels[6][kStrips] = {
        { "THR",    "ATTACK", "RELEASE","RATIO", "S/C HPF","MIX",  "EXT S/C","BUS COMP" },
        { "OUTGAIN","",       "",       "",      "",       "",     "",       "" },
        { "",       "",       "",       "",      "",       "",     "",       "" },
        { "",       "",       "",       "",      "",       "",     "",       "" },
        { "",       "",       "",       "",      "",       "",     "",       "" },
        { "",       "",       "",       "",      "",       "",     "",       "" },
    };

    constexpr int kCsMaxBank = 5;
    constexpr int kBcMaxBank = 5;

    // Domain-aware bank max so the bank index can be clamped after a
    // domain switch (BC has fewer banks than CS).
    inline int maxBankFor(uf8::Domain d) {
        return d == uf8::Domain::BusComp ? kBcMaxBank : kCsMaxBank;
    }

    // Resolve the current 8-slot view (linkIdx + label arrays) for a
    // given domain + bank. Caller clamps bank to maxBankFor(domain).
    struct View {
        const int*        linkIdx;
        const char* const* labels;
    };
    inline View viewFor(uf8::Domain d, int bank) {
        if (d == uf8::Domain::BusComp) {
            return { kBcBanks[bank], kBcLabels[bank] };
        }
        return { kCsBanks[bank], kCsLabels[bank] };
    }

    // Find which bank in `domain` contains a given linkIdx. -1 if not
    // found. Used to auto-follow the bank to externally-set focus
    // (UC1 knob, plugin GUI mouse, FocusedFX chase).
    inline int bankContaining(uf8::Domain d, int linkIdx) {
        const int max = maxBankFor(d);
        for (int b = 0; b <= max; ++b) {
            const View v = viewFor(d, b);
            for (size_t s = 0; s < kStrips; ++s) {
                if (v.linkIdx[s] == linkIdx) return b;
            }
        }
        return -1;
    }
}

// Per-strip cache for the SEL-follows-DAW-Colour LED state. Avoids
// sending FF 38/39 every tick — only on actual changes. Value encodes
// 0xFF for "bright" / 0x00 for "dim" / 0xFE as "unset".
uint8_t g_lastSelBright[8] = {0xFE, 0xFE, 0xFE, 0xFE, 0xFE, 0xFE, 0xFE, 0xFE};
uint16_t g_lastSelMask = 0xFFFF;  // 0xFFFF = "unset" sentinel

// ---- Brightness management (LED + LCD on both devices) ----
//
// SSL 360° exposes 5 brightness steps ("dark / dim / half / bright /
// full"). Until the Phase 2 settings UI is built we expose two REAPER
// custom actions that cycle up/down through those steps, persisted to
// ExtState so the user's choice survives REAPER restarts.
enum BrightnessLevel {
    BL_Dark   = 0,
    BL_Dim    = 1,
    BL_Half   = 2,
    BL_Bright = 3,
    BL_Full   = 4,
};
std::atomic<int> g_brightness{BL_Full};

struct BrightnessBytes {
    uint8_t uf8_led; uint8_t uf8_lcd;
    uint8_t uc1_led; uint8_t uc1_lcd; uint8_t uc1_status;
};
constexpr BrightnessBytes kBrightnessTable[5] = {
    {0x05, 0x18, 0x0A, 0x18, 0x08},  // dark
    {0x0A, 0x30, 0x13, 0x30, 0x0F},  // dim
    {0x10, 0x50, 0x20, 0x50, 0x19},  // half
    {0x13, 0x60, 0x26, 0x60, 0x1E},  // bright
    {0x20, 0xA0, 0x40, 0xA0, 0x32},  // full
};

void pushBrightness(int level)
{
    if (level < 0) level = 0;
    if (level > 4) level = 4;
    const auto& b = kBrightnessTable[level];
    if (g_dev && g_dev->isOpen()) {
        g_dev->send(uf8::buildLedBrightness(b.uf8_led));
        g_dev->send(uf8::buildLcdBrightness(b.uf8_lcd));
    }
    if (g_uc1_dev && g_uc1_dev->isOpen()) {
        g_uc1_dev->send(uc1::buildLedBrightness(b.uc1_led));
        g_uc1_dev->send(uc1::buildLcdBrightness(b.uc1_lcd));
        g_uc1_dev->send(uc1::buildStatusBrightness(b.uc1_status));
    }
}

void applyBrightness()
{
    const int lvl = g_brightness.load();
    pushBrightness(lvl);
    char buf[8];
    std::snprintf(buf, sizeof(buf), "%d", lvl);
    SetExtState("rea_sixty", "brightness", buf, true);  // persist=true
}

void loadBrightness()
{
    const char* s = GetExtState("rea_sixty", "brightness");
    if (s && *s) {
        int lvl = std::atoi(s);
        if (lvl < 0) lvl = 0;
        if (lvl > 4) lvl = 4;
        g_brightness.store(lvl);
    }
}

bool brightnessUp()
{
    int cur = g_brightness.load();
    if (cur >= BL_Full) return false;
    g_brightness.store(cur + 1);
    applyBrightness();
    return true;
}

bool brightnessDown()
{
    int cur = g_brightness.load();
    if (cur <= BL_Dark) return false;
    g_brightness.store(cur - 1);
    applyBrightness();
    return true;
}

// Read the "UI" volume for a track — reflects the same value the REAPER
// mixer displays, including automation playback and the effect of a
// just-applied CSurf_OnVolumeChange. Safer than GetMediaTrackInfo_Value
// for motor-echo purposes. Same pattern CSI uses.
double uiVolLinear(MediaTrack* tr)
{
    double vol = 1.0;
    double pan = 0.0;
    GetTrackUIVolPan(tr, &vol, &pan);
    return vol;
}

// REAPER's C API expects calls on the main thread. The UF8 input handler
// runs on libusb's transfer-callback thread, so per-strip input events
// (buttons / fader / v-pot) are pushed into this queue and drained in
// onTimer(), which REAPER invokes on the main thread at ~30 Hz.
// Forward decls — full definitions live near the LCD helpers below;
// PanDelta/PanCenter handlers consume them earlier in the file.
bool isBinarySlot(const uf8::LinkSlot& s);
bool isBipolarSlot(const uf8::LinkSlot& s);

struct PendingInput {
    enum Kind : uint8_t {
        SoloToggle,
        MuteToggle,
        SelectToggle,    // additive (Shift held) — toggles selection on this track
        SelectExclusive, // no modifier — selects only this track
        VolumeAbs,       // value = linear volume (1.0 == 0 dB)
        PanDelta,        // value = signed pan delta (−1..+1 is full sweep)
        PanCenter,       // reset pan to 0 (center)
        SelectRelative,  // value = signed track-index delta (channel encoder, Nav mode)
        PlayheadNudge,   // value = signed seconds delta (channel encoder, Nudge mode)
        MouseScroll,     // value = signed scroll delta (channel encoder, Focus mode)
        MainAction,      // value = REAPER action ID (Main_OnCommand)
        AutomationMode,  // value = REAPER automation mode (0..4) on selected track
        FocusSelected,   // re-scroll REAPER MCP + UF8 bank to currently selected track
    };
    Kind    kind;
    uint8_t strip;
    double  value;
};

// Encoder-mode state. Default = Nav (= track select) matching the UF8's
// out-of-box feel. Toggled by the NAV (0x73), NUDGE (0x74), FOCUS (0x75)
// buttons and pushed back to Nav by the channel-encoder push (0x76).
enum class EncoderMode : uint8_t { Nav, Nudge, Focus };
std::atomic<EncoderMode> g_encoderMode{EncoderMode::Nav};

// Nudge step per physical detent (seconds). User-settings-facing later;
// hard-coded for now.
constexpr double kNudgeSecondsPerStep = 1.0;

// Follow mode: when a track becomes selected, either snap the UF8 bank
// to the 8-wide bucket that contains it (BucketSnap) or make the
// selected track always the leftmost strip (LeftmostStrip). Same
// setting applies to REAPER MCP scroll. User-settings-facing later.
enum class FollowMode : uint8_t { BucketSnap, LeftmostStrip };
std::atomic<FollowMode> g_followMode{FollowMode::BucketSnap};

// Global modifier-key state, set from the USB-input thread, read from the
// main thread in drainInputQueue(). Atomic read/write is sufficient since
// each modifier is a single bool.
std::atomic<bool> g_shiftHeld{false};

// Fractional accumulators for the channel encoder. The UF8 emits
// several events per physical detent (each with speed 1-2), so we
// divide by the scale and only consume whole integer steps. Separate
// accumulators per mode so mode switches don't bleed fractional state
// across. Main-thread only.
double g_selectAccum = 0.0;
double g_nudgeAccum  = 0.0;
constexpr double kChannelEncoderScale = 4.0;

// When set, the Channel-Encoder Select also shifts the UF8 bank and the
// REAPER MCP scroll so the newly-selected track stays visible. Planned
// as a user-facing settings option (see memory backlog); currently
// hard-coded ON because the mixer-follow behaviour is what makes the
// wheel feel right in every test session.
constexpr bool kSelectFollowsMixer = true;

void followSelectedInMixer(MediaTrack* tr)
{
    if (!kSelectFollowsMixer || !tr) return;

    // REAPER MCP: scroll so the selected track becomes leftmost (or
    // stays within the visible range if REAPER decides to keep context).
    SetMixerScroll(tr);

    const int trackCount = CountTracks(nullptr);
    int idx = -1;
    for (int t = 0; t < trackCount; ++t) {
        if (GetTrack(nullptr, t) == tr) { idx = t; break; }
    }
    if (idx < 0) return;

    int bank = g_bankOffset.load();
    if (g_followMode.load() == FollowMode::LeftmostStrip) {
        // Selected track always at strip 0. Clamp so we don't scroll
        // past the last track.
        bank = idx;
        if (bank > trackCount - 1) bank = trackCount > 0 ? trackCount - 1 : 0;
    } else {
        // Bucket snap: only shift if the selection fell outside the
        // current 8-wide window.
        if (idx < bank || idx >= bank + 8) bank = (idx / 8) * 8;
    }
    if (bank != g_bankOffset.exchange(bank)) g_bankDirty.store(true);
}

// Synthesise a mouse-wheel scroll event at the cursor's current screen
// position so the "Focus" encoder mode emulates a real scroll wheel —
// hover the mouse over a plug-in parameter and spin the encoder.
#ifdef __APPLE__
void emitMouseScroll(int32_t delta)
{
    CGEventRef ev = CGEventCreateScrollWheelEvent(
        nullptr, kCGScrollEventUnitLine, 1, delta);
    if (!ev) return;
    CGEventPost(kCGHIDEventTap, ev);
    CFRelease(ev);
}
#else
void emitMouseScroll(int32_t) {}   // non-macOS: TBD
#endif
std::mutex                  g_inQueueMutex;
std::vector<PendingInput>   g_inQueue;

// Forward declarations for helpers defined later in the file but
// referenced from drainInputQueue's FLIP-mode path.
uint16_t linearVolumeToPb(double linear);
double   pbToLinearVolume(uint16_t pb14);
double   uiVolLinear(MediaTrack* tr);

// Hardware fader top — the actual highest pb14 the UF8 firmware emits.
// Defined here so drainInputQueue can normalise FLIP-mode fader values
// against the real hardware range, not the 14-bit protocol max (16383).
// Full rationale + calibration in the kUf8FaderTopDb / pbToLinearVolume
// block further down. Kept in sync with that block.
constexpr uint16_t kUf8FaderPbMax = 15583;

// CS plug-in's "Fader Level" param (= the SSL strip's own internal
// fader, distinct from REAPER's track volume). Used when the Plugin
// button is engaged so the UF8 motor faders drive the SSL strip
// fader instead of post-effect track volume. The vst3 param index
// differs per CS variant — looked up by the plug-in's display short
// name. Returns {-1, -1} when the track has no CS plug-in.
struct CsFaderHandle { int fxIndex; int vst3Param; };
CsFaderHandle csFaderForTrack(MediaTrack* tr)
{
    if (!tr) return { -1, -1 };
    auto mm = uf8::lookupPluginOnTrack(tr, uf8::Domain::ChannelStrip);
    if (!mm.map) return { -1, -1 };
    int p = -1;
    const char* sn = mm.map->displayShort;
    if      (std::strcmp(sn, "CS 2") == 0) p = 38;
    else if (std::strcmp(sn, "4K G") == 0) p = 12;
    else if (std::strcmp(sn, "4K E") == 0) p = 6;
    else if (std::strcmp(sn, "4K B") == 0) p = 6;
    return { mm.fxIndex, p };
}

// CS plug-in's Pan param (linkIdx 3 across all four CS variants). In
// Plugin mode, the V-Pot's Pan-fallback drives this instead of REAPER's
// track pan, so the SSL strip Pan stays the surface's source-of-truth.
struct CsPanHandle { int fxIndex; int vst3Param; };
CsPanHandle csPanForTrack(MediaTrack* tr)
{
    if (!tr) return { -1, -1 };
    auto mm = uf8::lookupPluginOnTrack(tr, uf8::Domain::ChannelStrip);
    if (!mm.map) return { -1, -1 };
    const auto* sl = uf8::findSlotByLinkIdx(*mm.map, 3);
    if (!sl) return { -1, -1 };
    return { mm.fxIndex, sl->vst3Param };
}

void queueInput(PendingInput e)
{
    std::lock_guard<std::mutex> lk(g_inQueueMutex);
    // Coalesce volume updates: only the latest absolute position matters,
    // so collapse runs of fader events into a single pending entry per strip.
    if (e.kind == PendingInput::VolumeAbs) {
        for (auto& p : g_inQueue) {
            if (p.kind == PendingInput::VolumeAbs && p.strip == e.strip) {
                p.value = e.value;
                return;
            }
        }
    }
    g_inQueue.push_back(e);
}

void drainInputQueue()
{
    std::vector<PendingInput> local;
    {
        std::lock_guard<std::mutex> lk(g_inQueueMutex);
        local.swap(g_inQueue);
    }
    const int trackCount = CountTracks(nullptr);
    const int bankOffset = g_bankOffset.load();
    for (const auto& e : local) {
        // Global-scope events (no strip) are dispatched before the
        // per-strip track resolution below.
        if (e.kind == PendingInput::MainAction) {
            Main_OnCommand(static_cast<int>(e.value), 0);
            continue;
        }
        if (e.kind == PendingInput::AutomationMode) {
            if (MediaTrack* tr = GetSelectedTrack(nullptr, 0)) {
                SetTrackAutomationMode(tr, static_cast<int>(e.value));
            }
            continue;
        }
        if (e.kind == PendingInput::FocusSelected) {
            if (MediaTrack* tr = GetSelectedTrack(nullptr, 0)) {
                followSelectedInMixer(tr);
            }
            continue;
        }
        if (e.kind == PendingInput::PlayheadNudge) {
            g_nudgeAccum += e.value / kChannelEncoderScale;
            int step = 0;
            if (g_nudgeAccum >=  1.0) { step = static_cast<int>(g_nudgeAccum); g_nudgeAccum -= step; }
            if (g_nudgeAccum <= -1.0) { step = static_cast<int>(g_nudgeAccum); g_nudgeAccum -= step; }
            if (step != 0) {
                const double cur = GetCursorPosition();
                SetEditCurPos(cur + step * kNudgeSecondsPerStep, true, false);
            }
            continue;
        }
        if (e.kind == PendingInput::MouseScroll) {
            // Mouse-wheel feels natural with per-event events: high rate
            // produces fast scroll, low rate slow. No accumulation.
            emitMouseScroll(static_cast<int32_t>(e.value));
            continue;
        }
        if (e.kind == PendingInput::SelectRelative) {
            if (trackCount == 0) continue;
            // Accumulate scaled fractional deltas — the UF8 emits
            // multiple events per physical detent, so we divide each
            // per-event magnitude and only consume whole-track steps.
            g_selectAccum += e.value / kChannelEncoderScale;
            int step = 0;
            if (g_selectAccum >=  1.0) { step = static_cast<int>(g_selectAccum);        g_selectAccum -= step; }
            if (g_selectAccum <= -1.0) { step = static_cast<int>(g_selectAccum);        g_selectAccum -= step; }
            if (step == 0) continue;

            int cur = -1;
            for (int t = 0; t < trackCount; ++t) {
                if (GetMediaTrackInfo_Value(GetTrack(nullptr, t), "I_SELECTED") > 0.5) {
                    cur = t;
                    break;
                }
            }
            int next = (cur >= 0 ? cur : 0) + step;
            if (next < 0) next = 0;
            if (next > trackCount - 1) next = trackCount - 1;
            if (MediaTrack* tr = GetTrack(nullptr, next)) {
                SetOnlyTrackSelected(tr);
                followSelectedInMixer(tr);
            }
            continue;
        }

        const int slot = e.strip + bankOffset;
        if (slot >= trackCount) continue;
        MediaTrack* tr = GetTrack(nullptr, slot);
        if (!tr) continue;
        switch (e.kind) {
            case PendingInput::SoloToggle:     CSurf_OnSoloChange(tr, -1); break;
            case PendingInput::MuteToggle:     CSurf_OnMuteChange(tr, -1); break;
            case PendingInput::SelectToggle:   CSurf_OnSelectedChange(tr, -1); break;
            case PendingInput::SelectExclusive:
                SetOnlyTrackSelected(tr);
                followSelectedInMixer(tr);
                break;
            case PendingInput::VolumeAbs: {
                // FLIP: fader drives the focused plug-in parameter on this
                // strip's track instead of track volume. Read the raw
                // pb14 straight from the touch buffer — bypass the volume
                // calibration (generic params want a clean 0..1 sweep,
                // not REAPER's slider law) and divide by kUf8FaderPbMax
                // (15583, the actual hardware top — see fader-top probe
                // 2026-04-29) so norm reaches 1.0 at mechanical top.
                // 16383 here would cap norm at 0.951, leaving e.g. Pan
                // 5% short of full R.
                const auto focusedF = uf8::getFocusedParam();
                auto mmF = uf8::lookupPluginOnTrack(tr, focusedF.domain);
                const bool forcePanF = g_forcePan.load();
                const uf8::LinkSlot* slF = (!forcePanF && mmF.map)
                    ? uf8::findSlotByLinkIdx(*mmF.map, focusedF.slotIdx)
                    : nullptr;
                // Pan-focus is owned by Plugin/PAN buttons — don't let it
                // hijack the fader either (FLIP+Pan would conflict with
                // Plugin-fader mode's fader→CS-Fader routing).
                if (isVPotPanFocus(focusedF)) slF = nullptr;
                if (g_flip.load() && slF) {
                    const uint16_t pbF = linearVolumeToPb(e.value);
                    double normF = static_cast<double>(pbF) /
                                   static_cast<double>(kUf8FaderPbMax);
                    if (slF->inverted) normF = 1.0 - normF;
                    if (normF < 0.0) normF = 0.0;
                    if (normF > 1.0) normF = 1.0;
                    TrackFX_SetParamNormalized(tr, mmF.fxIndex,
                        slF->vst3Param, normF);
                    break;
                }
                // Plugin-fader mode: route the fader to the SSL strip's
                // internal Fader Level param (vst3 index varies per
                // variant) instead of REAPER's post-FX track volume.
                // Same pb14/kUf8FaderPbMax mapping the FLIP path uses
                // — gives an even 0..1 sweep the plug-in expects.
                if (g_pluginFaderMode.load()) {
                    const auto cs = csFaderForTrack(tr);
                    if (cs.vst3Param >= 0) {
                        const uint16_t pbCs = linearVolumeToPb(e.value);
                        double n = static_cast<double>(pbCs) /
                                   static_cast<double>(kUf8FaderPbMax);
                        if (n < 0.0) n = 0.0;
                        if (n > 1.0) n = 1.0;
                        TrackFX_SetParamNormalized(tr, cs.fxIndex,
                            cs.vst3Param, n);
                        break;
                    }
                    // No CS plug-in on this track — fall through to track
                    // volume so the fader still does *something*.
                }
                // Normal: CSurf_OnVolumeChange applies the new position to
                // the track AND broadcasts to other surfaces. We do not
                // cache the value — motor echo reads GetTrackUIVolPan
                // on each tick so it always reflects whatever REAPER
                // actually has (including envelope playback).
                CSurf_OnVolumeChange(tr, e.value, false);
                break;
            }
            case PendingInput::PanDelta: {
                // V-pot rotation: if the strip's track hosts a plug-in of
                // the focused domain (CS / BC) AND we're not in global Pan
                // mode, drive the focused parameter on that strip's track.
                // Otherwise fall back to track pan.
                //
                // FLIP exception: with a slot present, the V-Pot drives
                // track volume in pb14 space instead of the param (the
                // fader has taken over the param).
                const auto focused = uf8::getFocusedParam();
                // Synthetic toggles ignore rotation — push-only per user
                // instruction (no continuous value to scrub).
                if (focused.domain == uf8::Domain::ChannelStrip
                    && (focused.slotIdx == uf8::ext::TrackPhase
                        || focused.slotIdx == uf8::ext::PluginAB
                        || focused.slotIdx == uf8::ext::PluginHQ)) {
                    break;
                }
                auto mm = uf8::lookupPluginOnTrack(tr, focused.domain);
                const bool forcePan = g_forcePan.load();
                const uf8::LinkSlot* slPtr = (!forcePan && mm.map)
                    ? uf8::findSlotByLinkIdx(*mm.map, focused.slotIdx)
                    : nullptr;
                // Pan-focus ignored for V-Pot drive (Plugin/PAN buttons own
                // the Pan-mode tree). FLIP is also bypassed — flipping Pan
                // onto the fader would conflict with Plugin-fader mode.
                if (isVPotPanFocus(focused)) slPtr = nullptr;
                if (g_flip.load() && slPtr) {
                    // Map detent fraction (signed6/128) to pb14 delta —
                    // single detent ≈ 128 pb (1/128 of full sweep, same
                    // feel as the V-Pot driving the param). Fine quarters.
                    const uint16_t curPb = linearVolumeToPb(uiVolLinear(tr));
                    double dPb = e.value * 16383.0;
                    if (g_shiftHeld.load()) dPb *= 0.25;
                    int newPb = static_cast<int>(std::round(
                        static_cast<double>(curPb) + dPb));
                    if (newPb < 0) newPb = 0;
                    if (newPb > 16383) newPb = 16383;
                    const double newLin = pbToLinearVolume(
                        static_cast<uint16_t>(newPb));
                    CSurf_OnVolumeChange(tr, newLin, false);
                    break;
                }
                if (slPtr) {
                    const uf8::LinkSlot& sl = *slPtr;
                    const double cur = TrackFX_GetParamNormalized(tr, mm.fxIndex,
                                                                  sl.vst3Param);
                    // e.value is pan-scaled (≈1/128 per event). The
                    // earlier 4× multiplier (32-detent full sweep) was
                    // too coarse — values jumped past target. Drop to
                    // 1× for a 128-detent sweep, matching SSL's V-Pot
                    // feel in 360°. Fine mode (Shift) quarters.
                    double delta = e.value * (sl.inverted ? -1.0 : 1.0);
                    if (g_shiftHeld.load()) delta *= 0.25;
                    double next = cur + delta;
                    if (next < 0.0) next = 0.0;
                    if (next > 1.0) next = 1.0;
                    TrackFX_SetParamNormalized(tr, mm.fxIndex, sl.vst3Param, next);
                } else if (g_pluginFaderMode.load() && !forcePan) {
                    // Plugin mode + no focused slot → V-Pot drives the
                    // SSL strip's own Pan param (linkIdx 3) instead of
                    // REAPER track pan, so the plug-in remains the
                    // surface's source of truth for the strip's panorama.
                    const auto pn = csPanForTrack(tr);
                    if (pn.vst3Param >= 0) {
                        const double cur = TrackFX_GetParamNormalized(
                            tr, pn.fxIndex, pn.vst3Param);
                        double delta = e.value * 0.5;  // pan range 0..1, half-scale of REAPER's -1..+1
                        if (g_shiftHeld.load()) delta *= 0.25;
                        double next = cur + delta;
                        if (next < 0.0) next = 0.0;
                        if (next > 1.0) next = 1.0;
                        TrackFX_SetParamNormalized(tr, pn.fxIndex,
                            pn.vst3Param, next);
                        break;
                    }
                    // Fall through to REAPER pan if no CS plug-in.
                    const double cur = GetMediaTrackInfo_Value(tr, "D_PAN");
                    double next = cur + e.value;
                    if (next >  1.0) next =  1.0;
                    if (next < -1.0) next = -1.0;
                    SetMediaTrackInfo_Value(tr, "D_PAN", next);
                } else {
                    const double cur = GetMediaTrackInfo_Value(tr, "D_PAN");
                    double next = cur + e.value;
                    if (next >  1.0) next =  1.0;
                    if (next < -1.0) next = -1.0;
                    SetMediaTrackInfo_Value(tr, "D_PAN", next);
                }
                break;
            }
            case PendingInput::PanCenter: {
                // V-pot push: with a plug-in of the focused domain present
                // (and not in global Pan mode), reset the focused param to
                // its midpoint. Otherwise, re-center pan.
                //
                // FLIP exception: with a slot present, the V-Pot push
                // resets track volume to 0 dB (linear 1.0) — the V-Pot
                // is driving volume.
                const auto focused = uf8::getFocusedParam();
                // Synthetic toggles (Phase / A/B / HQ) are not VST3 params
                // — handled directly here on the strip's track. Push
                // toggles, rotation is ignored.
                if (focused.domain == uf8::Domain::ChannelStrip) {
                    if (focused.slotIdx == uf8::ext::TrackPhase) {
                        const double cur = GetMediaTrackInfo_Value(tr, "B_PHASE");
                        SetMediaTrackInfo_Value(tr, "B_PHASE", cur > 0.5 ? 0.0 : 1.0);
                        break;
                    }
                    if (focused.slotIdx == uf8::ext::PluginAB) {
                        uf8::togglePluginAB(tr);
                        break;
                    }
                    if (focused.slotIdx == uf8::ext::PluginHQ) {
                        uf8::togglePluginHQ(tr);
                        break;
                    }
                }
                auto mm = uf8::lookupPluginOnTrack(tr, focused.domain);
                const bool forcePan = g_forcePan.load();
                const uf8::LinkSlot* slPtr = (!forcePan && mm.map)
                    ? uf8::findSlotByLinkIdx(*mm.map, focused.slotIdx)
                    : nullptr;
                // Pan-focus ignored for V-Pot push (Plugin/PAN buttons own
                // the Pan-mode tree).
                if (isVPotPanFocus(focused)) slPtr = nullptr;
                if (g_flip.load() && slPtr) {
                    CSurf_OnVolumeChange(tr, 1.0, false);
                    break;
                }
                if (slPtr) {
                    if (isBinarySlot(*slPtr)) {
                        // V-Pot push cycles to next discrete step. For a
                        // 2-state toggle (EQ In, Dyn In, S/C Listen) this
                        // collapses to 0↔1. For a multi-step enumeration
                        // (4K G EQ Colour: Black/Pink — VST3 reports 2
                        // steps but plain 0↔1 was hitting an unused 3rd
                        // value, hence the user's "3 push" complaint),
                        // step-size cycling lands on each defined value.
                        double step = 0.0, smallstep = 0.0, largestep = 0.0;
                        bool istoggle = false;
                        const bool haveSteps = TrackFX_GetParameterStepSizes(
                            tr, mm.fxIndex, slPtr->vst3Param,
                            &step, &smallstep, &largestep, &istoggle);
                        const double cur = TrackFX_GetParamNormalized(
                            tr, mm.fxIndex, slPtr->vst3Param);
                        double next;
                        if (!haveSteps || istoggle || step <= 0.0 || step >= 1.0) {
                            next = (cur < 0.5) ? 1.0 : 0.0;
                        } else {
                            next = cur + step;
                            if (next > 1.0 + step * 0.5) next = 0.0;
                            if (next > 1.0) next = 1.0;
                        }
                        TrackFX_SetParamNormalized(tr, mm.fxIndex,
                            slPtr->vst3Param, next);
                    } else {
                        const double resetVal = slPtr->deflt.value_or(0.5);
                        TrackFX_SetParamNormalized(tr, mm.fxIndex,
                            slPtr->vst3Param, resetVal);
                    }
                } else if (g_pluginFaderMode.load() && !forcePan) {
                    // Plugin mode → reset SSL strip's own Pan to centre
                    // (norm 0.5 = C). forcePan overrides this so PAN
                    // button always resets REAPER track pan instead.
                    const auto pn = csPanForTrack(tr);
                    if (pn.vst3Param >= 0) {
                        TrackFX_SetParamNormalized(tr, pn.fxIndex,
                            pn.vst3Param, 0.5);
                    } else {
                        SetMediaTrackInfo_Value(tr, "D_PAN", 0.0);
                    }
                } else {
                    SetMediaTrackInfo_Value(tr, "D_PAN", 0.0);
                }
                break;
            }
        }
    }
}

// UF8 fader calibration. Two layers:
//
//   1. Hardware facts (probed 2026-04-29): the fader emits pb14 ≈ 15583
//      at mechanical top, not the protocol max 16383 — there's a ~5%
//      deadband at the top of travel.
//
//   2. Curve mismatch: REAPER's default slider law is not the same as
//      UF8's printed scale. Even after stretching (1) to put +12 at
//      mechanical top, intermediate marks land 2–14 dB hot (e.g. UF8 "0"
//      read +4.07, UF8 "-30" read -21). The user's calibration table
//      below maps from REAPER-current-reading → UF8-printed dB so the
//      printed marks line up with REAPER's volume display.
constexpr double   kUf8FaderTopDb  = 12.0;
// kUf8FaderPbMax is forward-declared earlier (used by drainInputQueue).

// Calibration sample: with kUf8FaderTopDb=12 and kUf8FaderPbMax=15583
// the bare slider-law mapping placed each UF8 mark at this much in
// REAPER. We piecewise-linear interpolate to push the printed marks
// onto their stated dB values.
//
// Sorted descending in current_db so the lookup walks from the top.
struct FaderCalPoint { double current_db; double target_db; };
constexpr FaderCalPoint kFaderCal[] = {
    {  +12.00,  +12.0 },   // mechanical top (slider already at top)
    {   +8.40,   +6.0 },
    {   +4.07,    0.0 },
    {   +0.74,   -5.0 },
    {   -5.65,  -10.0 },
    {  -13.70,  -20.0 },
    {  -21.00,  -30.0 },
    {  -32.00,  -40.0 },
    {  -46.00,  -60.0 },
    { -150.00, -150.0 },   // silence floor
};

double interpFaderCal(double x, bool current_to_target)
{
    const auto& tab = kFaderCal;
    constexpr size_t n = std::size(kFaderCal);

    auto getX = [&](size_t i) {
        return current_to_target ? tab[i].current_db : tab[i].target_db;
    };
    auto getY = [&](size_t i) {
        return current_to_target ? tab[i].target_db  : tab[i].current_db;
    };

    if (x >= getX(0))     return getY(0);
    if (x <= getX(n - 1)) return getY(n - 1);
    for (size_t i = 1; i < n; ++i) {
        if (x >= getX(i)) {
            const double t = (x - getX(i)) / (getX(i - 1) - getX(i));
            return getY(i) + t * (getY(i - 1) - getY(i));
        }
    }
    return getY(n - 1);
}

// Convert a 14-bit pb value to linear REAPER volume. Two stages:
//   pb14 → REAPER's slider-law dB (the "raw" reading) → calibrated dB.
double pbToLinearVolume(uint16_t pb14)
{
    if (pb14 == 0) return 0.0;
    if (pb14 > kUf8FaderPbMax) pb14 = kUf8FaderPbMax;
    const double topSlider = DB2SLIDER(kUf8FaderTopDb);
    const double slider    = static_cast<double>(pb14) /
                             static_cast<double>(kUf8FaderPbMax) * topSlider;
    const double db_raw    = SLIDER2DB(slider);
    const double db_cal    = interpFaderCal(db_raw, /*current_to_target=*/true);
    return std::pow(10.0, db_cal / 20.0);
}

// Inverse of pbToLinearVolume. Used to echo REAPER volume back onto the
// UF8 motor so the physical fader follows mouse edits in the DAW.
uint16_t linearVolumeToPb(double linear)
{
    if (!(linear > 0.0)) return 0;                  // catches 0 and NaN
    const double db_target = 20.0 * std::log10(linear);
    const double db_raw    = interpFaderCal(db_target, /*current_to_target=*/false);
    const double slider    = DB2SLIDER(db_raw);
    const double topSlider = DB2SLIDER(kUf8FaderTopDb);
    if (!(topSlider > 0.0)) return 0;
    double pb = slider / topSlider * static_cast<double>(kUf8FaderPbMax);
    if (pb < 0)                                pb = 0;
    if (pb > static_cast<double>(kUf8FaderPbMax)) pb = kUf8FaderPbMax;
    return static_cast<uint16_t>(pb + 0.5);
}

// Parse a UF8 vendor-USB IN report and, when it's an input event, convert
// to the equivalent MCU MIDI message and push it back up to REAPER via the
// virtual MIDI source. The UF8 firmware packs events into this EP 0x81 IN
// stream, inter-mingled with its continuous polling heartbeat (which we
// just skip).
//
// Observed event framing — all starting from a position past the 31 60
// session prefix (stripped here if present):
//
//   FF 21 03 <strip> <A> <B> CKSUM    — fader position (16-bit abs.)
//   FF 22 03 <id>    00 <s> CKSUM     — button press/release
//   FF 23 02 <strip> <s>    CKSUM     — fader touch
//   FF 24 02 <strip> <d>    CKSUM     — v-pot rotation (delta)
//   FF 33 02 <strip> <s>    CKSUM     — v-pot push toggle (tbc)
//
// Plus the "FF 04 02 9d 01 a4" and partner "FF 04 02 94 01 9b" polling
// packets which repeat at ~100 Hz — we ignore those.
// Per-strip touch state with debounce. The UF8 touch sensor bounces
// during a single sustained touch (~9 press+release pairs per second
// observed in captures). Emitting every one of those as MCU note-on/off
// would make REAPER's automation engine stutter (e.g. Touch mode would
// release-then-reacquire constantly). Instead:
//   - On any touch-press IN event: emit MCU Note-on immediately (if not
//     already "reported as touched"). Update last-press timestamp.
//   - On touch-release IN event: do NOTHING immediately.
//   - The REAPER timer (30 Hz) emits Note-off when (now - lastPress)
//     exceeds the hold threshold, which means the user really let go.
// Touch-state is debounced because the UF8 capacitive sensor bounces
// (~9 press+release pairs per second during a sustained touch — confirmed
// in captures). Press events commit immediately so the motor releases
// right as the user grabs the fader; release events are deferred until
// the sensor has stayed quiet for kTouchDebounceQuiet — if any press
// arrives during that window the release is cancelled.
std::array<std::atomic<bool>, 8>                       g_touchReported{};
std::array<std::atomic<bool>, 8>                       g_touchReleasePending{};
std::array<std::chrono::steady_clock::time_point, 8>   g_touchLastPress{};
constexpr auto kTouchDebounceQuiet = std::chrono::milliseconds(150);

bool ReaSixtySurface::GetTouchState(MediaTrack* tr, int isPan)
{
    if (isPan != 0) return false;   // fader touch only
    for (int s = 0; s < 8; ++s) {
        if (!g_touchReported[s].load()) continue;
        if (g_slotTrack[s] == tr) return true;
    }
    return false;
}

// Button LEDs — coloured path (cap31, 2026-04-26).
//   FF 38 04 <cell> 00 <a> <b> CKSUM   +
//   FF 39 04 <cell> 00 <a> <b> CKSUM
// Cell formula: cell = 0x17 - 3*strip - led_offset, with led_offset 0=SOLO,
// 1=CUT, 2=SEL. Strip 0 (leftmost UF8 strip) → cells 0x15..0x17, strip 7 →
// 0x00..0x02. Matches the FF 3B id map — the legacy mono-on/off path lives
// in the same id space — but unlike FF 3B this pair sets the LED to its
// proper colour: SOLO yellow, CUT orange, SEL white.
//
// REC ARM has no colour-pair mapping yet; left on the FF 3B path and gated
// off until a cap23b verifies its id range.
enum class LedClass : uint8_t { Sel = 0, Mute = 1, Solo = 2, Arm = 3 };

// Resolve the LED colour for a given class on a given track. SEL pulls from
// REAPER's track-colour (snapped to SSL360's DAW-Colour palette); SOLO and
// CUT use class defaults (yellow / red). When the user later wires a
// settings UI for per-class overrides, this is the spot to read them.
uf8::LedColour ledColourFor(LedClass cls, MediaTrack* tr)
{
    if (cls == LedClass::Sel && tr) {
        // Rec-armed override: SSL UF8 paints the SEL LED red when the
        // track is armed (no separate Rec-Arm LED — same physical LED).
        // Confirmed by user against SSL 360°'s rendering (2026-04-26).
        if (GetMediaTrackInfo_Value(tr, "I_RECARM") > 0.5) {
            return uf8::ledColourRed();
        }
        const uint32_t rgb = static_cast<uint32_t>(GetTrackColor(tr)) & 0x00FFFFFFu;
        return uf8::ledColourForTrackRgb(rgb);
    }
    switch (cls) {
        case LedClass::Solo: return uf8::ledColourYellow();
        case LedClass::Mute: return uf8::ledColourRed();
        case LedClass::Sel:  return uf8::ledColourWhite();
        default:             return uf8::ledColourWhite();
    }
}

uf8::LedClass toUf8LedClass(LedClass cls)
{
    switch (cls) {
        case LedClass::Solo: return uf8::LedClass::Solo;
        case LedClass::Mute: return uf8::LedClass::Cut;
        case LedClass::Sel:  return uf8::LedClass::Sel;
        default:             return uf8::LedClass::Sel;
    }
}

void sendLedFrames(uf8::LedColourFrames frames)
{
    if (!g_dev) return;
    if (!frames.ff38.empty())   g_dev->send(std::move(frames.ff38));
    if (!frames.ff39.empty())   g_dev->send(std::move(frames.ff39));
    if (!frames.legacy.empty()) g_dev->send(std::move(frames.legacy));
}

// Forward declarations for helpers defined further down (used by
// sendSelRenderTrigger to restore the AutoTrim LED after the cap33
// trigger sequence, and by drainInputQueue's FLIP path which routes
// fader pb14 → focused-param normalised position).
void pushAutoModeLeds(int mode);
int  autoModeToLedIndex(int mode);
uint16_t linearVolumeToPb(double linear);
extern int g_lastAutoMode;

// Cell 0x24 sits outside the per-strip LED range. cap33 shows SSL360
// firing a fixed off→on toggle on this cell at every selection event,
// always before the selected-strip bitmask. Replicate the exact 4-frame
// sequence — values are constant (no track-colour involved).
//
// Quirk: cell 0x24 ALSO happens to be the AutoTrim LED in MCU mode
// (`0x3F 0xF0` = AutoTrim's bright-orange). cap33 was recorded with the
// captured track in REAPER mode 0 (Trim/Read), so SSL360 lighting the
// AutoTrim LED looked indistinguishable from a "render trigger" pulse.
// In our extension, this fires on every SEL change regardless of auto
// mode, leaving TRIM pinned on next to whatever auto-LED is the actual
// active mode. Re-assert the correct AutoTrim state at the end of the
// sequence so the user sees only the LED that matches the track's mode.
void sendSelRenderTrigger()
{
    if (!g_dev) return;
    g_dev->send({0xFF, 0x38, 0x04, 0x24, 0x00, 0x12, 0xF0, 0x62});
    g_dev->send({0xFF, 0x39, 0x04, 0x24, 0x00, 0x12, 0xF0, 0x63});
    g_dev->send({0xFF, 0x38, 0x04, 0x24, 0x00, 0x3F, 0xF0, 0x8F});
    g_dev->send({0xFF, 0x39, 0x04, 0x24, 0x00, 0x00, 0xF0, 0x51});
    // Restore AutoTrim to the value driven by the current REAPER auto
    // mode (lit only when mode == 0/Trim/Read).
    const bool trimActive = (autoModeToLedIndex(g_lastAutoMode) == 2);
    sendLedFrames(uf8::buildUf8GlobalLed(uf8::Uf8GlobalLed::AutoTrim,
                                         trimActive));
}

// Push the selected-strip bitmask. cap33: SSL360 sends this after the
// cell 0x24 toggle, on every selection change.
void pushSelectedStripBitmask()
{
    if (!g_dev) return;
    uint16_t mask = 0;
    for (int s = 0; s < 8; ++s) {
        MediaTrack* t = g_slotTrack[s];
        if (t && GetMediaTrackInfo_Value(t, "I_SELECTED") > 0.5) {
            mask |= static_cast<uint16_t>(1u << s);
        }
    }
    g_dev->send(uf8::buildSelectedStripBitmask(mask));
}

void sendLed(LedClass cls, MediaTrack* tr, bool on)
{
    if (!g_dev) return;
    if (cls == LedClass::Arm) return;   // gate ARM until its colour-pair is captured
    for (int s = 0; s < 8; ++s) {
        if (g_slotTrack[s] != tr) continue;
        const uf8::LedClass devCls = toUf8LedClass(cls);
        const uf8::LedColour col = ledColourFor(cls, tr);
        sendLedFrames(uf8::buildLedColourPair(static_cast<uint8_t>(s), devCls, on, col));
        if (cls == LedClass::Sel) {
            sendSelRenderTrigger();
            pushSelectedStripBitmask();
        }
        return;
    }
}

void ReaSixtySurface::SetSurfaceSolo(MediaTrack* tr, bool solo)
{
    sendLed(LedClass::Solo, tr, solo);
    // UC1's Solo / Solo Clear LEDs track REAPER state. Solo Clear
    // reflects "any track soloed anywhere", so every SetSurfaceSolo
    // callback has to refresh regardless of which track fired it.
    (void)solo;
    if (g_uc1_surface) g_uc1_surface->refresh();
}
void ReaSixtySurface::SetSurfaceMute(MediaTrack* tr, bool mute)
{
    sendLed(LedClass::Mute, tr, mute);
    // Only the focused track's Cut LED matters on UC1 — skip refresh
    // when REAPER reports a different track's mute change.
    (void)mute;
    if (g_uc1_surface && tr && g_uc1_surface->focusedTrack() == tr) {
        g_uc1_surface->refresh();
    }
}
void ReaSixtySurface::SetSurfaceSelected(MediaTrack* tr, bool sel)
{
    sendLed(LedClass::Sel, tr, sel);
    // UC1 always follows whichever track just became selected. Ignoring
    // the sel=false side means we keep the last-focused track driving
    // UC1 until a new one is picked — matches SSL 360°'s Focus Mode.
    if (sel && g_uc1_surface) {
        g_uc1_surface->setFocusedTrack(tr);
    }
    // UF8 bank follows REAPER selection. Any selection change — clicks
    // in the TCP/MCP, ReaScript, another surface — rebanks so the
    // active track is visible on the UF8. Uses whichever FollowMode is
    // set globally (BucketSnap by default; LeftmostStrip is the
    // planned settings toggle). Only the sel=true edge triggers; a
    // deselect shouldn't move the view.
    if (sel) followSelectedInMixer(tr);
}
void ReaSixtySurface::SetSurfaceRecArm(MediaTrack* tr, bool arm)
{
    // Rec-arm doesn't have its own dedicated LED on the UF8 — SSL360
    // repaints the SEL LED in red when the track is armed and back to
    // track-colour/white when disarmed. Push a SEL refresh so the
    // colour switches even if I_SELECTED didn't change.
    sendLed(LedClass::Sel, tr, GetMediaTrackInfo_Value(tr, "I_SELECTED") > 0.5);
    (void)arm;
}

// Device lifecycle: the surface instance owns the UF8 connection and the
// timer registration. REAPER creates the instance when the user adds
// "Rea-Sixty" in Control Surface settings, and destroys it on removal or
// on shutdown.
void onTimer();
void onUf8Input(const uint8_t* data, size_t len);
void onMidiFromReaper(std::span<const uint8_t> bytes);

ReaSixtySurface::ReaSixtySurface()
{
    // Open the virtual MIDI ports first — harmless if unused, and keeps
    // the legacy MCU-SysEx scribble pipe available for anyone still
    // running CSI alongside us.
    g_midi = std::make_unique<uf8::MidiBridge>();
    if (g_midi->open("reaper_uf8")) {
        g_midi->setIncomingHandler(onMidiFromReaper);
        // Also try to open the UF8's own OS-level MCU MIDI port so we
        // can drive the per-strip LEDs via MCU note-on/off. Silently
        // no-ops if no matching destination exists (e.g. SSL 360° not
        // installed and the UF8 not exposed as native MIDI) — LED
        // feedback just stays dark in that case.
        g_midi->openUf8Output();
    }

    // UF8 open — optional now. Either UF8, UC1, or both may be on the bus
    // during any given session. Previously the surface bailed out if UF8
    // couldn't be opened, which meant a UC1-only setup never reached the
    // UC1 init below.
    g_dev = std::make_unique<uf8::UF8Device>();
    const bool uf8Opened = g_dev->open();
    if (!uf8Opened) {
        ShowConsoleMsg(("Rea-Sixty UF8: " + g_dev->lastError()
                        + "  (UF8 optional — continuing)\n").c_str());
        g_dev.reset();
    } else {
        g_sync = std::make_unique<uf8::ColorSync>(*g_dev);
        g_sync->invalidate();
        g_dev->setRawInputHandler(onUf8Input);

        // UF8 firmware powers up with every SEL/MUTE/SOLO LED lit; we want
        // them all dark until REAPER state says otherwise. Push the OFF
        // colour-pair for every per-strip LED at open time so the initial
        // display matches an idle REAPER session.
        for (uint8_t s = 0; s < 8; ++s) {
            sendLedFrames(uf8::buildLedColourPair(s, uf8::LedClass::Solo, false));
            sendLedFrames(uf8::buildLedColourPair(s, uf8::LedClass::Cut,  false));
            sendLedFrames(uf8::buildLedColourPair(s, uf8::LedClass::Sel,  false));
        }

        // Force the bank-change re-sync block to fire on the very first
        // timer tick so LED state, slot caches and colors all reflect
        // REAPER's actual state from the get-go (rather than whatever stale
        // values the surface booted with).
        g_bankDirty.store(true);

        // Zero the fader positions captured in the layer-replay blob.
        const uint16_t pb0dB = linearVolumeToPb(1.0);
        const uint8_t  lsb   = static_cast<uint8_t>(pb0dB & 0x7F);
        const uint8_t  msb   = static_cast<uint8_t>((pb0dB >> 7) & 0x7F);
        for (uint8_t s = 0; s < 8; ++s) {
            g_dev->send(uf8::buildFaderPosition(s, lsb, msb));
        }
    }

    // Best-effort UC1 attach — absence is fine, UF8 runs standalone.
    g_uc1_dev = std::make_unique<uc1::UC1Device>();
    if (g_uc1_dev->open()) {
        ShowConsoleMsg("Rea-Sixty UC1: opened\n");
        g_uc1_surface = std::make_unique<uc1::UC1Surface>();
        g_uc1_surface->attach(*g_uc1_dev);
        // Dump the first handful of raw IN frames to the console so we
        // can see what UC1 is sending on its own vs. only after we push
        // something. Useful for diagnosing connection-lost failures.
        static int kUc1DumpRemaining = 12;
        g_uc1_dev->setRawInputHandler([](const uint8_t* data, size_t len) {
            if (kUc1DumpRemaining <= 0) return;
            --kUc1DumpRemaining;
            char buf[256];
            int off = std::snprintf(buf, sizeof(buf), "UC1 IN len=%zu ", len);
            for (size_t i = 0; i < len && off + 4 < (int)sizeof(buf); ++i) {
                off += std::snprintf(buf + off, sizeof(buf) - off, "%02x", data[i]);
            }
            if (off + 2 < (int)sizeof(buf)) {
                buf[off++] = '\n';
                buf[off] = '\0';
            }
            ShowConsoleMsg(buf);
        });
        // Focus whatever track REAPER currently considers "last touched"
        // so UC1 has something meaningful to drive from the first click.
        if (auto* tr = GetLastTouchedTrack()) {
            g_uc1_surface->setFocusedTrack(tr);
        }
    } else {
        ShowConsoleMsg(("Rea-Sixty UC1: " + g_uc1_dev->lastError()
                        + "  (UC1 optional — UF8 continues)\n").c_str());
        g_uc1_dev.reset();
    }

    plugin_register("timer", reinterpret_cast<void*>(onTimer));

    // Push the persisted brightness level to both devices now that
    // they're open. If no ExtState yet (first-run), defaults to "full".
    loadBrightness();
    applyBrightness();
}

ReaSixtySurface::~ReaSixtySurface()
{
    plugin_register("-timer", reinterpret_cast<void*>(onTimer));
    g_sync.reset();
    if (g_midi) g_midi->close();
    g_midi.reset();
    if (g_dev) g_dev->close();
    g_dev.reset();
    g_uc1_surface.reset();
    if (g_uc1_dev) g_uc1_dev->close();
    g_uc1_dev.reset();
    g_slotTrack.fill(nullptr);
}

IReaperControlSurface* createReaSixty(const char* /*type*/, const char* /*config*/,
                                      int* /*errStats*/)
{
    return new ReaSixtySurface();
}

HWND reaSixtyShowConfig(const char* /*type*/, HWND /*parent*/, const char* /*config*/)
{
    // No user-configurable options yet — we auto-detect the USB device.
    return nullptr;
}

reaper_csurf_reg_t g_csurfReg = {
    "REASIXTY",
    "Rea-Sixty",
    createReaSixty,
    reaSixtyShowConfig,
};

// De-dup for pitch-bend so REAPER's motor echo doesn't re-trigger us.
std::array<std::atomic<uint8_t>, 8> g_lastMsbOut{};
std::array<std::atomic<uint8_t>, 8> g_lastLsbOut{};

// Latest raw fader position (pb14) seen during a touch — recorded
// regardless of deadband, so commitDebouncedTouchReleases can snap
// REAPER to where the user's hand actually left the fader. Without
// this the motor jerks back to pre-touch on release because the
// >=4-LSB deadband swallowed the tiny finger-induced shift.
std::array<std::atomic<uint16_t>, 8> g_lastTouchPb{};
std::array<std::atomic<bool>, 8>     g_lastTouchPbValid{};

// Last fader motor position we actually wrote to the UF8 — lets the timer
// dedup motor-echo pushes, and lets the touch-release handler prime the
// firmware with the user's new position before re-enabling the motor.
std::array<uint16_t, 8>             g_lastFaderPb{};
bool                                g_faderPbInit{false};

void onUf8Input(const uint8_t* data, size_t len)
{
    // Debug: log every non-trivial IN packet that reaches this handler,
    // including payloads that might be interesting (anything not a pure
    // 31 60 / poll pair).
    if (FILE* f = std::fopen("/tmp/reaper_uf8_in_dispatch.log", "a")) {
        std::fprintf(f, "[%zu] ", len);
        for (size_t k = 0; k < len && k < 32; ++k) std::fprintf(f, "%02x ", data[k]);
        std::fprintf(f, "\n");
        std::fclose(f);
    }

    if (!g_midi) return;

    // Walk past each FF frame in the buffer (multiple frames can arrive
    // concatenated, optionally preceded by a 31 60 / 31 00 session prefix).
    size_t i = 0;
    while (i < len) {
        // Session-prefix byte 0x31 + flag byte: skip both.
        if (data[i] == 0x31 && i + 1 < len) { i += 2; continue; }
        if (data[i] != 0xFF) { ++i; continue; }
        if (i + 2 >= len) break;  // need at least FF, cmd, something

        // Figure out frame size based on the command byte.
        const uint8_t cmd = data[i + 1];
        size_t frameSize = 0;
        switch (cmd) {
            // FF 04 02 XX 01 XX = 6 bytes total. Two payload variants
            // (02 9d 01 a4 / 02 94 01 9b) cycle alternately. Previous
            // frameSize=7 was wrong: it skipped past the actual end of
            // the poll frame, swallowing whatever FF byte came next —
            // which is exactly how bundled "FF 04 ... FF 21 03 ..."
            // and "FF 04 ... FF 20 02 ..." packets lost their touch /
            // fader events. Symptom: motor stays engaged because the
            // firmware sees its FF 21 03 echoes never come back from
            // host (we never receive the events to echo them).
            case 0x04: frameSize = 6; break;   // poll
            case 0x21: frameSize = 7; break;   // fader position
            case 0x22: frameSize = 7; break;   // button
            case 0x20: frameSize = 6; break;   // fader touch (capacitive)
            case 0x23: frameSize = 6; break;   // pressure sensor (TBD)
            case 0x24: frameSize = 6; break;   // v-pot rotation
            case 0x33: frameSize = 6; break;   // pressure sensor (TBD)
            default:   frameSize = 0; break;
        }
        if (frameSize == 0 || i + frameSize > len) {
            // Log the first few bytes of anything we didn't recognise so
            // unknown events (e.g. channel-encoder rotation) can be
            // reverse-engineered by turning the control with logging on.
            if (FILE* f = std::fopen("/tmp/reaper_uf8_unknown.log", "a")) {
                std::fprintf(f, "unknown:");
                const size_t show = std::min<size_t>(len - i, 12);
                for (size_t k = 0; k < show; ++k) std::fprintf(f, " %02X", data[i + k]);
                std::fprintf(f, "\n");
                std::fclose(f);
            }
            ++i;
            continue;
        }

        // Dispatch by command.
        if (cmd == 0x21 && data[i + 2] == 0x03) {
            // Fader position: FF 21 03 strip A B cksum
            // A = MCU LSB (high bit is a flag, masked), B = MCU MSB.
            // Native route: push into the input queue as an absolute volume
            // (coalesced by strip) — the timer will apply it to the track.
            // We only queue while the user is actively touching the fader,
            // so REAPER's motor echo doesn't feed back.
            const uint8_t strip = data[i + 3];
            if (strip < 8 && g_touchReported[strip].load()) {
                const uint8_t lsb = data[i + 4] & 0x7F;
                const uint8_t msb = data[i + 5] & 0x7F;
                const uint16_t pb14 = static_cast<uint16_t>(lsb | (msb << 7));
                // Always record the raw position so the touch-release
                // commit can snap REAPER to where the fader physically
                // ended up, even if every frame this touch was sub-deadband.
                g_lastTouchPb[strip].store(pb14);
                g_lastTouchPbValid[strip].store(true);
                // Echo the position back to UF8 with bit 7 of LSB SET.
                // SSL360 does this throughout every touch (cap32 OUT
                // frames at 0.497..1.748). The firmware uses these
                // echoes to update its motor-target buffer WITHOUT
                // engaging the motor. When the touch ends the firmware
                // implicitly re-engages on this target — no jerk
                // because the target is exactly where the user's hand
                // left the fader. Without these echoes the firmware
                // re-engages to the pre-touch target = the jerk.
                if (g_dev) {
                    g_dev->send(uf8::buildFaderPosition(strip, lsb | 0x80, msb));
                }
                const uint8_t prevMsb = g_lastMsbOut[strip].load();
                const uint8_t prevLsb = g_lastLsbOut[strip].load();
                const int lsbDelta = std::abs(int(lsb) - int(prevLsb));
                if (msb != prevMsb || lsbDelta >= 4) {
                    g_lastMsbOut[strip].store(msb);
                    g_lastLsbOut[strip].store(lsb);
                    queueInput({PendingInput::VolumeAbs, strip, pbToLinearVolume(pb14)});
                }
            }
        } else if (cmd == 0x22 && data[i + 2] == 0x03) {
            // Button: FF 22 03 id 00 state cksum
            //
            // UF8 PM-mode button ID map (see docs/protocol-notes.md). The
            // firmware does NOT follow the MCU-standard per-strip layout —
            // for example 0x18..0x1F are the top soft-keys above each
            // scribble, not MCU SELECT.
            //
            // Per-strip:
            //   0x08..0x0F  V-Pot push           (stride 1)
            //   0x18..0x1F  Top soft-key         (stride 1)
            //   0x20..0x37  SOLO/CUT/SEL         (3-byte group per strip)
            //
            // Global buttons live at 0x40..0x7E (see docs/protocol-notes.md).
            //
            // In PM mode REAPER's MCU surface (CSI) is not the active target,
            // so per-strip SOLO/CUT/SEL are routed via the REAPER API
            // directly on press-edge. Top soft-keys are swallowed (otherwise
            // they'd fire MCU SELECT). Everything else still falls through
            // to MCU passthrough so CSI mappings for global functions keep
            // working.
            const uint8_t id    = data[i + 3];
            const uint8_t state = data[i + 5];
            const bool pressed  = state == 0x01;

            bool handledNatively = false;

            if (id == 0x6F) {
                // Fine / Shift modifier — track state so SEL can distinguish
                // exclusive (plain) from additive (Shift) selection.
                g_shiftHeld.store(pressed);
                handledNatively = true;
            } else if (id == 0x73) {
                if (pressed) {
                    g_encoderMode.store(EncoderMode::Nav);
                    SetExtState("ReaSixty", "encoderMode", "Nav", true);
                }
                handledNatively = true;
            } else if (id == 0x74) {
                if (pressed) {
                    g_encoderMode.store(EncoderMode::Nudge);
                    SetExtState("ReaSixty", "encoderMode", "Nudge", true);
                }
                handledNatively = true;
            } else if (id == 0x75) {
                if (pressed) {
                    g_encoderMode.store(EncoderMode::Focus);
                    SetExtState("ReaSixty", "encoderMode", "Focus", true);
                }
                handledNatively = true;
            } else if (id == 0x76) {
                // Channel Encoder Push — UF8 default is "return to
                // CHANNEL mode" (the track-selection default). We map it
                // to the same: flip the encoder back into Nav mode.
                if (pressed) {
                    g_encoderMode.store(EncoderMode::Nav);
                    SetExtState("ReaSixty", "encoderMode", "Nav", true);
                }
                handledNatively = true;
            } else if (id >= 0x58 && id <= 0x5D) {
                // Automation keys. REAPER modes: 0 Trim/Read (default),
                // 1 Read, 2 Touch, 3 Write, 4 Latch, 5 Latch Preview.
                // UF8 Off and Trim both map to Trim/Read (REAPER has no
                // separate "Off" mode).
                static constexpr int kModeById[6] = {
                    /*0x58 Off  */ 0,
                    /*0x59 Read */ 1,
                    /*0x5A Wri  */ 3,
                    /*0x5B Trim */ 0,
                    /*0x5C Latc */ 4,
                    /*0x5D Touc */ 2,
                };
                if (pressed) {
                    const int target = kModeById[id - 0x58];
                    queueInput({PendingInput::AutomationMode, 0,
                                static_cast<double>(target)});
                    // Light the target Auto LED immediately. Without this,
                    // the firmware briefly flashes its default (TRIM) over
                    // the held button while we wait for REAPER to apply
                    // the mode change and the next tick to read it back.
                    pushAutoModeLeds(target);
                }
                handledNatively = true;
            } else if (id == 0x7A || id == 0x7B || id == 0x7C
                    || id == 0x7D || id == 0x7E) {
                // Zoom pad. 4 arrows zoom horizontally/vertically; the
                // centre (0x7C) fits the project to window. Wired to
                // REAPER's built-in zoom actions via Main_OnCommand on
                // press only (repeat-on-hold not implemented — single
                // press = single zoom step). LED lights momentarily
                // while held as press feedback.
                uf8::Uf8GlobalLed led = uf8::Uf8GlobalLed::ZoomCenter;
                int action = 0;
                switch (id) {
                    case 0x7A: led = uf8::Uf8GlobalLed::ZoomUp;     action = 40112; break;
                    case 0x7E: led = uf8::Uf8GlobalLed::ZoomDown;   action = 40111; break;
                    case 0x7B: led = uf8::Uf8GlobalLed::ZoomLeft;   action = 1011;  break;
                    case 0x7D: led = uf8::Uf8GlobalLed::ZoomRight;  action = 1012;  break;
                    case 0x7C: led = uf8::Uf8GlobalLed::ZoomCenter; action = 40295; break;
                }
                sendLedFrames(uf8::buildUf8GlobalLed(led, pressed));
                if (pressed && action) {
                    queueInput({PendingInput::MainAction, 0,
                                static_cast<double>(action)});
                }
                handledNatively = true;
            } else if (id == 0x54) {
                // FLIP button: swap fader ↔ V-Pot mapping for every strip.
                // Toggle and persist; pageDirty forces a fresh re-render
                // of value-line / motor / V-Pot bar with swapped sources.
                if (pressed) {
                    const bool next = !g_flip.load();
                    g_flip.store(next);
                    g_pageDirty.store(true);
                    SetExtState("ReaSixty", "flip", next ? "1" : "0", true);
                }
                handledNatively = true;
            } else if (id == 0x50) {
                // Plugin button — toggles "UF8 faders drive SSL strip
                // fader" mode. With it on, the fader steers the CS
                // plug-in's Fader Level (linkIdx 35) and the V-Pot's
                // Pan-fallback steers the plug-in's Pan (linkIdx 3);
                // off restores REAPER track volume + track pan.
                if (pressed) {
                    const bool next = !g_pluginFaderMode.load();
                    g_pluginFaderMode.store(next);
                    g_pageDirty.store(true);
                    SetExtState("ReaSixty", "pluginFaderMode",
                                next ? "1" : "0", true);
                }
                handledNatively = true;
            } else if (id == 0x6E) {
                // PAN button: toggle "force all V-Pots to Pan" regardless
                // of SSL-plug-in presence on each track. Invalidates the
                // per-strip caches so the Value Line / V-Pot bar switch
                // to pan-mode rendering on the next tick.
                if (pressed) {
                    const bool next = !g_forcePan.load();
                    g_forcePan.store(next);
                    g_pageDirty.store(true);
                    SetExtState("ReaSixty", "forcePan", next ? "1" : "0", true);
                }
                handledNatively = true;
            } else if (id == 0x43 || id == 0x44) {
                // Quick 1 (0x43) / Quick 2 (0x44) — Selection Mode row
                // above the channel encoder. In Plug-in Mixer Mode these
                // are locked to domain switching per SSL UF8 User Guide:
                //   Quick 1 → Channel Strip
                //   Quick 2 → Bus Compressor
                // (Quick 3 / 0x45 is reserved for the I/O meter toggle —
                //  not wired yet.)
                if (pressed) {
                    const auto target = (id == 0x43)
                        ? uf8::Domain::ChannelStrip
                        : uf8::Domain::BusComp;
                    const auto fp = uf8::getFocusedParam();
                    if (fp.domain != target) {
                        // Slot 0 = Bypass for both domains (CS plug-in's
                        // own Bypass on CS, BC plug-in's CompBypass on
                        // BC). Bank-follow-focus snaps the soft-key bank
                        // to V-POT (bank 0) on the next tick.
                        uf8::setFocus({target, 0});
                    }
                }
                handledNatively = true;
            } else if (id >= 0x68 && id <= 0x6D) {
                // Soft-key bank selectors: 0x68 = V-POT bank, 0x69..0x6D
                // = Bank 1..5. Switches which 8 param labels appear in
                // the top zones. BC mode only has 0..1 — ignore higher.
                // Pressing any of these also clears global Pan override
                // (matches SSL paradigm: "I want params, not pan").
                if (pressed) {
                    const int target = (id == 0x68) ? 0 : (id - 0x69 + 1);
                    const auto fp = uf8::getFocusedParam();
                    const auto domain = (fp.domain == uf8::Domain::BusComp)
                        ? uf8::Domain::BusComp : uf8::Domain::ChannelStrip;
                    if (target <= softkey::maxBankFor(domain)) {
                        if (g_softKeyBank.exchange(target) != target) {
                            g_softKeyDirty.store(true);
                            char buf[8];
                            std::snprintf(buf, sizeof(buf), "%d", target);
                            SetExtState("ReaSixty", "softKeyBank", buf, true);
                        }
                    }
                    if (g_forcePan.load()) {
                        g_forcePan.store(false);
                        g_pageDirty.store(true);
                        SetExtState("ReaSixty", "forcePan", "0", true);
                    }
                }
                handledNatively = true;
            } else if (id == 0x52 || id == 0x53) {
                // Page ← (0x52) / Page → (0x53) → previous/next Soft-Key
                // Bank. Walks g_softKeyBank in the same domain-aware way
                // the dedicated bank selectors (0x68/0x69..0x6D) do —
                // clamped to softkey::maxBankFor(domain) (both = 5).
                // LED lights momentarily while held as press feedback.
                const auto led = (id == 0x53)
                    ? uf8::Uf8GlobalLed::PageRight
                    : uf8::Uf8GlobalLed::PageLeft;
                sendLedFrames(uf8::buildUf8GlobalLed(led, pressed));
                if (pressed) {
                    const int delta = (id == 0x53) ? 1 : -1;
                    const auto fp = uf8::getFocusedParam();
                    const auto domain = (fp.domain == uf8::Domain::BusComp)
                        ? uf8::Domain::BusComp : uf8::Domain::ChannelStrip;
                    const int maxBank = softkey::maxBankFor(domain);
                    int next = g_softKeyBank.load() + delta;
                    if (next < 0)         next = 0;
                    if (next > maxBank)   next = maxBank;
                    if (g_softKeyBank.exchange(next) != next) {
                        g_softKeyDirty.store(true);
                        char buf[8];
                        std::snprintf(buf, sizeof(buf), "%d", next);
                        SetExtState("ReaSixty", "softKeyBank", buf, true);
                    }
                }
                handledNatively = true;
            } else if (id == 0x78 || id == 0x79) {
                // Bank ← (0x78) / Bank → (0x79). 8-strip scroll, clamped
                // so the bank start can go from 0 up to max(0, tracks-1).
                // Allowing up to tracks-1 means the last bank can end
                // with empty slots rather than snapping short of the end.
                // LED lights momentarily while held as press feedback.
                const auto led = (id == 0x79)
                    ? uf8::Uf8GlobalLed::BankRight
                    : uf8::Uf8GlobalLed::BankLeft;
                sendLedFrames(uf8::buildUf8GlobalLed(led, pressed));
                if (pressed) {
                    const int delta      = (id == 0x79) ? 8 : -8;
                    const int trackCount = CountTracks(nullptr);
                    const int maxStart   = trackCount > 1 ? trackCount - 1 : 0;
                    int next = g_bankOffset.load() + delta;
                    if (next < 0)        next = 0;
                    if (next > maxStart) next = maxStart;
                    if (next != g_bankOffset.exchange(next)) {
                        g_bankDirty.store(true);
                    }
                }
                handledNatively = true;
            } else if (id >= 0x08 && id <= 0x0F) {
                // V-Pot push: reset focused param / pan to neutral. The
                // toggle actions (Phase/A/B/HQ) live on the top soft-keys
                // (0x18..0x1F), not here — see that branch below.
                if (pressed) {
                    queueInput({PendingInput::PanCenter,
                                static_cast<uint8_t>(id - 0x08), 0.0});
                }
                handledNatively = true;
            } else if (id >= 0x20 && id <= 0x37) {
                const uint8_t strip = static_cast<uint8_t>((id - 0x20) / 3);
                const int which     = (id - 0x20) % 3;   // 0=SOLO 1=CUT 2=SEL
                if (pressed) {
                    PendingInput::Kind k = PendingInput::SoloToggle;
                    if (which == 1) {
                        k = PendingInput::MuteToggle;
                    } else if (which == 2) {
                        k = g_shiftHeld.load() ? PendingInput::SelectToggle
                                               : PendingInput::SelectExclusive;
                    }
                    queueInput({k, strip, 0.0});
                }
                handledNatively = true;
            } else if (id >= 0x18 && id <= 0x1F) {
                // Per-strip top soft-key. In Plug-in Mixer Mode this picks
                // the param under that strip from the active bank and
                // assigns it to all 8 V-Pots (= setFocus across the bus).
                //
                // BYPASS / BUS COMP are normal LinkSlots now (plug-in's
                // own Bypass param at linkIdx 0). Pressing them sets
                // focus across the bus; V-Pot push toggles per-strip.
                //
                // Soft-key sets focus to the column's slot (real VST3 param
                // OR synthetic ext::* like TrackPhase / PluginAB / PluginHQ).
                // Render + V-Pot push paths special-case the synthetics.
                // kNoSlot positions (WIDTH, EXT S/C, PRE on CS2 etc.) stay
                // silent — Settings UI will wire them to user actions later.
                if (pressed) {
                    const int strip = id - 0x18;
                    const auto fp = uf8::getFocusedParam();
                    const auto domain = (fp.domain == uf8::Domain::BusComp)
                        ? uf8::Domain::BusComp : uf8::Domain::ChannelStrip;
                    const int bank = std::clamp(g_softKeyBank.load(),
                        0, softkey::maxBankFor(domain));
                    const auto v = softkey::viewFor(domain, bank);
                    const int linkIdx = v.linkIdx[strip];
                    if (linkIdx != softkey::kNoSlot) {
                        uf8::setFocus({domain, linkIdx});
                    }
                }
                handledNatively = true;
            }

            if (!handledNatively) {
                const uint8_t mcu[3] = {0x90, id, pressed ? uint8_t{0x7F} : uint8_t{0x00}};
                g_midi->send(std::span<const uint8_t>(mcu, 3));
            }
        } else if (cmd == 0x20 && data[i + 2] == 0x02) {
            // Fader touch: FF 20 02 strip state cksum
            //
            // Capacitive touch — hardware-debounced. We track the state so
            // fader-position events only apply while the user is touching
            // (kills the motor-echo feedback loop), and release the motor
            // so the user's hand isn't fighting it.
            //
            // **CRITICAL** strip-byte indexing: the firmware reports touch
            // events 1-indexed (strip 1 = leftmost fader, ..., strip 8 =
            // rightmost). Position events (FF 21 03), V-Pot rotation
            // (FF 24 02), and motor commands (FF 1D / FF 1E) all use
            // 0-indexed (strip 0 = leftmost). Discovered 2026-04-30 from
            // the Mac trace: touching PS1 produced touch=02 + position=01
            // and our LIMP command went to PS2, leaving PS1 motor engaged
            // — the "fader sperrt" symptom + the two-finger trick directly
            // explained. Subtract 1 here so all internal state and outbound
            // motor commands stay in 0-indexed land.
            const uint8_t rawStrip = data[i + 3];
            const uint8_t state    = data[i + 4];
            // Out-of-range guard: rawStrip should be 1..8. Anything else
            // is a malformed frame; skip it.
            if (rawStrip == 0 || rawStrip > 8) {
                i += frameSize;
                continue;
            }
            const uint8_t strip = static_cast<uint8_t>(rawStrip - 1);
            if (strip < 8) {
                // Diag log — same path as f73201c. Append-mode, one line
                // per touch event so we can correlate with FF 1B keepalive
                // and FF 1D motor commands logged from the worker thread.
                if (FILE* lg = std::fopen("/tmp/reaper_uf8_motor.log", "a")) {
                    const auto t = std::chrono::system_clock::now().time_since_epoch();
                    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t).count();
                    std::fprintf(lg, "[%lld] TOUCH strip=%u state=%u\n",
                                 static_cast<long long>(ms), strip, state);
                    std::fclose(lg);
                }
                // UF8 in PM mode does not auto-release the fader motor
                // on capacitive touch (that behaviour is only present in
                // DAW/MCU mode), so we send the motor-limp command
                // ourselves. On press we disable immediately so the user
                // isn't fighting the motor; on release we commit through
                // a debounced two-phase sequence (position first, motor
                // re-enable one timer tick later) to work around the
                // firmware's tendency to drive toward its stale target
                // between re-enable and the next position command.
                if (state != 0) {
                    g_touchLastPress[strip] = std::chrono::steady_clock::now();
                    g_touchReleasePending[strip].store(false);
                    if (!g_touchReported[strip].exchange(true)) {
                        // Priority-send so motor-limp jumps any LED /
                        // value-line bursts in the queue. Same path as
                        // f73201c — single FF 1D 02 strip 00.
                        if (g_dev) g_dev->sendPriority(uf8::buildMotorEnable(strip, false));
                    }
                } else {
                    g_touchReleasePending[strip].store(true);
                }
            }
        } else if (cmd == 0x24 && data[i + 2] == 0x02) {
            // V-pot rotation: FF 24 02 strip raw cksum
            //
            // `raw` is a 6-bit signed detent delta (two's complement) in
            // the low 6 bits:
            //   0x01..0x1F =  +1 .. +31  (clockwise)
            //   0x3F..0x20 =  -1 .. -32  (counter-clockwise)
            // Bits 0x40/0x80 are unused — confirmed empirically on a live
            // UF8 by capturing slow CW vs. CCW rotations.
            const uint8_t strip = data[i + 3];
            const uint8_t raw   = data[i + 4];
            int8_t signed6 = static_cast<int8_t>(raw & 0x3F);
            if (signed6 & 0x20) signed6 |= 0xC0;   // sign-extend from 6 bits
            if (strip < 8) {
                // Per-strip V-Pot — currently wired to track pan.
                // Scale: single detent = 1/128 of full pan range (≈0.78 %).
                const double delta = static_cast<double>(signed6) / 128.0;
                queueInput({PendingInput::PanDelta, strip, delta});
            } else if (strip == 0x08) {
                // Channel encoder — behaviour depends on the current
                // encoder mode. In all modes we pass the raw signed6
                // value; the drain accumulates fractional progress so
                // multi-event physical detents collapse to one step.
                switch (g_encoderMode.load()) {
                    case EncoderMode::Nav:
                        queueInput({PendingInput::SelectRelative, 0,
                                    static_cast<double>(signed6)});
                        break;
                    case EncoderMode::Nudge:
                        queueInput({PendingInput::PlayheadNudge, 0,
                                    static_cast<double>(signed6)});
                        break;
                    case EncoderMode::Focus:
                        queueInput({PendingInput::MouseScroll, 0,
                                    static_cast<double>(signed6)});
                        break;
                }
            }
        }
        // cmd 0x33 (v-pot push?) skipped — need more samples to verify

        i += frameSize;
    }
}

// Log raw HID reports until we've reverse-engineered the report format.
void logHid(const uint8_t* data, size_t len)
{
    if (FILE* f = std::fopen("/tmp/reaper_uf8_hid.log", "a")) {
        for (size_t i = 0; i < len; ++i) std::fprintf(f, "%02x ", data[i]);
        std::fprintf(f, "\n");
        std::fclose(f);
    }
}

// Very simple rolling log file so we can see what CSI sends us without
// needing REAPER's console plumbing. Written from the Core-MIDI thread —
// kept fopen/fprintf/fclose for simplicity; REAPER's MIDI rate is low.
void logMidi(std::span<const uint8_t> bytes)
{
    FILE* f = std::fopen("/tmp/reaper_uf8_midi.log", "a");
    if (!f) return;
    for (auto b : bytes) std::fprintf(f, "%02x ", b);
    std::fprintf(f, "\n");
    std::fclose(f);
}

// Translate an incoming MCU MIDI packet to UF8 vendor-USB commands.
// Currently handles the scribble-strip SysEx only — enough to prove the
// end-to-end pipe. Fuller mapping (meters, LEDs, V-pot, fader) is next.
void onMidiFromReaper(std::span<const uint8_t> bytes)
{
    logMidi(bytes);
    if (!g_dev || !g_dev->isOpen()) return;

    // MCU meter forwarding DISABLED — the UF8 meter command layout
    // (FF 38 04 <X> 00 <Y> <Z>) isn't fully decoded yet. Empirically
    // the expected `X = strip * 3` mapping doesn't match the byte
    // values seen in non-fader captures. Correlated audio-playback
    // capture on Windows is the next step — then we can safely
    // translate MCU D0 events without sending garbage to the device.
    (void)bytes;

    // Scribble-strip SysEx: F0 00 00 66 14 12 <pos> <text> F7
    // <pos> indexes into a 56-char-wide virtual display (7 chars * 8 strips),
    // row 0 (upper) at 0..0x37, row 1 (lower) at 0x38..0x6F.
    if (bytes.size() >= 8
        && bytes[0] == 0xF0 && bytes[1] == 0x00 && bytes[2] == 0x00
        && bytes[3] == 0x66 && bytes[4] == 0x14 && bytes[5] == 0x12)
    {
        const uint8_t startPos = bytes[6];

        size_t textStart = 7;
        size_t textEnd   = bytes.size();
        if (bytes.back() == 0xF7) --textEnd;

        // Walk the payload 7 chars at a time, one UF8 command per strip.
        size_t cursor = textStart;
        uint8_t pos = startPos;
        while (cursor < textEnd) {
            const size_t chunkLen = (textEnd - cursor) < 7u ? (textEnd - cursor) : 7u;
            std::string_view chunk(
                reinterpret_cast<const char*>(bytes.data() + cursor), chunkLen);

            // Trim trailing spaces — SSL 360° sends upper-row at natural
            // (unpadded) length. Keeping the text unpadded matches the
            // captured command format.
            auto last = chunk.find_last_not_of(' ');
            std::string_view trimmed = (last == std::string_view::npos)
                                       ? chunk.substr(0, 0) : chunk.substr(0, last + 1);

            if (pos < 0x38) {
                // Upper row (track names)
                const uint8_t strip = pos / 7;
                if (strip < 8) g_dev->send(uf8::buildStripTextUpper(strip, trimmed));
            } else {
                // Lower row (v-pot / value display)
                const uint8_t strip = (pos - 0x38) / 7;
                if (strip < 8) g_dev->send(uf8::buildStripTextLower(strip, chunk));
            }

            cursor += chunkLen;
            pos    += 7;
        }
        return;
    }

    // MCU pitch-bend (E<ch> LSB MSB) was previously forwarded as
    // FF 1E motor-drive — but pushZonesForVisibleSlots already streams
    // motor targets natively via uiVolLinear() and gates on
    // g_touchReported, while this MIDI path did NOT. Result: any user
    // who left an MCU surface configured against our virtual MIDI port
    // (legacy CSI setup) had REAPER's volume-echo pitchbend re-engaging
    // the motor mid-touch — fader felt locked because every motor-limp
    // we sent was undone microseconds later by the next pitchbend.
    // Drop the forward; native motor echo handles the same job correctly.
    if (bytes.size() == 3 && (bytes[0] & 0xF0) == 0xE0) {
        return;
    }
}

uint32_t reaperColorForVisibleSlot(int slot)
{
    const int trackCount = CountTracks(nullptr);
    const int realSlot = slot + g_bankOffset.load();
    if (realSlot >= trackCount) return 0;
    MediaTrack* tr = GetTrack(nullptr, realSlot);
    if (!tr) return 0;
    // REAPER returns native color as int. Bit 0x1000000 is "color set";
    // low 24 bits are 0xBBGGRR on Windows, 0xRRGGBB on mac/Linux via the
    // "native" encoding. GetTrackColor wraps that — low 24 bits are what
    // we want, matching quantize()'s 0xRRGGBB expectation.
    const int c = GetTrackColor(tr);
    return static_cast<uint32_t>(c) & 0x00FFFFFFu;
}

// If the track hosts an SSL 360°-enabled plug-in, return the short label
// the UF8 would display in Plug-in Mixer / Channel Strip Mode's "Channel
// Strip Type" zone ("CS 2", "4K B", "4K E", "BusComp"). Empty string
// means "no SSL plug-in on this track" — caller falls back to REAPER-native
// rendering.
//
// Detection is name-based: the VST3 plug-ins SSL ships expose their
// identity via TrackFX_GetFXName, prefixed with "VST3: SSL Native ...".
// We match the substring rather than the exact name to be forgiving of
// version bumps.
std::string sslPluginShortName(MediaTrack* tr)
{
    if (!tr) return {};
    if (!ValidatePtr2(nullptr, tr, "MediaTrack*")) return {};
    const int fxCount = TrackFX_GetCount(tr);
    char buf[256];
    for (int fx = 0; fx < fxCount; ++fx) {
        if (!TrackFX_GetFXName(tr, fx, buf, sizeof(buf))) continue;
        std::string n(buf);
        // TrackFX_GetFXName returns e.g. "VST3: SSL Native Channel Strip 2 (SSL)"
        if (n.find("Channel Strip 2") != std::string::npos) return "CS 2";
        if (n.find("Channel Strip")   != std::string::npos) return "CS";
        if (n.find("4K B")            != std::string::npos) return "4K B";
        if (n.find("4K E")            != std::string::npos) return "4K E";
        if (n.find("Bus Compressor")  != std::string::npos) return "BusComp";
        if (n.find("360 Link")        != std::string::npos) return "360 Link";
    }
    return {};
}

// Label shown in the "Currently Selected Parameter" zone (`FF 66 <n> 04
// <strip>`) — also the command that keeps the color bar rendered.
//
// Two-mode logic (matches the user's request 2026-04-20):
//   - Track hosts SSL 360° plug-in → show plug-in short name (e.g. "CS 2")
//     so the UF8 displays the plug-in identity the same way SSL 360° would.
//   - Otherwise → show the REAPER track name (truncated to 12 chars, as
//     that's the widest this zone renders cleanly). Empty track name →
//     "CH N" fallback so the slot still reads as populated.
std::string slotLabelForVisibleSlot(int slot)
{
    const int trackCount = CountTracks(nullptr);
    const int realSlot   = slot + g_bankOffset.load();
    if (realSlot >= trackCount) {
        char fallback[8];
        std::snprintf(fallback, sizeof(fallback), "CH %d", realSlot + 1);
        return fallback;
    }
    MediaTrack* tr = GetTrack(nullptr, realSlot);
    if (!tr) return "";

    if (auto ssl = sslPluginShortName(tr); !ssl.empty()) return ssl;

    // REAPER track name via P_NAME config parm.
    char name[256] = {0};
    if (GetSetMediaTrackInfo_String(tr, "P_NAME", name, false) && name[0] != 0) {
        std::string s(name);
        if (s.size() > 12) s.resize(12);
        return s;
    }
    char fallback[8];
    std::snprintf(fallback, sizeof(fallback), "CH %d", realSlot + 1);
    return fallback;
}

// Convert REAPER linear-amplitude volume (0..~4) to a dB string for the
// O/PdB zone's 6-char value slot: "-inf", "-6.0", "0.0", "12.0", "-12.5",
// "-100".
//
// REAPER stores fader position as a linear multiplier — 1.0 = 0 dB.
// Below ~10^-5 we call it "-inf" to match what the SSL LCD shows at
// the fader bottom. Always one decimal where it fits in 6 chars; values
// past -100 dB drop the decimal to keep the leading minus visible.
std::string formatDbReadout(double linearAmp)
{
    if (linearAmp < 1e-5) return "-inf";
    const double dB = 20.0 * std::log10(linearAmp);
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%.1f", dB);
    std::string s(buf);
    if (s.size() > 6) {
        std::snprintf(buf, sizeof(buf), "%.0f", dB);
        s.assign(buf);
        if (s.size() > 6) s.resize(6);
    }
    return s;
}

// Is this slot a binary in/out toggle (driven by a UC1 button on the
// physical surface)? Binary params render as full-or-empty on the
// V-Pot bar (no gradient) and respond to V-Pot push as a 0↔1 toggle
// instead of a "reset to default". Match by slot id rather than
// linkIdx so any future button additions show up here automatically.
bool isBinarySlot(const uf8::LinkSlot& s)
{
    if (!s.id) return false;
    std::string_view id{s.id};
    return id == "Bypass"          || id == "CompBypass"
        || id == "EqIn"            || id == "DynamicsIn"
        || id == "Listen"          || id == "HighEqBell"
        || id == "LowEqBell"       || id == "CompFastAttack"
        || id == "CompPeak"        || id == "GateExpander"
        || id == "GateAttack"      || id == "EqType"
        || id == "Pre"             || id == "ImpedanceIn";
}

// Is this slot a bipolar param with a meaningful centre detent (0 dB
// Gain, centred Trim/Fader)? Bipolar slots render as fill from centre
// outward with a centre marker (cap37 HF Gain). Unipolar slots
// (frequencies, Q, thresholds — anything with a min..max linear sweep
// and no neutral position) render as a single line moving across the
// bar (cap38 HF Freq).
bool isBipolarSlot(const uf8::LinkSlot& s)
{
    if (!s.id) return false;
    std::string_view id{s.id};
    return id == "InputTrim"          || id == "OutputTrim"
        || id == "LinkableFaderLevel" || id == "HighEqGain"
        || id == "HighMidEqGain"      || id == "LowMidEqGain"
        || id == "LowEqGain";
}

// Format a -1..+1 pan value as SSL-convention "L100" / "C" / "R50".
// Centred values render as bare "C" so the label-value gap is obvious.
std::string formatPanReadout(double pan)
{
    if (pan < -1.0) pan = -1.0;
    if (pan >  1.0) pan =  1.0;
    if (std::abs(pan) < 0.005) return "C";
    int pct = static_cast<int>(std::round(std::abs(pan) * 100.0));
    if (pct > 100) pct = 100;
    char buf[8];
    std::snprintf(buf, sizeof(buf), "%c%d", pan < 0 ? 'L' : 'R', pct);
    return buf;
}

// Compose the Value Line (19 chars) — e.g. "Vol        -6.0dB".
// Left-justified label, right-justified value. Truncates both if needed
// so the total fits within 19 chars.
std::string composeValueLine(std::string_view label, std::string_view value)
{
    constexpr size_t kWidth = 19;
    if (label.size() + value.size() + 1 > kWidth) {
        // Prefer to show full value; trim label.
        const size_t labelMax = kWidth - value.size() - 1;
        label = label.substr(0, labelMax);
    }
    std::string out(label);
    const size_t padding = kWidth - label.size() - value.size();
    out.append(padding, ' ');
    out.append(value);
    return out;
}

// Slot-level state caches — push only on change to avoid hammering the
// OUT endpoint 30× per second.
std::array<std::string, 8> g_lastTrackName{};
std::array<std::string, 8> g_lastSlotLabel{};
// Top-soft-key LED dedup. -1 = unset. Encodes the TopSoftKeyState as
// 0=Off / 1=Dim / 2=On so transitions between any two visible levels
// trigger a re-push.
std::array<int8_t, 8>      g_lastTopSoftKey{-1, -1, -1, -1, -1, -1, -1, -1};
std::array<std::string, 8> g_lastCsType{};
std::array<std::string, 8> g_lastValueLine{};
std::array<std::string, 8> g_lastFaderDb{};
std::array<std::string, 8> g_lastChanNum{};
std::array<uint16_t, 8>    g_lastVPotBar{};      // 16-bit LE per strip
std::array<uint8_t, 8>     g_lastVPotMode{};     // FF 66 09 0D mode byte per strip
bool                       g_vpotBarInit{false};

// Resolve the LinkSlot for one visible strip at the current page index.
// Returns nullptr when:
//   - no track
//   - PAN mode is forced globally (treat every strip as if it had no plug-in)
//   - the focused-param domain is None (no plugin selected at all)
//   - the track's plug-in isn't in our PluginMap registry
//   - the focused slot index is past the plug-in's slot count (e.g. BC2
//     has 7 slots, walking further reveals pan fallback)
// Must be called on the main thread — touches REAPER API.
//
// Domain-aware: a track with both CS2 + BC2 returns the slot of the plug-in
// matching the focused domain (so a focused BC param doesn't render against
// the CS plug-in and miss).
const uf8::LinkSlot* slotForStrip(MediaTrack* tr,
                                  const uf8::FocusedParam& focused,
                                  int* outFxIdx)
{
    if (!tr) return nullptr;
    if (g_forcePan.load()) return nullptr;
    auto match = uf8::lookupPluginOnTrack(tr, focused.domain);
    if (!match.map) return nullptr;
    // Resolve the Link slot index against THIS track's plugin map —
    // different tracks may host different CS variants (CS 2 vs 4K E
    // vs 4K G vs 4K B), each with its own slot ordering.
    const uf8::LinkSlot* slot = uf8::findSlotByLinkIdx(*match.map,
                                                       focused.slotIdx);
    if (!slot) return nullptr;
    if (outFxIdx) *outFxIdx = match.fxIndex;
    return slot;
}

// V-Pot bar 16-bit LE encoding from cap37 (HF Gain ±20 dB on 4K E)
// and cap38 (HF Freq sweep). Mode register is paired 0x08 per strip
// (bipolar centre-out render in firmware).
//
//   Bipolar (Gain, Pan, Trim):
//     byte0 = signed 8-bit two's complement, range [-100..+100]
//       0x00 → centre   (REQUIRES byte1 = 0x80 anchor)
//       0x64 → +full    (cap37 +20 dB max)
//       0x9C → -full    (cap37 -20 dB max, = -100 signed)
//     byte1 = 0x80 ONLY when byte0 = 0x00, else 0x00.
//
//   Unipolar (Freq, Q, Threshold):
//     byte0 = 0x01..0x62 linear (cap38 full sweep), byte1 = 0x00.
//
// Wire order matters: cap37 sends position (FF 66 11 0F) FIRST,
// then mode (FF 66 09 0D) ~55 µs later. Sending mode-then-position
// with mismatched encoding renders a wrong-direction bar.
uint16_t vpotPosFromBipolar(double v)
{
    if (v < -1.0) v = -1.0;
    if (v >  1.0) v =  1.0;
    int b0 = static_cast<int>(std::round(v * 100.0));
    if (b0 == 0) return 0x8000;  // centre anchor: byte0=0x00, byte1=0x80
    if (b0 < -100) b0 = -100;
    if (b0 >  100) b0 =  100;
    return static_cast<uint16_t>(static_cast<uint8_t>(static_cast<int8_t>(b0)));
}

// Mode 0x01 unipolar: byte0 = 0..0x64 (0..100), byte1 = 0x00.
// cap15 t=11.052/13.206/14.795 mode-register `01 01 01 01 03 03 03 03`
// with positions `00 00`/`32 00`/`64 00` cover 0%/50%/100%.
uint16_t vpotPosFromUnipolar(double v)
{
    if (v < 0.0) v = 0.0;
    if (v > 1.0) v = 1.0;
    int b0 = static_cast<int>(std::round(v * 100.0));
    if (b0 < 0)   b0 = 0;
    if (b0 > 100) b0 = 100;
    return static_cast<uint16_t>(b0 & 0xFF);
}

uint16_t vpotPosFromNormalized(double v) { return vpotPosFromUnipolar(v); }

uint16_t vpotPosFromPan(double pan) { return vpotPosFromBipolar(pan); }

void pushZonesForVisibleSlots()
{
    if (!g_dev || !g_dev->isOpen()) return;

    const int trackCount = CountTracks(nullptr);
    const int bankOffset = g_bankOffset.load();
    const auto focused   = uf8::getFocusedParam();

    std::array<uint16_t, 8> vpotBar{};

    // Full re-push on a bank change — every cached "last" value refers
    // to a different track now, so dedup would suppress legitimate
    // updates for a few ticks until each value happens to drift. Also
    // re-sync SOLO/MUTE/SEL/ARM LEDs to the new bank's tracks, since
    // REAPER only calls SetSurface* on actual state changes, not on
    // bank shifts.
    //
    // Same story for page changes: every strip's Parameter Label and
    // Value Line now refer to a different plugin slot, so dedup must
    // re-fire the pushes.
    const bool bankChanged    = g_bankDirty.exchange(false);
    // g_focusedDirty is the canonical "focused param changed" signal,
    // set by anyone who mutates uf8::g_focusedParam (Stage 2+ adds
    // cross-device writers). g_pageDirty is the older UF8-local flag —
    // currently set in tandem from main.cpp; both are drained here so
    // either path forces a full label/value re-push next tick.
    const bool focusChanged   = uf8::g_focusedDirty.exchange(false);
    const bool pageChanged    = g_pageDirty.exchange(false) || focusChanged;
    // Bank-follow-focus: when an external writer (UC1, plugin GUI,
    // chase) changes the focused param, switch the soft-key bank to
    // whichever bank in the new domain holds that linkIdx. Skips when
    // the linkIdx isn't in any bank (custom param, or not yet
    // registered in the soft-key tables).
    if (focusChanged) {
        const int b = softkey::bankContaining(focused.domain, focused.slotIdx);
        if (b >= 0 && g_softKeyBank.exchange(b) != b) {
            g_softKeyDirty.store(true);
            char buf[8];
            std::snprintf(buf, sizeof(buf), "%d", b);
            SetExtState("ReaSixty", "softKeyBank", buf, true);
        }
    }
    const bool softKeyChanged = g_softKeyDirty.exchange(false);
    if (pageChanged || softKeyChanged) {
        g_lastSlotLabel.fill({});
        g_lastValueLine.fill({});
        g_lastTopSoftKey.fill(-1);
    }
    if (pageChanged) {
        // The Plugin / FLIP / forcePan / focus toggles all set pageDirty
        // because they flip what the fader and V-Pot represent on each
        // strip. Force a full re-push of motor pb14, O/PdB readout, and
        // V-Pot bar — their dedup caches would otherwise pin the strip
        // to the previous mode's source (e.g. fader stays at the SSL
        // CS Fader position even after Plugin mode is toggled off).
        g_lastFaderPb.fill(0xFFFF);
        g_lastFaderDb.fill({});
        g_lastVPotBar.fill(0xFFFF);
    }
    if (bankChanged) {
        g_lastTrackName.fill({});
        g_lastSlotLabel.fill({});
        g_lastCsType.fill({});
        g_lastValueLine.fill({});
        g_lastFaderDb.fill({});
        g_lastChanNum.fill({});
        g_lastFaderPb.fill(0xFFFF);
        g_lastTopSoftKey.fill(-1);
        g_vpotBarInit = false;
        if (g_sync) g_sync->invalidate();

        for (int s = 0; s < 8; ++s) {
            const int rs = s + bankOffset;
            MediaTrack* t = (rs < trackCount) ? GetTrack(nullptr, rs) : nullptr;
            const bool solo = t && GetMediaTrackInfo_Value(t, "I_SOLO")     > 0.5;
            const bool mute = t && GetMediaTrackInfo_Value(t, "B_MUTE")     > 0.5;
            const bool sel  = t && GetMediaTrackInfo_Value(t, "I_SELECTED") > 0.5;
            const bool arm  = t && GetMediaTrackInfo_Value(t, "I_RECARM")   > 0.5;
            // LED bank re-sync: push SEL/MUTE/SOLO for each strip's new
            // track. ARM LED ID mapping still unverified (cap23b needed)
            // so it's gated inside sendLed itself.
            if (g_dev) {
                const auto strip = static_cast<uint8_t>(s);
                sendLedFrames(uf8::buildLedColourPair(strip, uf8::LedClass::Sel,  sel,
                                                      ledColourFor(LedClass::Sel,  t)));
                sendLedFrames(uf8::buildLedColourPair(strip, uf8::LedClass::Cut,  mute,
                                                      ledColourFor(LedClass::Mute, t)));
                sendLedFrames(uf8::buildLedColourPair(strip, uf8::LedClass::Solo, solo,
                                                      ledColourFor(LedClass::Solo, t)));
                (void)arm;
            }
        }
        // After bank shift, g_slotTrack hasn't been refreshed yet (that
        // happens in the next loop), so compute the bitmask directly from
        // the new bank's tracks instead of pushSelectedStripBitmask().
        if (g_dev) {
            uint16_t mask = 0;
            for (int s = 0; s < 8; ++s) {
                const int rs = s + bankOffset;
                MediaTrack* t = (rs < trackCount) ? GetTrack(nullptr, rs) : nullptr;
                if (t && GetMediaTrackInfo_Value(t, "I_SELECTED") > 0.5) {
                    mask |= static_cast<uint16_t>(1u << s);
                }
            }
            g_dev->send(uf8::buildSelectedStripBitmask(mask));
        }
    }

    for (int s = 0; s < 8; ++s) {
        const int realSlot = s + bankOffset;
        MediaTrack* tr = (realSlot < trackCount) ? GetTrack(nullptr, realSlot) : nullptr;

        // Keep the slot→track mapping fresh so GetTouchState can map
        // REAPER's track pointer back to a strip index.
        g_slotTrack[s] = tr;

        // Empty strip (bank window extends past the last track): blank
        // every zone so the last bucket's residue doesn't linger. Without
        // this, shifting from e.g. tracks 1–8 to 9–12 leaves strips 5–8
        // displaying whatever tracks 13–16 showed the session before.
        //
        // Dedup subtlety: the bankChanged branch above clears every
        // g_last* cache to "". That means after a bank shift, "cache ==
        // target" is indistinguishable between "display is already
        // blank" and "display state unknown, need to push". Force the
        // first-tick push via bankChanged so the blanks actually reach
        // the device; subsequent ticks dedup normally.
        if (!tr) {
            const std::string blankCs   = "    ";
            const std::string blankDb   = "    ";
            const std::string empty{};
            if (bankChanged || g_lastCsType[s] != blankCs) {
                g_lastCsType[s] = blankCs;
                g_dev->send(uf8::buildChannelStripType(static_cast<uint8_t>(s), blankCs));
            }
            if (bankChanged || !g_lastSlotLabel[s].empty()) {
                g_lastSlotLabel[s].clear();
                g_dev->send(uf8::buildPluginSlotName(static_cast<uint8_t>(s), empty));
            }
            if (bankChanged || !g_lastChanNum[s].empty()) {
                g_lastChanNum[s].clear();
                g_dev->send(uf8::buildChannelNumber(static_cast<uint8_t>(s), empty));
            }
            if (bankChanged || !g_lastTrackName[s].empty()) {
                g_lastTrackName[s].clear();
                g_dev->send(uf8::buildStripTextUpper(static_cast<uint8_t>(s), empty));
            }
            if (bankChanged || g_lastFaderDb[s] != blankDb) {
                g_lastFaderDb[s] = blankDb;
                g_dev->send(uf8::buildFaderDbReadout(static_cast<uint8_t>(s), blankDb));
            }
            if (bankChanged || !g_lastValueLine[s].empty()) {
                g_lastValueLine[s].clear();
                g_dev->send(uf8::buildValueLine(static_cast<uint8_t>(s), empty));
            }
            vpotBar[s] = 0;
            continue;
        }

        // Resolve the SSL plug-in (if any) and the currently-paged slot
        // for this strip. `slot == nullptr` means "no SSL plug-in on
        // this track" OR "plug-in has fewer slots than the current page
        // demands" (e.g. BC2 on page 8 — only 7 slots exist). In both
        // cases we fall back to the REAPER-native display (track name +
        // volume).
        int fxIdx = -1;
        const uf8::LinkSlot* slot = slotForStrip(tr, focused, &fxIdx);
        const uf8::PluginMap* map = nullptr;
        if (slot) {
            auto mm = uf8::lookupPluginOnTrack(tr);
            map = mm.map;
        }

        // Channel Strip Type zone: the plug-in's short name if recognised,
        // else a 4-char REAPER mnemonic ("RPR ").
        std::string csType = map ? std::string(map->displayShort)
                                 : std::string{};
        if (csType.empty()) csType = "RPR ";
        csType.resize(std::min<size_t>(csType.size(), 4), ' ');
        while (csType.size() < 4) csType += ' ';
        if (csType != g_lastCsType[s]) {
            g_lastCsType[s] = csType;
            g_dev->send(uf8::buildChannelStripType(static_cast<uint8_t>(s), csType));
        }

        // Top-zone label + LED (FF 66 .. 04 + cells 0x18..0x1F).
        // Label = soft-key bank's row N. LED = bright when this strip's
        // bank position holds the currently-focused param, dim otherwise.
        // (UF8 manual p.174 "soft-key label" + cap41 LED decode.)
        {
            const auto domSk = (focused.domain == uf8::Domain::BusComp)
                ? uf8::Domain::BusComp : uf8::Domain::ChannelStrip;
            const int bankSk = std::clamp(g_softKeyBank.load(),
                0, softkey::maxBankFor(domSk));
            const auto vSk = softkey::viewFor(domSk, bankSk);
            // Pad to 12 chars centred (leading + trailing spaces) so
            //  - shorter / empty labels actively overwrite any longer
            //    residue left in the LCD zone from the previous bank;
            //  - the firmware doesn't left-justify our padded output
            //    (which broke centring after the original space-pad
            //    fix that only added trailing spaces).
            // An empty payload (`FF 66 02 04 <strip>`) would flip the
            // strip into "slot empty" mode and darken the colour bar —
            // not what we want; we just want the label cleared.
            std::string label(vSk.labels[s]);
            if (label.size() < 12) {
                const size_t pad = 12 - label.size();
                const size_t lead = pad / 2;
                label = std::string(lead, ' ') + label
                      + std::string(pad - lead, ' ');
            }
            const int slotLink = vSk.linkIdx[s];
            // Synthetic toggle columns: read the per-strip state directly
            // (not the focused state) so each column's LED reflects the
            // toggle's actual on/off for THIS strip's track. Only ONE
            // strip per bank carries a synthetic, so at most one chunk
            // read per render tick — same cost as a normal param.
            bool isToggleCell = false;
            int  toggleOn = 0;
            if (slotLink == uf8::ext::TrackPhase) {
                isToggleCell = true;
                if (tr) {
                    toggleOn = (GetMediaTrackInfo_Value(tr, "B_PHASE") > 0.5) ? 1 : 0;
                }
            } else if (slotLink == uf8::ext::PluginAB) {
                isToggleCell = true;
                if (tr) {
                    int ab = -1, hq = -1;
                    uf8::readPluginToggleStates(tr, ab, hq);
                    toggleOn = (ab == 0) ? 1 : 0;  // bright = comparing (B active)
                }
            } else if (slotLink == uf8::ext::PluginHQ) {
                isToggleCell = true;
                if (tr) {
                    int ab = -1, hq = -1;
                    uf8::readPluginToggleStates(tr, ab, hq);
                    toggleOn = (hq == 1) ? 1 : 0;
                }
            }
            uf8::TopSoftKeyState tssk;
            int8_t ledCacheKey;
            if (isToggleCell) {
                tssk = toggleOn ? uf8::TopSoftKeyState::On
                                : uf8::TopSoftKeyState::Dim;
                ledCacheKey = static_cast<int8_t>(toggleOn ? 6 : 5);
            } else if (slotLink != softkey::kNoSlot && slotLink == focused.slotIdx) {
                tssk = uf8::TopSoftKeyState::On;         // bright = focused
                ledCacheKey = 2;
            } else {
                // All other strips — including kNoSlot positions — render
                // dim so the row stays visibly populated. Lighting only
                // strips with a wired slot would leave gaps the user
                // reads as "broken LEDs". Per user instruction 2026-04-30:
                // bright only when the parameter is selected.
                tssk = uf8::TopSoftKeyState::Dim;
                ledCacheKey = 1;
            }
            if (ledCacheKey != g_lastTopSoftKey[s]) {
                g_lastTopSoftKey[s] = ledCacheKey;
                // SSL-mode default: white. Two visible levels —
                // dim white (11 F1) for available / unbound, bright
                // white (FF FF) for focused. Settings UI will let the
                // user pick per-strip colours later.
                sendLedFrames(uf8::buildTopSoftKeyLed(
                    static_cast<uint8_t>(s), tssk, uf8::ledColourWhite()));
            }
            if (label != g_lastSlotLabel[s]) {
                g_lastSlotLabel[s] = label;
                g_dev->send(uf8::buildPluginSlotName(static_cast<uint8_t>(s), label));
            }
        }

        // Channel Number Zone — the tiny digit top-left of each strip's
        // color bar. REAPER track index is 0-based; UF8 expects 1-based
        // ASCII.
        {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "%d", realSlot + 1);
            std::string chan(buf);
            if (chan != g_lastChanNum[s]) {
                g_lastChanNum[s] = chan;
                g_dev->send(uf8::buildChannelNumber(static_cast<uint8_t>(s), chan));
            }
        }

        // FLIP active on this strip = global flip + a usable plug-in
        // slot. Without a slot there's no parameter to flip onto the
        // fader, so the strip falls back to normal mode silently.
        const bool flipActive = g_flip.load() && slot && fxIdx >= 0;

        // Plugin-fader mode active on this strip = global plugin-fader
        // toggle ON + a CS plug-in is loaded. FLIP wins if both are on
        // (FLIP is per-strip and explicit; plugin-fader is global).
        const auto cs = (g_pluginFaderMode.load() && !flipActive)
                          ? csFaderForTrack(tr) : CsFaderHandle{-1, -1};
        const bool csFaderActive = cs.vst3Param >= 0;

        // V-Pot Readout Bar position. Binary toggle slots render as
        // full (0xFF) or empty (0x00) — no in-between gradient; their
        // V-Pot push toggles the param (handled in PanCenter). Other
        // plugin params + pan render as a single-dot indicator at the
        // normalised position. The mode register (FF 66 09 0D, set
        // below) stays at 0x01 so the firmware draws a single line
        // instead of the linear-fill animation mode 0x02 produces.
        // Decision order (top wins):
        //   1. FLIP        → V-Pot mirrors track volume.
        //   2. forcePan    → V-Pot is REAPER track pan, period. Overrides
        //                    Plugin-mode and any focus, since the user
        //                    just pressed PAN to *demand* REAPER pan.
        //   3. focused slot resolves on this track → drive that param.
        //   4. focused but unavailable here → blank (collapsed bar).
        //   5. Plugin mode → SSL strip Pan (linkIdx 3).
        //   6. default     → REAPER track pan.
        if (flipActive) {
            // FLIP: V-Pot reads track volume. Map pb14 → 0..100 unipolar.
            const double volLinFlip = uiVolLinear(tr);
            const uint16_t pbVol = linearVolumeToPb(volLinFlip);
            vpotBar[s] = vpotPosFromUnipolar(
                static_cast<double>(pbVol) / 16383.0);
        } else if (g_forcePan.load()) {
            const double pan = GetMediaTrackInfo_Value(tr, "D_PAN");
            vpotBar[s] = vpotPosFromPan(pan);
        } else if (slot && fxIdx >= 0 && !isVPotPanFocus(focused)) {
            const double norm = TrackFX_GetParamNormalized(tr, fxIdx, slot->vst3Param);
            const double visual = slot->inverted ? 1.0 - norm : norm;
            if (isBinarySlot(*slot)) {
                // ON = max positive (0x7F in signed). OFF = 0x00 +
                // byte1=0x80 (centre marker = collapsed bar).
                vpotBar[s] = (norm >= 0.5)
                    ? static_cast<uint16_t>(0x7F)
                    : (uint16_t{0x00} | (uint16_t{0x80} << 8));
            } else if (isBipolarSlot(*slot)) {
                vpotBar[s] = vpotPosFromBipolar(visual * 2.0 - 1.0);
            } else {
                vpotBar[s] = vpotPosFromUnipolar(visual);
            }
        } else if (focused.slotIdx != -1 && !isVPotPanFocus(focused)) {
            // A param is focused but this strip's plug-in doesn't have
            // it (e.g. IMP IN focused while track hosts CS 2). Render
            // the V-Pot blank so the user isn't misled.
            vpotBar[s] = (uint16_t{0x00} | (uint16_t{0x80} << 8));
        } else if (g_pluginFaderMode.load()) {
            const auto pn = csPanForTrack(tr);
            if (pn.vst3Param >= 0) {
                const double norm = TrackFX_GetParamNormalized(
                    tr, pn.fxIndex, pn.vst3Param);
                vpotBar[s] = vpotPosFromBipolar(norm * 2.0 - 1.0);
            } else {
                const double pan = GetMediaTrackInfo_Value(tr, "D_PAN");
                vpotBar[s] = vpotPosFromPan(pan);
            }
        } else {
            const double pan = GetMediaTrackInfo_Value(tr, "D_PAN");
            vpotBar[s] = vpotPosFromPan(pan);
        }

        // Upper scribble row — REAPER track name (max 7 chars).
        {
            char name[256] = {0};
            GetSetMediaTrackInfo_String(tr, "P_NAME", name, false);
            std::string n(name);
            if (n.size() > 7) n.resize(7);
            if (n.empty()) {
                char fallback[8];
                std::snprintf(fallback, sizeof(fallback), "CH %d", realSlot + 1);
                n = fallback;
            }
            if (n != g_lastTrackName[s]) {
                g_lastTrackName[s] = n;
                g_dev->send(uf8::buildStripTextUpper(static_cast<uint8_t>(s), n));
            }
        }

        // O/PdB Fader Readout — normally track volume in dB. In FLIP
        // mode this zone shows the focused plug-in parameter's
        // formatted value (truncated to 6 chars to fit the zone), so
        // the fader has its own readout matching what it now drives.
        // In Plugin-Fader mode the readout shows the SSL strip's
        // internal Fader Level dB instead of REAPER's track volume.
        const double volLin = uiVolLinear(tr);
        std::string dbStr;
        if (flipActive || csFaderActive) {
            char paramBuf[64] = {0};
            const int useFx = flipActive ? fxIdx : cs.fxIndex;
            const int useParam = flipActive ? slot->vst3Param : cs.vst3Param;
            const double norm = TrackFX_GetParamNormalized(tr, useFx, useParam);
            TrackFX_FormatParamValueNormalized(tr, useFx, useParam,
                                               norm, paramBuf, sizeof(paramBuf));
            std::string s2(paramBuf);
            // Squash UTF-8 ∞ then non-printable, trim leading spaces — same
            // sanitisation as the value-line path so the LCD stays clean.
            for (size_t p = 0; p + 2 < s2.size(); ) {
                if (static_cast<unsigned char>(s2[p])     == 0xE2 &&
                    static_cast<unsigned char>(s2[p + 1]) == 0x88 &&
                    static_cast<unsigned char>(s2[p + 2]) == 0x9E) {
                    s2.replace(p, 3, "INF"); p += 3;
                } else ++p;
            }
            for (auto& c : s2) {
                const unsigned char u = static_cast<unsigned char>(c);
                if (u < 0x20 || u > 0x7E) c = '-';
            }
            while (!s2.empty() && s2.front() == ' ') s2.erase(0, 1);
            // Drop space between number and unit so "16.00 KHz" → "16.00KHz".
            for (size_t p = s2.size(); p > 1; --p) {
                if (s2[p - 1] != ' ') continue;
                const char prev = s2[p - 2];
                if ((prev >= '0' && prev <= '9') || prev == '.') s2.erase(p - 1, 1);
                break;
            }
            // dB readout zone has "dB" baked into the protocol frame
            // (Protocol.cpp:378). SSL's own formatter returns "-0.6 dB"
            // → space-strip leaves "-0.6dB" → frame appends "dB" → the
            // LCD renders "-0.6dBdB". Strip a trailing dB suffix so the
            // CS Fader value lines up with REAPER-volume formatting (no
            // unit; the frame supplies it).
            if (s2.size() >= 2) {
                const char a = s2[s2.size() - 2];
                const char b = s2[s2.size() - 1];
                if ((a == 'd' || a == 'D') && (b == 'B' || b == 'b')) {
                    s2.erase(s2.size() - 2);
                    while (!s2.empty() && s2.back() == ' ') s2.pop_back();
                }
            }
            if (s2.size() > 6) s2.resize(6);
            dbStr = s2;
        } else {
            dbStr = formatDbReadout(volLin);
        }
        if (dbStr != g_lastFaderDb[s]) {
            g_lastFaderDb[s] = dbStr;
            g_dev->send(uf8::buildFaderDbReadout(static_cast<uint8_t>(s), dbStr));
        }

        // Motor echo: push the fader target every tick — but NOT while
        // the strip is touch-reported. Position commands during touch
        // re-engage the motor against the user's hand: the user moves
        // the fader, drainInputQueue applies the new volume, and the
        // very next tick reads the new volume back, sees pb changed,
        // and sends a fresh fader-position frame. Firmware treats that
        // as "drive to target" — not the limp-target-update we'd hoped
        // for. commitDebouncedTouchReleases sends the authoritative
        // position multiple times right after motor-enable, so we lose
        // nothing by staying silent during touch.
        //
        // In FLIP mode the fader target is the focused parameter's
        // normalised position (0..1) scaled to pb14, so the motor
        // travels to where the plug-in param actually is.
        if (!g_touchReported[s].load()) {
            uint16_t pb;
            if (flipActive || csFaderActive) {
                const int useFx = flipActive ? fxIdx : cs.fxIndex;
                const int useParam = flipActive ? slot->vst3Param : cs.vst3Param;
                const bool inverted = flipActive ? slot->inverted : false;
                const double norm = TrackFX_GetParamNormalized(tr, useFx, useParam);
                const double v = inverted ? 1.0 - norm : norm;
                // Scale to the actual hardware fader top (kUf8FaderPbMax)
                // so a fully-right param parks the motor at mechanical top
                // exactly. 16383 here would aim past the hardware deadband.
                int p14 = static_cast<int>(std::round(
                    v * static_cast<double>(kUf8FaderPbMax)));
                if (p14 < 0)              p14 = 0;
                if (p14 > kUf8FaderPbMax) p14 = kUf8FaderPbMax;
                pb = static_cast<uint16_t>(p14);
            } else {
                pb = linearVolumeToPb(volLin);
            }
            if (!g_faderPbInit || pb != g_lastFaderPb[s]) {
                g_lastFaderPb[s] = pb;
                const uint8_t lsb = static_cast<uint8_t>(pb & 0x7F);
                const uint8_t msb = static_cast<uint8_t>((pb >> 7) & 0x7F);
                g_dev->send(uf8::buildFaderPosition(static_cast<uint8_t>(s), lsb, msb));
            }
        }

        // Value Line — for SSL plug-ins: slot name + formatted param value
        // ("HF Freq    8.00kHz"). Otherwise: track volume ("Vol   -6.0dB").
        // In FLIP mode the fader is driving the parameter, so we show
        // track volume here — the data the V-Pot is now driving.
        std::string valLine;
        const bool synthFocused = (focused.domain == uf8::Domain::ChannelStrip)
            && (focused.slotIdx == uf8::ext::TrackPhase
             || focused.slotIdx == uf8::ext::PluginAB
             || focused.slotIdx == uf8::ext::PluginHQ);
        if (flipActive) {
            valLine = composeValueLine("Vol", formatDbReadout(volLin));
        } else if (g_forcePan.load()) {
            // forcePan overrides Plugin mode + focus. Pure REAPER pan.
            const double pan = GetMediaTrackInfo_Value(tr, "D_PAN");
            valLine = composeValueLine("Pan", formatPanReadout(pan));
        } else if (synthFocused) {
            // Synthetic toggle focused: render this strip's own state.
            // No VST3 param to format — read directly from REAPER /
            // SSL plug-in chunk. tr was guaranteed non-null above
            // (empty-strip block returned earlier).
            if (focused.slotIdx == uf8::ext::TrackPhase) {
                const bool on = GetMediaTrackInfo_Value(tr, "B_PHASE") > 0.5;
                valLine = composeValueLine("Phase", on ? "INV" : "OFF");
            } else {
                int ab = -1, hq = -1;
                uf8::readPluginToggleStates(tr, ab, hq);
                if (focused.slotIdx == uf8::ext::PluginAB) {
                    const char* v = (ab == 1) ? "A" : (ab == 0) ? "B" : "-";
                    valLine = composeValueLine("A/B", v);
                } else { // PluginHQ
                    const char* v = (hq == 1) ? "ON" : (hq == 0) ? "OFF" : "-";
                    valLine = composeValueLine("HQ", v);
                }
            }
        } else if (slot && fxIdx >= 0 && !isVPotPanFocus(focused)) {
            char paramBuf[64] = {0};
            const double norm = TrackFX_GetParamNormalized(tr, fxIdx, slot->vst3Param);
            TrackFX_FormatParamValueNormalized(tr, fxIdx, slot->vst3Param,
                                               norm, paramBuf, sizeof(paramBuf));
            std::string valStr(paramBuf);
            // Replace UTF-8 ∞ (E2 88 9E) with "INF" before the non-ASCII
            // squash — Comp Ratio at max returns "∞:1", which would
            // otherwise become "---:1" hieroglyphs on the LCD.
            for (size_t p = 0; p + 2 < valStr.size(); ) {
                if (static_cast<unsigned char>(valStr[p])     == 0xE2 &&
                    static_cast<unsigned char>(valStr[p + 1]) == 0x88 &&
                    static_cast<unsigned char>(valStr[p + 2]) == 0x9E) {
                    valStr.replace(p, 3, "INF");
                    p += 3;
                } else {
                    ++p;
                }
            }
            // Squash any remaining non-printable-ASCII so character count
            // and cursor positions stay correct on the LCD.
            for (auto& c : valStr) {
                const unsigned char u = static_cast<unsigned char>(c);
                if (u < 0x20 || u > 0x7E) c = '-';
            }
            while (!valStr.empty() && valStr.front() == ' ') valStr.erase(0, 1);
            // Non-numeric value (e.g. "OUT Hz" when LP/HP filter is bypassed,
            // "Off Hz") — drop the unit suffix so the LCD reads just "OUT".
            if (!valStr.empty()) {
                const char first = valStr.front();
                const bool numeric = (first >= '0' && first <= '9')
                                  || first == '-' || first == '+' || first == '.';
                if (!numeric) {
                    const size_t sp = valStr.find(' ');
                    if (sp != std::string::npos) valStr.erase(sp);
                }
            }
            // Strip the space between numeric value and unit suffix so
            // "16.00 KHz" → "16.00KHz" (saves one char so the leading
            // digit doesn't get clipped by the LCD value zone). Match
            // both lower- and upper-case unit variants — 4K E returns
            // "KHz" with capital K, CS 2 uses lowercase. Generic walk:
            // find the rightmost space whose preceding char is a digit
            // or '.' and erase it.
            for (size_t p = valStr.size(); p > 1; --p) {
                if (valStr[p - 1] != ' ') continue;
                const char prev = valStr[p - 2];
                if ((prev >= '0' && prev <= '9') || prev == '.') {
                    valStr.erase(p - 1, 1);
                }
                break;
            }
            valLine = composeValueLine(slot->name, valStr);
        } else if (focused.slotIdx != -1 && !isVPotPanFocus(focused)) {
            // Param is focused but unavailable on this strip's plug-in
            // — leave the Value Line blank instead of falling back to
            // Pan, which would mislead the user into thinking the
            // V-Pot controls something on this strip.
            valLine = std::string(19, ' ');
        } else if (g_pluginFaderMode.load()) {
            // Plugin mode → show the SSL strip's own Pan instead of
            // REAPER track pan. Plug-in pan is normalised 0..1 with
            // 0.5 = centre; convert to REAPER's -1..+1 for the
            // existing formatPanReadout helper. Falls back to track
            // pan when there's no CS plug-in on the track.
            const auto pn = csPanForTrack(tr);
            if (pn.vst3Param >= 0) {
                const double norm = TrackFX_GetParamNormalized(
                    tr, pn.fxIndex, pn.vst3Param);
                valLine = composeValueLine("Pan",
                    formatPanReadout(norm * 2.0 - 1.0));
            } else {
                const double pan = GetMediaTrackInfo_Value(tr, "D_PAN");
                valLine = composeValueLine("Pan", formatPanReadout(pan));
            }
        } else {
            // Nothing focused → V-Pot controls Pan; reflect that in
            // the Value Line. Fader dB stays in the dedicated O/PdB
            // zone above, so we don't need to repeat volume here.
            const double pan = GetMediaTrackInfo_Value(tr, "D_PAN");
            valLine = composeValueLine("Pan", formatPanReadout(pan));
        }
        if (valLine != g_lastValueLine[s]) {
            g_lastValueLine[s] = valLine;
            g_dev->send(uf8::buildValueLine(static_cast<uint8_t>(s), valLine));
        }
    }
    g_faderPbInit = true;

    // V-Pot Readout Bar — per cap37, SSL360 sends position (FF 66 11 0F)
    // FIRST, then mode (FF 66 09 0D) ~55 µs later. Reverse order with
    // mismatched encoding makes the firmware render the bar wrong.
    //
    // Mode register is init-/transition-only (sent when going inactive
    // → active, never re-asserted per tick). Per-strip mode:
    //   0x08 = bipolar centre-out (Gain/Pan/Trim — cap37 mode register)
    //   0x01 = unipolar L→R full sweep (Freq/Q/Threshold — cap15 mode
    //          register `01 01 01 01 03 03 03 03` with byte0=0..0x64)
    //   0x03 = empty / disabled (no track in bank, or binary toggle)
    std::array<uint8_t, 8> vpotMode{};
    {
        const int trackCount = CountTracks(nullptr);
        const auto focused = uf8::getFocusedParam();
        for (uint8_t s = 0; s < 8; ++s) {
            const int realSlot = static_cast<int>(s) + g_bankOffset.load();
            if (realSlot >= trackCount) {
                vpotMode[s] = 0x03;
                continue;
            }
            MediaTrack* tr = GetTrack(nullptr, realSlot);
            int fxIdx = -1;
            const uf8::LinkSlot* slot = slotForStrip(tr, focused, &fxIdx);
            // Pan-focus is treated as no-V-Pot-focus by the position +
            // value-line render branches (defers to the Plugin/forcePan/
            // REAPER pan tree, which is bipolar centre-out). Mode register
            // must agree — otherwise the firmware renders the bipolar
            // centre encoding (byte0=0x00, byte1=0x80) as left-edge in
            // unipolar mode 0x01.
            if (slot && isVPotPanFocus(focused)) slot = nullptr;
            const bool flipHere = g_flip.load() && slot && fxIdx >= 0;
            if (flipHere) {
                vpotMode[s] = 0x01;  // FLIP: V-Pot = volume (unipolar)
            } else if (!slot) {
                vpotMode[s] = 0x08;  // pan fallback — bipolar centre
            } else if (isBinarySlot(*slot)) {
                vpotMode[s] = 0x03;  // binary — no bar
            } else if (isBipolarSlot(*slot)) {
                vpotMode[s] = 0x08;  // bipolar centre-out
            } else {
                vpotMode[s] = 0x01;  // unipolar L→R
            }
        }
    }
    bool modeChanged = !g_vpotBarInit;
    if (!modeChanged) {
        for (uint8_t s = 0; s < 8; ++s) {
            if (vpotMode[s] != g_lastVPotMode[s]) { modeChanged = true; break; }
        }
    }
    bool barChanged = false;
    for (uint8_t s = 0; s < 8; ++s) {
        if (vpotBar[s] != g_lastVPotBar[s]) { barChanged = true; break; }
    }
    // Position FIRST per cap37 ordering.
    if (barChanged) {
        g_lastVPotBar = vpotBar;
        g_dev->send(uf8::buildVPotReadoutBar(vpotBar));
    }
    if (modeChanged) {
        g_lastVPotMode = vpotMode;
        std::vector<uint8_t> mf{0xFF, 0x66, 0x09, 0x0D};
        for (auto m : vpotMode) mf.push_back(m);
        uint8_t cks = 0;
        for (size_t i = 1; i < mf.size(); ++i) cks += mf[i];
        mf.push_back(cks);
        g_dev->send(std::move(mf));
        g_vpotBarInit = true;
    }
}

// Mouse-edits in the SSL plug-in GUI: REAPER tracks the most recently
// touched (track, fx, param) tuple via GetLastTouchedFX. Polling it on
// each timer tick lets UF8 + UC1 chase plugin-GUI moves without the
// user needing to first nudge a knob on UC1 / V-Pot on UF8.
//
// Dedup against the previous tuple so we don't pay map lookups every
// tick when the user is idle. Static state is fine: timer is the
// single caller, all on the main thread.
//
// Caveats handled inline:
//   - master track (trWord low word == 0): skip
//   - take-FX (high word of trWord nonzero): skip
//   - record-FX (bit 24 of fxWord set): skip
// Anything we don't understand falls through silently — chase is
// best-effort.
void chaseLastTouchedFx()
{
    int trWord = -1, fxWord = -1, paramIdx = -1;
    if (!GetLastTouchedFX(&trWord, &fxWord, &paramIdx)) return;
    static int lastTr = -2, lastFx = -2, lastParam = -2;
    if (trWord == lastTr && fxWord == lastFx && paramIdx == lastParam) return;
    lastTr = trWord; lastFx = fxWord; lastParam = paramIdx;

    if ((trWord & 0xFFFF0000) != 0) return;        // take-FX
    const int trLow = trWord & 0xFFFF;
    if (trLow <= 0) return;                        // master / invalid
    MediaTrack* tr = GetTrack(nullptr, trLow - 1);
    if (!tr) return;

    if ((fxWord >> 24) & 0x01) return;             // record-FX
    const int fxIdx = fxWord & 0x00FFFFFF;

    char fxName[512] = {0};
    TrackFX_GetFXName(tr, fxIdx, fxName, sizeof(fxName));
    const uf8::PluginMap* map = uf8::lookupPluginMapByName(fxName);
    if (!map) return;

    const int linkIdx = uf8::slotIdxForVst3Param(*map, paramIdx);
    if (linkIdx < 0) return;

    uf8::setFocus({map->domain, linkIdx});
    if (g_uc1_surface) g_uc1_surface->setFocusedTrack(tr);
}

void commitDebouncedTouchReleases()
{
    const auto now = std::chrono::steady_clock::now();
    for (uint8_t s = 0; s < 8; ++s) {
        if (!g_touchReleasePending[s].load()) continue;
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - g_touchLastPress[s]).count();
        if (elapsed < kTouchDebounceQuiet.count()) continue;
        g_touchReleasePending[s].store(false);
        const bool wasReported = g_touchReported[s].exchange(false);
        if (!wasReported) continue;

        if (!g_dev) continue;
        MediaTrack* tr = g_slotTrack[s];
        if (!tr || !ValidatePtr2(nullptr, tr, "MediaTrack*")) continue;

        // Snap REAPER (or the active plug-in target) to the user's last
        // raw fader position (regardless of the >=4-LSB deadband). The
        // fader drives:
        //   - focused param  in FLIP mode (when the strip has a slot),
        //   - SSL CS Fader   in Plugin-Fader mode,
        //   - track volume   otherwise.
        if (g_lastTouchPbValid[s].load()) {
            const uint16_t touchPb = g_lastTouchPb[s].load();
            const auto focusedT = uf8::getFocusedParam();
            auto mmT = uf8::lookupPluginOnTrack(tr, focusedT.domain);
            const uf8::LinkSlot* slT = (!g_forcePan.load() && mmT.map)
                ? uf8::findSlotByLinkIdx(*mmT.map, focusedT.slotIdx)
                : nullptr;
            if (isVPotPanFocus(focusedT)) slT = nullptr;
            const auto csT = csFaderForTrack(tr);
            if (g_flip.load() && slT) {
                double normT = static_cast<double>(touchPb) /
                               static_cast<double>(kUf8FaderPbMax);
                if (slT->inverted) normT = 1.0 - normT;
                if (normT < 0.0) normT = 0.0;
                if (normT > 1.0) normT = 1.0;
                TrackFX_SetParamNormalized(tr, mmT.fxIndex,
                    slT->vst3Param, normT);
            } else if (g_pluginFaderMode.load() && csT.vst3Param >= 0) {
                double n = static_cast<double>(touchPb) /
                           static_cast<double>(kUf8FaderPbMax);
                if (n < 0.0) n = 0.0;
                if (n > 1.0) n = 1.0;
                TrackFX_SetParamNormalized(tr, csT.fxIndex,
                    csT.vst3Param, n);
            } else {
                CSurf_OnVolumeChange(tr, pbToLinearVolume(touchPb), false);
            }
            g_lastTouchPbValid[s].store(false);
        }

        // Re-engage the motor. The firmware's target buffer is ALREADY
        // pointing at the user's final touch position thanks to the
        // bit-7-set echoes we sent throughout the touch (see FF 21 03
        // handler in onUf8Input). So FF 1D 02 strip 01 engages the
        // motor at the correct target — no jerk to a stale pre-touch
        // value. Without this enable the firmware stays limp and
        // ignores subsequent FF 1E motor-echo commands.
        //
        // For the cached pb, mirror the active fader source so the next
        // dedup tick doesn't re-push a stale REAPER-volume position
        // when Plugin mode is on.
        uint16_t pb = linearVolumeToPb(uiVolLinear(tr));
        if (g_pluginFaderMode.load()) {
            const auto cs = csFaderForTrack(tr);
            if (cs.vst3Param >= 0) {
                const double norm = TrackFX_GetParamNormalized(
                    tr, cs.fxIndex, cs.vst3Param);
                int p14 = static_cast<int>(std::round(
                    norm * static_cast<double>(kUf8FaderPbMax)));
                if (p14 < 0) p14 = 0;
                if (p14 > kUf8FaderPbMax) p14 = kUf8FaderPbMax;
                pb = static_cast<uint16_t>(p14);
            }
        }
        g_dev->send(uf8::buildMotorEnable(s, true));
        g_lastFaderPb[s] = pb;
    }
}

// Linear peak (0..1) → UF8 VU byte (0..31). -55 dBFS → 0, 0 dBFS → 31.
// The cutoff is intentionally above -60 dB: REAPER's track-peak ballistics
// have a slow decay tail that drifts through -60..-55 even on silent
// tracks, which would otherwise flicker the bottom LED. Snapping anything
// below -55 to 0 gives a clean noise-floor.
uint8_t peakToVuByte(double peak)
{
    if (peak <= 0.0) return 0;
    const double dbfs = 20.0 * std::log10(peak);
    if (dbfs >= 0.0)   return 0x1F;
    if (dbfs <= -55.0) return 0x00;
    const double f = (dbfs + 55.0) / 55.0;
    const int byte = static_cast<int>(f * 31.0 + 0.5);
    return static_cast<uint8_t>(std::clamp(byte, 0, 0x1F));
}

std::array<uint8_t, 16> g_lastVuLevels{};
bool g_vuInit = false;
std::chrono::steady_clock::time_point g_lastVuPushTime{};

// UF8 GR bytes — one per visible strip. Carried by the FF 66 09 15
// heartbeat at offsets 4..11 (strip 1 → byte 4, strip 8 → byte 11).
// Byte 0x00 = no LEDs lit (true rest); ramps up monotonically to ~0x18
// at full GR. Only the strip that hosts the focused CS plug-in carries
// nonzero values; other strips stay at 0x00 (cleared each tick).
std::array<uint8_t, 8> g_uf8GrBytes{};

void pushVuMeter()
{
    if (!g_dev || !g_dev->isOpen()) return;

    const int trackCount = CountTracks(nullptr);
    const int bankOffset = g_bankOffset.load();
    std::array<uint8_t, 16> levels{};

    // Byte-level hysteresis per strip × channel. REAPER's peak ballistics
    // produce 1-byte drift on continuous audio (block boundaries don't
    // align with the cycle), which flickers the LED at the boundary
    // between two byte values. Up moves go through immediately; a drop
    // is only accepted if the new byte is at least 2 below the held byte.
    static std::array<uint8_t, 16> s_held{};
    auto stepByte = [](uint8_t& held, uint8_t raw) {
        if (raw > held) held = raw;
        else if (held - raw >= 2) held = raw;
        // else: hold previous
        return held;
    };

    for (int s = 0; s < 8; ++s) {
        const int idx = s + bankOffset;
        uint8_t rawL = 0, rawR = 0;
        if (idx < trackCount) {
            if (MediaTrack* tr = GetTrack(nullptr, idx)) {
                // Left = channel 0, right = channel 1. REAPER's peak is
                // the channel's post-fader tap; pre-fader VU isn't
                // exposed via this call, so "in" and "out" end up
                // mirroring each other for mono fader moves. Good
                // enough until a JSFX probe exposes pre-fader.
                rawL = peakToVuByte(Track_GetPeakInfo(tr, 0));
                rawR = peakToVuByte(Track_GetPeakInfo(tr, 1));
            }
        }
        levels[s * 2 + 0] = stepByte(s_held[s * 2 + 0], rawL);  // "input"
        levels[s * 2 + 1] = stepByte(s_held[s * 2 + 1], rawR);  // "output"
    }
    // Throttle: peak bytes are 0..31, so a single audio sample drift can
    // toggle one byte every tick → 30 Hz of OUT frames during playback,
    // saturating the queue and pushing latency-sensitive frames (motor-
    // limp on touch press) hundreds of ms behind. Push only when either
    //   (a) some byte moved by ≥ 2 (suppresses jitter), OR
    //   (b) ≥ 50 ms passed AND any byte changed at all (so slow decays
    //       still update smoothly without flooding).
    if (g_vuInit) {
        if (levels == g_lastVuLevels) return;
        uint8_t maxDelta = 0;
        for (size_t i = 0; i < levels.size(); ++i) {
            const uint8_t d = levels[i] > g_lastVuLevels[i]
                            ? levels[i] - g_lastVuLevels[i]
                            : g_lastVuLevels[i] - levels[i];
            if (d > maxDelta) maxDelta = d;
        }
        const auto now = std::chrono::steady_clock::now();
        if (maxDelta < 2 && now - g_lastVuPushTime < std::chrono::milliseconds(50)) {
            return;
        }
        g_lastVuPushTime = now;
    } else {
        g_lastVuPushTime = std::chrono::steady_clock::now();
    }
    g_vuInit = true;
    g_lastVuLevels = levels;
    auto frames = uf8::buildVuMeter(levels);
    g_dev->send(std::move(frames[0]));
    g_dev->send(std::move(frames[1]));
}

void pushSelColourBar()
{
    if (!g_dev || !g_dev->isOpen()) return;

    const int trackCount = CountTracks(nullptr);
    const int bankOffset = g_bankOffset.load();

    // Determine each visible strip's selection state + compute the mask
    // for the selected strip. Empty slots (past trackCount) default to
    // unselected. The 16-bit mask has bit (strip+1) set for the
    // currently-selected strip (T1=0x02, T2=0x04, …, T8=0x0100).
    uint16_t mask = 0;
    for (int s = 0; s < 8; ++s) {
        const int idx = s + bankOffset;
        MediaTrack* tr = (idx < trackCount) ? GetTrack(nullptr, idx) : nullptr;
        const bool sel = tr && GetMediaTrackInfo_Value(tr, "I_SELECTED") > 0.5;
        const uint8_t target = sel ? 0xFF : 0x00;
        if (sel) mask |= static_cast<uint16_t>(1 << s);
        if (g_lastSelBright[s] != target) {
            g_lastSelBright[s] = target;
            // White-mode LED update. DAW-Colour byte encoding is still
            // partial; white mode is visually correct even without the
            // per-track palette-to-bytes decode, since unselected=dim
            // and selected=bright matches SSL's default behaviour.
            auto frames = uf8::buildSelWhite(static_cast<uint8_t>(s), sel);
            if (!frames[0].empty()) g_dev->send(std::move(frames[0]));
            if (!frames[1].empty()) g_dev->send(std::move(frames[1]));
        }
    }
    if (mask != g_lastSelMask) {
        g_lastSelMask = mask;
        g_dev->send(uf8::buildSelectedStripMask(mask));
    }
}

// UF8 global-button LED state. We only drive the LEDs that map cleanly
// to REAPER state we can read on every tick: the Automation mode of the
// selected track (Read/Write/Trim/Latch/Touch — radio group), and the
// global Rec/ALL indicator (lit when any track is rec-armed).
//
// The other ~25 global LEDs (Layer/Soft/Modifier/Zoom rows) are not
// wired up yet — they need surface-internal mode tracking which the
// extension doesn't currently model. Cell map for those is in
// docs/uf8-global-led-map.md and Protocol::buildUf8GlobalLed when we
// get to it.
int g_lastAutoMode = -2;          // -1 = no track, 0..5 = REAPER auto modes
bool g_lastAnyArmed = false;
bool g_lastForcePan = false;
bool g_lastFlip = false;
int  g_lastSoftKeyBank = -1;
bool g_lastShiftHeld = false;
EncoderMode g_lastEncoderMode = EncoderMode::Nav;
int  g_lastPageLeftLit  = -1;     // -1 = unknown / 0 = off / 1 = on
int  g_lastPageRightLit = -1;
int  g_lastPluginLit    = -1;     // -1 = unknown / 0 = dim / 1 = bright (mode)
int  g_lastDomainLed    = -1;     // -1 = unknown, 0 = CS, 1 = BC
bool g_globalLedsInit = false;

// Map REAPER's automation-mode integer to a position in kAutoLeds.
//   0 = Trim/Read (REAPER's per-track default; reads automation, no
//                  punch-in writing) → light the dedicated Trim LED.
//   1 = Read     → AutoRead.
//   2 = Touch    → AutoTouch.
//   3 = Write    → AutoWrite.
//   4 = Latch / 5 = Latch Preview → AutoLatch.
constexpr uf8::Uf8GlobalLed kAutoLeds[5] = {
    uf8::Uf8GlobalLed::AutoRead,
    uf8::Uf8GlobalLed::AutoWrite,
    uf8::Uf8GlobalLed::AutoTrim,
    uf8::Uf8GlobalLed::AutoLatch,
    uf8::Uf8GlobalLed::AutoTouch,
};
int autoModeToLedIndex(int mode)
{
    switch (mode) {
        case 0:         return 2;   // Trim/Read → Trim LED
        case 1:         return 0;   // Read
        case 2:         return 4;   // Touch
        case 3:         return 1;   // Write
        case 4: case 5: return 3;   // Latch / Latch Preview
        default:        return -1;  // No selection — clear all
    }
}

// Push the 5-button Auto LED state for `mode`. Used both by the periodic
// LED refresh in pushUf8GlobalLeds and by the press handler — pre-empting
// the firmware's auto-button "transition flash" through TRIM that would
// otherwise be visible during the ~33 ms gap before our next tick reads
// the new mode back from REAPER.
void pushAutoModeLeds(int mode)
{
    if (!g_dev || !g_dev->isOpen()) return;
    const int active = autoModeToLedIndex(mode);
    for (int i = 0; i < 5; ++i) {
        sendLedFrames(uf8::buildUf8GlobalLed(kAutoLeds[i], i == active));
    }
    g_lastAutoMode = mode;
}

void pushUf8GlobalLeds()
{
    if (!g_dev || !g_dev->isOpen()) return;

    // Automation mode of the focused/selected track. REAPER values:
    //   0 = Trim/Read, 1 = Read, 2 = Touch, 3 = Write, 4 = Latch,
    //   5 = Latch Preview.
    int autoMode = -1;
    MediaTrack* sel = GetSelectedTrack(nullptr, 0);
    if (sel) autoMode = GetTrackAutomationMode(sel);

    // Any armed track in the project — drives the global Rec LED.
    bool anyArmed = false;
    const int n = CountTracks(nullptr);
    for (int i = 0; i < n; ++i) {
        if (GetMediaTrackInfo_Value(GetTrack(nullptr, i), "I_RECARM") > 0.5) {
            anyArmed = true; break;
        }
    }

    const bool forcePan        = g_forcePan.load();
    const bool flip            = g_flip.load();
    const bool shiftHeld       = g_shiftHeld.load();
    const EncoderMode encMode  = g_encoderMode.load();
    const int  softKeyBank     = g_softKeyBank.load();
    const int  domainLed       = (uf8::getFocusedParam().domain == uf8::Domain::BusComp)
                                     ? 1 : 0;

    // Page Left LED — confirmed via probe 2026-04-30 to live at cell
    // 0x2D (NOT 0x5D as cap35/36 originally suggested — that earlier
    // assumption is what caused the "2 buttons selected" surface bug
    // in 2026-04-28; 0x5D actually lights Soft 2). Page navigates the
    // soft-key bank with wrap-around so the LED stays lit always while
    // the extension is active. Page Right LED still unwired — its
    // real cell hasn't been discovered (0x5C is suspect).

    // Plugin button: dim while extension is active, "bright" (= our
    // function's on=true bytes) when push-mode is on. cap44 only ever
    // observed off + dim for cell 0x2F so the bright variant might be
    // visually identical to dim — user asked for both states regardless.
    const int pluginLit = g_pluginFaderMode.load() ? 1 : 0;

    if (g_globalLedsInit && autoMode == g_lastAutoMode &&
        anyArmed == g_lastAnyArmed && forcePan == g_lastForcePan &&
        flip == g_lastFlip && shiftHeld == g_lastShiftHeld &&
        encMode == g_lastEncoderMode && softKeyBank == g_lastSoftKeyBank &&
        pluginLit == g_lastPluginLit && domainLed == g_lastDomainLed) {
        return;
    }

    if (autoMode != g_lastAutoMode || !g_globalLedsInit) {
        pushAutoModeLeds(autoMode);
    }

    if (anyArmed != g_lastAnyArmed || !g_globalLedsInit) {
        sendLedFrames(uf8::buildUf8GlobalLed(uf8::Uf8GlobalLed::Rec, anyArmed));
        g_lastAnyArmed = anyArmed;
    }

    // Pan LED tracks the global "force all V-Pots to Pan" toggle so the
    // hardware shows the active mode at a glance.
    if (forcePan != g_lastForcePan || !g_globalLedsInit) {
        sendLedFrames(uf8::buildUf8GlobalLed(uf8::Uf8GlobalLed::Pan, forcePan));
        g_lastForcePan = forcePan;
    }

    // FLIP LED indicates fader↔V-Pot swap is active.
    if (flip != g_lastFlip || !g_globalLedsInit) {
        sendLedFrames(uf8::buildUf8GlobalLed(uf8::Uf8GlobalLed::Flip, flip));
        g_lastFlip = flip;
    }

    // Shift/Fine LED — momentary, follows the held state of 0x6F.
    if (shiftHeld != g_lastShiftHeld || !g_globalLedsInit) {
        sendLedFrames(uf8::buildUf8GlobalLed(uf8::Uf8GlobalLed::Fine, shiftHeld));
        g_lastShiftHeld = shiftHeld;
    }

    // Soft-key bank LEDs — exactly one of V-POT / Soft 1..5 is lit
    // matching g_softKeyBank (0..5). Single dirty-check rewrites all 6
    // since the user expects a clean radio-button look on every change.
    if (softKeyBank != g_lastSoftKeyBank || !g_globalLedsInit) {
        sendLedFrames(uf8::buildUf8GlobalLed(uf8::Uf8GlobalLed::VPotBank, softKeyBank == 0));
        sendLedFrames(uf8::buildUf8GlobalLed(uf8::Uf8GlobalLed::Soft1,    softKeyBank == 1));
        sendLedFrames(uf8::buildUf8GlobalLed(uf8::Uf8GlobalLed::Soft2,    softKeyBank == 2));
        sendLedFrames(uf8::buildUf8GlobalLed(uf8::Uf8GlobalLed::Soft3,    softKeyBank == 3));
        sendLedFrames(uf8::buildUf8GlobalLed(uf8::Uf8GlobalLed::Soft4,    softKeyBank == 4));
        sendLedFrames(uf8::buildUf8GlobalLed(uf8::Uf8GlobalLed::Soft5,    softKeyBank == 5));
        g_lastSoftKeyBank = softKeyBank;
    }

    // Page LEDs intentionally not driven — see comment at the top of
    // pushUf8GlobalLeds explaining the cell collision with Soft 2/3.

    // Plugin button — dim baseline when extension is active, bright
    // variant when fader-push mode is on. buildUf8GlobalLed(.., false)
    // emits 11 F1 (= dim, the only "on" state cap44 observed for cell
    // 0x2F). buildUf8GlobalLed(.., true) emits FF FF — hardware may or
    // may not render that as bright; cap44 didn't catch it. Both modes
    // wired so future hardware revisions / plugin-mixer-mode contexts
    // get visual differentiation if they support it.
    if (pluginLit != g_lastPluginLit || !g_globalLedsInit) {
        sendLedFrames(uf8::buildUf8GlobalLed(uf8::Uf8GlobalLed::Plugin,
                                             pluginLit == 1));
        g_lastPluginLit = pluginLit;
    }

    // Quick 1 / Quick 2 LEDs — radio group reflecting the focused-param
    // domain. Quick 1 bright = ChannelStrip, Quick 2 bright = BusComp.
    // Quick 3 (= I/O meter toggle, not yet wired) stays init-dim.
    // Cells confirmed via probe 2026-04-30: Q1 0x3C, Q2 0x3B, Q3 0x3A.
    if (domainLed != g_lastDomainLed || !g_globalLedsInit) {
        sendLedFrames(uf8::buildUf8GlobalLed(uf8::Uf8GlobalLed::Quick1, domainLed == 0));
        sendLedFrames(uf8::buildUf8GlobalLed(uf8::Uf8GlobalLed::Quick2, domainLed == 1));
        if (!g_globalLedsInit) {
            sendLedFrames(uf8::buildUf8GlobalLed(uf8::Uf8GlobalLed::Quick3, false));
        }
        g_lastDomainLed = domainLed;
    }

    // Init-dim LEDs whose state we don't drive yet — without these,
    // anything left bright by the probe action (or by SSL360 before
    // Rea-Sixty took the device) lingers across reloads. All shipped
    // dim baseline so the user sees the row exists; they go bright
    // when actively used (press feedback) or when bound via Settings.
    if (!g_globalLedsInit) {
        for (auto led : {
            uf8::Uf8GlobalLed::Layer1, uf8::Uf8GlobalLed::Layer2,
            uf8::Uf8GlobalLed::Layer3, uf8::Uf8GlobalLed::Btn360,
            uf8::Uf8GlobalLed::Channel,
            uf8::Uf8GlobalLed::AutoOff, uf8::Uf8GlobalLed::Auto,
            uf8::Uf8GlobalLed::Norm,
            uf8::Uf8GlobalLed::BankLeft, uf8::Uf8GlobalLed::BankRight,
            uf8::Uf8GlobalLed::ZoomUp, uf8::Uf8GlobalLed::ZoomDown,
            uf8::Uf8GlobalLed::ZoomLeft, uf8::Uf8GlobalLed::ZoomRight,
            uf8::Uf8GlobalLed::ZoomCenter,
        }) {
            sendLedFrames(uf8::buildUf8GlobalLed(led, false));
        }
        // Send/Plugin 1..8 ship dim baseline. Settings UI will let the
        // user assign actions per button — at runtime the LED of the
        // currently-active assignment goes bright as a radio.
        for (auto led : {
            uf8::Uf8GlobalLed::SendPlugin1, uf8::Uf8GlobalLed::SendPlugin2,
            uf8::Uf8GlobalLed::SendPlugin3, uf8::Uf8GlobalLed::SendPlugin4,
            uf8::Uf8GlobalLed::SendPlugin5, uf8::Uf8GlobalLed::SendPlugin6,
            uf8::Uf8GlobalLed::SendPlugin7, uf8::Uf8GlobalLed::SendPlugin8,
        }) {
            sendLedFrames(uf8::buildUf8GlobalLed(led, false));
        }
    }

    // Page Left / Page Right LEDs — dim baseline, bright momentarily
    // while held (driven from the press handler, not here). At init
    // we just ensure both are at known dim state. Cells PL 0x2D,
    // PR 0x2C (cap48 2026-04-30). Both are 3-state legacy LEDs.
    if (!g_globalLedsInit) {
        sendLedFrames(uf8::buildUf8GlobalLed(uf8::Uf8GlobalLed::PageLeft, false));
        sendLedFrames(uf8::buildUf8GlobalLed(uf8::Uf8GlobalLed::PageRight, false));
        g_lastPageLeftLit = 0;
        g_lastPageRightLit = 0;
    }

    // Channel-encoder mode LEDs — exactly one of Nav/Nudge/Focus is bright,
    // matching the active EncoderMode. Encoder push (0x76) returns to Nav.
    if (encMode != g_lastEncoderMode || !g_globalLedsInit) {
        sendLedFrames(uf8::buildUf8GlobalLed(uf8::Uf8GlobalLed::Nav,
                                             encMode == EncoderMode::Nav));
        sendLedFrames(uf8::buildUf8GlobalLed(uf8::Uf8GlobalLed::Nudge,
                                             encMode == EncoderMode::Nudge));
        sendLedFrames(uf8::buildUf8GlobalLed(uf8::Uf8GlobalLed::Focus,
                                             encMode == EncoderMode::Focus));
        g_lastEncoderMode = encMode;
    }

    g_globalLedsInit = true;
}

// Tick counter since extension load. Used to fire a "settle-time"
// re-refresh on the UC1 a moment after open, so the LED rings start
// in correct state even if the firmware's own init flood overwrote
// our first refresh. REAPER's onTimer fires at ~30 Hz, so 60 ticks
// ≈ 2 s.
int g_uc1RefireAtTick = 60;
int g_tickCounter = 0;

void onTimer()
{
    ++g_tickCounter;
    if (g_tickCounter == g_uc1RefireAtTick && g_uc1_surface) {
        // Force a full LED re-push once the device + REAPER project
        // are settled. Without this, the first refresh races with
        // UC1's init-flood and leaves rings stuck until the next
        // focus change.
        g_uc1_surface->invalidateCache();
        g_uc1_surface->refresh();
    }
    // UF8 init / project-load refire: when track count goes from 0
    // (no project, or project-load mid-flight) to >0 (project ready),
    // re-arm bank state and global-LED dedup. Without this:
    //  - the open-time g_bankDirty.store(true) gets consumed by the
    //    first onTimer tick before REAPER has finished loading, the
    //    loop pushes blank/off LEDs, and bankDirty clears leaving SEL
    //    LEDs in firmware-default white state.
    //  - on subsequent project loads, dedup keeps Pan/Shift/Nav LEDs
    //    in their pre-load committed state, but the firmware may have
    //    cleared them — invalidating g_globalLedsInit forces re-push.
    static int g_lastTrackCountForReinit = 0;
    const int currentTrackCount = CountTracks(nullptr);
    if (g_dev && g_dev->isOpen() &&
        currentTrackCount > 0 && g_lastTrackCountForReinit == 0) {
        g_bankDirty.store(true);
        g_globalLedsInit = false;
    }
    g_lastTrackCountForReinit = currentTrackCount;
    chaseLastTouchedFx();
    drainInputQueue();
    commitDebouncedTouchReleases();
    if (g_sync) g_sync->refresh(reaperColorForVisibleSlot);
    pushZonesForVisibleSlots();
    pushUf8GlobalLeds();
    // pushSelColourBar() removed: it was a per-tick fallback that wrote
    // SEL LEDs in white-only mode (buildSelWhite). With track-colour SEL
    // now driven through sendLed() + the bank-shift refresh, this fallback
    // was overwriting the coloured frames with plain white on every tick.
    pushVuMeter();
    // UC1 stereo VU.
    //   Input  meter L/R: AudioAccessor (samples "immediately pre-FX"
    //                     per REAPER docs). Pre-CS, pre-everything.
    //   Output meter L/R: by default Track_GetPeakInfo (post-FX-chain,
    //                     == what REAPER's track meter shows). User
    //                     toggle via ExtState ("Rea-Sixty" /
    //                     "cs_output_source"): "track" (default) shows
    //                     post-FX-chain; "off" silences the strip.
    //                     Default-on because the post-FX level is what
    //                     the user hears, which is genuinely useful;
    //                     toggle exists for purist SSL UC1 fidelity.
    if (g_uc1_surface) {
        void* focus = g_uc1_surface->focusedTrack();
        static MediaTrack*    s_lastVuTrack = nullptr;
        static AudioAccessor* s_vuAccessor  = nullptr;
        auto teardownAccessor = [&] {
            if (s_vuAccessor) {
                DestroyAudioAccessor(s_vuAccessor);
                s_vuAccessor = nullptr;
            }
            s_lastVuTrack = nullptr;
        };
        if (!focus) {
            teardownAccessor();
        } else {
            MediaTrack* tr = static_cast<MediaTrack*>(focus);
            auto peakToDb = [](double p) -> float {
                if (p <= 0.0) return -120.f;
                return static_cast<float>(20.0 * std::log10(p));
            };

            // Output meter source — ExtState toggle, default = track meter.
            float dbOutL = -120.f;
            float dbOutR = -120.f;
            const char* outSrc = GetExtState("Rea-Sixty", "cs_output_source");
            if (!outSrc || !*outSrc || std::strcmp(outSrc, "track") == 0) {
                dbOutL = peakToDb(Track_GetPeakInfo(tr, 0));
                dbOutR = peakToDb(Track_GetPeakInfo(tr, 1));
            }
            // "off" or any unrecognised value → leave at -120 (silent).

            if (tr != s_lastVuTrack) {
                teardownAccessor();
                s_vuAccessor = CreateTrackAudioAccessor(tr);
                s_lastVuTrack = tr;
            }
            // Peak-hold with decay, applied to all four channels (in L/R
            // + out L/R). Without it a steady sine produces 1-2 dB peak
            // jitter per tick (block size doesn't align with the sine
            // cycle), which flickers the LED at the boundary between
            // two thresholds. Decay constant ~150 ms — fast enough that
            // the meter falls off audibly after a transient, slow
            // enough to absorb sample-block variability.
            static double s_holdInL  = -120.0, s_holdInR  = -120.0;
            static double s_holdOutL = -120.0, s_holdOutR = -120.0;
            // Linear-dB rate-limited fall. Original "0.85 of distance-from-
            // floor" formula (commit 2026-04-28) decayed exponentially in
            // linear-distance-from-floor space — h=-3 fell to h=-59 in 5
            // ticks, fighting REAPER's internal peak ballistics and producing
            // multi-LED flicker on the UC1 VU strip. Linear-dB at 1.5 dB/tick
            // ≈ 45 dB/sec gives full-scale fall in ~1.3 s — standard VU look.
            constexpr double kDbPerTick = 1.5;
            auto holdPeak = [&](double& hold, double raw) {
                if (raw > hold) hold = raw;
                else {
                    const double next = hold - kDbPerTick;
                    hold = (next > raw) ? next : raw;
                }
            };

            float dbInL = -120.f;
            float dbInR = -120.f;
            if (s_vuAccessor) {
                AudioAccessorValidateState(s_vuAccessor);
                constexpr int kBlock  = 512;
                constexpr int kNchans = 2;
                constexpr int kSr     = 48000;
                static double buf[kBlock * kNchans];
                const double t = GetPlayPosition();
                const int rc = GetAudioAccessorSamples(
                    s_vuAccessor, kSr, kNchans, t, kBlock, buf);
                if (rc > 0) {
                    double pL = 0.0, pR = 0.0;
                    for (int i = 0; i < kBlock; ++i) {
                        const double sL = std::fabs(buf[i * 2]);
                        const double sR = std::fabs(buf[i * 2 + 1]);
                        if (sL > pL) pL = sL;
                        if (sR > pR) pR = sR;
                    }
                    dbInL = peakToDb(pL);
                    dbInR = peakToDb(pR);
                }
            }
            holdPeak(s_holdInL,  dbInL);
            holdPeak(s_holdInR,  dbInR);
            holdPeak(s_holdOutL, dbOutL);
            holdPeak(s_holdOutR, dbOutR);
            g_uc1_surface->pushCsVu(static_cast<float>(s_holdInL),
                                    static_cast<float>(s_holdInR),
                                    static_cast<float>(s_holdOutL),
                                    static_cast<float>(s_holdOutR));
        }
    }
    // UF8 GR — driven from the focused track's CS plug-in via
    // TrackFX_GetNamedConfigParm("GainReduction_dB"). The frame
    // FF 66 09 15 <s1>..<s8> <chk> carries one GR byte per visible
    // strip — earlier mistake was treating bytes 5..11 as zero-padding,
    // so all GR rendered onto strip 1 only. Now: locate the focused
    // track inside the visible bank and stamp its strip's byte.
    // Calibration: byte 0x00 = rest (no LEDs), byte ~0x18 ≈ 10 dB GR
    // (1.4 byte/dB slope, retained from earlier visual tuning).
    // Ballistics: up immediately, down 1 byte/tick — Plugin's
    // GainReduction_dB tracks the signal envelope tightly so the raw
    // byte swings several positions per audio beat without rate-limit.
    {
        std::array<uint8_t, 8> targetBytes{};  // all zero by default
        int focStrip = -1;
        if (g_uc1_surface) {
            if (auto* tr = static_cast<MediaTrack*>(g_uc1_surface->focusedTrack())) {
                if (!ValidatePtr2(nullptr, tr, "MediaTrack*")) tr = nullptr;
                const int trackCount = CountTracks(nullptr);
                const int bankOffset = g_bankOffset.load();
                int focIdx = -1;
                if (tr) for (int i = 0; i < trackCount; ++i) {
                    if (GetTrack(nullptr, i) == tr) { focIdx = i; break; }
                }
                if (focIdx >= bankOffset && focIdx < bankOffset + 8) {
                    focStrip = focIdx - bankOffset;
                    uc1::UC1Bindings b = uc1::lookupBindingsOnTrack(tr);
                    if (b.channelMap && b.channelFxIdx >= 0) {
                        char buf[64];
                        if (TrackFX_GetNamedConfigParm(tr, b.channelFxIdx,
                                                       "GainReduction_dB",
                                                       buf, sizeof(buf))) {
                            float gr = static_cast<float>(std::atof(buf));
                            if (gr < 0) gr = -gr;
                            if (gr > 10.f) gr = 10.f;
                            const uint8_t newByte = static_cast<uint8_t>(
                                std::lround(gr * (0x18 / 10.0f)));
                            uint8_t& held = g_uf8GrBytes[focStrip];
                            if (newByte > held) held = newByte;
                            else if (held > newByte) --held;
                            targetBytes[focStrip] = held;
                        }
                    }
                }
            }
        }
        // Clear holders for non-focused strips so a previously-focused
        // strip doesn't keep displaying its last GR after focus moves.
        for (int s = 0; s < 8; ++s) {
            if (s != focStrip) g_uf8GrBytes[s] = 0;
        }
        if (g_dev && g_dev->isOpen()) {
            g_dev->setGrBytes(targetBytes);
        }
    }
    if (g_uc1_surface) g_uc1_surface->poll();

    // Once-per-second UC1 wire stats — only print if something is
    // actually wrong (errors accumulating, or OUT flow stalled).
    // Silent otherwise so the console stays readable while the user
    // explores knob mappings.
    static auto lastStat = std::chrono::steady_clock::now();
    static uint64_t prevOutFrames = 0, prevOutErrors = 0;
    if (g_uc1_dev) {
        auto now = std::chrono::steady_clock::now();
        if (now - lastStat >= std::chrono::seconds(1)) {
            const auto of = uc1::debugOutFrames();
            const auto oe = uc1::debugOutErrors();
            const auto dOf = of - prevOutFrames;
            const auto dOe = oe - prevOutErrors;
            if (dOe > 0 || dOf < 20) {
                char line[128];
                std::snprintf(line, sizeof(line),
                    "UC1 WARN: OUT=%llu frames/s errs=%llu (expected ~50)\n",
                    (unsigned long long)dOf, (unsigned long long)dOe);
                ShowConsoleMsg(line);
            }
            prevOutFrames = of; prevOutErrors = oe;
            lastStat = now;
        }
    }

    // ImGui frame for the Plugin Mixer / Settings window. No-op while the
    // window is closed; when open, drives the entire ReaImGui paint cycle
    // for this tick. Kept last so any REAPER-API reads above (track
    // peaks, GR, focus state) are settled before the UI samples them.
    g_mixerWindow.onRunTick();
}

// Brightness custom actions — registered at plugin entry point. REAPER
// dispatches via hookcommand. Unique-section-id 0 = main section.
custom_action_register_t g_actionBrightnessUp{
    0, "REASIXTY_BRIGHTNESS_UP", "Rea-Sixty: Brightness up", nullptr,
};
custom_action_register_t g_actionBrightnessDown{
    0, "REASIXTY_BRIGHTNESS_DOWN", "Rea-Sixty: Brightness down", nullptr,
};
int g_cmdBrightnessUp = 0;
int g_cmdBrightnessDown = 0;

// LED-cell discovery probe — for buttons whose LED cell hasn't been
// captured yet. Each call dims the previous candidate cell, lights
// the next bright-white, and logs the cell ID. User runs the action
// repeatedly while watching the hardware to discover the mapping.
//
// Round 4: 0x30..0x37 produced ZERO visible LEDs in round 3 even
// though cap35/36 originally placed Send/Plugin there — the original
// decode was wrong (same fate as Layer 1/2 / Page Left). Same outcome
// for 0x64..0x77. Send/Plugin + Page Right must live in cells we
// haven't touched yet, or use a different frame family. Sweep the
// remaining never-probed gaps first; if still nothing, the LEDs are
// likely on `FF 3B 03 <id>` (legacy mono path) instead of FF 38/39 04.
constexpr uint8_t kProbeCells[] = {
    // Tiny gap above the per-strip top-soft-keys
    0x20, 0x21,
    // Wide unexplored range above 0x77
    0x78, 0x79, 0x7A, 0x7B, 0x7C, 0x7D, 0x7E, 0x7F,
    0x80, 0x81, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87,
    0x88, 0x89, 0x8A, 0x8B, 0x8C, 0x8D, 0x8E, 0x8F,
    0x90, 0x91, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97,
    0x98, 0x99, 0x9A, 0x9B, 0x9C, 0x9D, 0x9E, 0x9F,
    0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7,
    0xA8, 0xA9, 0xAA, 0xAB, 0xAC, 0xAD, 0xAE, 0xAF,
};
constexpr size_t kProbeCellCount = sizeof(kProbeCells) / sizeof(kProbeCells[0]);
int g_probeCellIndex = -1;  // -1 = nothing lit yet

uint8_t probeChecksum(std::span<const uint8_t> payload)
{
    unsigned sum = 0;
    for (auto b : payload) sum += b;
    return static_cast<uint8_t>(sum & 0xFF);
}

void sendProbeFrame(uint8_t cmd, uint8_t cell, uint8_t a, uint8_t b)
{
    std::vector<uint8_t> frame{0xFF, cmd, 0x04, cell, 0x00, a, b};
    std::span<const uint8_t> payload{frame.data() + 1, frame.size() - 1};
    frame.push_back(probeChecksum(payload));
    if (g_dev && g_dev->isOpen()) g_dev->send(frame);
}

void probeNextLedCell()
{
    if (!g_dev || !g_dev->isOpen()) {
        ShowConsoleMsg("Rea-Sixty probe: UF8 not open\n");
        return;
    }
    // Dim the previously-lit cell (if any) so only one lights at a time.
    if (g_probeCellIndex >= 0) {
        const uint8_t prev = kProbeCells[g_probeCellIndex];
        sendProbeFrame(0x38, prev, 0x11, 0xF1);
        sendProbeFrame(0x39, prev, 0x11, 0xF1);
    }
    g_probeCellIndex = (g_probeCellIndex + 1) % static_cast<int>(kProbeCellCount);
    const uint8_t cell = kProbeCells[g_probeCellIndex];
    // Bright white: FF 38 = FF FF, FF 39 = 00 F0 (matches standard LED on-state).
    sendProbeFrame(0x38, cell, 0xFF, 0xFF);
    sendProbeFrame(0x39, cell, 0x00, 0xF0);
    char line[80];
    std::snprintf(line, sizeof(line),
        "Rea-Sixty probe: cell 0x%02X bright (idx %d/%zu)\n",
        static_cast<unsigned>(cell), g_probeCellIndex + 1, kProbeCellCount);
    ShowConsoleMsg(line);
}

custom_action_register_t g_actionProbeLed{
    0, "REASIXTY_PROBE_LED", "Rea-Sixty: Probe next global LED cell",
    nullptr,
};
int g_cmdProbeLed = 0;

// Legacy mono-LED probe — same idea as the colour-pair probe above
// but uses the `FF 3B 03 <id> 00 <state>` frame family that Plugin
// (0x2F) is documented as a "2-state outlier" for. Send/Plugin row +
// Page Right LEDs aren't reachable via FF 38/39 04 (verified via the
// 2026-04-30 sweep of 0x18..0xAF), so this is the next family to try.
constexpr uint8_t kLegacyProbeCells[] = {
    // Suspects from cap35/36 Send/Plugin decode — cells already cleared
    // empty under FF 38/39 04, may respond here.
    0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37,
    // 0x2C — symmetric to PageLeft 0x2D, top suspect for PageRight.
    0x2C,
    // Other cells that came up empty under the colour-pair probe.
    0x28, 0x29, 0x2A, 0x38,
    0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48,
    0x50, 0x51,
};
constexpr size_t kLegacyProbeCellCount =
    sizeof(kLegacyProbeCells) / sizeof(kLegacyProbeCells[0]);
int g_legacyProbeCellIndex = -1;

void probeNextLegacyLedCell()
{
    if (!g_dev || !g_dev->isOpen()) {
        ShowConsoleMsg("Rea-Sixty legacy probe: UF8 not open\n");
        return;
    }
    // Turn off the previously-lit cell (state 0x00).
    if (g_legacyProbeCellIndex >= 0) {
        g_dev->send(uf8::buildLedCommand(
            kLegacyProbeCells[g_legacyProbeCellIndex], false));
    }
    g_legacyProbeCellIndex =
        (g_legacyProbeCellIndex + 1) % static_cast<int>(kLegacyProbeCellCount);
    const uint8_t cell = kLegacyProbeCells[g_legacyProbeCellIndex];
    g_dev->send(uf8::buildLedCommand(cell, true));
    char line[80];
    std::snprintf(line, sizeof(line),
        "Rea-Sixty legacy probe: id 0x%02X on (idx %d/%zu)\n",
        static_cast<unsigned>(cell), g_legacyProbeCellIndex + 1,
        kLegacyProbeCellCount);
    ShowConsoleMsg(line);
}

custom_action_register_t g_actionProbeLegacyLed{
    0, "REASIXTY_PROBE_LEGACY_LED",
    "Rea-Sixty: Probe next legacy mono-LED id", nullptr,
};
int g_cmdProbeLegacyLed = 0;

// Diagnostic — toggle a per-frame USB trace into /tmp/reaper_uf8_frames.log.
// Used to compare Rea-Sixty's frame stream against an SSL360 baseline when
// the motor-lock symptom recurs. Off by default (zero overhead).
void toggleFrameTrace()
{
    if (!g_dev) {
        ShowConsoleMsg("Rea-Sixty trace: UF8 not open\n");
        return;
    }
    const bool on = !g_dev->frameTrace();
    g_dev->setFrameTrace(on);
    if (on) {
        // Drop any prior log so the trace starts clean.
        std::remove("/tmp/reaper_uf8_frames.log");
        ShowConsoleMsg(
            "Rea-Sixty trace ON — logging to /tmp/reaper_uf8_frames.log\n");
    } else {
        ShowConsoleMsg("Rea-Sixty trace OFF\n");
    }
}

custom_action_register_t g_actionFrameTrace{
    0, "REASIXTY_FRAME_TRACE",
    "Rea-Sixty: Toggle USB frame trace", nullptr,
};
int g_cmdFrameTrace = 0;

// Diagnostic — replay the EXACT byte sequence captured from SSL360
// at uc1_40 t=8.700 (first indicator frame in the sweep). Bypasses
// our build* functions entirely. If this still doesn't paint the
// yellow indicator, we're missing a precursor setup frame SSL360
// sent earlier; if it does paint, our slot-lookup / encoding has
// a bug.
void uc1FireTestIndicator()
{
    if (!g_uc1_dev || !g_uc1_dev->isOpen()) {
        ShowConsoleMsg("Rea-Sixty UC1 test: device not open\n");
        return;
    }
    // Verbatim from uc1_40 capture: banner(06) + header "Mix" +
    // triple "PLUG-IN/COMP MIX/PAN" + value "99.3" + unit "%" +
    // indicator FF FF FF (n=23 = max).
    const std::vector<std::vector<uint8_t>> burst = {
        {0xff,0x66,0x03,0x00,0x06,0x00,0x6f},                                                                   // banner
        {0xff,0x66,0x04,0x01,0x4d,0x69,0x78,0x99},                                                              // header "Mix"
        {0xff,0x66,0x2b,0x04,                                                                                    // triple — large 3-slot
         0x50,0x4c,0x55,0x47,0x2d,0x49,0x4e, 0x00,0x00,0x00,0x00,0x00,0x00,0x00,                                // "PLUG-IN" + pad
         0x43,0x4f,0x4d,0x50,0x20,0x4d,0x49,0x58, 0x00,0x00,0x00,0x00,0x00,0x00,                                // "COMP MIX" + pad
         0x50,0x41,0x4e, 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,                                // "PAN" + pad
         0xad},
        {0xff,0x66,0x05,0x0e,0x39,0x39,0x2e,0x33,0x4c},                                                          // value "99.3"
        {0xff,0x66,0x02,0x0f,0x25,0x9c},                                                                         // unit "%"
        {0xff,0x66,0x04,0x0d,0xff,0xff,0xff,0x74},                                                               // indicator FF FF FF
    };
    for (const auto& f : burst) g_uc1_dev->send(f);
    ShowConsoleMsg("Rea-Sixty UC1 test: replayed uc1_40 t=8.700 verbatim\n");
}

custom_action_register_t g_actionUc1TestIndicator{
    0, "REASIXTY_UC1_TEST_INDICATOR",
    "Rea-Sixty: UC1 test EXT_FUNCS indicator (debug)", nullptr,
};
int g_cmdUc1TestIndicator = 0;

// Diagnostic: poll a battery of TrackFX_GetNamedConfigParm candidate
// names against the focused track's CS plug-in and dump the returned
// values to the REAPER console. Used to find the data source for Gate
// GR (currently no known parmname; SSL CS2 only documents combined
// "GainReduction_dB"). User runs the action while moving the Gate
// Range knob — whichever name returns a non-zero value that varies
// with Range is our Gate GR API.
void probeGateGrSources()
{
    MediaTrack* tr = GetSelectedTrack(nullptr, 0);
    if (!tr) {
        ShowConsoleMsg("Gate-GR probe: no selected track\n");
        return;
    }
    auto match = uf8::lookupPluginOnTrack(tr, uf8::Domain::ChannelStrip);
    if (!match.map) {
        ShowConsoleMsg("Gate-GR probe: focused track has no CS plug-in\n");
        return;
    }
    // Candidate parmnames — PreSonus VST3 conventions + likely SSL
    // extensions. Anything that returns a non-empty buffer is logged.
    static constexpr const char* kCandidates[] = {
        "GainReduction_dB",
        "GateReduction_dB",
        "GateGainReduction_dB",
        "GateGR_dB",
        "GR_dB",
        "GR_Gate_dB",
        "GainReductionGate_dB",
        "Gate_dB",
        "ExpReduction_dB",
        "ExpansionReduction_dB",
        "GateAttenuation_dB",
        "InputGainReduction_dB",
        "OutputGainReduction_dB",
        "ParamValue_GateRange",
        "GateRangeApplied_dB",
    };
    char header[128];
    std::snprintf(header, sizeof(header),
        "Gate-GR probe — track=%s fx=%d\n",
        match.map->displayShort, match.fxIndex);
    ShowConsoleMsg(header);
    char buf[128];
    for (auto* name : kCandidates) {
        buf[0] = '\0';
        const bool ok = TrackFX_GetNamedConfigParm(
            tr, match.fxIndex, name, buf, sizeof(buf));
        char line[256];
        if (ok) {
            std::snprintf(line, sizeof(line),
                "  [%s] = %s\n", name, buf);
        } else {
            std::snprintf(line, sizeof(line),
                "  [%s] not exposed\n", name);
        }
        ShowConsoleMsg(line);
    }
}

custom_action_register_t g_actionProbeGateGr{
    0, "REASIXTY_PROBE_GATE_GR",
    "Rea-Sixty: Probe Gate GR sources on focused CS plug-in", nullptr,
};
int g_cmdProbeGateGr = 0;

// Diagnostic: dump every VST3 param's name + ident + current normalised
// value for the focused CS plug-in. Use to find candidate Gate-GR
// parameters when the named-config-parm probe comes up empty (the SSL
// CS plug-in doesn't expose the PreSonus VST3 GR readback for the Gate
// section — verified 2026-05-01). Run twice at different Range knob
// positions and diff to find params whose value tracks Range.
void dumpCsPluginParams()
{
    MediaTrack* tr = GetSelectedTrack(nullptr, 0);
    if (!tr) {
        ShowConsoleMsg("Param-dump: no selected track\n");
        return;
    }
    auto match = uf8::lookupPluginOnTrack(tr, uf8::Domain::ChannelStrip);
    if (!match.map) {
        ShowConsoleMsg("Param-dump: focused track has no CS plug-in\n");
        return;
    }
    const int n = TrackFX_GetNumParams(tr, match.fxIndex);
    char header[128];
    std::snprintf(header, sizeof(header),
        "Param-dump %s — %d params\n", match.map->displayShort, n);
    ShowConsoleMsg(header);
    for (int p = 0; p < n; ++p) {
        char name[128] = {};
        char ident[128] = {};
        char fmt[64]   = {};
        TrackFX_GetParamName(tr, match.fxIndex, p, name, sizeof(name));
        TrackFX_GetParamIdent(tr, match.fxIndex, p, ident, sizeof(ident));
        const double v = TrackFX_GetParamNormalized(tr, match.fxIndex, p);
        TrackFX_FormatParamValueNormalized(tr, match.fxIndex, p, v,
                                           fmt, sizeof(fmt));
        char line[512];
        std::snprintf(line, sizeof(line),
            "  [%3d] %-32s ident=%-40s norm=%.4f val=%s\n",
            p, name, ident, v, fmt);
        ShowConsoleMsg(line);
    }
}

custom_action_register_t g_actionDumpParams{
    0, "REASIXTY_DUMP_CS_PARAMS",
    "Rea-Sixty: Dump all CS plug-in params (find Gate GR)", nullptr,
};
int g_cmdDumpParams = 0;

// Diagnostic: dump the focused CS plug-in's full track-state chunk to
// /tmp/reasixty_cs_chunk.txt. Use to find the XML attribute name SSL
// uses for the processing-order routing setting (10 orders + 'b'
// variants per UC1 manual p.20). Run twice at two different routing
// orders, diff, and the differing attribute is the patch target.
void dumpCsChunk()
{
    MediaTrack* tr = GetSelectedTrack(nullptr, 0);
    if (!tr) {
        ShowConsoleMsg("Chunk-dump: no selected track\n");
        return;
    }
    int chunkSize = 0;
    char* chunk = GetSetObjectState(tr, "");
    if (!chunk) {
        ShowConsoleMsg("Chunk-dump: GetSetObjectState returned null\n");
        return;
    }
    const std::string body{chunk};
    FreeHeapPtr(chunk);
    const char* path = "/tmp/reasixty_cs_chunk.txt";
    FILE* f = std::fopen(path, "w");
    if (!f) {
        ShowConsoleMsg("Chunk-dump: cannot open /tmp/reasixty_cs_chunk.txt\n");
        return;
    }
    std::fwrite(body.data(), 1, body.size(), f);
    std::fclose(f);
    char line[256];
    std::snprintf(line, sizeof(line),
        "Chunk-dump: %zu bytes → %s\n", body.size(), path);
    ShowConsoleMsg(line);
    chunkSize = static_cast<int>(body.size());
    (void)chunkSize;
}

custom_action_register_t g_actionDumpChunk{
    0, "REASIXTY_DUMP_CS_CHUNK",
    "Rea-Sixty: Dump focused track's chunk (find SSL routing attr)",
    nullptr,
};
int g_cmdDumpChunk = 0;

// Diagnostic: dump just the 5 SSL routing flags + 3 section In/Out
// flags from the focused CS plug-in. Compact one-line output so the
// user can run it at each of the 10 SSL360 routing orders and we
// build the preset table from the results.
void dumpRoutingFlags()
{
    MediaTrack* tr = GetSelectedTrack(nullptr, 0);
    if (!tr) {
        ShowConsoleMsg("Routing-flags: no selected track\n");
        return;
    }
    auto match = uf8::lookupPluginOnTrack(tr, uf8::Domain::ChannelStrip);
    if (!match.map) {
        ShowConsoleMsg("Routing-flags: focused track has no CS plug-in\n");
        return;
    }
    // Slot ids → vst3 param indices, resolved via PluginMap so this
    // works across all four CS variants (CS 2 / 4K E / 4K G / 4K B).
    auto val = [&](const char* slotId) -> int {
        for (const auto& s : match.map->slots) {
            if (s.id && std::strcmp(s.id, slotId) == 0 && s.vst3Param >= 0) {
                const double v = TrackFX_GetParamNormalized(
                    tr, match.fxIndex, s.vst3Param);
                return v >= 0.5 ? 1 : 0;
            }
        }
        return -1;  // not in this plug-in's map
    };
    char line[256];
    std::snprintf(line, sizeof(line),
        "Routing %s: ExtSC=%d FiltIn=%d FiltSC=%d EqSC=%d DynPreEq=%d  |  Filt=%d Eq=%d Dyn=%d\n",
        match.map->displayShort,
        val("ExternalSC"),
        val("FiltersToInput"),
        val("FiltersToSC"),
        val("EqToSC"),
        val("DynamicsPreEq"),
        val("FiltersIn"),
        val("EqIn"),
        val("DynamicsIn"));
    ShowConsoleMsg(line);
}

custom_action_register_t g_actionDumpRouting{
    0, "REASIXTY_DUMP_ROUTING_FLAGS",
    "Rea-Sixty: Dump SSL routing flags (run at each of 10 orders)",
    nullptr,
};
int g_cmdDumpRouting = 0;

// Custom action descriptor for the mixer-toggle. The instance itself is
// declared earlier in this TU so onTimer() can call it; this block keeps
// the action wiring next to its brightness siblings for readability.
custom_action_register_t g_actionToggleMixer{
    0, "REASIXTY_TOGGLE_MIXER", "Rea-Sixty: Toggle Plugin Mixer Window", nullptr,
};
int g_cmdToggleMixer = 0;

// hookcommand2 is the correct hook for custom_action dispatch per SDK
// note at reaper_plugin.h:1086. hookcommand (v1) only catches actions
// triggered via menu/keyboard, not custom_action registered entries.
bool hookCommand2(KbdSectionInfo* /*sec*/, int command,
                  int /*val*/, int /*val2*/, int /*relmode*/,
                  HWND /*hwnd*/)
{
    if (command == 0) return false;
    if (command == g_cmdBrightnessUp)   { brightnessUp();   return true; }
    if (command == g_cmdBrightnessDown) { brightnessDown(); return true; }
    if (command == g_cmdProbeLed)       { probeNextLedCell();       return true; }
    if (command == g_cmdProbeLegacyLed) { probeNextLegacyLedCell(); return true; }
    if (command == g_cmdFrameTrace)     { toggleFrameTrace();       return true; }
    if (command == g_cmdUc1TestIndicator) { uc1FireTestIndicator();  return true; }
    if (command == g_cmdProbeGateGr)    { probeGateGrSources();     return true; }
    if (command == g_cmdDumpParams)     { dumpCsPluginParams();     return true; }
    if (command == g_cmdDumpChunk)      { dumpCsChunk();            return true; }
    if (command == g_cmdDumpRouting)    { dumpRoutingFlags();       return true; }
    if (command == g_cmdToggleMixer)    { g_mixerWindow.toggle();   return true; }
    return false;
}

} // anonymous

// External hook so UC1Surface (different TU) can trigger the same
// MCP-scroll + UF8-rebank behaviour that the UF8 select/encoder paths
// use. Anonymous-namespace internals (g_bankOffset, g_followMode…) stay
// private; this wrapper is the only symbol exposed. The name differs
// from the internal helper because giving the anonymous-namespace
// version external linkage would conflict with this definition.
void reasixty_followSelectedInMixer(MediaTrack* tr)
{
    followSelectedInMixer(tr);
}

extern "C" REAPER_PLUGIN_DLL_EXPORT int REAPER_PLUGIN_ENTRYPOINT(
    REAPER_PLUGIN_HINSTANCE hInstance, reaper_plugin_info_t* rec)
{
    if (!rec) {
        // Unload. REAPER destroys our ReaSixtySurface instances (if any
        // still exist) via IReaperControlSurface's virtual destructor;
        // those destructors tear down the USB device and timer. Here we
        // just un-register the class so no new instances can be created.
        plugin_register("-csurf", &g_csurfReg);
        return 0;
    }

    if (rec->caller_version != REAPER_PLUGIN_VERSION) return 0;
    if (REAPERAPI_LoadAPI(rec->GetFunc) != 0) return 0;

    // Restore persisted UI mode flags (Pan override, encoder mode) so
    // they survive REAPER restarts. ExtState is global per-extension —
    // persistAcrossSessions=true writes through to reaper-extstate.ini.
    if (const char* v = GetExtState("ReaSixty", "forcePan");
        v && v[0] == '1') {
        g_forcePan.store(true);
    }
    // Don't restore FLIP state — start with FLIP off so the user has
    // a known-good baseline. Re-enable persistence once the FLIP code
    // paths are fully de-risked.
    SetExtState("ReaSixty", "flip", "0", true);
    if (const char* m = GetExtState("ReaSixty", "encoderMode"); m && *m) {
        if (std::strcmp(m, "Nudge") == 0)      g_encoderMode.store(EncoderMode::Nudge);
        else if (std::strcmp(m, "Focus") == 0) g_encoderMode.store(EncoderMode::Focus);
        else                                   g_encoderMode.store(EncoderMode::Nav);
    }
    if (const char* sb = GetExtState("ReaSixty", "softKeyBank"); sb && *sb) {
        const int v = std::atoi(sb);
        if (v >= 0 && v <= softkey::kCsMaxBank) g_softKeyBank.store(v);
    }

    // Register as a full control-surface class. The user adds a
    // "Rea-Sixty" entry in Preferences → Control/OSC/Web; REAPER then
    // calls createReaSixty() to instantiate ReaSixtySurface, which
    // opens the UF8 and starts the timer.
    plugin_register("csurf", &g_csurfReg);

    // Custom actions: brightness up/down. REAPER assigns a command ID
    // when we register — stash it for dispatch in hookCommand.
    g_cmdBrightnessUp   = plugin_register("custom_action", &g_actionBrightnessUp);
    g_cmdBrightnessDown = plugin_register("custom_action", &g_actionBrightnessDown);
    g_cmdProbeLed       = plugin_register("custom_action", &g_actionProbeLed);
    g_cmdProbeLegacyLed = plugin_register("custom_action", &g_actionProbeLegacyLed);
    g_cmdFrameTrace     = plugin_register("custom_action", &g_actionFrameTrace);
    g_cmdUc1TestIndicator = plugin_register("custom_action", &g_actionUc1TestIndicator);
    g_cmdProbeGateGr    = plugin_register("custom_action", &g_actionProbeGateGr);
    g_cmdDumpParams     = plugin_register("custom_action", &g_actionDumpParams);
    g_cmdDumpChunk      = plugin_register("custom_action", &g_actionDumpChunk);
    g_cmdDumpRouting    = plugin_register("custom_action", &g_actionDumpRouting);
    g_cmdToggleMixer    = plugin_register("custom_action", &g_actionToggleMixer);
    plugin_register("hookcommand2", reinterpret_cast<void*>(hookCommand2));

    return 1;
}
