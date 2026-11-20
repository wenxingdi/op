# 反向验证（CLAUDE.md 强制）：故意改错代码，确认对应用例必须 FAIL。
# 用法：pwsh -File scripts\probes\reverse_verify.ps1
#
# 严格模式（第二版，2026-11-21）：第一版有 3 个场景"改错后仍然全绿"，
# 其中一部分是**打补丁没生效**（模式串不匹配）被误判成"断言恒真"。
# 本版对每个场景强制校验：
#   1) 补丁 pattern 必须在文件里唯一命中（0 次 = 报 PATTERN-MISS，>1 次 = 报 PATTERN-AMBIGUOUS）
#   2) 打完补丁后必须真的被编译（构建成功且 .obj 时间戳更新）
#   3) 用例必须 RED
# 三者缺一即判该场景"验证不成立"，不能当通过。
#
# 注意：仓库里 libop/algorithm/StrMatch.h 是 **LF** 行尾，ImageSearchService.cpp /
# OnnxOcrEngine.cpp 需实测确认，故本版统一用 [regex]::Escape + 单行 pattern，
# 并用 -replace 的 MatchEvaluator 做字面替换，避开行尾差异。

$ErrorActionPreference = "Stop"
$repo = "D:\AutoPro\op-master\op"
$ws = "$repo\workbench\tmp"   # 中间产物，不入库
$python = "D:\Program Files\Python312\python.exe"

New-Item -ItemType Directory -Force -Path $ws | Out-Null
$env:TEMP = $ws; $env:TMP = $ws

function Build-Once {
    & $python "$repo\scripts\build\_wb_build.py" *>$null
    $log = "$repo\build\nmake-x64-Release\_wb_build_out.txt"
    $errs = Select-String -Path $log -Pattern 'error C\d|error LNK' -ErrorAction SilentlyContinue
    if ($errs) { throw "BUILD FAILED: $($errs[0].Line)" }
}

# 关键（2026-11-21 实证）：本机构建目录里 **86/233 个 .obj.d 依赖文件是 0 字节**，
# 导致 CMake 的头文件依赖跟踪失效 —— 改 .h 不会触发任何重新编译。
# 实测：touch libop/algorithm/StrMatch.h 后 op_test 的 .obj 时间戳不变。
#
# 这会让"反向验证"彻底失真：打了 saboteur 补丁但测试用的还是旧 obj，结果恒绿。
# 因此每个场景在构建后必须**确认目标 obj 真的被重新编译**，否则判 INVALID。
function Force-Rebuild([string[]]$objPaths) {
    foreach ($o in $objPaths) {
        if (Test-Path $o) { Remove-Item $o -Force }
    }
    Build-Once
}

function Obj-Newer([string]$objPath, [datetime]$since) {
    if (-not (Test-Path $objPath)) { return $false }
    return ((Get-Item $objPath).LastWriteTime -gt $since)
}

function Run-Tests([string]$filter) {
    $env:PATH = "$repo\build\nmake-x64-Release\libop;" + $env:PATH
    Push-Location $repo
    $out = & "$repo\build\nmake-x64-Release\tests\op_test.exe" --gtest_filter=$filter 2>&1
    Pop-Location
    return ($out -join "`n")
}

# 用字面串替换（不经过正则），返回 (success, newText, hitCount)
function Literal-Patch([string]$text, [string]$find, [string]$repl) {
    $hits = ([regex]::Matches($text, [regex]::Escape($find))).Count
    if ($hits -ne 1) { return @($false, $text, $hits) }
    $idx = $text.IndexOf($find)
    $out = $text.Substring(0, $idx) + $repl + $text.Substring($idx + $find.Length)
    return @($true, $out, $hits)
}

$scenarios = @(
    @{
        Name = "RV1-exact-degrades-to-contains"
        Desc = "MatchMode::Exact 的位置约束恒真（exact 退化为 contains）"
        File = "libop\algorithm\StrMatch.h"
        # 不依赖行尾符：只匹配单行，替换后 exact 分支恒真
        Find = "        return h.begin == 0 && h.end == line_len;"
        Repl = "        return true; // SABOTAGE: exact degraded to contains"
        Filter = "StrMatchTest.ExactRejectsSubstring:FindStrBatchTest.ExactModeRejectsSubstringInJson"
        Objs = @("$repo\build\nmake-x64-Release\tests\CMakeFiles\op_test.dir\find_str_batch_test.cpp.obj")
    },
    @{
        Name = "RV2-charrange-becomes-linebox"
        Desc = "cx1/cx2 直接用行框（逐字定位退化为整行）"
        File = "libop\image\ImageSearchService.cpp"
        Find = @(
            "                cx1 = ln.cx1[size_t(h.begin)];"
            "                cx2 = ln.cx2[size_t(h.end) - 1];"
        ) -join "`r`n"
        Repl = @(
            "                cx1 = ln.x1; // SABOTAGE: degrade to line box"
            "                cx2 = ln.x2;"
        ) -join "`r`n"
        Filter = "FindStrBatchTest.CharRangeIsSubstringNotWholeLine"
        Objs = @("$repo\build\nmake-x64-Release\tests\CMakeFiles\op_test.dir\find_str_batch_test.cpp.obj", "$repo\build\nmake-x64-Release\libop\CMakeFiles\op_x64.dir\image\ImageSearchService.cpp.obj", "$repo\build\nmake-x64-Release\libop\CMakeFiles\libop.dir\image\ImageSearchService.cpp.obj")
    },
    @{
        Name = "RV3-double-offset"
        Desc = "逐字 x 上额外再加一次 det 框原点（重复偏移）"
        File = "libop\image\ImageSearchService.cpp"
        Find = "                    v.cx1[c] = ext[idx].chars[c].x1 + off_x;"
        Repl = "                    v.cx1[c] = ext[idx].chars[c].x1 + off_x + (it.second.right_bottom.x - it.second.left_top.x); // SABOTAGE: double offset"
        Filter = "FindStrBatchTest.BatchFindsTargetAndLocatesChars:FindStrBatchTest.CharRangeIsSubstringNotWholeLine"
        Objs = @("$repo\build\nmake-x64-Release\tests\CMakeFiles\op_test.dir\find_str_batch_test.cpp.obj", "$repo\build\nmake-x64-Release\libop\CMakeFiles\op_x64.dir\image\ImageSearchService.cpp.obj", "$repo\build\nmake-x64-Release\libop\CMakeFiles\libop.dir\image\ImageSearchService.cpp.obj")
    },
    @{
        Name = "RV4-fuzzy-always-true"
        Desc = "模糊判据恒真（不比对编辑距离）"
        File = "libop\algorithm\StrMatch.h"
        Find = "        if (best.ed <= t.fuzzy)"
        Repl = "        if (true) // SABOTAGE: fuzzy never rejects"
        Filter = "StrMatchTest.FuzzyRejectsDistantText:StrMatchTest.FuzzyTolerance"
        Objs = @("$repo\build\nmake-x64-Release\tests\CMakeFiles\op_test.dir\find_str_batch_test.cpp.obj")
    },
    @{
        Name = "RV5-det-timing-not-accumulated"
        Desc = "det 计时不累加（profile 的 det_ms 恒 0）"
        File = "libop\ocr\OnnxOcrEngine.cpp"
        # 三处同样的计时行（成功分支 + 两个 catch）；用带前置行的锚点锁定成功分支
        Find = @(
            "    }"
            "    if (ms) *ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t_begin).count();"
            "    if (res.empty()) {"
        ) -join "`r`n"
        Repl = @(
            "    }"
            "    if (ms) *ms = 0.0; // SABOTAGE: never accumulate"
            "    if (res.empty()) {"
        ) -join "`r`n"
        Filter = "OcrProfileTest.SectionsAreConsistent"
        Objs = @("$repo\build\nmake-x64-Release\tests\CMakeFiles\op_test.dir\find_str_batch_test.cpp.obj", "$repo\build\nmake-x64-Release\libop\CMakeFiles\op_x64.dir\ocr\OnnxOcrEngine.cpp.obj", "$repo\build\nmake-x64-Release\libop\CMakeFiles\libop.dir\ocr\OnnxOcrEngine.cpp.obj")
    }
)

$results = @()
foreach ($s in $scenarios) {
    $path = Join-Path $repo $s.File
    $backup = "$path.rvbak"
    Copy-Item $path $backup -Force
    $orig = [System.IO.File]::ReadAllText($path)

    $patched = Literal-Patch $orig $s.Find $s.Repl
    if (-not $patched[0]) {
        Write-Host "[$($s.Name)] PATTERN-MISS (hits=$($patched[2])) -- scenario NOT verified" -ForegroundColor Yellow
        $results += [pscustomobject]@{ Scenario = $s.Name; Patch = "MISS($($patched[2]))"; Tests = "n/a" }
        Copy-Item $backup $path -Force
        Remove-Item $backup -Force
        continue
    }
    [System.IO.File]::WriteAllText($path, $patched[1])
    (Get-Item $path).LastWriteTime = Get-Date
    $tPatch = Get-Date
    Write-Host "[$($s.Name)] $($s.Desc) -- patched (1 unique hit), force-rebuilding..." -ForegroundColor Cyan

    $status = "BUILD-FAILED"
    try {
        # 强制重编受影响 TU（依赖跟踪失效，必须手动删 obj）
        Force-Rebuild $s.Objs
        $stale = @()
        foreach ($o in $s.Objs) {
            if (-not (Obj-Newer $o $tPatch)) { $stale += $o }
        }
        if ($stale.Count -gt 0) {
            $status = "INVALID-STALE-OBJ"
            Write-Host "  obj NOT rebuilt (header dep tracking is broken): $($stale -join ', ')" -ForegroundColor Red
        } else {
            $out = Run-Tests $s.Filter
            $status = if ($out -match '\[  FAILED  \]') { "RED-as-expected" } else { "STILL-GREEN(BAD)" }
            if ($status -like "STILL*") {
                $out -split "`n" | Where-Object { $_ -match '\[  PASSED  \]|\[  FAILED  \]' } | Select-Object -Last 4 | ForEach-Object { "      $_" }
            }
            Write-Host "  -> $status" -ForegroundColor $(if ($status -eq "RED-as-expected") { "Green" } else { "Red" })
        }
    } catch {
        Write-Host "  build error: $_" -ForegroundColor Red
    }

    Copy-Item $backup $path -Force
    (Get-Item $path).LastWriteTime = Get-Date
    Remove-Item $backup -Force
    $results += [pscustomobject]@{ Scenario = $s.Name; Patch = "applied"; Tests = $status }
}

Write-Host "`n=== restoring clean build (force recompile of every touched TU) ===" -ForegroundColor Cyan
$allObjs = @()
foreach ($s in $scenarios) { if ($s.Objs) { $allObjs += $s.Objs } }
Force-Rebuild ($allObjs | Select-Object -Unique)

Write-Host "`n=== summary ==="
$results | Format-Table -AutoSize
$bad = ($results | Where-Object { $_.Tests -ne "RED-as-expected" }).Count
Write-Host "scenarios verified RED: $(($results | Where-Object { $_.Tests -eq 'RED-as-expected' }).Count) / $($results.Count)" -ForegroundColor $(if ($bad -eq 0) { "Green" } else { "Red" })

Write-Host "`n=== final sanity: all new tests must be GREEN again ==="
$final = Run-Tests "StrMatchTest.*:FindStrBatchTest.*:OcrProfileTest.*"
$final -split "`n" | Select-Object -Last 5
