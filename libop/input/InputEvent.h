#pragma once
#ifndef OP_INPUT_INPUT_EVENT_H_
#define OP_INPUT_INPUT_EVENT_H_

// 时间轴层的事件记录（批 E 2026-11-22）。
//
// 落盘格式是 **JSONL**（一行一个事件），字段与规划 §6 一致：
//   {"t":123.4,"type":"mouse_move","vk":0,"x":300,"y":200,"wheel":0,"flags":0}
//
// 设计取舍（为什么不用"每个 API 一个事件类型"）：
//   鼠标移动是高频事件（MoveToSmooth 一次能产生上百个 MoveTo），用独立的
//   mouse_move 类型可以让回放端**合并/抽稀**；而按键/滚轮是离散事件，
//   必须原样重放（少一个 down 就是点了个寂寞）。
//
// flags 的语义按事件类型分（与 Win32 的 MOUSEEVENTF_* 数值对齐，便于核对）：
//   mouse_move      0（纯移动，不带按键状态）
//   mouse_button    MOUSEEVENTF_LEFTDOWN/LEFTUP/RIGHT.../XDOWN/XUP（0x0002/0x0004/…）
//   mouse_wheel     MOUSEEVENTF_WHEEL(0x0800) 或 MOUSEEVENTF_HWHEEL(0x1000)
//   key             KEYEVENTF_EXTENDEDKEY(0x0001) | KEYEVENTF_KEYUP(0x0002)
//   key_char        KEYEVENTF_UNICODE(0x0004)；x 字段复用为字符码
//   sync（主从投递）不落盘，只在总线内部流动。
//
// 坐标系：x/y 是**客户区坐标**（与 Mouse/Key 各 API 同口径，见 libop.h 顶部约定）。
// 未绑定窗口时客户区即屏幕坐标。回放时要落在同一个绑定窗口上才有意义——
// 这一点在 InputPlayback 的 JSON 里以 "bound":0/1 报出来，不做静默假装。

#include <string>

namespace op::input {

enum class InputEventType : int {
    MouseMove = 0,
    MouseButton = 1,
    MouseWheel = 2,
    Key = 3,
    KeyChar = 4,
};

// `type` 字段的字符串形式（JSONL 落盘用）
const wchar_t *input_event_type_name(InputEventType type);

struct InputEvent {
    // 相对录制起点的时间（毫秒，浮点保留亚毫秒精度：高速移动时相邻事件可能 <1ms）
    double t = 0.0;
    InputEventType type = InputEventType::MouseMove;
    long vk = 0;
    long x = 0;
    long y = 0;
    long wheel = 0;
    long flags = 0;

    // JSONL 单行（不含换行符）。字段顺序固定为 t,type,vk,x,y,wheel,flags。
    std::wstring to_jsonl() const;
};

// 解析一行 JSONL。结构不合法（缺字段/类型不符）返回 false。
// 未知的 type 字符串视为不合法：宁可拒绝，也不要把事件错投成移动。
bool parse_input_event(const std::wstring &line, InputEvent &out);

} // namespace op::input

#endif // OP_INPUT_INPUT_EVENT_H_
