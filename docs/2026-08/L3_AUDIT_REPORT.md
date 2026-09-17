# L3（对外接口层）体检报告

> 范围：公共 API 门面（`include/libop.h` 564 个 API + `libop/op/` 门面实现）、COM 自动化接口（`libop/com/OpAutomation.cpp/.h`，给 VBScript/按键精灵/易语言用）、Python SWIG 绑定（`swig/op.i`）。
> 评审方法：静态通读 + 缺陷特征扫描；**先发现问题 → 与用户确认风险等级 → 用户授权后才改 → 改完编译验证**（按用户约定流程）。

---

## 一、发现的问题与风险等级（已与用户确认）

### 🔴 必修（崩溃类，已修）

**① COM 层完全没有异常防护**
- 位置：`OpAutomation.cpp`（~1647 行，~160 个 `STDMETHODIMP` 方法）
- 现象：全文件 grep `try/catch/__try/__except` — **0 命中**。每个方法内部直接调 C++ 库（`obj.xxx()`），一旦 C++ 库抛 `std::exception`（OpenCV / 内存分配 / 字符串转换），异常**直接逃逸 COM 边界**，导致脚本宿主（按键精灵/VBScript/易语言）崩溃，而非返回错误码。
- 风险：🔴 高（宿主进程无理由崩溃，最难排查）

**② COM 出参指针未做 NULL 检查即解引用**
- 位置：13 个方法（ClientToScreen / GetClientRect / GetClientSize / GetWindowRect / ScreenToClient / GetCursorPos / FindColor / FindPic / FindColorBlock / GetPicSize / GetScreenDataBmp / GetScreenFrameInfo / FindStr）
- 现象：直接用 `x->vt = ...` / `obj.X(x, &x->lVal, ...)` 解引用调用方传入的出参 `VARIANT* / LONG*`，调用方传 NULL → **Access Violation 崩溃**。作者已在 `FindMultiColor` / `GetWordResultPos` 示范 `if(!x) return E_POINTER` 规范写法，但漏了一大片。
- 风险：🔴 高（空指针即崩，脚本常见误用）

### 🟡 建议修（已修）

**③ BSTR* 出参所有权转移不严谨 + 忽略 HRESULT**
- 位置：41 个 BSTR 出参方法（Ver / GetPath / EnumWindow / FindNearestPos / GetCmdStr / GetClipboard / FindPicEx / FindColorBlockEx …）
- 现象：手写 `CComBSTR x; x.Append(s.data()); x.CopyTo(ret); return S_OK;` —— 两个问题：
  - `ret` 为 NULL 时 `CComBSTR::CopyTo(NULL)` **崩溃**；
  - 忽略 `CopyTo` 返回的 HRESULT，失败仍 `return S_OK`（错误被吞）。
- 修复：统一改用代码里**已存在**的 `CopyOutBstr(target, const std::wstring&)` helper（自带 NULL 检查 + 返回真实 hr）。`FindNearestPos` / `EnumWindow` 原以 `return hr;` 结尾，重构后死代码 `return hr;` 一并清除。
- 风险：🟡 中（NULL 崩溃 + 错误被吞，但影响面小于①②）

---

## 二、改动清单（用户授权：①②③ + 改动小方案）

| 项 | 方案 | 改动 |
|---|---|---|
| A | 在 `IDispatchImpl::Invoke` 覆写处集中 `try/catch`（1 处，罩住全部 COM 方法） | `OpAutomation.h` 加 `Invoke` override 声明；`OpAutomation.cpp` 加集中异常防护实现（catch `std::exception` 与 `...`，填 `EXCEPINFO` 并 `return E_FAIL`） |
| B | 13 个方法的出参指针加 `if(!x) return E_POINTER` | `OpAutomation.cpp` 13 处 |
| C | 41 个 BSTR 出参方法改用 `CopyOutBstr` | `OpAutomation.cpp` 41 处（脚本化批量重构，见 `scripts/l3_bstr_refactor.py`） |

**方案选型说明**：② 的 NULL 解引用在 Windows 上是 Access Violation（SEH），C++ `try/catch` 抓不到，故②必须显式 `if(!x) return E_POINTER`（仿现有 `FindMultiColor` 规范），不能依赖 Invoke 层兜底。集中式（A/C 复用现有 helper）相比逐方法包 try/catch 改动更小、覆盖更全、可审查性更高——用户确认“改动小方案风险更低”。

---

## 三、编译验证（用户要求：改完必须编译）

- 环境：MSVC 14.44.35207 + WinSDK 10.0.26100.0（`build/nmake-x64-Release` 增量 nmake）
- 注意：`OpAutomation.cpp` 含 ATL，需把 `atlmfc/include` 与 `atlmfc/lib/x64` 加入 INCLUDE/LIB（L2 时只重编 `DisplayHook.cpp` 未触发此依赖，本次才暴露）。
- 结果：**`nmake op_x64` EXIT=0，`op_x64.dll` 干净构建通过。**
- 附加发现：构建日志 `D9025` 显示实际生效的是 **`/EHa`**（捕获 C++ 与 SEH/AV），故 A 的 `try/catch` 还能顺带兜住 NULL 解引用导致的访问违例，鲁棒性优于预期。
- 运行时回归说明：`op_test.exe` 为 Windows 子系统程序，Bash 管道捕获不到 stdout（L1 时就此情况）。本次三项改动均为**行为保持**的安全增强（崩溃→返回错误码 / AV→E_POINTER / 吞错误→返回真实 hr），无语义回退；建议在 CI 或控制台跑全套 `op_test` 复核。

---

## 四、后续建议（未纳入本次，待确认）

- ④ 全局单例跨实例共享：OCR/YOLO/模板缓存为 `static` 单例（L2 已确认有内部 mutex，但并发脚本下模板名冲突等仍可能干扰）。
- ⑤ Python SWIG（`swig/op.i`）：缺 `%exception`（C++ 异常→Python 异常）与 `%newobject`（返回指针内存释放），需读实证后补。
- ⑥ C 类重构脚本可入库为后续巡检工具（已存 `scripts/l3_bstr_refactor.py`）。
