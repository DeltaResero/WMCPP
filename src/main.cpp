// main.cpp

#include <cstdio>
#include <cstdlib>
#include <malloc.h>
#include <ogcsys.h>
#include <gccore.h>
#include <wiiuse/wpad.h>
#include <ogc/lwp_watchdog.h>
#include "palettes.hpp"

// Aligned buffer sizes for DMA transfers
#define ALIGN32(x) (((x) + 31) & ~31)

static constexpr double INITIAL_ZOOM = 0.007;
static constexpr int INITIAL_LIMIT = 200;
static constexpr int LIMIT_MAX = 3200;
static constexpr double MAX_ZOOM_PRECISION = 1e-14;

// Pre-computed constants for cardioid/bulb check
static constexpr double CARD_P1 = 0.25;
static constexpr double CARD_P2 = 0.0625;

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
    cachedX = static_cast<double*>(memalign(32, ALIGN32(sizeof(double) * rmode->fbWidth)));
    cachedY = static_cast<double*>(memalign(32, ALIGN32(sizeof(double) * rmode->xfbHeight)));
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

static void drawdot(void* xfb, GXRModeObj* rmode, int cx, int cy, u32 color)
{
  u32* fb = static_cast<u32*>(xfb);
  const int fbWidthHalf = rmode->fbWidth >> 1;
  const int height = rmode->xfbHeight;

  // Cursor dimensions (approx 5x9 pixels)
  const int rx = 2;
  const int ry = 4;

  // Calculate bounds
  int x_start = (cx >> 1) - rx;
  int x_end = (cx >> 1) + rx;
  int y_start = cy - ry;
  int y_end = cy + ry;

  // Clamp to screen edges
  if (x_start < 0) x_start = 0;
  if (x_end >= fbWidthHalf) x_end = fbWidthHalf - 1;
  if (y_start < 0) y_start = 0;
  if (y_end >= height) y_end = height - 1;

  // Early exit if cursor is entirely off-screen
  if (x_start > x_end || y_start > y_end) return;

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
static u32 PackYUVPair(int n1, int n2, int limit, PalettePtr palette)
{
  int y1, cb1, cr1, y2, cb2, cr2;

  if (n1 == limit)
  {
    y1 = 0; cb1 = 128; cr1 = 128;
  }
  else
  {
    const uint8_t* p = palette[n1 & 255];
    y1 = p[0]; cb1 = p[1]; cr1 = p[2];
  }

  if (n2 == limit)
  {
    y2 = 0; cb2 = 128; cr2 = 128;
  }
  else
  {
    const uint8_t* p = palette[n2 & 255];
    y2 = p[0]; cb2 = p[1]; cr2 = p[2];
  }

  return (y1 << 24) | ((cb1 + cb2) >> 1 << 16) | (y2 << 8) | ((cr1 + cr2) >> 1);
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

int main(int argc, char** argv)
{
  init();
  std::atexit(cleanup_field);
  lastTime = gettime();

  double cr, ci, zr, zi, zrSquared, ziSquared;
  int n1, n2, w, h, screenWH, screenWHHalf;
  u32 type;
  WPADData* wd;

  const int screenW = (rmode->fbWidth + 31) & ~31;
  const int screenH = rmode->xfbHeight;
  const int fbStride = ((rmode->fbWidth * VI_DISPLAY_PIX_SZ) + 31) & ~31;
  field = static_cast<int*>(memalign(32, ALIGN32(sizeof(int) * screenW * screenH)));
  const int screenW2 = screenW >> 1;
  const int screenH2 = screenH >> 1;

  MandelbrotState state;
  bool bufferIndex = 0;

  const int console_x = 4;
  const int console_y = 0;
  const int console_w = rmode->fbWidth - (console_x * 2);

  do
  {
    bufferIndex = !bufferIndex;
    PalettePtr currentPalette = GetPalettePtr(state.paletteIndex);

    // Clear the top 20 pixels of the current buffer to prevent text smearing
    for (int i = 0; i < (screenW * 20) >> 1; i++) {
        xfb[bufferIndex][i] = COLOR_BLACK;
    }

    console_init(xfb[bufferIndex], console_x, console_y, console_w, 20, fbStride);

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

    // Cache state variables locally to allow the compiler to use registers
    const int localLimit = state.limit;
    const double localZoom = state.zoom;
    const double localCenterX = state.centerX;
    const double localCenterY = state.centerY;
    const bool localProcess = state.process;
    const int localCycle = state.cycle;

    h = 20; // Fractal rendering starts below the console area
    do
    {
      screenWH = screenW * h;
      screenWHHalf = (screenW * h) >> 1;

      // Variables hoisted out of the pixel loop
      double ciSquared = 0;

      if (localProcess)
      {
        ci = -1.0 * (h - screenH2) * localZoom - localCenterY;
        state.cachedY[h] = ci;
        ciSquared = ci * ci; // Calculate once per row
      }
      else
      {
        ci = state.cachedY[h];
        ciSquared = ci * ci;
      }

      w = 0;
      do
      {
        if (localProcess)
        {
          // Unrolled loop for two pixels to maximize register usage and minimize branching
          for (int i = 0; i < 2; ++i)
          {
            int currentW = w + i;
            cr = (currentW - screenW2) * localZoom + localCenterX;
            state.cachedX[currentW] = cr;

            // Inlined Cardioid/Bulb check using pre-calculated ciSquared
            // q = (x - 1/4)^2 + y^2
            double q = (cr - CARD_P1) * (cr - CARD_P1) + ciSquared;

            // Cardioid: q * (q + (x - 1/4)) <= 1/4 * y^2
            // Period-2 Bulb: (x + 1)^2 + y^2 <= 1/16
            if ((q * (q + (cr - CARD_P1)) <= CARD_P1 * ciSquared) ||
                (((cr + 1.0) * (cr + 1.0) + ciSquared) <= CARD_P2))
            {
              n1 = localLimit;
            }
            else
            {
              zr = zi = 0;
              n1 = 0;
              zrSquared = zr * zr;
              ziSquared = zi * zi;

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
                ++n1;

                if (zr == checkZr && zi == checkZi)
                {
                  n1 = localLimit;
                  break;
                }

                if (++count >= updateInterval)
                {
                  checkZr = zr;
                  checkZi = zi;
                  count = 0;
                  updateInterval <<= 1;
                  if (updateInterval > 128) updateInterval = 128;
                }
              } while (zrSquared + ziSquared < 4 && n1 != localLimit);
            }
            field[currentW + screenWH] = n1;
          }
        }

        n1 = field[w + screenWH] + localCycle;
        n2 = field[w + 1 + screenWH] + localCycle;
        xfb[bufferIndex][(w >> 1) + screenWHHalf] = PackYUVPair(n1, n2, localLimit, currentPalette);

        w += 2;
      } while (w < screenW);
    } while (++h < screenH);

    if (state.process)
    {
      state.process = false;
    }

    if (state.cycling)
    {
      ++state.cycle;
    }

    WPAD_ReadPending(WPAD_CHAN_ALL, countevs);
    if (WPAD_Probe(0, &type) == WPAD_ERR_NONE)
    {
      wd = WPAD_Data(0);
      if (wd->ir.valid)
      {
        if (!state.debugMode)
        {
          printf(" re:%.8f im:%.8f",
            (wd->ir.x - screenW2) * localZoom + localCenterX,
            (screenH2 - wd->ir.y) * localZoom - localCenterY);
        }
        drawdot(xfb[bufferIndex], rmode, static_cast<u16>(wd->ir.x), static_cast<u16>(wd->ir.y), COLOR_RED);
      }
      else if (!state.debugMode)
      {
        printf(" No Cursor");
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

      if ((wd->btns_d & WPAD_BUTTON_HOME) || reboot)
      {
        shutdown_system();
        SYS_ResetSystem(SYS_RETURNTOMENU, 0, 0);
        return 0;
      }
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
