/**
 * @file    nfc.c
 * @brief   MN63Y1213 主机侧 I2C 驱动 (v11 —— 严格按原厂固件 0x7Bxx 逆向复刻)
 *
 * ★★★ v11: 严格按原厂固件(fw_LG290_256k.bin) 0x7B00-0x7F30 逆向结果复刻 ★★★
 *
 * ★ v10→v11 重大修正:
 *   1. 从地址 0x54 → 0x55 (原厂 0x7DE5: MOV A,#0x55 写入 XRAM 0x1B61)
 *   2. 命令层改为"直接块寻址"(EEPROM-like), 去掉命令码 0x08/0x18
 *   3. 读改为两段式: 先 write_cmd 设块地址, 再 read_cmd 读数据
 *   4. START 改为原厂非标准时序
 *   5. 物理层时序延时改为原厂量级 (3C/78 单位)
 *
 * ★ 原厂 NFC 物理层映射(全部已确认):
 *   0x7B48 set_slv   0x7B5F read_sda  0x7B8F write_byte
 *   0x7BEE START     0x7BCC STOP
 *   0x7C10 read_cmd  0x7CC7 write_cmd
 *   0x7DAD batch_read 0x7E5C batch_write  0x7EED verify
 *
 * 原厂协议(直接块寻址, 非手册标准命令):
 *   write: START(0x7BEE) -> 写地址(0xAA) -> ACK -> 块地址 -> ACK -> 数据... -> ACK -> STOP(0x7BCC 结束)
 *   read:  先 write_cmd(块地址,len=0) 设地址 -> read_cmd(len) 读数据
 *   结束用 STOP(0x7BCC), 第一字节是块地址, 无命令码!
 *
 * CPU 主频: 16MHz RCOSC (原厂 0xFE11: CLKCONCMD=0x80)
 * SDA/SCL: P0.0/P0.1 推挽 (原厂 0x7D4D: P0DIR|=0x03)
 * VDDEX: P2.3, 复位脉冲 5ms
 *
 * 诊断功能(ScopeTx/Decide9/PinDrive/BusOn 等)保留, 仅供调试, 不在 I2C 通信路径上。
 */
#include "nfc.h"

/* ------------------------------------------------------------ 配置 -------- */
static __idata uint8  swap01  = 0u;   /* 0: SDA=P0.0 SCL=P0.1 */
static __idata uint8  drv_pp  = 1u;   /* SCL 推挽(默认, 数据手册允许) */
static __idata uint8  ord_fac = 1u;   /* 1: 上电后打开 MCU 内部上拉做备份; 0: 纯靠外部上拉 */
static __idata uint8  in_pu_off = 0u; /* 1 = 关掉 P0.0/P0.1 内部上拉(三态), 只为测外部上拉 */
/* ★ v17: 上电序列可配 —— 用于排查"芯片根本没进串行模式" */
static __idata uint8  pwr_pulse = 1u; /* 1 = VDDEX 高→低→高 复位脉冲(v8-v16 默认) */
static __idata uint8  pwr_early = 1u; /* 1 = 先把 SDA/SCL 推挽驱动高, 再上 VDDEX (v8-v16 默认)
                                       * 0 = 先把 VDDEX 上到稳定, 之后才碰两条线
                                       *     手册: SDA/SCL 是 N-ch 开漏, 靠**外部上拉到 VDDEX**;
                                       *     VDDEX=0 时用推挽把线压到 3.3V, 会经芯片 ESD
                                       *     二极管给未上电的芯片"偷供电", 破坏干净的上电复位 */
/* v10: SDA 统一推挽(原厂 0xF5B9: P0DIR|=0x03), 不再用开漏模型 */

/* -------------------------------------------------- Timer1 时间基准 ------- */
static uint8  g_t1ok    = 0u;
static uint16 g_tickNs  = 31u;        /* 1 个 T1 tick = ? ns (默认按 32MHz ≈31.25) */
static uint8  g_calOk   = 0u;         /* 1 = 睡眠定时器可用, 数据是实测的 */
static uint16 g_calT1   = 0u;         /* 参考区间的 T1 tick 数 (原始读数) */
static uint16 g_calSt   = 0u;         /* 参考区间的 ST  tick 数 (原始读数) */
static uint16 g_us4     = 125u;       /* 4us   对应的 T1 tick 数 */
static uint16 g_us10    = 312u;       /* 10us  对应的 T1 tick 数 */
static uint16 g_us20    = 625u;       /* 20us  对应的 T1 tick 数 */
static uint16 g_bitTicks= 1249u;       /* 位周期(诊断参考用) */
/* ★★ v22: T1Init 时钟切换诊断 (串口打印用) —— 排查"轮询 CLKCONSTA 死等" */
static uint8  g_t1ClkCmd0 = 0u;       /* 进入 T1Init 时 CLKCONCMD 原值 */
static uint8  g_t1ClkSta  = 0u;       /* 切换后实测 CLKCONSTA */
static uint8  g_t1ClkCmd  = 0u;       /* 切换后 CLKCONCMD */
static uint16 g_t1WaitLoops = 0u;     /* 轮询圈数 (0 = 一次就过; 60000 = 超时!) */
static uint8  g_t1ClkForced32 = 1u;   /* ★ v23: 1 = 确认本板跑 32MHz XOSC(切 16M 失败) */
uint8 NFC_GetT1Forced32(void) { return g_t1ClkForced32; }
static uint16 g_calSpin  = 0u;        /* v22: TimebaseCal 里等 ST 的自旋圈数(40000=ST不动) */
uint16 NFC_GetCalSpin(void) { return g_calSpin; }
/* ★★ v21: 1ms 对应的 T1 tick 数 (TimebaseCal 算出) —— 给 NFC_DelayMs 用。
 *   16MHz DIV=/1 → 16000; 若 T1 不可用则保持 0, NFC_DelayMs 自动退回 ST 路径。 */
static uint16 g_msTicks = 0u;
/* ★ v11: 原厂 I2C 位时序延时 (原厂 0x1590 delay, 参数 0x3C/0x78)
 *   原厂 @32MHz: 0x3C=60单位≈22us, 0x78=120单位≈45us (0.375us/单位)
 *   本工程 @16MHz: 同循环计数约为一半时间(11us/22us), 仍在 I2C 20-100kHz 范围内
 *   dly_i2c = 原厂 0x3C (短延时, ~22us@32MHz / ~11us@16MHz)
 *   dly_i2c_l = 原厂 0x78 (长延时, ~45us@32MHz / ~22us@16MHz) */
static uint16 g_i2c_3c   = 350u;   /* ~22us @16MHz (原厂 delay(0x3C)=60单位) */
static uint16 g_i2c_78   = 700u;   /* ~45us @16MHz (原厂 delay(0x78)=120单位) */
static uint16 g_i2c_100  = 1600u;  /* ~100us (保留兼容) */

/* ------------------------------------------------------------ 诊断量 ------ */
uint8  g_ack_kind = NFC_ACK_NONE;
uint8  g_status   = 0xFFu;
uint8  g_nirq_ms  = 0xFFu;
uint8  g_ndef_at  = 0xFFu;
uint16 g_mem_len  = 0u;
/* ★ v27: NFC_DiagBackfeed 导出量 */
uint8  g_bfP23    = 0u;   /* P2.3 推挽驱高稳定后读回 (0/1) */
uint16 g_bfHold   = 0u;   /* P2.3 放开后靠外部撑住的 ms */

/* ------------------------------------------------------------ 采样示波 ---- */
/* ★★ v43 XDATA 危机 —— 必须精打细算 ★★
 *   CC2530 只有 8KB SRAM(0x0000-0x1FFF), 而链接器按 _XDATA0_END=0xFFFF
 *   放行, **从不报溢出**。实测 XDATA 已用到 8141 + XSTACK 512 = 8653 > 8192,
 *   表现出来就是"跑到某个调用较深的函数(如 addr scan)时突然死机",
 *   而且报错/卡死位置会随新变量增加而漂移 —— 典型的栈踩出物理 SRAM。
 *   EPD_Frame 独占 4736 字节(0xBC8-0x1E47), 是压倒性的大头, 不能动。
 *   ⇒ 其余所有缓冲在 V40_EPD(纯诊断, 不需要 FeRAM/示波全缓冲)下一律压到最小。 */
#if (defined(V40_EPD) && (V40_EPD != 0)) || (defined(V45_WAKE) && (V45_WAKE != 0))
__xdata uint8 g_scp[1];               /* V40/V45 不用示波缓冲, 只留符号 */
#else
__xdata uint8 g_scp[SCP_N];
#endif
uint8  g_scp_ack = 0u;                /* p<<3 | e<<2 | l<<1 | a */
uint8  g_scp_post = 0x0Fu;            /* SCL 落之后 SDA 回到高用了几个采样点 */

/* ------------------------------------------------------------ FeRAM ------- */
#if (defined(V40_EPD) && (V40_EPD != 0)) || (defined(V45_WAKE) && (V45_WAKE != 0))
/* V40/V45 前端只用 NFC_ReadMemDiag(dst=调用方缓冲), 不碰 g_mem; 压到 1 字节省 511 */
__xdata uint8 g_mem[1];
#else
__xdata uint8 g_mem[MN63Y_FERAM_SIZE];
#endif

/* ★ v23: 前向声明 —— wait_us 定义在文件后部, 但 NFC_VddAlive 等前置函数要用 */
static void wait_us(uint16 us);

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

/* ★★ v44: 地址全扫描的实时进度。写在全局是为了被 ISR / 复位后路径读到:
 *   一旦卡住, 这个值就是"最后一张还没干完的活"。 */
uint8 g_scanCur = 0u;

/* T1 等待超时统计 (v44) */
static uint16 g_waitOvf  = 0u;    /* 累计超时次数   */
static uint16 g_waitLast = 0u;    /* 最后一次要等多少 tick */

/* ★★ v44 硬件看门狗: 唯一的作用是把"无声卡死"变成"可读的报告"。
 *   CC2530 (SWRU191F 手册 p.154):
 *     MODE[1:0] = WDCTL[3:2]: 00=IDLE  10=看门狗模式  11=定时器模式
 *     INT[1:0]  = WDCTL[1:0]: 00=~1s  01=0.25s  10=15.6ms  11=1.9ms
 *     CLR[3:0]  = WDCTL[7:4]: 看门狗模式下写 0xA 后写 0x5 即喂狗
 *   ★ 看门狗模式一旦使能就无法关掉, 所以只能在退出-复位前一直喂。
 */
void NFC_WdtArm(void)
{
    WDCTL = 0x00u;          /* 先回到 IDLE, 顺带把 INT 定成 00 (~1s) */
    WDCTL = 0x08u;          /* MODE=10 -> 看门狗模式 */
}
void NFC_WdtFeed(void)
{
    WDCTL = (uint8)((WDCTL & 0x0Fu) | 0xA0u);   /* CLR = 0xA */
    WDCTL = (uint8)((WDCTL & 0x0Fu) | 0x50u);   /* CLR = 0x5 */
}

/* 等 T1 前进 t 个 tick (自动处理 16 位回绕)
 *
 * ★★ v44 重大修复: 这里原先是**零保护死等**
 *      do { e = read_t1(); } while ((uint16)(e - s) < t);
 *   只要 T1 因为任何原因不再计数(被别的外设关掉 / 时钟源没了 / 溢出标志异常),
 *   整段代码就永久卡在这个 do-while 里 —— 无任何外层救援。
 *   Round 45 的"step 9 卡死"就是这个:
 *     NFC_AddrScanAck 一轮 128 个地址, 每个地址 ~20 次位延时,
 *     它是全固件里唯一会密集调用位延时的地方, 所以概率上先在这里炸。
 *   现在加硬上限: 超时不再等, 直接返回并累计计数, 上层把计数画到屏上。
 */
uint16 NFC_WaitOvf(void)  { return g_waitOvf;  }
uint16 NFC_WaitLast(void) { return g_waitLast; }
void   NFC_WaitClr(void)  { g_waitOvf = 0u; g_waitLast = 0u; }
uint16 NFC_T1Count(void)  { return read_t1(); }   /* v44: 给复位面包屑用 */

#define T1_WAIT_GUARD  40000UL
static void t1_wait_ticks(uint16 t)
{
    uint16 s, e;
    uint32 guard;

    if (t == 0u) { return; }              /* 0 tick: 立刻返回 */
    if (g_waitOvf == 0xFFFFu) { g_waitOvf = 0u; }

    s = read_t1();
    guard = 0u;
    do {
        e = read_t1();
        guard++;
        if (guard > T1_WAIT_GUARD) {      /* ★ 上限: T1 不计数也不再死等 */
            if (g_waitOvf < 0xFFFFu) { g_waitOvf++; }
            g_waitLast = t;
            return;
        }
    } while ((uint16)(e - s) < t);
}

void NFC_T1Init(void)
{
    /* ★ v10: 原厂固件写 CLKCONCMD=0x80. ★★ v24 更正其含义(查 TI 手册 SWRU191F):
     *   0x80 = 0b1000_0000 => bit7 OSC32K=1 (选 32kHz **RCOSC**), bit6 OSC=0 (32MHz XOSC)。
     *   也就是说原厂**不是**要切 16MHz! 它是选 32MHz XOSC + 把 32kHz 源设成 RCOSC。
     *   之前以为"要切 16MHz RCOSC"、拿 CLKCONSTA.7 当 16M 标志, 全是误读。
     *
     * ★★ v22 现象(串口实测): 写 0x80 后轮询 `(CLKCONSTA & 0x80)` 会**死等** ——
     *   因为该位是 OSC32K 状态, 芯片若没有 32kHz RC 或该位行为不同, 就永远等不到 1。
     *   原厂轮询的其实也是这一位, 但原厂芯片上它能变 → 在**我们的板**上不行。
     *
     * ★★ v24 正确做法: **不要**再去动 32kHz 源(保持复位默认 OSC32K=1→按芯片实际),
     *   只做两件确定的事:
     *     ① 明确选 32MHz XOSC (清 bit6), 等 CLKCONSTA.6==0 变稳定;
     *     ② TICKSPD/CLKSPD 清 0 → T1 与 CPU 都 32MHz。
     *   T1 16 位 @32MHz → tick=31.25ns (tickNs≈31)。
     *   32kHz 源/睡眠定时器是否能走, 由 TimebaseCal 实测(有超时, 不再死等)。 */
    uint16 guard = 0u;
    uint8  sta;

    g_t1ClkCmd0 = CLKCONCMD;           /* 切换前原值 */

    /* ① 选 32MHz XOSC: 清 bit6(OSC), 等 CLKCONSTA.6 变 0 */
    CLKCONCMD &= (uint8)~0x40u;        /* bit6 OSC=0 -> 32MHz XOSC */
    while (guard < 60000u) {
        sta = CLKCONSTA;
        if ((sta & 0x40u) == 0u) { break; }   /* CLKCONSTA.6=0: 已切到 32MHz XOSC */
        guard++;
    }
    g_t1WaitLoops = guard;             /* 实际等了多少圈(0 = 一次就过) */
    g_t1ClkSta    = CLKCONSTA;         /* 切换后实测状态 */
    g_t1ClkCmd    = CLKCONCMD;

    /* ② TICKSPD / CLKSPD 清 0 -> 定时器与 CPU 均 32MHz */
    CLKCONCMD &= (uint8)~0x47u;
    g_t1ClkForced32 = 1u;              /* 本板确定跑 32MHz XOSC */

    T1CTL = 0x01u;                    /* MODE=free(01), DIV=/1(00) */
    {
        /* ★ v25i: 自检加固 —— 实测 t1ok=0 / t1ok=1 会随机出现(疑似 T1 起振竞态)。
         *   连测 3 次, 任一次看到计数前进就判 OK。 */
        uint8 tries = 0u;
        g_t1ok = 0u;
        while (tries < 3u) {
            uint16 a0 = read_t1();
            uint16 g  = 0u;
            while (g < 2000u) { if ((uint16)(read_t1() - a0) >= 50u) { break; } g++; }
            if ((uint16)(read_t1() - a0) != 0u) { g_t1ok = 1u; break; }
            tries++;
        }
    }
}

uint8 NFC_T1Ok(void) { return g_t1ok; }

/* ★ v22: T1Init 时钟诊断 getter */
uint8  NFC_GetT1ClkCmd0(void)   { return g_t1ClkCmd0; }
uint8  NFC_GetT1ClkSta(void)    { return g_t1ClkSta; }
uint8  NFC_GetT1ClkCmd(void)    { return g_t1ClkCmd; }
uint16 NFC_GetT1WaitLoops(void) { return g_t1WaitLoops; }

/* ===========================================================================
 * ★★★ v25: 32.768kHz 晶振 / P2.3-P2.4 冲突诊断 (决定性)
 * ===========================================================================
 * 背景: v24 串口实测 —— P2.3 与 P2.4 (TI 数据手册 SWRS081B Table 1: pin33 =
 *   P2_3/XOSC32K_Q2, pin32 = P2_4/XOSC32K_Q1, 都是 32.768kHz 晶振脚) 在
 *   **任何状态**下读回都是 0: 推挽驱高=0、内部上拉=0、三态=0。
 *   而 P2.0/P2.1 读回 1。说明这两脚被**外部强下拉/钳位**。
 *
 * 两种可能必须分开:
 *   (A) 板上真装了 32.768kHz 晶振 → 振荡器把这两脚钳在直流偏置上,
 *       且原厂把 VDDEX 使能**复用到晶振脚** → 硬件设计与固件打架。
 *   (B) 没晶振, P2.3(VDDEX) 这根网被硬拉到 GND(短路/芯片损坏/走线到地)。
 *
 * 判别手法(全在固件内, 不吃外部设备):
 *   1) 读 CLKCONSTA.7 (OSC32K 实际源): 0 = 32kHz 晶体在跑, 1 = 32kHz RCOSC。
 *   2) 把 CLKCONCMD.7 强制设为**晶体**(=0), 等一小会儿, 再看 CLKCONSTA.7:
 *        仍为 0  → 芯片认为晶体可用(有晶振的子证据之一);
 *        一直为 1 → 晶体没起振(强制不了)。
 *   3) ★ 杀手判据: 在 **P2SEL.1=1(晶振外设接管)** 与 **P2SEL.1=0(GPIO)** 两种
 *      模式下分别把 P2.3 推挽驱高并读回。
 *        两种情况读回都为 0 → 钳位与晶振无关, 是 (B) 硬拉到地;
 *        晶振模式读 0、GPIO 模式读 1 → 是 (A), 晶振在钳制。
 *   4) 顺带量一个 ~2ms 窗口里 ST 走了几拍(晶振起振时 ST 会动)。
 *
 * 返回: 打包字节
 *   bit0 = 起始 CLKCONSTA.7 (OSC32K 实际源: 0=晶体,1=RC)
 *   bit1 = 强制晶体后 CLKCONSTA.7 (0=晶体生效,1=强制不了)
 *   bit2 = GPIO 模式推挽驱高能否读回 1 (1=能)
 *   bit3 = 晶振模式(P2SEL.1=1)驱高能否读回 1 (1=能)
 *   bit4 = ~2ms 内 ST 是否走了(1=动)
 *   bit5 = P2.4 同 GPIO 模式能否读回 1 (对照: 是不是两个晶振脚都坏)
 * 另外 *stTicks 回填 ST 拍数。
 */
uint8 NFC_Probe32kXosc(uint16 *stTicks)
{
    uint8  saveSel, saveDir, saveInp, saveCmd;
    uint8  r0, rCry, rGpio, rPer, r24, rRc;
    uint32 s0, s1;
    uint16 spin;
    uint8  wSta0, wSta1;

    if (stTicks) { *stTicks = 0u; }

    saveSel = P2SEL; saveDir = P2DIR; saveInp = P2INP; saveCmd = CLKCONCMD;

    /* --- 1) 起始 OSC32K 实际源 --- */
    wSta0 = (uint8)((CLKCONSTA & 0x80u) ? 1u : 0u);
    r0    = (uint8)(wSta0 ? 1u : 0u);

    /* --- 2) 强制 OSC32K = RC(bit7=1 即 RCOSC) —— ★★★ 关键新实验
     *   ★ 原厂 T1Init 写 CLKCONCMD=0x80 = bit7 OSC32K=1(选 RCOSC 而非晶体)!
     *     若 P2.3 被 32kHz **晶体振荡电路**钳住, 那么把 32kHz 源切到 RCOSC
     *     (晶体振荡器关掉) 后, P2.3 应当**立刻可以被驱高读回高**。
     *     这正是"原厂能用 P2.3 当 VDDEX"的前提条件。 */
    CLKCONCMD |= 0x80u;                      /* bit7 OSC32K=1 -> 32kHz RCOSC */
    NFC_DelayMs(2u);
    P2SEL &= (uint8)~0x02u;                  /* P2.3 -> GPIO */
    P2INP |= 0x08u;
    P2DIR |= 0x08u; P2_3 = 1;
    NFC_DelayMs(2u);
    rRc = (uint8)((P2 & 0x08u) ? 1u : 0u);   /* ★ 切 RCOSC 后能否驱高 */

    /* --- 3) 再切回晶体(bit7=0), 看同样驱高能否读回 --- */
    CLKCONCMD &= (uint8)~0x80u;              /* bit7 OSC32K=0 -> 32kHz 晶体 */
    NFC_DelayMs(2u);
    wSta1 = (uint8)((CLKCONSTA & 0x80u) ? 1u : 0u);
    rCry  = (uint8)(wSta1 ? 1u : 0u);

    /* --- 3a) GPIO 模式: P2SEL.1=0, 推挽驱高读回 --- */
    P2SEL &= (uint8)~0x02u;                   /* P2.3 -> GPIO */
    P2INP |= 0x08u;                           /* 输入时三态(不外拉) */
    P2DIR |= 0x08u; P2_3 = 1;
    NFC_DelayMs(2u);
    rGpio = (uint8)((P2 & 0x08u) ? 1u : 0u);

    /* --- 3b) 晶振模式: P2SEL.1=1, 让 32kHz 振荡器接管 P2.3 --- */
    P2SEL |= 0x02u;                           /* P2.3 -> 32kHz XOSC 外设 */
    NFC_DelayMs(2u);
    /* 取引脚状态(此时 P2DIR.3 应已被外设接管; 直接读 P2 引脚) */
    rPer = (uint8)((P2 & 0x08u) ? 1u : 0u);

    /* --- 4) 2ms 窗口里 ST 走几拍 (晶振起振时 ST 会动) --- */
    s0 = st24();
    spin = 0u;
    while (spin < 30000u) { if ((st24() - s0) != 0u) { break; } spin++; }
    s1 = st24();
    if (stTicks) { *stTicks = (uint16)((s1 - s0) & 0xFFFFu); }
    r0 = (uint8)((r0 ? 1u : 0u)
               | (rCry ? 2u : 0u)
               | (rGpio ? 4u : 0u)
               | (rPer ? 8u : 0u)
               | (((s1 - s0) != 0u) ? 16u : 0u)
               | (rRc ? 64u : 0u));           /* ★ bit6 = 切 RCOSC 后 P2.3 能否驱高 */

    /* --- 3c) 对照组: P2.4 同样 GPIO 推挽驱高 --- */
    P2SEL &= (uint8)~0x04u;                   /* P2.4 -> GPIO */
    P2INP |= 0x10u;
    P2DIR |= 0x10u; P2_4 = 1;
    NFC_DelayMs(2u);
    r24 = (uint8)((P2 & 0x10u) ? 1u : 0u);
    r0 |= (uint8)(r24 ? 32u : 0u);

    /* --- 还原 --- */
    P2SEL = saveSel; P2DIR = saveDir; P2INP = saveInp; CLKCONCMD = saveCmd;
    return r0;
}

/* ===========================================================================
 *   ★★★ v27: 倒灌源定位 (决定性, 纯 MCU 侧, 不改硬件)
 *
 *   背景: P2.3(VDDEX) 驱高只有 0.6V, 但 SDA/SCL 空闲时又是高(bus=0x03)。
 *   假说: P2.3 的 0.6V 不是"P2.3 驱不动负载", 而是 SDA/SCL 上的电经
 *         芯片内部 ESD 二极管**倒灌**进 VDDEX 网络(再经走线回到 P2.3)。
 *
 *   做法: P2.3 全程驱低(VDDEX=OFF), 逐个隔离 SDA/SCL, 看还剩哪根线有电;
 *         并用"拉低一根看另一根是否跟着掉"判断两线是否有耦合通道。
 *   P2.3 拉低时 SDA/SCL 的残留电压来源只有三种:
 *     ① 外部上拉接在别的 3.3V 轨(不是 VDDEX)  -> tb 和 tc 都会有电
 *     ② MCU 内部上拉(本例已关)                -> 不会
 *     ③ 芯片 VDDEX 网络经 ESD 被 SDA/SCL 倒灌  -> 拉低一根, 另一根仍高
 * =========================================================================== */
uint8 NFC_DiagBackfeed(uint16 *stTicks)
{
    uint8 saveInp, saveDir, saveSel, r;
    uint16 s0, spin = 0u;
    uint8 ta = 0u, tb = 0u, tc = 0u, td = 0u, te = 0u, tf = 0u;
    uint8 tg = 0u; uint16 th = 0u;

    /* ★ 纯软件忙等: 完全不依赖 T1/ST(g_msTicks 可能=0, NFC_DelayMs 会极慢)。
     *   与 main() 里 [VDD] 窗口同一形状: 260000 圈(uint32 volatile) ≈ 1s @32MHz。
     *   BF_WAIT(n) 的 n = 毫秒数。 */
    #define BF_WAIT(n) do { uint8 _k = (n); \
                            while (_k != 0u) { volatile uint32 _w = 0u; \
                                while (_w < 260UL) { _w++; } _k--; } } while (0)

    saveInp = P2INP; saveDir = P2DIR; saveSel = P2SEL;

    /* --- P2.3 固定为: GPIO 推挽输出, 驱低 (VDDEX = OFF) --- */
    P2SEL &= (uint8)~0x02u;          /* P2SEL.1 = P2.3 功能位 -> GPIO */
    P2INP |= 0x08u;                  /* P2INP.3 = 1 -> 输入时三态(不内拉) */
    P2DIR |= 0x08u;
    P2_3 = 0;
    BF_WAIT(2u);

    /* P0.0/P0.1 一律: 关内部上拉(靠外部), 关中断, 转 GPIO */
    P0SEL &= (uint8)~0x03u;
    P0IEN &= (uint8)~0x03u;
    P0INP |= 0x03u;                  /* 三态: 不内拉 */
    P2INP &= (uint8)~0x20u;          /* PDUP0 = 0 (但 P0INP 三态优先) */

    /* --- a) 两根线全放开 -> 读 SDA/SCL --- */
    P0DIR &= (uint8)~0x03u;          /* 都转输入 */
    P0_0 = 1; P0_1 = 1;
    BF_WAIT(2u);
    ta = (uint8)(P0 & 0x03u);

    /* --- b) SCL 驱低, 只放开 SDA -> 读 SDA(bit0) --- */
    P0_1 = 0; P0DIR |= 0x02u;        /* SCL 推挽低 */
    P0DIR &= (uint8)~0x01u;          /* SDA 输入 */
    P0_0 = 1;
    BF_WAIT(2u);
    tb = (uint8)(P0 & 0x01u);

    /* --- c) SDA 驱低, 只放开 SCL -> 读 SCL(bit1) --- */
    P0_0 = 0; P0DIR |= 0x01u;        /* SDA 推挽低 */
    P0DIR &= (uint8)~0x02u;          /* SCL 输入 */
    P0_1 = 1;
    BF_WAIT(2u);
    tc = (uint8)((P0 & 0x02u) ? 1u : 0u);

    /* --- d) SDA 驱低 200ms, SCL 输入 -> SCL 是否被拽低(耦合) --- */
    P0_0 = 0; P0DIR |= 0x01u;        /* SDA 保持低 */
    P0DIR &= (uint8)~0x02u;          /* SCL 输入 */
    P0_1 = 1;
    BF_WAIT(200u);
    td = (uint8)((P0 & 0x02u) ? 1u : 0u);   /* 若=0 说明 SCL 被 SDA 拽下来了 */

    /* --- e) SCL 驱低 200ms, SDA 输入 -> SDA 是否被拽低 --- */
    P0_1 = 0; P0DIR |= 0x02u;        /* SCL 保持低 */
    P0DIR &= (uint8)~0x01u;          /* SDA 输入 */
    P0_0 = 1;
    BF_WAIT(200u);
    te = (uint8)(P0 & 0x01u);

    /* --- f) P2.3 驱高(VDDEX ON), 两根线全放开 -> 读 SDA/SCL ---
     *   ★ v27d: 关键 —— 给足充电时间。前面 d/e 把两线驱低过, VDDEX 刚抬起时
     *   外部上拉(VDDEX 轨)要经线电容充电, 需要时间。等 50ms 再读。 */
    P2SEL &= (uint8)~0x02u;
    P2DIR |= 0x08u;
    P2_3 = 1;
    P0DIR &= (uint8)~0x03u;
    P0_0 = 1; P0_1 = 1;
    BF_WAIT(50u);                          /* ★ 50ms 充分稳定 */
    tf = (uint8)(P0 & 0x03u);

    /* --- g) ★★ 核心: P2.3 驱高稳定后, 直接读回 P2.3 自身电平 ---
     *   推挽驱高时读回应=1。若=0 或 0.6V, 说明有外部件在吸流。 */
    P2DIR |= 0x08u;
    P2_3 = 1;
    BF_WAIT(100u);                         /* 100ms 充分 */
    tg = (uint8)((P2 & 0x08u) ? 1u : 0u);  /* ★ P2.3 驱高读回 */

    /* --- h) 放开 P2.3, 看它靠外部(电容+上拉)能撑几 ms --- */
    P2INP |= 0x08u;                        /* 三态 */
    P2DIR &= (uint8)~0x08u;                /* 放开 */
    th = 0u;
    while (th < 250u) {
        if ((P2 & 0x08u) == 0u) { break; }
        BF_WAIT(1u);
        th++;
    }
    P2DIR |= 0x08u; P2_3 = 1;

    /* --- ST 是否在走(顺带确认时钟) --- */
    s0 = st24(); spin = 0u;
    while (spin < 30000u) { if ((st24() - s0) != 0u) { break; } spin++; }
    if (stTicks != 0) { *stTicks = (uint16)(st24() - s0); }

    r = (uint8)((ta & 0x03u)
              | (uint8)(tb ? 0x04u : 0u)
              | (uint8)(tc ? 0x08u : 0u)
              | (uint8)(td ? 0x10u : 0u)
              | (uint8)(te ? 0x20u : 0u)
              | (uint8)((tf & 0x03u) << 6));

    g_bfP23 = tg;                          /* ★ 全局导出: P2.3 驱高读回 */
    g_bfHold = th;                         /* ★ 全局导出: 放开后撑住 ms */

    /* --- 还原 --- */
    P2INP = saveInp; P2DIR = saveDir; P2SEL = saveSel;
    P0DIR &= (uint8)~0x03u;
    return r;
}

void NFC_TimebaseCal(void)
{
    uint16 tn0, tn1;
    uint32 st0, st1, dt_st;
    uint16 dt_t1;
    uint32 s0;
    uint16 spin = 0u;

    /* 测量一段 ~1ms 的区间, 同时用 T1 和 ST 量(两把尺子互校)。
     * 1ms < T1 回绕周期(2.048ms), 不会跨边界。
     *
     * ★★ v22 关键修正 (串口实测): 这里原来是 `while((st24()-s0)<33u){}` 无出口死等!
     *   实测: 本板 CLKCONSTA.7 恒为 0(切不到 16MHz RCOSC), 32.768kHz XOSC 可能未起振
     *         → 睡眠定时器 ST 不计数 → 死等。串口证据: 打完 "[T1] ..." 后再无输出。
     *   对策: 加**硬超时**(spin 上限); 超时则判 ST 不可用, 退回"按 32MHz 估 tickNs=31"。
     *         并记录 g_calSpin 供串口打印。 */
    tn0 = read_t1();
    st0 = st24();
    s0  = st24();
    /* ★ v24: 只等 ST 走 30 拍(≈0.92ms)。若 12000 圈内还没到 30, 判定 ST 不可信,
     *   立刻退出(不再空转到 40000)。这样既不浪费毫秒, 也不会采信坏 ST 数据。 */
    while (spin < 12000u) {
        if ((st24() - s0) >= 30u) { break; }
        spin++;
    }
    g_calSpin = spin;                       /* 12000 = 到顶(ST 没走够 30 拍) */
    tn1 = read_t1(); st1 = st24();

    dt_st = (st1 > st0) ? (st1 - st0) : 0u;
    dt_t1 = (uint16)(tn1 - tn0);

    g_calT1 = dt_t1;
    g_calSt = (uint16)dt_st;

    /* ST 一拍 = 30517.578 ns。tickNs = dt_st * 30517.578 / dt_t1
     *
     * ★★ v24 关键修正 (串口实测): 本板 ST **能走但极慢/极不稳** —— 实测
     *   calSt=1(只走了 1 拍!)、dt_t1≈1.5~3 万 → 算出 tickNs=1(荒谬)。
     *   一旦采信这个值, 所有延时会被放大 31 倍, 固件直接跑飞/卡死。
     *   所以判据收紧: **只有当 ST 这次至少走了 30 拍**(量化误差 <3%) 才采信 ST 校准;
     *   否则若已确认 32MHz(forced32) 就直接用 **31 ns**(32MHz 的 31.25ns 取整)。
     *   这样既保留 ST 正常时的自校准能力, 又绝不会被"ST 半死不活"坑到。 */
    if (dt_t1 != 0u && dt_st >= 30u) {
        uint32 ns_loop = (dt_st * 305176UL) / 10UL;   /* 区间总长(ns) */
        uint32 tns     = ns_loop / (uint32)dt_t1;      /* 每 T1 tick 的 ns */
        if (tns > 5u && tns < 200u) {                  /* 合理窗口: 5~200ns */
            g_tickNs = (uint16)tns;
            g_calOk  = 1u;
        } else {
            g_tickNs = 31u; g_calOk = 0u;
        }
    } else if (g_t1ClkForced32 != 0u && g_t1ok != 0u) {
        /* ★★ ST 不可信/不可用, 但已确认 T1 跑在 32MHz XOSC。
         *   32MHz → 1 tick = 31.25ns → tickNs = 31(取整)。这是**有依据的确定值**,
         *   不是"兜底估测"。旧代码恰好也落到 31, 所以之前 I2C 时序一直是对的。 */
        g_tickNs = 31u;
        g_calOk  = 1u;
    } else {
        g_tickNs = 31u; g_calOk = 0u;
    }

    /* 由 tickNs 反推各档位等待的 T1 tick 数 */
    g_us4  = (uint16)(4000u  / g_tickNs);
    g_us10 = (uint16)(10000u / g_tickNs);
    g_us20 = (uint16)(20000u / g_tickNs);
    g_bitTicks = (uint16)(g_us10 + g_us20 + g_us10);
    /* ★ v11: 原厂 I2C 位时序延时 (0x3C/0x78 单位 @32MHz→~22/45us, @16MHz 减半) */
    g_i2c_3c  = (uint16)(22000u  / g_tickNs);   /* ~22us */
    g_i2c_78  = (uint16)(45000u  / g_tickNs);   /* ~45us */
    g_i2c_100 = (uint16)(100000u / g_tickNs);   /* ~100us (保留兼容) */

    /* ★★ v21: 1ms 对应的 T1 tick 数, 供 NFC_DelayMs 使用。
     *   g_tickNs = 每个 T1 tick 的 ns。1ms = 1e6 ns → ticks = 1e6 / g_tickNs。
     *   若算出 0 或 >65535(不可能容纳 1ms), 视为 T1 基准不可用 → 保持 0,
     *   NFC_DelayMs 会自动退回带硬上限的 ST 路径。 */
    {
        uint32 t = 1000000UL / (uint32)g_tickNs;   /* 1ms 需要多少 T1 tick */
        if (t >= 1u && t <= 65535UL) { g_msTicks = (uint16)t; }
        else                         { g_msTicks = 0u; }
    }
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

/* ===========================================================================
 * ★ v16: I2C 位周期可调 —— 手册硬约束 "SCL 20 kHz ~ 100 kHz"
 * ===========================================================================
 * 原实现把 dly_i2c_s/l 固定成 22us/45us, 于是 SCL 周期 = 22+45 = 67us
 * → 14.9kHz, **低于手册 20kHz 下限**。这里允许把整周期设成任意 us 值,
 * 用于定点扫描"SDA/SCL 电平都对、却整条总线 NOACK"到底是
 * ① 从来没有器件(接线/未进串行模式) 还是 ② 时钟跑在规格外。
 *   NFC_SetBitUs(us) : us = 一个完整 SCL 周期的微秒数(高/低各半)
 *   NFC_GetBitPerUs(): 当前生效的整周期实测值
 *   NFC_BitSave/Restore : 扫描前后保存/还原原厂时序 */
static uint16 g_save_3c = 0u;
static uint16 g_save_78 = 0u;

void NFC_BitSave(void)
{
    g_save_3c = g_i2c_3c;
    g_save_78 = g_i2c_78;
}

void NFC_BitRestore(void)
{
    if (g_save_3c != 0u) { g_i2c_3c = g_save_3c; }
    if (g_save_78 != 0u) { g_i2c_78 = g_save_78; }
}

void NFC_SetBitUs(uint16 us)
{
    uint16 half;
    if (us < 6u) { us = 6u; }             /* 下限: 6us → 166kHz(远快过规格, 仅供对照) */
    half = (uint16)(us >> 1);
    if (half == 0u) { half = 3u; }
    g_i2c_3c = (uint16)(((uint32)half * 1000UL) / (uint32)g_tickNs);
    g_i2c_78 = g_i2c_3c;
    if (g_i2c_3c == 0u) { g_i2c_3c = 1u; g_i2c_78 = 1u; }
}

uint16 NFC_GetBitPerUs(void)
{
    uint32 t = ((uint32)g_i2c_3c + (uint32)g_i2c_78) * (uint32)g_tickNs;
    return (uint16)(t / 1000UL);
}

/* 微秒级延时(由 T1 决定) */
/* v11: 延时分三档
 *   dly_s/dly_l = 通用 10/20us (诊断/测量/初始化用)
 *   dly_i2c_s   = 原厂 I2C 短延时 (0x3C, ~22us@32MHz / ~11us@16MHz)
 *   dly_i2c_l   = 原厂 I2C 长延时 (0x78, ~45us@32MHz / ~22us@16MHz)
 *   dly_i2c     = 100us (保留兼容, 旧诊断用)
 * 位层函数(i2c_tx8/i2c_rx8/start/stop/read_ack/putack)全用 dly_i2c_s/l;
 * 诊断函数(clock9_kind/PinDrive/ScopeTx/上升下降测量)继续用 dly_s/dly_l。
 */
static void dly_s(void) { t1_wait_ticks(g_us10); }   /* ~10us 通用短(诊断用) */
static void dly_l(void) { t1_wait_ticks(g_us20); }   /* ~20us 通用长(诊断用) */
/* ★ v11: 原厂 I2C 位时序延时 (0x3C/0x78) */
static void dly_i2c_s(void) { t1_wait_ticks(g_i2c_3c); }  /* 原厂 0x3C */
static void dly_i2c_l(void) { t1_wait_ticks(g_i2c_78); }  /* 原厂 0x78 */
static void dly_i2c(void)   { t1_wait_ticks(g_i2c_100); } /* ~100us (保留兼容) */

/* ★★ v21: NFC_DelayMs 改为「T1 为主 + ST 兜底 + 硬上限」 —— 绝不再死等 ★★
 *
 *  背景(Round 29): v20 固件实测卡在 autotune 的 NFC_DelayMs(30u) 不返回,
 *  下游 NFC_PowerUp/Probe/ReadMem/EPD_Init 全都到不了。
 *  旧实现 `while((st24()-s)<33u)` 用 24 位睡眠定时器(ST0/ST1/ST2)做 1ms 基准;
 *  一旦 ST 不按 32.768kHz 走(16MHz RCOSC 下 SLEEPCMD=0x04 已关 32k XOSC),
 *  这个循环就是**无限死等** —— 没有任何出口。
 *
 *  v21 策略:
 *    1) 主基准用 T1(实测 T1CTL=0x01、T1CNT 正常计数, 最可靠)。
 *       T1 16 位 @ CPU 时钟(DIV=/1) → 回绕 4.096ms @16MHz; 用 16 位差值(T1cnt 自动回绕)。
 *       1ms 对应 g_msTicks 个 T1 tick (由 TimebaseCal 给出; 缺省按 16MHz 估 16000)。
 *    2) 若 T1 没在跑(T1CTL==0 或 g_msTicks==0), 退回 ST, 但**带硬上限**:
 *       ST 每 1ms 期望 33 tick; 若 ST 连续 3 倍时间(即 99 tick)都没动, 直接放弃该 ms
 *       (宁可少延时也不允许死等)。
 *    3) 最外层再加一个总计数上限, 双保险。
 */
void NFC_DelayMs(uint16 ms)
{
    uint16 msT;
    uint8  useT1;
    uint8  useST;

    /* 决定 1ms 需要多少 T1 tick */
    msT   = g_msTicks;                 /* v21: TimebaseCal 算出, 缺省 0 */
    useT1 = (uint8)((msT != 0u) ? 1u : 0u);
    /* ★★★ v29: ST 只有在 TimebaseCal 确认它真的按 32.768kHz 走时才算可用。
     *   v21 的 ST 路径用 "spin >= 9900 就放弃该 ms" —— 但 9900 圈在外层
     *   `while (ms != 0)` 里是**每个 ms 都空转 9900 圈**, 即 1ms 的延时实际
     *   要花 ~9900 圈 CPU 时间。`NFC_DelayMs(30)` 就是 297000 圈 ≈ 1 秒,
     *   autotune 里 16 轮 × (30+50)ms 会变成几十秒 → 看起来像"卡死"。
     *   这正是 t1ok=0/calOk=0 时整条流程跑不完的直接原因。
     *   对策: ST 不可信时**不再空转**, 直接走固定软件忙等兜底(见下)。 */
    useST = g_calOk;                   /* 1 = ST 校准过, 可当基准 */

    while (ms != 0u) {
        if (useT1) {
            /* ---- T1 基准: 16 位差值自动处理回绕 ---- */
            uint16 t0 = read_t1();
            uint16 guard = 0u;
            while ((uint16)(read_t1() - t0) < msT) {
                if (++guard == 0u) { break; }   /* 极端兜底: 转满一圈就跳出 */
            }
        } else if (useST) {
            /* ---- ST 基准(已确认可信): 33 tick ≈ 1ms ---- */
            uint32 s    = st24();
            uint16 spin = 0u;
            while ((st24() - s) < 33u) {
                if (++spin >= 9900u) { break; } /* 保险丝: 万一 ST 又停了 */
            }
        } else {
            /* ★★★ v29 固定软件忙等兜底 —— 完全不依赖 T1/ST ★★★
             *   与 main.c 的 [VDD] 窗口同形状: 内层 260 圈 ≈ 1ms @32MHz。
             *   这样即使时钟/定时器全乱, 上电/探测流程也能以正常速度跑完。 */
            volatile uint32 w = 0u;
            while (w < 260UL) { w++; }
        }
        ms--;
    }
}

/* ===========================================================================
 *                       位层 (SDA/SCL 全部推挽 —— 严格复刻原厂 0xF5B9)
 * ===========================================================================
 * 原厂 0xF5B9: P0SEL&=~0x03, P0DIR|=0x03 → P0.0/P0.1 均推挽输出
 *   SDA 低 = P0.0=0, P0DIR|=0x01 (推挽驱动低)
 *   SDA 高 = P0.0=1, P0DIR|=0x01 (推挽驱动高, 非开漏释放)
 *   SCL 同理
 * 原厂读 SDA 时(0xF5C6): P0SEL&=~01, P0DIR&=~01, P0INP&=~01, P2INP&=~0x20
 *   → 切输入+内部上拉, 读完再切回推挽(0xF5D5)
 */
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
/* v10: SDA 高 = 推挽驱动高(原厂 0xF5D5: P0DIR|=0x01, SETB P0.0), 非开漏释放 */
static void sda_hi(void)
{
    if (swap01 == 0u) { P0_0 = 1; P0DIR |= 0x01u; }
    else              { P0_1 = 1; P0DIR |= 0x02u; }
}
/* SCL 高: 推挽(原厂与 SDA 同, 均推挽) */
static void scl_hi(void)
{
    if (swap01 == 0u) { P0_1 = 1; P0DIR |= 0x02u; }
    else              { P0_0 = 1; P0DIR |= 0x01u; }
}
/* SDA 切输入+内部上拉(原厂 0xF5C6) —— 读 SDA 前调用 */
static void sda_input(void)
{
    if (swap01 == 0u) {
        P0SEL &= (uint8)~0x01u;  P0DIR &= (uint8)~0x01u;
        P0INP &= (uint8)~0x01u;  P2INP &= (uint8)~0x20u;
    } else {
        P0SEL &= (uint8)~0x02u;  P0DIR &= (uint8)~0x02u;
        P0INP &= (uint8)~0x02u;  P2INP &= (uint8)~0x20u;
    }
}
/* SDA 切回推挽输出(原厂 0xF5D5) —— 读完/NAK 用 */
static void sda_output(void)
{
    if (swap01 == 0u) { P0SEL &= (uint8)~0x01u;  P0DIR |= 0x01u; }
    else              { P0SEL &= (uint8)~0x02u;  P0DIR |= 0x02u; }
}
/* SCL 释放(转输入) —— 仅诊断用, 正常时序 SCL 始终推挽 */
static void scl_rel(void)
{
    if (swap01 == 0u) { P0_1 = 1; P0DIR &= (uint8)~0x02u; }
    else              { P0_0 = 1; P0DIR &= (uint8)~0x01u; }
}
/* SDA 释放(转输入) —— 仅诊断用, 正常时序 SDA 推挽 */
static void sda_rel(void)
{
    if (swap01 == 0u) { P0_0 = 1; P0DIR &= (uint8)~0x01u; }
    else              { P0_1 = 1; P0DIR &= (uint8)~0x02u; }
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
    /* ★ v10: 原厂 0xF5B9 —— P0.0/P0.1 推挽输出(非开漏+上拉) */
    P0SEL &= (uint8)~0x03u;   /* P0.0/P0.1 -> GPIO */
    P0DIR |= 0x03u;           /* ★ 推挽输出(原厂 0xF5B9) */
    if (in_pu_off != 0u) { P0INP |= 0x03u; }          /* 三态(诊断用) */
    else                 { P0INP &= (uint8)~0x03u; }  /* 使能内部上拉(诊断用) */
    P2INP &= (uint8)~0x20u;   /* PDUP0 = 0 -> Port0 上拉(诊断用) */
    P0IEN &= (uint8)~0x03u;   /* 关 P0 引脚中断: bit-bang 会疯狂触发 */
    P1SEL &= (uint8)~0x01u;   /* P1.0(NIRQ) -> GPIO */
    P1DIR &= (uint8)~0x01u;
    P1INP &= (uint8)~0x01u;
    P1IEN &= (uint8)~0x01u;
    P2INP &= (uint8)~0x40u;   /* PDUP1 = 0 -> Port1 上拉 (PDUP1 是 bit6!) */
    P2SEL &= (uint8)~0x02u;   /* ★ v24: P2SEL.1 = P2.3 功能选择(0=GPIO);
                               *   旧代码错清 bit3(那是 P1 外设优先级), P2.3 从没真正设成 GPIO!
                               *   原厂 0x7D60: ANL P2SEL,#0xFD 正是清 bit1。 */
    P2DIR |= 0x08u;           /* P2.3 = VDDEX, 输出 */
    /* 原厂初始化末态: SDA/SCL 推挽高 */
    P0_0 = 1; P0_1 = 1;
}

void NFC_PowerUp(uint8 bootMs)
{
    /* ★★ v10: 严格按原厂 0xF586 复刻 VDDEX 复位脉冲 ★★
     *   原厂: SETB P2.3 → delay(5) → CLR P2.3 → delay(5) → SETB P2.3 → delay(5) → I2C
     *   delay(5) @16MHz = 5.0ms (原厂 0x163E6: 5圈×delay(1000)≈5.0ms)
     * ★ v17: 脉冲 + "先碰线还是先上电" 改为可配(pwr_pulse / pwr_early),
     *        用于排查芯片是否因为上电顺序不对而没打开 I2C 接口。 */
    P2SEL &= (uint8)~0x02u;               /* ★ v24: P2SEL.1=P2.3 功能位(0=GPIO) */
    P2DIR |= 0x08u;                       /* P2.3 = 输出 */

    if (pwr_early != 0u) {
        /* v8-v16 老行为: 先把两条线推挽驱动高, 再上 VDDEX */
        P0SEL &= (uint8)~0x03u;
        P0_0 = 1; P0_1 = 1;
        P0DIR |= 0x03u;                    /* ★ 推挽输出(原厂 0xF5B9) */
        dly_l();
    } else {
        /* v17 新行为: VDDEX 未稳前**不碰**两条线(输入+三态),
         * 让芯片先靠自己的 VDDEX 干净地上电复位, 之后再接管总线 */
        P0SEL &= (uint8)~0x03u;
        P0INP |= 0x03u;                    /* 三态 */
        P0DIR &= (uint8)~0x03u;            /* 输入 */
        P0_0 = 1; P0_1 = 1;
        dly_l();
    }

    if (pwr_pulse != 0u) {
        /* ★ VDDEX 复位脉冲（原厂 0xF586）★ */
        P2_3 = 1;                         /* VDDEX 先高 */
        NFC_DelayMs(5u);                  /* delay(5) = 5ms @16MHz */
        P2_3 = 0;                         /* VDDEX 拉低（复位脉冲）*/
        NFC_DelayMs(5u);                  /* delay(5) = 5ms @16MHz */
        P2_3 = 1;                         /* VDDEX 重新拉高 */
    } else {
        P2_3 = 1;                         /* 只做 低→高, 与原厂 mode1 一致 */
    }
    NFC_DelayMs(bootMs);                  /* tBoot: 等芯片稳定 */

    in_pu_off = (ord_fac != 0u) ? 0u : 1u;
    gpio_prep();                          /* VDDEX 已稳, 现在才碰两条线 */
    sda_hi(); scl_hi();                   /* 推挽高(原厂末态) */
    dly_i2c_s();                          /* delay(3C) */
}

void NFC_PowerOff(void){
    P2_3 = 0;
    NFC_DelayMs(2u);
    P0SEL &= (uint8)~0x03u;
    P0DIR &= (uint8)~0x03u;               /* 转输入 */
    P0INP |= 0x03u;                       /* 三态: 不给断电的芯片经 ESD 二极管灌电 */
    P2INP &= (uint8)~0x20u;
    P0IEN &= (uint8)~0x03u;
    P1IEN &= (uint8)~0x01u;
    P0_0 = 1;
    P0_1 = 1;
}

/* ★ v29: 把内部 gpio_prep 暴露给 main.c 的诊断段(验证 "idle bus=0x03 是
 *   外部上拉还是 MCU 推挽自造")。语义与 gpio_prep 完全一致。 */
void NFC_GpioPrepPub(void) { gpio_prep(); }

uint8 NFC_NirqLevel(void)
{
    P1SEL &= (uint8)~0x01u;
    P1DIR &= (uint8)~0x01u;
    dly_l();
    return (uint8)((P1 & 0x01u) ? 1u : 0u);
}

uint8 NFC_VddAlive(void)
{
    /* ★★ v24 重要更正: 用户实测**万用表**确认 P2.3↔VDDEX 连通、可到 3.2V。
     *   所以 v23 报的 `vddHold=0` 是**测量假象**, 不是硬件故障。假象来源:
     *     ① P2SEL.1 没清 → P2.3 可能没真正当 GPIO(已在上游修);
     *     ② 更关键: P2DIR.3 切输入后, **P2INP.3 决定该脚是上拉/下拉/三态**:
     *        P2INP.3=0 → 上/下拉模式, 再由 P2INP.5(PDUP2) 选 0=上拉 / 1=下拉。
     *        若落在"下拉", 放开瞬间就被内部**拉到 0** → 读 0 → 误判"掉电"。
     *   对策: 测之前把 P2INP.3 置 **1 = 三态**, 让节点只由外部(VDDEX 电容+芯片)决定。 */
    uint8 v;

    P2SEL &= (uint8)~0x02u;           /* P2SEL.1=P2.3 功能位(0=GPIO) */
    P2INP |= 0x08u;                   /* ★ P2INP.3=1 -> P2.3 输入时三态(不内拉) */
    P2DIR |= 0x08u;
    P2_3 = 1;
    NFC_DelayMs(1u);                  /* 让 VDDEX 节点稳定 */
    v = (uint8)((P2 & 0x08u) ? 1u : 0u);
    P2DIR |= 0x08u;
    P2_3 = 1;
    return v;                         /* 1 = 驱高读得到高 -> 供电通路存在 */
}

/* ★ v24: 把 P2.3"放开后保持时长"单独返回(ms) —— 三态测量, 不内拉 */
uint16 NFC_VddHoldMs2(void)
{
    uint16 ms = 0u;
    P2SEL &= (uint8)~0x02u;
    P2INP |= 0x08u;                   /* 三态: 不内拉, 只看外部 */
    P2DIR |= 0x08u;
    P2_3 = 1;
    NFC_DelayMs(50u);                 /* 充满 VDDEX 电容 */
    P2DIR &= (uint8)~0x08u;           /* 放开 */
    while (ms < 500u) {
        if ((P2 & 0x08u) == 0u) { break; }
        NFC_DelayMs(1u);
        ms++;
    }
    P2DIR |= 0x08u;
    P2_3 = 1;
    return ms;
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

/* ★ v17: 实测 SCL 整周期(us) —— 用 32.768kHz 睡眠定时器量, 不再"设了多少就信多少"。
 *   翻转 100 个 SCL 周期, 用的是 i2c_tx8 同一组延时(dly_i2c_l / dly_i2c_s)。
 *   手册硬约束: I2C 只在 20kHz~100kHz 可用 => 整周期必须落在 10us~50us。
 *   返回整数 us; 0 = 睡眠定时器没走(校准失败)。 */
uint16 NFC_MeasSclUs(void)
{
    uint32 s0, s1, ticks;
    uint16 i;

    P0SEL &= (uint8)~0x03u;
    P0DIR |= 0x03u;
    s0 = st24();
    for (i = 0u; i < 100u; i++) {
        scl_hi(); dly_i2c_l();
        scl_low(); dly_i2c_s();
    }
    s1 = st24();
    if (s1 <= s0) { return 0u; }

    ticks = s1 - s0;                              /* 100 个周期 */
    /* 1 ST tick = 30.5176us  ->  period_us = ticks * 30.5176 / 100 */
    return (uint16)(((ticks * 30518UL) / 1000UL) / 100UL);
}

/* ------------------------------------------------------------ primitives -- */
/* v14 FIX: i2c_start / i2c_stop bodies were swapped relative to their names
 * (and the comment labels were also reversed). The bus therefore emitted
 * STOP(0x7BCC) then START(0x7BEE); the MN63Y1213 slave never received a
 * valid START, so every transfer returned NOACK.
 * Now copied EXACTLY from the factory disassembly (timing verbatim, no
 * standard-I2C 'improvement'):
 *   0x7BEE = START: ORL P0DIR,#01 -> SCL hi -> SDA hi -> dly(78) -> SDA lo -> dly(3C) -> SCL lo -> dly(3C)
 *   0x7BCC = STOP : ORL P0DIR,#03 -> SDA lo -> dly(3C) -> SCL hi -> SDA lo -> dly(3C) -> SDA hi -> dly(78)
 */
static void i2c_start(void)
{
    /* factory 0x7BEE: START condition (SDA 1->0 while SCL high) */
    P0DIR |= 0x01u;           /* 原厂 0x7BEE: ORL P0DIR,#01 仅 SDA 推挽 */
    scl_hi();                 /* SCL 高 */
    sda_hi();                 /* SDA 高 */
    dly_i2c_l();              /* delay(78) */
    sda_low();                /* SDA 拉低 = START 信号(SCL 仍高) */
    dly_i2c_s();              /* delay(3C) */
    scl_low();                /* SCL 低 */
    dly_i2c_s();              /* delay(3C) */
}

/* v14: factory 0x7BCC STOP, copied verbatim */
static void i2c_stop(void)
{
    /* factory 0x7BCC: STOP condition (SDA 0->1 while SCL high) */
    P0DIR |= 0x03u;           /* 原厂 0x7BCC: ORL P0DIR,#03 推挽 */
    sda_low();                /* SDA 拉低 */
    dly_i2c_s();              /* delay(3C) */
    scl_hi();                 /* SCL 拉高 */
    sda_low();                /* SDA 再拉低 */
    dly_i2c_s();              /* delay(3C) */
    sda_hi();                 /* SDA 拉高 = STOP 信号(SCL 仍高) */
    dly_i2c_l();              /* delay(78) */
}

/* ★ v12: 严格按原厂 0x7B8F 复刻写位
 *   原厂序列(每位): SDA设值 → delay(3C) → SCL hi → delay(78) → SCL lo → delay(3C)
 *   MSB 先行, 出口 SCL 低
 *   ★ 开头 ORL P0DIR,#03 确保推挽(原厂 write_byte 0x7B96) */
static void i2c_tx8(uint8 b)
{
    uint8 i;
    P0DIR |= 0x03u;           /* ★ 原厂 0x7B96: ORL P0DIR,#03 确保推挽 */
    for (i = 0; i < 8u; i++) {
        if (b & 0x80u) { sda_hi(); }   /* 推挽高 */
        else           { sda_low(); } /* 推挽低 */
        dly_i2c_s();            /* delay(3C) */
        scl_hi();               /* SCL 高 */
        dly_i2c_l();            /* delay(78) */
        scl_low();              /* SCL 低 */
        dly_i2c_s();            /* delay(3C) */
        b = (uint8)(b << 1);
    }
}

/* ★ v12: 严格按原厂 read_cmd 0x7C3E 循环体复刻读位
 *   原厂序列(每字节): 先调 0x7CBD(sda_input) → 8 位循环 → 存字节
 *   每位: SCL hi → delay(3C) → 读 SDA → (非末位则左移) → SCL lo → delay(78)
 *   ★ sda_input 在循环外只调一次(原厂 0x7C44), 8 位期间 SDA 保持输入
 *   ★ 末尾不切回推挽(原厂由下一帧 inter-byte ACK 脉冲 ORL P0DIR,#03 切回) */
static uint8 i2c_rx8(void)
{
    uint8 i, b = 0;
    sda_input();               /* ★ 原厂 0x7C44: LCALL 0x7CBD (ANL P0DIR,#FE) */
    dly_i2c_s();               /* delay(3C) */
    for (i = 0; i < 8u; i++) {
        scl_hi();               /* SCL 高(原厂 0x7C4A) */
        dly_i2c_s();            /* delay(3C) */
        b = (uint8)(b << 1);
        if (sda_rd()) { b |= 0x01u; }  /* ★ SCL 高时采样(原厂 0x7C53) */
        scl_low();              /* SCL 低(原厂 0x7C70) */
        dly_i2c_l();            /* delay(78)(原厂 0x7C72) */
    }
    return b;
}

/* ★ v12: 严格按原厂 0x7B5F(read_sda) 复刻 ACK 读取
 *   原厂: SDA切输入(0x7CBD, 内含 CLR P0.1) → delay(3C) → SCL hi → delay(3C)
 *         → 读 SDA → delay(3C) → SCL lo → delay(3C)
 *   ★ 在 SCL 高电平期间采样 SDA(标准 I2C 做法)
 *   ★ 末尾不切回推挽! SDA 留在输入模式(原厂由下一帧 write_byte ORL P0DIR,#03 切回)
 *   返回(★ v31 更正注释, 旧注释写反了):
 *     1 = ACK  (采样到 SDA 低)
 *     0 = NACK (采样到 SDA 高)
 *   即"是否收到 ACK"的布尔值, 不是原始 SDA 电平。调用方 tx_byte_ack 依赖此语义。
 * *
 * ★★ v20 曾"修正"(CSpyBat 实测发现"上一位是0残留"): 在 sda_input() 前加
 *    `sda_hi(); dly_i2c_l();` 强制把线顶高。
 * ★★ v23 **撤销该修正** —— 它是"假 ACK"的真凶!
 *   分析: i2c_tx8 发完最后一位若为 0, 会推挽把 SDA 驱动到低, 并保持 SCL 低。
 *   原厂做法: 接下来 sda_input() 只把 SDA 切输入, 然后立刻 SCL hi。
 *     此时**从机**在 SCL 低电平期就该把 SDA 拉低(真 ACK);
 *     若从机不存在, 外部上拉在 SCL 抬升前的整个低电平期(>22us)足以把线拉高 → NACK。
 *   v20 的 sda_hi() 问题: 它**先把线强驱到高**, 然后 sda_input() 放开的瞬间,
 *     SCL 还没抬升, 采样点又在 SCL 高之后 —— 但关键是 `dly_i2c_l()`(45us) 期间
 *     时钟线一直低、SDA 被主机驱高, 从机即使想 ACK 也被主机压住; 等 sda_input()
 *     后仅 dly_i2c_l() 才放开, 从机能否在这段里拉低不确定 → 采样点偏移,
 *     于是"无论从机在不在"都倾向于读到高(或读到残影低) —— 实测就是 swap=0 下
 *     8 档**全 R**, 证明判据已被这步污染, 无法区分 REAL 与 LATE。
 *   现回到原厂节奏, 并把 clock9_kind 的"5 点判决"另行用于 NFC_Decide9(它一直都是对的)。 */
static uint8 i2c_read_ack(void)
{
    uint8 ack;
    scl_low();                /* 原厂 0x7CBD 内含 CLR P0.1: 确保在 SCL 低电平期切 SDA */
    sda_input();              /* 原厂 0x7CBD: ANL P0DIR,#FE (SDA 切输入+上拉) */
    dly_i2c_s();              /* delay(3C) */
    scl_hi();                 /* SCL 高(原厂 0x7B6A SETB P0.1) */
    dly_i2c_s();              /* delay(3C): SCL 抬高后按原厂节奏采样 */
    ack = sda_rd();           /* ★ SCL 高时采样(原厂 0x7B73 MOV A,P0) */
    dly_i2c_s();              /* delay(3C) */
    scl_low();                /* SCL 低(原厂 0x7B7D) */
    dly_i2c_s();              /* delay(3C) */
    /* ★ 不切回推挽! SDA 留输入模式(原厂 read_sda 0x7B5F 末尾不做 sda_output) */
    return (uint8)((ack == 0u) ? 1u : 0u);  /* 0=ACK(低), 1=NACK(高) */
}

/* ★ v12: 严格按原厂 read_cmd 0x7C10 循环体复刻
 *   原厂 read_cmd (0x7C10) 循环里不发标准 I2C ACK/NACK,
 *   而是每字节后做推挽 ACK 脉冲:
 *     ORL P0DIR,#03(切回推挽) → SCL lo → SDA lo(ACK) → delay(3C) → SCL hi → SDA lo → delay(78) → SCL lo → SDA lo → delay(3C)
 *   ★ 每字节都发 ACK, 无 NACK! (原厂 0x7C88-0x7CA9)
 *   ★ 结束用 START, 不用 STOP */
static void i2c_rx_ack_pulse(void)
{
    P0DIR |= 0x03u;           /* ★ 原厂 0x7C88: ORL P0DIR,#03 切回推挽 */
    scl_low();                /* SCL lo(原厂 0x7C8B: CLR P0.1) */
    sda_low();                /* SDA lo = ACK(原厂 0x7C8D: CLR P0.0) */
    dly_i2c_s();              /* delay(3C) */
    scl_hi();                 /* SCL hi(原厂 0x7C96) */
    sda_low();                /* SDA lo 保持(原厂 0x7C98: CLR P0.0) */
    dly_i2c_l();              /* delay(78) */
    scl_low();                /* SCL lo(原厂 0x7CA1) */
    sda_low();                /* SDA lo 保持(原厂 0x7CA3: CLR P0.0) */
    dly_i2c_s();              /* delay(3C) */
}

/* ★★ v45: 手册协议读事务最后一字节的主机 NACK 脉冲
 *   手册 5.2.6 时序: "主机给时钟读状态+数据, 最后一字节回 NACK 后 STOP"
 *   与 i2c_rx_ack_pulse 同形状, 但 SDA 全程保持高(=NACK)。 */
static void i2c_rx_nack_pulse(void)
{
    P0DIR |= 0x03u;           /* 切回推挽 */
    scl_low();
    sda_hi();                 /* NACK = SDA 高 */
    dly_i2c_s();
    scl_hi();
    sda_hi();
    dly_i2c_l();
    scl_low();
    sda_hi();
    dly_i2c_s();
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

/* ★ v10: tx_byte_ack 使用原厂 ACK 读取方式 (i2c_read_ack)
 *   返回 NFC_ACK_REAL(ACK) 或 NFC_ACK_NONE(NACK), 供 cmd_send/resp_read 判定
 *   诊断用的 clock9_kind 保留, 仅供 NFC_AckProbe(0xFF) 对照实验
 *
 * ★★★ v31 致命 bug 修复 ★★★
 *   i2c_read_ack() 的返回语义是 1 = 有 ACK(SDA 被从机拉低), 0 = NACK
 *   (见其实现末尾: return (ack == 0u) ? 1u : 0u —— 原始 SDA 电平取反)。
 *   旧代码却按反的来用:
 *       return (ack != 0u) ? NFC_ACK_NONE : NFC_ACK_REAL;
 *   于是每一次从机给出的**真 ACK** 都被判成 NACK, 导致
 *   i2c_write_cmd 第一步就 return 0 -> NFC_ReadMem 恒返回 NFC_ERR_NOACK。
 *   ★ 这也解释了长期以来的矛盾现象: NFC_Decide9(不经过本函数) 一直报 k=0 REAL
 *     (确认真 ACK 存在), 而 NFC_ReadMem 却永远 rm=1 NOACK。
 *   v30e 抓包正是这个样子(44 轮全 k=0 REAL + rm=1), 据此定位到此处。
 */
static uint8 tx_byte_ack(uint8 b)
{
    uint8 ack;
    i2c_tx8(b);
    ack = i2c_read_ack();      /* 返回 1 = 有 ACK(SDA 被拉低), 0 = NACK */
    return (uint8)((ack != 0u) ? NFC_ACK_REAL : NFC_ACK_NONE);
}

/* ------------------------------------------------------------ 配置 API ---- */
void NFC_SetSwap(uint8 s)   { swap01  = (s != 0u) ? 1u : 0u; }
void NFC_SetDrive(uint8 pp) { drv_pp  = (pp != 0u) ? 1u : 0u; }  /* 仅影响 SCL(诊断用) */
void NFC_SetOrder(uint8 o)  { ord_fac = (o != 0u) ? 1u : 0u; }
/* ★ v17: 上电序列开关 —— pulse=1 发 VDDEX 复位脉冲; early=1 先碰线再上 VDDEX */
void NFC_SetPwrSeq(uint8 pulse, uint8 early)
{
    pwr_pulse = (pulse != 0u) ? 1u : 0u;
    pwr_early = (early != 0u) ? 1u : 0u;
}
/* v10: SDA 统一推挽, sda_pp 不再影响 I2C 路径; 保留接口不破坏 main.c */
void NFC_SetSdaPP(uint8 on) { (void)on; }  /* 兼容旧接口, 实际无操作(恒推挽) */

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
/* ★★ v23: NFC_AckProbe 的判据升级 —— 快路径 + 5 点复核
 *
 *  问题(串口实测确认): 旧 NFC_AckProbe 只靠 tx_byte_ack 的**单点**采样,
 *  而单点采样在"从机根本不在、只有上拉造成的慢爬升"时也会读到低 → 判 REAL。
 *  实测: `ack: R R R R R R R R`(8 次全 REAL), 但同一时刻 NFC_Decide9 对
 *  0x54 判 LATE、0x55/0x6B 判 NONE —— 说明快路径的"REAL"是**假 ACK**。
 *
 *  对策: 快路径读出 REAL 时, **再用 NFC_Decide9 的 5 点判决复核**;
 *        只有复核也判 REAL 才返回 REAL; 复核判 LATE/STUCK/NONE 则返回相应值。
 *        快路径本来就读到 NONE(STUCK) 的, 直接采信(NACK 不会假)。
 *  代价: 命中地址多一次完整 I2C 事务(可接受, probe 只在候选地址上做)。
 */
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

    if (kind == NFC_ACK_REAL) {
        /* ★ v23 复核: 用 5 点判决确认真伪, 拒绝 LATE/STUCK 冒充 REAL */
        static uint8 s5[5];
        uint8 k2; (void)s5;
        k2 = NFC_Decide9(addr7, (uint8)((rd != 0u) ? 1u : 0u), s5);
        if (k2 != NFC_ACK_REAL) {
            i2c_stop();
            dly_l();
            g_ack_kind = k2;      /* LATE / STUCK / NONE */
            return k2;
        }
        /* 复核也判 REAL —— 真 ACK, 继续读 status */
        if (rd != 0u && status != 0) {
            i2c_start();          /* 复核事务用到了 STOP, 重新发 START */
            kind = tx_byte_ack((uint8)(((uint8)(addr7 << 1)) | 1u));
            if (kind == NFC_ACK_REAL) {
                *status = i2c_rx8();
                i2c_rx_ack_pulse();
                g_status = *status;
            } else {
                *status = 0xFFu;
            }
        }
        i2c_stop();
        dly_l();
        g_ack_kind = NFC_ACK_REAL;
        return NFC_ACK_REAL;
    }

    /* 快路径判 NONE(从机不在) —— 采信 */
    i2c_stop();
    dly_l();
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
/*
 * ★★ v11: 按原厂"直接块寻址"协议重写命令层 ★★
 *
 * 原厂协议(EEPROM-like, 非手册标准命令):
 *
 * write_cmd(slv, block_addr, data, len):
 *   START(0x7BEE) -> 写地址(slv<<1=0xAA) -> ACK -> 块地址 -> ACK -> 数据[0..len-1] -> ACK -> STOP(0x7BCC 结束)
 *   第一字节是块地址, 无命令码!
 *
 * read_cmd(slv, buf, len):
 *   START(0x7BEE) -> 读地址(slv<<1|1=0xAB) -> ACK -> 读 N 字节 -> STOP(0x7BCC 结束)
 *   不发块地址! 块地址由外部先调 write_cmd 设入芯片内部地址指针
 *
 * batch_read = 两段式:
 *   1. write_cmd(slv, block_addr, NULL, 0)  <- 设块地址(无数据)
 *   2. read_cmd(slv, buf, 16)               <- 读 16 字节
 */
static uint8 i2c_write_cmd(uint8 slv, uint8 block_addr, const uint8 *data, uint8 len)
{
    uint8 i, k;
    /* 1. START -> 写地址 -> ACK (原厂 write_cmd 入口即 START, 无前置 STOP) */
    i2c_start();
    if (tx_byte_ack((uint8)((uint8)(slv << 1) & 0xFEu)) != NFC_ACK_REAL) {
        i2c_stop(); dly_l(); return 0u;
    }
    /* 2. 块地址 -> ACK */
    if (tx_byte_ack(block_addr) != NFC_ACK_REAL) {
        i2c_stop(); dly_l(); return 0u;
    }
    /* 3. 数据 -> ACK (数据字节放宽: 只要不是 NONE 就继续) */
    for (i = 0; i < len; i++) {
        k = tx_byte_ack(data[i]);
        if (k == NFC_ACK_NONE) { i2c_stop(); dly_l(); return 0u; }
    }
    /* 4. 原厂用 STOP(0x7BCC) 结束帧 */
    i2c_stop();
    dly_l();
    return 1u;
}

static uint8 i2c_read_cmd(uint8 slv, uint8 *buf, uint8 len)
{
    uint8 i;
    /* 1. START -> 读地址(slv<<1|1) -> ACK (原厂 read_cmd 入口即 START, 无前置 STOP) */
    i2c_start();
    if (tx_byte_ack((uint8)(((uint8)(slv << 1)) | 1u)) != NFC_ACK_REAL) {
        i2c_stop(); dly_l(); return 0u;
    }
    /* 2. 读 N 字节 (原厂 0x7C3E: sda_input -> 8位读 -> 存 -> 推挽ACK脉冲)
     *    每字节后都发 ACK, 无 NACK! 结束用 STOP */
    for (i = 0; i < len; i++) {
        buf[i] = i2c_rx8();
        i2c_rx_ack_pulse();   /* 原厂推挽 ACK 脉冲, 每字节都 ACK */
    }
    /* 3. 原厂用 STOP(0x7BCC) 结束帧 */
    i2c_stop();
    dly_l();
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

/*
 * ★★★ v45: NIRQ 紧轮询等待 —— 微秒粒度, 抓短脉冲 ★★★
 *   NFC_WaitNirqLow 用 DelayMs(1) 轮询: 若芯片 NIRQ 低脉冲 < 1ms 会全部漏检!
 *   本函数纯 volatile 紧循环 @16MHz, 每圈 ~1.43us, 分辨率微秒级。
 *   返回: 捕获到的 us(0xFFFF=超时)。调用方喂狗责任自负(窗口 < 1s 安全)。
 */
uint16 g_nirqUsLast = 0xFFFFu;       /* 最近一次捕获的 us (0xFFFF=超时) */

uint16 NFC_WaitNirqFast(uint16 ms)
{
    uint32 loops;
    uint32 max = (uint32)ms * 700UL;     /* 约 1.43us/圈 @16MHz */
    if (ms > 250u) { ms = 250u; }
    for (loops = 0u; loops < max; loops++) {
        if ((P1 & 0x01u) == 0u) {
            g_nirqUsLast = (uint16)((loops * 2UL) / 1400UL);   /* ~us */
            return g_nirqUsLast;
        }
    }
    g_nirqUsLast = 0xFFFFu;
    return 0xFFFFu;
}

/*
 * ★ v11: NFC_ReadMem 改为原厂"两段式直接块寻址"协议
 *   1. write_cmd(块地址, len=0) —— 设芯片内部地址指针
 *   2. read_cmd(len) —— 读数据
 *   ★ 无命令码 0x08, 无状态字节, 无 NIRQ 等待
 *   ★ addr 是 FeRAM 字节地址(0~511), block_addr = addr(直接用低字节)
 *     原厂 batch_read 每次 16 字节, 块地址 = 基址+偏移
 */
uint8 NFC_ReadMem(uint8 slv, uint16 addr, uint8 len, uint8 *dst, uint8 *status)
{
    uint8 block_addr;
    if (len == 0u || len > NFC_READ_CHUNK || dst == 0) { return NFC_ERR_PARAM; }
    if (status != 0) { *status = 0xFFu; }
    block_addr = (uint8)(addr & 0xFFu);   /* FeRAM 最大 512B, 块地址=addr低字节 */

    /* 1. 设块地址(无数据) */
    if (i2c_write_cmd(slv, block_addr, (const uint8 *)0, 0u) == 0u) {
        return NFC_ERR_NOACK;
    }
    /* 2. 读数据 */
    if (i2c_read_cmd(slv, dst, len) == 0u) {
        return NFC_ERR_NOACK;
    }
    /* v11: 原厂协议无状态字节, 直接返回 OK */
    g_status = 0x05u;   /* 假定成功(原厂无状态返回) */
    if (status != 0) { *status = g_status; }
    return NFC_OK;
}

/*
 * ★ v31: 分步诊断版 ReadMem —— NFC_Decide9 只验证了"地址字节"这一段,
 *   无法定位后面到底哪一拍断掉。本函数把每一步的第 9 位判定都吐出来:
 *     dbg[0] = 写方向地址字节 (slv<<1|0)  的 ACK 类型
 *     dbg[1] = 块地址                     的 ACK 类型
 *     dbg[2] = 读方向地址字节 (slv<<1|1)  的 ACK 类型
 *     dbg[3] = 0x5A 表示流程走完(仅作哨兵)
 *   ACK 类型: 0 REAL / 1 LATE / 2 NONE / 3 STUCK / 0xFF 未执行
 *   ★ 与普通 ReadMem 不同: 本函数**不因 NACK 提前退出**, 而是硬着头皮走完并读数据,
 *     这样即使某拍没 ACK, 也能看到芯片到底有没有把数据吐出来。
 */
uint8 NFC_ReadMemDiag(uint8 slv, uint8 block, uint8 len, uint8 *dst, uint8 *dbg)
{
    uint8 i;
    if (dst == 0) { return 0xFEu; }
    if (dbg != 0) { dbg[0] = 0xFFu; dbg[1] = 0xFFu; dbg[2] = 0xFFu; dbg[3] = 0xFFu; }

    /* --- 第一段: 设内部地址指针 --- */
    i2c_start();
    if (dbg != 0) { dbg[0] = tx_byte_ack((uint8)((uint8)(slv << 1) & 0xFEu)); }
    else          { (void)tx_byte_ack((uint8)((uint8)(slv << 1) & 0xFEu)); }
    if (dbg != 0) { dbg[1] = tx_byte_ack(block); }
    else          { (void)tx_byte_ack(block); }
    i2c_stop();
    dly_l();

    /* --- 第二段: 读数据 --- */
    i2c_start();
    if (dbg != 0) { dbg[2] = tx_byte_ack((uint8)(((uint8)(slv << 1)) | 1u)); }
    else          { (void)tx_byte_ack((uint8)(((uint8)(slv << 1)) | 1u)); }
    for (i = 0u; i < len; i++) {
        dst[i] = i2c_rx8();
        i2c_rx_ack_pulse();      /* 原厂推挽 ACK 脉冲, 每字节都 ACK */
    }
    i2c_stop();
    dly_l();

    if (dbg != 0) { dbg[3] = 0x5Au; }
    return 0u;
}

/*
 * ★ v31 变体: 两段之间**不发 STOP**, 用"重复 START"(Repeated Start) 衔接。
 *   若 NFC_ReadMemDiag 的 dbg[2] 报 NONE(读方向地址没 ACK), 极可能就是这里:
 *   中间那次 STOP+P 会让芯片结束当前事务、清掉刚设好的地址指针 / 或让总线释放后
 *   从机不再响应。标准 EEPROM 随机读(如 24Cxx)本来就是 **写地址 → 重复 START → 读**,
 *   中间绝不下 STOP。故本变体必须和 Diag 并列对照。
 *     dbg[0]=写方向地址 ACK, dbg[1]=块地址 ACK, dbg[2]=读方向地址 ACK, dbg[3]=0x5A
 */
uint8 NFC_ReadMemDiagRs(uint8 slv, uint8 block, uint8 len, uint8 *dst, uint8 *dbg)
{
    uint8 i;
    if (dst == 0) { return 0xFEu; }
    if (dbg != 0) { dbg[0] = 0xFFu; dbg[1] = 0xFFu; dbg[2] = 0xFFu; dbg[3] = 0xFFu; }

    /* --- 第一段: 写方向 + 块地址(不停!) --- */
    i2c_start();
    if (dbg != 0) { dbg[0] = tx_byte_ack((uint8)((uint8)(slv << 1) & 0xFEu)); }
    else          { (void)tx_byte_ack((uint8)((uint8)(slv << 1) & 0xFEu)); }
    if (dbg != 0) { dbg[1] = tx_byte_ack(block); }
    else          { (void)tx_byte_ack(block); }

    /* --- ★ 關鍵: 这里不下 STOP, 直接再来一次 START = Repeated Start --- */
    i2c_start();
    if (dbg != 0) { dbg[2] = tx_byte_ack((uint8)(((uint8)(slv << 1)) | 1u)); }
    else          { (void)tx_byte_ack((uint8)(((uint8)(slv << 1)) | 1u)); }
    for (i = 0u; i < len; i++) {
        dst[i] = i2c_rx8();
        i2c_rx_ack_pulse();
    }
    i2c_stop();
    dly_l();

    if (dbg != 0) { dbg[3] = 0x5Au; }
    return 0u;
}

/*
 * ★★★ v45: MN63Y1213 手册标准 READ 协议 (第 5 章 —— 从未被完整测过) ★★★
 *
 * 之前 44 版全部在复刻原厂固件的"直接块寻址"非标准协议;
 * 但板端现象始终是: 从地址字节有真 ACK、任何第二字节 NACK。
 * 手册 Table 5-5 的标准命令里, 从地址后面跟的是**命令码**:
 *   READ 0x08 | 起始地址(2B 大端) | 长度n(1B)
 * 芯片若跑的是手册协议, 拿"块地址"当第二字节它当然全 NACK。
 * 本函数把手册 5.2.5.1 + 5.2.6 时序完整走一遍:
 *
 *   写事务: START + slv+W + 0x08 + addrH + addrL + len + STOP
 *   (t1)   主机停时钟, 芯片内部处理
 *   (t2)   处理完成 -> 芯片把 NIRQ 拉低通知主机
 *   读事务: START + slv+R + status + data[0..len-1](末字节 NACK) + STOP
 *
 * 状态字节 (Table 5-3/5-4): bit5=RFCMD_IRQ bit4=RFDET_IRQ
 *   CMD_RES(低4位): 0x5=正常 0x7=BUSY 0x8=未实现 0xA=参数错 0xB=自诊断
 *
 * 返回码:
 *   0 = OK        状态 CMD_RES=0x5, 数据有效
 *   1 = W_NOACK   写事务某字节 NACK (看 dbg[0..4] 哪拍断)
 *   2 = NIRQ_TOUT 250ms 内芯片没拉低 NIRQ
 *   3 = R_NOACK   读事务地址字节 NACK
 *   4 = ST_BAD    状态 CMD_RES != 0x5
 *   5 = PARAM     参数错
 *
 * dbg[0]=slv+W ACK  dbg[1]=0x08 ACK  dbg[2]=addrH ACK  dbg[3]=addrL ACK
 * dbg[4]=len ACK    dbg[5]=slv+R ACK  dbg[6]=status 原值 dbg[7]=0x5A 哨兵(走完)
 * *nirqMs = NIRQ 拉低前等待的 ms (0xFF = 超时)
 *
 * ★ 与 NFC_ReadMemDiag 同风格: 不因 NACK 提前退出, 硬走完, 每拍留痕。
 */
uint8 NFC_ReadStd(uint8 slv, uint16 addr, uint8 len, uint8 *dst,
                  uint8 *dbg, uint8 *nirqMs)
{
    uint8 i, wok = 1u, rok = 1u;

    if (dst == 0u || dbg == 0u || nirqMs == 0u) { return 5u; }
    if (len == 0u || len > NFC_READ_CHUNK)      { return 5u; }
    for (i = 0u; i < 8u; i++) { dbg[i] = 0xFFu; }
    *nirqMs = 0xFFu;

    /* --- 写事务: 命令 0x08 + 起始地址(2B 大端) + 长度 --- */
    i2c_start();
    dbg[0] = tx_byte_ack((uint8)((uint8)(slv << 1) & 0xFEu));
    dbg[1] = tx_byte_ack(MN63Y_CMD_READ);
    dbg[2] = tx_byte_ack((uint8)(addr >> 8));
    dbg[3] = tx_byte_ack((uint8)(addr & 0xFFu));
    dbg[4] = tx_byte_ack(len);
    i2c_stop();
    dly_l();

    for (i = 0u; i < 5u; i++) {
        if (dbg[i] != NFC_ACK_REAL) { wok = 0u; }
    }

    /* --- 等 NIRQ 低 (t1..t2: 芯片处理完成的通知) ---
     *   ★ v45: 改用微秒粒度紧轮询 —— 旧 WaitNirqLow 是 1ms 粒度,
     *   若芯片脉冲 < 1ms 会全部漏检, 造成"NIRQ 从未动作"的假象。 */
    *nirqMs = 0xFFu;
    {
        uint16 irqUs = NFC_WaitNirqFast(250u);
        if (irqUs != 0xFFFFu) {
            *nirqMs = (uint8)((((uint32)irqUs) / 1000UL) + 1u);  /* 捕获: ms 计数至少 1 */
        }
    }

    /* --- 读事务: 状态 + 数据 (最后一字节主机 NACK) --- */
    i2c_start();
    dbg[5] = tx_byte_ack((uint8)(((uint8)(slv << 1)) | 1u));
    if (dbg[5] != NFC_ACK_REAL) { rok = 0u; }
    dbg[6] = i2c_rx8();
    i2c_rx_ack_pulse();                        /* 主机 ACK 状态字节 */
    for (i = 0u; i < len; i++) {
        dst[i] = i2c_rx8();
        if ((uint8)(i + 1u) < len) { i2c_rx_ack_pulse(); }  /* 数据 ACK */
        else                       { i2c_rx_nack_pulse(); }  /* 末字节 NACK */
    }
    i2c_stop();
    dly_l();
    dbg[7] = 0x5Au;

    if (wok == 0u)         { return 1u; }
    if (*nirqMs == 0xFFu)  { return 2u; }
    if (rok == 0u)         { return 3u; }
    if ((uint8)(dbg[6] & 0x0Fu) != MN63Y_ST_OK) { return 4u; }
    return 0u;
}

/*
 * ★★★ v45: STATUS 直读 (手册 5.1: "省略 SNo.1 只做 SNo.2 即可读状态") ★★★
 *   帧结构(手册 5.2.5.5): START + slv+R -> ACK -> 主机读 1 字节状态 -> NACK + STOP
 *   不经过命令处理(t1/t2), 是"数字核死活"的最短探针:
 *     读回非 0xFF  -> 数字核活着, 有状态字节可给
 *     读回 0xFF    -> 响应通道也没数据(数字核复位循环/异常)
 *   返回: 0 = 走完; *ackK = 地址字节 ACK 判定; *status = 读回字节
 */
uint8 NFC_StatusRead(uint8 slv, uint8 *status, uint8 *ackK)
{
    if (status == 0u || ackK == 0u) { return 1u; }
    *status = 0xFFu;
    i2c_start();
    *ackK = tx_byte_ack((uint8)(((uint8)(slv << 1)) | 1u));
    *status = i2c_rx8();
    i2c_rx_nack_pulse();            /* 单字节响应: NACK 结束(手册 5.2.6 t3) */
    i2c_stop();
    dly_l();
    return 0u;
}

/*
 * ★ v33 TX8 Trace: 逐比特总线读回
 *   推挽输出状态下读 P0 得到的是**引脚真实电平**, 于是可以边发边读, 直接分清:
 *     写1读回1 / 写0读回0 -> 总线干净, 字节完好送出
 *     写1读回0            -> 有东西把总线钳低(争用或钳位)
 *     写0读回1            -> 有东西把总线顶高
 *   同时监控 SCL: 若驱动高却读回 0, 说明从机在**时钟拉伸**而主机硬推 -> 打架。
 *
 *   ★ 关键: 同一段代码把"首字节(地址)"和"次字节"都跑一遍做对照。
 *     当前异常正是"首字节有 ACK、次字节没有", 只有把两者放在同一个探针下比较,
 *     才能判断是"芯片行为"还是"我方发的第二个字节根本没上总线"。
 *
 *   rep[0..2] = 首字节: SDA读回不符位图 / SCL被拉低位图 / SCL高期间总线实际值
 *   rep[3..5] = 次字节: 同上
 *   rep[6]    = 首字节 ACK 槽: bit0=抬时钟前SDA bit1=时钟高中SDA bit2=落以后SDA (置1表示读到高)
 *   rep[7]    = 次字节 ACK 槽: 同上
 */
static void tx8_trace(uint8 b, uint8 *sdamis, uint8 *sclmis, uint8 *seen)
{
    uint8 i, mask, need, sd;
    P0DIR |= 0x03u;                       /* 确保推挽 */
    *sdamis = 0u; *sclmis = 0u; *seen = 0u;
    for (i = 0u; i < 8u; i++) {
        mask = (uint8)(0x80u >> i);
        need = (uint8)((b & mask) ? 1u : 0u);
        if (need != 0u) { sda_hi();  } else { sda_low();  }
        dly_i2c_s();
        sd = sda_rd();
        if (sd != need) { *sdamis |= mask; }
        scl_hi();
        dly_i2c_l();
        if (scl_rd() == 0u) { *sclmis |= mask; }
        if (sda_rd() != 0u) { *seen |= mask; }
        scl_low();
        dly_i2c_s();
    }
}

/* ACK 槽三点采样: 抬时钟前 / 时钟高中 / 落以后。返回位图, 置 1 表示该点读到高电平 */
static uint8 ack_slot_trace(void)
{
    uint8 r = 0u;
    scl_low();
    sda_input();
    dly_i2c_s();
    if (sda_rd() != 0u) { r |= 0x01u; }
    scl_hi();
    dly_i2c_s();
    if (sda_rd() != 0u) { r |= 0x02u; }
    dly_i2c_s();
    scl_low();
    dly_i2c_s();
    if (sda_rd() != 0u) { r |= 0x04u; }
    return r;
}

/*
 * ★ v34 ACK 矩阵: 定位"首字节必 ACK"这个假象的触发条件。
 *   地址全扫描显示 128/128 都"有 ACK", 而当事物的第二个字节时恒为 NACK,
 *   说明判据被某个"首字节之后才成立"的因素污染了。本矩阵逐一剥离变量:
 *
 *   m[0] A: START + 发 0x00           -> 数据位全 0(SDA 会被驱低)
 *   m[1] B: START + 发 0xFF           -> 数据位全 1(SDA 全程高)
 *   m[2] C: START + 不发字节          -> 剥离"数据字节"这个变量
 *   m[3] D: 无 START + 不发字节        -> 再剥离 START
 *   m[4] E: START + 发 0xAA           -> 真实用的地址字节
 *   m[5] F: 同一事务里 AA 之后的 0x00  -> 已知的"NACK"对照
 *
 *   每个返回值是 3 点采样位图: bit0=抬时钟前(pre) bit1=时钟高中(hi) bit2=落以后(post)
 *   (置 1 = 读到高; hi=0 才是"看起来有 ACK", post=0 表示线被钉死)
 *
 *   判读:
 *     A/B 都为 ACK  -> 与字节值无关
 *     C 也为 ACK    -> 与有没有数据字节无关, 只跟 START 有关
 *     D 也为 ACK    -> 连 START 都无关 -> 是"连续两次采样"本身的系统偏差
 */
/*
 * ★ v34 时序扫描: 验证"假 ACK"是否由**SCL 翻转本身**引起。
 *   rep[0] = G 情形: START + 8 个**不驱动 SDA** 的时钟 + ACK 槽
 *       若 G 也报"有 ACK", 则与发了什么字节完全无关 -> 纯由"翻转了 8 次 SCL"造成,
 *       属于电气假象(耦合/电荷积累), 总线上并没有器件在应答。
 *   rep[1..5] = START+AA 之后、抬第 9 时钟**之前**, 把 SDA 放开,
 *       分别等约 22 / 66 / 176 / 616 / 1716 us 后读 SDA 电平(1=高)。
 *       若越等越往上抬, 说明内部上拉太弱、还没来得及拉回高 -> 也是电气问题。
 */
/* 前置原型: ADC 换算函数定义在后面的 v35 区块里 */
static uint16 sda_adc_mv(void);

/*
 * ★★ v36 引脚状态快照 ★★
 *   ADC 实测 SDA = 3.07V(远高于 VIH), 而同一时刻数字读值却是 0。
 *   说明问题不在电平, 而在"读那一位"这件事本身。本函数在同一时刻同时取:
 *     数字读值 / 真实电压 / 全部相关寄存器
 *   regs[0]=P0 regs[1]=P0DIR regs[2]=P0SEL regs[3]=P0INP regs[4]=P2INP regs[5]=APCFG
 *       regs[6]=swap01 regs[7]=sda_rd() 用的原始 P0 字节
 *   注意: 寄存器快照必须在调用 ADC **之前**抓, 因为 ADC 会动 APCFG/P0DIR。
 */
/*
 * ★ v36 静态漏电观测: 上电后**一个时钟都不翻**, 直接放开 SDA,
 *   在 0 / 1ms / 5ms / 20ms 处同时取数字读值和真实电压。
 *   判读:
 *     全程稳定 3300mV 且数字=1 -> 静态时总线是好的, 问题只在"翻转几次时钟之后才出现"
 *                                 => 是芯片在事务中被拉低(ACK 保持 / 状态机), 加电阻也救不了
 *     一开始就飘在中间电平     -> 存在**静态**漏电通路需要外部上拉,
 *                                 内部 20k 上拉拉不动 -> 必须外接上拉电阻
 */
uint8 NFC_IdleLeakProbe(uint16 *mv, uint8 *dig)
{
    uint8 i;
    static const uint8 wait_ms[4] = { 0u, 1u, 5u, 20u };

    if (mv == 0 || dig == 0) { return 0u; }

    P0SEL &= (uint8)~0x03u;
    P0DIR |= 0x03u;
    P0INP &= (uint8)~0x03u;              /* 上拉使能 */
    P2INP &= (uint8)~0x20u;              /* PDUP0=0 -> Port0 上拉 */
    P0_0 = 1; P0_1 = 1;                  /* 先推挽驱高, 给节点充上电 */
    dly_i2c_l();
    scl_low();                           /* SCL 保持低 */
    sda_input();                         /* SDA 放开, 交给上拉维持 */

    for (i = 0u; i < 4u; i++) {
        if (wait_ms[i] != 0u) { NFC_DelayMs(wait_ms[i]); }
        else                  { dly_i2c_s(); }
        dig[i] = sda_rd();
        mv[i]  = sda_adc_mv();
    }
    return 4u;
}

uint8 NFC_PinProbe(uint16 *mv, uint8 *dig, uint8 *regs)
{
    uint8 i;
    if (mv == 0 || dig == 0 || regs == 0) { return 0u; }

    P0SEL &= (uint8)~0x03u;
    P0DIR |= 0x03u;

    /* START + 8 个空时钟(全程不驱动 SDA) —— 复现"假 ACK"现场 */
    i2c_start();
    sda_input();
    for (i = 0u; i < 8u; i++) {
        scl_hi();  dly_i2c_s();
        scl_low(); dly_i2c_s();
    }
    sda_input();
    dly_i2c_s();

    /* --- 点 1: SCL 低 --- */
    dig[0] = sda_rd();
    regs[0] = P0;  regs[1] = P0DIR; regs[2] = P0SEL;
    regs[3] = P0INP; regs[4] = P2INP; regs[5] = APCFG;
    regs[6] = swap01; regs[7] = P0;
    mv[0] = sda_adc_mv();

    /* --- 点 2: SCL 高(就是"ACK 采样点") --- */
    scl_hi();
    dly_i2c_s();
    dig[1] = sda_rd();
    mv[1] = sda_adc_mv();

    scl_low();
    dly_i2c_s();
    i2c_stop();
    dly_l();
    return 2u;
}

/*
 * ★★ v35 ADC 实测 SDA 电压 ★★
 *   数字读值已经反复自相矛盾(128/128 全"ACK"、空时钟也"ACK"、放开后 1.7ms 回不到高),
 *   再靠读 0/1 推理没有意义 —— 必须知道**真实电压**。
 *   用 CC2530 自带 ADC 量 P0.0(SDA), AVDD(3.3V) 作参考, 12 位。
 *
 *   mv[0] = SDA 推挽驱高时的电压(基准, 应约 3300mV, 用来验证 ADC 本身没配错)
 *   mv[1] = SDA 放开(输入+内部上拉)静置后的电压 —— 总线空闲时的真实电平
 *   mv[2] = START + 8 个空时钟 之后、放开 SDA 的电压 —— "假ACK"现场
 *   mv[3] = 紧接着第 9 个时钟**抬高中**的 SDA 电压 —— 就是"ACK 采样点"
 *   mv[4] = 第 9 时钟**落下之后**的 SDA 电压 —— 解释为什么 post 会读到 1
 *
 *   判读(3.3V 系统, CC2530 VIH 约 2.0~2.3V, VIL 约 0.9V):
 *     ~3300mV -> 线确实是高的, 那"读到0"就是引脚/寄存器层面的问题(不是电平问题)
 *     ~0~400mV -> 有东西硬拉低 -> 外部\succ 上拉, 总线被占住
 *     ~1000~2000mV -> 落在判决门限附近的**不确定区**, 读到 0 还是 1 全看运气和残余电荷
 *                     => 这才是"时好时坏/128全ACK"的根因
 */
static uint16 sda_adc_mv(void)
{
    uint16 v;
    uint8  dirsave = P0DIR;
    uint8  inpsave = P0INP;               /* ★ v41a: 必须一并保存 */
    uint16 guard;

    APCFG |= 0x01u;                       /* P0.0 -> 模拟输入 */
    ADCCON3 = (uint8)(0xC0u | 0x30u | 0x00u);  /* AVDD 参考 | 512抽取(12bit) | AIN0 */
    /* ★ v41a: 加超时兜底 —— 万一 EOC 不置位, 绝不在这里死等 */
    for (guard = 0u; guard < 60000u; guard++) {
        if ((ADCCON1 & 0x80u) != 0u) { break; }
    }
    v = (uint16)((((uint16)(ADCH & 0x0Fu)) << 8) | ADCL);
    /* ★ v41a: 完整还原 —— 原来漏了 P0INP, 会让 P0.0 的上拉保持在
     *   "模拟模式"遗留状态, 之后所有 sda_rd() 全部失真。 */
    APCFG &= (uint8)~0x01u;               /* 还原数字 IO */
    P0SEL &= (uint8)~0x01u;
    P0INP = inpsave;                      /* 还原上拉配置 */
    P0DIR = dirsave;                      /* 还原方向(可能是 input) */
    return (uint16)((v * 3300UL) / 4096UL);
}

uint8 NFC_AdcProbe(uint16 *mv)
{
    uint8 i;
    if (mv == 0) { return 0u; }

    P0SEL &= (uint8)~0x03u;
    P0DIR |= 0x03u;
    P0_0 = 1; P0_1 = 1;
    dly_i2c_s();
    mv[0] = sda_adc_mv();                 /* 基准: 推挽高 */

    sda_input();
    dly_i2c_s();
    mv[1] = sda_adc_mv();                 /* 放开静置 */

    /* START + 8 空时钟, 全程不驱动 SDA */
    i2c_start();
    sda_input();
    for (i = 0u; i < 8u; i++) {
        scl_hi();  dly_i2c_s();
        scl_low(); dly_i2c_s();
    }
    sda_input();
    dly_i2c_s();
    mv[2] = sda_adc_mv();                 /* 假ACK现场 */

    scl_hi();
    dly_i2c_s();
    mv[3] = sda_adc_mv();                 /* 第9时钟高中 = ACK采样点 */

    scl_low();
    dly_i2c_s();
    mv[4] = sda_adc_mv();                 /* 落以后 */

    i2c_stop();
    dly_l();
    return 5u;
}

uint8 NFC_AckTimingSweep(uint8 *rep)
{
    uint8 i, n;
    static const uint8 steps[5] = { 1u, 2u, 5u, 20u, 50u };

    if (rep == 0) { return 0u; }

    /* --- G: START + 8 个空时钟(SDA 全程释放) + ACK 槽 --- */
    i2c_start();
    sda_input();
    for (i = 0u; i < 8u; i++) {
        scl_hi();  dly_i2c_s();
        scl_low(); dly_i2c_s();
    }
    rep[0] = ack_slot_trace();
    i2c_stop();
    dly_l();

    /* --- 放开 SDA 后的延时扫描(还没抬第 9 时钟) --- */
    i2c_start();
    i2c_tx8(0xAAu);
    scl_low();
    dly_i2c_s();
    sda_input();
    for (i = 0u; i < 5u; i++) {
        for (n = 0u; n < steps[i]; n++) { dly_i2c_s(); }
        rep[1u + i] = sda_rd();
    }
    i2c_stop();
    dly_l();
    return 6u;
}

uint8 NFC_AckMatrix(uint8 *m)
{
    uint8 d0, d1, d2;

    if (m == 0) { return 0u; }

    i2c_start(); tx8_trace(0x00u, &d0, &d1, &d2);
    m[0] = ack_slot_trace(); i2c_stop(); dly_l();

    i2c_start(); tx8_trace(0xFFu, &d0, &d1, &d2);
    m[1] = ack_slot_trace(); i2c_stop(); dly_l();

    i2c_start();
    m[2] = ack_slot_trace(); i2c_stop(); dly_l();

    m[3] = ack_slot_trace(); i2c_stop(); dly_l();

    i2c_start(); tx8_trace(0xAAu, &d0, &d1, &d2);
    m[4] = ack_slot_trace(); i2c_stop(); dly_l();

    i2c_start(); tx8_trace(0xAAu, &d0, &d1, &d2); (void)ack_slot_trace();
    tx8_trace(0x00u, &d0, &d1, &d2);
    m[5] = ack_slot_trace(); i2c_stop(); dly_l();

    return 6u;
}

/*
 * ★ v33 地址全扫描: 用**逐比特读回 + ACK 槽三点采样**扫 0x00..0x7F,
 *   验证"0x55 的 ACK"到底是不是唯一真实存在的从设备。
 *   —— 若一大片地址都"有 ACK", 说明判据仍有系统误差, 前面所有结论全部作废重来。
 *   out[a]: 0=无ACK  1=真ACK(时钟高期间为低, 落沿后释放)  2=被钉死(ACK 不可信)
 *   返回真 ACK 的地址个数。
 */
/* out 为 **2bit 位图**: 每字节装 4 个地址, 共 128 地址 -> 32 字节
 *   地址 a 的取值放在 out[a>>2] 的 bit(2*(a&3)):
 *     00 = 无 ACK   01 = 真 ACK   10 = 被钉死
 *   (v43: 原来是 uint8 out[128], 占 128 字节; XDATA 太紧张, 压成位图省 96 字节)
 *   返回真 ACK 的地址个数。 */
uint8 NFC_AddrScanAck(uint8 *out)
{
    uint8 a, d0, d1, d2, r, cnt = 0u;
    if (out == 0) { return 0u; }
    for (a = 0u; a < 32u; a++) { out[a] = 0u; }      /* 先全部清成"无 ACK" */
    for (a = 0u; a < 128u; a++) {
        uint8 val = 0u;
        g_scanCur = a;                /* ★ v44: 实时进度, 卡住时这一位就是最后一句还没干完的活 */
        i2c_start();
        tx8_trace((uint8)(a << 1), &d0, &d1, &d2);
        r = ack_slot_trace();
        i2c_stop();
        dly_l();
        if ((r & 0x02u) == 0u) {
            if ((r & 0x04u) != 0u) { val = 1u; cnt++; }
            else                   { val = 2u; }
        }
        if (val != 0u) {
            out[(uint8)(a >> 2)] |= (uint8)(val << (uint8)(2u * (a & 3u)));
        }
    }
    return cnt;
}

uint8 NFC_Tx8Trace(uint8 addr7, uint8 second, uint8 *rep)
{
    uint8 a0, a1, a2, b0, b1, b2, t6, t7;

    i2c_start();
    tx8_trace((uint8)((uint8)(addr7 << 1) & 0xFEu), &a0, &a1, &a2);
    t6 = ack_slot_trace();
    tx8_trace(second, &b0, &b1, &b2);     /* ack_slot_trace 留 SDA 为输入, tx8_trace 会切回推挽 */
    t7 = ack_slot_trace();
    i2c_stop();
    dly_l();

    if (rep != 0) {
        rep[0] = a0; rep[1] = a1; rep[2] = a2;
        rep[3] = b0; rep[4] = b1; rep[5] = b2;
        rep[6] = t6; rep[7] = t7;
    }
    return 0u;
}

/*
 * ★ v32 第二字节扫描: 对每个候选字节做一轮 START + 0xAA + cand + STOP,
 *   记录 cand 那一拍的 ACK 类型。
 *   用途: 区分两种情况
 *     (a) 候选里**某些**值能拿到 ACK  -> 第二字节是有语义的协议字段(命令/块地址),
 *         说明是我方协议字节值不对, 改字节值即可。
 *     (b) 候选**全部** NACK          -> 芯片只做地址匹配, 不接受任何后续字节,
 *         典型是"还没进入可传输状态"(缺 RF 场 / 缺上电握手 / 仍处于 RF-only)。
 *   out[i] 对应 cand[i] 的 ACK 类型 (0 REAL / 1 LATE / 2 NONE / 3 STUCK)
 */
uint8 NFC_ProbeSecondByte(uint8 slv, const uint8 *cand, uint8 n, uint8 *out)
{
    uint8 i;
    if (cand == 0 || n == 0u) { return 0u; }
    for (i = 0u; i < n; i++) {
        i2c_start();
        (void)tx_byte_ack((uint8)((uint8)(slv << 1) & 0xFEu));
        if (out != 0) { out[i] = tx_byte_ack(cand[i]); }
        else          { (void)tx_byte_ack(cand[i]); }
        i2c_stop();
        dly_l();
    }
    return n;
}

/*
 * ★ v11: NFC_WriteMem 改为原厂"直接块寻址"协议
 *   write_cmd(块地址, 数据, len): START → 写地址 → 块地址 → 数据 → START(结束)
 *   ★ 无命令码 0x18, 无 addrHi, 无 len 字段, 无状态字节, 无 NIRQ 等待
 *   ★ addr 是 FeRAM 字节地址(0~511), 块地址=addr低字节
 */
uint8 NFC_WriteMem(uint8 slv, uint16 addr, const uint8 *src, uint8 len, uint8 *status)
{
    uint8 block_addr;
    if (len == 0u || len > NFC_WRITE_CHUNK || src == 0) { return NFC_ERR_PARAM; }
    if (status != 0) { *status = 0xFFu; }
    block_addr = (uint8)(addr & 0xFFu);

    if (i2c_write_cmd(slv, block_addr, src, len) == 0u) {
        return NFC_ERR_NOACK;
    }
    /* v11: 原厂协议无状态字节返回, 直接返回 OK */
    g_status = 0x05u;
    if (status != 0) { *status = g_status; }
    return NFC_OK;
}

/*
 * ★ v11: NFC_RReg/NFC_WReg 改为原厂协议兼容版
 *   原厂无 RREG/WREG 命令码, 寄存器读写可能通过块寻址特定地址实现。
 *   暂保留接口, 内部用 ReadMem/WriteMem 从特定地址读写。
 *   如果 MN63Y1213 不支持这些操作, 会返回 NOACK。
 */
uint8 NFC_RReg(uint8 slv, uint8 *status, uint8 *data)
{
    uint8 st = 0xFFu, d = 0xFFu;
    /* v11: 用原厂块寻址读地址 0x00(假设寄存器区) */
    if (NFC_ReadMem(slv, 0x00u, 1u, &d, &st) != NFC_OK) { return NFC_ERR_NOACK; }
    if (status != 0) { *status = st; }
    if (data   != 0) { *data   = d;  }
    return NFC_OK;
}

uint8 NFC_WReg(uint8 slv, uint8 data, uint8 *status)
{
    uint8 st = 0xFFu;
    /* v11: 用原厂块寻址写地址 0x00 */
    if (NFC_WriteMem(slv, 0x00u, &data, 1u, &st) != NFC_OK) { return NFC_ERR_NOACK; }
    if (status != 0) { *status = st; }
    return NFC_OK;
}

/* --------------------------------------------------------- NDEF 解析 ------- */
/*
 * ★★ v7: MN63Y1213 是 NFC Forum Type 3 Tag (FeliCa) ★★
 *   实测(NFC Tools HEX 转储):
 *     Block0 (0x0000) = Attribute 块, 16 字节:
 *       [0]    Ver  (0x10 = 1.0)
 *       [1]    Nbr  (一次可读块数, 实测 0x01)
 *       [2]    Nbw  (一次可写块数, 实测 0x01)
 *       [3..5] 保留 0
 *       [6..7] NmaxB (最大块数? 实测 0x0002)
 *       [8..9] 保留 0
 *       [10]   WrFlag(写标志, 0x00=可写; 0xFF=只读)
 *       [11]   WrFlag2
 *       [12..13] NdefDataLen (NDEF 数据长度, 实测 0x0017 = 23)
 *       [14..15] Ln (总长度含属性? 实测 0x002B = 43)
 *     Block1.. (0x0010..) = NDEF 记录 (从 block 1 开始!)
 *       D1 01 paylen 54 02 65 6E <text...>
 *       D1 = TNF=well-known | MB=1 | ME=1 | SR=1 | CF=0
 *       01 = TypeLen=1, paylen = PayloadLen, 54='T', 02=Status(lang=en)
 *   ★ 根因: 旧代码把 NDEF 记录直接写到地址 0x0000, 覆盖了 Attribute 块,
 *     导致手机读到属性错乱("可写:不")、CC2530 自己也读不回。
 *   ★ 修复: 写时保护 block 0, NDEF 从 block 1(0x0010) 写起; 读时从 block 1 解析。
 */
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

/*
 * v7: 读全部 FeRAM, NDEF 从 block 1(偏移 0x10) 开始解析
 *   block 0 是 Type3 Attribute, 不是 NDEF, 跳过; 从 off=0x10 起扫描 D1 01 .. 'T'
 */
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
    /* ★ Type3: 从 block 1(0x0010) 开始找 NDEF, 跳过 block 0 的 Attribute */
    {
        uint16 ndef_start = 0x0010u;
        if (ndef_start >= off) { return NFC_ERR_NDEF; }
        return ndef_find_text(&g_mem[ndef_start], (uint16)(off - ndef_start), out, max);
    }
}

/*
 * v7: 按 Type 3 协议写 NDEF 文本
 *   1) 先读 block 0 拿到 Attribute(保护原有属性, 不覆盖)
 *   2) NDEF 记录从 block 1(0x0010) 写起, 16 字节对齐按块写
 *   3) 更新 block 0 的 NdefDataLen 字段 [12..13]
 *
 *   NDEF 记录格式(与实测一致):
 *     D1 01 paylen 54 02 65 6E <text...>
 *     头 7 字节 + text, 总长 = 7 + tlen
 *   写入按 16 字节块, 不足补 0
 */
uint8 NFC_WriteTextEx(uint8 slv, uint16 addr, const char *text)
{
    static __xdata uint8 attr[16];        /* block 0 原始 Attribute */
    static __xdata uint8 blk[16];         /* 单块写入缓冲(16 字节) */
    uint8  tlen = 0u, st = 0xFFu, nblk, bi, i;
    uint16 ndef_total, blk_addr;
    uint8  r;

    if (text == 0) { return NFC_ERR_PARAM; }
    while (text[tlen] != '\0' && tlen < 40u) { tlen++; }

    /* NDEF 记录: D1 01 paylen 'T' 02 'e' 'n' <text>
     *   payload = status(1) + lang(2) + text(tlen) = 3 + tlen
     *   记录头 = 4 字节(D1 01 paylen 'T')
     *   总长 = 4 + 3 + tlen = 7 + tlen
     */
    ndef_total = (uint16)(7u + tlen);

    /* 1) 读 block 0(0x0000) 保护 Attribute */
    r = NFC_ReadMem(slv, 0x0000u, 16u, attr, &st);
    if (r != NFC_OK) { return r; }

    /* 2) 从 block 1(0x0010) 写 NDEF, 按 16 字节块写, 不足补 0 */
    nblk = (uint8)((ndef_total + 15u) / 16u);     /* 需要几块 */
    if (nblk == 0u) { nblk = 1u; }

    for (bi = 0u; bi < nblk; bi++) {
        uint16 base = (uint16)(bi * 16u);
        blk_addr = (uint16)(0x0010u + base);
        /* 填 blk[0..15] */
        for (i = 0u; i < 16u; i++) {
            uint16 idx = (uint16)(base + i);
            if (idx < ndef_total) {
                uint8 c;
                if (idx == 0u)      { c = 0xD1u; }                 /* TNF=wellknown,MB,ME,SR */
                else if (idx == 1u) { c = 0x01u; }                /* TypeLen=1 */
                else if (idx == 2u) { c = (uint8)(3u + tlen); }   /* PayloadLen */
                else if (idx == 3u) { c = 'T'; }                  /* Type='T' */
                else if (idx == 4u) { c = 0x02u; }                /* Status: lang len=2, UTF-8 */
                else if (idx == 5u) { c = 'e'; }                  /* lang "en" */
                else if (idx == 6u) { c = 'n'; }
                else                { c = (uint8)text[(uint8)(idx - 7u)]; }
                blk[i] = c;
            } else {
                blk[i] = 0x00u;                                   /* 不足补 0 */
            }
        }
        r = NFC_WriteMem(slv, blk_addr, blk, 16u, &st);
        if (r != NFC_OK) { return r; }
        NFC_DelayMs(5u);   /* 块间留余量, 让 FeRAM 写完 */
    }

    /* 3) 更新 block 0 的 NdefDataLen[12..13] (NDEF 数据字节数, 不含属性块) */
    attr[12] = (uint8)(ndef_total >> 8);
    attr[13] = (uint8)(ndef_total & 0xFFu);
    /* Ln[14..15] = ndef_total + 16 (含属性块? 实测 0x2B=43 = 23+20, 保持公式) */
    {
        uint16 ln = (uint16)(ndef_total + 20u);
        attr[14] = (uint8)(ln >> 8);
        attr[15] = (uint8)(ln & 0xFFu);
    }
    r = NFC_WriteMem(slv, 0x0000u, attr, 16u, &st);
    return r;
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
    P2SEL &= (uint8)~0x02u;            /* ★ v24: P2.3 功能位(0=GPIO) */
    P2INP |= 0x08u;                    /* ★ v24: P2.3 输入时三态, 不内拉(否则下拉会误判掉电) */
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

/* ===========================================================================
 *   ★★★ v28: 寄生供电探测 —— 不改硬件的最后一条软件路径
 *
 *   前提: v27h 已证明 P2.3 被钳死(驱高读回 0, hold=0ms), VDDEX 无法由 MCU 供。
 *   手册 §2.2: "I2C is N-ch open drain, pulled up to VDDEX externally",
 *   且芯片 VDDEX 内部有 ESD 钳位二极管。**若把 SDA/SCL 推挽驱高**,
 *   电流可经 ESD 二极管倒灌进 VDDEX 网络, 给芯片"寄生供电"
 *   (原厂 gpio_prep 正是推挽驱高 —— 这可能是原厂板能用的原因之一)。
 *
 *   做法: 把 SDA/SCL 推挽驱高并保持一段时间(给芯片内部电容充电),
 *         然后**在不碰 P2.3**的前提下做 5 点判决 ACK 探测。
 *   返回: NFC_ACK_REAL/LATE/STUCK/NONE; *s5 回填 5 点采样。
 * =========================================================================== */
uint8 NFC_ProbeParasitic(uint8 addr7, uint16 chargeMs, uint8 *s5)
{
    uint8 k;
    /* ① SDA/SCL 推挽驱高, 保持 chargeMs(充电) —— 不碰 P2.3 */
    P0SEL &= (uint8)~0x03u;
    P0DIR |= 0x03u;              /* 推挽输出(非开漏) */
    P0INP &= (uint8)~0x03u;
    P2INP &= (uint8)~0x20u;      /* Port0 上拉使能 */
    P0IEN &= (uint8)~0x03u;
    P0_0 = 1; P0_1 = 1;
    {
        uint16 t = 0u;
        while (t < chargeMs) {
            volatile uint32 w = 0u;
            while (w < 260UL) { w++; }      /* ~1ms @32MHz */
            t++;
        }
    }
    /* ② 用这些线做 5 点判决探测 */
    k = NFC_Decide9(addr7, 1u, s5);
    return k;
}

/* ===========================================================================
 *          v6: 第 9 位「长保持」判决 + 盲读 + 长判据扫描
 * =========================================================================== */
/* us 级等待(由 T1 决定), 自动分段避免 16 位溢出 */
static void wait_us(uint16 us)
{
    uint32 t = ((uint32)us * 1000UL) / (uint32)g_tickNs;
    while (t > 60000UL) { t1_wait_ticks(60000u); t -= 60000UL; }
    if (t != 0UL) { t1_wait_ticks((uint16)t); }
}

/* NFC_SetSdaPP 已移至配置 API 区(v10: 恒推挽, 接口保留无操作) */

/* 发一个地址字节(可不发), 然后把 SCL 抬高并**保持 ~1.1ms**, 5 点采样 SDA。
 * 判据只用 s[3](SCL 高 1.1ms 后仍低): 真从机 ACK 必须一直低; 假象早回高。 */
uint8 NFC_Decide9(uint8 addr7, uint8 rd, uint8 *s5)
{
    i2c_start();
    if (addr7 != 0xFFu) {
        i2c_tx8((uint8)(((uint8)(addr7 << 1)) | ((rd != 0u) ? 1u : 0u)));
    }
    sda_rel();
    scl_low();
    dly_l();
    s5[0] = sda_rd();          /* SCL 高之前(低电平期末) */
    scl_hi();
    s5[1] = sda_rd();          /* 刚抬 SCL */
    wait_us(100u);
    s5[2] = sda_rd();          /* +100us */
    wait_us(1000u);
    s5[3] = sda_rd();          /* +1.1ms, SCL 仍高 */
    scl_low();
    wait_us(200u);
    s5[4] = sda_rd();          /* SCL 落之后 +200us */
    i2c_stop();
    dly_l();
    if (s5[3] == 0u) { return (s5[4] == 0u) ? NFC_ACK_STUCK : NFC_ACK_REAL; }
    if ((s5[1] == 0u) || (s5[2] == 0u)) { return NFC_ACK_LATE; }
    return NFC_ACK_NONE;
}

uint8 NFC_BlindRead(uint8 addr7, uint8 *dst, uint8 n)
{
    uint8 i;
    i2c_start();
    (void)tx_byte_ack((uint8)(((uint8)(addr7 << 1)) | 1u));   /* 结果不理会 */
    for (i = 0u; i < n; i++) {
        dst[i] = i2c_rx8();
        i2c_rx_ack_pulse();   /* ★ 原厂推挽 ACK 脉冲, 每字节都 ACK */
    }
    i2c_stop();
    dly_l();
    return n;
}

uint8 NFC_ScanLong(uint8 *first3, uint8 *holdCnt)
{
    uint8 a, n = 0u, hc = 0u, s[5], k;
    first3[0] = 0xFFu; first3[1] = 0xFFu; first3[2] = 0xFFu;
    for (a = 0u; a < 128u; a++) {
        k = NFC_Decide9(a, 1u, s);
        if ((k == NFC_ACK_REAL) || (k == NFC_ACK_STUCK)) {
            if (n < 3u) { first3[n] = a; }
            n++;
        }
        if (s[3] == 0u) { hc++; }
    }
    if (holdCnt != 0) { *holdCnt = hc; }
    return n;
}

/* ===========================================================================
 * ★★★★★ v41 [SLOW]: SDA 释放后的"慢充电"实验 ★★★★★
 *
 * 动机(来自 v40 的 EPD 实测):
 *   DET 2/4 的 ADC 数据显示, SDA 放开后电压是 "1786mV -> 1363mV" 这种**中间值**,
 *   一直回不到 3296mV。而 ack_slot_trace() 里:
 *       sda_input()  = P0DIR&=~0x01  +  P2INP&=~0x20(开内部上拉)
 *       dly_i2c_s()  = 22us
 *   CC2530 内部上拉约 20kΩ 量级, 而 SDA 线(含线缆/屏)寄生电容可达 100~300pF,
 *   RC = 20k*200p = 4us 看着够 —— 但如果走线长/容性大(1nF 量级), RC 就到 20us,
 *   22us 只充到 63%, 于是读口得到 0 —— 这是**纯电气现象, 和从机无关**。
 *
 * ★★★ 致命坑 (v41a 修复) ★★★
 *   本区块**绝对不能**用 dly_i2c_s()/dly_i2c_l() 做延时 —— 它们最终调用
 *   t1_wait_ticks(), 而该函数是**无保护的纯等待**:
 *       do { e = read_t1(); } while ((e - s) < t);
 *   T1 一旦不计数(本板 t1ok/calOk 曾被实测为 0), 这里就是**永久死循环**。
 *   NFC_DelayMs() 里专门为 T1/ST 各加了一道兜底, 恰恰说明作者知道 T1 会停;
 *   但位层 delay 没有兜底 —— 历史上"看起来卡死"多半就是踩了这个。
 *   ⇒ 本区块一律改用 v41_spin_us(): 纯 volatile 软循环, 不碰任何定时器。
 *
 * 两个互相独立的实验, 各用一个条目/一页, 结论必须能对上:
 *   [SLOW] 采样点时间轴: 放开 SDA 后在 22us/66us/176us/660us/1980us/6600us
 *          读数字值 + 量 ADC 电压。若电压**单调爬升**-> 就是 RC 充电不足。
 *   [STRONG] 强上拉对照: 先把 SDA 用**推挽驱动高**再放开(线已充满), 立即在同样的
 *          时间点读。若这次立刻读到 1 -> 证明"读不到高"纯粹是充电时间不够。
 * =========================================================================== */

/* 与 main.c 的固定忙等同形状: 内层 260 圈 ≈ 1ms @32MHz
 * 这里按 1/1000 粒度: 26 圈 ≈ 100us; 用 1us 当量 = 0.26 圈, 取 1 圈/2us 的保守值。 */
static void v41_spin_us(uint16 us)
{
    volatile uint16 w;
    if (us == 0u) { return; }            /* 0 = "不再加延时" 的哨兵值 */
    for (w = 0u; w < us; w++) {
        volatile uint8 k = 0u;
        while (k < 8u) { k++; }          /* ~2us @32MHz, 往上取安全 */
    }
}

/* 采样时间档(us): 22 / 66 / 176 / 660 / 1980 / 6600 —— 收敛到 22..1980,
 * 6600us 那档没有额外信息量(660us 已经远超任何 RC 时间常数), 去掉以缩短一轮耗时 */
static const uint16 v41_us[6] = { 22u, 66u, 176u, 660u, 1980u, 0u };

uint8 NFC_SlowReleaseProbe(uint16 *mv, uint8 *dig)
{
    uint8 i;
    if (mv == 0 || dig == 0) { return 0u; }

    P0SEL &= (uint8)~0x03u;
    P0DIR |= 0x03u;

    /* 先保证线是满高: 推挽驱高 */
    P0_0 = 1; P0_1 = 1;
    v41_spin_us(50u);
    sda_input();                        /* 放开 SDA, 只靠内部上拉 */

    for (i = 0u; i < 6u; i++) {
        v41_spin_us(v41_us[i]);
        dig[i] = sda_rd();
        mv[i]  = sda_adc_mv();
    }
    P0DIR |= 0x03u;
    v41_spin_us(50u);
    return 6u;
}

uint8 NFC_StrongPullProbe(uint16 *mv, uint8 *dig)
{
    uint8 i;
    if (mv == 0 || dig == 0) { return 0u; }

    P0SEL &= (uint8)~0x03u;

    /* ★ 用推挽把线**彻底充满**, 再放开 —— 与 [SLOW] 唯一的差别就在这里 */
    P0DIR |= 0x03u;
    P0_0 = 1; P0_1 = 1;
    v41_spin_us(200u);
    sda_input();
    v41_spin_us(1u);                    /* 只放 1us 就采第一个点 */

    for (i = 0u; i < 6u; i++) {
        v41_spin_us(v41_us[i]);
        dig[i] = sda_rd();
        mv[i]  = sda_adc_mv();
    }
    P0DIR |= 0x03u;
    v41_spin_us(50u);
    return 6u;
}

/* ★ v41 [SLOT]: ACK 槽"慢速版" —— 每个采样点前都留足 500us(纯软件忙等),
 *   让内部弱上拉有足够时间把线充起来。若这样就变成"无 ACK",
 *   说明之前所有 ACK 全是充电不足造成的假象。 */
uint8 NFC_AckSlotSlow(uint8 *rep)
{
    uint8 r = 0u;

    if (rep == 0) { return 0u; }

    /* START: SCL 高, SDA 高->低 */
    P0DIR |= 0x03u;
    P0_1 = 1; P0_0 = 1;
    v41_spin_us(50u);
    P0_0 = 0;
    v41_spin_us(25u);
    P0_1 = 0;
    v41_spin_us(25u);

    sda_input();                        /* 放开 SDA */

    P0_1 = 0;                           /* --- 采样点 1: 抬时钟前 --- */
    v41_spin_us(500u);
    if (sda_rd() != 0u) { r |= 0x01u; }

    P0_1 = 1;                           /* --- 采样点 2: 时钟高中 --- */
    v41_spin_us(500u);
    if (sda_rd() != 0u) { r |= 0x02u; }

    P0_1 = 0;                           /* --- 采样点 3: 落下后 --- */
    v41_spin_us(500u);
    if (sda_rd() != 0u) { r |= 0x04u; }

    P0DIR |= 0x03u;
    P0_0 = 0; P0_1 = 1;                 /* STOP */
    v41_spin_us(50u);
    P0_0 = 1;
    v41_spin_us(100u);

    rep[0] = r;
    return 1u;
}

/* ===========================================================================
 * ★★★★★ v42 [WHO]: 先钉死"我们量的到底是哪根线" ★★★★★
 *
 * 现象回顾(v41 实测, EPD):
 *   DET 5/6: 释放 SDA 后 22us..1980us 全是 "dig=1, 3296mV" —— 弱上拉够用, 充电无问题。
 *   DET 2/4: 但 "9th clk HIGH = 228mV" —— 同一根线在 ACK 槽却只有 228mV。
 *   两者矛盾, 除非**它们量的不是同一根线**, 或**量之前线被人为驱动过**。
 *
 * 嫌疑: swap01(0=正常 SDA=P0.0 / SCL=P0.1; 1=两线对调)。
 *   sda_adc_mv() 硬编码量 AIN0 = P0.0。若 swap01=1, 那它量的是 SCL 而不是 SDA,
 *   而 ACK 槽里 SCL 正好被推挽驱高/驱低 —— 228mV 就完全合理了。
 *
 * 本函数做三件事:
 *   ① 把 swap01 与全部相关寄存器原样打出来(不猜)。
 *   ② 逐个把 P0.0 驱高/驱低, 量真实电压 —— 证明"ADC->引脚"这条通道没错位。
 *   ③ 同样对 P0.1 做一遍, 得到两根线的对照。
 *   判读:
 *     P0.0Hi≈3296, P0.0Lo≈0 且 P0.1Hi≈3296, P0.1Lo≈0  -> ADC 通道正确, 两根线都能驱到两端
 *     某根 Hi/Lo 都是中间值 -> 该脚被外部钳位(或 ADC 配错通道)
 */
static uint16 v42_pin_adc(uint8 pin)   /* pin: 0 = P0.0(AIN0), 1 = P0.1(AIN1) */
{
    uint16 v;
    uint16 guard;
    uint8  apc = (uint8)((pin == 0u) ? 0x01u : 0x02u);

    APCFG |= apc;
    ADCCON3 = (uint8)(0xC0u | 0x30u | (pin == 0u ? 0x00u : 0x01u));
    for (guard = 0u; guard < 60000u; guard++) {
        if ((ADCCON1 & 0x80u) != 0u) { break; }
    }
    v = (uint16)((((uint16)(ADCH & 0x0Fu)) << 8) | ADCL);
    APCFG &= (uint8)~apc;
    return (uint16)((v * 3300UL) / 4096UL);
}

uint8 NFC_PinTruthProbe(uint8 *regs, uint16 *mv)
{
    uint8 i;

    if (regs == 0 || mv == 0) { return 0u; }

    /* ① 寄存器原样快照(此时 GPIO 已被 gpio_prep 配好) */
    regs[0] = swap01;
    regs[1] = P0;
    regs[2] = P0DIR;
    regs[3] = P0SEL;
    regs[4] = P0INP;
    regs[5] = P2INP;
    regs[6] = APCFG;

    /* ② P0.0 驱高 / 驱低 各量一次 */
    P0SEL &= (uint8)~0x03u;
    P0DIR |= 0x03u;

    P0_0 = 1; P0_1 = 1;
    for (i = 0u; i < 200u; i++) { volatile uint8 k = 0u; while (k < 20u) { k++; } }
    mv[0] = v42_pin_adc(0u);      /* P0.0 高 */

    P0_0 = 0; P0_1 = 1;
    for (i = 0u; i < 200u; i++) { volatile uint8 k = 0u; while (k < 20u) { k++; } }
    mv[1] = v42_pin_adc(0u);      /* P0.0 低 */

    P0_0 = 1; P0_1 = 1;
    for (i = 0u; i < 200u; i++) { volatile uint8 k = 0u; while (k < 20u) { k++; } }
    mv[2] = v42_pin_adc(1u);      /* P0.1 高 */

    P0_0 = 1; P0_1 = 0;
    for (i = 0u; i < 200u; i++) { volatile uint8 k = 0u; while (k < 20u) { k++; } }
    mv[3] = v42_pin_adc(1u);      /* P0.1 低 */

    P0_0 = 1; P0_1 = 1;

    regs[7] = (uint8)((P0 & 0x03u));   /* 驱高后回读 */
    return 8u;
}

/* ★ v42 [SLOTMAP]: ACK 槽期间**逐点**量 SDA 与 SCL 的真实电压,
 *   两个采样点各量两根线, 直接回答"228mV 到底是 SDA 还是 SCL"。
 *   out[0..3] = SDA@pre, SCL@pre, SDA@hi, SCL@hi   (mV)
 *   dig[0..3] = 对应的数字读值
 */
uint8 NFC_AckSlotMap(uint16 *out, uint8 *dig)
{
    uint8 r = 0u;

    if (out == 0 || dig == 0) { return 0u; }

    P0SEL &= (uint8)~0x03u;
    P0DIR |= 0x03u;
    P0_0 = 1; P0_1 = 1;
    { volatile uint16 w = 0u; while (w < 300u) { w++; } }

    /* START: SCL 高, SDA 高 -> 低 */
    P0_1 = 1; P0_0 = 1;
    { volatile uint16 w = 0u; while (w < 60u) { w++; } }
    P0_0 = 0;
    { volatile uint16 w = 0u; while (w < 30u) { w++; } }
    P0_1 = 0;
    { volatile uint16 w = 0u; while (w < 30u) { w++; } }

    /* 放开 SDA(输入+上拉) —— 与 ack_slot_trace 完全一致 */
    P0DIR &= (uint8)~0x01u;
    P0SEL &= (uint8)~0x01u;
    P0INP &= (uint8)~0x01u;
    P2INP &= (uint8)~0x20u;

    /* --- 点 1: 抬时钟前 (SCL 低) --- */
    P0_1 = 0;
    { volatile uint16 w = 0u; while (w < 200u) { w++; } }
    dig[0] = (uint8)(P0 & 0x01u);   out[0] = v42_pin_adc(0u);   /* SDA */
    dig[1] = (uint8)((P0 >> 1) & 0x01u); out[1] = v42_pin_adc(1u); /* SCL */

    /* --- 点 2: 时钟高中 (ACK 采样点) --- */
    P0_1 = 1;
    { volatile uint16 w = 0u; while (w < 200u) { w++; } }
    dig[2] = (uint8)(P0 & 0x01u);   out[2] = v42_pin_adc(0u);   /* SDA */
    dig[3] = (uint8)((P0 >> 1) & 0x01u); out[3] = v42_pin_adc(1u); /* SCL */

    /* 收尾 */
    P0DIR |= 0x03u;
    P0_1 = 0;
    { volatile uint16 w = 0u; while (w < 30u) { w++; } }
    P0_0 = 1;
    { volatile uint16 w = 0u; while (w < 60u) { w++; } }
    P0_1 = 1;
    { volatile uint16 w = 0u; while (w < 60u) { w++; } }

    return 4u;
}
