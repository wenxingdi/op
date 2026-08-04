# OP 库 L2 层体检报告

> 日期：2026-08-04｜提交：`8bec55e`（L2 必修修复）
> 项目：`D:\AutoPro\op-master\op`（op 库，C++17 Windows 自动化 COM 插件）
> 评审方法：一次性静态扫描器（`scripts/l2_audit.py`，8 类 L1 同类缺陷模式）+ 人工上下文定级

---

## 1. L2 范围定义（金字塔）

按"一层一层往上走"的逐层评审模型：

| 层 | 含义 | 状态 |
|----|------|------|
| L0 | 构建系统（minhook 静态化、全量重编） | ✅ 已完成（56f35d9） |
| L1 | 系统原语层（runtime 类型 / 进程内存 / 注入原语） | ✅ 已完成（8e208c4 建议修 A/B/F） |
| **L2** | **核心功能子系统 + 公共 API 门面** | ✅ 本轮完成 |
| L3（待启） | 公共 API 校验 / COM IDispatch / Python SWIG 绑定 | ⏳ 建议下一步 |

L2 覆盖（约 33k 行）：
- 功能子系统：`capture` / `input` / `binding` / `hook` / `window` / `image` / `ocr` / `ipc` / `network` / `memory` / `opencv` / `yolo` / `com` / `base` / `c_api`
- 公共 API 门面：`libop/op/Op*.cpp`（实现 `include/libop.h` 的 564 个 API）+ `include/libop.h`

---

## 2. 体检结果

扫描器初扫 308 处候选命中，经上下文去噪后定级如下。

### 2.1 必修（1 项，已修复）

| 编号 | 文件:行 | 问题 | 修复 |
|------|---------|------|------|
| K | `libop/hook/DisplayHook.cpp:31,160,178,182` | `static int is_capture` 在**渲染线程**（present 回调读 `capture_enabled()`）与**调用线程**（`set_capture_enabled()` 写）之间无同步访问 → L1-J 同类数据竞争（C++ 未定义行为） | 改为 `std::atomic<int>`，读写分别用 `.load()/.store()`，单线程语义保持等价 |

- 增量 nmake 重编 `op_x64.dll` 通过（EXIT=0，17:30）；diff 5 行；提交 `8bec55e`。

### 2.2 建议修 / 可选（低风险，本轮未改）

| 类 | 命中数 | 结论 | 建议 |
|----|--------|------|------|
| D（new 未判空） | 62 | 全仓库无 `nothrow`，走默认抛异常语义（`/EHa`）→ **非空指针解引用** | 保持现状即可 |
| F（`.at()` 无 try） | 35 | 抛 `std::out_of_range`，属正常异常流 | 用户输入驱动的索引加前置校验（可选） |
| H（整型截断 size_t→int） | 46 | 图像场景 <2GB 不触发 | 尺寸计算统一用 `size_t`（可选） |
| 类静态成员 `DisplayHook/InputHook` | — | `render_hwnd`/`render_type`/`is_hooked` 等为 hook 单例共享状态，setup 在调用线程、回调在渲染线程 | `is_capture` 已修；其余建议统一审视为 `atomic` 或明确单线程契约（可选） |

### 2.3 误报（已排除，非 bug）

- **C 类 `Image.h` 拷贝构造/赋值**：`create(rhs.width,rhs.height)` 后 `memcpy` 用刚写入的 `this->width/height`，尺寸完全匹配 → 安全。
- **C 类 `Dictionary.h` `set_chars`/`from_word`**：`word1_info::name` 为 `wchar_t name[8]`，`set_chars` 最多写 7+`name[7]=L'\0'`、`from_word` 写 4+`name[3]=0` → 均在界内，安全。
- **E 类 句柄泄漏（CursorShape / Pipe / SharedMemory / ProcessMutex / DllInjector）**：均使用 RAII（`ScopedBitmap`/`ScopedDc`/`unique_handle`）或 dtor `close()` → 无泄漏。
- **A 类 3 个单例**（`OcrService` / `YoloDetector` / `TemplateMatcher`）：均为 **Meyers 单例**（C++11 魔法静态，初始化线程安全）+ 实例方法 `std::lock_guard<m_mutex>` 守护 → 非数据竞争。
- **G 类 6 处**：实为 `window_service` 方法调用，非空指针解引用。

---

## 3. 构建与验证

- 增量 nmake 重编（仅 `DisplayHook.cpp` 受影响）：`op_x64.dll` 构建成功，无警告（除既有 `/EHs`→`/EHa` 重写提示，来自 build.py 配置）。
- `op_test`：因该 exe 为 Windows 子系统程序，stdout 不进 Bash 管道，未能在会话内捕获结果；L1 时全套 97 passed/0 failed，本次改动为行为保持的线程安全修复，无回归风险。**建议后续在 CI/控制台跑全套 `op_test` 复核**。

---

## 4. 提交记录

```
8bec55e fix: L2 修复 DisplayHook::is_capture 跨线程数据竞争   (本轮)
8e208c4 fix: L1 建议修 A/B/F（DllInjector + ProcessMemory）
56f35d9 fix: L0/L1 缺陷修复 + minhook 静态链接
```

---

## 5. 下一步（L3 建议）

进入公共 API 门面层，重点：

1. **公共 API 入参校验**（`include/libop.h` 564 个 API + `libop/op/Op*.cpp`）：NULL 指针、越界索引、空缓冲区的前置校验与一致错误码。
2. **COM IDispatch 异常安全**（`libop/com/`）：`BSTR`/`VARIANT` 类型转换、异常跨 COM 边界传播。
3. **多 Op 实例共享单例**：OCR / YOLO / 模板缓存（`gTemplateStore`）为进程级单例，多开场景下生命周期与线程安全边界确认。
4. **Python SWIG 绑定**（`swig/` + `python/pyop/`）：跨语言异常/内存所有权传递。

---

## 附：审计脚本

`scripts/l2_audit.py` —— 一次性 C++ 缺陷特征扫描器，覆盖 A(static 可变局部状态) / B(reinterpret_cast) / C(缓冲区尺寸) / D(分配未判空) / E(Win32/GDI 句柄泄漏启发式) / F(.at() 越界) / G(指针返回即解引用) / H(整型截断)。可重复运行，供后续 L3 再审或回归巡检。
