# uc1_12_gr_dynamic

Date: 2026-04-22
Windows host: StoerPC (Windows 10.0.26200.8037)
Wireshark / tshark: 4.6.4
USBPcap interface: `\\.\USBPcap3`
SSL 360°: running
REAPER: running
UF8: physically disconnected
UC1 device address: 34

## Session state

Same track / compressor setup as `uc1_11` (aggressive Bus Comp 2), but with dynamic program material instead of a sustained tone — GR bar-graph moving across the full range.

## Action

10 s capture. Audio playing, GR animated, no user input.

## Summary

- 11370 packets total on USBPcap3 (`uc1_11` static: 11302; `uc1_02` idle: 11288 — small positive delta vs. idle suggests GR updates either ride the heartbeat slot or add a modest number of dedicated frames per second)

## Analysis

```
python3 analysis/parse_usbpcap_uc1.py \
    captures/uc1/uc1_12_gr_dynamic.pcapng \
    --baseline captures/uc1/uc1_02_idle_baseline.pcapng
```

Diffing against the idle baseline should now surface a **wide range of novel payloads** rather than the small set seen in `uc1_11` — each distinct novel value corresponds to one GR level the meter passed through. That gives us:
1. GR frame structure (should match whatever repeated in uc1_11)
2. Value-byte location (the byte that varies with GR level)
3. Update rate (frames per second attributable to GR, by subtracting the static-capture novel-frame rate)
4. Full encoding range by sampling min / max / intermediate values present in the dynamic run

Combining with `uc1_11`'s known fixed value (~12 dB) gives us a two-point calibration for the byte → dB mapping — a third known-level capture later can pin it further if needed.
