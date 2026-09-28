#pragma once
#ifndef OP_LIBOP_COM_COM_VARIANT_H_
#define OP_LIBOP_COM_COM_VARIANT_H_

// COM late-binding 出参封送的纯函数（header-only）。
//
// 起因：COM 是易语言 / VBScript / PowerShell 的真实入口，而动态调用（IDispatch）
// 传进来的出参形态与 C++ 直接调用完全不同 —— 09-19 连续两个 late-binding 出参
// bug（`79aede7`、`8da94da`）都出在这里。这些判据原先住在 OpAutomation.cpp 的匿名
// 命名空间里，测试链接不进去（OpAutomation.cpp 依赖 ATL/COM 运行时），
// 于是「VT_BYREF|VT_I4 要写穿、VT_BYREF|VT_VARIANT 要解一层」长期零覆盖。
//
// 搬到这里后测试可直接 include。改动本文件请同步跑 tests/com_variant_test.cpp。
//
// 说明：依赖 ATL 的 CopyOutBstr（CComBSTR）不搬，留在 OpAutomation.cpp。

#include <windows.h>
#include <wtypes.h>

namespace op {
namespace com {

template <typename Target, typename Value>
HRESULT SetOutValue(Target *target, Value value) {
    if (!target)
        return E_POINTER;
    *target = static_cast<Target>(value);
    return S_OK;
}

template <typename Callback>
HRESULT RunCvRetOnly(LONG *ret, Callback &&callback) {
    if (!ret)
        return E_POINTER;

    SetOutValue(ret, 0L);
    callback(ret);
    return S_OK;
}

// ---- VARIANT LONG in/out helpers (IDispatch late-binding adaptation) ----
// Dynamic dispatch clients (PowerShell [ref]) pass VT_BYREF|VT_I4 wrappers:
// oleaut hands the server the wrapper VARIANT itself, so the value must be
// written through the byref pointer instead of overwriting the wrapper.
// Standard VT_BYREF|VT_VARIANT args are dereferenced one level.
inline LONG InLong(const VARIANT *v) {
    if (!v)
        return 0;
    if (v->vt == (VT_BYREF | VT_I4))
        return v->plVal ? *v->plVal : 0;
    if (v->vt == (VT_BYREF | VT_VARIANT) && v->pvarVal) {
        const VARIANT &inner = *v->pvarVal;
        if (inner.vt == VT_I4 || inner.vt == VT_INT)
            return inner.lVal;
        if (inner.vt == (VT_BYREF | VT_I4))
            return inner.plVal ? *inner.plVal : 0;
        return 0;
    }
    if (v->vt & VT_BYREF)
        return 0;
    return v->lVal;
}

inline void OutLong(VARIANT *v, LONG value) {
    if (!v)
        return;
    if (v->vt == (VT_BYREF | VT_I4)) {  // PowerShell [ref] wrapper: write through
        if (v->plVal)
            *v->plVal = value;
        return;
    }
    if (v->vt == (VT_BYREF | VT_VARIANT)) {  // byref VARIANT: write the inner
        if (v->pvarVal) {
            v->pvarVal->vt = VT_I4;
            v->pvarVal->lVal = value;
        }
        return;
    }
    if (v->vt & VT_BYREF)  // unsupported byref payload (BYREF|BSTR etc.): leave untouched
        return;
    v->vt = VT_I4;
    v->lVal = value;
}

}  // namespace com
}  // namespace op

#endif  // OP_LIBOP_COM_COM_VARIANT_H_
