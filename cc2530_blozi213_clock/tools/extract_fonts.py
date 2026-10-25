# -*- coding: utf-8 -*-
"""
extract_fonts.py
================
从 DA14585 日历工程 (da14585_213_104x212_calendar) 的字体源码里,
抽取 CC2530 时钟程序要用的字模, 生成 IAR 8051 可用 (__code 常量) 的
clockfont.c / clockfont.h.

被抽取的字体 (原工程格式一致: 前 4 个字节 = 首字符, 宽, 高, 字数):
  EPD_ASCII_7X12      (font8.c)   ASCII 95 个, 7x12
  EPD_40X80_TABLE     (font8.c)   大号数字 '0'..'9' 和 ':', 40x80
  EPD_ASCII_11X16     (font16.c)  ASCII 95 个, 11x16
  EPD_FontUTF8_16x16  (font16CN.c)UTF-8 汉字 66 个, 16x16 (含 星期一二三四五六日年月…)

用法:  python tools/extract_fonts.py
"""
import os
import re
import sys

SRC_ROOT = r"C:\Users\tyh21\Desktop\da14585_213_104x212_calendar\projects\target_apps\ble_examples\ble_app_ota\src"
OUT_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "src")

WANTED = [
    # (输出符号名, 源文件, 取第几个定义(0=第一个, -1=最后一个))
    ("EPD_ASCII_7X12",     "Fonts/font8.c",   0),
    ("EPD_40X80_TABLE",    "Fonts/font8.c",  -1),   # 该文件里有两份, 取最后一份
    ("EPD_ASCII_11X16",    "Fonts/font16.c",  0),
    ("EPD_FontUTF8_16x16", "Fonts/font16CN.c",0),
]

def strip_comments(text):
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    text = re.sub(r"//[^\n]*", " ", text)
    return text

def find_array_body(text, name, which):
    """返回 name[] = { ... } 的第 which 个定义的正文 (不含外层花括号)。"""
    bodies = []
    for m in re.finditer(re.escape(name) + r"\s*\[\s*\]\s*=\s*\{", text):
        i = m.end()
        depth = 1
        j = i
        while j < len(text) and depth > 0:
            c = text[j]
            if c == "{":
                depth += 1
            elif c == "}":
                depth -= 1
            j += 1
        bodies.append(text[i:j - 1])
    if not bodies:
        raise SystemExit("找不到数组: " + name)
    return bodies[which]

TOKEN_RE = re.compile(r"0[xX][0-9a-fA-F]+|\d+|'(\\?.)'|\"(\\?.)\"")

def parse_bytes(body):
    out = []
    for m in TOKEN_RE.finditer(body):
        tok = m.group(0)
        if tok.startswith("'") or tok.startswith('"'):
            ch = tok[1:-1]
            if ch.startswith("\\"):
                ch = {"\\0": "\0", "\\\\": "\\", "\\'": "'", '\\"': '"', "\\n": "\n", "\\r": "\r", "\\t": "\t"}.get(ch, ch[-1])
            out.append(ord(ch))
        elif tok.lower().startswith("0x"):
            out.append(int(tok, 16))
        else:
            out.append(int(tok))
    return out

def c_array(name, data):
    lines = []
    lines.append("__code const uint8 %s[%d] = {" % (name, len(data)))
    for i in range(0, len(data), 16):
        chunk = data[i:i + 16]
        lines.append("    " + " ".join("0x%02X," % b for b in chunk))
    lines.append("};")
    return "\n".join(lines)

def main():
    cache = {}
    def load(rel):
        if rel not in cache:
            path = os.path.join(SRC_ROOT, rel.replace("/", os.sep))
            with open(path, "r", encoding="utf-8", errors="replace") as f:
                cache[rel] = strip_comments(f.read())
        return cache[rel]

    header = []
    body = []
    summary = []

    for name, rel, which in WANTED:
        text = load(rel)
        arr = parse_bytes(find_array_body(text, name, which))
        first, w, h, cnt = arr[0], arr[1], arr[2], arr[3]
        glyph = ((w + 7) // 8) * h
        # UTF-8 字库: 每个字模前多 3 字节 UNICODE 索引 (大端)
        stride = glyph + (3 if "UTF8" in name.upper() else 0)
        expect = 4 + cnt * stride
        if len(arr) != expect:
            print("  [warn] %-20s 长度 %d, 期望 %d (首字符 0x%02X %dx%d x%d)" %
                  (name, len(arr), expect, first, w, h, cnt))
        else:
            print("  [ok]   %-20s %d 字节, 首字符 0x%02X (%r), %dx%d, %d 字" %
                  (name, len(arr), first, chr(first) if 32 <= first < 127 else "?", w, h, cnt))
        header.append("extern __code const uint8 %s[];" % name)
        body.append(c_array(name, arr))
        summary.append((name, first, w, h, cnt, len(arr)))

    hpath = os.path.normpath(os.path.join(OUT_DIR, "clockfont.h"))
    cpath = os.path.normpath(os.path.join(OUT_DIR, "clockfont.c"))

    with open(hpath, "w", encoding="utf-8", newline="\n") as f:
        f.write("""/**
 * @file    clockfont.h
 * @brief   时钟程序字模 (由 tools/extract_fonts.py 自动生成, 勿手改)
 *
 * 字模来源: DA14585 日历工程 (da14585_213_104x212_calendar) 的 Fonts 目录
 *   数字: EPD_40X80_TABLE (40x80 大号数字 + ':')
 *   汉字: EPD_FontUTF8_16x16 (UTF-8, 含 星期一二三四五六日 等)
 *   西文: EPD_ASCII_11X16 / EPD_ASCII_7X12
 * 全部放在 __code (ROM) 里, 不占 RAM。
 */
#ifndef __CLOCKFONT_H__
#define __CLOCKFONT_H__

#include "hal_types.h"

%s

#endif /* __CLOCKFONT_H__ */
""" % "\n".join(header))

    with open(cpath, "w", encoding="utf-8", newline="\n") as f:
        f.write("""/**
 * @file    clockfont.c
 * @brief   时钟程序字模 (由 tools/extract_fonts.py 自动生成, 勿手改)
 *          数据原样抽自 DA14585 日历工程, 仅改成 IAR 8051 的 __code 常量。
 */
#include "clockfont.h"

%s
""" % "\n\n".join(body))

    print("生成:")
    print("  " + hpath)
    print("  " + cpath)
    total = sum(s[5] for s in summary)
    print("字模合计 %d 字节 (ROM)" % total)

if __name__ == "__main__":
    main()
