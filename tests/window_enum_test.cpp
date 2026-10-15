// W1a：EnumWindowInternal（filter 0-31，32 分支 switch）特征化测试。
// 重构前把现状行为钉死：全部基于自建窗口树——父窗口级枚举用「精确集合」断言，
// 桌面级枚举（会带进其他进程窗口）用「成员资格」断言。
// W1b 表驱动重构前后用本文件对拍，任何行为偏移（含历史怪癖）都会被抓住。
#include "test_support.h"

#include <algorithm>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>
#include <windows.h>

namespace {

constexpr wchar_t kClsParent[] = L"OpEnumZParentCls";
constexpr wchar_t kClsKidA[] = L"OpEnumZKidACls";
constexpr wchar_t kClsKidB[] = L"OpEnumZKidBCls";
constexpr wchar_t kClsGrand[] = L"OpEnumZGrandCls";
constexpr wchar_t kClsOwned[] = L"OpEnumZOwnedCls";
constexpr wchar_t kClsHidden[] = L"OpEnumZHiddenCls";

constexpr wchar_t kTitleParent[] = L"OpEnumZ Parent Root";
constexpr wchar_t kTitleKidA[] = L"OpEnumZ Kid Alpha";
constexpr wchar_t kTitleKidB[] = L"OpEnumZ Kid Beta";
constexpr wchar_t kTitleGrand[] = L"OpEnumZ Grand Gamma";
constexpr wchar_t kTitleOwned[] = L"OpEnumZ Owned Top";
constexpr wchar_t kTitleHidden[] = L"OpEnumZ Hidden Top";

void EnsureClass(const wchar_t *name) {
    WNDCLASSW existing{};
    if (::GetClassInfoW(::GetModuleHandleW(nullptr), name, &existing))
        return;
    WNDCLASSW wc{};
    wc.lpfnWndProc = ::DefWindowProcW;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.lpszClassName = name;
    ::RegisterClassW(&wc); // 失败时让随后的 CreateWindow 自然失败，由 ASSERT 兜住
}

// 自建窗口树（句柄值唯一，不与系统窗口混淆）：
//   parent(顶层,可见,owner=0)
//     ├─ kid_a(子,可见)
//     │    └─ grand(孙,可见)      ← 用于验证 filter 1/2/3 的「全深度递归」
//     └─ kid_b(子,隐藏)           ← 用于验证 bit4(16) 可见性过滤
//   owned(顶层,可见,owner=parent) ← 用于验证 bit3(8) owner==0 过滤
//   hidden_top(顶层,隐藏)          ← 用于验证可见性过滤 + filter=4/8 不带可见性要求
struct EnumTree {
    HWND parent = nullptr;
    HWND kid_a = nullptr;
    HWND kid_b = nullptr;
    HWND grand = nullptr;
    HWND owned = nullptr;
    HWND hidden_top = nullptr;

    void Create() {
        const HINSTANCE inst = ::GetModuleHandleW(nullptr);
        EnsureClass(kClsParent);
        EnsureClass(kClsKidA);
        EnsureClass(kClsKidB);
        EnsureClass(kClsGrand);
        EnsureClass(kClsOwned);
        EnsureClass(kClsHidden);
        parent = ::CreateWindowExW(0, kClsParent, kTitleParent, WS_OVERLAPPEDWINDOW | WS_VISIBLE, 96, 96, 320,
                                   200, nullptr, nullptr, inst, nullptr);
        kid_a = ::CreateWindowExW(0, kClsKidA, kTitleKidA, WS_CHILD | WS_VISIBLE, 8, 8, 140, 60, parent, nullptr,
                                  inst, nullptr);
        kid_b = ::CreateWindowExW(0, kClsKidB, kTitleKidB, WS_CHILD, 8, 80, 140, 60, parent, nullptr, inst,
                                  nullptr);
        grand = ::CreateWindowExW(0, kClsGrand, kTitleGrand, WS_CHILD | WS_VISIBLE, 4, 4, 90, 30, kid_a, nullptr,
                                  inst, nullptr);
        owned = ::CreateWindowExW(0, kClsOwned, kTitleOwned, WS_OVERLAPPEDWINDOW | WS_VISIBLE, 480, 96, 320,
                                  200, parent /* owner=parent，仍是桌面子窗口 */, nullptr, inst, nullptr);
        hidden_top = ::CreateWindowExW(0, kClsHidden, kTitleHidden, WS_OVERLAPPEDWINDOW, 864, 96, 320, 200,
                                       nullptr, nullptr, inst, nullptr);
    }

    ~EnumTree() {
        if (hidden_top)
            ::DestroyWindow(hidden_top);
        if (owned)
            ::DestroyWindow(owned);
        if (parent)
            ::DestroyWindow(parent); // 连带销毁 kid_a/kid_b/grand
    }
};

std::vector<HWND> ParseHwndList(const std::wstring &list) {
    std::vector<HWND> result;
    size_t start = 0;
    while (start <= list.size()) {
        const size_t comma = list.find(L',', start);
        const std::wstring token =
            list.substr(start, comma == std::wstring::npos ? std::wstring::npos : comma - start);
        if (!token.empty()) {
            result.push_back(reinterpret_cast<HWND>(
                static_cast<ULONG_PTR>(std::wcstoull(token.c_str(), nullptr, 10))));
        }
        if (comma == std::wstring::npos)
            break;
        start = comma + 1;
    }
    return result;
}

std::wstring FormatHwndList(const std::vector<HWND> &windows) {
    std::wstring out;
    for (HWND hwnd : windows) {
        if (!out.empty())
            out += L',';
        out += std::to_wstring(static_cast<unsigned long long>(reinterpret_cast<ULONG_PTR>(hwnd)));
    }
    return out.empty() ? L"<empty>" : out;
}

void ExpectContains(const std::vector<HWND> &windows, HWND hwnd, const wchar_t *what) {
    EXPECT_NE(std::find(windows.begin(), windows.end(), hwnd), windows.end())
        << what << L" 应出现在枚举结果中，实际=" << FormatHwndList(windows);
}

void ExpectNotContains(const std::vector<HWND> &windows, HWND hwnd, const wchar_t *what) {
    EXPECT_EQ(std::find(windows.begin(), windows.end(), hwnd), windows.end())
        << what << L" 不应出现在枚举结果中，实际=" << FormatHwndList(windows);
}

void ExpectExactSet(const std::vector<HWND> &windows,
                    std::initializer_list<std::pair<HWND, const wchar_t *>> expect) {
    ASSERT_EQ(windows.size(), expect.size()) << L"枚举结果数量不符，实际=" << FormatHwndList(windows);
    for (const auto &entry : expect)
        ExpectContains(windows, entry.first, entry.second);
}

std::wstring CurrentProcessExeName() {
    wchar_t path[MAX_PATH] = {};
    DWORD len = MAX_PATH;
    if (::QueryFullProcessImageNameW(::GetCurrentProcess(), 0, path, &len) == 0 || len == 0)
        return L"";
    const std::wstring full(path, len);
    const size_t pos = full.find_last_of(L"\\/");
    return pos == std::wstring::npos ? full : full.substr(pos + 1);
}

class WindowEnumTest : public ::testing::Test {
  protected:
    void SetUp() override {
        tree_.Create();
        ASSERT_TRUE(::IsWindow(tree_.parent));
        ASSERT_TRUE(::IsWindow(tree_.kid_a));
        ASSERT_TRUE(::IsWindow(tree_.kid_b));
        ASSERT_TRUE(::IsWindow(tree_.grand));
        ASSERT_TRUE(::IsWindow(tree_.owned));
        ASSERT_TRUE(::IsWindow(tree_.hidden_top));
    }

    std::vector<HWND> Enum(LONG_PTR parent, const wchar_t *title, const wchar_t *cls, long filter) {
        std::wstring list;
        op_.EnumWindow(parent, title, cls, filter, list);
        return ParseHwndList(list);
    }

    EnumTree tree_;
    op::Op op_;
};

class WindowEnumByProcessTest : public ::testing::Test {
  protected:
    void SetUp() override {
        tree_.Create();
        proc_name_ = CurrentProcessExeName();
        ASSERT_FALSE(proc_name_.empty());
    }

    std::vector<HWND> EnumByProcess(const wchar_t *title, const wchar_t *cls, long filter) {
        std::wstring list;
        op_.EnumWindowByProcess(proc_name_.c_str(), title, cls, filter, list);
        return ParseHwndList(list);
    }

    EnumTree tree_;
    op::Op op_;
    std::wstring proc_name_;
};

// 前提钉死：可见性沿父链判定、owned 的 owner 语义、标题写入生效。
// 若某台机器上前提不成立，后续特征化测试的失败点可以追溯到本用例。
TEST_F(WindowEnumTest, Premise_VisibilityOwnershipAndTitles) {
    EXPECT_TRUE(::IsWindowVisible(tree_.parent));
    EXPECT_TRUE(::IsWindowVisible(tree_.kid_a));
    EXPECT_FALSE(::IsWindowVisible(tree_.kid_b)) << L"kid_b 无 WS_VISIBLE，预期不可见";
    EXPECT_TRUE(::IsWindowVisible(tree_.grand));
    EXPECT_TRUE(::IsWindowVisible(tree_.owned));
    EXPECT_FALSE(::IsWindowVisible(tree_.hidden_top));

    EXPECT_EQ(::GetWindow(tree_.parent, GW_OWNER), static_cast<HWND>(nullptr));
    EXPECT_EQ(::GetWindow(tree_.kid_a, GW_OWNER), static_cast<HWND>(nullptr));
    EXPECT_EQ(::GetWindow(tree_.owned, GW_OWNER), tree_.parent) << L"owned 的 owner 应为 parent";

    wchar_t buffer[128] = {};
    ::GetWindowTextW(tree_.kid_a, buffer, 128);
    EXPECT_EQ(std::wstring(buffer), std::wstring(kTitleKidA));
}

// case 0：全枚举——父窗口的全部后代（不查标题/类名/可见性）。
TEST_F(WindowEnumTest, Filter0_EnumeratesAllDescendants) {
    ExpectExactSet(Enum(reinterpret_cast<LONG_PTR>(tree_.parent), L"", L"", 0),
                   {{tree_.kid_a, L"kid_a"}, {tree_.kid_b, L"kid_b"}, {tree_.grand, L"grand"}});
}

// case 4：只取父窗口第一层孩子（不带标题/类名过滤，无递归）。
TEST_F(WindowEnumTest, Filter4_FirstLevelChildrenOnly) {
    ExpectExactSet(Enum(reinterpret_cast<LONG_PTR>(tree_.parent), L"", L"", 4),
                   {{tree_.kid_a, L"kid_a"}, {tree_.kid_b, L"kid_b"}});
}

// case 1：标题模糊匹配，且对每个孩子的子树递归同标题匹配（全深度）。
TEST_F(WindowEnumTest, Filter1_TitleMatchesAtAllDepths) {
    ExpectExactSet(Enum(reinterpret_cast<LONG_PTR>(tree_.parent), L"OpEnumZ", L"", 1),
                   {{tree_.kid_a, L"kid_a"}, {tree_.kid_b, L"kid_b"}, {tree_.grand, L"grand"}});
    ExpectExactSet(Enum(reinterpret_cast<LONG_PTR>(tree_.parent), L"Kid", L"", 1),
                   {{tree_.kid_a, L"kid_a"}, {tree_.kid_b, L"kid_b"}});
}

// case 2：类名模糊匹配，同样全深度递归。
TEST_F(WindowEnumTest, Filter2_ClassMatchesAtAllDepths) {
    ExpectExactSet(Enum(reinterpret_cast<LONG_PTR>(tree_.parent), L"", L"OpEnumZ", 2),
                   {{tree_.kid_a, L"kid_a"}, {tree_.kid_b, L"kid_b"}, {tree_.grand, L"grand"}});
    ExpectExactSet(Enum(reinterpret_cast<LONG_PTR>(tree_.parent), L"", L"KidACls", 2), {{tree_.kid_a, L"kid_a"}});
}

// case 3：标题与类名必须同时匹配；且只对「自身匹配」的窗口递归其子树——
// kid_a 不匹配时不会进入其子树，grand 即使匹配也永远到不了（历史怪癖②）。
TEST_F(WindowEnumTest, Filter3_TitleAndClassMustBothMatch) {
    ExpectExactSet(Enum(reinterpret_cast<LONG_PTR>(tree_.parent), L"OpEnumZ", L"OpEnumZ", 3),
                   {{tree_.kid_a, L"kid_a"}, {tree_.kid_b, L"kid_b"}, {tree_.grand, L"grand"}});
    EXPECT_TRUE(Enum(reinterpret_cast<LONG_PTR>(tree_.parent), L"Grand", L"Grand", 3).empty())
        << L"怪癖②：case3 只递归进自身匹配的窗口，kid_a 不匹配 → grand 不可达";
    EXPECT_TRUE(Enum(reinterpret_cast<LONG_PTR>(tree_.parent), L"Kid", L"Grand", 3).empty());
}

// case 8：owner==0——桌面级枚举（含其他进程窗口），只断言成员资格；
// owned 的 owner=parent，必须被 bit3 排除。
TEST_F(WindowEnumTest, Filter8_OwnerZeroTopLevel) {
    const auto windows = Enum(0, L"", L"", 8);
    ExpectContains(windows, tree_.parent, L"parent");
    ExpectContains(windows, tree_.kid_a, L"kid_a");
    ExpectContains(windows, tree_.kid_b, L"kid_b");
    ExpectContains(windows, tree_.grand, L"grand");
    ExpectContains(windows, tree_.hidden_top, L"hidden_top（bit8 不带可见性要求）");
    ExpectNotContains(windows, tree_.owned, L"owned（owner!=0）");
}

// case 16特例①：无视传入 parent，强制桌面；只收可见窗口。
// 传入 parent 却收回 parent 本人/owned，即证明「强制桌面」发生。
TEST_F(WindowEnumTest, Filter16_VisibleOnlyAndForcesDesktopParent) {
    const auto windows = Enum(reinterpret_cast<LONG_PTR>(tree_.parent), L"", L"", 16);
    ExpectContains(windows, tree_.parent, L"parent（可见顶层）");
    ExpectContains(windows, tree_.kid_a, L"kid_a（可见）");
    ExpectContains(windows, tree_.grand, L"grand（可见）");
    ExpectContains(windows, tree_.owned, L"owned（可见，bit16 不查 owner）");
    ExpectNotContains(windows, tree_.hidden_top, L"hidden_top（隐藏）");
    ExpectNotContains(windows, tree_.kid_b, L"kid_b（隐藏）");
}

// case 17：标题+可见——尊重传入 parent（与 case 16 的强制桌面形成对照），隐藏窗口被排除。
TEST_F(WindowEnumTest, Filter17_TitleVisibleRespectsParent) {
    ExpectExactSet(Enum(reinterpret_cast<LONG_PTR>(tree_.parent), L"OpEnumZ", L"", 17),
                   {{tree_.kid_a, L"kid_a"}, {tree_.grand, L"grand"}});
}

// case 24 = 8+16：owner==0 且可见。
TEST_F(WindowEnumTest, Filter24_OwnerZeroAndVisible) {
    const auto windows = Enum(0, L"", L"", 24);
    ExpectContains(windows, tree_.parent, L"parent");
    ExpectContains(windows, tree_.kid_a, L"kid_a");
    ExpectContains(windows, tree_.grand, L"grand");
    ExpectNotContains(windows, tree_.owned, L"owned（owner!=0）");
    ExpectNotContains(windows, tree_.hidden_top, L"hidden_top（隐藏）");
    ExpectNotContains(windows, tree_.kid_b, L"kid_b（隐藏）");
}

// filter>32：解析 Z 序标记（32）后减 32 再分发——33 必须等价于 1；32 本身无对应 case，返回空。
TEST_F(WindowEnumTest, FilterAbove32_ZFlagIsStripped) {
    ExpectExactSet(Enum(reinterpret_cast<LONG_PTR>(tree_.parent), L"OpEnumZ", L"", 33),
                   {{tree_.kid_a, L"kid_a"}, {tree_.kid_b, L"kid_b"}, {tree_.grand, L"grand"}});
    EXPECT_TRUE(Enum(reinterpret_cast<LONG_PTR>(tree_.parent), L"OpEnumZ", L"", 32).empty());
}

// filter=1 但 title 为空：入口校验拒绝，返回空。
TEST_F(WindowEnumTest, Filter1_EmptyTitleRejected) {
    EXPECT_TRUE(Enum(reinterpret_cast<LONG_PTR>(tree_.parent), L"", L"", 1).empty());
}

// case 0 + process_name 互斥：上游大漠同样拒绝，直接返回空。
TEST_F(WindowEnumByProcessTest, Filter0_ProcessNameRejected) {
    EXPECT_TRUE(EnumByProcess(L"", L"", 0).empty());
}

// case 1 + process：只匹配桌面顶层窗口标题，不进子树递归（历史怪癖①——
// 与无进程版 case 1 的「全深度递归」行为不同，重构必须保留或显式决策）。
TEST_F(WindowEnumByProcessTest, Filter1_TitleTopLevelNoRecursion) {
    ExpectExactSet(EnumByProcess(L"Parent", L"", 1), {{tree_.parent, L"parent"}});
    EXPECT_TRUE(EnumByProcess(L"Kid", L"", 1).empty()) << L"进程版 filter=1 不递归，子窗口标题不应命中";
}

// case 4 + process：取进程的第一层窗口（不查 owner/可见性）。
TEST_F(WindowEnumByProcessTest, Filter4_FirstLevelOfProcess) {
    const auto windows = EnumByProcess(L"", L"", 4);
    ExpectContains(windows, tree_.parent, L"parent");
    ExpectContains(windows, tree_.hidden_top, L"hidden_top（bit4 不带可见性）");
    ExpectContains(windows, tree_.owned, L"owned（bit4 不查 owner）");
}

// case 5 = 1+4 + process：标题过滤进程第一层窗口。
TEST_F(WindowEnumByProcessTest, Filter5_TitleWithinFirstLevel) {
    ExpectExactSet(EnumByProcess(L"Owned", L"", 5), {{tree_.owned, L"owned"}});
}

// ---- 组合位补充（W1b 前补拍：bit2/3/4 的交叉组合此前零覆盖）----

// filter=8，parent=父窗口：owner 闸门 + 对每个孩子无条件递归子树（不带可见性要求）。
TEST_F(WindowEnumTest, Filter8_UnderParentRecurseAlways) {
    ExpectExactSet(Enum(reinterpret_cast<LONG_PTR>(tree_.parent), L"", L"", 8),
                   {{tree_.kid_a, L"kid_a"}, {tree_.kid_b, L"kid_b（隐藏但 bit8 不查可见）"}, {tree_.grand, L"grand"}});
}

// filter=12 = 4+8：owner==0 的第一层孩子，不递归（bit2=只首层）。
TEST_F(WindowEnumTest, Filter12_FirstLevelOwnerZeroNoRecurse) {
    ExpectExactSet(Enum(reinterpret_cast<LONG_PTR>(tree_.parent), L"", L"", 12),
                   {{tree_.kid_a, L"kid_a"}, {tree_.kid_b, L"kid_b"}});
}

// filter=20 = 4+16：可见的第一层孩子，不递归——grand 虽可见但被 bit2 挡住。
TEST_F(WindowEnumTest, Filter20_FirstLevelVisibleNoRecurse) {
    ExpectExactSet(Enum(reinterpret_cast<LONG_PTR>(tree_.parent), L"", L"", 20), {{tree_.kid_a, L"kid_a"}});
}

// filter=24 = 8+16（无 bit2）：owner+可见闸门，对通过者无条件递归——grand 经递归进入。
TEST_F(WindowEnumTest, Filter24_UnderParentRecurseAlways) {
    ExpectExactSet(Enum(reinterpret_cast<LONG_PTR>(tree_.parent), L"", L"", 24),
                   {{tree_.kid_a, L"kid_a"}, {tree_.grand, L"grand"}});
}

// filter=28 = 4+8+16：owner+可见的第一层，不递归。
TEST_F(WindowEnumTest, Filter28_FirstLevelOwnerZeroVisibleNoRecurse) {
    ExpectExactSet(Enum(reinterpret_cast<LONG_PTR>(tree_.parent), L"", L"", 28), {{tree_.kid_a, L"kid_a"}});
}

// ByProcess filter=8：owner 闸门 + 递归子树；hidden_top 无可见位要求应入选，owned 因 owner 被排除。
TEST_F(WindowEnumByProcessTest, Filter8_OwnerZeroWithProcess) {
    const auto windows = EnumByProcess(L"", L"", 8);
    ExpectContains(windows, tree_.parent, L"parent");
    ExpectContains(windows, tree_.kid_a, L"kid_a");
    ExpectContains(windows, tree_.kid_b, L"kid_b（bit8 不查可见）");
    ExpectContains(windows, tree_.grand, L"grand");
    ExpectContains(windows, tree_.hidden_top, L"hidden_top");
    ExpectNotContains(windows, tree_.owned, L"owned（owner!=0）");
}

// ByProcess filter=12 = 4+8：owner 闸门 + 「首个进程」递归子树（(true,false,proc)）。
TEST_F(WindowEnumByProcessTest, Filter12_OwnerZeroFirstProcess) {
    const auto windows = EnumByProcess(L"", L"", 12);
    ExpectContains(windows, tree_.parent, L"parent");
    ExpectContains(windows, tree_.kid_a, L"kid_a");
    ExpectContains(windows, tree_.kid_b, L"kid_b");
    ExpectContains(windows, tree_.grand, L"grand");
    ExpectContains(windows, tree_.hidden_top, L"hidden_top");
    ExpectNotContains(windows, tree_.owned, L"owned（owner!=0）");
}

// ByProcess filter=20 = 4+16：可见闸门 + 「首个进程」递归子树；hidden_top/kid_b 因隐藏被排除。
// 注意：bit3(8) 未置位，owned 不受 owner 限制、可见即入选。
TEST_F(WindowEnumByProcessTest, Filter20_VisibleFirstProcess) {
    const auto windows = EnumByProcess(L"", L"", 20);
    ExpectContains(windows, tree_.parent, L"parent");
    ExpectContains(windows, tree_.kid_a, L"kid_a");
    ExpectContains(windows, tree_.grand, L"grand");
    ExpectContains(windows, tree_.owned, L"owned（bit20 无 owner 位，可见即入选）");
    ExpectNotContains(windows, tree_.hidden_top, L"hidden_top（隐藏）");
    ExpectNotContains(windows, tree_.kid_b, L"kid_b（隐藏）");
}

// ByProcess filter=28 = 4+8+16：owner+可见 + 「首个进程」递归子树。
TEST_F(WindowEnumByProcessTest, Filter28_OwnerZeroVisibleFirstProcess) {
    const auto windows = EnumByProcess(L"", L"", 28);
    ExpectContains(windows, tree_.parent, L"parent");
    ExpectContains(windows, tree_.kid_a, L"kid_a");
    ExpectContains(windows, tree_.grand, L"grand");
    ExpectNotContains(windows, tree_.hidden_top, L"hidden_top（隐藏）");
    ExpectNotContains(windows, tree_.kid_b, L"kid_b（隐藏）");
    ExpectNotContains(windows, tree_.owned, L"owned（owner!=0）");
}

} // namespace
