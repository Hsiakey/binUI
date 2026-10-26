// binui_component.h - 外挂组件的运行期加载（header-only，外壳层）
//
// 编译期内联（默认）把组件并进一个 .bui，运行时零依赖；但有些场景需要
// "组件是独立文件、能单独替换"：插件面板、可换皮肤、按需加载的大模块。
// 这类组件在源码里写 <use src="x.bum" external>，编译器只留占位节点并把
// 组件路径记进 v12 组件引用段；本头文件负责在运行期把它们挂上去：
//
//   BinUIRenderer renderer;
//   renderer.loadLayout(bytes.data(), bytes.size());
//   binui::ex::MountAll(renderer, uiDir, renderer.layoutBytes());
//
// 组件的来源按 include/binui_asset.h 的规则解析：二进制 .bum 直接挂；
// 只有 .bum.xml 源码时就地编译（开发期友好，慢一点）；两者都有且源码更新
// 时优先源码 —— 改组件源码 + 热重载即可看到效果，不用重编 .bum。
#pragma once

#include <cstdio>
#include <string>
#include <vector>

#include "binui_asset.h"
#include "binui_compiler.h"
#include "binui_link.h"
#include "binui_paths.h"
#include "binui_renderer.h"

namespace binui {
namespace ex {

struct MountReport {
    int mounted = 0;   // 成功挂载的组件数
    int skipped = 0;   // 已挂载过（幂等跳过）
    int failed = 0;    // 失败（缺文件/编译失败/合并失败）
    bool ok() const { return failed == 0; }
};

// 挂载布局里记录的全部外挂组件。幂等：已挂过的锚点会跳过。
// uiDir：UI 文件所在目录（组件路径相对它解析）；传空则按进程 CWD 解析。
inline MountReport MountAll(BinUIRenderer& renderer, const std::string& uiDir,
                            const std::vector<uint8_t>& layout) {
    MountReport rep;
    const std::vector<ComponentRef> refs = ReadComponentRefs(layout.data(), layout.size());
    for (const auto& ref : refs) {
        if (renderer.isComponentMounted(ref.anchorId)) {
            ++rep.skipped;
            continue;
        }
        const AssetFile af = ProbeAsset(uiDir, ref.src);
        if (!af.valid()) {
            std::fprintf(stderr, "[binui] 外挂组件缺失：%s（锚点 #%u）\n",
                         ref.src.c_str(), (unsigned)ref.anchorId);
            ++rep.failed;
            continue;
        }
        std::vector<uint8_t> bytes;
        if (af.kind == AssetKind::SourceXml) {
            // 只有源码：就地编译（开发期；发行版应随包提供 .bum）
            CompileResult cr = CompileBinUIFileEx(af.path);
            for (const auto& w : cr.warnings)
                std::fprintf(stderr, "[binui] 组件 %s 编译告警: %s\n", af.path.c_str(), w.c_str());
            bytes = cr.layout;
        } else {
            bytes = ReadFileBytes(af.path);
        }
        if (bytes.empty()) {
            std::fprintf(stderr, "[binui] 外挂组件为空：%s\n", af.path.c_str());
            ++rep.failed;
            continue;
        }
        if (!renderer.mountComponent(bytes.data(), bytes.size(), ref.anchorId)) {
            std::fprintf(stderr, "[binui] 外挂组件挂载失败：%s（锚点 #%u）\n",
                         af.path.c_str(), (unsigned)ref.anchorId);
            ++rep.failed;
            continue;
        }
        ++rep.mounted;
    }
    return rep;
}

// 按布局内的资源段（v12）加载图片：调用方不用再保存编译期的图片列表。
inline std::unordered_map<std::string, std::vector<uint8_t>>
LoadLayoutImages(const BinUIRenderer& renderer, const std::string& uiPath,
                 const std::string& appName, const std::vector<std::string>& extraDirs = {}) {
    return LoadImages(uiPath, renderer.imagePaths(), appName, extraDirs);
}

} // namespace ex
} // namespace binui
