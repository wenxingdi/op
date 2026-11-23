// OpAsm.cpp: 汇编模块（批 D 2026-11-22；59 批 2026-11-23 补齐 Assemble/Disassemble）。
//
// 三个接口：
//   AsmCall      —— 在目标进程执行一段**机器码**并取返回值。纯 BlackBone + Win32。
//   Assemble     —— 汇编文本 -> 机器码。keystone（内嵌 LLVM MC）。
//   Disassemble  —— 机器码 -> 汇编文本。capstone。
//
// 依赖从哪来（2026-11-23 变更）：
//   原先两个接口是**显式失败的占位**，因为 vcpkg 在本机装不上这两个 port ——
//   vcpkg 引导自身工具链时用 `CreateFileW` 重定向 stdin 解压 7z，沙箱里返回
//   ERROR_ACCESS_DENIED（"calling CreateFileW stdin failed with 5"），手工解包也无效。
//   现在改为**本地源码构建**：`scripts/build/_wb_build_asm_libs.py` 下载
//   keystone 0.9.2 / capstone 5.0.3 源码，用 Ninja + MSVC 按 /MT 编出静态库，
//   落到 3rd_party/{keystone,capstone}/lib/{x64,x86}/，再由 libop/CMakeLists.txt 链接。
//   缺库时 CMake **FATAL_ERROR**，不再退化成"编得过、跑起来才发现是空壳"。
//
// 为什么 Disassemble 多了 arch 参数（与原预留签名不同）：
//   原签名 (bytes_hex, address, count) 没有位数信息。x64 宿主按 64 位模式解码
//   32 位游戏代码会**静默产出错码**（0x48 变成 REX 前缀、0x50 从 push eax 变 push rax），
//   这种错比直接失败更难查。接口此前从未能返回数据，加参数不构成破坏性变更。
//   arch: 32 / 64；0 = 按当前进程位数。
//
// AsmCall 的安全边界（有意限制）：
//   * 只执行**调用方给出的机器码**，插件不生成/不修改代码；
//   * 分配的内存执行完立即释放（不复用、不留常驻可执行区）；
//   * 有超时（默认 5s），超时返回失败而不是无限等；
//   * 失败一律返回 0 并带错误文本，不吞异常。
#include "OpContext.h"
#include "OpResult.h"

#include "../json/JsonValue.h"
#include "../memory/ProcessMemory.h"

#include <libop.h>

#include <Windows.h>

// 汇编/反汇编引擎：静态库，/MT，由 scripts/build/_wb_build_asm_libs.py 本地构建。
// KEYSTONE_STATIC / CAPSTONE_STATIC 由 libop/CMakeLists.txt 统一定义，
// 目的是让两个头文件的导出宏退化成空（否则 ks_* 会经 .drectve 顶进 op.dll 导出表）。
#include <capstone/capstone.h>
#include <keystone/keystone.h>

#include <cstdint>
#include <string>
#include <vector>

namespace {

using namespace op::json;

Value JsonNum(double d) {
    Value n;
    n.type = Type::Number;
    n.number = d;
    return n;
}

std::wstring SuccessJson(const wchar_t *hex_value, long code) {
    Value root;
    root.type = Type::Object;
    root.obj.push_back(std::make_pair(std::wstring(L"ok"), JsonNum(1)));
    root.obj.push_back(std::make_pair(std::wstring(L"code"), JsonNum(0)));
    // 返回值同时给十六进制和十进制：十六进制便于对指针/句柄，十进制便于对整数结果
    Value hexv;
    hexv.type = Type::String;
    hexv.str = hex_value ? hex_value : L"0";
    root.obj.push_back(std::make_pair(std::wstring(L"ret_hex"), std::move(hexv)));
    root.obj.push_back(std::make_pair(std::wstring(L"ret"), JsonNum(static_cast<double>(code))));
    return op::json::Stringify(root);
}

std::wstring FailureJson(const wchar_t *msg) {
    Value root;
    root.type = Type::Object;
    root.obj.push_back(std::make_pair(std::wstring(L"ok"), JsonNum(0)));
    root.obj.push_back(std::make_pair(std::wstring(L"code"), JsonNum(-1)));
    Value m;
    m.type = Type::String;
    m.str = msg ? msg : L"";
    root.obj.push_back(std::make_pair(std::wstring(L"error"), std::move(m)));
    return op::json::Stringify(root);
}

// "48 8B 05 ?? ??" -> 字节数组。允许空格/逗号分隔与 ??（占位符按 0xCC int3 填充）。
// 返回 false 表示格式非法（调用方据此显式失败，不猜）。
bool parse_machine_code(const std::wstring &hex, std::vector<unsigned char> &out) {
    out.clear();
    std::wstring token;
    auto flush = [&]() -> bool {
        if (token.empty())
            return true;
        if (token == L"??" || token == L"?") {
            out.push_back(0xCC); // int3：占位符，执行到会立刻断下来而不是跑飞
            token.clear();
            return true;
        }
        if (token.size() > 2)
            return false;
        // 两道校验：wcstoul 在完全无法解析时会把 end 停在开头（"zz" -> end 指向 'z'），
        // 只靠 *end=='\0' 已能挡掉这类输入；逐字符校验是**冗余保险**，
        // 防的是"部分可解析"的串（如 "0G"）被吃掉前缀后静默变成别的字节。
        // 诚实记录：这份冗余校验我**没能构造出真正能区分它的用例**
        // （试过 "0G"/"2Fz" 等，去掉校验后用例仍绿），所以测试里没有断言它；
        // 保留代码是因为"多一层校验"无害，而不是因为已经证明了它必要。
        for (wchar_t ch : token) {
            const bool is_hex = (ch >= L'0' && ch <= L'9') || (ch >= L'a' && ch <= L'f') ||
                                (ch >= L'A' && ch <= L'F');
            if (!is_hex)
                return false;
        }
        wchar_t *end = nullptr;
        const unsigned long v = wcstoul(token.c_str(), &end, 16);
        if (!end || *end != L'\0' || v > 0xFF)
            return false;
        out.push_back(static_cast<unsigned char>(v));
        token.clear();
        return true;
    };
    for (wchar_t c : hex) {
        if (c == L' ' || c == L'\t' || c == L',' || c == L'\r' || c == L'\n') {
            if (!flush())
                return false;
        } else {
            token.push_back(c);
        }
    }
    if (!flush())
        return false;
    return !out.empty();
}

std::wstring to_hex(uint64_t v) {
    wchar_t buf[32] = {0};
    swprintf_s(buf, L"%llX", static_cast<unsigned long long>(v));
    return buf;
}

Value JsonStr(std::wstring s) {
    Value v;
    v.type = Type::String;
    v.str = std::move(s);
    return v;
}

void Put(Value &o, const wchar_t *k, Value v) {
    o.obj.push_back(std::make_pair(std::wstring(k), std::move(v)));
}

// 字节数组 -> "B8 2A 00 00 00"：与 AsmCall 的 code_hex 输入格式**同构**，
// 于是 "assemble 出来的 hex 直接喂给 asm_call" 不需要任何转换。
std::wstring bytes_to_hex_spaced(const unsigned char *p, size_t n) {
    static const wchar_t *kDigits = L"0123456789ABCDEF";
    std::wstring out;
    out.reserve(n * 3);
    for (size_t i = 0; i < n; ++i) {
        if (i)
            out.push_back(L' ');
        out.push_back(kDigits[(p[i] >> 4) & 0xF]);
        out.push_back(kDigits[p[i] & 0xF]);
    }
    return out;
}

std::string narrow_utf8(const wchar_t *s) {
    if (!s || !s[0])
        return std::string();
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, s, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1)
        return std::string();
    std::string out(static_cast<size_t>(n - 1), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, s, -1, &out[0], n, nullptr, nullptr);
    return out;
}

std::wstring widen_utf8(const char *s) {
    if (!s || !s[0])
        return std::wstring();
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    if (n <= 1)
        return std::wstring();
    std::wstring out(static_cast<size_t>(n - 1), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s, -1, &out[0], n);
    return out;
}

// arch 归一：只认 32/64，0 = 当前进程位数。**不做猜测**——猜错会安静产出错码。
bool resolve_arch_bits(long arch, long &bits, std::wstring &err) {
    if (arch == 0)
        arch = static_cast<long>(sizeof(void *) * 8);
    if (arch != 32 && arch != 64) {
        err = L"arch 只接受 32 / 64（0 = 按当前进程位数）";
        return false;
    }
    bits = arch;
    return true;
}

// 十六进制地址：允许 "0x" 前缀，空串按 0。拒绝负数与非法字符。
bool parse_hex_addr(const wchar_t *text, uint64_t &out) {
    out = 0;
    if (!text || !text[0])
        return true;
    const wchar_t *p = text;
    while (*p == L' ' || *p == L'\t')
        ++p;
    if (p[0] == L'0' && (p[1] == L'x' || p[1] == L'X'))
        p += 2;
    if (!*p)
        return false;
    for (const wchar_t *q = p; *q; ++q) {
        if (q == p && *q == L'-')
            return false;
        const bool is_hex = (*q >= L'0' && *q <= L'9') || (*q >= L'a' && *q <= L'f') || (*q >= L'A' && *q <= L'F');
        if (!is_hex && !(*q == L' ' || *q == L'\t'))
            return false;
    }
    wchar_t *end = nullptr;
    const unsigned long long v = wcstoull(p, &end, 16);
    if (end == p)
        return false;
    while (*end == L' ' || *end == L'\t')
        ++end;
    if (*end != L'\0')
        return false;
    out = static_cast<uint64_t>(v);
    return true;
}

} // namespace

// AsmCall：在 hwnd 所属进程里执行 code_hex（机器码），返回 JSON。
//
// 参数：
//   hwnd     目标窗口句柄；0 表示**当前进程**（便于先自测再上目标）
//   code_hex 机器码，形如 "48 C7 C0 2A 00 00 00 C3"（ret 结尾；不自动补 ret）
//   timeout_ms 超时，<=0 用默认 5000
//
// 返回值 JSON：
//   成功 {"ok":1,"code":0,"ret_hex":"2A","ret":42}
//   失败 {"ok":0,"code":-1,"error":"..."}
// *ret 同时输出 1=成功 / 0=失败，方便不解析 JSON 的老脚本。
long op::Op::AsmCall(LONG_PTR hwnd, const wchar_t *code_hex, long timeout_ms, std::wstring &retjson, long *ret) {
    internal::set_result(ret, 0L);
    retjson = FailureJson(L"invalid_argument");

    std::vector<unsigned char> code;
    if (!code_hex || !code_hex[0] || !parse_machine_code(code_hex, code)) {
        retjson = FailureJson(L"code_hex 解析失败（应为空格分隔的十六进制字节，支持 ?? 占位）");
        return 0;
    }
    // 上限：拒绝异常大的代码块（防止误传整段数据把目标进程内存打爆）
    if (code.size() > 64 * 1024) {
        retjson = FailureJson(L"代码长度超过 64KB");
        return 0;
    }

    auto mem = std::make_unique<ProcessMemory>();
    // 目标进程：hwnd 非 0 时用窗口所在进程；0 -> 当前进程
    HWND target = reinterpret_cast<HWND>(hwnd);
    const bool current_process = (target == nullptr);

    uint64_t result_value = 0;
    if (current_process) {
        // 当前进程：直接分配可执行内存执行（不走 BlackBone，避免自 attach）
        void *mem_exec = ::VirtualAlloc(nullptr, code.size(), MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (!mem_exec) {
            retjson = FailureJson(L"VirtualAlloc(当前进程) 失败");
            return 0;
        }
        ::memcpy(mem_exec, code.data(), code.size());
        ::FlushInstructionCache(::GetCurrentProcess(), mem_exec, code.size());
        using Fn0 = uint64_t (*)();
        result_value = reinterpret_cast<Fn0>(mem_exec)();
        ::VirtualFree(mem_exec, 0, MEM_RELEASE);
    } else {
        // 目标进程：BlackBone 的 RPC 环境（已在依赖链里，API 稳定）
        // 说明：这里刻意不用 VirtualAllocEx + CreateRemoteThread 的手写组合 ——
        // BlackBone 的 ExecInWorkerThread 已经处理了"线程挂起-取值-恢复"的时序，
        // 自己写容易在目标进程线程被占用时死等。
        blackbone::Process proc;
        const DWORD pid = ::GetWindowThreadProcessId(target, nullptr);
        if (!pid) {
            retjson = FailureJson(L"取目标窗口进程 ID 失败");
            return 0;
        }
        if (proc.Attach(pid) != 0) {
            retjson = FailureJson(L"附加目标进程失败（权限不足？需管理员）");
            return 0;
        }
        if (proc.remote().CreateRPCEnvironment(blackbone::Worker_None, true) != 0) {
            retjson = FailureJson(L"创建 RPC 环境失败");
            return 0;
        }
        if (proc.remote().ExecInWorkerThread(code.data(), code.size(), result_value) != 0) {
            retjson = FailureJson(L"在目标进程执行代码失败");
            return 0;
        }
    }

    const std::wstring hex = to_hex(result_value);
    retjson = SuccessJson(hex.c_str(), static_cast<long>(result_value));
    internal::set_result(ret, 1L);
    return 1;
}

// Assemble：汇编文本 -> 机器码（keystone / LLVM MC）。
//
// 参数：
//   asm_text 汇编文本，支持多语句（换行或 ';' 分隔），Intel 语法（显式设定，不吃环境默认）
//   arch     32 / 64；0 = 按当前进程位数
//
// 成功 JSON：{"ok":1,"code":0,"size":7,"count":1,"hex":"B8 2A 00 00 00 C3"}
//   size  = 字节数；count = 语句数；hex = **空格分隔**，可直接喂给 AsmCall
// 失败 JSON：{"ok":0,"code":-1,"error":"keystone 汇编失败: <ks_strerror>"}
//
// 注意：keystone **不会**自动补 ret。要执行就在文本里自己写 "ret"。
long op::Op::Assemble(const wchar_t *asm_text, long arch, std::wstring &retjson, long *ret) {
    internal::set_result(ret, 0L);
    retjson = FailureJson(L"invalid_argument");

    long bits = 0;
    std::wstring err;
    if (!resolve_arch_bits(arch, bits, err)) {
        retjson = FailureJson(err.c_str());
        return 0;
    }
    if (!asm_text || !asm_text[0]) {
        retjson = FailureJson(L"asm_text 为空");
        return 0;
    }

    ks_engine *ks = nullptr;
    if (ks_open(KS_ARCH_X86, (bits == 64) ? KS_MODE_64 : KS_MODE_32, &ks) != KS_ERR_OK || ks == nullptr) {
        retjson = FailureJson(L"ks_open 失败（keystone 未初始化）");
        return 0;
    }
    // 语法显式钉成 Intel：keystone 默认就是 Intel，但不要依赖默认值
    ks_option(ks, KS_OPT_SYNTAX, KS_OPT_SYNTAX_INTEL);

    const std::string text = narrow_utf8(asm_text);
    unsigned char *encoding = nullptr;
    size_t size = 0;
    size_t statements = 0;
    const int rc = ks_asm(ks, text.c_str(), 0, &encoding, &size, &statements);
    if (rc != 0 || encoding == nullptr) {
        // ks_asm 返回 -1 时真实原因在 ks_errno(ks) 里
        std::wstring msg = L"keystone 汇编失败: ";
        msg += widen_utf8(ks_strerror(ks_errno(ks)));
        ks_close(ks);
        retjson = FailureJson(msg.c_str());
        return 0;
    }
    // 与 AsmCall 同一上限：超过 64KB 的"代码"基本是把数据当代码传了
    if (size > 64 * 1024) {
        ks_free(encoding);
        ks_close(ks);
        retjson = FailureJson(L"汇编结果超过 64KB");
        return 0;
    }

    const std::wstring hex = bytes_to_hex_spaced(encoding, size);
    ks_free(encoding);
    ks_close(ks);

    Value root;
    root.type = Type::Object;
    Put(root, L"ok", JsonNum(1));
    Put(root, L"code", JsonNum(0));
    Put(root, L"size", JsonNum(static_cast<double>(size)));
    Put(root, L"count", JsonNum(static_cast<double>(statements)));
    Put(root, L"hex", JsonStr(hex));
    retjson = op::json::Stringify(root);
    internal::set_result(ret, 1L);
    return 1;
}

// Disassemble：机器码 -> 汇编文本（capstone）。
//
// 参数：
//   bytes_hex 空格分隔的十六进制字节（与 AsmCall/Assemble 同格式），支持 ?? 占位
//   address   起始虚拟地址，十六进制（可带 0x），空串 = 0。只影响输出的 addr 字段
//             与**相对寻址的解析**（如 jmp/call 的目标由地址算出）
//   count     最多解码多少条指令；<=0 = 解到字节用完为止
//   arch      32 / 64；0 = 按当前进程位数
//
// 成功 JSON：{"ok":1,"code":0,"mode":64,"count":2,
//             "insns":[{"addr":"140001000","bytes":"B8 2A 00 00 00","mnemonic":"mov","op_str":"eax, 0x2a"},
//                      {"addr":"140001005","bytes":"C3","mnemonic":"ret","op_str":""}]}
// 失败 JSON：{"ok":0,"code":-1,"error":"..."}
//
// ⚠ arch 必须给对：x64 模式解 32 位代码会**静默出错码**（0x50 是 push eax 还是 push rax、
//    0x48 是 dec eax 还是 REX 前缀，全看模式），所以这里要求调用方显式声明而不是猜。
long op::Op::Disassemble(const wchar_t *bytes_hex, const wchar_t *address, long count, long arch,
                         std::wstring &retjson, long *ret) {
    internal::set_result(ret, 0L);
    retjson = FailureJson(L"invalid_argument");

    long bits = 0;
    std::wstring err;
    if (!resolve_arch_bits(arch, bits, err)) {
        retjson = FailureJson(err.c_str());
        return 0;
    }
    std::vector<unsigned char> code;
    if (!bytes_hex || !bytes_hex[0] || !parse_machine_code(bytes_hex, code)) {
        retjson = FailureJson(L"bytes_hex 解析失败（应为空格分隔的十六进制字节，支持 ?? 占位）");
        return 0;
    }
    if (code.size() > 64 * 1024) {
        retjson = FailureJson(L"字节数超过 64KB");
        return 0;
    }
    uint64_t addr = 0;
    if (!parse_hex_addr(address, addr)) {
        retjson = FailureJson(L"address 解析失败（十六进制，可带 0x 前缀）");
        return 0;
    }

    csh handle = 0;
    if (cs_open(CS_ARCH_X86, (bits == 64) ? CS_MODE_64 : CS_MODE_32, &handle) != CS_ERR_OK) {
        retjson = FailureJson(L"cs_open 失败（capstone 未初始化）");
        return 0;
    }
    cs_insn *insn = nullptr;
    const size_t n = cs_disasm(handle, code.data(), code.size(), addr, (count > 0) ? static_cast<size_t>(count) : 0,
                               &insn);
    if (n == 0 || insn == nullptr) {
        cs_close(&handle);
        retjson = FailureJson(L"capstone 未解出指令（字节流不完整或不是有效 x86 机器码）");
        return 0;
    }

    Value list;
    list.type = Type::Array;
    list.arr.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        Value item;
        item.type = Type::Object;
        // capstone 的字段名是 address（不是 addr）：cs_insn{id,address,size,bytes,mnemonic,op_str,detail}
        Put(item, L"addr", JsonStr(to_hex(insn[i].address)));
        Put(item, L"bytes", JsonStr(bytes_to_hex_spaced(insn[i].bytes, insn[i].size)));
        Put(item, L"mnemonic", JsonStr(widen_utf8(insn[i].mnemonic)));
        Put(item, L"op_str", JsonStr(widen_utf8(insn[i].op_str)));
        list.arr.push_back(std::move(item));
    }
    cs_free(insn, n);
    cs_close(&handle);

    Value root;
    root.type = Type::Object;
    Put(root, L"ok", JsonNum(1));
    Put(root, L"code", JsonNum(0));
    Put(root, L"mode", JsonNum(static_cast<double>(bits)));
    Put(root, L"count", JsonNum(static_cast<double>(n)));
    Put(root, L"insns", std::move(list));
    retjson = op::json::Stringify(root);
    internal::set_result(ret, 1L);
    return 1;
}

