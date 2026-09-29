# -*- coding: utf-8 -*-
"""修复 libop/CMakeLists.txt 中 x86 .def 块的控制流（一次性修复脚本）。"""
import io

P = r"D:\AutoPro\op-master\op\libop\CMakeLists.txt"
lines = io.open(P, encoding="utf-8").read().split("\n")

# 1) 删除孤儿尾巴：458..469 行（1-based）
assert lines[457].strip() == "endforeach()", lines[457]
assert lines[468].strip() == "endif()", lines[468]
del lines[457:469]
txt = "\n".join(lines)

# 2) 补全后半块（缺失 endforeach + 生成 def + set_property + 外层 endif）
broken_start = '    endif()\n\n\nif(enable_wgc)'
fixed = (
    "    endif()\n"
    "  endforeach()\n"
    '  list(REMOVE_DUPLICATES _op_capi_names)\n'
    '  set(_op_capi_def_content "EXPORTS\\n")\n'
    "  foreach(_n ${_op_capi_names})\n"
    '    string(APPEND _op_capi_def_content "    ${_n}\\n")\n'
    "  endforeach()\n"
    '  file(WRITE "${_op_capi_def}" "${_op_capi_def_content}")\n'
    "  list(LENGTH _op_capi_names _op_capi_count)\n"
    '  message(STATUS "op_c_api x86 def: ${_op_capi_def} (${_op_capi_count} exports, bare names)")\n'
    "  set_property(TARGET ${op_c_api} APPEND_STRING PROPERTY LINK_FLAGS\n"
    '               " /DEF:\\"${_op_capi_def}\\"")\n'
    "endif()\n"
    "\n"
    "if(enable_wgc)"
)
assert broken_start in txt, "pattern not found"
txt = txt.replace(broken_start, fixed, 1)
io.open(P, "w", encoding="utf-8", newline="").write(txt)
print("fixed ok")
