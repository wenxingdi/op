// #include "stdafx.h"
#include "WinMouse.h"
#include "CursorShape.h"
#include "../InputMessageUtils.h"
#include "../InputEventBus.h"
#include "../../base/AutomationModes.h"
#include "../../base/Utils.h"
#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <cwctype>
#include <functional>
#include <sstream>
#include <vector>

namespace {
double point_distance(POINT a, POINT b) {
    const double dx = static_cast<double>(b.x - a.x);
    const double dy = static_cast<double>(b.y - a.y);
    return std::sqrt(dx * dx + dy * dy);
}

int movement_steps(POINT from, POINT to, int duration) {
    const double distance = point_distance(from, to);
    const int by_distance = static_cast<int>(std::ceil(distance / 10.0));
    const int by_duration = duration > 0 ? static_cast<int>(std::ceil(duration / 8.0)) : 0;
    int steps = by_distance;
    steps = (std::max)(steps, by_duration);
    steps = (std::max)(steps, 3);
    return std::clamp(steps, 3, 120);
}

double smoothstep(double t) {
    return t * t * (3.0 - 2.0 * t);
}

int random_offset(int value) {
    if (value == 0)
        return 0;
    const int span = value == INT_MIN ? INT_MAX : std::abs(value);
    // 正态偏置：σ = span/3，落点密集在目标中心、边缘稀疏（真人点击分布），
    // 替代旧的均匀分布（边缘与中心等概率是机器特征）。
    const double n = std::fabs(rand_normal());
    int offset = static_cast<int>(std::lround(n * (span / 3.0)));
    offset = (std::clamp)(offset, 0, span - 1);
    return value > 0 ? offset : -offset;
}

// 速度曲线族：真人每次移动的速度轮廓不完全一样，单一缓动函数本身就是统计特征。
// 三种都是"S 型起步-滑行-减速"家族，但加速段/减速段的比例不同。
double apply_easing(int kind, double t) {
    switch (kind) {
    case 1: // quintic smootherstep：两端更平缓，中段更果断
        return t * t * t * (t * (t * 6.0 - 15.0) + 10.0);
    case 2: // 正弦 in-out：整段更柔
        return 0.5 - 0.5 * std::cos(3.14159265358979323846 * t);
    case 0:
    default: // 经典 smoothstep
        return smoothstep(t);
    }
}

// 真人双击间隔约 80~150ms；复用单击 down/up 延时（30ms 级）是典型机器特征。
long human_double_click_gap_ms() {
    return rand_range(80, 150);
}

// 末端过冲参数：距离够远时 35% 概率落点偏出 2~5px 后收回（真人手臂运动的固有特征）。
constexpr int kOvershootMinDistance = 40;
constexpr int kOvershotTriggerPercent = 35;
constexpr int kOvershootMinPx = 2;
constexpr int kOvershootMaxPx = 5;

POINT bezier_point(POINT p0, POINT p1, POINT p2, POINT p3, double t) {
    const double u = 1.0 - t;
    const double uu = u * u;
    const double tt = t * t;
    const double x = uu * u * p0.x + 3.0 * uu * t * p1.x + 3.0 * u * tt * p2.x + tt * t * p3.x;
    const double y = uu * u * p0.y + 3.0 * uu * t * p1.y + 3.0 * u * tt * p2.y + tt * t * p3.y;
    return POINT{static_cast<LONG>(std::lround(x)), static_cast<LONG>(std::lround(y))};
}

std::vector<POINT> build_linear_path(POINT from, POINT to, int duration) {
    if (point_distance(from, to) < 1.0)
        return {to};

    const int easing_kind = rand_range(0, 2);
    std::vector<POINT> path;
    const int steps = movement_steps(from, to, duration);
    path.reserve(static_cast<size_t>(steps));
    POINT last{LONG_MIN, LONG_MIN};
    for (int i = 1; i <= steps; ++i) {
        const double t = apply_easing(easing_kind, static_cast<double>(i) / static_cast<double>(steps));
        POINT pt{static_cast<LONG>(std::lround(from.x + (to.x - from.x) * t)),
                 static_cast<LONG>(std::lround(from.y + (to.y - from.y) * t))};
        if (pt.x != last.x || pt.y != last.y) {
            path.push_back(pt);
            last = pt;
        }
    }
    if (path.empty() || path.back().x != to.x || path.back().y != to.y)
        path.push_back(to);
    return path;
}

std::vector<POINT> build_smooth_path(POINT from, POINT to, int duration, int jitter) {
    const double distance = point_distance(from, to);
    if (distance < 1.0)
        return {to};

    const double dx = static_cast<double>(to.x - from.x);
    const double dy = static_cast<double>(to.y - from.y);
    const double nx = -dy / distance;
    const double ny = dx / distance;
    const double raw_bend = distance * static_cast<double>(jitter) / 100.0;
    const double bend = raw_bend > 0.0 ? std::clamp(raw_bend, 3.0, 80.0) : 0.0;
    // 两个控制点同向弯曲（同一符号、0.5~1.0 倍幅值）：真人一次移动基本朝一侧弧线，
    // 反向弯曲会互相抵消拉成近直线，且给测试一个"必弯"的下界。
    const double sign = rand_unit_signed() < 0.0 ? -1.0 : 1.0;
    const double m1 = 0.5 + 0.5 * std::fabs(rand_unit_signed());
    const double m2 = 0.5 + 0.5 * std::fabs(rand_unit_signed());
    const double bend1 = sign * bend * m1;
    const double bend2 = sign * bend * m2;

    // 控制点只做轻微偏移，保留方向感，同时避免完全笔直的机械轨迹。
    POINT c1{static_cast<LONG>(std::lround(from.x + dx * 0.35 + nx * bend1)),
             static_cast<LONG>(std::lround(from.y + dy * 0.35 + ny * bend1))};
    POINT c2{static_cast<LONG>(std::lround(from.x + dx * 0.70 + nx * bend2)),
             static_cast<LONG>(std::lround(from.y + dy * 0.70 + ny * bend2))};

    const int easing_kind = rand_range(0, 2);
    std::vector<POINT> path;
    const int steps = movement_steps(from, to, duration);
    path.reserve(static_cast<size_t>(steps));
    POINT last{LONG_MIN, LONG_MIN};
    for (int i = 1; i <= steps; ++i) {
        const double t = apply_easing(easing_kind, static_cast<double>(i) / static_cast<double>(steps));
        POINT pt = bezier_point(from, c1, c2, to, t);
        if (pt.x != last.x || pt.y != last.y) {
            path.push_back(pt);
            last = pt;
        }
    }
    if (path.empty() || path.back().x != to.x || path.back().y != to.y)
        path.push_back(to);
    return path;
}

std::vector<POINT> build_mouse_path(POINT from, POINT to, int duration, int mode, int jitter) {
    if (mode == 1)
        return build_linear_path(from, to, duration);
    return build_smooth_path(from, to, duration, jitter);
}

bool parse_path_point(const std::wstring &text, POINT &pt) {
    std::wistringstream stream(text);
    long x = 0;
    long y = 0;
    wchar_t comma = 0;
    if (!(stream >> x >> comma >> y) || comma != L',')
        return false;
    while (stream && std::iswspace(stream.peek()))
        stream.get();
    if (!stream.eof())
        return false;
    pt = POINT{static_cast<LONG>(x), static_cast<LONG>(y)};
    return true;
}

bool parse_mouse_path(const std::wstring &path_text, std::vector<POINT> &path) {
    path.clear();
    size_t start = 0;
    while (start <= path_text.size()) {
        const size_t end = path_text.find(L'|', start);
        const std::wstring item = path_text.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start);
        POINT pt{};
        if (!parse_path_point(item, pt))
            return false;
        path.push_back(pt);
        if (end == std::wstring::npos)
            break;
        start = end + 1;
    }
    return path.size() >= 2;
}

std::vector<POINT> expand_mouse_path(const std::vector<POINT> &key_points) {
    if (key_points.empty())
        return {};

    std::vector<POINT> path;
    path.push_back(key_points.front());
    for (size_t i = 1; i < key_points.size(); ++i) {
        const POINT from = key_points[i - 1];
        const POINT to = key_points[i];
        const int steps = std::clamp(static_cast<int>(std::ceil(point_distance(from, to) / 10.0)), 1, 60);
        POINT last = path.back();
        for (int step = 1; step <= steps; ++step) {
            const double t = static_cast<double>(step) / static_cast<double>(steps);
            POINT pt{static_cast<LONG>(std::lround(from.x + (to.x - from.x) * t)),
                     static_cast<LONG>(std::lround(from.y + (to.y - from.y) * t))};
            if (pt.x != last.x || pt.y != last.y) {
                path.push_back(pt);
                last = pt;
            }
        }
    }
    return path;
}

long run_mouse_path(const std::vector<POINT> &path, int duration,
                    const std::function<long(POINT)> &move_to_point) {
    if (path.empty() || duration < 0)
        return 0;

    const int delay = duration > 0 && path.size() > 1 ? duration / static_cast<int>(path.size() - 1) : 0;
    for (size_t i = 0; i < path.size(); ++i) {
        if (!move_to_point(path[i]))
            return 0;
        if (delay > 0 && i + 1 < path.size()) {
            // 时间轴拟真：每步 ±30% 抖动 + 5% 概率微停 20~60ms。
            // 等步长是机器移动的最强统计特征，真人速度曲线必有波动与偶发停顿。
            int step_delay = jittered_delay_ms(delay, 30);
            if (rand_range(0, 99) < 5)
                step_delay += rand_range(20, 60);
            ::Delay(step_delay);
        }
    }
    return 1;
}

void delay_if_needed(int delay) {
    if (delay > 0)
        ::Delay(delay);
}

// 窗口消息 -> SendInput 鼠标标志（时间轴层录制的 flags 统一口径）。
// IN_WINDOWS / DX 后端发的是窗口消息，IN_NORMAL 发的是 INPUT 标志；录制文件
// 必须两者同构，否则同一段操作换个绑定模式就录出两种 flags，回放端要做两套分支。
//
// 双击消息（WM_*DBLCLK）**映射到对应的 down**：回放时重发一次 DBLCLK 会让目标
// 收到"系统判定之外的双击"，行为与录制时不一致；录制里的 down/up/down/up 序列
// 天然就会重新触发双击。
long win_message_button_flag(UINT message) {
    switch (message) {
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
        return MOUSEEVENTF_LEFTDOWN;
    case WM_LBUTTONUP:
        return MOUSEEVENTF_LEFTUP;
    case WM_RBUTTONDOWN:
    case WM_RBUTTONDBLCLK:
        return MOUSEEVENTF_RIGHTDOWN;
    case WM_RBUTTONUP:
        return MOUSEEVENTF_RIGHTUP;
    case WM_MBUTTONDOWN:
    case WM_MBUTTONDBLCLK:
        return MOUSEEVENTF_MIDDLEDOWN;
    case WM_MBUTTONUP:
        return MOUSEEVENTF_MIDDLEUP;
    default:
        return 0;
    }
}
}

namespace op::input {

WinMouse::WinMouse()
    : _hwnd(NULL), _mode(0), _x(0), _y(0), _button_state(0), _trajectory_mode(0),
      _trajectory_min_duration(0), _trajectory_max_duration(0), _trajectory_jitter(18),
      _trajectory_start_delay(0), _trajectory_end_delay(0) {
}

WinMouse::~WinMouse() {
    _hwnd = NULL;
}

long WinMouse::Bind(HWND h, int mode) {
    // 与 WinKeyboard::Bind 对齐：拒绝空/失效句柄，防 _hwnd=NULL 时
    // IN_WINDOWS 消息路径 SendMessageTimeout(NULL) 广播全部顶层窗口。
    if (!::IsWindow(h))
        return 0;
    _hwnd = h;
    _mode = mode;
    _x = _y = 0;
    _button_state = 0;
    return 1;
}

long WinMouse::UnBind() {
    _hwnd = 0;
    _mode = 0;
    _x = _y = 0;
    _button_state = 0;
    return 1;
}

long WinMouse::GetCursorPos(long &x, long &y) {
    POINT pt = {0, 0}; // 失败时输出 (0,0)，避免未初始化垃圾坐标
    BOOL ret = ::GetCursorPos(&pt);
    if (_hwnd && _hwnd != ::GetDesktopWindow()) {
        ret = ::ScreenToClient(_hwnd, &pt);
    }
    x = pt.x;
    y = pt.y;
    return ret;
}

long WinMouse::GetCursorShape(std::wstring &ret) {
    CursorShapeInfo info;
    if (!cursor_shape::FromSystem(info)) {
        ret.clear();
        return 0;
    }

    ret = cursor_shape::Format(info);
    return 1;
}

long WinMouse::MoveR(int rx, int ry) {
    switch (_mode) {
    case INPUT_TYPE::IN_NORMAL: {
        // https://learn.microsoft.com/zh-cn/windows/win32/api/winuser/ns-winuser-mouseinput
        _x += rx;
        _y += ry;

        INPUT Input = {0};
        Input.type = INPUT_MOUSE;
        Input.mi.dwFlags = MOUSEEVENTF_MOVE;
        Input.mi.dx = static_cast<LONG>(rx);
        Input.mi.dy = static_cast<LONG>(ry);
        const long ok = ::SendInput(1, &Input, sizeof(INPUT)) > 0 ? 1 : 0;
        // 录制记**移动后的绝对客户区坐标**：相对位移在回放端毫无意义
        //（回放时的起点未必是录制时的起点，累加误差会越放越偏）。
        if (ok)
            trace_move(_x, _y);
        return ok;
    }
    }
    return MoveTo(_x + rx, _y + ry);
}

long WinMouse::MoveTo(int x, int y) {
    long ret = 0;
    POINT client_pt{x, y};
    switch (_mode) {
    case INPUT_TYPE::IN_NORMAL: {
        POINT pt = client_pt;
        if (_hwnd)
            ::ClientToScreen(_hwnd, &pt);

        const int screen_x = ::GetSystemMetrics(SM_XVIRTUALSCREEN);
        const int screen_y = ::GetSystemMetrics(SM_YVIRTUALSCREEN);
        const int screen_width = ::GetSystemMetrics(SM_CXVIRTUALSCREEN);
        const int screen_height = ::GetSystemMetrics(SM_CYVIRTUALSCREEN);
        const double width = screen_width > 1 ? static_cast<double>(screen_width - 1) : 1.0;
        const double height = screen_height > 1 ? static_cast<double>(screen_height - 1) : 1.0;
        const double fx = (pt.x - screen_x) * (65535.0 / width);
        const double fy = (pt.y - screen_y) * (65535.0 / height);

        INPUT Input = {0};
        Input.type = INPUT_MOUSE;
        Input.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
        Input.mi.dx = static_cast<LONG>(fx);
        Input.mi.dy = static_cast<LONG>(fy);
        ret = ::SendInput(1, &Input, sizeof(INPUT)) > 0 ? 1 : 0;
        break;
    }
    case INPUT_TYPE::IN_WINDOWS: {
        ret = message::SendTimeout(_hwnd, WM_MOUSEMOVE, button_state(), MAKELPARAM(client_pt.x, client_pt.y));
        break;
    }
    }
    _x = client_pt.x, _y = client_pt.y;
    if (ret)
        trace_move(_x, _y);
    return ret;
}

POINT WinMouse::current_client_point() const {
    return POINT{_x, _y};
}

long WinMouse::sync_system_cursor() {
    POINT pt = current_client_point();
    if (_hwnd)
        ::ClientToScreen(_hwnd, &pt);
    return ::SetCursorPos(pt.x, pt.y) ? 1 : 0;
}

WPARAM WinMouse::button_state() const {
    return _button_state;
}

WPARAM WinMouse::button_state_with(WPARAM button, bool down) const {
    return down ? (_button_state | button) : (_button_state & ~button);
}

void WinMouse::set_button_state(WPARAM button, bool down) {
    _button_state = button_state_with(button, down);
}

// ---- 时间轴层录制（批 E 2026-11-22）----
// 事件只在**成功**之后记：录一条"按下失败"的 down 会让回放把弹不起来的键按下去。
// x/y 一律取**目标客户区坐标**（不是屏幕坐标、不是移动后的实际落点）：
// 回落放时要落在同一个绑定窗口的同一个位置，屏幕坐标跨窗口就错了。

void WinMouse::trace_move(int x, int y) const {
    op::input::InputEvent ev;
    ev.type = op::input::InputEventType::MouseMove;
    ev.x = x;
    ev.y = y;
    op::input::emit_input_event(-1, std::move(ev));
}

void WinMouse::trace_button(long flags, long aux) const {
    op::input::InputEvent ev;
    ev.type = op::input::InputEventType::MouseButton;
    ev.flags = flags;
    // XBUTTON1/2 借用 wheel 字段传递（JSONL 里 wheel 对按键事件无意义）
    ev.wheel = aux;
    const POINT pt = current_client_point();
    ev.x = pt.x;
    ev.y = pt.y;
    op::input::emit_input_event(-1, std::move(ev));
}

void WinMouse::trace_wheel(long flags, int delta) const {
    op::input::InputEvent ev;
    ev.type = op::input::InputEventType::MouseWheel;
    ev.flags = flags;
    ev.wheel = delta;
    const POINT pt = current_client_point();
    ev.x = pt.x;
    ev.y = pt.y;
    op::input::emit_input_event(-1, std::move(ev));
}

long WinMouse::send_input_mouse(DWORD flags, DWORD mouse_data, bool sync_cursor) {
    if (sync_cursor && !sync_system_cursor())
        return 0;

    INPUT input = {};
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = flags;
    input.mi.mouseData = mouse_data;
    return ::SendInput(1, &input, sizeof(INPUT)) > 0 ? 1 : 0;
}

long WinMouse::send_input_click(DWORD down_flags, DWORD up_flags, DWORD mouse_data, long delay) {
    if (!sync_system_cursor())
        return 0;

    const long r1 = send_input_mouse(down_flags, mouse_data, false);
    DelayJitter(delay);
    const long r2 = send_input_mouse(up_flags, mouse_data, false);
    return r1 && r2 ? 1 : 0;
}

long WinMouse::send_windows_button(UINT message, WPARAM button, bool down) {
    const POINT pt = current_client_point();
    const WPARAM state = button_state_with(button, down);
    const long ret = message::SendTimeout(_hwnd, message, state, MAKELPARAM(pt.x, pt.y));
    if (ret) {
        set_button_state(button, down);
        trace_button(win_message_button_flag(message), 0);
    }
    return ret;
}

long WinMouse::send_windows_xbutton(UINT message, WORD xbutton, WPARAM button, bool down) {
    const POINT pt = current_client_point();
    const WPARAM state = button_state_with(button, down);
    const long ret =
        message::SendTimeout(_hwnd, message, MAKEWPARAM(static_cast<WORD>(state), xbutton), MAKELPARAM(pt.x, pt.y));
    if (ret) {
        set_button_state(button, down);
        trace_button(down ? MOUSEEVENTF_XDOWN : MOUSEEVENTF_XUP, xbutton);
    }
    return ret;
}

long WinMouse::button_click(long (WinMouse::*down)(), long (WinMouse::*up)(), long delay) {
    const long r1 = (this->*down)();
    DelayJitter(delay);
    const long r2 = (this->*up)();
    return r1 && r2 ? 1 : 0;
}

long WinMouse::normal_double_click(long (WinMouse::*click)(), long /*delay*/) {
    const long r1 = (this->*click)();
    // 双击间隔用拟人区间（80~150ms），不复用单击 down/up 延时——30ms 级连击是机器特征。
    ::Delay(human_double_click_gap_ms());
    const long r2 = (this->*click)();
    return r1 && r2 ? 1 : 0;
}

long WinMouse::button_double_click(long (WinMouse::*click)(), UINT message, UINT up_message, WPARAM button,
                                   long /*delay*/) {
    const long r1 = (this->*click)();
    ::Delay(human_double_click_gap_ms());
    const POINT pt = current_client_point();
    const WPARAM state = button_state_with(button, true);
    const long r2 = message::SendTimeout(_hwnd, message, state, MAKELPARAM(pt.x, pt.y));
    if (r2)
        set_button_state(button, true);
    ::Delay(human_double_click_gap_ms());
    const long r3 = send_windows_button(up_message, button, false);
    return r1 && r2 && r3 ? 1 : 0;
}

long WinMouse::xbutton(WORD xbutton_id, WPARAM button, bool down) {
    switch (_mode) {
    case INPUT_TYPE::IN_NORMAL: {
        const long ret = send_input_mouse(down ? MOUSEEVENTF_XDOWN : MOUSEEVENTF_XUP, xbutton_id);
        if (ret)
            trace_button(down ? MOUSEEVENTF_XDOWN : MOUSEEVENTF_XUP, xbutton_id);
        return ret;
    }
    case INPUT_TYPE::IN_WINDOWS:
        return send_windows_xbutton(down ? WM_XBUTTONDOWN : WM_XBUTTONUP, xbutton_id, button, down);
    }
    return 0;
}

long WinMouse::xbutton_double_click(long (WinMouse::*click)(), WORD xbutton_id, WPARAM button, long /*delay*/) {
    const long r1 = (this->*click)();
    ::Delay(human_double_click_gap_ms());
    const long r2 = send_windows_xbutton(WM_XBUTTONDBLCLK, xbutton_id, button, true);
    ::Delay(human_double_click_gap_ms());
    const long r3 = send_windows_xbutton(WM_XBUTTONUP, xbutton_id, button, false);
    return r1 && r2 && r3 ? 1 : 0;
}

long WinMouse::send_wheel(DWORD input_flag, UINT window_message, int delta) {
    switch (_mode) {
    case INPUT_TYPE::IN_NORMAL: {
        const long ret = send_input_mouse(input_flag, static_cast<DWORD>(delta));
        if (ret)
            trace_wheel(input_flag, delta);
        return ret;
    }
    case INPUT_TYPE::IN_WINDOWS: {
        POINT pt = current_client_point();
        if (_hwnd)
            ::ClientToScreen(_hwnd, &pt);
        const long ret = message::SendTimeout(_hwnd, window_message,
                                              MAKEWPARAM(static_cast<WORD>(button_state()), static_cast<WORD>(delta)),
                                              MAKELPARAM(pt.x, pt.y));
        if (ret)
            trace_wheel(input_flag, delta);
        return ret;
    }
    }
    return 0;
}

long WinMouse::MoveToEx(int x, int y, int w, int h, int &dst_x, int &dst_y) {
    dst_x = x + random_offset(w);
    dst_y = y + random_offset(h);
    return MoveTo(dst_x, dst_y);
}

int WinMouse::resolve_trajectory_duration(int duration) const {
    if (duration < 0)
        return -1;

    int resolved = duration > 0 ? duration : _trajectory_min_duration;
    resolved = (std::max)(resolved, _trajectory_min_duration);
    if (_trajectory_max_duration > 0)
        resolved = (std::min)(resolved, _trajectory_max_duration);
    return resolved;
}

long WinMouse::MoveToSmooth(int x, int y, int duration) {
    const int resolved_duration = resolve_trajectory_duration(duration);
    if (resolved_duration < 0)
        return 0;

    POINT from = current_client_point();
    if (_mode == INPUT_TYPE::IN_NORMAL) {
        long current_x = 0;
        long current_y = 0;
        if (GetCursorPos(current_x, current_y)) {
            from.x = static_cast<LONG>(current_x);
            from.y = static_cast<LONG>(current_y);
        }
    }

    const POINT to{x, y};
    const std::vector<POINT> path =
        build_mouse_path(from, to, resolved_duration, _trajectory_mode, _trajectory_jitter);
    delay_if_needed(_trajectory_start_delay);
    const long ret = run_mouse_path(path, resolved_duration, [this](POINT pt) { return MoveTo(pt.x, pt.y); });
    if (ret) {
        // 末端过冲：落点偏出 2~5px 后收回，真人手臂运动的固有特征。
        // jitter=0 或线性模式视为"明确要笔直/确定性轨迹"，不叠加随机量
        // （MouseKeyTest.MouseTrajectoryJitterBendsPath 以 jitter=0 笔直为不变量）。
        if (_trajectory_jitter > 0 && _trajectory_mode != 1 && point_distance(from, to) >= kOvershootMinDistance &&
            rand_range(0, 99) < kOvershotTriggerPercent) {
            const int ox = rand_range(kOvershootMinPx, kOvershootMaxPx) * (rand_unit_signed() < 0.0 ? -1 : 1);
            const int oy = rand_range(kOvershootMinPx, kOvershootMaxPx) * (rand_unit_signed() < 0.0 ? -1 : 1);
            MoveTo(to.x + ox, to.y + oy);
            ::Delay(rand_range(15, 40));
            MoveTo(to.x, to.y);
        }
        delay_if_needed(_trajectory_end_delay);
    }
    return ret;
}

long WinMouse::MoveToExSmooth(int x, int y, int w, int h, int duration, int &dst_x, int &dst_y) {
    dst_x = x + random_offset(w);
    dst_y = y + random_offset(h);
    return MoveToSmooth(dst_x, dst_y, duration);
}

long WinMouse::MovePath(const std::wstring &path_text, int duration) {
    std::vector<POINT> key_points;
    const int resolved_duration = resolve_trajectory_duration(duration);
    if (resolved_duration < 0 || !parse_mouse_path(path_text, key_points))
        return 0;

    // MovePath 接收的是关键点，段内补点后再发送，避免长距离两点之间直接跳过去。
    const std::vector<POINT> path = expand_mouse_path(key_points);
    if (path.size() < 2)
        return 0;
    delay_if_needed(_trajectory_start_delay);
    const long ret = run_mouse_path(path, resolved_duration, [this](POINT pt) { return MoveTo(pt.x, pt.y); });
    if (ret)
        delay_if_needed(_trajectory_end_delay);
    return ret;
}

long WinMouse::DragPath(const std::wstring &path_text, int duration) {
    std::vector<POINT> key_points;
    const int resolved_duration = resolve_trajectory_duration(duration);
    if (resolved_duration < 0 || !parse_mouse_path(path_text, key_points))
        return 0;

    if (key_points.size() < 2)
        return 0;
    if (!MoveTo(key_points.front().x, key_points.front().y))
        return 0;

    const long down_ret = LeftDown();
    if (!down_ret)
        return 0;

    delay_if_needed(_trajectory_start_delay);
    // 段内走拟人轨迹（贝塞尔 + 速度曲线族），不再线性硬拉——直线拖拽是
    // 行为检测最好抓的特征之一。时长按段均摊，多段拖拽每段都有完整加减速。
    long move_ret = 1;
    const int per_segment = static_cast<int>(key_points.size()) > 2
                                ? resolved_duration / (static_cast<int>(key_points.size()) - 1)
                                : resolved_duration;
    for (size_t i = 1; i < key_points.size() && move_ret; ++i) {
        const std::vector<POINT> segment =
            build_mouse_path(key_points[i - 1], key_points[i], per_segment, _trajectory_mode, _trajectory_jitter);
        move_ret = run_mouse_path(segment, per_segment, [this](POINT pt) { return MoveTo(pt.x, pt.y); });
    }
    // 拖拽到终点后先停一下再松手，更接近按住后确认落点的操作习惯。
    delay_if_needed(_trajectory_end_delay);
    const long up_ret = LeftUp();
    return move_ret && up_ret ? 1 : 0;
}

long WinMouse::SetMouseTrajectory(int mode, int min_duration, int max_duration, int jitter, int start_delay,
                                  int end_delay) {
    if (mode < 0 || mode > 2 || min_duration < 0 || max_duration < 0 || jitter < 0 || jitter > 100 ||
        start_delay < 0 || end_delay < 0)
        return 0;
    if (max_duration > 0 && min_duration > max_duration)
        return 0;

    // max_duration 为 0 表示不封顶，旧脚本传入的 duration 会按原样生效。
    _trajectory_mode = mode;
    _trajectory_min_duration = min_duration;
    _trajectory_max_duration = max_duration;
    _trajectory_jitter = jitter;
    _trajectory_start_delay = start_delay;
    _trajectory_end_delay = end_delay;
    return 1;
}

long WinMouse::LeftClick() {
    switch (_mode) {
    case INPUT_TYPE::IN_NORMAL: {
        return send_input_click(MOUSEEVENTF_LEFTDOWN, MOUSEEVENTF_LEFTUP, 0, MOUSE_NORMAL_DELAY);
    }

    case INPUT_TYPE::IN_WINDOWS: {
        return button_click(&WinMouse::LeftDown, &WinMouse::LeftUp, MOUSE_WINDOWS_DELAY);
    }
    }
    return 0;
}

long WinMouse::LeftDoubleClick() {
    switch (_mode) {
    case INPUT_TYPE::IN_NORMAL: {
        return normal_double_click(&WinMouse::LeftClick, MOUSE_NORMAL_DELAY);
    }
    case INPUT_TYPE::IN_WINDOWS: {
        return button_double_click(&WinMouse::LeftClick, WM_LBUTTONDBLCLK, WM_LBUTTONUP, MK_LBUTTON,
                                   MOUSE_WINDOWS_DELAY);
    }
    }
    return 0;
}

long WinMouse::LeftDown() {
    long ret = 0;
    switch (_mode) {
    case INPUT_TYPE::IN_NORMAL: {
        ret = send_input_mouse(MOUSEEVENTF_LEFTDOWN);
        if (ret)
            trace_button(MOUSEEVENTF_LEFTDOWN);
        break;
    }

    case INPUT_TYPE::IN_WINDOWS: {
        ret = send_windows_button(WM_LBUTTONDOWN, MK_LBUTTON, true);
        break;
    }
    }
    return ret;
}

long WinMouse::LeftUp() {
    long ret = 0;
    switch (_mode) {
    case INPUT_TYPE::IN_NORMAL: {
        ret = send_input_mouse(MOUSEEVENTF_LEFTUP);
        if (ret)
            trace_button(MOUSEEVENTF_LEFTUP);
        break;
    }

    case INPUT_TYPE::IN_WINDOWS: {
        ret = send_windows_button(WM_LBUTTONUP, MK_LBUTTON, false);
        break;
    }
    }
    return ret;
}

long WinMouse::MiddleClick() {
    switch (_mode) {
    case INPUT_TYPE::IN_NORMAL:
        return send_input_click(MOUSEEVENTF_MIDDLEDOWN, MOUSEEVENTF_MIDDLEUP, 0, MOUSE_NORMAL_DELAY);
    case INPUT_TYPE::IN_WINDOWS:
        return button_click(&WinMouse::MiddleDown, &WinMouse::MiddleUp, MOUSE_WINDOWS_DELAY);
    }
    return 0;
}

long WinMouse::MiddleDoubleClick() {
    switch (_mode) {
    case INPUT_TYPE::IN_NORMAL: {
        return normal_double_click(&WinMouse::MiddleClick, MOUSE_NORMAL_DELAY);
    }
    case INPUT_TYPE::IN_WINDOWS:
        return button_double_click(&WinMouse::MiddleClick, WM_MBUTTONDBLCLK, WM_MBUTTONUP, MK_MBUTTON,
                                   MOUSE_WINDOWS_DELAY);
    }
    return 0;
}

long WinMouse::MiddleDown() {
    long ret = 0;
    switch (_mode) {
    case INPUT_TYPE::IN_NORMAL: {
        ret = send_input_mouse(MOUSEEVENTF_MIDDLEDOWN);
        if (ret)
            trace_button(MOUSEEVENTF_MIDDLEDOWN);
        break;
    }

    case INPUT_TYPE::IN_WINDOWS: {
        ret = send_windows_button(WM_MBUTTONDOWN, MK_MBUTTON, true);
        break;
    }
    }
    return ret;
}

long WinMouse::MiddleUp() {
    long ret = 0;
    switch (_mode) {
    case INPUT_TYPE::IN_NORMAL: {
        ret = send_input_mouse(MOUSEEVENTF_MIDDLEUP);
        if (ret)
            trace_button(MOUSEEVENTF_MIDDLEUP);
        break;
    }

    case INPUT_TYPE::IN_WINDOWS: {
        ret = send_windows_button(WM_MBUTTONUP, MK_MBUTTON, false);
        break;
    }
    }
    return ret;
}

long WinMouse::RightClick() {
    switch (_mode) {
    case INPUT_TYPE::IN_NORMAL: {
        return send_input_click(MOUSEEVENTF_RIGHTDOWN, MOUSEEVENTF_RIGHTUP, 0, MOUSE_NORMAL_DELAY);
    }

    case INPUT_TYPE::IN_WINDOWS: {
        return button_click(&WinMouse::RightDown, &WinMouse::RightUp, MOUSE_WINDOWS_DELAY);
    }
    }
    return 0;
}

long WinMouse::RightDoubleClick() {
    switch (_mode) {
    case INPUT_TYPE::IN_NORMAL: {
        return normal_double_click(&WinMouse::RightClick, MOUSE_NORMAL_DELAY);
    }
    case INPUT_TYPE::IN_WINDOWS:
        return button_double_click(&WinMouse::RightClick, WM_RBUTTONDBLCLK, WM_RBUTTONUP, MK_RBUTTON,
                                   MOUSE_WINDOWS_DELAY);
    }
    return 0;
}

long WinMouse::RightDown() {
    long ret = 0;
    switch (_mode) {
    case INPUT_TYPE::IN_NORMAL: {
        ret = send_input_mouse(MOUSEEVENTF_RIGHTDOWN);
        if (ret)
            trace_button(MOUSEEVENTF_RIGHTDOWN);
        break;
    }
    case INPUT_TYPE::IN_WINDOWS: {
        ret = send_windows_button(WM_RBUTTONDOWN, MK_RBUTTON, true);
        break;
    }
    }
    return ret;
}

long WinMouse::RightUp() {
    long ret = 0;
    switch (_mode) {
    case INPUT_TYPE::IN_NORMAL: {
        ret = send_input_mouse(MOUSEEVENTF_RIGHTUP);
        if (ret)
            trace_button(MOUSEEVENTF_RIGHTUP);
        break;
    }

    case INPUT_TYPE::IN_WINDOWS: {
        ret = send_windows_button(WM_RBUTTONUP, MK_RBUTTON, false);
        break;
    }
    }
    return ret;
}

long WinMouse::XButton1Click() {
    switch (_mode) {
    case INPUT_TYPE::IN_NORMAL:
        return send_input_click(MOUSEEVENTF_XDOWN, MOUSEEVENTF_XUP, XBUTTON1, MOUSE_NORMAL_DELAY);
    case INPUT_TYPE::IN_WINDOWS:
        return button_click(&WinMouse::XButton1Down, &WinMouse::XButton1Up, MOUSE_WINDOWS_DELAY);
    }
    return 0;
}

long WinMouse::XButton1DoubleClick() {
    switch (_mode) {
    case INPUT_TYPE::IN_NORMAL: {
        return normal_double_click(&WinMouse::XButton1Click, MOUSE_NORMAL_DELAY);
    }
    case INPUT_TYPE::IN_WINDOWS:
        return xbutton_double_click(&WinMouse::XButton1Click, XBUTTON1, MK_XBUTTON1, MOUSE_WINDOWS_DELAY);
    }
    return 0;
}

long WinMouse::XButton1Down() {
    return xbutton(XBUTTON1, MK_XBUTTON1, true);
}

long WinMouse::XButton1Up() {
    return xbutton(XBUTTON1, MK_XBUTTON1, false);
}

long WinMouse::XButton2Click() {
    switch (_mode) {
    case INPUT_TYPE::IN_NORMAL:
        return send_input_click(MOUSEEVENTF_XDOWN, MOUSEEVENTF_XUP, XBUTTON2, MOUSE_NORMAL_DELAY);
    case INPUT_TYPE::IN_WINDOWS:
        return button_click(&WinMouse::XButton2Down, &WinMouse::XButton2Up, MOUSE_WINDOWS_DELAY);
    }
    return 0;
}

long WinMouse::XButton2DoubleClick() {
    switch (_mode) {
    case INPUT_TYPE::IN_NORMAL: {
        return normal_double_click(&WinMouse::XButton2Click, MOUSE_NORMAL_DELAY);
    }
    case INPUT_TYPE::IN_WINDOWS:
        return xbutton_double_click(&WinMouse::XButton2Click, XBUTTON2, MK_XBUTTON2, MOUSE_WINDOWS_DELAY);
    }
    return 0;
}

long WinMouse::XButton2Down() {
    return xbutton(XBUTTON2, MK_XBUTTON2, true);
}

long WinMouse::XButton2Up() {
    return xbutton(XBUTTON2, MK_XBUTTON2, false);
}

long WinMouse::Wheel(int delta) {
    return send_wheel(MOUSEEVENTF_WHEEL, WM_MOUSEWHEEL, delta);
}

long WinMouse::HWheel(int delta) {
    return send_wheel(MOUSEEVENTF_HWHEEL, WM_MOUSEHWHEEL, delta);
}

long WinMouse::WheelDown() {
    return Wheel(-WHEEL_DELTA);
}

long WinMouse::WheelUp() {
    return Wheel(WHEEL_DELTA);
}

} // namespace op::input
