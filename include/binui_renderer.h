#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <array>
#include <unordered_map>
#include <unordered_set>
#include <functional>
#include <memory>

#include "binui_format.h"
#include "binui_paint.h"   // 路径条带缓存（PathBandCacheEntry）
#include "binui_link.h"

namespace binui {

enum class EventType {
    CLICK,
    INPUT,
    RESIZE,
    HOVER,
    FOCUS,
    PRESS,
    SCROLL,
    VIDEO_PLAY,   // <video> 开始播放（点击切换/键盘/API 触发）
    VIDEO_PAUSE,  // <video> 暂停
    VIDEO_SEEK,   // <video> 进度跳转（控制条点击/拖动；引擎内部校正不触发）
    VIDEO_ENDED,  // <video> 播放到末尾（loop 时回绕不触发）
};

struct Event {
    EventType type;
    uint16_t  nodeId;
    union {
        struct { float x, y; } click;
        struct { const char* text; } input;
        struct { int width, height; } resize;
    };
};

struct ImageResource {
    std::vector<uint8_t> data;   // 编码字节（PNG/JPEG…）或 rawPixels 时的原始 RGBA 像素
    int width = 0;
    int height = 0;
    int channels = 0;
    bool rawPixels = false;      // true = data 为 RGBA8 原始像素（updateImageRaw 设置）
};

struct FontResource {
    std::vector<uint8_t> data;
    float size = 14.0f;
};

// 文本框编辑会话状态（由焦点驱动，blur 时提交到节点文本）
struct EditState {
    bool active = false;
    uint16_t nodeId = 0;
    std::string value;   // 编辑中的文本
    size_t caret = 0;    // 光标位置（UTF-8 字节偏移）
    size_t selStart = 0; // 选区起点（== caret 表示无选区）
};

// 滚动条外观（overflow:auto/scroll 容器由后端绘制滚动条，样式来自这里）。
// 未显式设置时按根节点底色亮度派生：浅底给浅色轨道/滑块，深底给深色 —— 不再有"浅色主题一条黑杠"。
struct BinUIScrollbarStyle {
    uint32_t thumb = 0xFF5A5A5A;  // 滑块色 AARRGGBB
    uint32_t track = 0xFF2E2E2E;  // 轨道色 AARRGGBB
    int      width = 8;           // 轨道宽（像素，会被钳到 2..32）
};

// UI 核心：binUI 布局解析、节点状态、命中测试与事件分发。
// 不依赖任何平台 API，渲染由外部后端通过 getNodes() 读取节点树完成。
class BinUIRenderer {
public:
    using EventCallback = std::function<void(const Event&)>;

    BinUIRenderer();
    ~BinUIRenderer();

    void shutdown();

    // 解析 binUI（.xhtml）二进制布局
    bool loadLayout(const uint8_t* data, size_t size);

    // ============ 组件挂载（外挂 .bum）============
    // 把一份组件布局挂到 anchorId 节点下，然后按合并后的布局重新解析。
    // 与编译期内联共用同一个链接器（binui_link），合并规则完全一致：
    // 节点 id 重编号、字符串表后移、规格/状态/动画/视频/资源段一起迁移。
    // 挂载会重置指针/键盘/编辑等瞬时状态（节点 id 变了，锚点之前的焦点无意义）。
    bool mountComponent(const uint8_t* data, size_t size, uint16_t anchorId);
    // 该锚点是否已挂过组件（ex::MountAll 用它保证幂等）
    bool isComponentMounted(uint16_t anchorId) const {
        return mountedAnchors_.count(anchorId) != 0;
    }

    // 布局里记录的 imageIndex -> 图片路径（v12 资源段；布局自描述）
    const std::vector<std::string>& imagePaths() const { return imagePaths_; }
    // 布局里记录的运行期挂载点（v12 组件引用段，<use ... external>）
    const std::vector<ComponentRef>& componentRefs() const { return componentRefs_; }
    // 当前布局二进制（含编译期内联的组件；mountComponent 之后会变大）
    const std::vector<uint8_t>& layoutBytes() const { return layoutBytes_; }

    // v13 扩展绘制段：节点的文字色/边框/渐变/字重/行高/文字装饰（无条目返回 nullptr）
    const BinUIPaintNode* paintOf(uint16_t nodeId) const {
        auto it = paintIdx_.find(nodeId);
        return it == paintIdx_.end() ? nullptr : &paints_[it->second];
    }

    // v15 路径段：BINUI_PATH 节点的折线（归一化坐标 + 子路径表；无条目返回 nullptr）
    struct PathRecord {
        BinUIPathEntry head{};
        std::vector<uint16_t> pts;      // x,y 交替，0..65535 映射节点盒
        std::vector<uint32_t> first;    // 每条子路径起始点下标
        std::vector<uint32_t> count;    // 每条子路径点数
        std::vector<uint8_t>  closed;   // 1 = 原始 Z
        // v16：填充渐变（PATH_FILL_GRAD 置位时有效）
        bool hasGrad = false;
        BinUIPathGradient grad{};
        // v17：描边渐变（PATH_STROKE_GRAD 置位时有效）
        bool hasStrokeGrad = false;
        BinUIPathGradient strokeGrad{};
    };
    const PathRecord* pathOf(uint16_t nodeId) const {
        auto it = pathIdx_.find(nodeId);
        return it == pathIdx_.end() ? nullptr : &paths_[it->second];
    }
    // v19 路径条带缓存：按 nodeId 取一个可写的缓存条目（不存在就地建）。
    // 布局一变（loadLayout / relayout / 增删节点 / 挂载组件）就整表作废，
    // 所以条目里只需要"代数"而不必逐项追踪失效。
    PathBandCacheEntry* pathBandCache(uint16_t nodeId) const {
        if (!pathBands_.count(nodeId) && pathBands_.size() >= kMaxPathCacheEntries) return nullptr;
        PathBandCacheEntry& e = pathBands_[nodeId];
        if (e.gen != pathCacheGen_) { e.gen = pathCacheGen_; e.key = 0; e.bands.clear(); }
        return &e;
    }
    uint64_t pathCacheGen() const { return pathCacheGen_; }
    void invalidatePathBands() {
        pathBands_.clear();
        ++pathCacheGen_;
    }
    // 该节点是否只是"裁剪容器"（CSS overflow:hidden）：可裁剪但不可滚动
    bool isClipOnly(uint16_t nodeId) const { return clipOnly_.count(nodeId) != 0; }
    // 该节点是否可被滚轮/拖动滚动（SCROLLABLE 且不是 clip-only）
    bool isUserScrollable(uint16_t nodeId) const;

    // v14 表单/文字附加标记（BinUITextFlag；无扩展绘制条目返回 0）
    uint32_t textFlagsOf(uint16_t nodeId) const {
        const BinUIPaintNode* p = paintOf(nodeId);
        return p ? p->textFlags : 0u;
    }
    // 禁用（<input disabled> / <button disabled>）：不进焦点链、不响应悬停/点击，绘制压到 55% 淡
    bool isDisabled(uint16_t nodeId) const;
    // 只读输入框：可聚焦、可选中，但拒绝编辑
    bool isReadonly(uint16_t nodeId) const { return (textFlagsOf(nodeId) & TEXT_READONLY) != 0; }
    // 该输入框当前的 text 是占位符（真实值为空）
    bool isPlaceholder(uint16_t nodeId) const { return (textFlagsOf(nodeId) & TEXT_PLACEHOLDER) != 0; }
    // 斜体（<i>/<em>/font-style:italic）
    bool isItalic(uint16_t nodeId) const { return (textFlagsOf(nodeId) & TEXT_ITALIC) != 0; }
    // 输入框的真实值（占位符状态下为空串；此时节点 text 存的是占位符提示语）
    std::string inputValue(uint16_t nodeId) const;

    // 文字色：扩展绘制段有就用它，否则回退节点自带颜色（LABEL 的旧约定）
    uint32_t textColorOf(uint16_t nodeId) const;

    void loadResources(const std::unordered_map<std::string, std::vector<uint8_t>>& resources);

    // 记录 imageIndex -> 资源路径（与编译器输出 CompileResult::images 顺序一致）
    void setImageOrder(const std::vector<std::string>& paths);

    // 按 imageIndex 取已加载的图片（未加载返回 nullptr）
    const ImageResource* getImage(uint16_t index) const;

    // 运行时换图：以编码数据（PNG/JPEG…）替换指定 imageIndex 的图片；
    // 后端在 generation 变化时重新上传纹理（配合 drawNodes* 的自动检测）
    bool updateImage(uint16_t index, const uint8_t* data, size_t size);
    // 运行时换图：以原始 RGBA8 像素（w*h*4，行序从上到下）替换指定 imageIndex 的图片
    bool updateImageRaw(uint16_t index, int width, int height, const uint8_t* rgba);
    // 该 imageIndex 的换图版本号（updateImage/updateImageRaw 每次自增，从 0 起）
    uint32_t imageGeneration(uint16_t index) const;
    // 确保图片槽编号空间至少 count 个（视频引擎推送帧前调用；setImageOrder 未覆盖的槽补齐）
    void reserveImageSlots(uint16_t count);

    // 运行时更新节点
    void setText(uint16_t nodeId, const std::string& text);
    void setVisible(uint16_t nodeId, bool visible);
    // 运行时改节点矩形（宿主自己排版：配合 setText 换长文本后手动撑高）。
    // 只改这一个节点，不牵动兄弟节点（不做流式重排）；宿主需要自己挪后续节点。
    // 返回 false = 无此节点。
    bool setNodeBounds(uint16_t nodeId, float x, float y, float w, float h);
    // ---- 局部重排（v21）----
    // setNodeBounds 只改盒子、不动内容；下面这组按编译期规格段（百分比 / 流式 / flex /
    // min·max）把**这棵子树**重新排一遍：
    //   relayoutSubtree(id)        —— 盒子不变，只按盒子重排内容（宿主换文本撑高后调用）
    //   resizeNodeBounds(id,...)   —— 改矩形 + 钉住尺寸覆盖 + 重排（缩放拖拽内部就用它）；
    //                                节点在流式/flex 父容器里时连兄弟位置一起修正
    //   clearBoundsOverride(id)    —— 解除钉住：下次 window resize/重排回到规格值的尺寸
    // 不做的事：不改兄弟（除非本节点在流式/flex 里）、不重算文本高度、不重排父级链。
    bool relayoutSubtree(uint16_t nodeId);
    bool resizeNodeBounds(uint16_t nodeId, float x, float y, float w, float h);
    void clearBoundsOverride(uint16_t nodeId);
    bool hasBoundsOverride(uint16_t nodeId) const { return boundsOverride_.count(nodeId) != 0; }
    // 该节点的规格段是否带 heightAuto 标记（高度是编译期按文本估算撑开的）。
    // true 的节点在 setText 换文本时会自动重算高度；作者显式写了 height 的为 false。
    bool specHeightAuto(uint16_t nodeId) const;

    // v8 运行时动态节点：图表 API 等直接在节点表追加元素（静态绝对定位，无缩放规格）
    uint16_t addNode(uint16_t parentId, uint8_t type,
                     float x, float y, float w, float h,
                     uint32_t color = 0xFF888888, uint16_t radius = 0,
                     uint16_t fontSize = 0);
    void removeNode(uint16_t nodeId); // 隐藏（不可见），节点保留供复用
    // 按 HTML id / XML name 属性查找节点（编译器输出名称表；未找到返回 0）
    uint16_t findNodeByName(const std::string& name) const;

    // 有效可见性：节点自身及其所有祖先都必须可见
    bool isNodeVisible(uint16_t nodeId) const;

    void setSize(int width, int height);
    // 是否有缩放规格段（百分比/流式重排规则；有则在 setSize 时重排）
    bool hasResizeSpec() const { return hasSpecs_; }

    // 命中测试（返回节点 id，未命中返回 -1）
    int hitTest(float x, float y) const;
    // 扩展命中：allowStyled 时把"声明了 state 状态样式"的普通节点也当作命中目标
    int hitTestEx(float x, float y, bool allowStyled, uint8_t state) const;

    void setEventCallback(EventCallback cb);
    // 点击分发：命中 INPUT 节点自动聚焦，其余情况失焦；返回命中节点 id（-1 未命中）
    int dispatchClick(float x, float y);
    void dispatchInput(uint16_t nodeId, const char* text);

    // ============ 指针状态（hover / 按下） ============
    int hoveredNode() const { return hoverId_; }
    int pressedNode() const { return pressedId_; }
    bool isHovered(uint16_t nodeId) const { return hoverId_ == (int)nodeId; }
    bool isPressed(uint16_t nodeId) const { return pressedId_ == (int)nodeId; }

    void setHover(float x, float y);  // 鼠标移动：更新悬停节点（进入时触发 HOVER 事件）
    void clearHover();                // 鼠标离开窗口
    int press(float x, float y);      // 按下：记录按下节点（INPUT 聚焦），触发 PRESS 事件
    int release(float x, float y);    // 松开：仍在原节点上则触发 CLICK；返回点击节点 id（-1 无）
    void cancelPress();               // 取消按下（如窗口失焦），不触发点击

    // ============ 键盘导航（焦点） ============
    // 方向键**不再**移动焦点：它和浏览器一样滚页面（见下面 handleKey）。
    // 移动焦点用 WASD（W 上 / A 左 / S 下 / D 右），Tab 仍按文档顺序循环。
    enum class KeyDirection { UP, DOWN, LEFT, RIGHT };

    int keyboardNode() const { return keyboardId_; }           // 当前键盘焦点节点（-1 无）
    bool isKeyboardFocused(uint16_t nodeId) const { return keyboardId_ == (int)nodeId; }
    void setKeyboardFocus(int nodeId);                         // 直接设置焦点（-1 清除）
    int moveKeyboardFocus(KeyDirection dir);                   // 向指定方向移动焦点，返回新焦点（-1 无候选）
    int cycleKeyboardFocus(bool forward = true);               // Tab 循环：按文档顺序在可交互节点间移动（Shift+Tab 反向），返回新焦点
    bool activateKeyboardFocus();                              // Enter/Space：INPUT 进入编辑，其余触发 CLICK

    // ============ 键盘路由（焦点 / 滚动 / 文本编辑） ============
    // 宿主把 WM_KEYDOWN 的 VK_* 直接丢进 handleKey 就行，库按当前状态决定谁消费：
    //
    //   · 文本编辑中（isEditing）：退格/删除/左右/Home/End 走光标；Enter 与 Esc 退出编辑
    //     回到焦点控制；Tab 退出并跳到下一个控件。**这个状态下 WASD 不移动焦点** ——
    //     字母要留给输入法/文本；焦点环从虚线换成"编辑中"的实线蓝环（见 focusRingOf）。
    //   · 焦点控制中（默认）：WASD 移动焦点；方向键滚页面（离焦点最近的滚动容器，
    //     焦点不在任何滚动容器里时滚视口内面积最大的那个）；PageUp/PageDown 翻页；
    //     Home/End 到顶/到底；Tab/Shift+Tab 循环；Enter 激活（INPUT 进入编辑、其余
    //     触发 CLICK）；空格有焦点时激活、无焦点时向下翻页（浏览器语义）。
    //   · 鼠标点击：只切 :focus 状态，**不点亮焦点环**（focus-visible 语义）。
    //
    // 焦点环在 2 秒没有操作后自动消失（kFocusRingIdleSec）：宿主不用自己定时，
    // stepAnimations() 会在隐藏的那一帧把焦点节点报回来，照常 invalidate 即可。
    enum : uint32_t {
        KEYMOD_SHIFT = 1u,
        KEYMOD_CTRL  = 2u,
        KEYMOD_ALT   = 4u,
    };
    struct KeyResult {
        bool handled = false;              // 库消费了这个键（宿主别再当快捷键用）
        std::vector<uint16_t> changed;     // 需要重绘的节点（焦点环 / 光标 / 滚动容器）
    };
    KeyResult handleKey(int vk, uint32_t mods = 0);

    // 焦点环三态（每个节点单独判，后端按它画）：
    //   None    = 不画
    //   Dashed  = 待编辑：**只有输入元素**在键盘选中、还没按 Enter 时用。虚线是给
    //             输入框的状态标记（"再按一下 Enter 才能打字"），别拿它当通用焦点样式；
    //             颜色 = 底色对比色，2 秒无操作自动消失
    //   Solid   = 普通焦点：按钮等非输入元素被键盘选中时用，实线 + 对比色，
    //             同样 2 秒无操作自动消失
    //   Editing = 编辑中：正在编辑的输入框，实线加粗 + 强调蓝，常亮（有光标）
    // 线型 + 颜色双编码：只靠颜色区分对色弱用户不友好，虚线/实线一眼能看出来。
    enum class FocusRing { None, Dashed, Solid, Editing };
    FocusRing focusRingOf(uint16_t nodeId) const;
    static constexpr uint32_t kFocusRingEditColor = 0xFF3E8BFF; // 编辑中实线环（强调蓝）

    // 待编辑态焦点环是否点亮（不含编辑中的实线环）
    bool focusRingVisible() const { return focusRing_ && !edit_.active; }
    void showFocusRing();                 // 主动点亮（同时重置 2 秒计时）
    void hideFocusRing();                 // 立即隐藏（窗口失焦等）
    // 焦点环无操作后自动消失的秒数（默认 2.0；小于等于 0 = 一直显示）
    void setFocusRingIdleTime(double seconds);
    static constexpr double kFocusRingIdleSec = 2.0;

    // 方向键滚动：滚动"焦点所在的滚动容器"（无焦点时取视口内面积最大的可滚动容器），
    // 返回被滚动的容器 id（-1 = 没有可滚动的容器）。dx/dy 是像素；正值 = 内容上移/左移。
    int scrollFocused(float dx, float dy);

    // ============ 滚动容器 ============
    struct ScrollState { float x = 0, y = 0; };

    // 滚动条样式：显式指定 / 回到默认（默认 = 按根节点底色派生，见 BinUIScrollbarStyle）。
    // 派生时机是 loadLayout（那时根节点底色已知）；之后 setScrollbarStyle 覆盖，
    // resetScrollbarStyleToDefault 重新派生。
    using ScrollbarStyle = BinUIScrollbarStyle;
    void setScrollbarStyle(uint32_t thumb_argb, uint32_t track_argb, int width);  // 显式指定
    void resetScrollbarStyleToDefault();                                          // 回到按根节点底色派生
    const BinUIScrollbarStyle& scrollbarStyle() const;

    bool isScrollable(uint16_t nodeId) const;
    void scrollTo(uint16_t nodeId, float x, float y);          // 设置滚动偏移（钳制到内容范围）
    void scrollBy(uint16_t nodeId, float dx, float dy);
    const ScrollState* scrollState(uint16_t nodeId) const;     // 滚动偏移（非滚动容器返回 nullptr）
    int scrollAt(float x, float y, float dy);                  // 滚轮：滚动光标处最顶层滚动容器，返回其 id
    float contentHeight(uint16_t nodeId) const;                // 内容高度（子节点下界 - 容器顶）
    float contentWidth(uint16_t nodeId) const;                 // 内容宽度（子节点右界 - 容器左）
    int hitScrollBar(float x, float y, bool& vertical) const;  // 命中滚动条/轨道：返回容器 id（-1 无）

    // ============ 光标 ============
    // 窗口外壳拿它决定鼠标指针形状（WM_SETCURSOR）。按"光标下是什么"给：
    //   输入框 → I 型；可交互（按钮等）→ 手型；滚动条边缘 → 竖直/水平拖动；
    //   可缩放元素的边/角 → 系统 resize 光标；其余 → 箭头。
    enum class CursorKind : uint8_t {
        Arrow = 0, Hand = 1, IBeam = 2,
        SizeNS = 3, SizeWE = 4,          // 上下 / 左右边
        SizeNWSE = 5, SizeNESW = 6,      // 左上-右下 / 右上-左下角
    };
    CursorKind cursorAt(float x, float y) const;
    bool scrollBarHitTest(float x, float y) const;             // 点是否在滚动条区域（测试用）

    // ============ 元素缩放（v21：CSS resize） ============
    // 编译器把 `resize: left right top bottom + 四角` 编成节点的 resizeMask；指针贴到
    // 开放的那条边/那个角上，引擎自己接管拖拽 —— 和滚动条拖动同一条路子：
    //   · cursorAt() 把 4 种系统 resize 光标交给外壳，宿主不用自己判边
    //   · press() 命中热区就进入缩放态（不发 PRESS/CLICK），setHover() 期间每步改
    //     节点矩形，release() 收尾（同样不发 CLICK）
    //   · 只改这一个节点：子节点不重排（引擎没有运行时重排子树的接口），超出盒子
    //     的部分由 overflow 决定裁不裁
    // 热区：贴边 ±4px；四角 12×12。最小尺寸 8px，拖动时被拖的那条边顶住不动。
    static constexpr float kResizeSlop     = 4.0f;
    static constexpr float kResizeCorner   = 12.0f;
    static constexpr float kResizeMinSize  = 8.0f;
    uint8_t resizeMaskOf(uint16_t nodeId) const;                // BinUIResizeFlag（0 = 不可缩放）
    // 指针下的缩放热区：返回节点 id（-1 = 没有），handle 输出命中位（BinUIResizeFlag）
    int resizeHandleAt(float x, float y, uint8_t* handle = nullptr) const;
    bool isResizing() const { return resizeNode_ >= 0; }
    uint16_t resizingNode() const;                              // 0xFFFF = 不在缩放
    // 运行时开关缩放（宿主做"锁定尺寸"按钮之类）。返回 false = 无此节点。
    bool setResizeMask(uint16_t nodeId, uint8_t mask);
    // 拖动期间累积的脏矩形（**屏幕坐标**，已含旧矩形 ∪ 新矩形）；取一次后清零。
    // 宿主在 move 回调里取它调 invalidateRect 即可，不用每帧整窗重绘。
    bool takeResizeDirty(float& x, float& y, float& w, float& h);

    // ============ 视频（v9：<video> 节点播放状态） ============
    // 播放时钟由 stepAnimations() 驱动（应用每帧已调用）；解码帧由可选模块
    // （binui_video，BINUI_ENABLE_VIDEO=ON 时 FFmpeg 实现）经 updateImageRaw
    // 推送到伪图片槽。无模块时视频仅绘制底色，API 仍可用。
    struct VideoState {
        uint16_t nodeId = 0;
        uint16_t imageIndex = 0;   // 伪图片槽（解码帧写入处）
        uint16_t flags = 0;        // binui_format.h BinUIVideoFlag
        float    volume = 1.0f;    // 0..1
        std::string src;           // 媒体路径（编译期 src 属性）
        // ---- 运行时状态 ----
        bool   playing = false;
        bool   ended = false;      // 播放到末尾且未 loop（再次 play 从头开始）
        double position = 0.0;     // 当前播放位置（秒）
        double duration = 0.0;     // 时长（秒；0 = 未知，由解码模块报告）
    };

    const std::vector<VideoState>& getVideos() const { return videos_; }
    const VideoState* findVideo(uint16_t nodeId) const;          // 非视频节点返回 nullptr

    bool playVideo(uint16_t nodeId);       // ended 状态下自动回绕到 0
    bool pauseVideo(uint16_t nodeId);
    bool toggleVideo(uint16_t nodeId);     // 返回切换后是否在播放
    void seekVideo(uint16_t nodeId, double sec, bool quiet = false);
    // quiet=true 不发 VIDEO_SEEK 事件（引擎内部 A/V 校正用）
    void setVideoVolume(uint16_t nodeId, float vol);             // 0..1
    void setVideoMuted(uint16_t nodeId, bool muted);
    void setVideoSource(uint16_t nodeId, const std::string& src);// attach 解码模块前更换源
    void setVideoDuration(uint16_t nodeId, double sec);          // 解码模块探测时长后报告
    // 控制条当前是否可见（后端绘制依据）：有 controls 且（暂停 || 悬停 || 正在拖动进度）
    bool videoControlsVisible(uint16_t nodeId) const;

    // ============ 文本框编辑 ============
    bool isEditing() const { return edit_.active; }
    // 是否在绘制光标（只读输入框可聚焦但不显示光标，所以这里排除 readonly）
    bool isFocused(uint16_t nodeId) const {
        return edit_.active && edit_.nodeId == nodeId && !isReadonly(nodeId);
    }
    const EditState& edit() const { return edit_; }

    // 开始编辑会话（仅 INPUT 节点，其他情况失焦）。viaKeyboard 表示是键盘（Enter）进来的：
    // 两种来源都会进编辑态、都会设 :focus，但**鼠标点进来的不画编辑焦点环** ——
    // 焦点框是键盘专用的提示，鼠标用户看光标就够了（浏览器 :focus-visible 同款取舍）。
    void focus(uint16_t nodeId, bool viaKeyboard = false);
    void blur();                       // 提交当前值到节点文本并结束会话
    bool insertText(const std::string& utf8);          // 在光标处插入（替换选区）
    bool backspace();                                  // 删除光标前字符（或整个选区）
    bool deleteForward();                              // 删除光标后字符
    bool moveCaret(int deltaChars, bool extend = false); // 按字符移动光标，extend 时保留选区起点
    bool setCaret(size_t byteOffset);                  // 设置光标（钳制到字符边界）
    // 后端测量后按 UTF-16 索引定位。extend=true 时**保留选区起点** —— 鼠标拖选就靠它：
    // 按下时 anchor 落在 selStart，拖动过程中只挪 caret。
    bool setCaretByUtf16Index(size_t utf16Index, bool extend = false);
    // 双击选词：按 UTF-16 索引选中它所在的"词"（ASCII 字母数字下划线 / 连续非 ASCII
    // 当一个词；点在标点上则选中连续的一段标点/符号）。返回 false = 那里没有可选内容。
    bool selectWordByUtf16Index(size_t utf16Index);

    // 剪贴板编辑（真正的剪贴板 I/O 在平台层：GL/D3D 外壳与 C ABI 各读各的）。
    // 这三个是"文本层面的动作"：取选中文字、删掉选区、全选。
    std::string selectedText() const;                  // 无选区返回空串
    bool deleteSelection();                            // 删掉选区（无选区返回 false）
    bool selectAll();                                  // 全选（只读输入框也可以选）

    struct NodeInfo {
        uint16_t id;
        uint16_t parentId;
        uint8_t  type;
        uint8_t  flags;
        float    x, y, width, height;
        uint32_t color;
        std::string text;
        uint16_t imageIndex;
        uint16_t fontSize;
        uint16_t padding;
        uint16_t radius;   // 圆角半径（像素；0 = 直角）
        int16_t  zIndex;   // 层叠顺序（v10；越大越靠上，同值 absolute 高于流式）
    };

    const std::vector<NodeInfo>& getNodes() const { return nodes_; }

    // 绘制顺序（v10）：按 z-index 稳定分层——z 升序；同 z 下流式先于 absolute；
    // 再按 DOM 顺序（父先于子）。返回节点下标，后出现者覆盖先出现者。后端按此顺序绘制。
    std::vector<uint32_t> drawOrder() const;

    // ============ 窗口描述（v5 <canvas>） ============
    struct WindowDesc {
        bool     present = false; // 布局含窗口描述段
        uint32_t width = 0, height = 0;  // 窗口初始尺寸
        uint16_t flags = 0;              // binui_format.h BinUIWindowFlag
        float    aspect = 0;             // 等比 w/h（BINUI_WIN_ASPECT 时有效）
        uint32_t bgColor = 0xFFFFFFFF;   // 画布背景 AARRGGBB
        std::string title;               // 窗口标题
        bool resizable() const { return (flags & BINUI_WIN_RESIZABLE) != 0; }
        bool aspectLocked() const { return (flags & BINUI_WIN_ASPECT) != 0; }
        bool transparent() const { return (flags & BINUI_WIN_TRANSPARENT) != 0; }
        bool scaleProportional() const { return (flags & BINUI_WIN_SCALE) != 0; }
    };
    const WindowDesc& windowDesc() const { return winDesc_; }

    // ============ 裁剪链 ============
    // 从近到远，把**会裁剪子元素**的祖先 id 追加到 out（不含自身）。
    // 判定口径只有一条：祖先带 BINUI_SCROLLABLE（编译期由 overflow:auto/scroll/hidden 标出）。
    // 普通 <div>（包括组件自己的根 div）按 CSS 语义**不**裁剪 —— 以前后端无条件按每个
    // 祖先矩形裁剪，hover 放大到 1.04 的组件按钮会被与它等大的组件根 div 切成原来那块。
    void clippingAncestors(uint16_t nodeId, std::vector<uint16_t>& out) const;

    // ============ 动画（v7：@keyframes 动画 + transition 过渡） ============
    // 应用每帧调用：推进动画时钟，返回本帧可视属性发生变化的节点 id（据此 invalidate 重绘）。
    // 无动画/过渡在运行时不产生重绘请求。同时推进视频播放时钟（v9），变化的视频节点
    // id 一并返回。
    std::vector<uint16_t> stepAnimations();

    // 运行时设置样式目标（触发该节点的 transition；未配置 transition 则立即生效）
    void setStyleFloat(uint16_t nodeId, uint16_t prop, float value); // ANIM_OPACITY/TRANSLATE/SCALE/RADIUS
    void setStyleColor(uint16_t nodeId, uint16_t prop, uint32_t value); // ANIM_BG_COLOR

    // 重置关键帧动画时钟（所有动画从当前时刻重新开始），用于“重播”按钮等交互
    void restartAnimations();

    // ============ 动画事件（v23：animationend / animationiteration / transitionend）============
    // 为什么是"取"而不是事件回调：这些事件在 stepAnimations() 内部产生，而宿主常在这条
    // 路径上改样式/删节点，回调会就地重入 —— 和 takeResizeDirty() 同一个理由。
    // 用法：每帧 stepAnimations() 之后取一次（取完清空），拿 "哪个节点的什么动画结束了"
    // 去做下一步编排，不用再自己按 durationMs 定时（那是重复实现引擎的时间轴）。
    enum class AnimEventKind : uint8_t {
        ANIM_END,        // 关键帧动画整体结束（infinite 与 iteration 未定的不报）
        ANIM_ITERATION,  // 迭代边界：第 iteration 次结束、下一次开始（同一帧跨多次只报一次）
        TRANSITION_END,  // 某个属性的过渡结束（prop = ANIM_*；被新过渡取代的不报）
    };
    struct AnimEvent {
        uint16_t nodeId = 0;
        AnimEventKind kind = AnimEventKind::ANIM_END;
        uint16_t prop = 0;       // 仅 TRANSITION_END 有效
        uint32_t iteration = 0;  // ANIM_ITERATION / ANIM_END：已完成的迭代次数（1 起）
    };
    std::vector<AnimEvent> takeAnimEvents();

    // 绘制查询：节点当前应显示的颜色（含 hover/按下 tint、过渡与关键帧动画）
    uint32_t drawColor(uint16_t nodeId) const;
    float drawOpacity(uint16_t nodeId) const;   // 1 = 不透明
    float drawOffsetX(uint16_t nodeId) const;   // 视觉平移 x
    float drawOffsetY(uint16_t nodeId) const;   // 视觉平移 y
    float drawScale(uint16_t nodeId) const;     // 1 = 原尺寸（绕中心缩放）
    float drawRadius(uint16_t nodeId) const;    // 圆角半径（-1 = 无动画，用节点原值）

    // ---- 绘制期批量查询（v20） ----
    // 上面 6 个函数分开调时，**每个**都要各自查 animResult_ / animState_ / nodeIndexById_，
    // 一个没有动画的节点就是 17 次哈希探测。遍历里每个节点都这么来一遍，实测 features
    // 80 个节点 0.054 ms/帧 ≈ 0.68 µs/节点，其中一大半花在"查不到东西"上。
    // drawState() 把这一帧需要的全部可视属性一次取出：
    //   1 次 id→下标（稠密 id 走数组直读快路径）+ 两张动画表（空表直接跳过，
    //   静态页面就是这种）+ 1 次扩展绘制段查询。语义与逐个查询完全一致
    //   （动画结果优先，样式目标兜底）。
    struct DrawState {
        uint32_t color     = 0xFFFFFFFF;
        float    opacity   = 1.0f;
        float    offX      = 0.0f, offY = 0.0f;
        float    scale     = 1.0f;
        float    radius    = -1.0f;   // -1 = 无动画（用节点自身的圆角）
        // v20：绘制期仿射的两维。scaleX/Y 在"只写了等比 scale"时等于 scale，
        // 旋转是**度**、顺时针为正；三者都为默认值时节点走原来的轴对齐路径。
        float    scaleX    = 1.0f;
        float    scaleY    = 1.0f;
        float    rotateDeg = 0.0f;
        bool     disabled  = false;
        const BinUIPaintNode* paint = nullptr;   // 扩展绘制段（文字色/边框/渐变…）
        // 是否需要走仿射绘制（非等比缩放或旋转；等比缩放+平移仍走亚像素轴对齐路径）
        bool needsAffine() const {
            return rotateDeg != 0.0f || scaleX != scaleY;
        }
    };
    DrawState drawState(uint16_t nodeId) const;

    // 节点这一帧的失效矩形：布局矩形 ∪ 动画位移/缩放后的包围盒，各边再外扩 pad。
    // 只按布局矩形失效，:hover 放大时"长出来"的那一圈（边框/底色）不会被重绘，
    // 表现为"边框被削掉一半"；缩回时旧位置也会留残影。返回 false = 节点不存在/不可见。
    bool invalidRect(uint16_t nodeId, float& x, float& y, float& w, float& h,
                     float pad = 2.0f) const;

    // ============ 状态样式（v11：CSS :hover/:active/:focus/:checked） ============
    // 编译期把伪类规则编译成"状态目标"：指针/焦点/勾选变化时渲染器切换目标值，
    // 声明了 transition 的节点走过渡通道（否则瞬时）。未声明状态样式的节点仍按
    // 旧行为对按钮做 hover tint。
    struct StateStyle {
        uint8_t  fields = 0;      // BinUIStateField
        uint32_t color = 0;       // BINUI_STF_COLOR
        float    scale = 1.0f;    // BINUI_STF_SCALE
        float    radius = -1.0f;  // BINUI_STF_RADIUS
    };
    // 状态下标：0=hover 1=active 2=focus 3=checked
    static constexpr int kStateHover = 0, kStateActive = 1, kStateFocus = 2, kStateChecked = 3;
    const std::unordered_map<uint16_t, std::array<StateStyle, 4>>& stateStyles() const {
        return stateCSS_;
    }
    const StateStyle* stateStyle(uint16_t nodeId, uint8_t state) const;   // 未声明返回 nullptr
    const StateStyle* activeStateStyle(uint16_t nodeId) const;            // 当前生效（active>focus>checked>hover）
    // 勾选状态（:checked）：核心库没有勾选控件，由外壳按自己的控件语义调用
    void setNodeChecked(uint16_t nodeId, bool checked);
    bool isNodeChecked(uint16_t nodeId) const { return checkedNodes_.count(nodeId) != 0; }

private:
    void commitTextEvent(); // 编辑值变化时派发 INPUT 事件
    void applyFocus(int nodeId); // 按节点类型聚焦/失焦
    void dragScroll(float px, float py); // 滚动条拖动
    // 元素缩放：进入 / 拖一步 / 收尾（press / setHover / release 内部）
    void beginResize(uint16_t nodeId, uint8_t handle, float px, float py);
    void dragResize(float px, float py);
    // 累加祖先滚动偏移（布局坐标 → 屏幕坐标）；点落在某个滚动祖先的视口外返回 false
    bool screenOffsetOf(size_t nodeIndex, float px, float py, float& ox, float& oy) const;
    void relayout(int width, int height); // 运行时重排（百分比/流式/flex；无规格段仅同步根）
    // 局部重排的共用算法：从 startIdx 起按规格段重算这棵子树（relayout 也从根走它）
    void relayoutFrom(size_t startIdx);
    void clampScrollOffsets();            // 重排后把滚动偏移钳回内容范围
    // 子树在屏幕坐标下的包围盒（缩放拖拽算脏矩形用；含祖先滚动偏移、不做视口裁剪）
    bool subtreeScreenBounds(uint16_t nodeId, float& l, float& t, float& r, float& b) const;
    // 文本节点的"自然高度"：heightAuto 的 LABEL/LIST/BUTTON 按给定宽度重算行数。
    // setText（换文本）与重排（换宽度）共用同一份口径；不是这类节点返回 -1。
    float autoTextHeight(size_t idx, float width) const;
    // 按根节点底色亮度派生滚动条默认色（浅底浅色、深底深色）
    void deriveScrollbarStyleFromRoot();
    BinUIScrollbarStyle scrollbarStyle_;
    bool scrollbarStyleExplicit_ = false;  // true = setScrollbarStyle 指定过，不再自动派生
    std::vector<NodeInfo> nodes_;
    std::unordered_map<uint16_t, size_t> nodeIndexById_;
    // 节点 id → nodes_ 下标（static_cast<size_t>(-1) = 不存在）。
    // 绘制热路径用它替代 nodeIndexById_：编译器分配的 id 稠密且与下标同序，
    // 所以快路径是一次数组直读 + 一次 id 比对（动态增删破坏了顺序才落回哈希表）。
    size_t nodeIndexOf(uint16_t nodeId) const;
    std::unordered_map<std::string, uint16_t> nameToId_; // v8 名称表：htmlId -> nodeId
    std::vector<uint8_t> layoutBytes_;                   // 当前布局（组件合并的基底）
    std::vector<std::string> imagePaths_;                // v12 资源段：imageIndex -> 路径
    std::vector<ComponentRef> componentRefs_;            // v12 组件引用段：运行期挂载点
    std::vector<BinUIPaintNode> paints_;                 // v13 扩展绘制段
    std::unordered_map<uint16_t, size_t> paintIdx_;      // nodeId -> paints_ 下标
    std::vector<PathRecord> paths_;                      // v15 路径段
    std::unordered_map<uint16_t, size_t> pathIdx_;       // nodeId -> paths_ 下标
    // v19 路径条带的跨帧缓存（条带数量有上限，条目数也封顶）。
    // mutable：三个绘制后端拿到的是 const renderer，缓存属于"绘制期副作用"。
    mutable std::unordered_map<uint16_t, PathBandCacheEntry> pathBands_;
    mutable uint64_t pathCacheGen_ = 1;                  // 布局代数（变化即整表作废）
    static constexpr size_t kMaxPathCacheEntries = 256;
    // 边框宽度快照（与 paints_ 同下标）：ui_scale="fixed" 子树在等比缩放时按比例重设边框，
    // 每次都从这份原始值算，避免多次 setSize 累乘
    std::vector<std::array<uint8_t, 4>> designBorderW_;
    std::unordered_set<uint16_t> clipOnly_;              // PAINT_CLIP_ONLY 的节点（overflow:hidden）
    std::unordered_set<uint16_t> mountedAnchors_;        // 已挂载组件的锚点（幂等）
    uint16_t nextNodeId_ = 0;                            // v8 动态节点 id 分配
    std::unordered_map<std::string, ImageResource> images_;
    std::vector<std::string> imageOrder_;
    std::vector<uint32_t> imageGen_; // 与 imageOrder_ 平行的换图版本号
    std::unordered_map<std::string, FontResource> fonts_;
    EventCallback eventCallback_;
    EditState edit_;
    WindowDesc winDesc_;
    int hoverId_ = -1;
    int pressedId_ = -1;
    int keyboardId_ = -1;
    bool   focusRing_ = false;         // 焦点环是否点亮（2 秒无操作自动熄灭）
    double focusRingHideAt_ = 0;       // 自动熄灭时刻（AnimNow 时钟）
    bool   editRing_ = false;          // 编辑中的蓝环要不要画：只有键盘进编辑才画
    std::unordered_map<uint16_t, ScrollState> scroll_;
    int scrollDragNode_ = -1;   // 正在拖动滚动条（thumb 位置偏移）
    float scrollDragGrab_ = 0;
    bool scrollDragVert_ = true;
    // 元素缩放拖拽（v21）：按下时快照原矩形 + 祖先滚动偏移，拖动中按屏幕位移改节点矩形
    int     resizeNode_ = -1;
    uint8_t resizeHandle_ = 0;      // BinUIResizeFlag（正在拖的那条边/角）
    float   resizeStartX_ = 0, resizeStartY_ = 0;
    float   resizeOrigX_ = 0, resizeOrigY_ = 0, resizeOrigW_ = 0, resizeOrigH_ = 0;
    float   resizeOffX_ = 0, resizeOffY_ = 0;  // 按下时节点的祖先滚动偏移（屏幕坐标换算基准）
    float   resizeDirty_[4] = {0, 0, 0, 0};    // 拖动脏矩形（屏幕坐标）
    bool    resizeDirtyOn_ = false;
    // 尺寸覆盖（v21）：宿主拖过尺寸的节点，重排时保持这个矩形不被规格段改写
    std::unordered_map<uint16_t, std::array<float, 4>> boundsOverride_;
    // 缩放规格段（v3）：节点下标 -> 规格；flex 容器 id -> 容器规格
    std::vector<const BinUISpecNode*> specOf_;
    std::unordered_map<uint16_t, const BinUISpecFlex*> flexOf_;
    std::vector<uint8_t> specData_;
    bool hasSpecs_ = false;

    // 等比 UI 缩放（ui_scale="proportional"）：resize 时整棵节点树随窗口缩放，
    // 节点坐标/字号/圆角写回 nodes_（命中/绘制/滚动自动一致），不参与重排。
    bool proportionalScale_ = false;
    float designW_ = 0, designH_ = 0;   // 设计尺寸（loadLayout 时根节点尺寸）
    float scaleX_ = 1, scaleY_ = 1;     // 当前窗口 / 设计尺寸
    std::vector<NodeInfo> designNodes_; // 设计坐标快照（缩放基准）

    // ============ 视频运行时（v9） ============
    std::vector<VideoState> videos_;
    std::unordered_map<uint16_t, size_t> videoIdxByNode_;
    // 播放段基准：nodeId -> {起始位置秒, 起始 animNow 时钟}；暂停/结束即移除
    std::unordered_map<uint16_t, std::pair<double, double>> vidClock_;
    bool    vidAutoplayDone_ = false; // autoplay 只在首帧步进时触发一次
    int     vidPressNode_ = -1;  // 按下的视频节点
    int     vidPressRegion_ = 0; // 0=画面体 1=播放/暂停按钮 2=进度条
    bool    vidScrubbing_ = false;

    void stepVideos(double now);                    // 推进时钟/回绕/ENDED（stepAnimations 调用）
    void videoPress(float x, float y, int nodeId);  // press 内部：记录控制条按下区域
    bool videoRelease(float x, float y, int nodeId);// release 内部：处理控制条点击；true=已消费
    VideoState* findVideoMut(uint16_t nodeId);
    void emitVideoEvent(EventType t, uint16_t nodeId);

public:
    // 控制条几何（渲染器命中与后端绘制共用同一布局）
    struct VideoBarGeo {
        float x, y, w, h;        // 条矩形
        float btnX, btnY, btnS;  // 播放/暂停按钮（方形）
        float trX, trW, trY, trH;// 进度轨道
        float thumbR;            // 滑块半径
    };
    static constexpr float kVideoBarH = 36.0f;
    static bool VideoBarLayout(const NodeInfo& n, VideoBarGeo& g); // 有控制条时输出几何并返回 true

private:
    // ============ 动画引擎（v7） ============
    struct KeyGroup { std::vector<BinUIKeyFrame> kfs; }; // 已按 percent 排序
    std::vector<KeyGroup> keyGroups_;
    std::vector<BinUIAnimDef> animDefs_;
    std::vector<BinUITransition> transitions_;

    // 每节点运行时状态
    struct AnimRuntime {
        struct KF {            // 关键帧动画实例
            const BinUIAnimDef* def;
            double start;      // 绝对时钟（含 delay）
            double total;      // 全部迭代时长（秒；infinite = 很大值）
            bool    infinite;
            // 动画事件去重：已报过的迭代次数 / 是否已报过结束（v23）
            uint32_t iterDone = 0;
            bool     ended = false;
        };
        std::vector<KF> anims;
        struct TR {            // 过渡实例（完成/被取代后移除）
            uint16_t prop;
            double   start, end;
            uint8_t  easing;
            float    e0x, e0y, e1x, e1y;
            float    from[2], to[2];   // x/y（标量属性只用 [0]）
            uint32_t fromC, toC;
            bool     isColor;
        };
        std::vector<TR> trans;
        // 样式目标（hover/按下/运行时设置）：过渡的终点；无过渡时立即生效
        std::unordered_map<uint16_t, float>  floatTargets;
        std::unordered_map<uint16_t, uint32_t> colorTargets;
    };
    std::unordered_map<uint16_t, AnimRuntime> animState_;

    // 每帧计算的可视结果（供绘制查询 + 过渡 from 基准）
    struct AnimResult {
        bool   active = false;
        bool   opSet = false;    float opacity = 1.0f;
        bool   offSet = false;   float offX = 0, offY = 0;
        // 单轴平移（ANIM_TRANSLATE_X / _Y）：与 offSet 互不覆盖
        bool   offXSet = false;
        bool   offYSet = false;
        bool   scaleSet = false; float scale = 1.0f;
        bool   colorSet = false; uint32_t color = 0;
        bool   radiusSet = false; float radius = -1;
        // v20：非等比缩放 + 旋转（写法见 BinUIKeyFrame 的注释）
        bool   kSet = false;     float kx = 1.0f, ky = 1.0f;
        bool   rotSet = false;   float rot = 0.0f;
    };
    std::unordered_map<uint16_t, AnimResult> animResult_;
    // 动画扫过的极值（缩放/平移）。当前值不足以描述"刚从放大缩回来"的那一帧：
    // 放大时在最外沿画过的像素，缩小到原尺寸后没有任何矩形去擦，就成了边缘残影。
    struct AnimExtent {
        float smin = 1.0f, smax = 1.0f;
        float dxmin = 0.0f, dxmax = 0.0f, dymin = 0.0f, dymax = 0.0f;
    };
    std::unordered_map<uint16_t, AnimExtent> animExtent_;
    // v11 状态样式：nodeId -> 4 个状态目标（hover/active/focus/checked）
    std::unordered_map<uint16_t, std::array<StateStyle, 4>> stateCSS_;
    std::unordered_set<uint16_t> checkedNodes_;   // :checked 运行时开关
    double animT0_ = 0;          // 布局加载时刻（关键帧动画基准）
    double animNow_ = 0;         // 最近一次步进时钟
    bool   animTimeInit_ = false;
    std::vector<uint16_t> animChanged_; // stepAnimations 输出缓存
    std::vector<AnimEvent> pendingAnimEvents_; // 待取的动画事件（takeAnimEvents 取走并清空）

    void refreshNodeStyle(int nodeId); // hover/按下/焦点变化后重算颜色/缩放/圆角目标
    void commitFocus(int newId, bool showRing = true); // 键盘焦点变更（同时刷新两端的 :focus 状态样式）
    void bumpFocusRing();              // 点亮焦点环并重置无操作计时
    int scrollTargetNode() const;      // 方向键滚谁：最近的可滚动祖先，否则视口内面积最大者
    double focusRingIdle_ = kFocusRingIdleSec;
    bool hasTransition(uint16_t nodeId, uint16_t prop) const;
    void startTransition(uint16_t nodeId, uint16_t prop, double now,
                         float fromX, float fromY, float toX, float toY,
                         uint32_t fromC, uint32_t toC);
    void startKeyframeAnim(uint16_t nodeId, const BinUIAnimDef& def);
    void computeAnimResult(uint16_t nodeId); // 计算某节点当前可视属性
    static bool AnimResultEqual(const AnimResult& a, const AnimResult& b);
    static void ApplyKeyFrameProps(const BinUIKeyFrame& kf, uint16_t mask, AnimResult& o);
    static void InterpKeyFrames(const BinUIKeyFrame* kfs, size_t n, uint16_t mask, float e, AnimResult& o);
    static void EvalKeyFrameAnim(const BinUIKeyFrame* kfs, size_t n, const BinUIAnimDef& def,
                                 double start, double now, AnimResult& o);
};

} // namespace binui
