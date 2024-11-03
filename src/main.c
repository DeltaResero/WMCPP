// main.c

#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>
#include <ogcsys.h>
#include <gccore.h>
#include <wiiuse/wpad.h>
#include "palettes.h"

const double INITIAL_ZOOM = 0.007;
const int INITIAL_LIMIT = 200;
const int LIMIT_MAX = 3200;
const double MAX_ZOOM_PRECISION = 1e-14;

static u32 *xfb[2] = { NULL, NULL };
static GXRModeObj *rmode;
int evctr = 0;
bool reboot = false, switchoff = false;
int *field = NULL;

void reset()
{
  reboot = true;
}

void poweroff()
{
  switchoff = true;
}

static void init();
u32 CvtYUV(int n2, int n1, int limit, uint8_t paletteIndex);

static inline u32 fast_reciprocal(u32 a)
{
  // Newton-Raphson iteration for 1/x
  // This gives us about 16 bits of precision
  u32 x = 0x7FFFFFFF / (a | 1);  // Initial guess
  x = x * (0x20000 - ((a * x) >> 16)) >> 15;  // One iteration
  return x;
}

static inline void drawdot(void *xfb, GXRModeObj *rmode, u16 fx, u16 fy, u32 color)
{
  u32 *fb = (u32 *)xfb;
  const u32 fbWidthHalf = rmode->fbWidth >> 1;
  const u32 fbStride = fbWidthHalf; // Since VI_DISPLAY_PIX_SZ is 2

  const int x = fx >> 1;
  const int y = fy;

  const int y_start = (y - 4 >= 0) ? y - 4 : 0;
  const int y_end = (y + 4 < rmode->xfbHeight) ? y + 4 : rmode->xfbHeight - 1;
  const int x_start = (x - 2 >= 0) ? x - 2 : 0;
  const int x_end = (x + 2 < fbWidthHalf) ? x + 2 : fbWidthHalf - 1;

  int py = y_start;
  do
  {
    u32 fbOffset = fbStride * py;
    int px = x_start;

    do
    {
      fb[fbOffset + px] = color;
      px++;
    } while (px <= x_end);
    py++;
  } while (py <= y_end);
}

void countevs(int chan, const WPADData *data)
{
  evctr++;
}

void cleanup_field()
{
  free(field);
  field = NULL;
}

void shutdown_system()
{
  cleanup_field();
  if (xfb[0])
  {
    free(MEM_K1_TO_K0(xfb[0]));
    xfb[0] = NULL;
  }
  if (xfb[1])
  {
    free(MEM_K1_TO_K0(xfb[1]));
    xfb[1] = NULL;
  }
}

int main(int argc, char **argv)
{
  init();
  atexit(cleanup_field);

  int res;
  u32 type;
  WPADData *wd;

  const int screenW = rmode->fbWidth;
  const int screenH = rmode->xfbHeight;
  const int fbStride = rmode->fbWidth * VI_DISPLAY_PIX_SZ;
  field = (int *)memalign(32, sizeof(int) * screenW * screenH);

  const int screenW2 = screenW >> 1;
  const int screenH2 = screenH >> 1;

  double centerX = 0, centerY = 0, oldX = 0, oldY = 0;
  int mouseX = 0, mouseY = 0;
  int limit = INITIAL_LIMIT;
  uint8_t paletteIndex = 4;
  double zoom = INITIAL_ZOOM;
  bool process = true, cycling = false;
  int cycle = 0, buffer = 0;

  void moving()
  {
    centerX = mouseX * zoom - screenW2 * zoom + oldX;
    oldX = centerX;
    centerY = mouseY * zoom - screenH2 * zoom + oldY;
    oldY = centerY;
    process = true;
  }

  void zooming()
  {
    moving();
    zoom *= 0.35;
    if (zoom < MAX_ZOOM_PRECISION)
    {
      zoom = MAX_ZOOM_PRECISION;
    }
    process = true;
  }

  while (true)
  {
    buffer = !buffer;
    if (process)
    {
      for (int h = 20; h < screenH; h++)
      {
        int screenWH = screenW * h;
        double ci = -1.0 * (h - screenH2) * zoom - centerY;
        for (int w = 0; w < screenW; w++)
        {
          double cr = (w - screenW2) * zoom + centerX;
          double zr = 0, zi = 0;
          int n1 = 0;
          double zrSquared, ziSquared;
          while ((zrSquared = zr * zr) + (ziSquared = zi * zi) < 4 && n1 != limit)
          {
            double temp = 2 * zr * zi + ci;
            zr = zrSquared - ziSquared + cr;
            zi = temp;
            n1++;
          }
          field[w + screenWH] = n1;
        }
      }
      process = false;
    }

    if (cycling)
    {
      cycle++;
    }

    console_init(xfb[buffer], 0, 20, rmode->fbWidth, 20, fbStride);
    printf(" cX:%.8f cY:%.8f", centerX, -centerY);
    printf("  zoom:%.4e ", INITIAL_ZOOM / zoom);

    for (int h = 20; h < screenH; h++)
    {
      int screenWHHalf = (screenW * h) >> 1;
      for (int w = 0; w < screenW; w++)
      {
        int n1 = field[w + screenW * h] + cycle;
        xfb[buffer][(w >> 1) + screenWHHalf] = CvtYUV(n1, n1, limit, paletteIndex);
      }
    }

    WPAD_ReadPending(WPAD_CHAN_ALL, countevs);
    res = WPAD_Probe(0, &type);
    if (res == WPAD_ERR_NONE)
    {
      wd = WPAD_Data(0);
      if (wd->ir.valid)
      {
        printf(" re:%.8f im:%.8f", (wd->ir.x - screenW2) * zoom + centerX, (screenH2 - wd->ir.y) * zoom - centerY);
        drawdot(xfb[buffer], rmode, (u16)wd->ir.x, (u16)wd->ir.y, COLOR_RED);
      }
      else
      {
        printf(" No Cursor");
      }

      if (wd->btns_d & WPAD_BUTTON_A)
      {
        mouseX = wd->ir.x;
        mouseY = wd->ir.y;
        zooming();
      }
      if (wd->btns_d & WPAD_BUTTON_B)
      {
        zoom = INITIAL_ZOOM;
        centerX = centerY = oldX = oldY = 0;
        process = true;
      }
      if (wd->btns_d & WPAD_BUTTON_DOWN)
      {
        cycling = !cycling;
      }
      if (wd->btns_d & WPAD_BUTTON_2)
      {
        limit = (limit > 1) ? (limit >> 1) : 1;
        process = true;
      }
      if (wd->btns_d & WPAD_BUTTON_1)
      {
        limit = (limit < LIMIT_MAX) ? (limit << 1) : LIMIT_MAX;
        process = true;
      }
      if (wd->btns_d & WPAD_BUTTON_MINUS)
      {
        paletteIndex = (paletteIndex > 0) ? (paletteIndex - 1) : 9;
      }
      if (wd->btns_d & WPAD_BUTTON_PLUS)
      {
        paletteIndex = (paletteIndex + 1) % 10;
      }
      if ((wd->btns_d & WPAD_BUTTON_HOME) || reboot)
      {
        shutdown_system();
        SYS_ResetSystem(SYS_RETURNTOMENU, 0, 0);
        exit(0);
      }
    }

    VIDEO_SetNextFramebuffer(xfb[buffer]);
    VIDEO_Flush();
    VIDEO_WaitVSync();
    if (switchoff)
    {
      shutdown_system();
      SYS_ResetSystem(SYS_POWEROFF, 0, false);
    }
  }
  return 0;
}

u32 CvtYUV(int n2, int n1, int limit, uint8_t paletteIndex)
{
  int y1, cb1, cr1, y2, cb2, cr2, cb, crx;

  if (n2 == limit)
  {
    y1 = 0; cb1 = 128; cr1 = 128;
  }
  else
  {
    Palette(paletteIndex, n2, &y1, &cb1, &cr1);
  }

  if (n1 == limit)
  {
    y2 = 0; cb2 = 128; cr2 = 128;
  }
  else
  {
    Palette(paletteIndex, n1, &y2, &cb2, &cr2);
  }

  cb = (cb1 + cb2) >> 1;
  crx = (cr1 + cr2) >> 1;
  return (y1 << 24) | (cb << 16) | (y2 << 8) | crx;
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
  xfb[0] = (u32 *)MEM_K0_TO_K1(SYS_AllocateFramebuffer(rmode));
  xfb[1] = (u32 *)MEM_K0_TO_K1(SYS_AllocateFramebuffer(rmode));
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

// EOF
