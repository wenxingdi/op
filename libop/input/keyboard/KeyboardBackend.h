#pragma once
#include "../../base/Types.h"

namespace op::input {

class KeyboardBackend {
  public:
    KeyboardBackend();

    virtual ~KeyboardBackend();

    virtual long Bind(HWND hwnd, long mode) = 0;

    virtual long UnBind();

    virtual long GetKeyState(long vk_code) = 0;

    virtual long KeyDown(long vk_code) = 0;

    // virtual long GetKeyState(long vk_code);

    virtual long KeyUp(long vk_code) = 0;

    virtual long WaitKey(long vk_code, unsigned long time_out) = 0;

    virtual long KeyPress(long vk_code) = 0;

    virtual long InputChar(wchar_t ch) = 0;

  protected:
    // ---- 时间轴层录制钩子（批 E 2026-11-22）----
    // 放在基类而不是各后端各写一份：WinKeyboard / DxKeyboard 的 flags 口径相同
    // （KEYEVENTF_KEYUP / KEYEVENTF_UNICODE），重复实现只会漂移。
    // KeyPress 刻意**不调**：它由 KeyDown + KeyUp 组成，再钩一次就录重了。
    void trace_key(long vk, long flags) const;
    void trace_char(wchar_t ch) const;

    HWND _hwnd;
    int _mode;
};

} // namespace op::input
