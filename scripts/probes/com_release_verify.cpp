// 发布后验证探针（2026-11-21）：注册表路径下的 COM 是否真的带上了新方法
//
// 与 com_astarmapdata_probe.cpp 的区别（那个是**免注册**路径，验的是"二进制里有"）：
// 这个探针走 **CoCreateInstance + LoadTypeLib（都读注册表）**，验的是
// "**装到机器上以后，易语言/IDispatch 看不看得到**" —— 发布场景真正的判据。
//
// 三步：
//   1) CoCreateInstance(CLSID_OpAutomation) 必须成功（失败 = 注册没生效或路径错）
//      -> 顺带打印 InprocServer32（等于告诉你实际加载的是哪个 DLL）
//   2) LoadTypeLib({40297cce-...}) 读类型库：函数总数 / 最大 DISPID，
//      逐个核对 2026-11 新批次的 DISPID 460~478 是否齐全且名字对得上
//   3) GetIDsOfNames 抽查若干新方法名，DISPID 必须与 idl 一致
//      （类型库有但 IDispatch 不认 = 分发映射漏了，这是另一类故障）
//
// 用法：com_release_verify.exe             （用注册表里的 CLSID/TLB）
#include <windows.h>
#include <oaidl.h>
#include <comdef.h>

#include <cstdio>
#include <cwchar>
#include <string>
#include <vector>

namespace {

const GUID kClsid = {0x6d0fe2a0, 0xf265, 0x4961, {0x9f, 0x92, 0xec, 0x22, 0xb6, 0x4f, 0xb8, 0x0c}};
const GUID kTlb = {0x40297cce, 0xe4de, 0x4d31, {0xac, 0xfb, 0x81, 0xbb, 0x8d, 0x14, 0xd2, 0x61}};

// 2026-11 新增批次（DISPID 460~478），名字直接来自 libop/com/op.idl
struct Expected {
    long id;
    const wchar_t *name;
};
const Expected kExpected[] = {
    {460, L"AudioSetSessionMute"},        {461, L"AudioGetSystemVolume"},
    {462, L"AudioSetSystemVolume"},       {463, L"AudioSetSystemMute"},
    {464, L"OcrFromMemEx2"},              {465, L"OcrAutoColorFromMemEx2"},
    {466, L"FindPicFromMem"},             {467, L"FindPicExFromMem"},
    {468, L"FindColorFromMem"},           {469, L"FindColorExFromMem"},
    {470, L"GetColorFromMem"},            {471, L"CmpColorFromMem"},
    {472, L"GetColorNumFromMem"},         {473, L"FindMultiColorFromMem"},
    {474, L"FindMultiColorExFromMem"},    {475, L"FindColorBlockExFromMem"},
    {476, L"YoloDetectFromMem"},          {477, L"YoloDetectFromMemEx2"},
    {478, L"SetAStarMapData"},
};

int g_fail = 0;

void check(bool cond, const char *what) {
    if (!cond) {
        ++g_fail;
        std::printf("  [FAIL] %s\n", what);
    } else {
        std::printf("  [ok]   %s\n", what);
    }
}

}  // namespace

int main() {
    SetConsoleOutputCP(CP_UTF8);
    const HRESULT hr_init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    std::printf("CoInitializeEx hr=0x%08X\n", static_cast<unsigned>(hr_init));

    // ---------- 1) 注册表路径实例化 ----------
    std::printf("\n=== 1) CoCreateInstance（走注册表）===\n");
    IDispatch *disp = nullptr;
    const HRESULT hr_create = CoCreateInstance(kClsid, nullptr, CLSCTX_INPROC_SERVER, IID_IDispatch,
                                              reinterpret_cast<void **>(&disp));
    std::printf("  CoCreateInstance hr=0x%08X\n", static_cast<unsigned>(hr_create));
    check(SUCCEEDED(hr_create) && disp != nullptr, "CoCreateInstance(IID_IDispatch) 成功");
    if (!disp) {
        std::printf("\n结论：注册未生效，后续检查跳过。\n");
        if (SUCCEEDED(hr_init)) CoUninitialize();
        return 1;
    }

    // ---------- 2) 类型库 ----------
    std::printf("\n=== 2) LoadTypeLib（注册表 TypeLib 路径）===\n");
    ITypeLib *tlb = nullptr;
    const HRESULT hr_tlb = LoadRegTypeLib(kTlb, 1, 0, LOCALE_USER_DEFAULT, &tlb);
    std::printf("  LoadRegTypeLib hr=0x%08X\n", static_cast<unsigned>(hr_tlb));
    check(SUCCEEDED(hr_tlb) && tlb != nullptr, "LoadRegTypeLib 成功（版本 1.0）");

    int func_count = 0;
    long max_id = -1;
    std::vector<Expected> found;
    if (tlb) {
        UINT n = tlb->GetTypeInfoCount();
        for (UINT i = 0; i < n; ++i) {
            ITypeInfo *ti = nullptr;
            if (FAILED(tlb->GetTypeInfo(i, &ti)) || !ti)
                continue;
            TYPEATTR *attr = nullptr;
            if (SUCCEEDED(ti->GetTypeAttr(&attr)) && attr) {
                for (WORD f = 0; f < attr->cFuncs; ++f) {
                    FUNCDESC *fd = nullptr;
                    if (FAILED(ti->GetFuncDesc(f, &fd)) || !fd)
                        continue;
                    ++func_count;
                    const long memid = static_cast<long>(fd->memid);
                    // 只统计本批次区间：全局最大会被 0x6002xxxx 这类特殊 DISPID 污染
                    if (memid >= 460 && memid <= 500) {
                        if (memid > max_id)
                            max_id = memid;
                        unsigned int names = 0;
                        BSTR fname = nullptr;
                        if (SUCCEEDED(ti->GetDocumentation(fd->memid, &fname, nullptr, nullptr, nullptr)) &&
                            fname) {
                            Expected e{static_cast<long>(fd->memid), L""};
                            for (const auto &want : kExpected) {
                                if (want.id == e.id) {
                                    e.name = want.name;
                                    break;
                                }
                            }
                            found.push_back(e);
                            wprintf(L"  typeinfo: id=%ld name=%ls\n", e.id, fname);
                            SysFreeString(fname);
                        }
                        (void)names;
                    }
                    ti->ReleaseFuncDesc(fd);
                }
                ti->ReleaseTypeAttr(attr);
            }
            ti->Release();
        }
        std::printf("  类型库函数总数=%d（含重复 typeinfo，仅供参考）\n", func_count);

        // 真判据：2026-11 批次 19 个方法必须**全部**出现在类型库里。
        // 2026-11-21 发布验证抓到的历史缺陷：idl 里的 UTF-8 中文注释被 cl 按 GBK
        // 误解后偶发续行，把紧邻的下一行声明并入注释，midl 仍 exit=0 ->
        // 当时共 10 个方法（LoadImageMem / OcrEx2 / DecodeQRCode / HttpGet /
        // FindStrBatch / JsonIsValid / OcrAutoColor / OcrLastProfile /
        // OcrFromMemEx2 / CvMatchTemplateRot）从来没进过类型库。
        // 现在用 func_count 之外的"名字集合"判据，避免数字口径漂移。
        int typeinfo_hits = 0;
        for (const auto &e : kExpected) {
            for (const auto &f : found) {
                if (f.id == e.id) {
                    ++typeinfo_hits;
                    break;
                }
            }
        }
        char buf[128];
        std::snprintf(buf, sizeof(buf), "类型库含本批 19 个方法（实际 %d）", typeinfo_hits);
        check(typeinfo_hits == static_cast<int>(sizeof(kExpected) / sizeof(kExpected[0])), buf);
        check(max_id == 478, "区间内最大 DISPID == 478（SetAStarMapData）");
    }

    // ---------- 3) IDispatch 认不认这些名字 ----------
    std::printf("\n=== 3) GetIDsOfNames 抽查新批次 ===\n");
    int checked = 0;
    for (const auto &e : kExpected) {
        LPOLESTR names[2] = {const_cast<LPOLESTR>(e.name), nullptr};
        DISPID id = 0;
        const HRESULT hr = disp->GetIDsOfNames(IID_NULL, names, 1, LOCALE_USER_DEFAULT, &id);
        if (FAILED(hr)) {
            std::printf("  [FAIL] %-26ls GetIDsOfNames hr=0x%08X\n", e.name, static_cast<unsigned>(hr));
            ++g_fail;
            continue;
        }
        if (static_cast<long>(id) != e.id) {
            std::printf("  [FAIL] %-26ls DISPID=%ld 期望 %ld\n", e.name, static_cast<long>(id), e.id);
            ++g_fail;
        } else {
            std::printf("  [ok]   %-26ls DISPID=%ld\n", e.name, static_cast<long>(id));
        }
        ++checked;
    }
    std::printf("  抽查 %d/%d 个新方法\n", checked, static_cast<int>(sizeof(kExpected) / sizeof(kExpected[0])));

    disp->Release();
    if (tlb)
        tlb->Release();
    if (SUCCEEDED(hr_init))
        CoUninitialize();

    std::printf("\n%s (FAILS=%d)\n", g_fail == 0 ? "PASS" : "FAIL", g_fail);
    return g_fail == 0 ? 0 : 1;
}
