#ifndef OP_ALGORITHM_STRMATCH_H_
#define OP_ALGORITHM_STRMATCH_H_
// 文本匹配内核（P1 FindStr 增强，2026-11-21）。
//
// 设计为**纯函数 + 零依赖**（不含 <regex> 之外的平台设施），因此可以脱离 OCR
// 引擎单测，也让能力层（ImageSearchService）只负责"取行文本 + 给坐标"。
//
// 为什么不用 std::wstring::find 直接了事：FindStr 旧实现是纯子串包含，
// 找 "909" 会命中 "1909"/"9090"，在游戏读数字场景误报很高。本内核把
// 「包含 / 精确 / 前缀 / 后缀 / 正则 / 编辑距离容错」统一成一次窗口扫描。
//
// 所有函数均不抛异常：正则构造与匹配全部 try/catch，异常 -> 视为未命中。

#include <algorithm>
#include <cwctype>
#include <string>
#include <vector>

// MSVC 的 std::regex 在 <regex> 里，但它编译慢且体积大；能力层已包含。
// 这里保留显式包含，避免依赖调用方的包含顺序。
#include <regex>

namespace op::str_match {

// 匹配模式。数值稳定（COM/C API 的 flags 位映射依赖它不要重排）。
enum class MatchMode {
    Contains = 0, // 子串包含（默认，与旧 FindStr 一致）
    Exact = 1,    // 整行完全相等
    Prefix = 2,   // 行首匹配
    Suffix = 3,   // 行尾匹配
    Regex = 4,    // 正则搜索（命中范围 = 该 match 的范围）
};

// 单个目标的匹配配置。
struct Target {
    std::wstring text;             // 目标文本；mode==Regex 时是正则表达式
    MatchMode mode = MatchMode::Contains;
    int fuzzy = 0;                 // >0 = 允许编辑距离 <= fuzzy（与 Regex 互斥）
    int max_hits = 0;              // 每个目标最多返回多少条；0 = 不限（仍受调用方总上限约束）
    bool ci = false;               // 忽略 ASCII 大小写（对 Regex 无效，见 .cpp 注释）
    bool confirmed_only = false;   // 只匹配 is_partial==false 的行
    std::wstring id;               // 调用方自定义 id，原样回显
};

// 命中结果。
struct Hit {
    int line = 0;      // 所属行的索引（= 调用方传入 lines 的下标）
    int begin = 0;     // 命中窗口在行文本中的起始下标（字符级，非字节）
    int end = 0;       // 结束下标（开区间）
    int ed = 0;        // 实际编辑距离；非模糊模式恒 0
};

// 归一化配置的合法性校验结果。
struct ConfigError {
    bool bad = false;
    const wchar_t *reason = L"";
};

inline ConfigError ValidateTarget(const Target &t) {
    if (t.fuzzy > 3)
        return {true, L"fuzzy exceeds the supported maximum of 3"};
    if (t.fuzzy > 0 && t.mode == MatchMode::Regex)
        return {true, L"fuzzy and regex are mutually exclusive"};
    if (t.max_hits < 0)
        return {true, L"max_hits must be >= 0"};
    return {};
}

// ASCII 大小写折叠。只处理 ASCII 是**有意**的：
// 全 Unicode 大小写折叠（towlower）在中文/日文文本上会改变字符，导致"忽略大小写"
// 反而改变匹配结果。ASCII 覆盖游戏 UI 里英文/数字的全部实际需求。
inline wchar_t FoldAscii(wchar_t ch) {
    if (ch >= L'A' && ch <= L'Z')
        return wchar_t(ch - L'A' + L'a');
    return ch;
}

inline bool CharEq(wchar_t a, wchar_t b, bool ci) {
    if (a == b)
        return true;
    return ci && FoldAscii(a) == FoldAscii(b);
}

// 编辑距离（Levenshtein）。长度不同即提前退出：本内核只在等长或 ±fuzzy 的
// 窗口上调用，长度差 > fuzzy 的候选已被枚举阶段排除，这里仍做保护。
inline int EditDistance(const std::wstring &a, const std::wstring &b, int limit) {
    const int n = int(a.size()), m = int(b.size());
    if (n == 0)
        return m;
    if (m == 0)
        return n;
    if (std::abs(n - m) > limit)
        return limit + 1;
    std::vector<int> prev(size_t(m) + 1), cur(size_t(m) + 1);
    for (int j = 0; j <= m; ++j)
        prev[size_t(j)] = j;
    for (int i = 1; i <= n; ++i) {
        cur[0] = i;
        int row_min = cur[0];
        for (int j = 1; j <= m; ++j) {
            const int cost = (a[size_t(i) - 1] == b[size_t(j) - 1]) ? 0 : 1;
            cur[size_t(j)] = (std::min)({prev[size_t(j)] + 1, cur[size_t(j) - 1] + 1, prev[size_t(j) - 1] + cost});
            row_min = (std::min)(row_min, cur[size_t(j)]);
        }
        // 提前剪枝：整行最小值已超限，最终距离不可能回到 limit 以内
        if (row_min > limit)
            return limit + 1;
        prev.swap(cur);
    }
    return prev[size_t(m)];
}

namespace detail {

// 在 line 中查找与 target 等长（或 ±fuzzy）的窗口，收集全部命中。
// 不做整行模式约束（Exact/Prefix/Suffix 在外层按位置过滤）。
inline void CollectContains(const std::wstring &line, const Target &t, int line_index,
                            std::vector<Hit> &out) {
    const std::wstring &needle = t.text;
    const int L = int(line.size()), N = int(needle.size());
    if (N == 0 || N > L)
        return;
    if (t.fuzzy <= 0) {
        for (int i = 0; i + N <= L; ++i) {
            bool ok = true;
            for (int k = 0; k < N; ++k) {
                if (!CharEq(line[size_t(i + k)], needle[size_t(k)], t.ci)) {
                    ok = false;
                    break;
                }
            }
            if (ok)
                out.push_back(Hit{line_index, i, i + N, 0});
        }
        return;
    }
    // 模糊：枚举 len-fuzzy .. len+fuzzy 的窗口，按 (起始位置, 距离) 取每处最优。
    // 同一起点只保留最小 ed 的一条，避免同一处报多条。
    //
    // 为什么要"同长度优先"：fuzzy=1 找 "割喉台"(3 字) 时，"割喉"(2 字) 的编辑距离
    // 也是 1（删一个字）。若纯按 ed 最小取，会报出一个只有 2 字、明显不属于目标的
    // 命中，后续字符级坐标也就落在错误的位置上。这与直觉相反：删掉一个字的代价
    // 在"找完整词"的语义下应高于替换一个字。故排序键为 (ed, |候选长度-目标长度|,
    // 起始位置)，同 ed 时优先长度贴近目标的候选。
    const int lo = (std::max)(1, N - t.fuzzy);
    const int hi = N + t.fuzzy;
    struct Cand {
        int ed;
        int len;
        int len_diff;
    };
    for (int i = 0; i < L; ++i) {
        Cand best{t.fuzzy + 1, 0, std::abs(0 - N)};
        for (int w = lo; w <= hi; ++w) {
            if (i + w > L)
                break;
            const std::wstring cand = line.substr(size_t(i), size_t(w));
            const int ed = EditDistance(cand, needle, t.fuzzy);
            const int len_diff = std::abs(w - N);
            // 更好 = ed 更小；ed 相同则长度更接近目标长度者优先
            if (ed < best.ed || (ed == best.ed && ed <= t.fuzzy && len_diff < best.len_diff)) {
                best = Cand{ed, w, len_diff};
            }
        }
        if (best.ed <= t.fuzzy)
            out.push_back(Hit{line_index, i, i + best.len, best.ed});
    }
    // 模糊命中可能在同一处重叠（例如 "abc" 在 "aabc" 上 i=0/1 都可 ≤1），
    // 保留最左的最优解，并剔除被已接受命中完全包含的项。
    std::vector<Hit> kept;
    for (const Hit &h : out) {
        bool covered = false;
        for (const Hit &k : kept) {
            if (h.begin >= k.begin && h.end <= k.end && h.ed >= k.ed) {
                covered = true;
                break;
            }
        }
        if (!covered)
            kept.push_back(h);
    }
    out.swap(kept);
}

// 整行位置约束：Exact 要求命中覆盖整行；Prefix/Suffix 约束首尾。
inline bool PositionOk(MatchMode mode, const Hit &h, int line_len) {
    switch (mode) {
    case MatchMode::Exact:
        return h.begin == 0 && h.end == line_len;
    case MatchMode::Prefix:
        return h.begin == 0;
    case MatchMode::Suffix:
        return h.end == line_len;
    default:
        return true;
    }
}

} // namespace detail

// 正则模式的长度上限：防止病态模式把 std::regex 拖进指数级回溯。
inline constexpr int kMaxRegexLength = 512;

// 对**单行**做匹配，命中追加到 out。
// line 为空或配置非法 -> 不追加、返回 false。
// 正则异常（非法模式/回溯炸栈）-> 不追加、返回 false（调用方据此写 error 字段）。
inline bool MatchLine(const std::wstring &line, const Target &t, int line_index, std::vector<Hit> &out) {
    if (ValidateTarget(t).bad)
        return false;
    if (t.text.empty() || line.empty())
        return false;

    if (t.mode == MatchMode::Regex) {
        if (int(t.text.size()) > kMaxRegexLength)
            return false;
        try {
            const std::wregex re(t.text);
            auto it = std::wsregex_iterator(line.begin(), line.end(), re);
            const auto end_it = std::wsregex_iterator();
            for (; it != end_it; ++it) {
                const auto &m = *it;
                if (m.length(0) == 0) // 零宽匹配：不报命中，避免无限语义
                    continue;
                const int b = int(m.position(0));
                out.push_back(Hit{line_index, b, b + int(m.length(0)), 0});
            }
            return true;
        } catch (...) {
            // std::regex_error / std::bad_alloc / 其他 -> 视为该目标无效
            return false;
        }
    }

    std::vector<Hit> raw;
    detail::CollectContains(line, t, line_index, raw);
    const int L = int(line.size());
    for (const Hit &h : raw) {
        if (t.mode != MatchMode::Contains && !detail::PositionOk(t.mode, h, L))
            continue;
        out.push_back(h);
    }
    return true;
}

// 对多行做匹配（跨行不命中：命中窗口必须落在同一行内）。
// 返回 true = 配置合法；结果按行号升序、行内按下标升序。
inline bool MatchAll(const std::vector<std::wstring> &lines, const Target &t, std::vector<Hit> &out) {
    out.clear();
    if (ValidateTarget(t).bad)
        return false;
    bool ok = true;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (!MatchLine(lines[i], t, int(i), out))
            ok = false; // 正则非法等情况：整目标视为未命中
    }
    std::stable_sort(out.begin(), out.end(), [](const Hit &a, const Hit &b) {
        if (a.line != b.line)
            return a.line < b.line;
        return a.begin < b.begin;
    });
    if (t.max_hits > 0 && int(out.size()) > t.max_hits)
        out.resize(size_t(t.max_hits));
    if (!ok)
        out.clear();
    return ok;
}

// 把 flags 位掩码解析为 Target（C API / COM 的标量入参路径）。
// 位定义与 docs/2026-11/P1_FindStr增强与P3耗时自省_实施方案.md §2.3 一致：
//   1=regex  2=exact  4=ci  64=fuzzy<=1  128=fuzzy<=2  256=confirmed_only
// 位 8/16/32 预留（prefix/suffix/normalize）。
inline bool TargetFromFlags(const std::wstring &pattern, long flags, Target &out) {
    out = Target{};
    out.text = pattern;
    bool has_regex = (flags & 1) != 0;
    bool has_exact = (flags & 2) != 0;
    bool has_fuzzy1 = (flags & 64) != 0;
    bool has_fuzzy2 = (flags & 128) != 0;
    // 互斥组合直接判非法（不静默降级：静默降级会让调用方以为生效了）
    if (has_regex + has_exact + has_fuzzy1 + has_fuzzy2 > 1)
        return false;
    if (has_regex)
        out.mode = MatchMode::Regex;
    else if (has_exact)
        out.mode = MatchMode::Exact;
    else
        out.mode = MatchMode::Contains;
    if (has_fuzzy1)
        out.fuzzy = 1;
    if (has_fuzzy2)
        out.fuzzy = 2;
    out.ci = (flags & 4) != 0;
    out.confirmed_only = (flags & 256) != 0;
    return true;
}

} // namespace op::str_match

#endif // OP_ALGORITHM_STRMATCH_H_
