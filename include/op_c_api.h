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
OP_C_API int OP_CALL OpDrawRect(op_handle handle, int x1, int y1, int x2, int y2, const wchar_t *color, int width);
OP_C_API int OP_CALL OpDrawLine(op_handle handle, int x1, int y1, int x2, int y2, const wchar_t *color, int width);
OP_C_API int OP_CALL OpDrawText(op_handle handle, int x, int y, const wchar_t *text, const wchar_t *color,
                                int font_size);
OP_C_API int OP_CALL OpDrawClear(op_handle handle);
OP_C_API int OP_CALL OpDrawSetVisible(op_handle handle, int show);

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
OP_C_API int OP_CALL OpSetYoloEngine(op_handle handle, const wchar_t *path_of_engine, const wchar_t *dll_name,
                                     const wchar_t *argv);
OP_C_API const wchar_t *OP_CALL OpYoloDetect(op_handle handle, int x1, int y1, int x2, int y2, double conf,
                                             double iou);
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
OP_C_API int OP_CALL OpFindStr(op_handle handle, int x1, int y1, int x2, int y2, const wchar_t *strs,
                               const wchar_t *color, double sim, int *x, int *y);
OP_C_API const wchar_t *OP_CALL OpFindStrEx(op_handle handle, int x1, int y1, int x2, int y2,
                                            const wchar_t *strs, const wchar_t *color, double sim);
OP_C_API const wchar_t *OP_CALL OpOcrAuto(op_handle handle, int x1, int y1, int x2, int y2, double sim);
OP_C_API const wchar_t *OP_CALL OpOcrFromFile(op_handle handle, const wchar_t *file_name,
                                              const wchar_t *color_format, double sim);
OP_C_API const wchar_t *OP_CALL OpOcrAutoColorFromFile(op_handle handle, const wchar_t *file_name,
                                                        const wchar_t *color_format, double sim);
OP_C_API const wchar_t *OP_CALL OpOcrAutoFromFile(op_handle handle, const wchar_t *file_name, double sim);
/* 免字库 OCR 归一入口（推荐）：color 为空 → 原图直识别；非空 → 按颜色二值化。行为同 OpOcrAuto/OpOcrAutoColor 两分支 */
OP_C_API const wchar_t *OP_CALL OpAutoOcr(op_handle handle, int x1, int y1, int x2, int y2, const wchar_t *color,
                                          double sim);
/* ---- 以下旧名为废弃别名，保留转发以兼容已有脚本，新代码请用 OpOcrAutoColor* / OpAutoOcr ---- */
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

#ifdef __cplusplus
}
#endif
