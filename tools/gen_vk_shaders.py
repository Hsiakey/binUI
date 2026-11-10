import pathlib, struct

def spv_to_header(path):
    data = pathlib.Path(path).read_bytes()
    words = struct.unpack("<%dI" % (len(data)//4), data)
    return words

vs = spv_to_header("tools/vk_shaders/quad.vert.spv")
fs = spv_to_header("tools/vk_shaders/quad.frag.spv")

def emit(name, words):
    out = [f"static const uint32_t {name}[] = {{"]
    line = "    "
    for i, w in enumerate(words):
        tok = f"0x{w:08x}u, "
        if len(line) + len(tok) > 96:
            out.append(line.rstrip())
            line = "    "
        line += tok
    if line.strip():
        out.append(line.rstrip())
    out.append("};")
    return "\n".join(out)

hdr = f'''// binui_vk_shaders.h - Vulkan 后端的 SPIR-V（由 tools/vk_shaders/*.{{vert,frag}} 生成，勿手改）
//
// 生成方式（构建**不需要**这一步，产物已入库）：
//   glslc={chr(39)}<NDK>/shader-tools/windows-x86_64/glslc.exe{chr(39)}
//   $glslc --target-env=vulkan1.0 -O tools/vk_shaders/quad.vert -o tools/vk_shaders/quad.vert.spv
//   $glslc --target-env=vulkan1.0 -O tools/vk_shaders/quad.frag -o tools/vk_shaders/quad.frag.spv
//   py tools/gen_vk_shaders.py
//
// 背景：本机没有 Vulkan SDK，但 Android NDK 自带 glslc；SPIR-V 以 C 数组入库，
// 运行期不依赖任何 shader 工具链（与 binUI "构建只依赖 mingw + 入库产物" 的规矩一致）。
#ifndef BINUI_VK_SHADERS_H
#define BINUI_VK_SHADERS_H

#include <cstdint>

namespace binui {{

{emit("kVkQuadVertSpv", vs)}

{emit("kVkQuadFragSpv", fs)}

}} // namespace binui

#endif // BINUI_VK_SHADERS_H
'''
pathlib.Path("src/backends/binui_vk_shaders.h").write_text(hdr, encoding="utf-8")
print("words:", len(vs), len(fs))
