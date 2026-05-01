# uc1_36_gate_gr.pcapng

Captured 2026-05-01 to confirm the Gate GR strip protocol on the UC1
Channel-Strip Dynamics section, and to look for the data source SSL360
uses to drive it (currently a TODO in `pollGainReduction_`).

## What was captured

15-second tshark capture on `\\.\USBPcap3` while the user swept the
**Gate Range** knob slowly through its full range — without audio,
because the SSL CS Gate plug-in mirrors `Range` directly into Gate GR
on idle (no signal triggers gating dynamics).

## Hardware-side findings (confirmed)

Gate GR strip cells are exactly what `uc1-led-encoding.md` had:

* Cells `0x61..0x65`, byte5=`0x01`, bank=`0x02` (brightness) +
  bank=`0x01` (selection bit on activate).
* Brightness cascade `{0x03, 0x19, 0x2D, 0x54, 0x99, 0xFF}` — same
  6-step ramp Comp GR uses (memory had the visible 5-step subset
  `{0x19..0xFF}` from `dual_35_cs_gr_ramp`; `0x03` is the dim seed
  before the ramp begins).
* Activation order matches Comp GR: bank=0x02 of next cell to `0x00`
  → bank=0x01 of next cell to `0x01` → bank=0x02 of previous cell to
  `0xFF` → ramp the active cell's bank=0x02.

So `pushGainReduction()` already speaks the right protocol — the
five-element `kLevels` table is fine for our stripTargets math, the
0x03 seed is just the firmware's animation prelude.

## Plug-in-side: still open

Range ring writes started at t=0.17s, but Gate-GR strip writes didn't
start until t=2.00s — 1.8s of Range advance before any GR animation.
That gap rules out "Gate GR = Range param value" as the source: the
plug-in is computing a gating-depth value and SSL360 reads it from the
plug-in (not the param register).

Currently `pollGainReduction_()` hardcodes `csGateGr = 0.0f` because
no parmname is known for Gate-only readback. Added a diagnostic action
`Rea-Sixty: Probe Gate GR sources on focused CS plug-in` that polls a
battery of candidate `TrackFX_GetNamedConfigParm` names and dumps each
one's return value. User runs the action while turning Range — the
name whose value follows Range (and varies as Range moves) is the one
to wire into `pollGainReduction_`.
