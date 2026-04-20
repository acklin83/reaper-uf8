# Plugin-Mixer Zone Decode — Capture Plan (cap14–cap19)

Goal: reverse-engineer the host → UF8 commands for every LCD zone in
Plug-in Mixer Layer / Channel Strip Mode, so the extension can drive each
zone independently (both for SSL-plugin passthrough and REAPER-only
repurpose).

Known zones (from UF8 User Guide, page 153):

| Zone | Zweck | Cmd bisher |
|---|---|---|
| Top Zone | Soft-Key Label | TBD |
| Plug-in Mixer Position | Slot-Nr ("No") | TBD |
| Channel Strip Type | "CS 2" / "4K B" / "4K E" | TBD |
| DAW Colour | Farbbalken | `FF 66 09 18 …` ✓ |
| TrkNam | Track-Name groß | `FF 66 <n> 0B <strip>` ✓ (via MCU) |
| O/PdB | Fader dB | TBD |
| Input/Output Metering | Pegel links | `FF 38/39 04 …` part-decoded |
| Dynamics Metering | GR-Balken rechts | TBD |
| Currently Selected Parameter | V-Pot-Label | `FF 66 <n> 04 <strip>` ✓ (identifiziert) |
| Value | V-Pot-Wert | `FF 66 09 0E <strip>` ✓ (via MCU) |
| V-Pot Readout Bar | Bogen | TBD |

## Setup (identisch für alle Captures)
- Windows-Box, SSL 360° läuft, REAPER läuft
- REAPER-Projekt: 16 Tracks, je andere REAPER-Farbe, klare Namen
  (T1 "Kick", T2 "Snare" usw., damit im Capture klar ist was wohin geht)
- Wireshark + USBPcap auf dem Bus der UF8
- Für jede Capture: 5 s idle vor der Trigger-Action, 5 s idle danach

## cap14_pm_populated — Baseline mit geladenen SSL Plugins
- SSL Native Channel Strip 2 auf T1, T2, T3 (alle 3 gleiches Plugin)
- Track-Farben deutlich verschieden: T1 rot, T2 grün, T3 blau
- Trigger: nichts — nur idle PM-Mode 10 s
- Zweck: Welche Commands bleiben dauerhaft drin wenn Slots belegt sind?
  Referenz gegen cap13 ("leerer" PM-Mode).

## cap15_pm_param_cycle — "Currently Selected Parameter"
- Gleiches Setup wie cap14
- Trigger: auf UF8 die Soft-Keys durchklicken, die V-Pot-Assignments
  wechseln (INPUT / EQ / DYN / SEND / CHANNEL…). Jeden mindestens
  2 s halten damit der State stabil ist.
- Zweck: Bestätigen `FF 66 <n> 04 <strip>` = Parameter Label.
  Abgrenzen gegen Value-Command.

## cap16_pm_fader_dB — "O/PdB"
- Setup wie cap14
- Trigger: Fader T1 langsam von -∞ über 0 nach +12 dB bewegen, 10 s
- Zweck: Welcher Command zeigt die "O/PdB"-Zahl?

## cap17_pm_gain_reduction — Dynamics Metering
- Setup wie cap14, aber SSL Comp aggressiv einstellen (Threshold low, Ratio high)
- Audio durchspielen lassen ~10 s
- Zweck: GR-Balken-Command? Vermutlich `FF 3A` oder ähnlich (in cap13 kurz gesehen: `FF 3B 03 …`).

## cap18_pm_cs_type — Channel Strip Type
- **Anderes** Setup: T1 = CS 2, T2 = 4K B, T3 = 4K E, T4 = Bus Compressor 2 (falls verfügbar)
- Trigger: nichts, nur 10 s idle
- Zweck: Command der "CS 2" / "4K B" String-Label pro Strip schreibt.

## cap19_pm_bank_position — Plug-in Mixer Position
- Setup wie cap14 erweitert auf 16+ Tracks mit SSL Plugins
- Trigger: Bank → Bank → Bank ← Bank ← (4 Bank-Shifts, je 2 s Pause)
- Zweck: Welcher Command aktualisiert die "Position"-Zahl oben links?

## Nach dem Capture
1. Alle .pcapng hier ablegen als `capXX_<name>.pcapng`
2. Je eine `.md` Sibling mit:
   - Datum/Uhrzeit
   - Exaktem SSL 360°-Setup (Version, Projekt-File-Name)
   - Exaktem Trigger-Ablauf
   - Erste Stichwort-Beobachtung falls schon auffällig
3. Dann Analyse auf Mac: diff gegen cap01_baseline_pluginmixer als Baseline,
   cap13_layer_switch als Second-Baseline.
