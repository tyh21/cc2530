
#include <stdlib.h>
#include <string.h>
#include "epd2in9.h"
#include "epdpaint.h"
#include "epdtest.h"

/* 旧画布备份 (用于差分局刷: 写 0x26 old RAM) */
static __xdata unsigned char paint_old[896];

void EpdtestNotRefresh(void)
{
  EpdClearFrameMemory(0xFF); 
    
  //Rectangle
  PaintSetWidth(56);
  PaintSetHeight(42);
  PaintSetRotate(ROTATE_90);
  
  PaintClear(UNCOLORED);
  PaintDrawRectangle(0, 0, 40, 50, COLORED);
  PaintDrawLine(0, 0, 40, 50, COLORED);
  PaintDrawLine(40, 0, 0, 50, COLORED);
  EpdSetFrameMemoryXY(PaintGetImage(), 0, 15, PaintGetWidth(), PaintGetHeight());

  //Filled Rectangle
  PaintClear(UNCOLORED);
  PaintDrawFilledRectangle(0, 0, 40, 50, COLORED);
  PaintDrawLine(0, 0, 40, 50, UNCOLORED);
  PaintDrawLine(40, 0, 0, 50, UNCOLORED);
  EpdSetFrameMemoryXY(PaintGetImage(), 0, 82, PaintGetWidth(), PaintGetHeight());  

  //Circle
  PaintSetWidth(64);
  PaintSetHeight(64);
  PaintSetRotate(ROTATE_90);
  
  PaintClear(UNCOLORED);
  PaintDrawCircle(32, 32, 30, COLORED);
  EpdSetFrameMemoryXY(PaintGetImage(), 64, 5, PaintGetWidth(), PaintGetHeight());

  //Filled Circle
  PaintClear(UNCOLORED);
  PaintDrawFilledCircle(32, 32, 30, COLORED);
  EpdSetFrameMemoryXY(PaintGetImage(), 64, 72, PaintGetWidth(), PaintGetHeight());
  //
  PaintSetWidth(36);
  PaintSetHeight(100);
  PaintSetRotate(ROTATE_90);
  PaintClear(UNCOLORED);
  PaintDrawRectangle(0, 0, 96, 34, COLORED);
  EpdSetFrameMemoryXY(PaintGetImage(), 12, 142, PaintGetWidth(), PaintGetHeight());
 
  EpdDisplayFrame();
}

/* 局刷版时钟+继电器: 差分局刷 (old→0x26, new→0x24, 0x0C快刷) */
void EpdtestRefreshPartial(uint8 relay_state, unsigned long time_now_s)
{
  unsigned char *pimg;
  int pw, ph;

  // clock
  char time_string[] = {'0', '0', ':', '0', '0', '\0'};
  time_string[0] = time_now_s / 60 / 10 + '0';
  time_string[1] = time_now_s / 60 % 10 + '0';
  time_string[3] = time_now_s % 60 / 10 + '0';
  time_string[4] = time_now_s % 60 % 10 + '0';

  PaintSetWidth(24);
  PaintSetHeight(85);
  PaintSetRotate(ROTATE_90);

  /* 备份旧画布 → 0x26 old RAM */
  pimg = PaintGetImage();
  pw = PaintGetWidth();
  ph = PaintGetHeight();
  memcpy(paint_old, pimg, (pw / 8) * ph);
  EpdSetFrameMemoryXYOld(paint_old, 23, 148, pw, ph);

  /* 画新时钟 → 0x24 new RAM */
  PaintClear(UNCOLORED);
  PaintDrawStringAt(0, 4, time_string, &Font24, COLORED);
  EpdSetFrameMemoryXY(PaintGetImage(), 23, 148, PaintGetWidth(), PaintGetHeight());
  
  // Relay
  PaintSetWidth(16);
  PaintSetHeight(110);
  PaintSetRotate(ROTATE_90);    

  /* 备份旧画布 → 0x26 old RAM */
  pimg = PaintGetImage();
  pw = PaintGetWidth();
  ph = PaintGetHeight();
  memcpy(paint_old, pimg, (pw / 8) * ph);
  EpdSetFrameMemoryXYOld(paint_old, 90, 148, pw, ph);

  /* 画新继电器状态 → 0x24 new RAM */
  PaintClear(UNCOLORED);
  if (relay_state == 0) {
    PaintDrawStringAt(0, 0, "Relay: OFF", &Font16, COLORED);
  } else {
    PaintDrawStringAt(0, 0, "Relay: ON ", &Font16, COLORED);
  }
  EpdSetFrameMemoryXY(PaintGetImage(), 90, 148, PaintGetWidth(), PaintGetHeight());

  /* 局刷: 0x0C 快刷 (无闪烁) */
  EpdDisplayFramePartial();    
}
