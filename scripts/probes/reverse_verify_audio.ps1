# 音量模块反向验证
# 用法：pwsh -File scripts\probes\reverse_verify_audio.ps1
#
# 四场景：
#   RV-A1：found 判据恒真（不比对 PID）        -> NonexistentPidReportsNotFound 必须 RED
#   RV-A2：非法 PID 不校验                      -> InvalidPidIsRejected 必须 RED
#   RV-A3：volume 与 peak 写颠倒                -> RealSessionReadWriteRoundTrip 必须 RED
#   RV-A4：ComGuard 无条件 CoUninitialize       -> RepeatedCallsAreStable 必须 RED（部分宿主）
#
# 前提：改 .cpp 也可能不触发重编，故强制删 obj 并校验时间戳。

$ErrorActionPreference = "Stop"
$repo = "D:\AutoPro\op-master\op"
$ws = "$repo\workbench\tmp"
$python = "D:\Program Files\Python312\python.exe"
$svc = "$repo\libop\audio\AudioService.cpp"
$objs = @(
    "$repo\build\nmake-x64-Release\libop\CMakeFiles\op_x64.dir\audio\AudioService.cpp.obj",
    "$repo\build\nmake-x64-Release\libop\CMakeFiles\libop.dir\audio\AudioService.cpp.obj",
    "$repo\build\nmake-x64-Release\tests\CMakeFiles\op_test.dir\audio_test.cpp.obj"
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

function Run-AudioTests {
    $env:PATH = "$repo\build\nmake-x64-Release\libop;" + $env:PATH
    Push-Location $repo
    $out = & "$repo\build\nmake-x64-Release\tests\op_test.exe" --gtest_filter="AudioTest.*" 2>&1
    Pop-Location
    return ($out -join "`n")
}

$scenarios = @(
    @{
        Name = "RV-A1-found-always-true"
        Desc = "found 判据恒真（不比对 PID）"
        Find = '        if (session_pid != pid)
            continue;

        // 命中该 PID 的会话
        found = true;'
        Repl = '        // SABOTAGE: ignore pid match
        found = true;'
        Target = "NonexistentPidReportsNotFoundGracefully"
    },
    @{
        Name = "RV-A2-no-pid-validation"
        Desc = "非法 PID 不校验（pid==0 也放行）"
        # 注意：`pid == 0` 在 QueryByPid / SetSessionVolume / SetSessionMute 各有一处，
        # 必须带上前面的 `info.pid = pid;` 锚点才能唯一定位到 QueryByPid。
        Find = '    info.pid = pid;
    if (pid == 0) {
        info.code = -4;
        info.error = kErrBadPid;
        return info;
    }'
        Repl = '    info.pid = pid;
    // SABOTAGE: no pid validation'
        Target = "InvalidPidIsRejected"
    },
    @{
        Name = "RV-A3-volume-peak-swapped"
        Desc = "volume 与 peak 写颠倒"
        Find = '            if (SUCCEEDED(meter->GetPeakValue(&p)))
                peak = static_cast<double>(p);'
        Repl = '            if (SUCCEEDED(meter->GetPeakValue(&p)))
                volume = static_cast<double>(p); // SABOTAGE: writes into volume'
        Target = "RealSessionReadWriteRoundTrip"
    },
    @{
        Name = "RV-A4-com-uninit-ignores-changed-mode"
        Desc = "ComGuard 把 RPC_E_CHANGED_MODE 也当作自己初始化（owned 恒真）"
        Find = '        owned_ = SUCCEEDED(hr);'
        Repl = '        owned_ = SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE; // SABOTAGE'
        Target = "RepeatedCallsAreStable"
    }
)

$results = @()
foreach ($s in $scenarios) {
    $backup = "$svc.rvbak"
    Copy-Item $svc $backup -Force
    $text = [System.IO.File]::ReadAllText($svc)
    $find = $s.Find.Replace("`r`n", "`n")
    $repl = $s.Repl.Replace("`r`n", "`n")
    $normalized = $text.Replace("`r`n", "`n")
    $hits = ([regex]::Matches($normalized, [regex]::Escape($find))).Count
    if ($hits -ne 1) {
        Write-Host "[$($s.Name)] PATTERN-MISS hits=$hits -- NOT verified" -ForegroundColor Yellow
        $results += [pscustomobject]@{ Scenario = $s.Name; Patch = "MISS($hits)"; Result = "n/a" }
        Copy-Item $backup $svc -Force; Remove-Item $backup -Force
        continue
    }
    $idx = $normalized.IndexOf($find)
    $patched = $normalized.Substring(0, $idx) + $repl + $normalized.Substring($idx + $find.Length)
    if ($text -match "`r`n") { $patched = $patched.Replace("`n", "`r`n") }
    [System.IO.File]::WriteAllText($svc, $patched)
    (Get-Item $svc).LastWriteTime = Get-Date
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
            $out = Run-AudioTests
            $red = $out -match '\[  FAILED  \]'
            $named = $out -match [regex]::Escape("FAILED  ] AudioTest.$($s.Target)")
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

    Copy-Item $backup $svc -Force
    (Get-Item $svc).LastWriteTime = Get-Date
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
(Run-AudioTests) -split "`n" | Select-String -Pattern "^\[  PASSED  \]|^\[  FAILED  \]" | ForEach-Object { $_.Line }
