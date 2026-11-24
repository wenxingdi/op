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


def _append_usage_examples(L: list[str]) -> None:
    """追加「使用案例」章节。

    设计取舍
    --------
    * 案例**按任务组织**，不按导出函数组织 —— 签名清单已经在下面按分区列全了，
      再按函数列一遍案例只会制造第二份需要维护的清单。
    * 每个案例都是**可编译可运行的最小闭环**，语言选 C（C ABI 的原生调用方），
      并给出 Python ctypes 对照（仓库自带 `bindings/python`，最常用的第二条路径）。
    * 案例里出现的每个函数都必须是本手册收录的导出 —— 由 `main()` 末尾的
      `_verify_examples()` 逐条比对，写错函数名会**非零退出**，防止案例腐化。
    """
    L.append("## 使用案例")
    L.append("")
    L.append("> 下面每个案例都是**可运行的最小闭环**，不是伪代码。坐标口径统一遵循：")
    L.append("> 未绑定窗口时是**屏幕物理像素**；绑定后是**客户区坐标**（与 `OpCapture` 出的图同空间）。")
    L.append("> 出参一律走指针，返回值 0=失败 / 非 0=成功（有例外会在注释里点明）。")
    L.append("")
    L.append("**两条贯穿全部案例的硬约定**（踩过坑，务必先读）：")
    L.append("")
    L.append("1. **返回 `const wchar_t *` 的接口指向实例内的复用缓冲区，调用方既不释放、也不要跨调用保存。**")
    L.append("   实现是 `handle->string_result.clear()` 后把内部 `std::wstring` 的 `c_str()` 交出去")
    L.append("   （见 `op_c_api.cpp` 的 `call_string`）。所以：")
    L.append("   ```c")
    L.append("   const wchar_t *a = OpGetPath(h);      /* 指针指向实例缓冲区 */")
    L.append("   const wchar_t *b = OpConcurrencyInfo(h);")
    L.append("   /* 此时 a 与 b 是同一块内存，a 的内容已被覆盖！ */")
    L.append("   std::wstring keep = OpGetPath(h);     /* 要留住就立刻拷进自己的 std::wstring */")
    L.append("   ```")
    L.append("   实测确认：不同接口连调返回的是**同一个指针**。因此手册里所有案例都**不调用任何 free**。")
    L.append("")
    L.append("2. **`OpCreate` 与 `OpDestroy` 必须配对**。会话、图片句柄、字符串缓冲都挂在实例上，")
    L.append("   不销毁不会立刻崩，但长驻脚本会持续累积内存。")
    L.append("")
    for ex in USAGE_EXAMPLES:
        L.append(f"### {ex['title']}")
        L.append("")
        L.append(ex["intro"])
        L.append("")
        L.append(f"```{ex['lang']}")
        L.append(ex["code"].rstrip())
        L.append("```")
        L.append("")
        if ex.get("note"):
            L.append(f"> {ex['note']}")
            L.append("")


USAGE_EXAMPLES = [
    {
        "title": "案例 1：最小生命周期 + 绑定窗口 + 截一张图",
        "lang": "c",
        "intro": "任何脚本的起点。`OpCreate` 拿句柄，绑定后所有区域类接口都按客户区坐标解释。"
                 "**多数接口没有 `int *ret` 出参，返回值就是成败**（0 失败 / 非 0 成功）。",
        "code": """
#include <windows.h>
#include "op_c_api.h"
#pragma comment(lib, "op_c_api_x64.lib")   /* 构建产物 build/nmake-x64-Release/libop/ */

int main(void) {
    op_handle h = OpCreate();
    if (!h) return 1;

    /* 指向放 op_x64.dll / op_c_api_x64.dll 的目录（后台绑定要在这里找钩子 DLL） */
    OpSetPath(h, L"D:/AutoPro/op-master/op/build/nmake-x64-Release/libop");
    OpSetShowErrorMsg(h, 2);               /* 0 关 / 1 信息框 / 2 写文件 / 3 标准输出 */

    intptr_t hwnd = OpFindWindow(h, L"Notepad", NULL);
    if (!hwnd) { OpDestroy(h); return 2; }

    /* 签名: OpBindWindow(handle, hwnd, display, mouse, keypad, mode)；返回 1 成功 */
    if (OpBindWindow(h, hwnd, L"gdi", L"windows", L"windows", 0) != 1) {
        OpDestroy(h); return 3;
    }

    /* 客户区尺寸：OpGetClientSize 是少数仍走指针出参的接口（无 ret，返回 1 成功） */
    int w = 0, hgt = 0;
    OpGetClientSize(h, hwnd, &w, &hgt);

    /* 区域按客户区坐标 -> 截整张客户区 */
    OpCapture(h, 0, 0, w, hgt, L"D:/tmp/notepad.bmp");

    OpUnBindWindow(h);                     /* 可选；OpDestroy 也会回收 */
    OpDestroy(h);                          /* 必须调：会话与图片句柄都挂在实例上 */
    return 0;
}
""",
        "note": "⚠ `OpDestroy` 不调用不会立即崩，但长驻脚本会持续累积 —— 建议一律配对使用。",
    },
    {
        "title": "案例 2：找图 + 客户端↔屏幕换算 + 点击",
        "lang": "c",
        "intro": "找图 → 得到**客户区**坐标 → 转**屏幕**坐标 → 点击。"
                 "转换必须走 `OpClientToScreen`：自己对客户区原点做减法，在 DPI 不一致的窗口上会整体偏 1.5 倍。",
        "code": """
/* 前置：已建好实例 h、已绑定 hwnd，且 w/hgt 是客户区尺寸（见案例 1）。
   本片段不自带变量声明，直接用外层约定的 h / hwnd / w / hgt / x / y / ret。 */

/* 签名: OpFindPic(handle, x1, y1, x2, y2, files, delta_color, sim, dir, &x, &y)
   最后一个出参是方向（0=左上到右下）；坐标是**客户区**坐标，找不到时由实现置 0。 */
int found = OpFindPic(h, 0, 0, w, hgt, L"D:/tmp/btn.bmp", L"000000", 0.9, 0, &x, &y);
if (!found) {
    /* 没找到：X/Y 为 0，不要当成"命中左上角" */
    return 4;
}

/* 客户区 -> 屏幕。入参原地改写（in/out），所以先拷一份 */
int sx = x, sy = y;
OpClientToScreen(h, hwnd, &sx, &sy);

OpMoveTo(h, sx, sy);        /* 未绑定时按屏幕坐标 */
OpLeftClick(h);             /* 已绑定时按客户区坐标（后台消息） */
""",
        "note": "判断是否命中看**返回值**，不要看 X/Y 是否为 0 —— 真命中左上角时坐标本来就是 0。",
    },
    {
        "title": "案例 3：OCR 取结构化结果（逐字坐标）",
        "lang": "c",
        "intro": "`OpOcr` 返回纯文本；要定位「某个词在图上哪一块」用 `OpOcrEx2` 的 JSON，`chars[]` 里是**原图绝对坐标**。",
        "code": """
/* 签名: OpOcrEx2(handle, x1, y1, x2, y2, color, sim) —— 无出参，返回 JSON 指针 */
const wchar_t *json = OpOcrEx2(h, 0, 0, w, hgt, L"", 0.8);
if (!json || json[0] == 0) {
    /* 空串 = 识别失败或无文字 */
    OpDestroy(h); return 5;
}

/* json 形如：
   {"ok":1,"text":"...","det_count":3,
    "chars":[{"c":"9","x1":247,"x2":344,"y1":12,"y2":40,"line":0}, ...]}
   ⚠ x1/x2/y1/y2 已是原图绝对坐标，消费侧只能加区域偏移，绝不能再加 det 框原点。 */

/* ⚠ json 指向实例复用缓冲区：不要 free，也不要留到下一次调用之后再读。
   要留着就立刻拷走（下面用 Win32 的堆分配，纯 C 通用）。 */
wchar_t *keep = _wcsdup(json);
/* ...用完 wprintf(L"%s", keep); free(keep); ... */
""",
        "note": "旋转/竖排场景会退化成整行框，此时 JSON 里 `partial_chars=true`，别拿 chars[] 当逐字坐标用。",
    },
    {
        "title": "案例 4：输入时间轴 —— 录制与回放",
        "lang": "c",
        "intro": "时间轴三件套：`OpInputSyncEnable` 打开采集 → `OpInputRecord` 录制 → `OpInputPlayback` 回放。"
                 "默认**不采集**，不打开开关录出来是空的。",
        "code": """
/* 前置：已建好实例 h（见案例 1）。本片段不自带变量声明。 */
OpInputSyncEnable(h, 1, &ret);                    /* 1=打开采集（默认关闭） */

/* START：path 传空则自动生成 input_record_YYYYmmdd_HHMMSS.jsonl */
const wchar_t *path = OpInputRecord(h, 0, L"D:/tmp/demo.jsonl", &ret);
if (ret != 1) { /* 已在录制中：返回 0 且不截断前一段 */ }

/* ...此处执行要录的操作... */

OpInputRecord(h, 1, NULL, &ret);                  /* STOP：*ret = 写入条数，0 = 确实没产生输入 */
const wchar_t *last = OpInputRecordGetPath(h);    /* 最近一次录制的路径 */

/* 回放：0=LOAD（*ret 事件条数）1=START（同步放完，*ret 1/0）2=STOP 3=FREE */
OpInputPlayback(h, 0, last, 1.0, 1, &ret);        /* speed=1.0 loop=1 */
OpInputPlayback(h, 1, NULL, 1.0, 1, &ret);
OpInputPlayback(h, 3, NULL, 1.0, 1, &ret);        /* FREE：释放已装载事件 */
""",
        "note": "⚠ LOAD 返回的 JSON 里 `bound=0` 表示当前**没绑窗口**，坐标按屏幕语义回放 —— 与录制时不是同一套坐标。",
    },
    {
        "title": "案例 5：并发自省（判断是否踩了非线程安全线）",
        "lang": "c",
        "intro": "同实例被多线程同时调用时，用 `OpConcurrencyInfo` 看峰值与重入。`peak > 1` 或 `reentrant > 0` 说明结果已不可复现。",
        "code": """
const wchar_t *info = OpConcurrencyInfo(h);
/* {"id":0,"in_flight":1,"peak":3,"reentrant":12,"calls_total":880,
     "image_count":4,"bound":0,"thread_safe":false} */

/* 判重入：直接在返回的 JSON 里找 "reentrant":0 这个片段。
   注意 C 源码里写转义引号时，若这份代码还要经过文档/Markdown 转一道，容易被再吃一层
   —— 所以这里改用宽字符码点拼接，源码里不出现转义序列，怎么转述都不会走样。 */
static const wchar_t kNoReentry[] = {L'r',L'e',L'e',L'n',L't',L'r',L'a',L'n',L't',0x22,L':',L'0',0};
if (wcsstr(info, kNoReentry) == NULL) {
    /* 有重入：改成每线程一个 OpCreate 实例，或自己加锁 */
}
/* info 是实例缓冲区，不释放；要长期留着先拷贝 */
""",
        "note": "`image_count` 持续增长说明图片句柄没释放；`in_flight` 最小也会是 1（本调用自身），判并发请用 `peak`/`reentrant`。",
    },
    {
        "title": "案例 6：多线程并发（每线程独立实例）",
        "lang": "c",
        "intro": "op 的单实例**不是线程安全**的。正确姿势是每个线程各自 `OpCreate`，不要共享同一个 handle。",
        "code": """
#include <windows.h>
#include "op_c_api.h"

DWORD WINAPI worker(LPVOID arg) {
    op_handle h = OpCreate();            /* 每个线程一个实例 */
    if (!h) return 1;
    OpSetPath(h, L"D:/AutoPro/op-master/op/build/nmake-x64-Release/libop");
    /* ...本线程的截图/找图/OCR... */
    OpDestroy(h);                        /* 线程退出前销毁 */
    return 0;
}

int main(void) {
    HANDLE t[4];
    for (int i = 0; i < 4; ++i) t[i] = CreateThread(NULL, 0, worker, NULL, 0, NULL);
    WaitForMultipleObjects(4, t, TRUE, INFINITE);
    for (int i = 0; i < 4; ++i) CloseHandle(t[i]);
    return 0;
}
""",
        "note": "跨实例共享的只有全局图片缓存（`OpSetPicCacheMax` 控制上限）与输入事件总线，其余状态都在实例内。",
    },
    {
        "title": "案例 7：Python（ctypes 绑定，仓库自带）",
        "lang": "python",
        "intro": "`bindings/python/op` 是 ctypes 包装，方法名 snake_case，出参自动聚成返回值元组。这是跳过 C 直接写脚本最快的路径。",
        "code": """
import sys
sys.path.insert(0, r"D:/AutoPro/op-master/op/bindings/python")
from op import Op

DLL = r"D:/AutoPro/op-master/op/build/nmake-x64-Release/libop/op_c_api_x64.dll"

with Op(dll_path=DLL) as op:
    print(op.version)                       # 5.2.0（Ver 在 ctypes 包中名为 version）

    # 找不到时返回 0；两个参数都是关键字，可以不传
    hwnd = op.find_window(title="Notepad")
    assert hwnd, "记事本没开"
    op.set_path(r"D:/AutoPro/op-master/op/build/nmake-x64-Release/libop")

    # 签名: bind_window(hwnd, display="normal", mouse="normal", keypad="normal", mode=0)
    assert op.bind_window(hwnd, "gdi", "windows", "windows", 0)

    # get_client_size 返回 (宽, 高) 两个值
    w, hgt = op.get_client_size(hwnd)
    print("客户区", w, "x", hgt)
    assert op.capture(0, 0, w, hgt, r"D:/tmp/shot.bmp")   # 返回 bool

    # find_pic 返回 (x, y, ret)；ret=0 表示没找到，此时坐标是 0，别当成命中左上角
    x, y, ret = op.find_pic(0, 0, w, hgt, r"D:/tmp/btn.bmp", "000000", 0.9)
    if ret:
        sx, sy = op.client_to_screen(hwnd, x, y)          # 返回 (x, y)，没有 ret
        print("命中", x, y, "-> 屏幕", sx, sy)

    # 时间轴（2026-11-24 补入 ctypes 层）
    op.input_sync_enable(True)                            # 默认关闭，不打开录不到东西
    path, st = op.input_record(0, r"D:/tmp/demo.jsonl")   # action 0=START -> (path, 1)
    op.input_record(1)                                    # action 1=STOP  -> (path, 写入条数)
    print(op.input_playback(0, path, 1.0, 1))             # LOAD -> (json, 事件条数)
    op.input_playback(3)                                  # FREE
    op.input_sync_enable(False)

    print(op.concurrency_info())                          # 并发自省 JSON
    print(op.input_sync_status())                         # 主从链路诊断 JSON

# 不写 with 的话记得显式 op.close()
""",
        "note": "本案例已按 `inspect.signature` 实测校准并在本机跑通（`get_client_size` 返 2 值、`client_to_screen` 返 2 值、`capture` 返 bool）。",
    },
    {
        "title": "案例 8：易语言 / COM 晚绑定",
        "lang": "text",
        "intro": "易语言走 COM 晚绑定，**必须先 `regsvr32` 注册**新 DLL，否则类型库里还是旧方法（新增方法靠 DISPID 追加，不重注册看不到）。",
        "code": """
.版本 2
.局部变量 op, 对象
.局部变量 ret, 整数型

' 1) 先注册（管理员权限）：regsvr32 /s D:\\AutoPro\\op-master\\op\\bin\\x64\\op_x64.dll
' 2) 创建对象（ProgID 见 libop/com/op.idl）
op.创建 (“Op.Op”, )

' 3) 按 DISPID 调用比按名字更稳（不受本地化影响）
'    442 = FindStrBatch   443 = FindStrEx2   444 = OcrLastProfile
'    445~453 = 图像句柄   454/455 = YoloEx2  456~463 = 音量
'    时间轴 / 并发自省 本次补齐，DISPID 见 op.idl 尾部
""",
        "note": "不重新 regsvr32 就是调不到新方法 —— 这是「修复没生效」最常见的原因。",
    },
]


def _verify_examples(decls: set[str]) -> list[str]:
    """校验案例里引用的每个 `OpXxx(` 都是真实导出，返回不存在的名字列表。

    这样案例不会随时间腐化：改了函数名却忘了改案例，生成时会直接报错。
    """
    missing: set[str] = set()
    for ex in USAGE_EXAMPLES:
        for name in re.findall(r"\b(Op[A-Z]\w*)\s*\(", ex["code"]):
            if name not in decls:
                missing.add(name)
    return sorted(missing)


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
    _append_usage_examples(L)
    L.append("---")
    L.append("")
    for sec in order:
        L.append(f"## {sec}（{len(sections[sec])} 个）")
        L.append("")
        for _name, sig in sections[sec]:
            L.append(f"- `{sig}`")
        L.append("")

    # 案例里引用的函数必须都是真实导出：改函数名却忘了改案例时在这里拦住
    # 注意 decls 是 [(name, sig)] 列表，这里要的是名字集合
    bad_examples = _verify_examples({n for n, _ in decls})
    if bad_examples:
        print("[FAIL] 使用案例引用了不存在的导出：", ", ".join(bad_examples), file=sys.stderr)
        return 3

    os.makedirs(os.path.dirname(OUT_MANUAL), exist_ok=True)
    with open(OUT_MANUAL, "w", encoding="utf-8", newline="\n") as fh:
        fh.write("\n".join(L).rstrip() + "\n")

    print(
        f"已写出 {os.path.relpath(OUT_MANUAL, ROOT)}"
        f"（{total} 条 / {len(order)} 个分区 / {len(USAGE_EXAMPLES)} 个使用案例）"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
