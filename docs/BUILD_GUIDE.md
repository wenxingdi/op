# 构建与环境配置完整指南（零基础版）

> 适用对象：第一次接触本项目的开发者 / 配合大模型（Claude Code、Cursor、WorkBuddy 等）做二次开发的用户。
> 目标：从零装好环境 → 构建出 DLL → 跑通测试 → 运行 OPTool → 会备份会恢复。
> 当前稳定基线：`v2026.09.23-stable`（op = `d02548f`，OPTool = `f756ff0`，2026-09-23 验证）。

---

## 0. 项目组成

| 仓库 | 路径 | 技术 | 产出 |
|---|---|---|---|
| op（插件本体） | `D:\AutoPro\op-master\op` | C++17 / CMake / nmake | `op_x64.dll`（COM 接口）、`op_c_api_x64.dll`（纯 C 接口） |
| OPTool（测试工具） | `D:\AutoPro\OPTool` | C# / .NET 10 WinForms | `OPTestTool.exe`、`WordDictTool.exe` |
| 备份脚本 | `D:\AutoPro\op-master\backup\make_backup.py` | Python | bundle + 发布件快照 + manifest |
| GUI 冒烟器 | `<工作区>\optool_verify` | C#（GuiSmoke） | Designer/UI 改动的自动化验收 |

依赖关系：OPTool 运行时在 `Dll/` 目录加载 `op_c_api_x64.dll`，所以 **op 构建后必须同步 DLL 到 OPTool**（见第 5 节）。

---

## 1. 环境安装（一次性）

按顺序装，每步都给了验证命令，装完必须能输出版本号才算成功。

### 1.1 Visual Studio 2022（含 C++ 工具链）

1. 下载 <https://visualstudio.microsoft.com/zh-hans/downloads/>，选 **Professional**（Community 亦可，见 9.3）。
2. 安装器里勾选工作负载 **「使用 C++ 的桌面开发」**。
3. 验证（装在本机默认路径）：
   ```
   dir "D:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Tools\MSVC"
   ```
   应存在 `14.44.35207` 目录（构建脚本写死了这个版本，见 9.3 换版本方法）。

### 1.2 Windows SDK 10.0.26100

1. VS 安装器 → 修改 → 单个组件 → 搜索 **「Windows 10 SDK (10.0.26100.0)」** 勾选安装。
2. 验证：
   ```
   dir "C:\Program Files (x86)\Windows Kits\10\Include"
   ```
   应存在 `10.0.26100.0` 目录。

### 1.3 CMake

1. 下载 <https://cmake.org/download/> 安装器，勾选 **「Add CMake to system PATH」**。
2. 验证：`cmake --version`（≥3.20 即可）。

### 1.4 Python 3

1. <https://www.python.org/downloads/> 装 3.12+，安装器勾选 **「Add python.exe to PATH」**。
2. 验证：`python --version`。

### 1.5 .NET 10 SDK（OPTool 用）

1. <https://dotnet.microsoft.com/download> 下载 **.NET 10 SDK** 安装。
2. 验证：`dotnet --version`（本机为 10.0.301）。

### 1.6 Git

1. <https://git-scm.com/download/win> 安装（默认选项即可）。
2. 验证：`git --version`。

---

## 2. 首次构建 op（全新机器）

> 已有 `build/_deps/` 的机器**跳过本节**，直接进第 3 节。

```powershell
cd D:\AutoPro\op-master\op

# ① bootstrap 第三方依赖（BlackBone 源码 + OpenCV 5.0.0 编译 + vcpkg/minhook）
#    这是上游脚本，唯一用途就是拉依赖；耗时较长（OpenCV 全量编译）
python build.py

# ② 配置 nmake 构建目录（一次性；^ 为 PowerShell 续行符）
cmake -S . -B build/nmake-x64-Release -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_TOOLCHAIN_FILE=D:/AutoPro/op-master/op/build/_deps/vcpkg/scripts/buildsystems/vcpkg.cmake `
  -DVCPKG_TARGET_TRIPLET=x64-windows-static `
  -DBLACKBONE_ROOT=D:/AutoPro/op-master/op/build/_deps/BlackBone `
  -DBLACKBONE_INCLUDE_DIR=D:/AutoPro/op-master/op/build/_deps/BlackBone/src `
  -DBLACKBONE_LIBRARY=D:/AutoPro/op-master/op/build/_deps/BlackBone/build/nmake-x64/BlackBone/BlackBone.lib `
  -DOPENCV_ROOT=D:/AutoPro/op-master/op/build/_deps/opencv/install/nmake-x64 `
  -DOPENCV_LIB_SUFFIX=500

# ③ 全量构建（之后日常全部走第 3 节的增量脚本）
python build/_wb_build.py
```

成功标志：控制台输出 `exit: 0`，且 `build/nmake-x64-Release/libop/` 下生成 `op_x64.dll`、`op_c_api_x64.dll`、`onnxruntime.dll`。

## 3. 日常增量构建（改完代码就跑）

```powershell
cd D:\AutoPro\op-master\op
python build/_wb_build.py                 # 增量编 op_x64 + op_test
python build/_wb_build.py op_c_api_x64    # 只编 C-API DLL（OPTool 用的那份）
```

- 改任意 `.cpp` 后直接跑，nmake 自动只编受影响的文件（通常 10~60 秒）。
- **新增 .cpp 文件**必须先登记进 `CMakeLists.txt` 对应的目标源列表，再构建，否则不参与编译。
- 日志：`build/nmake-x64-Release/_wb_build_out.txt`。

## 4. 跑测试

```powershell
# 方式一（推荐）：一键全量，自动处理工作目录
powershell -File scripts/run_tests.ps1

# 方式二：手动跑单个用例
cd D:\AutoPro\op-master\op
$env:PATH = "build\nmake-x64-Release\libop;" + $env:PATH
build\nmake-x64-Release\tests\op_test.exe --gtest_filter="OcrTest.*"
```

基线（2026-09-23）：270 ran / 262 PASS / 6 SKIP / 2 FAILED——其中 2 个 FAILED 是键鼠域的既有环境项（机器上虚拟显示驱动多导致），**不是回归**。

**两条铁律**：
1. 测试进程运行期间**禁止构建**（LNK1104 文件占用）。
2. 跑 C++ 测试必须 cd 到仓库根——`op_test` 按当前目录找 `testdata`。

## 5. 同步发布件（构建后必做，否则 OPTool 跑的是旧 DLL）

```powershell
cd D:\AutoPro\OPTool
python sync_op_dll.py
```

脚本把 `build/nmake-x64-Release/libop/` 的最新 DLL 覆盖到全部消费点并逐个校验 sha1：
`op\bin\x64`、`bindings\python\op\bin\x64`、`Common\Dll`、两个工具的 `bin\Release\...\Dll`。

若遇到 `WinError 32`（文件被占用）：关掉正在运行的 OPTestTool/WordDictTool 后重跑。

## 6. 构建并运行 OPTool

```powershell
cd D:\AutoPro\OPTool
dotnet build OPTestTool.sln -c Release
```

- 运行 exe：**右键 → 以管理员身份运行**（截图/注入/全局热键需要）。
  路径：`OPTestTool\bin\Release\net10.0-windows7.0\OPTestTool.exe`
- UI/Designer 改动后必须跑 **GuiSmoke 冒烟**（工作区 `optool_verify`），`EXIT=0` 全绿才算过——这是 Designer 改动的唯一可靠判据。
- 字库工具同理：`WordDictTool\bin\Release\net10.0-windows7.0\WordDictTool.exe`。

## 7. 备份与恢复

### 7.1 升级/大改前做备份（强约定）

```powershell
# ① 两仓库打同名 tag（在各自目录执行）
git tag -a v2026.09.23-stable -m "稳定版本备份点"
# ② 一键备份（自动 bundle 全历史 + 发布件快照 + manifest）
python D:\AutoPro\op-master\backup\make_backup.py
# ③ 校验
python D:\AutoPro\op-master\backup\make_backup.py --verify D:\AutoPro\op-master\backup\<生成的目录>
```

产物在 `D:\AutoPro\op-master\backup\<日期>_<标记>\`：`op-repo.bundle`、`optool-repo.bundle`（单文件全量历史，**两仓库的唯一完整冷备份**）、`op-bin-x64\`、`OPTestTool-Release\`、`manifest.json`（全部文件的 sha1 清单）、`恢复说明.md`。

### 7.2 恢复

- **回滚代码**：`git checkout v2026.09.23-stable`；或从 bundle 冷恢复：`git clone xxx.bundle 新目录` → `git checkout v2026.09.23-stable`。
- **回滚运行件**：从备份目录按 manifest 把 `op-bin-x64` 拷回 `op\bin\x64`，再跑 `sync_op_dll.py`。
- 云上另有 GitHub 私有备份：`wenxingdi/op`（源码 + Release `backup-2026-09-23` 附件）与 `wenxingdi/OPTool`。

## 8. 配合大模型开发

1. 让大模型先读仓库根 **`CLAUDE.md`**（AGENTS.md 与之等效）——里面有真实构建路径、约定、踩坑速查。
2. 提交信息用中文 + `fix:`/`feat:`/`docs:` 前缀；功能完成要在 `docs\CHANGELOG.md` 追加条目，正式报告放 `docs\<年月>\`。
3. 大模型产物三查：是否同步了发布件（第 5 节）、是否跑了 GuiSmoke（第 6 节）、是否提醒你做备份（第 7 节）。

## 9. 常见问题

### 9.1 LNK1104: cannot open file 'op_x64.dll'

测试进程（op_test.exe 或 Python 探针）还占着 DLL。先全部关掉再构建。

### 9.2 工具里「修复没生效」

九成是漏了第 5 节同步。全量查时间戳：`build\libop` 产物晚于 `bin\x64` 和 OPTool 的 `Dll\` 就必须同步。

### 9.3 换 MSVC / VS 版本

`build\_wb_build.py` 写死了 `MSVC 14.44.35207` 与 SDK `10.0.26100.0` 的绝对路径。装了别的版本后，打开该脚本把 `MSVC`、`SDK_VER` 两行改成实际路径即可（`dir "D:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Tools\MSVC"` 查版本目录名）。

### 9.4 git push 走代理报 CONNECT 502 / schannel 握手失败

本机环境变量有 `http_proxy=127.0.0.1:4629`，代理进程抽风时 git 全挂。处理：等代理恢复，或 `git -c http.proxy= -c https.proxy= ...` 临时直连（国内直连 GitHub 通常不通，推荐等代理）。

### 9.5 bash 里 `/d/xxx` 路径喂给 python.exe / git clone 报错

Git Bash 的 `/d/` 风格路径不能传给原生 Windows 程序（会被解析成 `D:\d\...`）。统一用 `D:/...` 绝对路径，或先 `cd` 用相对路径。

### 9.6 PowerShell 中文乱码

终端编码问题。把输出重定向到文件再用编辑器读，不要反复重试命令：
`dotnet build ... > out.txt 2>&1`
