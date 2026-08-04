#pragma once
#ifndef OP_BASE_AUTOMATION_MODES_H_
#define OP_BASE_AUTOMATION_MODES_H_
#include "Types.h"
#include <cstdint>
// 这三个宏原先是"裸多语句"形式,展开后只有第一条语句受 if 控制:
//     if (cond) SAFE_RELEASE(p); else foo();
// 会展开成 if(cond) if(p) p->Release(); p = nullptr; else foo();  —— 编译错误/语义错乱。
// 统一包进 do{...}while(0),让它们在语法上等价于单条语句。
#define SAFE_CLOSE(h)                                                                                                  \
    do {                                                                                                               \
        if ((h) && (h) != INVALID_HANDLE_VALUE)                                                                        \
            CloseHandle(h);                                                                                            \
        (h) = NULL;                                                                                                    \
    } while (0)

template <class Type> void SAFE_DELETE(Type *&ptr) {
    delete ptr;
    ptr = nullptr;
}

#define SAFE_DELETE_ARRAY(ptr)                                                                                         \
    do {                                                                                                               \
        delete[](ptr);                                                                                                 \
        (ptr) = nullptr;                                                                                               \
    } while (0)

#define SAFE_RELEASE(obj)                                                                                              \
    do {                                                                                                               \
        if (obj) {                                                                                                     \
            (obj)->Release();                                                                                          \
            (obj) = nullptr;                                                                                           \
        }                                                                                                              \
    } while (0)

// #define _sto_wstring(s) boost::locale::conv::to_utf<wchar_t>(s, "GBK")
// #define _wsto_string(s)  boost::locale::conv::from_utf(s,"GBK")

#define DLL_API extern "C" _declspec(dllexport)
// normal windows,gdi;,dx;opengl;
enum RENDER_TYPE {
    NORMAL = 0,
    GDI = 1,
    DX = 2,
    OPENGL = 3
};

// 参数与整体都要加括号:旧写法 GET_RENDER_TYPE(t) (t >> 16) 在
// GET_RENDER_TYPE(x) * 2 处会展开成 x >> 32(移位优先级低于乘法)。
#define MAKE_RENDER(type, flag) (((type) << 16) | (flag))

#define GET_RENDER_TYPE(t) ((t) >> 16)

#define GET_RENDER_FLAG(t) ((t) & 0xffff)

constexpr int RDT_NORMAL = MAKE_RENDER(NORMAL, 0);
constexpr int RDT_NORMAL_DXGI = MAKE_RENDER(NORMAL, 1);
constexpr int RDT_NORMAL_WGC = MAKE_RENDER(NORMAL, 2);
constexpr int RDT_GDI = MAKE_RENDER(GDI, 0);
constexpr int RDT_GDI2 = MAKE_RENDER(GDI, 1);
constexpr int RDT_GDI_DX2 = MAKE_RENDER(GDI, 2);
constexpr int RDT_DX_DEFAULT = MAKE_RENDER(DX, 0);
constexpr int RDT_DX_D3D9 = MAKE_RENDER(DX, 1);
constexpr int RDT_DX_D3D10 = MAKE_RENDER(DX, 2);
constexpr int RDT_DX_D3D11 = MAKE_RENDER(DX, 3);
constexpr int RDT_DX_D3D12 = MAKE_RENDER(DX, 4);
constexpr int RDT_GL_DEFAULT = MAKE_RENDER(OPENGL, 0);
constexpr int RDT_GL_STD = MAKE_RENDER(OPENGL, 1);
constexpr int RDT_GL_NOX = MAKE_RENDER(OPENGL, 2);
constexpr int RDT_GL_ES = MAKE_RENDER(OPENGL, 3);
constexpr int RDT_GL_FI = MAKE_RENDER(OPENGL, 4); // glFinish

enum INPUT_TYPE {
    IN_NORMAL = 0,
    IN_NORMAL2 = 1,
    IN_WINDOWS = 2,
    IN_DX = 3,
};
// define Image byte format
constexpr int IBF_R8G8B8A8 = 0;
constexpr int IBF_B8G8R8A8 = 1;
constexpr int IBF_R8G8B8 = 2;

// const size_t MAX_IMAGE_WIDTH = 1<<11;
// const size_t SHARED_MEMORY_SIZE = 1080 * 1928 * 4;

// 注意:下面两个函数名与它们生成的前缀在历史上是"反的"——
//   MakeOpSharedResourceName() 生成 "op_mutex_<hwnd>"     (实际用作共享内存名)
//   MakeOpMutexName()          生成 "op_shared_mem_<hwnd>" (实际用作互斥体名)
// 宿主进程与被注入 DLL 共用本头文件,所以两边一致、功能正确;
// 但改前缀会导致新旧版本的注入 DLL 互不认识,故此处只加注释不改名值。
// 若要重命名,必须宿主与 hook DLL 同时重编译并重新分发。
// (原先还有 SHARED_RES_NAME_FORMAT / MUTEX_NAME_FORMAT 两个常量,名值同样颠倒
//  且全项目零引用,已删除。)
inline std::wstring MakeOpSharedResourceName(HWND hwnd) {
    return std::wstring(L"op_mutex_") + std::to_wstring(reinterpret_cast<std::uintptr_t>(hwnd));
}

inline std::wstring MakeOpMutexName(HWND hwnd) {
    return std::wstring(L"op_shared_mem_") + std::to_wstring(reinterpret_cast<std::uintptr_t>(hwnd));
}

extern long KEYPAD_NORMAL_DELAY;
extern long KEYPAD_NORMAL2_DELAY;
extern long KEYPAD_WINDOWS_DELAY;
extern long KEYPAD_DX_DELAY;

extern long MOUSE_NORMAL_DELAY;
extern long MOUSE_WINDOWS_DELAY;
extern long MOUSE_DX_DELAY;

#ifndef _M_X64
#define OP64 0
#else
#define OP64 1
#endif

#define _TOSTRING(x) #x

#define MAKE_OP_VERSION(a, b, c, d) _TOSTRING(a##.##b##.##c##.##d)

#ifndef OP_VERSION
#define OP_VERSION MAKE_OP_VERSION(0, 4, 8, 3)
#endif // OP_VERSION
// 模块句柄
// extern HINSTANCE gInstance;
// 是否显示错误信息
// extern int gShowError;
// op 路径
// extern wstring m_opPath;

// extern wstring g_op_name;

#endif // OP_BASE_AUTOMATION_MODES_H_
