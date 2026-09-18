// #include "stdafx.h"
#include "ImageSearchService.h"
#include "../base/Utils.h"
#include "../ocr/OcrService.h"
#include <algorithm>
#include <array>
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
    vector<PicMatchTemplate *> vmatches;
    // 算法层沿用裸指针入参，这里用 holders 保证匹配期间图片对象仍然存活。
    vector<std::shared_ptr<Image>> holders;
    vector<std::shared_ptr<PicMatchTemplate>> match_holders;
    color_t dfcolor;
    vector<std::wstring> vpic_name;
    files2mats(files, vmatches, vpic_name, holders, match_holders);
    dfcolor.str2color(delta_colors);
    sim = 0.5 + sim / 2;
    long ret = ImageSearchAlgorithms::FindPicTh(vmatches, dfcolor, sim, dir, x, y);
    return ret;
}
//
long ImageSearchService::FindPicEx(const std::wstring &files, const wstring &delta_colors, double sim, long dir, wstring &retstr,
                          bool returnID) {
    vector<PicMatchTemplate *> vmatches;
    // 算法层沿用裸指针入参，这里用 holders 保证匹配期间图片对象仍然存活。
    vector<std::shared_ptr<Image>> holders;
    vector<std::shared_ptr<PicMatchTemplate>> match_holders;
    vpoint_desc_t vpd;
    color_t dfcolor;
    vector<std::wstring> vpic_name;
    files2mats(files, vmatches, vpic_name, holders, match_holders);
    dfcolor.str2color(delta_colors);
    sim = 0.5 + sim / 2;
    long ret = ImageSearchAlgorithms::FindPicExTh(vmatches, dfcolor, sim, dir, vpd);
    std::wstringstream ss(std::wstringstream::in | std::wstringstream::out);
    if (returnID) {
        for (auto &it : vpd) {
            ss << it.id << L"," << it.pos << L"|";
        }
    } else {
        for (auto &it : vpd) {
            ss << vpic_name[it.id] << L"," << it.pos << L"|";
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

long ImageSearchService::OCR(const wstring &color, double sim, std::wstring &out_str) {
    out_str.clear();
    if (sim < 0. || sim > 1.)
        sim = 0.7;  // 默认置信度阈值：免字库 onnx 输出 conf≈0.85+，1.0 会全滤掉
    long s = 0;
    auto dict = ActiveDict(_curr_idx);
    if (!dict) {
        vocr_rec_t res;
        HttpOcrService::getInstance()->ocr(_src.pdata, _src.width, _src.height, 4, res);
        for (auto &it : res) {
            if (it.confidence >= sim - 1e-9) {
                out_str += it.text;
            }
        }
    } else {
        str2pointbinaryfbk(color, sim);
        s = ImageSearchAlgorithms::Ocr(*dict, sim, out_str);
    }

    return s;
}

long ImageSearchService::autoocr(const wstring &color, double sim, wstring &out_str) {
    out_str.clear();
    if (sim < 0. || sim > 1.)
        sim = 0.7; // 默认置信度阈值：免字库 onnx 输出 conf≈0.85+，1.0 会全滤掉

    // 1) 按颜色二值化 -> _binary（WORD_COLOR=1 命中文字，WORD_BKCOLOR=0 背景）。
    //    用 str2binaryfbk 而非裸 str2colordfs+bgr2binary：它支持 "@背景色" 格式
    //    （分流到 bgr2binarybk，反白字/白字深底场景），与字库制作同一入口。
    str2binaryfbk(color);

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
    HttpOcrService::getInstance()->ocr(buf.data(), w, h, 4, res);
    for (auto &it : res) {
        if (it.confidence >= sim - 1e-9)
            out_str += it.text;
    }
    return 0;
}

long ImageSearchService::autoocr_line(const wstring &color, double sim, wstring &out_str) {
    out_str.clear();
    if (sim < 0. || sim > 1.)
        sim = 0.7;

    // 1) 按颜色二值化 -> _binary（命中文字=1，背景=0）。
    //    str2binaryfbk：支持 "@背景色" 反白格式，与字库制作同一入口。
    str2binaryfbk(color);

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
        for (auto &it : res) {
            if (it.confidence >= sim - 1e-9)
                out_str += it.text;
        }
    }
    return 0;
}

long ImageSearchService::autoocr_ex(const wstring &color, double sim, wstring &out_str) {
    out_str.clear();
    if (sim < 0. || sim > 1.)
        sim = 0.7;

    // 1) 按颜色二值化 -> _binary。
    //    str2binaryfbk：支持 "@背景色" 反白格式，与字库制作同一入口。
    str2binaryfbk(color);

    auto dict = ActiveDict(_curr_idx);
    if (dict) {
        // 字库兜底：沿用 OcrEx 的 "x,y,text|..." 格式
        return ImageSearchAlgorithms::OcrEx(*dict, sim, out_str);
    }

    // 2) 免字库：白字黑底 BGRA 缓冲 -> det+rec，bbox 偏移为屏幕绝对坐标
    int w = _binary.width, h = _binary.height;
    if (w <= 0 || h <= 0)
        return 0;
    std::vector<unsigned char> buf;
    binary_to_white_on_black_bgra(_binary.data(), w, h, buf);
    vocr_rec_t res;
    HttpOcrService::getInstance()->ocr(buf.data(), w, h, 4, res);
    // det 结果按阅读序稳定排序（上→下、同行左→右），输出顺序确定
    std::stable_sort(res.begin(), res.end(), [](const auto &a, const auto &b) {
        if (a.left_top.y != b.left_top.y)
            return a.left_top.y < b.left_top.y;
        return a.left_top.x < b.left_top.x;
    });
    long find_ct = 0;
    wchar_t confbuf[16];
    for (auto &it : res) {
        if (it.confidence < sim - 1e-9)
            continue;
        if (find_ct >= _max_return_obj_ct)
            break; // 先判上限再收集（修正 off-by-one）
        swprintf(confbuf, 16, L"%.2f", it.confidence);
        out_str += std::to_wstring(it.left_top.x + _x1 + _dx);
        out_str += L",";
        out_str += std::to_wstring(it.left_top.y + _y1 + _dy);
        out_str += L",";
        out_str += std::to_wstring(it.right_bottom.x + _x1 + _dx);
        out_str += L",";
        out_str += std::to_wstring(it.right_bottom.y + _y1 + _dy);
        out_str += L",";
        out_str += confbuf;
        out_str += L",";
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
        cr.color.str2color(vstr2[0]);
        const bool has_explicit_df = vstr2.size() == 2;
        cr.df.str2color(has_explicit_df ? vstr2[1] : L"000000");
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
        cr.str2color(it);
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
    vector<color_df_t> colors;
    if (str2colordfs(color, colors) == 0) {
        bgr2binary(colors);
    } else {
        bgr2binarybk(colors);
    }
}

void ImageSearchService::str2binaryfbk(const wstring &color, double sim) {
    vector<color_df_t> colors;
    vector<bool> explicit_dfs;
    if (str2colordfs(color, colors, &explicit_dfs) == 0) {
        const auto implicit_df = sim_to_point_color_diff(sim);
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
    if (sim < 0. || sim > 1.)
        sim = 0.7;
    auto dict = ActiveDict(_curr_idx);
    if (!dict) {
        vocr_rec_t res;
        int find_ct = 0;
        HttpOcrService::getInstance()->ocr(_src.pdata, _src.width, _src.height, 4, res);
        for (auto &it : res) {
            if (it.confidence >= sim - 1e-9) {
                retstr += std::to_wstring(it.left_top.x + _x1 + _dx);
                retstr += L",";
                retstr += std::to_wstring(it.left_top.y + _y1 + _dy);
                retstr += L",";
                retstr += it.text;
                retstr += L"|";
                ++find_ct;
                if (find_ct > _max_return_obj_ct)
                    break;
            }
        }
        if (!retstr.empty() && retstr.back() == L'|')
            retstr.pop_back();
        return find_ct;
    } else {
        str2pointbinaryfbk(color, sim);
        return ImageSearchAlgorithms::OcrEx(*dict, sim, retstr);
    }
}

long ImageSearchService::FindStr(const wstring &str, const wstring &color, double sim, long &retx, long &rety) {
    vector<wstring> vstr;
    split(str, vstr, L"|");
    if (sim < 0. || sim > 1.)
        sim = 1.;
    std::map<point_t, ocr_rec_t> ocr_res;
    auto dict = ActiveDict(_curr_idx);
    if (!dict) {
        vocr_rec_t res;
        HttpOcrService::getInstance()->ocr(_src.pdata, _src.width, _src.height, 4, res);
        for (auto &it : res) {
            if (it.confidence >= sim - 1e-9) {
                ocr_res[it.left_top] = it;
            }
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
    if (sim < 0. || sim > 1.)
        sim = 1.;
    std::map<point_t, ocr_rec_t> ocr_res;
    auto dict = ActiveDict(_curr_idx);
    if (!dict) {
        vocr_rec_t res;
        HttpOcrService::getInstance()->ocr(_src.pdata, _src.width, _src.height, 4, res);
        for (auto &it : res) {
            if (it.confidence >= sim - 1e-9) {
                ocr_res[it.left_top] = it;
            }
        }
    } else {
        str2pointbinaryfbk(color, sim);
        ImageSearchAlgorithms::bin_ocr(*dict, sim, ocr_res);
    }
    return ImageSearchAlgorithms::FindStrEx(ocr_res, vstr, out_str);
}

long ImageSearchService::OcrAuto(double sim, std::wstring &retstr) {
    return OCR(L"", sim, retstr);
}

long ImageSearchService::OcrFromFile(const wstring &files, const wstring &color, double sim, std::wstring &retstr) {
    retstr.clear();
    if (sim < 0. || sim > 1.)
        sim = 0.7;
    wstring fullpath;
    if (Path2GlobalPath(files, _curr_path, fullpath)) {
        _src.read(fullpath.data());
        return OCR(color, sim, retstr);
    }
    return 0;
}

long ImageSearchService::OcrAutoFromFile(const wstring &files, double sim, std::wstring &retstr) {
    retstr.clear();
    if (sim < 0. || sim > 1.)
        sim = 0.7;
    wstring fullpath;

    if (Path2GlobalPath(files, _curr_path, fullpath)) {
        _src.read(fullpath.data());
        return OCR(L"", sim, retstr);
    }
    return 0;
}

long ImageSearchService::autoocrFromFile(const wstring &files, const wstring &color, double sim, std::wstring &retstr) {
    retstr.clear();
    if (sim < 0. || sim > 1.)
        sim = 0.7;
    wstring fullpath;
    if (Path2GlobalPath(files, _curr_path, fullpath)) {
        _src.read(fullpath.data());
        return autoocr(color, sim, retstr); // 复用真实 autoocr：颜色二值化后再免字库 OCR
    }
    return 0;
}

long ImageSearchService::FindLine(const wstring &color, double sim, wstring &retStr) {
    retStr.clear();
    if (sim < 0. || sim > 1.)
        sim = 1.;
    str2binaryfbk(color, sim);
    return ImageSearchAlgorithms::FindLine(retStr);
}

long ImageSearchService::FindLineExS(const wstring &color, double sim, long min_points, wstring &retStr) {
    retStr.clear();
    if (sim < 0. || sim > 1.)
        sim = 1.;
    // 走 point 链路：二值化后应用孤立点去噪（与 OCR 链路同一套 _binary_preprocess 配置）
    str2pointbinaryfbk(color, sim);
    const long peak = ImageSearchAlgorithms::FindLine(retStr);
    if (peak < min_points)
        retStr.clear(); // 未达阈值：视为无可靠直线，防止幻觉线
    return peak < 0 ? 0 : peak;
}

} // namespace op::image
