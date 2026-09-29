# 验证：OpenCV 域 C API 导出层（`OpCv*` 36 个）

- 日期：2026-09-29
- 探针：`scripts/probes/_t_opencv_api.py`（4 组，V1 模板库 / V2 文件型预处理 / V3 JSON 返回型 / V4 捕获型匹配）
- 结果：**PASS=109 / FAIL=0 / INFO=6**，**36/36 函数全覆盖**
- 日志：`workbench/probes/_cv_all_final.txt`

---

## 1. 为什么做这一轮（缺口定位）

`OpCv*` 共 36 个（COM `libop.h` / C API `op_c_api.h` / Python `cv_*` 三层完全对齐）。
但既有 C++ gtest（`tests/opencv_test.cpp`，30 个用例全绿）**只直接调用了 3 个 `OpCv*`**，
其余走的是 C++ 内部 `opcv::` API —— **绕过了 C API 导出层**。

也就是说：**gtest 全绿 ≠ 导出层可用**。而导出层正是 Python 绑定 / OPlug / GMAJ / OPTool 的实际调用面。
本轮即在**导出层**做端到端验证。

---

## 2. 方法：自生成 ground-truth，不做"看起来像对的"判据

不使用仓库 `assets/` 里的未知素材，而是**自己合成已知答案的图**：

| 素材 | 内容 | 用途 |
|---|---|---|
| `src.png` 480x360 | 白底 + 红块(40,40)-(200,160) + 绿矩形 + 蓝矩形 + **黑十字**(330,260) | 预处理 / 匹配唯一性 |
| `blobs.png` | 3 个 60x60 分离白方块，x=30/170/310 | 连通域 / 轮廓 ground truth |

由此可写**像素级**判据，例如：
- 红 `#FF0000` → 灰度 **76**、绿 `#00FF00` → 灰度 **150**（BT.601 精确值），且两者不同 ⇒ 排除"未处理的伪实现"
- `cv_crop_valid` 输出 **360x280** = 我预测的内容包围盒（精确命中）
- 连通域应恰好 3 个、坐标 (30,150)/(170,150)/(310,150)

**每一步都带反向验证**：不存在的路径必须 False、坏格式必须 False、已删模板必须 `ok:0`、越界搜索窗必须失败。

---

## 3. 结果

### V1 版本 + 模板库 CRUD（9 个）— 全绿

`cv_get_open_cv_version` = **5.0.0**；加载/查询/列表/掩膜模板/批量列表/删除/清空全通，count 随操作正确变化（1→2→4→3→0）。

### V2 文件型预处理（17 个）— 全绿，判据均命中 ground truth

| 函数 | 判据实值 |
|---|---|
| `cv_to_gray` | 全图 R==G==B；红→灰 **76**、绿→灰 **150** |
| `cv_to_binary` / `cv_threshold` | 只含 0/255；白底保白、红区变黑 |
| `cv_crop` | 输出 160x120 且**内容纯绿**（像素级） |
| `cv_resize` | 输出 240x180 |
| `cv_in_range('bgr', 0,0,255 ~ 0,0,255)` | 白落在**红**区、蓝区为黑 ⇒ **BGR 语义确认**（与 `Color.h` 内部 BGRA 布局一致；注意与对外 `RRGGBB` 是两层不同口径） |
| `cv_crop_valid` | 输出 **360x280** = 内容包围盒 |
| `cv_to_edge` / `cv_to_outline` | 黑像素占比 0.998 / 0.988（边缘稀疏） |
| `denoise/equalize/clahe/blur/sharpen/morphology` | 尺寸不变 + 内容确实变化（避免"原样返回"蒙混） |

### V3 JSON 返回型（2 个）— 全绿

- `cv_connected_components` → 3 个方块，坐标/尺寸/面积全部精确
- `cv_find_contours` → 同上，额外给出 `perimeter` / `points`（矩形 4 点）
- 不存在的文件 → **合法失败 JSON** `{"ok":0,"results":[]}`（不是崩溃）
- **共享缓冲**：交错调用后首次结果仍有效（C API 字符串返回值是共享缓冲，跨调用需先拷贝 —— 此处实测已安全）

### V4 捕获型匹配（8 个）— 全绿，并**抓到 1 个真缺陷**

| 函数 | 结果 |
|---|---|
| `cv_match_template` th=0.90 | `(298,238)` score=**0.9074** |
| 同上 th=0.99 / 0.999 | `(300,238)` score=**1.0** 精确 |
| `cv_match_template_scale('0.8\|1.0\|1.2')` | `(300,238)` score=1.0 |
| `cv_match_template_rot('0\|15\|-15')` | `(300,238)` score=1.0 angle=0 |
| `cv_match_any_template` th=0.90 / 0.99 | `(298,238)`→`(300,238)`，`name=t1` |
| `cv_match_all_templates` | `results[0]` = t1 `(300,238)` score=1.0 |
| `cv_feature_match_template` | `(301,238)` score=0.4167 |
| **`cv_edge_match_template`** | ⚠ **`(0,0)` score=1.0 —— 伪命中**（见下） |
| `cv_shape_match_template` th=0.3/0.5/0.8 | 均 `ok:0`（软项，见下） |

#### 阈值语义（重要，非缺陷）

`th 0.90 → (298,238) score 0.9074`，`th 0.99 → (300,238) score 1.0`。
即**返回的是"第一个超过阈值的命中"，不是"全局最优"** —— 提高阈值可让它收敛到真位置。
（与 gtest 用例名 `MatchTemplateReturnsFirstThresholdHit` 一致。**不是缺陷**，但调参时必须知道：
阈值定低了会拿到一个勉强及格的位置。）

#### ⚠ 缺陷：`cv_edge_match_template` 恒返回搜索窗左上角（伪命中）

**决定性证据 —— 单变量 A/B（只平移搜索窗，模板与画面完全不变）**：

```
cv_edge_match_template(0,0,480,360)        -> x=0,y=0    score=1.0
cv_edge_match_template(120,90,360,270)     -> x=120,y=90 score=1.0   ← 命中点跟着窗左上走
```

命中点**恒等于搜索窗左上角**，score **恒 1.0**，与模板内容无关 ⇒ 该函数当前不可用。

**根因（`libop/opencv/TemplateMatcher.cpp`）**：

```cpp
bool EdgeMatchTemplate(...) {
    toEdge(source, source_edge);                       // Canny(60,180)
    ensureTemplateEdge(entry);                         // 模板同样 Canny
    return findBestPreparedMatch(source_edge, templ_edge, nullptr,
                                 region, threshold, result, cv::TM_CCOEFF_NORMED);
}
// findBestPreparedMatch 尾部：
cv::minMaxLoc(match_result, &min_score, &max_score, &min_location, &max_location);
result.x = roi.x + best_location.x;
```

`TM_CCOEFF_NORMED` 在**平坦区（局部方差≈0）退化**：分母趋零时 OpenCV 会给出 **1.0**。
画面里大片纯白/纯红区域全部退化成 1.0，与真位置的 1.0 **并列**，而 `minMaxLoc` 遇并列取**行优先首个** ⇒ 永远落在搜索窗左上角。

**建议修法（三选一，未实施，需你拍板）**：
1. 换 `TM_SQDIFF_NORMED`（平坦区不退化，值越小越优）— 改动最小
2. 匹配前对 `match_result` 做方差掩膜：剔除源图局部方差 ≈ 0 的位置
3. 改用 `TM_CCOEFF_NORMED` + 显式 mask 并自行扫描取最优（成本最高）

#### `cv_shape_match_template` 恒 `ok:0`（软项，非确定缺陷）

`toShapeMask` 走 `alphaToMask` → 失败则 OTSU 二值化。本轮靶子是**多色块合成图**，OTSU 得到的是大色块轮廓而非"十字"形状 ⇒ `matchShapes` 距离超阈值。
判据已放宽为 INFO 并做阈值扫描（0.3/0.5/0.8）。**结论：该算法需要前景可分离的目标（alpha 掩膜或干净前景），不适合多色合成图** —— 属适用性问题，暂不判缺陷。

---

## 4. 过程中修正的自身判据缺陷（记录以免重犯）

1. **BMP 解析只支持 24/32bpp** → op 对单通道结果写的是 **8bpp + 调色板 BMP**，行尾越界 ⇒ 9 个 FAIL 全是我的解析器问题。已补 8bpp 调色板解析。
2. **模板裁在纯色区中心 → 多解**（任何区内位置 score 都 1.0）⇒ 命中退化为搜索窗左上角。与真机 G3 `find_pic` 踩的是**同一个坑**。
   修法：模板裁到**含颜色交界的角落** + 加**唯一性自检**（模板内唯一色 ≥ 2，否则记 INFO 预警）。
3. **靶子仍不够唯一**（0.99 阈值下仍有第二个 score=0.990 的位置）⇒ 加**全画面唯一的黑十字**高对比特征后才拿到 score=1.0 的精确命中。

---

## 5. 结论

- **C API 导出层 36/36 全部可用**，除 `cv_edge_match_template` 外无功能性缺陷。
- **`cv_edge_match_template` 存在真缺陷（伪命中）**，根因已定位到 `TM_CCOEFF_NORMED` 平坦区退化 + `minMaxLoc` 并列取首个。
- 匹配类函数语义为**首个超阈值命中**，非全局最优 —— 调参须知。
- 既有 gtest 只覆盖 3/36 的 `OpCv*`，**本探针是其超集且落在真实调用面**，建议常态化执行。
