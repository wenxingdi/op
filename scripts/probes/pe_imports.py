# -*- coding: utf-8 -*-
"""pe_imports.py — 极简 PE 导入表解析（普通导入 + 延迟导入），用于核对发布件依赖是否齐全。

用法: python pe_imports.py <dll_or_exe> [...]
"""
import struct, sys, os


def _rva_to_off(secs, rva):
    for name, va, vsz, praw, rsz in secs:
        if va <= rva < va + max(vsz, rsz):
            return praw + (rva - va)
    return None


def parse(path):
    d = open(path, "rb").read()
    if d[:2] != b"MZ":
        return None
    e_lfanew = struct.unpack_from("<I", d, 0x3C)[0]
    assert d[e_lfanew:e_lfanew + 4] == b"PE\0\0", "not PE"
    coff = e_lfanew + 4
    nsec, = struct.unpack_from("<H", d, coff + 2)
    optsz, = struct.unpack_from("<H", d, coff + 16)
    opt = coff + 20
    magic, = struct.unpack_from("<H", d, opt)
    pe32p = (magic == 0x20B)
    ddir = opt + (112 if pe32p else 96)
    secs_off = opt + optsz
    secs = []
    for i in range(nsec):
        b = secs_off + i * 40
        name = d[b:b + 8].rstrip(b"\0").decode("ascii", "replace")
        vsz, va, rsz, praw = struct.unpack_from("<IIII", d, b + 8)
        secs.append((name, va, vsz, praw, rsz))

    def dir_names(idx, is_delay=False):
        rva, sz = struct.unpack_from("<II", d, ddir + idx * 8)
        if not rva:
            return []
        off = _rva_to_off(secs, rva)
        if off is None:
            return []
        out = []
        step = 8 if is_delay else 20
        entsz = 32 if is_delay else 20
        i = 0
        while True:
            e = off + i * (entsz if is_delay else 20)
            if is_delay:
                # DELAY_IMPORT_DESCRIPTOR: Attributes(0) DllNameRVA(4) ...
                attrs, name_rva = struct.unpack_from("<II", d, e)
            else:
                # IMAGE_IMPORT_DESCRIPTOR: OriginalFirstThunk(0) TimeDateStamp(4)
                #   ForwarderChain(8) Name(12) FirstThunk(16)
                name_rva, = struct.unpack_from("<I", d, e + 12)
            if name_rva == 0:
                break
            no = _rva_to_off(secs, name_rva)
            if no is None:
                break
            end = d.index(b"\0", no)
            out.append(d[no:end].decode("ascii", "replace"))
            i += 1
        return out

    return {
        "file": os.path.basename(path),
        "imports": dir_names(1),
        "delay": dir_names(13, True),
    }


if __name__ == "__main__":
    for p in sys.argv[1:]:
        r = parse(p)
        if r is None:
            print(p, "NOT PE")
            continue
        print("=== %s" % r["file"])
        print("  导入:      %s" % (", ".join(sorted(r["imports"])) or "-"))
        print("  延迟导入:  %s" % (", ".join(sorted(r["delay"])) or "-"))
