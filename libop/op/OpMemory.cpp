#include "OpContext.h"
#include "OpResult.h"

#include "memory/ProcessMemory.h"

#include <libop.h>

#include <Windows.h>
#include <cstdint>
#include <string>

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

} // namespace

void op::Op::WriteData(LONG_PTR hwnd, const wchar_t *address, const wchar_t *data, long size, long *ret) {
    internal::set_result(ret, 0L);
    if (!ret || !address || !data || size < 0)
        return;
    hwnd = resolve_memory_hwnd(this, hwnd);
    try {
        ProcessMemory mem;
        internal::set_result(ret, mem.WriteData(reinterpret_cast<HWND>(static_cast<LONG_PTR>(hwnd)), address, data, size));
    } catch (...) {
        internal::set_result(ret, 0L);
    }
}
// 读取数据
void op::Op::ReadData(LONG_PTR hwnd, const wchar_t *address, long size, std::wstring &retstr) {
    retstr.clear();
    if (!address || size < 0)
        return;
    hwnd = resolve_memory_hwnd(this, hwnd);
    try {
        ProcessMemory mem;
        retstr = mem.ReadData(reinterpret_cast<HWND>(static_cast<LONG_PTR>(hwnd)), address, size);
    } catch (...) {
        retstr.clear();
    }
}

void op::Op::ReadInt(LONG_PTR hwnd, const wchar_t *address, long type, int64_t *ret) {
    internal::set_result(ret, 0);
    if (!address || !ret)
        return;
    hwnd = resolve_memory_hwnd(this, hwnd);
    try {
        ProcessMemory mem;
        internal::set_result(ret, mem.ReadInt(reinterpret_cast<HWND>(static_cast<LONG_PTR>(hwnd)), address, type));
    } catch (...) {
        internal::set_result(ret, 0);
    }
}

void op::Op::WriteInt(LONG_PTR hwnd, const wchar_t *address, long type, int64_t value, long *ret) {
    internal::set_result(ret, 0L);
    if (!address || !ret)
        return;
    hwnd = resolve_memory_hwnd(this, hwnd);
    try {
        ProcessMemory mem;
        internal::set_result(ret, mem.WriteInt(reinterpret_cast<HWND>(static_cast<LONG_PTR>(hwnd)), address, type, value));
    } catch (...) {
        internal::set_result(ret, 0L);
    }
}

void op::Op::ReadFloat(LONG_PTR hwnd, const wchar_t *address, float *ret) {
    internal::set_result(ret, 0.0f);
    if (!address || !ret)
        return;
    hwnd = resolve_memory_hwnd(this, hwnd);
    try {
        ProcessMemory mem;
        internal::set_result(ret, mem.ReadFloat(reinterpret_cast<HWND>(static_cast<LONG_PTR>(hwnd)), address));
    } catch (...) {
        internal::set_result(ret, 0.0f);
    }
}

void op::Op::WriteFloat(LONG_PTR hwnd, const wchar_t *address, float value, long *ret) {
    internal::set_result(ret, 0L);
    if (!address || !ret)
        return;
    hwnd = resolve_memory_hwnd(this, hwnd);
    try {
        ProcessMemory mem;
        internal::set_result(ret, mem.WriteFloat(reinterpret_cast<HWND>(static_cast<LONG_PTR>(hwnd)), address, value));
    } catch (...) {
        internal::set_result(ret, 0L);
    }
}

void op::Op::ReadDouble(LONG_PTR hwnd, const wchar_t *address, double *ret) {
    internal::set_result(ret, 0.0);
    if (!address || !ret)
        return;
    hwnd = resolve_memory_hwnd(this, hwnd);
    try {
        ProcessMemory mem;
        internal::set_result(ret, mem.ReadDouble(reinterpret_cast<HWND>(static_cast<LONG_PTR>(hwnd)), address));
    } catch (...) {
        internal::set_result(ret, 0.0);
    }
}

void op::Op::WriteDouble(LONG_PTR hwnd, const wchar_t *address, double value, long *ret) {
    internal::set_result(ret, 0L);
    if (!address || !ret)
        return;
    hwnd = resolve_memory_hwnd(this, hwnd);
    try {
        ProcessMemory mem;
        internal::set_result(ret, mem.WriteDouble(reinterpret_cast<HWND>(static_cast<LONG_PTR>(hwnd)), address, value));
    } catch (...) {
        internal::set_result(ret, 0L);
    }
}

void op::Op::ReadString(LONG_PTR hwnd, const wchar_t *address, long type, long len, std::wstring &retstr) {
    retstr.clear();
    if (!address)
        return;
    hwnd = resolve_memory_hwnd(this, hwnd);
    try {
        ProcessMemory mem;
        retstr = mem.ReadString(reinterpret_cast<HWND>(static_cast<LONG_PTR>(hwnd)), address, type, len);
    } catch (...) {
        retstr.clear();
    }
}

void op::Op::WriteString(LONG_PTR hwnd, const wchar_t *address, long type, const wchar_t *value, long *ret) {
    internal::set_result(ret, 0L);
    if (!address || !value || !ret)
        return;
    hwnd = resolve_memory_hwnd(this, hwnd);
    try {
        ProcessMemory mem;
        internal::set_result(ret,
                             mem.WriteString(reinterpret_cast<HWND>(static_cast<LONG_PTR>(hwnd)), address, type, value));
    } catch (...) {
        internal::set_result(ret, 0L);
    }
}

void op::Op::FindData(LONG_PTR hwnd, const wchar_t *addr_range, const wchar_t *string, std::wstring &retstr) {
    FindDataEx(hwnd, addr_range, string, 1, 0, retstr);
}

void op::Op::FindDataEx(LONG_PTR hwnd, const wchar_t *addr_range, const wchar_t *string, long step, long count,
                        std::wstring &retstr) {
    retstr.clear();
    if (!string)
        return;
    hwnd = resolve_memory_hwnd(this, hwnd);
    try {
        ProcessMemory mem;
        retstr = mem.FindData(reinterpret_cast<HWND>(static_cast<LONG_PTR>(hwnd)), addr_range ? addr_range : L"",
                              string, step, count);
    } catch (...) {
        retstr.clear();
    }
}

void op::Op::GetModuleBaseAddr(LONG_PTR hwnd, const wchar_t *module, std::wstring &retstr) {
    retstr.clear();
    if (!module || !*module)
        return;
    hwnd = resolve_memory_hwnd(this, hwnd);
    try {
        ProcessMemory mem;
        retstr = mem.GetModuleBaseAddr(reinterpret_cast<HWND>(static_cast<LONG_PTR>(hwnd)), module);
    } catch (...) {
        retstr.clear();
    }
}
