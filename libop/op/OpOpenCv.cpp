#include "OpContext.h"
#include "OpResult.h"

#include "opencv/OpenCvBridge.h"
#include "opencv/OpenCvHelpers.h"
#include "opencv/TemplateMatcher.h"
#include "base/Utils.h"

#include <libop.h>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/objdetect.hpp>

#include <filesystem>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {

// 二维码文本走 CP_UTF8 编解码（与 MiniImage.h 同口径；OpenCV imread/imwrite
// 的 string 路径在 Windows 按 ANSI 解释，内容串必须显式 UTF-8）。
std::string utf8_encode(const wchar_t *text) {
    if (!text || !text[0])
        return "";
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1)
        return "";
    std::string out(n - 1, '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring utf8_decode(const std::string &text) {
    if (text.empty())
        return L"";
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0);
    if (n <= 0)
        return L"";
    std::wstring out(n, L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(), n);
    return out;
}

// 公共别名匹配（第十二批 OP1）：小写化后在静态 {别名, 枚举值} 表中查值，
// 空串别名即"缺省值"。五个 parse_cv_* 模式解析器共用。
template <typename Enum, size_t N>
bool match_cv_alias(const wchar_t *text, const std::pair<const wchar_t *, Enum> (&table)[N], Enum &out) {
    std::wstring value = text ? text : L"";
    wstring2lower(value);
    for (const auto &entry : table) {
        if (value == entry.first) {
            out = entry.second;
            return true;
        }
    }
    return false;
}

bool parse_cv_threshold_mode(const wchar_t *mode_text, opcv::ThresholdMode &mode) {
    static const std::pair<const wchar_t *, opcv::ThresholdMode> kTable[] = {
        {L"binary", opcv::ThresholdMode::Binary},
        {L"", opcv::ThresholdMode::Binary},
        {L"binary_inv", opcv::ThresholdMode::BinaryInv},
        {L"inv", opcv::ThresholdMode::BinaryInv},
        {L"otsu", opcv::ThresholdMode::Otsu},
        {L"otsu_inv", opcv::ThresholdMode::OtsuInv},
        {L"adaptive", opcv::ThresholdMode::Adaptive},
        {L"adaptive_inv", opcv::ThresholdMode::AdaptiveInv},
    };
    return match_cv_alias(mode_text, kTable, mode);
}

bool parse_cv_color_space(const wchar_t *space_text, opcv::InRangeColorSpace &color_space) {
    static const std::pair<const wchar_t *, opcv::InRangeColorSpace> kTable[] = {
        {L"bgr", opcv::InRangeColorSpace::Bgr},
        {L"", opcv::InRangeColorSpace::Bgr},
        {L"hsv", opcv::InRangeColorSpace::Hsv},
        {L"gray", opcv::InRangeColorSpace::Gray},
        {L"grey", opcv::InRangeColorSpace::Gray},
    };
    return match_cv_alias(space_text, kTable, color_space);
}

bool parse_cv_morphology_mode(const wchar_t *mode_text, opcv::MorphologyMode &mode) {
    static const std::pair<const wchar_t *, opcv::MorphologyMode> kTable[] = {
        {L"erode", opcv::MorphologyMode::Erode},
        {L"dilate", opcv::MorphologyMode::Dilate},
        {L"open", opcv::MorphologyMode::Open},
        {L"", opcv::MorphologyMode::Open},
        {L"close", opcv::MorphologyMode::Close},
    };
    return match_cv_alias(mode_text, kTable, mode);
}

bool parse_cv_thin_mode(const wchar_t *mode_text, opcv::ThinMode &mode) {
    static const std::pair<const wchar_t *, opcv::ThinMode> kTable[] = {
        {L"zhang_suen", opcv::ThinMode::ZhangSuen},
        {L"zhangsuen", opcv::ThinMode::ZhangSuen},
        {L"", opcv::ThinMode::ZhangSuen},
        {L"guo_hall", opcv::ThinMode::GuoHall},
        {L"guohall", opcv::ThinMode::GuoHall},
        {L"morph", opcv::ThinMode::Morph},
    };
    return match_cv_alias(mode_text, kTable, mode);
}

bool parse_cv_blur_mode(const wchar_t *mode_text, opcv::BlurMode &mode) {
    static const std::pair<const wchar_t *, opcv::BlurMode> kTable[] = {
        {L"gaussian", opcv::BlurMode::Gaussian},
        {L"", opcv::BlurMode::Gaussian},
        {L"median", opcv::BlurMode::Median},
        {L"bilateral", opcv::BlurMode::Bilateral},
        {L"box", opcv::BlurMode::Box},
        {L"mean", opcv::BlurMode::Box},
    };
    return match_cv_alias(mode_text, kTable, mode);
}

bool parse_cv_number_list(const wchar_t *text, std::vector<double> &values) {
    values.clear();
    if (text == nullptr || text[0] == L'\0') {
        return false;
    }

    // 支持 "1,2,3" 和 "1|2|3"，方便 COM/Python 侧直接传字符串。
    std::wstring normalized = text;
    replacew(normalized, L"|", L",");

    std::vector<std::wstring> parts;
    split(normalized, parts, L",");
    for (const auto &part : parts) {
        if (part.empty()) {
            return false;
        }

        try {
            size_t parsed = 0;
            const double value = std::stod(part, &parsed);
            if (parsed != part.size()) {
                return false;
            }
            values.push_back(value);
        } catch (...) {
            return false;
        }
    }
    return !values.empty();
}

template <typename Preprocess>
void run_cv_file_preprocess(const wchar_t *src_file, const wchar_t *dst_file, long *ret, Preprocess &&preprocess) {
    op::internal::set_result(ret, 0L);
    if (src_file == nullptr || dst_file == nullptr) {
        return;
    }

    opcv::ImageHandle source;
    opcv::ImageHandle output;
    if (!opcv::LoadImageFromFile(src_file, source) || !preprocess(source, output) ||
        !opcv::SaveImageToFile(output, dst_file)) {
        return;
    }

    op::internal::set_result(ret, 1L);
}

// 公共 JSON 框架（第十二批 OP3）：{"ok":..,"results":[<items>]}，元素序列化由调用方传入。
// components/contours 两份构建器仅元素字段不同，合并为一个框架 + 两个元素格式化 lambda。
template <typename Item, typename FormatItem>
std::wstring build_cv_results_json(bool ok, const std::vector<Item> &results, FormatItem &&format_item) {
    std::wostringstream oss;
    oss << L"{\"ok\":" << (ok ? 1 : 0) << L",\"results\":[";
    for (size_t i = 0; i < results.size(); ++i) {
        if (i != 0) {
            oss << L",";
        }
        format_item(oss, results[i]);
    }
    oss << L"]}";
    return oss.str();
}

std::wstring build_cv_components_json(const std::vector<opcv::RegionAnalysisResult> &results, bool ok) {
    return build_cv_results_json(ok, results, [](std::wostringstream &oss, const opcv::RegionAnalysisResult &item) {
        oss << L"{\"x\":" << item.x << L",\"y\":" << item.y << L",\"width\":" << item.width << L",\"height\":"
            << item.height << L",\"area\":" << static_cast<long long>(item.area) << L"}";
    });
}

std::wstring build_cv_contours_json(const std::vector<opcv::ContourAnalysisResult> &results, bool ok) {
    return build_cv_results_json(ok, results, [](std::wostringstream &oss, const opcv::ContourAnalysisResult &item) {
        oss << L"{\"x\":" << item.x << L",\"y\":" << item.y << L",\"width\":" << item.width << L",\"height\":"
            << item.height << L",\"area\":" << static_cast<long long>(item.area) << L",\"perimeter\":"
            << static_cast<long long>(item.perimeter) << L",\"points\":" << item.points << L"}";
    });
}

bool parse_cv_pipeline_step(const std::wstring &step, std::wstring &name, std::vector<std::wstring> &args) {
    name.clear();
    args.clear();
    if (step.empty()) {
        return false;
    }

    const size_t colon = step.find(L':');
    name = colon == std::wstring::npos ? step : step.substr(0, colon);
    wstring2lower(name);
    if (name.empty()) {
        return false;
    }

    if (colon != std::wstring::npos && colon + 1 < step.size()) {
        split(step.substr(colon + 1), args, L",");
    }
    return true;
}

bool parse_cv_pipeline_double_arg(const std::vector<std::wstring> &args, size_t index, double default_value,
                                  double &value) {
    value = default_value;
    if (index >= args.size() || args[index].empty()) {
        return true;
    }

    try {
        size_t parsed = 0;
        value = std::stod(args[index], &parsed);
        return parsed == args[index].size();
    } catch (...) {
        return false;
    }
}

bool parse_cv_pipeline_long_arg(const std::vector<std::wstring> &args, size_t index, long default_value, long &value) {
    value = default_value;
    if (index >= args.size() || args[index].empty()) {
        return true;
    }

    try {
        size_t parsed = 0;
        value = std::stol(args[index], &parsed);
        return parsed == args[index].size();
    } catch (...) {
        return false;
    }
}

// 无参步骤别名表（第十二批 OP2）：gray/binary/edge/outline/denoise/equalize/cropvalid
// 及其全部历史别名 → opcv 一步函数。查不到返回 false，调用方继续走带参步骤链。
bool cv_pipeline_apply_noarg_step(const std::wstring &name, const opcv::ImageHandle &source,
                                  opcv::ImageHandle &output) {
    using StepFn = bool (*)(const opcv::ImageHandle &, opcv::ImageHandle &);
    static const std::pair<const wchar_t *, StepFn> kNoArgSteps[] = {
        {L"gray", &opcv::ToGray},
        {L"togray", &opcv::ToGray},
        {L"to_gray", &opcv::ToGray},
        {L"cvtogray", &opcv::ToGray},
        {L"binary", &opcv::ToBinary},
        {L"tobinary", &opcv::ToBinary},
        {L"to_binary", &opcv::ToBinary},
        {L"cvtobinary", &opcv::ToBinary},
        {L"edge", &opcv::ToEdge},
        {L"toedge", &opcv::ToEdge},
        {L"to_edge", &opcv::ToEdge},
        {L"cvtoedge", &opcv::ToEdge},
        {L"outline", &opcv::ToOutline},
        {L"tooutline", &opcv::ToOutline},
        {L"to_outline", &opcv::ToOutline},
        {L"cvtooutline", &opcv::ToOutline},
        {L"denoise", &opcv::Denoise},
        {L"cvdenoise", &opcv::Denoise},
        {L"equalize", &opcv::Equalize},
        {L"cvequalize", &opcv::Equalize},
        {L"cropvalid", &opcv::CropValid},
        {L"crop_valid", &opcv::CropValid},
        {L"cvcropvalid", &opcv::CropValid},
    };
    for (const auto &entry : kNoArgSteps) {
        if (name == entry.first)
            return entry.second(source, output);
    }
    return false;
}

bool cv_pipeline_apply_step(const std::wstring &name, const std::vector<std::wstring> &args,
                            const opcv::ImageHandle &source, opcv::ImageHandle &output) {
    // 每一步只接收当前图像并输出下一张图，保证流水线状态简单清晰。
    // 无参步骤先查别名映射表；带参步骤逻辑各异，保留 if 链。
    if (cv_pipeline_apply_noarg_step(name, source, output))
        return true;

    if (name == L"clahe") {
        double clip_limit = 2.0;
        long tile_grid_size = 8;
        return parse_cv_pipeline_double_arg(args, 0, 2.0, clip_limit) &&
               parse_cv_pipeline_long_arg(args, 1, 8, tile_grid_size) &&
               opcv::CLAHE(source, output, clip_limit, static_cast<int>(tile_grid_size));
    }
    if (name == L"blur") {
        opcv::BlurMode mode;
        long kernel_size = 3;
        const wchar_t *mode_text = args.empty() ? L"gaussian" : args[0].c_str();
        return parse_cv_blur_mode(mode_text, mode) && parse_cv_pipeline_long_arg(args, 1, 3, kernel_size) &&
               opcv::Blur(source, output, mode, static_cast<int>(kernel_size));
    }
    if (name == L"sharpen") {
        double strength = 1.0;
        return parse_cv_pipeline_double_arg(args, 0, 1.0, strength) && opcv::Sharpen(source, output, strength);
    }
    if (name == L"threshold") {
        opcv::ThresholdMode mode;
        const wchar_t *mode_text = args.empty() ? L"otsu" : args[0].c_str();
        double threshold = 0.0;
        double max_value = 255.0;
        return parse_cv_threshold_mode(mode_text, mode) && parse_cv_pipeline_double_arg(args, 1, 0.0, threshold) &&
               parse_cv_pipeline_double_arg(args, 2, 255.0, max_value) &&
               opcv::Threshold(source, output, threshold, max_value, mode);
    }
    if (name == L"inrange") {
        if (args.size() < 7) {
            return false;
        }

        opcv::InRangeColorSpace color_space;
        const std::wstring lower = args[1] + L"," + args[2] + L"," + args[3];
        const std::wstring upper = args[4] + L"," + args[5] + L"," + args[6];
        std::vector<double> lower_values;
        std::vector<double> upper_values;
        return parse_cv_color_space(args[0].c_str(), color_space) &&
               parse_cv_number_list(lower.c_str(), lower_values) && parse_cv_number_list(upper.c_str(), upper_values) &&
               opcv::InRange(source, output, color_space, lower_values, upper_values);
    }
    if (name == L"morph") {
        opcv::MorphologyMode mode;
        long kernel_size = 3;
        long iterations = 1;
        const wchar_t *mode_text = args.empty() ? L"open" : args[0].c_str();
        return parse_cv_morphology_mode(mode_text, mode) && parse_cv_pipeline_long_arg(args, 1, 3, kernel_size) &&
               parse_cv_pipeline_long_arg(args, 2, 1, iterations) &&
               opcv::Morphology(source, output, mode, static_cast<int>(kernel_size), static_cast<int>(iterations));
    }
    if (name == L"thin") {
        opcv::ThinMode mode;
        const wchar_t *mode_text = args.empty() ? L"zhang_suen" : args[0].c_str();
        return parse_cv_thin_mode(mode_text, mode) && opcv::Thin(source, output, mode);
    }
    if (name == L"resize") {
        long width = 0;
        long height = 0;
        return parse_cv_pipeline_long_arg(args, 0, 0, width) && parse_cv_pipeline_long_arg(args, 1, 0, height) &&
               opcv::Resize(source, static_cast<int>(width), static_cast<int>(height), output);
    }

    return false;
}

} // namespace

void op::Op::CvLoadTemplate(const wchar_t *name, const wchar_t *file_path, long *ret) {
    internal::set_result(ret, (name != nullptr && file_path != nullptr && opcv::LoadTemplate(name, file_path)) ? 1L : 0L);
}

void op::Op::CvLoadMaskedTemplate(const wchar_t *name, const wchar_t *template_path, const wchar_t *mask_path,
                                 long *ret) {
    internal::set_result(ret,
                         (name != nullptr && template_path != nullptr && mask_path != nullptr &&
                          opcv::LoadMaskedTemplate(name, template_path, mask_path))
                             ? 1L
                             : 0L);
}

void op::Op::CvRemoveTemplate(const wchar_t *name, long *ret) {
    internal::set_result(ret, (name != nullptr && opcv::RemoveTemplate(name)) ? 1L : 0L);
}

void op::Op::CvRemoveAllTemplates(long *ret) {
    opcv::RemoveAllTemplates();
    internal::set_result(ret, 1L);
}

void op::Op::CvHasTemplate(const wchar_t *name, long *ret) {
    internal::set_result(ret, (name != nullptr && opcv::HasTemplate(name)) ? 1L : 0L);
}

void op::Op::CvGetTemplateCount(long *ret) {
    internal::set_result(ret, opcv::GetTemplateCount());
}

void op::Op::CvGetAllTemplateNames(std::wstring &retstr) {
    retstr.clear();
    const auto names = opcv::GetAllTemplateNames();
    for (size_t i = 0; i < names.size(); ++i) {
        if (i != 0) {
            retstr += L"|";
        }
        retstr += names[i];
    }
}

void op::Op::CvGetOpenCvVersion(std::wstring &retstr) {
    retstr = opcv::GetOpenCvVersion();
}

void op::Op::CvLoadTemplateList(const wchar_t *template_list, long *ret) {
    internal::set_result(ret, 0L);
    if (template_list == nullptr || template_list[0] == L'\0') {
        return;
    }

    std::vector<std::wstring> items;
    split(template_list, items, L"|");

    std::vector<std::pair<std::wstring, std::wstring>> templates;
    templates.reserve(items.size());
    for (const auto &item : items) {
        if (item.empty()) {
            continue;
        }

        const size_t comma_pos = item.find(L',');
        if (comma_pos == std::wstring::npos || comma_pos == 0 || comma_pos + 1 >= item.size()) {
            return;
        }

        const std::wstring name = item.substr(0, comma_pos);
        const std::wstring path = item.substr(comma_pos + 1);
        if (name.empty() || path.empty()) {
            return;
        }
        templates.emplace_back(name, path);
    }

    if (templates.empty()) {
        return;
    }

    internal::set_result(ret, opcv::LoadTemplateList(templates) ? 1L : 0L);
}

void op::Op::CvToGray(const wchar_t *src_file, const wchar_t *dst_file, long *ret) {
    run_cv_file_preprocess(src_file, dst_file, ret, [](const opcv::ImageHandle &source, opcv::ImageHandle &output) {
        return opcv::ToGray(source, output);
    });
}

void op::Op::CvToBinary(const wchar_t *src_file, const wchar_t *dst_file, long *ret) {
    run_cv_file_preprocess(src_file, dst_file, ret, [](const opcv::ImageHandle &source, opcv::ImageHandle &output) {
        return opcv::ToBinary(source, output);
    });
}

void op::Op::CvToEdge(const wchar_t *src_file, const wchar_t *dst_file, long *ret) {
    run_cv_file_preprocess(src_file, dst_file, ret, [](const opcv::ImageHandle &source, opcv::ImageHandle &output) {
        return opcv::ToEdge(source, output);
    });
}

void op::Op::CvToOutline(const wchar_t *src_file, const wchar_t *dst_file, long *ret) {
    run_cv_file_preprocess(src_file, dst_file, ret, [](const opcv::ImageHandle &source, opcv::ImageHandle &output) {
        return opcv::ToOutline(source, output);
    });
}

void op::Op::CvDenoise(const wchar_t *src_file, const wchar_t *dst_file, long *ret) {
    run_cv_file_preprocess(src_file, dst_file, ret, [](const opcv::ImageHandle &source, opcv::ImageHandle &output) {
        return opcv::Denoise(source, output);
    });
}

void op::Op::CvEqualize(const wchar_t *src_file, const wchar_t *dst_file, long *ret) {
    run_cv_file_preprocess(src_file, dst_file, ret, [](const opcv::ImageHandle &source, opcv::ImageHandle &output) {
        return opcv::Equalize(source, output);
    });
}

void op::Op::CvCLAHE(const wchar_t *src_file, const wchar_t *dst_file, double clip_limit, long tile_grid_size,
                    long *ret) {
    run_cv_file_preprocess(src_file, dst_file, ret, [&](const opcv::ImageHandle &source, opcv::ImageHandle &output) {
        return opcv::CLAHE(source, output, clip_limit, static_cast<int>(tile_grid_size));
    });
}

void op::Op::CvBlur(const wchar_t *src_file, const wchar_t *dst_file, const wchar_t *mode, long kernel_size, long *ret) {
    opcv::BlurMode blur_mode;
    if (!parse_cv_blur_mode(mode, blur_mode)) {
        internal::set_result(ret, 0L);
        return;
    }

    run_cv_file_preprocess(src_file, dst_file, ret, [&](const opcv::ImageHandle &source, opcv::ImageHandle &output) {
        return opcv::Blur(source, output, blur_mode, static_cast<int>(kernel_size));
    });
}

void op::Op::CvSharpen(const wchar_t *src_file, const wchar_t *dst_file, double strength, long *ret) {
    run_cv_file_preprocess(src_file, dst_file, ret, [&](const opcv::ImageHandle &source, opcv::ImageHandle &output) {
        return opcv::Sharpen(source, output, strength);
    });
}

void op::Op::CvCropValid(const wchar_t *src_file, const wchar_t *dst_file, long *ret) {
    run_cv_file_preprocess(src_file, dst_file, ret, [](const opcv::ImageHandle &source, opcv::ImageHandle &output) {
        return opcv::CropValid(source, output);
    });
}

void op::Op::CvConnectedComponents(const wchar_t *src_file, double min_area, std::wstring &retjson, long *ret) {
    internal::set_result(ret, 0L);
    retjson = build_cv_components_json({}, false);
    if (src_file == nullptr) {
        return;
    }

    opcv::ImageHandle source;
    std::vector<opcv::RegionAnalysisResult> results;
    const bool ok = opcv::LoadImageFromFile(src_file, source) && opcv::ConnectedComponents(source, min_area, results);
    retjson = build_cv_components_json(results, ok);
    internal::set_result(ret, ok ? 1L : 0L);
}

void op::Op::CvFindContours(const wchar_t *src_file, double min_area, std::wstring &retjson, long *ret) {
    internal::set_result(ret, 0L);
    retjson = build_cv_contours_json({}, false);
    if (src_file == nullptr) {
        return;
    }

    opcv::ImageHandle source;
    std::vector<opcv::ContourAnalysisResult> results;
    const bool ok = opcv::LoadImageFromFile(src_file, source) && opcv::FindContours(source, min_area, results);
    retjson = build_cv_contours_json(results, ok);
    internal::set_result(ret, ok ? 1L : 0L);
}

void op::Op::CvPreprocessPipeline(const wchar_t *src_file, const wchar_t *dst_file, const wchar_t *pipeline, long *ret) {
    internal::set_result(ret, 0L);
    if (src_file == nullptr || dst_file == nullptr || pipeline == nullptr || pipeline[0] == L'\0') {
        return;
    }

    opcv::ImageHandle current;
    if (!opcv::LoadImageFromFile(src_file, current)) {
        return;
    }

    std::vector<std::wstring> steps;
    split(pipeline, steps, L"|");

    bool applied = false;
    for (const auto &step : steps) {
        std::wstring name;
        std::vector<std::wstring> args;
        if (step.empty()) {
            continue;
        }
        if (!parse_cv_pipeline_step(step, name, args)) {
            return;
        }

        opcv::ImageHandle next;
        if (!cv_pipeline_apply_step(name, args, current, next)) {
            return;
        }

        current = std::move(next);
        applied = true;
    }

    if (!applied || !opcv::SaveImageToFile(current, dst_file)) {
        return;
    }

    internal::set_result(ret, 1L);
}

void op::Op::CvCrop(const wchar_t *src_file, long x, long y, long width, long height, const wchar_t *dst_file,
                   long *ret) {
    internal::set_result(ret, 0L);
    if (src_file == nullptr || dst_file == nullptr) {
        return;
    }

    opcv::ImageHandle source;
    opcv::ImageHandle output;
    opcv::Region region;
    region.x = static_cast<int>(x);
    region.y = static_cast<int>(y);
    region.width = static_cast<int>(width);
    region.height = static_cast<int>(height);
    if (!opcv::LoadImageFromFile(src_file, source) || !opcv::Crop(source, region, output) ||
        !opcv::SaveImageToFile(output, dst_file)) {
        return;
    }

    internal::set_result(ret, 1L);
}

void op::Op::CvResize(const wchar_t *src_file, long width, long height, const wchar_t *dst_file, long *ret) {
    internal::set_result(ret, 0L);
    if (src_file == nullptr || dst_file == nullptr) {
        return;
    }

    opcv::ImageHandle source;
    opcv::ImageHandle output;
    if (!opcv::LoadImageFromFile(src_file, source) ||
        !opcv::Resize(source, static_cast<int>(width), static_cast<int>(height), output) ||
        !opcv::SaveImageToFile(output, dst_file)) {
        return;
    }

    internal::set_result(ret, 1L);
}

void op::Op::CvThreshold(const wchar_t *src_file, const wchar_t *dst_file, double threshold, double max_value,
                        const wchar_t *mode, long *ret) {
    internal::set_result(ret, 0L);
    if (src_file == nullptr || dst_file == nullptr) {
        return;
    }

    opcv::ThresholdMode threshold_mode;
    if (!parse_cv_threshold_mode(mode, threshold_mode)) {
        return;
    }

    opcv::ImageHandle source;
    opcv::ImageHandle output;
    if (!opcv::LoadImageFromFile(src_file, source) ||
        !opcv::Threshold(source, output, threshold, max_value, threshold_mode) ||
        !opcv::SaveImageToFile(output, dst_file)) {
        return;
    }

    internal::set_result(ret, 1L);
}

void op::Op::CvInRange(const wchar_t *src_file, const wchar_t *dst_file, const wchar_t *color_space,
                      const wchar_t *lower, const wchar_t *upper, long *ret) {
    internal::set_result(ret, 0L);
    if (src_file == nullptr || dst_file == nullptr) {
        return;
    }

    opcv::InRangeColorSpace parsed_color_space;
    std::vector<double> lower_values;
    std::vector<double> upper_values;
    if (!parse_cv_color_space(color_space, parsed_color_space) || !parse_cv_number_list(lower, lower_values) ||
        !parse_cv_number_list(upper, upper_values)) {
        return;
    }

    opcv::ImageHandle source;
    opcv::ImageHandle output;
    if (!opcv::LoadImageFromFile(src_file, source) ||
        !opcv::InRange(source, output, parsed_color_space, lower_values, upper_values) ||
        !opcv::SaveImageToFile(output, dst_file)) {
        return;
    }

    internal::set_result(ret, 1L);
}

void op::Op::CvMorphology(const wchar_t *src_file, const wchar_t *dst_file, const wchar_t *mode, long kernel_size,
                         long iterations, long *ret) {
    internal::set_result(ret, 0L);
    if (src_file == nullptr || dst_file == nullptr) {
        return;
    }

    opcv::MorphologyMode morphology_mode;
    if (!parse_cv_morphology_mode(mode, morphology_mode)) {
        return;
    }

    opcv::ImageHandle source;
    opcv::ImageHandle output;
    if (!opcv::LoadImageFromFile(src_file, source) ||
        !opcv::Morphology(source, output, morphology_mode, static_cast<int>(kernel_size),
                          static_cast<int>(iterations)) ||
        !opcv::SaveImageToFile(output, dst_file)) {
        return;
    }

    internal::set_result(ret, 1L);
}

void op::Op::CvThin(const wchar_t *src_file, const wchar_t *dst_file, const wchar_t *mode, long *ret) {
    internal::set_result(ret, 0L);
    if (src_file == nullptr || dst_file == nullptr) {
        return;
    }

    opcv::ThinMode thin_mode;
    if (!parse_cv_thin_mode(mode, thin_mode)) {
        return;
    }

    opcv::ImageHandle source;
    opcv::ImageHandle output;
    if (!opcv::LoadImageFromFile(src_file, source) || !opcv::Thin(source, output, thin_mode) ||
        !opcv::SaveImageToFile(output, dst_file)) {
        return;
    }

    internal::set_result(ret, 1L);
}

namespace {

// Match 骨架（第十二批 OP3）：8 个 Cv*MatchTemplate 变体共享的「失败 JSON + ret 归零 +
// 区域捕获」。fn 收到捕获成功的 source/region/origin，负责匹配、origin 平移、
// BuildXxxJson 与 set_result；捕获失败 fn 不执行，retjson/ret 保持入口处失败值。
// m_context 为 Op 私有成员，故以两个服务引用传入。
template <typename Fn>
void cv_match_capture_run(op::binding::BindingSession &bkproc, op::image::ImageSearchService &image_proc, long x,
                          long y, long width, long height, std::wstring &retjson, long *ret, const wchar_t *fail_json,
                          Fn &&fn) {
    retjson = fail_json;
    op::internal::set_result(ret, 0L);
    opcv::ImageHandle source;
    opcv::Region region;
    int origin_x = 0;
    int origin_y = 0;
    if (!opcv::bridge::CaptureRegion(bkproc, image_proc, x, y, width, height, source, region, origin_x, origin_y))
        return;
    fn(source, region, origin_x, origin_y);
}

} // namespace

void op::Op::CvMatchTemplate(long x, long y, long width, long height, const wchar_t *template_name, double threshold,
                            long dir, long strip_mode, long method, long color_mode, std::wstring &retjson, long *ret) {
    if (template_name == nullptr) {
        retjson = L"{\"ok\":0}";
        internal::set_result(ret, 0L);
        return;
    }
    cv_match_capture_run(m_context->bkproc, m_context->image_proc, x, y, width, height, retjson, ret, L"{\"ok\":0}",
                         [&](const opcv::ImageHandle &source, const opcv::Region &region, int origin_x, int origin_y) {
                             opcv::MatchResult match;
                             const bool ok = opcv::MatchTemplate(
                                 source, template_name, region, threshold, match, static_cast<opcv::SearchDirection>(dir),
                                 static_cast<opcv::StripMode>(strip_mode), method,
                                 opcv::bridge::ParseColorMode(color_mode));
                             if (ok) {
                                 match.x += origin_x;
                                 match.y += origin_y;
                             }
                             retjson = opcv::bridge::BuildMatchJson(match, ok);
                             internal::set_result(ret, ok ? 1L : 0L);
                         });
}

void op::Op::CvMatchTemplateScale(long x, long y, long width, long height, const wchar_t *template_name,
                                 const wchar_t *scales, double threshold, long method, long color_mode,
                                 std::wstring &retjson, long *ret) {
    if (template_name == nullptr) {
        retjson = L"{\"ok\":0}";
        internal::set_result(ret, 0L);
        return;
    }
    std::vector<double> scale_values;
    if (!opcv::bridge::ParseScaleList(scales, scale_values)) {
        retjson = L"{\"ok\":0}";
        internal::set_result(ret, 0L);
        return;
    }

    cv_match_capture_run(m_context->bkproc, m_context->image_proc, x, y, width, height, retjson, ret, L"{\"ok\":0}",
                         [&](const opcv::ImageHandle &source, const opcv::Region &region, int origin_x, int origin_y) {
                             opcv::MatchResult match;
                             const bool ok = opcv::MatchTemplateScale(source, template_name, region, scale_values,
                                                                      threshold, match, method,
                                                                      opcv::bridge::ParseColorMode(color_mode));
                             if (ok) {
                                 match.x += origin_x;
                                 match.y += origin_y;
                             }
                             retjson = opcv::bridge::BuildMatchJson(match, ok);
                             internal::set_result(ret, ok ? 1L : 0L);
                         });
}

void op::Op::CvMatchAnyTemplate(long x, long y, long width, long height, const wchar_t *template_names, double threshold,
                               long dir, long strip_mode, long method, long color_mode, std::wstring &retjson,
                               long *ret) {
    std::vector<std::wstring> names;
    if (!opcv::bridge::ParseTemplateNames(template_names, names)) {
        retjson = L"{\"ok\":0}";
        internal::set_result(ret, 0L);
        return;
    }

    cv_match_capture_run(m_context->bkproc, m_context->image_proc, x, y, width, height, retjson, ret, L"{\"ok\":0}",
                         [&](const opcv::ImageHandle &source, const opcv::Region &region, int origin_x, int origin_y) {
                             opcv::NamedMatchResult match;
                             const bool ok = opcv::MatchAnyTemplate(
                                 source, names, region, threshold, match, static_cast<opcv::SearchDirection>(dir),
                                 static_cast<opcv::StripMode>(strip_mode), method,
                                 opcv::bridge::ParseColorMode(color_mode));
                             if (ok) {
                                 match.match.x += origin_x;
                                 match.match.y += origin_y;
                             }
                             retjson = opcv::bridge::BuildNamedMatchJson(match, ok);
                             internal::set_result(ret, ok ? 1L : 0L);
                         });
}

void op::Op::CvMatchAllTemplates(long x, long y, long width, long height, const wchar_t *template_names,
                                double threshold, long dir, long strip_mode, long method, long color_mode,
                                std::wstring &retjson, long *ret) {
    std::vector<std::wstring> names;
    if (!opcv::bridge::ParseTemplateNames(template_names, names)) {
        retjson = L"{\"ok\":0,\"results\":[]}";
        internal::set_result(ret, 0L);
        return;
    }

    cv_match_capture_run(m_context->bkproc, m_context->image_proc, x, y, width, height, retjson, ret,
                         L"{\"ok\":0,\"results\":[]}",
                         [&](const opcv::ImageHandle &source, const opcv::Region &region, int origin_x, int origin_y) {
                             std::vector<opcv::NamedMatchResult> matches;
                             const bool ok = opcv::MatchAllTemplates(
                                 source, names, region, threshold, matches, static_cast<opcv::SearchDirection>(dir),
                                 static_cast<opcv::StripMode>(strip_mode), method,
                                 opcv::bridge::ParseColorMode(color_mode));
                             if (ok) {
                                 for (auto &match : matches) {
                                     match.match.x += origin_x;
                                     match.match.y += origin_y;
                                 }
                             }
                             retjson = opcv::bridge::BuildAllMatchesJson(matches, ok);
                             internal::set_result(ret, ok ? 1L : 0L);
                         });
}

void op::Op::CvFeatureMatchTemplate(long x, long y, long width, long height, const wchar_t *template_name,
                                   double threshold, std::wstring &retjson, long *ret) {
    if (template_name == nullptr) {
        retjson = L"{\"ok\":0}";
        internal::set_result(ret, 0L);
        return;
    }
    cv_match_capture_run(m_context->bkproc, m_context->image_proc, x, y, width, height, retjson, ret, L"{\"ok\":0}",
                         [&](const opcv::ImageHandle &source, const opcv::Region &region, int origin_x, int origin_y) {
                             opcv::MatchResult match;
                             const bool ok = opcv::FeatureMatchTemplate(source, template_name, region, threshold, match);
                             if (ok) {
                                 match.x += origin_x;
                                 match.y += origin_y;
                             }
                             retjson = opcv::bridge::BuildMatchJson(match, ok);
                             internal::set_result(ret, ok ? 1L : 0L);
                         });
}

void op::Op::CvEdgeMatchTemplate(long x, long y, long width, long height, const wchar_t *template_name, double threshold,
                                std::wstring &retjson, long *ret) {
    if (template_name == nullptr) {
        retjson = L"{\"ok\":0}";
        internal::set_result(ret, 0L);
        return;
    }
    cv_match_capture_run(m_context->bkproc, m_context->image_proc, x, y, width, height, retjson, ret, L"{\"ok\":0}",
                         [&](const opcv::ImageHandle &source, const opcv::Region &region, int origin_x, int origin_y) {
                             opcv::MatchResult match;
                             const bool ok = opcv::EdgeMatchTemplate(source, template_name, region, threshold, match);
                             if (ok) {
                                 match.x += origin_x;
                                 match.y += origin_y;
                             }
                             retjson = opcv::bridge::BuildMatchJson(match, ok);
                             internal::set_result(ret, ok ? 1L : 0L);
                         });
}

void op::Op::CvShapeMatchTemplate(long x, long y, long width, long height, const wchar_t *template_name,
                                 double threshold, std::wstring &retjson, long *ret) {
    if (template_name == nullptr) {
        retjson = L"{\"ok\":0}";
        internal::set_result(ret, 0L);
        return;
    }
    cv_match_capture_run(m_context->bkproc, m_context->image_proc, x, y, width, height, retjson, ret, L"{\"ok\":0}",
                         [&](const opcv::ImageHandle &source, const opcv::Region &region, int origin_x, int origin_y) {
                             opcv::MatchResult match;
                             const bool ok = opcv::ShapeMatchTemplate(source, template_name, region, threshold, match);
                             if (ok) {
                                 match.x += origin_x;
                                 match.y += origin_y;
                             }
                             retjson = opcv::bridge::BuildMatchJson(match, ok);
                             internal::set_result(ret, ok ? 1L : 0L);
                         });
}

void op::Op::CvMatchTemplateRot(long x, long y, long width, long height, const wchar_t *template_name,
                                const wchar_t *angles, double threshold, long method, long color_mode,
                                std::wstring &retjson, long *ret) {
    if (template_name == nullptr) {
        retjson = L"{\"ok\":0}";
        internal::set_result(ret, 0L);
        return;
    }
    std::vector<double> angle_values;
    if (!opcv::bridge::ParseAngleList(angles, angle_values)) {
        retjson = L"{\"ok\":0}";
        internal::set_result(ret, 0L);
        return;
    }

    cv_match_capture_run(m_context->bkproc, m_context->image_proc, x, y, width, height, retjson, ret, L"{\"ok\":0}",
                         [&](const opcv::ImageHandle &source, const opcv::Region &region, int origin_x, int origin_y) {
                             opcv::MatchResult match;
                             double best_angle = 0.0;
                             const bool ok =
                                 opcv::MatchTemplateRotated(source, template_name, region, angle_values, threshold,
                                                            match, best_angle, method,
                                                            opcv::bridge::ParseColorMode(color_mode));
                             if (ok) {
                                 match.x += origin_x;
                                 match.y += origin_y;
                             }
                             retjson = opcv::bridge::BuildRotatedMatchJson(match, best_angle, ok);
                             internal::set_result(ret, ok ? 1L : 0L);
                         });
}


// ---- 二维码（P1 新增 2026-10-15）：OpenCV QRCodeDetector/Encoder 直桥 ----
// 区域解码复用 cv_match_capture_run 截图链；文件读写宽路径走 filesystem 转换
// （与 OpenCvHelpers::loadImageFromFile / TemplateMatcher 保存同口径）。

void op::Op::DecodeQRCode(long x1, long y1, long x2, long y2, std::wstring &retstr) {
    retstr.clear();
    cv_match_capture_run(m_context->bkproc, m_context->image_proc, x1, y1, x2, y2, retstr, nullptr, L"",
                         [&](const opcv::ImageHandle &source, const opcv::Region &, int, int) {
                             const cv::Mat mat = opcv::helpers::createMatView(source);
                             if (mat.empty())
                                 return;
                             cv::QRCodeDetector detector;
                             std::vector<cv::Point> points;
                             cv::Mat straight;
                             const std::string decoded = detector.detectAndDecode(mat, points, straight);
                             if (!decoded.empty())
                                 retstr = utf8_decode(decoded);
                         });
}

void op::Op::DecodeQRCodeFromFile(const wchar_t *file_name, std::wstring &retstr) {
    retstr.clear();
    if (!file_name || !file_name[0])
        return;
    const cv::Mat mat = cv::imread(std::filesystem::path(file_name).string(), cv::IMREAD_COLOR);
    if (mat.empty())
        return;
    cv::QRCodeDetector detector;
    std::vector<cv::Point> points;
    cv::Mat straight;
    const std::string decoded = detector.detectAndDecode(mat, points, straight);
    if (!decoded.empty())
        retstr = utf8_decode(decoded);
}

void op::Op::CreateQRCode(const wchar_t *text, const wchar_t *file_name, long size, long *ret) {
    internal::set_result(ret, 0L);
    if (!text || !text[0] || !file_name || !file_name[0])
        return;
    const std::string utf8_text = utf8_encode(text);
    if (utf8_text.empty())
        return;

    cv::QRCodeEncoder::Params params;
    const cv::Ptr<cv::QRCodeEncoder> encoder = cv::QRCodeEncoder::create(params);
    cv::Mat qr;
    encoder->encode(utf8_text, qr); // 灰度小图，模块数随内容定
    if (qr.empty())
        return;

    // size 指定输出边长（<=0 用 256）；INTER_NEAREST 保持模块锐利不混色
    const int out_size = size > 0 ? static_cast<int>(size) : 256;
    cv::Mat out;
    cv::resize(qr, out, cv::Size(out_size, out_size), 0.0, 0.0, cv::INTER_NEAREST);
    if (cv::imwrite(std::filesystem::path(file_name).string(), out))
        internal::set_result(ret, 1L);
}
