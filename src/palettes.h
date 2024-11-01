// palettes.h

#ifndef PALETTES_H
#define PALETTES_H

#include <stdint.h>

// Function prototype for Palette (YUV: Y = luminance, U = blue chrominance, V = red chrominance)
void Palette(uint8_t paletteIndex, int iterations, int *y, int *u, int *v);

#endif // PALETTES_H

// EOF
