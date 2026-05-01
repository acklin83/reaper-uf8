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

enum class ActionType : uint8_t { Noop, Reaper, Keyboard, Builtin };
enum class Behavior   : uint8_t { Momentary, Toggle, Hold };

struct Binding {
    ActionType  type     = ActionType::Noop;
    Behavior    behavior = Behavior::Momentary;
    std::string action;        // builtin name / REAPER action id / keyboard chord
    int         param    = 0;
    std::string label;
    uint8_t     color[3] = {0, 0, 0};
};

struct Layer {
    std::string name;
    bool        autoWhenMixerVisible = false;
    std::string vpotDefaultMode      = "pan";
    std::unordered_map<ButtonId, Binding> bindings;
};

struct Config {
    int   version     = 1;
    int   activeLayer = 0;
    Layer layers[3];
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
    Run     run;
    StateOf stateOf;   // may be empty; consumed by Phase B LED-pusher
};

void registerBuiltin(const char* name, BuiltinDescriptor desc);

// ---- lifecycle / API -------------------------------------------------------

// Load JSON from disk; on missing/corrupt file seed factory defaults and
// write them back. Idempotent.
void load();

// Write the current Config to disk.
void save();

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

} // namespace uf8::bindings
