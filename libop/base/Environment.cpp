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
