# DX 输入通道真机验证结果

- 时间：2026-09-16 12:15:37
- 目标 hwnd：656988
- 管理员权限：**是**
- 发送点击/按键：否(--no-click)
- OP 版本：0.4.8.3
- 窗口标题：BlueStacks 5
- 窗口类名：Qt672QWindowIcon
- 进程 PID：14988
- 客户区尺寸：440x741
- 绑定前 GetDxAttr：7

## 1. 绑定矩阵（mouse × keypad）

| mouse | keypad | BindWindow | GetDxAttr | MoveTo | LeftClick | KeyPress(F1) |
|---|---|---|---|---|---|---|
| dx | windows | 0 | - | - | - | - |
| dx | dx | 0 | - | - | - | - |
| dx.dinput | windows | 0 | - | - | - | - |
| dx.dinput | dx | 0 | - | - | - | - |
| dx.raw | windows | 0 | - | - | - | - |
| dx.raw | dx | 0 | - | - | - | - |
| dx.win | windows | 0 | - | - | - | - |
| dx.win | dx | 0 | - | - | - | - |
| dx.dinput+raw | windows | 0 | - | - | - | - |
| dx.dinput+raw | dx | 0 | - | - | - | - |

绑定成功组合数：**0 / 10**

> 全部失败。请确认：① 管理员权限；② 目标为 64 位进程；
> ③ 目标进程未被反作弊/保护拒绝注入；④ Hook DLL 与 op_x64.dll 版本一致。
> 详细原因见 op SetLog 输出（`BindWindowEx failed. display=.. ret=.. mouse=.. ret=..`）。

---
说明：通道开关验证用于判断目标程序实际依赖哪条 dx 通道。
关闭某通道后若目标立即失去响应，说明该通道是主驱动通路。