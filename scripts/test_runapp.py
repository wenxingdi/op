# -*- coding: utf-8 -*-
"""RunApp 真机测试: 启动蜀门 game.exe"""
import ctypes, os, sys, time

DLL_DIR = r"D:\AutoPro\op-master\op\build\nmake-x64-Release\libop"
os.add_dll_directory(DLL_DIR)
os.environ["PATH"] = DLL_DIR + os.pathsep + os.environ["PATH"]

dll = ctypes.CDLL(os.path.join(DLL_DIR, "op_c_api_x64.dll"))
dll.OpCreate.restype = ctypes.c_void_p
dll.OpRunApp.argtypes = [ctypes.c_void_p, ctypes.c_wchar_p, ctypes.c_int, ctypes.POINTER(ctypes.c_uint32)]
dll.OpRunApp.restype = ctypes.c_int

op = dll.OpCreate()
print(f"handle = {op}")

exe = r"D:\Program Files (x86)\shumen\game.exe"
print(f"exists = {os.path.isfile(exe)}")

pid = ctypes.c_uint32(0)
# 常规控制台/桌面模式 mode=0
t0 = time.time()
ret = dll.OpRunApp(op, exe, 0, ctypes.byref(pid))
print(f"ret = {ret}  pid = {pid.value}  ({(time.time()-t0)*1000:.0f} ms)")

if ret != 1:
    print("RunApp 未成功，退出")
    sys.exit(1)

# 验证进程存活
time.sleep(3)
import subprocess
out = subprocess.run(
    ["tasklist", "/FI", f"PID eq {pid.value}", "/FO", "CSV"],
    capture_output=True, text=True, encoding="gbk")
print("tasklist:", out.stdout.strip().splitlines()[-1])

# 枚举新出现的顶层窗口
user32 = ctypes.windll.user32
FOUND = []
def cb(hwnd, _):
    if not user32.IsWindowVisible(hwnd): return True
    pidwin = ctypes.c_ulong()
    user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pidwin))
    if pidwin.value == pid.value:
        n = user32.GetWindowTextLengthW(hwnd)
        buf = ctypes.create_unicode_buffer(n + 1)
        user32.GetWindowTextW(hwnd, buf, n + 1)
        cls = ctypes.create_unicode_buffer(256)
        user32.GetClassNameW(hwnd, cls, 256)
        FOUND.append((hwnd, buf.value, cls.value))
    return True
CB = ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)(cb)
user32.EnumWindows(CB, None)
print(f"顶层窗口 {len(FOUND)} 个:")
for hwnd, title, cls in FOUND:
    print(f"  hwnd={hwnd:#x}  title={title!r}  class={cls!r}")
