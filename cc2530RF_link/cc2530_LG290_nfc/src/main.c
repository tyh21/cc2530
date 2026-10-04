/**
 * @file    main.c
 * @brief   LG290 板 MN63Y1213 —— v5 诊断固件
 *
 * ============================================================================
 * v4 四屏读完之后的结论(也是 v5 要证伪/证实的):
 *   · 屏1 "kHz=251" 是**显示 bug**: NFC_GetBitUs100() 把 ns 除了 100(应该除 10),
 *     真实位周期一直是 40us = 25kHz —— 正好在手册 5.2.1 允许的 20~100kHz 之内。
 *     所以"跑太快芯片不认"这条可以划掉。
 *   · 屏1 "ns=061" ⇒ T1 每 tick 61ns(≈16.4MHz) 而不是 32MHz。CC2530 复位后
 *     CLKCONCMD.TICKSPD 默认不是 /1。v5 显式设成 /1。
 *   · 屏2(示波)与屏3(单发 0x54/0x55)都说"没有 ACK", 屏4(扫 128)却说"128 个全 ACK"
 *     —— 自相矛盾。v5 用**同一条探测代码**分别以 rd=0 / rd=1 各扫一遍, 把这个矛盾钉死。
 *   · 手册 2.3 + Figure 2-3: SDA/SCL/NIRQ 要 **3.3k 上拉到 VDDEX**, VDDEX 挂电容。
 *     而 v4 在 VDDEX 还没抬起来之前就用 MCU 内部上拉把两条线拉到 3.3V —— 电流经芯片
 *     ESD 二极管灌进 VDDEX 节点, 芯片"半个上电", 上电复位不干净。v5 改成: 先三态、
 *     再抬 VDDEX、等 tBoot、最后才碰两条线。
 *
 * v5 五屏(每屏全刷, 停 5 秒; 第 5 屏要拿手机贴线圈):
 *   1 TIME&PWR  T1 时间基准(修正后的 us/kHz) + 外部上拉在不在 + 断电倒灌 + VDDEX 去耦
 *   2 SCOPE     0x54 的 R/W 两种真波形画上屏, 第 9 位是什么就看图
 *   3 tBOOT     抬 VDDEX 后 2/10/50/200/1000ms 各探一次 0x54; 再连探 20 次
 *   4 SCAN      rd0 与 rd1 各扫 0x00..0x7F, 直接对比(破"128 全 ACK")
 *   5 RF ALIVE  拿手机贴线圈, 看 NIRQ 会不会动 —— 证明 VDDEX 真喂到芯片、芯片活着
 *
 * ★ 铁律: VDDEX = P2.3。所有测量在 VDDEX ON 时做完, 画屏前必须 NFC_PowerOff()。
 * ============================================================================
 */
#include <ioCC2530.h>
#include "epd2in9.h"
#include "nfc.h"

/* ============================ 串口 ======================================= */
static void uart_init(void)
{
    PERCFG |= 0x01u;
    P1SEL  |= 0x08u;          /* P1.3 = TX */
    P1DIR  |= 0x08u;
    P1SEL  &= (uint8)~0x04u;  /* P1.2 留给 EPD BUSY */
    U0CSR   = 0x80u;
    U0GCR   = 8u;
    U0BAUD  = 59u;            /* 9600 @32MHz */
    U0UCR   = 0x80u;
    UTX0IF  = 0;
}

static uint16 uart_guard;

static void uputc(char c)
{
    U0DBUF = (uint8)c;
    for (uart_guard = 0; uart_guard < 60000u; uart_guard++) {
        if (UTX0IF) { break; }
    }
    UTX0IF = 0;
}

static void uputs(const char *s)
{
    while (*s) { uputc(*s++); }
}

static void clock_init(void)
{
    CLKCONCMD &= (uint8)~0x40u;
    while (CLKCONSTA & 0x40u) { ; }
    CLKCONCMD &= (uint8)~0x47u;
}

/* ============================ 文本 ======================================= */
static const char __code HEX[] = "0123456789ABCDEF";

static char  lb[80];
static uint8 lp;

static void b_reset(void) { lp = 0u; lb[0] = '\0'; }
static void b_ch(char c)  { if (lp < 74u) { lb[lp++] = c; lb[lp] = '\0'; } }
static void b_str(const char *s)
{
    uint8 i = 0u;
    while (s[i] != '\0' && lp < 70u) { lb[lp++] = s[i]; i++; }
    lb[lp] = '\0';
}
static void b_hex8(uint8 v)
{
    b_ch(HEX[(uint8)((v >> 4) & 0x0Fu)]);
    b_ch(HEX[(uint8)(v & 0x0Fu)]);
}
static void b_dig(uint8 v) { b_ch((char)('0' + (uint8)(v & 0x0Fu))); }

static void b_udec(uint16 v, uint8 w)
{
    char  tmp[6];
    uint8 n = 0u;
    if (w > 5u) { w = 5u; }
    do {
        tmp[n++] = (char)('0' + (uint8)(v % 10u));
        v = (uint16)(v / 10u);
    } while (v != 0u && n < 5u);
    while (n < w && n < 5u) { tmp[n++] = '0'; }
    while (n > 0u) { b_ch(tmp[--n]); }
}

/* ACK 类别 -> 4 个字 */
static void b_kind(uint8 k)
{
    if      (k == NFC_ACK_REAL)  { b_str("REAL"); }
    else if (k == NFC_ACK_LATE)  { b_str("LATE"); }
    else if (k == NFC_ACK_STUCK) { b_str("STCK"); }
    else                         { b_str("NONE"); }
}

/* ============================ 画屏 ======================================= */
#define X0      2u
#define DY      10u
#define YTITLE  2u
#define YSUB    22u
#define YROW    32u
#define MAXROW  8u
#define WAIT_MS 5000u

static void row8(uint8 r, const char *s)
{
    if (r > MAXROW) { return; }
    EPD_DrawString(X0, (uint16)(YROW + (uint16)r * DY), s, &Font8, 1);
}

static void title(const char *big, const char *sub)
{
    EPD_FrameClear(0xFF);
    EPD_DrawString(X0, YTITLE, big, &Font16, 1);
    if (sub != 0) { EPD_DrawString(X0, YSUB, sub, &Font8, 1); }
}

static void flush(void)
{
    EPD_DisplayImage((const uint8 *)EPD_Frame);
}

static void dr(uint8 r)
{
    row8(r, lb);
    uputs(lb);
    uputs("\r\n");
}

/* 把 g_scp[] 画成两条波形线: SCL(y=20/30) 与 SDA(y=48/68) */
static void draw_scope(void)
{
    uint8  i;
    const uint16 x0 = 4u, xstep = 2u;
    uint16 px = x0; uint8 psda = 1u, pscl = 1u;

    for (i = 0; i < SCP_N; i++) {
        uint16 x  = (uint16)(x0 + (uint16)i * xstep);
        uint8  sda = (uint8)(g_scp[i] & 0x01u);
        uint8  scl = (uint8)((g_scp[i] >> 1) & 0x01u);
        uint8  sy  = sda ? 58u : 76u;
        uint8  cy  = scl ? 28u : 40u;
        if (i > 0u) {
            EPD_DrawLine((int)px, (int)psda, (int)x, (int)sy, 1);
            EPD_DrawLine((int)px, (int)pscl, (int)x, (int)cy, 1);
        } else {
            EPD_SetPixel(x, sy, 1);
            EPD_SetPixel(x, cy, 1);
        }
        px = x; psda = sy; pscl = cy;
    }
}

/* ============================ 结果量 ===================================== */
/* 屏1 */
static uint8  g_t1ok, g_calok;
static uint16 g_tickns, g_bitns;
static uint8  g_bus, g_buson, g_busoff;
static uint16 g_vddhold;
static uint16 g_sdar, g_sclr, g_sdaf;
static uint8  g_p00_0, g_p00_1, g_p00_i;
static uint8  g_p01_0, g_p01_1, g_p01_i;
static uint8  g_x00lo, g_x00hi, g_x01lo;
static uint8  g_nq, g_vdd;

/* 屏2 */
static uint8  g_k_a8, g_k_a9, g_k_aa, g_k_ab, g_k_ctl;
static uint8  g_scp_kind, g_ack_nib, g_ack_post;

/* 屏3 */
static uint8  g_bw[5];        /* 每个 tBoot 档: 0x54 rd=0 的 kind */
static uint8  g_br[5];        /* 每个 tBoot 档: 0x54 rd=1 的 kind */
static uint8  g_rep_real;

/* 屏4 */
static uint8  g_n_r0, g_n_r1;
static uint8  g_f0[3], g_f1[3];
static uint8  g_p54w, g_p54r, g_p55w, g_p55r;
static uint8  g_nq_on, g_nq_off;

/* 屏5 */
static uint16 g_rf_low;
static uint8  g_rf_hit;

/* ============================ 上电 ======================================= */
static void power_cycle(uint8 bootMs)
{
    NFC_PowerOff();
    NFC_DelayMs(50u);
    NFC_PowerUp(bootMs);
    NFC_BusRecover();
}

/* ============================ 主流程 ===================================== */
int main(void)
{
    uint8 i;

    clock_init();
    uart_init();
    uputs("\r\n=== MN63Y1213 v5: timebase fix + pull-up/VDDEX + rd0/rd1 ===\r\n");

    NFC_PowerOff();
    NFC_SetSwap(0u);
    NFC_SetDrive(1u);
    NFC_SetOrder(1u);
    NFC_T1Init();
    NFC_TimebaseCal();

    EPD_Init();
    EPD_Fill(0xFF);

    for (;;) {
    /* ============ 第 1 屏: 时间基准 + 上拉/倒灌/VDDEX (VDDEX ON) ============ */
    power_cycle(50u);

    g_t1ok   = NFC_T1Ok();
    g_calok  = NFC_GetCalOk();
    g_tickns = NFC_GetTickNs();
    g_bitns  = NFC_GetBitNs();

    g_bus    = NFC_BusLevel();
    g_sdar   = NFC_SdaRiseUs10();
    g_sclr   = NFC_SclRiseUs10();
    g_sdaf   = NFC_SdaFallUs10();

    g_buson  = NFC_BusOn();       /* 高4位=内拉时, 低4位=三态时 */
    g_vddhold = NFC_VddHoldMs();
    g_busoff = NFC_BusOff();      /* 同上, 但 VDDEX 断 */

    g_p00_0 = NFC_PinDrive(0u, 0u);
    g_p00_1 = NFC_PinDrive(0u, 1u);
    g_p00_i = NFC_PinFloat(0u);
    g_p01_0 = NFC_PinDrive(1u, 0u);
    g_p01_1 = NFC_PinDrive(1u, 1u);
    g_p01_i = NFC_PinFloat(1u);
    g_x00lo = NFC_DriveAndReadOther(0u, 0u);
    g_x00hi = NFC_DriveAndReadOther(0u, 1u);
    g_x01lo = NFC_DriveAndReadOther(1u, 0u);
    /* 这两个是诊断量, 暂不显示, 用 void 抑制 "set but never used" 警告 */
    (void)g_sdaf;
    (void)g_x01lo;

    NFC_PowerUp(50u);
    g_nq    = NFC_NirqLevel();
    g_vdd   = (uint8)((P2 & 0x08u) ? 1u : 0u);

    NFC_PowerOff();
    title("1 TIME&PWR", "T1 tick + pull-up + VDDEX");

    b_reset(); b_str("T1ok="); b_dig(g_t1ok);
    b_str(" cal="); b_dig(g_calok);
    b_str(" tick="); b_udec(g_tickns, 3u);
    b_str("ns (want 031)"); dr(0u);

    b_reset(); b_str("bit="); b_udec((uint16)(g_bitns / 1000u), 3u);
    b_str("us  kHz=");
    { uint16 khz = (g_bitns != 0u) ? (uint16)(1000000UL / (uint32)g_bitns) : 0u;
      b_udec(khz, 3u); }
    dr(1u);

    b_reset(); b_str("bus=0x"); b_hex8(g_bus);
    b_str(" sdR="); b_udec(g_sdar, 4u);
    b_str(" scR="); b_udec(g_sclr, 4u); dr(2u);

    b_reset(); b_str("On=0x"); b_hex8(g_buson);
    b_str(" Off=0x"); b_hex8(g_busoff);
    b_str(" hold="); b_udec(g_vddhold, 3u);
    b_str("ms"); dr(3u);

    b_reset(); b_str("P00 d0="); b_dig(g_p00_0);
    b_str(" d1="); b_dig(g_p00_1);
    b_str(" in="); b_dig(g_p00_i); dr(4u);

    b_reset(); b_str("P01 d0="); b_dig(g_p01_0);
    b_str(" d1="); b_dig(g_p01_1);
    b_str(" in="); b_dig(g_p01_i); dr(5u);

    b_reset(); b_str("X00>01lo="); b_dig(g_x00lo);
    b_str(" hi="); b_dig(g_x00hi);
    b_str(" nq="); b_dig(g_nq);
    b_str(" vdd="); b_dig(g_vdd); dr(6u);

    /* 结论行: On 的低 4 位(三态) 必须 0x03; Off 的低 4 位(三态) 必须 0x00 */
    {
        uint8 tri_on  = (uint8)(g_buson  & 0x0Fu);
        uint8 tri_off = (uint8)(g_busoff & 0x0Fu);
        if ((g_p00_0 != 0u) || (g_p01_0 != 0u)) {
            row8(7u, "!! PIN CANNOT DRIVE LOW !!");
            row8(8u, "check solder / wrong pin");
        } else if (tri_on != 0x03u) {
            row8(7u, "!! NO EXT PULL-UP !!");
            row8(8u, "add 3k3 to VDDEX (ds 2.3)");
        } else if (tri_off != 0x00u) {
            row8(7u, "!! FED WHILE VDDEX OFF !!");
            row8(8u, "phantom -> bad POR");
        } else if (g_vddhold == 0u) {
            row8(7u, "pins+pullup OK, no VDDEX cap");
            row8(8u, "hold=0 -> check VDDEX net");
        } else {
            row8(7u, "pins OK + pullup OK + cap OK");
            row8(8u, "-> see scope/scan next");
        }
    }
    uputs("--- s1 done ---\r\n");
    flush();
    NFC_DelayMs(WAIT_MS);

    /* ============ 第 2 屏: 0x54 的 R/W 两种真波形 (VDDEX ON) ================= */
    power_cycle(50u);
    g_k_a8 = NFC_Probe(0x54u, 0u);   /* 0x54 写 = 0xA8 */
    g_k_a9 = NFC_Probe(0x54u, 1u);   /* 0x54 读 = 0xA9 */
    g_k_aa = NFC_Probe(0x55u, 0u);   /* 0x55 写 = 0xAA */
    g_k_ab = NFC_Probe(0x55u, 1u);   /* 0x55 读 = 0xAB */
    NFC_ScopeTx(0xA9u);              /* 画 0x54 读这一笔 */
    g_scp_kind = NFC_ScpKind();
    g_ack_nib  = g_scp_ack;
    g_ack_post = g_scp_post;
    g_k_ctl    = NFC_Probe(0xFFu, 0u);   /* 对照: 一个地址字节都不发 */

    NFC_PowerOff();
    title("2 SCOPE", "START+0x54/0x55+9th @25kHz");
    draw_scope();

    b_reset(); b_str("0x54  W(0xA8)="); b_kind(g_k_a8);
    b_str("  R(0xA9)="); b_kind(g_k_a9); dr(5u);

    b_reset(); b_str("0x55  W(0xAA)="); b_kind(g_k_aa);
    b_str("  R(0xAB)="); b_kind(g_k_ab); dr(6u);

    b_reset(); b_str("ctl(no byte)="); b_kind(g_k_ctl);
    b_str(" trace9th="); b_kind(g_scp_kind); dr(7u);

    b_reset(); b_str("ack=0x"); b_hex8(g_ack_nib);
    b_str(" post="); b_udec((uint16)g_ack_post, 2u);
    b_str(" top=SCL bot=SDA"); dr(8u);
    uputs("--- s2 done ---\r\n");
    flush();
    NFC_DelayMs(WAIT_MS);

    /* ============ 第 3 屏: tBoot 扫描 (VDDEX ON) ============================ */
    for (i = 0u; i < 5u; i++) {
        uint8 bm;
        if      (i == 0u) { bm = 2u;   }
        else if (i == 1u) { bm = 10u;  }
        else if (i == 2u) { bm = 50u;  }
        else if (i == 3u) { bm = 200u; }
        else              { bm = 250u; }   /* NFC_PowerUp 的 bootMs 是 uint8, 用 250 代表 ~1s */
        NFC_PowerOff();
        NFC_DelayMs(50u);
        NFC_PowerUp(bm);
        if (i == 4u) { NFC_DelayMs(750u); }  /* 第 5 档凑到 ~1s */
        NFC_BusRecover();
        g_bw[i] = NFC_Probe(0x54u, 0u);
        g_br[i] = NFC_Probe(0x54u, 1u);
    }
    /* 连探 20 次, 看会不会"自己好起来" */
    NFC_PowerOff(); NFC_DelayMs(50u); NFC_PowerUp(50u); NFC_BusRecover();
    g_rep_real = 0u;
    for (i = 0u; i < 20u; i++) {
        if (NFC_Probe(0x54u, 0u) == NFC_ACK_REAL) { g_rep_real++; }
    }
    NFC_PowerOff();

    title("3 tBOOT", "probe 0x54 after VDDEX + t");
    for (i = 0u; i < 5u; i++) {
        b_reset(); b_str("t=");
        b_udec((uint16)((i == 0u) ? 2u : (i == 1u) ? 10u : (i == 2u) ? 50u : (i == 3u) ? 200u : 1000u), 4u);
        b_str("ms  w="); b_kind(g_bw[i]);
        b_str(" r="); b_kind(g_br[i]);
        dr(i);
    }
    b_reset(); b_str("repeat20 @50ms: real="); b_udec((uint16)g_rep_real, 2u); dr(5u);
    row8(6u, "w = 0x54 write (rd=0)");
    row8(7u, "r = 0x54 read  (rd=1)");
    row8(8u, "any REAL -> pwr/tBoot seq");
    uputs("--- s3 done ---\r\n");
    flush();
    NFC_DelayMs(WAIT_MS);

    /* ============ 第 4 屏: rd0 vs rd1 各扫 128 (VDDEX ON) =================== */
    power_cycle(50u);
    g_n_r0 = NFC_ScanRd(0u, g_f0);
    g_n_r1 = NFC_ScanRd(1u, g_f1);
    g_p54w = NFC_Probe(0x54u, 0u);  g_p54r = NFC_Probe(0x54u, 1u);
    g_p55w = NFC_Probe(0x55u, 0u);  g_p55r = NFC_Probe(0x55u, 1u);
    g_nq_on  = NFC_NirqLevel();
    NFC_PowerOff();
    g_nq_off = NFC_NirqLevel();
    NFC_PowerUp(50u);

    title("4 SCAN", "0x00..0x7F, R/W bit matters?");
    b_reset(); b_str("rd0 real="); b_udec((uint16)g_n_r0, 3u);
    b_str(" f="); b_hex8(g_f0[0]); b_ch(' '); b_hex8(g_f0[1]); b_ch(' '); b_hex8(g_f0[2]);
    dr(0u);

    b_reset(); b_str("rd1 real="); b_udec((uint16)g_n_r1, 3u);
    b_str(" f="); b_hex8(g_f1[0]); b_ch(' '); b_hex8(g_f1[1]); b_ch(' '); b_hex8(g_f1[2]);
    dr(1u);

    b_reset(); b_str("54 w="); b_kind(g_p54w); b_str(" r="); b_kind(g_p54r); dr(2u);
    b_reset(); b_str("55 w="); b_kind(g_p55w); b_str(" r="); b_kind(g_p55r); dr(3u);
    b_reset(); b_str("nq on="); b_dig(g_nq_on); b_str(" off="); b_dig(g_nq_off); dr(4u);

    row8(5u, "rd0=128 & rd1=0 -> scan bug");
    row8(6u, "rd1=128 only -> R/W bit");
    row8(7u, "both 0 -> no slave at all");
    row8(8u, "REAL at 54/55 -> GOT IT");
    uputs("--- s4 done ---\r\n");
    flush();
    NFC_DelayMs(WAIT_MS);

    /* ============ 第 5 屏: 手机贴线圈看 NIRQ (VDDEX ON) ===================== */
    NFC_PowerOff();
    EPD_FrameClear(0xFF);
    EPD_DrawString(X0, YTITLE, "5 RF ALIVE", &Font16, 1);
    row8(1u, "PUT PHONE ON COIL NOW");
    row8(3u, "watching NIRQ for 8s...");
    row8(5u, "VDDEX is ON.");
    row8(6u, "chip alive -> NIRQ pulses");
    flush();
    NFC_DelayMs(4000u);

    NFC_PowerUp(50u);
    g_rf_hit = NFC_NirqWatchMs(8000u, &g_rf_low);
    NFC_PowerOff();

    title("5 RF ALIVE", "phone on coil, watch NIRQ");
    b_reset(); b_str("saw low = "); b_dig(g_rf_hit);
    b_str("  lowCnt = "); b_udec(g_rf_low, 4u); dr(1u);
    row8(3u, "lowCnt>0 => VDDEX really");
    row8(4u, "  reaches chip & IRQ on");
    row8(5u, "=> chip alive, only I2C");
    row8(6u, "   handshake is broken");
    row8(7u, "lowCnt=0 => inconclusive");
    row8(8u, "(IRQSEL may be off)");
    uputs("--- s5 done ---\r\n");
    flush();

    NFC_DelayMs(WAIT_MS);
    uputs("--- loop ---\r\n");
    }                                 /* for(;;) */
}
