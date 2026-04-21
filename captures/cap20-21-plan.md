# cap20 / cap21 — capture plan

Two protocol zones still undecoded, both blocking Plug-in Mixer Mode
feature-parity with SSL 360°. Both need fresh USBPcap sessions on
Windows with SSL 360° driving a UF8 — same setup as the cap13–19
series.

## cap20 — V-Pot Readout Bar byte semantics

**Command (already identified):** `FF 66 09 0D <8 bytes> CKSUM` (13 B
broadcast, one byte per strip). Values 0x01–0x08 seen in prior captures
but the byte→LED-position mapping hasn't been cross-referenced against
actual UF8 display output.

**Problem:** the extension currently emits positions it computes from
the plug-in param value, but the rendered bar doesn't line up with the
value visually. Either the encoding is MCU-style (upper nibble = mode,
lower nibble = position) or the byte range / mapping differs from
plain "LED position 1..8".

**Capture setup:**
1. SSL 360° running, 4K E on track 1, Channel Strip Mode active on UF8.
2. Set HF Freq V-Pot to the leftmost physical detent (lowest value).
3. Camera-record the UF8 display (LED bar state) while scrolling the
   V-Pot slowly from bottom to top of its range. ~5 seconds.
4. Note every visible bar state change (e.g. "1 LED lit left", "2 LEDs
   lit left", "4 LEDs", "full", etc.).
5. Tail the capture for the `FF 66 09 0D` bytes sent during that time.
   Cross-reference time-aligned visual state ↔ transmitted byte value.

**Output:** a table in `docs/protocol-notes.md` under V-Pot Readout Bar:

| Byte | Visual state |
|------|--------------|
| 0x01 | 1 LED pos 1  |
| ...  | ...          |

Also test: is the bar direction fixed, or does it mirror / spread for
bipolar params like Pan? Do a pass with Width (bipolar) on the V-Pot
and see if a different byte range / encoding kicks in.

## cap21 — Channel-Number Zone

**Where:** the small numeric digit top-left corner of each scribble
strip's color bar. Visible on the UF8 in Channel Strip Mode — per track,
shows the REAPER track index (or DAW channel number).

**Command:** UNKNOWN — not seen in any cap01–19. Likely a per-strip
text command distinct from Channel Strip Type (`FF 66 06 17`) and
Parameter Label (`FF 66 <n+2> 04`). Possibly `FF 66 ?? ??` with a
different subcmd byte.

**Capture setup:**
1. SSL 360° running, bank set to tracks 1–8, CS 2 on each.
2. Start capture.
3. Press BANK → once → tracks 9–16 visible. The channel-number zone
   should change from 1–8 to 9–16.
4. Press BANK ← back to 1–8.
5. Repeat once more for confirmation.
6. Stop capture.

**Expected:** two deltas in traffic — one at each BANK event. Each
delta should contain the channel-number command. Look for a command
that changes its second-byte payload between "1" / "9" / "10" etc.
ASCII characters `0x31` / `0x39` / `0x31 0x30`, or as a binary index.

**Output:** command format documented in `docs/protocol-notes.md`, then
wired into the extension's `pushZonesForVisibleSlots` to emit the real
REAPER track number per strip.

## cap22 — DAW-Layer LED commands

**Goal:** find the vendor-USB command(s) SSL 360° sends to light the
UF8's per-strip SOLO / CUT / SEL / ARM LEDs when the DAW pushes state
changes.

**Context:** `captures/led/*` (cap_led_01..11) exhausted the PM-Mode
search and found nothing — SSL 360° seems to use a non-USB path in PM
mode. The LED README flags the DAW (MCU) layer as "definitely receives
LED commands over USB" — we just haven't captured it yet.

**Capture setup:**
1. Windows box, SSL 360° running. UF8 switched to **DAW Layer**
   (press the Layer 1 / DAW button). Should show MCU scribbles.
2. REAPER running with Mackie Control Universal surface configured on
   SSL V-MIDI Port 1.
3. Tracks 1–8 visible. Press-record baseline (10 s, no activity).
4. Start capture.
5. Solo track 1 via REAPER mixer click. Unsolo. Mute track 1. Unmute.
   Record-arm track 1. Disarm. Select track 3.
6. Solo track 5, track 7, track 2 in quick succession.
7. Stop.
8. `python3 analysis/parse_usbpcap.py cap22.pcap --baseline baseline.pcap`

**Expected:** novel OUT frames on the UF8's vendor-USB endpoint that
correlate 1:1 with the state changes. Likely pattern:
`FF <cmd> 02 <id> <on|off> CKSUM` where `id` is an MCU note number
(0x00..0x07 ARM, 0x08..0x0F SOLO, 0x10..0x17 MUTE, 0x18..0x1F SELECT).

**Phase-2 step once decoded:** test whether the same command drives
LEDs while UF8 is in PM Layer (Layer 2). If yes, wire into
`ReaSixtySurface::SetSurfaceSolo/Mute/Selected/RecArm` via `g_dev->send(...)`
instead of the current MCU-MIDI path (which requires SSL 360° running).
If the command is layer-gated (PM mode ignores it), LED feedback stays
DAW-Mode-only.
