# -*- coding: utf-8 -*-
"""蜀门(32位) dx 注入专项：dx/dx/dx 与 dx/windows/windows 对照 + capture 统计。
日志固定落 workbench\\__op.log（注入侧 walk 到目标进程 cwd，故一并 chdir 到同目录）。
"""
import ctypes, os, time
from ctypes import wintypes

DLL_DIR = r"D:\AutoPro\op-master\op\build\nmake-x64-Release\libop"
WORKDIR = r"D:\AutoPro\op-master\op\workbench"
os.chdir(WORKDIR)
os.environ["PATH"] = DLL_DIR + ";" + os.environ["PATH"]
os.add_dll_directory(DLL_DIR)

LOG = os.path.join(WORKDIR, "_t_dx32_shumen_out.txt")


def log(s):
    print(s, flush=True)
    with open(LOG, "a", encoding="utf-8") as f:
        f.write(s + "\n")


open(LOG, "w").close()

dll = ctypes.CDLL(os.path.join(DLL_DIR, "op_c_api_x64.dll"))
dll.OpCreate.restype = ctypes.c_void_p
dll.OpDestroy.argtypes = [ctypes.c_void_p]
dll.OpSetPath.argtypes = [ctypes.c_void_p, ctypes.c_wchar_p]
dll.OpSetShowErrorMsg.argtypes = [ctypes.c_void_p, ctypes.c_int]
dll.OpBindWindow.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_wchar_p,
                             ctypes.c_wchar_p, ctypes.c_wchar_p, ctypes.c_int]
dll.OpBindWindow.restype = ctypes.c_int
dll.OpUnBindWindow.argtypes = [ctypes.c_void_p]
dll.OpUnBindWindow.restype = ctypes.c_int
dll.OpIsBind.argtypes = [ctypes.c_void_p]
dll.OpIsBind.restype = ctypes.c_int
dll.OpGetBindWindow.argtypes = [ctypes.c_void_p]
dll.OpGetBindWindow.restype = ctypes.c_void_p
dll.OpGetScreenData.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int,
                                ctypes.c_int, ctypes.c_int, ctypes.POINTER(ctypes.c_int)]
dll.OpGetScreenData.restype = ctypes.c_void_p

user32 = ctypes.windll.user32
k32 = ctypes.windll.kernel32


class PE(ctypes.Structure):
    _fields_ = [("dwSize", wintypes.DWORD), ("cntUsage", wintypes.DWORD),
                ("th32ProcessID", wintypes.DWORD), ("th32DefaultHeapID", ctypes.c_void_p),
                ("th32ModuleID", wintypes.DWORD), ("cntThreads", wintypes.DWORD),
                ("th32ParentProcessID", wintypes.DWORD), ("pcPriClassBase", ctypes.c_long),
                ("dwFlags", wintypes.DWORD), ("szExeFile", ctypes.c_char * 260)]


snap = k32.CreateToolhelp32Snapshot(0x2, 0)
pe = PE()
pe.dwSize = ctypes.sizeof(pe)
pid = 0
ok = k32.Process32First(snap, ctypes.byref(pe))
while ok:
    if pe.szExeFile.lower() == b"client.exe":
        pid = pe.th32ProcessID
        break
    ok = k32.Process32Next(snap, ctypes.byref(pe))
k32.CloseHandle(snap)
log(f"[pid] client.exe={pid}")

found = []
CB = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)


def cb(h, lp):
    w = wintypes.DWORD(0)
    user32.GetWindowThreadProcessId(h, ctypes.byref(w))
    if w.value == pid:
        b = ctypes.create_unicode_buffer(256)
        user32.GetWindowTextW(h, b, 256)
        if b.value and user32.IsWindowVisible(h):
            found.append((h, b.value))
    return True


user32.EnumWindows(CB(cb), 0)
log(f"[wins] {[(hex(h), t) for h, t in found]}")
hwnd = next((h for h, t in found if "客户端" in t or "蜀门" in t), found[0][0] if found else 0)
log(f"[hwnd] {hex(hwnd)}")


def caps(op, tag):
    # 注意：OpGetScreenData 第 6 个出参是 ret（1=成功），不是 size！像素缓冲须按请求的
    # 宽高自行解读（w*h*4 字节，BGRA 自上而下）。
    ret = ctypes.c_int(0)
    ptr = dll.OpGetScreenData(op, 0, 0, 200, 150, ctypes.byref(ret))
    if not ptr or ret.value != 1:
        log(f"  [{tag}] capture FAIL ptr={ptr} ret={ret.value}")
        return
    n = 200 * 150 * 4
    data = bytes((ctypes.c_char * n).from_address(ptr))
    uniq = len({data[i:i + 4] for i in range(0, n, 4)})
    log(f"  [{tag}] capture OK ret=1 bytes={n} uniq_colors={uniq}")


def once(h, disp, m, k, tag):
    hh = dll.OpCreate()
    dll.OpSetPath(hh, DLL_DIR)
    dll.OpSetShowErrorMsg(hh, 2)
    r = dll.OpBindWindow(hh, ctypes.c_void_p(h), disp, m, k, 0)
    ib = dll.OpIsBind(hh)
    gw = dll.OpGetBindWindow(hh)
    log(f"[{tag}] bind ret={r} is_bind={ib} gw={hex(gw or 0)}")
    caps(hh, tag)
    time.sleep(0.8)
    caps(hh, tag)
    ur = dll.OpUnBindWindow(hh)
    log(f"[{tag}] unbind={ur} is_bind_after={dll.OpIsBind(hh)}")
    dll.OpDestroy(hh)
    return r


once(hwnd, "dx", "dx", "dx", "dx/dx/dx")
time.sleep(1)
once(hwnd, "dx", "windows", "windows", "dx/win/win")
log("done")
