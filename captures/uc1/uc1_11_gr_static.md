# uc1_11_gr_static

Date: 2026-04-22
Windows host: StoerPC (Windows 10.0.26200.8037)
Wireshark / tshark: 4.6.4
USBPcap interface: `\\.\USBPcap3`
SSL 360°: running
REAPER: running
UF8: physically disconnected
UC1 device address: 34

## Session state

One track with SSL Native Bus Compressor 2 loaded, aggressive settings (low threshold, high ratio, fast attack). Sustained audio routed in so the compressor sat on it continuously. User confirmed the UC1 GR bar-graph was **pinned at ~12 dB reduction** and held steady throughout.

UF8 physically disconnected — so any GR frame family present in this capture is headed for the UC1. (This is also the capture that starts to address the `uc1-gr-routing` re-verification note: if we see `FF 13 04 …` frames here, the cap17 finding for UC1-targeted GR frames is confirmed for this frame family; whether UF8 uses the same or a different family remains open until a UF8-only counterpart capture runs.)

## Action

10 s capture. Audio playing, compressor squashing, GR steady. No user input during the window.

## Summary

- 11302 packets total on USBPcap3 (vs. 11288 in `uc1_02` idle baseline — near-identical rate, meaning the GR stream rides inside the normal heartbeat rate rather than adding separate packets, or the delta is small and concentrated in payload contents rather than packet count)

## Analysis

```
python3 analysis/parse_usbpcap_uc1.py \
    captures/uc1/uc1_11_gr_static.pcapng \
    --baseline captures/uc1/uc1_02_idle_baseline.pcapng
```

Expected output: a small set of repeating novel payloads (the GR meter stream at a fixed value) distinguishable from baseline heartbeat. Value ≈ 12 dB should show up as a consistent byte / pair of bytes inside the GR frame, giving us the encoding unit directly.
