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

    // ============ 键盘导航（方向键） ============
    enum class KeyDirection { UP, DOWN, LEFT, RIGHT };

    int keyboardNode() const { return keyboardId_; }           // 当前键盘焦点节点（-1 无）
    bool isKeyboardFocused(uint16_t nodeId) const { return keyboardId_ == (int)nodeId; }
    void setKeyboardFocus(int nodeId);                         // 直接设置焦点（-1 清除）
    int moveKeyboardFocus(KeyDirection dir);                   // 向方向键移动焦点，返回新焦点（-1 无候选）
    int cycleKeyboardFocus(bool forward = true);               // Tab 循环：按文档顺序在可交互节点间移动（Shift+Tab 反向），返回新焦点
    bool activateKeyboardFocus();                              // Enter/Space：INPUT 进入编辑，其余触发 CLICK

    // ============ 滚动容器 ============
    struct ScrollState { float x = 0, y = 0; };

    bool isScrollable(uint16_t nodeId) const;
    void scrollTo(uint16_t nodeId, float x, float y);          // 设置滚动偏移（钳制到内容范围）
    void scrollBy(uint16_t nodeId, float dx, float dy);
    const ScrollState* scrollState(uint16_t nodeId) const;     // 滚动偏移（非滚动容器返回 nullptr）
    int scrollAt(float x, float y, float dy);                  // 滚轮：滚动光标处最顶层滚动容器，返回其 id
    float contentHeight(uint16_t nodeId) const;                // 内容高度（子节点下界 - 容器顶）
    int hitScrollBar(float x, float y, bool& vertical) const;  // 命中滚动条/轨道：返回容器 id（-1 无）
    bool scrollBarHitTest(float x, float y) const;             // 点是否在滚动条区域（测试用）

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
    bool isFocused(uint16_t nodeId) const { return edit_.active && edit_.nodeId == nodeId; }
    const EditState& edit() const { return edit_; }

    void focus(uint16_t nodeId);       // 开始编辑会话（仅 INPUT 节点，其他情况失焦）
    void blur();                       // 提交当前值到节点文本并结束会话
    bool insertText(const std::string& utf8);          // 在光标处插入（替换选区）
    bool backspace();                                  // 删除光标前字符（或整个选区）
    bool deleteForward();                              // 删除光标后字符
    bool moveCaret(int deltaChars, bool extend = false); // 按字符移动光标，extend 时保留选区起点
    bool setCaret(size_t byteOffset);                  // 设置光标（钳制到字符边界）
    bool setCaretByUtf16Index(size_t utf16Index);      // 后端测量后按 UTF-16 索引定位

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

    // 绘制查询：节点当前应显示的颜色（含 hover/按下 tint、过渡与关键帧动画）
    uint32_t drawColor(uint16_t nodeId) const;
    float drawOpacity(uint16_t nodeId) const;   // 1 = 不透明
    float drawOffsetX(uint16_t nodeId) const;   // 视觉平移 x
    float drawOffsetY(uint16_t nodeId) const;   // 视觉平移 y
    float drawScale(uint16_t nodeId) const;     // 1 = 原尺寸（绕中心缩放）
    float drawRadius(uint16_t nodeId) const;    // 圆角半径（-1 = 无动画，用节点原值）

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
    void relayout(int width, int height); // 运行时重排（百分比/流式/flex；无规格段仅同步根）
    std::vector<NodeInfo> nodes_;
    std::unordered_map<uint16_t, size_t> nodeIndexById_;
    std::unordered_map<std::string, uint16_t> nameToId_; // v8 名称表：htmlId -> nodeId
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
    std::unordered_map<uint16_t, ScrollState> scroll_;
    int scrollDragNode_ = -1;   // 正在拖动滚动条（thumb 位置偏移）
    float scrollDragGrab_ = 0;
    bool scrollDragVert_ = true;
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
        bool   scaleSet = false; float scale = 1.0f;
        bool   colorSet = false; uint32_t color = 0;
        bool   radiusSet = false; float radius = -1;
    };
    std::unordered_map<uint16_t, AnimResult> animResult_;
    // v11 状态样式：nodeId -> 4 个状态目标（hover/active/focus/checked）
    std::unordered_map<uint16_t, std::array<StateStyle, 4>> stateCSS_;
    std::unordered_set<uint16_t> checkedNodes_;   // :checked 运行时开关
    double animT0_ = 0;          // 布局加载时刻（关键帧动画基准）
    double animNow_ = 0;         // 最近一次步进时钟
    bool   animTimeInit_ = false;
    std::vector<uint16_t> animChanged_; // stepAnimations 输出缓存

    void refreshNodeStyle(int nodeId); // hover/按下/焦点变化后重算颜色/缩放/圆角目标
    void commitFocus(int newId);       // 键盘焦点变更（同时刷新两端的 :focus 状态样式）
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
