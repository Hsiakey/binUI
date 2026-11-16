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
// v13: 扩展绘制段（文字色/四边边框/渐变底色/字重/行高/下划线），见 BinUIPaintNode
// v14: 扩展绘制段追加 textFlags（斜体/占位符/只读/初始勾选），BinUIPaintNode 40 -> 44 字节
// v15: 路径段（SVG <path>/<ellipse> 的归一化折线），见 BinUIPathEntry
// v16: 路径渐变（SVG <linearGradient>/<radialGradient> 填充），见 BinUIPathGradient
// v17: 渐变描边（stroke="url(#id)"）+ stroke-linecap="square"，多一条可选渐变记录
// v18: 径向渐变的焦点 fx/fy（BinUIPathGradient 92 -> 100 字节）
// v19: 路径边缘抗锯齿开关（shape-rendering=auto/optimizeQuality → PATH_AA）
// v20: 无格式变化（渲染器内部：非等比缩放的脏矩形按外接圆算）
// v21: 扩展绘制段追加 resizeMask（CSS resize 的边/角位图），BinUIPaintNode 44 -> 48 字节
// v22: flex 属性补齐（flex 简写/flex-shrink/flex-basis/order/align-self/flex-wrap/align-content），
//      BinUISpecNode 72 -> 84 字节；BinUISpecFlex 的两个 reserve 变成 wrap / alignContent（尺寸不变）
// v23: 阴影段（box-shadow），见 BinUIShadow
// v24: 文字阴影段（text-shadow），见 BinUITextShadow
// v25: 绘制附加段（九宫格图片），见 BinUIPaintEx
// v26: 图片滤镜段（CSS filter 的颜色矩阵），见 BinUIImageFilter
// v27: 裁剪路径段（SVG <clipPath> / clip-path），见 BinUIClipEntry
// v28: 遮罩段（SVG <mask> / mask），见 BinUIMaskEntry
// v29: 毛玻璃段（backdrop-filter: blur），见 BinUIBackdropEntry
constexpr uint32_t BINUI_VERSION    = 29;

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
    // v15: SVG 路径。节点 x/y/w/h 就是几何包围盒（有描边时外扩 strokeW/2），
    // 折线数据在同名路径段里（按 nodeId 索引），见 BinUIPathEntry。
    BINUI_PATH   = 10,
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
    // 固定子树（元素属性 ui_scale="fixed"）：ui_scale="proportional" 的窗口里，这个节点**不参与
    // 等比缩放**——自身尺寸按窗口拉伸/缩放，后代保持原有形状（统一按纵向比例缩放 + 贴边/居中），
    // 用来保住"标题栏三颗控制按钮"这类必须是正方形的 chrome。旧布局这一位恒为 0，不升格式版本。
    SPEC_FIXED_SCALE = 1 << 11,
    // v22：flex 子项的交叉轴尺寸是"作者没写、由默认盒高补的"（可被 align-items/align-self
    // 的 stretch 拉伸占满整行）。作者显式写了宽高时该位为 0，stretch 不会盖掉声明值。
    SPEC_CROSS_AUTO  = 1 << 12,
    // v22：该子项的主轴基准尺寸是"内容估算出来的"（width:content / flex-basis:content）。
    // 运行期换文本后可以按同一口径重算，不用把编译期的估算值锁死。
    SPEC_BASIS_CONTENT = 1 << 13,
};

#pragma pack(push, 1)
struct BinUISpecNode {
    uint16_t nodeId;
    uint16_t flags;        // BinUISpecFlag
    uint8_t  flexWeight;   // SPEC_IN_FLEX：弹性权重（0 = 固定/百分比尺寸）
    // heightAuto：1 = 高度是编译期按文本估算撑开的（作者未声明 height），
    //   运行期 setText 换文本后需要按同一口径（include/binui_text_estimate.h）重算高度。
    //   占用旧布局的 reserve[0]：旧二进制这里恒为 0，等价于 heightAuto=0，因此不升
    //   BINUI_VERSION（先例：BINUI_TEXT_CENTER 复用 flags 的空闲位）。
    uint8_t  heightAuto;
    // autoBase：heightAuto 时的**基线高度**（估算之前的高度，即元素默认盒高，像素）。
    //   运行期重算按 max(autoBase, 估算高度) 取，才与编译期完全一致 —— 否则 13px 的
    //   span 会被重算成 17px 而编译期是 20px，短文本反而被压扁。
    //   两者都占用旧的 reserve 字节（旧布局恒为 0 = 不重算、不做基线钳制）。
    uint16_t autoBase;
    float    relX, relY;   // 编译期相对父内容盒坐标（流式节点即 margin 偏移；无 % 时作锚点）
    float    pctX, pctY, pctW, pctH; // 0..1，无效为 -1
    float    ml, mr, mt, mb;         // 流式 margin（SPEC_FLOW_Y / SPEC_AUTO_W 时有效）
    float    minW, maxW, minH, maxH; // 尺寸约束（v6，min/max-width/height）；-1 = 未指定，SPEC_*_PCT 时按父内容盒百分比解析
    float    relW, relH;             // v6：显式像素宽/高声明值（未钳制前）；auto/%/flex 尺寸为 -1（运行时重排基准）
    // ---- v22：flex 子项属性（SPEC_IN_FLEX 时有效，算法见 include/binui_flex_layout.h）----
    uint8_t  flexShrink;   // 收缩因子（CSS 默认 1）：主轴溢出时按 shrink×basis 加权收缩
    uint8_t  alignSelf;    // 交叉轴对齐：0=start 1=center 2=end 3=stretch 0xFF=跟随容器
    uint8_t  flexOrder;    // order：主轴排序键（同值保持文档顺序）
    uint8_t  basisKind;    // flexBasis 的含义：0=无（基准 0）1=像素 2=百分比（0..1，相对主轴内容盒）
    float    flexBasis;    // 主轴基准尺寸（flex-basis 解析后的值）
    float    flexCross;    // 交叉轴基准尺寸（像素；作者的声明值或元素默认盒高）
};
#pragma pack(pop)
static_assert(sizeof(BinUISpecNode) == 84, "BinUISpecNode must be 84 bytes");

#pragma pack(push, 1)
struct BinUISpecFlex {
    uint16_t nodeId;   // flex 容器节点 id
    uint8_t  dir;      // 0=row 1=column
    uint8_t  justify;  // 0=flex-start 1=flex-end 2=center 3=space-between 4=space-around 5=space-evenly
    uint8_t  align;    // 0=flex-start 1=center 2=flex-end 3=stretch（交叉轴，默认 flex-start）
    uint8_t  autoH;    // 1=容器高度由内容决定（未声明 height）
    uint8_t  wrap;       // v22：0=nowrap 1=wrap 2=wrap-reverse
    uint8_t  alignContent; // v22：多行时行间分配 0=start 1=center 2=end 3=between 4=around 5=evenly 6=stretch
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
    // v20：非均匀缩放与旋转。三者合起来组成绘制期的 2D 仿射
    // （绕矩形中心：先缩放、再旋转、最后平移），用来做"形变 + 自转"。
    // 只有 kx/kx 或 rot 生效的节点才走仿射绘制路径（见 binui_draw_nodes.h），
    // 静态节点与"只平移/等比缩放"的节点仍走原来的亚像素路径 —— 像素中性。
    ANIM_SCALE_X   = 1 << 5, // 横向缩放（1 = 原宽）
    ANIM_SCALE_Y   = 1 << 6, // 纵向缩放（1 = 原高）
    ANIM_ROTATE    = 1 << 7, // 旋转角度（度，顺时针为正）
    // 单轴平移（过渡 / 运行时驱动用）。ANIM_TRANSLATE 只能同时改 x 和 y，
    // 想"只上下滑 24px"就必须有独立的轴通道。
    ANIM_TRANSLATE_X = 1 << 8,
    ANIM_TRANSLATE_Y = 1 << 9,
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
    // v20：transform 的另外两维（非均匀缩放 + 旋转）。scaleX/Y 与 scale 是**两种写法**：
    // `scale(s)` 写等比（mask=ANIM_SCALE），`scale(sx, sy)` 写非等比（mask=ANIM_SCALE_X|_Y）。
    float    scaleX;    // ANIM_SCALE_X
    float    scaleY;    // ANIM_SCALE_Y
    float    rotateDeg; // ANIM_ROTATE（度）
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
static_assert(sizeof(BinUIKeyFrame) == 40, "BinUIKeyFrame must be 40 bytes");
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

// ============================================================
// 扩展绘制段（v13，恒定写出，位于组件引用段之后、名称段之前）
// 布局：uint32 paintBytes（0 = 无扩展绘制）+ uint32 paintCount + BinUIPaintNode[paintCount]
//
// 为什么单独开一段，而不往 BinUINode 里加字段：
//   · BinUINode 是 40 字节定长（节点数组按它整体序列化），加字段会改所有节点、
//     破坏"手写 BINU 布局"和已发布二进制；
//   · 这些属性只出现在少数节点上（文字色/边框/渐变/字重/行高/下划线），
//     稀疏存储比给每个节点都留 20+ 字节便宜得多；
//   · 与 spec/state/anim 段同一套路：旧布局没有这段 = 全部取节点自有默认值。
//
// 关键语义 —— **底色与文字色分家**：
//   BinUINode.color 仍然只表示"底色"（LABEL 例外：历史上它表示文字色，
//   为向后兼容，LABEL 且无 TEXT_COLOR 时沿用 color 作文字色）。
//   有了 textColor，容器/按钮/列表的文字色终于可以和底色分别指定。
//
// 节点自身的底色（node.color）与本段的渐变互斥：渐变存在时以渐变填底。
// 边框画在节点**内侧**（不参与布局）：borderW 为像素宽，四边可取不同值。
// ============================================================
enum BinUIPaintFlag : uint16_t {
    PAINT_TEXT_COLOR  = 1 << 0, // textColor 有效（AARRGGBB）
    PAINT_BORDER      = 1 << 1, // borderW / borderColor 有效
    PAINT_BORDER_TOP    = 1 << 2, // 四边各自可关（缺省用 borderW 的四边）
    PAINT_BORDER_RIGHT  = 1 << 3,
    PAINT_BORDER_BOTTOM = 1 << 4,
    PAINT_BORDER_LEFT   = 1 << 5,
    PAINT_GRADIENT    = 1 << 6, // gradFrom / gradTo / gradDir 有效
    PAINT_GRAD_RADIAL = 1 << 7, // 径向渐变（否则线性）
    PAINT_WEIGHT      = 1 << 8, // weight 有效（CSS font-weight）
    PAINT_LINE_HEIGHT = 1 << 9, // lineHeight 有效（百分数，150 = 1.5 倍字号）
    PAINT_UNDERLINE   = 1 << 10, // 文字下划线
    PAINT_STRIKE      = 1 << 11, // 文字删除线
    PAINT_OVERLINE    = 1 << 12, // 文字上划线
    PAINT_CLIP_ONLY   = 1 << 13, // 只裁剪不滚动（CSS overflow:hidden 与 auto/scroll 的区别）
    PAINT_RESIZE      = 1 << 14, // resizeMask 有效（CSS resize：贴边拖动改盒子尺寸）
    // 0.16.8：元素的静态 opacity 存在 `reserve[0]`（0..255，255 = 1.0）。
    // 以前是编译期折进颜色的 alpha —— 那样**子节点完全不受影响**（一个卡片 fade 时背景在淡、
    // 文字和子块纹丝不动），和 CSS 的"组透明度"语义相反。现在留给运行期，
    // 由渲染器按祖先链乘下去（`drawOpacity` / `drawState().opacity` 都返回**级联后**的值）。
    // 旧二进制这一位恒为 0（那时已经折进颜色了），行为不变，所以不升 BINUI_VERSION。
    PAINT_OPACITY     = 1 << 15, // reserve[0] = 静态 opacity（0..255）
};

// 渐变方向（PAINT_GRADIENT 且非 RADIAL 时有效）
enum BinUIGradDir : uint8_t {
    BINUI_GRAD_V = 0, // to bottom（默认，CSS 180deg）
    BINUI_GRAD_H = 1, // to right（CSS 90deg）
    BINUI_GRAD_D = 2, // to bottom right（CSS 135deg）
};

// 元素缩放热区（v21，BinUIPaintNode::resizeMask）。CSS `resize` 的取值：
//   none / both|all / horizontal / vertical / left|right|top|bottom /
//   top-left|top-right|bottom-left|bottom-right（也接受 `<名字>-resize` 拼法）
// 边框和四角分开记：只写两个角（`resize: top-left bottom-right`）时两条边中间
// 那段不该能拖 —— 位图比"有左边就自动有角"更贴近作者的意图。
enum BinUIResizeFlag : uint8_t {
    RESIZE_LEFT         = 1 << 0,
    RESIZE_RIGHT        = 1 << 1,
    RESIZE_TOP          = 1 << 2,
    RESIZE_BOTTOM       = 1 << 3,
    RESIZE_TOP_LEFT     = 1 << 4,
    RESIZE_TOP_RIGHT    = 1 << 5,
    RESIZE_BOTTOM_LEFT  = 1 << 6,
    RESIZE_BOTTOM_RIGHT = 1 << 7,
    // 简写组合（编译期用，运行期只认位）
    RESIZE_SIDES   = RESIZE_LEFT | RESIZE_RIGHT | RESIZE_TOP | RESIZE_BOTTOM,
    RESIZE_CORNERS = RESIZE_TOP_LEFT | RESIZE_TOP_RIGHT | RESIZE_BOTTOM_LEFT | RESIZE_BOTTOM_RIGHT,
    RESIZE_ALL     = RESIZE_SIDES | RESIZE_CORNERS,
};

// 文字/表单附加标记（v14，BinUIPaintNode::textFlags）。
// 为什么单独一个字段：PAINT_* 的 16 位已经用满，而这些标记本身也是"少数节点才需要"，
// 跟扩展绘制段一起做稀疏存储最省。textFlags 为 0 时与 v13 的行为完全一致。
enum BinUITextFlag : uint32_t {
    TEXT_ITALIC      = 1u << 0, // 斜体（FreeType 合成倾斜 / GDI CreateFontA italic）
    // node.text 是**占位符**而不是真实值：输入框为空时显示（按 60% 文字色画），
    // 首次输入即被清空，失去焦点且内容为空时恢复。getText() 返回真实值（此时为空串）。
    TEXT_PLACEHOLDER = 1u << 1,
    TEXT_READONLY    = 1u << 2, // 只读输入框：可聚焦/可选中，但拒绝编辑
    TEXT_CHECKED     = 1u << 3, // 初始勾选（<input type=checkbox checked>），加载时播种进渲染器
};

#pragma pack(push, 1)
struct BinUIPaintNode {
    uint16_t nodeId;
    uint16_t flags;        // BinUIPaintFlag
    uint8_t  borderW[4];   // 上右下左，像素（0 = 该边不画）
    uint8_t  gradDir;      // BinUIGradDir
    uint8_t  weight;       // CSS font-weight / 100（4 = 常规，7 = 粗体；0 = 未声明）
    uint8_t  lineHeight;   // 行高百分数：0 = 默认（135），150 = 1.5 倍字号
    uint8_t  borderStyle;  // 0 = solid（预留 dashed/dotted）
    uint8_t  resizeMask;   // v21：BinUIResizeFlag（PAINT_RESIZE 置位时有效，0 = 不可缩放）
    uint8_t  reserve[3];   // 对齐填充（留给后续绘制标记，旧布局这三位恒为 0）
    uint32_t textColor;    // AARRGGBB
    uint32_t borderColor[4]; // 上右下左各自的颜色（AARRGGBB）
    uint32_t gradFrom;     // AARRGGBB（渐变起点色）
    uint32_t gradTo;       // AARRGGBB（渐变终点色）
    uint32_t textFlags;    // v14：BinUITextFlag（斜体/占位符/只读/初始勾选）
};
#pragma pack(pop)
static_assert(sizeof(BinUIPaintNode) == 48, "BinUIPaintNode must be 48 bytes");

// ============================================================
// 四角带色四边形（v30）：任意角度线性渐变的几何单元
// ------------------------------------------------------------
// 线性渐变在屏幕空间是**仿射函数**：颜色 = lerp(from, to, ((p-s)·a)/len)。
// 仿射函数被三角形上的线性插值**精确复现**（三个顶点值定死一个仿射函数），所以
// "一个四边形 + 四角各取一次色"是精确解，不是近似 —— 于是 320×180 的 135° 卡片
// 从 3,694 条 1px 条带（12 张 = 43,409 条/帧、≈5 MB 顶点）变成 1 个四边形。
//
// 圆角走九宫格：角片仍借用后端那张圆角模板位图（模板 alpha × 顶点色），每块四角
// 各自取渐变在**屏幕坐标**上的颜色。与 fillRounded 同一套角片，所以弧线 AA 也一致。
//
// c[4] 是**已预乘**的颜色（三端的 Premult 公式相同，这里只算一次，保证三端逐字节
// 一致）。预乘顺序 = "先按直线插值取色、再预乘"，与老条带路径同口径；差别只在
// "两端 alpha 不同"的渐变上（GPU 逐顶点插值预乘色 = CSS Color 4 的规范做法）。
// ============================================================
struct BinUIGradQuad {
    float    x = 0, y = 0, w = 0, h = 0;
    float    u0 = 0, v0 = 0, u1 = 1, v1 = 1;    // 圆角模板 UV（直角时 0..1 = 白纹理）
    uint32_t c[4] = {0, 0, 0, 0};               // 左上 / 右上 / 左下 / 右下
};

// ============================================================
// 阴影段（v23）：CSS 的 box-shadow
//
// 布局：uint32 shadowBytes（0 = 无阴影）+ uint32 shadowCount + BinUIShadow[count]
// 恒定写出，位于路径段之后、名称段之前（与其余可选段同一套路）。
//
// 为什么单独开一段：一个阴影要 5 个数（两个偏移 + 模糊 + 展开 + 颜色），48 字节的
// BinUIPaintNode 装不下（PAINT_* 的 16 位也已用满），而阴影只出现在少数卡片上，
// 稀疏存储最省 —— 与 v13 开扩展绘制段、v15 开路径段是同一条取舍。
//
// **数组顺序 = 绘制顺序**（下标小的先画，压在最下面）。CSS 的 box-shadow 列表是
// "先写的盖在上面"，所以编译器按逆序写出；渲染器从头到尾画一遍即可。
// 同一个节点出现多条时不合并：多层阴影（贴边硬影 + 大范围柔影）是常见手法。
// ============================================================
enum BinUIShadowFlag : uint16_t {
    // inset：编译期只告警、不写出该条（后端只有 fillRect/fillRounded，画不出"挖内孔"
    // 的几何）。这一位留在这里是为了将来翻实现时二进制不用再改一轮，目前恒为 0。
    SHADOW_INSET = 1 << 0,
};

#pragma pack(push, 1)
struct BinUIShadow {
    uint16_t nodeId;
    uint16_t flags;      // BinUIShadowFlag
    float    offsetX;    // 水平偏移（像素；CSS offset-x，正值向右）
    float    offsetY;    // 垂直偏移（像素；CSS offset-y，正值向下）
    float    blur;       // 模糊半径（像素；CSS blur-radius，标准差 σ = blur/2）
    float    spread;     // 展开半径（像素；CSS spread-radius，负值收缩）
    uint32_t color;      // AARRGGBB（已折入 opacity，画的时候直接用）
};
#pragma pack(pop)
static_assert(sizeof(BinUIShadow) == 24, "BinUIShadow must be 24 bytes");

// ============================================================
// 文字阴影段（v24）：CSS 的 text-shadow
//
// 布局：uint32 tsBytes（0 = 无）+ uint32 tsCount + BinUITextShadow[count]
// 恒定写出，位于阴影段之后、名称段之前（段序见 BinUIGlyphAtlas 的注释）。
//
// 为什么再开一段而不是塞进 box-shadow 段：一条 box-shadow 有 5 个几何量（多了
// spread），一条 text-shadow 只有 3 个，而且两者的**绘制位置**完全不同（一个是
// 盒子轮廓的环，一个是字形位图的模糊）。混在一条里得靠 flags 分支区分，读段的人
// 反而要多想一层；分开是每种 20 字节的定长数组，两边都最省。
//
// **数组顺序同样是绘制顺序**（下标小的先画）。text-shadow 是**可继承**属性，
// 所以编译器会把祖先的阴影折进每个真正画字的节点（见 binui_compiler.cpp 的
// Ctx::textShadow）—— 换句话说，一个卡片上写一次，卡片里所有文字都带阴影。
// ============================================================
#pragma pack(push, 1)
struct BinUITextShadow {
    uint16_t nodeId;
    uint16_t flags;      // 保留（目前恒 0）
    float    offsetX;    // 水平偏移（像素）
    float    offsetY;    // 垂直偏移（像素）
    float    blur;       // 模糊半径（像素；标准差 σ = blur/2，与 box-shadow 同一口径）
    uint32_t color;      // AARRGGBB（已折入 opacity）
};
#pragma pack(pop)
static_assert(sizeof(BinUITextShadow) == 20, "BinUITextShadow must be 20 bytes");

// ============================================================
// 绘制附加段（v25）：九宫格图片 等"每个节点一两个小整数"的绘制参数
//
// 布局：uint32 peBytes（0 = 无）+ uint32 peCount + BinUIPaintEx[count]
// 恒定写出，位于文字阴影段之后、名称段之前。
//
// 为什么又开一段：九宫格只需要一个内缩像素（1 字节），可 `BinUIPaintNode` 的
// `reserve[3]` 三个字节已经全部用完（opacity / 渐变角度 / 图片亮度），而 48 字节
// 的结构一个字节都加不了（换布局就得动所有后端与旧二进制）。稀疏段是这里唯一
// 不破坏兼容的落点 —— 与 v23 阴影段、v24 文字阴影段同一条取舍。
//
// 语义：`imageSlice > 0` 表示这个 `<img>` 按九宫格画（四边内缩 = 源图像素）。
// 详见 src/backends/binui_draw_nodes.h 的 BINUI_IMAGE 分支与
// IBinUIGfx::drawImageNine。
// ============================================================
#pragma pack(push, 1)
struct BinUIPaintEx {
    uint16_t nodeId;
    uint8_t  imageSlice;  // 九宫格内缩（源图像素，四边同宽；0 = 普通拉伸）
    uint8_t  flags;       // 保留（目前恒 0）
};
#pragma pack(pop)
static_assert(sizeof(BinUIPaintEx) == 4, "BinUIPaintEx must be 4 bytes");

// ============================================================
// 图片滤镜段（v26）：CSS filter 合成后的颜色矩阵
//
// 布局：uint32 ffBytes（0 = 无）+ uint32 ffCount + BinUIImageFilter[count]
// 恒定写出，位于绘制附加段之后、名称段之前。
//
// 为什么单独开一段：颜色矩阵是 12 个 float（48 字节），`BinUIPaintNode` 一个字节都
// 加不了（reserve[3] 已满），也不该为极少数带滤镜的图片给所有节点加 48 字节。
//
// 语义：只对 `<img>` / `<video>` 的**像素**生效（后端解码后跑一遍矩阵再上传纹理），
// 每节点一条，按 (imageIndex, 矩阵) 缓存纹理变体。brightness 不走这里 —— 它等价于
// "乘一个顶点色"，0.16.15 起就走 paint.reserve[2]，GDI 也能吃。
// 矩阵的合成（多个函数按书写顺序相乘）在编译期完成，运行期只做逐像素乘法。
// 详见 include/binui_filter.h。
// ============================================================
#pragma pack(push, 1)
struct BinUIImageFilter {
    uint16_t nodeId;
    uint8_t  flags;       // bit0 = 有效（恒 1；留给将来的 opacity 并入等）
    uint8_t  reserved;
    float    m[9];        // 3×3 行主序，作用在 0..1 的 RGB（sRGB）
    float    offset[3];   // 偏移（invert / contrast 需要）
};
#pragma pack(pop)
static_assert(sizeof(BinUIImageFilter) == 52, "BinUIImageFilter must be 52 bytes");

// ============================================================
// 裁剪路径段（v27）：SVG <clipPath> / CSS clip-path: url(#id)
//
// 布局：uint32 cBytes（0 = 无）+ uint32 cCount + 每条：
//     BinUIClipEntry                    定长头（24 字节）
//     uint32 contourFirst[contourCount] 每条子路径的起始点下标
//     uint8  contourClosed[contourCount]
//     uint16 points[2*pointCount]       归一化点（0..65535 映射包围盒）
// 恒定写出，位于图片滤镜段之后、名称段之前。
//
// 语义：带裁剪的节点先画进**离屏图层**，再用"图层矩形 + 这条路径"的 even-odd 填充
//   把路径外的一切**擦除**（混合 dst *= 1-srcA）—— 任意形状的裁剪都能量表达，而且
//   三个后端只需要一个额外的混合状态（VK 是管线变体，不动 SPIR-V）。
// 坐标：x/y/w/h 是**设备空间**的包围盒（编译期已过 svgX 与页面偏移），点按同一个
//   盒子归一化 —— 运行期不需要再知道 SVG 变换链。
// ============================================================
#pragma pack(push, 1)
struct BinUIClipEntry {
    uint16_t nodeId;
    uint16_t flags;        // bit0 = even-odd（clip-rule="evenodd"）
    float    x, y, w, h;   // 设备空间包围盒
    uint16_t pointCount;
    uint16_t contourCount;
};
#pragma pack(pop)
static_assert(sizeof(BinUIClipEntry) == 24, "BinUIClipEntry must be 24 bytes");

// ============================================================
// 遮罩段（v28）：SVG <mask> / CSS mask: url(#id)
//
// 布局：uint32 mBytes（0 = 无）+ uint32 mCount + 每条：
//     BinUIMaskEntry                    定长头（44 字节）
//     uint32 contourFirst[contourCount] 每条子路径的起始点下标
//     uint8  contourClosed[contourCount]
//     uint16 points[2*pointCount]       归一化点（0..65535 映射**本组几何**包围盒 x/y/w/h）
// 恒定写出，位于裁剪路径段之后、名称段之前。
//
// 语义：带 mask 的节点先画进**离屏图层**（图层框 = mask 区域 rx/ry/rw/rh），然后两趟：
//   ① 保留趟：把掩码几何按 color 的 alpha 用"dst *= srcA"画一遍（白 = 保留、灰 = 压暗）；
//   ② 补集趟：把"图层矩形 ∪ 所有掩码几何（even-odd）"用"dst *= 1-srcA"画一遍 ——
//      掩码没盖到的像素被清零（SVG 语义：mask 内容之外 = 黑 = 透明）。
// 两趟都只用到混合状态，三端零着色器改动（VK 是管线变体，SPIR-V 不动）。
//
// 与裁剪段的区别：裁剪段是"一条几何 + 一个填充规则"，遮罩段是"多组几何 + 每组一个
//   alpha"（同一组 = 同样的亮度，编译期已把 RGB 亮度折进 alpha）。
// 亮度口径：sRGB 系数 0.2126R + 0.7152G + 0.0722B，再乘 fill-opacity / opacity。
// ============================================================
#pragma pack(push, 1)
struct BinUIMaskEntry {
    uint16_t nodeId;       // 被遮罩的元素
    uint16_t flags;        // 保留（目前恒 0）
    uint32_t color;        // AARRGGBB：alpha = 掩码亮度，RGB 恒白
    float    rx, ry, rw, rh;   // mask 区域（图层框，设备空间）
    float    x, y, w, h;       // 本组几何的包围盒（归一化基准，设备空间）
    uint16_t pointCount;
    uint16_t contourCount;
};
#pragma pack(pop)
static_assert(sizeof(BinUIMaskEntry) == 44, "BinUIMaskEntry must be 44 bytes");

// ============================================================
// 毛玻璃段（v29）：CSS backdrop-filter: blur(Npx)
//
// 布局：uint32 bBytes（0 = 无）+ uint32 bCount + 每条 BinUIBackdropEntry（12 字节）。
// 恒定写出，位于遮罩段之后、名称段之前。
//
// 语义：画这个节点**自身内容之前**，把当前目标上 (x,y,w,h) 处的背景抓下来做多级
//   2× 降采样模糊（每级就是一个 2×2 盒式模糊，链起来逼近高斯），再按 radius 圆角、
//   以 **replace** 混合画回同一位置 —— 于是"背景被糊掉"而不是"再叠一层模糊"。
//   调用方随后照常画元素自己的半透明底色/边框，两者合起来就是毛玻璃。
//
// 为什么不是着色器：着色器只做"纹理 × 顶点色"，模糊链靠**多级降采样**在固定管线里
//   完成，VK 的预编译 SPIR-V 一行不用动；三端算法完全一致（同一套 UV 公式）。
// ============================================================
#pragma pack(push, 1)
struct BinUIBackdropEntry {
    uint16_t nodeId;
    uint16_t flags;      // 保留（目前恒 0）
    float    blur;       // 模糊半径（px，已乘 ui_scale）
    float    radius;     // 合成时的圆角（= 节点圆角）
};
#pragma pack(pop)
static_assert(sizeof(BinUIBackdropEntry) == 12, "BinUIBackdropEntry must be 12 bytes");

// ============================================================
// 路径段（v15，恒定写出，位于扩展绘制段之后、名称段之前）
//
// 为什么单独一段而不是复用扩展绘制段：路径数据是变长的（点数组），
// 而 paint 段是定长条目数组，混在一起会让"按 nodeId 线性查找"变成跳读。
//
// 布局：uint32 pathBytes（0 = 无路径）+ uint32 pathCount
//       + pathCount 条子记录，顺序拼接：
//           BinUIPathEntry              定长头
//           BinUIPathGradient           填充渐变，仅 PATH_FILL_GRAD 置位时才写（v16）
//           BinUIPathGradient           描边渐变，仅 PATH_STROKE_GRAD 置位时才写（v17）
//           uint32 contourFirst[n]      每条子路径的起始**点下标**（升序）
//           uint8  contourClosed[n]     1 = 原始 Z（描边时连接首尾）
//           uint16 points[m*2]          归一化坐标（x,y 交替，0..65535 映射包围盒）
//
// 为什么存折线、不存 `d` 命令：运行期只做"归一化点 × 节点盒"的两次乘加，
// 不用解析、不用缓存；曲线细分在编译期一次做完（容差按设备像素取）。
// 代价是极限放大能看到折线 —— 这里留了 flags 位给将来的"命令 + 运行时再细分"。
// ============================================================
enum BinUIPathFlag : uint16_t {
    PATH_FILL      = 1 << 0, // 填充（fill 有效且不透明）
    PATH_STROKE    = 1 << 1, // 描边（stroke 有效且线宽 > 0）
    PATH_EVENODD   = 1 << 2, // fill-rule="evenodd"；否则非零环绕
    PATH_FILL_GRAD   = 1 << 3, // v16：填充用渐变（BinUIPathGradient 紧随定长头）
    PATH_STROKE_GRAD = 1 << 4, // v17：描边用渐变（第二条 BinUIPathGradient，在填充渐变之后）
    PATH_AA          = 1 << 5, // v19：填充边缘抗锯齿（shape-rendering=auto/optimizeQuality）
};


enum BinUIPathJoin : uint8_t {
    BINUI_JOIN_ROUND = 0, // 转角补圆点（binUI 的历史默认，也是 v15 的唯一实现）
    BINUI_JOIN_MITER = 1, // 斜接：补三角形 + 尖角四边形；超过 miter-limit 按 bevel 画
    BINUI_JOIN_BEVEL = 2, // 斜面：只补三角形
};

enum BinUIPathCap : uint8_t {
    BINUI_CAP_BUTT   = 0, // 默认：端点就是路径端点
    BINUI_CAP_ROUND  = 1, // 两端补半圆
    BINUI_CAP_SQUARE = 2, // v17：两端各外延半个线宽（方头）
};

#pragma pack(push, 1)
struct BinUIPathEntry {
    uint16_t nodeId;
    uint16_t flags;         // BinUIPathFlag
    uint32_t fillColor;     // AARRGGBB（已折入 fill-opacity / opacity）
    uint32_t strokeColor;   // AARRGGBB（已折入 stroke-opacity / opacity）
    // 编译期包围盒尺寸：运行期按 node.height / designH 求描边缩放系数。
    // 归一化点已经自带缩放，只有线宽需要这个（与边框/字号同一条取舍：只跟纵向比例）。
    float    designW;
    float    designH;
    uint16_t pointCount;    // 点总数（points 数组里是 2×pointCount 个 uint16）
    uint16_t contourCount;  // 子路径数
    uint8_t  strokeW;       // 设计线宽（像素，0 = 不描边）
    uint8_t  join;          // BinUIPathJoin
    uint8_t  cap;           // BinUIPathCap
    uint8_t  reserve;       // stroke-miterlimit（0 = 运行期按 SVG 默认 4）
};
#pragma pack(pop)
static_assert(sizeof(BinUIPathEntry) == 28, "BinUIPathEntry must be 28 bytes");

// ============================================================
// 路径渐变（v16）
//
// 为什么不复用扩展绘制段的 gradFrom/gradTo：那两个只描述"矩形底色、两色、上下/左右/
// 对角"，而 SVG 渐变是任意角度的线性渐变 + 椭圆径向渐变 + 多色标 + 三种 spread。
// 也不做"逐点取色的位图"：路径填充本身是扫描线条带（各端只画 fillRect），
// 渐变只要给每条带算出颜色就能三端一致，不需要额外纹理与采样器。
//
// 坐标是**包围盒归一化坐标**：设备坐标 = 节点盒左上 + 归一化值 × 节点盒尺寸，
// 与折线点同一套映射。所以缩放节点盒时渐变自然跟着缩放 —— 这正是 SVG 的
// objectBoundingBox 语义（默认值）。userSpaceOnUse 在编译期就被折算进这套坐标。
// ============================================================
enum BinUIPathGradType : uint8_t {
    PATH_GRAD_LINEAR = 1,
    PATH_GRAD_RADIAL = 2,
};

enum BinUIPathGradSpread : uint8_t {
    PATH_GRAD_PAD     = 0, // 默认：端点色延伸到无穷（SVG 默认）
    PATH_GRAD_REPEAT  = 1,
    PATH_GRAD_REFLECT = 2,
};

#pragma pack(push, 1)
struct BinUIPathGradStop {
    float    offset;   // 0..1（已排序、已夹紧）
    uint32_t color;    // AARRGGBB（已折入 stop-opacity / fill-opacity / opacity）
};

constexpr int kPathGradMaxStops = 8;

struct BinUIPathGradient {
    uint8_t  type;         // BinUIPathGradType
    uint8_t  spread;       // BinUIPathGradSpread
    uint8_t  stopCount;    // 2..kPathGradMaxStops
    uint8_t  reserve;
    // 线性：(x1,y1) → (x2,y2)；径向：圆心 (x1,y1)，椭圆半径 rx/ry。
    // 径向用两个半径而不是一个：包围盒非正方时 SVG 的径向渐变本来就是椭圆，
    // 一个半径表达不了（userSpaceOnUse 下两轴缩放不同也会走到这条路）。
    float    x1, y1, x2, y2;
    float    rx, ry;
    // v18：径向渐变的焦点。等于圆心时就是普通同心圆（绝大多数素材如此）；
    // 不等于圆心时按 SVG 的锥形语义解（从焦点出发的射线打到椭圆的位置定义 t）。
    // 线性渐变忽略这两个字段。
    float    fx, fy;
    BinUIPathGradStop stops[kPathGradMaxStops];   // 前 stopCount 个有效
};
#pragma pack(pop)
static_assert(sizeof(BinUIPathGradient) == 100, "BinUIPathGradient must be 100 bytes");



// 文字装饰线（下划线/删除线/上划线）在节点文字盒中的位置：
// 后端只需按 (x, y, w, h) 画一条 height 高的实心条；返回值 0 = 该装饰线不需要画。
//   fontSize  节点字号；textTop 文字块顶部 y；textH 文字块高度
//   which     0=下划线 1=删除线 2=上划线
struct BinUITextRule { float off; float thickness; };
inline BinUITextRule BinUITextRuleOf(int which, float fontSize) {
    const float th = fontSize >= 20.0f ? 2.0f : 1.0f;
    switch (which) {
        case 1: return { fontSize * 0.55f, th };  // 删除线：大致穿过字符中腰
        case 2: return { fontSize * 0.12f, th };  // 上划线：贴近字形顶部
        default: return { fontSize * 0.98f, th }; // 下划线：基线下方
    }
}

} // namespace binui
