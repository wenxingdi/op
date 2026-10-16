# OP 插件 API 速查（LLM 上下文版）

> 源：`include/libop.h` · 310 个接口 · 生成于 2026-10-16 13:23 · 由 scripts/gen_api_reference.py 生成（勿手改）

调用约定：出参不传入，聚成返回值元组（顺序 = signature 的 → 后列表）；坐标为物理像素，绑定窗口后为客户区坐标；字符串 UTF-16。

## Runtime 运行时（32）

- `Ver() → string` — 1.版本号Version
- `SetPath(path) → (ret)` — 设置目录
- `GetPath() → (et)` — 获取目录
- `GetBasePath() → (et)` — 获取插件目录
- `GetID() → (ret)` — 返回当前对象的ID值，这个值对于每个对象是唯一存在的。可以用来判定两个对象是否一致
- `GetLastError() → (ret)` — 取最近一次失败调用的内部错误码（0=无错误）；定位问题配合 SetShowErrorMsg(2) 日志
- `SetShowErrorMsg(show_type) → (ret)` — 设置是否弹出错误信息,默认是打开 0:关闭，1:显示为信息框，2:保存到文件,3:输出到标准输出
- `Sleep(millseconds) → (ret)` — sleep
- `InjectDll(process_name, dll_name) → (ret)` — Process
- `EnablePicCache(enable) → (ret)` — 设置是否开启或者关闭插件内部的图片缓存机制
- `ClearPicCache() → (ret)` — 清空进程级图片缓存（FindPic 自动缓存与显式加载共用）
- `SetPicCacheMax(max_count) → (ret)` — 设置进程级图片缓存条目上限；max_count<=0 表示不设上限，默认 500，满时整体清空重存
- `CapturePre(file_name) → (ret)` — 取上次操作的图色区域，保存为file(24位位图)
- `SetScreenDataMode(mode) → (ret)` — 设置屏幕数据模式，0:从上到下(默认),1:从下到上
- `RunApp(cmdline, mode) → (pid, ret)` — 运行可执行文件/快捷方式/文档/网址。mode：
- `WinExec(cmdline, cmdshow) → (ret)` — 运行可执行文件，可指定显示模式
- `GetCmdStr(cmd, millseconds) → (etstr)` — 运行命令行并返回结果
- `SetClipboard(str) → (ret)` — 设置剪贴板数据
- `GetClipboard() → (et)` — 获取剪贴板数据
- `Delay(mis) → (ret)` — 延时指定的毫秒,过程中不阻塞UI操作
- `Delays(mis_min, mis_max) → (ret)` — 延时指定范围内随机毫秒,过程中不阻塞UI操作
- `GetScreenWidth() → (ret)` — 获取屏幕宽度(像素)
- `GetScreenHeight() → (ret)` — 获取屏幕高度(像素)
- `GetScreenDepth() → (ret)` — 获取屏幕色深(位/像素, 如 32)
- `GetDPI() → (ret)` — 获取系统 DPI 缩放值(96=100%)
- `GetTime() → (et)` — 获取当前系统时间, 格式 "yyyy-MM-dd HH:mm:ss"
- `Beep(freq, dur) → (ret)` — 蜂鸣器发声. freq: 频率Hz(37..32767), dur: 时长ms. 返回1成功
- `GetRandomNumber(min, max) → (ret)` — 获取 [min, max] 区间内随机整数(闭区间, 含两端)
- `GetRandomDouble(min, max) → (ret)` — 获取 [min, max] 区间内随机浮点数
- `GaiLu(p) → (ret)` — 概率判定: 以 1/p 的概率返回1, 否则返回0. p<=0 返回0, p==1 恒返回1(拟人化核心)
- `GetMachineCode() → (et)` — 获取本机机器码(注册表 MachineGuid), 失败返回空串
- `IsElevated() → (ret)` — 检测当前进程是否以管理员权限运行. 1是 0否

## Window 窗口（49）

- `EnumWindow(parent, title, class_name, filter) → (et)` — 根据指定条件,枚举系统中符合条件的窗口
- `EnumWindowByProcess(process_name, title, class_name, filter) → (et)` — 根据指定进程以及其它条件,枚举系统中符合条件的窗口
- `EnumProcess(name) → (et)` — 根据指定进程名,枚举系统中符合条件的进程PID
- `ClientToScreen(hwnd, x, y) → (ret)` — 把窗口坐标转换为屏幕坐标
- `FindWindow(class_name, title) → (ret)` — 查找符合类名或者标题名的顶层可见窗口
- `FindWindowByProcess(process_name, class_name, title) → (ret)` — 根据指定的进程名字，来查找可见窗口
- `FindWindowByProcessId(process_id, class_name, title) → (ret)` — 根据指定的进程Id，来查找可见窗口
- `FindWindowEx(parent, class_name, title) → (ret)` — 查找符合类名或者标题名的顶层可见窗口,如果指定了parent,则在parent的第一层子窗口中查找
- `GetClientRect(hwnd) → (x1, y1, x2, y2, ret)` — 获取窗口客户区域在屏幕上的位置
- `GetClientSize(hwnd) → (width, height, ret)` — 获取窗口客户区域的宽度和高度
- `GetForegroundFocus() → (ret)` — 获取顶层活动窗口中具有输入焦点的窗口句柄
- `GetForegroundWindow() → (ret)` — 获取顶层活动窗口,可以获取到按键自带插件无法获取到的句柄
- `GetMousePointWindow() → (ret)` — 获取鼠标指向的可见窗口句柄
- `GetPointWindow(x, y) → (ret)` — 获取给定坐标的可见窗口句柄
- `GetProcessInfo(pid) → (et)` — 根据指定的pid获取进程详细信息
- `GetSpecialWindow(flag) → (ret)` — 获取特殊窗口
- `GetWindow(hwnd, flag) → (ret)` — 获取给定窗口相关的窗口句柄
- `GetWindowClass(hwnd) → (et)` — 获取窗口的类名
- `GetWindowProcessId(hwnd) → (ret)` — 获取指定窗口所在的进程ID
- `GetWindowProcessPath(hwnd) → (et)` — 获取指定窗口所在的进程的exe文件全路径
- `GetWindowRect(hwnd) → (x1, y1, x2, y2, ret)` — 获取窗口在屏幕上的位置
- `GetWindowState(hwnd, flag) → (ret)` — 获取指定窗口的一些属性
- `GetWindowTitle(hwnd) → (ettitle)` — 获取窗口的标题
- `MoveWindow(hwnd, x, y) → (ret)` — 移动指定窗口到指定位置
- `ScreenToClient(hwnd, x, y) → (ret)` — 把屏幕坐标转换为窗口坐标
- `SendPaste(hwnd) → (ret)` — 向指定窗口发送粘贴命令
- `SetClientSize(hwnd, width, hight) → (ret)` — 设置窗口客户区域的宽度和高度
- `SetWindowState(hwnd, flag) → (ret)` — 设置窗口的状态
- `SetWindowSize(hwnd, width, height) → (ret)` — 设置窗口的大小
- `LayoutWindows(hwnds, layout_type, columns, start_x, start_y, gap_x, gap_y, size_mode, window_width, window_height, anchor_mode) → (ret)` — 按指定布局批量排列多个窗口。hwnds 格式例如 "123|456|789"（正整数 hwnd 竖线分隔，含 0/非法项整体拒绝）。
- `SetWindowText(hwnd, title) → (ret)` — 设置窗口的标题
- `SetWindowTransparent(hwnd, trans) → (ret)` — 设置窗口的透明度
- `SendString(hwnd, str) → (ret)` — 向指定窗口发送文本数据
- `SendStringIme(hwnd, str) → (ret)` — 向指定窗口发送文本数据-输入法
- `LockWindowPosition(hwnd, enable) → (ret)` — 锁定/解锁窗口位置:锁定后外部移动该窗口会被守护线程自动拉回(50ms 轮询),解锁后停止
- `LockWindowSize(hwnd, enable) → (ret)` — 锁定/解锁窗口尺寸:锁定后外部缩放该窗口会被自动拉回
- `DisableMinMax(hwnd, enable) → (ret)` — 禁止/恢复窗口的最大最小化按钮(修改 GWL_STYLE,恢复时还原锁定前的原始样式)
- `SetIme(hwnd, enable) → (ret)` — 设置指定窗口的输入法开关:enable=0 关闭输入,1 恢复
- `EnableDebugPrivilege() → (ret)` — 为当前进程启用 SeDebugPrivilege（读高权限/提权进程内存的前置）。已启用或启用成功返回 1。
- `GetLastErrorString() → (et)` — 取当前线程的 GetLastError 错误文本（调用本方法前的最后一次 Win32 错误）。无法解析返回空串。
- `Md5(text) → (et)` — MD5/SHA1（UTF-8 编码后哈希），输出小写十六进制；空输入返回空串。
- `Sha1(text) → (et)` — 计算文本的 SHA-1 摘要（hex 串）
- `Base64Encode(text) → (et)` — Base64 编码/解码（输入按 UTF-8 字节流处理）。解码遇非法串返回空串。
- `Base64Decode(base64) → (et)` — Base64 解码，返回原文本
- `DrawRect(x1, y1, x2, y2, color, width) → (ret)` — 在屏幕透明叠加层追加一个矩形/直线/文本框。color="RRGGBB"（与找色同格式）。返回 1=成功。
- `DrawLine(x1, y1, x2, y2, color, width) → (ret)` — 在屏幕标注层画直线（调试标注用；DrawClear 清除）
- `DrawText(x, y, text, color, font_size) → (ret)` — 在屏幕标注层绘制文本（调试标注用）
- `DrawClear() → (ret)` — 清空叠加层全部图元并重绘。
- `DrawSetVisible(show) → (ret)` — 显示/隐藏叠加层窗口（图元保留）。1=成功。

## Binding 后台绑定（10）

- `BindWindow(hwnd, display, mouse, keypad, mode) → (ret)` — 兼容旧接口的单句柄绑定。显示和输入都使用同一个 hwnd。
- `BindWindowEx(display_hwnd, input_hwnd, display, mouse, keypad, mode) → (ret)` — 扩展绑定接口。显示截图使用 display_hwnd，鼠标和键盘输入使用 input_hwnd。
- `UnBindWindow() → (ret)` — 解绑窗口
- `LockInput(lock) → (ret)` — 临时锁定目标窗口的外部输入。只对 dx 鼠标、dx 键盘有效。
- `DownCpu(type, rate) → (ret)` — 绑定降载(降低多开 CPU 占用):每次截图成功后强制延时 rate 毫秒(0-100,自动钳制),0 表示关闭。
- `SetDxAttr(attr, value) → (ret)` — 设置 dx 输入通道开关。只对 dx 鼠标、dx 键盘有效。
- `GetDxAttr() → (ret)` — 获取当前的 dx 输入通道开关掩码。
- `GetBindWindow() → (ret)` — 获取当前对象已经绑定的显示窗口句柄. 无绑定返回0
- `IsBind() → (ret)` — 判定当前对象是否已绑定窗口.
- `GetDisplayMethod() → (et)` — 获取当前实际生效的图色模式名（"normal"/"gdi2"/"dx2"/"dx.d3d11"/"normal.wgc"/"opengl.nox" …）。

## Input 键鼠输入（46）

- `GetCursorPos() → (x, y, ret)` — 获取鼠标位置.
- `GetCursorShape() → (et)` — 获取当前鼠标形状: visible,hash,width,height,hotX,hotY.
- `GetCursorImage() → (et)` — 获取当前鼠标光标图像：32bpp BMP 的 Base64（top-down BGRA 含 alpha）。
- `MoveR(x, y) → (ret)` — 鼠标相对于上次的位置移动rx,ry.
- `MoveTo(x, y) → (ret)` — 把鼠标移动到目的点(x,y)
- `MoveToEx(x, y, w, h) → (et)` — 把鼠标移动到目的范围内的任意一点
- `MoveToSmooth(x, y, duration) → (ret)` — 按模拟轨迹移动到指定点
- `MoveToExSmooth(x, y, w, h, duration) → (et)` — 在目标范围内随机取点，并按模拟轨迹移动过去
- `MovePath(path, duration) → (ret)` — 按 x,y|x,y 格式提供的路径移动
- `DragPath(path, duration) → (ret)` — 按 x,y|x,y 格式提供的路径拖拽
- `SetMouseTrajectory(mode, min_duration, max_duration, jitter, start_delay, end_delay) → (ret)` — 设置模拟轨迹参数，只影响 Smooth 和 Path 系列鼠标移动
- `LeftClick() → (ret)` — 按下鼠标左键
- `LeftDoubleClick() → (ret)` — 双击鼠标左键
- `LeftDown() → (ret)` — 按住鼠标左键
- `LeftUp() → (ret)` — 弹起鼠标左键
- `MiddleClick(ret)` — 按下鼠标中键
- `MiddleDoubleClick(ret)` — 双击鼠标中键
- `MiddleDown() → (ret)` — 按住鼠标中键
- `MiddleUp() → (ret)` — 弹起鼠标中键
- `RightClick(ret)` — 按下鼠标右键
- `RightDoubleClick(ret)` — 双击鼠标右键
- `RightDown() → (ret)` — 按住鼠标右键
- `RightUp(ret)` — 弹起鼠标右键
- `XButton1Click(ret)` — 按下鼠标侧键1
- `XButton1DoubleClick(ret)` — 双击鼠标侧键1
- `XButton1Down(ret)` — 按住鼠标侧键1
- `XButton1Up(ret)` — 弹起鼠标侧键1
- `XButton2Click(ret)` — 按下鼠标侧键2
- `XButton2DoubleClick(ret)` — 双击鼠标侧键2
- `XButton2Down(ret)` — 按住鼠标侧键2
- `XButton2Up(ret)` — 弹起鼠标侧键2
- `Wheel(delta, ret)` — 垂直滚轮滚动指定 delta
- `HWheel(delta, ret)` — 水平滚轮滚动指定 delta
- `WheelDown() → (ret)` — 滚轮向下滚
- `WheelUp() → (ret)` — 滚轮向上滚
- `SetMouseDelay(type, delay) → (ret)` — 设置鼠标单击或者双击时,鼠标按下和弹起的时间间隔
- `GetKeyState(vk_code) → (ret)` — 获取指定的按键状态.(前台信息,不是后台)
- `KeyDown(vk_code) → (ret)` — 按住指定的虚拟键码
- `KeyDownChar(vk_code) → (ret)` — 按住指定的虚拟键码
- `KeyUp(vk_code) → (ret)` — 弹起来虚拟键vk_code
- `KeyUpChar(vk_code) → (ret)` — 弹起来虚拟键vk_code
- `WaitKey(vk_code, time_out) → (ret)` — 等待指定的按键按下 (前台,不是后台)
- `KeyPress(vk_code) → (ret)` — 发送字符串
- `KeyPressChar(vk_code) → (ret)` — 按下并弹起指定键（键名描述串，如 "enter"/"f1"/"ctrl+a"）
- `SetKeypadDelay(type, delay) → (ret)` — 设置按键时,键盘按下和弹起的时间间隔
- `KeyPressStr(key_str, delay) → (ret)` — 根据指定的字符串序列，依次按顺序按下其中的字符

## Image 图色（25）

- `Capture(x1, y1, x2, y2, file_name) → (ret)` — 抓取指定区域(x1, y1, x2, y2)的图像, 保存为file
- `CmpColor(x, y, color, sim) → (ret)` — sim 语义注意（继承大漠，各家族不一致，传参前请确认）：
- `FindColor(x1, y1, x2, y2, color, sim, dir) → (x, y, ret)` — 查找指定区域内的颜色
- `FindColorEx(x1, y1, x2, y2, color, sim, dir) → (etstr)` — 查找指定区域内的所有颜色
- `GetColorNum(x1, y1, x2, y2, color, sim) → (ret)` — 查找指定区域内的所有颜色数量
- `FindMultiColor(x1, y1, x2, y2, first_color, offset_color, sim, dir) → (x, y, ret)` — 根据指定的多点查找颜色坐标
- `FindMultiColorEx(x1, y1, x2, y2, first_color, offset_color, sim, dir) → (etstr)` — 根据指定的多点查找所有颜色坐标
- `FindPic(x1, y1, x2, y2, files, delta_color, sim, dir) → (x, y, ret)` — 查找指定区域内的图片
- `FindPicEx(x1, y1, x2, y2, files, delta_color, sim, dir) → (etstr)` — 查找多个图片
- `FindPicExS(x1, y1, x2, y2, files, delta_color, sim, dir) → (etstr)` — 这个函数可以查找多个图片, 并且返回所有找到的图像的坐标.此函数同FindPicEx.只是返回值不同.(file1,x,y|file2,x,y|...)
- `FindColorBlock(x1, y1, x2, y2, color, sim, count, height, width) → (x, y, ret)` — 查找指定区域内的颜色块,颜色格式"RRGGBB-DRDGDB",注意,和按键的颜色格式相反
- `FindColorBlockEx(x1, y1, x2, y2, color, sim, count, height, width) → (etstr)` — 查找指定区域内的所有颜色块, 颜色格式"RRGGBB-DRDGDB", 注意, 和按键的颜色格式相反
- `FindColorBlockExS(x1, y1, x2, y2, color, sim, count, height, width, mode) → (etstr)` — 颜色块增强版：mode=0 raw（同 FindColorBlockEx）；mode=1 重合窗口聚类合并，每个色块只返回一个左上角坐标
- `GetColor(x, y) → (et)` — 获取(x,y)的颜色
- `SetDisplayInput(mode) → (ret)` — 设置图像输入方式，默认窗口截图
- `LoadPic(file_name) → (ret)` — 显式加载图片进全局缓存（FindPic 引用缓存名；源文件变更后须重新 Load 刷新）
- `FreePic(file_name) → (ret)` — 从全局缓存释放指定图片
- `LoadMemPic(file_name, data, size) → (ret)` — 从内存加载要查找的图片
- `GetPicSize(pic_name) → (width, height, ret)` — 获取指定图片的尺寸，如果指定的图片已经被加入缓存，则从缓存中获取信息.此接口也会把此图片加入缓存
- `GetScreenData(x1, y1, x2, y2) → (data, ret)` — 取区域 32 位原始像素内存指针（BGRA 字节序，同 Win32 DIB；生命周期到下一次截图操作前）
- `GetScreenDataBmp(x1, y1, x2, y2) → (data, size, ret)` — 取区域标准 BMP 格式内存数据指针（含文件头）
- `GetScreenFrameInfo() → (frame_id, time)` — 查询最近一帧的序号与时间戳（dx 显示模式调试用）
- `IsDisplayDead(x1, y1, x2, y2, check_times, interval_ms) → (ret)` — 卡屏检测（P2 第二十四批 2026-10-16，对齐 OLA IsDisplayDead）：
- `GetFPS() → (ret)` — 获取绑定窗口当前帧率。采样窗口约 1 秒,按 frameId 增量/经过毫秒换算。
- `MatchPicName(pic_name) → (etstr)` — 宽松匹配缓存中的图片名（可省扩展名/大小写差异），返回匹配到的完整名

## OpenCV 视觉（38）

- `CvLoadTemplate(name, file_path) → (ret)` — 加载模板图片并命名，供 CvMatch* 匹配系按名引用
- `CvLoadMaskedTemplate(name, template_path, mask_path) → (ret)` — 加载带掩码模板：白色=参与匹配区域，黑色=忽略
- `CvRemoveTemplate(name) → (ret)` — 移除并释放指定模板
- `CvRemoveAllTemplates() → (ret)` — 移除全部已加载模板
- `CvHasTemplate(name) → (ret)` — 查询模板是否已加载
- `CvGetTemplateCount() → (ret)` — 已加载模板数量
- `CvGetAllTemplateNames() → (etstr)` — 全部已加载模板名（| 连接）
- `CvGetOpenCvVersion() → (etstr)` — OpenCV 版本串
- `CvLoadTemplateList(template_list) → (ret)` — 批量加载模板："name,path|name,path|..."，任一项缺逗号或空串则整体失败
- `CvToGray(src_file, dst_file) → (ret)` — 转灰度图
- `CvToBinary(src_file, dst_file) → (ret)` — Otsu 自动阈值二值化
- `CvToEdge(src_file, dst_file) → (ret)` — 边缘提取
- `CvToOutline(src_file, dst_file) → (ret)` — 轮廓提取
- `CvDenoise(src_file, dst_file) → (ret)` — 中值滤波去噪
- `CvEqualize(src_file, dst_file) → (ret)` — 直方图均衡
- `CvCLAHE(src_file, dst_file, clip_limit, tile_grid_size) → (ret)` — 对比度受限自适应直方图均衡（clip_limit 典型 2~4，tile_grid_size 典型 8）
- `CvBlur(src_file, dst_file, mode, kernel_size) → (ret)` — 模糊（mode：gaussian/median/bilateral/box）
- `CvSharpen(src_file, dst_file, strength) → (ret)` — 锐化（strength 0~100）
- `CvCropValid(src_file, dst_file) → (ret)` — 自动裁掉纯色边缘
- `CvRotate(src_file, dst_file, angle_deg) → (ret)` — 旋转（角度制，正角逆时针）：90 倍数走无损路径；任意角扩画布白边。ret=1 成功。
- `CvFlip(src_file, dst_file, mode) → (ret)` — 翻转：mode 0 上下 / 1 左右 / 2 双向（=旋转 180°）。ret=1 成功。
- `CvConnectedComponents(src_file, min_area) → (etjson, ret)` — 连通域分析，返回 JSON 数组（每元素含 x/y/w/h/area）
- `CvFindContours(src_file, min_area) → (etjson, ret)` — 轮廓查找，返回 JSON 数组（外接矩形与轮廓点）
- `CvPreprocessPipeline(src_file, dst_file, pipeline) → (ret)` — 预处理流水线：| 分隔多步，步骤可带 "步骤名:参数1,参数2"
- `CvMatchTemplateRot(x, y, width, height, template_name, angles, threshold, method, color_mode) → (etjson, ret)` — 多角度模板匹配：angles 形如 "0|15|-15"（空串走默认角度集）；retjson 额外带命中角度 angle。
- `CvCrop(src_file, x, y, width, height, dst_file) → (ret)` — 裁剪到指定矩形
- `CvResize(src_file, width, height, dst_file) → (ret)` — 缩放到指定宽高
- `CvThreshold(src_file, dst_file, threshold, max_value, mode) → (ret)` — 阈值化（mode：binary/binary_inv/otsu/otsu_inv/adaptive/adaptive_inv）
- `CvInRange(src_file, dst_file, color_space, lower, upper) → (ret)` — 颜色区间过滤（color_space：bgr/hsv/gray，lower/upper 为各通道下限/上限串）
- `CvMorphology(src_file, dst_file, mode, kernel_size, iterations) → (ret)` — 形态学运算（mode：erode/dilate/open/close）
- `CvThin(src_file, dst_file, mode) → (ret)` — 线条细化（mode：zhang_suen/guo_hall/morph）
- `CvMatchTemplate(x, y, width, height, template_name, threshold, dir, strip_mode, method, color_mode) → (etjson, ret)` — 模板匹配，返回命中 JSON 数组 [{x,y,width,height,score},...]
- `CvMatchTemplateScale(x, y, width, height, template_name, scales, threshold, method, color_mode) → (etjson, ret)` — 多缩放比模板匹配（scales 逗号分隔，如 "0.8,0.9,1.0,1.1"），结果含实际 scale
- `CvMatchAnyTemplate(x, y, width, height, template_names, threshold, dir, strip_mode, method, color_mode) → (etjson, ret)` — 多模板任一命中即返回首个
- `CvMatchAllTemplates(x, y, width, height, template_names, threshold, dir, strip_mode, method, color_mode) → (etjson, ret)` — 多模板全部各搜一遍，结果含模板名
- `CvFeatureMatchTemplate(x, y, width, height, template_name, threshold) → (etjson, ret)` — 特征点匹配，返回命中 JSON 数组
- `CvEdgeMatchTemplate(x, y, width, height, template_name, threshold) → (etjson, ret)` — 边缘匹配，返回命中 JSON 数组
- `CvShapeMatchTemplate(x, y, width, height, template_name, threshold) → (etjson, ret)` — 形状匹配，返回命中 JSON 数组

## OCR 文字识别（81）

- `SetOcrEngine(path_of_engine, dll_name, argv) → ret` — 选择 OCR 引擎并透传启动参数。engine: "onnx"/空/任意未知名=内置 ONNX（默认）。
- `OcrWarmup() → (ret)` — OCR 预热（P2 第二十六批 2026-10-16，对齐 OLA OcrWarmup）：触发引擎懒加载 + 一次小图推理，
- `SetDict(idx, file_name) → (ret)` — 设置字库文件
- `GetDict(idx, font_index) → (etstr)` — 获取指定字库中指定条目的字库信息
- `SetMemDict(idx, data, size) → (ret)` — 设置内存字库文件
- `UseDict(idx) → (ret)` — 使用哪个字库文件进行识别
- `AddDict(idx, dict_info) → (ret)` — 给指定的字库中添加一条字库信息
- `SaveDict(idx, file_name) → (ret)` — 保存指定的字库到指定的文件中
- `ClearDict(idx) → (ret)` — 清空指定的字库
- `GetDictCount(idx) → (ret)` — 获取指定的字库中的字符数量
- `GetNowDict() → (ret)` — 获取当前使用的字库序号
- `SetBinaryPreprocess(mode, isolated_threshold, min_component_area, bridge_gap) → (ret)` — 设置点阵二值图预处理。mode: 0关闭, 1去孤立点, 2再过滤小连通域, 3允许按 bridge_gap 补1像素断笔
- `GetBinaryPreprocess() → (mode, isolated_threshold, min_component_area, bridge_gap, ret)` — 读取当前点阵二值图预处理参数
- `FetchWord(x1, y1, x2, y2, color, word) → (etstr)` — 根据指定的范围,以及指定的颜色描述，提取点阵信息，类似于大漠工具里的单独提取
- `FetchWordEx(x1, y1, x2, y2, color, sim, word) → (etstr)` — 根据相似度提取单个点阵字，适合有抗锯齿或轻微色差的文字
- `ExtractWordRects(x1, y1, x2, y2, color, sim, min_word_h) → (etstr)` — 自动切出指定范围内的点阵字块，返回格式: x1,y1,x2,y2|...
- `ExtractWordRectsEx(x1, y1, x2, y2, color, sim, min_word_w, min_word_h, padding) → (etstr)` — 自动切出点阵字块，并按最小宽高过滤噪点；padding 用来给每个字块保留一点边缘
- `FetchWords(x1, y1, x2, y2, color, sim, words, min_word_h) → (etstr)` — 自动切字并按 words 的字符顺序生成多条点阵字库，返回多行字库文本
- `FetchWordsEx(x1, y1, x2, y2, color, sim, words, min_word_w, min_word_h, padding) → (etstr)` — 使用更细的切字参数批量生成点阵字库，适合截图里夹杂小噪点的情况
- `FetchWordsByRects(x1, y1, x2, y2, color, sim, words, rects) → (etstr)` — 按指定字块生成多条点阵字库，rects 格式与 ExtractWordRects 的返回值一致
- `GetBinaryPreview(x1, y1, x2, y2, color, sim) → (etstr, ret)` — 返回指定区域按颜色规则二值化后的文本预览，ret 为前景点数量。
- `GetWordPreview(dict_info) → (etstr, ret)` — 返回点阵字库条目的文本预览，ret 为条目是否合法
- `CheckWordDict(dict_info) → (etstr, ret)` — 检查一段点阵字库文本，ret 为合法条目数量，retstr 为每条字库的摘要
- `NormalizeWordDict(dict_info) → (etstr, ret)` — 清理点阵字库文本，只保留合法条目，并统一输出为当前字库格式
- `RenameWordDict(dict_info, words) → (etstr, ret)` — 按顺序替换点阵字库条目的字名，适合制作后统一校正标签
- `GetWordsNoDict(x1, y1, x2, y2, color) → (etstr)` — 识别这个范围内所有满足条件的词组，这个识别函数不会用到字库. 只是识别大概形状的位置
- `GetWordResultCount(result) → (ret)` — 在使用GetWords进行词组识别以后,可以用此接口进行识别词组数量的计算
- `GetWordResultPos(result, index) → (x, y, ret)` — 在使用GetWords进行词组识别以后,可以用此接口进行识别各个词组的坐标
- `GetWordResultStr(result, index) → (et_str)` — 在使用GetWords进行词组识别以后,可以用此接口进行识别各个词组的内容
- `AutoOcr(x1, y1, x2, y2, color, sim) → (et_str)` — 免字库 OCR 家族速查（免字库 = 未 SetDict/UseDict，走内置 ONNX 引擎；有字库 = 点阵字库匹配）：
- `Ocr(x1, y1, x2, y2, color, sim) → (et_str)` — 识别屏幕范围(x1,y1,x2,y2)内符合color_format的字符串,并且相似度为sim,sim取值范围(0.1-1.0),
- `OcrEx(x1, y1, x2, y2, color, sim) → (et_str)` — 同 Ocr，但返回识别到的字符串以及每个字符的坐标（已装载字库时结果按阅读顺序拼接：包围盒纵向重叠聚类成行、行内按 x 排序）
- `OcrAutoColor(x1, y1, x2, y2, color, sim) → (et_str)` — 区域截图后按颜色二值化再做免字库 OCR：输入区域、颜色、相似度（置信度阈值，默认 0.7）。
- `OcrAutoColorLine(x1, y1, x2, y2, color, sim) → (et_str)` — 单行快模式 OcrAutoColor：颜色二值化后整图直接 rec（跳过检测），适用于读出区/固定单行文本。
- `OcrAutoColorEx(x1, y1, x2, y2, color, sim) → (et_str)` — 结构化 OcrAutoColor：输出 "x1,y1,x2,y2,conf,text|..."（bbox 为客户区坐标，相对绑定窗口客户区左上角），返回命中行数。
- `FindStr(x1, y1, x2, y2, strs, color, sim) → (ret, ret, ret)` — 在屏幕范围(x1,y1,x2,y2)内,查找string(可以是任意个字符串的组合),并返回符合color_format的坐标位置
- `FindStrEx(x1, y1, x2, y2, strs, color, sim) → (etstr)` — 返回符合color_format的所有坐标位置
- `OcrAuto(x1, y1, x2, y2, sim) → (et_str)` — 识别屏幕范围(x1,y1,x2,y2)内的字符串，无需指定颜色。
- `OcrFromFile(file_name, color_format, sim) → (etstr)` — 从文件中识别图片，规则同 Ocr
- `OcrAutoColorFromFile(file_name, color_format, sim) → (etstr)` — 文件版 OcrAutoColor：读取图片后按颜色二值化再做免字库 OCR。
- `OcrAutoFromFile(file_name, sim) → (etstr)` — 从文件中识别图片，无需指定颜色，规则同 OcrAuto
- `FindLine(x1, y1, x2, y2, color, sim) → (etstr)` — 查找频幕中的直线
- `FindLineEx(x1, y1, x2, y2, color, sim) → (etstr, ret)` — 查找屏幕中的直线(扩展版): 额外返回该直线上的点数(霍夫累加器峰值)。
- `FindLineExS(x1, y1, x2, y2, color, sim, min_points) → (etstr, ret)` — 查找直线(增强版): 二值化走孤立点去噪 + min_points 阈值。
- `GetModuleSize(hwnd, module) → (ret)` — 模块大小（字节），失败/未找到返回 0。module 为模块基名（大小写不敏感）。
- `GetModuleInfoList(hwnd) → (etstr)` — 模块列表 JSON：{"ok":1,"modules":[{"name":..,"path":..,"base":"HEX","size":n},...]}。
- `VirtualProtectEx(hwnd, address, size, new_protect) → (old_protect, ret)` — 修改地址保护属性，ret=1 成功 0 失败，old_protect 带回原属性（PAGE_* 常量）。
- `VirtualAllocEx(hwnd, address, size, alloc_type, protect) → (etstr)` — 在目标进程分配内存，返回分配地址大写十六进制串；失败返回空。
- `FileExists(file_name) → (ret)` — 全部路径为宽字符绝对/相对路径；返回值约定与窗口系一致：ret=1 成功，0 失败（路径非法/不存在/拒绝访问）。
- `CreateFolder(folder_name) → (ret)` — 创建文件夹（含多级父目录，已存在视为成功）
- `DeleteFolder(folder_name) → (ret)` — 递归删除文件夹及其全部内容；路径非法/不存在返回 0
- `GetFileList(folder, wildcard) → (etstr)` — 遍历文件夹，wildcard 支持 * ?（空=全部）。返回全路径 "p1|p2|..."，无匹配返回空串。
- `CopyFile(src_file, dst_file) → (ret)` — 复制文件（覆盖目标）
- `MoveFile(src_file, dst_file) → (ret)` — 移动/重命名文件
- `DeleteFile(file_name) → (ret)` — 删除文件
- `GetFileSize(file_name) → (size, ret)` — 文件大小（字节，int64）。ret=1 成功，0 失败。
- `ReadFileString(file_name) → (etstr)` — 读取文本文件全部内容（按 UTF-8 解码；BOM 自动剔除）。失败/空文件返回空串。
- `WriteStringToFile(file_name, content) → (ret)` — 写入文本文件（UTF-8 带 BOM，覆盖写）。ret=1 成功。
- `GetFileName(file_path, flag) → (etstr)` — 路径解析：flag 0=文件名含扩展名 1=仅文件名 2=仅扩展名(含.) 3=目录部分
- `ToAbsolutePath(path) → (etstr)` — 相对路径转绝对路径（不做存在性检查，仅规范化 .\ ..\ 与斜杠）
- `KeyPressHotkey(combo) → (ret)` — 发送组合键，combo 形如 "ctrl+alt+a" / "shift+f1" / "win+d"。修饰键支持 ctrl/control/alt/shift/win，主键支持
- `GetWindowScale(hwnd) → (etstr)` — 取窗口实际缩放系数（1.00=96DPI 无缩放），两位小数字符串如 "1.25"；失败返回空串。
- `TerminateProcess(pid) → (ret)` — 按 PID 终止进程（不限树）；TerminateProcessTree 先父后子递归终止整棵进程树。
- `TerminateProcessTree(pid) → (ret)` — 结束进程及其整棵子进程树
- `DecodeQRCode(x1, y1, x2, y2) → (etstr)` — 识别绑定窗口客户区区域内二维码文本，未识别返回空串
- `DecodeQRCodeFromFile(file_name) → (etstr)` — 识别图片文件中的二维码文本
- `CreateQRCode(text, file_name, size) → (ret)` — 生成二维码保存为图片（PNG/BMP 由扩展名决定），size 为边长像素（<=0 用 256）。ret=1 成功。
- `JsonIsValid(json_text) → ret` — 无句柄路径直取式 JSON 族：所有函数收 JSON 文本 + 路径（"a.b[0].c"，兼容 "a.b.0"），
- `JsonGetString(json_text, path) → (etstr, ret)` — 取路径处的字符串值；类型不符/不存在返回空串且 ret=0。
- `JsonGetNumber(json_text, path) → (value, ret)` — 取路径处的数值；类型不符/不存在 ret=0。
- `JsonGetBool(json_text, path) → (value, ret)` — 取路径处的布尔值（value 0/1）；类型不符/不存在 ret=0。
- `JsonGetSize(json_text, path) → ret` — 数组长度/对象键数；非集合或不存在返回 -1。
- `JsonGetKeys(json_text, path) → (etstr, ret)` — 对象键名序列化为 JSON 数组文本（如 ["a","b"]）；非对象/不存在返回空串且 ret=0。
- `JsonSetString(json_text, path, value) → (etstr, ret)` — 写入路径（不存在自动创建中间对象/数组），返回新 JSON 串；失败返回空串。
- `JsonSetNumber(json_text, path, value) → (etstr, ret)` — 向 JSON 文本路径处写入数值，返回新 JSON 串；父路径不可达返回空串
- `JsonSetBool(json_text, path, value) → (etstr, ret)` — 向 JSON 文本路径处写入布尔值，返回新 JSON 串；父路径不可达返回空串
- `JsonAppend(json_text, path, raw_json) → (etstr, ret)` — 数组追加元素（raw_json 为元素文本，如 "abc"/123/{"k":1}；路径不存在先建数组）。
- `JsonDelete(json_text, path) → (etstr, ret)` — 删除对象键或数组元素，返回新 JSON 串；父路径不可达返回空串。
- `HttpGet(url, headers_json, timeout_ms) → (etstr, ret)` — 系统 WinHTTP 直连（零第三方依赖），用途：脚本对接大模型等 OpenAI 兼容 API——
- `HttpPost(url, body, headers_json, timeout_ms) → (etstr, ret)` — POST 请求（系统 WinHTTP 直连，UTF-8；headers_json 放 Content-Type）；非 2xx 也返回响应体，传输失败空串
- `HttpDownloadFile(url, file_name, timeout_ms) → (ret)` — 下载到文件（原始字节流式落盘）。parent 目录须已存在。ret=1 成功。

## YOLO 目标检测（3）

- `SetYoloEngine(path_of_engine, dll_name, argv) → ret` — 选择 YOLO 引擎并加载模型。统一入口：path_of_engine 直接传 .onnx 模型文件路径
- `YoloDetect(x1, y1, x2, y2, conf, iou) → (etjson, ret)` — 在区域运行 YOLO 目标检测，返回 JSON 数组 [{class,score,x,y,w,h},...]
- `YoloDetectFromFile(file_name, conf, iou) → (etjson, ret)` — 对图片文件运行 YOLO 检测，返回同 YoloDetect

## Algorithm 算法（13）

- `AStarFindPath(mapWidth, mapHeight, disable_points, beginX, beginY, endX, endY) → (et)` — A星算法。disable_points: "x,y|x,y" 障碍点列表（越界点忽略）；返回路径 "x,y|x,y"（起点→终点），不可达返回空串。
- `FindNearestPos(all_pos, type, x, y) → (et)` — 从坐标列表中查找距离指定坐标最近的点。all_pos: type=1 时 "x,y|x,y"；否则 "name,x,y|..."（name 不含空格/逗号）。
- `SortPosDistance(all_pos, type, x, y) → (et)` — 坐标点按与 (x,y) 的距离升序排序（欧氏距离平方比较）。格式与 FindNearestPos 相同，输出保持输入格式，等距保持输入顺序。
- `SetAStarMap(bitmap_file, scale, offset_x, offset_y) → (ret)` — 设置寻路障碍地图（位图文件，黑=障碍：亮度 (r+g+b)/3<128）。scale 降采样倍率（块内任一障碍像素则整格障碍）。
- `SetAStarMapData(width, height, bgra_data, size, scale, offset_x, offset_y) → (ret)` — 内存版障碍地图（32bpp BGRA top-down）。C API 专用。
- `AStarFindPathBM(beginX, beginY, endX, endY) → (et)` — 障碍位图 A*：对已设置地图寻路，输出世界坐标路径 "x,y|x,y"（起点→终点，格中心），不可达/未设图返回空串。
- `AStarFindPathWay(points) → (et)` — 多点途经寻路："x,y|x,y|..." 首点为起点，逐段寻路拼接（衔接点去重）；任一段不可达整体返回空串。
- `FindNearestFeasiblePoint(x, y) → (et)` — 找距离 (x,y) 最近的格中心可通行坐标（世界坐标 "x,y"）：落障碍格时从该格 BFS 扩散找最近可走格。
- `SmoothPathByLOS(path) → (et)` — 视线拉直：贪心合并可直视的相邻路径点（需已设置障碍地图），路径更自然；未设图原样返回。
- `SimplifyPath(path, epsilon) → (et)` — RDP 路径抽稀：epsilon 为最大垂直偏差（世界坐标单位），<=0 原样返回。
- `IsLineBlocked(x1, y1, x2, y2) → (ret)` — 两点视线遮挡判定（世界坐标，需已设置障碍地图）：1=被挡 0=通 -1=未设置地图。
- `FindNearestPathPoint(path, x, y) → (index, nx, ny, ret)` — 点到路径最近点：输出 0 基索引与最近点坐标；path 非法/为空返回 0。
- `PointInPolygon(point, polygon) → (ret)` — 点在多边形内判定（射线法）。point="x,y"；polygon="x,y|x,y|..."（>=3 顶点）：1=在内 0=在外/非法。

## Memory 内存（13）

- `WriteData(hwnd, address, data, size) → (ret)` — 内存读写（大漠兼容）。hwnd=0 时优先作用于已绑定窗口；未绑定才是本进程，
- `ReadData(hwnd, address, size) → (etstr)` — 读取数据（大写十六进制串；失败返回空串。size 超 data 实际字节时补零，详见日志）
- `ReadInt(hwnd, address, type) → (ret)` — 大漠兼容内存接口
- `WriteInt(hwnd, address, type, value) → (ret)` — 向内存地址写整数（type 选位宽：同 ReadInt）
- `ReadFloat(hwnd, address) → (ret)` — 读内存 float
- `WriteFloat(hwnd, address, value) → (ret)` — 写内存 float
- `ReadDouble(hwnd, address) → (ret)` — 读内存 double
- `WriteDouble(hwnd, address, value) → (ret)` — 写内存 double
- `ReadString(hwnd, address, type, len) → (etstr)` — 读内存字符串（type：0=ANSI 1=Unicode；len 字符数，超限截断）
- `WriteString(hwnd, address, type, value) → (ret)` — 写内存字符串（type 同 ReadString）
- `FindData(hwnd, addr_range, string) → (etstr)` — 特征码搜索内存：pattern 十六进制串支持 ?? 通配；range "start-end" 十六进制，空=全部已提交可读区。
- `FindDataEx(hwnd, addr_range, string, step, count) → (etstr)` — 特征码搜索加强版：步进与最大返回数可控（step=1 逐字节最准最慢）
- `GetModuleBaseAddr(hwnd, module) → (etstr)` — 取目标进程（hwnd=0 为本进程）模块基址，返回大写十六进制串；失败返回空。
