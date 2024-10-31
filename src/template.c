#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <ogcsys.h>
#include <gccore.h>
#include "palettes.h"

static u32 *xfb = NULL;

static GXRModeObj *rmode = NULL;

void (*reload)() = (void(*)())0x90000020;

int main(int argc, char **argv) {

	VIDEO_Init();
	PAD_Init();
	
	switch(VIDEO_GetCurrentTvMode()) {
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
			break;
	}

	xfb = MEM_K0_TO_K1(SYS_AllocateFramebuffer(rmode));
	
	console_init(xfb,20,20,rmode->fbWidth,rmode->xfbHeight,rmode->fbWidth*VI_DISPLAY_PIX_SZ);
	
	VIDEO_Configure(rmode);
	VIDEO_SetNextFramebuffer(xfb);
	VIDEO_SetBlack(FALSE);
	VIDEO_Flush();
	VIDEO_WaitVSync();
	if(rmode->viTVMode&VI_NON_INTERLACE) VIDEO_WaitVSync();
    
    /////////////////////////////////////////////////////////////////////////////////
    const int screenW = rmode->fbWidth;
    const int screenH = rmode->xfbHeight;
    double stredX = 0, stredY = 0;
    double oldX = 0, oldY = 0;
    int mousex = 0;int mousey = 0;

    int limit = 200; int r, g, b;int paleta = 4;
    double zoom = 0.007;
    int proces = 1;int counter = 0; 
    
    int n1 = 0;
    int n2 = 0;
    
    int w = 0; int h = 0;
    double cr = 0; double ci = 0;double zr1 = 0;double zr = 0;double zi1 = 0;double zi = 0; 
    
    u32
    CvtRGB ()
    {
      int y1, cb1, cr1, y2, cb2, cr2, cb, crx;
    
      if(n2==limit)
      {
      y1 = 0;
      cb1 = 128;
      cr1 = 128;           
      }                   
      else {Paleta(paleta, n2, &r, &g, &b);          
      y1 = (299 * r + 587 * g + 114 * b) / 1000;
      cb1 = (-16874 * r - 33126 * g + 50000 * b + 12800000) / 100000;
      cr1 = (50000 * r - 41869 * g - 8131 * b + 12800000) / 100000;}
 
      if(n1==limit)
      {
      y2 = 0;
      cb2 = 128;
      cr2 = 128;           
      } 
      else {Paleta(paleta, n1, &r, &g, &b);   
      y2 = (299 * r + 587 * g + 114 * b) / 1000;
      cb2 = (-16874 * r - 33126 * g + 50000 * b + 12800000) / 100000;
      cr2 = (50000 * r - 41869 * g - 8131 * b + 12800000) / 100000;}
  
      cb = (cb1 + cb2) >> 1;
      crx = (cr1 + cr2) >> 1;
 
      return (y1 << 24) | (cb << 16) | (y2 << 8) | crx;
    }
        
    void moving()
    {
    stredX = mousex*zoom - (screenW/2)*zoom + oldX;
    oldX = stredX;
             
    stredY = mousey*zoom - (screenH/2)*zoom + oldY;
    oldY = stredY;                                    
                                                  
    proces = 1;        
    }
    
    void zooming()
    {
    moving();                                     
    zoom *= 0.35;
    proces = 1;        
    }
    
    while(1) {
        if(proces == 1)
        {
                 w = 0; h = 0;
                            for(;h<screenH;h++)
                            {
                                   w = 0;
                                   for(;w<screenW;w++)
                                   {
                                   cr = 0; ci = 0; zr1 = 0; zr = 0; zi1 = 0; zi = 0;                   
                                   cr = (w - screenW/2)*zoom + stredX;
                                   ci = -1.0*(h - screenH/2)*zoom - stredY;
                                   n1 = 0;
                                   for(;(zr*zr+zi*zi)<4&&n1!=limit;n1++)
                                          {
                                          zi=2*zi1*zr1+ci;
                                          zr=(zr1*zr1)-(zi1*zi1)+cr;
                                          zr1=zr;
                                          zi1=zi;                   
                                          }
                                   
                                          counter++;
                                          if(counter==2)
                                          {
                                          xfb[(w/2)+(screenW*h/2)] = CvtRGB();
                                          counter = 0;
                                          }
                                          n2 = n1;   
                                   }
                                   VIDEO_WaitVSync(); 
                             }
                  proces = 0;          
        }
        ///////////////////////////////////////////////////////////////////////////////////

		VIDEO_WaitVSync();
		PAD_ScanPads();

        int buttonsDown = PAD_ButtonsHeld(0);
        if(buttonsDown & PAD_BUTTON_Y){limit/=2;proces = 1;}
        if(buttonsDown & PAD_BUTTON_X){limit*=2;proces = 1;}
        if(buttonsDown & PAD_BUTTON_A)
                       {
                        mousex = screenW/2;
                        mousey = rmode->xfbHeight/2;              
                        zooming();
                       }
        
        if(buttonsDown & PAD_BUTTON_LEFT)
                       {
                        mousex = screenW*0.25;
                        mousey = screenH/2;              
                        moving();
                       }
        if(buttonsDown & PAD_BUTTON_RIGHT)
                       {
                        mousex = screenW*0.75;
                        mousey = screenH/2;              
                        moving();
                       }
        if(buttonsDown & PAD_BUTTON_UP)
                       {
                        mousex = screenW/2;
                        mousey = screenH*0.25;              
                        moving();
                       }
        if(buttonsDown & PAD_BUTTON_DOWN)
                       {
                        mousex = screenW/2;
                        mousey = screenH*0.75;              
                        moving();
                       }
                       
        if(buttonsDown & PAD_TRIGGER_L) {paleta--;proces = 1;if(paleta<0){paleta = 0;proces = 0;};}         
        if(buttonsDown & PAD_TRIGGER_R) {paleta++;proces = 1;if(paleta>10){paleta = 10;proces = 0;};}                    
        if( (buttonsDown & PAD_TRIGGER_Z) && (buttonsDown & PAD_BUTTON_START)) {reload();} 
   	}

	return 0;
}
