#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <ogcsys.h>
#include <gccore.h>
#include <wiiuse/wpad.h>
#include "palettes.h"

static u32 *xfb[2] = {NULL, NULL};
static GXRModeObj *rmode;
int reboot = 0, switchoff = 0, evctr = 0;

void reset()
{
  reboot = 1;
}

void poweroff()
{
  switchoff = 1;
}

static void init();
u32 CvtRGB(int n2, int n1, int limit, int paleta);

void drawdot(void *xfb, GXRModeObj *rmode, float w, float h, float fx, float fy, u32 color)
{
  u32 *fb;
  int px, py;
  int x, y;
  fb = (u32*)xfb;
  y = fy * rmode->xfbHeight / h;
  x = fx * rmode->fbWidth / w / 2;

  for (py = y - 4; py <= (y + 4); py++)
  {
    if (py < 0 || py >= rmode->xfbHeight)
    {
      continue;
    }

    for (px = x - 2; px <= (x + 2); px++)
    {
      if (px < 0 || px >= rmode->fbWidth / 2)
      {
        continue;
      }

      fb[rmode->fbWidth / VI_DISPLAY_PIX_SZ * py + px] = color;
    }
  }
}

void countevs(int chan, const WPADData *data)
{
  evctr++;
}

int main(int argc, char **argv)
{
  init();
  int res;
  u32 type;
  WPADData *wd;
  const int screenW = rmode->fbWidth;
  const int screenH = rmode->xfbHeight;
  int *field = (int*)malloc(sizeof(int) * screenW * screenH);
  double stredX = 0, stredY = 0;
  double oldX = 0, oldY = 0;
  int mousex = 0, mousey = 0, x = 0, y = 0;
  int limit = 200, paleta = 4;
  double zoom = 0.007;
  int proces = 1, counter = 0, first = 1;
  int n1 = 0;
  int n2 = 0;
  int w = 0;
  int h = 0;
  int cycling = 0;
  int cycle = 0, buffer = 0;
  double cr = 0, ci = 0, zr1 = 0, zr = 0, zi1 = 0, zi = 0;

  void moving()
  {
    stredX = mousex * zoom - (screenW / 2) * zoom + oldX;
    oldX = stredX;
    stredY = mousey * zoom - (screenH / 2) * zoom + oldY;
    oldY = stredY;
    proces = 1;
  }

  void zooming()
  {
    moving();
    zoom *= 0.35;
    proces = 1;
  }

  while (1)
  {
    buffer ^= 1;

    if (proces == 1)
    {
      h = 20;

      for (; h < screenH; h++)
      {
        w = 0;

        for (; w < screenW; w++)
        {
          cr = 0;
          ci = 0;
          n1 = 0;
          zr1 = 0;
          zr = 0;
          zi1 = 0;
          zi = 0;
          cr = (w - screenW / 2) * zoom + stredX;
          ci = -1.0 * (h - screenH / 2) * zoom - stredY;

          for (; (zr * zr + zi * zi) < 4 && n1 != limit; n1++)
          {
            zi = 2 * zi1 * zr1 + ci;
            zr = (zr1 * zr1) - (zi1 * zi1) + cr;
            zr1 = zr;
            zi1 = zi;
          }

          field[w + (screenW * h)] = n1;
        }
      }
      proces = 0;
      first = 1;
    }

    if (cycling == true)
    {
      cycle++;
    }

    console_init(xfb[buffer], 20, 20, rmode->fbWidth, 20, rmode->fbWidth * VI_DISPLAY_PIX_SZ);
    printf(" cX = %.4f cY = %.4f", stredX, -stredY);
    printf(" zoom = %.2f", 0.007 / zoom);
    h = 20;

    for (; h < screenH; h++)
    {
      w = 0;

      for (; w < screenW; w++)
      {
        n1 = field[w + screenW * h] + cycle;
        counter++;

        if (counter == 2)
        {
          xfb[buffer][(w / 2) + (screenW * h / 2)] = CvtRGB(n2, n1, limit, paleta);
          counter = 0;
        }

        n2 = n1;
      }
    }

    WPAD_ReadPending(WPAD_CHAN_ALL, countevs);
    res = WPAD_Probe(0, &type);

    if (res == WPAD_ERR_NONE)
    {
      wd = WPAD_Data(0);

      if (wd->ir.valid)
      {
        printf("  re = % .4f, im = % .4f", (wd->ir.x - screenW / 2) * zoom + stredX,
                                             (screenH / 2 - wd->ir.y) * zoom - stredY);
        x = wd->ir.x / 2;
        y = wd->ir.y;
        drawdot(xfb[buffer], rmode, rmode->fbWidth, rmode->xfbHeight, wd->ir.x, wd->ir.y, COLOR_RED);
      }
      else
      {
        printf("  No Cursor");
      }

      if (wd->btns_h & WPAD_BUTTON_A)
      {
        mousex = wd->ir.x;
        mousey = wd->ir.y;
        zooming();
      }

      if (wd->btns_h & WPAD_BUTTON_B)
      {
        zoom = 0.007;
        stredX = 0;
        stredY = 0;
        oldX = 0;
        oldY = 0;
        proces = 1;
      }

      if (wd->btns_d & WPAD_BUTTON_DOWN)
      {
        cycling ^= 1;
      }

      if (wd->btns_h & WPAD_BUTTON_2)
      {
        limit /= 2;
        proces = 1;
      }

      if (wd->btns_h & WPAD_BUTTON_1)
      {
        limit *= 2;
        proces = 1;
      }

      if (wd->btns_d & WPAD_BUTTON_MINUS)
      {
        paleta--;
        if (paleta < 0)
        {
          paleta = 10;
        }
      }

      if (wd->btns_d & WPAD_BUTTON_PLUS)
      {
        paleta++;
        paleta %= 11;
      }

      if ((wd->btns_h & WPAD_BUTTON_HOME) || reboot)
      {
        free(field);
        exit(0);
      }
    }

    VIDEO_SetNextFramebuffer(xfb[buffer]);
    VIDEO_Flush();
    VIDEO_WaitVSync();

    if (switchoff)
    {
      SYS_ResetSystem(SYS_POWEROFF, 0, false);
    }
  }

  return 0;
}

u32 CvtRGB(int n2, int n1, int limit, int paleta)
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
    Paleta(paleta, n2, &r, &g, &b);
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
    Paleta(paleta, n1, &r, &g, &b);
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
