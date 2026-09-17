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
    "FindWindow": {
        "class_name": "精确匹配（直调 FindWindowW）。空串或 NULL = 不限",
        "title": "精确匹配。空串或 NULL = 不限。需唯一窗口名时用本函数；子串查找请用 EnumWindow 系",
    },
    "FindWindowEx": {
        "parent": "父窗口句柄，0 = 顶层",
        "class_name": "精确匹配，空/NULL 不限",
        "title": "精确匹配，空/NULL 不限",
    },
    "EnumWindow": {
        "title": "<b>模糊匹配</b>（wcsstr 子串包含，大小写敏感）；空 = 不限。注意单字符标题(&lt;2 字符)不参与匹配",
        "class_name": "模糊匹配（子串包含）；空 = 不限。title 与 class_name 同时传时为 AND 关系",
        "filter": "1=匹配标题 2=匹配类名 4=只匹配父窗口第一层子窗 8=只匹配顶级窗口(Owner=0)；按位或组合；<b>+32</b>=结果按 Z 序排列（枚举顺序本身即 Z 序，仅兼容语义）",
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
    "GetClientRect": {
        "ret": "坐标为<b>物理像素</b>（OP 构造时提升进程 DPI 感知，见 OP_DPI 语义文档）。窗口最小化时返回 -32000 哨兵"
    },
    "LayoutWindows": {
        "layout_type": "0=宫格 1=对角线 2=层叠Cascade（第 i 窗偏移 i×gap）",
        "size_mode": "0=保持各自原大小 1=统一客户区大小（须 ≥50×50，否则 ret=0）",
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
        "delta_color": "透明色（16 进制 BGR，如 000000 表黑色透）；不需要透明匹配传 <code>000000</code> 配 sim 使用普通匹配",
        "sim": "相似度 0~1。内部阈值=<code>0.5 + sim/2</code>；越界不钳制",
        "dir": "0=左上→右下 1=左下→右上 2=右上→左下 3=右下→左上",
    },
    "FindColor": {
        "color": "16 进制 RGB（如 <code>eef4f5</code>），可带偏移标记",
        "sim": "相似度 0~1；<b>sim&lt;0 时钳为 1.0（变精确匹配）</b>。多点串中 sim 同理",
        "dir": "0=左上→右下 1=左下→右上 2=右上→左下 3=右下→左上",
    },
    "FindMultiColor": {
        "first_color": "基准点颜色 RGB",
        "offset_color": "偏移点串：<code>\"dx|dy|RGB,dx|dy|RGB,...\"</code>；畸形段自动跳过（09-17 加固）",
    },
    "FindColorBlock": {
        "count": "期望找到的色块数量",
        "height": "色块最小高度（⚠️ 顺序是 count, <b>height, width</b>——高在宽前，与大漠一致）",
        "width": "色块最小宽度",
    },
    "GetColor": {"ret": "BGR 16 进制串，如 <code>eef4f5</code>"},
    "CmpColor": {"sim": "同 FindColor：sim&lt;0 钳为 1.0"},
    # ---------- Input 键鼠 ----------
    "SetMouseDelay": {
        "t": "点击/弹起间隔基准毫秒值，实际值 = 基准 ±40% 随机抖动（下限 1ms，防按下弹起合并为单次）。0=不等待"
    },
    "SetKeypadDelay": {"t": "同 SetMouseDelay：基准值 + ±40% 抖动"},
    "MoveTo": {
        "options": "扩展选项（大漠兼容位），当前实现按 0 处理"
    },
    "WaitKey": {"key_code": "虚拟键码 VK（如 13=回车）"},
    # ---------- Runtime 运行时 ----------
    "Sleep": {"millseconds": "裸 ::Sleep，期间<b>不处理窗口消息（界面冻结）</b>。脚本侧建议改用 Delay/Delays"},
    "Delay": {"mis": "带消息泵的阻塞延时：线程挂起不占 CPU，期间窗口消息正常派发"},
    "Delays": {"mis_min": "区间下限毫秒", "mis_max": "区间上限毫秒（max&lt;min 自动交换）；区间内均匀随机取一值后走 Delay"},
    "RunApp": {
        "cmdline": "可执行文件完整路径；<b>支持 .lnk 快捷方式</b>（经 ShellExecuteEx 解析，lnk 里的工作目录/参数一并生效）。cwd 敏感的程序建议 mode=1 或走 lnk",
        "mode": "0=以<b>调用方当前目录</b>为工作目录启动（cwd 敏感程序会失败）；1=以 exe 所在目录为工作目录（推荐用于游戏/大型程序）。.lnk 时本参数不生效",
        "pid": "新进程 PID；.lnk 走 Shell 执行时若拿不到句柄则为 0（ret 仍为 1）",
    },
    "GetCmdStr": {"millseconds": "等待输出的毫秒上限，超时杀整棵进程树"},
    # ---------- OCR ----------
    "SetDict": {"index": "字库槽位 0~9；0 为全局默认，1~9 为私有槽（可被 UseDict 切换）"},
    "UseDict": {"index": "切换当前生效字库槽位"},
    "FindStr": {
        "color_format": "颜色格式串（同 OCR），如 <code>\"9f2e3f-000000\"</code>",
        "sim": "⚠️ OCR 系 sim&lt;0 时<b>回退 0.7</b>（与找色系钳 1.0 不同）",
    },
    # ---------- Memory ----------
    "FindData": {
        "hwnd": "0 = 优先作用于当前绑定窗口的进程（非系统全局）",
        "data": "特征码串，支持 ?? 通配与 64 位地址块，详见 OP_CAPABILITY_INVENTORY",
    },
    # ---------- OpenCV ----------
    "CvMatchTemplate": {
        "method": "匹配算法：0=TM_SQDIFF 1=TM_SQDIFF_NORMED 2=TM_CCORR 3=TM_CCORR_NORMED 4=TM_CCOEFF 5=TM_CCOEFF_NORMED"
    },
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
                    f'<td class="p-doc">{pdoc.get(p["name"], "")}</td></tr>'
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
    # 统计输出
    from collections import Counter
    c = Counter(f["group"] for f in fns)
    print(f"total: {len(fns)}")
    for k in GROUP_ORDER:
        if c.get(k):
            print(f"  {GROUP_NAME[k]}: {c[k]}")
    print("->", os.path.relpath(a.out, ROOT))


if __name__ == "__main__":
    main()
