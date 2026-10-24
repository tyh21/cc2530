/**
 * @file    main.c
 * @brief   blozi 213 电子价签 (2.13 寸 HINK-E0213A20, 104x212) - pic_link 组网节点
 *
 * 角色: RF 接收节点, 收网关 (LG213) 转发的图, 流式变换后全刷显示
 *   屏: HINK-E0213A20-A2, 面板 104x212 (13B/行 x 212 行), 横屏 212x104
 *   统一图源 212x104 = 面板原生布局, 直映射显示 (不缩放不嵌入)
 *   驱动序列 = 本板原厂固件逆向 (yuanma_Blozi_213_cc2530.hex), 见 epd2in9.c
 *
 * RF:   CC2530 片内 802.15.4, PAN=0xCAFE ch20, 节点地址 0xB290
 * EPD:  BUSY=P0.4 RST=P0.5 DC=P0.6 CS=P0.7 SCLK=P1.0 SDI=P1.1 PWR=P0.3
 * LED:  P0.0=红 P0.1=蓝 P0.2=绿 (与 290 板同款接线, 低电平点亮)
 * 显示: 全刷 ~3s; 显示后深睡, 下一张图自动唤醒
 */
#include <ioCC2530.h>
#include "basic_rf.h"
#include "pl_node.h"
#include "epd2in13.h"

/* ---------------- 板载 RGB LED (与 290 板同款接线) ----------------
 * 引脚对应 (290 板实测标注: LED 红/绿/蓝 = CC2530 17/18/19 脚):
 *   P0.0 = 红   P0.1 = 蓝   P0.2 = 绿
 * 低电平点亮 (LED 阳极接 VCC); 若实机点不亮, 把 LED_ON_LVL 改成 1 再试
 */
#define LED_ON_LVL      0
#if LED_ON_LVL
#define LED_ON          1
#define LED_OFF         0
#else
#define LED_ON          0
#define LED_OFF         1
#endif

#define LED_RED         P0_0
#define LED_BLUE        P0_1
#define LED_GREEN       P0_2

/* 上电点亮哪几路 (1=亮 0=灭): 默认三色全亮最醒目
 * 想只亮单色就改这里, 例如 红=1 蓝=0 绿=0 */
#define LED_BOOT_R      1
#define LED_BOOT_B      0
#define LED_BOOT_G      0

static void led_init(void)
{
    P0SEL &= ~0x07;                /* P0.0-0.2 设为通用 IO */
    P0DIR |=  0x07;                /* 输出 */
    LED_RED   = LED_OFF;
    LED_BLUE  = LED_OFF;
    LED_GREEN = LED_OFF;
}

/* 1 = 点亮该路 */
static void led_set(uint8 r, uint8 b, uint8 g)
{
    LED_RED   = r ? LED_ON : LED_OFF;
    LED_BLUE  = b ? LED_ON : LED_OFF;
    LED_GREEN = g ? LED_ON : LED_OFF;
}

/* ---------------- EPD 自检 (bring-up 用, 默认关) ----------------
 * 置 1 编译: 上电不进 RF, 循环跑两段自检 (绿色 LED 闪几下 = 第几段):
 *   闪 1 下: 红层(0x26) 写 0x00 + 图案  -> 应为 白底 + 黑框 + 左上黑方块 + 对角线
 *   闪 2 下: 红层(0x26) 写 0xFF + 图案  -> A/B 对照 (本板实测会整片红)
 * 判读:
 *   ① 图案有没有出来      -> 没有 = 命令序列族不对 (告诉我, 换 UC8151D 那套)
 *   ② 黑方块在左上角吗    -> 在右上/右下 = 左右镜像, 改 epd2in9.h 的 EPD_RAM_Y_DESC
 *                         -> 整屏上下颠倒 = 改 EPD_ROT_CW
 */
#define EPD_SELFTEST    0

#if EPD_SELFTEST

static void test_delay_ms(uint16 ms)
{
    uint16 i, j;
    for (i = 0; i < ms; i++)
        for (j = 0; j < 535; j++);
}

static void test_blink(uint8 times)
{
    uint8 k;
    for (k = 0; k < times; k++) {
        LED_GREEN = LED_ON;
        test_delay_ms(200);
        LED_GREEN = LED_OFF;
        test_delay_ms(200);
    }
    test_delay_ms(700);
}

static void epd_selftest(void)
{
    uint8 stage;
    for (;;) {
        for (stage = 0; stage < 2; stage++) {
            EPD_RedNone = stage ? 0xFF : 0x00;       /* A/B: 红层电平 */
            test_blink((uint8)(stage + 1));          /* 闪 1/2 下 = 第 1/2 段 */
            EPD_Init();
            EPD_FrameClear(0xFF);                    /* 白底 */
            EPD_DrawRect(2, 2, 209, 101, 1);         /* 四边黑框 */
            EPD_DrawFilledRect(5, 5, 46, 30, 1);     /* 左上角黑方块 (方向参照) */
            EPD_DrawLine(5, 98, 206, 5, 1);          /* 左下->右上 对角线 */
            EPD_DisplayImage((const uint8 *)EPD_Frame);
            EPD_Sleep();
            test_delay_ms(6000);
        }
    }
}

#endif /* EPD_SELFTEST */

/* ---------------- 系统时钟: 32MHz (BasicRF halMcuWaitUs 按此校准) ---------------- */
static void clock_init(void)
{
    CLKCONCMD &= ~0x40;            /* 选 32MHz 晶振 */
    while (CLKCONSTA & 0x40)       /* 等稳定 */
        ;
    CLKCONCMD &= ~0x47;            /* 分频 1, 主频 32MHz */
}

int main(void)
{
    basicRfCfg_t rf;

    clock_init();

    /* 上电点亮板载 RGB LED (状态指示) */
    led_init();
    led_set(LED_BOOT_R, LED_BOOT_B, LED_BOOT_G);

#if EPD_SELFTEST
    epd_selftest();                /* 自检模式: 永不返回 */
#endif

    /* 上电白底打底: "节点就绪"信号 */
    EPD_Init();
    EPD_Fill(0xFF);
    EPD_Sleep();

    /* BasicRF 初始化: 节点地址 0xB290 */
    rf.myAddr     = PL_ADDR_NODEB29;
    rf.panId      = PL_PAN_ID;
    rf.channel    = PL_CHANNEL;
    rf.ackRequest = TRUE;
    if (basicRfInit(&rf) == FAILED) {
        /* [2026-09-25] RF init 失败不再无声挂死 (屏幕停在白色, 推图无任何
         * 反应, 无法与链路问题区分): 刷黑底白 X 大叉报错画面后停机。
         * 排查方向: 电池电压 (刷白后电压跌落致 RF 校准失败) / 32M 晶振 */
        EPD_Init();
        EPD_FrameClear(0x00);
        EPD_DrawLine(8, 8, 203, 95, 0);        /* 黑底白色大 X (横屏 212x104) */
        EPD_DrawLine(8, 95, 203, 8, 0);
        EPD_DisplayImage((const uint8 *)EPD_Frame);
        EPD_Sleep();
        for (;;)
            ;
    }
    basicRfReceiveOn();

    pl_node_run();                 /* 永不返回 */

    return 0;
}
