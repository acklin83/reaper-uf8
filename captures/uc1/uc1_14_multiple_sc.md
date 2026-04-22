# uc1_14_multiple_sc

Date: 2026-04-22
Windows host: StoerPC (Windows 10.0.26200.8037)
Wireshark / tshark: 4.6.4
USBPcap interface: `\\.\USBPcap3`
SSL 360°: running
REAPER: running
UF8: physically disconnected
UC1 device address: 34

## Session state

Bus Comp 2 loaded on the focused track. Focus is on the Ext-SC (external sidechain) **button LED** behavior on the UC1, not on actual sidechain audio routing — user confirmed source identity is irrelevant for this capture.

## Action

20 s capture. User toggled the External Sidechain state on / off multiple times so the corresponding LED on the UC1 flipped on and off. Exact toggle count and timing not recorded; the pattern is "several SC-on / SC-off transitions spread across 20 s".

## Summary

- 22547 packets total on USBPcap3

## Analysis

```
python3 analysis/parse_usbpcap_uc1.py \
    captures/uc1/uc1_14_multiple_sc.pcapng \
    --baseline captures/uc1/uc1_02_idle_baseline.pcapng
```

Two complementary outputs should emerge:

1. **UC1 → host** event frames for each Ext-SC button press (also present in `uc1_08_buttons_all` at row 11 "S/C Listen" — cross-check the two captures to confirm the same button-ID byte shows up here).
2. **Host → UC1** LED-state frames that light/dark the Ext-SC button indicator on each toggle. These are the interesting new decode target for this capture: one frame at SC-on, one at SC-off, repeatedly.

Note: uc1_14 captures LED feedback for **one** specific button (Ext-SC). Generalizing the LED-feedback frame family to all UC1 buttons will require either diffing against each button's UC1_08 entry or a dedicated follow-up capture that toggles state on every LED-capable button in sequence.
