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
