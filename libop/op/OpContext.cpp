#include "OpContext.h"

#include "base/Environment.h"
#include "base/Utils.h"

#include <Windows.h>
#include <cstdio>
#include <filesystem>
#include <system_error>
#include <vector>

namespace {

std::wstring current_directory() {
    std::error_code ec;
    auto path = std::filesystem::current_path(ec);
    if (!ec)
        return path.wstring();

    const DWORD required = ::GetCurrentDirectoryW(0, nullptr);
    if (required == 0)
        return L"";

    std::vector<wchar_t> buffer(required, L'\0');
    const DWORD copied = ::GetCurrentDirectoryW(required, buffer.data());
    if (copied == 0 || copied >= required)
        return L"";

    return std::wstring(buffer.data(), copied);
}

// 进程 DPI 感知现状（老 API，Win2000+ 可用，不需要版本宏）。
bool process_dpi_aware() {
    return ::IsProcessDPIAware() != FALSE;
}

// 系统 DPI 与缩放比。提升为 DPI 感知后读到的是物理 DPI（150% -> 144）。
std::string system_dpi_text() {
    HDC screen = ::GetDC(nullptr);
    if (!screen)
        return "";
    const int dpi = ::GetDeviceCaps(screen, LOGPIXELSX);
    ::ReleaseDC(nullptr, screen);
    if (dpi <= 0)
        return "";

    char buf[64];
    std::snprintf(buf, sizeof(buf), "system dpi=%d (scale %d%%)", dpi, dpi * 100 / 96);
    return buf;
}

} // namespace

op::internal::OpContext::OpContext(int client_id) : id(client_id) {
    // 将进程默认 DPI 感知设置为系统 DPI 感知。
    // 该调用是进程级、一次性生效：它会把宿主进程的坐标语义从"被系统虚拟化
    // 的缩放后坐标"改成"物理像素"。脚本若混用了提升之前取到的坐标，会整体
    // 偏移一个缩放比（如 150% 缩放下偏 1.5 倍），现场表现是"点了但没点中"。
    // 故记录一行，便于把这类偏移直接归因到 DPI，而不是去查绑定/找色。
    const bool dpi_aware_before = process_dpi_aware();
    ::SetProcessDPIAware();
    if (!dpi_aware_before && process_dpi_aware()) {
        setlog("dpi: process elevated from DPI-unaware to DPI-aware, %s; "
               "window/client rects are physical pixels now",
               system_dpi_text().c_str());
    } else {
        // 提升只发生在首个实例上，而那时日志往往还没打开，故后续实例
        // 继续记录当前状态，保证这条信息随时可取。
        setlog("dpi: process is DPI-aware, %s; window/client rects are physical pixels",
               system_dpi_text().c_str());
    }

    // 初始化目录
    curr_path = current_directory();
    image_proc._curr_path = curr_path;

    // 初始化键码表
    vkmap[L"back"] = VK_BACK;
    vkmap[L"ctrl"] = VK_CONTROL;
    vkmap[L"lctrl"] = VK_LCONTROL;
    vkmap[L"rctrl"] = VK_RCONTROL;
    vkmap[L"alt"] = VK_MENU;
    vkmap[L"lalt"] = VK_LMENU;
    vkmap[L"ralt"] = VK_RMENU;
    vkmap[L"shift"] = VK_SHIFT;
    vkmap[L"lshift"] = VK_LSHIFT;
    vkmap[L"rshift"] = VK_RSHIFT;
    vkmap[L"win"] = VK_LWIN;
    vkmap[L"lwin"] = VK_LWIN;
    vkmap[L"rwin"] = VK_RWIN;
    vkmap[L"space"] = VK_SPACE;
    vkmap[L"cap"] = VK_CAPITAL;
    vkmap[L"tab"] = VK_TAB;
    vkmap[L"esc"] = VK_ESCAPE;
    vkmap[L"enter"] = VK_RETURN;
    vkmap[L"up"] = VK_UP;
    vkmap[L"down"] = VK_DOWN;
    vkmap[L"left"] = VK_LEFT;
    vkmap[L"right"] = VK_RIGHT;
    vkmap[L"menu"] = VK_APPS;
    vkmap[L"print"] = VK_SNAPSHOT;
    vkmap[L"insert"] = VK_INSERT;
    vkmap[L"delete"] = VK_DELETE;
    vkmap[L"pause"] = VK_PAUSE;
    vkmap[L"scroll"] = VK_SCROLL;
    vkmap[L"home"] = VK_HOME;
    vkmap[L"end"] = VK_END;
    vkmap[L"pgup"] = VK_PRIOR;
    vkmap[L"pgdn"] = VK_NEXT;
    vkmap[L"f1"] = VK_F1;
    vkmap[L"f2"] = VK_F2;
    vkmap[L"f3"] = VK_F3;
    vkmap[L"f4"] = VK_F4;
    vkmap[L"f5"] = VK_F5;
    vkmap[L"f6"] = VK_F6;
    vkmap[L"f7"] = VK_F7;
    vkmap[L"f8"] = VK_F8;
    vkmap[L"f9"] = VK_F9;
    vkmap[L"f10"] = VK_F10;
    vkmap[L"f11"] = VK_F11;
    vkmap[L"f12"] = VK_F12;

    // Numpad keys
    vkmap[L"num0"] = VK_NUMPAD0;
    vkmap[L"num1"] = VK_NUMPAD1;
    vkmap[L"num2"] = VK_NUMPAD2;
    vkmap[L"num3"] = VK_NUMPAD3;
    vkmap[L"num4"] = VK_NUMPAD4;
    vkmap[L"num5"] = VK_NUMPAD5;
    vkmap[L"num6"] = VK_NUMPAD6;
    vkmap[L"num7"] = VK_NUMPAD7;
    vkmap[L"num8"] = VK_NUMPAD8;
    vkmap[L"num9"] = VK_NUMPAD9;
    vkmap[L"numlock"] = VK_NUMLOCK;
    vkmap[L"num."] = VK_DECIMAL;
    vkmap[L"num*"] = VK_MULTIPLY;
    vkmap[L"num+"] = VK_ADD;
    vkmap[L"num-"] = VK_SUBTRACT;
    vkmap[L"num/"] = VK_DIVIDE;

    opPath = RuntimeEnvironment::getBasePath();
}
