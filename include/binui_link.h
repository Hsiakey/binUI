// binui_link.h - 布局链接器：把一个组件布局并进宿主布局
//
// 一个 binUI 布局（.bui / .bum）是"节点树 + 若干可选段"的自包含二进制。
// 组件化有两条路，共用这里的同一份合并逻辑：
//
//   1) 内嵌（默认）：编译期把 <use src="x.bum"> 合并进宿主，产出单个 .bui
//      —— 运行时零依赖、零开销。
//   2) 外挂（<use src="x.bum" external>）：编译器留占位节点 + 组件引用段，
//      运行期由外壳调 BinUIRenderer::mountComponent() 现场合并。
//
// 合并语义：
//   · 组件节点重新编号（id 偏移 = 宿主最大 id + 1），整棵挂到锚点节点下
//   · 组件根是"合成根"（编译期包裹层，无实际视觉），合并时丢弃，
//     它的直接子节点改挂锚点，坐标加上锚点的 padding（内容原点）
//   · 字符串表整体后移并重算所有偏移（节点文本 / 视频 src / 图片路径 / 名称）
//   · 缩放规格段、状态样式段、动画段、视频段、资源段、组件引用段、名称段一并合并
//   · 窗口描述段不合并（宿主保留自己的）
//
// 注意：组件内节点的坐标按"相对父节点"解释（与 HTML 编译产物一致）。
// 用 <binui> 手写 XML（绝对坐标）当组件时，子节点位置会按相对坐标理解 ——
// 组件请用 HTML/XML 源码经编译器产出（.bum / .bui），不要手搓二进制。
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "binui_format.h"

namespace binui {

// 二进制布局嗅探：magic 合法 + 版本匹配 + 头部字段自洽（不解析内容）
bool IsLayoutData(const void* data, size_t size);

// 资源清单（按 imageIndex 顺序）；无资源段返回空
std::vector<std::string> ReadImagePaths(const void* data, size_t size);

// 外挂组件引用（v12 组件引用段）；无该段返回空
struct ComponentRef {
    uint16_t anchorId = 0;  // 占位节点 id
    std::string src;        // 组件路径（相对布局文件所在目录）
};
std::vector<ComponentRef> ReadComponentRefs(const void* data, size_t size);

struct LinkStats {
    uint16_t rootId = 0;      // 合并进来的第一个节点（组件合成根的第一个子节点）id
    uint16_t nodeCount = 0;   // 合并进来的节点数（不含被丢弃的合成根）
    uint32_t imageCount = 0;  // 合并后资源总数
};

// 合并选项（都可选）
struct LinkOptions {
    // 组件内相对资源路径（图片 / 视频 src / 嵌套组件）重定基：
    // compDir 是组件文件所在目录，hostDir 是宿主 UI 文件所在目录。
    // 两者都非空且不同时，路径按 "相对 compDir → 相对 hostDir" 改写；
    // 绝对路径原样保留。
    std::string compDir;
    std::string hostDir;
    // 组件根覆盖：来自 <use> 的 w / h / bg / radius 属性。
    // 二进制组件的参数在编 .bum 时已固化，这几个几何/底色属性是仅剩的
    // "宿主可覆盖项"（与源码内联时的行为保持一致）。
    bool  hasW = false, hasH = false, hasColor = false, hasRadius = false;
    float w = 0, h = 0, radius = 0;
    uint32_t color = 0;
    bool verbose = false;     // true = 合并失败时把原因打到 stderr
};

// 把组件布局 comp 挂到 host 的 anchorId 节点下（就地修改 host）。
// anchorId 必须存在于 host；失败返回 false 并把原因写进 err，host 保持不变。
bool MergeLayout(std::vector<uint8_t>& host, const void* comp, size_t compSize,
                 uint16_t anchorId, const LinkOptions* opts = nullptr,
                 LinkStats* stats = nullptr, std::string* err = nullptr);

// 同上，但组件从文件读取（读不到返回 false）；compDir 自动取文件所在目录
bool MergeLayoutFile(std::vector<uint8_t>& host, const std::string& compPath,
                     uint16_t anchorId, const LinkOptions* opts = nullptr,
                     LinkStats* stats = nullptr, std::string* err = nullptr);

// 读出布局版本（magic 不合法返回 0）
uint32_t LayoutVersion(const void* data, size_t size);

} // namespace binui
