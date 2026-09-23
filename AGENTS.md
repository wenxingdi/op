# 面向 AI 助手的工作指南（与 CLAUDE.md 等效）

本文件与 `CLAUDE.md` 内容一致，供 Cursor / Codex / 其他读取 AGENTS.md 的工具使用。
构建、测试、发布件同步、约定、踩坑速查全部以 `CLAUDE.md` 为准，请直接阅读仓库根目录的 `CLAUDE.md`。

要点三条：

1. **日常构建**：`python build/_wb_build.py`（nmake 增量）。不要跑上游的 `build.py` 构建流程（仅全新机器 bootstrap 依赖时用）。
2. **测试**：`scripts/run_tests.ps1` 或 cd 仓库根 + PATH 带 `build/nmake-x64-Release/libop` 后跑 `op_test.exe`；测试期间禁止构建（LNK1104）。
3. **构建后必同步发布件**：`D:\AutoPro\OPTool\sync_op_dll.py`（bin/x64 + Python 绑定包 + OPTool 三处 Dll，sha1 校验）。
