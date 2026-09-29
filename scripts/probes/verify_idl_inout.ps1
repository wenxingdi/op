# Verify [in, out] VARIANT* IDL fix: late-binding readback via PowerShell
# ASCII only. Self-contained: spawns notepad, kills it at the end.
# Output goes to idl_inout_verify.txt (PS child may hang at exit due to known P1,
# so the script force-kills itself after writing results).
$ErrorActionPreference = "Continue"
$out = "D:\AutoPro\op-master\op\workbench\idl_inout_verify.txt"
Set-Content -Path $out -Value "=== IDL [in,out] verify $(Get-Date -Format HH:mm:ss) ==="
function L($s) { Add-Content -Path $out -Value $s }

$op = New-Object -ComObject op.opsoft
$op.SetShowErrorMsg(2) | Out-Null   # enable __op.log (diag GetClientSize/GetCursorPos vt logging)
L ("Ver=" + $op.Ver() + "  cwd=" + (Get-Location).Path)

# --- T1: GetCursorPos (no window required) ---
$x = [object]0; $y = [object]0
$ret = $op.GetCursorPos([ref]$x, [ref]$y)
L "T1 GetCursorPos ref-object: ret=$ret x=$x y=$y type=$($x.GetType().Name)"

$x2 = 0; $y2 = 0
$ret = $op.GetCursorPos([ref]$x2, [ref]$y2)
L "T1 GetCursorPos ref-int:    ret=$ret x=$x2 y=$y2"

try {
    $x3 = 0; $y3 = 0
    $ret = $op.GetCursorPos($x3, $y3)
    L "T1 GetCursorPos plain:      ret=$ret x=$x3 y=$y3"
} catch { L "T1 GetCursorPos plain: EXC $_" }

# --- T2: GetClientSize / GetWindowRect on a fresh notepad ---
$np = Start-Process notepad -PassThru
Start-Sleep -Milliseconds 1500
$hwnd = $op.FindWindow("Notepad", "")
L "T2 FindWindow(Notepad)=$hwnd pid=$($np.Id)"
if ($hwnd -gt 0) {
    $w = [object]0; $h = [object]0
    $r1 = $op.GetClientSize($hwnd, [ref]$w, [ref]$h)
    L "T2 GetClientSize ref-object: ret=$r1 w=$w h=$h"

    $w2 = 0; $h2 = 0
    $r1b = $op.GetClientSize($hwnd, [ref]$w2, [ref]$h2)
    L "T2 GetClientSize ref-int:    ret=$r1b w=$w2 h=$h2"

    $ax = [object]0; $ay = [object]0; $bx = [object]0; $by = [object]0
    $r2 = $op.GetWindowRect($hwnd, [ref]$ax, [ref]$ay, [ref]$bx, [ref]$by)
    L "T2 GetWindowRect ref-object: ret=$r2 ($ax,$ay)-($bx,$by)"
} else {
    L "T2 SKIP: no notepad window found"
}
Stop-Process -Id $np.Id -Force -ErrorAction SilentlyContinue

L "=== DONE ==="
# append __op.log diag lines (written in this PS process cwd)
$oplog = Join-Path (Get-Location).Path "__op.log"
if (Test-Path $oplog) {
    L "----- __op.log (tail) -----"
    Get-Content $oplog -Tail 40 | ForEach-Object { L $_ }
} else {
    L "no __op.log at $oplog"
}
# P1 known issue: op DLL keeps the PS process alive at exit. Force exit.
Stop-Process -Id $PID -Force
