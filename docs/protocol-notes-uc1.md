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
| 2026-04-23 | `uc1_23_ssl360_startup.pcapng` | SSL 360° cold-start handshake — 5 init frames (FF 01/02/05/4B/4E) + 1394-frame LED-init flood |
| 2026-04-23 | `uc1_24_trackname.pcapng` | Bus Comp IN toggle on track TESTBUS (20 s) — button events only, no track-name frames |
| 2026-04-23 | `uc1_24b_trackname_reload.pcapng` | BC 2 remove/re-add on TESTBUS (25 s, 28 250 pkts) — BC track name = zone 0x04 pos 14 |
| 2026-04-23 | `uc1_25_cs_trackname.pcapng` | CS 2 remove/re-add on TESTCS (25 s, 29 290 pkts) — CS track name = zone 0x02 pos 12 |
| 2026-04-23 | `uc1_26_7seg_scroll.pcapng` | CHANNEL-encoder scroll through 16 CS 2 instances (30 s, 34 056 pkts) — 7-seg LEDs at bank 0x01 cells, 22 unique FF 13 04 frames, per-segment addressing still TBD |

## Central-panel 7-segment display (TBD)

The 3-digit red 7-segment display left of the CHANNEL encoder is
driven by `FF 13 04 01 <cell> 00 <state>` frames — same bank (0x01)
as the VU meters but with different cell addresses and `byte3=0x00`
instead of the VU `byte3=0x01`.

uc1_26 (16 CS instances, scrolled through) showed 22 unique cell
addresses around 0x08..0x16 getting toggled between state 0x00 and
0x01 as positions changed. This is per-segment addressing — each
cell corresponds to one LED segment of one digit.

Decoding map requires a targeted capture that shows individual digits
in isolation (e.g. force-display "1", then "2", then "8", log which
cells change). Defer until a specific need arises — track-select
feature works without the 7-seg readout.

**Structure from uc1_27 (110-position scroll)** — 18 cells involved,
grouped by activity frequency (higher freq = digit that cycles more
often during scroll):

| Bank | Cells | Frequency | Likely digit |
|------|-------|-----------|--------------|
| 0x01 | 0x10..0x16 (7 cells) | 40..160 events | Ones digit |
| 0x01 | 0x08..0x0E (7 cells) | 4..16 events | Tens digit |
| 0x01 | 0x00,0x03..0x05 (4 of 7 cells) | 2 events each | Hundreds digit (only "1"-forming segments lit during 99→100 transition) |

Per-segment mapping within each digit is not decoded — would need
timeline alignment of FF 13 04 frames with CHANNEL-encoder events to
correlate position-at-time with cell-state. `uc1_27_7seg_decode.pcapng`
captures this data; decode is a later follow-up.
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

## Decode pass 2026-04-24 — brightness, track colour, VU, GR, pot LED rings

### LED + LCD brightness (`FF 14 02` + `FF 4F 02` + `FF 5C 02`)

SSL 360° brightness slider → three coupled frames per step. `FF 4F 02` identical to UF8 (shared LCD-backlight encoding).

| Step | `FF 14 02 <b> 00` | `FF 4F 02 <b> 00` | `FF 5C 02 00 <b>` |
|------|-------------------|-------------------|--------------------|
| dark   | `0x0A` | `0x18` | `0x08` |
| dim    | `0x13` | `0x30` | `0x0F` |
| half   | `0x20` | `0x50` | `0x19` |
| bright | `0x26` | `0x60` | `0x1E` |
| full   | `0x40` | `0xA0` | `0x32` |

`FF 14 02` ≈ 2× `FF 2D 08` (UF8's LED brightness). Likely maps to UC1's denser LED surface.
`FF 5C 02` role unclear — possibly Bus Comp GR meter baseline / status LEDs.

### Focused-track colour (`FF 66 02 11 <palette_idx>`)

The top bar on the UC1 LCD mirrors the focused track's colour. Single-byte palette index (range 0x01..0x0C observed); values match the UF8 palette-broadcast at the currently-focused strip's position. Fires on REAPER track-colour change AND on focus change.

### Colour-bar enable flag (`FF 66 03 00 01 <flag>`)

7-byte frame. `flag = 0x00` → central display shows "MAIN" (no SSL plugin on focused track). `flag = 0x01` → colour bar and plugin context active.

### Central plugin label (`FF 66 05 01 <4 ASCII>`)

9-byte frame carrying a 4-character label. Observed values: `"MAIN"` (no plugin), `"CS 2"` (SSL Native Channel Strip 2). Analog to UF8's `FF 66 06 17` Channel Strip Type zone.

### Track-name carousel (`FF 66 25 02` and `FF 66 2B 04`)

Two related frame families each containing a **3-slot track-name triple**:

- `FF 66 25 02 <36 bytes> <chk>` — 41 bytes total. 3 × 12-byte slots.
- `FF 66 2B 04 <42 bytes> <chk>` — 47 bytes total. 3 × 14-byte slots.

Each slot is a zero-padded ASCII string (track name, e.g. `"1"`, `"TESTCS"`). Layout appears to be `[prev-track | current-track | next-track]` but alignment varies (leading zeros sometimes present). The smaller 12-byte slot corresponds to one UC1 display zone, the 14-byte slot to another.

### GR meter — Channel Strip dynamics (`FF 13 04 01 <cell> 01 <state>`)

UC1 has 5 dedicated GR LEDs on cells **0x5C..0x60** (bank 0x01). Each LED has 5 visible brightness states observed (plus off):

| state | level |
|-------|-------|
| `0x00` | off |
| `0x01`..`0x03` | threshold-entering flicker |
| `0x19` | step 1 |
| `0x2D` | step 2 |
| `0x54` | step 3 |
| `0x99` | step 4 |
| `0xFF` | step 5 (full) |

User's hypothesis that UF8's GR byte drives UC1 LEDs simultaneously confirmed via `dual_35`: both devices receive synchronized GR information, encoded differently (UF8 = single byte, UC1 = per-LED brightness).

### VU meter LEDs — bank 0x02

Input + Output VU strips on UC1 use bank `0x02` LED cells (different from GR which uses bank 0x01). Cells observed during `dual_36_cs_vu_ramp`:

- Contiguous block `0x71..0x7C` (12 cells) — main VU segments
- `0x3A..0x3E` (5 cells) — likely peak hold or second row
- Multi-state cells `0x5B, 0x5C, 0x5D` with 5 brightness levels (same encoding as GR LEDs)
- Isolated cells `0x45, 0x50, 0x66, 0x87` — dB-level markers or peak dots

Exact cell-to-LED-position mapping and Input-vs-Output split needs offline alignment with the ramp timeline (deferred).

### Pot LED rings — encoding (`FF 13 04 <bank> <cell> 00 <state>`)

Each UC1 pot has an LED ring around it. Two-bank encoding:

- **bank 0x01, state 0x00 / 0x01** — selection bitmap: which segments are lit (part of the arc)
- **bank 0x02, state 0x00 / 0xFF** — brightness: per-segment on/off at the selected brightness

Both banks use role byte `0x00` (not `0x01` which is the LED-mode used elsewhere). Same cell address is written to both banks on every pot tick.

### Pot → cell clusters

Per-pot cell maps derived by correlating EP 0x81 IN knob events (`FF 24 02 <id> <delta>`) with EP 0x02 OUT LED writes and bucketing each LED event into the knob whose event window contains it (midpoint-split between consecutive knob-id transitions). The primary source is `uc1_15_knob_channelstrip_sweep.pcapng` — a comprehensive single-session capture covering all 22 CS+Dyn pots with full sweeps. BC pots come from `dual_40_bc_pots.pcapng` plus `uc1_04..07` (per-section captures).

All UC1 pot rings render in firmware as a **single moving LED dot** (Position-encoded — the "Kerbe" of the analog knob), regardless of the underlying parameter. SSL's gradient-brightness writes during sweeps are visual smoothing during in-between values; the steady-state always shows a single dot.

| Knob ID | Knob | Cells | n | Source |
|--------:|------|-------|---|--------|
| `0x00` | Low Pass        | `0x95..0x9F`              | 11 | uc1_15 |
| `0x01` | High Pass       | `0x8A..0x94`              | 11 | uc1_15 |
| `0x02` | HF Gain         | `0x7E..0x88`              | 11 | uc1_15 |
| `0x03` | HF Freq         | `0x73..0x7D`              | 11 | uc1_15 |
| `0x04` | HMF Gain        | `0x68..0x72`              | 11 | uc1_15 |
| `0x05` | HMF Freq        | `0x5D..0x67`              | 11 | uc1_15 |
| `0x06` | HMF Q           | `0x52..0x5C`              | 11 | uc1_15 |
| `0x07` | LMF Gain        | `0x45..0x4F`              | 11 | uc1_15 |
| `0x08` | LMF Freq        | `0x3A..0x44`              | 11 | uc1_15 |
| `0x09` | LMF Q           | `0x2F..0x39`              | 11 | uc1_15 |
| `0x0A` | LF Freq         | `0x24..0x2E`              | 11 | uc1_15 |
| `0x0B` | LF Gain         | `0x18..0x22`              | 11 | uc1_15 |
| `0x0C` | CS Input Trim   | `0xC0..0xCA`              | 11 | uc1_15 |
| `0x0E` | BC Ratio        | `0xD6..0xDC`              |  7 | dual_40 |
| `0x0F` | BC ScHpf        | `0xCB..0xD5`              | 11 | dual_40 + uc1_07 |
| `0x10` | BC Attack       | `0xDD..0xE3`              |  7 | dual_40 |
| `0x11` | BC Release      | `0xFA..0xFF, 0x00`        |  7 | dual_40 + uc1_06 (wraps byte boundary) |
| `0x12` | BC Threshold    | `0xE4..0xED`              | 10 | uc1_04 |
| `0x13` | BC Makeup       | `0xEF..0xF3`              |  5 | dual_40 (partial — no full sweep) |
| `0x14` | BC Mix          | `0x03..0x0C`              | 10 | dual_40 + uc1_07 |
| `0x16` | CS Fader Level  | `0x0E..0x17`              | 10 | uc1_15 |
| `0x17` | Gate Release    | `0x7C..0x86`              | 11 | uc1_15 |
| `0x18` | Gate Hold       | `0x87..0x91`              | 11 | dual_39 |
| `0x19` | Gate Threshold  | `0x72..0x7B`              | 10 | uc1_15 |
| `0x1A` | Gate Range      | `0x62..0x70`              | 15 | uc1_15 (15 cells incl. center 0x66) |
| `0x1B` | Comp Release    | `0x50..0x5A`              | 11 | uc1_15 |
| `0x1C` | Comp Threshold  | `0x46..0x4F`              | 10 | uc1_15 |
| `0x1D` | Comp Ratio      | `0x3B..0x44`              | 10 | uc1_15 |

Implementation in `extension/src/UC1Surface.cpp` (`ringFor()` / `pushKnobRing_`). All entries render as Position dots: the LED at `cells[round(value × (n-1))]` is lit, every other cell is off.

**OPEN ISSUES** — observed on hardware test, 2026-04-26:
- **Comp Ratio (0x1D) lights LMF Freq's ring instead of its own.** The cells `0x3B..0x44` we attributed to Comp Ratio overlap with LMF Freq's `0x3A..0x44`. Either the knob ID assignment for `0x1D` is wrong (the doc table at line 178 lists it without source — only 0x18 is confirmed via uc1_18), or our LMF Freq cell map is wrong, or `uc1_15` had ID-event labelling we are misreading. Needs a clean per-pot capture (turn ONE physical pot, capture, decode) for each Dyn knob to nail the (knob ID ↔ physical pot ↔ LED ring) mapping.
- BC Makeup partial — only 5 cells captured.

Tool: `analysis/decode_pot_clusters.py` does the basic time-bucketing; the per-knob attribution above used a follow-up script that joined LED events to `FF 24` knob events by midpoint-split.

### Session log (additions)

| date | capture | finding |
|------|---------|---------|
| 2026-04-24 | `uc1_28_idle_baseline_v2.pcapng` | Fresh 10 s idle baseline for UC1 decode pass. |
| 2026-04-24 | `uc1_29_led_brightness.pcapng` | UC1 brightness slider 5 steps — `FF 14 02` + `FF 4F 02` + `FF 5C 02` decoded. |
| 2026-04-24 | `uc1_30_track_colour_bar.pcapng` | Track selection sweep — `FF 66 02 11` (colour), `FF 66 25 02` / `FF 66 2B 04` (track-name carousel), `FF 66 05 01` (MAIN label). |
| 2026-04-24 | `uc1_32_slow_t1_t2.pcapng` | Slow T1↔T2 toggle — revealed `FF 66 03 00 01 <flag>` colour-bar-enable and `FF 66 05 01` CS 2 label. |
| 2026-04-24 | `dual_33_sel_tracks.pcapng` | Dual-device baseline — SEL colour goes only to UF8. |
| 2026-04-24 | `dual_34_colour_change.pcapng` | REAPER track-colour change — UC1 gets `FF 66 02 11 <palette_idx>`, UF8 gets `FF 66 09 18` palette broadcast. |
| 2026-04-24 | `dual_35_cs_gr_ramp.pcapng` | Channel Strip GR ramp — UC1 bank 0x01 cells 0x5C..0x60 with 5-step brightness. |
| 2026-04-24 | `dual_36_cs_vu_ramp.pcapng` | CS VU ramp — UC1 bank 0x02 cells 0x3A..0x3E, 0x71..0x7C, plus scattered markers. UF8 `FF 66 21 09` byte 6/7 = In/Out VU. |
| 2026-04-24 | `dual_37_lpf_ring.pcapng` | Low Pass pot sweep — 10 cells `0x95..0x9F`, dual-bank encoding verified. |
| 2026-04-24 | `dual_38_eq_pots.pcapng` | 11 EQ pots (HPF + HF/HMF/LMF/LF gain/freq/Q) — cell ranges extracted. |
| 2026-04-24 | `dual_39_dyn_pots.pcapng` | 7 Dyn pots (Comp + Gate) — cell ranges `0x72..0x91`. |
| 2026-04-24 | `dual_40_bc_pots.pcapng` | 7 BC pots — cell ranges `0xCB..0xF3`, 7-LED rings for attack/release/ratio. |
| 2026-04-24 | `dual_41_io_gain.pcapng` | Input + Output gain — additive LED rings (lower LEDs stay lit as value increases). |
