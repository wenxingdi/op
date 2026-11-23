#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""生成 C API 手册（docs/2026-11/OP_API手册_20261123.md）。

为什么要有这个脚本
------------------
`docs/2026-10/OP_API手册_20261015.md` 是 316 条时代的**手写**功能对比基线，
后来陆续加了 116 个导出（加密/注册表/热键/并发/绘制/UIA/图像句柄/内存版识别/
音频/YOLO/汇编/时间轴/颜色转换…），手册没跟上，于是「手册 316 ↔ 头文件 432」
对不上。手写维护这种清单必然再次漂移，所以改成生成。

分区策略（两条硬规则，保证结果可预测）
--------------------------------------
1. **旧手册里已有的函数，分区原样保留**，一条都不许漂移。
   实现：先从旧手册解析出 fn -> 分区名 的基线映射，优先命中。
2. 新增函数按 `NEW_SECTIONS` 的正则**显式**归类；仍有剩余则归入「未归类」，
   由脚本以非零退出码报警 —— 不静默塞进某个分区。

用法
----
    python scripts/gen_c_api_manual.py            # 生成
    python scripts/gen_c_api_manual.py --check    # 只校验条数，不写文件
"""

from __future__ import annotations

import argparse
import os
import re
import sys
from datetime import date

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HEADER = os.path.join(ROOT, "include", "op_c_api.h")
OLD_MANUAL = os.path.join(ROOT, "docs", "2026-10", "OP_API手册_20261015.md")
OUT_MANUAL = os.path.join(ROOT, "docs", "2026-11", "OP_API手册_20261123.md")

# ---------------------------------------------------------------- 新分区规则
# (分区名, 功能域说明, 匹配函数名正则)。顺序即手册里的出现顺序，先匹配先归属。
# 只对「旧手册没收录」的函数生效 —— 老函数永远跟着旧手册走。
NEW_SECTIONS = [
    # `^OpYolo` 必须排第一：YOLO 也有 `*FromMem` 变体，若先命中「内存版识别入口」的
    # `FromMem$` 规则，YOLO 推理接口会被错归到图色内存族（2026-11-23 首次生成时踩到）。
    ("YOLO 任务维度与模型平台", "Detect/Pose/Obb/Segment/Classify + 模型加载列表加密解密",
     r"^OpYolo"),
    ("加密与摘要（2026-11-23）", "AES/RSA/SHA/MD5/HMAC/PBKDF2 文件与内存摘要/GUID/随机字节",
     r"^Op(Aes(Encrypt|Decrypt)|Rsa(Encrypt|Decrypt)|Sha1File|Sha256|Sha256File|Md5File|"
     r"HmacSha256|Pbkdf2Sha256|GenerateGuid|GenerateRandomBytes)$"),
    ("注册表（2026-11-23）", "HKCU/HKLM 字符串与 DWORD 读写",
     r"^OpRegistry"),
    ("全局热键（2026-11-23）", "注册/注销/轮询/全量注销",
     r"^OpHotkey"),
    ("并发诊断（批 A 2026-11-22）", "锁竞争与队列深度快照",
     r"^OpConcurrencyInfo$"),
    ("图像句柄注册表（P4 2026-11-21）", "图片载入内存句柄/取数据与尺寸/全局计数/释放",
     r"^Op(LoadImageBmp|LoadImageMem|GetImageData|GetImageSize|GetImageCount|FreeImage|FreeAllImages)$"),
    ("内存版识别入口（二期 2026-11-21）", "从内存图像直接比色/找色/找图/OCR，免落盘",
     r"FromMem(Ex2)?$|^OpOcrEx2$|^OpOcrAutoColorEx2$"),
    ("绘制：状态与图片上屏（批 G 2026-11-23）", "全局描边/填充状态、按 alpha 叠图、截图句柄直接上屏、绑定层计数",
     r"^OpDraw(SetColor|SetFill|ImageEx|ImageFromHandle|ClearBind|GetCount)$"),
    ("绘制：标签替换（批 G2 2026-11-23）", "同标签自动替换而非累加，免去脚本维护数字 id",
     r"^OpDraw(RectTag|RectExTag|RemoveTag)$"),
    ("UIA 元素层（批 H 2026-11-23）", "dumping/选择器查找/坐标命中/Invoke/Value 读写/元素释放",
     r"^OpUia"),
    ("音频音量（WASAPI 2026-11-21）", "会话级与系统级音量/静音/进程与窗口反查",
     r"^OpAudio"),
    ("汇编模块（批 D / 59 批）", "机器码远程执行 / 汇编转机器码 / 反汇编",
     r"^Op(AsmCall|Assemble|Disassemble)$"),
    ("时间轴层（批 E 2026-11-22）", "录制/回放/输入监听/主从同步/总线标识",
     r"^Op(Input|Timeline)|^OpSetInputMonitor$"),
    ("颜色空间与十六进制转换（补充）", "RGB/HSV/ARGB 与 hex 串互转、取色十六进制形式",
     r"^Op(ARGB2Hex|Hex2ARGB|Hex2RGB|Hex2HSV|RGB2HSV|GetColorHex)$"),
    # 以下三条把「旧手册已有分区、但里面是新函数」的情况补进去（分区名就是旧分区名）
    ("Draw overlay", "全屏分层窗口绘制：基础图形 / 带线宽与 mode 的 *Ex / 圆与文字 / 显隐与清空",
     r"^OpDraw"),
    ("Image and color", "找图找色找块找线/取色/截图数据/卡屏检测/图片缓存/裁剪与密致矩形/排除坐标",
     r"^Op(CropImage|GetDenseRect|ExcludePos)$"),
    ("OCR", "引擎选择/预热/字库维护（增·删·存·读）/免字库 OCR 三族/FindStr（含内存版与批量）/词提取预览/画像",
     r"^Op(FindStrMem|FindStrBatch|FindStrEx2|OcrLastProfile|RemoveDict)$"),
]

# 旧手册的 20 个分区按原始顺序列出，保证新手册的章节顺序与旧版一致。
# 旧手册标题形如 `## File（12 个，第十九批新增 2026-10-15）`，解析时只取括号前的短名。
OLD_SECTION_ORDER = [
    "Lifecycle", "Basic", "Algorithm", "Window and process", "System misc",
    "Background binding", "Mouse", "Keyboard", "Image and color", "OpenCV",
    "OCR", "Memory", "File", "Input 补充", "Window 补充", "Process 补充",
    "QRCode", "Draw overlay", "JSON", "HTTP",
]


def read(path: str) -> str:
    with open(path, encoding="utf-8") as fh:
        return fh.read()


def parse_header(src: str) -> list[tuple[str, str]]:
    """从 op_c_api.h 抽出 (函数名, 一行化签名)，保持声明顺序。"""
    out: list[tuple[str, str]] = []
    for m in re.finditer(r"OP_C_API\s+[^;]*?\b(Op\w+)\s*\([^;]*?\)\s*;", src):
        sig = re.sub(r"\s+", " ", m.group(0)).strip()
        # 只压掉括号内侧空白；逗号后统一留一个空格（旧手册的排版口径）。
        # 指针星号两侧空白**不动**：头文件里是 `const wchar_t *OP_CALL`，压掉会粘成 `wchar_t*OP_CALL`。
        sig = re.sub(r"\s*\(\s*", "(", sig)
        sig = re.sub(r"\s*\)\s*", ")", sig)
        sig = re.sub(r"\s*,\s*", ", ", sig)
        name = re.search(r"\b(Op\w+)\s*\(", sig).group(1)
        out.append((name, sig))
    return out


def parse_old_manual(src: str) -> tuple[dict[str, str], dict[str, list[str]]]:
    """解析旧手册：返回 fn->分区名 与 分区名->[fn...]（保序）。"""
    owner: dict[str, str] = {}
    order: dict[str, list[str]] = {}
    cur = None
    for line in src.splitlines():
        # 标题形如 `## File（12 个，第十九批新增 2026-10-15）`：只取括号前的短名
        h = re.match(r"^##\s+(.+?)（\d+\s*个[^）]*）\s*$", line)
        if h:
            cur = h.group(1).strip()
            order.setdefault(cur, [])
            continue
        if line.startswith("## "):
            cur = None
            continue
        if cur:
            for fn in re.findall(r"`[^`]*?\b(Op\w+)\s*\(", line):
                if fn not in owner:
                    owner[fn] = cur
                    order[cur].append(fn)
    return owner, order


def classify(name: str, owner: dict[str, str]) -> str | None:
    if name in owner:
        return owner[name]
    for title, _desc, pattern in NEW_SECTIONS:
        if re.search(pattern, name):
            return title
    return None


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true", help="只校验，不写文件")
    args = ap.parse_args()

    decls = parse_header(read(HEADER))
    if len({n for n, _ in decls}) != len(decls):
        print("头文件里有重名导出，先处理再生成", file=sys.stderr)
        return 2

    old_owner, _old_order = parse_old_manual(read(OLD_MANUAL))

    sections: dict[str, list[tuple[str, str]]] = {}
    unclassified: list[str] = []
    for name, sig in decls:
        sec = classify(name, old_owner)
        if sec is None:
            unclassified.append(name)
            continue
        sections.setdefault(sec, []).append((name, sig))

    total = sum(len(v) for v in sections.values())
    print(f"头文件导出 {len(decls)} 条；已归类 {total} 条；未归类 {len(unclassified)} 条")
    if unclassified:
        print("未归类（请补 NEW_SECTIONS 规则，脚本不静默兜底）:", file=sys.stderr)
        for n in unclassified:
            print("  -", n, file=sys.stderr)
        return 1
    if args.check:
        return 0

    # 章节顺序：旧 20 分区按原顺序在前，新分区按 NEW_SECTIONS 顺序在后（去重）
    order: list[str] = [s for s in OLD_SECTION_ORDER if s in sections]
    order += [t for t, _d, _p in NEW_SECTIONS if t in sections and t not in order]
    order += [s for s in sections if s not in order]  # 兜底：旧手册里出现的其他分区名

    desc_of = {t: d for t, d, _p in NEW_SECTIONS}
    old_desc = {
        "Lifecycle": "对象创建/销毁",
        "Basic": "版本/路径/错误/插件注入/图片缓存/A* 寻路入口/截图预处理",
        "Algorithm": "A* 寻路（点阵版）/最近坐标/坐标距离排序/最近可走点吸附",
        "Window and process": "窗口查找枚举/窗口属性读写/进程信息/剪贴板/字符串发送/运行程序",
        "System misc": "屏幕参数/DPI/时间/蜂鸣/随机数/机器码/管理员检查/调试权限/错误文本/摘要编码",
        "Background binding": "后台绑定 dx 系/降载/输入通道开关",
        "Mouse": "移动（含轨迹/平滑）/三键+X1X2/滚轮/光标形状与图像/延迟",
        "Keyboard": "按键（码/字符/字符串）/等待按键/延迟",
        "Image and color": "找图找色找块找线/取色/截图数据/卡屏检测/图片缓存/裁剪与密致矩形/找图排除坐标",
        "OpenCV": "模板管理/预处理（灰度/二值/边缘/形态学/细化）/旋转翻转/多策略匹配",
        "OCR": "引擎选择/预热/字库维护/免字库 OCR 三族/FindStr（含内存版·批量·Ex2）/词提取预览/画像",
        "Memory": "进程内存读写/特征码搜索/模块基址/模块大小/模块列表/远程分配与保护修改",
        "File": "文件存在/目录增删（递归）/遍历/拷贝移动删除/大小/UTF-8 读写/路径解析",
        "Input 补充": "组合键 KeyPressHotkey",
        "Window 补充": "窗口 DPI 缩放 GetWindowScale",
        "Process 补充": "进程终止/进程树终止",
        "QRCode": "区域/文件解码二维码 + 生成二维码 PNG",
        "Draw overlay": "全屏分层窗口绘制：矩形/直线/文字/圆/图片/清空/显隐，*Ex 带线宽与 mode",
        "JSON": "校验/路径直取（字符串/数值/布尔/长度/键名）/路径写入三型/数组追加/删除",
        "HTTP": "WinHTTP 系统直连 GET/POST（对接大模型 API）+ 文件下载",
    }

    com_ids = len(re.findall(r"^\s*\[id\(\d+\)\]", read(os.path.join(ROOT, "libop", "com", "op.idl")), re.M))
    today = date.today().isoformat()

    L: list[str] = []
    L.append(f"# OP Master API 手册（{today}）")
    L.append("")
    L.append(f"> 数据源：`include/op_c_api.h`（C ABI 导出契约，**{len(decls)}** 个 `OP_CALL` 导出）。")
    L.append(f"> 同仓 COM 接口数（`libop/com/op.idl` 的 `id(...)`）为 **{com_ids}** —— 与 C ABI 是**不同口径**，不可互相引用。")
    L.append("> 本手册由 `scripts/gen_c_api_manual.py` 生成，**请勿手改**；新增导出后重跑脚本即可。")
    L.append("")
    L.append("> **v1→v2 更正（2026-11-23）**：上一版 `docs/2026-10/OP_API手册_20261015.md` "
             "停在 316 条，此后新增的 116 个导出（加密/注册表/热键/并发/绘制/UIA/图像句柄/内存版识别/"
             "音频/YOLO/汇编/时间轴/颜色转换等）未收录，且该文件是手写清单、每加一批接口就漂一次。"
             "本版改为脚本生成，条数与头文件**逐条对得上**，并由脚本在出现未归类函数时**非零退出报警**"
             "——不再有「手册说 316、实际 432」这种静默偏差。旧文件保留原样作为历史基线。")
    L.append("")
    L.append("## 模块总览")
    L.append("")
    L.append("| 分区 | 导出数 | 功能域 |")
    L.append("|---|---|---|")
    for sec in order:
        desc = old_desc.get(sec) or desc_of.get(sec, "")
        L.append(f"| {sec} | {len(sections[sec])} | {desc} |")
    L.append(f"| **合计** | **{total}** | — |")
    L.append("")
    L.append("**命名族说明**：免字库 OCR 现行名 = `OcrAuto`（原图）/ `OcrAutoColor*`（按色二值化）/")
    L.append("`AutoOcr`（归一入口，推荐）；`OpAutoOcrLine/Ex/FromFile` 为旧名废弃别名（保留转发）。")
    L.append("`OpOcrAutoFromFile` 同理为旧名别名。")
    L.append("")
    L.append("---")
    L.append("")
    for sec in order:
        L.append(f"## {sec}（{len(sections[sec])} 个）")
        L.append("")
        for _name, sig in sections[sec]:
            L.append(f"- `{sig}`")
        L.append("")

    os.makedirs(os.path.dirname(OUT_MANUAL), exist_ok=True)
    with open(OUT_MANUAL, "w", encoding="utf-8", newline="\n") as fh:
        fh.write("\n".join(L).rstrip() + "\n")

    print(f"已写出 {os.path.relpath(OUT_MANUAL, ROOT)}（{total} 条 / {len(order)} 个分区）")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
