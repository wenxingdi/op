# 面向 AI 助手的工作指南（与 CLAUDE.md 等效）

本文件与 `CLAUDE.md` 内容一致，供 Cursor / Codex / 其他读取 AGENTS.md 的工具使用。
构建、测试、发布件同步、约定、踩坑速查全部以 `CLAUDE.md` 为准，请直接阅读仓库根目录的 `CLAUDE.md`。

要点三条：

1. **日常构建**：`python scripts/build/_wb_build.py`（nmake 增量）。不要跑上游的 `build.py` 构建流程（仅全新机器 bootstrap 依赖时用）。
2. **测试**：`powershell -NoProfile -ExecutionPolicy Bypass -File scripts/run_tests.ps1 -Filter "-*WindowServiceTest.RunAppLaunchesShortcut" -TimeoutSec 2400`
   —— **TimeoutSec 必须给足**（全量 10~12 分钟，默认 1800 已够；不要用旧文档里的裸调用，曾因硬编码 180s 被腰斩）。
   —— **跑前必须先把 TEMP 指到工作区**（`mkdir -p workbench/tmp; $env:TEMP="$pwd\workbench\tmp"; $env:TMP=$env:TEMP`）：
   本机沙箱拦截原生进程写系统 `%TEMP%`，不指的话所有落盘造图的用例会 pseudo-fail 98 个（全是假象）。
   当前基线（2026-11-26）：注册 690（4 DISABLED），回归实跑 685（排除 RunAppLaunchesShortcut；
   本日新增 8：OCR memo 6 / mask 端到端 1 / 内存句柄缓存 1），除 1 个恒定 FAILED
   （`HookCaptureTest.BindLoadsMissingGraphicsAndInputModulesOnDemand`，
   dx 模块按需加载环境项）全 PASS；`GetCmdStrReturnsPartialOutputOnTimeout` 偶发红，复核即可。
   测试期间禁止构建（LNK1104）。
3. **构建后必同步发布件**：`D:\AutoPro\OPTool\sync_op_dll.py`（bin/x64 + Python 绑定包 + OPTool 三处 Dll，sha1 校验）。

完整踩坑速查（WGC 进程级熔断、dx 环境判据、TEMP 指向、改头文件不重编译等）以 `CLAUDE.md` 为准；
当前状态总览见 `docs/2026-11/最终收尾_清理与验证_20261124.md`。
