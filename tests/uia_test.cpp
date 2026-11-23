// UIA 元素层（批 H 2026-11-23）用例。
//
// 测什么：接口契约——JSON 形状、选择器语义、Invoke 真的触发按钮、ValuePattern 读写、
//         元素注册表的释放计数、非法入参显式失败。
// 不测什么：真实第三方软件的读取效果。不同框架（WinForms/WPF/Qt/Chromium）暴露的
//         UIA 树差别很大，那属于人工验收（用 inspect.exe 看一眼就知道）。
//
// 关键设计：窗口必须跑在**独立线程**。本进程既是 UIA client 又是 provider，
// 同一线程上查询会互相等待（provider 要处理 WM_GETOBJECT 才能回答），直接死等。

#include "test_support.h"

#include <gtest/gtest.h>

#include <Windows.h>

#include <atomic>
#include <string>
#include <thread>

#include <op_c_api.h>

namespace {

constexpr int kButtonId = 1001;
constexpr int kEditId = 1002;

struct CApiHandle {
    op_handle handle = OpCreate();
    ~CApiHandle() {
        OpDestroy(handle);
    }
};

// C API 的字符串返回值是共享缓冲，拿到的内容必须立刻拷走。
std::wstring TakeString(const wchar_t *text) {
    return text ? std::wstring(text) : std::wstring();
}

// 从 JSON 里取第一个 "key":<int>。够用就行——用例只关心 count/id 这类平铺数字。
long json_number(const std::wstring &json, const wchar_t *key) {
    const std::wstring pattern = L"\"" + std::wstring(key) + L"\":";
    const size_t pos = json.find(pattern);
    if (pos == std::wstring::npos)
        return -1;
    return _wtoi(json.c_str() + pos + pattern.size());
}

// 独立线程承载的原生窗口：一个 BUTTON + 一个 EDIT（够 UIA 读到两种控件类型）。
class ProbeWindow {
  public:
    ~ProbeWindow() {
        Close();
    }

    bool Start() {
        std::atomic<bool> ready{false};
        thread_ = std::thread([this, &ready] { run(ready); });
        // 等建窗完成（最多 5 秒）；失败时 hwnd_ 仍为空
        for (int i = 0; i < 250 && !ready.load(); ++i)
            ::Sleep(20);
        return hwnd_ != nullptr;
    }

    HWND hwnd() const {
        return hwnd_;
    }
    int click_count() const {
        return clicks_.load();
    }

    void Close() {
        if (hwnd_)
            ::PostMessageW(hwnd_, WM_CLOSE, 0, 0);
        if (thread_.joinable())
            thread_.join();
        hwnd_ = nullptr;
    }

  private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
        if (msg == WM_COMMAND) {
            if (LOWORD(wparam) == kButtonId && HIWORD(wparam) == BN_CLICKED) {
                auto *self = reinterpret_cast<ProbeWindow *>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
                if (self)
                    self->clicks_.fetch_add(1);
                return 0;
            }
        }
        if (msg == WM_CLOSE) {
            ::DestroyWindow(hwnd);
            return 0;
        }
        if (msg == WM_DESTROY) {
            ::PostQuitMessage(0);
            return 0;
        }
        return ::DefWindowProcW(hwnd, msg, wparam, lparam);
    }

    void run(std::atomic<bool> &ready) {
        const wchar_t *cls = L"OpUiaProbeWindow";
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = WndProc;
        wc.hInstance = ::GetModuleHandleW(nullptr);
        wc.lpszClassName = cls;
        wc.hbrBackground = static_cast<HBRUSH>(::GetStockObject(WHITE_BRUSH));
        ::RegisterClassExW(&wc);
        HWND hwnd = ::CreateWindowExW(0, cls, L"uia-probe", WS_OVERLAPPEDWINDOW, 100, 100, 320, 200, nullptr, nullptr,
                                      ::GetModuleHandleW(nullptr), nullptr);
        if (!hwnd) {
            ready.store(true);
            return;
        }
        hwnd_ = hwnd;
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
        ::CreateWindowExW(0, L"BUTTON", L"OK-PROBE", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 10, 10, 120, 30, hwnd,
                          reinterpret_cast<HMENU>(static_cast<INT_PTR>(kButtonId)), ::GetModuleHandleW(nullptr), nullptr);
        ::CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_LEFT, 10, 50, 200, 28, hwnd,
                          reinterpret_cast<HMENU>(static_cast<INT_PTR>(kEditId)), ::GetModuleHandleW(nullptr), nullptr);
        ::ShowWindow(hwnd, SW_SHOWNA);
        ::UpdateWindow(hwnd);
        ready.store(true);

        MSG msg = {};
        while (::GetMessageW(&msg, nullptr, 0, 0) > 0) {
            ::TranslateMessage(&msg);
            ::DispatchMessageW(&msg);
        }
    }

    std::thread thread_;
    HWND hwnd_ = nullptr;
    std::atomic<int> clicks_{0};
};

} // namespace

// Dump 能看见原生子控件（名字 + 类型）
TEST(UiaTest, DumpShowsNativeControls) {
    CApiHandle api;
    ASSERT_NE(nullptr, api.handle);
    ProbeWindow win;
    ASSERT_TRUE(win.Start()) << "建窗口失败（GDI 环境问题）";

    const std::wstring dump = TakeString(OpUiaDump(api.handle, reinterpret_cast<intptr_t>(win.hwnd()), 4));
    ASSERT_FALSE(dump.empty());
    EXPECT_NE(std::wstring::npos, dump.find(L"\"ok\":1")) << "dump: " << dump;
    EXPECT_NE(std::wstring::npos, dump.find(L"OK-PROBE")) << "按钮文本应出现在 dump 里";
    EXPECT_NE(std::wstring::npos, dump.find(L"Button")) << "按钮控件类型应出现";
    EXPECT_GE(json_number(dump, L"count"), 2) << "至少应有按钮 + 编辑框两个节点";
}

// 选择器：name= 精确匹配 -> Invoke 真的把按钮按下去（WM_COMMAND/BN_CLICKED 计数）
TEST(UiaTest, FindByNameAndInvokeClicksButton) {
    CApiHandle api;
    ASSERT_NE(nullptr, api.handle);
    ProbeWindow win;
    ASSERT_TRUE(win.Start());

    const std::wstring found =
        TakeString(OpUiaFind(api.handle, reinterpret_cast<intptr_t>(win.hwnd()), L"name=OK-PROBE", 0));
    ASSERT_NE(std::wstring::npos, found.find(L"\"ok\":1")) << "find: " << found;
    ASSERT_EQ(1, json_number(found, L"count")) << "精确名字应只命中一个";

    const long id = json_number(found, L"id");
    ASSERT_GT(id, 0) << "应注册出元素 id";
    ASSERT_EQ(1, OpUiaInvoke(api.handle, id)) << "按钮应支持 Invoke";

    // Invoke 是异步的（UIA 把点击投递给目标），等一小会再看计数
    for (int i = 0; i < 50 && win.click_count() == 0; ++i)
        ::Sleep(20);
    EXPECT_GE(win.click_count(), 1) << "Invoke 之后按钮应收到 BN_CLICKED";
}

// 选择器：class= / 包含匹配 / index 取第 N 个
TEST(UiaTest, SelectorVariantsWork) {
    CApiHandle api;
    ASSERT_NE(nullptr, api.handle);
    ProbeWindow win;
    ASSERT_TRUE(win.Start());

    const auto hwnd = reinterpret_cast<intptr_t>(win.hwnd());
    const std::wstring by_class = TakeString(OpUiaFind(api.handle, hwnd, L"class=Edit", 0));
    EXPECT_NE(std::wstring::npos, by_class.find(L"\"ok\":1")) << by_class;
    EXPECT_GE(json_number(by_class, L"count"), 1) << "应找到编辑框";

    const std::wstring by_contains = TakeString(OpUiaFind(api.handle, hwnd, L"name*=PROBE", 0));
    EXPECT_GE(json_number(by_contains, L"count"), 1) << "name*= 应做包含匹配";

    const std::wstring by_index = TakeString(OpUiaFind(api.handle, hwnd, L"class=Edit >> index=0", 0));
    EXPECT_GE(json_number(by_index, L"count"), 1) << "index=0 应取到第一个";
    const std::wstring out_of_range = TakeString(OpUiaFind(api.handle, hwnd, L"class=Edit >> index=99", 0));
    EXPECT_EQ(0, json_number(out_of_range, L"count")) << "index 越界应返回 0 条";
}

// ValuePattern 读写：Win32 EDIT 一般支持；若某个环境不暴露则 SKIP（不能假绿也不能假红）
TEST(UiaTest, EditValueRoundTrip) {
    CApiHandle api;
    ASSERT_NE(nullptr, api.handle);
    ProbeWindow win;
    ASSERT_TRUE(win.Start());

    const std::wstring found =
        TakeString(OpUiaFind(api.handle, reinterpret_cast<intptr_t>(win.hwnd()), L"class=Edit", 0));
    const long id = json_number(found, L"id");
    ASSERT_GT(id, 0) << "find: " << found;

    if (OpUiaSetValue(api.handle, id, L"hello-uia") != 1)
        GTEST_SKIP() << "该环境下的 EDIT 不暴露 ValuePattern";

    EXPECT_EQ(std::wstring(L"hello-uia"), TakeString(OpUiaGetValue(api.handle, id)));
}

// 非法入参一律显式失败（ok:0），不能返回空串让调用方以为"没找到"
TEST(UiaTest, InvalidInputsFailExplicitly) {
    CApiHandle api;
    ASSERT_NE(nullptr, api.handle);
    ProbeWindow win;
    ASSERT_TRUE(win.Start());
    const auto hwnd = reinterpret_cast<intptr_t>(win.hwnd());

    EXPECT_NE(std::wstring::npos, TakeString(OpUiaFind(api.handle, hwnd, L"bogus=1", 0)).find(L"\"ok\":0"))
        << "未知键应失败";
    EXPECT_NE(std::wstring::npos, TakeString(OpUiaFind(api.handle, hwnd, L"", 0)).find(L"\"ok\":0"))
        << "空选择器应失败";
    EXPECT_NE(std::wstring::npos, TakeString(OpUiaFind(api.handle, hwnd, L"name", 0)).find(L"\"ok\":0"))
        << "缺少 '=' 应失败";
    EXPECT_NE(std::wstring::npos,
              TakeString(OpUiaFind(api.handle, static_cast<intptr_t>(0xDEADBEEF), L"name=x", 0)).find(L"\"ok\":0"))
        << "无效窗口句柄应失败";

    EXPECT_EQ(0, OpUiaInvoke(api.handle, 999999)) << "不存在的元素 id 应失败";
    EXPECT_EQ(0, OpUiaSetValue(api.handle, 999999, L"x")) << "不存在的元素 id 应失败";
    EXPECT_EQ(std::wstring(), TakeString(OpUiaGetValue(api.handle, 999999))) << "不存在的元素应返回空串";
}

// 注册表释放：ReleaseAll 返回释放个数，再调一次应为 0（幂等）
TEST(UiaTest, ReleaseAllReturnsCountAndIsIdempotent) {
    CApiHandle api;
    ASSERT_NE(nullptr, api.handle);
    ProbeWindow win;
    ASSERT_TRUE(win.Start());

    // 先制造一些注册项
    OpUiaDump(api.handle, reinterpret_cast<intptr_t>(win.hwnd()), 2);
    const int released = OpUiaReleaseAll(api.handle);
    EXPECT_GT(released, 0) << "dump 之后应有元素在注册表里";
    EXPECT_EQ(0, OpUiaReleaseAll(api.handle)) << "清空后再调应为 0";
}
