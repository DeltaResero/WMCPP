// palettes.hpp

#ifndef PALETTES_HPP
#define PALETTES_HPP

#include <cstdint>

// Define a type for a pointer to a palette array (256 entries of 3 bytes)
typedef const uint8_t (*PalettePtr)[3];

// Returns the pointer to the requested palette data
PalettePtr GetPalettePtr(uint8_t paletteIndex);

// Function prototype for Palette (YUV: Y = luminance, U = blue chrominance, V = red chrominance)
void Palette(uint8_t paletteIndex, int iterations, int* y, int* u, int* v);

#endif // PALETTES_HPP

// EOF
