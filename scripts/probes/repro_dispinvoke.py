# -*- coding: utf-8 -*-
"""裸调 IDispatch::Invoke（dispid 67 = GetClientSize），复刻 PS 的封送方式，
判定丢值发生在：① PS 封送 ② 服务端 IDispatch/typelib 路径。"""
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
    _fields_ = [
        ("vt", VARTYPE),
        ("r1", wintypes.WORD), ("r2", wintypes.WORD), ("r3", wintypes.WORD),
        ("_", ctypes.c_void_p),  # union，直接用原始指针
    ]

class DISPPARAMS(ctypes.Structure):
    _fields_ = [
        ("rgvarg", ctypes.POINTER(VARIANT)),
        ("rgdispidNamedArgs", ctypes.POINTER(ctypes.c_long)),
        ("cArgs", wintypes.UINT),
        ("cNamedArgs", wintypes.UINT),
    ]

class EXCEPINFO(ctypes.Structure):
    _fields_ = [(n, wintypes.WORD) for n in ("wCode", "wReserved")] + \
               [(n, ctypes.c_void_p) for n in ("bstrSource", "bstrDescription", "bstrHelpFile")] + \
               [(n, wintypes.DWORD) for n in ("dwReserved1",)] + \
               [("pvReserved", ctypes.c_void_p), ("pfnDeferredFillIn", ctypes.c_void_p),
                ("scode", ctypes.c_long)]

oleaut32 = ctypes.oledll.oleaut32
u32 = ctypes.windll.user32

# ---- 找记事本窗口 ----
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

r = wintypes.RECT()
u32.GetClientRect(hwnd, ctypes.byref(r))
print(f"[native] GetClientRect = {r.right}x{r.bottom}")

# ---- 拿 IDispatch ----
import comtypes, comtypes.client, comtypes.automation as auto
op = comtypes.client.CreateObject("op.opsoft", interface=auto.IDispatch)
disp = op  # IDispatch 指针
lpVtbl = disp.lpVtbl if hasattr(disp, "lpVtbl") else None
# comtypes 的 IDispatch 包装：直接用其 Invoke 原始方法
# disp 是 POINTER(IDispatch)；取 vtable 里的 Invoke (槽位 6：QI/AddRef/Release/GetTypeInfoCount/GetTypeInfo/GetIDsOfNames/Invoke → 索引 6)
# disp 是 POINTER(IDispatch)：对象首字段 = vtable 指针；Invoke 是 vtable 槽 6
obj_addr = ctypes.addressof(disp.contents) if hasattr(disp, "contents") else ctypes.cast(disp, ctypes.c_void_p).value
vtbl_addr = ctypes.c_void_p.from_address(obj_addr).value
entries = ctypes.cast(vtbl_addr, ctypes.POINTER(ctypes.c_void_p * 7)).contents
INVOKE = ctypes.WINFUNCTYPE(ctypes.c_long, ctypes.c_void_p, ctypes.c_long, ctypes.c_void_p, wintypes.DWORD, wintypes.DWORD, ctypes.POINTER(DISPPARAMS), ctypes.POINTER(VARIANT), ctypes.POINTER(EXCEPINFO), ctypes.POINTER(wintypes.UINT))
fnInvoke = ctypes.cast(entries[6], INVOKE)

# GetIDsOfNames（槽 5）：解析真实 dispid
GETIDSOFNAMES = ctypes.WINFUNCTYPE(ctypes.c_long, ctypes.c_void_p, ctypes.c_void_p, ctypes.POINTER(ctypes.c_wchar_p), wintypes.UINT, wintypes.LCID, ctypes.POINTER(ctypes.c_long))
fnGetIds = ctypes.cast(entries[5], GETIDSOFNAMES)

def get_dispid(name):
    dispid = ctypes.c_long(-1)
    names = (ctypes.c_wchar_p * 1)(name)
    hr = fnGetIds(disp, riid_null, names, 1, 0, ctypes.byref(dispid))
    return hr, dispid.value

riid_null = ctypes.c_void_p(0)

hr_id, real_dispid = get_dispid("GetClientSize")
print(f"GetIDsOfNames('GetClientSize') hr=0x{hr_id & 0xFFFFFFFF:08X} dispid={real_dispid} (IDL id=67)")


def call_invoke(dispid, args_desc, want_result=False):
    """args_desc: list of (kind,)，顺序=参数自然顺序，内部反转。
    返回 (hr, holders, res_val)：holders 按参数顺序保活可读回。"""
    variants = []
    holders = []
    for kind in args_desc:
        v = VARIANT()
        if kind == "byref_variant":
            inner = VARIANT()
            inner.vt = VT_EMPTY
            inner._ = None
            v.vt = VT_BYREF | VT_VARIANT
            v._ = ctypes.cast(ctypes.pointer(inner), ctypes.c_void_p)
            holders.append(inner)
        elif kind == "byref_i4":
            v.vt = VT_BYREF | VT_I4
            holder = wintypes.LONG(-1)
            v._ = ctypes.cast(ctypes.pointer(holder), ctypes.c_void_p)
            holders.append(holder)
        elif kind == "i8":
            v.vt = VT_I8
            v._ = ctypes.c_void_p(hwnd & 0xFFFFFFFFFFFFFFFF)
            holders.append(None)
        elif kind == "plain_variant":
            v.vt = VT_EMPTY  # 非 byref 的空 VARIANT，模拟 PS 的临时副本
            v._ = None
            holders.append(v)  # 直接读自身
        variants.append(v)
    rev = variants[::-1]
    arr = (VARIANT * len(rev))(*rev)
    dp = DISPPARAMS()
    dp.rgvarg = ctypes.cast(arr, ctypes.POINTER(VARIANT))
    dp.cArgs = len(rev)
    dp.cNamedArgs = 0
    result = VARIANT()
    ei = EXCEPINFO()
    argerr = wintypes.UINT(0)
    pRes = ctypes.byref(result) if want_result else None
    hr = fnInvoke(disp, dispid, riid_null, 0, DISPATCH_METHOD | DISPATCH_PROPERTYGET,
                  ctypes.byref(dp), pRes, ctypes.byref(ei), ctypes.byref(argerr))
    res_val = None
    if want_result and hr == 0 and result.vt == VT_I4:
        res_val = wintypes.LONG.from_address(ctypes.addressof(result) + 8).value
    return hr, holders, res_val

USE_DISPID = real_dispid if hr_id == 0 else 67

print(f"T1 (std byref_variant x2, retval via pVarResult, dispid={USE_DISPID}):")
hr1, hs1, res1 = call_invoke(USE_DISPID, ["i8", "byref_variant", "byref_variant"], want_result=True)
print(f"T1 HRESULT=0x{hr1 & 0xFFFFFFFF:08X}  nret={res1}")
for i, h in enumerate(hs1):
    if h is not None:
        lval = wintypes.LONG.from_address(ctypes.addressof(h) + 8).value
        print(f"  arg{i}: inner.vt={h.vt} val={lval}")

print(f"T3 (plain VT_VARIANT non-byref, dispid={USE_DISPID}):")
hr3, hs3, res3 = call_invoke(USE_DISPID, ["i8", "plain_variant", "plain_variant"], want_result=True)
print(f"T3 HRESULT=0x{hr3 & 0xFFFFFFFF:08X}  nret={res3}")
if hr3 == 0:
    for i, h in enumerate(hs3):
        if h is not None:
            lval = wintypes.LONG.from_address(ctypes.addressof(h) + 8).value
            print(f"  arg{i}: vt={h.vt:#06x} val={lval}")

print(f"T2 (PS-suspect byref_i4 for VARIANT*, dispid={USE_DISPID}):")
hr2, hs2, res2 = call_invoke(USE_DISPID, ["i8", "byref_i4", "byref_i4"], want_result=True)
print(f"T2 HRESULT=0x{hr2 & 0xFFFFFFFF:08X}  nret={res2}")
if hr2 == 0:
    for i, h in enumerate(hs2):
        if h is not None:
            print(f"  arg{i}: holder={h.value}")

print("T1 (std byref_variant x2, retval via pVarResult):")
hr, vs = call_invoke(67, [
    ("i8", hwnd),
    ("byref_variant", None),
    ("byref_variant", None),
], want_result=True)
print(f"T1 HRESULT=0x{hr & 0xFFFFFFFF:08X}  result.vt={hr_res[0] if hr_res else '?'} result={hr_res[1] if hr_res else '?'}")
for i, (v, inner, slot) in enumerate(vs):
    if v.vt == (VT_BYREF | VT_VARIANT) and inner is not None:
        lval = wintypes.LONG.from_address(ctypes.addressof(inner) + 8).value
        print(f"  arg{i}: inner.vt={inner.vt} val={lval}")

# T2: 模拟 PS —— width/height 传 VT_BYREF|VT_I4（类型不匹配猜想）
print("T2 (byref_i4 for VARIANT*):")
hr2, vs2 = call_invoke(67, [
    ("i8", hwnd),
    ("byref_i4", -555),
    ("byref_i4", -556),
], want_result=True)
print(f"T2 HRESULT=0x{hr2 & 0xFFFFFFFF:08X}")
if hr2 == 0:
    print(f"  result={hr_res2}")
    for i, (v, inner, slot) in enumerate(vs2):
        if inner is not None:
            print(f"  arg{i}: vt={v.vt:#06x} holder={inner.value}")
