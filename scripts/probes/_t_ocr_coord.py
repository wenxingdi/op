# -*- coding: utf-8 -*-
"""决定性实验：op 绑定态坐标口径 —— **客户区坐标** 还是 **屏幕绝对坐标**？

做法（双变量单次实验）：
  把受控 Tk 窗口在屏幕上整体平移 (+300,+200)，窗口内容完全不变，然后在**同一客户区区域**
  上调用同一个 API：
    - 返回坐标**不变**        → 坐标相对客户区左上角（客户区坐标）
    - 返回坐标**同步平移**    → 屏幕绝对坐标

旁证（代码层）：libop/binding/BindingSession.cpp:575-580
    long BindingSession::RectConvert(long &x1, ...) {
        /*if (_capture && (_display == NORMAL || _display == GDI)) {
            x1 += _capture->_client_x; y1 += _capture->_client_y;   // ← 客户区→屏幕 偏移
        }*/                                                          // ← 整段被注释掉
        x2 = std::min<long>(this->get_width(), x2);                  // 只做裁剪
        ...
  即：RectConvert 不加窗口原点偏移 → 预期为「客户区坐标」。
  本脚本给出实测反向验证（若实测同步平移，则代码注释掉的那段就是问题所在）。

用法：D:\\Program Files\\Python312\\python.exe scripts/probes/_t_ocr_coord.py
（tkinter 需系统 Python312；managed python 无 tk）
"""
import ctypes
import ctypes.wintypes as wt
import os
import sys
import time
from pathlib import Path

REPO = Path(r"D:\AutoPro\op-master\op")
DLL_DIR = REPO / "build" / "nmake-x64-Release" / "libop"
OUT = REPO / "workbench" / "probes"
sys.path.insert(0, str(REPO / "bindings" / "python"))

from op import Op  # noqa: E402


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


def client_origin(hwnd):
    """客户区原点在屏幕上的坐标（DPI-aware 口径）。"""
    u = ctypes.windll.user32
    u.ClientToScreen.argtypes = [ctypes.c_void_p, ctypes.POINTER(wt.POINT)]
    u.ClientToScreen.restype = ctypes.c_bool
    pt = wt.POINT(0, 0)
    u.ClientToScreen(ctypes.c_void_p(hwnd), ctypes.byref(pt))
    return (pt.x, pt.y)


def first_item_x(v):
    """从 autoocr_ex 的 'x1,y1,x2,y2,conf,text|...' 取首项 x1。"""
    s = v[0] if isinstance(v, tuple) else v
    if not s:
        return None
    try:
        return int(str(s).split("|")[0].split(",")[0])
    except Exception:
        return None


def pos_of(v):
    """find_color 返回 (ret, x, y) 之类 —— 统一取最后两个数作为坐标。"""
    if not v:
        return None
    if isinstance(v, (tuple, list)):
        nums = [n for n in v if isinstance(n, (int, float))]
        return (int(nums[-2]), int(nums[-1])) if len(nums) >= 2 else None
    return None


def main():
    # DPI 感知必须在任何窗口 API 之前
    mode = dpi_aware()
    import tkinter as tk

    root = tk.Tk()
    root.title("OP_COORD_PROBE")
    root.configure(bg="white")
    lbl = tk.Label(root, text="许愿树[320,-778]12345", bg="white", fg="black", font=("SimHei", 24))
    lbl.pack(padx=12, pady=12, expand=True)
    root.update_idletasks()
    root.geometry("")  # 自适应，避免长文本被居中裁剪
    root.update()
    time.sleep(0.4)
    root.update()

    op = Op(dll_dir=str(DLL_DIR), raise_on_error=False)
    op.set_show_error_msg(2)
    hwnd = op.find_window("", "OP_COORD_PROBE") or ctypes.windll.user32.FindWindowW(None, "OP_COORD_PROBE")
    if not hwnd:
        print("FAIL: 找不到受控窗口 OP_COORD_PROBE")
        os._exit(2)
    r = op.bind_window(hwnd, "gdi", "windows", "windows", 0)
    cw, ch = op.get_client_size(hwnd)
    print("DPI=%s  hwnd=%s  bind=%s  client=%dx%d" % (mode, hex(hwnd), r, cw, ch))
    if not r:
        os._exit(2)

    def probe(tag):
        o = client_origin(hwnd)
        ex = op.autoocr_ex(0, 0, cw, ch, "000000-303030", 0.7)
        fc = op.find_color(0, 0, cw, ch, "000000", 0.9)
        ex_x = first_item_x(ex)
        fc_p = pos_of(fc)
        print("[%s] client_origin_screen=%s  autoocr_ex=%r (x=%s) | find_color=%r (pos=%s)"
              % (tag, o, ex, ex_x, fc, fc_p))
        return {"origin": o, "ex_x": ex_x, "fc": fc_p}

    root.geometry("+1300+300"); root.update(); time.sleep(0.5); root.update()
    A = probe("A @1300,300")
    root.geometry("+1600+500"); root.update(); time.sleep(0.5); root.update()
    B = probe("B @1600,500")

    dox = B["origin"][0] - A["origin"][0]
    doy = B["origin"][1] - A["origin"][1]
    print("\n客户区屏幕原点位移 = (%+d, %+d)" % (dox, doy))

    verdict = "不确定"
    if A["ex_x"] is not None and B["ex_x"] is not None:
        dex = B["ex_x"] - A["ex_x"]
        print("autoocr_ex 首项 x 位移 = %+d   (A=%s B=%s)" % (dex, A["ex_x"], B["ex_x"]))
        if abs(dex) <= 2 and abs(dox) >= 100:
            verdict = "客户区坐标"
            print("==> 判定：**客户区坐标**（窗口平移 %d px，返回坐标纹丝不动）" % dox)
        elif abs(dex - dox) <= 2:
            verdict = "屏幕绝对坐标"
            print("==> 判定：**屏幕绝对坐标**（返回坐标随窗口同步平移 %d px）" % dox)
        else:
            print("==> 判定：不确定（位移既不≈0，也不≈窗口位移 %d）" % dox)
    if A["fc"] and B["fc"]:
        print("find_color 位移 = (%+d, %+d)   A=%s B=%s"
              % (B["fc"][0] - A["fc"][0], B["fc"][1] - A["fc"][1], A["fc"], B["fc"]))

    OUT.mkdir(parents=True, exist_ok=True)
    with open(OUT / "_t_ocr_coord_result.txt", "w", encoding="utf-8") as f:
        f.write("verdict(autoocr_ex) = %s\n" % verdict)
        f.write("window_origin_delta = (%+d, %+d)\n" % (dox, doy))
        f.write("autoocr_ex_x  A=%s B=%s\n" % (A["ex_x"], B["ex_x"]))
        f.write("find_color    A=%s B=%s\n" % (A["fc"], B["fc"]))

    # 不调 unbind/close/destroy：实测 Tk + op 的 OCR 单例线程在解释器收尾时挂住，
    # 结果文件已落盘但进程不退（表现为工具侧 SIGTERM）。gdi 通道无注入，进程退出即释放；
    # 结果已落盘 → 直接硬退出。
    sys.stdout.flush()
    os._exit(0)


if __name__ == "__main__":
    main()
