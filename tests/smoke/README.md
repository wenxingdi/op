# OP 冒烟测试（tests/smoke）

每次构建/替换 `op_c_api` DLL 后的快速健康检查，替代早年丢失的 GuiSmoke。
与 `tests/` 下的 C++ gtest 回归互补：gtest 钉死行为判据，smoke 在真机
上 1 分钟内确认"DLL 没坏、引擎能起、识别路径通"。

## smoke_dll.py — DLL 冒烟（无头）

```bat
python smoke_dll.py --dll D:\AutoPro\op-master\op\build\nmake-x64-Release\libop\op_c_api_x64.dll
```

- 必须用与 DLL 同架构的 Python（x64 DLL 配 64 位 Python）。
- 覆盖：核心导出清单（约 40 个，跨全模块）→ 实例生命周期 → 路径/JSON
  无头接口 → OCR 引擎加载与预热 → 合成图识别两条路径（免字库直识 /
  颜色二值化小偏色）→ **非法偏色段必须写 `__op.log` 报错**（2026-10-17
  加固的钉死项，防 str2color 静默错解析回归）。
- `--skip-ocr` 可跳过引擎加载，只做导出与无头接口检查（约 1 秒）。

## ocr_batch_score.py — 批量识别打分（上机验证）

```bat
python ocr_batch_score.py --dll ...\op_c_api_x64.dll --dir D:\GMPLUG\captures ^
    --color "E01111-202020|33EE33-202020" --sim 0.8 --expected "12345" --csv out.csv
```

对目录下截图逐张跑同一参数，汇总非空率/命中率。用于验证某颜色串在
真实场景的合格率——2026-10-16 游戏批次实测：默认 0x60 仅 5% 合格，
双色枚举 58/58 全对（详见 docs/2026-10/OCR颜色容差场景化结论_20261016.md）。

## 历史

- 2026-10-17 由 2026-10-15/16 会话中的临时验证脚本沉淀而成
  （原脚本在会话工作区，随任务结束即失）。
