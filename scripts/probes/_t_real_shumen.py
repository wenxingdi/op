# -*- coding: utf-8 -*-
"""蜀门真机「剩余项」验收脚本（承接阶段 5 的 37 PASS，只测此前没在真机上跑过的项）。

分组：
  G1 绑定矩阵  —— gdi / gdi2 / normal / dx / dx·dx·dx / dx.d3d11 六通道，含 rebind、unbind
  G2 解绑干净度 —— 目标进程模块表 before/after + 多轮 bind/unbind + 解绑后调用行为（不得崩）
  G3 图色/OCR/帧 —— get_color / cmp_color / find_color / get_color_num / find_color_ex /
                    frame_info / fps / autoocr_line / autoocr_ex / ocr_from_file 一致性
  G4 键鼠      —— **默认关闭**，需显式 --input（会真的向游戏发送输入）

用法：
  python scripts/probes/_t_real_shumen.py                     # 跑 G1~G3
  python scripts/probes/_t_real_shumen.py --groups G1,G2
  python scripts/probes/_t_real_shumen.py --input             # 追加 G4（有副作用）

判据一律用可证伪的观测量：唯一色数、命中坐标、模块表差集、Win32 状态、异常与否。
"""
import argparse
import ctypes
import ctypes.wintypes as wt
import os
import struct
import sys
import time
from pathlib import Path

REPO = Path(r"D:\AutoPro\op-master\op")
DLL_DIR = REPO / "build" / "nmake-x64-Release" / "libop"
OUT_DIR = REPO / "workbench" / "probes"
sys.path.insert(0, str(REPO / "bindings" / "python"))

from op import Op  # noqa: E402

TS = time.strftime("%Y%m%d_%H%M%S")
SHOT = OUT_DIR / f"real_shumen_{TS}"
RES = []          # (group, api, detail, status)
GRP = ["G0"]
_logf = None


def log(msg=""):
    print(msg)
    if _logf:
        _logf.write(str(msg) + "\n")
        _logf.flush()


def sec(name):
    GRP[0] = name.split()[0]
    log()
    log("=" * 100)
    log(name)
    log("=" * 100)


def rec(api, detail, status):
    RES.append((GRP[0], api, detail, status))
    mark = {"PASS": "  [PASS]", "FAIL": "[FAIL] ", "INFO": "  [INFO]", "SKIP": "  [SKIP]"}.get(status, "  [?]")
    log("%s %-58s %s" % (mark, api, detail))


def T(api, fn, ok=lambda r: bool(r), show=lambda r: r, soft=False):
    """软断言：soft=True 时失败记 INFO（用于可证伪预测/环境项）。"""
    try:
        r = fn()
    except Exception as e:
        rec(api, "EXC: %r" % (e,), "FAIL")
        return None
    try:
        good = bool(ok(r))
    except Exception:
        good = False
    rec(api, show(r), "PASS" if good else ("INFO" if soft else "FAIL"))
    return r


# ---------------------------------------------------------------- 工具
def dpi_aware():
    u = ctypes.windll.user32
    try:
        u.SetProcessDpiAwarenessContext.argtypes = [ctypes.c_void_p]
        u.SetProcessDpiAwarenessContext.restype = ctypes.c_bool
        u.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))
        return "PER_MONITOR_AWARE_V2"
    except Exception:
        u.SetProcessDPIAware()
        return "SYSTEM_AWARE"


def find_pid(name=b"client.exe"):
    k32 = ctypes.windll.kernel32

    class PE(ctypes.Structure):
        _fields_ = [("dwSize", wt.DWORD), ("cntUsage", wt.DWORD), ("th32ProcessID", wt.DWORD),
                    ("th32DefaultHeapID", ctypes.c_void_p), ("th32ModuleID", wt.DWORD),
                    ("cntThreads", wt.DWORD), ("th32ParentProcessID", wt.DWORD),
                    ("pcPriClassBase", ctypes.c_long), ("dwFlags", wt.DWORD),
                    ("szExeFile", ctypes.c_char * 260)]

    snap = k32.CreateToolhelp32Snapshot(0x2, 0)
    if snap == -1:
        return []
    pe = PE()
    pe.dwSize = ctypes.sizeof(pe)
    out = []
    ok = k32.Process32First(snap, ctypes.byref(pe))
    while ok:
        if pe.szExeFile.lower() == name:
            out.append(pe.th32ProcessID)
        ok = k32.Process32Next(snap, ctypes.byref(pe))
    k32.CloseHandle(snap)
    return out


def enum_windows(pid=None, title_kw=None):
    u = ctypes.windll.user32
    u.IsWindowVisible.argtypes = [ctypes.c_void_p]
    u.GetWindowTextLengthW.argtypes = [ctypes.c_void_p]
    u.GetWindowTextW.argtypes = [ctypes.c_void_p, ctypes.c_wchar_p, ctypes.c_int]
    u.GetClassNameW.argtypes = [ctypes.c_void_p, ctypes.c_wchar_p, ctypes.c_int]
    u.EnumWindows.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
    found = []
    CB = ctypes.CFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)

    def cb(h, _):
        h = ctypes.c_void_p(h)
        if not u.IsWindowVisible(h):
            return True
        wpid = wt.DWORD(0)
        u.GetWindowThreadProcessId(h, ctypes.byref(wpid))
        if pid and wpid.value != pid:
            return True
        n = u.GetWindowTextLengthW(h)
        if n <= 0:
            return True
        b = ctypes.create_unicode_buffer(n + 2)
        u.GetWindowTextW(h, b, n + 2)
        if title_kw and title_kw not in b.value:
            return True
        c = ctypes.create_unicode_buffer(256)
        u.GetClassNameW(h, c, 256)
        found.append((int(h.value or 0), b.value, c.value))
        return True

    u.EnumWindows(CB(cb), 0)
    return found


MODULE_SNAP_FLAGS = 0x8 | 0x10  # TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32


def modules_of(pid):
    """目标进程模块表（32 位目标须带 SNAPMODULE32，否则拿不到）。"""
    k32 = ctypes.windll.kernel32

    class ME(ctypes.Structure):
        _fields_ = [("dwSize", wt.DWORD), ("th32ModuleID", wt.DWORD), ("th32ProcessID", wt.DWORD),
                    ("GlblcntUsage", wt.DWORD), ("ProccntUsage", wt.DWORD),
                    ("modBaseAddr", ctypes.c_void_p), ("modBaseSize", wt.DWORD),
                    ("hModule", ctypes.c_void_p), ("szModule", ctypes.c_wchar * 256),
                    ("szExePath", ctypes.c_wchar * 260)]

    snap = k32.CreateToolhelp32Snapshot(MODULE_SNAP_FLAGS, pid)
    if snap in (-1, 0xFFFFFFFF):
        return None
    me = ME()
    me.dwSize = ctypes.sizeof(me)
    out = {}
    ok = k32.Module32FirstW(snap, ctypes.byref(me))
    while ok:
        out[me.szModule.lower()] = int(me.modBaseSize)
        ok = k32.Module32NextW(snap, ctypes.byref(me))
    k32.CloseHandle(snap)
    return out


def proc_alive(pid):
    k32 = ctypes.windll.kernel32
    h = k32.OpenProcess(0x1000, False, pid)
    if not h:
        return False
    code = wt.DWORD(0)
    got = k32.GetExitCodeProcess(h, ctypes.byref(code))
    k32.CloseHandle(h)
    return bool(got) and code.value == 259  # STILL_ACTIVE


def bmp_info(path):
    b = open(path, "rb").read()
    off = struct.unpack_from("<I", b, 10)[0]
    w, h = struct.unpack_from("<ii", b, 18)
    bpp = struct.unpack_from("<H", b, 28)[0]
    return w, abs(h), bpp, len(b), off


def uniq_colors(path, stride=4):
    """BMP 像素唯一色数（粗排，用于判「截到的是真画面还是黑/空白」）。"""
    b = open(path, "rb").read()
    off = struct.unpack_from("<I", b, 10)[0]
    body = b[off:]
    cols = set()
    for i in range(0, len(body) - stride + 1, stride):
        cols.add(body[i:i + stride])
    return len(cols)


def v_of(op):
    v = op.version
    return v() if callable(v) else v


# ---------------------------------------------------------------- G1
COMBOS = [
    ("gdi", "windows", "windows", True),
    ("gdi2", "windows", "windows", True),
    ("normal", "windows", "windows", True),
    ("dx", "windows", "windows", True),
    ("dx", "dx", "dx", True),
    # 可证伪预测：dx.d3d11 装的是同一个 IDXGISwapChain::Present，但取图要 ID3D11Resource
    # → 对 D3D9 游戏应「绑得上、看不到帧」，故此处只作观察项（soft）
    ("dx.d3d11", "windows", "windows", False),
]


def g1(op, hwnd, cw, ch):
    sec("G1 绑定矩阵（六通道：bind → 截图 → rebind → unbind）")
    shots = {}
    for disp, mouse, key, must_frame in COMBOS:
        tag = "%s/%s/%s" % (disp, mouse, key)
        log("")
        log("[%s] ---" % tag)
        try:
            r = op.bind_window(hwnd, disp, mouse, key, 0)
            ib = op.is_bind()
            gw = op.get_bind_window()
            log("  bind ret=%s is_bind=%s get_bind_window=%s" % (r, ib, "OK" if gw == hwnd else hex(gw or 0)))
        except Exception as e:
            rec(tag + " bind", "EXC %r" % (e,), "FAIL")
            continue
        if not r:
            rec(tag, "BIND-FAIL（ret=0）", "FAIL" if must_frame else "INFO")
            try:
                op.unbind_window()
            except Exception:
                pass
            continue

        # 单变量控制：每通道截图前先把目标**前台化**，彻底消除「被别的窗口遮挡」这个环境干扰。
        # （normal 是桌面级捕获，被遮挡时会截到遮挡者 —— 那是环境条件，不是通道能力；
        #   前台化后各通道的 uniq 才可直接横向比较。）
        try:
            op.set_window_state(hwnd, 1)  # 1 = 激活/前台
            time.sleep(0.5)
            fg0 = ctypes.windll.user32.GetForegroundWindow()
            log("  前台化: GetForegroundWindow=%s 期望=%s%s" % (hex(int(fg0 or 0)), hex(hwnd),
                "" if int(fg0 or 0) == hwnd else "  ⚠ 未抢到前台（继续，A/B 兜底）"))
        except Exception as e:
            log("  前台化 EXC %r" % (e,))

        shotp = str(SHOT / ("cap_%s_%s.bmp" % (disp.replace(".", "_"), key)))
        try:
            cr = op.capture(0, 0, cw, ch, shotp)
            uniq = uniq_colors(shotp) if os.path.exists(shotp) else -1
            info = bmp_info(shotp) if os.path.exists(shotp) else None
            log("  capture ret=%s %s uniq=%s" % (cr, ("%dx%d %dbpp %dB" % info[:4]) if info else "无文件", uniq))
            shots[tag] = (shotp, uniq)
        except Exception as e:
            rec(tag + " capture", "EXC %r" % (e,), "FAIL")
            uniq = -1
            shots[tag] = (None, -1)

        fr = None
        try:
            fr = op.get_screen_frame_info()
            log("  frame_info=%s" % (fr,))
        except Exception as e:
            log("  frame_info EXC %r" % (e,))

        ok_cap = uniq > 5000
        # normal 是桌面级捕获：被别的窗口遮挡时截到遮挡者属**环境条件**，不判 FAIL，
        # 由下面的「前台化 A/B」承担判定（同一二进制、唯一变量=遮挡）。
        st = "PASS" if ok_cap else ("INFO" if (not must_frame or disp == "normal") else "FAIL")
        rec("%s 截图" % tag, "uniq=%s %s" % (uniq, "真画面" if ok_cap else "低色数(桌面被遮挡)" if disp == "normal" else "低色数(疑黑/空白)"), st)

        # normal 是**桌面级**捕获（DXGI/WGC 抓屏幕合成）：游戏被别的窗口遮挡时会截到遮挡者。
        # 单变量 A/B：同一通道、同一二进制，只改「游戏是否前台」。
        if disp == "normal":
            u32 = ctypes.windll.user32
            u32.GetForegroundWindow.restype = ctypes.c_void_p
            u32.GetForegroundWindow.argtypes = []
            port = str(SHOT / "cap_normal_foreground.bmp")
            try:
                op.set_window_state(hwnd, 1)  # 1 = 激活
                time.sleep(0.7)
                fg = int(u32.GetForegroundWindow() or 0)
                op.capture(0, 0, cw, ch, port)
                u2 = uniq_colors(port) if os.path.exists(port) else -1
                log("  前台化后: fg=%s 期望=%s uniq=%s (遮挡时 uniq=%s)"
                    % (hex(fg), hex(hwnd), u2, uniq))
                rec("normal 前台化复验（A/B 单变量=遮挡）",
                    "前台 uniq=%s vs 遮挡 uniq=%s -> %s" % (u2, uniq, "真实画面" if u2 > 5000 else "仍非真画面"),
                    "PASS" if u2 > 5000 else "FAIL")
                if os.path.exists(port):
                    shots["normal/foreground"] = (port, u2)
            except Exception as e:
                rec("normal 前台化复验", "EXC %r" % (e,), "FAIL")

        try:
            r2 = op.bind_window(hwnd, disp, mouse, key, 0)
            log("  rebind ret=%s is_bind=%s" % (r2, op.is_bind()))
        except Exception as e:
            log("  rebind EXC %r" % (e,))
        try:
            ur = op.unbind_window()
            ib3 = op.is_bind()
            log("  unbind ret=%s is_bind_after=%s" % (ur, ib3))
            rec("%s unbind 干净" % tag, "unbind=%s is_bind=%s" % (ur, ib3),
                "PASS" if (ur and not ib3) else "FAIL")
        except Exception as e:
            rec("%s unbind" % tag, "EXC %r" % (e,), "FAIL")
        time.sleep(0.3)
    return shots


# ---------------------------------------------------------------- G2
def g2(op, pid, hwnd, cw, ch):
    sec("G2 解绑干净度（模块表 + 多轮 bind/unbind + 解绑后调用行为）")
    if not proc_alive(pid):
        rec("目标进程存活", "pid=%d 已退出" % pid, "FAIL")
        return

    before = modules_of(pid)
    rec("模块表快照（before）", "%s 个模块" % (len(before) if before else "?"),
        "PASS" if before else "FAIL")

    ROUNDS = 5
    ok_rounds = 0
    sizes = []
    for i in range(1, ROUNDS + 1):
        try:
            r = op.bind_window(hwnd, "dx", "dx", "dx", 0)
            ib = op.is_bind()
            cap = op.capture(0, 0, cw, ch, str(SHOT / ("g2_r%d.bmp" % i)))
            ur = op.unbind_window()
            ib2 = op.is_bind()
            if r and ib and ur and not ib2:
                ok_rounds += 1
            m = modules_of(pid)
            sizes.append(len(m) if m else -1)
            log("  轮%d: bind=%s is_bind=%s capture=%s unbind=%s is_bind_after=%s 模块数=%s 存活=%s"
                % (i, r, ib, cap, ur, ib2, sizes[-1], proc_alive(pid)))
        except Exception as e:
            rec("轮%d" % i, "EXC %r" % (e,), "FAIL")
            break
        if not proc_alive(pid):
            rec("轮%d 后进程存活" % i, "目标进程已死", "FAIL")
            break
        time.sleep(0.3)

    rec("5 轮 bind/unbind 全部干净", "%d/%d" % (ok_rounds, ROUNDS),
        "PASS" if ok_rounds == ROUNDS else "FAIL")
    rec("目标进程全程存活", "pid=%d yes" % pid, "PASS" if proc_alive(pid) else "FAIL")

    after = modules_of(pid)
    if before and after:
        added = sorted(set(after) - set(before))
        removed = sorted(set(before) - set(after))
        rec("模块表差集", "+%s  -%s" % (added or "无", removed or "无"), "INFO")
        # 关键：注入侧 op dll 不应残留；按需加载的图形/输入库**故意不 FreeLibrary**，残留属设计
        residue = [m for m in added if "op_c_api" in m or m.startswith("op_")]
        rec("注入 dll 无残留", "残留=%s" % (residue or "无"), "PASS" if not residue else "FAIL")
        for k in ("dinput8.dll", "d3d9.dll", "op_x86.dll", "op_c_api_x86.dll"):
            if k in after and k not in before:
                rec("  按需加载痕迹 %s" % k, "由无到有（符合设计，永不 FreeLibrary）", "INFO")
        rec("模块数不随轮次增长", "各轮=%s" % sizes,
            "PASS" if len(set(sizes)) == 1 else "INFO")

    # 解绑后调用行为：应明确失败而非崩溃
    log("")
    try:
        c = op.get_color(10, 10)
        rec("解绑后 get_color", "返回 %r（未绑定应失败）" % (c,), "INFO")
    except Exception as e:
        rec("解绑后 get_color", "抛异常（可接受，未崩溃）: %r" % (e,), "PASS")
    try:
        c = op.capture(0, 0, cw, ch, str(SHOT / "g2_after_unbind.bmp"))
        rec("解绑后 capture", "ret=%s（未绑定应失败）" % c, "INFO")
    except Exception as e:
        rec("解绑后 capture", "抛异常（可接受，未崩溃）: %r" % (e,), "PASS")
    rec("解绑后进程仍存活", "yes" if proc_alive(pid) else "no", "PASS" if proc_alive(pid) else "FAIL")


# ---------------------------------------------------------------- G3
def g3(op, hwnd, cw, ch):
    sec("G3 图色 / OCR / 帧信息（阶段 5 未覆盖项）")
    r = op.bind_window(hwnd, "gdi", "windows", "windows", 0)
    rec("bind_window(gdi/windows/windows)", "ret=%s" % r, "PASS" if r else "FAIL")
    if not r:
        return

    # ---- 尺寸/深度/FPS ----
    T("get_client_size", lambda: op.get_client_size(hwnd),
      ok=lambda v: tuple(v) == (cw, ch), show=lambda v: str(v))
    T("get_screen_width/height", lambda: (op.get_screen_width(), op.get_screen_height()),
      ok=lambda v: v[0] > 0 and v[1] > 0, show=lambda v: str(v))
    T("get_screen_depth", lambda: op.get_screen_depth(), ok=lambda v: v > 0, show=lambda v: str(v))
    T("get_fps", lambda: op.get_fps(), ok=lambda v: v >= 0, show=lambda v: str(v), soft=True)

    # ---- 帧信息：按**捕获次数**推进（不是按时间）----
    f0 = op.get_screen_frame_info()
    op.capture(0, 0, 40, 40, str(SHOT / "g3_f0.bmp"))
    f1 = op.get_screen_frame_info()
    op.capture(0, 0, 40, 40, str(SHOT / "g3_f1.bmp"))
    f2 = op.get_screen_frame_info()
    rec("frame_info 随捕获推进", "%s -> %s -> %s" % (f0, f1, f2),
        "PASS" if f2[0] > f0[0] or f2[1] > f0[1] else "INFO")

    # ---- 取色 / 比对 / 查找闭环 ----
    full = str(SHOT / "g3_full.bmp")
    T("capture 全窗落盘", lambda: op.capture(0, 0, cw, ch, full),
      ok=lambda v: v and os.path.exists(full))

    probes = [(20, 20), (cw // 2, ch // 2), (cw - 20, ch - 20), (cw // 2, 40)]
    col = None
    for (px, py) in probes:
        c = T("get_color(%d,%d)" % (px, py), lambda: op.get_color(px, py),
              ok=lambda v: isinstance(v, str) and len(v) == 6,
              show=lambda v: repr(v))
        if c and isinstance(c, str) and len(c) == 6 and col is None:
            col = (px, py, c)
    if col:
        px, py, c = col
        T("cmp_color 同点同色", lambda: op.cmp_color(px, py, c, 1.0),
          ok=lambda v: v is True, show=lambda v: str(v))
        r2 = T("find_color(0,0,w,h,%s)" % c,
               lambda: op.find_color(0, 0, cw, ch, c, 0.95),
               ok=lambda v: v[0] >= 0, show=lambda v: "x=%s y=%s" % (v[0], v[1]) if v else v)
        if r2 and r2[0] >= 0:
            hx, hy = r2[1], r2[2]
            T("  命中点复核（按同 sim 比对，非精确相等）", lambda: op.cmp_color(hx, hy, c, 0.95),
              ok=lambda v: v is True, show=lambda v: str(v))
        T("get_color_num(%s)" % c, lambda: op.get_color_num(0, 0, cw, ch, c, 0.95),
          ok=lambda v: v > 0, show=lambda v: "%d 个像素" % v)
        T("find_color_ex", lambda: op.find_color_ex(0, 0, cw, ch, c, 0.95),
          ok=lambda v: isinstance(v, str), show=lambda v: repr(v)[:70])

    # ---- 找图闭环（真机裁块当模板）----
    tx, ty, tw, th = cw // 3, ch // 2, 90, 40
    tpl = str(SHOT / "g3_tpl.bmp")
    T("裁模板 (%d,%d,%d,%d)" % (tx, ty, tw, th),
      lambda: op.capture(tx, ty, tx + tw, ty + th, tpl),
      ok=lambda v: v and os.path.exists(tpl))
    if os.path.exists(tpl):
        r3 = T("find_pic 原地命中", lambda: op.find_pic(tx - 40, ty - 20, tx + tw + 40, ty + th + 20,
                                                        tpl, "000000", 0.9),
               ok=lambda v: v[0] >= 0, show=lambda v: "x=%s y=%s" % (v[1], v[2]) if v else v)
        if r3 and r3[0] >= 0:
            # 注意**不在游戏上断言坐标精确性**：游戏画面持续动画（frame_info 逐捕获推进），
            # 模板裁剪时刻与 find_pic 时刻画面已变；叠加中部 B0FFFF 浅色区纹理重复 → 多解漂移。
            # 坐标精确性只在 **G5 静态靶子**上验证（那里画面不变，零漂移才算数）。
            rec("  命中坐标≈模板原点（动态画面，仅供参考）",
                "(%d,%d) vs (%d,%d)" % (r3[1], r3[2], tx, ty), "INFO")

    # ---- OCR：阶段5 只测了 ocr_auto，这里补 line / ex / 文件路径一致性 ----
    areas = [("顶栏角色", (1196, 0, 1356, 19)),
             ("底部技能栏", (400, 700, 900, 760)),
             ("左上", (8, 8, 260, 30))]
    hits = 0
    for nm, (x1, y1, x2, y2) in areas:
        p = str(SHOT / ("g3_ocr_%s.bmp" % nm))
        try:
            op.capture(x1, y1, x2, y2, p)
        except Exception:
            pass
        txt = T("ocr_auto %s" % nm, lambda: op.ocr_auto(x1, y1, x2, y2, 0.8),
                ok=lambda v: isinstance(v, str) and len(v) > 0, show=lambda v: repr(v)[:50], soft=True)
        if txt:
            hits += 1
        # autoocr_line / autoocr_ex 带**颜色过滤**参数（000000-303030 = 近黑）。
        # 游戏文字是金/白描边，近黑像素≈0 → 空串可能是**输入口径**问题而非 OCR 路径不通。
        # 判据 = **非空**（有区分度；恒真的 isinstance 判据无意义）；空 → INFO（soft），
        # OCR 路径本身的正确性由 G5 受控目标（白底黑字）覆盖。
        # 单变量 A/B：同一区域，只把过滤色放宽成全色域。
        line_txt = T("autoocr_line %s（近黑过滤）" % nm,
                     lambda: op.autoocr_line(x1, y1, x2, y2, "000000-303030", 0.7),
                     ok=lambda v: isinstance(v, str) and len(v) > 0, show=lambda v: repr(v)[:50], soft=True)
        if not line_txt:
            wide = T("  ↳ 放宽色域全色重试 %s" % nm,
                     lambda: op.autoocr_line(x1, y1, x2, y2, "000000-FFFFFF", 0.7),
                     ok=lambda v: isinstance(v, str) and len(v) > 0, show=lambda v: repr(v)[:60], soft=True)
            if wide:
                rec("  autoocr_line 空串归因", "近黑过滤滤掉了金/白文字 → **输入口径**，非 OCR 缺陷", "INFO")
        if os.path.exists(p):
            T("autoocr_ex %s（结构化）" % nm, lambda: op.autoocr_ex(x1, y1, x2, y2, "000000-303030", 0.7),
              ok=lambda v: isinstance(v, tuple) and len(v) == 2 and v[1] >= 1,
              show=lambda v: "count=%s items=%r" % (v[1], v[0][:40]), soft=True)
            T("ocr_from_file 对照 %s" % nm, lambda: op.ocr_from_file(p, "000000-303030", 0.7),
              ok=lambda v: isinstance(v, str) and len(v) > 0, show=lambda v: repr(v)[:50], soft=True)
    rec("OCR 至少一处有文本", "%d/%d" % (hits, len(areas)), "PASS" if hits >= 1 else "INFO")

    T("unbind_window", lambda: op.unbind_window(), ok=lambda v: v is True)


# ---------------------------------------------------------------- G5/G6
def g5_g6(op):
    """受控目标（自建白底黑字 Tk 窗口）—— 原清单 A/B 组（doc2/smoke_real.py 已丢失）的替代实现。

    为什么必须用受控目标：autoocr_line 走「颜色二值化 → 水平投影切行 → 逐行 rec」的免字库快路径，
    游戏 UI 是描边金字、二值化不出干净字形，用它判定「autoocr_line 是否可用」是无效判据。
    """
    sec("G5/G6 受控目标（自建 Tk 白底黑字）：OCR 四路一致性 + 耗时 + resize 放大超容截图")
    try:
        import tkinter as tk
    except Exception as e:
        rec("tkinter 可用", "不可用（需用 D:\\Program Files\\Python312\\python.exe 跑本组）: %r" % (e,), "SKIP")
        return

    TEXT = "许愿树[320,-778]12345"
    root = tk.Tk()
    root.title("OP_REAL_PROBE_TARGET")
    root.configure(bg="white")
    lbl = tk.Label(root, text=TEXT, bg="white", fg="black", font=("SimHei", 24))
    lbl.pack(padx=12, pady=12, expand=True)
    root.update_idletasks()
    # **关键**：让窗口自适应内容。固定 320x200 会把长文本居中裁掉两头
    # （实测 OCR 只拿到 '树[320,-778]12' —— 那是目标被裁，不是 OCR 缺陷）。
    root.geometry("")
    root.update()
    time.sleep(0.4)
    root.update()

    hwnd = op.find_window("", "OP_REAL_PROBE_TARGET")
    if not hwnd:
        hwnd = ctypes.windll.user32.FindWindowW(None, "OP_REAL_PROBE_TARGET")
    rec("自建目标窗口（op.find_window）", "hwnd=%s" % hex(hwnd or 0), "PASS" if hwnd else "FAIL")
    if not hwnd:
        root.destroy()
        return

    r = op.bind_window(hwnd, "gdi", "windows", "windows", 0)
    rec("bind_window(gdi)`", "ret=%s" % r, "PASS" if r else "FAIL")
    if not r:
        root.destroy()
        return

    cw, ch = op.get_client_size(hwnd)
    reqw = lbl.winfo_reqwidth()
    log("  客户区 = %dx%d  标签需要宽 %d（须 <= 客户区，否则文字被裁）" % (cw, ch, reqw))
    rec("目标未被裁剪（标签需宽 <= 客户区）", "%d <= %d" % (reqw, cw), "PASS" if reqw <= cw else "FAIL")
    base = str(SHOT / "g5_target_base.bmp")
    op.capture(0, 0, cw, ch, base)
    rec("基线 capture", "ret 有文件=%s %s" % (os.path.exists(base),
        ("%dx%d" % bmp_info(base)[:2]) if os.path.exists(base) else ""), "PASS" if os.path.exists(base) else "FAIL")
    if os.path.exists(base):
        w, h = bmp_info(base)[:2]
        rec("  基线截图尺寸 == 客户区", "%dx%d vs %dx%d" % (w, h, cw, ch),
            "PASS" if (w, h) == (cw, ch) else "FAIL")

    # ---- find_pic 原地命中：**静态靶子才是这条判据的正确位置** ----
    # 游戏画面持续动画，模板与命中时刻画面已变 → 坐标必漂（G3 已改为 INFO）。
    # 本窗口画面静止，故「命中 == 模板原点」是零漂移的硬判据。
    # 模板刻意裁在**客户区中心**（而非搜索窗左上角）：否则只返回搜索区左上角的
    # 伪实现也能"蒙对"，判据无区分度。
    tw, th = 120, 40
    tx = (cw - tw) // 2
    ty = (ch - th) // 2
    # 该区域必须真含字形：纯白模板会匹配到任意白处 → 假漂移。
    n_black = op.get_color_num(tx, ty, tx + tw, ty + th, "000000", 0.9)
    rec("  模板区域含黑字（非纯白，否则无判别性）", "(%d,%d,%d,%d) 内 %d 个黑像素" % (tx, ty, tw, th, n_black),
        "PASS" if n_black > 0 else "FAIL")
    if n_black > 0:
        tpl = str(SHOT / "g5_tpl.bmp")
        op.capture(tx, ty, tx + tw, ty + th, tpl)
        if os.path.exists(tpl):
            r = T("find_pic 原地命中（静态靶子）",
                  lambda: op.find_pic(0, 0, cw, ch, tpl, "000000", 0.9),
                  ok=lambda v: v[0] >= 0, show=lambda v: "x=%s y=%s" % (v[1], v[2]))
            if r and r[0] >= 0:
                rec("  命中坐标 == 模板原点（静态图零漂移）",
                    "(%d,%d) vs (%d,%d)" % (r[1], r[2], tx, ty),
                    "PASS" if abs(r[1] - tx) <= 1 and abs(r[2] - ty) <= 1 else "FAIL")
    else:
        rec("find_pic 原地命中（静态靶子）", "模板区无黑字，无法构成有效判据", "SKIP")

    def has_all(s):
        return isinstance(s, str) and all(k in s for k in ("许愿树", "320", "778", "12345"))

    log("  期望文本: %r" % TEXT)
    T("ocr_auto（AI 引擎）", lambda: op.ocr_auto(0, 0, cw, ch, 0.7),
      ok=has_all, show=lambda v: repr(v)[:70])
    T("autoocr_line（免字库快路径，黑字过滤）",
      lambda: op.autoocr_line(0, 0, cw, ch, "000000-303030", 0.7),
      ok=has_all, show=lambda v: repr(v)[:70])
    T("autoocr_ex（结构化）", lambda: op.autoocr_ex(0, 0, cw, ch, "000000-303030", 0.7),
      ok=lambda v: isinstance(v, tuple) and v[1] >= 1 and has_all(v[0]),
      show=lambda v: "count=%s items=%r" % (v[1], v[0][:60]))
    if os.path.exists(base):
        T("ocr_from_file（同图不同入口，应一致）",
          lambda: op.ocr_from_file(base, "000000-303030", 0.7),
          ok=has_all, show=lambda v: repr(v)[:70])

    # 反向验证（强区分）：传图上**不存在**的颜色（洋红 FF00FF）→ 二值图无前景 → 必返回空串。
    # 这能区分「颜色过滤真的生效」与「被绕过」；比 @ffffff 反白更强（实测反白后结果不变，无区分度）。
    inv = T("  ↳ 反向验证：不存在的颜色 FF00FF 应返回空（区分性）",
            lambda: op.autoocr_line(0, 0, cw, ch, "FF00FF", 0.7),
            ok=lambda v: isinstance(v, str) and v == "",
            show=lambda v: repr(v)[:70])
    # 旁证：@ffffff 反白在本图（白底黑字）上结果与正向相同 —— 仅记录该现象，不作判据（无区分度）。
    T("  ↳ 旁证：@ffffff 反白（白底图上与正向同）",
      lambda: op.autoocr_line(0, 0, cw, ch, "@ffffff", 0.7),
      ok=lambda v: isinstance(v, str), show=lambda v: repr(v)[:70], soft=True)

    # 耗时：单行快路径 avg
    n = 20
    t0 = time.perf_counter()
    for _ in range(n):
        op.autoocr_line(0, 0, cw, ch, "000000-303030", 0.7)
    avg = (time.perf_counter() - t0) / n * 1000
    rec("autoocr_line 耗时 avg（%d 次）" % n, "%.1f ms/帧" % avg,
        "PASS" if avg < 30 else "INFO")

    # ---- G6 resize 放大超容（原 A 组：放大后整窗 capture 不得越界/错位）----
    log("")
    log("[G6] resize 放大到 920x720（面积约 10 倍）后整窗 capture")
    root.geometry("920x720")
    root.update()
    root.update_idletasks()
    time.sleep(0.5)
    root.update()
    cw2, ch2 = op.get_client_size(hwnd)
    log("  放大后客户区 = %dx%d" % (cw2, ch2))
    big = str(SHOT / "g6_target_big.bmp")
    try:
        okc = op.capture(0, 0, cw2, ch2, big)
        info = bmp_info(big) if os.path.exists(big) else None
        log("  capture ret=%s %s" % (okc, ("%dx%d %dbpp" % info[:3]) if info else "无文件"))
        rec("放大后整窗 capture 成功", "ret=%s size=%s" % (okc, ("%dx%d" % info[:2]) if info else "?"),
            "PASS" if (okc and info and info[0] == cw2 and info[1] == ch2) else "FAIL")
    except Exception as e:
        rec("放大后整窗 capture", "EXC %r" % (e,), "FAIL")
    T("放大后中心取色（白底应 ffffff）", lambda: op.get_color(cw2 // 2, 40),
      ok=lambda v: v.upper() == "FFFFFF", show=lambda v: repr(v), soft=True)
    if os.path.exists(big):
        u = uniq_colors(big)
        rec("  放大后唯一色数", "%d（白底黑字应远低于整帧）" % u, "INFO")

    op.unbind_window()
    root.destroy()
    time.sleep(0.3)


def g4(op, hwnd, cw, ch):
    sec("G4 键鼠真机（有副作用：会真的向游戏发送输入）")
    r = op.bind_window(hwnd, "dx", "dx", "dx", 0)
    rec("bind_window(dx/dx/dx)", "ret=%s" % r, "PASS" if r else "FAIL")
    if not r:
        return
    # 仅做「移动 + 不动任何 UI」的最小集：鼠标移到窗口左上角内侧的空白区，不点击
    T("move_to(2,2)", lambda: op.move_to(2, 2), ok=lambda v: bool(v))
    time.sleep(0.3)
    cp = T("get_cursor_pos（绑定态返屏幕坐标）", lambda: op.get_cursor_pos(),
           ok=lambda v: isinstance(v, tuple), show=lambda v: str(v))
    log("  提示：绑定态 get_cursor_pos 返**屏幕**坐标，不等于客户区坐标，勿用它判落点")
    T("get_key_state(VK_SHIFT)", lambda: op.get_key_state(0x10), ok=lambda v: v in (0, 1),
      show=lambda v: str(v))
    T("unbind_window", lambda: op.unbind_window(), ok=lambda v: v is True)


# ---------------------------------------------------------------- main
def main():
    global _logf
    ap = argparse.ArgumentParser()
    ap.add_argument("--groups", default="G1,G2,G3,G5,G6")
    ap.add_argument("--input", action="store_true", help="追加 G4 键鼠（有副作用）")
    ap.add_argument("--pid", type=int, default=0)
    a = ap.parse_args()

    OUT_DIR.mkdir(parents=True, exist_ok=True)
    SHOT.mkdir(parents=True, exist_ok=True)
    _logf = open(OUT_DIR / f"_t_real_shumen_{TS}.txt", "w", encoding="utf-8", buffering=1)

    groups = {g.strip().upper() for g in a.groups.split(",") if g.strip()}
    if a.input:
        groups.add("G4")

    mode = dpi_aware()

    sec("G0 定位与环境")
    log("DPI 感知档位: %s" % mode)
    pid = a.pid or (find_pid() or [0])[0]
    rec("定位 client.exe", "pid=%d" % pid, "PASS" if pid else "FAIL")
    if not pid:
        log("未找到 client.exe —— 请确认游戏已启动")
        return
    wins = enum_windows(pid=pid)
    log("  pid 可见窗口: %s" % [(hex(h), t[:30], c) for h, t, c in wins])
    hwnd = next((h for h, t, c in wins if "蜀门" in t), None)
    if hwnd is None and wins:
        hwnd = wins[0][0]
        log("  (标题无「蜀门」，退回第一个可见窗口)")
    rec("定位游戏窗口", "hwnd=%s" % hex(hwnd or 0), "PASS" if hwnd else "FAIL")
    if not hwnd:
        return

    u = ctypes.windll.user32
    log("  GetDpiForWindow=%s" % (u.GetDpiForWindow(ctypes.c_void_p(hwnd)) if hasattr(u, "GetDpiForWindow") else "n/a"))
    wr = wt.RECT()
    u.GetWindowRect(ctypes.c_void_p(hwnd), ctypes.byref(wr))
    cr = wt.RECT()
    u.GetClientRect(ctypes.c_void_p(hwnd), ctypes.byref(cr))
    log("  window rect=%dx%d  client rect=%dx%d" % (
        wr.right - wr.left, wr.bottom - wr.top, cr.right - cr.left, cr.bottom - cr.top))

    op = Op(dll_dir=str(DLL_DIR), raise_on_error=False)
    op.set_show_error_msg(2)
    log("  op.dll = %s" % op.dll_path)
    log("  op 版本 = %s" % v_of(op))
    log("  进程已提权 = %s | op 报已提权 = %s" % (bool(ctypes.windll.shell32.IsUserAnAdmin()), op.is_elevated()))

    cw, ch = op.get_client_size(hwnd)
    rec("客户区尺寸（DPI-aware 口径）", "%dx%d" % (cw, ch),
        "PASS" if (cw, ch) == (1360, 768) else "INFO")

    if "G1" in groups:
        g1(op, hwnd, cw, ch)
    if "G2" in groups:
        g2(op, pid, hwnd, cw, ch)
    if "G3" in groups:
        g3(op, hwnd, cw, ch)
    if "G5" in groups or "G6" in groups:
        g5_g6(op)
    if "G4" in groups:
        g4(op, hwnd, cw, ch)
    else:
        log("\n[G4 键鼠] 未启用（会真的向游戏发送输入）。需要时加 --input。")

    try:
        if op.is_bind():
            op.unbind_window()
    except Exception:
        pass
    op.close()

    sec("汇总")
    n = {k: sum(1 for r in RES if r[3] == k) for k in ("PASS", "FAIL", "INFO", "SKIP")}
    log("PASS=%d FAIL=%d INFO=%d  截图=%s" % (n["PASS"], n["FAIL"], n["INFO"], SHOT))
    for g, api, detail, st in RES:
        if st == "FAIL":
            log("  FAIL [%s] %s -> %s" % (g, api, detail))
    log("日志: %s" % (OUT_DIR / f"_t_real_shumen_{TS}.txt"))
    # Tk + op 的 OCR 单例线程在解释器收尾时可能挂住 → 直接落盘退出
    sys.stdout.flush()
    if _logf:
        _logf.flush()
    os._exit(0 if n["FAIL"] == 0 else 1)


if __name__ == "__main__":
    main()
