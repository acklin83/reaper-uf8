# Plugin ↔ 360° ↔ UF8 — architecture + path forward

Goal: drive SSL plugin parameters on the UF8 scribble strips / V-Pots without
running SSL 360°. That's the piece that, until now, forced SSL 360° to stay
installed even when our extension handles the USB protocol.

Written 2026-04-21 after reading the 360° app bundle, one native SSL plugin
binary (`SSL Native Channel Strip 2.vst3`), and the SSL 360° Link config
directory on the Mac Studio.

## How SSL 360° wires plugins to the hardware

Three stages, end-to-end:

```
 SSL plugin (VST3) ───▶ SSL360Core (app) ───▶ UF8  (vendor-USB, already decoded)
        │                   │
        │  JUCE              │  SSL 360° GUI / layout / bank
        │  InterprocessConnection
        │  + Apache Thrift
        │  + encrypted envelope
        │  (kEncryptedThriftContainerThriftId)
```

Evidence in `SSL Native Channel Strip 2.vst3` binary:

- `N4juce22InterprocessConnectionE` — JUCE's cross-platform IPC class
  (TCP-localhost on macOS, named-pipe on Windows)
- `TSocket::open() socket()`, `TProtocolException` — Apache Thrift client
- `kEncryptedThriftContainerThriftId` — Thrift payloads are wrapped in an
  SSL-proprietary encryption container
- `N3Ssl24CommsAwareAudioParameterE` — every plugin param is a comms-capable
  parameter; changes publish to the remote + can be driven remotely
- `N3Ssl22TrackColourCommsObjectE`, `N3Ssl20TrackNameCommsObjectE` — the
  plugin pushes **track colour / track name** up to 360°. This is how the
  SSL app learns track colors, and why the per-track-plugin requirement
  exists in their design.
- `N3Ssl19DawExtensionManagerE` + `PresonusExtensionHandler`,
  `SteinbergExtensionHandler`, `VST3ExtensionHandler` — plugin reads track
  metadata from the DAW via VST3 context attributes (Steinberg
  `ContextInfo` API).
- `N3Ssl21AssignerLinkParameterE`, `N3Ssl27AssignerLinkSwitchParameterE` —
  the "which param lands on which UF8 slot" mapping is carried in the
  plugin + the `.factory` XMLs.

`SSL 360.app` itself is a **.NET single-file executable** (CoreCLR strings
everywhere). Its resources folder contains `SSL360Core` (the service that
claims the vendor-USB interface and serves Thrift requests from plugins) +
`SSL360Gui` (the WPF-like UI layer).

## The `.factory` gift

`/Library/Application Support/SSL/SSLPlugins/SSL360Link/` contains XML
mapping files — one per *third-party* plugin SSL 360 Link can host:

```xml
<VALUE name="LowPassFreq">
  <LINK_PARAM LinkParamIndex="6" LinkParamId="LowPassFreq"
              LinkParamName="Low Pass Filter" LinkParamLegend="LPF">
    <HOSTED_PARAM HostedParamIndex="18" HostedParamId="16"
                  HostedParamName="LP Frq"/>
  </LINK_PARAM>
</VALUE>
```

Each entry is exactly the data we need:

| Field | Meaning |
|-------|---------|
| `LinkParamIndex` | UF8 V-Pot slot (1-based) |
| `LinkParamId` | Stable ID string |
| `LinkParamName` | Long label |
| `LinkParamLegend` | 3-4 char scribble-strip legend (e.g. "LPF") |
| `HostedParamIndex` | VST3 parameter index of the hosted plugin |
| `HostedParamName` | Plugin's own name for that param |
| `LinkInverted="1"` | Invert the value (optional) |

These cover the 4K E/G, UAD, VMR, Vertigo, Pulsar, Waves, etc. wrappers.
Native SSL plugins don't ship a `.factory` because they embed the same
assignment table internally and push it via `AssignerLinkParameter`.

## Two paths to replace the app's plugin responsibility

### Path A — fake the Thrift server

Build an IPC server that impersonates SSL360Core so existing SSL plugins
connect to us instead.

Cons:
- Thrift schema is unpublished; we'd have to reverse it from the wire
- Payloads are wrapped in an SSL-proprietary encryption envelope —
  breaking past that is both technically hard and legally risky
- Auth path likely involves iLok / PACE Eden tokens (strings confirm iLok
  integration on both ends)
- Fragile: any 360° update could change the schema / envelope / protocol
  version and silently break us

Conclusion: **not worth it**. Doing this "legit" would mean SSL cooperates
and publishes an SDK. They won't.

### Path B — drive plugins via REAPER's VST3 bridge  ✅

Ignore the 360° IPC entirely. Use REAPER's `TrackFX_*` API to enumerate,
read, and write SSL plugin parameters directly:

- `TrackFX_GetNumParams(tr, fx)`
- `TrackFX_GetParamName(tr, fx, paramIdx, buf, bufsz)`
- `TrackFX_GetParamNormalized(tr, fx, paramIdx)` → `double 0..1`
- `TrackFX_SetParamNormalized(tr, fx, paramIdx, val)`
- `TrackFX_FormatParamValueNormalized(tr, fx, paramIdx, val, buf, bufsz)`
  → pre-formatted string (e.g. `"-6.0 dB"`) — exactly what we render on
  the Value Line
- `TrackFX_GetFXName(tr, fx, buf, bufsz)` — already used for SSL plugin
  detection

Mapping source: the `.factory` XMLs, plus a hand-rolled table for each
native SSL plugin (Channel Strip 2, 4K B, 4K E, 4K G, Bus Compressor 2,
X-EQ 2, X-Comp, etc.). One static table per plugin ID, checked in.

Pros:
- No reverse engineering of encrypted IPC, no legal risk
- Resilient: as long as REAPER's VST3 bridge exists and the plugin doesn't
  rename parameters, our code keeps working
- Works for *any* VST3 plugin — opens the door to user-defined mappings
  for non-SSL plugins as a Phase 2 feature (cf. SSL 360 Link)
- Parameter feedback is automatic: REAPER's surface API fires
  `CSurf_OnParamChange` when the plugin GUI moves a knob — the extension
  sees it and pushes new values to the UF8

## Implementation sketch (Phase 1 stretch or early Phase 2)

```
extension/
  src/
    PluginMap.h          // LinkSlot struct + static tables per plugin ID
    PluginMap.cpp        //   + factory-XML loader for SSL360Link configs
    ParamMirror.cpp      // per-strip: resolve track→plugin→slot→VST3 idx
                         //            pull name/legend/value → UF8 zones
                         //            map V-Pot rotation → SetParamNormalized
  data/
    ssl_channel_strip_2.json    // native plugin maps (hand-built once)
    ssl_4k_e.json
    ssl_4k_g.json
    ssl_4k_b.json
    ssl_bus_comp.json
```

Wire into the existing timer (`pushZonesForVisibleSlots`):

1. Per visible slot, find the first detected SSL plugin on the track
   (reuse `sslPluginShortName()` logic).
2. Look up the plugin's map. For each of the 8 V-Pot slots the UF8 shows,
   read the mapped VST3 param's normalized value + formatted string.
3. Push Parameter Label (`LinkParamLegend`), Value Line, V-Pot Readout
   Bar from the formatted value.
4. In `onUf8Input` V-Pot rotation: instead of `PanDelta`, queue a
   `PluginParamDelta` event; the main-thread drain computes
   new = old ± scaled delta and calls `TrackFX_SetParamNormalized`.

## Open items this unblocks

- **Plug-in Mixer Position indicator** (blocker #4) — will resolve when we
  have plugin + slot identification in place (we choose the slot number
  ourselves, just render it into the "No" zone command).
- **Top soft-key labels** — become "parameter page" labels per the SSL
  convention, or can be remapped to user actions (Phase 2 config UI).
- **GR meter** — independent; still waiting on `cap17b_uf8_only_gr.pcap`
  (blocker #3 in the memory file).

## Non-goals

- We are NOT reimplementing SSL 360's Thrift IPC, encryption envelope, or
  plugin-side registration. SSL plugins running inside REAPER still try to
  connect to 360° and will silently fail (no 360° running). Their audio
  processing doesn't depend on the connection, only their decorative
  "connected to 360°" LED does. Users who want that LED on can still run
  360° alongside — but only if they disconnect the UF8 from 360° first,
  since 360° claims the vendor-USB interface exclusively.

- We are NOT handling plugin-to-plugin side-chain metadata (360° Link's
  inter-plugin routing). Out of scope for Phase 1.

## Next concrete step

Phase-2 task, plan before coding:

1. Enumerate `Channel Strip 2` params: run a tiny ReaScript in REAPER to
   dump `TrackFX_GetParamName` for each index on a track that hosts the
   plugin. Commit the dump as `docs/ssl-native-params/channel_strip_2.md`.
2. Draft the Link-slot mapping by eyeballing SSL 360°'s Plugin Mixer
   layout screenshot + the param list. Single JSON file.
3. Repeat for 4K E, 4K G, 4K B, Bus Comp 2. A half-day of setup for every
   native SSL plugin we care about.
4. Load the JSON + `.factory` XMLs from `PluginMap.cpp`.
5. Wire V-Pot read/write in the existing `ReaSixtySurface` timer.
