import ctypes
from ctypes import wintypes

u32 = ctypes.windll.user32
dwm = ctypes.windll.dwmapi

h = wintypes.HWND(115543794)

class RECT(ctypes.Structure):
    _fields_ = [("L", ctypes.c_long), ("T", ctypes.c_long),
                ("R", ctypes.c_long), ("B", ctypes.c_long)]

r = RECT()
ok = u32.GetWindowRect(h, ctypes.byref(r))
print(f"GetWindowRect ok={ok} rect=({r.L},{r.T})-({r.R},{r.B})")
print(f"IsWindowVisible={bool(u32.IsWindowVisible(h))}")

buf = ctypes.create_unicode_buffer(256)
u32.GetClassNameW(h, buf, 256)
print(f"ClassName='{buf.value}'")
u32.GetWindowTextW(h, buf, 512)
print(f"Title='{buf.value}'")

cloaked = ctypes.c_int(0)
dwm.DwmGetWindowAttribute(h, 14, ctypes.byref(cloaked), 4)
print(f"DWM_CLOAKED={cloaked.value}")

# client rect
class POINT(ctypes.Structure):
    _fields_ = [("x", ctypes.c_long), ("y", ctypes.c_long)]
pt = POINT()
u32.ClientToScreen(h, ctypes.byref(pt))
crc = RECT()
u32.GetClientRect(h, ctypes.byref(crc))
print(f"GetClientRect ok={bool(u32.GetClientRect(h, ctypes.byref(crc)))} client={crc.R-crc.L}x{crc.B-crc.T} origin=({pt.x},{pt.y})")

# pid / thread
pid = wintypes.DWORD(0)
tid = u32.GetWindowThreadProcessId(h, ctypes.byref(pid))
print(f"pid={pid.value} tid={tid}")
