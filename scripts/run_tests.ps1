# run_tests.ps1 — op_test 运行器（解决 Windows CONSOLE gtest 在 Git Bash 下无 stdout 的问题）
# 用法: powershell -NoProfile -ExecutionPolicy Bypass -File run_tests.ps1 [-Filter "图案"]
# 依赖: op_x64.dll / op_c_api_x64.dll 需在 build\libop\ 下（nmake 构建产物）
param(
    [string]$Filter = "-*Ocr*:-*OCR*:-*Yolo*",   # 默认排除依赖外部服务的测试
    [string]$BuildDir = ""                        # 留空则自动探测 build\nmake-x64-Release
)

$ErrorActionPreference = "Stop"

# 自动探测构建目录
if (-not $BuildDir) {
    $root = Split-Path -Parent $PSScriptRoot      # op/ 根
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

# 把 DLL 目录加到 PATH（op_x64.dll / op_c_api_x64.dll 在 libop\）
$env:PATH = "$dllDir;$env:PATH"

$outFile = Join-Path $BuildDir "optest_out.txt"
$errFile = Join-Path $BuildDir "optest_err.txt"

$args = @("--gtest_print_time=0")
if ($Filter) { $args += "--gtest_filter=$Filter" }

Write-Host "运行: $exe $($args -join ' ')"
$p = Start-Process -FilePath $exe -ArgumentList $args -NoNewWindow `
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
