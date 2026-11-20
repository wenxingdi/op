import ctypes, sys
from ctypes import wintypes, POINTER, byref

class GUID(ctypes.Structure):
    _fields_ = [("Data1", wintypes.DWORD), ("Data2", wintypes.WORD),
                ("Data3", wintypes.WORD), ("Data4", ctypes.c_ubyte * 8)]

ole32 = ctypes.WinDLL("ole32")
ole32.CLSIDFromProgID.argtypes = [wintypes.LPCWSTR, POINTER(GUID)]
ole32.CLSIDFromProgID.restype = ctypes.c_long
ole32.CoInitialize.argtypes = [ctypes.c_void_p]
ole32.CoInitialize(None)
ole32.CoCreateInstance.argtypes = [ctypes.c_void_p, ctypes.c_void_p, wintypes.DWORD,
                                   ctypes.c_void_p, POINTER(ctypes.c_void_p)]
ole32.CoCreateInstance.restype = ctypes.c_long

clsid = GUID()
hr = ole32.CLSIDFromProgID("op.opsoft", byref(clsid))
print("CLSIDFromProgID hr=0x%08X" % (hr & 0xFFFFFFFF))
if hr != 0: sys.exit(1)

IID_IDispatch = GUID(0x00020400,0,0,(ctypes.c_ubyte*8)(0xC0,0,0,0,0,0,0,0x46))
punk = ctypes.c_void_p()
hr = ole32.CoCreateInstance(ctypes.cast(byref(clsid), ctypes.c_void_p), None, 1,
                            ctypes.cast(byref(IID_IDispatch), ctypes.c_void_p), byref(punk))
print("CoCreateInstance hr=0x%08X" % (hr & 0xFFFFFFFF))
if hr != 0: sys.exit(1)

vtable = ctypes.cast(punk, POINTER(POINTER(ctypes.c_void_p)))[0]
GetIDsOfNames = ctypes.WINFUNCTYPE(ctypes.c_long, ctypes.c_void_p, ctypes.c_void_p,
                                   POINTER(wintypes.LPWSTR), ctypes.c_uint, ctypes.c_uint,
                                   POINTER(ctypes.c_long))(vtable[5])

def dispid(name):
    names = (wintypes.LPWSTR * 1)(name)
    d = ctypes.c_long()
    hr = GetIDsOfNames(punk, None, names, 1, 0, byref(d))
    return hr, d.value

print()
print("=== IDispatch::GetIDsOfNames (late binding name -> DISPID) ===")
ok = True
for n, expect in [("FindStrBatch",442),("FindStrEx2",443),("OcrLastProfile",444),
                  ("FindStr",None),("FindStrEx",None),("OcrEx2",440),("OcrAutoColorEx2",441)]:
    hr, d = dispid(n)
    extra = ""
    if expect is not None:
        match = (d == expect)
        extra = "  expect=%d %s" % (expect, "MATCH" if match else "MISMATCH")
        if not match: ok = False
    print("  %-18s hr=0x%08X dispid=%-4d %s%s" % (n, hr & 0xFFFFFFFF, d, "OK" if hr == 0 else "FAIL", extra))
print()
print("RESULT: ALL NEW DISPIDS RESOLVED CORRECTLY" if ok else "RESULT: MISMATCH")
