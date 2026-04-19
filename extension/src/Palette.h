#pragma once
//
// REAPER RGB -> UF8 palette index.
//
// The UF8 quantizes to a small palette (16 entries, 0x00..0x0F hypothesis).
// We replicate that mapping here. For entries we've observed in captures we
// use their exact RGB. For the 5 unmapped entries (0x00, 0x01, 0x05, 0x0D,
// 0x0F) we include TBD stubs — nearest-match still works, but until we
// measure them they may be attributed incorrectly.
//

#include <array>
#include <cstdint>
#include <optional>

namespace uf8 {

struct Rgb {
    uint8_t r, g, b;
};

// Map REAPER track color (24-bit RGB as 0xRRGGBB, low 24 bits) to a UF8
// palette index in 0..15. Uses nearest-match in RGB space against the
// reference palette below.
uint8_t quantize(Rgb color);

// Exposed for tests and for the case where a caller already has an 0xRRGGBB
// value from REAPER's GetTrackColor (which returns 0x01000000 | 0x00RRGGBB).
uint8_t quantize(uint32_t rgb24);

// Reference palette: what each confirmed index looks like. Entries that are
// still unmapped (0x00, 0x01, 0x05, 0x0D, 0x0F) return std::nullopt.
std::optional<Rgb> paletteEntry(uint8_t index);

} // namespace uf8
