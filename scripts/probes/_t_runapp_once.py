# -*- coding: utf-8 -*-
"""run_app 单实例启动蜀门 game.exe（mode=0）。"""
import ctypes
import os

DLL_DIR = r"D:\AutoPro\op-master\op\build\nmake-x64-Release\libop"
os.environ["PATH"] = DLL_DIR + ";" + os.environ["PATH"]
os.add_dll_directory(DLL_DIR)

dll = ctypes.CDLL(os.path.join(DLL_DIR, "op_c_api_x64.dll"))
dll.OpCreate.restype = ctypes.c_void_p
dll.OpDestroy.argtypes = [ctypes.c_void_p]
dll.OpRunApp.argtypes = [ctypes.c_void_p, ctypes.c_wchar_p, ctypes.c_int,
                         ctypes.POINTER(ctypes.c_uint32)]
dll.OpRunApp.restype = ctypes.c_int

h = dll.OpCreate()
pid = ctypes.c_uint32(0)
ret = dll.OpRunApp(h, r"D:\Program Files (x86)\shumen\game.exe", 1, ctypes.byref(pid))
print(f"ret={ret} pid={pid.value}")
dll.OpDestroy(h)
