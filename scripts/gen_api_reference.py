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
            # 参数表
            if fn["params"]:
                rows = "".join(
                    f'<tr><td class="p-name"><code>{esc(p["name"])}</code></td>'
                    f'<td class="p-type"><code>{esc(p["type"])}</code></td>'
                    f'<td class="p-dir"><span class="dir {p["dir"]}">{p["dir"]}</span></td></tr>'
                    for p in fn["params"])
                ptable = f'<table class="params"><thead><tr><th>参数</th><th>类型</th><th>方向</th></tr></thead><tbody>{rows}</tbody></table>'
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
