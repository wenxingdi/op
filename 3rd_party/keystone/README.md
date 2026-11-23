# keystone（x86 汇编引擎，静态库）

op 的 `Assemble` 用它把汇编文本编成机器码。**不通过 vcpkg**，由
`scripts/build/_wb_build_asm_libs.py` 本地源码构建（vcpkg 引导自身工具链时
解压 7z 会 `CreateFileW` 失败，见 `docs/CHANGELOG.md` 第五十二批）。

| 项 | 值 |
|---|---|
| 版本 | keystone 0.9.2（上游最后一个 release） |
| 源码 | `https://codeload.github.com/keystone-engine/keystone/tar.gz/refs/tags/0.9.2` |
| sha256 | `c9b3a343ed3e05ee168d29daf89820aff9effb2c74c6803c2d9e21d55b5b7c24` |
| 许可证 | GPLv2 + 商业双授权，见 `LICENSE`（`COPYING`）与 `LICENSE-COM.TXT` |
| 产物 | `lib/x64/keystone.lib`、`lib/x86/keystone.lib` |

构建口径（`_wb_build_asm_libs.py`）：Ninja + MSVC 14.44，`/MT /O2 /std:c++14`，
`-DLLVM_TARGETS_TO_BUILD=X86`（只编 x86 后端），静态运行库。

## 对上游源码的两处改动

两处都由构建脚本**幂等打补丁**，不是手工改完就忘：

### 1. `include/keystone/keystone.h`：`KEYSTONE_EXPORT` 受 `KEYSTONE_STATIC` 控制

原文件在 MSVC 下无条件 `#define KEYSTONE_EXPORT __declspec(dllexport)`。
`dllexport` 会写进**定义该符号的 .obj 的 `.drectve` 段**（`/EXPORT:ks_open`），
链接器无条件照办 —— 也就是 `keystone.lib` 一进 op.dll，`ks_open/ks_asm/...`
十来个符号就**自动出现在 op.dll 的导出表**里，改使用侧头文件也拦不住。
op.dll 的导出表是对外契约（宿主按名 `GetProcAddress`），多出的 `ks_*` 与外部
keystone.dll 撞名会命中错模块，所以在**源头**关掉。

验证：`dumpbin /directives lib/x64/keystone.lib | grep -c "EXPORT:ks_"` → `0`。

### 2. `llvm/keystone/ks.cpp`：裸数字默认十进制（修 radix bug）

`ks_option()` 第一行是无条件 `ks->MAI->setRadix(16);`，而 `KS_OPT_SYNTAX_INTEL`
等**不带 RADIX16 位**的分支只设 dialect、从不改回，导致：

* `mov eax, 100` 编成 `B8 00 01 00 00`（0x100 = 256）；
* **写不出十进制** —— `42d` / `0d42` / `42h` 全部 `KS_ERR_ASM_INVALIDOPERAND`，
  因为 radix 一旦为 16，`0x` 前缀与裸数字等价；
* 官方选项 `KS_OPT_SYNTAX_RADIX16` 形同虚设（默认已是 16）。

改为入口 `setRadix(10)`：裸数字=十进制，`0x` 前缀=十六进制，带 `RADIX16`
位时才切 16（该分支里本来就有 `setRadix(16)`）。十六进制仍可表达 ⇒ 这是
**严格更宽**的字面量语法，不存在"改成十进制就写不出十六进制"的退化。

## 重新构建

```bash
python scripts/build/_wb_build_asm_libs.py --lib keystone            # 双架构
python scripts/build/_wb_build_asm_libs.py --lib keystone --arch x86
```

⚠ 必须用 **VS 自带的 cmake 3.31.6**（脚本已内置）。pip 装的 cmake 4.x 会直接拒绝
keystone 的 `cmake_minimum_required(VERSION 2.8.7)` 与 `cmake_policy(SET CMP0051 OLD)`。
