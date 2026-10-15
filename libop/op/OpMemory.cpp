#include "OpContext.h"
#include "OpResult.h"

#include "memory/ProcessMemory.h"

#include <libop.h>

#include <Windows.h>
#include <cstdint>
#include <string>
#include <utility>

namespace {

static LONG_PTR resolve_memory_hwnd(op::Op *self, LONG_PTR hwnd) {
    if (hwnd != 0)
        return hwnd;
    // hwnd=0 的语义：已绑定时作用于绑定窗口（与图色/键鼠一致），未绑定才读本进程。
    // 注意这与 ProcessMemory 头注释"空=当前进程"不同——那是底层视角，这里以 Op 层为准。
    LONG_PTR bind_hwnd = 0;
    self->GetBindWindow(&bind_hwnd);
    return bind_hwnd;
}

// 公共骨架（第十二批 OP4）：hwnd 归一 + ProcessMemory 临时对象 + 异常兜底。
// 各包装函数只留参数守卫与业务调用；异常路径返回 dflt，
// 与各函数原有 catch 分支的复位行为一致。
template <typename T, typename Fn>
T memory_call(op::Op *self, LONG_PTR hwnd, T dflt, Fn &&fn) {
    hwnd = resolve_memory_hwnd(self, hwnd);
    try {
        op::ProcessMemory mem;
        return fn(mem, reinterpret_cast<HWND>(static_cast<LONG_PTR>(hwnd)));
    } catch (...) {
        return dflt;
    }
}

} // namespace

void op::Op::WriteData(LONG_PTR hwnd, const wchar_t *address, const wchar_t *data, long size, long *ret) {
    internal::set_result(ret, 0L);
    if (!ret || !address || !data || size < 0)
        return;
    internal::set_result(ret, memory_call(this, hwnd, 0L, [&](ProcessMemory &mem, HWND target) {
        return mem.WriteData(target, address, data, size);
    }));
}
// 读取数据
void op::Op::ReadData(LONG_PTR hwnd, const wchar_t *address, long size, std::wstring &retstr) {
    retstr.clear();
    if (!address || size < 0)
        return;
    retstr = memory_call(this, hwnd, std::wstring(), [&](ProcessMemory &mem, HWND target) {
        return mem.ReadData(target, address, size);
    });
}

void op::Op::ReadInt(LONG_PTR hwnd, const wchar_t *address, long type, int64_t *ret) {
    internal::set_result(ret, 0);
    if (!address || !ret)
        return;
    internal::set_result(ret, memory_call(this, hwnd, int64_t{0}, [&](ProcessMemory &mem, HWND target) {
        return mem.ReadInt(target, address, type);
    }));
}

void op::Op::WriteInt(LONG_PTR hwnd, const wchar_t *address, long type, int64_t value, long *ret) {
    internal::set_result(ret, 0L);
    if (!address || !ret)
        return;
    internal::set_result(ret, memory_call(this, hwnd, 0L, [&](ProcessMemory &mem, HWND target) {
        return mem.WriteInt(target, address, type, value);
    }));
}

void op::Op::ReadFloat(LONG_PTR hwnd, const wchar_t *address, float *ret) {
    internal::set_result(ret, 0.0f);
    if (!address || !ret)
        return;
    internal::set_result(ret, memory_call(this, hwnd, 0.0f, [&](ProcessMemory &mem, HWND target) {
        return mem.ReadFloat(target, address);
    }));
}

void op::Op::WriteFloat(LONG_PTR hwnd, const wchar_t *address, float value, long *ret) {
    internal::set_result(ret, 0L);
    if (!address || !ret)
        return;
    internal::set_result(ret, memory_call(this, hwnd, 0L, [&](ProcessMemory &mem, HWND target) {
        return mem.WriteFloat(target, address, value);
    }));
}

void op::Op::ReadDouble(LONG_PTR hwnd, const wchar_t *address, double *ret) {
    internal::set_result(ret, 0.0);
    if (!address || !ret)
        return;
    internal::set_result(ret, memory_call(this, hwnd, 0.0, [&](ProcessMemory &mem, HWND target) {
        return mem.ReadDouble(target, address);
    }));
}

void op::Op::WriteDouble(LONG_PTR hwnd, const wchar_t *address, double value, long *ret) {
    internal::set_result(ret, 0L);
    if (!address || !ret)
        return;
    internal::set_result(ret, memory_call(this, hwnd, 0L, [&](ProcessMemory &mem, HWND target) {
        return mem.WriteDouble(target, address, value);
    }));
}

void op::Op::ReadString(LONG_PTR hwnd, const wchar_t *address, long type, long len, std::wstring &retstr) {
    retstr.clear();
    if (!address)
        return;
    retstr = memory_call(this, hwnd, std::wstring(), [&](ProcessMemory &mem, HWND target) {
        return mem.ReadString(target, address, type, len);
    });
}

void op::Op::WriteString(LONG_PTR hwnd, const wchar_t *address, long type, const wchar_t *value, long *ret) {
    internal::set_result(ret, 0L);
    if (!address || !value || !ret)
        return;
    internal::set_result(ret, memory_call(this, hwnd, 0L, [&](ProcessMemory &mem, HWND target) {
        return mem.WriteString(target, address, type, value);
    }));
}

void op::Op::FindData(LONG_PTR hwnd, const wchar_t *addr_range, const wchar_t *string, std::wstring &retstr) {
    FindDataEx(hwnd, addr_range, string, 1, 0, retstr);
}

void op::Op::FindDataEx(LONG_PTR hwnd, const wchar_t *addr_range, const wchar_t *string, long step, long count,
                        std::wstring &retstr) {
    retstr.clear();
    if (!string)
        return;
    retstr = memory_call(this, hwnd, std::wstring(), [&](ProcessMemory &mem, HWND target) {
        return mem.FindData(target, addr_range ? addr_range : L"", string, step, count);
    });
}

void op::Op::GetModuleBaseAddr(LONG_PTR hwnd, const wchar_t *module, std::wstring &retstr) {
    retstr.clear();
    if (!module || !*module)
        return;
    retstr = memory_call(this, hwnd, std::wstring(), [&](ProcessMemory &mem, HWND target) {
        return mem.GetModuleBaseAddr(target, module);
    });
}
