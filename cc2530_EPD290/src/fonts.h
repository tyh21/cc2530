#ifndef __FONTS_H
#define __FONTS_H

#include <ioCC2530.h>

#ifndef _UINT8_T_DEFINED
#define _UINT8_T_DEFINED
typedef unsigned char  uint8;
#endif
#ifndef _UINT16_T_DEFINED
#define _UINT16_T_DEFINED
typedef unsigned short uint16;
#endif

typedef struct sFONT_tag {
  const uint8 __code *table;   /* 字模在 CODE 区 */
  uint16 Width;
  uint16 Height;
} sFONT;

extern sFONT Font24;
extern sFONT Font16;
extern sFONT Font8;

#endif /* __FONTS_H */
