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
    switch (filter) {
    case 0: // 所有模式
    {
        if (process_name) // EnumWindowByProcess
        {
            // filter=0 是「全枚举」语义，与「按进程过滤」互斥，上游大漠同样拒绝。
            // 调用方应改用 filter 1/2/4/8/16（带进程名的按标题/类名匹配）。
            return false;
        }

        HWND p = ::GetWindow(parent, GW_CHILD); // 获取桌面窗口的子窗口
        if (p == NULL)
            return false;
        p = ::GetWindow(p, GW_HWNDFIRST);
        while (p != NULL) {
            AppendHwndText(retstring, retstringlen, p);
            bret = true;
            HWND child_hwnd = ::GetWindow(p, GW_CHILD);
            if (child_hwnd != NULL) {
                FindChildWnd(child_hwnd, NULL, NULL, retstring);
            }

            p = ::GetWindow(p, GW_HWNDNEXT); // 获取下一个窗口
        }
        break;
    }
    case 1: // 1 : 匹配窗口标题,参数title有效
    {
        if (safe_wcslen(title) < 1)
            return false;
        HWND p = ::GetWindow(parent, GW_CHILD); // 获取桌面窗口的子窗口
        if (p == NULL)
            return false;
        p = ::GetWindow(p, GW_HWNDFIRST);
        while (p != NULL) {
            auto WindowTitle = WindowTitleText(p);
            if (safe_wcslen(WindowTitle) > 1) {
                const wchar_t *strfind = wcsstr(WindowTitle, title); // 模糊匹配
                if (strfind) {
                    if (process_name) // EnumWindowByProcess
                    {
                        DWORD pid = 0;
                        GetWindowThreadProcessId(p, &pid);
                        if (EnumProcessbyName(pid, process_name)) {
                            AppendHwndText(retstring, retstringlen, p);
                            bret = true;
                        }
                    } else {
                        AppendHwndText(retstring, retstringlen, p);
                        bret = true;
                        HWND child_hwnd = ::GetWindow(p, GW_CHILD);
                        if (child_hwnd != NULL) {
                            FindChildWnd(child_hwnd, title, NULL, retstring);
                        }
                    }
                }
            }

            p = ::GetWindow(p, GW_HWNDNEXT); // 获取下一个窗口
        }
        break;
    }
    case 2: // 2 : 匹配窗口类名,参数class_name有效.
    {
        if (safe_wcslen(class_name) < 1)
            return false;
        HWND p = ::GetWindow(parent, GW_CHILD); // 获取桌面窗口的子窗口
        if (p == NULL)
            return false;
        p = ::GetWindow(p, GW_HWNDFIRST);
        while (p != NULL) {
            auto WindowClassName = WindowClassNameText(p);
            if (safe_wcslen(WindowClassName) > 1) {
                const wchar_t *strfind = wcsstr(WindowClassName, class_name); // 模糊匹配
                if (strfind) {
                    if (process_name) // EnumWindowByProcess
                    {
                        DWORD pid = 0;
                        GetWindowThreadProcessId(p, &pid);
                        if (EnumProcessbyName(pid, process_name)) {
                            AppendHwndText(retstring, retstringlen, p);
                            bret = true;
                        }
                    } else {
                        AppendHwndText(retstring, retstringlen, p);
                        bret = true;
                        HWND child_hwnd = ::GetWindow(p, GW_CHILD);
                        if (child_hwnd != NULL) {
                            FindChildWnd(child_hwnd, NULL, class_name, retstring);
                        }
                    }
                }
            }
            p = ::GetWindow(p, GW_HWNDNEXT); // 获取下一个窗口
        }
        break;
    }
    case 3: // 1.窗口标题+2.窗口类名
    {
        if (safe_wcslen(class_name) < 1 && safe_wcslen(title) < 1)
            return false;
        HWND p = ::GetWindow(parent, GW_CHILD); // 获取桌面窗口的子窗口
        if (p == NULL)
            return false;
        p = ::GetWindow(p, GW_HWNDFIRST);
        while (p != NULL) {
            auto WindowClassName = WindowClassNameText(p);
            auto WindowTitle = WindowTitleText(p);
            if (safe_wcslen(WindowClassName) > 1 && safe_wcslen(WindowTitle) > 1) {
                const wchar_t *strfindclass = wcsstr(WindowClassName, class_name); // 模糊匹配
                const wchar_t *strfindtitle = wcsstr(WindowTitle, title);          // 模糊匹配
                if (strfindclass && strfindtitle) {
                    if (process_name) // EnumWindowByProcess
                    {
                        DWORD pid = 0;
                        GetWindowThreadProcessId(p, &pid);
                        if (EnumProcessbyName(pid, process_name)) {
                            AppendHwndText(retstring, retstringlen, p);
                            bret = true;
                        }
                    } else {
                        AppendHwndText(retstring, retstringlen, p);
                        bret = true;
                        HWND child_hwnd = ::GetWindow(p, GW_CHILD);
                        if (child_hwnd != NULL) {
                            FindChildWnd(child_hwnd, title, class_name, retstring);
                        }
                    }
                }
            }

            p = ::GetWindow(p, GW_HWNDNEXT); // 获取下一个窗口
        }
        break;
    }
    case 4: // 4 : 只匹配指定父窗口的第一层孩子窗口
    {
        HWND p = ::GetWindow(parent, GW_CHILD); // 获取桌面窗口的子窗口
        if (p == NULL)
            return false;
        p = ::GetWindow(p, GW_HWNDFIRST);
        while (p != NULL) {
            if (process_name) // EnumWindowByProcess
            {
                DWORD pid = 0;
                GetWindowThreadProcessId(p, &pid);
                if (EnumProcessbyName(pid, process_name)) {
                    if (processpid != pid) // 只匹配指定映像的所对应的第一个进程.
                                           // 可能有很多同映像名的进程，只匹配第一个进程的.
                    {
                        if (indexpid < enum_process_success_count) {
                            indexpid++;
                            processpid = pid;
                            ClearHwndText(retstring, retstringlen); // 清空返回字符串
                        }
                    }
                    if (processpid == pid) {
                        AppendHwndText(retstring, retstringlen, p);
                        bret = true;
                        HWND child_hwnd = ::GetWindow(p, GW_CHILD);
                        if (child_hwnd != NULL) {
                            FindChildWnd(child_hwnd, NULL, NULL, retstring, false, false, process_name);
                        }
                    }
                }
            } else {
                AppendHwndText(retstring, retstringlen, p);
                bret = true;
            }
            p = ::GetWindow(p, GW_HWNDNEXT); // 获取下一个窗口
        }
        break;
    }
    case 5: // 1.匹配窗口标题+//4 : 只匹配指定父窗口的第一层孩子窗口
    {
        if (safe_wcslen(title) < 1)
            return false;

        HWND p = ::GetWindow(parent, GW_CHILD); // 获取桌面窗口的子窗口
        if (p == NULL)
            return false;
        p = ::GetWindow(p, GW_HWNDFIRST);
        while (p != NULL) {
            if (process_name) // EnumWindowByProcess
            {
                DWORD pid = 0;
                GetWindowThreadProcessId(p, &pid);
                if (EnumProcessbyName(pid, process_name)) {
                    if (processpid != pid) // 只匹配指定映像的所对应的第一个进程.
                                           // 可能有很多同映像名的进程，只匹配第一个进程的.
                    {
                        if (indexpid < enum_process_success_count) {
                            indexpid++;
                            processpid = pid;
                            ClearHwndText(retstring, retstringlen); // 清空返回字符串
                        }
                    }
                    if (processpid == pid) {
                        auto WindowTitle = WindowTitleText(p);
                        if (safe_wcslen(WindowTitle) > 1) {
                            if (wcsstr(WindowTitle, title)) {
                                AppendHwndText(retstring, retstringlen, p);
                                bret = true;
                            }
                        }
                        HWND child_hwnd = ::GetWindow(p, GW_CHILD);
                        if (child_hwnd != NULL) {
                            FindChildWnd(child_hwnd, title, NULL, retstring, false, false, process_name);
                        }
                    }
                }

            } else {
                auto WindowTitle = WindowTitleText(p);
                if (safe_wcslen(WindowTitle) > 1) {
                    if (wcsstr(WindowTitle, title)) {
                        AppendHwndText(retstring, retstringlen, p);
                        bret = true;
                    }
                }
            }

            p = ::GetWindow(p, GW_HWNDNEXT); // 获取下一个窗口
        }
        break;
    }
    case 6: // 2 : 匹配窗口类名+4 : 只匹配指定父窗口的第一层孩子窗口
    {
        if (safe_wcslen(class_name) < 1)
            return false;
        HWND p = ::GetWindow(parent, GW_CHILD); // 获取桌面窗口的子窗口
        if (p == NULL)
            return false;
        p = ::GetWindow(p, GW_HWNDFIRST);
        while (p != NULL) {
            if (process_name) // EnumWindowByProcess
            {
                DWORD pid = 0;
                GetWindowThreadProcessId(p, &pid);
                if (EnumProcessbyName(pid, process_name)) {
                    if (indexpid < enum_process_success_count) {
                        indexpid++;
                        processpid = pid;
                        ClearHwndText(retstring, retstringlen); // 清空返回字符串
                    }
                }
                if (processpid == pid) {
                    auto WindowClassName = WindowClassNameText(p);
                    if (safe_wcslen(WindowClassName) > 1) {
                        if (wcsstr(WindowClassName, class_name)) {
                            AppendHwndText(retstring, retstringlen, p);
                            bret = true;
                        }
                    }
                    HWND child_hwnd = ::GetWindow(p, GW_CHILD);
                    if (child_hwnd != NULL) {
                        FindChildWnd(child_hwnd, NULL, class_name, retstring, false, false, process_name);
                    }
                }
            } else {
                auto WindowClassName = WindowClassNameText(p);
                if (safe_wcslen(WindowClassName) > 1) {
                    if (wcsstr(WindowClassName, class_name)) {
                        AppendHwndText(retstring, retstringlen, p);
                        bret = true;
                    }
                }
            }
            p = ::GetWindow(p, GW_HWNDNEXT); // 获取下一个窗口
        }
        break;
    }
    case 7: // 1.窗口标题+2.窗口类名+4 : 只匹配指定父窗口的第一层孩子窗口
    {
        if (safe_wcslen(class_name) < 1 && safe_wcslen(title) < 1)
            return false;
        HWND p = ::GetWindow(parent, GW_CHILD); // 获取桌面窗口的子窗口
        if (p == NULL)
            return false;
        p = ::GetWindow(p, GW_HWNDFIRST);
        while (p != NULL) {
            if (process_name) // EnumWindowByProcess
            {
                DWORD pid = 0;
                GetWindowThreadProcessId(p, &pid);
                if (EnumProcessbyName(pid, process_name)) {
                    if (indexpid < enum_process_success_count) {
                        indexpid++;
                        processpid = pid;
                        ClearHwndText(retstring, retstringlen); // 清空返回字符串
                    }
                }
                if (processpid == pid) {
                    auto WindowClassName = WindowClassNameText(p);
                    auto WindowTitle = WindowTitleText(p);
                    if (safe_wcslen(WindowClassName) > 1 && safe_wcslen(WindowTitle) > 1) {
                        const wchar_t *strfindclass = wcsstr(WindowClassName, class_name); // 模糊匹配
                        const wchar_t *strfindtitle = wcsstr(WindowTitle, title);          // 模糊匹配
                        if (strfindclass && strfindtitle) {
                            AppendHwndText(retstring, retstringlen, p);
                            bret = true;
                        }
                    }
                    HWND child_hwnd = ::GetWindow(p, GW_CHILD);
                    if (child_hwnd != NULL) {
                        FindChildWnd(child_hwnd, title, class_name, retstring, false, false, process_name);
                    }
                }
            } else {
                auto WindowClassName = WindowClassNameText(p);
                auto WindowTitle = WindowTitleText(p);
                if (safe_wcslen(WindowClassName) > 1 && safe_wcslen(WindowTitle) > 1) {
                    const wchar_t *strfindclass = wcsstr(WindowClassName, class_name); // 模糊匹配
                    const wchar_t *strfindtitle = wcsstr(WindowTitle, title);          // 模糊匹配
                    if (strfindclass && strfindtitle) {
                        AppendHwndText(retstring, retstringlen, p);
                        bret = true;
                    }
                }
            }

            p = ::GetWindow(p, GW_HWNDNEXT); // 获取下一个窗口
        }
        break;
    }
    case 8: // 8 : 匹配所有者窗口为0的窗口,即顶级窗口
    {
        HWND p = ::GetWindow(parent, GW_CHILD); // 获取桌面窗口的子窗口
        if (p == NULL)
            return false;
        p = ::GetWindow(p, GW_HWNDFIRST);
        while (p != NULL) {
            if (::GetWindow(p, GW_OWNER) == 0) {
                if (process_name) // EnumWindowByProcess
                {
                    DWORD pid = 0;
                    GetWindowThreadProcessId(p, &pid);
                    if (EnumProcessbyName(pid, process_name)) {
                        AppendHwndText(retstring, retstringlen, p);
                        bret = true;
                        HWND child_hwnd = ::GetWindow(p, GW_CHILD);
                        if (child_hwnd != NULL) {
                            FindChildWnd(child_hwnd, NULL, NULL, retstring, true, false, process_name);
                        }
                    }
                } else {
                    AppendHwndText(retstring, retstringlen, p);
                    bret = true;
                    HWND child_hwnd = ::GetWindow(p, GW_CHILD);
                    if (child_hwnd != NULL) {
                        FindChildWnd(child_hwnd, NULL, NULL, retstring, true);
                    }
                }
            }
            p = ::GetWindow(p, GW_HWNDNEXT); // 获取下一个窗口
        }
        break;
    }
    case 9: // 1.窗口标题+8 : 匹配所有者窗口为0的窗口,即顶级窗口
    {
        if (safe_wcslen(title) < 1)
            return false;
        HWND p = ::GetWindow(parent, GW_CHILD); // 获取桌面窗口的子窗口
        if (p == NULL)
            return false;
        p = ::GetWindow(p, GW_HWNDFIRST);
        while (p != NULL) {
            if (::GetWindow(p, GW_OWNER) == 0) {
                if (process_name) // EnumWindowByProcess
                {
                    DWORD pid = 0;
                    GetWindowThreadProcessId(p, &pid);
                    if (EnumProcessbyName(pid, process_name)) {
                        auto WindowTitle = WindowTitleText(p);
                        if (safe_wcslen(WindowTitle) > 1) {
                            const wchar_t *strfind = wcsstr(WindowTitle, title); // 模糊匹配
                            if (strfind) {
                                AppendHwndText(retstring, retstringlen, p);
                                bret = true;
                            }
                        }
                        HWND child_hwnd = ::GetWindow(p, GW_CHILD);
                        if (child_hwnd != NULL) {
                            FindChildWnd(child_hwnd, title, NULL, retstring, true, false, process_name);
                        }
                    }
                } else {
                    auto WindowTitle = WindowTitleText(p);
                    if (safe_wcslen(WindowTitle) > 1) {
                        const wchar_t *strfind = wcsstr(WindowTitle, title); // 模糊匹配
                        if (strfind) {
                            AppendHwndText(retstring, retstringlen, p);
                            bret = true;
                        }
                    }
                    HWND child_hwnd = ::GetWindow(p, GW_CHILD);
                    if (child_hwnd != NULL) {
                        FindChildWnd(child_hwnd, title, NULL, retstring, true);
                    }
                }
            }
            p = ::GetWindow(p, GW_HWNDNEXT); // 获取下一个窗口
        }
        break;
    }
    case 10: // 2.窗口类名+8 : 匹配所有者窗口为0的窗口,即顶级窗口
    {
        if (safe_wcslen(class_name) < 1)
            return false;
        HWND p = ::GetWindow(parent, GW_CHILD); // 获取桌面窗口的子窗口
        if (p == NULL)
            return false;
        p = ::GetWindow(p, GW_HWNDFIRST);
        while (p != NULL) {
            if (::GetWindow(p, GW_OWNER) == 0) {
                if (process_name) // EnumWindowByProcess
                {
                    DWORD pid = 0;
                    GetWindowThreadProcessId(p, &pid);
                    if (EnumProcessbyName(pid, process_name)) {
                        auto WindowClassName = WindowClassNameText(p);
                        if (safe_wcslen(WindowClassName) > 1) {
                            const wchar_t *strfind = wcsstr(WindowClassName, class_name); // 模糊匹配
                            if (strfind) {
                                AppendHwndText(retstring, retstringlen, p);
                                bret = true;
                            }
                        }
                        HWND child_hwnd = ::GetWindow(p, GW_CHILD);
                        if (child_hwnd != NULL) {
                            FindChildWnd(child_hwnd, NULL, class_name, retstring, true, false, process_name);
                        }
                    }
                } else {
                    auto WindowClassName = WindowClassNameText(p);
                    if (safe_wcslen(WindowClassName) > 1) {
                        const wchar_t *strfind = wcsstr(WindowClassName, class_name); // 模糊匹配
                        if (strfind) {
                            AppendHwndText(retstring, retstringlen, p);
                            bret = true;
                        }
                    }

                    HWND child_hwnd = ::GetWindow(p, GW_CHILD);
                    if (child_hwnd != NULL) {
                        FindChildWnd(child_hwnd, NULL, class_name, retstring, true);
                    }
                }
            }
            p = ::GetWindow(p, GW_HWNDNEXT); // 获取下一个窗口
        }
        break;
    }
    case 11: ////1.窗口标题+2.窗口类名+8 : 匹配所有者窗口为0的窗口,即顶级窗口
    {
        if (safe_wcslen(class_name) < 1 && safe_wcslen(title) < 1)
            return false;
        HWND p = ::GetWindow(parent, GW_CHILD); // 获取桌面窗口的子窗口
        if (p == NULL)
            return false;
        p = ::GetWindow(p, GW_HWNDFIRST);
        while (p != NULL) {
            if (::GetWindow(p, GW_OWNER) == 0) {
                if (process_name) // EnumWindowByProcess
                {
                    DWORD pid = 0;
                    GetWindowThreadProcessId(p, &pid);
                    if (EnumProcessbyName(pid, process_name)) {
                        auto WindowClassName = WindowClassNameText(p);
                        auto WindowTitle = WindowTitleText(p);
                        if (safe_wcslen(WindowClassName) > 1 && safe_wcslen(WindowTitle) > 1) {
                            const wchar_t *strfindclass = wcsstr(WindowClassName, class_name); // 模糊匹配
                            const wchar_t *strfindtitle = wcsstr(WindowTitle, title);          // 模糊匹配
                            if (strfindclass && strfindtitle) {
                                AppendHwndText(retstring, retstringlen, p);
                                bret = true;
                            }
                        }
                        HWND child_hwnd = ::GetWindow(p, GW_CHILD);
                        if (child_hwnd != NULL) {
                            FindChildWnd(child_hwnd, title, class_name, retstring, true, false, process_name);
                        }
                    }
                } else {
                    auto WindowClassName = WindowClassNameText(p);
                    auto WindowTitle = WindowTitleText(p);
                    if (safe_wcslen(WindowClassName) > 1 && safe_wcslen(WindowTitle) > 1) {
                        const wchar_t *strfindclass = wcsstr(WindowClassName, class_name); // 模糊匹配
                        const wchar_t *strfindtitle = wcsstr(WindowTitle, title);          // 模糊匹配
                        if (strfindclass && strfindtitle) {
                            AppendHwndText(retstring, retstringlen, p);
                            bret = true;
                        }
                    }
                    HWND child_hwnd = ::GetWindow(p, GW_CHILD);
                    if (child_hwnd != NULL) {
                        FindChildWnd(child_hwnd, title, class_name, retstring, true);
                    }
                }
            }
            p = ::GetWindow(p, GW_HWNDNEXT); // 获取下一个窗口
        }
        break;
    }
    case 12: // 4 : 只匹配指定父窗口的第一层孩子窗口+8 :
             // 匹配所有者窗口为0的窗口,即顶级窗口
    {
        HWND p = ::GetWindow(parent, GW_CHILD); // 获取桌面窗口的子窗口
        p = ::GetWindow(p, GW_HWNDFIRST);
        while (p != NULL) {
            if (::GetWindow(p, GW_OWNER) == 0) {
                if (process_name) // EnumWindowByProcess
                {
                    DWORD pid = 0;
                    GetWindowThreadProcessId(p, &pid);
                    if (EnumProcessbyName(pid, process_name)) {
                    if (processpid != pid) // 只匹配指定映像的所对应的第一个进程.
                                               // 可能有很多同映像名的进程，只匹配第一个进程的.
                        {
                            if (indexpid < enum_process_success_count) {
                                indexpid++;
                                processpid = pid;
                                ClearHwndText(retstring, retstringlen); // 清空返回字符串
                            }
                        }
                        if (processpid == pid) {
                            AppendHwndText(retstring, retstringlen, p);
                            bret = true;
                            HWND child_hwnd = ::GetWindow(p, GW_CHILD);
                            if (child_hwnd != NULL) {
                                FindChildWnd(child_hwnd, NULL, NULL, retstring, true, false, process_name);
                            }
                        }
                    }
                } else {
                    AppendHwndText(retstring, retstringlen, p);
                    bret = true;
                }
            }
            p = ::GetWindow(p, GW_HWNDNEXT); // 获取下一个窗口
        }
        break;
    }
    case 13: // 1.窗口标题+4 : 只匹配指定父窗口的第一层孩子窗口+8 :
             // 匹配所有者窗口为0的窗口,即顶级窗口
    {
        if (safe_wcslen(title) < 1)
            return false;
        HWND p = ::GetWindow(parent, GW_CHILD); // 获取桌面窗口的子窗口
        p = ::GetWindow(p, GW_HWNDFIRST);
        while (p != NULL) {
            if (::GetWindow(p, GW_OWNER) == 0) {
                if (process_name) // EnumWindowByProcess
                {
                    DWORD pid = 0;
                    GetWindowThreadProcessId(p, &pid);
                    if (EnumProcessbyName(pid, process_name)) {
                    if (processpid != pid) // 只匹配指定映像的所对应的第一个进程.
                                               // 可能有很多同映像名的进程，只匹配第一个进程的.
                        {
                            if (indexpid < enum_process_success_count) {
                                indexpid++;
                                processpid = pid;
                                ClearHwndText(retstring, retstringlen); // 清空返回字符串
                            }
                        }
                        if (processpid == pid) {
                            auto WindowTitle = WindowTitleText(p);
                            if (safe_wcslen(WindowTitle) > 1) {
                                const wchar_t *strfind = wcsstr(WindowTitle, title); // 模糊匹配
                                if (strfind) {
                                    AppendHwndText(retstring, retstringlen, p);
                                    bret = true;
                                }
                            }
                            HWND child_hwnd = ::GetWindow(p, GW_CHILD);
                            if (child_hwnd != NULL) {
                                FindChildWnd(child_hwnd, title, NULL, retstring, true, false, process_name);
                            }
                        }
                    }
                } else {
                    auto WindowTitle = WindowTitleText(p);
                    if (safe_wcslen(WindowTitle) > 1) {
                        const wchar_t *strfind = wcsstr(WindowTitle, title); // 模糊匹配
                        if (strfind) {
                            AppendHwndText(retstring, retstringlen, p);
                            bret = true;
                        }
                    }
                }
            }
            p = ::GetWindow(p, GW_HWNDNEXT); // 获取下一个窗口
        }
        break;
    }
    case 14: // 2.窗口类名+4 : 只匹配指定父窗口的第一层孩子窗口+8 :
             // 匹配所有者窗口为0的窗口,即顶级窗口
    {
        if (safe_wcslen(class_name) < 1)
            return false;
        HWND p = ::GetWindow(parent, GW_CHILD); // 获取桌面窗口的子窗口
        p = ::GetWindow(p, GW_HWNDFIRST);
        while (p != NULL) {
            if (::GetWindow(p, GW_OWNER) == 0) {
                if (process_name) // EnumWindowByProcess
                {
                    DWORD pid = 0;
                    GetWindowThreadProcessId(p, &pid);
                    if (EnumProcessbyName(pid, process_name)) {
                    if (processpid != pid) // 只匹配指定映像的所对应的第一个进程.
                                               // 可能有很多同映像名的进程，只匹配第一个进程的.
                        {
                            if (indexpid < enum_process_success_count) {
                                indexpid++;
                                processpid = pid;
                                ClearHwndText(retstring, retstringlen); // 清空返回字符串
                            }
                        }
                        if (processpid == pid) {
                            auto WindowClassName = WindowClassNameText(p);
                            if (safe_wcslen(WindowClassName) > 1) {
                                const wchar_t *strfind = wcsstr(WindowClassName, class_name); // 模糊匹配
                                if (strfind) {
                                    AppendHwndText(retstring, retstringlen, p);
                                    bret = true;
                                }
                            }
                            HWND child_hwnd = ::GetWindow(p, GW_CHILD);
                            if (child_hwnd != NULL) {
                                FindChildWnd(child_hwnd, NULL, class_name, retstring, true, false, process_name);
                            }
                        }
                    }
                } else {
                    auto WindowClassName = WindowClassNameText(p);
                    if (safe_wcslen(WindowClassName) > 1) {
                        const wchar_t *strfind = wcsstr(WindowClassName, class_name); // 模糊匹配
                        if (strfind) {
                            AppendHwndText(retstring, retstringlen, p);
                            bret = true;
                        }
                    }
                }
            }
            p = ::GetWindow(p, GW_HWNDNEXT); // 获取下一个窗口
        }
        break;
    }
    case 15: ////1.窗口标题+2.窗口类名+4 : 只匹配指定父窗口的第一层孩子窗口+8 :
             // 匹配所有者窗口为0的窗口,即顶级窗口
    {
        if (safe_wcslen(class_name) < 1 && safe_wcslen(title) < 1)
            return false;
        HWND p = ::GetWindow(parent, GW_CHILD); // 获取桌面窗口的子窗口
        p = ::GetWindow(p, GW_HWNDFIRST);
        while (p != NULL) {
            if (::GetWindow(p, GW_OWNER) == 0) {
                if (process_name) // EnumWindowByProcess
                {
                    DWORD pid = 0;
                    GetWindowThreadProcessId(p, &pid);
                    if (EnumProcessbyName(pid, process_name)) {
                    if (processpid != pid) // 只匹配指定映像的所对应的第一个进程.
                                               // 可能有很多同映像名的进程，只匹配第一个进程的.
                        {
                            if (indexpid < enum_process_success_count) {
                                indexpid++;
                                processpid = pid;
                                ClearHwndText(retstring, retstringlen); // 清空返回字符串
                            }
                        }
                        if (processpid == pid) {
                            auto WindowClassName = WindowClassNameText(p);
                            auto WindowTitle = WindowTitleText(p);
                            if (safe_wcslen(WindowClassName) > 1 && safe_wcslen(WindowTitle) > 1) {
                                const wchar_t *strfindclass = wcsstr(WindowClassName, class_name); // 模糊匹配
                                const wchar_t *strfindtitle = wcsstr(WindowTitle, title);          // 模糊匹配
                                if (strfindclass && strfindtitle) {
                                    AppendHwndText(retstring, retstringlen, p);
                                    bret = true;
                                }
                            }
                            HWND child_hwnd = ::GetWindow(p, GW_CHILD);
                            if (child_hwnd != NULL) {
                                FindChildWnd(child_hwnd, title, class_name, retstring, true, false, process_name);
                            }
                        }
                    }
                } else {
                    auto WindowClassName = WindowClassNameText(p);
                    auto WindowTitle = WindowTitleText(p);
                    if (safe_wcslen(WindowClassName) > 1 && safe_wcslen(WindowTitle) > 1) {
                        const wchar_t *strfindclass = wcsstr(WindowClassName, class_name); // 模糊匹配
                        const wchar_t *strfindtitle = wcsstr(WindowTitle, title);          // 模糊匹配
                        if (strfindclass && strfindtitle) {
                            AppendHwndText(retstring, retstringlen, p);
                            bret = true;
                        }
                    }
                }
            }
            p = ::GetWindow(p, GW_HWNDNEXT); // 获取下一个窗口
        }
        break;
    }
    case 16: // 匹配可见的窗口
    {
        parent = GetDesktopWindow();
        HWND p = ::GetWindow(parent, GW_CHILD); // 获取桌面窗口的子窗口
        p = ::GetWindow(p, GW_HWNDFIRST);
        while (p != NULL) {
            if (::IsWindowVisible(p)) {
                if (process_name) // EnumWindowByProcess
                {
                    DWORD pid = 0;
                    GetWindowThreadProcessId(p, &pid);
                    if (EnumProcessbyName(pid, process_name)) {
                        AppendHwndText(retstring, retstringlen, p);
                        bret = true;
                        HWND child_hwnd = ::GetWindow(p, GW_CHILD);
                        if (child_hwnd != NULL) {
                            FindChildWnd(child_hwnd, NULL, NULL, retstring, false, true, process_name);
                        }
                    }
                } else {
                    AppendHwndText(retstring, retstringlen, p);
                    bret = true;
                    HWND child_hwnd = ::GetWindow(p, GW_CHILD);
                    if (child_hwnd != NULL) {
                        FindChildWnd(child_hwnd, NULL, NULL, retstring, false, true);
                    }
                }
            }
            p = ::GetWindow(p, GW_HWNDNEXT); // 获取下一个窗口
        }
        break;
    }
    case 17: // 1.窗口标题+//匹配可见的窗口
    {
        if (safe_wcslen(title) < 1)
            return false;
        HWND p = ::GetWindow(parent, GW_CHILD); // 获取桌面窗口的子窗口
        p = ::GetWindow(p, GW_HWNDFIRST);
        while (p != NULL) {
            if (::IsWindowVisible(p)) {
                if (process_name) // EnumWindowByProcess
                {
                    DWORD pid = 0;
                    GetWindowThreadProcessId(p, &pid);
                    if (EnumProcessbyName(pid, process_name)) {
                        auto WindowTitle = WindowTitleText(p);
                        if (safe_wcslen(WindowTitle) > 1) {
                            const wchar_t *strfind = wcsstr(WindowTitle, title); // 模糊匹配
                            if (strfind) {
                                AppendHwndText(retstring, retstringlen, p);
                                bret = true;
                            }
                        }
                        HWND child_hwnd = ::GetWindow(p, GW_CHILD);
                        if (child_hwnd != NULL) {
                            FindChildWnd(child_hwnd, title, NULL, retstring, false, true, process_name);
                        }
                    }
                } else {
                    auto WindowTitle = WindowTitleText(p);
                    if (safe_wcslen(WindowTitle) > 1) {
                        const wchar_t *strfind = wcsstr(WindowTitle, title); // 模糊匹配
                        if (strfind) {
                            AppendHwndText(retstring, retstringlen, p);
                            bret = true;
                        }
                    }
                    HWND child_hwnd = ::GetWindow(p, GW_CHILD);
                    if (child_hwnd != NULL) {
                        FindChildWnd(child_hwnd, title, NULL, retstring, false, true);
                    }
                }
            }
            p = ::GetWindow(p, GW_HWNDNEXT); // 获取下一个窗口
        }
        break;
    }
    case 18: // 2.窗口类名+//匹配可见的窗口
    {
        if (safe_wcslen(class_name) < 1)
            return false;
        HWND p = ::GetWindow(parent, GW_CHILD); // 获取桌面窗口的子窗口
        p = ::GetWindow(p, GW_HWNDFIRST);
        while (p != NULL) {
            if (::IsWindowVisible(p)) {
                if (process_name) // EnumWindowByProcess
                {
                    DWORD pid = 0;
                    GetWindowThreadProcessId(p, &pid);
                    if (EnumProcessbyName(pid, process_name)) {
                        auto WindowClassName = WindowClassNameText(p);
                        if (safe_wcslen(WindowClassName) > 1) {
                            const wchar_t *strfind = wcsstr(WindowClassName, class_name); // 模糊匹配
                            if (strfind) {
                                AppendHwndText(retstring, retstringlen, p);
                                bret = true;
                            }
                        }
                        HWND child_hwnd = ::GetWindow(p, GW_CHILD);
                        if (child_hwnd != NULL) {
                            FindChildWnd(child_hwnd, NULL, class_name, retstring, false, true, process_name);
                        }
                    }
                } else {
                    auto WindowClassName = WindowClassNameText(p);
                    if (safe_wcslen(WindowClassName) > 1) {
                        const wchar_t *strfind = wcsstr(WindowClassName, class_name); // 模糊匹配
                        if (strfind) {
                            AppendHwndText(retstring, retstringlen, p);
                            bret = true;
                        }
                    }
                    HWND child_hwnd = ::GetWindow(p, GW_CHILD);
                    if (child_hwnd != NULL) {
                        FindChildWnd(child_hwnd, NULL, class_name, retstring, false, true);
                    }
                }
            }
            p = ::GetWindow(p, GW_HWNDNEXT); // 获取下一个窗口
        }
        break;
    }
    case 19: ////1.窗口标题+2.窗口类名+匹配可见的窗口
    {
        if (safe_wcslen(class_name) < 1 && safe_wcslen(title) < 1)
            return false;
        HWND p = ::GetWindow(parent, GW_CHILD); // 获取桌面窗口的子窗口
        p = ::GetWindow(p, GW_HWNDFIRST);
        while (p != NULL) {
            if (::IsWindowVisible(p)) {
                if (process_name) // EnumWindowByProcess
                {
                    DWORD pid = 0;
                    GetWindowThreadProcessId(p, &pid);
                    if (EnumProcessbyName(pid, process_name)) {
                        auto WindowClassName = WindowClassNameText(p);
                        auto WindowTitle = WindowTitleText(p);
                        if (safe_wcslen(WindowClassName) > 1 && safe_wcslen(WindowTitle) > 1) {
                            const wchar_t *strfindclass = wcsstr(WindowClassName, class_name); // 模糊匹配
                            const wchar_t *strfindtitle = wcsstr(WindowTitle, title);          // 模糊匹配
                            if (strfindclass && strfindtitle) {
                                AppendHwndText(retstring, retstringlen, p);
                                bret = true;
                            }
                        }
                        HWND child_hwnd = ::GetWindow(p, GW_CHILD);
                        if (child_hwnd != NULL) {
                            FindChildWnd(child_hwnd, title, class_name, retstring, false, true, process_name);
                        }
                    }
                } else {
                    auto WindowClassName = WindowClassNameText(p);
                    auto WindowTitle = WindowTitleText(p);
                    if (safe_wcslen(WindowClassName) > 1 && safe_wcslen(WindowTitle) > 1) {
                        const wchar_t *strfindclass = wcsstr(WindowClassName, class_name); // 模糊匹配
                        const wchar_t *strfindtitle = wcsstr(WindowTitle, title);          // 模糊匹配
                        if (strfindclass && strfindtitle) {
                            AppendHwndText(retstring, retstringlen, p);
                            bret = true;
                        }
                    }
                    HWND child_hwnd = ::GetWindow(p, GW_CHILD);
                    if (child_hwnd != NULL) {
                        FindChildWnd(child_hwnd, title, class_name, retstring, false, true);
                    }
                }
            }
            p = ::GetWindow(p, GW_HWNDNEXT); // 获取下一个窗口
        }
        break;
    }
    case 20: // 4 : 只匹配指定父窗口的第一层孩子窗口+匹配可见的窗口
    {
        HWND p = ::GetWindow(parent, GW_CHILD); // 获取桌面窗口的子窗口
        p = ::GetWindow(p, GW_HWNDFIRST);
        while (p != NULL) {
            if (::IsWindowVisible(p)) {
                if (process_name) // EnumWindowByProcess
                {
                    DWORD pid = 0;
                    GetWindowThreadProcessId(p, &pid);
                    if (EnumProcessbyName(pid, process_name)) {
                    if (processpid != pid) // 只匹配指定映像的所对应的第一个进程.
                                               // 可能有很多同映像名的进程，只匹配第一个进程的.
                        {
                            if (indexpid < enum_process_success_count) {
                                indexpid++;
                                processpid = pid;
                                ClearHwndText(retstring, retstringlen); // 清空返回字符串
                            }
                        }
                        if (processpid == pid) {
                            AppendHwndText(retstring, retstringlen, p);
                            bret = true;
                            HWND child_hwnd = ::GetWindow(p, GW_CHILD);
                            if (child_hwnd != NULL) {
                                FindChildWnd(child_hwnd, NULL, NULL, retstring, false, true, process_name);
                            }
                        }
                    }
                } else {
                    AppendHwndText(retstring, retstringlen, p);
                    bret = true;
                }
            }
            p = ::GetWindow(p, GW_HWNDNEXT); // 获取下一个窗口
        }
        break;
    }
    case 21: // 1.窗口标题+4 : 只匹配指定父窗口的第一层孩子窗口+匹配可见的窗口
    {
        if (safe_wcslen(title) < 1)
            return false;
        HWND p = ::GetWindow(parent, GW_CHILD); // 获取桌面窗口的子窗口
        p = ::GetWindow(p, GW_HWNDFIRST);
        while (p != NULL) {
            if (::IsWindowVisible(p)) {
                if (process_name) // EnumWindowByProcess
                {
                    DWORD pid = 0;
                    GetWindowThreadProcessId(p, &pid);
                    if (EnumProcessbyName(pid, process_name)) {
                    if (processpid != pid) // 只匹配指定映像的所对应的第一个进程.
                                               // 可能有很多同映像名的进程，只匹配第一个进程的.
                        {
                            if (indexpid < enum_process_success_count) {
                                indexpid++;
                                processpid = pid;
                                ClearHwndText(retstring, retstringlen); // 清空返回字符串
                            }
                        }
                        if (processpid == pid) {
                            auto WindowTitle = WindowTitleText(p);
                            if (safe_wcslen(WindowTitle) > 1) {
                                const wchar_t *strfind = wcsstr(WindowTitle, title); // 模糊匹配
                                if (strfind) {
                                    AppendHwndText(retstring, retstringlen, p);
                                    bret = true;
                                }
                            }
                            HWND child_hwnd = ::GetWindow(p, GW_CHILD);
                            if (child_hwnd != NULL) {
                                FindChildWnd(child_hwnd, title, NULL, retstring, false, true, process_name);
                            }
                        }
                    }
                } else {
                    auto WindowTitle = WindowTitleText(p);
                    if (safe_wcslen(WindowTitle) > 1) {
                        const wchar_t *strfind = wcsstr(WindowTitle, title); // 模糊匹配
                        if (strfind) {
                            AppendHwndText(retstring, retstringlen, p);
                            bret = true;
                        }
                    }
                }
            }
            p = ::GetWindow(p, GW_HWNDNEXT); // 获取下一个窗口
        }
        break;
    }
    case 22: // 2.窗口类名+4 : 只匹配指定父窗口的第一层孩子窗口+匹配可见的窗口
    {
        if (safe_wcslen(class_name) < 1)
            return false;
        HWND p = ::GetWindow(parent, GW_CHILD); // 获取桌面窗口的子窗口
        p = ::GetWindow(p, GW_HWNDFIRST);
        while (p != NULL) {
            if (::IsWindowVisible(p)) {
                if (process_name) // EnumWindowByProcess
                {
                    DWORD pid = 0;
                    GetWindowThreadProcessId(p, &pid);
                    if (EnumProcessbyName(pid, process_name)) {
                    if (processpid != pid) // 只匹配指定映像的所对应的第一个进程.
                                               // 可能有很多同映像名的进程，只匹配第一个进程的.
                        {
                            if (indexpid < enum_process_success_count) {
                                indexpid++;
                                processpid = pid;
                                ClearHwndText(retstring, retstringlen); // 清空返回字符串
                            }
                        }
                        if (processpid == pid) {
                            auto WindowClassName = WindowClassNameText(p);
                            if (safe_wcslen(WindowClassName) > 1) {
                                const wchar_t *strfind = wcsstr(WindowClassName, class_name); // 模糊匹配
                                if (strfind) {
                                    AppendHwndText(retstring, retstringlen, p);
                                    bret = true;
                                }
                            }
                            HWND child_hwnd = ::GetWindow(p, GW_CHILD);
                            if (child_hwnd != NULL) {
                                FindChildWnd(child_hwnd, NULL, class_name, retstring, false, true, process_name);
                            }
                        }
                    }
                } else {
                    auto WindowClassName = WindowClassNameText(p);
                    if (safe_wcslen(WindowClassName) > 1) {
                        const wchar_t *strfind = wcsstr(WindowClassName, class_name); // 模糊匹配
                        if (strfind) {
                            AppendHwndText(retstring, retstringlen, p);
                            bret = true;
                        }
                    }
                }
            }
            p = ::GetWindow(p, GW_HWNDNEXT); // 获取下一个窗口
        }
        break;
    }
    case 23: // 1.窗口标题+2.窗口类名+4 :
             // 只匹配指定父窗口的第一层孩子窗口+16:匹配可见的窗口
    {
        if (safe_wcslen(class_name) < 1 && safe_wcslen(title) < 1)
            return false;
        HWND p = ::GetWindow(parent, GW_CHILD); // 获取桌面窗口的子窗口
        p = ::GetWindow(p, GW_HWNDFIRST);
        while (p != NULL) {
            if (::IsWindowVisible(p)) {
                if (process_name) // EnumWindowByProcess
                {
                    DWORD pid = 0;
                    GetWindowThreadProcessId(p, &pid);
                    if (EnumProcessbyName(pid, process_name)) {
                    if (processpid != pid) // 只匹配指定映像的所对应的第一个进程.
                                               // 可能有很多同映像名的进程，只匹配第一个进程的.
                        {
                            if (indexpid < enum_process_success_count) {
                                indexpid++;
                                processpid = pid;
                                ClearHwndText(retstring, retstringlen); // 清空返回字符串
                            }
                        }
                        if (processpid == pid) {
                            auto WindowClassName = WindowClassNameText(p);
                            auto WindowTitle = WindowTitleText(p);
                            if (safe_wcslen(WindowClassName) > 1 && safe_wcslen(WindowTitle) > 1) {
                                const wchar_t *strfindclass = wcsstr(WindowClassName, class_name); // 模糊匹配
                                const wchar_t *strfindtitle = wcsstr(WindowTitle, title);          // 模糊匹配
                                if (strfindclass && strfindtitle) {
                                    AppendHwndText(retstring, retstringlen, p);
                                    bret = true;
                                }
                            }
                            HWND child_hwnd = ::GetWindow(p, GW_CHILD);
                            if (child_hwnd != NULL) {
                                FindChildWnd(child_hwnd, title, class_name, retstring, false, true, process_name);
                            }
                        }
                    }
                } else {
                    auto WindowClassName = WindowClassNameText(p);
                    auto WindowTitle = WindowTitleText(p);
                    if (safe_wcslen(WindowClassName) > 1 && safe_wcslen(WindowTitle) > 1) {
                        const wchar_t *strfindclass = wcsstr(WindowClassName, class_name); // 模糊匹配
                        const wchar_t *strfindtitle = wcsstr(WindowTitle, title);          // 模糊匹配
                        if (strfindclass && strfindtitle) {
                            AppendHwndText(retstring, retstringlen, p);
                            bret = true;
                        }
                    }
                }
            }
            p = ::GetWindow(p, GW_HWNDNEXT); // 获取下一个窗口
        }
        break;
    }
    case 24: // 8 : 匹配所有者窗口为0的窗口,即顶级窗口+16.匹配可见的窗口
    {
        HWND p = ::GetWindow(parent, GW_CHILD); // 获取桌面窗口的子窗口
        p = ::GetWindow(p, GW_HWNDFIRST);
        while (p != NULL) {
            if (::IsWindowVisible(p) && ::GetWindow(p, GW_OWNER) == 0) {
                if (process_name) // EnumWindowByProcess
                {
                    DWORD pid = 0;
                    GetWindowThreadProcessId(p, &pid);
                    if (EnumProcessbyName(pid, process_name)) {
                        AppendHwndText(retstring, retstringlen, p);
                        bret = true;
                        HWND child_hwnd = ::GetWindow(p, GW_CHILD);
                        if (child_hwnd != NULL) {
                            FindChildWnd(child_hwnd, NULL, NULL, retstring, true, true, process_name);
                        }
                    }
                } else {
                    AppendHwndText(retstring, retstringlen, p);
                    bret = true;

                    HWND child_hwnd = ::GetWindow(p, GW_CHILD);
                    if (child_hwnd != NULL) {
                        FindChildWnd(child_hwnd, NULL, NULL, retstring, true, true);
                    }
                }
            }
            p = ::GetWindow(p, GW_HWNDNEXT); // 获取下一个窗口
        }
        break;
    }
    case 25: // 1.窗口标题+
             // 8:匹配所有者窗口为0的窗口,即顶级窗口+16:匹配可见的窗口
    {
        if (safe_wcslen(title) < 1)
            return false;
        HWND p = ::GetWindow(parent, GW_CHILD); // 获取桌面窗口的子窗口
        p = ::GetWindow(p, GW_HWNDFIRST);
        while (p != NULL) {
            if (::IsWindowVisible(p) && ::GetWindow(p, GW_OWNER) == 0) {
                if (process_name) // EnumWindowByProcess
                {
                    DWORD pid = 0;
                    GetWindowThreadProcessId(p, &pid);
                    if (EnumProcessbyName(pid, process_name)) {
                        auto WindowTitle = WindowTitleText(p);
                        if (safe_wcslen(WindowTitle) > 1) {
                            const wchar_t *strfind = wcsstr(WindowTitle, title); // 模糊匹配
                            if (strfind) {
                                AppendHwndText(retstring, retstringlen, p);
                                bret = true;
                            }
                        }
                        HWND child_hwnd = ::GetWindow(p, GW_CHILD);
                        if (child_hwnd != NULL) {
                            FindChildWnd(child_hwnd, title, NULL, retstring, true, true, process_name);
                        }
                    }
                } else {
                    auto WindowTitle = WindowTitleText(p);
                    if (safe_wcslen(WindowTitle) > 1) {
                        const wchar_t *strfind = wcsstr(WindowTitle, title); // 模糊匹配
                        if (strfind) {
                            AppendHwndText(retstring, retstringlen, p);
                            bret = true;
                        }
                    }
                    HWND child_hwnd = ::GetWindow(p, GW_CHILD);
                    if (child_hwnd != NULL) {
                        FindChildWnd(child_hwnd, title, NULL, retstring, true, true);
                    }
                }
            }
            p = ::GetWindow(p, GW_HWNDNEXT); // 获取下一个窗口
        }
        break;
    }
    case 26: // 2.窗口类名+
             // 8:匹配所有者窗口为0的窗口,即顶级窗口+16:匹配可见的窗口
    {
        if (safe_wcslen(class_name) < 1)
            return false;
        HWND p = ::GetWindow(parent, GW_CHILD); // 获取桌面窗口的子窗口
        p = ::GetWindow(p, GW_HWNDFIRST);
        while (p != NULL) {
            if (::IsWindowVisible(p) && ::GetWindow(p, GW_OWNER) == 0) {
                if (process_name) // EnumWindowByProcess
                {
                    DWORD pid = 0;
                    GetWindowThreadProcessId(p, &pid);
                    if (EnumProcessbyName(pid, process_name)) {
                        auto WindowClassName = WindowClassNameText(p);
                        if (safe_wcslen(WindowClassName) > 1) {
                            const wchar_t *strfind = wcsstr(WindowClassName, class_name); // 模糊匹配
                            if (strfind) {
                                AppendHwndText(retstring, retstringlen, p);
                                bret = true;
                            }
                        }
                        HWND child_hwnd = ::GetWindow(p, GW_CHILD);
                        if (child_hwnd != NULL) {
                            FindChildWnd(child_hwnd, NULL, class_name, retstring, true, true, process_name);
                        }
                    }
                } else {
                    auto WindowClassName = WindowClassNameText(p);
                    if (safe_wcslen(WindowClassName) > 1) {
                        const wchar_t *strfind = wcsstr(WindowClassName, class_name); // 模糊匹配
                        if (strfind) {
                            AppendHwndText(retstring, retstringlen, p);
                            bret = true;
                        }
                    }
                    HWND child_hwnd = ::GetWindow(p, GW_CHILD);
                    if (child_hwnd != NULL) {
                        FindChildWnd(child_hwnd, NULL, class_name, retstring, true, true);
                    }
                }
            }
            p = ::GetWindow(p, GW_HWNDNEXT); // 获取下一个窗口
        }
        break;
    }
    case 27: // 1.窗口标题+2.窗口类名+8:匹配所有者窗口为0的窗口,即顶级窗口+16.匹配可见的窗口
    {
        if (safe_wcslen(class_name) < 1 && safe_wcslen(title) < 1)
            return false;
        HWND p = ::GetWindow(parent, GW_CHILD); // 获取桌面窗口的子窗口
        p = ::GetWindow(p, GW_HWNDFIRST);
        while (p != NULL) {
            if (::IsWindowVisible(p) && ::GetWindow(p, GW_OWNER) == 0) {
                if (process_name) // EnumWindowByProcess
                {
                    DWORD pid = 0;
                    GetWindowThreadProcessId(p, &pid);
                    if (EnumProcessbyName(pid, process_name)) {
                        auto WindowClassName = WindowClassNameText(p);
                        auto WindowTitle = WindowTitleText(p);
                        if (safe_wcslen(WindowClassName) > 1 && safe_wcslen(WindowTitle) > 1) {
                            const wchar_t *strfindclass = wcsstr(WindowClassName, class_name); // 模糊匹配
                            const wchar_t *strfindtitle = wcsstr(WindowTitle, title);          // 模糊匹配
                            if (strfindclass && strfindtitle) {
                                AppendHwndText(retstring, retstringlen, p);
                                bret = true;
                            }
                        }
                        HWND child_hwnd = ::GetWindow(p, GW_CHILD);
                        if (child_hwnd != NULL) {
                            FindChildWnd(child_hwnd, title, class_name, retstring, true, true, process_name);
                        }
                    }
                } else {
                    auto WindowClassName = WindowClassNameText(p);
                    auto WindowTitle = WindowTitleText(p);
                    if (safe_wcslen(WindowClassName) > 1 && safe_wcslen(WindowTitle) > 1) {
                        const wchar_t *strfindclass = wcsstr(WindowClassName, class_name); // 模糊匹配
                        const wchar_t *strfindtitle = wcsstr(WindowTitle, title);          // 模糊匹配
                        if (strfindclass && strfindtitle) {
                            AppendHwndText(retstring, retstringlen, p);
                            bret = true;
                        }
                    }
                    HWND child_hwnd = ::GetWindow(p, GW_CHILD);
                    if (child_hwnd != NULL) {
                        FindChildWnd(child_hwnd, title, class_name, retstring, true, true);
                    }
                }
            }
            p = ::GetWindow(p, GW_HWNDNEXT); // 获取下一个窗口
        }
        break;
    }
    case 28: // 4 :
             // 只匹配指定父窗口的第一层孩子窗口+8:匹配所有者窗口为0的窗口,即顶级窗口+16:匹配可见的窗口
    {
        HWND p = ::GetWindow(parent, GW_CHILD); // 获取桌面窗口的子窗口
        p = ::GetWindow(p, GW_HWNDFIRST);
        while (p != NULL) {
            if (::IsWindowVisible(p) && ::GetWindow(p, GW_OWNER) == 0) {
                if (process_name) // EnumWindowByProcess
                {
                    DWORD pid = 0;
                    GetWindowThreadProcessId(p, &pid);
                    if (EnumProcessbyName(pid, process_name)) {
                    if (processpid != pid) // 只匹配指定映像的所对应的第一个进程.
                                               // 可能有很多同映像名的进程，只匹配第一个进程的.
                        {
                            if (indexpid < enum_process_success_count) {
                                indexpid++;
                                processpid = pid;
                                ClearHwndText(retstring, retstringlen); // 清空返回字符串
                            }
                        }
                        if (processpid == pid) {
                            AppendHwndText(retstring, retstringlen, p);
                            bret = true;
                            HWND child_hwnd = ::GetWindow(p, GW_CHILD);
                            if (child_hwnd != NULL) {
                                FindChildWnd(child_hwnd, NULL, NULL, retstring, true, true, process_name);
                            }
                        }
                    }
                } else {
                    AppendHwndText(retstring, retstringlen, p);
                    bret = true;
                }
            }
            p = ::GetWindow(p, GW_HWNDNEXT); // 获取下一个窗口
        }
        break;
    }
    case 29: ////1.窗口标题+4 :
             // 只匹配指定父窗口的第一层孩子窗口+8:匹配所有者窗口为0的窗口,即顶级窗口+16:匹配可见的窗口
    {
        if (safe_wcslen(title) < 1)
            return false;
        HWND p = ::GetWindow(parent, GW_CHILD); // 获取桌面窗口的子窗口
        p = ::GetWindow(p, GW_HWNDFIRST);
        while (p != NULL) {
            if (::IsWindowVisible(p) && ::GetWindow(p, GW_OWNER) == 0) {
                if (process_name) // EnumWindowByProcess
                {
                    DWORD pid = 0;
                    GetWindowThreadProcessId(p, &pid);
                    if (EnumProcessbyName(pid, process_name)) {
                    if (processpid != pid) // 只匹配指定映像的所对应的第一个进程.
                                               // 可能有很多同映像名的进程，只匹配第一个进程的.
                        {
                            if (indexpid < enum_process_success_count) {
                                indexpid++;
                                processpid = pid;
                                ClearHwndText(retstring, retstringlen); // 清空返回字符串
                            }
                        }
                        if (processpid == pid) {
                            auto WindowTitle = WindowTitleText(p);
                            if (safe_wcslen(WindowTitle) > 1) {
                                const wchar_t *strfind = wcsstr(WindowTitle, title); // 模糊匹配
                                if (strfind) {
                                    AppendHwndText(retstring, retstringlen, p);
                                    bret = true;
                                }
                            }
                            HWND child_hwnd = ::GetWindow(p, GW_CHILD);
                            if (child_hwnd != NULL) {
                                FindChildWnd(child_hwnd, title, NULL, retstring, true, true, process_name);
                            }
                        }
                    }
                } else {
                    auto WindowTitle = WindowTitleText(p);
                    if (safe_wcslen(WindowTitle) > 1) {
                        const wchar_t *strfind = wcsstr(WindowTitle, title); // 模糊匹配
                        if (strfind) {
                            AppendHwndText(retstring, retstringlen, p);
                            bret = true;
                        }
                    }
                }
            }
            p = ::GetWindow(p, GW_HWNDNEXT); // 获取下一个窗口
        }
        break;
    }
    case 30: // 2.窗口类名+4 :
             // 只匹配指定父窗口的第一层孩子窗口+8:匹配所有者窗口为0的窗口,即顶级窗口+16:匹配可见的窗口
    {
        if (safe_wcslen(class_name) < 1)
            return false;
        HWND p = ::GetWindow(parent, GW_CHILD); // 获取桌面窗口的子窗口
        p = ::GetWindow(p, GW_HWNDFIRST);
        while (p != NULL) {
            if (::IsWindowVisible(p) && ::GetWindow(p, GW_OWNER) == 0) {
                if (process_name) // EnumWindowByProcess
                {
                    DWORD pid = 0;
                    GetWindowThreadProcessId(p, &pid);
                    if (EnumProcessbyName(pid, process_name)) {
                    if (processpid != pid) // 只匹配指定映像的所对应的第一个进程.
                                               // 可能有很多同映像名的进程，只匹配第一个进程的.
                        {
                            if (indexpid < enum_process_success_count) {
                                indexpid++;
                                processpid = pid;
                                ClearHwndText(retstring, retstringlen); // 清空返回字符串
                            }
                        }
                        if (processpid == pid) {
                            auto WindowClassName = WindowClassNameText(p);
                            if (safe_wcslen(WindowClassName) > 1) {
                                const wchar_t *strfind = wcsstr(WindowClassName, class_name); // 模糊匹配
                                if (strfind) {
                                    AppendHwndText(retstring, retstringlen, p);
                                    bret = true;
                                }
                            }
                            HWND child_hwnd = ::GetWindow(p, GW_CHILD);
                            if (child_hwnd != NULL) {
                                FindChildWnd(child_hwnd, NULL, class_name, retstring, true, true, process_name);
                            }
                        }
                    }
                } else {
                    auto WindowClassName = WindowClassNameText(p);
                    if (safe_wcslen(WindowClassName) > 1) {
                        const wchar_t *strfind = wcsstr(WindowClassName, class_name); // 模糊匹配
                        if (strfind) {
                            AppendHwndText(retstring, retstringlen, p);
                            bret = true;
                        }
                    }
                }
            }
            p = ::GetWindow(p, GW_HWNDNEXT); // 获取下一个窗口
        }
        break;
    }
    case 31: // 1.窗口标题+2.窗口类名+4 :
             // 只匹配指定父窗口的第一层孩子窗口+8:匹配所有者窗口为0的窗口,即顶级窗口+16.匹配可见的窗口
    {
        if (safe_wcslen(class_name) < 1 && safe_wcslen(title) < 1)
            return false;
        HWND p = ::GetWindow(parent, GW_CHILD); // 获取桌面窗口的子窗口
        p = ::GetWindow(p, GW_HWNDFIRST);
        while (p != NULL) {
            if (::IsWindowVisible(p) && ::GetWindow(p, GW_OWNER) == 0) {
                if (process_name) // EnumWindowByProcess
                {
                    DWORD pid = 0;
                    GetWindowThreadProcessId(p, &pid);
                    if (EnumProcessbyName(pid, process_name)) {
                    if (processpid != pid) // 只匹配指定映像的所对应的第一个进程.
                                               // 可能有很多同映像名的进程，只匹配第一个进程的.
                        {
                            if (indexpid < enum_process_success_count) {
                                indexpid++;
                                processpid = pid;
                                ClearHwndText(retstring, retstringlen); // 清空返回字符串
                            }
                        }
                        if (processpid == pid) {
                            auto WindowClassName = WindowClassNameText(p);
                            auto WindowTitle = WindowTitleText(p);
                            if (safe_wcslen(WindowClassName) > 1 && safe_wcslen(WindowTitle) > 1) {
                                const wchar_t *strfindclass = wcsstr(WindowClassName, class_name); // 模糊匹配
                                const wchar_t *strfindtitle = wcsstr(WindowTitle, title);          // 模糊匹配
                                if (strfindclass && strfindtitle) {
                                    AppendHwndText(retstring, retstringlen, p);
                                    bret = true;
                                }
                            }
                            HWND child_hwnd = ::GetWindow(p, GW_CHILD);
                            if (child_hwnd != NULL) {
                                FindChildWnd(child_hwnd, title, class_name, retstring, true, true, process_name);
                            }
                        }
                    }
                } else {
                    auto WindowClassName = WindowClassNameText(p);
                    auto WindowTitle = WindowTitleText(p);
                    if (safe_wcslen(WindowClassName) > 1 && safe_wcslen(WindowTitle) > 1) {
                        const wchar_t *strfindclass = wcsstr(WindowClassName, class_name); // 模糊匹配
                        const wchar_t *strfindtitle = wcsstr(WindowTitle, title);          // 模糊匹配
                        if (strfindclass && strfindtitle) {
                            AppendHwndText(retstring, retstringlen, p);
                            bret = true;
                        }
                    }
                }
            }
            p = ::GetWindow(p, GW_HWNDNEXT); // 获取下一个窗口
        }
        break;
    }
    default:
        return bret;
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
