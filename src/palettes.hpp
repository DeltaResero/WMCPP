// src/palettes.hpp
// SPDX-License-Identifier: GPL-3.0-or-later
//
// WMCPP (Wii Mandelbrot Computation Project Plus)
// Copyright (C) 2025 DeltaResero
// Portions Copyright (C) 2011 Krupkat <krupkat@seznam.cz>
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

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
