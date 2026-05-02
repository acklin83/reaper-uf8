//
// CsiImport — see CsiImport.h.
//
// Parsing approach: hand-rolled line scanner. CSI's .zon format is
// trivial enough (line-based, `//` comments, `Modifier+Widget  Action
// args` rows) that pulling in a real parser would be overkill. The
// nuances we have to honour:
//   - Comments start with `//` or `;`
//   - Indentation is irrelevant
//   - Reaper actions can be quoted: `Reaper "_S&M_FXOFF|"`
//   - Modifier prefixes: `Shift+Plugin`, `Global+Send`, `Hold+Stop`, …
//   - Per-strip postfix `|` (e.g. `Plugin|`) marks a strip-scoped binding
//     — we never import those (Rea-Sixty handles per-strip in dispatch)
//

#include "CsiImport.h"
#include "Bindings.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>
#include <unordered_map>

#include "reaper_plugin_functions.h"

namespace uf8::csi_import {

namespace {

// ---- IO helpers -----------------------------------------------------------

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

bool fileExists_(const std::string& path)
{
    struct stat st{};
    return stat(path.c_str(), &st) == 0;
}

// Strip leading + trailing whitespace.
std::string trim_(std::string s)
{
    auto issp = [](unsigned char c) { return std::isspace(c) != 0; };
    while (!s.empty() && issp(s.front())) s.erase(s.begin());
    while (!s.empty() && issp(s.back()))  s.pop_back();
    return s;
}

// Strip CSI line comments: `// ...` or `; ...`. We must be careful not
// to chop inside a quoted action like `Reaper "_X //weird"`. Cheap walk
// that respects double quotes.
std::string stripComment_(const std::string& s)
{
    bool inq = false;
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (c == '"') inq = !inq;
        if (!inq) {
            if (c == ';') return s.substr(0, i);
            if (c == '/' && i + 1 < s.size() && s[i + 1] == '/')
                return s.substr(0, i);
        }
    }
    return s;
}

// Tokenise on whitespace, honouring `"..."` as a single token with the
// quotes stripped. Returns {} on empty / pure-comment lines.
std::vector<std::string> tokenize_(const std::string& s)
{
    std::vector<std::string> out;
    std::string cur;
    bool inq = false;
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (c == '"') { inq = !inq; continue; }
        if (!inq && std::isspace(static_cast<unsigned char>(c))) {
            if (!cur.empty()) { out.push_back(std::move(cur)); cur.clear(); }
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) out.push_back(std::move(cur));
    return out;
}

// ---- Widget catalogue -----------------------------------------------------
// Surface.txt lists every widget on the SSL UF8. We only care about the
// subset that overlaps with our bindable ButtonId enum. Anything else is
// reported as "no Rea-Sixty equivalent" and skipped.
//
// CSI's `Track` widget is the UF8's PAGE LEFT button (per the comment in
// SSLUF8/Surface.txt) and `Send` is PAGE RIGHT. The naming is misleading
// but baked into existing CSI configs in the wild.

struct WidgetMap {
    const char*           csiName;
    bindings::ButtonId    rsButton;
};

constexpr WidgetMap kWidgetMap[] = {
    // Bank / page navigation (CSI uses physically-printed labels rather
    // than function names, so the SSL "Track" button = page-left etc.)
    { "Track",        bindings::ButtonId::PageLeft  },
    { "Send",         bindings::ButtonId::PageRight },
    { "BankLeft",     bindings::ButtonId::BankLeft  },
    { "BankRight",    bindings::ButtonId::BankRight },

    // Mode buttons
    { "Plugin",       bindings::ButtonId::PluginBtn },
    { "Pan",          bindings::ButtonId::Pan       },
    { "Flip",         bindings::ButtonId::Flip      },

    // Modifier — CSI Shift = Rea-Sixty Fine
    { "Shift",        bindings::ButtonId::Fine      },

    // Automation row
    { "Read",         bindings::ButtonId::AutoRead  },
    { "Write",        bindings::ButtonId::AutoWrite },
    { "Trim",         bindings::ButtonId::AutoTrim  },
    { "Touch",        bindings::ButtonId::AutoTouch },
    { "Latch",        bindings::ButtonId::AutoLatch },

    // Encoder modes — CSI doesn't have these as discrete widgets, but
    // a few configs map them via Marker/Nudge/Zoom modifier groups. We
    // import standalone Nudge as the matching encoder mode toggle.
    { "Nudge",        bindings::ButtonId::Nudge     },

    // Zoom pad — CSI Up/Down/Left/Right are repurposed by Zoom modifier.
    // Standalone bindings on those go to Zoom*; user can rebind later.
    { "Up",           bindings::ButtonId::ZoomUp    },
    { "Down",         bindings::ButtonId::ZoomDown  },
    { "Left",         bindings::ButtonId::ZoomLeft  },
    { "Right",        bindings::ButtonId::ZoomRight },
};

bindings::ButtonId widgetToButtonId_(const std::string& w)
{
    for (auto& m : kWidgetMap) {
        if (w == m.csiName) return m.rsButton;
    }
    return bindings::ButtonId::None;
}

// ---- CSI action → Rea-Sixty Binding ---------------------------------------
// Returns one of:
//   {Type,...}                  — translated, can be written
//   Type=Noop with descriptive label — CSI verb has no equivalent yet
//
// `description` is filled with what we did so the UI preview can show it.

bindings::Binding translateAction_(const std::vector<std::string>& tokens,
                                   std::string& description,
                                   bool&        warning)
{
    bindings::Binding bd;
    bd.behavior = bindings::Behavior::Momentary;
    bd.color[0] = bd.color[1] = bd.color[2] = 255;
    bd.inactiveColor[0] = bd.inactiveColor[1] = bd.inactiveColor[2] = 255;
    warning = false;
    auto& sp = bd.shortPress[static_cast<int>(bindings::Modifier::Plain)];

    if (tokens.empty()) {
        description = "(empty action)";
        return bd;
    }

    const std::string& verb = tokens[0];

    // ---- Reaper <id|_named> ----
    if (verb == "Reaper" && tokens.size() >= 2) {
        sp.type   = bindings::ActionType::Reaper;
        sp.action = tokens[1];
        bd.label  = tokens[1];
        const bool numeric = !sp.action.empty() &&
                             std::all_of(sp.action.begin(), sp.action.end(),
                                         [](unsigned char c) { return std::isdigit(c); });
        if (numeric) {
            description = "REAPER action " + sp.action;
        } else {
            // Named REAPER action (SWS / ReaPack / S&M etc.). Rea-Sixty's
            // dispatch resolves these via NamedCommandLookup at fire time.
            description = "REAPER named action " + sp.action;
            warning = true;
        }
        return bd;
    }

    // ---- GoZone <Name> ----
    if (verb == "GoZone" && tokens.size() >= 2) {
        sp.type  = bindings::ActionType::Noop;
        bd.label = "Go: " + tokens[1];
        description = "CSI GoZone — no Rea-Sixty equivalent (kept as placeholder)";
        warning = true;
        return bd;
    }

    // ---- Direct CSI verbs that map to Rea-Sixty builtins ----
    struct VerbMap { const char* csi; const char* builtin; bindings::Behavior beh; };
    static constexpr VerbMap kVerbs[] = {
        { "Flip",                  "flip",                bindings::Behavior::Toggle    },
        { "GlobalView",            "mixer_toggle",        bindings::Behavior::Momentary },
        // Shift family — all routed to mod_shift; Cmd/Ctrl bindings get
        // their own flavour via dedicated CSI verbs below.
        { "Shift",                 "mod_shift",           bindings::Behavior::Hold      },
        { "Option",                "mod_cmd",             bindings::Behavior::Hold      },
        { "Control",               "mod_ctrl",            bindings::Behavior::Hold      },
        { "Alt",                   "mod_cmd",             bindings::Behavior::Hold      },
    };
    for (auto& v : kVerbs) {
        if (verb == v.csi && v.builtin && *v.builtin) {
            sp.type     = bindings::ActionType::Builtin;
            sp.action   = v.builtin;
            bd.behavior = v.beh;
            bd.label    = v.csi;
            description = "Built-in: " + std::string(v.builtin);
            return bd;
        }
    }

    // ---- TrackAutoMode <N> → matching auto_* builtin ----
    if (verb == "TrackAutoMode" && tokens.size() >= 2) {
        sp.type     = bindings::ActionType::Builtin;
        bd.behavior = bindings::Behavior::Momentary;
        const std::string& mode = tokens[1];
        if      (mode == "0") sp.action = "auto_off";
        else if (mode == "1") sp.action = "auto_read";
        else if (mode == "2") sp.action = "auto_touch";
        else if (mode == "3") sp.action = "auto_write";
        else if (mode == "4") sp.action = "auto_trim";
        else if (mode == "5") sp.action = "auto_latch";
        else                  sp.action = "";
        if (!sp.action.empty()) {
            bd.label    = sp.action;
            description = "Built-in: " + sp.action + " (TrackAutoMode " + mode + ")";
            return bd;
        }
    }

    // ---- Bank Track ±1 → page nav ---------------------------------------
    if (verb == "Bank" && tokens.size() >= 3 && tokens[1] == "Track") {
        const std::string& delta = tokens[2];
        if (delta == "-1" || delta == "-8" || delta == "-999") {
            sp.type   = bindings::ActionType::Builtin;
            sp.action = "bank_left";
            bd.label  = "BANK <";
            description = "CSI Bank Track " + delta + " → bank_left";
            warning = true;
            return bd;
        }
        if (delta == "1" || delta == "8" || delta == "999") {
            sp.type   = bindings::ActionType::Builtin;
            sp.action = "bank_right";
            bd.label  = "BANK >";
            description = "CSI Bank Track " + delta + " → bank_right";
            warning = true;
            return bd;
        }
    }

    // ---- Catch-all: keep as Noop with a descriptive label ---------------
    sp.type  = bindings::ActionType::Noop;
    bd.label = verb;
    if (tokens.size() > 1) {
        bd.label += " " + tokens[1];
    }
    description = "CSI verb \"" + verb + "\" — no Rea-Sixty equivalent";
    warning = true;
    return bd;
}

// ---- Home.zon scanner -----------------------------------------------------

struct ZoneLine {
    std::string             raw;       // original (trimmed) source line
    std::vector<std::string> modifiers;
    std::string             widget;
    bool                    perStrip = false;   // trailing `|`
    std::vector<std::string> action;
};

// Parse a `Modifier1+Modifier2+Widget[|]   Action arg arg` line. Returns
// false for header lines like `Zone Home`, `OnInitialization …`,
// `IncludedZones`, `ZoneEnd`, etc.
bool parseZoneLine_(const std::string& line, ZoneLine& out)
{
    std::string s = stripComment_(line);
    s = trim_(s);
    if (s.empty()) return false;

    auto toks = tokenize_(s);
    if (toks.empty()) return false;

    // Skip CSI structural keywords.
    static const char* kSkip[] = {
        "Zone", "ZoneEnd", "OnInitialization", "OnZoneActivation",
        "OnZoneDeactivation", "IncludedZones", "IncludedZonesEnd",
        "StepSize", "StepSizeEnd", "AccelerationValues",
        "AccelerationValuesEnd",
    };
    for (auto* k : kSkip) {
        if (toks[0] == k) return false;
    }

    // First token = Mod1+Mod2+...+Widget[|]
    std::string head = toks[0];
    bool perStrip = false;
    if (!head.empty() && head.back() == '|') {
        perStrip = true;
        head.pop_back();
    }
    // Split on '+'
    std::vector<std::string> parts;
    {
        std::string cur;
        for (char c : head) {
            if (c == '+') { parts.push_back(std::move(cur)); cur.clear(); }
            else          { cur.push_back(c); }
        }
        if (!cur.empty()) parts.push_back(std::move(cur));
    }
    if (parts.empty()) return false;

    out.raw      = s;
    out.widget   = parts.back();
    out.perStrip = perStrip;
    out.modifiers.assign(parts.begin(), parts.end() - 1);
    out.action.assign(toks.begin() + 1, toks.end());

    // No action = it's a continuation / orphan; ignore.
    if (out.action.empty()) return false;
    return true;
}

// ---- Per-binding: pick the import outcome --------------------------------

// Map a CSI modifier prefix to a Rea-Sixty Modifier slot. Returns
// `false` (and leaves `out` unchanged) for prefixes Rea-Sixty doesn't
// have a slot for — the caller should skip those lines with a warning.
bool csiModifierToSlot_(const std::string& csi, bindings::Modifier& out)
{
    // Mac convention: CSI's Shift = Rea-Sixty Shift; Option = Cmd
    // (because Option is the macOS modifier most often substituted for
    // Cmd in cross-platform configs); Control = Ctrl; Alt is treated
    // as Cmd to keep PC-flavoured CSI configs sensible. Anything else
    // (Global, Hold, Marker, Zoom, …) maps to nothing.
    if (csi == "Shift")   { out = bindings::Modifier::Shift; return true; }
    if (csi == "Option")  { out = bindings::Modifier::Cmd;   return true; }
    if (csi == "Alt")     { out = bindings::Modifier::Cmd;   return true; }
    if (csi == "Control") { out = bindings::Modifier::Ctrl;  return true; }
    return false;
}

// Outcome of parsing one Home.zon line. The caller (scan_) merges this
// into a working Binding for the widget — multiple lines for the same
// widget (one Plain + N modifiers) accumulate in a single Binding.
struct ParsedLine {
    bindings::ButtonId   bid       = bindings::ButtonId::None;
    bindings::Modifier   mod       = bindings::Modifier::Plain;
    bindings::ActionSlot slot;
    bindings::Behavior   behavior  = bindings::Behavior::Momentary;
    std::string          label;
    std::string          description;
    bool                 warning   = false;
    std::string          skipReason;   // non-empty = skip with this message
};

ParsedLine parseImportLine_(const ZoneLine& zl)
{
    ParsedLine p;

    if (zl.perStrip) {
        p.skipReason = "per-strip binding (Rea-Sixty hardcodes per-strip)";
        return p;
    }

    // Translate modifier prefix (if any). Multi-modifier prefixes
    // ("Shift+Option+Widget") are too rare to model — skip them.
    if (zl.modifiers.size() > 1) {
        p.skipReason = "multi-modifier prefix (\"" + zl.modifiers.front()
                     + "+" + zl.modifiers[1] + "+\") not supported";
        return p;
    }
    if (!zl.modifiers.empty()) {
        if (!csiModifierToSlot_(zl.modifiers.front(), p.mod)) {
            p.skipReason = "modifier prefix \"" + zl.modifiers.front()
                         + "+\" has no Rea-Sixty slot";
            return p;
        }
    }

    p.bid = widgetToButtonId_(zl.widget);
    if (p.bid == bindings::ButtonId::None) {
        p.skipReason = "CSI widget \"" + zl.widget
                     + "\" has no Rea-Sixty bindable equivalent";
        return p;
    }

    bindings::Binding tmp = translateAction_(zl.action, p.description, p.warning);
    p.slot     = tmp.shortPress[static_cast<int>(bindings::Modifier::Plain)];
    p.behavior = tmp.behavior;
    p.label    = tmp.label.empty() ? zl.widget : tmp.label;
    return p;
}

ImportReport scan_(const std::string& surfaceDir, bool doApply,
                   int targetLayer, bool clearLayerFirst)
{
    ImportReport r;
    r.surfaceDir = surfaceDir;

    if (surfaceDir.empty()) {
        r.error = "No surface directory configured.";
        return r;
    }

    const std::string surfaceTxt = surfaceDir + "/Surface.txt";
    const std::string homeZon    = surfaceDir + "/Zones/HomeZones/Home.zon";
    r.zonePath = homeZon;

    if (!fileExists_(surfaceTxt)) {
        r.error = "Surface.txt not found at: " + surfaceTxt;
        return r;
    }
    if (!fileExists_(homeZon)) {
        r.error = "Home.zon not found at: " + homeZon;
        return r;
    }

    std::string contents;
    if (!readFile_(homeZon, contents)) {
        r.error = "Could not read: " + homeZon;
        return r;
    }
    r.loaded = true;

    if (doApply && clearLayerFirst &&
        targetLayer >= 0 && targetLayer <= 2) {
        // Clear the layer by walking known ButtonIds. We don't expose a
        // single "clear-all" entry-point — clearBinding per id is fine
        // since the catalogue is small.
        static const bindings::ButtonId kAll[] = {
            bindings::ButtonId::BankLeft,    bindings::ButtonId::BankRight,
            bindings::ButtonId::PageLeft,    bindings::ButtonId::PageRight,
            bindings::ButtonId::Layer1,      bindings::ButtonId::Layer2,
            bindings::ButtonId::Layer3,      bindings::ButtonId::Quick1,
            bindings::ButtonId::Quick2,      bindings::ButtonId::Quick3,
            bindings::ButtonId::PluginBtn,   bindings::ButtonId::Flip,
            bindings::ButtonId::Pan,         bindings::ButtonId::Fine,
            bindings::ButtonId::Btn360,      bindings::ButtonId::AutoOff,
            bindings::ButtonId::AutoRead,    bindings::ButtonId::AutoWrite,
            bindings::ButtonId::AutoTrim,    bindings::ButtonId::AutoLatch,
            bindings::ButtonId::AutoTouch,   bindings::ButtonId::ZoomUp,
            bindings::ButtonId::ZoomDown,    bindings::ButtonId::ZoomLeft,
            bindings::ButtonId::ZoomRight,   bindings::ButtonId::ZoomCenter,
            bindings::ButtonId::Nav,         bindings::ButtonId::Nudge,
            bindings::ButtonId::EncFocus,    bindings::ButtonId::ChannelPush,
        };
        for (auto id : kAll) bindings::clearBinding(targetLayer, id);
        // Re-seed the layer-select bindings on the cleared layer so the
        // user can navigate back. Mirrors seedFactoryDefaults_.
        bindings::Binding ls;
        ls.behavior = bindings::Behavior::Momentary;
        ls.color[0] = ls.color[1] = ls.color[2] = 255;
        ls.inactiveColor[0] = ls.inactiveColor[1] = ls.inactiveColor[2] = 255;
        auto& lsSp = ls.shortPress[static_cast<int>(bindings::Modifier::Plain)];
        lsSp.type = bindings::ActionType::Builtin;
        lsSp.action = "layer_select_1"; ls.label = "LAYER 1";
        bindings::setBinding(targetLayer, bindings::ButtonId::Layer1, ls);
        lsSp.action = "layer_select_2"; ls.label = "LAYER 2";
        bindings::setBinding(targetLayer, bindings::ButtonId::Layer2, ls);
        lsSp.action = "layer_select_3"; ls.label = "LAYER 3";
        bindings::setBinding(targetLayer, bindings::ButtonId::Layer3, ls);
    }

    // Walk every line. We accumulate into a local map<ButtonId, Binding>
    // so multiple lines for the same widget (one Plain + N modifiers)
    // merge into a single Binding without later lines overwriting
    // earlier slots. The map is written out via setBinding only at the
    // end (apply mode).
    std::unordered_map<bindings::ButtonId, bindings::Binding> accum;

    auto recordEntry = [&](const ZoneLine& zl, const ParsedLine& p) {
        ImportEntry e;
        e.sourceLine = zl.raw;
        e.widget     = zl.widget;
        {
            std::string a;
            for (size_t i = 0; i < zl.action.size(); ++i) {
                if (i) a += ' ';
                a += zl.action[i];
            }
            e.action = a;
        }
        if (!p.skipReason.empty()) {
            e.mappedTo = "skipped — " + p.skipReason;
            ++r.skippedCount;
        } else {
            const char* modTag = "";
            switch (p.mod) {
                case bindings::Modifier::Plain: modTag = "";          break;
                case bindings::Modifier::Shift: modTag = " (Shift+)"; break;
                case bindings::Modifier::Cmd:   modTag = " (Cmd+)";   break;
                case bindings::Modifier::Ctrl:  modTag = " (Ctrl+)";  break;
            }
            e.mappedTo = std::string(bindings::toName(p.bid))
                       + modTag + "  ←  " + p.description;
            e.warning  = p.warning;
            e.applied  = true;
            ++r.appliedCount;
        }
        if (e.warning) ++r.warningCount;
        r.entries.push_back(std::move(e));
    };

    size_t pos = 0;
    while (pos < contents.size()) {
        size_t eol = contents.find('\n', pos);
        if (eol == std::string::npos) eol = contents.size();
        std::string line = contents.substr(pos, eol - pos);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        pos = eol + 1;

        ZoneLine zl;
        if (!parseZoneLine_(line, zl)) continue;

        ParsedLine p = parseImportLine_(zl);
        recordEntry(zl, p);
        if (!p.skipReason.empty()) continue;

        bindings::Binding& bd = accum[p.bid];
        if (p.mod == bindings::Modifier::Plain) {
            bd.behavior = p.behavior;
            bd.label    = p.label;
            bd.shortPress[static_cast<int>(bindings::Modifier::Plain)] = p.slot;
        } else {
            bd.shortPress[static_cast<int>(p.mod)] = p.slot;
            // If we've never seen a plain line for this widget yet, leave
            // its plain slot empty — the user can fill it in via the editor.
            // Behavior defaults to Momentary, which is the correct mode for
            // modifier-prefixed lines anyway.
        }
    }

    // Default colours for newly-imported bindings. setBinding overwrites
    // whatever was on the layer (or seeded by the optional clear pass),
    // so we ensure each accumulated binding carries a sensible LED.
    for (auto& kv : accum) {
        bindings::Binding& bd = kv.second;
        if (bd.color[0] == 0 && bd.color[1] == 0 && bd.color[2] == 0) {
            bd.color[0] = bd.color[1] = bd.color[2] = 255;
        }
        if (bd.inactiveColor[0] == 0 && bd.inactiveColor[1] == 0
            && bd.inactiveColor[2] == 0) {
            bd.inactiveColor[0] = bd.color[0];
            bd.inactiveColor[1] = bd.color[1];
            bd.inactiveColor[2] = bd.color[2];
        }
    }

    if (doApply && targetLayer >= 0 && targetLayer <= 2) {
        for (auto& kv : accum) {
            bindings::setBinding(targetLayer, kv.first, kv.second);
        }
    }

    return r;
}

} // namespace

std::string defaultSurfaceDir()
{
    const char* base = GetResourcePath ? GetResourcePath() : nullptr;
    if (!base || !*base) return "";
    return std::string(base) + "/CSI/Surfaces/SSLUF8";
}

ImportReport preview(const std::string& surfaceDir)
{
    return scan_(surfaceDir, /*doApply*/ false, /*layer*/ -1, /*clear*/ false);
}

ImportReport apply(const std::string& surfaceDir, int targetLayer,
                   bool clearLayerFirst)
{
    return scan_(surfaceDir, /*doApply*/ true, targetLayer, clearLayerFirst);
}

} // namespace uf8::csi_import
