//
// UserPluginCatalog — runtime catalogue of user-learned plugin maps.
//
// JSON style mirrors Bindings.cpp: WDL/jsonparse for reading, hand-written
// serializer for writing. Path: <REAPER_RESOURCE>/rea_sixty/user_plugins.json.
// Sibling to bindings.json on purpose — same lifecycle, same backup story.
//
// Atomic write: serialise to <path>.tmp, rename onto <path>. Pre-write
// backup to <path>.bak when the destination already exists.
//

#include "UserPluginCatalog.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <sstream>
#include <string>
#include <sys/stat.h>

#ifdef _WIN32
  #include <direct.h>
  #include <io.h>
#else
  #include <unistd.h>
#endif

#include "reaper_plugin_functions.h"

#include "WDL/jsonparse.h"

namespace uf8::user_plugins {

namespace {

std::mutex          g_mutex;
UserPluginCatalog   g_catalog;

// View cache: synthesised PluginMap structs returned from lookupByName.
// Rebuilt on every mutation so spans stay valid until the next change.
struct ViewCacheEntry {
    std::string             match;        // owns string memory
    std::string             displayShort;
    PluginMap               map;          // points into the strings + slotsBuf
    std::vector<LinkSlot>   slotsBuf;
};
std::vector<ViewCacheEntry> g_viewCache;

// ---- path helpers ----------------------------------------------------------

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
    return configDir_() + "/user_plugins.json";
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

bool fileExists_(const std::string& path)
{
    struct stat st{};
    return stat(path.c_str(), &st) == 0;
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

bool writeFileAtomic_(const std::string& path, const std::string& contents)
{
    const std::string tmp = path + ".tmp";
    FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f) return false;
    if (!contents.empty()) {
        if (std::fwrite(contents.data(), 1, contents.size(), f) != contents.size()) {
            std::fclose(f);
            std::remove(tmp.c_str());
            return false;
        }
    }
    std::fclose(f);

    if (fileExists_(path)) {
        const std::string bak = path + ".bak";
        // Best-effort backup; ignore failure (rename across the same
        // directory is cheap and rarely fails — but if it does, we'd
        // rather still write the new file than block the save).
        std::remove(bak.c_str());
        std::rename(path.c_str(), bak.c_str());
    }

    if (std::rename(tmp.c_str(), path.c_str()) != 0) {
        std::remove(tmp.c_str());
        return false;
    }
    return true;
}

void logErr_(const char* fmt, ...)
{
#ifdef _WIN32
    const char* logPath = "rea_sixty.log";
#else
    const char* logPath = "/tmp/rea_sixty.log";
#endif
    if (FILE* lf = std::fopen(logPath, "a")) {
        va_list ap;
        va_start(ap, fmt);
        std::fprintf(lf, "[user_plugins] ");
        std::vfprintf(lf, fmt, ap);
        std::fprintf(lf, "\n");
        va_end(ap);
        std::fclose(lf);
    }
}

// ---- JSON helpers ----------------------------------------------------------

const char* domainName_(Domain d)
{
    switch (d) {
        case Domain::ChannelStrip: return "ChannelStrip";
        case Domain::BusComp:      return "BusComp";
        default:                   return "None";
    }
}

Domain domainFromName_(const char* s)
{
    if (!s) return Domain::None;
    if (std::strcmp(s, "ChannelStrip") == 0) return Domain::ChannelStrip;
    if (std::strcmp(s, "BusComp")      == 0) return Domain::BusComp;
    return Domain::None;
}

void appendEscaped_(std::ostringstream& os, const std::string& s)
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

bool getStrI_(wdl_json_element* obj, const char* key, std::string& out)
{
    if (!obj) return false;
    auto* v = obj->get_item_by_name(key);
    if (!v) return false;
    if (auto* s = v->get_string_value()) { out = s; return true; }
    return false;
}

bool getIntI_(wdl_json_element* obj, const char* key, int& out)
{
    if (!obj) return false;
    auto* v = obj->get_item_by_name(key);
    if (!v) return false;
    if (auto* s = v->get_string_value(true)) { out = std::atoi(s); return true; }
    return false;
}

bool getDoubleI_(wdl_json_element* obj, const char* key, double& out)
{
    if (!obj) return false;
    auto* v = obj->get_item_by_name(key);
    if (!v) return false;
    if (auto* s = v->get_string_value(true)) { out = std::atof(s); return true; }
    return false;
}

bool getBoolI_(wdl_json_element* obj, const char* key, bool& out)
{
    if (!obj) return false;
    auto* v = obj->get_item_by_name(key);
    if (!v) return false;
    if (auto* s = v->get_string_value(true)) {
        out = (std::strcmp(s, "true") == 0 || std::strcmp(s, "1") == 0);
        return true;
    }
    return false;
}

// ---- Serialise --------------------------------------------------------------

std::string serialize_(const UserPluginCatalog& c)
{
    std::ostringstream os;
    os << "{\n";
    os << "  \"format_version\": " << c.formatVersion << ",\n";
    os << "  \"plugins\": [";
    bool firstPlugin = true;
    for (const auto& m : c.maps) {
        if (!firstPlugin) os << ",";
        firstPlugin = false;
        os << "\n    {\n";
        os << "      \"match\": ";        appendEscaped_(os, m.match);        os << ",\n";
        os << "      \"domain\": ";       appendEscaped_(os, domainName_(m.domain)); os << ",\n";
        os << "      \"displayShort\": "; appendEscaped_(os, m.displayShort); os << ",\n";
        os << "      \"isDefault\": "     << (m.isDefault ? "true" : "false") << ",\n";
        os << "      \"slots\": [";
        bool firstSlot = true;
        for (const auto& s : m.slots) {
            if (!firstSlot) os << ",";
            firstSlot = false;
            os << "\n        { \"linkIdx\": " << s.linkIdx
               << ", \"vst3Param\": "         << s.vst3Param
               << ", \"inverted\": "          << (s.inverted ? "true" : "false")
               << " }";
        }
        os << "\n      ]";
        if (m.metering.grVst3Param >= 0) {
            os << ",\n      \"metering\": { \"gainReduction\": { "
               << "\"vst3Param\": " << m.metering.grVst3Param
               << ", \"offsetDb\": " << m.metering.grOffsetDb
               << " } }";
        }
        os << "\n    }";
    }
    if (!firstPlugin) os << "\n  ";
    os << "]\n}\n";
    return os.str();
}

// ---- Parse ------------------------------------------------------------------

bool parse_(const std::string& json, UserPluginCatalog& out)
{
    wdl_json_parser p;
    wdl_json_element* root = p.parse(json.c_str(), static_cast<int>(json.size()));
    if (!root || !root->is_object()) return false;

    int fv = 1;
    getIntI_(root, "format_version", fv);
    if (fv > kCurrentFormatVersion) {
        logErr_("refusing to load format_version=%d (max known=%d)",
                fv, kCurrentFormatVersion);
        return false;
    }
    out.formatVersion = fv;
    out.maps.clear();

    auto* arr = root->get_item_by_name("plugins");
    if (!arr || !arr->is_array() || !arr->m_array) return true;

    const int n = arr->m_array->GetSize();
    for (int i = 0; i < n; ++i) {
        wdl_json_element* po = arr->enum_item(i);
        if (!po || !po->is_object()) continue;

        UserPluginMap m;
        getStrI_(po, "match", m.match);
        std::string dom;
        if (getStrI_(po, "domain", dom)) m.domain = domainFromName_(dom.c_str());
        getStrI_(po, "displayShort", m.displayShort);
        if (m.displayShort.size() > 4) m.displayShort.resize(4);
        getBoolI_(po, "isDefault", m.isDefault);

        if (auto* slotsArr = po->get_item_by_name("slots");
            slotsArr && slotsArr->is_array() && slotsArr->m_array)
        {
            const int sn = slotsArr->m_array->GetSize();
            for (int s = 0; s < sn; ++s) {
                wdl_json_element* so = slotsArr->enum_item(s);
                if (!so || !so->is_object()) continue;
                UserLinkSlot us{};
                getIntI_(so, "linkIdx", us.linkIdx);
                getIntI_(so, "vst3Param", us.vst3Param);
                getBoolI_(so, "inverted", us.inverted);
                if (us.linkIdx < 0 || us.vst3Param < 0) continue;
                m.slots.push_back(us);
            }
        }

        if (auto* met = po->get_item_by_name("metering");
            met && met->is_object())
        {
            if (auto* gr = met->get_item_by_name("gainReduction");
                gr && gr->is_object())
            {
                int gp = -1;
                getIntI_(gr, "vst3Param", gp);
                m.metering.grVst3Param = gp;
                getDoubleI_(gr, "offsetDb", m.metering.grOffsetDb);
            }
        }

        if (m.match.empty() || m.domain == Domain::None) continue;
        out.maps.push_back(std::move(m));
    }

    // Enforce isDefault one-of per domain (highest-index wins on conflict).
    bool seenCs = false, seenBc = false;
    for (auto it = out.maps.rbegin(); it != out.maps.rend(); ++it) {
        if (!it->isDefault) continue;
        bool& seen = (it->domain == Domain::BusComp) ? seenBc : seenCs;
        if (seen) it->isDefault = false;
        else      seen = true;
    }
    return true;
}

// ---- View-cache rebuild ----------------------------------------------------

// Find canonical id/name/legend strings (static-storage const char*) for a
// linkIdx by walking built-in maps. Falls back to empty strings when no
// built-in slot uses that linkIdx (e.g. a learned param the user pinned to
// linkIdx 200 — unusual but legal).
const LinkSlot* canonicalSlot_(int linkIdx)
{
    for (const auto& m : allPluginMaps()) {
        if (const auto* s = findSlotByLinkIdx(m, linkIdx)) return s;
    }
    return nullptr;
}

void rebuildViewCache_()
{
    g_viewCache.clear();
    g_viewCache.reserve(g_catalog.maps.size());
    for (const auto& m : g_catalog.maps) {
        ViewCacheEntry e;
        e.match        = m.match;
        e.displayShort = m.displayShort.empty() ? std::string("USR") : m.displayShort;
        e.slotsBuf.reserve(m.slots.size());
        for (const auto& us : m.slots) {
            const LinkSlot* canon = canonicalSlot_(us.linkIdx);
            LinkSlot ls{
                us.linkIdx,
                canon ? canon->id     : "",
                canon ? canon->name   : "",
                canon ? canon->legend : "",
                us.vst3Param,
                us.inverted,
                canon ? canon->deflt  : std::nullopt,
            };
            e.slotsBuf.push_back(ls);
        }
        g_viewCache.push_back(std::move(e));
    }
    // Now populate map fields whose char* / span members must point into
    // the cached entry. Done in a second pass so vector reallocation
    // during reserve()/push_back doesn't dangle the pointers.
    for (auto& e : g_viewCache) {
        // Domain is captured by value from the source UserPluginMap.
        const UserPluginMap* src = nullptr;
        for (const auto& m : g_catalog.maps) {
            if (m.match == e.match) { src = &m; break; }
        }
        e.map = PluginMap{
            e.match.c_str(),
            e.displayShort.c_str(),
            src ? src->domain : Domain::None,
            std::span<const LinkSlot>{ e.slotsBuf.data(), e.slotsBuf.size() },
        };
    }
}

} // namespace

void load()
{
    std::lock_guard<std::mutex> lk(g_mutex);
    g_catalog = {};

    std::string contents;
    if (readFile_(configPath_(), contents) && !contents.empty()) {
        UserPluginCatalog tmp;
        tmp.formatVersion = kCurrentFormatVersion;
        if (parse_(contents, tmp)) {
            g_catalog = std::move(tmp);
        } else {
            logErr_("parse failed for %s — leaving catalog empty",
                    configPath_().c_str());
        }
    }
    rebuildViewCache_();
}

// Built-in collision check. Walks the static built-in registry (no
// catalog access), so it's safe to call with or without g_mutex held.
bool collidesWithBuiltin_(std::string_view match)
{
    if (match.empty()) return false;
    for (const auto& bm : allPluginMaps()) {
        std::string_view bmm{ bm.match };
        if (match.find(bmm) != std::string_view::npos) return true;
        if (bmm.find(match) != std::string_view::npos) return true;
    }
    return false;
}

SaveResult save()
{
    std::lock_guard<std::mutex> lk(g_mutex);

    for (const auto& m : g_catalog.maps) {
        if (collidesWithBuiltin_(m.match)) {
            // Built-in already claims this match string. Refuse the save
            // — built-ins must stay unshadowable, and this also prevents
            // the user from accidentally shadowing CS 2 with a half-
            // mapped catalog entry.
            logErr_("save refused: '%s' collides with a built-in PluginMap",
                    m.match.c_str());
            return SaveResult::Collision;
        }
    }

    ensureConfigDir_();
    g_catalog.formatVersion = kCurrentFormatVersion;
    if (!writeFileAtomic_(configPath_(), serialize_(g_catalog))) {
        logErr_("atomic write failed for %s", configPath_().c_str());
        return SaveResult::IoError;
    }
    return SaveResult::Ok;
}

const UserPluginCatalog& get()
{
    return g_catalog;
}

void setAll(UserPluginCatalog c)
{
    std::lock_guard<std::mutex> lk(g_mutex);
    g_catalog = std::move(c);
    rebuildViewCache_();
}

void upsert(UserPluginMap m)
{
    std::lock_guard<std::mutex> lk(g_mutex);
    auto it = std::find_if(g_catalog.maps.begin(), g_catalog.maps.end(),
        [&](const UserPluginMap& x) { return x.match == m.match; });

    if (m.isDefault) {
        for (auto& other : g_catalog.maps) {
            if (other.domain == m.domain && other.match != m.match) {
                other.isDefault = false;
            }
        }
    }
    if (it != g_catalog.maps.end()) *it = std::move(m);
    else                            g_catalog.maps.push_back(std::move(m));
    rebuildViewCache_();
}

bool removeByMatch(std::string_view match)
{
    std::lock_guard<std::mutex> lk(g_mutex);
    auto it = std::find_if(g_catalog.maps.begin(), g_catalog.maps.end(),
        [&](const UserPluginMap& x) { return x.match == match; });
    if (it == g_catalog.maps.end()) return false;
    g_catalog.maps.erase(it);
    rebuildViewCache_();
    return true;
}

const PluginMap* lookupByName(std::string_view fxName)
{
    // No lock needed for read path: rebuildViewCache_ runs under the
    // mutex, and the cache is stable between mutations. Lookup happens
    // on the REAPER tick thread; mutations happen from the editor UI on
    // the same thread, so there's no race in practice. If we ever push
    // mutations off-thread this needs revisiting.
    for (const auto& e : g_viewCache) {
        if (fxName.find(e.match) != std::string_view::npos) return &e.map;
    }
    return nullptr;
}

bool collidesWithBuiltin(std::string_view match)
{
    return collidesWithBuiltin_(match);
}

} // namespace uf8::user_plugins
