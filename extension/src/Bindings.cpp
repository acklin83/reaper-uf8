//
// Bindings — Phase A implementation. See Bindings.h for the architecture.
//
// JSON is loaded with WDL's wdl_json_parser (already vendored under
// extension/vendor/WDL). Writing is a small hand-written serializer
// since the schema is shallow.
//
// Config path: <REAPER resource path>/rea_sixty/bindings.json
//   macOS:   ~/Library/Application Support/REAPER/rea_sixty/bindings.json
//   Windows: %APPDATA%/REAPER/rea_sixty/bindings.json
//

#include "Bindings.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <unordered_map>
#include <vector>

#include "reaper_plugin_functions.h"

#include "WDL/jsonparse.h"

namespace uf8::bindings {

namespace {

// ---- ButtonId <-> snake_case name -----------------------------------------

struct NameEntry {
    ButtonId id;
    const char* name;
};

constexpr NameEntry kNames[] = {
    { ButtonId::BankLeft,    "bank_left"    },
    { ButtonId::BankRight,   "bank_right"   },
    { ButtonId::PageLeft,    "page_left"    },
    { ButtonId::PageRight,   "page_right"   },
    { ButtonId::Layer1,      "layer_1"      },
    { ButtonId::Layer2,      "layer_2"      },
    { ButtonId::Layer3,      "layer_3"      },
    { ButtonId::Quick1,      "quick_1"      },
    { ButtonId::Quick2,      "quick_2"      },
    { ButtonId::Quick3,      "quick_3"      },
    { ButtonId::PluginBtn,   "plugin_btn"   },
    { ButtonId::Flip,        "flip"         },
    { ButtonId::Pan,         "pan"          },
    { ButtonId::Fine,        "fine"         },
    { ButtonId::Btn360,      "btn_360"      },
    { ButtonId::AutoOff,     "auto_off"     },
    { ButtonId::AutoRead,    "auto_read"    },
    { ButtonId::AutoWrite,   "auto_write"   },
    { ButtonId::AutoTrim,    "auto_trim"    },
    { ButtonId::AutoLatch,   "auto_latch"   },
    { ButtonId::AutoTouch,   "auto_touch"   },
    { ButtonId::ZoomUp,      "zoom_up"      },
    { ButtonId::ZoomDown,    "zoom_down"    },
    { ButtonId::ZoomLeft,    "zoom_left"    },
    { ButtonId::ZoomRight,   "zoom_right"   },
    { ButtonId::ZoomCenter,  "zoom_center"  },
    { ButtonId::Nav,         "nav"          },
    { ButtonId::Nudge,       "nudge"        },
    { ButtonId::EncFocus,    "focus"        },
    { ButtonId::ChannelPush, "channel_push" },
    { ButtonId::SendPlugin1, "send_plugin_1" },
    { ButtonId::SendPlugin2, "send_plugin_2" },
    { ButtonId::SendPlugin3, "send_plugin_3" },
    { ButtonId::SendPlugin4, "send_plugin_4" },
    { ButtonId::SendPlugin5, "send_plugin_5" },
    { ButtonId::SendPlugin6, "send_plugin_6" },
    { ButtonId::SendPlugin7, "send_plugin_7" },
    { ButtonId::SendPlugin8, "send_plugin_8" },
    { ButtonId::Channel,     "channel"      },
    { ButtonId::TopSoftKey1, "top_soft_1"   },
    { ButtonId::TopSoftKey2, "top_soft_2"   },
    { ButtonId::TopSoftKey3, "top_soft_3"   },
    { ButtonId::TopSoftKey4, "top_soft_4"   },
    { ButtonId::TopSoftKey5, "top_soft_5"   },
    { ButtonId::TopSoftKey6, "top_soft_6"   },
    { ButtonId::TopSoftKey7, "top_soft_7"   },
    { ButtonId::TopSoftKey8, "top_soft_8"   },
    { ButtonId::VPotBank,      "vpot_bank"      },
    { ButtonId::SoftKey1Bank,  "softkey_bank_1" },
    { ButtonId::SoftKey2Bank,  "softkey_bank_2" },
    { ButtonId::SoftKey3Bank,  "softkey_bank_3" },
    { ButtonId::SoftKey4Bank,  "softkey_bank_4" },
    { ButtonId::SoftKey5Bank,  "softkey_bank_5" },
    { ButtonId::ChannelEncoder, "channel_encoder" },
};

} // namespace

const char* toName(ButtonId id)
{
    for (auto& e : kNames) if (e.id == id) return e.name;
    return "";
}

ButtonId fromName(const char* name)
{
    if (!name) return ButtonId::None;
    for (auto& e : kNames) if (std::strcmp(e.name, name) == 0) return e.id;
    return ButtonId::None;
}

ButtonId fromUf8DeviceId(uint8_t id)
{
    switch (id) {
        case 0x6F: return ButtonId::Fine;
        case 0x73: return ButtonId::Nav;
        case 0x74: return ButtonId::Nudge;
        case 0x75: return ButtonId::EncFocus;
        case 0x76: return ButtonId::ChannelPush;
        case 0x58: return ButtonId::AutoOff;
        case 0x59: return ButtonId::AutoRead;
        case 0x5A: return ButtonId::AutoWrite;
        case 0x5B: return ButtonId::AutoTrim;
        case 0x5C: return ButtonId::AutoLatch;
        case 0x5D: return ButtonId::AutoTouch;
        case 0x7A: return ButtonId::ZoomUp;
        case 0x7E: return ButtonId::ZoomDown;
        case 0x7B: return ButtonId::ZoomLeft;
        case 0x7D: return ButtonId::ZoomRight;
        case 0x7C: return ButtonId::ZoomCenter;
        case 0x54: return ButtonId::Flip;
        case 0x50: return ButtonId::PluginBtn;
        case 0x46: return ButtonId::Btn360;
        case 0x6E: return ButtonId::Pan;
        case 0x43: return ButtonId::Quick1;
        case 0x44: return ButtonId::Quick2;
        case 0x45: return ButtonId::Quick3;
        case 0x52: return ButtonId::PageLeft;
        case 0x53: return ButtonId::PageRight;
        case 0x78: return ButtonId::BankLeft;
        case 0x79: return ButtonId::BankRight;
        case 0x40: return ButtonId::Layer1;
        case 0x41: return ButtonId::Layer2;
        case 0x42: return ButtonId::Layer3;
        // Send/Plugin row 0x48..0x4F (docs/buttons-leds-quickref.md).
        case 0x48: return ButtonId::SendPlugin1;
        case 0x49: return ButtonId::SendPlugin2;
        case 0x4A: return ButtonId::SendPlugin3;
        case 0x4B: return ButtonId::SendPlugin4;
        case 0x4C: return ButtonId::SendPlugin5;
        case 0x4D: return ButtonId::SendPlugin6;
        case 0x4E: return ButtonId::SendPlugin7;
        case 0x4F: return ButtonId::SendPlugin8;
        case 0x51: return ButtonId::Channel;
        // Top-soft-keys 0x18..0x1F (one per strip, above the V-Pots).
        case 0x18: return ButtonId::TopSoftKey1;
        case 0x19: return ButtonId::TopSoftKey2;
        case 0x1A: return ButtonId::TopSoftKey3;
        case 0x1B: return ButtonId::TopSoftKey4;
        case 0x1C: return ButtonId::TopSoftKey5;
        case 0x1D: return ButtonId::TopSoftKey6;
        case 0x1E: return ButtonId::TopSoftKey7;
        case 0x1F: return ButtonId::TopSoftKey8;
        // SSL plug-in soft-key bank selectors 0x68..0x6D.
        case 0x68: return ButtonId::VPotBank;
        case 0x69: return ButtonId::SoftKey1Bank;
        case 0x6A: return ButtonId::SoftKey2Bank;
        case 0x6B: return ButtonId::SoftKey3Bank;
        case 0x6C: return ButtonId::SoftKey4Bank;
        case 0x6D: return ButtonId::SoftKey5Bank;
        default:   return ButtonId::None;
    }
}

namespace {

// ---- Behavior / ActionType <-> string -------------------------------------

const char* behaviorName(Behavior b)
{
    switch (b) {
        case Behavior::Momentary: return "momentary";
        case Behavior::Toggle:    return "toggle";
        case Behavior::Hold:      return "hold";
    }
    return "momentary";
}

Behavior behaviorFromName(const char* s)
{
    if (!s) return Behavior::Momentary;
    if (std::strcmp(s, "toggle") == 0) return Behavior::Toggle;
    if (std::strcmp(s, "hold")   == 0) return Behavior::Hold;
    return Behavior::Momentary;
}

const char* actionTypeName(ActionType t)
{
    switch (t) {
        case ActionType::Noop:     return "noop";
        case ActionType::Reaper:   return "reaper";
        case ActionType::Keyboard: return "keyboard";
        case ActionType::Builtin:  return "builtin";
        case ActionType::Midi:     return "midi";
    }
    return "noop";
}

ActionType actionTypeFromName(const char* s)
{
    if (!s) return ActionType::Noop;
    if (std::strcmp(s, "reaper")   == 0) return ActionType::Reaper;
    if (std::strcmp(s, "keyboard") == 0) return ActionType::Keyboard;
    if (std::strcmp(s, "builtin")  == 0) return ActionType::Builtin;
    if (std::strcmp(s, "midi")     == 0) return ActionType::Midi;
    return ActionType::Noop;
}

const char* brightnessName(Brightness b)
{
    switch (b) {
        case Brightness::Off:    return "off";
        case Brightness::Dim:    return "dim";
        case Brightness::Bright: return "bright";
    }
    return "bright";
}

Brightness brightnessFromName(const char* s)
{
    if (!s) return Brightness::Bright;
    if (std::strcmp(s, "off") == 0) return Brightness::Off;
    if (std::strcmp(s, "dim") == 0) return Brightness::Dim;
    return Brightness::Bright;
}

// ---- Module state ---------------------------------------------------------

std::mutex                                 g_cfgMutex;
Config                                     g_cfg;
std::unordered_map<std::string, BuiltinDescriptor> g_builtins;

// Mixer auto-switch save slot. -1 means "no transient swap in effect".
// When the mixer opens and a Layer 2/3 has auto_when_mixer_visible=true,
// we stash the currently-active layer here and flip activeLayer to the
// flagged one. On mixer close (or a manual layer press in the meantime)
// we restore (or invalidate) this slot.
int g_savedLayer = -1;

// Long-press support — measured from press-edge to release-edge per
// (layer, button-id). dispatch() runs single-threaded on the libusb
// worker thread, so this map needs no locking. Threshold is 500 ms
// (matches generic "tap vs hold" UX expectations).
constexpr std::chrono::milliseconds kLongPressThreshold{500};

// Per-press record so the long-press path knows both WHEN the press
// started AND WHICH modifier was held at press time. Snapshot stays
// stable across the press window even if the user releases the
// modifier mid-hold — gives predictable Shift+button semantics.
struct PressRecord {
    std::chrono::steady_clock::time_point start;
    Modifier                              mod;
};
std::unordered_map<uint32_t, PressRecord> g_pressStart;

// Modifier state, set by main.cpp's mod_shift / mod_cmd / mod_ctrl
// builtin handlers. dispatch reads currentModifierSnapshot() at press
// edge; precedence at snapshot time is Ctrl > Cmd > Shift > Plain so
// the most specific bind wins when multiple modifiers are held.
std::atomic<bool> g_modShiftHeld{false};
std::atomic<bool> g_modCmdHeld  {false};
std::atomic<bool> g_modCtrlHeld {false};

// Monotonic counter bumped on every mutation of g_cfg (setBinding,
// clearBinding, layer setters, load, importFrom). main.cpp reads this
// in pushUf8GlobalLeds and invalidates its dedup cache on a delta so
// LED-colour edits in Settings → Bindings reach the hardware on the
// next tick instead of waiting for a press to dirty the state.
std::atomic<uint64_t> g_bindingsGen{0};

// Per-ButtonId modifier of the last action that ACTUALLY fired (slot
// type != Noop). Lets the LED pusher resolve the active-state colour
// from the slot whose action is engaged — Shift+press of a Toggle
// button now keeps the LED showing the Shift slot's active colour
// after release, instead of falling back to Plain. Sized to 256 to
// cover any future ButtonId additions without resizing.
constexpr size_t kLastFiredModSize = 256;
std::array<std::atomic<uint8_t>, kLastFiredModSize> g_lastFiredMod{};

uint32_t pressKey(int layer, ButtonId id)
{
    return (static_cast<uint32_t>(layer) << 16)
         | static_cast<uint32_t>(static_cast<uint16_t>(id));
}

// ---- Factory defaults -----------------------------------------------------

Binding mkBuiltin(const char* name, Behavior b, const char* label,
                  uint8_t r = 255, uint8_t g = 255, uint8_t b_ = 255,
                  int param = 0)
{
    Binding bd;
    bd.behavior = b;
    bd.label    = label;
    bd.color[0] = r; bd.color[1] = g; bd.color[2] = b_;
    bd.inactiveColor[0] = r;
    bd.inactiveColor[1] = g;
    bd.inactiveColor[2] = b_;
    auto& s = bd.shortPress[static_cast<int>(Modifier::Plain)];
    s.type   = ActionType::Builtin;
    s.action = name;
    s.param  = param;
    return bd;
}

void seedFactoryDefaults_(Config& c)
{
    c = Config{};
    c.version     = 2;
    c.activeLayer = 0;
    c.layers[0].name = "Layer 1";
    c.layers[1].name = "Layer 2";
    c.layers[2].name = "Layer 3";

    // Layer-select bindings live on ALL three layers so the user can
    // always navigate back even on the otherwise-empty Layer 2/3
    // scaffolds. Each press commits through setActiveLayer → persists.
    // Layer button LED state is driven by main.cpp's pushUf8GlobalLeds
    // based on getActiveLayer().
    for (int li = 0; li < 3; ++li) {
        auto& L = c.layers[li].bindings;
        L[ButtonId::Layer1] = mkBuiltin("layer_select_1", Behavior::Momentary, "LAYER 1");
        L[ButtonId::Layer2] = mkBuiltin("layer_select_2", Behavior::Momentary, "LAYER 2");
        L[ButtonId::Layer3] = mkBuiltin("layer_select_3", Behavior::Momentary, "LAYER 3");
    }

    auto& L1 = c.layers[0].bindings;

    // Fine / Shift modifier (hold).
    L1[ButtonId::Fine] = mkBuiltin("fine_modifier", Behavior::Hold, "FINE");

    // Encoder modes (momentary press = enter mode).
    L1[ButtonId::Nav]         = mkBuiltin("encoder_nav",   Behavior::Momentary, "NAV");
    L1[ButtonId::Nudge]       = mkBuiltin("encoder_nudge", Behavior::Momentary, "NUDGE");
    L1[ButtonId::EncFocus]    = mkBuiltin("encoder_focus", Behavior::Momentary, "FOCUS");
    L1[ButtonId::ChannelPush] = mkBuiltin("encoder_nav",   Behavior::Momentary, "");

    // Channel encoder rotation. Plain = mode-dispatch (preserves the
    // legacy Nav/Nudge/Focus/Instance mode system). Shift = direct
    // instance cycle (was hardcoded). Cmd / Ctrl = unbound, user picks
    // any builtin in Settings → Bindings → Channel Encoder.
    {
        auto& ce = L1[ButtonId::ChannelEncoder];
        ce.behavior = Behavior::Momentary;
        ce.label    = "Encoder";
        auto& spPlain = ce.shortPress[static_cast<int>(Modifier::Plain)];
        spPlain.type   = ActionType::Builtin;
        spPlain.action = "encoder_mode_dispatch";
        auto& spShift = ce.shortPress[static_cast<int>(Modifier::Shift)];
        spShift.type   = ActionType::Builtin;
        spShift.action = "instance_cycle";
    }

    // Automation row — one builtin per mode. Factory colours all white;
    // the user sets each LED themselves via Settings → Bindings (Frank
    // 2026-05-07: explicitly does NOT want hardware-default colours
    // imposed). The hardware LED table in Protocol.cpp is now only a
    // fallback for the rare 2-arg buildUf8GlobalLed call paths that
    // bypass resolveLed_.
    L1[ButtonId::AutoOff]   = mkBuiltin("auto_off",   Behavior::Momentary, "OFF");
    L1[ButtonId::AutoRead]  = mkBuiltin("auto_read",  Behavior::Momentary, "READ");
    L1[ButtonId::AutoWrite] = mkBuiltin("auto_write", Behavior::Momentary, "WRITE");
    L1[ButtonId::AutoTrim]  = mkBuiltin("auto_trim",  Behavior::Momentary, "TRIM");
    L1[ButtonId::AutoLatch] = mkBuiltin("auto_latch", Behavior::Momentary, "LATCH");
    L1[ButtonId::AutoTouch] = mkBuiltin("auto_touch", Behavior::Momentary, "TOUCH");

    // Zoom pad — bundled builtins. Factory colours all white; user
    // chooses per LED.
    L1[ButtonId::ZoomUp]     = mkBuiltin("zoom_up",     Behavior::Momentary, "ZOOM UP");
    L1[ButtonId::ZoomDown]   = mkBuiltin("zoom_down",   Behavior::Momentary, "ZOOM DOWN");
    L1[ButtonId::ZoomLeft]   = mkBuiltin("zoom_left",   Behavior::Momentary, "ZOOM LEFT");
    L1[ButtonId::ZoomRight]  = mkBuiltin("zoom_right",  Behavior::Momentary, "ZOOM RIGHT");
    L1[ButtonId::ZoomCenter] = mkBuiltin("zoom_center", Behavior::Momentary, "FIT");

    // Mode toggles.
    L1[ButtonId::Flip]      = mkBuiltin("flip",                  Behavior::Toggle,    "FLIP");
    L1[ButtonId::PluginBtn] = mkBuiltin("ssl_strip_mode_toggle", Behavior::Toggle,    "PLUGIN");
    L1[ButtonId::Btn360]    = mkBuiltin("mixer_toggle",          Behavior::Momentary, "360");
    L1[ButtonId::Pan]       = mkBuiltin("pan_force",             Behavior::Toggle,    "PAN");

    // Flip long-press routes the focused track's sends/receives onto
    // V-Pots: long alone = sends (LED green when active), long+Shift =
    // receives (LED red). Behavior must be Momentary for long-press to
    // arm; the regular FLIP toggle still works on a quick press.
    {
        auto& fl = L1[ButtonId::Flip];
        fl.behavior     = Behavior::Momentary;
        fl.hasLongPress = true;
        auto& lpPlain = fl.longPress[static_cast<int>(Modifier::Plain)];
        lpPlain.type   = ActionType::Builtin;
        lpPlain.action = "send_this";
        lpPlain.param  = 1;   // Flip → V-Pots (this track's sends spread across V-Pots)
        auto& lpShift = fl.longPress[static_cast<int>(Modifier::Shift)];
        lpShift.type   = ActionType::Builtin;
        lpShift.action = "recv_this";
        lpShift.param  = 1;
    }

    // Send/Plugin row — each button switches to the matching send
    // index. Plain = Send N for all tracks, Shift+ = Receive N. Param
    // 0 routes onto Faders by default; the user can flip onto V-Pots
    // via the per-binding "Flip" checkbox.
    static const ButtonId kSendPluginIds[8] = {
        ButtonId::SendPlugin1, ButtonId::SendPlugin2,
        ButtonId::SendPlugin3, ButtonId::SendPlugin4,
        ButtonId::SendPlugin5, ButtonId::SendPlugin6,
        ButtonId::SendPlugin7, ButtonId::SendPlugin8,
    };
    for (int i = 0; i < 8; ++i) {
        char nameSend[20], nameRecv[20], label[8];
        std::snprintf(nameSend, sizeof(nameSend), "send_all_%d", i + 1);
        std::snprintf(nameRecv, sizeof(nameRecv), "recv_all_%d", i + 1);
        std::snprintf(label,    sizeof(label),    "S/P %d",    i + 1);
        Binding bd;
        bd.behavior = Behavior::Momentary;
        bd.label    = label;
        auto& sp = bd.shortPress[static_cast<int>(Modifier::Plain)];
        sp.type   = ActionType::Builtin;
        sp.action = nameSend;
        sp.param  = 0;   // default: Faders
        auto& shft = bd.shortPress[static_cast<int>(Modifier::Shift)];
        shft.type   = ActionType::Builtin;
        shft.action = nameRecv;
        shft.param  = 0;
        L1[kSendPluginIds[i]] = bd;
    }

    // CHANNEL — defaults to "home": one press clears every routing
    // toggle (send/recv on V-Pots and Faders) so the strips return to
    // their normal track-volume + pan view.
    L1[ButtonId::Channel] = mkBuiltin("home", Behavior::Momentary, "HOME");

    // Top-soft-keys (one per strip, above the V-Pots) — default to the
    // SSL Channel-Strip plug-in's softkey-focus behaviour. param =
    // strip 0..7. Press fires `ssl_softkey` which looks up the
    // current PAGE bank + focused-domain plugin map and calls
    // setFocus on the slot's linkIdx — same as SSL 360°.
    static const ButtonId kTopSoftKeyIds[8] = {
        ButtonId::TopSoftKey1, ButtonId::TopSoftKey2,
        ButtonId::TopSoftKey3, ButtonId::TopSoftKey4,
        ButtonId::TopSoftKey5, ButtonId::TopSoftKey6,
        ButtonId::TopSoftKey7, ButtonId::TopSoftKey8,
    };
    for (int i = 0; i < 8; ++i) {
        char label[12];
        std::snprintf(label, sizeof(label), "Soft-Key %d", i + 1);
        L1[kTopSoftKeyIds[i]] = mkBuiltin("ssl_softkey",
                                          Behavior::Momentary, label,
                                          255, 255, 255, /*param*/ i);
    }

    // SSL soft-key bank selectors. Default: each switches the SSL
    // plug-in's PAGE bank (0 = V-POT, 1..5 = Bank N) — same row that
    // SSL 360°'s PAGE ←/→ navigates between. User can rebind any
    // button to softkey_bank_select with a different param to jump
    // directly to a specific bank.
    static const ButtonId kBankIds[6] = {
        ButtonId::VPotBank,
        ButtonId::SoftKey1Bank, ButtonId::SoftKey2Bank,
        ButtonId::SoftKey3Bank, ButtonId::SoftKey4Bank,
        ButtonId::SoftKey5Bank,
    };
    static const char* kBankLabels[6] = {
        "V-POT", "BANK 1", "BANK 2", "BANK 3", "BANK 4", "BANK 5",
    };
    for (int i = 0; i < 6; ++i) {
        L1[kBankIds[i]] = mkBuiltin("softkey_bank_select",
                                    Behavior::Momentary, kBankLabels[i],
                                    255, 255, 255, /*param*/ i);
    }

    // Quick keys: Q1=CS domain, Q2=BC domain, Q3 reserved (no factory binding —
    // falls through to legacy MCU path, matching today's behaviour).
    L1[ButtonId::Quick1] = mkBuiltin("domain_cs", Behavior::Momentary, "CS");
    L1[ButtonId::Quick2] = mkBuiltin("domain_bc", Behavior::Momentary, "BC");

    // Bank scroll (8-strip) and soft-key bank navigation (page).
    L1[ButtonId::BankLeft]  = mkBuiltin("bank_left",  Behavior::Momentary, "BANK <");
    L1[ButtonId::BankRight] = mkBuiltin("bank_right", Behavior::Momentary, "BANK >");
    L1[ButtonId::PageLeft]  = mkBuiltin("page_left",  Behavior::Momentary, "PAGE <");
    L1[ButtonId::PageRight] = mkBuiltin("page_right", Behavior::Momentary, "PAGE >");

    // Layers 2 + 3 start fully empty per resolved Q3.
}

// ---- JSON serialization ---------------------------------------------------

void appendEscaped(std::ostringstream& os, const std::string& s)
{
    os << '"';
    for (char c : s) {
        switch (c) {
            case '"':  os << "\\\""; break;
            case '\\': os << "\\\\"; break;
            case '\n': os << "\\n";  break;
            case '\r': os << "\\r";  break;
            case '\t': os << "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(c));
                    os << buf;
                } else {
                    os << c;
                }
                break;
        }
    }
    os << '"';
}

// Write one layer's body — name, flags, vpot mode, bindings map. The
// `pad` strings are the indentation prefixes for the wrapping object's
// fields and the bindings map keys, so the same helper formats both
// the full-Config layer-array entry (4 / 8 spaces) and the standalone
// per-layer file (2 / 4 spaces).

const char* modifierKeyName_(int m)
{
    switch (m) {
        case static_cast<int>(Modifier::Plain): return "plain";
        case static_cast<int>(Modifier::Shift): return "shift";
        case static_cast<int>(Modifier::Cmd):   return "cmd";
        case static_cast<int>(Modifier::Ctrl):  return "ctrl";
    }
    return "plain";
}

int modifierFromKey_(const char* s)
{
    if (!s) return -1;
    if (std::strcmp(s, "plain") == 0) return static_cast<int>(Modifier::Plain);
    if (std::strcmp(s, "shift") == 0) return static_cast<int>(Modifier::Shift);
    if (std::strcmp(s, "cmd")   == 0) return static_cast<int>(Modifier::Cmd);
    if (std::strcmp(s, "ctrl")  == 0) return static_cast<int>(Modifier::Ctrl);
    return -1;
}

bool slotIsEmpty_(const ActionSlot& s)
{
    if (s.type != ActionType::Noop || !s.action.empty()) return false;
    for (const auto& st : s.extraSteps) {
        if (st.type != ActionType::Noop || !st.action.empty()) return false;
    }
    return true;
}

// Emit a single step's flat fields inline (no surrounding braces) —
// caller owns the wrapping object. Used by both the legacy single-step
// slot writer and the new step-array writer.
void serializeStepFields_(const ActionStep& s, std::ostringstream& os)
{
    os << "\"type\": ";   appendEscaped(os, actionTypeName(s.type));
    os << ", \"action\": "; appendEscaped(os, s.action);
    os << ", \"param\": " << s.param;
    if (!s.label.empty()) {
        os << ", \"label\": ";
        appendEscaped(os, s.label);
    }
    if (s.type == ActionType::Midi) {
        os << ", \"midi\": {";
        os << "\"device\": ";   appendEscaped(os, s.midiDevice);
        os << ", \"channel\": " << s.midiChannel;
        os << ", \"msg\": "     << s.midiMsgType;
        os << ", \"d1\": "      << s.midiData1;
        os << ", \"d2\": "      << s.midiData2;
        os << "}";
    }
    if (s.wait_ms > 0) {
        os << ", \"wait_ms\": " << s.wait_ms;
    }
}

bool slotHasLedOverride_(const ActionSlot& s)
{
    return s.led.hasActive || s.led.hasInactive;
}

void serializeLedOverride_(const LedOverride& lo, std::ostringstream& os)
{
    os << "{";
    bool first = true;
    if (lo.hasActive) {
        os << "\"color\": ["
           << int(lo.color[0]) << ", "
           << int(lo.color[1]) << ", "
           << int(lo.color[2]) << "]";
        os << ", \"brightness\": ";
        appendEscaped(os, brightnessName(lo.brightness));
        first = false;
    }
    if (lo.hasInactive) {
        if (!first) os << ", ";
        os << "\"inactive_color\": ["
           << int(lo.inactiveColor[0]) << ", "
           << int(lo.inactiveColor[1]) << ", "
           << int(lo.inactiveColor[2]) << "]";
        os << ", \"inactive_brightness\": ";
        appendEscaped(os, brightnessName(lo.inactiveBrightness));
    }
    os << "}";
}

// Emit a slot's body. Single-step slot with no LED override and no
// wait_ms collapses to the legacy flat shape (type/action/param/label/
// midi at the slot level). Anything richer emits {steps:[...], led:{}}.
void serializeSlotFields_(const ActionSlot& s, std::ostringstream& os)
{
    const bool useNew = !s.extraSteps.empty()
                     || s.wait_ms > 0
                     || slotHasLedOverride_(s);
    if (!useNew) {
        serializeStepFields_(static_cast<const ActionStep&>(s), os);
        return;
    }
    os << "\"steps\": [";
    const int n = stepCount(s);
    for (int i = 0; i < n; ++i) {
        if (i) os << ", ";
        os << "{";
        serializeStepFields_(stepAt(s, i), os);
        os << "}";
    }
    os << "]";
    if (slotHasLedOverride_(s)) {
        os << ", \"led\": ";
        serializeLedOverride_(s.led, os);
    }
}

// Emit the {plain, shift, cmd, ctrl} matrix object for one row of the
// short/long matrix. Slots with no action set are omitted to keep the
// JSON compact for the common case (most bindings only use plain).
void serializeMatrixRow_(const ActionSlot (&row)[kModifierCount],
                         std::ostringstream& os)
{
    os << "{";
    bool first = true;
    for (int m = 0; m < kModifierCount; ++m) {
        if (slotIsEmpty_(row[m])) continue;
        if (!first) os << ", ";
        first = false;
        os << "\"" << modifierKeyName_(m) << "\": {";
        serializeSlotFields_(row[m], os);
        os << "}";
    }
    os << "}";
}

// Emit a Binding's body inline (no surrounding braces, no leading
// "name": prefix) — caller owns the wrapping object. Used by both the
// per-layer binding map and the user-bank slot serializers so the
// schema stays in one place.
void serializeBindingBody_(const Binding& bd, std::ostringstream& os)
{
    os << "\"behavior\": "; appendEscaped(os, behaviorName(bd.behavior));
    os << ", \"label\": ";  appendEscaped(os, bd.label);
    os << ", \"color\": ["
       << int(bd.color[0]) << ", "
       << int(bd.color[1]) << ", "
       << int(bd.color[2]) << "]";
    os << ", \"brightness\": ";
    appendEscaped(os, brightnessName(bd.brightness));
    os << ", \"inactive_color\": ["
       << int(bd.inactiveColor[0]) << ", "
       << int(bd.inactiveColor[1]) << ", "
       << int(bd.inactiveColor[2]) << "]";
    os << ", \"inactive_brightness\": ";
    appendEscaped(os, brightnessName(bd.inactiveBrightness));
    if (bd.ledShowWhenEmpty) {
        os << ", \"led_show_when_empty\": true";
    }
    os << ", \"short\": ";
    serializeMatrixRow_(bd.shortPress, os);
    if (bd.hasLongPress) {
        os << ", \"long\": ";
        serializeMatrixRow_(bd.longPress, os);
    }
}

void serializeLayerBody_(const Layer& L, std::ostringstream& os,
                         const char* fieldPad, const char* bindingPad)
{
    os << fieldPad << "\"name\": ";
    appendEscaped(os, L.name);
    os << ",\n";
    os << fieldPad << "\"auto_when_mixer_visible\": "
       << (L.autoWhenMixerVisible ? "true" : "false") << ",\n";
    os << fieldPad << "\"vpot_default_mode\": ";
    appendEscaped(os, L.vpotDefaultMode);
    os << ",\n";
    os << fieldPad << "\"bindings\": {";
    bool first = true;
    for (auto& kv : L.bindings) {
        const char* name = toName(kv.first);
        if (!name || !*name) continue;
        if (!first) os << ",";
        first = false;
        os << "\n" << bindingPad << "\"" << name << "\": {";
        serializeBindingBody_(kv.second, os);
        os << "}";
    }
    if (!first) os << "\n" << fieldPad;
    os << "}\n";
}

void serializeUserBanks_(const Config& c, std::ostringstream& os)
{
    // Skip emission entirely when every bank is at default — keeps
    // pre-existing configs from gaining a noisy 200-line empty array.
    bool anyData = false;
    for (const auto& b : c.userBanks) {
        if (!b.name.empty()) { anyData = true; break; }
        for (const auto& s : b.slots) {
            for (int m = 0; m < kModifierCount && !anyData; ++m) {
                if (!slotIsEmpty_(s.shortPress[m]) ||
                    !slotIsEmpty_(s.longPress[m])) {
                    anyData = true;
                }
            }
            if (anyData) break;
        }
        if (anyData) break;
    }
    if (!anyData) return;

    os << ",\n  \"user_banks\": [";
    bool firstBank = true;
    for (int i = 0; i < kUserBankCount; ++i) {
        const auto& bank = c.userBanks[i];
        if (!firstBank) os << ",";
        firstBank = false;
        os << "\n    {\"index\": " << i;
        os << ", \"name\": "; appendEscaped(os, bank.name);
        os << ", \"slots\": [";
        bool firstSlot = true;
        for (int s = 0; s < kUserBankSlots; ++s) {
            if (!firstSlot) os << ",";
            firstSlot = false;
            os << "\n      {";
            serializeBindingBody_(bank.slots[s], os);
            os << "}";
        }
        os << "\n    ]}";
    }
    os << "\n  ]";
}

std::string serialize(const Config& c)
{
    std::ostringstream os;
    os << "{\n";
    os << "  \"version\": " << c.version << ",\n";
    os << "  \"active_layer\": " << c.activeLayer << ",\n";
    os << "  \"layers\": [\n";
    for (int i = 0; i < 3; ++i) {
        os << "    {\n";
        serializeLayerBody_(c.layers[i], os, "      ", "        ");
        os << "    }" << (i < 2 ? "," : "") << "\n";
    }
    os << "  ]";
    serializeUserBanks_(c, os);
    os << "\n}\n";
    return os.str();
}

// Standalone per-layer envelope. The "type" / "index" fields let
// importLayerFrom verify the file before applying it (and let users
// recognise the file at a glance).
std::string serializeOneLayer_(const Layer& L, int idx)
{
    std::ostringstream os;
    os << "{\n";
    os << "  \"version\": 1,\n";
    os << "  \"type\": \"layer\",\n";
    os << "  \"index\": " << idx << ",\n";
    os << "  \"layer\": {\n";
    serializeLayerBody_(L, os, "    ", "      ");
    os << "  }\n";
    os << "}\n";
    return os.str();
}

// Read a single ActionStep's fields from a JSON object.
bool parseStepFields_(wdl_json_element* obj, ActionStep& out)
{
    if (!obj || !obj->is_object()) return false;
    if (auto* v = obj->get_item_by_name("type"))
        out.type = actionTypeFromName(v->get_string_value());
    if (auto* v = obj->get_item_by_name("action"))
        if (auto* s = v->get_string_value()) out.action = s;
    if (auto* v = obj->get_item_by_name("param"))
        if (auto* s = v->get_string_value(true)) out.param = std::atoi(s);
    if (auto* v = obj->get_item_by_name("label"))
        if (auto* s = v->get_string_value()) out.label = s;
    if (auto* v = obj->get_item_by_name("wait_ms"))
        if (auto* s = v->get_string_value(true)) out.wait_ms = std::atoi(s);
    if (auto* mi = obj->get_item_by_name("midi"); mi && mi->is_object()) {
        if (auto* v = mi->get_item_by_name("device"))
            if (auto* s = v->get_string_value()) out.midiDevice = s;
        if (auto* v = mi->get_item_by_name("channel"))
            if (auto* s = v->get_string_value(true)) out.midiChannel = std::atoi(s);
        if (auto* v = mi->get_item_by_name("msg"))
            if (auto* s = v->get_string_value(true)) out.midiMsgType = std::atoi(s);
        if (auto* v = mi->get_item_by_name("d1"))
            if (auto* s = v->get_string_value(true)) out.midiData1 = std::atoi(s);
        if (auto* v = mi->get_item_by_name("d2"))
            if (auto* s = v->get_string_value(true)) out.midiData2 = std::atoi(s);
    }
    return true;
}

void parseLedOverride_(wdl_json_element* obj, LedOverride& out)
{
    if (!obj || !obj->is_object()) return;
    if (auto* v = obj->get_item_by_name("color"); v && v->is_array()) {
        for (int k = 0; k < 3 && k < v->m_array->GetSize(); ++k) {
            if (auto* s = v->enum_item(k)->get_string_value(true)) {
                int x = std::atoi(s);
                if (x < 0) x = 0; else if (x > 255) x = 255;
                out.color[k] = static_cast<uint8_t>(x);
            }
        }
        out.hasActive = true;
    }
    if (auto* v = obj->get_item_by_name("brightness")) {
        out.brightness = brightnessFromName(v->get_string_value());
        out.hasActive = true;
    }
    if (auto* v = obj->get_item_by_name("inactive_color"); v && v->is_array()) {
        for (int k = 0; k < 3 && k < v->m_array->GetSize(); ++k) {
            if (auto* s = v->enum_item(k)->get_string_value(true)) {
                int x = std::atoi(s);
                if (x < 0) x = 0; else if (x > 255) x = 255;
                out.inactiveColor[k] = static_cast<uint8_t>(x);
            }
        }
        out.hasInactive = true;
    }
    if (auto* v = obj->get_item_by_name("inactive_brightness")) {
        out.inactiveBrightness = brightnessFromName(v->get_string_value());
        out.hasInactive = true;
    }
}

// Read a single ActionSlot's fields. Accepts both legacy single-action
// shape (type/action/param/label/midi at the slot level) and the new
// {steps:[...], led:{}} shape. Missing keys leave defaults intact.
bool parseSlotFields_(wdl_json_element* obj, ActionSlot& out)
{
    if (!obj || !obj->is_object()) return false;
    if (auto* steps = obj->get_item_by_name("steps");
        steps && steps->is_array() && steps->m_array) {
        const int n = steps->m_array->GetSize();
        for (int i = 0; i < n; ++i) {
            if (i == 0) {
                parseStepFields_(steps->enum_item(0),
                                 static_cast<ActionStep&>(out));
            } else {
                ActionStep st;
                parseStepFields_(steps->enum_item(i), st);
                out.extraSteps.push_back(std::move(st));
            }
        }
    } else {
        parseStepFields_(obj, static_cast<ActionStep&>(out));
    }
    if (auto* led = obj->get_item_by_name("led"); led && led->is_object()) {
        parseLedOverride_(led, out.led);
    }
    return true;
}

// Read a {plain, shift, cmd, ctrl} matrix-row object into the 4-element
// slot array. Missing modifier keys leave their slot at default (Noop).
void parseMatrixRow_(wdl_json_element* obj, ActionSlot (&row)[kModifierCount])
{
    if (!obj || !obj->is_object()) return;
    const int n = obj->m_array ? obj->m_array->GetSize() : 0;
    for (int i = 0; i < n; ++i) {
        const char* key = obj->enum_item_name(i);
        wdl_json_element* it = obj->enum_item(i);
        const int m = modifierFromKey_(key);
        if (m < 0) continue;
        parseSlotFields_(it, row[m]);
    }
}

// Parse a Binding from its JSON object (new-schema only — no
// type/action/param/midi/long_press fallback). Used by parseUserBanks_.
// parseLayer_ has its own inline logic that also covers the old
// pre-matrix schema, so it doesn't go through this helper.
void parseBindingBody_(wdl_json_element* be, Binding& bd)
{
    if (!be || !be->is_object()) return;
    if (auto* v = be->get_item_by_name("behavior"))
        bd.behavior = behaviorFromName(v->get_string_value());
    if (auto* v = be->get_item_by_name("label"))
        if (auto* s = v->get_string_value()) bd.label = s;
    if (auto* v = be->get_item_by_name("color"); v && v->is_array()) {
        for (int k = 0; k < 3 && k < v->m_array->GetSize(); ++k) {
            if (auto* s = v->enum_item(k)->get_string_value(true)) {
                int x = std::atoi(s);
                if (x < 0) x = 0; else if (x > 255) x = 255;
                bd.color[k] = static_cast<uint8_t>(x);
            }
        }
    }
    if (auto* v = be->get_item_by_name("brightness"))
        bd.brightness = brightnessFromName(v->get_string_value());
    if (auto* v = be->get_item_by_name("inactive_color"); v && v->is_array()) {
        for (int k = 0; k < 3 && k < v->m_array->GetSize(); ++k) {
            if (auto* s = v->enum_item(k)->get_string_value(true)) {
                int x = std::atoi(s);
                if (x < 0) x = 0; else if (x > 255) x = 255;
                bd.inactiveColor[k] = static_cast<uint8_t>(x);
            }
        }
    } else {
        bd.inactiveColor[0] = bd.color[0];
        bd.inactiveColor[1] = bd.color[1];
        bd.inactiveColor[2] = bd.color[2];
    }
    if (auto* v = be->get_item_by_name("inactive_brightness"))
        bd.inactiveBrightness = brightnessFromName(v->get_string_value());
    if (auto* v = be->get_item_by_name("led_show_when_empty"))
        if (auto* s = v->get_string_value(true))
            bd.ledShowWhenEmpty = (std::strcmp(s, "true") == 0
                                || std::strcmp(s, "1") == 0);
    if (auto* v = be->get_item_by_name("short"))
        parseMatrixRow_(v, bd.shortPress);
    if (auto* v = be->get_item_by_name("long"); v && v->is_object()) {
        bd.hasLongPress = true;
        parseMatrixRow_(v, bd.longPress);
    }
}

void parseUserBanks_(wdl_json_element* root, Config& out)
{
    auto* arr = root->get_item_by_name("user_banks");
    if (!arr || !arr->is_array() || !arr->m_array) return;
    const int n = arr->m_array->GetSize();
    for (int i = 0; i < n; ++i) {
        wdl_json_element* bo = arr->enum_item(i);
        if (!bo || !bo->is_object()) continue;
        int idx = -1;
        if (auto* v = bo->get_item_by_name("index"))
            if (auto* s = v->get_string_value(true)) idx = std::atoi(s);
        if (idx < 0 || idx >= kUserBankCount) continue;
        UserBank& bank = out.userBanks[idx];
        if (auto* v = bo->get_item_by_name("name"))
            if (auto* s = v->get_string_value()) bank.name = s;
        auto* slots = bo->get_item_by_name("slots");
        if (!slots || !slots->is_array() || !slots->m_array) continue;
        const int m = slots->m_array->GetSize();
        for (int s = 0; s < m && s < kUserBankSlots; ++s) {
            wdl_json_element* slotObj = slots->enum_item(s);
            // Reset to default first so omitted JSON fields fall back
            // to defaults (matches the parseLayer_ semantics).
            bank.slots[s] = Binding{};
            parseBindingBody_(slotObj, bank.slots[s]);
        }
    }
}

bool parseLayer_(wdl_json_element* lobj, Layer& out)
{
    if (!lobj || !lobj->is_object()) return false;
    if (auto* v = lobj->get_item_by_name("name"))
        if (auto* s = v->get_string_value()) out.name = s;
    if (auto* v = lobj->get_item_by_name("auto_when_mixer_visible"))
        if (auto* s = v->get_string_value(true))
            out.autoWhenMixerVisible = (std::strcmp(s, "true") == 0 || std::strcmp(s, "1") == 0);
    if (auto* v = lobj->get_item_by_name("vpot_default_mode"))
        if (auto* s = v->get_string_value()) out.vpotDefaultMode = s;
    auto* bobj = lobj->get_item_by_name("bindings");
    if (!bobj || !bobj->is_object()) return true;
    const int n = bobj->m_array ? bobj->m_array->GetSize() : 0;
    for (int i = 0; i < n; ++i) {
        const char* key = bobj->enum_item_name(i);
        wdl_json_element* be = bobj->enum_item(i);
        if (!key || !be || !be->is_object()) continue;
        ButtonId bid = fromName(key);
        if (bid == ButtonId::None) continue;  // forward-compat: skip unknown keys
        Binding bd;

        if (auto* v = be->get_item_by_name("behavior"))
            bd.behavior = behaviorFromName(v->get_string_value());
        if (auto* v = be->get_item_by_name("label"))
            if (auto* s = v->get_string_value()) bd.label = s;
        if (auto* v = be->get_item_by_name("color"); v && v->is_array()) {
            for (int k = 0; k < 3 && k < v->m_array->GetSize(); ++k) {
                if (auto* s = v->enum_item(k)->get_string_value(true)) {
                    int x = std::atoi(s);
                    if (x < 0) x = 0; else if (x > 255) x = 255;
                    bd.color[k] = static_cast<uint8_t>(x);
                }
            }
        }
        if (auto* v = be->get_item_by_name("brightness"))
            bd.brightness = brightnessFromName(v->get_string_value());
        if (auto* v = be->get_item_by_name("inactive_color"); v && v->is_array()) {
            for (int k = 0; k < 3 && k < v->m_array->GetSize(); ++k) {
                if (auto* s = v->enum_item(k)->get_string_value(true)) {
                    int x = std::atoi(s);
                    if (x < 0) x = 0; else if (x > 255) x = 255;
                    bd.inactiveColor[k] = static_cast<uint8_t>(x);
                }
            }
        } else {
            // Pre-split configs only carried `color`. Mirror it into
            // inactiveColor so quantising into the same palette entry
            // keeps the old visual identity.
            bd.inactiveColor[0] = bd.color[0];
            bd.inactiveColor[1] = bd.color[1];
            bd.inactiveColor[2] = bd.color[2];
        }
        if (auto* v = be->get_item_by_name("inactive_brightness"))
            bd.inactiveBrightness = brightnessFromName(v->get_string_value());
        if (auto* v = be->get_item_by_name("led_show_when_empty"))
            if (auto* s = v->get_string_value(true))
                bd.ledShowWhenEmpty = (std::strcmp(s, "true") == 0
                                    || std::strcmp(s, "1") == 0);

        // New-schema matrix. Both `short` and `long` are optional —
        // missing slots stay at default (Noop).
        if (auto* v = be->get_item_by_name("short"))
            parseMatrixRow_(v, bd.shortPress);
        if (auto* v = be->get_item_by_name("long"); v && v->is_object()) {
            bd.hasLongPress = true;
            parseMatrixRow_(v, bd.longPress);
        }

        // Old-schema fallback: pre-modifier-matrix configs carried bare
        // `type`/`action`/`param`/`midi` + `long_press` at the binding
        // level. Both the binding object and the `long_press` object
        // happen to use the same {type,action,param,midi:{}} shape that
        // parseSlotFields_ already understands — re-use it. Skipped if
        // the new matrix already populated the corresponding plain slot.
        ActionSlot& sp = bd.shortPress[static_cast<int>(Modifier::Plain)];
        if (slotIsEmpty_(sp)) parseSlotFields_(be, sp);
        if (auto* lp = be->get_item_by_name("long_press"); lp && lp->is_object()
            && slotIsEmpty_(bd.longPress[static_cast<int>(Modifier::Plain)])) {
            bd.hasLongPress = true;
            parseSlotFields_(lp, bd.longPress[static_cast<int>(Modifier::Plain)]);
        }

        // Migration: rename the legacy `fine_modifier` builtin to the
        // generic `mod_shift` so it slots into the new modifier framework.
        if (sp.type == ActionType::Builtin && sp.action == "fine_modifier") {
            sp.action = "mod_shift";
        }
        // Migration: send/receive routing builtins were originally split
        // by physical output (`send_all_3_vpot`, `send_all_3_fader`,
        // `send_this_vpot`, `send_this_fader`, plus recv_* twins). They
        // collapsed to a single name + a "Flip" param (0 = Faders,
        // 1 = V-Pots) — strip the suffix and set the param accordingly.
        if (sp.type == ActionType::Builtin) {
            auto endsWith = [](const std::string& s, const char* suffix) {
                const size_t n = std::strlen(suffix);
                return s.size() >= n
                    && std::strncmp(s.c_str() + s.size() - n, suffix, n) == 0;
            };
            if ((sp.action.rfind("send_all_", 0) == 0
              || sp.action.rfind("recv_all_", 0) == 0
              || sp.action == "send_this_vpot" || sp.action == "send_this_fader"
              || sp.action == "recv_this_vpot" || sp.action == "recv_this_fader")) {
                if (endsWith(sp.action, "_vpot")) {
                    sp.action.resize(sp.action.size() - 5);
                    sp.param = 1;   // Flip → V-Pots
                } else if (endsWith(sp.action, "_fader")) {
                    sp.action.resize(sp.action.size() - 6);
                    sp.param = 0;   // Default → Faders
                }
            }
        }

        out.bindings[bid] = std::move(bd);
    }
    return true;
}

// v5 → v6: convert "type=Builtin, action=empty" entries to Noop so
// they stop silently no-op'ing on press and are visible-to-fix in
// the Settings editor. This ALSO walks every modifier slot and the
// long-press matrix.
void upgradeEmptyBuiltinSlots_(Layer& L)
{
    auto fix = [](ActionStep& sp) {
        if (sp.type == ActionType::Builtin && sp.action.empty()) {
            sp.type = ActionType::Noop;
        }
    };
    for (auto& kv : L.bindings) {
        Binding& bd = kv.second;
        for (int m = 0; m < kModifierCount; ++m) {
            fix(bd.shortPress[m]);
            for (auto& step : bd.shortPress[m].extraSteps) fix(step);
            fix(bd.longPress[m]);
            for (auto& step : bd.longPress[m].extraSteps) fix(step);
        }
    }
}

// v4 → v5 reset: wipe ALL Auto-row + Zoom-pad colours to white.
// Frank 2026-05-07: factory hardware-default colours are not wanted —
// every LED is user-chosen via Settings → Bindings. Configs created
// before this rule had auto_*/zoom_* coloured by seedFactoryDefaults_
// and/or the old in-parseLayer migration. This one-shot upgrade
// resets them to white so the editor presents a blank canvas.
// Buttons whose binding the user has explicitly recoloured to
// something OTHER than the previous factory value are left alone
// (the upgrade only touches bindings whose colour exactly matches
// the historical hardcoded swatch).
void upgradeStripFactoryColours_(Layer& L)
{
    struct Reset { const char* action; uint8_t r, g, b; };
    static constexpr Reset kResets[] = {
        {"auto_read",    0,   255,   0},
        {"auto_write",   255, 0,     0},
        {"auto_trim",    255, 128,   0},
        {"auto_latch",   255, 0,     0},
        {"auto_touch",   255, 255,   0},
        {"zoom_up",      0,   255,   0},
        {"zoom_down",    255, 255,   0},
        {"zoom_center",  255, 0,     0},
    };
    for (auto& kv : L.bindings) {
        Binding& bd = kv.second;
        ActionStep& sp = bd.shortPress[
            static_cast<int>(Modifier::Plain)];
        if (sp.type != ActionType::Builtin) continue;
        for (const auto& rs : kResets) {
            if (sp.action != rs.action) continue;
            const bool matchesOld =
                (bd.color[0] == rs.r && bd.color[1] == rs.g && bd.color[2] == rs.b);
            if (matchesOld) {
                bd.color[0] = 0xFF; bd.color[1] = 0xFF; bd.color[2] = 0xFF;
                bd.inactiveColor[0] = 0xFF;
                bd.inactiveColor[1] = 0xFF;
                bd.inactiveColor[2] = 0xFF;
            }
            break;
        }
    }
}

// ---- Path helpers ---------------------------------------------------------

std::string configDir_()
{
    const char* base = GetResourcePath ? GetResourcePath() : nullptr;
    if (!base || !*base) base = ".";
    std::string d = base;
    d += "/rea_sixty";
    return d;
}

std::string configPath_()
{
    return configDir_() + "/bindings.json";
}

void ensureConfigDir_()
{
    const std::string d = configDir_();
    struct stat st{};
    if (stat(d.c_str(), &st) == 0) return;
#ifdef _WIN32
    _mkdir(d.c_str());
#else
    mkdir(d.c_str(), 0755);
#endif
}

bool readFile_(const std::string& path, std::string& out)
{
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (n < 0) { std::fclose(f); return false; }
    out.resize(static_cast<size_t>(n));
    if (n > 0) std::fread(out.data(), 1, static_cast<size_t>(n), f);
    std::fclose(f);
    return true;
}

bool writeFile_(const std::string& path, const std::string& contents)
{
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    std::fwrite(contents.data(), 1, contents.size(), f);
    std::fclose(f);
    return true;
}

bool tryParse_(const std::string& json, Config& out)
{
    wdl_json_parser p;
    wdl_json_element* root = p.parse(json.c_str(), static_cast<int>(json.size()));
    if (!root || !root->is_object()) return false;

    if (auto* v = root->get_item_by_name("version"))
        if (auto* s = v->get_string_value(true)) out.version = std::atoi(s);
    if (auto* v = root->get_item_by_name("active_layer"))
        if (auto* s = v->get_string_value(true)) out.activeLayer = std::atoi(s);
    if (out.activeLayer < 0 || out.activeLayer > 2) out.activeLayer = 0;

    if (auto* arr = root->get_item_by_name("layers"); arr && arr->is_array()) {
        const int n = arr->m_array ? arr->m_array->GetSize() : 0;
        for (int i = 0; i < n && i < 3; ++i) {
            parseLayer_(arr->enum_item(i), out.layers[i]);
        }
    }
    parseUserBanks_(root, out);
    return true;
}

} // namespace

void registerBuiltin(const char* name, BuiltinDescriptor desc)
{
    if (!name || !*name) return;
    g_builtins[name] = std::move(desc);
}

// Bumped each time we ship a default-binding change that needs to
// reach existing configs. load() runs every defined upgrade step in
// order, then writes the bumped version back so the upgrade is
// idempotent across REAPER restarts.
// v6 (2026-05-07): clean up corrupt "type=Builtin, action=empty"
// entries left behind by the Settings UI's combo-picker race (user
// clicks Built-in radio, picks no name in the combo, dirty flag
// triggers setBinding with an unsalvageable entry that silently
// no-ops on press). Convert those entries to Noop so they're at
// least UI-fixable (currently they sit corrupt forever).
// v5 (2026-05-07): zoom + auto factory colours abolished — every LED
// is user-chosen. v4→v5 upgrade resets the historical hardcoded
// swatches (auto_read/green, zoom_up/green, …) back to white.
// v4 (2026-05-07): unused — bumped only to gate the colour-migration
// fix that landed mid-day; superseded by v5 the same day.
constexpr int kCurrentBindingsVersion = 6;

// Upgrade hook: existing configs get factory long-press defaults on
// the FLIP button (send_this / recv_this+Shift) without touching any
// other field. Skipped if the user has already set their own
// long-press for FLIP — explicit assignments always win.
void upgradeFlipLongPress_(Layer& L)
{
    auto it = L.bindings.find(ButtonId::Flip);
    if (it == L.bindings.end()) return;
    Binding& bd = it->second;
    if (bd.hasLongPress) return;
    bd.behavior     = Behavior::Momentary;
    bd.hasLongPress = true;
    auto& lpPlain = bd.longPress[static_cast<int>(Modifier::Plain)];
    lpPlain.type   = ActionType::Builtin;
    lpPlain.action = "send_this";
    lpPlain.param  = 1;
    auto& lpShift = bd.longPress[static_cast<int>(Modifier::Shift)];
    lpShift.type   = ActionType::Builtin;
    lpShift.action = "recv_this";
    lpShift.param  = 1;
}

// v3 upgrade: a previous editor version auto-filled per-binding
// labels for ssl_softkey from the V-POT bank's slot names. ssl_softkey
// is bank-aware though — that label then mis-displayed on every other
// PAGE bank. Clear those auto-filled labels so the runtime falls
// back to the live SSL softkey label per current bank. Only sweeps
// shortPress[Plain] since that's the only slot the auto-fill could
// have touched.
void upgradeSslSoftkeyLabels_(Layer& L)
{
    for (auto& kv : L.bindings) {
        Binding& bd = kv.second;
        auto& sp = bd.shortPress[static_cast<int>(Modifier::Plain)];
        if (sp.type == ActionType::Builtin && sp.action == "ssl_softkey") {
            sp.label.clear();
        }
    }
}

void load()
{
    std::lock_guard<std::mutex> lk(g_cfgMutex);

    std::string contents;
    if (readFile_(configPath_(), contents) && !contents.empty()) {
        Config tmp;
        seedFactoryDefaults_(tmp);     // start from factories so missing fields fall back
        if (tryParse_(contents, tmp)) {
            // One-shot upgrades for configs from older versions. Each
            // step is idempotent (re-running a completed step is a
            // no-op) so the conditional guard is mostly a perf hint.
            if (tmp.version < 2) {
                for (auto& L : tmp.layers) upgradeFlipLongPress_(L);
            }
            if (tmp.version < 3) {
                for (auto& L : tmp.layers) upgradeSslSoftkeyLabels_(L);
            }
            if (tmp.version < 5) {
                for (auto& L : tmp.layers) upgradeStripFactoryColours_(L);
            }
            if (tmp.version < 6) {
                for (auto& L : tmp.layers) upgradeEmptyBuiltinSlots_(L);
            }
            tmp.version = kCurrentBindingsVersion;
            g_cfg = std::move(tmp);
            // Persist the upgraded config so the next load doesn't
            // re-walk the upgrade chain.
            ensureConfigDir_();
            writeFile_(configPath_(), serialize(g_cfg));
            g_bindingsGen.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }

    // First run, missing file, or parse error: seed factories + persist.
    seedFactoryDefaults_(g_cfg);
    ensureConfigDir_();
    writeFile_(configPath_(), serialize(g_cfg));
    g_bindingsGen.fetch_add(1, std::memory_order_relaxed);
}

void save()
{
    std::lock_guard<std::mutex> lk(g_cfgMutex);
    ensureConfigDir_();
    writeFile_(configPath_(), serialize(g_cfg));
}

bool exportTo(const std::string& path)
{
    std::lock_guard<std::mutex> lk(g_cfgMutex);
    return writeFile_(path, serialize(g_cfg));
}

bool importFrom(const std::string& path)
{
    std::string contents;
    if (!readFile_(path, contents) || contents.empty()) return false;

    Config tmp;
    seedFactoryDefaults_(tmp);
    if (!tryParse_(contents, tmp)) return false;

    {
        std::lock_guard<std::mutex> lk(g_cfgMutex);
        g_cfg = std::move(tmp);
        ensureConfigDir_();
        writeFile_(configPath_(), serialize(g_cfg));
        g_bindingsGen.fetch_add(1, std::memory_order_relaxed);
    }
    return true;
}

bool exportLayerTo(int layer, const std::string& path)
{
    if (layer < 0 || layer > 2) return false;
    std::lock_guard<std::mutex> lk(g_cfgMutex);
    return writeFile_(path, serializeOneLayer_(g_cfg.layers[layer], layer));
}

bool importLayerFrom(int layer, const std::string& path)
{
    if (layer < 0 || layer > 2) return false;
    std::string contents;
    if (!readFile_(path, contents) || contents.empty()) return false;

    wdl_json_parser p;
    wdl_json_element* root = p.parse(contents.c_str(),
                                     static_cast<int>(contents.size()));
    if (!root || !root->is_object()) return false;

    // Accept both the wrapped {"type":"layer", ...} form and a bare layer
    // object (no "type" field) for forward-compat with hand-edited files.
    wdl_json_element* layerObj = nullptr;
    if (auto* t = root->get_item_by_name("type"); t) {
        const char* ts = t->get_string_value();
        if (!ts || std::strcmp(ts, "layer") != 0) return false;
        layerObj = root->get_item_by_name("layer");
    } else if (root->get_item_by_name("name")) {
        layerObj = root;  // bare layer
    }
    if (!layerObj || !layerObj->is_object()) return false;

    Layer tmp;
    if (!parseLayer_(layerObj, tmp)) return false;

    {
        std::lock_guard<std::mutex> lk(g_cfgMutex);
        g_cfg.layers[layer] = std::move(tmp);
        ensureConfigDir_();
        writeFile_(configPath_(), serialize(g_cfg));
        g_bindingsGen.fetch_add(1, std::memory_order_relaxed);
    }
    return true;
}

const Config& get()
{
    return g_cfg;
}

namespace {

void runStep_(const ActionStep& a, bool firing, bool pressed)
{
    switch (a.type) {
        case ActionType::Noop:
            break;
        case ActionType::Reaper: {
            if (!firing) break;
            // Named commands (ReaScripts, custom actions) are stored as
            // "_RS<hash>" / "_<name>" — atoi would yield 0. Resolve via
            // NamedCommandLookup so script bindings dispatch correctly.
            int actionId = 0;
            if (!a.action.empty() && a.action[0] == '_') {
                actionId = NamedCommandLookup(a.action.c_str());
            } else {
                actionId = std::atoi(a.action.c_str());
            }
            if (actionId <= 0) break;
            auto it = g_builtins.find("__reaper_action__");
            if (it != g_builtins.end() && it->second.run) {
                it->second.run(true, pressed, actionId);
            }
            break;
        }
        case ActionType::Keyboard:
            // Phase D — needs platform key-event injection.
            break;
        case ActionType::Builtin: {
            auto it = g_builtins.find(a.action);
            if (it != g_builtins.end() && it->second.run) {
                it->second.run(firing, pressed, a.param);
            }
            break;
        }
        case ActionType::Midi: {
            if (!firing) break;
            // Phase D wiring: real MIDI out via REAPER's API. For now log
            // the intent so the user can see their binding is reaching
            // dispatch. File-log instead of ShowConsoleMsg to stay quiet
            // on rapid presses.
            if (FILE* f = std::fopen("/tmp/rea_sixty_midi.log", "a")) {
                std::fprintf(f, "midi: dev='%s' ch=%d msg=%d d1=%d d2=%d\n",
                             a.midiDevice.c_str(), a.midiChannel,
                             a.midiMsgType, a.midiData1, a.midiData2);
                std::fclose(f);
            }
            break;
        }
    }
}

// Pending multi-step chain. Held in g_pendingChains until each step's
// `fireAt` elapses on the main-thread timer drain. Single-step chains
// short-circuit in runSlot_ and never sit on the queue.
struct PendingChain {
    ActionSlot                            snapshot;
    int                                   nextStepIdx;
    bool                                  firing;
    bool                                  pressed;
    std::chrono::steady_clock::time_point fireAt;
};

std::mutex                 g_pendingMutex;
std::vector<PendingChain>  g_pendingChains;

// Run a slot's chain. Single-step slots fire synchronously (preserving
// the legacy zero-latency path); multi-step chains run step 0 inline
// and queue the rest for tickPending_ to drain on the main thread.
void runSlot_(const ActionSlot& slot, bool firing, bool pressed)
{
    const int n = stepCount(slot);
    if (n <= 1) {
        runStep_(static_cast<const ActionStep&>(slot), firing, pressed);
        return;
    }
    runStep_(stepAt(slot, 0), firing, pressed);
    if (!firing) return;
    const int wait0 = slot.wait_ms < 0 ? 0 : slot.wait_ms;
    PendingChain pc;
    pc.snapshot    = slot;
    pc.nextStepIdx = 1;
    pc.firing      = firing;
    pc.pressed     = pressed;
    pc.fireAt      = std::chrono::steady_clock::now()
                   + std::chrono::milliseconds(wait0);
    std::lock_guard<std::mutex> lk(g_pendingMutex);
    g_pendingChains.push_back(std::move(pc));
}

} // namespace

void tickPending()
{
    const auto now = std::chrono::steady_clock::now();
    std::vector<PendingChain> ready;
    {
        std::lock_guard<std::mutex> lk(g_pendingMutex);
        for (auto it = g_pendingChains.begin(); it != g_pendingChains.end(); ) {
            if (it->fireAt <= now) {
                ready.push_back(std::move(*it));
                it = g_pendingChains.erase(it);
            } else {
                ++it;
            }
        }
    }
    for (auto& pc : ready) {
        const int n = stepCount(pc.snapshot);
        if (pc.nextStepIdx < 0 || pc.nextStepIdx >= n) continue;
        const ActionStep& st = stepAt(pc.snapshot, pc.nextStepIdx);
        runStep_(st, pc.firing, pc.pressed);
        const int next = pc.nextStepIdx + 1;
        if (next < n) {
            const int wait = st.wait_ms < 0 ? 0 : st.wait_ms;
            pc.nextStepIdx = next;
            pc.fireAt      = std::chrono::steady_clock::now()
                           + std::chrono::milliseconds(wait);
            std::lock_guard<std::mutex> lk(g_pendingMutex);
            g_pendingChains.push_back(std::move(pc));
        }
    }
}

void effectiveLedActive(const Binding& bd, const ActionSlot& slot,
                        uint8_t (&rgb)[3], Brightness& bri)
{
    if (slot.led.hasActive) {
        rgb[0] = slot.led.color[0];
        rgb[1] = slot.led.color[1];
        rgb[2] = slot.led.color[2];
        bri    = slot.led.brightness;
    } else {
        rgb[0] = bd.color[0];
        rgb[1] = bd.color[1];
        rgb[2] = bd.color[2];
        bri    = bd.brightness;
    }
}

void effectiveLedInactive(const Binding& bd, const ActionSlot& slot,
                          uint8_t (&rgb)[3], Brightness& bri)
{
    if (slot.led.hasInactive) {
        rgb[0] = slot.led.inactiveColor[0];
        rgb[1] = slot.led.inactiveColor[1];
        rgb[2] = slot.led.inactiveColor[2];
        bri    = slot.led.inactiveBrightness;
    } else {
        rgb[0] = bd.inactiveColor[0];
        rgb[1] = bd.inactiveColor[1];
        rgb[2] = bd.inactiveColor[2];
        bri    = bd.inactiveBrightness;
    }
}

void setModifierHeld(Modifier m, bool held)
{
    switch (m) {
        case Modifier::Shift: g_modShiftHeld.store(held); break;
        case Modifier::Cmd:   g_modCmdHeld.store(held);   break;
        case Modifier::Ctrl:  g_modCtrlHeld.store(held);  break;
        case Modifier::Plain: break;  // no state to set
    }
}

bool modifierHeld(Modifier m)
{
    switch (m) {
        case Modifier::Shift: return g_modShiftHeld.load();
        case Modifier::Cmd:   return g_modCmdHeld.load();
        case Modifier::Ctrl:  return g_modCtrlHeld.load();
        case Modifier::Plain: return false;
    }
    return false;
}

Modifier currentModifierSnapshot()
{
    // Precedence Ctrl > Cmd > Shift > Plain. Most-specific-modifier-wins
    // matches typical keyboard-shortcut conventions; the editor will let
    // the user route Ctrl+Shift+button via the Ctrl slot only.
    if (g_modCtrlHeld.load())  return Modifier::Ctrl;
    if (g_modCmdHeld.load())   return Modifier::Cmd;
    if (g_modShiftHeld.load()) return Modifier::Shift;
    return Modifier::Plain;
}

bool dispatch(ButtonId id, bool pressed)
{
    if (id == ButtonId::None) return false;

    Binding bd;
    int layer;
    {
        std::lock_guard<std::mutex> lk(g_cfgMutex);
        layer = g_cfg.activeLayer;
        if (layer < 0 || layer > 2) layer = 0;
        auto it = g_cfg.layers[layer].bindings.find(id);
        if (it == g_cfg.layers[layer].bindings.end()) return false;
        bd = it->second;   // copy under lock so the rest runs lock-free
    }

    // Long-press support (Momentary primary only). Defer the primary-
    // action fire until release-edge so we can choose between primary
    // and long-press based on the held duration. Modifier snapshot is
    // taken at PRESS time and re-used on release / threshold so the
    // press commits to a slot even if the user releases the modifier
    // mid-hold.
    const bool longPressArmed =
        bd.hasLongPress && bd.behavior == Behavior::Momentary;
    if (longPressArmed) {
        const uint32_t k = pressKey(layer, id);
        if (pressed) {
            g_pressStart[k] = { std::chrono::steady_clock::now(),
                                currentModifierSnapshot() };
        } else {
            auto it = g_pressStart.find(k);
            if (it != g_pressStart.end()) {
                const auto held = std::chrono::steady_clock::now() - it->second.start;
                const int  m    = static_cast<int>(it->second.mod);
                g_pressStart.erase(it);
                if (held >= kLongPressThreshold) {
                    runSlot_(bd.longPress[m],
                               /*firing*/ true, /*pressed*/ false);
                } else {
                    runSlot_(bd.shortPress[m],
                               /*firing*/ true, /*pressed*/ false);
                }
            }
        }
        return true;
    }

    // Standard (no long-press) path — fire per behavior. Modifier slot
    // is selected at the press edge and re-used for the release edge so
    // a binding's release matches its press even if the user dropped
    // the modifier between. All three behaviours honour the modifier
    // now (Toggle and Hold used to fall back to Plain — Frank
    // 2026-05-06: Shift+Press should fire the Shift slot regardless
    // of the binding's behaviour).
    int slotIdx = static_cast<int>(Modifier::Plain);
    if (bd.behavior == Behavior::Momentary || bd.behavior == Behavior::Hold) {
        const uint32_t k = pressKey(layer, id);
        if (pressed) {
            g_pressStart[k] = { std::chrono::steady_clock::now(),
                                currentModifierSnapshot() };
            slotIdx = static_cast<int>(g_pressStart[k].mod);
        } else {
            auto it = g_pressStart.find(k);
            if (it != g_pressStart.end()) {
                slotIdx = static_cast<int>(it->second.mod);
                g_pressStart.erase(it);
            }
        }
    } else {  // Behavior::Toggle — fires only on press-edge, uses live mod.
        if (pressed) slotIdx = static_cast<int>(currentModifierSnapshot());
    }
    bool firing;
    switch (bd.behavior) {
        case Behavior::Momentary: firing = pressed; break;
        case Behavior::Toggle:    firing = pressed; break;
        case Behavior::Hold:      firing = true;    break;
    }
    const auto& slot = bd.shortPress[slotIdx];
    if (firing && slot.type != ActionType::Noop) {
        // Remember which modifier slot this button last actually fired
        // — main.cpp's LED pusher reads this so the active-state
        // colour matches the slot whose action is engaged. Without it,
        // a Shift+press fired the Shift slot's action but the LED kept
        // showing the Plain slot's active colour after release.
        const auto idx = static_cast<size_t>(id);
        if (idx < g_lastFiredMod.size()) {
            g_lastFiredMod[idx].store(static_cast<uint8_t>(slotIdx),
                                       std::memory_order_relaxed);
        }
    }
    runSlot_(slot, firing, pressed);
    return true;
}

int getActiveLayer()
{
    std::lock_guard<std::mutex> lk(g_cfgMutex);
    int n = g_cfg.activeLayer;
    if (n < 0 || n > 2) n = 0;
    return n;
}

namespace {
// Factories all the mutators below funnel through. Caller must already
// hold g_cfgMutex.
void persistLocked_()
{
    ensureConfigDir_();
    writeFile_(configPath_(), serialize(g_cfg));
    g_bindingsGen.fetch_add(1, std::memory_order_relaxed);
}
}

Binding getBinding(int layer, ButtonId id)
{
    std::lock_guard<std::mutex> lk(g_cfgMutex);
    if (layer < 0 || layer > 2) return {};
    auto it = g_cfg.layers[layer].bindings.find(id);
    if (it == g_cfg.layers[layer].bindings.end()) return {};
    return it->second;
}

bool hasBinding(int layer, ButtonId id)
{
    std::lock_guard<std::mutex> lk(g_cfgMutex);
    if (layer < 0 || layer > 2 || id == ButtonId::None) return false;
    return g_cfg.layers[layer].bindings.find(id)
        != g_cfg.layers[layer].bindings.end();
}

void setBinding(int layer, ButtonId id, const Binding& bd)
{
    if (layer < 0 || layer > 2 || id == ButtonId::None) return;
    // Guard: an empty-action Builtin slot is uninvocable — silently
    // no-ops on press AND falsely registers as "bound" so the user
    // can't see why their button does nothing. Coerce to Noop.
    Binding clean = bd;
    auto sanitize = [](ActionStep& sp) {
        if (sp.type == ActionType::Builtin && sp.action.empty()) {
            sp.type = ActionType::Noop;
        }
    };
    for (int m = 0; m < kModifierCount; ++m) {
        sanitize(clean.shortPress[m]);
        for (auto& s : clean.shortPress[m].extraSteps) sanitize(s);
        sanitize(clean.longPress[m]);
        for (auto& s : clean.longPress[m].extraSteps) sanitize(s);
    }
    std::lock_guard<std::mutex> lk(g_cfgMutex);
    g_cfg.layers[layer].bindings[id] = clean;
    persistLocked_();
}

void clearBinding(int layer, ButtonId id)
{
    if (layer < 0 || layer > 2 || id == ButtonId::None) return;
    std::lock_guard<std::mutex> lk(g_cfgMutex);
    g_cfg.layers[layer].bindings.erase(id);
    persistLocked_();
}

UserBank getUserBank(int bankIdx)
{
    std::lock_guard<std::mutex> lk(g_cfgMutex);
    if (bankIdx < 0 || bankIdx >= kUserBankCount) return {};
    return g_cfg.userBanks[bankIdx];
}

void setUserBank(int bankIdx, const UserBank& bank)
{
    if (bankIdx < 0 || bankIdx >= kUserBankCount) return;
    std::lock_guard<std::mutex> lk(g_cfgMutex);
    g_cfg.userBanks[bankIdx] = bank;
    persistLocked_();
}

bool dispatchEncoder(ButtonId id, int stepDelta)
{
    if (id == ButtonId::None || stepDelta == 0) return false;
    Binding bd;
    int layer;
    {
        std::lock_guard<std::mutex> lk(g_cfgMutex);
        layer = g_cfg.activeLayer;
        if (layer < 0 || layer > 2) layer = 0;
        auto it = g_cfg.layers[layer].bindings.find(id);
        if (it == g_cfg.layers[layer].bindings.end()) return false;
        bd = it->second;
    }
    const int slotIdx = static_cast<int>(currentModifierSnapshot());
    if (slotIdx < 0 || slotIdx >= kModifierCount) return false;
    const ActionSlot& slot = bd.shortPress[slotIdx];
    if (slot.type == ActionType::Noop || slot.action.empty()) return false;
    if (slot.type != ActionType::Builtin) {
        // REAPER actions / keyboard / MIDI: fire once per detent. Not
        // step-aware. Acceptable trade-off — for delta-aware behaviour
        // the user picks an encoder-aware builtin.
        runSlot_(slot, /*firing*/ true, /*pressed*/ false);
        return true;
    }
    auto bit = g_builtins.find(slot.action);
    if (bit == g_builtins.end() || !bit->second.run) return false;
    // Delta-aware builtins read `param` as the signed step. Trigger-only
    // builtins (toggles etc.) ignore it and fire once per detent.
    bit->second.run(/*firing*/ true, /*pressed*/ false, /*param*/ stepDelta);
    return true;
}

Binding getUserBankSlot(int bankIdx, int slotIdx)
{
    std::lock_guard<std::mutex> lk(g_cfgMutex);
    if (bankIdx < 0 || bankIdx >= kUserBankCount) return {};
    if (slotIdx < 0 || slotIdx >= kUserBankSlots) return {};
    return g_cfg.userBanks[bankIdx].slots[slotIdx];
}

void setUserBankSlot(int bankIdx, int slotIdx, const Binding& bd)
{
    if (bankIdx < 0 || bankIdx >= kUserBankCount) return;
    if (slotIdx < 0 || slotIdx >= kUserBankSlots) return;
    std::lock_guard<std::mutex> lk(g_cfgMutex);
    g_cfg.userBanks[bankIdx].slots[slotIdx] = bd;
    persistLocked_();
}

bool dispatchUserBankSlot(int bankIdx, int slotIdx, bool pressed)
{
    if (bankIdx < 0 || bankIdx >= kUserBankCount) return false;
    if (slotIdx < 0 || slotIdx >= kUserBankSlots) return false;

    Binding bd;
    {
        std::lock_guard<std::mutex> lk(g_cfgMutex);
        bd = g_cfg.userBanks[bankIdx].slots[slotIdx];
    }

    // Reuse the same long-press / modifier-snapshot logic that
    // dispatch(ButtonId) implements. Press-key namespace uses a
    // synthetic "layer" of 100 + bankIdx so the keys can never
    // collide with real (layer 0..2, id) keys.
    const int  syntheticLayer = 100 + bankIdx;
    const ButtonId pseudoId   = static_cast<ButtonId>(
        0x4000 + slotIdx);   // outside the real ButtonId range
    const uint32_t k = pressKey(syntheticLayer, pseudoId);

    const bool longPressArmed =
        bd.hasLongPress && bd.behavior == Behavior::Momentary;
    const auto& shortP = bd.shortPress;
    const auto& longP  = bd.longPress;

    if (longPressArmed) {
        if (pressed) {
            g_pressStart[k] = { std::chrono::steady_clock::now(),
                                currentModifierSnapshot() };
        } else {
            auto it = g_pressStart.find(k);
            if (it != g_pressStart.end()) {
                const auto held = std::chrono::steady_clock::now() - it->second.start;
                const int  m    = static_cast<int>(it->second.mod);
                g_pressStart.erase(it);
                if (held >= kLongPressThreshold) {
                    runSlot_(longP[m], /*firing*/ true, /*pressed*/ false);
                } else {
                    runSlot_(shortP[m], /*firing*/ true, /*pressed*/ false);
                }
            }
        }
        return true;
    }

    int slotMod = static_cast<int>(Modifier::Plain);
    if (bd.behavior == Behavior::Momentary) {
        if (pressed) {
            g_pressStart[k] = { std::chrono::steady_clock::now(),
                                currentModifierSnapshot() };
            slotMod = static_cast<int>(g_pressStart[k].mod);
        } else {
            auto it = g_pressStart.find(k);
            if (it != g_pressStart.end()) {
                slotMod = static_cast<int>(it->second.mod);
                g_pressStart.erase(it);
            }
        }
    }
    bool firing;
    switch (bd.behavior) {
        case Behavior::Momentary: firing = pressed; break;
        case Behavior::Toggle:    firing = pressed; break;
        case Behavior::Hold:      firing = true;    break;
    }
    runSlot_(shortP[slotMod], firing, pressed);
    return shortP[slotMod].type != ActionType::Noop
        || !shortP[slotMod].action.empty();
}

void setLayerName(int layer, const std::string& name)
{
    if (layer < 0 || layer > 2) return;
    std::lock_guard<std::mutex> lk(g_cfgMutex);
    g_cfg.layers[layer].name = name;
    persistLocked_();
}

void setLayerVpotDefaultMode(int layer, const std::string& mode)
{
    if (layer < 0 || layer > 2) return;
    std::lock_guard<std::mutex> lk(g_cfgMutex);
    g_cfg.layers[layer].vpotDefaultMode = mode;
    persistLocked_();
}

void setLayerAutoMixer(int layer, bool flag)
{
    // Layer 0 (Layer 1) doesn't carry the flag per resolved Q5.
    if (layer < 1 || layer > 2) return;
    std::lock_guard<std::mutex> lk(g_cfgMutex);
    g_cfg.layers[layer].autoWhenMixerVisible = flag;
    if (flag) {
        // Architectural invariant: at most one layer flagged.
        const int other = (layer == 1) ? 2 : 1;
        g_cfg.layers[other].autoWhenMixerVisible = false;
    }
    persistLocked_();
}

void resetLayerToDefaults(int layer)
{
    if (layer < 0 || layer > 2) return;
    std::lock_guard<std::mutex> lk(g_cfgMutex);
    Config tmp;
    seedFactoryDefaults_(tmp);
    g_cfg.layers[layer] = std::move(tmp.layers[layer]);
    persistLocked_();
}

std::vector<std::string> builtinNames()
{
    // No lock — g_builtins is populated once at startup before the USB
    // thread starts and never mutated thereafter. Safe to read.
    std::vector<std::string> out;
    out.reserve(g_builtins.size());
    for (auto& kv : g_builtins) {
        if (kv.first.rfind("__", 0) == 0) continue;  // skip internal sentinels
        out.push_back(kv.first);
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::string builtinDisplayName(const std::string& name)
{
    auto it = g_builtins.find(name);
    if (it == g_builtins.end() || it->second.displayName.empty()) return name;
    return it->second.displayName;
}

bool builtinUsesParam(const std::string& name)
{
    auto it = g_builtins.find(name);
    if (it == g_builtins.end()) return false;
    return it->second.usesParam;
}

bool builtinStateOf(const std::string& name, int param)
{
    auto it = g_builtins.find(name);
    if (it == g_builtins.end() || !it->second.stateOf) return false;
    return it->second.stateOf(param);
}

bool builtinHasState(const std::string& name)
{
    auto it = g_builtins.find(name);
    if (it == g_builtins.end()) return false;
    return static_cast<bool>(it->second.stateOf);
}

void setActiveLayer(int layer)
{
    if (layer < 0 || layer > 2) return;
    std::lock_guard<std::mutex> lk(g_cfgMutex);
    if (g_cfg.activeLayer == layer) return;
    g_cfg.activeLayer = layer;
    // Manual switch wins over a pending mixer-auto save; otherwise
    // closing the mixer would override the user's deliberate choice.
    g_savedLayer = -1;
    ensureConfigDir_();
    writeFile_(configPath_(), serialize(g_cfg));
}

void onMixerVisibilityChanged(bool visible)
{
    std::lock_guard<std::mutex> lk(g_cfgMutex);
    if (visible) {
        // Walk Layers 2/3 (index 1, 2). Layer 1 doesn't carry the flag
        // per resolved Q5. First match wins; UI invariant (Phase C) is
        // "at most one layer flagged".
        for (int i = 1; i <= 2; ++i) {
            if (g_cfg.layers[i].autoWhenMixerVisible) {
                if (g_savedLayer < 0) g_savedLayer = g_cfg.activeLayer;
                g_cfg.activeLayer = i;
                return;
            }
        }
    } else {
        if (g_savedLayer >= 0) {
            g_cfg.activeLayer = g_savedLayer;
            g_savedLayer = -1;
        }
    }
}

uint64_t generation()
{
    return g_bindingsGen.load(std::memory_order_relaxed);
}

Modifier lastFiredModifier(ButtonId id)
{
    const auto idx = static_cast<size_t>(id);
    if (idx >= g_lastFiredMod.size()) return Modifier::Plain;
    const auto v = g_lastFiredMod[idx].load(std::memory_order_relaxed);
    if (v >= kModifierCount) return Modifier::Plain;
    return static_cast<Modifier>(v);
}

} // namespace uf8::bindings
