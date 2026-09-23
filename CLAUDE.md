# CLAUDE.md

> 本文件给 AI 编程助手（Claude Code / Cursor / 其他大模型）提供本仓库的**真实**构建与协作指南。
> 上游原版指南（`build.py` + vcpkg bootstrap + VS 生成器 + ctest 流程）**不适用**本仓库的日常迭代，请勿照抄。

## 项目概述

OP 是 Windows 桌面自动化 COM 插件（C++17），功能：截图（GDI/DX/WGC/Hook）、键鼠输入（SendInput/SendMessage/DX 注入）、找图找色、OCR（点阵字库 + **内置 ONNX 引擎**，HTTP 远程后端已编译期移除）、本地 ONNX YOLO 检测。本仓库是上游 0.4.8.3（tag `0.4.8.3` = 6d6b285）之后的私有迭代 fork，上游 remote 保留为 `upstream`。

## 构建（唯一可信路径）

**环境硬依赖**（已安装则跳过）：

| 组件 | 本机路径 | 说明 |
|---|---|---|
| VS2022 Professional | `D:\Program Files\Microsoft Visual Studio\2022\Professional` | MSVC 14.44.35207 |
| Windows SDK | `C:\Program Files (x86)\Windows Kits\10` | 版本 10.0.26100.0 |
| CMake | 任意 | 仅首次配置构建目录用 |
| Python 3 | `D:\Program Files\Python312` | 跑构建/备份脚本 |

**日常增量构建**（改完 cpp 就跑这个）：

```bash
python build/_wb_build.py              # 默认目标 op_x64 + op_test
python build/_wb_build.py op_c_api_x64 # 只构建 C-API DLL
```

脚本内部：绝对路径 nmake + MSVC/SDK 环境变量，`cwd=build/nmake-x64-Release`。输出与日志落在构建目录 `_wb_build_out.txt`。

**全新机器首次构建**（依赖 `build/_deps/` 不存在时）：

```bash
# 1) bootstrap 依赖（BlackBone / OpenCV 5.0.0 / vcpkg+minhook）
python build.py                        # 上游脚本，只用于拉依赖
# 2) 配置 nmake 构建目录（一次性）
cmake -S . -B build/nmake-x64-Release -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release ^
  -DCMAKE_TOOLCHAIN_FILE=D:/AutoPro/op-master/op/build/_deps/vcpkg/scripts/buildsystems/vcpkg.cmake ^
  -DVCPKG_TARGET_TRIPLET=x64-windows-static ^
  -DBLACKBONE_ROOT=D:/AutoPro/op-master/op/build/_deps/BlackBone ^
  -DBLACKBONE_INCLUDE_DIR=D:/AutoPro/op-master/op/build/_deps/BlackBone/src ^
  -DBLACKBONE_LIBRARY=D:/AutoPro/op-master/op/build/_deps/BlackBone/build/nmake-x64/BlackBone/BlackBone.lib ^
  -DOPENCV_ROOT=D:/AutoPro/op-master/op/build/_deps/opencv/install/nmake-x64 ^
  -DOPENCV_LIB_SUFFIX=500
# 3) 之后全部走 _wb_build.py
```

- 编译开关：`/MT` 静态 CRT、`/EHa`。minhook **静态链接**（产物目录里的 minhook.x64.dll 是历史残留，发布件不含）。
- 产物：`build/nmake-x64-Release/libop/` 下 `op_x64.dll`（COM）、`op_c_api_x64.dll`（C API/OPTool 用）、`op_test.exe`、`onnxruntime*.dll`。

## 测试

```bash
# 一键全量（PowerShell）：自动带工作目录
powershell -File scripts/run_tests.ps1

# 手动跑（必须 cd 仓库根 + PATH 带 libop，否则 op_test 按 cwd 找不到 testdata）
cd D:/AutoPro/op-master/op
set PATH=build/nmake-x64-Release/libop;%PATH%    # bash: export PATH=...
build/nmake-x64-Release/tests/op_test.exe --gtest_filter="OcrTest.*"
```

**禁忌**：测试进程运行期间**不要构建**（op_test.exe 占用 lib → LNK1104）。构建与测试串行。

基线：全量 270 ran / 262 PASS / 6 SKIP / 2 FAILED（2 条为键鼠域环境项）。

## 发布件同步（构建后必做）

构建出新 DLL 后，同步到三处（`OPTool/sync_op_dll.py` 自动完成并校验 sha1）：

1. `bin/x64/`（发布目录）
2. `bindings/python/op/bin/x64/`（Python 绑定包内副本）
3. `D:\AutoPro\OPTool\Common\Dll\` + OPTestTool/WordDictTool 的 `bin/Release/*/Dll/`（工具加载的就是这份）

**口诀**：构建后凡 09-19 及更早时间戳的 op DLL 一律覆盖；用户工具"修复没生效"九成是漏同步。

## 仓库约定

- 提交信息中文，`fix:` / `feat:` / `docs:` 前缀；commit 后追加 `docs/CHANGELOG.md`。
- 正式报告放 `docs/<年月>/`；探针脚本、中间产物放 `workbench/`（**workbench/ 不入库**，勿把唯一副本放这里）。
- 新增 .cpp 必须同步 `CMakeLists.txt` + `tests/CMakeLists.txt`（如含测试）。
- 新能力必须带端到端成功用例；断言必须反向验证（故意改错 → 必须 FAIL）。

## 高频判据（踩坑速查）

- **颜色**：对外 RGB hex `RRGGBB`；大漠 FindPic delta 是 BGR（迁移红蓝互换）；内部 `color_t` 是 BGRA。
- **DPI**：Designer 坐标是 96dpi 逻辑值，运行时按 `DeviceDpi/96` 缩放；OpContext 构造 `SetProcessDPIAware()` → 全进程物理像素。
- **截图验收**：`Capture`/`PrintWindow` ret=1 ≠ 有内容；必须像素级校验（非黑占比/唯一色），像素统计判不出"截到的是谁" → 必须落盘目视。
- **路径**：`/d/xxx` 不能喂原生 Windows 程序（python.exe、git clone）→ 用 `D:/...` 绝对路径。
- **shell**：`find`/`sort`/`awk` 在此环境不可用 → Python os.walk；PowerShell 中文输出重定向文件再读。

## 备份与恢复

- 升级/大改前：**打 tag `v<日期>-stable`（两仓库同名）+ 跑 `D:\AutoPro\op-master\backup\make_backup.py`**（自动 bundle 全历史 + 发布件快照 + manifest）；`--verify <目录>` 校验。
- 本仓库历史大文件 95.3MB（BlackBone.lib），GitHub 可直推无需 LFS。
- 首个稳定点：`v2026.09.23-stable` = d02548f；GitHub 私有 fork `wenxingdi/op` + Release `backup-2026-09-23`（bundle 单文件冷备份）。

## 架构（与上游一致的部分）

```
include/libop.h            — 公开 C++ API（class op::Op，OP_API 导出）
libop/libop.cpp            — 主实现：WindowService/BindingSession/截图/输入/找图 编排
libop/com/op.idl           — MIDL COM 接口（IOpAutomation，~300 dispids）
libop/capture/backends/    — GdiCapture / DxgiCapture / WgcCapture / HookCapture / GdiInputHook
libop/input/               — 鼠标/键盘/DX 输入后端
libop/image/               — 找图算法 / 图像搜索服务
libop/ocr/                 — OCR：字库点阵匹配 + 内置 ONNX 引擎（OcrEngine 抽象；HTTP 后端被
                             OP_ENABLE_HTTP_OCR_BACKEND 宏包裹，默认不编译，network/HttpClient.cpp 已移出构建）
libop/yolo/                — 本地 ONNX YOLO 检测（HTTP 引擎同样已移除）
libop/window|binding|ipc|algorithm|runtime|hook/  — 其余服务
tests/                     — GoogleTest（必须 cd 仓库根跑）
```

截图模式速记：`normal`=桌面 DC BitBlt（看 Z 序，被遮挡截到遮挡物属预期）；`gdi/gdi2/dx2`=窗口内容（本仓库已改 PW_RENDERFULLCONTENT，UWP/Chromium 不再全黑，见 docs/2026-09/UWP窗口截图全黑根因_20260919.md）。
