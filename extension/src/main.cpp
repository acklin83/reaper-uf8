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
#include "HidDevice.h"
#include "MidiBridge.h"
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

// Currently-visible parameter page for SSL plug-in mirroring. Each
// plugin's PluginMap holds an ordered slots[] list; g_pageIdx indexes
// into that list per-strip-per-plugin (clamped to each plugin's size).
// Stage-1 is hard-coded to page 0; Stage 3 wires the UF8 Page ←/→
// buttons to increment/decrement this value.
std::atomic<int>  g_pageIdx{0};
std::atomic<bool> g_pageDirty{false};

// When the user hits the PAN button, we globally override every strip's
// V-Pot to act as pan control regardless of whether the track hosts an
// SSL plug-in. Any V-Pot assignment soft key (0x68–0x6D) returns to
// automatic plug-in-param mode. Same invalidation path as a page change.
std::atomic<bool> g_forcePan{false};

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
            case PendingInput::VolumeAbs:
                // CSurf_OnVolumeChange applies the user's new position to
                // the track AND broadcasts to other surfaces. We do not
                // cache the value — motor echo reads GetTrackUIVolPan
                // on each tick so it always reflects whatever REAPER
                // actually has (including envelope playback).
                CSurf_OnVolumeChange(tr, e.value, false);
                break;
            case PendingInput::PanDelta: {
                // V-pot rotation: if the strip's track hosts an SSL plug-in
                // map AND we're not in global Pan mode, drive its paged
                // parameter. Otherwise fall back to track pan.
                const int pgIdx = g_pageIdx.load();
                auto mm = uf8::lookupPluginOnTrack(tr);
                const bool forcePan = g_forcePan.load();
                if (!forcePan && mm.map
                    && static_cast<size_t>(pgIdx) < mm.map->slots.size())
                {
                    const uf8::LinkSlot& sl = mm.map->slots[pgIdx];
                    const double cur = TrackFX_GetParamNormalized(tr, mm.fxIndex,
                                                                  sl.vst3Param);
                    // e.value is pan-scaled (≈1/128 per event). Plug-in param
                    // ranges are 0..1, so without a bump a full sweep would
                    // take ~128 detents. 4× gives ~32-detent full sweep —
                    // snappy enough for quick gain grabs without being
                    // nervous. Fine mode (Shift) quarters.
                    double delta = e.value * 4.0 * (sl.inverted ? -1.0 : 1.0);
                    if (g_shiftHeld.load()) delta *= 0.25;
                    double next = cur + delta;
                    if (next < 0.0) next = 0.0;
                    if (next > 1.0) next = 1.0;
                    TrackFX_SetParamNormalized(tr, mm.fxIndex, sl.vst3Param, next);
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
                // V-pot push: with an SSL plug-in present (and not in
                // global Pan mode), reset the paged param to its midpoint.
                // Otherwise, re-center pan.
                auto mm = uf8::lookupPluginOnTrack(tr);
                const int pgIdx = g_pageIdx.load();
                const bool forcePan = g_forcePan.load();
                if (!forcePan && mm.map
                    && static_cast<size_t>(pgIdx) < mm.map->slots.size())
                {
                    TrackFX_SetParamNormalized(tr, mm.fxIndex,
                        mm.map->slots[pgIdx].vst3Param, 0.5);
                } else {
                    SetMediaTrackInfo_Value(tr, "D_PAN", 0.0);
                }
                break;
            }
        }
    }
}

// UF8 fader-top dB value. The MCU-compatible top on UF8 is +10 dB
// (different from REAPER's default slider top of +12 dB). Mapping pb14
// through REAPER's slider law directly puts the UF8 0-dB tick ≈ +2.25 dB
// hotter in REAPER; scaling to DB2SLIDER(+10) fixes the round-trip so
// physical 0-dB on the UF8 matches 0-dB in REAPER.
constexpr double kUf8FaderTopDb = 10.0;

// Convert a 14-bit MCU pitch-bend value (0..16383) to a linear REAPER
// volume (1.0 == 0 dB) via REAPER's own slider curve, scaled so pb14
// = 16383 maps to kUf8FaderTopDb.
double pbToLinearVolume(uint16_t pb14)
{
    if (pb14 == 0) return 0.0;
    const double topSlider = DB2SLIDER(kUf8FaderTopDb);
    const double slider    = static_cast<double>(pb14) / 16383.0 * topSlider;
    const double db        = SLIDER2DB(slider);
    return std::pow(10.0, db / 20.0);
}

// Inverse of pbToLinearVolume. Used to echo REAPER volume back onto the
// UF8 motor so the physical fader follows mouse edits in the DAW.
uint16_t linearVolumeToPb(double linear)
{
    if (!(linear > 0.0)) return 0;                  // catches 0 and NaN
    const double db        = 20.0 * std::log10(linear);
    const double slider    = DB2SLIDER(db);
    const double topSlider = DB2SLIDER(kUf8FaderTopDb);
    if (!(topSlider > 0.0)) return 0;
    double pb = slider / topSlider * 16383.0;
    if (pb < 0)       pb = 0;
    if (pb > 16383.0) pb = 16383.0;
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

// Button LEDs: decoded via cap22 (command) + cap23 (ID map). Format is
//   FF 3B 03 <id> 00 <state> CKSUM
// `id = (7 - strip) * 3 + type` where type 0=SEL, 1=MUTE, 2=SOLO. That
// covers the 24 LEDs in IDs 0x00..0x17 — note the REVERSED strip order:
// leftmost UF8 strip (our index 0) uses IDs 0x15..0x17, rightmost strip
// (index 7) uses 0x00..0x02. Confirmed empirically when track 1 (left)
// was initially lighting channel 8 (right).
//
// REC ARM isn't in cap23 (its UF8-only selection mode wasn't active
// during that capture); best guess is id = 0x18 + (7 - strip), disabled
// until verified.
enum class LedClass : uint8_t { Sel = 0, Mute = 1, Solo = 2, Arm = 3 };

uint8_t ledIdFor(LedClass cls, int strip)
{
    const int reversed = 7 - strip;
    switch (cls) {
        case LedClass::Sel:  return static_cast<uint8_t>(reversed * 3 + 0);
        case LedClass::Mute: return static_cast<uint8_t>(reversed * 3 + 1);
        case LedClass::Solo: return static_cast<uint8_t>(reversed * 3 + 2);
        case LedClass::Arm:  return static_cast<uint8_t>(0x18 + reversed);  // unverified
    }
    return 0;
}

void sendLed(LedClass cls, MediaTrack* tr, bool on)
{
    if (!g_dev) return;
    if (cls == LedClass::Arm) return;   // gate ARM until cap23b verifies its ID range
    for (int s = 0; s < 8; ++s) {
        if (g_slotTrack[s] != tr) continue;
        g_dev->send(uf8::buildLedCommand(ledIdFor(cls, s), on));
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
    sendLed(LedClass::Arm, tr, arm);   // currently a no-op (gated)
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
        // them all dark until REAPER state says otherwise. Blast an OFF for
        // every id 0x00..0x17 at open time so the initial display matches
        // an idle REAPER session.
        for (uint8_t id = 0x00; id <= 0x17; ++id) {
            g_dev->send(uf8::buildLedCommand(id, false));
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
            case 0x04: frameSize = 7; break;   // poll (02 9d 01 a4) — ignore
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
                if (pressed) g_encoderMode.store(EncoderMode::Nav);
                handledNatively = true;
            } else if (id == 0x74) {
                if (pressed) g_encoderMode.store(EncoderMode::Nudge);
                handledNatively = true;
            } else if (id == 0x75) {
                if (pressed) g_encoderMode.store(EncoderMode::Focus);
                handledNatively = true;
            } else if (id == 0x76) {
                // Channel Encoder Push — UF8 default is "return to
                // CHANNEL mode" (the track-selection default). We map it
                // to the same: flip the encoder back into Nav mode.
                if (pressed) g_encoderMode.store(EncoderMode::Nav);
                handledNatively = true;
            } else if (id >= 0x58 && id <= 0x5D) {
                // Automation keys. REAPER modes: 0 Trim, 1 Read,
                // 2 Touch, 3 Write, 4 Latch. UF8 Off/Trim both map to
                // Trim (REAPER has no separate "Off"); can be disambig
                // later via the Phase-2 settings page.
                static constexpr int kModeById[6] = {
                    /*0x58 Off  */ 0,
                    /*0x59 Read */ 1,
                    /*0x5A Wri  */ 3,
                    /*0x5B Trim */ 0,
                    /*0x5C Latc */ 4,
                    /*0x5D Touc */ 2,
                };
                if (pressed) {
                    queueInput({PendingInput::AutomationMode, 0,
                                static_cast<double>(kModeById[id - 0x58])});
                }
                handledNatively = true;
            } else if (id == 0x7A || id == 0x7B || id == 0x7C
                    || id == 0x7D || id == 0x7E) {
                // Zoom pad. 4 arrows zoom horizontally/vertically; the
                // centre (0x7C) fits the project to window. These are
                // wired to REAPER's built-in zoom actions via
                // Main_OnCommand on press only (repeat-on-hold is not
                // yet implemented — single press = single zoom step).
                if (pressed) {
                    int action = 0;
                    switch (id) {
                        case 0x7A: action = 40112; break;  // Zoom in vertical
                        case 0x7E: action = 40111; break;  // Zoom out vertical
                        case 0x7B: action = 1011;  break;  // Zoom out horizontal
                        case 0x7D: action = 1012;  break;  // Zoom in horizontal
                        case 0x7C: action = 40295; break;  // Zoom to project
                    }
                    if (action) {
                        queueInput({PendingInput::MainAction, 0,
                                    static_cast<double>(action)});
                    }
                }
                handledNatively = true;
            } else if (id == 0x6E) {
                // PAN button: toggle "force all V-Pots to Pan" regardless
                // of SSL-plug-in presence on each track. Invalidates the
                // per-strip caches so the Value Line / V-Pot bar switch
                // to pan-mode rendering on the next tick.
                if (pressed) {
                    g_forcePan.store(!g_forcePan.load());
                    g_pageDirty.store(true);
                }
                handledNatively = true;
            } else if (id >= 0x68 && id <= 0x6D) {
                // V-Pot assignment soft keys (0x68 = V-POT top, 0x69-0x6D =
                // Soft Keys 1-5). Any of these returns from PAN override
                // back to automatic plug-in-param mode. Specific per-key
                // page assignment is a Phase-2 (Config UI) feature; for
                // now we just clear the pan override.
                if (pressed && g_forcePan.load()) {
                    g_forcePan.store(false);
                    g_pageDirty.store(true);
                }
                handledNatively = true;
            } else if (id == 0x52 || id == 0x53) {
                // Page ← (0x52) / Page → (0x53). Shifts the plugin-param
                // page index that `pushZonesForVisibleSlots` reads. Clamped
                // at 0..31 — the longest PluginMap slot list (CS2) has 32
                // entries; plugins with fewer slots just show blank on
                // pages past their end.
                if (pressed) {
                    const int delta = (id == 0x53) ? 1 : -1;
                    int next = g_pageIdx.load() + delta;
                    if (next < 0) next = 0;
                    if (next > 31) next = 31;
                    if (next != g_pageIdx.exchange(next)) {
                        g_pageDirty.store(true);
                    }
                }
                handledNatively = true;
            } else if (id == 0x78 || id == 0x79) {
                // Bank ← (0x78) / Bank → (0x79). 8-strip scroll, clamped
                // so the bank start can go from 0 up to max(0, tracks-1).
                // Allowing up to tracks-1 means the last bank can end
                // with empty slots rather than snapping short of the end.
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
                // V-Pot push. In Pan mode (currently the only mode we
                // implement for the V-Pot) a push resets pan to center.
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
                // Top soft-key above the scribble. PM-mode-specific (selects
                // what the V-Pot controls on SSL plug-ins). We have no
                // equivalent yet — swallow so the firmware's MCU-SELECT
                // overlap doesn't leak to REAPER.
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
            // Touch-mode automation recording would require the extension
            // to register as an IReaperControlSurface and feed touch into
            // REAPER's automation engine — that's a Phase-2 item. For now
            // the touch state is purely local.
            const uint8_t strip = data[i + 3];
            const uint8_t state = data[i + 4];
            if (strip < 8) {
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
                        if (g_dev) g_dev->send(uf8::buildMotorEnable(strip, false));
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

    // Fader pitch-bend: E<ch> <LSB> <MSB>  (3 bytes, channel 0..7 = strip)
    if (bytes.size() == 3 && (bytes[0] & 0xF0) == 0xE0) {
        const uint8_t strip = bytes[0] & 0x07;
        if (strip < 8) {
            g_dev->send(uf8::buildFaderPosition(strip, bytes[1], bytes[2]));
        }
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

// Convert REAPER linear-amplitude volume (0..~4) to a dB string that
// fits the O/PdB zone's 4-char value slot: "-inf", "-6.0", "0.0", "12.0".
//
// REAPER stores fader position as a linear multiplier — 1.0 = 0 dB.
// Below ~10^-5 we call it "-inf" to match what the SSL LCD shows at
// the fader bottom.
std::string formatDbReadout(double linearAmp)
{
    if (linearAmp < 1e-5) return "-inf";
    const double dB = 20.0 * std::log10(linearAmp);
    char buf[8];
    if (std::abs(dB) < 10.0) {
        std::snprintf(buf, sizeof(buf), "%.1f", dB);
    } else {
        std::snprintf(buf, sizeof(buf), "%.0f", dB);
    }
    std::string s(buf);
    if (s.size() > 4) s.resize(4);
    return s;
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
std::array<std::string, 8> g_lastCsType{};
std::array<std::string, 8> g_lastValueLine{};
std::array<std::string, 8> g_lastFaderDb{};
std::array<std::string, 8> g_lastChanNum{};
std::array<uint16_t, 8>    g_lastVPotBar{};      // 16-bit LE per strip
bool                       g_vpotBarInit{false};

// Resolve the LinkSlot for one visible strip at the current page index.
// Returns nullptr when:
//   - no track
//   - PAN mode is forced globally (treat every strip as if it had no plug-in)
//   - the track's plug-in isn't in our PluginMap registry
//   - the current page index is past the plug-in's slot count (e.g. BC2
//     has 7 slots, paging further reveals pan fallback)
// Must be called on the main thread — touches REAPER API.
const uf8::LinkSlot* slotForStrip(MediaTrack* tr, int pageIdx,
                                  int* outFxIdx)
{
    if (!tr) return nullptr;
    if (g_forcePan.load()) return nullptr;
    auto match = uf8::lookupPluginOnTrack(tr);
    if (!match.map) return nullptr;
    if (pageIdx < 0 || static_cast<size_t>(pageIdx) >= match.map->slots.size()) {
        return nullptr;
    }
    if (outFxIdx) *outFxIdx = match.fxIndex;
    return &match.map->slots[pageIdx];
}

// Map a normalised 0..1 to a V-Pot readout-bar 16-bit value. SSL 360°
// encodes the bar as 2 bytes LE per strip in a FF 66 11 0F broadcast.
// Working theory (cap20, 2026-04-21): byte[0] is a LED bitmask where
// bits 0..7 correspond to the 8 LED segments. "Bar fill up to N" =
// (1 << N) - 1. Linear interpretation (byte[0] = round(v * 255)) didn't
// render anything in a live test — the firmware likely only accepts
// specific bit patterns. Needs cap24 to confirm; for now we stay close
// to the bit-mask theory and let the user report back.
uint16_t vpotPosFromNormalized(double v)
{
    if (v < 0.0) v = 0.0;
    if (v > 1.0) v = 1.0;
    int n = static_cast<int>(v * 8.0 + 0.5);
    if (n < 0) n = 0;
    if (n > 8) n = 8;
    // Build a "fill from left" bitmask: n=0 empty, n=8 all 8 bits lit.
    uint16_t mask = static_cast<uint16_t>((n == 8) ? 0xFF : ((1u << n) - 1u));
    return mask;
}

// Pan in [-1, +1] → V-Pot bar 0..255 with centre at bit-4.
uint16_t vpotPosFromPan(double pan)
{
    if (pan < -1.0) pan = -1.0;
    if (pan >  1.0) pan =  1.0;
    int n = static_cast<int>((pan + 1.0) * 4.0 + 0.5);
    if (n < 0) n = 0;
    if (n > 8) n = 8;
    uint16_t mask = static_cast<uint16_t>((n == 8) ? 0xFF : ((1u << n) - 1u));
    return mask;
}

void pushZonesForVisibleSlots()
{
    if (!g_dev || !g_dev->isOpen()) return;

    const int trackCount = CountTracks(nullptr);
    const int bankOffset = g_bankOffset.load();
    const int pageIdx    = g_pageIdx.load();

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
    const bool bankChanged = g_bankDirty.exchange(false);
    const bool pageChanged = g_pageDirty.exchange(false);
    if (pageChanged) {
        g_lastSlotLabel.fill({});
        g_lastValueLine.fill({});
    }
    if (bankChanged) {
        g_lastTrackName.fill({});
        g_lastSlotLabel.fill({});
        g_lastCsType.fill({});
        g_lastValueLine.fill({});
        g_lastFaderDb.fill({});
        g_lastChanNum.fill({});
        g_lastFaderPb.fill(0xFFFF);
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
                g_dev->send(uf8::buildLedCommand(ledIdFor(LedClass::Sel,  s), sel));
                g_dev->send(uf8::buildLedCommand(ledIdFor(LedClass::Mute, s), mute));
                g_dev->send(uf8::buildLedCommand(ledIdFor(LedClass::Solo, s), solo));
                (void)arm;
            }
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
        const uf8::LinkSlot* slot = slotForStrip(tr, pageIdx, &fxIdx);
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

        // Parameter Label (Currently Selected Parameter zone) — also the
        // "populate slot" flag that gates color-bar rendering. When an
        // SSL plug-in is active, use the slot legend ("LPF", "HF", …);
        // otherwise fall back to track name / CH N.
        std::string label;
        if (slot) {
            label = slot->legend;
        } else {
            label = slotLabelForVisibleSlot(s);
        }
        if (label != g_lastSlotLabel[s]) {
            g_lastSlotLabel[s] = label;
            g_dev->send(uf8::buildPluginSlotName(static_cast<uint8_t>(s), label));
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

        // V-Pot Readout Bar position. Plugin param → normalised position;
        // otherwise centre-mapped pan (-1..+1 → 0..14).
        if (slot && fxIdx >= 0) {
            const double norm = TrackFX_GetParamNormalized(tr, fxIdx, slot->vst3Param);
            vpotBar[s] = vpotPosFromNormalized(slot->inverted ? 1.0 - norm : norm);
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

        // O/PdB Fader Readout — track volume in dB. GetTrackUIVolPan
        // returns the UI-displayed value (reflects playback automation),
        // which is what we want on the UF8 display.
        const double volLin = uiVolLinear(tr);
        std::string dbStr = formatDbReadout(volLin);
        if (dbStr != g_lastFaderDb[s]) {
            g_lastFaderDb[s] = dbStr;
            g_dev->send(uf8::buildFaderDbReadout(static_cast<uint8_t>(s), dbStr));
        }

        // Motor echo: push the fader target every tick, including during
        // touch. While the motor is limp the firmware won't physically
        // move the fader, but the target update still lands so the
        // re-enable on release fires into an already-up-to-date target.
        {
            const uint16_t pb = linearVolumeToPb(volLin);
            if (!g_faderPbInit || pb != g_lastFaderPb[s]) {
                g_lastFaderPb[s] = pb;
                const uint8_t lsb = static_cast<uint8_t>(pb & 0x7F);
                const uint8_t msb = static_cast<uint8_t>((pb >> 7) & 0x7F);
                g_dev->send(uf8::buildFaderPosition(static_cast<uint8_t>(s), lsb, msb));
            }
        }

        // Value Line — for SSL plug-ins: slot name + formatted param value
        // ("HF Freq    8.00kHz"). Otherwise: track volume ("Vol   -6.0dB").
        std::string valLine;
        if (slot && fxIdx >= 0) {
            char paramBuf[64] = {0};
            const double norm = TrackFX_GetParamNormalized(tr, fxIdx, slot->vst3Param);
            TrackFX_FormatParamValueNormalized(tr, fxIdx, slot->vst3Param,
                                               norm, paramBuf, sizeof(paramBuf));
            std::string valStr(paramBuf);
            // SSL plug-ins format corner values like "-∞ dB" with UTF-8
            // multi-byte runes that the UF8 LCD can't render (and which
            // glitch the display). Squash anything non-printable-ASCII
            // to '-' so the character count and cursor position stay
            // correct. Trim leading pad spaces while we're at it.
            for (auto& c : valStr) {
                const unsigned char u = static_cast<unsigned char>(c);
                if (u < 0x20 || u > 0x7E) c = '-';
            }
            while (!valStr.empty() && valStr.front() == ' ') valStr.erase(0, 1);
            valLine = composeValueLine(slot->name, valStr);
        } else {
            valLine = composeValueLine("Vol",
                dbStr == "-inf" ? std::string("-inf dB") : dbStr + "dB");
        }
        if (valLine != g_lastValueLine[s]) {
            g_lastValueLine[s] = valLine;
            g_dev->send(uf8::buildValueLine(static_cast<uint8_t>(s), valLine));
        }
    }
    g_faderPbInit = true;

    // V-Pot Readout Bar (FF 66 11 0F). Command + byte layout confirmed
    // in cap20 but the per-strip 16-bit value encoding still eludes us:
    //   - Linear byte[0] 0..255 → flicker within a narrow range
    //   - Bit-mask (1<<n)-1 pattern → renders nothing
    // DISABLED pending cap24: record SSL 360° with plug-in params at
    // specific normalised values (not just V-Pot rotation deltas) to
    // cross-reference. Keep the computation intact so the dedup state
    // stays consistent for when we re-enable.
    (void)vpotBar;
    (void)g_lastVPotBar;
    (void)g_vpotBarInit;
}

void commitDebouncedTouchReleases()
{
    const auto now = std::chrono::steady_clock::now();
    for (uint8_t s = 0; s < 8; ++s) {
        if (!g_touchReleasePending[s].load()) continue;
        if (now - g_touchLastPress[s] < kTouchDebounceQuiet) continue;
        g_touchReleasePending[s].store(false);
        if (!g_touchReported[s].exchange(false)) continue;

        if (!g_dev) continue;
        MediaTrack* tr = g_slotTrack[s];
        if (!tr) continue;

        // Firmware behaviour observed in PM mode: position commands sent
        // while the motor is limp are silently discarded (the target
        // remembered for the eventual re-enable is the last position
        // pushed while the motor was active — i.e. the pre-touch value).
        // Workaround: send enable first and immediately follow with the
        // new position multiple times. The first hundred microseconds
        // after enable the motor briefly heads toward its stale target;
        // the rapid follow-up position commands flip the target before
        // the fader has visibly moved.
        const uint16_t pb  = linearVolumeToPb(uiVolLinear(tr));
        const uint8_t  lsb = static_cast<uint8_t>(pb & 0x7F);
        const uint8_t  msb = static_cast<uint8_t>((pb >> 7) & 0x7F);
        g_dev->send(uf8::buildMotorEnable(s, true));
        for (int i = 0; i < 3; ++i) {
            g_dev->send(uf8::buildFaderPosition(s, lsb, msb));
        }
        g_lastFaderPb[s] = pb;
    }
}

// Linear peak (0..1) → UF8 VU byte (0..31). -60 dBFS → 0, 0 dBFS → 31.
// Uniform log-scale mapping across 60 dB. Matches the visible behaviour
// on SSL's VU strips closely enough for Phase 1.
uint8_t peakToVuByte(double peak)
{
    if (peak <= 0.0) return 0;
    const double dbfs = 20.0 * std::log10(peak);
    if (dbfs >= 0.0)  return 0x1F;
    if (dbfs <= -60.0) return 0x00;
    const double f = (dbfs + 60.0) / 60.0;  // 0..1
    const int byte = static_cast<int>(f * 31.0 + 0.5);
    return static_cast<uint8_t>(std::clamp(byte, 0, 0x1F));
}

std::array<uint8_t, 16> g_lastVuLevels{};
bool g_vuInit = false;

// UF8 GR byte. Source is undefined until a plugin-GR probe is wired —
// keep at 0 so the display doesn't show stale SSL-360°-left state. When
// a GR provider exists it can call pushUf8Gr(byte) to update.
uint8_t g_uf8GrByte = 0;
uint8_t g_lastUf8GrByte = 0xFE;  // sentinel "unset"

void pushVuMeter()
{
    if (!g_dev || !g_dev->isOpen()) return;

    const int trackCount = CountTracks(nullptr);
    const int bankOffset = g_bankOffset.load();
    std::array<uint8_t, 16> levels{};
    for (int s = 0; s < 8; ++s) {
        const int idx = s + bankOffset;
        if (idx >= trackCount) continue;
        MediaTrack* tr = GetTrack(nullptr, idx);
        if (!tr) continue;
        // Left = channel 0, right = channel 1. REAPER's peak is the
        // channel's post-fader tap; pre-fader VU isn't exposed via this
        // call, so "in" and "out" end up mirroring each other for mono
        // fader moves. Good enough until a JSFX probe exposes pre-fader.
        const double pl = Track_GetPeakInfo(tr, 0);
        const double pr = Track_GetPeakInfo(tr, 1);
        levels[s * 2 + 0] = peakToVuByte(pl);     // "input"
        levels[s * 2 + 1] = peakToVuByte(pr);     // "output"
    }
    if (g_vuInit && levels == g_lastVuLevels) return;
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

void onTimer()
{
    drainInputQueue();
    commitDebouncedTouchReleases();
    if (g_sync) g_sync->refresh(reaperColorForVisibleSlot);
    pushZonesForVisibleSlots();
    pushSelColourBar();
    pushVuMeter();
    // UC1 VU — same peak data, mapped to the focused track's L/R
    // channels. Single meter-pair on UC1 (not per-strip).
    if (g_uc1_surface) {
        void* focus = g_uc1_surface->focusedTrack();
        if (focus) {
            MediaTrack* tr = static_cast<MediaTrack*>(focus);
            const double pl = Track_GetPeakInfo(tr, 0);
            const double pr = Track_GetPeakInfo(tr, 1);
            auto peakToDb = [](double p) -> float {
                if (p <= 0.0) return -120.f;
                return static_cast<float>(20.0 * std::log10(p));
            };
            g_uc1_surface->pushCsVu(peakToDb(pl), peakToDb(pr));
        }
    }
    // UF8 GR — push only on change. Without a GR data source we leave
    // g_uf8GrByte at 0 which clears the meter. A future JSFX probe
    // updates the byte via a dedicated setter.
    if (g_dev && g_dev->isOpen() && g_uf8GrByte != g_lastUf8GrByte) {
        g_lastUf8GrByte = g_uf8GrByte;
        g_dev->send(uf8::buildGrByte(g_uf8GrByte));
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

bool hookCommand(int command, int /*flag*/)
{
    if (command == 0) return false;
    if (command == g_cmdBrightnessUp)   { brightnessUp();   return true; }
    if (command == g_cmdBrightnessDown) { brightnessDown(); return true; }
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

    // Register as a full control-surface class. The user adds a
    // "Rea-Sixty" entry in Preferences → Control/OSC/Web; REAPER then
    // calls createReaSixty() to instantiate ReaSixtySurface, which
    // opens the UF8 and starts the timer.
    plugin_register("csurf", &g_csurfReg);

    // Custom actions: brightness up/down. REAPER assigns a command ID
    // when we register — stash it for dispatch in hookCommand.
    g_cmdBrightnessUp   = plugin_register("custom_action", &g_actionBrightnessUp);
    g_cmdBrightnessDown = plugin_register("custom_action", &g_actionBrightnessDown);
    plugin_register("hookcommand", reinterpret_cast<void*>(hookCommand));

    return 1;
}
