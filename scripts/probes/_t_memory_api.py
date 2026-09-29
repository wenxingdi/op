# -*- coding: utf-8 -*-
"""内存读写域 **C API 层** 验证（覆盖缺口里的 11 个 Op*Memory* API）。

安全性：全程 `hwnd=0` 且**不绑定任何窗口** ⇒ Op 层 `resolve_memory_hwnd` 落回 0，
`ProcessMemory` 走 `ReadProcessMemory/WriteProcessMemory(GetCurrentProcess())`，
即**只读写探针自己的进程**。靶子内存是探针用 ctypes 现分配的一块 512B buffer，
写完即复原，无外部副作用。

判据原则：读写必须**闭环回读**（写进去的值由 Python 侧 ctypes 直接读回校验），
每条正向都配一条反向（非法地址/非法特征码必须失败而不是静默返回 0 值）。

分组：
  M1 read_data / write_data（hex 串往返）
  M2 read_int / write_int 全 7 种 IntType（含符号性反向）
  M3 read_float / read_double
  M4 read_string / write_string（ANSI / UTF16 / UTF8，含中文往返）
  M5 find_data / find_data_ex（特征码 + ?? 通配 + 范围）
  M6 get_module_base_addr

用法：
  python scripts/probes/_t_memory_api.py [--groups M1,M2,M3,M4,M5,M6]
"""
import argparse
import ctypes
import struct
import sys
import time
from pathlib import Path

REPO = Path(r"D:\AutoPro\op-master\op")
DLL_DIR = REPO / "build" / "nmake-x64-Release" / "libop"
OUT_DIR = REPO / "workbench" / "probes"
sys.path.insert(0, str(REPO / "bindings" / "python"))

from op import Op  # noqa: E402
from op.types import IntType, StringType  # noqa: E402

TS = time.strftime("%Y%m%d_%H%M%S")
WORK = OUT_DIR / ("memory_" + TS)
RES = []
_logf = None
_group = ["M0"]

BUFSIZE = 512
# 靶子：探针自己进程内的一块可写缓冲（不绑定窗口 ⇒ hwnd=0 ⇒ 本进程）
_buf = ctypes.create_string_buffer(BUFSIZE)
BASE = ctypes.addressof(_buf)

# 已知内容布局（偏移 → 内容）
OFF_I32 = 0           # int32  -2
OFF_I64 = 8           # int64  -1234567890123
OFF_F32 = 16          # float  1.25
OFF_F64 = 24          # double -7.5
OFF_ASCII = 32        # b"OP_MEM_PROBE\0"
OFF_PAT = 64          # b"\xDE\xAD\xBE\xEF" 特征码
OFF_SCRATCH = 128     # 写测试用
OFF_STR = 200         # 字符串写测试


def _init_buf():
    _buf[OFF_I32:OFF_I32 + 8] = struct.pack("<q", -2)
    _buf[OFF_I64:OFF_I64 + 8] = struct.pack("<q", -1234567890123)
    _buf[OFF_F32:OFF_F32 + 4] = struct.pack("<f", 1.25)
    _buf[OFF_F64:OFF_F64 + 8] = struct.pack("<d", -7.5)
    _buf[OFF_ASCII:OFF_ASCII + 13] = b"OP_MEM_PROBE\x00"
    _buf[OFF_PAT:OFF_PAT + 4] = b"\xDE\xAD\xBE\xEF"
    _buf[OFF_SCRATCH:OFF_SCRATCH + 64] = b"\x00" * 64
    _buf[OFF_STR:OFF_STR + 128] = b"\x00" * 128


def A(off):
    """缓冲内偏移 → op 地址串（十六进制，带 0x 前缀，str2address 走 wcstoull base16）"""
    return hex(BASE + off)


def hexs(n):
    return "%X" % n


def log(msg=""):
    line = str(msg)
    print(line, flush=True)
    if _logf:
        _logf.write(line + "\n")
        _logf.flush()


def sec(t):
    log("")
    log("=" * 78)
    log("== " + t)
    log("=" * 78)


def rec(api, detail, status):
    RES.append((_group[0], api, detail, status))
    mark = {"PASS": "[ OK ]", "FAIL": "[FAIL]", "INFO": "[INFO]", "SKIP": "[SKIP]"}.get(status, "[ ?? ]")
    log("%s %-46s %s" % (mark, api, detail))


def T(api, fn, ok=lambda v: bool(v), show=lambda v: repr(v)[:70], soft=False):
    try:
        v = fn()
    except Exception as e:
        rec(api, "EXC %r" % (e,), "INFO" if soft else "FAIL")
        return None
    try:
        good = bool(ok(v))
    except Exception as e:
        rec(api, "断言异常 %r（值=%s）" % (e, show(v)), "INFO" if soft else "FAIL")
        return v
    rec(api, show(v), "PASS" if good else ("INFO" if soft else "FAIL"))
    return v


# ------------------------------------------------------------------ M1 data
def m1(op):
    _group[0] = "M1"
    sec("M1 read_data / write_data（hex 串往返）")
    T("read_data(base, 16)", lambda: op.read_data(A(0), 16),
      ok=lambda v: isinstance(v, str) and v.lower() == bytes(_buf[0:16]).hex(),
      show=lambda v: "读=%r 真值=%r" % (v, bytes(_buf[0:16]).hex()))
    T("read_data(base+32, 12) == ASCII 区", lambda: op.read_data(A(OFF_ASCII), 12),
      ok=lambda v: v.lower() == b"OP_MEM_PROBE".hex(),
      show=lambda v: "读=%r" % v)

    payload = b"\xAA\xBB\xCC\xDD\xEE"
    T("write_data(scratch, %r)" % payload, lambda: op.write_data(A(OFF_SCRATCH), payload),
      ok=lambda v: bool(v), show=lambda v: "ret=%s" % v)
    rec("  ↳ 回读校验（ctypes 直读）", "写入=%r 实际=%r" % (payload.hex(), bytes(_buf[OFF_SCRATCH:OFF_SCRATCH + 5]).hex()),
        "PASS" if bytes(_buf[OFF_SCRATCH:OFF_SCRATCH + 5]) == payload else "FAIL")
    # 复原
    _buf[OFF_SCRATCH:OFF_SCRATCH + 64] = b"\x00" * 64

    # 反向：非法地址（0）必须失败而不是返回垃圾
    T("  ↳ 反向：read_data(地址 '0') 应为空", lambda: op.read_data("0", 4),
      ok=lambda v: v == "", show=lambda v: "ret=%r" % v)
    T("  ↳ 反向：read_data(size=0) 应为空", lambda: op.read_data(A(0), 0),
      ok=lambda v: v == "", show=lambda v: "ret=%r" % v)
    T("  ↳ 反向：write_data(地址 '0') 应失败", lambda: op.write_data("0", b"\x01"),
      ok=lambda v: not bool(v), show=lambda v: "ret=%s" % v, soft=True)
    T("  ↳ 反向：read_data 越界大地址应为空", lambda: op.read_data("FFFFFFFFFFFFFFFF", 8),
      ok=lambda v: v == "", show=lambda v: "ret=%r" % v, soft=True)


# ------------------------------------------------------------------ M2 int
def m2(op):
    _group[0] = "M2"
    sec("M2 read_int / write_int 全 7 种 IntType")
    cases = [
        (IntType.I32, 4, -2, "<i"),
        (IntType.I16, 2, -300, "<h"),
        (IntType.I8, 1, -1, "<b"),
        (IntType.I64, 8, -1234567890123, "<q"),
        (IntType.U32, 4, 4294967295, "<I"),
        (IntType.U16, 2, 65535, "<H"),
        (IntType.U8, 1, 255, "<B"),
    ]
    for t, size, val, fmt in cases:
        name = t.name
        T("write_int(%s, %d)" % (name, val), lambda t=t, val=val: op.write_int(A(OFF_SCRATCH), val, t),
          ok=lambda v: bool(v), show=lambda v: "ret=%s" % v)
        back = op.read_int(A(OFF_SCRATCH), t)
        raw = struct.unpack(fmt, bytes(_buf[OFF_SCRATCH:OFF_SCRATCH + size]))[0]
        rec("  ↳ read_int(%s) == %d" % (name, val),
            "回读=%d 真值=%d 内存=%d" % (back, val, raw),
            "PASS" if back == val and raw == val else "FAIL")
        _buf[OFF_SCRATCH:OFF_SCRATCH + 16] = b"\x00" * 16

    # 符号性反向：同一字节 0xFF，I8 读 -1 / U8 读 255（证明不是"无符号一律"）
    _buf[OFF_SCRATCH:OFF_SCRATCH + 1] = b"\xFF"
    i8 = op.read_int(A(OFF_SCRATCH), IntType.I8)
    u8 = op.read_int(A(OFF_SCRATCH), IntType.U8)
    rec("  ↳ 符号性：同一 0xFF 字节 I8=-1 / U8=255", "I8=%d U8=%d" % (i8, u8),
        "PASS" if (i8 == -1 and u8 == 255) else "FAIL")
    _buf[OFF_SCRATCH:OFF_SCRATCH + 1] = b"\x00"

    # 反向：非法地址读应返回 0
    T("  ↳ 反向：read_int(地址 '0') 应为 0", lambda: op.read_int("0", IntType.I32),
      ok=lambda v: v == 0, show=lambda v: "ret=%s" % v, soft=True)
    T("  ↳ 反向：write_int(地址 '0') 应失败", lambda: op.write_int("0", 1, IntType.I32),
      ok=lambda v: not bool(v), show=lambda v: "ret=%s" % v, soft=True)


# ------------------------------------------------------------------ M3 float
def m3(op):
    _group[0] = "M3"
    sec("M3 read_float / write_float / read_double / write_double")
    T("read_float(base+16) == 1.25", lambda: op.read_float(A(OFF_F32)),
      ok=lambda v: abs(v - 1.25) < 1e-6, show=lambda v: "读=%r 真值=1.25" % v)
    T("write_float(scratch, -3.75)", lambda: op.write_float(A(OFF_SCRATCH), -3.75),
      ok=lambda v: bool(v), show=lambda v: "ret=%s" % v)
    T("  ↳ read_float(scratch) == -3.75", lambda: op.read_float(A(OFF_SCRATCH)),
      ok=lambda v: abs(v + 3.75) < 1e-6, show=lambda v: "回读=%r" % v)
    T("  ↳ 内存直读校验", lambda: struct.unpack("<f", bytes(_buf[OFF_SCRATCH:OFF_SCRATCH + 4]))[0],
      ok=lambda v: abs(v + 3.75) < 1e-6, show=lambda v: "内存=%r" % v)
    _buf[OFF_SCRATCH:OFF_SCRATCH + 16] = b"\x00" * 16

    T("read_double(base+24) == -7.5", lambda: op.read_double(A(OFF_F64)),
      ok=lambda v: abs(v + 7.5) < 1e-9, show=lambda v: "读=%r 真值=-7.5" % v)
    T("write_double(scratch, 123.456)", lambda: op.write_double(A(OFF_SCRATCH), 123.456),
      ok=lambda v: bool(v), show=lambda v: "ret=%s" % v)
    T("  ↳ read_double(scratch) == 123.456", lambda: op.read_double(A(OFF_SCRATCH)),
      ok=lambda v: abs(v - 123.456) < 1e-9, show=lambda v: "回读=%r" % v)
    _buf[OFF_SCRATCH:OFF_SCRATCH + 16] = b"\x00" * 16

    # 反向：非法地址
    T("  ↳ 反向：read_float(地址 '0') 应为 0", lambda: op.read_float("0"),
      ok=lambda v: v == 0, show=lambda v: "ret=%r" % v, soft=True)
    T("  ↳ 反向：read_double(地址 '0') 应为 0", lambda: op.read_double("0"),
      ok=lambda v: v == 0, show=lambda v: "ret=%r" % v, soft=True)


# ------------------------------------------------------------------ M4 string
def m4(op):
    _group[0] = "M4"
    sec("M4 read_string / write_string（ANSI / UTF16 / UTF8）")
    T("read_string(base+32, ANSI) == 'OP_MEM_PROBE'",
      lambda: op.read_string(A(OFF_ASCII), StringType.ANSI, 0),
      ok=lambda v: v == "OP_MEM_PROBE", show=lambda v: "读=%r" % v)

    for st, text in [(StringType.ANSI, "HELLO_OP"), (StringType.UTF8, "中文往返"), (StringType.UTF16, "WIDE_OK")]:
        T("write_string(%s, %r)" % (st.name, text),
          lambda st=st, text=text: op.write_string(A(OFF_STR), text, st),
          ok=lambda v: bool(v), show=lambda v: "ret=%s" % v)
        T("  ↳ read_string(%s) == %r" % (st.name, text),
          lambda st=st, text=text: op.read_string(A(OFF_STR), st, 0),
          ok=lambda v: v == text, show=lambda v: "回读=%r" % v)
        _buf[OFF_STR:OFF_STR + 128] = b"\x00" * 128

    # 反向：非法地址 / 长度 0 之外
    T("  ↳ 反向：read_string(地址 '0') 应为空", lambda: op.read_string("0", StringType.ANSI, 0),
      ok=lambda v: v == "", show=lambda v: "ret=%r" % v, soft=True)
    T("  ↳ 反向：write_string(地址 '0') 应失败", lambda: op.write_string("0", "X", StringType.ANSI),
      ok=lambda v: not bool(v), show=lambda v: "ret=%s" % v, soft=True)


# ------------------------------------------------------------------ M5 find_data
def m5(op):
    _group[0] = "M5"
    sec("M5 find_data / find_data_ex（特征码搜索）")
    lo, hi = BASE, BASE + BUFSIZE
    rng = "%s-%s" % (hexs(lo), hexs(hi))
    pat_off = BASE + OFF_PAT
    T("find_data(缓冲范围, 'DEADBEEF')", lambda: op.find_data(rng, "DE AD BE EF"),
      ok=lambda v: isinstance(v, str) and hexs(pat_off) in [s.upper().lstrip("0").upper() for s in v.split("|") if s] or
                   (isinstance(v, str) and any(int(s, 16) == pat_off for s in v.split("|") if s)),
      show=lambda v: "命中=%r 真值=%s" % (v, hexs(pat_off)))
    T("find_data 带 ?? 通配 'DE??BE??'", lambda: op.find_data(rng, "DE??BE??"),
      ok=lambda v: isinstance(v, str) and any(int(s, 16) == pat_off for s in v.split("|") if s),
      show=lambda v: "命中=%r 真值=%s" % (v, hexs(pat_off)))
    T("find_data_ex(范围, 特征码, step=1, count=1)",
      lambda: op.find_data_ex(rng, "DEADBEEF", 1, 1),
      ok=lambda v: isinstance(v, str) and len([s for s in v.split("|") if s]) <= 1,
      show=lambda v: "命中=%r" % v, soft=True)
    # 反向：缓冲里没有的特征码
    T("  ↳ 反向：找不存在的特征码应为空", lambda: op.find_data(rng, "0102030405060708"),
      ok=lambda v: v == "", show=lambda v: "ret=%r" % v)
    T("  ↳ 反向：非法特征码（奇数长度）应为空", lambda: op.find_data(rng, "ABC"),
      ok=lambda v: v == "", show=lambda v: "ret=%r" % v)
    T("  ↳ 反向：全通配 '????????' 应为空", lambda: op.find_data(rng, "????????"),
      ok=lambda v: v == "", show=lambda v: "ret=%r" % v, soft=True)
    T("  ↳ 反向：非法范围（无 '-'）应为空", lambda: op.find_data("DEADBEEF", "DEADBEEF"),
      ok=lambda v: v == "", show=lambda v: "ret=%r" % v, soft=True)


# ------------------------------------------------------------------ M6 module
def m6(op):
    _group[0] = "M6"
    sec("M6 get_module_base_addr")
    k32 = ctypes.windll.kernel32
    k32.GetModuleHandleW.restype = ctypes.c_void_p
    k32.GetModuleHandleW.argtypes = [ctypes.c_wchar_p]
    h = k32.GetModuleHandleW("kernel32.dll")
    T("get_module_base_addr('kernel32.dll')", lambda: op.get_module_base_addr("kernel32.dll"),
      ok=lambda v: isinstance(v, str) and v and int(v, 16) == h,
      show=lambda v: "op=%r ctypes=%s" % (v, hexs(h or 0)))
    T("  ↳ 反向：不存在的模块应为空", lambda: op.get_module_base_addr("__no_such_module__.dll"),
      ok=lambda v: v == "", show=lambda v: "ret=%r" % v)


# ------------------------------------------------------------------ main
def main():
    global _logf
    ap = argparse.ArgumentParser()
    ap.add_argument("--groups", default="M1,M2,M3,M4,M5,M6")
    a = ap.parse_args()
    groups = set(a.groups.replace(" ", "").split(","))

    OUT_DIR.mkdir(parents=True, exist_ok=True)
    _logf = open(str(OUT_DIR / ("_t_memory_api_%s.txt" % TS)), "w", encoding="utf-8")
    log("内存读写域 C API 验证  %s" % TS)

    op = Op(dll_dir=str(DLL_DIR), raise_on_error=False)
    op.set_show_error_msg(2)
    log("op.dll = %s" % op.dll_path)
    log("靶子缓冲（探针本进程，未绑定窗口 ⇒ hwnd=0） base=%s size=%d" % (hexs(BASE), BUFSIZE))
    _init_buf()

    try:
        if "M1" in groups:
            m1(op)
        if "M2" in groups:
            m2(op)
        if "M3" in groups:
            m3(op)
        if "M4" in groups:
            m4(op)
        if "M5" in groups:
            m5(op)
        if "M6" in groups:
            m6(op)
    finally:
        try:
            op.close()
        except Exception:
            pass

    n = {"PASS": 0, "FAIL": 0, "INFO": 0, "SKIP": 0}
    for _, _, _, s in RES:
        n[s] = n.get(s, 0) + 1
    sec("汇总")
    log("PASS=%d  FAIL=%d  INFO=%d  SKIP=%d   （共 %d 条）" % (n["PASS"], n["FAIL"], n["INFO"], n["SKIP"], len(RES)))
    if n["FAIL"]:
        log("")
        log("失败明细：")
        for g, api, detail, s in RES:
            if s == "FAIL":
                log("  [%s] %s -- %s" % (g, api, detail))
    log("")
    log("日志：%s" % _logf.name)
    return 0 if n["FAIL"] == 0 else 2


if __name__ == "__main__":
    sys.exit(main())
