#ifndef OP_BASE_ENVIRONMENT_H_
#define OP_BASE_ENVIRONMENT_H_
#include <string>
#include <Windows.h>
class RuntimeEnvironment {
  public:
    static void setInstance(void *instance);
    static void *getInstance();
    static std::wstring getBasePath();
    static std::wstring getOpName();
    static int m_showErrorMsg;

private:
    static void *m_instance;
    static std::wstring m_basePath;
    static std::wstring m_opName;
};
#endif // OP_BASE_ENVIRONMENT_H_

// 进程完整性级别 RID（TokenIntegrityLevel 的最后一个 sub-authority，如 0x2000=中、0x3000=高）。
// 返回 false 表示无法判定（进程打不开/无 token），rid 输出 0。
bool GetProcessIntegrityRid(DWORD pid, DWORD &rid);
bool GetCurrentIntegrityRid(DWORD &rid);
// 目标进程完整性是否高于当前进程。UIPI 会静默吞掉低→高完整性的跨进程输入（SendMessage/
// 钩子注入均失效但 API 照常返成功），绑定前用它把"假成功"挡掉。
// 无法打开目标进程且原因是访问拒绝时（UIPI 连查询权限都不给）按"更高"处理；其余判定失败按"否"放行。
bool IsProcessIntegrityHigher(DWORD pid);
