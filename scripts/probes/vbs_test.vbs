' VBScript 是大漠系 COM 的经典消费端，验证 IDispatch 派发路径出参是否正常
Dim op, hwnd, w, h, r
Set op = CreateObject("op.opsoft")
hwnd = 115543794
w = 0
h = 0
r = op.GetClientSize(hwnd, w, h)
Dim fso, f
Set fso = CreateObject("Scripting.FileSystemObject")
Set f = fso.CreateTextFile("D:\AutoPro\op-master\op\workbench\vbs_test.txt", True)
f.WriteLine "ret=" & r & " w=" & w & " h=" & h
f.Close
