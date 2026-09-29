# -*- coding: utf-8 -*-
"""Dump threads of a hung process via MiniDumpWriteDump, then resolve each
thread's RIP + stack-top return addresses to module+offset using the dump's
module list (no symbol server needed)."""
import ctypes
import os
import struct
import sys
from ctypes import wintypes

dbghelp = ctypes.WinDLL("dbghelp")
k32 = ctypes.WinDLL("kernel32")

PROCESS_ALL = 0x1F0FFF
MiniDumpNormal = 0x00000000

# MINIDUMP_TYPE as int
dbghelp.MiniDumpWriteDump.argtypes = [wintypes.HANDLE, wintypes.DWORD, wintypes.HANDLE,
                                      ctypes.c_int, ctypes.c_void_p, ctypes.c_void_p,
                                      ctypes.c_void_p]
dbghelp.MiniDumpWriteDump.restype = wintypes.BOOL


def write_dump(pid, path):
    h = k32.OpenProcess(PROCESS_ALL, False, pid)
    if not h:
        print(f"OpenProcess({pid}) failed err={k32.GetLastError()}")
        return False
    try:
        import msvcrt
        fd = os.open(path, os.O_CREAT | os.O_WRONLY | os.O_TRUNC | os.O_BINARY)
        fh = msvcrt.get_osfhandle(fd)
        ok = dbghelp.MiniDumpWriteDump(h, pid, fh, MiniDumpNormal, None, None, None)
        os.close(fd)
        print(f"MiniDumpWriteDump ok={ok} err={k32.GetLastError()} size={os.path.getsize(path)}")
        return bool(ok)
    finally:
        k32.CloseHandle(h)


def parse_dump(path):
    """Extract module list and per-thread RIP / stack top pointers."""
    data = open(path, "rb").read()
    sig, ver, nstreams, dir_rva, checksum, ts, flags = struct.unpack_from("<IIIIIIQ", data, 0)
    assert sig == 0x504D444D, hex(sig)  # 'MDMP'

    streams = {}
    for i in range(nstreams):
        st, sz, rva = struct.unpack_from("<III", data, dir_rva + i * 12)
        streams.setdefault(st, []).append((sz, rva))

    # ModuleListStream = 4
    modules = []
    if 4 in streams:
        sz, rva = streams[4][0]
        n = struct.unpack_from("<I", data, rva)[0]
        # MINIDUMP_MODULE size = 108
        for i in range(n):
            base = rva + 4 + i * 108
            mbase, msize, cks = struct.unpack_from("<QII", data, base)
            name_rva = struct.unpack_from("<I", data, base + 20)[0]
            ln = struct.unpack_from("<I", data, name_rva)[0]
            name = data[name_rva + 4:name_rva + 4 + ln].decode("utf-16-le", "replace")
            modules.append((mbase, msize, os.path.basename(name)))

    def which(addr):
        for mbase, msize, name in modules:
            if mbase <= addr < mbase + msize:
                return f"{name}+0x{addr - mbase:x}"
        return hex(addr)

    # ExceptionStream = 6 gives one thread's exception context; ThreadListStream = 3
    out = []
    if 3 in streams:
        sz, rva = streams[3][0]
        n = struct.unpack_from("<I", data, rva)[0]
        # MINIDUMP_THREAD size = 48
        for i in range(n):
            base = rva + 4 + i * 48
            tid, susp, pri = struct.unpack_from("<III", data, base)
            teb = struct.unpack_from("<Q", data, base + 16)[0]
            stack_start = struct.unpack_from("<Q", data, base + 24)[0]
            stack_size = struct.unpack_from("<I", data, base + 32)[0]
            stack_rva = struct.unpack_from("<I", data, base + 36)[0]
            ctx_rva = struct.unpack_from("<I", data, base + 44)[0]
            # CONTEXT (x64): Rip at offset 0xF8
            rip = struct.unpack_from("<Q", data, ctx_rva + 0xF8)[0]
            rsp = struct.unpack_from("<Q", data, ctx_rva + 0x98)[0]
            # walk 64 qwords on stack as potential return addresses
            rets = []
            soff = rsp - stack_start if stack_start <= rsp < stack_start + stack_size else 0
            for j in range(64):
                p = stack_rva + soff + j * 8
                if p + 8 <= len(data):
                    v = struct.unpack_from("<Q", data, p)[0]
                    m = which(v)
                    if not m.startswith("0x"):
                        rets.append(f"{m}@{j}")
            out.append((tid, which(rip), rets[:20]))
    return out


if __name__ == "__main__":
    pid = int(sys.argv[1])
    dump = sys.argv[2]
    write_dump(pid, dump)  # best-effort; parse proceeds on existing file
    for tid, rip, rets in parse_dump(dump):
        print(f"TID {tid}: RIP={rip}")
        for r in rets:
            print(f"    ret-> {r}")
