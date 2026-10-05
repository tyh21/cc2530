/**
 * @file    main.c
 * @brief   LG290 板 MN63Y1213 —— v8 读写验证固件 (VDDEX 复位脉冲)
 *
 * ============================================================================
 * v7 实测: I2C 物理层完全不通(0 个真 ACK), 盲读全 FF
 *
 * v8 修复 (逆向原厂固件发现):
 *   ★ 核心发现: 原厂固件(0xF586)在 I2C 通信前执行 VDDEX 复位脉冲:
 *     SETB P2.3 → delay(5) → CLR P2.3 → delay(5) → SETB P2.3 → I2C
 *   ★ 即 VDDEX 高→低→高的电源复位脉冲, 触发芯片内部 POR
 *   ★ v5-v7 只做 低→高, 缺少高→低→高脉冲, 导致芯片 I2C 接口不响应
 *   - 降低 I2C 延时: 10us/20us → 3us/5us (原厂约 0.4us)
 *   - 引脚配置一致: SDA=P0.0, SCL=P0.1, VDDEX=P2.3, P0DIR=输入(开漏)
 *
 * v8 四屏验证流程 (每屏全刷, 停 5s):
 *   1 ATTR    读 block 0(16B hex) —— 验证 I2C 是否打通
 *   2 READ    读 NDEF 文本 —— 验证能否读回手机写入的内容
 *   3 WRITE   写 NDEF "Hello CC2530!" —— 验证写入
 *   4 VERIFY  读回验证 + 从地址探测 —— 写后读回确认
 *
 * ============================================================================
 * v6 已诊断: 硬件 DC 层全干净(25kHz/上拉/去耦均 OK), 128 全 REAL 是假象,
 *   盲读全 FF = 无设备响应。根因在软件:
 *     1) cmd_send/resp_read 对每个字节要求 REAL, 多字节长帧必败
 *     2) NDEF 按 Type 2 格式写到地址 0x0000, 覆盖了 Type 3 的 Attribute 块
 *
 * v7 已修复 (nfc.c):
 *   - cmd_send: 从地址字节仍要求 REAL, 数据字节只要不是 NONE 就继续
 *   - resp_read: 读取地址 ACK 只要不是 NONE 就继续
 *   - ReadTextEx: 从 block 1(0x0010) 解析 NDEF, 跳过 block 0 Attribute
 *   - WriteTextEx: 保护 block 0, NDEF 从 block 1 按块写, 更新 Attribute 长度字段
 *
 * v7 四屏验证流程 (每屏全刷, 停 5s):
 *   1 ATTR    读 block 0(16B hex) —— 验证 I2C 是否打通
 *   2 READ    读 NDEF 文本 —— 验证能否读回手机写入的内容
 *   3 WRITE   写 NDEF "Hello CC2530!" —— 验证写入
 *   4 VERIFY  读回验证 + 从地址探测 —— 写后读回确认
 *
 * ★ 从地址: 先试 0x54(手册默认), 不行试 0x55(原厂固件值)
 * ★ 画屏前 NFC_PowerOff() 不必要(I2C 与 EPD 引脚不冲突), 但读写后断电更安全
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

/* ============================ 文本缓冲 =================================== */
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

static void b_kind(uint8 k)
{
    if      (k == NFC_OK)         { b_str("OK"); }
    else if (k == NFC_ERR_NOACK)  { b_str("NOACK"); }
    else if (k == NFC_ERR_STATUS) { b_str("STATUS"); }
    else if (k == NFC_ERR_NDEF)   { b_str("NDEF"); }
    else if (k == NFC_ERR_PARAM)  { b_str("PARAM"); }
    else                          { b_str("?"); }
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

/* ============================ 结果量 ===================================== */
/* 屏1: block 0 Attribute */
static uint8  g_attr[16];
static uint8  g_attr_r;       /* ReadMem 返回码 */

/* 屏2: 读 NDEF */
static char   g_rdbuf[64];
static uint8  g_rd_r;         /* ReadTextEx 返回码 */

/* 屏3: 写 NDEF */
static uint8  g_wr_r;         /* WriteTextEx 返回码 */

/* 屏4: 读回验证 + 从地址探测 */
static char   g_vrbuf[64];
static uint8  g_vr_r;         /* 读回返回码 */
static uint8  g_p54, g_p55;   /* 从地址探测 */
/* g_mem_len 由 NFC_ReadTextEx 填充, 声明在 nfc.h (uint16) */

/* ============================ 上电 ======================================= */
static void power_cycle(uint8 bootMs)
{
    NFC_PowerOff();
    NFC_DelayMs(50u);
    NFC_PowerUp(bootMs);
    NFC_BusRecover();
}

/* 尝试两个从地址读 block 0, 返回成功的从地址; 0 = 都失败 */
static uint8 try_attr(uint8 *out_r)
{
    uint8 r;
    r = NFC_ReadMem(0x54u, 0x0000u, 16u, g_attr, 0);
    if (r == NFC_OK) { *out_r = r; return 0x54u; }
    r = NFC_ReadMem(0x55u, 0x0000u, 16u, g_attr, 0);
    if (r == NFC_OK) { *out_r = r; return 0x55u; }
    *out_r = r;
    return 0u;
}

/* ============================ 主流程 ===================================== */
int main(void)
{
    uint8 i;
    uint8 slv = 0u;        /* 本轮使用的从地址 */
    static char WRITE_TEXT[] = "Hello CC2530!";

    clock_init();
    uart_init();
    uputs("\r\n=== MN63Y1213 v7: Type3 read/write verify ===\r\n");

    NFC_PowerOff();
    NFC_SetSwap(0u);
    NFC_SetDrive(1u);
    NFC_SetOrder(1u);
    NFC_SetSdaPP(0u);
    NFC_T1Init();
    NFC_TimebaseCal();

    EPD_Init();
    EPD_Fill(0xFF);

    for (;;) {
    /* ============ 第 1 屏: 读 block 0 Attribute 块 ====================== */
    power_cycle(50u);
    slv = try_attr(&g_attr_r);
    NFC_PowerOff();

    title("1 ATTR", "read block0 (Type3 attr)");
    b_reset();
    b_str("slv=");
    if (slv != 0u) { b_hex8(slv); } else { b_str("none"); }
    b_str(" r="); b_kind(g_attr_r);
    dr(0u);

    if (g_attr_r == NFC_OK) {
        /* Ver / Nbr / Nbw / NmaxB / NdefLen */
        b_reset(); b_str("Ver=0x"); b_hex8(g_attr[0]);
        b_str(" Nbr="); b_dig(g_attr[1]);
        b_str(" Nbw="); b_dig(g_attr[2]);
        dr(1u);
        b_reset(); b_str("NmaxB=");
        b_udec((uint16)((uint16)g_attr[6] << 8 | g_attr[7]), 3u);
        b_str(" WrF=0x"); b_hex8(g_attr[10]);
        dr(2u);
        b_reset(); b_str("NdefLen=");
        b_udec((uint16)((uint16)g_attr[12] << 8 | g_attr[13]), 3u);
        b_str(" Ln=");
        b_udec((uint16)((uint16)g_attr[14] << 8 | g_attr[15]), 3u);
        dr(3u);
        /* hex 转储 16 字节, 两行各 8 */
        b_reset(); b_str("B0:");
        for (i = 0u; i < 8u; i++) { b_ch(' '); b_hex8(g_attr[i]); }
        dr(4u);
        b_reset(); b_str("    ");
        for (i = 8u; i < 16u; i++) { b_ch(' '); b_hex8(g_attr[i]); }
        dr(5u);
        /* 判读 */
        if (g_attr[0] == 0x10u) {
            row8(7u, "Ver=1.0 Type3 attr OK");
            row8(8u, "-> see READ next");
        } else if (slv == 0u) {
            row8(7u, "!! NO SLAVE ACK !!");
            row8(8u, "check wiring / addr");
        } else {
            row8(7u, "!! UNEXPECTED Ver !!");
            row8(8u, "not Type3 attr?");
        }
    } else {
        row8(4u, "!! READ FAILED !!");
        if (g_attr_r == NFC_ERR_NOACK) {
            row8(5u, "no I2C ack from chip");
            row8(6u, "check VDDEX/SCL/SDA");
        } else if (g_attr_r == NFC_ERR_STATUS) {
            row8(5u, "chip status error");
            row8(6u, "status not MN63Y_ST_OK");
        }
        row8(8u, "fix hw before write");
    }
    uputs("--- s1 done ---\r\n");
    flush();
    NFC_DelayMs(WAIT_MS);

    /* ============ 第 2 屏: 读 NDEF 文本 ================================= */
    if (slv == 0u) { slv = 0x54u; }   /* 读失败也继续, 用默认地址 */
    power_cycle(50u);
    g_rd_r = NFC_ReadTextEx(slv, g_rdbuf, (uint16)sizeof(g_rdbuf));
    NFC_PowerOff();

    title("2 READ", "read NDEF text from block1");
    b_reset(); b_str("slv=0x"); b_hex8(slv);
    b_str(" r="); b_kind(g_rd_r);
    b_str(" mem="); b_udec(g_mem_len, 3u);
    dr(0u);

    if (g_rd_r == NFC_OK) {
        /* 分行显示文本, 每行约 36 字符 */
        uint8 pos = 0u, row = 1u, col = 0u;
        b_reset(); b_str("TEXT: ");
        col = 6u;
        while (g_rdbuf[pos] != '\0' && row <= MAXROW) {
            if (col >= 30u) {
                dr(row); b_reset(); col = 0u; row++;
            }
            b_ch(g_rdbuf[pos]); col++; pos++;
        }
        if (col > 0u && row <= MAXROW) { dr(row); }
        row8(7u, "read OK -> see WRITE");
        row8(8u, "next screen");
    } else if (g_rd_r == NFC_ERR_NDEF) {
        row8(3u, "!! NDEF NOT FOUND !!");
        row8(4u, "block1 has no 'T' rec");
        row8(5u, "maybe empty / wrong");
        row8(6u, "format -> WRITE next");
        row8(7u, "will write new text");
        row8(8u, "in screen 3");
    } else {
        row8(3u, "!! READ FAILED !!");
        row8(4u, "I2C / status error");
        row8(5u, "check screen 1 first");
        row8(8u, "skip write? -> still try");
    }
    uputs("--- s2 done ---\r\n");
    flush();
    NFC_DelayMs(WAIT_MS);

    /* ============ 第 3 屏: 写 NDEF 文本 ================================= */
    power_cycle(50u);
    /* 用宏写入: addr 参数保留兼容但内部从 block 1 写 */
    g_wr_r = NFC_WriteTextEx(slv, 0x0010u, WRITE_TEXT);
    NFC_PowerOff();

    title("3 WRITE", "write NDEF 'Hello CC2530!'");
    b_reset(); b_str("slv=0x"); b_hex8(slv);
    b_str(" r="); b_kind(g_wr_r);
    dr(0u);

    b_reset(); b_str("write: \"");
    {
        uint8 ti = 0u;
        while (WRITE_TEXT[ti] != '\0' && ti < 20u) { b_ch(WRITE_TEXT[ti]); ti++; }
    }
    b_str("\""); dr(1u);

    if (g_wr_r == NFC_OK) {
        row8(2u, "blocks written OK");
        row8(3u, "attr updated");
        row8(5u, "-> see VERIFY next");
        row8(6u, "read back & compare");
        row8(8u, "screen 4");
    } else if (g_wr_r == NFC_ERR_NOACK) {
        row8(3u, "!! WRITE NO ACK !!");
        row8(4u, "I2C failed mid-frame");
        row8(5u, "long frame ack issue");
        row8(6u, "check cmd_send path");
        row8(8u, "see nfc.c v7 notes");
    } else if (g_wr_r == NFC_ERR_STATUS) {
        row8(3u, "!! STATUS ERROR !!");
        row8(4u, "chip rejected write");
        row8(5u, "maybe WrFlag=0xFF");
        row8(6u, "(read-only tag)");
        row8(8u, "check attr[10]");
    } else {
        row8(3u, "!! WRITE FAILED !!");
        row8(4u, "unknown error");
        row8(8u, "check screen 1");
    }
    uputs("--- s3 done ---\r\n");
    flush();
    NFC_DelayMs(WAIT_MS);

    /* ============ 第 4 屏: 读回验证 + 从地址探测 ========================= */
    power_cycle(50u);
    g_vr_r = NFC_ReadTextEx(slv, g_vrbuf, (uint16)sizeof(g_vrbuf));
    NFC_PowerOff();

    /* 从地址探测 (画屏前做, 此时 VDDEX 还在) */
    NFC_PowerUp(20u);
    g_p54 = NFC_Probe(0x54u, 1u);
    g_p55 = NFC_Probe(0x55u, 1u);
    NFC_PowerOff();

    title("4 VERIFY", "read-back & slave probe");
    b_reset(); b_str("rdback r="); b_kind(g_vr_r);
    dr(0u);

    if (g_vr_r == NFC_OK) {
        uint8 pos = 0u, row = 1u, col = 0u, match = 1u;
        /* 对比 */
        {
            uint8 ti = 0u;
            while (WRITE_TEXT[ti] != '\0') {
                if (g_vrbuf[ti] != WRITE_TEXT[ti]) { match = 0u; break; }
                ti++;
                if (ti >= 40u) { break; }
            }
            if (g_vrbuf[ti] != '\0') { match = 0u; }
        }
        b_reset(); b_str("match=");
        if (match) { b_str("YES !!"); } else { b_str("NO"); }
        b_str(" txt:");
        col = 12u;
        while (g_vrbuf[pos] != '\0' && row <= 5u) {
            if (col >= 30u) { dr(row); b_reset(); col = 0u; row++; }
            b_ch(g_vrbuf[pos]); col++; pos++;
        }
        if (col > 0u) { dr(row); }
        if (match) {
            row8(7u, "*** WRITE VERIFY OK ***");
            row8(8u, "Type3 R/W works!");
        } else {
            row8(7u, "!! MISMATCH !!");
            row8(8u, "write or read wrong");
        }
    } else {
        row8(2u, "!! READ-BACK FAILED !!");
        row8(3u, "write may not persist");
        row8(4u, "check attr / status");
        row8(5u, "(write might be OK");
        row8(6u, " but read parse fail)");
        row8(8u, "check NFC Tools on phone");
    }

    /* 从地址探测 */
    {
        uint8 r = 7u;
        /* 如果上面文本太长占了行, 这里跳过 */
        if (g_vr_r == NFC_OK) {
            /* 文本已占行, 不覆盖 */
        } else {
            b_reset(); b_str("probe: 0x54=");
            if (g_p54 == NFC_ACK_REAL) { b_str("REAL"); } else { b_str("none"); }
            b_str(" 0x55=");
            if (g_p55 == NFC_ACK_REAL) { b_str("REAL"); } else { b_str("none"); }
            dr(r);
        }
    }

    uputs("--- s4 done ---\r\n");
    flush();

    NFC_DelayMs(WAIT_MS);
    uputs("--- loop ---\r\n");
    }                                 /* for(;;) */
}
