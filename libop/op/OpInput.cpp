#include "OpContext.h"
#include "OpResult.h"

#include "base/AutomationModes.h"
#include "base/Utils.h"

#include "input/mouse/CursorShape.h"

#include <libop.h>

#include <Windows.h>
#include <wincrypt.h>

// Base64 走 crypt32（共享 onnx 分支 CMake 不链系统库，pragma 补链同 OpCrypto 先例）。
#pragma comment(lib, "crypt32.lib")

#include <algorithm>
#include <cwchar>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace {

using op::input::KeyboardBackend;

struct key_combo_t {
    long vk = 0;
    std::vector<long> modifiers;
};

bool is_named_vk(const std::map<std::wstring, long> &vkmap, const wchar_t *text, long &vk) {
    if (text == nullptr || text[0] == L'\0')
        return false;

    std::wstring key = text;
    wstring2lower(key);
    auto it = vkmap.find(key);
    if (it == vkmap.end())
        return false;

    vk = it->second;
    return true;
}

bool resolve_char_key_combo(const wchar_t ch, key_combo_t &combo) {
    const SHORT mapped = ::VkKeyScanW(ch);
    if (mapped == -1)
        return false;

    combo = {};
    combo.vk = LOBYTE(mapped);

    const BYTE shift_state = HIBYTE(mapped);
    if (shift_state & 1)
        combo.modifiers.push_back(VK_SHIFT);
    if (shift_state & 2)
        combo.modifiers.push_back(VK_CONTROL);
    if (shift_state & 4)
        combo.modifiers.push_back(VK_MENU);

    return combo.vk != 0;
}

bool resolve_text_key_combo(const std::map<std::wstring, long> &vkmap, const wchar_t *text, key_combo_t &combo) {
    long named_vk = 0;
    if (is_named_vk(vkmap, text, named_vk)) {
        combo = {};
        combo.vk = named_vk;
        return true;
    }

    if (text == nullptr || text[0] == L'\0' || text[1] != L'\0')
        return false;

    return resolve_char_key_combo(text[0], combo);
}

// P1 组合键解析（2026-10-15）："ctrl+alt+a" / "shift+f1" / "win+d"。
// 除最后一个 token 外必须是修饰键（ctrl/control/alt/shift/win，重复自动去重）；
// 主键走命名键 vkmap 或单字符（字符自带 shift 态如 '!' 会并入修饰集）。
bool parse_hotkey_combo(const std::map<std::wstring, long> &vkmap, const std::wstring &text, key_combo_t &combo) {
    combo = {};
    if (text.empty())
        return false;

    std::vector<std::wstring> tokens;
    size_t begin = 0;
    while (true) {
        const size_t pos = text.find(L'+', begin);
        tokens.push_back(text.substr(begin, pos == std::wstring::npos ? std::wstring::npos : pos - begin));
        if (pos == std::wstring::npos)
            break;
        begin = pos + 1;
    }
    for (auto &token : tokens) {
        const auto first = token.find_first_not_of(L" \t");
        const auto last = token.find_last_not_of(L" \t");
        token = first == std::wstring::npos ? L"" : token.substr(first, last - first + 1);
        wstring2lower(token);
        if (token.empty())
            return false;
    }

    for (size_t i = 0; i + 1 < tokens.size(); ++i) {
        long modifier = 0;
        if (tokens[i] == L"ctrl" || tokens[i] == L"control")
            modifier = VK_CONTROL;
        else if (tokens[i] == L"alt")
            modifier = VK_MENU;
        else if (tokens[i] == L"shift")
            modifier = VK_SHIFT;
        else if (tokens[i] == L"win")
            modifier = VK_LWIN;
        else
            return false;
        if (std::find(combo.modifiers.begin(), combo.modifiers.end(), modifier) == combo.modifiers.end())
            combo.modifiers.push_back(modifier);
    }

    const std::wstring &main_key = tokens.back();
    long named_vk = 0;
    if (is_named_vk(vkmap, main_key.c_str(), named_vk)) {
        combo.vk = named_vk;
        return combo.vk != 0;
    }
    if (main_key.size() == 1) {
        // resolve_char_key_combo 会整体重置 combo，先在临时对象上解析再合并
        key_combo_t main_combo;
        if (!resolve_char_key_combo(main_key[0], main_combo))
            return false;
        combo.vk = main_combo.vk;
        for (long modifier : main_combo.modifiers) {
            if (std::find(combo.modifiers.begin(), combo.modifiers.end(), modifier) == combo.modifiers.end())
                combo.modifiers.push_back(modifier);
        }
        return true;
    }
    return false;
}

long key_combo_down(KeyboardBackend *keypad, const key_combo_t &combo) {
    for (long modifier : combo.modifiers) {
        if (keypad->KeyDown(modifier) != 1)
            return 0;
    }
    return keypad->KeyDown(combo.vk);
}

long key_combo_up(KeyboardBackend *keypad, const key_combo_t &combo) {
    long ret = keypad->KeyUp(combo.vk);
    if (ret != 1)
        return ret;

    for (auto it = combo.modifiers.rbegin(); it != combo.modifiers.rend(); ++it) {
        if (keypad->KeyUp(*it) != 1)
            return 0;
    }
    return 1;
}

long key_combo_press(KeyboardBackend *keypad, const key_combo_t &combo) {
    if (combo.modifiers.empty())
        return keypad->KeyPress(combo.vk);

    if (key_combo_down(keypad, combo) != 1)
        return 0;
    return key_combo_up(keypad, combo);
}

} // namespace

void op::Op::BindWindow(LONG_PTR hwnd, const wchar_t *display, const wchar_t *mouse, const wchar_t *keypad,
                            long mode, long *ret) {
    BindWindowEx(hwnd, hwnd, display, mouse, keypad, mode, ret);
}

void op::Op::BindWindowEx(LONG_PTR display_hwnd, LONG_PTR input_hwnd, const wchar_t *display,
                              const wchar_t *mouse, const wchar_t *keypad, long mode, long *ret) {
    if (m_context->bkproc.IsBind())
        m_context->bkproc.UnBindWindow();
    internal::set_result(ret, m_context->bkproc.BindWindowEx(display_hwnd, input_hwnd, display, mouse, keypad, mode));
}

void op::Op::UnBindWindow(long *ret) {
    internal::set_result(ret, m_context->bkproc.UnBindWindow());
}

void op::Op::LockInput(long lock, long *ret) {
    internal::set_result(ret, m_context->bkproc.LockInput(lock));
}

void op::Op::DownCpu(long type, long rate, long *ret) {
    internal::set_result(ret, m_context->bkproc.DownCpu(type, rate));
}

void op::Op::SetDxAttr(long attr, long value, long *ret) {
    internal::set_result(ret, m_context->bkproc.SetDxAttr(attr, value));
}

void op::Op::GetDxAttr(long *ret) {
    internal::set_result(ret, m_context->bkproc.GetDxAttr());
}

void op::Op::GetBindWindow(LONG_PTR *ret) {
    internal::set_result(ret, m_context->bkproc.GetBindWindow());
}

void op::Op::IsBind(long *ret) {
    internal::set_result(ret, m_context->bkproc.IsBind());
}

void op::Op::GetDisplayMethod(std::wstring &ret) {
    const wchar_t *name = m_context->bkproc.GetDisplayMethodName();
    ret = name ? name : L"";
}

void op::Op::GetCursorPos(long *x, long *y, long *ret) {
    long cursor_x = 0;
    long cursor_y = 0;
    internal::set_result(ret, m_context->bkproc._mouse->GetCursorPos(cursor_x, cursor_y));
    internal::set_result(x, cursor_x);
    internal::set_result(y, cursor_y);
}

void op::Op::GetCursorShape(std::wstring &ret) {
    m_context->bkproc._mouse->GetCursorShape(ret);
}

void op::Op::GetCursorImage(std::wstring &ret) {
    ret.clear();
    std::vector<unsigned char> bmp;
    if (!cursor_image::FromSystem(bmp, nullptr, nullptr) || bmp.empty())
        return;

    DWORD len = 0;
    if (!::CryptBinaryToStringW(bmp.data(), static_cast<DWORD>(bmp.size()),
                                CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &len))
        return;
    std::wstring out(len, L'\0');
    if (!::CryptBinaryToStringW(bmp.data(), static_cast<DWORD>(bmp.size()),
                                CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, out.data(), &len))
        return;
    out.resize(len);
    while (!out.empty() && out.back() == L'\0')
        out.pop_back();
    ret = std::move(out);
}

void op::Op::MoveR(long x, long y, long *ret) {
    internal::set_result(ret, m_context->bkproc._mouse->MoveR(x, y));
}

void op::Op::MoveTo(long x, long y, long *ret) {
    internal::set_result(ret, m_context->bkproc._mouse->MoveTo(x, y));
}

void op::Op::MoveToEx(long x, long y, long w, long h, std::wstring &ret) {
    int dst_x = x;
    int dst_y = y;
    if (m_context->bkproc._mouse->MoveToEx(x, y, w, h, dst_x, dst_y)) {
        ret = std::to_wstring(dst_x) + L"," + std::to_wstring(dst_y);
    } else {
        ret.clear();
    }
}

void op::Op::MoveToSmooth(long x, long y, long duration, long *ret) {
    internal::set_result(ret, m_context->bkproc._mouse->MoveToSmooth(x, y, duration));
}

void op::Op::MoveToExSmooth(long x, long y, long w, long h, long duration, std::wstring &ret) {
    int dst_x = x;
    int dst_y = y;
    if (m_context->bkproc._mouse->MoveToExSmooth(x, y, w, h, duration, dst_x, dst_y)) {
        ret = std::to_wstring(dst_x) + L"," + std::to_wstring(dst_y);
    } else {
        ret.clear();
    }
}

void op::Op::MovePath(const wchar_t *path, long duration, long *ret) {
    internal::set_result(ret, m_context->bkproc._mouse->MovePath(path ? path : L"", duration));
}

void op::Op::DragPath(const wchar_t *path, long duration, long *ret) {
    internal::set_result(ret, m_context->bkproc._mouse->DragPath(path ? path : L"", duration));
}

void op::Op::SetMouseTrajectory(long mode, long min_duration, long max_duration, long jitter, long start_delay,
                                long end_delay, long *ret) {
    const long result = m_context->bkproc._mouse->SetMouseTrajectory(
        static_cast<int>(mode), static_cast<int>(min_duration), static_cast<int>(max_duration),
        static_cast<int>(jitter), static_cast<int>(start_delay), static_cast<int>(end_delay));
    internal::set_result(ret, result);
}

void op::Op::LeftClick(long *ret) {
    internal::set_result(ret, m_context->bkproc._mouse->LeftClick());
}

void op::Op::LeftDoubleClick(long *ret) {
    internal::set_result(ret, m_context->bkproc._mouse->LeftDoubleClick());
}

void op::Op::LeftDown(long *ret) {
    internal::set_result(ret, m_context->bkproc._mouse->LeftDown());
}

void op::Op::LeftUp(long *ret) {
    internal::set_result(ret, m_context->bkproc._mouse->LeftUp());
}

void op::Op::MiddleClick(long *ret) {
    internal::set_result(ret, m_context->bkproc._mouse->MiddleClick());
}

void op::Op::MiddleDoubleClick(long *ret) {
    internal::set_result(ret, m_context->bkproc._mouse->MiddleDoubleClick());
}

void op::Op::MiddleDown(long *ret) {
    internal::set_result(ret, m_context->bkproc._mouse->MiddleDown());
}

void op::Op::MiddleUp(long *ret) {
    internal::set_result(ret, m_context->bkproc._mouse->MiddleUp());
}

void op::Op::RightClick(long *ret) {
    internal::set_result(ret, m_context->bkproc._mouse->RightClick());
}

void op::Op::RightDoubleClick(long *ret) {
    internal::set_result(ret, m_context->bkproc._mouse->RightDoubleClick());
}

void op::Op::RightDown(long *ret) {
    internal::set_result(ret, m_context->bkproc._mouse->RightDown());
}

void op::Op::RightUp(long *ret) {
    internal::set_result(ret, m_context->bkproc._mouse->RightUp());
}

void op::Op::XButton1Click(long *ret) {
    internal::set_result(ret, m_context->bkproc._mouse->XButton1Click());
}

void op::Op::XButton1DoubleClick(long *ret) {
    internal::set_result(ret, m_context->bkproc._mouse->XButton1DoubleClick());
}

void op::Op::XButton1Down(long *ret) {
    internal::set_result(ret, m_context->bkproc._mouse->XButton1Down());
}

void op::Op::XButton1Up(long *ret) {
    internal::set_result(ret, m_context->bkproc._mouse->XButton1Up());
}

void op::Op::XButton2Click(long *ret) {
    internal::set_result(ret, m_context->bkproc._mouse->XButton2Click());
}

void op::Op::XButton2DoubleClick(long *ret) {
    internal::set_result(ret, m_context->bkproc._mouse->XButton2DoubleClick());
}

void op::Op::XButton2Down(long *ret) {
    internal::set_result(ret, m_context->bkproc._mouse->XButton2Down());
}

void op::Op::XButton2Up(long *ret) {
    internal::set_result(ret, m_context->bkproc._mouse->XButton2Up());
}

void op::Op::Wheel(long delta, long *ret) {
    internal::set_result(ret, m_context->bkproc._mouse->Wheel(delta));
}

void op::Op::HWheel(long delta, long *ret) {
    internal::set_result(ret, m_context->bkproc._mouse->HWheel(delta));
}

void op::Op::WheelDown(long *ret) {
    internal::set_result(ret, m_context->bkproc._mouse->WheelDown());
}

void op::Op::WheelUp(long *ret) {
    internal::set_result(ret, m_context->bkproc._mouse->WheelUp());
}

void op::Op::SetMouseDelay(const wchar_t *type, long delay, long *ret) {
    internal::set_result(ret, 0L);
    if (!type || delay < 0)
        return;
    internal::set_result(ret, 1L);
    if (wcscmp(type, L"normal") == 0)
        MOUSE_NORMAL_DELAY = delay;
    else if (wcscmp(type, L"windows") == 0)
        MOUSE_WINDOWS_DELAY = delay;
    else if (wcscmp(type, L"dx") == 0)
        MOUSE_DX_DELAY = delay;
    else
        internal::set_result(ret, 0L);
}

void op::Op::GetKeyState(long vk_code, long *ret) {
    internal::set_result(ret, m_context->bkproc._keyboard->GetKeyState(vk_code));
}

void op::Op::KeyDown(long vk_code, long *ret) {
    internal::set_result(ret, m_context->bkproc._keyboard->KeyDown(vk_code));
}

void op::Op::KeyDownChar(const wchar_t *vk_code, long *ret) {
    internal::set_result(ret, 0L);
    key_combo_t combo;
    if (resolve_text_key_combo(m_context->vkmap, vk_code, combo))
        internal::set_result(ret, key_combo_down(m_context->bkproc._keyboard.get(), combo));
}

void op::Op::KeyUp(long vk_code, long *ret) {
    internal::set_result(ret, m_context->bkproc._keyboard->KeyUp(vk_code));
}

void op::Op::KeyUpChar(const wchar_t *vk_code, long *ret) {
    internal::set_result(ret, 0L);
    key_combo_t combo;
    if (resolve_text_key_combo(m_context->vkmap, vk_code, combo))
        internal::set_result(ret, key_combo_up(m_context->bkproc._keyboard.get(), combo));
}

void op::Op::WaitKey(long vk_code, long time_out, long *ret) {
    unsigned long t = time_out < 0 ? 0xffffffffu : static_cast<unsigned long>(time_out);
    internal::set_result(ret, m_context->bkproc._keyboard->WaitKey(vk_code, t));
}

void op::Op::KeyPress(long vk_code, long *ret) {
    internal::set_result(ret, m_context->bkproc._keyboard->KeyPress(vk_code));
}

void op::Op::KeyPressChar(const wchar_t *vk_code, long *ret) {
    internal::set_result(ret, 0L);
    key_combo_t combo;
    if (resolve_text_key_combo(m_context->vkmap, vk_code, combo))
        internal::set_result(ret, key_combo_press(m_context->bkproc._keyboard.get(), combo));
}

void op::Op::KeyPressHotkey(const wchar_t *combo_text, long *ret) {
    internal::set_result(ret, 0L);
    key_combo_t combo;
    if (parse_hotkey_combo(m_context->vkmap, combo_text ? std::wstring(combo_text) : std::wstring(), combo))
        internal::set_result(ret, key_combo_press(m_context->bkproc._keyboard.get(), combo));
}

void op::Op::SetKeypadDelay(const wchar_t *type, long delay, long *ret) {
    internal::set_result(ret, 0L);
    if (!type || delay < 0)
        return;
    internal::set_result(ret, 1L);
    if (wcscmp(type, L"normal") == 0)
        KEYPAD_NORMAL_DELAY = delay;
    else if (wcscmp(type, L"normal.hd") == 0)
        KEYPAD_NORMAL2_DELAY = delay;
    else if (wcscmp(type, L"windows") == 0)
        KEYPAD_WINDOWS_DELAY = delay;
    else if (wcscmp(type, L"dx") == 0)
        KEYPAD_DX_DELAY = delay;
    else
        internal::set_result(ret, 0L);
}

void op::Op::KeyPressStr(const wchar_t *key_str, long delay, long *ret) {
    internal::set_result(ret, 0L);
    if (!key_str)
        return;
    auto nlen = wcslen(key_str);
    for (size_t i = 0; i < nlen; ++i) {
        const long key_ret = m_context->bkproc._keyboard->InputChar(key_str[i]);
        internal::set_result(ret, key_ret);
        if (key_ret == 0)
            return;
        // 键间隔拟真抖动（±40%，下限 1ms）：固定间隔是机器打字的最强统计特征，
        // 与鼠标点击节奏（send_input_click/button_click 的 DelayJitter）对齐。
        DelayJitter(delay > 0 ? delay : 1);
    }
}

// ---------------------------------------------------------------- 全局热键
//
// 状态必须进程级共享：op_com / op_c_api 各自静态链入一份 libop，
// 同进程内有两份本 TU 的静态变量。若各建各的窗口/队列，
// 「A 模块注册、B 模块轮询」永远拿不到事件（与时间轴总线同一类坑）。
// 故 hwnd/握手/事件队列全部放进命名共享内存 + 命名事件，注册请求
// 用 SendMessage 同步投给持有窗口的模块。

namespace {

constexpr LONG kHotkeyMaxId = 0xBFFF;
constexpr UINT kHotkeyMsgRegister = WM_APP + 0x51;
constexpr UINT kHotkeyMsgUnregister = WM_APP + 0x52;
constexpr UINT kHotkeyMsgUnregisterAll = WM_APP + 0x53;

struct hotkey_request {
    LONG id = 0;
    LONG modifiers = 0;
    LONG vk = 0;
    LONG ok = 0;
};

struct hotkey_shm {
    volatile LONG state; // 0 未初始化 / 1 创建中 / 2 就绪 / 3 失败
    DWORD tid;
    HWND hwnd;
    volatile LONG head;
    volatile LONG tail;
    volatile LONG dropped;
    LONG ids[256];
};

HANDLE g_hk_map = nullptr;
hotkey_shm *g_hk_shm = nullptr;
HANDLE g_hk_event = nullptr;

std::wstring hk_object_name(const wchar_t *prefix) {
    wchar_t buf[96];
    swprintf(buf, 96, L"Local\\%s_%lu", prefix, static_cast<unsigned long>(::GetCurrentProcessId()));
    return buf;
}

bool hk_valid_args(long id, long modifiers, long vk) {
    return id >= 1 && id <= kHotkeyMaxId &&
           modifiers >= 0 && (modifiers & ~0xFL) == 0 &&
           vk >= 1 && vk <= 0xFF;
}

struct hotkey_sink {
    std::set<LONG> ids;

    static hotkey_sink &instance() {
        static hotkey_sink *sink = new hotkey_sink();
        return *sink;
    }

    static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        if (msg == WM_NCCREATE) {
            auto *cs = reinterpret_cast<CREATESTRUCTW *>(lp);
            ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
            return TRUE;
        }
        auto *self = reinterpret_cast<hotkey_sink *>(
            ::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (!self)
            return ::DefWindowProcW(hwnd, msg, wp, lp);

        switch (msg) {
        case WM_HOTKEY: {
            if (!g_hk_shm)
                break;
            const LONG head = g_hk_shm->head;
            const LONG next = (head + 1) % 256;
            if (next == g_hk_shm->tail) {
                ::InterlockedIncrement(&g_hk_shm->dropped);
            } else {
                g_hk_shm->ids[head] = static_cast<LONG>(wp);
                ::InterlockedExchange(&g_hk_shm->head, next);
                if (g_hk_event)
                    ::SetEvent(g_hk_event);
            }
            return 0;
        }
        case kHotkeyMsgRegister: {
            auto *req = reinterpret_cast<hotkey_request *>(lp);
            if (!req)
                return 0;
            const bool exists = self->ids.count(req->id) != 0;
            req->ok = (!exists &&
                       ::RegisterHotKey(hwnd, static_cast<int>(req->id),
                                        static_cast<UINT>(req->modifiers) | MOD_NOREPEAT,
                                        static_cast<UINT>(req->vk)))
                          ? 1
                          : 0;
            if (req->ok)
                self->ids.insert(req->id);
            return req->ok;
        }
        case kHotkeyMsgUnregister: {
            auto *req = reinterpret_cast<hotkey_request *>(lp);
            if (!req)
                return 0;
            req->ok = (self->ids.erase(req->id) && ::UnregisterHotKey(hwnd, static_cast<int>(req->id)))
                          ? 1
                          : 0;
            return req->ok;
        }
        case kHotkeyMsgUnregisterAll: {
            LONG n = 0;
            for (LONG id : self->ids) {
                if (::UnregisterHotKey(hwnd, static_cast<int>(id)))
                    ++n;
            }
            self->ids.clear();
            return n;
        }
        default:
            break;
        }
        return ::DefWindowProcW(hwnd, msg, wp, lp);
    }

    static DWORD WINAPI thread_proc(LPVOID) {
        if (!g_hk_shm)
            return 1;
        const std::wstring cls = hk_object_name(L"OpHotkeySink");
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = &hotkey_sink::wnd_proc;
        wc.hInstance = ::GetModuleHandleW(nullptr);
        wc.lpszClassName = cls.c_str();
        if (!::RegisterClassExW(&wc) && ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            ::InterlockedExchange(&g_hk_shm->state, 3);
            return 1;
        }
        HWND hwnd = ::CreateWindowExW(0, cls.c_str(), cls.c_str(), 0, 0, 0, 0, 0,
                                      HWND_MESSAGE, nullptr, wc.hInstance, &instance());
        if (!hwnd) {
            ::InterlockedExchange(&g_hk_shm->state, 3);
            return 1;
        }
        g_hk_shm->tid = ::GetCurrentThreadId();
        g_hk_shm->hwnd = hwnd;
        ::InterlockedExchange(&g_hk_shm->state, 2);

        MSG msg;
        while (::GetMessageW(&msg, nullptr, 0, 0) > 0) {
            ::TranslateMessage(&msg);
            ::DispatchMessageW(&msg);
        }
        return 0;
    }
};

bool hk_ensure() {
    if (g_hk_shm && g_hk_shm->state == 2)
        return true;

    const std::wstring map_name = hk_object_name(L"OpHotkeyShm");
    const std::wstring evt_name = hk_object_name(L"OpHotkeyEvt");
    g_hk_map = ::CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                    sizeof(hotkey_shm), map_name.c_str());
    if (!g_hk_map)
        return false;
    g_hk_shm = static_cast<hotkey_shm *>(
        ::MapViewOfFile(g_hk_map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(hotkey_shm)));
    if (!g_hk_shm)
        return false;
    g_hk_event = ::CreateEventW(nullptr, FALSE, FALSE, evt_name.c_str());
    if (!g_hk_event)
        return false;

    const LONG owner = ::InterlockedCompareExchange(&g_hk_shm->state, 1, 0);
    if (owner == 0) {
        HANDLE thread = ::CreateThread(nullptr, 0, &hotkey_sink::thread_proc, nullptr, 0, nullptr);
        if (!thread) {
            ::InterlockedExchange(&g_hk_shm->state, 3);
            return false;
        }
        ::CloseHandle(thread);
    }

    for (int i = 0; i < 1000; ++i) {
        const LONG state = g_hk_shm->state;
        if (state == 2)
            return g_hk_shm->hwnd != nullptr;
        if (state == 3)
            return false;
        ::Sleep(5);
    }
    return false;
}

} // namespace

void op::Op::HotkeyRegister(long id, long modifiers, long vk, long *ret) {
    internal::set_result(ret, 0L);
    if (!hk_valid_args(id, modifiers, vk) || !hk_ensure())
        return;
    hotkey_request req;
    req.id = id;
    req.modifiers = modifiers;
    req.vk = vk;
    ::SendMessageW(g_hk_shm->hwnd, kHotkeyMsgRegister, 0, reinterpret_cast<LPARAM>(&req));
    internal::set_result(ret, req.ok ? 1L : 0L);
}

void op::Op::HotkeyUnregister(long id, long *ret) {
    internal::set_result(ret, 0L);
    if (id < 1 || id > kHotkeyMaxId || !hk_ensure())
        return;
    hotkey_request req;
    req.id = id;
    ::SendMessageW(g_hk_shm->hwnd, kHotkeyMsgUnregister, 0, reinterpret_cast<LPARAM>(&req));
    internal::set_result(ret, req.ok ? 1L : 0L);
}

void op::Op::HotkeyUnregisterAll(long *ret) {
    internal::set_result(ret, 0L);
    if (!hk_ensure())
        return;
    const LRESULT n = ::SendMessageW(g_hk_shm->hwnd, kHotkeyMsgUnregisterAll, 0, 0);
    internal::set_result(ret, static_cast<long>(n));
}

void op::Op::HotkeyPoll(long timeout_ms, long *ret) {
    internal::set_result(ret, 0L);
    if (!hk_ensure())
        return;
    const DWORD timeout = timeout_ms < 0 ? 60000u : static_cast<DWORD>(timeout_ms);
    for (;;) {
        for (;;) {
            const LONG tail = g_hk_shm->tail;
            if (tail == g_hk_shm->head)
                break;
            const LONG id = g_hk_shm->ids[tail];
            const LONG next = (tail + 1) % 256;
            if (::InterlockedCompareExchange(&g_hk_shm->tail, next, tail) == tail) {
                internal::set_result(ret, id);
                return;
            }
        }
        if (timeout_ms == 0)
            return;
        if (::WaitForSingleObject(g_hk_event, timeout) != WAIT_OBJECT_0)
            return;
    }
}
