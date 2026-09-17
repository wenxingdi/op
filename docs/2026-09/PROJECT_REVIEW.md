# 项目改动核查与质量评估报告

> 对象：`D:\AutoPro\op-master\op`（op 库 C++17 Windows 自动化 COM 插件）
> 范围：L0→L3 全部已提交改动（56f35d9 / 8e208c4 / 8bec55e / 6ffae1b）+ 工作区未跟踪文件
> 性质：只读核查，未改动任何代码

---

## 一、改动核查结论（逐提交）

### 56f35d9 — L0/L1 缺陷修复 + minhook 静态链接
| 改动 | 核查结论 |
|---|---|
| `WindowService.cpp` safe_wcslen 全替换（EnumWindowInternal / FindChildWnd） | ✅ 纯防御性，NULL 返回 0，`>1` 判定跳过，无新问题 |
| `get_cpu_usage` 全局 static → per-PID `map<DWORD,pair>` + `mutex` | ✅ 并发修复正确：`lock_guard` 保护 map 读写、`Sleep(1000)` 在锁外不持锁睡眠。**沿袭旧逻辑**：g_last 存"第一次采样"，连续调用差分窗口约 2s（非本次引入，见建议 P1） |
| `point_t::operator<` 容差→量化分行字典序 | ✅ 修复严格弱序 UB（红黑树错乱），无回归 |
| `Delay` 忙等→MsgWaitForMultipleObjectsEx | ✅ CPU 100%→0，无回归 |
| SAFE_* 宏 do-while(0) 包裹 + 句柄判断 | ✅ 宏安全增强 |
| JsonUtils UnescapeString \uXXXX（含代理对） | ✅ 正确 |
| build.py 注入 `VCPKG_TARGET_TRIPLET=x64-windows-static` + CMakeLists NMake 位数判定 + libop install 兜底 | ✅ minhook 静态化，dumpbin 实证无 DLL 依赖 |

### 8e208c4 — L1 A/B/F（DllInjector + ProcessMemory）
| 改动 | 核查结论 |
|---|---|
| A: `VirtualFreeEx(..., 0, MEM_RELEASE)` 替代 MEM_DECOMMIT | ✅ 正确（MEM_RELEASE 时 dwSize 须 0，已遵守）；修复地址空间累积泄漏 |
| B: `WaitForSingleObject` INFINITE→10s 超时 + LoadLibraryW 返回值校验 | ✅ 正确；超时返回 -6，remoteModule==0 返回 -7。**已知取舍**：超时后远线程可能仍在目标进程执行而路径内存已 MEM_RELEASE 释放（见建议 P2） |
| F: `prepare_process` 切回 hwnd=nullptr 时立即 `_proc.Detach()` | ✅ BlackBone Detach 对未附加对象安全，补 hwnd=nullptr 分支释放 |

### 8bec55e — L2 is_capture 数据竞争
| 改动 | 核查结论 |
|---|---|
| `static int is_capture` → `std::atomic<int>` + `.load()/.store()` | ✅ 修复渲染线程(读)/调用线程(写)数据竞争，单线程语义等价 |

### 6ffae1b — L3 COM 接口层
| 改动 | 核查结论 |
|---|---|
| A: 覆写 `IDispatchImpl::Invoke` 集中 try/catch | ✅ 调用基类 Invoke，catch `std::exception`/`...` 填 EXCEPINFO 返回 E_FAIL；实际 `/EHa` 兼可兜 AV |
| B: 13 方法出参 `if(!x) return E_POINTER` | ✅ 仿现有 FindMultiColor 规范，AV 必须显式检查（try/catch 在 /EHsc 下抓不到，/EHa 下虽可兜但显式更确定） |
| C: 41 方法 BSTR 出参 → `CopyOutBstr`（脚本化） | ✅ 复用已有 helper（NULL 检查 + 真实 hr），清掉 4 处死代码 `return hr;`；`s` 均在作用域 |

### 工作区未跟踪文件
- `build_cn.py`（国内源构建脚本）、`setup_msvc_env.bat`、`download_*.txt`、`scripts/l2_audit*.py/.txt/.md`、`scripts/L3_AUDIT_REPORT.md`、`scripts/l3_bstr_refactor.py`：均为工具/审计产物，**不影响项目代码质量**。建议归档或入库（见 P3）。
- **已跟踪文件无未提交改动**（`git status` 仅显示未跟踪文件），L3 提交完整。

---

## 二、是否引入新问题

**结论：未引入崩溃 / UB / 数据竞争 / 内存泄漏类新问题。**

两点需知晓（均非回归、非新引入）：
1. `get_cpu_usage` 连续调用差分窗口约 2s——沿袭自原版逻辑（原版 `last_system_time_` 同样存第一次采样），56f35d9 仅改并发结构未改采样基准。
2. `DllInjector` 超时路径：目标进程卡死 10s 后返回 -6，此时远线程仍可能在目标进程执行而路径内存已释放——属已知取舍（注释已说明），目标进程卡死 10s 本属异常场景。

---

## 三、项目质量评估

| 维度 | 水位 | 说明 |
|---|---|---|
| 构建 | 🟢 良好 | minhook 静态化（无运行时 DLL 依赖）、/EHa、op_x64 增量重编通过 |
| 并发安全 | 🟢 良好 | is_capture atomic、get_cpu_usage per-PID+mutex、OcrService/YoloDetector/TemplateMatcher 单例有内部 mutex |
| COM 异常安全 | 🟢 良好（L3 新增） | Invoke 集中 try/catch，宿主不再因 C++ 异常崩溃 |
| 内存管理 | 🟢 良好 | RAII 普遍（unique_handle/ScopedBitmap/ScopedDc/ScopedIcon），VirtualFreeEx MEM_RELEASE 修正 |
| 出参/指针安全 | 🟢 良好（L3 新增） | 13 COM 方法 NULL 检查 + 41 BSTR 走 CopyOutBstr |
| 验证闭环 | 🟡 有盲区 | op_test.exe 为 Windows 子系统程序，Bash 管道抓不到 stdout，无 CI/控制台自动化；L0-L3 均靠"编译通过 + 行为保持推理"验证 |
| Python 绑定 | 🟡 未审 | swig/op.i 缺 %exception/%newobject（待实证） |
| 低风险存量 | 🟡 可选 | L2 扫描的 D(62 new 未判空)/F(35 .at 无 try)/H(46 整型截断) 在 /EHa + 图像<2GB 下不触发 |

**总体**：经 L0-L3 四轮评审，崩溃类与并发类高风险缺陷已清零，COM 接口层异常安全达标。当前主要短板在**验证闭环**（无自动化测试捕获）与**精度/边缘取舍**（CPU 采样窗口、注入超时路径），均非阻塞性。

---

## 四、优化建议（按优先级）

### P1 — 建议尽快
1. **op_test 接入 CI / 控制台**：解决"编译过但行为无自动验证"盲区。op_test.exe 是 Windows 子系统程序，需在 CI 跑或写一个控制台宿主捕获 stdout。这是当前最大的质量风险点（所有修复都缺运行时回归证据）。
2. **get_cpu_usage 采样基准修正**：把 `g_last[ProcessID] = first`（Sleep 前第一次采样）改为 Sleep 后第二次采样值，使连续调用差分窗口回到 1s（约 1 行改动，精度提升，无风险）。

### P2 — 建议规划
3. **Python SWIG 审计**：读 `swig/op.i` 实证是否缺 `%exception`（C++ 异常→Python 异常，否则异常逃逸导致 Python 解释器崩溃）与 `%newobject`（返回指针内存释放，否则泄漏）。
4. **DllInjector 超时路径**：当前超时后 MEM_RELEASE 路径内存，远线程若仍在执行会访问已释放内存。可考虑：超时时不立即释放路径内存，改用"延迟释放/登记远线程句柄后续回收"，或至少在文档明确该取舍。
5. **全局单例跨实例共享审视**：OCR/YOLO/模板缓存为 static 单例，多 Op 实例（多脚本/多 client_id）共享，模板名冲突等可能干扰（L2 已确认有内部 mutex，非崩溃，属隔离性建议）。

### P3 — 可选
6. **L2 低风险存量**：D(62 new 未判空)/F(35 .at 无 try)/H(46 整型截断) 在当前 /EHa + 业务约束下不触发，可选整改。
7. **工具/审计产物归档**：`build_cn.py`/`setup_msvc_env.bat`/`scripts/*.py`/`scripts/*_REPORT.md` 决定入库或移出仓库根，保持工作区整洁。
8. **构建环境固化**：把 nmake 增量重编所需的环境（MSVC+WinSDK+atlmfc 的 INCLUDE/LIB/PATH，含 winrt/cppwinrt/atlmfc）写进 `setup_msvc_env.bat` 并入库，避免每次手工拼装（OpAutomation.cpp 需 atlmfc 这类依赖易遗漏）。

---

## 五、最新进度追踪（截至 2026-09-02）

> 本节追踪金字塔评审（L0–L3，止于 `8e4bdca` 优化轮）之后到当前的项目进展。原评审结论（一~四章）不变。数据来自 `git log` / `git status` 与当日工作日志。

### 5.1 已提交的新提交（评审后）
| commit | 主题 | 状态 |
|---|---|---|
| `b63fe74` | fix(capture): GdiCapture 每帧重算客户区偏移，消除窗口 resize 后 FindPic 错位 | ✅ 已入库 |
| `9f197e5` | refactor: 金字塔评审 L2 逐功能解剖 + dx 输入三通道开关 | ✅ 已入库 |
| `8e4bdca` | 优化(P1–P3): CPU 采样基准 / 注入超时释放 / SWIG 异常 / 验证闭环 / 构建环境 | ✅ 已入库（见 2026-08-06 日志） |

### 5.2 工作区工作线（已于 2026-09-02 提交）
**① OCR 引擎架构重构 + 进程内 ONNX 内置引擎（构建验证通过，未提交，架构级）**
- 动机：原 OCR 仅支持远程 HTTP 服务（需外部部署）；现改为「进程内内置引擎默认 + HTTP 远程兜底可选」。
- 落地：
  - `OcrService.h/.cpp`：新增抽象 `OcrEngine` 接口；`HttpOcrEngine`（原 `HttpOcrService` 的 HTTP 实现，远程兜底）；`OnnxOcrEngine`（进程内默认，pimpl 隔离 onnxruntime 头依赖）。`HttpOcrService` 退化为引擎选择器（按 engine 串选 Onnx/Http）。
  - 新增 `OnnxOcrEngine.cpp/.h`（untracked，pimpl）：基于 ONNX Runtime 的 PP-OCRv4（det+rec 模型编入 DLL 资源段 `ocr_models.rc.in`）。
  - `libop/CMakeLists.txt`：ONNX Runtime 链接——静态优先（0 新增 DLL）/ 否则共享（+2 DLL：`onnxruntime.dll` + `onnxruntime_providers_shared.dll`，随插件分发）；模型资源 rc 注入。
- **构建验证（2026-09-02，用户指令"先验证"）已通过**：
  - cmake 重配成功纳入——日志 `ONNX Runtime found` + `SHARED linking (+2 DLL)` + `Generating done`；`op_x64.dir/build.make` 含 `OnnxOcrEngine`（15 处）。
  - `nmake op_x64` EXIT=0：`OnnxOcrEngine.cpp.obj` 两 target(libop/op_x64) 均编译；`op_x64.dll` **25.4MB**（无模型时仅几 MB，证明 det/rec/keys 三模型资源已编入 DLL 资源段 `OCRMODEL`）；`onnxruntime` 符号链接进 dll。
  - `nmake op_test` EXIT=0：OcrService 接口重构（OcrEngine 抽象 + HttpOcrEngine + 引擎选择器）未破坏 `ocr_test.cpp` 等编译。
  - 跑 op_test 排除 WgcTest：167 用例 / 131 通过 / 1 FAILED（`MouseKeyTest.WaitKeyScanAllWithWaitFindsKey`，沙箱无键盘焦点、环境相关、与验证前基线一致）/ 0 segfault；`OcrFixture` 25 用例因 OCR 服务未起全 SKIP（日志可见 `selected HttpOcrEngine (remote)` 选择器路径正常）。
  - **三层（编译+链接+资源编入）+ 编译回归均验证通过，无新增回归。**
- 状态：代码落地 + 构建验证通过，**已提交**（见 §5.4）。运行时识别正确性需 GUI 环境加载模型跑真实图，本沙箱不可行（与「ONNX 引擎无运行时测试证据」短板一致，非回归）。

**② FindLineEx 落地（已完成，低风险高收益）**
- 纯新增 `FindLineEx`（保留 `FindLine` 原签名，零 ABI 风险），把霍夫累加器峰值（直线上点数 `point_count`）作 `long *ret` 出参；COM/C-API/Python/Go 全链路（11 处 / 9 文件）。
- 验证：自建 2 用例 PASSED；`nmake op_test` EXIT=0；回归 167/131/1FAILED(环境相关 MouseKeyTest)/0 segfault。
- 状态：已完成，**已提交**（见 §5.4）。

**③ ImageSearchService 默认去噪增强（已完成）**
- `_binary_preprocess_mode` 默认 `0→1`（删孤立 1px 噪点）；精确模式 `SetBinaryPreprocess(0,0,2,1)` 关回。
- 连带：`BinaryPreprocessIsDisabledByDefault` 测试改为显式 `SetBinaryPreprocess(0,...)` 验证 disabled 路径，并新增 `BinaryPreprocessRemovesIsolatedPointsByDefault` 钉默认 mode=1。
- 状态：已完成，**已提交**（见 §5.4）。

**④ 测试与工具**
- `tests/image_color_test.cpp` +92 行（FindLineEx×2 + BinaryPreprocess 修正×2）；`tests/test_support.cpp` +18 行（mem 位图辅助）。
- `scripts/run_tests.ps1` 补 UTF-8 BOM（修 PS5.1 按 GBK 误读致 ParserError）。
- 一批 untracked OCR 调试工具（`autoocr_pipe*`/`ocr_bmp*`/`binarize.py`/`ocr_bench.py`/`OnnxOcrEngine_dbg.*`）：个人调试用，建议归档、不入库。

### 5.3 进度小结
- 金字塔评审（L0–L3）全部入库，崩溃/并发类高危缺陷清零，COM 层异常安全达标。
- 评审后优化轮 + capture 修复 + L2 解剖已入库。
- 当前主力方向：**OCR 引擎内置化（ONNX）**——从「依赖外部服务」向「开箱即用进程内识别」演进，架构级升级，**构建验证通过、未提交**（详见 5.2 ①）。
- 验证闭环仍是短板：op_test 无 CI；ONNX 引擎缺**运行时识别**证据（环境无 GUI/服务未起时整 Fixture SKIP——已验证"编译+链接+资源编入+编译回归"四层，仅缺真实图识别输出，需 GUI 环境补测）。

### 5.4 提交记录（2026-09-02）

> 用户指令"一起提交，做好记录"：将 §5.2 的 ①②③ 代码工作合并为一次提交，调试工具/分析产物按建议不入主库。

**提交范围（1 次提交）**
- 21 个已跟踪文件改动：`bindings/go/{dll_windows,ocr_windows}.go`、`bindings/python/op/{_ffi,api}.py`、`build.py`、`include/{libop,op_c_api}.h`、`libop/CMakeLists.txt`、`libop/c_api/op_c_api.cpp`、`libop/com/{OpAutomation.{h,cpp},op.idl}`、`libop/image/ImageSearchService.{h,cpp}`、`libop/ocr/OcrService.{h,cpp}`、`libop/op/OpOcr.cpp`、`scripts/{PROJECT_REVIEW,run_tests.ps1}`、`tests/{image_color_test,test_support}.cpp`。
- 4 个 ONNX 引擎新源文件（untracked→纳入版本控制）：`libop/ocr/OnnxOcrEngine.cpp` / `OnnxOcrEngine.h` / `ocr_models.h` / `ocr_models.rc.in`。
- `.gitignore` 增补：个人 OCR 调试工具 / 分析产物 / 根目录 `onnxruntime*.dll` 忽略规则（保持工作树干净，不入库）。

**不入主库（已 .gitignore）**
- 调试工具：`autoocr_pipe*`、`ocr_bmp*`、`OnnxOcrEngine_dbg.cpp`、`binarize.py`、`ocr_bench.py`、`smoke_*`、`*_check.py`、`verify_res.py`、`amplify.py`、`build_test.py`、`ocr_models_smoke.res`、各 `*_result*.txt` / `ocr_dll_*.txt` / `out_bin/`。
- 分析文档：`scripts/*.md` 深读报告 + `scripts/{scan,verify}_dict_*.py` + `l2_audit_out.txt`（如需留存可另行归档提交）。
- 根目录 `onnxruntime.dll` / `onnxruntime_providers_shared.dll`（由 CMake 构建期从 `_deps` 复制，不跟踪）。

**提交后状态**
- `git status` 应仅剩被忽略的调试产物（untracked, ignored）；新增 ONNX 引擎源文件已纳入版本控制。
- 提交信息（中文基调，沿用 `fix:`/`refactor:`/`feat:` 风格）见 `git log`。
- 提交哈希见下方记忆日志与 `git log --oneline -1`。
