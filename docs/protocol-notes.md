# UF8 Protocol Notes (living document)

Every finding goes here. Treat as the single source of truth for what we know about the UF8 wire protocol.

## Device (known)
- VID `0x31e9` / PID `0x0021`, USB 2.0 Full Speed
- 1 interface, vendor-specific class `0xFF/0xFF/0xFF`
- 2 endpoints — direction + transfer type TBD from capture
- Runs alongside a separate HID device (PID `0x0022`) that handles buttons/knobs

## Endpoint layout
TBD — after first capture, fill in:
```
EP 0x01 OUT  bulk?  maxPacketSize=?   (commands host → device)
EP 0x81 IN   bulk?  maxPacketSize=?   (responses / heartbeats?)
```

## Framing
TBD. Questions to answer from captures:
- Fixed-length frames or variable-length with header?
- Magic bytes / sync pattern at packet start?
- Sequence numbers or CRC?

## Color command
TBD. Hypotheses ranked by likelihood:
1. **`[cmd_byte, channel_0..7, R, G, B]`** — per-channel direct RGB
2. **`[cmd_byte, channel, palette_index]`** — indexed palette (like X-Touch's 8 colors)
3. **Frame-buffer dump** — full display bitmap per channel, colors embedded in pixel data

First capture will tell us which.

## Bank switch
TBD. Expected:
- A command that changes "which 8 REAPER tracks the hardware is currently showing"
- Possibly sent BY the host (REAPER → UF8 "now displaying tracks 9-16")
- OR: pressed on UF8, sent device → host, host then reacts with fresh color data

## Heartbeat / keepalive
TBD — establish from idle baseline capture.

## Session log
| date       | capture                           | finding                                        |
|------------|-----------------------------------|------------------------------------------------|
| 2026-04-19 | (pending)                         | Device enumerated, endpoints TBD               |
