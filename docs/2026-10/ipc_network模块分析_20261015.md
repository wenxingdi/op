# ipc/ + network/ 模块分析（2026-10-15，逐模块专项第 13 弹）

> 范围：`libop/ipc/`（SharedMemory 93 / ProcessMutex 104 / Pipe 190+37 /
> CommandRunner 23，共 447 行）与 `libop/network/`（HttpClient 317+52，共 369 行）。
> 两模块合计 816 行，是逐模块专项的收尾小模块。

## 结构总览

```
SharedMemory.h    93  命名内存映射，open/open_create 双模式，RAII close
ProcessMutex.h   104  跨进程互斥，abandoned 检测 + lock_count 守卫（已有注释）
Pipe.h/.cpp      227  子进程管道：双管道 + 读线程 + 进程树终止 + CancelSynchronousIo 兜底
CommandRunner.h   23  Pipe 派生：收输出字符串（唯一使用方 OpProcess::GetCmdStr）
HttpClient.h/.cpp 369  WinHTTP POST + base64 + 端点配置（已整体移出构建）
```

整体评价：两模块都是**已被精心维护过的代码**——ProcessMutex 有 abandoned
语义注释与 unlock 守卫，Pipe 的 reader 阻塞读/CancelSynchronousIo 兜底都有
完整归因注释。network/ 是唯一大件，但属**有意保留的死代码**。

## network/ 整体死代码——有意保留，不动

HttpClient.h 顶部即 2026-09-18 决策注释：OCR/YOLO 不走 HTTP 远程后端，
本模块移出 CMake 源列表（`libop/CMakeLists.txt` L194 注释行）。调用方
OcrService.cpp / HttpYoloEngine.cpp 的 HTTP 分支全部包在
`#ifdef OP_ENABLE_HTTP_OCR_BACKEND` / `OP_ENABLE_HTTP_YOLO_BACKEND` 里
（宏未定义，走"引擎不可用"分支），CMake L271 同样注释掉 HttpYoloEngine.cpp。
恢复路径在三处注释里写明了。**删除会与已记录的决策相悖，且 369 行留着
不占任何构建/运行成本——不动，仅在此登记。**

## IN1【精简·低】Pipe::close 收尾关闭清单重复

**位置**：`Pipe.cpp` L104-105 与 L134-139。开头已 `SAFE_CLOSE(_hwrite2)` /
`SAFE_CLOSE(_hread2)`（L104-105 的用途是关 stdin 写端给子进程 EOF 信号），
收尾扫荡清单 L134-139 又把 `_hread/_hwrite/_hwrite2/_hread2` 全部再关一遍。
SAFE_CLOSE 幂等，纯属无害冗余。
**方案**：收尾清单收窄为只关剩余三枚（_hread/_hwrite/_pi.hProcess/_pi.hThread
中未提前关的两枚管道句柄 + 进程/线程句柄，具体以代码为准），-4 行，无行为变化。

## IN2【精简·低·可选】Pipe 六枚裸句柄 RAII 化

**位置**：`Pipe.h/.cpp`。仓库已有 `op::win32::unique_handle`（带 release()），
Pipe 仍手动 SAFE_CLOSE 六枚句柄 + ZeroMemory 三结构。
**方案**：unique_handle 化，消掉全部 SAFE_CLOSE 样板，预计 -25 行。
风险点：句柄生命周期有特殊性——`_hwrite`/`_hread2` 在 CreateProcess 成功后
立刻转移关闭（继承句柄不在子进程存活期由父进程持有），换 RAII 需用
release()/提前 reset，属"动句柄生命周期路径"的改动。而 Pipe 唯一使用方是
CommandRunner::GetCmdStr（OpProcess::GetCmdStr 跑外部命令，冷路径）。
**裁定建议：可不做**——收益是行数，动的是进程/管道正确性路径，性价比不如
不动；若用户要求行数指标再做。

## 观察项（不动）

- **ProcessMutex::lock() 未 open 时静默**（WaitForSingleObject(nullptr) →
  WAIT_FAILED，无任何提示）：unlock 已有"守卫先于 assert"的范式，lock 加
  同类守卫价值极低（调用方全都先 open_create/open 成功才 lock），不动。
- **SharedMemory::open_create 的 mapped_size 取 RegionSize**：打开已存在的
  更大 mapping 时 _size 是实际 mapping 大小而非请求大小——读侧用法本就
  按实际大小工作，语义正确，不动。
- **CommandRunner 命名**（_readed）：拼写瑕疵但属公开行为无关的私有成员，
  改名动 OpProcess 无关、收益为零，不动。

## 处置记录

用户已确认（2026-10-15）：**仅 IN1**；IN2（Pipe 裸句柄 RAII 化）按"动进程/
管道正确性路径、性价比不如不动"的建议跳过；network/ 死代码按 09-18 决策
继续保留。

- **IN1 已做**：收尾清单删除 `_hwrite2`/`_hread2` 两行重复关闭（净 -2 行
  代码，+4 行归因注释说明各路径职责：成功路径 _hread2 在 open() CreateProcess
  后即关、失败路径 -4 由 close() 开头兜底、-2/-3 的 _hwrite 靠收尾扫荡）。
- 构建零错误；全量回归 401 ran / 399 PASS（唯一失败 MouseKeyTest.
  WaitKeyScanAllWithWaitFindsKey 为既有环境 flake）/ 1 SKIPPED（FurMark 缺席），
  与基线一致；DLL 同步副本全部一致。
