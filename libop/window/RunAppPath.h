#pragma once
//
// RunApp(mode=1) 的「工作目录」推导 —— 从命令行字符串里提取可执行文件所在目录。
//
// 背景（2026-09-28 蜀门真机全量测阶段 3 暴露的真缺陷）：
// 旧实现是 `pos = cmd.find(".exe")` 后**向前**找 `\\` / `/`，找不到时**不重置 pos**，
// 于是 `pos` 仍停在 `.exe` 的位置 → `curr_dir = cmd.substr(0, pos)` 把
// `"notepad.exe"` 截成 **`"notepad"`** 当 `lpCurrentDirectory` 传给 `CreateProcessW`
// → **ERROR_DIRECTORY(267)**，宿主只看到 pid=0。即：**mode=1 一旦传裸文件名就必失败**。
//
// 本文件把它抽成 header-only 纯函数（零 Windows 依赖），好处：
//   1. 可以直接编进测试进程做边界用例（CreateProcessW 起真进程太重、且有副作用）；
//   2. 「返回空串」= 无法确定 = 调用方应传 nullptr（子进程继承当前目录），语义显式。
//
#include <cstddef>
#include <string>

namespace op {
namespace runapp {

// 返回可执行文件所在目录；**返回空串 = 无法确定，调用方应传 nullptr 而非空串指针**。
inline std::wstring ExtractAppDirectory(const std::wstring &cmd) {
    const size_t n = cmd.size();
    if (n == 0)
        return std::wstring();

    // 1) 路径起点：跳过前导空白与成对引号（cmd 常写成 "C:\a b\c.exe" arg）
    size_t start = 0;
    while (start < n && (cmd[start] == L' ' || cmd[start] == L'\t'))
        ++start;
    if (start < n && cmd[start] == L'"')
        ++start;

    // 2) 路径终点：优先按扩展名 .exe 定位（大小写不敏感），找不到就退化为第一个空白。
    //    ⚠ 用独立 bool 判命中，不能用 `end == n` 判断"没找到"——.exe 恰好在串尾时
    //    end 同样等于 n，会被误判成未命中而走 fallback（2026-09-29 蜀门真机实测暴露：
    //    `D:\Program Files (x86)\shumen\game.exe` → 返回 "D:\" → lpDirectory 为错误目录）。
    //    旧单测全 PASS 是因为用例全是无空格路径，fallback 找不到空白碰巧蒙对。
    size_t end = n;
    bool hit_ext = false;
    for (size_t i = start; i + 4 <= n; ++i) {
        const wchar_t c0 = cmd[i], c1 = cmd[i + 1], c2 = cmd[i + 2], c3 = cmd[i + 3];
        const bool is_exe = (c0 == L'.') && (c1 == L'e' || c1 == L'E') && (c2 == L'x' || c2 == L'X') &&
                            (c3 == L'e' || c3 == L'E');
        if (is_exe) {
            end = i + 4;
            hit_ext = true;
            break;
        }
    }
    if (!hit_ext) {
        for (size_t i = start; i < n; ++i) {
            if (cmd[i] == L' ' || cmd[i] == L'\t') {
                end = i;
                break;
            }
        }
    }

    // 3) 在 [start, end) 内从后往前找分隔符
    size_t sep = std::wstring::npos;
    for (size_t i = end; i > start; --i) {
        if (cmd[i - 1] == L'\\' || cmd[i - 1] == L'/') {
            sep = i - 1;
            break;
        }
    }
    if (sep == std::wstring::npos)
        return std::wstring(); // 裸文件名（notepad.exe）→ 交给系统按 PATH 搜索，别编造目录

    // 4) 目录体
    if (sep == start) {
        // 形如 `\notepad.exe` / "/notepad"：exe 就在当前盘根目录
        return std::wstring(1, cmd[start] == L'/' ? L'/' : L'\\');
    }
    std::wstring dir = cmd.substr(start, sep - start);
    // `C:\a.exe` → substr 得到 "C:"，那是「C 盘当前目录」而非根目录；补成分区根。
    if (dir.size() == 2 && dir[1] == L':')
        dir += L'\\';
    return dir;
}

} // namespace runapp
} // namespace op
