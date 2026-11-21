# 内存直操作二期反向验证（第二版，2026-11-21）
# 用法：pwsh -File scripts\probes\reverse_verify_memimage.ps1
#
# 第一版 4 场景只有 1 个变红。排查后确认**不是断言太弱**，而是：
#   RV-1 set_offset 被删 -> 新 Op 实例的 _x1 恰好是 0，结果不变（无副作用）
#   RV-3 判空改成 right < left -> w=0 时 create(0,h) 仍成功，扫描 0 列 = 0 像素，结果不变
#   RV-4 绕过首个判空 -> 空图的 right<=left 仍为真，被后一道防线拦下
# 即：代码有**防御纵深**，单点改动会被下游兜住。
#
# 本版改用"能真正造成单点故障"的改动，每个都必须变红。
# 前提：改 .h 会重建大量 TU（约 10~25 分钟/次），耐心等。

$ErrorActionPreference = "Stop"
$repo = "D:\AutoPro\op-master\op"
$ws = "$repo\workbench\tmp"
$python = "D:\Program Files\Python312\python.exe"
$helpers = "$repo\libop\op\OpCaptureHelpers.h"
$objs = @(
    "$repo\build\nmake-x64-Release\libop\CMakeFiles\op_x64.dir\op\OpMemImage.cpp.obj",
    "$repo\build\nmake-x64-Release\libop\CMakeFiles\libop.dir\op\OpMemImage.cpp.obj",
    "$repo\build\nmake-x64-Release\tests\CMakeFiles\op_test.dir\mem_image_test.cpp.obj",
    "$repo\build\nmake-x64-Release\libop\CMakeFiles\op_x64.dir\op\OpImage.cpp.obj",
    "$repo\build\nmake-x64-Release\libop\CMakeFiles\libop.dir\op\OpImage.cpp.obj",
    "$repo\build\nmake-x64-Release\libop\CMakeFiles\op_x64.dir\op\OpYolo.cpp.obj",
    "$repo\build\nmake-x64-Release\libop\CMakeFiles\libop.dir\op\OpYolo.cpp.obj"
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

function Run-Tests {
    $env:PATH = "$repo\build\nmake-x64-Release\libop;" + $env:PATH
    Push-Location $repo
    $out = & "$repo\build\nmake-x64-Release\tests\op_test.exe" --gtest_filter="MemImageTest.*" 2>&1
    Pop-Location
    return ($out -join "`n")
}

# 每个场景：File / Find / Repl / Target
$scenarios = @(
    @{
        Name = "RV-M2-1-bogus-offset"
        Desc = "with_mem_region 用错误的偏移（999,999）而不是清零"
        File = $helpers
        Find = "    context->image_proc.set_offset(0, 0);"
        Repl = "    context->image_proc.set_offset(999, 999); // SABOTAGE: bogus offset"
        Target = "CoordinatesAreImageLocal"
    },
    @{
        Name = "RV-M2-2-wrong-stride"
        Desc = "裁图行跨度用 w 而非源图 img->width（像素斜移）"
        File = $helpers
        Find = "            const unsigned char *s = src + (size_t(top + row) * size_t(img->width) + size_t(left)) * 4;"
        Repl = "            const unsigned char *s = src + (size_t(top + row) * size_t(w) + size_t(left)) * 4; // SABOTAGE"
        Target = "SubRegionIsClampedAndScoped"
    },
    @{
        Name = "RV-M2-3-extra-column"
        Desc = "裁图目标宽度多算 1 列（读越界/垃圾数据）"
        File = $helpers
        Find = "        context->image_proc._src.create(static_cast<int>(w), static_cast<int>(h));"
        Repl = "        context->image_proc._src.create(static_cast<int>(w + 1), static_cast<int>(h)); // SABOTAGE: extra column"
        Target = "SubRegionIsClampedAndScoped"
    },
    @{
        Name = "RV-M2-4-fabricate-on-invalid-handle"
        Desc = "句柄无效时伪造一张纯红图（而不是失败）"
        File = $helpers
        Find = "    if (!img || img->width <= 0 || img->height <= 0 || img->pdata == nullptr)`r`n        return false;"
        Repl = "    if (!img || img->width <= 0 || img->height <= 0 || img->pdata == nullptr) {`r`n        // SABOTAGE: fabricate a red image instead of failing`r`n        img = std::make_shared<Image>();`r`n        img->create(8, 8);`r`n        for (int i = 0; i < 64; ++i) {`r`n            auto *q = reinterpret_cast<unsigned char *>(img->pdata) + i * 4;`r`n            q[0] = 0; q[1] = 0; q[2] = 0xFF; q[3] = 0xFF;`r`n        }`r`n    }"
        Target = "InvalidHandleReturnsDefaults"
    },
    @{
        Name = "RV-M2-5-whole-image-misread"
        Desc = "(0,0,0,0) 不再表示整图（改成 1x1）"
        File = $helpers
        Find = "    if (left == 0 && top == 0 && right == 0 && bottom == 0) {`r`n        right = img->width;`r`n        bottom = img->height;`r`n    }"
        Repl = "    if (left == 0 && top == 0 && right == 0 && bottom == 0) {`r`n        right = 1; // SABOTAGE: not whole image`r`n        bottom = 1;`r`n    }"
        Target = "ZeroRegionMeansWholeImage"
    }
)

$results = @()
foreach ($s in $scenarios) {
    $path = $s.File
    $backup = "$path.rvbak"
    Copy-Item $path $backup -Force
    $text = [System.IO.File]::ReadAllText($path)
    $findN = $s.Find.Replace("`r`n", "`n")
    $replN = $s.Repl.Replace("`r`n", "`n")
    $normalized = $text.Replace("`r`n", "`n")
    $hits = ([regex]::Matches($normalized, [regex]::Escape($findN))).Count
    if ($hits -ne 1) {
        Write-Host "[$($s.Name)] PATTERN-MISS hits=$hits -- NOT verified" -ForegroundColor Yellow
        $results += [pscustomobject]@{ Scenario = $s.Name; Patch = "MISS($hits)"; Result = "n/a" }
        Copy-Item $backup $path -Force; Remove-Item $backup -Force
        continue
    }
    $idx = $normalized.IndexOf($findN)
    $patched = $normalized.Substring(0, $idx) + $replN + $normalized.Substring($idx + $findN.Length)
    if ($text -match "`r`n") { $patched = $patched.Replace("`n", "`r`n") }
    [System.IO.File]::WriteAllText($path, $patched)
    (Get-Item $path).LastWriteTime = Get-Date
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
            $out = Run-Tests
            $red = $out -match '\[  FAILED  \]'
            $named = $out -match [regex]::Escape("FAILED  ] MemImageTest.$($s.Target)")
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

    Copy-Item $backup $path -Force
    (Get-Item $path).LastWriteTime = Get-Date
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
(Run-Tests) -split "`n" | Select-String -Pattern "^\[  PASSED  \]|^\[  FAILED  \]" | ForEach-Object { $_.Line }
