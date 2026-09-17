# 字库（Dictionary / OCR）体系深拆 — 含「大→太」噪点误判根因 + 修复落地（①②⑤）

> 阶段：发现（只读）→ 修复落地（用户选 ① 加强去噪 / ② 制库端去噪 / ⑤ 文档化超集陷阱）。
> 结论先行：**「大→太」不是个例 bug，而是字库 OCR 的「超集匹配 + 对称比对 + 选字即停」三重设计缺陷叠加默认去噪过弱**。sim=1 也救不了，因为 sim=1 走严格 `full_match`，而「太」点阵是「大」的超集，噪点恰好补上「太」多出的点时，「太」反而能完美匹配含噪的「大」图。

---

## 1. 字库体系总览（四个角色）

| 角色 | 文件 / 符号 | 职责 |
|---|---|---|
| **点阵存储** | `Dictionary.h` `word1_t`（按 `(w*h+7)/8` 字节逐 bit 存） | 一个字的二值点阵 + 尺寸 + 名称 |
| **字库加载/排序** | `Dictionary`（`read_dict` / `sort_dict`） | 支持 OP 二进制 `.dict`、大漠文本 `.txt`、内存字库；按 `高→宽→bit_cnt` 排序 |
| **制库** | `FetchWord` / `FetchWordFromBinary` / `ExtractWordRects` | 把屏幕上某区域二值图转成 `word1_t` 点阵（写进字库） |
| **识别** | `ImageSearchAlgorithms::_bin_ocr`（严格版 + 模糊版）、`FindStr`/`Ocr` | 把屏幕二值图逐字比对字库，输出文字+坐标 |

字库点阵两种历史格式：`word_t`（第0代，固定 32 列，大漠 11 高）、`word1_t`（第1代，变长，当前主用）。

---

## 2. OCR 端到端流水线

```
op::Op::Ocr / FindStr(x1,y1,x2,y2, color, sim)
   │
   ▼  with_captured_region → 截图到 _src
str2binaryfbk(color, sim)        ← 二值化：按 color 串把彩色图转成 0/1 点阵 _binary
   │  · 颜色容差 = sim_to_point_color_diff(sim)（颜色值层面，不是点阵层面）
ApplyBinaryPreprocess()          ← 去噪（【已修复】默认 mode=1，见 §5）
   │
   ▼  bin_ocr(dict, sim, ps)
       if sim ≈ 1  → _bin_ocr(dict, ps)        严格 full_match
       else        → sim = 0.5 + sim/2 → _bin_ocr(dict, sim, ps) 模糊 part_match
   │
   ▼  逐像素位置 (px,py) 尝试匹配字库每个 word
       · 取 w×h 区域 crc
       · full_match / part_match 比对
       · 命中即 break（第一个命中的字）—— 不是「最高置信度」
   │
   ▼  build_ocr_text_spans → 拼成字符串
FindStr: str.find(目标串) 返回坐标
Ocr:    直接输出识别文本
```

> **制库端同源**：单字 `FetchWord`(ImageSearchService.cpp:467) 与批量 `FetchWords`(550) 同样走 `str2pointbinaryfbk(color, sim)` → `ApplyBinaryPreprocess()`，**制库时与识别端共享同一套 `_binary_*` 去噪配置**（见 §4 更正 + §9 ②）。

---

## 3. 两个匹配核心（逐字讲解）

### full_match（严格，sim≈1 走这条）
```cpp
inline int full_match(const ImageBin &binary, rect_t &rc, const uint8_t *data) {
    int idx = 0;
    for x in [rc.x1, rc.x2):
      for y in [rc.y1, rc.y2):
        int val = GET_BIT(data[idx/8], idx&7);   // 字库该 bit
        if (binary.at(y, x) != val) return 0;     // 图上 bit ≠ 字库 bit → 立即失败
        idx++;
    return 1;
}
```
语义：**图上点集 必须 == 字库点集**（图上不能多、也不能少一个 bit）。

### part_match（模糊，sim<1 走这条，带 error_tolerance）
```cpp
inline int part_match(const ImageBin &binary, rect_t &rc, int max_error, const uint8_t *data) {
    int err_ct = 0, idx = 0;
    for x, y:
        int val = GET_BIT(...);
        if (binary.at(y, x) != val) { ++err_ct; if (err_ct > max_error) return err_ct; }
        idx++;
    return err_ct;
}
```
`error_tolerance = (1 - sim) * w * h`（line 1739）。
- sim=1 → 0（但 sim=1 根本不走这版）
- sim=0.9（实际重映射成 0.95）→ tolerance = 0.05 * w*h，一个 15×15 的字允许 ~11 个错误 bit。

**关键点：`full_match` 和 `part_match` 都对「图上多点（噪点）」和「图上少点（缺笔画）」一视同仁地惩罚。** 它们不区分"这份点阵的主人是谁"。

---

## 4. ★ 「大→太」误判机理（数学本质）

设：
- 屏幕含噪图点集 `S = {大笔画} ∪ {噪点}`
- 字库「大」点集 `D大 = {大笔画}`
- 字库「太」点集 `D太 = {大笔画} ∪ {P}`（P = 「太」比「大」多出的那个点）

匹配是「图上点集 vs 字库点集」的对称比对：

| 比对 | 结果 |
|---|---|
| full_match(S, D大) | 噪点∈S 但∉D大 → **失败** |
| full_match(S, D太) | 若 噪点==P：S 与 D太 逐 bit 一致 → **成功！** |
| full_match(S, D太) | 若 噪点≠P：S≠D太 → 失败 |

**铁证：`sim=1` 走严格 `full_match`，但「太」是「大」的超集；只要噪声点恰好落在「太」多出的那个位置 P，「太」字库就能完美匹配含噪的「大」图，而「大」字库因多出噪点反而失败 → 输出「太」。**

更现实的情况（sim=0.9，模糊版）：
- 噪声常分布在笔画边缘（抗锯齿/压缩伪影/字体渲染），而「太」多出的点 P 也常在「大」下方/边缘，统计上噪声补齐 P 的概率不低；
- 即便噪点≠P，「太」的 err 也只是「噪点 + P处缺笔画」≈2，仍 ≤ tolerance（~11），照样命中；
- 而「大」字的 err = 噪点数（≈1~5）。两者都命中时，代码**取第一个命中即停**，结果取决于字库排序，并不保证本尊「大」优先。

→ 这就是用户实测 "sim=0.9 或 1 都把大识别成太" 的完整解释。

### 根因清单
1. **超集非互斥（核心）**：字库字之间存在「太⊃大」的包含关系，而匹配是对称比对，不要求"图上点集 ⊆ 字库点集（图上不能多余）"。任何能补齐超集多出点的噪声，都会让超集字库完美/低误差胜出。
2. **选字即停非择优**：命中第一个就 `break`，不比较 confidence，顺序由 `sort_dict`（bit_cnt 小优先）决定，本尊不必然在前。
3. **默认去噪过弱（已修复，见 §5/§9）**：修复前默认 mode=0 完全不去噪，紧邻笔画的噪声（连通域≥2）删不掉，直接进点阵。
4. **制库侧其实已去噪（更正）**：经源码核查，`FetchWord`(单字, line 467) 与 `FetchWords`(批量, line 550) 都调用 `str2pointbinaryfbk` → `ApplyBinaryPreprocess()`，**制库时与识别端共享同一套 `_binary_*` 去噪配置**。因此"制库端脏字库"只在默认 `mode=0`（两端都不去噪）或字库来源非本库工具时发生。真正缺口是**默认 mode=0 不去噪**（本次已修复为 mode=1），而非"制库端无去噪"。

---

## 5. 去噪机制（ApplyBinaryPreprocess）与默认参数

`ImageSearchService` 构造：`_binary_isolated_threshold=0`、`_binary_min_component_area=2`、`_binary_bridge_gap=1`。

**【已修复】默认 `_binary_preprocess_mode` 从 `0` 改为 `1`（本次提交，见 §9 ①）**：即默认开启 mode=1，自动删除完全孤立的 1 像素噪点，不误伤任何连通笔画（笔画端点至少含 1 个邻居）。

`ApplyBinaryPreprocess`（由 `str2pointbinaryfbk` 在制库端与识别端统一调用）：
- mode≥1：删「孤立点」（邻居数 ≤ isolated_threshold）。threshold=0 表示**只删完全没有邻居的单点**——噪点只要挨着任何笔画（连通域≥2）就不删。
- mode≥2：删「小连通域」（面积 < min_component_area）。默认 2 表示**只有单点孤立域**才删，同理拦不住贴边噪点。
- mode≥3：桥接断裂（反而可能把噪点连进笔画）。

→ **修复前**：用户没调用 `SetBinaryPreprocess` 时 mode=0 完全不去噪。**修复后**：默认 mode=1 已删孤立噪点；若需更强（删贴边小连通域），仍调 `SetBinaryPreprocess(2, 1, 4, 1)`（mode=2 + 删面积<4 的连通域 + 不桥接）。

---

## 6. 测试覆盖现状（诚实）

`tests/ocr_test.cpp` 覆盖：SetDict/UseDict/GetDict/AddDict/SaveDict/SetMemDict、Ocr/OcrEx/OcrAuto/OcrFromFile、FetchWord、FindStr/FindStrEx、结果解析、多种 color/sim 组合。

**未覆盖（这正是缺口，本次未补）**：
- 超集字（太/大、未/末、已/己）在含噪图下的取舍；
- 任意单字在 sim=1 下的严格匹配正确性；
- part_match 的 error_tolerance 边界；
- 去噪效果专项测试（`ApplyBinaryPreprocess` / `_binary` 为 private，需重构测试访问或加 friend 才能写单元回归）。

> 本次修复（默认 mode=1）通过**编译 + 现有 ocr_test 全量跑通**验证无回归：现有 BMP 用例文字笔画连通、无孤立点，mode=1 不影响其识别结果。

---

## 7. 修复方向评估（状态更新）

| # | 方向 | 风险 | 状态 |
|---|---|---|---|
| A | **加强默认去噪** | 低 | **【已落地】** 默认 `_binary_preprocess_mode` 改为 1（删孤立噪点）；更强抗噪仍用 `SetBinaryPreprocess(2,1,4,1)`。 |
| B | **制库去噪** | 低 | **【经核查已满足】** 制库端 `FetchWord`/`FetchWords` 本就走 `str2pointbinaryfbk` 共享去噪，无需改代码。 |
| C | **匹配改为"子集+惩罚多余点"** | 中 | 未做（根治方向，留待后续）。改 `part_match`/`full_match` 区分「图上多点（噪点，重罚）」与「字库多点（缺笔，轻罚」，让本尊优先。 |
| D | **选字改为最高置信度** | 中 | 未做（根治方向，留待后续）。去掉 `break` 即停，收集命中后选 `confidence` 最大者。 |
| E | **文档化超集陷阱** | 低 | **【已落地】** 见 §9 ⑤ + 全文更正。 |

> 推荐路径：先做 **A+B+E**（低成本、立竿见影、无害）→ 本次已完成；再视需要评估 **C+D**（改匹配内核，需仔细回归）。

---

## 8. 一句话给小帅

"大"会识别成"太"，是因为**字库里「太」的点阵包含了「大」的全部点、还多了下面那一点**；你图上那个噪点，恰好把"太"多出来的那一点补上了，于是"太"字库比"大"字库更"完美"地吻合了你的图。这不是 sim 调高就能解决的——sim=1 只是要求"一模一样"，但"太"借噪声反而做到了一模一样，而"大"因为有噪点做不到。要根治，得从"匹配时优先本尊、惩罚多余点 + 制库/识别两端都去噪"入手（C/D 方向）。

---

## 9. 修复落地（用户选 ① ② ⑤，本次执行）

### ② 制库端去噪 —— 经核查已满足，无需改代码
- 源码实证：单字 `FetchWord`(`ImageSearchService.cpp:467`) 与批量 `FetchWords`(`:550`) 均调用 `str2pointbinaryfbk(color, sim)` → `ApplyBinaryPreprocess()`，与识别端（`Ocr`/`FindStr` 在 `:1059`/`:1080` 同样走 `str2pointbinaryfbk`）**共享同一套 `_binary_*` 去噪配置**。
- 结论：制库端与识别端去噪一致，原本就是通的。早期"制库侧无去噪"的判断是误判，已更正（§4 第4点）。

### ① 加强去噪 —— 代码落地（默认 mode 0 → 1）
- **改动文件**：`libop/image/ImageSearchService.cpp`（构造函数，约 line 181）。
- **改动内容**：`_binary_preprocess_mode = 0` → `_binary_preprocess_mode = 1`（附注释说明）。
- **效果**：默认自动删除完全孤立的 1 像素噪点；连通笔画（含端点）不受影响，安全无副作用。用户无需改任何脚本即获基础抗噪。
- **可回退**：如需完全精确的逐像素模式（关掉默认去噪），调用 `SetBinaryPreprocess(0, 0, 2, 1)` 即可。

### ⑤ 文档化超集陷阱 —— 已落地
- 本文档全文重写，含：超集匹配数学本质、默认 mode 修复说明、去噪三档机制、制库/识别同源更正。
- **字库制作 + 识别最佳实践建议（给小帅）**：
  1. **避免造互为超集的字**：字库里不要同时有「大」和「太」且期望「大」被优先——「太⊃大」必然让含噪「大」图有概率被「太」反杀。确需共存时，配合 C/D 根治方向。
  2. **制库与识别用同一套去噪参数**：两端已共享 `_binary_*`，但要在制库前调 `SetBinaryPreprocess`，保证字库本身干净；识别时再用同样参数。
  3. **更强抗噪**：`SetBinaryPreprocess(2, 1, 4, 1)` 删贴边小连通域（面积<4），比默认 mode=1 更强。
  4. **sim 语义**：`FindStr`/`Ocr` 的 `sim` 与 `FindPic` 不同，**无 `0.5+sim/2` 重映射**，sim=1 即精确逐位比对；但精确比对仍不抗超集噪点（见 §4）。

### 验证
- 编译：`build.py -g nmake -t Release -a x64` 通过（BUILD_EXIT=0）。
- 回归：`tests/ocr_test.cpp` 全量跑通，无回归（mode=1 不影响现有连通笔画用例）。

### 未做（根治方向，留待后续）
- **C 匹配内核惩罚多余点**：改 `full_match`/`part_match`，让"图上多点（噪点）"重罚、"字库多点（缺笔）"轻罚，从根上让本尊优先于超集字。需仔细回归，风险中。
- **D 选字改最高置信度**：去掉 `break` 即停，收集所有命中后选 `confidence` 最大者。当前 `confidence` 已算但未用于选字。风险中。
