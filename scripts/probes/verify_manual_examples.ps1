#!/usr/bin/env pwsh
# verify_manual_examples.ps1 — 校验 C API 手册里的使用案例真的能编译。
#
# 为什么需要：手册案例承诺"可运行"，但手写案例极易写错签名（本轮实测：
# 初稿 8 个案例里 6 个把签名写成了带 int* 出参的形式，而 op_c_api.h 里
# OpBindWindow/OpCapture/OpLeftClick 等根本没有出参；还臆造了不存在的
# OpFreeString）。不编译一遍根本发现不了。
#
# 用法：pwsh -File scripts/probes/verify_manual_examples.ps1
# 退出码：0 = 全部编译通过；非 0 = 有案例编译失败
$ErrorActionPreference = "Stop"

$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)   # op/
$py = "D:\Program Files\Python312\python.exe"
if (-not (Test-Path $py)) { $py = "python" }

Write-Host "[1/3] 重新生成手册（顺带校验案例引用的函数都真实存在）"
& $py (Join-Path $root "scripts\gen_c_api_manual.py")
if ($LASTEXITCODE -ne 0) { Write-Error "手册生成失败（退出码 $LASTEXITCODE）"; exit $LASTEXITCODE }

Write-Host "[2/3] 从手册抽出 C 案例"
& $py (Join-Path $root "scripts\probes\_extract_manual_examples.py")
if ($LASTEXITCODE -ne 0) { Write-Error "案例抽取失败"; exit $LASTEXITCODE }

Write-Host "[3/3] 用 MSVC 编译全部案例"
$vcvars = "D:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat"
if (-not (Test-Path $vcvars)) { Write-Error "找不到 vcvars64.bat：$vcvars"; exit 2 }

$tmp = Join-Path $root "workbench\tmp"
$cases = Get-ChildItem $tmp -Filter "_manual_case*.c" | Sort-Object Name
if (-not $cases) { Write-Error "没有抽到任何案例"; exit 3 }

$log = Join-Path $tmp "_manual_compile.log"
$files = ($cases | ForEach-Object { $_.FullName }) -join " "
cmd /c "`"$vcvars`" >nul 2>&1 && cd /d `"$root`" && cl /nologo /W3 /c /I include /Fo:`"$tmp\\`" $files" > $log 2>&1

$errors = Select-String -Path $log -Pattern ": error " -Encoding utf8
if ($errors) {
    Write-Host "=== 编译失败 ===" -ForegroundColor Red
    $errors | ForEach-Object { Write-Host $_.Line }
    exit 1
}

Write-Host "[OK] $($cases.Count) 个 C 案例全部编译通过" -ForegroundColor Green
exit 0
