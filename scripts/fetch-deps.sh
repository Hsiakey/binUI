#!/bin/sh
# 拉取第三方源码（FreeType + HarfBuzz）。已在场则跳过，可反复执行。
#
#   sh scripts/fetch-deps.sh
#   GITHUB_MIRROR=https://mirror.example.com  sh scripts/fetch-deps.sh   # 换镜像
#
# 版本与 third_party/README.md、CHANGELOG.md 保持一致。
set -e

ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"

FT_TAG=VER-2-13-3
HB_TAG=10.2.0
MIRROR=${GITHUB_MIRROR:-https://github.com}

mkdir -p third_party

clone() {
    dir=$1; url=$2; tag=$3
    if [ -d "third_party/$dir" ] && [ -n "$(ls -A "third_party/$dir" 2>/dev/null)" ]; then
        echo "[deps] third_party/$dir 已存在，跳过"
        return 0
    fi
    echo "[deps] clone $dir ($tag) <- $url"
    git clone --depth 1 --branch "$tag" "$url" "third_party/$dir"
}

clone freetype "$MIRROR/freetype/freetype.git" "$FT_TAG"
clone harfbuzz "$MIRROR/harfbuzz/harfbuzz.git" "$HB_TAG"

echo "[deps] 完成。CMake 构建会自动把两者编成静态库；"
echo "[deps] MinGW + Makefile 路径需要预编译静态库："
echo "[deps]   third_party/freetype/build/libfreetype.a"
echo "[deps]   third_party/harfbuzz/build-static/src/libharfbuzz.a"
