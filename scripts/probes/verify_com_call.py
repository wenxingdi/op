# COM 后期绑定端到端验证（pywin32）
#
# 验证 DISPID 442 FindStrBatch / 443 FindStrEx2 / 444 OcrLastProfile 在真实
# 后期绑定客户端（易语言/VBScript 同类路径：IDispatch::GetIDsOfNames + Invoke）下可用。
#
# 用法：
#   python scripts\probes\verify_com_call.py <测试图片路径>
#
# 注意 pywin32 的返回值约定：[out] BSTR* retstr + [out, retval] LONG* ret
# 会返回成 (retval_long, retstr_bstr) 元组。
import sys
import json

try:
    import win32com.client as wc
except ImportError:
    print("SKIP: pywin32 not installed (pip install pywin32)")
    sys.exit(2)

pic = sys.argv[1]
op = wc.Dispatch("op.opsoft")
print("SetOcrEngine   :", op.SetOcrEngine("onnx", "", ""))
print("SetDisplayInput:", op.SetDisplayInput("pic:" + pic))

print()
print("=== DISPID 442 FindStrBatch ===")
ret, j = op.FindStrBatch(0, 0, 460, 100, '["909"]', "", 0.3)
print("ret(long) =", ret)
print("json =", j)
d = json.loads(j)
print("  targets=%s found=%s hits=%s lines=%s" % (d["targets"], d["found"], d["hits"], d["lines"]))
if d["hits"]:
    h = d["results"][0]["hits"][0]
    print("  hit0: x=%s y=%s cx1=%s cx2=%s char_precise=%s begin=%s end=%s" %
          (h["x"], h["y"], h["cx1"], h["cx2"], h["char_precise"], h["begin"], h["end"]))
    print("  line_text = %s" % h["line_text"])

print()
print("=== DISPID 442 多目标（一个命中一个不命中） ===")
ret, j = op.FindStrBatch(0, 0, 460, 100, '["909","不存在的东西"]', "", 0.3)
print("ret(long) =", ret)
d = json.loads(j)
print("  targets=%s found=%s hits=%s" % (d["targets"], d["found"], d["hits"]))

print()
print("=== DISPID 443 FindStrEx2 flags=0 (contains) ===")
ret2, j2 = op.FindStrEx2(0, 0, 460, 100, "909", "", 0.3, 0)
print("ret =", ret2)
d2 = json.loads(j2)
print("  found=%s hits=%s" % (d2["found"], d2["hits"]))

print()
print("=== DISPID 443 FindStrEx2 flags=2 (exact; 909 只是子串 -> 期望 found=False) ===")
ret3, j3 = op.FindStrEx2(0, 0, 460, 100, "909", "", 0.3, 2)
print("ret =", ret3)
d3 = json.loads(j3)
print("  found=%s  (expected False)" % d3["found"])

print()
print("=== DISPID 444 OcrLastProfile ===")
p = op.OcrLastProfile()
if isinstance(p, tuple):
    p = p[0]
print("json =", p)
pp = json.loads(p)
print("  valid=%s total_ms=%.2f det_ms=%.2f cls_ms=%.2f rec_ms=%.2f" %
      (pp["valid"], pp["total_ms"], pp["det_ms"], pp["cls_ms"], pp["rec_ms"]))
print("  boxes=%s lines=%s chars=%s rec_width=%s upscale=%s" %
      (pp["boxes"], pp["lines"], pp["chars"], pp["rec_width"], pp["upscale"]))

print()
print("=== 回归：旧接口仍可用 ===")
print("  legacy FindStr(strs='909') -> idx =", op.FindStr(0, 0, 460, 100, "909", "", 0.3))
print()
print("=== DONE ===")
