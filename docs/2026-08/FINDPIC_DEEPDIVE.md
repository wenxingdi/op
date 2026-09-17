# FindPic 找图能力深拆（发现阶段 · 只读）

> 源码基准：`libop/op/OpImage.cpp`、`libop/image/ImageSearchService.{h,cpp}`、`libop/image/ImageSearchAlgorithms.{h,cpp}`、`libop/image/Color.h`
> 阶段：发现（只读，未改动任何 OP 源码）

## 1. 端到端流水线

```
op::Op::FindPic(x1,y1,x2,y2, files, delta_color, sim, dir, *x,*y,*ret)
  └─ internal::with_captured_region(ctx, x1,y1,x2,y2, λ)   // 抓图到 _src，记录区域原点 _x1,_y1
       └─ ImageSearchService::FindPic(files, delta_color, sim, dir, x, y)
            ├─ files2mats()      // 解析 files('|'分隔) → 查全局缓存 → 否则 read_pic_file → make_pic_match(build_pic_match_template) → 可选入缓存
            ├─ dfcolor.str2color(delta_color)
            ├─ sim = 0.5 + sim/2          // ★ 相似度重映射
            └─ ImageSearchAlgorithms::FindPicTh(...)   // 多线程版
                 ├─ _gray.fromImage4(_src)
                 ├─ record_sum(_gray)             // 积分图（前缀和）
                 └─ 对每个模板 pic：
                      matchRect.shrinkRect(w,h)    // 限制可匹配区域
                      max_err_ct = (w*h - transparent_count) * (1 - sim)
                      按线程数分块 → 每块 for_each_scan_point(dir, …) 调 trans_match / real_match
                      按 dir 优先级选最优命中点
  → 返回 pic_id（命中模板下标，0-based）或 -1；x,y = 命中左上角【绝对屏幕坐标】
```

坐标累加：`x = j + _x1 + _dx; y = i + _y1 + _dy`（`_dx/_dy` 为图源偏移，全屏搜索时均为 0）。

## 2. 两种匹配内核

| 模板类型 | 判定 | 匹配函数 | 比较方式 |
|---|---|---|---|
| **透明模板** | 四角同色 且 背景像素占比 ∈ [50%,100%)（`check_transparent`） | `trans_match<false>` | 只比**前景点**；`IN_RANGE(cr1,cr2,dfcolor)` 每通道容差；双指针（left/right）早停 |
| **不透明模板** | 否则 | `real_match` | 转灰度 + `gray_norm`=灰度和；积分图快速拒绝 + 灰度 SAD |

- `get_match_points`：把"非四角背景色"的像素压成 `(i<<16)|j` 前景点表（透明模板专用）。
- `trans_match`：遍历前景点，超 `max_err_ct` 立即返回 0。
- `real_match`：先 `abs(tnorm - region_sum(...)) > tnorm*(1-sim)` 用积分图 O(1) 整块拒绝；再做逐像素 `err += abs(*p1-*p2)`，超 `maxErr=(1-sim)*tnorm` 拒绝。

## 3. 参数语义纠正（★重点，和"大漠"心智模型差异最大）

1. **`delta_color` 不是"透明色"！**
   `dfcolor.str2color(delta_color)` 把字符串按 `%02X%02X%02X` 解析成 `color_t`（R/G/B 三字节），
   而 `color_t` 的 `df` 偏色字段**不会被设置**（仍为 0）。
   `trans_match` 里 `IN_RANGE(cr1,cr2,dfcolor)` 实际用 `dfcolor.b/g/r` 当作**每通道绝对容差**：
   - `"000000"` → 容差 0（精确匹配）
   - `"202020"` → 每个通道允许 ±32
   - 即 `delta_color` 的语义是 **"每通道颜色容差"**，且**只对透明模板生效**；不透明模板完全忽略它。

2. **透明色是自动检测的**，来自模板图片四角是否同色（`check_transparent`），用户无法显式指定透明色。

3. **`sim` 重映射**：`sim = 0.5 + sim/2`。
   用户侧 sim=0 → 内部 0.5（允许约 50% 像素误差！）；sim=1 → 1.0（精确）；sim=0.9 → 0.95。
   这与"大漠 sim 直接当容差"的直觉不同，低 sim 端非常宽松。

4. `dir` 0–8 九向扫描，`point_precedes_in_dir` 已定义全部方向语义，测试实测一致。

## 4. 加速手段

- **积分图**：`record_sum` 建前缀和，`real_match` 入口 O(1) 整块灰度和快速拒绝（绝大多数非匹配位秒拒）。
- **分块多线程**：`scan_block_count` 按 `width>height` 决定按行/列切，`ThreadPool` 并发扫各块，最后 `point_precedes_in_dir` 选最优。
- **透明图只比前景点**：背景占比越高越快。
- 公开 `op::Op::FindPic` 走的是 **`FindPicTh` 多线程版**（非单线程 `FindPic`）。

## 5. FindPic / FindPicEx / FindPicExS 差异

| 方法 | 返回 | 结果条数 | 备注 |
|---|---|---|---|
| `FindPic` | `ret`=模板下标 / `x,y`=坐标 | 1（首个，按 dir 序） | 最常用 |
| `FindPicEx` | `"id,x,y\|id,x,y\|..."` | 全部，上限 `_max_return_obj_ct=1800`，按 dir 排序 | `returnID=true` |
| `FindPicExS` | 同上但用**模板名**代替 id | 同上 | `returnID=false` |

多模板：`files` 用 `|` 分隔；`FindPic` 返回 dir 顺序下**第一个**命中的模板下标。

## 6. 坐标与并发

- 返回坐标 = 绝对屏幕坐标（`_x1+_dx` 累加）。
- **同 `Op` 实例不可并发 `FindPic`**：`_src/_gray/_sum` 工作缓冲非线程安全（类注释已声明）。
- 全局模板缓存 `g_pic_cache` 用 `shared_mutex` 保护，可跨 `Op` 对象共享（见 `SharedPicCacheIsGlobalAcrossObjects` 测试）。缓存开关 `_enable_cache` 默认 = 1（`ImageSearchService` 构造里 `_enable_cache = 1`）。

## 7. 实测覆盖（`tests/image_color_test.cpp`）

已覆盖：
- `FindPicHonorsDirection`（9 向 + FindPicEx + FindPicExS）
- `FindPicReturnsMinusOneWhenTemplateIsMissing`
- `SharedPicCacheIsGlobalAcrossObjects`（全局缓存跨对象）
- `FindPicTransparentOddPointsCountsCenterMismatchOnce`（透明模板 + sim=0.8，奇点计数容错）
- `MissingPicSizeClearsOutputValues`

未覆盖：
- `delta_color` 非零容差的实际效果（如 `"202020"` 影响）
- 不透明模板 `sim<1` 的灰度 SAD 行为
- 多模板优先序（dir 下第一个命中）
- 性能基准

## 8. 发现点（待你决策，本阶段未改动）

风险等级按四阶段约定：必修=崩溃/UB/数据竞争/泄漏；建议修=边界场景可能出错/语义歧义；可选=理论可改、当前不触发。

- **【建议修】`delta_color` 语义与"大漠"不兼容**：大漠 `FindPic` 的 `delta_color`=透明色标记 + `sim`=容差；OP 中它是每通道容差且仅对透明模板生效，透明色由四角自动判定。从大漠迁移的脚本传 `"203020"` 期望当透明色，实际被当容差。→ 建议：文档明确语义 + 可选兼容开关（把 delta_color 同时当透明色候选）。
- **【可选】`sim` 重映射导致低 sim 极宽松**：sim=0 允许约 50% 像素误差，可能超出用户预期；需确认是否符合设计意图。
- **【可选/已知局限】不透明模板走灰度 SAD，丢色彩信息**：同亮度异色图可能误匹配，光照变化敏感。是有意为之的"快"取舍。
- **【可选】透明判定阈值 ≥50% 背景占比**：背景占比 <50% 的图不触发透明优化，前景=全图。
- **【清理】`OpImage.cpp:131-135` 一段被注释掉的 client-offset 死代码**（已无作用，客户区偏移由 capture 后端内部处理，见 GdiCapture 修复）。
- **【并发·已知】同 `Op` 不可并发 `FindPic`**，类注释已声明；缓存本身线程安全。

---
下一步（四阶段）：以上均为发现。若要改动，需你确认风险等级并明确同意后我才动代码。
候选方向：① 仅补充 `delta_color` 容差/透明语义文档与测试；② 修 `OpImage.cpp` 死代码（清理）；③ 补 `delta_color` 兼容（同时支持"透明色"语义）；④ 其他。
