# capstone（反汇编引擎，静态库）

op 的 `Disassemble` 用它把机器码解成汇编文本。**不通过 vcpkg**，由
`scripts/build/_wb_build_asm_libs.py` 本地源码构建。

| 项 | 值 |
|---|---|
| 版本 | capstone 5.0.3 |
| 源码 | `https://codeload.github.com/capstone-engine/capstone/tar.gz/refs/tags/5.0.3` |
| sha256 | `3970c63ca1f8755f2c8e69b41432b710ff634f1b45ee4e5351defec4ec8e1753` |
| 许可证 | BSD-3-Clause，见 `LICENSE` |
| 产物 | `lib/x64/capstone.lib`、`lib/x86/capstone.lib` |

构建口径：Ninja + MSVC 14.44，`/MT /O2`，`BUILD_SHARED_LIBS=OFF`，
`CAPSTONE_BUILD_STATIC_RUNTIME=ON`（内嵌静态运行库），
`CAPSTONE_ARCHITECTURE_DEFAULT=OFF` + `CAPSTONE_X86_SUPPORT=ON`（只编 x86 后端）。

## 为什么只编 x86 后端

`Disassemble(bytes_hex, address, count, arch)` 只暴露 x86/x64（`arch` 32/64），
没有 ARM/MIPS 等需求。全架构静态库约 3 倍体积，编进来只是白占 DLL 空间；
真要支持别的架构，改一行 CMake 选项重跑脚本即可。

## 无需源码改动

capstone 的 `CAPSTONE_EXPORT` 本身就受 `CAPSTONE_SHARED` 控制，静态构建下为空，
不会像 keystone 那样把 `cs_*` 顶进 op.dll 导出表。使用侧只需定义
`CAPSTONE_STATIC`（`libop/CMakeLists.txt` 已统一定义），口径与上游一致。

## 重新构建

```bash
python scripts/build/_wb_build_asm_libs.py --lib capstone           # 双架构
python scripts/build/_wb_build_asm_libs.py --lib capstone --arch x64
```
