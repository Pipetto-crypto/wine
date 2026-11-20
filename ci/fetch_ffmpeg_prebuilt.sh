#!/usr/bin/env bash
# 用 as14725836/glibc-packages 里预编译好的 FFmpeg 8.1.1（Termux glibc ABI）
# 代替在 CI 里从源码编译，产出与 build_ffmpeg_ge.sh 完全一致的布局：
#
#   $FFMPEG_PREFIX/lib/libav*.so*        （实体 + soname 链接）
#   $FFMPEG_PREFIX/lib/pkgconfig/*.pc    （prefix 已改写为 $FFMPEG_PREFIX）
#   $FFMPEG_PREFIX/include/libav*/...
#   $FFMPEG_PREFIX/lib/libx264/libx265/libvpx/libwebp/libtheora/libxvidcore
#       —— 这些是 libavcodec 的 DT_NEEDED，必须一起随包分发
#
# 架构说明（重要）：
#   本仓库这条 wine-wow64 流水线在 x86_64 runner 上编译，产物是
#   lib/wine/x86_64-unix（设备侧靠 box64 执行 x86_64 代码），因此依赖
#   必须是 x86_64 的 .so。aarch64 包只有在原生 aarch64 构建里才用得上，
#   混用会在链接/加载时报 wrong ELF class。所以默认 ARCH=x86_64，
#   可用 FFMPEG_PKG_ARCH 覆盖。
set -u

PREFIX="${FFMPEG_PREFIX:-/tmp/ffmpeg}"
RELEASE="${FFMPEG_PKG_RELEASE:-packages-39239132896}"
ARCH="${FFMPEG_PKG_ARCH:-x86_64}"
VER="${FFMPEG_VERSION:-8.1.1}"
BASE="https://github.com/as14725836/glibc-packages/releases/download/${RELEASE}"
CACHE="${FFMPEG_PKG_CACHE:-/tmp/ffmpeg-pkg}"
TERMUX_GLIBC="/data/data/com.termux/files/usr/glibc"

log() { echo "[ffmpeg-pkg] $*"; }
die() { echo "[ffmpeg-pkg][FATAL] $*" >&2; exit 1; }

# ffmpeg 本体 + libavcodec 的共享依赖（同一 Release 里的其它包）
PKGS=(
  "ffmpeg-glibc-${VER}-1-${ARCH}"
  "libvpx-glibc-1.17.0-2-${ARCH}"
  "libwebp-glibc-1.6.0-0-${ARCH}"
  "libtheora-glibc-1.2.0-0-${ARCH}"
  "libx264-glibc-0.164.3191-0-${ARCH}"
  "libx265-glibc-4.3-0-${ARCH}"
  "xvidcore-glibc-1.3.7-0-${ARCH}"
)

log "release: $RELEASE  arch: $ARCH  prefix: $PREFIX"
mkdir -p "$CACHE" || die "无法创建缓存目录 $CACHE"

# 可选：私有/限流时带 token。用数组避免引号被分词撕裂。
CURL_AUTH=()
if [ -n "${GHT:-}" ]; then CURL_AUTH=(-H "Authorization: token $GHT"); fi

for p in "${PKGS[@]}"; do
  f="$CACHE/$p.pkg.tar.xz"
  if [ -s "$f" ] && [ "$(stat -c %s "$f")" -gt 10000 ]; then
    log "  缓存命中 $p"
    continue
  fi
  log "  下载 $p"
  if ! curl -fL --retry 3 --retry-delay 2 -m 600 \
        "${CURL_AUTH[@]}" \
        -o "$f" "$BASE/$p.pkg.tar.xz"; then
    die "下载失败: $BASE/$p.pkg.tar.xz"
  fi
  [ -s "$f" ] || die "下载得到空文件: $p"
done

STAGE="$(mktemp -d /tmp/ffmpeg-stage.XXXXXX)" || die "mktemp 失败"
for p in "${PKGS[@]}"; do
  tar -xf "$CACHE/$p.pkg.tar.xz" -C "$STAGE" || die "解包失败: $p"
done

ROOT="$(find "$STAGE" -maxdepth 12 -type d -path '*glibc/include/libavcodec' 2>/dev/null | head -n 1)"
[ -n "$ROOT" ] || die "解包后找不到 glibc/include/libavcodec"
ROOT="$(dirname "$(dirname "$ROOT")")"
log "  包根目录: ${ROOT#$STAGE}"

rm -rf "$PREFIX"
mkdir -p "$PREFIX/lib" "$PREFIX/include" || die "无法创建 $PREFIX"
cp -a "$ROOT/lib/." "$PREFIX/lib/" || die "拷贝 lib 失败"
cp -a "$ROOT/include/." "$PREFIX/include/" || die "拷贝 include 失败"

# .pc 里的 prefix 是设备路径，全部改写成本次构建的 prefix；
# 同时给 Libs 加 -Wl,--allow-shlib-undefined。
#
# 为什么必须加：这批包是照 glibc 2.44 编的（libavutil.so.60 里有
# sinh@GLIBC_2.44 / cosh@GLIBC_2.44），而构建 runner 是 Ubuntu 24.04
# （glibc 2.39）。链接可执行文件时 ld 会尝试从 runner 的 libc 解析
# 这些版本符号，必然失败，于是 Wine configure 报
#   checking for av_log_set_callback in -lavutil... no
#   configure: error: FFmpeg 64-bit development files not found.
# 放行共享库的未定义符号后，链接期不再追这部分；运行时由设备侧
# （Termux glibc >= 2.44）提供。
for pc in "$PREFIX"/lib/pkgconfig/*.pc; do
  [ -f "$pc" ] || continue
  sed -i "s#$TERMUX_GLIBC#$PREFIX#g" "$pc"
  if ! grep -q -- '--allow-shlib-undefined' "$pc"; then
    sed -i -E 's|^(Libs:.*)$|\1 -Wl,--allow-shlib-undefined|' "$pc"
  fi
done
# .la 是 libtool 产物，Wine 用不到，还会把绝对路径钉死
rm -f "$PREFIX"/lib/*.la

# libtheoraenc DT_NEEDED libogg.so.0；libogg 不在这个 Release 里，
# 用 apt 的 libogg0 供打包脚本收集（设备侧本来就有 libogg）。
if ! ldconfig -p 2>/dev/null | grep -q 'libogg.so.0'; then
  sudo apt-get install -y -qq libogg0 >/dev/null 2>&1 || true
fi

[ -f "$PREFIX/lib/pkgconfig/libavformat.pc" ] || die "缺少 libavformat.pc"
[ -e "$PREFIX/lib/libavcodec.so" ] || die "缺少 libavcodec.so"

log "  实体库: $(ls -1 "$PREFIX"/lib/*.so.* 2>/dev/null | wc -l) 个"
log "  pkg-config 版本:"
PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig" pkg-config --modversion \
  libavutil libavformat libavcodec libswscale libswresample 2>/dev/null \
  | sed 's/^/    /' || true
log "  FFMPEG_CFLAGS: $(PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig" pkg-config --cflags libavutil libavformat libavcodec 2>/dev/null)"
log "  FFMPEG_LIBS  : $(PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig" pkg-config --libs libavutil libavformat libavcodec 2>/dev/null)"
ABI="$(PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig" pkg-config --modversion libavformat 2>/dev/null || echo unknown)"
if [ -n "${GITHUB_ENV:-}" ]; then
  {
    echo "FFMPEG_ROOT=$PREFIX"
    echo "FFMPEG_ABI=$ABI"
  } >> "$GITHUB_ENV"
fi
log "  libavformat ABI: $ABI"
log "OK: 预编译 FFmpeg 就绪 @ $PREFIX"
rm -rf "$STAGE"
