// 32 位目标 hook 导出名解析（HookExportName.h）的纯函数 + 真实产物判据。
//
// 为什么必须测：32 位目标上**所有依赖注入的显示模式**（dx / dx.d3d9 / dx.d3d11 / opengl 系）
// 全部绑定失败，根因是宿主按未修饰名 `SetDisplayHook` 解析，而 x86 dll 里实际叫
// `_SetDisplayHook@8`（`extern "C"` 只消 C++ 修饰，不消 `__stdcall` 的 `_Name@N`）。
// 这条链路原先零覆盖，只有拿真实 32 位游戏（蜀门 client.exe，i386 + D3D9）实测才暴露。
//
// 三层覆盖：
//   1. IsStdcallDecoratedExportName —— 命名匹配边界。**误匹配最危险**：会把宿主接到
//      另一个函数地址上，表现为"绑定成功但行为诡异"，比"找不到"更难查。
//   2. PickRemoteExportName —— 优先级与回退
//   3. ReadExportNames / ResolveRemoteExportName —— 真实 PE 产物端到端 + 失败语义

#include "test_support.h"

#include "../libop/hook/HookExportName.h"

#include <algorithm>
#include <fstream>
#include <string>
#include <vector>

namespace {

using op::hook::IsStdcallDecoratedExportName;
using op::hook::PickRemoteExportName;
using op::hook::ReadExportNames;
using op::hook::ResolveRemoteExportName;

std::wstring ExeDir() {
    wchar_t buf[MAX_PATH] = {0};
    const DWORD n = ::GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        return std::wstring();
    }
    const std::wstring path(buf, n);
    const auto slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
}

}  // namespace

// ---------------------------------------------------------------------------
// 1. 命名匹配边界
// ---------------------------------------------------------------------------

TEST(HookExportNameMatchTest, StdcallDecorationBoundaries) {
    struct Case {
        const char *decorated;
        const char *plain;
        bool expected;
    };
    const Case cases[] = {
        // — 真实形态（x86 dumpbin -exports op_c_api_x86.dll 实测名） —
        {"_SetDisplayHook@8", "SetDisplayHook", true},
        {"_ReleaseDisplayHook@0", "ReleaseDisplayHook", true},
        {"_SetInputHook@8", "SetInputHook", true},
        {"_ReleaseInputHook@0", "ReleaseInputHook", true},
        {"_SetInputLock@4", "SetInputLock", true},
        {"_GetInputCursorShapeHashLow@0", "GetInputCursorShapeHashLow", true},
        {"_Foo@12", "Foo", true},  // 多位数参数字节数
        // — 必须拒绝 —
        {"SetDisplayHook", "SetDisplayHook", false},      // 未修饰（x64 形态）
        {"SetDisplayHook@8", "SetDisplayHook", false},    // 缺前导下划线
        {"_setdisplayhook@8", "SetDisplayHook", false},   // 大小写不同
        {"_SetInputHookEx@8", "SetInputHook", false},     // 前缀相同的兄弟导出
        {"_SetDisplay@8", "SetDisplayHook", false},       // 只是 plain 的前缀
        // 只有「无 '@'、但 plain 之后是纯数字」这一形状才真正依赖 '@' 位置检查
        // （反向验证实测：去掉该检查时这两条会用例化地暴露，其余诱饵都被"纯数字"检查兜住）
        {"_Foo12", "Foo", false},
        {"_SetDisplayHook12", "SetDisplayHook", false},
        {"_SetDisplayHook@", "SetDisplayHook", false},    // 无参数字节数
        {"_SetDisplayHook@8x", "SetDisplayHook", false},  // 数字后还有字符
        {"_SetDisplayHook@x", "SetDisplayHook", false},   // 非数字
        {"_SetDisplayHook@8 ", "SetDisplayHook", false},  // 尾随空格
        {"", "SetDisplayHook", false},                    // 空名
        // 逆向诱饵：dll 里真有 `_GetInputCursorShapeHash@0`，而宿主查的是 …HashLow
        {"_GetInputCursorShapeHash@0", "GetInputCursorShapeHashLow", false},
        // 空 plain 不得匹配（否则 `_@0` 会通过）
        {"_@0", "", false},
        {"@0", "", false},
        {"SetDisplayHook", "", false},
        {"", "", false},
    };

    // 一条 FAIL 不遮住其余：逐条累计后一次报全（同 REFERENCE「断言不串联」）。
    std::string failures;
    for (const auto &c : cases) {
        const bool got = IsStdcallDecoratedExportName(c.decorated, c.plain);
        if (got != c.expected) {
            failures += "\n  ('" + std::string(c.decorated) + "', '" + std::string(c.plain) +
                        "') 期望 " + (c.expected ? "true" : "false") + "，实得 " +
                        (got ? "true" : "false");
        }
    }
    EXPECT_TRUE(failures.empty()) << "命名匹配判据不符：" << failures;
}

// ---------------------------------------------------------------------------
// 2. 优先级与回退
// ---------------------------------------------------------------------------

TEST(HookExportNamePickTest, PrefersUndecoratedWhenBothPresent) {
    // 未修饰名优先：x64、以及将来给 x86 补了 .def 的构建都走最短路径、行为不变。
    const std::vector<std::string> exports = {"_Foo@8", "Foo", "Bar"};
    EXPECT_EQ(PickRemoteExportName(exports, "Foo"), "Foo");
}

TEST(HookExportNamePickTest, PicksDecoratedWhenUndecoratedAbsent) {
    const std::vector<std::string> exports = {"Bar", "_Foo@8"};
    EXPECT_EQ(PickRemoteExportName(exports, "Foo"), "_Foo@8");
}

TEST(HookExportNamePickTest, ReturnsPlainWhenNothingMatches) {
    // 回退成原名，让上层照常报 "not found" —— 不掩盖真实缺失。
    EXPECT_EQ(PickRemoteExportName({"Bar", "Baz"}, "Foo"), "Foo");
    EXPECT_EQ(PickRemoteExportName({}, "Foo"), "Foo");
}

TEST(HookExportNamePickTest, DoesNotPickSamePrefixSibling) {
    const std::vector<std::string> exports = {"_SetInputHookEx@8"};
    EXPECT_EQ(PickRemoteExportName(exports, "SetInputHook"), "SetInputHook");
}

TEST(HookExportNamePickTest, DeterministicFirstMatch) {
    const std::vector<std::string> exports = {"_Foo@8", "_Foo@4"};
    EXPECT_EQ(PickRemoteExportName(exports, "Foo"), "_Foo@8");
}

TEST(HookExportNamePickTest, RealX86ExportShapeAllResolve) {
    // 照 x86 dll 实测导出名构造：宿主实际会按名解析的 9 个导出必须全部解析得出。
    const std::vector<std::string> exports = {
        "_GetInputCursorShapeHash@0",     "_GetInputCursorShapeHashHigh@0",
        "_GetInputCursorShapeHashLow@0",  "_GetInputCursorShapeMeta@0",
        "_GetInputCursorShapeMetaHigh@0", "_GetInputCursorShapeMetaLow@0",
        "_ReleaseDisplayHook@0",          "_ReleaseInputHook@0",
        "_SetDisplayHook@8",              "_SetInputHook@8",
        "_SetInputLock@4",
    };
    const char *plains[] = {
        "SetDisplayHook",           "ReleaseDisplayHook",         "SetInputHook",
        "ReleaseInputHook",         "SetInputLock",              "GetInputCursorShapeHashLow",
        "GetInputCursorShapeHashHigh", "GetInputCursorShapeMetaLow", "GetInputCursorShapeMetaHigh",
    };
    std::string failures;
    for (const char *p : plains) {
        if (PickRemoteExportName(exports, p) == p) {
            failures += std::string("\n  ") + p + " 未解析出修饰名";
        }
    }
    EXPECT_TRUE(failures.empty()) << "以下导出名未解析：" << failures;
}

// ---------------------------------------------------------------------------
// 3. 真实 PE 产物 + 失败语义
// ---------------------------------------------------------------------------

TEST(HookExportNameFileTest, ReadsRealOpDllExportTable) {
    const std::wstring dir = ExeDir();
    ASSERT_FALSE(dir.empty()) << "取不到测试 exe 目录";

    // op_test.exe 在 build/<cfg>/tests/，op_c_api_x64.dll 在 build/<cfg>/libop/。
    std::vector<std::string> names;
    std::wstring used;
    for (const std::wstring &cand : {dir + L"\\op_c_api_x64.dll",
                                     dir + L"\\..\\libop\\op_c_api_x64.dll"}) {
        if (ReadExportNames(cand, names)) {
            used = cand;
            break;
        }
    }
    if (used.empty()) {
        GTEST_SKIP() << "找不到 op_c_api_x64.dll，跳过真实产物判据";
    }

    const auto has = [&names](const std::string &n) {
        return std::find(names.begin(), names.end(), n) != names.end();
    };
    EXPECT_TRUE(has("OpBindWindow")) << "导出表里没有 OpBindWindow，PE 解析疑似读错位置：" << used;
    // x64 导出名不带 __stdcall 修饰 —— 这正是 x64 一直正常、只有 32 位踩坑的原因。
    EXPECT_FALSE(has("_OpBindWindow@24")) << "x64 出现修饰名，说明解析到了 x86 文件？" << used;
}

TEST(HookExportNameFileTest, MissingFileReturnsFalse) {
    std::vector<std::string> names;
    EXPECT_FALSE(ReadExportNames(L"Z:\\__definitely_missing_op_test__.dll", names));
    EXPECT_TRUE(names.empty());
}

TEST(HookExportNameFileTest, RejectsNonPeFile) {
    wchar_t tmp_dir[MAX_PATH] = {0};
    ASSERT_NE(0u, ::GetTempPathW(MAX_PATH, tmp_dir));
    wchar_t tmp_file[MAX_PATH] = {0};
    ASSERT_NE(0u, ::GetTempFileNameW(tmp_dir, L"opf", 0, tmp_file));
    {
        std::ofstream f(tmp_file, std::ios::binary);
        f << "definitely not a PE file, just text.";
    }
    std::vector<std::string> names;
    const bool ok = ReadExportNames(tmp_file, names);
    ::DeleteFileW(tmp_file);
    EXPECT_FALSE(ok);
    EXPECT_TRUE(names.empty());
}

TEST(HookExportNameResolveTest, MissingDllFallsBackToPlainName) {
    const std::wstring bogus = L"Z:\\__missing_op_test__.dll";
    // 读不到文件时必须**原样返回**原名（让上层照常报 not found），而不是返回空串。
    EXPECT_EQ(ResolveRemoteExportName(bogus, "SetDisplayHook"), "SetDisplayHook");
    // 且不得把失败缓存成终值：第二次调用仍应得到同样结果（dll 稍后落盘也能被解析到）。
    EXPECT_EQ(ResolveRemoteExportName(bogus, "SetDisplayHook"), "SetDisplayHook");
    EXPECT_EQ(ResolveRemoteExportName(bogus, ""), "");
    EXPECT_EQ(ResolveRemoteExportName(bogus, nullptr), "");
}

TEST(HookExportNameResolveTest, RealDllKeepsUndecoratedName) {
    const std::wstring dir = ExeDir();
    ASSERT_FALSE(dir.empty());

    std::wstring dll;
    for (const std::wstring &cand : {dir + L"\\op_c_api_x64.dll",
                                     dir + L"\\..\\libop\\op_c_api_x64.dll"}) {
        std::vector<std::string> probe;
        if (ReadExportNames(cand, probe)) {
            dll = cand;
            break;
        }
    }
    if (dll.empty()) {
        GTEST_SKIP() << "找不到 op_c_api_x64.dll，跳过真实 dll 解析判据";
    }
    // 走完整解析链：x64 上应解析回原名，不得被"修饰名回退"改写成别的东西。
    EXPECT_EQ(ResolveRemoteExportName(dll, "OpBindWindow"), "OpBindWindow");
    // 不存在的导出名也必须原样返回（由上层报 not found）。
    EXPECT_EQ(ResolveRemoteExportName(dll, "OpDefinitelyNotExported"), "OpDefinitelyNotExported");
}
