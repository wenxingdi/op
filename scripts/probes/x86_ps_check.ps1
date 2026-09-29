# 32 位 PowerShell 交叉验证 op_c_api_x86.dll
$src = @'
using System;
using System.Runtime.InteropServices;
public static class OpX86 {
    [DllImport("op_c_api_x86.dll", CharSet = CharSet.Unicode)]
    public static extern IntPtr OpCreate();
    [DllImport("op_c_api_x86.dll", CharSet = CharSet.Unicode)]
    public static extern void OpDestroy(IntPtr h);
    [DllImport("op_c_api_x86.dll", CharSet = CharSet.Unicode)]
    public static extern IntPtr OpVer();
    [DllImport("op_c_api_x86.dll", CharSet = CharSet.Unicode)]
    public static extern int OpGetID(IntPtr h);
    [DllImport("op_c_api_x86.dll", CharSet = CharSet.Unicode)]
    public static extern int OpGetScreenWidth(IntPtr h, out int w);
    [DllImport("op_c_api_x86.dll", CharSet = CharSet.Unicode)]
    public static extern int OpGetScreenHeight(IntPtr h, out int hh);
    [DllImport("op_c_api_x86.dll", CharSet = CharSet.Unicode)]
    public static extern IntPtr OpGetPath(IntPtr h);
    [DllImport("op_c_api_x86.dll", CharSet = CharSet.Unicode)]
    public static extern int OpFindWindow(IntPtr h, string cls, string title);
    [DllImport("op_c_api_x86.dll", CharSet = CharSet.Unicode)]
    public static extern int OpEnumWindow(IntPtr h, IntPtr parent, string filter, int flag, out string outList);
    [DllImport("op_c_api_x86.dll", CharSet = CharSet.Unicode)]
    public static extern int OpBeep(IntPtr h);
}
'@
Add-Type -TypeDefinition $src

Write-Host ("BITNESS: " + [IntPtr]::Size * 8)
Write-Host ("Is64BitProcess: " + [Environment]::Is64BitProcess)

$ver = [Runtime.InteropServices.Marshal]::PtrToStringUni([OpX86]::OpVer())
Write-Host "OpVer -> $ver"

$h = [OpX86]::OpCreate()
Write-Host ("OpCreate -> " + $h)

$id = [OpX86]::OpGetID($h)
Write-Host "OpGetID -> $id"

$w = 0; $hh = 0
[OpX86]::OpGetScreenWidth($h, [ref]$w) | Out-Null
[OpX86]::OpGetScreenHeight($h, [ref]$hh) | Out-Null
Write-Host "Screen -> ${w}x${hh}"

$path = [Runtime.InteropServices.Marshal]::PtrToStringUni([OpX86]::OpGetPath($h))
Write-Host "OpGetPath -> $path"

$beep = [OpX86]::OpBeep($h)
Write-Host "OpBeep -> $beep"

$lst = ""
$n = [OpX86]::OpEnumWindow($h, [IntPtr]::Zero, "notepad", 0, [ref]$lst)
Write-Host "OpEnumWindow(notepad) -> n=$n lst=$lst"

[OpX86]::OpDestroy($h)
Write-Host "OpDestroy ok"
Write-Host "ALL-DONE"
