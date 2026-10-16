#pragma once

#include <string>
#include <utility>
#include <vector>

namespace op::json {

// 轻量 JSON DOM + 解析/序列化 + 路径导航（P3 第一批，2026-10-16）。
// 自包含递归下降实现，不引第三方库（项目无 nlohmann 等依赖）。
// 输入输出均为 std::wstring（与 C API/COM 的 wchar_t 一致）。
//
// 设计取舍：
// - 对象保留插入顺序（vector<pair>），重复键原地覆盖值；
// - Number 统一存 double，整数序列化时若可无损表示则按整数输出；
// - Windows 下 wchar_t 为 UTF-16 码元，解析 \uXXXX 时补充平面字符按代理对
//   两个码元存储，序列化原样输出（JSON 文本层面合法）；
// - 解析深度上限 200，防止恶意输入栈溢出。

enum class Type { Null, Bool, Number, String, Array, Object };

struct Value {
    Type type = Type::Null;
    bool boolean = false;
    double number = 0.0;
    std::wstring str;
    std::vector<Value> arr;
    std::vector<std::pair<std::wstring, Value>> obj;
};

// 解析。成功返回 true 且 out 为根节点；失败返回 false 且 out 为 Null。
bool Parse(const std::wstring &text, Value &out);

// 紧凑序列化（无多余空白）。数字按整数/%.15g 规则输出。
std::wstring Stringify(const Value &v);

// 路径段：键 或 数组下标。路径语法 "a.b[0].c" 与 "a.b.0.c" 等价，
// 空路径指向根。键名不支持含 '.' / '[' 的字符。
struct PathSeg {
    bool is_index = false;
    std::wstring key;
    long index = 0;
};

// 解析路径表达式；语法非法返回 false（空路径合法 = 根）。
bool ParsePath(const std::wstring &path, std::vector<PathSeg> &segs);

// 只读导航：逐段下钻，任何一段不存在/类型不符返回 nullptr。
const Value *Navigate(const Value &root, const std::vector<PathSeg> &segs);
Value *Navigate(Value &root, const std::vector<PathSeg> &segs);

// 可写导航（供 Set 使用）：中间段不存在时按后续段的类型自动创建
// Object/Array（Null 节点原地转型；数组越界用 Null 填充），类型冲突返回 nullptr。
Value *NavigateOrCreate(Value &root, const std::vector<PathSeg> &segs);

// 删除最终段：对象删键、数组按下标 erase。父路径不可达返回 false。
bool DeleteAt(Value &root, const std::vector<PathSeg> &segs);

} // namespace op::json
