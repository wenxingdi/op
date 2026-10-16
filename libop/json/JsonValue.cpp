#include "JsonValue.h"

#include "base/JsonUtils.h"

#include <cmath>
#include <cstdio>

namespace op::json {

namespace {

constexpr int kMaxDepth = 200;

class Parser {
public:
    explicit Parser(const std::wstring &text) : text_(text) {}

    bool Parse(Value &out) {
        SkipWs();
        if (!ParseValue(out, 0))
            return false;
        SkipWs();
        return pos_ == text_.size(); // 顶层值后只允许空白
    }

private:
    const std::wstring &text_;
    size_t pos_ = 0;

    void SkipWs() {
        while (pos_ < text_.size()) {
            const wchar_t c = text_[pos_];
            if (c == L' ' || c == L'\t' || c == L'\n' || c == L'\r')
                ++pos_;
            else
                break;
        }
    }

    bool Consume(wchar_t c) {
        if (pos_ < text_.size() && text_[pos_] == c) {
            ++pos_;
            return true;
        }
        return false;
    }

    bool ParseValue(Value &out, int depth) {
        if (depth > kMaxDepth)
            return false;
        SkipWs();
        if (pos_ >= text_.size())
            return false;
        switch (text_[pos_]) {
        case L'{':
            return ParseObject(out, depth);
        case L'[':
            return ParseArray(out, depth);
        case L'"':
            out.type = Type::String;
            return ParseString(out.str);
        case L't': {
            if (!MatchLiteral(L"true"))
                return false;
            out.type = Type::Bool;
            out.boolean = true;
            return true;
        }
        case L'f': {
            if (!MatchLiteral(L"false"))
                return false;
            out.type = Type::Bool;
            out.boolean = false;
            return true;
        }
        case L'n': {
            if (!MatchLiteral(L"null"))
                return false;
            out.type = Type::Null;
            return true;
        }
        default:
            return ParseNumber(out);
        }
    }

    bool MatchLiteral(const wchar_t *lit) {
        size_t i = 0;
        while (lit[i]) {
            if (pos_ + i >= text_.size() || text_[pos_ + i] != lit[i])
                return false;
            ++i;
        }
        pos_ += i;
        return true;
    }

    bool ParseObject(Value &out, int depth) {
        Consume(L'{');
        out.type = Type::Object;
        out.obj.clear();
        SkipWs();
        if (Consume(L'}'))
            return true;
        while (true) {
            SkipWs();
            std::wstring key;
            if (!ParseString(key))
                return false;
            SkipWs();
            if (!Consume(L':'))
                return false;
            Value val;
            if (!ParseValue(val, depth + 1))
                return false;
            bool replaced = false;
            for (auto &kv : out.obj) {
                if (kv.first == key) {
                    kv.second = std::move(val); // 重复键：原地覆盖，保位置
                    replaced = true;
                    break;
                }
            }
            if (!replaced)
                out.obj.emplace_back(std::move(key), std::move(val));
            SkipWs();
            if (Consume(L','))
                continue;
            return Consume(L'}');
        }
    }

    bool ParseArray(Value &out, int depth) {
        Consume(L'[');
        out.type = Type::Array;
        out.arr.clear();
        SkipWs();
        if (Consume(L']'))
            return true;
        while (true) {
            Value val;
            if (!ParseValue(val, depth + 1))
                return false;
            out.arr.emplace_back(std::move(val));
            SkipWs();
            if (Consume(L','))
                continue;
            return Consume(L']');
        }
    }

    bool ParseString(std::wstring &out) {
        if (!Consume(L'"'))
            return false;
        out.clear();
        while (pos_ < text_.size()) {
            const wchar_t c = text_[pos_++];
            if (c == L'"')
                return true;
            if (c != L'\\') {
                out.push_back(c); // 控制字符宽松放行（输出端 EscapeString 会补转义）
                continue;
            }
            if (pos_ >= text_.size())
                return false;
            const wchar_t esc = text_[pos_++];
            switch (esc) {
            case L'"':
                out.push_back(L'"');
                break;
            case L'\\':
                out.push_back(L'\\');
                break;
            case L'/':
                out.push_back(L'/');
                break;
            case L'b':
                out.push_back(L'\b');
                break;
            case L'f':
                out.push_back(L'\f');
                break;
            case L'n':
                out.push_back(L'\n');
                break;
            case L'r':
                out.push_back(L'\r');
                break;
            case L't':
                out.push_back(L'\t');
                break;
            case L'u': {
                unsigned int cp = 0;
                if (!ParseHex4(cp))
                    return false;
                if (cp >= 0xD800u && cp <= 0xDBFFu) {
                    // 高位代理，尝试拼接低位代理
                    if (pos_ + 1 < text_.size() && text_[pos_] == L'\\' && text_[pos_ + 1] == L'u') {
                        const size_t save = pos_;
                        pos_ += 2;
                        unsigned int lo = 0;
                        if (ParseHex4(lo) && lo >= 0xDC00u && lo <= 0xDFFFu) {
                            if (sizeof(wchar_t) == 2) {
                                out.push_back(static_cast<wchar_t>(cp));
                                out.push_back(static_cast<wchar_t>(lo));
                            } else {
                                const unsigned int full =
                                    0x10000u + ((cp - 0xD800u) << 10) + (lo - 0xDC00u);
                                out.push_back(static_cast<wchar_t>(full));
                            }
                            break;
                        }
                        pos_ = save; // 孤立高位代理：按原码元输出
                    }
                }
                out.push_back(static_cast<wchar_t>(cp));
                break;
            }
            default:
                return false; // 非法转义
            }
        }
        return false; // 未闭合
    }

    bool ParseHex4(unsigned int &out) {
        if (pos_ + 4 > text_.size())
            return false;
        unsigned int v = 0;
        for (int k = 0; k < 4; ++k) {
            const wchar_t c = text_[pos_ + k];
            unsigned int d;
            if (c >= L'0' && c <= L'9')
                d = static_cast<unsigned int>(c - L'0');
            else if (c >= L'a' && c <= L'f')
                d = static_cast<unsigned int>(c - L'a') + 10u;
            else if (c >= L'A' && c <= L'F')
                d = static_cast<unsigned int>(c - L'A') + 10u;
            else
                return false;
            v = (v << 4) | d;
        }
        pos_ += 4;
        out = v;
        return true;
    }

    bool ParseNumber(Value &out) {
        const size_t start = pos_;
        Consume(L'-');
        if (pos_ >= text_.size())
            return false;
        if (text_[pos_] == L'0') {
            ++pos_;
        } else if (text_[pos_] >= L'1' && text_[pos_] <= L'9') {
            while (pos_ < text_.size() && text_[pos_] >= L'0' && text_[pos_] <= L'9')
                ++pos_;
        } else {
            return false;
        }
        if (pos_ < text_.size() && text_[pos_] == L'.') {
            ++pos_;
            if (pos_ >= text_.size() || text_[pos_] < L'0' || text_[pos_] > L'9')
                return false;
            while (pos_ < text_.size() && text_[pos_] >= L'0' && text_[pos_] <= L'9')
                ++pos_;
        }
        if (pos_ < text_.size() && (text_[pos_] == L'e' || text_[pos_] == L'E')) {
            ++pos_;
            if (pos_ < text_.size() && (text_[pos_] == L'+' || text_[pos_] == L'-'))
                ++pos_;
            if (pos_ >= text_.size() || text_[pos_] < L'0' || text_[pos_] > L'9')
                return false;
            while (pos_ < text_.size() && text_[pos_] >= L'0' && text_[pos_] <= L'9')
                ++pos_;
        }
        try {
            out.number = std::stod(text_.substr(start, pos_ - start));
        } catch (...) {
            return false;
        }
        out.type = Type::Number;
        return true;
    }
};

void AppendNumber(std::wstring &out, double v) {
    if (!std::isfinite(v)) {
        out += L"null";
        return;
    }
    // 整数且在 int64 无损范围内按整数输出，避免 42.000000
    const double fl = std::floor(v);
    if (fl == v && std::fabs(v) < 9.0e15) {
        wchar_t buf[32];
        _snwprintf_s(buf, 32, L"%lld", static_cast<long long>(v));
        out += buf;
        return;
    }
    wchar_t buf[64];
    _snwprintf_s(buf, 64, L"%.15g", v);
    out += buf;
}

void StringifyTo(const Value &v, std::wstring &out) {
    switch (v.type) {
    case Type::Null:
        out += L"null";
        break;
    case Type::Bool:
        out += v.boolean ? L"true" : L"false";
        break;
    case Type::Number:
        AppendNumber(out, v.number);
        break;
    case Type::String:
        out.push_back(L'"');
        out += op::internal::json::EscapeString(v.str);
        out.push_back(L'"');
        break;
    case Type::Array:
        out.push_back(L'[');
        for (size_t i = 0; i < v.arr.size(); ++i) {
            if (i)
                out.push_back(L',');
            StringifyTo(v.arr[i], out);
        }
        out.push_back(L']');
        break;
    case Type::Object:
        out.push_back(L'{');
        for (size_t i = 0; i < v.obj.size(); ++i) {
            if (i)
                out.push_back(L',');
            out.push_back(L'"');
            out += op::internal::json::EscapeString(v.obj[i].first);
            out += L"\":";
            StringifyTo(v.obj[i].second, out);
        }
        out.push_back(L'}');
        break;
    }
}

} // namespace

bool Parse(const std::wstring &text, Value &out) {
    out = Value{};
    if (text.empty())
        return false;
    Parser parser(text);
    return parser.Parse(out);
}

std::wstring Stringify(const Value &v) {
    std::wstring out;
    out.reserve(64);
    StringifyTo(v, out);
    return out;
}

bool ParsePath(const std::wstring &path, std::vector<PathSeg> &segs) {
    segs.clear();
    if (path.empty())
        return true; // 空路径 = 根
    size_t i = 0;
    const size_t n = path.size();
    bool seg_start = true; // 当前处于段开头（路径首或 '.' 之后）
    std::wstring key;
    auto flush_key = [&]() {
        if (!key.empty()) {
            segs.push_back(PathSeg{false, key, 0});
            key.clear();
        }
    };
    while (i < n) {
        const wchar_t c = path[i];
        if (c == L'.') {
            if (seg_start)
                return false; // 空段："a..b"、".a"、"a."
            flush_key();      // 先落盘当前键，再开新段
            seg_start = true;
            ++i;
            continue;
        }
        if (c == L'[') {
            flush_key();
            ++i;
            if (i >= n || path[i] == L'-')
                return false; // 不支持负下标
            long idx = 0;
            bool any = false;
            while (i < n && path[i] >= L'0' && path[i] <= L'9') {
                idx = idx * 10 + (path[i] - L'0');
                if (idx > 10000000)
                    return false; // 防御
                ++i;
                any = true;
            }
            if (!any || i >= n || path[i] != L']')
                return false;
            ++i;
            segs.push_back(PathSeg{true, L"", idx});
            seg_start = false; // ']' 后可接 '.' 或 '['
            continue;
        }
        if (c == L']')
            return false;
        key.push_back(c);
        seg_start = false;
        ++i;
    }
    if (seg_start)
        return false; // 路径以 '.' 结尾
    flush_key();
    return true;
}

// 纯数字键（"a.b.0" 写法）遇到数组时按下标解析；非纯数字返回 false。
bool KeyToIndex(const std::wstring &key, long &idx) {
    if (key.empty())
        return false;
    long v = 0;
    for (wchar_t c : key) {
        if (c < L'0' || c > L'9')
            return false;
        v = v * 10 + (c - L'0');
        if (v > 10000000)
            return false;
    }
    idx = v;
    return true;
}

const Value *Navigate(const Value &root, const std::vector<PathSeg> &segs) {
    const Value *cur = &root;
    for (const PathSeg &seg : segs) {
        if (seg.is_index) {
            if (cur->type != Type::Array || seg.index >= static_cast<long>(cur->arr.size()))
                return nullptr;
            cur = &cur->arr[static_cast<size_t>(seg.index)];
        } else {
            if (cur->type == Type::Array) {
                // 兼容 "a.0" 写法：纯数字键 = 数组下标
                long idx = 0;
                if (KeyToIndex(seg.key, idx) && idx < static_cast<long>(cur->arr.size())) {
                    cur = &cur->arr[static_cast<size_t>(idx)];
                    continue;
                }
                return nullptr;
            }
            if (cur->type != Type::Object)
                return nullptr;
            const Value *next = nullptr;
            for (const auto &kv : cur->obj) {
                if (kv.first == seg.key) {
                    next = &kv.second;
                    break;
                }
            }
            if (!next)
                return nullptr;
            cur = next;
        }
    }
    return cur;
}

Value *Navigate(Value &root, const std::vector<PathSeg> &segs) {
    return const_cast<Value *>(Navigate(static_cast<const Value &>(root), segs));
}

Value *NavigateOrCreate(Value &root, const std::vector<PathSeg> &segs) {
    Value *cur = &root;
    for (size_t i = 0; i < segs.size(); ++i) {
        const PathSeg &seg = segs[i];
        const bool last = (i + 1 == segs.size());
        if (seg.is_index) {
            if (cur->type == Type::Null) {
                cur->type = Type::Array;
                cur->arr.clear();
            }
            if (cur->type != Type::Array)
                return nullptr;
            if (seg.index >= static_cast<long>(cur->arr.size()))
                cur->arr.resize(static_cast<size_t>(seg.index) + 1); // Null 填充
            cur = &cur->arr[static_cast<size_t>(seg.index)];
        } else {
            if (cur->type == Type::Array) {
                // 兼容 "a.0" 写法：纯数字键 = 数组下标
                long idx = 0;
                if (!KeyToIndex(seg.key, idx))
                    return nullptr;
                if (cur->type == Type::Null) {
                    cur->type = Type::Array;
                    cur->arr.clear();
                }
                if (idx >= static_cast<long>(cur->arr.size()))
                    cur->arr.resize(static_cast<size_t>(idx) + 1);
                cur = &cur->arr[static_cast<size_t>(idx)];
                continue;
            }
            if (cur->type == Type::Null) {
                cur->type = Type::Object;
                cur->obj.clear();
            }
            if (cur->type != Type::Object)
                return nullptr;
            Value *next = nullptr;
            for (auto &kv : cur->obj) {
                if (kv.first == seg.key) {
                    next = &kv.second;
                    break;
                }
            }
            if (!next) {
                cur->obj.emplace_back(seg.key, Value{});
                next = &cur->obj.back().second;
            }
            cur = next;
        }
        (void)last;
    }
    return cur;
}

bool DeleteAt(Value &root, const std::vector<PathSeg> &segs) {
    if (segs.empty())
        return false; // 不允许删根
    Value *parent = Navigate(root, std::vector<PathSeg>(segs.begin(), segs.end() - 1));
    if (!parent)
        return false;
    const PathSeg &last = segs.back();
    long array_index = -1;
    bool use_index = last.is_index;
    if (!use_index && parent->type == Type::Array && KeyToIndex(last.key, array_index))
        use_index = true; // 兼容 "a.0" 写法删数组元素
    if (use_index) {
        const long idx = last.is_index ? last.index : array_index;
        if (parent->type != Type::Array || idx >= static_cast<long>(parent->arr.size()))
            return false;
        parent->arr.erase(parent->arr.begin() + idx);
        return true;
    }
    if (parent->type != Type::Object)
        return false;
    for (auto it = parent->obj.begin(); it != parent->obj.end(); ++it) {
        if (it->first == last.key) {
            parent->obj.erase(it);
            return true;
        }
    }
    return false;
}

} // namespace op::json
