#include "OpContext.h"
#include "OpResult.h"

#include "base/Environment.h"
#include "base/Utils.h"

#include <libop.h>
#include <Shlwapi.h>
#include <tchar.h>

#include <cstdio>
#include <random>

namespace {
// 进程级共享的随机引擎: 用 random_device 播种一次, 保证多实例不取同一序列
std::mt19937 &Rng() {
    static std::mt19937 rng{std::random_device{}()};
    return rng;
}
} // namespace

std::wstring op::Op::Ver() {
    return _T(OP_VERSION);
}

void op::Op::SetPath(const wchar_t *path, long *ret) {
    std::wstring fpath = path;
    replacew(fpath, L"/", L"\\");
    if (fpath.find(L'\\') != std::wstring::npos && ::PathFileExistsW(fpath.data())) {
        m_context->curr_path = fpath;
        m_context->image_proc._curr_path = m_context->curr_path;
        m_context->bkproc._curr_path = m_context->curr_path;
        internal::set_result(ret, 1L);
    } else {

        if (!fpath.empty() && fpath[0] != L'\\')
            fpath = m_context->curr_path + L'\\' + fpath;
        else
            fpath = m_context->curr_path + fpath;
        if (::PathFileExistsW(fpath.data())) {
            m_context->curr_path = path;
            m_context->image_proc._curr_path = m_context->curr_path;
            m_context->bkproc._curr_path = m_context->curr_path;
            internal::set_result(ret, 1L);
        } else {
            setlog("path '%s' not exists", fpath.data());
            internal::set_result(ret, 0L);
        }
    }
}

void op::Op::GetPath(std::wstring &path) {
    path = m_context->curr_path;
}

void op::Op::GetBasePath(std::wstring &path) {
    path = RuntimeEnvironment::getBasePath();
}

void op::Op::GetID(long *ret) {
    internal::set_result(ret, m_context->id);
}

void op::Op::GetLastError(long *ret) {
    internal::set_result(ret, ::GetLastError());
}

void op::Op::SetShowErrorMsg(long show_type, long *ret) {
    RuntimeEnvironment::m_showErrorMsg = show_type;
    internal::set_result(ret, 1L);
}

void op::Op::Sleep(long millseconds, long *ret) {
    ::Sleep(millseconds);
    internal::set_result(ret, 1L);
}

void op::Op::Delay(long mis, long *ret) {
    internal::set_result(ret, ::Delay(mis));
}

void op::Op::Delays(long mis_min, long mis_max, long *ret) {
    internal::set_result(ret, ::Delays(mis_min, mis_max));
}

void op::Op::GetScreenWidth(long *ret) {
    internal::set_result(ret, static_cast<long>(::GetSystemMetrics(SM_CXSCREEN)));
}

void op::Op::GetScreenHeight(long *ret) {
    internal::set_result(ret, static_cast<long>(::GetSystemMetrics(SM_CYSCREEN)));
}

void op::Op::GetScreenDepth(long *ret) {
    HDC dc = ::GetDC(nullptr);
    const int depth = dc ? ::GetDeviceCaps(dc, BITSPIXEL) : 0;
    if (dc)
        ::ReleaseDC(nullptr, dc);
    internal::set_result(ret, static_cast<long>(depth));
}

void op::Op::GetDPI(long *ret) {
    internal::set_result(ret, static_cast<long>(::GetDpiForSystem()));
}

void op::Op::GetTime(std::wstring &ret) {
    SYSTEMTIME st;
    ::GetLocalTime(&st);
    wchar_t buf[32] = {0};
    swprintf_s(buf, _countof(buf), L"%04d-%02d-%02d %02d:%02d:%02d", st.wYear, st.wMonth, st.wDay, st.wHour,
               st.wMinute, st.wSecond);
    ret = buf;
}

void op::Op::Beep(long freq, long dur, long *ret) {
    internal::set_result(ret, ::Beep(static_cast<DWORD>(freq), static_cast<DWORD>(dur)) ? 1L : 0L);
}

void op::Op::GetRandomNumber(long min, long max, long *ret) {
    if (min > max)
        std::swap(min, max);
    std::uniform_int_distribution<long> dist(min, max);
    internal::set_result(ret, dist(Rng()));
}

void op::Op::GetRandomDouble(double min, double max, double *ret) {
    if (min > max)
        std::swap(min, max);
    std::uniform_real_distribution<double> dist(min, max);
    *ret = dist(Rng());
}

void op::Op::GaiLu(long p, long *ret) {
    if (p <= 0) {
        internal::set_result(ret, 0L);
        return;
    }
    if (p == 1) {
        internal::set_result(ret, 1L);
        return;
    }
    std::uniform_int_distribution<long> dist(0, p - 1);
    internal::set_result(ret, dist(Rng()) == 0 ? 1L : 0L);
}

void op::Op::GetMachineCode(std::wstring &ret) {
    ret.clear();
    HKEY key = nullptr;
    // 64位视图读 MachineGuid(32位进程也能取到本机唯一标识)
    if (::RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Cryptography", 0,
                        KEY_READ | KEY_WOW64_64KEY, &key) == ERROR_SUCCESS) {
        wchar_t buf[64] = {0};
        DWORD size = sizeof(buf);
        DWORD type = 0;
        if (::RegQueryValueExW(key, L"MachineGuid", nullptr, &type, reinterpret_cast<LPBYTE>(buf), &size) ==
                ERROR_SUCCESS &&
            type == REG_SZ) {
            ret = buf;
        }
        ::RegCloseKey(key);
    }
}

void op::Op::IsElevated(long *ret) {
    // 不用 IsUserAnAdmin（依赖 _WIN32_WINNT>=0x0600 声明，本项目未达标）；
    // 直查进程令牌提权状态，语义等价：UAC 开启时管理员进程 TokenIsElevated=1
    HANDLE token = nullptr;
    if (::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token) == FALSE) {
        internal::set_result(ret, 0L);
        return;
    }
    TOKEN_ELEVATION elevation = {};
    DWORD len = 0;
    const BOOL ok =
        ::GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &len);
    ::CloseHandle(token);
    internal::set_result(ret, (ok && elevation.TokenIsElevated) ? 1L : 0L);
}
