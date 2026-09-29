# -*- coding: utf-8 -*-
"""Clean re-run of the raw IDispatch::Invoke experiment after the [in,out] IDL fix.

Replicates the exact oleaut path PowerShell late-binding uses.
Pass criteria: T1 byref_variant args read back inner.vt=VT_I4 with real values.
"""
import ctypes
from ctypes import wintypes

VT_BYREF = 0x4000
VT_VARIANT = 0x000C
VT_I4 = 0x0003
VT_I8 = 0x0014
VT_EMPTY = 0x0000
DISPATCH_METHOD = 1
DISPATCH_PROPERTYGET = 2
VARTYPE = ctypes.c_ushort

class VARIANT(ctypes.Structure):
    # REAL x64 VARIANT = 24 bytes (vt + 3 reserved WORDs + 16-byte union).
    # A 16-byte struct makes DISPPARAMS arrays misaligned under oleaut's
    # 24-byte stride -> scrambled args (this poisoned the previous experiment).
    _fields_ = [("vt", VARTYPE), ("r1", wintypes.WORD), ("r2", wintypes.WORD),
                ("r3", wintypes.WORD), ("_", ctypes.c_void_p), ("__", ctypes.c_void_p)]

class DISPPARAMS(ctypes.Structure):
    _fields_ = [("rgvarg", ctypes.POINTER(VARIANT)),
                ("rgdispidNamedArgs", ctypes.POINTER(ctypes.c_long)),
                ("cArgs", wintypes.UINT), ("cNamedArgs", wintypes.UINT)]

class EXCEPINFO(ctypes.Structure):
    _fields_ = [(n, wintypes.WORD) for n in ("wCode", "wReserved")] + \
               [(n, ctypes.c_void_p) for n in ("bstrSource", "bstrDescription", "bstrHelpFile")] + \
               [(n, wintypes.DWORD) for n in ("dwReserved1",)] + \
               [("pvReserved", ctypes.c_void_p), ("pfnDeferredFillIn", ctypes.c_void_p),
                ("scode", ctypes.c_long)]

oleaut32 = ctypes.oledll.oleaut32
u32 = ctypes.windll.user32

def lval_of(v):
    return wintypes.LONG.from_address(ctypes.addressof(v) + 8).value

# ---- target window (notepad, class "Notepad") ----
notepad = None
def _enum_cb(hwnd, lparam):
    global notepad
    if not u32.IsWindowVisible(hwnd):
        return True
    cbuf = ctypes.create_unicode_buffer(256)
    u32.GetClassNameW(hwnd, cbuf, 256)
    tbuf = ctypes.create_unicode_buffer(256)
    u32.GetWindowTextW(hwnd, tbuf, 256)
    if cbuf.value == "Notepad" and tbuf.value and notepad is None:
        notepad = hwnd
    return True
ENUMPROC = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
u32.EnumWindows(ENUMPROC(_enum_cb), 0)
hwnd = notepad
print(f"target hwnd={hwnd}")
if hwnd:
    r = wintypes.RECT()
    u32.GetClientRect(hwnd, ctypes.byref(r))
    print(f"[native] GetClientRect = {r.right}x{r.bottom} (150% DPI => OP should report {int(r.right*1.5)}x{int(r.bottom*1.5)})")

# ---- IDispatch ----
import comtypes, comtypes.client, comtypes.automation as auto
op = comtypes.client.CreateObject("op.opsoft", interface=auto.IDispatch)
disp = op
obj_addr = ctypes.addressof(disp.contents) if hasattr(disp, "contents") else ctypes.cast(disp, ctypes.c_void_p).value
vtbl_addr = ctypes.c_void_p.from_address(obj_addr).value
entries = ctypes.cast(vtbl_addr, ctypes.POINTER(ctypes.c_void_p * 7)).contents
INVOKE = ctypes.WINFUNCTYPE(ctypes.c_long, ctypes.c_void_p, ctypes.c_long, ctypes.c_void_p,
                            wintypes.DWORD, wintypes.DWORD, ctypes.POINTER(DISPPARAMS),
                            ctypes.POINTER(VARIANT), ctypes.POINTER(EXCEPINFO),
                            ctypes.POINTER(wintypes.UINT))
fnInvoke = ctypes.cast(entries[6], INVOKE)
GETIDSOFNAMES = ctypes.WINFUNCTYPE(ctypes.c_long, ctypes.c_void_p, ctypes.c_void_p,
                                   ctypes.POINTER(ctypes.c_wchar_p), wintypes.UINT, wintypes.LCID,
                                   ctypes.POINTER(ctypes.c_long))
fnGetIds = ctypes.cast(entries[5], GETIDSOFNAMES)
riid_null = ctypes.c_void_p(0)

def get_dispid(name):
    dispid = ctypes.c_long(-1)
    names = (ctypes.c_wchar_p * 1)(name)
    hr = fnGetIds(disp, riid_null, names, 1, 0, ctypes.byref(dispid))
    return dispid.value

def call_invoke(dispid, args_desc):
    """args_desc: natural param order. kinds: i8(value) / byref_variant / byref_i4(init) / plain_variant"""
    variants, holders = [], []
    for kind in args_desc:
        v = VARIANT()
        if kind == "byref_variant":
            inner = VARIANT(); inner.vt = VT_EMPTY; inner._ = None
            v.vt = VT_BYREF | VT_VARIANT
            v._ = ctypes.cast(ctypes.pointer(inner), ctypes.c_void_p)
            holders.append(inner)
        elif kind == "byref_i4":
            v.vt = VT_BYREF | VT_I4
            holder = wintypes.LONG(-1)
            v._ = ctypes.cast(ctypes.pointer(holder), ctypes.c_void_p)
            holders.append(holder)
        elif kind == "plain_variant":
            v.vt = VT_EMPTY; v._ = None
            holders.append(v)
        else:  # ("i8", value)
            v.vt = VT_I8
            v._ = ctypes.c_void_p(kind[1] & 0xFFFFFFFFFFFFFFFF)
            holders.append(None)
        variants.append(v)
    rev = variants[::-1]
    arr = (VARIANT * len(rev))(*rev)
    dp = DISPPARAMS()
    dp.rgvarg = ctypes.cast(arr, ctypes.POINTER(VARIANT))
    dp.cArgs = len(rev); dp.cNamedArgs = 0
    result = VARIANT(); ei = EXCEPINFO(); argerr = wintypes.UINT(0)
    hr = fnInvoke(disp, dispid, riid_null, 0, DISPATCH_METHOD | DISPATCH_PROPERTYGET,
                  ctypes.byref(dp), ctypes.byref(result), ctypes.byref(ei), ctypes.byref(argerr))
    res = lval_of(result) if hr == 0 and result.vt == VT_I4 else None
    return hr, holders, res

# ---- T0: GetCursorPos (no window dependency), dispid resolves by name ----
d_gcp = get_dispid("GetCursorPos")
print(f"\nT0 GetCursorPos (dispid={d_gcp}, std byref_variant x2):")
hr, hs, res = call_invoke(d_gcp, ["byref_variant", "byref_variant"])
print(f"  HRESULT=0x{hr & 0xFFFFFFFF:08X} nret={res}")
for i, h in enumerate(hs):
    if h is not None:
        print(f"  arg{i}: inner.vt={h.vt:#06x} val={lval_of(h)}")

# ---- T1: GetClientSize on notepad ----
if hwnd:
    d_gcs = get_dispid("GetClientSize")
    print(f"\nT1 GetClientSize (dispid={d_gcs}, std byref_variant x2):")
    hr, hs, res = call_invoke(d_gcs, [("i8", hwnd), "byref_variant", "byref_variant"])
    print(f"  HRESULT=0x{hr & 0xFFFFFFFF:08X} nret={res}")
    for i, h in enumerate(hs):
        if h is not None:
            print(f"  arg{i}: inner.vt={h.vt:#06x} val={lval_of(h)}")

    print(f"\nT3 GetClientSize (plain non-byref VARIANT):")
    hr, hs, res = call_invoke(d_gcs, [("i8", hwnd), "plain_variant", "plain_variant"])
    print(f"  HRESULT=0x{hr & 0xFFFFFFFF:08X} nret={res}")
    for i, h in enumerate(hs):
        if h is not None:
            print(f"  arg{i}: vt={h.vt:#06x} val={lval_of(h)}")

    print(f"\nT2 GetClientSize (byref_i4 for VARIANT*):")
    hr, hs, res = call_invoke(d_gcs, [("i8", hwnd), "byref_i4", "byref_i4"])
    print(f"  HRESULT=0x{hr & 0xFFFFFFFF:08X} nret={res}")
    for i, h in enumerate(hs):
        if h is not None and hasattr(h, "value"):
            print(f"  arg{i}: holder={h.value}")

print("\nPASS = T0/T1 byref_variant inner.vt==VT_I4(3) with plausible values")
