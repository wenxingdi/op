#pragma once
#include "ImageSearchAlgorithms.h"
#include "../algorithm/StrMatch.h" // P1 FindStr 增强的匹配内核（纯头文件）
#include "../ocr/OcrService.h"     // vocr_rec_t / vocr_line_ext_t（P0 逐字 JSON 用）
#include <memory>
#include <string>

namespace op::image {

/*
此类为图像处理，包含以下工作
1.像素比较，查找
2.颜色转化
3.图像定位
4.简单OCR
5....

同一个 ImageSearchService 实例会复用截图和算法工作区，不支持并发调用；
多线程场景应保持每个线程使用独立 Op 对象。
*/
class ImageSearchService : public ImageSearchAlgorithms {
  public:
    // 字库槽位对外范围为 0-99。
    const static int _max_dict = 100;

    ImageSearchService();
    ~ImageSearchService();
    //
    long Capture(const std::wstring &file);

    long CmpColor(long x, long y, const std::wstring &scolor, double sim);

    long FindColor(const wstring &color, double sim, long dir, long &x, long &y);

    long FindColorEx(const wstring &color, double sim, long dir, wstring &retstr);

    long FindMultiColor(const wstring &first_color, const wstring &offset_color, double sim, long dir, long &x,
                        long &y);

    long FindMultiColorEx(const wstring &first_color, const wstring &offset_color, double sim, long dir,
                          wstring &retstr);
    // 图形定位
    long FindPic(const std::wstring &files, const wstring &delta_colors, double sim, long dir, long &x, long &y);
    //
    long FindPicEx(const std::wstring &files, const wstring &delta_colors, double sim, long dir, wstring &retstr,
                   bool returnID = true);

    long FindColorBlock(const wstring &color, double sim, long count, long height, long width, long &x, long &y);

    long FindColorBlockEx(const wstring &color, double sim, long count, long height, long width, wstring &retstr);

    long FindColorBlockExS(const wstring &color, double sim, long count, long height, long width, long mode, wstring &retstr);

    std::wstring GetColor(long x, long y);

    long GetColorNum(const wstring &color, double sim);

    long SetMemDict(int idx, const void *data, long size);

    long SetDict(int idx, const wstring &file);

    std::wstring GetDict(long idx, long font_index);

    long UseDict(int idx);

    long AddDict(long idx, const wstring &dict_info);

    long SaveDict(long idx, const wstring &file_name);

    long ClearDict(long idx);

    long GetDictCount(long idx);

    long GetNowDict();

    long SetBinaryPreprocess(long mode, long isolated_threshold, long min_component_area, long bridge_gap);

    long GetBinaryPreprocess(long &mode, long &isolated_threshold, long &min_component_area, long &bridge_gap) const;

    std::wstring FetchWord(rect_t rc, const wstring &color, const wstring &word);

    std::wstring FetchWord(rect_t rc, const wstring &color, double sim, const wstring &word);

    // 取模（单字）：直接基于当前已二值化的 _binary，不再截屏/二值化（GetWordsNoDict 等批量场景用）
    std::wstring FetchWordFromBinary(rect_t rc, const wstring &word);

    long ExtractWordRects(const wstring &color, double sim, long min_word_h, std::vector<rect_t> &rects);

    long ExtractWordRectsEx(const wstring &color, double sim, long min_word_w, long min_word_h, long padding,
                            std::vector<rect_t> &rects);

    long FetchWords(const wstring &color, double sim, const wstring &words, long min_word_h, std::wstring &out_str);

    long FetchWordsEx(const wstring &color, double sim, const wstring &words, long min_word_w, long min_word_h,
                      long padding, std::wstring &out_str);

    long FetchWordsByRects(const wstring &color, double sim, const wstring &words, const std::vector<rect_t> &rects,
                           std::wstring &out_str);

    long GetBinaryPreview(const wstring &color, double sim, std::wstring &out_str);

    long GetWordPreview(const wstring &dict_info, std::wstring &out_str);

    long CheckWordDict(const wstring &dict_info, std::wstring &out_str);

    long NormalizeWordDict(const wstring &dict_info, std::wstring &out_str);

    long RenameWordDict(const wstring &dict_info, const wstring &words, std::wstring &out_str);

    long OCR(const wstring &color, double sim, std::wstring &out_str);

    // 免字库识别：按颜色对(已截入 _src 的)区域做二值化(白字黑底)后再做 OCR。
    // 与 OCR() 的区别：强制先按颜色二值化，适用于彩色背景下隔离指定颜色文字。
    long ocr_auto_color(const wstring &color, double sim, std::wstring &out_str);

    // 单行快模式 ocr_auto_color：颜色二值化后水平投影切行 -> 每行直 rec（跳过 det）。
    // 行定位自动裁掉上下留白（整窗直 rec 会压扁字符致错字），多行区域逐行拼接；
    // 适用于读出区/固定文本，比 det+rec 快。
    long ocr_auto_color_line(const wstring &color, double sim, std::wstring &out_str);

    // 结构化 ocr_auto_color：输出 "x1,y1,x2,y2,conf,text|..."（bbox 已偏移为客户区坐标，相对绑定窗口客户区左上角）。
    long ocr_auto_color_ex(const wstring &color, double sim, std::wstring &out_str);

    // P0（2026-11-20）逐字结构化输出：JSON
    //   [{"text","conf","conf_partial"?,"is_partial","box":[x1,y1,x2,y2],
    //     "partial_chars","chars":[{"ch","conf","x1","x2"}]}]
    // 坐标原图像素系；conf 与旧 API 同一口径（不可与 chars[].conf 互推）。
    // ocr_ex2 与 OcrEx 同源（有字库时走字库匹配，无逐字信息）；ocr_auto_color_ex2 与
    // ocr_auto_color_ex 同源。返回值 = 行数。
    long ocr_ex2(const wstring &color, double sim, std::wstring &out_json);
    long ocr_auto_color_ex2(const wstring &color, double sim, std::wstring &out_json);

    // P3（2026-11-21）OCR 分项耗时自省：返回上一次 OCR 的 det/cls/rec 分项毫秒数。
    // 引擎不支持或还没跑过 -> 输出 {"valid":false,...}。返回值 = 1 表示 valid。
    long ocr_last_profile(std::wstring &out_json);

    long OcrEx(const wstring &color, double sim, std::wstring &out_str);

    long FindStr(const wstring &str, const wstring &color, double sim, long &retx, long &rety);

    long FindStrEx(const wstring &str, const wstring &color, double sim, std::wstring &out_str);

    // P1（2026-11-21）FindStr 增强：多目标结构化查找。
    // json_targets 为 JSON 数组，元素可以是字符串（等价 {"text":...}）或对象：
    //   {"id","text","match":"contains|exact|prefix|suffix|regex",
    //    "fuzzy","max_hits","ci","confirmed_only"}
    // 一次 det+rec 后引擎内匹配，返回每目标的全部命中（含字符级精确范围）。
    // 返回值 = 成功解析并执行的目标数；JSON 非法时返回 -1 且 out_json 为空。
    long find_str_batch(const std::wstring &json_targets, const wstring &color, double sim,
                        std::wstring &out_json);

    // P1：单目标增强查找（标量入参，易语言友好）。flags 位定义见
    // docs/2026-11/P1_FindStr增强与P3耗时自省_实施方案.md §2.3。
    // JSON 结构与 find_str_batch 的同名单目标结果一致。返回值 = 命中总数。
    long find_str_ex2(const std::wstring &pattern, long flags, const wstring &color, double sim,
                      std::wstring &out_json);

  private:
    // P1 共用：抓取全图 OCR 行（含逐字）并做匹配。targets 已校验合法。
    long match_targets(const std::vector<op::str_match::Target> &targets, const wstring &color, double sim,
                       std::wstring &out_json);
    // P1 共用：一次 OCR 得到「行文本 + 行框 + 逐字 x 区间」（已客户区化）。
    struct ocr_line_view_t {
        std::wstring text;
        int x1 = 0, y1 = 0, x2 = 0, y2 = 0;
        bool partial = false;
        bool chars_usable = false;                  // chars.size()==text.size() 且非旋转降级
        std::vector<int> cx1, cx2;                  // 与 text 同下标的字符级 x 区间
    };
    bool collect_ocr_lines(const wstring &color, double sim, std::vector<ocr_line_view_t> &lines);

    // 智能选路（2026-11-20）：统计 _src 中命中指定颜色（逐通道容差）的像素数。
    // 返回 -1 = 无法探测（空串/@反白等特殊写法），调用方应保守沿用颜色路径。
    // out_total_px 输出总像素数，供调用方算占比。
    long count_color_hits(const wstring &color, int &out_total_px);
    // P0 JSON 构建：把逐字过滤后的结果拼成 JSON。off_x/off_y 为坐标偏移，
    // force_full_width_chars=true 时逐字退化为整行跨度（字库/旋转降级）。
    long build_ocr_json(const vocr_rec_t &res, const vocr_line_ext_t &ext, int off_x, int off_y,
                        int clip_w, int clip_h, bool bbox_known, bool force_full_width_chars,
                        std::wstring &out_json);
    // 字库路径 "x,y,text|..." 旧格式转 JSON（无逐字信息）。
    long legacy_ocr_to_json(const std::wstring &legacy, long count, std::wstring &out_json);
    // 免字库识别（OnnxOcrEngine 直吃 _src 原图，不按颜色二值化）。
    // 供 OCR() 的无字库兜底分支、以及 OcrAuto / OcrAutoFromFile 共用。
    long OcrFree(double sim, std::wstring &out_str);

  public:
    long OcrAuto(double sim, std::wstring &retstr);

    long OcrFromFile(const wstring &files, const wstring &color, double sim, std::wstring &retstr);

    // 文件版 ocr_auto_color：读取图片到 _src 后按颜色二值化再做免字库 OCR（与 ocr_auto_color 同源）。
    long ocr_auto_color_from_file(const wstring &files, const wstring &color, double sim, std::wstring &retstr);

    long OcrAutoFromFile(const wstring &files, double sim, std::wstring &retstr);

    long FindLine(const wstring &color, double sim, wstring &retStr);

    // 带阈值 + 去噪的找线：峰值点数 < min_points 时清空 retstr（根治幻觉线）。
    long FindLineExS(const wstring &color, double sim, long min_points, wstring &retStr);

    long LoadPic(const wstring &files);

    long FreePic(const wstring &files);

    // 清空进程级图片缓存（FindPic 自动缓存与 LoadPic/LoadMemPic 显式加载共用）。
    long clear_pic_cache();

    // 设置进程级图片缓存条目上限；max_count<=0 表示不设上限。默认 500，满时整体清空重存。
    void set_pic_cache_max(long max_count);

    long LoadMemPic(const wstring &file_name, void *data, long size);

    long GetPicSize(const wstring &file_name, long *x, long *y);

    void str2binaryfbk(const wstring &color);

  private:
    // SetDict/SetMemDict 写入全局字库槽；AddDict/ClearDict 写入对象私有槽。
    Dictionary _private_dicts[_max_dict];
    bool _private_dict_overrides[_max_dict];
    // 当前字库索引
    int _curr_idx;
    long _binary_preprocess_mode;
    long _binary_isolated_threshold;
    long _binary_min_component_area;
    long _binary_bridge_gap;

  public:
    // 当前目录
    wstring _curr_path;
    // 是否允许 FindPic 自动缓存本地文件；LoadPic/LoadMemPic 属于显式加载，不受此开关影响。
    int _enable_cache;

  private:
    // RETURN TYPE 0:word colors info; 1:bk color info
    int str2colordfs(const wstring &color_str, std::vector<color_df_t> &colors);
    int str2colordfs(const wstring &color_str, std::vector<color_df_t> &colors, std::vector<bool> *explicit_dfs);
    void parse_multi_color_args(const wstring &first_color, const wstring &offset_color,
                                std::vector<color_df_t> &vfirst_color, std::vector<pt_cr_df_t> &voffset_cr);
void str2binaryfbk(const wstring &color, double sim, int implicit_df_override = -1);
    void str2pointbinaryfbk(const wstring &color);
    void str2pointbinaryfbk(const wstring &color, double sim);
    void ApplyBinaryPreprocess();
    long FetchWordsFromBinary(const wstring &words, const std::vector<rect_t> &rects, std::wstring &out_str);
    std::shared_ptr<Dictionary> ActiveDict(int idx);
    Dictionary &MutablePrivateDict(int idx);
    void str2colors(const wstring &color, std::vector<color_t> &vcolor);
    // vmatches 是算法层使用的裸指针视图，holders 负责延长共享缓存对象的生命周期。
    void files2mats(const wstring &files, std::vector<PicMatchTemplate *> &vmatches, std::vector<wstring> &vstr,
                    std::vector<std::shared_ptr<Image>> &holders,
                    std::vector<std::shared_ptr<PicMatchTemplate>> &match_holders);
    // FindPic/FindPicEx 共用的入口准备结果：模板视图 + 名称 + 生命周期持有者 + delta 偏色。
    struct find_pic_ctx_t {
        std::vector<PicMatchTemplate *> vmatches;
        std::vector<wstring> vpic_name;
        std::vector<std::shared_ptr<Image>> holders;
        std::vector<std::shared_ptr<PicMatchTemplate>> match_holders;
        color_t dfcolor;
    };
    // files2mats + delta 偏色解析 + sim 域变换（0.5+sim/2），FindPic/FindPicEx 共用。
    void prepare_find_pic(const wstring &files, const wstring &delta_colors, double &sim, find_pic_ctx_t &ctx);
};

} // namespace op::image
