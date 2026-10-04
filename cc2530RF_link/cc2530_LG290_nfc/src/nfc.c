/**
 * @file    nfc.c
 * @brief   MN63Y1213 主机侧 I2C 驱动 (v4 —— Timer1 时间基准 + 屏上示波 + 真伪 ACK)
 *
 * 协议 (手册 5.2.6 时序图, 与 v3 相同):
 *   ┌ 写命令 ─────────────────────────────────────────────┐
 *   │ START | 从地址+R/W(L) | ACK | 命令字节... | ACK | STOP │
 *   └─────────────────────────────────────────────────────┘
 *            ↓ 芯片内部处理 → NIRQ 被拉低(t2)
 *   ┌ 读响应 ─────────────────────────────────────────────┐
 *   │ START | 从地址+R/W(H) | ACK | 状态... 数据... | NACK | STOP │
 *   └─────────────────────────────────────────────────────┘
 *
 * ★★★ v4 相对 v3 的硬改动 ★★★
 *   1) 位时序不再靠"软件延时循环估" —— 全部由 **Timer1(32MHz) 计数**决定,
 *      且用 32.768kHz 睡眠定时器交叉校准, 这样"位周期/真实 kHz"是可测可信的。
 *   2) SDA 强制 **开漏语义**: 低 = 驱动, 高 = 释放(靠上拉变高)。
 *      —— 绝不在 NFC 没电时主动推高 SDA/SCL(那是之前拖垮整板的元凶)。
 *   3) 第 9 位的真伪判定增加 **SCL 下降沿之后**那一点: 真 ACK 一定在那里已松开,
 *      读到低 = 线被钉死(STUCK), 任何"ACK"都作废(专治 128 全 ACK)。
 *   4) 新增 **NFC_ScopeTx**: 主机自己发 "START+字节+第9时钟+STOP", 同时按 4us
 *      节拍把(SCL,SDA)真实电平存进 g_scp[], 直接画到墨水屏上 —— "第 9 位为什么低"
 *      从此是一幅能看的图, 不再是靠一个采样点猜。
 *   5) 新增 **上升/下降时间实测**(Timer1 纳秒级): 直接回答"外部上拉到底在不在",
 *      这是"硬件没问题"必须过的唯一量化关卡。
 */
#include "nfc.h"

/* ------------------------------------------------------------ 配置 -------- */
static __idata uint8  swap01  = 0u;   /* 0: SDA=P0.0 SCL=P0.1 */
static __idata uint8  drv_pp  = 1u;   /* SCL 推挽(默认, 数据手册允许); SDA 恒开漏 */
static __idata uint8  ord_fac = 1u;   /* 1: 上电后打开 MCU 内部上拉做备份; 0: 纯靠外部上拉 */
static __idata uint8  in_pu_off = 0u; /* 1 = 关掉 P0.0/P0.1 内部上拉(三态), 只为测外部上拉 */

/* -------------------------------------------------- Timer1 时间基准 ------- */
static uint8  g_t1ok    = 0u;
static uint16 g_tickNs  = 31u;        /* 1 个 T1 tick = ? ns (默认按 32MHz ≈31.25) */
static uint8  g_calOk   = 0u;         /* 1 = 睡眠定时器可用, 数据是实测的 */
static uint16 g_calT1   = 0u;         /* 参考区间的 T1 tick 数 (原始读数) */
static uint16 g_calSt   = 0u;         /* 参考区间的 ST  tick 数 (原始读数) */
static uint16 g_us4     = 125u;       /* 4us   对应的 T1 tick 数 */
static uint16 g_us10    = 312u;       /* 10us  对应的 T1 tick 数 */
static uint16 g_us20    = 625u;       /* 20us  对应的 T1 tick 数 */
static uint16 g_bitTicks= 1249u;       /* 一个位周期 = dly_s+dly_l+dly_s 的 T1 tick 数 */

/* ------------------------------------------------------------ 诊断量 ------ */
uint8  g_ack_kind = NFC_ACK_NONE;
uint8  g_status   = 0xFFu;
uint8  g_nirq_ms  = 0xFFu;
uint8  g_ndef_at  = 0xFFu;
uint16 g_mem_len  = 0u;

/* ------------------------------------------------------------ 采样示波 ---- */
__xdata uint8 g_scp[SCP_N];
uint8  g_scp_ack = 0u;                /* p<<3 | e<<2 | l<<1 | a */
uint8  g_scp_post = 0x0Fu;            /* SCL 落之后 SDA 回到高用了几个采样点 */

/* ------------------------------------------------------------ FeRAM ------- */
__xdata uint8 g_mem[MN63Y_FERAM_SIZE];

/* ===========================================================================
 *                       Timer1 / 睡眠定时器 底层
 * =========================================================================== */
/* 读 T1 16 位计数器。CC2530: 先读 T1CNTL 会锁存 T1CNTH, 必须 L 后 H。 */
static uint16 read_t1(void)
{
    uint8 l = T1CNTL;
    uint8 h = T1CNTH;
    return (uint16)(((uint16)h << 8) | (uint16)l);
}

/* 读 32.768kHz 睡眠定时器(24 位, ST0 最低) */
static uint32 st24(void)
{
    uint32 v;
    uint8  a, b, c;
    a = ST0; b = ST1; c = ST2;
    v = (uint32)c;
    v = (v << 8) | (uint32)b;
    v = (v << 8) | (uint32)a;
    return v;
}

/* 等 T1 前进 t 个 tick (自动处理 16 位回绕) */
static void t1_wait_ticks(uint16 t)
{
    uint16 s = read_t1();
    uint16 e;
    do { e = read_t1(); } while ((uint16)(e - s) < t);
}

void NFC_T1Init(void)
{
    /* ★ v5: CC2530 复位后 CLKCONCMD.TICKSPD 默认不是 /1, 实测 T1 每 tick 是 61ns
     * (≈16.4MHz) 而不是 31.25ns(32MHz) —— 这就是屏上 "ns=061" 的来源。
     * 显式把 TICKSPD 设成 /1, 让 T1 拿到干净 32MHz, 分辨率才够量 40us 位周期
     * 和几十 ns 级的沿。位时序本身是用 tickNs 反推的, 所以设不设都不跑偏,
     * 但设了以后 "bit / kHz" 才是真的。 */
    CLKCONCMD = (uint8)(CLKCONCMD & (uint8)~0x38u);   /* TICKSPD = 000 -> 32MHz */
    T1CTL = 0x01u;                    /* MODE=free(01), DIV=/1(00) */
    {
        uint16 a = read_t1();
        t1_wait_ticks(50u);
        {
            uint16 b = read_t1();
            g_t1ok = (uint8)(((uint16)(b - a) != 0u) ? 1u : 0u);
        }
    }
}

uint8 NFC_T1Ok(void) { return g_t1ok; }

void NFC_TimebaseCal(void)
{
    uint16 tn0, tn1;
    uint32 st0, st1, dt_st;
    uint16 dt_t1;
    uint32 s0;

    /* 测量一段 ~1ms 的区间, 同时用 T1 和 ST 量(两把尺子互校)。
     * 1ms < T1 回绕周期(2.048ms), 不会跨边界。 */
    tn0 = read_t1(); st0 = st24();
    s0  = st24();
    while ((st24() - s0) < 33u) { /* ~1ms (33 * 30.5176us ≈ 1.007ms) */ }
    tn1 = read_t1(); st1 = st24();

    dt_st = (st1 > st0) ? (st1 - st0) : 0u;
    dt_t1 = (uint16)(tn1 - tn0);

    g_calT1 = dt_t1;
    g_calSt = (uint16)dt_st;

    /* ST 一拍 = 30517.578 ns。tickNs = dt_st * 30517.578 / dt_t1 */
    if (dt_t1 != 0u && dt_st != 0u) {
        uint32 ns_loop = (dt_st * 305176UL) / 10UL;   /* 区间总长(ns) */
        uint32 tns     = ns_loop / (uint32)dt_t1;      /* 每 T1 tick 的 ns */
        if (tns > 0u && tns < 1000u) {
            g_tickNs = (uint16)tns;
            g_calOk  = 1u;
        } else {
            g_tickNs = 31u; g_calOk = 0u;
        }
    } else {
        g_tickNs = 31u; g_calOk = 0u;
    }

    /* 由 tickNs 反推各档位等待的 T1 tick 数 */
    g_us4  = (uint16)(4000u  / g_tickNs);
    g_us10 = (uint16)(10000u / g_tickNs);
    g_us20 = (uint16)(20000u / g_tickNs);
    g_bitTicks = (uint16)(g_us10 + g_us20 + g_us10);
}

uint16 NFC_GetTickNs(void)    { return g_tickNs; }
uint16 NFC_GetBitTicks(void)  { return g_bitTicks; }
uint16 NFC_GetCalT1(void)     { return g_calT1; }
uint16 NFC_GetCalSt(void)     { return g_calSt; }
uint16 NFC_GetBitNs(void)
{
    return (uint16)((uint32)g_bitTicks * (uint32)g_tickNs);   /* ns */
}
uint16 NFC_GetBitUs100(void)
{
    /* ★ v5 修正: ns -> 0.01us 是 **除以 10**, 不是除以 100。
     *   v4 除以 100, 于是真实的 40us(25kHz) 被显示成 "0396 x0.01uS" + "kHz=251",
     *   白白让人以为跑到了 250kHz、怀疑"太快芯片不认"。实际上 25kHz 一直落在
     *   手册允许的 20~100kHz 里 —— 所以"时钟太快"这条假设可以划掉了。 */
    return (uint16)(NFC_GetBitNs() / 10UL);
}
uint8  NFC_GetCalOk(void)     { return g_calOk; }

/* 微秒级延时(由 T1 决定) */
static void dly_s(void) { t1_wait_ticks(g_us10); }   /* ~10us = 每位的两段短相 */
static void dly_l(void) { t1_wait_ticks(g_us20); }   /* ~20us = 每位的长相(SCL 高) */

void NFC_DelayMs(uint16 ms)
{
    while (ms != 0u) {
        uint32 s = st24();
        while ((st24() - s) < 33u) { /* ~1ms */ }
        ms--;
    }
}

/* ===========================================================================
 *                       位层 (SDA 开漏 / SCL 推挽)
 * =========================================================================== */
static void sda_low(void)
{
    if (swap01 == 0u) { P0_0 = 0; P0DIR |= 0x01u; }
    else              { P0_1 = 0; P0DIR |= 0x02u; }
}
static void scl_low(void)
{
    if (swap01 == 0u) { P0_1 = 0; P0DIR |= 0x02u; }
    else              { P0_0 = 0; P0DIR |= 0x01u; }
}
/* SDA 开漏"高" = 释放(转输入, 靠上拉变高) —— 绝不主动推高 */
static void sda_rel(void)
{
    if (swap01 == 0u) { P0_0 = 1; P0DIR &= (uint8)~0x01u; }
    else              { P0_1 = 1; P0DIR &= (uint8)~0x02u; }
}
/* SCL 高: 默认推挽(drv_pp=1); drv_pp=0 时改开漏释放(高靠上拉) */
static void scl_hi(void)
{
    if (drv_pp) {
        if (swap01 == 0u) { P0_1 = 1; P0DIR |= 0x02u; }
        else              { P0_0 = 1; P0DIR |= 0x01u; }
    } else {
        if (swap01 == 0u) { P0_1 = 1; P0DIR &= (uint8)~0x02u; }
        else              { P0_0 = 1; P0DIR &= (uint8)~0x01u; }
    }
}
/* SCL 释放(转输入) —— 仅在"看电平"时用, 正常时序里 SCL 始终由主机驱动 */
static void scl_rel(void)
{
    if (swap01 == 0u) { P0_1 = 1; P0DIR &= (uint8)~0x02u; }
    else              { P0_0 = 1; P0DIR &= (uint8)~0x01u; }
}

static uint8 sda_rd(void)
{
    uint8 v = P0;
    return (uint8)((swap01 == 0u) ? (v & 0x01u) : ((v >> 1) & 0x01u));
}
static uint8 scl_rd(void)
{
    uint8 v = P0;
    return (uint8)((swap01 == 0u) ? ((v >> 1) & 0x01u) : (v & 0x01u));
}

/* ------------------------------------------------------------ 电源 -------- */
static void gpio_prep(void)
{
    P0SEL &= (uint8)~0x03u;   /* P0.0/P0.1 -> GPIO */
    P1SEL &= (uint8)~0x01u;   /* P1.0(NIRQ) -> GPIO */
    if (in_pu_off != 0u) { P0INP |= 0x03u; }          /* 三态(内部上拉关掉) */
    else                 { P0INP &= (uint8)~0x03u; }  /* 使能内部上拉(配合 PDUP0=0) */
    P2INP &= (uint8)~0x20u;   /* PDUP0 = 0 -> Port0 上拉 */
    P0IEN &= (uint8)~0x03u;   /* 关 P0 引脚中断: bit-bang 会疯狂触发 */
    P1DIR &= (uint8)~0x01u;
    P1INP &= (uint8)~0x01u;
    P1IEN &= (uint8)~0x01u;
    P2INP &= (uint8)~0x40u;   /* PDUP1 = 0 -> Port1 上拉 (PDUP1 是 bit6!) */
    P2SEL &= (uint8)~0x08u;
    P2DIR |= 0x08u;           /* P2.3 = VDDEX, 输出 */
    /* 安全默认: 两根线都释放成输入(内部上拉拉高), 绝不主动推高 */
    P0_0 = 1; P0_1 = 1; P0DIR &= (uint8)~0x03u;
}

void NFC_PowerUp(uint8 bootMs)
{
    /* ★★ v5: 上电顺序按手册 2.3 + Figure 2-3 重排(上拉必须挂在 VDDEX 上) ★★
     *   1) 先让 SDA/SCL **三态** —— 既不是 MCU 内部上拉到 3.3V, 也不是推高。
     *      v4 是"内部上拉着让线先高, 再抬 VDDEX", 于是 VDDEX 还没来时, 芯片的
     *      SDA/SCL 就已经被 MCU 通过 20k 拉到 3.3V, 电流经芯片 ESD 二极管灌进
     *      芯片的 VDDEX 节点 —— 芯片"半个上电", 等 VDDEX 真抬起来时上电复位
     *      已经不干净了, 触点接口起不来 -> 永远不 ACK、NIRQ 永远不低。
     *   2) 抬 VDDEX(此时线三态, 靠外部上拉跟随 VDDEX)。
     *   3) 等 tBoot。
     *   4) 这时才打开内部上拉/放开两线。 */
    P2SEL &= (uint8)~0x08u;
    P2DIR |= 0x08u;                       /* P2.3 = 输出 */
    P2_3   = 0;
    P0SEL &= (uint8)~0x03u;
    P0_0 = 1; P0_1 = 1;
    P0DIR &= (uint8)~0x03u;               /* 输入 */
    P0INP |= 0x03u;                       /* 三态 */
    dly_l();

    P2_3 = 1;                             /* 抬 VDDEX */
    NFC_DelayMs(bootMs);                  /* tBoot */

    in_pu_off = (ord_fac != 0u) ? 0u : 1u;
    gpio_prep();                          /* VDDEX 已稳, 现在才碰两条线 */
    sda_rel(); scl_rel();
    dly_l();
}

void NFC_PowerOff(void)
{
    P2_3 = 0;
    NFC_DelayMs(2u);
    P0SEL &= (uint8)~0x03u;
    P0DIR &= (uint8)~0x03u;
    P0INP |= 0x03u;                       /* ★ 三态: 不给断电的芯片经 ESD 二极管灌电 */
    P2INP &= (uint8)~0x20u;
    P0IEN &= (uint8)~0x03u;
    P1IEN &= (uint8)~0x01u;
    P0_0 = 1;
    P0_1 = 1;
}

uint8 NFC_NirqLevel(void)
{
    P1SEL &= (uint8)~0x01u;
    P1DIR &= (uint8)~0x01u;
    dly_l();
    return (uint8)((P1 & 0x01u) ? 1u : 0u);
}

uint8 NFC_VddAlive(void)
{
    uint8 v;
    /* 假设调用前 VDDEX 已开(P2.3 输出高)。短暂转输入读引脚, 再恢复。 */
    P2DIR &= (uint8)~0x08u;
    t1_wait_ticks(g_us4);
    v = (uint8)((P2 & 0x08u) ? 1u : 0u);
    P2DIR |= 0x08u;
    P2_3 = 1;
    return v;
}

uint8 NFC_BusLevel(void)
{
    gpio_prep();
    sda_rel(); scl_rel();
    dly_l();
    return (uint8)(P0 & 0x03u);
}

void NFC_BusRecover(void)
{
    uint8 i;
    sda_rel(); scl_hi();
    dly_l();
    for (i = 0; i < 20u; i++) {
        scl_low(); dly_l();
        scl_hi();  dly_l();
    }
    /* 补一个 STOP: SCL 高时把 SDA 由低变高 */
    sda_low(); dly_s(); scl_hi(); dly_s(); sda_rel(); dly_l();
}

/* ------------------------------------------------------------ 原语 -------- */
static void i2c_start(void)
{
    sda_rel(); scl_hi();      /* 空闲: SDA 高, SCL 高 */
    dly_l();
    sda_low();                /* SCL 高, SDA 1->0 = START */
    dly_s();
    scl_low();
    dly_s();
}

static void i2c_stop(void)
{
    sda_low(); scl_low();     /* SDA 低, SCL 低 */
    dly_s();
    scl_hi();                 /* SCL 1->0... 抬 SCL */
    dly_s();
    sda_rel();                /* SCL 高, SDA 0->1 = STOP */
    dly_l();
}

static void i2c_tx8(uint8 b)
{
    uint8 i;
    for (i = 0; i < 8u; i++) {
        if (b & 0x80u) { sda_rel(); } else { sda_low(); }
        dly_s();
        scl_hi();
        dly_l();
        scl_low();
        dly_s();
        b = (uint8)(b << 1);
    }
}

static uint8 i2c_rx8(void)
{
    uint8 i, b = 0;
    for (i = 0; i < 8u; i++) {
        sda_rel();            /* 释放 SDA 让从机驱动 */
        scl_low();
        dly_s();
        scl_hi();
        dly_l();
        b = (uint8)(b << 1);
        if (sda_rd()) { b |= 0x01u; }
        scl_low();
        dly_s();
    }
    return b;
}

static void i2c_putack(uint8 nack)
{
    sda_low();                /* 默认驱动低 = ACK */
    if (nack) { sda_rel(); }  /* NACK = 释放(高) */
    scl_low();
    dly_s();
    scl_hi();
    dly_l();
    scl_low();
    sda_low();
    dly_s();
}

/*
 * ★★ v4 第 9 位真伪判定(新增"SCL 下降沿之后"那一点) ★★
 *   sda_rel() 后 SCL 低: 从机应在此时把 SDA 拉低(ACK 建立)
 *   SCL 高: 早(e)/晚(l)采样
 *   SCL 落: 从机**必须松开** SDA
 *   post-fall 采样 = 0 -> STUCK(线被钉死, 任何 ACK 作废)
 */
uint8 g_probe_pre = 1u;      /* 第 9 时钟抬升前 SDA 是否已自由 (1 = 自由) */

/*
 * ★★ 第 9 位真伪判定 ★★
 *   sda_rel() 后 SCL 低: 真从机会在这个低电平期就把 SDA 拉低(ACK 建立)
 *   SCL 高: 早(e)/晚(l)采样
 *   SCL 落: 从机**必须松开** SDA
 *   下降沿之后再采 = 0 -> STUCK(线被钉死, 任何 ACK 作废) —— 专治"128 全 ACK"
 *
 * v5 改动: 抬 SCL 前把低电平期从 10us 拉到 20us。因为上一字节最后一位如果是 0,
 *   主机会**驱动低**; 放开后要靠上拉拉回高。抖动越小, "低"就越是"别人拉的"。
 */
static uint8 clock9_kind(void)
{
    uint8 pre, early, late, post;
    sda_rel();
    scl_low();
    dly_l();                  /* ★ 20us: 确保上一位的"驱动低"已彻底放开 */
    pre = sda_rd();           /* 第 9 时钟抬升前, 线自由吗 */
    scl_hi();
    early = sda_rd();
    dly_l();
    late  = sda_rd();
    scl_low();
    dly_l();                  /* ★ 20us 之后再采 */
    post = sda_rd();          /* SCL 已落, 健康从机应已释放 -> 高 */
    g_probe_pre = pre;
    if (post == 0u) { return NFC_ACK_STUCK; }
    if (early == 0u) {
        if (late == 0u) { return NFC_ACK_REAL; }
        return NFC_ACK_LATE;
    }
    return NFC_ACK_NONE;
}

static uint8 tx_byte_ack(uint8 b)
{
    i2c_tx8(b);
    return clock9_kind();
}

/* ------------------------------------------------------------ 配置 API ---- */
void NFC_SetSwap(uint8 s)   { swap01  = (s != 0u) ? 1u : 0u; }
void NFC_SetDrive(uint8 pp) { drv_pp  = (pp != 0u) ? 1u : 0u; }  /* 仅影响 SCL(本版恒推挽) */
void NFC_SetOrder(uint8 o)  { ord_fac = (o != 0u) ? 1u : 0u; }

/* ------------------------------------------------------------ 引脚真值 ---- */
uint8 NFC_PinDrive(uint8 pin, uint8 level)
{
    gpio_prep();
    if (pin == 0u) {
        if (level != 0u) { P0_0 = 1; } else { P0_0 = 0; }
        P0DIR |= 0x01u;
    } else {
        if (level != 0u) { P0_1 = 1; } else { P0_1 = 0; }
        P0DIR |= 0x02u;
    }
    dly_l();
    return (uint8)((P0 >> (pin ? 1u : 0u)) & 0x01u);
}

uint8 NFC_PinFloat(uint8 pin)
{
    gpio_prep();
    if (pin == 0u) { P0_0 = 1; P0DIR &= (uint8)~0x01u; }
    else           { P0_1 = 1; P0DIR &= (uint8)~0x02u; }
    dly_l();
    return (uint8)((P0 >> (pin ? 1u : 0u)) & 0x01u);
}

uint8 NFC_DriveAndReadOther(uint8 drivePin, uint8 level)
{
    uint8 other = (uint8)(drivePin ^ 1u);
    gpio_prep();
    if (other == 0u) { P0_0 = 1; P0DIR &= (uint8)~0x01u; }
    else             { P0_1 = 1; P0DIR &= (uint8)~0x02u; }
    dly_s();
    if (drivePin == 0u) {
        if (level != 0u) { P0_0 = 1; } else { P0_0 = 0; }
        P0DIR |= 0x01u;
    } else {
        if (level != 0u) { P0_1 = 1; } else { P0_1 = 0; }
        P0DIR |= 0x02u;
    }
    dly_l();
    return (uint8)((P0 >> (other ? 1u : 0u)) & 0x01u);
}

/* ------------------------------------------------------------ 上升/下降 --- */
/* 返回 0.1us 单位; 0 = 一直没变到目标电平(上拉缺失/线被钉死) */
uint16 NFC_SdaRiseUs10(void)
{
    uint16 t0, t1, i; uint8 hi = 0;
    gpio_prep();
    sda_low(); dly_l();                 /* 先强拉低 */
    t0 = read_t1();
    sda_rel();                          /* 释放, 应靠上拉回到高 */
    for (i = 0; i < 400u; i++) { if (sda_rd()) { hi = 1; break; } }
    t1 = read_t1();
    if (!hi) { return 0u; }             /* 一直低 = 上拉缺失/被钉 */
    return (uint16)(((uint32)(t1 - t0) * (uint32)g_tickNs) / 100UL);
}

uint16 NFC_SclRiseUs10(void)
{
    uint16 t0, t1, i; uint8 hi = 0;
    gpio_prep();
    scl_low(); dly_l();                 /* 先拉低 */
    t0 = read_t1();
    scl_hi();                           /* 推挽抬升 */
    for (i = 0; i < 400u; i++) { if (scl_rd()) { hi = 1; break; } }
    t1 = read_t1();
    if (!hi) { return 0u; }
    return (uint16)(((uint32)(t1 - t0) * (uint32)g_tickNs) / 100UL);
}

uint16 NFC_SdaFallUs10(void)
{
    uint16 t0, t1, i; uint8 lo = 0;
    gpio_prep();
    sda_rel(); dly_l();                 /* 释放(应高) */
    t0 = read_t1();
    sda_low();                          /* 驱动拉低 */
    for (i = 0; i < 400u; i++) { if (!sda_rd()) { lo = 1; break; } }
    t1 = read_t1();
    if (!lo) { return 0u; }
    return (uint16)(((uint32)(t1 - t0) * (uint32)g_tickNs) / 100UL);
}

/* ===========================================================================
 *                       采样示波 (v4 判决性)
 * =========================================================================== */
/*
 * 产生 "START + b(8位) + 第9时钟 + STOP" 的完整波形, 同时每 4us 采一次真实
 * (SCL,SDA) 电平存进 g_scp[]:
 *   bit0 = SDA 引脚电平(看的是线的真实状态)   bit1 = SCL 引脚电平
 * 采样点下标(固定):
 *   0..3   : 空闲 + START
 *   4..83  : 8 个数据位 (每位 10 点 = 40us)
 *   84..93 : 第 9 个时钟 (第 86 点上升, 第 91 点下降)
 *   94..125: STOP + 尾部(看从机是否继续钉住 SDA)
 */
static void scope_set(uint8 sdaDrvLow, uint8 sclHi, uint8 *idx)
{
    if (sdaDrvLow) { sda_low(); } else { sda_rel(); }
    if (sclHi)     { scl_hi();  } else { scl_low(); }
    t1_wait_ticks(g_us4);                 /* 保持 4us */
    if (*idx < SCP_N) {
        uint8 s = sda_rd();
        uint8 c = scl_rd();
        g_scp[*idx] = (uint8)((s & 0x01u) | ((c & 0x01u) << 1));
    }
    (*idx)++;
}

void NFC_ScopeTx(uint8 b)
{
    uint8 i, bit, idx = 0u;

    /* 清空缓冲 */
    for (i = 0; i < SCP_N; i++) { g_scp[i] = 0u; }

    /* 0..3: 空闲 + START */
    scope_set(0, 1, &idx);   /* 0 idle SDA高 SCL高 */
    scope_set(0, 1, &idx);   /* 1 idle */
    scope_set(1, 1, &idx);   /* 2 START: SDA 落(此时 SCL 高) */
    scope_set(1, 1, &idx);   /* 3 START 保持 */

    /* 4..83: 8 个数据位, 每位 10 点 */
    for (bit = 0; bit < 8u; bit++) {
        uint8 v = (uint8)((b & 0x80u) ? 1u : 0u);
        uint8 j;
        for (j = 0; j < 10u; j++) {
            uint8 sclHi = (j >= 4u) ? 1u : 0u;   /* 后 5 点 SCL 高 */
            scope_set(v ? 0u : 1u, sclHi, &idx);  /* v=1 -> 释放(高), v=0 -> 拉低 */
        }
        b = (uint8)(b << 1);
    }

    /* 84..93: 第 9 个时钟 (SCL 在 idx 86..90 高, 91 落) */
    for (i = 0; i < 10u; i++) {
        uint8 sclHi = ((i >= 2u) && (i <= 6u)) ? 1u : 0u;
        scope_set(0, sclHi, &idx);     /* SDA 全程释放, 等从机拉 */
    }

    /* 94..125: STOP + 尾部 (idx98 起 SDA 释放=STOP; 之后看是否仍被钉低) */
    for (i = 0; i < 32u; i++) {
        uint8 sclHi  = (i >= 2u) ? 1u : 0u;       /* idx96 起 SCL 高 */
        uint8 sdaLow = (i < 4u)  ? 1u : 0u;        /* idx94..97 SDA 低, 98 起释放 */
        scope_set(sdaLow, sclHi, &idx);
    }

    /* 派生量 */
    {
        uint8 p = (g_scp[85] & 0x01u);
        uint8 e = (g_scp[87] & 0x01u);
        uint8 l = (g_scp[90] & 0x01u);
        uint8 a = (g_scp[93] & 0x01u);
        uint8 k;
        g_scp_ack = (uint8)((p << 3) | (e << 2) | (l << 1) | a);
        g_scp_post = 0x0Fu;
        for (k = 91u; k < SCP_N; k++) {
            if (g_scp[k] & 0x01u) { g_scp_post = (uint8)(k - 91u); break; }
        }
    }
}

uint8 NFC_ScpKind(void)
{
    uint8 a = (g_scp[93] & 0x01u);
    uint8 e = (g_scp[87] & 0x01u);
    uint8 l = (g_scp[90] & 0x01u);
    if (a == 0u) { return NFC_ACK_STUCK; }
    if (e == 0u) {
        if (l == 0u) { return NFC_ACK_REAL; }
        return NFC_ACK_LATE;
    }
    return NFC_ACK_NONE;
}

/* ------------------------------------------------------------ ACK 探测 ---- */
uint8 NFC_AckProbe(uint8 addr7, uint8 rd, uint8 *status)
{
    uint8 kind;
    if (status != 0) { *status = 0xFFu; }

    i2c_start();

    if (addr7 == 0xFFu) {
        /* 对照实验: 一个地址字节都不发, 从机绝不可能应答 */
        kind = clock9_kind();
        i2c_stop();
        dly_l();
        g_ack_kind = kind;   /* STUCK/NONE/REAL/LATE 直接反映线的真实状态 */
        return kind;
    }

    kind = tx_byte_ack((uint8)(((uint8)(addr7 << 1)) | ((rd != 0u) ? 1u : 0u)));

    if (rd != 0u && kind == NFC_ACK_REAL && status != 0) {
        *status = i2c_rx8();
        i2c_putack(1u);
        i2c_stop();
        dly_l();
        g_status = *status;
    } else {
        i2c_stop();
        dly_l();
    }

    g_ack_kind = kind;
    return kind;
}

uint8 NFC_StatusCmd(uint8 slv, uint8 *status)
{
    return NFC_AckProbe(slv, 1u, status);
}

uint8 NFC_ScanAll(uint8 *first, uint8 *late, uint8 *none, uint8 *stuck)
{
    uint8 a, n = 0u, l = 0u, no = 0u, sk = 0u, k;
    *first = 0xFFu;
    for (a = 0; a < 128u; a++) {
        k = NFC_AckProbe(a, 1u, 0);
        if      (k == NFC_ACK_REAL)  { if (n == 0u) { *first = a; } n++; }
        else if (k == NFC_ACK_LATE)  { l++; }
        else if (k == NFC_ACK_STUCK) { sk++; }
        else                         { no++; }
    }
    *late  = l;
    *none  = no;
    *stuck = sk;
    return n;
}

/* ------------------------------------------------------------ 命令帧 ------ */
static uint8 cmd_send(uint8 slv, const uint8 *buf, uint8 n)
{
    uint8 i;
    i2c_start();
    if (tx_byte_ack((uint8)((uint8)(slv << 1) & 0xFEu)) != NFC_ACK_REAL) {
        i2c_stop(); dly_l(); return 0u;
    }
    for (i = 0; i < n; i++) {
        if (tx_byte_ack(buf[i]) != NFC_ACK_REAL) { i2c_stop(); dly_l(); return 0u; }
    }
    i2c_stop(); dly_l();
    return 1u;
}

static uint8 resp_read(uint8 slv, uint8 *st, uint8 *dst, uint8 n)
{
    uint8 i;
    i2c_start();
    if (tx_byte_ack((uint8)(((uint8)(slv << 1)) | 1u)) != NFC_ACK_REAL) {
        i2c_stop(); dly_l(); return 0u;
    }
    *st = i2c_rx8();
    i2c_putack((uint8)((n == 0u) ? 1u : 0u));
    for (i = 0; i < n; i++) {
        dst[i] = i2c_rx8();
        i2c_putack((uint8)(((uint8)(i + 1u) >= n) ? 1u : 0u));
    }
    i2c_stop(); dly_l();
    return 1u;
}

uint8 NFC_WaitNirqLow(uint16 ms)
{
    uint8  w = 0u;
    uint16 i;
    if (ms > 250u) { ms = 250u; }
    for (i = 0; i < ms; i++) {
        if (NFC_NirqLevel() == 0u) { g_nirq_ms = w; return w; }
        NFC_DelayMs(1u);
        w++;
    }
    g_nirq_ms = 0xFFu;
    return 0xFFu;
}

uint8 NFC_ReadMem(uint8 slv, uint16 addr, uint8 len, uint8 *dst, uint8 *status)
{
    uint8 cmd[4];
    uint8 st = 0xFFu;
    if (len == 0u || len > NFC_READ_CHUNK || dst == 0) { return NFC_ERR_PARAM; }
    cmd[0] = MN63Y_CMD_READ;
    cmd[1] = (uint8)(addr >> 8);
    cmd[2] = (uint8)(addr & 0xFFu);
    cmd[3] = len;
    if (cmd_send(slv, cmd, 4u) == 0u) { return NFC_ERR_NOACK; }
    (void)NFC_WaitNirqLow(200u);
    if (resp_read(slv, &st, dst, len) == 0u) { return NFC_ERR_NOACK; }
    if (status != 0) { *status = st; }
    g_status = st;
    if ((uint8)(st & 0x0Fu) != MN63Y_ST_OK) { return NFC_ERR_STATUS; }
    return NFC_OK;
}

uint8 NFC_WriteMem(uint8 slv, uint16 addr, const uint8 *src, uint8 len, uint8 *status)
{
    static __xdata uint8 cmd[NFC_WRITE_CHUNK + 4u];
    uint8 st = 0xFFu, i;
    if (len == 0u || len > NFC_WRITE_CHUNK || src == 0) { return NFC_ERR_PARAM; }
    cmd[0] = MN63Y_CMD_WRITE;
    cmd[1] = (uint8)(addr >> 8);
    cmd[2] = (uint8)(addr & 0xFFu);
    cmd[3] = len;
    for (i = 0; i < len; i++) { cmd[4u + i] = src[i]; }
    if (cmd_send(slv, cmd, (uint8)(4u + len)) == 0u) { return NFC_ERR_NOACK; }
    (void)NFC_WaitNirqLow(200u);
    if (resp_read(slv, &st, 0, 0u) == 0u) { return NFC_ERR_NOACK; }
    if (status != 0) { *status = st; }
    g_status = st;
    if ((uint8)(st & 0x0Fu) != MN63Y_ST_OK) { return NFC_ERR_STATUS; }
    return NFC_OK;
}

uint8 NFC_RReg(uint8 slv, uint8 *status, uint8 *data)
{
    uint8 cmd[1];
    uint8 st = 0xFFu, d = 0xFFu;
    cmd[0] = MN63Y_CMD_RREG;
    if (cmd_send(slv, cmd, 1u) == 0u) { return NFC_ERR_NOACK; }
    (void)NFC_WaitNirqLow(200u);
    if (resp_read(slv, &st, &d, 1u) == 0u) { return NFC_ERR_NOACK; }
    if (status != 0) { *status = st; }
    if (data   != 0) { *data   = d;  }
    g_status = st;
    if ((uint8)(st & 0x0Fu) != MN63Y_ST_OK) { return NFC_ERR_STATUS; }
    return NFC_OK;
}

uint8 NFC_WReg(uint8 slv, uint8 data, uint8 *status)
{
    uint8 cmd[2];
    uint8 st = 0xFFu;
    cmd[0] = MN63Y_CMD_WREG;
    cmd[1] = data;
    if (cmd_send(slv, cmd, 2u) == 0u) { return NFC_ERR_NOACK; }
    (void)NFC_WaitNirqLow(200u);
    if (resp_read(slv, &st, 0, 0u) == 0u) { return NFC_ERR_NOACK; }
    if (status != 0) { *status = st; }
    g_status = st;
    if ((uint8)(st & 0x0Fu) != MN63Y_ST_OK) { return NFC_ERR_STATUS; }
    return NFC_OK;
}

/* --------------------------------------------------------- NDEF 解析 ------- */
static uint8 ndef_find_text(const uint8 *p, uint16 n, char *out, uint16 max)
{
    uint16 i;
    for (i = 0; (uint16)(i + 5u) < n; i++) {
        uint8 pl, status, langlen, src;
        uint16 body, k, w;
        if ((p[i] & 0x10u) != 0u) continue;        /* SR 必须为 1 */
        if ((p[i] & 0x1Fu) != 0x01u) continue;     /* TNF = NFC well-known */
        if (p[i + 1u] != 0x01u) continue;          /* TYPE LENGTH = 1 */
        if (p[i + 3u] != 'T')   continue;          /* TYPE = 'T' */
        pl      = p[i + 2u];
        status  = p[i + 4u];
        langlen = (uint8)(status & 0x3Fu);
        if ((uint16)pl < (uint16)(1u + langlen)) continue;
        src  = (uint8)(i + 5u + langlen);
        body = (uint16)(pl - 1u - langlen);
        if ((uint16)src + body > n) continue;
        w = 0;
        if ((status & 0x80u) != 0u) {              /* UTF-16LE: 只取低字节 */
            for (k = 0; k < body; k++) {
                if ((uint16)(src + (uint8)(k * 2u) + 1u) >= n) break;
                if (w + 1u >= max) break;
                out[w++] = (char)p[src + (uint8)(k * 2u) + 1u];
            }
        } else {
            for (k = 0; k < body; k++) {
                char ch;
                if (w + 1u >= max) break;
                ch = (char)p[(uint8)(src + (uint8)k)];
                out[w++] = (char)(((ch >= 0x20) && (ch < 0x7F)) ? ch : '.');
            }
        }
        out[w] = '\0';
        g_ndef_at = (uint8)i;
        return NFC_OK;
    }
    return NFC_ERR_NDEF;
}

uint8 NFC_ReadTextEx(uint8 slv, char *out, uint16 max)
{
    uint16 off = 0u;
    uint8  st = 0xFFu;
    if (out == 0 || max < 2u) { return NFC_ERR_PARAM; }
    out[0] = '\0';
    g_ndef_at = 0xFFu;
    g_mem_len = 0u;
    while (off < MN63Y_FERAM_SIZE) {
        uint16 left = (uint16)(MN63Y_FERAM_SIZE - off);
        uint8  n = (uint8)((left > (uint16)NFC_READ_CHUNK) ? (uint16)NFC_READ_CHUNK : left);
        uint8  r = NFC_ReadMem(slv, off, n, &g_mem[off], &st);
        if (r != NFC_OK) { break; }
        off = (uint16)(off + n);
        g_mem_len = off;
        NFC_DelayMs(1u);
    }
    if (off == 0u) { return NFC_ERR_NOACK; }
    return ndef_find_text(g_mem, off, out, max);
}

uint8 NFC_WriteTextEx(uint8 slv, uint16 addr, const char *text)
{
    static __xdata uint8 blk[64];
    uint8  i, k, tlen = 0u, langlen = 2u, paylen, n;
    uint8  st = 0xFFu;
    uint16 total;
    if (text == 0) { return NFC_ERR_PARAM; }
    while (text[tlen] != '\0' && tlen < 40u) { tlen++; }
    paylen = (uint8)(1u + langlen + tlen);
    total  = (uint16)(3u + paylen);
    blk[0] = (uint8)(total >> 8);
    blk[1] = (uint8)(total & 0xFFu);
    blk[2] = 0xD1u;
    blk[3] = 0x01u;
    blk[4] = paylen;
    blk[5] = 'T';
    blk[6] = langlen;
    blk[7] = 'e';
    blk[8] = 'n';
    for (k = 0; k < tlen; k++) { blk[9u + k] = (uint8)text[k]; }
    n = (uint8)(9u + tlen);
    for (i = n; i < 64u; i++) { blk[i] = 0x00u; }
    return NFC_WriteMem(slv, addr, blk, 64u, &st);
}

/* ===========================================================================
 *                   上拉 / 电源 诊断 (v5 新增)
 *   "硬件没问题"这句话必须过的三道量化关卡:
 *     A) 外部上拉在不在   -> NFC_BusOn()  低 4 位(三态时) 必须 = 0b11
 *     B) 断电时线有没有倒灌 -> NFC_BusOff() 低 4 位(三态时) 必须 = 0b00
 *     C) VDDEX 有没有去耦电容 / 到底连没连到芯片 -> NFC_VddHoldMs()
 * =========================================================================== */
void NFC_SetTristate(uint8 on) { in_pu_off = (on != 0u) ? 1u : 0u; }

uint8 NFC_BusLevel2(void)
{
    sda_rel(); scl_rel();
    dly_l();
    return (uint8)(P0 & 0x03u);        /* bit0 = SDA(P0.0), bit1 = SCL(P0.1) */
}

uint8 NFC_BusOn(void)
{
    uint8 a, b;
    in_pu_off = 0u; gpio_prep(); P2_3 = 1; dly_l();
    a = NFC_BusLevel2();                       /* 有 MCU 内部上拉时 */
    in_pu_off = 1u; gpio_prep(); P2_3 = 1; dly_l();
    b = NFC_BusLevel2();                       /* 三态(只有外部上拉能拉高) */
    in_pu_off = 0u; gpio_prep(); P2_3 = 1; dly_l();
    return (uint8)((a << 4) | (b & 0x0Fu));
}

uint8 NFC_BusOff(void)
{
    uint8 a, b;
    in_pu_off = 0u; gpio_prep(); P2_3 = 0; dly_l();
    a = NFC_BusLevel2();
    in_pu_off = 1u; gpio_prep(); P2_3 = 0; dly_l();
    b = NFC_BusLevel2();
    in_pu_off = 0u; gpio_prep();
    return (uint8)((a << 4) | (b & 0x0Fu));
}

uint16 NFC_VddHoldMs(void)
{
    uint16 ms = 0u;
    in_pu_off = 1u;                    /* 别让内部上拉经 ESD 二极管给节点续命 */
    gpio_prep();
    P2_3 = 1;
    NFC_DelayMs(50u);                  /* 先把 VDDEX 上的电容充满 */
    P2DIR &= (uint8)~0x08u;            /* 放开 P2.3, 看节点自己撑多久 */
    while (ms < 500u) {
        if ((P2 & 0x08u) == 0u) { break; }
        NFC_DelayMs(1u);
        ms++;
    }
    P2DIR |= 0x08u;
    P2_3 = 1;
    in_pu_off = 0u;
    gpio_prep();
    return ms;
}

uint8 NFC_NirqWatchMs(uint16 ms, uint16 *lows)
{
    uint16 n = 0u, l = 0u;
    while (n < ms) {
        if (NFC_NirqLevel() == 0u) { l++; }
        NFC_DelayMs(1u);
        n++;
    }
    if (lows != 0) { *lows = l; }
    return (uint8)((l != 0u) ? 1u : 0u);
}

uint8 NFC_ScanRd(uint8 rd, uint8 *first3)
{
    uint8 a, n = 0u, k;
    first3[0] = 0xFFu; first3[1] = 0xFFu; first3[2] = 0xFFu;
    for (a = 0u; a < 128u; a++) {
        k = NFC_AckProbe(a, rd, 0);
        if (k == NFC_ACK_REAL) {
            if (n < 3u) { first3[n] = a; }
            n++;
        }
    }
    return n;
}

uint8 NFC_Probe(uint8 addr7, uint8 rd)
{
    return NFC_AckProbe(addr7, rd, 0);
}
