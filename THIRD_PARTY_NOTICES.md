# 第三方组件声明

本项目本体按 [MIT](LICENSE) 授权。下列第三方组件**不受本仓库许可证约束**，各自遵循上游许可证；
它们不入库（约 130 MB），由 `sh scripts/fetch-deps.sh` 拉取到 `third_party/`。

| 组件 | 版本 | 用途 | 许可证 |
|---|---|---|---|
| [FreeType](https://freetype.org/) | VER-2-13-3 | 字形解析 / 光栅化 | [FTL 或 GPLv2](https://gitlab.freedesktop.org/freetype/freetype/-/blob/master/docs/FTL.TXT)（二选一） |
| [HarfBuzz](https://harfbuzz.github.io/) | 10.2.0 | 文本整形（复杂文字 / 连字 / 字距） | [MIT](https://github.com/harfbuzz/harfbuzz/blob/main/COPYING) |
| [FFmpeg](https://ffmpeg.org/) | 自行指定 | **可选**：`<video>` 解码 + WASAPI 音频（`BINUI_ENABLE_VIDEO=ON` 才链接） | [LGPL v2.1+ / GPL](https://ffmpeg.org/legal.html) |

## 分发注意

- **静态链接 FreeType** 时，若选择 FTL 许可，需在分发物中保留 FreeType 的版权与许可声明
  （见 `third_party/freetype/docs/FTL.TXT`）；若选择 GPLv2，则整个分发物需符合 GPLv2。
- **启用 FFmpeg 模块**（`BINUI_ENABLE_VIDEO=ON`）并静态链接时，LGPL 会要求提供可重新链接的目标文件
  或动态链接 FFmpeg 库；这是使用者自己的合规责任。默认 `OFF`（桩实现零外部依赖）。
- 字体文件（如系统字体、`tests/*.png` 之外的素材）不随仓库分发，由运行环境提供。
