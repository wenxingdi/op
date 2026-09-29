# OP real-machine test: bind matrix + capture + OCR (multi-target)
# Usage:
#   powershell -ExecutionPolicy Bypass -File real_machine_test.ps1 -Hwnd 19924714,1313938
#   powershell -ExecutionPolicy Bypass -File real_machine_test.ps1 -Hwnd 0x13006EA -Title "Notepad"
# Output: D:\AutoPro\op-master\op\workbench\real_machine\test_log.txt + cap_*.bmp
param(
    [string]$Hwnd = "",
    [string]$Title = "",
    [string]$Proc = "",
    [string]$DllPath = "D:\AutoPro\op-master\op\bin\x64\op_x64.dll"
)

$ErrorActionPreference = "Continue"
$outDir = "D:\AutoPro\op-master\op\workbench\real_machine"
New-Item -ItemType Directory -Force -Path $outDir | Out-Null
$log = Join-Path $outDir "test_log.txt"
"=== OP real machine test $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss') ===" | Out-File -FilePath $log -Encoding utf8

function Log($msg) {
    $line = "[$(Get-Date -Format 'HH:mm:ss.fff')] $msg"
    Add-Content -Path $log -Value $line -Encoding utf8
    Write-Host $line
}

function ConvertTo-HwndNum([string]$s) {
    if ($s.StartsWith("0x", "OrdinalIgnoreCase")) { return [Convert]::ToInt64($s, 16) }
    return [Convert]::ToInt64($s)
}

# ---- resolve targets ----
$targets = @()
if ($Proc) {
    foreach ($pname in ($Proc -split "," | ForEach-Object { $_.Trim() } | Where-Object { $_ })) {
        $procs = [System.Diagnostics.Process]::GetProcessesByName($pname)
        if (-not $procs -or $procs.Count -eq 0) { Log "ERROR: process '$pname' not found"; continue }
        foreach ($p in $procs) {
            if ($p.MainWindowHandle -ne [IntPtr]::Zero) {
                $targets += @{ hwnd = $p.MainWindowHandle.ToInt64(); name = "proc_${pname}" }
                Log ("proc match: '{0}' pid={1} hwnd=0x{2:X}" -f $pname, $p.Id, $p.MainWindowHandle.ToInt64())
            }
        }
    }
}
if ($Hwnd) {
    foreach ($tok in ($Hwnd -split "," | ForEach-Object { $_.Trim() } | Where-Object { $_ })) {
        try {
            $n = ConvertTo-HwndNum $tok
            $targets += @{ hwnd = $n; name = "hwnd_0x{0:X}" -f $n }
        } catch { Log "ERROR: bad hwnd token '$tok': $($_.Exception.Message)" }
    }
}
if ($Title) {
    Add-Type @"
using System;
using System.Text;
using System.Collections.Generic;
using System.Runtime.InteropServices;
public static class U32 {
    public delegate bool EnumProc(IntPtr hwnd, IntPtr lparam);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr lparam);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr hwnd);
    [DllImport("user32.dll")] public static extern int GetWindowTextLength(IntPtr hwnd);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr hwnd, StringBuilder sb, int max);
    public static List<object[]> VisibleWindows() {
        var list = new List<object[]>();
        EnumWindows((h, lp) => {
            if (!IsWindowVisible(h)) return true;
            int len = GetWindowTextLength(h);
            if (len <= 0) return true;
            var sb = new StringBuilder(len + 1);
            GetWindowText(h, sb, sb.Capacity);
            list.Add(new object[] { h.ToInt64(), sb.ToString() });
            return true;
        }, IntPtr.Zero);
        return list;
    }
}
"@
    $wins = [U32]::VisibleWindows()
    $hit = $wins | Where-Object { ([string]$_[1]).IndexOf($Title, [StringComparison]::OrdinalIgnoreCase) -ge 0 }
    if ($hit) {
        foreach ($m in $hit) {
            $targets += @{ hwnd = [Int64]$m[0]; name = "title_$($m[1])" }
            Log ("match: hwnd=0x{0:X} title='{1}'" -f [Int64]$m[0], $m[1])
        }
    } else {
        Log "ERROR: no window title contains '$Title'. Visible top-level windows:"
        foreach ($w in $wins) { Log ("  hwnd=0x{0:X}  '{1}'" -f [Int64]$w[0], $w[1]) }
    }
}
if ($targets.Count -eq 0) { Log "FATAL: no valid target"; exit 1 }

# ---- create COM ----
try { $op = New-Object -ComObject op.opsoft } catch { Log "FATAL: New-Object failed: $($_.Exception.Message)"; exit 1 }
try { Log "Ver: $($op.Ver())" } catch { Log "Ver FAILED: $($_.Exception.Message)" }
try { Log "SetPath ret: $($op.SetPath('D:\AutoPro\op-master\op\bin\x64'))" } catch { Log "SetPath FAILED: $($_.Exception.Message)" }

# ---- per-target test ----
foreach ($t in $targets) {
    $hwndNum = $t.hwnd
    $tag = $t.name -replace "[^\w]", "_"
    Log "########## TARGET $($t.name) hwnd=$hwndNum ##########"

    # unbound window info
    # 已知限制：GetClientSize/GetWindowRect 的 [out] VARIANT* 出参经 IDispatch late-binding
    # 在 PS 5.1/7 下都无法读回（vtable/C-API 路径已验证值正确，见 repro_com_raw.py）。
    # 这里仅做行为观测，实际尺寸用 fallback；故用最不易抛错的写法。
    $w = 0; $h = 0
    try {
        $rc = $op.GetClientSize($hwndNum, [ref]$w, [ref]$h)
        Log "GetClientSize ret=$rc client=${w}x${h}"
    } catch { Log "GetClientSize FAILED: $($_.Exception.Message)" }
    try {
        $x1 = 0; $y1 = 0; $x2 = 0; $y2 = 0
        $rc = $op.GetWindowRect($hwndNum, [ref]$x1, [ref]$y1, [ref]$x2, [ref]$y2)
        Log "GetWindowRect ret=$rc rect=($x1,$y1)-($x2,$y2)"
    } catch { Log "GetWindowRect FAILED: $($_.Exception.Message)" }
    if ($w -le 0 -or $h -le 0) { $w = 800; $h = 600; Log "fallback client size ${w}x${h}" }

    # bind matrix
    foreach ($disp in @("normal", "gdi", "dx2", "opengl")) {
        Log "----- display=[$disp] -----"
        try {
            $r = $op.BindWindow($hwndNum, $disp, "normal", "normal", 0)
            Log "BindWindow($disp) ret=$r"
            if ($r -ne 1) { Log "bind failed, skip this mode"; continue }
            Log "IsBind=$($op.IsBind())  GetBindWindow=$($op.GetBindWindow())"
            $img = Join-Path $outDir "cap_${tag}_$disp.bmp"
            $r2 = $op.Capture(0, 0, $w, $h, $img)
            if (Test-Path $img) { Log "Capture($disp) ret=$r2 file=$((Get-Item $img).Length) bytes" }
            else { Log "Capture($disp) ret=$r2 but FILE NOT CREATED" }
            $txt = $op.GetWordsNoDict(0, 0, $w, $h, "000000-000000")
            Log "GetWordsNoDict($disp) = '$txt'"
            $txt2 = $op.OcrAuto(0, 0, $w, $h, 0.8)
            Log "OcrAuto($disp) = '$txt2'"
            $r3 = $op.UnBindWindow()
            Log "UnBindWindow ret=$r3"
        } catch {
            Log "MODE [$disp] EXCEPTION: $($_.Exception.Message)"
            try { $op.UnBindWindow() | Out-Null } catch {}
        }
    }
}

# ---- summary ----
Log "=== DONE ==="
Get-ChildItem $outDir -Filter "cap_*.bmp" | ForEach-Object { Log "artifact: $($_.Name) ($($_.Length) bytes)" }
