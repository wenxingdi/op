# 内存模块（图像句柄注册表）反向验证
# 用法：pwsh -File scripts\probes\reverse_verify_image_registry.ps1
#
# 四场景，每个都必须变红（都是"真实 bug"型改动，不是等价重构）：
#   RV-M1：copy_to_bgra 忽略 stride（用 width*bpp）-> StrideWithPaddingIsRespected 必须 RED
#   RV-M2：crop_image 源偏移漏掉 x0             -> CropMatchesSourceRegion 必须 RED
#   RV-M3：注册表满时改为"清空重存"（静默淘汰）  -> RegistryFullRejectsButKeepsExisting 必须 RED
#   RV-M4：find_str_mem 不校验句柄（直接用空图）  -> FindsTextInsideRegisteredImage 必须 RED
#
# 关键前提（2026-11-21 多次踩坑）：本仓库改 .cpp/.h 都可能不触发重编，
# 故每场景强制删 obj 并校验时间戳晚于补丁时间，否则判 INVALID-STALE-OBJ。

$ErrorActionPreference = "Stop"
$repo = "D:\AutoPro\op-master\op"
$ws = "$repo\workbench\tmp"
$python = "D:\Program Files\Python312\python.exe"
$svcSrc = "$repo\libop\image\ImageSearchService.cpp"
$objs = @(
    "$repo\build\nmake-x64-Release\libop\CMakeFiles\op_x64.dir\image\ImageSearchService.cpp.obj",
    "$repo\build\nmake-x64-Release\libop\CMakeFiles\libop.dir\image\ImageSearchService.cpp.obj",
    "$repo\build\nmake-x64-Release\tests\CMakeFiles\op_test.dir\image_registry_test.cpp.obj"
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

function Run-RegistryTests {
    $env:PATH = "$repo\build\nmake-x64-Release\libop;" + $env:PATH
    Push-Location $repo
    $out = & "$repo\build\nmake-x64-Release\tests\op_test.exe" --gtest_filter="ImageRegistry*" 2>&1
    Pop-Location
    return ($out -join "`n")
}

$scenarios = @(
    @{
        Name   = "RV-M1-stride-ignored"
        Desc   = "copy_to_bgra 忽略 stride（用 width*bpp 当作行跨度）"
        Find   = '        const unsigned char *row = src + size_t(y) * size_t(stride);'
        Repl   = '        const unsigned char *row = src + size_t(y) * size_t(width) * size_t(bpp); // SABOTAGE'
        Target = "ImageRegistryTest.StrideWithPaddingIsRespected"
    },
    @{
        Name   = "RV-M2-crop-missing-x-offset"
        Desc   = "crop_image 源偏移漏掉 x0"
        Find   = '        const unsigned char *s = src + (size_t(y0 + row) * size_t(img->width) + size_t(x0)) * 4;'
        Repl   = '        const unsigned char *s = src + (size_t(y0 + row) * size_t(img->width)) * 4; // SABOTAGE'
        Target = "ImageRegistryTest.CropMatchesSourceRegion"
    },
    @{
        Name   = "RV-M3-registry-evicts-on-full"
        Desc   = "注册表满时静默清空重存（淘汰既有句柄）"
        Find   = '    if (cap > 0 && static_cast<long>(g_image_registry.size()) >= cap)
        return L""; // 拒绝新增，不淘汰既有句柄（见文件头说明）'
        Repl   = '    if (cap > 0 && static_cast<long>(g_image_registry.size()) >= cap)
        g_image_registry.clear(); // SABOTAGE: silent eviction'
        Target = "ImageRegistryTest.RegistryFullRejectsButKeepsExisting"
    },
    @{
        Name   = "RV-M4-findstr-ignores-handle"
        Desc   = "find_str_mem 不校验句柄（退化成对空图匹配）"
        Find   = '    auto img = acquire_image(handle);
    if (!img)
        return -1;

    // 复用 P1 的匹配内核'
        Repl   = '    auto img = acquire_image(handle);
    if (!img)
        img = std::make_shared<Image>(); // SABOTAGE: pretend we have an image
    if (!img)
        return -1;

    // 复用 P1 的匹配内核'
        Target = "ImageRegistryFindStr.FindsTextInsideRegisteredImage"
    }
)

$results = @()
foreach ($s in $scenarios) {
    $backup = "$svcSrc.rvbak"
    Copy-Item $svcSrc $backup -Force
    $text = [System.IO.File]::ReadAllText($svcSrc)
    $find = $s.Find.Replace("`r`n", "`n")
    $repl = $s.Repl.Replace("`r`n", "`n")
    # 统一按 LF 比较，再还原目标文件的换行风格
    $normalized = $text.Replace("`r`n", "`n")
    $hits = ([regex]::Matches($normalized, [regex]::Escape($find))).Count
    if ($hits -ne 1) {
        Write-Host "[$($s.Name)] PATTERN-MISS hits=$hits -- NOT verified" -ForegroundColor Yellow
        $results += [pscustomobject]@{ Scenario = $s.Name; Patch = "MISS($hits)"; Result = "n/a" }
        Copy-Item $backup $svcSrc -Force; Remove-Item $backup -Force
        continue
    }
    $idx = $normalized.IndexOf($find)
    $patched = $normalized.Substring(0, $idx) + $repl + $normalized.Substring($idx + $find.Length)
    if ($text -match "`r`n") { $patched = $patched.Replace("`n", "`r`n") }
    [System.IO.File]::WriteAllText($svcSrc, $patched)
    (Get-Item $svcSrc).LastWriteTime = Get-Date
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
            $out = Run-RegistryTests
            $red = $out -match '\[  FAILED  \]'
            $named = $out -match [regex]::Escape("FAILED  ] $($s.Target)")
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

    Copy-Item $backup $svcSrc -Force
    (Get-Item $svcSrc).LastWriteTime = Get-Date
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
(Run-RegistryTests) -split "`n" | Select-String -Pattern "^\[  PASSED  \]|^\[  FAILED  \]" | ForEach-Object { $_.Line }
