# -*- coding: utf-8 -*-
"""P1 exit-hang bisection probe.

Runs a configurable stage of op API calls via COM late binding (comtypes
dynamic dispatch, same oleaut path as PowerShell), leaves a marker file
tracing progress, then EXITS NATURALLY. The parent decides if exit hung.

Usage: python exit_hang_probe.py <stage>
Stages: none | gdi | gdi_ocr | dx2 | dx2_ocr | ocr_only
"""
import ctypes
import os
import subprocess
import sys
import time

MARK = r"D:\AutoPro\op-master\op\workbench\exit_hang_marker.txt"
IMG = r"D:\AutoPro\op-master\op\workbench\exit_hang_cap.bmp"


def mark(s):
    with open(MARK, "a", encoding="utf-8") as f:
        f.write(f"{time.strftime('%H:%M:%S')} [{os.getpid()}] {s}\n")


def start_notepad():
    pid_holder = []

    subprocess.Popen(["notepad.exe"])
    time.sleep(1.5)
    u32 = ctypes.windll.user32
    hwnd = None

    def cb(h, lp):
        nonlocal hwnd
        if not u32.IsWindowVisible(h):
            return True
        c = ctypes.create_unicode_buffer(256)
        u32.GetClassNameW(h, c, 256)
        t = ctypes.create_unicode_buffer(256)
        u32.GetWindowTextW(h, t, 256)
        if c.value == "Notepad" and t.value and hwnd is None:
            hwnd = h
            pid = wintypes.DWORD()
            u32.GetWindowThreadProcessId(h, ctypes.byref(pid))
            pid_holder.append(pid.value)
        return True

    CB = ctypes.WINFUNCTYPE(ctypes.c_int, ctypes.c_void_p, ctypes.c_void_p)
    u32.EnumWindows(CB(cb), 0)
    return hwnd, (pid_holder[0] if pid_holder else None)


def kill_pid(pid):
    if not pid:
        return
    k32 = ctypes.windll.kernel32
    h = k32.OpenProcess(0x0001, False, pid)  # PROCESS_TERMINATE
    if h:
        k32.TerminateProcess(h, 0)
        k32.CloseHandle(h)


def main():
    stage = sys.argv[1] if len(sys.argv) > 1 else "none"
    mark(f"STAGE={stage} BEGIN")

    import comtypes
    import comtypes.client

    op = comtypes.client.CreateObject("op.opsoft", dynamic=True)
    mark("S0_CREATED ver=" + str(op.Ver()))

    hwnd, npid = start_notepad()
    mark(f"S0B_NOTEPAD hwnd={hwnd} pid={npid}")
    if not hwnd and stage != "none":
        mark("ABORT no notepad")
        return

    try:
        if stage.startswith("gdi"):
            r = op.BindWindow(int(hwnd), "gdi", "normal", "normal", 0)
            mark(f"S1_BIND_GDI ret={r}")
            if r == 1:
                r2 = op.Capture(0, 0, 400, 300, IMG)
                mark(f"S2_CAPTURE ret={r2} exists={os.path.exists(IMG)}")
        elif stage.startswith("dx2"):
            r = op.BindWindow(int(hwnd), "dx2", "normal", "normal", 0)
            mark(f"S1_BIND_DX2 ret={r}")
            if r == 1:
                r2 = op.Capture(0, 0, 400, 300, IMG)
                mark(f"S2_CAPTURE ret={r2} exists={os.path.exists(IMG)}")

        if stage.endswith("_ocr"):
            txt = op.OcrAuto(0, 0, 400, 300, 0.8)
            mark(f"S3_OCR len={len(str(txt))}")
        elif stage == "ocr_only":
            op.BindWindow(int(hwnd), "gdi", "normal", "normal", 0)
            mark("S1_BIND_GDI ret=1(ocr_only)")
            txt = op.OcrAuto(0, 0, 400, 300, 0.8)
            mark(f"S3_OCR len={len(str(txt))}")

        if stage != "none" and hwnd:
            r = op.UnBindWindow()
            mark(f"S4_UNBIND ret={r}")
        kill_pid(npid)
    except Exception as e:
        mark(f"EXC {e!r}")
        kill_pid(npid)
    finally:
        op = None
        mark("S5_RELEASED")
    mark("PRE_EXIT")
    # natural exit; interpreter shutdown + DLL detach happens here


if __name__ == "__main__":
    main()
