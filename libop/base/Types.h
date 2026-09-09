#pragma once
#ifndef OP_BASE_TYPES_H_
#define OP_BASE_TYPES_H_
#include <Windows.h>
#include <assert.h>

#include <map>
#include <string>
#include <vector>

namespace op {

using uint = unsigned int;
using uchar = unsigned char;

using std::map;
using std::string;
using std::vector;
using std::wstring;

using bytearray = std::vector<uchar>;

struct point_t {
    int x, y;

    // OCR 结果按"阅读顺序"排序时的行高量化粒度(像素)。
    // 历史实现用 |y1-y2| < 9 的容差比较来判定"同一行",但容差比较不满足传递性,
    // 因此不是严格弱序(strict weak ordering)。把它交给 std::map / std::set /
    // std::sort 属于标准规定的未定义行为:
    //   a=(0,0) b=(0,8) c=(0,16) 时 a~b、b~c 等价却有 a<c,红黑树可能结构错乱。
    // 现改为"先把 y 量化成行号,再按 x 字典序",语义近似且严格弱序成立。
    // 需要按实际字号调整分行粒度时,修改这个值即可(0 或负数会被当作 1)。
    static inline int row_height = 9;

    point_t() : x(0), y(0) {
    }
    point_t(int x_, int y_) : x(x_), y(y_) {
    }

    // 行号:对 y 做 floor 除法,保证对负坐标也单调不减。
    int row() const {
        const int h = row_height > 0 ? row_height : 1;
        return y >= 0 ? y / h : -((h - 1 - y) / h);
    }

    bool operator<(const point_t &rhs) const {
        const int r1 = row();
        const int r2 = rhs.row();
        return r1 != r2 ? r1 < r2 : x < rhs.x;
    }
    bool operator==(const point_t &rhs) const {
        return x == rhs.x && y == rhs.y;
    }
};

using vpoint_t = std::vector<point_t>;
//(5,3) --> (2, 2, 1)
class NumberGen {
    int _q, _r;

  public:
    NumberGen(int n, int cnt) : _q(n / cnt), _r(n % cnt) {
    }
    int operator[](int idx) const {
        return idx < _r ? _q + 1 : _q;
    }
};

struct rect_t {
    rect_t() : x1(0), y1(0), x2(0), y2(0) {
    }
    rect_t(int x1_, int y1_, int x2_, int y2_) : x1(x1_), y1(y1_), x2(x2_), y2(y2_) {
    }
    int x1, y1;
    int x2, y2;
    int width() const {
        return x2 - x1;
    }
    int height() const {
        return y2 - y1;
    }
    int area() const {
        return width() * height();
    }
    rect_t &shrinkRect(int w, int h) {
        x2 -= w;
        y2 -= h;
        x2 += 1;
        y2 += 1;
        return *this;
    }
    bool valid() const {
        return 0 <= x1 && x1 < x2 && 0 <= y1 && y1 < y2;
    }

    void divideBlock(int count, bool vertical, std::vector<rect_t> &blocks) {
        assert(valid());

        // 防御：count<=0 时 NumberGen 内 n/cnt 除零(release 崩溃)；
        // count>span 时尾部产生 0 尺寸空块。统一早退/clamp 到 [1, span]。
        if (count <= 0) {
            blocks.clear();
            return;
        }
        const int span = vertical ? height() : width();
        if (count > span)
            count = span;
        assert(count > 0);
        blocks.resize(count);
        if (vertical) {
            NumberGen gen(height(), count);
            int basey = y1;
            for (int i = 0; i < count; ++i) {
                blocks[i] = rect_t(x1, basey, x2, basey + gen[i]);
                basey += gen[i];
            }

        } else {
            NumberGen gen(width(), count);
            int basex = x1;
            for (int i = 0; i < count; ++i) {
                blocks[i] = rect_t(basex, y1, basex + gen[i], y2);
                basex += gen[i];
            }
        }
        assert(blocks.back().x2 == x2);
        assert(blocks.back().y2 == y2);
    }
};

using vrect_t = std::vector<rect_t>;

struct point_desc_t {
    int id;
    point_t pos;
};

using vpoint_desc_t = std::vector<point_desc_t>;
// ocr result
struct ocr_rec_t {
    // BBox of the text
    point_t left_top;
    point_t right_bottom;
    // content of the text
    wstring text;
    // confidence of the text
    float confidence;
};

using vocr_rec_t = std::vector<ocr_rec_t>;

using byte = unsigned char;

struct yolo_rec_t {
    int class_id = -1;
    wstring label;
    point_t left_top;
    point_t right_bottom;
    float confidence = 0.0f;
};

using vyolo_rec_t = std::vector<yolo_rec_t>;

} // namespace op

#endif // OP_BASE_TYPES_H_
