#pragma once
#ifndef OP_IMAGE_COLOR_H_
#define OP_IMAGE_COLOR_H_
#include "../base/Types.h"
#include <algorithm>
#include <cwctype>
#include <cctype>
#define WORD_BKCOLOR 0
#define WORD_COLOR 1
// #include "../Tool.h"
#include <math.h>
#define color2uint(color) (*(uint *)&color)

namespace op {

template <typename T> constexpr T OP_ABS(T x) {
    return x < 0 ? -x : x;
}
template <typename T> constexpr bool IN_RANGE(T lhs, T rhs, T df) {
    return OP_ABS(lhs.b - rhs.b) <= df.b && OP_ABS(lhs.g - rhs.g) <= df.g && OP_ABS(lhs.r - rhs.r) <= df.r;
}
constexpr char HEX_DIGIT_A(unsigned char value) {
    return value < 10 ? static_cast<char>('0' + value) : static_cast<char>('A' + value - 10);
}
constexpr wchar_t HEX_DIGIT_W(unsigned char value) {
    return value < 10 ? static_cast<wchar_t>(L'0' + value) : static_cast<wchar_t>(L'A' + value - 10);
}
// 颜色结构

// #pragma pack(push)
#pragma pack(1)
struct color_t {
    // b is in low address ,alpha is in high address
    uchar b, g, r, alpha;
    color_t() : b(0), g(0), r(0), alpha(0) {
    }
    color_t(int b_, int g_, int r_) : b(b_), g(g_), r(r_), alpha(0xffu) {
    }

    // 严格颜色解析（2026-10-17 加固，替代裸 swscanf）：
    // 仅接受恰好 6 位十六进制（忽略首尾空白）；空串/纯空白视为合法“零颜色”。
    // 旧实现 swscanf("%02X%02X%02X") 对短段/非法段部分成功后静默错位
    // （如偏色 "20" → r=0x20,g=0,b=0，逐通道错位且无任何报错），此处显式拒绝。
    // 返回 false 时已把颜色置为 000000，调用方应 setlog 报错。
    bool try_str2color(const std::wstring &s) {
        const size_t bpos = s.find_first_not_of(L" \t\r\n");
        if (bpos == std::wstring::npos) { // 空/纯空白：合法零颜色
            b = g = r = 0;
            return true;
        }
        const size_t epos = s.find_last_not_of(L" \t\r\n");
        std::wstring t = s.substr(bpos, epos - bpos + 1);
        std::transform(t.begin(), t.end(), t.begin(), ::towupper);
        const bool hex6 = t.size() == 6 && std::all_of(t.begin(), t.end(), [](wchar_t c) { return iswxdigit(c) != 0; });
        unsigned int rv = 0, gv = 0, bv = 0;
        if (!hex6 || swscanf(t.c_str(), L"%02X%02X%02X", &rv, &gv, &bv) != 3) {
            b = g = r = 0;
            return false;
        }
        b = static_cast<uchar>(bv);
        g = static_cast<uchar>(gv);
        r = static_cast<uchar>(rv);
        return true;
    }
    bool try_str2color(const std::string &s) {
        const size_t bpos = s.find_first_not_of(" \t\r\n");
        if (bpos == std::string::npos) {
            b = g = r = 0;
            return true;
        }
        const size_t epos = s.find_last_not_of(" \t\r\n");
        std::string t = s.substr(bpos, epos - bpos + 1);
        std::transform(t.begin(), t.end(), t.begin(), ::toupper);
        const bool hex6 = t.size() == 6 && std::all_of(t.begin(), t.end(), [](char c) { return isxdigit(static_cast<unsigned char>(c)) != 0; });
        unsigned int rv = 0, gv = 0, bv = 0;
        if (!hex6 || sscanf(t.c_str(), "%02X%02X%02X", &rv, &gv, &bv) != 3) {
            b = g = r = 0;
            return false;
        }
        b = static_cast<uchar>(bv);
        g = static_cast<uchar>(gv);
        r = static_cast<uchar>(rv);
        return true;
    }
    // 兼容入口：解析失败同样置零但不报错——需要暴露错误的调用点改用 try_str2color。
    color_t &str2color(const std::wstring &s) {
        try_str2color(s);
        return *this;
    }
    color_t &str2color(const std::string &s) {
        try_str2color(s);
        return *this;
    }
    std::string tostr() {
        std::string result(6, '0');
        result[0] = HEX_DIGIT_A(r >> 4);
        result[1] = HEX_DIGIT_A(r & 0x0F);
        result[2] = HEX_DIGIT_A(g >> 4);
        result[3] = HEX_DIGIT_A(g & 0x0F);
        result[4] = HEX_DIGIT_A(b >> 4);
        result[5] = HEX_DIGIT_A(b & 0x0F);
        return result;
    }
    std::wstring towstr() {
        std::wstring result(6, L'0');
        result[0] = HEX_DIGIT_W(r >> 4);
        result[1] = HEX_DIGIT_W(r & 0x0F);
        result[2] = HEX_DIGIT_W(g >> 4);
        result[3] = HEX_DIGIT_W(g & 0x0F);
        result[4] = HEX_DIGIT_W(b >> 4);
        result[5] = HEX_DIGIT_W(b & 0x0F);
        return result;
    }
    uchar toGray() const {
        return (r * 299 + g * 587 + b * 114 + 500) / 1000;
    }
};
#pragma pack()
// 颜色-偏色结构
struct color_df_t {
    // 颜色
    color_t color;
    // 偏色
    color_t df;
};
// 坐标-颜色-偏色结构
struct pt_cr_df_t {
    int x, y;
    std::vector<color_df_t> crdfs;
};

} // namespace op

#endif // OP_IMAGE_COLOR_H_
