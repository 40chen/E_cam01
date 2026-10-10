#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Ecam Status Display - PC Monitor for ESP32
Version: 5.0 (修复 exe 不记忆配置：路径绑定 exe 真实目录;  单价支持锁定/随动: 手填或反推单价可钉为最高优先锚点, 之后随 API 余额实时反推 Tokens)
  - 优先使用 pynvml 进程内读取 GPU（无子进程、无窗口闪烁、响应更快）
  - 未安装 pynvml 时回退 nvidia-smi，并通过 CREATE_NO_WINDOW 抑制控制台窗口
"""

import tkinter as tk
from tkinter import ttk, messagebox
import threading
import time
import json
import os
import base64
import subprocess
import sys
import platform
import os
import urllib.request
import urllib.error

# ---------- 依赖检查 ----------
try:
    import serial
    import serial.tools.list_ports
except ImportError:
    _r = tk.Tk(); _r.withdraw()
    messagebox.showerror("Missing Module", "Please install pyserial:\npip install pyserial")
    sys.exit(1)

try:
    import psutil
except ImportError:
    _r = tk.Tk(); _r.withdraw()
    messagebox.showerror("Missing Module", "Please install psutil:\npip install psutil")
    sys.exit(1)

# ==========================================================
# 配置区：API 接入
#   不再内置任何平台预设，只需填入接口地址(Base URL)与 API Key，
#   即可实时获取 Tokens / 已消费 / 余额；接口未提供的项可手填并自动联动计算。
# ==========================================================
# 关键修复(问题一)：打包为 exe 后 __file__ 指向 PyInstaller 的临时解包目录(_MEIPASS)，
# 退出即被清理，导致配置写到临时目录、下次读不回(表现为“不记录上次登录信息”)。
# 故：冻结(exe)时以 sys.executable 真实所在目录为准，源码运行时以脚本目录为准。
def _app_dir():
    if getattr(sys, "frozen", False):
        return os.path.dirname(os.path.abspath(sys.executable))
    return os.path.dirname(os.path.abspath(__file__))

CONFIG_PATH = os.path.join(_app_dir(), "ecam_config.json")
# API Key 优先存入操作系统密钥库(keyring)：
#   - Windows 下走凭据管理器(DPAPI)，加密且绑定当前用户账户，换到别人电脑解不开；
#   - 因此把整个文件夹/压缩包分享出去也不会泄露 Key。
# 仅当未安装 keyring 时才退回 json 内的 Base64(可逆，仅"防君子不防小人")。
# 安装：pip install keyring   (Windows 自带凭据后端，一般无需额外插件)
KEYRING_SERVICE = "EcamStatusDisplay"
KEYRING_ACCOUNT = "api_key"
try:
    import keyring
    _HAS_KEYRING = True
except Exception:
    _HAS_KEYRING = False
API_BASE_URL_DEFAULT = "https://api.siliconflow.cn"  # 默认接口地址，可在界面修改

# 常用平台接口地址(Base URL)预设，可在界面下拉选择，也可手动输入任意 OpenAI 兼容地址
API_BASE_URL_PRESETS = [
    "https://api.siliconflow.cn",                       # 硅基流动 SiliconFlow
    "https://api.deepseek.com",                         # DeepSeek
    "https://dashscope.aliyuncs.com/compatible-mode",   # 阿里通义千问 DashScope
    "https://open.bigmodel.cn/api/paas/v4",             # 智谱 GLM
    "https://ark.cn-beijing.volces.com/api/v3",         # 火山方舟 豆包
    "https://api.moonshot.cn",                          # 月之暗面 Kimi
    "https://api.openai.com",                           # OpenAI
]
API_REFRESH_INTERVAL = 10  # 秒
API_PRICE_PER_1M_DEFAULT = ""      # 单价默认为空(随动反推)；仅在数据不足时手填作锚点
API_RECHARGE_DEFAULT = ""          # 默认初始充值总额(元)，留空则用「已消费+余额」自动推断

# ==========================================================
# 苹果风格配色
# ==========================================================
class AppleTheme:
    BG        = "#F2F2F7"
    CARD      = "#FFFFFF"
    CARD_ALT  = "#F9F9FB"
    TEXT      = "#1C1C1E"
    TEXT_SEC  = "#8E8E93"
    SEPARATOR = "#E5E5EA"
    ACCENT    = "#007AFF"
    GREEN     = "#34C759"
    ORANGE    = "#FF9500"
    RED       = "#FF3B30"
    PURPLE    = "#AF52DE"
    TRACK     = "#E5E5EA"
    FONT      = "Segoe UI"
    MONO      = "Consolas"

# ==========================================================
# 圆角卡片（组合模式 + 尺寸缓存）
# ==========================================================
class RoundedCard:
    def __init__(self, parent, title=""):
        self._ready = False
        self._last_size = None
        self._last_inner_h = None
        self._last_canvas_w = None

        self.root = tk.Frame(parent, bg=AppleTheme.BG)

        if title:
            tk.Label(
                self.root, text=title,
                font=(AppleTheme.FONT, 11, "bold"),
                fg=AppleTheme.TEXT_SEC, bg=AppleTheme.BG, anchor="w"
            ).pack(fill="x", padx=4, pady=(0, 6))

        self.canvas = tk.Canvas(
            self.root, bg=AppleTheme.BG,
            highlightthickness=0, height=1
        )
        self.canvas.pack(fill="both", expand=True)

        self.inner = tk.Frame(self.canvas, bg=AppleTheme.CARD)
        self._win = self.canvas.create_window((0, 0), window=self.inner, anchor="nw")

        self.canvas.bind("<Configure>", self._on_resize)
        self.inner.bind("<Configure>", self._on_inner)
        self.root.after_idle(self._init_draw)

    def pack(self, **kw):
        self.root.pack(**kw)

    def grid(self, **kw):
        self.root.grid(**kw)

    def _init_draw(self):
        self._ready = True
        try:
            w = self.canvas.winfo_width()
            h = max(self.canvas.winfo_height(), self.inner.winfo_reqheight())
            self._last_inner_h = self.inner.winfo_reqheight()
            self._last_canvas_w = w
            self._redraw(w, h)
        except tk.TclError:
            pass

    def _on_resize(self, e):
        if self._last_canvas_w == e.width:
            return
        self._last_canvas_w = e.width
        try:
            self.canvas.itemconfigure(self._win, width=e.width)
        except tk.TclError:
            return
        self._redraw(e.width, self.canvas.winfo_height())

    def _on_inner(self, e):
        if self._last_inner_h == e.height:
            return
        self._last_inner_h = e.height
        try:
            self.canvas.configure(height=e.height)
        except tk.TclError:
            return
        self._redraw(self.canvas.winfo_width(), e.height)

    def _redraw(self, w, h):
        if w <= 1 or h <= 1:
            return
        size = (w, h)
        if size == self._last_size:
            return
        self._last_size = size
        try:
            self.canvas.delete("bg")
        except tk.TclError:
            return
        r = 14
        self._round_rect(2, 2, w - 2, h - 2, r, fill=AppleTheme.CARD, outline="", tags="bg")
        try:
            self.canvas.tag_lower("bg")
        except tk.TclError:
            pass

    def _round_rect(self, x1, y1, x2, y2, r, **kw):
        try:
            pts = [
                x1 + r, y1, x1 + r, y1,
                x2 - r, y1, x2 - r, y1, x2, y1,
                x2, y1 + r, x2, y1 + r,
                x2, y2 - r, x2, y2 - r,
                x2, y2, x2 - r, y2,
                x2 - r, y2, x2 - r, y2,
                x1 + r, y2, x1 + r, y2,
                x1, y2, x1, y2 - r,
                x1, y2 - r, x1, y2 - r,
                x1, y1 + r, x1, y1 + r,
                x1, y1,
            ]
            return self.canvas.create_polygon(pts, smooth=True, **kw)
        except tk.TclError:
            return None

# ==========================================================
# 苹果风格进度条
# 关键：使用持久化的 canvas 图形项，只更新坐标/颜色，
# 永远不 delete，因此不会出现"一秒闪一次"的问题。
# ==========================================================
class AppleBar:
    def __init__(self, parent, height=8, color=AppleTheme.GREEN, width=200):
        self._h = height
        self._color = color
        self._value = 0
        self._w = width
        self._ready = False
        self._last_drawn_value = None
        self._last_color = None
        self._last_size = None

        self.frame = tk.Frame(parent, bg=AppleTheme.CARD, height=height, width=width)
        self.frame.pack_propagate(False)

        self.canvas = tk.Canvas(
            self.frame, bg=AppleTheme.CARD,
            highlightthickness=0, height=height, width=width
        )
        self.canvas.pack(fill="both", expand=True)

        # === 持久化 canvas 图形项：只创建一次 ===
        self._track_id = self.canvas.create_rectangle(
            0, 0, width, height,
            fill=AppleTheme.TRACK, outline=""
        )
        self._fill_id = self.canvas.create_rectangle(
            0, 0, 0, height,
            fill=color, outline=""
        )

        self.canvas.bind("<Configure>", self._on_resize)
        self.frame.after_idle(self._init_draw)

    # --- 对外接口 ---
    def pack(self, **kw):
        self.frame.pack(**kw)

    def configure(self, **kw):
        self.frame.configure(**kw)

    def set(self, value, color=None):
        value = max(0, min(100, value))
        new_color = color if color else self._color
        iv = int(round(value))
        if iv == self._last_drawn_value and new_color == self._last_color:
            return
        self._value = value
        self._color = new_color
        if self._ready:
            self._draw()

    # --- 内部 ---
    def _init_draw(self):
        self._ready = True
        self._draw()

    def _on_resize(self, e):
        size = (e.width, e.height)
        if size == self._last_size:
            return
        self._last_size = size
        self._w = e.width
        self._h = e.height
        if self._ready:
            self._draw()

    def _draw(self):
        """只更新持久化图形项的坐标和颜色，绝不 delete"""
        if not self._ready or self._w <= 1 or self._h <= 1:
            return
        self._last_drawn_value = int(round(self._value))
        self._last_color = self._color
        try:
            self.canvas.coords(self._track_id, 0, 0, self._w, self._h)
            fill_w = self._w * self._value / 100
            self.canvas.coords(self._fill_id, 0, 0, fill_w, self._h)
            self.canvas.itemconfig(self._fill_id, fill=self._color)
        except tk.TclError:
            return

# ==========================================================
# 硬件采集
# GPU 采集：优先 pynvml（无子进程，不闪控制台窗口）；
# 未安装时回退 nvidia-smi，并用 CREATE_NO_WINDOW 抑制窗口闪烁。
# ==========================================================
_GPU_AVAILABLE = False
_GPU_HANDLE = None
try:
    import pynvml
    pynvml.nvmlInit()
    _GPU_HANDLE = pynvml.nvmlDeviceGetHandleByIndex(0)
    _GPU_AVAILABLE = True

    def get_gpu_usage():
        try:
            return int(pynvml.nvmlDeviceGetUtilizationRates(_GPU_HANDLE).gpu)
        except Exception:
            return -1

except Exception:
    _GPU_AVAILABLE = False
    _NO_WINDOW_FLAG = 0x08000000 if platform.system() == "Windows" else 0

    def get_gpu_usage():
        try:
            extra = {}
            if _NO_WINDOW_FLAG:
                extra["creationflags"] = _NO_WINDOW_FLAG
            r = subprocess.run(
                ["nvidia-smi", "--query-gpu=utilization.gpu",
                 "--format=csv,noheader,nounits"],
                capture_output=True, text=True, timeout=3, **extra
            )
            if r.returncode == 0:
                return int(r.stdout.strip().split("\n")[0])
        except Exception:
            pass
        return -1

def get_system_snapshot():
    cpu = psutil.cpu_percent(interval=None)
    mem = psutil.virtual_memory()
    net = psutil.net_io_counters()
    battery = -1
    try:
        bat = psutil.sensors_battery()
        if bat:
            battery = int(bat.percent)
    except Exception:
        pass
    return {
        "cpu": round(cpu, 1),
        "ram": round(mem.percent, 1),
        "ram_used_gb": round((mem.total - mem.available) / 1024**3, 2),
        "ram_total_gb": round(mem.total / 1024**3, 2),
        "gpu": get_gpu_usage(),
        "net_up": net.bytes_sent,
        "net_down": net.bytes_recv,
        "battery": battery,
        "host": platform.node(),
    }

# ==========================================================
# 主界面
# ==========================================================
# ==========================================================
# API 用量获取（标准库实现，无需第三方 requests）
#   依据填入的 API(接口地址 + Key) 实时查询 Tokens / 已消费 / 余额，
#   不区分平台：依次尝试 OpenAI 兼容的常见计费/用户信息端点，
#   取到多少填多少，取不到的项保持 None（界面显示 --）。
# ==========================================================
def _http_get_json(url, key, timeout=8):
    req = urllib.request.Request(url, headers={
        "Authorization": "Bearer " + key,
        "Content-Type": "application/json",
        "User-Agent": "EcamStatusDisplay/3.9",
    })
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        return json.loads(resp.read().decode("utf-8", "replace"))


def _to_float(v):
    try:
        return float(v)
    except (ValueError, TypeError):
        return None


def _to_int(v):
    try:
        return int(float(v))
    except (ValueError, TypeError):
        return None


def _pick(d, *names):
    """从字典（含 data 嵌套层）里按候选键取第一个非空值"""
    pools = [d]
    if isinstance(d, dict) and isinstance(d.get("data"), dict):
        pools.append(d["data"])
    for pool in pools:
        for n in names:
            if isinstance(pool, dict) and pool.get(n) not in (None, ""):
                return pool.get(n)
    return None


def fetch_api_usage(key, base_url=API_BASE_URL_DEFAULT):
    """依据填入的 API 实时查询用量。
    返回 {"balance":float|None, "cost":float|None, "tokens":int|None, "error":str?}
    任一字段取不到即 None（界面显示 --）。"""
    key = (key or "").strip()
    result = {"balance": None, "cost": None, "tokens": None}
    if not key:
        return result
    base = (base_url or "").strip().rstrip("/")
    if not base:
        base = API_BASE_URL_DEFAULT
    candidates = [
        base + "/v1/user/info",
        base + "/user/balance",
        base + "/v1/dashboard/billing/subscription",
        base + "/v1/dashboard/billing/credit_grants",
    ]
    err = None
    for url in candidates:
        try:
            d = _http_get_json(url, key)
        except Exception as e:
            err = str(e)
            continue
        if isinstance(d, dict) and isinstance(d.get("balance_infos"), list) and d["balance_infos"]:
            first = d["balance_infos"][0]
            if isinstance(first, dict) and first.get("total_balance") not in (None, ""):
                result["balance"] = _to_float(first.get("total_balance"))
        if result["balance"] is None:
            result["balance"] = _to_float(_pick(
                d, "balance", "totalBalance", "total_balance", "available", "total_granted"))
        if result["cost"] is None:
            result["cost"] = _to_float(_pick(
                d, "totalCost", "total_cost", "usedBalance", "totalUsed",
                "used_balance", "charge_used", "usedTotalBalance",
                "amount_used_total", "total_usage"))
        if result["tokens"] is None:
            result["tokens"] = _to_int(_pick(
                d, "totalToken", "total_token", "totalTokens", "total_tokens",
                "total_usage_tokens", "tokens"))
        if all(result[k] is not None for k in ("balance", "cost", "tokens")):
            break
    if err and all(result[k] is None for k in ("balance", "cost", "tokens")):
        result["error"] = err
    return result


class EcamDashboard:
    def __init__(self, root):
        self.root = root
        self.root.title("Ecam Status Display")
        self.root.geometry("750x650")
        self.root.minsize(560, 460)
        self.root.resizable(True, True)
        self.root.configure(bg=AppleTheme.BG)

        self.monitoring = False
        self.serial_conn = None
        self.monitor_thread = None
        self._last_net = None
        self._last_time = None
        self._last_net_text = ""

        # 读取上次关闭时保存的配置(接口/单价/充值/Key)，存在则覆盖默认值
        self._cfg = self._load_config()
        self.base_url_var = tk.StringVar(value=self._cfg.get("base_url") or API_BASE_URL_DEFAULT)
        self.api_key_var = tk.StringVar(value=self._cfg.get("api_key") or "")
        # API 用量：Tokens / 已消费(元) / 余额(元)，未取到显示 --（可手填自动计算）
        self.tokens_var = tk.StringVar(value="--")
        self.cost_var = tk.StringVar(value="--")
        self.balance_var = tk.StringVar(value="--")
        # 自动计算所需基准：初始充值总额(元) 与 单价(元/百万 Tokens)
        self.recharge_var = tk.StringVar(value=self._cfg.get("recharge") or API_RECHARGE_DEFAULT)
        _loaded_price = self._cfg.get("price")
        if _loaded_price is None:
            _loaded_price = API_PRICE_PER_1M_DEFAULT
        self.price_var = tk.StringVar(value=_loaded_price)
        # 每个字段数据来源：""(空/--) / api(实时锁定) / manual(手填) / calc(自动计算)
        _lp = _loaded_price.strip() if isinstance(_loaded_price, str) else ""
        self.field_mode = {"tokens": "", "cost": "", "balance": "",
                           "price": ("manual" if _lp and _lp != "--" else "")}
        self._updating = False

        self._build_ui()
        if _HAS_KEYRING:
            self._keyring_note = "API Key 将加密存入系统密钥库(keyring)"
        else:
            self._keyring_note = "提示：pip install keyring 可让 Key 加密存储(推荐)"
        self._refresh_ports()
        self._start_api_thread()
        # 系统监控独立运行，不依赖串口连接
        self._system_monitor_running = True
        self._start_system_monitor()
        try:
            self._log(getattr(self, "_keyring_note", ""))
        except Exception:
            pass

    # ------------------------------------------------------
    # UI 构建
    # ------------------------------------------------------
    def _build_ui(self):
        header = tk.Frame(self.root, bg=AppleTheme.BG, height=64)
        header.pack(fill="x", padx=20, pady=(16, 8))
        header.pack_propagate(False)

        tk.Label(header, text="Ecam Status", font=(AppleTheme.FONT, 20, "bold"),
                 fg=AppleTheme.TEXT, bg=AppleTheme.BG).pack(side="left")
        tk.Label(header, text="ESP32 Monitor", font=(AppleTheme.FONT, 10),
                 fg=AppleTheme.TEXT_SEC, bg=AppleTheme.BG
                 ).pack(side="left", padx=(8, 0), pady=(8, 0))

        self.status_dot = tk.Label(header, text="● Ready", font=(AppleTheme.FONT, 10),
                                   fg=AppleTheme.TEXT_SEC, bg=AppleTheme.BG)
        self.status_dot.pack(side="right", pady=(6, 0))

        # === 主体：上面两列并排（系统状态 | API 用量 各占一半），串口连接在最下面通栏 ===
        self.body = tk.Frame(self.root, bg=AppleTheme.BG)
        self.body.pack(fill="both", expand=True, padx=20, pady=(4, 4))
        self.body.columnconfigure(0, weight=1, uniform="col")
        self.body.columnconfigure(1, weight=1, uniform="col")
        self.body.rowconfigure(0, weight=1)
        self.body.rowconfigure(1, weight=0)

        # --- 第 1 列：系统监控卡片 ---
        sys_card = RoundedCard(self.body, title="系统状态")
        sys_card.grid(row=0, column=0, sticky="nsew", padx=(0, 6))

        self.cpu_bar = self._make_row(sys_card.inner, "CPU", AppleTheme.ACCENT)
        self.ram_bar = self._make_row(sys_card.inner, "内存", AppleTheme.GREEN)
        self.gpu_bar = self._make_row(sys_card.inner, "GPU", AppleTheme.PURPLE)

        net_row = tk.Frame(sys_card.inner, bg=AppleTheme.CARD)
        net_row.pack(fill="x", padx=16, pady=(6, 14))
        tk.Label(net_row, text="网络", width=6, anchor="w", font=(AppleTheme.FONT, 11),
                 fg=AppleTheme.TEXT_SEC, bg=AppleTheme.CARD).pack(side="left")
        self.net_label = tk.Label(net_row, text="↑ 0 KB/s ↓ 0 KB/s",
                                  font=(AppleTheme.MONO, 10),
                                  fg=AppleTheme.TEXT, bg=AppleTheme.CARD)
        self.net_label.pack(side="left", padx=(10, 0))

        # --- 第 2 列：串口配置卡片 ---
        port_card = RoundedCard(self.body, title="串口连接")
        port_card.grid(row=1, column=0, columnspan=2, sticky="ew", pady=(12, 0))

        port_row = tk.Frame(port_card.inner, bg=AppleTheme.CARD)
        port_row.pack(fill="x", padx=16, pady=(10, 6))
        tk.Label(port_row, text="端口", font=(AppleTheme.FONT, 11),
                 fg=AppleTheme.TEXT_SEC, bg=AppleTheme.CARD,
                 width=6, anchor="w").pack(side="left")
        self.port_combo = ttk.Combobox(port_row, state="readonly",
                                       font=(AppleTheme.FONT, 10))
        self.port_combo.pack(side="left", fill="x", expand=True, padx=(10, 6))
        self.refresh_btn = tk.Button(port_row, text="刷新", font=(AppleTheme.FONT, 10),
                                     fg=AppleTheme.ACCENT, bg=AppleTheme.CARD,
                                     activebackground=AppleTheme.CARD,
                                     activeforeground=AppleTheme.ACCENT,
                                     bd=0, cursor="hand2",
                                     command=self._refresh_ports)
        self.refresh_btn.pack(side="left")

        self.connect_btn = tk.Button(
            port_card.inner, text="开始传递数据",
            font=(AppleTheme.FONT, 12, "bold"),
            fg="white", bg=AppleTheme.ACCENT,
            activebackground="#0066CC", activeforeground="white",
            bd=0, cursor="hand2", height=2,
            command=self._toggle_monitor
        )
        self.connect_btn.pack(fill="x", padx=16, pady=(6, 14), side="bottom")

        # --- 第 3 列：API 用量卡片 ---
        api_card = RoundedCard(self.body, title="API 用量")
        api_card.grid(row=0, column=1, sticky="nsew", padx=(6, 0))

        m_row = tk.Frame(api_card.inner, bg=AppleTheme.CARD)
        m_row.pack(fill="x", padx=16, pady=(10, 6))
        tk.Label(m_row, text="接口", width=6, anchor="w", font=(AppleTheme.FONT, 11),
                 fg=AppleTheme.TEXT_SEC, bg=AppleTheme.CARD).pack(side="left")
        self.base_combo = ttk.Combobox(
            m_row, textvariable=self.base_url_var,
            values=API_BASE_URL_PRESETS, font=(AppleTheme.FONT, 10))
        self.base_combo.pack(side="left", fill="x", expand=True, padx=(10, 0))

        k_row = tk.Frame(api_card.inner, bg=AppleTheme.CARD)
        k_row.pack(fill="x", padx=16, pady=6)
        tk.Label(k_row, text="Key", width=6, anchor="w", font=(AppleTheme.FONT, 11),
                 fg=AppleTheme.TEXT_SEC, bg=AppleTheme.CARD).pack(side="left")
        tk.Entry(k_row, textvariable=self.api_key_var, show="•",
                 font=(AppleTheme.MONO, 10), bg=AppleTheme.CARD_ALT,
                 fg=AppleTheme.TEXT, relief="flat", bd=6
                 ).pack(side="left", fill="x", expand=True, padx=(10, 0))

        rbtn_row = tk.Frame(api_card.inner, bg=AppleTheme.CARD)
        rbtn_row.pack(fill="x", padx=16, pady=(2, 0))
        self.api_auto_var = tk.BooleanVar(value=True)
        self.auto_check = tk.Checkbutton(
            rbtn_row, text="自动刷新(实时)", variable=self.api_auto_var,
            command=self._on_auto_toggle,
            font=(AppleTheme.FONT, 10), bg=AppleTheme.CARD,
            activebackground=AppleTheme.CARD, selectcolor=AppleTheme.CARD_ALT,
            fg=AppleTheme.TEXT_SEC,
        )
        self.auto_check.pack(side="left")
        self.refresh_btn_now = tk.Button(
            rbtn_row, text="立即刷新", font=(AppleTheme.FONT, 10),
            fg=AppleTheme.ACCENT, bg=AppleTheme.CARD, bd=0, cursor="hand2",
            activebackground=AppleTheme.CARD, activeforeground=AppleTheme.ACCENT,
            command=self._refresh_api_now,
        )
        self.refresh_btn_now.pack(side="right")

        tk.Frame(api_card.inner, bg=AppleTheme.SEPARATOR, height=1).pack(fill="x", padx=16, pady=(8, 6))

        # 计算基准：充值总额 + 单价（实时量与手填量之间自动换算的锚点）
        cfg_row = tk.Frame(api_card.inner, bg=AppleTheme.CARD)
        cfg_row.pack(fill="x", padx=16, pady=4)
        tk.Label(cfg_row, text="充值", width=6, anchor="w", font=(AppleTheme.FONT, 11),
                 fg=AppleTheme.TEXT_SEC, bg=AppleTheme.CARD).pack(side="left")
        e_re = tk.Entry(cfg_row, textvariable=self.recharge_var, font=(AppleTheme.MONO, 10),
                        bg=AppleTheme.CARD_ALT, fg=AppleTheme.TEXT, relief="flat", bd=6, width=8)
        e_re.pack(side="left", padx=(10, 0))
        self.recharge_entry = e_re
        tk.Label(cfg_row, text="元", font=(AppleTheme.FONT, 10),
                 fg=AppleTheme.TEXT_SEC, bg=AppleTheme.CARD).pack(side="left", padx=(4, 0))
        e_re.bind("<FocusOut>", lambda e: self._recompute())
        e_re.bind("<Return>", lambda e: self._recompute())

        price_row = tk.Frame(api_card.inner, bg=AppleTheme.CARD)
        price_row.pack(fill="x", padx=16, pady=(2, 6))
        tk.Label(price_row, text="单价", width=6, anchor="w", font=(AppleTheme.FONT, 11),
                 fg=AppleTheme.TEXT_SEC, bg=AppleTheme.CARD).pack(side="left")
        e_pr = tk.Entry(price_row, textvariable=self.price_var, font=(AppleTheme.MONO, 10),
                        bg=AppleTheme.CARD_ALT, fg=AppleTheme.TEXT, relief="flat", bd=6, width=8)
        self.entry_price = e_pr
        e_pr.pack(side="left", padx=(10, 0))
        tk.Label(price_row, text="元/百万Tk", font=(AppleTheme.FONT, 10),
                 fg=AppleTheme.TEXT_SEC, bg=AppleTheme.CARD).pack(side="left", padx=(4, 0))
        e_pr.bind("<FocusOut>", lambda e: self._on_price_edit())
        e_pr.bind("<Return>", lambda e: self._on_price_edit())
        # 锁定/随动 切换按钮：把当前单价(手填或反推值)钉为最高优先锚点，之后随余额实时反推 Tokens
        self._btn_price_lock = tk.Button(
            price_row, text="锁定单价", font=(AppleTheme.FONT, 9),
            fg=AppleTheme.ACCENT, bg=AppleTheme.CARD, bd=0, cursor="hand2",
            activebackground=AppleTheme.CARD, activeforeground=AppleTheme.ACCENT,
            command=self._toggle_price_lock)
        self._btn_price_lock.pack(side="right")

        tk.Frame(api_card.inner, bg=AppleTheme.SEPARATOR, height=1).pack(fill="x", padx=16, pady=(4, 2))

        # Tokens：接口取到则自动锁定(灰)，取不到可手填；三项按算法自动联动
        t_row = tk.Frame(api_card.inner, bg=AppleTheme.CARD)
        t_row.pack(fill="x", padx=16, pady=6)
        tk.Label(t_row, text="Tokens", width=6, anchor="w", font=(AppleTheme.FONT, 11),
                 fg=AppleTheme.TEXT_SEC, bg=AppleTheme.CARD).pack(side="left")
        self.entry_tokens = tk.Entry(t_row, textvariable=self.tokens_var, font=(AppleTheme.MONO, 10),
                 bg=AppleTheme.CARD_ALT, fg=AppleTheme.TEXT, relief="flat", bd=6)
        self.entry_tokens.pack(side="left", fill="x", expand=True, padx=(10, 0))
        self._bind_manual("tokens", self.entry_tokens)

        # 已消费
        c_row = tk.Frame(api_card.inner, bg=AppleTheme.CARD)
        c_row.pack(fill="x", padx=16, pady=6)
        tk.Label(c_row, text="已消费", width=6, anchor="w", font=(AppleTheme.FONT, 11),
                 fg=AppleTheme.TEXT_SEC, bg=AppleTheme.CARD).pack(side="left")
        self.entry_cost = tk.Entry(c_row, textvariable=self.cost_var, font=(AppleTheme.MONO, 10),
                 bg=AppleTheme.CARD_ALT, fg=AppleTheme.TEXT, relief="flat", bd=6)
        self.entry_cost.pack(side="left", fill="x", expand=True, padx=(10, 0))
        tk.Label(c_row, text="元", font=(AppleTheme.FONT, 10),
                 fg=AppleTheme.TEXT_SEC, bg=AppleTheme.CARD).pack(side="left", padx=(4, 0))
        self._bind_manual("cost", self.entry_cost)

        # 余额
        b_row = tk.Frame(api_card.inner, bg=AppleTheme.CARD)
        b_row.pack(fill="x", padx=16, pady=(6, 14))
        tk.Label(b_row, text="余额", width=6, anchor="w", font=(AppleTheme.FONT, 11),
                 fg=AppleTheme.TEXT_SEC, bg=AppleTheme.CARD).pack(side="left")
        self.entry_balance = tk.Entry(b_row, textvariable=self.balance_var, font=(AppleTheme.MONO, 10),
                 bg=AppleTheme.CARD_ALT, fg=AppleTheme.TEXT, relief="flat", bd=6)
        self.entry_balance.pack(side="left", fill="x", expand=True, padx=(10, 0))
        tk.Label(b_row, text="元", font=(AppleTheme.FONT, 10),
                 fg=AppleTheme.TEXT_SEC, bg=AppleTheme.CARD).pack(side="left", padx=(4, 0))
        self._bind_manual("balance", self.entry_balance)

        self.log_label = tk.Label(
            self.root, text="就绪", font=(AppleTheme.FONT, 9),
            fg=AppleTheme.TEXT_SEC, bg=AppleTheme.BG, anchor="w"
        )
        self.log_label.pack(fill="x", padx=24, pady=(0, 10))

        # 点击空白区域时，将焦点从输入框移走，触发 FocusOut → _recompute
        self.root.bind("<Button-1>", self._on_root_click)

    def _on_root_click(self, event):
        """点击空白区域时，将焦点从输入框移走，触发 FocusOut → _recompute"""
        clicked = event.widget
        input_widgets = {self.entry_tokens, self.entry_cost, self.entry_balance,
                       self.entry_price, self.recharge_entry}
        # 点的是输入框本身，不处理
        if clicked in input_widgets:
            return
        # 点的是按钮/复选框等交互控件，不处理
        if isinstance(clicked, (tk.Button, tk.Checkbutton)):
            return
        # 当前焦点在输入框上 → 把焦点移到 root，触发 FocusOut
        current = self.root.focus_get()
        if current in input_widgets:
            self.root.focus_set()

    def _make_row(self, parent, name, color):
        row = tk.Frame(parent, bg=AppleTheme.CARD)
        row.pack(fill="x", padx=16, pady=5)
        tk.Label(row, text=name, width=6, anchor="w", font=(AppleTheme.FONT, 11),
                 fg=AppleTheme.TEXT, bg=AppleTheme.CARD).pack(side="left")
        bar = AppleBar(row, height=8, color=color)
        bar.pack(side="left", fill="x", expand=True, padx=(10, 10))
        val = tk.Label(row, text="--%", width=6, anchor="e",
                       font=(AppleTheme.MONO, 10),
                       fg=AppleTheme.TEXT, bg=AppleTheme.CARD)
        val.pack(side="right")
        return {"bar": bar, "val": val, "color": color, "last_val": None}

    # ------------------------------------------------------
    # 交互逻辑
    # ------------------------------------------------------
    # ---------- API 用量自动刷新 ----------
    def _start_api_thread(self):
        self._api_stop = threading.Event()
        # 真暂停开关：勾选=set(运行)，取消=clear(无限期阻塞，0 CPU)
        self._refresh_event = threading.Event()
        try:
            if self.api_auto_var.get():
                self._refresh_event.set()
        except Exception:
            self._refresh_event.set()
        threading.Thread(target=self._api_loop, daemon=True).start()

    def _on_auto_toggle(self):
        # 勾选框切换：真暂停/真运行
        if not hasattr(self, "_refresh_event"):
            return
        if self.api_auto_var.get():
            # 勾选：唤醒后台线程，并立即拉取一次后恢复 10 秒定时循环
            self._refresh_event.set()
            self._log("自动刷新已开启（立即同步一次）")
        else:
            # 取消：后台线程下一轮进入无限期阻塞，彻底停止唤醒
            self._refresh_event.clear()
            self._log("自动刷新已暂停（后台线程已休眠）")

    def _api_loop(self):
        while not self._api_stop.is_set():
            # 真暂停核心：未勾选时在此无限期阻塞，不消耗 CPU
            self._refresh_event.wait()
            if self._api_stop.is_set():
                break
            try:
                key = self.api_key_var.get().strip()
                if key:
                    data = fetch_api_usage(key, self.base_url_var.get())
                    self.root.after(0, self._apply_api_data, data)
            except Exception:
                pass
            # 周期等待；若期间取消勾选，下一轮 _refresh_event.wait() 即阻塞
            self._api_stop.wait(API_REFRESH_INTERVAL)

    def _refresh_api_now(self):
        # 网络拉取 + 本地重算：点击后请求 API 拿最新余额，拿到后强制更新显示框 + 触发 _recompute
        # 防抖：按钮变灰 + 显示"刷新中..."
        self.refresh_btn_now.config(text="刷新中...", state="disabled")
        self._log("正在请求 API 获取最新余额...")

        def _fetch_and_apply():
            try:
                key = self.api_key_var.get().strip()
                base_url = self.base_url_var.get()
                if key:
                    data = fetch_api_usage(key, base_url)
                    # 回到主线程更新 UI
                    self.root.after(0, self._apply_api_data, data)
                else:
                    self.root.after(0, lambda: self._log("未配置 API Key，无法刷新"))
            except Exception as e:
                self.root.after(0, lambda: self._log(f"刷新失败：{e}"))
            finally:
                # 无论成功/失败，恢复按钮
                self.root.after(0, self._restore_refresh_btn)

        threading.Thread(target=_fetch_and_apply, daemon=True).start()

    def _restore_refresh_btn(self):
        self.refresh_btn_now.config(text="立即刷新", state="normal")

    def _apply_api_data(self, data):
        if not data:
            return
        provided = {"balance": data.get("balance"),
                    "cost": data.get("cost"),
                    "tokens": data.get("tokens")}
        got = []
        for f in ("tokens", "cost", "balance"):
            v = provided[f]
            if v is None:
                # 本轮接口没给该项：若此前是实时值则退回可编辑(自动计算)态，便于手填
                if self.field_mode.get(f) == "api":
                    self.field_mode[f] = ""
                    self._apply_mode(f)
                continue
            got.append(f)
            self.field_mode[f] = "api"
            if f == "tokens":
                self.tokens_var.set(str(int(v)))
            else:
                self._var_of(f).set("%.2f" % v)
            self._apply_mode(f)
        self._recompute()
        if data.get("error"):
            self._log("查询失败：" + str(data["error"]) + "（可手填任意一项自动计算其余）")
        elif not got:
            self._log("接口未返回可用数据，可手填 Tokens/已消费/余额 任一自动计算")
        else:
            self._log("已实时刷新，未提供项按算法自动计算")

    # ---------- 手填 / 自动计算引擎 ----------
    def _var_of(self, field):
        return {"tokens": self.tokens_var, "cost": self.cost_var,
                "balance": self.balance_var}[field]

    def _entry_widget(self, field):
        return {"tokens": self.entry_tokens, "cost": self.entry_cost,
                "balance": self.entry_balance}[field]

    def _bind_manual(self, field, entry):
        entry.bind("<FocusOut>", lambda e, f=field: self._on_manual_edit(f))
        entry.bind("<Return>", lambda e, f=field: self._on_manual_edit(f))

    def _parse_num(self, var, cast=float):
        s = var.get().strip()
        if s in ("", "--"):
            return None
        try:
            return cast(s)
        except (ValueError, TypeError):
            return None

    def _on_manual_edit(self, field):
        if self._updating:
            return
        v = self._parse_num(self._var_of(field), int if field == "tokens" else float)
        if v is None:
            self.field_mode[field] = ""
            self._apply_mode(field)
            self._recompute()
            return
        self.field_mode[field] = "manual"
        self._apply_mode(field)
        self._recompute(base=field)

    # ---------- 关窗持久化：保存/加载 上次关闭时的接口与单价等 ----------
    def _load_config(self):
        cfg = {}
        raw = {}
        try:
            if os.path.exists(CONFIG_PATH):
                with open(CONFIG_PATH, "r", encoding="utf-8") as f:
                    raw = json.load(f)
            cfg["base_url"] = raw.get("base_url")
            cfg["recharge"] = raw.get("recharge")
            cfg["price"] = raw.get("price")
        except Exception:
            raw = {}
        # Key 读取：优先系统密钥库(加密、绑定当前账户)，取不到再回退 json 里的旧 Base64
        key = None
        if _HAS_KEYRING:
            try:
                key = keyring.get_password(KEYRING_SERVICE, KEYRING_ACCOUNT)
            except Exception:
                key = None
        if key is None:
            ak = raw.get("api_key_b64")
            if ak:
                try:
                    key = base64.b64decode(ak.encode()).decode("utf-8", "replace")
                except Exception:
                    key = None
        cfg["api_key"] = key or ""
        return cfg

    def _save_config(self):
        try:
            key = self.api_key_var.get().strip()
            # 不敏感项仍写 json；Key 不写 json(明文清空)，改存系统密钥库
            raw = {
                "base_url": self.base_url_var.get(),
                "recharge": self.recharge_var.get(),
                "price": self.price_var.get(),
                "api_key_b64": "",
            }
            with open(CONFIG_PATH, "w", encoding="utf-8") as f:
                json.dump(raw, f, ensure_ascii=False, indent=2)
        except Exception:
            pass
        # Key 持久化
        try:
            key = self.api_key_var.get().strip()
            if _HAS_KEYRING:
                if key:
                    keyring.set_password(KEYRING_SERVICE, KEYRING_ACCOUNT, key)
                else:
                    try:
                        keyring.delete_password(KEYRING_SERVICE, KEYRING_ACCOUNT)
                    except Exception:
                        pass
                self._keyring_note = "API Key 已加密存入系统密钥库"
            else:
                # 未安装 keyring：退回 json 内 Base64(可逆，注意分享时排除 json)
                try:
                    raw = {}
                    if os.path.exists(CONFIG_PATH):
                        with open(CONFIG_PATH, "r", encoding="utf-8") as f:
                            raw = json.load(f)
                    raw["api_key_b64"] = (base64.b64encode(key.encode("utf-8")).decode() if key else "")
                    with open(CONFIG_PATH, "w", encoding="utf-8") as f:
                        json.dump(raw, f, ensure_ascii=False, indent=2)
                    self._keyring_note = "未安装 keyring，Key 以 Base64 存于 json(建议 pip install keyring)"
                except Exception:
                    pass
        except Exception:
            pass

    def _toggle_price_lock(self):
        """锁定/解锁单价：
        - 未锁定 -> 取当前单价(反推值或手填值)钉为 manual 最高优先锚点，之后 API 刷新余额时
          用固定单价 + 消耗额(R-B) 实时反推 Tokens；
        - 已锁定 -> 清空锁定，恢复“随动反推”（单价重新变灰色只读随动）。"""
        if getattr(self, "_updating", False):
            return
        if self.field_mode.get("price") == "manual":
            self.field_mode["price"] = ""
            try:
                self._btn_price_lock.config(text="锁定单价")
            except Exception:
                pass
            self._log("单价已解锁，恢复随动反推")
        else:
            p = self._parse_num(self.price_var, float)
            if p is None or p <= 0:
                self._log("单价无效，无法锁定（请先填或等反推出有效单价）")
                return
            self.field_mode["price"] = "manual"
            try:
                self._btn_price_lock.config(text="已锁定·点解锁")
            except Exception:
                pass
            self._log("单价已锁定：将按固定单价随消耗实时反推 Tokens")
        self._apply_price_mode()
        self._recompute()

    def _on_price_edit(self):
        # 用户手动改单价：有值记为 manual(锚点)，清空则退出锚点，再统一重算
        if getattr(self, "_updating", False):
            return
        v = self._parse_num(self.price_var, float)
        if v is None or v <= 0:
            self.field_mode["price"] = ""
        else:
            self.field_mode["price"] = "manual"
        self._apply_price_mode()
        self._recompute()

    def _apply_price_mode(self):
        # calc(随动反推)=灰色只读；其余(空/manual)=白色可编辑
        try:
            if self.field_mode.get("price") == "calc":
                self.entry_price.config(state="readonly", fg=AppleTheme.TEXT_SEC)
            else:
                self.entry_price.config(state="normal", fg=AppleTheme.TEXT)
        except tk.TclError:
            pass

    def _eff_price(self):
        # 单价(元/百万 Tokens) -> 元/Token；非法或 0 返回 0
        p = self._parse_num(self.price_var, float)
        return (p / 1_000_000.0) if p and p > 0 else 0.0

    def _eff_recharge(self):
        # 用户填了充值(>0)优先；否则当「已消费」「余额」都已知时自动推断 R=已消费+余额
        r = self._parse_num(self.recharge_var, float)
        if r is not None and r > 0:
            return r
        c = self._parse_num(self.cost_var, float)
        b = self._parse_num(self.balance_var, float)
        if c is not None and b is not None:
            return c + b
        return 0.0

    def _recompute(self, base=None):
        """核心联动（绝对联动）：
        - 单价锁定(price=manual 锚点)：以 充值R / 余额B / 单价P 为锚，
          已消费 C = R - B，Tokens = C*1e6/P；改充值或余额都会实时重算 C 与 Tokens。
        - 未锁定(单价随动)：以 Tokens / 已消费 / 余额 中的已知项为锚，单价随动反推。
        两条等式恒成立：已消费 = Tokens*单价/1e6 ；余额 = 充值 - 已消费。
        """
        if getattr(self, "_updating", False):
            return
        self._updating = True
        try:
            p_user = self._parse_num(self.price_var, float)
            price_locked = (self.field_mode.get("price") == "manual") and p_user and p_user > 0

            R = self._eff_recharge()
            R_has = R is not None and R > 0
            B = self._parse_num(self.balance_var, float) if self.field_mode.get("balance") in ("api", "manual") else None

            # ===== 单价锁定模式：R / B / P 为锚，C 与 Tokens 均为派生量 =====
            if price_locked:
                p_per_token = p_user / 1_000_000.0
                # 已消费优先由 R-B 得出（实时消耗的权威来源）
                if R_has and B is not None:
                    C = R - B
                    cost_is_derived = True
                else:
                    C = self._parse_num(self.cost_var, float) if self.field_mode.get("cost") in ("api", "manual") else None
                    cost_is_derived = False
                # Tokens 始终由固定单价实时反推：T = C*1e6/P
                T = int(C / p_per_token) if (C is not None and C >= 0) else None
                vals = {"tokens": T, "cost": C, "balance": B}
                if vals["balance"] is None and R_has and vals["cost"] is not None:
                    vals["balance"] = R - vals["cost"]
                # 锁定单价时 Tokens 强制为派生量：即使它是 api/manual 也覆盖，
                # 这样"改充值 / API 刷新余额 -> Tokens 实时跳动"才成立。
                known_tokens = set()
                known_cost = set() if cost_is_derived else ({"cost"} if self.field_mode.get("cost") in ("api", "manual") else set())
                known_balance = {"balance"} if self.field_mode.get("balance") in ("api", "manual") else set()
                self._set_computed("tokens", vals["tokens"], known_tokens)
                self._set_computed("cost", vals["cost"], known_cost)
                self._set_computed("balance", vals["balance"], known_balance)
                # 单价钉死为锁定值，不写回、不改状态
                self._apply_price_mode()
                return

            # ===== 未锁定：单价随动反推 =====
            T = self._parse_num(self.tokens_var, int) if self.field_mode.get("tokens") in ("api", "manual") else None
            C = self._parse_num(self.cost_var, float) if self.field_mode.get("cost") in ("api", "manual") else None
            price_derivable = False
            P = None
            if T is not None and T > 0 and (C is not None or (R_has and B is not None)):
                price_derivable = True
                P = C * 1_000_000.0 / T if C is not None else (R - B) * 1_000_000.0 / T
            p_per_token = (P / 1_000_000.0) if (P and P > 0) else 0.0

            vals = {"tokens": T, "cost": C, "balance": B}
            for _ in range(4):
                chg = False
                if vals["tokens"] is not None and p_per_token > 0 and vals["cost"] is None:
                    vals["cost"] = vals["tokens"] * p_per_token; chg = True
                if vals["cost"] is not None and p_per_token > 0 and vals["tokens"] is None:
                    vals["tokens"] = int(vals["cost"] / p_per_token); chg = True
                if R_has and vals["cost"] is not None and vals["balance"] is None:
                    vals["balance"] = R - vals["cost"]; chg = True
                if R_has and vals["balance"] is not None and vals["cost"] is None:
                    vals["cost"] = R - vals["balance"]; chg = True
                if not chg:
                    break

            known = {f for f in ("tokens", "cost", "balance")
                     if self.field_mode.get(f) in ("api", "manual")}
            self._set_computed("tokens", vals["tokens"], known)
            self._set_computed("cost", vals["cost"], known)
            self._set_computed("balance", vals["balance"], known)

            if price_derivable and P is not None:
                if self._focused_widget() is not self.entry_price:
                    self.price_var.set("%.2f" % P)
                self.field_mode["price"] = "calc"
            elif self.field_mode.get("price") == "calc":
                self.field_mode["price"] = ""
            self._apply_price_mode()
        finally:
            self._updating = False

    def _set_computed(self, field, value, known):
        if field in known:      # 已知项(API/手填)不被覆盖
            return
        if self._focused_widget() is self._entry_widget(field):
            return              # 用户正在此框输入时不打断
        var = self._var_of(field)
        if value is None:
            var.set("--")
            self.field_mode[field] = ""
        else:
            var.set(str(int(value)) if field == "tokens" else "%.2f" % value)
            self.field_mode[field] = "calc"
        self._apply_mode(field)

    def _focused_widget(self):
        try:
            return self.root.focus_get()
        except Exception:
            return None

    def _apply_mode(self, field):
        """api=灰色只读(系统实时接管)；manual/calc/空=白色可编辑(可手填)"""
        entry = self._entry_widget(field)
        try:
            if self.field_mode.get(field, "") == "api":
                entry.config(state="readonly", fg=AppleTheme.TEXT_SEC)
            else:
                entry.config(state="normal", fg=AppleTheme.TEXT)
        except tk.TclError:
            pass

    @staticmethod
    def _num(var, cast=float):
        """安全解析输入框数字，失败返回 0"""
        try:
            return cast(var.get().strip())
        except (ValueError, TypeError):
            return 0

    def _refresh_ports(self):
        ports = list(serial.tools.list_ports.comports())
        items = [f"{p.device} - {p.description}" for p in ports]
        self.port_combo["values"] = items
        if items:
            self.port_combo.current(0)
            self._log(f"检测到 {len(items)} 个串口")
        else:
            self.port_combo.set("")
            self._log("未检测到串口设备")

    def _log(self, msg):
        self.log_label.config(text=msg)

    def _set_status(self, text, color):
        self.status_dot.config(text=f"● {text}", fg=color)

    # ------------------------------------------------------
    # 监控开关
    # ------------------------------------------------------
    def _toggle_monitor(self):
        if self.monitoring:
            self._stop_monitor()
        else:
            self._start_monitor()

    def _start_monitor(self):
        sel = self.port_combo.get()
        if not sel:
            messagebox.showwarning("提示", "请先选择一个串口")
            return
        port = sel.split(" - ")[0].strip()
        try:
            self.serial_conn = serial.Serial(port, 115200, timeout=1)
            time.sleep(2)
        except Exception as e:
            messagebox.showerror("连接失败", f"无法打开串口 {port}\n\n{e}")
            self.serial_conn = None
            return

        # 这里必须真的把发送线程起起来。
        # v3.9 之前只改了按钮文案、从没建过这个线程（self.monitor_thread 在
        # __init__ 里设成 None 就再没被赋过值），所以界面写着"开始推送数据"，
        # 实际一行 JSON 都没往串口写出去——板子那边永远等不到数据。
        self.monitor_thread = threading.Thread(target=self._monitor_loop, daemon=True)
        self.monitor_thread.start()

        # 这个标志是按钮"按一下开、再按一下关"的依据，**必须在这里置上**。
        # v3.9 之前它只在 __init__ 里被设成 False、再没被赋过值，于是
        # _toggle_monitor() 永远走 _start_monitor()：点"停止传递数据"其实是
        # 再去开一次同一个串口，系统报"端口被占用"，弹出来的是
        # "连接失败 / 无法打开串口 xxx" —— 看着像串口坏了，其实是按钮没接上。
        self.monitoring = True

        self.connect_btn.config(text="停止传递数据", bg=AppleTheme.RED,
                                activebackground="#CC2E26")
        self._set_status("Monitoring", AppleTheme.GREEN)
        self._log(f"已连接 {port}，开始推送数据")

    def _stop_monitor(self):
        # 先清标志再关串口：反过来的话，这中间点一下按钮会又走一遍 _start_monitor()
        self.monitoring = False
        try:
            if self.serial_conn and self.serial_conn.is_open:
                self.serial_conn.close()
        except Exception:
            pass
        self.serial_conn = None
        # 等发送线程自己看到串口关了退出，免得它和下一次"开始"叠在一起跑
        if self.monitor_thread is not None:
            try:
                self.monitor_thread.join(timeout=2.0)
            except Exception:
                pass
            self.monitor_thread = None
        self.connect_btn.config(text="开始传递数据", bg=AppleTheme.ACCENT,
                                activebackground="#0066CC")
        self._set_status("Ready", AppleTheme.TEXT_SEC)
        self._log("串口已断开，监控数据持续刷新中")

    # ------------------------------------------------------
    # 监控主循环
    # ------------------------------------------------------
    def _monitor_loop(self):
        """串口数据发送循环（仅在串口连接时运行）"""
        # 网络计数基线**必须自己记一份**，不能和 _system_monitor_loop 共用
        # self._last_net / self._last_time：两条循环都是一秒一拍，共用的话
        # 对方刚更新过这里，dt 就接近 0，算出来的"KB/s"会大得离谱。
        last_net = psutil.net_io_counters()
        last_time = time.time()

        while self.serial_conn and self.serial_conn.is_open:
            try:
                snap = get_system_snapshot()
                now = time.time()
                net = psutil.net_io_counters()
                dt = max(now - last_time, 0.001)
                up_speed = (net.bytes_sent - last_net.bytes_sent) / dt
                down_speed = (net.bytes_recv - last_net.bytes_recv) / dt
                last_net = net
                last_time = now

                payload = {
                    "type": "status",
                    "host": snap["host"],
                    "cpu": snap["cpu"],
                    "ram": snap["ram"],
                    "gpu": snap["gpu"],
                    "net_up": round(up_speed / 1024, 1),
                    "net_down": round(down_speed / 1024, 1),
                    "battery": snap["battery"],
                    "api": {
                        "base_url": self.base_url_var.get(),
                        "key_set": bool(self.api_key_var.get().strip()),
                        "tokens": self._num(self.tokens_var, int),
                        "cost": round(self._num(self.cost_var, float), 4),
                        "balance": round(self._num(self.balance_var, float), 4),
                    },
                    "ts": int(now),
                }
                line = json.dumps(payload, ensure_ascii=False) + "\n"
                if self.serial_conn and self.serial_conn.is_open:
                    self.serial_conn.write(line.encode("utf-8"))
            except Exception as e:
                self.root.after(0, self._log, f"发送错误: {e}")
            time.sleep(1.0)

    def _start_system_monitor(self):
        """启动系统监控线程（独立于串口，持续刷新UI）"""
        net = psutil.net_io_counters()
        self._last_net = (net.bytes_sent, net.bytes_recv)
        self._last_time = time.time()
        t = threading.Thread(target=self._system_monitor_loop, daemon=True)
        t.start()

    def _system_monitor_loop(self):
        """系统监控主循环：持续获取硬件数据并更新UI，不依赖串口"""
        while self._system_monitor_running:
            try:
                snap = get_system_snapshot()
                now = time.time()
                net = psutil.net_io_counters()
                dt = max(now - self._last_time, 0.001)
                up_speed = (net.bytes_sent - self._last_net[0]) / dt
                down_speed = (net.bytes_recv - self._last_net[1]) / dt
                self._last_net = (net.bytes_sent, net.bytes_recv)
                self._last_time = now

                self.root.after(0, self._update_ui, snap, up_speed, down_speed)
            except Exception as e:
                self.root.after(0, self._log, f"监控错误: {e}")
            time.sleep(1.0)

    def _update_ui(self, snap, up_speed, down_speed):
        self._set_bar(self.cpu_bar, snap["cpu"])
        self._set_bar(self.ram_bar, snap["ram"])
        if _GPU_AVAILABLE and snap["gpu"] >= 0:
            self._set_bar(self.gpu_bar, snap["gpu"])
        else:
            self._set_bar(self.gpu_bar, 0, value_text="N/A")

        text = f"↑ {up_speed/1024:6.1f} KB/s ↓ {down_speed/1024:6.1f} KB/s"
        if text != self._last_net_text:
            self._last_net_text = text
            self.net_label.config(text=text)

    @staticmethod
    def _set_bar(item, value, value_text=None):
        if value >= 80:
            color = AppleTheme.RED
        elif value >= 60:
            color = AppleTheme.ORANGE
        else:
            color = item["color"]
        item["bar"].set(value, color)
        txt = value_text if value_text is not None else f"{int(round(value))}%"
        if item.get("last_val") != txt:
            item["last_val"] = txt
            item["val"].config(text=txt)

# ==========================================================
# 入口
# ==========================================================
def main():
    root = tk.Tk()
    try:
        from ctypes import windll
        windll.shcore.SetProcessDpiAwareness(1)
    except Exception:
        pass

    app = EcamDashboard(root)

    def on_close():
        try:
            app._save_config()
        except Exception:
            pass
        app._system_monitor_running = False
        if app.monitoring:
            app._stop_monitor()
        try:
            app._api_stop.set()
        except Exception:
            pass
        root.destroy()

    root.protocol("WM_DELETE_WINDOW", on_close)
    root.mainloop()

if __name__ == "__main__":
    main()