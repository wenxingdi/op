# OP 图色能力深拆 ③：颜色块 / 找线 / 截图存图

> 只读源码分析，**未改动任何代码**。
> 源码依据：
> - `libop/op/OpImage.cpp`（公共层 Capture / FindColorBlock / GetColorNum）
> - `libop/op/OpOcr.cpp:473`（公共层 FindLine）
> - `libop/image/ImageSearchService.cpp`（服务层 193 / 309-325 / 864-896 / 1141-1147）
> - `libop/image/ImageSearchAlgorithms.cpp`（算法层 586 / 922-962 / 1071-1110）
> - `libop/image/Image.h:233`（落盘 write）
> - `libop/op/OpCaptureHelpers.h`（区域抓取模板）

至此图色 6 大类（A 截图 / B 找色 / C 找图 / D 找字 / E 颜色块 / F 找线）**全部深拆完毕**。

---

## 🔴 头号发现：默认去噪对本篇三个能力**完全不生效**

上一轮我们把 `_binary_preprocess_mode` 由 `0` 改为 `1`（默认删孤立单点噪点）。本次读码确认它的**作用域被限定了**：

| 二值化入口 | 是否调 `ApplyBinaryPreprocess()` | 谁在用 |
|---|---|---|
| `str2pointbinaryfbk(color, sim)` | ✅ **走去噪** | OCR / FindStr / FetchWord / FetchWords（制库 + 识别） |
| `str2binaryfbk(color, sim)` | ❌ **不去噪** | **FindColorBlock / FindColorBlockEx / FindLine** |

源码（`ImageSearchService.cpp:311/317/1145`）三处全部调的是 `str2binaryfbk`：

```cpp
long ImageSearchService::FindColorBlock(...)   { str2binaryfbk(color, sim); ... }  // 311
long ImageSearchService::FindColorBlockEx(...) { str2binaryfbk(color, sim); ... }  // 317
long ImageSearchService::FindLine(...)         { str2binaryfbk(color, sim); ... }  // 1145
```

**结论**：`SetBinaryPreprocess(...)` 和默认 `mode=1` 都只影响字库/OCR 链路，**颜色块和找线拿到的是原始二值图**。这对 `FindLine` 尤其致命——霍夫变换是投票机制，噪点会直接污染累加器。

---

## A. 截图存图 `Capture` / `CapturePre`

### 调用链
```
op::Op::Capture(x1,y1,x2,y2,file)
  → with_captured_region()      // 绑定检查 + 坐标转换 + 抓图到 _src
    → ImageSearchService::Capture(file)
      → _src.write(fullpath)    // ATL::CImage 落盘
```

### 关键事实（含对旧盘点的纠正）

| 项 | 真实行为 |
|---|---|
| **位深** | ⚠️ **32 位**，不是旧文档写的"24 位 BMP"（`img.Create(width, height, 32)`） |
| **格式** | 由**文件扩展名**决定（`ATL::CImage::Save` → GDI+ 编码器），`.bmp`/`.png`/`.jpg`/`.gif` 都可以 |
| **区间** | 半开 `[x1,x2)`，宽 = `x2-x1`（传 `0,0,100,100` 得 100×100） |
| **相对路径** | 基于 `SetPath` 设的 `_curr_path` 拼绝对路径 |
| **必须先绑定** | `with_captured_region` 里 `check_bind()` 失败 → **直接 return，ret 保持 0，静默无提示** |
| **失败原因不可区分** | 路径不存在 / 扩展名不支持 / GDI+ 失败 / 未绑定，统统返回 0 |

### `CapturePre` 的真实语义
```cpp
void op::Op::CapturePre(const wchar_t *file, LONG *ret) {
    internal::set_result(ret, m_context->image_proc.Capture(file));   // 不抓图！
}
```
它**不做任何抓图动作**，直接把 `_src` 里**上一次图色操作残留的那张图**写盘。

- 用途：找图/找色失败后，立刻 `CapturePre` 把"当时到底看到了什么"存下来 —— **调试神器**
- 坑：如果本次会话还没做过任何图色操作，`_src` 为空 → `write()` 返回 false → **ret=0**
- 坑：`_src` 只是上次那块**区域**，不是全屏

### 一个容易误报的实现细节（已核实**不是** bug）
`Image::write` 里 `pdst += pitch`，而 `CImage::Create` 正高度创建的是 bottom-up DIB、`GetPitch()` 返回**负值**、`GetBits()` 指向图像第 0 行。负 pitch 逐行回退恰好与源图 0→h-1 对齐，**不存在上下翻转问题**。只是强依赖 ATL 的负 pitch 语义，改动时容易踩。

---

## E. 颜色块 `FindColorBlock` / `FindColorBlockEx`

### 🔴 它不是"连通色块检测"

旧盘点写的是"找面积 ≥ count、尺寸 ≤ h×w 的同色区域"，**语义不准**。看算法层（`ImageSearchAlgorithms.cpp:922`）：

```cpp
record_sum(_binary);                                    // 建积分图
for (int i = 0; i <= _binary.height - height; ++i)
  for (int j = 0; j <= _binary.width - width; ++j)
    if (region_sum(j, i, j + width, i + height) >= count) {   // 窗口内点数 >= count
        x = j + _x1 + _dx;  y = i + _y1 + _dy;  return 1;
    }
```

真实语义：**存在一个固定 `width × height` 的滑动窗口，窗口内匹配色的像素数 ≥ `count`**。

推论（实战必须知道）：
1. **不要求这些点相连** —— 满屏均匀散点，只要密度够，一样命中
2. **窗口尺寸是固定的，不是上限** —— `height/width` 是"检测框大小"，不是"色块最大尺寸"
3. `count` 是**绝对像素数**，不是比例。改窗口大小必须同步改 count
4. 返回的是**窗口左上角**坐标（已加 `_x1+_dx` 偏移，屏幕坐标系），**不是色块中心**

### `FindColorBlockEx` 的返回值爆炸问题

```cpp
if (region_sum(...) >= count) {
    retstr += ...;  ++cnt;
    if (cnt > _max_return_obj_ct) goto _quick_return;
}
```

**没有任何去重 / NMS**。一个实心色块，窗口每移动 1 像素都满足条件 → 一个块能吐出成百上千个几乎重合的坐标，然后被 `_max_return_obj_ct` 粗暴截断。

- 副作用：结果里全是同一个块的邻近点，**根本找不到"第二个块"**
- 细节 bug 级：`goto` 判断在 `++cnt` **之后**，实际最多返回 `_max_return_obj_ct + 1` 个
- 实战对策（调用端）：拿到坐标串后自己做一次网格聚类/间距过滤，或直接改用 `FindColorEx` + 自建连通域

### 参数与容差

| 项 | 说明 |
|---|---|
| 参数顺序 | `(x1,y1,x2,y2, color, sim, count, **height**, **width**, ...)` —— **h 在 w 前面，极易传反** |
| 容差公式 | `ceil((1-sim)*255)` 每通道绝对差（`sim_to_point_color_diff`），与 `FindColor` **完全一致** |
| sim 重映射 | ❌ 无（`FindPic` 独有 `0.5+sim/2`）→ 同一个 `sim=0.9`，这里比 FindPic 严格 2 倍 |
| 边界 | `height > 图高 || width > 图宽` → 直接返回 0（不报错） |
| 去噪 | ❌ 不走 `ApplyBinaryPreprocess` |
| 复杂度 | 积分图 O(W·H) + 滑窗 O(W·H)，**与窗口尺寸无关**，很快 |

---

## F. 找线 `FindLine`

### 实现：标准霍夫直线变换（`ImageSearchAlgorithms.cpp:1071`）

```cpp
int h = sqrt(w*w + hh*hh) + 2;
_sum.create(360, h);                       // 累加器：360 列角度(1°步长) × h 行距离(1px步长)
for 每个前景点 (j=x, i=y):
    for (t = 0; t < 360; ++t):
        d = j*cos(t) + i*sin(t);
        if (d >= 0) _sum.at<int>(d, t)++;  // 投票
取全局最大值 → outStr = "角度,距离"，return maxval;   // maxval = 该直线上的点数
```

### 🔴 缺陷 1：公共层把"置信度"丢了

```cpp
void op::Op::FindLine(long x1, long y1, long x2, long y2, const wchar_t *color, double sim, wstring &retstr) {
    with_captured_region(..., [&]() { m_context->image_proc.FindLine(color, sim, retstr); });
}   // ← 服务层返回的 maxval（直线上点数）被直接丢弃，没有 ret 出参
```

服务层算出了 `maxval`（这条线上有多少个点），但**公共层没有把它传出去**。调用者只拿到 `"角度,距离"`。

### 🔴 缺陷 2：永远返回一条"线"

累加器取的是全局最大值，**没有阈值判断**。哪怕区域里一条直线都没有（甚至只有几个随机噪点），也会返回一个角度/距离。

叠加缺陷 1 —— **调用者完全没有办法判断这条线是真实存在的，还是噪点拟合出来的幻觉**。这是 `FindLine` 目前最影响可用性的问题。

> ✅ **已修复（2026-08-06）**：保留 `FindLine` 原签名不动（零 ABI 风险），新增 `FindLineEx` 把霍夫累加器峰值（直线上点数 `point_count`）作为 `long *ret` 出参传出。调用者据此判断线是否可信——`point_count` 越大线越可信，0=未截图/无匹配点。已覆盖 `ImageColorTest.FindLineEx*` 两个用例（点数 16 / 0 均验证通过）。

### 其余关键点

| 项 | 说明 |
|---|---|
| 只能找**一条**线 | 无 `FindLineEx`，无法返回多条候选 |
| 坐标系 | 距离 `d` 相对**二值图局部原点**（即区域左上角），**未加 `_x1+_dx` 偏移**，要自己换算 |
| 角度定义 | `d = x·cosθ + y·sinθ` 的法线式，θ 是**法线角**（不是直线倾角），单位度，范围 0–359 |
| 精度 | 角度 1° 步长；距离 `int` **截断**（非四舍五入）→ 固有 ±0.5px 偏差 |
| 去噪 | ❌ 不走 `ApplyBinaryPreprocess` —— 霍夫是投票机制，**对噪点最敏感的却恰恰没去噪** |
| 性能 | **全图色最重操作**：O(前景点数 × 360)。sim 放宽导致前景点上万 → 数百万次累加。cos/sin 已查表优化，但量级摆在那 |
| `_sum` 复用 | 调用后 `_sum` 从"积分图"变成 360×h 霍夫累加器；其他函数用前都会 `record_sum` 重建，**当前安全**，但属隐式耦合，改动易踩 |
| `assert(d <= h)` | Release 下失效；数学上 `d_max = sqrt(w²+h²) < h`，安全 |

---

## 三套 sim 语义汇总（跨模块，务必记住）

| 能力 | 容差算法 | sim 重映射 |
|---|---|---|
| `FindPic` / `FindPicEx` | 灰度差累计 vs `(1-sim)·norm` | ✅ `sim = 0.5 + sim/2` |
| `FindColor` / `CmpColor` / `GetColorNum` | 每通道 `ceil((1-sim)*255)` 绝对差 | ❌ 无 |
| `FindColorBlock` / `FindLine`（二值化） | 每通道 `ceil((1-sim)*255)` 绝对差 | ❌ 无 |

→ 同样写 `sim=0.9`：FindPic 实际按 0.95 算（宽松），找色/颜色块/找线按 25.5/255 通道容差算（严格）。**跨函数照搬 sim 数值必然翻车。**

---

## 改进建议（①已实施；②③④⑤⑥ **未实施**，需你拍板）

| # | 项 | 风险 | 收益 |
|---|---|---|---|
| 1 | ✅ **已完成**：新增 `FindLineEx`（保留 `FindLine` 原签名，**零 ABI 风险**），追加 `long *ret` 出参把霍夫累加器峰值（线上点数）传出；已同步 libop / COM(`op.idl` id=348) / C-API(`OpFindLineEx`) / Python(`find_line_ex`) / Go(`FindLineEx`) | 低 | **高** —— 调用者终于能判断线是否可信（`point_count` 越大越可信，0=未截图/无匹配点） |
| 2 | `FindColorBlockEx` 加最小间距去重（如命中后 x 跳 width、y 跳 height） | 中（改变返回语义，可能影响既有脚本） | **高** —— 一个块只返回一次，结果可用 |
| 3 | 颜色块/找线改走 `str2pointbinaryfbk`（享受去噪） | 中（改变默认行为，可能影响既有阈值） | 中 —— 找线抗噪显著改善 |
| 4 | `FindLine` 加阈值参数（点数 < N 视为无线，返回空串） | 低（新增可选参数） | 中 —— 与 1 二选一即可 |
| 5 | `Capture` 区分失败原因（写 `GetLastError`） | 低 | 中 —— 排障省时间 |
| 6 | `FindColorBlockEx` 的 `cnt > _max` 判断挪到 push 前 | 极低 | 低 —— 修掉多返回 1 个的小瑕疵 |

### 调用端立刻能做的规避（不改 OP 代码）
- **颜色块**：`FindColorBlockEx` 结果自行按 `width/height` 网格去重；`count` 随窗口尺寸等比调整；牢记 `height` 在 `width` 前。
- **找线**：把检测区域**尽量裁小**、`sim` 尽量收紧（控制前景点数），否则性能和误检双输；结果距离记得 `+ 区域左上角` 换算。
- **截图**：`Capture` 前确认已绑定；扩展名用 `.png`（无损且小）；失败排障优先用 `CapturePre` 落盘"上次看到的图"。

---

## 测试覆盖现状

`tests/image_color_test.cpp` 中：
- ✅ `FindColorBlock` / `FindColorBlockEx`（含失败路径）、`GetColorNum` 已覆盖
- ❌ `Capture` / `CapturePre`（存图 I/O）零覆盖；⚠️ `FindLine` 仍零直接覆盖，但已新增 `FindLineEx` 两个用例：`FindLineExReportsPointCountAsConfidence`（16×16 第8行整行白 → `point_count==16`）、`FindLineExReturnsZeroCountWhenNoForeground`（全黑 → `point_count==0`）

→ `FindLine` 是"零测试 + 无阈值 + 丢返回值 + 不去噪 + 最慢"的四重叠加，是本篇三个能力里**最需要谨慎使用**的一个。
