// #include "stdafx.h"
#include "WindowService.h"

#include <utility>

namespace op {

namespace {

// 防御性 wcslen：参数为 NULL 时返回 0 而非崩溃（调用方可能只传 class_name 而 title 为 NULL）
static inline size_t safe_wcslen(const wchar_t *s) noexcept {
    return s ? wcslen(s) : 0;
}

class WindowTextValue {
  public:
    WindowTextValue() = default;
    explicit WindowTextValue(std::wstring value) : value_(std::move(value)) {
    }

    operator const wchar_t *() const noexcept {
        return value_.c_str();
    }

  private:
    std::wstring value_;
};

WindowTextValue WindowClassNameText(HWND hwnd) {
    std::vector<wchar_t> buffer(256, L'\0');
    for (;;) {
        const int copied = ::GetClassNameW(hwnd, buffer.data(), static_cast<int>(buffer.size()));
        if (copied <= 0)
            return WindowTextValue();
        if (static_cast<size_t>(copied) < buffer.size() - 1)
            return WindowTextValue(std::wstring(buffer.data(), static_cast<size_t>(copied)));
        buffer.assign(buffer.size() * 2, L'\0');
    }
}

WindowTextValue WindowTitleText(HWND hwnd) {
    const int length = ::GetWindowTextLengthW(hwnd);
    if (length <= 0)
        return WindowTextValue();

    std::vector<wchar_t> buffer(static_cast<size_t>(length) + 1, L'\0');
    const int copied = ::GetWindowTextW(hwnd, buffer.data(), static_cast<int>(buffer.size()));
    if (copied <= 0)
        return WindowTextValue();

    return WindowTextValue(std::wstring(buffer.data(), static_cast<size_t>(copied)));
}

void AppendHwndText(std::wstring *retstring, int &retstringlen, HWND hwnd) {
    if (!retstring)
        return;

    const auto value = static_cast<unsigned long long>(reinterpret_cast<ULONG_PTR>(hwnd));
    if (!retstring->empty())
        retstring->push_back(L',');
    *retstring += std::to_wstring(value);
    retstringlen = static_cast<int>(retstring->size());
}

void ClearHwndText(std::wstring *retstring, int &retstringlen) {
    if (retstring)
        retstring->clear();
    retstringlen = 0;
}

} // namespace

WindowService::WindowService(void) {
    retstringlen = 0;
    window_version = 0;
    enum_process_success_count = 0;
    npid.clear();
}

WindowService::~WindowService(void) {
}

HWND WindowService::FindChildWnd(HWND child_hwnd, const wchar_t *title, const wchar_t *classname, std::wstring *retstring,
                          bool isGW_OWNER, bool isVisible, const wchar_t *process_name) {
    // 命中处置（原实现 4 个匹配分支逐份重复，收拢为一）：
    // retstring 模式（EnumWindow 系）→ 追加 hwnd 并继续枚举；
    // 查找模式（FindWindowByProcess）→ 返回 hwnd，命中即停。
    // 带 process_name 时命中还需进程匹配（EnumProcessbyName 内部按 npid 成员资格判断）。
    const auto on_match = [&](HWND hwnd) -> HWND {
        if (process_name) {
            DWORD pid = 0;
            GetWindowThreadProcessId(hwnd, &pid);
            if (!EnumProcessbyName(pid, process_name))
                return nullptr;
        }
        if (retstring) {
            AppendHwndText(retstring, retstringlen, hwnd);
            return nullptr;
        }
        return hwnd;
    };

    child_hwnd = ::GetWindow(child_hwnd, GW_HWNDFIRST);
    while (child_hwnd != NULL) {
        if (isGW_OWNER && ::GetWindow(child_hwnd, GW_OWNER) != 0) {
            child_hwnd = ::GetWindow(child_hwnd, GW_HWNDNEXT); // 获取下一个窗口
            continue;
        }

        if (isVisible && !::IsWindowVisible(child_hwnd)) {
            child_hwnd = ::GetWindow(child_hwnd, GW_HWNDNEXT); // 获取下一个窗口
            continue;
        }

        bool matched = false;
        if (title == NULL && classname == NULL) {
            matched = true;
        } else if (title != NULL && classname != NULL) {
            auto WindowClassName = WindowClassNameText(child_hwnd);
            auto WindowTitle = WindowTitleText(child_hwnd);
            matched = safe_wcslen(WindowClassName) > 1 && safe_wcslen(WindowTitle) > 1 &&
                      wcsstr(WindowClassName, classname) != NULL && wcsstr(WindowTitle, title) != NULL; // 模糊匹配
        } else if (title != NULL) {
            auto WindowTitle = WindowTitleText(child_hwnd);
            matched = safe_wcslen(WindowTitle) > 1 && wcsstr(WindowTitle, title) != NULL; // 模糊匹配
        } else { // classname != NULL
            auto WindowClassName = WindowClassNameText(child_hwnd);
            matched = safe_wcslen(WindowClassName) > 1 && wcsstr(WindowClassName, classname) != NULL; // 模糊匹配
        }

        if (matched) {
            if (HWND found = on_match(child_hwnd))
                return found;
        }

        HWND grandchild_hwnd = ::GetWindow(child_hwnd, GW_CHILD);
        if (grandchild_hwnd != NULL) {
            HWND dret = FindChildWnd(grandchild_hwnd, title, classname, retstring, isGW_OWNER, isVisible, process_name);
            if (dret != nullptr)
                break;
        }

        child_hwnd = ::GetWindow(child_hwnd, GW_HWNDNEXT); // 获取下一个窗口
    }
    return nullptr;
}

// TSEnumWindow:filter整形数: 取值定义如下
//
// 1 : 匹配窗口标题,参数title有效
//
// 2 : 匹配窗口类名,参数class_name有效.
//
// 4 : 只匹配指定父窗口的第一层孩子窗口
//
// 8 : 匹配所有者窗口为0的窗口,即顶级窗口
//
// 16 : 匹配可见的窗口
//
// 32 : 匹配出的窗口按照窗口打开顺序依次排列
bool WindowService::EnumWindow(HWND parent, const wchar_t *title, const wchar_t *class_name, LONG filter,
                               std::wstring &retstring, const wchar_t *process_name) {
    retstring.clear();
    return EnumWindowInternal(parent, title, class_name, filter, &retstring, process_name);
}

bool WindowService::EnumWindowInternal(HWND parent, const wchar_t *title, const wchar_t *class_name, LONG filter,
                                       std::wstring *retstring, const wchar_t *process_name) {
    bool bret = false;
    bool bZwindow = false; // 匹配出的窗口按照窗口打开顺序依次排列
    if (parent == 0) {
        parent = GetDesktopWindow();
    }
    if (filter > 32) {
        bZwindow = true; // 说明要排序窗口句柄
        filter = filter - 32;
    }
    // 注：bZwindow 仅作语义标记不再消费——GW_HWNDFIRST 枚举顺序本身就是 Z 序，
    // 与「按打开顺序排列」恰好等价，因此无需额外排序，保留解析以兼容 filter=33+ 入参。

    int indexpid = 0;
    if (process_name) // EnumWindowByProcess
    {
        if (safe_wcslen(process_name) < 1)
            return false;
        npid.clear();
        enum_process_success_count = 0;
        if (EnumProcessbyName(0, process_name) == false)
            return false;
    }

    DWORD processpid = 0;
    retstringlen = 0;
    // ---- filter 位语义（原 32 分支 switch 逐 case 通读核对后收拢为单循环；
    //      行为组合表与历史怪癖清单见 docs/2026-10/窗口内存模块分析_20261015.md）----
    // bit0(1)=标题模糊匹配  bit1(2)=类名模糊匹配
    // bit2(4)=只取父窗口第一层孩子（带进程名时激活「每进程取第一个」逻辑）
    // bit3(8)=owner==0  bit4(16)=只匹配可见
    const bool want_title = (filter & 1) != 0;
    const bool want_class = (filter & 2) != 0;
    const bool first_level = (filter & 4) != 0;
    const bool owner_zero = (filter & 8) != 0;
    const bool visible_only = (filter & 16) != 0;

    if (filter < 0 || filter > 31)
        return false; // 原 switch default：0-31 之外的值（含 32、负值）无对应 case，直接拒绝
    if (filter == 0 && process_name) {
        // filter=0 是「全枚举」语义，与「按进程过滤」互斥，上游大漠同样拒绝。
        // 调用方应改用 filter 1/2/4/8/16（带进程名的按标题/类名匹配）。
        return false;
    }
    if (want_title && safe_wcslen(title) < 1)
        return false;
    if (want_class && safe_wcslen(class_name) < 1)
        return false;
    if (filter == 16) // 特例：无视传入 parent，强制以桌面为父窗口枚举（与原 case 16 一致）
        parent = GetDesktopWindow();

    // 标题/类名匹配谓词（bit0/bit1 置位才校验；存在性要求长度>1，与原各 case 一致）
    const auto text_match = [&](HWND hwnd) {
        if (want_title && want_class) {
            auto cls = WindowClassNameText(hwnd);
            auto ttl = WindowTitleText(hwnd);
            return safe_wcslen(cls) > 1 && safe_wcslen(ttl) > 1 && wcsstr(cls, class_name) != NULL &&
                   wcsstr(ttl, title) != NULL; // 模糊匹配
        }
        if (want_title) {
            auto ttl = WindowTitleText(hwnd);
            return safe_wcslen(ttl) > 1 && wcsstr(ttl, title) != NULL; // 模糊匹配
        }
        if (want_class) {
            auto cls = WindowClassNameText(hwnd);
            return safe_wcslen(cls) > 1 && wcsstr(cls, class_name) != NULL; // 模糊匹配
        }
        return true;
    };

    // 子树递归策略（历史行为，勿随意"修正"）：
    // - filter 1-3：无进程时只对「自身匹配」的窗口递归（怪癖②：不匹配的子树整支不可达）；
    //   带进程时不递归（怪癖①）。
    // - filter 4-7：无进程不递归（首层语义）；带进程对「首个进程」窗口无条件递归。
    // - filter 8-31：无进程时对「未置 bit2 且置 bit3/4」的窗口无条件递归；
    //   带进程时（含 12-15/20-23/28-31 的 bit2 组合）对通过进程闸门的窗口无条件递归，
    //   均与标题/类名是否匹配无关。
    // - filter 0：对全部窗口递归。
    // 递归谓词恒为 (want_title?title:NULL, want_class?class_name:NULL, owner_zero, visible_only, process_name)。
    const bool recurse_np_always = (filter & 4) == 0 && (filter & 24) != 0; // 无进程：未置 bit2 且置 bit3/4
    const bool recurse_p = (filter & 28) != 0;                               // 有进程：bit2/3/4 任一置位

    HWND p = ::GetWindow(parent, GW_CHILD); // 获取父窗口的子窗口
    p = ::GetWindow(p, GW_HWNDFIRST);
    while (p != NULL) {
        if (owner_zero && ::GetWindow(p, GW_OWNER) != 0) {
            p = ::GetWindow(p, GW_HWNDNEXT); // 获取下一个窗口
            continue;
        }
        if (visible_only && !::IsWindowVisible(p)) {
            p = ::GetWindow(p, GW_HWNDNEXT); // 获取下一个窗口
            continue;
        }

        DWORD pid = 0;
        GetWindowThreadProcessId(p, &pid);
        bool process_gate = true; // 非进程枚举恒真
        if (process_name) {
            const bool in_process = EnumProcessbyName(pid, process_name) != FALSE;
            if (first_level) {
                // 只匹配指定映像对应的第一个进程（遇到新进程时清掉先前进程的累积结果）。
                // 统一采用 4/5/12-31 的记账方式（processpid != pid 前置守卫）；
                // 原 case 6/7 在「≥2 个同名进程且同 pid 窗口反复出现」时的双重计数差异随之归一
                // ——该场景下所有变体的「只取第一个进程」本来就都被 ClearHwndText 破坏，见分析文档。
                if (in_process && processpid != pid && indexpid < enum_process_success_count) {
                    indexpid++;
                    processpid = pid;
                    ClearHwndText(retstring, retstringlen); // 清空返回字符串
                }
                process_gate = (processpid == pid);
            } else {
                process_gate = in_process;
            }
        }

        if (process_gate) {
            const bool m = text_match(p);
            if (m) {
                AppendHwndText(retstring, retstringlen, p);
                bret = true;
            }
            const bool recurse =
                process_name ? recurse_p : (filter == 0 || recurse_np_always || ((filter & 3) != 0 && m));
            if (recurse) {
                HWND child_hwnd = ::GetWindow(p, GW_CHILD);
                if (child_hwnd != NULL) {
                    FindChildWnd(child_hwnd, want_title ? title : NULL, want_class ? class_name : NULL, retstring,
                                 owner_zero, visible_only, process_name);
                }
            }
        }

        p = ::GetWindow(p, GW_HWNDNEXT); // 获取下一个窗口
    }


    return bret;
}

bool WindowService::ClientToScreen(HWND hwnd, LONG &x, LONG &y) {
    POINT point;

    point.x = x;
    point.y = y;
    ::ClientToScreen(hwnd, &point);
    x = point.x;
    y = point.y;

    return true;
}
HWND WindowService::FindWindow(const wchar_t *class_name, const wchar_t *title) {
    if (class_name && class_name[0] == L'\0')
        class_name = nullptr;
    if (title && title[0] == L'\0')
        title = nullptr;
    return ::FindWindowW(class_name, title);
}

HWND WindowService::FindWindowEx(HWND parent, const wchar_t *class_name, const wchar_t *title) {
    if (class_name && class_name[0] == L'\0')
        class_name = nullptr;
    if (title && title[0] == L'\0')
        title = nullptr;
    return ::FindWindowExW(parent, NULL, class_name, title);
}

bool WindowService::FindWindowByProcess(const wchar_t *class_name, const wchar_t *title, HWND &rethwnd,
                                 const wchar_t *process_name, DWORD Pid) {
    bool bret = false;
    rethwnd = nullptr;
    if (process_name) {
        if (safe_wcslen(process_name) < 1)
            return false;
        npid.clear();
        enum_process_success_count = 0;
        if (EnumProcessbyName(0, process_name) == false)
            return false;
    } else if (Pid == 0) {
        return false; // 无进程名且 Pid 非法：两个分支都不命中，与原实现一致
    }

    // process_name 分支与 Pid 分支原各 ~50 行几乎相同，统一为：
    // 「可见 + 无 owner 的桌面顶层窗口」→ 进程匹配（进程名=npid 成员资格 / Pid=相等）→ 类名或标题匹配 → 子窗口查找。
    HWND p = ::GetWindow(GetDesktopWindow(), GW_CHILD); // 获取桌面窗口的子窗口
    p = ::GetWindow(p, GW_HWNDFIRST);
    while (p != NULL) {
        if (::IsWindowVisible(p) && ::GetWindow(p, GW_OWNER) == 0) {
            DWORD wnd_pid = 0;
            GetWindowThreadProcessId(p, &wnd_pid);
            const bool pid_match =
                process_name ? (EnumProcessbyName(wnd_pid, process_name) != FALSE) : (Pid == wnd_pid);
            if (pid_match) {
                if (safe_wcslen(class_name) < 1 && safe_wcslen(title) < 1) {
                    rethwnd = p;
                    bret = true;
                    break;
                }
                auto WindowClassName = WindowClassNameText(p);
                auto WindowTitle = WindowTitleText(p);
                if (safe_wcslen(WindowClassName) > 1 && safe_wcslen(WindowTitle) > 1) {
                    // wcsstr 的 needle 不能为 NULL：仅一侧条件存在时另一侧置空再匹配
                    const wchar_t *strfindclass =
                        (class_name && safe_wcslen(class_name) >= 1) ? wcsstr(WindowClassName, class_name) : nullptr;
                    const wchar_t *strfindtitle =
                        (title && safe_wcslen(title) >= 1) ? wcsstr(WindowTitle, title) : nullptr;
                    if (strfindclass || strfindtitle) {
                        rethwnd = p;
                        bret = true;
                        break;
                    }
                }
                HWND child_hwnd = ::GetWindow(p, GW_CHILD);
                if (child_hwnd != NULL) {
                    const wchar_t *classname = safe_wcslen(class_name) > 0 ? class_name : NULL;
                    const wchar_t *titles = safe_wcslen(title) > 0 ? title : NULL;
                    HWND dret = FindChildWnd(child_hwnd, titles, classname, NULL, false, false, process_name);
                    if (dret != nullptr) {
                        rethwnd = dret;
                        bret = true;
                        break;
                    }
                }
            }
        }
        p = ::GetWindow(p, GW_HWNDNEXT); // 获取下一个窗口
    }

    return bret;
}

} // namespace op
