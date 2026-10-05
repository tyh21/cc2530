#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""flash_capture.py -- 烧录v45复位运行 + 抓串口 (CSpyBat自然超时退出,不强杀)
用法: python flash_capture.py [抓取秒数, 默认55]
"""
import serial, subprocess, time, sys, os

os.chdir(r"C:\Users\tyh21\Desktop\cc2530\cc2530_RF_link\cc2530_LG290_nfc_mac")
CSPYBAT = r"C:\Program Files (x86)\IAR Systems\Embedded Workbench 8.0\common\bin\CSpyBat.exe"
GEN = r"settings\project.Debug.general.xcl"
DRV = r"settings\project.Debug.driver.xcl"
MAC = r"tools\cspy\_run.mac"
N = float(sys.argv[1]) if len(sys.argv) > 1 else 55.0

ser = serial.Serial('COM13', 115200, timeout=0.2)
ser.reset_input_buffer()

p = subprocess.Popen([CSPYBAT, '--silent', '--timeout', '20', '--macro', MAC,
                      '-f', GEN, '--backend', '-f', DRV],
                     stdout=subprocess.PIPE, stderr=subprocess.STDOUT)

buf = b''
t0 = time.time()
while time.time() - t0 < N:
    d = ser.read(256)
    if d:
        buf += d

cspy_out = b''
try:
    out, _ = p.communicate(timeout=8)     # CSpyBat 20s 超时应已自己退
    cspy_out = out or b''
except Exception:
    p.kill()
    out, _ = p.communicate()
    cspy_out = out or b'(killed)'

print("---- CSpyBat ----")
print(cspy_out.decode('latin-1', errors='replace'))
print("---- SERIAL ----")
print(buf.decode('latin-1', errors='replace'))
print("---- END %d chars ----" % len(buf))
ser.close()
