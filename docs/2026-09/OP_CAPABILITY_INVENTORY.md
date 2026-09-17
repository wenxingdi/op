# OP 库能力清单（图色 / 图像搜索 + 全项目能力地图）

> 整理自源码：
> - 门面 `include/libop.h`（`op::Op`，聚合 9 个服务组）
> - 图像服务 `libop/image/ImageSearchService.h` + 算法层 `ImageSearchAlgorithms.h`
> - 实测基准 `tests/image_color_test.cpp`
> - 绑定/输入/窗口方法见 `include/libop.h`

---

## 一、全项目能力地图（op::Op 门面 = 9 个服务组）

`op::Op` 把所有能力挂在同一个对象上，按逻辑切成 9 组服务（源码 `libop.h:49-85` 的 `using` 别名）。

| 服务组 | 代表方法（公开） | 职责 |
|---|---|---|
| **Runtime** | `Ver` / `SetPath` / `GetPath` / `Sleep` / `InjectDll` / `EnablePicCache` / `GetID` / `GetLastError` / `SetShowErrorMsg` | 版本、路径、休眠、注入 DLL、全局配置 |
| **Window** | `FindWindow` / `FindWindowEx` / `FindWindowByProcess` / `FindWindowByProcessId` / `GetWindowTitle` / `MoveWindow` / `SetWindowState` | 窗口查找与基本操控 |
| **Binding** | `BindWindowEx` / `SetDisplayInput`（含 `dx`/`dinput`/`rawinput`/`winmsg` 通道与 `.dx` 后缀法） | 绑定窗口、选择显示/输入后端、dx 三通道开关 |
| **Input** | `MoveR` / `LeftClick` / `RightClick` / `MiddleClick` / `WheelUp` / `WheelDown` / `KeyDown` / `KeyUp` / `KeyPress` / `KeyPressChar` / `KeyPressStr` | 鼠标移动/点击/滚轮、键盘按下/弹起/按键串 |
| **Image**（图色） | 见第二节 6 大类 | 截图、找色、找图、找字/OCR、颜色块、找线/取色 |
| **Ocr** | `FindStr` / `FindStrEx` / `Ocr` / `OcrEx` / `OcrAuto` / 字库管理（见下） | 本地字库点阵匹配（**非远程服务**） |
| **OpenCv** | `CvThreshold` / `MatchPicName` / … | OpenCV 图像处理原语 |
| **Yolo** | `SetYoloEngine` / `YoloDetect` / `YoloDetectFromFile` | YOLO 目标检测（引擎外挂） |
| **Memory** | `ReadData` / `WriteData` / `ReadInt`~`WriteDouble` / `ReadString` / `WriteString` / `FindData(Ex)` / `GetModuleBaseAddr` | 进程内存读写、**特征码搜索（?? 通配）**、模块基址 |
> 注：OCR 与 Image 在门面上分开成 `ocr()`/`image()` 两个别名，但实现都落在 `ImageSearchService` 内。

---

## 二、图色 / 图像搜索能力详解（6 大类 + 支撑能力）

### A. 截图存图
| 方法 | 说明 |
|---|---|
| `Capture(x1,y1,x2,y2,file)` | 抓取指定区域并落盘（**32 位**，格式由**扩展名**决定：bmp/png/jpg/gif） |
| `CapturePre(file)` | **不抓图**，直接把 `_src`（上次图色操作用的那张图）写盘 —— 调试神器 |

> ⚠️ 用户口中的 `SavePic` 不是公开方法；等价能力由 `Capture` 提供。
> ⚠️ 旧版本文档写的"24 位 BMP"**已证伪**：源码 `Image.h:238` 为 `img.Create(w,h,32)` + `CImage::Save`（GDI+ 按扩展名编码）。
> ⚠️ 未绑定窗口时 `Capture` **静默失败**（ret=0，无错误提示）。详见 `BLOCK_LINE_CAPTURE_DEEPDIVE.md`。

### B. 找色
| 方法 | 说明 | 变体 |
|---|---|---|
| `FindColor(x1,y1,x2,y2,color,sim,dir,&x,&y,&ret)` | 找第一个匹配点 | 返回坐标 |
| `FindColorEx(...)` | 找所有匹配点 | 返回 `"x,y|x,y|..."` |
| `FindMultiColor(first,offset,sim,dir,&x,&y,&ret)` | 基准色 + 偏移色（多点着色） | `FindMultiColorEx` 全返回 |
| `FindColorNum(color,sim)` / `GetColorNum(x1..y2,color,sim,&count)` | 计数匹配点数量 | — |
| `CmpColor(x,y,color,sim,&ret)` | 单点比色（返回 0/1） | — |
| `GetColor(x,y) -> wstring` | 取某点颜色值（6 位 hex `RRGGBB`） | — |

### C. 找图（模板匹配，最常用）
| 方法 | 说明 |
|---|---|
| `FindPic(x1,y1,x2,y2,files,delta_colors,sim,dir,&x,&y,&ret)` | 多图模板匹配，返回第一个命中 |
| `FindPicEx(...)` | 多图全返回，格式 `"id,x,y|id,x,y|..."`，可选 `returnID` |
| `FindPicExS(...)` | 带模板名字返回（如 `"name,x,y|..."`） |

> 算法层还有 `FindPicTh` / `FindPicExTh`（多线程版本），公开层主要暴露上述非 Th 版本。
> `files` 支持多图（按 `|` 分隔文件名或内存名）；`delta_colors` 为透明色/容差串。

### D. 找字 / OCR（本地字库点阵匹配）
| 方法 | 说明 |
|---|---|
| `FindStr(x1,y1,x2,y2,str,color,sim,&x,&y,&ret)` | 字库找字，返回首个位置 |
| `FindStrEx(...)` | 全返回 `"x,y,idx|x,y,idx|..."` |
| `Ocr(x1,y1,x2,y2,color,sim,str)` | 区域内 OCR 识别文本 |
| `OcrEx(...)` | OCR 带坐标 `"x,y,char|..."` |
| `OcrAuto(sim,retstr)` / `OcrFromFile(...)` / `OcrAutoFromFile(...)` | 自动/从文件识别 |

> ⚠️ 这里 **OCR = 本地字库 + 二值化点阵匹配**（`bin_ocr` over `Dictionary`），不依赖外部 OCR 服务。

### E. 颜色块
| 方法 | 说明 |
|---|---|
| `FindColorBlock(x1,y1,x2,y2,color,sim,count,h,w,&x,&y,&ret)` | 在**固定 `w×h` 滑动窗口**内匹配色点数 ≥ `count` 时命中，返回**窗口左上角** |
| `FindColorBlockEx(...)` | 全返回 `"x,y|x,y|..."`（⚠️ **无去重**，单个色块会吐出大量重合坐标） |

> ⚠️ 旧表述"找面积 ≥ count、尺寸 ≤ h×w 的同色区域"**已证伪**：它**不做连通域分析**，散点也算；`h/w` 是固定检测框尺寸而非上限。
> ⚠️ 参数顺序是 `count, height, width`（**h 在 w 前**，极易传反）。

### F. 找线 / 取色
| 方法 | 说明 |
|---|---|
| `FindLine(x1,y1,x2,y2,color,sim,retstr)` | 霍夫变换找**唯一一条**最强直线，返回 `"法线角度,距离"`（角度 1° 步长；距离相对**区域左上角**） |
| `GetColor(x,y)` | 取色（见 B） |
| `CmpColor(x,y,color,sim)` | 比色（见 B） |

> ⚠️ `GetPixel` 仅在算法层 `ImageSearchAlgorithms` 内部使用，**未作为公开门面方法暴露**。
> 🔴 `FindLine` 三重风险：① 公共层**丢弃了返回值**（直线上点数），调用者无法判断线是否可信；② **无阈值**，没有直线时也必返回一个"幻觉线"；③ 不去噪 + O(前景点×360)，是全图色**最慢**操作。零测试覆盖。详见 `BLOCK_LINE_CAPTURE_DEEPDIVE.md`。

### 支撑能力（图管理 + 字库 / 二值化工具）
- **图管理**：`LoadPic` / `FreePic` / `LoadMemPic` / `GetPicSize`
- **字库管理**（槽位 0–99，全局共享）：`SetDict` / `SetMemDict` / `GetDict` / `UseDict` / `AddDict` / `SaveDict` / `ClearDict` / `GetDictCount` / `GetNowDict`
- **字提取 / 字库生成**：`FetchWord` / `FetchWordEx` / `ExtractWordRects` / `ExtractWordRectsEx` / `FetchWords` / `FetchWordsEx` / `FetchWordsByRects`
- **二值化 / 校验工具**：`SetBinaryPreprocess` / `GetBinaryPreprocess` / `GetBinaryPreview` / `GetWordPreview` / `CheckWordDict` / `NormalizeWordDict` / `RenameWordDict`

---

## 三、统一参数约定

> 🔴 **二值化去噪的作用域**（`SetBinaryPreprocess` / 默认 `_binary_preprocess_mode=1`）：
> 只对走 `str2pointbinaryfbk` 的链路生效 —— **OCR / FindStr / FetchWord / FetchWords**。
> **`FindColorBlock` / `FindColorBlockEx` / `FindLine` 走 `str2binaryfbk`，不去噪**。


- **相似度 `sim`**：既作显式容差，也作「无显式 delta 时的隐式颜色容差」。例：`CmpColor(x,y,"433222",0.98)` 在近似度下命中；带显式 delta `"433222-030202"` 则严格按 delta。
- **方向 `dir`（0–8）**：控制扫描顺序（0=从左到右/上到下，1–8 为八方向起点），`image_color_test.cpp` 已实测 9 个方向行为一致。
- **颜色串语法**：
  - 多色用 `|` 分隔：`"010203|102030"`
  - 显式 delta：`"RRGGBB-DDRRGGBB"`
  - 多点着色偏移：`FindMultiColor` 的 `offset_color` 用 `"dx|dy|RRGGBB"` 描述相对偏移与颜色
  - 通道容差（OCR/找字）：`"RRGGBB-DDRRGGBB"` 或 `"@背景色-容差|..."` 标记背景

---

## 四、已实测覆盖（image_color_test.cpp）

覆盖充分的（含方向/相似度/失败路径/字库格式）：
`GetColor` · `CmpColor` · `FindColor`(+Ex, 9 方向) · `FindMultiColor`(+Ex, 9 方向) · `FindPic`(+Ex, 9 方向, 透明点, 缺失模板) · `FindColorBlock`(+Ex, 失败路径) · `GetColorNum` · `LoadPic`/`FreePic`/`LoadMemPic`/`GetPicSize`（全局缓存跨对象共享）· `FetchWord`(+Ex) · `ExtractWordRects`(+Ex) · `FetchWords`(+Ex, ByRects) · `GetBinaryPreview` · `SetBinaryPreprocess`/`GetBinaryPreprocess` · `GetWordPreview`/`CheckWordDict`/`NormalizeWordDict`/`RenameWordDict` · `SetDict`/`SetMemDict`/`AddDict`/`UseDict`/`ClearDict`/`GetDict`/`GetDictCount`/`SaveDict`（含大漠/OP 字库格式互通）· `FindStr`(+Ex, 通道容差/抗锯齿/多前景色/自动背景二值化) · `Ocr`(+Ex, 本地字库)。

未在该测试中覆盖：
`Capture`/`CapturePre`（存图 I/O）、`FindLine`、`OcrAuto`/`OcrFromFile`/`OcrAutoFromFile`、YOLO 系列、`CvThreshold`/`MatchPicName`。

---

## 五、待澄清 / 纠正项

1. **`SavePic` → 实为 `Capture`**；不存在独立 SavePic 公开方法。
2. **`GetPixel` 不对外**；取色用 `GetColor` + `CmpColor`。
3. **OCR 为本地字库点阵匹配**，非远程/本地 OCR 服务依赖（与早期"无服务时 SKIP"假设不符，需在讨论中确认）。
4. **`FindPic` 的 `delta_colors` / 透明色处理**与 `FindColor` 的颜色串语法细节需在逐条讨论时对齐。
5. **OpenCV / YOLO / Memory 三组**仅列方法名，未展开（属下一阶段能力）。
   - 更新（2026-09-04 第二轮）：Memory 组已补齐——`FindData(hwnd, addr_range, string)` / `FindDataEx(..., step, count)` 特征码搜索（`??` 通配、`VirtualQueryEx` 枚举已提交可读区、16MB 分块、默认上限 1024 条）+ `GetModuleBaseAddr(hwnd, module)`。COM id 260-262，C-API/Python/Go 全链路，测试 `tests/memory_search_test.cpp` 6 用例（本进程自测）。OpenCV / YOLO 待展开。

---

## 六、深拆进度（图色 6 大类已全部完成）

| 类 | 能力 | 深拆文档 |
|---|---|---|
| A | 截图存图 | ✅ `BLOCK_LINE_CAPTURE_DEEPDIVE.md` |
| B | 找色 / 多点着色 | ✅ `FINDCOLOR_DEEPDIVE.md` |
| C | 找图 | ✅ `FINDPIC_DEEPDIVE.md` |
| D | 找字 / 字库 | ✅ `OCR_DICTIONARY_DEEPDIVE.md` + `OCR_FUNCTION_COVERAGE.md` + `DICT_SUPERSET_REPORT.md` + `OP_DICT_MUSTDO.md` |
| E | 颜色块 | ✅ `BLOCK_LINE_CAPTURE_DEEPDIVE.md` |
| F | 找线 / 取色 | ✅ `BLOCK_LINE_CAPTURE_DEEPDIVE.md` |

**尚未展开的能力组**（属下一阶段）：`OpenCv`（`CvThreshold`/`MatchPicName`）、`Yolo`、`Memory`（内存读写/汇编）、支撑能力专章（图缓存、字库槽位共享机制）。
