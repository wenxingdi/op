#pragma once
#ifndef OP_BASE_UTILS_H_
#define OP_BASE_UTILS_H_
#include "Types.h"
std::wstring _s2wstring(const std::string &s);
std::string _ws2string(const std::wstring &s);

std::string utf8_to_ansi(std::string strUTF8);
// 将路径转化为全局路径
long Path2GlobalPath(const std::wstring &file, const std::wstring &curr_path, std::wstring &out);

void split(const std::wstring &s, std::vector<std::wstring> &v, const std::wstring &c);
void split(const std::string &s, std::vector<std::string> &v, const std::string &c);

void wstring2upper(std::wstring &s);
void string2upper(std::string &s);

void wstring2lower(std::wstring &s);
void string2lower(std::string &s);

void replacea(std::string &str, const std::string &oldval, const std::string &newval);
void replacew(std::wstring &str, const std::wstring &oldval, const std::wstring &newval);

// for debug
long setlog(const wchar_t *format, ...);
//
long setlog(const char *format, ...);

// Returns the last Win32 error, in string format. Returns an empty string if there is no error.
std::string GetLastErrorAsString();

int inline hex2bin(int c) {
    // 归一化小写 a-f(调用方 ProcessMemory 等可能直接传原始字符,不保证已 toupper)
    if (c >= L'a' && c <= L'f')
        c -= (L'a' - L'A');
    return c <= L'9' ? c - L'0' : c - L'A' + 10;
};

int inline bin2hex(int c) {
    int ans = 0;
    int c1 = c >> 4 & 0xf;
    int c2 = c & 0xf;
    ans |= (c1 <= 9 ? c1 + L'0' : c1 + 'A' - 10) << 8;
    ans |= c2 <= 9 ? c2 + L'0' : c2 + 'A' - 10;
    return ans;
};

constexpr int PTY(op::uint pt) {
    return pt >> 16;
}

constexpr int PTX(op::uint pt) {
    return pt & 0xffff;
}

template <typename T> void nextVal(const T &t, int *next) {
    next[0] = -1;
    int k = -1, j = 0;
    while (j < (int)t.size() - 1) {
        if (k == -1 || t[k] == t[j]) {
            k++;
            j++;
            next[j] = k;
        } else {
            k = next[k];
        }
    }
}
// KMP 子串查找,返回 t 在 s 中首次出现的下标,未找到返回 -1。
// 修正两处:
//   1) 命中条件原写作 j == s.size(),应为 j == t.size()(模式串走完才算命中);
//      当 s.size() != t.size() 时旧版恒返回 -1。
//   2) t 为空串时 next.data() 是 nullptr,nextVal 里 next[0] = -1 直接崩溃。
template <typename T> int kmp(const T &s, const T &t) {
    if (t.empty())
        return 0;
    if (s.size() < t.size())
        return -1;

    std::vector<int> next(t.size(), 0);
    nextVal(t, next.data());
    int i = 0, j = 0;
    while (i < (int)s.size() && j < (int)t.size()) {
        if (j == -1 || s[i] == t[j]) {
            i++;
            j++;
        } else {
            j = next[j];
        }
    }
    return j == (int)t.size() ? i - j : -1;
}

namespace op {
std::ostream &operator<<(std::ostream &o, point_t const &rhs);
std::wostream &operator<<(std::wostream &o, point_t const &rhs);
} // namespace op

bool Delay(long mis);
bool Delays(long mis_min, long mis_max);

// 拟人延时抖动：以 base_ms 为基准做 ±percent% 均匀随机，下限 1ms（防按下/弹起被合并）；
// base_ms<=0 返回 0（保持旧的无延时语义）。
long jittered_delay_ms(long base_ms, long percent);
// 实际执行抖动延时，等价 Delay(jittered_delay_ms(base_ms, percent))。percent 缺省 40（±40%）。
bool DelayJitter(long base_ms, long percent = 40);
// 进程级一次性播种 rand()：本次调用完成播种返回 true，已被（本进程任一线程）播种返回 false。
// rand() 默认种子固定会让"随机"轨迹/落点跨进程跑出完全相同的序列，多开同脚本等于明文自动化。
bool SeedProcessRandom();

#endif // OP_BASE_UTILS_H_
