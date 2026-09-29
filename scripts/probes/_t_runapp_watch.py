# -*- coding: utf-8 -*-
"""run_app 启动蜀门：lnk 与 exe 对比测试，监控进程存活与窗口标题。"""
import ctypes
import ctypes.wintypes as wt
import os
import sys
import time

DLL_DIR = r"D:\AutoPro\op-master\op\build\nmake-x64-Release\libop"
os.environ["PATH"] = DLL_DIR + ";" + os.environ["PATH"]
os.add_dll_directory(DLL_DIR)

dll = ctypes.CDLL(os.path.join(DLL_DIR, "op_c_api_x64.dll"))
dll.OpCreate.restype = ctypes.c_void_p
dll.OpDestroy.argtypes = [ctypes.c_void_p]
dll.OpRunApp.argtypes = [ctypes.c_void_p, ctypes.c_wchar_p, ctypes.c_int,
                         ctypes.POINTER(ctypes.c_uint32)]
dll.OpRunApp.restype = ctypes.c_int

user32 = ctypes.windll.user32
kernel32 = ctypes.windll.kernel32

h = dll.OpCreate()

target = sys.argv[1]  # lnk 或 exe
mode = int(sys.argv[2]) if len(sys.argv) > 2 else 0
path = {
    "lnk": r"D:\Program Files (x86)\shumen\game.lnk",
    "exe": r"D:\Program Files (x86)\shumen\game.exe",
}[target]

pid = ctypes.c_uint32(0)
ret = dll.OpRunApp(h, path, mode, ctypes.byref(pid))
print(f"[{target} mode={mode}] ret={ret} pid={pid.value}")
game_pid = pid.value

# 轮询 40 秒：进程存活 + 该 pid 的顶层窗口标题
EnumWindows = user32.EnumWindows
GetWindowTextW = user32.GetWindowTextW
GetWindowThreadProcessId = user32.GetWindowThreadProcessId
IsWindowVisible = user32.IsWindowVisible


def windows_of(pid_target):
    result = []
    WNDENUMPROC = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)

    def cb(hwnd, lparam):
        wpid = wt.DWORD(0)
        GetWindowThreadProcessId(hwnd, ctypes.byref(wpid))
        if wpid.value == pid_target:
            buf = ctypes.create_unicode_buffer(256)
            GetWindowTextW(hwnd, buf, 256)
            result.append((hwnd, buf.value, bool(IsWindowVisible(hwnd))))
        return True

    EnumWindows(WNDENUMPROC(cb), 0)
    return result


start = time.time()
last_alive = None
while time.time() - start < 40:
    alive = kernel32.OpenProcess(0x1000, False, game_pid)  # PROCESS_QUERY_LIMITED_INFORMATION
    if alive:
        kernel32.CloseHandle(alive)
        state = "alive"
    else:
        state = "dead"
    if state != last_alive:
        print(f"  t={time.time()-start:4.1f}s process {state}")
        last_alive = state
    if state == "dead":
        break
    wins = windows_of(game_pid)
    vis = [(hex(h2), t2) for h2, t2, v in wins if v and t2]
    if vis:
        print(f"  t={time.time()-start:4.1f}s visible windows: {vis}")
        # 若是「错误」对话框，读子控件文本
        for h2, t2, v in wins:
            if v and t2 == "错误":
                texts = []
                CB = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)

                def cb(child, lparam):
                    cls = ctypes.create_unicode_buffer(64)
                    user32.GetClassNameW(child, cls, 64)
                    buf = ctypes.create_unicode_buffer(512)
                    user32.GetWindowTextW(child, buf, 512)
                    texts.append((cls.value, buf.value))
                    return True

                user32.EnumChildWindows(h2, CB(cb), 0)
                for cls, txt in texts:
                    print(f"    [{cls}] {txt}")
        break
    time.sleep(2)

# 结束时再报一次完整状态
alive = kernel32.OpenProcess(0x1000, False, game_pid)
if alive:
    kernel32.CloseHandle(alive)
    print(f"FINAL: pid={game_pid} alive, windows={windows_of(game_pid)}")
else:
    print(f"FINAL: pid={game_pid} dead")
dll.OpDestroy(h)
