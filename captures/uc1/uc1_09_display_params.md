# uc1_09_display_params

Date: 2026-04-22
Windows host: StoerPC (Windows 10.0.26200.8037)
Wireshark / tshark: 4.6.4
USBPcap interface: `\\.\USBPcap3`
SSL 360°: running
REAPER: running
UF8: physically disconnected
UC1 device address: 28

## Session state

Same session as `uc1_08`. SSL Native Bus Compressor 2 + Channel Strip 2 on the focused track. UC1 mirroring.

## Action

20 s capture. User changed multiple Bus Comp 2 parameters via the plugin GUI (mouse, **not** the UC1), ~500 ms between changes. **Specific parameters and order were not recorded** — instructions said order didn't matter, which in hindsight was wrong for per-param attribution but is fine for format decoding.

## Summary

- 23130 packets total on USBPcap3

## Analysis

```
python3 analysis/parse_usbpcap_uc1.py \
    captures/uc1/uc1_09_display_params.pcapng \
    --baseline captures/uc1/uc1_02_idle_baseline.pcapng
```

Goal of this capture: reveal the **format** of host → UC1 display-write frames (zone/knob-addressing bytes, ASCII vs. encoded value, frame length for the numeric readouts). Per-parameter ID mapping is not recoverable from this file on its own, but comes for free from `uc1_04` through `uc1_07`: turning a physical knob produces the same display-update frames in the host→UC1 direction as a mouse drag on the GUI, so the Threshold/Ratio/Attack/Release/Makeup/Mix/HPF IDs get pinned there.

If a clean per-param mapping capture turns out necessary later, redo as a dedicated file with one param per second, documented step by step.
