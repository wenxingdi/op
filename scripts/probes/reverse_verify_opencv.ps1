# OpenCV 优化：反向验证脚本（2026-11-21）
# 用法：pwsh -File scripts\probes\reverse_verify_opencv.ps1
#
# 验证两处优化的断言有效性：
#   RV-CV1：去掉 pyramidFindFirstMatch 的候选裁剪（恢复 12）不会让测试变红
#           —— 这一项是**纯性能优化**，行为等价，因此预期 GREEN（用于确认无副作用）
#   RV-CV2：把 refineScaleCandidate 的金字塔分支去掉，回退纯全分辨率
#           —— 同样行为等价，预期 GREEN
#   RV-CV3：**故意把金字塔精修的坐标偏移写错**（不加 roi 原点）
#           —— 这会让位置出错，OpenCv* 用例必须变红（证明坐标断言有效）
#
# 关键前提（2026-11-21 实证）：本仓库改 .cpp 也可能因依赖跟踪失效而不重编，
# 故每个场景都**强制删除 obj** 并校验时间戳。

$ErrorActionPreference = "Stop"
$repo = "D:\AutoPro\op-master\op"
$ws = "$repo\workbench\tmp"
$python = "D:\Program Files\Python312\python.exe"
$src = "$repo\libop\opencv\TemplateMatcher.cpp"
$objs = @(
    "$repo\build\nmake-x64-Release\libop\CMakeFiles\op_x64.dir\opencv\TemplateMatcher.cpp.obj",
    "$repo\build\nmake-x64-Release\libop\CMakeFiles\libop.dir\opencv\TemplateMatcher.cpp.obj"
)

New-Item -ItemType Directory -Force -Path $ws | Out-Null
$env:TEMP = $ws; $env:TMP = $ws

function Force-Build {
    foreach ($o in $objs) { if (Test-Path $o) { Remove-Item $o -Force } }
    & $python "$repo\scripts\build\_wb_build.py" *>$null
    $log = "$repo\build\nmake-x64-Release\_wb_build_out.txt"
    $errs = Select-String -Path $log -Pattern 'error C\d|error LNK' -ErrorAction SilentlyContinue
    if ($errs) { throw "BUILD FAILED: $($errs[0].Line)" }
}

function Run-CvTests {
    $env:PATH = "$repo\build\nmake-x64-Release\libop;" + $env:PATH
    Push-Location $repo
    $out = & "$repo\build\nmake-x64-Release\tests\op_test.exe" --gtest_filter="OpenCv*:*Cv*" 2>&1
    Pop-Location
    return ($out -join "`n")
}

$scenarios = @(
    @{
        Name     = "RV-CV3-pyramid-offset-broken"
        Desc     = "金字塔精修坐标不加回 roi 原点（故意的真 bug）"
        Find     = "                pyramid_match.x += roi.x;"
        Repl     = "                pyramid_match.x += 0; // SABOTAGE: forgot roi origin"
        ExpectRed = $true
    },
    @{
        Name     = "RV-CV1-candidates-restored-to-12"
        Desc     = "候选数恢复 12（纯性能项，行为应等价）"
        Find     = "                               kPyramidFirstMatchCandidates, results, false, kPyramidCoarseThresholdRelax, nullptr,"
        Repl     = "                               kPyramidCoarseCandidates, results, false, kPyramidCoarseThresholdRelax, nullptr,"
        ExpectRed = $false
    },
    @{
        Name     = "RV-CV2-pyramid-branch-disabled"
        Desc     = "refineScaleCandidate 不用金字塔（回退全分辨率，行为应等价）"
        Find     = "    if (shouldTryPyramid(rectToRegion(roi), templ.width, templ.height, opcv::StripMode::None)) {"
        Repl     = "    if (false && shouldTryPyramid(rectToRegion(roi), templ.width, templ.height, opcv::StripMode::None)) {"
        ExpectRed = $false
    }
)

$results = @()
foreach ($s in $scenarios) {
    $backup = "$src.rvbak"
    Copy-Item $src $backup -Force
    $text = [System.IO.File]::ReadAllText($src)
    $hits = ([regex]::Matches($text, [regex]::Escape($s.Find))).Count
    if ($hits -ne 1) {
        Write-Host "[$($s.Name)] PATTERN-MISS hits=$hits -- NOT verified" -ForegroundColor Yellow
        $results += [pscustomobject]@{ Scenario = $s.Name; Patch = "MISS($hits)"; Result = "n/a" }
        Copy-Item $backup $src -Force; Remove-Item $backup -Force
        continue
    }
    $idx = $text.IndexOf($s.Find)
    [System.IO.File]::WriteAllText($src, $text.Substring(0,$idx) + $s.Repl + $text.Substring($idx + $s.Find.Length))
    (Get-Item $src).LastWriteTime = Get-Date
    $tPatch = Get-Date
    Write-Host "[$($s.Name)] $($s.Desc) -- patched, rebuilding..." -ForegroundColor Cyan

    $status = "BUILD-FAILED"
    try {
        Force-Build
        $stale = $objs | Where-Object { -not (Test-Path $_) -or (Get-Item $_).LastWriteTime -lt $tPatch }
        if ($stale) {
            $status = "INVALID-STALE-OBJ"
            Write-Host "  obj NOT rebuilt" -ForegroundColor Red
        } else {
            $out = Run-CvTests
            $red = $out -match '\[  FAILED  \]'
            if ($s.ExpectRed) {
                $status = if ($red) { "RED-as-expected" } else { "STILL-GREEN(BAD)" }
            } else {
                $status = if ($red) { "UNEXPECTED-RED(BAD)" } else { "GREEN-as-expected" }
            }
            Write-Host "  -> $status" -ForegroundColor $(if ($status -like "*BAD*") { "Red" } else { "Green" })
        }
    } catch {
        Write-Host "  build error: $_" -ForegroundColor Red
    }

    Copy-Item $backup $src -Force
    (Get-Item $src).LastWriteTime = Get-Date
    Remove-Item $backup -Force
    $results += [pscustomobject]@{ Scenario = $s.Name; Patch = "applied"; Result = $status }
}

Write-Host "`n=== restore clean build ===" -ForegroundColor Cyan
Force-Build

Write-Host "`n=== summary ==="
$results | Format-Table -AutoSize
$bad = ($results | Where-Object { $_.Result -like "*BAD*" -or $_.Result -eq "INVALID-STALE-OBJ" }).Count
Write-Host "clean: $($bad -eq 0)" -ForegroundColor $(if ($bad -eq 0) { "Green" } else { "Red" })

Write-Host "`n=== final sanity ==="
(Run-CvTests) -split "`n" | Select-String -Pattern "^\[  PASSED  \]|^\[  FAILED  \]" | ForEach-Object { $_.Line }
