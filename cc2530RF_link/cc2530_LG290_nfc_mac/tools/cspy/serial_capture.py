#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
serial_capture.py — 无 GUI 下载/复位 CC2530 并同进程抓串口(COM13@115200)。

背景/用法(本工程已验证):
  CSpyBat 用 CC Debugger 复位运行目标; 必须在**先打开串口**再触发复位,
  否则会错过 banner。本脚本把 pyserial + subprocess 合到一个进程里做闭环。

用法:
  python serial_capture.py [--port COM13] [--baud 115200] [--secs 12]
                           [--proj <工程目录>] [--mode run|dl]

  --mode run : (默认) 复位并运行固件, 抓 --secs 秒串口输出
  --mode dl  : 只下载(不运行), 验证烧写成功
"""
import argparse, subprocess, sys, time, os

try:
    import serial
except ImportError:
    print("!! 需要 pyserial。请用工程的 venv: "
          "C:/Users/tyh21/.workbuddy/binaries/python/envs/default/Scripts/python.exe")
    sys.exit(2)

CSPYBAT = r"C:\Program Files (x86)\IAR Systems\Embedded Workbench 8.0\common\bin\CSpyBat.exe"
DEF_PROJ = r"C:\Users\tyh21\Desktop\cc2530\cc2530_RF_link\cc2530_LG290_nfc_mac"


def cspy_cmd(proj, mac, extra=None):
    gen = os.path.join(proj, "settings", "project.Debug.general.xcl")
    drv = os.path.join(proj, "settings", "project.Debug.driver.xcl")
    cmd = [CSPYBAT, "--silent", "--timeout", "25"]
    if mac:
        cmd += ["--macro", mac]
    if extra:
        cmd += extra
    cmd += ["-f", gen, "--backend", "-f", drv]
    return cmd


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="COM13")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--secs", type=float, default=12.0)
    ap.add_argument("--proj", default=DEF_PROJ)
    ap.add_argument("--mode", default="run", choices=["run", "dl"])
    ap.add_argument("--out", default=None, help="把串口原始数据存到文件")
    ap.add_argument("--cspy-timeout", type=int, default=90,
                    help="CSpyBat 的 --timeout(秒); 必须 >= secs")
    a = ap.parse_args()

    toolsdir = os.path.join(a.proj, "tools", "cspy")
    mac = os.path.join(toolsdir, "_run.mac" if a.mode == "run" else "_dl.mac")

    print("[*] port=%s baud=%d mode=%s secs=%s cspy_timeout=%d"
          % (a.port, a.baud, a.mode, a.secs, a.cspy_timeout))

    # 1) 先开串口
    ser = serial.Serial(a.port, a.baud, timeout=0.2)
    ser.reset_input_buffer()
    print("[*] serial opened")

    # 2) 再触发 CSpyBat（超时给足，避免目标被过早打断）
    cmd = cspy_cmd(a.proj, mac, extra=["--timeout", str(a.cspy_timeout)])
    # 注意: cspy_cmd 里已有 --timeout 25, 这里改成追加覆盖不了; 直接重建
    gen = os.path.join(a.proj, "settings", "project.Debug.general.xcl")
    drv = os.path.join(a.proj, "settings", "project.Debug.driver.xcl")
    cmd = [CSPYBAT, "--silent", "--timeout", str(a.cspy_timeout),
           "--macro", mac, "-f", gen, "--backend", "-f", drv]
    print("[*] CSpyBat start...")
    p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                         text=True, errors="replace")

    # 3) 边跑边读（不因进程退出而提前 break —— 串口可能有缓冲尾巴）
    buf = bytearray()
    t0 = time.time()
    while time.time() - t0 < a.secs:
        d = ser.read(256)
        if d:
            buf += d
    try:
        p.terminate()
    except Exception:
        pass
    time.sleep(0.2)
    ser.close()

    txt = buf.decode("latin-1", errors="replace")
    print("[*] captured %d bytes" % len(buf))
    print("---- SERIAL ----")
    print(txt)
    print("---- EOF ----")
    if a.out:
        with open(a.out, "wb") as f:
            f.write(buf)
        print("[*] saved to %s" % a.out)


if __name__ == "__main__":
    main()
