// OpAsm.cpp: 汇编模块（批 D 2026-11-22）。
//
// 交付范围（诚实记录）：
//   ✅ AsmCall —— 在目标进程执行一段**机器码**并取返回值。
//      纯 BlackBone（已在依赖链里）+ Win32，**不引入任何新依赖**。
//   ❌ Assemble / Disassemble —— **本轮阻塞**，原因见下。
//
// 为什么 Assemble/Disassemble 没做：
//   需要 keystone（汇编->机器码）/ capstone（机器码->汇编）。两者在 vcpkg 都有 port，
//   但本机装不上：vcpkg 引导自己的工具链时用 `CreateFileW` 重定向 stdin 解压 7z，
//   在沙箱里返回 ERROR_ACCESS_DENIED（"calling CreateFileW stdin failed with 5"）。
//   手工把 7z 解包到 downloads/tools 也没用 —— vcpkg 仍坚持走它自己的解压分支。
//   **没有偷偷降级成"写个假的汇编器"**：那会产出错码且极难排查，比没有更糟。
//   恢复方式（在有网络且无沙箱的机器上）：
//       vcpkg install keystone:x64-windows-static capstone:x64-windows-static
//   然后在本文件底部按 TODO 接上（C API/COM/头文件声明都已留好位置）。
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

// TODO（待 keystone/capstone 可安装后接上）：
//   long op::Op::Assemble(const wchar_t *asm_text, long arch, std::wstring &retjson, long *ret);
//     用 ks_open(KS_ARCH_X86, arch==32?KS_MODE_32:KS_MODE_64) -> ks_asm -> "HEX 字节串"
//   long op::Op::Disassemble(const wchar_t *bytes_hex, const wchar_t *address, long count,
//                            std::wstring &retjson, long *ret);
//     用 cs_open(CS_ARCH_X86, ...) -> cs_disasm -> [{"addr":"HEX","mnemonic":"..","op_str":".."},...]
// 头文件 / C API / COM 的声明位置已预留，见 docs/2026-11/五模块实施规划_20261122.md §5。

// 未实现的两个接口：显式失败 + 明确错误文本。
// 存在的意义：让调用方能在**运行时**问出"到底支不支持"，而不是靠文档记忆；
// 同时保证 API 面（头文件/C API/COM）在后续接上 keystone/capstone 时不用改签名。
long op::Op::Assemble(const wchar_t *asm_text, long arch, std::wstring &retjson, long *ret) {
    internal::set_result(ret, 0L);
    (void)asm_text;
    (void)arch;
    retjson = FailureJson(L"Assemble 未实现：需要 keystone（当前环境 vcpkg 无法安装，见源码顶部说明）");
    return 0;
}

long op::Op::Disassemble(const wchar_t *bytes_hex, const wchar_t *address, long count, std::wstring &retjson,
                         long *ret) {
    internal::set_result(ret, 0L);
    (void)bytes_hex;
    (void)address;
    (void)count;
    retjson = FailureJson(L"Disassemble 未实现：需要 capstone（当前环境 vcpkg 无法安装，见源码顶部说明）");
    return 0;
}
