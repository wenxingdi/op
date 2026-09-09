# OP 插件更新日志（本地维护，不入库）

> 基线：上游 0.4.8.3（6d6b285，2026-07-07）。以下为本仓库自有迭代记录。
> 位置：`op/doc2/CHANGELOG.md`（doc2/ 已在 .gitignore，仅存本地）。

## [Unreleased] — 2026-09

### 2026-09-08（夜·WgcTest 崩溃 SEH 熔断闭环，026123a，2文件 +81/-10）

- **真机复跑（18:20）验证 P0 SEH 熔断生效**：EXIT_CODE=1（非崩溃码 -1073741819），WgcTest 全组 10 例**跑完不中断**。第 5 例 `NormalAutoUsesWgcForKnownBrowserClasses` 日志出现 `WgcCapture::Init SEH fault code=0xC0000005` → worker 正常收尾（init not ready）→ **FAIL 而非进程崩**；第 6-10 例 `BindEx: WGC broken by previous in-process SEH fault` 熔断快速失败。结果 4 PASSED / 6 FAILED 输出可读、根因明示。这是对 18362 WGC 系统组件崩溃（非我们代码 bug）的工程化兜底：**进程不亡、可判、可跑**，而非修复系统。
- 提交 026123a 含全套工作树改动：SEH 壳 `win10InitSehGuard`（__try/__except 规避 C2712）+ 进程级熔断 `s_win10WgcBroken`（BindEx/requestCapture 快速失败）+ A（applySessionOptions Borderless 轮询替代 .get() 无限阻塞）+ C（worker join 轮询+detach 兜底）+ 时序常量 300/200→2000 + Init 链 4 处阶段日志。
- C 组待办状态：窗口功能全序列前 4 例（静态/普通/DXGI 首帧/最大化裁剪）真机 PASS；后 6 例 FAIL 归因系统 WGC 崩溃 + 熔断连锁，**非功能缺陷**。若要全绿 → P1 进程隔离（验收脚本逐例独立跑 `--gtest_filter`），待用户拍板。
- **P1 进程隔离验证（18:31）——C 组完整闭环，10/10 全 PASS**：`doc2/run_wgc_isolated.bat`（逐例独立进程）10 例全过（exit=0，含第 5 例 NormalAutoUsesWgcForKnownBrowserClasses 与 6-10 例全部窗口场景）。**结论：WGC 功能实现零缺陷；崩溃纯属 18362 同进程背靠背会话系统 bug（kernelbase AV）；026123a 的 SEH 熔断兜底被证为正确且必要**（同进程跑保进程不亡，逐例跑证功能无损，两法互补）。真机验收 A/B/C 三组全部收官。

### 2026-09-08（晚·OCR 根因批，3c61c19，3文件 +66/-18）

- **AutoOcrLine 错字 + 耗时双根因坐实（代码级）**，真机 smoke B 组复盘：
  - 错字根因：`autoocr_line` 不做行定位、整区域直 rec，区域含上下留白被等比压扁（620x160 窗、文本行仅 35px → 字符缩到 ~10px 糊）→ '愿'→'原'、'树'重复、'-778'→'77'。det 版（autoocr_ex/ocr_from_file）先裁行所以全对，非遮挡、非截取差异。
  - 耗时根因：`SetIntraOpNumThreads(1)` 单线程推理（d10c796 首版即有，无演进理由）。67ms ≈ det~30 + rec~40（PP-OCRv4 单线程量级）；目标 9-13ms 是 rapidocr 多线程基准。
- 修复（2 文件）：
  - `OnnxOcrEngine.cpp`：线程参数化 `--threads=N`（缺省 min(4,核)；`--threads=0` 不设走 ORT 默认=全核），SessionOptions 设置移入 init（Impl 构造不再锁 1）。
  - `ImageSearchService.cpp` `autoocr_line`：二值图水平投影切行（行内空隙 ≤3px 合并、段高 <5px 滤噪）→ 每行独立直 rec → y 序拼接。跳过 det 的快路径语义保留，多行区域从"整窗压一行乱码"升级为逐行拼接。
- 沙箱验证：编译过（DLL 15:03）；排除 WGC 全量 **146 PASSED / 1 已知环境 FAIL（MouseKey）** 零回归。
- **真机复核（15:08）ALL PASSED 闭环**：`autoocr_line` '许原树材[320,77]' → **'许愿树[320,-778]12345'**（投影切行生效）；耗时 autoocr_line avg 49.8→**33.4ms**（含整窗截图）、ocr_from_file 67.3→**42.6ms**（det+rec 全流程）。仍高于 9-13ms 纯 rec 目标——剩余构成含截图/二值化/文件IO/小张量线程收益有限，留后续引擎热点核查（可选）。已提交 3c61c19。

### 2026-09-08（晚）

- **真机冒烟修复：GdiCapture 绑定后放大尺寸钳制（f0d689d，3文件 +85）**：真机冒烟 A 组暴露——绑定 320x200 后窗口放大到 920x720，整窗 Capture 产物仍为 320x200（diag 读文件头证实），新区域内 GetColor 越界返回 000000。
  - 根因：`RectConvert`（BindingSession.cpp:543）用 `get_width()/get_height()` 钳请求上限，且每帧先调 `_capture->refreshMetrics()`（:539）；但 **GdiCapture 未 override refreshMetrics**（基类空实现，WgcCapture 有同款），`_width/_height` 锁死绑定时刻值 → 放大后截取区域被钳到旧尺寸。6314cbd/b63fe74 只修了偏移与容量协商，未覆盖此上限刷新缺口。
  - 修复：`GdiCapture` override `refreshMetrics()`（GetClientRect 刷新 `_width/_height`，与 BindEx 初始化同款语义）。
  - 回归用例：`ImageColorTest.GdiCaptureRefreshesFrameSizeAfterEnlarge`（放大超绑定尺寸 → Capture 文件宽高==新客户区 + 新区域 GetColor 非黑）——现有 `GdiCaptureRefreshesClientOffsetAfterResize` 只取色 (60,60) 测不到钳制上限。
  - 验证：新用例沙箱 PASS（区分性，修复前必 FAIL）；排除 WGC 全量 **146 PASSED / 1 已知环境 FAIL（MouseKey）** 零回归。冒烟脚本同步改（root topmost+lift 防 BitBlt 屏幕 DC 遮挡截错；B 组补 ocr_from_file 纯 rec 计时对照）。
  - **真机闭环（2026-09-08 14:53）**：smoke_real.py A 组 normal/gdi 均 PASS（中心/右下角 FFFFFF），B 组 OCR PASS（autoocr_line 49.8ms 含整窗截图；ocr_from_file 纯 rec 67.3ms 仍高于 9-13ms 目标——留待引擎侧优化核查）。已提交 f0d689d。遗留观察：autoocr_line 绑定路径错字 '许原树材[320,77]' vs 文件版 '许愿树[320,-778]12345'，topmost 后仍现 → 疑绑定路径 BitBlt 截到内容与文件版不一致，后续核查（下节已坐实为不裁行所致）。

### 2026-09-08（下午）

- **全盘核查修复批次（6314cbd，12文件 +187/-46）**：4 路子代理分模块只读深扫 + 关键项人工复核（报告 `doc2/第二轮_全盘核查报告.md`）。
  - P0-1 **GdiCapture**：PrintWindow/normal 分支写共享段前 `ensureSharedFrameCapacity` 容量协商（镜像 HookCapture），窗口 resize 放大不再越界写穿共享段；恢复 `_pmutex` 帧锁；FrameInfo 头尺寸与整窗载荷一致。
  - P0-2 **D3D12Capture**：废弃 `commandQueueOffset_` 野指针取 queue（swapchain vtable 当 ID3D12CommandQueue 调 ExecuteCommandLists，D3D12 游戏必崩）；改 `device->CreateCommandQueue` 自建拷贝队列 + `ID3D12Fence` 围栏同步（Execute 后 Signal / 下帧 Wait），command allocator 每帧 Reset。
  - P0-3 `str2colordfs`/`str2colors`：颜色串空段（前导/连续 `|`）跳过，防空 vector 取下标 UB（CmpColor/FindColor 共用解析器）。
  - P0-4 `WindowService::FindWindowByProcess`：`titles = titles;` 自赋值→`titles = title;`（两处），子窗口搜索恢复标题过滤。
  - P0-5 `WinMouse`/`DxMouse::GetCursorPos`：POINT 初始化为 {0,0}，GetCursorPos/ScreenToClient 失败不再回垃圾栈值。
  - P1-6 FindWindow/FindWindowEx 入参 NULL 判空；FindWindowByProcess `wcsstr` 先判空再传（needle 不可为 NULL）。
  - P1-7 GetWindowState 语义对齐大漠：flag1（激活）`GetActiveWindow`→`GetForegroundWindow`（原跨进程恒 false）；flag5（置顶）→ `WS_EX_TOPMOST` 判定。
  - P1-12 `Image::read(内存路径)` 补 `IsSupportedImageFormat` 校验：拒绝 16bpp 等不支持格式，不再返回未初始化内存。
  - P1-13 FindColorEx/FindMultiColorEx/AutoOcrEx 结果上限 off-by-one（原 1800 上限实返 1801）：先判 `find_ct>=max` 再收集，集满即停。
  - E3 AutoOcrEx 输出按阅读序稳定排序（上→下、同行左→右）。
  - E4 YOLO `--labels=@file`：支持从 UTF-8 文件按行读类别（ultralytics classes.txt 格式）。
  - 验证：编译通过；回归 145 PASSED / 71 SKIP / 1 已知环境 FAIL（MouseKey），与基线一致无回归。
  - 复核排除误报：ProcessMemory FindData 无句柄泄漏（break 路径均汇聚 CloseHandle）。

### 2026-09-08

- **YOLO 内置化（70170dc）**：引擎抽象 `YoloEngine` + `HttpYoloEngine`（原 HTTP 平移）+ `OnnxYoloEngine`（pimpl，进程内 ONNX 推理）。`YoloDetector` 退化为引擎选择器，对外 `SetYoloEngine/YoloDetect` 接口不变。
  - **版本自适应**：按输出张量形状自动识别，无需版本参数——v5 `[1,3*8400,5+nc]`（obj×cls）/ v8-v11 `[1,4+nc,8400]` 通道在前 / `[1,8400,4+nc]` 转置；anchor 数由模型输入尺寸推得，nc 从形状推导。
  - 预处理 letterbox（灰114）+ /255 + RGB CHW；类别感知 NMS；argv：`--conf= / --iou= / --labels=`（UTF-8 逗号分隔）。
  - **方案B**：默认不内置模型（DLL 零增量）。`("onnx","",...)` 走资源段（构建期检测 `build/_deps/yolo_models/yolo.onnx` 存在才编入）；`("onnx","path.onnx",...)` 外挂任意自训模型。
  - 验证：Yolo 用例 8/8；全量回归 145 PASSED。

- **OCR 阶段2/3（6fcd028）**：`AutoOcrLine`（COM id265，单行整图直 rec 跳 det 快路径，bbox=整图）+ `AutoOcrEx`（id266，输出 `x1,y1,x2,y2,conf,text|...` 屏幕绝对坐标，返回行数）。全链路接线：COM/C-API/Python/Go。

### 2026-09-04

- **第二轮金字塔 L1+L2（6ea3ff3，24文件 +864）**：
  - L1 内存搜索：`FindData` / `FindDataEx` / `GetModuleBaseAddr`（COM id260-262）。特征码 `??` 通配、16MB 分块、首非通配字节 memchr 锚点、uintptr_t 全程。
  - L2 图色修正：`FindColorBlockExS`（id263，mode=1 并查集聚类重合窗口）+ `FindLineExS`（id264，Hough 直线 min_points 阈值根治幻觉线 + 孤立点去噪）。
  - 核验撤销两项：FindPicMT（FindPic 内部已是多线程默认路径）、DllInjector 超时（8e4bdca 已修）。

### 2026-09-03

- **OCR 解码期字符白名单（7de779d）**：`SetOcrEngine` argv 支持 `--charset=@zh0123456789[],-+`（`@zh`=字库内全部中文 U+4E00-9FFF）。ctc_greedy 加 mask：白名单外类别屏蔽、blank 恒允许、logits 子集 softmax 重归一；模型 6625 类 vs keys 6623 错位兼容。

### 2026-09-02

- **OCR 引擎内置化（d10c796）**：抽象 `OcrEngine` + `OnnxOcrEngine`（进程内默认，PP-OCRv4 det/rec 编入 DLL 资源段）+ `HttpOcrEngine`（远程兜底）；`HttpOcrService` 退化为引擎选择器。附带 `FindLineEx` 接线与默认去噪增强（`_binary_preprocess_mode` 默认删孤立单点，`SetBinaryPreprocess(0,0,2,1)` 关回）。
- DLL 体积 25.4MB（ONNX Runtime 共享链接 +2 DLL 随插件分发）。

## [0.4.8.3-p1] — 2026-08 金字塔评审修复轮

### 2026-08-05

- **b63fe74**：GdiCapture 每帧重算客户区偏移，消除窗口 resize 后 FindPic 错位。
- **9f197e5**：L2 逐功能解剖报告 + dx 输入三通道开关。

### 2026-08-04

- **8e4bdca**：P1-P3 优化（CPU 采样基准/注入超时资源释放/SWIG 异常/验证闭环/构建环境）。
- **6ffae1b**：L3 COM 接口层——Invoke try/catch（/EHa）+ 13 方法 NULL 检查 + 41 方法 CopyOutBstr。
- **8bec55e**：L2 修复 `DisplayHook::is_capture` 跨线程数据竞争（atomic）。
- **8e208c4**：L1 建议修 A/B/F（DllInjector + ProcessMemory）。
- **56f35d9**：L0/L1 缺陷修复 + minhook 静态链接（去 minhook DLL 依赖）。

## 测试基线

- op_test 当前（2026-09-09 P1/P2 后实测，`--gtest_filter=-WgcTest.*`）：**182 用例，146 PASSED / 1 已知环境 FAIL（MouseKeyTest.WaitKey）/ 余为 SKIP，零回归**。WgcTest 需真机（沙箱无 WGC worker 会 segfault，非回归；真机隔离跑 `doc2/run_wgc_isolated.bat` 预期 10/10）。
- 运行方式：`scripts/run_tests.ps1`，或 Git Bash：`PATH+="build/nmake-x64-Release/libop" ./tests/op_test.exe`。

## 待办

- [x] 真机验收（验收包已就绪：`doc2/真机验收清单.md` + `doc2/smoke_real.py`，2026-09-08 产出）：
  - [x] A 组 P0-1 GDI/normal 放大超容 Capture（脚本一键，normal+gdi）—— **2026-09-08 14:53 真机 PASS，闭环 f0d689d**
  - [x] B 组 OCR 三件套：AutoOcrLine/AutoOcrEx/--charset + 耗时 —— **3c61c19 修复后复核 PASS**：错字消除（'许愿树[320,-778]12345'）；autoocr_line 33.4ms、ocr_from_file 42.6ms（仍高于 9-13ms 纯 rec 目标，剩余为截图/二值化/文件IO/小张量开销，可选后续核查）
  - [x] C 组 WgcTest 真机窗口组（最大化/最小化/resize 全序列）—— **026123a SEH 兜底 + P1 进程隔离双闭环**：同进程全组跑完不崩（前 4 PASS，后 6=系统崩溃+熔断连锁）；`run_wgc_isolated.bat` 逐例独立进程 **10/10 全 PASS** → WGC 功能零缺陷，崩因纯属 18362 背靠背会话系统 bug
  - [x] D 组 P0-2 D3D12 hook 截图 —— **2026-09-08 19:53 真机闭环（自建 d3d12_target.exe 载体 + 注入依赖治本 95fc42e）**：渲染 hook 链路**首次端到端跑通**。首个盲点=**注入依赖 0xC0000135**：HookCapture 注入模块按宿主同构选（c_api 宿主→op_c_api_x64.dll），其非系统依赖 onnxruntime.dll 在 libop 目录（m_opPath），目标进程 LoadLibrary 标准搜索序找不到 → 注入失败且 setlog 默认静默不可见（须 `set_show_error_msg(2)` 开 `__op.log`）。**治本 95fc42e**：注入前 MakeRemoteFunction 远端 SetDllDirectoryW(m_opPath)、Inject 后无条件恢复（RPC 执行成功但取结果抛异常被 catch 误报 false——实测目录已生效 Inject 成功，故恢复不依赖返回值）；BindEx/BindNox 双路径。冒烟层 cwd 规避已还原，**不带规避 ALL PASSED**（bind=True、三色 distinct=3、Capture 像素 PASS）。待查噪音：①unbind 报 OpUnBindWindow failed（无 setlog 不影响功能）；②SetDllDirectoryW RPC 取结果抛 unknown exception（降级无碍，根因未明）。DPI 注意：python 非 DPI aware，get_client_size 报虚拟坐标，断言用固定客户区。
  - [ ] E 组 YOLO 自训模型 v8/v11 转置路径 + --labels=@file —— **2026-09-08 挂起**：用户暂无自训模型，且外网全断（github/hf/pypi 全 000）无法取官方预训练 onnx 顶替。机制层代码已就绪（decode ch_first 转置自适应 + `--labels=@file` 按行读取，见 OnnxYoloEngine.cpp:269/355），成功路径未闭环。恢复条件：任一合法 yolo 检测 onnx（v8/v11 各一更佳）到位后，`smoke_real.py --yolo-model/--yolo-image/--yolo-labels` 三参重跑即验，无需改码。
- [x] 遗留观察：autoocr_line 绑定路径错字（'许原树材[320,77]'）vs ocr_from_file 全对 —— **已坐实**：非截取差异，autoocr_line 不裁行整窗直 rec 压扁字符所致；3c61c19 投影切行修复后真机复核正确。
- [x] L2 暂缓项拍板（2026-09-08）：① Capture 未绑定回退桌面截图——**实测已存在**（check_bind 自动 BindWindow(桌面)，137e948 起；probe 证未绑定 get_color 返真实桌面像素/capture 307KB），核查报告"未绑定空返回"不成立；残留风险=忘绑定静默打桌面，本次仅补 check_bind setlog 诊断（E5，用户批"维持+日志"）。② 去噪全局扩展——**维持分界不动**（E6，用户批"维持"）：色块三件套保 1px 语义、旧 FindLine 保原语义；找字/OCR 域已默认 mode=1 去噪；autoocr 系旁路属刻意，未接入。E5 改动已提交 **37fab58**。
- [x] **P1 竞态类六项（第二轮核查遗留，2026-09-09 三批闭环）**：批1 **a35eb3e**（P1-14 TemplateMatcher static ORB → thread_local 线程隔离；P1-10 InputHookClient UnBind 先远端释放后清引用+RPC 异常保留可重试）；批2 **af037b2**（P1-8 InputHook 状态镜像 m_mouseState/m_keyboardState/m_vkState/m_wheelDelta 新增 g_stateMutex 消除无锁 RMW——fill_mouse_state/consumeWheelDelta 读清原子化，m_inputLock/m_dxAttrs 转 atomic；P1-9 hook_dinput 全失败不再谎报成功，setup 中止输入绑定）；批3 **1be3057**（P1-11 Pipe close 对 reader 阻塞 ReadFile 调 CancelSynchronousIo，防子进程退出后孙进程持写端致 join 无限挂起；P1-15 WgcCapture 尺寸读写经 sharedResourceMutex_ 快照/写入 + getFrameInfo 覆写持资源锁——核查确认 GDI/Hook 后端 ensure 删重建全程持 _pmutex 无同病，仅 Wgc 独病）。每批 nmake+op_test(-Wgc) 零回归（仅已知 MouseKey 环境失败）。**注意：WgcCapture 收帧/尺寸路径改动需真机复核**——C 组 `run_wgc_isolated.bat` 重跑 10/10 预期不变，resize 场景留意。
- [x] **P2 死代码清理（2026-09-09，提交 1bf2c89，10 文件 -403 行）**：删 TesseractOcr.{h,cpp}（从未编入 CMake 源列表）、ImageView.h（class ImageView 零引用，撞名 RawImageView 独立保留）、ImagePreprocessor.h（空壳转发头）4 个死文件；**#18 报告误判修正**：WindowState.cpp 实为 WindowService 的 9 个方法实现（8 个有 COM API 调用链），不能整文件删——只删唯一死的 `GetTopWindowSp`（0 调用，唯一引用 GdiCapture.cpp:107 注释代码；含 GetWindowLongA 不更新 i 的死循环缺陷，删定义+WindowService.h:45 声明）；WinKeyboard.cpp 删 file-static `oem_code`（8-33 含函数内局部码表，0 调用）；ImageSearchAlgorithms 删 `gen_next`（.h:47+.cpp:188）/`Connectivity`/`extractConnectivity`（.h:56+.cpp:263）及连带 `flood_connectivity`（仅被二者调用）。残留仅注释（GdiCapture.cpp:107、KeyboardBackend.cpp:6）与 l2_audit_out.txt 快照，非源码。op.rc TODO 占位**维持现状**（用户拍板，非死代码，UTF-16 留待填真实元数据）。每批 nmake+op_test(-Wgc) 零回归（182 用例 146 PASS 仅已知 MouseKey 环境失败）。
- [x] **OCR 耗时核查（2026-09-09，纯核查未改码，报告 doc2/OCR_性能核查.md + 剖析脚本 ocr_profile{,_2,_3}.py）**：沙箱复现 ocr_from_file 冷净 49ms（真机 42.6 同量级）、单行 1 次 rec；**同配置跨进程 49 vs 130-150ms 3 倍波动**——经冷却 60s 复测排除降频，归因共存负载（雷电 HD-Player 常驻/SearchIndexer/i5-13400 P-E 核调度），9-13ms 目标本机日常环境不可达不可复现。det 缩放实验（张量 160×608→32×160）仅省 26ms（20%）→ det 非绝对大头；autoocr_line 免 det 仍 33ms → rec 单行本机或 ≥20ms（未复现注释"4线程 10-15ms"）。threads 4 vs 1 省 46ms。**瓶颈归属无法外部定论（C-API 无 rec-only 文件入口），需引擎内 QPC 分段计时（已列建议，待拍板）。**
- [x] **pyop vs op 双绑定澄清 + op-capi 可安装化（2026-09-09，提交 6dd9e57）**：初判"pyop 是死代码"被推翻——`python/pyop/` 是 **SWIG 4.4.1 生成绑定**（依赖 `_pyop` C 扩展，CMake build_swig_py=ON 编译，根 pyproject 为 scikit-build-core 编译通道），`bindings/python/op/` 是 **ctypes 新绑定**（纯 .py 零编译，smoke/剖析实际使用）。**进一步核查推翻"改根 pyproject wheel.packages 指向 ctypes"方案**：`bindings/python/` 本就是独立发布单元（自带 pyproject.toml，name=op-capi，纯 setuptools，README 明写与 SWIG 相互独立）；`_ffi._candidate_paths()` 候选链第 3 位即包内 `op/bin/x64/`，架构完备仅 DLL 缺位。**落地**：DLL 预置 `bindings/python/op/bin/x64/`（op_c_api_x64.dll 26.6MB + onnxruntime.dll 11.2MB + providers_shared），.gitignore 增 `bindings/python/op/bin/` 防 37.9MB 入库；验证=仓库外 cwd 无参 `Op()` 构造成功自动命中包内 DLL（get_last_error=0）→ `pip install ./bindings/python` 开箱可用。**遗留**：SWIG 通道（根 pyproject+swig/+python/pyop/）**保留作编译型备选**（2026-09-09 用户拍板，不退役不删，需要编译型 wheel 或 C 扩展场景时仍可用）。
- [~] **push：暂缓**（2026-09-09 收尾时共 24 个提交，56f35d9 起最新见 `git log`）。2026-09-08/09 复测 github/gitee/gitcode/hf/pypi 全不可达（白名单式封锁）——2026-09-09 用户定调：先不 push、不做 bundle 备份，变更以本地记录为准（git 提交链 + 本 CHANGELOG + 项目 memory）。网络恢复后可推时无需特殊准备（提交链完整）。
