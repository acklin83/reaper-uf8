# uc1_07_knob_makeup_mix

Date: 2026-04-22
Windows host: StoerPC (Windows 10.0.26200.8037)
Wireshark / tshark: 4.6.4
USBPcap interface: `\\.\USBPcap3`
SSL 360°: running
REAPER: running
UF8: physically disconnected
UC1 device address: 28

## Session state

Same session as `uc1_04`/`uc1_05`/`uc1_06`. SSL Native Bus Compressor 2 on the focused track, UC1 mirroring.

## Action

25 s capture. Three continuous-knob sweeps, each full CCW → full CW in ~5 s, separated by ~2 s idle gaps:
- Makeup gain
- Mix (dry/wet)
- Sidechain HPF

Tail idle after the third sweep.

## Summary

- 29184 packets total on USBPcap3

## Analysis

```
python3 analysis/parse_usbpcap_uc1.py \
    captures/uc1/uc1_07_knob_makeup_mix.pcapng \
    --baseline captures/uc1/uc1_02_idle_baseline.pcapng
```

Three dense novel-payload windows separated by gaps. Completes the Bus Comp section's knob-ID table when combined with `uc1_04` (Threshold) and `uc1_06` (Attack, Release). `uc1_05` handles Ratio's stepped encoding separately.
