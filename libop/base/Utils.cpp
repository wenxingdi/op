// #include "stdafx.h"
#include "Utils.h"
#include "AutomationModes.h"
#include "Environment.h"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <shlwapi.h>
#include <sstream>
#include <utility>
#include <vector>
// #define USE_BOOST_STACK_TRACE
#ifdef USE_BOOST_STACK_TRACE
#include <boost/stacktrace.hpp>
#endif

std::wstring _s2wstring(const std::string &s) {
    if (s.empty())
        return L"";

    const int length = MultiByteToWideChar(CP_ACP, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (length <= 0)
        return L"";

    std::wstring out(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_ACP, 0, s.data(), static_cast<int>(s.size()), out.data(), length);
    return out;
}

std::string _ws2string(const std::wstring &ws) {
    // std::string strLocale = setlocale(LC_ALL, "");
    // const wchar_t* wchSrc = ws.c_str();
    // size_t nDestSize = wcstombs(NULL, wchSrc, 0) + 1;
    // char *chDest = new char[nDestSize];
    // memset(chDest, 0, nDestSize);
    // wcstombs(chDest, wchSrc, nDestSize);
    // std::string strResult = chDest;
    // delete[]chDest;
    // setlocale(LC_ALL, strLocale.c_str());
    // return strResult;
    if (ws.empty())
        return "";

    const int length = WideCharToMultiByte(CP_ACP, 0, ws.data(), static_cast<int>(ws.size()), nullptr, 0, nullptr, nullptr);
    if (length <= 0)
        return "";

    std::string out(static_cast<size_t>(length), '\0');
    WideCharToMultiByte(CP_ACP, 0, ws.data(), static_cast<int>(ws.size()), out.data(), length, nullptr, nullptr);
    return out;
}

std::string utf8_to_ansi(std::string strUTF8) {
    if (strUTF8.empty())
        return "";

    int wide_length = MultiByteToWideChar(CP_UTF8, 0, strUTF8.c_str(), -1, nullptr, 0);
    if (wide_length <= 0)
        return "";

    std::wstring wide(static_cast<size_t>(wide_length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, strUTF8.c_str(), -1, wide.data(), wide_length);

    int ansi_length = WideCharToMultiByte(936, 0, wide.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (ansi_length <= 0)
        return "";

    std::string ansi(static_cast<size_t>(ansi_length), '\0');
    WideCharToMultiByte(936, 0, wide.c_str(), -1, ansi.data(), ansi_length, nullptr, nullptr);
    if (!ansi.empty() && ansi.back() == '\0')
        ansi.pop_back();
    return ansi;
}

long Path2GlobalPath(const std::wstring &file, const std::wstring &curr_path, std::wstring &out) {
    if (::PathFileExistsW(file.c_str())) {
        out = file;
        return 1;
    }
    out.clear();
    out = curr_path + L"\\" + file;
    if (::PathFileExistsW(out.c_str())) {
        return 1;
    }
    return 0;
}

namespace {
// 日志输出体(内部)：时间戳前缀 + 架构 + 级别，按 m_showErrorMsg 分发。
// s 为已格式化完成的文本，绝不二次解析 —— 宽字符版经此落盘时不再把
// 已展开文本当 format 调用 vsprintf_s（旧实现二次 va_start 读取不存在的
// 可变参数，日志含 % 字面即 UB，可能崩溃）。
long setlog_text(const std::string &s) {
    SYSTEMTIME sys;
    GetLocalTime(&sys);
    char tm[128];
    std::snprintf(tm, sizeof(tm), "[%4d/%02d/%02d %02d:%02d:%02d.%03d]", sys.wYear, sys.wMonth, sys.wDay, sys.wHour,
                  sys.wMinute, sys.wSecond, sys.wMilliseconds);

    std::stringstream ss;
    ss << tm << (OP64 == 1 ? "x64" : "x32") << "info: " << s << std::endl;
#ifdef USE_BOOST_STACK_TRACE
    ss << "<stack>\n" << boost::stacktrace::stacktrace() << std::endl;
#endif // USE_BOOST_STACK_TRACE

    const std::string out = ss.str();
    if (RuntimeEnvironment::m_showErrorMsg == 1) {
        MessageBoxA(NULL, out.data(), "error", MB_ICONERROR);
    } else if (RuntimeEnvironment::m_showErrorMsg == 2) {
        std::fstream file;
        file.open("__op.log", std::ios::app | std::ios::out);
        if (!file.is_open())
            return 0;
        file << out << std::endl;
        file.close();
    } else if (RuntimeEnvironment::m_showErrorMsg == 3) {
        std::cout << out << std::endl;
    }

    return 1;
}
} // namespace

long setlog(const wchar_t *format, ...) {
    va_list args;
    va_start(args, format);
    const int length = _vscwprintf(format, args);
    va_end(args);

    if (length < 0)
        return 0;

    std::vector<wchar_t> buffer(static_cast<size_t>(length) + 1, L'\0');
    va_start(args, format);
    vswprintf_s(buffer.data(), buffer.size(), format, args);
    va_end(args);

    const std::wstring tmpw(buffer.data(), static_cast<size_t>(length));
    return setlog_text(_ws2string(tmpw));
}

long setlog(const char *format, ...) {
    va_list args;
    va_start(args, format);
    const int length = _vscprintf(format, args);
    va_end(args);

    if (length < 0)
        return 0;

    std::vector<char> buffer(static_cast<size_t>(length) + 1, '\0');
    va_start(args, format);
    vsprintf_s(buffer.data(), buffer.size(), format, args);
    va_end(args);

    return setlog_text(buffer.data());
}

void split(const std::wstring &s, std::vector<std::wstring> &v, const std::wstring &c) {
    std::wstring::size_type pos1, pos2;
    size_t len = s.length();
    pos2 = s.find(c);
    pos1 = 0;
    v.clear();
    while (std::wstring::npos != pos2) {
        v.emplace_back(s.substr(pos1, pos2 - pos1));

        pos1 = pos2 + c.size();
        pos2 = s.find(c, pos1);
    }
    if (pos1 != len)
        v.emplace_back(s.substr(pos1));
}

void split(const std::string &s, std::vector<std::string> &v, const std::string &c) {
    std::string::size_type pos1, pos2;
    size_t len = s.length();
    pos2 = s.find(c);
    pos1 = 0;
    v.clear();
    while (std::string::npos != pos2) {
        v.emplace_back(s.substr(pos1, pos2 - pos1));

        pos1 = pos2 + c.size();
        pos2 = s.find(c, pos1);
    }
    if (pos1 != len)
        v.emplace_back(s.substr(pos1));
}

void wstring2upper(std::wstring &s) {
    std::transform(s.begin(), s.end(), s.begin(), towupper);
}

void string2upper(std::string &s) {
    std::transform(s.begin(), s.end(), s.begin(), toupper);
}

void wstring2lower(std::wstring &s) {
    std::transform(s.begin(), s.end(), s.begin(), towlower);
}

void string2lower(std::string &s) {
    std::transform(s.begin(), s.end(), s.begin(), tolower);
}

// 旧实现用 dx = newval.len - oldval.len + 1 计算步进,newval 比 oldval 短时
// size_t 下溢成天文数字,循环直接结束(漏替换);且 oldval 为空串时死循环。
// 现改为直接从替换后的位置继续扫描。
namespace {
template <typename StrT> void replace_impl(StrT &str, const StrT &oldval, const StrT &newval) {
    if (oldval.empty())
        return;
    typename StrT::size_type pos = 0;
    while ((pos = str.find(oldval, pos)) != StrT::npos) {
        str.replace(pos, oldval.length(), newval);
        pos += newval.length(); // 跳过刚写入的内容,避免自我匹配造成的死循环
    }
}
} // namespace

void replacea(std::string &str, const std::string &oldval, const std::string &newval) {
    replace_impl(str, oldval, newval);
}

void replacew(std::wstring &str, const std::wstring &oldval, const std::wstring &newval) {
    replace_impl(str, oldval, newval);
}

namespace op {

std::ostream &operator<<(std::ostream &o, point_t const &rhs) {
    o << rhs.x << "," << rhs.y;
    return o;
}

std::wostream &operator<<(std::wostream &o, point_t const &rhs) {
    o << rhs.x << L"," << rhs.y;
    return o;
}

} // namespace op

// Returns the last Win32 error, in string format. Returns an empty string if there is no error.
std::string GetLastErrorAsString() {
    // Get the error message ID, if any.
    DWORD errorMessageID = ::GetLastError();
    if (errorMessageID == 0) {
        return std::string(); // No error message has been recorded
    }

    LPSTR messageBuffer = nullptr;

    // Ask Win32 to give us the string version of that message ID.
    // The parameters we pass in, tell Win32 to create the buffer that holds the message for us (because we don't yet
    // know how long the message string will be).
    size_t size =
        FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                       NULL, errorMessageID, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (LPSTR)&messageBuffer, 0, NULL);

    // Copy the error message into a std::string.
    std::string message(messageBuffer, size);

    // Free the Win32's string's buffer.
    LocalFree(messageBuffer);

    return message;
}

// 带消息泵的延时。
// 旧实现是 while(GetTickCount64() < deadline) 的忙等自旋:线程在整段延时里
// 100% 占满一个 CPU 核。项目里 Delay/Delays 共 41 个调用点,其中 21 个在 input/
// (每次点击、每次按键都会进来),多开 N 个实例就等于烧满 N 个核。
// 现改为 MsgWaitForMultipleObjectsEx 阻塞等待:没有消息时线程真正挂起,
// 有消息时唤醒并派发,消息泵语义与原来一致。
bool Delay(long mis) {
    if (mis <= 0)
        return true;

    MSG msg = {};
    const ULONGLONG deadline = ::GetTickCount64() + static_cast<ULONGLONG>(mis);
    for (;;) {
        const ULONGLONG now = ::GetTickCount64();
        if (now >= deadline)
            break;

        const DWORD remain = static_cast<DWORD>(deadline - now);
        // MWMO_INPUTAVAILABLE:避免"消息已在队列里但未被标记为新消息"导致的漏唤醒。
        const DWORD r = ::MsgWaitForMultipleObjectsEx(0, nullptr, remain, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        if (r == WAIT_TIMEOUT)
            break;
        if (r == WAIT_FAILED) {
            // 极端情况(线程无法建立消息队列)退化为纯睡眠,仍然不自旋。
            ::Sleep(remain);
            break;
        }

        while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                // 不吞掉退出消息,重新投递给外层消息循环。
                ::PostQuitMessage(static_cast<int>(msg.wParam));
                return false;
            }
            ::TranslateMessage(&msg);
            ::DispatchMessageW(&msg);
        }
    }
    return true;
}

// 随机延时。旧实现 mis_min + rand() % mis_max 会溢出上界:
// Delays(100, 200) 实际产生 100~299。正确区间应为 [mis_min, mis_max]。
bool Delays(long mis_min, long mis_max) {
    if (mis_min <= 0 || mis_max <= 0)
        return false;
    if (mis_max < mis_min)
        std::swap(mis_min, mis_max);
    const long span = mis_max - mis_min + 1;
    const long mis = mis_min + (span > 0 ? rand() % span : 0);
    return Delay(mis);
}

long jittered_delay_ms(long base_ms, long percent) {
    if (base_ms <= 0)
        return 0;
    if (percent < 0)
        percent = 0;
    const long span = base_ms * percent / 100;
    const long lo = (std::max)(1L, base_ms - span);
    const long hi = base_ms + span;
    return lo + (hi > lo ? rand() % (hi - lo + 1) : 0);
}

bool DelayJitter(long base_ms, long percent) {
    return Delay(jittered_delay_ms(base_ms, percent));
}

bool SeedProcessRandom() {
    static std::atomic<bool> seeded{false};
    bool expected = false;
    if (!seeded.compare_exchange_strong(expected, true))
        return false;
    const unsigned long long tick = static_cast<unsigned long long>(::GetTickCount64());
    const unsigned long long pid = static_cast<unsigned long long>(::GetCurrentProcessId());
    // 混一个地址位，同一毫秒启动的同 pid 短生命周期进程（不太可能但成本为零）也能分开。
    const unsigned long long addr = reinterpret_cast<unsigned long long>(&seeded) >> 4;
    std::srand(static_cast<unsigned>(tick ^ (pid << 16) ^ addr));
    return true;
}
