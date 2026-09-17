# 找色 / 多点着色（FindColor 家族）深拆

> 阶段：发现（只读，未改 OP 源码）。对齐 `scripts/FINDPIC_DEEPDIVE.md`。
> 基于源码实测：`ImageSearchService.cpp` / `ImageSearchAlgorithms.cpp` / `Color.h`。

## 一、调用入口与端到端流水线

```
op::Op::FindColor(x1,y1,x2,y2, color, sim, dir, x, y)      // 门面带区域
  └─ with_captured_region()  抓图到 _src，记区域原点 _x1/_y1
  └─ ImageSearchService::FindColor(color, sim, dir, x, y)
       ├─ str2colordfs(color, colors)        解析颜色串 → vector<color_df_t>
       └─ ImageSearchAlgorithms::FindColor(colors, sim, dir, x, y)
            ├─ prepared_color_dfs(colors, sim)   准备每通道容差 df
            └─ for_each_scan_point(range, dir, fn)  按 dir 扫描
                 └─ color_matches_prepared(_src.at, it)  → IN_RANGE
```

返回的 `x = j + _x1 + _dx`、`y = i + _y1 + _dy` 是**绝对屏幕坐标**（与 FindPic 一致）。

## 二、颜色串语法（str2colordfs，Service.cpp:747）

| 写法 | 含义 | 解析结果 |
|---|---|---|
| `"RRGGBB"` | 单色 | `color = RRGGBB`，`df = 000000` |
| `"RRGGBB\|RRGGBB"` | 多色（或） | 多个 `color_df_t`，任一命中即匹配 |
| `"RRGGBB-dfRRGGBB"` | 带显式偏色 | `df` 是**每通道绝对容差**（见下）；`dd` 解析为颜色字节 |
| `"@RRGGBB..."` | 背景色标记 | 仅解析时 `ret=1`，**FindColor 路径算法层未消费**（见第七节） |

- 分隔符：`|` 多色，`-` 偏色。空串 `""` → `colors` 为空 → 永远不匹配、返回 0（不报错）。
- `df` 是**颜色值**（如 `102030` = B±16 / G±32 / R±48），**不是比例**，也**不是大漠的 0~255 偏色标量**。

## 三、容差语义（核心公式）

隐式容差（无 `-df` 时由 `sim` 推导）：

```cpp
// ImageSearchAlgorithms.cpp:21
color_t sim_to_color_diff(double sim) {
    if (sim < 0.0 || sim > 1.0) sim = 1.0;
    const auto diff = static_cast<uchar>(std::ceil((1.0 - sim) * 255.0));
    color_diff.{b,g,r} = diff;   // 三通道统一容差
    return color_diff;
}
// 匹配：Color.h:17  → 每通道 |lhs - rhs| <= df
bool IN_RANGE(T lhs, T rhs, T df) { 三通道各自判断 }
```

| sim | 每通道容差 diff = ceil((1-sim)*255) |
|---|---|
| 1.0 | 0（精确） |
| 0.9 | 26 |
| 0.5 | 128 |
| 0.0 | 255（完全不限制） |

显式 `-df` 优先：若 `has_color_diff(df)`（任一通道非零）则用它，否则用 `sim_to_color_diff(sim)`。

## 四、两种找色家族

### A. 单色族（FindColor / FindColorEx / FindColorNum）
- `FindColor(color, sim, dir, x, y)`：返回**首个**（按 dir 顺序）命中，1 条，`x,y=-1` 为未找到。
- `FindColorEx(color, sim, dir, retstr)`：返回全部 `"x,y|x,y|..."`，上限 `_max_return_obj_ct`，按 dir 顺序。
- `FindColorNum(color, sim)`：全图计数（匹配像素个数），无方向。

### B. 多点着色族（FindMultiColor / FindMultiColorEx）—— ★ 与大漠语法不同
- 语法：`first_color`（主色，支持 `|` 多色）、`offset_color = "dx|dy|color,dx|dy|color,..."`
  - 多偏移点用**英文逗号**分隔；
  - 每点内部用 **`|`** 分隔：`dx|dy|RRGGBB`，颜色可带 `-df`；
  - **不是大漠的 `dx.dy.颜色`（点分隔）**，迁移脚本需注意。
- 匹配：先找主色命中，再对每个偏移点检查 `currentColor at (j+dx, i+dy)` 是否匹配该点颜色（越界点算 err）。
- **容错率**（非逐像素）：`max_err_ct = offset_color.size() * (1 - sim)`
  - sim=0.9 → 允许 10% 的偏移点不命中；sim=1 → 0 个容忍。
- `FindMultiColorEx` 同理返回全量 `"x,y|..."`。

## 五、CmpColor / GetColor（取色对照）

- `CmpColor(x, y, scolor, sim)`：`GetPixel` 取点 → `str2colordfs` → `color_matches(color, vcolor, sim)` → 命中返回 1。
- `GetColor(x, y)`：返回该点 `RRGGBB` 字串（门面层，实现见 OpImage）。
- `GetPixel` 不对外，仅算法层内部用（已在能力清单纠正）。

## 六、dir 扫描方向（与 FindPic 完全共用）

`for_each_scan_point`（Algorithms.cpp:469）方向定义：

| dir | 含义 |
|---|---|
| 0 | 左上→右下（行优先） |
| 1 | 左下→右上 |
| 2 | 右上→左下 |
| 3 | 右下→左上 |
| 4 | 中心螺旋（按距中心距离） |
| 5 | 上→下按列 |
| 6 | 右→左按列 |
| 7 | 上→下单列倒序 |
| 8 | 右→左单列倒序 |

`normalize_dir`：越界值归 0。找色/找图共用同一实现 → 此前实测"9 向行为一致"成立。

## 七、★ 关键不一致：找色 vs 找图 的 sim 语义

| | 入口是否重映射 sim | 实际容差（sim=0.9 时） |
|---|---|---|
| **FindColor / FindMultiColor** | **否**（直接传 `sim`） | `ceil((1-0.9)*255)=26`/通道 |
| **FindPic / FindPicEx** | **是**（`sim = 0.5 + sim/2`，Service.cpp:272/288） | 内部 sim=0.95 → `ceil(0.05*255)=13`/通道 |

→ **同一 `sim=0.9`，找色比找图"宽 2 倍容差"**。从大漠迁移时，用户通常对 `sim` 有统一预期，两族实际精度不同，易产生"为什么找图更严/找色更松"的困惑。

另外 `@背景色` 标记：仅在 `FindColorBlock` 的 `str2binaryfbk` 路径生效；在 `FindColor` 路径被 `str2colordfs` 解析（返回 ret=1）但**算法层未使用**，即 `FindColor("@bk...", ...)` 的 `@` 是**无效**的。

## 八、测试覆盖与发现点（待你决策，本阶段未改代码）

### 已覆盖（image_color_test.cpp）
- FindColor / FindColorEx（9 向 dir 实测一致）
- FindMultiColor / FindMultiColorEx（偏移点语法、`-df`）
- CmpColor / FindColorNum

### 发现点
1. **【建议修 / 一致性】sim 语义两族不一致**（第七节）—— 要么文档显式标注，要么（破坏性）给找色也加重映射。建议先文档化。
2. **【认知澄清】`@背景色` 在 FindColor 无效** —— 若用户以为 FindColor 支持 `@bk` 自动排除背景，会误解；需文档说明仅在 FindColorBlock 生效。
3. **【认知澄清】多点偏移语法是 `dx|dy|RRGGBB`（逗号分点）**，非大漠 `dx.dy.颜色`（点分隔）—— 迁移坑。
4. **【可选 / 边界】`FindColor("")` 空色串永远返回 0**（不报错），属预期还是应异常，低优先级。
5. **【可选 / 测试缺口】** 未专门覆盖：sim 容差精确值（如 sim=0.9→26）、找色与找图 sim 不一致、空色串行为。

---
附：与 FindPic 深拆的关系 —— FindPic 用灰度 SAD + 积分图（色盲、有 `0.5+sim/2` 重映射）；FindColor 族用真彩每通道 `IN_RANGE`（色彩敏感、无重映射）。两者 `dir`/坐标体系一致，但匹配内核与容差口径不同。
