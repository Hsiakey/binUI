// binui_video.h - 可选视频编解码模块（v9）
//
// 核心库 binui 不依赖任何解码器：<video> 节点的播放状态由渲染器管理，
// 像素帧由"某个引擎"经 renderer.updateImageRaw(video.imageIndex, ...) 推送到
// 伪图片槽。本头文件提供该引擎的接入口：
//
//   BINUI_ENABLE_VIDEO=OFF（默认）→ 桩实现：Attach 恒返 false，
//                                   视频节点仅绘制底色，其余功能照常。
//   BINUI_ENABLE_VIDEO=ON         → src/binui_video_ffmpeg.cpp：
//                                   FFmpeg 解码（视频 sws→RGBA，音频 swr→混音格式）
//                                   + WASAPI 共享模式输出；渲染器播放时钟为主时钟。
//
// 用法：loadLayout + loadResources 之后、主循环开始前调用 AttachVideoEngine。
#pragma once
#include <string>
#include "binui_renderer.h"

namespace binui {

// 为 renderer 中每个 <video> 节点打开媒体并启动解码线程。
// baseDir：相对 src 的基准目录（通常传 exe 所在目录）；空 = 仅按原样/cwd 解析。
// 返回 false = 未启用解码模块或没有任何可打开的媒体（应用应给出降级提示）。
bool AttachVideoEngine(BinUIRenderer& renderer, const std::string& baseDir = "");

// 停止全部解码线程并释放资源（进程退出前调用；重复调用安全）
void DetachVideoEngine();

} // namespace binui
