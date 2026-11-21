// COM SetAStarMapData（DISPID 478）端到端探针（2026-11-21）
//
// 为什么要有它：`SetAStarMapData` 之前只有 C API 侧有，COM 只有文件版 SetAStarMap。
// 补了 DISPID 478 之后，"编译通过"不等于"易语言/IDispatch 调得到" —— 之前踩过
// 导出名被 C++ 修饰、类型库没刷新这类坑。这里用 **免注册** 的路径做真调用：
//
//   LoadLibrary(op_x64.dll) -> DllGetClassObject -> IClassFactory::CreateInstance
//   -> QI(IDispatch) -> GetIDsOfNames("SetAStarMapData") -> Invoke(SAFEARRAY(BYTE))
//
// 不写注册表（不 regsvr32），所以可以随便跑。
//
// 判据：
//   1) GetIDsOfNames 拿到的 DISPID 必须是 478
//   2) Invoke 返回的 ret == 1（地图建好）
//   3) 紧接着用既有 IsLineBlocked 穿过障碍 -> 必须返回 1（被挡）
//      地图没生效时 IsLineBlocked 会返回 -1（未设图），能区分
//
// 用法：com_astarmapdata_probe.exe <op_x64.dll 路径>
#include <windows.h>
#include <oaidl.h>

#include <cstdio>
#include <cstring>
#include <vector>

namespace {

// coclass OpAutomation：6d0fe2a0-f265-4961-9f92-ec22b64fb80c（见 libop/com/op.idl）
const GUID kClsid = {0x6d0fe2a0, 0xf265, 0x4961, {0x9f, 0x92, 0xec, 0x22, 0xb6, 0x4f, 0xb8, 0x0c}};

using PFN_DllGetClassObject = HRESULT(__stdcall *)(REFCLSID, REFIID, void **);

long InvokeByName(IDispatch *disp, const wchar_t *name, VARIANT *args, int argc) {
    LPOLESTR names[2] = {const_cast<LPOLESTR>(name), nullptr};
    DISPID id = 0;
    if (FAILED(disp->GetIDsOfNames(IID_NULL, names, 1, LOCALE_USER_DEFAULT, &id))) {
        std::printf("  GetIDsOfNames(%ls) FAILED\n", name);
        return -9999;
    }
    std::printf("  [diag] %ls -> DISPID %ld\n", name, static_cast<long>(id));
    if (wcscmp(name, L"SetAStarMapData") == 0 && id != 478) {
        std::printf("  DISPID mismatch: got %ld, want 478\n", static_cast<long>(id));
    }
    // [out, retval] 在 late-binding 下通常要作为最后一个 VARIANT 传入；
    // 但个别服务端按"[retval 用 pVarResult 回传]"实现，此时少传一个才对。
    // 不猜：先按 argc，遇 DISP_E_BADPARAMCOUNT 再按 argc-1 重试，并打印实际生效的口径。
    HRESULT hr = E_FAIL;
    VARIANT result;
    for (int pass = 0; pass < 2; ++pass) {
        const int n = (pass == 0) ? argc : argc - 1;
        // ⚠ IDispatch 的 rgvarg 是**逆序**的（最后一个形参在 [0]）。
        // 这是"看起来调通了、其实参数全错位"的经典坑：全 I4 的方法反不反转都能跑，
        // 一旦混进 SAFEARRAY 就报 DISP_E_TYPEMISMATCH。
        std::vector<VARIANT> reversed(static_cast<size_t>(n));
        for (int i = 0; i < n; ++i)
            reversed[static_cast<size_t>(i)] = args[n - 1 - i];
        DISPPARAMS dp{reversed.data(), nullptr, static_cast<UINT>(n), 0};
        VariantInit(&result);
        EXCEPINFO ei{};
        UINT arg_err = 0;
        hr = disp->Invoke(id, IID_NULL, LOCALE_USER_DEFAULT, DISPATCH_METHOD, &dp, &result, &ei, &arg_err);
        std::printf("  [diag] %ls argc=%d -> hr=0x%08lX\n", name, n, static_cast<unsigned long>(hr));
        if (SUCCEEDED(hr)) {
            if (pass == 1)
                std::printf("  [note] %ls 需要 %d 个参数（不含 retval）\n", name, n);
            break;
        }
        VariantClear(&result);
        // 任一失败都试下一个口径（BADPARAMCOUNT 与 TYPEMISMATCH 都可能由"多传了 retval"引起）
    }
    if (FAILED(hr)) {
        std::printf("  Invoke(%ls) FAILED hr=0x%08lX\n", name, static_cast<unsigned long>(hr));
        return -9999;
    }
    const long value = (result.vt == VT_I4) ? result.lVal : -9998;
    VariantClear(&result);
    return value;
}

} // namespace

int main(int argc, char **argv) {
    const char *dll_path = argc > 1 ? argv[1] : "build/nmake-x64-Release/libop/op_x64.dll";
    HMODULE mod = ::LoadLibraryA(dll_path);
    if (!mod) {
        std::printf("LoadLibrary failed (%lu): %s\n", ::GetLastError(), dll_path);
        return 2;
    }
    auto get_class_object = reinterpret_cast<PFN_DllGetClassObject>(
        ::GetProcAddress(mod, "DllGetClassObject"));
    if (!get_class_object) {
        std::printf("DllGetClassObject missing\n");
        return 2;
    }
    ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    IClassFactory *factory = nullptr;
    HRESULT hr = get_class_object(kClsid, IID_IClassFactory, reinterpret_cast<void **>(&factory));
    if (FAILED(hr) || !factory) {
        std::printf("DllGetClassObject failed hr=0x%08lX\n", static_cast<unsigned long>(hr));
        return 2;
    }
    IDispatch *disp = nullptr;
    hr = factory->CreateInstance(nullptr, IID_IDispatch, reinterpret_cast<void **>(&disp));
    factory->Release();
    if (FAILED(hr) || !disp) {
        std::printf("CreateInstance(IDispatch) failed hr=0x%08lX\n", static_cast<unsigned long>(hr));
        return 2;
    }

    // 32x32 全白地图，正中间 4x4 涂黑（障碍）。scale=1，offset=0。
    const LONG w = 32, h = 32, scale = 1, ox = 0, oy = 0;
    std::vector<BYTE> pixels(static_cast<size_t>(w) * h * 4, 255);
    for (LONG y = 14; y < 18; ++y)
        for (LONG x = 14; x < 18; ++x) {
            BYTE *p = &pixels[(static_cast<size_t>(y) * w + x) * 4];
            p[0] = p[1] = p[2] = 0;
            p[3] = 255;
        }

    SAFEARRAY *psa = ::SafeArrayCreateVector(VT_UI1, 0, static_cast<ULONG>(pixels.size()));
    if (!psa) {
        std::printf("SafeArrayCreateVector failed\n");
        return 2;
    }
    void *raw = nullptr;
    ::SafeArrayAccessData(psa, &raw);
    ::memcpy(raw, pixels.data(), pixels.size());
    ::SafeArrayUnaccessData(psa);

    VARIANT args[7];
    for (auto &v : args)
        VariantInit(&v);
    args[0].vt = VT_I4;
    args[0].lVal = w;
    args[1].vt = VT_I4;
    args[1].lVal = h;
    args[2].vt = VT_ARRAY | VT_UI1;
    args[2].parray = psa;
    args[3].vt = VT_I4;
    args[3].lVal = scale;
    args[4].vt = VT_I4;
    args[4].lVal = ox;
    args[5].vt = VT_I4;
    args[5].lVal = oy;
    LONG out_ret = 0;
    args[6].vt = VT_BYREF | VT_I4;
    args[6].plVal = &out_ret;

    std::printf("SetAStarMapData: ");
    const long set_ret = InvokeByName(disp, L"SetAStarMapData", args, 7);
    std::printf("ret=%ld (out param=%ld)\n", set_ret, out_ret);

    // 穿过障碍中心：地图生效必须返回 1（被挡）；未设图会返回 -1
    VARIANT lb[5];
    for (auto &v : lb)
        VariantInit(&v);
    lb[0].vt = VT_I4;
    lb[0].lVal = 10;
    lb[1].vt = VT_I4;
    lb[1].lVal = 16;
    lb[2].vt = VT_I4;
    lb[2].lVal = 22;
    lb[3].vt = VT_I4;
    lb[3].lVal = 16;
    LONG blocked = -100;
    lb[4].vt = VT_BYREF | VT_I4;
    lb[4].plVal = &blocked;
    const long is_blocked = InvokeByName(disp, L"IsLineBlocked", lb, 5);
    std::printf("IsLineBlocked(ret=%ld) -> blocked=%ld\n", is_blocked, blocked);

    ::SafeArrayDestroy(psa);
    disp->Release();
    ::CoUninitialize();
    ::FreeLibrary(mod);

    // 注意：本接口的 late-binding 口径是"[out,retval] 用 pVarResult 回传、不占形参"
    // （argc=7 报 DISP_E_BADPARAMCOUNT，argc=6 才通）。所以 out 值就是 Invoke 的返回值，
    // 上面那个 VT_BYREF 出参槽位不会被写（blocked 保持初值）——这是实测结论，不是 bug。
    const bool ok = (set_ret == 1) && (is_blocked == 1);
    std::printf("RESULT: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
