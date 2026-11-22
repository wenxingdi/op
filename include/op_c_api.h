#pragma once

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#define OP_CALL __stdcall
#if defined(OP_C_API_STATIC)
#define OP_C_API
#elif defined(OP_C_API_EXPORTS)
#define OP_C_API __declspec(dllexport)
#else
#define OP_C_API __declspec(dllimport)
#endif
#else
#define OP_CALL
#if defined(OP_C_API_STATIC)
#define OP_C_API
#elif defined(OP_C_API_EXPORTS)
#define OP_C_API __attribute__((visibility("default")))
#else
#define OP_C_API
#endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct op_c_context op_c_context;
typedef op_c_context *op_handle;

// Lifecycle
OP_C_API op_handle OP_CALL OpCreate(void);
OP_C_API void OP_CALL OpDestroy(op_handle handle);

// Basic
OP_C_API const wchar_t *OP_CALL OpVer(void);
OP_C_API int OP_CALL OpSetPath(op_handle handle, const wchar_t *path);
OP_C_API const wchar_t *OP_CALL OpGetPath(op_handle handle);
OP_C_API const wchar_t *OP_CALL OpGetBasePath(op_handle handle);
OP_C_API int OP_CALL OpGetID(op_handle handle);
OP_C_API int OP_CALL OpGetLastError(op_handle handle);
OP_C_API int OP_CALL OpSetShowErrorMsg(op_handle handle, int show_type);
OP_C_API int OP_CALL OpSleep(op_handle handle, int millseconds);
OP_C_API int OP_CALL OpInjectDll(op_handle handle, const wchar_t *process_name, const wchar_t *dll_name);
OP_C_API int OP_CALL OpEnablePicCache(op_handle handle, int enable);
OP_C_API int OP_CALL OpClearPicCache(op_handle handle);
OP_C_API int OP_CALL OpSetPicCacheMax(op_handle handle, int max_count);
OP_C_API int OP_CALL OpSetAStarMap(op_handle handle, const wchar_t *bitmap_file, int scale, int offset_x, int offset_y);
OP_C_API int OP_CALL OpSetAStarMapData(op_handle handle, int width, int height, const unsigned char *bgra_data,
                                       int size, int scale, int offset_x, int offset_y);
OP_C_API const wchar_t *OP_CALL OpAStarFindPathBM(op_handle handle, int begin_x, int begin_y, int end_x, int end_y);
OP_C_API const wchar_t *OP_CALL OpAStarFindPathWay(op_handle handle, const wchar_t *points);
OP_C_API const wchar_t *OP_CALL OpFindNearestFeasiblePoint(op_handle handle, int x, int y);
OP_C_API const wchar_t *OP_CALL OpSmoothPathByLOS(op_handle handle, const wchar_t *path);
OP_C_API const wchar_t *OP_CALL OpSimplifyPath(op_handle handle, const wchar_t *path, double epsilon);
OP_C_API int OP_CALL OpIsLineBlocked(op_handle handle, int x1, int y1, int x2, int y2);
OP_C_API int OP_CALL OpFindNearestPathPoint(op_handle handle, const wchar_t *path, int x, int y, int *index, int *nx,
                                            int *ny);
OP_C_API int OP_CALL OpPointInPolygon(op_handle handle, const wchar_t *point, const wchar_t *polygon);
OP_C_API int OP_CALL OpCapturePre(op_handle handle, const wchar_t *file_name);
OP_C_API int OP_CALL OpSetScreenDataMode(op_handle handle, int mode);

// Algorithm
OP_C_API const wchar_t *OP_CALL OpAStarFindPath(op_handle handle, int map_width, int map_height,
                                                const wchar_t *disable_points, int begin_x, int begin_y, int end_x,
                                                int end_y);
OP_C_API const wchar_t *OP_CALL OpFindNearestPos(op_handle handle, const wchar_t *all_pos, int type, int x, int y);
OP_C_API const wchar_t *OP_CALL OpSortPosDistance(op_handle handle, const wchar_t *all_pos, int type, int x, int y);

// Window and process
OP_C_API const wchar_t *OP_CALL OpEnumWindow(op_handle handle, intptr_t parent, const wchar_t *title,
                                             const wchar_t *class_name, int filter);
OP_C_API const wchar_t *OP_CALL OpEnumWindowByProcess(op_handle handle, const wchar_t *process_name,
                                                      const wchar_t *title, const wchar_t *class_name, int filter);
OP_C_API const wchar_t *OP_CALL OpEnumProcess(op_handle handle, const wchar_t *name);
OP_C_API int OP_CALL OpClientToScreen(op_handle handle, intptr_t hwnd, int *x, int *y);
OP_C_API intptr_t OP_CALL OpFindWindow(op_handle handle, const wchar_t *class_name, const wchar_t *title);
OP_C_API intptr_t OP_CALL OpFindWindowByProcess(op_handle handle, const wchar_t *process_name,
                                                const wchar_t *class_name, const wchar_t *title);
OP_C_API intptr_t OP_CALL OpFindWindowByProcessId(op_handle handle, int process_id, const wchar_t *class_name,
                                                  const wchar_t *title);
OP_C_API intptr_t OP_CALL OpFindWindowEx(op_handle handle, intptr_t parent, const wchar_t *class_name,
                                         const wchar_t *title);
OP_C_API int OP_CALL OpGetClientRect(op_handle handle, intptr_t hwnd, int *x1, int *y1, int *x2, int *y2);
OP_C_API int OP_CALL OpGetClientSize(op_handle handle, intptr_t hwnd, int *width, int *height);
OP_C_API intptr_t OP_CALL OpGetForegroundFocus(op_handle handle);
OP_C_API intptr_t OP_CALL OpGetForegroundWindow(op_handle handle);
OP_C_API intptr_t OP_CALL OpGetMousePointWindow(op_handle handle);
OP_C_API intptr_t OP_CALL OpGetPointWindow(op_handle handle, int x, int y);
OP_C_API const wchar_t *OP_CALL OpGetProcessInfo(op_handle handle, int pid);
OP_C_API intptr_t OP_CALL OpGetSpecialWindow(op_handle handle, int flag);
OP_C_API intptr_t OP_CALL OpGetWindow(op_handle handle, intptr_t hwnd, int flag);
OP_C_API const wchar_t *OP_CALL OpGetWindowClass(op_handle handle, intptr_t hwnd);
OP_C_API int OP_CALL OpGetWindowProcessId(op_handle handle, intptr_t hwnd);
OP_C_API const wchar_t *OP_CALL OpGetWindowProcessPath(op_handle handle, intptr_t hwnd);
OP_C_API int OP_CALL OpGetWindowRect(op_handle handle, intptr_t hwnd, int *x1, int *y1, int *x2, int *y2);
OP_C_API int OP_CALL OpGetWindowState(op_handle handle, intptr_t hwnd, int flag);
OP_C_API const wchar_t *OP_CALL OpGetWindowTitle(op_handle handle, intptr_t hwnd);
OP_C_API int OP_CALL OpMoveWindow(op_handle handle, intptr_t hwnd, int x, int y);
OP_C_API int OP_CALL OpScreenToClient(op_handle handle, intptr_t hwnd, int *x, int *y);
OP_C_API int OP_CALL OpSendPaste(op_handle handle, intptr_t hwnd);
OP_C_API int OP_CALL OpSetClientSize(op_handle handle, intptr_t hwnd, int width, int height);
OP_C_API int OP_CALL OpSetWindowState(op_handle handle, intptr_t hwnd, int flag);
OP_C_API int OP_CALL OpSetWindowSize(op_handle handle, intptr_t hwnd, int width, int height);
OP_C_API int OP_CALL OpLayoutWindows(op_handle handle, const wchar_t *hwnds, int layout_type, int columns,
                                     int start_x, int start_y, int gap_x, int gap_y, int size_mode,
                                     int window_width, int window_height, int anchor_mode);
OP_C_API int OP_CALL OpSetWindowText(op_handle handle, intptr_t hwnd, const wchar_t *title);
OP_C_API int OP_CALL OpSetWindowTransparent(op_handle handle, intptr_t hwnd, int trans);
OP_C_API int OP_CALL OpSendString(op_handle handle, intptr_t hwnd, const wchar_t *str);
OP_C_API int OP_CALL OpSendStringIme(op_handle handle, intptr_t hwnd, const wchar_t *str);
// 锁定/解锁窗口位置(守护线程 50ms 轮询回弹)。返回 1 成功,0 句柄非法。
OP_C_API int OP_CALL OpLockWindowPosition(op_handle handle, intptr_t hwnd, int enable);
// 锁定/解锁窗口尺寸。
OP_C_API int OP_CALL OpLockWindowSize(op_handle handle, intptr_t hwnd, int enable);
// 禁止/恢复窗口最大最小化按钮(恢复时还原锁定前原始样式)。
OP_C_API int OP_CALL OpDisableMinMax(op_handle handle, intptr_t hwnd, int enable);
// 设置指定窗口的输入法开关:enable=0 关闭,1 恢复。
OP_C_API int OP_CALL OpSetIme(op_handle handle, intptr_t hwnd, int enable);
// 运行可执行文件/快捷方式/文档/网址。mode：0=Shell启动+继承目录；1=Shell启动+程序所在目录；
// 2=CreateProcess+继承目录；3=CreateProcess+程序所在目录（启动器类游戏对工作目录敏感，推荐 1）
OP_C_API int OP_CALL OpRunApp(op_handle handle, const wchar_t *cmdline, int mode, uint32_t *pid);
OP_C_API int OP_CALL OpWinExec(op_handle handle, const wchar_t *cmdline, int cmdshow);
OP_C_API const wchar_t *OP_CALL OpGetCmdStr(op_handle handle, const wchar_t *cmd, int millseconds);
OP_C_API int OP_CALL OpSetClipboard(op_handle handle, const wchar_t *str);
OP_C_API const wchar_t *OP_CALL OpGetClipboard(op_handle handle);
OP_C_API int OP_CALL OpDelay(op_handle handle, int mis);
OP_C_API int OP_CALL OpDelays(op_handle handle, int mis_min, int mis_max);

// System misc
OP_C_API int OP_CALL OpGetScreenWidth(op_handle handle);
OP_C_API int OP_CALL OpGetScreenHeight(op_handle handle);
OP_C_API int OP_CALL OpGetScreenDepth(op_handle handle);
OP_C_API int OP_CALL OpGetDPI(op_handle handle);
OP_C_API const wchar_t *OP_CALL OpGetTime(op_handle handle);
OP_C_API int OP_CALL OpBeep(op_handle handle, int freq, int dur);
OP_C_API int OP_CALL OpGetRandomNumber(op_handle handle, int min, int max);
OP_C_API double OP_CALL OpGetRandomDouble(op_handle handle, double min, double max);
OP_C_API int OP_CALL OpGaiLu(op_handle handle, int p);
OP_C_API const wchar_t *OP_CALL OpGetMachineCode(op_handle handle);
OP_C_API int OP_CALL OpIsElevated(op_handle handle);
OP_C_API int OP_CALL OpEnableDebugPrivilege(op_handle handle);
OP_C_API const wchar_t *OP_CALL OpGetLastErrorString(op_handle handle);
OP_C_API const wchar_t *OP_CALL OpMd5(op_handle handle, const wchar_t *text);
OP_C_API const wchar_t *OP_CALL OpSha1(op_handle handle, const wchar_t *text);
OP_C_API const wchar_t *OP_CALL OpBase64Encode(op_handle handle, const wchar_t *text);
OP_C_API const wchar_t *OP_CALL OpBase64Decode(op_handle handle, const wchar_t *base64);

// ---- 并发诊断（批 A 2026-11-22）----
// 约定不变：**单个 op_handle 不是线程安全的**，多线程每线程一个 handle。
// 本接口把"同实例被多线程使用"变成可观测事实，返回 JSON：
//   {"id":1,"in_flight":0,"peak":3,"reentrant":12,"calls_total":880,
//    "image_count":4,"bound":0,"thread_safe":false}
// peak > 1 或 reentrant > 0 即说明脚本踩了非线程安全线（结果不再可复现）。
OP_C_API const wchar_t *OP_CALL OpConcurrencyInfo(op_handle handle);
OP_C_API int OP_CALL OpDrawRect(op_handle handle, int x1, int y1, int x2, int y2, const wchar_t *color, int width);
OP_C_API int OP_CALL OpDrawLine(op_handle handle, int x1, int y1, int x2, int y2, const wchar_t *color, int width);
OP_C_API int OP_CALL OpDrawText(op_handle handle, int x, int y, const wchar_t *text, const wchar_t *color,
                                int font_size);
OP_C_API int OP_CALL OpDrawClear(op_handle handle);
OP_C_API int OP_CALL OpDrawSetVisible(op_handle handle, int show);

/* ---- 绘制补齐（批 B 2026-11-22，对齐 OLA DrawGui 常用子集）----
 * 屏幕层：DrawCircle / DrawImage（进程级透明叠加层，屏幕坐标）。
 * 绑定层（*Ex）：坐标是**绑定窗口客户区**，与找色/鼠标同口径；画在独立透明窗口上，
 *   **不写目标 DX 后台缓冲 -> 不会出现在 DX 截图里**；窗口移动时跟随。
 *   未绑定窗口时返回 0（不是静默画到屏幕）。
 *   ⚠ 本批只支持描边：DrawRectEx 的 mode=0(填充) 会按描边绘制并在日志里说明。
 * DrawClear 清屏幕层；DrawClearBind 只清绑定层；DrawGetCount 分别返回两层图元数。
 */
OP_C_API int OP_CALL OpDrawCircle(op_handle handle, int x, int y, int radius, const wchar_t *color, int width);
OP_C_API int OP_CALL OpDrawImage(op_handle handle, int x, int y, const wchar_t *file_name, int alpha);
OP_C_API int OP_CALL OpDrawRectEx(op_handle handle, int x, int y, int width, int height, int mode, int line_width);
OP_C_API int OP_CALL OpDrawCircleEx(op_handle handle, int x, int y, int radius, int line_width);
OP_C_API int OP_CALL OpDrawTextEx(op_handle handle, int x, int y, const wchar_t *text, int font_size);
OP_C_API int OP_CALL OpDrawClearBind(op_handle handle);
OP_C_API int OP_CALL OpDrawGetCount(op_handle handle, int *screen_count, int *bind_count);

// Background binding
OP_C_API int OP_CALL OpBindWindow(op_handle handle, intptr_t hwnd, const wchar_t *display, const wchar_t *mouse,
                                  const wchar_t *keypad, int mode);
OP_C_API int OP_CALL OpBindWindowEx(op_handle handle, intptr_t display_hwnd, intptr_t input_hwnd,
                                    const wchar_t *display, const wchar_t *mouse, const wchar_t *keypad, int mode);
OP_C_API int OP_CALL OpUnBindWindow(op_handle handle);
OP_C_API int OP_CALL OpLockInput(op_handle handle, int lock);
// 绑定降载:每次截图成功后强制延时 rate 毫秒(0-100)。type 0=推荐 1=高效(实现相同)。解绑不重置。
OP_C_API int OP_CALL OpDownCpu(op_handle handle, int type, int rate);
// dx 输入通道开关：1=DirectInput 2=RawInput 4=窗口消息；attr=0 时 value 为完整掩码。
OP_C_API int OP_CALL OpSetDxAttr(op_handle handle, int attr, int value);
OP_C_API int OP_CALL OpGetDxAttr(op_handle handle);
OP_C_API intptr_t OP_CALL OpGetBindWindow(op_handle handle);
OP_C_API int OP_CALL OpIsBind(op_handle handle);
// 当前实际生效的图色模式名（"normal"/"gdi2"/"dx.d3d11"/"normal.wgc"…），未绑定返回空串。
// 返回值为共享缓冲，跨调用请先拷贝。
OP_C_API const wchar_t *OP_CALL OpGetDisplayMethod(op_handle handle);

// Mouse
OP_C_API int OP_CALL OpGetCursorPos(op_handle handle, int *x, int *y);
OP_C_API const wchar_t *OP_CALL OpGetCursorShape(op_handle handle);
// 当前鼠标光标图像：32bpp BMP 的 Base64（top-down BGRA 含 alpha）；失败返回空串。
OP_C_API const wchar_t *OP_CALL OpGetCursorImage(op_handle handle);
OP_C_API int OP_CALL OpMoveR(op_handle handle, int x, int y);
OP_C_API int OP_CALL OpMoveTo(op_handle handle, int x, int y);
OP_C_API const wchar_t *OP_CALL OpMoveToEx(op_handle handle, int x, int y, int w, int h);
OP_C_API int OP_CALL OpMoveToSmooth(op_handle handle, int x, int y, int duration);
OP_C_API const wchar_t *OP_CALL OpMoveToExSmooth(op_handle handle, int x, int y, int w, int h, int duration);
OP_C_API int OP_CALL OpMovePath(op_handle handle, const wchar_t *path, int duration);
OP_C_API int OP_CALL OpDragPath(op_handle handle, const wchar_t *path, int duration);
OP_C_API int OP_CALL OpSetMouseTrajectory(op_handle handle, int mode, int min_duration, int max_duration, int jitter,
                                          int start_delay, int end_delay);
OP_C_API int OP_CALL OpLeftClick(op_handle handle);
OP_C_API int OP_CALL OpLeftDoubleClick(op_handle handle);
OP_C_API int OP_CALL OpLeftDown(op_handle handle);
OP_C_API int OP_CALL OpLeftUp(op_handle handle);
OP_C_API int OP_CALL OpMiddleClick(op_handle handle);
OP_C_API int OP_CALL OpMiddleDoubleClick(op_handle handle);
OP_C_API int OP_CALL OpMiddleDown(op_handle handle);
OP_C_API int OP_CALL OpMiddleUp(op_handle handle);
OP_C_API int OP_CALL OpRightClick(op_handle handle);
OP_C_API int OP_CALL OpRightDoubleClick(op_handle handle);
OP_C_API int OP_CALL OpRightDown(op_handle handle);
OP_C_API int OP_CALL OpRightUp(op_handle handle);
OP_C_API int OP_CALL OpXButton1Click(op_handle handle);
OP_C_API int OP_CALL OpXButton1DoubleClick(op_handle handle);
OP_C_API int OP_CALL OpXButton1Down(op_handle handle);
OP_C_API int OP_CALL OpXButton1Up(op_handle handle);
OP_C_API int OP_CALL OpXButton2Click(op_handle handle);
OP_C_API int OP_CALL OpXButton2DoubleClick(op_handle handle);
OP_C_API int OP_CALL OpXButton2Down(op_handle handle);
OP_C_API int OP_CALL OpXButton2Up(op_handle handle);
OP_C_API int OP_CALL OpWheel(op_handle handle, int delta);
OP_C_API int OP_CALL OpHWheel(op_handle handle, int delta);
OP_C_API int OP_CALL OpWheelDown(op_handle handle);
OP_C_API int OP_CALL OpWheelUp(op_handle handle);
OP_C_API int OP_CALL OpSetMouseDelay(op_handle handle, const wchar_t *type, int delay);

// Keyboard
OP_C_API int OP_CALL OpGetKeyState(op_handle handle, int vk_code);
OP_C_API int OP_CALL OpKeyDown(op_handle handle, int vk_code);
OP_C_API int OP_CALL OpKeyDownChar(op_handle handle, const wchar_t *vk_code);
OP_C_API int OP_CALL OpKeyUp(op_handle handle, int vk_code);
OP_C_API int OP_CALL OpKeyUpChar(op_handle handle, const wchar_t *vk_code);
OP_C_API int OP_CALL OpWaitKey(op_handle handle, int vk_code, int time_out);
OP_C_API int OP_CALL OpKeyPress(op_handle handle, int vk_code);
OP_C_API int OP_CALL OpKeyPressChar(op_handle handle, const wchar_t *vk_code);
OP_C_API int OP_CALL OpSetKeypadDelay(op_handle handle, const wchar_t *type, int delay);
OP_C_API int OP_CALL OpKeyPressStr(op_handle handle, const wchar_t *key_str, int delay);

// Image and color
OP_C_API int OP_CALL OpCapture(op_handle handle, int x1, int y1, int x2, int y2, const wchar_t *file_name);
OP_C_API int OP_CALL OpCmpColor(op_handle handle, int x, int y, const wchar_t *color, double sim);
OP_C_API int OP_CALL OpFindColor(op_handle handle, int x1, int y1, int x2, int y2, const wchar_t *color,
                                 double sim, int dir, int *x, int *y);
OP_C_API const wchar_t *OP_CALL OpFindColorEx(op_handle handle, int x1, int y1, int x2, int y2,
                                              const wchar_t *color, double sim, int dir);
OP_C_API int OP_CALL OpFindMultiColor(op_handle handle, int x1, int y1, int x2, int y2,
                                      const wchar_t *first_color, const wchar_t *offset_color, double sim, int dir,
                                      int *x, int *y);
OP_C_API const wchar_t *OP_CALL OpFindMultiColorEx(op_handle handle, int x1, int y1, int x2, int y2,
                                                   const wchar_t *first_color, const wchar_t *offset_color,
                                                   double sim, int dir);
OP_C_API int OP_CALL OpFindPic(op_handle handle, int x1, int y1, int x2, int y2, const wchar_t *files,
                               const wchar_t *delta_color, double sim, int dir, int *x, int *y);
OP_C_API const wchar_t *OP_CALL OpFindPicEx(op_handle handle, int x1, int y1, int x2, int y2,
                                            const wchar_t *files, const wchar_t *delta_color, double sim, int dir);
OP_C_API const wchar_t *OP_CALL OpFindPicExS(op_handle handle, int x1, int y1, int x2, int y2,
                                             const wchar_t *files, const wchar_t *delta_color, double sim, int dir);
OP_C_API int OP_CALL OpFindColorBlock(op_handle handle, int x1, int y1, int x2, int y2, const wchar_t *color,
                                      double sim, int count, int height, int width, int *x, int *y);
OP_C_API const wchar_t *OP_CALL OpFindColorBlockEx(op_handle handle, int x1, int y1, int x2, int y2,
                                                   const wchar_t *color, double sim, int count, int height,
                                                   int width);
OP_C_API const wchar_t *OP_CALL OpGetColor(op_handle handle, int x, int y);
OP_C_API int OP_CALL OpGetColorNum(op_handle handle, int x1, int y1, int x2, int y2, const wchar_t *color,
                                   double sim);
OP_C_API int OP_CALL OpSetDisplayInput(op_handle handle, const wchar_t *mode);
// 测试专用：直接驱动 BindingSession::requestCapture 的 pic/mem 路径，用于验证越界防御（改动 A）。
// 返回 1=成功(已写入 out_pixel 左上角像素)；0=被越界检查拒绝；<0=mem_mode 设置失败。
OP_C_API int OP_CALL OpRequestCaptureForTest(const wchar_t *mem_mode, int x1, int y1, int w, int h,
                                             unsigned char out_pixel[4]);
OP_C_API int OP_CALL OpLoadPic(op_handle handle, const wchar_t *file_name);
OP_C_API int OP_CALL OpFreePic(op_handle handle, const wchar_t *file_name);
OP_C_API int OP_CALL OpLoadMemPic(op_handle handle, const wchar_t *file_name, void *data, int size);
OP_C_API int OP_CALL OpGetPicSize(op_handle handle, const wchar_t *pic_name, int *width, int *height);
OP_C_API uintptr_t OP_CALL OpGetScreenData(op_handle handle, int x1, int y1, int x2, int y2, int *ret);
OP_C_API uintptr_t OP_CALL OpGetScreenDataBmp(op_handle handle, int x1, int y1, int x2, int y2, int *size,
                                              int *ret);
OP_C_API void OP_CALL OpGetScreenFrameInfo(op_handle handle, int *frame_id, int *time);
// 获取绑定窗口当前帧率(采样约 1 秒)。未绑定/后端无帧返回 0。
OP_C_API int OP_CALL OpGetFPS(op_handle handle);
// 卡屏检测：区域连续 check_times 次截图（间隔 interval_ms）帧哈希全同=卡死返回 1，
// 有变化/截图失败返回 0。check_times<=0 默认 3（钳 2-20），interval_ms<=0 默认 500（钳 50-10000）。
OP_C_API int OP_CALL OpIsDisplayDead(op_handle handle, int x1, int y1, int x2, int y2, int check_times,
                                     int interval_ms);
OP_C_API const wchar_t *OP_CALL OpMatchPicName(op_handle handle, const wchar_t *pic_name);

// OpenCV
OP_C_API int OP_CALL OpCvLoadTemplate(op_handle handle, const wchar_t *name, const wchar_t *file_path);
OP_C_API int OP_CALL OpCvLoadMaskedTemplate(op_handle handle, const wchar_t *name, const wchar_t *template_path,
                                            const wchar_t *mask_path);
OP_C_API int OP_CALL OpCvRemoveTemplate(op_handle handle, const wchar_t *name);
OP_C_API int OP_CALL OpCvRemoveAllTemplates(op_handle handle);
OP_C_API int OP_CALL OpCvHasTemplate(op_handle handle, const wchar_t *name);
OP_C_API int OP_CALL OpCvGetTemplateCount(op_handle handle);
OP_C_API const wchar_t *OP_CALL OpCvGetAllTemplateNames(op_handle handle);
OP_C_API const wchar_t *OP_CALL OpCvGetOpenCvVersion(op_handle handle);
OP_C_API int OP_CALL OpCvLoadTemplateList(op_handle handle, const wchar_t *template_list);
OP_C_API int OP_CALL OpCvToGray(op_handle handle, const wchar_t *src_file, const wchar_t *dst_file);
OP_C_API int OP_CALL OpCvToBinary(op_handle handle, const wchar_t *src_file, const wchar_t *dst_file);
OP_C_API int OP_CALL OpCvToEdge(op_handle handle, const wchar_t *src_file, const wchar_t *dst_file);
OP_C_API int OP_CALL OpCvToOutline(op_handle handle, const wchar_t *src_file, const wchar_t *dst_file);
OP_C_API int OP_CALL OpCvDenoise(op_handle handle, const wchar_t *src_file, const wchar_t *dst_file);
OP_C_API int OP_CALL OpCvEqualize(op_handle handle, const wchar_t *src_file, const wchar_t *dst_file);
OP_C_API int OP_CALL OpCvCLAHE(op_handle handle, const wchar_t *src_file, const wchar_t *dst_file,
                               double clip_limit, int tile_grid_size);
OP_C_API int OP_CALL OpCvBlur(op_handle handle, const wchar_t *src_file, const wchar_t *dst_file,
                              const wchar_t *mode, int kernel_size);
OP_C_API int OP_CALL OpCvSharpen(op_handle handle, const wchar_t *src_file, const wchar_t *dst_file,
                                 double strength);
OP_C_API int OP_CALL OpCvCropValid(op_handle handle, const wchar_t *src_file, const wchar_t *dst_file);
// 旋转（角度制，正角逆时针）：90 倍数无损无白边；任意角扩画布白边。返回 1 成功。
OP_C_API int OP_CALL OpCvRotate(op_handle handle, const wchar_t *src_file, const wchar_t *dst_file, double angle_deg);
// 翻转：mode 0 上下 / 1 左右 / 2 双向（=旋转 180°）。返回 1 成功。
OP_C_API int OP_CALL OpCvFlip(op_handle handle, const wchar_t *src_file, const wchar_t *dst_file, int mode);
OP_C_API const wchar_t *OP_CALL OpCvConnectedComponents(op_handle handle, const wchar_t *src_file, double min_area);
OP_C_API const wchar_t *OP_CALL OpCvFindContours(op_handle handle, const wchar_t *src_file, double min_area);
OP_C_API int OP_CALL OpCvPreprocessPipeline(op_handle handle, const wchar_t *src_file, const wchar_t *dst_file,
                                            const wchar_t *pipeline);
OP_C_API int OP_CALL OpCvCrop(op_handle handle, const wchar_t *src_file, int x, int y, int width, int height,
                              const wchar_t *dst_file);
OP_C_API int OP_CALL OpCvResize(op_handle handle, const wchar_t *src_file, int width, int height,
                                const wchar_t *dst_file);
OP_C_API int OP_CALL OpCvThreshold(op_handle handle, const wchar_t *src_file, const wchar_t *dst_file,
                                   double threshold, double max_value, const wchar_t *mode);
OP_C_API int OP_CALL OpCvInRange(op_handle handle, const wchar_t *src_file, const wchar_t *dst_file,
                                 const wchar_t *color_space, const wchar_t *lower, const wchar_t *upper);
OP_C_API int OP_CALL OpCvMorphology(op_handle handle, const wchar_t *src_file, const wchar_t *dst_file,
                                    const wchar_t *mode, int kernel_size, int iterations);
OP_C_API int OP_CALL OpCvThin(op_handle handle, const wchar_t *src_file, const wchar_t *dst_file,
                              const wchar_t *mode);
OP_C_API const wchar_t *OP_CALL OpCvMatchTemplate(op_handle handle, int x, int y, int width, int height,
                                                  const wchar_t *template_name, double threshold, int dir,
                                                  int strip_mode, int method, int color_mode);
OP_C_API const wchar_t *OP_CALL OpCvMatchTemplateScale(op_handle handle, int x, int y, int width, int height,
                                                       const wchar_t *template_name, const wchar_t *scales,
                                                       double threshold, int method, int color_mode);
OP_C_API const wchar_t *OP_CALL OpCvMatchAnyTemplate(op_handle handle, int x, int y, int width, int height,
                                                     const wchar_t *template_names, double threshold, int dir,
                                                     int strip_mode, int method, int color_mode);
OP_C_API const wchar_t *OP_CALL OpCvMatchAllTemplates(op_handle handle, int x, int y, int width, int height,
                                                     const wchar_t *template_names, double threshold, int dir,
                                                     int strip_mode, int method, int color_mode);
OP_C_API const wchar_t *OP_CALL OpCvFeatureMatchTemplate(op_handle handle, int x, int y, int width, int height,
                                                        const wchar_t *template_name, double threshold);
OP_C_API const wchar_t *OP_CALL OpCvEdgeMatchTemplate(op_handle handle, int x, int y, int width, int height,
                                                     const wchar_t *template_name, double threshold);
OP_C_API const wchar_t *OP_CALL OpCvShapeMatchTemplate(op_handle handle, int x, int y, int width, int height,
                                                      const wchar_t *template_name, double threshold);
OP_C_API const wchar_t *OP_CALL OpCvMatchTemplateRot(op_handle handle, int x, int y, int width, int height,
                                                     const wchar_t *template_name, const wchar_t *angles,
                                                     double threshold, int method, int color_mode);

// OCR
OP_C_API int OP_CALL OpSetOcrEngine(op_handle handle, const wchar_t *path_of_engine, const wchar_t *dll_name,
                                    const wchar_t *argv);
// OCR 预热：触发引擎懒加载 + 一次小图推理。返回 1 引擎就绪，0 不可用。
OP_C_API int OP_CALL OpOcrWarmup(op_handle handle);
OP_C_API int OP_CALL OpSetYoloEngine(op_handle handle, const wchar_t *path_of_engine, const wchar_t *dll_name,
                                     const wchar_t *argv);
OP_C_API const wchar_t *OP_CALL OpYoloDetect(op_handle handle, int x1, int y1, int x2, int y2, double conf,
                                             double iou);

/* 【YOLO 增强 2026-11-21】带过滤/截断的检测。
 *
 * json_options（可为 nullptr/空串 = 不过滤）：
 *   {"class_ids":[0,2], "labels":["person"], "max_results":20}
 *   - class_ids 与 labels 同时给时取**并集**（满足任一即保留）
 *   - max_results 按 confidence **降序**截断（取最可信的 N 个）
 *
 * 返回 JSON：
 *   {"ok":1,"code":0,"total":3,"returned":2,
 *    "results":[{"index":0,"class_id":0,"label":"person",
 *                "bbox":[x1,y1,x2,y2],"confidence":0.91,
 *                "cx":60,"cy":120,"w":100,"h":200}]}
 *
 * 返回 nullptr = 参数非法（坏 JSON / 未知字段 / 类型错），与"没检测到"区分。
 * 坐标口径与 OpOcrEx2 / OpFindStrBatch 一致（截图偏移 + 客户区偏移）。
 */
OP_C_API const wchar_t *OP_CALL OpYoloDetectEx2(op_handle handle, int x1, int y1, int x2, int y2, double conf,
                                                 double iou, const wchar_t *json_options, int *find_count);
OP_C_API const wchar_t *OP_CALL OpYoloDetectFromFile(op_handle handle, const wchar_t *file_name, double conf,
                                                     double iou);
OP_C_API int OP_CALL OpSetDict(op_handle handle, int idx, const wchar_t *file_name);
OP_C_API const wchar_t *OP_CALL OpGetDict(op_handle handle, int idx, int font_index);
OP_C_API int OP_CALL OpSetMemDict(op_handle handle, int idx, const void *data, int size);
OP_C_API int OP_CALL OpUseDict(op_handle handle, int idx);
OP_C_API int OP_CALL OpAddDict(op_handle handle, int idx, const wchar_t *dict_info);
OP_C_API int OP_CALL OpSaveDict(op_handle handle, int idx, const wchar_t *file_name);
OP_C_API int OP_CALL OpClearDict(op_handle handle, int idx);
OP_C_API int OP_CALL OpGetDictCount(op_handle handle, int idx);
OP_C_API int OP_CALL OpGetNowDict(op_handle handle);
OP_C_API int OP_CALL OpSetBinaryPreprocess(op_handle handle, int mode, int isolated_threshold,
                                           int min_component_area, int bridge_gap);
OP_C_API int OP_CALL OpGetBinaryPreprocess(op_handle handle, int *mode, int *isolated_threshold,
                                           int *min_component_area, int *bridge_gap);
OP_C_API const wchar_t *OP_CALL OpFetchWord(op_handle handle, int x1, int y1, int x2, int y2,
                                            const wchar_t *color, const wchar_t *word);
OP_C_API const wchar_t *OP_CALL OpFetchWordEx(op_handle handle, int x1, int y1, int x2, int y2,
                                              const wchar_t *color, double sim, const wchar_t *word);
OP_C_API const wchar_t *OP_CALL OpExtractWordRects(op_handle handle, int x1, int y1, int x2, int y2,
                                                   const wchar_t *color, double sim, int min_word_h);
OP_C_API const wchar_t *OP_CALL OpExtractWordRectsEx(op_handle handle, int x1, int y1, int x2, int y2,
                                                     const wchar_t *color, double sim, int min_word_w,
                                                     int min_word_h, int padding);
OP_C_API const wchar_t *OP_CALL OpFetchWords(op_handle handle, int x1, int y1, int x2, int y2,
                                             const wchar_t *color, double sim, const wchar_t *words,
                                             int min_word_h);
OP_C_API const wchar_t *OP_CALL OpFetchWordsEx(op_handle handle, int x1, int y1, int x2, int y2,
                                               const wchar_t *color, double sim, const wchar_t *words,
                                               int min_word_w, int min_word_h, int padding);
OP_C_API const wchar_t *OP_CALL OpFetchWordsByRects(op_handle handle, int x1, int y1, int x2, int y2,
                                                    const wchar_t *color, double sim, const wchar_t *words,
                                                    const wchar_t *rects);
OP_C_API const wchar_t *OP_CALL OpGetBinaryPreview(op_handle handle, int x1, int y1, int x2, int y2,
                                                   const wchar_t *color, double sim, int *ret);
OP_C_API const wchar_t *OP_CALL OpGetWordPreview(op_handle handle, const wchar_t *dict_info, int *ret);
OP_C_API const wchar_t *OP_CALL OpCheckWordDict(op_handle handle, const wchar_t *dict_info, int *ret);
OP_C_API const wchar_t *OP_CALL OpNormalizeWordDict(op_handle handle, const wchar_t *dict_info, int *ret);
OP_C_API const wchar_t *OP_CALL OpRenameWordDict(op_handle handle, const wchar_t *dict_info,
                                                 const wchar_t *words, int *ret);
OP_C_API const wchar_t *OP_CALL OpGetWordsNoDict(op_handle handle, int x1, int y1, int x2, int y2,
                                                 const wchar_t *color);
OP_C_API int OP_CALL OpGetWordResultCount(op_handle handle, const wchar_t *result);
OP_C_API int OP_CALL OpGetWordResultPos(op_handle handle, const wchar_t *result, int index, int *x, int *y);
OP_C_API const wchar_t *OP_CALL OpGetWordResultStr(op_handle handle, const wchar_t *result, int index);
OP_C_API const wchar_t *OP_CALL OpOcr(op_handle handle, int x1, int y1, int x2, int y2, const wchar_t *color,
                                      double sim);
OP_C_API const wchar_t *OP_CALL OpOcrEx(op_handle handle, int x1, int y1, int x2, int y2, const wchar_t *color,
                                        double sim);
OP_C_API const wchar_t *OP_CALL OpOcrAutoColor(op_handle handle, int x1, int y1, int x2, int y2,
                                                const wchar_t *color, double sim);
/* 单行快模式 OcrAutoColor：颜色二值化后整图直接 rec（跳过检测），适用于读出区/固定单行文本 */
OP_C_API const wchar_t *OP_CALL OpOcrAutoColorLine(op_handle handle, int x1, int y1, int x2, int y2,
                                                   const wchar_t *color, double sim);
/* 结构化 OcrAutoColor：返回 "x1,y1,x2,y2,conf,text|..."（客户区坐标，相对绑定窗口客户区左上角），find_count 输出命中行数 */
OP_C_API const wchar_t *OP_CALL OpOcrAutoColorEx(op_handle handle, int x1, int y1, int x2, int y2,
                                                 const wchar_t *color, double sim, int *find_count);
/* 【P0】结构化 OCR（JSON）：逐字置信度 + 逐字坐标。返回 JSON 数组，每行：
 *   {"text","conf","is_partial",["conf_partial",]"box":[x1,y1,x2,y2],
 *    "partial_chars","chars":[{"ch","conf","x1","x2"}]}
 * 坐标为原图像素坐标（含入口放大自动除回）。
 * ⚠ conf（整行）与 chars[].conf（逐字）口径不同、不可互推。
 * ⚠ partial_chars=true：该行经倾斜旋转校正，逐字 x 已退化为整行框范围。
 * 语义同 OpOcrEx（有字库时走字库匹配，此时 chars 为空数组）。 */
OP_C_API const wchar_t *OP_CALL OpOcrEx2(op_handle handle, int x1, int y1, int x2, int y2,
                                         const wchar_t *color, double sim);
/* 【P0】同 OpOcrEx2，但先按 color 二值化（语义同 OpOcrAutoColorEx），find_count 输出命中行数。
 * ⚠ 颜色为 **RGB 顺序** `RRGGBB-DDDDDD`（红色写 e01111，不是 1111e0）；多色用 | 分隔取并集。
 * 智能选路：图中命中该色的像素占比 <0.1% 时自动回退原图直识（白字图传色也不会返回空）。 */
OP_C_API const wchar_t *OP_CALL OpOcrAutoColorEx2(op_handle handle, int x1, int y1, int x2, int y2,
                                                  const wchar_t *color, double sim, int *find_count);

/* ===== 图像句柄注册表（P4 2026-11-21，内存直操作一期地基）=====
 * 目的：让调用方把**自己手上已有的像素**直接交给 op，省掉"落盘再读盘"的往返
 * （该链路实测 50~200ms，内存注册 <5ms）。
 *
 * 句柄是字符串 "img:<n>"，与既有 "pic:<路径>" 的显示输入风格一致，
 * 因此在 C/COM/Python 各层都是普通字符串，客户端零学习成本。
 *
 * 约定：
 *  - 注册后像素**不可变**（句柄可被多线程并发读；要变体就新建句柄）
 *  - 注册表上限 256 条，达上限**拒绝新增并返回空串**（不淘汰既有句柄）
 *  - 非法输入（nullptr/0 尺寸/未知格式/超 64MB）一律返回空串或 0，不崩溃
 */

/* 从原始像素内存注册。fmt: "bgra"(默认)/"bgr"/"gray"（也接受 "4"/"3"/"1"）。
 * stride<=0 表示紧凑排列；stride>width*bpp 视为带行填充，逐行复制。
 * 返回句柄字符串；失败返回空串。 */
OP_C_API const wchar_t *OP_CALL OpLoadImageMem(op_handle handle, const void *data, int width, int height,
                                                int stride, const wchar_t *fmt);
/* 从 BMP 字节流注册（支持 24/32bpp、top-down/bottom-up、行填充）。返回句柄；失败空串。 */
OP_C_API const wchar_t *OP_CALL OpLoadImageBmp(op_handle handle, const void *data, int size);
/* 释放单个图像句柄。返回 1=成功，0=句柄不存在。 */
OP_C_API int OP_CALL OpFreeImage(op_handle handle, const wchar_t *image_handle);
/* 释放全部图像句柄。返回被释放的数量。 */
OP_C_API int OP_CALL OpFreeAllImages(op_handle handle);
/* 查询图像尺寸。channels 恒为 4（内部统一 BGRA）。返回 1=成功，0=句柄不存在。 */
OP_C_API int OP_CALL OpGetImageSize(op_handle handle, const wchar_t *image_handle, int *width, int *height,
                                    int *channels);
/* 读回像素到调用方缓冲区。fmt: "raw"(BGRA 紧凑)/"bmp"(带 BMP 头)。
 * 返回实际写出的字节数；缓冲区不足或句柄无效返回 0。
 * 提示：先调 OpGetImageSize 算好容量（raw = w*h*4，bmp = w*h*4 + 54）。 */
OP_C_API int OP_CALL OpGetImageData(op_handle handle, const wchar_t *image_handle, const wchar_t *fmt,
                                    void *buffer, int buffer_size);
/* 裁剪并注册为新句柄（深拷贝）。区域自动 clamp；空区域返回空串。 */
OP_C_API const wchar_t *OP_CALL OpCropImage(op_handle handle, const wchar_t *image_handle, int x, int y, int width,
                                             int height);
/* 在图像句柄上做多目标查找（复用 OpFindStrBatch 的匹配内核与 JSON 格式）。
 * 返回 JSON；句柄无效或 JSON 非法返回 nullptr。坐标从图像左上角算起。 */
OP_C_API const wchar_t *OP_CALL OpFindStrMem(op_handle handle, const wchar_t *image_handle,
                                              const wchar_t *json_targets, double sim);
/* 当前注册表条目数（诊断用）。 */
OP_C_API int OP_CALL OpGetImageCount(op_handle handle);
OP_C_API int OP_CALL OpFindStr(op_handle handle, int x1, int y1, int x2, int y2, const wchar_t *strs,
                               const wchar_t *color, double sim, int *x, int *y);
OP_C_API const wchar_t *OP_CALL OpFindStrEx(op_handle handle, int x1, int y1, int x2, int y2,
                                            const wchar_t *strs, const wchar_t *color, double sim);
/* 【P1 2026-11-21】FindStr 增强：多目标结构化查找（一次 det+rec，引擎内匹配）。
 *
 * json_targets 为 JSON 数组，元素可以是：
 *   - 字符串：  "击倒机关兽"            （等价于 {"text":"击倒机关兽"} 的 contains 目标）
 *   - 对象：    {"id":"hp","text":"909","match":"exact","fuzzy":0,"max_hits":0,
 *               "ci":false,"confirmed_only":false}
 *     match 取值 contains(默认) | exact | prefix | suffix | regex
 *     fuzzy 允许编辑距离 0..3（与 regex 互斥）；ci 忽略 ASCII 大小写；
 *     confirmed_only 只匹配未贴边裁切的行（is_partial=false）
 *     未知字段 / 未知 match 取值 / 类型错误 -> 整个调用返回 nullptr（不静默降级）
 *
 * 返回 JSON：
 *   {"targets":2,"found":1,"hits":1,"lines":29,
 *    "results":[{"id":"hp","text":"909","match":"exact","found":true,"count":1,
 *      "hits":[{"line":11,"x":57,"y":112,"x2":77,"y2":126,
 *               "cx1":57,"cx2":77,"char_precise":true,
 *               "ed":0,"exact":true,"partial":false,
 *               "begin":9,"end":12,"line_text":"击倒机关兽(909m)0/5"}]}]}
 *
 * ⚠ cx1/cx2 是**命中片段的字符级水平范围**（来自 P0 逐字坐标）；逐字不可用
 *   （旋转降级 / 已装载字库）时退化为整行范围，此时 char_precise=false，
 *   但 cx1/cx2 仍然是可用的合法值。
 * ⚠ x,y 是**所属行框的左上角**，与旧 OpFindStr/OpFindStrEx 返回的坐标语义完全一致
 *   （便于新旧结果互相校验）；命中片段自己的位置请看 cx1/cx2。
 * ⚠ 匹配**不跨行**：命中片段必须完整落在同一行内。
 * ⚠ color 语义与旧 OpFindStr 一致（有字库时用于二值化，无字库时原图直识）。
 */
OP_C_API const wchar_t *OP_CALL OpFindStrBatch(op_handle handle, int x1, int y1, int x2, int y2,
                                                const wchar_t *json_targets, const wchar_t *color,
                                                double sim);
/* 【P1】单目标增强查找（标量入参，易语言/脚本友好）。返回 JSON 形状与
 * OpFindStrBatch 的 results[0] 一致（顶层加 targets/found/hits/lines）。
 * flags 位：
 *   1   = regex            2 = exact（默认 contains）
 *   4   = ci（ASCII）      64 = 模糊 <=1      128 = 模糊 <=2
 *   256 = confirmed_only
 * 位 8/16/32 预留（prefix/suffix/normalize）。位 1/2/64/128 互斥，同时给多个返回 nullptr。
 */
OP_C_API const wchar_t *OP_CALL OpFindStrEx2(op_handle handle, int x1, int y1, int x2, int y2,
                                             const wchar_t *pattern, const wchar_t *color, double sim,
                                             int flags);
/* 【P3 2026-11-21】上一次 OCR 的分项耗时自省。返回 JSON：
 *   {"valid":true,"total_ms":139.7,"pre_ms":6.3,"det_ms":41.2,"cls_ms":2.1,
 *    "rec_ms":88.4,"post_ms":1.7,"boxes":14,"lines":14,"chars":142,
 *    "det_calls":1,"rec_calls":1,"cls_calls":1,"runs_failed":0,
 *    "upscale":true,"oscale":2,"rec_width":320}
 * 不触发新的 OCR（连续读两次数值不变）。valid=false = 还没跑过 OCR 或引擎不支持。
 * ⚠ 并发调用同一 Op 时，数值对应"任意一次"调用（不加锁，避免干扰计时本身）。
 */
OP_C_API const wchar_t *OP_CALL OpOcrLastProfile(op_handle handle);
OP_C_API const wchar_t *OP_CALL OpOcrAuto(op_handle handle, int x1, int y1, int x2, int y2, double sim);
OP_C_API const wchar_t *OP_CALL OpOcrFromFile(op_handle handle, const wchar_t *file_name,
                                              const wchar_t *color_format, double sim);
OP_C_API const wchar_t *OP_CALL OpOcrAutoColorFromFile(op_handle handle, const wchar_t *file_name,
                                                        const wchar_t *color_format, double sim);
OP_C_API const wchar_t *OP_CALL OpOcrAutoFromFile(op_handle handle, const wchar_t *file_name, double sim);
/* 免字库 OCR 归一入口（推荐）：color 为空 → 原图直识别；非空 → 按颜色二值化。行为同 OpOcrAuto/OpOcrAutoColor 两分支 */
OP_C_API const wchar_t *OP_CALL OpAutoOcr(op_handle handle, int x1, int y1, int x2, int y2, const wchar_t *color,
                                          double sim);
/* ---- 以下旧名为废弃别名，保留转发以兼容已有脚本，新代码请用 OpOcrAutoColor* / OpAutoOcr ----
 * 三个别名与 OpAutoOcr 同门面语义：color 为空 → 原图直识别；非空 → 按颜色二值化。
 * 注意 OpAutoOcrEx 空颜色时坐标格式为 "x,y,text|..."（转发 OpOcrEx），
 * 非空时为 "x1,y1,x2,y2,conf,text|..."（OpOcrAutoColorEx），两者不同。 */
OP_C_API const wchar_t *OP_CALL OpAutoOcrLine(op_handle handle, int x1, int y1, int x2, int y2,
                                              const wchar_t *color, double sim);
OP_C_API const wchar_t *OP_CALL OpAutoOcrEx(op_handle handle, int x1, int y1, int x2, int y2,
                                            const wchar_t *color, double sim, int *find_count);
OP_C_API const wchar_t *OP_CALL OpAutoOcrFromFile(op_handle handle, const wchar_t *file_name,
                                                 const wchar_t *color_format, double sim);
OP_C_API const wchar_t *OP_CALL OpFindLine(op_handle handle, int x1, int y1, int x2, int y2,
                                           const wchar_t *color, double sim);
/* 查找直线(扩展版): 返回"角度,距离"; point_count 输出该直线上的点数。
 * FindLine 永远返回一条"最强"直线(无阈值)，point_count 越大越可信；0 表示未截图或无前景点。*/
OP_C_API const wchar_t *OP_CALL OpFindLineEx(op_handle handle, int x1, int y1, int x2, int y2,
                                             const wchar_t *color, double sim, int *point_count);

// Memory
OP_C_API int OP_CALL OpWriteData(op_handle handle, intptr_t hwnd, const wchar_t *address, const wchar_t *data,
                                 int size);
OP_C_API const wchar_t *OP_CALL OpReadData(op_handle handle, intptr_t hwnd, const wchar_t *address, int size);
OP_C_API int OP_CALL OpReadInt(op_handle handle, intptr_t hwnd, const wchar_t *address, int type, int64_t *value);
OP_C_API int OP_CALL OpWriteInt(op_handle handle, intptr_t hwnd, const wchar_t *address, int type, int64_t value);
OP_C_API int OP_CALL OpReadFloat(op_handle handle, intptr_t hwnd, const wchar_t *address, float *value);
OP_C_API int OP_CALL OpWriteFloat(op_handle handle, intptr_t hwnd, const wchar_t *address, float value);
OP_C_API int OP_CALL OpReadDouble(op_handle handle, intptr_t hwnd, const wchar_t *address, double *value);
OP_C_API int OP_CALL OpWriteDouble(op_handle handle, intptr_t hwnd, const wchar_t *address, double value);
OP_C_API const wchar_t *OP_CALL OpReadString(op_handle handle, intptr_t hwnd, const wchar_t *address, int type,
                                             int len);
OP_C_API int OP_CALL OpWriteString(op_handle handle, intptr_t hwnd, const wchar_t *address, int type,
                                   const wchar_t *value);
// 特征码搜索：string 十六进制串支持 ?? 通配；addr_range "start-end" 十六进制，空=全部已提交可读区。
// 返回 "addr|addr|..."（大写十六进制）；count<=0 用默认上限 1024。
OP_C_API const wchar_t *OP_CALL OpFindData(op_handle handle, intptr_t hwnd, const wchar_t *addr_range,
                                           const wchar_t *string);
OP_C_API const wchar_t *OP_CALL OpFindDataEx(op_handle handle, intptr_t hwnd, const wchar_t *addr_range,
                                             const wchar_t *string, int step, int count);
// 取模块基址（大写十六进制串）；失败返回空串。
OP_C_API const wchar_t *OP_CALL OpGetModuleBaseAddr(op_handle handle, intptr_t hwnd, const wchar_t *module);
// 模块大小（字节）；失败/未找到返回 0。
OP_C_API long OP_CALL OpGetModuleSize(op_handle handle, intptr_t hwnd, const wchar_t *module);
// 模块列表 JSON：{"ok":1,"modules":[{"name":..,"path":..,"base":"HEX","size":n},...]}。
OP_C_API const wchar_t *OP_CALL OpGetModuleInfoList(op_handle handle, intptr_t hwnd);
// 修改地址保护属性；返回 1 成功 0 失败，old_protect 带回原 PAGE_* 值。
OP_C_API int OP_CALL OpVirtualProtectEx(op_handle handle, intptr_t hwnd, const wchar_t *address, long size,
                                        int new_protect, int *old_protect);
// 在目标进程分配内存，返回分配地址大写十六进制串；失败返回空。
// alloc_type/protect 传 0 用默认 MEM_COMMIT|MEM_RESERVE / PAGE_READWRITE。
OP_C_API const wchar_t *OP_CALL OpVirtualAllocEx(op_handle handle, intptr_t hwnd, const wchar_t *address, long size,
                                                 int alloc_type, int protect);
// 颜色块增强版：mode=0 raw；mode=1 重合窗口聚类合并，每块只返回一个左上角坐标。
OP_C_API const wchar_t *OP_CALL OpFindColorBlockExS(op_handle handle, int x1, int y1, int x2, int y2,
                                                    const wchar_t *color, double sim, int count, int height,
                                                    int width, int mode);
// 找线增强版：去噪 + min_points 阈值；未达阈值返回空串（点数值经 out_count 返回真实峰值）。
OP_C_API const wchar_t *OP_CALL OpFindLineExS(op_handle handle, int x1, int y1, int x2, int y2, const wchar_t *color,
                                              double sim, int min_points, int *out_count);

// ---- 文件模块（P1 新增 2026-10-15；ret=1 成功 0 失败，路径支持正斜杠）----
OP_C_API int OP_CALL OpFileExists(op_handle handle, const wchar_t *file_name);
OP_C_API int OP_CALL OpCreateFolder(op_handle handle, const wchar_t *folder_name);
OP_C_API int OP_CALL OpDeleteFolder(op_handle handle, const wchar_t *folder_name);
// 遍历返回全路径 "p1|p2|..."（wildcard 空=全部）
OP_C_API const wchar_t *OP_CALL OpGetFileList(op_handle handle, const wchar_t *folder, const wchar_t *wildcard);
OP_C_API int OP_CALL OpCopyFile(op_handle handle, const wchar_t *src_file, const wchar_t *dst_file);
OP_C_API int OP_CALL OpMoveFile(op_handle handle, const wchar_t *src_file, const wchar_t *dst_file);
OP_C_API int OP_CALL OpDeleteFile(op_handle handle, const wchar_t *file_name);
// 文件大小字节数经 out_ret 返回（int64）；函数返回值同 ret 语义
OP_C_API intptr_t OP_CALL OpGetFileSize(op_handle handle, const wchar_t *file_name, int *out_ret);
// 文本读写：UTF-8 带 BOM，覆盖写
OP_C_API const wchar_t *OP_CALL OpReadFileString(op_handle handle, const wchar_t *file_name);
OP_C_API int OP_CALL OpWriteStringToFile(op_handle handle, const wchar_t *file_name, const wchar_t *content);
// 路径解析：flag 0=文件名含扩展名 1=仅文件名 2=仅扩展名(含.) 3=目录部分
OP_C_API const wchar_t *OP_CALL OpGetFileName(op_handle handle, const wchar_t *file_path, int flag);
OP_C_API const wchar_t *OP_CALL OpToAbsolutePath(op_handle handle, const wchar_t *path);

// ---- 组合键（P1 新增 2026-10-15）：形如 "ctrl+alt+a" / "shift+f1" / "win+d" ----
OP_C_API int OP_CALL OpKeyPressHotkey(op_handle handle, const wchar_t *combo);

// ---- 窗口 DPI 缩放（P1 新增 2026-10-15）：两位小数字符串，失败返回空串 ----
OP_C_API const wchar_t *OP_CALL OpGetWindowScale(op_handle handle, intptr_t hwnd);

// ---- 进程终止（P1 新增 2026-10-15）----
OP_C_API int OP_CALL OpTerminateProcess(op_handle handle, int pid);
OP_C_API int OP_CALL OpTerminateProcessTree(op_handle handle, int pid);

// ---- 二维码（P1 新增 2026-10-15，OpenCV QRCodeDetector/Encoder 直桥）----
OP_C_API const wchar_t *OP_CALL OpDecodeQRCode(op_handle handle, int x1, int y1, int x2, int y2);
OP_C_API const wchar_t *OP_CALL OpDecodeQRCodeFromFile(op_handle handle, const wchar_t *file_name);
// 生成二维码保存为图片（size<=0 用 256）
OP_C_API int OP_CALL OpCreateQRCode(op_handle handle, const wchar_t *text, const wchar_t *file_name, int size);

// ---- JSON 族（P3 第一批新增 2026-10-16）：无句柄路径直取式，解析 OP 自身 JSON 返回值/用户数据 ----
// 路径语法 "a.b[0].c"（兼容 "a.b.0"），空路径 = 根。失败约定：取值类返 0/空串，写类返回空串。
// 校验文本是否合法 JSON
OP_C_API int OP_CALL OpJsonIsValid(op_handle handle, const wchar_t *json_text);
// 取字符串值；失败/类型不符返回空串
OP_C_API const wchar_t *OP_CALL OpJsonGetString(op_handle handle, const wchar_t *json_text, const wchar_t *path);
// 取数值；返回值 1 成功 0 失败
OP_C_API int OP_CALL OpJsonGetNumber(op_handle handle, const wchar_t *json_text, const wchar_t *path, double *value);
// 取布尔值（value 0/1）；返回值 1 成功 0 失败
OP_C_API int OP_CALL OpJsonGetBool(op_handle handle, const wchar_t *json_text, const wchar_t *path, int *value);
// 数组长度/对象键数；非集合或不存在返回 -1
OP_C_API int OP_CALL OpJsonGetSize(op_handle handle, const wchar_t *json_text, const wchar_t *path);
// 对象键名序列化为 JSON 数组文本；非对象返回空串
OP_C_API const wchar_t *OP_CALL OpJsonGetKeys(op_handle handle, const wchar_t *json_text, const wchar_t *path);
// 写入路径（自动创建中间节点），返回新 JSON 串；失败返回空串
OP_C_API const wchar_t *OP_CALL OpJsonSetString(op_handle handle, const wchar_t *json_text, const wchar_t *path,
                                                const wchar_t *value);
OP_C_API const wchar_t *OP_CALL OpJsonSetNumber(op_handle handle, const wchar_t *json_text, const wchar_t *path,
                                                double value);
OP_C_API const wchar_t *OP_CALL OpJsonSetBool(op_handle handle, const wchar_t *json_text, const wchar_t *path,
                                              int value);
// 数组追加元素（raw_json 为元素文本）；失败返回空串
OP_C_API const wchar_t *OP_CALL OpJsonAppend(op_handle handle, const wchar_t *json_text, const wchar_t *path,
                                             const wchar_t *raw_json);
// 删除对象键或数组元素，返回新 JSON 串；失败返回空串
OP_C_API const wchar_t *OP_CALL OpJsonDelete(op_handle handle, const wchar_t *json_text, const wchar_t *path);

// ---- HTTP 小族（P3 第二项新增 2026-10-16，WinHTTP 系统直连）：对接大模型 API 用 ----
// headers_json 为 JSON 对象文本（如 {"Authorization":"Bearer sk-xxx"}）；timeout<=0 用 30000。
// 非 2xx 也返回响应体（错误 JSON 可见）；传输失败返回空串。
OP_C_API const wchar_t *OP_CALL OpHttpGet(op_handle handle, const wchar_t *url, const wchar_t *headers_json,
                                          int timeout_ms);
OP_C_API const wchar_t *OP_CALL OpHttpPost(op_handle handle, const wchar_t *url, const wchar_t *body,
                                           const wchar_t *headers_json, int timeout_ms);
// 下载到文件（原始字节流式落盘），1 成功 0 失败
OP_C_API int OP_CALL OpHttpDownloadFile(op_handle handle, const wchar_t *url, const wchar_t *file_name,
                                        int timeout_ms);

/* 【YOLO 增强 2026-11-21】文件版 Ex2：从磁盘图片检测并支持过滤/截断。
 * 存在的理由：OpYoloDetectEx2 依赖绑定窗口截图，无窗口环境下无法验证过滤逻辑；
 * 文件版让过滤/截断/JSON 契约可以用真实模型闭环验证。
 * 参数与返回 JSON 同 OpYoloDetectEx2；find_count 输出过滤后条数。 */
OP_C_API const wchar_t *OP_CALL OpYoloDetectFromFileEx2(op_handle handle, const wchar_t *file_name, double conf,
                                                        double iou, const wchar_t *json_options, int *find_count);

/* ---- YOLO 任务维度（批 C 2026-11-22）----
 * 原来只有"检测"。这里补 分类 / 实例分割 两类任务（Pose/Obb 未做，见 docs/2026-11/）。
 *
 * 任务由**模型**决定：SetYoloEngine 加载时先读 ONNX metadata 的 task 字段，
 * 读不到再按输出形状推断。因此：
 *   * 返回值 -1 = **模型不支持该任务**（比如拿检测模型调 Segment）—— 不是"没结果"；
 *   * 返回值  0 = 任务支持但**没有结果**；
 *   * 返回值 >0 = 结果条数（分类为候选数，分割为目标数）。
 * 调用方必须把 -1 和 0 分开处理，混用会掩盖"模型选错了"这类问题。
 *
 * YoloClassify：整图分类，top_k<=0 取 5。JSON 按 score 降序：
 *   {"ok":1,"code":0,"task":"classify","returned":K,
 *    "results":[{"class_id":2,"label":"charlie","score":0.71}, ...]}
 *
 * YoloSegment：实例分割。polygon 是掩码**外轮廓**（原图像素坐标，与 bbox 同口径；
 * 可能为空数组 = 掩码面积过小被判为噪声）。mask_area 是原图尺度的掩码像素面积：
 *   {"ok":1,"code":0,"task":"segment","total":N,"returned":N,
 *    "results":[{"class_id":0,"label":"blob","bbox":[x1,y1,x2,y2],"confidence":0.9,
 *                "mask_area":64.0,"polygon":[[x,y],...]}]}
 *
 * YoloGetModelInfo：当前已加载模型的元信息（未加载时 task="none"），
 * 用来在脚本启动时自检"模型选对了没"：
 *   {"ok":1,"task":"segment","task_code":2,"input_width":640,"input_height":640}
 */
OP_C_API const wchar_t *OP_CALL OpYoloClassify(op_handle handle, int x1, int y1, int x2, int y2, int top_k,
                                              int *find_count);
OP_C_API const wchar_t *OP_CALL OpYoloClassifyFromFile(op_handle handle, const wchar_t *file_name, int top_k,
                                                       int *find_count);
OP_C_API const wchar_t *OP_CALL OpYoloSegment(op_handle handle, int x1, int y1, int x2, int y2, double conf, double iou,
                                              int *find_count);
OP_C_API const wchar_t *OP_CALL OpYoloSegmentFromFile(op_handle handle, const wchar_t *file_name, double conf,
                                                      double iou, int *find_count);
OP_C_API const wchar_t *OP_CALL OpYoloGetModelInfo(op_handle handle);

/* ===== 音频音量（2026-11-21，WASAPI 会话级 + 系统级）=====
 * 设计见 docs/2026-11/音量模块_方案_20261121.md。
 *
 * 语义区分（误用率最高）：
 *   volume（滑块值）与出声大小无直接关系；判断"有没有声音"要看 peak。
 *   found=false = 该 PID 无音频会话，不等于没声音。
 *
 * 返回 JSON：{"ok":1,"code":0,"found":true,"pid":123,"muted":false,"playing":true,
 *            "volume":0.75,"peak":0.34,"error":""}
 * code: 0 成功 / -1 无音频设备 / -2 COM 或激活失败 / -3 枚举失败 / -4 参数非法 / -5 设置失败
 *
 * 无状态（DLL 不缓存 COM 指针，每次重新枚举），峰值保持留给调用方。
 */
OP_C_API const wchar_t *OP_CALL OpAudioQueryWindow(op_handle handle, long long hwnd);
OP_C_API const wchar_t *OP_CALL OpAudioQueryPid(op_handle handle, int pid);
/* 纯取窗口所属 PID（不碰 COM）。失败返回 0。 */
OP_C_API int OP_CALL OpAudioGetPidFromHwnd(op_handle handle, long long hwnd);
/* 设置会话音量/静音。返回 0 成功，负数为错误码（见上）。 */
OP_C_API int OP_CALL OpAudioSetSessionVolume(op_handle handle, int pid, double volume);
OP_C_API int OP_CALL OpAudioSetSessionMute(op_handle handle, int pid, int muted);
/* 系统主音量 / 静音。 */
OP_C_API const wchar_t *OP_CALL OpAudioGetSystemVolume(op_handle handle);
OP_C_API int OP_CALL OpAudioSetSystemVolume(op_handle handle, double volume);
OP_C_API int OP_CALL OpAudioSetSystemMute(op_handle handle, int muted);

/* ===== 内存直操作二期：识别入口的内存版（2026-11-21）=====
 * 设计见 docs/2026-11/内存直操作二期_全家桶_方案_20261121.md
 *
 * 与屏幕版的唯一区别是"图从哪来"：
 *   屏幕版 x1..y2 是客户区坐标；内存版 x1..y2 是**图像内坐标**（相对 img: 句柄左上角）。
 *   其余参数逐字相同；出参坐标也是图像内坐标。
 *   区域传 (0,0,0,0) 表示**整张图**。
 *
 * 实现直接复用屏幕版（只把截屏换成从句柄取图、偏移清零），
 * 所以行为严格一致。句柄无效时返回与"截屏失败"相同的默认值。
 */
OP_C_API const wchar_t *OP_CALL OpOcrFromMemEx2(op_handle handle, const wchar_t *image_handle, int x1, int y1,
                                                 int x2, int y2, const wchar_t *color, double sim);
OP_C_API const wchar_t *OP_CALL OpOcrAutoColorFromMemEx2(op_handle handle, const wchar_t *image_handle, int x1,
                                                          int y1, int x2, int y2, const wchar_t *color, double sim,
                                                          int *find_count);
OP_C_API int OP_CALL OpFindPicFromMem(op_handle handle, const wchar_t *image_handle, int x1, int y1, int x2, int y2,
                                      const wchar_t *files, const wchar_t *delta_color, double sim, int dir, int *x,
                                      int *y);
OP_C_API const wchar_t *OP_CALL OpFindPicExFromMem(op_handle handle, const wchar_t *image_handle, int x1, int y1,
                                                     int x2, int y2, const wchar_t *files, const wchar_t *delta_color,
                                                     double sim, int dir);
OP_C_API int OP_CALL OpFindColorFromMem(op_handle handle, const wchar_t *image_handle, int x1, int y1, int x2, int y2,
                                        const wchar_t *color, double sim, int dir, int *x, int *y);
OP_C_API const wchar_t *OP_CALL OpFindColorExFromMem(op_handle handle, const wchar_t *image_handle, int x1, int y1,
                                                      int x2, int y2, const wchar_t *color, double sim, int dir);
OP_C_API const wchar_t *OP_CALL OpGetColorFromMem(op_handle handle, const wchar_t *image_handle, int x, int y);
OP_C_API int OP_CALL OpCmpColorFromMem(op_handle handle, const wchar_t *image_handle, int x, int y,
                                       const wchar_t *color, double sim);
OP_C_API int OP_CALL OpGetColorNumFromMem(op_handle handle, const wchar_t *image_handle, int x1, int y1, int x2, int y2,
                                          const wchar_t *color, double sim);
OP_C_API int OP_CALL OpFindMultiColorFromMem(op_handle handle, const wchar_t *image_handle, int x1, int y1, int x2,
                                              int y2, const wchar_t *first_color, const wchar_t *offset_color,
                                              double sim, int dir, int *x, int *y);
OP_C_API const wchar_t *OP_CALL OpFindMultiColorExFromMem(op_handle handle, const wchar_t *image_handle, int x1, int y1,
                                                           int x2, int y2, const wchar_t *first_color,
                                                           const wchar_t *offset_color, double sim, int dir);
OP_C_API const wchar_t *OP_CALL OpFindColorBlockExFromMem(op_handle handle, const wchar_t *image_handle, int x1,
                                                           int y1, int x2, int y2, const wchar_t *color, double sim,
                                                           int count, int height, int width);
OP_C_API const wchar_t *OP_CALL OpYoloDetectFromMem(op_handle handle, const wchar_t *image_handle, int x1, int y1,
                                                     int x2, int y2, double conf, double iou, int *find_count);
OP_C_API const wchar_t *OP_CALL OpYoloDetectFromMemEx2(op_handle handle, const wchar_t *image_handle, int x1, int y1,
                                                        int x2, int y2, double conf, double iou,
                                                        const wchar_t *json_options, int *find_count);
#ifdef __cplusplus
}
#endif
