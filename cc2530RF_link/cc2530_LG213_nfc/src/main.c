/**
 * @file    main.c
 * @brief   第 10 轮排查固件 —— ★ 关键改动：诊断数据走串口，不再靠看屏照
 *
 * ================= 为什么这一轮必须加串口 =================
 *   上一轮我把屏幕上的 V= 一行读错了 digits（当成 2.6 V，实际是 3.3 V），
 *   于是在一个**错误的前提**上推出了一大坨结论（IO 倒灌 / VDD 塌陷），
 *   现在已确认全部作废：VDD 两轮采样都在 3.3 V 附近，供电一直很稳。
 *   => 靠照片读数不可靠，诊断数据必须有机器可读的通道。
 *
 *   UART0 Alt2 TX 独占 P1.3（9600 8N1），与其他功能无冲突：
 *     EPD 用 P0.2/3/4/5/7 + P1.1/1.2；NFC 用 P0.0/0.1 + P2.3；P1.0 作 NIRQ 采样。
 *     P1.3 空闲 -> 正好给 TX。RX 不用，所以不占 P1.2。
 *   接法：USB-TTL 的 RXD 接板子 P1.3，GND 共地。
 *
 * ================= 本轮回答的两个问题 =================
 *   1) PT=0 到底是"外部把我们顶住"还是"我们读引脚的代码写错了"？
 *      -> NFC_PinAnalog() 用 ADC 直接量引脚真实电压，自校准，不依赖任何标定常数。
 *   2) 为什么四个供电方式下都没有器件应答？
 *      -> 数据全部打印到串口，可以直接看，也许还能贴回来给我分析。
 *
 * 屏幕仍然会刷同样的内容（万一没接串口也能看）。
 */
#include <ioCC2530.h>
#include "epd2in9.h"
#include "nfc.h"

#ifndef _UINT32_T_DEFINED
#define _UINT32_T_DEFINED
typedef unsigned long uint32;
#endif

/*
 * 结尾状态：什么都别驱动，只把 P0.0 推挽拉低，方便万用表量这一脚。
 *   量到 ~0 V    = 我们能拉低，之前 PT=0 是数字读法的问题（软件可修）
 *   量到 ~3.3 V  = 外部有东西把 SDA 钉在高电平（硬件/走线问题）
 * 0=不park  1=P0.0拉低  2=P0.0拉高
 */
#ifndef FINAL_PARK
#define FINAL_PARK   1
#endif

/* ADC -> mV 的换算常数。
 * VDD = 3 * VREF_INT * code / 8191  ->  mV = code * (3*VREF_INT*1000) / 8191
 * ★ 用 VREF_INT = 1.15 V 而不是 TI 手册标称的 1.25 V：
 *   用户万用表量到 P2.3 = 3.28 V，而当时 ADC 原始码 = 31303 (>>2 = 7825)，
 *   反解 VREF_INT = 3*VREF*7825/8191 = 3.28  ->  VREF ≈ 1.146 V。
 *   所以内部基准实际是 ~1.15 V（Z-Stack 的 hal_adc.c 注释里就是这么写的）。
 *   常数取 3 * 1.15 * 1000 = 3450。 */
#define VDD_MV_K    3450UL

/* ------------------------------------------------------ 掉电保持的结果 ----- */
__no_init static __xdata uint16 g_boot;
__no_init static __xdata uint8  g_done;
__no_init static __xdata uint16 g_magic;

__no_init static __xdata uint8  g_lo_f,  g_lo_n;   /* VDDEX 推挽拉低 + 开漏扫 */
__no_init static __xdata uint8  g_log_f, g_log_n;  /* VDDEX 推挽拉低 + 推挽扫 */
__no_init static __xdata uint8  g_pu_f,  g_pu_n;   /* VDDEX 弱上拉   */
__no_init static __xdata uint8  g_hi_f,  g_hi_n;   /* VDDEX 推挽拉高 + 开漏扫 */
__no_init static __xdata uint8  g_hp_f,  g_hp_n;   /* VDDEX 推挽拉高 + 推挽扫 */
__no_init static __xdata uint8  g_pu_lvl;          /* 弱上拉时读回的 P2.3 电平 */
__no_init static __xdata uint8  g_p10_lo, g_p10_hi;/* P1.0(NIRQ?) 电平 */
__no_init static __xdata uint8  g_pt, g_xt, g_ph;  /* 引脚自检 / 互证 / 单线拉高 */

__no_init static __xdata uint16 g_a0lo, g_a0hi, g_a1lo, g_a1hi; /* ADC 原始码 */
__no_init static __xdata uint8  g_pa0, g_pa1;      /* 拉低残留百分比 */

__no_init static __xdata uint8  g_bus;
__no_init static __xdata uint8  g_a1, g_a2, g_a3;  /* 三种速率下 0x55 的 ACK */
__no_init static __xdata uint16 g_v_off, g_v_hi, g_v_hold, g_v_sda, g_v_scl;

__no_init static __xdata uint8  g_raw_n, g_rc, g_err, g_blk;
__no_init static __xdata uint8  g_raw[64];
__no_init static __xdata char   g_text[NFC_TEXT_MAX];

#define NV_MAGIC   0xA55Au

#define DONE_ANLG  0x01u
#define DONE_LO    0x02u
#define DONE_PU    0x04u
#define DONE_HI    0x08u
#define DONE_HOLD  0x10u
#define DONE_ISO   0x20u

/* ===================================================== UART0 Alt2 @ P1.3 == */
static void uart_init(void)
{
    PERCFG |= 0x01u;          /* UART0 -> Alt2 (Port1) */
    P1SEL  |= 0x08u;          /* P1.3 = TX 外设 */
    P1DIR  |= 0x08u;          /* 输出 */
    U0CSR   = 0x80u;          /* UART 模式 */
    U0GCR   = 8u;
    U0BAUD  = 59u;            /* 9600 @ 32 MHz */
    U0UCR   = 0x80u;          /* 无流控, 8N1 */
}

static void uputc(char c)
{
    while (U0CSR & 0x01u) {   /* ACTIVE: 等上一字节发完 */
    }
    U0DBUF = (uint8)c;
}

static void uputs(const char *s)
{
    while (*s != '\0') {
        if (*s == '\n') {
            uputc('\r');
        }
        uputc(*s++);
    }
}

static void udec(uint16 v)
{
    char buf[7];
    uint8 i = 0, started = 0;
    uint16 div = 10000u;

    while (div != 0u) {
        uint8 d = 0;
        while (v >= div) { v = (uint16)(v - div); d++; }
        if (d != 0u || started || div == 1u) {
            buf[i++] = (char)('0' + d);
            started = 1u;
        }
        div = (uint16)(div / 10u);
    }
    buf[i] = '\0';
    uputs(buf);
}

static void uhex2(uint8 v)
{
    static const char __code HX[] = "0123456789ABCDEF";
    uputc(HX[(v >> 4) & 0x0Fu]);
    uputc(HX[v & 0x0Fu]);
}

/* ------------------------------------------------- VDDEX(P2.3) 四种给法 --- */
static void vdd_off(void)
{
    P2SEL &= (uint8)~0x08u;
    P2INP |= 0x08u;
    P2_3 = 0;
    P2DIR &= (uint8)~0x08u;
}

static void vdd_low(void)
{
    P2SEL &= (uint8)~0x08u;
    P2DIR |= 0x08u;
    P2_3 = 0;
}

static void vdd_weak(void)
{
    P2SEL &= (uint8)~0x08u;
    P2INP &= (uint8)~0x80u;
    P2INP &= (uint8)~0x08u;
    P2_3   = 1;
    P2DIR &= (uint8)~0x08u;
}

static void vdd_strong(void)
{
    P2SEL &= (uint8)~0x08u;
    P2DIR |= 0x08u;
    P2_3 = 1;
}

/* ------------------------------------------------- P1.0 (疑似 NIRQ) ------ */
static void gpio_probe_init(void)
{
    P1SEL &= (uint8)~0x01u;
    P1DIR &= (uint8)~0x01u;
    P1INP &= (uint8)~0x01u;
    P2INP &= (uint8)~0x40u;
    P1IEN &= (uint8)~0x01u;
}

/* ---------------------------------------------------- 片内 ADC 量 VDD ----- */
static __idata uint16 av;
static __idata uint32 at;
static __idata uint16 aguard;

static uint16 adc_vdd_mv(void)
{
    NFC_DelayMs(1);
    ADCCON1 = 0x30u;
    ADCCON3 = 0x3Fu;          /* 内部基准 / 512 抽取 / AVDD5/3 */
    ADCCON1 |= 0x40u;

    for (aguard = 0; aguard < 60000u; aguard++) {
        if (ADCCON1 & 0x80u) {
            break;
        }
    }

    av = (uint16)ADCL;
    av |= (uint16)((uint16)ADCH << 8);
    av >>= 2;
    at = (uint32)av * VDD_MV_K;
    at /= 8191UL;
    return (uint16)at;
}

/* ------------------------------------------------------------ 小工具 ------ */
static void clock_init(void)
{
    uint16 guard;

    CLKCONCMD &= (uint8)~0x40u;
    for (guard = 0; guard < 60000u; guard++) {
        if ((CLKCONSTA & 0x40u) == 0u) {
            break;
        }
    }
    if (CLKCONSTA & 0x40u) {
        CLKCONCMD = (uint8)((CLKCONCMD | 0x40u) & (uint8)~0x07u);
    } else {
        CLKCONCMD &= (uint8)~0x47u;
    }
}

static void dec3(char *d, uint8 v)
{
    uint8 h = 0, t = 0;
    while (v >= 100u) { v = (uint8)(v - 100u); h++; }
    while (v >= 10u)  { v = (uint8)(v - 10u);  t++; }
    d[0] = (h ? (char)('0' + h) : ' ');
    d[1] = ((h == 0 && t == 0) ? ' ' : (char)('0' + t));
    d[2] = (char)('0' + v);
    d[3] = '\0';
}

static void dec5(char *d, uint16 v)
{
    d[0] = (char)('0' + (uint8)(v / 10000u));
    d[1] = (char)('0' + (uint8)((v / 1000u) % 10u));
    d[2] = (char)('0' + (uint8)((v / 100u)  % 10u));
    d[3] = (char)('0' + (uint8)((v / 10u)   % 10u));
    d[4] = (char)('0' + (uint8)(v % 10u));
    d[5] = '\0';
}

static void hex2(char *d, uint8 v)
{
    static const char __code HX[] = "0123456789ABCDEF";
    d[0] = HX[(v >> 4) & 0x0Fu];
    d[1] = HX[v & 0x0Fu];
    d[2] = '\0';
}

static void sapp(char *dst, const char *s)
{
    uint8 i = 0, j = 0;
    while (dst[i] != '\0') { i++; }
    while (s[j]   != '\0') { dst[i++] = s[j++]; }
    dst[i] = '\0';
}

static __xdata char line[48];
static __xdata char tmp[8];

/* ------------------------------------------------------ 冷启动判定 -------- */
static void nv_init_if_cold(void)
{
    uint8 i;

    if (g_magic != NV_MAGIC) {
        g_magic = NV_MAGIC;
        g_boot  = 0;
        g_done  = 0;
        g_lo_f = 0xFFu; g_lo_n = 0;
        g_log_f = 0xFFu; g_log_n = 0;
        g_pu_f = 0xFFu; g_pu_n = 0;
        g_hi_f = 0xFFu; g_hi_n = 0;
        g_hp_f = 0xFFu; g_hp_n = 0;
        g_pu_lvl  = 0;
        g_p10_lo  = 0;
        g_p10_hi  = 0;
        g_pt      = 0;
        g_xt      = 0;
        g_ph      = 0;
        g_a0lo = 0; g_a0hi = 0; g_a1lo = 0; g_a1hi = 0;
        g_pa0 = 0xFFu; g_pa1 = 0xFFu;
        g_bus     = 0;
        g_a1 = 1u; g_a2 = 1u; g_a3 = 1u;
        g_v_off = 0; g_v_hi = 0; g_v_hold = 0; g_v_sda = 0; g_v_scl = 0;
        g_raw_n = 0; g_rc = NFC_ERR_I2C;
        g_err = 0;  g_blk = 0xFFu;
        for (i = 0; i < 64u; i++) {
            g_raw[i] = 0x00u;
        }
        g_text[0] = '\0';
    }
}

/* --------------------------------------------------------- 结果屏 --------- */
static void draw_screen(void)
{
    uint8 i, k;

    EPD_Init();
    EPD_FrameClear(0xFF);

    line[0] = '\0';
    sapp(line, "LO  ad="); hex2(tmp, g_lo_f); sapp(line, tmp);
    sapp(line, " n=");     dec3(tmp, g_lo_n); sapp(line, tmp);
    sapp(line, " LG=");    dec3(tmp, g_log_n); sapp(line, tmp);
    EPD_DrawString(2, 2, line, &Font8, 1);

    line[0] = '\0';
    sapp(line, "PU  ad="); hex2(tmp, g_pu_f); sapp(line, tmp);
    sapp(line, " n=");     dec3(tmp, g_pu_n); sapp(line, tmp);
    sapp(line, " l=");     dec3(tmp, g_pu_lvl); sapp(line, tmp);
    EPD_DrawString(2, 11, line, &Font8, 1);

    line[0] = '\0';
    sapp(line, "HI  ad="); hex2(tmp, g_hi_f); sapp(line, tmp);
    sapp(line, " n=");     dec3(tmp, g_hi_n); sapp(line, tmp);
    sapp(line, " i=");     dec3(tmp, g_p10_hi); sapp(line, tmp);
    EPD_DrawString(2, 20, line, &Font8, 1);

    line[0] = '\0';
    sapp(line, "HP  ad="); hex2(tmp, g_hp_f); sapp(line, tmp);
    sapp(line, " n=");     dec3(tmp, g_hp_n); sapp(line, tmp);
    EPD_DrawString(2, 29, line, &Font8, 1);

    line[0] = '\0';
    sapp(line, "ACK ");
    dec3(tmp, g_a1); sapp(line, tmp);
    dec3(tmp, g_a2); sapp(line, tmp);
    dec3(tmp, g_a3); sapp(line, tmp);
    sapp(line, " PT="); dec3(tmp, g_pt); sapp(line, tmp);
    EPD_DrawString(2, 38, line, &Font8, 1);

    line[0] = '\0';
    sapp(line, "V=");
    dec5(tmp, g_v_off);  sapp(line, tmp); sapp(line, " ");
    dec5(tmp, g_v_hi);   sapp(line, tmp); sapp(line, " ");
    dec5(tmp, g_v_hold); sapp(line, tmp);
    EPD_DrawString(2, 47, line, &Font8, 1);

    /* ★ 本轮主角：ADC 自校准的引脚诊断（百分比，越小越好） */
    line[0] = '\0';
    sapp(line, "A0="); dec3(tmp, g_pa0); sapp(line, tmp);
    sapp(line, "% A1="); dec3(tmp, g_pa1); sapp(line, tmp);
    sapp(line, "% XT="); dec3(tmp, g_xt); sapp(line, tmp);
    EPD_DrawString(2, 56, line, &Font8, 1);

    line[0] = '\0';
    sapp(line, "Vf="); dec5(tmp, g_v_sda); sapp(line, tmp);
    sapp(line, " Vscl="); dec5(tmp, g_v_scl); sapp(line, tmp);
    EPD_DrawString(2, 65, line, &Font8, 1);

    line[0] = '\0';
    sapp(line, "boot="); dec3(tmp, (uint8)(g_boot & 0xFFu)); sapp(line, tmp);
    sapp(line, " done="); hex2(tmp, g_done);                 sapp(line, tmp);
    sapp(line, " bus=");  hex2(tmp, g_bus);                  sapp(line, tmp);
    EPD_DrawString(2, 74, line, &Font8, 1);

    for (k = 0; k < 2; k++) {
        line[0] = 'B';
        line[1] = (char)('0' + k);
        line[2] = ':';
        line[3] = '\0';
        for (i = 0; i < 8; i++) {
            hex2(tmp, g_raw[(uint8)(k * 16u + i)]);
            sapp(line, tmp);
        }
        EPD_DrawString(2, (uint16)(83u + (uint16)k * 9u), line, &Font8, 1);
    }

    EPD_ShowFrame();
}

/* ------------------------------------------------------- 串口报告 -------- */
static void ureport(void)
{
    uputs("\n=== NFC DIAG ===\n");
    uputs("boot="); udec(g_boot);
    uputs(" done=0x"); uhex2(g_done);
    uputs(" bus=0x"); uhex2(g_bus);
    uputs("\n");

    uputs("LO  ad=0x"); uhex2(g_lo_f);  uputs(" n="); udec(g_lo_n);
    uputs("  LG n="); udec(g_log_n); uputs("\n");

    uputs("PU  ad=0x"); uhex2(g_pu_f);  uputs(" n="); udec(g_pu_n);
    uputs("  P23lvl="); udec(g_pu_lvl); uputs("\n");

    uputs("HI  ad=0x"); uhex2(g_hi_f);  uputs(" n="); udec(g_hi_n);
    uputs("  NIRQhi="); udec(g_p10_hi);
    uputs(" NIRQlo="); udec(g_p10_lo); uputs("\n");

    uputs("HP  ad=0x"); uhex2(g_hp_f);  uputs(" n="); udec(g_hp_n); uputs("\n");

    uputs("ACK(21k/10k/5k)=");
    udec(g_a1); uputs(","); udec(g_a2); uputs(","); udec(g_a3); uputs("\n");

    uputs("PT=0x"); uhex2(g_pt);
    uputs(" XT=0x"); uhex2(g_xt);
    uputs(" PH=0x"); uhex2(g_ph);
    uputs("\n");

    uputs("VDD(mV) off="); udec(g_v_off);
    uputs(" hi=");  udec(g_v_hi);
    uputs(" hold="); udec(g_v_hold);
    uputs(" sda="); udec(g_v_sda);
    uputs(" scl="); udec(g_v_scl);
    uputs("\n");

    /* ★ 引脚拉低残留率 —— 本轮核心判据 */
    uputs("PIN-LOW %% : P0.0="); udec(g_pa0);
    uputs("%%  P0.1="); udec(g_pa1); uputs("%\n");
    uputs("  (~0%%=能拉低/外部没抢; >50%%=外部强驱动顶住)\n");
    uputs("ADC raw: a0 lo="); udec(g_a0lo); uputs(" hi="); udec(g_a0hi);
    uputs(" | a1 lo="); udec(g_a1lo); uputs(" hi="); udec(g_a1hi);
    uputs("\n");

    uputs("read n="); udec(g_raw_n);
    uputs(" rc="); udec(g_rc);
    uputs(" err="); udec(g_err);
    uputs(" blk="); udec(g_blk);
    uputs("\n");
}

/* --------------------------------------------------------- 探测级 -------- */
/* NOTE: 引脚 ADC 诊断放在 main() 最前面内联执行（保证是"上电后没碰过总线"
   的干净状态），这里就不再单独做一级了。 */

static void stage_lo(void)
{
    vdd_low();
    NFC_DelayMs(20);
    g_bus    = NFC_BusLevel();
    g_p10_lo = (P1 & 0x01u) ? 1u : 0u;

    g_pt = NFC_PinTest();
    g_xt = NFC_XTalk();

    NFC_SetDrive(0);
    NFC_BusRecover();
    g_lo_n = NFC_ScanAddr(&g_lo_f);

    NFC_SetDrive(1);
    NFC_BusRecover();
    g_log_n = NFC_ScanAddr(&g_log_f);
    NFC_SetDrive(0);
}

static void stage_pu(void)
{
    vdd_weak();
    NFC_DelayMs(20);
    g_bus    = NFC_BusLevel();
    g_pu_lvl = (P2 & 0x08u) ? 1u : 0u;
    NFC_SetDrive(0);
    NFC_BusRecover();
    g_pu_n = NFC_ScanAddr(&g_pu_f);
}

static void stage_hi(void)
{
    vdd_strong();
    NFC_DelayMs(50);
    g_bus    = NFC_BusLevel();
    g_p10_hi = (P1 & 0x01u) ? 1u : 0u;
    g_v_hi   = adc_vdd_mv();

    NFC_SetDrive(0);
    NFC_BusRecover();
    g_hi_n = NFC_ScanAddr(&g_hi_f);

    NFC_SetDrive(1);
    NFC_BusRecover();
    g_hp_n = NFC_ScanAddr(&g_hp_f);
    NFC_SetDrive(0);

    NFC_SetSpeed(1); g_a1 = NFC_Probe();
    NFC_SetSpeed(2); g_a2 = NFC_Probe();
    NFC_SetSpeed(4); g_a3 = NFC_Probe();
    NFC_SetSpeed(1);

    g_raw_n = NFC_ReadRaw(0, 4, g_raw);
    g_rc    = NFC_ReadText(g_text, NFC_TEXT_MAX);
    g_err   = g_nfc_errcode;
    g_blk   = g_nfc_lastblk;
}

static void stage_hold(void)
{
    vdd_strong();
    NFC_DelayMs(30);
    g_v_hold = adc_vdd_mv();
}

static void stage_iso(void)
{
    vdd_off();

    NFC_BusFloat();
    NFC_DelayMs(20);
    adc_vdd_mv();                 /* 读数丢弃，只为稳定 */
    NFC_DelayMs(20);
    g_v_off = adc_vdd_mv();       /* 重测：这才是干净基线 */

    g_ph = NFC_PullHighTest();

    NFC_BusFloat();
    NFC_DelayMs(10);
    P0_0 = 1; P0DIR |= 0x01u;     /* 只驱动 SDA */
    NFC_DelayMs(20);
    g_v_sda = adc_vdd_mv();
    P0DIR &= (uint8)~0x01u; P0_0 = 1;

    NFC_BusFloat();
    NFC_DelayMs(10);
    P0_1 = 1; P0DIR |= 0x02u;     /* 只驱动 SCL */
    NFC_DelayMs(20);
    g_v_scl = adc_vdd_mv();
    P0DIR &= (uint8)~0x02u; P0_1 = 1;

    NFC_BusFloat();
}

/* ------------------------------------------------------------ main ------- */
int main(void)
{
    clock_init();
    vdd_off();
    uart_init();
    nv_init_if_cold();
    g_boot++;
    gpio_probe_init();

    uputs("\n\n######## NFC DIAG BOOT ########\n");
    uputs("UART0 Alt2 TX=P1.3 9600 8N1\n");

    NFC_PinAnalog();              /* 先拿一份干净的引脚数据 */
    g_a0lo = g_nfc_a0_lo; g_a0hi = g_nfc_a0_hi;
    g_a1lo = g_nfc_a1_lo; g_a1hi = g_nfc_a1_hi;
    g_pa0 = (g_nfc_a0_hi != 0u)
          ? (uint8)(((uint32)g_nfc_a0_lo * 100u) / (uint32)g_nfc_a0_hi) : 0xFFu;
    g_pa1 = (g_nfc_a1_hi != 0u)
          ? (uint8)(((uint32)g_nfc_a1_lo * 100u) / (uint32)g_nfc_a1_hi) : 0xFFu;

    g_v_off = adc_vdd_mv();
    draw_screen();
    ureport();

    if ((g_done & DONE_LO) == 0u) {
        g_done |= DONE_LO;
        stage_lo();
        vdd_off(); draw_screen(); ureport();
    }
    if ((g_done & DONE_PU) == 0u) {
        g_done |= DONE_PU;
        stage_pu();
        vdd_off(); draw_screen(); ureport();
    }
    if ((g_done & DONE_HI) == 0u) {
        g_done |= DONE_HI;
        stage_hi();
        vdd_off(); draw_screen(); ureport();
    }
    if ((g_done & DONE_HOLD) == 0u) {
        g_done |= DONE_HOLD;
        stage_hold();
        vdd_off(); draw_screen(); ureport();
    }
    if ((g_done & DONE_ISO) == 0u) {
        g_done |= DONE_ISO;
        stage_iso();
        vdd_off(); draw_screen(); ureport();
    }

    uputs("=== ALL DONE ===\n");

#if FINAL_PARK == 1
    /* 停在"P0.0 推挽拉低"：万用表量这一脚，0V=软件读法问题，3.3V=外部顶住 */
    vdd_off();
    P0SEL &= (uint8)~0x01u;
    P0_0 = 0;
    P0DIR |= 0x01u;
#elif FINAL_PARK == 2
    vdd_off();
    P0SEL &= (uint8)~0x01u;
    P0_0 = 1;
    P0DIR |= 0x01u;
#endif

    while (1) {
    }
}
