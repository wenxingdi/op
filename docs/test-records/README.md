# 历史测试记录（归档）

本目录收录**结论性**的测试原始输出，使记录随仓库一起流通 —— 此前这些文件都在
`workbench/`（.gitignore 内），clone / 下载仓库后无法复查，本机坏盘即永久丢失。

## 归档规则

- **只收定稿级别**：全量回归原始输出、真机与载体判定、A/B 对比、根因定位、
  OpenCV 素材生成与自检。
- **不收过程噪音**：同名迭代中间态（`_t_build_hookfix1..6.txt`）、空文件、
  一次性试错输出 —— 那些仍留在 `workbench/`。
- 每轮收口（备份点）时归档一次，按 `YYYY-MM/` 分目录。
- 采集脚本见 `scripts/probes/`；跑法说明见各脚本 docstring 与 `docs/INDEX.md`。

## 索引（2026-09）

| 分组 | 文件 | 说明 |
|---|---|---|
| 全量回归 | `_last_optest.txt` | 2026-09-29 晚全量原始输出（401 条 = 399 PASS / 1 SKIP / 1 FAILED，唯一 FAILED 为本机 VK133 幽灵按键环境项） |
| | `optest_full_20260928_c.txt` / `optest_full_20260929_a/b.txt` | 各轮全量摘要 |
| | `_t_full_regress_ondemand*.txt` / `_t_full_regress_final_console.txt` | 按需模块加载落地后的全量回归 |
| | `_t_full_regress_dx32.txt` / `_t_full_regress_runapp.txt` | 含 32 位靶子 / RunApp 用例的加长回归 |
| | `optest_after_fullcontent.txt` | 09-19 全量（截屏矩阵改动后） |
| A/B 与归因 | `_t_ab_op-bin-x64.txt` | 用**旧二进制**跑同一 filter → 证明新失败非本轮引入 |
| | `_t_bind_any_out.txt` / `_t_bind_shumen_out.txt` | 任意程序/蜀门绑定探针输出 |
| dx 靶子体系 | `_t_dx32_carrier_out.txt` / `_t_dx32_carrier_report.txt` | x86 D3D9 受控载体四象限像素校验 |
| | `_t_dx32_notepad_out.txt` / `_t_dx32_shumen_out.txt` | 记事本（系统目录遮蔽例外）/ 蜀门真机 32 位绑定 |
| | `_t_scr_probe_out.txt` / `_t_scr_probe_report.txt` | 屏保载体（`Bubbles.scr /p`）路线 |
| 出口层契约 | `return_contract_survey.txt` | 返回值普查（对外负值仅剩 4 处，全被既有测试钉住） |
| | `nullhandle_run1.txt` | null handle 用例运行输出 |
| 挂起定位 | `hang_parse.txt` / `exit_hang_run*.txt` | OCR 轮次进程退出挂起的 minidump 分阶段二分 |
| 历史回归 | `regress_idl_fix*.txt` / `regress_inout_v2.txt` / `regress_leakfix.txt` | IDL 修复 / in-out 语义 / 泄漏修复各轮 |
| 截屏矩阵 | `matrix_all_before.txt` / `matrix_after.txt` / `matrix_after_olddll.txt` / `matrix_bs.txt` / `mk_recheck.txt` | 全模式截屏矩阵，含「旧 DLL」对照 |
| | `capmode_out.txt` / `rm_bs.txt` / `rm_calc.txt` | 捕获模式耗时 / 真机模式实测 |
| OCR 素材 | `ocv_baseline_scan.txt` / `ocv_full_after_assets.txt` / `ocv_assets_gen5.txt` | OpenCV 测试素材生成与生成后全量 |
| 其他 | `smoke_real_out.txt` / `real_machine_stdout.txt` / `idl_inout_verify.txt` / `calc_modules.txt` / `winlist.txt` | 真机冒烟、模块与窗口清单 |

> 注：`assets/` 与载体 exe 仍不入库（体积与许可证原因）。缺素材时 OpenCvTest 的
> 若干用例会 SKIP，属预期行为（详见 `scripts/probes/make_screen_assets.py`）。
