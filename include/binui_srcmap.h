// binui_srcmap.h - 打包清单（.bui.srcmap）：把 .bui 还原成打包前的目录结构
//
// 为什么是"旁挂文件"而不是塞进 .bui：
//   · 不动二进制布局格式（不升 BINUI_VERSION，不改变加载路径与启动性能）
//   · 想解包就随包带上；不想暴露源码、不想包变大就不带 —— 主 .bui 一个字节都不变
//   · 存的是**原始文件内容**，解包是逐字节还原；不做"节点树反推 HTML"那种有损转换
//     （那种转换会把 CSS 级联、flex、{{参数}} 全拍平成绝对值，导出的是快照不是源码）
//
// 路径口径：一律**相对 appRootDir**（= 主 UI 所在目录），用 '/' 分隔。
// 所以把整个目录搬到别处再解包，还原出来的还是打包前的结构，不会带上编译机的绝对路径。
//
// 格式（小端，无压缩）：
//   char   magic[8] = "BINUSRC1"
//   uint32 count
//   repeat count 次 {
//       uint32 pathLen; char path[pathLen];   // 不含结尾 '\0'
//       uint32 dataLen; char data[dataLen];   // 原始文件字节（源码文本或 .bum 二进制）
//   }
//   第 0 条固定是主 UI 自己（路径 = 文件名，如 main.bui.html）。
#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace binui {

struct SrcMapEntry {
    std::string path;              // 相对 appRootDir
    std::vector<uint8_t> data;     // 原始字节
};

inline const char* SrcMapMagic() { return "BINUSRC1"; }

inline void SrcMapAppendU32(std::vector<uint8_t>& out, uint32_t v) {
    const uint8_t* p = reinterpret_cast<const uint8_t*>(&v);
    out.insert(out.end(), p, p + 4);
}

// 把条目序列化成 .bui.srcmap 内容（保持传入顺序，调用方保证第 0 条是主 UI）
inline std::vector<uint8_t> BuildSrcMap(const std::vector<SrcMapEntry>& entries) {
    std::vector<uint8_t> out;
    out.insert(out.end(), SrcMapMagic(), SrcMapMagic() + 8);
    SrcMapAppendU32(out, static_cast<uint32_t>(entries.size()));
    for (const auto& e : entries) {
        SrcMapAppendU32(out, static_cast<uint32_t>(e.path.size()));
        out.insert(out.end(), e.path.begin(), e.path.end());
        SrcMapAppendU32(out, static_cast<uint32_t>(e.data.size()));
        out.insert(out.end(), e.data.begin(), e.data.end());
    }
    return out;
}

// 解析失败返回 false（magic 不对 / 越界）
inline bool ParseSrcMap(const uint8_t* data, size_t size, std::vector<SrcMapEntry>& out) {
    out.clear();
    if (!data || size < 12) return false;
    if (std::memcmp(data, SrcMapMagic(), 8) != 0) return false;
    size_t off = 8;
    auto rdU32 = [&](uint32_t& v) -> bool {
        if (off + 4 > size) return false;
        std::memcpy(&v, data + off, 4);
        off += 4;
        return true;
    };
    uint32_t count = 0;
    if (!rdU32(count)) return false;
    if (count > (size - off) / 8 + 1) return false;   // 明显不合理的计数，别去分配
    for (uint32_t i = 0; i < count; i++) {
        uint32_t plen = 0, dlen = 0;
        if (!rdU32(plen)) return false;
        if (plen > size - off) return false;
        SrcMapEntry e;
        e.path.assign(reinterpret_cast<const char*>(data + off), plen);
        off += plen;
        if (!rdU32(dlen)) return false;
        if (dlen > size - off) return false;
        e.data.assign(data + off, data + off + dlen);
        off += dlen;
        out.push_back(std::move(e));
    }
    return true;
}

} // namespace binui
