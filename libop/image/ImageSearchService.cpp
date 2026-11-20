// #include "stdafx.h"
#include "ImageSearchService.h"
#include "../base/Utils.h"
#include "../json/JsonValue.h"
#include "../ocr/OcrService.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <bitset>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <sstream>

namespace op::image {

using op::ocr::HttpOcrService;

namespace {
// 普通找图模板缓存。放在进程级别，多个 Op 实例共享同一份已解码图片和预处理模板。
// 匹配时会复制 shared_ptr 快照；FreePic 只移除缓存入口，不会提前释放正在匹配的图片。
std::map<wstring, std::shared_ptr<Image>> g_pic_cache;
std::map<wstring, std::shared_ptr<PicMatchTemplate>> g_pic_match_cache;
std::shared_mutex g_pic_cache_mutex;
// 图片缓存条目上限。达到上限（且不是刷新已有条目）时整体清空重存——
// 简单可预期，不做 LRU；防止长跑/遍历大模板目录时缓存单调膨胀。
std::atomic<long> g_pic_cache_max_entries{500};

// SetDict 加载的是文件字库，放在进程级槽位中，多个 Op 对象可以直接复用。
std::array<std::shared_ptr<Dictionary>, ImageSearchService::_max_dict> g_file_dicts;
std::shared_mutex g_file_dict_mutex;

template <typename Target, typename Value> bool set_out(Target *target, Value value) {
    if (!target)
        return false;
    *target = static_cast<Target>(value);
    return true;
}

color_t sim_to_point_color_diff(double sim) {
    if (sim < 0.0 || sim > 1.0)
        sim = 1.0;

    const auto diff = static_cast<uchar>(std::ceil((1.0 - sim) * 255.0));
    color_t color_diff;
    color_diff.b = diff;
    color_diff.g = diff;
    color_diff.r = diff;
    return color_diff;
}

// OCR 免字库路径（ocr_auto_color 系）的隐式颜色容差。
// 实测抗锯齿文字在"零容差(碎核心)"与"宽容差(全笔画)"下识别最好，
// 中间带（如 0x40 只包进半个抗锯齿边缘）反而最差；
// 0x60 能包全笔画，而背景色差普遍 >200 不会误包。
constexpr int kOcrImplicitColorDiff = 0x60;

// 免字库共用：二值图（1=文字, 0=背景）-> 白字黑底 BGRA 缓冲，喂给 OCR 引擎。
void binary_to_white_on_black_bgra(const unsigned char *bin, int w, int h, std::vector<unsigned char> &buf) {
    buf.assign(static_cast<size_t>(w) * h * 4, 0);
    for (int i = 0; i < w * h; ++i) {
        const unsigned char v = bin[i] ? 255 : 0; // 命中文字->白，背景->黑
        buf[size_t(i) * 4 + 0] = v;               // B
        buf[size_t(i) * 4 + 1] = v;               // G
        buf[size_t(i) * 4 + 2] = v;               // R
        buf[size_t(i) * 4 + 3] = 255;             // A
    }
}

// sim 钳制：免字库 onnx 输出 conf≈0.85+，默认阈值 0.7（1.0 会全滤掉）。
double clamp_sim_default(double sim) {
    return (sim < 0. || sim > 1.) ? 0.7 : sim;
}

// sim 钳制：字库匹配/找线等"精确"路径默认 1.0。
double clamp_sim_exact(double sim) {
    return (sim < 0. || sim > 1.) ? 1. : sim;
}

// 免字库共用：引擎调用 + 置信度过滤一体化，res 只保留 conf >= sim 的条目。
// OcrFree/OcrEx/ocr_auto_color(_line/_ex)/FindStr/FindStrEx 的重复样板收拢于此。
void engine_ocr_filtered(unsigned char *data, int w, int h, double sim, vocr_rec_t &res) {
    HttpOcrService::getInstance()->ocr(data, w, h, 4, res);
    res.erase(std::remove_if(res.begin(), res.end(),
                             [sim](const ocr_rec_t &r) { return r.confidence < sim - 1e-9; }),
              res.end());
}

// 逐字版（P0）：与 engine_ocr_filtered 完全同一套过滤，ext 与 res 保持等长同序
// （过滤时按相同条件同步剔除）。引擎不支持逐字时 ext 各元素为空结构。
void engine_ocr_filtered_ext(unsigned char *data, int w, int h, double sim, vocr_rec_t &res,
                             vocr_line_ext_t &ext) {
    HttpOcrService::getInstance()->ocr_ext(data, w, h, 4, res, ext);
    // 引擎若未回填（长度不匹配），补齐成空结构以保证等长
    if (ext.size() != res.size()) ext.assign(res.size(), ocr_line_ext_t{});
    // 同步过滤：按 conf 阈值同时剔除 res 与 ext，保持一一对应
    size_t out = 0;
    for (size_t i = 0; i < res.size(); ++i) {
        if (res[i].confidence < sim - 1e-9) continue;
        if (out != i) {
            res[out] = std::move(res[i]);
            ext[out] = std::move(ext[i]);
        }
        ++out;
    }
    res.resize(out);
    ext.resize(out);
}

template <typename Fn> void for_each_dict_line(const wstring &dict_info, Fn fn) {
    long item_index = 0;
    size_t begin = 0;
    while (begin <= dict_info.size()) {
        size_t end = dict_info.find(L'\n', begin);
        if (end == std::wstring::npos)
            end = dict_info.size();

        std::wstring item = dict_info.substr(begin, end - begin);
        if (!item.empty() && item.back() == L'\r')
            item.pop_back();

        if (!item.empty())
            fn(item_index++, item);

        if (end == dict_info.size())
            break;
        begin = end + 1;
    }
}

long clamp_preprocess_mode(long mode) {
    if (mode < 0)
        return 0;
    if (mode > 3)
        return 3;
    return mode;
}

long clamp_long(long value, long low, long high) {
    if (value < low)
        return low;
    if (value > high)
        return high;
    return value;
}

std::shared_ptr<PicMatchTemplate> make_pic_match(std::shared_ptr<Image> image) {
    if (!image || image->empty())
        return nullptr;

    auto match = std::make_shared<PicMatchTemplate>();
    build_pic_match_template(*image, *match);
    return match;
}

bool find_cached_pic(const wstring &key, std::shared_ptr<Image> &image, std::shared_ptr<PicMatchTemplate> &match) {
    std::shared_lock<std::shared_mutex> lock(g_pic_cache_mutex);
    auto it = g_pic_cache.find(key);
    if (it == g_pic_cache.end())
        return false;

    auto match_it = g_pic_match_cache.find(key);
    if (match_it == g_pic_match_cache.end())
        return false;

    image = it->second;
    match = match_it->second;
    return image && match;
}

bool store_cached_pic(const wstring &key, std::shared_ptr<Image> image, std::shared_ptr<PicMatchTemplate> match) {
    if (key.empty() || !image || image->empty() || !match)
        return false;

    std::unique_lock<std::shared_mutex> lock(g_pic_cache_mutex);
    const long max_entries = g_pic_cache_max_entries.load(std::memory_order_relaxed);
    // 达到上限（且不是刷新已有条目）时整体清空重存，防止长跑场景缓存单调膨胀。
    // max_entries<=0 = 不设上限。
    if (max_entries > 0 && g_pic_cache.find(key) == g_pic_cache.end() &&
        static_cast<long>(g_pic_cache.size()) >= max_entries) {
        g_pic_cache.clear();
        g_pic_match_cache.clear();
    }
    g_pic_cache[key] = std::move(image);
    g_pic_match_cache[key] = std::move(match);
    return true;
}

bool store_cached_pic(const wstring &key, std::shared_ptr<Image> image) {
    return store_cached_pic(key, image, make_pic_match(image));
}

bool erase_cached_pic(const wstring &key) {
    std::unique_lock<std::shared_mutex> lock(g_pic_cache_mutex);
    const bool erased_image = g_pic_cache.erase(key) > 0;
    const bool erased_match = g_pic_match_cache.erase(key) > 0;
    return erased_image || erased_match;
}

std::shared_ptr<Image> read_pic_file(const wstring &path) {
    auto image = std::make_shared<Image>();
    if (!image->read(path.data()) || image->empty())
        return nullptr;
    return image;
}

std::shared_ptr<Image> read_mem_pic(void *data, long size) {
    if (!data || size <= 0)
        return nullptr;

    auto image = std::make_shared<Image>();
    if (!image->read(data, size) || image->empty())
        return nullptr;
    return image;
}

std::shared_ptr<Dictionary> find_global_file_dict(int idx) {
    if (idx < 0 || idx >= ImageSearchService::_max_dict)
        return nullptr;

    std::shared_lock<std::shared_mutex> lock(g_file_dict_mutex);
    return g_file_dicts[static_cast<size_t>(idx)];
}

bool store_global_file_dict(int idx, std::shared_ptr<Dictionary> dict) {
    if (idx < 0 || idx >= ImageSearchService::_max_dict || !dict || dict->empty())
        return false;

    std::unique_lock<std::shared_mutex> lock(g_file_dict_mutex);
    g_file_dicts[static_cast<size_t>(idx)] = std::move(dict);
    return true;
}

void clear_global_file_dict(int idx) {
    if (idx < 0 || idx >= ImageSearchService::_max_dict)
        return;

    std::unique_lock<std::shared_mutex> lock(g_file_dict_mutex);
    g_file_dicts[static_cast<size_t>(idx)].reset();
}
} // namespace

ImageSearchService::ImageSearchService() {
    _curr_idx = 0;
    for (bool &it : _private_dict_overrides)
        it = false;
    _enable_cache = 1;
    // 默认开启保守去噪（mode=1）：自动删除完全孤立的 1 像素噪点。
    // 不误伤连通笔画（笔画端点至少含 1 个邻居），可显著缓解"大→太"类噪点误判。
    // 如需完全精确的逐像素模式，调用 SetBinaryPreprocess(0, 0, 2, 1) 关回。
    _binary_preprocess_mode = 1;
    _binary_isolated_threshold = 0;
    _binary_min_component_area = 2;
    _binary_bridge_gap = 1;
}

ImageSearchService::~ImageSearchService() {
}

long ImageSearchService::Capture(const std::wstring &file) {
    std::filesystem::path fpath(file);
    if (!fpath.is_absolute())
        fpath = std::filesystem::path(_curr_path) / fpath;

    const long ret = _src.write(fpath.c_str());
    if (ret != 1) {
        // 抓屏本身已成功，失败的是落盘这一步。CImage::Save 按扩展名查 GDI+
        // 编码器：路径没有扩展名、或扩展名不受支持（可用 .bmp/.png/.jpg/
        // .jpeg/.gif/.tif/.tiff），会直接返回非 S_OK；其次是目录不存在或
        // 无写权限。这三种原因表现完全一样（capture 返回 0 且无任何提示）。
        setlog(L"capture write failed: %s (%dx%d), check the file extension"
               L"(.bmp/.png/.jpg) and that the directory exists",
               fpath.c_str(), _src.width, _src.height);
    }
    return ret;
}

long ImageSearchService::CmpColor(long x, long y, const std::wstring &scolor, double sim) {
    std::vector<color_df_t> vcolor;
    str2colordfs(scolor, vcolor);
    color_t color;
    if (!ImageSearchAlgorithms::GetPixel(x, y, color))
        return 0;
    return ImageSearchAlgorithms::CmpColor(color, vcolor, sim);
}

long ImageSearchService::FindColor(const wstring &color, double sim, long dir, long &x, long &y) {
    std::vector<color_df_t> colors;
    str2colordfs(color, colors);
    // setlog("%s cr size=%d",colors[0].color.tostr().data(), colors.size());
    // setlog("sim:,dir:%d", dir);
    return ImageSearchAlgorithms::FindColor(colors, sim, dir, x, y);
}

long ImageSearchService::FindColorEx(const wstring &color, double sim, long dir, wstring &retstr) {
    std::vector<color_df_t> colors;
    str2colordfs(color, colors);
    return ImageSearchAlgorithms::FindColorEx(colors, sim, dir, retstr);
}

void ImageSearchService::parse_multi_color_args(const wstring &first_color, const wstring &offset_color,
                                                std::vector<color_df_t> &vfirst_color,
                                                std::vector<pt_cr_df_t> &voffset_cr) {
    str2colordfs(first_color, vfirst_color);

    // offset_color 兼容旧格式: x|y|颜色描述，多段之间用英文逗号分隔。
    std::vector<wstring> vseconds;
    split(offset_color, vseconds, L",");
    voffset_cr.clear();
    for (auto &it : vseconds) {
        size_t id1, id2;
        id1 = it.find(L'|');
        id2 = (id1 == wstring::npos ? wstring::npos : it.find(L'|', id1 + 1));
        if (id2 == wstring::npos)
            continue;
        // 偏移必须先清零再解析：sscanf 失败时未初始化的 x/y 会以垃圾值入 vector，
        // 导致同一串输入在不同调用间匹配行为不确定。
        pt_cr_df_t tp = {};
        if (swscanf(it.c_str(), L"%d|%d", &tp.x, &tp.y) != 2) {
            setlog(L"parse_multi_color_args: malformed offset segment skipped: %s", it.c_str());
            continue;
        }
        if (id2 + 1 == it.length()) {
            setlog(L"parse_multi_color_args: offset segment without color skipped: %s", it.c_str());
            continue;
        }
        str2colordfs(it.substr(id2 + 1), tp.crdfs);
        voffset_cr.push_back(tp);
    }
}

long ImageSearchService::FindMultiColor(const wstring &first_color, const wstring &offset_color, double sim, long dir,
                                        long &x, long &y) {
    std::vector<color_df_t> vfirst_color;
    std::vector<pt_cr_df_t> voffset_cr;
    parse_multi_color_args(first_color, offset_color, vfirst_color, voffset_cr);
    return ImageSearchAlgorithms::FindMultiColor(vfirst_color, voffset_cr, sim, dir, x, y);
}

long ImageSearchService::FindMultiColorEx(const wstring &first_color, const wstring &offset_color, double sim, long dir,
                                          wstring &retstr) {
    std::vector<color_df_t> vfirst_color;
    std::vector<pt_cr_df_t> voffset_cr;
    parse_multi_color_args(first_color, offset_color, vfirst_color, voffset_cr);
    return ImageSearchAlgorithms::FindMultiColorEx(vfirst_color, voffset_cr, sim, dir, retstr);
}
// 图形定位
long ImageSearchService::FindPic(const std::wstring &files, const wstring &delta_colors, double sim, long dir, long &x,
                        long &y) {
    find_pic_ctx_t ctx;
    prepare_find_pic(files, delta_colors, sim, ctx);
    return ImageSearchAlgorithms::FindPicTh(ctx.vmatches, ctx.dfcolor, sim, dir, x, y);
}

void ImageSearchService::prepare_find_pic(const wstring &files, const wstring &delta_colors, double &sim,
                                          find_pic_ctx_t &ctx) {
    files2mats(files, ctx.vmatches, ctx.vpic_name, ctx.holders, ctx.match_holders);
    if (!ctx.dfcolor.try_str2color(delta_colors))
        setlog(L"FindPic: 非法偏色串 '%s'（期望 6 位十六进制，如 606060），已按 000000 处理", delta_colors.c_str());
    sim = 0.5 + sim / 2;
}
//
long ImageSearchService::FindPicEx(const std::wstring &files, const wstring &delta_colors, double sim, long dir, wstring &retstr,
                          bool returnID) {
    find_pic_ctx_t ctx;
    prepare_find_pic(files, delta_colors, sim, ctx);
    vpoint_desc_t vpd;
    long ret = ImageSearchAlgorithms::FindPicExTh(ctx.vmatches, ctx.dfcolor, sim, dir, vpd);
    std::wstringstream ss(std::wstringstream::in | std::wstringstream::out);
    if (returnID) {
        for (auto &it : vpd) {
            ss << it.id << L"," << it.pos << L"|";
        }
    } else {
        for (auto &it : vpd) {
            ss << ctx.vpic_name[it.id] << L"," << it.pos << L"|";
        }
    }
    retstr = ss.str();
    if (vpd.size())
        retstr.pop_back();
    return ret;
}

long ImageSearchService::FindColorBlock(const wstring &color, double sim, long count, long height, long width, long &x,
                               long &y) {
    str2binaryfbk(color, sim);
    return ImageSearchAlgorithms::FindColorBlock(count, height, width, x, y);
}

long ImageSearchService::FindColorBlockEx(const wstring &color, double sim, long count, long height, long width,
                                 wstring &retstr) {
    str2binaryfbk(color, sim);
    return ImageSearchAlgorithms::FindColorBlockEx(count, height, width, retstr);
}

long ImageSearchService::FindColorBlockExS(const wstring &color, double sim, long count, long height, long width,
                                           long mode, wstring &retstr) {
    str2binaryfbk(color, sim);
    return ImageSearchAlgorithms::FindColorBlockExS(count, height, width, mode, retstr);
}

long ImageSearchService::GetColorNum(const wstring &color, double sim) {
    std::vector<color_df_t> colors;
    str2colordfs(color, colors);
    return ImageSearchAlgorithms::FindColorNum(colors, sim);
}

long ImageSearchService::SetDict(int idx, const wstring &file_name) {
    if (idx < 0 || idx >= _max_dict)
        return 0;
    _private_dicts[idx].clear();
    _private_dict_overrides[idx] = false;
    wstring fullpath;
    if (Path2GlobalPath(file_name, _curr_path, fullpath)) {
        auto dict = std::make_shared<Dictionary>();
        // SetDict 按文件内容识别：OP 二进制 .dict 优先，失败后兼容大漠 txt。
        dict->read_dict(fullpath);
        if (store_global_file_dict(idx, dict))
            return 1;
        clear_global_file_dict(idx);
        return 0;
    } else {
        clear_global_file_dict(idx);
        setlog(L"file '%s' does not exist", file_name.c_str());
    }

    return 0;
}

std::wstring ImageSearchService::GetDict(long idx, long font_index) {
    wstring tp;
    if (idx < 0 || idx >= _max_dict)
        return tp;
    auto dict = ActiveDict(static_cast<int>(idx));
    if (!dict || font_index < 0 || static_cast<size_t>(font_index) >= dict->words.size())
        return tp;
    return dict->words[font_index].to_string();
}

long ImageSearchService::SetMemDict(int idx, const void *data, long size) {
    if (idx < 0 || idx >= _max_dict || !data || size <= 0)
        return 0;

    auto dict = std::make_shared<Dictionary>();
    if (!dict->read_memory_dict(data, static_cast<size_t>(size)) || dict->empty())
        return 0;

    if (!store_global_file_dict(idx, dict))
        return 0;

    _private_dicts[idx].clear();
    _private_dict_overrides[idx] = false;
    return 1;
}

long ImageSearchService::UseDict(int idx) {
    if (idx < 0 || idx >= _max_dict)
        return 0;
    _curr_idx = idx;
    return 1;
}

long ImageSearchService::AddDict(long idx, const wstring &dict_info) {
    if (idx < 0 || idx >= _max_dict)
        return 0;

    word1_t word;
    if (!dict_entry_importer::parse_text_dict_entry(dict_info, word))
        return 0;
    MutablePrivateDict(static_cast<int>(idx)).add_word(word);
    return 1;
}

long ImageSearchService::SaveDict(long idx, const wstring &file_name) {
    if (idx < 0 || idx >= _max_dict)
        return 0;
    auto dict = ActiveDict(static_cast<int>(idx));
    if (!dict)
        return 0;
    Dictionary snapshot = *dict;
    return snapshot.write_dict(file_name) ? 1 : 0;
}

long ImageSearchService::ClearDict(long idx) {
    if (idx < 0 || idx >= _max_dict)
        return 0;

    _private_dicts[idx].clear();
    _private_dict_overrides[idx] = true;
    return 1;
}

long ImageSearchService::GetDictCount(long idx) {
    if (idx < 0 || idx >= _max_dict)
        return 0;

    auto dict = ActiveDict(static_cast<int>(idx));
    return dict ? dict->info._word_count : 0;
}

long ImageSearchService::GetNowDict() {
    return _curr_idx;
}

std::shared_ptr<Dictionary> ImageSearchService::ActiveDict(int idx) {
    if (idx < 0 || idx >= _max_dict)
        return nullptr;
    if (!_private_dicts[idx].empty())
        return std::shared_ptr<Dictionary>(&_private_dicts[idx], [](Dictionary *) {});
    if (_private_dict_overrides[idx])
        return nullptr;

    auto dict = find_global_file_dict(idx);
    return dict && !dict->empty() ? dict : nullptr;
}

Dictionary &ImageSearchService::MutablePrivateDict(int idx) {
    if (_private_dicts[idx].empty() && !_private_dict_overrides[idx]) {
        if (auto global_dict = find_global_file_dict(idx)) {
            // 修改字库前先拷贝全局槽，避免 AddDict 影响其它对象正在使用的文件字库。
            _private_dicts[idx] = *global_dict;
        }
    }
    _private_dict_overrides[idx] = true;
    return _private_dicts[idx];
}

long ImageSearchService::SetBinaryPreprocess(long mode, long isolated_threshold, long min_component_area,
                                             long bridge_gap) {
    _binary_preprocess_mode = clamp_preprocess_mode(mode);
    _binary_isolated_threshold = clamp_long(isolated_threshold, 0, 8);
    _binary_min_component_area = min_component_area <= 0 ? 2 : clamp_long(min_component_area, 1, 4096);
    _binary_bridge_gap = bridge_gap > 0 ? 1 : 0;
    return 1;
}

long ImageSearchService::GetBinaryPreprocess(long &mode, long &isolated_threshold, long &min_component_area,
                                             long &bridge_gap) const {
    mode = _binary_preprocess_mode;
    isolated_threshold = _binary_isolated_threshold;
    min_component_area = _binary_min_component_area;
    bridge_gap = _binary_bridge_gap;
    return 1;
}

wstring ImageSearchService::FetchWord(rect_t rc, const wstring &color, const wstring &word) {
    return FetchWord(rc, color, 1.0, word);
}

wstring ImageSearchService::FetchWord(rect_t rc, const wstring &color, double sim, const wstring &word) {
    str2pointbinaryfbk(color, sim);
    return FetchWordFromBinary(rc, word);
}

wstring ImageSearchService::FetchWordFromBinary(rect_t rc, const wstring &word) {
    auto orc = rc;
    if (!bin_image_cut(2, rc, orc))
        return L"";
    // check is too large（字模 w/h 为 uint8，上限 255；超限部分静默裁剪，记日志提示用户收窄取模区域）
    if (orc.width() > 255) {
        setlog(L"FetchWord: width %d > 255, truncated to 255 (narrow the rect to avoid losing glyph columns)",
               orc.width());
        orc.x2 = orc.x1 + 255;
        rc = orc;
        if (!bin_image_cut(2, rc, orc))
            return L"";
    }
    if (orc.height() > 255) {
        setlog(L"FetchWord: height %d > 255, truncated to 255 (narrow the rect to avoid losing glyph rows)",
               orc.height());
        orc.y2 = orc.y1 + 255;
        rc = orc;
        if (!bin_image_cut(2, rc, orc))
            return L"";
    }
    Dictionary dict_new;
    dict_new.add_word(_binary, orc);
    auto &wt = dict_new.words[0];
    wt.set_chars(word);
    return wt.to_string();
}

long ImageSearchService::FetchWordsFromBinary(const wstring &words, const std::vector<rect_t> &rects,
                                              std::wstring &out_str) {
    out_str.clear();
    if (rects.empty() || rects.size() != words.size()) {
        setlog(L"FetchWords: extracted %zu word rect(s) but words has %zu char(s), they must be equal; returns empty",
               rects.size(), words.size());
        return 0;
    }

    for (size_t i = 0; i < rects.size(); ++i) {
        if (!rects[i].valid() || rects[i].x2 > _src.width || rects[i].y2 > _src.height) {
            out_str.clear();
            return 0;
        }

        const std::wstring word(1, words[i]);
        const std::wstring entry = FetchWordFromBinary(rects[i], word);
        if (entry.empty()) {
            out_str.clear();
            return 0;
        }
        out_str += entry;
        out_str += L"\n";
    }
    if (!out_str.empty())
        out_str.pop_back();
    return static_cast<long>(rects.size());
}

long ImageSearchService::ExtractWordRects(const wstring &color, double sim, long min_word_h, std::vector<rect_t> &rects) {
    rects.clear();
    if (min_word_h <= 0)
        min_word_h = 2;
    str2pointbinaryfbk(color, sim);
    get_rois(static_cast<int>(min_word_h), rects);
    return static_cast<long>(rects.size());
}

long ImageSearchService::ExtractWordRectsEx(const wstring &color, double sim, long min_word_w, long min_word_h,
                                            long padding, std::vector<rect_t> &rects) {
    rects.clear();
    if (min_word_w <= 0)
        min_word_w = 1;
    if (min_word_h <= 0)
        min_word_h = 2;
    if (padding < 0)
        padding = 0;

    str2pointbinaryfbk(color, sim);
    get_rois(static_cast<int>(min_word_w), static_cast<int>(min_word_h), static_cast<int>(padding), rects);
    return static_cast<long>(rects.size());
}

long ImageSearchService::FetchWords(const wstring &color, double sim, const wstring &words, long min_word_h,
                                    std::wstring &out_str) {
    out_str.clear();
    if (min_word_h <= 0)
        min_word_h = 2;
    str2pointbinaryfbk(color, sim);
    std::vector<rect_t> rects;
    get_rois(static_cast<int>(min_word_h), rects);
    const long rect_count = static_cast<long>(rects.size());
    if (rect_count == 0 || rects.size() != words.size()) {
        setlog(L"FetchWords: extracted %zu word rect(s) but words has %zu char(s), they must be equal; returns empty",
               rects.size(), words.size());
        return 0;
    }
    return FetchWordsFromBinary(words, rects, out_str);
}

long ImageSearchService::FetchWordsEx(const wstring &color, double sim, const wstring &words, long min_word_w,
                                      long min_word_h, long padding, std::wstring &out_str) {
    out_str.clear();
    if (min_word_w <= 0)
        min_word_w = 1;
    if (min_word_h <= 0)
        min_word_h = 2;
    if (padding < 0)
        padding = 0;

    std::vector<rect_t> rects;
    str2pointbinaryfbk(color, sim);
    get_rois(static_cast<int>(min_word_w), static_cast<int>(min_word_h), static_cast<int>(padding), rects);
    const long rect_count = static_cast<long>(rects.size());
    if (rect_count == 0)
        return 0;
    return FetchWordsFromBinary(words, rects, out_str);
}

long ImageSearchService::FetchWordsByRects(const wstring &color, double sim, const wstring &words,
                                           const std::vector<rect_t> &rects, std::wstring &out_str) {
    out_str.clear();
    if (rects.empty() || rects.size() != words.size())
        return 0;
    str2pointbinaryfbk(color, sim);
    return FetchWordsFromBinary(words, rects, out_str);
}

long ImageSearchService::GetBinaryPreview(const wstring &color, double sim, std::wstring &out_str) {
    out_str.clear();
    str2pointbinaryfbk(color, sim);
    if (_binary.empty())
        return 0;

    long point_count = 0;
    out_str += std::to_wstring(_binary.width);
    out_str += L",";
    out_str += std::to_wstring(_binary.height);
    out_str += L"\n";
    for (int y = 0; y < _binary.height; ++y) {
        for (int x = 0; x < _binary.width; ++x) {
            if (_binary.at(y, x) == WORD_COLOR) {
                out_str += L"#";
                ++point_count;
            } else {
                out_str += L".";
            }
        }
        if (y + 1 < _binary.height)
            out_str += L"\n";
    }
    return point_count;
}

long ImageSearchService::GetWordPreview(const wstring &dict_info, std::wstring &out_str) {
    out_str.clear();
    word1_t word;
    if (!dict_entry_importer::parse_text_dict_entry(dict_info, word))
        return 0;

    out_str += word.info.name;
    out_str += L",";
    out_str += std::to_wstring(word.info.w);
    out_str += L",";
    out_str += std::to_wstring(word.info.h);
    out_str += L",";
    out_str += std::to_wstring(word.info.bit_cnt);
    out_str += L"\n";

    for (int y = 0; y < word.info.h; ++y) {
        for (int x = 0; x < word.info.w; ++x) {
            const int idx = x * word.info.h + y;
            out_str += GET_BIT(word.data[idx / 8], idx & 7) ? L"#" : L".";
        }
        if (y + 1 < word.info.h)
            out_str += L"\n";
    }
    return 1;
}

long ImageSearchService::CheckWordDict(const wstring &dict_info, std::wstring &out_str) {
    out_str.clear();

    long valid_count = 0;
    for_each_dict_line(dict_info, [&](long item_index, const std::wstring &item) {
        word1_t word;
        if (dict_entry_importer::parse_text_dict_entry(item, word)) {
            const int area = word.info.w * word.info.h;
            const int density = area > 0 ? static_cast<int>((word.info.bit_cnt * 100 + area / 2) / area) : 0;
            out_str += std::to_wstring(item_index);
            out_str += L",1,";
            out_str += word.info.name;
            out_str += L",";
            out_str += std::to_wstring(word.info.w);
            out_str += L",";
            out_str += std::to_wstring(word.info.h);
            out_str += L",";
            out_str += std::to_wstring(word.info.bit_cnt);
            out_str += L",";
            out_str += std::to_wstring(density);
            ++valid_count;
        } else {
            out_str += std::to_wstring(item_index);
            out_str += L",0,invalid";
        }
        out_str += L"|";
    });

    if (!out_str.empty())
        out_str.pop_back();
    return valid_count;
}

long ImageSearchService::NormalizeWordDict(const wstring &dict_info, std::wstring &out_str) {
    out_str.clear();

    long valid_count = 0;
    for_each_dict_line(dict_info, [&](long item_index, const std::wstring &item) {
        word1_t word;
        if (dict_entry_importer::parse_text_dict_entry(item, word)) {
            if (!out_str.empty())
                out_str += L"\n";
            out_str += word.to_string();
            ++valid_count;
        } else {
            setlog(L"NormalizeWordDict: line %ld dropped (unparseable dict entry)", item_index + 1);
        }
    });

    return valid_count;
}

long ImageSearchService::RenameWordDict(const wstring &dict_info, const wstring &words, std::wstring &out_str) {
    out_str.clear();

    std::vector<word1_t> entries;
    for_each_dict_line(dict_info, [&](long, const std::wstring &item) {
        word1_t word;
        if (dict_entry_importer::parse_text_dict_entry(item, word))
            entries.push_back(word);
    });

    if (entries.empty() || entries.size() != words.size()) {
        setlog(L"RenameWordDict: %zu valid dict entr(ies) but words has %zu char(s), they must be equal; returns empty",
               entries.size(), words.size());
        return 0;
    }

    for (size_t i = 0; i < entries.size(); ++i) {
        entries[i].set_chars(std::wstring(1, words[i]));
        if (!out_str.empty())
            out_str += L"\n";
        out_str += entries[i].to_string();
    }

    return static_cast<long>(entries.size());
}

long ImageSearchService::OcrFree(double sim, std::wstring &out_str) {
    out_str.clear();
    sim = clamp_sim_default(sim);
    if (_src.width <= 0 || _src.height <= 0)
        return 0; // 与 ocr_auto_color 系对齐：截屏/读图失败早退，不喂 0x0 给引擎
    vocr_rec_t res;
    engine_ocr_filtered(_src.pdata, _src.width, _src.height, sim, res);
    for (auto &it : res) {
        out_str += it.text;
    }
    // 返回过滤后条数，与 OcrEx 免字库路径对齐（原恒返 0，见 OC4）
    return static_cast<long>(res.size());
}

long ImageSearchService::OCR(const wstring &color, double sim, std::wstring &out_str) {
    out_str.clear();
    sim = clamp_sim_default(sim);
    auto dict = ActiveDict(_curr_idx);
    if (!dict) {
        // 免字库路径：直接对整幅区域原图识别，color 不参与（要按颜色过滤请用 OcrAutoColor 系）
        return OcrFree(sim, out_str);
    }
    str2pointbinaryfbk(color, sim);
    return ImageSearchAlgorithms::Ocr(*dict, sim, out_str);
}

long ImageSearchService::ocr_auto_color(const wstring &color, double sim, wstring &out_str) {
    out_str.clear();
    sim = clamp_sim_default(sim);

    // 1) 按颜色二值化 -> _binary（WORD_COLOR=1 命中文字，WORD_BKCOLOR=0 背景）。
    //    用 str2binaryfbk 而非裸 str2colordfs+bgr2binary：它支持 "@背景色" 格式
    //    （分流到 bgr2binarybk，反白字/白字深底场景），与字库制作同一入口。
    //    无显式偏色段时容差用 kOcrImplicitColorDiff（0x60 包全抗锯齿笔画），不用 sim 推导——
    //    中间容差带（只包半个边缘）比零容差更糟，实测结论。
    str2binaryfbk(color, 1.0, kOcrImplicitColorDiff);

    auto dict = ActiveDict(_curr_idx);
    if (dict) {
        // 字库兜底：在颜色二值化结果上做字库 OCR
        return ImageSearchAlgorithms::Ocr(*dict, sim, out_str);
    }

    // 2) 免字库：把二值图转成白字黑底 BGRA 缓冲，喂给 OnnxOcrEngine
    int w = _binary.width, h = _binary.height;
    if (w <= 0 || h <= 0)
        return 0;
    std::vector<unsigned char> buf;
    binary_to_white_on_black_bgra(_binary.data(), w, h, buf);
    vocr_rec_t res;
    engine_ocr_filtered(buf.data(), w, h, sim, res);
    for (auto &it : res)
        out_str += it.text;
    return 0;
}

long ImageSearchService::ocr_auto_color_line(const wstring &color, double sim, wstring &out_str) {
    out_str.clear();
    sim = clamp_sim_default(sim);

    // 1) 按颜色二值化 -> _binary（命中文字=1，背景=0）。
    //    str2binaryfbk：支持 "@背景色" 反白格式，与字库制作同一入口。
    //    无显式偏色段时容差用 kOcrImplicitColorDiff，理由同 ocr_auto_color。
    str2binaryfbk(color, 1.0, kOcrImplicitColorDiff);

    auto dict = ActiveDict(_curr_idx);
    if (dict) {
        // 字库兜底：在颜色二值化结果上做字库 OCR
        return ImageSearchAlgorithms::Ocr(*dict, sim, out_str);
    }

    // 2) 免字库快路径：二值图水平投影切行 -> 每行独立直 rec（跳过 det，保持快路径）。
    //    必须裁行：整区域直 rec 会把含上下留白的区域等比压扁（字符糊），
    //    真机实测整窗 160px 高、文本行仅 35px 时 10px 级字符必错字（愿->原、丢负号）。
    int w = _binary.width, h = _binary.height;
    if (w <= 0 || h <= 0)
        return 0;
    const unsigned char *bin = _binary.data();
    // 水平投影：统计每行前景(文字)像素
    std::vector<int> row(size_t(h), 0);
    for (int y = 0; y < h; ++y) {
        const unsigned char *p = bin + size_t(y) * w;
        int cnt = 0;
        for (int x = 0; x < w; ++x)
            if (p[x]) ++cnt;
        row[size_t(y)] = cnt;
    }
    // 按行间空隙切段：行内字符纵向像素连续（或 <=2px 微隙），行间留白通常 >=3px；
    // 段高 < kMinH 视为噪点残留，丢弃。
    const int kMaxInLineGap = 3, kMinSegH = 5;
    std::vector<std::pair<int, int>> segs; // (y0, y1) 含
    for (int y = 0; y < h;) {
        if (row[size_t(y)] == 0) { ++y; continue; }
        int y0 = y, y1 = y, gap = 0;
        for (++y; y < h; ++y) {
            if (row[size_t(y)] != 0) { y1 = y; gap = 0; }
            else if (++gap > kMaxInLineGap) break;
        }
        if (y1 - y0 + 1 >= kMinSegH) segs.push_back({y0, y1});
    }
    if (segs.empty())
        return 0;

    for (const auto &s : segs) {
        int seg_h = s.second - s.first + 1;
        std::vector<unsigned char> buf;
        binary_to_white_on_black_bgra(bin + size_t(s.first) * w, w, seg_h, buf);
        vocr_rec_t res;
        HttpOcrService::getInstance()->ocr_line(buf.data(), w, seg_h, 4, res);
        res.erase(std::remove_if(res.begin(), res.end(),
                                 [sim](const ocr_rec_t &r) { return r.confidence < sim - 1e-9; }),
                  res.end());
        for (auto &it : res)
            out_str += it.text;
    }
    return 0;
}

long ImageSearchService::ocr_auto_color_ex(const wstring &color, double sim, wstring &out_str) {
    out_str.clear();
    sim = clamp_sim_default(sim);

    // 1) 按颜色二值化 -> _binary。
    //    str2binaryfbk：支持 "@背景色" 反白格式，与字库制作同一入口。
    //    无显式偏色段时容差用 kOcrImplicitColorDiff，理由同 ocr_auto_color。
    str2binaryfbk(color, 1.0, kOcrImplicitColorDiff);

    auto dict = ActiveDict(_curr_idx);
    if (dict) {
        // 字库兜底：沿用 OcrEx 的 "x,y,text|..." 格式
        return ImageSearchAlgorithms::OcrEx(*dict, sim, out_str);
    }

    // 2) 免字库：白字黑底 BGRA 缓冲 -> det+rec，bbox 偏移为客户区坐标（_x1/_y1 由 capture_region 设为区域左上角，_dx/_dy 恒 0）
    int w = _binary.width, h = _binary.height;
    if (w <= 0 || h <= 0)
        return 0;
    std::vector<unsigned char> buf;
    binary_to_white_on_black_bgra(_binary.data(), w, h, buf);
    vocr_rec_t res;
    engine_ocr_filtered(buf.data(), w, h, sim, res);
    // det 结果按阅读序稳定排序（上→下、同行左→右），输出顺序确定
    std::stable_sort(res.begin(), res.end(), [](const auto &a, const auto &b) {
        if (a.left_top.y != b.left_top.y)
            return a.left_top.y < b.left_top.y;
        return a.left_top.x < b.left_top.x;
    });
    long find_ct = 0;
    wchar_t itembuf[96];
    for (auto &it : res) {
        if (find_ct >= _max_return_obj_ct)
            break; // 先判上限再收集（置信度已在 engine_ocr_filtered 过滤）
        swprintf(itembuf, 96, L"%d,%d,%d,%d,%.2f,", it.left_top.x + _x1 + _dx, it.left_top.y + _y1 + _dy,
                 it.right_bottom.x + _x1 + _dx, it.right_bottom.y + _y1 + _dy, it.confidence);
        out_str += itembuf;
        out_str += it.text;
        out_str += L"|";
        ++find_ct;
    }
    if (!out_str.empty() && out_str.back() == L'|')
        out_str.pop_back();
    return find_ct;
}

wstring ImageSearchService::GetColor(long x, long y) {
    color_t cr;
    if (ImageSearchAlgorithms::GetPixel(x, y, cr)) {
        return _s2wstring(cr.tostr());
    } else {
        return L"";
    }
}

int ImageSearchService::str2colordfs(const wstring &color_str, std::vector<color_df_t> &colors) {
    return str2colordfs(color_str, colors, nullptr);
}

int ImageSearchService::str2colordfs(const wstring &color_str, std::vector<color_df_t> &colors, std::vector<bool> *explicit_dfs) {
    std::vector<wstring> vstr, vstr2;
    color_df_t cr;
    colors.clear();
    if (explicit_dfs)
        explicit_dfs->clear();
    int ret = 0;
    if (color_str.empty()) { // default
        return 1;
    }
    if (color_str[0] == L'@') { // bk color info
        ret = 1;
    }
    split(ret ? color_str.substr(1) : color_str, vstr, L"|");
    for (auto &it : vstr) {
        if (it.empty()) // 空段（前导/连续 '|'）跳过，防空 vector 取下标
            continue;
        split(it, vstr2, L"-");
        if (vstr2.size() > 2)
            setlog(L"str2colordfs: 颜色段 '%s' 含多个 '-'（偏色只能有一段），多余部分被忽略", it.c_str());
        if (!cr.color.try_str2color(vstr2[0]))
            setlog(L"str2colordfs: 非法颜色段 '%s'（期望 6 位十六进制 RRGGBB），已按 000000 处理", vstr2[0].c_str());
        const bool has_explicit_df = vstr2.size() == 2;
        if (has_explicit_df) {
            if (!cr.df.try_str2color(vstr2[1]))
                setlog(L"str2colordfs: 非法偏色段 '%s'（必须写满 6 位十六进制，如 202020），已按 000000 处理",
                       vstr2[1].c_str());
        } else {
            cr.df.str2color(L"000000");
        }
        colors.push_back(cr);
        if (explicit_dfs)
            explicit_dfs->push_back(has_explicit_df);
    }
    return ret;
}

void ImageSearchService::str2colors(const wstring &color, std::vector<color_t> &vcolor) {
    std::vector<wstring> vstr, vstr2;
    color_t cr;
    vcolor.clear();
    split(color, vstr, L"|");
    for (auto &it : vstr) {
        if (it.empty()) // 空段（前导/连续 '|'）跳过
            continue;
        if (!cr.try_str2color(it))
            setlog(L"str2colors: 非法颜色段 '%s'（期望 6 位十六进制 RRGGBB），已按 000000 处理", it.c_str());
        vcolor.push_back(cr);
    }
}

long ImageSearchService::LoadPic(const wstring &files) {
    std::vector<wstring> vstr;
    int loaded = 0;
    split(files, vstr, L"|");
    wstring tp;
    for (auto &it : vstr) {
        // 显式加载会按当前文件内容刷新全局缓存，EnablePicCache 只影响 FindPic 的自动缓存。
        if (!Path2GlobalPath(it, _curr_path, tp))
            continue;

        auto image = read_pic_file(tp);
        if (!image)
            continue;

        store_cached_pic(tp, std::move(image));
        loaded++;
    }
    return loaded;
}

long ImageSearchService::FreePic(const wstring &files) {
    std::vector<wstring> vstr;
    int loaded = 0;
    split(files, vstr, L"|");
    wstring tp;
    for (auto &it : vstr) {
        // 先按内存图片名或完整 key 删除。
        if (erase_cached_pic(it)) {
            loaded++;
            continue;
        }

        // 没查到再按资源目录解析后的文件路径删除。
        if (Path2GlobalPath(it, _curr_path, tp) && tp != it && erase_cached_pic(tp)) {
            loaded++;
        }
    }
    return loaded;
}

long ImageSearchService::clear_pic_cache() {
    std::unique_lock<std::shared_mutex> lock(g_pic_cache_mutex);
    g_pic_cache.clear();
    g_pic_match_cache.clear();
    return 1;
}

void ImageSearchService::set_pic_cache_max(long max_count) {
    // max_count<=0 存 0，store_cached_pic 视 0 为不设上限。
    g_pic_cache_max_entries.store(max_count > 0 ? max_count : 0, std::memory_order_relaxed);
}

long ImageSearchService::LoadMemPic(const wstring &file_name, void *data, long size) {
    try {
        if (file_name.empty())
            return 0;

        auto image = read_mem_pic(data, size);
        if (!image)
            return 0;

        // 同名内存图按新内容覆盖，避免全局缓存复用到旧模板。
        return store_cached_pic(file_name, std::move(image)) ? 1 : 0;
    } catch (...) {
        return 0;
    }
}

long ImageSearchService::GetPicSize(const wstring &file_name, long *width, long *height) {
    set_out(width, 0L);
    set_out(height, 0L);

    std::shared_ptr<Image> image;
    std::shared_ptr<PicMatchTemplate> match;
    find_cached_pic(file_name, image, match);
    if (!image) {
        wstring tp;
        if (Path2GlobalPath(file_name, _curr_path, tp))
            find_cached_pic(tp, image, match);
    }

    if (image) {
        if (!set_out(width, image->width) || !set_out(height, image->height))
            return 0;
        return 1;
    }
    return 0;
}

void ImageSearchService::str2binaryfbk(const wstring &color) {
    // sim=1.0 时隐式偏色展开为 0（=000000），与单参版"隐式 df=000000"语义完全一致，
    // 直接复用双参实现，消两份维护。
    str2binaryfbk(color, 1.0);
}

void ImageSearchService::str2binaryfbk(const wstring &color, double sim, int implicit_df_override) {
    vector<color_df_t> colors;
    vector<bool> explicit_dfs;
    if (str2colordfs(color, colors, &explicit_dfs) == 0) {
        // implicit_df_override >= 0 时按调用方指定值（如 OCR 免字库路径的 kOcrImplicitColorDiff），
        // 否则由 sim 推导（ceil((1-sim)*255)）。
        const color_t implicit_df = implicit_df_override >= 0
                                        ? color_t{static_cast<uchar>(implicit_df_override),
                                                  static_cast<uchar>(implicit_df_override),
                                                  static_cast<uchar>(implicit_df_override)}
                                        : sim_to_point_color_diff(sim);
        for (size_t i = 0; i < colors.size() && i < explicit_dfs.size(); ++i) {
            if (!explicit_dfs[i])
                colors[i].df = implicit_df;
        }
        bgr2binary(colors);
    } else {
        bgr2binarybk(colors);
    }
}

void ImageSearchService::str2pointbinaryfbk(const wstring &color) {
    str2binaryfbk(color);
    ApplyBinaryPreprocess();
}

void ImageSearchService::str2pointbinaryfbk(const wstring &color, double sim) {
    str2binaryfbk(color, sim);
    ApplyBinaryPreprocess();
}

void ImageSearchService::ApplyBinaryPreprocess() {
    if (_binary_preprocess_mode <= 0 || _binary.empty())
        return;

    auto count_neighbors = [](const ImageBin &image, int x, int y) {
        int count = 0;
        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                if (dx == 0 && dy == 0)
                    continue;
                const int nx = x + dx;
                const int ny = y + dy;
                if (nx < 0 || ny < 0 || nx >= image.width || ny >= image.height)
                    continue;
                if (image.at(ny, nx) == WORD_COLOR)
                    ++count;
            }
        }
        return count;
    };

    if (_binary_preprocess_mode >= 1) {
        ImageBin source = _binary;
        for (int y = 0; y < source.height; ++y) {
            for (int x = 0; x < source.width; ++x) {
                if (source.at(y, x) == WORD_COLOR &&
                    count_neighbors(source, x, y) <= _binary_isolated_threshold) {
                    _binary.at(y, x) = WORD_BKCOLOR;
                }
            }
        }
    }

    if (_binary_preprocess_mode >= 2 && _binary_min_component_area > 1) {
        ImageBin visited;
        visited.create(_binary.width, _binary.height);
        std::fill(visited.begin(), visited.end(), 0);

        std::vector<point_t> stack;
        std::vector<point_t> component;
        for (int y = 0; y < _binary.height; ++y) {
            for (int x = 0; x < _binary.width; ++x) {
                if (_binary.at(y, x) != WORD_COLOR || visited.at(y, x))
                    continue;

                stack.clear();
                component.clear();
                stack.push_back(point_t(x, y));
                visited.at(y, x) = 1;

                while (!stack.empty()) {
                    const point_t p = stack.back();
                    stack.pop_back();
                    component.push_back(p);

                    for (int dy = -1; dy <= 1; ++dy) {
                        for (int dx = -1; dx <= 1; ++dx) {
                            if (dx == 0 && dy == 0)
                                continue;
                            const int nx = p.x + dx;
                            const int ny = p.y + dy;
                            if (nx < 0 || ny < 0 || nx >= _binary.width || ny >= _binary.height)
                                continue;
                            if (visited.at(ny, nx) || _binary.at(ny, nx) != WORD_COLOR)
                                continue;

                            visited.at(ny, nx) = 1;
                            stack.push_back(point_t(nx, ny));
                        }
                    }
                }

                if (static_cast<long>(component.size()) < _binary_min_component_area) {
                    for (const auto &p : component)
                        _binary.at(p.y, p.x) = WORD_BKCOLOR;
                }
            }
        }
    }

    if (_binary_preprocess_mode >= 3 && _binary_bridge_gap > 0) {
        ImageBin source = _binary;
        for (int y = 0; y < source.height; ++y) {
            for (int x = 0; x < source.width; ++x) {
                if (source.at(y, x) == WORD_COLOR)
                    continue;

                const bool bridge_x = x > 0 && x + 1 < source.width && source.at(y, x - 1) == WORD_COLOR &&
                                      source.at(y, x + 1) == WORD_COLOR;
                const bool bridge_y = y > 0 && y + 1 < source.height && source.at(y - 1, x) == WORD_COLOR &&
                                      source.at(y + 1, x) == WORD_COLOR;
                if (bridge_x || bridge_y)
                    _binary.at(y, x) = WORD_COLOR;
            }
        }
    }
}

void ImageSearchService::files2mats(const wstring &files, std::vector<PicMatchTemplate *> &vmatches,
                                    std::vector<wstring> &vstr, std::vector<std::shared_ptr<Image>> &holders,
                                    std::vector<std::shared_ptr<PicMatchTemplate>> &match_holders) {
    // std::vector<wstring>vstr, vstr2;
    vmatches.clear();
    vstr.clear();
    holders.clear();
    match_holders.clear();
    std::vector<wstring> names;
    split(files, names, L"|");
    wstring tp;
    for (auto &it : names) {
        // 先按原始名字查找，覆盖 LoadMemPic 名称和完整路径两种情况。
        std::shared_ptr<Image> image;
        std::shared_ptr<PicMatchTemplate> match;
        find_cached_pic(it, image, match);
        if (!image) {
            if (!Path2GlobalPath(it, _curr_path, tp)) {
                // 路径解析失败（含 LoadMemPic 名称与磁盘文件都不存在），
                // 静默 continue 会让调用方把 ret=-1 误读为"图上没找到"。
                setlog(L"files2mats: cannot resolve pic path: %s", it.c_str());
                continue;
            }
            find_cached_pic(tp, image, match);
            if (!image) {
                image = read_pic_file(tp);
                if (!image) {
                    setlog(L"files2mats: pic missing or unsupported format: %s", tp.c_str());
                    continue;
                }
                match = make_pic_match(image);
                if (!match) {
                    setlog(L"files2mats: cannot build match template: %s", tp.c_str());
                    continue;
                }
                // 自动读取本地文件时，只有开启缓存才写入全局缓存。
                if (_enable_cache)
                    store_cached_pic(tp, image, match);
            }
        }

        holders.push_back(image);
        match_holders.push_back(match);
        vmatches.push_back(match.get());
        vstr.push_back(it);
    }
}

long ImageSearchService::OcrEx(const wstring &color, double sim, std::wstring &retstr) {
    retstr.clear();
    sim = clamp_sim_default(sim);
    auto dict = ActiveDict(_curr_idx);
    if (!dict) {
        // 免字库路径：直接对整幅区域原图识别，color 不参与（同 OCR()）
        if (_src.width <= 0 || _src.height <= 0)
            return 0;
        vocr_rec_t res;
        int find_ct = 0;
        engine_ocr_filtered(_src.pdata, _src.width, _src.height, sim, res);
        wchar_t coordbuf[64];
        for (auto &it : res) {
            if (find_ct >= _max_return_obj_ct)
                break; // 先判上限再收集（与 ocr_auto_color_ex 一致；置信度已过滤）
            swprintf(coordbuf, 64, L"%d,%d,", it.left_top.x + _x1 + _dx, it.left_top.y + _y1 + _dy);
            retstr += coordbuf;
            retstr += it.text;
            retstr += L"|";
            ++find_ct;
        }
        if (!retstr.empty() && retstr.back() == L'|')
            retstr.pop_back();
        return find_ct;
    } else {
        str2pointbinaryfbk(color, sim);
        return ImageSearchAlgorithms::OcrEx(*dict, sim, retstr);
    }
}

long ImageSearchService::ocr_ex2(const wstring &color, double sim, std::wstring &out_json) {
    out_json.clear();
    sim = clamp_sim_default(sim);
    auto dict = ActiveDict(_curr_idx);
    if (!dict) {
        // 免字库路径：与 OcrEx 同源（原图直识，color 不参与）
        if (_src.width <= 0 || _src.height <= 0)
            return 0;
        vocr_rec_t res;
        vocr_line_ext_t ext;
        engine_ocr_filtered_ext(_src.pdata, _src.width, _src.height, sim, res, ext);
        return build_ocr_json(res, ext, 0, 0, 0, 0, /*bbox_known=*/false, false, out_json);
    }
    // 字库路径：字库匹配无逐字信息，仅输出整行结果（chars 为空数组）
    str2pointbinaryfbk(color, sim);
    std::wstring legacy;
    long ct = ImageSearchAlgorithms::OcrEx(*dict, sim, legacy);
    return legacy_ocr_to_json(legacy, ct, out_json);
}

// 智能选路（2026-11-20）：统计源图中命中指定颜色（含容差）的像素数。
// 用途：决定该走颜色二值化路径还是原图直识路径。
//
// 背景（实测 D:\GMPLUG\DmTools\Dict 8 张小图）：
//   - 坐标类文本（如 "断臂崖[-1219,-1864]"）用红色字 RGB(224,17,17)，
//     原图路径只有 12~24% 准确率（红字+抗锯齿+12~14px 小字），
//     颜色二值化后提到 100%——因为红字被单独抠出、背景噪声被去掉。
//   - 纯汉字名（如 "青螺山割喉者"）用白色字，图里根本没有目标色，
//     此时走颜色路径会得到全背景图、结果恒空（0 行）。
// 故按"图里是否真有这个颜色"选路，实测 16 组全命中「两路取优」的上界
// （平均字符准确率 77.0% → 97.4%）。
//
// 判据说明：
//   - 复用 str2colordfs 解析颜色串（支持 "RRGGBB-DDDDDD" 与 "a|b" 多色并集），
//     与既有颜色路径用同一套解析，口径不会漂移。
//   - 逐通道容差比较（|px - target| <= df），与 bgr2binary 的判定一致。
//   - 空颜色串（"@xxx" 反白格式等无法逐色比对的写法）直接返回 -1 表示"无法探测"，
//     调用方据此保守地仍走颜色路径（保持既有行为）。
long ImageSearchService::count_color_hits(const wstring &color, int &out_total_px) {
    out_total_px = _src.width * _src.height;
    if (color.empty() || _src.width <= 0 || _src.height <= 0) return -1;
    std::vector<color_df_t> colors;
    str2colordfs(color, colors);
    if (colors.empty()) return -1; // 反白等特殊写法：无法逐色探测
    long hits = 0;
    // _src 是 BGRA 32bpp：pdata 每像素 4 字节，字节序 b,g,r,a（见 Color.h 的 color_t 注释）
    const auto *px = reinterpret_cast<const unsigned char *>(_src.pdata);
    const size_t stride = size_t(_src.width) * 4;
    for (int y = 0; y < _src.height; ++y) {
        const unsigned char *row = px + size_t(y) * stride;
        for (int x = 0; x < _src.width; ++x) {
            const unsigned char *p = row + size_t(x) * 4;
            for (const color_df_t &c : colors) {
                const int db = std::abs(int(p[0]) - int(c.color.b));
                const int dg = std::abs(int(p[1]) - int(c.color.g));
                const int dr = std::abs(int(p[2]) - int(c.color.r));
                if (db <= c.df.b && dg <= c.df.g && dr <= c.df.r) {
                    ++hits;
                    break;
                }
            }
        }
    }
    return hits;
}

long ImageSearchService::ocr_auto_color_ex2(const wstring &color, double sim, std::wstring &out_json) {
    out_json.clear();
    // 空颜色 = 不做颜色过滤 -> 走原图直识，与 AutoOcr / AutoOcrLine 的归一语义一致。
    // 空颜色若继续走二值化会产出全背景图、结果恒空（历史 bug #ocr-empty-color，
    // 见 op_c_api.cpp 中 OpAutoOcrLine 的注释）。实测 4.bmp 传空色即因此返回 0 行。
    if (color.empty())
        return ocr_ex2(color, sim, out_json);
    // 智能选路：图里几乎没有目标色时，二值化会得到全背景图、识别恒空。
    // 此时回退原图直识（白字场景的实测准确率是 100%，见上方 count_color_hits 注释）。
    // 阈值 1/1000：小图上 30~400 个彩色字像素约占 0.5%~7%，而纯白字图的命中数是 0。
    int total_px = 0;
    const long hits = count_color_hits(color, total_px);
    if (hits >= 0 && total_px > 0 && hits * 1000 < total_px) {
        return ocr_ex2(color, sim, out_json);
    }
    sim = clamp_sim_default(sim);

    str2binaryfbk(color, 1.0, kOcrImplicitColorDiff);

    auto dict = ActiveDict(_curr_idx);
    if (dict) {
        std::wstring legacy;
        long ct = ImageSearchAlgorithms::OcrEx(*dict, sim, legacy);
        return legacy_ocr_to_json(legacy, ct, out_json);
    }

    int w = _binary.width, h = _binary.height;
    if (w <= 0 || h <= 0)
        return 0;
    std::vector<unsigned char> buf;
    binary_to_white_on_black_bgra(_binary.data(), w, h, buf);
    vocr_rec_t res;
    vocr_line_ext_t ext;
    engine_ocr_filtered_ext(buf.data(), w, h, sim, res, ext);
    // 与 ocr_auto_color_ex 同一阅读序排序，且 ext 同步换位以保持一一对应
    std::vector<size_t> order(res.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&res](size_t a, size_t b) {
        if (res[a].left_top.y != res[b].left_top.y) return res[a].left_top.y < res[b].left_top.y;
        return res[a].left_top.x < res[b].left_top.x;
    });
    vocr_rec_t sorted;
    vocr_line_ext_t sorted_ext;
    sorted.reserve(res.size());
    sorted_ext.reserve(ext.size());
    for (size_t idx : order) {
        sorted.push_back(std::move(res[idx]));
        sorted_ext.push_back(std::move(ext[idx]));
    }
    // 二值化路径的 is_partial 无 det 框贴边语义参考，统一 false；partial_chars 由引擎给。
    // 注意：这里不能传 force_full_width_chars=true——引擎已经给出真实逐字 x，
    // 只有字库路径/旋转降级才需要压平成整行跨度。
    return build_ocr_json(sorted, sorted_ext, _x1 + _dx, _y1 + _dy, w, h, /*bbox_known=*/true,
                          /*force_full_width_chars=*/false, out_json);
}

long ImageSearchService::build_ocr_json(const vocr_rec_t &res, const vocr_line_ext_t &ext, int off_x,
                                        int off_y, int clip_w, int clip_h, bool bbox_known,
                                        bool force_full_width_chars, std::wstring &out_json) {
    using namespace op::json;
    Value root;
    root.type = Type::Array;
    long emitted = 0;

    auto num = [](double v) {
        Value n;
        n.type = Type::Number;
        n.number = v;
        return n;
    };
    auto strv = [](const std::wstring &s) {
        Value v;
        v.type = Type::String;
        v.str = s;
        return v;
    };
    auto boolv = [](bool b) {
        Value v;
        v.type = Type::Bool;
        v.boolean = b;
        return v;
    };
    auto key = [](const wchar_t *k, Value v) { return std::make_pair(std::wstring(k), std::move(v)); };

    for (size_t i = 0; i < res.size(); ++i) {
        if (emitted >= _max_return_obj_ct) break;
        const ocr_rec_t &r = res[i];
        const ocr_line_ext_t empty_ext;
        const ocr_line_ext_t &e = (i < ext.size()) ? ext[i] : empty_ext;

        int x1 = r.left_top.x + off_x;
        int y1 = r.left_top.y + off_y;
        int x2 = r.right_bottom.x + off_x;
        int y2 = r.right_bottom.y + off_y;

        Value line;
        line.type = Type::Object;
        line.obj.push_back(key(L"text", strv(r.text)));
        line.obj.push_back(key(L"conf", num(double(r.confidence))));
        // is_partial：框底贴图边界 <2px。仅在坐标可判（原图路径）时给真值。
        if (e.is_partial) line.obj.push_back(key(L"conf_partial", num(double(r.confidence) * 0.8)));
        line.obj.push_back(key(L"is_partial", boolv(e.is_partial)));
        Value box;
        box.type = Type::Array;
        box.arr.push_back(num(x1));
        box.arr.push_back(num(y1));
        box.arr.push_back(num(x2));
        box.arr.push_back(num(y2));
        line.obj.push_back(key(L"box", std::move(box)));
        line.obj.push_back(key(L"partial_chars", boolv(e.partial_chars)));

        Value chars;
        chars.type = Type::Array;
        for (const ocr_char_t &c : e.chars) {
            Value cv;
            cv.type = Type::Object;
            cv.obj.push_back(key(L"ch", strv(c.ch)));
            cv.obj.push_back(key(L"conf", num(double(c.confidence))));
            // 注意：引擎回的 ocr_char_t.x1/x2 已是**原图绝对坐标**（ocr() 内已加
            // rec.left_top.x 并除回放大），这里只能加区域偏移 off_x，不能再加
            // det 框原点——重复加会导致逐字坐标成倍越界。
            int cx1 = c.x1 + off_x;
            int cx2 = c.x2 + off_x;
            if (force_full_width_chars) { // degraded: whole-line span
                cx1 = x1;
                cx2 = x2;
            }
            if (bbox_known && clip_w > 0) { // clamp into the line box (A1)
                if (cx1 < x1) cx1 = x1;
                if (cx2 > x2) cx2 = x2;
                if (cx2 <= cx1) cx2 = cx1 + 1;
            }
            cv.obj.push_back(key(L"x1", num(cx1)));
            cv.obj.push_back(key(L"x2", num(cx2)));
            chars.arr.push_back(std::move(cv));
        }
        line.obj.push_back(key(L"chars", std::move(chars)));

        root.arr.push_back(std::move(line));
        ++emitted;
    }
    out_json = op::json::Stringify(root);
    return emitted;
}

long ImageSearchService::legacy_ocr_to_json(const std::wstring &legacy, long count, std::wstring &out_json) {
    using namespace op::json;
    Value root;
    root.type = Type::Array;
    if (!legacy.empty()) {
        std::vector<std::wstring> items;
        split(legacy, items, L"|");
        for (const std::wstring &item : items) {
            if (item.empty()) continue;
            // 字库路径格式 "x,y,text"（OcrEx）：拆出坐标与文本，无逐字信息
            Value line;
            line.type = Type::Object;
            std::wstring text = item;
            int x1 = 0, y1 = 0;
            size_t c1 = item.find(L',');
            size_t c2 = (c1 == std::wstring::npos) ? std::wstring::npos : item.find(L',', c1 + 1);
            if (c1 != std::wstring::npos && c2 != std::wstring::npos) {
                x1 = _wtoi(item.substr(0, c1).c_str());
                y1 = _wtoi(item.substr(c1 + 1, c2 - c1 - 1).c_str());
                text = item.substr(c2 + 1);
            }
            Value textv;
            textv.type = Type::String;
            textv.str = text;
            line.obj.push_back(std::make_pair(std::wstring(L"text"), std::move(textv)));
            Value confv;
            confv.type = Type::Number;
            confv.number = 1.0; // 字库匹配为精确匹配，无置信度概念
            line.obj.push_back(std::make_pair(std::wstring(L"conf"), std::move(confv)));
            Value isp;
            isp.type = Type::Bool;
            isp.boolean = false;
            line.obj.push_back(std::make_pair(std::wstring(L"is_partial"), std::move(isp)));
            Value box;
            box.type = Type::Array;
            Value vx, vy;
            vx.type = Type::Number;
            vx.number = x1;
            vy.type = Type::Number;
            vy.number = y1;
            box.arr.push_back(std::move(vx));
            box.arr.push_back(std::move(vy));
            line.obj.push_back(std::make_pair(std::wstring(L"box"), std::move(box)));
            Value pc;
            pc.type = Type::Bool;
            pc.boolean = true; // 字库路径无逐字坐标
            line.obj.push_back(std::make_pair(std::wstring(L"partial_chars"), std::move(pc)));
            Value chars;
            chars.type = Type::Array;
            line.obj.push_back(std::make_pair(std::wstring(L"chars"), std::move(chars)));
            root.arr.push_back(std::move(line));
        }
    }
    out_json = op::json::Stringify(root);
    return count;
}

// 上一次 OCR 的分项耗时（P3 2026-11-21）。
// 直接从引擎读，**不触发**新的 OCR：连续读两次 profile 不会改变数值。
// valid=false 时其余字段全 0，调用方据此判断"还没跑过 OCR / 引擎不支持"。
long ImageSearchService::ocr_last_profile(std::wstring &out_json) {
    using namespace op::json;
    out_json.clear();
    const op::ocr::ocr_profile_t p = HttpOcrService::getInstance()->last_profile();

    auto num = [](double d) {
        Value n;
        n.type = Type::Number;
        n.number = d;
        return n;
    };
    auto boolv = [](bool b) {
        Value v;
        v.type = Type::Bool;
        v.boolean = b;
        return v;
    };
    auto key = [](const wchar_t *k, Value v) { return std::make_pair(std::wstring(k), std::move(v)); };

    Value root;
    root.type = Type::Object;
    root.obj.push_back(key(L"valid", boolv(p.valid)));
    root.obj.push_back(key(L"total_ms", num(p.total_ms)));
    root.obj.push_back(key(L"pre_ms", num(p.pre_ms)));
    root.obj.push_back(key(L"det_ms", num(p.det_ms)));
    root.obj.push_back(key(L"cls_ms", num(p.cls_ms)));
    root.obj.push_back(key(L"rec_ms", num(p.rec_ms)));
    root.obj.push_back(key(L"post_ms", num(p.post_ms)));
    root.obj.push_back(key(L"boxes", num(double(p.boxes))));
    root.obj.push_back(key(L"lines", num(double(p.lines))));
    root.obj.push_back(key(L"chars", num(double(p.chars))));
    root.obj.push_back(key(L"det_calls", num(double(p.det_calls))));
    root.obj.push_back(key(L"rec_calls", num(double(p.rec_calls))));
    root.obj.push_back(key(L"cls_calls", num(double(p.cls_calls))));
    root.obj.push_back(key(L"runs_failed", num(double(p.runs_failed))));
    root.obj.push_back(key(L"upscale", boolv(p.upscale)));
    root.obj.push_back(key(L"oscale", num(double(p.oscale))));
    root.obj.push_back(key(L"rec_width", num(double(p.rec_width))));
    out_json = op::json::Stringify(root);
    return p.valid ? 1 : 0;
}

long ImageSearchService::FindStr(const wstring &str, const wstring &color, double sim, long &retx, long &rety) {
    vector<wstring> vstr;
    split(str, vstr, L"|");
    sim = clamp_sim_exact(sim);
    std::map<point_t, ocr_rec_t> ocr_res;
    auto dict = ActiveDict(_curr_idx);
    if (!dict) {
        vocr_rec_t res;
        engine_ocr_filtered(_src.pdata, _src.width, _src.height, sim, res);
        for (auto &it : res) {
            ocr_res[it.left_top] = it;
        }
    } else {
        str2pointbinaryfbk(color, sim);
        ImageSearchAlgorithms::bin_ocr(*dict, sim, ocr_res);
    }
    return ImageSearchAlgorithms::FindStr(ocr_res, vstr, retx, rety);
}

long ImageSearchService::FindStrEx(const wstring &str, const wstring &color, double sim, std::wstring &out_str) {
    out_str.clear();
    vector<wstring> vstr;
    split(str, vstr, L"|");
    sim = clamp_sim_exact(sim);
    std::map<point_t, ocr_rec_t> ocr_res;
    auto dict = ActiveDict(_curr_idx);
    if (!dict) {
        vocr_rec_t res;
        engine_ocr_filtered(_src.pdata, _src.width, _src.height, sim, res);
        for (auto &it : res) {
            ocr_res[it.left_top] = it;
        }
    } else {
        str2pointbinaryfbk(color, sim);
        ImageSearchAlgorithms::bin_ocr(*dict, sim, ocr_res);
    }
    return ImageSearchAlgorithms::FindStrEx(ocr_res, vstr, out_str);
}

// ============================================================================
// P1（2026-11-21）FindStr 增强：多目标结构化查找 + 字符级精确范围
// ============================================================================
//
// 与既有 FindStr/FindStrEx 的关系：**不改动旧路径一行代码**，新接口共享独立内核。
//
// 旧实现的三个缺口（详见 docs/2026-11/P1_FindStr增强与P3耗时自省_实施方案.md §1.2）：
//   a) 纯子串包含：找 "909" 会命中 "1909"/"9090"（游戏读数字场景误报高）
//   b) 只能返回整行 left_top：拿不到被找子串自己的框
//   c) 无正则、无形近容错
//
// 本实现用 P0 产出的逐字坐标把命中定位到字符级，这是"纯匹配层零新增推理成本"
// 之外的新增价值点。

// P1 辅助：把 OCR 命中块按"阅读顺序"展开（行聚类 -> 行内按 x 升序）。
// ImageSearchAlgorithms.cpp 里的同名逻辑在匿名 namespace 内、本 TU 不可见，
// 这里按**完全相同的口径**重写一份（纵向重叠 ≥1px 即并入同一行，行下边缘取
// 成员最大值）。口径必须一致，否则同一张图上 FindStr 与 FindStrBatch 的行序
// 会不同，调用方拿两者的 line 下标互相换算就会出错。
std::vector<std::pair<point_t, ocr_rec_t>>
reading_order_items(const std::map<point_t, ocr_rec_t> &ps, std::map<point_t, size_t> *index_map) {
    std::vector<std::pair<point_t, ocr_rec_t>> items;
    items.reserve(ps.size());
    if (index_map)
        index_map->clear();
    for (const auto &kv : ps) {
        if (index_map)
            (*index_map)[kv.first] = items.size();
        items.emplace_back(kv.first, kv.second);
    }
    if (items.size() <= 1)
        return items;

    std::sort(items.begin(), items.end(), [](const auto &a, const auto &b) {
        const int ay = a.second.left_top.y, by = b.second.left_top.y;
        return ay != by ? ay < by : a.first.x < b.first.x;
    });

    struct line_t {
        int bottom = 0;
        std::vector<size_t> idx;
    };
    std::vector<line_t> lines;
    for (size_t i = 0; i < items.size(); ++i) {
        const int top = items[i].second.left_top.y;
        int bottom = items[i].second.right_bottom.y;
        if (bottom <= top)
            bottom = top + 1; // 退化包围盒按 1px 高处理
        if (!lines.empty() && top < lines.back().bottom) {
            lines.back().idx.push_back(i);
            if (bottom > lines.back().bottom)
                lines.back().bottom = bottom;
        } else {
            line_t ln;
            ln.bottom = bottom;
            ln.idx.push_back(i);
            lines.push_back(std::move(ln));
        }
    }

    std::vector<std::pair<point_t, ocr_rec_t>> out;
    out.reserve(items.size());
    for (auto &ln : lines) {
        std::sort(ln.idx.begin(), ln.idx.end(),
                  [&items](size_t a, size_t b) { return items[a].first.x < items[b].first.x; });
        for (const size_t i : ln.idx)
            out.push_back(std::move(items[i]));
    }
    return out;
}

bool ImageSearchService::collect_ocr_lines(const wstring &color, double sim,
                                           std::vector<ocr_line_view_t> &lines) {
    lines.clear();
    sim = clamp_sim_exact(sim);

    auto dict = ActiveDict(_curr_idx);
    if (!dict) {
        // 免字库路径：与 FindStr 同源，原图直识（逐字可用）。
        if (_src.width <= 0 || _src.height <= 0)
            return false;
        vocr_rec_t res;
        vocr_line_ext_t ext;
        engine_ocr_filtered_ext(_src.pdata, _src.width, _src.height, sim, res, ext);

        // 逐字数据按 left_top 建索引：ocr_res 以 left_top 为 key（与既有 FindStrEx
        // 一致），这里额外记住 first_index 以便把 ext 的逐字信息对上号。
        std::map<point_t, ocr_rec_t> keyed;
        std::map<point_t, size_t> first_index;
        for (size_t i = 0; i < res.size(); ++i) {
            if (keyed.find(res[i].left_top) == keyed.end()) {
                first_index[res[i].left_top] = i;
                keyed[res[i].left_top] = res[i];
            }
        }
        const auto ordered = reading_order_items(keyed, nullptr);

        const int off_x = _x1 + _dx;
        const int off_y = _y1 + _dy;
        for (const auto &it : ordered) {
            if (it.second.text.empty())
                continue;
            const size_t idx = first_index[it.first];
            ocr_line_view_t v;
            v.text = it.second.text;
            v.x1 = it.second.left_top.x + off_x;
            v.y1 = it.second.left_top.y + off_y;
            v.x2 = it.second.right_bottom.x + off_x;
            v.y2 = it.second.right_bottom.y + off_y;
            v.partial = (idx < ext.size()) ? ext[idx].is_partial : false;
            // 逐字数据可用性：非旋转降级，且逐字条目数与行文本长度一致。
            // 不一致说明有字符没产出跨度（模型异常/字典空串残留），此时宁可退化为
            // 整行范围，也不给可能错位的字符级坐标（P0 的 A3 契约在此处的延伸）。
            const bool usable = idx < ext.size() && !ext[idx].partial_chars &&
                                ext[idx].chars.size() == v.text.size();
            v.chars_usable = usable;
            if (usable) {
                v.cx1.resize(v.text.size());
                v.cx2.resize(v.text.size());
                for (size_t c = 0; c < v.text.size(); ++c) {
                    // 逐字 x 已是**原图绝对坐标**：只能加区域偏移 off_x，
                    // 绝不能再加 det 框原点（P0 修过的坐标成倍越界 bug）。
                    v.cx1[c] = ext[idx].chars[c].x1 + off_x;
                    v.cx2[c] = ext[idx].chars[c].x2 + off_x;
                }
            }
            lines.push_back(std::move(v));
        }
        return true;
    }

    // 字库路径：逐字信息不存在（chars 为空数组），cx1/cx2 后续退化为行范围。
    str2pointbinaryfbk(color, sim);
    std::map<point_t, ocr_rec_t> ocr_res;
    ImageSearchAlgorithms::bin_ocr(*dict, sim, ocr_res);

    const int off_x = _x1 + _dx;
    const int off_y = _y1 + _dy;
    for (const auto &it : reading_order_items(ocr_res, nullptr)) {
        if (it.second.text.empty())
            continue;
        ocr_line_view_t v;
        v.text = it.second.text;
        v.x1 = it.second.left_top.x + off_x;
        v.y1 = it.second.left_top.y + off_y;
        v.x2 = it.second.right_bottom.x + off_x;
        v.y2 = it.second.right_bottom.y + off_y;
        v.partial = false;
        v.chars_usable = false;
        lines.push_back(std::move(v));
    }
    return true;
}

long ImageSearchService::match_targets(const std::vector<op::str_match::Target> &targets, const wstring &color,
                                       double sim, std::wstring &out_json) {
    using namespace op::json;
    out_json.clear();

    std::vector<ocr_line_view_t> lines;
    if (!collect_ocr_lines(color, sim, lines))
        return 0;

    auto num = [](double d) {
        Value n;
        n.type = Type::Number;
        n.number = d;
        return n;
    };
    auto strv = [](const std::wstring &s) {
        Value v;
        v.type = Type::String;
        v.str = s;
        return v;
    };
    auto boolv = [](bool b) {
        Value v;
        v.type = Type::Bool;
        v.boolean = b;
        return v;
    };
    auto key = [](const wchar_t *k, Value v) { return std::make_pair(std::wstring(k), std::move(v)); };

    static const wchar_t *kModeNames[] = {L"contains", L"exact", L"prefix", L"suffix", L"regex"};

    Value root;
    root.type = Type::Object;

    Value results;
    results.type = Type::Array;
    long found_targets = 0;
    long total_hits = 0;

    // 行文本只取一次，供每个目标复用（避免 N 目标 × M 行重复拷贝）
    std::vector<std::wstring> line_texts;
    line_texts.reserve(lines.size());
    for (const auto &ln : lines)
        line_texts.push_back(ln.text);

    for (const op::str_match::Target &t : targets) {
        std::vector<op::str_match::Hit> hits;
        std::vector<op::str_match::Hit> raw;
        const bool ok = op::str_match::MatchAll(line_texts, t, raw);

        // confirmed_only：剔掉贴边行（其文本可能没读全）
        if (t.confirmed_only) {
            for (const auto &h : raw) {
                if (h.line >= 0 && size_t(h.line) < lines.size() && !lines[size_t(h.line)].partial)
                    hits.push_back(h);
            }
        } else {
            hits = raw;
        }

        // 总上限：与 FindStrEx 一致地用 _max_return_obj_ct 兜底，防止超大图 ×
        // 单字符目标产出几十万条命中把 JSON 撑爆。
        if (_max_return_obj_ct > 0 && long(hits.size()) > _max_return_obj_ct)
            hits.resize(size_t(_max_return_obj_ct));

        Value item;
        item.type = Type::Object;
        item.obj.push_back(key(L"id", strv(t.id)));
        item.obj.push_back(key(L"text", strv(t.text)));
        item.obj.push_back(key(L"match", strv(kModeNames[static_cast<int>(t.mode)])));
        item.obj.push_back(key(L"found", boolv(ok && !hits.empty())));
        item.obj.push_back(key(L"count", num(double(hits.size()))));
        if (!ok)
            item.obj.push_back(key(L"error", strv(L"invalid configuration or bad regex")));

        Value harr;
        harr.type = Type::Array;
        for (const auto &h : hits) {
            const ocr_line_view_t &ln = lines[size_t(h.line)];
            // 命中片段的字符级水平范围。逐字不可用时退化为整行（px1/px2 语义）。
            int cx1 = ln.x1, cx2 = ln.x2;
            bool char_precise = false;
            if (ln.chars_usable && h.end > h.begin && size_t(h.end) <= ln.cx1.size() &&
                size_t(h.begin) < ln.cx2.size()) {
                cx1 = ln.cx1[size_t(h.begin)];
                cx2 = ln.cx2[size_t(h.end) - 1];
                // 保证不越出行框（A1 契约的延伸），且区间非空
                if (cx1 < ln.x1) cx1 = ln.x1;
                if (cx2 > ln.x2) cx2 = ln.x2;
                if (cx2 <= cx1) cx2 = (std::min)(ln.x2, cx1 + 1);
                char_precise = true;
            }

            Value hv;
            hv.type = Type::Object;
            hv.obj.push_back(key(L"line", num(double(h.line))));
            // x/y 与旧 FindStr/FindStrEx 的坐标语义保持一致：**行框左上角**
            // （旧接口返回的就是 span->point，即该行 left_top），这样新旧结果
            // 可以直接互相校验。命中片段自己的水平范围看 cx1/cx2。
            hv.obj.push_back(key(L"x", num(ln.x1)));
            hv.obj.push_back(key(L"y", num(ln.y1)));
            hv.obj.push_back(key(L"x2", num(ln.x2)));
            hv.obj.push_back(key(L"y2", num(ln.y2)));
            hv.obj.push_back(key(L"cx1", num(cx1)));
            hv.obj.push_back(key(L"cx2", num(cx2)));
            hv.obj.push_back(key(L"char_precise", boolv(char_precise)));
            hv.obj.push_back(key(L"ed", num(double(h.ed))));
            hv.obj.push_back(key(L"exact", boolv(h.ed == 0)));
            hv.obj.push_back(key(L"partial", boolv(ln.partial)));
            hv.obj.push_back(key(L"begin", num(double(h.begin))));
            hv.obj.push_back(key(L"end", num(double(h.end))));
            hv.obj.push_back(key(L"line_text", strv(ln.text)));
            harr.arr.push_back(std::move(hv));
        }
        item.obj.push_back(key(L"hits", std::move(harr)));
        if (ok && !hits.empty())
            ++found_targets;
        total_hits += long(hits.size());
        results.arr.push_back(std::move(item));
    }

    root.obj.push_back(key(L"targets", num(double(targets.size()))));
    root.obj.push_back(key(L"found", num(double(found_targets))));
    root.obj.push_back(key(L"hits", num(double(total_hits))));
    root.obj.push_back(key(L"lines", num(double(lines.size()))));
    root.obj.push_back(key(L"results", std::move(results)));
    out_json = op::json::Stringify(root);
    return found_targets;
}

long ImageSearchService::find_str_batch(const std::wstring &json_targets, const wstring &color, double sim,
                                        std::wstring &out_json) {
    out_json.clear();
    op::json::Value root;
    if (!op::json::Parse(json_targets, root) || root.type != op::json::Type::Array)
        return -1;

    std::vector<op::str_match::Target> targets;
    targets.reserve(root.arr.size());
    for (const op::json::Value &item : root.arr) {
        op::str_match::Target t;
        if (item.type == op::json::Type::String) {
            // 裸字符串 = 默认配置的 contains 目标，零学习成本
            t.text = item.str;
            t.id = std::to_wstring(targets.size());
        } else if (item.type == op::json::Type::Object) {
            bool has_text = false;
            for (const auto &kv : item.obj) {
                const std::wstring &k = kv.first;
                const op::json::Value &v = kv.second;
                if (k == L"text") {
                    if (v.type != op::json::Type::String) return -1;
                    t.text = v.str;
                    has_text = true;
                } else if (k == L"id") {
                    if (v.type == op::json::Type::String)
                        t.id = v.str;
                    else if (v.type == op::json::Type::Number)
                        t.id = std::to_wstring(long(v.number));
                    else
                        return -1;
                } else if (k == L"match") {
                    if (v.type != op::json::Type::String) return -1;
                    if (v.str == L"contains") t.mode = op::str_match::MatchMode::Contains;
                    else if (v.str == L"exact") t.mode = op::str_match::MatchMode::Exact;
                    else if (v.str == L"prefix") t.mode = op::str_match::MatchMode::Prefix;
                    else if (v.str == L"suffix") t.mode = op::str_match::MatchMode::Suffix;
                    else if (v.str == L"regex") t.mode = op::str_match::MatchMode::Regex;
                    else return -1; // 未知模式：显式报错，不静默当 contains
                } else if (k == L"fuzzy") {
                    if (v.type != op::json::Type::Number) return -1;
                    t.fuzzy = int(v.number);
                } else if (k == L"max_hits") {
                    if (v.type != op::json::Type::Number) return -1;
                    t.max_hits = int(v.number);
                } else if (k == L"ci") {
                    if (v.type != op::json::Type::Bool) return -1;
                    t.ci = v.boolean;
                } else if (k == L"confirmed_only") {
                    if (v.type != op::json::Type::Bool) return -1;
                    t.confirmed_only = v.boolean;
                } else {
                    return -1; // 未知字段：显式报错，避免拼写错误静默失效
                }
            }
            if (!has_text)
                return -1;
            if (t.id.empty())
                t.id = std::to_wstring(targets.size());
        } else {
            return -1; // 元素只能是 string 或 object
        }
        if (op::str_match::ValidateTarget(t).bad)
            return -1;
        targets.push_back(std::move(t));
    }
    // 空数组是**合法**输入（调用方可能动态拼目标列表）：返回
    // {"targets":0,"found":0,"hits":0,"lines":N,"results":[]} 而不是 nullptr，
    // 这样"没目标"与"输入非法"在调用方看来是可区分的两种状态。
    match_targets(targets, color, sim, out_json);
    return long(targets.size());
}

long ImageSearchService::find_str_ex2(const std::wstring &pattern, long flags, const wstring &color, double sim,
                                      std::wstring &out_json) {
    out_json.clear();
    op::str_match::Target t;
    if (!op::str_match::TargetFromFlags(pattern, flags, t))
        return -1;
    if (op::str_match::ValidateTarget(t).bad)
        return -1;
    t.id = L"0";
    std::vector<op::str_match::Target> targets;
    targets.push_back(std::move(t));
    return match_targets(targets, color, sim, out_json);
}

long ImageSearchService::OcrAuto(double sim, std::wstring &retstr) {
    // Auto 系（无 color 参数）语义 = **免字库**（大漠同名接口同义）。
    // 不能复用 OCR(L"")：字库路径要用 color 做二值化，而这里拿不到 color，
    // 空串 → str2colordfs 得 0 个颜色 → bgr2binary 产出**全背景**的 _binary
    // ⇒ 一旦加载过字库就恒返回空串（静默失败，实测 2026-09-30：同区域
    // ocr(color='000000')='AB12' 而 ocr_auto()=''）。
    // 无字库时两条路径等价（都走 OcrFree），故此改动不改变既有用例行为。
    return OcrFree(sim, retstr);
}

long ImageSearchService::OcrFromFile(const wstring &files, const wstring &color, double sim, std::wstring &retstr) {
    retstr.clear();
    sim = clamp_sim_default(sim);
    wstring fullpath;
    if (Path2GlobalPath(files, _curr_path, fullpath)) {
        _src.read(fullpath.data());
        return OCR(color, sim, retstr);
    }
    return 0;
}

long ImageSearchService::OcrAutoFromFile(const wstring &files, double sim, std::wstring &retstr) {
    retstr.clear();
    wstring fullpath;
    if (!Path2GlobalPath(files, _curr_path, fullpath))
        return 0;
    _src.read(fullpath.data());
    return OcrFree(sim, retstr); // 同 OcrAuto：Auto 系 = 免字库，不走字库
}

long ImageSearchService::ocr_auto_color_from_file(const wstring &files, const wstring &color, double sim, std::wstring &retstr) {
    retstr.clear();
    sim = clamp_sim_default(sim);
    wstring fullpath;
    if (Path2GlobalPath(files, _curr_path, fullpath)) {
        _src.read(fullpath.data());
        return ocr_auto_color(color, sim, retstr); // 复用真实 ocr_auto_color：颜色二值化后再免字库 OCR
    }
    return 0;
}

long ImageSearchService::FindLine(const wstring &color, double sim, wstring &retStr) {
    retStr.clear();
    sim = clamp_sim_exact(sim);
    str2binaryfbk(color, sim);
    return ImageSearchAlgorithms::FindLine(retStr);
}

long ImageSearchService::FindLineExS(const wstring &color, double sim, long min_points, wstring &retStr) {
    retStr.clear();
    sim = clamp_sim_exact(sim);
    // 走 point 链路：二值化后应用孤立点去噪（与 OCR 链路同一套 _binary_preprocess 配置）
    str2pointbinaryfbk(color, sim);
    const long peak = ImageSearchAlgorithms::FindLine(retStr);
    if (peak < min_points)
        retStr.clear(); // 未达阈值：视为无可靠直线，防止幻觉线
    return peak < 0 ? 0 : peak;
}

} // namespace op::image
