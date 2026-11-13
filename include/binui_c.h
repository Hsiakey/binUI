/* ============================================================================
 * binui_c.h - binUI 的 C ABI（给 Python / Go / Rust 等宿主语言的唯一入口）
 *
 * 为什么单独一层 C ABI：引擎本身是 C++ 库（std::string / std::function / 异常
 * 语义），直接跨 FFI 会撞上半打语言各自的雷（名称修饰、ABI 版本、回调线程、
 * 异常穿过 C 边界）。这里把对外曲面压成"定长 POD + 不透明句柄 + 轮询"三件套，
 * 任何能调 C 函数的语言都能绑，且绑定写法完全一样。
 *
 * ---------------------------------------------------------------------------
 * 三条设计约束（改 ABI 前先读）
 * ---------------------------------------------------------------------------
 * 1. 结构体只用定长类型：int32_t / uint32_t / float。没有 bool（C 是 1 字节、
 *    C# 默认 4 字节）、没有 enum（宽度随编译器）、没有 size_t / long（32/64 位
 *    会变）。所有结构体天然 4 字节对齐，不需要任何 #pragma pack。
 * 2. 没有跨语言回调。所有事件走 binui_poll_event() 轮询 —— 宿主不进引擎的
 *    调用栈，就没有线程亲和 / GC 保活 / 重入这些坑，PHP/Ruby 这类没有稳定回调
 *    机制的语言也能用。
 * 3. 字符串一律 UTF-8。取文本用 (buf, cap) 两段式（返回所需长度，和 snprintf
 *    一个习惯），不返回需要释放的指针。
 *
 * ---------------------------------------------------------------------------
 * 最小可用流程（三个语言写法几乎一样）
 * ---------------------------------------------------------------------------
 *   app = binui_app_create(layout_bytes, len, ui_dir);  // 加载 .bui（ui_dir 可为 NULL）
 *   binui_app_open(app, 0, 0, BINUI_OPEN_RESIZABLE, "窗口标题");
 *   while (binui_pump(app, 8) == 0) {                  // 8ms 超时，空闲不烧 CPU
 *       while (binui_poll_event(app, &ev))
 *           if (ev.type == BINUI_EV_CLICK) { ... }
 *   }
 *   binui_app_destroy(app);
 *
 * 布局从哪来：构建期用 binui_compile（命令行）或 binui_compile_file() 把
 * .html/.xml 编成 .bui，再读成字节喂给 binui_app_create()。
 *
 * ---------------------------------------------------------------------------
 * 当前未覆盖（明确不支持，不是 bug）
 * ---------------------------------------------------------------------------
 * - <canvas> 的绘制回调（需要跨语言回调，与约束 2 冲突；需要就在宿主侧自己画）
 * - <video> 的播放控制（解码模块 BINUI_ENABLE_VIDEO，另行挂载）
 * - 非 Windows 平台（GL-GDI 外壳是 Win32 实现；核心库跨平台，外壳不是）
 *
 * 窗口嵌入已经支持：走 binui_app_open_child()（WS_CHILD 子窗口模式），
 * 宿主自己收消息、自己 DispatchMessage，每帧调 binui_render()。
 * ==========================================================================*/
#ifndef BINUI_C_H
#define BINUI_C_H

#include <stddef.h>
#include <stdint.h>

#ifdef _WIN32
#  if defined(BINUI_C_BUILD_DLL)
#    define BINUI_C_API __declspec(dllexport)
#  elif defined(BINUI_C_USE_DLL)
#    define BINUI_C_API __declspec(dllimport)
#  else
#    define BINUI_C_API
#  endif
#  define BINUI_C_CALL __cdecl
#else
#  define BINUI_C_API
#  define BINUI_C_CALL
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ============================ 版本 ============================ */

/* ABI 版本：结构体布局/函数签名有任何不兼容变化就 +1。
 * 宿主应在启动时校验，避免拿到旧 DLL 后安静地读错内存。
 *
 * 本次（运行期能力：scroll_to / set_node_bounds / add_node 等）为**增量新增**：
 * 只加函数、不动任何既有结构体与函数签名，所以 BINUI_C_ABI_VERSION 保持 1。
 * 旧宿主链新 DLL 照常工作；新宿主连旧 DLL 时找不到符号（链接/加载期报错），
 * 不会读错内存。
 *
 * v3（0.14.0）：`BinUIEvent` 再追加 `dx` / `button` / `focused` / `editing` / `prev_id`，
 * 新增指针（PRESS/RELEASE/MOVE/DBL_CLICK/CONTEXT_MENU/HOVER_LEAVE）、焦点与编辑
 * （FOCUS/EDIT/CHANGE）、SCROLL、元素拖拽（RESIZE_START/END）、LOAD 事件。
 * v2（0.13.0）：`BinUIEvent` 尾部追加 `prop` / `iteration`（动画事件用），并新增
 * BINUI_EV_ANIM_END / ANIM_ITER / TRANS_END 三个事件类型 —— 结构体变大，所以
 * ABI 版本升到 2；宿主必须按新头文件重编（旧头文件 + 新 DLL 会被写出界）。 */
#define BINUI_C_ABI_VERSION 3

BINUI_C_API const char* BINUI_C_CALL binui_version(void);      /* 库版本串（静态，勿释放） */
BINUI_C_API int32_t     BINUI_C_CALL binui_abi_version(void);  /* = BINUI_C_ABI_VERSION */

/* 最近一次失败的中文说明（线程局部缓冲，下一次失败覆盖）。
 * 成功不保证清空 —— 只在返回负值/失败时读它。 */
BINUI_C_API const char* BINUI_C_CALL binui_last_error(void);

/* 释放本库分配的内存（binui_compile_memory 的输出）。
 * 必须用这个，不要用宿主的 free：Windows 上跨 CRT 释放堆内存会崩。 */
BINUI_C_API void BINUI_C_CALL binui_free(void* p);

/* ============================ 编译 ============================ */

/* UI 源码 -> 二进制布局。
 * 返回：1 = 成功；0 = 失败（看 binui_last_error）。
 * 源码已是二进制布局时原样写出（便于"源码/二进制"两条流水线共用一条命令）。 */
BINUI_C_API int32_t BINUI_C_CALL binui_compile_file(const char* src_path_utf8,
                                                   const char* out_path_utf8);

/* 同上，直接传源码字符串，成功时 *out_data / *out_len 返回 malloc 的布局字节
 * （用 binui_free 释放）。适合脚本里写死 UI 或从网络/数据库取 UI。 */
BINUI_C_API int32_t BINUI_C_CALL binui_compile_memory(const char* src_utf8, int32_t src_len,
                                                     void** out_data, int32_t* out_len);

/* ============================ 事件 ============================ */

typedef enum BinUIEventType {
    BINUI_EV_NONE   = 0,
    BINUI_EV_CLICK  = 1,  /* 节点被点击/键盘激活：node_id */
    BINUI_EV_INPUT  = 2,  /* 文本框值变化：node_id + text（失焦提交，或编辑中逐字） */
    BINUI_EV_HOVER  = 3,  /* 悬停节点变化：node_id（离开为 -1） */
    BINUI_EV_RESIZE = 4,  /* 窗口尺寸变化：width/height */
    BINUI_EV_KEY    = 5,  /* 按键：key = 虚拟键码（见 binui_c.h 底部 BINUI_KEY_*） */
    BINUI_EV_CHAR   = 6,  /* 输入字符：text（UTF-8，可能是多个字节） */
    BINUI_EV_WHEEL  = 7,  /* 滚轮：x/y 光标位置，dy 增量（正 = 向上） */
    BINUI_EV_CLOSE  = 8,  /* 窗口关闭（用户按 Alt+F4 / 宿主调用 request_exit） */
    /* v2 追加：动画事件。引擎侧是"取"的（takeAnimEvents），C 侧统一排进同一个队列，
     * 用法不变 —— 每帧 pump 之后照常 poll，不需要另外的接口。 */
    BINUI_EV_ANIM_END  = 9,   /* 关键帧动画整体结束：node_id + iteration（infinite 不报）*/
    BINUI_EV_ANIM_ITER = 10,  /* 关键帧动画迭代边界：node_id + iteration（1 起）*/
    BINUI_EV_TRANS_END = 11,  /* 属性过渡结束：node_id + prop（= BinUIAnimProp 数值）*/
    /* v3 追加：指针/焦点/编辑/滚动/拖拽/加载 —— 与引擎 EventType 一一对应 */
    BINUI_EV_PRESS       = 12, /* 按下：node_id + x/y + button */
    BINUI_EV_RELEASE     = 13, /* 松开：node_id（按下时命中的节点）+ x/y + button */
    BINUI_EV_MOVE        = 14, /* 鼠标移动：node_id（-1 = 空白）+ x/y */
    BINUI_EV_DBL_CLICK   = 15, /* 双击：node_id + x/y */
    BINUI_EV_CONTEXT_MENU= 16, /* 右键按下：node_id + x/y */
    BINUI_EV_HOVER_LEAVE = 17, /* 离开节点：node_id */
    BINUI_EV_FOCUS       = 18, /* 键盘焦点变化：node_id + focused（0/1）+ prev_id */
    BINUI_EV_EDIT        = 19, /* 进入/退出文本编辑：node_id + editing（0/1）*/
    BINUI_EV_CHANGE      = 20, /* 文本框提交：node_id + text */
    BINUI_EV_SCROLL      = 21, /* 滚动位置变化：node_id + x/y（新位置）+ dx/dy（本次增量）*/
    BINUI_EV_RESIZE_START= 22, /* 开始拖元素尺寸：node_id */
    BINUI_EV_RESIZE_END  = 23, /* 拖完：node_id + x/y/width/height（最终矩形）*/
    BINUI_EV_LOAD        = 24  /* 布局加载完成：node_id = 0 */
} BinUIEventType;

typedef struct BinUIEvent {
    int32_t     type;      /* BinUIEventType */
    int32_t     node_id;   /* 相关节点；无则 -1 */
    float       x, y;      /* 鼠标坐标（CLICK / HOVER / WHEEL 有效） */
    float       dy;        /* 滚轮增量 */
    int32_t     key;       /* KEY：虚拟键码 */
    const char* text;      /* INPUT / CHAR：UTF-8 文本；可能为 NULL。
                            * 指向库内缓冲，**下一次 binui_poll_event / binui_pump
                            * 之前有效**（要留就当场拷走） */
    int32_t     text_len;  /* text 的字节数（不含结尾 NUL） */
    int32_t     width, height; /* RESIZE */
    int32_t     prop;          /* TRANS_END：过渡的属性号（binui_renderer.h 的 ANIM_*）*/
    int32_t     iteration;     /* ANIM_END / ANIM_ITER：已完成的迭代次数 */
    float       dx;            /* SCROLL：本次水平增量；WHEEL 时为 0 */
    int32_t     button;        /* PRESS / RELEASE：0=左 1=右 2=中 */
    int32_t     focused;       /* FOCUS：1=获得 0=失去 */
    int32_t     editing;       /* EDIT：1=进入编辑 0=退出 */
    int32_t     prev_id;       /* FOCUS：上一个焦点节点（无则 -1）*/
} BinUIEvent;

/* ============================ 节点 ============================ */

/* 节点类型（与二进制布局里的 BinUINodeType 一一对应） */
#define BINUI_NODE_ROOT   0
#define BINUI_NODE_LABEL  1
#define BINUI_NODE_IMAGE  2
#define BINUI_NODE_BUTTON 3
#define BINUI_NODE_INPUT  4
#define BINUI_NODE_LIST   5
#define BINUI_NODE_LINE   6   /* x,y = 起点；width,height = 终点；radius = 线宽 */
#define BINUI_NODE_SVG    7
#define BINUI_NODE_CANVAS 8
#define BINUI_NODE_VIDEO  9
#define BINUI_NODE_PATH   10  /* SVG 路径：x,y,w,h = 几何包围盒；折线在 v15 路径段里 */

/* 节点标志位（BinUINodeFlag） */
#define BINUI_NF_VISIBLE      (1u << 0)
#define BINUI_NF_DISABLED     (1u << 1)
#define BINUI_NF_INTERACTIVE  (1u << 2)
#define BINUI_NF_SCROLLABLE   (1u << 3)
#define BINUI_NF_ABSOLUTE     (1u << 4)
#define BINUI_NF_TITLEBAR     (1u << 5)
#define BINUI_NF_TEXT_CENTER  (1u << 6)
#define BINUI_NF_TEXT_RIGHT   (1u << 7)

/* 定长节点快照。想拿文本用 binui_node_text()（节点文本不进这个结构，
 * 免得把指针生命周期问题丢给宿主）。 */
typedef struct BinUINode {
    int32_t  id;
    int32_t  parent_id;
    int32_t  type;         /* BINUI_NODE_* */
    int32_t  flags;        /* BINUI_NF_* 位掩码 */
    float    x, y, width, height;
    uint32_t color;        /* 0xAARRGGBB（节点底色） */
    int32_t  image_index;  /* IMAGE / VIDEO 的图片槽 */
    int32_t  font_size;
    int32_t  padding;
    int32_t  radius;
    int32_t  z_index;
} BinUINode;

/* ============================ 应用 ============================ */

/* 应用句柄是 void*（不透明）。内部 = 渲染器 + 窗口外壳 + 事件队列。
 * 用 void* 而不是具名不透明结构体：Python ctypes / Go / Rust / C# 全都直接
 * 当裸指针处理，少一层 typedef 就少一处绑定差异。 */

/* binui_app_open 的 flags */
#define BINUI_OPEN_RESIZABLE   (1u << 0)  /* 可拖边框改变大小 */
#define BINUI_OPEN_TRANSPARENT (1u << 1)  /* 分层窗口：画布 alpha 透出桌面 */
#define BINUI_OPEN_NO_VSYNC    (1u << 2)  /* 关垂直同步（默认开；仅压测时关） */

/* 创建应用并加载布局。layout 是 .bui 的字节内容，调用返回后宿主即可释放。
 * asset_dir_utf8：图片/外挂组件的基准目录（相对路径从这里解析），可为 NULL。
 * 失败返回 NULL（看 binui_last_error）。 */
BINUI_C_API void* BINUI_C_CALL binui_app_create(const void* layout, int32_t len,
                                               const char* asset_dir_utf8);

/* 建窗口 + GL 上下文（必须在创建 app 的同一线程调用）。
 * width / height <= 0 时取布局自带的窗口描述，再不行用 900x840。
 * 返回 1 = 成功；0 = 失败。 */
BINUI_C_API int32_t BINUI_C_CALL binui_app_open(void* app, int32_t width, int32_t height,
                                                uint32_t flags, const char* title_utf8);

/* ---------- 子窗口模式：把 binUI 作为控件嵌进宿主窗口 ----------
 *
 * 与 binui_app_open 的区别只有三处：
 *   1. 窗口是 WS_CHILD，父窗口是 parent_hwnd，位置由 (x, y, w, h) 指定；
 *   2. **不碰进程 DPI 策略**（DPI 由宿主决定），也不抢焦点；
 *   3. **不自己收消息** —— 宿主在自己的消息循环里 DispatchMessage，Windows 会把
 *      鼠标/键盘投递给本子窗口的窗口过程，事件照旧经 binui_poll_event 出来。
 *
 * 因此子窗口模式下每帧请调 **binui_render()** 而不是 binui_pump()。
 * flags 只认 BINUI_OPEN_NO_VSYNC：RESIZABLE / TRANSPARENT 对子窗口无意义
 * （WS_EX_LAYERED 不能用于子窗口，逐像素透明做不到），传了会被忽略。
 *
 * 宿主销毁自己的窗口时子窗口跟着消失，之后的 binui_render 返回 1。 */
BINUI_C_API int32_t BINUI_C_CALL binui_app_open_child(void* app, void* parent_hwnd,
                                                      int32_t x, int32_t y,
                                                      int32_t width, int32_t height,
                                                      uint32_t flags);

/* 只出一帧（推进动画 + 绘制脏区），**不碰消息队列**。
 * 返回 0 = 继续；1 = 窗口已关闭；-1 = 调用线程不是建窗线程（见下面的「线程亲和」）。
 * 子窗口模式的主循环入口。 */
BINUI_C_API int32_t BINUI_C_CALL binui_render(void* app);

/* 宿主改了本区域尺寸时调用（等价于子窗口收到 WM_SIZE，下一帧生效）。
 * 宿主用 SetWindowPos 改尺寸也行 —— 这条只是省掉一次消息往返。 */
BINUI_C_API void BINUI_C_CALL binui_resize(void* app, int32_t width, int32_t height);

/* 本窗口的 HWND（窗口没开则返回 NULL）。
 * 子窗口模式必备：宿主布局/移动/隐藏这块控件时要 SetWindowPos / ShowWindow 它。
 * 注意别拿它去 DestroyWindow —— 销毁请走 binui_app_destroy。 */
BINUI_C_API void* BINUI_C_CALL binui_app_hwnd(void* app);

/* 推进一帧：收消息 -> 推进动画 -> 绘制脏区。
 * timeout_ms > 0 时，空闲状态下阻塞等待输入（省 CPU）；0 = 立即返回。
 * 返回 0 = 继续；1 = 窗口已关闭（此后应停止 pump 并 destroy）；
 *      **-1 = 调用线程不是建窗线程**（见下面的「线程亲和」）。
 * 建议宿主每轮先 pump 再 poll，timeout 取 8~16ms。 */
BINUI_C_API int32_t BINUI_C_CALL binui_pump(void* app, int32_t timeout_ms);

/* 取一个事件。返回 1 = 已填充 out；0 = 队列空。 */
BINUI_C_API int32_t BINUI_C_CALL binui_poll_event(void* app, BinUIEvent* out);

/* 销毁（内部会先关窗口）。NULL 安全。
 *
 * ---------------------------------------------------------------------------
 * 线程亲和（**必读**，docs/issues/08）：窗口、消息循环、GL 上下文都有线程亲和。
 * binui_app_create / open / pump / render / resize / destroy 必须在**同一个 OS 线程**
 * 上调用。Win32 与 WGL 不会替你检查这一点，症状是随机 AV 或退出时挂死，日志里一句提示
 * 都没有 —— 所以本库自己拦了一道：
 *
 *   · binui_pump / binui_render 在非建窗线程上调用时返回 -1（并往 stderr 打一行说明）；
 *   · binui_app_destroy 在非建窗线程上调用时**拒绝执行**（窗口与 GL 上下文会泄漏到
 *     进程退出），并设置 binui_last_error —— 跨线程拆 Win32/WGL 只会崩，泄漏好过崩溃。
 *     注意这不会让 app 句柄失效：宿主可以继续持有它（比如直接 os.Exit）。
 *
 * Go 宿主特别注意：`main` goroutine **不是**固定在主线程上的 —— 运行期可能被调度到
 * 别的 M。必须在 main 里显式 `runtime.LockOSThread()`（不是"只加在子 goroutine"），
 * 或者直接用 `bindings/go` 里的 `binui.RunMain()`。 */
BINUI_C_API void BINUI_C_CALL binui_app_destroy(void* app);

/* ============================ 节点查询 ============================ */

BINUI_C_API int32_t BINUI_C_CALL binui_node_count(void* app);
/* 按下标取节点快照（下标 = 布局里的文档顺序）。返回 1 = 成功，0 = 越界。 */
BINUI_C_API int32_t BINUI_C_CALL binui_node_at(void* app, int32_t index, BinUINode* out);
/* 按节点 id 取快照。返回 1 = 成功，0 = 无此节点。 */
BINUI_C_API int32_t BINUI_C_CALL binui_node_by_id(void* app, int32_t id, BinUINode* out);
/* 按 HTML id / XML name 查节点 id。返回 id，未找到返回 -1。 */
BINUI_C_API int32_t BINUI_C_CALL binui_find_node(void* app, const char* name_utf8);

/* 取节点文本。返回文本字节数（不含结尾 NUL）；-1 = 无此节点。
 * cap 足够（>= 返回值 + 1）时写入 buf 并以 NUL 结尾；不够时只返回长度。
 * 两段式：先传 cap=0 拿长度，再分配。 */
BINUI_C_API int32_t BINUI_C_CALL binui_node_text(void* app, int32_t id, char* buf, int32_t cap);

/* ============================ 节点修改 ============================ */

/* 改文本（会自动重绘该节点）。len < 0 时按 NUL 结尾取长度。
 * 返回 1 = 成功，0 = 无此节点。
 *
 * 文本节点在编译期"没写 height、由引擎估算撑开"的（规格段 heightAuto=1），
 * 这里会按新文本重算高度（同一套粗估口径：CJK 1.0×、拉丁 0.52×字号，
 * 行距 = line-height，未声明按 1.35×）。作者显式写了 height 的节点不重排 ——
 * 需要自己撑高就用 binui_set_node_bounds。 */
BINUI_C_API int32_t BINUI_C_CALL binui_set_text(void* app, int32_t id,
                                                const char* utf8, int32_t len);
/* 显隐。返回 1 = 成功。 */
BINUI_C_API int32_t BINUI_C_CALL binui_set_visible(void* app, int32_t id, int32_t visible);
/* :checked 状态（自绘开关类控件用）。返回 1 = 成功。 */
BINUI_C_API int32_t BINUI_C_CALL binui_set_checked(void* app, int32_t id, int32_t on);
/* 只重绘一个节点（含其动画包围盒）。返回 1 = 成功，0 = 节点不可见。 */
BINUI_C_API int32_t BINUI_C_CALL binui_invalidate_node(void* app, int32_t id);
/* 全窗重绘（改了主题/大批节点后用）。 */
BINUI_C_API void BINUI_C_CALL binui_invalidate_all(void* app);
/* 命中测试：返回节点 id，未命中 -1。 */
BINUI_C_API int32_t BINUI_C_CALL binui_hit_test(void* app, float x, float y);
/* 当前窗口尺寸。参数可为 NULL。返回 1 = 成功（窗口已开）。 */
BINUI_C_API int32_t BINUI_C_CALL binui_app_size(void* app, int32_t* w, int32_t* h);

/* ============================ 运行期能力 ============================ */

/* "节点修改"那一组的口径是"布局编译期定死，运行期只改内容"。下面这一组是为列表 /
 * 聊天流这类"数量和高度都要运行期才知道"的界面补的：滚动、节点几何、动态增删节点。
 *
 * 共同限制（重要）：运行期新增的节点**不参与** setSize 的百分比 / flex 重排
 * （重排只认静态布局里的缩放规格段），也不会自动给兄弟节点让位 —— 需要自己挪。 */

/* 滚动容器：设置滚动偏移（自动钳制到内容范围）。追加一条消息后要"滚到底"，
 * 就先 binui_content_height() 再 scroll_to(..., ch - 视口高)。 */
BINUI_C_API void BINUI_C_CALL binui_scroll_to(void* app, int32_t node_id, float x, float y);
BINUI_C_API void BINUI_C_CALL binui_scroll_by(void* app, int32_t node_id, float dx, float dy);
/* 滚动容器的内容总高（子节点下界 - 容器顶）。无此节点返回 -1。 */
BINUI_C_API float BINUI_C_CALL binui_content_height(void* app, int32_t node_id);

/* 直接改节点矩形（宿主自己排版 / 手动撑高）。只动这一个节点，不牵动兄弟节点。
 * 返回 1 = 成功，0 = 无此节点或参数非法（NaN / 负尺寸）。
 *
 * 要"内容跟着盒子排"用下面三个（v21 局部重排）。 */
BINUI_C_API int32_t BINUI_C_CALL binui_set_node_bounds(void* app, int32_t node_id,
                                                       float x, float y, float w, float h);
/* 局部重排：盒子不变，只按当前盒子重排这棵子树（百分比 / 流式 / flex / min·max）。
 * 返回 1 = 成功，0 = 无此节点 / 布局没有规格段 / 等比缩放窗口（那里没有内容盒重排）。 */
BINUI_C_API int32_t BINUI_C_CALL binui_relayout_subtree(void* app, int32_t node_id);
/* 改矩形 + 钉住尺寸 + 局部重排（缩放拖拽内部就是它）。节点在流式/flex 父容器里时
 * 连兄弟位置一起修正。返回 1 = 成功。 */
BINUI_C_API int32_t BINUI_C_CALL binui_resize_node_bounds(void* app, int32_t node_id,
                                                          float x, float y, float w, float h);
/* 解除钉住：下次重排（含窗口 resize）回到作者声明的尺寸。 */
BINUI_C_API void BINUI_C_CALL binui_clear_bounds_override(void* app, int32_t node_id);

/* 动态节点：在 parent_id 下追加一个节点，返回新节点 id（>= 0）；失败返回 -1。
 * type 用 BINUI_NODE_*；color 为 0xAARRGGBB；radius / font_size 传 0 = 默认。
 * 这是"列表项数量运行期才知道"的官方出口（以前必须在布局里预留 N 个槽位）。 */
BINUI_C_API int32_t BINUI_C_CALL binui_add_node(void* app, int32_t parent_id, int32_t type,
                                                float x, float y, float w, float h,
                                                uint32_t color, int32_t radius,
                                                int32_t font_size);
/* 隐藏一个节点（节点保留在表里供复用，id 不会被回收）。返回 1 = 成功。 */
BINUI_C_API int32_t BINUI_C_CALL binui_remove_node(void* app, int32_t node_id);

/* 滚动条外观（轨道色 / 滑块色 / 轨道宽 2..32）。不调用时按根节点底色亮度自动派生：
 * 浅色主题给浅色滚动条，深色主题给深色 —— 不会再出现"浅底上一条黑杠"。
 * 返回 1 = 成功，0 = app 为空。宽度会被钳到 2..32。 */
BINUI_C_API int32_t BINUI_C_CALL binui_set_scrollbar_style(void* app, uint32_t track_argb,
                                                           uint32_t thumb_argb, int32_t width);
/* 回到"按根节点底色派生"的默认滚动条样式。 */
BINUI_C_API void BINUI_C_CALL binui_reset_scrollbar_style(void* app);

/* ============================ 窗口动作 ============================ */

BINUI_C_API void BINUI_C_CALL binui_request_exit(void* app);
BINUI_C_API void BINUI_C_CALL binui_minimize(void* app);
BINUI_C_API void BINUI_C_CALL binui_maximize(void* app);   /* 最大化 / 还原 */
BINUI_C_API void BINUI_C_CALL binui_begin_window_drag(void* app);

/* ============================ 资源 ============================ */

/* 用原始 RGBA8 像素（w*h*4 字节，行序自上而下）替换图片槽。
 * 程序化纹理 / 视频帧走这个（槽号 = 节点的 image_index）。返回 1 = 成功。 */
BINUI_C_API int32_t BINUI_C_CALL binui_set_image_raw(void* app, int32_t index,
                                                     int32_t w, int32_t h, const void* rgba);
/* 用编码数据（PNG / JPEG / WebP）替换图片槽。返回 1 = 成功。 */
BINUI_C_API int32_t BINUI_C_CALL binui_set_image(void* app, int32_t index,
                                                 const void* data, int32_t len);

/* 挂载布局里 <use ... external> 记录的外挂组件（路径相对 asset_dir / layout_dir 解析）。
 * 返回已挂载的组件数（>= 0）；-1 = 无此 app；失败项见 binui_last_error。 */
BINUI_C_API int32_t BINUI_C_CALL binui_mount_components(void* app, const char* layout_dir_utf8);

/* ============================ 键码 ============================ */

/* BINUI_EV_KEY 的 key 值：Win32 虚拟键码（VK_*）。
 *
 * 焦点 / 滚动 / 编辑三态由库自己路由（渲染器 BinUIRenderer::handleKey），宿主不要自己
 * 处理文本框编辑和焦点移动：
 *   · 编辑中：退格/删除/左右/Home/End 走光标；回车或 Esc 退出编辑回到焦点控制；
 *     Tab 退出并跳到下一个控件。字母归输入法，WASD 在这个状态下不移动焦点。
 *   · 非编辑：WASD 移动焦点（W 上 / A 左 / S 下 / D 右），方向键滚页面
 *     （离焦点最近的滚动容器，没有就滚视口内面积最大的那个），PageUp/PageDown 翻页，
 *     Home/End 到顶/到底，Tab/Shift+Tab 循环，回车激活焦点（输入框进入编辑、其余触发点击），
 *     空格有焦点时激活、无焦点时向下翻页。
 *   · 焦点环只在键盘导航时画，2 秒没有操作自动消失（鼠标点击不画；鼠标点进输入框
 *     直接进入编辑，那圈蓝环同样不画）。
 * 清空后 KEY 事件照旧推给宿主，宿主只做库做不了的事（Esc 关窗、Ctrl+C/V 由库内部接
 * 系统剪贴板）。 */
#define BINUI_KEY_BACK   0x08
#define BINUI_KEY_TAB    0x09
#define BINUI_KEY_RETURN 0x0D
#define BINUI_KEY_ESCAPE 0x1B
#define BINUI_KEY_LEFT   0x25
#define BINUI_KEY_UP     0x26
#define BINUI_KEY_RIGHT  0x27
#define BINUI_KEY_DOWN   0x28
#define BINUI_KEY_DELETE 0x2E
#define BINUI_KEY_HOME   0x24
#define BINUI_KEY_END    0x23

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* BINUI_C_H */
