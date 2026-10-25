/**
 * @file    clockface.h
 * @brief   墨水屏时钟 (CC2530 + 2.13 寸 104x212 三色屏)
 *
 * 当前阶段: 时间源先"写死"——上电从 CLOCK_START_* 开始, 之后软件每秒走时。
 *          以后接对时程序时, 只要在收到时间后调 clockface_set_time() 即可。
 *
 * 显示: 顶部一行 = 日期 + 中文星期;  中间 = 40x80 大号数字 HH:MM
 *       数字/汉字字模都取自 DA14585 日历工程 (见 clockfont.c)。
 */
#ifndef __CLOCKFACE_H__
#define __CLOCKFACE_H__

#include "hal_types.h"

/* ---------------- 上电初始时间 (对时程序接进来之前的起点) ---------------- */
#define CLOCK_START_HOUR    12
#define CLOCK_START_MIN     34
#define CLOCK_START_SEC     0
#define CLOCK_START_YEAR    2026
#define CLOCK_START_MON     10
#define CLOCK_START_DAY     25

/* ---------------- 时间基准 ----------------
 * 纯软件计时 (和驱动用同一套校准的延时循环), 不会卡死。
 * 刷新时驱动内部花掉的毫秒会被"记账", 由 clockface_delay_ms() 抵扣掉,
 * 所以十几秒的全刷也不会让时钟变慢。
 * (上一版用 Timer1 + 函数指针回调, 实机卡死不刷新, 已改掉) */

/* ---------------- 走时校准 ----------------
 * 1 个"软件秒"的毫秒数。halMcuWaitMs 是按 32MHz 校准的, 但仍有几个 % 误差:
 * 实测约 58s 跳一次分钟 -> 说明 1000ms 实际只有 ~967ms,
 * 于是按 1000/0.967 ≈ 1035 补一点。偏快就调大, 偏慢就调小。
 * (以后接对时程序/加 RTC 就不靠它了) */
#define CLOCK_SECOND_MS     1035

/* ---------------- 刷新节奏 ----------------
 * 1 = 整点全刷 + 平时局刷 (同原 DA14585 工程: 不闪, 整点清一次残影)  ← 默认
 * 0 = 每次全刷 (会闪; 若出现"有第一屏但分钟不再变", 把它改 0 再试)
 */
#define CLOCK_USE_PARTIAL   1

/* ---------------- 画完是否让屏进深睡 ----------------
 * 0 = 长期保持唤醒 (bring-up 阶段默认, 最稳, 刷新也快)
 * 1 = 每次刷完 EPD_Sleep(), 省电, 但下次刷新要重新 EPD_Init (多约 1~4s)
 */
#define CLOCK_SLEEP_AFTER_DRAW  0

/* ---------------- API ---------------- */

/* 走时状态 (Hour 0-23 / Min 0-59 / Sec 0-59) */
extern uint8 clock_hour;
extern uint8 clock_min;
extern uint8 clock_sec;
extern uint16 clock_year;
extern uint8 clock_mon;
extern uint8 clock_day;

/* 设置时间 / 日期 (以后对时程序收到时间后调用) */
void clockface_set_time(uint8 hh, uint8 mm, uint8 ss);
void clockface_set_date(uint16 y, uint8 m, uint8 d);

/* 把当前时间整屏画出来 (全刷, 会闪; 上电/整点用) */
void clockface_draw(void);

/* 把当前时间整屏画出来 (分钟更新用; EPD_PART_MODE=0 时即 ~3s 全刷) */
void clockface_draw_part(void);

/* 走 1 秒; 返回 1 表示分钟变了 (调用方该重绘), 返回 0 表示还在同一分钟 */
uint8 clockface_tick_1s(void);

/* 毫秒延时 (基于 Timer1 硬件计时) */
void clockface_delay_ms(uint16 ms);

/* 时钟主循环: init -> 全刷一次 -> 切局刷 -> 每秒走时, 分钟变化局刷, 整点全刷。
 * 永不返回 */
void clockface_run(void);

#endif /* __CLOCKFACE_H__ */
