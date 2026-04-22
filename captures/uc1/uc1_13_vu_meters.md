# uc1_13_vu_meters

Date: 2026-04-22
Windows host: StoerPC (Windows 10.0.26200.8037)
Wireshark / tshark: 4.6.4
USBPcap interface: `\\.\USBPcap3`
SSL 360°: running
REAPER: running
UF8: physically disconnected
UC1 device address: 34

## Session state

Track with SSL Native Channel Strip 2 loaded, Channel Strip 2 **bypassed** so input and output VU reflect the raw signal without EQ / Dynamics coloration. (Bus Comp 2 irrelevant here — UC1's I/O VU meters belong to the Channel Strip section, not the Bus Comp section.)

## Action

20 s capture. Test tones played in sequence through the track at:

| ~T    | Level | Expected UC1 VU |
|-------|-------|------------------|
| 0 s   | silence / pre-roll | bottom |
| ~1 s  | −20 dBFS           | ~1/3 of scale (depending on dBFS→meter mapping) |
| ~3 s  | silence            | bottom |
| ~4 s  | −10 dBFS           | higher |
| ~6 s  | silence            | bottom |
| ~7 s  | −6 dBFS            | near top |
| ~9 s  | silence            | bottom |
| ~10 s | 0 dBFS             | top / clip |
| tail  | silence            | bottom |

(Exact second-by-second alignment approximate; the four distinct level plateaus separated by silences should be clearly visible as novel-payload bursts.)

## Summary

- 22894 packets total on USBPcap3 — notably higher than the idle baseline (~11 k / 10 s = ~22 k / 20 s would be "idle only"), so the VU stream adds roughly +5–15 % packets. VU frames likely distinct from GR frames (separate family) and updated faster than GR since VU must track transients.

## Analysis

```
python3 analysis/parse_usbpcap_uc1.py \
    captures/uc1/uc1_13_vu_meters.pcapng \
    --baseline captures/uc1/uc1_02_idle_baseline.pcapng
```

Four novel-payload plateaus, one per level, with matching silence gaps between. Two decode targets:

1. VU frame family / format — likely `FF XX 04 <in-byte> <out-byte>` or similar, separate from GR frames seen in uc1_11/12.
2. Level → byte calibration: known dBFS values at the four plateaus give us a 4-point curve for the byte-to-VU-LED mapping on the UC1's I/O meter strips.
