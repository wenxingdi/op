// OpJson.cpp: JSON 族实现（P3 第一批 2026-10-16，对齐 OLA JSON 模块用途，
// 采用无句柄路径直取式设计）。解析/序列化/路径在 json/JsonValue.cpp，
// 本文件只做参数防御 + 结果回填。
#include <libop.h>

#include "../json/JsonValue.h"

#include <string>
#include <utility>
#include <vector>

namespace {

// 取值类共用：解析 + 路径解析 + 导航，失败返回 nullptr。
const op::json::Value *Locate(const wchar_t *json_text, const wchar_t *path,
                              std::vector<op::json::PathSeg> &segs, op::json::Value &root) {
    if (!json_text || !path)
        return nullptr;
    if (!op::json::Parse(json_text, root))
        return nullptr;
    if (!op::json::ParsePath(path, segs))
        return nullptr;
    return op::json::Navigate(root, segs);
}

// 写入类共用：解析 + 路径解析 + 可写导航（自动建中间节点）。
// 输入文本为 null/空串时视为空文档（从零构建）；非法非空文本返回 nullptr。
op::json::Value *LocateForWrite(const wchar_t *json_text, const wchar_t *path,
                                std::vector<op::json::PathSeg> &segs, op::json::Value &root) {
    if (!path)
        return nullptr;
    if (json_text && json_text[0] != L'\0' && !op::json::Parse(json_text, root))
        return nullptr;
    if (!op::json::ParsePath(path, segs))
        return nullptr;
    return op::json::NavigateOrCreate(root, segs);
}

} // namespace

long op::Op::JsonIsValid(const wchar_t *json_text) {
    if (!json_text)
        return 0;
    op::json::Value root;
    return op::json::Parse(json_text, root) ? 1 : 0;
}

void op::Op::JsonGetString(const wchar_t *json_text, const wchar_t *path, std::wstring &retstr, long *ret) {
    retstr.clear();
    *ret = 0;
    op::json::Value root;
    std::vector<op::json::PathSeg> segs;
    const op::json::Value *v = Locate(json_text, path, segs, root);
    if (v && v->type == op::json::Type::String) {
        retstr = v->str;
        *ret = 1;
    }
}

void op::Op::JsonGetNumber(const wchar_t *json_text, const wchar_t *path, double *value, long *ret) {
    *ret = 0;
    if (value)
        *value = 0.0;
    op::json::Value root;
    std::vector<op::json::PathSeg> segs;
    const op::json::Value *v = Locate(json_text, path, segs, root);
    if (v && v->type == op::json::Type::Number) {
        if (value)
            *value = v->number;
        *ret = 1;
    }
}

void op::Op::JsonGetBool(const wchar_t *json_text, const wchar_t *path, long *value, long *ret) {
    *ret = 0;
    if (value)
        *value = 0;
    op::json::Value root;
    std::vector<op::json::PathSeg> segs;
    const op::json::Value *v = Locate(json_text, path, segs, root);
    if (v && v->type == op::json::Type::Bool) {
        if (value)
            *value = v->boolean ? 1 : 0;
        *ret = 1;
    }
}

long op::Op::JsonGetSize(const wchar_t *json_text, const wchar_t *path) {
    op::json::Value root;
    std::vector<op::json::PathSeg> segs;
    const op::json::Value *v = Locate(json_text, path, segs, root);
    if (!v)
        return -1;
    if (v->type == op::json::Type::Array)
        return static_cast<long>(v->arr.size());
    if (v->type == op::json::Type::Object)
        return static_cast<long>(v->obj.size());
    return -1;
}

void op::Op::JsonGetKeys(const wchar_t *json_text, const wchar_t *path, std::wstring &retstr, long *ret) {
    retstr.clear();
    *ret = 0;
    op::json::Value root;
    std::vector<op::json::PathSeg> segs;
    const op::json::Value *v = Locate(json_text, path, segs, root);
    if (!v || v->type != op::json::Type::Object)
        return;
    op::json::Value keys;
    keys.type = op::json::Type::Array;
    for (const auto &kv : v->obj) {
        op::json::Value k;
        k.type = op::json::Type::String;
        k.str = kv.first;
        keys.arr.emplace_back(std::move(k));
    }
    retstr = op::json::Stringify(keys);
    *ret = 1;
}

void op::Op::JsonSetString(const wchar_t *json_text, const wchar_t *path, const wchar_t *value,
                           std::wstring &retstr, long *ret) {
    retstr.clear();
    *ret = 0;
    if (!value)
        return;
    op::json::Value root;
    std::vector<op::json::PathSeg> segs;
    op::json::Value *v = LocateForWrite(json_text, path, segs, root);
    if (!v)
        return;
    v->type = op::json::Type::String;
    v->boolean = false;
    v->number = 0.0;
    v->str = value;
    v->arr.clear();
    v->obj.clear();
    retstr = op::json::Stringify(root);
    *ret = 1;
}

void op::Op::JsonSetNumber(const wchar_t *json_text, const wchar_t *path, double value, std::wstring &retstr,
                           long *ret) {
    retstr.clear();
    *ret = 0;
    op::json::Value root;
    std::vector<op::json::PathSeg> segs;
    op::json::Value *v = LocateForWrite(json_text, path, segs, root);
    if (!v)
        return;
    v->type = op::json::Type::Number;
    v->number = value;
    v->boolean = false;
    v->str.clear();
    v->arr.clear();
    v->obj.clear();
    retstr = op::json::Stringify(root);
    *ret = 1;
}

void op::Op::JsonSetBool(const wchar_t *json_text, const wchar_t *path, long value, std::wstring &retstr,
                         long *ret) {
    retstr.clear();
    *ret = 0;
    op::json::Value root;
    std::vector<op::json::PathSeg> segs;
    op::json::Value *v = LocateForWrite(json_text, path, segs, root);
    if (!v)
        return;
    v->type = op::json::Type::Bool;
    v->boolean = value != 0;
    v->number = 0.0;
    v->str.clear();
    v->arr.clear();
    v->obj.clear();
    retstr = op::json::Stringify(root);
    *ret = 1;
}

void op::Op::JsonAppend(const wchar_t *json_text, const wchar_t *path, const wchar_t *raw_json,
                        std::wstring &retstr, long *ret) {
    retstr.clear();
    *ret = 0;
    if (!raw_json)
        return;
    op::json::Value element;
    if (!op::json::Parse(raw_json, element))
        return;
    op::json::Value root;
    std::vector<op::json::PathSeg> segs;
    // 输入文本为 null/空串时视为空文档（从零建数组）；非法非空文本失败。
    if (json_text && json_text[0] != L'\0' && !op::json::Parse(json_text, root))
        return;
    if (!op::json::ParsePath(path ? path : L"", segs))
        return;
    op::json::Value *v = op::json::NavigateOrCreate(root, segs);
    if (!v)
        return;
    if (v->type == op::json::Type::Null) {
        v->type = op::json::Type::Array;
        v->arr.clear();
    }
    if (v->type != op::json::Type::Array)
        return;
    v->arr.emplace_back(std::move(element));
    retstr = op::json::Stringify(root);
    *ret = 1;
}

void op::Op::JsonDelete(const wchar_t *json_text, const wchar_t *path, std::wstring &retstr, long *ret) {
    retstr.clear();
    *ret = 0;
    op::json::Value root;
    if (!op::json::Parse(json_text ? json_text : L"", root))
        return;
    std::vector<op::json::PathSeg> segs;
    if (!op::json::ParsePath(path ? path : L"", segs))
        return;
    if (!op::json::DeleteAt(root, segs))
        return;
    retstr = op::json::Stringify(root);
    *ret = 1;
}
