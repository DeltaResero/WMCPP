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

// Helper function for cardioid/period-2 bulb check
static inline bool isInMainCardioidOrBulb(double cr, double ci)
{
    double ci2 = ci * ci;
    // Check cardioid
    double q = (cr - CARD_P1) * (cr - CARD_P1) + ci2;
    if (q * (q + (cr - CARD_P1)) <= CARD_P1 * ci2)
    {
        return true;
    }
    // Check period-2 bulb
    return ((cr + 1.0) * (cr + 1.0) + ci2) <= CARD_P2;
}

void reset(u32 resetCode, void* resetData)
{
  reboot = true;
}

void poweroff()
{
  switchoff = true;
}

static inline u32 fast_reciprocal(u32 a)
{
  // Newton-Raphson iteration for 1/x
  // This gives us about 16 bits of precision
  u32 x = 0x7FFFFFFF / (a | 1);  // Initial guess
  x = x * (0x20000 - ((a * x) >> 16)) >> 15;  // One iteration
  return x;
}

static inline void drawdot(void* xfb, GXRModeObj* rmode, u16 fx, u16 fy, u32 color)
{
  u32* fb = (u32*)xfb;
  const int fbWidthHalf = rmode->fbWidth >> 1;
  const int x = fx >> 1;
  const int y = fy;
  const int x_end = (x + 2 < fbWidthHalf) ? x + 2 : fbWidthHalf - 1;
  const int y_end = (y + 4 < rmode->xfbHeight) ? y + 4 : rmode->xfbHeight - 1;

  int y_start = (y - 4 >= 0) ? y - 4 : 0;
  do
  {
    u32 fbOffset = fbWidthHalf * y_start;
    int x_start = (x - 2 >= 0) ? x - 2 : 0;
    do
    {
      fb[fbOffset + x_start] = color;
    } while (++x_start <= x_end);
  } while (++y_start <= y_end);
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
 * @param n2 First pixel's iteration count
 * @param n1 Second pixel's iteration count
 * @param limit Maximum iteration count
 * @param paletteIndex Current color palette index
 * @return Packed 32-bit YUV value ready for framebuffer
 */
static u32 PackYUVPair(int n2, int n1, int limit, PalettePtr palette)
{
  int y1, cb1, cr1, y2, cb2, cr2;

  if (n2 == limit)
  {
    y1 = 0; cb1 = 128; cr1 = 128;
  }
  else
  {
    const uint8_t* p = palette[n2 & 255];
    y1 = p[0]; cb1 = p[1]; cr1 = p[2];
  }

  if (n1 == limit)
  {
    y2 = 0; cb2 = 128; cr2 = 128;
  }
  else
  {
    const uint8_t* p = palette[n1 & 255];
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

  console_init(xfb[0], 0, 20, rmode->fbWidth, rmode->xfbHeight, rmode->fbWidth * VI_DISPLAY_PIX_SZ);
  VIDEO_ClearFrameBuffer(rmode, xfb[0], COLOR_BLACK);
  VIDEO_ClearFrameBuffer(rmode, xfb[1], COLOR_BLACK);
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
  int n1, w, h, screenWH, screenWHHalf;
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

  do
  {
    bufferIndex = !bufferIndex;
    PalettePtr currentPalette = GetPalettePtr(state.paletteIndex);
    console_init(xfb[bufferIndex], 0, 20, rmode->fbWidth, 20, fbStride);

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

    h = 20;
    do
    {
      screenWH = screenW * h;
      screenWHHalf = (screenW * h) >> 1;

      if (state.process)
      {
        ci = -1.0 * (h - screenH2) * state.zoom - state.centerY;
        state.cachedY[h] = ci;
      }
      else
      {
        ci = state.cachedY[h];
      }

      w = 0;
      do
      {
        if (state.process)
        {
          cr = (w - screenW2) * state.zoom + state.centerX;
          state.cachedX[w] = cr;
        }
        else
        {
          cr = state.cachedX[w];
        }

        if (state.process)
        {
          if (isInMainCardioidOrBulb(cr, ci))
          {
            n1 = state.limit;
          }
          else
          {
            zr = zi = 0;
            n1 = 0;
            zrSquared = zr * zr;
            ziSquared = zi * zi;

            do
            {
              zi = 2 * zr * zi + ci;
              zr = zrSquared - ziSquared + cr;
              zrSquared = zr * zr;
              ziSquared = zi * zi;
              ++n1;
            } while (zrSquared + ziSquared < 4 && n1 != state.limit);
          }
          field[w + screenWH] = n1;
        }
        n1 = field[w + screenW * h] + state.cycle;
        xfb[bufferIndex][(w >> 1) + screenWHHalf] = PackYUVPair(n1, n1, state.limit, currentPalette);
      } while (++w < screenW);
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
            (wd->ir.x - screenW2) * state.zoom + state.centerX,
            (screenH2 - wd->ir.y) * state.zoom - state.centerY);
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
