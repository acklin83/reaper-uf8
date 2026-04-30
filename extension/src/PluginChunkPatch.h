#pragma once
//
// PluginChunkPatch — toggle SSL plug-in state that lives only in the
// VST3 chunk and isn't reachable via TrackFX_GetParam… or
// TrackFX_GetNamedConfigParm.
//
// Two cases addressed (probe-discovered 2026-04-30):
//   * HQ Mode  = <PARAM_NON_AUTO id="HighQuality" value="0.0|1.0"/>
//                appears once per A/B slot in CS / 4K B/E/G plug-ins.
//                Active slot determined by the StateASelected attribute.
//   * A/B comp = <SSL_PLUGIN_STATE … StateASelected="0|1" …>
//                top-level attribute on every SSL plug-in (CS + BC).
//
// Both diffs are length-preserving so the chunk size stays identical and
// no length headers in the VST3 wrapper need updating.
//
// Implementation: GetTrackStateChunk → decode each base64 line
// independently (REAPER stores VST chunks as multiple per-line blocks
// each with its own padding) → patch UTF-8 XML payload → re-encode
// preserving the original per-line byte boundaries → SetTrackStateChunk.
//
// Caveats:
//   * SetTrackStateChunk forces a plug-in state reload — internal
//     processing buffers reset, may produce a sub-millisecond click
//     during playback. User-confirmed acceptable for HQ / A/B since
//     these toggle infrequently.
//   * Both togglers walk the WHOLE FX chain and patch every SSL plug-in
//     they recognise (CS2, 4K B/E/G for HQ; CS-family + BC for A/B).
//     Otherwise a track with two SSL plug-ins (e.g. 4K G + CS2 stacked,
//     or CS + BC) would only get the first one toggled, which the user
//     called out as the obvious surprise.

struct MediaTrack;

namespace uf8 {

// Toggle <PARAM_NON_AUTO id="HighQuality"> in the active A/B slot of
// every CS-family SSL plug-in on `track`. Returns the number of plug-ins
// patched (0 if none found / parse failure on every match).
int togglePluginHQ(MediaTrack* track);

// Toggle StateASelected on every SSL plug-in (CS-family or BC) on
// `track`. Returns the number of plug-ins patched.
int togglePluginAB(MediaTrack* track);

} // namespace uf8
