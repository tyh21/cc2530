/**
 * @file    nfc.c
 * @brief   MN63Y1213 Serial(I2C) 模式驱动  —— 帧格式照抄原厂固件实证
 *
 * ============================================================================
 * 这份驱动的依据不是猜的，而是原厂固件 yuanma_LG_290 的反汇编实证：
 *
 *   bank0 0x7CC7  i2c_write_seq(wordaddr, buf, len)
 *   bank0 0x7C10  i2c_read_seq(len, buf)
 *   bank0 0x7DAD  nfc_read(addr, count, dst)
 *
 * ★ 它就是一颗标准 I2C EEPROM 的访问模型，而不是带命令码的寄存器设备：
 *
 *     写 / 定位 : START  0xAA  ACK  block(8bit)  ACK  [d0 ACK d1 ACK ...]  STOP
 *     读        : ① 虚写定位  START 0xAA ACK block ACK STOP
 *                 ② START  0xAB  ACK  d0 ACK(master)  d1 ACK(master) ...  STOP
 *
 * ★ 三个最容易搞错的地方（前一版就是在这些地方错的）：
 *     1. 地址是 8 位 "block 号(0-31)"，不是 16 位字节地址。
 *        手册 Ch.3：32 blocks x 16 bytes = 4 Kbit。
 *     2. 定位与读取之间是 STOP，没有 repeated-START；第二个 START 之前要有间隔。
 *     3. 没有"命令字节 0x08 / 长度字节"，也没有写完后的长忙等。
 *        原厂 0x7DED 处：write_seq(block, NULL, 0) → read_seq(16)，两者直接相邻。
 *
 * 依据的原文片段：
 *   7DE2: MOV DPTR,#0x1B61 / MOV A,#0x55 / MOVX @DPTR,A   ← slave = 0x55
 *   7DF3: MOV A,SFR(0x0C) ; ADD A,SFR(0x0A) ; MOV R1,A    ← 地址是 8 位, 带偏移
 *   7DED: MOV R4,#0x00 (len=0), 7DF8: LCALL write_seq     ← 虚写: 只发地址就 STOP
 *   7DFB: MOV R1,#0x10 (len=16), 7E07: LCALL read_seq     ← 每次固定读 16 字节
 *   7E33: ADD A,#0x10                                      ← 步进 0x10 = 一个 block
 *
 * MN63Y1213 datasheet 印证：
 *   Ch.3  32 blocks x 16 B, block 0-27 = 用户区, 28-31 = 系统区
 *   Ch.2  SCL/SDA = N-ch open drain, 需外部上拉, 有效频率 20 kHz - 100 kHz
 *         VDDEX 供电后才能 Serial/Tunnel 通信，且上电后需等 tBoot
 *
 * ---- 时序标定记录（改 DLY_S/DLY_L 之前先看这里）----------------------------
 *   dly() 编译后每次迭代约 17~18 个时钟周期（计数在内部 RAM，直寻址，无 MOVX）。
 *   i2c_tx 每一位 = dly(S) + dly(L) + dly(S) + 若干位操作 ≈ 72*DLY_S + 48 周期。
 *   当前 DLY_S=20 / DLY_L=40  ->  每位 ≈ 1488 周期 ≈ 46.5 us @32MHz -> SCL ≈ 21 kHz。
 *   手册允许 20~100 kHz，这里贴着下限取，距离上限有 5 倍余量。
 *     · 完全读不到 ACK 时：DLY_S/DLY_L 一起翻倍 -> ~10 kHz（接近原厂实测 8.8 kHz）
 *     · 想跑更快：DLY_S=4/DLY_L=8 -> ~100 kHz，不推荐
 * ============================================================================
 */
#include "nfc.h"

/* ---------------------------------------------------------------- 引脚 ---- */
#define SDA         P0_0
#define SCL         P0_1
#define VDDEX       P2_3

#define SLAVE_7BIT  0x55u
#define I2C_WR      ((uint8)(SLAVE_7BIT << 1))        /* 0xAA */
#define I2C_RD      ((uint8)((SLAVE_7BIT << 1) | 1))  /* 0xAB */

#define BLOCK_SIZE  16u

/* --------------------------------------------------------- 延时/速率 ------ */
/*
 * 下面三个常数由实测反汇编标定（见文件末尾 "时序标定记录"）。
 * - DLY_S / DLY_L 必须保持 DLY_L = DLY_S * 2，比例与原厂固件一致
 * - 目前标定目标：SCL ≈ 9 kHz，与原厂固件实测值 ~8.8 kHz 相当。
 *   手册允许 20~100 kHz；跑慢一点余量更大，想提速按同比例缩小这两个常数。
 */
#define DLY_S       20u     /* 短延时 */
#define DLY_L       40u     /* 长延时 = DLY_S * 2 */
#define DLY_1MS     1780u   /* dly_ms() 每毫秒的迭代数 */

/*
 * ★ 为什么延时计数用静态 __idata 变量，而不是 volatile 局部变量：
 *   IAR 8051 把局部变量放在"伪栈" PSTACK 上，PSTACK 位于 XDATA 第 0 页(0x0000
 *   起)，编译器用寄存器间接 MOVX 访问它；而 MOVX @Ri 的地址高字节取自 P2 锁存。
 *   本工程里 P2.3 = NFC 的 VDDEX 供电脚，一旦把它拉高(P2 != 0)，所有走 PSTACK
 *   的读写（含 IAR 运行库自身）就会落到 XDATA 的另一页上，正好砸进 EPD 帧缓冲，
 *   表现为屏幕上出现莫名的脏点。
 *   静态 __idata 变量走直寻址，不产生 MOVX —— 从根上避开，且迭代周期数稳定。
 */
static __idata volatile uint16 dly_cnt;   /* volatile: 防止忙等被优化掉 */
static __idata uint16 dly_lim;
static __idata uint8  dly_mul = 1u;

/*
 * 速率倍率：1 = 标称(约 21 kHz)，2 = 半速(约 10 kHz)，4 = 四分之一速(约 5 kHz)。
 * 探测时用它把同一个 Probe 在三种速率下各跑一遍，一次就能看出是不是"太慢/太快"
 * 的问题，省一次烧录。
 */

void NFC_SetSpeed(uint8 mul)
{
    dly_mul = (mul == 0u) ? 1u : mul;
}

static void dly(uint16 n)
{
    dly_lim = (uint16)(n * dly_mul);
    dly_cnt = 0;
    while (dly_cnt < dly_lim) {
        dly_cnt++;
    }
}

/* 毫秒级忙等（用于 VDDEX 上电后的 tBoot） */
static void dly_ms(uint16 ms)
{
    uint16 m;
    for (m = 0; m < ms; m++) {
        dly(DLY_1MS);
    }
}

void NFC_DelayMs(uint16 ms)
{
    dly_ms(ms);
}

/* --------------------------------------------------------- 诊断变量 ------- */
uint8  g_nfc_ack_w   = 0;    /* 最后一次 i2c_tx 的 ACK 结果：0=收到ACK */
uint8  g_nfc_ack_r   = 0;    /* 读地址阶段的 ACK：0=OK */
uint8  g_nfc_status  = 0;    /* 保留（MN63Y1213 无状态读出需求） */
uint8  g_nfc_lastblk = 0xFF; /* 最后一次访问的 block 号 */
uint8  g_nfc_errcode = 0;    /* 内部错误点，见下方 ERRAT_* */
uint8  g_nfc_ndef_at = 0xFF; /* NDEF 记录在 raw 数据里的偏移，0xFF = 未找到 */
uint8  g_nfc_raw_bytes = 0;  /* 本次实际读到的 tag 字节数 */

#define ERRAT_NONE      0
#define ERRAT_WRADDR    1   /* 发 0xAA 无 ACK */
#define ERRAT_BLK       2   /* 发 block 号无 ACK */
#define ERRAT_DATA      3   /* 写数据无 ACK */
#define ERRAT_RDADDR    4   /* 发 0xAB 无 ACK */

/* ============================================================================
 * 开漏位层 —— ★ 这是本轮最重要的改动
 *
 * 只有两种状态："拉低" 和 "释放(靠上拉变高)"，绝不由 MCU 主动推高。
 *
 * 原因（实测教训）：P2.3 直连 MN63Y1213 的 VDDEX，中间没串电阻。
 * VDDEX 关断时芯片是没电的，这时候如果用推挽把 SDA/SCL 打高，电流会经
 * 芯片引脚的 ESD 保护二极管灌进它已经放电的电源轨 -> 整板 VDD 跌落
 * -> 掉电复位（表现就是"屏没反应"）。
 * 改成开漏 + 片内上拉，最坏情况也只有 ~150uA，不会再拖垮供电。
 * ========================================================================= */
/*
 * drv_pp : 0 = 开漏(释放靠上拉, 最安全)   1 = 推挽(原厂固件做法)
 *
 * 原厂 0x7D4D~0x7D58 实测是推挽：ORL P0DIR,#0x02 / ORL P0DIR,#0x01 然后
 * SETB P0.1 / SETB P0.0 —— 但它在**读**的时候又 ANL P0DIR,#0xFE 把 P0.0 切成输入
 * (0x7CBD / 0x7C53 MOV C,P0.0)。所以原厂真实模型是：
 *      主机发数据/起停  -> 推挽
 *      主机读 ACK / 收数据 -> 输入(开漏+上拉)
 * 这里两种模式都能跑，排查时可以用 NFC_SetDrive(1) 完全复刻原厂。
 * ★ 推挽只允许在"主机独占总线"的阶段用；读阶段必须切成输入，
 *   否则从机拉低 ACK 时会被 MCU 的高电平顶住，灌大电流。
 */
static __idata uint8 drv_pp = 0u;

void NFC_SetDrive(uint8 pp)
{
    drv_pp = (pp != 0u) ? 1u : 0u;
}

static void sda_low(void) { P0_0 = 0; P0DIR |=  0x01u; }
static void scl_low(void) { P0_1 = 0; P0DIR |=  0x02u; }

static void sda_hi(void)
{
    P0_0 = 1;
    if (drv_pp) { P0DIR |= 0x01u; } else { P0DIR &= (uint8)~0x01u; }
}

static void scl_hi(void)
{
    P0_1 = 1;
    if (drv_pp) { P0DIR |= 0x02u; } else { P0DIR &= (uint8)~0x02u; }
}

/* 读 ACK / 收数据阶段：必须释放成输入 */
static void sda_in(void) { P0_0 = 1; P0DIR &= (uint8)~0x01u; }

/* --------------------------------------------------------- 位层 ----------- */
static void i2c_start(void)
{
    scl_hi();
    sda_hi();                 /* 空闲态：高 */
    dly(DLY_L);
    sda_low();                /* SCL 为高时 SDA 1→0 = START */
    dly(DLY_S);
    scl_low();
    dly(DLY_S);
}

static void i2c_stop(void)
{
    scl_low();
    sda_low();
    dly(DLY_S);
    scl_hi();                 /* SCL 拉高 */
    dly(DLY_S);
    sda_hi();                 /* SCL 为高时 SDA 0→1 = STOP */
    dly(DLY_L);
}

/* 发 1 字节，然后读 ACK。返回 0 = 从机回了 ACK */
static uint8 i2c_tx(uint8 b)
{
    uint8 i;
    for (i = 0; i < 8; i++) {
        if (b & 0x80u) { sda_hi(); } else { sda_low(); }
        dly(DLY_S);           /* 数据建立 */
        scl_hi();
        b = (uint8)(b << 1);
        dly(DLY_L);           /* SCL 高电平保持 = 低电平的 2 倍 */
        scl_low();
        dly(DLY_S);
    }
    /* ---- 第 9 个时钟读 ACK ---- */
    sda_in();                 /* 释放 SDA：此刻绝不能推挽 */
    dly(DLY_S);
    scl_hi();
    dly(DLY_S);
    g_nfc_ack_w = P0_0 ? 1u : 0u;   /* 0 = ACK(从机拉低) */
    dly(DLY_S);
    scl_low();
    dly(DLY_S);
    return g_nfc_ack_w;
}

/* 收 1 字节（SDA 全程输入，由上拉拉高） */
static uint8 i2c_rx(void)
{
    uint8 i, b = 0;
    for (i = 0; i < 8; i++) {
        sda_in();
        dly(DLY_S);
        scl_hi();
        dly(DLY_S);
        b = (uint8)(b << 1);
        if (P0_0) {
            b |= 0x01u;
        }
        dly(DLY_S);
        scl_low();
        dly(DLY_L);           /* 读位时 SCL 低电平长、高电平短（与写相反） */
    }
    return b;
}

/*
 * 主机在第 9 个时钟回 ACK/NACK。
 * ★ 前一版的 bug 在这里：先 SETB SCL 再改 SDA。若 nack=1，SCL 已经为高时
 *   SDA 由 0→1，从机会把它当成 STOP 条件，整条总线立刻乱掉。
 *   正确做法（同原厂 0x7C88）：在 SCL 为低期间把 SDA 先设好，再拉高 SCL。
 */
static void i2c_putack(uint8 nack)
{
    scl_low();
    if (nack) { sda_hi(); } else { sda_low(); }
    dly(DLY_S);
    scl_hi();
    dly(DLY_L);
    scl_low();
    sda_low();                /* 收尾保持低，避免毛刺被误判成 START/STOP */
    dly(DLY_S);
}

/* --------------------------------------------------------- 帧层 ----------- */

/* 定位：发 DEVICE_ADDR + block 号后 STOP（写入数据时 len>0） */
static uint8 mn63y_seq(uint8 block, const uint8 *src, uint8 len)
{
    uint8 i;

    g_nfc_lastblk = block;
    g_nfc_errcode = ERRAT_NONE;

    i2c_start();
    if (i2c_tx(I2C_WR) != 0) {
        g_nfc_errcode = ERRAT_WRADDR;
        i2c_stop();
        return 1;
    }
    if (i2c_tx(block) != 0) {
        g_nfc_errcode = ERRAT_BLK;
        i2c_stop();
        return 1;
    }
    for (i = 0; i < len; i++) {
        if (i2c_tx(src[i]) != 0) {
            g_nfc_errcode = ERRAT_DATA;
            i2c_stop();
            return 1;
        }
    }
    i2c_stop();
    return 0;
}

/* 读一个 block。dst 至少 len 字节（<=16） */
uint8 NFC_ReadBlock(uint8 block, uint8 *dst, uint8 len)
{
    uint8 i;

    if (len == 0 || len > BLOCK_SIZE) {
        return NFC_ERR_PARAM;
    }

    /* ① 虚写：把内部地址指针指到 block */
    g_nfc_lastblk = block;
    g_nfc_errcode = ERRAT_NONE;
    i2c_start();
    if (i2c_tx(I2C_WR) != 0) {
        g_nfc_errcode = ERRAT_WRADDR;
        i2c_stop();
        g_nfc_ack_r = 1;
        return NFC_ERR_I2C;
    }
    if (i2c_tx(block) != 0) {
        g_nfc_errcode = ERRAT_BLK;
        i2c_stop();
        g_nfc_ack_r = 1;
        return NFC_ERR_I2C;
    }
    i2c_stop();
    dly(DLY_L);               /* 两次独立传输之间留一点间隔 */

    /* ② 读：START + 读地址 + 数据，每个字节后主机回 ACK */
    i2c_start();
    if (i2c_tx(I2C_RD) != 0) {
        g_nfc_errcode = ERRAT_RDADDR;
        i2c_stop();
        g_nfc_ack_r = 1;
        return NFC_ERR_I2C;
    }
    g_nfc_ack_r = 0;
    for (i = 0; i < len; i++) {
        dst[i] = i2c_rx();
        i2c_putack(0);        /* 原厂对所有字节（含最后一个）都回 ACK */
    }
    i2c_stop();
    return NFC_OK;
}

/* 写一个 block（<=16 字节）。用于 MCU→手机的反向传字符 */
uint8 NFC_WriteBlock(uint8 block, const uint8 *src, uint8 len)
{
    if (len == 0 || len > BLOCK_SIZE) {
        return NFC_ERR_PARAM;
    }
    if (mn63y_seq(block, src, len) != 0) {
        return NFC_ERR_I2C;
    }
    dly_ms(2);                /* FeRAM 写入很快，留 2ms 足够 */
    return NFC_OK;
}

/* --------------------------------------------------------- 电源 / 初始化 -- */
static void i2c_init(void)
{
    P0SEL &= (uint8)~0x03u;   /* P0.0/P0.1 -> GPIO */
    P0INP &= (uint8)~0x03u;   /* 使能片上上/下拉 */
    P2INP &= (uint8)~0x20u;   /* PDUP0=0 -> Port0 选上拉 */
    P0IEN &= (uint8)~0x03u;   /* 关引脚中断，否则 bit-bang 抖动会不停进中断 */
    P0_0 = 1;                 /* 锁存先置 1：释放时就是上拉，不会推出高电平 */
    P0_1 = 1;
    P0DIR &= (uint8)~0x03u;   /* 释放总线 */
    dly(DLY_L);
}

/*
 * 诊断：把总线释放后读回两根线的电平。
 * bit0=SDA bit1=SCL。正常应该是 0x03（都被上拉拉高）；
 * 若为 0 说明片上上拉没起来（外部也没上拉），那时"扫到一堆 ACK"是假的。
 */
uint8 NFC_BusLevel(void)
{
    uint8 v = 0;

    i2c_init();
    dly(DLY_L);
    if (P0_0) { v |= 0x01u; }      /* 分开读：同一表达式里两个 volatile 位会报 Pa082 */
    if (P0_1) { v |= 0x02u; }
    return v;
}

void NFC_PowerOn(void)
{
    P2SEL &= (uint8)~0x08u;   /* P2.3 -> GPIO */
    P2DIR |= 0x08u;           /* 输出 */
    VDDEX = 1;                /* ★ 手册: Serial/Tunnel 模式必须供 VDDEX */
    dly_ms(15);               /* tBoot: 上电后等待再访问 */
    i2c_init();
}

void NFC_PowerOff(void)
{
    VDDEX = 0;
    dly_ms(1);
}

/* --------------------------------------------------------- 诊断 ----------- */

/* 只探测总线上有没有 ACK：发 START + 0xAA 就 STOP */
uint8 NFC_Probe(void)
{
    uint8 r;
    i2c_init();
    i2c_start();
    r = i2c_tx(I2C_WR);
    i2c_stop();
    return r;
}

/*
 * 总线恢复：SDA 保持高，给 20 个 SCL 脉冲，最后补一个 STOP。
 * 若从机卡在某个字节的中间（之前被复位打断过），这串时钟能把它踢回空闲态。
 */
void NFC_BusRecover(void)
{
    uint8 i;

    sda_in();
    scl_hi();
    dly(DLY_L);
    for (i = 0; i < 20u; i++) {
        scl_low();
        dly(DLY_L);
        scl_hi();
        dly(DLY_L);
    }
    i2c_stop();
}

/*
 * 扫总线：对 0x00-0x7F 全部 7 位地址各发一次"写地址"看 ACK。
 * 返回应答的个数，第一个应答的 7 位地址写进 *first（没有则 0xFF）。
 * 排查用：一次就能知道总线上到底有没有器件、地址是不是 0x55。
 */
uint8 NFC_ScanAddr(uint8 *first)
{
    uint8 a, n = 0, r;

    i2c_init();
    *first = 0xFFu;
    for (a = 0; a < 128u; a++) {
        i2c_start();
        r = i2c_tx((uint8)((uint8)(a << 1) & 0xFEu));
        i2c_stop();
        if (r == 0u) {
            if (n == 0u) {
                *first = a;
            }
            n++;
        }
    }
    return n;
}

/*
 * 引脚/通路自检 —— 这一级回答的是"线到底通不通"，而不是"从机回没回话"。
 *
 * 做法：把 SDA 强推挽拉低（P0.0=0 + P0DIR|=0x01），然后分别读回 P0.0 的输入锁存。
 *   读到 0 = 拉低有效（脚能控制、没被别的强驱动顶住）
 *   读到 1 = SDA 被别的东西强行拉高 / 脚焊错 / 根本没接在这颗 MCU 的 P0.0 上
 * bit0 在 SCL 低时测，bit1 在 SCL 高时测（顺便看 SCL 电平有没有影响）。
 *
 * ★ 为什么要做这个：上一版 LO 级出现 "128 个地址全 ACK"，那只有一种解释 ——
 *   SCL 被钉在高电平、START 条件根本发不出来，于是第 9 个时钟谁都没拉 SDA，
 *   代码把"读到高"当成了 ACK。也就是说那次 n=128 是**假 ACK**。
 *   要区分"总线死了"和"芯片没说话"，就必须先证明路是通的。
 */
uint8 NFC_PinTest(void)
{
    uint8 v = 0;

    i2c_init();
    dly(DLY_L);

    /* ---- ① SCL 拉低时 ---- */
    scl_low();
    dly(DLY_L);
    P0_0 = 0;
    P0DIR |= 0x01u;           /* SDA 强推挽拉低 */
    dly(DLY_L);
    if ((P0 & 0x01u) == 0u) { /* 读回：确实被拉低了？ */
        v |= 0x01u;
    }
    sda_in();
    scl_hi();
    dly(DLY_L);

    /* ---- ② SCL 拉高时 ---- */
    scl_hi();
    dly(DLY_L);
    P0_0 = 0;
    P0DIR |= 0x01u;
    dly(DLY_L);
    if ((P0 & 0x01u) == 0u) {
        v |= 0x02u;
    }
    sda_in();
    scl_hi();
    dly(DLY_L);

    return v;
}

/*
 * 双线互证：把 SDA 拉低，看 SCL 会不会跟着动（两线短路会跟着动）。
 * ★ 这里必须读 P0 的**输入锁存**（P0 & mask），不能用 P0_0 这种位寻址写法 ——
 *   P0DIR=1 时位地址读到的是输出锁存，恒等于我们写进去的值，永远看不出真相。
 */
uint8 NFC_XTalk(void)
{
    uint8 v = 0;

    i2c_init();
    dly(DLY_L);

    scl_hi();                 /* SCL 释放成高 */
    dly(DLY_L);
    sda_in();                 /* SDA 先释放 */
    dly(DLY_L);
    if ((P0 & 0x01u) != 0u) { /* SDA 应该被上拉拉高 */
        v |= 0x01u;
    }

    P0_0 = 0;
    P0DIR |= 0x01u;           /* SDA 推挽拉低 */
    dly(DLY_L);
    if ((P0 & 0x02u) != 0u) { /* SCL 若也跟着低 -> 短到一起了 */
        v |= 0x02u;
    }

    sda_in();
    scl_hi();
    dly(DLY_L);
    return v;
}

/* 把所有总线引脚恢复成"不驱动"：高阻输入 + 关掉片上上/下拉 */
void NFC_BusFloat(void)
{
    P0SEL &= (uint8)~0x03u;
    P0DIR &= (uint8)~0x03u;   /* 输入 */
    P0INP |= 0x03u;           /* 三态：连内部上拉也关掉 */
    P0_0 = 0;
    P0_1 = 0;
}

/*
 * 单线拉高测试：只把 SDA 推挽拉高（或只把 SCL），返回读回的电平。
 * 用途 —— 找出"到底哪根线在往外部倒灌电流、把 VDD 拖低"：
 *   配合 main 里"每拉一根线就量一次 VDD"，就能定位是哪根。
 * bit0 = SDA 读回电平(1=拉高了), bit1 = SCL 读回电平(1=拉高了)
 */
uint8 NFC_PullHighTest(void)
{
    uint8 v = 0;

    /* 先全部悬空 */
    NFC_BusFloat();
    dly(DLY_L);

    /* 只驱动 SDA */
    P0_0 = 1;
    P0DIR |= 0x01u;
    dly(DLY_1MS);             /* 给足时间让倒灌电流体现出来 */
    if ((P0 & 0x01u) != 0u) { v |= 0x01u; }

    /* 收回 SDA，单独驱动 SCL */
    P0DIR &= (uint8)~0x01u;
    P0_1 = 1;
    P0DIR |= 0x02u;
    dly(DLY_1MS);
    if ((P0 & 0x02u) != 0u) { v |= 0x02u; }

    NFC_BusFloat();
    return v;
}

/* ============================================================================
 * ★ 模拟法引脚诊断 —— 绕开"数字读引脚"的所有坑
 *
 * 背景：NFC_PinTest() 报 PT=0，即"推挽拉低 SDA 后，数字读回 P0.0 仍是 1"。
 * 这有两种完全相反的解释：
 *   (a) 外部有东西把 SDA 强拽在高电平  -> 硬件/走线问题
 *   (b) 我们能拉低，但读引脚的代码有问题 -> 纯软件问题，改一行就好
 * 数字读回区分不了这两者。
 *
 * 解法：P0.0 = AIN0, P0.1 = AIN1，直接用 ADC 量这两个脚的**真实模拟电压**，
 * 完全绕开数字输入锁存。
 *
 * 巧妙之处 —— 自校准，不需要知道任何参考电压：
 *   先把脚推挽拉低 -> 读 ADC 得 code_lo
 *   再把脚推挽拉高 -> 读 ADC 得 code_hi
 *   用 code_lo / code_hi 的百分比表示"拉低时残留多少电平"：
 *     ~0%   = 确实拉低了，外部没跟我们抢 -> (b)，软件问题
 *     >50%  = 外部有强驱动把脚顶在高电平 -> (a)，硬件问题
 *   参考电压、满量程、抽取率全部约掉，不需要任何标定常数。
 *
 * ★ ADC 测量不受 P0DIR 影响：它量的是引脚真实电势，不是输出锁存。
 *   所以当"外面在跟我们对顶"时，ADC 能如实反映出来。
 * ========================================================================= */
uint16 g_nfc_a0_lo, g_nfc_a0_hi, g_nfc_a1_lo, g_nfc_a1_hi;

/* ADCCON3 = SREF(10=AVDD5) | SDIV(11=512) | ECH  —— 用 AVDD5 当基准，
   这样读出来的码就是"该脚电压 / VDD"的比例，正好是我们想要的形式。 */
#define ADC_AIN0    (0x80u | 0x30u | 0x00u)
#define ADC_AIN1    (0x80u | 0x30u | 0x01u)

static __idata uint16 a_guard;

static uint16 adc_read_raw(uint8 cfg)
{
    uint16 v;

    ADCCON1 = 0x30u;          /* STSEL=11 手动触发，清 EOC/ST */
    ADCCON3 = cfg;
    ADCCON1 |= 0x40u;         /* ST=1 启动 */

    for (a_guard = 0; a_guard < 60000u; a_guard++) {
        if (ADCCON1 & 0x80u) {
            break;
        }
    }
    v = (uint16)ADCL;
    v |= (uint16)((uint16)ADCH << 8);
    return (uint16)(v >> 2);  /* 14 位补码 -> 幅值 */
}

void NFC_PinAnalog(void)
{
    APCFG |= 0x03u;           /* P0.0/P0.1 作模拟输入（会覆盖 P0SEL） */
    P0SEL &= (uint8)~0x03u;

    /* ---- P0.0 (SDA / AIN0) ---- */
    P0_0 = 0; P0DIR |= 0x01u; /* 推挽拉低 */
    dly_ms(5);
    g_nfc_a0_lo = adc_read_raw(ADC_AIN0);

    P0_0 = 1; P0DIR |= 0x01u; /* 推挽拉高 */
    dly_ms(5);
    g_nfc_a0_hi = adc_read_raw(ADC_AIN0);

    P0DIR &= (uint8)~0x01u;   /* 释放回输入 */
    P0_0 = 1;

    /* ---- P0.1 (SCL / AIN1) ---- */
    P0_1 = 0; P0DIR |= 0x02u;
    dly_ms(5);
    g_nfc_a1_lo = adc_read_raw(ADC_AIN1);

    P0_1 = 1; P0DIR |= 0x02u;
    dly_ms(5);
    g_nfc_a1_hi = adc_read_raw(ADC_AIN1);

    P0DIR &= (uint8)~0x02u;
    P0_1 = 1;

    APCFG &= (uint8)~0x03u;   /* 用完还原成数字脚 */
}

/* --------------------------------------------------------- NDEF 解析 ------ */
/*
 * Type3 Tag 布局（手机写入时生成的）：
 *   block 0            : Attribute Information Block
 *   block 1 起         : [NLEN 高][NLEN 低][NDEF 记录 ...]
 *   NDEF 短记录(Text)  : hdr(0xD1) TYPE_LEN(0x01) PAYLOAD_LEN TYPE('T') [status][lang..][text..]
 *   status 字节        : bit7=UTF-16标志, bit5..0=语言码长度
 *
 * 不硬假设"一定从某个偏移开始"，而是在读出来的整段数据里扫描特征，
 * 这样无论手机用了哪个 App 写、落在哪个 block，都能识别。
 */
static uint8 ndef_find_text(const uint8 *p, uint8 n, char *out, uint16 max)
{
    uint8  i;
    uint16 body, k;

    for (i = 0; i + 5u < n; i++) {
        /* SR(短记录) 且 TNF=well-known(0x01)，TYPE LENGTH=1，TYPE='T' */
        if (((p[i] & 0x10u) == 0) || ((p[i] & 0x1Fu) != 0x01u))  continue;
        if (i + 3u >= n) break;
        if (p[i + 1] != 0x01u) continue;
        if (p[i + 3] != 'T')  continue;

        {
            uint8  pl      = p[i + 2];
            uint8  status  = p[i + 4];
            uint8  langlen = (uint8)(status & 0x3Fu);
            uint8  src     = (uint8)(i + 5u + langlen);
            int    len     = (int)pl - 1 - (int)langlen;

            if (len <= 0) continue;
            if ((uint16)src + (uint16)len > n) continue;

            body = (uint16)len;
            if (body > (uint16)(max - 1u)) {
                body = (uint16)(max - 1u);
            }

            if (status & 0x80u) {
                /* UTF-16: 每字符 2 字节，这里降级取低字节（只处理 ASCII） */
                uint16 w = 0;
                for (k = 0; k < body; k++) {
                    if (src + k * 2u + 1u >= n) break;
                    out[w++] = (char)p[src + k * 2u + 1u];
                }
                out[w] = '\0';
            } else {
                /* UTF-8（常见手机 App 默认） */
                uint16 w = 0;
                char   ch;
                for (k = 0; k < body; k++) {
                    ch = (char)p[src + k];
                    out[w++] = (ch >= 0x20 && ch < 0x7F) ? ch : '.';
                }
                out[w] = '\0';
            }
            g_nfc_ndef_at = i;
            return NFC_OK;
        }
    }
    return NFC_ERR_NDEF;
}

/* --------------------------------------------------------- 高层 API ------ */

/* 读 tag 数据到 raw（自动 block -> 字节），返回读到的字节数 */
uint8 NFC_ReadRaw(uint8 startBlock, uint8 nBlocks, uint8 *dst)
{
    uint8 k;
    if (dst == 0 || nBlocks == 0) {
        return 0;
    }
    for (k = 0; k < nBlocks; k++) {
        if (NFC_ReadBlock((uint8)(startBlock + k), &dst[(uint8)(k * BLOCK_SIZE)], BLOCK_SIZE) != NFC_OK) {
            return k * BLOCK_SIZE;   /* 只返回成功读到的块数 */
        }
    }
    return nBlocks * BLOCK_SIZE;
}

/* 读 NDEF 文本（手机 → MCU → 墨水屏） */
uint8 NFC_ReadText(char *out, uint16 max)
{
    static __xdata uint8 raw[NFC_RAW_BLOCKS * BLOCK_SIZE];
    uint8 got;

    if (out == 0 || max < 2u) {
        return NFC_ERR_PARAM;
    }
    out[0] = '\0';
    g_nfc_ndef_at = 0xFF;

    /* 注意：不再内部上电，由调用方决定 VDDEX 怎么给（排查供电问题时要能分开控制）*/
    got = NFC_ReadRaw(0, NFC_RAW_BLOCKS, raw);
    if (got < BLOCK_SIZE * 2u) {
        return NFC_ERR_I2C;      /* 连 AIB + NDEF 头都没读到 */
    }
    g_nfc_raw_bytes = got;

    return ndef_find_text(raw, got, out, max);
}

/*
 * 反方向：MCU 写一条 NDEF Text，让手机能读到（MCU → 手机）
 * 写入 block 1 起：[NLEN高][NLEN低][0xD1][0x01][PL]['T'][status][lang..][text]
 */
uint8 NFC_WriteText(const char *text)
{
    static __xdata uint8 blk[BLOCK_SIZE];
    uint8  i, k, langlen, paylen, total;
    uint16 nlen;

    if (text == 0) {
        return NFC_ERR_PARAM;
    }
    /* payload = status(1) + lang(2, "en") + body */
    langlen = 2;
    {
        uint8 tlen = 0;
        while (text[tlen] != '\0' && tlen < 200u) {
            tlen++;
        }
        /* 一条 NDEF 短记录最多 255 字节 payload，这里用两个 block 足够 */
        paylen = (uint8)(1 + langlen + tlen);
        if (paylen > (BLOCK_SIZE - 3u)) {
            tlen   = (uint8)(BLOCK_SIZE - 3u - 1 - langlen);
            paylen = (uint8)(1 + langlen + tlen);
        }
        total  = (uint8)(3 + paylen);          /* hdr + typelen + paylen + payload */
        nlen   = (uint16)total;

        for (i = 0; i < BLOCK_SIZE; i++) {
            blk[i] = 0x00;
        }
        blk[0] = (uint8)(nlen >> 8);
        blk[1] = (uint8)(nlen & 0xFFu);
        blk[2] = 0xD1u;                        /* MB|ME|SR|TNF=well-known */
        blk[3] = 0x01u;                        /* TYPE LENGTH */
        blk[4] = paylen;
        blk[5] = 'T';
        blk[6] = langlen;                      /* status: bit7=0 UTF-8, low6=len */
        blk[7] = 'e';
        blk[8] = 'n';
        for (k = 0; k < tlen; k++) {
            blk[9 + k] = (uint8)text[k];
        }
    }

    if (NFC_WriteBlock(1, blk, BLOCK_SIZE) != NFC_OK) {
        return NFC_ERR_I2C;
    }
    return NFC_OK;
}
