$ErrorActionPreference = "Continue"
$out = @()
$op = New-Object -ComObject op.opsoft
$out += "Ver: " + $op.Ver()
$hwnd = 115543794

# S1: [object]0 + [ref]
$w = [object]0; $h = [object]0
try { $r = $op.GetClientSize($hwnd, [ref]$w, [ref]$h); $out += "S1(obj+ref) ret=$r w=$w h=$h" }
catch { $out += "S1(obj+ref) EX: " + $_.Exception.Message }

# S2: int 0 + [ref]（昨天的原始写法）
$w = 0; $h = 0
try { $r = $op.GetClientSize($hwnd, [ref]$w, [ref]$h); $out += "S2(int+ref) ret=$r w=$w h=$h" }
catch { $out += "S2(int+ref) EX: " + $_.Exception.Message }

# S3: 不用 ref，直接传 0 —— 看 PS 是否把 out 参数并入返回值
try { $r = $op.GetClientSize($hwnd, 0, 0); $out += "S3(plain) ret=$r" }
catch { $out += "S3(plain) EX: " + $_.Exception.Message }

# S4: [long] 强类型 + [ref]
[long]$lw = 0; [long]$lh = 0
try { $r = $op.GetClientSize($hwnd, [ref]$lw, [ref]$lh); $out += "S4(long+ref) ret=$r w=$lw h=$lh" }
catch { $out += "S4(long+ref) EX: " + $_.Exception.Message }

$out | Out-File -FilePath "D:\AutoPro\op-master\op\workbench\style_test.txt" -Encoding utf8
