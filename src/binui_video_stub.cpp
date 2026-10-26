// binui_video_stub.cpp - 视频引擎桩实现（未启用 BINUI_ENABLE_VIDEO 时编译）
// 恒返 false：应用据此显示降级提示；视频节点仅绘制底色，播放状态/控制条交互照常。
#include "binui_video.h"

namespace binui {

bool AttachVideoEngine(BinUIRenderer& /*renderer*/, const std::string& /*baseDir*/) {
    return false;
}

void DetachVideoEngine() {}

} // namespace binui
