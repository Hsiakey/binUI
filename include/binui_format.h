#pragma once
#include <cstdint>

namespace binui {

constexpr uint32_t BINUI_MAGIC      = 0x554E4942; // 'BINU' 原生/手写二进制格式
constexpr uint32_t BINUI_HTML_MAGIC = 0x4D544858; // 'XHTM' 由 XML 或 HTML5 编译（.xhtml）
// v3: 缩放规格段；v4: 节点 radius；v5: 窗口描述段；v6: 规格节点 min/max 尺寸约束
// v7: 动画段；v8: 名称表；v9: 视频描述段（<video>，可选编解码模块见 include/binui_video.h）
// v10: 节点 zIndex（CSS z-index 简化实现：数值越大越靠上，同值下 absolute 高于流式）
// v11: 状态样式段（CSS :hover/:active/:focus/:checked 的运行时切换目标）
// v12: 资源段（imageIndex -> 图片路径，布局自描述）+ 组件引用段（外挂 .bum 的锚点与路径）
constexpr uint32_t BINUI_VERSION    = 12;

enum BinUINodeType : uint8_t {
    BINUI_ROOT   = 0,
    BINUI_LABEL  = 1,
    BINUI_IMAGE  = 2,
    BINUI_BUTTON = 3,
    BINUI_INPUT  = 4,
    BINUI_LIST   = 5,
    BINUI_LINE   = 6,   // 线段：x,y=起点；width,height=终点；radius=线宽
    BINUI_SVG    = 7,   // <svg> 透明容器（子图形按容器坐标系定位，绘制裁剪到容器）
    BINUI_CANVAS = 8,   // <canvas> 绘制面：绘制其矩形 + 触发应用侧 canvas 回调
    BINUI_VIDEO  = 9,   // <video>：帧像素经伪 imageIndex 推送（updateImageRaw），控制条由后端绘制
};

enum BinUINodeFlag : uint8_t {
    BINUI_VISIBLE     = 1 << 0,
    BINUI_DISABLED    = 1 << 1,
    BINUI_INTERACTIVE = 1 << 2,
    BINUI_SCROLLABLE  = 1 << 3, // 滚动容器（overflow:auto/scroll/hidden），内容可裁剪滚动
    BINUI_ABSOLUTE    = 1 << 4, // 绝对定位（HTML position:absolute/left+top），绘制在流式内容之上
    BINUI_TITLEBAR    = 1 << 5, // 自绘标题栏（<titlebar>），其非交互子区域可拖动窗口
    // 文本水平对齐（text-align / XML align 属性）：两位互斥，都不置位 = 左对齐。
    // 复用 flags 的空闲位，不新增节点字段 —— 旧布局这两位为 0，天然是左对齐，无需升版。
    BINUI_TEXT_CENTER = 1 << 6,
    BINUI_TEXT_RIGHT  = 1 << 7,
};

#pragma pack(push, 1)
struct BinUIHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t nodeCount;
    uint32_t stringSize;
};

struct BinUINode {
    uint16_t id;
    uint16_t parentId;
    uint8_t  type;
    uint8_t  flags;
    float    x;
    float    y;
    float    width;
    float    height;
    uint32_t color;
    uint32_t textOffset;
    uint16_t imageIndex;
    uint16_t fontSize;
    uint16_t padding;   // 内边距（文本绘制内缩量）
    uint16_t radius;    // 圆角半径（像素；0 = 直角，未指定 border-radius 时默认 0）
    int16_t  zIndex;    // v10 层叠顺序：数值越大绘制越靠后（越靠上）；0 = 默认
};
#pragma pack(pop)

static_assert(sizeof(BinUIHeader) == 16, "BinUIHeader must be 16 bytes");
static_assert(sizeof(BinUINode) == 40, "BinUINode must be 40 bytes");

// ============================================================
// 缩放规格段（v3 追加在字符串表之后；运行时 setSize 重排依据）
// 编译器为每个节点输出一条 BinUISpecNode；flex 容器额外输出 BinUISpecFlex。
// 布局：uint32 specCount + BinUISpecNode[specCount]
//       + uint32 flexCount + BinUISpecFlex[flexCount]
// 无规格段（手写 BINUI / 默认布局）＝静态布局，setSize 仅同步根节点尺寸。
// ============================================================
enum BinUISpecFlag : uint16_t {
    SPEC_PCT_X    = 1 << 0, // pctX 有效（相对父内容盒宽）
    SPEC_PCT_Y    = 1 << 1, // pctY 有效（相对父内容盒高）
    SPEC_PCT_W    = 1 << 2, // pctW 有效
    SPEC_PCT_H    = 1 << 3, // pctH 有效
    SPEC_FLOW_Y   = 1 << 4, // 流式排布：y 由父容器流光标决定（未声明 top/y）
    SPEC_AUTO_W   = 1 << 5, // 宽度自动填满父内容盒（未声明 width/w）
    SPEC_IN_FLEX  = 1 << 6, // flex 子项：位置/尺寸由 flex 算法重算
    SPEC_MINW_PCT = 1 << 7, // minW 为父内容盒宽百分比（0..1）
    SPEC_MAXW_PCT = 1 << 8, // maxW 为父内容盒宽百分比（0..1）
    SPEC_MINH_PCT = 1 << 9, // minH 为父内容盒高百分比（0..1）
    SPEC_MAXH_PCT = 1 << 10,// maxH 为父内容盒高百分比（0..1）
};

#pragma pack(push, 1)
struct BinUISpecNode {
    uint16_t nodeId;
    uint16_t flags;        // BinUISpecFlag
    uint8_t  flexWeight;   // SPEC_IN_FLEX：弹性权重（0 = 固定/百分比尺寸）
    uint8_t  reserve[3];
    float    relX, relY;   // 编译期相对父内容盒坐标（流式节点即 margin 偏移；无 % 时作锚点）
    float    pctX, pctY, pctW, pctH; // 0..1，无效为 -1
    float    ml, mr, mt, mb;         // 流式 margin（SPEC_FLOW_Y / SPEC_AUTO_W 时有效）
    float    minW, maxW, minH, maxH; // 尺寸约束（v6，min/max-width/height）；-1 = 未指定，SPEC_*_PCT 时按父内容盒百分比解析
    float    relW, relH;             // v6：显式像素宽/高声明值（未钳制前）；auto/%/flex 尺寸为 -1（运行时重排基准）
};
#pragma pack(pop)
static_assert(sizeof(BinUISpecNode) == 72, "BinUISpecNode must be 72 bytes");

#pragma pack(push, 1)
struct BinUISpecFlex {
    uint16_t nodeId;   // flex 容器节点 id
    uint8_t  dir;      // 0=row 1=column
    uint8_t  justify;  // 0=flex-start 1=flex-end 2=center 3=space-between 4=space-around
    uint8_t  align;    // 0=flex-start 1=center 2=flex-end
    uint8_t  autoH;    // 1=容器高度由内容决定（未声明 height）
    uint8_t  reserve[2];
    float    gap;
};
#pragma pack(pop)
static_assert(sizeof(BinUISpecFlex) == 12, "BinUISpecFlex must be 12 bytes");

// ============================================================
// 窗口描述段（v5 追加在缩放规格段之后）
// <app_window> 是编译期包裹层，不进节点树；编译器把它的属性编成这段，
// 外壳通过 renderer.windowDesc() 读取后创建窗口。
// resizable=false → 禁缩放；aspect>0（BINUI_WIN_ASPECT）→ 仅等比例缩放；
// <app_window window_resize="allow|disable|aspect"> 为统一三态控制，覆盖前两者，
// aspect 未显式给比例时按窗口初始宽高比兜底。
// 布局：uint32 winDescBytes + BinUIWindowDesc + char title[titleLen]（UTF-8 无 '\0'）
//       winDescBytes == 0 表示无窗口描述（外壳回退节点根尺寸）。
// ============================================================
enum BinUIWindowFlag : uint16_t {
    BINUI_WIN_RESIZABLE   = 1 << 0, // 可调整宽高
    BINUI_WIN_ASPECT      = 1 << 1, // 等比缩放（aspect 有效）
    BINUI_WIN_TRANSPARENT = 1 << 2, // 窗口级透明（分层窗口，画布 alpha 透出桌面）
    BINUI_WIN_SCALE       = 1 << 3, // 等比 UI 缩放（ui_scale="proportional"：resize 时整棵节点树随窗口缩放）
};

#pragma pack(push, 1)
struct BinUIWindowDesc {
    uint32_t width;      // 窗口初始尺寸（画布大小）
    uint32_t height;
    uint16_t flags;      // BinUIWindowFlag
    uint16_t titleLen;   // 标题 UTF-8 字节数（紧跟结构体后）
    float    aspect;     // 等比 w/h（BINUI_WIN_ASPECT 时有效）
    uint32_t bgColor;    // 画布背景 AARRGGBB（与根节点 color 一致）
};
#pragma pack(pop)
static_assert(sizeof(BinUIWindowDesc) == 20, "BinUIWindowDesc must be 20 bytes");

// ============================================================
// 动画段（v7 追加在窗口描述段之后，布局的最后一个可选段）
// 布局：uint32 animBytes（0 = 无动画段；否则为紧跟的动画数据字节数）
//   动画数据：
//     uint32 kfGroupCount
//     每组：uint32 kfCount + BinUIKeyFrame[kfCount]
//     uint32 animDefCount + BinUIAnimDef[animDefCount]
//     uint32 transitionCount + BinUITransition[transitionCount]
// 无动画段 = 静态/无动画布局。
// ============================================================

// 可动画属性位（transition/animation 的 propMask；同一段内的关键帧可只含部分属性）
enum BinUIAnimProp : uint16_t {
    ANIM_OPACITY   = 1 << 0, // 透明度（1 = 不透明）
    ANIM_TRANSLATE = 1 << 1, // 平移 (tx, ty) 像素
    ANIM_SCALE     = 1 << 2, // 缩放（1 = 原尺寸）
    ANIM_BG_COLOR  = 1 << 3, // 背景/文本颜色（节点 color，AARRGGBB）
    ANIM_RADIUS    = 1 << 4, // 圆角半径（像素）
};

enum BinUIEasing : uint8_t {
    BINUI_EASE_LINEAR   = 0,
    BINUI_EASE_EASE     = 1, // 默认缓动（cubic-bezier(.25,.1,.25,1)）
    BINUI_EASE_IN       = 2,
    BINUI_EASE_OUT      = 3,
    BINUI_EASE_IN_OUT   = 4,
    BINUI_EASE_CUBIC    = 5, // 自定义贝塞尔（e0x/e0y/e1x/e1y 控制点）
};

enum BinUIAnimDir : uint8_t {
    BINUI_ANIM_NORMAL        = 0,
    BINUI_ANIM_REVERSE       = 1,
    BINUI_ANIM_ALTERNATE     = 2,
    BINUI_ANIM_ALT_REVERSE   = 3,
};

enum BinUIAnimFill : uint8_t {
    BINUI_ANIM_FILL_NONE      = 0, // 动画结束后恢复原始值
    BINUI_ANIM_FILL_FORWARDS  = 1, // 结束后保持最后一帧
    BINUI_ANIM_FILL_BACKWARDS = 2, // 延迟期间应用第一帧
    BINUI_ANIM_FILL_BOTH      = 3,
};

#pragma pack(push, 1)
struct BinUIKeyFrame {
    uint16_t percent;   // 0..10000（0 = from，10000 = to）
    uint16_t propMask;  // 本帧携带的 BinUIAnimProp
    float    opacity;   // ANIM_OPACITY
    float    tx, ty;    // ANIM_TRANSLATE
    float    scale;     // ANIM_SCALE
    float    radius;    // ANIM_RADIUS
    uint32_t bgColor;   // ANIM_BG_COLOR（AARRGGBB）
};

struct BinUIAnimDef {
    uint16_t nodeId;
    uint16_t groupIdx;  // 关键帧组下标
    uint16_t propMask;  // 本动画驱动的属性
    uint8_t  easing;    // BinUIEasing
    uint8_t  direction; // BinUIAnimDir
    uint16_t iteration; // 迭代次数（0 = 无限）
    uint8_t  fillMode;  // BinUIAnimFill
    uint8_t  reserve;
    uint32_t durationMs;
    uint32_t delayMs;
    float    e0x, e0y, e1x, e1y; // BINUI_EASE_CUBIC 控制点
};

struct BinUITransition {
    uint16_t nodeId;
    uint16_t propMask;
    uint8_t  easing;
    uint8_t  reserve[3];
    uint32_t durationMs;
    uint32_t delayMs;
    float    e0x, e0y, e1x, e1y;
};
#pragma pack(pop)
static_assert(sizeof(BinUIKeyFrame) == 28, "BinUIKeyFrame must be 28 bytes");
static_assert(sizeof(BinUIAnimDef) == 36, "BinUIAnimDef must be 36 bytes");
static_assert(sizeof(BinUITransition) == 32, "BinUITransition must be 32 bytes");

// ============================================================
// 视频描述段（v9，恒定写出，位于动画段之后、名称段之前）
// 布局：uint32 vidBytes（0 = 无视频）+ uint32 vidCount + BinUIVideoDesc[vidCount]
// <video> 编译为 BINUI_VIDEO 节点 + 本段描述；解码由可选模块
// （BINUI_ENABLE_VIDEO，FFmpeg）完成：解出 RGBA 帧 → updateImageRaw(imageIndex)
// 推送到伪图片槽，绘制后端按 imageGeneration 自动重传纹理。
// 无解码模块时节点仅绘制底色（优雅降级），核心库不依赖 FFmpeg。
// ============================================================
enum BinUIVideoFlag : uint16_t {
    VID_AUTOPLAY = 1 << 0, // autoplay 属性
    VID_LOOP     = 1 << 1, // loop 属性
    VID_MUTED    = 1 << 2, // muted 属性（运行时可改）
    VID_CONTROLS = 1 << 3, // controls 属性（浏览器式控制条）
};

#pragma pack(push, 1)
struct BinUIVideoDesc {
    uint16_t nodeId;      // 对应 BINUI_VIDEO 节点 id
    uint16_t imageIndex;  // 伪图片槽（解码帧经 updateImageRaw 写入）
    uint16_t flags;       // BinUIVideoFlag
    uint16_t srcOffset;   // src 路径在字符串表中的偏移（0 = 无源）
    float    volume;      // 音量 0..1（默认 1）
};
#pragma pack(pop)
static_assert(sizeof(BinUIVideoDesc) == 12, "BinUIVideoDesc must be 12 bytes");

// ============================================================
// 状态样式段（v11，恒定写出，位于视频描述段之后、名称段之前）
// 布局：uint32 stateBytes（0 = 无状态样式）+ uint32 stateCount + BinUIStateStyle[stateCount]
//
// CSS 伪类 :hover/:active/:focus/:checked 的声明被编译成"状态样式"：渲染器在
// 指针/焦点/勾选状态变化时按这些条目切换节点的目标颜色/缩放/圆角（无过渡时瞬时
// 生效，声明了 transition 时走 v7 过渡通道）。
// 结构伪类（:first-child/:nth-child/…）与伪元素（::before/::after）在编译期
// 展开成节点或普通样式，不占用本段。
// ============================================================
enum BinUIStateFlag : uint8_t {
    BINUI_ST_HOVER   = 1 << 0, // 指针悬停
    BINUI_ST_ACTIVE  = 1 << 1, // 按下（左键按住）
    BINUI_ST_FOCUS   = 1 << 2, // 键盘焦点 / 正在编辑
    BINUI_ST_CHECKED = 1 << 3, // 勾选（需外壳调用 setNodeChecked；核心库无勾选控件）
};

enum BinUIStateField : uint8_t {
    BINUI_STF_COLOR  = 1 << 0, // color 有效（AARRGGBB）
    BINUI_STF_SCALE  = 1 << 1, // scale 有效（1 = 原尺寸）
    BINUI_STF_RADIUS = 1 << 2, // radius 有效（负值 = 不改变）
};

#pragma pack(push, 1)
struct BinUIStateStyle {
    uint16_t nodeId;
    uint8_t  state;   // BinUIStateFlag（单条目只描述一个状态）
    uint8_t  fields;  // BinUIStateField
    uint32_t color;   // BINUI_STF_COLOR
    float    scale;   // BINUI_STF_SCALE
    float    radius;  // BINUI_STF_RADIUS
};
#pragma pack(pop)
static_assert(sizeof(BinUIStateStyle) == 16, "BinUIStateStyle must be 16 bytes");

// ============================================================
// 资源段（v12，恒定写出，位于状态样式段之后、组件引用段之前）
// 布局：uint32 imgBytes（0 = 无资源）+ uint32 imgCount + uint32 pathOffset[imgCount]
// pathOffset 指向字符串表（UTF-8 路径，按 imageIndex 顺序）。
//
// 有了这段，布局就是自描述的：外壳不必再依赖编译器返回的图片列表就能按
// imageIndex 加载资源；组件二进制（.bum）合并进宿主时，资源下标在这里整体重排。
// ============================================================

// ============================================================
// 组件引用段（v12，恒定写出，位于资源段之后、名称段之前）
// 布局：uint32 compBytes（0 = 无外挂组件）+ uint32 compCount + BinUIComponentRef[compCount]
//
// <use src="x.bum" external> 表示"运行期挂载"：编译器留一个占位节点（锚点），
// 把组件路径记进本段；外壳读取后用 renderer.mountComponent() 把组件布局挂上去。
// 未标 external 的 <use> 一律在编译期合并进单个 .bui（见 docs/components.md）：
// 源码形式编译期内联（可参数化），二进制形式由链接器离线合并。
// ============================================================
#pragma pack(push, 1)
struct BinUIComponentRef {
    uint16_t anchorId;   // 宿主里的占位节点 id（组件挂到它下面）
    uint8_t  flags;      // 保留（0）
    uint8_t  reserved;
    uint32_t srcOffset;  // 组件路径在字符串表中的偏移（UTF-8）
};
#pragma pack(pop)
static_assert(sizeof(BinUIComponentRef) == 8, "BinUIComponentRef must be 8 bytes");

} // namespace binui
