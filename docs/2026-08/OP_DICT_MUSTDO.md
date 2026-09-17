# 字库识别率与制库效率 · 必修六项落地指南

> 基于 OP 库源码（`Dictionary.h` / `ImageSearchAlgorithms.cpp` / `ImageSearchService.cpp`）分析整理。
> 适用：想"识别率 + 制作效率"同时拉满、且**不想动 OP 内核**的场景。
> 这 6 条全部零风险（纯调用约定/工具），做完基本够用；只有真遇到"大→太"类超集反杀且去重兜不住，才需走四阶段改匹配内核（根治项 ③④）。

---

## ✅ 字库同步结论（已用 verify_dict_sync.py 核对）

**`DM_Dict.txt` 与 `DM_Dict0.dict` 是同一本游戏字库的两种格式（非两本）。**
- `.txt` 解析 123 字，`.dict`（v1, count=124, check OK）解析 123 字；按字名匹配全部对得上，**122/123 (99.2%) 点阵字节完全一致**。
- 唯一差异：字 `-9` 在 `.txt` 高被强制 11（多 3 行空白），`.dict` 高=8（真实高度）——`.dict` 反而更准。
- 两份文件同源、可任选；`.dict` 因存真实高度、二进制路径无 GBK/locale 依赖，略优。详见 `DICT_SUPERSET_REPORT.md`。

---

## ① 制库 + 识别统一 `SetBinaryPreprocess(2,1,4,1)` —— 提识别率 + 制库干净度

**根因**：默认去噪 `mode=1` 只删"完全孤立的 1 像素"噪点；相连噪点（≥2）照样进点阵、进字库，喂养超集误识。
**落地**：脚本开头调一次，制库前也调同样参数（制库端 `FetchWord` 走 `str2pointbinaryfbk`，已共享此配置）。

```python
o.set_binary_preprocess(2, 1, 4, 1)
#   │  │  │  └─ bridge_gap=1（桥接被割断的笔画）
#   │  │  └──── min_component_area=4（删面积<4的连通噪点）
#   │  └─────── isolated_threshold=1
#   └────────── mode=2（保守去噪，删小连通域，安全不误伤笔画）
```

## ② 同字体 / 同字号 / 同抗锯齿制库 —— 提识别率（最大误差源）

**根因**：匹配是纯位图重叠，无字体归一。`fromDm`/`parse_dm_text_word` 只存点阵本身，字体/字号/AA 不一致 → sim=1 也失败。
**落地**：用游戏同款渲染截取制库，别用系统字体代做；同一字库内保持统一字号。

## ③ 制库前 `ExtractWordRects` 试切 + 核对字名数==框数 —— 提制作效率

**根因**：投影切分（`binshadowx`/`binshadowy`）只认空白间隙、不认字；两字贴一起→合并，内部有竖缝→误切；`FetchWords` 要求 `字名数 == 切出框数`，不符**静默 return 0**（白做）。
**落地**：

```python
# 先试切看框得对不对
rects = o.extract_word_rects(x1, y1, x2, y2, "FF0000", 1.0)
assert len(rects) == len(word_names), f"框数{len(rects)}≠字名数{len(word_names)}"
# 确认无误再正式制库
o.fetch_words_by_rects(x1, y1, x2, y2, "FF0000", 1.0, word_names, rects)
# 单字：
o.fetch_word(rc, "FF0000", "都")
```

## ④ 优先用二进制 `.dict` —— 可靠性

**根因**：`.txt`(大漠文本) 走 `mbstowcs` + 系统 ANSI 码页（中文 Win=GBK），且 `parse_dm_text_word` **强制 lattice 高=11**（多出空白行，bbox 偏松）；`.dict` 存宽字符、无 locale 坑、存真实高度、加载快。
**落地**：两者同源等价，推荐用 `.dict`（更紧的 bbox + 无编码依赖）；若只有 `.txt` 也完全可用：

```python
# 两者都行，推荐 .dict（真实高度、无 GBK/locale 依赖）
o.set_dict(0, r"D:\GMPLUG\OP\OPTool\dict\DM_Dict0.dict")
# 或：o.set_dict(0, r"D:\GMPLUG\OP\OPTool\dict\DM_Dict.txt")
```

## ⑤ 字库超集扫描去重 —— 提识别率（根治超集反杀最省力）

**根因**：匹配 `full_match` 只查"图上点==字库点"，不罚图上多余点；超集字（太⊃大）借噪反杀本尊。
**落地**：用本仓库工具 `scripts/scan_dict_superset.py` 扫字库，出的报告见 `DICT_SUPERSET_REPORT.md`。

```bash
python scripts/scan_dict_superset.py
```

**本次扫描结果（危险档，尺寸接近/形近）**：最该处理的是 **`大⊂犬`**；另有 3 对标点伪超集（`,⊂镇`、`[⊂巨`、`[⊂区`，尺寸悬殊、实际风险低），完整 6 对见 `DICT_SUPERSET_REPORT.md`：
| 本尊(A) | 超集字(B) | 尺寸 | A点 | B点 |
|---|---|---|---|---|
| `-3` | `-8` | 11×11 | 21 | 25 |
| `大` | `犬` | 11×11 | 26 | 28 |
| `3` | `8` | 5×11 | 15 | 19 |

→ 处理：字库里只保留"本尊"或改字避免重叠；若必共存，靠 ① + 根治项 ③ 兜底。

## ⑥ 识别用显式 `color` + `sim=1` —— 提识别率

**根因**：`OcrAuto` 不传颜色、全局自动二值化，背景非均匀时阈值选错、点阵乱；`sim` 越松容差越大。
**落地**：

```python
ret = o.ocr(x1, y1, x2, y2, "FF0000", 1.0)   # 显式颜色 + sim=1（最严）
# 不要用：o.ocr_auto(x1,y1,x2,y2)  ← 自动阈值不稳
```

---

## 一键初始化模板（Python 调用 OP）

```python
import op
o = op.Op()

# 必修① 去噪（制库与识别统一）
o.set_binary_preprocess(2, 1, 4, 1)

# 必修④⑥ 加载游戏字库（推荐 .dict，与 .txt 同源、存真实高度）
o.set_dict(0, r"D:\GMPLUG\OP\OPTool\dict\DM_Dict0.dict")

# 必修⑥ 识别：显式颜色 + sim=1
text = o.ocr(x1, y1, x2, y2, "FF0000", 1.0)
```

## 进阶 / 按需（非必修，遇到具体难题再做）

| 选项 | 档位 | 何时做 |
|---|---|---|
| `bridge_gap` 修复断笔 | B | 二值化割断笔画、字库缺笔 |
| 多候选 / 投票选字 | B | 易混字（0/O、1/l）业务层兜底 |
| **③ 改 `full_match` 惩罚图上多余点** | C（根治） | 真遇超集反杀、①⑤ 兜不住 |
| **④ 选字改最高置信度（去"命中即停"）** | C（根治） | 同尺寸多字低 sim 选错 |
| 多尺度匹配 | 进阶 | 屏幕字符缩放/间距波动漏识 |

> 若要把"默认去噪加强到删相连噪点（mode=1→2）"做成 OP 库默认，属于改 OP 内核，需走四阶段（发现→确认风险→等你同意→改→编译验证），可另提方案。
