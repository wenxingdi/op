# -*- coding: utf-8 -*-
"""OP 库 API 参考手册生成器。

从 include/libop.h（COM 接口唯一权威声明）解析全部公开方法：
分组横幅 + 上方中文注释 + SAL 标注参数 → 单文件 HTML（左侧导航 + 搜索 + 参数表）。

用法：
    python scripts/gen_api_reference.py [--out path]
默认输出 docs/api_reference.html。接口面变更后重跑即可（勿手改 HTML）。
"""
import os, re, sys, argparse, datetime, html

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
HEADER = os.path.join(ROOT, "include", "libop.h")

# 横幅 → (组 key, 组显示名)；None 表示按函数名单独分流
BANNER_MAP = {
    "基本设置/属性": ("runtime", "Runtime 运行时"),
    "algorithm": ("algorithm", "Algorithm 算法"),
    "windows api": ("window", "Window 窗口"),
    "Background": ("binding", "Binding 后台绑定"),
    "mouse & keyboard": ("input", "Input 键鼠输入"),
    "image and color": ("image", "Image 图色"),
    "opcv": ("opencv", "OpenCV 视觉"),
    "ocr": ("ocr", "OCR 文字识别"),
}

# 语义分流：windows api 横幅里的进程/工具函数归 Runtime；ocr 横幅里的 Yolo 归 YOLO；
# 内存区（无独立横幅，跟在 ocr 段后）显式归 Memory
REROUTE = {
    "RunApp": "runtime", "WinExec": "runtime", "GetCmdStr": "runtime",
    "SetClipboard": "runtime", "GetClipboard": "runtime",
    "Delay": "runtime", "Delays": "runtime",
    "SetYoloEngine": "yolo", "YoloDetect": "yolo", "YoloDetectFromFile": "yolo",
}
MEMORY_FNS = {
    "WriteData", "ReadData", "ReadInt", "WriteInt", "ReadFloat", "WriteFloat",
    "ReadDouble", "WriteDouble", "ReadString", "WriteString",
    "FindData", "FindDataEx", "GetModuleBaseAddr",
}

GROUP_META = [
    ("runtime",   "Runtime 运行时",   "版本/路径/日志/进程/剪贴板/延时等全局属性与工具"),
    ("window",    "Window 窗口",      "窗口枚举/查找/属性/状态/文本发送/排列"),
    ("binding",   "Binding 后台绑定", "窗口绑定/解绑/dx 输入通道控制"),
    ("input",     "Input 键鼠输入",   "鼠标移动/点击/滚轮/键盘按键/轨迹模拟"),
    ("image",     "Image 图色",       "截图/找色/多点找色/找图/色块/屏幕数据"),
    ("opencv",    "OpenCV 视觉",      "模板管理/图像预处理/模板匹配/特征与轮廓"),
    ("ocr",       "OCR 文字识别",     "引擎选择/字库管理/点阵提取/免字库识别/找字/找线"),
    ("yolo",      "YOLO 目标检测",    "引擎选择/区域与文件目标检测"),
    ("algorithm", "Algorithm 算法",   "A 星寻路/最近点查找"),
    ("memory",    "Memory 内存",      "进程内存读写/特征码搜索/模块基址"),
]
GROUP_ORDER = [k for k, _, _ in GROUP_META]
GROUP_DESC = {k: d for k, _, d in GROUP_META}
GROUP_NAME = {k: n for k, n, _ in GROUP_META}

BANNER_RE = re.compile(r"^\s*//\s*-{3,}\s*(.+?)\s*-*\s*$")
SAL_RE = re.compile(r"_(In|Out|Inout|In_reads_bytes)_(\([^)]*\))?\s*")

# ---------------------------------------------------------------------------
# 参数取值/语义注解层：函数名 → 参数名 → HTML 说明。
# 数据以源码为准（BindingSession.cpp 模式解析 / WindowState.cpp flag / EnumWindow filter 等），
# 新增参数或改语义时同步改这里，重跑本脚本即可。
# ---------------------------------------------------------------------------
DISPLAY_MODES = (
    "<code>normal</code> PrintWindow 后台截图（通用，推荐）<br>"
    "<code>gdi</code> / <code>gdi2</code> GDI 位图拷贝<br>"
    "<code>normal.dxgi</code> DXGI 桌面复制（Win8+）<br>"
    "<code>normal.wgc</code> WGC 捕获（Win10 1903+，本机偶发不稳）<br>"
    "<code>normal.auto</code> 按窗口特征自动逐个尝试候选后端<br>"
    "<code>dx</code> DX 注入取帧（d3d9~12 自动）；细分 <code>dx.d3d9</code> <code>dx.d3d10</code> <code>dx.d3d11</code> <code>dx.d3d12</code>。注意：Qt/GL 渲染目标 dx 无帧，capture 静默失败，此时改用 normal/gdi/dx2<br>"
    "<code>dx2</code> 实为 GDI 家族兼容模式<br>"
    "<code>opengl</code> GL 注入取帧；细分 <code>opengl.std</code> <code>opengl.nox</code> <code>opengl.es</code> <code>opengl.fi</code>(glFinish)"
)
INPUT_MODES = (
    "<code>normal</code> 驱动级模拟，移动系统光标<br>"
    "<code>windows</code> 消息派发（PostMessage 系），不移动光标<br>"
    "<code>dx</code> 注入 Hook 真后台输入（不抢光标，多开/挂机适用），三通道全开<br>"
    "<code>dx.dinput</code>/<code>di</code> 仅 DirectInput 通道（目标须已加载 dinput8.dll）<br>"
    "<code>dx.raw</code>/<code>rawinput</code> 仅 RawInput 通道<br>"
    "<code>dx.win</code>/<code>windowmsg</code> 仅窗口消息通道<br>"
    "后缀可组合：<code>dx.dinput+raw</code>（+ 连接）"
)
KEYPAD_MODES = INPUT_MODES + "<br>键盘额外支持 <code>normal.hd</code>（高精度驱动模式）"

# 找图/找色扫描方向（image 族）
DIR_IMAGE = "0=左上→右下 1=左下→右上 2=右上→左下 3=右下→左上"
# OpenCV 模板搜索方向（TemplateMatcher.h SearchDirection）
DIR_OPENCV = "0=从左到右 1=从右到左 2=从上到下 3=从下到上（条带分发顺序也受影响）"

# ---------------------------------------------------------------------------
# 通用参数兜底注解（按参数名）：函数级 PARAM_DOCS 未命中时使用。
# 只放跨函数语义一致的参数；有特殊取值的必须在函数级单独写。
# ---------------------------------------------------------------------------
GLOBAL_PARAM_DOCS = {
    "ret": "1=成功 0=失败（失败原因开 <code>set_show_error_msg(2)</code> 查 cwd/<code>__op.log</code>）",
    "et": "出参：字符串结果（COM 为 BSTR 出参 / C-API 返回 const wchar_t*）",
    "etstr": "出参：字符串结果",
    "et_str": "出参：字符串结果",
    "ettitle": "出参：窗口标题字符串",
    "etjson": "出参：JSON 格式结果串",
    "x1": "区域左上角横坐标（物理像素；绑定窗口后为客户区坐标系）",
    "y1": "区域左上角纵坐标（物理像素）",
    "x2": "区域右下角横坐标（含）（物理像素）",
    "y2": "区域右下角纵坐标（含）（物理像素）",
    "x": "横坐标（物理像素；绑定窗口后为客户区坐标）",
    "y": "纵坐标（物理像素；绑定窗口后为客户区坐标）",
    "sim": "相似度 0~1（越大越严格）；找色系 sim&lt;0 钳为 1.0，OCR/找字系回退 0.7",
    "dir": DIR_IMAGE,
    "color": "RGB hex <code>RRGGBB</code>（如 <code>FF0000</code>=红）；<code>-</code> 后为<b>偏色容差</b>（如 <code>9f2e3f-303030</code>，非背景色）；<code>@</code> 开头为背景色模式（命中像素置 0，反白字场景）",
    "hwnd": "窗口句柄（HWND，十进制数值）",
    "file_name": "文件路径；相对路径基于 <code>SetPath</code> 设定的全局目录解析",
    "src_file": "源图片路径（含扩展名）",
    "dst_file": "目标输出图片路径（含扩展名）",
    "width": "宽度（像素）",
    "height": "高度（像素）",
    "pid": "进程 PID",
    "delay": "间隔毫秒数",
    "duration": "总时长毫秒数",
    "path": "路径点串 <code>\"x,y|x,y|...\"</code>",
    "vk_code": "虚拟键码 VK（如 13=回车 112=F1）",
    "index": "序号（从 0 开始）",
    "template_name": "模板名（须先经 CvLoadTemplate 注册）",
    "conf": "置信度阈值 0~1，低于该值的检测结果被丢弃",
    "iou": "IOU 重叠阈值 0~1，用于抑制重复检测框",
    "address": "内存地址表达式：16 进制地址或 <code>\"模块名+偏移\"</code>（如 <code>\"game.exe+1234\"</code>），支持 + - * / 运算",
    "size": "数据长度（字节）",
    "count": "数量（上限/期望值，见各函数说明）",
    "threshold": "阈值（匹配类函数 0~1；像素阈值类如 CvThreshold 为 0~255）",
    "value": "要写入的值",
    "process_id": "进程 PID",
    "files": "图片文件名串，多图用 | 分隔（相对路径基于 SetPath）",
    "delta_color": "透明色 RGB hex <code>RRGGBB</code>（与 color 同规则；<b>注意与大漠相反</b>——大漠 FindPic delta 为 BGR，迁移需红蓝互换）；不需要透明匹配传 000000 配 sim 普通匹配",
    "word": "单字模串（点阵字库格式）",
    "words": "多字模串（每个字一个字模）",
    "pic_name": "缓存中的图片名（LoadPic/LoadMemPic 注册的名）",
    "strs": "待查找字符串，多个用 | 分隔",
    "idx": "字库槽位 0~9",
    "min_word_h": "最小字高（过滤过小噪点）",
    "min_word_w": "最小字宽",
    "padding": "矩形外扩像素",
}

PARAM_DOCS = {
    # ---------- Binding 后台绑定 ----------
    "BindWindow": {
        "display": DISPLAY_MODES,
        "mouse": INPUT_MODES,
        "keypad": INPUT_MODES + "<br>keypad 无 <code>normal.hd</code> 之外的额外模式，其余同 mouse",
        "mode": "兼容大漠签名保留，当前实现<b>不消费此参数</b>（含 dx 通道期望掩码，见 SetInputHook）。传 0 即可",
        "hwnd": "0 = 绑定整个桌面（截全屏场景）",
    },
    "BindWindowEx": {
        "display_hwnd": "截图目标窗口句柄，0 = 桌面",
        "input_hwnd": "输入目标窗口句柄，0 = 同 display_hwnd。截图与输入可以是两个不同窗口（display 与 mouse/keypad 是两条独立通道）",
        "display": DISPLAY_MODES,
        "mouse": INPUT_MODES,
        "keypad": KEYPAD_MODES,
        "mode": "同 BindWindow，当前不消费，传 0",
    },
    "UnBindWindow": {"ret": "1=成功；未绑定时也返回 1"},
    "SetInputHook": {
        "entryid": "当前忽略（设计预留：期望通道掩码，见 BindingSession 注释）"
    },
    "LockInput": {
        "lock": "<code>1</code> 锁定目标窗口输入 / <code>0</code> 解锁。解绑时自动解锁，防脚本异常退出后目标残留锁定"
    },
    # ---------- Window 窗口 ----------
    "EnumWindow": {
        "parent": "父窗口句柄；filter 含 4 时表示只枚举其第一层子窗",
        "title": "<b>模糊匹配</b>（wcsstr 子串包含，大小写敏感）；空 = 不限。注意单字符标题(&lt;2 字符)不参与匹配",
        "class_name": "模糊匹配（子串包含）；空 = 不限。title 与 class_name 同时传时为 AND 关系",
        "filter": "1=匹配标题 2=匹配类名 4=只匹配父窗口第一层子窗 8=只匹配顶级窗口(Owner=0)；按位或组合；<b>+32</b>=结果按 Z 序排列（枚举顺序本身即 Z 序，仅兼容语义）",
    },
    "FindWindow": {
        "class_name": "精确匹配（直调 FindWindowW）。空串或 NULL = 不限",
        "title": "精确匹配。空串或 NULL = 不限。需唯一窗口名时用本函数；子串查找请用 EnumWindow 系",
    },
    "FindWindowEx": {
        "parent": "父窗口句柄，0 = 顶层",
        "class_name": "精确匹配，空/NULL 不限",
        "title": "精确匹配，空/NULL 不限",
    },
    "EnumWindowByProcess": {
        "process_name": "进程名（如 <code>game.exe</code>），先按进程过滤再比 title/class",
        "title": "模糊匹配（子串包含）",
        "class_name": "模糊匹配（子串包含）",
        "filter": "同 EnumWindow（1/2/4/8 按位或，+32 排序）",
    },
    "FindWindowByProcess": {
        "process_name": "进程名过滤",
        "class_name": "模糊匹配。⚠️ 与 title 是 <b>OR</b> 关系（任一匹配即中），与 EnumWindow 的 AND 语义不同",
        "title": "模糊匹配。⚠️ 同上，OR 关系",
    },
    "FindWindowByProcessId": {
        "pid": "进程 PID",
        "class_name": "模糊匹配。⚠️ 与 title 是 OR 关系",
        "title": "模糊匹配。⚠️ 同上，OR 关系",
    },
    "GetWindowState": {
        "flag": "0=窗口是否存在 1=是否前台(激活) 2=是否可见 3=是否最小化 4=是否最大化 5=是否置顶 6=是否无响应 7=是否可用(禁用返回 0)"
    },
    "SetWindowState": {
        "flag": "0=关闭(WM_CLOSE) 1=激活 2=最小化(不激活) 3=最小化并释放内存(激活) 4=最大化 5=恢复(不激活) 6=隐藏 7=显示并激活 8=置顶 9=取消置顶 10=禁用 11=启用 12=恢复并激活 13=强制结束窗口所在进程"
    },
    "GetSpecialWindow": {
        "flag": "0=桌面窗口 1=任务栏(Shell_TrayWnd) 2=... 其余值返回 0"
    },
    "LayoutWindows": {
        "hwnds": "窗口句柄串 <code>\"hwnd|hwnd|...\"</code>，按传入顺序排列",
        "layout_type": "0=宫格 1=对角线 2=层叠Cascade（第 i 窗偏移 i×gap）",
        "columns": "宫格列数（layout_type=0 时生效）",
        "start_x": "排列起点屏幕横坐标",
        "start_y": "排列起点屏幕纵坐标",
        "gap_x": "窗口间水平间距（像素）",
        "gap_y": "窗口间垂直间距（像素）",
        "size_mode": "0=保持各自原大小 1=统一客户区大小（须 ≥50×50，否则 ret=0）",
        "window_width": "size_mode=1 时的统一客户区宽",
        "window_height": "size_mode=1 时的统一客户区高",
        "anchor_mode": "0=按窗口外框对齐 1=按客户区对齐（补偿边框/标题栏厚度）",
        "ret": "0=失败（任一窗口失败即整体返回 0，前面窗口可能已移动=半应用；失败原因落 __op.log）",
    },
    "SetWindowTransparent": {
        "trans": "0~255 透明度（0=全透明，255=不透明）。实现为读旧扩展样式 |WS_EX_LAYERED，不会清掉 TOPMOST 等已有样式"
    },
    # ---------- Image 图色 ----------
    "Capture": {
        "file_name": "保存路径，<b>必须带扩展名</b>（bmp/png/jpg/jpeg/gif/tif，大小写不敏感）；无扩展名或 .txt/.emf 等格式静默失败（ret=0，日志见 __op.log）。窗口最小化时同样静默失败"
    },
    "FindPic": {
        "delta_color": "透明色 RGB hex <code>RRGGBB</code>（与 color 同规则；<b>注意与大漠相反</b>——大漠 FindPic delta 为 BGR，迁移需红蓝互换）；不需要透明匹配传 <code>000000</code> 使用普通匹配",
        "sim": "相似度 0~1。内部阈值=<code>0.5 + sim/2</code>；越界不钳制",
        "dir": "0=左上→右下 1=左下→右上 2=右上→左下 3=右下→左上",
    },
    "FindColor": {
        "color": "RGB hex <code>RRGGBB</code>（如 <code>FF0000</code>=红），可带偏移标记",
        "sim": "相似度 0~1；<b>sim&lt;0 时钳为 1.0（变精确匹配）</b>。多点串中 sim 同理",
        "dir": "0=左上→右下 1=左下→右上 2=右上→左下 3=右下→左上",
    },
    "FindMultiColor": {
        "first_color": "基准点颜色 RGB hex <code>RRGGBB</code>",
        "offset_color": "偏移点串：<code>\"dx|dy|RRGGBB,dx|dy|RRGGBB,...\"</code>；畸形段自动跳过（09-17 加固）",
    },
    "FindColorBlock": {
        "count": "期望找到的色块数量",
        "height": "色块最小高度（⚠️ 顺序是 count, <b>height, width</b>——高在宽前，与大漠一致）",
        "width": "色块最小宽度",
    },
    "GetColor": {"ret": "RGB hex <code>RRGGBB</code> 串（如 <code>FF0000</code>=红）"},
    "CmpColor": {"sim": "同 FindColor：sim&lt;0 钳为 1.0"},
    # ---------- Input 键鼠 ----------
    "SetMouseDelay": {
        "type": "作用的输入通道：<code>normal</code> / <code>windows</code> / <code>dx</code> 分别设置",
        "delay": "按下/弹起间隔<b>基准毫秒值</b>，实际值 = 基准 ±40% 随机抖动（下限 1ms，防按下弹起被合并为单次）。0=不等待",
    },
    "SetKeypadDelay": {
        "type": "同 SetMouseDelay 的通道语义",
        "delay": "同 SetMouseDelay：基准值 + ±40% 抖动",
    },
    "MoveTo": {
        "options": "扩展选项（大漠兼容位），当前实现按 0 处理"
    },
    "MoveR": {"x": "相对当前位置的水平偏移（可为负）", "y": "相对当前位置的垂直偏移（可为负）"},
    "MoveToEx": {
        "w": "随机落点范围宽：实际落点在 (x,y)~(x+w,y+h) 矩形内随机",
        "h": "随机落点范围高",
        "et": "返回实际落点 <code>\"实际x,实际y\"</code>（未移动返回空串）",
    },
    "MoveToSmooth": {"duration": "平滑移动总时长毫秒（拟人化轨迹，配合 SetMouseTrajectory 参数）"},
    "MoveToExSmooth": {
        "w": "同 MoveToEx 的随机范围宽",
        "h": "同 MoveToEx 的随机范围高",
        "duration": "平滑移动总时长毫秒",
        "et": "返回实际落点 <code>\"实际x,实际y\"</code>",
    },
    "MovePath": {"path": "路径点串 <code>\"x,y|x,y|...\"</code>（只移动不按键）", "duration": "沿路径全程总时长毫秒"},
    "DragPath": {"path": "路径点串（按住左键拖动）", "duration": "全程总时长毫秒"},
    "SetMouseTrajectory": {
        "mode": "0=关闭拟人轨迹 1=开启",
        "min_duration": "轨迹时长下限毫秒",
        "max_duration": "轨迹时长上限毫秒",
        "jitter": "轨迹抖动幅度（像素）",
        "start_delay": "起点停顿毫秒",
        "end_delay": "终点停顿毫秒",
    },
    "Wheel": {"delta": "滚轮增量：<b>正=向上 负=向下</b>（WheelUp=+120 / WheelDown=-120，|120|≈一格）"},
    "HWheel": {"delta": "横向滚轮：<b>正=向右 负=向左</b>"},
    "GetKeyState": {"ret": "1=按下 0=未按"},
    "KeyDownChar": {"vk_code": "键名/字符描述串（如 <code>\"enter\"</code> <code>\"f1\"</code> <code>\"ctrl+a\"</code>），经 vkmap 解析为键组合"},
    "KeyUpChar": {"vk_code": "同 KeyDownChar 的键名描述串"},
    "KeyPressChar": {"vk_code": "同 KeyDownChar 的键名描述串"},
    "KeyPressStr": {
        "key_str": "逐字符输入的文本串（每字符走 InputChar，支持中文）",
        "delay": "字符间隔毫秒（&lt;1 时按 1 处理）",
    },
    "WaitKey": {"time_out": "等待毫秒上限；<b>&lt;0 表示无限等待</b>"},
    # ---------- Runtime 运行时 ----------
    "Sleep": {"millseconds": "裸 ::Sleep，期间<b>不处理窗口消息（界面冻结）</b>。脚本侧建议改用 Delay/Delays"},
    "Delay": {"mis": "带消息泵的阻塞延时：线程挂起不占 CPU，期间窗口消息正常派发"},
    "Delays": {"mis_min": "区间下限毫秒", "mis_max": "区间上限毫秒（max&lt;min 自动交换）；区间内均匀随机取一值后走 Delay"},
    "RunApp": {
        "cmdline": "可执行文件完整路径；<b>支持 .lnk 快捷方式</b>（经 ShellExecuteEx 解析，lnk 里的工作目录/参数一并生效）。cwd 敏感的程序建议 mode=1 或走 lnk",
        "mode": "0=以<b>调用方当前目录</b>为工作目录启动（cwd 敏感程序会失败）；1=以 exe 所在目录为工作目录（推荐用于游戏/大型程序）。.lnk 时本参数不生效",
        "pid": "新进程 PID；.lnk 走 Shell 执行时若拿不到句柄则为 0（ret 仍为 1）",
    },
    "GetCmdStr": {
        "cmd": "命令行（可含参数）：启动子进程并捕获其 stdout",
        "millseconds": "等待输出的毫秒上限，超时杀整棵进程树",
    },
    # ---------- OCR ----------
    "SetDict": {"idx": "字库槽位 0~9；0 为全局默认，1~9 为私有槽（可被 UseDict 切换）", "file_name": "字库文件路径（点阵字库文本）"},
    "UseDict": {"idx": "切换当前生效字库槽位（0~9）"},
    "FindStr": {
        "color_format": "颜色格式串（同 OCR），RGB hex <code>\"RRGGBB[-DDRGGBB]\"</code>，如 <code>\"9f2e3f-000000\"</code>",
        "sim": "⚠️ OCR 系 sim&lt;0 时<b>回退 0.7</b>（与找色系钳 1.0 不同）",
        "retx": "出参：首个命中横坐标（-1=未找到）",
        "rety": "出参：首个命中纵坐标",
    },
    # ---------- Memory ----------
    # ---------- Runtime 运行时（其余） ----------
    "SetPath": {"path": "设置全局根目录：找图/字库等相对路径均基于此目录解析"},
    "GetLastError": {"ret": "最近一次失败的内部错误码（0=无错误）；定位问题需配合 set_show_error_msg(2) 日志"},
    "SetShowErrorMsg": {
        "show_type": "0=静默（默认）1=弹框（阻塞调用线程）2=写日志文件（cwd/<code>__op.log</code>，排查推荐）3=输出到调试器"
    },
    "InjectDll": {
        "process_name": "目标进程名（按名字找进程）",
        "dll_name": "要注入的 DLL 完整路径（位数须与目标进程一致，需要足够权限）",
    },
    "EnablePicCache": {
        "enable": "1=开启找图模板全局缓存；FindPic 自动缓存已加载图片，源文件变更后须 FreePic / 重新 LoadPic 刷新"
    },
    "CapturePre": {"file_name": "预加载图片进缓存（提前读盘，消首次 FindPic 卡顿）"},
    "SetScreenDataMode": {
        "mode": "0=顶向下行序（默认，DIB 负高度）1=自底向上行序（BMP 标准，数据首行=图像底行）"
    },
    "WinExec": {
        "cmdline": "命令行（可含参数；兼容入口，新代码建议用 RunApp）",
        "cmdshow": "窗口显示命令：0=隐藏 1=正常 3=最大化 6=最小化",
    },
    "SetClipboard": {"str": "写入系统剪贴板的文本"},
    "GetClipboard": {"et": "读出系统剪贴板文本"},
    # ---------- Algorithm ----------
    "AStarFindPath": {
        "mapWidth": "地图宽（格子数）；尺寸超上限返回空路径",
        "mapHeight": "地图高（格子数）",
        "disable_points": "障碍点串 <code>\"x,y|x,y|...\"</code>；遇非法项，该项及其后全部忽略",
        "beginX": "起点格横坐标", "beginY": "起点格纵坐标",
        "endX": "终点格横坐标", "endY": "终点格纵坐标",
        "et": "返回路径 <code>\"x,y|x,y|...\"</code>（起点→终点顺序）；无解返回空串",
    },
    "FindNearestPos": {
        "all_pos": "点位串：type=1 时 <code>\"x,y|x,y,...\"</code>；否则 <code>\"名称,x,y|名称,x,y,...\"</code>（逗号可用空格替代）",
        "type": "1=纯坐标点（返回 <code>\"x,y\"</code>）；其他=带名称点（返回 <code>\"名称,x,y\"</code>）",
        "et": "距离参照点 (x,y) 最近的点",
    },
    # ---------- Window 窗口（其余） ----------
    "EnumProcess": {"name": "进程名过滤（空=全部）；返回 PID 串 <code>\"pid|pid|...\"</code>"},
    "ClientToScreen": {"x": "客户区坐标→屏幕坐标（inout）", "y": "同左", "bret": "1=成功"},
    "ScreenToClient": {"x": "屏幕坐标→客户区坐标（inout）", "y": "同左"},
    "GetWindow": {
        "flag": "0=父窗口(GetParent) 1=第一个子窗(GW_CHILD) 2=同级最前(GW_HWNDFIRST) 3=同级最后(GW_HWNDLAST) "
                "4=下一个同级(GW_HWNDNEXT) 5=上一个同级(GW_HWNDPREV) 6=属主窗口(GW_OWNER) 7=顶层窗口（沿 Owner 链上溯）"
    },
    "GetProcessInfo": {"pid": "进程 PID", "et": "返回进程信息串"},
    "GetPointWindow": {"x": "屏幕横坐标", "y": "屏幕纵坐标"},
    "GetClientSize": {"width": "出参：客户区宽", "height": "出参：客户区高"},
    "GetClientRect": {
        "x1": "窗口客户区左上角屏幕横坐标（物理像素）", "y1": "左上角纵坐标",
        "x2": "右下角横坐标", "y2": "右下角纵坐标",
        "ret": "坐标为<b>物理像素</b>（OP 构造时提升进程 DPI 感知，见 OP_DPI 语义文档）。窗口最小化时返回 -32000 哨兵",
    },
    "GetWindowRect": {
        "x1": "窗口外框左上角屏幕横坐标（含边框/标题栏）", "y1": "左上角纵坐标",
        "x2": "右下角横坐标", "y2": "右下角纵坐标",
    },
    "MoveWindow": {"x": "窗口外框左上角目标屏幕横坐标", "y": "目标纵坐标"},
    "SetClientSize": {"width": "客户区宽度", "hight": "客户区高度（⚠️ 参数名拼写为 hight）"},
    "SetWindowSize": {"width": "窗口外框宽度", "height": "窗口外框高度"},
    "SetWindowText": {"title": "设置窗口标题"},
    "SendString": {"str": "向窗口逐键发送文本（消息派发；部分游戏收不到时可换 SendStringIme）"},
    "SendStringIme": {"str": "走 IME 消息发送文本（中文/游戏内输入框推荐）"},
    "SendPaste": {"hwnd": "把当前剪贴板内容粘贴进目标窗口（先 SetClipboard 再调用）"},
    "GetWindowProcessId": {"ret": "窗口所属进程 PID"},
    "GetWindowProcessPath": {"et": "窗口所属进程 exe 完整路径"},
    "GetWindowClass": {"et": "窗口类名"},
    "GetForegroundWindow": {"ret": "当前前台窗口句柄"},
    "GetForegroundFocus": {"ret": "当前拥有键盘焦点的窗口句柄"},
    "GetMousePointWindow": {"ret": "鼠标指针所在窗口句柄"},
    # ---------- Binding 后台绑定（其余） ----------
    "SetDxAttr": {
        "attr": "0 = value 直接作为完整通道掩码；否则 attr 自身为位掩码（1=DirectInput 2=RawInput 4=窗口消息），value=1 开 / 0 关",
        "value": "含义随 attr：attr=0 时为完整掩码（0~7），否则为开关 0/1",
    },
    "GetDxAttr": {"ret": "当前 dx 输入通道掩码（1=dinput 2=rawinput 4=winmsg，相加组合；7=全开）"},
    "GetBindWindow": {"ret": "当前绑定信息串 <code>\"display_hwnd,input_hwnd,display,mouse,keypad,mode\"</code>；未绑定返回空"},
    "IsBind": {"ret": "1=已绑定 0=未绑定"},
    # ---------- Input 键鼠（其余） ----------
    "GetCursorPos": {
        "x": "出参：系统光标屏幕横坐标（dx 模式不移动系统光标，读数不反映后台移动）",
        "y": "出参：系统光标屏幕纵坐标",
    },
    "GetCursorShape": {"et": "当前光标形状特征串"},
    # ---------- Image 图色（其余） ----------
    "FindColorEx": {"etstr": "全部命中坐标串，每条 <code>\"x,y\"</code>，| 连接"},
    "FindMultiColorEx": {
        "first_color": "基准点颜色 RGB hex <code>RRGGBB</code>",
        "offset_color": "偏移点串：<code>\"dx|dy|RRGGBB,dx|dy|RRGGBB,...\"</code>；畸形段自动跳过",
        "etstr": "全部命中坐标串，每条 <code>\"x,y\"</code>，| 连接",
    },
    "FindPicEx": {
        "etstr": "全部命中串：每条 <code>\"图片序号,x,y\"</code>（序号为 files 列表从 0 起的下标），| 连接",
    },
    "FindPicExS": {
        "etstr": "全部命中串：每条 <code>\"图片文件名,x,y\"</code>（区别于 FindPicEx 的序号），| 连接",
    },
    "FindColorBlockEx": {"etstr": "全部色块中心坐标串，每条 <code>\"x,y\"</code>，| 连接"},
    "FindColorBlockExS": {
        "mode": "0=旧版行为 1=并查集聚类去重后返回（推荐，去重相邻同色块）",
        "etstr": "全部色块中心坐标串，每条 <code>\"x,y\"</code>，| 连接",
    },
    "GetColorNum": {"ret": "区域内匹配颜色的像素数量"},
    "LoadPic": {"file_name": "显式加载图片进全局缓存（会按当前文件内容刷新同名缓存）"},
    "FreePic": {"file_name": "从全局缓存释放指定图片"},
    "LoadMemPic": {
        "file_name": "给内存图片命名（进缓存后按此名引用）",
        "data": "图片文件字节数据（BMP/PNG 等内存 buffer）",
        "size": "data 字节数",
    },
    "GetPicSize": {"width": "出参：图片宽（像素）", "height": "出参：图片高（像素）"},
    "MatchPicName": {"pic_name": "宽松匹配缓存中的图片名（可省扩展名/大小写差异），返回匹配到的完整名"},
    "SetDisplayInput": {"mode": "内部显示输入模式（开发调试保留），脚本侧请用 BindWindow"},
    "GetScreenData": {
        "x1": "区域左上角（物理像素）", "y1": "左上角纵", "x2": "右下角横（含）", "y2": "右下角纵（含）",
        "data": "出参：32 位原始像素内存指针（4 字节/像素；BGRA 字节序=B 在低地址，同 Win32 DIB，非 RRGGBB 字符串），生命周期到下一次截图操作前",
    },
    "GetScreenDataBmp": {
        "data": "出参：标准 BMP 格式内存数据指针（含文件头）",
        "size": "出参：BMP 数据字节数",
    },
    "GetScreenFrameInfo": {
        "frame_id": "出参：最近一帧的序号",
        "time": "出参：最近一帧的时间戳（dx 显示模式调试用）",
    },
    # ---------- OpenCV ----------
    "CvLoadTemplate": {"name": "模板名（后续匹配函数按名引用）", "file_path": "模板图片路径"},
    "CvLoadMaskedTemplate": {
        "name": "模板名",
        "template_path": "模板图片路径",
        "mask_path": "掩码图路径：白色=参与匹配，黑色=忽略",
    },
    "CvRemoveTemplate": {"name": "移除并释放指定模板"},
    "CvHasTemplate": {"name": "查询模板是否已加载", "ret": "1=已加载 0=未加载"},
    "CvGetTemplateCount": {"ret": "已加载模板数量"},
    "CvGetAllTemplateNames": {"etstr": "全部已加载模板名，| 连接"},
    "CvLoadTemplateList": {"template_list": "批量加载串 <code>\"name,path|name,path|...\"</code>；任一项缺逗号或空串则整体失败"},
    "CvGetOpenCvVersion": {"etstr": "OpenCV 版本串"},
    "CvToGray": {"dst_file": "灰度图输出路径"},
    "CvToBinary": {"dst_file": "二值图输出路径（Otsu 自动阈值）"},
    "CvToEdge": {"dst_file": "边缘图输出路径"},
    "CvToOutline": {"dst_file": "轮廓图输出路径"},
    "CvDenoise": {"dst_file": "去噪图输出路径（中值滤波）"},
    "CvEqualize": {"dst_file": "直方图均衡输出路径"},
    "CvCLAHE": {
        "clip_limit": "对比度受限阈值（典型 2.0~4.0）",
        "tile_grid_size": "分块网格边长（典型 8）",
        "dst_file": "输出路径",
    },
    "CvBlur": {
        "mode": "模糊模式串：<code>gaussian</code>(默认) <code>median</code> <code>bilateral</code> <code>box</code>/<code>mean</code>",
        "kernel_size": "核边长（像素）",
    },
    "CvSharpen": {"strength": "锐化强度（0~100）"},
    "CvCropValid": {"dst_file": "裁剪输出路径（自动裁掉纯色边缘）"},
    "CvThreshold": {
        "threshold": "阈值 0~255（Otsu/Adaptive 模式忽略）",
        "max_value": "超过阈值的输出值（通常 255）",
        "mode": "模式串：<code>binary</code>(默认) <code>binary_inv</code>/<code>inv</code> <code>otsu</code> <code>otsu_inv</code> <code>adaptive</code> <code>adaptive_inv</code>",
    },
    "CvInRange": {
        "color_space": "颜色空间串：<code>bgr</code>(默认) <code>hsv</code> <code>gray</code>/<code>grey</code>",
        "lower": "各通道下限串（如 <code>\"0,0,0\"</code>，通道顺序随 color_space：B/G/R 或 H/S/V）",
        "upper": "各通道上限串（如 <code>\"255,255,255\"</code>）",
    },
    "CvMorphology": {
        "mode": "模式串：<code>erode</code> 腐蚀 <code>dilate</code> 膨胀 <code>open</code>(默认) 开运算 <code>close</code> 闭运算",
        "kernel_size": "结构元边长（像素）",
        "iterations": "迭代次数",
    },
    "CvThin": {
        "mode": "细化算法串：<code>zhang_suen</code>/<code>zhangsuen</code>(默认) <code>guo_hall</code>/<code>guohall</code> <code>morph</code> 形态学细化"
    },
    "CvCrop": {"x": "裁剪区左上角横坐标", "y": "左上角纵坐标", "width": "裁剪宽", "height": "裁剪高"},
    "CvResize": {"width": "目标宽", "height": "目标高"},
    "CvConnectedComponents": {
        "min_area": "最小连通域面积（小于该值的噪点被过滤）",
        "etjson": "连通域 JSON 数组：每个元素含 x/y/w/h/area",
    },
    "CvFindContours": {
        "min_area": "最小轮廓面积（过滤噪点）",
        "etjson": "轮廓 JSON 数组：每个元素含外接矩形与轮廓点",
    },
    "CvPreprocessPipeline": {
        "pipeline": "流水线串，<code>|</code> 分隔步骤，步骤可带参数 <code>\"步骤名:参数1,参数2\"</code>。步骤名：gray/binary/edge/outline/denoise/equalize/clahe/blur/sharpen/cropvalid/crop/resize/threshold/inrange/morph/thin"
    },
    "CvMatchTemplate": {
        "x": "搜索区域左上角横坐标", "y": "左上角纵坐标",
        "width": "搜索区域宽", "height": "搜索区域高",
        "threshold": "匹配阈值 0~1（score 低于该值丢弃）",
        "dir": DIR_OPENCV,
        "strip_mode": "0=整区搜索 1=横向条带并行 2=纵向条带并行（命中即停偏延迟，不保证全局最优）",
        "method": "匹配算法：0=TM_SQDIFF 1=TM_SQDIFF_NORMED 2=TM_CCORR 3=TM_CCORR_NORMED 4=TM_CCOEFF 5=TM_CCOEFF_NORMED（默认）",
        "color_mode": "0=彩色匹配 1=灰度匹配",
        "etjson": "命中结果 JSON 数组：[{x,y,width,height,score},...]",
    },
    "CvMatchTemplateScale": {
        "scales": "缩放比串（逗号分隔），如 <code>\"0.8,0.9,1.0,1.1\"</code>：模板按各缩放比逐次匹配",
        "threshold": "匹配阈值 0~1",
        "method": "同 CvMatchTemplate",
        "color_mode": "0=彩色 1=灰度",
        "etjson": "命中结果 JSON 数组（含实际使用的 scale）",
    },
    "CvMatchAnyTemplate": {
        "template_names": "多模板名串（| 分隔），任一命中即返回首个",
        "dir": DIR_OPENCV, "strip_mode": "同 CvMatchTemplate",
        "method": "同 CvMatchTemplate", "color_mode": "0=彩色 1=灰度",
        "etjson": "命中结果 JSON 数组",
    },
    "CvMatchAllTemplates": {
        "template_names": "多模板名串（| 分隔），全部模板各搜一遍",
        "dir": DIR_OPENCV, "strip_mode": "同 CvMatchTemplate",
        "method": "同 CvMatchTemplate", "color_mode": "0=彩色 1=灰度",
        "etjson": "命中结果 JSON 数组（含模板名）",
    },
    "CvFeatureMatchTemplate": {"threshold": "特征匹配阈值 0~1", "etjson": "命中结果 JSON 数组"},
    "CvEdgeMatchTemplate": {"threshold": "边缘匹配阈值 0~1", "etjson": "命中结果 JSON 数组"},
    "CvShapeMatchTemplate": {"threshold": "形状匹配阈值 0~1", "etjson": "命中结果 JSON 数组"},
    # ---------- OCR ----------
    "SetOcrEngine": {
        "path_of_engine": "OCR 引擎目录（按引擎要求的目录结构）",
        "dll_name": "引擎 DLL 名",
        "argv": "传给引擎初始化函数的参数字符串",
    },
    "GetDict": {"idx": "字库槽位", "font_index": "字库内第几个字（从 0）", "etstr": "该字的字库条目串"},
    "SetMemDict": {"data": "字库文本字节数据", "size": "字节数"},
    "AddDict": {"idx": "目标槽位", "dict_info": "字库条目串（字库文本格式）"},
    "SaveDict": {"idx": "槽位", "file_name": "保存路径"},
    "ClearDict": {"idx": "清空槽位", "ret": "1=成功"},
    "GetDictCount": {"idx": "槽位", "ret": "该槽位字数"},
    "GetNowDict": {"ret": "当前生效字库槽位"},
    "SetBinaryPreprocess": {
        "mode": "0=关（逐像素精确）1=保守去噪（默认，删完全孤立的 1 像素噪点）2=并查集聚类（按 min_component_area 过滤小组件）",
        "isolated_threshold": "孤立判定邻域阈值 0~8（邻居数 ≤ 该值的点视为噪点删除）",
        "min_component_area": "最小连通组件面积（&le;0 按 2），范围 1~4096",
        "bridge_gap": "笔画桥接开关：1=开启 0=关闭",
    },
    "GetBinaryPreprocess": {
        "mode": "出参：当前模式（含义同 SetBinaryPreprocess）",
        "isolated_threshold": "出参：当前孤立阈值",
        "min_component_area": "出参：当前最小组件面积",
        "bridge_gap": "出参：当前桥接开关",
    },
    "FetchWord": {
        "word": "单字模串（点阵字库格式）",
        "etstr": "返回该字在区域中的坐标 <code>\"x,y\"</code>；未找到返回空串",
    },
    "FetchWords": {
        "words": "多字模串（每个字一个字模）",
        "min_word_h": "最小字高（过滤过小噪点）",
        "etstr": "逐字结果串（坐标+匹配到的字）",
    },
    "FetchWordsByRects": {"rects": "限定区域串：为 words 中每个字指定独立搜索范围"},
    "ExtractWordRects": {"min_word_h": "最小字高", "etstr": "识别到的文字块矩形串"},
    "ExtractWordRectsEx": {"min_word_w": "最小字宽", "padding": "矩形外扩像素"},
    "GetBinaryPreview": {
        "etstr": "返回二值化预览文本：首行 <code>\"宽,高\"</code>，其后每行以 <code>#</code>=前景点、<code>.</code>=背景绘制",
    },
    "GetWordPreview": {"dict_info": "字库条目串", "etstr": "字模的文本预览（# 点阵）"},
    "CheckWordDict": {"dict_info": "字库文本", "etstr": "校验结果（规范化后的字库文本）"},
    "NormalizeWordDict": {"dict_info": "字库文本", "etstr": "规范化后的字库文本"},
    "RenameWordDict": {"dict_info": "字库文本", "words": "重命名字表", "etstr": "重命名后的字库文本"},
    "GetWordsNoDict": {
        "etstr": "免字库识别结果：<code>\"x,y-文字/x,y-文字/...\"</code>（可配 GetWordResultCount/Pos/Str 拆解）",
    },
    "GetWordResultCount": {"result": "GetWordsNoDict 等的结果串", "ret": "结果条目数（按 / 计数）"},
    "GetWordResultPos": {"result": "结果串", "index": "第几条（从 0）", "x": "出参：横坐标", "y": "出参：纵坐标"},
    "GetWordResultStr": {"result": "结果串", "index": "第几条（从 0）", "et_str": "出参：该条的文字"},
    "Ocr": {"et_str": "识别出的纯文本"},
    "OcrEx": {"et_str": "逐块结果串：<code>\"x,y,文字|x,y,文字|...\"</code>"},
    "AutoOcr": {"et_str": "自动阈值版 OCR 的纯文本（内部自动调整二值化）"},
    "AutoOcrLine": {"et_str": "逐行结果串（自动阈值 + 分行）"},
    "AutoOcrEx": {"et_str": "自动阈值版逐块结果串（同 OcrEx 格式）"},
    "OcrAuto": {
        "color": "本接口<b>不传颜色</b>（自动取区域主色做二值化）",
        "et_str": "免配色 OCR 纯文本",
    },
    "OcrFromFile": {
        "file_name": "图片文件路径",
        "color_format": "颜色格式串（RGB hex）：<code>\"RRGGBB\"</code>、<code>\"RRGGBB-偏色\"</code>（如 <code>9f2e3f-303030</code>）或 <code>\"@RRGGBB\"</code> 背景色模式",
        "etstr": "识别出的纯文本",
    },
    "AutoOcrFromFile": {"file_name": "图片路径", "color_format": "颜色格式串（RGB hex <code>RRGGBB</code>）", "etstr": "自动阈值版识别文本"},
    "OcrAutoFromFile": {"file_name": "图片路径", "etstr": "免配色识别文本"},
    "FindStrEx": {
        "etstr": "全部命中串：每条 <code>\"strs序号,x,y\"</code>（序号为 strs 以 | 拆分后的下标，从 0），| 连接",
    },
    "FindLineExS": {
        "min_points": "一条线最少点数（低于该值的结果被丢弃，过滤噪线）",
        "etstr": "返回 <code>\"角度,距离\"</code> 描述的直线串",
        "ret": "拟合出的线上点数（0=未捕获/不可信）",
    },
    "FindLineEx": {"etstr": "返回 <code>\"角度,距离\"</code>；ret=线上点数（0=不可信）"},
    # ---------- YOLO ----------
    "SetYoloEngine": {
        "path_of_engine": "统一入口：<b>.onnx 模型文件路径</b>（dll_name 留空，自动切进程内 ONNX 引擎）；旧写法 = 引擎名（<code>onnx</code> 开头=进程内 ONNX）或 http(s) URL / 别名 <code>yolo</code> <code>yolo_http</code>",
        "dll_name": "模型路径（ONNX 引擎；空=内置资源段模型）",
        "argv": "空格分隔参数；ONNX 类别名<b>自动读模型内嵌 metadata</b>（ultralytics 导出自带，免 --labels），无 metadata 时用 <code>--labels=a,b,c</code>（UTF-8）或 <code>--labels=@classes.txt</code> 兜底（显式优先）；另支持 <code>--conf=0.25</code> <code>--iou=0.45</code>；HTTP 支持 <code>--timeout=毫秒</code>",
    },
    "YoloDetect": {
        "etjson": "检测结果 JSON 数组：每个元素含类别/置信度/包围盒 {class,score,x,y,w,h}",
    },
    "YoloDetectFromFile": {"file_name": "待检测图片路径", "etjson": "同 YoloDetect"},
    # ---------- Memory ----------
    "WriteData": {"data": "要写入的字节数据", "size": "字节数"},
    "ReadData": {"size": "读取字节数", "etstr": "读出的数据（16 进制串）"},
    "ReadInt": {"type": "整数类型：0=int32 1=int16 2=int8 3=int64 4=uint32 5=uint16 6=uint8"},
    "WriteInt": {"type": "同 ReadInt", "value": "要写入的整数值"},
    "ReadString": {"type": "字符宽度：0=ANSI(1 字节) 1=Unicode(2 字节)", "len": "字符数（超限自动截断）"},
    "WriteString": {"type": "同 ReadString", "value": "要写入的文本"},
    "FindData": {
        "hwnd": "0 = 优先作用于当前绑定窗口的进程（非系统全局）",
        "addr_range": "搜索范围 <code>\"起始-结束\"</code>（16 进制地址，如 <code>\"00010000-7FFFFFFF\"</code>）",
        "string": "特征码串：16 进制字节，<code>??</code> 通配任意字节",
    },
    "FindDataEx": {
        "addr_range": "同 FindData", "string": "同 FindData",
        "step": "搜索步进字节数（1=逐字节最准最慢）",
        "count": "最大返回结果数",
    },
    "GetModuleBaseAddr": {"module": "模块名（如 <code>game.exe</code>）", "etstr": "模块基址（16 进制串）"},
}


def parse_header(path):
    lines = open(path, encoding="utf-8", errors="replace").read().splitlines()
    fns = []  # {group,name,ret,params:[{dir,type,name}],desc}
    banner = None
    comments = []
    i = 0
    while i < len(lines):
        line = lines[i]
        m = BANNER_RE.match(line)
        if m:
            t = m.group(1).strip()
            if t in BANNER_MAP:
                banner = t
            comments = []
            i += 1
            continue
        s = line.strip()
        if s.startswith("//"):
            txt = s[2:].strip()
            if txt and not txt.startswith("以下 public"):
                comments.append(txt)
            i += 1
            continue
        # 方法声明：以 void/long + Name( 开头，跨行收集到分号
        if re.match(r"^(void|long)\s+\w+\s*\(", s) or re.match(r"^void\s+\w+\s*$", s):
            decl = s
            while ";" not in decl and i + 1 < len(lines):
                i += 1
                decl += " " + lines[i].strip()
            decl = decl.rstrip(";").strip()
            mm = re.match(r"^(void|long)\s+(\w+)\s*\((.*)\)$", decl, re.S)
            if mm:
                ret, name, pstr = mm.groups()
                group = REROUTE.get(name) or ("memory" if name in MEMORY_FNS else
                             (BANNER_MAP[banner][0] if banner else "memory"))
                params = parse_params(pstr)
                desc = "\n".join(comments).strip()
                fns.append({"group": group, "name": name, "ret": ret,
                            "params": params, "desc": desc})
            comments = []
            i += 1
            continue
        if s == "":
            i += 1
            continue
        # 其他代码行（类声明等）：不重置注释，但方法前的空行/杂行后注释仍有效
        if not s.startswith("//"):
            comments = []
        i += 1
    return fns


def parse_params(pstr):
    """按顶层逗号切分参数，解析方向/类型/名。"""
    pstr = pstr.strip()
    if not pstr or pstr == "void":
        return []
    parts, depth, cur = [], 0, ""
    for ch in pstr:
        if ch in "(<[":
            depth += 1
        elif ch in ")>]":
            depth -= 1
        if ch == "," and depth == 0:
            parts.append(cur)
            cur = ""
        else:
            cur += ch
    if cur.strip():
        parts.append(cur)
    out = []
    for p in parts:
        p = p.strip()
        direction = "in"
        if "_Inout_" in p:
            direction = "inout"
        elif "_Out_" in p:
            direction = "out"
        p = SAL_RE.sub("", p)
        p = re.sub(r"\s+", " ", p).strip()
        m = re.match(r"^(.*?[\w\)])\s*(\*+)?\s*(\w+)$", p)
        if not m:
            out.append({"dir": direction, "type": p, "name": ""})
            continue
        typ, stars, name = m.groups()
        typ = (typ + (stars or "")).replace(" *", "*").strip()
        out.append({"dir": direction, "type": typ, "name": name})
    return out


def py_sig(fn):
    """Python 风格调用签名：入参按序，出参聚成返回值元组。"""
    ins = [p["name"] for p in fn["params"] if p["dir"] in ("in", "inout")]
    outs = [p["name"] for p in fn["params"] if p["dir"] == "out"]
    # ret / bret / rettitle 等是返回值状态位，显示为 ret
    outs_disp = ["ret" if o in ("ret", "bret", "rettitle", "rety", "retx") else o for o in outs]
    sig = f"{fn['name']}({', '.join(ins)})"
    if outs_disp:
        sig += " → (" + ", ".join(outs_disp) + ")"
    else:
        sig += " → ret" if fn["ret"] == "long" else ""
    return sig


TYPE_CN = {
    "long": "long 整数", "double": "double 浮点", "float": "float 浮点",
    "int64_t": "int64 整数", "size_t": "size_t 指针/长度", "void": "void",
    "unsigned long": "ulong (PID)", "LONG_PTR": "LONG_PTR 句柄/指针",
}


def esc(s):
    return html.escape(s, quote=False)


def render(fns, src_rel, gen_time):
    groups = {k: [] for k in GROUP_ORDER}
    for fn in fns:
        groups.setdefault(fn["group"], []).append(fn)
    total = len(fns)

    nav_items, cards = [], []
    for gkey in GROUP_ORDER:
        gfs = groups.get(gkey, [])
        if not gfs:
            continue
        nav_items.append(f'<div class="nav-group"><a class="nav-g" href="#g-{gkey}">'
                         f'{esc(GROUP_NAME[gkey])} <span class="cnt">{len(gfs)}</span></a>')
        nav_items.append('<div class="nav-fns">')
        cards.append(f'<section class="group" id="g-{gkey}">'
                     f'<h2>{esc(GROUP_NAME[gkey])} <span class="cnt">{len(gfs)}</span></h2>'
                     f'<p class="g-desc">{esc(GROUP_DESC[gkey])}</p>')
        for fn in gfs:
            anchor = f"fn-{fn['name']}"
            nav_items.append(f'<a class="nav-f" href="#{anchor}" data-name="{fn["name"].lower()}">'
                             f'{esc(fn["name"])}</a>')
            # 参数表（带取值/语义注解列）
            pdoc = PARAM_DOCS.get(fn["name"], {})
            if fn["params"]:
                rows = "".join(
                    f'<tr><td class="p-name"><code>{esc(p["name"])}</code></td>'
                    f'<td class="p-type"><code>{esc(p["type"])}</code></td>'
                    f'<td class="p-dir"><span class="dir {p["dir"]}">{p["dir"]}</span></td>'
                    f'<td class="p-doc">{pdoc.get(p["name"]) or GLOBAL_PARAM_DOCS.get(p["name"], "")}</td></tr>'
                    for p in fn["params"])
                ptable = f'<table class="params"><thead><tr><th>参数</th><th>类型</th><th>方向</th><th>取值 / 语义</th></tr></thead><tbody>{rows}</tbody></table>'
            else:
                ptable = '<p class="no-params">（无参数）</p>'
            desc_html = "<br>".join(esc(l) for l in fn["desc"].splitlines()) if fn["desc"] else '<span class="nodesc">（源文件无注释）</span>'
            cards.append(
                f'<li class="fn" id="{anchor}" data-name="{fn["name"].lower()}">'
                f'<div class="fn-head"><code class="fn-name">{esc(fn["name"])}</code>'
                f'<span class="fn-sig">{esc(py_sig(fn))}</span>'
                f'<button class="copy" data-name="{esc(fn["name"])}" title="复制方法名">⧉</button></div>'
                f'<div class="fn-desc">{desc_html}</div>{ptable}</li>')
        cards.append('</section>')
        nav_items.append('</div></div>')

    return TEMPLATE.replace("__TITLE__", "OP 库 API 参考手册") \
        .replace("__GEN__", f"源：{esc(src_rel)} · {total} 个接口 · 生成于 {gen_time} · 由 scripts/gen_api_reference.py 生成") \
        .replace("__NAV__", "\n".join(nav_items)) \
        .replace("__CARDS__", "\n".join(cards)) \
        .replace("__TOTAL__", str(total))


TEMPLATE = """<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>__TITLE__</title>
<style>
:root{--bg:#f7f8fa;--panel:#fff;--line:#e3e6ea;--txt:#1f2329;--sub:#5a6270;
--accent:#2563eb;--accent-soft:#eaf1fe;--code:#0b57d0;--chip:#eef1f4;}
*{box-sizing:border-box}
body{margin:0;font:14px/1.65 "Segoe UI","Microsoft YaHei",system-ui,sans-serif;color:var(--txt);background:var(--bg)}
.layout{display:flex;min-height:100vh}
nav{width:280px;flex:0 0 280px;position:sticky;top:0;height:100vh;overflow:auto;background:var(--panel);border-right:1px solid var(--line);padding:16px 12px}
nav .brand{font-size:16px;font-weight:700;margin:4px 8px 10px}
nav .brand small{display:block;font-weight:400;color:var(--sub);font-size:11px;margin-top:2px}
#q{width:100%;padding:8px 10px;border:1px solid var(--line);border-radius:8px;font-size:13px;background:var(--bg);margin-bottom:10px;outline:none}
#q:focus{border-color:var(--accent)}
.nav-g{display:block;font-weight:600;color:var(--txt);text-decoration:none;padding:6px 8px;border-radius:6px;font-size:13px}
.nav-g:hover{background:var(--accent-soft)}
.nav-g .cnt,.cnt{color:var(--sub);font-weight:400;font-size:12px;margin-left:4px}
.nav-fns{display:flex;flex-direction:column;margin:0 0 6px 8px;border-left:2px solid var(--line);padding-left:6px}
.nav-f{font-size:12px;color:var(--sub);text-decoration:none;padding:2px 6px;border-radius:4px;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.nav-f:hover{color:var(--accent);background:var(--accent-soft)}
main{flex:1;padding:24px 32px;max-width:1080px}
.gen{color:var(--sub);font-size:12px;margin:0 0 16px}
.group h2{font-size:19px;margin:28px 0 4px;padding-bottom:6px;border-bottom:2px solid var(--accent)}
.g-desc{color:var(--sub);font-size:13px;margin:4px 0 12px}
ul.fns{list-style:none;margin:0;padding:0}
.fn{background:var(--panel);border:1px solid var(--line);border-radius:10px;padding:12px 16px;margin:10px 0}
.fn-head{display:flex;align-items:center;gap:10px;flex-wrap:wrap}
.fn-name{font-size:15px;font-weight:700;color:var(--code);background:var(--chip);padding:2px 8px;border-radius:6px}
.fn-sig{font-family:Consolas,monospace;font-size:12.5px;color:var(--txt)}
.copy{margin-left:auto;border:1px solid var(--line);background:var(--bg);border-radius:6px;cursor:pointer;padding:2px 8px;color:var(--sub)}
.copy:hover{border-color:var(--accent);color:var(--accent)}
.fn-desc{margin:8px 0;font-size:13.5px;color:var(--txt)}
.nodesc{color:var(--sub);font-style:italic}
table.params{border-collapse:collapse;width:100%;margin:6px 0 2px;font-size:12.5px}
table.params th,table.params td{border:1px solid var(--line);padding:4px 10px;text-align:left}
table.params th{background:var(--chip);font-weight:600}
.p-name code{color:var(--code)}
.p-type code{color:var(--sub);font-size:12px}
.p-doc{font-size:12px;color:var(--txt);line-height:1.6}
.p-doc code{background:var(--chip);padding:0 4px;border-radius:4px;font-size:11.5px;color:var(--code)}
.dir{display:inline-block;padding:0 8px;border-radius:10px;font-size:11px;font-weight:600}
.dir.in{background:#e8f5e9;color:#2e7d32}
.dir.out{background:#fdecea;color:#c62828}
.dir.inout{background:#fff8e1;color:#f9a825}
.no-params{color:var(--sub);font-size:12px;margin:4px 0}
.hidden{display:none!important}
</style>
</head>
<body>
<div class="layout">
<nav>
  <div class="brand">OP 库 API 参考手册<small>__TOTAL__ 个接口 · 10 个服务组</small></div>
  <input id="q" type="search" placeholder="搜索函数名…" autocomplete="off">
__NAV__
</nav>
<main>
<p class="gen">__GEN__</p>
__CARDS__
</main>
</div>
<script>
const q=document.getElementById('q');
const all=[...document.querySelectorAll('.fn')];
const navFns=[...document.querySelectorAll('.nav-f')];
q.addEventListener('input',()=>{
  const v=q.value.trim().toLowerCase();
  all.forEach(el=>{el.classList.toggle('hidden',v&&!el.dataset.name.includes(v));});
  navFns.forEach(el=>{el.classList.toggle('hidden',v&&!el.dataset.name.includes(v));});
  document.querySelectorAll('.group').forEach(g=>{
    const any=[...g.querySelectorAll('.fn')].some(el=>!el.classList.contains('hidden'));
    g.classList.toggle('hidden',!any);
  });
});
document.querySelectorAll('.copy').forEach(b=>b.addEventListener('click',()=>{
  navigator.clipboard.writeText(b.dataset.name).then(()=>{
    b.textContent='✓';setTimeout(()=>b.textContent='⧉',800);
  });
}));
</script>
</body>
</html>
"""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=os.path.join(ROOT, "docs", "api_reference.html"))
    ap.add_argument("--header", default=HEADER)
    a = ap.parse_args()
    fns = parse_header(a.header)
    dup = [n for n in {f["name"] for f in fns} if sum(1 for f in fns if f["name"] == n) > 1]
    if dup:
        print("WARN duplicate:", dup)
    gen_time = datetime.datetime.now().strftime("%Y-%m-%d %H:%M")
    out = render(fns, os.path.relpath(a.header, ROOT), gen_time)
    os.makedirs(os.path.dirname(a.out), exist_ok=True)
    open(a.out, "w", encoding="utf-8").write(out)
    # 统计输出 + 注解覆盖率
    from collections import Counter
    c = Counter(f["group"] for f in fns)
    print(f"total: {len(fns)}")
    for k in GROUP_ORDER:
        if c.get(k):
            print(f"  {GROUP_NAME[k]}: {c[k]}")
    n_params = 0
    n_doc = 0
    for fn in fns:
        fdoc = PARAM_DOCS.get(fn["name"], {})
        for p in fn["params"]:
            n_params += 1
            if fdoc.get(p["name"]) or GLOBAL_PARAM_DOCS.get(p["name"]):
                n_doc += 1
    print(f"参数注解覆盖: {n_doc}/{n_params} ({100.0 * n_doc / max(1, n_params):.1f}%)")
    print("->", os.path.relpath(a.out, ROOT))


if __name__ == "__main__":
    main()
