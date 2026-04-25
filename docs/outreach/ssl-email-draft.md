# SSL outreach — email draft

Status: draft, not sent.

Suggested recipient: `support@solidstatelogic.com` with subject prefix `ATTN: Product Team / 360° dev`. If a previous support contact has a name, send to that person directly with a one-line reference to the prior thread.

---

**Subject:** Independent open-source UF8/UC1 project for REAPER — saying hi

Hello,

I'm an independent REAPER user and the developer behind a small open-source extension project called **Rea-Sixty**. I'm writing as a courtesy heads-up, to be transparent about what we're doing, and to ask a couple of questions.

**What we're building**

Rea-Sixty is an open-source REAPER extension (https://github.com/acklin83/reaper-uf8) that drives UF8 (and UC1) directly from REAPER's API. The attraction was simple: the colour-aware Plug-in Mixer experience on UF8 is genuinely lovely, and we wanted that experience available natively in REAPER without depending on a plug-in being present on every track. Once we started looking at the protocol it turned into its own little reverse-engineering hobby project, and the result is a working extension.

When SSL 360° isn't running, the extension opens UF8's vendor USB interface, reads track state from REAPER (colour, name, fader, pan, sends, metering, automation), and pushes it to the surface using the same vendor-protocol frames SSL 360° emits. UC1 is supported in the same way.

Concretely we have already mapped: frame format and checksum on both endpoints; init and keepalive required for the Plug-in Mixer rendering path; per-strip TFT colour command and the 16-entry palette; per-strip SEL/MUTE/SOLO LED frames and metering frames; the inbound button-event ID map for UF8 (per-strip + globals); button, knob, GR, VU, and display-zone formats for UC1. The architecture works end-to-end today; remaining gaps (per-strip text rendering, UC1 7-segment digits, a few unmapped palette slots) are detail work.

**The asks**

1. **Do you have any objection to this being a public, open-source project?** No SSL software, plug-in, firmware, or trademark is redistributed; the extension is entirely original code talking to the device at the USB layer. We'd rather hear early if you'd prefer we shape it differently.

2. **Would SSL be willing to share protocol documentation** — under NDA if needed? Even a partial reference would save us substantial capture-and-decode effort and produce a more robust result for shared customers.

3. If this could one day grow into something more formal (a documented partner extension, a listing alongside your supported integrations), we'd be glad to talk.

We did this because we love the hardware and were curious what was possible — collaboration is genuinely the preferred outcome.

Thank you for reading. Happy to share captures, decoders, or a working build if it would help an internal conversation.

Best regards,
[Name]
[Contact]
