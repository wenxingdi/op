# OP 插件更新日志（本地维护，不入库）

> 基线：上游 0.4.8.3（6d6b285，2026-07-07）。以下为本仓库自有迭代记录。
> 位置：`op/doc2/CHANGELOG.md`（doc2/ 已在 .gitignore，仅存本地）。

### 2026-09-18（YOLO 方案B 落地：类别名自动识别 + 修复 ONNX detect 恒失败的 ok 门禁 bug）

- **类别名自动识别（方案B 第 1 步）**：`OnnxYoloEngine::load` 成功创建会话后，自动读模型 metadata 的 `names` 键（ORT 1.19 `LookupCustomMetadataMapAllocated`），解析 ultralytics 嵌入的类别名字典（`{0: 'a', 1: 'b'}` / `{"0": "a"}` 两种 repr 兼容，UTF-8，索引空洞留空，≥4096 索引防御）填充标签表。显式 `--labels` 优先；无 metadata（旧模型/其他框架导出）保持空，JSON 只给 `class_id`。
- **🔴 顺手修复潜伏 bug**：`Impl::ok` 从未被置 true，`detect()` 门禁 `if (!m_impl->ok) return -1` 导致 **ONNX 引擎加载成功后 detect 恒返 -1**（静默 ret=0）——自 ONNX 引擎引入以来该路径从未真正跑通过，旧测试全走失败分支所以零暴露。修复 = `load()` 成功路径 `ok = true`。
- **端到端测试资产**：`scripts/gen_yolo_meta_test_model.py`（onnx+numpy 托管 venv 生成）产出 `tests/testdata/yolo_meta_test.onnx`——单 Constant 节点输出固定 [1,84,8400] v8 布局（全 anchor 类别 1 分数 0.99），metadata 嵌 `names`。新用例 `YoloOnnxTest.EndToEndAutoLoadsLabelsFromModelMetadata`：统一入口加载（无 --labels）→ detect → 断言 `class_id=1` 且 `label="target_cls"`。**YoloTest 10/10 PASS**。
- 过程自纠：`sregex_iterator` 是 string 迭代器 typedef，直接喂 `const char*` 编译失败 → 先包 `std::string` 再迭代（独立小程序验证 8 组解析场景全过后才进主代码）。
- 同步：`libop.h` 注释 · `doc/yolo.md`（detect 按标签过滤 Python 一行示例）· API 手册注解（899/899 覆盖保持）。
- **基线**：**264 用例 · 232 PASS · 31 SKIP · 1 FAILED**（唯一 FAILED=WaitKeyScanAll 已知时序环境项），全量 1m26s 零回归。

### 2026-09-17（YOLO 统一入口：SetYoloEngine 直接吃 .onnx 模型路径，零接口变化）

- **统一入口**：`SetYoloEngine("D:/xx/best.onnx", "", "--labels=...")` 一步到位——第一参数以 `.onnx` 结尾（大小写不敏感、兼容正反斜杠）且 `dll_name` 为空时，自动切进程内 ONNX 引擎并以该路径为模型文件，等价于旧写法 `SetYoloEngine("onnx", 模型路径, ...)`。旧写法/HTTP 模式（URL、yolo/yolo_http 别名）完全不受影响；`dll_name` 非空时保持旧语义（优先作模型路径）。
- 实现：`OpYolo.cpp` SetYoloEngine 入口做扩展名识别与参数改写（+`has_onnx_model_extension` 助手），YoloDetector/引擎层不动。
- 同步：`libop.h` 函数注释、`doc/yolo.md`（改为"统一入口推荐 + HTTP 模式"两节）、API 手册 SetYoloEngine 注解（`scripts/gen_api_reference.py`）。
- 测试：新增 `YoloOnnxTest.UnifiedEntryAcceptsOnnxModelPathDirectly`——缺失路径/大写扩展名/乱码模型均优雅失败 ret=0，且 dll_name 非空时旧语义不被改写。YoloTest 9/9 PASS。
- 接口面：COM 223 / C-API 227 / Python 225 三层零变化（api_surface_diff 校验通过）。

### 2026-09-17（opencv 模块排查闭环：CV1-CV4 + OpenCV 测试链修复）

- **排查结论**：无 P0。线程安全设计扎实（模板缓存 shared_mutex 读写分离 / ORB thread_local / 线程池 magic static），六条匹配路径（直搜/金字塔/条带/缩放/特征/边缘/形状）架构清晰，测试素材覆盖真实照片。发现 4 项落地：
- **CV1（P1 性能）**：`collectRegionThresholdMatches` 逐像素全扫描 → 复用峰值抑制版 `collectPeakCandidates`，低阈值下不再产生海量相邻命中点（后续 suppressOverlapping 反正要合并）。**首版引入两个 bug 均已修**：①`reserve(SIZE_MAX)` 触发 vector too long——预分配改有界 `min(max, 4096)`；②见 CV3。
- **CV2（P1 内存）**：`TemplateEntry::scaled_templates` 无界增长（自动缩放模式试 10 档、每档 color+gray+mask 三份像素常驻）→ 每模板上限 16 档，超限整体清空重填。
- **CV3（P1 性能）**：gray 模式三入口（MatchTemplate/MatchAnyTemplate/MatchAllTemplates）原先整图 toGray 再搜 ROI → 先裁 ROI 再转灰度。**坐标系陷阱**：裁剪后结果坐标是 ROI 局部坐标，出函数前须加回 origin 偏移（8 个出口点逐一核对，金字塔/直搜/条带/并行/merge 全覆盖）；color 模式 origin=0 天然无影响。
- **CV4（P2 注释）**：MatchAnyTemplate 头注释承诺"模板之间并行"与实现不符（非条带是串行金字塔预筛+必要时并行回退），已改写。
- **测试链修复（本次关键发现）**：tests/CMakeLists.txt 硬编码 `x64/vc18/staticlib`，本机 OpenCV 安装是 vc17 → **OpenCvTest 26 用例自 a6fbe45 起从未编进 op_test**，此前基线 245 从未包含它们。修法：工具集目录 vc18→vc17 自动探测。重新编入后 22/22 PASS（含新增 MatchTemplateScaleEvictsStaleCache：单调用传 21+1 档 scale 触发逐出后仍命中）。
- **基线**：全量（-WgcTest.*）**262 用例 · 230 PASS · 31 SKIP · 1 FAILED**（唯一 FAILED=MouseKeyTest.WaitKeyScanAllWithWaitFindsKey，已知 WaitKey 家族时序环境项；5 个 OpenCvTest 真实素材用例因素材缺失 SKIP 与 OCR 一致）。上一基线 245/218/26/1 → 差值 +17（OpenCvTest 新编入 22 − 素材 SKIP 5）。零 API 变化。

### 2026-09-17（API 参考手册参数注解层补齐：全函数 100% 覆盖）

- **背景**：上一版（`1a86b71`）注解只覆盖 37 个函数的 63 个参数格，用户要求全部函数加上。
- **生成器改动**（`scripts/gen_api_reference.py`，仅文档生成层，零 API/代码变化）：
  - 新增 `GLOBAL_PARAM_DOCS`（28 个通用参数名兜底：ret/坐标/sim/dir/color/hwnd/file_name/address/conf/iou 等），渲染时"函数级 → 参数名兜底"两级查找。
  - `PARAM_DOCS` 从 37 个函数扩到 187 个（223 接口中无参或全兜底函数不重复列），覆盖全部 10 服务组；注解数据以源码为准逐项核对（EnumWindow filter/WindowState flag/DX_ATTR 位掩码/OpenCV 字符串模式 erode|dilate|zhang_suen|gaussian|binary|otsu|bgr|hsv 等/Memory type 0-6/FindPicEx 序号 vs FindPicExS 文件名/SetBinaryPreprocess 四参数/Wheel ±120）。
  - 修正旧版错误参数名：SetDict/UseDict 的 `index`→`idx`；SetMouseDelay/SetKeypadDelay 的 `t`→`type`+`delay`；WaitKey 的 `key_code`→`vk_code`+`time_out`。
  - main 输出注解覆盖率统计（本次 899/899=100%）。
- **验证**：生成 `docs/api_reference.html`（223 接口/292KB），AST 查重零重复键，参数覆盖脚本断言 missing=[]。
- 注意：个别描述性条目（如 CvLoadTemplateList 整体失败行为、GetScreenFrameInfo）以源码实现为准；接口面变更后重跑本脚本即可。

### 2026-09-17（ipc 模块排查闭环：IP1-IP4，纯卫生零行为变化）

- **排查结论**：无 P0/P1。重点设计项核查均健康——多开按 `op_*_<hwnd>` 命名隔离不串数据；宿主崩溃残锁靠 `FrameInfo` 校验和 + hwnd/宽高三重校验兜底（撕裂帧过不了校验和即判"无帧"）；Pipe reader 阻塞 ReadFile + `CancelSynchronousIo` 防孙进程继承句柄致 join 挂死；CommandRunner 超时 `terminate_process_tree` 杀整棵进程树不留孤儿。
- **IP1**：`Pipe.cpp` 删除重复 SAFE_DELETE（死代码）。
- **IP2**：`Pipe.h` 删除拷贝构造/赋值（持有裸句柄+线程指针，防双重 close/join）。
- **IP3**：`ProcessMutex::unlock()` assert 挪到空值守卫之后——op_test 构建（无 NDEBUG，assert 生效）下"unlock 未 open 的锁"从测试中止变安全返回；`was_abandoned()` 补注释说明读取侧靠校验和兜底故不逐个查询。
- **IP4**：删除 `SharedMemory::at<T>` 死代码（全库零调用，实现按字节索引强转怪异；`data<T>` 保留）。
- **遗留记录**：Pipe 基类本身全库零调用（仅 CommandRunner 派生用），上游大漠遗留"连接外部识别程序"设计，本仓库已内置 OCR/YOLO——保留不动，未来可考虑整体移除。

### 2026-09-17（image 模块排查闭环：I1-I3）

- **I1（P1）异常穿出 COM 边界**：`Image::create` 尺寸非法/溢出/realloc 失败时 `throw(const char*)`、`ImageBin::create` 抛 `bad_alloc`，全库仅 OpImage 一处局部 catch，截图链与直调入口均无兜底 → 异常穿出 COM 方法 = 宿主进程 terminate。修法：①`OpCaptureHelpers.h` 新增 `guard_exceptions`（catch std::exception/const char*/...，setlog 后吞掉，出参保持调用方已初始化值）；②`capture_region` 整体包裹（覆盖全部截图链入口）；③直调图像入口逐个包裹——OpImage 的 LoadPic/LoadMemPic/GetPicSize/MatchPicName、OpOcr 的 OcrFromFile/AutoOcrFromFile/OcrAutoFromFile（经核查字典解析无 throw，SetDict 系不用包）。
- **I2（P1）FindPic 系模板缺失静默**：`files2mats` 路径解析失败/解码失败/建模板失败三处 continue，调用方无法区分 ret=-1 是"没找到"还是"模板根本没加载"。三处补 setlog（含解析后全路径）。注意 .mem 前缀（LoadMemPic 名）路径解析失败也走第一条日志，属正常语义。
- **I3（P1）parse_multi_color_args 偏移未初始化**：`pt_cr_df_t tp;` 的 x/y 未初始化，sscanf 失败时垃圾偏移入 vector → 同一串输入不同调用间行为不确定。修法：tp 清零 + 校验 sscanf==2 + 畸形段跳过（旧实现对"有偏移无颜色"段是 break，会吞掉后续合法段，一并改 skip）。
- **文档化不改**（I4）：sim 越界语义各家族不一致——找色系 sim<0 钳 1.0（变精确匹配）、OCR 系回退 0.7、找图系内部 `0.5+sim/2`。已写入 libop.h image 区注释。
- **文档化不改**（I5）：`get_rois(1参)` 用 `width >= min_word_h` 比宽度对高度阈值（窄字跳过 cut 带留白），3 参重载正确分 w/h。够用原则，修则 OCR 切字行为变化需重做判别力验证。
- **记录不排期**（I6）：`record_sum/region_sum` int 累加，5K+ 屏幕灰度和溢出 21.4 亿 → FindPic quick-check 误判。当前 4K 内安全。
- **测试**：image_color_test.cpp +3——FindMultiColorSkipsMalformedOffsetSegments（畸形段跳过且结果确定，setlog 实测可见）、FindPicWithMissingTemplateDoesNotMatch、DirectImageEntriesFailSoft。零 API 变化。

### 2026-09-17（LayoutWindows 加固 + 层叠布局）

- **层叠布局（layout_type=2，Cascade）**：第 i 个窗口左上角 = 起点 + i×(gap_x, gap_y)，尺寸取各自值；gap 语义转为层叠步距（经典值 32，标题栏逐层露出便于点击切换）。`Type` 枚举 + `Calculate` 分支 + parse case，**接口签名零变化**（layout_type 本就 long 透传，COM/idl/C-API/Python 全不动）。
- **#1 全路径 setlog**：非法 layout_type/size_mode/anchor_mode、句柄串空/含非法项/含 0 或负值、Uniform 尺寸 <50×50、Apply 每窗 IsWindow/SetWindowPos/尺寸/位置校验失败——原先全部静默 ret=0，现在原因落 `__op.log`（失败时注明"前 k 个可能已移动"）。
- **#2 拒绝 HWND 0**：`_wcstoi64("0")` 解析成 HWND 0 原先能过 parse 关，摆到它才 `IsWindow` 失败造成半应用；现 parse 阶段拦截。
- **#3 两阶段 Apply**：先全量 SetWindowPos（记录移动前 insets）→ `Delay(100)` 一次 → 统一逐窗校验。校验语义与逐窗交错版严格一致，N 窗口等待从 N×100ms 降到 100ms，且裸 Sleep 换消息泵 Delay。
- **测试**：`window_state_test.cpp` +3——Cascade 步距判别力（3 窗实测左上角 = 起点+i×32）、句柄串含 0 拒绝、非法 layout_type 拒绝。WindowStateTest 10/10。
- libop.h LayoutWindows 注释补格式/失败语义/半应用说明。

### 2026-09-17（RunApp 支持 .lnk 快捷方式）

- **背景**：RunApp 底层直调 `CreateProcessW`，不解析 .lnk（非 PE 文件），实测传快捷方式 mode=0/1 均 ret=0。用户拍板走 ShellExecuteEx 方案。
- **实现**（`WindowProcess.cpp` `RunApp` 开头分支）：检测后缀 `.lnk`（不区分大小写、容忍尾部引号/空格）→ `ShellExecuteExW`（`SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI`，verb=open）由 Shell 解析目标/参数/工作目录。非 .lnk 路径原逻辑一行不动。
- **语义边界**（已在 libop.h 注释写明）：mode 对 .lnk 不生效（工作目录由快捷方式自带）；Shell 能拿到进程句柄时（exe 目标）pid 为真实 pid，否则 **pid=0 但 ret=1**（如 .bat/协议类目标）；失败 ret=0 + setlog（原始 err）。
- **测试**：`WindowServiceTest.RunAppLaunchesShortcut`——IShellLinkW 现场建 .lnk 指向 notepad → RunApp → ret=1 + pid 进程存活验证 + 清理。定向 RunApp* 2/2 PASS。
- **验证**：修复前 .lnk ret=0/0 → 修复后 ret=1/1，真机 Super Browser.lnk 实际拉起 ziniaobrowser.exe（pid=0 属预期语义）。

### 2026-09-17（window 模块排查闭环，`74f8e70`）

- **排查结论**：2 个真缺陷（W1/W2，均继承自上游大漠）+ 6 项建议。WindowService 为 OpContext 成员（每实例独立）；DllInjector/RunApp/Clipboard 全 RAII 无泄漏面。
- **W1（P1 修复）SetWindowTransparent 覆写扩展样式**：原 `SetWindowLong(hwnd, GWL_EXSTYLE, 0x80001)` 直接赋值，调一次透明窗口已有的 WS_EX_TOPMOST（置顶）/TOOLWINDOW 等全部丢失。改为「读旧值 `| WS_EX_LAYERED`」+ `SetWindowLongPtrW`（宽字符/指针安全）。
- **W2（P1 修复）GetMousePointWindow 回退矩形判定**：原 `rc.right >= point.x - rc.left` 把屏幕绝对坐标与相对坐标混比，改为与 `point` 直接比较（回退路径，仅在 `WindowFromPoint` 失败时走）。
- **W3 SendPaste 目标解析对齐**：与 SendString/SendStringIme 一致走 `ResolveInputTargetWindow`，焦点在子控件（编辑框）时不再静默无效。
- **W4 GetProcessInfo CPU 失败值**：`get_cpu_usage` 失败返回 -1 强转 DWORD 得 4294967295，改为 <0 时输出 0。
- **W6 错误日志**：RunApp CreateProcessW 失败 / EnumProcess 无匹配 / SetWindowTransparent 失败或非法 hwnd 三处 setlog。
- **W5/W7 注释**：bZwindow 语义标记（GW_HWNDFIRST 枚举即 Z 序，无需排序）；EnumWindowByProcess filter=0 拒绝语义；EnumProcessbyName type=1 模糊匹配当前无内部调用方。
- **W8 测试**：新建 `tests/window_state_test.cpp`（8 条）。SetWindowTransparent 保留 TOPMOST/TOOLWINDOW + alpha=200（W1 判别力）、非法 hwnd 拒绝、GetWindowState 五 flag、最大化/恢复、禁用/启用、剪贴板往返、SendPaste 投递焦点子控件（W3 判别力）。
- **验证**：window 定向 19/19（7 新 + 12 旧）；全量 **238 / 205 / 26 / 7**，FAILED 与基线同名同数，零回归，1m31s 自行退完

### 2026-09-17（algorithm 模块排查闭环，`97386a0`）

- **排查结论**：无 P0/P1。AStar.h 实现正确（切比雪夫启发式与 8 方向等代价步进精确一致；f 相同按大 g 优先；禁对角穿墙；父指针回溯）。栈上实例纯计算无资源面，天然线程安全。
- **A2/A5 地图尺寸上限**：`set_map` 新增 64M 格上限（约 576MB 临时数组），超出返回 false（`_walls` 置空 = 全图视为墙）→ Op 层 setlog 拒绝寻路。原实现 w×h 无上限，insane 尺寸直接 bad_alloc 崩溃；int 索引溢出边角一并覆盖。
- **A4 错误日志**：非法尺寸、disable_points 非法项（break 截断语义留痕）、超上限 三处 setlog（经 `set_show_error_msg(2)` 落 `__op.log`）。
- **A3 文档化**：libop.h 注释写明输出格式——路径 `"x,y|x,y"`（起点→终点）、不可达返回空串；FindNearestPos 返回 `"name,x,y"` 或 `"x,y"`，点名不含空格/逗号，等距取先出现。
- **A1 测试补齐**：新建 `tests/algorithm_test.cpp`（新增 17 条；排查记录"无专测"不准，实有 3 条散落于 `op_algorithm_windows_test.cpp`）。新增用例按确定性行为推演（不依赖优先队列等价节点弹出序）：走廊逼迁/禁穿墙角/部分墙绕行/malformed 截断语义/越界墙忽略/非法与超限尺寸/不可达空串/最近点 type1·2·等距·裸点跳过。
- **验证**：AlgorithmTest 定向 20/20（17 新 + 3 旧）；全量 231/198/26/7，FAILED 回到基线 7 条（上轮多挂的 1 条 WGC 时序敏感本轮自愈），零回归。

### 2026-09-17（memory 模块排查闭环，`c4eac12`）

- **背景**：memory 模块（`ProcessMemory` 844 行 + `OpMemory` 185 行，13 API）按五步闭环排查。结论：**无 P0/P1**——每次调用新建实例零全局状态（并发 ✅）、FindData 的 OpenProcess 全路径无泄漏（资源 ✅）。必修 0 项，落地 4 项建议加固 + 1 份新测试 + 1 处文档化。
- **M1 测试补齐**：新建 `tests/memory_readwrite_test.cpp`（10 条）。此前 FindData/GetModuleBaseAddr 有 6 条专测，但 Read/Write × Int/Float/Double/String/Data 十函数**零覆盖**。全部本进程自测（免注入沙箱可跑）：Int 全 7 类型往返、Float/Double 往返、String 三编码往返、Data 十六进制往返（输出恒大写）、地址表达式（`<kernel32.dll>` 读 PE MZ 魔数、`[[p2]]` 二级指针）。String 类用例用 VirtualAlloc 双页布局把缓冲区放第二页页首——auto-len 整页 4KB 读确定性不越界（静态变量缓冲区读后 4KB 是否已提交不可控，会引入随机失败）。
- **M2 WriteData 静默零填充留痕**（`ProcessMemory.cpp` `WriteData`）：size 超出 data 实际字节时按大漠兼容行为补零写入，但静默补零会掩盖 size 手滑（多写一位就往目标进程写一串 0）→ 补零发生时 `setlog` 留痕。行为不变。
- **M3 ReadString 显式 len 上限**（`ReadString`）：len≤0 本有 4096 自动上限，但显式 len 无上限（len=10 亿 ≈ 1GB 一次分配）→ 截断到 16MB + `setlog` 留痕。
- **M4 FindData lazy 分配**（`FindData`）：原每次无条件预分配 16MB chunk，搜小范围也是 16MB → 按 range 实际大小（`min(16MB, span)`）分配。
- **M5 错误分支日志**（对照 capture 批次标准）：Attach 失败、模块 `<mod>` 未找到、非法特征码/范围、无效句柄、OpenProcess 失败，全部 `setlog`（经 `set_show_error_msg(2)` 落 `__op.log`）。
- **M6 文档化**：libop.h 内存段 + `OpMemory.cpp` 注释明确——**Op 层 hwnd=0 优先作用于已绑定窗口**（未绑定才读本进程），与 ProcessMemory 底层"空=当前进程"的视角差异。
- **验证**：memory 定向 **17/17**；全量 **214 / 180 PASS / 26 SKIP / 8 FAILED**（FAILED 全为已知环境项：6+1 WGC 首例 SEH 熔断连带 + WaitKey，无 memory 相关），90.7s 自行退完，零回归。首编因 `HexOf` 未接受 volatile 指针 C2664 返工一次；`StringRoundTrip` 曾误断言 UTF-8 写入可按 UTF-16 读回（实际返回 `\x6261` 乱码是正确行为），已修正断言。

### 2026-09-17（键鼠拟人化批次，`f389b6a`）

- **背景**：键鼠已有贝塞尔轨迹 + smoothstep 加减速 + jitter 弯曲打底，但存在 1 个真缺陷 + 4 个机器统计特征。全部零 API 变更（`SetMouseDelay`/`SetKeypadDelay` 语义从"精确值"变"基准值"）。
- **真缺陷 · 随机数从未播种**（`libop/op/OpContext.cpp` + `libop/base/Utils.*`）：全库无 `srand()`，`rand()` 默认种子固定 → 每次进程启动"随机"轨迹/落点序列完全相同，多开同脚本等于明文自动化。修法：新增 `SeedProcessRandom()`（原子 CAS 保证进程级一次，种子 = tick^pid^地址），`OpContext` 构造时调用。
- **点击/按键时长抖动**（`WinMouse.cpp` `send_input_click`/`button_click`、`DxMouse.cpp` 5 处 `MOUSE_DX_DELAY`、`WinKeyboard.cpp` 3 处 `KEYPAD_*`、`DxKeyboard.cpp` `KEYPAD_DX_DELAY`）：固定 30/10/50ms → 每次 ±40%。新增 `jittered_delay_ms(base, percent)`（下限 1ms 防按下/弹起被合并，base<=0 保持无延时）+ `DelayJitter()` 包装。
- **轨迹时间轴抖动**（`WinMouse.cpp` `run_mouse_path`）：原每步等步长（duration/(n-1)）是机器移动最强统计特征 → 每步 ±30% 抖动 + 5% 概率微停 20~60ms。
- **双击间隔抖动**（`normal_double_click`/`button_double_click`/`xbutton_double_click` + DxMouse 对应路径）：固定间隔 → ±40%。
- **测试**：`utils_test.cpp` +3 用例——`JitteredDelayStaysWithinBoundsAndVaries`（200 样本全落在 [60,140] 且样本数>1）、`JitteredDelayFloorAndZeroBase`（下限 1ms / base<=0 返回 0）、`SeedProcessRandomSeedsAtMostOnce`（至多一次播种；时序类行为断言易受 15.6ms 系统计时量化影响，故只测纯函数 + 幂等语义，不测行为时序）。
- **验证**：`UtilsTest` 10/10（3 条新用例全 PASS）；`MouseKeyTest.*` 42 RUN / 41 PASS / 1 FAILED（FAILED = 既有 `WaitKeyScanAllWithWaitFindsKey` 环境项）；全量 **203 用例 / 170 PASS / 26 SKIP / 7 FAILED**，FAILED 与基线**同名同数**（6 WGC + WaitKey），且 203-200=3 差值恰为 3 条新用例 → 零回归。全量 1m30s 自行退完（上一轮 1h6m 为环境偶发，未复现）。首编曾因 `DelayJitter` 声明缺默认参数 C2660 返工一次。
- **备注**：首轮回全量后 `git status` 发现键盘两文件漏改（`WinKeyboard.cpp`/`DxKeyboard.cpp` 共 4 处 `KEYPAD_*` 仍是 `::Delay`），已补改并重编重跑，最终数字为补改后结果。

### 2026-09-17（测试进程退出挂起修复）

- **现象（系统性，非偶发）**：op_test 全量/部分套件跑完后 gtest 总结已打印、结果已落盘，但进程不退出（此前多轮全量均为手动杀进程）。逐套件二分定位：**HandleCompatTest / MouseKeyTest / ImageColorTest / WgcTest** 四个建"可见+焦点窗口"的套件挂起（HandleCompatTest 最稳，5 挂 4），纯逻辑套件全正常。
- **根因（环境）**：窗口抢焦点后 Windows 把本机 IME（**搜狗 SogouPY.ime**）+ TextInputFramework 载入测试进程；挂起进程 tasklist /V 显示 `OleMainThreadWndName / Not Responding`——COM/OLE 在 ExitProcess 分离阶段被第三方 IME 卡死。op 代码本身无缺陷。
- **修法（仅测试进程）**：`tests/main.cpp` 在 `RUN_ALL_TESTS` 返回、结果已落盘后 `fflush` + `TerminateProcess` 自决退出，绕开第三方 IME 的 detach 挂起。生产宿主无焦点窗口不受影响。
- **验证**：修复前 `ForegroundWindowHandleFitsInLongOnlyWhenNoTruncation` 3/3 挂；修复后该用例 5/5 退出，四个挂起套件各 2/2 退出；全量回归见本节回填。
- **遗留**：本次挂起遗留在系统中的僵尸 op_test.exe（PID 16552，内核态杀不掉）持有 `tests/op_test.exe` 文件锁 → **需重启系统释放**；重启前增量链接会 LNK1104，验证用的是临时改名目标 `op_test_new.exe`（已还原，CMakeLists 无残留）。

## [Unreleased] — 2026-09

### 2026-09-17（键鼠增强与加固批次）

- **dx 字符输入补齐**（`libop/hook/InputHook.cpp`）：`OP_WM_CHAR` 不再受 WINDOWMSG 通道开关约束——该开关只约束鼠标/键盘事件的窗口消息镜像，而 WM_CHAR 是向 GDI/Qt 类目标输入文本的唯一载体。修复前绑 `dx.dinput`/`dx.raw` 后 `KeyPressStr` 返回 1 但目标收不到字符（静默无效）。新用例 `DxModeKeyPressStrDeliversCharOutsideWindowMsgChannel`。
- **bind 后缀防污染**（`libop/binding/BindingSession.*`）：新增 `_dx_attr_from_suffix` 来源标记，上次带后缀的绑定不再把掩码残留到下一次纯 `dx` 绑定（无后缀且有标 → 回默认全开；`SetDxAttr` 显式设置优先）。新用例 `BindWindowDxSuffixDoesNotPolluteSessionAttr`。
- **远端 Hook 自愈重绑**（`libop/hook/HookExport.cpp` `SetInputHook`）：`is_hooked && input_hwnd != 新hwnd`（宿主异常退出残留）时先 `release()` 再 `setup()`，不再永久拒绝——正常路径（`bb98e92` pid 缓存解绑）不受影响。
- **验证**：`MouseKeyTest.*` 42 RUN / **41 PASS** / 0 SKIP / 1 FAILED（FAILED = 已知 `WaitKey` 环境项）；全量 **200 用例 / 167 PASS / 26 SKIP / 7 FAILED**，FAILED 与既有基线同名同数（6 WGC 环境项 + WaitKey），3 条新用例全 PASS → 零回归。

### 2026-09-17（键鼠域 · MoveR 跨模式一致性核查收口，`d285b83`）

- **背景**：阶段①报告 🔴 待查项「`WinMouse::MoveR` 的 `_x/_y` 与 `_button_state` 跨模式一致性」。
- **核查结论：无生产缺陷，不改代码**。① 跨模式污染不存在——每次 `BindWindowEx` 都新建鼠标/键盘对象（`BindingSession.cpp:310-311` + `createMouse/createKeypad:689-702`），`_x/_y/_button_state` 零跨绑残留；② 三条记账路径（`WinMouse::MoveR:254-255` 手动累加 / `WinMouse::MoveTo:299` / `DxMouse::MoveTo:78` 无条件回填）统一为「记最后请求位置」的意图语义，失败也记账是**一致设计**，按够用总原则不动；③ IN_NORMAL 无自动测试是**结构性限制**（驱动真实系统光标，会抢用户鼠标），非疏漏。
- **改动（仅测试 +40 行）**：新增 `MouseKeyTest.RebindResetsMousePositionState`——钉住「重绑后账本清零、首个 MoveR 落在 (rx,ry)」的重置语义，防回归。
- **验证**：`MouseKeyTest.*` 40 RUN / 39 PASS / 0 SKIP / 1 FAILED（FAILED = 已知 `WaitKeyScanAllWithWaitFindsKey` 环境项）；全量 **198 用例 / 164 PASS / 26 SKIP / 8 FAILED**，与上一基线（197/164/26/7）差值 = +1 新用例 PASS 且 1 条 WGC 时序敏感用例翻转（`NormalDxgiFirstCaptureAfterBindUsesFreshFrame`，本机既有不稳定，非回归）→ 键鼠域零回归。
- **教训**：后台全量测试进程常驻会持有 `op_test.exe` 文件锁 → 增量链接 LNK1104；跑测试期间勿构建，构建前 `tasklist` 确认无残留。

### 2026-09-17（键鼠域 · dx Hook 生命周期修复，`bb98e92`）

- **背景**：键鼠域排查（阶段①报告 `键鼠域_阶段1发现_20260916.md`）发现 9 条 `MouseKeyTest.DxMode*` 用例长期 SKIP，旧结论「环境不支持」被推翻——**单跑即 PASS（1278ms）、全量必 SKIP（34ms）**，是测试间的顺序状态污染。
- **根因**：`InputHookClient.cpp` 的引用计数表只存 `HWND`，解绑时用 `GetWindowThreadProcessId(hwnd)` 现取 pid；当收尾顺序是「目标窗口销毁 → 再解绑」（对象析构 / 脚本退出）时 pid 取到 0 → 远端 `ReleaseInputHook` 不执行 → 目标进程内 `is_hooked` 永久为 true、MinHook 未卸；目标进程仍存活时后续重新绑定被 `HookExport.cpp` 的 `is_hooked && input_hwnd != 新hwnd` 直接挡掉（`return 0`）。
- **真机影响**：脚本绑 dx 后若**目标程序先退出 / 窗口先关闭**才解绑 → 该进程内 dx 永久失效，且**重启脚本无效**（Hook 在目标进程内）。此前的真机验证都是「干净绑定 → 验证 → 退出」，从未覆盖「解绑后重绑」，故该洞一直未暴露。
- **改动**（2 文件 +45/-12）：① 引用计数表值 `long` → `struct HookBindRef { long refs; DWORD pid; }`，绑定成功时即固定 pid；`call_release_input_hook` 改收 pid，解绑不再依赖 hwnd 是否仍然有效。② 测试 `BindWindowDxSuffix{DefaultsToAll,NarrowsToDinput,CombinesMouseKeypad}` 三条补显式 `UnBindWindow`。
- **验证（阶段④）**：nmake rc=0（仅既存 D9025）；`MouseKeyTest.*` **39 RUN / 29 PASS / 9 SKIP → 39 RUN / 38 PASS / 0 SKIP**——9 条 `DxMode*` 首次全部真跑 PASS，解锁 dx 三通道开关 / `LockInput` / `CursorShape` / buffered DirectInput / `KeyPressStr` Unicode 回退的回归覆盖；全量 **197 用例 / 164 PASS / 26 SKIP / 7 FAILED**，FAILED 与基线**同名同数** → 零回归。
- **文档**：`scripts/DX_INPUT_CHANNEL_REPORT.md` 新增 §8（9 条已知限制与设计取舍，含「dx 非 windowmsg 通道下字符输入不可达」等）、§9（本次修复）；并更正 §7 旧说法「本机 DX 注入在测试后期可能偶发不可用」——实为 hook 残留污染，非环境问题。

### 2026-09-16（晚 · 四处静默失败补日志 + DPI 检测，`896f3af`）

- **背景**：真机复盘中确认四类失败**全程无声**（返回 False 或哨兵值，无日志、无异常、无错误码），使用者只能反复试错。本次统一策略：**只加日志，不改任何返回值与控制流** → 零兼容风险。共 4 文件 +64/-4 行。
- **① capture 落盘失败**（`image/ImageSearchService.cpp` `Capture()`）：记录完整路径 + 尺寸 + "检查扩展名与目录"。真因：`CImage::Save` 按扩展名查 GDI+ 编码器，无扩展名/未知扩展名直接返回非 `S_OK` → `capture` 返回 0 且无提示（**抓屏其实早已成功**，失败的只是写文件）。
- **② hook 显示模式无帧**（`capture/backends/HookCapture.cpp` `requestCapture()`）：记录 hwnd + "目标未产生 present 帧" + 建议改用 `normal`/`gdi`/`dx2`。`dx` 显示模式依赖目标自身的 Present/SwapBuffers 调用，纯 GDI/Qt 渲染窗口（如 BlueStacks）永不产生该类帧，表现为 **bind 成功但图色全空**。
- **③ 窗口最小化**（`op/OpWindow.cpp` `GetClientRect()`）：记录一行提示 `-32000` 哨兵值。该值会被调用方当作正常尺寸继续参与坐标换算/裁剪，结果是巨大负数。窗口恢复后**不误报**（已实测）。
- **④ DPI 检测（新增）**（`op/OpContext.cpp` 构造函数）：记录提升前后 DPI 状态 + 系统 DPI + 缩放比。**机制查清**：`::SetProcessDPIAware()` 是进程级一次性调用（属**有意设计**，与类大漠一致：统一物理像素），会把宿主进程坐标语义从"被系统虚拟化的缩放后坐标"改为"物理像素"；脚本混用提升前的坐标即整体偏移一个缩放比（150% → 偏 1.5 倍），现场表现"点了没中"。**子进程隔离 A/B 铁证**：同一 HWND 未加载 OP 时线程 `UNAWARE` / `GetClientRect=293×494`，加载后 `SYSTEM_AWARE` / `440×741`，逐项**精确 ×1.5**（`LOGPIXELS=144`）。因提升只发生在首个实例（那时日志通常尚未打开），后续实例继续记录当前状态，保证信息随时可取。
- **验证（BlueStacks 5 真机，四条全部命中）**：`dpi: process is DPI-aware, system dpi=144 (scale 150%)...` / `capture write failed: ...\noext (160x120), check the file extension...` / `hook frame not ready: hwnd=... no present frame...` / `get_client_rect: hwnd=... is minimized, the rect is a -32000 sentinel...`
- **回归**：`op_test` **197 用例 / 155 PASS / 7 FAILED**，与改动前**同名同数**（`MouseKeyTest.WaitKeyScanAllWithWaitFindsKey` + `WgcTest` 6 项，均为已知环境问题）→ **零回归**。

### 2026-09-16（DX 注入崩溃闭环 + 绑定层收口，4 提交）

- **背景**：DX 输入通道（mouse/keypad 含 `dx`）绑定时注入宿主 `op_c_api_x64.dll`，其隐式依赖 `onnxruntime.dll`。上一轮 `/DELAYLOAD` 只把「加载期失败 `0xC0000135`」推迟成「运行期失败」—— 目标进程执行 `InputHook::setup` 触碰 ONNX 符号时，延迟加载在**目标进程**内搜不到 onnxruntime → **目标进程 `0xC0000005` 崩溃**。唯一变量对照：目标目录有/无 `onnxruntime.dll` → bind=1+存活 / bind=0+崩溃。
- **修复（方案 A1）`c4420ae`**：新增 `libop/hook/DliFailureHook.cpp` 实现 `__pfnDliFailureHook2` —— `dliFailLoadLib` 时改用**本模块所在目录**（op 安装目录）的绝对路径 + `LOAD_WITH_ALTERED_SEARCH_PATH` 兜底加载；非 onnxruntime 的失败一律不干预。两个 `/DELAYLOAD` 目标各编入一份，故显式列入 `OP_COM_SOURCES` / `OP_C_API_SOURCES`。**坑**：`delayimp.h` 默认把该符号声明为 `const`，覆写须先 `#define DELAYIMP_INSECURE_WRITABLE_HOOKS`；实测无 LNK2005（delayimp.lib 的默认钩子是弱符号）。修复后**目标进程不再需要自带 onnxruntime.dll**。
- **验证（真机，自控目标 `dx_target.exe` 置于不含 onnxruntime 的目录）**：绑定矩阵 **10/10 bind=1**（修复前该场景 bind=0 且目标崩溃）；端到端送达 `MoveTo+LeftClick` → move=1 down=1 up=1、`KeyPress(F1)` → key=1；目标进程全程存活。
- **回归**：`op_test` 197 用例 **155 PASS / 35 SKIP / 7 FAILED**。其中 WGC 6 项经 **A/B 判别**（换回修复前 DLL 跑同一 `--gtest_filter=WgcTest.*` → 同样 6 FAILED + 同样 `WgcCapture::Init SEH fault 0xC0000005`）确认为**本机既有 WGC 环境问题**（首个用例 SEH 后熔断连带），与本次改动无因果；另 1 项为已知 `MouseKeyTest.WaitKey` 环境失败。
- **绑定层收口**：`21108d6` Go 补 `SetDxAttr`/`GetDxAttr`（本机无 Go 工具链，未 `go build`）；`6b0af21` SWIG 4.4.1 重新生成，补齐落后契约的 **12 个方法**（wrapper `_wrap_Op_*` 223→235，`_pyop.pyd` 424,960 B，`import _pyop` 后 235 个 `Op_*` 全可见）；`228751a` 四类验证工具入库（`api_surface_diff.py` / `five_layer_matrix.py` / `check_swig_sync.py` / `dx_probe.py` / `dx_target.cpp` + `build_dx_target.py`）。
- **DX 门槛 B（按"够用"原则降级为文档化，未做）**：目标进程须已加载 `dinput8.dll`，否则 `hook_dinput()` 失败即**整体拒绝**输入绑定（`InputHook.cpp:883-890`，P1-9 引入），无辜牵连 `dx.raw` / `dx.win`。改"按需判定"需先把期望通道掩码传进 `SetInputHook`（现第二参数被忽略），属**增强而非必需**。

### 2026-09-16（base 修复补针对性测试 + 判别力验证，2文件）

- **背景**：73dce39 的四项修复此前只有"编译 + 全量回归"验证（证明未引入回归，未证明修复行为生效）；原 `tests/utils_test.cpp` 仅 2 个 happy-path 用例（ThreadPool(4) / divideBlock(2)）。
- **新增 5 个回归用例**（utils_test.cpp 2→7）：S3 `ThreadPool(0)`（用 wait_for 防挂死）、S1 `count<=0` 清空 + `count>span` clamp、S2 hex2bin 小写、B1 setlog 宽版不再二次解析（**比对 `__op.log` 内容而非返回值**——旧实现同样返回 1）。
- **tests/CMakeLists.txt**：`../libop/base/Utils.cpp` + `Environment.cpp` 编入测试目标（`setlog` 未从 DLL 导出，直接 include 会 LNK2019，跟随 WindowLayout/CursorShape 既有模式），补链接 `shlwapi.lib`。
- **判别力验证（回退对照，实证）**：`git restore --source=73dce39^` 回退 base 重编实测 —— B1 用例**进程中断**（UB 实致崩溃，非理论风险）、S1 zero-count 触发 `Assertion failed: count > 0`（Types.h:99，EXIT=3）、S1 count>span FAIL（size=20≠10 + 20 个 0 宽块）、S2 FAIL（'a'=42/'f'=47 恰为 c-'A'+10）、S3 FAIL（wait_for 5s 超时）。恢复后 7/7 PASS。**5/5 捕获旧缺陷，判别力成立**。
- **附带发现**：op_test（`/MT`、**无 NDEBUG**）与生产（`/MD`、`/DNDEBUG`）编译标志不一致 → S1 在测试中表现 assert 中止、生产中才是除零（报告"release 除零"表述成立）；测试对跨 DLL 内存传递无覆盖（观察项）。
- 验证：全量回归 **187 用例 / 151 PASS / 35 SKIP / 1 FAILED（已知 MouseKey）= 零回归**。

### 2026-09-09（模块排查·批1 base 修复，4文件 +54/-30）

- **base 模块体检（doc2/模块排查_base_2026-09-09.md）后执行 B1+S1+S2+S3**：
  - **B1** `setlog(wchar)` 二次格式化 UB（崩溃级，触发面 58 处宽版调用）——抽 `setlog_text()` 内部输出体，宽/窄两版均"格式化一次→落盘"，不再把已展开文本当 format 二次 va_start/vsprintf_s（日志含 `%` 字面即 UB，`%s` 展开内容含百分号时可能崩）。
  - **S1** `rect_t::divideBlock` release 除零 + 0 尺寸块——入口 `count<=0` 早退 clear、`count>span` clamp 到 span。
  - **S2** `hex2bin` 不认小写 a-f（ProcessMemory.cpp:552 直调未 towupper 解析错乱）——本体入口归一化小写，全局通吃。
  - **S3** `ThreadPool(0)` enqueue future 永不 ready——构造 `threads==0→1`（两调用方已防护，纯加固）。
  - 验证：nmake 全库重编（头文件改动）8m52s EXIT=0 / 0 error；op_test 回归 182 用例 146 PASS / 1 已知 MouseKey 环境 FAIL / 2 DISABLED = **零回归**。产物已同步 bin/x64。

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

- **op_test 全量（2026-09-17 `bb98e92` 后实测，无 filter）：197 用例 / 164 PASS / 26 SKIP / 7 FAILED**。7 FAILED = WGC 6（首个用例 `WgcCapture::Init` SEH 0xC0000005 → 熔断连带，**已用 A/B 判别确认为既有环境问题**）+ `MouseKeyTest.WaitKey`（已知环境项）。SKIP 26 = `OcrFixture` 25（本环境无 OCR 服务）+ `IntegrationTest` 1。`MouseKeyTest.*` 单跑为 **39 RUN / 38 PASS / 0 SKIP**（仅已知 `WaitKey` 环境失败）。
- 上一基线（2026-09-16 A1 后）：197 / 155 PASS / 35 SKIP / 7 FAILED。**差值 = 9 条 `DxMode*` 由假 SKIP 转为真跑 PASS**（见 2026-09-17 条目），非回归。
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
- [x] **P1 竞态类六项（第二轮核查遗留，2026-09-09 三批闭环）**：批1 **a35eb3e**（P1-14 TemplateMatcher static ORB → thread_local 线程隔离；P1-10 InputHookClient UnBind 先远端释放后清引用+RPC 异常保留可重试）；批2 **af037b2**（P1-8 InputHook 状态镜像 m_mouseState/m_keyboardState/m_vkState/m_wheelDelta 新增 g_stateMutex 消除无锁 RMW——fill_mouse_state/consumeWheelDelta 读清原子化，m_inputLock/m_dxAttrs 转 atomic；P1-9 hook_dinput 全失败不再谎报成功，setup 中止输入绑定）；批3 **1be3057**（P1-11 Pipe close 对 reader 阻塞 ReadFile 调 CancelSynchronousIo，防子进程退出后孙进程持写端致 join 无限挂起；P1-15 WgcCapture 尺寸读写经 sharedResourceMutex_ 快照/写入 + getFrameInfo 覆写持资源锁——核查确认 GDI/Hook 后端 ensure 删重建全程持 _pmutex 无同病，仅 Wgc 独病）。每批 nmake+op_test(-Wgc) 零回归（仅已知 MouseKey 环境失败）。**WgcCapture 收帧/尺寸路径改动已真机复核**——2026-09-09 用户跑 `run_wgc_isolated.bat` **10/10 全 PASS 零崩溃**，含批3 核心路径用例 FirstFrameResizeAndCloseScenarios / RepeatedBindUnbindAndMaximizedRepeatedCapture / 最大化两例，无功能回归。
- [x] **P2 死代码清理（2026-09-09，提交 1bf2c89，10 文件 -403 行）**：删 TesseractOcr.{h,cpp}（从未编入 CMake 源列表）、ImageView.h（class ImageView 零引用，撞名 RawImageView 独立保留）、ImagePreprocessor.h（空壳转发头）4 个死文件；**#18 报告误判修正**：WindowState.cpp 实为 WindowService 的 9 个方法实现（8 个有 COM API 调用链），不能整文件删——只删唯一死的 `GetTopWindowSp`（0 调用，唯一引用 GdiCapture.cpp:107 注释代码；含 GetWindowLongA 不更新 i 的死循环缺陷，删定义+WindowService.h:45 声明）；WinKeyboard.cpp 删 file-static `oem_code`（8-33 含函数内局部码表，0 调用）；ImageSearchAlgorithms 删 `gen_next`（.h:47+.cpp:188）/`Connectivity`/`extractConnectivity`（.h:56+.cpp:263）及连带 `flood_connectivity`（仅被二者调用）。残留仅注释（GdiCapture.cpp:107、KeyboardBackend.cpp:6）与 l2_audit_out.txt 快照，非源码。op.rc TODO 占位**维持现状**（用户拍板，非死代码，UTF-16 留待填真实元数据）。每批 nmake+op_test(-Wgc) 零回归（182 用例 146 PASS 仅已知 MouseKey 环境失败）。
- [x] **OCR 耗时核查（2026-09-09，纯核查未改码，报告 doc2/OCR_性能核查.md + 剖析脚本 ocr_profile{,_2,_3}.py）**：沙箱复现 ocr_from_file 冷净 49ms（真机 42.6 同量级）、单行 1 次 rec；**同配置跨进程 49 vs 130-150ms 3 倍波动**——经冷却 60s 复测排除降频，归因共存负载（雷电 HD-Player 常驻/SearchIndexer/i5-13400 P-E 核调度），9-13ms 目标本机日常环境不可达不可复现。det 缩放实验（张量 160×608→32×160）仅省 26ms（20%）→ det 非绝对大头；autoocr_line 免 det 仍 33ms → rec 单行本机或 ≥20ms（未复现注释"4线程 10-15ms"）。threads 4 vs 1 省 46ms。**瓶颈归属无法外部定论（C-API 无 rec-only 文件入口），需引擎内 QPC 分段计时（已列建议，待拍板）。**
- [x] **pyop vs op 双绑定澄清 + op-capi 可安装化（2026-09-09，提交 6dd9e57）**：初判"pyop 是死代码"被推翻——`python/pyop/` 是 **SWIG 4.4.1 生成绑定**（依赖 `_pyop` C 扩展，CMake build_swig_py=ON 编译，根 pyproject 为 scikit-build-core 编译通道），`bindings/python/op/` 是 **ctypes 新绑定**（纯 .py 零编译，smoke/剖析实际使用）。**进一步核查推翻"改根 pyproject wheel.packages 指向 ctypes"方案**：`bindings/python/` 本就是独立发布单元（自带 pyproject.toml，name=op-capi，纯 setuptools，README 明写与 SWIG 相互独立）；`_ffi._candidate_paths()` 候选链第 3 位即包内 `op/bin/x64/`，架构完备仅 DLL 缺位。**落地**：DLL 预置 `bindings/python/op/bin/x64/`（op_c_api_x64.dll 26.6MB + onnxruntime.dll 11.2MB + providers_shared），.gitignore 增 `bindings/python/op/bin/` 防 37.9MB 入库；验证=仓库外 cwd 无参 `Op()` 构造成功自动命中包内 DLL（get_last_error=0）→ `pip install ./bindings/python` 开箱可用。**遗留**：SWIG 通道（根 pyproject+swig/+python/pyop/）**保留作编译型备选**（2026-09-09 用户拍板，不退役不删，需要编译型 wheel 或 C 扩展场景时仍可用）。
- [~] **push：暂缓**（2026-09-09 收尾时共 24 个提交，56f35d9 起最新见 `git log`）。2026-09-08/09 复测 github/gitee/gitcode/hf/pypi 全不可达（白名单式封锁）——2026-09-09 用户定调：先不 push、不做 bundle 备份，变更以本地记录为准（git 提交链 + 本 CHANGELOG + 项目 memory）。网络恢复后可推时无需特殊准备（提交链完整）。
