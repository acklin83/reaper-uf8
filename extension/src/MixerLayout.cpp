#include "MixerLayout.h"

namespace uf8 {

void MixerLayout::draw()
{
    // Phase 2.6b — single Channel Strip column:
    //   for each visible track:
    //     PluginMatch m = lookupPluginOnTrack(tr, Domain::ChannelStrip);
    //     drawTrackHeader(tr);
    //     drawSection(m, "Input/Trim");
    //     drawSection(m, "HPF/LPF");
    //     drawSection(m, "EQ HF/HMF/LMF/LF");
    //     drawSection(m, "Comp");
    //     drawGrMeter(tr, m.fxIndex);
    //     drawSection(m, "Gate");
    //     drawSection(m, "Output");
    //     drawFader(tr) + drawAudioMeter(tr);
    //
    // Phase 2.6c — Bus Compressor rack on the right:
    //   for each track with lookupPluginOnTrack(tr, Domain::BusComp):
    //     drawBusCompStrip(tr, fxIndex)
    //
    // Layout constants mirror UC1Surface.cpp groupings:
    //   Group 1 (Dyn/Gate/SC), Group 2 (EQ HfBell/EqType/EqIn/LfBell),
    //   EQ knob trios (Freq/Gain/Q), Comp trio, Gate quartet.
}

} // namespace uf8
