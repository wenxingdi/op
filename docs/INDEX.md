# OP 项目文档索引

> 2026-09-17 全项目整理后建立。正式文档按**时间线**归档在 `docs/<年月>/`；
> 优化过程产生的中间产物（日志/探针/截图 dump）隔离在 `workbench/`（不入库）。
> 迁移明细见 `workbench/ORGANIZE_2026-09-17.log`。

## 常驻文档

| 文档 | 说明 |
|---|---|
| [CHANGELOG.md](CHANGELOG.md) | 本仓库自有迭代日志（基线：上游 0.4.8.3） |

## docs/2026-07 —— 环境搭建期

| 文档 | 说明 |
|---|---|
| [download_blackbone_guide.txt](2026-07/download_blackbone_guide.txt) | Blackbone 依赖下载指引 |
| [download_opencv_guide.txt](2026-07/download_opencv_guide.txt) | OpenCV 依赖下载指引 |

## docs/2026-08 —— L0-L3 审计期（第一波深拆）

| 文档 | 说明 |
|---|---|
| [L2_AUDIT_REPORT.md](2026-08/L2_AUDIT_REPORT.md) | L2 契约-实现一致性审计 |
| [L3_AUDIT_REPORT.md](2026-08/L3_AUDIT_REPORT.md) | L3 BSTR/字符串重构审计 |
| [CAPTURE_L2_REPORT.md](2026-08/CAPTURE_L2_REPORT.md) | 截图域 L2 核查 |
| [FINDCOLOR_DEEPDIVE.md](2026-08/FINDCOLOR_DEEPDIVE.md) | 找色系深拆 |
| [FINDPIC_DEEPDIVE.md](2026-08/FINDPIC_DEEPDIVE.md) | 找图系深拆 |
| [OCR_DICTIONARY_DEEPDIVE.md](2026-08/OCR_DICTIONARY_DEEPDIVE.md) | 字库 OCR 深拆 |
| [OCR_FUNCTION_COVERAGE.md](2026-08/OCR_FUNCTION_COVERAGE.md) | OCR 函数覆盖盘点 |
| [BLOCK_LINE_CAPTURE_DEEPDIVE.md](2026-08/BLOCK_LINE_CAPTURE_DEEPDIVE.md) | 色块/找线/截图深拆 |
| [DICT_SUPERSET_REPORT.md](2026-08/DICT_SUPERSET_REPORT.md) | 字典超集危险配对报告 |
| [OP_DICT_MUSTDO.md](2026-08/OP_DICT_MUSTDO.md) | 字典整改必做项 |

## docs/2026-09 —— 模块排查期（第二波·当前）

### 总纲与路线

| 文档 | 说明 |
|---|---|
| [OP_Master_功能分类与优化路线_20260916.md](2026-09/OP_Master_功能分类与优化路线_20260916.md) | 11 功能域总纲 + 既有资产索引 + 三批路线 |
| [模块排查_总纲_2026-09-09.md](2026-09/模块排查_总纲_2026-09-09.md) | 五步闭环 + 17 模块 + 体检清单 A-H |
| [模块排查_base_2026-09-09.md](2026-09/模块排查_base_2026-09-09.md) | base 模块排查实录 |
| [第二轮_全盘核查报告.md](2026-09/第二轮_全盘核查报告.md) | 第二轮全盘核查 |
| [真机验收清单.md](2026-09/真机验收清单.md) | 真机验收项清单 |
| [收官小结_2026-09-09.md](2026-09/收官小结_2026-09-09.md) | 09-09 阶段收官 |

### 资产盘点

| 文档 | 说明 |
|---|---|
| [OP_CAPABILITY_INVENTORY.md](2026-09/OP_CAPABILITY_INVENTORY.md) | 9 服务组能力清单 + 图色 6 大类详解 |
| [PROJECT_REVIEW.md](2026-09/PROJECT_REVIEW.md) | L0-L3 逐提交核查 |
| [five_layer_matrix.md](2026-09/five_layer_matrix.md) | 五层覆盖矩阵（`scripts/five_layer_matrix.py` 自动生成，勿手改） |

### 专项排查（按域）

| 文档 | 说明 |
|---|---|
| [键鼠域_阶段1发现_20260916.md](2026-09/键鼠域_阶段1发现_20260916.md) | 键鼠域拟人化前发现 + 阶段③④落地结果 |
| [DX_INPUT_CHANNEL_REPORT.md](2026-09/DX_INPUT_CHANNEL_REPORT.md) | dx 输入三通道机制报告 |
| [DX通道_根因定位与修复方案_20260916.md](2026-09/DX通道_根因定位与修复方案_20260916.md) | 门槛A/B 根因与修复方案 |
| [DX通道_真机测试_BlueStacks_20260916.md](2026-09/DX通道_真机测试_BlueStacks_20260916.md) | BlueStacks 真机验证（含判据迭代 3 次记录） |
| [DX通道_BlueStacks实测原始输出_20260916.md](2026-09/DX通道_BlueStacks实测原始输出_20260916.md) | 真机实测原始数据 |
| [DX通道_真机验证指令_20260916.md](2026-09/DX通道_真机验证指令_20260916.md) | 真机验证执行指令存档 |
| [OP_DPI坐标语义与检测_20260916.md](2026-09/OP_DPI坐标语义与检测_20260916.md) | DPI 语义机制 + 子进程隔离 A/B 铁证 |
| [OCR_性能核查.md](2026-09/OCR_性能核查.md) | OCR 性能核查 |

## workbench/ —— 中间产物隔离区（.gitignore 不入库）

| 子目录 | 内容 |
|---|---|
| `workbench/logs/` | 构建/回归日志，文件名带 `YYYY-MM-DD_` 日期前缀（2026-08-04 lockcheck/astar → 2026-09-17 各轮回归） |
| `workbench/probes/` | 一次性诊断脚本（d3d12/smoke/ocr_profile 等）+ 其结果 txt + d3d12_target |
| `workbench/dumps/` | 调试截图 bmp/png（smoke/d3d12/字典二值化等） |

约定：后续排查产生的中间产物一律进 `workbench/` 对应类型目录，日志命名带日期前缀；
正式结论沉淀为 md 进 `docs/<当月>/`，并在本索引登记。

## 仓库外留存（未迁入）

| 位置 | 内容 |
|---|---|
| `D:\AutoPro\op-master\` 根 | OP_API_REFERENCE.md / OP_BUILD_NOTES.md / OP_OCR_INTERNAL_PLAN.md / `_backup_L0_20260804` / `_backup_docs_20260827` / opencv-5.0.0.zip |
| WorkBuddy 工作区根 | 09-16 七份文档的原件（已复制入 docs/2026-09/） |
