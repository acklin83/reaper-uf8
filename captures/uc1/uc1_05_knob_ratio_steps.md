# uc1_05_knob_ratio_steps

Date: 2026-04-22
Windows host: StoerPC (Windows 10.0.26200.8037)
Wireshark / tshark: 4.6.4
USBPcap interface: `\\.\USBPcap3`
SSL 360°: running
REAPER: running
UF8: physically disconnected
UC1 device address: 28

## Session state

Same session as `uc1_04`. SSL Native Bus Compressor 2 on the focused track, UC1 mirroring.

## Action

15 s capture. User clicked through each discrete Ratio step (1.5, 2, 4, 10), ~500 ms pause per step, **one direction only** (no return sweep). First capture attempt was redone from scratch — this file is the second take.

## Summary

- 16940 packets total on USBPcap3

## Analysis

```
python3 analysis/parse_usbpcap_uc1.py \
    captures/uc1/uc1_05_knob_ratio_steps.pcapng \
    --baseline captures/uc1/uc1_02_idle_baseline.pcapng
```

Expected pattern vs. `uc1_04`: **discrete** novel-payload bursts at each step rather than a continuous stream. Contrast against Threshold reveals whether stepped knobs use a separate encoding (absolute value byte) or the same delta/event family with different timing.
