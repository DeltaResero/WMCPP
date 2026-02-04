// src/main.cpp
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

#include "palettes.hpp"

#include <algorithm> // For std::min, std::max
#include <cstdio>
#include <cstdlib>
#include <malloc.h>
#include <ogcsys.h>
#include <gccore.h>
#include <wiiuse/wpad.h>
#include <ogc/lwp_watchdog.h>

// Aligned buffer sizes for DMA transfers
#define ALIGN32(x) (((x) + 31) & ~31)

static constexpr double INITIAL_ZOOM = 0.007;
static constexpr int INITIAL_LIMIT = 200;
static constexpr int LIMIT_MAX = 3200;
static constexpr double MAX_ZOOM_PRECISION = 1e-14;

// Pre-computed constants for cardioid/bulb check
static constexpr double CARD_P1 = 0.25;
static constexpr double CARD_P2 = 0.0625;

// Color constant for points inside the set (Black in YUV: Y=0, U=128, V=128)
static const uint8_t Black[3] = {0, 128, 128};

static u32* xfb[2] = {nullptr, nullptr};
static GXRModeObj* rmode;
static int evctr = 0;
static bool reboot = false;
static bool switchoff = false;
static int* __attribute__((aligned(32))) field = nullptr;
static u64 lastTime = 0;

void reset(u32, void*);
void poweroff();

class MandelbrotState
{
public:
  double* __attribute__((aligned(32))) cachedX;
  double* __attribute__((aligned(32))) cachedY;
  double __attribute__((aligned(32))) centerX;
  double __attribute__((aligned(32))) centerY;
  double __attribute__((aligned(32))) oldX;
  double __attribute__((aligned(32))) oldY;
  int mouseX;
  int mouseY;
  int limit;
  uint8_t paletteIndex;
  double zoom;
  bool process;
  bool cycling;
  int cycle;
  bool debugMode;

  MandelbrotState()
  {
    centerX = 0;
    centerY = 0;
    oldX = 0;
    oldY = 0;
    mouseX = 0;
    mouseY = 0;
    limit = INITIAL_LIMIT;
    paletteIndex = 4;
    zoom = INITIAL_ZOOM;
    process = true;
    cycling = false;
    cycle = 0;
    debugMode = false;
    // Align arrays to 32-byte boundary for DMA
    cachedX = static_cast<double*>(aligned_alloc(32, ALIGN32(sizeof(double) * rmode->fbWidth)));
    cachedY = static_cast<double*>(aligned_alloc(32, ALIGN32(sizeof(double) * rmode->xfbHeight)));
  }

  ~MandelbrotState()
  {
    free(cachedX);
    free(cachedY);
  }

  // Prevent copying to avoid double-free of cached arrays
  MandelbrotState(const MandelbrotState&) = delete;
  MandelbrotState& operator=(const MandelbrotState&) = delete;

  inline void moveView(int screenW2, int screenH2)
  {
    centerX = mouseX * zoom - screenW2 * zoom + oldX;
    oldX = centerX;
    centerY = mouseY * zoom - screenH2 * zoom + oldY;
    oldY = centerY;
    process = true;
  }

  inline void zoomView(int screenW2, int screenH2)
  {
    moveView(screenW2, screenH2);
    zoom *= 0.35;
    if (zoom < MAX_ZOOM_PRECISION)
    {
      zoom = MAX_ZOOM_PRECISION;
    }
    process = true;
  }
};

void reset(u32 resetCode, void* resetData)
{
  reboot = true;
}

void poweroff()
{
  switchoff = true;
}

/**
 * Packs two adjacent pixels' YUV values into the Wii's native framebuffer format.
 * The Wii uses an interleaved YUV format where two pixels share chrominance (U,V)
 * values to save memory bandwidth. The resulting 32-bit value contains two Y
 * (luminance) values with shared U and V components between adjacent pixels.
 *
 * @param n1 First pixel's iteration count
 * @param n2 Second pixel's iteration count
 * @param limit Maximum iteration count
 * @param palette Current color palette pointer
 * @return Packed 32-bit YUV value ready for framebuffer
 */
static inline u32 PackYUVPair(int n1, int n2, int limit, PalettePtr palette)
{
  // Branchless selection: if n == limit, point to Black, otherwise point to palette color
  const uint8_t* p1 = (n1 == limit) ? Black : palette[n1 & 255];
  const uint8_t* p2 = (n2 == limit) ? Black : palette[n2 & 255];

  // Pack Y1, Average U, Y2, Average V
  return (p1[0] << 24) | ((p1[1] + p2[1]) >> 1 << 16) | (p2[0] << 8) | ((p1[2] + p2[2]) >> 1);
}

/**
 * Checks if a point is inside the main Cardioid or the period-2 Bulb.
 * Extracted to reduce cyclomatic complexity of the main compute function.
 */
static inline bool isInsideCardioidOrBulb(double cr, double ciSquared)
{
  // q = (x - 1/4)^2 + y^2
  double q = (cr - CARD_P1) * (cr - CARD_P1) + ciSquared;

  // Cardioid: q * (q + (x - 1/4)) <= 1/4 * y^2
  if (q * (q + (cr - CARD_P1)) <= CARD_P1 * ciSquared)
  {
    return true;
  }
  // Period-2 Bulb: (x + 1)^2 + y^2 <= 1/16
  if (((cr + 1.0) * (cr + 1.0) + ciSquared) <= CARD_P2)
  {
    return true;
  }

  return false;
}

/**
 * Computes the iteration count for a single Mandelbrot pixel
 */
static inline int computeMandelbrotIteration(double cr, double ci, double ciSquared, int localLimit)
{
  // Inlined Cardioid/Bulb check using pre-calculated ciSquared
  if (isInsideCardioidOrBulb(cr, ciSquared))
  {
    return localLimit;
  }

  double zr = 0;
  double zi = 0;
  int n = 0;
  double zrSquared = 0;
  double ziSquared = 0;

  double checkZr = 0;
  double checkZi = 0;
  int updateInterval = 1;
  int count = 0;

  do
  {
    zi = (zr + zr) * zi + ci;
    zr = zrSquared - ziSquared + cr;
    zrSquared = zr * zr;
    ziSquared = zi * zi;
    ++n;

    if (zr == checkZr && zi == checkZi)
    {
      return localLimit;
    }

    if (++count >= updateInterval)
    {
      checkZr = zr;
      checkZi = zi;
      count = 0;
      updateInterval <<= 1;
      if (updateInterval > 128)
      {
        updateInterval = 128;
      }
    }
  } while (zrSquared + ziSquared < 4 && n != localLimit);

  return n;
}

/**
 * Renders a single row of the Mandelbrot set.
 * Extracted to reduce line count of renderMandelbrot.
 */
static void renderRow(MandelbrotState& state, int h, int screenW, double rowCr, double ci, double ciSquared)
{
  int* rowField = field + (screenW * h);
  int w = 0;
  int localLimit = state.limit;
  double localZoom = state.zoom;

  do
  {
    // Unrolled loop for two pixels to maximize register usage and minimize branching
    for (int i = 0; i < 2; ++i)
    {
      int currentW = w + i;
      // Use incremental addition instead of multiplication: cr = rowCr + (i * localZoom)
      double cr = rowCr;
      if (i == 1)
      {
        cr += localZoom;
      }

      state.cachedX[currentW] = cr;

      // Compute Mandelbrot iteration count for this pixel
      int n1 = computeMandelbrotIteration(cr, ci, ciSquared, localLimit);

      // Use pointer arithmetic: rowField[index] instead of field[index + offset]
      rowField[currentW] = n1;
    }
    w += 2;
    // Increment the base X coordinate for the next pair of pixels
    rowCr += 2.0 * localZoom;
  } while (w < screenW);
}


/**
 * Renders the Mandelbrot set to the framebuffer
 */
static void renderMandelbrot(
  MandelbrotState& state,
  u32* framebuffer,
  PalettePtr currentPalette,
  int screenW,
  int screenH,
  int screenW2,
  int screenH2)
{
  // Cache state variables locally to allow the compiler to use registers
  const int localLimit = state.limit;
  const double localZoom = state.zoom;
  const double localCenterX = state.centerX;
  const double localCenterY = state.centerY;
  const bool localProcess = state.process;
  const int localCycle = state.cycle;

  int h = 20; // Fractal rendering starts below the console area
  do
  {
    int screenWH = screenW * h;
    double ciSquared = 0;
    double ci;

    if (localProcess)
    {
      ci = -1.0 * (h - screenH2) * localZoom - localCenterY;
      state.cachedY[h] = ci;
      ciSquared = ci * ci; // Calculate once per row
      // Render the row data if processing is needed
      renderRow(state, h, screenW, -screenW2 * localZoom + localCenterX, ci, ciSquared);
    }

    // Draw pixels to XFB
    int* rowField = field + screenWH;
    u32* rowXfb = framebuffer + (screenWH >> 1);
    int w = 0;

    do
    {
      // Retrieve iteration counts using pointer arithmetic
      int n1 = rowField[w] + localCycle;
      int n2 = rowField[w + 1] + localCycle;
      // Write to XFB using pointer arithmetic
      rowXfb[w >> 1] = PackYUVPair(n1, n2, localLimit, currentPalette);
      w += 2;
    } while (w < screenW);

  } while (++h < screenH);

  if (state.process)
  {
    state.process = false;
  }
}

/**
 * Updates the display with coordinate information
 */
static void updateDisplay(const MandelbrotState& state, const WPADData* wd, int screenW2, int screenH2)
{
  if (state.debugMode)
  {
    u64 currentTime = gettime();
    u32 frameTime = (u32)((currentTime - lastTime) * 1000 / TB_TIMER_CLOCK);
    lastTime = currentTime;

    struct mallinfo mi = mallinfo();
    float memused = mi.uordblks / (1024.0f * 1024.0f);

    printf(" Frame Time:%d Mem: %.1fMB Iter: %d", frameTime, memused, state.limit);
  }
  else
  {
    printf(" cX:%.8f cY:%.8f", state.centerX, state.centerY == -0.0 ? 0.0 : -state.centerY);
    printf("  zoom:%.4e ", INITIAL_ZOOM / state.zoom);
  }

  // Display cursor coordinates if IR is valid
  if (wd && wd->ir.valid && !state.debugMode)
  {
    printf(" re:%.8f im:%.8f",
      (wd->ir.x - screenW2) * state.zoom + state.centerX,
      (screenH2 - wd->ir.y) * state.zoom - state.centerY);
  }
  else if (wd && !state.debugMode)
  {
    printf(" No Cursor");
  }
}

static void drawdot(void* xfb, GXRModeObj* rmode, int cx, int cy, u32 color)
{
  u32* fb = static_cast<u32*>(xfb);
  const int fbWidthHalf = rmode->fbWidth >> 1;
  const int height = rmode->xfbHeight;

  // Cursor dimensions (approx 5x9 pixels)
  const int rx = 2;
  const int ry = 4;

  // Use std::max/min to clamp values without branching (reduces complexity)
  int x_start = std::max(0, (cx >> 1) - rx);
  int x_end = std::min(fbWidthHalf - 1, (cx >> 1) + rx);
  int y_start = std::max(0, cy - ry);
  int y_end = std::min(height - 1, cy + ry);

  // Early exit if cursor is entirely off-screen
  if (x_start > x_end || y_start > y_end)
  {
    return;
  }

  // Draw using pointer arithmetic
  u32* row = fb + (y_start * fbWidthHalf);
  for (int y = y_start; y <= y_end; ++y)
  {
    for (int x = x_start; x <= x_end; ++x)
    {
      row[x] = color;
    }
    row += fbWidthHalf;
  }
}

static void countevs(int chan, const WPADData* data)
{
  ++evctr;
}

static void cleanup_field()
{
  free(field);
  field = nullptr;
}

static void shutdown_system()
{
  cleanup_field();
  if (xfb[0])
  {
    free(MEM_K1_TO_K0(xfb[0]));
    xfb[0] = nullptr;
  }
  if (xfb[1])
  {
    free(MEM_K1_TO_K0(xfb[1]));
    xfb[1] = nullptr;
  }
}

static void init()
{
  VIDEO_Init();
  WPAD_Init();
  SYS_SetResetCallback(reset);
  SYS_SetPowerCallback(poweroff);

  switch (VIDEO_GetCurrentTvMode())
  {
    case VI_NTSC:
      rmode = &TVNtsc480IntDf;
      break;
    case VI_PAL:
      rmode = &TVPal528IntDf;
      break;
    case VI_MPAL:
      rmode = &TVMpal480IntDf;
      break;
    default:
      rmode = &TVNtsc480IntDf;
  }

  VIDEO_Configure(rmode);
  xfb[0] = static_cast<u32*>(MEM_K0_TO_K1(SYS_AllocateFramebuffer(rmode)));
  xfb[1] = static_cast<u32*>(MEM_K0_TO_K1(SYS_AllocateFramebuffer(rmode)));

  const int fbStride = ((rmode->fbWidth * VI_DISPLAY_PIX_SZ) + 31) & ~31;
  int console_x = 4;
  int console_y = 0;
  int console_w = rmode->fbWidth - (console_x * 2);
  int console_h = 20;

  VIDEO_ClearFrameBuffer(rmode, xfb[0], COLOR_BLACK);
  VIDEO_ClearFrameBuffer(rmode, xfb[1], COLOR_BLACK);

  console_init(xfb[0], console_x, console_y, console_w, console_h, fbStride);

  VIDEO_SetNextFramebuffer(xfb[0]);
  VIDEO_SetBlack(0);
  VIDEO_Flush();
  VIDEO_WaitVSync();

  if (rmode->viTVMode & VI_NON_INTERLACE)
  {
    VIDEO_WaitVSync();
  }

  WPAD_SetDataFormat(0, WPAD_FMT_BTNS_ACC_IR);
  WPAD_SetVRes(0, rmode->fbWidth, rmode->xfbHeight);
}

/**
 * Input Handler
 */
static bool handleInput(MandelbrotState& state, const WPADData* wd, int screenW2, int screenH2)
{
  if (!wd)
  {
    return false;
  }

  if ((wd->btns_d & WPAD_BUTTON_MINUS) && (wd->btns_d & WPAD_BUTTON_PLUS))
  {
    state.debugMode = !state.debugMode;
  }

  if (wd->btns_d & WPAD_BUTTON_A)
  {
    state.mouseX = wd->ir.x;
    state.mouseY = wd->ir.y;
    state.zoomView(screenW2, screenH2);
  }

  if (wd->btns_d & WPAD_BUTTON_B)
  {
    state.zoom = INITIAL_ZOOM;
    state.centerX = state.centerY = state.oldX = state.oldY = 0;
    state.process = true;
  }

  if (wd->btns_d & WPAD_BUTTON_DOWN)
  {
    state.cycling = !state.cycling;
  }

  if (wd->btns_d & WPAD_BUTTON_2)
  {
    state.limit = (state.limit > 1) ? (state.limit >> 1) : 1;
    state.process = true;
  }

  if (wd->btns_d & WPAD_BUTTON_1)
  {
    state.limit = (state.limit < LIMIT_MAX) ? (state.limit << 1) : LIMIT_MAX;
    state.process = true;
  }

  if (wd->btns_d & WPAD_BUTTON_MINUS)
  {
    state.paletteIndex = (state.paletteIndex > 0) ? (state.paletteIndex - 1) : 9;
  }

  if (wd->btns_d & WPAD_BUTTON_PLUS)
  {
    state.paletteIndex = (state.paletteIndex + 1) % 10;
  }

  return ((wd->btns_d & WPAD_BUTTON_HOME) || reboot);
}

int main(int argc, char** argv)
{
  init();
  std::atexit(cleanup_field);
  lastTime = gettime();

  const int screenW = (rmode->fbWidth + 31) & ~31;
  const int screenH = rmode->xfbHeight;
  const int fbStride = ((rmode->fbWidth * VI_DISPLAY_PIX_SZ) + 31) & ~31;
  field = static_cast<int*>(aligned_alloc(32, ALIGN32(sizeof(int) * screenW * screenH)));

  MandelbrotState state;
  bool bufferIndex = 0;
  u32 type;

  do
  {
    bufferIndex = !bufferIndex;
    PalettePtr currentPalette = GetPalettePtr(state.paletteIndex);

    // Clear the top 20 pixels of the current buffer to prevent text smearing
    for (int i = 0; i < (screenW * 20) >> 1; i++)
    {
      xfb[bufferIndex][i] = COLOR_BLACK;
    }
    console_init(xfb[bufferIndex], 4, 0, rmode->fbWidth - 8, 20, fbStride);

    renderMandelbrot(state, xfb[bufferIndex], currentPalette, screenW, screenH, screenW >> 1, screenH >> 1);

    if (state.cycling)
    {
      ++state.cycle;
    }

    WPAD_ReadPending(WPAD_CHAN_ALL, countevs);
    WPADData* wd = (WPAD_Probe(0, &type) == WPAD_ERR_NONE) ? WPAD_Data(0) : nullptr;

    updateDisplay(state, wd, screenW >> 1, screenH >> 1);

    if (wd && wd->ir.valid)
    {
      drawdot(xfb[bufferIndex], rmode, static_cast<int>(wd->ir.x), static_cast<int>(wd->ir.y), COLOR_RED);
    }

    if (handleInput(state, wd, screenW >> 1, screenH >> 1))
    {
      shutdown_system();
      SYS_ResetSystem(SYS_RETURNTOMENU, 0, 0);
      return 0;
    }

    VIDEO_SetNextFramebuffer(xfb[bufferIndex]);
    VIDEO_Flush();
    VIDEO_WaitVSync();

    if (switchoff)
    {
      shutdown_system();
      SYS_ResetSystem(SYS_POWEROFF, 0, false);
    }
  } while (true);

  return 0;
}

// EOF
