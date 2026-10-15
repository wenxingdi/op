// #include "stdafx.h"
#include "BindingSession.h"
#include "../base/AutomationModes.h"
#include "../base/Environment.h"
#include "../base/Utils.h"
#include "../base/WindowsVersion.h"
#include <algorithm>
#include <cwctype>
#include <vector>

#include "../capture/backends/DxgiCapture.h"
#include "../capture/backends/GdiCapture.h"
#include "../capture/backends/HookCapture.h"
#ifdef OP_ENABLE_WGC
#include "../capture/backends/WgcCapture.h"
#endif

#include "../capture/sources/MemoryImageSource.h"
#include "../hook/HookProtocol.h"
#include "../hook/InputHookClient.h"
#include "../input/keyboard/DxKeyboard.h"
#include "../input/keyboard/WinKeyboard.h"
#include "../input/mouse/DxMouse.h"
#include <memory>

namespace op::binding {

using op::capture::DxgiCapture;
using op::capture::GdiCapture;
using op::capture::HookCapture;
using op::capture::ICaptureBackend;
#ifdef OP_ENABLE_WGC
using op::capture::WgcCapture;
#endif
using op::input::DxKeyboard;
using op::input::DxMouse;
using op::input::KeyboardBackend;
using op::input::WinKeyboard;
using op::input::WinMouse;

namespace {

std::wstring to_lower_ascii(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch) {
        return static_cast<wchar_t>(std::towlower(ch));
    });
    return value;
}

// 解析 dx 输入的通道后缀。s 必须以 "dx" 开头（主模式 dx）。
// 成功返回 true；mask 输出后缀位掩码（0 表示无后缀，沿用默认全开）。
// 支持 "dx" / "dx.dinput" / "dx.raw" / "dx.win" 及组合 "dx.dinput+raw"（大小写不敏感）。
static bool parse_dx_channel_suffix(const std::wstring &s, int &mask) {
    mask = 0;
    if (s.rfind(L"dx", 0) != 0)
        return false;
    if (s == L"dx")
        return true; // 无后缀，保持默认全开
    if (s.size() <= 3 || s[2] != L'.')
        return false;
    const std::wstring suffix = s.substr(3); // 跳过 "dx."
    size_t start = 0;
    for (;;) {
        const size_t pos = suffix.find(L'+', start);
        const std::wstring token =
            (pos == std::wstring::npos) ? suffix.substr(start) : suffix.substr(start, pos - start);
        if (token.empty())
            return false;
        const std::wstring low = to_lower_ascii(token);
        if (low == L"dinput" || low == L"di")
            mask |= DX_ATTR_DINPUT;
        else if (low == L"raw" || low == L"rawinput")
            mask |= DX_ATTR_RAWINPUT;
        else if (low == L"win" || low == L"window" || low == L"windowmsg")
            mask |= DX_ATTR_WINDOWMSG;
        else
            return false; // 非法后缀
        if (pos == std::wstring::npos)
            break;
        start = pos + 1;
    }
    return true;
}

std::wstring window_class_name(HWND hwnd) {
    std::wstring buffer(256, L'\0');
    for (;;) {
        const int copied = ::GetClassNameW(hwnd, buffer.data(), static_cast<int>(buffer.size()));
        if (copied <= 0)
            return L"";
        if (static_cast<size_t>(copied) < buffer.size() - 1) {
            buffer.resize(static_cast<size_t>(copied));
            return buffer;
        }
        buffer.assign(buffer.size() * 2, L'\0');
    }
}

bool class_contains(const std::wstring &class_name_lower, const wchar_t *needle) {
    return class_name_lower.find(needle) != std::wstring::npos;
}

bool class_equals(const std::wstring &class_name_lower, const wchar_t *needle) {
    return class_name_lower == needle;
}

bool prefers_wgc(HWND hwnd) {
    const std::wstring class_name = to_lower_ascii(window_class_name(hwnd));
    if (class_name.empty())
        return false;

    // needle 均为小写字面量：class_name 已整体小写化，直接比较（Win32 类名本就不区分大小写）。
    const wchar_t *partial_matches[] = {L"chrome", L"mozilla", nullptr};
    for (const wchar_t **match = partial_matches; *match; ++match) {
        if (class_contains(class_name, *match))
            return true;
    }

    const wchar_t *whole_matches[] = {
        L"applicationframewindow",
        L"windows.ui.core.corewindow",
        L"winuidesktopwin32windowclass",
        L"gamingservicesui_hosting_window_class",
        L"xlmain",
        L"pptframeclass",
        L"screenclass",
        L"podiumparent",
        L"opusapp",
        L"omain",
        L"framework::cframe",
        L"rctrl_renwnd32",
        L"mswinpub",
        L"officeapp-frame",
        L"sdl_app",
        nullptr,
    };
    for (const wchar_t **match = whole_matches; *match; ++match) {
        if (class_equals(class_name, *match))
            return true;
    }

    return false;
}

std::vector<int> choose_auto_displays(HWND hwnd) {
    std::vector<int> displays;
    // 对已知容易被 GDI/DXGI 截黑的窗口优先 WGC，其余普通窗口优先 DXGI。
#ifdef OP_ENABLE_WGC
    if (IsWindows10BuildOrGreater(kWindows10Build1903) && prefers_wgc(hwnd))
        displays.push_back(RDT_NORMAL_WGC);
#else
    (void)hwnd;
#endif
    if (IsWindowsVersionAtLeast(6, 2, 0))
        displays.push_back(RDT_NORMAL_DXGI);
    displays.push_back(RDT_NORMAL);
    return displays;
}

// display 模式名 ↔ RDT_* 单一映射表：BindWindowEx 的解析与 GetDisplayMethodName 的命名
// 共用此表，避免两份手写 switch 双向漂移。
struct DisplayModeEntry {
    const wchar_t *name;
    int mode;
};
constexpr DisplayModeEntry kDisplayModes[] = {
    {L"normal", RDT_NORMAL},
    {L"normal.dxgi", RDT_NORMAL_DXGI},
    {L"normal.wgc", RDT_NORMAL_WGC},
    {L"gdi", RDT_GDI},
    {L"gdi2", RDT_GDI2},
    {L"dx2", RDT_GDI_DX2},
    {L"dx", RDT_DX_DEFAULT},
    {L"dx.d3d9", RDT_DX_D3D9},
    {L"dx.d3d10", RDT_DX_D3D10},
    {L"dx.d3d11", RDT_DX_D3D11},
    {L"dx.d3d12", RDT_DX_D3D12},
    {L"opengl", RDT_GL_DEFAULT},
    {L"opengl.std", RDT_GL_STD},
    {L"opengl.nox", RDT_GL_NOX},
    {L"opengl.es", RDT_GL_ES},
    {L"opengl.fi", RDT_GL_FI},
};

// 名字 → 模式；未命中返回 false。
bool display_mode_from_name(const std::wstring &s, int &mode) {
    for (const auto &entry : kDisplayModes) {
        if (s == entry.name) {
            mode = entry.mode;
            return true;
        }
    }
    return false;
}

// 模式 → 名字（对外查询用，未绑定/未知模式返回空串）。
const wchar_t *display_mode_name_full(int display) {
    for (const auto &entry : kDisplayModes) {
        if (entry.mode == display)
            return entry.name;
    }
    return L"";
}

// auto 回退日志用的短名：normal 三候选之外的精确模式直接复用全名表。
const wchar_t *display_mode_name(int display) {
    const wchar_t *name = display_mode_name_full(display);
    return name[0] != L'\0' ? name : L"normal.auto";
}

} // namespace

BindingSession::BindingSession()
    : _display_hwnd(0), _input_hwnd(0), _is_bind(0), _display(0), _mode(0), _mouse_mode(INPUT_TYPE::IN_NORMAL),
      _keypad_mode(INPUT_TYPE::IN_NORMAL), _dx_attr(DX_ATTR_ALL), _dx_attr_from_suffix(false), _capture(nullptr),
      _mouse(std::make_unique<WinMouse>()), _keyboard(std::make_unique<WinKeyboard>()) {
    _display_method = std::make_pair<wstring, wstring>(L"screen", L"");
}

BindingSession::~BindingSession() {
    reset_bind_state(false);
}

long BindingSession::BindWindow(LONG_PTR hwnd, const wstring &sdisplay, const wstring &smouse, const wstring &skeypad,
                              long mode) {
    return BindWindowEx(hwnd, hwnd, sdisplay, smouse, skeypad, mode);
}

long BindingSession::BindWindowEx(LONG_PTR display_hwnd, LONG_PTR input_hwnd, const wstring &sdisplay,
                                const wstring &smouse, const wstring &skeypad, long mode) {
    // step 1.避免重复绑定
    reset_bind_state(false);

    HWND displayWnd = display_hwnd == 0 ? GetDesktopWindow() : HWND(display_hwnd);
    HWND inputWnd = input_hwnd == 0 ? displayWnd : HWND(input_hwnd);
    // step 2.check hwnd
    if (!::IsWindow(displayWnd) || !::IsWindow(inputWnd)) {
        setlog("Invalid window handles display=%p input=%p", displayWnd, inputWnd);
        ::SetLastError(BIND_FAIL_INVALID_HWND);
        return 0;
    }

    int display, mouse, keypad;
    std::vector<int> display_candidates;
    const bool auto_display = sdisplay == L"normal.auto";
    // step 3.check display... mode（模式名查表，见 kDisplayModes）
    if (auto_display) {
        display_candidates = choose_auto_displays(displayWnd);
        display = display_candidates.front();
    } else if (!display_mode_from_name(sdisplay, display)) {
        setlog(L"error display mode: %s", sdisplay.c_str());
        ::SetLastError(BIND_FAIL_DISPLAY_MODE);
        return 0;
    }
    if (!auto_display)
        display_candidates.push_back(display);
    // check mouse
    int mouse_dx_mask = 0;
    if (smouse == L"normal")
        mouse = INPUT_TYPE::IN_NORMAL;
    else if (smouse == L"windows")
        mouse = INPUT_TYPE::IN_WINDOWS;
    else if (smouse.rfind(L"dx", 0) == 0) {
        mouse = INPUT_TYPE::IN_DX;
        if (!parse_dx_channel_suffix(smouse, mouse_dx_mask)) {
            setlog(L"error mouse mode: %s", smouse.c_str());
            ::SetLastError(BIND_FAIL_MOUSE_MODE);
            return 0;
        }
    } else {
        setlog(L"error mouse mode: %s", smouse.c_str());
        ::SetLastError(BIND_FAIL_MOUSE_MODE);
        return 0;
    }
    // check keypad
    int keypad_dx_mask = 0;
    if (skeypad == L"normal")
        keypad = INPUT_TYPE::IN_NORMAL;
    else if (skeypad == L"normal.hd")
        keypad = INPUT_TYPE::IN_NORMAL2;
    else if (skeypad == L"windows")
        keypad = INPUT_TYPE::IN_WINDOWS;
    else if (skeypad.rfind(L"dx", 0) == 0) {
        keypad = INPUT_TYPE::IN_DX;
        if (!parse_dx_channel_suffix(skeypad, keypad_dx_mask)) {
            setlog(L"error keypad mode: %s", skeypad.c_str());
            ::SetLastError(BIND_FAIL_KEYPAD_MODE);
            return 0;
        }
    } else {
        setlog(L"error keypad mode: %s", skeypad.c_str());
        ::SetLastError(BIND_FAIL_KEYPAD_MODE);
        return 0;
    }

    // step 3.5 完整性预检：UIPI 会静默吞掉低完整性→高完整性的跨进程输入（windows 投递被丢、
    // dx 注入/RPC 失败但 API 照常返成功），绑定时看似成功实则一切键鼠操作无效。
    // normal 系是真输入不受 UIPI 影响，只拦依赖跨进程信任的输入模式。
    const bool cross_process_input =
        (mouse != INPUT_TYPE::IN_NORMAL && mouse != INPUT_TYPE::IN_NORMAL2) ||
        (keypad != INPUT_TYPE::IN_NORMAL && keypad != INPUT_TYPE::IN_NORMAL2);
    if (cross_process_input) {
        DWORD input_pid = 0;
        ::GetWindowThreadProcessId(inputWnd, &input_pid);
        if (input_pid != 0 && input_pid != ::GetCurrentProcessId() &&
            IsProcessIntegrityHigher(input_pid)) {
            setlog(L"BindWindow rejected: target process(pid=%d) integrity is higher than ours, "
                   L"UIPI would silently drop all cross-process input. Run the host as administrator.",
                   input_pid);
            ::SetLastError(BIND_FAIL_UIPI_INTEGRITY);
            return 0;
        }
    }
    // DXGI 只在 Windows 8 及以上开放。
    if (display == RDT_NORMAL_DXGI && !IsWindowsVersionAtLeast(6, 2, 0)) {
        setlog(L"normal.dxgi requires Windows 8 or later");
        ::SetLastError(BIND_FAIL_DXGI_OS);
        return 0;
    }

    // WGC 窗口捕获从 Windows 10 1903 开始可用。
    if (display == RDT_NORMAL_WGC && !IsWindows10BuildOrGreater(kWindows10Build1903)) {
        setlog(L"normal.wgc requires Windows 10 build 18362 or later");
        ::SetLastError(BIND_FAIL_WGC_OS);
        return 0;
    }

    // dx 通道后缀（"dx.dinput" 等）：合并 mouse/keypad 的显式通道掩码，覆盖 _dx_attr。
    // 仅在写了后缀时才覆盖；纯 "dx" 不动 SetDxAttr 的设置，但若 _dx_attr 来自上次的
    // 后缀残留则回到默认全开——避免上一次带后缀的绑定污染下一次绑定。
    const int bind_dx_mask = mouse_dx_mask | keypad_dx_mask;
    if (bind_dx_mask != 0) {
        _dx_attr = bind_dx_mask;
        _dx_attr_from_suffix = true;
    } else if (_dx_attr_from_suffix) {
        _dx_attr = DX_ATTR_ALL;
        _dx_attr_from_suffix = false;
    }

    // 输入后端与显示候选无关：创建并绑定一次即可，auto 回退不再重复跨进程注入/重建。
    // 旧的逐候选重建里，回退到第 2/3 个显示后端时会把鼠标键盘再 Bind 一遍，纯属浪费绑定延迟。
    const bool dx_input = mouse == INPUT_TYPE::IN_DX || keypad == INPUT_TYPE::IN_DX;
    _mouse = createMouse(mouse);
    _keyboard = createKeypad(keypad);
    bool backend_create_failed = !_mouse || !_keyboard;
    long display_ret = 0;
    long mouse_ret = 0;
    long keypad_ret = 0;
    if (!backend_create_failed) {
        mouse_ret = _mouse->Bind(inputWnd, mouse);
        keypad_ret = (mouse_ret == 1) ? _keyboard->Bind(inputWnd, keypad) : 0;
        // dx 输入注入成功后再把通道开关下发给远端 Hook；默认值无需多做一次跨进程调用。
        if (keypad_ret == 1 && dx_input) {
            // 钩子活性回环：注入+SetInputHook 返 1 不等于钩子真能工作（权限边界/残留态可造成
            // 静默假成功），绑定成功瞬间做一次轻量 RPC，钩子不应答则整次绑定判失败。
            if (op::hook::input_hook_client::PingHook(inputWnd) != 1) {
                setlog("input hook ping failed after bind. hwnd=%p", inputWnd);
                mouse_ret = 0;
                keypad_ret = 0;
            } else if (_dx_attr != DX_ATTR_ALL &&
                       op::hook::input_hook_client::SetInputAttr(inputWnd, _dx_attr) != 1) {
                setlog("apply dx input attr failed. hwnd=%p attr=%d", inputWnd, _dx_attr);
            }
        }
    }

    // normal.auto 逐个尝试候选显示后端；输入后端已在上方就绪，这里每轮只重建 capture。
    bool bind_ok = false;
    for (size_t i = 0; i < display_candidates.size(); ++i) {
        if (i > 0 && _capture) {
            // 只释放显示后端：输入后端跨候选复用，不再随 reset_bind_state 清空。
            _capture->UnBind();
            _capture.reset();
        }

        display = display_candidates[i];
        _mode = mode;
        _display = display;
        _display_hwnd = displayWnd;
        _input_hwnd = inputWnd;
        _mouse_mode = mouse;
        _keypad_mode = keypad;
        set_display_method(L"screen");

        _capture = createDisplay(display);
        if (!_capture) {
            setlog("create instance error!");
            backend_create_failed = true;
            continue;
        }

        display_ret = _capture->Bind(displayWnd, display);
        if (display_ret == 1 && mouse_ret == 1 && keypad_ret == 1) {
            bind_ok = true;
            break;
        }

        if (auto_display && i + 1 < display_candidates.size()) {
            setlog(L"normal.auto selected %s but bind failed, fallback to %s", display_mode_name(display),
                   display_mode_name(display_candidates[i + 1]));
        }
    }
    if (!bind_ok) {
        setlog(L"BindWindowEx failed. display_hwnd=%p input_hwnd=%p display=%s(%d) ret=%d mouse=%s(%d) ret=%d "
               L"keypad=%s(%d) ret=%d",
               displayWnd, inputWnd, sdisplay.c_str(), display, display_ret, smouse.c_str(), mouse, mouse_ret,
               skeypad.c_str(), keypad, keypad_ret);
        // 先解绑再写原因码：UnBindWindow 内部会调 Win32 API，可能覆盖 last-error。
        UnBindWindow();
        ::SetLastError(backend_create_failed ? BIND_FAIL_CREATE_BACKEND : BIND_FAIL_BACKEND_BIND);
        return 0;
    }

    _capture->waitForBindReady();

    _is_bind = 1;
    ::SetLastError(BIND_FAIL_NONE); // 成功路径清零，避免上一次失败的原因码残留
    return 1;
}

long BindingSession::UnBindWindow() {
    return reset_bind_state(true);
}

long BindingSession::LockInput(long lock) {
    if (lock < 0 || lock > 3)
        return 0;
    if (lock == 0)
        return op::hook::input_hook_client::LockInput(_input_hwnd, 0);
    if (!_is_bind || !_input_hwnd)
        return 0;

    const bool need_mouse = lock == 1 || lock == 2;
    const bool need_keyboard = lock == 1 || lock == 3;
    if ((need_mouse && _mouse_mode != INPUT_TYPE::IN_DX) || (need_keyboard && _keypad_mode != INPUT_TYPE::IN_DX))
        return 0;

    return op::hook::input_hook_client::LockInput(_input_hwnd, static_cast<int>(lock));
}

long BindingSession::DownCpu(long type, long rate) {
    if (type < 0 || type > 1)
        return 0;
    if (rate < 0)
        rate = 0;
    if (rate > 100)
        rate = 100;
    _down_cpu_rate = static_cast<int>(rate);
    return 1;
}

long BindingSession::SetDxAttr(long attr, long value) {
    int next = _dx_attr;
    if (attr == 0) {
        // attr=0：value 直接就是完整掩码。
        if (value < 0 || (value & ~DX_ATTR_ALL) != 0)
            return 0;
        next = static_cast<int>(value);
    } else {
        if (attr < 0 || (attr & ~DX_ATTR_ALL) != 0)
            return 0;
        if (value != 0)
            next |= static_cast<int>(attr);
        else
            next &= ~static_cast<int>(attr);
    }

    _dx_attr = next;
    _dx_attr_from_suffix = false; // 显式 SetDxAttr 优先于任何后缀残留语义

    // 当前没有生效中的 dx 输入绑定时只记录配置，等下次绑定成功后统一下发。
    if (!_is_bind || !_input_hwnd ||
        (_mouse_mode != INPUT_TYPE::IN_DX && _keypad_mode != INPUT_TYPE::IN_DX))
        return 1;

    return op::hook::input_hook_client::SetInputAttr(_input_hwnd, _dx_attr);
}

long BindingSession::GetDxAttr() {
    return _dx_attr;
}

long BindingSession::reset_bind_state(bool restore_default_input) {
    if (_input_hwnd && (_mouse_mode == INPUT_TYPE::IN_DX || _keypad_mode == INPUT_TYPE::IN_DX)) {
        // 解绑时主动解锁，避免脚本异常退出后目标窗口还留在锁定状态。
        op::hook::input_hook_client::LockInput(_input_hwnd, 0);
    }

    _display_hwnd = NULL;
    _input_hwnd = NULL;
    _display = 0;
    _is_bind = 0;
    _mode = 0;
    _mouse_mode = INPUT_TYPE::IN_NORMAL;
    _keypad_mode = INPUT_TYPE::IN_NORMAL;

    if (_capture) {
        _capture->UnBind();
        _capture.reset();
    }
    if (_mouse) {
        _mouse->UnBind();
        _mouse.reset();
    }
    if (_keyboard) {
        _keyboard->UnBind();
        _keyboard.reset();
    }

    // 恢复默认前台输入对象。
    if (restore_default_input) {
        _mouse = std::make_unique<WinMouse>();
        _keyboard = std::make_unique<WinKeyboard>();
    }

    return 1;
}

LONG_PTR BindingSession::GetBindWindow() {
    return reinterpret_cast<LONG_PTR>(_display_hwnd);
}

long BindingSession::IsBind() {
    return _is_bind;
}

long BindingSession::GetDisplay() {
    return _display;
}

const wchar_t *BindingSession::GetDisplayMethodName() const {
    // _display 为 0 既是"未绑定"也是 RDT_NORMAL，靠 _is_bind 区分。
    if (!_is_bind)
        return L"";
    return display_mode_name_full(_display);
}

void BindingSession::lock_data() {
    if (_capture) {
        auto p = _capture->get_mutex();
        if (p)
            p->lock();
    }
}

void BindingSession::unlock_data() {
    if (_capture) {
        auto p = _capture->get_mutex();
        if (p)
            p->unlock();
    }
}

long BindingSession::get_height() {
    const auto &method = get_display_method().first;
    if (method == L"pic" || method == L"mem") {
        return _pic.height;
    }
    return _capture ? _capture->get_height() : 0;
}

long BindingSession::get_width() {
    const auto &method = get_display_method().first;
    if (method == L"pic" || method == L"mem") {
        return _pic.width;
    }
    return _capture ? _capture->get_width() : 0;
}

long BindingSession::RectConvert(long &x1, long &y1, long &x2, long &y2) {
    // WGC 在窗口最小化/恢复后尺寸会异步变化，裁剪前先给后端一次刷新机会。
    if (_capture) {
        _capture->refreshMetrics();
    }

    x2 = std::min<long>(this->get_width(), x2);
    y2 = std::min<long>(this->get_height(), y2);
    if (x1 < 0 || y1 < 0 || x1 >= x2 || y1 >= y2) {
        setlog(L"invalid rectangle:%d %d %d %d", x1, y1, x2, y2);
        return 0;
    }
    return 1;
}

long BindingSession::get_image_type() {

    if (_display_method.first == L"pic")
        return 0;
    else if (_display_method.first == L"mem") {

        return 1;
    } else {
        switch (GET_RENDER_TYPE(_display)) {
        case RENDER_TYPE::NORMAL:
            return -2;
        case RENDER_TYPE::GDI:
            return -1;
        case RENDER_TYPE::DX:
            return 0;
        case RENDER_TYPE::OPENGL:
            return -1;
        default:
            return 0;
        }
    }
}

bool BindingSession::check_bind() {
    // 显示模式非屏幕
    if (get_display_method().first != L"screen") {
        return !_pic.empty();
    }
    // 已绑定
    if (IsBind())
        return true;

    // 绑定前台桌面
    // 自动回退：未显式绑定时区域截图/找图/取色会静默打到整个桌面，
    // 可能掩盖"忘绑定/绑定失败"。若脚本本意是操作某窗口，请检查此前 BindWindow 返回值。
    setlog(L"check_bind: no explicit bind, auto-bind desktop window (screen mode fallback)");
    return BindWindow(reinterpret_cast<LONG_PTR>(::GetDesktopWindow()), L"normal", L"normal", L"normal", 0);
}

const std::pair<wstring, wstring> &BindingSession::get_display_method() const {
    return _display_method;
}

long BindingSession::set_display_method(const wstring &method) {
    if (method == L"screen") {
        _display_method = {L"screen", L""};
        return 1;
    }

    auto idx = method.find(L"pic:");
    if (idx != wstring::npos) {
        const auto file_path = method.substr(idx + 4);
        wstring fullpath;
        if (!Path2GlobalPath(file_path, _curr_path, fullpath)) {
            return 0;
        }

        Image image;
        if (!image.read(fullpath.data())) {
            return 0;
        }

        _pic = std::move(image); // 移动接管，省一次全图深拷贝
        _display_method = {L"pic", file_path};
        return 1;
    }

    idx = method.find(L"mem:");
    if (idx != wstring::npos) {
        std::wstring mem_arg;
        Image image;
        if (!op::capture::ParseMemoryImageSource(method.substr(idx + 4), image, mem_arg)) {
            return 0;
        }

        _pic = std::move(image);
        _display_method = {L"mem", mem_arg};
        return 1;
    }

    return 0;
}

bool BindingSession::requestCapture(int x1, int y1, int w, int h, Image &img) {
    const auto &method = get_display_method().first;
    bool ok = false;
    if (method == L"screen")
        ok = _capture && _capture->requestCapture(x1, y1, w, h, img);
    else if (method == L"pic" || method == L"mem") {
        // 防御性边界校验：pic/mem 路径不走 RectConvert，直接调用者可能传入越界坐标；
        // 越界 memcpy 会读 _pic 外内存（UB/崩溃），这里 fail-loud 返回 false。
        const int pw = _pic.width;
        const int ph = _pic.height;
        if (x1 < 0 || y1 < 0 || w <= 0 || h <= 0 || x1 + w > pw || y1 + h > ph) {
            setlog(L"requestCapture out of pic bounds: x=%d y=%d w=%d h=%d pic=%dx%d", x1, y1, w, h, pw, ph);
            return false;
        }
        img.create(w, h);
        // 源子矩形宽度 w 可能小于整图宽度 pw，行与行之间不连续，必须逐行拷贝。
        for (int i = 0; i < h; i++)
            memcpy(img.ptr<uchar>(i), _pic.ptr<uchar>(i + y1) + x1 * 4, w * 4);
        ok = true;
    }
    // 降载(DownCpu):截图成功后强制延时 rate 毫秒,压低多开轮询场景 CPU 占用
    if (ok && _down_cpu_rate > 0)
        Sleep(static_cast<DWORD>(_down_cpu_rate));
    return ok;
}

std::shared_ptr<ICaptureBackend> BindingSession::createDisplay(int mode) {
    if (mode == RDT_NORMAL || GET_RENDER_TYPE(mode) == RENDER_TYPE::GDI) {
        return std::make_shared<GdiCapture>();
    } else if (mode == RDT_NORMAL_DXGI) {
        return std::make_shared<DxgiCapture>();
    }
#ifdef OP_ENABLE_WGC
    else if (mode == RDT_NORMAL_WGC) {
        return std::make_shared<WgcCapture>();
    }
#endif
    else if (GET_RENDER_TYPE(mode) == RENDER_TYPE::DX) {
        return std::make_shared<HookCapture>();
    } else if (GET_RENDER_TYPE(mode) == RENDER_TYPE::OPENGL)
        return std::make_shared<HookCapture>();
    return nullptr;
}

std::unique_ptr<WinMouse> BindingSession::createMouse(int mode) {
    if (mode == INPUT_TYPE::IN_NORMAL || mode == INPUT_TYPE::IN_WINDOWS)
        return std::make_unique<WinMouse>();
    else if (mode == INPUT_TYPE::IN_DX) {
        return std::make_unique<DxMouse>();
    }
    return nullptr;
}

std::unique_ptr<KeyboardBackend> BindingSession::createKeypad(int mode) {
    if (mode == INPUT_TYPE::IN_DX)
        return std::make_unique<DxKeyboard>();
    return std::make_unique<WinKeyboard>();
}

} // namespace op::binding
