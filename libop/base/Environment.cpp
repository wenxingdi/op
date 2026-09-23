#include "Environment.h"
#include <windows.h>
#include <vector>

namespace {

constexpr DWORD kInitialModulePathChars = 260;

std::wstring module_file_name(HINSTANCE instance) {
    std::vector<wchar_t> buffer(kInitialModulePathChars, L'\0');
    for (;;) {
        const DWORD copied = ::GetModuleFileNameW(instance, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (copied == 0)
            return L"";
        if (copied < buffer.size() - 1)
            return std::wstring(buffer.data(), copied);
        buffer.assign(buffer.size() * 2, L'\0');
    }
}

} // namespace

void *RuntimeEnvironment::m_instance = nullptr;
std::wstring RuntimeEnvironment::m_basePath;
std::wstring RuntimeEnvironment::m_opName;
// 0=静默 1=MessageBox 弹窗 2=写 __op.log 3=stdout
// 作为库,默认值必须是静默:setlog 在项目里有 ~190 个调用点,默认弹模态框会在
// 多开场景下弹满屏并阻塞调用线程。需要排错时显式调用 SetShowErrorMsg(2) 写日志。
int RuntimeEnvironment::m_showErrorMsg = 0;
void RuntimeEnvironment::setInstance(void *instance) {
    m_instance = instance;
    std::wstring s = module_file_name(static_cast<HINSTANCE>(m_instance));
    size_t index = s.rfind(L"\\");
    if (index != s.npos) {
        m_basePath = s.substr(0, index);
        m_opName = s.substr(index + 1);
    }
}
void *RuntimeEnvironment::getInstance() {
    return m_instance;
}

std::wstring RuntimeEnvironment::getBasePath() {

    return m_basePath;
}

std::wstring RuntimeEnvironment::getOpName() {
    return m_opName;
}

namespace {

// 从进程/线程句柄取完整性级别 RID；失败返回 false 且 rid=0。
bool integrity_rid_from_handle(HANDLE handle, DWORD &rid) {
    rid = 0;
    HANDLE token = nullptr;
    if (::OpenProcessToken(handle, TOKEN_QUERY, &token) == FALSE)
        return false;

    bool ok = false;
    char buf[64] = {0};
    DWORD size = sizeof(buf);
    // TokenIntegrityLevel = 25，缓冲区前 8 字节是 SID_AND_ATTRIBUTES，其首字段即 SID 指针。
    if (::GetTokenInformation(token, TokenIntegrityLevel, buf, size, &size) != FALSE) {
        const PSID sid = *reinterpret_cast<PSID *>(buf);
        const auto count_ptr = ::GetSidSubAuthorityCount(sid);
        if (count_ptr != nullptr) {
            const auto rid_ptr = ::GetSidSubAuthority(sid, *count_ptr - 1);
            if (rid_ptr != nullptr) {
                rid = *rid_ptr;
                ok = true;
            }
        }
    }
    ::CloseHandle(token);
    return ok;
}

} // namespace

bool GetCurrentIntegrityRid(DWORD &rid) {
    return integrity_rid_from_handle(::GetCurrentProcess(), rid);
}

bool GetProcessIntegrityRid(DWORD pid, DWORD &rid) {
    rid = 0;
    const HANDLE proc = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (proc == nullptr)
        return false;
    const bool ok = integrity_rid_from_handle(proc, rid);
    ::CloseHandle(proc);
    return ok;
}

bool IsProcessIntegrityHigher(DWORD pid) {
    DWORD self = 0;
    if (!GetCurrentIntegrityRid(self))
        return false;

    DWORD target = 0;
    if (GetProcessIntegrityRid(pid, target))
        return target > self;

    // 打不开目标进程且原因是访问拒绝：UIPI 对更高完整性进程连查询权限都不发，按"更高"处理，
    // 宁可误伤受保护进程（本就不能注入），也不放行注定静默失败的绑定。
    return ::GetLastError() == ERROR_ACCESS_DENIED;
}
