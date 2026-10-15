#pragma once
#ifndef OP_BINDING_BINDING_SESSION_H_
#define OP_BINDING_BINDING_SESSION_H_
#include "../image/Image.h"
#include <memory>
#include <string>
#include <utility>

#include "../capture/ICaptureBackend.h"

#include "../input/keyboard/KeyboardBackend.h"
#include "../input/mouse/WinMouse.h"

namespace op::binding {

// 绑定失败原因码：BindWindowEx 返回 0 时同步写入 Win32 last-error，脚本可直接用
// GetLastError() 读出（此前只进 setlog，调用者无法区分六类失败）。
// 取值放 0x2000+ 区间，避开 Win32 系统错误码段，避免与无关的 last-error 噪声混淆。
enum BindFailReason : unsigned long {
    BIND_FAIL_NONE = 0,
    BIND_FAIL_INVALID_HWND = 0x2001,   // 显示/输入句柄无效
    BIND_FAIL_DISPLAY_MODE = 0x2002,   // display 模式串非法
    BIND_FAIL_MOUSE_MODE = 0x2003,     // mouse 模式串非法（含 dx 后缀解析失败）
    BIND_FAIL_KEYPAD_MODE = 0x2004,    // keypad 模式串非法（含 dx 后缀解析失败）
    BIND_FAIL_UIPI_INTEGRITY = 0x2005, // 目标进程完整性更高，UIPI 会吞掉跨进程输入
    BIND_FAIL_DXGI_OS = 0x2006,        // normal.dxgi 需要 Windows 8+
    BIND_FAIL_WGC_OS = 0x2007,         // normal.wgc 需要 Windows 10 1903+
    BIND_FAIL_CREATE_BACKEND = 0x2008, // 后端实例创建失败
    BIND_FAIL_BACKEND_BIND = 0x2009,   // 后端 Bind 失败（含 dx 钩子活性回环未通过）
};

class BindingSession {
  public:
    BindingSession();
    ~BindingSession();

  public:
    // 扩展绑定接口。显示截图绑定到 display_hwnd，鼠标和键盘输入绑定到 input_hwnd。
    virtual long BindWindowEx(LONG_PTR display_hwnd, LONG_PTR input_hwnd, const wstring &sdisplay, const wstring &smouse,
                              const wstring &skeypad, long mode);
    // 兼容旧接口的单句柄绑定，内部等价于显示和输入都绑定到同一个 hwnd。
    virtual long BindWindow(LONG_PTR hwnd, const wstring &sdisplay, const wstring &smouse, const wstring &skeypad,
                            long mode);
    virtual long UnBindWindow();
    virtual long LockInput(long lock);
    // 绑定降载:每次截图成功后强制延时 rate 毫秒(0-100)。type 0=推荐 1=高效(实现相同,预留)。
    // 会话级配置:解绑不清空,0 表示关闭。
    virtual long DownCpu(long type, long rate);
    // 设置 dx 输入通道开关。attr 为 0 时把 value 当作完整的 DX_ATTR_* 掩码；
    // attr 为 DX_ATTR_* 位（可组合）时，value 非 0 表示打开这些通道，0 表示关闭。
    // 会话级配置：解绑不会清空，下次绑定 dx 输入时自动下发。
    virtual long SetDxAttr(long attr, long value);
    // 返回当前的 dx 输入通道开关掩码。
    virtual long GetDxAttr();
    // 返回当前绑定的显示窗口句柄。使用 BindWindowEx 时，输入句柄可能不同于这个句柄。
    virtual LONG_PTR GetBindWindow();
    virtual long IsBind();

    long GetDisplay();
    // 返回当前实际生效的图色模式名（"gdi2"/"dx.d3d11"/"normal.wgc" …），未绑定返回空串。
    // 与 GetDisplay() 的裸模式值互补：绑定失败/截图异常时用它确认走的是哪条链路。
    // 注意：不可命名为 display_mode_name——会同本文件匿名 namespace 里的自由函数冲突（成员函数隐藏它）。
    const wchar_t *GetDisplayMethodName() const;
    void lock_data();
    void unlock_data();
    long get_height();
    long get_width();
    long RectConvert(long &x1, long &y1, long &x2, long &y2);
    // 0:normal;-1 reserve 1 need cut
    long get_image_type();

    bool check_bind();
    const std::pair<wstring, wstring> &get_display_method() const;
    long set_display_method(const wstring &method);

    bool requestCapture(int x1, int y1, int w, int h, Image &img);

  private:
    HWND _display_hwnd;
    HWND _input_hwnd;
    int _is_bind;
    int _display;
    int _mode;
    int _mouse_mode;
    int _keypad_mode;
    // dx 输入通道开关，默认三通道全开。由 SetDxAttr 维护，不随解绑重置。
    int _dx_attr;
    // _dx_attr 是否来自 bind 后缀（"dx.dinput" 等）：是则下次无后缀绑定时回默认全开，
    // 避免上次的后缀掩码残留污染后续绑定；SetDxAttr 显式设置会清掉该标记。
    bool _dx_attr_from_suffix;
    // 每次截图成功后的强制延时毫秒数(0-100),由 DownCpu 维护,不随解绑重置。
    int _down_cpu_rate = 0;
    std::pair<wstring, wstring> _display_method;
    Image _pic;

    long reset_bind_state(bool restore_default_input);
    std::shared_ptr<op::capture::ICaptureBackend> createDisplay(int mode);
    std::unique_ptr<op::input::WinMouse> createMouse(int mode);
    std::unique_ptr<op::input::KeyboardBackend> createKeypad(int mode);

  public:
    std::shared_ptr<op::capture::ICaptureBackend> _capture;
    std::unique_ptr<op::input::WinMouse> _mouse;
    std::unique_ptr<op::input::KeyboardBackend> _keyboard;
    wstring _curr_path;
};

} // namespace op::binding

#endif // OP_BINDING_BINDING_SESSION_H_
