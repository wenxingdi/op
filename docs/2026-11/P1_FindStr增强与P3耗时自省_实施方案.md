# P1 FindStr 增强 + P3 OCR 耗时自省 实施方案

> 目标：`OP全面增强建议.md` §9.2 P1（FindStr 增强）+ P3（OCR 分项耗时自省）
> 状态：**自主实施中**（用户授权「自己决定，确定到最佳，需要编译就编译」）
> 前置：P0 逐字坐标 JSON 已完成（commit `2abeb16`），P2 动态 rec 长宽比已完成（`b13da15`）

---

## 一、P1 现状与问题

### 1.1 现有实现

```
FindStr(strs="a|b|c", color, sim)       → 返回命中序号（-1=未找到），坐标 retx/rety
FindStrEx(strs="a|b|c", color, sim)     → 返回 "序号,x,y|序号,x,y|..."
```

调用链：

```
OpFindStr / OpFindStrEx                    (libop/c_api/op_c_api.cpp:1684/1694)
  → op::Op::FindStr / FindStrEx            (libop/op/OpOcr.cpp:501/519)
    → ImageSearchService::FindStr          (libop/image/ImageSearchService.cpp:1614/1633)
      → engine_ocr_filtered 全图 det+rec    ← 每次调用一次完整推理
      → ImageSearchAlgorithms::FindStr     (libop/image/ImageSearchAlgorithms.cpp:933)
        → build_ocr_text_spans：按阅读序把各行 text 拼成全文
        → str.find(目标) → 定位到所属"行" → 返回该行的 left_top
```

### 1.2 三个真实痛点

| # | 痛点 | 后果 |
|---|---|---|
| P1-a | **每个目标串都是"子串包含"语义**：`str.find(vstr[i])` | 找 `909` 时 `1909`、`9090` 也命中；找 `5/5` 时 `15/5` 也命中。游戏读数字场景误报高 |
| P1-b | **只能返回整行坐标** | 在 `击倒机关兽(909m)0/5` 里找 `909`，返回整行 left_top，调用方拿不到 `909` 自己的框 |
| P1-c | **无正则、无形近容错、无批量控制** | N 个目标要 N 次全图 OCR（N 次 det+rec）；形近字（`割喉台`→`割喉合`）直接失败 |

> **关键澄清（推翻交接文档的一处描述）**：
> 交接文档 §五 P1 写「N 个目标要跑 N 次全图 OCR」——**这对 `FindStrEx` 不成立**。
> 现有 `FindStrEx` 已支持 `a|b|c` 一次传入、一次 OCR、引擎内循环匹配。
> 因此 `OpFindStrBatch(json_targets)` 的**真实价值不是"少跑 N 次"，而是**：
> 1. 支持**按目标独立配置**（每个目标自己的 match/exact/regex/fuzzy/阈值）；
> 2. 返回**每目标命中坐标 + 字符级精确框**（P0 逐字数据的应用）；
> 3. 返回**全部命中**而非只给你"哪几个串命中"。

### 1.3 既有契约（必须保持）

- `OpFindStr` 返回命中序号，`-1` = 未找到；`0` 是合法命中序号。**不能**按布尔归零。
- `OpFindStrEx` 输出 `序号,x,y|`，上限 `_max_return_obj_ct`（默认 1800）。
- 坐标客户区化：`it.left_top.x + _x1 + _dx`（区域原点偏移 + 窗口客户区偏移）。
- `sim` 走 `clamp_sim_exact`（字库路径默认 1.0，免字库路径调用方通常传 0.3~0.7）。
  注意 `engine_ocr_filtered` 按 `confidence < sim` 过滤 —— 对免字库，`sim` 兼作**置信度门槛**。

---

## 二、P1 设计

### 2.1 新增接口（三层 + COM）

| 层 | 符号 | 形态 |
|---|---|---|
| C API | `OpFindStrBatch(handle, x1,y1,x2,y2, json_targets, color, sim)` | 返回 JSON |
| C API | `OpFindStrEx2(handle, x1,y1,x2,y2, pattern, color, sim, flags)` | 返回 JSON |
| C++ | `op::Op::FindStrBatch(...)` / `op::Op::FindStrEx2(...)` | `std::wstring&` out |
| COM | `FindStrBatch` = **DISPID 442** / `FindStrEx2` = **443** | `BSTR*` / `LONG*` |

**为什么两个而不是一个**：
- `FindStrBatch`：**多目标结构化**。输入是 JSON 数组，每项可独立配置。适合"一次找 10 个东西"。
- `FindStrEx2`：**单目标增强**。`pattern + flags` 标量入参，最适合易语言（易语言拼 JSON 徒手很痛，标量参数对其友好）。轻量、无 JSON 解析、零失败面。
- **两者共享同一套匹配内核**（`MatchOne`），行为必然一致。

### 2.2 `json_targets` 格式

```json
["简单串", {"text":"909","match":"exact","fuzzy":0,"max_hits":0}]
```

- 数组元素为 **string** 时：等价于 `{"text": <该串>}`，全部取默认配置。
- 数组元素为 **object** 时支持以下可选字段：

| 字段 | 类型 | 默认 | 说明 |
|---|---|---|---|
| `text` | string | —（必填） | 目标文本；`mode=regex` 时是正则表达式 |
| `match` | string | `"contains"` | `contains` / `exact` / `prefix` / `suffix` / `regex` |
| `fuzzy` | int | `0` | 允许编辑距离（≤ 该值即算命中），实际上限 3 |
| `max_hits` | int | `0`（不限） | 本目标最多返回多少条命中 |
| `ci` | bool | `false` | 忽略 ASCII 大小写 |
| `confirmed_only` | bool | `false` | 只匹配**未贴边裁切**的行（`is_partial=false`） |
| `id` | string 或 int | `""` | 原样回显，便于调用方按自定 id 索引 |

> 兼容性设计：**裸字符串数组合法**，让「只想要 N 目标」的调用方零学习成本。
> 只有需要精细控制时才写 object。

### 2.3 `flags` 位掩码（`OpFindStrEx2` / COM `FindStrEx2`）

| 位 | 值 | 含义 |
|---|---|---|
| 0 | `1` | `match=regex` |
| 1 | `2` | `match=exact`（默认 `contains`） |
| 2 | `4` | `ci`（忽略 ASCII 大小写） |
| 6 | `64` | 模糊匹配 ≤1 |
| 7 | `128` | 模糊匹配 ≤2 |
| 8 | `256` | `confirmed_only`（只匹配未贴边的行） |

> 位 3/4/5（8/16/32）**刻意留空**，是为将来加 `prefix`/`suffix`/`normalize` 留出位置，
> 不至于让已上线的位段语义被打乱。模糊与 exact/regex 互斥时**报错返回**（不做静默降级）。

### 2.4 输出 JSON

```json
{"total":2,"lines":29,
 "results":[
   {"id":0,"text":"909","match":"contains","found":true,"count":1,
    "hits":[{"line":11,"x":57,"y":112,"x2":77,"y2":126,
             "cx1":57,"cx2":77,"px1":57,"px2":77,
             "conf":0.998,"line_text":"击倒机关兽(909m)0/5",
             "partial":false,"ed":0,"exact":true}]},
   {"id":1,"text":"不存在的串","match":"contains","found":false,"count":0,"hits":[]}
 ]}
```

字段口径：

| 字段 | 含义 |
|---|---|
| `x,y` | **命中片段的左边界**坐标（客户区化），`y` 取所属行顶边；与传统 `FindStr` 的 `x,y` 语义一致 |
| `x2,y2` | **行**的右边界/底边（客户区化）。`y2` 无逐字纵向信息可用，只能给行底 |
| `cx1,cx2` | **命中片段的字符级水平范围**（P0 逐字坐标推导），客户区化 |
| `px1,px2` | 同上，但**永远有值**：逐字不可用（旋转降级/字库路径）时退化为行范围 |
| `ed` | 实际编辑距离（`match=fuzzy` 时有效，其余为 `0`） |
| `exact` | 该命中是否**未经模糊/容错**即命中（= `ed==0`）|
| `partial` | 该行 `is_partial`（底部贴边，可能没读全） |
| `line_text` | 命中所在行的完整识别文本，便于调用方自校验 |

> **`px1/px2` 存在的理由**：`cx1/cx2` 在字库路径下没有意义（`chars` 为空数组）。
> 与其让调用方写两条分支，不如直接给一个"退化后仍然合理"的 `px1/px2`。

### 2.5 匹配内核语义（逐条钉死）

给定命中窗口 `W = [idx, idx+len)`，要求**完全落在同一行内**（跨行不命中）：

| 模式 | 命中条件 |
|---|---|
| `contains` | 行文本中长度 = `len` 的任意窗口字符逐一相等（ASCII 可按 `ci` 折叠） |
| `exact` | 同上，且 `idx==0 && len==行长度` |
| `prefix` | 同上，且 `idx==0` |
| `suffix` | 同上，且 `idx+len==行长度` |
| `regex` | `std::wregex` 在行内 `regex_search`，命中范围 = 该 match；**忽略 `ci`**（由 `(?i)` 控制，MSVC `std::regex` 不支持内联标志，故 `ci` 对 regex 无效并写入文档） |
| `fuzzy` | 在行内枚举与 `len±fuzzy` 等长的候选窗口，取 `ed <= fuzzy` 的最小 `ed` 者 |

**为什么要"同一行内"**：现有 `FindStr` 把全文拼成一个长串，`OK` 可能横跨两个不同的 UI 元素（`O` 在左框、`K` 在右框）。
按行约束后语义明确；若将来需要跨行，加 `cross_line` 开关（本期不做）。

**`fuzzy` 的候选长度**：`len-fuzzy .. len+fuzzy`（下限 1）。同长度优先（`ed` 更小优先，`ed` 相等取更左）。

**`regex` 的安全边界**：
- 模式长度 > 512 直接返回空（防病态模式）。
- `std::wregex` 构造与 `regex_search` **全部 try/catch**，异常 → 该目标 `found=false` 并写 `error` 字段。
- **不做**正则引擎的步数限制（`std::regex` 无此能力）——文档写明"不可填不可信的模式"，
  与仓库既有的 `json` 解析深度上限 200 是同一类防护思路。

### 2.6 坐标来源：为什么这次能做到字符级

`engine_ocr_filtered_ext()` 已经拿到 `vocr_line_ext_t`（每行 `chars` 的 `x1/x2`，**原图绝对坐标**）。
流程：

```
1. det+rec 一次 → res + ext（逐字）
2. 按行的阅读序建行列表（保持与 FindStrEx 一致的行聚类口径）
3. 每行行内做匹配 → 命中窗口 [idx, idx+len)
4. cx1 = chars[idx].x1   （逐字起点）
   cx2 = chars[idx+len-1].x2（逐字终点）
   若 chars 不可用 / 数量与行文本长度不一致 → cx1/cx2 退化为行框
5. 全部坐标 + off_x/off_y（区域原点 + 客户区偏移）
```

**关键陷阱（踩过，写死）**：`chars[].x1/x2` 已是**原图绝对坐标**，只能加 `off_x`，
**绝不能再加 det 框原点**（P0 修过的坐标成倍越界 bug，见交接文档 §2.1 第 2 条）。

**逐字与文本长度不一致的处置**：`chars.size() != line.text.size()` 时视为逐字不可用（退化）。
P0 已过滤空串字典条目，正常情况下两者相等；这里做防御式判断，宁可退化也不给错坐标。

### 2.7 与既有 `FindStr` / `FindStrEx` 的关系

**不改一行既有代码的行为。** 新增三个函数共享一个内部内核，旧的两条路径保持原样：

| 旧接口 | 新接口 | 关系 |
|---|---|---|
| `FindStr` | `FindStrEx2` | 旧 = "首个目标的首个命中 + 整行坐标"；新 = "全部命中 + 字符级框 + 模式/容错" |
| `FindStrEx` | `FindStrBatch` | 旧 = "序号,x,y"（只有行左上角）；新 = "每目标结构化命中列表" |

> 语义差异**只在新增接口上**存在，旧接口零风险。回归测试 `FindStrTest` 既有用例必须全绿。

### 2.8 免字库 vs 字库路径

| 路径 | 逐字数据 | 新增接口行为 |
|---|---|---|
| 免字库（无活动字库） | **有**（P0 产出） | `cx1/cx2` 为真实字符级 |
| 字库匹配（`ActiveDict` 非空） | **无**（`chars` 空数组） | `cx1/cx2` 退化为行范围，`partial_chars` 语义保持 |

**颜色路径**：`FindStrBatch`/`FindStrEx2` 走 `color` 二值化以保持与旧 `FindStr` 一致的口径
（旧的 `FindStr` 在字库路径下用 `str2pointbinaryfbk(color, sim)`，免字库路径直接吃原图 `_src`）。
**新增接口沿用同一分支**，不自作聪明加"智能选路"——一旦加，同图同色会给出与旧接口不同的结果，
破坏"新旧结果可互相校验"这个调试期最有用的性质。

> 这是与 `OcrAutoColorEx2` 的**有意不一致**：那边是为了救"传错色恒空"的可用性，
> 这边是查找接口，宁可行为保守可预期。

---

## 三、P3 设计：OCR 分项耗时自省

### 3.1 现状

所有耗时分析都靠外部脚本（`workbench/` 下十来个），且本机计时方差极大
（交接文档 §1.3 坑 7：同一图同一配置冷启动 1056ms / 稳定 370ms）。缺一份**引擎内部**的分项数据。

### 3.2 接口

| 层 | 符号 |
|---|---|
| C API | `OpOcrLastProfile(handle)` → JSON |
| C++ | `op::Op::OcrLastProfile(std::wstring &ret)` |
| COM | `OcrLastProfile` = **DISPID 444**，`[out, retval] BSTR*` |

输出：

```json
{"det_ms":41.2,"cls_ms":2.1,"rec_ms":88.4,"pre_ms":6.3,"post_ms":1.7,
 "total_ms":139.7,"boxes":14,"lines":14,"chars":142,
 "upscale":true,"oscale":2,"rec_width":320,"engine":"v6","cls":true,
 "det_calls":1,"rec_calls":1,"cls_calls":1,"runs_failed":0}
```

| 字段 | 含义 |
|---|---|
| `pre_ms` | 入口预处理（含 2x 放大 + det 输入 resize/归一化） |
| `det_ms` / `cls_ms` / `rec_ms` | 三次 `session.Run` 的纯推理耗时累加 |
| `post_ms` | DB 后处理 + 逐字映射 + 组装 |
| `total_ms` | 从 `ocr_ext` 入口到返回的总耗时 |
| `boxes` | det 出来的框数；`lines` | 非空文本行数 |
| `upscale` / `oscale` | 是否走了 2x 放大 / 放大倍数 |
| `rec_width` | 本次 rec 实际用的 RW（P2 动态宽度的可观测值） |
| `runs_failed` | `ort_run` 失败次数（异常被吞的情况） |

### 3.3 实现要点

- `ort_run` 加可选出参累计耗时（用 `std::chrono::steady_clock`），**不改签名语义**：
  默认参数 `nullptr`，旧调用点零改动。
- 计时写进 `OnnxOcrEngine::Impl` 的 `m_prof` 结构，在 `ocr_common` / `ocr_line_common` 入口清零。
- **线程安全**：`m_prof` 每实例一份；`HttpOcrService` 取引擎快照后在锁外调用，
  同一实例可能被并发使用 → `m_prof` 用 `std::mutex` 保护，或接受"最后写入者胜"。
  **决策：接受最后写入者胜**（profile 是诊断信息，不是精确账单；加锁会污染计时本身）。
  文档写明"并发调用时数值可能对应任意一次调用"。
- **关闭开关**：环境变量 `OP_OCR_PROFILE=0` 时不累计（省两次 `steady_clock::now()`）。
  默认开启（开销 <1µs/次，可忽略）。
- 对外读取走 `HttpOcrService::last_profile()`，返回一个 POD 结构拷贝，加锁把 `m_engine` 快照转为
  `OnnxOcrEngine*` 后取值。**非 ONNX 引擎或未跑过 → 返回全 0 + `engine` 字段给档位名**。

### 3.4 不做的事（明确范围）

- 不记录**每次**调用的历史（环形缓冲会让内存与锁开销失控）。只保留"上一次"。
- 不做 p50/p99 统计（需要长期聚合，交给调用方用多次 `total_ms` 自己算）。

---

## 四、验收判据（可执行）

| # | 判据 | 验证方式 |
|---|---|---|
| F1 | `FindStrBatch` 多目标一次调用，全部命中且 `found=true` | gtest |
| F2 | `match=exact` 不命中子串（`909` 不匹配 `1909`/`9090`） | gtest（**反向验证核心**） |
| F3 | `cx1/cx2` 落在被找子串的真实字符范围内，而非整行 | gtest（用合成图已知字位） |
| F4 | `cx1 >= x` 且 `cx2 <= x2`（字符框不越出行框） | gtest |
| F5 | `fuzzy=1` 能匹配单字形近错误，`fuzzy=0` 不能 | gtest |
| F6 | `regex` 命中的范围等于该 match 的范围；非法正则不崩溃、`found=false` | gtest |
| F7 | 旧 `OpFindStr` / `OpFindStrEx` 输出**逐字节不变** | 同图新旧对比 |
| F8 | `OpFindStrEx2` 的 flags 与 `FindStrBatch` 的 object 配置给出**一致结果** | 交叉比对 |
| F9 | `OcrLastProfile` 各分项非负且 `total_ms >= det_ms+cls_ms+rec_ms` | gtest |
| F10 | `OcrLastProfile` 的 `rec_width` 与 P2 动态宽度一致（超长行 > 320） | gtest |
| F11 | 全量回归不新增失败 | `op_test.exe` |
| F12 | 真实素材回归 `test_gmplug.py` 仍 PASSED | 脚本 |

**反向验证清单**（必须逐条实做，故意改错后确认对应用例 FAIL）：

1. 把 `exact` 分支改回 `contains` → **F2 必须 FAIL**。
2. 把 `cx1 = chars[idx].x1` 改成 `cx1 = 行框.x1` → **F3 必须 FAIL**。
3. 把 `cx1` 再额外加一次 det 框原点 → **F4 必须 FAIL**。
4. 把模糊的 `ed <= fuzzy` 改成恒 `true` → **F5 前半必须 FAIL**（`fuzzy=0` 也会命中）。
5. 把 `total_ms` 写成常量 → **F9 必须 FAIL**（`total_ms >= 分项和`）。

---

## 五、任务拆解

| 步骤 | 内容 | 文件 |
|---|---|---|
| S1 | 匹配内核 `str_match.h/.cpp`（纯函数、可单测） | `libop/algorithm/StrMatch.*` |
| S2 | 能力层 `find_str_batch` / `find_str_ex2` + JSON 构建 | `ImageSearchService.cpp/.h` |
| S3 | 引擎 profile 采集 | `OnnxOcrEngine.cpp/.h`、`OcrService.h/.cpp` |
| S4 | 能力层 `ocr_last_profile` | `ImageSearchService.cpp/.h` |
| S5 | 三层导出（`op_c_api.h`/`op_c_api.cpp`/`libop.h`/`OpOcr.cpp`） | — |
| S6 | COM：IDL 442/443/444 + `OpAutomation.h/.cpp` | — |
| S7 | 测试 `tests/find_str_batch_test.cpp` + 登记 CMakeLists | — |
| S8 | 构建 x64 + 全量回归 + `test_gmplug.py` + 反向验证 | — |
| S9 | 文档（API 说明）+ CHANGELOG + 提交 | — |

**不做**：x86 构建（用户明确 x86 失败可接受）；发布件同步（用户要求先不发布）；手册 `api_reference.html` 重生成
（不在用户当前授权范围内，且需单独确认口径）。
