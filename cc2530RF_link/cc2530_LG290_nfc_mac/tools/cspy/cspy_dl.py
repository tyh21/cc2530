#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
cspy_dl.py — 串口拔掉后仍然能给目标下载固件并让它跑起来。

CSpyBat 走的是 CC Debugger(TI/Chipcon 驱动), **完全不经过串口**,
所以拔掉 USB-TTL 之后只需要一个不 import serial 的下载脚本。

用法(在项目根目录执行):
  <venv python> tools/cspy/cspy_dl.py                # 下载 + 复位运行
  <venv python> tools/cspy/cspy_dl.py --dl-only      # 只下载不跑

venv python (本机唯一装了 pyserial 的那个, 这里其实用不上 pyserial):
  C:/Users/tyh21/.workbuddy/binaries/python/envs/default/Scripts/python.exe
"""
import argparse
import os
import subprocess
import sys

CSPYBAT = r"C:\Program Files (x86)\IAR Systems\Embedded Workbench 8.0\common\bin\CSpyBat.exe"
DEF_PROJ = r"C:\Users\tyh21\Desktop\cc2530\cc2530_RF_link\cc2530_LG290_nfc_mac"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--proj", default=DEF_PROJ)
    ap.add_argument("--timeout", type=int, default=90)
    ap.add_argument("--dl-only", action="store_true",
                    help="只下载(不触发运行), 用于验证烧写是否成功")
    a = ap.parse_args()

    gen = os.path.join(a.proj, "settings", "project.Debug.general.xcl")
    drv = os.path.join(a.proj, "settings", "project.Debug.driver.xcl")
    toolsdir = os.path.join(a.proj, "tools", "cspy")
    mac = os.path.join(toolsdir, "_dl.mac" if a.dl_only else "_run.mac")

    for tag, f in (("general", gen), ("driver", drv), ("macro", mac)):
        if not os.path.isfile(f):
            print("!! 找不到 %s 文件: %s" % (tag, f))
            return 2

    cmd = [CSPYBAT, "--silent", "--timeout", str(a.timeout),
           "--macro", mac, "-f", gen, "--backend", "-f", drv]
    print("[*] CSpyBat: %s" % ("download only" if a.dl_only else "download + run"))
    print("[*] cmd: %s" % " ".join('"%s"' % c if " " in c else c for c in cmd))

    p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                         text=True, errors="replace")
    try:
        out, _ = p.communicate(timeout=a.timeout + 30)
    except subprocess.TimeoutExpired:
        p.kill()
        out, _ = p.communicate()
        print("[!] CSpyBat 超时被杀掉")

    print("---- CSpyBat output ----")
    print(out)
    print("---- exit code = %d ----" % p.returncode)
    return p.returncode


if __name__ == "__main__":
    sys.exit(main())
