# algorithm/ 模块分析（2026-10-15，逐模块专项第 14 弹·收官）

> 范围：`libop/algorithm/` 三个 header-only 共 530 行——AStar.h（八方向
> 寻路本体）、MiniImage.h（极简 BGR 容器，今天刚自 OCR/YOLO 双引擎提取
> 单源化）、PathTools.h（障碍网格/LOS/RDP/点串解析/多边形）。
> 测试覆盖：tests/algorithm_test.cpp + op_algorithm_windows_test.cpp。

## 结构总览

```
AStar.h      199  二叉堆 A*，Chebyshev 启发，禁止斜穿墙角，kMaxCells 64M 上限
MiniImage.h   95  BGR 容器 + to_bgr/resize_bilinear（2026-10-15 已单源化）
PathTools.h  236  位图→降采样障碍网格 + Bresenham LOS + RDP + 点串解析
```

整体评价：三个文件都带完整注释与语义说明，MiniImage.h 今天已收拢过。
剩余空间在**两处效率**（都是实在的、可论证等价的）与一处微重复。

## AL1【效率·中】build_astar_map_from_bgra 每像素一次整数除

**位置**：`PathTools.h` L85-96。内层循环对每个像素做 `x / scale`（L94），
整数除 20-40 周期；1920×1080 图 = 约 200 万次除法 ≈ 20-40ms，而
SetAStarMap 是同步用户 API。`y / scale` 已按行提升（L86），x 侧漏了。
**方案**：行内用"列所属格游标 + 剩余计数"累进（scale 为 1 时快速路径直拷
亮度比较），行为等价（x 非负，除法语义与计数器一致）。预计该函数耗时降
一半以上。顺带可把 `row[x*4]` 改为指针步进，消掉每像素的 size_t 乘法。

## AL2【效率·中】AStar::findpath 消除 closed[] 数组

**位置**：`AStar.h` L76/L90-93/L114。每次寻路分配三个辅助向量
（g_score/parents/closed），closed 用 vector<bool> 位压缩，每次访问带
位运算开销。该启发式（八方向等代价 + Chebyshev）是 consistent 的：
节点首次弹出即最优，之后弹出的同节点条目 g 必然更大。
**方案**：用陈旧判据 `current.g > g_score[current_index]` 取代 closed
检查（惰性删除，教科书标准做法），删掉 closed 向量——辅助内存 3 份变 2 份，
大图上少分配/清扫最多 64M 位的位压缩数组。⚠️ 动的是核心算法路径，等价
性论证依赖启发式一致性（本实现满足：正交/斜移一步 h 恰降 1 = 代价 1），
且已有 algorithm_test 全量回归网。若保守，可只在注释里写明一致性前提。

## AL3【精简·低】set_map / set_map_grid 尺寸校验+分配重复

**位置**：`AStar.h` L31-46 / L49-60。两函数开头`_mapSize` 赋值、cells
计算、合法性判据、kMaxCells 检查、`_walls.clear()` 失败路径完全同构。
**方案**：提 `bool prepare_map(int w, int h, size_t &cells)`（置 _mapSize、
算 cells、判非法+清 _walls），两函数各留一行调用。-8 行，零行为变化。

## 观察项（不动）

- **MiniImage.h to_bgr 三 bpp 分支**：逐行直白，模板化省 ~5 行但损可读性，
  且该文件今天刚单源化，不动。
- **parse_points 的 std::stod 异常驱动**：点串短、调用低频，异常开销无感，
  不动。
- **findpath 终点->起点输出顺序**：对外契约（大漠语义），不动。
- **point_in_polygon 的 1e-12 防除零**：数值稳妥写法，不动。

## 处置记录

用户已确认（2026-10-15）：**全做（AL1-AL3）**。实施完成于当日，构建零错误，
全量回归 401 ran / 399 PASS（唯一失败 MouseKeyTest.WaitKeyScanAllWithWaitFindsKey
为既有环境 flake）/ 1 SKIPPED（FurMark 缺席），algorithm 相关用例全绿，
与基线一致；DLL 同步副本全部一致。代码净 +20 行（+43/-23，效率改动的
游标/注释开销大于删的 closed[] 等行数，行数非本批指标）。

| 项 | 处置 | 结果 |
|---|---|---|
| AL1 | 已做：x/scale 整数除 → 格内剩余列数游标 + 行指针步进 | 回归绿 |
| AL2 | 已做：closed[] → 惰性删除（`current.g > g_score[idx]` 判陈旧） | 回归绿 |
| AL3 | 已做：prepare_map 收拢双函数同构前导 | 回归绿 |

**实现细节**：

- AL1 游标语义：`col_in_cell` 每像素自增，满 scale 进一格——x 非负时与
  `x / scale` 逐点等价；scale > img_w 时全行留在 gx=0（ceil 语义正确）。
- AL2 等价性论证写进注释：Chebyshev + 八方向等代价是 consistent 启发式
  （每步 h 恰降 1 = 代价 1），节点首次弹出即最优，残留旧条目 g 必更大；
  邻接侧原 `closed[next]` 检查由 `next_g >= g_score[next]` 天然涵盖
  （consistency 保证已弹出节点的最优 g 不会被后续路径超越）。
  `next_g` 改用 `current.g + 1`（通过陈旧判据后二者恒等）。
- AL3 注意点：set_map_grid 的 `blocked.size() != cells` 分支单独保留清墙
  语义（原版参数非法同样清墙 = 全图视为墙，而非保留旧图）。
