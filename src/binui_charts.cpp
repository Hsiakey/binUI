// binui_charts.cpp - 高层图表 API 实现
#include <cmath>
#include <chrono>
#include <algorithm>

#include "binui_charts.h"
#include "binui_format.h"
#include "backends/binui_gl-gdi.h"

namespace binui {

// ============ 基类 ============
BinUIChart::BinUIChart(BinUIRenderer& r, uint16_t containerId)
    : r_(r), container_(containerId) {
    for (const auto& n : r_.getNodes())
        if (n.id == containerId) {
            x_ = n.x; y_ = n.y; w_ = n.width; h_ = n.height;
            pad_ = (float)n.padding;
            break;
        }
}

double BinUIChart::now() const {
    return std::chrono::duration_cast<std::chrono::duration<double>>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

double BinUIChart::easeProg(double from, double to, double t0, double n, bool& done) const {
    double p = (n - t0) / duration_;
    done = p >= 1.0;
    if (p < 0) p = 0;
    else if (p > 1) p = 1;
    else if (p < 0.5) p = 4 * p * p * p;                    // easeInOutCubic
    else { double f = -2 * p + 2; p = 1 - f * f * f / 2.0; }
    return from + (to - from) * p;
}

void BinUIChart::invalidateNode(uint16_t id) {
    for (const auto& n : r_.getNodes())
        if (n.id == id) { invalidateRect((int)n.x, (int)n.y, (int)n.width, (int)n.height); break; }
}

// ============ 柱形图 ============
BarChart::BarChart(BinUIRenderer& r, uint16_t cid, int slicePerBar, uint32_t color)
    : BinUIChart(r, cid), slicePerBar_(slicePerBar), color_(color) {}

void BarChart::rebuild() {
    for (auto& e : els_) r_.removeNode(e.id);
    els_.clear();
    from_.clear(); to_.clear(); t0_.clear(); cur_.clear();
    barCount_ = 0;
}

void BarChart::ensure(int n) {
    if (n == barCount_) return;
    rebuild();
    float innerL = x_ + pad_, innerT = y_ + pad_;
    float innerW = w_ - 2 * pad_, innerH = h_ - 2 * pad_;
    if (innerW <= 0 || innerH <= 0) return;
    float gap = 6.0f;
    float barW = (innerW - gap * (n - 1)) / n;
    float sliceH = innerH / slicePerBar_;
    float bottom = innerT + innerH;
    for (int b = 0; b < n; b++) {
        float bx = innerL + b * (barW + gap);
        uint32_t c = colors_.size() == (size_t)n ? colors_[b] : color_;
        for (int s = 0; s < slicePerBar_; s++) {
            float sy = bottom - (s + 1) * sliceH;
            uint16_t id = r_.addNode(container_, BINUI_LIST, bx, sy, barW, sliceH + 0.5f, c, 2);
            r_.setVisible(id, false);   // 初始全隐藏，由数据填充
            els_.push_back({ id, (int)bx, (int)sy, (int)barW, (int)(sliceH + 0.5f) });
        }
    }
    barCount_ = n;
    from_.assign(n, 0); to_.assign(n, 0); t0_.assign(n, 0); cur_.assign(n, 0);
}

void BarChart::setData(const float* values, int n) {
    ensure(n);
    if (n == 0) return;
    double nw = now();
    for (int b = 0; b < n; b++) {
        from_[b] = (float)to_[b];
        to_[b] = std::max(0.0f, std::min(1.0f, values[b]));
        t0_[b] = nw;
    }
}

bool BarChart::tick(float) {
    if (barCount_ == 0) return false;
    double nw = now();
    bool changed = false;
    for (int b = 0; b < barCount_; b++) {
        bool d = false;
        float v = (float)easeProg(from_[b], to_[b], t0_[b], nw, d);
        int nf = (int)std::lround(v * slicePerBar_);
        if (nf < 0) nf = 0;
        if (nf > slicePerBar_) nf = slicePerBar_;
        int cf = cur_[b];
        if (nf == cf) continue;
        int base = b * slicePerBar_;
        if (nf > cf) {
            for (int s = cf; s < nf; s++) {
                const El& e = els_[base + s];
                invalidateRect(e.x, e.y, e.w, e.h);
                r_.setVisible(e.id, true);
            }
        } else {
            for (int s = nf; s < cf; s++) {
                const El& e = els_[base + s];
                invalidateRect(e.x, e.y, e.w, e.h);
                r_.setVisible(e.id, false);
            }
        }
        cur_[b] = nf;
        changed = true;
    }
    return changed;
}

// ============ 折线图（真线条） ============
LineChart::LineChart(BinUIRenderer& r, uint16_t cid, uint32_t color)
    : BinUIChart(r, cid), color_(color) {}

void LineChart::ensure(int n) {
    if (n == cols_) return;
    cols_ = n;
    float innerL = x_ + pad_, innerT = y_ + pad_;
    float innerW = w_ - 2 * pad_, innerH = h_ - 2 * pad_;
    if (innerW <= 0 || innerH <= 0) return;
    left_ = innerL;
    top_ = innerT;
    colW_ = innerW / n;
    rowH_ = innerH / (rows_ - 1);
    from_.assign(n, 0); to_.assign(n, 0); t0_.assign(n, 0);
    curY_.assign(n, innerT + innerH / 2);   // 初始在中间
}

void LineChart::setData(const float* values, int n) {
    ensure(n);
    if (n == 0) return;
    double nw = now();
    for (int c = 0; c < n; c++) {
        float v = std::max(0.0f, std::min(1.0f, values[c]));
        float ty = top_ + (rows_ - 1 - v * (rows_ - 1)) * rowH_;  // 值 0 在底部
        from_[c] = curY_[c];
        to_[c] = ty;
        t0_[c] = nw;
    }
}

bool LineChart::tick(float) {
    if (cols_ == 0) return false;
    double nw = now();
    bool changed = false;
    for (int c = 0; c < cols_; c++) {
        bool d = false;
        float v = (float)easeProg(from_[c], to_[c], t0_[c], nw, d);
        if (std::fabs(v - curY_[c]) < 0.02f) continue;
        curY_[c] = v;
        changed = true;
    }
    if (changed) {
        int l = (int)left_ - 1, t = (int)top_ - 1;
        int w = (int)(colW_ * cols_) + 2, h = (int)(rowH_ * (rows_ - 1)) + 2;
        invalidateRect(l, t, w, h);
    }
    return changed;
}

void LineChart::draw(BinUIGLGdi& gfx) {
    if (cols_ == 0) return;
    float hw = lineWidth_ * 0.5f;
    for (int c = 0; c < cols_; c++) {
        float x = left_ + c * colW_ + colW_ * 0.5f;
        // 端点圆点（圆滑折点）
        gfx.fillRounded(x - hw, curY_[c] - hw, lineWidth_, lineWidth_, hw, color_);
        if (c + 1 < cols_) {
            float x2 = left_ + (c + 1) * colW_ + colW_ * 0.5f;
            gfx.fillLine(x, curY_[c], x2, curY_[c + 1], lineWidth_, color_);
        }
    }
}

// ============ 环形进度 ============
RingGauge::RingGauge(BinUIRenderer& r, uint16_t cid, int ticks, uint32_t color)
    : BinUIChart(r, cid), ticks_(ticks), color_(color) {}

void RingGauge::ensure() {
    if (!els_.empty()) return;
    float cx = x_ + w_ / 2, cy = y_ + h_ / 2;
    float R = std::min(w_, h_) / 2 - 6;
    int size = 7;
    for (int i = 0; i < ticks_; i++) {
        double a = 2 * 3.14159265358979 * i / ticks_;
        float px = cx + R * (float)std::sin(a);
        float py = cy - R * (float)std::cos(a);
        int lx = (int)std::floor(px - size / 2 + 0.5f);
        int ty = (int)std::floor(py - size / 2 + 0.5f);
        uint16_t id = r_.addNode(container_, BINUI_LIST, lx, ty, size, size, color_, size / 2);
        r_.setVisible(id, false);
        els_.push_back({ id, lx, ty, size, size });
        on_.push_back(0);
    }
}

void RingGauge::setValue(float pct) {
    ensure();
    from_ = (double)curFilled_;
    to_ = std::max(0.0, std::min(100.0, (double)pct)) * ticks_ / 100.0;
    t0_ = now();
    animating_ = true;
}

bool RingGauge::tick(float) {
    if (!animating_ || els_.empty()) return false;
    bool done = false;
    double v = easeProg(from_, to_, t0_, now(), done);
    int nf = (int)std::lround(v);
    if (nf < 0) nf = 0;
    if (nf > ticks_) nf = ticks_;
    int cf = curFilled_;
    if (nf != cf) {
        if (nf > cf) {
            for (int i = cf; i < nf; i++) {
                const El& e = els_[i];
                invalidateRect(e.x, e.y, e.w, e.h);
                r_.setVisible(e.id, true);
                on_[i] = 1;
            }
        } else {
            for (int i = nf; i < cf; i++) {
                const El& e = els_[i];
                invalidateRect(e.x, e.y, e.w, e.h);
                r_.setVisible(e.id, false);
                on_[i] = 0;
            }
        }
        curFilled_ = nf;
    }
    if (done) animating_ = false;
    return nf != cf;
}

// ============ 分段进度条 ============
ProgressBar::ProgressBar(BinUIRenderer& r, uint16_t cid, int segs, uint32_t color)
    : BinUIChart(r, cid), segs_(segs), color_(color) {}

void ProgressBar::ensure() {
    if (!els_.empty()) return;
    float innerL = x_ + pad_, innerT = y_ + pad_;
    float innerW = w_ - 2 * pad_, innerH = h_ - 2 * pad_;
    if (innerW <= 0) return;
    float barW = innerW - 56;          // 右侧留给百分比文本
    float segW = barW / segs_;
    for (int i = 0; i < segs_; i++) {
        float sx = innerL + i * segW;
        uint16_t id = r_.addNode(container_, BINUI_LIST, sx, innerT, segW - 1.5f, innerH, color_, 2);
        r_.setVisible(id, false);
        els_.push_back({ id, (int)sx, (int)innerT, (int)(segW - 1.5f), (int)innerH });
        on_.push_back(0);
    }
    label_ = r_.addNode(container_, BINUI_LABEL, innerL + barW + 6, innerT - 3, 52, (int)innerH + 6,
                        0xFFF5F5F7, 0, 12);
    r_.setText(label_, "0%");
}

void ProgressBar::setValue(float pct) {
    ensure();
    from_ = (double)curFilled_;
    to_ = std::max(0.0, std::min(100.0, (double)pct)) * segs_ / 100.0;
    t0_ = now();
    animating_ = true;
}

void ProgressBar::setLabel(const char* text) {
    ensure();
    if (label_) { r_.setText(label_, text ? text : ""); invalidateNode(label_); }
}

bool ProgressBar::tick(float) {
    if (!animating_ || els_.empty()) return false;
    bool done = false;
    double v = easeProg(from_, to_, t0_, now(), done);
    int nf = (int)std::lround(v);
    if (nf < 0) nf = 0;
    if (nf > segs_) nf = segs_;
    int cf = curFilled_;
    if (nf != cf) {
        if (nf > cf) {
            for (int i = cf; i < nf; i++) {
                const El& e = els_[i];
                invalidateRect(e.x, e.y, e.w, e.h);
                r_.setVisible(e.id, true);
                on_[i] = 1;
            }
        } else {
            for (int i = nf; i < cf; i++) {
                const El& e = els_[i];
                invalidateRect(e.x, e.y, e.w, e.h);
                r_.setVisible(e.id, false);
                on_[i] = 0;
            }
        }
        curFilled_ = nf;
    }
    if (done) animating_ = false;
    return nf != cf;
}

} // namespace binui
