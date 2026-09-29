# OP 项目文档索引

> 2026-09-17 全项目整理后建立。正式文档按**时间线**归档在 `docs/<年月>/`；
> 优化过程产生的中间产物（日志/探针/截图 dump）隔离在 `workbench/`（不入库）。
> 迁移明细见 `workbench/ORGANIZE_2026-09-17.log`。

## 常驻文档

| 文档 | 说明 |
|---|---|
| [CHANGELOG.md](CHANGELOG.md) | 本仓库自有迭代日志（基线：上游 0.4.8.3） |
| [api_reference.html](api_reference.html) | **API 参考手册**：223 个接口按 10 服务组分类，含参数表/**取值语义注解**（绑定模式/flag 枚举/sim 口径等 63 项）/搜索（`scripts/gen_api_reference.py` 从 libop.h + PARAM_DOCS 注解层生成，勿手改） |

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
| [真机验收_免字库OCR颜色口径与坐标语义_20260929.md](2026-09/真机验收_免字库OCR颜色口径与坐标语义_20260929.md) | 真机验收：颜色口径=**RGB**（受控目标决定性实验）/ 绑定坐标=**客户区**（平移零位移）/ 免字库 OCR 颜色语义（空串也能识别）/ 字库路径负号不可靠 |
| [验证_OpenCV域C_API导出层_20260929.md](2026-09/验证_OpenCV域C_API导出层_20260929.md) | OpenCV 域 **C API 导出层 36/36 全覆盖**（gtest 只覆盖 3/36 且绕过导出层）；ground-truth 像素级判据；⚠ 抓到 `cv_edge_match_template` 伪命中缺陷（`TM_CCOEFF_NORMED` 平坦区退化） |
| [真机验收_键鼠与charset白名单_20260929.md](2026-09/真机验收_键鼠与charset白名单_20260929.md) | 真机验收：**键鼠通路闭环**（原生 WndProc 直读 lParam，`WM_MOUSEMOVE` 坐标精确一致）/ **charset 白名单闭环** / 靶子方法学三次纠正 / **真机覆盖缺口普查**（272 方法未测域清单） |

## scripts/build/ + scripts/probes/ —— 脚本归档（入库）

> 2026-09-29 起：**构建与探针脚本**自 `build/`、`workbench/` 顶层迁入版本控制
> （此前两处均在 .gitignore 内，脚本属于唯一副本却毫无备份）。载体源码与生成脚本
> 原本就在 `scripts/`。

| 目录 | 内容 |
|---|---|
| `scripts/build/` | 构建链：`_wb_build.py`（x64 日常增量）、`_wb_build_{opencv,minhook,blackbone,directxheaders,op}_x86.py`（32 位全链）、`_sync_release_b2.py` |
| `scripts/probes/` | 探针与回归：`run_optest.py`（唯一正确跑法，cwd=仓库根）、`_t_ab_run.py`（新旧二进制 A/B）、`_t_mods.py`（跨位数模块枚举）、`_t_bind_any.py`、`_t_dx32_carrier.py`、`scan_release_copies.py`、`sync_extra_copies.py` 等 |

### 历史测试记录（`docs/test-records/`）

2026-09-29 起，散落在 `workbench/` 与 `build/` 的**回归/靶子/绑定/A-B 记录文本**
（45 份，896 KB）归档进 `docs/test-records/2026-09/`，随 git 入库，可直接下载复查。
索引见 `docs/test-records/README.md`。日志类中间产物仍留在 `workbench/`（不入库）。

## scripts/ —— 仓库级工具（入库）

| 脚本 | 用途 |
|---|---|
| `push.py` | 推送 master 到 GitHub 并**硬校验**（远端 sha == 本地 sha）。绕开「默认 `credential.helper` 派生 GCM 子进程会静默崩掉 git」→ 内联 auth URL + `-c credential.helper=`，token 由 GCM exe 直取；推送成功后显式 `update-ref origin/master`（**内联 URL 推送不更新本地跟踪引用**，否则 `git bundle create --all` 会带上过期引用）；输出全程脱敏 |

## scripts/backup/ —— 备份与发布工具（入库）

| 脚本 | 用途 |
|---|---|
| `make_backup.py` | 生成一次完整备份（bundle + 运行件 + manifest + 恢复说明）；tag 由 `detect_tag()` 自动探测，不再硬编码 |
| `release_assets.py` | 把备份目录上传为 GitHub Release 附件（幂等，已存在则跳过；中文名自动映射 ASCII） |
| `replace_assets.py` | **替换**已发布 Release 上的指定附件（先 DELETE 再上传，用于补打 bundle 后刷新） |
| `refresh_bundle.py` | 重打某备份目录的 `*-repo.bundle` 至最新 master，并同步 manifest / 恢复说明 |
| `verify_assets.py` | **硬校验线上 Release 附件 == 本地备份文件**（asset `digest`(sha256) + `size` 对本地逐字节 sha256；刚上传后 API 可能返回缓存旧 digest，内置 3 轮重试） |
| `create_repos.py` | 初始创建两仓库的辅助脚本 |

## workbench/ —— 中间产物隔离区（.gitignore 不入库）

| 子目录 | 内容 |
|---|---|
| `workbench/logs/` | 构建/回归日志，文件名带 `YYYY-MM-DD_` 日期前缀（2026-08-04 lockcheck/astar → 2026-09-17 各轮回归） |
| `workbench/probes/` | 探针**产物**：载体 exe（dx_carrier / dx_carrier_x86 / dx_target）、其结果 txt |
| `workbench/dumps/` | 调试截图 bmp/png（smoke/d3d12/字典二值化等） |

约定：后续排查产生的**中间产物**（日志/截图/载体 exe/dump）进 `workbench/` 对应类型目录，
日志命名带日期前缀；**脚本本身写进 `scripts/probes/`、`scripts/build/`（入库）**；
正式结论沉淀为 md 进 `docs/<当月>/`，并在本索引登记。

## 仓库外留存（未迁入）

| 位置 | 内容 |
|---|---|
| `D:\AutoPro\op-master\` 根 | OP_API_REFERENCE.md / OP_BUILD_NOTES.md / OP_OCR_INTERNAL_PLAN.md / `_backup_L0_20260804` / `_backup_docs_20260827` / opencv-5.0.0.zip |
| WorkBuddy 工作区根 | 09-16 七份文档的原件（已复制入 docs/2026-09/） |
