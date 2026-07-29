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

// The debug strip prints Iter and AvgIterPx four columns wide each, and neither
// can exceed the limit
static_assert(LIMIT_MAX <= 9999, "Iter and AvgIterPx fields are four columns wide");

// Pre-computed constants for cardioid/bulb check
static constexpr double CARD_P1 = 0.25;
static constexpr double CARD_P2 = 0.0625;

// Color constant for points inside the set (Black in YUV: Y=0, U=128, V=128)
static const uint8_t Black[3] = {0, 128, 128};

static u32* xfb[2] = {nullptr, nullptr};
static GXRModeObj* rmode;
static int evctr = 0;
// Written from interrupt context by the reset and power callbacks, so every
// read has to come from memory rather than a register the loop kept
static volatile bool reboot = false;
static volatile bool switchoff = false;
// The buffer's 32-byte alignment comes from aligned_alloc at the call site;
// qualifying the pointer here would only align the pointer itself
static int* field = nullptr;
static u64 lastTime = 0;

// Debug strip readings, held between the frame loop that measures them and the
// display that prints them. The iteration totals describe whatever the field
// currently holds, so they stay put on frames that only repaint it
static u32 lastRenderMicros = 0;
static u64 fieldIterSum = 0;
static u32 fieldIterPixels = 0;

void reset(u32, void*);
void poweroff();

class MandelbrotState
{
public:
  double centerX;
  double centerY;
  double oldX;
  double oldY;
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
  }

  // Passed by reference throughout, so a copy would silently diverge from
  // the state the input handler mutates
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
 * @param cycle Palette rotation offset, applied only to points that escaped
 * @param palette Current color palette pointer
 * @return Packed 32-bit YUV value ready for framebuffer
 */
static inline u32 PackYUVPair(int n1, int n2, int limit, int cycle, PalettePtr palette)
{
  // A count of exactly limit means the point never escaped and belongs to the
  // set, so it stays black and only the escape counts take the rotation
  const uint8_t* p1 = (n1 == limit) ? Black : palette[(n1 + cycle) & 255];
  const uint8_t* p2 = (n2 == limit) ? Black : palette[(n2 + cycle) & 255];

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
 *
 * @return Total iteration count across the row, for the debug strip's average
 */
static u32 renderRow(const MandelbrotState& state, int h, int screenW, double rowCr, double ci, double ciSquared)
{
  int* rowField = field + (screenW * h);
  int w = 0;
  int localLimit = state.limit;
  double localZoom = state.zoom;
  u32 rowSum = 0;

  do
  {
    // Two pixels per pass, so the running coordinate takes one addition per pair
    // instead of one per pixel and accumulates half as much rounding error
    int n1 = computeMandelbrotIteration(rowCr, ci, ciSquared, localLimit);
    int n2 = computeMandelbrotIteration(rowCr + localZoom, ci, ciSquared, localLimit);
    rowField[w] = n1;
    rowField[w + 1] = n2;
    rowSum += static_cast<u32>(n1 + n2);
    w += 2;
    rowCr += 2.0 * localZoom;
  } while (w < screenW);

  return rowSum;
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

  if (localProcess)
  {
    fieldIterSum = 0;
    fieldIterPixels = 0;
  }

  int h = 20; // Fractal rendering starts below the console area
  do
  {
    int screenWH = screenW * h;

    if (localProcess)
    {
      double ci = -1.0 * (h - screenH2) * localZoom - localCenterY;
      double ciSquared = ci * ci; // Calculate once per row
      // Render the row data if processing is needed
      fieldIterSum += renderRow(state, h, screenW, -screenW2 * localZoom + localCenterX, ci, ciSquared);
      fieldIterPixels += static_cast<u32>(screenW);
    }

    // Draw pixels to XFB
    int* rowField = field + screenWH;
    u32* rowXfb = framebuffer + (screenWH >> 1);
    int w = 0;

    do
    {
      // Retrieve iteration counts using pointer arithmetic
      int n1 = rowField[w];
      int n2 = rowField[w + 1];
      // Write to XFB using pointer arithmetic
      rowXfb[w >> 1] = PackYUVPair(n1, n2, localLimit, localCycle, currentPalette);
      w += 2;
    } while (w < screenW);

  } while (++h < screenH);

  if (state.process)
  {
    state.process = false;
  }
}

/**
 * Writes a number right aligned into a field exactly width columns wide, or a
 * "999+" style marker when it will not fit. The console is one row tall and
 * text past its last column wraps onto a row that is not there, so every field
 * on the debug strip has to have a width that cannot grow.
 */
static void fitField(char* out, size_t size, double value, int markerCap, int width, int decimals)
{
  if (snprintf(out, size, "%*.*f", width, decimals, value) > width)
  {
    snprintf(out, size, "%*d+", width - 1, markerCap);
  }
}

/**
 * Prints the debug strip: frame timings, iteration counts, memory, battery
 */
static void printDebugLine(const MandelbrotState& state, const WPADData* wd, u32 frameMicros)
{
  u32 fps = (frameMicros > 0) ? ((1000000u + (frameMicros >> 1)) / frameMicros) : 0;
  u32 avgIterPx = (fieldIterPixels > 0) ? static_cast<u32>(fieldIterSum / fieldIterPixels) : 0;

  char fpsText[8];
  char renderText[12];
  char freeText[12];
  fitField(fpsText, sizeof(fpsText), fps, 999, 4, 0);
  fitField(renderText, sizeof(renderText), lastRenderMicros / 1000.0, 9999, 6, 1);
  fitField(freeText, sizeof(freeText), SYS_GetArena1Size() / (1024.0 * 1024.0), 999, 5, 1);

  printf(" FPS:%s RenTime:%sms Iter:%4d AvgIterPx:%4u FreeMem:%sMB Bat:%3u",
    fpsText, renderText, state.limit, avgIterPx, freeText,
    static_cast<unsigned>(wd ? wd->battery_level : 0));
}

/**
 * Prints the normal strip: view centre, zoom, and the cursor's coordinate
 */
static void printCoordinateLine(const MandelbrotState& state, const WPADData* wd, int screenW2, int screenH2)
{
  printf(" cX:%.8f cY:%.8f", state.centerX, state.centerY == -0.0 ? 0.0 : -state.centerY);
  printf("  zoom:%.4e ", INITIAL_ZOOM / state.zoom);

  // Display cursor coordinates if IR is valid
  if (wd && wd->ir.valid)
  {
    printf(" re:%.8f im:%.8f",
      (wd->ir.x - screenW2) * state.zoom + state.centerX,
      (screenH2 - wd->ir.y) * state.zoom - state.centerY);
  }
  else if (wd)
  {
    printf(" No Cursor");
  }
}

/**
 * Updates the display with coordinate information
 */
static void updateDisplay(const MandelbrotState& state, const WPADData* wd, int screenW2, int screenH2)
{
  // Sampled every frame rather than only in debug mode, so the first frame
  // after the toggle measures one frame instead of the whole time it was off
  u64 currentTime = gettime();
  u32 frameMicros = static_cast<u32>(ticks_to_microsecs(currentTime - lastTime));
  lastTime = currentTime;

  if (state.debugMode)
  {
    printDebugLine(state, wd, frameMicros);
  }
  else
  {
    printCoordinateLine(state, wd, screenW2, screenH2);
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

/**
 * Reports an unrecoverable startup failure and hands control back to the
 * loader once the user has acknowledged it. The console strip is a single
 * text row, so the caller's message has to share one line with the prompt.
 */
static void fatalError(const char* message)
{
  printf(" %s  Press HOME to exit.", message);

  // Honor the console's own RESET and POWER buttons too, since a failure
  // this early may mean no Wii Remote has been paired yet
  while (!reboot)
  {
    VIDEO_WaitVSync();
    WPAD_ScanPads();

    if (WPAD_ButtonsDown(0) & WPAD_BUTTON_HOME)
    {
      break;
    }

    if (switchoff)
    {
      shutdown_system();
      SYS_ResetSystem(SYS_POWEROFF, 0, false);
    }
  }

  shutdown_system();
  SYS_ResetSystem(SYS_RETURNTOMENU, 0, 0);
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

  // Both palette handlers below also fire on this chord. They step down and
  // then back up, which cancels for any palette count, so debug mode toggles
  // without disturbing the palette
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
    // Clamp the doubled value rather than the value going in. Halving drops the
    // low bit, so 25 becomes 12 and the ladder leaves the powers of two that
    // divide LIMIT_MAX. Testing before the shift lets 3072 double clean past it
    const int doubled = state.limit << 1;
    state.limit = (doubled < LIMIT_MAX) ? doubled : LIMIT_MAX;
    state.process = true;
  }

  if (wd->btns_d & WPAD_BUTTON_MINUS)
  {
    state.paletteIndex = (state.paletteIndex > 0) ? (state.paletteIndex - 1) : (GetPaletteCount() - 1);
  }

  if (wd->btns_d & WPAD_BUTTON_PLUS)
  {
    state.paletteIndex = (state.paletteIndex + 1) % GetPaletteCount();
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

  if (!field)
  {
    fatalError("Not enough memory for the iteration buffer.");
    return 1;
  }

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

    u64 renderStart = gettime();
    renderMandelbrot(state, xfb[bufferIndex], currentPalette, screenW, screenH, screenW >> 1, screenH >> 1);
    lastRenderMicros = static_cast<u32>(ticks_to_microsecs(gettime() - renderStart));

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
