# capture / hook L2 拆解报告

> 关联：`scripts/DX_INPUT_CHANNEL_REPORT.md`（dx 输入三通道 + 点号后缀法）
> 范围：OP 库金字塔评审 L2 —— capture 后端 + hook 输入/截帧子系统逐函数解剖（发现阶段只读，改代码按铁律四阶段经用户授权）

## 一、解剖范围（全部只读）

| 类别 | 文件 |
| --- | --- |
| 捕获后端 | `libop/capture/backends/GdiCapture.cpp` / `DxgiCapture.cpp` / `WgcCapture.cpp` / `HookCapture.cpp` |
| 抽象基类 | `libop/capture/ICaptureBackend.h` / `ICaptureBackend.cpp`（共享内存 + 互斥量） |
| 注入侧截帧 | `libop/hook/D3D9Capture.cpp` / `D3D10Capture.cpp` / `D3D11Capture.cpp` / `D3D12Capture.cpp` / `OpenGLCapture.cpp` |
| 跨进程同步 | `libop/hook/DisplayHook.cpp` ↔ `HookCapture.cpp` 的共享资源名 / 互斥量名 |
| 坐标转换 | `libop/binding/BindingSession.cpp`（`RectConvert`）/ `libop/op/OpImage.cpp` |

## 二、核心结论

**capture/hook 子系统没有必修项**（无崩溃 / UB / 泄漏 / 数据竞争）。

- 现代捕获后端已在**内部**正确实现客户区对齐，不需要外部再偏移。
- 基类 `ICaptureBackend._client_x/_client_y` 是**死字段**（全 capture 目录无任何赋值点，恒为 0）。
- 三处被注释的客户区偏移代码（`RectConvert:533-536` / `OpImage.cpp:100-104,131-135`）注释掉**恰恰正确**——恢复会造成 double-offset 错位。

## 三、风险评级清单

| # | 项 | 位置 | 等级 | 触发条件 / 影响 |
| --- | --- | --- | --- | --- |
| ① | **GdiCapture `dx_/dy_` 绑定后不随 resize 刷新** | `GdiCapture.cpp` BindEx + requestCapture | **建议修** | 仅 `normal`/`gdi` 后端 + 带边框窗口 + 绑定后 resize/去边框；坐标对齐过时，FindPic 偏 (0,边框宽) 或漏抓。非崩溃。 |
| ② | 基类 `_client_x/_client_y` 死字段 + 三处被注释偏移代码 | `ICaptureBackend` / `RectConvert` / `OpImage` | **可选·清理** | 运行零影响，纯技术债 |
| ③ | `HookCapture::waitForBindReady` 读 FrameInfo 未加锁、BindEx 空 `if{}` 死代码、`D3D11/12Capture` `static int cnt` 未用 | hook/capture | **可选·清理** | 低危 / 无关行为，仅代码整洁 |
| ④ | 所有 D3D/GL 钩子在目标 Present 内同步截帧，每帧加延迟 | `D3D*Capture` / `OpenGLCapture` | **建议·性能** | present-hook 固有特性，需 ring buffer/异步复制才根除，属大重构 |

## 四、① GdiCapture 修复详情（已实现 + 验证）

**根因**
`dx_/dy_`（客户区相对窗口的左边框宽 / 上边框高）在 `BindEx` 计算一次存为成员；`requestCapture` 的 RDT_NORMAL 分支用 `src_x = x1 + rc.left + dx_` 把客户区坐标转屏幕坐标去 BitBlt。其中 `rc.left` 每帧重算（窗口移动 OK），但 `dx_/dy_` 是绑定时的缓存（窗口变边框就过时）→ 带边框窗口 resize 后 FindPic 坐标整体偏移。

**修复**（仅 `GdiCapture.cpp` 一处，约 8 行）
在 `requestCapture` 入口（资源检查后、`img.create` 前）每帧重算：
```cpp
{
    RECT wrc;
    ::GetWindowRect(_hwnd, &wrc);
    POINT cpt = { 0 };
    ::ClientToScreen(_hwnd, &cpt);
    dx_ = cpt.x - wrc.left;
    dy_ = cpt.y - wrc.top;
}
```
覆盖 RDT_NORMAL 与 gdi 两个使用分支；不影响 RDT_GDI_DX2（它不用 `dx_/dy_`）。

**风险**：低（每帧重算，未 resize 时结果 = 原绑定值，行为不变）。

**测试**
`ImageColorTest.GdiCaptureRefreshesClientOffsetAfterResize`：带边框窗口 `bind normal` → 基线取色 → `SetWindowPos` resize → 再次取色，验证不崩溃且返回有效。`normal` 捕获不可用时 `GTEST_SKIP` 兜底。

**验证结果**
- 编译 `BUILD_EXIT=0`，0 错误
- `ImageColorTest.*` 72/72 PASSED（新增测试真实跑通，非 SKIP；headless 下 normal 模式可用）
- 无功能回归

## 五、未做项（用户未授权）

- ② 死字段 / 死代码清理（可选）
- ③ hook/capture 小瑕疵清理（可选）
- ④ 截帧异步化（性能大重构，不建议现在做）

## 六、与 dx 输入改造的关系

dx 三通道开关（`DX_INPUT_CHANNEL_REPORT.md`）作用于**输入**链路；本报告的 capture 解剖作用于**截图**链路，二者正交。capture/hook 子系统经解剖确认无必修项，仅 GdiCapture 一处边界 bug（①）已修。
