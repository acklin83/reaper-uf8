# UC1 Protocol Notes

Living spec of the SSL UC1 USB protocol, reverse-engineered by capturing SSL 360° ↔ UC1 traffic with USBPcap on Windows. Structure mirrors `protocol-notes.md` (UF8) so the two can be diffed side-by-side.

**Status 2026-04-22:** skeleton. Nothing captured yet. All payload columns are placeholders until the first capture session lands. Do not implement device code against anything marked `TBD`.

## Device

- VID `0x31E9` / PID `0x0023`  (UF8 is `0x0021`; UC1 HID controller — if any — TBD from descriptor)
- Expected USB 2.0 Full Speed, vendor-specific class `0xFF/0xFF/0xFF` (by analogy with UF8, unverified)
- **Endpoints:** TBD from descriptor capture. Likely `0x02` OUT bulk + `0x81` IN bulk, same as UF8.
- Exclusive claim: `SSL360Core` when running — cannot co-open. Captures must be taken on Windows with USBPcap (which taps at the XHC layer, above the driver claim) rather than via userland libusb.

## Capture constraints

**UF8 must be disconnected during UC1 captures.** `cap17_pm_gain_reduction.md` (2026-04-20) proved that SSL 360° steers Bus Compressor GR meter traffic to whichever controller has the dedicated GR display — UC1 wins when both are present. To see the full UC1 frame family we need UC1 as the only SSL device on the bus.

## Frame format

Working assumption (unverified): `FF <payload> <checksum>` with checksum = `sum(payload) mod 256`, same as UF8. First capture should confirm this by running `analysis/parse_usbpcap.py` with its existing UF8 frame detector — if it parses UC1 traffic cleanly, the family is shared.

Incoming (UC1 → host) session prefix: unknown whether UC1 uses the same `31 60` header as UF8.

## Commands — host → UC1 (EP TBD)

### Init / wakeup sequence
TBD from `uc1_01_init_clean.pcap`. Expected: ~50–150 frames sent by SSL 360° on fresh enumeration before the device becomes responsive. Will be extracted verbatim into `extension/src/init_sequence_uc1.inc` (parallel to the UF8 version).

### Idle heartbeat
TBD from `uc1_02_idle_baseline.pcap`.

### Layer / mode selection
TBD. UC1 has at least two modes: Bus Compressor mode (default, physical knobs drive the selected track's bus comp) and Channel Strip mode (physical knobs drive the channel strip plugin on the focused track). The mode switch likely arrives as either a `FF 1B …` style command (like UF8's layer selector) or a distinct command family.

### Parameter display
UC1 has a 4-character-ish numeric display per knob section (Threshold, Ratio, Attack, Release, Makeup, Mix). Format TBD — likely `FF 66 <len> <zone> <strip/knob-id> <ASCII chars>` analogous to UF8's scribble-strip zones, but the zone byte will differ.

### GR display
**The headline target.** The UC1 GR bar-graph is a ~20-LED vertical strip. Format TBD. Working hypothesis from `cap17`: `FF 13 04 <4 bytes>` frames seen going to UC1 when UF8 was present are the GR stream — but that capture didn't have the UC1 in standalone mode, so we can't tell whether those are meter-only frames or include other telemetry. Resolve in `uc1_12_gr_dynamic.pcap`.

### VU (input/output) meters
TBD from `uc1_13_vu_meters.pcap`. UF8 uses `FF 38 04 …` / `FF 39 04 …`; UC1 may share or differ.

### LED / button feedback
Per-button LED frames (Bypass-active, Ext-SC, etc.) — TBD from `uc1_08_buttons_all.pcap`.

## Events — UC1 → host (EP TBD)

### Idle heartbeat
TBD.

### Knob rotation
UC1's continuous knobs (Threshold, Makeup, Attack, Release, Mix, HPF) are most likely encoded as rotation deltas analogous to UF8's V-Pot `FF 24 02 <id> <delta>`. Stepped knobs (Ratio) may use an absolute-value encoding instead. Confirm in `uc1_04`–`uc1_07`.

### Button press / release
TBD from `uc1_08`. Likely `FF 22 03 <id> 00 <state>` family, matching UF8.

### Track-selection follow
UC1 doesn't have bank buttons — it locks onto the "currently focused track" and re-renders when the focus changes. The mechanism is **host-driven**, not device-driven: SSL 360° detects REAPER's track selection (via DAW Extension API / Thrift callbacks from plugins) and pushes a "retarget UC1" frame. That retarget frame is what we need to identify in `uc1_10_track_select.pcap`. Our implementation will mirror it: Rea-Sixty's `FocusedTrack` watches `SetTrackSelected` and re-pushes the knob-mirror state.

## Plugin detection / mapping

The UC1 is physically laid out to match SSL Native Bus Compressor 2's control set. `PluginMap.cpp` already has the slot table (`kBusComp2Slots`); for UC1 we add a parallel table mapping each physical UC1 knob ID to a `LinkSlot`. Example (speculative IDs, pending captures):

| UC1 knob      | LinkSlot `linkIdx` | VST3 param on Bus Comp 2 |
|---------------|-------------------:|-------------------------:|
| Threshold     | 1                  | 2                        |
| Makeup        | 2                  | 3                        |
| Attack        | 3                  | 4                        |
| Release       | 4                  | 5                        |
| Ratio         | 5                  | 6                        |
| Sidechain HPF | 6                  | 7                        |
| Mix           | 7                  | 8                        |

Channel Strip mode: the UC1 also has a channel-strip knob bank for driving Native Channel Strip 2 params (Input Trim, HPF, LPF, EQ bands, Dynamics). That mapping gets its own table once the capture shows which physical controls belong to which LinkSlots.

## GR data source — not from the plugin

The SSL plugins ship GR to 360° over encrypted Thrift IPC (see `plugin-ipc-notes.md`). We do **not** read that channel. Instead, GR is computed by a bundled JSFX envelope-follower (`extension/jsfx/rea_sixty_gr_probe.jsfx`) inserted next to the SSL compressor, and its value is read via `TrackFX_GetParam`. Ballistic mismatch against the plugin's internal detector is expected (≤2 dB transient error acceptable); if the hardware meter feels visibly off compared to the plugin GUI, the fallback is Thrift-MITM — but that path is not preferred and stays deferred.

## Open items (track as captures land)

- [ ] USB descriptor dump — endpoints, max packet size, interface class/subclass
- [ ] Init sequence extraction (uc1_01)
- [ ] Idle heartbeat identification (uc1_02)
- [ ] Layer/mode selector command (uc1_03)
- [ ] Physical-knob → event-frame ID map (uc1_04 through uc1_07)
- [ ] Button ID map (uc1_08)
- [ ] Display (numeric readout) frame format (uc1_09)
- [ ] Track-focus retarget frame (uc1_10)
- [ ] GR bar-graph frame format, static (uc1_11)
- [ ] GR bar-graph frame format, dynamic stream rate and encoding (uc1_12)
- [ ] VU LED frame format (uc1_13)
- [ ] External-sidechain indicator frames (uc1_14)

## Capture index

| Date | File | Summary |
|------|------|---------|
| 2026-04-22 | `uc1_01_init_clean.pcapng` | Init/wakeup sequence on fresh enumeration — 27944 pkts to address 28, endpoints 0x00/0x80/0x02/0x81 |
| 2026-04-22 | `uc1_02_idle_baseline.pcapng` | 10 s idle heartbeat — 11288 pkts, ~1130 pkt/s, same endpoint set |
| _TBD_ | `uc1_03_layer_boot.pcap` | Mode switch (Bus Comp / Channel Strip) |
| _TBD_ | `uc1_04_knob_threshold_sweep.pcap` | Threshold full sweep |
| _TBD_ | `uc1_05_knob_ratio_steps.pcap` | Ratio discrete steps |
| _TBD_ | `uc1_06_knob_attack_release.pcap` | Attack + Release sweeps |
| _TBD_ | `uc1_07_knob_makeup_mix.pcap` | Makeup + Mix + HPF sweeps |
| _TBD_ | `uc1_08_buttons_all.pcap` | Every button pressed once |
| _TBD_ | `uc1_09_display_params.pcap` | REAPER-side param changes → display output frames |
| _TBD_ | `uc1_10_track_select.pcap` | REAPER track selection changes → UC1 retarget |
| _TBD_ | `uc1_11_gr_static.pcap` | Audio off, Threshold pinned low, max GR |
| _TBD_ | `uc1_12_gr_dynamic.pcap` | Audio loop through aggressive comp, 10 s |
| _TBD_ | `uc1_13_vu_meters.pcap` | Known-level sines (−20/−10/−6/0 dBFS) |
| _TBD_ | `uc1_14_multiple_sc.pcap` | External sidechain on, various SC sources |
