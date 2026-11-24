// op::Op的声明
/*
所有op的开放接口都从此cpp类衍生而出

并发约定：单个 Op 实例不是线程安全的（图色/OCR 复用实例内二值图与源图缓冲，
多线程同时调用同一实例会互相污染结果）。多线程场景请每线程一个实例；
不同实例之间互不影响（引擎级单例内部有锁）。与大漠插件的单线程语义一致。
*/
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include <BaseTsd.h>
#ifdef U_STATIC_IMPLEMENTATION
#define OP_API
#else
#ifndef OP_API
#if defined(OP_EXPORTS)
#define OP_API __declspec(dllexport)
#else
#define OP_API __declspec(dllimport)
#endif
#endif
#endif
// op::Op
#undef FindWindow
#undef FindWindowEx
#undef SetWindowText
// 文件模块成员名与 Win32 宏冲突（CopyFileW/MoveFileW/DeleteFileW），一并取消映射
#undef CopyFile
#undef MoveFile
#undef DeleteFile
// DrawText 宏同理（UNICODE 下映射 DrawTextW）
#undef DrawText

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4251)
#endif

// C API 的 handle 类型（定义在 libop/c_api/op_c_api.cpp）。
// 这里前置声明，供 class Op 的 friend 使用：OpConcurrencyInfo 与"同实例并发"告警
// 需要读 OpContext 里的计数，而那只应由 C API 门面可见。
struct op_c_context;

namespace op {

namespace internal {
struct OpContext;
}

// 监听回调看到的事件（时间轴层，批 E 2026-11-22）。
// 独立于内部实现结构：本头文件是**公开 API**，不能把 libop/input/InputEvent.h
// 拖进来（那会把内部目录结构变成 ABI 的一部分，且 C API/COM 侧也要用同一份布局）。
// 字段顺序固定，改动即为 ABI 变更。
struct op_input_event_view {
    double t; // 相对录制起点的毫秒数（未录制时为 0）
    int type; // 0=mouse_move 1=mouse_button 2=mouse_wheel 3=key 4=key_char
    long vk;
    long x; // 客户区坐标；key_char 事件里复用为字符码
    long y;
    long wheel;
    long flags; // MOUSEEVENTF_* / KEYEVENTF_*，见 InputRecord 上方说明
};

class OP_API Op {

  public:
    Op();
    ~Op();
    // 复制构造
    Op(Op const &) = delete;
    Op &operator=(Op const &) = delete;

    using RuntimeService = Op;
    using WindowService = Op;
    using BindingService = Op;
    using InputService = Op;
    using ImageService = Op;
    using OcrService = Op;
    using OpenCvService = Op;
    using YoloService = Op;
    using MemoryService = Op;

    RuntimeService &runtime() noexcept {
        return *this;
    }
    WindowService &window() noexcept {
        return *this;
    }
    BindingService &binding() noexcept {
        return *this;
    }
    InputService &input() noexcept {
        return *this;
    }
    ImageService &image() noexcept {
        return *this;
    }
    OcrService &ocr() noexcept {
        return *this;
    }
    OpenCvService &opencv() noexcept {
        return *this;
    }
    YoloService &yolo() noexcept {
        return *this;
    }
    MemoryService &memory() noexcept {
        return *this;
    }

  private:
    // C API 门面需要读取 OpContext 里的并发计数（in_flight / peak_in_flight / ...）来
    // 实现 OpConcurrencyInfo 与"同实例并发"告警。只授予这一个实现文件，不对外开接口。
    friend struct ::op_c_context;
    std::unique_ptr<internal::OpContext> m_context;
    static std::atomic<int> s_id;

  public:
    // 以下 public 接口的参数需标注 SAL 2.0（_In_ / _Out_ / _Inout_）
    //---------------基本设置/属性-------------------

    // 1.版本号Version
    std::wstring Ver();
    // 设置目录
    void SetPath(_In_ const wchar_t *path, _Out_ long *ret);
    // 获取目录
    void GetPath(_Out_ std::wstring &ret);
    // 获取插件目录
    void GetBasePath(_Out_ std::wstring &ret);
    // 返回当前对象的ID值，这个值对于每个对象是唯一存在的。可以用来判定两个对象是否一致
    void GetID(_Out_ long *ret);
    //
    void GetLastError(_Out_ long *ret);
    // 设置是否弹出错误信息,默认是打开 0:关闭，1:显示为信息框，2:保存到文件,3:输出到标准输出
    void SetShowErrorMsg(_In_ long show_type, _Out_ long *ret);

    // sleep
    void Sleep(_In_ long millseconds, _Out_ long *ret);
    // Process
    // inject dll
    void InjectDll(_In_ const wchar_t *process_name, _In_ const wchar_t *dll_name, _Out_ long *ret);
    // 设置是否开启或者关闭插件内部的图片缓存机制
    void EnablePicCache(_In_ long enable, _Out_ long *ret);
    // 清空进程级图片缓存（FindPic 自动缓存与显式加载共用）
    void ClearPicCache(_Out_ long *ret);
    // 设置进程级图片缓存条目上限；max_count<=0 表示不设上限，默认 500，满时整体清空重存
    void SetPicCacheMax(_In_ long max_count, _Out_ long *ret);
    // 取上次操作的图色区域，保存为file(24位位图)
    void CapturePre(_In_ const wchar_t *file_name, _Out_ long *ret);
    // 设置屏幕数据模式，0:从上到下(默认),1:从下到上
    void SetScreenDataMode(_In_ long mode, _Out_ long *ret);
    //---------------------algorithm-------------------------------
    // A星算法。disable_points: "x,y|x,y" 障碍点列表（越界点忽略）；返回路径 "x,y|x,y"（起点→终点），不可达返回空串。
    void AStarFindPath(_In_ long mapWidth, _In_ long mapHeight, _In_ const wchar_t *disable_points, _In_ long beginX,
                       _In_ long beginY, _In_ long endX, _In_ long endY, _Out_ std::wstring &ret);
    // 从坐标列表中查找距离指定坐标最近的点。all_pos: type=1 时 "x,y|x,y"；否则 "name,x,y|..."（name 不含空格/逗号）。
    // 返回 "x,y"（type=1）或 "name,x,y"；列表无有效项返回空串；等距时取先出现的。
    void FindNearestPos(_In_ const wchar_t *all_pos, _In_ long type, _In_ long x, _In_ long y, _Out_ std::wstring &ret);
    // 坐标点按与 (x,y) 的距离升序排序（欧氏距离平方比较）。格式与 FindNearestPos 相同，输出保持输入格式，等距保持输入顺序。
    void SortPosDistance(_In_ const wchar_t *all_pos, _In_ long type, _In_ long x, _In_ long y, _Out_ std::wstring &ret);
    // 排除落在指定矩形内的坐标点。exclude_rects="x1,y1,x2,y2|..."（含边界，坐标顺序不限，自动归一化；
    // 非法项跳过）。输出保持输入格式；exclude_rects 为空时原样返回；ret 为保留点数。
    void ExcludePos(_In_ const wchar_t *all_pos, _In_ long type, _In_ const wchar_t *exclude_rects,
                    _Out_ std::wstring &retstr, _Out_ long *ret);
    // 找包含坐标点最多的矩形窗口（窗口大小固定 width x height，左上角取某个输入点；
// 右/下边界为开区间 [x,x+width) × [y,y+height)）。
    // 输出 "x,y,count"（窗口左上角与世界坐标点数）；无有效点或宽高<=0 返回空串且 ret=0；并列取最靠上再靠左。
    void GetDenseRect(_In_ const wchar_t *all_pos, _In_ long type, _In_ long width, _In_ long height,
                      _Out_ std::wstring &retstr, _Out_ long *ret);
    // 设置寻路障碍地图（位图文件，黑=障碍：亮度 (r+g+b)/3<128）。scale 降采样倍率（块内任一障碍像素则整格障碍）。
    // offset_x/offset_y：位图 (0,0) 对应的世界坐标（支持负值）。file 为空串清空地图。
    void SetAStarMap(_In_ const wchar_t *bitmap_file, _In_ long scale, _In_ long offset_x, _In_ long offset_y, _Out_ long *ret);
    // 内存版障碍地图（32bpp BGRA top-down）。C API(=OpSetAStarMapData) 与 COM
    // (=SetAStarMapData, DISPID 478, SAFEARRAY(BYTE)) 均可用。
    void SetAStarMapData(_In_ long width, _In_ long height, _In_ void *bgra_data, _In_ long size, _In_ long scale,
                         _In_ long offset_x, _In_ long offset_y, _Out_ long *ret);
    // 障碍位图 A*：对已设置地图寻路，输出世界坐标路径 "x,y|x,y"（起点→终点，格中心），不可达/未设图返回空串。
    void AStarFindPathBM(_In_ long beginX, _In_ long beginY, _In_ long endX, _In_ long endY, _Out_ std::wstring &ret);
    // 多点途经寻路："x,y|x,y|..." 首点为起点，逐段寻路拼接（衔接点去重）；任一段不可达整体返回空串。
    void AStarFindPathWay(_In_ const wchar_t *points, _Out_ std::wstring &ret);
    // 找距离 (x,y) 最近的格中心可通行坐标（世界坐标 "x,y"）：落障碍格时从该格 BFS 扩散找最近可走格。
    // 未设地图/全图皆墙返回空串。欧拉 FindNearestFeasiblePoint 对等。
    void FindNearestFeasiblePoint(_In_ long x, _In_ long y, _Out_ std::wstring &ret);
    // 视线拉直：贪心合并可直视的相邻路径点（需已设置障碍地图），路径更自然；未设图原样返回。
    void SmoothPathByLOS(_In_ const wchar_t *path, _Out_ std::wstring &ret);
    // RDP 路径抽稀：epsilon 为最大垂直偏差（世界坐标单位），<=0 原样返回。
    void SimplifyPath(_In_ const wchar_t *path, _In_ double epsilon, _Out_ std::wstring &ret);
    // 两点视线遮挡判定（世界坐标，需已设置障碍地图）：1=被挡 0=通 -1=未设置地图。
    void IsLineBlocked(_In_ long x1, _In_ long y1, _In_ long x2, _In_ long y2, _Out_ long *ret);
    // 点到路径最近点：输出 0 基索引与最近点坐标；path 非法/为空返回 0。
    void FindNearestPathPoint(_In_ const wchar_t *path, _In_ long x, _In_ long y, _Out_ long *index, _Out_ long *nx,
                              _Out_ long *ny, _Out_ long *ret);
    // 点在多边形内判定（射线法）。point="x,y"；polygon="x,y|x,y|..."（>=3 顶点）：1=在内 0=在外/非法。
    void PointInPolygon(_In_ const wchar_t *point, _In_ const wchar_t *polygon, _Out_ long *ret);
    //--------------------windows api------------------------------
    // 根据指定条件,枚举系统中符合条件的窗口
    void EnumWindow(_In_ LONG_PTR parent, _In_ const wchar_t *title, _In_ const wchar_t *class_name, _In_ long filter,
                    _Out_ std::wstring &ret);
    // 根据指定进程以及其它条件,枚举系统中符合条件的窗口
    void EnumWindowByProcess(_In_ const wchar_t *process_name, _In_ const wchar_t *title, _In_ const wchar_t *class_name,
                             _In_ long filter, _Out_ std::wstring &ret);
    // 根据指定进程名,枚举系统中符合条件的进程PID
    void EnumProcess(_In_ const wchar_t *name, _Out_ std::wstring &ret);
    // 把窗口客户区坐标转换为屏幕坐标。输入按「截帧口径」客户区坐标解释，输出为宿主视角的
    // 屏幕坐标（DPI-aware 宿主下即物理像素），可直接用于屏幕框选/取色；与 ScreenToClient 互逆。
    void ClientToScreen(_In_ LONG_PTR hwnd, _Inout_ long *x, _Inout_ long *y, _Out_ long *bret);
    // 查找符合类名或者标题名的顶层可见窗口
    void FindWindow(_In_ const wchar_t *class_name, _In_ const wchar_t *title, _Out_ LONG_PTR *ret);
    // 根据指定的进程名字，来查找可见窗口
    void FindWindowByProcess(_In_ const wchar_t *process_name, _In_ const wchar_t *class_name, _In_ const wchar_t *title,
                             _Out_ LONG_PTR *ret);
    // 根据指定的进程Id，来查找可见窗口
    void FindWindowByProcessId(_In_ long process_id, _In_ const wchar_t *class_name, _In_ const wchar_t *title,
                               _Out_ LONG_PTR *ret);
    // 查找符合类名或者标题名的顶层可见窗口,如果指定了parent,则在parent的第一层子窗口中查找
    void FindWindowEx(_In_ LONG_PTR parent, _In_ const wchar_t *class_name, _In_ const wchar_t *title,
                      _Out_ LONG_PTR *ret);
    // 获取窗口客户区域在屏幕上的位置。坐标与尺寸均为「截帧口径」：目标窗口 DPI 不感知时
    // 返回其逻辑像素（与 Capture 出的图尺寸一致），而不是屏幕物理像素 —— 150% 缩放下两者
    // 相差 1.5 倍。区域类 API（Capture/FindPic/Ocr）的坐标系与此完全一致。
    void GetClientRect(_In_ LONG_PTR hwnd, _Out_ long *x1, _Out_ long *y1, _Out_ long *x2, _Out_ long *y2,
                       _Out_ long *ret);
    // 获取窗口客户区域的宽度和高度（截帧口径，与 Capture 出的图尺寸相同）
    void GetClientSize(_In_ LONG_PTR hwnd, _Out_ long *width, _Out_ long *height, _Out_ long *ret);
    // 获取顶层活动窗口中具有输入焦点的窗口句柄
    void GetForegroundFocus(_Out_ LONG_PTR *ret);
    // 获取顶层活动窗口,可以获取到按键自带插件无法获取到的句柄
    void GetForegroundWindow(_Out_ LONG_PTR *ret);
    // 获取鼠标指向的可见窗口句柄
    void GetMousePointWindow(_Out_ LONG_PTR *ret);
    // 获取给定坐标的可见窗口句柄
    void GetPointWindow(_In_ long x, _In_ long y, _Out_ LONG_PTR *ret);
    // 根据指定的pid获取进程详细信息
    void GetProcessInfo(_In_ long pid, _Out_ std::wstring &ret);
    // 获取特殊窗口
    void GetSpecialWindow(_In_ long flag, _Out_ LONG_PTR *ret);
    // 获取给定窗口相关的窗口句柄
    void GetWindow(_In_ LONG_PTR hwnd, _In_ long flag, _Out_ LONG_PTR *ret);
    // 获取窗口的类名
    void GetWindowClass(_In_ LONG_PTR hwnd, _Out_ std::wstring &ret);
    // 获取指定窗口所在的进程ID
    void GetWindowProcessId(_In_ LONG_PTR hwnd, _Out_ long *ret);
    // 获取指定窗口所在的进程的exe文件全路径
    void GetWindowProcessPath(_In_ LONG_PTR hwnd, _Out_ std::wstring &ret);
    // 获取窗口在屏幕上的位置
    void GetWindowRect(_In_ LONG_PTR hwnd, _Out_ long *x1, _Out_ long *y1, _Out_ long *x2, _Out_ long *y2,
                       _Out_ long *ret);
    // 获取指定窗口的一些属性
    void GetWindowState(_In_ LONG_PTR hwnd, _In_ long flag, _Out_ long *ret);
    // 获取窗口的标题
    void GetWindowTitle(_In_ LONG_PTR hwnd, _Out_ std::wstring &rettitle);
    // 移动指定窗口到指定位置
    void MoveWindow(_In_ LONG_PTR hwnd, _In_ long x, _In_ long y, _Out_ long *ret);
    // 把屏幕坐标转换为窗口客户区坐标。输入按宿主视角的屏幕坐标（DPI-aware 宿主下为物理
    // 像素）解释，输出为「截帧口径」的客户区坐标，可直接作为 Capture/FindPic/Ocr 的区域参数。
    // 框选回填务必走本接口：自己减客户区原点会漏掉 DPI 比例，区域整体偏大 1.5 倍。
    void ScreenToClient(_In_ LONG_PTR hwnd, _Inout_ long *x, _Inout_ long *y, _Out_ long *ret);
    // 向指定窗口发送粘贴命令
    void SendPaste(_In_ LONG_PTR hwnd, _Out_ long *ret);
    // 设置窗口客户区域的宽度和高度
    void SetClientSize(_In_ LONG_PTR hwnd, _In_ long width, _In_ long hight, _Out_ long *ret);
    // 设置窗口的状态
    void SetWindowState(_In_ LONG_PTR hwnd, _In_ long flag, _Out_ long *ret);
    // 设置窗口的大小
    void SetWindowSize(_In_ LONG_PTR hwnd, _In_ long width, _In_ long height, _Out_ long *ret);
    // 按指定布局批量排列多个窗口。hwnds 格式例如 "123|456|789"（正整数 hwnd 竖线分隔，含 0/非法项整体拒绝）。
    // layout_type: 0=宫格 1=对角线 2=层叠（层叠时 gap_x/gap_y 语义为层叠步距，经典值 32）。
    // 失败返回 0，原因经 set_show_error_msg(2) 落 __op.log；任一窗口失败时前面已排列的窗口保持不动。
    void LayoutWindows(_In_ const wchar_t *hwnds, _In_ long layout_type, _In_ long columns, _In_ long start_x,
                       _In_ long start_y, _In_ long gap_x, _In_ long gap_y, _In_ long size_mode, _In_ long window_width,
                       _In_ long window_height, _In_ long anchor_mode, _Out_ long *ret);
    // 设置窗口的标题
    void SetWindowText(_In_ LONG_PTR hwnd, _In_ const wchar_t *title, _Out_ long *ret);
    // 设置窗口的透明度
    void SetWindowTransparent(_In_ LONG_PTR hwnd, _In_ long trans, _Out_ long *ret);
    // 向指定窗口发送文本数据
    void SendString(_In_ LONG_PTR hwnd, _In_ const wchar_t *str, _Out_ long *ret);
    // 向指定窗口发送文本数据-输入法
    void SendStringIme(_In_ LONG_PTR hwnd, _In_ const wchar_t *str, _Out_ long *ret);
    // 锁定/解锁窗口位置:锁定后外部移动该窗口会被守护线程自动拉回(50ms 轮询),解锁后停止
    void LockWindowPosition(_In_ LONG_PTR hwnd, _In_ long enable, _Out_ long *ret);
    // 锁定/解锁窗口尺寸:锁定后外部缩放该窗口会被自动拉回
    void LockWindowSize(_In_ LONG_PTR hwnd, _In_ long enable, _Out_ long *ret);
    // 禁止/恢复窗口的最大最小化按钮(修改 GWL_STYLE,恢复时还原锁定前的原始样式)
    void DisableMinMax(_In_ LONG_PTR hwnd, _In_ long enable, _Out_ long *ret);
    // 设置指定窗口的输入法开关:enable=0 关闭输入,1 恢复
    void SetIme(_In_ LONG_PTR hwnd, _In_ long enable, _Out_ long *ret);
    // 运行可执行文件/快捷方式/文档/网址。mode：
    //   0 = ShellExecute 启动（默认，对齐大漠），工作目录 = 继承调用方
    //   1 = ShellExecute 启动，工作目录 = 程序所在目录（带自校验的启动器类游戏须用此值）
    //   2 = CreateProcess 启动，工作目录 = 继承调用方
    //   3 = CreateProcess 启动，工作目录 = 程序所在目录
    void RunApp(_In_ const wchar_t *cmdline, _In_ long mode, _Out_ unsigned long *pid, _Out_ long *ret);
    // 运行可执行文件，可指定显示模式
    void WinExec(_In_ const wchar_t *cmdline, _In_ long cmdshow, _Out_ long *ret);

    // 运行命令行并返回结果
    void GetCmdStr(_In_ const wchar_t *cmd, _In_ long millseconds, _Out_ std::wstring &retstr);
    // 设置剪贴板数据
    void SetClipboard(_In_ const wchar_t *str, _Out_ long *ret);
    // 获取剪贴板数据
    void GetClipboard(_Out_ std::wstring &ret);
    // 延时指定的毫秒,过程中不阻塞UI操作
    void Delay(_In_ long mis, _Out_ long *ret);
    // 延时指定范围内随机毫秒,过程中不阻塞UI操作
    void Delays(_In_ long mis_min, _In_ long mis_max, _Out_ long *ret);
    //--------------------system misc------------------------------
    // 获取屏幕宽度(像素)
    void GetScreenWidth(_Out_ long *ret);
    // 获取屏幕高度(像素)
    void GetScreenHeight(_Out_ long *ret);
    // 获取屏幕色深(位/像素, 如 32)
    void GetScreenDepth(_Out_ long *ret);
    // 获取系统 DPI 缩放值(96=100%)
    void GetDPI(_Out_ long *ret);
    // 获取当前系统时间, 格式 "yyyy-MM-dd HH:mm:ss"
    void GetTime(_Out_ std::wstring &ret);
    // 蜂鸣器发声. freq: 频率Hz(37..32767), dur: 时长ms. 返回1成功
    void Beep(_In_ long freq, _In_ long dur, _Out_ long *ret);
    // 获取 [min, max] 区间内随机整数(闭区间, 含两端)
    void GetRandomNumber(_In_ long min, _In_ long max, _Out_ long *ret);
    // 获取 [min, max] 区间内随机浮点数
    void GetRandomDouble(_In_ double min, _In_ double max, _Out_ double *ret);
    // 概率判定: 以 1/p 的概率返回1, 否则返回0. p<=0 返回0, p==1 恒返回1(拟人化核心)
    void GaiLu(_In_ long p, _Out_ long *ret);
    // 获取本机机器码(注册表 MachineGuid), 失败返回空串
    void GetMachineCode(_Out_ std::wstring &ret);
    // 检测当前进程是否以管理员权限运行. 1是 0否
    void IsElevated(_Out_ long *ret);
    // 为当前进程启用 SeDebugPrivilege（读高权限/提权进程内存的前置）。已启用或启用成功返回 1。
    void EnableDebugPrivilege(_Out_ long *ret);
    // 取当前线程的 GetLastError 错误文本（调用本方法前的最后一次 Win32 错误）。无法解析返回空串。
    void GetLastErrorString(_Out_ std::wstring &ret);
    // 注册表四件套。path 形如 "HKCU\Software\Vendor\App"（支持 HKCU/HKLM/HKCR/HKU/HKCC）；
    // name 为空串 = 默认值。Read 成功 ret=1，键/值不存在或类型不符 ret=0 且输出清空。
    void RegistryReadString(_In_ const wchar_t *path, _In_ const wchar_t *name, _Out_ std::wstring &retstr,
                            _Out_ long *ret);
    void RegistryWriteString(_In_ const wchar_t *path, _In_ const wchar_t *name, _In_ const wchar_t *value,
                             _Out_ long *ret);
    void RegistryReadDword(_In_ const wchar_t *path, _In_ const wchar_t *name, _Out_ long *value, _Out_ long *ret);
    void RegistryWriteDword(_In_ const wchar_t *path, _In_ const wchar_t *name, _In_ long value, _Out_ long *ret);
    // 全局热键：modifiers 位掩码 1=ALT 2=CTRL 4=SHIFT 8=WIN（可组合），vk 为 Win32 虚拟键码。
    // id∈[1,0xBFFF] 由调用方分配；注册冲突/非法参数 ret=0。注销后事件队列保留已收到的事件。
    void HotkeyRegister(_In_ long id, _In_ long modifiers, _In_ long vk, _Out_ long *ret);
    void HotkeyUnregister(_In_ long id, _Out_ long *ret);
    // 轮询热键事件，返回热键 id；0 = 超时。timeout_ms=0 非阻塞；<0 等待直到有事件（最长 60s）。
    // 事件在独立消息线程上入队，脚本按自己的节奏取，不会阻塞热键线程。
    void HotkeyPoll(_In_ long timeout_ms, _Out_ long *ret);
    void HotkeyUnregisterAll(_Out_ long *ret);
    // MD5/SHA1（UTF-8 编码后哈希），输出小写十六进制；空输入返回空串。
    void Md5(_In_ const wchar_t *text, _Out_ std::wstring &ret);
    void Sha1(_In_ const wchar_t *text, _Out_ std::wstring &ret);
    // Base64 编码/解码（输入按 UTF-8 字节流处理）。解码遇非法串返回空串。
    void Base64Encode(_In_ const wchar_t *text, _Out_ std::wstring &ret);
    void Base64Decode(_In_ const wchar_t *base64, _Out_ std::wstring &ret);
    // SHA256 与 HMAC-SHA256（UTF-8 编码后计算），输出小写十六进制；空串输入返回空串
    // （HMAC 的空消息合法，只有 key 为空时返回空串）。
    void Sha256(_In_ const wchar_t *text, _Out_ std::wstring &ret);
    void HmacSha256(_In_ const wchar_t *key, _In_ const wchar_t *text, _Out_ std::wstring &ret);
    // 生成 length 字节加密强随机数（CSPRNG），输出小写十六进制（长度翻倍）。0<=length<=4096，超出返回空串。
    void GenerateRandomBytes(_In_ long length, _Out_ std::wstring &ret);
    // 生成 GUID 文本，大写、带连字符、无花括号（8-4-4-4-12）。失败返回空串。
    void GenerateGuid(_Out_ std::wstring &ret);
    // AES-256-GCM 加密（随机 salt/nonce，PBKDF2-SHA256 10 万次派生），输出 Base64 密封串。
    // 密码为空、明文为空或加密失败返回空串。格式：magic|version|size|salt|nonce|tag|cipher。
    void AesEncrypt(_In_ const wchar_t *text, _In_ const wchar_t *password, _Out_ std::wstring &ret);
    // 解密 AesEncrypt 的输出。密码错误或密文被篡改返回空串（GCM 认证失败）。
    void AesDecrypt(_In_ const wchar_t *base64, _In_ const wchar_t *password, _Out_ std::wstring &ret);
    // PBKDF2-HMAC-SHA256。iterations∈[1,10000000]，length∈[1,1024]，输出小写十六进制。
    void Pbkdf2Sha256(_In_ const wchar_t *password, _In_ const wchar_t *salt, _In_ long iterations, _In_ long length,
                      _Out_ std::wstring &ret);
    // 文件摘要（流式读取，不整文件进内存），输出小写十六进制；文件不存在/无权限返回空串。
    void Md5File(_In_ const wchar_t *file_name, _Out_ std::wstring &ret);
    void Sha1File(_In_ const wchar_t *file_name, _Out_ std::wstring &ret);
    void Sha256File(_In_ const wchar_t *file_name, _Out_ std::wstring &ret);
    // RSA 加密/解密（PKCS#1 v1.5）。公钥 PEM 支持 SPKI（BEGIN PUBLIC KEY）；
    // 私钥 PEM 支持 PKCS#8（BEGIN PRIVATE KEY）与 PKCS#1（BEGIN RSA PRIVATE KEY）。
    // 密文输出 Base64；解密输出 UTF-8 文本。密钥格式错误/加解密失败返回空串。
    void RsaEncrypt(_In_ const wchar_t *text, _In_ const wchar_t *public_key_pem, _Out_ std::wstring &ret);
    void RsaDecrypt(_In_ const wchar_t *base64, _In_ const wchar_t *private_key_pem, _Out_ std::wstring &ret);
    //--------------------并发诊断（批 A 2026-11-22）----------------------------------
    // 约定不变：**单个 Op 实例不是线程安全的**（见本文件顶部），多线程场景每线程一个实例。
    // 本接口不改变该约定，只把"同实例被多线程使用"从静默污染变成可观测事实。
    // 返回 JSON：
    //   {"id":1,"in_flight":0,"peak":3,"reentrant":12,"calls_total":880,
    //    "image_count":4,"bound":0,"thread_safe":false}
    //   in_flight   当前正在本实例上执行的调用数（同一时刻 >1 即并发使用）
    //   peak        历史峰值并发度（>1 说明脚本确实踩了这条线）
    //   reentrant   检测到同实例并发使用的累计次数（>0 时结果不再可复现）
    //   thread_safe 恒为 false，保留字段供脚本做显式判断
    // 只做原子计数，不串行化、不改行为。
    void ConcurrencyInfo(_Out_ std::wstring &retjson);
    //--------------------Draw overlay（调试可视化，进程级单例）-----------------------
    // 在屏幕透明叠加层追加一个矩形/直线/文本框。color="RRGGBB"（与找色同格式）。返回 1=成功。
    // 叠加层不可点击穿透由窗口样式保证；首次调用自动创建（专用线程 + 全屏分层窗口）。
    void DrawRect(_In_ long x1, _In_ long y1, _In_ long x2, _In_ long y2, _In_ const wchar_t *color, _In_ long width,
                  _Out_ long *ret);
    void DrawLine(_In_ long x1, _In_ long y1, _In_ long x2, _In_ long y2, _In_ const wchar_t *color, _In_ long width,
                  _Out_ long *ret);
    void DrawText(_In_ long x, _In_ long y, _In_ const wchar_t *text, _In_ const wchar_t *color, _In_ long font_size,
                  _Out_ long *ret);
    // 清空叠加层全部图元并重绘。
    void DrawClear(_Out_ long *ret);
    // 显示/隐藏叠加层窗口（图元保留）。1=成功。
    void DrawSetVisible(_In_ long show, _Out_ long *ret);

    // ---- 批 B 补齐（2026-11-22，对齐 OLA DrawGui 的常用子集）----
    // 圆形（屏幕坐标）：x,y 为圆心，radius 为半径，color="RRGGBB"，width 为线宽。
    void DrawCircle(_In_ long x, _In_ long y, _In_ long radius, _In_ const wchar_t *color, _In_ long width,
                    _Out_ long *ret);
    // 在屏幕叠加层画一张图片（左上角 x,y）。alpha 0~255；<=0 或 >255 视为 255（不透明）。
    void DrawImage(_In_ long x, _In_ long y, _In_ const wchar_t *file_name, _In_ long alpha, _Out_ long *ret);

    // ---- 绑定窗口内绘制（坐标 = 绑定窗口客户区，与找色/鼠标同口径）----
    // 画在**独立透明窗口**上，不写目标 DX 后台缓冲 -> **不会出现在 DX 截图里**；
    // 窗口移动/缩放时跟随（250ms 轮询 + 每次绘制校正）。
    // 未绑定窗口时返回 0 并记日志（不静默画到屏幕，否则会被误判成坐标系问题）。
    // mode：0 纯填充 / 1 纯描边 / 2 填充+描边（其它值按描边，并记日志）。
    // 填充色与开关由 DrawSetFill 控制（默认开启、半透明红）；描边色由 DrawSetColor 控制（默认白）。
    // mode=0 时 line_width 被忽略（不描边）。
    void DrawRectEx(_In_ long x, _In_ long y, _In_ long width, _In_ long height, _In_ long mode, _In_ long line_width,
                    _Out_ long *ret);
    void DrawCircleEx(_In_ long x, _In_ long y, _In_ long radius, _In_ long line_width, _Out_ long *ret);
    void DrawTextEx(_In_ long x, _In_ long y, _In_ const wchar_t *text, _In_ long font_size, _Out_ long *ret);
    // 只清绑定窗口那批图元；屏幕叠加层的图元由 DrawClear 负责。
    void DrawClearBind(_Out_ long *ret);
    // 诊断：分别返回屏幕层/绑定层的图元数量（任一指针可为 nullptr）。
    void DrawGetCount(_Out_ long *screen_count, _Out_ long *bind_count);

    // ---- 批 G 补齐（2026-11-23）：颜色/填充状态 + 绑定层画图 + 图片句柄上屏 ----
    // 设置 Ex 系列（DrawRectEx/DrawCircleEx/DrawTextEx）的描边色，影响之后绘制的图元，
    // 已画的保持不变。color="RRGGBB"；解析失败返回 0 并保持原色。默认白色。
    void DrawSetColor(_In_ const wchar_t *color, _Out_ long *ret);
    // 填充开关 + 填充色。enable=0 关闭（DrawRectEx 的 mode 0/2 退化为描边）；
    // enable=1 开启，color 省略/空串则沿用当前填充色。
    // color 支持 "RRGGBB"（自动加 0x66 半透明，避免把目标窗口盖死）与 "AARRGGBB"（显式 alpha）。
    // 默认开启，默认色 = 半透明红（66FF0000）。
    void DrawSetFill(_In_ long enable, _In_ const wchar_t *color, _Out_ long *ret);
    // 绑定窗口内画图片文件（客户区坐标）。alpha 0~255（<=0 或 >255 视为 255）。
    // 与屏幕层 DrawImage 的差别：文件不存在时**显式失败返回 0**（不只是静默跳过）。
    void DrawImageEx(_In_ long x, _In_ long y, _In_ const wchar_t *file_name, _In_ long alpha, _Out_ long *ret);
    // 把图片句柄（OpLoadImage/CropImage 等产出）的内容画到**屏幕叠加层**。
    // 像素在图元里存一份，渲染线程不访问图像注册表——句柄随后释放也不影响已提交的图元。
    void DrawImageFromHandle(_In_ long x, _In_ long y, _In_ const wchar_t *image_handle, _In_ long alpha,
                             _Out_ long *ret);

    // ---- 批 G2（2026-11-23）：标签替换（实时跟踪框的正确用法）----
    // 带 tag 的图元遵循"同层同标签只保留最新一条"：重画同一个 tag 等于就地替换，
    // 不用 DrawClear（会闪、且图元会累积）。tag 为空返回 0。
    void DrawRectTag(_In_ const wchar_t *tag, _In_ long x1, _In_ long y1, _In_ long x2, _In_ long y2,
                     _In_ const wchar_t *color, _In_ long width, _Out_ long *ret);
    // 绑定层版本，坐标 = 绑定窗口客户区；mode 语义同 DrawRectEx。
    void DrawRectExTag(_In_ const wchar_t *tag, _In_ long x, _In_ long y, _In_ long width, _In_ long height,
                       _In_ long mode, _In_ long line_width, _Out_ long *ret);
    // 删除同标签图元（屏幕层 + 绑定层一起清），**返回删除个数**；0 = 没有匹配项或标签为空。
    void DrawRemoveTag(_In_ const wchar_t *tag, _Out_ long *ret);

    // ---- 批 H（2026-11-23）：UIA 元素层最小集 ----
    //
    // 定位：图色之外的第二条路，直接读 UI 元素树，不依赖分辨率/主题/DPI。
    // ⚠ 只对暴露 UIA 的程序有效（Win32/WinForms/WPF/Qt/Chromium 系）；DX 自绘的游戏界面读不到节点。
    // ⚠ 元素 id 是**本进程内**的注册表序号，op_com 与 op_c_api 各有一份注册表，跨层拿 id 必找不到。
    // ⚠ 坐标是**屏幕物理像素**，与绑定态客户区坐标不是一套。
    //
    // 按深度优先列出窗口下所有元素（hwnd=0 取前台窗口）。max_depth<0 视为 0。
    // 返回 {ok,nodes:[{id,name,class,aid,type,type_name,rect:[l,t,r,b],enabled,offscreen,depth}],count}
    void UiaDump(_In_ LONG_PTR hwnd, _In_ long max_depth, _Out_ std::wstring &retjson);
    // 按选择器查找。语法：name=确定 >> class=Button；`name*=确定` 包含匹配；
    // 键支持 name / class / id(automationId) / type(控件类型名或数字) / index(0 基)。
    // timeout_ms>0 时轮询到超时（每 100ms），0 = 只查一次。
    // 返回 {ok,count,elements:[{id,...,value}],}；失败 {ok:0,error:"..."}
    void UiaFind(_In_ LONG_PTR hwnd, _In_ const wchar_t *selector, _In_ long timeout_ms,
                 _Out_ std::wstring &retjson);
    // 屏幕坐标命中的元素（用于"鼠标所指是什么控件"）。返回 {ok,element:{...}}。
    void UiaFromPoint(_In_ long x, _In_ long y, _Out_ std::wstring &retjson);
    // 触发元素（Invoke 模式；复选框等走 Toggle 兜底）。1=成功。
    void UiaInvoke(_In_ long element_id, _Out_ long *ret);
    // 读元素的值（ValuePattern）；无该模式返回空串。
    void UiaGetValue(_In_ long element_id, _Out_ std::wstring &retstr);
    // 写元素的值（ValuePattern）。1=成功；只读控件返回 0。
    void UiaSetValue(_In_ long element_id, _In_ const wchar_t *value, _Out_ long *ret);
    // 释放单个元素 / 释放全部（返回释放个数）。长期运行的脚本必须及时释放，否则注册表只增不减。
    void UiaRelease(_In_ long element_id, _Out_ long *ret);
    void UiaReleaseAll(_Out_ long *ret);
    //--------------------Background -----------------------
    // 兼容旧接口的单句柄绑定。显示和输入都使用同一个 hwnd。
    void BindWindow(_In_ LONG_PTR hwnd, _In_ const wchar_t *display, _In_ const wchar_t *mouse, _In_ const wchar_t *keypad,
                    _In_ long mode, _Out_ long *ret);
    // 扩展绑定接口。显示截图使用 display_hwnd，鼠标和键盘输入使用 input_hwnd。
    void BindWindowEx(_In_ LONG_PTR display_hwnd, _In_ LONG_PTR input_hwnd, _In_ const wchar_t *display,
                      _In_ const wchar_t *mouse, _In_ const wchar_t *keypad, _In_ long mode, _Out_ long *ret);
    // 解绑窗口
    void UnBindWindow(_Out_ long *ret);
    // 临时锁定目标窗口的外部输入。只对 dx 鼠标、dx 键盘有效。
    void LockInput(_In_ long lock, _Out_ long *ret);
    // 绑定降载(降低多开 CPU 占用):每次截图成功后强制延时 rate 毫秒(0-100,自动钳制),0 表示关闭。
    // type: 0=推荐档,1=高效档(当前实现相同,预留语义)。随 BindingSession 生命周期,解绑不重置。
    void DownCpu(_In_ long type, _In_ long rate, _Out_ long *ret);
    // 设置 dx 输入通道开关。只对 dx 鼠标、dx 键盘有效。
    // attr=0 时 value 是完整掩码；attr 取 1(DirectInput)/2(RawInput)/4(窗口消息) 或其组合时，
    // value 非 0 表示打开这些通道，0 表示关闭。默认 7（三通道全开）。
    void SetDxAttr(_In_ long attr, _In_ long value, _Out_ long *ret);
    // 获取当前的 dx 输入通道开关掩码。
    void GetDxAttr(_Out_ long *ret);
    // 获取当前对象已经绑定的显示窗口句柄. 无绑定返回0
    void GetBindWindow(_Out_ LONG_PTR *ret);
    // 判定当前对象是否已绑定窗口.
    void IsBind(_Out_ long *ret);
    // 获取当前实际生效的图色模式名（"normal"/"gdi2"/"dx2"/"dx.d3d11"/"normal.wgc"/"opengl.nox" …）。
    // 未绑定返回空串。normal.auto 绑定成功后返回自动选中并成功的那一个。
    // 用途：截图黑屏/找图错位时先确认走的是哪条链路；配合 BindWindowEx 失败后的 GetLastError()
    // 原因码（0x2001+，见 BindFailReason）定位绑定问题。
    void GetDisplayMethod(_Out_ std::wstring &ret);
    //--------------------mouse & keyboard------------------
    // 获取鼠标位置.
    void GetCursorPos(_Out_ long *x, _Out_ long *y, _Out_ long *ret);
    // 获取当前鼠标形状: visible,hash,width,height,hotX,hotY.
    void GetCursorShape(_Out_ std::wstring &ret);
    // 获取当前鼠标光标图像：32bpp BMP 的 Base64（top-down BGRA 含 alpha）。
    // 失败（光标不可见/无桌面会话）返回空串；宽高/热点等元数据用 GetCursorShape 配套取。
    void GetCursorImage(_Out_ std::wstring &ret);
    // 鼠标相对于上次的位置移动rx,ry.
    void MoveR(_In_ long x, _In_ long y, _Out_ long *ret);
    // 把鼠标移动到目的点(x,y)
    void MoveTo(_In_ long x, _In_ long y, _Out_ long *ret);
    // 把鼠标移动到目的范围内的任意一点
    void MoveToEx(_In_ long x, _In_ long y, _In_ long w, _In_ long h, _Out_ std::wstring &ret);
    // 按模拟轨迹移动到指定点
    void MoveToSmooth(_In_ long x, _In_ long y, _In_ long duration, _Out_ long *ret);
    // 在目标范围内随机取点，并按模拟轨迹移动过去
    void MoveToExSmooth(_In_ long x, _In_ long y, _In_ long w, _In_ long h, _In_ long duration,
                        _Out_ std::wstring &ret);
    // 按 x,y|x,y 格式提供的路径移动
    void MovePath(_In_ const wchar_t *path, _In_ long duration, _Out_ long *ret);
    // 按 x,y|x,y 格式提供的路径拖拽
    void DragPath(_In_ const wchar_t *path, _In_ long duration, _Out_ long *ret);
    // 设置模拟轨迹参数，只影响 Smooth 和 Path 系列鼠标移动
    void SetMouseTrajectory(_In_ long mode, _In_ long min_duration, _In_ long max_duration, _In_ long jitter,
                            _In_ long start_delay, _In_ long end_delay, _Out_ long *ret);
    // 按下鼠标左键
    void LeftClick(_Out_ long *ret);
    // 双击鼠标左键
    void LeftDoubleClick(_Out_ long *ret);
    // 按住鼠标左键
    void LeftDown(_Out_ long *ret);
    // 弹起鼠标左键
    void LeftUp(_Out_ long *ret);
    // 按下鼠标中键
    void MiddleClick(long *ret);
    // 双击鼠标中键
    void MiddleDoubleClick(long *ret);
    // 按住鼠标中键
    void MiddleDown(_Out_ long *ret);
    // 弹起鼠标中键
    void MiddleUp(_Out_ long *ret);
    // 按下鼠标右键
    void RightClick(long *ret);
    // 双击鼠标右键
    void RightDoubleClick(long *ret);
    // 按住鼠标右键
    void RightDown(_Out_ long *ret);
    // 弹起鼠标右键
    void RightUp(long *ret);
    // 按下鼠标侧键1
    void XButton1Click(long *ret);
    // 双击鼠标侧键1
    void XButton1DoubleClick(long *ret);
    // 按住鼠标侧键1
    void XButton1Down(long *ret);
    // 弹起鼠标侧键1
    void XButton1Up(long *ret);
    // 按下鼠标侧键2
    void XButton2Click(long *ret);
    // 双击鼠标侧键2
    void XButton2DoubleClick(long *ret);
    // 按住鼠标侧键2
    void XButton2Down(long *ret);
    // 弹起鼠标侧键2
    void XButton2Up(long *ret);
    // 垂直滚轮滚动指定 delta
    void Wheel(long delta, long *ret);
    // 水平滚轮滚动指定 delta
    void HWheel(long delta, long *ret);
    // 滚轮向下滚
    void WheelDown(_Out_ long *ret);
    // 滚轮向上滚
    void WheelUp(_Out_ long *ret);
    // 设置鼠标单击或者双击时,鼠标按下和弹起的时间间隔
    void SetMouseDelay(_In_ const wchar_t *type, _In_ long delay, _Out_ long *ret);
    // 获取指定的按键状态.(前台信息,不是后台)
    void GetKeyState(_In_ long vk_code, _Out_ long *ret);
    // 按住指定的虚拟键码
    void KeyDown(_In_ long vk_code, _Out_ long *ret);
    // 按住指定的虚拟键码
    void KeyDownChar(_In_ const wchar_t *vk_code, _Out_ long *ret);
    // 弹起来虚拟键vk_code
    void KeyUp(_In_ long vk_code, _Out_ long *ret);
    // 弹起来虚拟键vk_code
    void KeyUpChar(_In_ const wchar_t *vk_code, _Out_ long *ret);
    // 等待指定的按键按下 (前台,不是后台)
    void WaitKey(_In_ long vk_code, _In_ long time_out, _Out_ long *ret);
    // 发送字符串
    // long SendString(long HWND)
    // 弹起来虚拟键vk_code
    void KeyPress(_In_ long vk_code, _Out_ long *ret);
    void KeyPressChar(_In_ const wchar_t *vk_code, _Out_ long *ret);
    // 设置按键时,键盘按下和弹起的时间间隔
    void SetKeypadDelay(_In_ const wchar_t *type, _In_ long delay, _Out_ long *ret);
    // 根据指定的字符串序列，依次按顺序按下其中的字符
    void KeyPressStr(_In_ const wchar_t *key_str, _In_ long delay, _Out_ long *ret);

    // ---- 时间轴层（批 E 2026-11-22）：录制 / 回放 / 监听 / 主从同步 ----
    //
    // 采集入口挂在**每个输入原语的后端出口**（MoveTo / LeftDown / KeyDown / …），
    // 所以 Down/Up 级、Click 级、KeyPressStr、DragPath 等**全部覆盖**，且一次
    // 「LeftClick」会录成 down+up 两条（回放才能复原"按了几下"）。
    //
    // 坐标系：录的是**客户区坐标**（与 Mouse 各 API 同口径）。录制与回放必须
    // 落在同一个绑定窗口上，否则坐标含义不同（未绑定时 = 屏幕坐标）。
    //
    // 录制文件格式：JSONL，一行一个事件，字段固定为
    //   {"t":12.340,"type":"mouse_button","vk":0,"x":300,"y":200,"wheel":0,"flags":4}
    //   type: mouse_move / mouse_button / mouse_wheel / key / key_char
    //   t:    相对录制起点的毫秒数（回放按它等比缩放）
    //   flags: mouse_button 用 MOUSEEVENTF_*（0x0002/0x0004/…）；
    //          mouse_wheel 用 MOUSEEVENTF_WHEEL(0x0800)/HWHEEL(0x1000)；
    //          key 用 KEYEVENTF_KEYUP(0x0002)；key_char 用 KEYEVENTF_UNICODE(0x0004) 且 x 字段存字符码。
    //
    // 采集开关：0 关闭（同时结束正在进行的录制）／1 打开。默认关闭，开启后热路径只有
    // 一次原子读，对正常脚本的输入耗时可忽略。
    void InputSyncEnable(_In_ long enable, _Out_ long *ret);
    // 输入监听回调。回调在**调用方线程**同步触发，参数是刚发生的事件（t 相对录制起点，
    // 未录制时为 0）。user 原样回传。
    // ⚠ 回调里**不要**再调输入 API：会被旁路护栏挡掉（不是崩溃，但那部分输入不录）。
    // 回调是函数指针而不是 std::function，与 C API / COM / 易语言 的用法同构。
    typedef void (*InputMonitorCallback)(const op_input_event_view *event, void *user);
    // 传 callback=nullptr 取消监听。
    void SetInputMonitor(_In_ InputMonitorCallback callback, _In_ void *user, _Out_ long *ret);
    // 录制。
    //   action 0 = START：开始录制；path 为空时自动生成
    //            （工作目录下 input_record_YYYYmmdd_HHMMSS.jsonl）。
    //            返回 1 成功、0 失败；**已在录制时返回 0 且不截断前一段**。
    //   action 1 = STOP ：结束录制，ret = 实际写入的事件条数（0 = 确实没产生输入）；
    //            retpath 为文件路径。
    void InputRecord(_In_ long action, _In_ const wchar_t *path, _Out_ std::wstring &retpath, _Out_ long *ret);
    // 取"最近一次录制（或正在录制）"的文件路径；没录过返回空串。
    void InputRecordGetPath(_Out_ std::wstring &retpath);
    // 回放。
    //   action 0 = LOAD ：读入 JSONL。ret = 事件条数，retjson 形如
    //            {"ok":1,"events":12,"bound":1,"path":"D:\\a.jsonl"}（读失败 ok=0 + error）。
    //            ⚠ bound=0 表示**当前没绑定窗口**，坐标会按屏幕语义回放，与录制时不同。
    //   action 1 = START：同步回放（本调用阻塞到放完或被 STOP）。speed 为倍速（<=0 视为 1.0），
    //            loop 为循环次数（<=0 视为 1）。ret=1 放完 / 0 没东西可放。
    //   action 2 = STOP ：请求停止（下一次事件边界生效，最长等待 5ms 的睡眠片）。
    //   action 3 = FREE ：释放已装载的事件。
    // 时间轴按 t 等比缩放：目标时刻 = t / speed，按"相对回放起点的绝对时刻"推进，
    // 长录音不会累积漂移。
    long InputPlayback(_In_ long action, _In_ const wchar_t *path, _In_ double speed, _In_ long loop,
                       _Out_ std::wstring &retjson, _Out_ long *ret);
    // 主从同步：把 master 实例产生的输入事件**异步**投递到 slave 实例。
    //   enable 0 = 取消（master 必须确实是当前的 master，否则返回 0）；
    //   enable 1 = 建立/改绑。
    // 语义与限制（重要）：
    //   * 投递是**异步 + 可丢弃**的：输入是边沿事件，迟到的点击会点错地方，所以队列
    //     满（256 条）时丢事件而不是阻塞主实例。丢弃数可从 InputSyncStatus 读到。
    //   * master == slave 会被拒绝（自己投自己会自我放大）。
    //   * slave 的绑定模式决定投递效果：WIN 语义（normal/windows）可完整复现；
    //     DX 模式的移动需要目标进程内载入器，可能只复现按键。
    void InputSyncLink(_In_ long master, _In_ long slave, _In_ long enable, _Out_ long *ret);
    // 主从链路诊断（本实例视角）：
    //   {"ok":1,"enabled":0/1,"master":<本实例 id>,"as_master_of":N,"as_slave_of":N,
    //    "async_dropped":0,"queue":0}
    void InputSyncStatus(_Out_ std::wstring &retjson);
    //--------------------image and color-----------------------
    // 抓取指定区域(x1, y1, x2, y2)的图像, 保存为file
    void Capture(_In_ long x1, _In_ long y1, _In_ long x2, _In_ long y2, _In_ const wchar_t *file_name, _Out_ long *ret);
    // sim 语义注意（继承大漠，各家族不一致，传参前请确认）：
    //   找色/多点找色家族：sim<0 被钳制为 1.0（变为精确匹配）；
    //   OCR 家族（Ocr/OcrEx/FindStr 等）：sim<0 或 >1 回退 0.7；
    //   找图家族（FindPic/FindPicEx/FindPicExS）：实际相似度 = 0.5 + sim/2，sim=0.8 对应内部 0.9。
    // 比较指定坐标点(x,y)的颜色
    void CmpColor(_In_ long x, _In_ long y, _In_ const wchar_t *color, _In_ double sim, _Out_ long *ret);
    // 查找指定区域内的颜色
    void FindColor(_In_ long x1, _In_ long y1, _In_ long x2, _In_ long y2, _In_ const wchar_t *color, _In_ double sim,
                   _In_ long dir, _Out_ long *x, _Out_ long *y, _Out_ long *ret);
    // 查找指定区域内的所有颜色
    void FindColorEx(_In_ long x1, _In_ long y1, _In_ long x2, _In_ long y2, _In_ const wchar_t *color, _In_ double sim,
                     _In_ long dir, _Out_ std::wstring &retstr);
    // 查找指定区域内的所有颜色数量
    void GetColorNum(_In_ long x1, _In_ long y1, _In_ long x2, _In_ long y2, _In_ const wchar_t *color, _In_ double sim,
                     _Out_ long *ret);
    // 根据指定的多点查找颜色坐标
    void FindMultiColor(_In_ long x1, _In_ long y1, _In_ long x2, _In_ long y2, _In_ const wchar_t *first_color,
                        _In_ const wchar_t *offset_color, _In_ double sim, _In_ long dir, _Out_ long *x, _Out_ long *y,
                        _Out_ long *ret);
    // 根据指定的多点查找所有颜色坐标
    void FindMultiColorEx(_In_ long x1, _In_ long y1, _In_ long x2, _In_ long y2, _In_ const wchar_t *first_color,
                          _In_ const wchar_t *offset_color, _In_ double sim, _In_ long dir, _Out_ std::wstring &retstr);
    // 查找指定区域内的图片
    void FindPic(_In_ long x1, _In_ long y1, _In_ long x2, _In_ long y2, _In_ const wchar_t *files,
                 _In_ const wchar_t *delta_color, _In_ double sim, _In_ long dir, _Out_ long *x, _Out_ long *y,
                 _Out_ long *ret);
    // 查找多个图片
    void FindPicEx(_In_ long x1, _In_ long y1, _In_ long x2, _In_ long y2, _In_ const wchar_t *files,
                   _In_ const wchar_t *delta_color, _In_ double sim, _In_ long dir, _Out_ std::wstring &retstr);
    //
    // 这个函数可以查找多个图片, 并且返回所有找到的图像的坐标.此函数同FindPicEx.只是返回值不同.(file1,x,y|file2,x,y|...)
    void FindPicExS(_In_ long x1, _In_ long y1, _In_ long x2, _In_ long y2, _In_ const wchar_t *files,
                    _In_ const wchar_t *delta_color, _In_ double sim, _In_ long dir, _Out_ std::wstring &retstr);
    // 查找指定区域内的颜色块,颜色格式"RRGGBB-DRDGDB",注意,和按键的颜色格式相反
    void FindColorBlock(_In_ long x1, _In_ long y1, _In_ long x2, _In_ long y2, _In_ const wchar_t *color,
                        _In_ double sim, _In_ long count, _In_ long height, _In_ long width, _Out_ long *x, _Out_ long *y,
                        _Out_ long *ret);
    // 查找指定区域内的所有颜色块, 颜色格式"RRGGBB-DRDGDB", 注意, 和按键的颜色格式相反
    void FindColorBlockEx(_In_ long x1, _In_ long y1, _In_ long x2, _In_ long y2, _In_ const wchar_t *color,
                          _In_ double sim, _In_ long count, _In_ long height, _In_ long width, _Out_ std::wstring &retstr);
    // 颜色块增强版：mode=0 raw（同 FindColorBlockEx）；mode=1 重合窗口聚类合并，每个色块只返回一个左上角坐标
    void FindColorBlockExS(_In_ long x1, _In_ long y1, _In_ long x2, _In_ long y2, _In_ const wchar_t *color,
                           _In_ double sim, _In_ long count, _In_ long height, _In_ long width, _In_ long mode,
                           _Out_ std::wstring &retstr);
    // 获取(x,y)的颜色
    void GetColor(_In_ long x, _In_ long y, _Out_ std::wstring &ret);
    // 颜色串转 RGB 分量。支持 "RRGGBB"/"AARRGGBB"，可带 # / 0x 前缀，大小写不敏感；
    // 8 位时按 AARRGGBB 解释。解析失败各分量置 0 且 ret=0。
    void Hex2RGB(_In_ const wchar_t *color, _Out_ long *r, _Out_ long *g, _Out_ long *b, _Out_ long *ret);
    // 同 Hex2RGB，另输出 alpha；6 位输入 alpha=255。
    void Hex2ARGB(_In_ const wchar_t *color, _Out_ long *a, _Out_ long *r, _Out_ long *g, _Out_ long *b,
                  _Out_ long *ret);
    // RGB 分量转 "AARRGGBB"（大写）。分量越界按 0/255 截断；ret=1。
    void ARGB2Hex(_In_ long a, _In_ long r, _In_ long g, _In_ long b, _Out_ std::wstring &ret);
    // RGB 转 HSV：H∈[0,360)，S/V∈[0,100]，均为 double。分量越界按 0/255 截断。
    void RGB2HSV(_In_ long r, _In_ long g, _In_ long b, _Out_ double *h, _Out_ double *s, _Out_ double *v,
                 _Out_ long *ret);
    // 颜色串转 HSV（颜色串格式同 Hex2RGB），输出口径同 RGB2HSV。
    void Hex2HSV(_In_ const wchar_t *color, _Out_ double *h, _Out_ double *s, _Out_ double *v, _Out_ long *ret);
    // RGB 分量转 "RRGGBB"（大写），分量越界按 0/255 截断。
    void GetColorHex(_In_ long r, _In_ long g, _In_ long b, _Out_ std::wstring &ret);
    //
    // 设置图像输入方式，默认窗口截图
    void SetDisplayInput(_In_ const wchar_t *mode, _Out_ long *ret);

    void LoadPic(_In_ const wchar_t *file_name, _Out_ long *ret);

    void FreePic(_In_ const wchar_t *file_name, _Out_ long *ret);
    // 从内存加载要查找的图片
    void LoadMemPic(_In_ const wchar_t *file_name, _In_ void *data, _In_ long size, _Out_ long *ret);
    // 获取指定图片的尺寸，如果指定的图片已经被加入缓存，则从缓存中获取信息.此接口也会把此图片加入缓存
    void GetPicSize(_In_ const wchar_t *pic_name, _Out_ long *width, _Out_ long *height, _Out_ long *ret);
    //
    void GetScreenData(_In_ long x1, _In_ long y1, _In_ long x2, _In_ long y2, _Out_ size_t *data, _Out_ long *ret);
    //
    void GetScreenDataBmp(_In_ long x1, _In_ long y1, _In_ long x2, _In_ long y2, _Out_ size_t *data, _Out_ long *size,
                          _Out_ long *ret);
    //
    void GetScreenFrameInfo(_Out_ long *frame_id, _Out_ long *time);
    // 卡屏检测（P2 第二十四批 2026-10-16，对齐 OLA IsDisplayDead）：
    // 对区域连续 check_times 次截图（间隔 interval_ms）做帧哈希比对，全部相同=卡死返回 1，
    // 任一帧有变化=存活返回 0；截图失败保守返回 0（视为无法判死）。
    // check_times<=0 默认 3（钳 2-20），interval_ms<=0 默认 500（钳 50-10000）。
    void IsDisplayDead(_In_ long x1, _In_ long y1, _In_ long x2, _In_ long y2, _In_ long check_times,
                       _In_ long interval_ms, _Out_ long *ret);
    // 获取绑定窗口当前帧率。采样窗口约 1 秒,按 frameId 增量/经过毫秒换算。
    // Hook/DX/WGC 等异步后端=真实渲染帧率;GDI 等按需截图后端=采样窗口内的抓帧频率(无抓帧为 0)。未绑定返回 0。
    void GetFPS(_Out_ long *ret);
    //
    void MatchPicName(_In_ const wchar_t *pic_name, _Out_ std::wstring &retstr);
    //----------------------opcv-------------------------
    void CvLoadTemplate(_In_ const wchar_t *name, _In_ const wchar_t *file_path, _Out_ long *ret);
    void CvLoadMaskedTemplate(_In_ const wchar_t *name, _In_ const wchar_t *template_path, _In_ const wchar_t *mask_path,
                              _Out_ long *ret);
    void CvRemoveTemplate(_In_ const wchar_t *name, _Out_ long *ret);
    void CvRemoveAllTemplates(_Out_ long *ret);
    void CvHasTemplate(_In_ const wchar_t *name, _Out_ long *ret);
    void CvGetTemplateCount(_Out_ long *ret);
    void CvGetAllTemplateNames(_Out_ std::wstring &retstr);
    void CvGetOpenCvVersion(_Out_ std::wstring &retstr);
    void CvLoadTemplateList(_In_ const wchar_t *template_list, _Out_ long *ret);
    void CvToGray(_In_ const wchar_t *src_file, _In_ const wchar_t *dst_file, _Out_ long *ret);
    void CvToBinary(_In_ const wchar_t *src_file, _In_ const wchar_t *dst_file, _Out_ long *ret);
    void CvToEdge(_In_ const wchar_t *src_file, _In_ const wchar_t *dst_file, _Out_ long *ret);
    void CvToOutline(_In_ const wchar_t *src_file, _In_ const wchar_t *dst_file, _Out_ long *ret);
    void CvDenoise(_In_ const wchar_t *src_file, _In_ const wchar_t *dst_file, _Out_ long *ret);
    void CvEqualize(_In_ const wchar_t *src_file, _In_ const wchar_t *dst_file, _Out_ long *ret);
    void CvCLAHE(_In_ const wchar_t *src_file, _In_ const wchar_t *dst_file, _In_ double clip_limit, _In_ long tile_grid_size,
                 _Out_ long *ret);
    void CvBlur(_In_ const wchar_t *src_file, _In_ const wchar_t *dst_file, _In_ const wchar_t *mode, _In_ long kernel_size,
               _Out_ long *ret);
    void CvSharpen(_In_ const wchar_t *src_file, _In_ const wchar_t *dst_file, _In_ double strength, _Out_ long *ret);
    void CvCropValid(_In_ const wchar_t *src_file, _In_ const wchar_t *dst_file, _Out_ long *ret);
    // 旋转（角度制，正角逆时针）：90 倍数走无损路径；任意角扩画布白边。ret=1 成功。
    void CvRotate(_In_ const wchar_t *src_file, _In_ const wchar_t *dst_file, _In_ double angle_deg, _Out_ long *ret);
    // 翻转：mode 0 上下 / 1 左右 / 2 双向（=旋转 180°）。ret=1 成功。
    void CvFlip(_In_ const wchar_t *src_file, _In_ const wchar_t *dst_file, _In_ long mode, _Out_ long *ret);
    void CvConnectedComponents(_In_ const wchar_t *src_file, _In_ double min_area, _Out_ std::wstring &retjson,
                               _Out_ long *ret);
    void CvFindContours(_In_ const wchar_t *src_file, _In_ double min_area, _Out_ std::wstring &retjson, _Out_ long *ret);
    void CvPreprocessPipeline(_In_ const wchar_t *src_file, _In_ const wchar_t *dst_file, _In_ const wchar_t *pipeline,
                              _Out_ long *ret);
    // 多角度模板匹配：angles 形如 "0|15|-15"（空串走默认角度集）；retjson 额外带命中角度 angle。
    void CvMatchTemplateRot(_In_ long x, _In_ long y, _In_ long width, _In_ long height, _In_ const wchar_t *template_name,
                            _In_ const wchar_t *angles, _In_ double threshold, _In_ long method, _In_ long color_mode,
                            _Out_ std::wstring &retjson, _Out_ long *ret);
    void CvCrop(_In_ const wchar_t *src_file, _In_ long x, _In_ long y, _In_ long width, _In_ long height,
                _In_ const wchar_t *dst_file, _Out_ long *ret);
    void CvResize(_In_ const wchar_t *src_file, _In_ long width, _In_ long height, _In_ const wchar_t *dst_file,
                  _Out_ long *ret);
    void CvThreshold(_In_ const wchar_t *src_file, _In_ const wchar_t *dst_file, _In_ double threshold, _In_ double max_value,
                     _In_ const wchar_t *mode, _Out_ long *ret);
    void CvInRange(_In_ const wchar_t *src_file, _In_ const wchar_t *dst_file, _In_ const wchar_t *color_space,
                   _In_ const wchar_t *lower, _In_ const wchar_t *upper, _Out_ long *ret);
    void CvMorphology(_In_ const wchar_t *src_file, _In_ const wchar_t *dst_file, _In_ const wchar_t *mode,
                      _In_ long kernel_size, _In_ long iterations, _Out_ long *ret);
    void CvThin(_In_ const wchar_t *src_file, _In_ const wchar_t *dst_file, _In_ const wchar_t *mode, _Out_ long *ret);
    void CvMatchTemplate(_In_ long x, _In_ long y, _In_ long width, _In_ long height, _In_ const wchar_t *template_name,
                         _In_ double threshold, _In_ long dir, _In_ long strip_mode, _In_ long method, _In_ long color_mode,
                         _Out_ std::wstring &retjson, _Out_ long *ret);
    void CvMatchTemplateScale(_In_ long x, _In_ long y, _In_ long width, _In_ long height, _In_ const wchar_t *template_name,
                              _In_ const wchar_t *scales, _In_ double threshold, _In_ long method, _In_ long color_mode,
                              _Out_ std::wstring &retjson, _Out_ long *ret);
    void CvMatchAnyTemplate(_In_ long x, _In_ long y, _In_ long width, _In_ long height, _In_ const wchar_t *template_names,
                            _In_ double threshold, _In_ long dir, _In_ long strip_mode, _In_ long method, _In_ long color_mode,
                            _Out_ std::wstring &retjson, _Out_ long *ret);
    void CvMatchAllTemplates(_In_ long x, _In_ long y, _In_ long width, _In_ long height, _In_ const wchar_t *template_names,
                             _In_ double threshold, _In_ long dir, _In_ long strip_mode, _In_ long method, _In_ long color_mode,
                             _Out_ std::wstring &retjson, _Out_ long *ret);
    void CvFeatureMatchTemplate(_In_ long x, _In_ long y, _In_ long width, _In_ long height, _In_ const wchar_t *template_name,
                                _In_ double threshold, _Out_ std::wstring &retjson, _Out_ long *ret);
    void CvEdgeMatchTemplate(_In_ long x, _In_ long y, _In_ long width, _In_ long height, _In_ const wchar_t *template_name,
                             _In_ double threshold, _Out_ std::wstring &retjson, _Out_ long *ret);
    void CvShapeMatchTemplate(_In_ long x, _In_ long y, _In_ long width, _In_ long height, _In_ const wchar_t *template_name,
                              _In_ double threshold, _Out_ std::wstring &retjson, _Out_ long *ret);
    //----------------------ocr-------------------------
    // 选择 OCR 引擎并透传启动参数。engine: "onnx"/空/任意未知名=内置 ONNX（默认）。
    //   http(s):// 与旧远程别名（tesseract/paddle 系）已移除，传入返 0。
    // argv 空格分隔，支持 --timeout=3000、--model-dir=path；内置 ONNX 另支持：
    //   --charset=<规则>  解码期字符白名单（只在名单内选字，形近误识如 0/O、,/. 被消除）。
    //                      语法：@zh = keys 内全部中文(U+4E00-9FFF)；其余可打印 ASCII 按字面加入。
    //                      规则串不能含空格（argv 以空格分隔，会被截断）。
    //                      未设置 = 全字典（默认，与原行为一致）。
    //                      建议值（中文 + 数字 + 大小写字母 + 常用符号）：
    //                      @zh0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ.,:;-+=/|!?%#*()[]~_
    //                      注：白名单缺哪个字符，该字符就被静默吞掉（漏字母时 "Lv99" 只识别出 "99"）。
    long SetOcrEngine(_In_ const wchar_t *path_of_engine, _In_ const wchar_t *dll_name, _In_ const wchar_t *argv);
    // OCR 预热（P2 第二十六批 2026-10-16，对齐 OLA OcrWarmup）：触发引擎懒加载 + 一次小图推理，
    // 消掉首次真实调用的模型加载延迟。ret=1 引擎就绪，0 引擎不可用。
    void OcrWarmup(_Out_ long *ret);
    // 选择 YOLO 引擎并加载模型。统一入口：path_of_engine 直接传 .onnx 模型文件路径
    // （dll_name 留空）即自动切进程内 ONNX 引擎，等价于 ("onnx", 模型路径, argv)。
    // 旧写法仍兼容：engine 以 "onnx" 开头=进程内 ONNX（dll_name=模型路径，空=内置资源段模型）；
    // 否则=远程 HTTP（http(s):// 完整/基础 URL 或别名 yolo/yolo_http 等）。
    // ONNX 类别名优先自动读取模型内嵌 metadata（ultralytics export 自带），无需 --labels；
    // 无 metadata 或非 ultralytics 导出时才需要 --labels=a,b,c 或 --labels=@classes.txt 兜底。
    // argv 空格分隔，ONNX 另支持 --conf=0.25 --iou=0.45（detect 传 <=0 时的默认阈值）
    // 与 --threads=N（推理线程数，默认 0=交给 ORT 用物理核；多实例并发时建议压到 1~2
    // 免得 N 个会话各开满线程互相抢核。2026-11-21 实测放开线程比写死单线程快 3.5~4.8 倍）。
    // GPU：--device=N 启用 DirectML（0=默认适配器）；需同版本 DML 版 onnxruntime.dll
    // + DirectML.dll。缺失时自动回退 CPU 并在日志标注，不返回失败；--device=cpu 可显式回 CPU。
    // 任务提示：--task=detect|segment|classify|pose|obb、--nc=N、--kpt=N（元数据缺失时）。
    long SetYoloEngine(_In_ const wchar_t *path_of_engine, _In_ const wchar_t *dll_name, _In_ const wchar_t *argv);
    void YoloDetect(_In_ long x1, _In_ long y1, _In_ long x2, _In_ long y2, _In_ double conf, _In_ double iou,
                    _Out_ std::wstring &retjson, _Out_ long *ret);

    // 【YOLO 增强 2026-11-21】带过滤/截断的检测。
    // json_options 形如 {"class_ids":[0,2],"labels":["person"],"max_results":20}，
    // 空串等价于不过滤（但仍输出更完整的 JSON）。语义：
    //   - class_ids 与 labels 同时给时取**并集**（满足任一即保留）
    //   - max_results 按 confidence 降序截断（要"最可信的 N 个"，不是"原序前 N 个"）
    //   - 非法 options（坏 JSON / 未知字段 / 类型错）返回 -1，与"没检测到"区分
    // 返回 JSON 比旧接口多 total/returned/index/cx/cy/w/h 字段。
    // 坐标口径与 OCR/找图一致（截图偏移 + 客户区偏移）。
    long YoloDetectEx2(_In_ long x1, _In_ long y1, _In_ long x2, _In_ long y2, _In_ double conf, _In_ double iou,
                       _In_ const wchar_t *json_options, _Out_ std::wstring &retjson, _Out_ long *ret);
    // 文件版 Ex2：同样支持 class_ids/labels/max_results 过滤。
    // 存在的理由：屏幕版依赖绑定窗口，无窗口环境下无法验证过滤逻辑；
    // 文件版让过滤/截断/JSON 契约能用真实模型闭环验证。
    long YoloDetectFromFileEx2(_In_ const wchar_t *file_name, _In_ double conf, _In_ double iou,
                                _In_ const wchar_t *json_options, _Out_ std::wstring &retjson, _Out_ long *ret);
    void YoloDetectFromFile(_In_ const wchar_t *file_name, _In_ double conf, _In_ double iou, _Out_ std::wstring &retjson,
                            _Out_ long *ret);

    // ---- YOLO 任务维度（批 C 2026-11-22）----
    // 除检测外补齐 Classify / Segment / Pose / Obb 四类任务（对齐 ultralytics 导出格式）。
    //
    // 任务由模型决定：SetYoloEngine 加载时先读 ONNX metadata 的 task，读不到再按输出
    // 形状推断。任务不符时返回 -1（"模型不支持该任务"），0 表示"没有结果"——两者
    // 语义不同，调用方必须分开处理。
    //
    // 整图分类。top_k<=0 取 5。JSON：
    //   {"ok":1,"code":0,"task":"classify","returned":K,
    //    "results":[{"class_id":2,"label":"charlie","score":0.71}, ...]}  // 按 score 降序
    long YoloClassify(_In_ long x1, _In_ long y1, _In_ long x2, _In_ long y2, _In_ long top_k,
                      _Out_ std::wstring &retjson, _Out_ long *ret);
    long YoloClassifyFromFile(_In_ const wchar_t *file_name, _In_ long top_k, _Out_ std::wstring &retjson, _Out_ long *ret);
    // 实例分割。每条的 polygon 是掩码外轮廓（原图像素坐标，可能为空 = 掩码过小），
    // mask_area 是该掩码在原图尺度的像素面积。JSON：
    //   {"ok":1,"code":0,"task":"segment","total":N,"returned":N,
    //    "results":[{"class_id":0,"label":"blob","bbox":[x1,y1,x2,y2],"confidence":0.9,
    //                "mask_area":64.0,"polygon":[[x,y],...]}]}
    long YoloSegment(_In_ long x1, _In_ long y1, _In_ long x2, _In_ long y2, _In_ double conf, _In_ double iou,
                     _Out_ std::wstring &retjson, _Out_ long *ret);
    long YoloSegmentFromFile(_In_ const wchar_t *file_name, _In_ double conf, _In_ double iou, _Out_ std::wstring &retjson,
                             _Out_ long *ret);
    // 姿态估计（Pose）。每条结果含检测框 + keypoints（按模型顺序，坐标原图尺度）。
    // JSON：
    //   {"ok":1,"code":0,"task":"pose","total":N,"returned":N,
    //    "results":[{"class_id":0,"label":"person","bbox":[x1,y1,x2,y2],"confidence":0.9,
    //                "keypoints":[{"x":100.5,"y":80.0,"confidence":0.95}, ...]}]}
    long YoloPose(_In_ long x1, _In_ long y1, _In_ long x2, _In_ long y2, _In_ double conf, _In_ double iou,
                  _Out_ std::wstring &retjson, _Out_ long *ret);
    long YoloPoseFromFile(_In_ const wchar_t *file_name, _In_ double conf, _In_ double iou, _Out_ std::wstring &retjson,
                          _Out_ long *ret);
    // 旋转框（OBB）。angle 为弧度，corners 为四角点（原图坐标，顺序左上/右上/右下/左下）。
    // JSON：
    //   {"ok":1,"code":0,"task":"obb","total":N,"returned":N,
    //    "results":[{"class_id":0,"label":"ship","confidence":0.9,
    //                "center":[cx,cy],"size":[w,h],"angle":0.32,"angle_deg":18.3,
    //                "bbox":[x1,y1,x2,y2],"corners":[[x,y],...]}]}
    long YoloObb(_In_ long x1, _In_ long y1, _In_ long x2, _In_ long y2, _In_ double conf, _In_ double iou,
                 _Out_ std::wstring &retjson, _Out_ long *ret);
    long YoloObbFromFile(_In_ const wchar_t *file_name, _In_ double conf, _In_ double iou, _Out_ std::wstring &retjson,
                         _Out_ long *ret);
    // 当前已加载模型的元信息（未加载时 task="none"）。用途：脚本启动时自检"模型选对了没"，
    // 而不是等到识别出一堆空结果才怀疑。JSON：
    //   {"ok":1,"task":"segment","task_code":2,"input_width":640,"input_height":640,
    //    "device":-1,"dml":0}
    // device=-1 表示 CPU；>=0 为实际生效的 DirectML 适配器序号（请求 GPU 但回退时也是 -1）。
    void YoloGetModelInfo(_Out_ std::wstring &retjson);

    // ---- YOLO 模型平台（2026-11-23）----
    // 命名模型注册表：可同时加载多个模型，推理始终走当前 active 模型。
    // 兼容旧入口：SetYoloEngine 等价于 YoloLoadModel("default", ...) 并激活。
    //
    // 加载 .onnx（明文）或 .opymodel（OPYMOD01 加密包）。加密包从 argv 读 --password=xxx
    // （密码不能含空格）；明文传了密码会被忽略。同名重复加载 = 替换旧模型。
    // retjson：{"ok":1,"name":"a","task":"detect","encrypted":0,"active":1}
    void YoloLoadModel(_In_ const wchar_t *name, _In_ const wchar_t *path, _In_ const wchar_t *argv,
                       _Out_ std::wstring &retjson, _Out_ long *ret);
    // 切换 active 模型。不存在 ret=0。
    void YoloUseModel(_In_ const wchar_t *name, _Out_ long *ret);
    // 释放指定模型；name 为空 = 释放全部。释放 active 后 active 为空（推理返 -1）。
    void YoloReleaseModel(_In_ const wchar_t *name, _Out_ long *ret);
    // 列出已加载模型：
    // {"ok":1,"active":"a","count":1,"models":[{"name":"a","task":"detect","encrypted":0,
    //   "input_width":640,"input_height":640,"device":-1,"dml":0}]}
    void YoloListModels(_Out_ std::wstring &retjson);
    // 列出推理设备（DXGI 枚举 + CPU）：
    // {"ok":1,"dml_available":0,"devices":[{"index":-1,"name":"CPU","type":"cpu"},
    //   {"index":0,"name":"...","type":"gpu","vram_mb":8192}]}
    void YoloListDevices(_Out_ std::wstring &retjson);
    // 把明文 .onnx 打成 OPYMOD01 加密包（AES-256-GCM + PBKDF2），或反向解包。
    // 密码为空/读写失败 ret=0。加密包在内存中解密后直接建会话，不落明文临时文件。
    void YoloEncryptModel(_In_ const wchar_t *src_path, _In_ const wchar_t *dst_path, _In_ const wchar_t *password,
                          _Out_ long *ret);
    void YoloDecryptModel(_In_ const wchar_t *src_path, _In_ const wchar_t *dst_path, _In_ const wchar_t *password,
                          _Out_ long *ret);

    // ---- 汇编模块（批 D 2026-11-22）----
    // 在目标进程执行一段**机器码**并取返回值（纯 BlackBone + Win32，无新依赖）。
    //   hwnd     目标窗口句柄；**0 = 当前进程**（便于先自测再上目标）
    //   code_hex 空格分隔的十六进制字节，如 "48 C7 C0 2A 00 00 00 C3"（需自带 ret）；
    //            支持 "??" 占位（按 0xCC/int3 填充，执行到立刻断下而不是跑飞）
    //   timeout_ms <=0 用默认 5000
    // 成功 JSON：{"ok":1,"code":0,"ret_hex":"2A","ret":42}
    // 失败 JSON：{"ok":0,"code":-1,"error":"..."}
    // ⚠ 只执行调用方给的机器码，插件不生成/不修改代码；执行完立即释放可执行内存。
    long AsmCall(_In_ LONG_PTR hwnd, _In_ const wchar_t *code_hex, _In_ long timeout_ms, _Out_ std::wstring &retjson,
                 _Out_ long *ret);
    // Assemble：汇编文本 -> 机器码（keystone / LLVM MC，静态链入，无新增 DLL 依赖）。
    //   asm_text 支持多语句（换行或 ';'），Intel 语法；**不自动补 ret**
    //   arch     32 / 64；0 = 按当前进程位数
    //   ⚠ 数字字面量：**裸数字是十进制**（`mov eax, 100` = 100），十六进制写 `0x64`。
    //     keystone 0.9.2 原实现无条件 radix=16（`mov eax, 100` 会变成 0x100=256，
    //     且写不出十进制），已在 3rd_party 构建脚本里修正，见 _wb_build_asm_libs.py。
    // 成功 JSON：{"ok":1,"code":0,"size":7,"count":1,"hex":"B8 2A 00 00 00 C3"}
    //   hex 为空格分隔，可直接作为 AsmCall 的 code_hex 入参（两接口格式同构）
    // 失败 JSON：{"ok":0,"code":-1,"error":"keystone 汇编失败: ..."}
    long Assemble(_In_ const wchar_t *asm_text, _In_ long arch, _Out_ std::wstring &retjson, _Out_ long *ret);
    // Disassemble：机器码 -> 汇编文本（capstone，静态链入）。
    //   bytes_hex 空格分隔十六进制字节（与 AsmCall/Assemble 同格式）
    //   address   起始虚拟地址，十六进制可带 0x；空串 = 0。影响输出 addr 与 jmp/call 目标解析
    //   count     最多解码指令数；<=0 = 解到字节用完
    //   arch      32 / 64；0 = 按当前进程位数
    // ⚠ arch 必须给对：x64 模式解 32 位代码会**静默出错码**（0x50 = push rax 而非 push eax），
    //   所以宁可要求显式声明，也不按"看起来像"去猜。
    // 成功 JSON：{"ok":1,"code":0,"mode":64,"count":2,
    //            "insns":[{"addr":"140001000","bytes":"B8 2A 00 00 00","mnemonic":"mov","op_str":"eax, 0x2a"},...]}
    long Disassemble(_In_ const wchar_t *bytes_hex, _In_ const wchar_t *address, _In_ long count, _In_ long arch,
                     _Out_ std::wstring &retjson, _Out_ long *ret);
    // 设置字库文件
    void SetDict(_In_ long idx, _In_ const wchar_t *file_name, _Out_ long *ret);
    // 获取指定字库中指定条目的字库信息
    void GetDict(_In_ long idx, _In_ long font_index, _Out_ std::wstring &retstr);
    // 设置内存字库文件
    void SetMemDict(_In_ long idx, _In_reads_bytes_(size) const void *data, _In_ long size, _Out_ long *ret);
    // 使用哪个字库文件进行识别
    void UseDict(_In_ long idx, _Out_ long *ret);
    // 给指定的字库中添加字库信息。
    //   dict_info 支持多行（换行分隔，每行一条 `名字$高,宽,点数$点阵HEX`）
    //   **整批原子**：任一行解析失败 → 一条都不写入，返回 0；
    //   返回值 = 成功处理的行数（单行成功 = 1，与旧行为一致）。
    void AddDict(_In_ long idx, _In_ const wchar_t *dict_info, _Out_ long *ret);
    // 从指定字库中删除字条。字条靠点阵唯一，dict_info 只取点阵部分（名字可留空）。
    //   返回删除条数：0 = 没找到该点阵，1 = 已删除。
    //   用途：做字库时发现个别字标错，不必 ClearDict 整槽重来。
    void RemoveDict(_In_ long idx, _In_ const wchar_t *dict_info, _Out_ long *ret);
    // 保存指定的字库到指定的文件中
    void SaveDict(_In_ long idx, _In_ const wchar_t *file_name, _Out_ long *ret);
    // 清空指定的字库
    void ClearDict(_In_ long idx, _Out_ long *ret);
    // 获取指定的字库中的字符数量
    void GetDictCount(_In_ long idx, _Out_ long *ret);
    // 获取当前使用的字库序号
    void GetNowDict(_Out_ long *ret);
    // 设置点阵二值图预处理。mode: 0关闭, 1去孤立点, 2再过滤小连通域, 3允许按 bridge_gap 补1像素断笔
    void SetBinaryPreprocess(_In_ long mode, _In_ long isolated_threshold, _In_ long min_component_area,
                             _In_ long bridge_gap, _Out_ long *ret);
    // 读取当前点阵二值图预处理参数
    void GetBinaryPreprocess(_Out_ long *mode, _Out_ long *isolated_threshold, _Out_ long *min_component_area,
                             _Out_ long *bridge_gap, _Out_ long *ret);
    // 根据指定的范围,以及指定的颜色描述，提取点阵信息，类似于大漠工具里的单独提取
    void FetchWord(_In_ long x1, _In_ long y1, _In_ long x2, _In_ long y2, _In_ const wchar_t *color, _In_ const wchar_t *word,
                   _Out_ std::wstring &retstr);
    // 根据相似度提取单个点阵字，适合有抗锯齿或轻微色差的文字
    void FetchWordEx(_In_ long x1, _In_ long y1, _In_ long x2, _In_ long y2, _In_ const wchar_t *color,
                     _In_ double sim, _In_ const wchar_t *word, _Out_ std::wstring &retstr);
    // 自动切出指定范围内的点阵字块，返回格式: x1,y1,x2,y2|...
    // 注意：min_word_h 是历史命名，本函数实际只按"字块宽度 >= min_word_h"过滤（不过滤高度）；
    // 需要宽高双重过滤请用 ExtractWordRectsEx。
    void ExtractWordRects(_In_ long x1, _In_ long y1, _In_ long x2, _In_ long y2, _In_ const wchar_t *color,
                          _In_ double sim, _In_ long min_word_h, _Out_ std::wstring &retstr);
    // 自动切出点阵字块，并按最小宽高过滤噪点；padding 用来给每个字块保留一点边缘
    void ExtractWordRectsEx(_In_ long x1, _In_ long y1, _In_ long x2, _In_ long y2, _In_ const wchar_t *color,
                            _In_ double sim, _In_ long min_word_w, _In_ long min_word_h, _In_ long padding,
                            _Out_ std::wstring &retstr);
    // 自动切字并按 words 的字符顺序生成多条点阵字库，返回多行字库文本
    // 注意：min_word_h 是历史命名，本函数实际只按"字块宽度 >= min_word_h"过滤（不过滤高度）；
    // 需要宽高双重过滤请用 FetchWordsEx。切出的字数与 words 长度不一致时返回空（日志有提示）。
    void FetchWords(_In_ long x1, _In_ long y1, _In_ long x2, _In_ long y2, _In_ const wchar_t *color, _In_ double sim,
                    _In_ const wchar_t *words, _In_ long min_word_h, _Out_ std::wstring &retstr);
    // 使用更细的切字参数批量生成点阵字库，适合截图里夹杂小噪点的情况
    void FetchWordsEx(_In_ long x1, _In_ long y1, _In_ long x2, _In_ long y2, _In_ const wchar_t *color,
                      _In_ double sim, _In_ const wchar_t *words, _In_ long min_word_w, _In_ long min_word_h,
                      _In_ long padding, _Out_ std::wstring &retstr);
    // 按指定字块生成多条点阵字库，rects 格式与 ExtractWordRects 的返回值一致
    void FetchWordsByRects(_In_ long x1, _In_ long y1, _In_ long x2, _In_ long y2, _In_ const wchar_t *color,
                           _In_ double sim, _In_ const wchar_t *words, _In_ const wchar_t *rects,
                           _Out_ std::wstring &retstr);
    // 返回指定区域按颜色规则二值化后的文本预览，ret 为前景点数量。
    // 文本格式：首行 "宽,高"；随后每行一个像素行，'#' 为前景点、'.' 为背景点（与区域像素一一对应）。
    // 预览与识别共用同一份二值图，并已包含 SetBinaryPreprocess 的预处理结果（默认 mode=1 去孤立点），
    // 因此可直接用返回值校准 color 颜色描述是否准确。
    void GetBinaryPreview(_In_ long x1, _In_ long y1, _In_ long x2, _In_ long y2, _In_ const wchar_t *color,
                          _In_ double sim, _Out_ std::wstring &retstr, _Out_ long *ret);
    // 返回点阵字库条目的文本预览，ret 为条目是否合法
    void GetWordPreview(_In_ const wchar_t *dict_info, _Out_ std::wstring &retstr, _Out_ long *ret);
    // 检查一段点阵字库文本，ret 为合法条目数量，retstr 为每条字库的摘要
    void CheckWordDict(_In_ const wchar_t *dict_info, _Out_ std::wstring &retstr, _Out_ long *ret);
    // 清理点阵字库文本，只保留合法条目，并统一输出为当前字库格式
    void NormalizeWordDict(_In_ const wchar_t *dict_info, _Out_ std::wstring &retstr, _Out_ long *ret);
    // 按顺序替换点阵字库条目的字名，适合制作后统一校正标签
    void RenameWordDict(_In_ const wchar_t *dict_info, _In_ const wchar_t *words, _Out_ std::wstring &retstr,
                        _Out_ long *ret);
    // 识别这个范围内所有满足条件的词组，这个识别函数不会用到字库. 只是识别大概形状的位置
    void GetWordsNoDict(_In_ long x1, _In_ long y1, _In_ long x2, _In_ long y2, _In_ const wchar_t *color,
                        _Out_ std::wstring &retstr);
    // 在使用GetWords进行词组识别以后,可以用此接口进行识别词组数量的计算
    void GetWordResultCount(_In_ const wchar_t *result, _Out_ long *ret);
    // 在使用GetWords进行词组识别以后,可以用此接口进行识别各个词组的坐标
    void GetWordResultPos(_In_ const wchar_t *result, _In_ long index, _Out_ long *x, _Out_ long *y, _Out_ long *ret);
    // 在使用GetWords进行词组识别以后,可以用此接口进行识别各个词组的内容
    void GetWordResultStr(_In_ const wchar_t *result, _In_ long index, _Out_ std::wstring &ret_str);
    // 免字库 OCR 家族速查（免字库 = 未 SetDict/UseDict，走内置 ONNX 引擎；有字库 = 点阵字库匹配）：
    //   AutoOcr / AutoOcrFromFile（归一入口，推荐）
    //                                    color 为空 → 原图直识别（同 OcrAuto）；
    //                                    color 非空 → 按颜色二值化（同 OcrAutoColor，含字库协同）
    //   OcrAuto / OcrAutoFromFile        显式原图：不二值化，原图直喂引擎，无需颜色（= AutoOcr 空色分支）
    //   Ocr / OcrFromFile / OcrEx        免字库时不二值化（颜色不参与）；有字库时按颜色二值化
    //   OcrAutoColor / OcrAutoColorFromFile / OcrAutoColorLine / OcrAutoColorEx
    //                                    显式二值化：先按颜色二值化（支持 "@背景色" 反白）再识别，必须给颜色
    //   ⚠️ sim 语义双关：免字库路径 = 置信度阈值（默认 0.7，ONNX 置信度 ≈0.85+，传 1.0 会全滤光）；
    //     字库路径 = 颜色/点阵相似度 (0.1-1.0)。
    // 免字库 OCR 归一入口（推荐）：color 为空走原图直识别，非空走按颜色二值化。
    // 两分支行为分别与 OcrAuto / OcrAutoColor 完全一致，已有脚本可无缝换用本入口。
    void AutoOcr(_In_ long x1, _In_ long y1, _In_ long x2, _In_ long y2, _In_ const wchar_t *color, _In_ double sim,
                 _Out_ std::wstring &ret_str);
    // 识别屏幕范围(x1,y1,x2,y2)内符合color_format的字符串,并且相似度为sim,sim取值范围(0.1-1.0),
    // 已装载字库时按 color_format 二值化后做字库识别；未装载字库时走免字库引擎，直接对整幅区域
    // 原图识别（color_format 不参与过滤，要按颜色识别请用 OcrAutoColor 系接口）
    void Ocr(_In_ long x1, _In_ long y1, _In_ long x2, _In_ long y2, _In_ const wchar_t *color, _In_ double sim,
             _Out_ std::wstring &ret_str);
    // 同 Ocr，但返回识别到的字符串以及每个字符的坐标（已装载字库时结果按阅读顺序拼接：包围盒纵向重叠聚类成行、行内按 x 排序）
    void OcrEx(_In_ long x1, _In_ long y1, _In_ long x2, _In_ long y2, _In_ const wchar_t *color, _In_ double sim,
               _Out_ std::wstring &ret_str);
    // 区域截图后按颜色二值化再做免字库 OCR：输入区域、颜色、相似度（置信度阈值，默认 0.7）。
    void OcrAutoColor(_In_ long x1, _In_ long y1, _In_ long x2, _In_ long y2, _In_ const wchar_t *color, _In_ double sim,
                      _Out_ std::wstring &ret_str);
    // 单行快模式 OcrAutoColor：颜色二值化后整图直接 rec（跳过检测），适用于读出区/固定单行文本。
    // 区域须只含单行文本，否则结果语义由使用方承担。
    // 行切分参数为硬编码阈值（OC6，暂不提供参数）：行间隙 >3px 切段、段高 <5px 视为噪点丢弃。
    void OcrAutoColorLine(_In_ long x1, _In_ long y1, _In_ long x2, _In_ long y2, _In_ const wchar_t *color,
                          _In_ double sim, _Out_ std::wstring &ret_str);
    // 结构化 OcrAutoColor：输出 "x1,y1,x2,y2,conf,text|..."（bbox 为客户区坐标，相对绑定窗口客户区左上角），返回命中行数。
    long OcrAutoColorEx(_In_ long x1, _In_ long y1, _In_ long x2, _In_ long y2, _In_ const wchar_t *color,
                        _In_ double sim, _Out_ std::wstring &ret_str);
    // 【P0 2026-11-20】结构化 OCR（JSON）：逐字置信度 + 逐字坐标。
    // 返回 JSON 数组：
    //   [{"text","conf","is_partial",["conf_partial",]"box":[x1,y1,x2,y2],
    //     "partial_chars","chars":[{"ch","conf","x1","x2"}]}]
    // 坐标系与旧接口一致（原图像素坐标，含入口放大自动除回）。
    // ⚠ conf（整行）与 chars[].conf（逐字）**口径不同、不可互推**：整行 conf 是全部
    //   时间步（含 blank）的概率均值（与旧 API 完全一致）；逐字 conf 仅统计该字符
    //   占用步。is_partial=true 时额外给 conf_partial=conf*0.8，由调用方决定是否采用。
    // ⚠ partial_chars=true 表示该行经过倾斜旋转校正，逐字 x 已退化为整行框范围
    //   （chars 仍逐字给出 ch/conf，但 x1/x2 等于 box 左右边界）。
    // color/sim 语义与该接口原版一致（有字库时走字库匹配，此时 chars 为空数组）。
    void OcrEx2(_In_ long x1, _In_ long y1, _In_ long x2, _In_ long y2, _In_ const wchar_t *color,
                _In_ double sim, _Out_ std::wstring &ret_str);
    // 同 OcrEx2，但先按 color_format 二值化（与 OcrAutoColorEx 同源）。
    // ⚠ 颜色格式为 **RGB 顺序** `RRGGBB-DDDDDD`（不是 BGR）：红色写 `e01111` 而非 `1111e0`。
    //   多色用 `|` 分隔 = 并集（各色按容差二值化后取并集，只跑一次 det+rec）。
    // 智能选路（自动）：图里命中 color 的像素占比 < 0.1% 时，二值化会得到全背景图、
    //   结果恒空，此时自动回退到原图直识（与 OcrEx2 行为一致）。故白字/无目标色的
    //   图传色也不会失败。实测 8 张真实小图：v6 从 3/8 全对提升到 8/8 全对。
    long OcrAutoColorEx2(_In_ long x1, _In_ long y1, _In_ long x2, _In_ long y2, _In_ const wchar_t *color,
                         _In_ double sim, _Out_ std::wstring &ret_str);
    // 在屏幕范围(x1,y1,x2,y2)内,查找string(可以是任意个字符串的组合),并返回符合color_format的坐标位置



    // ===== 内存直操作二期：识别入口的内存版（2026-11-21）=====
    // 设计见 docs/2026-11/内存直操作二期_全家桶_方案_20261121.md
    //
    // 与屏幕版的**唯一区别**是"图从哪来"：
    //   屏幕版 x1..y2 是客户区坐标；内存版 x1..y2 是**图像内坐标**（相对 img 句柄左上角）。
    //   其余参数逐字相同，出参坐标也是图像内坐标（不受窗口偏移影响）。
    //   区域传 (0,0,0,0) 表示**整张图**（脚本里"全图查找"的自然写法）。
    //
    // 实现上直接复用屏幕版：只把"截屏"换成"从句柄取图"，偏移清零。
    // 因此行为与屏幕版严格一致，这是结构保证而非测试推断。
    //
    // 句柄无效/已释放时不执行识别，返回与屏幕版"截屏失败"相同的默认值。

    // 结构化 OCR（逐字坐标 + conf），输出与 OcrEx2 完全相同
    void OcrFromMemEx2(_In_ const wchar_t *image_handle, _In_ long x1, _In_ long y1, _In_ long x2, _In_ long y2,
                       _In_ const wchar_t *color, _In_ double sim, _Out_ std::wstring &retjson);
    // 颜色二值化 OCR（含智能选路），返回值 = 行数
    long OcrAutoColorFromMemEx2(_In_ const wchar_t *image_handle, _In_ long x1, _In_ long y1, _In_ long x2,
                                _In_ long y2, _In_ const wchar_t *color, _In_ double sim,
                                _Out_ std::wstring &retjson);

    // 找图：ret = 命中的图片序号（-1=未找到，大漠索引语义）
    void FindPicFromMem(_In_ const wchar_t *image_handle, _In_ long x1, _In_ long y1, _In_ long x2, _In_ long y2,
                        _In_ const wchar_t *files, _In_ const wchar_t *delta_color, _In_ double sim, _In_ long dir,
                        _Out_ long *x, _Out_ long *y, _Out_ long *ret);
    void FindPicExFromMem(_In_ const wchar_t *image_handle, _In_ long x1, _In_ long y1, _In_ long x2, _In_ long y2,
                          _In_ const wchar_t *files, _In_ const wchar_t *delta_color, _In_ double sim, _In_ long dir,
                          _Out_ std::wstring &retstr);

    // 找色 / 取色 / 比色 / 计数
    void FindColorFromMem(_In_ const wchar_t *image_handle, _In_ long x1, _In_ long y1, _In_ long x2, _In_ long y2,
                          _In_ const wchar_t *color, _In_ double sim, _In_ long dir, _Out_ long *x, _Out_ long *y,
                          _Out_ long *ret);
    void FindColorExFromMem(_In_ const wchar_t *image_handle, _In_ long x1, _In_ long y1, _In_ long x2, _In_ long y2,
                            _In_ const wchar_t *color, _In_ double sim, _In_ long dir, _Out_ std::wstring &retstr);
    void GetColorFromMem(_In_ const wchar_t *image_handle, _In_ long x, _In_ long y, _Out_ std::wstring &retstr);
    void CmpColorFromMem(_In_ const wchar_t *image_handle, _In_ long x, _In_ long y, _In_ const wchar_t *color,
                         _In_ double sim, _Out_ long *ret);
    void GetColorNumFromMem(_In_ const wchar_t *image_handle, _In_ long x1, _In_ long y1, _In_ long x2, _In_ long y2,
                            _In_ const wchar_t *color, _In_ double sim, _Out_ long *ret);

    // 多色串 / 色块
    void FindMultiColorFromMem(_In_ const wchar_t *image_handle, _In_ long x1, _In_ long y1, _In_ long x2,
                               _In_ long y2, _In_ const wchar_t *first_color, _In_ const wchar_t *offset_color,
                               _In_ double sim, _In_ long dir, _Out_ long *x, _Out_ long *y, _Out_ long *ret);
    void FindMultiColorExFromMem(_In_ const wchar_t *image_handle, _In_ long x1, _In_ long y1, _In_ long x2,
                                 _In_ long y2, _In_ const wchar_t *first_color, _In_ const wchar_t *offset_color,
                                 _In_ double sim, _In_ long dir, _Out_ std::wstring &retstr);
    void FindColorBlockExFromMem(_In_ const wchar_t *image_handle, _In_ long x1, _In_ long y1, _In_ long x2,
                                 _In_ long y2, _In_ const wchar_t *color, _In_ double sim, _In_ long count,
                                 _In_ long height, _In_ long width, _Out_ std::wstring &retstr);

    // YOLO（内存版）。ret = 检测条数；JSON 结构同屏幕版
    void YoloDetectFromMem(_In_ const wchar_t *image_handle, _In_ long x1, _In_ long y1, _In_ long x2, _In_ long y2,
                           _In_ double conf, _In_ double iou, _Out_ std::wstring &retjson, _Out_ long *ret);
    // 带过滤/截断；json_options 同 YoloDetectEx2。非法 options 返回 -1
    long YoloDetectFromMemEx2(_In_ const wchar_t *image_handle, _In_ long x1, _In_ long y1, _In_ long x2,
                              _In_ long y2, _In_ double conf, _In_ double iou, _In_ const wchar_t *json_options,
                              _Out_ std::wstring &retjson, _Out_ long *ret);
    // ===== 音频音量（2026-11-21，WASAPI 会话级 + 系统级）=====
    // 设计见 docs/2026-11/音量模块_方案_20261121.md。
    //
    // 语义区分（误用率最高，务必看清）：
    //   volume（滑块值）**与出声大小无直接关系**：滑块 1.0 也可能当前无声。
    //   判断"现在有没有声音"要看 **peak**。
    //   found=false = 该 PID 没有音频会话，**不等于没声音**
    //   （可能在放系统音，或进程刚启动还没建会话）。
    //
    // 返回 JSON：
    //   {"ok":1,"code":0,"found":true,"pid":12345,"muted":false,"playing":true,
    //    "volume":0.75,"peak":0.3421,"error":""}
    //   ok=1 表示查询链路走通（不代表有声音）；ok=0 看 error。
    //   code: 0 成功 / -1 无音频设备 / -2 COM 或激活失败 / -3 枚举失败
    //         / -4 参数非法 / -5 设置失败
    //
    // 无状态：峰值保持/衰减/阈值判定留给调用方
    // （每次调用重新枚举，不缓存 COM 指针）。
    void AudioQueryWindow(_In_ long long hwnd, _Out_ std::wstring &retjson);
    void AudioQueryPid(_In_ long pid, _Out_ std::wstring &retjson);
    // 纯取窗口所属 PID（不碰 COM）。ret: 失败返回 0。
    void AudioGetPidFromHwnd(_In_ long long hwnd, _Out_ long *ret);
    // 设置会话滑块音量（0.0~1.0，越界 clamp）。ret: 1 成功 / 0 失败（与全库
    // 「0=失败、非 0=成功」一致）。具体失败原因写 __op.log；要结构化错误码的
    // 用 AudioQueryPid 的 code 字段（-1 无设备 / -2 COM / -3 枚举 / -5 设置失败）。
    void AudioSetSessionVolume(_In_ long pid, _In_ double volume, _Out_ long *ret);
    // ret: 1 成功 / 0 失败，口径同 AudioSetSessionVolume。
    void AudioSetSessionMute(_In_ long pid, _In_ long muted, _Out_ long *ret);
    // 系统主音量（同一套 enumerator 上多取 IAudioEndpointVolume，成本很低）。
    void AudioGetSystemVolume(_Out_ std::wstring &retjson);
    // ret: 1 成功 / 0 失败，口径同上。
    void AudioSetSystemVolume(_In_ double volume, _Out_ long *ret);
    void AudioSetSystemMute(_In_ long muted, _Out_ long *ret);

    // ===== 图像句柄注册表（2026-11-21，内存直操作一期地基）=====
    // 目的：让调用方把**自己手上已有的像素**直接交给 op，省掉落盘再读盘的往返。
    // 句柄形态为字符串 img:<n>，与既有 pic:<路径> 的显示输入风格一致，
    // 因此在 C API / COM 层都是普通 BSTR，客户端零学习成本。
    // 设计见 docs/2026-11/内存模块优化_图像句柄注册表_方案_20261121.md。
    //
    // 注册后像素**不可变**：句柄可被多线程并发读，允许就地写会引入数据竞争。
    //   需要变体请新建句柄（CropImage 即如此）。
    // 注册表达上限（256）时**拒绝新增并返回空串**，不会淘汰既有句柄。

    // 从原始像素内存注册图像。fmt: bgra(默认) / bgr / gray（也接受 4/3/1）。
    // stride<=0 表示紧凑排列（width*bpp）；stride>width*bpp 视为带行填充，逐行复制。
    // 成功返回句柄字符串，失败返回空串（参数非法 / 未知格式 / 尺寸超 64MB / 注册表满）。
    void LoadImageMem(_In_ void *data, _In_ long width, _In_ long height, _In_ long stride,
                      _In_ const wchar_t *fmt, _Out_ std::wstring &ret_handle);
    // 从 BMP 字节流注册（自动解析 24/32bpp、top-down/bottom-up、行填充）。
    void LoadImageBmp(_In_ void *data, _In_ long size, _Out_ std::wstring &ret_handle);
    // 释放单个图像句柄。ret: 1=成功，0=句柄不存在。
    void FreeImage(_In_ const wchar_t *handle, _Out_ long *ret);
    // 释放全部图像句柄。ret: 被释放的数量。
    void FreeAllImages(_Out_ long *ret);
    // 查询图像尺寸。channels 恒为 4（内部统一 BGRA）。ret: 1=成功，0=句柄不存在。
    void GetImageSize(_In_ const wchar_t *handle, _Out_ long *width, _Out_ long *height, _Out_ long *channels,
                      _Out_ long *ret);
    // 读回像素。fmt: raw(BGRA 紧凑) / bmp(带 BMP 头的完整字节流)。
    // buffer 为调用方提供的缓冲区，buffer_size 为其容量；ret: 实际写出的字节数（不足则 0）。
    void GetImageData(_In_ const wchar_t *handle, _In_ const wchar_t *fmt, _In_ void *buffer,
                      _In_ long buffer_size, _Out_ long *ret);
    // 裁剪并注册为新句柄（深拷贝）。区域自动 clamp 到图像内；空区域返回空串。
    void CropImage(_In_ const wchar_t *handle, _In_ long x, _In_ long y, _In_ long width, _In_ long height,
                   _Out_ std::wstring &ret_handle);
    // 在图像句柄上做多目标查找（复用 P1 FindStrBatch 的匹配内核）。
    // 返回值 = 目标数；句柄无效或 JSON 非法返回 -1。坐标从图像左上角算起。
    long FindStrMem(_In_ const wchar_t *handle, _In_ const wchar_t *json_targets, _In_ double sim,
                    _Out_ std::wstring &ret_json);
    // 当前注册表条目数（诊断用）。
    void GetImageCount(_Out_ long *ret);
    void FindStr(_In_ long x1, _In_ long y1, _In_ long x2, _In_ long y2, _In_ const wchar_t *strs, _In_ const wchar_t *color,
                 _In_ double sim, _Out_ long *retx, _Out_ long *rety, _Out_ long *ret);
    // 返回符合color_format的所有坐标位置
    void FindStrEx(_In_ long x1, _In_ long y1, _In_ long x2, _In_ long y2, _In_ const wchar_t *strs, _In_ const wchar_t *color,
                   _In_ double sim, _Out_ std::wstring &retstr);
    // 【P1 2026-11-21】多目标结构化查找：一次 det+rec，引擎内匹配全部目标。
    // json_targets 为 JSON 数组，元素可为主字符串或配置对象：
    //   {"id","text","match":"contains|exact|prefix|suffix|regex",
    //    "fuzzy"(0..3),"max_hits","ci","confirmed_only"}
    // 返回每个目标的全部命中，含**字符级**精确范围 cx1/cx2（来自 P0 逐字坐标；
    // 旋转降级或已装载字库时退化为整行范围，char_precise=false）。
    // 返回值 = 目标数；json_targets 非法（解析失败/未知字段/未知 match）返回 -1。
    // ⚠ 匹配不跨行：命中片段必须完整落在同一行内。
    long FindStrBatch(_In_ long x1, _In_ long y1, _In_ long x2, _In_ long y2,
                      _In_ const wchar_t *json_targets, _In_ const wchar_t *color, _In_ double sim,
                      _Out_ std::wstring &ret_json);
    // 【P1】单目标增强查找（标量入参）。flags 位：
    //   1=regex  2=exact（默认 contains）  4=ci(ASCII)  64=fuzzy<=1  128=fuzzy<=2
    //   256=confirmed_only；位 8/16/32 预留。位 1/2/64/128 互斥 -> 返回 -1。
    // 返回值 = 命中数；返回 JSON 与 FindStrBatch 的单目标 results[0] 同构。
    long FindStrEx2(_In_ long x1, _In_ long y1, _In_ long x2, _In_ long y2, _In_ const wchar_t *pattern,
                    _In_ const wchar_t *color, _In_ double sim, _In_ long flags, _Out_ std::wstring &ret_json);
    // 【P3 2026-11-21】上一次 OCR 的分项耗时（det/cls/rec 分别计时），JSON 输出。
    // 不触发新的 OCR。valid=false = 还没跑过 OCR 或引擎不支持。
    void OcrLastProfile(_Out_ std::wstring &ret_json);
    // 识别屏幕范围(x1,y1,x2,y2)内的字符串，无需指定颜色。
    // ⚠️ 并非"自动二值化"：免字库时原图 BGRA 直喂 ONNX 引擎（引擎自带预处理，二值化反而可能丢信息）；
    // 已装载字库时按空颜色串二值化 + 字库匹配（≈无颜色筛选）。要按颜色二值化请用 OcrAutoColor 系。
    void OcrAuto(_In_ long x1, _In_ long y1, _In_ long x2, _In_ long y2, _In_ double sim, _Out_ std::wstring &ret_str);
    // 从文件中识别图片，规则同 Ocr
    void OcrFromFile(_In_ const wchar_t *file_name, _In_ const wchar_t *color_format, _In_ double sim, _Out_ std::wstring &retstr);
    // 文件版 OcrAutoColor：读取图片后按颜色二值化再做免字库 OCR。
    void OcrAutoColorFromFile(_In_ const wchar_t *file_name, _In_ const wchar_t *color_format, _In_ double sim, _Out_ std::wstring &retstr);
    // 从文件中识别图片，无需指定颜色，规则同 OcrAuto
    void OcrAutoFromFile(_In_ const wchar_t *file_name, _In_ double sim, _Out_ std::wstring &retstr);
    // 查找频幕中的直线
    void FindLine(_In_ long x1, _In_ long y1, _In_ long x2, _In_ long y2, _In_ const wchar_t *color, _In_ double sim,
                  _Out_ std::wstring &retstr);
    // 查找屏幕中的直线(扩展版): 额外返回该直线上的点数(霍夫累加器峰值)。
    // FindLine 无阈值、永远返回一条"最强"直线，即便图中并无直线也会返回噪点拟合的结果；
    // 本扩展把点数一并输出，调用者可据此判断结果是否可信(点数越大越可信，0 表示未截图/无前景点)。
    void FindLineEx(_In_ long x1, _In_ long y1, _In_ long x2, _In_ long y2, _In_ const wchar_t *color,
                    _In_ double sim, _Out_ std::wstring &retstr, _Out_ long *ret);
    // 查找直线(增强版): 二值化走孤立点去噪 + min_points 阈值。
    // 峰值点数 < min_points 时 retstr 置空(视为无可靠直线，根治幻觉线)，ret 仍返回真实峰值点数供阈值标定。
    void FindLineExS(_In_ long x1, _In_ long y1, _In_ long x2, _In_ long y2, _In_ const wchar_t *color,
                     _In_ double sim, _In_ long min_points, _Out_ std::wstring &retstr, _Out_ long *ret);

    // 内存读写（大漠兼容）。hwnd=0 时优先作用于已绑定窗口；未绑定才是本进程，
    // 想读本进程请先解绑（UnBindWindow）。address 支持十六进制/四则运算/模块基址 <mod>/多级指针 [addr+off]。
    // 向某进程写入数据
    void WriteData(_In_ LONG_PTR hwnd, _In_ const wchar_t *address, _In_ const wchar_t *data, _In_ long size, _Out_ long *ret);
    // 读取数据（大写十六进制串；失败返回空串。size 超 data 实际字节时补零，详见日志）
    void ReadData(_In_ LONG_PTR hwnd, _In_ const wchar_t *address, _In_ long size, _Out_ std::wstring &retstr);
    // 大漠兼容内存接口
    void ReadInt(_In_ LONG_PTR hwnd, _In_ const wchar_t *address, _In_ long type, _Out_ int64_t *ret);
    void WriteInt(_In_ LONG_PTR hwnd, _In_ const wchar_t *address, _In_ long type, _In_ int64_t value, _Out_ long *ret);
    void ReadFloat(_In_ LONG_PTR hwnd, _In_ const wchar_t *address, _Out_ float *ret);
    void WriteFloat(_In_ LONG_PTR hwnd, _In_ const wchar_t *address, _In_ float value, _Out_ long *ret);
    void ReadDouble(_In_ LONG_PTR hwnd, _In_ const wchar_t *address, _Out_ double *ret);
    void WriteDouble(_In_ LONG_PTR hwnd, _In_ const wchar_t *address, _In_ double value, _Out_ long *ret);
    void ReadString(_In_ LONG_PTR hwnd, _In_ const wchar_t *address, _In_ long type, _In_ long len, _Out_ std::wstring &retstr);
    void WriteString(_In_ LONG_PTR hwnd, _In_ const wchar_t *address, _In_ long type, _In_ const wchar_t *value, _Out_ long *ret);
    // 特征码搜索内存：pattern 十六进制串支持 ?? 通配；range "start-end" 十六进制，空=全部已提交可读区。
    // 返回 "addr|addr|..." 大写十六进制；count<=0 用默认上限 1024。空范围扫全空间最坏数十秒，建议收窄 range。
    void FindData(_In_ LONG_PTR hwnd, _In_ const wchar_t *addr_range, _In_ const wchar_t *string, _Out_ std::wstring &retstr);
    void FindDataEx(_In_ LONG_PTR hwnd, _In_ const wchar_t *addr_range, _In_ const wchar_t *string, _In_ long step,
                    _In_ long count, _Out_ std::wstring &retstr);
    // 取目标进程（hwnd=0 为本进程）模块基址，返回大写十六进制串；失败返回空。
    void GetModuleBaseAddr(_In_ LONG_PTR hwnd, _In_ const wchar_t *module, _Out_ std::wstring &retstr);
    // ---- 内存三件套 + 模块列表（P2 第二十二批 2026-10-15，对齐 OLA 内存常用子集）----
    // 模块大小（字节），失败/未找到返回 0。module 为模块基名（大小写不敏感）。
    void GetModuleSize(_In_ LONG_PTR hwnd, _In_ const wchar_t *module, _Out_ long *ret);
    // 模块列表 JSON：{"ok":1,"modules":[{"name":..,"path":..,"base":"HEX","size":n},...]}。
    void GetModuleInfoList(_In_ LONG_PTR hwnd, _Out_ std::wstring &retstr);
    // 修改地址保护属性，ret=1 成功 0 失败，old_protect 带回原属性（PAGE_* 常量）。
    // address 支持表达式：十六进制/四则运算/<module>/多级指针 [addr+off]。
    void VirtualProtectEx(_In_ LONG_PTR hwnd, _In_ const wchar_t *address, _In_ long size, _In_ long new_protect,
                          _Out_ long *old_protect, _Out_ long *ret);
    // 在目标进程分配内存，返回分配地址大写十六进制串；失败返回空。
    // alloc_type/protect 传 0 用默认 MEM_COMMIT|MEM_RESERVE / PAGE_READWRITE。
    void VirtualAllocEx(_In_ LONG_PTR hwnd, _In_ const wchar_t *address, _In_ long size, _In_ long alloc_type,
                        _In_ long protect, _Out_ std::wstring &retstr);

    // ---- 文件模块（P1 新增 2026-10-15，参考 OLA 文件模块常用子集）----
    // 全部路径为宽字符绝对/相对路径；返回值约定与窗口系一致：ret=1 成功，0 失败（路径非法/不存在/拒绝访问）。
    // 判断文件或文件夹是否存在（file_name 可为目录）
    void FileExists(_In_ const wchar_t *file_name, _Out_ long *ret);
    // 创建文件夹（含多级父目录，已存在视为成功）
    void CreateFolder(_In_ const wchar_t *folder_name, _Out_ long *ret);
    // 递归删除文件夹及其全部内容；路径非法/不存在返回 0
    void DeleteFolder(_In_ const wchar_t *folder_name, _Out_ long *ret);
    // 遍历文件夹，wildcard 支持 * ?（空=全部）。返回全路径 "p1|p2|..."，无匹配返回空串。
    void GetFileList(_In_ const wchar_t *folder, _In_ const wchar_t *wildcard, _Out_ std::wstring &retstr);
    void CopyFile(_In_ const wchar_t *src_file, _In_ const wchar_t *dst_file, _Out_ long *ret);
    void MoveFile(_In_ const wchar_t *src_file, _In_ const wchar_t *dst_file, _Out_ long *ret);
    void DeleteFile(_In_ const wchar_t *file_name, _Out_ long *ret);
    // 文件大小（字节，int64）。ret=1 成功，0 失败。
    void GetFileSize(_In_ const wchar_t *file_name, _Out_ int64_t *size, _Out_ long *ret);
    // 读取文本文件全部内容（按 UTF-8 解码；BOM 自动剔除）。失败/空文件返回空串。
    void ReadFileString(_In_ const wchar_t *file_name, _Out_ std::wstring &retstr);
    // 写入文本文件（UTF-8 带 BOM，覆盖写）。ret=1 成功。
    void WriteStringToFile(_In_ const wchar_t *file_name, _In_ const wchar_t *content, _Out_ long *ret);
    // 路径解析：flag 0=文件名含扩展名 1=仅文件名 2=仅扩展名(含.) 3=目录部分
    void GetFileName(_In_ const wchar_t *file_path, _In_ long flag, _Out_ std::wstring &retstr);
    // 相对路径转绝对路径（不做存在性检查，仅规范化 .\ ..\ 与斜杠）
    void ToAbsolutePath(_In_ const wchar_t *path, _Out_ std::wstring &retstr);

    // ---- 组合键（P1 新增 2026-10-15）----
    // 发送组合键，combo 形如 "ctrl+alt+a" / "shift+f1" / "win+d"。修饰键支持 ctrl/control/alt/shift/win，主键支持
    // vkmap 命名键（f1/enter/esc...）或单字符。ret=1 成功，0 解析失败或后端失败。
    void KeyPressHotkey(_In_ const wchar_t *combo, _Out_ long *ret);

    // ---- 窗口 DPI 缩放（P1 新增 2026-10-15）----
    // 取窗口实际缩放系数（1.00=96DPI 无缩放），两位小数字符串如 "1.25"；失败返回空串。
    // DPI 不感知的窗口按其**实际显示比例**返回（150% 系统上为 "1.50"，旧版恒为 "1.00"）：
    // 该值可把宿主视角的物理屏幕坐标换算成窗口客户区坐标（或直接用 ScreenToClient）。
    // DPI 感知窗口仍按 GetDpiForWindow 计算，数值不变。
    void GetWindowScale(_In_ LONG_PTR hwnd, _Out_ std::wstring &retstr);

    // ---- 进程终止（P1 新增 2026-10-15，进程树逻辑复用 ipc/ProcessTree.h）----
    // 按 PID 终止进程（不限树）；TerminateProcessTree 先父后子递归终止整棵进程树。
    void TerminateProcess(_In_ long pid, _Out_ long *ret);
    void TerminateProcessTree(_In_ long pid, _Out_ long *ret);

    // ---- 二维码（P1 新增 2026-10-15，OpenCV QRCodeDetector/Encoder 直桥）----
    // 识别绑定窗口客户区区域内二维码文本，未识别返回空串
    void DecodeQRCode(_In_ long x1, _In_ long y1, _In_ long x2, _In_ long y2, _Out_ std::wstring &retstr);
    // 识别图片文件中的二维码文本
    void DecodeQRCodeFromFile(_In_ const wchar_t *file_name, _Out_ std::wstring &retstr);
    // 生成二维码保存为图片（PNG/BMP 由扩展名决定），size 为边长像素（<=0 用 256）。ret=1 成功。
    void CreateQRCode(_In_ const wchar_t *text, _In_ const wchar_t *file_name, _In_ long size, _Out_ long *ret);

    //----------------------json（P3 第一批 2026-10-16）-------------------------
    // 无句柄路径直取式 JSON 族：所有函数收 JSON 文本 + 路径（"a.b[0].c"，兼容 "a.b.0"），
    // 给 OP 自身的 JSON 返回值（YOLO/CV/OCR/模块列表等）补解析侧，亦支持用户自有数据。
    // 失败约定：取值类 ret=0/空串，Set/Append/Delete 返回空串。
    // 校验文本是否合法 JSON。ret=1 合法，0 非法。
    long JsonIsValid(_In_ const wchar_t *json_text);
    // 取路径处的字符串值；类型不符/不存在返回空串且 ret=0。
    void JsonGetString(_In_ const wchar_t *json_text, _In_ const wchar_t *path, _Out_ std::wstring &retstr,
                       _Out_ long *ret);
    // 取路径处的数值；类型不符/不存在 ret=0。
    void JsonGetNumber(_In_ const wchar_t *json_text, _In_ const wchar_t *path, _Out_ double *value, _Out_ long *ret);
    // 取路径处的布尔值（value 0/1）；类型不符/不存在 ret=0。
    void JsonGetBool(_In_ const wchar_t *json_text, _In_ const wchar_t *path, _Out_ long *value, _Out_ long *ret);
    // 数组长度/对象键数；非集合或不存在返回 -1。
    long JsonGetSize(_In_ const wchar_t *json_text, _In_ const wchar_t *path);
    // 对象键名序列化为 JSON 数组文本（如 ["a","b"]）；非对象/不存在返回空串且 ret=0。
    void JsonGetKeys(_In_ const wchar_t *json_text, _In_ const wchar_t *path, _Out_ std::wstring &retstr,
                     _Out_ long *ret);
    // 写入路径（不存在自动创建中间对象/数组），返回新 JSON 串；失败返回空串。
    void JsonSetString(_In_ const wchar_t *json_text, _In_ const wchar_t *path, _In_ const wchar_t *value,
                       _Out_ std::wstring &retstr, _Out_ long *ret);
    void JsonSetNumber(_In_ const wchar_t *json_text, _In_ const wchar_t *path, _In_ double value,
                       _Out_ std::wstring &retstr, _Out_ long *ret);
    void JsonSetBool(_In_ const wchar_t *json_text, _In_ const wchar_t *path, _In_ long value,
                     _Out_ std::wstring &retstr, _Out_ long *ret);
    // 数组追加元素（raw_json 为元素文本，如 "abc"/123/{"k":1}；路径不存在先建数组）。
    void JsonAppend(_In_ const wchar_t *json_text, _In_ const wchar_t *path, _In_ const wchar_t *raw_json,
                    _Out_ std::wstring &retstr, _Out_ long *ret);
    // 删除对象键或数组元素，返回新 JSON 串；父路径不可达返回空串。
    void JsonDelete(_In_ const wchar_t *json_text, _In_ const wchar_t *path, _Out_ std::wstring &retstr,
                    _Out_ long *ret);

    //----------------------http（P3 第二项 2026-10-16）-------------------------
    // 系统 WinHTTP 直连（零第三方依赖），用途：脚本对接大模型等 OpenAI 兼容 API——
    // OpHttpPost + Json 族即构成完整闭环。请求/响应体 UTF-8；headers_json 为 JSON
    // 对象文本（如 {"Authorization":"Bearer sk-xxx"}），复用 JSON 族解析。
    // 非 2xx 不算传输失败：ret=0 但 retstr 带回响应体（错误 JSON 可见）；传输失败空串。
    // timeout_ms<=0 用 30000，钳 1000-300000。
    void HttpGet(_In_ const wchar_t *url, _In_ const wchar_t *headers_json, _In_ long timeout_ms,
                 _Out_ std::wstring &retstr, _Out_ long *ret);
    void HttpPost(_In_ const wchar_t *url, _In_ const wchar_t *body, _In_ const wchar_t *headers_json,
                  _In_ long timeout_ms, _Out_ std::wstring &retstr, _Out_ long *ret);
    // 下载到文件（原始字节流式落盘）。parent 目录须已存在。ret=1 成功。
    void HttpDownloadFile(_In_ const wchar_t *url, _In_ const wchar_t *file_name, _In_ long timeout_ms,
                          _Out_ long *ret);
};

} // namespace op

#ifdef _MSC_VER
#pragma warning(pop)
#endif
