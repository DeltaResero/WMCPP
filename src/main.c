#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <ogcsys.h>
#include <gccore.h>
#include <wiiuse/wpad.h>
#include "palettes.h"

const double INITIAL_ZOOM = 0.007;
const int INITIAL_LIMIT = 200;
const int CYCLE_OFFSET = 1;
const int MIN_ITERATION = 1;
const double MAX_ZOOM_PRECISION = 1e-14;

static u32 *xfb[2] = {NULL, NULL};
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
u32 CvtRGB(int n2, int n1, int limit, int palette);

void drawdot(void *xfb, GXRModeObj *rmode, float w, float h, float fx, float fy, u32 color)
{
  u32 *fb = (u32*)xfb;
  int y = (int)(fy * rmode->xfbHeight / h);
  int x = (int)(fx * rmode->fbWidth / w) >> 1;
  int fbStride = rmode->fbWidth / VI_DISPLAY_PIX_SZ;

  for (int py = (y - 4); py <= (y + 4); py++)
  {
    if (py < 0 || py >= rmode->xfbHeight)
    {
      continue;
    }
    int fbWidthHalf = rmode->fbWidth >> 1;
    int fbOffset = fbStride * py;
    for (int px = (x - 2); px <= (x + 2); px++)
    {
      if (px < 0 || px >= fbWidthHalf)
      {
        continue;
      }
      fb[fbOffset + px] = color;
    }
  }
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
  field = (int*)malloc(sizeof(int) * screenW * screenH);

  const int screenW2 = screenW >> 1;
  const int screenH2 = screenH >> 1;

  double centerX = 0, centerY = 0, oldX = 0, oldY = 0;
  int mouseX = 0, mouseY = 0;
  int limit = INITIAL_LIMIT, palette = 4;
  double zoom = INITIAL_ZOOM;
  bool process = true, cycling = false;
  int counter = 0, cycle = 0, buffer = 0;

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
          double zr1 = 0, zr = 0, zi1 = 0, zi = 0;
          int n1 = 0;
          double zrSquared, ziSquared;
          while ((zrSquared = zr1 * zr1) + (ziSquared = zi1 * zi1) < 4 && n1 != limit)
          {
            zi = 2 * zi1 * zr1 + ci;
            zr = zrSquared - ziSquared + cr;
            zr1 = zr;
            zi1 = zi;
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

    console_init(xfb[buffer], 20, 20, rmode->fbWidth, 20, fbStride);
    printf(" cX = %.4f cY = %.4f", centerX, -centerY);
    printf(" zoom = %.2f", INITIAL_ZOOM / zoom);

    for (int h = 20; h < screenH; h++)
    {
      int screenWHHalf = (screenW * h) >> 1;
      for (int w = 0; w < screenW; w++)
      {
        int n1 = field[w + screenW * h] + cycle;
        counter++;
        if (counter == 2)
        {
          xfb[buffer][(w >> 1) + screenWHHalf] = CvtRGB(n1, n1, limit, palette);
          counter = 0;
        }
      }
    }

    WPAD_ReadPending(WPAD_CHAN_ALL, countevs);
    res = WPAD_Probe(0, &type);
    if (res == WPAD_ERR_NONE)
    {
      wd = WPAD_Data(0);
      if (wd->ir.valid)
      {
        printf(" re = %.4f, im = %.4f", (wd->ir.x - screenW2) * zoom + centerX, (screenH2 - wd->ir.y) * zoom - centerY);
        drawdot(xfb[buffer], rmode, rmode->fbWidth, rmode->xfbHeight, wd->ir.x, wd->ir.y, COLOR_RED);
      }
      else
      {
        printf(" No Cursor");
      }

      if (wd->btns_h & WPAD_BUTTON_A)
      {
        mouseX = wd->ir.x;
        mouseY = wd->ir.y;
        zooming();
      }
      if (wd->btns_h & WPAD_BUTTON_B)
      {
        zoom = INITIAL_ZOOM;
        centerX = centerY = oldX = oldY = 0;
        process = true;
      }
      if (wd->btns_d & WPAD_BUTTON_DOWN)
      {
        cycling = !cycling;
      }
      if (wd->btns_h & WPAD_BUTTON_2)
      {
        limit = (limit > MIN_ITERATION) ? (limit >> 1) : MIN_ITERATION;
        process = true;
      }
      if (wd->btns_h & WPAD_BUTTON_1)
      {
        limit <<= 1;
        process = true;
      }
      if (wd->btns_d & WPAD_BUTTON_MINUS)
      {
        palette = (palette > 0) ? (palette - 1) : 9;
      }
      if (wd->btns_d & WPAD_BUTTON_PLUS)
      {
        palette = (palette + 1) % 10;
      }
      if ((wd->btns_h & WPAD_BUTTON_HOME) || reboot)
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

u32 CvtRGB(int n2, int n1, int limit, int palette)
{
  int y1, cb1, cr1, y2, cb2, cr2, cb, crx, r, g, b;

  if (n2 == limit)
  {
    y1 = 0;
    cb1 = 128;
    cr1 = 128;
  }
  else
  {
    Palette(palette, n2, &r, &g, &b);
    y1 = (299 * r + 587 * g + 114 * b) / 1000;
    cb1 = (-16874 * r - 33126 * g + 50000 * b + 12800000) / 100000;
    cr1 = (50000 * r - 41869 * g - 8131 * b + 12800000) / 100000;
  }

  if (n1 == limit)
  {
    y2 = 0;
    cb2 = 128;
    cr2 = 128;
  }
  else
  {
    Palette(palette, n1, &r, &g, &b);
    y2 = (299 * r + 587 * g + 114 * b) / 1000;
    cb2 = (-16874 * r - 33126 * g + 50000 * b + 12800000) / 100000;
    cr2 = (50000 * r - 41869 * g - 8131 * b + 12800000) / 100000;
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
  xfb[0] = (u32*)MEM_K0_TO_K1(SYS_AllocateFramebuffer(rmode));
  xfb[1] = (u32*)MEM_K0_TO_K1(SYS_AllocateFramebuffer(rmode));
  console_init(xfb[0], 20, 30, rmode->fbWidth, rmode->xfbHeight, rmode->fbWidth * VI_DISPLAY_PIX_SZ);
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
