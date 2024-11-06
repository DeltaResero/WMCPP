// palettes.hpp

#ifndef PALETTES_HPP
#define PALETTES_HPP

#include <cstdint>

// Function prototype for Palette (YUV: Y = luminance, U = blue chrominance, V = red chrominance)
void Palette(uint8_t paletteIndex, int iterations, int* y, int* u, int* v);

#endif // PALETTES_HPP

// EOF
