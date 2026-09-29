"""前台模式截图探针：直接调 op_c_api_x64.dll 的 OpCapture（未绑定时自动绑桌面 = 前台模式）。

用法:
    python workbench/op_foreground_capture.py <out.bmp> [x1 y1 x2 y2]

仅用于素材采集，产物不入库（assets/ 在 .gitignore）。
"""

import ctypes
import os
import sys

DLL_DIR = os.path.abspath(
    os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "build", "nmake-x64-Release", "libop")
)
DLL_PATH = os.path.join(DLL_DIR, "op_c_api_x64.dll")


def load_op():
    if hasattr(os, "add_dll_directory"):
        os.add_dll_directory(DLL_DIR)
    dll = ctypes.CDLL(DLL_PATH)

    dll.OpCreate.restype = ctypes.c_void_p
    dll.OpCreate.argtypes = []

    dll.OpDestroy.restype = None
    dll.OpDestroy.argtypes = [ctypes.c_void_p]

    dll.OpSetPath.restype = ctypes.c_int
    dll.OpSetPath.argtypes = [ctypes.c_void_p, ctypes.c_wchar_p]

    dll.OpSetShowErrorMsg.restype = ctypes.c_int
    dll.OpSetShowErrorMsg.argtypes = [ctypes.c_void_p, ctypes.c_int]

    dll.OpSetScreenDataMode.restype = ctypes.c_int
    dll.OpSetScreenDataMode.argtypes = [ctypes.c_void_p, ctypes.c_int]

    dll.OpCapture.restype = ctypes.c_int
    dll.OpCapture.argtypes = [
        ctypes.c_void_p,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_wchar_p,
    ]

    dll.OpGetScreenData.restype = ctypes.c_int
    dll.OpGetScreenData.argtypes = [
        ctypes.c_void_p,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.c_int,
        ctypes.POINTER(ctypes.c_size_t),
    ]

    return dll


def main():
    out = os.path.abspath(sys.argv[1]) if len(sys.argv) > 1 else os.path.abspath("capture_foreground.bmp")
    if len(sys.argv) >= 6:
        x1, y1, x2, y2 = (int(v) for v in sys.argv[2:6])
    else:
        user32 = ctypes.windll.user32
        x1, y1 = 0, 0
        x2, y2 = user32.GetSystemMetrics(0), user32.GetSystemMetrics(1)

    dll = load_op()
    handle = dll.OpCreate()
    if not handle:
        raise SystemExit("OpCreate failed")

    print("handle =", hex(handle))
    print("set_path =", dll.OpSetPath(handle, os.path.dirname(os.path.abspath(__file__))))
    dll.OpSetShowErrorMsg(handle, 2)

    if not os.path.isdir(os.path.dirname(out)):
        os.makedirs(os.path.dirname(out), exist_ok=True)

    ret = dll.OpCapture(handle, x1, y1, x2, y2, out)
    print("capture(%d,%d,%d,%d) -> ret=%d file=%s exists=%s size=%s"
          % (x1, y1, x2, y2, ret, out, os.path.exists(out),
             os.path.getsize(out) if os.path.exists(out) else -1))

    # 顺带用 GetScreenData 交叉验证前台抓屏通道
    if os.path.exists(out):
        with open(out, "rb") as fh:
            head = fh.read(2)
        print("bmp_magic =", head)

    dll.OpDestroy(handle)


if __name__ == "__main__":
    main()
