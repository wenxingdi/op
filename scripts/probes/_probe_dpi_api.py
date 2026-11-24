# -*- coding: utf-8 -*-
"""查 DPI 感知 API 可用性与 op_test.exe 的 manifest（批 62 诊断）。"""
import ctypes
import ctypes.wintypes as wt
import re

u32 = ctypes.WinDLL("user32", use_last_error=True)
k32 = ctypes.WinDLL("kernel32", use_last_error=True)

print("=== 1) 导出存在性 ===")
for name in ("SetProcessDpiAwarenessContext", "SetProcessDpiAwarenessContext2",
             "SetThreadDpiAwarenessContext", "GetProcessDpiAwarenessContext",
             "GetWindowDpiAwarenessContext", "AreDpiAwarenessContextsEqual"):
    print("  user32.%-34s %s" % (name, "有" if hasattr(u32, name) else "无"))

try:
    shcore = ctypes.WinDLL("shcore")
    print("  shcore.SetProcessDpiAwarenessContext %s" %
          ("有" if hasattr(shcore, "SetProcessDpiAwarenessContext") else "无"))
except OSError as e:
    print("  shcore 加载失败:", e)

print()
print("=== 2) 各 API 实际行为（当前进程） ===")
u32.SetProcessDpiAwarenessContext.argtypes = [ctypes.c_void_p]
u32.SetProcessDpiAwarenessContext.restype = ctypes.c_bool
for value, label in ((-1, "UNAWARE"), (-2, "SYSTEM_AWARE"), (-3, "PER_MONITOR_AWARE"),
                     (-4, "PER_MONITOR_AWARE_V2"), (-5, "UNAWARE_GDISCALED")):
    ctypes.set_last_error(0)
    ok = u32.SetProcessDpiAwarenessContext(ctypes.c_void_p(value))
    print("  user32 SetProcessDpiAwarenessContext(%2d %-22s) -> %s err=%d" %
          (value, label, bool(ok), ctypes.get_last_error()))

if hasattr(u32, "SetProcessDpiAwarenessContext2"):
    u32.SetProcessDpiAwarenessContext2.argtypes = [ctypes.c_void_p]
    u32.SetProcessDpiAwarenessContext2.restype = ctypes.c_bool
    for value, label in ((-1, "UNAWARE"), (-2, "SYSTEM_AWARE"), (-3, "PER_MONITOR_AWARE"),
                         (-4, "PER_MONITOR_AWARE_V2")):
        ctypes.set_last_error(0)
        ok = u32.SetProcessDpiAwarenessContext2(ctypes.c_void_p(value))
        print("  user32 SetProcessDpiAwarenessContext2(%2d %-22s) -> %s err=%d" %
              (value, label, bool(ok), ctypes.get_last_error()))

print()
print("=== 3) op_test.exe 内嵌 manifest 是否声明 DPI 感知 ===")
path = r"D:\AutoPro\op-master\op\build\nmake-x64-Release\tests\op_test.exe"
data = open(path, "rb").read()
for kw in (b"dpiAware", b"dpiAwareness", b"PerMonitor", b"PerMonitorV2"):
    idx = data.find(kw)
    print("  %-14s %s" % (kw.decode(), ("命中 @%d" % idx) if idx >= 0 else "未出现"))
m = re.search(rb"<assembly.*?</assembly>", data, re.S)
if m:
    blob = m.group(0)
    for tag in (b"dpiAware", b"dpiAwareness", b"windowsSettings"):
        i = blob.find(tag)
        if i >= 0:
            print("  manifest:", blob[max(0, i - 120):i + 160].decode("utf-8", "replace").replace("\n", " "))
            break
    else:
        print("  manifest 里没有 DPI 感知声明")
else:
    print("  没找到 <assembly> 块")
