// 32/64 位 COM **vtable（早期绑定）**探针：绕开 IDispatch，直接点接口方法。
//
// 为什么需要它：32 位下走 IDispatch 调 `SetAStarMapData` 报 E_POINTER，
// 而该方法里唯一的 E_POINTER 来源是 `if (!ret) return E_POINTER;` ——
// 也就是 OLEAUT32/ATL 给方法传了 NULL 的 retval 指针。要区分是
//   ① 类型库/分发表的 retval 标记有问题（IDispatch 层）
//   ② 产品实现有问题
// 唯一可靠的办法是绕过 IDispatch：QI 到 IOpAutomation 直接调用。
//
// 用法：com_vtable_probe.exe <op_x86.dll|op_x64.dll>
//   include 路径指向 midl 生成的 op_i.h（先编 op_i.c 一起链接，里面有 IID 定义）
#include <windows.h>
#include <oaidl.h>

#include <cstdio>
#include <cstring>
#include <vector>

#include "op_i.h"

int main(int argc, char **argv) {
    SetConsoleOutputCP(CP_UTF8);
    const char *dll = argc > 1 ? argv[1] : "bin\\x86\\op_x86.dll";
    std::printf("dll=%s (ptr=%d bytes)\n", dll, static_cast<int>(sizeof(void *)));

    HMODULE mod = ::LoadLibraryA(dll);
    if (!mod) {
        std::printf("LoadLibrary failed %lu\n", ::GetLastError());
        return 2;
    }
    auto gco = reinterpret_cast<HRESULT(__stdcall *)(REFCLSID, REFIID, void **)>(
        ::GetProcAddress(mod, "DllGetClassObject"));
    if (!gco) {
        std::printf("DllGetClassObject missing\n");
        return 2;
    }
    ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    IClassFactory *factory = nullptr;
    HRESULT hr = gco(CLSID_OpAutomation, IID_IClassFactory, reinterpret_cast<void **>(&factory));
    if (FAILED(hr) || !factory) {
        std::printf("DllGetClassObject hr=0x%08lX\n", static_cast<unsigned long>(hr));
        return 2;
    }
    IOpAutomation *op = nullptr;
    hr = factory->CreateInstance(nullptr, IID_IOpAutomation, reinterpret_cast<void **>(&op));
    factory->Release();
    std::printf("CreateInstance(IID_IOpAutomation) hr=0x%08lX op=%p\n",
                static_cast<unsigned long>(hr), static_cast<void *>(op));
    if (FAILED(hr) || !op)
        return 2;

    // 32x32 全白，正中 4x4 黑块
    const LONG w = 32, h = 32;
    std::vector<BYTE> px(static_cast<size_t>(w) * h * 4, 255);
    for (LONG y = 14; y < 18; ++y)
        for (LONG x = 14; x < 18; ++x) {
            BYTE *p = &px[(static_cast<size_t>(y) * w + x) * 4];
            p[0] = p[1] = p[2] = 0;
            p[3] = 255;
        }
    SAFEARRAY *psa = ::SafeArrayCreateVector(VT_UI1, 0, static_cast<ULONG>(px.size()));
    void *raw = nullptr;
    ::SafeArrayAccessData(psa, &raw);
    ::memcpy(raw, px.data(), px.size());
    ::SafeArrayUnaccessData(psa);

    LONG set_ret = -1;
    hr = op->SetAStarMapData(w, h, psa, 1, 0, 0, &set_ret);
    std::printf("vtable SetAStarMapData hr=0x%08lX ret=%ld\n", static_cast<unsigned long>(hr), set_ret);

    LONG blocked = -100;
    hr = op->IsLineBlocked(0, 16, 31, 16, &blocked);
    std::printf("vtable IsLineBlocked  hr=0x%08lX blocked=%ld\n", static_cast<unsigned long>(hr), blocked);

    LONG free_space = -100;
    hr = op->IsLineBlocked(0, 2, 31, 2, &free_space);
    std::printf("vtable IsLineBlocked(空地) hr=0x%08lX blocked=%ld\n", static_cast<unsigned long>(hr),
                free_space);

    // 内存版 OCR（DISPID 464，历史上被 midl 吞掉的那个）：只要求不崩、不返回错误
    BSTR ocr_out = nullptr;
    BSTR handle = ::SysAllocString(L"img:99999");  // 故意不存在的句柄
    hr = op->OcrFromMemEx2(handle, 0, 0, 0, 0, nullptr, 0.9, &ocr_out);
    std::printf("vtable OcrFromMemEx2   hr=0x%08lX out=%ls\n", static_cast<unsigned long>(hr),
                ocr_out ? ocr_out : L"(null)");
    ::SysFreeString(handle);
    if (ocr_out)
        ::SysFreeString(ocr_out);

    const bool ok = (set_ret == 1) && (blocked == 1) && (free_space == 0);
    std::printf("RESULT: %s\n", ok ? "PASS" : "FAIL");

    // ---- 诊断：直接读 typeinfo 的参数描述（32/64 对比 IDispatch 行为差异用）----
    // IDispatch 后期绑定是 OLEAUT32 按 typeinfo 里的 FUNCDESC 派发的：
    //   cParams 含不含 retval / 最后一个参数带不带 PARAMFLAG_FRETVAL(0x08)
    // 直接决定 OLEAUT32 会不会给方法传有效的返回值指针。
    ITypeInfo *ti = nullptr;
    if (SUCCEEDED(op->GetTypeInfo(0, LOCALE_USER_DEFAULT, &ti)) && ti) {
        TYPEATTR *ta = nullptr;
        if (SUCCEEDED(ti->GetTypeAttr(&ta)) && ta) {
            std::printf("  typeinfo: cFuncs=%d cVars=%d\n", static_cast<int>(ta->cFuncs),
                        static_cast<int>(ta->cVars));
            for (WORD i = 0; i < ta->cFuncs; ++i) {
                FUNCDESC *fd = nullptr;
                if (FAILED(ti->GetFuncDesc(i, &fd)) || !fd)
                    continue;
                const long mid = static_cast<long>(fd->memid);
                if (mid == 478 || mid == 373) {
                    std::printf("  func memid=%ld cParams=%d cParamsOpt=%d invkind=%d cScodes=%d\n", mid,
                                static_cast<int>(fd->cParams), static_cast<int>(fd->cParamsOpt),
                                static_cast<int>(fd->invkind), static_cast<int>(fd->cScodes));
                    BSTR pnames[16] = {nullptr};
                    UINT got = 0;
                    if (SUCCEEDED(ti->GetNames(static_cast<MEMBERID>(mid), pnames, 16, &got))) {
                        std::printf("    names(%u):", got);
                        for (UINT k = 0; k < got && k < 16; ++k) {
                            std::printf(" %ls", pnames[k] ? pnames[k] : L"?");
                            if (pnames[k])
                                ::SysFreeString(pnames[k]);
                        }
                        std::printf("\n");
                    }
                }
                ti->ReleaseFuncDesc(fd);
            }
            ti->ReleaseTypeAttr(ta);
        }
        ti->Release();
    }

    op->Release();
    if (SUCCEEDED(hr))
        ::CoUninitialize();
    return ok ? 0 : 1;
}
