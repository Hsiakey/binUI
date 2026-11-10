#pragma once
// ============================================================
// binUI SVG 路径几何（编译期与运行期共用，纯头文件，无第三方依赖）
//
// 为什么单独一个头文件：
//   · 编译期要知道路径的包围盒（节点 x/y/w/h）、要把 `d` 解析成折线、
//     要用同一份容差做细分；运行期拿到的是"归一化折线"，但线宽缩放、
//     命中测试、椭圆展开也要同一套数学。两边共用一份就不会出现
//     "编译器算出来的包围盒比渲染器画出来的大一圈"这种对不上的问题。
//   · 与 binui_paint.h 同一套路：几何算在这边，后端只负责 fillRect /
//     fillLine，保证 GDI / GL / D3D11 三端像素一致。
//
// 关键取舍：**曲线在编译期就展平成折线**（见 docs/svg.md 3.2）。
//   存折线 = 运行期零解析、零缓存失效；代价是极限放大时能看到折线。
//   容差按"编译期已知的设备像素"取 0.1px，图标放大 3 倍内肉眼无感。
//
// 坐标：M/L/H/V/C/S/Q/T/A/Z 全部支持，`A` 在解析时转成三次贝塞尔
//   （每段 ≤90°，误差 < 0.1%），所以下面的细分只有"三次/二次贝塞尔"两件事。
// ============================================================
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

namespace binui {

// 归一化坐标上限：0..65535 映射包围盒内 0..1（存储用 uint16，见 v15 路径段）
constexpr uint16_t kPathNormMax = 65535;
// 单条路径的点数上限：超了就让调用方提高容差重来，绝不画一半
constexpr int kPathMaxPointsDefault = 4096;

struct PathPointF {
    float x = 0.0f;
    float y = 0.0f;
};

// 折线表示的路径：点数组 + 子路径表
//   · 子路径之间首尾相接存在 pts 里，靠 contours 切分
//   · closed 记录原始 `Z`：填充时子路径一律按闭合处理（SVG 语义），
//     描边时只有 closed 的连接最后一段，否则留缺口
struct PathShape {
    struct Contour {
        uint32_t first = 0;    // pts 起始下标
        uint32_t count = 0;    // 点数（≥2）
        bool     closed = false;
    };
    std::vector<PathPointF> pts;
    std::vector<Contour> contours;
    float minX = 0.0f, minY = 0.0f, maxX = 0.0f, maxY = 0.0f;
    bool  truncated = false;   // 撞到点数上限：调用方应提高容差重试并告警

    bool Empty() const { return contours.empty() || pts.size() < 2; }
    float Width() const { return maxX - minX; }
    float Height() const { return maxY - minY; }

    void Clear() {
        pts.clear();
        contours.clear();
        minX = minY = maxX = maxY = 0.0f;
        truncated = false;
    }

    // 按当前点集重算包围盒（空路径得到全 0）
    void RecomputeBounds() {
        if (pts.empty()) {
            minX = minY = maxX = maxY = 0.0f;
            return;
        }
        minX = maxX = pts[0].x;
        minY = maxY = pts[0].y;
        for (const PathPointF& p : pts) {
            if (p.x < minX) minX = p.x;
            if (p.x > maxX) maxX = p.x;
            if (p.y < minY) minY = p.y;
            if (p.y > maxY) maxY = p.y;
        }
    }
};

// ---- 2×3 仿射（SVG matrix(a b c d e f) 同序） ----
//   x' = a*x + c*y + e
//   y' = b*x + d*y + f
struct PathMatrix {
    float a = 1.0f, b = 0.0f, c = 0.0f, d = 1.0f, e = 0.0f, f = 0.0f;
};

inline PathPointF PathApply(const PathMatrix& m, float x, float y) {
    return { m.a * x + m.c * y + m.e, m.b * x + m.d * y + m.f };
}

// 先应用 n 再应用 m（m ∘ n）
inline PathMatrix PathMul(const PathMatrix& m, const PathMatrix& n) {
    PathMatrix r;
    r.a = m.a * n.a + m.c * n.b;
    r.b = m.b * n.a + m.d * n.b;
    r.c = m.a * n.c + m.c * n.d;
    r.d = m.b * n.c + m.d * n.d;
    r.e = m.a * n.e + m.c * n.f + m.e;
    r.f = m.b * n.e + m.d * n.f + m.f;
    return r;
}

inline PathMatrix PathTranslate(float tx, float ty) {
    return { 1.0f, 0.0f, 0.0f, 1.0f, tx, ty };
}

inline PathMatrix PathScale(float sx, float sy) {
    return { sx, 0.0f, 0.0f, sy, 0.0f, 0.0f };
}

// rotate(deg) / rotate(deg cx cy)：SVG 的 rotate 绕原点，带中心时等价于 T(c)·R·T(-c)
inline PathMatrix PathRotate(float deg, float cx = 0.0f, float cy = 0.0f) {
    const float kPi = 3.14159265358979323846f;
    const float r = deg * kPi / 180.0f;
    const float co = std::cos(r), si = std::sin(r);
    PathMatrix m;
    m.a = co;  m.b = si;
    m.c = -si; m.d = co;
    m.e = cx - co * cx + si * cy;
    m.f = cy - si * cx - co * cy;
    return m;
}

// ============================================================
// 内部工具
// ============================================================
namespace path_detail {

inline bool PathIsSep(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == ',';
}

inline bool PathIsDigit(char c) { return c >= '0' && c <= '9'; }

// 点到线段的距离（弦退化成一个点时退化为点到点距离）
inline float PathDistToSegment(float px, float py, float x0, float y0, float x1, float y1) {
    const float dx = x1 - x0, dy = y1 - y0;
    const float len2 = dx * dx + dy * dy;
    if (len2 <= 1e-12f) {
        const float ax = px - x0, ay = py - y0;
        return std::sqrt(ax * ax + ay * ay);
    }
    float t = ((px - x0) * dx + (py - y0) * dy) / len2;
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    const float qx = x0 + t * dx, qy = y0 + t * dy;
    const float ex = px - qx, ey = py - qy;
    return std::sqrt(ex * ex + ey * ey);
}

// `d` 属性解析器：手写扫描器。
// 不能复用编译器的 ParseLenPct —— 它遇到 "M10 10" 这种首字符非数字直接告警回退 0，
// 而路径数据里命令字母和数字是紧贴的（"M10 10L20 20" 合法）。
class PathScanner {
public:
    explicit PathScanner(const std::string& text) : s_(text) {}

    void SkipSep() {
        while (pos_ < s_.size() && PathIsSep(s_[pos_])) pos_++;
    }

    bool AtEnd() {
        SkipSep();
        return pos_ >= s_.size();
    }

    // 不消耗地看下一个非空白字符（没有则返回 '\0'）
    char Peek() {
        SkipSep();
        return pos_ < s_.size() ? s_[pos_] : '\0';
    }

    // 读取一个浮点数。返回 false = 这里没有数（调用方决定是否报错）。
    // 支持 "+1" "-.5" "1." "1e-3"；"-" 与下一个数之间不加空格时也合法（"1-2" → 1, -2）。
    bool Number(float& out) {
        SkipSep();
        if (pos_ >= s_.size()) return false;
        const size_t start = pos_;
        if (s_[pos_] == '+' || s_[pos_] == '-') pos_++;
        bool digits = false, dot = false;
        while (pos_ < s_.size()) {
            const char c = s_[pos_];
            if (PathIsDigit(c)) { digits = true; pos_++; continue; }
            if (c == '.' && !dot) { dot = true; pos_++; continue; }
            break;
        }
        if (!digits) { pos_ = start; return false; }   // 只有符号 / 单个 '.' = 不是数
        // 指数部分：只有后面真的跟着数字才算（"1e" 当成 1，让 'e' 留给调用方报错）
        if (pos_ < s_.size() && (s_[pos_] == 'e' || s_[pos_] == 'E')) {
            const size_t mark = pos_;
            pos_++;
            if (pos_ < s_.size() && (s_[pos_] == '+' || s_[pos_] == '-')) pos_++;
            bool expDigits = false;
            while (pos_ < s_.size() && PathIsDigit(s_[pos_])) { expDigits = true; pos_++; }
            if (!expDigits) pos_ = mark;
        }
        const std::string token = s_.substr(start, pos_ - start);
        out = static_cast<float>(std::strtod(token.c_str(), nullptr));
        return true;
    }

    // 弧标志位：SVG 允许 "a1 1 0 0110 10" 这种紧贴写法，所以只能读一个字符
    bool Flag(int& out) {
        SkipSep();
        if (pos_ < s_.size() && (s_[pos_] == '0' || s_[pos_] == '1')) {
            out = s_[pos_] - '0';
            pos_++;
            return true;
        }
        return false;
    }

    char Alpha() {
        SkipSep();
        if (pos_ < s_.size() && std::isalpha(static_cast<unsigned char>(s_[pos_])))
            return s_[pos_];
        return '\0';
    }

    void Step() { pos_++; }
    size_t Pos() const { return pos_; }

private:
    const std::string& s_;
    size_t pos_ = 0;
};

// 三次贝塞尔递归细分：控制点到弦的距离都在 tol 内就收一段。
// 用"控制点距离"而不是"曲线中点距离"当判据，是保守方向（多分几段而不是少分）。
inline void PathFlattenCubic(PathShape& out, float x0, float y0, float x1, float y1,
                             float x2, float y2, float x3, float y3,
                             float tol, int depth, int maxPoints) {
    if (static_cast<int>(out.pts.size()) >= maxPoints) {
        out.truncated = true;
        return;
    }
    const bool flat = PathDistToSegment(x1, y1, x0, y0, x3, y3) <= tol &&
                      PathDistToSegment(x2, y2, x0, y0, x3, y3) <= tol;
    if (flat || depth <= 0) {
        out.pts.push_back({ x3, y3 });
        return;
    }
    // de Casteljau 对分
    const float x01 = (x0 + x1) * 0.5f, y01 = (y0 + y1) * 0.5f;
    const float x12 = (x1 + x2) * 0.5f, y12 = (y1 + y2) * 0.5f;
    const float x23 = (x2 + x3) * 0.5f, y23 = (y2 + y3) * 0.5f;
    const float x012 = (x01 + x12) * 0.5f, y012 = (y01 + y12) * 0.5f;
    const float x123 = (x12 + x23) * 0.5f, y123 = (y12 + y23) * 0.5f;
    const float xm = (x012 + x123) * 0.5f, ym = (y012 + y123) * 0.5f;
    PathFlattenCubic(out, x0, y0, x01, y01, x012, y012, xm, ym, tol, depth - 1, maxPoints);
    PathFlattenCubic(out, xm, ym, x123, y123, x23, y23, x3, y3, tol, depth - 1, maxPoints);
}

// 二次贝塞尔：先精确升阶成三次，再复用上面那条路（省一份细分子程序）
inline void PathFlattenQuad(PathShape& out, float x0, float y0, float qx, float qy,
                            float x1, float y1, float tol, int depth, int maxPoints) {
    const float c1x = x0 + (qx - x0) * 2.0f / 3.0f, c1y = y0 + (qy - y0) * 2.0f / 3.0f;
    const float c2x = x1 + (qx - x1) * 2.0f / 3.0f, c2y = y1 + (qy - y1) * 2.0f / 3.0f;
    PathFlattenCubic(out, x0, y0, c1x, c1y, c2x, c2y, x1, y1, tol, depth, maxPoints);
}

// 椭圆弧 → 三次贝塞尔（SVG 1.1 附录 F.6.5 的端点参数化）。
// 每段不超过 90°，四段拼一个整圆时误差 < 0.02%，再细分就看不出来了。
inline void PathArcToCubics(PathShape& out, float x1, float y1, float rx, float ry,
                            float rotDeg, bool largeArc, bool sweep, float x2, float y2,
                            float tol, int maxPoints) {
    const float kPi = 3.14159265358979323846f;
    if (rx == 0.0f || ry == 0.0f) {          // 规范：半径为 0 当直线
        out.pts.push_back({ x2, y2 });
        return;
    }
    rx = std::fabs(rx);
    ry = std::fabs(ry);
    const float phi = rotDeg * kPi / 180.0f;
    const float cosPhi = std::cos(phi), sinPhi = std::sin(phi);

    // 1) 把端点换算到"圆心在原点、无旋转"的坐标系
    const float dx2 = (x1 - x2) * 0.5f, dy2 = (y1 - y2) * 0.5f;
    const float x1p =  cosPhi * dx2 + sinPhi * dy2;
    const float y1p = -sinPhi * dx2 + cosPhi * dy2;

    // 2) 半径太小装不下端点：等比放大到刚好
    float rx2 = rx * rx, ry2 = ry * ry;
    const float x1p2 = x1p * x1p, y1p2 = y1p * y1p;
    const float lam = x1p2 / rx2 + y1p2 / ry2;
    if (lam > 1.0f) {
        const float s = std::sqrt(lam);
        rx *= s; ry *= s;
        rx2 = rx * rx; ry2 = ry * ry;
    }

    // 3) 圆心
    float num = rx2 * ry2 - rx2 * y1p2 - ry2 * x1p2;
    if (num < 0.0f) num = 0.0f;
    const float den = rx2 * y1p2 + ry2 * x1p2;
    float coef = den > 0.0f ? std::sqrt(num / den) : 0.0f;
    if (largeArc == sweep) coef = -coef;
    const float cxp =  coef * (rx * y1p / ry);
    const float cyp = -coef * (ry * x1p / rx);
    const float cx = cosPhi * cxp - sinPhi * cyp + (x1 + x2) * 0.5f;
    const float cy = sinPhi * cxp + cosPhi * cyp + (y1 + y2) * 0.5f;

    auto angleOf = [](float ux, float uy, float vx, float vy) {
        const float dot = ux * vx + uy * vy;
        const float len = std::sqrt((ux * ux + uy * uy) * (vx * vx + vy * vy));
        if (len <= 1e-12f) return 0.0f;
        float a = std::acos(std::max(-1.0f, std::min(1.0f, dot / len)));
        if (ux * vy - uy * vx < 0.0f) a = -a;
        return a;
    };

    const float ux = (x1p - cxp) / rx, uy = (y1p - cyp) / ry;
    const float vx = (-x1p - cxp) / rx, vy = (-y1p - cyp) / ry;
    const float theta1 = angleOf(1.0f, 0.0f, ux, uy);
    float dTheta = angleOf(ux, uy, vx, vy);
    if (!sweep && dTheta > 0.0f) dTheta -= 2.0f * kPi;
    if (sweep && dTheta < 0.0f) dTheta += 2.0f * kPi;

    const int segs = std::max(1, static_cast<int>(std::ceil(std::fabs(dTheta) / (kPi * 0.5f))));
    const float delta = dTheta / static_cast<float>(segs);

    auto pointAt = [&](float t) {
        const float ct = std::cos(t), st = std::sin(t);
        return PathPointF{ cx + rx * ct * cosPhi - ry * st * sinPhi,
                           cy + rx * ct * sinPhi + ry * st * cosPhi };
    };
    auto derivAt = [&](float t) {
        const float ct = std::cos(t), st = std::sin(t);
        return PathPointF{ -rx * st * cosPhi - ry * ct * sinPhi,
                           -rx * st * sinPhi + ry * ct * cosPhi };
    };

    float t0 = theta1;
    for (int i = 0; i < segs; i++) {
        const float t1 = t0 + delta;
        const float k = std::sin(delta) * (std::sqrt(4.0f + 3.0f *
                       std::pow(std::tan(delta * 0.5f), 2.0f)) - 1.0f) / 3.0f;
        const PathPointF p0 = pointAt(t0), p3 = pointAt(t1);
        const PathPointF d0 = derivAt(t0), d3 = derivAt(t1);
        PathFlattenCubic(out, p0.x, p0.y,
                         p0.x + k * d0.x, p0.y + k * d0.y,
                         p3.x - k * d3.x, p3.y - k * d3.y,
                         p3.x, p3.y, tol, 12, maxPoints);
        if (out.truncated) return;
        t0 = t1;
    }
}

}  // namespace path_detail

// ============================================================
// `d` 属性 → 折线
//
// tolerance 是用户单位下的最大弦高误差（≤0 取 0.1）。
// maxPoints 撞上限时 out.truncated = true，调用方应提高容差重来（宁可略圆，不要画一半）。
// 语法错误返回 false 并在 err 里给出位置与原因；已经解析的部分保留在 out 里，
// 但调用方在失败时应丢弃（半条路径画出来比不画更让人困惑）。
// ============================================================
inline bool ParsePathData(const std::string& d, float tolerance, PathShape& out,
                          std::string* err = nullptr, int maxPoints = kPathMaxPointsDefault) {
    using namespace path_detail;
    out.Clear();
    if (tolerance <= 0.0f) tolerance = 0.1f;
    if (maxPoints <= 16) maxPoints = 16;

    PathScanner sc(d);
    float cx = 0.0f, cy = 0.0f;          // 当前点
    float sx = 0.0f, sy = 0.0f;          // 当前子路径起点（Z 回这里）
    float lastC2x = 0.0f, lastC2y = 0.0f; // 上一段三次曲线的第二控制点（S 的反射源）
    float lastQx = 0.0f, lastQy = 0.0f;   // 上一段二次曲线的控制点（T 的反射源）
    bool  prevCubic = false, prevQuad = false;
    bool  hasContour = false;
    char  cmd = '\0';

    auto fail = [&](const char* why) {
        if (err) {
            *err = std::string(why) + "（位置 " + std::to_string(sc.Pos()) + "，命令行 \"" +
                   d.substr(0, std::min<size_t>(sc.Pos() + 8, d.size())) + "\"）";
        }
        return false;
    };
    // 新起一条子路径：只有一个点的多边形没有面积，填充时会被丢掉，这里先记着
    auto beginContour = [&](float x, float y) {
        if (static_cast<int>(out.pts.size()) >= maxPoints) { out.truncated = true; return; }
        PathShape::Contour c;
        c.first = static_cast<uint32_t>(out.pts.size());
        c.count = 0;
        out.contours.push_back(c);
        out.pts.push_back({ x, y });
        hasContour = true;
    };
    // 追加点：连续重复点（细分边界处常见）不重复入表，省空间也避免零长边
    auto addPoint = [&](float x, float y) {
        if (static_cast<int>(out.pts.size()) >= maxPoints) { out.truncated = true; return; }
        if (!out.pts.empty()) {
            const PathPointF& last = out.pts.back();
            if (std::fabs(last.x - x) < 1e-4f && std::fabs(last.y - y) < 1e-4f) return;
        }
        out.pts.push_back({ x, y });
    };

    while (!sc.AtEnd()) {
        const char c = sc.Peek();
        if (std::isalpha(static_cast<unsigned char>(c))) {
            cmd = c;
            sc.Step();
            if (cmd == 'Z' || cmd == 'z') {
                if (hasContour && !out.contours.empty()) {
                    out.contours.back().closed = true;
                    cx = sx; cy = sy;
                }
                // 规范：闭合之后如果再出现绘制命令，从子路径起点新开一条
                hasContour = false;
                prevCubic = prevQuad = false;
                continue;
            }
        } else if (cmd == '\0') {
            return fail("路径数据必须以命令字母开头");
        }

        const char lc = (cmd >= 'A' && cmd <= 'Z')
                        ? static_cast<char>(cmd - 'A' + 'a') : cmd;
        const bool rel = (cmd >= 'a' && cmd <= 'z');

        if (lc == 'm' || lc == 'l') {
            float x = 0.0f, y = 0.0f;
            if (!sc.Number(x) || !sc.Number(y)) return fail("M/L 缺少坐标");
            const float ax = rel ? cx + x : x;
            const float ay = rel ? cy + y : y;
            if (lc == 'm') {
                beginContour(ax, ay);
                sx = ax; sy = ay;
                cmd = rel ? 'l' : 'L';   // 同一命令后跟的坐标对按 lineto 处理
            } else {
                if (!hasContour) { beginContour(cx, cy); sx = cx; sy = cy; }
                addPoint(ax, ay);
            }
            if (out.truncated) return true;
            cx = ax; cy = ay;
            prevCubic = prevQuad = false;
        } else if (lc == 'h' || lc == 'v') {
            float v = 0.0f;
            if (!sc.Number(v)) return fail("H/V 缺少坐标");
            if (!hasContour) { beginContour(cx, cy); sx = cx; sy = cy; }
            if (lc == 'h') cx = rel ? cx + v : v;
            else           cy = rel ? cy + v : v;
            addPoint(cx, cy);
            if (out.truncated) return true;
            prevCubic = prevQuad = false;
        } else if (lc == 'c') {
            float v[6];
            for (float& f : v) if (!sc.Number(f)) return fail("C 缺少控制点");
            if (!hasContour) { beginContour(cx, cy); sx = cx; sy = cy; }
            const float bx = cx, by = cy;
            const float x1 = rel ? bx + v[0] : v[0], y1 = rel ? by + v[1] : v[1];
            const float x2 = rel ? bx + v[2] : v[2], y2 = rel ? by + v[3] : v[3];
            const float x3 = rel ? bx + v[4] : v[4], y3 = rel ? by + v[5] : v[5];
            PathFlattenCubic(out, bx, by, x1, y1, x2, y2, x3, y3, tolerance, 14, maxPoints);
            if (out.truncated) return true;
            cx = x3; cy = y3;
            lastC2x = x2; lastC2y = y2;
            prevCubic = true; prevQuad = false;
        } else if (lc == 's') {
            float v[4];
            for (float& f : v) if (!sc.Number(f)) return fail("S 缺少控制点");
            if (!hasContour) { beginContour(cx, cy); sx = cx; sy = cy; }
            const float bx = cx, by = cy;
            const float x1 = prevCubic ? 2.0f * bx - lastC2x : bx;
            const float y1 = prevCubic ? 2.0f * by - lastC2y : by;
            const float x2 = rel ? bx + v[0] : v[0], y2 = rel ? by + v[1] : v[1];
            const float x3 = rel ? bx + v[2] : v[2], y3 = rel ? by + v[3] : v[3];
            PathFlattenCubic(out, bx, by, x1, y1, x2, y2, x3, y3, tolerance, 14, maxPoints);
            if (out.truncated) return true;
            cx = x3; cy = y3;
            lastC2x = x2; lastC2y = y2;
            prevCubic = true; prevQuad = false;
        } else if (lc == 'q') {
            float v[4];
            for (float& f : v) if (!sc.Number(f)) return fail("Q 缺少控制点");
            if (!hasContour) { beginContour(cx, cy); sx = cx; sy = cy; }
            const float bx = cx, by = cy;
            const float qx = rel ? bx + v[0] : v[0], qy = rel ? by + v[1] : v[1];
            const float x1 = rel ? bx + v[2] : v[2], y1 = rel ? by + v[3] : v[3];
            PathFlattenQuad(out, bx, by, qx, qy, x1, y1, tolerance, 14, maxPoints);
            if (out.truncated) return true;
            cx = x1; cy = y1;
            lastQx = qx; lastQy = qy;
            prevQuad = true; prevCubic = false;
        } else if (lc == 't') {
            float v[2];
            for (float& f : v) if (!sc.Number(f)) return fail("T 缺少终点");
            if (!hasContour) { beginContour(cx, cy); sx = cx; sy = cy; }
            const float bx = cx, by = cy;
            const float qx = prevQuad ? 2.0f * bx - lastQx : bx;
            const float qy = prevQuad ? 2.0f * by - lastQy : by;
            const float x1 = rel ? bx + v[0] : v[0], y1 = rel ? by + v[1] : v[1];
            PathFlattenQuad(out, bx, by, qx, qy, x1, y1, tolerance, 14, maxPoints);
            if (out.truncated) return true;
            cx = x1; cy = y1;
            lastQx = qx; lastQy = qy;
            prevQuad = true; prevCubic = false;
        } else if (lc == 'a') {
            float rx = 0.0f, ry = 0.0f, rot = 0.0f;
            int large = 0, sweep = 0;
            float x1 = 0.0f, y1 = 0.0f;
            if (!sc.Number(rx) || !sc.Number(ry) || !sc.Number(rot)) return fail("A 缺少半径/旋转角");
            if (!sc.Flag(large) || !sc.Flag(sweep)) return fail("A 的 large-arc-flag / sweep-flag 必须是 0 或 1");
            if (!sc.Number(x1) || !sc.Number(y1)) return fail("A 缺少终点");
            if (!hasContour) { beginContour(cx, cy); sx = cx; sy = cy; }
            const float ex = rel ? cx + x1 : x1;
            const float ey = rel ? cy + y1 : y1;
            PathArcToCubics(out, cx, cy, rx, ry, rot, large != 0, sweep != 0, ex, ey,
                            tolerance, maxPoints);
            if (out.truncated) return true;
            cx = ex; cy = ey;
            prevCubic = prevQuad = false;
        } else {
            return fail("未知的命令字母");
        }
    }

    // 收尾：beginContour 记下的 first 天然升序，子路径区间 = [first, 下一条 first)。
    // 只有一个点的子路径（M 后面什么都没画）没有面积，丢掉。
    std::vector<PathShape::Contour> kept;
    for (size_t i = 0; i < out.contours.size(); i++) {
        const uint32_t begin = out.contours[i].first;
        const uint32_t end = (i + 1 < out.contours.size())
                             ? out.contours[i + 1].first
                             : static_cast<uint32_t>(out.pts.size());
        if (end <= begin || end - begin < 2) continue;
        PathShape::Contour c;
        c.first = begin;
        c.count = end - begin;
        c.closed = out.contours[i].closed;
        kept.push_back(c);
    }
    out.contours.swap(kept);
    if (out.contours.empty()) out.pts.clear();
    out.RecomputeBounds();
    return true;
}

// ============================================================
// 其它图元 → 折线（椭圆没有 `d`，但走同一套细分才有一致的容差口径）
// ============================================================
// 椭圆（走同一套细分容差）
inline void AppendEllipse(PathShape& out, float cx, float cy, float rx, float ry,
                          float tolerance = 0.1f,
                          int maxPoints = kPathMaxPointsDefault) {
    using namespace path_detail;
    if (tolerance <= 0.0f) tolerance = 0.1f;
    if (rx <= 0.0f || ry <= 0.0f) return;
    PathShape::Contour c;
    c.first = static_cast<uint32_t>(out.pts.size());
    c.closed = true;
    out.contours.push_back(c);
    // 四段三次贝塞尔拼椭圆（kappa = 4/3·tan(π/8) ≈ 0.5523）
    const float k = 0.5522847498f;
    const float ex = rx * k, ey = ry * k;
    out.pts.push_back({ cx + rx, cy });
    PathFlattenCubic(out, cx + rx, cy, cx + rx, cy + ey, cx + ex, cy + ry, cx, cy + ry,
                     tolerance, 14, maxPoints);
    PathFlattenCubic(out, cx, cy + ry, cx - ex, cy + ry, cx - rx, cy + ey, cx - rx, cy,
                     tolerance, 14, maxPoints);
    PathFlattenCubic(out, cx - rx, cy, cx - rx, cy - ey, cx - ex, cy - ry, cx, cy - ry,
                     tolerance, 14, maxPoints);
    PathFlattenCubic(out, cx, cy - ry, cx + ex, cy - ry, cx + rx, cy - ey, cx + rx, cy,
                     tolerance, 14, maxPoints);
    // 最后一段的收尾点与起点重合，去掉重复
    if (out.pts.size() > 1) {
        const PathPointF& a = out.pts.back();
        const PathPointF& b = out.pts[c.first];
        if (std::fabs(a.x - b.x) < 1e-4f && std::fabs(a.y - b.y) < 1e-4f)
            out.pts.pop_back();
    }
    if (out.contours.back().first < out.pts.size()) {
        out.contours.back().count =
            static_cast<uint32_t>(out.pts.size()) - out.contours.back().first;
    }
    out.RecomputeBounds();
}

// 圆/椭圆的圆弧段（角度制，从 a0 扫到 a1），按容差自适应分段。
// 只用于圆角矩形；任意椭圆弧走 ParsePathData 的 `A` 命令（三次贝塞尔那条路）。
inline void AppendEllipseArc(PathShape& out, float cx, float cy, float r,
                             float a0deg, float a1deg,
                             float tolerance = 0.1f, int maxPoints = kPathMaxPointsDefault) {
    if (r <= 0.0f) return;
    const float kPi = 3.14159265358979323846f;
    const float a0 = a0deg * kPi / 180.0f;
    const float a1 = a1deg * kPi / 180.0f;
    // 弦高 ≤ tolerance 时的最大圆心角：θ ≈ 2·acos(1 - tol/r)
    float step = 0.35f;
    if (tolerance > 0.0f && tolerance < r) {
        step = 2.0f * std::acos(std::max(-1.0f, std::min(1.0f, 1.0f - tolerance / r)));
        step = std::max(0.05f, std::min(step, 0.35f));
    }
    const int n = std::max(1, static_cast<int>(std::ceil(std::fabs(a1 - a0) / step)));
    for (int i = 1; i <= n; i++) {
        if (static_cast<int>(out.pts.size()) >= maxPoints) break;
        const float t = a0 + (a1 - a0) * static_cast<float>(i) / static_cast<float>(n);
        out.pts.push_back({ cx + r * std::cos(t), cy + r * std::sin(t) });
    }
}

// 圆角矩形：四段直线 + 四个 1/4 圆弧（rx<=0 时退化成四段直线）。
// 用来把 <rect rx> 在"带描边"时编成路径 —— 轻量映射（LIST + radius）只有填充，
// 描边必须走 path 才有 fill+stroke 并存。
inline void AppendRoundedRect(PathShape& out, float x, float y, float w, float h,
                              float rx, float tolerance = 0.1f,
                              int maxPoints = kPathMaxPointsDefault) {
    if (w <= 0.0f || h <= 0.0f) return;
    const float r = std::max(0.0f, std::min(rx, std::min(w, h) * 0.5f));
    PathShape::Contour c;
    c.first = static_cast<uint32_t>(out.pts.size());
    c.closed = true;
    out.contours.push_back(c);
    auto push = [&](float px, float py) { out.pts.push_back({ px, py }); };
    if (r <= 0.0f) {
        push(x, y);
        push(x + w, y);
        push(x + w, y + h);
        push(x, y + h);
    } else {
        // 上边 → 右上弧 → 右边 → 右下弧 → 下边 → 左下弧 → 左边 → 左上弧
        push(x + r, y);
        push(x + w - r, y);
        AppendEllipseArc(out, x + w - r, y + r, r, -90.0f, 0.0f, tolerance, maxPoints);
        push(x + w, y + h - r);
        AppendEllipseArc(out, x + w - r, y + h - r, r, 0.0f, 90.0f, tolerance, maxPoints);
        push(x + r, y + h);
        AppendEllipseArc(out, x + r, y + h - r, r, 90.0f, 180.0f, tolerance, maxPoints);
        push(x, y + r);
        AppendEllipseArc(out, x + r, y + r, r, 180.0f, 270.0f, tolerance, maxPoints);
    }
    // 最后一个弧的终点回到起点附近，去掉重复点
    if (out.pts.size() > c.first + 1) {
        const PathPointF& a = out.pts.back();
        const PathPointF& b = out.pts[c.first];
        if (std::fabs(a.x - b.x) < 1e-3f && std::fabs(a.y - b.y) < 1e-3f) out.pts.pop_back();
    }
    out.contours.back().count = static_cast<uint32_t>(out.pts.size()) - c.first;
    out.RecomputeBounds();
}

// ============================================================
// stroke-dasharray：按弧长把子路径切成"实线段"，编译期做完
//
// 运行期描边就是"逐段画线"，没有虚线概念；与其给三端加虚线逻辑，
// 不如在编译期把折线切成一段段实线子路径 —— 运行期照常画，零改动。
// 代价：虚线间距按**用户单位**在编译期固定，运行期缩放时整条虚线一起缩放
// （SVG 语义本来就是随用户单位缩放，所以这是对的）。
// 返回 false 表示"虚线把整条路径吃光了"（调用方应把描边关掉）。
// ============================================================
inline bool ApplyDashPattern(PathShape& s, const std::vector<float>& pattern) {
    if (pattern.empty() || s.Empty()) return false;
    std::vector<float> pat;
    for (float v : pattern) pat.push_back(v > 0.0f ? v : 0.0f);
    float total = 0.0f;
    for (float v : pat) total += v;
    if (total <= 1e-4f) return false;   // 全 0 = 不画（SVG 规范）

    PathShape out;
    size_t patIdx = 0;
    float remain = pat[0];
    bool on = true;
    if (remain <= 0.0f) {               // 开头是 0 长度段：直接跳过
        for (size_t k = 1; k <= pat.size(); k++) {
            const size_t i2 = k % pat.size();
            if (pat[i2] > 0.0f) { patIdx = i2; remain = pat[i2]; on = (i2 % 2) == 0; break; }
        }
    }

    for (const PathShape::Contour& c : s.contours) {
        if (c.count < 2) continue;
        const size_t segs = c.closed ? c.count : c.count - 1;
        PathPointF cur = s.pts[c.first];
        if (on) {
            PathShape::Contour nc;
            nc.first = static_cast<uint32_t>(out.pts.size());
            out.pts.push_back(cur);
            nc.count = 1;
            out.contours.push_back(nc);
        }
        for (size_t k = 0; k < segs; k++) {
            const PathPointF a = s.pts[c.first + k];
            const PathPointF b = s.pts[c.first + (k + 1) % c.count];
            float segLen = std::sqrt((b.x - a.x) * (b.x - a.x) + (b.y - a.y) * (b.y - a.y));
            PathPointF p = a;
            while (segLen > 1e-6f) {
                const float take = std::min(remain, segLen);
                const float t = take / segLen;
                const PathPointF q{ p.x + (b.x - p.x) * t, p.y + (b.y - p.y) * t };
                if (on) out.pts.push_back(q);
                segLen -= take;
                remain -= take;
                p = q;
                if (remain <= 1e-6f) {
                    if (on) out.contours.back().count =
                        static_cast<uint32_t>(out.pts.size()) - out.contours.back().first;
                    patIdx = (patIdx + 1) % pat.size();
                    on = (patIdx % 2) == 0;
                    remain = pat[patIdx];
                    if (on && remain > 0.0f) {
                        PathShape::Contour nc2;
                        nc2.first = static_cast<uint32_t>(out.pts.size());
                        out.pts.push_back(p);
                        nc2.count = 1;
                        out.contours.push_back(nc2);
                    }
                }
            }
        }
        if (on && !out.contours.empty())       // 收尾：把最后一段的点数补上
            out.contours.back().count =
                static_cast<uint32_t>(out.pts.size()) - out.contours.back().first;
    }
    // 丢掉只剩一个点的碎片（零长实线段）
    std::vector<PathShape::Contour> kept;
    for (const PathShape::Contour& c : out.contours) {
        if (c.count < 2) continue;
        PathShape::Contour nc;
        nc.first = c.first;
        nc.count = c.count;
        kept.push_back(nc);
    }
    if (kept.empty()) return false;
    out.contours.swap(kept);
    out.RecomputeBounds();
    s = std::move(out);
    return true;
}

// 对整条路径套仿射（viewBox 映射 / <g transform> 都在编译期做完）
inline void TransformShape(PathShape& s, const PathMatrix& m) {
    for (PathPointF& p : s.pts) p = PathApply(m, p.x, p.y);
    if (!s.pts.empty()) s.RecomputeBounds();
}

// 仿射的等比缩放系数（面积比的平方根）：描边线宽、圆半径要按它缩放。
// 非等比拉伸时取几何平均 —— 与"线宽只跟纵向比例"的既有权衡一致。
inline float PathScaleFactor(const PathMatrix& m) {
    const float det = std::fabs(m.a * m.d - m.b * m.c);
    return std::sqrt(det);
}

// ============================================================
// SVG transform 属性 → 矩阵
//
// 支持 translate / scale / rotate / skewX / skewY / matrix，可按空格/逗号串联，
// 多个变换从**左到右依次作用**（SVG 语义：左边先作用在坐标上，等价于
// 矩阵按 m1 * m2 乘）。解析失败返回 false 并保留 out 不变，调用方告警。
// ============================================================
inline bool ParseSvgTransform(const std::string& text, PathMatrix& out,
                              std::string* err = nullptr) {
    PathMatrix acc;   // 单位阵
    size_t i = 0;
    const size_t n = text.size();
    auto skipSep = [&]() {
        while (i < n && (text[i] == ' ' || text[i] == '\t' || text[i] == '\n' ||
                         text[i] == '\r' || text[i] == ',')) i++;
    };
    auto readNumber = [&](float& v) -> bool {
        skipSep();
        if (i >= n) return false;
        const size_t start = i;
        if (text[i] == '+' || text[i] == '-') i++;
        bool digits = false, dot = false;
        while (i < n) {
            const char c = text[i];
            if (c >= '0' && c <= '9') { digits = true; i++; continue; }
            if (c == '.' && !dot) { dot = true; i++; continue; }
            break;
        }
        if (!digits) { i = start; return false; }
        v = static_cast<float>(std::strtod(text.substr(start, i - start).c_str(), nullptr));
        return true;
    };
    auto fail = [&](const char* why) {
        if (err) *err = std::string(why) + "（位置 " + std::to_string(i) + "）";
        return false;
    };

    for (;;) {
        skipSep();
        if (i >= n) break;
        const size_t nameStart = i;
        while (i < n && ((text[i] >= 'a' && text[i] <= 'z') || (text[i] >= 'A' && text[i] <= 'Z'))) i++;
        if (i == nameStart) return fail("transform 里有无法识别的内容");
        std::string name;
        for (size_t k = nameStart; k < i; k++)
            name += static_cast<char>(std::tolower(static_cast<unsigned char>(text[k])));
        if (i >= n || text[i] != '(') return fail("transform 函数缺少 '('");
        i++;   // '('
        PathMatrix m;
        if (name == "translate") {
            float tx = 0.0f, ty = 0.0f;
            if (!readNumber(tx)) return fail("translate 缺少参数");
            if (!readNumber(ty)) ty = 0.0f;
            m = PathTranslate(tx, ty);
        } else if (name == "scale") {
            float sx = 1.0f, sy = 1.0f;
            if (!readNumber(sx)) return fail("scale 缺少参数");
            if (!readNumber(sy)) sy = sx;
            m = PathScale(sx, sy);
        } else if (name == "rotate") {
            float a = 0.0f, cx = 0.0f, cy = 0.0f;
            if (!readNumber(a)) return fail("rotate 缺少角度");
            if (readNumber(cx) && readNumber(cy)) m = PathRotate(a, cx, cy);
            else m = PathRotate(a);
        } else if (name == "matrix") {
            float v[6];
            for (float& f : v) if (!readNumber(f)) return fail("matrix 需要 6 个数");
            m = { v[0], v[1], v[2], v[3], v[4], v[5] };
        } else if (name == "skewx" || name == "skewy") {
            float a = 0.0f;
            if (!readNumber(a)) return fail("skew 缺少角度");
            const float t = std::tan(a * 3.14159265358979323846f / 180.0f);
            if (name == "skewx") m = { 1.0f, 0.0f, t, 1.0f, 0.0f, 0.0f };
            else                 m = { 1.0f, t, 0.0f, 1.0f, 0.0f, 0.0f };
        } else {
            return fail("不支持的 transform 函数");
        }
        skipSep();
        if (i >= n || text[i] != ')') return fail("transform 函数缺少 ')'");
        i++;
        acc = PathMul(acc, m);   // 左边的先作用
    }
    out = acc;
    return true;
}

// viewBox → 内容盒的映射矩阵。
//   preserveAspectRatio 默认 "xMidYMid meet"：等比缩放 + 居中（完整显示）
//   "none"：sx/sy 各自拉伸（不做等比）
// 只解析是否以 none 开头，其余对齐方式一律按居中处理并告警一次（调用方负责告警）。
inline bool BuildViewBoxMatrix(float vx, float vy, float vw, float vh,
                               float boxW, float boxH, const std::string& preserve,
                               PathMatrix& out) {
    if (vw <= 0.0f || vh <= 0.0f || boxW <= 0.0f || boxH <= 0.0f) return false;
    float sx = boxW / vw, sy = boxH / vh;
    const std::string p = [&] {
        std::string s;
        for (char c : preserve) s += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    }();
    if (p.rfind("none", 0) == 0) {
        // 非等比：直接铺满
    } else {
        const float k = std::min(sx, sy);
        sx = sy = k;
    }
    const float tx = (boxW - vw * sx) * 0.5f - vx * sx;
    const float ty = (boxH - vh * sy) * 0.5f - vy * sy;
    out = { sx, 0.0f, 0.0f, sy, tx, ty };
    return true;
}

// ============================================================
// 命中测试（填充规则）：P1 用包围盒，这个函数留给 P3 的"点在路径内"
//   evenOdd=true → 奇偶规则；false → 非零环绕数
// 子路径一律按闭合处理（与填充一致；描边命中的口径不同，另算）
// ============================================================
inline bool PointInPath(const PathShape& s, float px, float py, bool evenOdd) {
    if (s.Empty()) return false;
    int winding = 0;    // 非零环绕数
    int crossings = 0;  // 奇偶规则的穿越次数
    for (const PathShape::Contour& c : s.contours) {
        if (c.count < 2) continue;
        for (uint32_t i = 0; i < c.count; i++) {
            const PathPointF& a = s.pts[c.first + i];
            const PathPointF& b = s.pts[c.first + (i + 1) % c.count];
            if (a.y == b.y) continue;                       // 水平边不产生穿越
            const float side = (b.x - a.x) * (py - a.y) - (px - a.x) * (b.y - a.y);
            // 射线向右的穿越：半开区间 [min,max) 保证顶点不重复计数
            if ((a.y <= py) != (b.y <= py)) {
                const float t = (py - a.y) / (b.y - a.y);
                if (a.x + t * (b.x - a.x) > px) crossings++;
            }
            if (a.y <= py) {
                if (b.y > py && side > 0.0f) winding++;
            } else {
                if (b.y <= py && side < 0.0f) winding--;
            }
        }
    }
    return evenOdd ? (crossings & 1) != 0 : winding != 0;
}

// ============================================================
// 描边连接几何（stroke-linejoin）：斜接 / 斜面缺口的三角形与四边形
//
// 两个地方要用同一份数学，所以放在几何头里而不是 paint 头：
//   · 运行期描边：`binui_paint.h` 用它在拐角补多边形；
//   · 编译期算包围盒：斜接尖角能伸出几何包围盒 (ratio-1)·半线宽，
//     不把盒子撑开就会被"每个节点都裁剪到自己矩形"的后端切平（见 buf 里 append tip）。
// ============================================================
inline float PathJoinCosThreshold() { return 0.966f; }   // cos(15°)

// 两段方向夹角是否需要补缺口（补 = 转角超过 15°）
inline bool PathNeedJoin(float ax, float ay, float bx, float by, float cx, float cy) {
    const float ux = bx - ax, uy = by - ay;
    const float vx = cx - bx, vy = cy - by;
    const float lu = std::sqrt(ux * ux + uy * uy);
    const float lv = std::sqrt(vx * vx + vy * vy);
    if (lu <= 1e-6f || lv <= 1e-6f) return false;
    const float cs = (ux * vx + uy * vy) / (lu * lv);
    return cs < PathJoinCosThreshold();
}

// 转角缺口的几何：两条外侧偏移点 + 斜接点（外侧两条偏移线的交点）
//   p1 = b + half·n(入射段)、p2 = b + half·n(出射段)、m = 两条偏移线的交点
//   缺口区域 = 三角形 (p1, b, p2)（斜面）；斜接时再补上 (p1, m, p2) 那块尖角，
//   合成四边形 (p1, m, p2, b)。两个多边形都只和线段"擦边"，不重复覆盖 ——
//   半透明描边不会因此被叠加两次。
struct PathJoinGeom {
    bool ok = false;
    float p1x = 0, p1y = 0;
    float p2x = 0, p2y = 0;
    float mx = 0, my = 0;
    float ratio = 0;   // |m-b| / half = 1/sin(θ/2)：斜接尖角的长度比
};

inline PathJoinGeom PathJoinAt(float ax, float ay, float bx, float by, float cx, float cy,
                               float half) {
    PathJoinGeom g;
    float ux = bx - ax, uy = by - ay, vx = cx - bx, vy = cy - by;
    const float lu = std::sqrt(ux * ux + uy * uy);
    const float lv = std::sqrt(vx * vx + vy * vy);
    if (lu <= 1e-6f || lv <= 1e-6f) return g;
    ux /= lu; uy /= lu; vx /= lv; vy /= lv;
    const float cross = ux * vy - uy * vx;
    if (std::fabs(cross) <= 1e-6f) return g;      // 共线 / 原地折返：没有缺口
    // 外侧法线：方向向量转 -90°（y 向上/向下都成立，靠 cross 符号选边）
    const float s = cross > 0.0f ? 1.0f : -1.0f;
    g.p1x = bx + s * uy * half; g.p1y = by - s * ux * half;
    g.p2x = bx + s * vy * half; g.p2y = by - s * vx * half;
    // 斜接点 = 过 p1 沿 u 的直线 ∩ 过 p2 沿 v 的直线（二维叉积解）
    const float dx = g.p2x - g.p1x, dy = g.p2y - g.p1y;
    const float den = ux * vy - uy * vx;
    if (std::fabs(den) <= 1e-9f) return g;
    const float t = (dx * vy - dy * vx) / den;
    g.mx = g.p1x + ux * t;
    g.my = g.p1y + uy * t;
    const float ddx = g.mx - bx, ddy = g.my - by;
    g.ratio = std::sqrt(ddx * ddx + ddy * ddy) / half;
    g.ok = true;
    return g;
}

// ============================================================
// 归一化（v15 路径段的存储形态）
//
// 每个点量化成 uint16：0..65535 映射包围盒内 0..1。
// 精度 = 1/65535 盒宽（24px 的图标 → 0.0004px），比任何显示器都细。
// 节点盒在重排/等比缩放下会变，归一化点自动跟着变，路径数据一次写入永不重算。
// ============================================================
struct PathNormData {
    std::vector<uint16_t> pts;              // x,y 交替
    std::vector<uint32_t> contourFirst;     // 每条子路径的起始点下标（点下标，不是 uint16 下标）
    std::vector<uint8_t>  contourClosed;
};

inline bool NormalizeShape(const PathShape& s, PathNormData& out) {
    out.pts.clear();
    out.contourFirst.clear();
    out.contourClosed.clear();
    if (s.Empty()) return false;
    const float w = s.maxX - s.minX;
    const float h = s.maxY - s.minY;
    // 退化成一条线/一个点：宽度或高度为 0 时用 1 兜底，避免除 0 后所有坐标塌到 0
    const float sx = w > 1e-6f ? static_cast<float>(kPathNormMax) / w : 0.0f;
    const float sy = h > 1e-6f ? static_cast<float>(kPathNormMax) / h : 0.0f;
    auto q = [](float v) -> uint16_t {
        if (!(v > 0.0f)) return 0;
        if (v >= static_cast<float>(kPathNormMax)) return kPathNormMax;
        return static_cast<uint16_t>(std::lround(v));
    };
    for (const PathShape::Contour& c : s.contours) {
        if (c.count < 2) continue;
        out.contourFirst.push_back(static_cast<uint32_t>(out.pts.size() / 2));
        out.contourClosed.push_back(c.closed ? 1u : 0u);
        for (uint32_t i = 0; i < c.count; i++) {
            const PathPointF& p = s.pts[c.first + i];
            out.pts.push_back(q((p.x - s.minX) * sx));
            out.pts.push_back(q((p.y - s.minY) * sy));
        }
    }
    return !out.contourFirst.empty();
}

// 归一化点 → 设备坐标（运行期绘制用；x/y/w/h 是节点包围盒）
inline void DenormPoints(const uint16_t* pts, uint32_t count,
                         float x, float y, float w, float h, PathPointF* out) {
    const float kx = w / static_cast<float>(kPathNormMax);
    const float ky = h / static_cast<float>(kPathNormMax);
    for (uint32_t i = 0; i < count; i++)
        out[i] = { x + static_cast<float>(pts[i * 2]) * kx,
                   y + static_cast<float>(pts[i * 2 + 1]) * ky };
}

}  // namespace binui
