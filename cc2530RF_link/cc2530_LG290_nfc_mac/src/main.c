/**
 * @file    main.c
 * @brief   LG290 板 MN63Y1213 —— v9a 读写验证固件 (VDDEX 复位脉冲 + 延时双档)
 *
 * ============================================================================
 * v7 实测: I2C 物理层完全不通(0 个真 ACK), 盲读全 FF
 *
 * v8 修复 (逆向原厂固件发现):
 *   ★ 核心发现: 原厂固件(0xF586)在 I2C 通信前执行 VDDEX 复位脉冲:
 *     SETB P2.3 → delay(5) → CLR P2.3 → delay(5) → SETB P2.3 → I2C
 *   ★ 即 VDDEX 高→低→高的电源复位脉冲, 触发芯片内部 POR
 *   ★ v5-v7 只做 低→高, 缺少高→低→高脉冲, 导致芯片 I2C 接口不响应
 *   - I2C 位时序专用延时: 10us/20us → 3us/5us (原厂约 0.4us)
 *   - 引脚配置一致: SDA=P0.0, SCL=P0.1, VDDEX=P2.3, P0DIR=输入(开漏)
 *
 * v9a 修正 (nfc.c): 延时双档分离 —— I2C 位时序 3/5us 专用,
 *   通用 dly_s/dly_l 保持 10/20us (clock9_kind ACK 判定/引脚测量不受影响)
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

/* ============================ 串口 =======================================
 * ★ v22: 改用与 LG213 板完全一致的调试串口 —— USART1 Alt2, TX=P1.6 RX=P1.7,
 *   115200 8N1 (32MHz)。
 *   依据: 213 工程 src/uart0.c 实测通过; 且原厂 LG290 固件本身也是
 *   `ORL PERCFG,#0x02` (U1CFG=1) + `ORL P1SEL,#0xC0` = P1.6/P1.7 调试串口
 *   (见 cc2530_LG213/nfc_analysis.md 第 260 行), 属"回到原厂做法"。
 *   引脚零冲突: 本板 NFC=SDA P0.1/SCL P0.0/PWR P2.3; EPD=P0.2~P0.7,P1.1,P1.2。
 *   P1.6/P1.7 全空闲。
 *   接线: USB-TTL RX <- 板 P1.6(TX); GND 共地即可(只发不收)。
 */
static void uart_init(void)
{
    PERCFG |= 0x02u;          /* U1CFG=1: USART1 -> Alt2 (TX=P1.6, RX=P1.7) */
    P1SEL  |= 0xC0u;          /* P1.6/P1.7 = 外设功能 */
    P2SEL  &= (uint8)~0x20u;  /* PRI2P1=0: USART1 优先于 Timer3 (TI 官方写法) */

    U1CSR   = 0x80u;          /* MODE=1: UART */
    U1UCR   = 0x02u;          /* FLUSH; 8 数据位, 1 停止位, 无校验 */
    U1GCR   = 11u;            /* BAUD_E */
    U1BAUD  = 216u;           /* BAUD_M -> 115200 @32MHz (与 213 一致) */
    UTX1IF  = 0;
}

static uint16 uart_guard;

static void uputc(char c)
{
    U1CSR &= (uint8)~0x02u;   /* 清 U1TX_BYTE */
    U1DBUF = (uint8)c;
    for (uart_guard = 0; uart_guard < 60000u; uart_guard++) {
        if (UTX1IF) { break; }
    }
    UTX1IF = 0;
}

static void uputs(const char *s)
{
    while (*s) { uputc(*s++); }
}

/* ★ v22: 直接输出到串口的小工具 (不经过 lb 缓冲区!) —— 诊断专用 */
static const char __code HEX[] = "0123456789ABCDEF";

static void uh8(uint8 v)
{
    uputc(HEX[(uint8)((v >> 4) & 0x0Fu)]);
    uputc(HEX[(uint8)(v & 0x0Fu)]);
}
static void udig(uint8 v) { uputc((char)('0' + (uint8)(v & 0x0Fu))); }
static void udec(uint16 v)
{
    char  tmp[6];
    uint8 n = 0u;
    do { tmp[n++] = (char)('0' + (uint8)(v % 10u)); v = (uint16)(v / 10u); }
    while (v != 0u && n < 5u);
    while (n > 0u) { uputc(tmp[--n]); }
}

/* ★★ v24 更正 (查 TI CC253x 用户手册 SWRU191F §7.4 确认):
 *   CLKCONCMD / CLKCONSTA 的 **bit6** 才是系统主时钟选择位, 不是 bit7!
 *     bit7  OSC32K  : 0 = 32kHz XOSC(晶体), 1 = 32kHz RCOSC   (复位=1)
 *     bit6  OSC     : 0 = 32MHz XOSC(晶体), 1 = 16MHz RCOSC    (复位=1)
 *     bit5:3 TICKSPD: 定时器 tick 速率 (复位=001 → 16MHz)
 *     bit2:0 CLKSPD : CPU 时钟      (复位=001 → 16MHz)
 *   ⇒ 上一版(v23)我误以为 OSC 是 bit7 而"修正"成 ~0x80 —— **那是错的**,
 *     反而清掉了 OSC32K(把 32kHz 晶体源弄丢)。现已改回 bit6。
 *
 *   原代码 `&=~0x40` / `while(CLKCONSTA&0x40)` / `&=~0x47` 一直是**对的**:
 *     清 bit6 → 选 32MHz XOSC; 等 CLKCONSTA.6 变 0 → 32MHz 已稳定;
 *     再清 TICKSPD/CLKSPD 低 3 位 → 定时器与 CPU 都跑 32MHz。
 *   ★ 唯一保留的改进: 明确不动 bit7(OSC32K), 让 32kHz 源保持芯片默认(晶体),
 *     这样睡眠定时器(ST)才有可能按 32.768kHz 走。 */
static void clock_init(void)
{
    CLKCONCMD &= (uint8)~0x40u;      /* OSC=0 -> 系统主时钟选 32MHz XOSC */
    while (CLKCONSTA & 0x40u) { ; }  /* 等 CLKCONSTA.6 变 0: 32MHz 已稳定 */
    CLKCONCMD &= (uint8)~0x47u;      /* TICKSPD=000, CLKSPD=000 (都跑 32MHz) */
}

/* ============================ 文本缓冲 =================================== */
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

/* ACK 类别单字符: R=真应答 L=迟到 n=无 S=线被钉死 */
static void b_k1(uint8 k)
{
    if      (k == NFC_ACK_REAL)  { b_ch('R'); }
    else if (k == NFC_ACK_LATE)  { b_ch('L'); }
    else if (k == NFC_ACK_STUCK) { b_ch('S'); }
    else                         { b_ch('n'); }
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

/* 屏1 增强: RORF 锁位 + 电源自检 */
static uint8  g_rorf;        /* RORF 系统区字节 @0x01F0 (芯片级 RF-only 只读锁) */
static uint8  g_rorf_r;
static uint8  g_bus;         /* 上电后 SDA/SCL 空闲电平(检测外部上拉) */
static uint8  g_vdd;         /* P2.3(VDDEX) 存活读数 */
/* g_mem_len 由 NFC_ReadTextEx 填充, 声明在 nfc.h (uint16) */

/* 屏0: 物理层 + SCL 频率扫描 */
static uint8  g_ext_nib;     /* 关 MCU 内部上拉后(只留外部上拉)读到的 P0&0x03 低4位 */
static uint8  g_off_nib;     /* VDDEX 断后同上; 应 = 0x00 */
static uint8  g_sh1, g_sh2;  /* 交叉驱动查两线短路: 期望都为 1 */
static uint16 g_hold;        /* VDDEX 放开后还撑几 ms */
static uint8  g_swk[8];      /* ★ v19: 8 档 SCL 周期下的 probe 结果 */
static uint8  g_scn_n;       /* 全地址扫描命中数 */
static uint8  g_scn_f[3];    /* 前 3 个命中地址 */
static uint16 g_meas_us;     /* ★ v17: 用睡眠定时器实测的 SCL 整周期(us) */
static uint8  g_scn_n1;      /* ★ v17: rd=1 的全地址扫描命中数 */

/* 屏0/自动寻优 (v18): 固件自己找到能通的 (swap, SCL周期) 组合 */
static uint8  g_at_ok;       /* 1 = 找到"probe 出 R 且能读出 Ver=0x10"的组合 */
static uint8  g_at_swap;     /* 命中组合的 swap01 (0: SDA=P0.0 / 1: SDA=P0.1) */
static uint8  g_at_us;       /* 命中组合的 SCL 整周期设定(us) */
static uint8  g_at_ver;      /* 命中组合读到的 AIB[0] (合法 Type3 = 0x10) */

/* 屏5: UNLOCK */
static uint8  g_aib2[16];    /* block0 读写缓冲 */
static uint8  g_sys[16];     /* 系统区 0x01F0 读写缓冲 */
static uint8  g_rosi[4];     /* ROSI 读回 */
static uint8  g_un_wr1, g_un_wr2;   /* 写 AIB10 / 写 RORF+ROSI 返回码 */
static uint8  g_un_rd1, g_un_rd2;   /* 读回返回码 */

/* ==================== v20: 自动寻优 (swap × SCL 周期) =====================
 * ★ v20 关键修正 (CSpyBat 实测):
 *   用 CC Debugger 断点跟踪发现:
 *     - swap=0 (SDA=P0.0) : 8 档 probe **全部失败** (读回假 ACK 但过不了第②关)
 *     - swap=1 (SDA=P0.1) : probe **通过** (第 9 轮起触发 ReadMem)
 *   → **这块 290 板的 NFC 前端连线与 213 板/原厂固件假设相反**,
 *     正确配置是 SDA=P0.1 / SCL=P0.0 (**swap=1**).
 *
 *   所以 v20 改动:
 *     ① **swap=1 优先**: 先扫 swap=1, 再扫 swap=0 (旧版相反)
 *     ② 第②关**放宽**: probe 通过 + ReadMem 返回 OK 即锁定 (不强求 b[0]==0x10);
 *        若恰好 b[0]==0x10 则标记为"完美命中"(perfect=1)。
 *     ③ 失败默认值改 swap=1 (与实测正确的硬件配置一致)。
 *
 *   判据 = ① probe 得到 NFC_ACK_REAL 且 ② 紧接着 ReadMem 返回 NFC_OK。
 *   只要读得出数据就认为通了, 这样即便 AIB 内容异常(芯片被写过/非常规初始化)
 *   也不会误判为"未命中"。
 * ======================================================================= */
static uint8 g_at_perfect;   /* ★ v20: 1 = 命中且 AIB[0]==0x10 (完美) */

static void autotune(void)
{
    /* v19: 扩档覆盖 4k~50kHz; v20: 顺序改为 swap=1 优先 */
    static const uint16 U[8] = { 250u, 125u, 100u, 67u, 50u, 33u, 25u, 20u };
    uint8 s, u, k;
    uint8 b[16];

    g_at_ok = 0u; g_at_swap = 1u; g_at_us = 67u; g_at_ver = 0u;
    g_at_perfect = 0u;

    /* ★ v20: swap=1 优先 (实测 290 板的正确配置) */
    uputs("\r\n[tune] start\r\n");                 /* ★ v21: UART 信标 —— 确定性进度 */
    for (s = 0u; s < 2u; s++) {
        uint8 sw = (uint8)((s == 0u) ? 1u : 0u);   /* 第1轮 swap=1, 第2轮 swap=0 */
        for (u = 0u; u < 8u; u++) {
            /* ★ v21: 每轮打一个标记 "sw u" —— 卡住时最后一条就是卡点 */
            uputc('['); uputc((char)('0' + sw)); uputc((char)('0' + u));
            NFC_SetSwap(sw);
            NFC_SetBitUs(U[u]);
            uputc('a');                            /* a = 开始 poweroff */
            NFC_PowerOff();
            uputc('b');                            /* b = poweroff 返回 */
            NFC_DelayMs(30u);
            uputc('c');                            /* c = DelayMs(30) 返回 */
            NFC_PowerUp(50u);
            uputc('d');                            /* d = PowerUp 返回 */
            NFC_BusRecover();
            uputc('e');                            /* e = BusRecover 返回 */

            k = NFC_Probe(0x55u, 0u);
            uputc((k == NFC_ACK_REAL) ? 'P' : 'p'); /* P=真 ACK, p=非 ACK */
            if (k == NFC_ACK_REAL) {
                /* ★ v20 第②关放宽: ReadMem 返回 OK 即算通, 不强求 0x10 */
                if (NFC_ReadMem(0x55u, 0x0000u, 16u, b, 0) == NFC_OK) {
                    g_at_ver = b[0];
                    g_at_ok   = 1u;
                    g_at_swap = sw;
                    g_at_us   = U[u];
                    if (b[0] == 0x10u) { g_at_perfect = 1u; }
                    uputs("] LOCK\r\n");
                    NFC_PowerOff();
                    return;
                } else {
                    uputc('R');                     /* R = probe 过了但 ReadMem 没过 */
                }
            }
            uputc(']'); uputc('\r'); uputc('\n');
            NFC_PowerOff();
        }
    }
    uputs("[tune] all failed\r\n");
    /* 全失败: 用实测正确的硬件配置(swap=1) + 原厂量级频率 */
    g_at_swap = 1u;
    g_at_us   = 67u;
}

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

/* ================== v22: 位层体检 (串口确定性诊断) ========================
 * 目的: 用"读全 0xFF / probe 全 REAL"这个矛盾当线索, 直接在真实硬件上量
 *       第 9 位(ACK 位)每个采样点的 SDA 电平, 判定是否存在**主机自造假 ACK**。
 * 方法: NFC_Decide9() 回传 5 点采样 s5[0..4]
 *         s5[0]=SCL 抬升前(低电平期末)  s5[1]=刚抬 SCL
 *         s5[2]=+100us  s5[3]=+1.1ms(SCL 仍高→真 ACK 必须=0)  s5[4]=SCL 落后+200us
 *   对 0x54/0x55(应 ACK) 和一个**必然不存在**的地址 0x6B(7bit) 分别测:
 *   若 0x6B 也报 REAL → 铁证: ACK 是主机自己造出来的(假 ACK), 不是芯片应答。
 * ========================================================================= */
static void bit_health(void)
{
    /* 0x54/0x55 = 候选取地址; 0x6B 必然无器件 → 若它也应答, 就是假 ACK */
    static const uint8 test_addr[3] = { 0x54u, 0x55u, 0x6Bu };
    uint8 sw, i, k, j;
    uint8 s5[5];

    uputs("\r\n[BIT] ==== bit-layer health ====\r\n");

    /* 0) 时间基准自检: g_tickNs / 位周期 —— 排除"除零/溢出导致延时全乱" */
    uputs("[BIT] tickNs="); udec(NFC_GetTickNs());
    uputs(" bitTick=");     udec(NFC_GetBitTicks());
    uputs(" calOk=");       udig(NFC_GetCalOk());
    uputs(" calT1=");       udec(NFC_GetCalT1());
    uputs(" calSt=");       udec(NFC_GetCalSt());
    uputs(" forced32=");    udig(NFC_GetT1Forced32());
    uputs(" sclUs=");       udec(NFC_MeasSclUs());
    uputs("\r\n");

    /* ★ v24: 直接打印 P2 相关寄存器 —— 看 P2SEL.1/P2INP.3/P2DIR.3 到底什么状态 */
    uputs("[BIT] P2SEL=0x"); uh8(P2SEL);
    uputs(" P2DIR=0x");      uh8(P2DIR);
    uputs(" P2INP=0x");      uh8(P2INP);
    uputs("  (P2SEL.1 应为0, P2DIR.3 应为1)");
    uputs("\r\n");

    /* ★★ v24b: P2.3 逐态穷举 —— 回答"为什么推挽驱高读回还是0" */
    {
        uint8 r;
        P2SEL &= (uint8)~0x02u;          /* P2.3 -> GPIO */
        P2INP |= 0x08u;                  /* 输入时三态 */
        /* 态1: 输出, 驱低 */
        P2DIR |= 0x08u; P2_3 = 0; NFC_DelayMs(2u);
        r = (uint8)(P2 & 0x08u);
        uputs("[P23] out=0 -> P2.3="); udig((uint8)(r ? 1u : 0u));
        /* 态2: 输出, 驱高 */
        P2DIR |= 0x08u; P2_3 = 1; NFC_DelayMs(2u);
        r = (uint8)(P2 & 0x08u);
        uputs("  out=1 -> P2.3=");     udig((uint8)(r ? 1u : 0u));
        /* 态3: 输入(三态), 不驱动 */
        P2DIR &= (uint8)~0x08u; NFC_DelayMs(2u);
        r = (uint8)(P2 & 0x08u);
        uputs("  in(3st)=");           udig((uint8)(r ? 1u : 0u));
        /* 态4: 输入 + 内部上拉 (P2INP.3=0 且 PDUP2=0) */
        P2INP &= (uint8)~0x08u;          /* P2.3 -> 上/下拉模式 */
        P2INP &= (uint8)~0x20u;          /* PDUP2=0 -> 上拉 */
        NFC_DelayMs(2u);
        r = (uint8)(P2 & 0x08u);
        uputs("  in(pullup)=");        udig((uint8)(r ? 1u : 0u));
        /* 态5: 输入 + 内部下拉 */
        P2INP |= 0x20u;                  /* PDUP2=1 -> 下拉 */
        NFC_DelayMs(2u);
        r = (uint8)(P2 & 0x08u);
        uputs("  in(pulldn)=");        udig((uint8)(r ? 1u : 0u));
        uputs("\r\n");
        /* 恢复 */
        P2INP |= 0x08u; P2DIR |= 0x08u; P2_3 = 1;
    }

    /* ★★ v25b: 对照组 P2.2 (TI SWRS081B: pin34 = P2_2, **纯数字 IO**, 非晶振脚)。
     *   若 P2.2 同样方法能驱高读回高, 说明"测量方法本身没问题",
     *   那么 P2.3/P2.4 读 0 就是**这两脚硬件被钳住**的铁证。 */
    {
        uint8 r;
        P2SEL &= (uint8)~0x04u;          /* 清 P2SEL.2(对 P2.4 的功能位, 无关 P2.2, 保险) */
        P2DIR |= 0x04u; P2_2 = 1; NFC_DelayMs(2u);
        r = (uint8)(P2 & 0x04u);
        uputs("[P22] out=1 -> P2.2="); udig((uint8)(r ? 1u : 0u));
        P2DIR |= 0x04u; P2_2 = 0; NFC_DelayMs(2u);
        r = (uint8)(P2 & 0x04u);
        uputs("  out=0 -> P2.2=");     udig((uint8)(r ? 1u : 0u));
        uputs("\r\n");
    }

    /* ★★★ v25d: 真正的对照组 = P2.0 (pin36, 纯数字 IO, 与调试口 P2.1/P2.2 无关)。
     *   ★ 注意: P2.1=P2_1 是 DD、P2.2=P2_2 是 DC (CC2530 调试口),
     *     所以上一条拿 P2.2 当"普通 IO"是**错的** —— 调试器会劫持它。
     *   P2.0 才是干净的自由 IO。若 P2.0 能驱高读回高、P2.3/P2.4 不能,
     *   才真正证明 P2.3/P2.4 这两路被硬件钳住。 */
    {
        uint8 r;
        P2DIR |= 0x01u; P2_0 = 1; NFC_DelayMs(2u);
        r = (uint8)(P2 & 0x01u);
        uputs("[P20] out=1 -> P2.0="); udig((uint8)(r ? 1u : 0u));
        P2DIR |= 0x01u; P2_0 = 0; NFC_DelayMs(2u);
        r = (uint8)(P2 & 0x01u);
        uputs("  out=0 -> P2.0=");     udig((uint8)(r ? 1u : 0u));
        P2DIR &= (uint8)~0x01u;          /* 恢复输入 */
        uputs("\r\n");
    }

    /* ★★ v25c: P2.3 释放后的 RC 衰减 —— 驱高 2ms 充满, 放开(三态), 逐拍看多久掉到 0。
     *   若"立刻掉"(≤1ms) => P2.3 上挂的是**低阻到地**(短路/硬开关管);
     *   若"保持数十~数百ms" => 挂在电容上但没到地, 供电通路可能真的存在。 */
    {
        uint16 ms = 0u;
        P2SEL &= (uint8)~0x02u;
        P2INP |= 0x08u;                  /* 三态, 不内拉 */
        P2DIR |= 0x08u; P2_3 = 1; NFC_DelayMs(2u);
        P2DIR &= (uint8)~0x08u;          /* 放开 */
        while (ms < 300u) { if ((P2 & 0x08u) == 0u) { break; } NFC_DelayMs(1u); ms++; }
        uputs("[P23rc] hold(ms)="); udec(ms);
        uputs("\r\n");
        P2DIR |= 0x08u; P2_3 = 1;
    }

    /* ★★ v24c: 对照组 —— 用同样方法驱动 P2.4, 看是不是"只有 P2.3 坏".
     *   若 P2.4 能被驱高读回高、而 P2.3 不能 => P2.3 这一路硬件被钳住。
     *   (不测 P2.0/P2.1: 那是调试口 DD/DC, 动了会断调试会话) */
    {
        uint8 r;
        /* P2.4: 注意 P2SEL.2 对应 P2.4 */
        P2SEL &= (uint8)~0x04u;
        P2INP |= 0x10u;                  /* P2.4 输入时三态 */
        P2DIR |= 0x10u; P2_4 = 0; NFC_DelayMs(2u);
        r = (uint8)(P2 & 0x10u);
        uputs("[P24] out=0 -> P2.4="); udig((uint8)(r ? 1u : 0u));
        P2DIR |= 0x10u; P2_4 = 1; NFC_DelayMs(2u);
        r = (uint8)(P2 & 0x10u);
        uputs("  out=1 -> P2.4=");     udig((uint8)(r ? 1u : 0u));
        P2INP &= (uint8)~0x10u;
        P2INP &= (uint8)~0x20u;          /* PDUP2=0 -> 上拉 */
        NFC_DelayMs(2u);
        r = (uint8)(P2 & 0x10u);
        uputs("  in(pullup)=");        udig((uint8)(r ? 1u : 0u));
        uputs("\r\n");
    }

    /* ★★★ v25: 32.768kHz 晶振 vs P2.3 硬钳位 判别 —— 决定性实验
     *   TI SWRS081B Table 1: pin33=P2_3/XOSC32K_Q2, pin32=P2_4/XOSC32K_Q1。
     *   v24 实测两脚在任何状态都读 0, 现在分辨是"晶振在钳"还是"硬拉到地"。 */
    {
        uint16 stk = 0u;
        uint8  pk  = NFC_Probe32kXosc(&stk);
        uputs("[X32] pk=0x"); uh8(pk);
        uputs(" st=");        udec(stk);
        uputs("  ->");
        uputs(" osc32k@start=");   udig((uint8)(pk & 0x01u));   /* 0=晶体 1=RC */
        uputs(" forceCry=");       udig((uint8)((pk >> 1) & 1u));/* 0=晶体生效 */
        uputs(" P23_gpio=");       udig((uint8)((pk >> 2) & 1u));/* 1=驱高读回高 */
        uputs(" P23_xosc=");       udig((uint8)((pk >> 3) & 1u));/* 1=晶振模式读高 */
        uputs(" stMoved=");        udig((uint8)((pk >> 4) & 1u));
        uputs(" P24_gpio=");       udig((uint8)((pk >> 5) & 1u));
        uputs(" afterRC_P23=");    udig((uint8)((pk >> 6) & 1u));/* 1=切RCOSC后可驱高 */
        uputs("\r\n");
    }

    for (sw = 0u; sw < 2u; sw++) {
        NFC_SetSwap(sw);
        NFC_SetBitUs(67u);
        uputs("[BIT] swap="); udig(sw);
        uputs((sw == 0u) ? " (SDA=P0.0/SCL=P0.1)" : " (SDA=P0.1/SCL=P0.0)");
        uputs("\r\n");

        NFC_PowerUp(50u);
        /* ★ v24: 供电自检 (两种测法对比) —— 万用表已确认能到 3.2V, 看固件读数是否一致 */
        uputs("[BIT]   vddHold(ms)=");  udec(NFC_VddHoldMs());
        uputs(" vddHold2=");            udec(NFC_VddHoldMs2());
        uputs(" vddAlive=");            udig(NFC_VddAlive());
        uputs(" P2(");                  uh8(P2);
        uputs(")\r\n");
        NFC_BusRecover();
        k = NFC_BusLevel();
        uputs("[BIT]   idle bus=0x"); uh8(k);
        uputs(" (expect 03)\r\n");

        for (i = 0u; i < 3u; i++) {
            for (j = 0u; j < 5u; j++) { s5[j] = 0xFFu; }
            k = NFC_Decide9(test_addr[i], 0u, s5);
            uputs("[BIT]   addr=0x"); uh8(test_addr[i]);
            uputs(" kind=");
            uputs((k == NFC_ACK_REAL)  ? "REAL"  :
                  (k == NFC_ACK_STUCK) ? "STUCK" :
                  (k == NFC_ACK_LATE)  ? "LATE"  : "NONE ");
            uputs(" s5=");
            for (j = 0u; j < 5u; j++) { udig(s5[j]); }
            uputs("\r\n");
            NFC_BusRecover();
        }
        NFC_PowerOff();
    }
    uputs("[BIT] ==== done ====\r\n");
}

/* ============================ 主流程 =====================================
 * ★ v30: 若定义了 V30_RFWATCH(=1), 则本 main() 被下面的 #else 分支跳过,
 *   改用文件末尾的 "RF 场实时观测" main()。 */
#if (!defined(V30_RFWATCH) || (V30_RFWATCH == 0)) && !defined(V40_EPD) && !defined(V45_WAKE)
int main(void)
{
    uint8 i;
    uint8 slv = 0u;        /* 本轮使用的从地址 */
    static char WRITE_TEXT[] = "Hello CC2530!";

    clock_init();
    uart_init();
    uputs("\r\n=== LG290 NFC v22: UART=P1.6/P1.7@115200 (same as LG213) ===\r\n");

    NFC_PowerOff();
    NFC_SetSwap(0u);
    NFC_SetDrive(1u);
    NFC_SetOrder(1u);
    NFC_SetPwrSeq(1u, 1u);    /* v17: 默认 = 发脉冲 + 先碰线(v8-v16 老行为) */
    NFC_SetSdaPP(0u);
    uputc('1');                              /* ★ v21: 已到 T1Init 之前 */
    NFC_T1Init();
    uputc('2');                              /* ★ v21: T1Init 完成 */
    /* ★ v22: T1Init 时钟切换诊断 —— 直接回答"是否卡在 CLKCONSTA 等待" */
    uputs("[T1] cmd0=0x"); uh8(NFC_GetT1ClkCmd0());
    uputs(" sta=0x");      uh8(NFC_GetT1ClkSta());
    uputs(" cmd=0x");      uh8(NFC_GetT1ClkCmd());
    uputs(" loops=");      udec(NFC_GetT1WaitLoops());
    uputs(" t1ok=");       udig(NFC_T1Ok());
    uputs("\r\n");
    NFC_TimebaseCal();
    uputc('3');                              /* ★ v21: TimebaseCal 完成 */
    uputs(" calSpin="); udec(NFC_GetCalSpin());
    uputs(" tickNs=");  udec(NFC_GetTickNs());
    uputs(" calOk=");   udig(NFC_GetCalOk());
    uputs(" cal done\r\n");

    /* ★★★ v26: 万用表验证窗口 —— 固件**无限循环**把 P2.3(VDDEX) 推挽驱高,
     *   每秒串口打点一次。用户可从容用万用表量各点电压, 不用抢时间。
     *
     *   请分别量(对 GND/电池负极):
     *     a) CC2530 P2.3 (QFN40 pin33)         ← 固件驱动点
     *     b) MN63Y1213 VDDEX (那颗 8 脚芯片 pin2)
     *     c) 板上 3.3V 轨 / 电池正极
     *     d) VR0 两端(若找得到 200Ω 那类电阻)
     *   预期判读见 doc/vddex_power_analysis.md。
     *   ★ 想退出该窗口: 按调试器复位/重烧即可(本窗口不返回, 是刻意的)。 */
    {
        uint8 t = 0u;
        P2SEL &= (uint8)~0x02u;      /* P2.3 = GPIO */
        P2INP |= 0x08u;              /* 输入时三态(不内拉) */
        P2DIR |= 0x08u;              /* P2.3 输出 */
        P2_3 = 1;                    /* 驱高 */
        uputs("[VDD] P2.3 held HIGH 3s - measure if needed...\r\n");
        for (t = 0u; t < 3u; t++) {
            uputs("[VDD] t="); udec((uint16)t);
            uputs("s P2=0x");  uh8(P2);
            uputs(" bit3=");   udig((uint8)((P2 & 0x08u) ? 1u : 0u));
            uputs("\r\n");
            /* ★ 不用 NFC_DelayMs: 若 T1 没在计数会退化成超慢/近似卡死。
             *   用固定软件忙等, 与 T1/ST 无关。 */
            { volatile uint32 w = 0u; while (w < 260000UL) { w++; } }
        }
        uputs("[VDD] window done, continuing normal flow...\r\n");
    }

    /* ★★★ v27: 倒灌源定位 —— 决定性实验 (放在 autotune 之前, 避免被慢速 autotune 拖住)
     *   P2.3 驱低(VDDEX OFF) 时, 逐个隔离 SDA/SCL, 看哪根线有电、两线是否耦合。
     *   若 "P2.3=0 时 SDA/SCL 仍有电" -> 证明 SDA/SCL 的上拉不来自 VDDEX,
     *   或 SDA/SCL 经芯片 ESD 二极管从别处倒灌 -> P2.3 的 0.6V 就是倒灌回来的。
     *   ★ v29: 该块每步用 BF_WAIT 忙等最多 200ms, 全程 <1.5s, 不阻塞。 */
    {
        uint16 stk2 = 0u;
        uint8  bf = NFC_DiagBackfeed(&stk2);
        uputs("[BF] bf=0x"); uh8(bf);
        uputs(" st=");       udec(stk2);
        uputs("  (VDDEX=OFF)\r\n");
        uputs("[BF]   a_allFloat=0x");  uh8((uint8)(bf & 0x03u));
        uputs("  b_sdaOnly=");          udig((uint8)((bf >> 2) & 1u));
        uputs("  c_sclOnly=");          udig((uint8)((bf >> 3) & 1u));
        uputs("\r\n");
        uputs("[BF]   d_pullSDA_seeSCL="); udig((uint8)((bf >> 4) & 1u));
        uputs("  e_pullSCL_seeSDA=");      udig((uint8)((bf >> 5) & 1u));
        uputs("  f_vddexON=0x");           uh8((uint8)((bf >> 6) & 0x03u));
        uputs("\r\n");
        uputs("[BF]   g_P23readback=");    udig(g_bfP23);
        uputs("  h_P23holdMs=");           udec(g_bfHold);
        uputs("\r\n");
    }

    /* ★★★ v27h: 同场对照 —— P2.0(已知好 IO) vs P2.3, 用完全相同代码路径。
     *   若 P2.0 驱高读 1、P2.3 驱高读 0 => 测量方法无误, P2.3 硬件被钳。
     *   ★ v29: 只做 1 轮、每步 300ms 忙等(原来 2 轮 × 2s × 2 = 8s, 太拖时间)。 */
    {
        uint8 r20, r23;
        uint8 rp0, rp1;
        /* --- P0.0 / P0.1 (SDA/SCL) 推挽驱高读回 --- */
        P0SEL &= (uint8)~0x03u;
        P0DIR |= 0x03u;  P0_0 = 1; P0_1 = 1;
        { volatile uint32 w = 0u; while (w < 78000UL) { w++; } }  /* ~300ms */
        rp0 = (uint8)((P0 & 0x01u) ? 1u : 0u);
        rp1 = (uint8)((P0 & 0x02u) ? 1u : 0u);
        uputs("[CMP0] P0.0_driveHi="); udig(rp0);
        uputs("  P0.1_driveHi=");      udig(rp1);
        uputs("\r\n");

        /* --- P2.0 对照 --- */
        P2SEL &= (uint8)~0x01u;          /* P2SEL.0 = P2.0 功能位 -> GPIO */
        P2INP |= 0x01u;                  /* P2.0 输入时三态 */
        P2DIR |= 0x01u; P2_0 = 1;
        { volatile uint32 w = 0u; while (w < 78000UL) { w++; } }  /* ~300ms */
        r20 = (uint8)((P2 & 0x01u) ? 1u : 0u);

        /* --- P2.3 --- */
        P2SEL &= (uint8)~0x02u;
        P2INP |= 0x08u;
        P2DIR |= 0x08u; P2_3 = 1;
        { volatile uint32 w = 0u; while (w < 78000UL) { w++; } }  /* ~300ms */
        r23 = (uint8)((P2 & 0x08u) ? 1u : 0u);

        uputs("[CMP] P2.0_driveHi="); udig(r20);
        uputs("  P2.3_driveHi=");     udig(r23);
        uputs("\r\n");
        P2DIR &= (uint8)~0x01u;
    }

    /* ★★★ v28: 寄生供电探测 —— SDA/SCL 推挽驱高给芯片充电, 不碰 P2.3 直接探测。
     *   若芯片真能靠 ESD 倒灌"寄生供电"而应答 -> 不改硬件也有救。 */
    {
        static uint8 s5p[5];
        uint8 kp;
        uint8 gi;
        /* 对照: 0xFF = 不发地址字节(绝不可能应答) */
        kp = NFC_ProbeParasitic(0xFFu, 200u, s5p);
        uputs("[PARA] ctrl(0xFF) kind="); udig(kp);
        uputs(" s5=");  for (gi = 0u; gi < 5u; gi++) { udig(s5p[gi]); }
        uputs("\r\n");
        /* 真地址 0x55 与 0x54 */
        kp = NFC_ProbeParasitic(0x55u, 200u, s5p);
        uputs("[PARA] 0x55 kind="); udig(kp);
        uputs(" s5=");  for (gi = 0u; gi < 5u; gi++) { udig(s5p[gi]); }
        uputs("\r\n");
        kp = NFC_ProbeParasitic(0x54u, 200u, s5p);
        uputs("[PARA] 0x54 kind="); udig(kp);
        uputs(" s5=");  for (gi = 0u; gi < 5u; gi++) { udig(s5p[gi]); }
        uputs("\r\n");
    }

    /* ===========================================================================
     * ★★★ v29: 「已知上电程序 → 驱动 NFC」 最小闭环验证 ★★★
     *
     * 用户要求: 不改硬件, 用已知可用的上电程序(原厂 0xF586 / 0x7D30 那套)把
     *   NFC 芯片驱动起来。本段把「原厂上电序列 + 直接块寻址读 block0」跑成
     *   一条最短路径, 串口逐字打印每一步, 拿确定性证据回答:
     *     ① 上电脉冲打完, 芯片到底有没有 ACK?
     *     ② 有 ACK 的话, block0 头两字节是不是 Type3 的 0x10 0x00?
     *     ③ 5 点长保持判据(s5)在每一步长什么样 —— 区分"真 ACK"与"边沿耦合假象"。
     *
     * 时序: 全部用 NFC_DelayMs, 它在 T1/ST 都不可用时已走**固定忙等兜底**(v29),
     *   所以这段不会再像旧 autotune 那样几十秒不返回。
     * =========================================================================== */
    uputs("\r\n===== v29 NFC BRING-UP (known-good power-up) =====\r\n");
    {
        /* ---------- 步骤 0: ★★★ 总线 DC 体检 —— 先确认两根线本身是不是好的 ----------
         *   上一轮 [BIT] 段出现 "0x55 和 0x6B 都 REAL，s5=00001" —— 真从机不可能
         *   这样。最可能的解释: SDA 线在 SCL 高电平期被**钉在低**(线/芯片把 SDA 钳住),
         *   于是任何地址都"读到 0" = 假 ACK。这里把它量出来:
         *     a) 推挽驱高 SDA 再读回: 若读回 0 -> SDA 被外部强拉低 (铁证)
         *     b) 两线全放开(三态,关内拉)再读: 若 SDA=0 而 SCL=1 -> 外部只在拉 SDA
         *     c) 驱低 SDA 200ms 后看 SCL: 若 SCL 跟着掉 -> 两线耦合/短路
         * ========================================================================= */
        P2SEL &= (uint8)~0x02u;
        P2INP |= 0x08u;  P2DIR |= 0x08u;  P2_3 = 0;   /* VDDEX = OFF */
        NFC_DelayMs(5u);

        /* a) SDA 推挽驱高读回 / SCL 推挽驱高读回 */
        P0SEL &= (uint8)~0x03u;
        P0IEN &= (uint8)~0x03u;
        P0DIR |= 0x03u;  P0_0 = 1; P0_1 = 1;
        NFC_DelayMs(5u);
        uputs("[BU0] driveHi SDA=");  udig((uint8)((P0 & 0x01u) ? 1u : 0u));
        uputs(" SCL=");              udig((uint8)((P0 & 0x02u) ? 1u : 0u));

        /* b) 全放开 + 关内部上拉(三态) -> 只看外部上拉 */
        P0DIR &= (uint8)~0x03u;
        P0INP |= 0x03u;                       /* 三态 */
        P2INP |= 0x20u;                       /* PDUP0=1: 关 Port0 上拉 */
        NFC_DelayMs(5u);
        uputs("  floatNoPU SDA=");   udig((uint8)((P0 & 0x01u) ? 1u : 0u));
        uputs(" SCL=");              udig((uint8)((P0 & 0x02u) ? 1u : 0u));

        /* c) 全放开 + 开内部上拉 -> 看两线能否被拉高 */
        P0INP &= (uint8)~0x03u;
        P2INP &= (uint8)~0x20u;
        NFC_DelayMs(5u);
        uputs("  floatPU  SDA=");    udig((uint8)((P0 & 0x01u) ? 1u : 0u));
        uputs(" SCL=");              udig((uint8)((P0 & 0x02u) ? 1u : 0u));
        uputs("\r\n");

        /* d) 驱低 SDA 200ms, 看 SCL 是否被拽低(耦合/短路) */
        P0DIR |= 0x01u; P0_0 = 0;
        P0DIR &= (uint8)~0x02u; P0_1 = 1;
        NFC_DelayMs(200u);
        uputs("[BU0] pullSDA_low -> SCL="); udig((uint8)((P0 & 0x02u) ? 1u : 0u));
        /* e) 反向 */
        P0DIR |= 0x02u; P0_1 = 0;
        P0DIR &= (uint8)~0x01u; P0_0 = 1;
        NFC_DelayMs(200u);
        uputs("  pullSCL_low -> SDA=");     udig((uint8)((P0 & 0x01u) ? 1u : 0u));
        uputs("\r\n");

        /* f) ★ 决定性: SDA 推挽驱高保持, 逐步加长, 看能否读回 1 */
        P0DIR |= 0x01u; P0_0 = 1;
        {
            uint8 tb;
            for (tb = 0u; tb < 4u; tb++) {
                NFC_DelayMs(5u);
                uputs("[BU0] SDA_hi+"); udec((uint16)((tb + 1u) * 5u));
                uputs("ms -> read=");   udig((uint8)((P0 & 0x01u) ? 1u : 0u));
                uputs("\r\n");
            }
        }
        /* g) ★★★ 决定性因果实验: 两线的"高"到底是不是 P2.3(VDDEX) 给的?
         *   做法: 两线全放开+关内拉(三态), 只改 P2.3 一个变量, 看两线电平跟不跟。
         *     g1) P2.3 = 0 (VDDEX OFF)  -> 读两线
         *     g2) P2.3 = 1 (VDDEX ON)   -> 读两线
         *     g3) P2.3 松开(三态)       -> 读两线(靠芯片/电容残余)
         *   若 g1=00 而 g2=11 => **SDA/SCL 的上拉源就是 VDDEX 网络**(3.3k 上拉),
         *   那么 VDDEX 上不去 -> 两线永远低 -> 芯片也永远没电 -> 死结。
         *   若 g1=11 (P2.3=0 时两线仍高) => 上拉来自别的 3.3V 轨, 那问题在别处。 */
        P0SEL &= (uint8)~0x03u;
        P0DIR &= (uint8)~0x03u;      /* 两线都输入 */
        P0INP |= 0x03u;              /* 三态, 关内拉 */
        P2INP |= 0x20u;              /* 关 Port0 上拉 */
        P0IEN &= (uint8)~0x03u;
        P2SEL &= (uint8)~0x02u;      /* P2.3 -> GPIO */
        P2INP |= 0x08u;              /* P2.3 输入时三态 */
        P2DIR |= 0x08u;

        P2_3 = 0;  NFC_DelayMs(20u);
        uputs("[BU0] g1 P23=0 -> SDA="); udig((uint8)((P0 & 0x01u) ? 1u : 0u));
        uputs(" SCL=");                  udig((uint8)((P0 & 0x02u) ? 1u : 0u));

        P2_3 = 1;  NFC_DelayMs(20u);
        uputs("  g2 P23=1 -> SDA=");     udig((uint8)((P0 & 0x01u) ? 1u : 0u));
        uputs(" SCL=");                  udig((uint8)((P0 & 0x02u) ? 1u : 0u));

        P2DIR &= (uint8)~0x08u;      /* P2.3 放开 */
        NFC_DelayMs(20u);
        uputs("  g3 P23=Z -> SDA=");     udig((uint8)((P0 & 0x01u) ? 1u : 0u));
        uputs(" SCL=");                  udig((uint8)((P0 & 0x02u) ? 1u : 0u));
        uputs("\r\n");

        P2DIR |= 0x08u; P2_3 = 0;
        P0DIR &= (uint8)~0x03u;
        NFC_DelayMs(5u);

        /* h) ★★★ 复核: 走完整 gpio_prep(推挽) 流程后再全三态, 看两线是否掉。
         *   这直接判定 [BIT] 段 "idle bus=0x03" 到底是外部上拉还是 MCU 推挽自造。 */
        NFC_SetSwap(0u);
        NFC_GpioPrepPub();           /* P0.0/P0.1 推挽 + P2.3 输出高 */
        P2_3 = 1;  NFC_DelayMs(10u);
        uputs("[BU0] gpio_prep+P23=1 推挽态 SDA=");
        udig((uint8)((P0 & 0x01u) ? 1u : 0u));
        uputs(" SCL="); udig((uint8)((P0 & 0x02u) ? 1u : 0u));
        /* 立刻全放开(三态,关内拉) —— 若掉到 0 说明"高"全靠 MCU 推挽 */
        P0DIR &= (uint8)~0x03u;
        P0INP |= 0x03u;
        P2INP |= 0x20u;
        NFC_DelayMs(10u);
        uputs("  -> 三态后 SDA=");
        udig((uint8)((P0 & 0x01u) ? 1u : 0u));
        uputs(" SCL="); udig((uint8)((P0 & 0x02u) ? 1u : 0u));
        uputs("\r\n");

        NFC_DelayMs(5u);
        uputs("===== v29 BRING-UP start =====\r\n");
    }
    {
        static uint8 s5b[5];
        uint8  k;
        uint8  b0[16];
        uint8  gi;

        /* ---------- 步骤 1: 原厂上电序列 (pulse=1, early=1 + 5ms tBoot) ---------- */
        NFC_SetSwap(0u);                 /* SDA=P0.0 / SCL=P0.1 —— 原厂 gpio_prep 的接法 */
        NFC_SetPwrSeq(1u, 1u);           /* 发 VDDEX 高→低→高 复位脉冲, 先碰线再上电 */
        NFC_PowerOff();
        NFC_DelayMs(50u);
        NFC_PowerUp(50u);                /* 原厂: SETB→5ms→CLR→5ms→SETB→tBoot */
        NFC_BusRecover();
        uputs("[BU] step1 power-up done (swap=0 pulse=1 early=1)\r\n");

        /* ---------- 步骤 2: 5 点长保持判决探 0x55 ---------- */
        k = NFC_Decide9(0x55u, 1u, s5b);
        uputs("[BU] step2 probe 0x55 kind="); udig(k);
        uputs(" s5="); for (gi = 0u; gi < 5u; gi++) { udig(s5b[gi]); }
        uputs("\r\n");

        /* ---------- 步骤 3: 若命中, 直接读 block0 前 16 字节 ---------- */
        if (NFC_ReadMem(0x55u, 0x0000u, 16u, b0, 0) == NFC_OK) {
            uputs("[BU] step3 read block0 OK: ");
            for (gi = 0u; gi < 16u; gi++) { uh8(b0[gi]); uputc(' '); }
            uputs("\r\n");
            uputs("[BU] step3 Ver=0x"); uh8(b0[0]);
            uputs((b0[0] == 0x10u) ? "  Type3 AIB OK!\r\n" : "  (unexpected)\r\n");
        } else {
            uputs("[BU] step3 read block0 FAILED (no ack)\r\n");
        }
        NFC_PowerOff();

        /* ---------- 步骤 4: 同上但 swap=1 (290 板实测正确接法) ---------- */
        NFC_SetSwap(1u);
        NFC_PowerUp(50u);
        NFC_BusRecover();
        k = NFC_Decide9(0x55u, 1u, s5b);
        uputs("[BU] step4 swap=1 probe kind="); udig(k);
        uputs(" s5="); for (gi = 0u; gi < 5u; gi++) { udig(s5b[gi]); }
        uputs("\r\n");
        if (NFC_ReadMem(0x55u, 0x0000u, 16u, b0, 0) == NFC_OK) {
            uputs("[BU] step4 read block0 OK: ");
            for (gi = 0u; gi < 16u; gi++) { uh8(b0[gi]); uputc(' '); }
            uputs("\r\n");
        } else {
            uputs("[BU] step4 read block0 FAILED\r\n");
        }
        NFC_PowerOff();

        /* ---------- 步骤 5: 对照组 —— 不发地址字节(0xFF), 必须全高 ---------- */
        NFC_SetSwap(1u);
        NFC_PowerUp(50u);
        NFC_BusRecover();
        k = NFC_Decide9(0xFFu, 1u, s5b);
        uputs("[BU] step5 ctrl(0xFF) kind="); udig(k);
        uputs(" s5="); for (gi = 0u; gi < 5u; gi++) { udig(s5b[gi]); }
        uputs("  (期望 kind=2 NONE, s5=11111)\r\n");
        NFC_PowerOff();

        /* ---------- 步骤 6: 反相序 —— 先上 VDDEX 稳定再碰线(early=0) ---------- */
        NFC_SetSwap(1u);
        NFC_SetPwrSeq(1u, 0u);
        NFC_PowerOff();
        NFC_DelayMs(50u);
        NFC_PowerUp(50u);
        NFC_BusRecover();
        k = NFC_Decide9(0x55u, 1u, s5b);
        uputs("[BU] step6 early=0 probe kind="); udig(k);
        uputs(" s5="); for (gi = 0u; gi < 5u; gi++) { udig(s5b[gi]); }
        uputs("\r\n");
        if (NFC_ReadMem(0x55u, 0x0000u, 16u, b0, 0) == NFC_OK) {
            uputs("[BU] step6 read block0 OK: ");
            for (gi = 0u; gi < 16u; gi++) { uh8(b0[gi]); uputc(' '); }
            uputs("\r\n");
        } else {
            uputs("[BU] step6 read block0 FAILED\r\n");
        }
        NFC_PowerOff();

        /* ---------- 步骤 7: 无脉冲(只做低→高), 复刻手册 §5.2.6 最简上电 ---------- */
        NFC_SetPwrSeq(0u, 0u);
        NFC_PowerOff();
        NFC_DelayMs(50u);
        NFC_PowerUp(60u);                /* 手册 tBoot 之后才能访问 */
        NFC_BusRecover();
        k = NFC_Decide9(0x55u, 1u, s5b);
        uputs("[BU] step7 no-pulse probe kind="); udig(k);
        uputs(" s5="); for (gi = 0u; gi < 5u; gi++) { udig(s5b[gi]); }
        uputs("\r\n");
        if (NFC_ReadMem(0x55u, 0x0000u, 16u, b0, 0) == NFC_OK) {
            uputs("[BU] step7 read block0 OK: ");
            for (gi = 0u; gi < 16u; gi++) { uh8(b0[gi]); uputc(' '); }
            uputs("\r\n");
        } else {
            uputs("[BU] step7 read block0 FAILED\r\n");
        }
        NFC_PowerOff();

        NFC_SetPwrSeq(1u, 1u);           /* 还原默认 */
        uputs("===== v29 BRING-UP end =====\r\n\r\n");
    }

    /* ★★ v18: 固件自己找"能通的" (swap, SCL周期) 组合, 找到就锁死给后面所有屏用
     *    判据 = probe 出真 ACK **且** 能读出合法 Type3 AIB(Ver=0x10) */
    autotune();
    uputc('4');                              /* ★ v21: autotune 返回 */
    NFC_SetSwap(g_at_swap);
    NFC_SetBitUs(g_at_us);

    /* ★ v22: autotune 之后, 用串口把"位层真实电平"打出来 (确定性诊断),
     *   并对 swap=0 / swap=1 两种接法各测一遍 —— 直接回答"到底哪根是 SDA"。 */
    bit_health();

    EPD_Init();
    uputc('5');                              /* ★ v21: EPD_Init 完成 */
    EPD_Fill(0xFF);

    for (;;) {
    /* ============ 第 0 屏: 物理层自检 + SCL 频率扫描 =====================
     * 回答"电平全对(v15 屏1 bus=0x03 vdd=HI)却整条总线 NOACK"到底为什么:
     *   A) ext  = 关掉 MCU 内部上拉(三态)后读总线 -> 只有**外部**上拉能拉高。
     *      手册 §2.2: "I2C is N-ch open drain, pulled up to VDDEX externally"
     *      => ext 必须 = 0x03, 否则外部上拉缺失(电平高是 MCU 内拉假象)
     *   B) off  = VDDEX 断后总线电平, 必须 = 0x00(线没有倒灌)
     *   C) short= 交叉驱动: 驱 SDA 低看 SCL 会不会跟着低(两线短路/耦合)
     *   D) SCL 频率扫描: 手册 §5: "available between 20 kHz and 100 kHz"
     *      原复刻实际 67us/周期 = 14.9kHz, **低于下限**。逐档探 0x55 看哪档应答。
     *   E) 全地址扫描: 确认总线上到底有没有任何器件。
     * ==================================================================== */
    {
        static const uint16 SW_US[8] = { 250u, 125u, 100u, 67u, 50u, 33u, 25u, 20u };

        /* A/B: 外部上拉真伪 + VDDEX 断电时线是否跟着掉 + 保持时间 */
        NFC_PowerOff();
        NFC_DelayMs(20u);
        g_off_nib = (uint8)(NFC_BusOff() & 0x0Fu);
        NFC_PowerUp(50u);
        g_ext_nib = (uint8)(NFC_BusOn() & 0x0Fu);
        g_hold    = NFC_VddHoldMs();

        /* C: 两线短路/耦合: 驱一根拉低, 另一根应保持高(读回 1=正常) */
        NFC_PowerUp(20u);
        g_meas_us = NFC_MeasSclUs();             /* ★ v17: 实测 SCL 整周期(us) */
        g_sh1 = NFC_DriveAndReadOther(0u, 0u);   /* 驱 SDA 低 -> 读 SCL */
        g_sh2 = NFC_DriveAndReadOther(1u, 0u);   /* 驱 SCL 低 -> 读 SDA */
        NFC_PowerOff();

        /* D: SCL 频率扫描(在 autotune 命中的 swap 下) —— 逐档探 0x55 */
        NFC_SetSwap(g_at_swap);
        NFC_BitSave();
        for (i = 0u; i < 8u; i++) {
            NFC_SetBitUs(SW_US[i]);
            NFC_PowerUp(20u);
            NFC_BusRecover();
            g_swk[i] = NFC_Probe(0x55u, 0u);
            NFC_PowerOff();
        }

        /* E: 全地址扫描(20us 档), 写/读两种 R/W 位都扫一遍
         *    ★ v17: 只扫 rd=0 会漏掉"位序反过来"的从机(那时 0xAA 会变成 0x55,
         *      落到 rd=1 那一组), 所以两组都扫。 */
        NFC_SetBitUs(g_at_us);
        NFC_PowerUp(20u);
        NFC_BusRecover();
        g_scn_f[0] = 0xFFu; g_scn_f[1] = 0xFFu; g_scn_f[2] = 0xFFu;
        g_scn_n  = NFC_ScanRd(0u, g_scn_f);
        g_scn_n1 = NFC_ScanRd(1u, g_scn_f);
        NFC_PowerOff();
        NFC_BitRestore();

        title("0 BUS", "auto-tune(swap x scl) + PHY");

        /* ★ v18: 第一行 = 自动寻优结论(最关键的一行)
         *   sw0 = SDA 在 P0.0(固件原假设) / sw1 = SDA 在 P0.1(交换) */
        b_reset(); b_str("tune sw"); b_dig(g_at_swap);
        b_str(" us"); b_udec(g_at_us, 2u);
        b_str(" v"); b_hex8(g_at_ver);
        if (g_at_ok) { b_str(g_at_perfect ? " OK!" : " OK?"); }
        else         { b_str(" NG"); }
        dr(0u);

        b_reset(); b_str("ext="); b_hex8(g_ext_nib);
        b_str(" off="); b_hex8(g_off_nib);
        b_str(" hold="); b_udec(g_hold, 3u);
        dr(1u);

        b_reset(); b_str("sh="); b_dig(g_sh1); b_ch('/'); b_dig(g_sh2);
        b_str(" meas="); b_udec(g_meas_us, 3u); b_ch('u');
        dr(2u);

        b_reset(); b_str("us : ");
        for (i = 0u; i < 8u; i++) { b_udec(SW_US[i], 3u); b_ch(' '); }
        dr(3u);
        b_reset(); b_str("ack: ");
        for (i = 0u; i < 8u; i++) { b_k1(g_swk[i]); b_ch(' '); }
        dr(4u);

        b_reset(); b_str("scan w="); b_udec(g_scn_n, 2u);
        b_str(" r="); b_udec(g_scn_n1, 2u);
        dr(5u);
        row8(6u, "sw1 = SDA on P0.1 (swapped!)");
        row8(7u, "ext!=3->noPU  sh!=1->short");
        row8(8u, "any ack=R -> chip here");
        uputs("--- s0 done ---\r\n");
        flush();
        NFC_DelayMs(WAIT_MS);
    }

    /* ============ 第 1 屏: 读 block 0 AIB + RORF 锁位 + 电源自检 ======== */
    power_cycle(50u);
    g_bus = NFC_BusLevel();          /* 上电后 SDA/SCL 空闲电平(VDDEX+上拉决定) */
    g_vdd = NFC_VddAlive();          /* P2.3(VDDEX) 是否真被拉高 */
    slv = try_attr(&g_attr_r);
    /* 读 RORF 系统区 @0x01F0: 真正锁 RF 只读的位(芯片级, RF-only 锁) */
    g_rorf_r = (slv != 0u) ? NFC_ReadMem(slv, 0x01F0u, 1u, &g_rorf, 0)
                           : NFC_ERR_NOACK;
    NFC_PowerOff();

    title("1 ATTR", "block0 AIB + RORF@1F0");
    b_reset();
    b_str("slv=");
    if (slv != 0u) { b_hex8(slv); } else { b_str("none"); }
    b_str(" r="); b_kind(g_attr_r);
    dr(0u);

    /* 电源/上拉自检: SDA/SCL 都应被上拉到高(=0x03) */
    b_reset();
    b_str("bus=0x"); b_hex8(g_bus);
    b_str((g_bus == 0x03u) ? " puOK" : " NO-PU!");
    b_str(" vdd="); b_str(g_vdd ? "HI" : "LO");
    dr(1u);

    if (g_attr_r == NFC_OK) {
        b_reset(); b_str("Ver=0x"); b_hex8(g_attr[0]);
        b_str(" Nbr="); b_dig(g_attr[1]);
        b_str(" Nbw="); b_dig(g_attr[2]);
        dr(2u);
        b_reset(); b_str("NdefLen=");
        b_udec((uint16)((uint16)g_attr[12] << 8 | g_attr[13]), 3u);
        b_str(" AIBwr=0x"); b_hex8(g_attr[10]);
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
            row8(7u, "Ver=1.0 Type3 OK");
        } else if (slv == 0u) {
            row8(7u, "!! NO SLAVE ACK !!");
        } else {
            row8(7u, "!! UNEXPECTED Ver !!");
        }
    } else {
        row8(4u, "!! READ FAILED !!");
        if (g_attr_r == NFC_ERR_NOACK) {
            row8(5u, "no I2C ack");
            row8(6u, "check VDDEX/PU");
        } else if (g_attr_r == NFC_ERR_STATUS) {
            row8(5u, "chip status err");
        }
    }

    /* RF 只读锁位(RORF)解读 */
    if (g_rorf_r == NFC_OK) {
        b_reset(); b_str("RORF@1F0=0x"); b_hex8(g_rorf);
        b_str((g_rorf == 0x00u) ? " RF-RW" : " RF-RO!");
        dr(6u);
        if (g_rorf != 0x00u) { row8(8u, "phone write blocked"); }
        else { row8(8u, "phone can write"); }
    } else if (g_attr_r == NFC_OK) {
        b_reset(); b_str("RORF read fail");
        dr(6u);
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

    /* ============ 第 5 屏: UNLOCK —— 解除只读锁 =========================
     * 手册 §3.3.1: 两把锁都是 **FeRAM 字节, 非 OTP**, 且 I2C 主机可写:
     *   RORF   (系统区 0x01F0..0x01F3): RF 模式读/写 vs 只读 (1=只读)
     *   ROSI   (系统区 0x01F4..0x01F7): **串行(I2C)模式**读/写 vs 只读 (1=只读)
     *   AIB[10](block0 字节10)        : Type3 NDEF 访问标志 (0x00=可写 0xFF=只读)
     * 做法: 读-改-写整块(保留 SECURITY 0x01F8..0x01FF 不动), 再读回验证。
     * ★ 注意: I2C 若仍 NOACK(看屏0), 本屏会显示失败 —— 先解决总线才能解锁。
     * ==================================================================== */
    {
        uint8 uslv = (slv != 0u) ? slv : 0x55u;

        power_cycle(50u);

        /* 1) AIB block0 字节10 -> 0x00 (允许 RF 写 NDEF) */
        g_un_rd1 = NFC_ReadMem(uslv, 0x0000u, 16u, g_aib2, 0);
        if (g_un_rd1 == NFC_OK) {
            g_aib2[10] = 0x00u;
            g_un_wr1 = NFC_WriteMem(uslv, 0x0000u, g_aib2, 16u, 0);
        } else {
            g_un_wr1 = NFC_ERR_NOACK;
        }

        /* 2) RORF(1F0..1F3) + ROSI(1F4..1F7) -> 0x00 (清两块只读) */
        g_un_rd2 = NFC_ReadMem(uslv, 0x01F0u, 16u, g_sys, 0);
        if (g_un_rd2 == NFC_OK) {
            for (i = 0u; i < 8u; i++) { g_sys[i] = 0x00u; }
            g_un_wr2 = NFC_WriteMem(uslv, 0x01F0u, g_sys, 16u, 0);
        } else {
            g_un_wr2 = NFC_ERR_NOACK;
        }

        /* 3) 读回验证 */
        g_un_rd1 = NFC_ReadMem(uslv, 0x0000u, 16u, g_aib2, 0);
        g_un_rd2 = NFC_ReadMem(uslv, 0x01F4u, 4u, g_rosi, 0);
        NFC_PowerOff();

        title("5 UNLOCK", "clear RORF/ROSI + AIB10");

        b_reset(); b_str("slv=0x"); b_hex8(uslv);
        b_str(" wAIB="); b_kind(g_un_wr1);
        dr(0u);

        b_reset(); b_str("w ROSI(1F4)="); b_kind(g_un_wr2);
        dr(1u);

        b_reset(); b_str("ROSI: ");
        if (g_un_rd2 == NFC_OK) {
            uint8 z;
            for (z = 0u; z < 4u; z++) { b_hex8(g_rosi[z]); b_ch(' '); }
        } else { b_str("--"); }
        dr(2u);

        b_reset(); b_str("AIB10 now=0x");
        if (g_un_rd1 == NFC_OK) { b_hex8(g_aib2[10]); } else { b_str("--"); }
        dr(3u);

        if ((g_un_wr1 == NFC_OK) && (g_un_wr2 == NFC_OK)) {
            row8(5u, "*** UNLOCK DONE ***");
            row8(6u, "locks cleared (FeRAM)");
            row8(7u, "phone can write now");
            row8(8u, "re-probe w/ NFC Tools");
        } else {
            row8(5u, "!! UNLOCK FAILED !!");
            row8(6u, "I2C still NOACK");
            row8(7u, "fix screen 0 BUS first");
            row8(8u, "then revisit here");
        }
        uputs("--- s5 UNLOCK done ---\r\n");
        flush();
        NFC_DelayMs(WAIT_MS);
    }

    /* ============ 第 6 屏: 上电序列 × SDA/SCL 交换 组合扫描 ================
     * 屏0 已证明: 总线电平/外部上拉/短路全部正常, 却 128 个地址 × 6 档速度
     * **零应答**。手册: SDA/SCL/NIRQ 全靠**外部上拉到 VDDEX**, 且"给 VDDEX 上电
     * 之后等 tBoot"才能访问 —— 说明芯片的串行接口是跟着 VDDEX 走的。
     * 而 v8~v16 的上电顺序是"**先**把 SDA/SCL 推挽驱动到 3.3V, **再**给 VDDEX 打
     * 高→低→高 的复位脉冲"。VDDEX=0 时用推挽压着两根线, 会经芯片的 ESD 二极管
     * 给它"偷供电", 很可能毁掉干净的上电复位 → 芯片根本不开 I2C 接口。
     * 本屏把 3 个开关全组合逐个试, 每个组合对 0x55 探"写/读"各一次:
     *   p = 1/0 : 是否发 VDDEX 复位脉冲
     *   e = 1/0 : 1=先驱动线再上 VDDEX(v8 老行为), 0=先上 VDDEX 稳定再碰线
     *   s = 0/1 : SDA/SCL 是否交换
     * 命中判据: 哪一行出现 R, 就是那一组组合对。
     * ==================================================================== */
    {
        uint8 p_set[8], e_set[8], s_set[8], k0[8], k1[8];
        p_set[0]=1u; e_set[0]=1u; s_set[0]=0u;
        p_set[1]=0u; e_set[1]=1u; s_set[1]=0u;
        p_set[2]=1u; e_set[2]=0u; s_set[2]=0u;
        p_set[3]=0u; e_set[3]=0u; s_set[3]=0u;
        p_set[4]=1u; e_set[4]=1u; s_set[4]=1u;
        p_set[5]=0u; e_set[5]=1u; s_set[5]=1u;
        p_set[6]=1u; e_set[6]=0u; s_set[6]=1u;
        p_set[7]=0u; e_set[7]=0u; s_set[7]=1u;

        for (i = 0u; i < 8u; i++) {
            NFC_PowerOff();
            NFC_DelayMs(50u);
            NFC_SetSwap(s_set[i]);
            NFC_SetPwrSeq(p_set[i], e_set[i]);
            NFC_PowerUp(50u);
            NFC_BusRecover();
            k0[i] = NFC_Probe(0x55u, 0u);
            k1[i] = NFC_Probe(0x55u, 1u);
        }
        NFC_PowerOff();
        NFC_SetPwrSeq(1u, 1u);      /* 还原默认 */
        NFC_SetSwap(g_at_swap);     /* ★ v18: 还原到 autotune 命中角色, 不是硬编码 0 */

        title("6 PWRSEQ", "power order x swap, probe 55");
        for (i = 0u; i < 8u; i++) {
            b_reset();
            b_dig(i);
            b_str(" p"); b_dig(p_set[i]);
            b_str(" e"); b_dig(e_set[i]);
            b_str(" s"); b_dig(s_set[i]);
            b_str("  "); b_k1(k0[i]); b_ch('/'); b_k1(k1[i]);
            dr(i);
        }
        row8(8u, "any R -> that combo OK !");
        uputs("--- s6 PWRSEQ done ---\r\n");
        flush();
        NFC_DelayMs(WAIT_MS);
    }

    uputs("--- loop ---\r\n");
    }                                 /* for(;;) */
}
#endif /* !V30_RFWATCH */

/* ===========================================================================
 * ★★★★ v30: RF 场实时观测模式 (只在 V30_RFWATCH 定义时编译)
 * ---------------------------------------------------------------------------
 * 假设(用户提出, 合理): MN63Y1213 是**双接口**芯片 —— RF 接口(线圈) + I2C 接口。
 *   RF 侧有整流器, 手机 NFC 场会经线圈给芯片取电, 可能因此把芯片供起来,
 *   于是 I2C 侧就能应答。本模式不休眠地循环探测, 结果实时打串口,
 *   用户把手机贴上线圈的瞬间就能看到变化。
 *
 * 每轮 (~0.2s):
 *   1. P2.3(VDDEX) 保持驱高(不来回开关, 不打扰 RF 侧可能建立的电位)
 *   2. NFC_BusRecover() 踢回空闲
 *   3. 探 0x55(真地址) 与 0x6B(必然不存在的对照地址)
 *   4. 打: 轮次 kind55 kind6B s5 两线电平 P2 NIRQ
 *   5. ★ 若 k55 != k6B ⇒ 总线"活了"(真器件在响应) —— 这是关键判据!
 * =========================================================================== */
#if defined(V30_RFWATCH) && (V30_RFWATCH != 0) && !(defined(V40_EPD) && (V40_EPD != 0))
/* ★★★★ v30e: 顺序实验 —— "RF 场先到 -> 再开 VDDEX" ★★★★
 *
 * ★★ 本轮来自用户的指正(核心思路):
 *   之前所有实验的**顺序反了** —— 不能先拉高 P2.3。
 *   芯片在 RF-only(场取电)模式下, 会用内部电路把 VDDEX 节点**牵制**住;
 *   此时 MCU 抢着推挽拉高, 两股力量顶牛, 节点被钉在 ~0.6V, 于是既供不上电、
 *   也把 I2C 侧拖死。正确顺序应该是:
 *
 *     RF 场出现 -> 芯片检测到 -> [芯片通知主机 NIRQ] -> **主机才拉 P2.3**
 *               -> VDDEX 正常升到 3.2V -> 芯片进 Serial 模式 -> I2C 可用
 *
 * ★ 但 v30b/c/d 全程实测数据说话: **NIRQ 从未拉低过 (nLo 恒等于 0)**。
 *   => 用户说的"通知主机"这一环, 在这块板 / 这种模式下**确实不存在**。
 *   => 所以本固件退一步, 改用 **RF 场在 P2.3 浮空节点上耦合出的电位抬升**
 *      本身当触发源(v30b/c/d 已反复证明: 手机贴上 -> P23 读数=1、
 *      line -> 0x03, 且能稳定维持几十秒)。
 *
 * ★ 本固件做两组对比, 一轮跑完就能定死结论:
 *     [A] 对照组   上电就硬拉 P2.3(不等 RF) -> 读回引脚真实电平 + 试 I2C
 *     [B] 实验组   浮空监测, **检测到 RF 场之后才**拉 P2.3 -> 读回 + 试 I2C
 *
 * ★★ 关键诊断手法(不需要万用表):
 *     CC2530 在**推挽输出高**的状态下读 P2 寄存器, 得到的是**引脚真实电平**:
 *       读到 1  -> 引脚确实 >= VIH(约 2.3V) -> VDDEX 真供上去了
 *       读到 0  -> 引脚被外部钉在 <2.3V     -> 存在牵制 / 钳位
 *     这一位就足以判定 VDDEX 到底有没有真正起来。
 *
 * 打印字段:
 *   [RFW] n / p23 / nirq / line / nHi / nLo / SEQ
 *   >>> SEQ#k <<<            第 k 轮序列
 *   P2.3 读回 = x            ★ 关键: 1=供上, 0=被钉低
 *   Decide9 k / s5           0 REAL 1 LATE 2 NONE 3 STUCK
 *   ReadMem[i] rm=           0=OK(并打印 blk) 1=NOACK
 */

/* P2.3 放开成三态输入 —— 用于浮空观测 RF 场耦合 */
static void v30e_p23_float(void)
{
    P2SEL &= (uint8)~0x02u;   /* P2SEL.1 = P2.3 功能位(0=GPIO) */
    P2INP |= 0x08u;           /* 三态: 不内拉, 只看外部 */
    P2DIR &= (uint8)~0x08u;   /* 输入 */
}

/* P2.3 推挽拉高 */
static void v30e_p23_drive_hi(void)
{
    P2SEL &= (uint8)~0x02u;
    P2DIR |= 0x08u;
    P2    |= 0x08u;
}

/* SDA/SCL 全放开(三态不内拉) —— 观测时不打扰芯片 */
static void v30e_bus_float(void)
{
    P0SEL &= (uint8)~0x03u;
    P0INP |= 0x03u;
    P0DIR &= (uint8)~0x03u;
    P2INP |= 0x20u;           /* PDUP0=1 -> Port0 三态 */
    P0IEN &= (uint8)~0x03u;
}

/* ---------- v31 分步诊断打印 ----------
 * 打印每一拍的第 9 位判定 + 实际读到的字节。
 *   0=REAL(真ACK)  1=LATE  2=NONE(没ACK)  3=STUCK(线被钉死)  F=未执行
 * 看到第一个不是 0 的位置, 就是断点。
 */
static void v30e_pr_ack(uint8 v)
{
    if (v == NFC_ACK_REAL)  { uputc('0'); uputs("=REAL  "); }
    else if (v == NFC_ACK_LATE)  { uputc('1'); uputs("=LATE  "); }
    else if (v == NFC_ACK_NONE)  { uputc('2'); uputs("=NONE  "); }
    else if (v == NFC_ACK_STUCK) { uputc('3'); uputs("=STUCK "); }
    else                         { uputc('F'); uputs("=?     "); }
}

static void v30e_pr_blk(const uint8 *b, uint8 n)
{
    uint8 j, allFF = 1u, all00 = 1u;
    for (j = 0u; j < n; j++) {
        uh8(b[j]); uputc(' ');
        if (b[j] != 0xFFu) { allFF = 0u; }
        if (b[j] != 0x00u) { all00 = 0u; }
    }
    uputs("  => ");
    if (allFF != 0u)      { uputs("全 FF (SDA 始终高, 芯片没拉过线/没吐数据)"); }
    else if (all00 != 0u) { uputs("全 00 (SDA 始终低, 芯片钉住总线或未释放)"); }
    else                  { uputs("*** 有变化数据 -> 芯片真的吐字节了! ***"); }
    uputs("\r\n");
}

static void v30e_diag_once(uint8 variant, uint8 idx)
{
    uint8 dbg[4], blk[8];

    if (variant == 0u) {
        uputs("    [V1 STOP分隔 ] #"); udig(idx); uputs("  ack: W=");
        (void)NFC_ReadMemDiag(0x55u, 0x00u, 8u, blk, dbg);
    } else {
        uputs("    [V2 重复START] #"); udig(idx); uputs("  ack: W=");
        (void)NFC_ReadMemDiagRs(0x55u, 0x00u, 8u, blk, dbg);
    }
    v30e_pr_ack(dbg[0]);
    uputs("B="); v30e_pr_ack(dbg[1]);
    uputs("R="); v30e_pr_ack(dbg[2]);
    uputs("\r\n      blk=");
    v30e_pr_blk(blk, 8u);
}

/* ★ v32: 原厂 powerup(bank1 0xF57C) 的 P2.3 供电波形:
 *   SETB P2.3 -> delay(5ms) -> CLR P2.3 -> delay(5ms) -> SETB P2.3
 *   即"先给一下、松开、再给"的复位脉冲。我们之前 v30e 只是一次拉高, 与原厂不符。
 *   若芯片需要一个 VDDEX 上升沿来完成上电复位/进 Serial 模式, 差别就在这里。 */
static void v30e_p23_pulse(void)
{
    v30e_p23_drive_hi();
    NFC_DelayMs(5u);
    P2 &= (uint8)~0x08u;      /* CLR P2.3 */
    NFC_DelayMs(5u);
    P2 |= 0x08u;              /* SETB P2.3 */
}

/* v32 第二字节扫描: 看是否**任何**第二字节都拿不到 ACK */
static void v32_sweep(void)
{
    static const uint8 cand[] = {
        0x00u, 0x01u, 0x02u, 0x04u, 0x08u, 0x10u,
        0x20u, 0x40u, 0x55u, 0x80u, 0xA0u, 0xFFu
    };
    uint8 out[sizeof(cand)];
    uint8 n = (uint8)(sizeof(cand) / sizeof(cand[0]));
    uint8 i, cnt = 0u;

    (void)NFC_ProbeSecondByte(0x55u, cand, n, out);
    uputs("    第二字节扫描 (START+AA+X+STOP 后 X 的 ACK):\r\n");
    for (i = 0u; i < n; i++) {
        uputc(' '); uputc(' '); uputc(' '); uputc(' ');
        uputs("0x"); uh8(cand[i]); uputs(" -> ");
        v30e_pr_ack(out[i]);
        uputc('\r'); uputc('\n');
        if (out[i] == NFC_ACK_REAL) { cnt++; }
    }
    uputs("    =>  ");
    if (cnt == 0u) {
        uputs("候选全 NACK: 芯片只做地址匹配, 拒绝任何第二字节 (未进可传输状态)\r\n");
    } else {
        uputs("有 "); udec(cnt); uputs(" 个候选拿到 ACK -> 第二字节是有语义的字段, 需改协议\r\n");
    }
}

/* ★ v32 [PRE] 无 VDDEX 基线: P2.3 明确**推挽拉低**(绝不给芯片送电),
 *   然后把同一套 I2C 问询原样做一遍。判读:
 *     结果与供电后**完全一样** -> 那些 ACK 与 P2.3 供电无关,
 *       说明芯片是靠总线/上拉经 ESD 二极管"幻电"在做地址匹配,
 *       而 VDDEX 这条路没真正把电送到芯片内核 -> 属于硬件供电路径问题。
 *     结果与供电后**不一样**(ACK 消失) -> 供电确实起作用了,
 *       芯片仍然是"地址认得、事务不肯走", 属于模式/握手问题。
 */
static void v32_seq_nopower(void)
{
    uint8 s5[5], k, i;

    uputs("\r\n  >>> [PRE] 基线: P2.3 推挽拉低(无 VDDEX) 时的 I2C 反应 <<<\r\n");
    P2SEL &= (uint8)~0x02u;
    P2DIR |= 0x08u;
    P2    &= (uint8)~0x08u;      /* 明确输出低, 输出锁存=0 */
    NFC_DelayMs(20u);

    NFC_GpioPrepPub();
    P2DIR |= 0x08u;
    P2    &= (uint8)~0x08u;      /* gpio_prep 之后再钉一次低, 防止半途被抬起 */

    uputs("    P2.3 读回 = "); udig((uint8)((P2 & 0x08u) ? 1u : 0u));
    uputs("  (必须为 0)\r\n");

    k = NFC_Decide9(0x55u, 0u, s5);
    uputs("    Decide9 k="); udig(k);
    uputs("  s5=");
    for (i = 0u; i < 5u; i++) { udig((uint8)(s5[i] & 0x01u)); }
    uputs("\r\n");

    v30e_diag_once(0u, 0u);
    NFC_DelayMs(30u);
    v30e_diag_once(1u, 0u);
    v32_sweep();
}

/* ★ v33 总线逐比特读回
 *   ACK[pre,hi,post] 三点: 1=读到高, 0=读到低。
 *   真 ACK 的形状应该是 pre=1? 不 —— ACK 要求"时钟高期间 SDA 为低":
 *     hi=0 才算有 ACK; 落以后 post=1 说明从机把 SDA 松开了(健康);
 *     post=0 说明线被钉死(STUCK), 此时的"ACK"不可信。 */
static void v33_pr_ackslot(uint8 v)
{
    uputc('[');
    uputc((v & 0x01u) ? '1' : '0'); uputc(',');
    uputc((v & 0x02u) ? '1' : '0'); uputc(',');
    uputc((v & 0x04u) ? '1' : '0');
    uputs("] pre="); uputc((v & 0x01u) ? '1' : '0');
    uputs(" hi=");   uputc((v & 0x02u) ? '1' : '0');
    uputs(" post="); uputc((v & 0x04u) ? '1' : '0');
    if ((v & 0x02u) == 0u) {
        uputs((v & 0x04u) != 0u ? "  -> 真ACK(已释放)" : "  -> 被钉死,ACK不可信");
    } else {
        uputs("  -> 无ACK");
    }
}

static void v33_trace_once(uint8 second)
{
    uint8 rep[8];
    uint8 addrbyte = (uint8)((0x55u << 1) & 0xFEu);

    (void)NFC_Tx8Trace(0x55u, second, rep);

    uputs("    ---- 总线逐比特读回: START + AA + "); uh8(second); uputs(" + STOP ----\r\n");

    uputs("      字节1 0x"); uh8(addrbyte);
    uputs("  SDA读回不符=0x"); uh8(rep[0]);
    uputs("  SCL被拉低=0x");  uh8(rep[1]);
    uputs("  总线实际=0x");   uh8(rep[2]);
    uputs("  ACK"); v33_pr_ackslot(rep[6]);
    uputs("\r\n");

    uputs("      字节2 0x"); uh8(second);
    uputs("  SDA读回不符=0x"); uh8(rep[3]);
    uputs("  SCL被拉低=0x");  uh8(rep[4]);
    uputs("  总线实际=0x");   uh8(rep[5]);
    uputs("  ACK"); v33_pr_ackslot(rep[7]);
    uputs("\r\n");

    uputs("      => ");
    if ((rep[0] | rep[1] | rep[3] | rep[4]) != 0u) {
        uputs("存在总线争用(SDA/SCL 读回与驱动不符) -> 硬件侧被钳位或时钟拉伸\r\n");
    } else if (rep[2] != addrbyte || rep[5] != second) {
        uputs("字节没原样送上总线 -> 读回值与要发的字节不一致\r\n");
    } else {
        uputs("两个字节都完好送上总线, 无争用。首字节有ACK而次字节无ACK = 芯片自身行为\r\n");
    }
}

/* 一轮完整序列: 拉 P2.3 -> 读回 -> gpio_prep -> 问 I2C -> 松开
 * mode: 0 = 单次拉高(v30e 原做法)  1 = 原厂复位脉冲(v32 新增) */
static void v30e_seq_once(uint16 seq, uint8 mode)
{
    uint8  s5[5], k, drv;
    uint8  i;

    uputs("\r\n  >>> SEQ#"); udec(seq); uputs(" 现在才拉起 P2.3 <<<\r\n");

    if (mode == 0u) {
        uputs("    [上电方式] 单次拉高\r\n");
        v30e_p23_drive_hi();
        NFC_DelayMs(10u);                       /* 让 VDDEX 电容充起来 */
    } else {
        uputs("    [上电方式] 原厂复位脉冲 (高5ms -> 低5ms -> 高)\r\n");
        v30e_p23_pulse();
        NFC_DelayMs(10u);                       /* 让 VDDEX 电容充起来 */
    }

    drv = (uint8)((P2 & 0x08u) ? 1u : 0u);  /* ★ 推挽输出下读回引脚真实电平 */
    uputs("    P2.3 读回 = "); udig(drv);
    if (drv != 0u) { uputs("  [OK] VDDEX 维持住高电平(>=VIH 2.3V)"); }
    else           { uputs("  [NG] 被牵制/钉低(<VIH), 顺序仍不对或有钳位"); }
    uputs("\r\n");

    NFC_GpioPrepPub();                      /* SDA/SCL 推挽; P2.3 保持输出高 */

    k = NFC_Decide9(0x55u, 0u, s5);
    uputs("    Decide9 k="); udig(k);
    uputs("  s5=");
    for (i = 0u; i < 5u; i++) { udig((uint8)(s5[i] & 0x01u)); }
    uputs("\r\n");

    /* v33: 逐比特读回 —— 这一步不需要 RF 场, 对照组就能出结论 */
    v33_trace_once(0x00u);
    v33_trace_once(0xFFu);

    /* v34: ACK 矩阵 —— 把"假 ACK"的触发条件钉死 */
    {
        uint8 mk[6];
        static const char *nm[6] = {
            "A:START+ 发00", "B:START+ 发FF", "C:START+ 不发字节",
            "D:无START+不发 ", "E:START+ 发AA", "F:AA之后的00  "
        };
        uint8 q;
        (void)NFC_AckMatrix(mk);
        uputs("    ---- ACK 矩阵 (pre/hi/post, 1=高) ----\r\n");
        for (q = 0u; q < 6u; q++) {
            uputs("      "); uputs((char *)nm[q]);
            uputs("  [");
            uputc((mk[q] & 0x01u) ? '1' : '0'); uputc(',');
            uputc((mk[q] & 0x02u) ? '1' : '0'); uputc(',');
            uputc((mk[q] & 0x04u) ? '1' : '0'); uputs("]  ");
            if ((mk[q] & 0x02u) == 0u) {
                uputs((mk[q] & 0x04u) != 0u ? "有ACK且已释放" : "被钉死");
            } else { uputs("无ACK"); }
            uputs("\r\n");
        }
    }

    /* v36: 静态漏电观测 —— 判断漏电是静态存在还是被时钟触发 */
    {
        uint16 lv[4]; uint8 ld[4];
        static const char *lb[4] = { "0ms ", "1ms ", "5ms ", "20ms" };
        uint8 q;
        (void)NFC_IdleLeakProbe(lv, ld);
        uputs("    ---- 静态漏电观测(上电后未翻任何时钟) ----\r\n");
        for (q = 0u; q < 4u; q++) {
            uputs("      "); uputs((char *)lb[q]);
            uputs(" 数字="); udig(ld[q]);
            uputs("  电压="); udec(lv[q]); uputs(" mV\r\n");
        }
        uputs("      => ");
        if (lv[3] > 2900u) {
            uputs("静态时总线能维持高电平 -> 漏电是 clock 触发的(芯片行为)\r\n");
        } else if (lv[3] < 1600u) {
            uputs("静态就抬不起来 -> 存在强静态漏电, 必须外接上拉\r\n");
        } else {
            uputs("静态也漂在中间电平 -> 必须外接上拉(内部20k扛不住)\r\n");
        }
    }

    /* v36: 同一时刻的数字读值 + 真实电压 + 寄存器快照 */
    {
        uint16 pv[2]; uint8 dg[2], rg[8];
        (void)NFC_PinProbe(pv, dg, rg);
        uputs("    ---- 引脚状态快照(数字 vs 真实电压) ----\r\n");
        uputs("      SCL低 : 数字="); udig(dg[0]);
        uputs("  电压="); udec(pv[0]); uputs(" mV\r\n");
        uputs("      SCL高 : 数字="); udig(dg[1]);
        uputs("  电压="); udec(pv[1]); uputs(" mV\r\n");
        uputs("      P0=0x");     uh8(rg[0]);
        uputs("  P0DIR=0x");      uh8(rg[1]);
        uputs("  P0SEL=0x");      uh8(rg[2]);
        uputs("  P0INP=0x");      uh8(rg[3]);
        uputs("\r\n      P2INP=0x"); uh8(rg[4]);
        uputs("  APCFG=0x");      uh8(rg[5]);
        uputs("  swap01=");       udig(rg[6]);
        uputs("\r\n      => ");
        if ((rg[1] & 0x01u) != 0u) {
            uputs("★ P0DIR.0=1: SDA 根本没被设成输入! 推挽输出下读到的是上一位的残留/锁存值\r\n");
        } else if ((rg[2] & 0x01u) != 0u) {
            uputs("★ P0SEL.0=1: SDA 被分配给外设, 读PORT不算引脚电平\r\n");
        } else if ((rg[5] & 0x01u) != 0u) {
            uputs("★ APCFG.0=1: SDA 在模拟模式, 数字读值无意义\r\n");
        } else {
            uputs("配置看着正常(输入+非外设+非模拟), 那就该再查读数时机\r\n");
        }
    }

    /* v35: ADC 实测 SDA 电压 —— 数字读值已自相矛盾, 看真实电平 */
    {
        uint16 mvv[5];
        static const char *lb[5] = {
            "推挽驱高(基准  )", "放开静置      ", "8空时钟后(假ACK现场)",
            "第9时钟高(采样点)", "第9时钟落下后  "
        };
        uint8 q;
        (void)NFC_AdcProbe(mvv);
        uputs("    ---- ADC 实测 SDA 电压 (mV) ----\r\n");
        for (q = 0u; q < 5u; q++) {
            uputs("      "); uputs((char *)lb[q]);
            uputs(" = "); udec(mvv[q]); uputs(" mV\r\n");
        }
    }

    /* v34: 时序扫描 —— 判定"假ACK"是不是 SCL 翻转本身造成的电气假象 */
    {
        uint8 ts[6];
        static const char *lb[5] = { "22us", "66us", "176us", "616us", "1716us" };
        uint8 q;
        (void)NFC_AckTimingSweep(ts);
        uputs("    ---- 时序扫描 ----\r\n");
        uputs("      G: START + 8个空时钟(不驱动SDA) + ACK槽 = [");
        uputc((ts[0] & 0x01u) ? '1' : '0'); uputc(',');
        uputc((ts[0] & 0x02u) ? '1' : '0'); uputc(',');
        uputc((ts[0] & 0x04u) ? '1' : '0'); uputs("]  ");
        if ((ts[0] & 0x02u) == 0u) {
            uputs("*** 空时钟也'有ACK' -> 与发的字节无关, 是纯电气假象 ***");
        } else { uputs("无ACK -> 需要真的输出字节才会出现, 非单纯空时钟效应"); }
        uputs("\r\n      释放SDA后延时采样: ");
        for (q = 0u; q < 5u; q++) {
            uputs((char *)lb[q]); uputs("=");
            uputc(ts[1u + q] ? '1' : '0'); uputs("  ");
        }
        uputs("\r\n");
    }

    /* v33: 地址全扫描 —— 确认 0x55 是不是唯一真有回应的地址 */
    uputs("    ---- 地址全扫描 0x00..0x7F ----\r\n");
    {
        static __xdata uint8 amap[128];
        uint8 a, cnt2, line = 0u;
        cnt2 = NFC_AddrScanAck(amap);
        uputs("      真ACK地址个数 = "); udec(cnt2); uputs("\r\n");
        for (a = 0u; a < 128u; a++) {
            if (amap[a] == 0u) { continue; }
            uputs("      0x"); uh8(a);
            uputs(amap[a] == 1u ? " 真ACK" : " 被钉死");
            if (++line >= 4u) { uputs("\r\n"); line = 0u; }
        }
        if (line != 0u) { uputs("\r\n"); }
        if (cnt2 > 3u) {
            uputs("      => 回应地址过多, 判据可能仍有系统误差, 需复查!\r\n");
        } else if (cnt2 == 0u) {
            uputs("      => 全无回应: 总线上没有活着的可寻址从设备\r\n");
        } else {
            uputs("      => 回应地址很少: 是真设备, 不是判据假象\r\n");
        }
    }

    /* v31: 连做 3 轮, 每轮跑两个变体 —— 给芯片进入 Serial 模式的时间窗 */
    for (i = 0u; i < 3u; i++) {
        v30e_diag_once(0u, i);      /* 变体 1: STOP 分隔(原厂协议)   */
        NFC_DelayMs(30u);
        v30e_diag_once(1u, i);      /* 变体 2: 重复 START(标准 EEPROM)*/
        NFC_DelayMs(60u);
    }

    /* v32: 扫描第二字节 —— 判断"协议字节值不对"还是"芯片根本不接受后续字节" */
    v32_sweep();

    /* 松开: 断电 + 总线/引脚全放开, 回到浮空观测 */
    P2 &= (uint8)~0x08u;
    NFC_DelayMs(2u);
    v30e_bus_float();
    v30e_p23_float();
}

int main(void)
{
    uint16 n    = 0u;
    uint16 seq  = 0u;
    uint16 nHi  = 0u;
    uint16 nLo  = 0u;
    uint16 sinceSeq = 0u;     /* 距上一轮 SEQ 过了几个采样周期 */

    clock_init();
    uart_init();
    uputs("\r\n===== v32 SEQ-TEST: 上电方式 A/B + 分步 ACK 诊断 =====\r\n");

    /* NIRQ(P1.0) 输入 + 上拉 */
    P1SEL &= (uint8)~0x01u;
    P1DIR &= (uint8)~0x01u;
    P1INP &= (uint8)~0x01u;
    P1IEN &= (uint8)~0x01u;
    P2INP &= (uint8)~0x40u;   /* PDUP1=0 -> Port1 上拉 */

    v30e_bus_float();
    v30e_p23_float();

    NFC_T1Init();
    NFC_TimebaseCal();

    /* ---------- [PRE] 无 VDDEX 基线 ---------- */
    uputs("\r\n---- [PRE] 基线: 完全不给 VDDEX(P2.3 推挽低) ----\r\n");
    v32_seq_nopower();
    NFC_DelayMs(300u);

    /* ---------- [A] 对照组: 不等 RF, 上电即硬拉(两种上电方式各一次) ---------- */
    uputs("\r\n---- [A] 对照组: 不等 RF 场, 上电直接拉 P2.3 ----\r\n");
    v30e_seq_once(0u, 0u);        /* [A0] 单次拉高 */
    NFC_DelayMs(300u);
    v30e_seq_once(0u, 1u);        /* [A1] 原厂复位脉冲 */
    NFC_DelayMs(300u);

    /* ---------- [B] 实验组: 检测到 RF 才拉 ---------- */
    uputs("\r\n---- [B] 实验组: 浮空监测, 检测到 RF 场后才拉 P2.3 ----\r\n");
    uputs("现在把手机贴上线圈并保持; 每检测到一次 RF 就跑一轮 SEQ\r\n\r\n");

    for (;;) {
        uint8 p23   = (uint8)((P2 & 0x08u) ? 1u : 0u);
        uint8 nirq  = NFC_NirqLevel();
        uint8 lines = (uint8)(P0 & 0x03u);

        if (p23  != 0u) { nHi++; }
        if (nirq == 0u) { nLo++; }

        uputs("[RFW] n=");   udec(n);
        uputs(" p23=");      udig(p23);
        uputs(" nirq=");     udig(nirq);
        uputs(" line=0x");   uh8(lines);
        uputs(" nHi=");      udec(nHi);
        uputs(" nLo=");      udec(nLo);
        uputs(" SEQ=");      udec(seq);
        uputs("\r\n");

        /* ★ RF 场存在 -> **周期性**跑 SEQ(每轮都是一次全新"先检测后供电")
         *   不能只靠上升沿: 手机一直贴着时 p23 恒为 1, 边沿只来一次就再也不触发了。 */
        if (p23 != 0u) {
            if (sinceSeq >= 12u) {        /* 约 12 个采样周期 ~ 1s */
                seq++;
                /* 奇偶轮换上电方式: 偶数轮=单次拉高, 奇数轮=原厂复位脉冲
                 *   -> 同一段贴机时间里把两种方式都覆盖到 */
                v30e_seq_once(seq, (uint8)((seq & 1u) != 0u ? 1u : 0u));
                sinceSeq = 0u;
            } else {
                sinceSeq++;
            }
        } else {
            sinceSeq = 12u;               /* 场一出现立刻开第一轮 */
        }

        n++;
        { volatile uint32 w = 0u; while (w < 15000UL) { w++; } }
    }
}
#endif  /* V30_RFWATCH */

/* ===========================================================================
 * ★ v40: 串口已拔 —— 整份 main() 移到 src/v40_epd.h, 结果全部画到板载 EPD。
 *   定义 V40_EPD=1 时生效, 此时 V30 的 main() 被上面的守卫排除。
 *   (放在文件末尾: 这里之后 b_reset/b_hex8/b_k1/lb 等文本工具已全部就位)
 * =========================================================================== */
#if defined(V40_EPD) && (V40_EPD != 0)
#include "v40_epd.h"
#endif

/* ===========================================================================
 * ★★★ v45: 唤醒实测固件 —— 干净唤醒序列 + swap/从地址/双协议 8 组合实测。
 *   串口 + EPD 双通道输出。定义 V45_WAKE=1 时生效。
 * ========================================================================= */
#if defined(V45_WAKE) && (V45_WAKE != 0)
#include "v45_wake.h"
#endif
