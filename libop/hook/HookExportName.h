#pragma once

// 32 位目标上按名字解析远端 hook 导出**必然失败**，本文件是它的解法。
//
// 起因（2026-09-28 蜀门真机实测）：`DLL_API` 展开为 `extern "C" __declspec(dllexport)`，
// `extern "C"` 只去掉 C++ 名字修饰，**不去掉 `__stdcall` 的 `_Name@<参数字节数>` 修饰**
// （x64 无此问题，所以只有 32 位目标踩坑）。而 blackbone 的
// `MakeRemoteFunction(proc, mod, name)` → `ProcessModules::GetExport` 是**精确名/序数**匹配，
// 不做任何修饰处理 —— 于是宿主传未修饰名时，x86 dll 一律报
//   remote function 'SetDisplayHook' not found in op_c_api_x86.dll
// 症状：32 位游戏（实测蜀门 client.exe，i386 + D3D9）上所有依赖注入的显示模式
// （dx / dx.d3d9 / dx.d3d10 / dx.d3d11 / dx.d3d12 / opengl 系）**全部绑定失败**，
// 只有不注入的 normal / gdi 能用。
//
// 修法：读本地 hook dll 的导出表，把未修饰名映射成文件里实际存在的名字。
// 未修饰名存在时优先用它（x64、以及将来给 x86 补了 .def 的构建都照常工作）；
// 一个都匹配不到时原样返回，让上层继续报 "not found" —— 不掩盖真实缺失。
//
// 本文件**零 blackbone 依赖**，纯 std + libop 日志，便于直接进单测。

#include "../base/Utils.h"

#include <cstdint>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace op::hook {

/// 纯函数：`decorated` 是否为 `plain` 的 __stdcall 修饰形式 `_plain@<十进制数字>`。
///
/// 两道检查各挡不同的误判，**都不能省**（反向验证实测：去掉 `@` 位置检查后，
/// `_Foo12` 这类「`_` + plain + 纯数字」的名字会被误判成匹配）：
///   - `@` 位置检查：挡 `_SetInputHookEx@8`（同前缀兄弟导出）与 `_Foo12`（根本没有 `@`）
///   - 后缀纯数字检查：挡 `_Foo@8x` / `_Foo@x` / `_Foo@`（`@` 对了但参数长度不合法）
inline bool IsStdcallDecoratedExportName(const std::string &decorated, const std::string &plain) {
    if (plain.empty()) {
        // 空 plain 会让 `_@0` 通过下面的检查，必须显式挡掉。
        return false;
    }
    // 最短形式 `_` + plain + `@` + 至少 1 位数字
    if (decorated.size() < plain.size() + 3) {
        return false;
    }
    if (decorated[0] != '_') {
        return false;
    }
    if (decorated.compare(1, plain.size(), plain) != 0) {
        return false;
    }
    if (decorated[1 + plain.size()] != '@') {
        return false;
    }
    const std::string digits = decorated.substr(2 + plain.size());
    return !digits.empty() && digits.find_first_not_of("0123456789") == std::string::npos;
}

/// 纯函数：在导出名集合里挑出可用的名字。
///
/// 优先级：未修饰原名 > __stdcall 修饰形式 > 原名（原样返回，交由上层报 "not found"）。
/// 未修饰名优先，是为了让 x64 与「显式加了 .def 的 x86 构建」走最短路径、行为不变。
inline std::string PickRemoteExportName(const std::vector<std::string> &exports,
                                        const std::string &plain) {
    for (const auto &e : exports) {
        if (e == plain) {
            return plain;
        }
    }
    for (const auto &e : exports) {
        if (IsStdcallDecoratedExportName(e, plain)) {
            return e;
        }
    }
    return plain;
}

namespace detail {

inline std::uint16_t ReadU16(const std::vector<unsigned char> &b, std::size_t off) {
    if (off + 2 > b.size()) {
        return 0;
    }
    return static_cast<std::uint16_t>(b[off] | (static_cast<std::uint16_t>(b[off + 1]) << 8));
}

inline std::uint32_t ReadU32(const std::vector<unsigned char> &b, std::size_t off) {
    if (off + 4 > b.size()) {
        return 0;
    }
    return static_cast<std::uint32_t>(b[off]) |
           (static_cast<std::uint32_t>(b[off + 1]) << 8) |
           (static_cast<std::uint32_t>(b[off + 2]) << 16) |
           (static_cast<std::uint32_t>(b[off + 3]) << 24);
}

/// RVA → 文件偏移。落在任何节之外返回 0（调用方据此放弃）。
inline std::size_t RvaToOffset(const std::vector<unsigned char> &b, std::size_t sections_off,
                               std::size_t section_count, std::uint32_t rva) {
    for (std::size_t i = 0; i < section_count; ++i) {
        const std::size_t s = sections_off + i * 40;  // IMAGE_SECTION_HEADER = 40 字节
        const std::uint32_t virt_size = ReadU32(b, s + 8);
        const std::uint32_t virt_addr = ReadU32(b, s + 12);
        const std::uint32_t raw_size = ReadU32(b, s + 16);
        const std::uint32_t raw_ptr = ReadU32(b, s + 20);
        // 映射到内存后实际长度取两者较大值（未初始化数据可能 virt_size > raw_size）。
        const std::uint32_t span = virt_size > raw_size ? virt_size : raw_size;
        if (rva >= virt_addr && rva < virt_addr + span) {
            return static_cast<std::size_t>(rva - virt_addr) + raw_ptr;
        }
    }
    return 0;
}

}  // namespace detail

/// 读取本地 PE 文件的导出名表（PE32 与 PE32+ 都支持）。任何异常路径都返回 false，不抛。
inline bool ReadExportNames(const std::wstring &path, std::vector<std::string> &out) {
    out.clear();
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        return false;
    }
    f.seekg(0, std::ios::end);
    const std::streamoff size = f.tellg();
    // 上限 2GB：op 的 dll 是 10~30MB 量级，超限说明读错了文件，直接放弃。
    if (size <= 0 || size > (static_cast<std::streamoff>(1) << 31)) {
        return false;
    }
    f.seekg(0, std::ios::beg);
    std::vector<unsigned char> buf(static_cast<std::size_t>(size));
    if (!f.read(reinterpret_cast<char *>(buf.data()), static_cast<std::streamsize>(size))) {
        return false;
    }

    if (buf.size() < 0x40 || detail::ReadU16(buf, 0) != 0x5A4D) {  // 'MZ'
        return false;
    }
    const std::uint32_t lfanew = detail::ReadU32(buf, 0x3C);
    if (lfanew + 24 > buf.size() || detail::ReadU32(buf, lfanew) != 0x00004550) {  // 'PE\0\0'
        return false;
    }
    const std::uint16_t section_count = detail::ReadU16(buf, lfanew + 6);
    const std::uint16_t opt_size = detail::ReadU16(buf, lfanew + 20);
    const std::size_t opt = static_cast<std::size_t>(lfanew) + 24;

    // 数据目录起点随 PE 魔数变化，写死会导致整表读错位。
    const std::uint16_t magic = detail::ReadU16(buf, opt);
    std::size_t data_dir = 0;
    if (magic == 0x10B) {  // PE32
        data_dir = opt + 96;
    } else if (magic == 0x20B) {  // PE32+
        data_dir = opt + 112;
    } else {
        return false;
    }

    const std::uint32_t export_rva = detail::ReadU32(buf, data_dir + 0);  // 目录项 0 = 导出表
    if (export_rva == 0) {
        return false;
    }
    const std::size_t sections = opt + opt_size;
    const std::size_t exp_off = detail::RvaToOffset(buf, sections, section_count, export_rva);
    if (exp_off == 0) {
        return false;
    }
    const std::uint32_t name_count = detail::ReadU32(buf, exp_off + 24);   // NumberOfNames
    const std::uint32_t names_rva = detail::ReadU32(buf, exp_off + 32);    // AddressOfNames
    const std::size_t names_off = detail::RvaToOffset(buf, sections, section_count, names_rva);
    if (names_off == 0) {
        return false;
    }

    for (std::uint32_t i = 0; i < name_count; ++i) {
        const std::uint32_t name_rva = detail::ReadU32(buf, names_off + i * 4);
        const std::size_t no = detail::RvaToOffset(buf, sections, section_count, name_rva);
        if (no == 0 || no >= buf.size()) {
            continue;
        }
        std::string s;
        while (no + s.size() < buf.size() && buf[no + s.size()] != 0) {
            s.push_back(static_cast<char>(buf[no + s.size()]));
        }
        if (!s.empty()) {
            out.push_back(std::move(s));
        }
    }
    return !out.empty();
}

/// 单个 dll 的缓存项。
///
/// `exports` 按 dll 缓存整张导出表：一次绑定要解析 2~11 个导出名，
/// 而 op 的 dll 是 10~30MB 量级，逐个名字重读文件会在绑定时产生几百毫秒抖动。
struct DllExportCache {
    bool loaded = false;  ///< 导出表是否已成功读入（失败保持 false，不落终值）
    std::vector<std::string> exports;
    std::map<std::string, std::string> resolved;  ///< plain -> 实际导出名
};

/// 导出名解析缓存。按 dll 路径分桶。
struct ExportNameCache {
    std::mutex mu;
    std::map<std::wstring, DllExportCache> by_path;
};

/// 仿 H15 风格刻意泄漏：避免静态析构顺序问题（本类型无析构副作用，但保持全库一致）。
inline ExportNameCache &export_name_cache() {
    static auto *cache = new ExportNameCache();
    return *cache;
}

/// 把 `plain` 解析成目标 dll 里实际存在的导出名（带缓存）。
///
/// 读不到文件 / 无导出表时**不缓存结果**并原样返回 —— 失败不落成终值，
/// 否则一次瞬时读取失败（例如 dll 尚未落盘）会让整个进程后续都用不到正确名字。
inline std::string ResolveRemoteExportName(const std::wstring &dll_path, const char *plain) {
    if (plain == nullptr || *plain == '\0') {
        return std::string();
    }
    const std::string key(plain);
    ExportNameCache &cache = export_name_cache();
    std::lock_guard<std::mutex> lock(cache.mu);
    DllExportCache &entry = cache.by_path[dll_path];
    if (!entry.loaded) {
        if (!ReadExportNames(dll_path, entry.exports)) {
            return key;
        }
        entry.loaded = true;
    }

    const auto it = entry.resolved.find(key);
    if (it != entry.resolved.end()) {
        return it->second;
    }
    const std::string resolved = PickRemoteExportName(entry.exports, key);
    entry.resolved.emplace(key, resolved);
    if (resolved != key) {
        // 只在缓存未命中且确实发生了改写时打一条：真机取证要能看到解析结果，
        // 但这段可能被 ping/光标轮询反复调用，不能每帧刷日志。
        setlog(L"hook export name resolved: %S -> %S (dll=%s)", key.c_str(), resolved.c_str(),
               dll_path.c_str());
    }
    return resolved;
}

}  // namespace op::hook
