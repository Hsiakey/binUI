# third_party —— 第三方源码（不入库）

本目录存放 **FreeType** 与 **HarfBuzz** 的源码，构建时由 CMake `add_subdirectory`
编成静态库链接进来（`Makefile` 路径使用预编译 `.a`，见 `Makefile` 的 `GL_LIBS`）。

因为体积（约 130 MB）远超仓库应有的体量，它们在 `.gitignore` 里被排除。**克隆后先拉取**：

```bash
sh scripts/fetch-deps.sh      # 或：make deps
```

脚本等价于（GitHub 不通时设 `GITHUB_MIRROR=https://你的镜像`）：

```bash
git clone --depth 1 --branch VER-2-13-3 https://github.com/freetype/freetype.git  third_party/freetype
git clone --depth 1 --branch 10.2.0    https://github.com/harfbuzz/harfbuzz.git  third_party/harfbuzz
```

## 版本

| 库 | 版本 | 许可证 |
|---|---|---|
| FreeType | VER-2-13-3 | FTL 或 GPLv2（二选一） |
| HarfBuzz | 10.2.0 | MIT |

版本升级后请重建静态库并在 `CHANGELOG.md` 记一笔——字形度量变化会影响文本换行结果。
