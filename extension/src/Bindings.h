#pragma once
//
// Bindings — Phase A: data model + dispatch refactor (no UI yet).
//
// Architecture: see memory bindings-architecture.md. Phase A scope:
//   1. Define data model + ButtonId enum.
//   2. JSON load/save at ~/Library/Application Support/REAPER/rea_sixty/
//      bindings.json.
//   3. Factory defaults that mirror today's hardcoded global-button
//      behaviour exactly (so reload-after-this-commit feels identical).
//   4. Refactor onUf8Input's global-button branches to call dispatch().
//
// Per-strip Sel/Cut/Solo/Rec stay hardcoded in onUf8Input (resolved Q2:
// modifier-customisation is a later phase). Per-strip V-Pot push (0x08..)
// and per-strip top soft-key (0x18..) also stay hardcoded in v1 — the
// PM-mode soft-key tables move into Layer 2/3 default bindings only when
// Phase B/C lands. v1 leaves them alone (architecture risks section).
//

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace uf8::bindings {

// Snake_case stable IDs — the JSON serialization key. Frozen so future
// versions can read older configs. Phase A enumerates only the buttons
// routed through dispatch in v1 (the previously-hardcoded globals).
// Phase B/C will widen the catalogue (per-strip, transport, layer
// buttons) without breaking older config files.
enum class ButtonId : uint16_t {
    None = 0,

    // Bank/Page
    BankLeft, BankRight,
    PageLeft, PageRight,

    // Layer (Phase B)
    Layer1, Layer2, Layer3,

    // Quick (selection-mode row above the channel encoder)
    Quick1, Quick2, Quick3,

    // Plugin / mode
    PluginBtn,
    Flip, Pan, Fine, Btn360,

    // Automation row
    AutoOff, AutoRead, AutoWrite, AutoTrim, AutoLatch, AutoTouch,

    // Zoom pad
    ZoomUp, ZoomDown, ZoomLeft, ZoomRight, ZoomCenter,

    // Encoder modes (channel encoder above transport)
    Nav, Nudge, EncFocus, ChannelPush,

    // Send/Plugin row — 8 buttons under the V-Pots, used by SSL 360°
    // for plug-in slot selection. Rea-Sixty defaults them to the
    // send_all_N routing builtins so each button switches all V-Pots
    // (or Faders, when Flip is enabled) to a different send index.
    SendPlugin1, SendPlugin2, SendPlugin3, SendPlugin4,
    SendPlugin5, SendPlugin6, SendPlugin7, SendPlugin8,

    // Channel — sits next to PLUGIN. Default action: HOME (clears every
    // routing toggle so V-Pots / faders return to track volume + pan).
    Channel,

    // Top-soft-keys: the 8 buttons above the V-Pots (one per strip,
    // device IDs 0x18..0x1F). Default action: ssl_softkey with
    // param=strip — focuses the SSL plug-in param at this strip's
    // position in the current PAGE bank, matching SSL 360°'s native
    // behaviour. User can rebind to anything via Settings → Bindings.
    TopSoftKey1, TopSoftKey2, TopSoftKey3, TopSoftKey4,
    TopSoftKey5, TopSoftKey6, TopSoftKey7, TopSoftKey8,

    // SSL plug-in soft-key bank selectors — V-POT (cell 0x68) plus
    // Bank 1..5 (0x69..0x6D). Default action: softkey_bank_select
    // with param 0..5 to switch g_softKeyBank, matching SSL 360°.
    // User can rebind to anything (jump to user bank, fire arbitrary
    // action, etc.).
    VPotBank,
    SoftKey1Bank, SoftKey2Bank, SoftKey3Bank, SoftKey4Bank, SoftKey5Bank,

    // Channel encoder rotation — not a press-event button; dispatched
    // via dispatchEncoder(stepDelta). Carries 4 modifier slots like any
    // ButtonId, so Plain (default) / Shift / Cmd / Ctrl can each map to
    // a different action. Plain defaults to `encoder_mode_dispatch`
    // (preserves the legacy Nav/Nudge/Focus/Instance mode system);
    // Shift defaults to `instance_cycle`. Cmd/Ctrl empty until user
    // binds them in Settings.
    ChannelEncoder,
};

// Map UF8 device byte (FF 22 03 <id> 00 <s>) to ButtonId. Returns None
// if the id isn't a v1-bindable global — caller falls through to whatever
// the existing legacy dispatch does (per-strip, MCU passthrough, etc.).
ButtonId fromUf8DeviceId(uint8_t id);

// Snake_case name for JSON. Stable across versions.
const char* toName(ButtonId id);

// Inverse of toName. None if unknown — used to silently skip stale keys
// from JSON.
ButtonId fromName(const char* name);

enum class ActionType : uint8_t { Noop, Reaper, Keyboard, Builtin, Midi };
enum class Behavior   : uint8_t { Momentary, Toggle, Hold };

// LED brightness for the button while idle (when not lit by an active
// state like Toggle=on or Hold=down). Mirrors GlobalLedState in
// Protocol.h but lives here so the editor can persist a user choice
// independent of which device class the button lives on.
enum class Brightness : uint8_t { Off, Dim, Bright };

// MIDI message presets — the UI surfaces these as a combo so users
// don't have to remember status-byte hex.
enum class MidiMsgType : uint8_t {
    NoteOn = 0,
    NoteOff,
    ControlChange,
    ProgramChange,
};

// One step of an action chain. `wait_ms` is the gap AFTER this step
// before the next step fires (ignored on the chain's last step).
struct ActionStep {
    ActionType  type     = ActionType::Noop;
    std::string action;        // builtin name / REAPER action id / keyboard chord
    int         param    = 0;
    // Per-step scribble label. Step 0 doubles as the slot's label (the
    // legacy single-action case): shown on the UF8 LCD when this slot is
    // the one a press will fire. Empty = fall back to the binding's
    // top-level Binding::label.
    std::string label;
    // MIDI command fields — read only when `type == Midi`.
    //   midiDevice  output device name (REAPER GetMIDIOutputName, "" = all)
    //   midiChannel 1..16 (stored 1-based for human-readability)
    //   midiMsgType see MidiMsgType — picks status nibble + interpretation
    //   midiData1   note number / CC number / program number
    //   midiData2   velocity / CC value (ignored for ProgramChange)
    std::string midiDevice;
    int         midiChannel = 1;
    int         midiMsgType = 0;
    int         midiData1   = 60;
    int         midiData2   = 127;
    // Delay BEFORE the next step in the chain fires. 0 = back-to-back.
    int         wait_ms     = 0;
};

// Optional per-slot LED override. Each (color, brightness) pair is
// independently opt-in; when unset the slot inherits the Binding's
// top-level LED config. Lets the editor surface distinct LEDs for
// e.g. base / longpress / modifier+long without needing four full
// Binding entries.
struct LedOverride {
    bool        hasActive            = false;
    uint8_t     color[3]             = {0xFF, 0xFF, 0xFF};
    Brightness  brightness           = Brightness::Bright;
    bool        hasInactive          = false;
    uint8_t     inactiveColor[3]     = {0xFF, 0xFF, 0xFF};
    Brightness  inactiveBrightness   = Brightness::Dim;
};

// One actionable cell of a binding. Holds an ordered list of steps —
// chain length 1 (the default) reproduces the legacy "fires one action"
// semantics; longer chains run sequentially with optional waits.
//
// Inherits from ActionStep so existing call-sites that read `slot.type`,
// `slot.action`, `slot.param`, `slot.label`, `slot.midi*` keep working
// — those fields ARE the chain's step 0. Additional steps live in
// `extraSteps`. The LED override is per-slot (per modifier × press
// combo), not per chain step.
struct ActionSlot : ActionStep {
    std::vector<ActionStep> extraSteps;
    LedOverride             led;
};

// Chain helpers — treat the slot as a contiguous N-step list. stepCount
// is always >= 1; stepAt(0) is the slot's inline step, stepAt(i>0) walks
// extraSteps. Used by dispatch and by the editor.
inline int stepCount(const ActionSlot& s) {
    return 1 + static_cast<int>(s.extraSteps.size());
}
inline const ActionStep& stepAt(const ActionSlot& s, int i) {
    return (i <= 0) ? static_cast<const ActionStep&>(s)
                    : s.extraSteps[static_cast<size_t>(i - 1)];
}
inline ActionStep& stepAt(ActionSlot& s, int i) {
    return (i <= 0) ? static_cast<ActionStep&>(s)
                    : s.extraSteps[static_cast<size_t>(i - 1)];
}

// Modifier-prefix index into Binding::shortPress / Binding::longPress.
// Plain slot = no modifier held at press-time. Order matches a fixed
// precedence used by dispatch when multiple modifiers are simultaneously
// held: Ctrl > Cmd > Shift (most-specific wins).
enum class Modifier : uint8_t {
    Plain = 0,
    Shift = 1,
    Cmd   = 2,
    Ctrl  = 3,
};
constexpr int kModifierCount = 4;

struct Binding {
    Behavior    behavior = Behavior::Momentary;
    std::string label;

    // LED appearance — split active / inactive state.
    //   Active   = the "engaged" visual: Toggle=on, Hold=held, Momentary
    //              while-pressed. Defaults to white@Bright.
    //   Inactive = the idle visual. Defaults to (active colour) @ Dim,
    //              matching SSL 360°'s baseline where unlit buttons glow
    //              dim so they remain visible.
    // Colours are 24-bit RGB; emitter quantises to the UF8 10-colour
    // palette (see Protocol.cpp selPaletteRgb).
    uint8_t     color[3]            = {0xFF, 0xFF, 0xFF};
    Brightness  brightness          = Brightness::Bright;
    uint8_t     inactiveColor[3]    = {0xFF, 0xFF, 0xFF};
    Brightness  inactiveBrightness  = Brightness::Dim;

    // 4×2 action matrix — [Modifier index][short=0 / long=1].
    //   shortPress[m] : fires on release-edge (or while-held for Hold
    //                    behaviour). The modifier `m` is snapshotted at
    //                    PRESS time and re-used for the release decision.
    //   longPress[m]  : fires when the press exceeds the long-press
    //                    threshold (~500 ms). Same modifier snapshot.
    // Behavior::Toggle / ::Hold collapse to shortPress[Plain] only;
    // hasLongPress is forced false and the editor greys out the rest.
    bool        hasLongPress = false;
    ActionSlot  shortPress[kModifierCount];
    ActionSlot  longPress[kModifierCount];

    // LED-when-empty override. Default false → if the binding has no
    // action in any slot, the LED stays OFF. When the user wants to
    // show the configured Inactive colour even on an unbound button
    // (e.g. as a hardware label glow) they tick this checkbox.
    bool        ledShowWhenEmpty = false;
};

// User-defined Soft-Key Bank. The 8 slots map 1:1 onto the top-soft-key
// row above the strips (one per V-Pot column). Each slot is a full
// Binding so it carries the same modifier matrix, long-press, and LED
// config as a regular button binding. The bank's `name` is what the
// editor lists and what pushZonesForVisibleSlots can show next to the
// active-bank indicator. Slots default to empty (no action). 12 banks
// total per Config — matches the user's "12 storage slots" ask.
constexpr int kUserBankCount   = 12;
constexpr int kUserBankSlots   = 8;

struct UserBank {
    std::string name;                       // user label, e.g. "Vocals", "Drums"
    Binding     slots[kUserBankSlots];      // 0..7 = top-soft-key positions
};

struct Layer {
    std::string name;
    bool        autoWhenMixerVisible = false;
    std::string vpotDefaultMode      = "pan";
    std::unordered_map<ButtonId, Binding> bindings;
};

struct Config {
    int      version     = 1;
    int      activeLayer = 0;
    Layer    layers[3];
    UserBank userBanks[kUserBankCount];
};

// Builtin registry. Phase A registers from main.cpp at REAPER_PLUGIN_ENTRY
// so handlers reach main.cpp's atomics + queueInput + sendLedFrames
// directly. Phase B+ may move handlers into a dedicated TU once patterns
// stabilise.
//
// Handler args:
//   firing   true when the action should run NOW (press-edge for
//            Momentary; on each state change for Toggle; on each
//            press-or-release for Hold)
//   pressed  current button physical state (relevant for Hold)
//   param    Binding.param, action-specific
struct BuiltinDescriptor {
    using Run     = std::function<void(bool firing, bool pressed, int param)>;
    using StateOf = std::function<bool(int param)>;
    Run         run;
    StateOf     stateOf;       // may be empty; consumed by Phase B LED-pusher
    std::string displayName;   // human-friendly label for the picker UI
    bool        usesParam = false;  // hide param field in UI when false
};

void registerBuiltin(const char* name, BuiltinDescriptor desc);

// Look up a builtin's display name. Falls back to the canonical name
// if the builtin isn't registered (UI then shows the snake_case name).
std::string builtinDisplayName(const std::string& name);

// Whether this builtin reads its `param` arg. UI uses this to hide the
// param column for buttons whose action is param-less.
bool builtinUsesParam(const std::string& name);

// Resolve a builtin's "is currently active?" state — used by the LED
// pusher so a button bound to e.g. `encoder_instance` lights bright
// when the encoder is actually in Instance mode. Returns false when
// the builtin doesn't expose a stateOf (one-shot actions) or when the
// name isn't registered.
bool builtinStateOf(const std::string& name, int param);

// Whether this builtin has a queryable state. Lets the LED pusher
// distinguish "stateful action that's currently false" (LED stays
// inactive) from "stateless action that just runs on press"
// (LED renders the binding's active appearance continuously, so the
// user's chosen colour is visible — without this, every zoom /
// bank / page button stays at the inactive default).
bool builtinHasState(const std::string& name);

// ---- lifecycle / API -------------------------------------------------------

// Load JSON from disk; on missing/corrupt file seed factory defaults and
// write them back. Idempotent.
void load();

// Write the current Config to disk.
void save();

// Portable export / import — read & write the same JSON as the on-disk
// config but to an arbitrary path so users can move bindings between
// machines. exportTo writes the current Config; importFrom replaces the
// active Config with the contents of `path` (and persists to the
// regular configPath). Both return false on I/O / parse errors.
bool exportTo(const std::string& path);
bool importFrom(const std::string& path);

// Modifier state — set by main.cpp's mod_shift / mod_cmd / mod_ctrl
// builtin handlers when their button is pressed/released. Read by
// dispatch() at press-edge to snapshot the current modifier into the
// in-flight press record. Atomics so the libusb input thread can
// publish without locking.
void     setModifierHeld(Modifier m, bool held);
bool     modifierHeld(Modifier m);
Modifier currentModifierSnapshot();

// Per-layer variants. exportLayerTo writes a single layer wrapped in a
// {"version":1,"type":"layer","index":N,"layer":{…}} object so the
// importer can refuse a mismatched payload. importLayerFrom reads the
// file, replaces the named layer in the active Config, and persists.
// Returns false on I/O, parse, or layer-index range errors.
bool exportLayerTo(int layer, const std::string& path);
bool importLayerFrom(int layer, const std::string& path);

const Config& get();

// Active-layer accessors. Phase B exposes these so the Layer LED-pusher
// in main.cpp can mirror state and so the layer_select_<n> builtin can
// drive the switch. setActiveLayer persists to JSON; for transient
// switches (mixer auto-switch) call onMixerVisibilityChanged instead.
int  getActiveLayer();
void setActiveLayer(int layer);

// Dispatch a hardware button event. `pressed` is true on press-edge,
// false on release-edge. Returns true if `id` is bound on the active
// layer (caller marks event as handled); false if no binding (caller
// falls through to legacy paths).
bool dispatch(ButtonId id, bool pressed);

// Dispatch a hardware encoder rotation event — fires the bound
// builtin's run() with `param = stepDelta` (signed integer detents).
// Reads currentModifierSnapshot() to pick the modifier slot. Returns
// true if `id` is bound and a builtin was fired; false otherwise.
// Encoder-aware builtins consume stepDelta as their effective param;
// trigger-only builtins (toggle/etc.) ignore it and just fire on each
// detent. Cmd / Ctrl modifier slots empty by default — user-bindable
// in Settings → Bindings → Channel Encoder.
bool dispatchEncoder(ButtonId id, int stepDelta);

// Mixer-window visibility change hook. Walks Layers 2/3 looking for
// `auto_when_mixer_visible=true`; on first match saves the current
// active layer and switches to the flagged one. On `visible=false`
// restores the saved layer. Manual layer switches via setActiveLayer
// invalidate the save (manual override wins on close).
void onMixerVisibilityChanged(bool visible);

// ---- Phase C mutator API (Settings → Bindings tab) ------------------------

// Read a copy of a single binding (Phase C UI snapshots state per row
// each frame). Returns a default-constructed Binding if no entry exists.
Binding getBinding(int layer, ButtonId id);

// Whether the layer has an entry for this id at all. Distinguishes
// "user has touched this binding" (entry exists, even if all fields
// are at defaults) from "untouched" (no entry, getBinding would
// return Binding{}). The LED pusher uses this to decide whether to
// honour the binding's appearance (entry exists → user-customised)
// or fall through to the cell's table-default colour (no entry →
// firmware/factory look). Without this distinction, a user who
// picks white deliberately on a non-white-table cell can't get
// white because Binding{} also has white as default.
bool hasBinding(int layer, ButtonId id);

// User Soft-Key Bank accessors. bankIdx 0..kUserBankCount-1, slotIdx
// 0..kUserBankSlots-1. Out-of-range indices return defaults / silently
// ignore writes.
UserBank getUserBank(int bankIdx);
void     setUserBank(int bankIdx, const UserBank& bank);
Binding  getUserBankSlot(int bankIdx, int slotIdx);
void     setUserBankSlot(int bankIdx, int slotIdx, const Binding& bd);

// Dispatch a press/release through a user-bank slot. Same long-press
// + modifier-matrix logic as dispatch(ButtonId), but the slot is
// addressed by (bankIdx, slotIdx) so the press timer keys stay
// distinct from layer-button presses. Returns true if the slot
// actually had an action to fire (lets the caller fall through to
// legacy MCU passthrough for empty slots if it wants).
bool     dispatchUserBankSlot(int bankIdx, int slotIdx, bool pressed);

// Replace (or insert) a single binding and persist. Caller is the UI;
// any in-flight USB-thread dispatch holding the lock blocks briefly.
void setBinding(int layer, ButtonId id, const Binding& bd);

// Remove a binding and persist. Equivalent to "unbind" — the next press
// of that button falls through to legacy MCU passthrough on that layer.
void clearBinding(int layer, ButtonId id);

// Per-layer settings.
void setLayerName(int layer, const std::string& name);
void setLayerVpotDefaultMode(int layer, const std::string& mode);

// auto_when_mixer_visible toggle. Enforces the architectural invariant
// "at most one layer flagged" — turning Layer 2 on automatically clears
// Layer 3 and vice versa. Layer 0 (Layer 1) ignores the setter.
void setLayerAutoMixer(int layer, bool flag);

// Re-seed a single layer with the factory defaults (Layer 1 = full
// catalogue, Layers 2/3 = layer_select bindings only). Persists.
void resetLayerToDefaults(int layer);

// List of registered builtin names — Phase C UI uses this to populate
// the action-picker combo. Internal sentinel names (anything starting
// with `__`) are filtered out.
std::vector<std::string> builtinNames();

// LED resolution helpers — slot's override wins, else falls back to
// the binding's top-level LED config. Out-params are written
// unconditionally so callers don't need to branch.
void effectiveLedActive(const Binding& bd, const ActionSlot& slot,
                        uint8_t (&rgb)[3], Brightness& bri);
void effectiveLedInactive(const Binding& bd, const ActionSlot& slot,
                          uint8_t (&rgb)[3], Brightness& bri);

// Drain pending multi-action chains. Called from main.cpp's onTimer at
// ~30 Hz — fires any chain step whose `fireAt` has elapsed. Single-step
// chains never sit in the queue (they run synchronously in dispatch).
void tickPending();

// Monotonic counter bumped on every Config mutation (setBinding,
// clearBinding, layer setters, load, importFrom, importLayerFrom).
// main.cpp polls this in pushUf8GlobalLeds and forces a full LED
// re-push on a delta — so colour edits in Settings → Bindings reach
// the hardware on the next tick instead of waiting for a button press
// to dirty the dedup cache.
uint64_t generation();

// Modifier of the last action that ACTUALLY fired on this button
// (slot type != Noop). Returns Modifier::Plain when no fire has
// happened yet on this id. Used by main.cpp's LED pusher to resolve
// the active-state colour from the slot whose action is currently
// engaged — Shift+press of a Toggle button keeps the LED showing
// the Shift slot's active colour after release.
Modifier lastFiredModifier(ButtonId id);

} // namespace uf8::bindings
