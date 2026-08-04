#pragma once

#include <iomanip>
#include <sstream>
#include <string>

namespace op::internal::json {

inline std::wstring EscapeString(const std::wstring &value) {
    std::wstring escaped;
    escaped.reserve(value.size() + 8);
    for (wchar_t ch : value) {
        switch (ch) {
        case L'\\':
            escaped += L"\\\\";
            break;
        case L'"':
            escaped += L"\\\"";
            break;
        case L'\b':
            escaped += L"\\b";
            break;
        case L'\f':
            escaped += L"\\f";
            break;
        case L'\n':
            escaped += L"\\n";
            break;
        case L'\r':
            escaped += L"\\r";
            break;
        case L'\t':
            escaped += L"\\t";
            break;
        default:
            // 其余控制字符必须转义成 \u00XX，否则生成的是非法 JSON，
            // 严格的服务端(如 FastAPI/pydantic)会直接 400。
            if (ch < 0x20) {
                static const wchar_t *kHex = L"0123456789ABCDEF";
                escaped += L"\\u00";
                escaped.push_back(kHex[(ch >> 4) & 0xF]);
                escaped.push_back(kHex[ch & 0xF]);
            } else {
                escaped.push_back(ch);
            }
            break;
        }
    }
    return escaped;
}

namespace detail {

// 把一个 Unicode 码点追加为 UTF-8 字节序列。
// UnescapeString 的调用方(OcrService / YoloDetector)拿到结果后会立刻做
// utf8_to_ansi()，所以这里必须输出 UTF-8。
inline void AppendUtf8(std::string &out, unsigned int cp) {
    if (cp <= 0x7Fu) {
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FFu) {
        out.push_back(static_cast<char>(0xC0u | (cp >> 6)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else if (cp <= 0xFFFFu) {
        out.push_back(static_cast<char>(0xE0u | (cp >> 12)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    } else {
        out.push_back(static_cast<char>(0xF0u | (cp >> 18)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    }
}

// 读取 pos 处的 4 位十六进制，成功返回 true。
inline bool ParseHex4(const std::string &s, size_t pos, unsigned int &out) {
    if (pos + 4 > s.size())
        return false;
    unsigned int v = 0;
    for (size_t k = 0; k < 4; ++k) {
        const unsigned char c = static_cast<unsigned char>(s[pos + k]);
        unsigned int d = 0;
        if (c >= '0' && c <= '9')
            d = c - '0';
        else if (c >= 'a' && c <= 'f')
            d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F')
            d = c - 'A' + 10;
        else
            return false;
        v = (v << 4) | d;
    }
    out = v;
    return true;
}

} // namespace detail

// 这里只反转服务响应里用到的字符串转义，不做完整 JSON 语法解析。
inline std::string UnescapeString(const std::string &value) {
    std::string out;
    out.reserve(value.size());
    for (size_t i = 0; i < value.size(); ++i) {
        if (value[i] != '\\') {
            out.push_back(value[i]);
            continue;
        }
        if (i + 1 >= value.size())
            break;
        char c = value[++i];
        switch (c) {
        case '"':
            out.push_back('"');
            break;
        case '\\':
            out.push_back('\\');
            break;
        case '/':
            out.push_back('/');
            break;
        case 'b':
            out.push_back('\b');
            break;
        case 'f':
            out.push_back('\f');
            break;
        case 'n':
            out.push_back('\n');
            break;
        case 'r':
            out.push_back('\r');
            break;
        case 't':
            out.push_back('\t');
            break;
        case 'u': {
            // Python 侧 json.dumps 默认 ensure_ascii=True，中文会被输出成 \u4e2d\u6587。
            // 旧实现走 default 分支，把 'u' 原样吐出、四位十六进制当普通字符留下，
            // 结果就是 PaddleOCR / Tesseract HTTP 服务返回的中文全部乱码。
            unsigned int cp = 0;
            if (!detail::ParseHex4(value, i + 1, cp)) {
                out.push_back(c); // 不是合法 \uXXXX，按原字符处理
                break;
            }
            i += 4;
            if (cp >= 0xD800u && cp <= 0xDBFFu) {
                // 高位代理，需要再拼一个 \uDCxx 低位代理才是完整码点(emoji 等)
                unsigned int lo = 0;
                if (i + 6 < value.size() && value[i + 1] == '\\' && value[i + 2] == 'u' &&
                    detail::ParseHex4(value, i + 3, lo) && lo >= 0xDC00u && lo <= 0xDFFFu) {
                    cp = 0x10000u + ((cp - 0xD800u) << 10) + (lo - 0xDC00u);
                    i += 6;
                } else {
                    cp = 0xFFFDu; // 孤立高位代理
                }
            } else if (cp >= 0xDC00u && cp <= 0xDFFFu) {
                cp = 0xFFFDu; // 孤立低位代理
            }
            detail::AppendUtf8(out, cp);
            break;
        }
        default:
            out.push_back(c);
            break;
        }
    }
    return out;
}

inline std::wstring FormatDouble(double value, int precision = 6) {
    std::wostringstream oss;
    oss << std::fixed << std::setprecision(precision) << value;
    return oss.str();
}

} // namespace op::internal::json
