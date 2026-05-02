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
    char        displayShort[5];// "FFP4", "U-EQ" — 4 chars, vom User
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

### Learn-Flow (UI)

1. User aktiviert `fx_learn` (Toggle oder gehalten — Builtin existiert schon, param 0/1 = Momentary/Toggle).
2. Im Plugin-Mixer-Window erscheint ein "Learning…" Banner mit:
   - Plugin-Name (vom focused Track gegrabbt — `GetLastTouchedFX` oder erste FX im Slot wenn keine touched).
   - Domain-Wahl: ChannelStrip / BusComp Radio-Buttons.
   - DisplayShort-Editor: 4 Char Feld.
   - "Slot to map next" Combo: alle 30+ bekannten LinkIdx mit Klartext-Namen ("HF Gain", "Comp Threshold", …).
3. User wackelt einen Param im Plugin-GUI → `GetLastTouchedFX` liefert den `vst3Param`.
4. UI zeigt: "Bind 'HF Gain' to '<plugin name>: param 14'?" → Confirm-Button.
5. Eintrag landet im UserPluginMap, sofort persistiert.
6. Repeat für weitere Slots.
7. "Done" — UserPluginMap ist jetzt aktiv für jeden Track der dieses Plugin hostet.

**Alternative (schneller):** User klickt im Plugin auf einen Knopf, drückt dann auf der UF8 die V-Pot-Position wo's hin soll — die Soft-Key-Bank kennt für CS-Domain die V-Pot-Slot-Belegung. Klassischer "wackel zuerst Hardware, dann Software"-Learn. Ist wie SSL 360 das macht. Erfordert aber dass die UF8 zur Learn-Zeit schon einen Bank zeigt der mit "CS-virtual" populated ist — Henne-Ei. Erste Iteration: Software-only Learn-UI.

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

**Anzeige-Logik:**

1. **Count > 1, User hat `renamed_name` gesetzt** → 4-char-Zone zeigt den Rename truncated + uppercase. User-Verantwortung, lesbare Kurznamen zu vergeben ("VOC ", "PRE ", "SIDE", "PARA"). Holen via `TrackFX_GetNamedConfigParm(tr, fx, "renamed_name", buf, sz)`.
2. **Count > 1, kein Rename** → `displayShort` wie immer ("CS 2"). Keine a/b/c-Verstümmelung. Welche Instanz aktiv ist erkennt der User im Plugin-Mixer-Window oder am Verhalten der V-Pots; navigiert wird mit `next_instance` / `prev_instance`.
3. **Count == 1** → unverändert.

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
