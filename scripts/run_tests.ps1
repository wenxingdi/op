# run_tests.ps1 — op_test 运行器（解决 Windows CONSOLE gtest 在 Git Bash 下无 stdout 的问题）
# 用法: powershell -NoProfile -ExecutionPolicy Bypass -File run_tests.ps1 [-Filter "图案"]
# 依赖: op_x64.dll / op_c_api_x64.dll 需在 build\libop\ 下（nmake 构建产物）
param(
    [string]$Filter = "-*Ocr*:-*OCR*:-*Yolo*",   # 默认排除依赖外部服务的测试
    [string]$BuildDir = ""                        # 留空则自动探测 build\nmake-x64-Release
)

$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $PSScriptRoot          # op/ 根（= op_test 的工作目录，资产按 cwd 查找）

# 自动探测构建目录
if (-not $BuildDir) {
    $candidates = @(
        Join-Path $root "build\nmake-x64-Release"
        Join-Path $root "build\nmake-x86-Release"
    )
    foreach ($c in $candidates) {
        if (Test-Path (Join-Path $c "tests\op_test.exe")) { $BuildDir = $c; break }
    }
    if (-not $BuildDir) { Write-Error "未找到 op_test.exe，请用 -BuildDir 指定"; exit 2 }
}

$exe = Join-Path $BuildDir "tests\op_test.exe"
$dllDir = Join-Path $BuildDir "libop"
if (-not (Test-Path $exe)) { Write-Error "op_test.exe 不存在: $exe"; exit 2 }

# 前置门禁：swig/python 绑定与 libop.h 同步检查（接口面漂移时缺 API 是静默的，必须在跑测试前拦下）
# python 探测要"能跑"才算数：PATH 里可能有失效的 shim（如指向已卸载运行时的别名）
function Find-UsablePython {
    # 返回可用 python 的调用 token 数组（首个为可执行文件，其余为前缀参数）
    $candidates = @()
    $pyLauncher = Get-Command py -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Source
    if ($pyLauncher) { $candidates += @( , @($pyLauncher, "-3") ) }
    $pythonCmd = Get-Command python -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Source
    if ($pythonCmd) { $candidates += @( , @($pythonCmd) ) }
    # PATH 找不到时兜底常见安装位置（本机 python 未注册进 py 也未进 PATH）
    foreach ($extra in @($env:PYTHON64_ROOT, "D:\Program Files\Python312", "C:\Program Files\Python312", "D:\Python312")) {
        if ($extra -and (Test-Path (Join-Path $extra "python.exe"))) {
            $candidates += @( , @( (Join-Path $extra "python.exe") ) )
        }
    }
    foreach ($c in $candidates) {
        # 前缀参数（Count=1 时为空；注意 $a[1..0] 在 PS 里是降序取值，会得到 $null+$a[0]）
        $prefix = @()
        if ($c.Count -gt 1) { $prefix = $c[1..($c.Count-1)] }
        # 探测必须容忍 stderr/非零退出：全局 Stop 偏好会让原生命令的 stderr 直接抛异常
        $prevPref = $ErrorActionPreference
        $ErrorActionPreference = "Continue"
        try {
            & $c[0] ($prefix + @("-c", "import sys; sys.exit(0)")) 2>$null
            $ok = ($LASTEXITCODE -eq 0)
        } catch {
            $ok = $false
        }
        $ErrorActionPreference = $prevPref
        if ($ok) { return , $c }
    }
    return $null
}
$pyExe = Find-UsablePython
if (-not $pyExe) {
    Write-Host "[WARN] 未找到可用的 python，跳过 swig 同步检查（漂移将无法被自动拦截）"
} else {
    Write-Host "绑定同步检查: python scripts\check_swig_sync.py"
    # 函数 return , $c 后 $pyExe 本身就是 token 数组（再取 [0] 会得到字符串首字符）
    $pyTokens = $pyExe
    $pyPrefix = @()
    if ($pyTokens.Count -gt 1) { $pyPrefix = $pyTokens[1..($pyTokens.Count-1)] }
    & $pyTokens[0] ($pyPrefix + @(Join-Path $root "scripts\check_swig_sync.py"))
    if ($LASTEXITCODE -ne 0) {
        Write-Error "swig/python 绑定与 libop.h 存在漂移（退出码 $LASTEXITCODE）。请重新生成：swig\genPythonWrap.bat，或见 docs\BUILD_GUIDE.md §3.1"
        exit 4
    }
    Write-Host "[OK] 绑定三层同步"
}

# 把 DLL 目录加到 PATH（op_x64.dll / op_c_api_x64.dll 在 libop\）
$env:PATH = "$dllDir;$env:PATH"

$outFile = Join-Path $BuildDir "optest_out.txt"
$errFile = Join-Path $BuildDir "optest_err.txt"

$args = @("--gtest_print_time=0")
if ($Filter) { $args += "--gtest_filter=$Filter" }

Write-Host "运行: $exe $($args -join ' ')"
$p = Start-Process -FilePath $exe -ArgumentList $args -NoNewWindow `
    -WorkingDirectory $root `
    -RedirectStandardOutput $outFile -RedirectStandardError $errFile -PassThru
if (-not $p.WaitForExit(180000)) {
    $p.Kill()
    Write-Error "op_test 超时(180s)被杀，输出见 $outFile"
    exit 3
}

# 输出末尾摘要（gtest 的 PASSED/FAILED 统计行）
Write-Host "=== 结果摘要 ==="
Get-Content $outFile -Tail 12
$code = $p.ExitCode
Write-Host "EXIT=$code  (0=全过, 1=有失败)"
exit $code
