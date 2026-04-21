# Interoperability Rationale

This document records the legal basis and methodology of the reverse
engineering used to build Rea-Sixty. It exists so that the project's
compliance posture is transparent to contributors and to any third
party who might question it.

This is not a legal opinion; it is a factual record of what the
project does and does not do, plus the statutory provisions on which
it relies.

## Purpose

Rea-Sixty enables REAPER users who own Solid State Logic UF8
control surfaces to display REAPER-native track information — most
importantly track colors — on the UF8 scribble strips without
running SSL 360° and without installing an SSL plug-in on every
track.

This is strictly an **interoperability** goal: making two pieces of
software (REAPER and the UF8's firmware, both legally
acquired/licensed by the user) work together in a way the hardware
vendor does not provide out of the box.

## Method

1. The user runs legally licensed SSL 360° software on their own
   computer, controlling their own legally purchased SSL UF8.
2. USB wire-level traffic between the two is observed passively
   (USBPcap on Windows; macOS 15 no longer exposes USB interfaces
   to Wireshark).
3. Captured byte streams are analysed to identify the functional
   control sequences (frame magic, checksum, per-command payloads)
   needed to drive the UF8.
4. An independent C++ implementation in this repository emits
   equivalent functional frames, driven by REAPER's public
   extension API.

What is **not** done:

- No decompilation, disassembly, or binary analysis of SSL 360°,
  SSL360Core, or any SSL driver or firmware.
- No circumvention of DRM, code signing, license checks, or other
  technological protection measures. There are none in the observed
  path; the USB traffic is cleartext between two components on the
  user's own machine.
- No redistribution of SSL binaries, firmware images, updaters,
  installers, or proprietary creative content.
- No use of SSL's trademarks as source identifiers for this project
  or its artefacts.

## Legal Basis

### European Union
**Directive 2009/24/EC (Software Directive), Article 6** permits
decompilation and observation of a program for the purpose of
achieving interoperability with an independently created program,
when (a) the acts are performed by a lawful user, (b) the
interoperability information is not otherwise readily available, and
(c) the acts are confined to the parts necessary to achieve
interoperability. Contractual clauses purporting to exclude these
rights are void under Article 8.

All three conditions are satisfied by this project.

### Germany
**§69e UrhG** (Dekompilierung) transposes Art. 6 into German law.
**§69d Abs. 3 UrhG** additionally permits observation, study, and
testing of a program during normal operation to determine its
underlying ideas and principles. Both apply to the passive USB
capture performed here.

### United States
**17 USC §1201(f)** (DMCA interoperability exception) permits
circumvention and related analysis for the purpose of identifying
and analysing elements of a program necessary to achieve
interoperability of an independently created program. No
circumvention is required for this project; the provision is cited
only as a fallback for potential US distribution.

**Sega Enterprises v. Accolade** (9th Cir. 1992) confirms that
reverse engineering for interoperability can constitute fair use
under US copyright law.

### Trademark
Use of the marks "SSL", "Solid State Logic", "SSL 360°", "UF8", and
"UC1" in this project's documentation is limited to **nominative
fair use** — identifying the hardware and software this project
interoperates with. The marks are not used as source identifiers
for Rea-Sixty, its artefacts, or its distribution channels.

## Scope — What Is and Is Not Reproduced

| Category | Status |
| --- | --- |
| Functional USB control byte sequences (frame magic, checksum, command opcodes, payload layout) | Reproduced — these are ideas / functional interfaces, not protected creative expression |
| SSL firmware, drivers, or software binaries | **Not reproduced, not redistributed** |
| SSL branded strings, graphics, UI assets | **Not reproduced, not redistributed** |
| Capture files containing the above | Kept local, `.pcapng` gitignored; occasional small reference captures committed only after inspection confirms no creative content |

## Commercial Status

This project is non-commercial open source, released under the MIT
license. A pivot to commercial distribution would require fresh
legal review; the current rationale covers the open-source
interoperability use case specifically.

## Review Triggers

This document should be revisited if any of the following change:

- The project starts decompiling SSL binaries (out of scope today).
- The project begins redistributing SSL firmware, installers, or
  binaries (out of scope today).
- The project is commercialised or bundled with paid software.
- A SSL cease-and-desist or legal inquiry is received.
- EU Software Directive is revised, or German / US law meaningfully
  changes around interoperability exceptions.
