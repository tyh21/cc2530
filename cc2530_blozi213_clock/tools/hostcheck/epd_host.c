/* 主机侧校验: 用 gcc 编译 epd_paint.c + clockfont.c + clockface.c,
 * 把时钟画面渲染成 PBM, 用来肉眼核对排版/字模对不对 (不涉及 CC2530 硬件)。
 * 编译:
 *   gcc -std=c99 -I tools/hostcheck -I src tools/hostcheck/epd_host.c \
 *       src/epd_paint.c src/clockfont.c src/clockface.c -o tools/hostcheck/clock.exe
 */
#include <stdio.h>
#include <string.h>
#include "epd2in13.h"
#include "epd_paint.h"
#include "clockface.h"

#define W 212
#define H 104

static unsigned char g_buf[H * W];
static const char *g_out = "clock.pbm";

uint8  EPD_Frame[EPD_IMG_BYTES];
uint8  EPD_RedNone = 0xFF;

void EPD_Init(void) {}
void EPD_InitPart(void) {}
void EPD_Sleep(void) {}
void EPD_DelayMS(uint16 ms) { (void)ms; }
uint16 EPD_DrainOpMs(void) { return 0; }
void halMcuWaitMs(uint16 ms) { (void)ms; }

void EPD_FrameClear(uint8 color)
{
    memset(g_buf, color == 0x00 ? 0 : 255, sizeof(g_buf));
}

void EPD_SetPixel(uint16 x, uint8 y, uint8 black)
{
    if (x >= W || y >= H) return;
    g_buf[(uint16)y * W + x] = black ? 0 : 255;
}

static void dump(void)
{
    FILE *f = fopen(g_out, "wb");
    int y, x;
    if (!f) { printf("cannot open %s\n", g_out); return; }
    fprintf(f, "P4\n%d %d\n", W, H);
    for (y = 0; y < H; y++) {
        unsigned char byte = 0;
        int nbit = 0;
        for (x = 0; x < W; x++) {
            byte = (unsigned char)((byte << 1) | (g_buf[y * W + x] == 0 ? 1 : 0));
            if (++nbit == 8) { fputc(byte, f); byte = 0; nbit = 0; }
        }
        if (nbit) { byte <<= (8 - nbit); fputc(byte, f); }   /* 行尾补齐 */
    }
    fclose(f);
    printf("wrote %s\n", g_out);
}

void EPD_ShowFrame(void)     { dump(); }
void EPD_ShowFrameFull(void) { dump(); }
void EPD_ShowFramePart(void) { dump(); }

int main(void)
{
    struct { uint8 h, m; const char *file; } cases[] = {
        {12, 34, "clock_1234.pbm"},
        { 8, 56, "clock_0856.pbm"},
        { 0,  0, "clock_0000.pbm"},
        {23, 59, "clock_2359.pbm"},
        {10,  8, "clock_1008.pbm"},
    };
    unsigned i;
    static const char *wdays[7] = {
        "wd_0_sun.pbm", "wd_1_mon.pbm", "wd_2_tue.pbm", "wd_3_wed.pbm",
        "wd_4_thu.pbm", "wd_5_fri.pbm", "wd_6_sat.pbm"
    };
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        g_out = cases[i].file;
        clockface_set_time(cases[i].h, cases[i].m, 0);
        clockface_draw();
    }
    /* 2026-10-25 是周日, 往后 7 天覆盖 星期日..星期六 */
    for (i = 0; i < 7; i++) {
        g_out = wdays[i];
        clockface_set_date(2026, 10, (uint8)(25 + i));
        clockface_draw();
    }
    return 0;
}
