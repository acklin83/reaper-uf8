# SSL outreach — email draft

Status: draft, not sent.

Suggested recipient: `support@solidstatelogic.com` with subject prefix `ATTN: Product Team / 360° dev`. If a previous support contact has a name, send to that person directly with a one-line reference to the prior thread.

---

**Subject:** Native UF8 colour support for large REAPER sessions — independent extension project

Hello,

I'm an independent REAPER user and the developer of a small open-source extension project called **Rea-Sixty**. I'm writing to share what we're building, be transparent about it, and ask a few questions.

**The problem**

I routinely work on REAPER sessions with 100+ tracks. UF8 is a phenomenal piece of hardware and the scribble-strip workflow genuinely changes how I mix — but in practice, the only way to get track colours onto the displays is to insert a 360°-enabled VST3 plug-in (Channel Strip 2, 4K B/E/G, 360 Link, or Bus Compressor) on every track that should appear in the Plug-in Mixer. At our session sizes that's a non-starter on CPU and template-management grounds. MCU mode alone carries no colour information, so it isn't an alternative.

A previous exchange with SSL support confirmed that colour delivery is intrinsically tied to Plug-in Mixer Mode and an SSL plug-in being present.

**What we're building**

Rea-Sixty is an open-source REAPER extension (https://github.com/acklin83/reaper-uf8) that drives UF8 directly from REAPER's API. When SSL 360° is not running, the extension opens UF8's vendor-specific USB interface, reads track state from REAPER (colour, name, fader, pan, sends, metering, automation), and pushes it to the surface using the same vendor-protocol frames SSL 360° emits. The goal is to make UF8 fully usable in REAPER sessions without requiring an SSL plug-in on any track. UC1 is supported in the same way.

Concretely we have already mapped: frame format and checksum on both endpoints; init and keepalive required for the Plug-in Mixer rendering path; per-strip TFT colour command and the 16-entry palette; per-strip SEL/MUTE/SOLO LED frames and metering frames; the inbound button-event ID map for UF8 (per-strip + globals); button, knob, GR, VU, and display-zone formats for UC1. The architecture works end-to-end today; remaining gaps (per-strip text rendering, UC1 7-segment digits, a few unmapped palette slots) are detail work.

**The asks**

1. **Do you have any objection to this being a public, open-source project?** No SSL software, plug-in, firmware, or trademark is redistributed; the extension is entirely original code talking to the device at the USB layer. We'd rather hear early if you'd prefer we shape it differently.

2. **Would SSL be willing to share protocol documentation** — under NDA if needed? Even a partial reference would save us substantial capture-and-decode effort and produce a more robust result for shared customers.

3. If this could one day grow into something more formal (a documented partner extension, a listing alongside your supported integrations), we'd be glad to talk.

I want to be transparent above all: we're doing this because we love the hardware and want it to work in our sessions — not as an end-run around your product. Collaboration is genuinely the preferred outcome.

Thank you for reading. Happy to share captures, decoders, or a working build if that would help an internal conversation.

Best regards,
[Name]
[Contact]
