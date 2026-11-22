// 时间轴层：事件结构 + JSONL 编解码（批 E 2026-11-22）。
//
// 为什么手写而不是走 json/JsonValue：
//   录制是**热路径**（MoveToSmooth 一次可产生上百个事件），JsonValue 每次构造
//   Object/Array 带一堆 std::wstring 分配。这里字段固定为 7 个标量，手写
//   swprintf 快一个量级，且输出格式完全可控（回放端与外部日志分析都依赖这个格式）。
//   解析端仍走 JsonValue：录制文件可能被人工编辑过，容忍空格/字段顺序更实际。
#include "InputEvent.h"

#include "../json/JsonValue.h"

#include <cmath>
#include <cstdio>
#include <cwchar>

namespace op::input {

const wchar_t *input_event_type_name(InputEventType type) {
    switch (type) {
    case InputEventType::MouseMove:
        return L"mouse_move";
    case InputEventType::MouseButton:
        return L"mouse_button";
    case InputEventType::MouseWheel:
        return L"mouse_wheel";
    case InputEventType::Key:
        return L"key";
    case InputEventType::KeyChar:
        return L"key_char";
    }
    return L"unknown";
}

namespace {

bool type_from_name(const std::wstring &name, InputEventType &out) {
    if (name == L"mouse_move")
        out = InputEventType::MouseMove;
    else if (name == L"mouse_button")
        out = InputEventType::MouseButton;
    else if (name == L"mouse_wheel")
        out = InputEventType::MouseWheel;
    else if (name == L"key")
        out = InputEventType::Key;
    else if (name == L"key_char")
        out = InputEventType::KeyChar;
    else
        return false; // 未知类型一律拒绝：错投成"移动"比丢一条更糟
    return true;
}

// t 用 %.3f（毫秒到微秒）：定宽是好处的——外部用 awk/Excel 切分时不会因为
// "1" / "1.5" / "1.000001" 混排而列宽跳变；3 位小数对输入回放足够了。
void append_time(std::wstring &out, double t) {
    if (!std::isfinite(t) || t < 0.0)
        t = 0.0;
    wchar_t buf[40];
    _snwprintf_s(buf, 40, L"%.3f", t);
    out += buf;
}

bool get_number(const json::Value &root, const wchar_t *key, double &out) {
    for (const auto &kv : root.obj) {
        if (kv.first == key) {
            if (kv.second.type != json::Type::Number)
                return false;
            out = kv.second.number;
            return true;
        }
    }
    return false;
}

bool get_string(const json::Value &root, const wchar_t *key, std::wstring &out) {
    for (const auto &kv : root.obj) {
        if (kv.first == key) {
            if (kv.second.type != json::Type::String)
                return false;
            out = kv.second.str;
            return true;
        }
    }
    return false;
}

} // namespace

std::wstring InputEvent::to_jsonl() const {
    std::wstring out;
    out.reserve(96);
    out += L"{\"t\":";
    append_time(out, t);
    out += L",\"type\":\"";
    out += input_event_type_name(type);
    out += L"\"";

    wchar_t buf[48];
    _snwprintf_s(buf, 48, L",\"vk\":%ld", vk);
    out += buf;
    _snwprintf_s(buf, 48, L",\"x\":%ld", x);
    out += buf;
    _snwprintf_s(buf, 48, L",\"y\":%ld", y);
    out += buf;
    _snwprintf_s(buf, 48, L",\"wheel\":%ld", wheel);
    out += buf;
    _snwprintf_s(buf, 48, L",\"flags\":%ld", flags);
    out += buf;
    out += L"}";
    return out;
}

bool parse_input_event(const std::wstring &line, InputEvent &out) {
    json::Value root;
    if (!json::Parse(line, root) || root.type != json::Type::Object)
        return false;

    std::wstring type_name;
    if (!get_string(root, L"type", type_name))
        return false;

    InputEvent parsed;
    if (!type_from_name(type_name, parsed.type))
        return false;

    double value = 0.0;
    // t 必须存在：没有时间轴就不是录制文件，硬补 0 会让回放"瞬间打完"
    if (!get_number(root, L"t", value) || !std::isfinite(value) || value < 0.0)
        return false;
    parsed.t = value;

    // 其余字段可选（缺省 0）：手写/裁剪过的录制文件仍然可用
    if (get_number(root, L"vk", value))
        parsed.vk = static_cast<long>(value);
    if (get_number(root, L"x", value))
        parsed.x = static_cast<long>(value);
    if (get_number(root, L"y", value))
        parsed.y = static_cast<long>(value);
    if (get_number(root, L"wheel", value))
        parsed.wheel = static_cast<long>(value);
    if (get_number(root, L"flags", value))
        parsed.flags = static_cast<long>(value);

    out = parsed;
    return true;
}

} // namespace op::input
