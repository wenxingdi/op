// #include "stdafx.h"
#include "AutomationModes.h"
#include "Types.h"
#include <time.h>

// HINSTANCE gInstance;

// int gShowError = 1;

// wstring m_opPath;

// wstring g_op_name;

// 按键按住时长（KeyPress 的 down-up 间隔，再叠 DelayJitter ±40%）。
// 真人按键时长典型 60~120ms 且右偏；旧默认 30/10ms 偏机器。可用 SetKeypadDelay 覆盖。
long KEYPAD_NORMAL_DELAY = 60;
long KEYPAD_NORMAL2_DELAY = 60;
long KEYPAD_WINDOWS_DELAY = 40;
long KEYPAD_DX_DELAY = 50;

long MOUSE_NORMAL_DELAY = 30;
long MOUSE_WINDOWS_DELAY = 10;
long MOUSE_DX_DELAY = 40;
