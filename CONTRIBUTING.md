# Contributing

## Principles

- Every capture that sits in `captures/` (or not — most are gitignored) must have a matching `.md` sibling describing what was recorded and why.
- Every new finding updates `docs/protocol-notes.md` — that's the single source of truth.
- Pure logic (framing, checksum, palette quantization) gets unit tests. Nothing that depends on libusb or REAPER needs tests beyond manual verification on the hardware.
- No stripped-out SSL code or firmware. Only passive USB capture + black-box protocol documentation.

## Workflow for adding a decoded command

1. Capture the triggering event on Windows with USBPcap (baseline + event, see `docs/windows-capture-workflow.md`).
2. Diff the OUT payloads against baseline; isolate novel frames.
3. Document the frame structure and checksum verification in `docs/protocol-notes.md`.
4. Add a builder (for OUT) or parser (for IN) in `extension/src/Protocol.{h,cpp}`.
5. Add a test against the exact captured byte string in `extension/tests/test_protocol.cpp`.
6. Commit in one go: capture notes + doc + code + test.

## Commit messages

Present tense, imperative, under 72 chars in the subject. Body explains *what changed and why* — not just restating the diff. Example:

```
Add bank-switch decoding

Bank → button triggers a fresh FF 66 09 18 <8 indices> CKSUM rather
than a bank-pointer update. Confirms the UF8 doesn't hold bank state
— host re-resolves the visible 8 tracks every time. Extension must
hook REAPER's mixer-scroll event.
```

## Captures policy

- `.pcap` / `.pcapng` files ≥ 1 MB: not committed (gitignored). Share via the issue description or an external drop if needed.
- `.pcap` files under ~1 MB that isolate a single clean event and serve as regression/reference: commit with `git add -f`, always with a sibling `.md`.
