/**
 * @file    clockfont.h
 * @brief   时钟程序字模 (由 tools/extract_fonts.py 自动生成, 勿手改)
 *
 * 字模来源: DA14585 日历工程 (da14585_213_104x212_calendar) 的 Fonts 目录
 *   数字: EPD_40X80_TABLE (40x80 大号数字 + ':')
 *   汉字: EPD_FontUTF8_16x16 (UTF-8, 含 星期一二三四五六日 等)
 *   西文: EPD_ASCII_11X16 / EPD_ASCII_7X12
 * 全部放在 __code (ROM) 里, 不占 RAM。
 */
#ifndef __CLOCKFONT_H__
#define __CLOCKFONT_H__

#include "hal_types.h"

extern __code const uint8 EPD_ASCII_7X12[];
extern __code const uint8 EPD_40X80_TABLE[];
extern __code const uint8 EPD_ASCII_11X16[];
extern __code const uint8 EPD_FontUTF8_16x16[];

#endif /* __CLOCKFONT_H__ */
