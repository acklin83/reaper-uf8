# Plan: FX Learn + Multi-Instance (CS / BC)

Status: design draft. Nichts gebaut. `fx_learn` Builtin existiert als Stub.

Zwei verkoppelte Features, die zusammen geplant werden müssen, weil sie dieselbe Lookup-API durchziehen (`PluginMap.cpp`).

---

## A) FX Learn — Drittplugins als virtuelle CS / BC behandeln

**Ziel:** Verhalten wie SSL 360° Link, aber **ohne VST3-Wrapper-Plugin**. Drittanbieter-Plugins werden nach Learn so behandelt als wären sie CS oder BC — derselbe V-Pot/Scribble/Push-Code-Pfad, dieselben Slot-Bedeutungen (HF Gain, HMF Gain, Comp Threshold …).

**Kern-Insight:** Das compile-time `PluginMap`-Modell ([PluginMap.h:92](extension/src/PluginMap.h:92)) ist bereits genau die richtige Abstraktion. Slot N hat eine bekannte semantische Bedeutung (`SlotIds::HfGain` = LinkIdx 7 etc.), gemappt auf einen plugin-spezifischen `vst3Param`. Die statischen Maps in `PluginMap.cpp` kennen Channel Strip 2, 4K B/E/G und Bus Comp 2.

**Was hinzukommt:** ein **Runtime-Layer** parallel zu den statischen Maps.

### Datenmodell

```cpp
// Persistente User-Definition — ein gelerntes Drittplugin.
struct UserSlotBinding {
    int  linkIdx;     // ein bestehender SlotIds::* Wert (HfGain, etc.)
    int  vst3Param;   // Param-Index auf DEM Plugin
    bool inverted;    // optional, default false
};

struct UserPluginMap {
    std::string match;          // Substring von TrackFX_GetFXName ("FabFilter Pro-Q 4")
    Domain      domain;         // ChannelStrip oder BusComp
    char        displayShort[5];// "FFP4", "U-EQ" — 4 chars, vom User (optional, sonst FX-Name truncated)
    bool        isDefault;      // Wenn true: gewinnt gegen built-in SSL als
                                // domain-default beim Initial-Picker für
                                // neue Tracks (siehe Teil B). Pro Domain
                                // sollte nur ein UserPluginMap diesen
                                // Flag tragen — UI enforced one-of.
    std::vector<UserSlotBinding> slots;
};

struct UserPluginCatalog {
    std::vector<UserPluginMap> maps;   // load/save aus JSON
};
```

**Persistenz:** sidecar zu `bindings.json` — `user_plugins.json` im REAPER-Resource-Pfad. Format-Version-Feld für künftige Migrationen (analog zu wie wir Bindings versionieren).

### Lookup-Integration

`lookupPluginMapByName(fxName)` ([PluginMap.cpp:363](extension/src/PluginMap.cpp:363)) ist die einzige Stelle die geändert werden muss. Zwei-Stufen-Lookup:

1. Erst **statische Maps** durchsuchen — Built-in CS/BC haben Vorrang. Wenn der User eine 4K E mappt, will er trotzdem die echte 4K-E-Definition.
2. Dann **User-Catalog** durchsuchen.

Damit der Rest der Pipeline (V-Pot push, Render, Focus, Multi-instance) **null Änderungen** braucht: User-Maps werden zu `PluginMap`-Instanzen synthetisiert (gleiche Struktur, slots aus den UserSlotBindings). Domain-Aware-Lookup, FocusedParam, alles funktioniert.

### Fader und Pan im Plugin-Modifier-Mode

Plugin-Modifier (`ssl_strip_mode_toggle` / `g_pluginFaderMode`) routet Fader auf den Plugin-Output-Fader und V-Pot auf den Plugin-Pan. Bei Built-in-Maps sind das die Slots mit **linkIdx=1** ("FaderLevel") und **linkIdx=3** ("Pan") — ganz normale Einträge in der `slots`-Liste ([PluginMap.cpp:274-276](extension/src/PluginMap.cpp:274)). Der Modifier-Code macht effektiv `findSlotByLinkIdx(map, 1)` / `findSlotByLinkIdx(map, 3)`.

**Konsequenz für FX Learn:** wenn der User beim Lernen die Slots mit linkIdx 1 und 3 mappt, sind sie automatisch Fader/Pan im Plugin-Modifier-Mode. Kein extra Datenfeld in `UserPluginMap` nötig — alles geht durch denselben Slot-Mechanismus. Beim Learn-Wizard werden Fader und Pan einfach prominent als die ersten zwei Slots vorgeschlagen, weil sie für den Plugin-Modifier-Mode kritisch sind.

**Fallback wenn Fader/Pan nicht gelernt:** `findSlotByLinkIdx` returnt nullptr → Modifier-Mode-Code fällt auf REAPER-Track-Volume / -Pan zurück. Akzeptables Verhalten für Plugins ohne sinnvolle Master-Fader (Multiband-Comps, EQs ohne Output-Stage, …).

### Learn-Flow (UI) — primär: visuelles Schematic

**Wir haben das richtige Werkzeug schon: `drawUf8Vector`** ([SettingsScreen.cpp:353](extension/src/SettingsScreen.cpp:353)) ist ein 1000×490 Vector-Canvas mit Hit-Test-Patterns (`drawHwBtn`) und etabliertem Hover/Select-Verhalten. Dasselbe Toolset ergibt eine **virtuelle Channel-Strip-Topologie** im Mixer-Window — visuell wie SSL 360 Link's Plugin-GUI, aber direkt in unserem ImGui-Window statt als VST3.

#### Layout pro Domain

**Channel-Strip-Schematic** (für `Domain::ChannelStrip`):
- Input-Section: Input Trim, Phase, Mic/Drive, Pre, Impedance — als kleine Pads links.
- EQ-Section: vier Bänder (HF / HMF / LMF / LF) mit je Gain / Freq / Q als Knob-Trio. HF/LF Shelf-Toggle als kleiner Pad oben am Knob.
- Filter-Section: HPF / LPF Cutoff + zugehörige Routing-Toggles.
- Dynamics-Section: Comp + Gate jeweils als Block (Threshold / Ratio / Attack / Release / Range / Hold), plus External-S/C / Filter-Routing-Pads.
- Output-Section: Width, Pan, Output Trim, **Fader Level** (gross hervorgehoben, bottom-right wie auf einem echten Strip).
- Authoritative Slot-Liste aus [docs/ssl-native-params/VST3__SSL_360_Link_(SSL).md](docs/ssl-native-params/VST3__SSL_360_Link_(SSL).md).

**Bus-Comp-Schematic** (für `Domain::BusComp`):
- Eigene kleinere Topologie — Threshold / Ratio / Attack / Release / Make-up / Mix / Sidechain-HPF / etc.
- Layout-Vorlage: [docs/ssl-native-params/VST3__SSL_360_Link_Bus_Compressor_(SSL).md](docs/ssl-native-params/VST3__SSL_360_Link_Bus_Compressor_(SSL).md).

**UC1-Schematic** (analog, separater Tab im Learn-UI):
- UC1 hat dedicated Hardware-Knöpfe für Channel-Strip-Topologie — die Schematic zeigt den **physischen** UC1-Layout. Für FX Learn weniger relevant (UC1 mappt automatisch was die Domain-Map liefert), aber sinnvoll für Verification-View: "welche meiner gelernten Slots erreicht UC1?".

#### Slot-Status-Anzeige

Jeder Slot im Schematic wird in einem von drei Zuständen gerendert:

- **Unmapped** (grau, gestrichelter Border): noch nichts gelernt für diesen Slot auf dem aktuell editierten Plugin.
- **Mapped** (grün, voller Border, kleiner Param-Index/Name daneben): `vst3Param` ist gebunden.
- **Listening** (gelb pulsierend): aktiv im Lern-Modus — der nächste `GetLastTouchedFX` füttert diesen Slot.

Inverted-Flag als kleiner "↺"-Indikator am Slot, Rechts-Klick toggled.

#### Interaktion

- **Linksklick auf Slot** → Slot in "Listening" schalten. Nur ein Slot listening at a time. Nächster Plugin-Param-Wackel fängt → bindet → grün.
- **Cmd+Klick / Bulk-Mode** → "Sequential Listen": nach jedem Bind springt der Listen-Status automatisch zum nächsten unmapped Slot in der Topologie. Erlaubt Quick-Mass-Mapping (Slot 1 wackeln, Slot 2 wackeln, …) ohne zwischen jedem in die UI zurückzuwechseln.
- **Rechtsklick** → Inverted-Flag toggle / Mapping löschen / Param-Re-Pick.
- **ESC** → Listening abbrechen.
- **Plugin-Selector oben** im Schematic-Header: Dropdown der FX-Chain auf dem focused Track plus Domain-Radio (CS/BC). Wechselt die UserPluginMap, an der gerade gearbeitet wird.

#### Identität-Footer

Unter dem Schematic ein kleiner Footer mit:
- `match`-Substring (auto-vorbelegt vom FX-Namen, editierbar).
- `displayShort` 4-char Feld (auto-vorbelegt vom getrimmten FX-Namen).
- Checkbox "Make this the default for CS / BC".
- Save-Button (oder live-save bei jedem Mapping-Edit, wenn die UX flüssig genug ist).

### Learn-Flow (UI) — sekundär: Wizard

Wenn ein User explizit nach Klick-Through fragt (zugänglicher für Anfänger): Reihenfolge der Schritte:

**1. Quelle wählen**
- User aktiviert `fx_learn` (Builtin existiert schon).
- Mixer-Window schaltet auf Learn-UI um.
- Plugin-Source: entweder `GetLastTouchedFX` (wenn der User gerade einen Param wackelt → wir wissen track + fxIdx + vst3Param sofort), oder Dropdown der FX-Chain auf dem focused Track. Festgehalten als FX-Name via `TrackFX_GetFXName` → wird der `match`-Substring der UserPluginMap.

**2. Domain wählen**
- Radio-Buttons ChannelStrip / BusComp.
- Pro UserPluginMap genau eine Domain. Wer ein Plugin als beides will, legt zwei Maps an (einmal pro Domain).

**3. Slots mappen — Wizard**
Slot-Liste, vorsortiert nach Wichtigkeit für Plugin-Modifier-Mode:
- linkIdx=1 "Fader Level" — kritisch für Modifier-Fader-Mode
- linkIdx=3 "Pan" — kritisch für Modifier-V-Pot-Mode
- Dann der Rest der Domain in 360-Link-Reihenfolge (HF Gain, HMF Gain, …).

Pro Slot:
- UI zeigt den Slot-Namen + kurze Erklärung.
- Drei Aktionen:
  - **Learn next touched param**: User wackelt im Plugin-GUI → wir lesen `GetLastTouchedFX().vst3Param` und binden.
  - **Skip**: Slot bleibt unbelegt (Plugin hat ihn nicht / User will ihn nicht).
  - **Back**: einen Slot zurück, korrigieren.
- Inverted-Checkbox pro Slot.
- Confirm-Button schreibt den Eintrag in den UserPluginMap-Entwurf.

**4. Identität bestätigen**
- `displayShort`: 4-char Feld, default-vorbelegt aus dem getrimmten FX-Namen ("FabFilter Pro-Q 4" → "FABF").
- Checkbox "Make this the default for CS" (oder BC). Setzt `isDefault = true`, löscht den Flag bei allen anderen UserPluginMaps derselben Domain (UI enforced one-of).

**5. Speichern**
- "Done" → UserPluginMap landet im Catalog, JSON wird sofort persistiert.
- Live-Test: User kann die Hardware drehen, sollte direkt den richtigen Param treiben.

**Alternative (schneller, später):** "wackel zuerst Hardware, dann Software" wie SSL 360 das macht. Erste Iteration ist Software-only Wizard, weil weniger Henne-Ei.

### Edge Cases

- **Plugin matched sowohl statisch als auch user-defined** — statische Map gewinnt (siehe oben).
- **User-Plugin matched mehrere Tracks mit verschiedenen FX-Namen** — `match` ist Substring; "Pro-Q" matched alle Pro-Q-Versionen.
- **Param-Index ändert sich nach Plugin-Update** — User muss neu lernen. Akzeptabel; wir können später `TrackFX_GetParamIdent` als Stable-Anchor nutzen wenn das Hersteller liefert.
- **Multi-Instance** — siehe Teil B; UserPluginMap funktioniert dort gleich wie statische Maps.

---

## B) Multi-Instance — 2a/2b/2c auf Surface

### Problem

`lookupPluginOnTrack(tr, domain)` ([PluginMap.cpp:428](extension/src/PluginMap.cpp:428)) returnt **erste passende Instanz**. ~25 Aufrufstellen (`grep -n lookupPluginOnTrack` in main.cpp + UC1Surface.cpp). Wenn ein Track zwei CS hostet (parallele EQ-Layer, Sidechain, Pre/Post-Routing), sehen wir nur eine.

### Datenmodell

**Keine Änderung an `FocusedParam`** — `{Domain, slotIdx}` bleibt. Active-Instance ist orthogonal: pro Track + pro Domain.

```cpp
// Per-Track Active-Instance — welche der N CS/BC Instanzen
// auf einem Track gerade "die" ist.
struct ActiveInstanceState {
    // key: track GUID (REAPER GUID strings sind stable über Reloads)
    // val: { cs_instance_idx, bc_instance_idx }
    // Default 0 — Single-Instance-Verhalten unverändert.
    std::unordered_map<std::string, std::array<int, 2>> perTrack;
};
```

**Persistenz:** REAPER `GetSetMediaTrackInfo_String("P_EXT:rea_sixty_inst", ...)` — per-track ExtState, lebt im Projekt-File. Damit folgt's beim Project-Save automatisch.

**Sticky-Verhalten:** der Active-Instance-Wert pro Track ist per Definition sticky — er wird beim Wechseln vom Track NICHT geändert. Wer Track 12 mit Instanz "b" verlässt und später zurückkommt, sieht wieder Instanz "b". Der Wert ändert sich nur durch explizite User-Aktion (`next_instance` / `prev_instance` / `instance_select` / Shift+Channel-Encoder).

**Initial-Default für unbekannte Tracks:** wenn ein Track zum ersten Mal angeschaut wird (kein `P_EXT:rea_sixty_inst` Eintrag), wählt der Picker:

1. Die erste Instanz die zu einem **`isDefault = true`** UserPluginMap matched (siehe Teil A) — falls eine vorhanden ist.
2. Sonst die erste Instanz die zu einer **built-in SSL Map** (CS 2, 4K B/E/G, BC 2) matched.
3. Sonst FX-Chain-Order [0] — fallback wenn weder noch.

Damit gilt: solange der User keinen User-Plugin als Default markiert, bleibt SSL erste Wahl. Wer "Pro-Q ist meine CS" sagt, kriegt Pro-Q als ersten Treffer auch wenn SSL CS 2 daneben sitzt.

### API-Erweiterung (PluginMap.h)

```cpp
// Alle passenden Instanzen in FX-Chain-Order. Empty → kein Match.
std::vector<PluginMatch> lookupPluginsOnTrack(void* track, Domain domain);

// Single-Instance Wrapper — bestehender Aufruf bleibt source-kompatibel.
// Default instanceIdx=0 → behavior unverändert für alle ~25 Callsites.
PluginMatch lookupPluginOnTrack(void* track, Domain domain, int instanceIdx = 0);
```

**Migration:** keine Massen-Edit-Welle. Bestehende Callsites holen `instanceIdx` aus `getActiveInstance(track, domain)` — Default 0 wenn nichts persistiert. Single-Instance-Tracks (≥99% in der Praxis) bleiben unverändert.

### Surface-Anzeige

Channel-Strip-Type-Zone ist **hardware-fixiert auf 4 chars** ([Protocol.cpp:155](extension/src/Protocol.cpp:155), `buildChannelStripType` — SSL-Frame `FF 66 06 17 <strip> <4 chars>`). Plugin-Slot-Name-Zone hat 12 chars und Value-Line hat 19 chars, sind aber für V-Pot-Slot-Label und Param-Wert belegt.

**Anzeige-Logik (Reihenfolge der Versuche):**

1. **`renamed_name` gesetzt** → Rename truncated + uppercase. Holen via `TrackFX_GetNamedConfigParm(tr, fx, "renamed_name", buf, sz)`. User-Verantwortung, lesbare Kurznamen zu vergeben ("VOC ", "PRE ", "SIDE", "PARA").
2. **Plugin-Identitäts-Name** — `TrackFX_GetFXName` getrimmt von "VST3: " / "VST: " / "VSTi: " / "JS: " Prefix, dann 4 chars uppercase. "FabFilter Pro-Q 4" → "FABF", "SSL Native Channel Strip 2" → "SSLN". Macht zwei Instanzen verschiedener Plugins sofort unterscheidbar.
3. **`displayShort`** als letzte Fallback — kuratiert pro PluginMap ("CS 2", "4K B"). Greift wenn FX-Name leer / unleserlich ist.

Reihenfolge gilt unabhängig von Instance-Count. Bei `count == 1` bleibt's effektiv beim selben Bild wie heute (Renames sind selten gesetzt, Plugin-Name truncated wirkt für Built-ins fast wie displayShort), aber Multi-Instance-Tracks profitieren sofort.

**UC1 7-Segment**: bleibt Zahlen-only ("2"). Multi-Instance ist auf der UC1-Anzeige nicht visuell unterscheidbar — Plugin-Mixer-Window übernimmt diese Aufgabe.

**Plugin-Mixer-Window (ImGui, beliebiger Platz)**: zeigt voller Rename-Name + Domain-Tag + Instanz-Index immer wenn count > 1. "Vocal Channel — CS 2 [2 of 3]" o.ä.

**Edge-Cases:**
- Rename mit Sonderzeichen / Umlauten → ASCII-uppercase filtern (Display kann nur ASCII).
- Rename leer / nur whitespace → behandelt als nicht gesetzt → fallback auf `displayShort`.

### Navigation — Builtins

```
next_instance     param 0 = CS, 1 = BC   "Next plugin instance (focused track)"
prev_instance     param 0 = CS, 1 = BC   "Previous plugin instance"
instance_select   param 1..8             "Select plugin instance N (focused track)"
```

`instance_select` mit param ist analog zu `selset_recall` — ein Builtin, Slot-Spinner im Bindings-Editor. Slot 1..N je nach was auf dem Track ist.

### Navigation — bestehende Encoder erweitern

Vom User vorgeschlagen:

**UF8 Channel-Encoder** (heute: Bank-Scroll bei Plain-Drehung):
- **Plain**: Track-zu-Track wie heute.
- **Shift+Drehung**: durch Instanzen des focused Tracks iterieren — kein Bank-Wechsel.

**UC1 Encoder1** (heute: Channel-Auswahl):
- Phase 1: `next_instance`/`prev_instance` als bindbare Aktionen — User mappt selber.
- Phase 2: optional — Encoder1-Drehung scrollt durch Tracks **inkl. Instanzen-Sub-Steps**: track1, track2.a, track2.b, track2.c, track3 …
  Macht das eine kontinuierliche List-View aus dem was der User sieht. Schöner UX, aber mehr Code-Aufwand und nicht reversibel ohne Mode-Switch. **Default off** hinter einem Settings-Toggle.

### Edge Cases

- **Track hat 0 Instanzen einer Domain** — `lookupPluginsOnTrack` returnt `{}`. Active-Instance bleibt 0 (no-op). Surface zeigt leeren CS-Type.
- **Track hat N Instanzen, Active-Instance ist 5, dann werden 2 gelöscht** — beim Render clamp auf `min(active, count-1)`. ExtState korrigieren.
- **Plugin reordering im FX-Chain** — Active-Instance index ist Position-basiert, nicht GUID-basiert (REAPER FX hat keine GUIDs für Plugin-Slots). Reorder verändert die Zuordnung. Dokumentiert, kein Fix — User muss neu wählen.

---

## Reihenfolge / Phasen

**Phase 2.5d-A — FX Learn (alleine baubar):**
1. UserPluginCatalog struct + JSON I/O.
2. `lookupPluginMapByName` Two-Stage-Lookup.
3. Learn-UI im Plugin-Mixer-Window — Software-only erste Version.
4. `fx_learn` Builtin-Handler füllt Catalog (heute Stub).

**Phase 2.5d-B — Multi-Instance (Folge):**
1. `lookupPluginsOnTrack` API + Wrapper für Single-Instance.
2. Active-Instance State + per-track ExtState.
3. Display-Suffix Logic in `displayShort`-Render.
4. `next_instance` / `prev_instance` / `instance_select` Builtins.
5. Shift+Channel-Encoder Verhalten.
6. Optional: UC1 Encoder1 Sub-Step-Mode hinter Settings-Toggle.

A und B sind unabhängig — A kann ohne B leben, B kann ohne A leben. Aber zusammen geben sie das volle Bild: man lernt einen Drittanbieter-Plugin, hat ihn als CS auf zwei Channel-Slots des Tracks, schaltet zwischen Instanz a und b mit Shift+Encoder.

## Offene Fragen

- **`displayShort` für UserPluginMap auto-vorgeben?** "FabFilter Pro-Q 4" → "FFP4"? Heuristik: Initialen + letzte Zahl. Oder einfach User entscheidet, default = erste 4 Chars.
- **Learn auch für SlotIds::TrackPhase / PluginAB / PluginHQ?** Das sind synthetische Slots ohne VST3-Param. Nein — User-Map hat nur echte VST3-Params; synthetische Slots gibt's nur auf den eingebauten Plugins.
- **Welche Soft-Key-Bank zeigt User-Plugins?** Bei einem User-Plugin gemappt als CS-Domain landet's im normalen CS-Bank-Layout — die Slots die er gelernt hat sind aktiv, Rest leer. Wie heute bei Plugins die nur 6 von 30 Slots ausfüllen.
