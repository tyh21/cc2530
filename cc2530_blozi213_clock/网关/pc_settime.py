#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
pc_settime.py - CC2530 时钟节点「对时上位机」(GUI)  V1.2.0

(不要改上面这行的版本号: 与固件 PL_VERSION 必须一致)

作用:
    在 PC 上把当前系统时间, 通过 USB-TTL 串口发给 LG213 网关,
    网关用 BasicRF 转发给时钟节点 (PL_U_TIME -> PL_R_TIME), 节点收到即整屏重画。

链路:
    PC(本工具) --USB-TTL 115200--> LG213 网关 --BasicRF 2.4GHz--> blozi213 时钟节点

用法 (Windows):
    python pc_settime.py                # 打开图形界面
    python pc_settime.py --selftest     # 不连硬件, 自检 CRC/帧格式 (本机也能跑)

协议 (与固件 213资料\\cc2530_blozi213_clock/pl_proto.h 逐字符一致):
    UART 帧: A5 5A | LEN_H LEN_L | TYPE | PAYLOAD | CRC_H CRC_L
      LEN   = PAYLOAD 字节数; CRC16-CCITT(poly 0x1021, init 0xFFFF) 覆盖 TYPE+PAYLOAD
    校时帧: TYPE = PL_U_TIME = 0x04, PAYLOAD = Y_H Y_L 月 日 时 分 秒 (7 字节, 年大端)
    应答  : TYPE = PL_U_ACK  = 0x80, PAYLOAD = 0xFF 0xFF

依赖: pyserial  (pip install pyserial)
      tkinter 随 Python 自带 (Windows 官方安装包默认已含)
"""

import sys
import time
import queue
import threading

# ==================== 协议常量 (与固件 pl_proto.h 一致) ====================
VERSION = "1.2.0"        # 与固件共享同一版本号

U_SYNC_H, U_SYNC_L = 0xA5, 0x5A
U_TYPE_TIME = 0x04       # 校时帧 PL_U_TIME
U_ACK, U_NAK = 0x80, 0x81

RETRIES = 3              # 一帧最多发 3 次
ACK_TIMEOUT = 0.5        # 数据帧停等超时
TIME_TIMEOUT = 20.0      # 校时帧超时 (网关可能在刷自己那块屏 ~15s, 要等得久一点)

DEFAULT_BAUD = 115200
BAUD_CHOICES = [9600, 19200, 38400, 57600, 115200, 230400]


# ==================== 协议实现 (纯 Python, 不依赖串口/图形库, 便于自检) ====================
def crc16(data: bytes) -> int:
    """CRC16-CCITT (poly 0x1021, init 0xFFFF), 与固件 pl_crc16 一致"""
    c = 0xFFFF
    for b in data:
        c ^= (b << 8) & 0xFFFF
        for _ in range(8):
            c = ((c << 1) ^ 0x1021) if (c & 0x8000) else (c << 1)
            c &= 0xFFFF
    return c


def make_frame(ftype: int, payload: bytes) -> bytes:
    """UART 帧: A5 5A | LEN_H LEN_L | TYPE | PAYLOAD | CRC_H CRC_L"""
    body = bytes([ftype]) + bytes(payload)
    c = crc16(body)
    return bytes([U_SYNC_H, U_SYNC_L,
                  (len(payload) >> 8) & 0xFF, len(payload) & 0xFF]) \
        + body + bytes([(c >> 8) & 0xFF, c & 0xFF])


def time_payload(t) -> bytes:
    """struct_time -> 校时 payload: Y_H Y_L 月 日 时 分 秒 (7 字节, 年大端)"""
    return bytes([(t.tm_year >> 8) & 0xFF, t.tm_year & 0xFF,
                  t.tm_mon, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec])


def fmt_time(t) -> str:
    return "%04d-%02d-%02d %02d:%02d:%02d" % (
        t.tm_year, t.tm_mon, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec)


# ==================== 串口链路 (读帧时透传网关 ASCII 日志) ====================
class SerialLink:
    def __init__(self, log_fn):
        self.ser = None
        self.port = None
        self.baud = DEFAULT_BAUD
        self._log = bytearray()          # 网关日志行缓冲
        self.log_fn = log_fn             # callable(str): 上报一行日志

    @property
    def is_open(self):
        return self.ser is not None and getattr(self.ser, "is_open", False)

    def open(self, port: str, baud: int):
        if self.is_open and self.port == port and self.baud == baud:
            return
        self.close()
        import serial                    # 延迟导入, 便于无串口环境跑自检
        self.ser = serial.Serial(port, baud, timeout=ACK_TIMEOUT)
        self.port = port
        self.baud = baud

    def close(self):
        if self.ser is not None:
            try:
                self.ser.close()
            except Exception:
                pass
        self.ser = None

    def _gw_log(self, b: int):
        """网关会把 ASCII 日志混在串口上, 按行透传到界面"""
        if 0x20 <= b < 0x7F:
            self._log.append(b)
        elif b == 0x0A:
            line = self._log.decode("ascii", "replace").strip()
            if line:
                self.log_fn("网关: " + line)
            self._log.clear()
        elif b == 0x0D:
            pass
        else:
            self._log.clear()

    def read_frame(self, timeout: float):
        """读一帧, 返回 (type, payload) 或 None; 非帧字节按网关日志打印"""
        deadline = time.time() + timeout
        prev = 0
        while time.time() < deadline:
            self.ser.timeout = max(0.05, deadline - time.time())
            b = self.ser.read(1)
            if not b:
                continue
            b = b[0]
            if prev == U_SYNC_H and b == U_SYNC_L:
                hdr = self.ser.read(3)                 # LEN_H LEN_L TYPE
                if len(hdr) < 3:
                    prev = 0
                    continue
                ln = (hdr[0] << 8) | hdr[1]
                rest = self.ser.read(ln + 2)           # PAYLOAD + CRC
                if len(rest) < ln + 2:
                    prev = 0
                    continue
                pay = rest[:ln]
                crc_rx = (rest[ln] << 8) | rest[ln + 1]
                if crc16(bytes([hdr[2]]) + pay) == crc_rx:
                    return hdr[2], pay
                prev = 0
                continue
            if b == U_SYNC_H:
                prev = b
                continue
            self._gw_log(b)
            prev = b
        return None

    def send_wait(self, ftype: int, payload: bytes, timeout: float) -> bool:
        """发一帧并等 ACK (NAK / 超时返回 False); 网关忙时按 timeout 等待"""
        frame = make_frame(ftype, payload)
        for attempt in range(1, RETRIES + 1):
            self.ser.reset_input_buffer()
            self.ser.write(frame)
            resp = self.read_frame(timeout)
            if resp and resp[0] == U_ACK:
                return True
            if resp and resp[0] == U_NAK:
                self.log_fn("收到 NAK, 第 %d 次重发" % attempt)
            else:
                self.log_fn("等待 ACK 超时, 第 %d 次重发" % attempt)
        return False


# ==================== 图形界面 ====================
class SetTimeApp:
    def __init__(self, root, tk, ttk, scrolledtext):
        self.root = root
        self.tk = tk
        self.ttk = ttk
        self.scrolledtext = scrolledtext
        self.busy = False
        self.next_auto = None
        self.ui_q = queue.Queue()          # 工作线程 -> 主线程的 UI 消息
        self.link = SerialLink(log_fn=lambda s: self.ui_q.put(("log", s)))

        self._build_ui()
        self.refresh_ports()
        self.root.after(100, self._drain)
        self.root.after(200, self._tick)
        self.root.protocol("WM_DELETE_WINDOW", self._on_close)

    # ---------- 界面搭建 ----------
    def _build_ui(self):
        ttk = self.ttk
        root = self.root
        try:                                     # 强调按钮字体 (缺失就退默认样式)
            ttk.Style().configure("Accent.TButton",
                                  font=("Microsoft YaHei", 10, "bold"))
        except Exception:
            pass
        root.title("CC2530 时钟对时上位机  v%s" % VERSION)
        root.minsize(560, 420)

        pad = {"padx": 6, "pady": 4}

        # --- 串口设置 ---
        top = ttk.LabelFrame(root, text="串口设置 (接 LG213 网关)")
        top.pack(fill="x", padx=8, pady=(8, 4))

        ttk.Label(top, text="串口:").grid(row=0, column=0, sticky="e", **pad)
        self.port_cb = ttk.Combobox(top, width=22, values=[])
        self.port_cb.grid(row=0, column=1, sticky="w", **pad)
        ttk.Button(top, text="刷新", width=6, command=self.refresh_ports) \
            .grid(row=0, column=2, sticky="w", **pad)

        ttk.Label(top, text="波特率:").grid(row=0, column=3, sticky="e", **pad)
        self.baud_cb = ttk.Combobox(top, width=8, values=BAUD_CHOICES,
                                    state="readonly")
        self.baud_cb.set(DEFAULT_BAUD)
        self.baud_cb.grid(row=0, column=4, sticky="w", **pad)

        # --- 时间显示 ---
        clock = ttk.LabelFrame(root, text="本机当前时间")
        clock.pack(fill="x", padx=8, pady=4)
        self.clock_lbl = ttk.Label(clock, text="--:--:--",
                                   font=("Consolas", 22, "bold"))
        self.clock_lbl.pack(pady=(6, 2))
        self.wday_lbl = ttk.Label(clock, text="", font=("Microsoft YaHei", 10))
        self.wday_lbl.pack(pady=(0, 6))

        # --- 对时控制 ---
        ctrl = ttk.LabelFrame(root, text="对时")
        ctrl.pack(fill="x", padx=8, pady=4)

        self.sync_btn = ttk.Button(ctrl, text="立即对时", width=14,
                                   command=self.do_sync)
        self.sync_btn.grid(row=0, column=0, rowspan=2, padx=8, pady=8)
        self.sync_btn.configure(style="Accent.TButton")

        self.auto_var = self.tk.BooleanVar(value=False)
        ttk.Checkbutton(ctrl, text="自动对时", variable=self.auto_var,
                        command=self.on_auto_toggle) \
            .grid(row=0, column=1, sticky="w", padx=6, pady=(8, 2))
        ttk.Label(ctrl, text="每隔").grid(row=0, column=2, sticky="e")
        self.interval_var = self.tk.StringVar(value="10")
        ttk.Spinbox(ctrl, from_=1, to=9999, width=6,
                    textvariable=self.interval_var).grid(row=0, column=3, padx=4)
        self.unit_var = self.tk.StringVar(value="分钟")
        ttk.Combobox(ctrl, width=5, values=["秒", "分钟"], state="readonly",
                     textvariable=self.unit_var).grid(row=0, column=4, padx=4)

        self.countdown_lbl = ttk.Label(ctrl, text="自动对时已关闭",
                                       foreground="#666")
        self.countdown_lbl.grid(row=1, column=1, columnspan=4, sticky="w",
                                padx=6, pady=(0, 8))

        # --- 日志 ---
        logframe = ttk.LabelFrame(root, text="运行日志")
        logframe.pack(fill="both", expand=True, padx=8, pady=4)
        self.log_txt = self.scrolledtext.ScrolledText(
            logframe, height=10, wrap="word", state="disabled",
            font=("Consolas", 9))
        self.log_txt.pack(fill="both", expand=True, padx=4, pady=4)
        ttk.Button(logframe, text="清空日志", command=self.clear_log) \
            .pack(anchor="e", padx=4, pady=(0, 4))

        # --- 状态栏 ---
        self.status_lbl = ttk.Label(root, text="就绪", anchor="w",
                                    relief="sunken")
        self.status_lbl.pack(fill="x", side="bottom")

        self._log("上位机 v%s 已启动, 选择串口后点「立即对时」" % VERSION)

    # ---------- 主线程工具 ----------
    def _log(self, s: str):
        ts = time.strftime("%H:%M:%S")
        self.log_txt.configure(state="normal")
        self.log_txt.insert("end", "[%s] %s\n" % (ts, s))
        self.log_txt.see("end")
        self.log_txt.configure(state="disabled")

    def clear_log(self):
        self.log_txt.configure(state="normal")
        self.log_txt.delete("1.0", "end")
        self.log_txt.configure(state="disabled")

    def _set_status(self, s: str, color: str = "#000"):
        self.status_lbl.configure(text=s, foreground=color)

    def refresh_ports(self):
        try:
            from serial.tools import list_ports
            ports = list(list_ports.comports())
        except Exception as e:
            self._log("枚举串口失败: %s" % e)
            return
        vals = ["%s - %s" % (p.device, (p.description or "")) for p in ports]
        self.port_cb["values"] = vals
        if vals and not self.port_cb.get():
            self.port_cb.set(vals[0])
        self._log("发现 %d 个串口%s"
                  % (len(ports), (" : " + "; ".join(vals)) if vals else ""))

    def _cur_port(self):
        raw = self.port_cb.get().strip()
        return raw.split(" ")[0] if raw else ""

    # ---------- 每秒刷新 + 自动对时调度 ----------
    def _tick(self):
        now = time.localtime()
        self.clock_lbl.configure(text=time.strftime("%H:%M:%S", now))
        wd = ["周一", "周二", "周三", "周四", "周五", "周六", "周日"][now.tm_wday]
        self.wday_lbl.configure(text="%s  %s"
                                % (time.strftime("%Y-%m-%d", now), wd))

        if self.auto_var.get():
            interval = self._interval_sec()
            if self.next_auto is None:
                self.next_auto = time.time() + interval
            remain = self.next_auto - time.time()
            if remain <= 0:
                if not self.busy:
                    self.next_auto = time.time() + interval
                    self.do_sync(auto=True)
                remain = self.next_auto - time.time()
            self.countdown_lbl.configure(
                text="自动对时开启中, 下次: %s 后" % _mmss(max(0, remain)))
        else:
            self.next_auto = None
            self.countdown_lbl.configure(text="自动对时已关闭")

        self.root.after(500, self._tick)

    def _interval_sec(self):
        try:
            v = max(1, int(self.interval_var.get()))
        except ValueError:
            v = 1
            self.interval_var.set("1")
        return v * 60 if self.unit_var.get() == "分钟" else v

    def on_auto_toggle(self):
        self.next_auto = None
        if self.auto_var.get():
            self._log("已开启自动对时 (间隔 %d %s)"
                      % (int(self.interval_var.get()), self.unit_var.get()))
        else:
            self._log("已关闭自动对时")

    # ---------- 对时 ----------
    def do_sync(self, auto=False):
        if self.busy:
            self._log("上一次对时还没结束, 忽略本次请求")
            return
        port = self._cur_port()
        if not port:
            self._set_status("请先选择串口", "#c00")
            self._log("未选择串口, 取消")
            return
        try:
            baud = int(self.baud_cb.get())
        except ValueError:
            baud = DEFAULT_BAUD

        self.busy = True
        self.sync_btn.configure(state="disabled")
        self._set_status("正在对时…", "#06c")
        t = time.localtime()
        threading.Thread(target=self._worker, args=(port, baud, t, auto),
                         daemon=True).start()

    def _worker(self, port, baud, t, auto):
        tag = "自动" if auto else "手动"
        ok = False
        try:
            self.link.open(port, baud)
            self.ui_q.put(("log", "[%s] 打开 %s @ %d" % (tag, port, baud)))
            payload = time_payload(t)
            self.ui_q.put(("log", "发送校时: %s (payload=%s)"
                           % (fmt_time(t), payload.hex(" ").upper())))
            frame = make_frame(U_TYPE_TIME, payload)
            self.ui_q.put(("log", "UART 帧 (%d B): %s"
                           % (len(frame), frame.hex(" ").upper())))
            start = time.time()
            ok = self.link.send_wait(U_TYPE_TIME, payload, TIME_TIMEOUT)
            dt = time.time() - start
            if ok:
                self.ui_q.put(("log", "网关已 ACK (%.2fs), 时间帧已 RF 转发给时钟节点" % dt))
            else:
                self.ui_q.put(("log", "失败: 时间帧无 ACK (%.2fs), 检查网关接线/串口/供电" % dt))
        except Exception as e:
            self.ui_q.put(("log", "异常: %s" % e))
        finally:
            self.ui_q.put(("done", ok, tag))

    # ---------- UI 消息泵 ----------
    def _drain(self):
        try:
            while True:
                msg = self.ui_q.get_nowait()
                if msg[0] == "log":
                    self._log(msg[1])
                elif msg[0] == "done":
                    ok, tag = msg[1], msg[2]
                    self.busy = False
                    self.sync_btn.configure(state="normal")
                    if ok:
                        self._set_status("%s对时成功 %s"
                                         % (tag, time.strftime("%H:%M:%S")), "#080")
                    else:
                        self._set_status("%s对时失败 %s"
                                         % (tag, time.strftime("%H:%M:%S")), "#c00")
        except queue.Empty:
            pass
        self.root.after(100, self._drain)

    def _on_close(self):
        self.link.close()
        self.root.destroy()


def _mmss(sec: float) -> str:
    sec = int(round(sec))
    return "%02d:%02d" % (sec // 60, sec % 60)


# ==================== 自检 (不连硬件) ====================
def selftest() -> int:
    print("pc_settime.py v%s 自检" % VERSION)
    ok = True

    v = crc16(b"123456789")
    print("CRC16('123456789') = 0x%04X (期望 0x29B1)" % v)
    ok &= (v == 0x29B1)

    t = time.struct_time((2026, 10, 25, 12, 34, 0, 0, 0, -1))
    pay = time_payload(t)
    print("payload = %s (期望 07 EA 0A 19 0C 22 00)" % pay.hex(" ").upper())
    ok &= (pay == bytes([0x07, 0xEA, 0x0A, 0x19, 0x0C, 0x22, 0x00]))
    ok &= (len(pay) == 7)

    f = make_frame(U_TYPE_TIME, pay)
    print("frame = %s" % f.hex(" ").upper())
    # 结构: A5 5A | 00 07 | 04 | ... | CRC_H CRC_L  -> 头4 + 1 + 7 + 2 = 14 字节
    ok &= (f[0] == 0xA5 and f[1] == 0x5A and f[2] == 0x00 and f[3] == 0x07
           and f[4] == 0x04 and len(f) == 14)
    body = f[4:12]
    ok &= (((f[12] << 8) | f[13]) == crc16(body))

    print("结果:", "通过" if ok else "失败")
    return 0 if ok else 1


# ==================== 入口 ====================
def main():
    if "--selftest" in sys.argv:
        return selftest()

    try:
        import tkinter as tk
        from tkinter import ttk, scrolledtext
    except ImportError:
        print("需要 tkinter (Windows 官方 Python 安装包默认自带)")
        return 2
    try:
        import serial  # noqa: F401
    except ImportError:
        print("需要 pyserial:  pip install pyserial")
        return 2

    root = tk.Tk()
    SetTimeApp(root, tk, ttk, scrolledtext)
    root.mainloop()
    return 0


if __name__ == "__main__":
    sys.exit(main())
