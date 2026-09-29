# -*- coding: utf-8 -*-
append_text = """

## 修正：GetClientSize 问题定性升级（09-19 追加实验）
- 之前"PS 封送假阳性"结论不完整。追加实验：
  - PS 5.1 / PS 7 四种调用风格（object+ref / int+ref / plain / long+ref）全部读不回 VARIANT* 出参，但 ret=1（nret LONG* retval 正常）
  - comtypes 原始 vtable 直调：值完全正确（1348x925 = 逻辑 898x616 x 150% DPI）
  - 裸 IDispatch::Invoke（ctypes，dispid 经 GetIDsOfNames 确认 67）：VT_BYREF|VT_VARIANT 形态 S_OK 但参数表现错乱（w 未写、h 被写 VT_I4、nret=0）；BYREF|VT_I4 与 plain 形态返回 TYPEMISMATCH/BADVARTYPE
- 定性：COM vtable 路径 + C-API 路径正确；IDispatch late-binding 路径对 [out] VARIANT* 出参存在读回问题（IDL 标准写法，DM 同款）
- 处置：real_machine_test.ps1 恢复 int+ref 写法（值不可信仅观测，有 fallback）；验证脚本 repro_com_raw.py / repro_size_bug.py 留档
- 待选项：C# tlbimp 强类型终验 / VBScript 终验（沙箱拦 cscript）/ 文档化已知限制
"""
with open(r"D:\AgentWork\WorkBuddy\2026-08-04-11-41-10\.workbuddy\memory\2026-09-18.md", "a", encoding="utf-8") as f:
    f.write(append_text)
print("appended")
