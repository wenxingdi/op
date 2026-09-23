# OP - Windows 自动化插件（本仓库为迭代 fork）

[English](README_EN.md)

> ** fork 说明**：本仓库 fork 自 [WallBreaker2/op](https://github.com/WallBreaker2/op)，基线为上游 `0.4.8.3`（2026-07-07）。
> 在上游基础上做了大量功能修复与扩展（详见 [docs/CHANGELOG.md](docs/CHANGELOG.md)），构建流程也已改为本仓库专用工具链（见下）。
> 上游的 [GitHub Wiki](https://github.com/WallBreaker2/op/wiki) 对基础接口仍适用，但 OCR/YOLO 接入方式、构建命令以本文档为准。

OP（Operator & Open）是一个面向 Windows 的自动化插件。它把窗口查找、后台绑定、截图、键鼠输入、找色找图、OCR、OpenCV、YOLO 检测和进程内存读写放在同一套接口里，方便脚本工具和桌面自动化程序直接调用。

核心代码使用 C++17 实现，提供 COM 与 C API，支持 x86/x64。截图后端覆盖普通窗口、GDI、DXGI、WGC、DirectX Hook、OpenGL 和 OpenGL ES；字库、图片模板可以从本地文件加载，也可以从内存加载，适合把资源随程序一起打包。

## OCR / YOLO（与上游的重要差异）

**本仓库已彻底移除 HTTP 远程服务接入，全部改为进程内本地引擎：**

| 能力 | 上游 0.4.8.3 | 本仓库 |
|---|---|---|
| 通用 OCR | 需部署独立 HTTP 服务（op_ocr_engine） | **内置 ONNX 引擎**（PP-OCRv4 模型内嵌 DLL，零外部依赖，免配置直接用） |
| 固定字体 OCR | 点阵字库 | 点阵字库（保留，兼容 OP 二进制字库和大漠文本点阵字库） |
| YOLO 检测 | 独立 HTTP 服务 | **本地 `.onnx` 模型**（`SetYoloEngine("xxx.onnx",...)` 统一入口，自训模型） |

- `SetOcrEngine("onnx"/空, ...)` 走内置引擎；传入 `http(s)://` 或旧远程别名（tesseract/paddle 系）会返回 0 并提示已移除。
- OCR 家族新增：`AutoOcr` / `AutoOcrLine`（单行快模式）/ `AutoOcrEx`（结构化输出带坐标置信度）/ 免字库自动识别；字库结果统一按阅读顺序拼接。
- 截图修复：`gdi/gdi2/dx2` 改用 `PW_RENDERFULLCONTENT` + 全黑回退，UWP（计算器）/Chromium（Electron、CEF）窗口不再截出纯黑（见 [docs/2026-09/UWP窗口截图全黑根因_20260919.md](docs/2026-09/UWP窗口截图全黑根因_20260919.md)）。

## 文档

- **[docs/BUILD_GUIDE.md](docs/BUILD_GUIDE.md) — 构建与环境配置完整指南（零基础版，推荐先读）**
- [docs/api_reference.html](docs/api_reference.html) — 接口速查手册（240 接口，含调用方式速览：Python/C#/C/C++/Go/易语言/火山 示例，参数注解 100%）
- [docs/CHANGELOG.md](docs/CHANGELOG.md) — 迭代记录
- [docs/INDEX.md](docs/INDEX.md) / [docs/<年月>/](docs/) — 专题报告与审计文档
- [CLAUDE.md](CLAUDE.md) / [AGENTS.md](AGENTS.md) — 给 AI 编程助手的真实构建/协作指南
- 接口文档与多语言示例可参考[上游 Wiki](https://github.com/WallBreaker2/op/wiki)（基础接口一致）

## 主要能力

- 窗口枚举、进程查询、窗口状态控制、批量窗口布局
- 普通绑定、后台绑定、显示句柄和输入句柄分离绑定
- GDI、DXGI、WGC、DX Hook、OpenGL、OpenGL ES 等截图方式（UWP/Chromium 已修复）
- 前台/后台鼠标键盘模拟，支持平滑移动、轨迹移动和 DX 输入锁定
- 找色、找图、透明图片模板、OpenCV 模板匹配和特征匹配
- 点阵字库 OCR + 内置 ONNX 通用 OCR（免外部服务）
- 本地 ONNX 模型 YOLO 检测（免外部服务）
- C API、COM 调用方式；Python 经 `bindings/python`（ctypes 直调 C API）
- 进程内存读写、汇编调用和常用算法工具

## 目录概览

```text
op/
├─ libop/            核心 C++ 源码
│  ├─ op/            op::Op 对外接口的分文件实现
│  ├─ c_api/         C API 封装（op_c_api_x64.dll）
│  ├─ com/           COM 组件、IDL 和自动化接口实现（op_x64.dll）
│  ├─ binding/       窗口绑定和后台模式调度
│  ├─ capture/       GDI/DXGI/WGC/Hook 截图后端
│  ├─ hook/          远端注入、显示/输入 hook
│  ├─ input/         鼠标、键盘输入后端
│  ├─ image/         图片加载、找色、找图
│  ├─ ocr/           字库 + 内置 ONNX OCR 引擎（HTTP 后端已编译期移除）
│  ├─ yolo/          本地 ONNX YOLO 检测（HTTP 后端已编译期移除）
│  ├─ opencv/        OpenCV 桥接、模板匹配和图像处理
│  ├─ window/        窗口、进程和 DLL 注入相关能力
│  ├─ memory/        目标进程内存读写
│  ├─ network/       HTTP 客户端（已移出构建，仅 HTTP 后端恢复时才需要）
│  ├─ ipc/           共享内存、互斥量等进程间通信
│  ├─ base/          基础类型与工具函数
│  └─ algorithm/     内部算法（含 A* 寻路）
├─ include/          对外头文件 libop.h
├─ bindings/python/  C API 的 Python ctypes 包装
├─ tests/            GoogleTest 测试套件
├─ bin/x64|x86/      发布件输出目录
├─ build/            构建目录（nmake-x64-Release + _deps 第三方依赖）
├─ scripts/          run_tests.ps1 / gen_api_reference.py 等
├─ docs/             本文档所在：BUILD_GUIDE / CHANGELOG / 专题报告
├─ workbench/        探针与中间产物（不入库）
├─ build.py          上游一键构建（仅全新机器 bootstrap 依赖时用）
├─ build/_wb_build.py  ★ 日常增量构建入口
└─ CMakeLists.txt
```

## 快速开始

从 `bin/x64/`（或 Release 包）取 DLL，按宿主程序位数使用。COM 方式需要注册（管理员）：

```powershell
regsvr32 .\op_x64.dll   # 64 位宿主
```

Python 通过 COM 调用：

```python
from win32com.client import Dispatch
op = Dispatch("op.opsoft")
print("op version:", op.Ver())
```

或 ctypes 直调 C API（免注册）：

```python
import ctypes
op = ctypes.windll.op_c_api_x64
# 详见 bindings/python 与 docs/api_reference.html
```

配套测试工具 **OPTestTool**（.NET 10 WinForms，管理员权限运行）位于独立仓库/目录 `D:\AutoPro\OPTool`，通过 `op_c_api_x64.dll` 调用本插件全部接口。

## 源码编译

环境要求与**逐步操作**（含每步验证命令）见 **[docs/BUILD_GUIDE.md](docs/BUILD_GUIDE.md)**，摘要：

- Windows 10+ / VS2022（MSVC 14.44）/ Windows SDK 10.0.26100 / CMake / Python 3.12
- 全新机器：`python build.py` 引导依赖 → cmake 配置 `build/nmake-x64-Release`（命令见指南）
- 日常增量：`python build/_wb_build.py`
- 测试：`powershell -File scripts/run_tests.ps1`（基线 270 ran / 262 PASS）
- 构建后同步发布件：`D:\AutoPro\OPTool\sync_op_dll.py`

## 许可证

[MIT License](LICENSE)（继承上游）
