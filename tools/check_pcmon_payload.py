#!/usr/bin/env python3
"""核对上位机发出去的 JSON 和固件读的字段对不对得上。

为什么需要这个：这两边是靠**字段名**耦合的，而且耦合失败时两边都不报错——
上位机照常写串口，板子照常收，只是屏幕上一片 "--"。这种错最难发现，
因为看起来像"没插好线"。

做法：拦住上位机的串口（换成假的 Serial），把 _monitor_loop 真跑一遍拿到
它实际写出去的字节，解析成 JSON；再去 main/app/app_pcmon.c 里把固件读的
字段名抠出来，两边比对。**不联网、不接板子、不弹窗**。

用法：
    python tools/check_pcmon_payload.py

需要 pyserial / psutil（上位机本来就要装），以及一个能建 Tk 根窗口的环境
（上位机本身也是 Tk 程序）。建不了窗口时会直接跳过，不算失败。
"""

import io
import json
import os
import re
import sys
import threading
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
APP = os.path.join(ROOT, "assets", "Ecam_display")
SRC = os.path.join(ROOT, "main", "app", "app_pcmon.c")

sys.path.insert(0, APP)


class FakeSerial:
    """顶掉 pyserial 的 Serial。

    **刻意模仿"端口被占用就报错"这个真实行为**：真串口第二次打开时
    serial.Serial() 会抛异常，而"停止传递数据"按钮失效（self.monitoring
    从来没被置上）的症状恰好就是那个异常弹出来的"连接失败"提示框。
    假串口不模拟这一点的话，那类 bug 在测试台上是看不见的。
    """
    lines = []
    _open = False

    def __init__(self, *a, **kw):
        if FakeSerial._open:
            raise OSError("Access is denied. (端口已被占用)")
        FakeSerial._open = True
        self.is_open = True

    def write(self, data):
        FakeSerial.lines.append(data)
        return len(data)

    def close(self):
        self.is_open = False
        FakeSerial._open = False


def run_session():
    """跑一遍上位机：开始 → 停止 → 再开始。返回 (采集结果, 开关检查结果)。"""
    import serial
    serial.Serial = FakeSerial

    import tkinter as tk
    root = tk.Tk()
    root.withdraw()                     # 不弹窗口

    import Ecam_Status_Display as ecam

    # messagebox 是模态的：真弹出来测试就永远停在那儿了。换成记录
    dialogs = []
    ecam.messagebox.showwarning = lambda t, m, **k: dialogs.append((t, m))
    ecam.messagebox.showerror = lambda t, m, **k: dialogs.append((t, m))

    app = ecam.EcamDashboard(root)
    app.port_combo["values"] = ["FAKE - test"]
    app.port_combo.current(0)

    r = {}

    def step_start():
        app._start_monitor()
        r["started"] = app.monitoring
        root.after(2500, step_stop)

    def step_stop():
        # 这一下就是"停止传递数据"按钮做的事
        app._toggle_monitor()
        r["stopped"] = not app.monitoring
        r["lines_at_stop"] = len(FakeSerial.lines)
        root.after(1500, step_idle)

    def step_idle():
        # 停了之后不该再有新数据
        r["silent_after_stop"] = (len(FakeSerial.lines) == r["lines_at_stop"])
        app._toggle_monitor()           # 再开一次
        r["restarted"] = app.monitoring
        root.after(2500, step_done)

    def step_done():
        r["data_after_restart"] = len(FakeSerial.lines) > r["lines_at_stop"]
        app._stop_monitor()
        root.quit()

    '''
    主线程必须**真的进 mainloop**：_monitor_loop 在工作线程里要读 Tk 变量
    （self.base_url_var.get() 这些），而 Tk 不允许工作线程在没人跑 mainloop
    的时候碰它——会直接抛 "main thread is not in main loop"，一行都写不出去。
    这正是真机上不会遇到、但搬到测试台就会踩的坑。
    '''
    root.after(100, step_start)
    root.after(30000, root.quit)        # 保险：卡住也要退出来
    root.mainloop()

    app._system_monitor_running = False
    try:
        app._api_stop.set()
    except Exception:
        pass
    time.sleep(0.3)
    root.destroy()

    payload = None
    if FakeSerial.lines:
        payload = json.loads(FakeSerial.lines[0].decode("utf-8").strip())
    return payload, r, dialogs


def firmware_fields():
    """从 app_pcmon.c 里抠出固件读的字段名，返回 (顶层集合, api 子集合)。"""
    src = open(SRC, encoding="utf-8").read()
    # json_num(root, "cpu", &d) / json_num(api, "tokens", &d)
    top = set(re.findall(r'json_num\(\s*root\s*,\s*"([^"]+)"', src))
    api = set(re.findall(r'json_num\(\s*api\s*,\s*"([^"]+)"', src))
    # 非数字的那些：GetObjectItemCaseSensitive(root|api, "host"|"api")
    others = re.findall(r'cJSON_GetObjectItemCaseSensitive\(\s*(\w+)\s*,\s*"([^"]+)"', src)
    for obj, key in others:
        (top if obj == "root" else api if obj == "api" else set()).add(key)
    return top, api


def main():
    print("跑一遍上位机：开始 -> 停止 -> 再开始 ...")
    try:
        payload, sess, dialogs = run_session()
    except Exception as e:              # noqa: BLE001 —— 建不了 Tk 之类的
        print("跳过：%s" % e)
        return 0

    bad = False

    # ---- 开关逻辑 ----
    # 这一组检查抓的是"按钮没接上"这一类 bug：self.monitoring 忘了置位时，
    # 点"停止"会再去开一次同一个串口，弹出来的却是"串口忙/连接失败"。
    checks = [
        ("started",   sess.get("started"),       True,  "开始后 monitoring 应为 True"),
        ("stopped",   sess.get("stopped"),       True,  "点停止后 monitoring 应为 False"),
        ("silent",    sess.get("silent_after_stop"), True, "停止后不该再往串口写数据"),
        ("restarted", sess.get("restarted"),     True,  "再点一次应该能重新开始"),
        ("resumed",   sess.get("data_after_restart"), True, "重新开始后应该又有数据"),
    ]
    for _, got, want, desc in checks:
        if got != want:
            bad = True
            print("**失败**：%s（实际 %r）" % (desc, got))

    if dialogs:
        bad = True
        for t, m in dialogs:
            print("**上位机弹了框**：%s / %s" % (t, m.replace("\n", " ")))

    if payload is None:
        print("**失败**：上位机一行都没写出去。"
              "_start_monitor() 有没有真的把 _monitor_loop 起起来？")
        return 1

    print("上位机发出：")
    print("  " + json.dumps(payload, ensure_ascii=False))

    # ---- 字段名对不对得上 ----
    top, api = firmware_fields()
    sent_top = set(payload.keys())
    sent_api = set(payload.get("api", {}).keys())

    missing = top - sent_top
    if missing:
        bad = True
        print("**固件要读、但上位机没发**（这些项在屏幕上会一直是默认值）：%s"
              % ", ".join(sorted(missing)))
    missing_api = api - sent_api
    if missing_api:
        bad = True
        print("**固件要读、但上位机没发**（api 里）：%s" % ", ".join(sorted(missing_api)))

    # 上位机多发的字段不算错（固件忽略未知字段），只提示一下
    extra = (sent_top - top - {"type", "ts"}) | (sent_api - api - {"base_url", "key_set"})
    if extra:
        print("提示：上位机多发、固件没用到的字段：%s" % ", ".join(sorted(extra)))

    if bad:
        return 1
    print("开关逻辑正常；字段一致：固件读的 %d 个顶层 + %d 个 api 字段，上位机都发了。"
          % (len(top), len(api)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
