#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
L2 缺陷特征扫描器 (一次性审计脚本)
针对 op 库 L2 层(core 功能子系统 + 公共 API 门面)做静态体检。
检测模式(L1 同类 + 常见 C++ 陷阱):
  A. static 可变局部状态 (并发隐患, 类 J 缺陷)
  B. reinterpret_cast<*>+解引用 (类型双关风险)
  C. memcpy/memmove/std::copy 尺寸含 * 或 + (溢出隐患)
  D. new/malloc/VirtualAlloc 结果未判空即使用
  E. Win32/GDI 句柄 open 后同函数无匹配 close (泄漏启发式)
  F. .at( 无 try/catch 保护 (越界)
  G. 直接解引用可能为空的函数返回值 (FindWindow/GetProcAddress/...->)
  H. 整型截断风险 (DWORD/ULONG/SIZE_T -> int 赋值)
"""
import os, re, sys

ROOT = r"D:\AutoPro\op-master\op"
TARGET_DIRS = [
    "libop/op", "libop/capture", "libop/input", "libop/binding",
    "libop/hook", "libop/window", "libop/image", "libop/ocr",
    "libop/ipc", "libop/network", "libop/memory", "libop/com",
    "libop/base", "libop/c_api", "libop/opencv", "libop/algorithm",
    "libop/yolo", "include",
]
EXT = (".cpp", ".h", ".hpp", ".cc", ".cxx")

# ---- 模式定义 ----
PAT_A = re.compile(r'\bstatic\s+(?:std::\w+|[a-zA-Z_][\w:]*)\s+(\w+)\s*[;(]')  # 局部 static
PAT_B = re.compile(r'reinterpret_cast\s*<\s*[^>]*\*\s*>')
PAT_C = re.compile(r'(memcpy|memmove|std::copy|std::copy_n|CopyMemory|MoveMemory)\s*\(')
PAT_C_SIZE = re.compile(r'\*\s*sizeof|\+\s*sizeof|\)\s*\*\s*\w+|\*\s*\w+\s*\)')  # 尺寸含乘法/加法
PAT_D_NEW = re.compile(r'(\w+)\s*=\s*(?:new|malloc|VirtualAlloc|reinterpret_cast)')
PAT_E_OPEN = {
    'HDC': (re.compile(r'\b(?:GetDC|CreateDC|CreateCompatibleDC|BeginPaint)\s*\('),
            re.compile(r'\b(?:ReleaseDC|DeleteDC|EndPaint)\s*\(')),
    'HANDLE': (re.compile(r'\b(?:OpenProcess|CreateFile\w*|CreateToolhelp32Snapshot|CreateEvent\w*|CreateMutex\w*|CreateSemaphore\w*|CreateFileMapping\w*|OpenThread|OpenEvent\w*)\s*\('),
               re.compile(r'\bCloseHandle\s*\(')),
    'HBITMAP': (re.compile(r'\b(?:CreateCompatibleBitmap|CreateDIBSection|LoadBitmap\w*)\s*\('),
                re.compile(r'\bDeleteObject\s*\(')),
    'HGLOBAL': (re.compile(r'\b(?:GlobalAlloc|GlobalLock)\s*\('),
                re.compile(r'\b(?:GlobalFree|GlobalUnlock)\s*\(')),
}
PAT_F_AT = re.compile(r'\.at\s*\(')
PAT_G_RISKY = re.compile(r'\b(?:FindWindow\w*|GetProcAddress|GetModuleHandle\w*|LoadLibrary\w*|GetActiveWindow|GetForegroundWindow|GetDesktopWindow|GetConsoleWindow|GetWindow|GetDlgItem|CreateWindow\w*)\s*\(')
PAT_H_TRUNC = re.compile(r'=\s*(?:static_cast\s*<\s*int\s*>|\(int\)|\(long\))\s*\(')

def collect_files():
    files = []
    for d in TARGET_DIRS:
        base = os.path.join(ROOT, d)
        if not os.path.isdir(base):
            continue
        for rt, _, fs in os.walk(base):
            # 跳过备份目录
            if "_backup" in rt:
                continue
            for f in fs:
                if f.lower().endswith(EXT):
                    files.append(os.path.join(rt, f))
    return files

def read_lines(path):
    try:
        with open(path, 'r', encoding='utf-8', errors='replace') as fh:
            return fh.read().split('\n')
    except Exception:
        return []

def func_span(lines, idx):
    """粗略定位 idx 所在函数范围: 向上找最近的 '{' 配对头, 向下到匹配 '}'"""
    # 向上找函数起始(含 '{' 的行)
    start = idx
    depth = 0
    # 先向上找到函数体开始的 '{'
    i = idx
    while i >= 0:
        line = lines[i]
        depth += line.count('}') - line.count('{')
        if depth < 0 and '{' in line:
            start = i
            break
        i -= 1
    # 向下找函数体结束
    end = idx
    depth = 0
    j = start
    while j < len(lines):
        depth += lines[j].count('{') - lines[j].count('}')
        if depth <= 0 and j > start and '}' in lines[j]:
            end = j
            break
        j += 1
    return max(0, start), min(len(lines) - 1, end)

def main():
    out = []
    files = collect_files()
    out.append(f"# L2 审计扫描: {len(files)} 个文件")
    counts = {}
    for path in files:
        rel = os.path.relpath(path, ROOT)
        lines = read_lines(path)
        for i, line in enumerate(lines):
            ln = i + 1
            # A: static 局部可变
            m = PAT_A.search(line)
            if m and 'static constexpr' not in line and 'static const' not in line and 'static inline' not in line:
                out.append(f"[A][static-state] {rel}:{ln}: {line.strip()[:120]}")
                counts['A'] = counts.get('A', 0) + 1
            # B: reinterpret_cast 指针解引用
            if PAT_B.search(line):
                # 仅记录后接解引用或用于计算的
                if '->' in line or '*' in line or 'reinterpret_cast' in line:
                    out.append(f"[B][reinterpret] {rel}:{ln}: {line.strip()[:120]}")
                    counts['B'] = counts.get('B', 0) + 1
            # C: 危险 size 计算
            if PAT_C.search(line) and PAT_C_SIZE.search(line):
                out.append(f"[C][bufsize] {rel}:{ln}: {line.strip()[:130]}")
                counts['C'] = counts.get('C', 0) + 1
            # D: 分配未判空
            m = PAT_D_NEW.search(line)
            if m:
                var = m.group(1)
                # 检查下行 8 行内是否判空
                nearby = " ".join(lines[i+1:i+9]).lower()
                if f"if (!{var.lower()}" not in nearby and f"if({var.lower()} ==" not in nearby and f"if (nullptr ==" not in nearby and f"if({var.lower()} ==" not in nearby:
                    out.append(f"[D][alloc-nocheck] {rel}:{ln}: {line.strip()[:120]}")
                    counts['D'] = counts.get('D', 0) + 1
            # F: .at( 无 try
            if PAT_F_AT.search(line):
                s, e = func_span(lines, i)
                func_text = "\n".join(lines[s:e+1])
                if 'try' not in func_text and 'catch' not in func_text and 'at(' in func_text:
                    out.append(f"[F][at-noguard] {rel}:{ln}: {line.strip()[:120]}")
                    counts['F'] = counts.get('F', 0) + 1
            # G: 风险返回值解引用(在邻近行有 -> 或 *)
            if PAT_G_RISKY.search(line):
                nxt = lines[i+1] if i+1 < len(lines) else ""
                if '->' in lines[i] or (nxt and '->' in nxt and var_like(line)):
                    out.append(f"[G][ptr-ret-deref] {rel}:{ln}: {line.strip()[:120]}")
                    counts['G'] = counts.get('G', 0) + 1
            # H: 整型截断
            if PAT_H_TRUNC.search(line):
                out.append(f"[H][trunc] {rel}:{ln}: {line.strip()[:120]}")
                counts['H'] = counts.get('H', 0) + 1

    # E: 句柄泄漏(函数级启发式)
    for path in files:
        rel = os.path.relpath(path, ROOT)
        lines = read_lines(path)
        i = 0
        n = len(lines)
        while i < n:
            # 找一个函数开头(含 '{' 且前一行像声明)
            if '{' in lines[i]:
                s, e = func_span(lines, i)
                if e - s > 200:  # 跳过超大(可能是命名空间聚合)
                    i = e + 1
                    continue
                seg = lines[s:e+1]
                text = "\n".join(seg)
                for htype, (open_re, close_re) in PAT_E_OPEN.items():
                    opens = len(open_re.findall(text))
                    closes = len(close_re.findall(text))
                    if opens > closes and opens > 0:
                        # 排除 RAII 封装(段内出现 unique_ptr/shared_ptr/auto 包装)
                        if 'unique_ptr' in text or 'shared_ptr' in text or 'ScopeGuard' in text or 'std::lock_guard' in text:
                            pass
                        else:
                            out.append(f"[E][handle-leak? {htype}] {rel}:{s+1}-{e+1} opens={opens} closes={closes}")
                            counts['E'] = counts.get('E', 0) + 1
                i = e + 1
            else:
                i += 1

    out.append("")
    out.append("## 命中统计: " + ", ".join(f"{k}={v}" for k, v in sorted(counts.items())))
    sys.stdout.write("\n".join(out))

def var_like(line):
    return bool(re.search(r'\b[a-zA-Z_]\w*\s*\(', line))

if __name__ == "__main__":
    main()
