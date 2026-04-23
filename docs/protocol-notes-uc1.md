# UC1 Protocol Notes

Living spec of the SSL UC1 USB protocol, reverse-engineered by capturing SSL 360° ↔ UC1 traffic with USBPcap on Windows. Structure mirrors `protocol-notes.md` (UF8) so the two can be diffed side-by-side.

**Status 2026-04-22:** first decode pass done. 14 captures analyzed with `analysis/parse_usbpcap_uc1.py` diffing against `uc1_02_idle_baseline`. Frame family, checksum, endpoint directions, GR encoding, VU encoding, knob IDs, button IDs, and display-text format are all concrete. Per-knob→VST3-param mapping, init replay, and UF8-vs-UC1 GR routing re-verification remain open.

## Device

- VID `0x31E9` / PID `0x0023`  (UF8 is `0x0021`)
- USB 2.0 Full Speed, vendor-specific class `0xFF/0xFF/0xFF`
- Bulk endpoints confirmed from capture:
  - **EP 0x81 IN** — events from UC1 (knob turns, button press/release)
  - **EP 0x02 OUT** — writes to UC1 (displays, LEDs, meters, init)
  - EP 0x00 IN/OUT (control) used only during enumeration
- Exclusive claim: `SSL360Core` when running — cannot co-open. Captures tapped at the XHC layer via USBPcap rather than through a userland libusb claim.

## Capture constraints

**UF8 must be disconnected during UC1 captures.** Rationale: clean single-device bus. With both controllers connected we'd have to disentangle two overlapping SSL-VID streams per capture. `cap17_pm_gain_reduction.md` saw `FF 13 04 …` frames going only to UC1 when both were present, but UF8 *does* display GR in normal operation, so the old "GR routes exclusively to UC1" reading was wrong — the routing picture is still an open question, to be closed by a dedicated both-connected reference capture. Additionally, the command `FF 13 04 …` on UC1 turns out to be **VU meter data** (see below), not GR — `FF 5B` is the GR command. The cap17 label was a misidentification.

## Frame format — confirmed

```
FF <cmd> <len> <data×len> <chk>
```

- Every host↔device frame starts with `0xFF`.
- Checksum = `sum(cmd + len + data_bytes) mod 256`. Verified on GR, knob-event, button-event, and display-write frames.
- IN direction (UC1 → host, EP 0x81) frames on the wire are prefixed with a 2-byte USB poll token `31 XX` where `XX = 0x00` (empty poll) or `XX = 0x60` (carries event). When `XX = 0x60`, the subsequent bytes are the `FF …` event frame.
- OUT direction (host → UC1, EP 0x02) frames are sent as the raw `FF …` packet with no poll wrapper.

## Commands — host → UC1 (EP 0x02 OUT)

### `FF 1B 01 <counter> <chk>` — idle keepalive
Seen in every baseline capture, 4 distinct payloads cycling through counter 0x00, 0x01, 0x02, 0x03. Sent at ~1 Hz. Likely a watchdog token; SSL 360° pings with the rotating counter and UC1 stays alive. **Rea-Sixty must replicate** or UC1 will time out.

Examples:
```
ff 1b 01 00 1c
ff 1b 01 01 1d
ff 1b 01 02 1e
ff 1b 01 03 1f
```

### `FF 5B 02 <hi> <lo> <chk>` — Gain Reduction meter (UC1 Bus Comp center section)
16-bit big-endian value in **units of 1/10 dB**.

| Payload | Decoded |
|---------|---------|
| `ff 5b 02 00 00 5d` | 0.0 dB (idle, no reduction) |
| `ff 5b 02 00 79 d6` | 12.1 dB (matches user-reported "~12 dB" in uc1_11) |
| `ff 5b 02 00 98 f5` | 15.2 dB (peak seen in uc1_12 dynamic material) |

`uc1_12` contained 100 distinct values from `0x0026` (3.8 dB) to `0x0098` (15.2 dB). Encoding is monotonic.

### `FF 13 04 01 <level> 01 <in/out> <chk>` — VU meter (Channel Strip I/O meters)
Special case of the general `FF 13 04` LED-cell-write command where **bank byte is `0x01`** — that bank is reserved for the VU level strips. Byte 4 selects input vs output meter (`0x00` / `0x01`).

Example pair from `uc1_13`:
```
ff 13 04 01 1a 01 00 34
ff 13 04 01 1a 01 01 35
```
Same level (0x1a), differing only in the last data byte → input and output meters sharing the same level in that moment.

### `FF 66 <len> <zone> <ascii…> <chk>` — display write (alphanumeric + context)
UC1 has multiple display zones addressed by the byte immediately after length. Each zone has its own fixed field width. Known zones from the capture set:

| Zone | Width | Purpose | Example text seen |
|-----:|------:|---------|-------------------|
| `0x02` | 37 B | Channel-strip context line with `a`/`b` byte markers at specific columns (likely track position / strip layout) | `"············a···········b···········"`, `"············-------·····b···········"` |
| `0x04` | 43 B | General status / "No Plug-ins" line | `"··············No Plug-ins·················"`, all-zero with a `0x61` sentinel when populated |
| `0x05` | 22 B | **Shared numeric readout** (time-multiplexed to the knob currently being turned) | `"Threshold       12.1dB"`, `"Ratio           4:1"`, `"Release         0.3s"`, `"Makeup          …"`, `"Mix             50.5%"`, `"S/C HPF         45.3Hz"` |
| `0x03` | 18–19 B | Status button label + on/off text | `"S/C Listen      On"`, `"S/C Listen      Off"` |
| `0x0E` | 3–4 B | Plugin state tag | `"Off"`, `"N/A"`, (presumably `"On"`) |
| `0x10` | 4–5 B | Plugin name tag | `"4K E"` (4K E Channel Strip detected — UC1 adapts to multiple SSL plugin variants, not just Native 2) |

Short `FF 66` variants seen alongside big updates:
- `ff 66 03 00 01 00 6a` — three-byte payload; likely a "repaint pending" / invalidate flag.
- `ff 66 02 0b 01 74` — similar short form, different zone.
- `ff 66 01 03 6a` — single-byte payload, zone 0x03 invalidate.

Zone 0x05 time-multiplexing means Rea-Sixty must maintain state for the currently-active knob (the one most recently turned) and push the 22-char string each time the value changes. Other zones are mostly written on plugin load/unload and track change.

### `FF 13 04 <bank> <cell> 01 <state> <chk>` — per-button LED feedback (and more)
**This is the main LED-update mechanism, not `FF 5C`.** Every button press is followed within ~10 ms by one `FF 13 04` frame with a unique `(bank, cell)` address per button. State byte encodes LED brightness:

- `0x00` — LED fully off
- `0x33` — LED **dimmed** (brightness reduced but still visible — the plugin-bypassed visual state, so the user still sees which buttons are "armed" even though the plugin isn't processing)
- `0xFF` — LED fully on

Per user feedback: when Channel IN is pressed to bypass the Channel Strip, the feature LEDs don't go dark — they dim. `0x33` is explicitly a dim level, not an off state. Rea-Sixty must preserve this: bypass-state rendering uses `0x33` across the section, plugin-active uses `0xFF` on the lit cells and `0x00` only where the feature is truly inactive.

Bank → Cell → Button map (derived from `uc1_21` direct evidence):

| ID | Button | Bank | Cell |
|----|--------|-----:|-----:|
| `0x08` | HF Bell | `0x02` | `0x89` (from `uc1_22` — 10× presses with CS 2 loaded) |
| `0x09` | EQ Type | `0x02` | `0x51` |
| `0x0A` | EQ In | `0x02` | `0x50` |
| `0x0B` | LF Bell | `0x02` | `0x23` |
| `0x0C` | Bus Comp IN | `0x02` | `0x01` |
| `0x14` | Fast Attack (Comp) | `0x02` | `0x38` |
| `0x15` | Peak | `0x02` | `0x39` |
| `0x16` | Dyn In | `0x02` | `0x5B` |
| `0x17` | Expand | `0x02` | `0x92` |
| `0x18` | Fast Attack (Gate) | `0x02` | `0x93` |
| `0x19` | Polarity | `0x02` | `0x98` |
| `0x1A` | S/C Listen | `0x02` | `0x99` |
| `0x1B` | Solo Clear | `0x01` | `0x9A` (state cycles 01 → 03 → 00 on clear) |
| `0x1C` | Solo | `0x02` | `0x97` |
| `0x1D` | Cut | `0x02` | `0x96` |
| `0x1E` | Channel IN | `0x02` | `0x94` |
| `0x1F` | Fine | `0x02` | `0x95` |

Plugin-bypass additionally fires a burst of `FF 13 04` frames for every feature-LED in the section. **LEDs are dimmed (`0x33`), not switched off** — the user still sees the armed button states visually, just at reduced brightness. Rea-Sixty must mirror this: bypass-state = re-push every cell at `0x33`, un-bypass = re-push each cell at whatever value was active before.

Same command family carries the VU meter frames (`ff 13 04 01 <level> 01 <in/out>`) — bank `0x01` for the I/O VU strips, bank `0x02` for button LEDs. So `FF 13 04` is the generic "LED cell write" command, one address space for everything lit on the device.

### `FF 5C 02 <hi> <lo> <chk>` — small global status register
Not per-button LED data. Only 3 distinct values seen across all captures (`00 00`, `00 0A`, `00 32`) and it updates rarely — probably a global status bitmask (Bus-Comp-active, Solo-active, etc.). Low-priority for the first Rea-Sixty release; individual LED pushes go via `FF 13 04`.

### Init sequence (trivial)
**UC1 needs no vendor-specific init.** Analysis of `uc1_01` (2846 OUT frames to the newly enumerated device) found only 5 distinct payloads in the whole post-replug stream, all of which already exist in the idle-baseline set:

```
ff 5b 02 00 00 5d       (GR = 0.0 dB)
ff 1b 01 00 1c          (keepalive counter 0)
ff 1b 01 01 1d          (keepalive counter 1)
ff 1b 01 02 1e          (keepalive counter 2)
ff 1b 01 03 1f          (keepalive counter 3)
```

Control-transfer side (EP 0x00) shows only the stock USB enumeration (GET_DESCRIPTOR, SET_CONFIGURATION). Rea-Sixty can open the bulk endpoints, immediately start the keepalive + zero-GR stream, and everything else comes on demand. No separate `init_sequence_uc1.inc` file is needed — replicate the 5 payloads above in code.

## Events — UC1 → host (EP 0x81 IN)

### Idle heartbeat
2-byte URB payloads `31 00` (empty) and `31 60` (carries a frame). Host polls at 500 Hz.

### `FF 24 02 <knob_id> <delta> <chk>` — knob rotation
Relative encoder event, **6-bit signed delta** (range −32…+31). Positive = CW, negative = CCW. Encoded in the low 6 bits with two's-complement sign.

| `knob_id` | Knob | Source of ID |
|----------:|------|--------------|
| `0x0E` | Ratio (Bus Comp) | `uc1_05` — display showed "Ratio …" (zone 0x05) |
| `0x0F` | Makeup (Bus Comp) | `uc1_07` — display showed "Makeup …" (zone 0x05) |
| `0x11` | Release (Bus Comp) | `uc1_06` — display showed "Release …" (zone 0x05) |
| `0x12` | Threshold (Bus Comp) | `uc1_04` — display showed "Threshold …" (zone 0x05) |
| `0x14` | Mix (Bus Comp) | `uc1_07` — display showed "Mix …" (zone 0x05) |
| `0x16` | S/C HPF (Bus Comp) | `uc1_07` — display showed "S/C HPF …" (zone 0x05) |
| `0x10`? | Attack (Bus Comp) | Not seen in `uc1_06` (Attack events didn't fire; ID still open) |

**The top-center V-Pot IDs `0x0C–0x16` repurpose based on the focused plugin.** `uc1_15` showed the same IDs driving Channel Strip params when Bus Comp 2 was absent and 4K E was loaded: `0x0C = Input Trim`, `0x16 = Fader Level / Output Trim`. So these 7 IDs are **soft-mapped** by SSL 360° depending on what's on the track.

**Dedicated Channel Strip pots — always drive CS params** (from `uc1_15`, display text in zone 0x03):

| `knob_id` | CS knob |
|----------:|---------|
| `0x00` | Low Pass (LPF freq) |
| `0x01` | High Pass (HPF freq) |
| `0x02` | HF Gain |
| `0x03` | HF Frequency |
| `0x04` | HMF Gain |
| `0x05` | HMF Frequency |
| `0x06` | HMF Q |
| `0x07` | LMF Gain |
| `0x08` | LMF Frequency |
| `0x09` | LMF Q |
| `0x0A` | LF Frequency |
| `0x0B` | LF Gain |
| `0x17` | Gate Release |
| `0x18` | Gate Hold — confirmed with SSL Native Channel Strip 2 (`uc1_18`); 4K E has no Gate Hold param |
| `0x19` | Gate Threshold |
| `0x1A` | Gate Range |
| `0x1B` | Dyn Comp Release |
| `0x1C` | Dyn Comp Threshold |
| `0x1D` | Dyn Comp Ratio |

Stepped knobs (Ratio) use the same `FF 24` event family — the firmware does not distinguish continuous vs discrete knobs on the wire, only the underlying plugin param is stepped.

### Display zones per section

- **Zone 0x05** (22-char ASCII) — Bus Comp readout, used when one of the top V-Pots is turned and Bus Comp 2 is the governing plugin
- **Zone 0x03** (22-char ASCII) — Channel Strip readout, used when one of the CS pots (or a repurposed V-Pot driving a CS param) is turned

Implementation: Rea-Sixty maintains a per-focused-track map of "which plugin owns the V-Pots right now?" and picks the target zone accordingly when pushing knob-value text.

Example delta pairs observed in `uc1_05` (Ratio steps):
```
ff 24 02 0e 01 35    ← +1 step CW
ff 24 02 0e 3f 73    ← −1 step CCW (0x3F = 6-bit −1)
```

### `FF 22 03 <button_id> 00 <state> <chk>` — button press / release
State byte: `0x01` = press, `0x00` = release. Middle byte (always `0x00` observed) probably reserved for a modifier flag.

Button IDs mapped against user's pressed sequence in `uc1_08`:

**Complete button-ID map (4K E context, direct zone 0x03 display text):**

| ID | Button | Display text |
|----|--------|--------------|
| `0x08` | HF Bell | `"HF Bell         In"` |
| `0x09` | EQ Type | `"EQ Colour       Orange"` (4K E) |
| `0x0A` | EQ In | `"EQ              Out/In"` |
| `0x0B` | LF Bell | `"LF Bell         In"` |
| `0x14` | Fast Attack (Comp) | `"Fast Attack     In"` |
| `0x15` | Peak | (no display text — button fires silently) |
| `0x16` | Dyn In | `"DYN             Out/In"` |
| `0x17` | Expand | `"Expander        In"` |
| `0x18` | Fast Attack (Gate) | `"Fast Attack     In"` (same label string as Comp's, different ID) |
| `0x19` | Polarity | `"Ø               In"` |
| `0x1A` | S/C Listen | `"S/C Listen      On/Off"` |
| `0x1B` | Solo Clear | `"Solo Clear      Off"` |
| `0x1C` | Solo | `"Solo            On"` |
| `0x1D` | Cut | `"Cut             On"` |
| `0x1F` | Fine | `"Fine            On"` |
| `0x1E` | Channel IN | `"Channel Strip   Out"` |

Plus `0x0C` = **Bus Comp IN** (from `uc1_08` positional evidence, direct-display-evidence still pending a targeted Bus-Comp-section capture).

All 16 Channel-Strip-area buttons + 1 Bus Comp button cover the UC1's full button inventory. Earlier apparent "Polarity doesn't emit events" was a physical-contact issue on the user side, not a firmware peculiarity — `uc1_19` confirms every button fires reliably when firmly pressed.

**Plugin context (tested with `uc1_20`, CS 2 loaded):** Button IDs are **stable** across 4K E and CS 2 — no per-plugin remapping, unlike the top V-Pots.

What depends on plugin / hardware-contact:
- **Display feedback per button varies**: Peak (`0x15`) shows `"Peak            In"` with CS 2 but no text with 4K E, matching user's note "Peak nur von CS2 verwendet". EQ Type's label varies per plugin (Black/Orange/Brown for 4K E, In/Out for CS 2, etc.).
- **Display text content obviously depends on plugin context** (the `In`/`Out` state of the toggled param).
- **HF Bell (`0x08`) and EQ Type (`0x09`) didn't emit events in `uc1_20`** despite CS 2 fully supporting both (EQ Type, HF Bell, LF Bell all present on CS 2 per user). This is the same physical-contact issue observed earlier with Polarity in `uc1_17/18` — the user pressed firmly for most buttons but certain physical buttons on the UC1 need more positive engagement. Firmware isn't gating these; user hardware contact was inconsistent. Not a protocol-level phenomenon.

Rea-Sixty can ship a single stable button-ID table and forward each press to the focused plugin — buttons that the plugin doesn't implement simply no-op.

### Track-selection follow (host-driven)
UC1 does not send a "track changed" event — track focus is driven by SSL 360° observing the DAW. `uc1_10` confirmed this: in the focus walk 1→2→3→4→1 the only novel payload was one OUT frame (`ff 66 2b 04 …`), no novel IN frames. Rea-Sixty's `FocusedTrack` must therefore push the retarget frame itself when REAPER's `SetTrackSelected` fires.

## Plugin detection / mapping

The UC1 is physically laid out to match SSL Native Bus Compressor 2 + Channel Strip 2. `PluginMap.cpp` already has slot tables (`kBusComp2Slots`); for UC1 we add a parallel table mapping each physical UC1 knob ID to a `LinkSlot`. Bus Comp section, concrete from the decode pass:

| UC1 knob | `knob_id` | Target VST3 param on Bus Comp 2 |
|----------|----------:|----------------------------------|
| Threshold | 0x12 | 2 |
| Makeup | 0x0F | 3 |
| Attack | 0x10? (unverified) | 4 |
| Release | 0x11 | 5 |
| Ratio | 0x0E | 6 |
| Sidechain HPF | 0x16 | 7 |
| Mix | 0x14 | 8 |

Channel Strip section knob IDs still TBD — `uc1_04`–`uc1_07` only exercised Bus Comp controls. A follow-up capture cycling the EQ and Dyn knobs is needed.

## GR data source — not from the plugin

The SSL plugins ship GR to 360° over encrypted Thrift IPC (see `plugin-ipc-notes.md`). We do **not** read that channel. GR is computed by a bundled JSFX envelope-follower (`extension/jsfx/rea_sixty_gr_probe.jsfx`) inserted next to the SSL compressor, and its value is read via `TrackFX_GetParam`. With the `FF 5B 02 …` encoding now concrete, wiring the JSFX output into `rea_sixty_gr_probe_send_dB()` → `FF 5B 02 <dB×10 BE-16> <chk>` is a straight translation.

## Open items

- [x] USB descriptor dump — endpoints, max packet size, interface class/subclass (confirmed EP 0x02 OUT / 0x81 IN bulk)
- [x] Init sequence — trivial; 5 reusable payloads (keepalive + zero-GR), no custom sequence file needed
- [x] Idle heartbeat identification (`FF 1B 01 <counter>`, 4-phase)
- [~] Plugin-presence frames — `FF 66` writes to zones 0x04/0x0E/0x10 land the plugin-name/state, `FF 5C` flips the LED mask. Full cross-plugin list requires more captures (4K B, SSL 360 Link, Native Channel Strip 2, etc.)
- [x] Physical-knob → event-frame ID map for Bus Comp section (see table above)
- [ ] Channel Strip knob IDs — no capture yet exercises those knobs
- [x] Button ID map — 11 of 13 confirmed (EQ IN + Solo Clear outstanding; `0x1A`/`0x1B` corrected via `uc1_14` direct evidence)
- [x] Display frame format — zones 0x02/0x03/0x04/0x05/0x0E/0x10 documented
- [~] Track-focus retarget frame — `FF 66 2B 04 …` with byte markers `0x61`/`0x62` at varying column positions; semantics (which byte = track index) still TBD
- [x] GR bar-graph frame format (`FF 5B 02 <BE-16 dB×10>`)
- [x] VU meter frame format (`FF 13 04 <bank> <level> <01> <in/out>`)
- [~] LED bitmask frame (`FF 5C 02 <16-bit mask>`) — bit→button mapping needs a single-button toggle capture per button
- [ ] Attack knob ID confirmation (probably 0x10 by analogy)
- [ ] UF8 vs UC1 GR routing re-verification — needs a both-connected capture

## Capture index

| Date | File | Summary |
|------|------|---------|
| 2026-04-23 | `uc1_15_knob_channelstrip_sweep.pcapng` | 4K E loaded, 20 CS pots swept sequentially (180 s, 209 802 pkts) — full CS knob-ID table via zone 0x03 display text |
| 2026-04-23 | `uc1_16_missing_buttons.pcapng` | User pressed EQ IN + Solo Clear each 5× (15 s, 16 990 pkts) — only `0x1B` fired; later proven to be Solo Clear (not EQ IN), user likely pressed wrong button |
| 2026-04-23 | `uc1_17_polarity_soloclear.pcapng` | Polarity + Solo + Solo Clear (20 s, 22 500 pkts) — `0x1C = Solo`, `0x1B = Solo Clear` via zone 0x03 display text; Polarity no events |
| 2026-04-23 | `uc1_18_polarity_eqin_hold.pcapng` | Polarity + EQ IN + Gate Hold (with CS 2 loaded) (20 s, 23 888 pkts) — `0x0A = EQ IN`, `0x18 = Gate Hold` (CS 2 param); Polarity no events (physical contact issue, not firmware) |
| 2026-04-23 | `uc1_19_buttons_4ke.pcapng` | All 16 Channel-Strip-area buttons, one press each, 4K E loaded (60 s, 67 384 pkts) — full button-ID map via zone 0x03 display text |
| 2026-04-23 | `uc1_20_buttons_cs2.pcapng` | Same 16-button sequence with SSL Native Channel Strip 2 (60 s, 67 372 pkts) — IDs confirmed stable across plugins; HF Bell + EQ Type gated off by CS 2 |
| 2026-04-23 | `uc1_21_led_buscomp.pcapng` | 17 buttons incl. Bus Comp IN, Bus Comp 2 loaded (75 s, 84 276 pkts) — Bus Comp IN = 0x0C confirmed; full `FF 13 04` per-button LED cell map derived; HF Bell silent with Bus Comp 2 |
| 2026-04-23 | `uc1_22_bells_cs2.pcapng` | HF Bell + LF Bell 10× each, CS 2 (30 s, 33 752 pkts) — HF Bell LED cell = 0x89; overturns uc1_20 "HF Bell silent with CS 2" (was contact issue) |
| 2026-04-22 | `uc1_01_init_clean.pcapng` | Init/wakeup sequence on fresh enumeration — 27944 pkts to address 28, endpoints 0x00/0x80/0x02/0x81 |
| 2026-04-22 | `uc1_02_idle_baseline.pcapng` | 10 s idle heartbeat — 11288 pkts, ~1130 pkt/s, same endpoint set. Baseline input for every diff. |
| 2026-04-22 | `uc1_03_plugin_presence.pcapng` | Plugin load/unload transitions (30 s, 34298 pkts, 315 novel): empty → +BusComp2 → +ChStrip2 → −BusComp2 → −ChStrip2 |
| 2026-04-22 | `uc1_04_knob_threshold_sweep.pcapng` | Threshold full sweep — id=0x12, 592 novel frames |
| 2026-04-22 | `uc1_05_knob_ratio_steps.pcapng` | Ratio discrete steps — id=0x0E, 160 novel frames |
| 2026-04-22 | `uc1_06_knob_attack_release.pcapng` | Release sweep only (Attack events not seen) — id=0x11, 174 novel |
| 2026-04-22 | `uc1_07_knob_makeup_mix.pcapng` | Makeup (0x0F) + Mix (0x14) + S/C HPF (0x16), 927 novel |
| 2026-04-22 | `uc1_08_buttons_all.pcapng` | 13 buttons (11 with `FF 22` events captured, 2 gaps) — 246 novel |
| 2026-04-22 | `uc1_09_display_params.pcapng` | GUI-side Bus Comp 2 param changes — 321 novel, all OUT display-write |
| 2026-04-22 | `uc1_10_track_select.pcapng` | Track focus walk — single novel OUT frame `ff 66 2b 04 …` repeating the identity block |
| 2026-04-22 | `uc1_11_gr_static.pcapng` | GR pinned ~12 dB steady — 505 novel, all identical `ff 5b 02 00 79 d6` |
| 2026-04-22 | `uc1_12_gr_dynamic.pcapng` | GR animated full range — 507 novel, 100 distinct 16-bit values `0x0026`..`0x0098` (3.8–15.2 dB) |
| 2026-04-22 | `uc1_13_vu_meters.pcapng` | Test tones −20/−10/−6/0 dBFS — 245 novel OUT, `FF 13 04` VU family |
| 2026-04-22 | `uc1_14_multiple_sc.pcapng` | Ext-SC button toggled — 8 novel IN + 25 novel OUT (LED feedback family) |
