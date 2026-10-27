/**
 * @file    epd2in13.c
 * @brief   2.13 寸 EPD 驱动 (104x212), 混合底座: OTP 全刷 + DA14585 局刷实验
 *
 * 全刷 = 原厂路线 (cc2530_RF_link 反汇编版, 实机黑正常):
 *   Init : Reset -> 0x12 -> 0x74/0x7E -> 0x2B(2B VCOM) -> 0x0C(booster)
 *          -> 0x01 -> 0x11 -> 0x44/0x45 -> 0x3C -> 0x18 -> 0x22=0xB1 清底
 *   刷新 : 0x24=图 / 0x26=RedNone -> 0x22=0xC7 -> 0x20   (~3s, OTP 波形)
 * 局刷 (EPD_PART_MODE=1, EPD_PART_DASTYLE=1, DA14585 环境复刻):
 *   首次进入: 0x12 软复位 (清 0x2B/0x0C/0x3D 族原厂电压覆盖) ->
 *             DA14585 Init(FULL) 寄存器组 -> Waveshare 电压轨 (0x03/0x04)
 *             + VCOM 0x2C=0x28 -> 原版局刷 LUT (TP0=A10,B0) -> 0xC0 上电
 *   每分钟  : 重装 LUT/0x37 -> 0x24=帧 / 0x26=~帧 -> 0x22=0x04 触发
 *   整点全刷: EPD_Init() 电源周期重建 OTP 环境 (已验证路径, ~15s)
 */
#include "epd2in13.h"
#include "hal_mcu.h"                 /* halMcuWaitMs: 按 32MHz 校准的精确延时 */

/* ==================================================================
 * 延时 / 记账 (给时钟把"刷新耗掉的时间"算回去)
 * ================================================================== */
static uint32 s_op_ms = 0;

uint16 EPD_DrainOpMs(void)
{
    uint16 v = (s_op_ms > 60000) ? 60000 : (uint16)s_op_ms;
    s_op_ms -= v;
    return v;
}

void EPD_DelayMS(uint16 ms)
{
    s_op_ms += ms;
    halMcuWaitMs(ms);
}

/* ==================================================================
 * 电源 / GPIO / SPI
 * ================================================================== */
void EPD_PowerOn(void)
{
    EPD_PWR = EPD_PWR_OFF_LVL;         /* 先断电放电 (原厂时序) */
    EPD_DelayMS(500);
    EPD_PWR = EPD_PWR_ON_LVL;
    EPD_DelayMS(500);
}

void EPD_PowerOff(void)
{
    EPD_PWR = EPD_PWR_OFF_LVL;
}

void EPD_GpioInit(void)
{
    P0SEL &= ~0xEF;                    /* P0.0-0.3 / 0.5-0.7 通用 IO */
    P1SEL &= ~0x03;

    P0DIR |=  0xEF;                    /* P0.3/5/6/7 + P0.0-0.2 输出 */
    P0DIR &= ~0x10;                    /* P0.4 输入 (BUSY) */
    P1DIR |=  0x03;                    /* P1.0/P1.1 输出 */

    P0INP  =  0x00;                    /* P0 上拉/下拉使能 */
    P2INP &= ~0x20;                    /* P0 选上拉 */

    /* 初始电平: CS=1/DC=1/RST=1 (CS/RST 常低 = 屏永远被复位) */
    EPD_CS   = 1;
    EPD_DC   = 1;
    EPD_SCLK = 0;
    EPD_SDI  = 0;
    EPD_RST  = 1;
    EPD_PWR  = EPD_PWR_OFF_LVL;
}

static void EPD_SendByte(uint8 b)
{
    uint8 i;
    for (i = 0; i < 8; i++)
    {
        EPD_SCLK = 0;
        EPD_SDI  = (b & 0x80) ? 1 : 0;
        b <<= 1;
        EPD_SCLK = 1;                  /* 上升沿锁存 (原厂同款) */
    }
    EPD_SCLK = 0;
}

void EPD_SendCommand(uint8 cmd)
{
    EPD_DC = 0;
    EPD_CS = 0;
    EPD_SendByte(cmd);
    EPD_CS = 1;
    EPD_DC = 1;
}

void EPD_SendData(uint8 dat)
{
    EPD_DC = 1;
    EPD_CS = 0;
    EPD_SendByte(dat);
    EPD_CS = 1;
}

/* ==================================================================
 * 等面板刷完 (高=忙, 带超时; 期间心跳灯每 ~100ms 翻转)
 * ================================================================== */
static void EPD_WaitBusy(uint16 max_ms)
{
    uint16 waited = 0;
#if EPD_BUSY_HEARTBEAT
    uint8  hb = 0;
#endif

    while (EPD_BUSY)
    {
        EPD_DelayMS(10);
        waited += 10;
#if EPD_BUSY_HEARTBEAT
        if (++hb >= 10)
        {
            hb = 0;
            EPD_BUSY_HB_PIN = (uint8)!EPD_BUSY_HB_PIN;
        }
#endif
        if (waited >= max_ms)
            break;
    }
    EPD_DelayMS(10);
}

static void EPD_Reset(void)
{
    EPD_RST = 1;
    EPD_DelayMS(10);
    EPD_RST = 0;
    EPD_DelayMS(10);
    EPD_RST = 1;
    EPD_DelayMS(10);
}

/* ==================================================================
 * 外部全刷 LUT (照抄 DA14585 EPD_2IN13_V2_lut_full_update, 76 字节)
 *   前 70 字节 = LUT0~LUT4 + TP0~TP6
 *   末 6 字节 = 0x03/0x04/0x3A/0x3B 的电压与时序 (0x15, 41,A8,32, 30, 0A)
 * 波形为三段振荡 (VSL/VSH/VSL), repeat=2, DA14585 上全刷约 1s。
 * ================================================================== */
#if EPD_FULL_USE_LUT
static __code const uint8 EPD_LutFull[76] = {
    0x80, 0x60, 0x40, 0x00, 0x00, 0x00, 0x00,   /* LUT0: BB */
    0x10, 0x60, 0x20, 0x00, 0x00, 0x00, 0x00,   /* LUT1: BW */
    0x80, 0x60, 0x40, 0x00, 0x00, 0x00, 0x00,   /* LUT2: WB */
    0x10, 0x60, 0x20, 0x00, 0x00, 0x00, 0x00,   /* LUT3: WW */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,   /* LUT4: VCOM */
    0x03, 0x03, 0x00, 0x00, 0x02,               /* TP0 */
    0x09, 0x09, 0x00, 0x00, 0x02,               /* TP1 */
    0x03, 0x03, 0x00, 0x00, 0x02,               /* TP2 */
    0x00, 0x00, 0x00, 0x00, 0x00,               /* TP3 */
    0x00, 0x00, 0x00, 0x00, 0x00,               /* TP4 */
    0x00, 0x00, 0x00, 0x00, 0x00,               /* TP5 */
    0x00, 0x00, 0x00, 0x00, 0x00,               /* TP6 */
    0x15, 0x41, 0xA8, 0x32, 0x30, 0x0A,         /* 尾部: 0x03/0x04/0x3A/0x3B */
};
#endif

/* ==================================================================
 * RF init 寄存器组 (原厂 init 的寄存器部分, 不含复位/电源/清底)
 * 供上电 init 与"局刷后回 OTP"两处复用
 * ================================================================== */
static void EPD_RfRegsLoad(void)
{
    EPD_SendCommand(0x74);             /* 温度传感器写 */
    EPD_SendData(0x54);

    EPD_SendCommand(0x7E);             /* 外部温度传感器读 */
    EPD_SendData(0x3B);

    EPD_SendCommand(0x2B);             /* VCOM 设置 (原厂 2 字节) */
    EPD_SendData(0x04);
    EPD_SendData(0x63);

    EPD_SendCommand(0x0C);             /* booster 软启动 (原厂; 偏淡的关键补丁) */
    EPD_SendData(0x8B);
    EPD_SendData(0x9C);
    EPD_SendData(0x96);
    EPD_SendData(0x0F);

    EPD_SendCommand(0x01);             /* 驱动输出控制: 212 条 gate */
    EPD_SendData(0xD3);                /* 211 = 212-1 */
    EPD_SendData(0x00);
    EPD_SendData(0x00);

    EPD_SendCommand(0x11);             /* 数据入口 (见 EPD_RAM_Y_DESC) */
#if EPD_RAM_Y_DESC
    EPD_SendData(0x01);                /* X增 Y减 (原厂顺序) */
#else
    EPD_SendData(0x03);
#endif

    EPD_SendCommand(0x44);             /* RAM X 窗口: 0 ~ 12 */
    EPD_SendData(0x00);
    EPD_SendData(0x0C);

    EPD_SendCommand(0x45);             /* RAM Y 窗口 */
#if EPD_RAM_Y_DESC
    EPD_SendData(0xD3);                /* 211 -> 0 */
    EPD_SendData(0x00);
    EPD_SendData(0x00);
    EPD_SendData(0x00);
#else
    EPD_SendData(0x00);
    EPD_SendData(0x00);
    EPD_SendData(0xD3);
    EPD_SendData(0x00);
#endif

    EPD_SendCommand(0x3C);             /* 边框波形 */
    EPD_SendData(0x01);

    EPD_SendCommand(0x18);             /* 内置温度传感器 */
    EPD_SendData(0x80);
}

/* ---------------- 上电初始化 (原厂序列, 含 OTP 清底) ----------------
 * [原厂 0x0CE2-0x0D3F] 清底后有一段按 0x1B 感测值条件配置的电压寄存器
 * (0x3D/0x3E/0x3F) —— 这才是这颗 IC 的 gate/source 电压体系, 档位比
 * Waveshare 的 0x03/0x04 高; OTP 全刷黑得饱满靠的就是它。
 * 这里取原厂常见分支的取值无条件写入。
 */
static void EPD_OemVoltLoad(void)
{
    EPD_SendCommand(0x3D);             /* 原厂条件电压 (感测后写) */
    EPD_SendData(0x09);
    EPD_SendData(0x09);
    EPD_SendCommand(0x3E);
    EPD_SendData(0x01);
    EPD_SendData(0x11);
    EPD_SendData(0x0C);
    EPD_SendCommand(0x3F);
    EPD_SendData(0x07);
}

static uint8 s_inited = 0;

void EPD_Init(void)
{
    EPD_GpioInit();
    EPD_PowerOn();

    EPD_Reset();
    EPD_WaitBusy(3000);

    EPD_SendCommand(0x12);             /* 软复位 */
    EPD_WaitBusy(3000);

    EPD_RfRegsLoad();

#if EPD_FULL_USE_LUT
    {   /* ---- 外部全刷 LUT 环境 (照 DA14585 Init(FULL)) ---- */
        uint8 i;

        EPD_SendCommand(0x2C);         /* VCOM (DA14585 全刷值 0x50) */
        EPD_SendData(0x50);

        EPD_SendCommand(0x32);         /* 全刷 LUT: 70 字节 */
        for (i = 0; i < 70; i++)
            EPD_SendData(EPD_LutFull[i]);

        EPD_SendCommand(0x03);         /* 尾部电压/时序 (LUT[70..75]) */
        EPD_SendData(EPD_LutFull[70]);
        EPD_SendCommand(0x04);
        EPD_SendData(EPD_LutFull[71]);
        EPD_SendData(EPD_LutFull[72]);
        EPD_SendData(EPD_LutFull[73]);
        EPD_SendCommand(0x3A);
        EPD_SendData(EPD_LutFull[74]);
        EPD_SendCommand(0x3B);
        EPD_SendData(EPD_LutFull[75]);

        EPD_SendCommand(0x22);         /* 用外部 LUT 清一次底色 */
        EPD_SendData(0xC4);
        EPD_SendCommand(0x20);
        EPD_WaitBusy(8000);
    }
#else
    EPD_SendCommand(0x22);             /* OTP 波形清一次底色 */
    EPD_SendData(0xB1);
    EPD_SendCommand(0x20);
    EPD_WaitBusy(8000);

    EPD_OemVoltLoad();                 /* 原厂条件电压 (照抄 0x0D10-0x0D3D) */
#endif

    s_inited = 1;
}

static void EPD_EnsureInit(void)
{
    if (!s_inited)
        EPD_Init();
}

/* ==================================================================
 * RAM 指针 (0x24/0x26 共用; 每次 写 RAM 前重设, 不依赖回卷)
 * ================================================================== */
static void EPD_SetPtrStart(void)
{
    EPD_SendCommand(0x4E);             /* RAM X 指针 = 0 */
    EPD_SendData(0x00);
    EPD_SendCommand(0x4F);             /* RAM Y 指针 */
#if EPD_RAM_Y_DESC
    EPD_SendData(0xD3);                /* 211 */
    EPD_SendData(0x00);
#else
    EPD_SendData(0x00);
    EPD_SendData(0x00);
#endif
}

/* ==================================================================
 * 局刷 (EPD_PART_MODE=1)
 *   EPD_PART_DASTYLE=1 (本轮): 【DA14585 同款环境复刻】
 *     实测: 原厂电压体系下 LUT 0x80(白向轨) 能翻转黑->白, 0x40(黑向轨)
 *     只能巩固已黑、翻不动白->黑 ("黑的能推白, 白的推不黑")。
 *     DA14585 同屏局刷正常 = Waveshare 电压轨 (0x03=0x15/0x04=41,A8,32)
 *     + VCOM 0x2C=0x28, 且芯片里无 0x2B/0x0C/0x3D 残留; 之前试 Waveshare
 *     轨失败时恰还带着这堆残留。本路径首次进入先 0x12 软复位清干净。
 *   其余 (AALL/WAVEFULL/partial): 历史实验对照路径。
 * ================================================================== */
#if EPD_PART_MODE && !EPD_PART_OTP
static uint8 s_in_part_env = 0;         /* 当前芯片处于局刷环境 (外部 LUT) */

#if EPD_PART_DASTYLE

/* 局刷 LUT (基于 DA14585 原版, 已按实测修正)。
 *
 * 关键结论 (2026-10-25 实测):
 *   原版每行只有 byte0 有码 + TP0.B=0, 即每个转换只有 1 个脉冲:
 *     LUT1(BW) = 0x80 (VSL 推白) —— 够用, 黑->白正常;
 *     LUT2(WB) = 0x40 (VSH 推黑) —— 力度不足, 白->黑发灰/不驱动。
 *   LUT 行的 7 个字节依次由 TP0~TP6 的 A 相驱动 (byteN -> TPN.A),
 *   每个字节含 4 个 2bit 电压码 (00=VSS 01=VSH 10=VSL)。
 *
 * 本版做法: 把 BW/WB 都扩成 TP0/TP1/TP2 三段同向脉冲, 推黑/推白力度
 * 提升到接近 OTP 全刷; BB(保持黑)、WW(保持白) 也各给同向脉冲加固。
 * 实测: 双向饱满, 且原轮廓残影消失 (残影本是 BB 推黑不足造成的)。 */
static __code const uint8 EPD_LutPart[70] = {
    0x40, 0x40, 0x40, 0x00, 0x00, 0x00, 0x00,   /* LUT0: BB -> TP0/TP1/TP2 三段 VSH (保持黑加固) */
    0x80, 0x80, 0x80, 0x00, 0x00, 0x00, 0x00,   /* LUT1: BW -> TP0/TP1/TP2 三段 VSL (推白) */
    0x40, 0x40, 0x40, 0x00, 0x00, 0x00, 0x00,   /* LUT2: WB -> TP0/TP1/TP2 三段 VSH (推黑) */
    0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,   /* LUT3: WW -> TP0 一次 VSL (保持白加固) */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,   /* LUT4: VCOM */
    EPD_PART_ALEN, 0x00, 0x00, 0x00, 0x00,  /* TP0: A 相驱动 LUT1(BW)+LUT2(WB) 的 VSH/VSL */
    EPD_PART_ALEN, 0x00, 0x00, 0x00, 0x00,  /* TP1: A 相第二段脉冲 */
    EPD_PART_ALEN, 0x00, 0x00, 0x00, 0x00,  /* TP2: A 相第三段脉冲 (LUT1/LUT2 byte2) */
    0x00, 0x00, 0x00, 0x00, 0x00,               /* TP3 */
    0x00, 0x00, 0x00, 0x00, 0x00,               /* TP4 */
    0x00, 0x00, 0x00, 0x00, 0x00,               /* TP5 */
    0x00, 0x00, 0x00, 0x00, 0x00,               /* TP6 */
};

/* 进入 DA14585 同款局刷环境。
 * 首次: 0x12 软复位 (清掉 0x2B/0x0C/0x3D 族原厂电压覆盖, 关键!)
 *       -> DA14585 Init(FULL) 寄存器组 -> Init(PART) 电压 (Waveshare 轨+VCOM)。
 * 之后: 只重装 LUT/0x37/上电 (DA14585 每周期 PartSetup 同款)。 */
static void EPD_PartEnter(void)
{
    uint8 i;

    if (!s_in_part_env)
    {
        EPD_SendCommand(0x12);         /* 软复位: 电压寄存器全回默认 */
        EPD_WaitBusy(3000);

        /* DA14585 Init(FULL) 寄存器组 (无 0x2B / 无 0x0C / 无 0x18) */
        EPD_SendCommand(0x74);
        EPD_SendData(0x54);
        EPD_SendCommand(0x7E);
        EPD_SendData(0x3B);
        EPD_SendCommand(0x01);
        EPD_SendData(0xD3);
        EPD_SendData(0x00);
        EPD_SendData(0x00);
        EPD_SendCommand(0x11);
        EPD_SendData(0x01);
        EPD_SendCommand(0x44);
        EPD_SendData(0x00);
        EPD_SendData(0x0C);
        EPD_SendCommand(0x45);
        EPD_SendData(0xD3);
        EPD_SendData(0x00);
        EPD_SendData(0x00);
        EPD_SendData(0x00);
        EPD_SendCommand(0x3C);
        EPD_SendData(0x01);

        /* DA14585 Init(PART) 电压: Waveshare 轨 + 局刷 VCOM */
        EPD_SendCommand(0x2C);
        EPD_SendData(0x28);
        EPD_SendCommand(0x03);
        EPD_SendData(0x15);
        EPD_SendCommand(0x04);
        EPD_SendData(0x41);
        EPD_SendData(0xA8);
        EPD_SendData(0x32);
        EPD_SendCommand(0x3A);
        EPD_SendData(0x30);
        EPD_SendCommand(0x3B);
        EPD_SendData(0x0A);

        s_in_part_env = 1;
    }

    /* 每周期重装 (DA14585 PartSetup 同款) */
    EPD_SendCommand(0x32);             /* 局刷波形 */
    for (i = 0; i < 70; i++)
        EPD_SendData(EPD_LutPart[i]);

    EPD_SendCommand(0x37);             /* Write Register for Display Option */
#if EPD_PART_PP10
    /* [开关3] 10 字节写法 (规格书字段序 A,B~E,F,G~J), 0x40 落 F 字节 = PingPong */
    EPD_SendData(0x00);                /* A: Spare VCOM OTP selection */
    EPD_SendData(0x00);                /* B: Display Mode for WS[7:0]   */
    EPD_SendData(0x00);                /* C: Display Mode for WS[15:8]  */
    EPD_SendData(0x00);                /* D: Display Mode for WS[23:16] */
    EPD_SendData(0x00);                /* E: Display Mode for WS[31:24] */
    EPD_SendData(0x40);                /* F: F[6]=1 -> PingPong enable   */
    EPD_SendData(0x00);                /* G */
    EPD_SendData(0x00);                /* H */
    EPD_SendData(0x00);                /* I */
    EPD_SendData(0x00);                /* J: module ID / waveform version */
#else
    /* 原 7 字节写法: 0x40 落 E 字节 (WS[31:24]), PingPong 未使能 (对照用) */
    EPD_SendData(0x00);
    EPD_SendData(0x00);
    EPD_SendData(0x00);
    EPD_SendData(0x00);
    EPD_SendData(0x40);
    EPD_SendData(0x00);
    EPD_SendData(0x00);
#endif

#if EPD_PART_BORDER_EN
    /* [开关2] 边框波形: 0x80 = VBD 固定 VCOM, 局刷时边框不参与闪烁 */
    EPD_SendCommand(0x3C);
    EPD_SendData(EPD_PART_BORDER);
#endif

    EPD_SendCommand(0x22);
    EPD_SendData(0xC0);                /* 上电, 不刷图 */
    EPD_SendCommand(0x20);
    EPD_WaitBusy(3000);
}

/* 退出局刷环境回 OTP 全刷: 完整 EPD_Init (电源周期+原厂 init+清底),
 * 已验证路径; 代价 ~15s, 整点一次可接受 */
static void EPD_OtpReenter(void)
{
    EPD_Init();
    s_in_part_env = 0;
}

#else /* legacy: 历史实验对照路径 */
#if EPD_PART_AALL
/* 全 A 相强推表 (DA14585 机制对齐):
 * OLD=~帧 下每个像素都走 BW/WB, 再把 BB/WW 也指向 A 相兜底;
 * A 相 = TP0.A, 时长 EPD_PART_ALEN。 */
static __code const uint8 EPD_LutPart[70] = {
    0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,   /* LUT0: BB -> A 相 */
    0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,   /* LUT1: BW -> A 相 */
    0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,   /* LUT2: WB -> A 相 */
    0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,   /* LUT3: WW -> A 相 */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,   /* LUT4: VCOM */
    EPD_PART_ALEN, 0x00, 0x00, 0x00, 0x00,      /* TP0: A=ALEN, 1 次不重复 */
    0x00, 0x00, 0x00, 0x00, 0x00,               /* TP1 */
    0x00, 0x00, 0x00, 0x00, 0x00,               /* TP2 */
    0x00, 0x00, 0x00, 0x00, 0x00,               /* TP3 */
    0x00, 0x00, 0x00, 0x00, 0x00,               /* TP4 */
    0x00, 0x00, 0x00, 0x00, 0x00,               /* TP5 */
    0x00, 0x00, 0x00, 0x00, 0x00,               /* TP6 */
};
#elif !EPD_PART_WAVEFULL
/* 局刷 LUT (Waveshare 2.13V2 原表; TP0 的 B 相时长受 EPD_PART_WB_B 控制) */
static __code const uint8 EPD_LutPart[70] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,   /* LUT0: BB */
    0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,   /* LUT1: BW */
    0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,   /* LUT2: WB */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,   /* LUT3: WW */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,   /* LUT4: VCOM */
#if EPD_PART_WB_B
    0x0A, 0x0A, 0x00, 0x00, 0x00,               /* TP0: A=10(推白) B=10(推黑) */
#else
    0x0A, 0x00, 0x00, 0x00, 0x00,               /* TP0: 原厂原样 (B=0) */
#endif
    0x00, 0x00, 0x00, 0x00, 0x00,               /* TP1 */
    0x00, 0x00, 0x00, 0x00, 0x00,               /* TP2 */
    0x00, 0x00, 0x00, 0x00, 0x00,               /* TP3 */
    0x00, 0x00, 0x00, 0x00, 0x00,               /* TP4 */
    0x00, 0x00, 0x00, 0x00, 0x00,               /* TP5 */
    0x00, 0x00, 0x00, 0x00, 0x00,               /* TP6 */
};
#elif EPD_PART_WAVEFULL
/* 完整全刷波形表 (Waveshare 2.13V2 全刷 LUT):
 * 目标黑走 LUT0/2 = A3+B9+B3 (黑向, 已实证能推黑); 目标白走 LUT1/3 = B9。
 * 配原厂高压档, 黑白都应饱满; 时长随 EPD_WAVE_REPEAT 旋钮。 */
static __code const uint8 EPD_LutPart[70] = {
    0x80, 0x60, 0x40, 0x00, 0x00, 0x00, 0x00,   /* LUT0: BB */
    0x10, 0x60, 0x20, 0x00, 0x00, 0x00, 0x00,   /* LUT1: BW */
    0x80, 0x60, 0x40, 0x00, 0x00, 0x00, 0x00,   /* LUT2: WB */
    0x10, 0x60, 0x20, 0x00, 0x00, 0x00, 0x00,   /* LUT3: WW */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,   /* LUT4: VCOM */
    0x03, 0x03, 0x00, 0x00, EPD_WAVE_REPEAT,    /* TP0: repeat 加大对齐 OTP 强度 */
    0x09, 0x09, 0x00, 0x00, EPD_WAVE_REPEAT,    /* TP1 */
    0x03, 0x03, 0x00, 0x00, EPD_WAVE_REPEAT,    /* TP2 */
    0x00, 0x00, 0x00, 0x00, 0x00,               /* TP3 */
    0x00, 0x00, 0x00, 0x00, 0x00,               /* TP4 */
    0x00, 0x00, 0x00, 0x00, 0x00,               /* TP5 */
    0x00, 0x00, 0x00, 0x00, 0x00,               /* TP6 */
};
#endif /* EPD_PART_AALL / WAVEFULL 分支结束 */

/* 进入局刷环境: 不复位; 【电压保持原厂档不动】
 * (之前在这里补写 Waveshare 的 0x2C=0x28/0x03/0x04 —— 把原厂高电压覆盖成
 * 了 Waveshare 低压, 黑推不满就是它; 现在只写时序参数和波形表) */
static void EPD_PartEnter(void)
{
    uint8 i;

    EPD_SendCommand(0x3A);             /* Dummy line (波形节拍) */
    EPD_SendData(0x30);
    EPD_SendCommand(0x3B);             /* Gate time */
    EPD_SendData(0x0A);

    EPD_SendCommand(0x32);             /* 局刷波形 */
    for (i = 0; i < 70; i++)
        EPD_SendData(EPD_LutPart[i]);

    EPD_SendCommand(0x37);
    EPD_SendData(0x00);
    EPD_SendData(0x00);
    EPD_SendData(0x00);
    EPD_SendData(0x00);
    EPD_SendData(0x40);
    EPD_SendData(0x00);
    EPD_SendData(0x00);

    EPD_SendCommand(0x22);
    EPD_SendData(0xC0);                /* 上电, 不刷图 */
    EPD_SendCommand(0x20);
    EPD_WaitBusy(3000);

    s_in_part_env = 1;
}

/* 退出局刷环境, 回到 OTP 全刷: 软复位清 LUT 寄存器 + 重发原厂寄存器组
 * (软复位不清 RAM, 屏面内容不动; 电压段也要补回) */
static void EPD_OtpReenter(void)
{
    EPD_SendCommand(0x12);
    EPD_WaitBusy(3000);
    EPD_RfRegsLoad();
    EPD_OemVoltLoad();
    s_in_part_env = 0;
}
#endif /* EPD_PART_DASTYLE */

/* 写 RAM 前的环境守卫 (OTP 显示路径用; 局刷路径用 EPD_EnsureInit) */
static void EPD_RamGuard(void)
{
    if (s_in_part_env)
        EPD_OtpReenter();
    EPD_EnsureInit();
}
#else /* 无局刷 LUT 环境 (OTP 局刷或纯全刷): 只需保证已 init */
static void EPD_RamGuard(void)
{
    EPD_EnsureInit();
}
#endif

/* ==================================================================
 * 写整屏 + 刷新
 * ================================================================== */
static void EPD_WriteScreenBW(const uint8 *img)
{
    uint16 i;

    EPD_SetPtrStart();
    EPD_SendCommand(0x24);             /* 黑白层 */
    for (i = 0; i < EPD_IMG_BYTES; i++)
        EPD_SendData(img[i]);

    EPD_SetPtrStart();
    EPD_SendCommand(0x26);             /* 0x26: OTP 波形下=红层, 不显红 */
    for (i = 0; i < EPD_IMG_BYTES; i++)
        EPD_SendData(EPD_RedNone);
}

void EPD_Display(void)                 /* 全刷: LUT 模式 ~1s / OTP 模式 ~3s */
{
    EPD_SendCommand(0x22);
#if EPD_FULL_USE_LUT
    EPD_SendData(0xC4);                /* 外部全刷 LUT (DA14585 同款) */
#else
    EPD_SendData(0xC7);                /* OTP 出厂波形 */
#endif
    EPD_SendCommand(0x20);
    EPD_WaitBusy(20000);
}

/* 红层"不显示红"的电平 (OTP 路径用) */
uint8 EPD_RedNone = 0x00;

/* ---------------- 对外显示接口 ---------------- */
void EPD_Fill(uint8 color)             /* 0xFF=白 0x00=黑 */
{
    uint16 i;

    EPD_RamGuard();
    EPD_SetPtrStart();
    EPD_SendCommand(0x24);
    for (i = 0; i < EPD_IMG_BYTES; i++)
        EPD_SendData(color);
    EPD_SetPtrStart();
    EPD_SendCommand(0x26);
    for (i = 0; i < EPD_IMG_BYTES; i++)
        EPD_SendData(EPD_RedNone);
    EPD_Display();
}

void EPD_DisplayImage(const uint8 *img)
{
    EPD_RamGuard();
    EPD_WriteScreenBW(img);
    EPD_Display();
}

static void EPD_SyncOldFrame(void);   /* 前向声明: 定义在帧缓冲处 */
extern __xdata uint8 EPD_OldFrame[EPD_IMG_BYTES];  /* 旧帧缓冲 (定义在文件下部) */

void EPD_ShowFrameFull(void)
{
    EPD_RamGuard();
    EPD_WriteScreenBW((const uint8 *)EPD_Frame);
    EPD_Display();
    EPD_SyncOldFrame();                /* 屏幕 = 新帧, 更新 OLD 基线 */
}

void EPD_ShowFramePart(void)
{
#if !EPD_PART_MODE
    EPD_ShowFrameFull();               /* 全刷模式: ~3s 整屏重写 */
#elif EPD_PART_OTP
    /* ---- OTP 局刷: 寄存器保持原厂状态, 波形走 OTP ----
     * 0xC0 上电 (clock+analog) -> 写 RAM -> 0x04 触发。
     * 0x26 内容见 EPD_PART_OLD_INV (~帧 / 不显红)。
     * 刷完耗时长短可分辨 OTP 里走的是哪张表: ~1-2s=快速表, ~3s=全刷表。 */
    uint16 i;

    EPD_RamGuard();

    EPD_SendCommand(0x22);
    EPD_SendData(0xC0);                /* clock + analog 上电 */
    EPD_SendCommand(0x20);
    EPD_WaitBusy(3000);

    EPD_SetPtrStart();
    EPD_SendCommand(0x24);             /* NEW = 新帧 */
    for (i = 0; i < EPD_IMG_BYTES; i++)
        EPD_SendData(EPD_Frame[i]);

    EPD_SetPtrStart();
    EPD_SendCommand(0x26);
#if EPD_PART_OLD_INV
    for (i = 0; i < EPD_IMG_BYTES; i++)
        EPD_SendData((uint8)~EPD_Frame[i]);   /* OLD = ~新帧 (DA14585 语义) */
#else
    for (i = 0; i < EPD_IMG_BYTES; i++)
        EPD_SendData(EPD_RedNone);            /* 不显红 (若 ~帧 出红改这档) */
#endif

    EPD_SendCommand(0x22);
    EPD_SendData(0x04);                /* display update (无寄存器 LUT -> OTP) */
    EPD_SendCommand(0x20);
    EPD_WaitBusy(10000);
#else
    /* ---- 局刷路径 (DASTYLE = DA14585 环境 / legacy = 实验对照) ---- */
    uint16 i;

    EPD_EnsureInit();                  /* 不走 RamGuard: 局刷环境要保持 */

    EPD_PartEnter();

    EPD_SetPtrStart();
    EPD_SendCommand(0x24);             /* NEW = 新帧 */
    for (i = 0; i < EPD_IMG_BYTES; i++)
        EPD_SendData(EPD_Frame[i]);

    EPD_SetPtrStart();
    EPD_SendCommand(0x26);             /* OLD = 上一帧真实屏幕内容 (非 ~新帧) */
    for (i = 0; i < EPD_IMG_BYTES; i++)
        EPD_SendData(EPD_OldFrame[i]);

    EPD_SendCommand(0x22);
    EPD_SendData(EPD_PART_TRIG);       /* [开关1] 见 epd2in13.h: 0x0C/0xCF/0xFF=Mode2 差分 */
    EPD_SendCommand(0x20);
    EPD_WaitBusy(10000);

    EPD_SyncOldFrame();                /* 屏幕上现在 = 新帧, 更新 OLD 基线 */
#endif
}

void EPD_ShowFrame(void)
{
    EPD_ShowFrameFull();
}

void EPD_Sleep(void)
{
    EPD_SendCommand(0x10);             /* deep sleep (0x01=retain RAM) */
    EPD_SendData(0x01);
    s_inited = 0;                      /* 深睡后寄存器状态不保证, 下次重新 init */
#if EPD_PART_MODE && !EPD_PART_OTP
    s_in_part_env = 0;                 /* 局刷环境同样失效 */
#endif
}

/* ==================================================================
 * 横屏帧缓冲 (配 EPD_RAM_Y_DESC=1: 帧第 j 组 = 屏列 j, 字节位 = src 行)
 * 排布与 pl_proto.c 的直接索引一致: idx = x*13 + (y>>3)
 * ================================================================== */
__xdata uint8 EPD_Frame[EPD_IMG_BYTES];

/* 上一帧"屏幕实际显示"的内容 (局刷 0x26 = 真实 OLD)。
 * 局刷/全刷显示完成后由 EPD_SyncOldFrame() 从 EPD_Frame 同步。
 * 与 EPD_Frame 极性一致: 1=白, 0=黑。 */
__xdata uint8 EPD_OldFrame[EPD_IMG_BYTES];

static void EPD_SyncOldFrame(void)
{
    uint16 i;
    for (i = 0; i < EPD_IMG_BYTES; i++)
        EPD_OldFrame[i] = EPD_Frame[i];
}

void EPD_FrameClear(uint8 color)
{
    uint16 i;
    for (i = 0; i < EPD_IMG_BYTES; i++)
        EPD_Frame[i] = color;
}

void EPD_SetPixel(uint16 x, uint8 y, uint8 black)
{
    uint16 idx;
    uint8  mask;

    if (x >= EPD_LAND_W || y >= EPD_LAND_H)
        return;

#if EPD_RAM_Y_DESC
    /* Y 从 211 递减写入: 帧第 j 组 = 屏列 x=j */
    idx  = (uint16)x * EPD_LINE_BYTES + (y >> 3);
#else
    /* Y 从 0 递增写入: 帧第 j 组 = gate j (屏列 x=211-j) */
    idx  = (uint16)(EPD_LAND_W - 1 - x) * EPD_LINE_BYTES + (y >> 3);
#endif
    mask = (uint8)(0x80 >> (y & 0x07));
    if (black)
        EPD_Frame[idx] &= (uint8)~mask;   /* 0=黑 */
    else
        EPD_Frame[idx] |= mask;           /* 1=白 */
}

/* ---------------- 画图 API (横屏坐标 x:0-211, y:0-103) ---------------- */
void EPD_DrawLine(int x0, int y0, int x1, int y1, uint8 black)
{
    int dx = x1 - x0 >= 0 ? x1 - x0 : x0 - x1;
    int sx = x0 < x1 ? 1 : -1;
    int dy = y1 - y0 <= 0 ? y1 - y0 : y0 - y1;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    while (1)
    {
        EPD_SetPixel((uint16)x0, (uint8)y0, black);
        if (x0 == x1 && y0 == y1) break;
        {
            int e2 = 2 * err;
            if (e2 >= dy) { err += dy; x0 += sx; }
            if (e2 <= dx) { err += dx; y0 += sy; }
        }
    }
}

void EPD_DrawRect(int x0, int y0, int x1, int y1, uint8 black)
{
    EPD_DrawLine(x0, y0, x1, y0, black);
    EPD_DrawLine(x0, y1, x1, y1, black);
    EPD_DrawLine(x0, y0, x0, y1, black);
    EPD_DrawLine(x1, y0, x1, y1, black);
}

void EPD_DrawFilledRect(int x0, int y0, int x1, int y1, uint8 black)
{
    int x, y;
    if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
    if (y0 > y1) { int t = y0; y0 = y1; y1 = t; }
    for (y = y0; y <= y1; y++)
        for (x = x0; x <= x1; x++)
            EPD_SetPixel((uint16)x, (uint8)y, black);
}

void EPD_DrawCircle(int xc, int yc, int r, uint8 black)
{
    int x = -r, y = 0, err = 2 - 2 * r, e2;
    do {
        EPD_SetPixel((uint16)(xc - x), (uint8)(yc + y), black);
        EPD_SetPixel((uint16)(xc + x), (uint8)(yc + y), black);
        EPD_SetPixel((uint16)(xc + x), (uint8)(yc - y), black);
        EPD_SetPixel((uint16)(xc - x), (uint8)(yc - y), black);
        e2 = err;
        if (e2 <= y) { err += ++y * 2 + 1; if (-x == y && e2 <= x) e2 = 0; }
        if (e2 > x)  { err += ++x * 2 + 1; }
    } while (x <= 0);
}

void EPD_DrawFilledCircle(int xc, int yc, int r, uint8 black)
{
    int x = -r, y = 0, err = 2 - 2 * r, e2;
    do {
        EPD_DrawLine(xc + x, yc + y, xc - x, yc + y, black);
        EPD_DrawLine(xc + x, yc - y, xc - x, yc - y, black);
        e2 = err;
        if (e2 <= y) { err += ++y * 2 + 1; if (-x == y && e2 <= x) e2 = 0; }
        if (e2 > x)  { err += ++x * 2 + 1; }
    } while (x <= 0);
}
