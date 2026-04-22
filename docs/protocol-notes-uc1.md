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

### `FF 13 04 <meter> <level> <flag1> <flag2> <chk>` — VU meter (Channel Strip I/O meters)
Seen in `uc1_13` test-tone capture. Not GR (cap17's old label was wrong).

Hypothesized byte roles (pending a cleaner calibration capture):
- byte 1: meter/bank selector. In `uc1_13` all novel frames had byte1=`0x01`.
- byte 2: level value, varies with signal. Climbs through the full dynamic range of the test tones (−20/−10/−6/0 dBFS).
- byte 3 and byte 4: seen as `01 00` or `01 01`; second bit likely differentiates input vs output meter.

Example pair from `uc1_13`:
```
ff 13 04 01 1a 01 00 34
ff 13 04 01 1a 01 01 35
```
Same level (0x1a), differing only in the last data byte → strongly suggests input/output meter pair with shared level byte.

### `FF 66 <len> <zone> <ascii…> <chk>` — display write (alphanumeric readouts)
UC1's per-knob numeric displays. Zone `0x05` seen in every knob capture, carrying a 22-character fixed-width ASCII string `<label><spaces><value>`:

| Payload (stripped) | Zone | Text |
|--------------------|-----:|------|
| `… 05 "Threshold       12.1dB"` | 0x05 | Threshold readout |
| `… 05 "Ratio           4:1"` | 0x05 | Ratio readout |
| `… 05 "Release         0.3s"` | 0x05 | Release readout |
| `… 05 "Makeup          …"` | 0x05 | Makeup readout |
| `… 05 "Mix             50.5%"` | 0x05 | Mix readout |
| `… 05 "S/C HPF         45.3Hz"` | 0x05 | Sidechain HPF readout |

Zone `0x05` is a **shared 22-char display slot** that swaps label when a different knob is being touched; it is not a per-knob address. `uc1_04`–`uc1_07` show only a single text field being time-multiplexed to whichever knob the user is currently turning.

Zone `0x04` carries a 43-byte frame that is almost entirely zero bytes plus a single `0x61` sentinel — probably a layout / context frame rather than text. `uc1_10` (track-select) surfaced a single novel frame `ff 66 2b 04 … 61 … 62 … 58` which looks like a track-context identity block, TBD.

Short variants:
- `ff 66 03 00 01 00 6a` — three-byte payload sent alongside every display update. Possibly a "repaint pending" flag.
- `ff 66 02 0b 01 74` (seen once in uc1_13) — similar, different zone.

### `FF 5C …` — LED feedback (button state echo)
Seen 2× in `uc1_08` during button capture, correlated with button-press events. Counterpart to the `FF 22` button event in the OUT direction: after UC1 signals a press, SSL 360° echoes a `FF 5C` frame to turn the button LED on or off. Full family decode needs a single-button toggle capture; `uc1_14` is that capture for Ext-SC specifically.

### Init sequence
Full init is in `uc1_01_init_clean.pcapng` — 27944 packets on EP 0x02/0x00 following replug. Extraction into `extension/src/init_sequence_uc1.inc` is pending. Expect ~50–150 vendor frames plus the standard USB enumeration on EP 0x00.

## Events — UC1 → host (EP 0x81 IN)

### Idle heartbeat
2-byte URB payloads `31 00` (empty) and `31 60` (carries a frame). Host polls at 500 Hz.

### `FF 24 02 <knob_id> <delta> <chk>` — knob rotation
Relative encoder event, **6-bit signed delta** (range −32…+31). Positive = CW, negative = CCW. Encoded in the low 6 bits with two's-complement sign.

| `knob_id` | Knob | Source of ID |
|----------:|------|--------------|
| `0x0E` | Ratio | `uc1_05` — display showed "Ratio …" during these events |
| `0x0F` | Makeup | `uc1_07` — display showed "Makeup …" |
| `0x11` | Release | `uc1_06` — display showed "Release …" |
| `0x12` | Threshold | `uc1_04` — display showed "Threshold …" |
| `0x14` | Mix | `uc1_07` — display showed "Mix …" |
| `0x16` | S/C HPF | `uc1_07` — display showed "S/C HPF …" |
| `0x10`? | Attack | Not seen in `uc1_06` (user confirms only Release frames arrived; Attack ID slot still open) |

Stepped knobs (Ratio) use the same `FF 24` event family — the firmware does not distinguish continuous vs discrete knobs on the wire, only the underlying plugin param is stepped.

Example delta pairs observed in `uc1_05` (Ratio steps):
```
ff 24 02 0e 01 35    ← +1 step CW
ff 24 02 0e 3f 73    ← −1 step CCW (0x3F = 6-bit −1)
```

### `FF 22 03 <button_id> 00 <state> <chk>` — button press / release
State byte: `0x01` = press, `0x00` = release. Middle byte (always `0x00` observed) probably reserved for a modifier flag.

Button IDs mapped against user's pressed sequence in `uc1_08`:

| `button_id` | Button (label on hardware) | Section |
|------------:|----------------------------|---------|
| `0x0A` | Bell HF | Channel Strip — EQ |
| `0x0B` | Type (E) | Channel Strip — EQ character |
| `0x0C` | Bus Comp IN | Bus Comp — enable |
| `0x14` | Bell LF | Channel Strip — EQ |
| `0x15` | Fast Attack | Channel Strip — Comp |
| `0x16` | Peak | Channel Strip — Comp |
| `0x17` | Dyn In | Channel Strip — dynamics enable |
| `0x18` | Expand | Channel Strip — Gate |
| `0x19` | Fast Attack (Gate) | Channel Strip — Gate |
| `0x1A` | Polarity | Channel Strip — Input |
| `0x1B` | S/C Listen | Bus Comp — sidechain listen |

**Two buttons in the user's 13-press sequence produced no `FF 22` event**: position 3 (EQ IN) and position 12 (Solo Clear). They may use a different command family, or the capture missed those physical presses. Re-run as a narrow follow-up capture if Rea-Sixty implementation discovers the buttons don't respond.

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

- [x] USB descriptor dump — endpoints, max packet size, interface class/subclass (confirmed EP 0x02 OUT / 0x81 IN bulk; exact max-packet-size still from `uc1_01` control transfers)
- [ ] Init sequence extraction (uc1_01) → `extension/src/init_sequence_uc1.inc`
- [x] Idle heartbeat identification (`FF 1B 01 <counter>`, 4-phase)
- [ ] Plugin-presence frames isolation (uc1_03 — has 315 novel payloads, needs per-transition windowing)
- [x] Physical-knob → event-frame ID map for Bus Comp section (see table above)
- [ ] Channel Strip knob IDs — no capture yet exercises those knobs
- [x] Button ID map for 11 of 13 buttons (EQ IN + Solo Clear outstanding)
- [x] Display (numeric readout) frame format (`FF 66 <len> <zone> <ascii> <chk>`; zone 0x05 = shared 22-char slot)
- [ ] Track-focus retarget frame — partial; `ff 66 2b 04 … 61 … 62 … 58` is the candidate, semantics TBD
- [x] GR bar-graph frame format (`FF 5B 02 <BE-16 dB×10>`)
- [x] VU LED frame format (`FF 13 04 <bank> <level> <01> <in/out>`)
- [ ] External-sidechain indicator frames — `uc1_14` has 33 novel, decode in next pass
- [ ] Attack knob ID confirmation (probably 0x10 by analogy)
- [ ] UF8 vs UC1 GR routing re-verification — needs a both-connected capture with UF8 also driving displays

## Capture index

| Date | File | Summary |
|------|------|---------|
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
