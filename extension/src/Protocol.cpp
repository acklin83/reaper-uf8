#include "Protocol.h"

#include <cstring>
#include <limits>
#include <numeric>

namespace uf8 {

uint8_t checksum(std::span<const uint8_t> payload)
{
    uint32_t sum = 0;
    for (auto b : payload) sum += b;
    return static_cast<uint8_t>(sum & 0xFFu);
}

std::vector<uint8_t> buildColorCommand(const std::array<uint8_t, kStripCount>& indices)
{
    // Frame: FF 66 09 18 <8 indices> CKSUM
    std::vector<uint8_t> frame;
    frame.reserve(14);
    frame.push_back(kFrameMagic);

    // Payload (bytes that go into the checksum)
    const std::array<uint8_t, 3> header{0x66, 0x09, 0x18};
    for (auto b : header) frame.push_back(b);
    for (auto b : indices) frame.push_back(b);

    // Checksum = sum of everything after FF, excluding the checksum byte itself
    std::span<const uint8_t> payload{frame.data() + 1, frame.size() - 1};
    frame.push_back(checksum(payload));
    return frame;
}

std::vector<uint8_t> buildStripTextUpper(uint8_t strip, std::string_view text)
{
    // Frame: FF 66 <len> 0B <strip> <N chars> CKSUM
    // where len = N + 2 (strip byte + N text bytes... wait: len = count of bytes between
    // len and cksum, which is 0B + strip + N chars = 2 + N).
    const size_t N = std::min(text.size(), size_t{7});
    const uint8_t len = static_cast<uint8_t>(N + 2);

    std::vector<uint8_t> frame;
    frame.reserve(5 + N);
    frame.push_back(kFrameMagic);
    frame.push_back(0x66);
    frame.push_back(len);
    frame.push_back(0x0B);
    frame.push_back(strip);
    for (size_t i = 0; i < N; ++i) frame.push_back(static_cast<uint8_t>(text[i]));

    std::span<const uint8_t> payload{frame.data() + 1, frame.size() - 1};
    frame.push_back(checksum(payload));
    return frame;
}

std::vector<uint8_t> buildFaderPosition(uint8_t strip, uint8_t lsb, uint8_t msb)
{
    std::vector<uint8_t> frame;
    frame.reserve(7);
    frame.push_back(kFrameMagic);
    frame.push_back(0x1E);
    frame.push_back(0x03);
    frame.push_back(strip);
    frame.push_back(lsb);
    frame.push_back(msb);
    std::span<const uint8_t> payload{frame.data() + 1, frame.size() - 1};
    frame.push_back(checksum(payload));
    return frame;
}

std::vector<uint8_t> buildMotorEnable(uint8_t strip, bool enable)
{
    std::vector<uint8_t> frame;
    frame.reserve(6);
    frame.push_back(kFrameMagic);
    frame.push_back(0x1D);
    frame.push_back(0x02);
    frame.push_back(strip);
    frame.push_back(enable ? 0x01 : 0x00);
    std::span<const uint8_t> payload{frame.data() + 1, frame.size() - 1};
    frame.push_back(checksum(payload));
    return frame;
}

std::vector<uint8_t> buildMeter(uint8_t strip, uint8_t level)
{
    std::vector<uint8_t> frame;
    frame.reserve(8);
    frame.push_back(kFrameMagic);
    frame.push_back(0x38);
    frame.push_back(0x04);
    frame.push_back(static_cast<uint8_t>(strip * 3));
    frame.push_back(0x00);
    frame.push_back(0x00);
    frame.push_back(static_cast<uint8_t>(0xF0 | (level & 0x0F)));
    std::span<const uint8_t> payload{frame.data() + 1, frame.size() - 1};
    frame.push_back(checksum(payload));
    return frame;
}

std::vector<uint8_t> buildLayerPluginMixer()
{
    // Captured byte-for-byte: FF 66 11 0F 10 00 40 00 <20 00 × 6> 96
    std::vector<uint8_t> frame{
        0xFF, 0x66, 0x11, 0x0F,
        0x10, 0x00, 0x40, 0x00,
        0x20, 0x00, 0x20, 0x00,
        0x20, 0x00, 0x20, 0x00,
        0x20, 0x00, 0x20, 0x00
    };
    std::span<const uint8_t> payload{frame.data() + 1, frame.size() - 1};
    frame.push_back(checksum(payload));
    return frame;
}

std::vector<uint8_t> buildLayerDaw()
{
    std::vector<uint8_t> frame{0xFF, 0x66, 0x11, 0x0F};
    for (int i = 0; i < 16; ++i) frame.push_back(0x00);
    std::span<const uint8_t> payload{frame.data() + 1, frame.size() - 1};
    frame.push_back(checksum(payload));
    return frame;
}

std::vector<uint8_t> buildPluginSlotActive()
{
    // FF 66 0A 00 03 00 00 00 00 00 00 00 00 CKSUM
    std::vector<uint8_t> frame{0xFF, 0x66, 0x0A, 0x00, 0x03};
    for (int i = 0; i < 8; ++i) frame.push_back(0x00);
    std::span<const uint8_t> payload{frame.data() + 1, frame.size() - 1};
    frame.push_back(checksum(payload));
    return frame;
}

std::vector<uint8_t> buildPluginSlotName(uint8_t strip, std::string_view text)
{
    // FF 66 <len> 04 <strip> <N text bytes> CKSUM   (len = N + 2)
    const size_t N = std::min(text.size(), size_t{12});
    const uint8_t len = static_cast<uint8_t>(N + 2);

    std::vector<uint8_t> frame;
    frame.reserve(5 + N);
    frame.push_back(kFrameMagic);
    frame.push_back(0x66);
    frame.push_back(len);
    frame.push_back(0x04);
    frame.push_back(strip);
    for (size_t i = 0; i < N; ++i) frame.push_back(static_cast<uint8_t>(text[i]));

    std::span<const uint8_t> payload{frame.data() + 1, frame.size() - 1};
    frame.push_back(checksum(payload));
    return frame;
}

std::vector<uint8_t> buildChannelStripType(uint8_t strip, std::string_view fourChars)
{
    // FF 66 06 17 <strip> <4 chars, space-padded> CKSUM
    std::vector<uint8_t> frame{0xFF, 0x66, 0x06, 0x17, strip};
    for (size_t i = 0; i < 4; ++i) {
        frame.push_back(i < fourChars.size() ? static_cast<uint8_t>(fourChars[i]) : 0x20);
    }
    std::span<const uint8_t> payload{frame.data() + 1, frame.size() - 1};
    frame.push_back(checksum(payload));
    return frame;
}

std::vector<uint8_t> buildValueLine(uint8_t strip, std::string_view text)
{
    // FF 66 15 0E <strip> <19 chars, space-padded> CKSUM    (24 bytes)
    std::vector<uint8_t> frame{0xFF, 0x66, 0x15, 0x0E, strip};
    for (size_t i = 0; i < 19; ++i) {
        frame.push_back(i < text.size() ? static_cast<uint8_t>(text[i]) : 0x20);
    }
    std::span<const uint8_t> payload{frame.data() + 1, frame.size() - 1};
    frame.push_back(checksum(payload));
    return frame;
}

std::vector<uint8_t> buildLedCommand(uint8_t ledId, bool on)
{
    // FF 3B 03 <id> 00 <state> CKSUM
    std::vector<uint8_t> frame{0xFF, 0x3B, 0x03, ledId, 0x00, uint8_t(on ? 0x01 : 0x00)};
    std::span<const uint8_t> payload{frame.data() + 1, frame.size() - 1};
    frame.push_back(checksum(payload));
    return frame;
}

// LED colour constants. Bright = lit, dim = unlit. cap31 (yellow/orange/
// white) and cap33 (10-colour DAW palette) supply the byte pairs.
LedColour ledColourYellow() { return {0xEF, 0xF0, 0x11, 0xF0}; }
LedColour ledColourOrange() { return {0x3F, 0xF0, 0x12, 0xF0}; }
LedColour ledColourRed()    { return {0x0F, 0xF0, 0x01, 0xF0}; }
LedColour ledColourWhite()  { return {0xFF, 0xFF, 0x11, 0xF1}; }

LedColour ledColourClassDefault(LedClass cls)
{
    switch (cls) {
        case LedClass::Solo: return ledColourYellow();
        case LedClass::Cut:  return ledColourRed();
        case LedClass::Sel:  return ledColourWhite();
    }
    return ledColourWhite();
}

namespace {
// SEL DAW-Colour palette anchors — captured in cap33 by sweeping
// REAPER track-colours through SSL360's quantizer. The hardware
// renders these 10 distinct colours: red, orange, yellow, green, cyan,
// blue, purple, magenta, pink, white. Ordered so that the nearest-match
// function below picks the visually-closest entry by Euclidean RGB
// distance.
struct PaletteEntry {
    uint8_t r, g, b;
    LedColour bytes;
};

constexpr PaletteEntry kSelPalette[] = {
    {255,   0,   0, {0x0F, 0xF0, 0x01, 0xF0}}, // red
    {255, 128,   0, {0x3F, 0xF0, 0x12, 0xF0}}, // orange
    {255, 255,   0, {0xEF, 0xF0, 0x11, 0xF0}}, // yellow
    {  0, 255,   0, {0xF0, 0xF0, 0x10, 0xF0}}, // green (also lime)
    {  0, 255, 255, {0xF0, 0xFF, 0x10, 0xF1}}, // cyan (also lightblue)
    {  0,   0, 255, {0x00, 0xFF, 0x00, 0xF1}}, // blue
    {128,   0, 255, {0x03, 0xFF, 0x01, 0xF3}}, // purple
    {255,   0, 255, {0x2F, 0xF4, 0x12, 0xF1}}, // magenta
    {255,   0, 128, {0x0F, 0xFF, 0x01, 0xF1}}, // pink
    {255, 255, 255, {0xFF, 0xFF, 0x11, 0xF1}}, // white
};
} // namespace

const PaletteRgb* selPaletteRgb(int* count)
{
    static const PaletteRgb kRgb[] = {
        {255,   0,   0}, {255, 128,   0}, {255, 255,   0}, {  0, 255,   0},
        {  0, 255, 255}, {  0,   0, 255}, {128,   0, 255}, {255,   0, 255},
        {255,   0, 128}, {255, 255, 255},
    };
    if (count) *count = static_cast<int>(sizeof(kRgb) / sizeof(kRgb[0]));
    return kRgb;
}

namespace {
// HSV chromatic-plane projection: (S·cosH, S·sinH). Used to compare
// colours by hue/saturation, not raw RGB. Raw-RGB Euclidean misranks
// dark inputs (r12/g25/b84 → orange instead of blue) because palette
// entries with small numbers happen to be close, regardless of hue.
// Even max-channel-normalised RGB misranks tinted darks (same input
// matched red-tinted "blue-leaning violet" instead of pure deep blue,
// because Euclidean treats green-tint and red-tint as both being
// equally "off-axis"). Polar HSV puts opposite tints on opposite
// sides, so hue distance is honest.
struct ChromaXY { double x, y; };
ChromaXY chromaXY(int r, int g, int b)
{
    const int mx = std::max({r, g, b});
    const int mn = std::min({r, g, b});
    if (mx == 0 || mx == mn) return {0.0, 0.0};
    const double chroma = static_cast<double>(mx - mn);
    const double s = chroma / mx;
    double h_deg = 0.0;
    if (mx == r) {
        h_deg = 60.0 * ((static_cast<double>(g) - b) / chroma);
        if (h_deg < 0) h_deg += 360.0;
    } else if (mx == g) {
        h_deg = 60.0 * (((static_cast<double>(b) - r) / chroma) + 2.0);
    } else {
        h_deg = 60.0 * (((static_cast<double>(r) - g) / chroma) + 4.0);
    }
    const double h_rad = h_deg * 3.14159265358979323846 / 180.0;
    return {s * std::cos(h_rad), s * std::sin(h_rad)};
}
} // anonymous

LedColour ledColourForTrackRgb(uint32_t rgb)
{
    if (rgb == 0) return ledColourWhite();
    const int r = static_cast<int>((rgb >> 16) & 0xFF);
    const int g = static_cast<int>((rgb >> 8)  & 0xFF);
    const int b = static_cast<int>( rgb        & 0xFF);
    const ChromaXY cxy = chromaXY(r, g, b);
    double bestDist = std::numeric_limits<double>::infinity();
    LedColour best = ledColourWhite();
    for (const auto& p : kSelPalette) {
        const ChromaXY pxy = chromaXY(p.r, p.g, p.b);
        const double dx = cxy.x - pxy.x;
        const double dy = cxy.y - pxy.y;
        const double d  = dx*dx + dy*dy;
        if (d < bestDist) { bestDist = d; best = p.bytes; }
    }
    return best;
}

LedColourFrames buildLedColourPair(uint8_t strip, LedClass cls, bool on,
                                   LedColour colour)
{
    const uint8_t a38 = on ? colour.aBright : colour.aDim;
    const uint8_t b38 = on ? colour.bBright : colour.bDim;
    // FF39's bytes when ON: empirically always (00, F0) across cap31 and
    // every cap33 palette entry — independent of bBright. Treat it as the
    // fixed "base / off-state" companion frame the firmware overlays under
    // FF38's bright colour.
    const uint8_t a39 = on ? 0x00 : colour.aDim;
    const uint8_t b39 = on ? 0xF0 : colour.bDim;

    const uint8_t cell = static_cast<uint8_t>(
        0x17 - 3 * strip - static_cast<uint8_t>(cls));

    LedColourFrames out;
    out.ff38 = {0xFF, 0x38, 0x04, cell, 0x00, a38, b38};
    {
        std::span<const uint8_t> payload{out.ff38.data() + 1, out.ff38.size() - 1};
        out.ff38.push_back(checksum(payload));
    }
    out.ff39 = {0xFF, 0x39, 0x04, cell, 0x00, a39, b39};
    {
        std::span<const uint8_t> payload{out.ff39.data() + 1, out.ff39.size() - 1};
        out.ff39.push_back(checksum(payload));
    }
    return out;
}

LedColourFrames buildLedColourPair(uint8_t strip, LedClass cls, bool on)
{
    return buildLedColourPair(strip, cls, on, ledColourClassDefault(cls));
}

LedColourFrames buildTopSoftKeyLed(uint8_t strip, TopSoftKeyState state,
                                   LedColour colour)
{
    uint8_t a38, b38, a39, b39;
    switch (state) {
        case TopSoftKeyState::On:
            a38 = colour.aBright;  b38 = colour.bBright;
            a39 = 0x00;            b39 = 0xF0;
            break;
        case TopSoftKeyState::Dim:
            a38 = colour.aDim;     b38 = colour.bDim;
            a39 = colour.aDim;     b39 = colour.bDim;
            break;
        case TopSoftKeyState::Off:
        default:
            // No-colour state (cap41 `00 F0` for unassigned strips).
            a38 = 0x00;  b38 = 0xF0;  a39 = 0x00;  b39 = 0xF0;
            break;
    }

    const uint8_t cell = static_cast<uint8_t>(0x1F - strip);

    LedColourFrames out;
    out.ff38 = {0xFF, 0x38, 0x04, cell, 0x00, a38, b38};
    {
        std::span<const uint8_t> payload{out.ff38.data() + 1, out.ff38.size() - 1};
        out.ff38.push_back(checksum(payload));
    }
    out.ff39 = {0xFF, 0x39, 0x04, cell, 0x00, a39, b39};
    {
        std::span<const uint8_t> payload{out.ff39.data() + 1, out.ff39.size() - 1};
        out.ff39.push_back(checksum(payload));
    }
    // Top-soft-keys are 3-state legacy LEDs (cap48 2026-04-30): the
    // colour-pair sets the colour level shown, but the FF 3B 03 mono
    // frame is the actual on/off toggle. Without it, Dim renders
    // invisible on most strips — only strips already in firmware-
    // default ON state would show as dim.
    out.legacy = buildLedCommand(cell, state != TopSoftKeyState::Off);
    return out;
}

std::vector<uint8_t> buildSelectedStripBitmask(uint16_t mask)
{
    // FF 66 03 06 <lo> <hi> CKSUM
    std::vector<uint8_t> frame{
        0xFF, 0x66, 0x03, 0x06,
        static_cast<uint8_t>(mask & 0xFF),
        static_cast<uint8_t>((mask >> 8) & 0xFF)};
    std::span<const uint8_t> payload{frame.data() + 1, frame.size() - 1};
    frame.push_back(checksum(payload));
    return frame;
}

std::vector<uint8_t> buildChannelNumber(uint8_t strip, std::string_view digits)
{
    // FF 66 <len> 14 <strip> <N ASCII> CKSUM  where len = N + 2
    const size_t N = digits.size();
    const uint8_t len = static_cast<uint8_t>(N + 2);
    std::vector<uint8_t> frame;
    frame.reserve(5 + N);
    frame.push_back(kFrameMagic);
    frame.push_back(0x66);
    frame.push_back(len);
    frame.push_back(0x14);
    frame.push_back(strip);
    for (char c : digits) frame.push_back(static_cast<uint8_t>(c));
    std::span<const uint8_t> payload{frame.data() + 1, frame.size() - 1};
    frame.push_back(checksum(payload));
    return frame;
}

std::vector<uint8_t> buildVPotReadoutBar(const std::array<uint16_t, kStripCount>& positions)
{
    // Frame: FF 66 11 0F <16 bytes: 8 × 2-byte LE per strip> CKSUM (20 bytes)
    std::vector<uint8_t> frame;
    frame.reserve(20);
    frame.push_back(kFrameMagic);
    const std::array<uint8_t, 3> head{0x66, 0x11, 0x0F};
    for (auto b : head) frame.push_back(b);
    for (auto p : positions) {
        frame.push_back(static_cast<uint8_t>(p & 0xFF));
        frame.push_back(static_cast<uint8_t>((p >> 8) & 0xFF));
    }
    std::span<const uint8_t> payload{frame.data() + 1, frame.size() - 1};
    frame.push_back(checksum(payload));
    return frame;
}

std::vector<uint8_t> buildFaderDbReadout(uint8_t strip, std::string_view value)
{
    // FF 66 0A 0C <strip> <6 bytes, NUL-padded> "dB" CKSUM    (14 bytes)
    // SSL 360° only fills the first 4 bytes; the remaining 2 are NUL there.
    // We treat the full 6 bytes as a value slot so values like "-12.5" fit
    // (NUL-padded, matching SSL's convention for short values).
    std::vector<uint8_t> frame{0xFF, 0x66, 0x0A, 0x0C, strip};
    for (size_t i = 0; i < 6; ++i) {
        frame.push_back(i < value.size() ? static_cast<uint8_t>(value[i]) : 0x00);
    }
    frame.push_back('d');
    frame.push_back('B');
    std::span<const uint8_t> payload{frame.data() + 1, frame.size() - 1};
    frame.push_back(checksum(payload));
    return frame;
}

std::vector<uint8_t> buildStripTextLower(uint8_t strip, std::string_view text)
{
    // Frame: FF 66 09 0E <strip> <7 chars, space-padded> CKSUM   (total 13 bytes)
    std::vector<uint8_t> frame;
    frame.reserve(13);
    frame.push_back(kFrameMagic);
    const std::array<uint8_t, 3> head{0x66, 0x09, 0x0E};
    for (auto b : head) frame.push_back(b);
    frame.push_back(strip);

    for (size_t i = 0; i < 7; ++i) {
        frame.push_back(i < text.size() ? static_cast<uint8_t>(text[i]) : 0x20);
    }
    std::span<const uint8_t> payload{frame.data() + 1, frame.size() - 1};
    frame.push_back(checksum(payload));
    return frame;
}

std::array<std::vector<uint8_t>, 2> buildPluginMixerHeartbeat()
{
    auto build = [](uint8_t counter) {
        std::vector<uint8_t> f;
        f.reserve(13);
        f.push_back(kFrameMagic);
        const std::array<uint8_t, 3> head{0x66, 0x09, counter};
        for (auto b : head) f.push_back(b);
        for (int i = 0; i < 8; ++i) f.push_back(0x00);
        std::span<const uint8_t> payload{f.data() + 1, f.size() - 1};
        f.push_back(checksum(payload));
        return f;
    };
    return {build(0x15), build(0x16)};
}

// --- Decoded 2026-04-24 ---

std::vector<uint8_t> buildLedBrightness(uint8_t level)
{
    // FF 2D 08 00 00 <b> 00 <b> 00 <b> 00 CKSUM
    std::vector<uint8_t> f;
    f.reserve(12);
    f.push_back(kFrameMagic);
    const std::array<uint8_t, 11> body{0x2D, 0x08, 0x00, 0x00,
                                        level, 0x00, level, 0x00,
                                        level, 0x00, 0x00};
    (void)body;
    // Actually the payload is 10 bytes after FF: 2D 08 00 00 <b> 00 <b> 00 <b> 00.
    // Let me rebuild without the trailing 0x00.
    f.clear();
    f.push_back(kFrameMagic);
    f.push_back(0x2D);
    f.push_back(0x08);
    f.push_back(0x00);
    f.push_back(0x00);
    f.push_back(level);
    f.push_back(0x00);
    f.push_back(level);
    f.push_back(0x00);
    f.push_back(level);
    f.push_back(0x00);
    std::span<const uint8_t> payload{f.data() + 1, f.size() - 1};
    f.push_back(checksum(payload));
    return f;
}

std::vector<uint8_t> buildLcdBrightness(uint8_t level)
{
    // FF 4F 02 <b> 00 CKSUM
    std::vector<uint8_t> f;
    f.reserve(6);
    f.push_back(kFrameMagic);
    f.push_back(0x4F);
    f.push_back(0x02);
    f.push_back(level);
    f.push_back(0x00);
    std::span<const uint8_t> payload{f.data() + 1, f.size() - 1};
    f.push_back(checksum(payload));
    return f;
}

std::array<std::vector<uint8_t>, 2> buildVuMeter(const std::array<uint8_t, 16>& levels)
{
    // Frame: FF 66 21 <subcmd> <strip0_in><strip0_out> … <strip7_in><strip7_out>
    //   <16 × 00> CKSUM
    // Strip 0 starts at payload byte 1 (right after sub-cmd), no leading
    // padding. The two "00 00" bytes visible in baseline captures were
    // from empty strip 0 + strip 1 inputs, not an intrinsic header.
    auto build = [&](uint8_t subcmd) {
        std::vector<uint8_t> f;
        f.reserve(37);
        f.push_back(kFrameMagic);
        f.push_back(0x66);
        f.push_back(0x21);
        f.push_back(subcmd);
        for (auto b : levels) f.push_back(b);        // 16 bytes: 8 × (in, out)
        for (int i = 0; i < 16; ++i) f.push_back(0x00);
        std::span<const uint8_t> payload{f.data() + 1, f.size() - 1};
        f.push_back(checksum(payload));
        return f;
    };
    return {build(0x09), build(0x0A)};
}

std::vector<uint8_t> buildGrByte(uint8_t grByte)
{
    // FF 66 09 15 <gr> 00×7 CKSUM   (13 bytes: 4 hdr + 8 data + 1 chk = 13)
    // Decoded 2026-04-28 from `dual_35_cs_gr_ramp.pcapng` — single byte
    // ramps 0x02..0x18 across a slow CS GR sweep. Earlier mislabel of
    // `FF 66 11 0F` as the GR frame was wrong; that opcode is actually
    // the Comp-Threshold parameter-readout zone (user observed flicker
    // when our writes were routed there).
    std::vector<uint8_t> f;
    f.reserve(13);
    f.push_back(kFrameMagic);
    f.push_back(0x66);
    f.push_back(0x09);
    f.push_back(0x15);
    f.push_back(grByte);
    for (int i = 0; i < 7; ++i) f.push_back(0x00);
    std::span<const uint8_t> payload{f.data() + 1, f.size() - 1};
    f.push_back(checksum(payload));
    return f;
}

std::vector<uint8_t> buildSelectedStripMask(uint16_t mask)
{
    // FF 66 03 06 <lo> <hi> CKSUM
    std::vector<uint8_t> f;
    f.reserve(7);
    f.push_back(kFrameMagic);
    f.push_back(0x66);
    f.push_back(0x03);
    f.push_back(0x06);
    f.push_back(static_cast<uint8_t>(mask & 0xFF));
    f.push_back(static_cast<uint8_t>((mask >> 8) & 0xFF));
    std::span<const uint8_t> payload{f.data() + 1, f.size() - 1};
    f.push_back(checksum(payload));
    return f;
}

uint8_t selCellForStrip(uint8_t strip)
{
    // From cap30 decode (re-interpreted after 2026-04-24 test): UF8's
    // leftmost strip 0 = cell 0x15; each strip right decrements cell
    // by 3. Original table had strips misread as 1-indexed because the
    // capture's REAPER project had a "TESTCS" track at index 0 before
    // the numbered tracks, so "T1 click" landed on UF8 strip 1 (not 0).
    static constexpr uint8_t kMap[8] = {0x15, 0x12, 0x0F, 0x0C, 0x09, 0x06, 0x03, 0x00};
    return (strip < 8) ? kMap[strip] : 0x00;
}

static std::vector<uint8_t> buildSelFrame(uint8_t cmd, uint8_t cell, uint8_t a, uint8_t b)
{
    std::vector<uint8_t> f;
    f.reserve(8);
    f.push_back(kFrameMagic);
    f.push_back(cmd);
    f.push_back(0x04);
    f.push_back(cell);
    f.push_back(0x00);
    f.push_back(a);
    f.push_back(b);
    std::span<const uint8_t> payload{f.data() + 1, f.size() - 1};
    f.push_back(checksum(payload));
    return f;
}

std::array<std::vector<uint8_t>, 2> buildSelColour(uint8_t strip, uint8_t byteA, uint8_t byteB)
{
    const uint8_t cell = selCellForStrip(strip);
    return {buildSelFrame(0x38, cell, byteA, byteB),
            buildSelFrame(0x39, cell, byteA, byteB)};
}

// Cell + colour table for global UF8 buttons. Decoded in cap35 + cap36.
// Each global LED stores a full LedColour (bright AND dim byte pairs)
// — same encoding as the SEL DAW-Colour palette. Dim mode preserves the
// hue at low intensity instead of falling back to white-dim, so e.g.
// inactive AutoRead stays green, inactive AutoTouch stays yellow.
// Plain-white buttons just store the universal white-dim sentinel.
//
// Bright/dim byte pairs are derived from `kSelPalette` (Protocol.cpp).
namespace {
struct Uf8GlobalLedDef {
    uint8_t   cell;
    LedColour colour;
    bool      legacy = false;  // true → use FF 3B 03 mono frame (Send/Plugin row)
};
// Colour shorthands — dim variants from `kSelPalette` so each colour
// keeps its hue at low intensity rather than falling back to white-dim.
constexpr LedColour kColourWhite  {0xFF, 0xFF, 0x11, 0xF1};
constexpr LedColour kColourGreen  {0xF0, 0xF0, 0x10, 0xF0};
constexpr LedColour kColourRed    {0x0F, 0xF0, 0x01, 0xF0};
constexpr LedColour kColourOrange {0x3F, 0xF0, 0x12, 0xF0};
constexpr LedColour kColourYellow {0xEF, 0xF0, 0x11, 0xF0};
constexpr Uf8GlobalLedDef kUf8GlobalLedTable[] = {
    // Layer / Quick / 360 cells fully confirmed via probe 2026-04-30:
    // physical button order along the row (left → right) is
    //   360 (0x39)  Quick3 (0x3A)  Quick2 (0x3B)  Quick1 (0x3C)
    //   Layer3 (0x3D)  Layer2 (0x3E)  Layer1 (0x3F)
    // — descending button-position vs ascending cell-id. Earlier
    // cap35/36 decoding put Layer 1/2 at 0x39/0x3A which turned out
    // to be 360/Quick3.
    /* Layer1       */ {0x3F, kColourWhite},
    /* Layer2       */ {0x3E, kColourWhite},
    /* Layer3       */ {0x3D, kColourWhite},
    /* Quick1       */ {0x3C, kColourWhite},
    /* Quick2       */ {0x3B, kColourWhite},
    /* Quick3       */ {0x3A, kColourWhite},
    /* Channel      */ {0x2E, kColourWhite},
    /* Btn360       */ {0x39, kColourWhite},
    // Send/Plugin 1..8 row uses the legacy mono-LED path (FF 3B 03 <id>
    // 00 <state>) — verified via probe 2026-04-30. Cells 0x37 (SP1) →
    // 0x30 (SP8), so id descends as button index ascends. The colour-pair
    // FF 38/39 04 family does not address these LEDs at all.
    /* SendPlugin1  */ {0x37, kColourWhite, true},
    /* SendPlugin2  */ {0x36, kColourWhite, true},
    /* SendPlugin3  */ {0x35, kColourWhite, true},
    /* SendPlugin4  */ {0x34, kColourWhite, true},
    /* SendPlugin5  */ {0x33, kColourWhite, true},
    /* SendPlugin6  */ {0x32, kColourWhite, true},
    /* SendPlugin7  */ {0x31, kColourWhite, true},
    /* SendPlugin8  */ {0x30, kColourWhite, true},
    /* Plugin       */ {0x2F, kColourWhite, true},   // 3-state legacy LED
    // Page Left / Page Right both 3-state LEDs (off/dim/bright) requiring
    // colour-pair + legacy mono frames together — verified via cap48
    // 2026-04-30. PageRight at 0x2C was never lit by colour-pair alone in
    // the probe sweeps because SSL360 always primes with the legacy frame.
    /* PageLeft     */ {0x2D, kColourWhite, true},
    /* PageRight    */ {0x2C, kColourWhite, true},
    /* Flip         */ {0x2B, kColourWhite},
    /* AutoOff      */ {0x27, kColourWhite},  // confirmed via probe 2026-04-30
    /* AutoRead     */ {0x26, kColourGreen},
    /* AutoWrite    */ {0x25, kColourRed},
    /* AutoTrim     */ {0x24, kColourOrange},
    /* AutoLatch    */ {0x23, kColourRed},
    /* AutoTouch    */ {0x22, kColourYellow},
    /* VPotBank     */ {0x5F, kColourWhite},
    /* Soft1        */ {0x5E, kColourWhite},
    /* Soft2        */ {0x5D, kColourWhite},
    /* Soft3        */ {0x5C, kColourWhite},
    /* Soft4        */ {0x5B, kColourWhite},
    /* Soft5        */ {0x5A, kColourWhite},
    /* Pan          */ {0x59, kColourWhite},
    /* Fine         */ {0x58, kColourWhite},
    /* Norm         */ {0x57, kColourWhite},
    /* Rec          */ {0x56, kColourRed},
    /* Auto         */ {0x55, kColourWhite},
    /* Nav          */ {0x54, kColourWhite},
    /* Nudge        */ {0x53, kColourWhite},
    /* Focus        */ {0x52, kColourWhite},
    /* BankLeft     */ {0x4F, kColourWhite},
    /* BankRight    */ {0x4E, kColourWhite},
    /* ZoomUp       */ {0x4D, kColourGreen},
    /* ZoomLeft     */ {0x4C, kColourWhite},
    /* ZoomCenter   */ {0x4B, kColourRed},
    /* ZoomRight    */ {0x4A, kColourWhite},
    /* ZoomDown     */ {0x49, kColourYellow},
};
} // namespace

LedColourFrames buildUf8GlobalLed(Uf8GlobalLed btn, GlobalLedState state,
                                  LedColour colour)
{
    const auto& def = kUf8GlobalLedTable[static_cast<size_t>(btn)];
    LedColourFrames out;
    switch (state) {
        case GlobalLedState::Bright:
            out.ff38 = buildSelFrame(0x38, def.cell,
                                     colour.aBright, colour.bBright);
            out.ff39 = buildSelFrame(0x39, def.cell, 0x00, 0xF0);
            if (def.legacy) out.legacy = buildLedCommand(def.cell, true);
            break;
        case GlobalLedState::Dim:
            // Dim mode keeps the button's hue at low intensity. Both FF 38
            // and FF 39 carry the dim pair — same encoding the SEL palette
            // uses for un-lit DAW-Colour LEDs (cap33). Legacy-family LEDs
            // also need their FF 3B 03 frame fired to leave legacy-OFF,
            // otherwise the colour-pair has no visible effect (cap48).
            out.ff38 = buildSelFrame(0x38, def.cell,
                                     colour.aDim, colour.bDim);
            out.ff39 = buildSelFrame(0x39, def.cell,
                                     colour.aDim, colour.bDim);
            if (def.legacy) out.legacy = buildLedCommand(def.cell, true);
            break;
        case GlobalLedState::Off:
            // True off — colour-pair `00 F0` zeros the baseline; legacy
            // mono with state 0x00 turns the LED off cleanly.
            out.ff38 = buildSelFrame(0x38, def.cell, 0x00, 0xF0);
            out.ff39 = buildSelFrame(0x39, def.cell, 0x00, 0xF0);
            if (def.legacy) out.legacy = buildLedCommand(def.cell, false);
            break;
    }
    return out;
}

LedColourFrames buildUf8GlobalLed(Uf8GlobalLed btn, GlobalLedState state)
{
    const auto& def = kUf8GlobalLedTable[static_cast<size_t>(btn)];
    return buildUf8GlobalLed(btn, state, def.colour);
}

// Backwards-compat: bool maps Bright/Dim. Callers needing explicit Off
// must use the GlobalLedState overload.
LedColourFrames buildUf8GlobalLed(Uf8GlobalLed btn, bool on)
{
    return buildUf8GlobalLed(btn,
        on ? GlobalLedState::Bright : GlobalLedState::Dim);
}

std::array<std::vector<uint8_t>, 2> buildSelWhite(uint8_t strip, bool bright)
{
    // From cap30: white-dim = 00 11 F1 (both FF38 and FF39 symmetric).
    // white-bright: only FF 38 fires with 00 FF FF; FF 39 is omitted by SSL.
    // We mirror that behaviour — for `bright` the FF 39 entry is an empty
    // vector the caller should skip-if-empty.
    if (bright) {
        const uint8_t cell = selCellForStrip(strip);
        return {buildSelFrame(0x38, cell, 0xFF, 0xFF), std::vector<uint8_t>{}};
    }
    return buildSelColour(strip, 0x11, 0xF1);
}

bool verifyFrame(std::span<const uint8_t> frame)
{
    if (frame.size() < 3) return false;
    if (frame.front() != kFrameMagic) return false;
    std::span<const uint8_t> payload{frame.data() + 1, frame.size() - 2};
    return checksum(payload) == frame.back();
}

std::optional<ButtonEvent> parseButtonEvent(std::span<const uint8_t> bytes)
{
    // Strip the 31 60 prefix if present (incoming UF8 wire form)
    if (bytes.size() >= 2 && bytes[0] == 0x31 && bytes[1] == 0x60) {
        bytes = bytes.subspan(2);
    }
    // Need exactly: FF 22 03 <id> 00 <state> CKSUM   (7 bytes)
    if (bytes.size() < 7) return std::nullopt;
    if (bytes[0] != kFrameMagic || bytes[1] != 0x22 || bytes[2] != 0x03) return std::nullopt;
    if (bytes[4] != 0x00) return std::nullopt;

    std::span<const uint8_t> payload{bytes.data() + 1, 5};  // 22 03 id 00 state
    if (checksum(payload) != bytes[6]) return std::nullopt;

    return ButtonEvent{bytes[3], bytes[5] == 0x01};
}

} // namespace uf8
