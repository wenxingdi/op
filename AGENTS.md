# 面向 AI 助手的工作指南（与 CLAUDE.md 等效）

本文件与 `CLAUDE.md` 内容一致，供 Cursor / Codex / 其他读取 AGENTS.md 的工具使用。
构建、测试、发布件同步、约定、踩坑速查全部以 `CLAUDE.md` 为准，请直接阅读仓库根目录的 `CLAUDE.md`。

要点三条：

1. **日常构建**：`python scripts/build/_wb_build.py`（nmake 增量）。不要跑上游的 `build.py` 构建流程（仅全新机器 bootstrap 依赖时用）。
2. **测试**：`powershell -NoProfile -ExecutionPolicy Bypass -File scripts/run_tests.ps1 -Filter "-*WindowServiceTest.RunAppLaunchesShortcut" -TimeoutSec 2400`
   —— **TimeoutSec 必须给足**（全量 10~12 分钟，默认 1800 已够；不要用旧文档里的裸调用，曾因硬编码 180s 被腰斩）。
   当前基线：676 ran / 647 PASS / 28 SKIP / 1 FAILED（既有环境项）。测试期间禁止构建（LNK1104）。
3. **构建后必同步发布件**：`D:\AutoPro\OPTool\sync_op_dll.py`（bin/x64 + Python 绑定包 + OPTool 三处 Dll，sha1 校验）。

完整踩坑速查（WGC 进程级熔断、dx 环境判据、TEMP 指向、改头文件不重编译等）以 `CLAUDE.md` 为准；
当前状态总览见 `docs/2026-11/最终收尾_清理与验证_20261124.md`。
