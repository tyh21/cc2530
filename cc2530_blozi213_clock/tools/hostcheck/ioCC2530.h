/* 主机侧编译校验用: 把 IAR 8051 的关键字抹掉, 让它能用 gcc 过一遍语法 */
#ifndef IOCC2530_STUB_H
#define IOCC2530_STUB_H
#define __code
#define __xdata
#define __data
#define __near
#endif
