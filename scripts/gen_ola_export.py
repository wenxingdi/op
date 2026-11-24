"""按 OLA 导出 JSON 格式生成 OP 的 com / def 两份接口清单。

用法（仓库根执行）：
    python scripts/gen_ola_export.py [--out-dir docs]

数据源：
  com 版: include/libop.h           （426 个 C++ 接口，复用 gen_api_reference 的解析与参数注解）
  def 版: include/op_c_api.h        （433 个 C ABI 声明）
  GUID  : libop/com/op.idl
输出：
  op_export_com.json / op_export_def.json
"""
from __future__ import annotations

import argparse
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

import gen_api_reference as ref  # noqa: E402

CONTRACT_NOTE = "OP 出口契约：int/status 返回 0=失败、非 0=成功；JSON 结果体内 code 字段 0=成功（两套口径并存）。"
STRING_NOTE = "字符串返回值使用 C API 共享缓冲，跨调用必须先拷贝。"

# 结构体模型：沿用 OLA 的 7 个公共模型（修正原文 typo），另补 OP 的逐字坐标结构。
MODELS = [
    {"Name": "Point", "Description": "坐标点", "Attribute": [
        {"Name": "X", "Description": "X 坐标", "Type": "int"},
        {"Name": "Y", "Description": "Y 坐标", "Type": "int"}],
     "Example": '{"X": 100, "Y": 200}'},
    {"Name": "Size", "Description": "尺寸", "Attribute": [
        {"Name": "Width", "Description": "宽度", "Type": "int"},
        {"Name": "Height", "Description": "高度", "Type": "int"}],
     "Example": '{"Width": 300, "Height": 400}'},
    {"Name": "ColorModel", "Description": "颜色（对外 RGB hex；内部 color_t 为 BGRA）", "Attribute": [
        {"Name": "StartColor", "Description": "颜色起始范围 RRGGBB 或 #RRGGBB", "Type": "string"},
        {"Name": "EndColor", "Description": "颜色结束范围 RRGGBB 或 #RRGGBB", "Type": "string"},
        {"Name": "Type", "Description": "0 普通取合集 1 反色取合集 2 普通取交集 3 反色取交集", "Type": "int"}],
     "Example": '{"StartColor":"#0A7497","EndColor":"#0A7497","Type":0}'},
    {"Name": "PointColorModel", "Description": "点阵颜色", "Attribute": [
        {"Name": "Point", "Description": "坐标点", "Type": "Point"},
        {"Name": "Colors", "Description": "颜色列表，按 Type 合并", "Type": "List<ColorModel>"}],
     "Example": '{"Point":{"X":0,"Y":2},"Colors":[{"StartColor":"#0A7497","EndColor":"#0A7497","Type":0}]}'},
    {"Name": "OcrModel", "Description": "识别结果（单条）", "Attribute": [
        {"Name": "Score", "Description": "识别评分", "Type": "double"},
        {"Name": "Text", "Description": "识别文字", "Type": "string"},
        {"Name": "Center", "Description": "中心点", "Type": "Point"},
        {"Name": "Vertices", "Description": "矩形 4 个顶点", "Type": "List<Point>"},
        {"Name": "Angle", "Description": "矩形角度", "Type": "double"}],
     "Example": '{"Score":0.95,"Text":"abc","Center":{"X":100,"Y":200},"Vertices":[],"Angle":0}'},
    {"Name": "OcrResult", "Description": "OCR 识别结果", "Attribute": [
        {"Name": "Regions", "Description": "识别结果列表", "Type": "List<OcrModel>"},
        {"Name": "Text", "Description": "识别文字", "Type": "string"}],
     "Example": '{"Regions":[{"Score":0.95,"Text":"abc","Center":{"X":100,"Y":200},"Vertices":[],"Angle":0}],"Text":"abc"}'},
    {"Name": "OcrWord", "Description": "逐字坐标结果（OP OcrEx 的 words 分项）", "Attribute": [
        {"Name": "Word", "Description": "单字/词条", "Type": "string"},
        {"Name": "X", "Description": "左上角 X", "Type": "int"},
        {"Name": "Y", "Description": "左上角 Y", "Type": "int"},
        {"Name": "Width", "Description": "宽度", "Type": "int"},
        {"Name": "Height", "Description": "高度", "Type": "int"},
        {"Name": "Score", "Description": "置信度", "Type": "double"}],
     "Example": '{"Word":"A","X":10,"Y":20,"Width":8,"Height":12,"Score":0.98}'},
    {"Name": "MatchResult", "Description": "图像匹配结果", "Attribute": [
        {"Name": "MatchState", "Description": "是否达到指定精度", "Type": "bool"},
        {"Name": "MatchVal", "Description": "相似度", "Type": "double"},
        {"Name": "Angle", "Description": "识别角度", "Type": "double"},
        {"Name": "Index", "Description": "多图索引，从 0 开始", "Type": "int"},
        {"Name": "X", "Description": "结果 X 坐标", "Type": "int"},
        {"Name": "Y", "Description": "结果 Y 坐标", "Type": "int"},
        {"Name": "Width", "Description": "结果宽度", "Type": "int"},
        {"Name": "Height", "Description": "结果高度", "Type": "int"}],
     "Example": '{"MatchVal":0.85,"MatchState":true,"Index":0,"Angle":45.0,"X":100,"Y":200,"Width":100,"Height":100}'},
]

# C++ 类型 → OLA 格式类型（COM 面）
CPP_TYPE_COM = {
    "std::wstring": "bstr", "std::string": "bstr", "const wchar_t *": "bstr",
    "std::wstring &": "OLA_STRING_RETURN*", "std::string &": "OLA_STRING_RETURN*",
    "long": "int32_t", "long *": "int32_t*", "int": "int32_t", "int *": "int32_t*",
    "int64_t": "int64_t", "int64_t *": "int64_t*", "long long": "int64_t",
    "double": "double", "double *": "double*", "float": "float", "bool": "bool",
    "unsigned long": "int64_t", "size_t": "int64_t", "unsigned char": "int32_t",
    "LONG_PTR": "int64_t", "unsigned long long": "int64_t", "void": "void",
    "void *": "int64_t", "intptr_t": "int64_t", "uintptr_t": "int64_t",
}

# C 声明类型 → OLA 格式类型（DLL 面）
C_TYPE_DEF = {
    "op_handle": "int64_t", "void": "void",
    "int": "int32_t", "int *": "int32_t*", "long": "int32_t", "long *": "int32_t*",
    "long long": "int64_t", "int64_t": "int64_t", "int64_t *": "int64_t*",
    "unsigned int": "int32_t", "unsigned char": "int32_t", "unsigned char *": "int64_t",
    "size_t": "int64_t", "double": "double", "double *": "double*",
    "float": "float", "bool": "bool", "intptr_t": "int64_t", "uintptr_t": "int64_t",
    "uint32_t *": "int32_t*", "uint32_t": "int32_t",
    "const wchar_t *": "string", "wchar_t *": "string", "const char *": "string",
    "const void *": "int64_t", "void *": "int64_t", "const unsigned char *": "int64_t",
}

OUT_NAMES = {"ret", "bret", "rettitle", "retx", "rety"}
JSON_HINT = re.compile(r"json|result|info|list|config|profile", re.I)


def cpp_type(t: str) -> str:
    t = re.sub(r"\s+", " ", t.strip())
    t = re.sub(r"\s*([*&])", r" \1", t)  # const wchar_t* → const wchar_t *
    return CPP_TYPE_COM.get(t, t)


def c_type(t: str) -> str:
    t = re.sub(r"\s+", " ", t.strip())
    t = re.sub(r"\s*\*", " *", t)  # const wchar_t* → const wchar_t *
    return C_TYPE_DEF.get(t, t)


def return_format(name: str, typ: str) -> str | None:
    if typ in ("bstr", "string", "OLA_STRING_RETURN*"):
        return "JSON" if JSON_HINT.search(name) else "string"
    return None


def com_functions() -> list[dict]:
    fns = ref.parse_header(os.path.join(ROOT, "include", "libop.h"))
    out = []
    for fn in fns:
        name = fn["name"]
        desc = ref.html_to_text(fn["desc"]) or f"OP 接口 {name}"
        params = list(fn["params"])
        rettype = fn["ret"]
        notes = []

        # out 状态位/字符串出参 → 归一化为 ReturnType（与 OLA 口径一致）
        payload_out = [p for p in params if p["dir"] == "out"
                       and (p["name"] in OUT_NAMES or p["name"].startswith("ret"))]
        if payload_out:
            p = payload_out[0]
            rettype = "long" if "*" in p["type"] else "std::wstring"
            params = [q for q in params if q is not p]

        rt = cpp_type(rettype)
        ret_item = {"Type": rt, "Description": "操作结果" if rt == "int32_t" else "返回值"}
        if payload_out:
            pd = ref.param_doc_text(fn, payload_out[0])
            if pd:
                ret_item["Description"] = pd
        fmt = return_format(name, rt)
        if fmt:
            ret_item["ReturnFormat"] = fmt

        plist = []
        for p in params:
            if p["dir"] == "in" and not p["name"]:
                continue
            typ = cpp_type(p["type"])
            doc = ref.param_doc_text(fn, p) or p["type"]
            plist.append({"Name": p["name"], "Type": typ, "Description": doc, "Mode": p["dir"]})

        item = {"FunctionName": name, "Description": desc, "ReturnType": ret_item, "Parameters": plist}
        if rt == "int32_t":
            item["ReturnEnums"] = {"0": "失败", "非0": "成功"}
        notes.append(CONTRACT_NOTE)
        if fmt:
            notes.append(STRING_NOTE)
        item["Notes"] = notes
        out.append(item)
    return out


def parse_c_api() -> list[tuple[str, str, str]]:
    src = open(os.path.join(ROOT, "include", "op_c_api.h"), encoding="utf-8").read()
    src = re.sub(r"/\*.*?\*/", "", src, flags=re.S)
    src = re.sub(r"//[^\n]*", "", src)
    pat = re.compile(
        r"^\s*OP_C_API\s+((?:const\s+)?(?:op_handle|void|[A-Za-z_]\w*(?:\s*\*)?))\s*"
        r"OP_CALL\s+(\w+)\s*\(([^;]*?)\)\s*;",
        re.M | re.S,
    )
    decls = pat.findall(src)
    return [(f[0].strip(), f[1], f[2].strip()) for f in decls]


def split_params(pstr: str) -> list[tuple[str, str]]:
    pstr = pstr.strip()
    if not pstr or pstr == "void":
        return []
    parts, depth, cur = [], 0, ""
    for ch in pstr:
        if ch in "(<[":
            depth += 1
        elif ch in ")>]":
            depth -= 1
        if ch == "," and depth == 0:
            parts.append(cur)
            cur = ""
        else:
            cur += ch
    if cur.strip():
        parts.append(cur)
    out = []
    for p in parts:
        p = re.sub(r"\s+", " ", p.strip())
        m = re.match(r"^(.*?[\w\)])\s*([&*]+)?\s*(\w+)$", p)
        if m:
            typ, stars, name = m.groups()
            out.append(((typ + (stars or "")).replace(" *", "*").replace(" &", "&").strip(), name))
        else:
            out.append((p, ""))
    return out


def def_functions(com: list[dict]) -> list[dict]:
    by_name = {f["FunctionName"]: f for f in com}
    out = []
    for ret, name, pstr in parse_c_api():
        base = by_name.get(name[2:] if name.startswith("Op") else name, {})
        ret_def = c_type(ret)
        if ret == "const wchar_t *":
            ret_def = "int64_t"  # DLL 面字符串返回值是指针地址（与 OLA def 口径一致）
        ret_item = {"Type": ret_def, "Description": base.get("ReturnType", {}).get("Description", "操作结果")}
        fmt = return_format(name, ret_def)
        if ret == "const wchar_t *":
            fmt = "string"
        if base.get("ReturnType", {}).get("ReturnFormat") == "JSON":
            fmt = "JSON"
        if fmt:
            ret_item["ReturnFormat"] = fmt

        plist = []
        if "op_handle" in pstr:
            plist.append({"Name": "instance", "Type": "int64_t",
                          "Description": "OP 插件实例句柄，由 OpCreate 创建", "Mode": "in"})
        base_params = {p["Name"]: p for p in base.get("Parameters", [])}
        for typ, pname in split_params(pstr):
            if pname == "handle" and typ == "op_handle":
                continue
            td = c_type(typ)
            doc = base_params.get(pname, {}).get("Description", td)
            mode = "out" if "*" in typ and pname in OUT_NAMES | {"count", "size", "num", "index", "length", "width", "height"} else "in"
            plist.append({"Name": pname, "Type": td, "Description": doc, "Mode": mode})

        item = {"FunctionName": name,
                "Description": base.get("Description", f"OP C ABI 接口 {name}"),
                "ReturnType": ret_item, "Parameters": plist}
        if base.get("ReturnEnums"):
            item["ReturnEnums"] = base["ReturnEnums"]
        notes = list(base.get("Notes", []))
        if not notes:
            notes.append(CONTRACT_NOTE)
        item["Notes"] = notes
        out.append(item)
    return out


def read_guids() -> dict:
    src = open(os.path.join(ROOT, "libop", "com", "op.idl"), encoding="utf-8", errors="replace").read()
    def grab(marker: str) -> str:
        pos = src.find(marker)
        if pos < 0:
            return ""
        m = list(re.finditer(r"uuid\(([0-9a-fA-F-]{36})\)", src[max(0, pos - 2000):pos]))
        return m[-1].group(1) if m else ""
    return {
        "CLSID_OpAutomation": grab("coclass OpAutomation"),
        "IID_IOpAutomation": grab("interface IOpAutomation"),
        "LIBID_opLib": grab("library opLib"),
    }


def build(version: str, com: list[dict], abi: list[dict]) -> tuple[dict, dict]:
    g = read_guids()
    head_com = {
        "Version": version,
        "UserCode": "",
        "CLSID_OpAutomation": g["CLSID_OpAutomation"],
        "IID_IOpAutomation": g["IID_IOpAutomation"],
        "LIBID_opLib": g["LIBID_opLib"],
        "Models": MODELS,
    }
    head_def = {
        "Version": version,
        "UserCode": "",
        "CLSID_OpAutomation": g["CLSID_OpAutomation"],
        "IID_IOpAutomation": g["IID_IOpAutomation"],
        "LIBID_opLib": g["LIBID_opLib"],
        "CreateFunction": "OpCreate",
        "DestroyFunction": "OpDestroy",
        "Models": MODELS,
    }
    # 顺序与 OLA 一致：Version, UserCode, Functions, GUID, Models
    doc_com = {"Version": version, "UserCode": "", "Functions": com,
               "CLSID_OpAutomation": g["CLSID_OpAutomation"],
               "IID_IOpAutomation": g["IID_IOpAutomation"],
               "LIBID_opLib": g["LIBID_opLib"], "Models": MODELS}
    doc_def = {"Version": version, "UserCode": "", "Functions": abi,
               "CLSID_OpAutomation": g["CLSID_OpAutomation"],
               "IID_IOpAutomation": g["IID_IOpAutomation"],
               "LIBID_opLib": g["LIBID_opLib"],
               "CreateFunction": "OpCreate", "DestroyFunction": "OpDestroy", "Models": MODELS}
    _ = head_com, head_def
    return doc_com, doc_def


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--out-dir", default="docs")
    ap.add_argument("--version", default="")
    a = ap.parse_args()
    version = a.version
    if not version:
        cm = open(os.path.join(ROOT, "libop", "CMakeLists.txt"), encoding="utf-8", errors="replace").read()
        m = re.search(r"OP_VERSION\s+\"([^\"]+)\"", cm)
        version = m.group(1) if m else "5.2.0"

    com = com_functions()
    abi = def_functions(com)
    dcom, ddef = build(version, com, abi)
    out_dir = a.out_dir if os.path.isabs(a.out_dir) else os.path.join(ROOT, a.out_dir)
    os.makedirs(out_dir, exist_ok=True)
    for name, doc in (("op_export_com.json", dcom), ("op_export_def.json", ddef)):
        path = os.path.join(out_dir, name)
        with open(path, "w", encoding="utf-8") as f:
            json.dump(doc, f, ensure_ascii=False, indent=2)
            f.write("\n")
        print(f"{path}  functions={len(doc['Functions'])}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
