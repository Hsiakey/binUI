// binui_charts.h - 高层图表 API
// HTML 只管 UI 布局（带 id 的空容器），cpp 填数据，动画由 tick() 每帧自动驱动。
// 元素节点通过 renderer.addNode() 运行时创建，无需在 HTML 里手写柱/点/环。
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <functional>

#include "binui_renderer.h"

namespace binui {

class BinUIGLGdi; // 前置声明：draw() 需要后端，避免强依赖

// 图表基类：定位容器 + 动画时钟 + 失效回调
class BinUIChart {
public:
    using InvalidateFn = std::function<void(int l, int t, int w, int h)>;

    BinUIChart(BinUIRenderer& r, uint16_t containerId);
    virtual ~BinUIChart() = default;

    void setDuration(double s) { duration_ = s; }   // 默认 0.6s
    double duration() const { return duration_; }
    void setInvalidateFn(InvalidateFn fn) { invalidate_ = std::move(fn); }

    // 应用 updateFn 每帧调用：推进动画；返回本帧是否有变化（需要重绘）
    virtual bool tick(float dt) = 0;
    // 应用 drawFn 在 drawNodesGLGdi 之后调用：图表直接向后端绘制（折线等非矩形元素）
    virtual void draw(BinUIGLGdi& gfx) { (void)gfx; }

protected:
    // 缓动插值：from -> to，按 (now - t0)/duration_ 推进；p 越界时 done 置 true
    double easeProg(double from, double to, double t0, double now, bool& done) const;
    double now() const;
    void invalidateRect(int x, int y, int w, int h) { if (invalidate_) invalidate_(x, y, w, h); }
    void invalidateNode(uint16_t id);  // 按节点矩形失效

    BinUIRenderer& r_;
    uint16_t container_;
    float x_, y_, w_, h_;    // 容器矩形（绝对坐标）
    float pad_ = 0;          // 容器内边距（NodeInfo.padding）
    double duration_ = 0.6;
    InvalidateFn invalidate_;
};

// 柱形图：内部自动建切片节点，setData 触发平滑生长（自基线向上）
class BarChart : public BinUIChart {
public:
    BarChart(BinUIRenderer& r, uint16_t containerId,
             int slicePerBar = 14, uint32_t color = 0xFF8B5CF6);
    void setData(const float* values, int n);       // 值 0..1
    void setData(const std::vector<float>& v) { setData(v.data(), (int)v.size()); }
    void setColors(const std::vector<uint32_t>& cs) { colors_ = cs; rebuild(); }
    bool tick(float dt) override;

private:
    struct El { uint16_t id; int x, y, w, h; };
    void ensure(int n);
    void rebuild();
    int slicePerBar_;
    uint32_t color_;
    std::vector<uint32_t> colors_;
    std::vector<El> els_;            // barCount * slicePerBar
    std::vector<float> from_, to_, t0_;
    std::vector<int> cur_;           // 每柱当前填充切片数
    int barCount_ = 0;
};

// 折线图：用真线条（fillLine）画，tick 推进顶点，draw 每帧重绘折线
class LineChart : public BinUIChart {
public:
    LineChart(BinUIRenderer& r, uint16_t containerId, uint32_t color = 0xFF06B6D4);
    void setData(const float* values, int n);       // 值 0..1
    void setData(const std::vector<float>& v) { setData(v.data(), (int)v.size()); }
    void setLineWidth(float w) { lineWidth_ = w; }
    bool tick(float dt) override;
    void draw(BinUIGLGdi& gfx) override;

private:
    void ensure(int n);
    int rows_ = 15;
    uint32_t color_;
    float lineWidth_ = 3.0f;
    float left_, top_, colW_, rowH_;   // 坐标映射
    std::vector<float> from_, to_, t0_;
    std::vector<float> curY_;          // 当前各顶点 y（绝对坐标）
    int cols_ = 0;
};

// 环形进度：点阵圆环，setValue 自动填充
class RingGauge : public BinUIChart {
public:
    RingGauge(BinUIRenderer& r, uint16_t containerId,
              int ticks = 32, uint32_t color = 0xFF8B5CF6);
    void setValue(float pct);                        // 0..100
    bool tick(float dt) override;

private:
    struct El { uint16_t id; int x, y, w, h; };
    void ensure();
    int ticks_;
    uint32_t color_;
    std::vector<El> els_;
    std::vector<int> on_;
    double from_ = 0, to_ = 0, t0_ = 0;
    int curFilled_ = 0;
    bool animating_ = false;
};

// 分段进度条：setValue 平滑填充 + setLabel 右侧文本
class ProgressBar : public BinUIChart {
public:
    ProgressBar(BinUIRenderer& r, uint16_t containerId,
                int segs = 20, uint32_t color = 0xFF10B981);
    void setValue(float pct);                        // 0..100
    void setLabel(const char* text);                 // 右侧百分比/文本
    bool tick(float dt) override;

private:
    struct El { uint16_t id; int x, y, w, h; };
    void ensure();
    int segs_;
    uint32_t color_;
    std::vector<El> els_;
    std::vector<int> on_;
    uint16_t label_ = 0;
    double from_ = 0, to_ = 0, t0_ = 0;
    int curFilled_ = 0;
    bool animating_ = false;
};

} // namespace binui
