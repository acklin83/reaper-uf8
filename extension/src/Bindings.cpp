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
std::unordered_map<uint32_t, std::chrono::steady_clock::time_point> g_pressStart;

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
    bd.type     = ActionType::Builtin;
    bd.behavior = b;
    bd.action   = name;
    bd.label    = label;
    bd.param    = param;
    bd.color[0] = r; bd.color[1] = g; bd.color[2] = b_;
    return bd;
}

void seedFactoryDefaults_(Config& c)
{
    c = Config{};
    c.version     = 1;
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

    // Automation row — one builtin per mode (no magic params in JSON).
    // Colours mirror the hardware LED table (Protocol.cpp kUf8GlobalLedTable)
    // so the Bindings editor displays the same swatch the device shows.
    L1[ButtonId::AutoOff]   = mkBuiltin("auto_off",   Behavior::Momentary, "OFF");
    L1[ButtonId::AutoRead]  = mkBuiltin("auto_read",  Behavior::Momentary, "READ",    0,   255,   0);  // green
    L1[ButtonId::AutoWrite] = mkBuiltin("auto_write", Behavior::Momentary, "WRITE", 255,     0,   0);  // red
    L1[ButtonId::AutoTrim]  = mkBuiltin("auto_trim",  Behavior::Momentary, "TRIM",  255,   128,   0);  // orange
    L1[ButtonId::AutoLatch] = mkBuiltin("auto_latch", Behavior::Momentary, "LATCH", 255,     0,   0);  // red
    L1[ButtonId::AutoTouch] = mkBuiltin("auto_touch", Behavior::Momentary, "TOUCH", 255,   255,   0);  // yellow

    // Zoom pad — bundled builtins (REAPER action + LED feedback). Phase B
    // collapses these into ActionType::Reaper once per-binding LED config
    // lands.
    L1[ButtonId::ZoomUp]     = mkBuiltin("zoom_up",     Behavior::Momentary, "ZOOM UP",      0, 255,   0);  // green
    L1[ButtonId::ZoomDown]   = mkBuiltin("zoom_down",   Behavior::Momentary, "ZOOM DOWN",  255, 255,   0);  // yellow
    L1[ButtonId::ZoomLeft]   = mkBuiltin("zoom_left",   Behavior::Momentary, "ZOOM LEFT");
    L1[ButtonId::ZoomRight]  = mkBuiltin("zoom_right",  Behavior::Momentary, "ZOOM RIGHT");
    L1[ButtonId::ZoomCenter] = mkBuiltin("zoom_center", Behavior::Momentary, "FIT",        255,   0,   0);  // red

    // Mode toggles.
    L1[ButtonId::Flip]      = mkBuiltin("flip",                  Behavior::Toggle,    "FLIP");
    L1[ButtonId::PluginBtn] = mkBuiltin("ssl_strip_mode_toggle", Behavior::Toggle,    "PLUGIN");
    L1[ButtonId::Btn360]    = mkBuiltin("mixer_toggle",          Behavior::Momentary, "360");
    L1[ButtonId::Pan]       = mkBuiltin("pan_force",             Behavior::Toggle,    "PAN");

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
        os << "\"type\": ";       appendEscaped(os, actionTypeName(kv.second.type));
        os << ", \"behavior\": "; appendEscaped(os, behaviorName(kv.second.behavior));
        os << ", \"action\": ";   appendEscaped(os, kv.second.action);
        os << ", \"param\": "     << kv.second.param;
        os << ", \"label\": ";    appendEscaped(os, kv.second.label);
        os << ", \"color\": ["
           << int(kv.second.color[0]) << ", "
           << int(kv.second.color[1]) << ", "
           << int(kv.second.color[2]) << "]";
        os << ", \"brightness\": ";
        appendEscaped(os, brightnessName(kv.second.brightness));
        os << ", \"inactive_color\": ["
           << int(kv.second.inactiveColor[0]) << ", "
           << int(kv.second.inactiveColor[1]) << ", "
           << int(kv.second.inactiveColor[2]) << "]";
        os << ", \"inactive_brightness\": ";
        appendEscaped(os, brightnessName(kv.second.inactiveBrightness));
        if (kv.second.type == ActionType::Midi) {
            os << ", \"midi\": {";
            os << "\"device\": ";   appendEscaped(os, kv.second.midiDevice);
            os << ", \"channel\": " << kv.second.midiChannel;
            os << ", \"msg\": "     << kv.second.midiMsgType;
            os << ", \"d1\": "      << kv.second.midiData1;
            os << ", \"d2\": "      << kv.second.midiData2;
            os << "}";
        }
        if (kv.second.hasLongPress) {
            os << ", \"long_press\": {";
            os << "\"type\": ";    appendEscaped(os, actionTypeName(kv.second.longPressType));
            os << ", \"action\": "; appendEscaped(os, kv.second.longPressAction);
            os << ", \"param\": "  << kv.second.longPressParam;
            if (kv.second.longPressType == ActionType::Midi) {
                os << ", \"midi\": {";
                os << "\"device\": ";   appendEscaped(os, kv.second.longPressMidiDevice);
                os << ", \"channel\": " << kv.second.longPressMidiChannel;
                os << ", \"msg\": "     << kv.second.longPressMidiMsgType;
                os << ", \"d1\": "      << kv.second.longPressMidiData1;
                os << ", \"d2\": "      << kv.second.longPressMidiData2;
                os << "}";
            }
            os << "}";
        }
        os << "}";
    }
    if (!first) os << "\n" << fieldPad;
    os << "}\n";
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
    os << "  ]\n";
    os << "}\n";
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
        if (auto* v = be->get_item_by_name("type"))
            bd.type = actionTypeFromName(v->get_string_value());
        if (auto* v = be->get_item_by_name("behavior"))
            bd.behavior = behaviorFromName(v->get_string_value());
        if (auto* v = be->get_item_by_name("action"))
            if (auto* s = v->get_string_value()) bd.action = s;
        if (auto* v = be->get_item_by_name("param"))
            if (auto* s = v->get_string_value(true)) bd.param = std::atoi(s);
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
            // Back-compat: pre-split configs only carried `color`. Mirror it
            // into the inactive colour so quantising into the same palette
            // entry keeps the old visual identity.
            bd.inactiveColor[0] = bd.color[0];
            bd.inactiveColor[1] = bd.color[1];
            bd.inactiveColor[2] = bd.color[2];
        }
        if (auto* v = be->get_item_by_name("inactive_brightness"))
            bd.inactiveBrightness = brightnessFromName(v->get_string_value());
        // MIDI sub-object — only meaningful when type == Midi but we
        // parse it whenever present so the user can switch types in the
        // UI without losing previously-entered MIDI values.
        if (auto* mi = be->get_item_by_name("midi"); mi && mi->is_object()) {
            if (auto* v = mi->get_item_by_name("device"))
                if (auto* s = v->get_string_value()) bd.midiDevice = s;
            if (auto* v = mi->get_item_by_name("channel"))
                if (auto* s = v->get_string_value(true)) bd.midiChannel = std::atoi(s);
            if (auto* v = mi->get_item_by_name("msg"))
                if (auto* s = v->get_string_value(true)) bd.midiMsgType = std::atoi(s);
            if (auto* v = mi->get_item_by_name("d1"))
                if (auto* s = v->get_string_value(true)) bd.midiData1 = std::atoi(s);
            if (auto* v = mi->get_item_by_name("d2"))
                if (auto* s = v->get_string_value(true)) bd.midiData2 = std::atoi(s);
        }
        // Long-press is optional — omit means hasLongPress stays false,
        // so older configs that predate this field still load cleanly.
        if (auto* lp = be->get_item_by_name("long_press"); lp && lp->is_object()) {
            bd.hasLongPress = true;
            if (auto* v = lp->get_item_by_name("type"))
                bd.longPressType = actionTypeFromName(v->get_string_value());
            if (auto* v = lp->get_item_by_name("action"))
                if (auto* s = v->get_string_value()) bd.longPressAction = s;
            if (auto* v = lp->get_item_by_name("param"))
                if (auto* s = v->get_string_value(true)) bd.longPressParam = std::atoi(s);
            if (auto* mi = lp->get_item_by_name("midi"); mi && mi->is_object()) {
                if (auto* v = mi->get_item_by_name("device"))
                    if (auto* s = v->get_string_value()) bd.longPressMidiDevice = s;
                if (auto* v = mi->get_item_by_name("channel"))
                    if (auto* s = v->get_string_value(true)) bd.longPressMidiChannel = std::atoi(s);
                if (auto* v = mi->get_item_by_name("msg"))
                    if (auto* s = v->get_string_value(true)) bd.longPressMidiMsgType = std::atoi(s);
                if (auto* v = mi->get_item_by_name("d1"))
                    if (auto* s = v->get_string_value(true)) bd.longPressMidiData1 = std::atoi(s);
                if (auto* v = mi->get_item_by_name("d2"))
                    if (auto* s = v->get_string_value(true)) bd.longPressMidiData2 = std::atoi(s);
            }
        }
        // Migration: pre-2026-05-02 configs were seeded with white for ALL
        // builtins. The hardware actually drives Auto*/Zoom* in colour. If
        // the user never overrode the colour (still pure white), patch in
        // the hardware default so the editor's swatch matches reality.
        // Skips bindings the user has explicitly customised (any non-white
        // colour is treated as user intent).
        if (bd.type == ActionType::Builtin
            && bd.color[0] == 0xFF && bd.color[1] == 0xFF && bd.color[2] == 0xFF) {
            struct Patch { const char* action; uint8_t r, g, b; };
            static constexpr Patch kPatches[] = {
                {"auto_read",    0,   255,   0},
                {"auto_write",   255, 0,     0},
                {"auto_trim",    255, 128,   0},
                {"auto_latch",   255, 0,     0},
                {"auto_touch",   255, 255,   0},
                {"zoom_up",      0,   255,   0},
                {"zoom_down",    255, 255,   0},
                {"zoom_center",  255, 0,     0},
            };
            uint8_t pr = 0xFF, pg = 0xFF, pb = 0xFF;
            bool patched = false;
            for (const auto& p : kPatches) {
                if (bd.action == p.action) {
                    pr = p.r; pg = p.g; pb = p.b;
                    patched = true;
                    break;
                }
            }
            // Older configs bound the automation row via the parameterised
            // `automation_mode` builtin instead of the per-mode auto_* set.
            // Map (button, param) → hardware colour. Param 0 is shared by
            // OFF and TRIM in REAPER's mode enum, so the ButtonId is the
            // tie-breaker. Param 4 = Latch, 5 = Latch Preview (both red).
            if (!patched && bd.action == "automation_mode") {
                switch (bd.param) {
                    case 1: pr = 0;   pg = 255; pb = 0;   patched = true; break; // Read
                    case 2: pr = 255; pg = 255; pb = 0;   patched = true; break; // Touch
                    case 3: pr = 255; pg = 0;   pb = 0;   patched = true; break; // Write
                    case 4:
                    case 5: pr = 255; pg = 0;   pb = 0;   patched = true; break; // Latch
                    case 0:
                        if (bid == ButtonId::AutoTrim) {
                            pr = 255; pg = 128; pb = 0; patched = true; // orange
                        }
                        // AutoOff at param 0 stays white.
                        break;
                }
            }
            if (patched) {
                bd.color[0]         = pr; bd.color[1]         = pg; bd.color[2]         = pb;
                bd.inactiveColor[0] = pr; bd.inactiveColor[1] = pg; bd.inactiveColor[2] = pb;
            }
        }
        out.bindings[bid] = std::move(bd);
    }
    return true;
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
    return true;
}

} // namespace

void registerBuiltin(const char* name, BuiltinDescriptor desc)
{
    if (!name || !*name) return;
    g_builtins[name] = std::move(desc);
}

void load()
{
    std::lock_guard<std::mutex> lk(g_cfgMutex);

    std::string contents;
    if (readFile_(configPath_(), contents) && !contents.empty()) {
        Config tmp;
        seedFactoryDefaults_(tmp);     // start from factories so missing fields fall back
        if (tryParse_(contents, tmp)) {
            g_cfg = std::move(tmp);
            return;
        }
    }

    // First run, missing file, or parse error: seed factories + persist.
    seedFactoryDefaults_(g_cfg);
    ensureConfigDir_();
    writeFile_(configPath_(), serialize(g_cfg));
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
    }
    return true;
}

const Config& get()
{
    return g_cfg;
}

namespace {

// Snapshot of the fields needed to run a single action — captures both
// the primary and long-press paths in one struct so dispatch doesn't
// have to branch on which sub-bundle of fields to read.
struct ActionSet {
    ActionType  type;
    std::string action;
    int         param;
    std::string midiDevice;
    int         midiChannel;
    int         midiMsgType;
    int         midiData1;
    int         midiData2;
};

ActionSet primaryActionOf(const Binding& bd)
{
    return { bd.type, bd.action, bd.param,
             bd.midiDevice, bd.midiChannel, bd.midiMsgType,
             bd.midiData1, bd.midiData2 };
}
ActionSet longPressActionOf(const Binding& bd)
{
    return { bd.longPressType, bd.longPressAction, bd.longPressParam,
             bd.longPressMidiDevice, bd.longPressMidiChannel,
             bd.longPressMidiMsgType, bd.longPressMidiData1,
             bd.longPressMidiData2 };
}

void runAction_(const ActionSet& a, bool firing, bool pressed)
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

} // namespace

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
    // and long-press based on the held duration.
    const bool longPressArmed =
        bd.hasLongPress && bd.behavior == Behavior::Momentary;
    if (longPressArmed) {
        const uint32_t k = pressKey(layer, id);
        if (pressed) {
            g_pressStart[k] = std::chrono::steady_clock::now();
        } else {
            auto it = g_pressStart.find(k);
            if (it != g_pressStart.end()) {
                const auto held = std::chrono::steady_clock::now() - it->second;
                g_pressStart.erase(it);
                if (held >= kLongPressThreshold) {
                    runAction_(longPressActionOf(bd), /*firing*/ true,
                               /*pressed*/ false);
                } else {
                    runAction_(primaryActionOf(bd),   /*firing*/ true,
                               /*pressed*/ false);
                }
            }
        }
        return true;
    }

    // Standard (no long-press) path — fire per behavior.
    bool firing;
    switch (bd.behavior) {
        case Behavior::Momentary: firing = pressed; break;
        case Behavior::Toggle:    firing = pressed; break;
        case Behavior::Hold:      firing = true;    break;
    }
    runAction_(primaryActionOf(bd), firing, pressed);
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

void setBinding(int layer, ButtonId id, const Binding& bd)
{
    if (layer < 0 || layer > 2 || id == ButtonId::None) return;
    std::lock_guard<std::mutex> lk(g_cfgMutex);
    g_cfg.layers[layer].bindings[id] = bd;
    persistLocked_();
}

void clearBinding(int layer, ButtonId id)
{
    if (layer < 0 || layer > 2 || id == ButtonId::None) return;
    std::lock_guard<std::mutex> lk(g_cfgMutex);
    g_cfg.layers[layer].bindings.erase(id);
    persistLocked_();
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

} // namespace uf8::bindings
