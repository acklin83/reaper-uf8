#pragma once
//
// CsiImport — translate an existing CSI Surface configuration into a
// Rea-Sixty bindings layer.
//
// Reads the CSI files from a chosen Surface directory (typically
// `~/Library/Application Support/REAPER/CSI/Surfaces/<Name>/`):
//   - Surface.txt           — confirms a widget exists on the device
//   - Zones/HomeZones/Home.zon  — the user's global key assignments
//
// The CSI catalogue is much wider than Rea-Sixty's bindable globals
// (per-strip Sel/Cut/Solo/Rec, transport, F1..F8, master fader etc. are
// hard-wired in v1 — see bindings-architecture.md). The import maps the
// subset that overlaps with our `ButtonId` enum and reports the rest as
// skipped, so the user sees what didn't carry over.
//
// CSI action translation:
//   Reaper <id|_named>      → ActionType::Reaper, action stored verbatim
//   GoZone <Name>           → noop, label="Go: <Name>"   (no Rea-Sixty equivalent)
//   Flip / Stop / Play / …  → matching builtin if one exists, else noop
//   Modifier+Widget lines   → skipped (modifier bindings aren't supported in v1)
//

#include <string>
#include <vector>

namespace uf8::csi_import {

struct ImportEntry {
    std::string sourceLine;     // verbatim line from Home.zon (trimmed)
    std::string widget;         // CSI widget name
    std::string action;         // CSI action verbatim
    std::string mappedTo;       // human description of what we did
    bool        applied = false; // true = wrote a binding, false = skipped
    bool        warning = false; // true = applied but with caveats (e.g. named action)
};

struct ImportReport {
    std::string surfaceDir;
    std::string zonePath;       // Home.zon resolved path
    bool        loaded   = false; // Surface.txt + Home.zon read OK
    std::string error;            // populated if loaded=false
    int         appliedCount = 0;
    int         skippedCount = 0;
    int         warningCount = 0;
    std::vector<ImportEntry> entries;
};

// Parse only — no side effects on the bindings config. Use for the
// Settings UI preview pane.
ImportReport preview(const std::string& surfaceDir);

// Apply: parse + write each mapped binding into `targetLayer` (0..2) of
// the active Bindings config. If `clearLayerFirst` is true the layer is
// re-seeded with empty bindings before applying (otherwise existing
// bindings on un-touched buttons are preserved). Returns the same report
// preview() would have produced; entries are marked `applied=true` for
// the rows actually committed.
ImportReport apply(const std::string& surfaceDir,
                   int                targetLayer,
                   bool               clearLayerFirst);

// Default surface directory based on REAPER's resource path. Returns
// "<resource>/CSI/Surfaces/SSLUF8" — typically the path the user has
// their CSI config under. The directory may not exist; caller checks.
std::string defaultSurfaceDir();

} // namespace uf8::csi_import
