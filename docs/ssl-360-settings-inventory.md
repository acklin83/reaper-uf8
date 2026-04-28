# SSL 360° Settings — Structural Inventory & Gap Analysis

Pulled from the public SSL UF8 / UC1 user guides (manualslib mirror — SSL's
own site 403's WebFetch). For Phase 2.7 spec consolidation only. **Structural
reference, not visual.** No screenshots, no copied SSL chrome.

## SSL 360° page hierarchy (UF8 + UC1 combined)

### HOME
- Menu Toolbar — top-of-window navigation between pages
- Software Version + "Update Software" button
- **Connected Units** list (UF8 / UC1 entries with serial numbers, ~10–15 s
  initial discovery)
  - **Identify** button — overrides the unit's LCD to confirm which
    physical device is which when several are plugged in
  - **Drag-to-Reorder** — sets the physical left-to-right order of multiple
    UF8s
  - Per-unit firmware-update button when SSL ships new firmware
- **Export Report** button — produces a diagnostic `.zip` (system info +
  SSL 360° logs) for support
- "SSL Socials" — links to website / support / social

### UF8 page
The bulk of UF8 configuration.

- **3 Layer tabs** — three DAW profile slots, configurable independently.
  User switches between them via UF8's left-hand Layer button at runtime.
- **DAW Profile** dropdown — picks the active DAW for the selected layer
  (Pro Tools / Logic / Cubase-Nuendo / Ableton Live / Studio One)
- **PORT INFO** button — modal with V-MIDI port setup instructions
- **REVERT** button — resets the current layer to its factory profile
- **Transport Master** dropdown — chooses which of the 3 layers owns the
  transport buttons
- **User-Assignable Keys** assignment grid:
  - 5 banks × 8 = 40 **Top-Row Soft Keys**
  - 3 **Quick Keys**
  - 2 **Foot-switches**
  - Each cell shows the current assignment and a pencil icon to edit
- **Per-key Assignment dialog** (opens on pencil click):
  - Radio: `DAW Command` | `Keyboard Shortcut`
  - DAW Command list — populated from the active DAW profile
  - Keyboard input field — `Type your commands here…`
  - **CLEAR** button
  - **SHORT LABEL** text field — what shows on the LCD above the key
- **Profile Management**:
  - Profile Name display
  - **SAVE** / **LOAD** / **SAVE AS** buttons (`.xml` profile files)
  - Auto-save in background
- **ADVANCED** section (per DAW profile, e.g., HUI block for Pro Tools):
  - **Always Fine Pan** — V-Pot enters fine mode for pan automatically
  - **Always Fine Sends** — same for send levels
  - **Show Auto State** — automation status (READ/WRITE/TRIM) on LCD

### UC1 page
Parallel to UF8 page; separate user keys, plus UC1-specific:
- GR meter ballistic
- Plugin focus / selection behaviour

### PLUG-IN MIXER page
The on-screen mixer view — **what we replace in Phase 2.6**.
- Standard view + zoomed-out view
- Add / remove Channel Strips
- Add / remove Bus Compressors
- Logic Pro 10.6.1+ Aux Tracks special handling

### UF8 LCD MESSAGES / UC1 LCD MESSAGES / SSL 360° SOFTWARE MESSAGES
Reference catalogue of all status / error messages the units can display
(troubleshooting aid).

### Support
FAQs, Q&A, compatibility info (effectively a links page).

## Gap analysis vs Rea-Sixty Phase 2.7 plan

| SSL 360° feature | Rea-Sixty status | Recommendation |
|---|---|---|
| HOME → Connected Units | Settings → **Device** tab — planned | **Add**: Identify (LCD-flash), Drag-to-Reorder, serial number, firmware-update integration (defer firmware to Phase 4 per existing Non-Goal) |
| HOME → Software Version + Update | Settings → **About** tab — planned | Add ReaPack auto-update check |
| HOME → Export Report | not planned | **Add** to Device tab. One button → produces `~/Desktop/rea_sixty_diag_<date>.zip` (build hash, REAPER version, recent extension log, USB device tree). Cheap and pays for itself the first time we debug a remote user |
| UF8 page → 3 Layer tabs (per-DAW) | N/A | **Skip**. Rea-Sixty is REAPER-only by architectural decision |
| UF8 page → DAW Profile dropdown | N/A | **Skip**. Same reason |
| UF8 page → Transport Master | N/A | **Skip**. Single REAPER instance |
| UF8 page → 5 banks × 8 Top Soft Keys | Settings → **Bindings** tab + **Soft-Key Banks** tab | Already planned — **but** existing Bindings doc (`docs/bindings.md`) speaks of "softkeys" generically; should confirm we expose all 5 bank pages, not just one |
| UF8 page → 3 Quick Keys | Settings → Bindings | Add explicit Quick Keys section in Bindings tab (3 dedicated rows). Default DAW assignment is Channel-Strip / Bus-Comp / Metering — we can default these to layer switches |
| UF8 page → 2 Foot-switches | **not planned** | **Add** to Bindings tab. UF8 has the physical jacks — wasted feature if we don't expose them. Same binding model as buttons (REAPER action, builtin, keyboard). Detection of foot-switch press already in our USB protocol decoding TODO |
| Per-key Assignment dialog | Bindings inspector — planned | Match SSL's modal flow: radio (REAPER action / Keyboard / Builtin) + Short Label field + CLEAR. Existing `bindings.md` already specifies this |
| Profile Management (SAVE/LOAD/SAVE AS XML) | Settings → Bindings → Import/Export — planned 2.7e | Use **JSON** not XML (we own the format). One-time SSL 360° XML import for migration, mentioned in ROADMAP non-goals already |
| ADVANCED → Always Fine Pan/Sends | not planned | **Add** to Modes tab as "V-Pot Behaviour" subsection: `Always fine pan` / `Always fine sends` toggles |
| ADVANCED → Show Auto State | not planned | **Add** to Modes tab — toggle to surface REAPER's `GetTrackAutomationMode()` on the scribble strip |
| UC1 page (mirror of UF8 page for UC1) | not planned | **Add** UC1-specific section in Device tab (or split: separate "UC1" tab if config diverges). UC1 has fewer user-assignable keys — minor section, not a top-level tab |
| PLUG-IN MIXER | **Mixer tab** (Phase 2.6) | Already planned, in flight |
| LCD / SOFTWARE MESSAGES catalogue | not planned | **Skip**. We log to file, not modal popups. About tab can link to log location |
| Support / FAQs | About tab links — planned | Sufficient |

## Recommended Phase 2.7 spec changes

**Promote from "deferred" to V1:**
1. **Identify Unit** (LCD-flash via the existing UF8/UC1 frame protocol)
2. **Foot-switch bindings** (2 inputs, same model as buttons)
3. **Export Diagnostic Report** (.zip with logs + version info)
4. **Always Fine Pan/Sends** (V-Pot behaviour toggles)
5. **Show Auto State** on scribble strip (toggle)

**Reject — out of scope by design:**
1. Layer tabs (3-DAW config) — REAPER-only architecture
2. Transport Master selection — single REAPER instance
3. DAW profile XML compatibility — we ship JSON; one-time import only

**Architectural delta from SSL 360°:**
- We don't need a separate "PLUG-IN MIXER" top-level page — Phase 2.6 makes
  it the default tab in the same dockable window. SSL splits HOME from the
  mixer; we don't have to.
- Our "About" replaces SSL's HOME for version/update info, since we have no
  multi-device management surface beyond the Device tab.
