#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""dx_probe.py — DX 输入通道真机验证探针

用途
    DX（DirectInput / RawInput / 窗口消息）三通道属于后台输入注入功能，
    单元测试用的自建窗口不使用 DirectInput，绑定阶段必然失败（长期 SKIP），
    因此三通道只能靠真机目标程序（游戏）验证。本脚本把验证过程结构化：

    1) bind-matrix   遍历 mouse/keypad 的 dx 后缀组合，记录 BindWindow 返回值
    2) channel-matrix 对绑定成功的组合，逐个开关三通道，人工确认目标是否响应

依赖
    bindings/python 的 op 包（ctypes 封装，调 op_c_api_x64.dll）
    需要管理员权限（DX 输入通道要跨进程注入 hook）

用法
    python dx_probe.py --list
    python dx_probe.py --title 蜀门                 # 交互式完整验证
    python dx_probe.py --hwnd 123456 --no-click     # 不点击，只看绑定返回值
    python dx_probe.py --hwnd 123456 --quick        # 只跑 bind-matrix

输出
    控制台表格 + 结果文件 dx_probe_<时间戳>.md
"""

from __future__ import annotations

import argparse
import ctypes
import ctypes.wintypes as wt
import datetime as _dt
import os
import sys
import traceback

# ── 落盘兜底 ──────────────────────────────────────────────────────────
# 本脚本常被要求以管理员权限在独立控制台里跑；某些终端（ConPTY / 老版
# PowerShell / 被包装的 python）会把子进程 stdout 吞掉，表现为「零输出」。
# 因此这里把启动记录与控制台输出同时写文件，保证任何情况下都有可读证据。
_SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
_RUN_LOG = os.path.join(_SCRIPT_DIR, "dx_probe_run.log")
_CONSOLE_LOG = os.path.join(_SCRIPT_DIR, "dx_probe_console.log")


def _boot(msg: str) -> None:
    """把一行启动/阶段记录追加到 dx_probe_run.log（永不抛异常）。"""
    try:
        with open(_RUN_LOG, "a", encoding="utf-8") as f:
            f.write("[%s] %s\n" % (_dt.datetime.now().strftime("%Y-%m-%d %H:%M:%S"), msg))
    except Exception:
        pass


class _Tee:
    """stdout/stderr 双写：原流 + 日志文件，逐次 flush，避免缓冲丢内容。"""

    def __init__(self, stream, path: str):
        self._stream = stream
        self._f = None
        try:
            self._f = open(path, "a", encoding="utf-8")
        except Exception:
            self._f = None

    def write(self, s):
        try:
            self._stream.write(s)
        except Exception:
            pass
        if self._f is not None:
            try:
                self._f.write(s)
                self._f.flush()
            except Exception:
                pass
        return len(s)

    def flush(self):
        for o in (self._stream, self._f):
            if o is not None:
                try:
                    o.flush()
                except Exception:
                    pass

    def isatty(self):
        try:
            return self._stream.isatty()
        except Exception:
            return False

    def __getattr__(self, name):
        return getattr(self._stream, name)


# ── 通道掩码（与 libop DX_ATTR_* 对齐）────────────────────────────────
CH_DINPUT = 1  # DirectInput
CH_RAW = 2  # RawInput
CH_WINMSG = 4  # 窗口消息
CH_ALL = 7

# 绑定组合：mouse 用 dx 后缀，keypad 跟随（后缀只对键鼠有效）
MOUSE_MODES = ["dx", "dx.dinput", "dx.raw", "dx.win", "dx.dinput+raw"]
KEYPAD_MODES = ["windows", "dx"]

user32 = ctypes.WinDLL("user32", use_last_error=True)


def is_admin() -> bool:
    try:
        return bool(ctypes.windll.shell32.IsUserAnAdmin())
    except Exception:
        return False


def list_windows(keyword: str = "") -> list[tuple[int, str, str]]:
    """枚举可见顶层窗口 -> (hwnd, title, class)"""
    out: list[tuple[int, str, str]] = []
    EnumWindowsProc = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)

    def _cb(hwnd, _lp):
        if not user32.IsWindowVisible(hwnd):
            return True
        n = user32.GetWindowTextLengthW(hwnd)
        if n <= 0:
            return True
        buf = ctypes.create_unicode_buffer(n + 1)
        user32.GetWindowTextW(hwnd, buf, n + 1)
        title = buf.value
        cbuf = ctypes.create_unicode_buffer(256)
        user32.GetClassNameW(hwnd, cbuf, 256)
        if not keyword or keyword.lower() in title.lower():
            out.append((int(hwnd), title, cbuf.value))
        return True

    user32.EnumWindows(EnumWindowsProc(_cb), 0)
    return out


def emit_diag(args) -> None:
    """环境体检：只读检查，不绑定、不注入、不发送任何输入。"""
    def P(s: str = "") -> None:
        print(s)

    P("=== DX 探针环境体检 ===")
    P("python      : %s" % sys.executable)
    P("version     : %s" % sys.version.replace("\n", " "))
    P("cwd         : %s" % os.getcwd())
    P("script      : %s" % os.path.abspath(__file__))
    P("admin       : %s" % ("是" if is_admin() else "否（DX 注入会失败，请以管理员重开终端）"))
    P("stdout isatty: %s" % (sys.stdout.isatty() if hasattr(sys.stdout, "isatty") else "?"))

    root = os.path.dirname(_SCRIPT_DIR)
    dll_dir = args.dll_dir or os.path.join(root, "bin", "x64")
    P("dll_dir     : %s  (exists=%s)" % (dll_dir, os.path.isdir(dll_dir)))
    for n in ("op_c_api_x64.dll", "op_x64.dll"):
        fp = os.path.join(dll_dir, n)
        ok = os.path.exists(fp)
        P("  %-18s exists=%-5s %s" % (n, ok,
                                      ("%.1f MB" % (os.path.getsize(fp) / 1048576.0)) if ok else ""))

    try:
        os.add_dll_directory(dll_dir)
    except Exception as e:  # noqa: BLE001
        P("add_dll_directory 失败: %s" % e)
    sys.path.insert(0, os.path.join(root, "bindings", "python"))
    try:
        from op import Op  # type: ignore
    except Exception as e:  # noqa: BLE001
        P("import op   : FAIL  %s: %s" % (type(e).__name__, e))
        return

    try:
        op = Op(dll_dir=dll_dir, raise_on_error=False)
    except Exception as e:  # noqa: BLE001
        P("Op()        : FAIL  %s: %s" % (type(e).__name__, e))
        return
    try:
        P("import op   : OK")
        P("OP version  : %s" % op.version)
        try:
            P("GetDxAttr   : %d" % op.get_dx_attr())
        except Exception as e:  # noqa: BLE001
            P("GetDxAttr   : ERR %s" % e)
    finally:
        try:
            op.close()
        except Exception:
            pass
    P()
    P("体检通过。下一步：--list 找窗口，再 --hwnd <数字> --quick --no-click 试绑定。")


def build_argparser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(description="DX 输入通道真机验证探针")
    p.add_argument("--hwnd", type=lambda s: int(s, 0), help="目标窗口句柄（十进制或 0x 十六进制）")
    p.add_argument("--title", help="按标题关键字查找目标窗口（取第一个匹配）")
    p.add_argument("--list", action="store_true", help="列出可见顶层窗口后退出")
    p.add_argument("--diag", action="store_true",
                   help="环境体检：解释器 / DLL / 绑定导入 / 管理员权限，不触碰目标窗口")
    p.add_argument("--quick", action="store_true", help="只跑 bind-matrix（不交互）")
    p.add_argument("--no-click", action="store_true", help="不发送点击/按键，只测绑定与通道开关")
    p.add_argument("--dll-dir", default=None, help="op_c_api_x64.dll 所在目录")
    p.add_argument("--out", default=None, help="结果文件路径")
    return p


def main() -> int:
    args = build_argparser().parse_args()

    if args.diag:
        _boot("--diag 开始")
        emit_diag(args)
        _boot("--diag 结束")
        return 0

    if args.list:
        wins = list_windows()
        _boot("--list: 枚举到 %d 个可见顶层窗口" % len(wins))
        print("%-12s %-30s %s" % ("HWND", "CLASS", "TITLE"))
        for h, t, c in sorted(wins, key=lambda x: x[2]):
            print("%-12d %-30s %s" % (h, c[:30], t[:70]))
        print("\n共 %d 个可见顶层窗口" % len(wins))
        return 0

    # 解析目标窗口
    hwnd = args.hwnd or 0
    if not hwnd and args.title:
        cands = list_windows(args.title)
        if not cands:
            print("[FAIL] 未找到标题包含 %r 的窗口，用 --list 查看" % args.title)
            return 2
        if len(cands) > 1:
            print("[WARN] 匹配到 %d 个窗口，取第一个；建议直接用 --hwnd 指定：" % len(cands))
            for h, t, c in cands:
                print("        %d  %s  [%s]" % (h, t[:50], c[:24]))
        hwnd = cands[0][0]
    if not hwnd:
        print("[FAIL] 需要 --hwnd 或 --title（或用 --list 查看候选）")
        return 2

    # 加载 op 绑定
    dll_dir = args.dll_dir or os.path.join(
        os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "bin", "x64")
    if os.path.isdir(dll_dir):
        try:
            os.add_dll_directory(dll_dir)
        except Exception:
            pass
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                                    "bindings", "python"))
    try:
        from op import Op  # type: ignore
    except Exception as e:  # noqa: BLE001
        print("[FAIL] 无法导入 op 绑定:", type(e).__name__, e)
        print("       若 DLL 不在默认位置，用 --dll-dir 指定")
        return 3

    lines: list[str] = []
    ts = _dt.datetime.now().strftime("%Y%m%d_%H%M%S")
    out_path = args.out or os.path.join(os.getcwd(), "dx_probe_%s.md" % ts)

    def emit(s: str = "") -> None:
        print(s)
        lines.append(s)

    emit("# DX 输入通道真机验证结果")
    emit()
    emit("- 时间：%s" % _dt.datetime.now().strftime("%Y-%m-%d %H:%M:%S"))
    emit("- 目标 hwnd：%d" % hwnd)
    emit("- 管理员权限：**%s**" % ("是" if is_admin() else "否（DX 注入大概率失败）"))
    emit("- 发送点击/按键：%s" % ("否(--no-click)" if args.no_click else "是"))

    op = Op(dll_dir=dll_dir, raise_on_error=False)
    try:
        emit("- OP 版本：%s" % op.version)
        try:
            emit("- 窗口标题：%s" % op.get_window_title(hwnd))
            emit("- 窗口类名：%s" % op.get_window_class(hwnd))
            emit("- 进程 PID：%d" % op.get_window_process_id(hwnd))
            cw, ch = op.get_client_size(hwnd)
            emit("- 客户区尺寸：%dx%d" % (cw, ch))
        except Exception as e:  # noqa: BLE001
            emit("- 窗口信息读取失败：%s" % e)
            cw, ch = 640, 480
        emit("- 绑定前 GetDxAttr：%d" % op.get_dx_attr())
        emit()

        # ── 第一段：bind-matrix ────────────────────────────────────
        emit("## 1. 绑定矩阵（mouse × keypad）")
        emit()
        emit("| mouse | keypad | BindWindow | GetDxAttr | MoveTo | LeftClick | KeyPress(F1) |")
        emit("|---|---|---|---|---|---|---|")
        bind_ok_combo: list[tuple[str, str]] = []
        for m in MOUSE_MODES:
            for k in KEYPAD_MODES:
                row = [m, k]
                try:
                    op.unbind_window()
                except Exception:
                    pass
                try:
                    ok = op.bind_window(hwnd, "normal", m, k, 0)
                except Exception as e:  # noqa: BLE001
                    ok = False
                    row_note = str(e)[:40]
                else:
                    row_note = ""
                row.append("1" if ok else ("0 %s" % row_note if row_note else "0"))
                if ok:
                    bind_ok_combo.append((m, k))
                    try:
                        row.append(str(op.get_dx_attr()))
                    except Exception:  # noqa: BLE001
                        row.append("ERR")
                    if not args.no_click:
                        cx, cy = (cw // 2, ch // 2) if cw and ch else (100, 100)
                        try:
                            op.move_to(cx, cy)
                            row.append("1")
                        except Exception:  # noqa: BLE001
                            row.append("0")
                        try:
                            op.left_click()
                            row.append("1")
                        except Exception:  # noqa: BLE001
                            row.append("0")
                        try:
                            op.key_press(0x70)  # VK_F1
                            row.append("1")
                        except Exception:  # noqa: BLE001
                            row.append("0")
                    else:
                        row += ["-", "-", "-"]
                else:
                    row += ["-", "-", "-", "-"]
                emit("| " + " | ".join(row) + " |")
        emit()
        emit("绑定成功组合数：**%d / %d**" % (len(bind_ok_combo), len(MOUSE_MODES) * len(KEYPAD_MODES)))
        emit()

        if not bind_ok_combo:
            emit("> 全部失败。请确认：① 管理员权限；② 目标为 64 位进程；")
            emit("> ③ 目标进程未被反作弊/保护拒绝注入；④ Hook DLL 与 op_x64.dll 版本一致。")
            emit("> 详细原因见 op SetLog 输出（`BindWindowEx failed. display=.. ret=.. mouse=.. ret=..`）。")

        # ── 第二段：channel-matrix ────────────────────────────────
        if bind_ok_combo and not args.quick:
            m, k = bind_ok_combo[0]
            emit("## 2. 通道开关矩阵（组合 %s / %s）" % (m, k))
            emit()
            emit("> 每步脚本会发送一次 MoveTo + LeftClick，请观察目标程序是否响应，然后按提示作答。")
            emit()
            emit("| 步骤 | SetDxAttr | 生效 GetDxAttr | 目标有反应 |")
            emit("|---|---|---|---|")
            try:
                op.unbind_window()
            except Exception:
                pass
            if op.bind_window(hwnd, "normal", m, k, 0):
                steps = [
                    ("全开（默认）", None),
                    ("仅关窗口消息", (CH_WINMSG, 0)),
                    ("恢复全开", (0, CH_ALL)),
                    ("仅关 RawInput", (CH_RAW, 0)),
                    ("恢复全开", (0, CH_ALL)),
                    ("仅关 DirectInput", (CH_DINPUT, 0)),
                    ("恢复全开", (0, CH_ALL)),
                ]
                for label, attr in steps:
                    cur = op.get_dx_attr()
                    if attr is not None:
                        try:
                            op.set_dx_attr(attr[0], attr[1])
                        except Exception as e:  # noqa: BLE001
                            emit("| %s | %s | ERR %s | - |" % (label, attr, e))
                            continue
                        cur = op.get_dx_attr()
                    if not args.no_click:
                        try:
                            op.move_to(cw // 2, ch // 2)
                            op.left_click()
                        except Exception:
                            pass
                        try:
                            ans = input("   [%s] 目标有反应吗? (y/n/s=跳过): " % label).strip().lower()
                        except EOFError:
                            ans = "s"
                        verdict = {"y": "**有**", "n": "无（该通道是主驱动）"}.get(ans, "跳过")
                    else:
                        verdict = "-"
                    emit("| %s | %s | %d | %s |" % (label, attr if attr else "未改动", cur, verdict))
                try:
                    op.set_dx_attr(0, CH_ALL)
                except Exception:
                    pass
                try:
                    op.unbind_window()
                except Exception:
                    pass
    finally:
        try:
            op.close()
        except Exception:
            pass

    emit()
    emit("---")
    emit("说明：通道开关验证用于判断目标程序实际依赖哪条 dx 通道。")
    emit("关闭某通道后若目标立即失去响应，说明该通道是主驱动通路。")

    try:
        with open(out_path, "w", encoding="utf-8") as f:
            f.write("\n".join(lines))
        print("\n[OK] 结果已写入 %s" % out_path)
    except Exception as e:  # noqa: BLE001
        print("\n[WARN] 结果写入失败:", e)
    return 0


if __name__ == "__main__":
    _boot("=== 启动 === argv=%r cwd=%r python=%s"
          % (sys.argv, os.getcwd(), sys.version.replace("\n", " ")))
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
        sys.stderr.reconfigure(encoding="utf-8", errors="replace")
    except Exception:
        pass
    try:
        sys.stdout = _Tee(sys.stdout, _CONSOLE_LOG)
        sys.stderr = _Tee(sys.stderr, _CONSOLE_LOG)
    except Exception:
        pass
    _boot("控制台输出副本：%s" % _CONSOLE_LOG)
    try:
        _rc = main()
    except SystemExit:
        raise
    except BaseException:
        _boot("!!! 未捕获异常：\n" + traceback.format_exc())
        traceback.print_exc()
        raise SystemExit(9)
    _boot("=== 正常退出 rc=%d ===" % _rc)
    raise SystemExit(_rc)
