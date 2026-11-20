# YOLO 模块反向验证
# 用法：pwsh -File scripts\probes\reverse_verify_yolo.ps1
#
# 三场景（都用真实模型 yolo_meta_test.onnx 验证）：
#   RV-Y1：keep_detection 恒真（过滤失效）  -> ClassIdsFilterKeepsOnlyMatching 必须 RED
#   RV-Y2：max_results 不截断               -> MaxResultsTruncatesAndReportsTotal 必须 RED
#   RV-Y3：并集改成交集                      -> LabelsFilterAndUnionSemantics 必须 RED
#
# 前提（本仓库已知坑）：改 .cpp 也可能不触发重编，故强制删 obj 并校验时间戳。

$ErrorActionPreference = "Stop"
$repo = "D:\AutoPro\op-master\op"
$ws = "$repo\workbench\tmp"
$python = "D:\Program Files\Python312\python.exe"
$src = "$repo\libop\op\OpYolo.cpp"
$objs = @(
    "$repo\build\nmake-x64-Release\libop\CMakeFiles\op_x64.dir\op\OpYolo.cpp.obj",
    "$repo\build\nmake-x64-Release\libop\CMakeFiles\libop.dir\op\OpYolo.cpp.obj",
    "$repo\build\nmake-x64-Release\tests\CMakeFiles\op_test.dir\yolo_ex2_test.cpp.obj"
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

function Run-YoloTests {
    $env:PATH = "$repo\build\nmake-x64-Release\libop;" + $env:PATH
    Push-Location $repo
    $out = & "$repo\build\nmake-x64-Release\tests\op_test.exe" --gtest_filter="YoloFilterRealTest.*:YoloOptionTest.*:YoloEx2Test.*" 2>&1
    Pop-Location
    return ($out -join "`n")
}

$scenarios = @(
    @{
        Name = "RV-Y1-filter-always-true"
        Desc = "keep_detection 恒返回 true（过滤完全失效）"
        Find = 'bool keep_detection(const op::vyolo_rec_t &items, size_t idx, const yolo_options_t &opt) {
    const auto &it = items[idx];'
        Repl = 'bool keep_detection(const op::vyolo_rec_t &items, size_t idx, const yolo_options_t &opt) {
    const auto &it = items[idx];
    return true; // SABOTAGE'
        Target = "ClassIdsFilterKeepsOnlyMatching"
    },
    @{
        Name = "RV-Y2-total-reports-filtered"
        Desc = "total 报成过滤后的条数（应报全量）"
        Find = '        root.obj.push_back(key(L"total", num(static_cast<double>(total))));'
        Repl = '        root.obj.push_back(key(L"total", num(static_cast<double>(kept.size())))); // SABOTAGE'
        Target = "ClassIdsFilterKeepsOnlyMatching"
    },
    @{
        Name = "RV-Y3-union-becomes-intersection"
        Desc = "class_ids 与 labels 从并集改成交集"
        Find = '    if (opt.has_class_filter) {
        for (long id : opt.class_ids) {
            if (id == it.class_id)
                return true;
        }
    }
    if (opt.has_label_filter) {
        for (const auto &name : opt.labels) {
            if (name == it.label)
                return true;
        }
    }
    return false;'
        Repl = '    bool cls_ok = !opt.has_class_filter, lbl_ok = !opt.has_label_filter;
    for (long id : opt.class_ids) { if (id == it.class_id) cls_ok = true; }
    for (const auto &name : opt.labels) { if (name == it.label) lbl_ok = true; }
    return cls_ok && lbl_ok; // SABOTAGE: intersection'
        Target = "LabelsFilterAndUnionSemantics"
    }
)

$results = @()
foreach ($s in $scenarios) {
    $backup = "$src.rvbak"
    Copy-Item $src $backup -Force
    $text = [System.IO.File]::ReadAllText($src)
    $find = $s.Find.Replace("`r`n", "`n")
    $repl = $s.Repl.Replace("`r`n", "`n")
    $normalized = $text.Replace("`r`n", "`n")
    $hits = ([regex]::Matches($normalized, [regex]::Escape($find))).Count
    if ($hits -ne 1) {
        Write-Host "[$($s.Name)] PATTERN-MISS hits=$hits -- NOT verified" -ForegroundColor Yellow
        $results += [pscustomobject]@{ Scenario = $s.Name; Patch = "MISS($hits)"; Result = "n/a" }
        Copy-Item $backup $src -Force; Remove-Item $backup -Force
        continue
    }
    $idx = $normalized.IndexOf($find)
    $patched = $normalized.Substring(0, $idx) + $repl + $normalized.Substring($idx + $find.Length)
    if ($text -match "`r`n") { $patched = $patched.Replace("`n", "`r`n") }
    [System.IO.File]::WriteAllText($src, $patched)
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
            $out = Run-YoloTests
            $red = $out -match '\[  FAILED  \]'
            $named = $out -match [regex]::Escape("FAILED  ] YoloFilterRealTest.$($s.Target)")
            if ($red -and $named) {
                $status = "RED-as-expected"
            } elseif ($red) {
                $status = "RED-but-other-test"
                ($out -split "`n" | Where-Object { $_ -match '\[  FAILED  \]' }) | Select-Object -First 4 | ForEach-Object { "      $_" }
            } else {
                $status = "STILL-GREEN(BAD)"
            }
            Write-Host "  -> $status" -ForegroundColor $(if ($status -eq "RED-as-expected") { "Green" } else { "Red" })
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
$bad = ($results | Where-Object { $_.Result -ne "RED-as-expected" }).Count
Write-Host "verified RED: $(($results | Where-Object { $_.Result -eq 'RED-as-expected' }).Count) / $($results.Count)" -ForegroundColor $(if ($bad -eq 0) { "Green" } else { "Red" })

Write-Host "`n=== final sanity ==="
(Run-YoloTests) -split "`n" | Select-String -Pattern "^\[  PASSED  \]|^\[  FAILED  \]" | ForEach-Object { $_.Line }
