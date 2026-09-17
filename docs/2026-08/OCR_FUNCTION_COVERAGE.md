# 字库 / OCR 函数覆盖盘点（未覆盖检查）

> 来源：静态核对 `include/libop.h`（公开声明）、`libop/op/OpOcr.cpp`（门面实现）、`tests/ocr_test.cpp`（测试）。
> 说明："未覆盖"分两层 —— **A. 测试未覆盖**（无 TEST 调用）；**B. 参数校验未覆盖**（门面层透传、未显式拦截）。
> 注意：本环境 `OcrFixture.*` 整 Fixture 在 OCR 服务未启动时 **全 SKIP**，所以"识别类 TEST"多数只是验证"API 能调不崩"，**真实识别正确性 0 验证**。

## 一、公开 API 全量（36 个，按 libop.h 行号）

| 组 | # | 函数 | 行号 |
|---|---|---|---|
| 字库管理 | 1 | `SetDict` | 461 |
| | 2 | `GetDict` | 463 |
| | 3 | `SetMemDict` | 465 |
| | 4 | `UseDict` | 467 |
| | 5 | `AddDict` | 469 |
| | 6 | `SaveDict` | 471 |
| | 7 | `ClearDict` | 473 |
| | 8 | `GetDictCount` | 475 |
| | 9 | `GetNowDict` | 477 |
| 去噪预处理 | 10 | `SetBinaryPreprocess` | 479 |
| | 11 | `GetBinaryPreprocess` | 482 |
| 制库/取点 | 12 | `FetchWord` | 485 |
| | 13 | `FetchWordEx` | 488 |
| | 14 | `ExtractWordRects` | 491 |
| | 15 | `ExtractWordRectsEx` | 494 |
| | 16 | `FetchWords` | 498 |
| | 17 | `FetchWordsEx` | 501 |
| | 18 | `FetchWordsByRects` | 505 |
| 识别(点阵) | 19 | `Ocr` | 530 |
| | 20 | `OcrEx` | 533 |
| | 21 | `FindStr` | 536 |
| | 22 | `FindStrEx` | 539 |
| | 23 | `OcrAuto` | 542 |
| | 24 | `OcrFromFile` | 544 |
| | 25 | `OcrAutoFromFile` | 546 |
| 结果解析 | 26 | `GetWordsNoDict` | 521 |
| | 27 | `GetWordResultCount` | 524 |
| | 28 | `GetWordResultPos` | 526 |
| | 29 | `GetWordResultStr` | 528 |
| 远程引擎 | 30 | `SetOcrEngine` | 454 |
| 调试/校验 | 31 | `GetBinaryPreview` | 509 |
| | 32 | `GetWordPreview` | 512 |
| | 33 | `CheckWordDict` | 514 |
| | 34 | `NormalizeWordDict` | 516 |
| | 35 | `RenameWordDict` | 518 |
| 边界(非字库核心) | 36 | `FindLine` | 548 |
| | 37 | `GetColorNum`(Image 组) | 347 |

## 二、测试覆盖现状

### ✅ 已覆盖（有 TEST 调用，25 个有效）
`SetDict` `UseDict` `GetDictCount` `GetNowDict` `GetDict(仅 idx=0)` `AddDict` `ClearDict` `SaveDict` `SetMemDict` `Ocr` `OcrEx` `OcrAuto` `OcrFromFile`(+WithColorFilter) `OcrAutoFromFile`×2 `FetchWord` `GetWordsNoDict` `FindStr` `FindStrEx` `GetWordResult{Count,Pos,Str}` `SetOcrEngine`×6。

> ⚠️ 但以上识别类 TEST 在本环境 **全 SKIP**（无 OCR 服务），仅验证"调用不崩"，未验证识别结果正确。

### ❌ 未覆盖（无任何 TEST，共 16 个）

| 优先级 | 函数 | 说明 |
|---|---|---|
| 🔴 高 | **`SetBinaryPreprocess`** | **你刚把默认 `mode` 从 0 改 1，竟 0 测试覆盖！** 最简单也最该补 |
| 🔴 高 | **`GetBinaryPreprocess`** | 配套的读回，0 测试 |
| 🟠 中 | `FetchWords` | 批量制库主路径之一，用户高频，0 测试 |
| 🟠 中 | `FetchWordEx` | 带 sim 的单字制库，0 测试 |
| 🟠 中 | `ExtractWordRects` / `ExtractWordRectsEx` | 自动切词，0 测试 |
| 🟠 中 | `FetchWordsEx` / `FetchWordsByRects` | 批量制库变体，0 测试 |
| 🟡 低 | `GetBinaryPreview` / `GetWordPreview` | 调试预览，0 测试 |
| 🟡 低 | `CheckWordDict` / `NormalizeWordDict` / `RenameWordDict` | 字库质检/清洗，0 测试 |
| 🟡 低 | `FindLine` | 找线（非字库核心），0 测试 |
| 🟡 低 | `GetColorNum` | 色统计（Image 组），0 测试 |
| 🟡 低 | `GetDict` 多/错 index、`AddDict` 坏格式 | 仅 `DISABLED_*` 诊断测过，常规无覆盖 |

## 三、参数校验维度（"未覆盖检查"第二层）

### ✅ 门面层（OpOcr.cpp）已做的 null/边界保护
- `FetchWordEx` / `ExtractWordRects*` / `FetchWords*`：`color ? color : L""`、`word ? word : L""` —— null 入参安全。
- `FetchWordsByRects`：`parse_word_rects` 失败 / `rects 数量 != word 数量` → 直接 return（**有参数校验**）。
- `GetWordPreview` / `CheckWordDict` / `NormalizeWordDict` / `RenameWordDict`：`dict_info ? ... : L""`。
- `FindStr`：`retx`/`rety`/`ret` 用 `internal::set_result` 安全写，**null 指针安全**。
- `GetWordResult{Count,Pos,Str}`：对 `null result` / 负 `index` 越界 —— 已被 `OcrParsing.WordResultParsingHandlesBadInput` 测试覆盖（返回 0/空，不崩）。

### ⚠️ 门面层透传、未显式拦截（潜在缺口，需下层兜底）
- `FindStr` / `Ocr`：**`color = null` 直接透传**给 `image_proc`，未在前台判空（依赖 `str2binary` 容忍空串 —— 待查）。
- `FindStr`：**`strs = null` 直接透传**，未判空/空串。
- `OcrFromFile`：**`file_name = null` 直接透传**，未判空。
- **空字库场景**：未 `SetDict` 就 `FindStr`/`Ocr` —— 门面不拦截，依赖下层"字典为空返回 0"，**未测**。
- **`idx` 越界**：`SetDict`/`UseDict`/`AddDict` 传 `idx<0` 或 `idx>99` —— 未测越界行为。
- **`AddDict` 坏格式**：缺 `$` 分隔符 / 点阵长度不符 —— 未测。
- **无效区域**：`FetchWord`/`FetchWords` 传 `x1>x2` 或 `y1>y2` —— 未测。

## 四、建议优先级（补覆盖，按四阶段执行）

1. **🔴 补 `SetBinaryPreprocess` / `GetBinaryPreprocess` 单测**：验证默认 `mode=0→1` 生效 + 三档阈值写入读回一致。零风险、立刻见效，且钉住你刚的改动。
2. **🟠 补 `FetchWords` / `FetchWordEx` / `ExtractWordRects` 单测**：批量制库是用户高频路径，用 `CreateConsoleLikeBmp` 造含字 BMP 验证点阵产出非空。
3. **🟠 补"空字库 / null 入参 / 越界 idx"防御测试**：验证不崩、返回 0/空（若下层未兜底，则转为"加参数校验"修复项）。
4. **🔴 补"大→太"超集回归测试**：构造「大」含噪图 + 字库含「大」「太」，断言优先识别「大」—— 钉住之前根因（根治需 ③④ 改匹配内核）。

---
> 本文件为**盘点（只读）**，未改动任何 OP 源码。下一步若要补测试或加参数校验，按四阶段（发现→确认风险→等你同意→改→编译验证）执行。
