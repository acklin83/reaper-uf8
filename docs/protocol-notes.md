# UF8 Protocol Notes (living document)

Every finding goes here. Treat as the single source of truth for what we know about the UF8 wire protocol.

## Device (confirmed)
- VID `0x31e9` / PID `0x0021`, USB 2.0 Full Speed
- 1 interface, vendor-specific class `0xFF/0xFF/0xFF`
- 2 endpoints (see below)
- Sibling devices on same machine: UC1 (PID `0x0023`), UF8 HID controller (PID `0x0022`)

## Capture environment
- Captured on Windows 11 24H2 with USBPcap 1.5.4.0 + Wireshark 4.6.4
- UF8 = **device_address 10** on USBPcap3 (depends on Windows USB enumeration; may differ per boot)
- SSL 360° version on the Windows machine: **2.0.6.67265** (current per SSL)

## Endpoint layout (from `probe_inj.pcap`, idle-only, 5 s)

| Endpoint | Direction | Transfer | Typical size | Role                              |
|----------|-----------|----------|--------------|-----------------------------------|
| `0x02`   | OUT       | bulk     | 64 B (heartbeat), 32 B (control) | host → UF8 commands |
| `0x81`   | IN        | bulk     | 29 B frame = 2 B payload `31 60` | UF8 → host heartbeat |

Idle-capture counts (5 s):
- OUT EP `0x02`: 248× 64-byte + 33× 32-byte = 281 packets
- IN EP `0x81`: 2506× 29-byte heartbeat + 2× 36-byte events = 2508 packets
- Plus a few EP0 control transfers at enumeration time (descriptor requests)

## Framing — confirmed

All host → UF8 packets start with magic byte `FF`, then payload, then a 1-byte checksum.

**Checksum formula:** sum all bytes *after* the leading `FF` and *before* the final checksum byte, take low 8 bits.

Verified against every unique idle payload captured so far:

| Payload | Bytes after FF (excl checksum) | Sum mod 256 | Checksum byte |
|---------|--------------------------------|-------------|---------------|
| `ff 66 21 09 00×59 90` | `66 21 09 00…00` | `0x90` | `0x90` ✓ |
| `ff 66 21 0a 00×59 91` | `66 21 0a 00…00` | `0x91` | `0x91` ✓ |
| `ff 1b 01 00 1c`       | `1b 01 00`      | `0x1c` | `0x1c` ✓ |
| `ff 1b 01 01 1d`       | `1b 01 01`      | `0x1d` | `0x1d` ✓ |
| `ff 1b 01 02 1e`       | `1b 01 02`      | `0x1e` | `0x1e` ✓ |
| `ff 1b 01 03 1f`       | `1b 01 03`      | `0x1f` | `0x1f` ✓ |

## Idle commands seen so far

### `FF 66 21 XX 00…(59 zeros) CKSUM` (64 B) — heartbeat / poll pair
- `XX` alternates `09` and `0a`, sent in pairs back-to-back (observed: every ~40 ms)
- Hypothesis: two-phase poll — one frame pings UF8, one pings UC1 (both at addr 10 on USBPcap3? no — UC1 is separate device, so more likely both pings are for UF8 and encode a state query)
- All-zero payload strongly suggests "keepalive / status request", not data

### `FF 1B 01 XX CKSUM` (32 B — wait, actual payload bytes = 5, frame.len 32 is pcap-header inclusive) — control
- `XX` cycles `00 → 01 → 02 → 03`
- Hypothesis: periodic "current layer/page" or "display mode" selector. UF8 has multiple display layers (Channel, Plugin Mixer, Send…) — this could be the layer-select stream.

*Note:* actual USB payload is 5 bytes for these. `frame.len` of 32/64 is the USBPcap-wrapped frame including USB-URB + USBPcap header. Use `usb.capdata` for the real payload.

### `31 60` (2 B) — UF8 idle heartbeat
- Sent at ~500 Hz (2506 in 5 s)
- Not yet decoded — likely "I'm alive, no events"

## Color command — NOT yet captured

Idle-only capture contains no color packets. That's consistent: no REAPER session was running, no SSL plugin on a track, therefore no Plugin Mixer colors to push.

**Next capture target:** color-change event with a REAPER project loaded and an SSL Channel Strip on at least one track. Expected to reveal a NEW magic-byte pattern (`FF XX …`) that does NOT match any of the idle commands above.

## Bank switch — NOT yet captured

TBD.

## Analysis recipes

```bash
# all UF8 packets
tshark -r CAPTURE.pcap -Y "usb.device_address == 10"

# unique OUT commands (host → UF8)
tshark -r CAPTURE.pcap -Y "usb.device_address == 10 and usb.endpoint_address == 0x02 and usb.capdata" \
       -T fields -e usb.capdata | sort -u

# novel OUT commands vs baseline
tshark -r BASELINE.pcap -Y "usb.device_address == 10 and usb.endpoint_address == 0x02 and usb.capdata" \
       -T fields -e usb.capdata | sort -u > /tmp/baseline.txt
tshark -r EVENT.pcap    -Y "usb.device_address == 10 and usb.endpoint_address == 0x02 and usb.capdata" \
       -T fields -e usb.capdata | sort -u > /tmp/event.txt
comm -23 /tmp/event.txt /tmp/baseline.txt   # commands novel to the event
```

`analysis/parse_usbpcap.py` does the same with Python/pyshark and verifies the checksum.

## Session log
| date       | capture                 | finding                                                            |
|------------|-------------------------|--------------------------------------------------------------------|
| 2026-04-19 | probe_inj.pcap (5 s)    | UF8 = addr 10 / USBPcap3. Endpoints 0x02 OUT + 0x81 IN. Checksum = sum-mod-256 after FF magic. Idle commands: `FF 66 21 XX …` (64 B heartbeat, XX=09/0a), `FF 1B 01 XX …` (32 B control, XX=0..3). IN heartbeat `31 60`. No color packets yet (idle-only capture). |
