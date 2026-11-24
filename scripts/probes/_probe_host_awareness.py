"""枚举可见顶层窗口的 DPI 感知级别，用于对比不同宿主的坐标口径差异。

用途：排查「op 绑定某宿主后 OCR/找图区域整体偏移」时，先确认**宿主自己**的
DPI_AWARENESS 与 GetDpiForWindow 值。不同宿主给出不同口径，是偏移的第一嫌疑。

跑法：python scripts/probes/_probe_host_awareness.py [关键字...]
输出：workbench/_awareness.txt（UTF-8，避免控制台 GBK 截断中文）
"""

import ctypes
import io
import os
import sys
from ctypes import wintypes as w

user32 = ctypes.WinDLL("user32", use_last_error=True)
kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
psapi = ctypes.WinDLL("psapi", use_last_error=True)

user32.GetWindowDpiAwarenessContext.restype = ctypes.c_void_p
user32.GetWindowDpiAwarenessContext.argtypes = [w.HWND]
user32.GetAwarenessFromDpiAwarenessContext.restype = ctypes.c_int
user32.GetAwarenessFromDpiAwarenessContext.argtypes = [ctypes.c_void_p]
user32.GetDpiForWindow.restype = ctypes.c_uint
user32.GetDpiForWindow.argtypes = [w.HWND]
user32.IsWindowVisible.argtypes = [w.HWND]
user32.IsWindowVisible.restype = w.BOOL

# 必须显式声明 argtypes：64 位下默认会把 HANDLE 当 c_int 截断，
# 导致 OpenProcess 的句柄被切掉高 32 位，QueryFullProcessImageNameW 失败。
# （QueryFullProcessImageNameW 在 kernel32，不在 psapi —— 这一条也踩过。）
kernel32.OpenProcess.argtypes = [w.DWORD, w.BOOL, w.DWORD]
kernel32.OpenProcess.restype = w.HANDLE
kernel32.CloseHandle.argtypes = [w.HANDLE]
kernel32.CloseHandle.restype = w.BOOL
kernel32.QueryFullProcessImageNameW.argtypes = [
    w.HANDLE,
    w.DWORD,
    w.LPWSTR,
    ctypes.POINTER(w.DWORD),
]
kernel32.QueryFullProcessImageNameW.restype = w.BOOL

AWARENESS_NAMES = {0: "UNAWARE", 1: "SYSTEM_AWARE", 2: "PER_MONITOR_AWARE"}


def process_name(pid: int) -> str:
    handle = kernel32.OpenProcess(0x1000, False, pid)  # PROCESS_QUERY_LIMITED_INFORMATION
    if not handle:
        return "?"
    buf = ctypes.create_unicode_buffer(1024)
    size = w.DWORD()
    kernel32.QueryFullProcessImageNameW(handle, 0, buf, ctypes.byref(size))
    kernel32.CloseHandle(handle)
    return os.path.basename(buf.value) or "?"


def main() -> int:
    keywords = [k.lower() for k in sys.argv[1:]]
    enum_proc = ctypes.WINFUNCTYPE(w.BOOL, w.HWND, w.LPARAM)
    rows: list[str] = []

    def on_window(hwnd, _lparam):
        try:
            # 不按 IsWindowVisible 过滤：远程会话 / 无交互桌面下大量窗口会返回 0，
            # 而排查 DPI 口径恰恰需要这些后台宿主的数值。改为"有标题"即收录。
            title = ctypes.create_unicode_buffer(512)
            user32.GetWindowTextW(hwnd, title, 512)
            if not title.value.strip():
                return True
            cls = ctypes.create_unicode_buffer(256)
            user32.GetClassNameW(hwnd, cls, 256)
            pid = w.DWORD()
            user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
            proc = process_name(pid.value)

            haystack = f"{title.value} {cls.value} {proc}".lower()
            if keywords and not any(k in haystack for k in keywords):
                return True

            ctx = user32.GetWindowDpiAwarenessContext(hwnd)
            aware = user32.GetAwarenessFromDpiAwarenessContext(ctx) if ctx else -1
            visible = bool(user32.IsWindowVisible(hwnd))
            rows.append(
                "%-40s | %-26s | %-20s | %-18s | %4d | %s"
                % (
                    title.value[:40],
                    cls.value[:26],
                    proc[:20],
                    AWARENESS_NAMES.get(aware, aware),
                    user32.GetDpiForWindow(hwnd),
                    "可见" if visible else "隐藏",
                )
            )
        except Exception:  # 单个窗口失败不影响整体枚举
            pass
        return True

    user32.EnumWindows(enum_proc(on_window), 0)

    out_path = os.path.join(
        os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))),
        "workbench",
        "_awareness.txt",
    )
    header = "%-40s | %-26s | %-20s | %-18s | %s" % ("TITLE", "CLASS", "PROCESS", "AWARENESS", "DPI")
    body = "\n".join([header, "-" * 130] + rows)
    with io.open(out_path, "w", encoding="utf-8") as fp:
        fp.write(body + "\n")
    print(body)
    print("\n[written] %s  (%d windows)" % (out_path, len(rows)))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
