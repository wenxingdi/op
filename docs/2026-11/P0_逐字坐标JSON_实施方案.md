# P0 实施方案：逐字置信度 + 逐字坐标 + JSON 结构化输出

> 目标提交：第二波第一项（《OP全面增强建议.md》§9.2 P0，含 P5 `is_partial` 并入）
> 状态：**方案已定稿（四项决策已于 2026-11-20 评审确认，见 §7.5），可进入实现**
> 覆盖范围：原图路径 + C API + COM（易语言）三层；不含颜色二值化路径的提升改动（见 §7 范围决策）

---

## 一、目标与验收标准

### 1.1 新增对外能力

| 层 | 新增符号 | 形态 |
|---|---|---|
| C API | `OpOcrEx2(handle, x1,y1,x2,y2, color, sim)` | 返回 JSON 字符串 |
| C API | `OpOcrAutoColorEx2(handle, x1,y1,x2,y2, color, sim, int* find_count)` | 返回 JSON 字符串 + 行数 |
| C++ | `op::Op::OcrEx2(...)` / `op::Op::OcrAutoColorEx2(..., long& ret_count)` | `std::wstring&` out |
| COM | `OcrEx2` = **DISPID 440**，`OcrAutoColorEx2` = **DISPID 441**（易语言刚需） | `BSTR* retstr` / `LONG* ret` |

返回 JSON 形状（沿用既有字段命名风格 `x1,y1,x2,y2/conf/text`，降低调用方迁移成本）：

```json
[{"text":"冰晶石林","conf":0.93,"box":[120,340,268,362],
  "is_partial":false,
  "chars":[{"ch":"冰","conf":0.97,"x1":120,"x2":156},
           {"ch":"晶","conf":0.95,"x1":157,"x2":193}]}]
```

### 1.2 验收标准（可执行判据）

| # | 判据 | 验证方式 |
|---|---|---|
| A1 | 每个 `chars[i]` 的 `x1,x2` 满足 `box[0] <= x1 <= x2 <= box[2]` | 断言测试，全量样本 |
| A2 | 逐字 x 区间**单调不减**且不重叠（`chars[i].x2 <= chars[i+1].x1`） | 断言测试 |
| A3 | `chars.map(ch).join('') == text` | 断言测试（**反向验证核心**） |
| A4 | 在**无放大、无旋转**样本上，逐字框中心落在该字符真实像素范围内（误差 ≤ 框宽 20%） | 合成字图样本，已知每字位置 |
| A5 | 旧 `OpOcrEx` / `OpOcrAutoColorEx` 输出**逐字节不变** | 回归对比（同图新旧 API 取文本部分比对） |
| A6 | `conf`（行级）与 `chars[].conf`（字级）语义在文档中写明，且行级 conf 口径**与旧 API 完全一致** | 代码审查 + 数值比对 |
| A7 | x64 / x86 双链构建、全量回归不新增失败 | `run_tests.ps1` |
| A8 | COM 层 `regsvr32` 后，后期绑定可达 DISPID 440/441 并返回合法 JSON | PowerShell `New-Object -ComObject op.opsoft` + 易语言实测 |
| A9 | 旧 DISPID 1~439 行为不变（尤其 `OcrAutoColorEx`=382 输出逐字节一致） | 回归对比 |

> **A3 是最关键的一条**：它保证「逐字切分」没有在 CTC 解码阶段错位。故意把 `t1` 写成 `t0+1`（少算一步）必须让 A3 或 A4 FAIL —— 这是 CLAUDE.md「断言必须反向验证」的要求。

---

## 二、现状核对（已读代码确认）

### 2.1 调用链

```
C API  OpOcrAutoColorEx            (libop/c_api/op_c_api.cpp:1602)
  → op::Op::OcrAutoColorEx         (libop/op/OpOcr.cpp:466)
    → image_proc.ocr_auto_color_ex  (能力层，内部走 OcrService)
      → OcrEngine::ocr()            (返回 vocr_rec_t = vector<ocr_rec_t>)
        → OnnxOcrEngine::ocr()      (libop/ocr/OnnxOcrEngine.cpp)
          → rec_images() / rec_image()
            → ctc_greedy()          (OnnxOcrEngine.cpp:292)
```

### 2.2 关键数据结构

`libop/base/Types.h:140` —— `ocr_rec_t` 只有 `left_top / right_bottom / text / confidence`，**没有任何逐字槽位**。逐字数据必须新增字段或新增类型。

### 2.3 三个必须先解决的硬约束（交接记录未提及）

**坑 1｜`ctc_greedy` 丢弃了字符的 t 区间，且 `conf` 口径混杂**

现状（`OnnxOcrEngine.cpp:334-342`）：只 `text += keys[ki]`，字符对应的时间步 `t` 被丢弃；`conf_sum` 累加的是**全部时间步（含 blank 与重复步）**的概率，最后除以步数。所以现有行级 `conf` 是**含 blank 的时间平均**，不是字符平均。

- 若逐字 conf 取「仅该字符占用的 t 步平均」，则 `chars[].conf` 的平均值 ≠ 行级 `conf`，调用方会困惑。
- **决策**：行级 `conf` **保持现有口径一字不改**（保护 A6/兼容性）；逐字 conf 是新增的独立指标，文档写清两者不可互推。可选在 JSON 里额外给 `"conf_char_avg"`，但这会增加字段，暂不加。

**坑 2｜线性映射的分母不是 `T` 和 `320`**

交接记录写的是 `x = t/T*roi_w`。这**不成立**。`rec_images`（`:394-429`）里每个 ROI 各自 `resize_bilinear(roi, RH, tw)` 缩到**不同宽度 `tw`**，然后**右侧补零 pad 到 320**。因此：

- `t/T*320` 得到的是「含右侧空白 padding 的 320 宽坐标系」，不是 ROI 像素系。
- 正确映射：`x_roi = t / T * tw`，其中 `tw = min(320, ceil(48 * roi.w/roi.h))`。
- 当 `tw` 被 320 截断（超宽行）时，ROI 被横向压扁，映射仍然成立（线性同压缩），但**精度按压缩比损失**——正好是 P2 要解决的问题。P0 阶段照实映射即可，并在文档标注该局限。

**坑 3｜PCA 倾斜校正会改写 ROI，坐标不可直接映射回框**

`ocr()` 在 `:650-654` 对 `roi.w > 1.3*roi.h` 的框做 `pca_angle` + `rotate(roi, -ang)`。旋转后 ROI 的像素系与 `kept[i]` 的轴对齐框**不再是简单线性关系**（需旋转逆变换 + 外接框校正）。

- **决策（本方案的核心取舍）**：逐字坐标只对**未旋转的行**做精确映射；对发生了旋转的行，`chars` 数组**整体退化为整行框范围**并置 `"chars_partial":true`，即不给出错误的逐字坐标。
- 理由：宁可退化也不静默给出偏移坐标——A4 判据要的就是「坐标可信」。旋转行在实测样本中占比很低（`|ang| ∉ (15°,75°)` 即不旋转）。
- 后续可在 P2 之后补旋转逆变换，作为独立小提交。

---

## 三、实现方案

### 3.1 第 1 层：引擎层解出逐字信息

**`OcrService.h` 新增逐字结构（放在 `ocr_rec_t` 旁边，避免污染 `Types.h` 的公共类型）**

```cpp
// libop/ocr/OcrService.h
struct ocr_char_t {
    wstring ch;         // 单字符（UTF-16，可能是代理对的两个 wchar_t，见下）
    float confidence;   // 仅该字符占用时间步的概率均值（与行级 conf 口径不同，见文档）
    int t0, t1;         // 占用的时间步区间 [t0, t1]
    int x1, x2;         // 原图像素坐标（已除回放大）；旋转行为整行框
};
```

> ⚠ 中文字在 UTF-16 里多数是**单个 wchar_t**（BMP），但 emoji / 生僻字是代理对。`keys` 里存的是 UTF-8 字符串。**决策**：`ch` 保留 `wstring`，按 `keys[ki]` 解码后的完整用户可见字符填充（CTCLoss 一个 output step 对应一个 dictionary 条目 = 一个完整字符），因此代理对会占 2 个 `wchar_t`，但仍是 1 个 `chars[]` 元素。这样 A3 判据 `join(chars.ch) == text` 依然成立。

**`ctc_greedy` 改造（新增重载，不破坏旧签名）**

```cpp
// 旧签名保留：内部转为调用新签名并丢弃逐字信息（保证 rec_image / 旧路径零行为变化）
std::string ctc_greedy(const float *out, const std::vector<int64_t> &shape,
                       const std::vector<std::string> &keys, float &conf,
                       const std::vector<uint8_t> &mask);

// 新签名：额外产出逐字 t 区间与字级 conf
std::string ctc_greedy(const float *out, const std::vector<int64_t> &shape,
                       const std::vector<std::string> &keys, float &conf,
                       const std::vector<uint8_t> &mask,
                       std::vector<CharSpan> *chars);   // CharSpan = {ki, prob, t0, t1}
```

在现有 `if (best != 0 && best != lastc)` 分支里，当前字符的 t 区间结束于**该字符最后一次连续出现的步**（CTC collapse 语义：同一字符连续重复只保留第一个，区间应延伸到最后一次出现的步）。实现要点：把「确认一个字符完整结束」推迟到「遇到 blank 或切换字符」时，此时回填 `t1`。

**`rec_images` 改造**：新增可选出参 `std::vector<std::vector<ocr_char_t>> *per_char`，回填时用**该行自己的 `tw`** 做 `x_roi = t * tw / T`，再由调用方乘 `inv`（`1/up`）并加上 `box.x0` 偏移。

> 关键：`t0/t1` 的语义定为「闭合区间 `[t0, t1]`」，且 `x2` 取 `t1+1` 的映射（右开），保证相邻字符不重叠（A2）。

**`ocr()` 填充**：在 `:666-682` 的循环里，若 `rois[i]` 未被旋转，则把 `per_char[i]` 映射到原图坐标；若被旋转，则 `chars` 退化为整行框 + `chars_partial=true`。是否旋转要在 `:650` 处记一个 `std::vector<uint8_t> rotated` 标记带下来。

### 3.2 第 2 层：能力层产出 JSON

不改 `ocr_rec_t` 的既有字段（保护旧调用方），而是**并行新增一条能力层方法**：

```cpp
// image_proc 层（与 ocr_auto_color_ex 并列）
long ocr_ex2(const std::wstring &color, double sim, std::wstring &json);
long ocr_auto_color_ex2(const std::wstring &color, double sim, std::wstring &json);
```

JSON 手工拼接（仓库已有 `json/` 目录，先确认是否复用；若无轻量 writer 则用 `std::wostringstream` + 显式转义，**必须转义 `"` `\` 控制字符**）。

`is_partial`（P5 并入）：`box[3]` 与 ROI 底边（即 `h`）距离 `< 2px` 时置 true，并**只对 JSON 里的 `conf` 降权 ×0.8**——注意此处降权若影响行级 conf，会破坏 A6。**决策**：JSON 中输出两个字段 `"conf"`（原值，口径同旧 API）与 `"is_partial"`，降权交给调用方；或输出 `"conf":原值, "conf_partial":原值*0.8`。**推荐后者**：既不破坏口径可比性，又给调用方现成值。

### 3.3 第 3 层：对外三层导出

| 文件 | 改动 |
|---|---|
| `include/op_c_api.h` | 新增 2 个 `OP_C_API` 声明 + 注释（JSON 形状、坐标系、conf 口径） |
| `libop/c_api/op_c_api.cpp` | 新增 2 个实现，沿用 `call_string` + `out_int` 模式 |
| `include/libop.h` | 新增 2 个 C++ 方法声明 |
| `libop/op/OpOcr.cpp` | 新增实现，沿用 `with_captured_region` 模式 |
| `libop/com/op.idl` | 新增 `[id(440)]` / `[id(441)]`（COM 刚需，见 §3.5） |
| `libop/com/OpAutomation.h` | 新增 2 个 `STDMETHOD` 声明 |
| `libop/com/OpAutomation.cpp` | 新增 2 个实现，沿用 `CopyOutBstr` 模式 |

### 3.5 COM 层实现（易语言刚需，已确认必做）

**关键事实：本仓库 COM 层是纯 IDL 驱动的 dual 接口，没有手写 MFC DISPATCH_MAP。**

- `IOpAutomation` 在 `op.idl:26-38` 声明为 `dual, nonextensible`，`dispatch` 部分由 MIDL 从 `[id(N)]` 自动生成 —— 加方法**只需在 IDL 加一行 + 在 `OpAutomation.h/.cpp` 加声明实现**，不存在遗漏 map 条目的风险。
- 现有 DISPID 范围 **1~439**（317 个 id，编号不连续），最大 439 = `HttpDownloadFile`（`op.idl:399`）。**新增从 440 起连续分配：`OcrEx2`=440、`OcrAutoColorEx2`=441**。追加在末尾而非插入中间，避免任何按序号假设的调用方错位。
- 签名沿用既有 OCR 家族惯例（`OpAutomation.h:495` 的 `OcrAutoColorEx`）：
  - `[id(440)] HRESULT OcrEx2(LONG x1,y1,x2,y2, BSTR color, DOUBLE sim, [out] BSTR* retstr)` —— 对齐 C API 的 `OpOcrEx2`（无行数出参）
  - `[id(441)] HRESULT OcrAutoColorEx2(..., [out] BSTR* retstr, [out, retval] LONG* ret)` —— 对齐 `OpOcrAutoColorEx2`（有行数）
- 实现模式照抄 `OpAutomation.cpp:1693-1700`：`if (!retstr) return E_POINTER;` → 调 `obj.*` → `return CopyOutBstr(retstr, s);`

**⚠ 易语言踩坑（本项目已有实证，必须遵守）**

`op.idl:33-37` 记录了一条血泪注释：`VARIANT*` 出参必须写成 `[in, out]` 而非 `[out]`，否则易语言/VBScript 这类**后期绑定**客户端读不回。本期两个新方法出参是 `BSTR*` 和 `LONG*`，**不是 `VARIANT*`**，按 `OcrAutoColorEx` 既有写法用 `[out]` / `[out, retval]` 即可，不受该约束。但若后续加 `VARIANT*` 出参（例如返回结构化数组）必须遵守该规则。

**构建接线已核实（无需改 CMakeLists）**

`libop/CMakeLists.txt:459-468` 已把 `com/op.idl` 列为 `add_custom_command` 的 `DEPENDS`，`midl` 自动重生成 `_i.c` / `.h` / `op.tlb`，且 `com/op.rc` 通过 `OBJECT_DEPENDS` 依赖 `.tlb`。**改 IDL 即自动触发重编，不需要动 CMakeLists.txt。**

> 唯一注意：`.tlb` 打进 `com/op.rc` 作为类型库资源 —— 这就是为什么**必须重新 `regsvr32`** 才能让易语言看到新方法。

**类型库版本策略（重要）**

`LIBID_opLib` 固定为 `40297cce-...`，`version(1.0)`（`op.idl:405-406`），`OpAutomation.h:38` 也硬编码 `wMajor=1, wMinor=0`。

- **决策：不改 LIBID、不改版本号，仅在接口尾部追加方法。** 这是 COM 的兼容扩展点 —— 旧客户端按旧 DISPID 调用不受影响，新方法对旧 typlib 不可见但运行时可用（dual 接口 + 后期绑定）。
- **必须做**：用户机器上要**重新 `regsvr32 op_x64.dll`**，否则注册表里还是旧类型库，易语言的"引用"看不到新方法。这是发布说明里必须写明的一条。
- **实证步骤**：`regsvr32` 后用 `D:\AutoPro\OPTool` 的 OPTestTool（走 C API）与一个易语言/PowerShell 后期绑定脚本各验一次。PowerShell 可用 `New-Object -ComObject op.opsoft` + `$op.OcrEx2(...)` 快速验证 DISPID 440 可达。

### 3.4 测试

新增 `tests/ocr_ex2_test.cpp`（需登记 `tests/CMakeLists.txt`）：

1. `CharsMatchText` — A3：`join(chars.ch) == text`
2. `CharsWithinBox` — A1 + A2：区间内含 + 单调不重叠
3. `CharsHitRealGlyphs` — A4：合成图（固定字距绘制 3 个汉字），断言逐字中心落在已知 x 段
4. `BackwardCompat` — A5：同图旧 `OpOcrEx` 与新 API 的 `text` 一致
5. `OldApiUntouched` — A6：旧 API `conf` 数值不变（与改动前基准值硬编码比对）

**反向验证**（CLAUDE.md 强制）：把 `x2 = t1 * tw / T` 改成 `x2 = t0 * tw / T` 后，用例 2 必须 FAIL；把 `t1` 回填漏掉后，用例 1/3 必须 FAIL。

---

## 四、风险与对策

| 风险 | 影响 | 对策 |
|---|---|---|
| 逐字 x 依赖 `tw` 而 `tw` 未随 `per_char` 传出 | 坐标系统性缩放到 320 系（A4 全错） | `rec_images` 内部直接完成 `t→x_roi` 映射，只外传 ROI 内像素坐标 |
| 旋转行坐标不可信 | 部分样本坐标偏 | `chars_partial=true` 退化，不输出错误值 |
| 行级 conf 口径被无意改动 | 旧调用方阈值行为漂移 | A6 硬编码基准值比对；新逻辑只走新重载 |
| UTC/JSON 转义遗漏 | 文本含 `"` 或 `\` 时 JSON 破损 | 专门用例 `SpecialCharsEscaped`（识别带引号的文本） |
| 代理对字符切分 | A3 失败 | `ch` 按完整字典条目填充，不按 wchar_t 切 |
| 全量回归新增失败 | 阻塞提交 | 已知 flake 清单见交接记录 §四；单跑复核 |

---

## 五、任务拆解与工作量

| 步骤 | 内容 | 预估 |
|---|---|---|
| S1 | `ctc_greedy` 新重载 + `CharSpan` + `ocr_char_t` | 0.3 天 |
| S2 | `rec_images` 出参 + `tw` 映射 + `ocr()` 回填（含旋转标记） | 0.3 天 |
| S3 | 能力层 `ocr_ex2` / `ocr_auto_color_ex2` + JSON 拼接（含转义、is_partial、conf_partial） | 0.4 天 |
| S4 | 三层导出（h/cpp/OpOcr.cpp） | 0.2 天 |
| S4b | **COM 层：IDL id(440/441) + OpAutomation.h/.cpp + regsvr32 后期绑定实证** | 0.4 天 |
| S5 | 5 个用例 + 反向验证 | 0.3 天 |
| S6 | 构建 x64/x86 + 同步发布件 + 全量回归 + 手册重生成 + CHANGELOG | 0.4 天 |
| 合计 | | **≈2.3 天**（含 COM；不含 COM 为 1.8 天） |

> 原文档估 1~1.5 天偏乐观，主要差在 S2 的坐标映射/旋转处理与 S4b 的 COM 双链验证。

---

## 六、验证与交付流程（按 CLAUDE.md 铁律）

1. 改码 → `python scripts/build/_wb_build.py`（**不加 `--target`**）
2. 构建完 → `D:\AutoPro\OPTool\sync_op_dll.py`
3. 测试：PATH 带 `build/nmake-x64-Release/libop`，cd 仓库根跑 `op_test.exe`；**测试期间不构建**
4. x86：`scripts/build/_wb_build_op_x86.py`（PATH 前置 VS 自带 Ninja 与 CMake\bin）
5. 回归：全量 `run_tests.ps1`，与交接记录基线对比（472/473，唯一已知 flake = `CaptureModeTest.DownCpuAddsDelayAfterEachCapture`）
6. 提交信息中文 `feat:` 前缀 + 追加 `docs/CHANGELOG.md`（下一批次编号接第三十七批 → **第三十八批**）
7. 端到端真机：用交接工作区的 `cls_func_test.py` 改造为 P0 回归脚本（文档 §五已指明可复用）

---

## 七、范围决策（本期明确不做）

| 项 | 决策 | 理由 |
|---|---|---|
| ~~COM 层 `OcrEx2`~~ | **改为本期必做**（用户确认易语言刚需） | 见 §3.5；实际成本 0.4 天，远低于原估 0.5 天，因为 COM 分发全由 IDL 生成、无需手写 map |
| 颜色二值化路径的逐字坐标 | **不做**（`OcrAutoColorEx2` 复用原图路径实现，color 仅透传既有语义） | 二值化会改变字形边界，逐字框语义需单独定义；先把原图路径的映射跑通 |
| 旋转行的旋转逆变换 | **不做**，退化 + `chars_partial:true` 标记 | **用户已确认接受** |
| `api_reference.html` 手册重生成 | **做**（S6 内） | 新增导出必须进手册，否则 316/310 统计口径漂移 |
| P1 FindStrBatch / P2 动态 rec 宽 / P3 profile | **不在本期** | 独立提交；P1 与 P0 无依赖，可并行 |

---

## 七.5 已确认决策（2026-11-20 评审）

| # | 待决点 | 决策 | 落地位置 |
|---|---|---|---|
| 1 | 逐字 conf 与行级 conf 口径不同 | **接受不可互推**。行级 `conf` 保持旧口径一字不改；`chars[].conf` 为字级指标；两者在 `libop.h` / `op_c_api.h` 注释与手册中写明"不可互推" | §2.3 坑 1、A6 |
| 2 | 旋转行坐标 | **接受退化**：`chars` 整体退化为整行框 + `chars_partial:true` | §2.3 坑 3 |
| 3 | `is_partial` 降权形态 | **按推荐**：输出 `conf`（原值，口径同旧 API）+ `conf_partial`（原值×0.8）两个字段并存，把选择权交给调用方 | §3.2 |
| 4 | COM 层 | **本期必做**（易语言刚需），`OcrEx2`=DISPID 440 / `OcrAutoColorEx2`=441 | §3.5 |

> 决策 3 的具体口径：`conf_partial` 仅在 `is_partial==true` 时输出该字段（否则省略），避免调用方误用。JSON 中 `conf` 永远是引擎原值。

---

## 八、待评审确认点

**四项已于 2026-11-20 评审确认，见 §7.5。** 方案已定稿，可进入实现。

实现前仅剩两处**技术性**开放式项（不需要用户决策，实现时按此办）：

1. **JSON 构造：复用仓库自带的 `op::json` DOM，不要手写字符串拼接。**
   已核实 `libop/json/JsonValue.h` 提供完整 DOM（`Value` + `Array`/`Object` 模型）与**紧凑序列化** `std::wstring Stringify(const Value&)`，且是自包含递归下降实现、不引第三方库、输入输出均为 `wstring`（与 C API/COM 的 `wchar_t` 天然对齐）。
   **转义由 `Stringify` 负责**（含控制字符与代理对处理），S3 只需构建 DOM 树，**禁止手写 `wostringstream` 拼 JSON** —— 手拼必然漏转义。

   构建方式示例：
   ```cpp
   op::json::Value root;  root.type = op::json::Type::Array;
   op::json::Value line;  line.type = op::json::Type::Object;
   // line.obj.push_back({L"text", str_value});  依此类推
   root.arr.push_back(std::move(line));
   std::wstring out = op::json::Stringify(root);
   ```
   注意 `Number` 统一存 `double`：`conf` 输出时用 `%.2f` 级别的精度即可（`Stringify` 内部对可无损整数按整数输出，坐标不会被写成 `120.0`）。

2. **`is_partial` 的边界判据**：det 框底边 `box.y1` 与 ROI 高度 `h` 差值 `< 2px` 判真。该比较必须发生在**除回 `inv` 之后**（即原图坐标系），否则 2x 放大时阈值实际退化为 1px。S2 实现时以此为验收子项。
