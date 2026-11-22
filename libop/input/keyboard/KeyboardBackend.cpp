#include "KeyboardBackend.h"

#include "../InputEventBus.h"

namespace op::input {

KeyboardBackend::KeyboardBackend() : _hwnd(0), _mode(0) {
}

KeyboardBackend::~KeyboardBackend() {
    UnBind();
}

long KeyboardBackend::UnBind() {
    _hwnd = NULL;
    _mode = 0;
    return 1;
}

// ---- 时间轴层录制（批 E 2026-11-22）----
// 只在**成功**之后记（调用方负责判断）。vk 用虚拟键码而不是字符：
// 回放要重现"按了哪个键"，而不是"打出了什么字"——后者受输入法/键盘布局影响。

void KeyboardBackend::trace_key(long vk, long flags) const {
    InputEvent ev;
    ev.type = InputEventType::Key;
    ev.vk = vk;
    ev.flags = flags;
    emit_input_event(-1, std::move(ev));
}

void KeyboardBackend::trace_char(wchar_t ch) const {
    InputEvent ev;
    ev.type = InputEventType::KeyChar;
    // 字符码复用 x 字段（JSONL 里字符事件没有坐标概念，见 InputEvent.h）
    ev.x = static_cast<long>(ch);
    ev.flags = KEYEVENTF_UNICODE;
    emit_input_event(-1, std::move(ev));
}

} // namespace op::input
