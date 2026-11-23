from __future__ import annotations

import ctypes
import locale
from enum import Enum
from pathlib import Path
from typing import Any

from . import constants
from ._ffi import NativeApi, load_dll
from .errors import ClosedHandleError, OpCallError
from .types import IntType, StringType


class Op:
    def __init__(
        self,
        dll_path: str | Path | None = None,
        dll_dir: str | Path | None = None,
        raise_on_error: bool = True,
    ) -> None:
        self._api: NativeApi = load_dll(dll_path=dll_path, dll_dir=dll_dir)
        self._dll = self._api.dll
        self._raise_on_error = raise_on_error
        self._handle = self._dll.OpCreate()
        if not self._handle:
            raise OpCallError("OpCreate")

    @property
    def dll_path(self) -> Path | None:
        return self._api.path

    @property
    def handle(self) -> int:
        self._check_open()
        return int(self._handle)

    @property
    def version(self) -> str:
        return self._dll.OpVer() or ""

    def close(self) -> None:
        if self._handle:
            self._dll.OpDestroy(self._handle)
            self._handle = None

    def __enter__(self) -> "Op":
        self._check_open()
        return self

    def __exit__(self, exc_type: object, exc: object, tb: object) -> None:
        self.close()

    def __del__(self) -> None:
        try:
            self.close()
        except Exception:
            pass

    def _check_open(self) -> None:
        if not self._handle:
            raise ClosedHandleError("Op handle is closed")

    def _ok(self, value: int, name: str) -> bool:
        if value:
            return True
        if self._raise_on_error:
            raise OpCallError(name)
        return False

    def _call_ok(self, name: str, *args: Any) -> bool:
        self._check_open()
        return self._ok(getattr(self._dll, name)(self._handle, *args), name)

    def _call_int(self, name: str, *args: Any) -> int:
        self._check_open()
        return int(getattr(self._dll, name)(self._handle, *args))

    def _call_intptr(self, name: str, *args: Any) -> int:
        self._check_open()
        return int(getattr(self._dll, name)(self._handle, *args))

    def _call_string(self, name: str, *args: Any) -> str:
        self._check_open()
        value = getattr(self._dll, name)(self._handle, *args)
        return value or ""

    def _call_string_with_int(self, name: str, *args: Any) -> tuple[str, int]:
        self._check_open()
        ret = ctypes.c_int()
        value = getattr(self._dll, name)(self._handle, *args, ctypes.byref(ret))
        return value or "", int(ret.value)

    @staticmethod
    def _enum_text(value: str | Enum) -> str:
        return str(value.value) if isinstance(value, Enum) else str(value)

    def _call_checked_point(self, name: str, *args: Any) -> tuple[int, int]:
        x = ctypes.c_int()
        y = ctypes.c_int()
        ok = getattr(self._dll, name)(self._handle, *args, ctypes.byref(x), ctypes.byref(y))
        self._ok(ok, name)
        return int(x.value), int(y.value)

    def _call_checked_inout_point(self, name: str, hwnd: int, x: int, y: int) -> tuple[int, int]:
        cx = ctypes.c_int(int(x))
        cy = ctypes.c_int(int(y))
        ok = getattr(self._dll, name)(self._handle, int(hwnd), ctypes.byref(cx), ctypes.byref(cy))
        self._ok(ok, name)
        return int(cx.value), int(cy.value)

    def _call_result_point(self, name: str, *args: Any) -> tuple[int, int, int]:
        x = ctypes.c_int()
        y = ctypes.c_int()
        ret = getattr(self._dll, name)(self._handle, *args, ctypes.byref(x), ctypes.byref(y))
        return int(ret), int(x.value), int(y.value)

    def _call_checked_rect(self, name: str, *args: Any) -> tuple[int, int, int, int]:
        x1 = ctypes.c_int()
        y1 = ctypes.c_int()
        x2 = ctypes.c_int()
        y2 = ctypes.c_int()
        ok = getattr(self._dll, name)(
            self._handle,
            *args,
            ctypes.byref(x1),
            ctypes.byref(y1),
            ctypes.byref(x2),
            ctypes.byref(y2),
        )
        self._ok(ok, name)
        return int(x1.value), int(y1.value), int(x2.value), int(y2.value)

    def _call_checked_size(self, name: str, *args: Any) -> tuple[int, int]:
        width = ctypes.c_int()
        height = ctypes.c_int()
        ok = getattr(self._dll, name)(self._handle, *args, ctypes.byref(width), ctypes.byref(height))
        self._ok(ok, name)
        return int(width.value), int(height.value)

    # Basic
    def set_path(self, path: str | Path) -> bool:
        return self._call_ok("OpSetPath", str(path))

    def get_path(self) -> str:
        return self._call_string("OpGetPath")

    def get_base_path(self) -> str:
        return self._call_string("OpGetBasePath")

    def get_id(self) -> int:
        return self._call_int("OpGetID")

    def get_last_error(self) -> int:
        return self._call_int("OpGetLastError")

    def set_show_error_msg(self, show_type: constants.ShowError | int) -> bool:
        return self._call_ok("OpSetShowErrorMsg", int(show_type))

    def sleep(self, milliseconds: int) -> bool:
        return self._call_ok("OpSleep", int(milliseconds))

    def inject_dll(self, process_name: str, dll_name: str) -> bool:
        return self._call_ok("OpInjectDll", process_name, dll_name)

    def enable_pic_cache(self, enable: bool | int) -> bool:
        return self._call_ok("OpEnablePicCache", int(enable))

    def clear_pic_cache(self) -> bool:
        """清空进程级图片缓存（FindPic 自动缓存与显式加载共用）。"""
        return self._call_ok("OpClearPicCache")

    def set_pic_cache_max(self, max_count: int) -> bool:
        """设置进程级图片缓存条目上限；<=0 表示不设上限，默认 500，满时整体清空重存。"""
        return self._call_ok("OpSetPicCacheMax", int(max_count))

    def capture_pre(self, file_name: str | Path) -> bool:
        return self._call_ok("OpCapturePre", str(file_name))

    def set_screen_data_mode(self, mode: constants.ScreenDataMode | int) -> bool:
        return self._call_ok("OpSetScreenDataMode", int(mode))

    # Algorithm
    def a_star_find_path(
        self,
        map_width: int,
        map_height: int,
        disable_points: str,
        begin_x: int,
        begin_y: int,
        end_x: int,
        end_y: int,
    ) -> str:
        return self._call_string(
            "OpAStarFindPath",
            int(map_width),
            int(map_height),
            disable_points,
            int(begin_x),
            int(begin_y),
            int(end_x),
            int(end_y),
        )

    def find_nearest_pos(self, all_pos: str, pos_type: constants.NearestPosType | int, x: int, y: int) -> str:
        return self._call_string("OpFindNearestPos", all_pos, int(pos_type), int(x), int(y))

    def sort_pos_distance(self, all_pos: str, pos_type: constants.NearestPosType | int, x: int, y: int) -> str:
        """按与 (x,y) 的距离排序坐标串："x,y|x,y|..."，type 0=近→远 1=远→近。"""
        return self._call_string("OpSortPosDistance", all_pos, int(pos_type), int(x), int(y))

    def exclude_pos(self, all_pos: str, pos_type: constants.NearestPosType | int, exclude_rects: str) -> tuple[str, int]:
        """剔除落在矩形内的坐标点。exclude_rects="x1,y1,x2,y2|..."（含边界，顺序不限）。
        返回 (过滤后坐标串, 保留点数)；exclude_rects 为空时原样返回。"""
        return self._call_string_with_int("OpExcludePos", all_pos, int(pos_type), exclude_rects)

    def get_dense_rect(self, all_pos: str, pos_type: constants.NearestPosType | int, width: int, height: int) -> tuple[str, int]:
        """找包含坐标点最多的 width×height 窗口（右/下边界开区间）。返回 ("x,y,count", count)。"""
        return self._call_string_with_int("OpGetDenseRect", all_pos, int(pos_type), int(width), int(height))

    def find_nearest_feasible_point(self, x: int, y: int) -> str:
        """障碍地图上找最近可走点（已设图为前提），返回 "x,y"，找不到返回空串。"""
        return self._call_string("OpFindNearestFeasiblePoint", int(x), int(y))

    def set_astar_map(self, bitmap_file: str | Path, scale: int, offset_x: int, offset_y: int) -> bool:
        """设置寻路障碍地图（位图文件，黑=障碍）。scale 降采样倍率，offset 为位图原点世界坐标；空文件名清空。"""
        return self._call_ok("OpSetAStarMap", str(bitmap_file), int(scale), int(offset_x), int(offset_y))

    def set_astar_map_data(
        self, width: int, height: int, bgra_data, scale: int, offset_x: int, offset_y: int
    ) -> bool:
        """内存版障碍地图：32bpp BGRA top-down 字节缓冲（ctypes/bytes 均可），大小自动取缓冲长度。"""
        size = len(bgra_data) if hasattr(bgra_data, "__len__") else ctypes.sizeof(bgra_data)
        return self._call_ok(
            "OpSetAStarMapData", int(width), int(height), bgra_data, int(size), int(scale), int(offset_x),
            int(offset_y)
        )

    def a_star_find_path_bm(self, begin_x: int, begin_y: int, end_x: int, end_y: int) -> str:
        """障碍位图 A*：对已设置地图寻路，返回世界坐标路径，不可达/未设图返回空串。"""
        return self._call_string("OpAStarFindPathBM", int(begin_x), int(begin_y), int(end_x), int(end_y))

    def a_star_find_path_way(self, points: str) -> str:
        """多点途经寻路："x,y|x,y|..." 首点为起点，任一段不可达返回空串。"""
        return self._call_string("OpAStarFindPathWay", points)

    def smooth_path_by_los(self, path: str) -> str:
        """视线拉直（需已设置障碍地图），未设图原样返回。"""
        return self._call_string("OpSmoothPathByLOS", path)

    def simplify_path(self, path: str, epsilon: float) -> str:
        """RDP 路径抽稀，epsilon 为最大垂直偏差（世界坐标单位）。"""
        return self._call_string("OpSimplifyPath", path, float(epsilon))

    def is_line_blocked(self, x1: int, y1: int, x2: int, y2: int) -> int:
        """两点视线遮挡判定：1=被挡 0=通 -1=未设置地图。"""
        return self._call_int("OpIsLineBlocked", int(x1), int(y1), int(x2), int(y2))

    def find_nearest_path_point(self, path: str, x: int, y: int) -> tuple[int, int, int] | None:
        """点到路径最近点，返回 (index, nx, ny)；path 非法返回 None。"""
        index = ctypes.c_int(0)
        nx = ctypes.c_int(0)
        ny = ctypes.c_int(0)
        ret = self._dll.OpFindNearestPathPoint(self._handle, path, int(x), int(y), ctypes.byref(index),
                                               ctypes.byref(nx), ctypes.byref(ny))
        self._ok(ret, "OpFindNearestPathPoint")
        return (index.value, nx.value, ny.value) if ret == 1 else None

    def point_in_polygon(self, point: str, polygon: str) -> bool:
        """点在多边形内判定（射线法）：point="x,y"，polygon="x,y|x,y|..."（>=3 顶点）。"""
        return self._call_ok("OpPointInPolygon", point, polygon)

    # Window and process
    def enum_window(
        self,
        parent: int = 0,
        title: str = "",
        class_name: str = "",
        filter: constants.WindowEnumFilter | int = constants.WindowEnumFilter.ALL,
    ) -> str:
        return self._call_string("OpEnumWindow", int(parent), title, class_name, int(filter))

    def enum_window_by_process(
        self,
        process_name: str,
        title: str = "",
        class_name: str = "",
        filter: constants.WindowEnumFilter | int = constants.WindowEnumFilter.ALL,
    ) -> str:
        return self._call_string("OpEnumWindowByProcess", process_name, title, class_name, int(filter))

    def enum_process(self, name: str = "") -> str:
        return self._call_string("OpEnumProcess", name)

    def client_to_screen(self, hwnd: int, x: int, y: int) -> tuple[int, int]:
        return self._call_checked_inout_point("OpClientToScreen", hwnd, x, y)

    def find_window(self, class_name: str = "", title: str = "") -> int:
        return self._call_intptr("OpFindWindow", class_name, title)

    def find_window_by_process(self, process_name: str, class_name: str = "", title: str = "") -> int:
        return self._call_intptr("OpFindWindowByProcess", process_name, class_name, title)

    def find_window_by_process_id(self, process_id: int, class_name: str = "", title: str = "") -> int:
        return self._call_intptr("OpFindWindowByProcessId", int(process_id), class_name, title)

    def find_window_ex(self, parent: int, class_name: str = "", title: str = "") -> int:
        return self._call_intptr("OpFindWindowEx", int(parent), class_name, title)

    def get_client_rect(self, hwnd: int) -> tuple[int, int, int, int]:
        return self._call_checked_rect("OpGetClientRect", int(hwnd))

    def get_client_size(self, hwnd: int) -> tuple[int, int]:
        return self._call_checked_size("OpGetClientSize", int(hwnd))

    def get_foreground_focus(self) -> int:
        return self._call_intptr("OpGetForegroundFocus")

    def get_foreground_window(self) -> int:
        return self._call_intptr("OpGetForegroundWindow")

    def get_mouse_point_window(self) -> int:
        return self._call_intptr("OpGetMousePointWindow")

    def get_point_window(self, x: int, y: int) -> int:
        return self._call_intptr("OpGetPointWindow", int(x), int(y))

    def get_process_info(self, pid: int) -> str:
        return self._call_string("OpGetProcessInfo", int(pid))

    def get_special_window(self, flag: constants.SpecialWindow | int) -> int:
        return self._call_intptr("OpGetSpecialWindow", int(flag))

    def get_window(self, hwnd: int, flag: constants.WindowRelation | int) -> int:
        return self._call_intptr("OpGetWindow", int(hwnd), int(flag))

    def get_window_class(self, hwnd: int) -> str:
        return self._call_string("OpGetWindowClass", int(hwnd))

    def get_window_process_id(self, hwnd: int) -> int:
        return self._call_int("OpGetWindowProcessId", int(hwnd))

    def get_window_process_path(self, hwnd: int) -> str:
        return self._call_string("OpGetWindowProcessPath", int(hwnd))

    def get_window_rect(self, hwnd: int) -> tuple[int, int, int, int]:
        return self._call_checked_rect("OpGetWindowRect", int(hwnd))

    def get_window_state(self, hwnd: int, flag: constants.WindowState | int) -> int:
        return self._call_int("OpGetWindowState", int(hwnd), int(flag))

    def get_window_title(self, hwnd: int) -> str:
        return self._call_string("OpGetWindowTitle", int(hwnd))

    def move_window(self, hwnd: int, x: int, y: int) -> bool:
        return self._call_ok("OpMoveWindow", int(hwnd), int(x), int(y))

    def screen_to_client(self, hwnd: int, x: int, y: int) -> tuple[int, int]:
        return self._call_checked_inout_point("OpScreenToClient", hwnd, x, y)

    def send_paste(self, hwnd: int) -> bool:
        return self._call_ok("OpSendPaste", int(hwnd))

    def set_client_size(self, hwnd: int, width: int, height: int) -> bool:
        return self._call_ok("OpSetClientSize", int(hwnd), int(width), int(height))

    def set_window_state(self, hwnd: int, flag: constants.WindowStateAction | int) -> bool:
        return self._call_ok("OpSetWindowState", int(hwnd), int(flag))

    def set_window_size(self, hwnd: int, width: int, height: int) -> bool:
        return self._call_ok("OpSetWindowSize", int(hwnd), int(width), int(height))

    def layout_windows(
        self,
        hwnds: str,
        layout_type: constants.LayoutType | int,
        columns: int,
        start_x: int,
        start_y: int,
        gap_x: int,
        gap_y: int,
        size_mode: constants.LayoutSize | int,
        window_width: int,
        window_height: int,
        anchor_mode: constants.LayoutAnchor | int,
    ) -> bool:
        return self._call_ok(
            "OpLayoutWindows",
            hwnds,
            int(layout_type),
            int(columns),
            int(start_x),
            int(start_y),
            int(gap_x),
            int(gap_y),
            int(size_mode),
            int(window_width),
            int(window_height),
            int(anchor_mode),
        )

    def set_window_text(self, hwnd: int, title: str) -> bool:
        return self._call_ok("OpSetWindowText", int(hwnd), title)

    def set_window_transparent(self, hwnd: int, trans: int) -> bool:
        return self._call_ok("OpSetWindowTransparent", int(hwnd), int(trans))

    def send_string(self, hwnd: int, value: str) -> bool:
        return self._call_ok("OpSendString", int(hwnd), value)

    def send_string_ime(self, hwnd: int, value: str) -> bool:
        return self._call_ok("OpSendStringIme", int(hwnd), value)

    def run_app(self, cmdline: str, mode: int = 1) -> int:
        self._check_open()
        pid = ctypes.c_uint32()
        ok = self._dll.OpRunApp(self._handle, cmdline, int(mode), ctypes.byref(pid))
        self._ok(ok, "OpRunApp")
        return int(pid.value)

    def win_exec(self, cmdline: str, cmdshow: int = 1) -> bool:
        return self._call_ok("OpWinExec", cmdline, int(cmdshow))

    def get_cmd_str(self, cmd: str, milliseconds: int) -> str:
        return self._call_string("OpGetCmdStr", cmd, int(milliseconds))

    def set_clipboard(self, value: str) -> bool:
        return self._call_ok("OpSetClipboard", value)

    def get_clipboard(self) -> str:
        return self._call_string("OpGetClipboard")

    def delay(self, milliseconds: int) -> bool:
        return self._call_ok("OpDelay", int(milliseconds))

    def delays(self, min_milliseconds: int, max_milliseconds: int) -> bool:
        return self._call_ok("OpDelays", int(min_milliseconds), int(max_milliseconds))

    def get_window_scale(self, hwnd: int) -> str:
        """窗口所在显示器缩放比（两位小数字符串，如 "1.50"）；失败返回空串。
        多屏混合 DPI 时与 get_dpi 的系统级值可不同。"""
        return self._call_string("OpGetWindowScale", int(hwnd))

    def terminate_process(self, pid: int) -> bool:
        """按 PID 终止进程。"""
        return self._call_ok("OpTerminateProcess", int(pid))

    def terminate_process_tree(self, pid: int) -> bool:
        """终止进程及其整棵子进程树。"""
        return self._call_ok("OpTerminateProcessTree", int(pid))

    # System misc
    def get_screen_width(self) -> int:
        return self._call_int("OpGetScreenWidth")

    def get_screen_height(self) -> int:
        return self._call_int("OpGetScreenHeight")

    def get_screen_depth(self) -> int:
        return self._call_int("OpGetScreenDepth")

    def get_dpi(self) -> int:
        return self._call_int("OpGetDPI")

    def get_time(self) -> str:
        return self._call_string("OpGetTime")

    def beep(self, freq: int, duration_ms: int) -> bool:
        return self._call_ok("OpBeep", int(freq), int(duration_ms))

    def get_random_number(self, min_value: int, max_value: int) -> int:
        return self._call_int("OpGetRandomNumber", int(min_value), int(max_value))

    def get_random_double(self, min_value: float, max_value: float) -> float:
        self._check_open()
        return float(self._dll.OpGetRandomDouble(self._handle, float(min_value), float(max_value)))

    def gai_lu(self, p: int) -> bool:
        """以 1/p 的概率返回 True（拟人化概率判定）。p<=0 恒 False，p==1 恒 True。"""
        return self._call_int("OpGaiLu", int(p)) == 1

    def get_machine_code(self) -> str:
        return self._call_string("OpGetMachineCode")

    def is_elevated(self) -> bool:
        return self._call_int("OpIsElevated") == 1

    def lock_window_position(self, hwnd: int, enable: bool) -> bool:
        """锁定/解锁窗口位置：锁定后外部移动该窗口会被守护线程自动拉回。"""
        return self._call_ok("OpLockWindowPosition", int(hwnd), 1 if enable else 0)

    def lock_window_size(self, hwnd: int, enable: bool) -> bool:
        """锁定/解锁窗口尺寸：锁定后外部缩放该窗口会被自动拉回。"""
        return self._call_ok("OpLockWindowSize", int(hwnd), 1 if enable else 0)

    def disable_min_max(self, hwnd: int, enable: bool) -> bool:
        """禁止/恢复窗口的最大最小化按钮（恢复时还原锁定前原始样式）。"""
        return self._call_ok("OpDisableMinMax", int(hwnd), 1 if enable else 0)

    def set_ime(self, hwnd: int, enable: bool) -> bool:
        """设置指定窗口的输入法开关：enable=False 关闭输入，True 恢复。"""
        return self._call_ok("OpSetIme", int(hwnd), 1 if enable else 0)

    def get_fps(self) -> int:
        """获取绑定窗口当前帧率（内部采样约 1 秒）。未绑定/后端无帧返回 0。"""
        return self._call_int("OpGetFPS")

    def is_display_dead(
        self, x1: int, y1: int, x2: int, y2: int, check_times: int = 3, interval_ms: int = 500
    ) -> int:
        """卡屏检测：区域连续 check_times 次截图（间隔 interval_ms）帧哈希全同=卡死返回 1，
        有变化/截图失败返回 0。check_times<=0 默认 3（钳 2-20），interval_ms<=0 默认 500（钳 50-10000）。"""
        return self._call_int("OpIsDisplayDead", int(x1), int(y1), int(x2), int(y2), int(check_times),
                              int(interval_ms))

    def enable_debug_privilege(self) -> bool:
        """为当前进程启用 SeDebugPrivilege（提权读内存/开进程前置）。"""
        return self._call_ok("OpEnableDebugPrivilege")

    def get_last_error_string(self) -> str:
        """最近一次失败的详细错误信息文本。"""
        return self._call_string("OpGetLastErrorString")

    def md5(self, text: str) -> str:
        """计算文本的 MD5 摘要（32 位小写十六进制）。"""
        return self._call_string("OpMd5", text)

    def sha1(self, text: str) -> str:
        """计算文本的 SHA-1 摘要（40 位小写十六进制）。"""
        return self._call_string("OpSha1", text)

    def base64_encode(self, text: str) -> str:
        """文本 Base64 编码。"""
        return self._call_string("OpBase64Encode", text)

    def base64_decode(self, base64: str) -> str:
        """Base64 文本解码；输入非法返回空串。"""
        return self._call_string("OpBase64Decode", base64)

    def sha256(self, text: str) -> str:
        """计算文本的 SHA-256 摘要（64 位小写十六进制）。"""
        return self._call_string("OpSha256", text)

    def hmac_sha256(self, key: str, text: str) -> str:
        """计算 HMAC-SHA256（64 位小写十六进制）；key 为空返回空串。"""
        return self._call_string("OpHmacSha256", key, text)

    def generate_random_bytes(self, length: int) -> str:
        """生成 length 字节密码学随机数，输出 2*length 位小写十六进制；length∈(0,4096]。"""
        return self._call_string("OpGenerateRandomBytes", int(length))

    def generate_guid(self) -> str:
        """生成大写 GUID 文本（8-4-4-4-12，无花括号）。"""
        return self._call_string("OpGenerateGuid")

    # Crypto 补全（2026-11-23）：流式文件哈希 / AES-256-GCM / PBKDF2 / RSA
    def md5_file(self, file_name: str | Path) -> str:
        """流式计算文件 MD5（32 位小写十六进制）；文件不存在或读失败返回空串。"""
        return self._call_string("OpMd5File", str(file_name))

    def sha1_file(self, file_name: str | Path) -> str:
        """流式计算文件 SHA-1（40 位小写十六进制）；文件不存在返回空串。"""
        return self._call_string("OpSha1File", str(file_name))

    def sha256_file(self, file_name: str | Path) -> str:
        """流式计算文件 SHA-256（64 位小写十六进制）；文件不存在返回空串。"""
        return self._call_string("OpSha256File", str(file_name))

    def aes_encrypt(self, text: str, password: str) -> str:
        """AES-256-GCM 加密并输出 Base64（容器头 OPAES01，PBKDF2-SHA256 10 万轮衍生密钥）。

        每次调用随机 salt/nonce，同一明文两次密文不同；明文或密码为空返回空串。
        """
        return self._call_string("OpAesEncrypt", text, password)

    def aes_decrypt(self, base64: str, password: str) -> str:
        """AES-256-GCM 解密；密码错 / 密文被改 / 非本库密文一律返回空串（不会吐乱码）。"""
        return self._call_string("OpAesDecrypt", base64, password)

    def pbkdf2_sha256(self, password: str, salt: str, iterations: int, length: int) -> str:
        """PBKDF2-HMAC-SHA256 衍生密钥，输出 2*length 位小写十六进制。

        iterations ∈ [1, 1e7]，length ∈ [1, 1024]，越界返回空串。
        """
        return self._call_string("OpPbkdf2Sha256", password, salt, int(iterations), int(length))

    def rsa_encrypt(self, text: str, public_key_pem: str) -> str:
        """RSA PKCS#1 v1.5 公钥加密，输出 Base64；支持 PKCS#1/PKCS#8 公钥 PEM。

        明文上限 mode_len-11 字节（2048 位即 245 字节），超限或私钥当公钥用返回空串。
        """
        return self._call_string("OpRsaEncrypt", text, public_key_pem)

    def rsa_decrypt(self, base64: str, private_key_pem: str) -> str:
        """RSA PKCS#1 v1.5 私钥解密；支持 PKCS#8 私钥 PEM。填充校验失败返回空串。"""
        return self._call_string("OpRsaDecrypt", base64, private_key_pem)

    # 注册表四件套（path 形如 "HKCU\\Software\\X"，name 传空串 = 默认值）
    def registry_read_string(self, path: str, name: str = "") -> tuple[str, int]:
        """读注册表字符串值，返回 (值, 1/0)；类型不符或缺失时返回 ("", 0)。"""
        return self._call_string_with_int("OpRegistryReadString", path, name)

    def registry_write_string(self, path: str, name: str, value: str) -> bool:
        """写注册表字符串值 REG_SZ（目录不存在会自动创建）。"""
        return self._call_ok("OpRegistryWriteString", path, name, value)

    def registry_read_dword(self, path: str, name: str = "") -> tuple[int, int]:
        """读注册表 DWORD 值，返回 (值, 1/0)；类型不符或缺失时返回 (0, 0)。"""
        self._check_open()
        value = ctypes.c_int(0)
        ret = int(self._dll.OpRegistryReadDword(self._handle, path, name, ctypes.byref(value)))
        return int(value.value), ret

    def registry_write_dword(self, path: str, name: str, value: int) -> bool:
        """写注册表 DWORD 值 REG_DWORD（按 32 位无符号解释，可传 0xFFFFFFFF）。"""
        return self._call_ok("OpRegistryWriteDword", path, name, int(value))

    # 全局热键（跨进程单实例：状态在命名共享内存 + 事件，owner 进程统一收消息）
    def hotkey_register(self, hotkey_id: int, modifiers: int, vk: int) -> bool:
        """注册全局热键：id ∈ [1, 0xBFFF]，modifiers 取低 4 位（1=Alt 2=Ctrl 4=Shift 8=Win），vk ∈ [1,255]。"""
        return self._call_ok("OpHotkeyRegister", int(hotkey_id), int(modifiers), int(vk))

    def hotkey_unregister(self, hotkey_id: int) -> bool:
        """注销指定热键；未注册的 id 返回 False。"""
        return self._call_ok("OpHotkeyUnregister", int(hotkey_id))

    def hotkey_poll(self, timeout_ms: int = 0) -> int:
        """等待并取出一个热键 id：超时/无事件返回 0；timeout_ms=0 非阻塞，<0 最多等 60 秒。"""
        return self._call_int("OpHotkeyPoll", int(timeout_ms))

    def hotkey_unregister_all(self) -> int:
        """注销本进程注册的全部热键，返回注销个数。"""
        return self._call_int("OpHotkeyUnregisterAll")

    # Screen draw
    def draw_rect(self, x1: int, y1: int, x2: int, y2: int, color: str, width: int = 1) -> bool:
        """屏幕矩形标注（坐标物理像素），color 同找色 RRGGBB 格式。"""
        return self._call_ok("OpDrawRect", int(x1), int(y1), int(x2), int(y2), color, int(width))

    def draw_line(self, x1: int, y1: int, x2: int, y2: int, color: str, width: int = 1) -> bool:
        """屏幕线段标注，color 同找色 RRGGBB 格式。"""
        return self._call_ok("OpDrawLine", int(x1), int(y1), int(x2), int(y2), color, int(width))

    def draw_text(self, x: int, y: int, text: str, color: str, font_size: int) -> bool:
        """屏幕文本标注（左上角为原点），color 同找色 RRGGBB 格式。"""
        return self._call_ok("OpDrawText", int(x), int(y), text, color, int(font_size))

    def draw_clear(self) -> bool:
        """清除全部屏幕标注。"""
        return self._call_ok("OpDrawClear")

    def draw_set_visible(self, show: bool) -> bool:
        """显示/隐藏屏幕标注层。"""
        return self._call_ok("OpDrawSetVisible", 1 if show else 0)

    def draw_circle(self, x: int, y: int, radius: int, color: str, width: int = 1) -> bool:
        """屏幕圆形标注：x,y 为圆心。"""
        return self._call_ok("OpDrawCircle", int(x), int(y), int(radius), color, int(width))

    def draw_image(self, x: int, y: int, file_name: str, alpha: int = 255) -> bool:
        """屏幕叠加层画图片文件（左上角）。alpha 0~255，<=0 视为 255。"""
        return self._call_ok("OpDrawImage", int(x), int(y), file_name, int(alpha))

    def draw_rect_ex(self, x: int, y: int, width: int, height: int, mode: int = 1,
                     line_width: int = 1) -> bool:
        """绑定窗口内矩形（客户区坐标）。mode：0 纯填充 / 1 纯描边 / 2 填充+描边。"""
        return self._call_ok("OpDrawRectEx", int(x), int(y), int(width), int(height), int(mode), int(line_width))

    def draw_circle_ex(self, x: int, y: int, radius: int, line_width: int = 1) -> bool:
        """绑定窗口内圆形（客户区坐标）。"""
        return self._call_ok("OpDrawCircleEx", int(x), int(y), int(radius), int(line_width))

    def draw_text_ex(self, x: int, y: int, text: str, font_size: int = 14) -> bool:
        """绑定窗口内文本（客户区坐标）。"""
        return self._call_ok("OpDrawTextEx", int(x), int(y), text, int(font_size))

    def draw_clear_bind(self) -> bool:
        """只清除绑定窗口那批图元。"""
        return self._call_ok("OpDrawClearBind")

    def draw_get_count(self):
        """返回 (屏幕层图元数, 绑定层图元数)。"""
        screen = ctypes.c_int(0)
        bind = ctypes.c_int(0)
        if not self._call_ok("OpDrawGetCount", ctypes.byref(screen), ctypes.byref(bind)):
            return (0, 0)
        return (screen.value, bind.value)

    def draw_set_color(self, color: str) -> bool:
        """设置 Ex 系列描边色（RRGGBB），影响之后绘制的图元。"""
        return self._call_ok("OpDrawSetColor", color)

    def draw_set_fill(self, enable: bool, color: str = "") -> bool:
        """填充开关 + 填充色。color 支持 RRGGBB（自动 0x66 半透明）或 AARRGGBB。"""
        return self._call_ok("OpDrawSetFill", 1 if enable else 0, color)

    def draw_image_ex(self, x: int, y: int, file_name: str, alpha: int = 255) -> bool:
        """绑定窗口内画图片文件（客户区坐标）；文件不存在返回 False。"""
        return self._call_ok("OpDrawImageEx", int(x), int(y), file_name, int(alpha))

    def draw_image_from_handle(self, x: int, y: int, image_handle: str, alpha: int = 255) -> bool:
        """把图片句柄内容画到屏幕叠加层。"""
        return self._call_ok("OpDrawImageFromHandle", int(x), int(y), image_handle, int(alpha))

    def draw_rect_tag(self, tag: str, x1: int, y1: int, x2: int, y2: int, color: str,
                      width: int = 1) -> bool:
        """屏幕层带标签矩形：同标签只保留最新一条（实时跟踪框用，不必全清重画）。"""
        return self._call_ok("OpDrawRectTag", tag, int(x1), int(y1), int(x2), int(y2), color, int(width))

    def draw_rect_ex_tag(self, tag: str, x: int, y: int, width: int, height: int, mode: int = 1,
                         line_width: int = 1) -> bool:
        """绑定层带标签矩形（客户区坐标）。mode：0 填充 / 1 描边 / 2 填充+描边。"""
        return self._call_ok("OpDrawRectExTag", tag, int(x), int(y), int(width), int(height), int(mode),
                             int(line_width))

    def draw_remove_tag(self, tag: str) -> int:
        """删除同标签图元（两层一起清），返回删除个数；0 = 没有匹配项。"""
        return self._call_int("OpDrawRemoveTag", tag)

    # UIA 元素层（批 H，2026-11-23）
    # ⚠ 只对暴露 UIA 的程序有效（Win32/WinForms/WPF/Qt/Chromium 系）；DX 自绘游戏读不到节点。
    # ⚠ element_id 是本进程内的注册表序号，跨 DLL 层不可通用；坐标是屏幕物理像素。
    def uia_dump(self, hwnd: int = 0, max_depth: int = 3) -> str:
        """按深度列出窗口下所有元素（hwnd=0 取前台窗口）。返回 JSON。"""
        return self._call_string("OpUiaDump", int(hwnd), int(max_depth))

    def uia_find(self, selector: str, hwnd: int = 0, timeout_ms: int = 0) -> str:
        """按选择器查找元素。语法：name=确定 >> class=Button；name*=包含；键 name/class/id/type/index。"""
        return self._call_string("OpUiaFind", int(hwnd), selector, int(timeout_ms))

    def uia_from_point(self, x: int, y: int) -> str:
        """屏幕坐标命中的元素（鼠标所指是什么控件）。返回 JSON。"""
        return self._call_string("OpUiaFromPoint", int(x), int(y))

    def uia_invoke(self, element_id: int) -> bool:
        """触发元素（Invoke 模式，复选框走 Toggle）。"""
        return self._call_ok("OpUiaInvoke", int(element_id))

    def uia_get_value(self, element_id: int) -> str:
        """读元素的值（ValuePattern）；无该模式返回空串。"""
        return self._call_string("OpUiaGetValue", int(element_id))

    def uia_set_value(self, element_id: int, value: str) -> bool:
        """写元素的值（ValuePattern）；只读控件返回 False。"""
        return self._call_ok("OpUiaSetValue", int(element_id), value)

    def uia_release(self, element_id: int) -> bool:
        """释放单个元素。长期运行的脚本必须及时释放。"""
        return self._call_ok("OpUiaRelease", int(element_id))

    def uia_release_all(self) -> int:
        """释放全部元素，返回释放个数。"""
        return self._call_int("OpUiaReleaseAll")

    # 汇编模块（批 D 补齐 2026-11-23）
    def asm_call(self, code_hex: str, hwnd: int = 0, timeout_ms: int = 5000) -> tuple[str, int]:
        """在 hwnd 所属进程执行机器码；hwnd=0 表示当前进程。返回 (JSON, 成功标志)。

        code_hex 形如 "48 C7 C0 2A 00 00 00 C3"（需自带 ret），支持 "??" 占位。
        成功 JSON 带 ret / ret_hex；失败 JSON 带 error。
        """
        return self._call_string_with_int("OpAsmCall", int(hwnd), code_hex, int(timeout_ms))

    def assemble(self, asm_text: str, arch: int = 0) -> tuple[str, int]:
        """汇编文本 -> 机器码。arch: 32 / 64；0 = 按当前进程位数。

        返回 (JSON, 成功标志)；成功时 JSON 的 hex 字段可直接传给 asm_call。
        数字字面量：裸数字是**十进制**（``mov eax, 100`` = 100），十六进制写 ``0x64``。
        """
        return self._call_string_with_int("OpAssemble", asm_text, int(arch))

    def disassemble(self, bytes_hex: str, address: str = "0", count: int = 0, arch: int = 0) -> tuple[str, int]:
        """机器码 -> 汇编文本。count<=0 表示解到字节用完；arch 同上（0 = 当前进程位数）。

        注意 arch 必须给对：用 64 位模式解 32 位代码会静默产出错码。
        """
        return self._call_string_with_int("OpDisassemble", bytes_hex, address, int(count), int(arch))

    # Background binding
    def bind_window(
        self,
        hwnd: int,
        display: constants.DisplayMode | str = constants.DisplayMode.NORMAL,
        mouse: constants.MouseMode | str = constants.MouseMode.NORMAL,
        keypad: constants.KeypadMode | str = constants.KeypadMode.NORMAL,
        mode: int = 0,
    ) -> bool:
        return self._call_ok(
            "OpBindWindow",
            int(hwnd),
            self._enum_text(display),
            self._enum_text(mouse),
            self._enum_text(keypad),
            int(mode),
        )

    def bind_window_ex(
        self,
        display_hwnd: int,
        input_hwnd: int,
        display: constants.DisplayMode | str = constants.DisplayMode.NORMAL,
        mouse: constants.MouseMode | str = constants.MouseMode.NORMAL,
        keypad: constants.KeypadMode | str = constants.KeypadMode.NORMAL,
        mode: int = 0,
    ) -> bool:
        return self._call_ok(
            "OpBindWindowEx",
            int(display_hwnd),
            int(input_hwnd),
            self._enum_text(display),
            self._enum_text(mouse),
            self._enum_text(keypad),
            int(mode),
        )

    def unbind_window(self) -> bool:
        return self._call_ok("OpUnBindWindow")

    def lock_input(self, lock: int) -> bool:
        return self._call_ok("OpLockInput", int(lock))

    def down_cpu(self, type: int = 0, rate: int = 0) -> bool:
        """绑定降载：每次截图成功后强制延时 rate 毫秒（0-100，自动钳制），0 关闭。
        type 0=推荐档 1=高效档（当前实现相同）。解绑不重置，随 Op 对象销毁。"""
        return self._call_ok("OpDownCpu", int(type), int(rate))

    def set_dx_attr(self, attr: int, value: int) -> bool:
        """设置 dx 输入通道开关。

        attr 为 0 时 value 是完整掩码；attr 取 1(DirectInput)/2(RawInput)/4(窗口消息)
        或其组合时，value 非 0 表示打开这些通道，0 表示关闭。默认 7（三通道全开）。
        """
        return self._call_ok("OpSetDxAttr", int(attr), int(value))

    def get_dx_attr(self) -> int:
        """返回当前的 dx 输入通道开关掩码。"""
        return self._call_int("OpGetDxAttr")

    def get_bind_window(self) -> int:
        return self._call_intptr("OpGetBindWindow")

    def is_bind(self) -> bool:
        return bool(self._call_int("OpIsBind"))

    def get_display_method(self) -> str:
        """当前实际生效的图色模式名（"normal"/"gdi2"/"dx.d3d11"/"normal.wgc"…）。

        未绑定返回空串；normal.auto 绑定成功后返回自动选中并成功的那一个。
        截图黑屏/找图错位时先用它确认走的是哪条链路。
        """
        return self._call_string("OpGetDisplayMethod")

    # Mouse
    def get_cursor_pos(self) -> tuple[int, int]:
        return self._call_checked_point("OpGetCursorPos")

    def get_cursor_shape(self) -> str:
        return self._call_string("OpGetCursorShape")

    def get_cursor_image(self) -> str:
        """当前鼠标光标图像：32bpp BMP 的 Base64（top-down BGRA 含 alpha）；失败返回空串。"""
        return self._call_string("OpGetCursorImage")

    def move_r(self, x: int, y: int) -> bool:
        return self._call_ok("OpMoveR", int(x), int(y))

    def move_to(self, x: int, y: int) -> bool:
        return self._call_ok("OpMoveTo", int(x), int(y))

    def move_to_ex(self, x: int, y: int, width: int, height: int) -> str:
        return self._call_string("OpMoveToEx", int(x), int(y), int(width), int(height))

    def move_to_smooth(self, x: int, y: int, duration: int) -> bool:
        return self._call_ok("OpMoveToSmooth", int(x), int(y), int(duration))

    def move_to_ex_smooth(self, x: int, y: int, width: int, height: int, duration: int) -> str:
        return self._call_string("OpMoveToExSmooth", int(x), int(y), int(width), int(height), int(duration))

    def move_path(self, path: str, duration: int) -> bool:
        return self._call_ok("OpMovePath", str(path), int(duration))

    def drag_path(self, path: str, duration: int) -> bool:
        return self._call_ok("OpDragPath", str(path), int(duration))

    def set_mouse_trajectory(
        self,
        mode: int,
        min_duration: int,
        max_duration: int,
        jitter: int,
        start_delay: int,
        end_delay: int,
    ) -> bool:
        return self._call_ok(
            "OpSetMouseTrajectory",
            int(mode),
            int(min_duration),
            int(max_duration),
            int(jitter),
            int(start_delay),
            int(end_delay),
        )

    def left_click(self) -> bool:
        return self._call_ok("OpLeftClick")

    def left_double_click(self) -> bool:
        return self._call_ok("OpLeftDoubleClick")

    def left_down(self) -> bool:
        return self._call_ok("OpLeftDown")

    def left_up(self) -> bool:
        return self._call_ok("OpLeftUp")

    def middle_click(self) -> bool:
        return self._call_ok("OpMiddleClick")

    def middle_double_click(self) -> bool:
        return self._call_ok("OpMiddleDoubleClick")

    def middle_down(self) -> bool:
        return self._call_ok("OpMiddleDown")

    def middle_up(self) -> bool:
        return self._call_ok("OpMiddleUp")

    def right_click(self) -> bool:
        return self._call_ok("OpRightClick")

    def right_double_click(self) -> bool:
        return self._call_ok("OpRightDoubleClick")

    def right_down(self) -> bool:
        return self._call_ok("OpRightDown")

    def right_up(self) -> bool:
        return self._call_ok("OpRightUp")

    def xbutton1_click(self) -> bool:
        return self._call_ok("OpXButton1Click")

    def xbutton1_double_click(self) -> bool:
        return self._call_ok("OpXButton1DoubleClick")

    def xbutton1_down(self) -> bool:
        return self._call_ok("OpXButton1Down")

    def xbutton1_up(self) -> bool:
        return self._call_ok("OpXButton1Up")

    def xbutton2_click(self) -> bool:
        return self._call_ok("OpXButton2Click")

    def xbutton2_double_click(self) -> bool:
        return self._call_ok("OpXButton2DoubleClick")

    def xbutton2_down(self) -> bool:
        return self._call_ok("OpXButton2Down")

    def xbutton2_up(self) -> bool:
        return self._call_ok("OpXButton2Up")

    def wheel(self, delta: int) -> bool:
        return self._call_ok("OpWheel", int(delta))

    def hwheel(self, delta: int) -> bool:
        return self._call_ok("OpHWheel", int(delta))

    def wheel_down(self) -> bool:
        return self._call_ok("OpWheelDown")

    def wheel_up(self) -> bool:
        return self._call_ok("OpWheelUp")

    def set_mouse_delay(self, delay_type: constants.MouseDelayType | str, delay: int) -> bool:
        return self._call_ok("OpSetMouseDelay", self._enum_text(delay_type), int(delay))

    # Keyboard
    def get_key_state(self, vk_code: int) -> int:
        return self._call_int("OpGetKeyState", int(vk_code))

    def key_down(self, vk_code: int) -> bool:
        return self._call_ok("OpKeyDown", int(vk_code))

    def key_down_char(self, vk_code: str) -> bool:
        return self._call_ok("OpKeyDownChar", vk_code)

    def key_up(self, vk_code: int) -> bool:
        return self._call_ok("OpKeyUp", int(vk_code))

    def key_up_char(self, vk_code: str) -> bool:
        return self._call_ok("OpKeyUpChar", vk_code)

    def wait_key(self, vk_code: int, time_out: int) -> int:
        return self._call_int("OpWaitKey", int(vk_code), int(time_out))

    def key_press(self, vk_code: int) -> bool:
        return self._call_ok("OpKeyPress", int(vk_code))

    def key_press_char(self, vk_code: str) -> bool:
        return self._call_ok("OpKeyPressChar", vk_code)

    def set_keypad_delay(self, delay_type: constants.KeypadDelayType | str, delay: int) -> bool:
        return self._call_ok("OpSetKeypadDelay", self._enum_text(delay_type), int(delay))

    def key_press_str(self, key_str: str, delay: int) -> bool:
        return self._call_ok("OpKeyPressStr", key_str, int(delay))

    def key_press_hotkey(self, combo: str) -> bool:
        """组合键一次按下：形如 "ctrl+alt+a" / "shift+f1" / "win+d"。"""
        return self._call_ok("OpKeyPressHotkey", combo)

    # Image and color
    def capture(self, x1: int, y1: int, x2: int, y2: int, file_name: str | Path) -> bool:
        return self._call_ok("OpCapture", int(x1), int(y1), int(x2), int(y2), str(file_name))

    def cmp_color(self, x: int, y: int, color: str, sim: float) -> bool:
        return bool(self._call_int("OpCmpColor", int(x), int(y), color, float(sim)))

    def find_color(
        self,
        x1: int,
        y1: int,
        x2: int,
        y2: int,
        color: str,
        sim: float,
        direction: constants.FindDirection | int = constants.FindDirection.LEFT_TOP_TO_RIGHT_BOTTOM,
    ) -> tuple[int, int, int]:
        return self._call_result_point(
            "OpFindColor",
            int(x1),
            int(y1),
            int(x2),
            int(y2),
            color,
            float(sim),
            int(direction),
        )

    def find_color_ex(
        self,
        x1: int,
        y1: int,
        x2: int,
        y2: int,
        color: str,
        sim: float,
        direction: constants.FindDirection | int = constants.FindDirection.LEFT_TOP_TO_RIGHT_BOTTOM,
    ) -> str:
        return self._call_string("OpFindColorEx", int(x1), int(y1), int(x2), int(y2), color, float(sim), int(direction))

    def find_multi_color(
        self,
        x1: int,
        y1: int,
        x2: int,
        y2: int,
        first_color: str,
        offset_color: str,
        sim: float,
        direction: constants.FindDirection | int = constants.FindDirection.LEFT_TOP_TO_RIGHT_BOTTOM,
    ) -> tuple[int, int, int]:
        return self._call_result_point(
            "OpFindMultiColor",
            int(x1),
            int(y1),
            int(x2),
            int(y2),
            first_color,
            offset_color,
            float(sim),
            int(direction),
        )

    def find_multi_color_ex(
        self,
        x1: int,
        y1: int,
        x2: int,
        y2: int,
        first_color: str,
        offset_color: str,
        sim: float,
        direction: constants.FindDirection | int = constants.FindDirection.LEFT_TOP_TO_RIGHT_BOTTOM,
    ) -> str:
        return self._call_string(
            "OpFindMultiColorEx",
            int(x1),
            int(y1),
            int(x2),
            int(y2),
            first_color,
            offset_color,
            float(sim),
            int(direction),
        )

    def find_pic(
        self,
        x1: int,
        y1: int,
        x2: int,
        y2: int,
        files: str,
        delta_color: str,
        sim: float,
        direction: constants.FindDirection | int = constants.FindDirection.LEFT_TOP_TO_RIGHT_BOTTOM,
    ) -> tuple[int, int, int]:
        return self._call_result_point(
            "OpFindPic",
            int(x1),
            int(y1),
            int(x2),
            int(y2),
            files,
            delta_color,
            float(sim),
            int(direction),
        )

    def find_pic_ex(
        self,
        x1: int,
        y1: int,
        x2: int,
        y2: int,
        files: str,
        delta_color: str,
        sim: float,
        direction: constants.FindDirection | int = constants.FindDirection.LEFT_TOP_TO_RIGHT_BOTTOM,
    ) -> str:
        return self._call_string(
            "OpFindPicEx",
            int(x1),
            int(y1),
            int(x2),
            int(y2),
            files,
            delta_color,
            float(sim),
            int(direction),
        )

    def find_pic_ex_s(
        self,
        x1: int,
        y1: int,
        x2: int,
        y2: int,
        files: str,
        delta_color: str,
        sim: float,
        direction: constants.FindDirection | int = constants.FindDirection.LEFT_TOP_TO_RIGHT_BOTTOM,
    ) -> str:
        return self._call_string(
            "OpFindPicExS",
            int(x1),
            int(y1),
            int(x2),
            int(y2),
            files,
            delta_color,
            float(sim),
            int(direction),
        )

    def find_color_block(
        self,
        x1: int,
        y1: int,
        x2: int,
        y2: int,
        color: str,
        sim: float,
        count: int,
        height: int,
        width: int,
    ) -> tuple[int, int, int]:
        return self._call_result_point(
            "OpFindColorBlock",
            int(x1),
            int(y1),
            int(x2),
            int(y2),
            color,
            float(sim),
            int(count),
            int(height),
            int(width),
        )

    def find_color_block_ex(
        self,
        x1: int,
        y1: int,
        x2: int,
        y2: int,
        color: str,
        sim: float,
        count: int,
        height: int,
        width: int,
    ) -> str:
        return self._call_string(
            "OpFindColorBlockEx",
            int(x1),
            int(y1),
            int(x2),
            int(y2),
            color,
            float(sim),
            int(count),
            int(height),
            int(width),
        )

    def get_color(self, x: int, y: int) -> str:
        return self._call_string("OpGetColor", int(x), int(y))

    def get_color_num(self, x1: int, y1: int, x2: int, y2: int, color: str, sim: float) -> int:
        return self._call_int("OpGetColorNum", int(x1), int(y1), int(x2), int(y2), color, float(sim))

    def hex2rgb(self, color: str) -> tuple[int, int, int]:
        """颜色串（"RRGGBB"/"AARRGGBB"，可带 #/0x 前缀）转 RGB 分量。解析失败抛 OpCallError。"""
        self._check_open()
        r, g, b = ctypes.c_int(), ctypes.c_int(), ctypes.c_int()
        ok = self._dll.OpHex2RGB(self._handle, color, ctypes.byref(r), ctypes.byref(g), ctypes.byref(b))
        self._ok(ok, "OpHex2RGB")
        return int(r.value), int(g.value), int(b.value)

    def hex2argb(self, color: str) -> tuple[int, int, int, int]:
        """同 hex2rgb，额外返回 alpha（6 位输入 alpha=255）。"""
        self._check_open()
        a, r, g, b = ctypes.c_int(), ctypes.c_int(), ctypes.c_int(), ctypes.c_int()
        ok = self._dll.OpHex2ARGB(self._handle, color, ctypes.byref(a), ctypes.byref(r), ctypes.byref(g), ctypes.byref(b))
        self._ok(ok, "OpHex2ARGB")
        return int(a.value), int(r.value), int(g.value), int(b.value)

    def argb2hex(self, a: int, r: int, g: int, b: int) -> str:
        """ARGB 分量转 "AARRGGBB"（大写），分量越界按 0/255 截断。"""
        return self._call_string("OpARGB2Hex", int(a), int(r), int(g), int(b))

    def rgb2hsv(self, r: int, g: int, b: int) -> tuple[float, float, float]:
        """RGB 转 HSV：H∈[0,360)，S/V∈[0,100]。"""
        self._check_open()
        h, s, v = ctypes.c_double(), ctypes.c_double(), ctypes.c_double()
        ok = self._dll.OpRGB2HSV(self._handle, int(r), int(g), int(b), ctypes.byref(h), ctypes.byref(s), ctypes.byref(v))
        self._ok(ok, "OpRGB2HSV")
        return float(h.value), float(s.value), float(v.value)

    def hex2hsv(self, color: str) -> tuple[float, float, float]:
        """颜色串转 HSV（口径同 rgb2hsv）。解析失败抛 OpCallError。"""
        self._check_open()
        h, s, v = ctypes.c_double(), ctypes.c_double(), ctypes.c_double()
        ok = self._dll.OpHex2HSV(self._handle, color, ctypes.byref(h), ctypes.byref(s), ctypes.byref(v))
        self._ok(ok, "OpHex2HSV")
        return float(h.value), float(s.value), float(v.value)

    def get_color_hex(self, r: int, g: int, b: int) -> str:
        """RGB 分量转 "RRGGBB"（大写），分量越界按 0/255 截断。"""
        return self._call_string("OpGetColorHex", int(r), int(g), int(b))

    def set_display_input(self, mode: constants.DisplayInputMode | str) -> bool:
        return self._call_ok("OpSetDisplayInput", self._enum_text(mode))

    def load_pic(self, file_name: str | Path) -> bool:
        return self._call_ok("OpLoadPic", str(file_name))

    def free_pic(self, file_name: str | Path) -> bool:
        return self._call_ok("OpFreePic", str(file_name))

    def load_mem_pic(self, file_name: str, data: bytes | bytearray | memoryview | int, size: int | None = None) -> bool:
        self._check_open()
        if isinstance(data, int):
            if size is None:
                raise ValueError("size is required when data is a pointer")
            ptr = ctypes.c_void_p(data)
            data_size = int(size)
        else:
            raw = bytes(data)
            buffer = ctypes.create_string_buffer(raw)
            ptr = ctypes.cast(buffer, ctypes.c_void_p)
            data_size = len(raw) if size is None else int(size)
        ok = self._dll.OpLoadMemPic(self._handle, file_name, ptr, data_size)
        return self._ok(ok, "OpLoadMemPic")

    def get_pic_size(self, pic_name: str) -> tuple[int, int]:
        return self._call_checked_size("OpGetPicSize", pic_name)

    def get_screen_data(self, x1: int, y1: int, x2: int, y2: int) -> int:
        self._check_open()
        ret = ctypes.c_int()
        ptr = self._dll.OpGetScreenData(self._handle, int(x1), int(y1), int(x2), int(y2), ctypes.byref(ret))
        self._ok(ret.value, "OpGetScreenData")
        return int(ptr)

    def get_screen_data_bmp(self, x1: int, y1: int, x2: int, y2: int) -> tuple[int, int]:
        self._check_open()
        size = ctypes.c_int()
        ret = ctypes.c_int()
        ptr = self._dll.OpGetScreenDataBmp(
            self._handle,
            int(x1),
            int(y1),
            int(x2),
            int(y2),
            ctypes.byref(size),
            ctypes.byref(ret),
        )
        self._ok(ret.value, "OpGetScreenDataBmp")
        return int(ptr), int(size.value)

    def get_screen_frame_info(self) -> tuple[int, int]:
        self._check_open()
        frame_id = ctypes.c_int()
        frame_time = ctypes.c_int()
        self._dll.OpGetScreenFrameInfo(self._handle, ctypes.byref(frame_id), ctypes.byref(frame_time))
        return int(frame_id.value), int(frame_time.value)

    def match_pic_name(self, pic_name: str) -> str:
        return self._call_string("OpMatchPicName", pic_name)

    # OpenCV
    def cv_load_template(self, name: str, file_path: str | Path) -> bool:
        return self._call_ok("OpCvLoadTemplate", name, str(file_path))

    def cv_load_masked_template(self, name: str, template_path: str | Path, mask_path: str | Path) -> bool:
        return self._call_ok("OpCvLoadMaskedTemplate", name, str(template_path), str(mask_path))

    def cv_remove_template(self, name: str) -> bool:
        return self._call_ok("OpCvRemoveTemplate", name)

    def cv_remove_all_templates(self) -> bool:
        return self._call_ok("OpCvRemoveAllTemplates")

    def cv_has_template(self, name: str) -> bool:
        return bool(self._call_int("OpCvHasTemplate", name))

    def cv_get_template_count(self) -> int:
        return self._call_int("OpCvGetTemplateCount")

    def cv_get_all_template_names(self) -> str:
        return self._call_string("OpCvGetAllTemplateNames")

    def cv_get_open_cv_version(self) -> str:
        return self._call_string("OpCvGetOpenCvVersion")

    def cv_load_template_list(self, template_list: str) -> bool:
        return self._call_ok("OpCvLoadTemplateList", template_list)

    def cv_to_gray(self, src_file: str | Path, dst_file: str | Path) -> bool:
        return self._call_ok("OpCvToGray", str(src_file), str(dst_file))

    def cv_to_binary(self, src_file: str | Path, dst_file: str | Path) -> bool:
        return self._call_ok("OpCvToBinary", str(src_file), str(dst_file))

    def cv_to_edge(self, src_file: str | Path, dst_file: str | Path) -> bool:
        return self._call_ok("OpCvToEdge", str(src_file), str(dst_file))

    def cv_to_outline(self, src_file: str | Path, dst_file: str | Path) -> bool:
        return self._call_ok("OpCvToOutline", str(src_file), str(dst_file))

    def cv_denoise(self, src_file: str | Path, dst_file: str | Path) -> bool:
        return self._call_ok("OpCvDenoise", str(src_file), str(dst_file))

    def cv_equalize(self, src_file: str | Path, dst_file: str | Path) -> bool:
        return self._call_ok("OpCvEqualize", str(src_file), str(dst_file))

    def cv_clahe(self, src_file: str | Path, dst_file: str | Path, clip_limit: float, tile_grid_size: int) -> bool:
        return self._call_ok("OpCvCLAHE", str(src_file), str(dst_file), float(clip_limit), int(tile_grid_size))

    def cv_blur(
        self,
        src_file: str | Path,
        dst_file: str | Path,
        mode: constants.Blur | str = constants.Blur.GAUSSIAN,
        kernel_size: int = 3,
    ) -> bool:
        return self._call_ok("OpCvBlur", str(src_file), str(dst_file), self._enum_text(mode), int(kernel_size))

    def cv_sharpen(self, src_file: str | Path, dst_file: str | Path, strength: float) -> bool:
        return self._call_ok("OpCvSharpen", str(src_file), str(dst_file), float(strength))

    def cv_crop_valid(self, src_file: str | Path, dst_file: str | Path) -> bool:
        return self._call_ok("OpCvCropValid", str(src_file), str(dst_file))

    def cv_rotate(self, src_file: str | Path, dst_file: str | Path, angle_deg: float) -> bool:
        """旋转（角度制，正角逆时针）：90 倍数无损无白边；任意角扩画布白边。"""
        return self._call_ok("OpCvRotate", str(src_file), str(dst_file), float(angle_deg))

    def cv_flip(self, src_file: str | Path, dst_file: str | Path, mode: int) -> bool:
        """翻转：mode 0 上下 / 1 左右 / 2 双向（=旋转 180°）。"""
        return self._call_ok("OpCvFlip", str(src_file), str(dst_file), int(mode))

    def cv_connected_components(self, src_file: str | Path, min_area: float) -> str:
        return self._call_string("OpCvConnectedComponents", str(src_file), float(min_area))

    def cv_find_contours(self, src_file: str | Path, min_area: float) -> str:
        return self._call_string("OpCvFindContours", str(src_file), float(min_area))

    def cv_preprocess_pipeline(self, src_file: str | Path, dst_file: str | Path, pipeline: str) -> bool:
        return self._call_ok("OpCvPreprocessPipeline", str(src_file), str(dst_file), pipeline)

    def cv_crop(self, src_file: str | Path, x: int, y: int, width: int, height: int, dst_file: str | Path) -> bool:
        return self._call_ok("OpCvCrop", str(src_file), int(x), int(y), int(width), int(height), str(dst_file))

    def cv_resize(self, src_file: str | Path, width: int, height: int, dst_file: str | Path) -> bool:
        return self._call_ok("OpCvResize", str(src_file), int(width), int(height), str(dst_file))

    def cv_threshold(
        self,
        src_file: str | Path,
        dst_file: str | Path,
        threshold: float,
        max_value: float,
        mode: constants.Threshold | str = constants.Threshold.BINARY,
    ) -> bool:
        return self._call_ok(
            "OpCvThreshold",
            str(src_file),
            str(dst_file),
            float(threshold),
            float(max_value),
            self._enum_text(mode),
        )

    def cv_in_range(
        self,
        src_file: str | Path,
        dst_file: str | Path,
        color_space: constants.ColorSpace | str,
        lower: str,
        upper: str,
    ) -> bool:
        return self._call_ok("OpCvInRange", str(src_file), str(dst_file), self._enum_text(color_space), lower, upper)

    def cv_morphology(
        self,
        src_file: str | Path,
        dst_file: str | Path,
        mode: constants.Morphology | str = constants.Morphology.OPEN,
        kernel_size: int = 3,
        iterations: int = 1,
    ) -> bool:
        return self._call_ok(
            "OpCvMorphology",
            str(src_file),
            str(dst_file),
            self._enum_text(mode),
            int(kernel_size),
            int(iterations),
        )

    def cv_thin(
        self,
        src_file: str | Path,
        dst_file: str | Path,
        mode: constants.Thin | str = constants.Thin.ZHANG_SUEN,
    ) -> bool:
        return self._call_ok("OpCvThin", str(src_file), str(dst_file), self._enum_text(mode))

    def cv_match_template(
        self,
        x: int,
        y: int,
        width: int,
        height: int,
        template_name: str,
        threshold: float,
        direction: constants.SearchDirection | int = constants.SearchDirection.LEFT_TO_RIGHT,
        strip_mode: constants.Strip | int = constants.Strip.NONE,
        method: constants.MatchMethod | int = constants.MatchMethod.SQDIFF_NORMED,
        color_mode: constants.MatchColor | int = constants.MatchColor.GRAY,
    ) -> str:
        return self._call_string(
            "OpCvMatchTemplate",
            int(x),
            int(y),
            int(width),
            int(height),
            template_name,
            float(threshold),
            int(direction),
            int(strip_mode),
            int(method),
            int(color_mode),
        )

    def cv_match_template_scale(
        self,
        x: int,
        y: int,
        width: int,
        height: int,
        template_name: str,
        scales: str,
        threshold: float,
        method: constants.MatchMethod | int = constants.MatchMethod.SQDIFF_NORMED,
        color_mode: constants.MatchColor | int = constants.MatchColor.GRAY,
    ) -> str:
        return self._call_string(
            "OpCvMatchTemplateScale",
            int(x),
            int(y),
            int(width),
            int(height),
            template_name,
            scales,
            float(threshold),
            int(method),
            int(color_mode),
        )

    def cv_match_template_rot(
        self,
        x: int,
        y: int,
        width: int,
        height: int,
        template_name: str,
        angles: str = "",
        threshold: float = 0.9,
        method: constants.MatchMethod | int = constants.MatchMethod.SQDIFF_NORMED,
        color_mode: constants.MatchColor | int = constants.MatchColor.GRAY,
    ) -> str:
        """多角度模板匹配。angles 形如「0|15|-15」，空串走默认角度集；返回 JSON，额外含命中角度 angle。"""
        return self._call_string(
            "OpCvMatchTemplateRot",
            int(x),
            int(y),
            int(width),
            int(height),
            template_name,
            angles,
            float(threshold),
            int(method),
            int(color_mode),
        )

    def cv_match_any_template(
        self,
        x: int,
        y: int,
        width: int,
        height: int,
        template_names: str,
        threshold: float,
        direction: constants.SearchDirection | int = constants.SearchDirection.LEFT_TO_RIGHT,
        strip_mode: constants.Strip | int = constants.Strip.NONE,
        method: constants.MatchMethod | int = constants.MatchMethod.SQDIFF_NORMED,
        color_mode: constants.MatchColor | int = constants.MatchColor.GRAY,
    ) -> str:
        return self._call_string(
            "OpCvMatchAnyTemplate",
            int(x),
            int(y),
            int(width),
            int(height),
            template_names,
            float(threshold),
            int(direction),
            int(strip_mode),
            int(method),
            int(color_mode),
        )

    def cv_match_all_templates(
        self,
        x: int,
        y: int,
        width: int,
        height: int,
        template_names: str,
        threshold: float,
        direction: constants.SearchDirection | int = constants.SearchDirection.LEFT_TO_RIGHT,
        strip_mode: constants.Strip | int = constants.Strip.NONE,
        method: constants.MatchMethod | int = constants.MatchMethod.SQDIFF_NORMED,
        color_mode: constants.MatchColor | int = constants.MatchColor.GRAY,
    ) -> str:
        return self._call_string(
            "OpCvMatchAllTemplates",
            int(x),
            int(y),
            int(width),
            int(height),
            template_names,
            float(threshold),
            int(direction),
            int(strip_mode),
            int(method),
            int(color_mode),
        )

    def cv_feature_match_template(self, x: int, y: int, width: int, height: int, template_name: str, threshold: float) -> str:
        return self._call_string(
            "OpCvFeatureMatchTemplate",
            int(x),
            int(y),
            int(width),
            int(height),
            template_name,
            float(threshold),
        )

    def cv_edge_match_template(self, x: int, y: int, width: int, height: int, template_name: str, threshold: float) -> str:
        return self._call_string(
            "OpCvEdgeMatchTemplate",
            int(x),
            int(y),
            int(width),
            int(height),
            template_name,
            float(threshold),
        )

    def cv_shape_match_template(self, x: int, y: int, width: int, height: int, template_name: str, threshold: float) -> str:
        return self._call_string(
            "OpCvShapeMatchTemplate",
            int(x),
            int(y),
            int(width),
            int(height),
            template_name,
            float(threshold),
        )

    # OCR
    def set_ocr_engine(self, path_of_engine: str | Path, dll_name: str, argv: str) -> bool:
        return self._call_ok("OpSetOcrEngine", str(path_of_engine), dll_name, argv)

    def ocr_warmup(self) -> bool:
        """OCR 预热：触发引擎懒加载 + 一次小图推理。返回 True 引擎就绪。"""
        return self._call_ok("OpOcrWarmup")

    def set_yolo_engine(self, path_of_engine: str | Path, dll_name: str, argv: str) -> bool:
        return self._call_ok("OpSetYoloEngine", str(path_of_engine), dll_name, argv)

    def yolo_detect(self, x1: int, y1: int, x2: int, y2: int, conf: float, iou: float) -> str:
        return self._call_string("OpYoloDetect", int(x1), int(y1), int(x2), int(y2), float(conf), float(iou))

    def yolo_detect_from_file(self, file_name: str | Path, conf: float, iou: float) -> str:
        return self._call_string("OpYoloDetectFromFile", str(file_name), float(conf), float(iou))

    def yolo_classify(self, x1: int, y1: int, x2: int, y2: int, top_k: int = 5) -> tuple[str, int]:
        """分类：返回 (json, 命中数)。top_k 截断前 N 个类别。"""
        return self._call_string_with_int(
            "OpYoloClassify", int(x1), int(y1), int(x2), int(y2), int(top_k)
        )

    def yolo_classify_from_file(self, file_name: str | Path, top_k: int = 5) -> tuple[str, int]:
        return self._call_string_with_int("OpYoloClassifyFromFile", str(file_name), int(top_k))

    def yolo_segment(self, x1: int, y1: int, x2: int, y2: int, conf: float, iou: float) -> tuple[str, int]:
        """实例分割：返回 (json, 命中数)，每条带 polygon（原图坐标）与 mask_area。"""
        return self._call_string_with_int(
            "OpYoloSegment", int(x1), int(y1), int(x2), int(y2), float(conf), float(iou)
        )

    def yolo_segment_from_file(self, file_name: str | Path, conf: float, iou: float) -> tuple[str, int]:
        return self._call_string_with_int("OpYoloSegmentFromFile", str(file_name), float(conf), float(iou))

    def yolo_pose(self, x1: int, y1: int, x2: int, y2: int, conf: float, iou: float) -> tuple[str, int]:
        """姿态估计：返回 (json, 命中数)，每个目标带 keypoints 数组 ({x,y,confidence})。"""
        return self._call_string_with_int(
            "OpYoloPose", int(x1), int(y1), int(x2), int(y2), float(conf), float(iou)
        )

    def yolo_pose_from_file(self, file_name: str | Path, conf: float, iou: float) -> tuple[str, int]:
        return self._call_string_with_int("OpYoloPoseFromFile", str(file_name), float(conf), float(iou))

    def yolo_obb(self, x1: int, y1: int, x2: int, y2: int, conf: float, iou: float) -> tuple[str, int]:
        """旋转框检测：返回 (json, 命中数)，带 center/size/angle/bbox/corners。"""
        return self._call_string_with_int(
            "OpYoloObb", int(x1), int(y1), int(x2), int(y2), float(conf), float(iou)
        )

    def yolo_obb_from_file(self, file_name: str | Path, conf: float, iou: float) -> tuple[str, int]:
        return self._call_string_with_int("OpYoloObbFromFile", str(file_name), float(conf), float(iou))

    def yolo_get_model_info(self) -> str:
        """当前模型信息（JSON：ok/task/task_code/input_width/input_height/device/dml）。"""
        return self._call_string("OpYoloGetModelInfo")

    # YOLO 命名模型平台（2026-11-23）：多模型注册表 / 热切换 / 设备枚举 / OPYMOD01 加密包
    def yolo_load_model(
        self, name: str, path: str | Path, argv: str = ""
    ) -> tuple[str, int]:
        """按名字加载模型进注册表，返回 (JSON, 1/0)；加载成功的新模型自动激活。

        密文包（头 OPYMOD01）必须用 argv 传 "--password=xxx"：
        走内存解密加载，不落明文临时文件。失败时 JSON 里 code 为
        file_not_found / password_required / decrypt_failed / load_failed。
        """
        return self._call_string_with_int("OpYoloLoadModel", name, str(path), argv)

    def yolo_use_model(self, name: str) -> bool:
        """热切换已注册模型为当前激活模型；未注册名字返回 False。"""
        return self._call_ok("OpYoloUseModel", name)

    def yolo_release_model(self, name: str) -> bool:
        """释放指定模型；name 传空串释放全部。释放当前激活模型会让引擎回到未加载。"""
        return self._call_ok("OpYoloReleaseModel", name)

    def yolo_list_models(self) -> str:
        """注册表快照 JSON：{"ok":1,"count":n,"active":"名字","models":[...]}。"""
        return self._call_string("OpYoloListModels")

    def yolo_list_devices(self) -> str:
        """可用设备 JSON：CPU 恒在（index=-1），有 DirectML 时附 GPU 列表 + dml_available。"""
        return self._call_string("OpYoloListDevices")

    def yolo_encrypt_model(
        self, src_path: str | Path, dst_path: str | Path, password: str
    ) -> bool:
        """把 .onnx 封装成 OPYMOD01 加密模型包（AES-256-GCM）；密码为空或读失败返回 False。"""
        return self._call_ok(
            "OpYoloEncryptModel", str(src_path), str(dst_path), password
        )

    def yolo_decrypt_model(
        self, src_path: str | Path, dst_path: str | Path, password: str
    ) -> bool:
        """把 OPYMOD01 包解回明文 .onnx；密码错或非密封包返回 False 且不留下半个文件。"""
        return self._call_ok(
            "OpYoloDecryptModel", str(src_path), str(dst_path), password
        )

    def set_dict(self, idx: int, file_name: str | Path) -> bool:
        return self._call_ok("OpSetDict", int(idx), str(file_name))

    def get_dict(self, idx: int, font_index: int) -> str:
        return self._call_string("OpGetDict", int(idx), int(font_index))

    def set_mem_dict(self, idx: int, data: str | bytes | bytearray | memoryview | int, size: int | None = None) -> bool:
        self._check_open()
        if isinstance(data, int):
            if size is None:
                raise ValueError("size is required when data is a pointer")
            ptr = ctypes.c_void_p(data)
            data_size = int(size)
        else:
            raw = data.encode(locale.getpreferredencoding(False)) if isinstance(data, str) else bytes(data)
            buffer = ctypes.create_string_buffer(raw)
            ptr = ctypes.cast(buffer, ctypes.c_void_p)
            data_size = len(raw) if size is None else int(size)
        ok = self._dll.OpSetMemDict(self._handle, int(idx), ptr, data_size)
        return self._ok(ok, "OpSetMemDict")

    def use_dict(self, idx: int) -> bool:
        return self._call_ok("OpUseDict", int(idx))

    def add_dict(self, idx: int, dict_info: str) -> int:
        """向指定字库添加字条，返回成功处理的行数（0 = 整批失败）。

        ``dict_info`` 支持多行（换行分隔，每行一条 ``名字$高,宽,点数$点阵HEX``）。
        **整批原子**：任一行解析失败则一条都不写入并返回 0。
        返回值 >0 在布尔上下文里同样成立，旧代码无需改动。
        """
        return self._call_int("OpAddDict", int(idx), dict_info)

    def remove_dict(self, idx: int, dict_info: str) -> bool:
        """按点阵从字库删除一条字条，返回是否删除成功。

        字条靠点阵唯一，``dict_info`` 只取点阵部分（名字可留空）。
        做字库时发现个别字标错，用它删单条即可，不必 clear_dict 整槽重来。

        注意：构造时 ``raise_on_error=True``（默认）下，点阵不存在会抛
        :class:`OpCallError` 而不是返回 False —— 与本类其它 dict_* 方法的
        0=失败口径一致。需要"删不到也算成功"的幂等语义时传 ``raise_on_error=False``。
        """
        return self._call_ok("OpRemoveDict", int(idx), dict_info)

    def save_dict(self, idx: int, file_name: str | Path) -> bool:
        return self._call_ok("OpSaveDict", int(idx), str(file_name))

    def clear_dict(self, idx: int) -> bool:
        return self._call_ok("OpClearDict", int(idx))

    def get_dict_count(self, idx: int) -> int:
        return self._call_int("OpGetDictCount", int(idx))

    def get_now_dict(self) -> int:
        return self._call_int("OpGetNowDict")

    def set_binary_preprocess(
        self,
        mode: int,
        isolated_threshold: int = 1,
        min_component_area: int = 2,
        bridge_gap: int = 0,
    ) -> bool:
        return self._call_ok(
            "OpSetBinaryPreprocess",
            int(mode),
            int(isolated_threshold),
            int(min_component_area),
            int(bridge_gap),
        )

    def get_binary_preprocess(self) -> tuple[int, int, int, int]:
        self._check_open()
        mode = ctypes.c_int()
        isolated_threshold = ctypes.c_int()
        min_component_area = ctypes.c_int()
        bridge_gap = ctypes.c_int()
        ok = self._dll.OpGetBinaryPreprocess(
            self._handle,
            ctypes.byref(mode),
            ctypes.byref(isolated_threshold),
            ctypes.byref(min_component_area),
            ctypes.byref(bridge_gap),
        )
        self._ok(ok, "OpGetBinaryPreprocess")
        return int(mode.value), int(isolated_threshold.value), int(min_component_area.value), int(bridge_gap.value)

    def fetch_word(self, x1: int, y1: int, x2: int, y2: int, color: str, word: str) -> str:
        return self._call_string("OpFetchWord", int(x1), int(y1), int(x2), int(y2), color, word)

    def fetch_word_ex(self, x1: int, y1: int, x2: int, y2: int, color: str, sim: float, word: str) -> str:
        return self._call_string("OpFetchWordEx", int(x1), int(y1), int(x2), int(y2), color, float(sim), word)

    def extract_word_rects(self, x1: int, y1: int, x2: int, y2: int, color: str, sim: float, min_word_h: int) -> str:
        return self._call_string(
            "OpExtractWordRects",
            int(x1),
            int(y1),
            int(x2),
            int(y2),
            color,
            float(sim),
            int(min_word_h),
        )

    def extract_word_rects_ex(
        self,
        x1: int,
        y1: int,
        x2: int,
        y2: int,
        color: str,
        sim: float,
        min_word_w: int,
        min_word_h: int,
        padding: int,
    ) -> str:
        return self._call_string(
            "OpExtractWordRectsEx",
            int(x1),
            int(y1),
            int(x2),
            int(y2),
            color,
            float(sim),
            int(min_word_w),
            int(min_word_h),
            int(padding),
        )

    def fetch_words(
        self,
        x1: int,
        y1: int,
        x2: int,
        y2: int,
        color: str,
        sim: float,
        words: str,
        min_word_h: int,
    ) -> str:
        return self._call_string(
            "OpFetchWords",
            int(x1),
            int(y1),
            int(x2),
            int(y2),
            color,
            float(sim),
            words,
            int(min_word_h),
        )

    def fetch_words_ex(
        self,
        x1: int,
        y1: int,
        x2: int,
        y2: int,
        color: str,
        sim: float,
        words: str,
        min_word_w: int,
        min_word_h: int,
        padding: int,
    ) -> str:
        return self._call_string(
            "OpFetchWordsEx",
            int(x1),
            int(y1),
            int(x2),
            int(y2),
            color,
            float(sim),
            words,
            int(min_word_w),
            int(min_word_h),
            int(padding),
        )

    def fetch_words_by_rects(
        self,
        x1: int,
        y1: int,
        x2: int,
        y2: int,
        color: str,
        sim: float,
        words: str,
        rects: str,
    ) -> str:
        return self._call_string(
            "OpFetchWordsByRects",
            int(x1),
            int(y1),
            int(x2),
            int(y2),
            color,
            float(sim),
            words,
            rects,
        )

    def get_binary_preview(
        self,
        x1: int,
        y1: int,
        x2: int,
        y2: int,
        color: str,
        sim: float,
    ) -> tuple[str, int]:
        return self._call_string_with_int(
            "OpGetBinaryPreview",
            int(x1),
            int(y1),
            int(x2),
            int(y2),
            color,
            float(sim),
        )

    def get_word_preview(self, dict_info: str) -> tuple[str, int]:
        return self._call_string_with_int("OpGetWordPreview", dict_info)

    def check_word_dict(self, dict_info: str) -> tuple[str, int]:
        return self._call_string_with_int("OpCheckWordDict", dict_info)

    def normalize_word_dict(self, dict_info: str) -> tuple[str, int]:
        return self._call_string_with_int("OpNormalizeWordDict", dict_info)

    def rename_word_dict(self, dict_info: str, words: str) -> tuple[str, int]:
        return self._call_string_with_int("OpRenameWordDict", dict_info, words)

    def get_words_no_dict(self, x1: int, y1: int, x2: int, y2: int, color: str) -> str:
        return self._call_string("OpGetWordsNoDict", int(x1), int(y1), int(x2), int(y2), color)

    def get_word_result_count(self, result: str) -> int:
        return self._call_int("OpGetWordResultCount", result)

    def get_word_result_pos(self, result: str, index: int) -> tuple[int, int, int]:
        return self._call_result_point("OpGetWordResultPos", result, int(index))

    def get_word_result_str(self, result: str, index: int) -> str:
        return self._call_string("OpGetWordResultStr", result, int(index))

    def ocr(self, x1: int, y1: int, x2: int, y2: int, color: str, sim: float) -> str:
        return self._call_string("OpOcr", int(x1), int(y1), int(x2), int(y2), color, float(sim))

    def ocr_ex(self, x1: int, y1: int, x2: int, y2: int, color: str, sim: float) -> str:
        return self._call_string("OpOcrEx", int(x1), int(y1), int(x2), int(y2), color, float(sim))

    def autoocr(self, x1: int, y1: int, x2: int, y2: int, color: str, sim: float) -> str:
        """免字库 OCR 归一入口（推荐）：``color`` 为空 → 原图直识别；非空 → 按颜色二值化。

        两分支分别与 :meth:`ocr_auto` / :meth:`ocr_auto_color` 完全一致
        （非空分支含字库协同：有字库走字库匹配，无字库走 ONNX）。
        """
        return self._call_string("OpAutoOcr", int(x1), int(y1), int(x2), int(y2), color, float(sim))

    def ocr_auto_color(self, x1: int, y1: int, x2: int, y2: int, color: str, sim: float) -> str:
        """区域截图后按颜色二值化再做免字库 OCR（置信度阈值 sim，默认 0.7）。"""
        return self._call_string("OpOcrAutoColor", int(x1), int(y1), int(x2), int(y2), color, float(sim))

    def ocr_auto_color_line(self, x1: int, y1: int, x2: int, y2: int, color: str, sim: float) -> str:
        """单行快模式 OcrAutoColor：颜色二值化后整图直接 rec（跳过检测）。

        适用于读出区/固定单行文本，速度显著快于 :meth:`ocr` / :meth:`ocr_auto_color`；
        区域须只含单行文本，否则结果语义由调用方承担。
        """
        return self._call_string("OpOcrAutoColorLine", int(x1), int(y1), int(x2), int(y2), color, float(sim))

    def ocr_auto_color_ex(self, x1: int, y1: int, x2: int, y2: int, color: str, sim: float) -> tuple[str, int]:
        """结构化 OcrAutoColor，返回 ``(items, count)``。

        ``items`` 格式 ``"x1,y1,x2,y2,conf,text|..."``，坐标为**客户区坐标**（相对绑定窗口客户区左上角，与
        :meth:`find_color` / :meth:`find_pic` 同口径；**不是**屏幕绝对坐标），
        ``conf`` 为置信度（0~1，两位小数）；``count`` 为命中行数。
        """
        return self._call_string_with_int(
            "OpOcrAutoColorEx", int(x1), int(y1), int(x2), int(y2), color, float(sim)
        )

    def ocr_auto_color_from_file(self, file_name: str | Path, color_format: str, sim: float) -> str:
        """文件版 OcrAutoColor：读取图片后按颜色二值化再做免字库 OCR。"""
        return self._call_string("OpOcrAutoColorFromFile", str(file_name), color_format, float(sim))

    def autoocr_line(self, x1: int, y1: int, x2: int, y2: int, color: str, sim: float) -> str:
        """.. deprecated:: 2026-10-15
        旧名别名，等同于 :meth:`ocr_auto_color_line`，新代码请用新名。"""
        return self.ocr_auto_color_line(x1, y1, x2, y2, color, sim)

    def autoocr_ex(self, x1: int, y1: int, x2: int, y2: int, color: str, sim: float) -> tuple[str, int]:
        """.. deprecated:: 2026-10-15
        旧名别名，等同于 :meth:`ocr_auto_color_ex`，新代码请用新名。"""
        return self.ocr_auto_color_ex(x1, y1, x2, y2, color, sim)

    def find_str(self, x1: int, y1: int, x2: int, y2: int, strs: str, color: str, sim: float) -> tuple[int, int, int]:
        return self._call_result_point("OpFindStr", int(x1), int(y1), int(x2), int(y2), strs, color, float(sim))

    def find_str_ex(self, x1: int, y1: int, x2: int, y2: int, strs: str, color: str, sim: float) -> str:
        return self._call_string("OpFindStrEx", int(x1), int(y1), int(x2), int(y2), strs, color, float(sim))

    def ocr_auto(self, x1: int, y1: int, x2: int, y2: int, sim: float) -> str:
        return self._call_string("OpOcrAuto", int(x1), int(y1), int(x2), int(y2), float(sim))

    def ocr_from_file(self, file_name: str | Path, color_format: str, sim: float) -> str:
        return self._call_string("OpOcrFromFile", str(file_name), color_format, float(sim))

    def ocr_auto_from_file(self, file_name: str | Path, sim: float) -> str:
        return self._call_string("OpOcrAutoFromFile", str(file_name), float(sim))

    def find_line(self, x1: int, y1: int, x2: int, y2: int, color: str, sim: float) -> str:
        return self._call_string("OpFindLine", int(x1), int(y1), int(x2), int(y2), color, float(sim))

    def find_line_ex(self, x1: int, y1: int, x2: int, y2: int, color: str, sim: float) -> tuple[str, int]:
        """查找直线并同时返回可信度。

        返回 ``(line, point_count)``：``line`` 为 ``"角度,距离"``（距离相对区域左上角），
        ``point_count`` 为该直线上的点数。``FindLine`` 无阈值、即便图中没有直线也会返回一条
        噪点拟合的"幻觉线"，因此应当用 ``point_count`` 自行判定：数值越大越可信，
        ``0`` 表示未截图成功或区域内没有匹配颜色的点。
        """
        return self._call_string_with_int(
            "OpFindLineEx", int(x1), int(y1), int(x2), int(y2), color, float(sim)
        )

    # Memory
    def read_data(self, address: str, size: int, hwnd: int = 0) -> str:
        return self._call_string("OpReadData", int(hwnd), address, int(size))

    def write_data(self, address: str, data: str | bytes | bytearray | memoryview, size: int | None = None, hwnd: int = 0) -> bool:
        hex_data, data_size = self._normalize_hex_data(data, size)
        return self._call_ok("OpWriteData", int(hwnd), address, hex_data, data_size)

    def read_int(self, address: str, int_type: IntType | int = IntType.I32, hwnd: int = 0) -> int:
        self._check_open()
        value = ctypes.c_int64()
        ok = self._dll.OpReadInt(self._handle, int(hwnd), address, int(int_type), ctypes.byref(value))
        self._ok(ok, "OpReadInt")
        return int(value.value)

    def write_int(self, address: str, value: int, int_type: IntType | int = IntType.I32, hwnd: int = 0) -> bool:
        return self._call_ok("OpWriteInt", int(hwnd), address, int(int_type), int(value))

    def read_float(self, address: str, hwnd: int = 0) -> float:
        self._check_open()
        value = ctypes.c_float()
        ok = self._dll.OpReadFloat(self._handle, int(hwnd), address, ctypes.byref(value))
        self._ok(ok, "OpReadFloat")
        return float(value.value)

    def write_float(self, address: str, value: float, hwnd: int = 0) -> bool:
        return self._call_ok("OpWriteFloat", int(hwnd), address, float(value))

    def read_double(self, address: str, hwnd: int = 0) -> float:
        self._check_open()
        value = ctypes.c_double()
        ok = self._dll.OpReadDouble(self._handle, int(hwnd), address, ctypes.byref(value))
        self._ok(ok, "OpReadDouble")
        return float(value.value)

    def write_double(self, address: str, value: float, hwnd: int = 0) -> bool:
        return self._call_ok("OpWriteDouble", int(hwnd), address, float(value))

    def read_string(
        self,
        address: str,
        string_type: StringType | int = StringType.ANSI,
        length: int = 0,
        hwnd: int = 0,
    ) -> str:
        return self._call_string("OpReadString", int(hwnd), address, int(string_type), int(length))

    def write_string(
        self,
        address: str,
        value: str,
        string_type: StringType | int = StringType.ANSI,
        hwnd: int = 0,
    ) -> bool:
        return self._call_ok("OpWriteString", int(hwnd), address, int(string_type), str(value))

    def find_data(self, addr_range: str, string: str, hwnd: int = 0) -> str:
        """特征码搜索内存，string 十六进制串支持 ?? 通配；返回 "addr|addr|..."。addr_range 空=全部可读区。"""
        return self._call_string("OpFindData", int(hwnd), addr_range, str(string))

    def find_data_ex(self, addr_range: str, string: str, step: int = 1, count: int = 0, hwnd: int = 0) -> str:
        """特征码搜索；step<=0 视为 1，count<=0 用默认上限 1024。"""
        return self._call_string("OpFindDataEx", int(hwnd), addr_range, str(string), int(step), int(count))

    def get_module_base_addr(self, module: str, hwnd: int = 0) -> str:
        """取目标进程模块基址（大写十六进制串）；失败返回空串。"""
        return self._call_string("OpGetModuleBaseAddr", int(hwnd), str(module))

    def get_module_size(self, module: str, hwnd: int = 0) -> int:
        """模块大小（字节）；失败/未找到返回 0。"""
        return self._call_int("OpGetModuleSize", int(hwnd), str(module))

    def get_module_info_list(self, hwnd: int = 0) -> str:
        """模块列表 JSON：{"ok":1,"modules":[{"name":..,"path":..,"base":"HEX","size":n},...]}。"""
        return self._call_string("OpGetModuleInfoList", int(hwnd))

    def virtual_protect_ex(self, hwnd: int, address: str, size: int, new_protect: int) -> int | None:
        """修改目标进程地址保护属性；成功返回原 PAGE_* 值，失败返回 None。"""
        self._check_open()
        old = ctypes.c_int(0)
        ret = self._dll.OpVirtualProtectEx(
            self._handle, int(hwnd), str(address), int(size), int(new_protect), ctypes.byref(old)
        )
        return int(old.value) if ret == 1 else None

    def virtual_alloc_ex(self, hwnd: int, address: str, size: int, alloc_type: int = 0, protect: int = 0) -> str:
        """在目标进程分配内存，返回分配地址大写十六进制串；失败返回空串。
        alloc_type/protect 传 0 用默认 MEM_COMMIT|MEM_RESERVE / PAGE_READWRITE。"""
        return self._call_string(
            "OpVirtualAllocEx", int(hwnd), str(address), int(size), int(alloc_type), int(protect)
        )

    def find_color_block_ex_s(
        self, x1: int, y1: int, x2: int, y2: int, color: str, sim: float, count: int, height: int, width: int,
        mode: int = 1,
    ) -> str:
        """颜色块增强版：mode=0 raw；mode=1 重合窗口聚类合并，每块只返回一个左上角坐标。"""
        return self._call_string(
            "OpFindColorBlockExS", int(x1), int(y1), int(x2), int(y2), str(color), float(sim), int(count),
            int(height), int(width), int(mode),
        )

    def find_line_ex_s(
        self, x1: int, y1: int, x2: int, y2: int, color: str, sim: float, min_points: int = 0,
    ) -> tuple[str, int]:
        """找线增强版：去噪 + min_points 阈值。返回 (retstr, 峰值点数)；未达阈值时 retstr 为空串。"""
        return self._call_string_with_int(
            "OpFindLineExS", int(x1), int(y1), int(x2), int(y2), str(color), float(sim), int(min_points)
        )

    # File system
    def file_exists(self, file_name: str | Path) -> bool:
        """文件/目录是否存在（存在返回 True）。"""
        return self._call_ok("OpFileExists", str(file_name))

    def create_folder(self, folder_name: str | Path) -> bool:
        """创建目录（支持正斜杠路径）。"""
        return self._call_ok("OpCreateFolder", str(folder_name))

    def delete_folder(self, folder_name: str | Path) -> bool:
        """删除目录。"""
        return self._call_ok("OpDeleteFolder", str(folder_name))

    def get_file_list(self, folder: str | Path, wildcard: str = "") -> str:
        """遍历目录返回全路径 "p1|p2|..."（wildcard 空=全部）。"""
        return self._call_string("OpGetFileList", str(folder), wildcard)

    def copy_file(self, src_file: str | Path, dst_file: str | Path) -> bool:
        return self._call_ok("OpCopyFile", str(src_file), str(dst_file))

    def move_file(self, src_file: str | Path, dst_file: str | Path) -> bool:
        return self._call_ok("OpMoveFile", str(src_file), str(dst_file))

    def delete_file(self, file_name: str | Path) -> bool:
        return self._call_ok("OpDeleteFile", str(file_name))

    def get_file_size(self, file_name: str | Path) -> tuple[int, int]:
        """返回 (size, ret)：size 为文件大小字节数（int64），ret 为成功标志 1/0。"""
        out_ret = ctypes.c_int(0)
        size = self._dll.OpGetFileSize(self._handle, str(file_name), ctypes.byref(out_ret))
        return int(size), int(out_ret.value)

    def read_file_string(self, file_name: str | Path) -> str:
        """读文本文件（UTF-8 带 BOM）；失败返回空串。"""
        return self._call_string("OpReadFileString", str(file_name))

    def write_string_to_file(self, file_name: str | Path, content: str) -> bool:
        """文本覆盖写入（UTF-8 带 BOM）。"""
        return self._call_ok("OpWriteStringToFile", str(file_name), content)

    def get_file_name(self, file_path: str | Path, flag: int) -> str:
        """路径解析：flag 0=文件名含扩展名 1=仅文件名 2=仅扩展名(含.) 3=目录部分。"""
        return self._call_string("OpGetFileName", str(file_path), int(flag))

    def to_absolute_path(self, path: str | Path) -> str:
        """相对路径转绝对路径（基于 SetPath 全局目录或进程目录）。"""
        return self._call_string("OpToAbsolutePath", str(path))

    # QR code
    def decode_qrcode(self, x1: int, y1: int, x2: int, y2: int) -> str:
        """屏幕区域解码二维码，返回文本；未识别返回空串。"""
        return self._call_string("OpDecodeQRCode", int(x1), int(y1), int(x2), int(y2))

    def decode_qrcode_from_file(self, file_name: str | Path) -> str:
        """图片文件解码二维码，返回文本；未识别/读图失败返回空串。"""
        return self._call_string("OpDecodeQRCodeFromFile", str(file_name))

    def create_qrcode(self, text: str, file_name: str | Path, size: int = 0) -> bool:
        """生成二维码保存为图片（size<=0 用 256）。"""
        return self._call_ok("OpCreateQRCode", text, str(file_name), int(size))

    # JSON
    def json_is_valid(self, json_text: str) -> bool:
        """校验文本是否合法 JSON（0 是正常答案，不抛异常）。"""
        return self._call_int("OpJsonIsValid", json_text) == 1

    def json_get_string(self, json_text: str, path: str) -> str:
        """取字符串值（路径语法 "a.b[0].c"，空路径=根）；失败/类型不符返回空串。"""
        return self._call_string("OpJsonGetString", json_text, path)

    def json_get_number(self, json_text: str, path: str) -> float | None:
        """取数值；成功返回 float，失败返回 None。"""
        self._check_open()
        value = ctypes.c_double(0.0)
        ret = self._dll.OpJsonGetNumber(self._handle, json_text, path, ctypes.byref(value))
        return float(value.value) if ret == 1 else None

    def json_get_bool(self, json_text: str, path: str) -> bool | None:
        """取布尔值；成功返回 bool，失败返回 None。"""
        self._check_open()
        value = ctypes.c_int(0)
        ret = self._dll.OpJsonGetBool(self._handle, json_text, path, ctypes.byref(value))
        return bool(value.value) if ret == 1 else None

    def json_get_size(self, json_text: str, path: str) -> int:
        """数组长度/对象键数；非集合或不存在返回 -1。"""
        return self._call_int("OpJsonGetSize", json_text, path)

    def json_get_keys(self, json_text: str, path: str) -> str:
        """对象键名序列化为 JSON 数组文本；非对象返回空串。"""
        return self._call_string("OpJsonGetKeys", json_text, path)

    def json_set_string(self, json_text: str, path: str, value: str) -> str:
        """写入字符串（自动创建中间节点），返回新 JSON 串；失败返回空串。"""
        return self._call_string("OpJsonSetString", json_text, path, value)

    def json_set_number(self, json_text: str, path: str, value: float) -> str:
        """写入数值，返回新 JSON 串；失败返回空串。"""
        return self._call_string("OpJsonSetNumber", json_text, path, float(value))

    def json_set_bool(self, json_text: str, path: str, value: bool) -> str:
        """写入布尔，返回新 JSON 串；失败返回空串。"""
        return self._call_string("OpJsonSetBool", json_text, path, 1 if value else 0)

    def json_append(self, json_text: str, path: str, raw_json: str) -> str:
        """数组追加元素（raw_json 为元素文本），返回新 JSON 串；失败返回空串。"""
        return self._call_string("OpJsonAppend", json_text, path, raw_json)

    def json_delete(self, json_text: str, path: str) -> str:
        """删除对象键或数组元素，返回新 JSON 串；失败返回空串。"""
        return self._call_string("OpJsonDelete", json_text, path)

    # HTTP
    def http_get(self, url: str, headers_json: str = "", timeout_ms: int = 0) -> str:
        """HTTP GET。headers_json 为 JSON 对象文本（如 {"Authorization":"Bearer sk-xxx"}）；
        timeout<=0 用 30000。非 2xx 也返回响应体；传输失败返回空串。"""
        return self._call_string("OpHttpGet", url, headers_json, int(timeout_ms))

    def http_post(self, url: str, body: str, headers_json: str = "", timeout_ms: int = 0) -> str:
        """HTTP POST（body 为原始文本，如 JSON 请求体）；其余语义同 http_get。"""
        return self._call_string("OpHttpPost", url, body, headers_json, int(timeout_ms))

    def http_download_file(self, url: str, file_name: str | Path, timeout_ms: int = 0) -> bool:
        """下载到文件（原始字节流式落盘）。"""
        return self._call_ok("OpHttpDownloadFile", url, str(file_name), int(timeout_ms))

    @staticmethod
    def _normalize_hex_data(data: str | bytes | bytearray | memoryview, size: int | None) -> tuple[str, int]:
        if isinstance(data, (bytes, bytearray, memoryview)):
            raw = bytes(data)
            return raw.hex().upper(), len(raw) if size is None else int(size)

        hex_data = "".join(str(data).split())
        if size is None:
            size = len(hex_data) // 2
        return hex_data, int(size)


    # ==== 2026-11 批次：图像句柄 / 内存直操作 / 音频 / OCR&FindStr 增强 / YOLO Ex2 ====
    #
    # 说明：这批接口此前只在 C API（op_c_api.h）与 COM 层有，Python 绑定漏了整整一批
    # （`scripts/api_surface_diff.py` 报 40 个缺口）。这里按既有约定补齐：
    #   返回字符串 -> _call_string；返回 1/0 -> _call_ok；
    #   "结果 + 出参" -> _call_result_point（返回 (ret, x, y)）/ _call_string_with_int（(str, count)）。

    # ---- 图像句柄注册表（img:n）----
    def load_image_mem(
        self,
        data: bytes | bytearray | memoryview,
        width: int,
        height: int,
        stride: int = 0,
        fmt: str = "bgra",
    ) -> str:
        """从原始像素注册图像句柄，返回 "img:n"；失败返回空串。

        fmt: bgra(默认)/bgr/gray（也接受 4/3/1）；stride<=0 表示紧凑排列。
        """
        return self._call_string(
            "OpLoadImageMem", bytes(data), int(width), int(height), int(stride), fmt
        )

    def load_image_bmp(self, data: bytes | bytearray | memoryview | str | Path) -> str:
        """从 BMP 字节流注册图像句柄；也可直接传 BMP 文件路径。返回 "img:n" 或空串。"""
        if isinstance(data, (str, Path)):
            raw = Path(data).read_bytes()
        else:
            raw = bytes(data)
        return self._call_string("OpLoadImageBmp", raw, len(raw))

    def free_image(self, image_handle: str) -> bool:
        """释放单个图像句柄。句柄不存在返回 False（raise_on_error=False 时）。"""
        return self._call_ok("OpFreeImage", image_handle)

    def free_all_images(self) -> int:
        """释放全部图像句柄，返回被释放的数量。"""
        return self._call_int("OpFreeAllImages")

    def get_image_count(self) -> int:
        """当前存活的图像句柄数量。"""
        return self._call_int("OpGetImageCount")

    def get_image_size(self, image_handle: str) -> tuple[int, int, int]:
        """返回 (width, height, channels)；channels 恒为 4（内部统一 BGRA）。"""
        self._check_open()
        width = ctypes.c_int()
        height = ctypes.c_int()
        channels = ctypes.c_int()
        ok = self._dll.OpGetImageSize(
            self._handle, image_handle, ctypes.byref(width), ctypes.byref(height), ctypes.byref(channels)
        )
        self._ok(ok, "OpGetImageSize")
        return int(width.value), int(height.value), int(channels.value)

    def get_image_data(self, image_handle: str, fmt: str = "raw") -> bytes:
        """读回像素。fmt: raw(BGRA 紧凑) / bmp(带 BMP 头)。失败返回 b""。"""
        width, height, _channels = self.get_image_size(image_handle)
        capacity = width * height * 4 + (54 if fmt.lower() == "bmp" else 0)
        if capacity <= 0:
            return b""
        buffer = ctypes.create_string_buffer(capacity)
        written = self._dll.OpGetImageData(self._handle, image_handle, fmt, buffer, capacity)
        written = int(written)
        if written <= 0:
            self._ok(0, "OpGetImageData")
            return b""
        return buffer.raw[:written]

    def crop_image(self, image_handle: str, x: int, y: int, width: int, height: int) -> str:
        """裁剪出新句柄，返回新的 "img:n"；失败返回空串。"""
        return self._call_string("OpCropImage", image_handle, int(x), int(y), int(width), int(height))

    def find_str_mem(self, image_handle: str, json_targets: str, sim: float = 0.9) -> str:
        """在图像句柄上找字。json_targets 是 JSON **数组**，元素为字符串或对象：
        ["主城"] 或 [{"text":"主城","match":"exact","id":"btn","fuzzy":1,"max_hits":1,"ci":true}]。
        返回 JSON 结果串 {"targets":N,"found":N,"hits":N,"lines":N,"results":[...]}。"""
        return self._call_string("OpFindStrMem", image_handle, json_targets, float(sim))

    # ---- OCR 结构化 / 耗时 ----
    def ocr_ex2(self, x1: int, y1: int, x2: int, y2: int, color: str, sim: float) -> str:
        """结构化 OCR（JSON：逐字坐标 + 置信度）。color 为空走免字库 ONNX。"""
        return self._call_string("OpOcrEx2", int(x1), int(y1), int(x2), int(y2), color, float(sim))

    def ocr_auto_color_ex2(
        self, x1: int, y1: int, x2: int, y2: int, color: str, sim: float
    ) -> tuple[str, int]:
        """按颜色二值化后的结构化 OCR，返回 (json, 命中行数)。"""
        return self._call_string_with_int(
            "OpOcrAutoColorEx2", int(x1), int(y1), int(x2), int(y2), color, float(sim)
        )

    def ocr_last_profile(self) -> str:
        """上一次 OCR 的分项耗时 JSON（det/rec/cls 各多少 ms）。"""
        return self._call_string("OpOcrLastProfile")

    def auto_ocr_from_file(self, file_name: str | Path, color_format: str, sim: float) -> str:
        """文件版免字库 OCR：color_format 为空=原图直识，非空=按颜色二值化。"""
        return self._call_string("OpAutoOcrFromFile", str(file_name), color_format, float(sim))

    def ocr_from_mem_ex2(
        self, image_handle: str, x1: int, y1: int, x2: int, y2: int, color: str, sim: float
    ) -> str:
        """内存版结构化 OCR。x1..y2 是**图像内坐标**；(0,0,0,0) 表示整张图。"""
        return self._call_string(
            "OpOcrFromMemEx2", image_handle, int(x1), int(y1), int(x2), int(y2), color, float(sim)
        )

    def ocr_auto_color_from_mem_ex2(
        self, image_handle: str, x1: int, y1: int, x2: int, y2: int, color: str, sim: float
    ) -> tuple[str, int]:
        """内存版颜色二值化结构化 OCR，返回 (json, 命中行数)。"""
        return self._call_string_with_int(
            "OpOcrAutoColorFromMemEx2", image_handle, int(x1), int(y1), int(x2), int(y2), color, float(sim)
        )

    # ---- FindStr 增强 ----
    def find_str_batch(
        self, x1: int, y1: int, x2: int, y2: int, json_targets: str, color: str, sim: float
    ) -> str:
        """多目标查找（JSON 目标列表 + 字符级坐标）。"""
        return self._call_string(
            "OpFindStrBatch", int(x1), int(y1), int(x2), int(y2), json_targets, color, float(sim)
        )

    def find_str_ex2(
        self,
        x1: int,
        y1: int,
        x2: int,
        y2: int,
        pattern: str,
        color: str,
        sim: float,
        flags: int = 0,
    ) -> str:
        """单目标 + flags（正则/精确/形近容错）。"""
        return self._call_string(
            "OpFindStrEx2", int(x1), int(y1), int(x2), int(y2), pattern, color, float(sim), int(flags)
        )

    # ---- 内存直操作二期：找图 ----
    def find_pic_from_mem(
        self,
        image_handle: str,
        x1: int,
        y1: int,
        x2: int,
        y2: int,
        files: str,
        delta_color: str,
        sim: float,
        direction: constants.FindDirection | int = constants.FindDirection.LEFT_TOP_TO_RIGHT_BOTTOM,
    ) -> tuple[int, int, int]:
        """在图像句柄里找图，返回 (图片序号, x, y)；-1 = 未找到（大漠索引语义）。"""
        return self._call_result_point(
            "OpFindPicFromMem",
            image_handle,
            int(x1),
            int(y1),
            int(x2),
            int(y2),
            files,
            delta_color,
            float(sim),
            int(direction),
        )

    def find_pic_ex_from_mem(
        self,
        image_handle: str,
        x1: int,
        y1: int,
        x2: int,
        y2: int,
        files: str,
        delta_color: str,
        sim: float,
        direction: constants.FindDirection | int = constants.FindDirection.LEFT_TOP_TO_RIGHT_BOTTOM,
    ) -> str:
        """在图像句柄里找图（全部命中，返回大漠坐标串 "x,y,index|x,y,index|..."，空串=未命中）。"""
        return self._call_string(
            "OpFindPicExFromMem",
            image_handle,
            int(x1),
            int(y1),
            int(x2),
            int(y2),
            files,
            delta_color,
            float(sim),
            int(direction),
        )

    # ---- 内存直操作二期：找色 / 取色 / 比色 / 计数 ----
    def find_color_from_mem(
        self,
        image_handle: str,
        x1: int,
        y1: int,
        x2: int,
        y2: int,
        color: str,
        sim: float,
        direction: constants.FindDirection | int = constants.FindDirection.LEFT_TOP_TO_RIGHT_BOTTOM,
    ) -> tuple[int, int, int]:
        """在图像句柄里找色，返回 (是否找到, x, y)。"""
        return self._call_result_point(
            "OpFindColorFromMem", image_handle, int(x1), int(y1), int(x2), int(y2), color,
            float(sim), int(direction),
        )

    def find_color_ex_from_mem(
        self,
        image_handle: str,
        x1: int,
        y1: int,
        x2: int,
        y2: int,
        color: str,
        sim: float,
        direction: constants.FindDirection | int = constants.FindDirection.LEFT_TOP_TO_RIGHT_BOTTOM,
    ) -> str:
        """在图像句柄里找色（全部命中，返回大漠坐标串 "x,y|x,y|..."，空串=未命中）。"""
        return self._call_string(
            "OpFindColorExFromMem", image_handle, int(x1), int(y1), int(x2), int(y2), color,
            float(sim), int(direction),
        )

    def get_color_from_mem(self, image_handle: str, x: int, y: int) -> str:
        """取图像句柄上某点颜色，返回 RGB hex（如 "FF8000"）。"""
        return self._call_string("OpGetColorFromMem", image_handle, int(x), int(y))

    def cmp_color_from_mem(
        self, image_handle: str, x: int, y: int, color: str, sim: float
    ) -> bool:
        """比对图像句柄上某点颜色是否匹配（带偏色段 "RRGGBB-DRDGDB"）。"""
        return self._call_ok("OpCmpColorFromMem", image_handle, int(x), int(y), color, float(sim))

    def get_color_num_from_mem(
        self, image_handle: str, x1: int, y1: int, x2: int, y2: int, color: str, sim: float
    ) -> int:
        """统计区域内符合颜色的像素数。"""
        return self._call_int(
            "OpGetColorNumFromMem", image_handle, int(x1), int(y1), int(x2), int(y2), color, float(sim)
        )

    def find_multi_color_from_mem(
        self,
        image_handle: str,
        x1: int,
        y1: int,
        x2: int,
        y2: int,
        first_color: str,
        offset_color: str,
        sim: float,
        direction: constants.FindDirection | int = constants.FindDirection.LEFT_TOP_TO_RIGHT_BOTTOM,
    ) -> tuple[int, int, int]:
        """多色查找，返回 (是否找到, x, y)。偏移串格式 "dx|dy|color"，段间英文逗号。"""
        return self._call_result_point(
            "OpFindMultiColorFromMem", image_handle, int(x1), int(y1), int(x2), int(y2),
            first_color, offset_color, float(sim), int(direction),
        )

    def find_multi_color_ex_from_mem(
        self,
        image_handle: str,
        x1: int,
        y1: int,
        x2: int,
        y2: int,
        first_color: str,
        offset_color: str,
        sim: float,
        direction: constants.FindDirection | int = constants.FindDirection.LEFT_TOP_TO_RIGHT_BOTTOM,
    ) -> str:
        """多色查找（全部命中，返回大漠坐标串 "x,y|x,y|..."，空串=未命中）。"""
        return self._call_string(
            "OpFindMultiColorExFromMem", image_handle, int(x1), int(y1), int(x2), int(y2),
            first_color, offset_color, float(sim), int(direction),
        )

    def find_color_block_ex_from_mem(
        self,
        image_handle: str,
        x1: int,
        y1: int,
        x2: int,
        y2: int,
        color: str,
        sim: float,
        count: int,
        height: int,
        width: int,
    ) -> str:
        """颜色块定位（滑动窗口语义：窗口内匹配像素 >= count 即命中）。"""
        return self._call_string(
            "OpFindColorBlockExFromMem", image_handle, int(x1), int(y1), int(x2), int(y2), color,
            float(sim), int(count), int(height), int(width),
        )

    # ---- YOLO Ex2 / 内存版 ----
    def yolo_detect_ex2(
        self,
        x1: int,
        y1: int,
        x2: int,
        y2: int,
        conf: float,
        iou: float,
        json_options: str = "",
    ) -> tuple[str, int]:
        """带过滤/截断的检测，返回 (json, 命中数)。json_options 形如 {"max_results":5}。"""
        return self._call_string_with_int(
            "OpYoloDetectEx2", int(x1), int(y1), int(x2), int(y2), float(conf), float(iou), json_options
        )

    def yolo_detect_from_file_ex2(
        self, file_name: str | Path, conf: float, iou: float, json_options: str = ""
    ) -> tuple[str, int]:
        """文件版 Ex2（可脱离绑定窗口用真实模型验证过滤/截断）。"""
        return self._call_string_with_int(
            "OpYoloDetectFromFileEx2", str(file_name), float(conf), float(iou), json_options
        )

    def yolo_detect_from_mem(
        self, image_handle: str, x1: int, y1: int, x2: int, y2: int, conf: float, iou: float
    ) -> tuple[str, int]:
        """内存版检测，返回 (json, 命中数)；坐标是图像内坐标。"""
        return self._call_string_with_int(
            "OpYoloDetectFromMem", image_handle, int(x1), int(y1), int(x2), int(y2),
            float(conf), float(iou),
        )

    def yolo_detect_from_mem_ex2(
        self,
        image_handle: str,
        x1: int,
        y1: int,
        x2: int,
        y2: int,
        conf: float,
        iou: float,
        json_options: str = "",
    ) -> tuple[str, int]:
        """内存版 + 过滤/截断，返回 (json, 命中数)。"""
        return self._call_string_with_int(
            "OpYoloDetectFromMemEx2", image_handle, int(x1), int(y1), int(x2), int(y2),
            float(conf), float(iou), json_options,
        )

    # ---- 音频音量（WASAPI 会话级 + 系统级）----
    def audio_query_window(self, hwnd: int) -> str:
        """按窗口查音频会话（JSON）。hwnd 传 0 表示当前绑定窗口。"""
        return self._call_string("OpAudioQueryWindow", int(hwnd))

    def audio_query_pid(self, pid: int) -> str:
        """按 PID 查音频会话（JSON：ok/code/found/muted/volume/peak）。"""
        return self._call_string("OpAudioQueryPid", int(pid))

    def audio_get_pid_from_hwnd(self, hwnd: int) -> int:
        """纯取窗口所属 PID（不碰 COM）；失败返回 0。"""
        return self._call_int("OpAudioGetPidFromHwnd", int(hwnd))

    def audio_get_system_volume(self) -> str:
        """系统主音量（JSON）。"""
        return self._call_string("OpAudioGetSystemVolume")

    def audio_set_session_volume(self, pid: int, volume: float) -> bool:
        """设置会话滑块音量（0.0~1.0，越界 clamp）。"""
        return self._call_ok("OpAudioSetSessionVolume", int(pid), float(volume))

    def audio_set_session_mute(self, pid: int, muted: bool) -> bool:
        """设置会话静音。"""
        return self._call_ok("OpAudioSetSessionMute", int(pid), 1 if muted else 0)

    def audio_set_system_volume(self, volume: float) -> bool:
        """设置系统主音量（0.0~1.0，越界 clamp）。"""
        return self._call_ok("OpAudioSetSystemVolume", float(volume))

    def audio_set_system_mute(self, muted: bool) -> bool:
        """设置系统静音。"""
        return self._call_ok("OpAudioSetSystemMute", 1 if muted else 0)
