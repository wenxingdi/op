// OpUia.cpp: UIA 元素层最小集（批 H 2026-11-23）
//
// 定位：图色之外的第二条路——直接读 UI 元素树，不依赖分辨率 / 主题 / DPI。
// 典型用法：先 UiaFind 定位面板矩形，再在该矩形内 FindPic/FindColor（搜索面积大幅缩小）；
// 或者直接读控件状态（可用/勾选/文本），完全绕开 OCR 的误识风险。
//
// 边界（文档里必须写清楚，别让人踩空）：
//   * 只对**暴露 UIA 的程序**有效：Win32 原生控件 / WinForms / WPF / Qt（开 accessibility）/
//     Chromium 系浏览器。DX 自绘的游戏界面节点树是空的，用不了。
//   * 元素 id 是**本进程内**的注册表序号，而且 op_com 与 op_c_api 各自链一份 libop
//     → 两份注册表互不可见。跨层拿 id 必然找不到，这是已知限制（与热键共享内存不同）。
//   * 遍历有节点上限 kMaxNodes，超大窗口不会把调用卡死，但可能截断。
//   * 坐标是**屏幕物理像素**（UIA 给的就是物理像素），与绑定态客户区坐标不是一套。

#include "OpCaptureHelpers.h" // guard_exceptions
#include "OpContext.h"
#include "OpResult.h"

#include "base/Utils.h"

#include <libop.h>

#include <Windows.h>
#include <UIAutomation.h>
#include <wrl/client.h>

#include <cstdlib>
#include <cwctype>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

using Microsoft::WRL::ComPtr;

constexpr int kMaxNodes = 4000;        // 单次遍历的节点上限
constexpr DWORD kPollIntervalMs = 100; // UiaFind 的轮询间隔

struct UiaState {
    std::mutex mtx;
    ComPtr<IUIAutomation> automation;
    std::unordered_map<long, ComPtr<IUIAutomationElement>> elements;
    long next_id = 1;
    bool init_failed = false;
};

UiaState &state() {
    static UiaState s;
    return s;
}

// 取（必要时创建）IUIAutomation。失败返回空并写 err。
ComPtr<IUIAutomation> acquire_automation(std::wstring &err) {
    auto &s = state();
    std::lock_guard<std::mutex> lk(s.mtx);
    if (s.automation)
        return s.automation;
    if (s.init_failed) {
        err = L"UIAutomation 初始化此前已失败";
        return nullptr;
    }
    // 宿主可能已经初始化过 COM（WinForms 主线程是 STA）：RPC_E_CHANGED_MODE 表示
    // "已经初始化过了"，继续用即可。把它当失败会导致在 .NET 宿主里永远调不通。
    const HRESULT hr_com = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr_com) && hr_com != RPC_E_CHANGED_MODE) {
        s.init_failed = true;
        err = L"CoInitializeEx 失败（hr=" + std::to_wstring(static_cast<long>(hr_com)) + L"）";
        return nullptr;
    }
    ComPtr<IUIAutomation> uia;
    const HRESULT hr =
        ::CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&uia));
    if (FAILED(hr) || !uia) {
        s.init_failed = true;
        err = L"CoCreateInstance(CUIAutomation) 失败（hr=" + std::to_wstring(static_cast<long>(hr)) + L"）";
        return nullptr;
    }
    s.automation = uia;
    return s.automation;
}

// ---- JSON 小工具（只处理本模块用到的形状，不引第三方库）----

void json_escape_to(std::wstring &out, const std::wstring &text) {
    for (const wchar_t ch : text) {
        switch (ch) {
        case L'"':
            out += L"\\\"";
            break;
        case L'\\':
            out += L"\\\\";
            break;
        case L'\n':
            out += L"\\n";
            break;
        case L'\r':
            out += L"\\r";
            break;
        case L'\t':
            out += L"\\t";
            break;
        default:
            // 其余控制字符直接换成空格：JSON 里它们没有意义，写出去反而让解析器报错
            out += (ch < 0x20) ? L' ' : ch;
            break;
        }
    }
}

std::wstring json_str(const std::wstring &text) {
    std::wstring out = L"\"";
    json_escape_to(out, text);
    out += L"\"";
    return out;
}

std::wstring bstr_to_wstring(BSTR value) {
    std::wstring out = value ? value : L"";
    if (value)
        ::SysFreeString(value);
    return out;
}

std::wstring lower(std::wstring text) {
    for (auto &ch : text)
        ch = static_cast<wchar_t>(::towlower(ch));
    return text;
}

std::wstring trim(const std::wstring &text) {
    const size_t begin = text.find_first_not_of(L" \t\r\n");
    if (begin == std::wstring::npos)
        return L"";
    const size_t end = text.find_last_not_of(L" \t\r\n");
    return text.substr(begin, end - begin + 1);
}

const wchar_t *control_type_name(CONTROLTYPEID type) {
    switch (type) {
    case UIA_ButtonControlTypeId:
        return L"Button";
    case UIA_CalendarControlTypeId:
        return L"Calendar";
    case UIA_CheckBoxControlTypeId:
        return L"CheckBox";
    case UIA_ComboBoxControlTypeId:
        return L"ComboBox";
    case UIA_EditControlTypeId:
        return L"Edit";
    case UIA_HyperlinkControlTypeId:
        return L"Hyperlink";
    case UIA_ImageControlTypeId:
        return L"Image";
    case UIA_ListItemControlTypeId:
        return L"ListItem";
    case UIA_ListControlTypeId:
        return L"List";
    case UIA_MenuControlTypeId:
        return L"Menu";
    case UIA_MenuBarControlTypeId:
        return L"MenuBar";
    case UIA_MenuItemControlTypeId:
        return L"MenuItem";
    case UIA_PaneControlTypeId:
        return L"Pane";
    case UIA_ProgressBarControlTypeId:
        return L"ProgressBar";
    case UIA_RadioButtonControlTypeId:
        return L"RadioButton";
    case UIA_ScrollBarControlTypeId:
        return L"ScrollBar";
    case UIA_SliderControlTypeId:
        return L"Slider";
    case UIA_SpinnerControlTypeId:
        return L"Spinner";
    case UIA_StatusBarControlTypeId:
        return L"StatusBar";
    case UIA_TabControlTypeId:
        return L"Tab";
    case UIA_TabItemControlTypeId:
        return L"TabItem";
    case UIA_TableControlTypeId:
        return L"Table";
    case UIA_TextControlTypeId:
        return L"Text";
    case UIA_ToolBarControlTypeId:
        return L"ToolBar";
    case UIA_TreeControlTypeId:
        return L"Tree";
    case UIA_TreeItemControlTypeId:
        return L"TreeItem";
    case UIA_WindowControlTypeId:
        return L"Window";
    case UIA_DocumentControlTypeId:
        return L"Document";
    case UIA_GroupControlTypeId:
        return L"Group";
    case UIA_DataItemControlTypeId:
        return L"DataItem";
    case UIA_HeaderControlTypeId:
        return L"Header";
    case UIA_HeaderItemControlTypeId:
        return L"HeaderItem";
    case UIA_TitleBarControlTypeId:
        return L"TitleBar";
    case UIA_SeparatorControlTypeId:
        return L"Separator";
    case UIA_ThumbControlTypeId:
        return L"Thumb";
    case UIA_CustomControlTypeId:
        return L"Custom";
    default:
        return L"Unknown";
    }
}

// 读元素的可编辑/显示值（ValuePattern）；没有该模式的控件返回空串。
std::wstring element_value(IUIAutomationElement *el) {
    ComPtr<IUnknown> pattern;
    if (FAILED(el->GetCurrentPattern(UIA_ValuePatternId, &pattern)) || !pattern)
        return L"";
    ComPtr<IUIAutomationValuePattern> value_pattern;
    if (FAILED(pattern.As(&value_pattern)) || !value_pattern)
        return L"";
    BSTR value = nullptr;
    if (FAILED(value_pattern->get_CurrentValue(&value)) || !value)
        return L"";
    return bstr_to_wstring(value);
}

// 元素 -> JSON 对象。with_value=true 时额外读 ValuePattern（有额外开销，按需开）。
void append_element_json(std::wstring &out, IUIAutomationElement *el, long id, bool with_value) {
    BSTR name = nullptr;
    BSTR cls = nullptr;
    BSTR aid = nullptr;
    el->get_CurrentName(&name);
    el->get_CurrentClassName(&cls);
    el->get_CurrentAutomationId(&aid);
    CONTROLTYPEID type = 0;
    el->get_CurrentControlType(&type);
    RECT rect = {};
    el->get_CurrentBoundingRectangle(&rect);
    BOOL enabled = FALSE;
    el->get_CurrentIsEnabled(&enabled);
    BOOL offscreen = FALSE;
    el->get_CurrentIsOffscreen(&offscreen);

    out += L"{\"id\":";
    out += std::to_wstring(id);
    out += L",\"name\":";
    out += json_str(bstr_to_wstring(name));
    out += L",\"class\":";
    out += json_str(bstr_to_wstring(cls));
    out += L",\"aid\":";
    out += json_str(bstr_to_wstring(aid));
    out += L",\"type\":";
    out += std::to_wstring(static_cast<long>(type));
    out += L",\"type_name\":";
    out += json_str(control_type_name(type));
    out += L",\"rect\":[";
    out += std::to_wstring(rect.left);
    out += L",";
    out += std::to_wstring(rect.top);
    out += L",";
    out += std::to_wstring(rect.right);
    out += L",";
    out += std::to_wstring(rect.bottom);
    out += L"]";
    out += L",\"enabled\":";
    out += (enabled ? L"1" : L"0");
    out += L",\"offscreen\":";
    out += (offscreen ? L"1" : L"0");
    if (with_value) {
        out += L",\"value\":";
        out += json_str(element_value(el));
    }
    out += L"}";
}

long register_element(const ComPtr<IUIAutomationElement> &el) {
    auto &s = state();
    std::lock_guard<std::mutex> lk(s.mtx);
    const long id = s.next_id++;
    s.elements[id] = el;
    return id;
}

ComPtr<IUIAutomationElement> lookup_element(long id) {
    auto &s = state();
    std::lock_guard<std::mutex> lk(s.mtx);
    const auto it = s.elements.find(id);
    return it == s.elements.end() ? nullptr : it->second;
}

// 收集 scope 下全部后代（上限 kMaxNodes）
void collect_descendants(IUIAutomation *uia, IUIAutomationElement *scope,
                         std::vector<ComPtr<IUIAutomationElement>> &out) {
    ComPtr<IUIAutomationCondition> cond;
    if (FAILED(uia->CreateTrueCondition(&cond)) || !cond)
        return;
    ComPtr<IUIAutomationElementArray> arr;
    if (FAILED(scope->FindAll(TreeScope_Descendants, cond.Get(), &arr)) || !arr)
        return;
    int len = 0;
    arr->get_Length(&len);
    for (int i = 0; i < len && static_cast<int>(out.size()) < kMaxNodes; ++i) {
        ComPtr<IUIAutomationElement> el;
        if (SUCCEEDED(arr->GetElement(i, &el)) && el)
            out.push_back(el);
    }
}

// ---- 选择器 ----
// 语法：`name=确定 >> class=Button`；`name*=确定` 表示包含匹配；
// 键支持 name / class / id(automationId) / type(ControlType 名或数字) / index(0 基，只能放最后)。
struct SelectorStep {
    std::wstring key;
    std::wstring value;
    bool contains = false;
    long index = -1;
};

std::vector<SelectorStep> parse_selector(const std::wstring &text, std::wstring &err) {
    std::vector<SelectorStep> steps;
    size_t pos = 0;
    while (pos <= text.size()) {
        const size_t sep = text.find(L">>", pos);
        std::wstring part = trim(text.substr(pos, sep == std::wstring::npos ? std::wstring::npos : sep - pos));
        if (!part.empty()) {
            const size_t eq = part.find(L'=');
            if (eq == std::wstring::npos || eq == 0) {
                err = L"选择器缺少 '='（片段: " + part + L"）";
                return {};
            }
            std::wstring key = trim(part.substr(0, eq));
            std::wstring value = trim(part.substr(eq + 1));
            SelectorStep step;
            if (!key.empty() && key.back() == L'*') {
                step.contains = true;
                key.pop_back();
            }
            step.key = lower(trim(key));
            step.value = value;
            if (step.key == L"index") {
                step.index = _wtoi(value.c_str());
                if (step.index < 0) {
                    err = L"index 必须 >= 0";
                    return {};
                }
            } else if (step.key != L"name" && step.key != L"class" && step.key != L"id" && step.key != L"type") {
                err = L"未知选择器键: " + step.key + L"（支持 name/class/id/type/index）";
                return {};
            }
            steps.push_back(std::move(step));
        }
        if (sep == std::wstring::npos)
            break;
        pos = sep + 2;
    }
    if (steps.empty())
        err = L"选择器为空";
    return steps;
}

bool match_step(IUIAutomationElement *el, const SelectorStep &step) {
    std::wstring current;
    if (step.key == L"name") {
        BSTR b = nullptr;
        el->get_CurrentName(&b);
        current = bstr_to_wstring(b);
    } else if (step.key == L"class") {
        BSTR b = nullptr;
        el->get_CurrentClassName(&b);
        current = bstr_to_wstring(b);
    } else if (step.key == L"id") {
        BSTR b = nullptr;
        el->get_CurrentAutomationId(&b);
        current = bstr_to_wstring(b);
    } else if (step.key == L"type") {
        // 允许写名字（Button）或数字（50000）
        if (!step.value.empty() && step.value.find_first_not_of(L"0123456789") == std::wstring::npos) {
            CONTROLTYPEID type = 0;
            el->get_CurrentControlType(&type);
            return static_cast<long>(type) == _wtoi(step.value.c_str());
        }
        CONTROLTYPEID type = 0;
        el->get_CurrentControlType(&type);
        current = control_type_name(type);
    } else {
        return false;
    }
    if (step.contains)
        return lower(current).find(lower(step.value)) != std::wstring::npos;
    return lower(current) == lower(step.value);
}

// 逐级过滤：每一级都在上一级结果的**后代**里找。
bool find_elements(IUIAutomation *uia, IUIAutomationElement *root, const std::vector<SelectorStep> &steps,
                   std::vector<ComPtr<IUIAutomationElement>> &out) {
    std::vector<ComPtr<IUIAutomationElement>> current;
    current.push_back(root);
    for (const auto &step : steps) {
        if (step.index >= 0) {
            // index 作用在上一级结果上（只能放最后一步，parse 时不强制，但语义如此）
            if (step.index >= static_cast<long>(current.size()))
                return false;
            ComPtr<IUIAutomationElement> pick = current[static_cast<size_t>(step.index)];
            current.clear();
            current.push_back(pick);
            continue;
        }
        std::vector<ComPtr<IUIAutomationElement>> next;
        for (const auto &parent : current) {
            if (!parent)
                continue;
            std::vector<ComPtr<IUIAutomationElement>> all;
            collect_descendants(uia, parent.Get(), all);
            for (const auto &el : all) {
                if (el && match_step(el.Get(), step)) {
                    next.push_back(el);
                    if (static_cast<int>(next.size()) >= kMaxNodes)
                        break;
                }
            }
        }
        current = std::move(next);
        if (current.empty())
            return false;
    }
    out = std::move(current);
    return true;
}

ComPtr<IUIAutomationElement> element_from_hwnd(IUIAutomation *uia, HWND hwnd) {
    ComPtr<IUIAutomationElement> el;
    if (FAILED(uia->ElementFromHandle(hwnd, &el)))
        return nullptr;
    return el;
}

std::wstring fail_json(const std::wstring &err) {
    return L"{\"ok\":0,\"error\":" + json_str(err) + L"}";
}

void dump_children(IUIAutomation *uia, IUIAutomationElement *parent, int depth, int max_depth, std::wstring &out,
                   int &count, bool &first) {
    if (depth > max_depth || count >= kMaxNodes)
        return;
    ComPtr<IUIAutomationCondition> cond;
    if (FAILED(uia->CreateTrueCondition(&cond)) || !cond)
        return;
    ComPtr<IUIAutomationElementArray> arr;
    if (FAILED(parent->FindAll(TreeScope_Children, cond.Get(), &arr)) || !arr)
        return;
    int len = 0;
    arr->get_Length(&len);
    for (int i = 0; i < len && count < kMaxNodes; ++i) {
        ComPtr<IUIAutomationElement> el;
        if (FAILED(arr->GetElement(i, &el)) || !el)
            continue;
        if (!first)
            out += L",";
        first = false;
        ++count;
        std::wstring item;
        append_element_json(item, el.Get(), register_element(el), false);
        // 在结尾的 '}' 前插入 depth
        item.insert(item.size() - 1, L",\"depth\":" + std::to_wstring(depth));
        out += item;
        dump_children(uia, el.Get(), depth + 1, max_depth, out, count, first);
    }
}

} // namespace

// ===================== op 门面 =====================

void op::Op::UiaDump(LONG_PTR hwnd, long max_depth, std::wstring &retjson) {
    retjson = L"{\"ok\":0}";
    internal::guard_exceptions("UiaDump", [&]() {
        std::wstring err;
        auto uia = acquire_automation(err);
        if (!uia) {
            retjson = fail_json(err);
            return;
        }
        const HWND target = hwnd ? reinterpret_cast<HWND>(hwnd) : ::GetForegroundWindow();
        if (!target || !::IsWindow(target)) {
            retjson = fail_json(L"窗口句柄无效（hwnd=0 时取前台窗口，当前无前台窗口）");
            return;
        }
        auto root = element_from_hwnd(uia.Get(), target);
        if (!root) {
            retjson = fail_json(L"ElementFromHandle 失败（窗口可能已销毁或不支持 UIA）");
            return;
        }
        const int depth_limit = max_depth < 0 ? 0 : static_cast<int>(max_depth);
        std::wstring out = L"{\"ok\":1,\"nodes\":[";
        int count = 0;
        bool first = true;
        dump_children(uia.Get(), root.Get(), 0, depth_limit, out, count, first);
        out += L"],\"count\":" + std::to_wstring(count) + L"}";
        retjson = out;
    });
}

void op::Op::UiaFind(LONG_PTR hwnd, const wchar_t *selector, long timeout_ms, std::wstring &retjson) {
    retjson = L"{\"ok\":0}";
    internal::guard_exceptions("UiaFind", [&]() {
        std::wstring err;
        auto uia = acquire_automation(err);
        if (!uia) {
            retjson = fail_json(err);
            return;
        }
        const auto steps = parse_selector(selector ? selector : L"", err);
        if (steps.empty()) {
            retjson = fail_json(err.empty() ? L"选择器为空" : err);
            return;
        }
        const HWND target = hwnd ? reinterpret_cast<HWND>(hwnd) : ::GetForegroundWindow();
        if (!target || !::IsWindow(target)) {
            retjson = fail_json(L"窗口句柄无效（hwnd=0 时取前台窗口，当前无前台窗口）");
            return;
        }
        auto root = element_from_hwnd(uia.Get(), target);
        if (!root) {
            retjson = fail_json(L"ElementFromHandle 失败（窗口可能已销毁或不支持 UIA）");
            return;
        }
        const DWORD deadline = timeout_ms > 0 ? ::GetTickCount() + static_cast<DWORD>(timeout_ms) : 0;
        std::vector<ComPtr<IUIAutomationElement>> found;
        for (;;) {
            found.clear();
            const bool ok = find_elements(uia.Get(), root.Get(), steps, found);
            if (ok && !found.empty())
                break;
            if (deadline == 0 || ::GetTickCount() >= deadline)
                break;
            ::Sleep(kPollIntervalMs);
        }
        std::wstring out = L"{\"ok\":1,\"count\":" + std::to_wstring(found.size()) + L",\"elements\":[";
        for (size_t i = 0; i < found.size(); ++i) {
            if (i)
                out += L",";
            append_element_json(out, found[i].Get(), register_element(found[i]), true);
        }
        out += L"]}";
        retjson = out;
    });
}

void op::Op::UiaFromPoint(long x, long y, std::wstring &retjson) {
    retjson = L"{\"ok\":0}";
    internal::guard_exceptions("UiaFromPoint", [&]() {
        std::wstring err;
        auto uia = acquire_automation(err);
        if (!uia) {
            retjson = fail_json(err);
            return;
        }
        POINT pt = {static_cast<LONG>(x), static_cast<LONG>(y)};
        ComPtr<IUIAutomationElement> el;
        if (FAILED(uia->ElementFromPoint(pt, &el)) || !el) {
            retjson = fail_json(L"ElementFromPoint 失败（该点无元素）");
            return;
        }
        std::wstring out = L"{\"ok\":1,\"element\":";
        append_element_json(out, el.Get(), register_element(el), true);
        out += L"}";
        retjson = out;
    });
}

void op::Op::UiaInvoke(long element_id, long *ret) {
    internal::set_result(ret, 0L);
    internal::guard_exceptions("UiaInvoke", [&]() {
        auto el = lookup_element(element_id);
        if (!el) {
            setlog(L"UiaInvoke: 元素 id 不存在或已释放（%ld）", element_id);
            return;
        }
        ComPtr<IUnknown> pattern;
        if (SUCCEEDED(el->GetCurrentPattern(UIA_InvokePatternId, &pattern)) && pattern) {
            ComPtr<IUIAutomationInvokePattern> invoke;
            if (SUCCEEDED(pattern.As(&invoke)) && invoke && SUCCEEDED(invoke->Invoke())) {
                internal::set_result(ret, 1L);
                return;
            }
        }
        // 退路：复选框/开关类控件没有 Invoke 模式，用 Toggle 兜
        if (SUCCEEDED(el->GetCurrentPattern(UIA_TogglePatternId, &pattern)) && pattern) {
            ComPtr<IUIAutomationTogglePattern> toggle;
            if (SUCCEEDED(pattern.As(&toggle)) && toggle && SUCCEEDED(toggle->Toggle())) {
                internal::set_result(ret, 1L);
                return;
            }
        }
        setlog(L"UiaInvoke: 该元素不支持 Invoke/Toggle 模式（%ld）", element_id);
    });
}

void op::Op::UiaGetValue(long element_id, std::wstring &retstr) {
    retstr.clear();
    internal::guard_exceptions("UiaGetValue", [&]() {
        auto el = lookup_element(element_id);
        if (!el)
            return;
        retstr = element_value(el.Get());
    });
}

void op::Op::UiaSetValue(long element_id, const wchar_t *value, long *ret) {
    internal::set_result(ret, 0L);
    internal::guard_exceptions("UiaSetValue", [&]() {
        auto el = lookup_element(element_id);
        if (!el) {
            setlog(L"UiaSetValue: 元素 id 不存在或已释放（%ld）", element_id);
            return;
        }
        ComPtr<IUnknown> pattern;
        if (FAILED(el->GetCurrentPattern(UIA_ValuePatternId, &pattern)) || !pattern) {
            setlog(L"UiaSetValue: 该元素不支持 ValuePattern（%ld）", element_id);
            return;
        }
        ComPtr<IUIAutomationValuePattern> value_pattern;
        if (FAILED(pattern.As(&value_pattern)) || !value_pattern)
            return;
        BSTR text = ::SysAllocString(value ? value : L"");
        const HRESULT hr = value_pattern->SetValue(text);
        ::SysFreeString(text);
        if (SUCCEEDED(hr))
            internal::set_result(ret, 1L);
        else
            setlog(L"UiaSetValue: SetValue 失败（hr=%ld，元素可能只读）", static_cast<long>(hr));
    });
}

void op::Op::UiaRelease(long element_id, long *ret) {
    internal::set_result(ret, 0L);
    auto &s = state();
    std::lock_guard<std::mutex> lk(s.mtx);
    if (s.elements.erase(element_id) > 0)
        internal::set_result(ret, 1L);
}

void op::Op::UiaReleaseAll(long *ret) {
    internal::set_result(ret, 0L);
    auto &s = state();
    std::lock_guard<std::mutex> lk(s.mtx);
    internal::set_result(ret, static_cast<long>(s.elements.size()));
    s.elements.clear();
}
