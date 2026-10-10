#!/usr/bin/env bash
# 宿主构建与验证（Linux/macOS）
#   ./tools/build_host.sh verify   自检（网格/确定性/锚点/不重不漏）
#   ./tools/build_host.sh demo     端到端演示 + 导出真字形画面（PPM）
#   ./tools/build_host.sh all      两者都做（默认）
#
# 真字形：需要 FreeType 与一款中文字体（默认文泉驿等宽正黑，可用 RF_FONT 覆盖）。
# 没有字体时会退回占位方块并告警 —— 那种画面看不出错字，不要用它下结论。
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
TARGET="${1:-all}"

CFLAGS="-O2 -Wall -std=c99 -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -Isrc/core"
mkdir -p build/host

SRC="src/core/*.c src/pal/pal_posix.c"

build_verify() {
  echo "[1/2] 自检二进制"
  gcc $CFLAGS -o build/host/rf_verify $SRC src/host/main.c
  ./build/host/rf_verify | tail -3
}

build_demo() {
  echo "[2/2] 演示二进制（真字形）"
  if pkg-config --exists freetype2; then
    FT_CFLAGS="$(pkg-config --cflags freetype2)"
    FT_LIBS="$(pkg-config --libs freetype2)"
  else
    FT_CFLAGS=""; FT_LIBS="-lfreetype"
  fi
  gcc $CFLAGS $FT_CFLAGS -o build/host/app_demo \
      $SRC src/host/glyph_ft.c src/host/app_demo.c $FT_LIBS
  ./build/host/app_demo | sed -n '1,6p'
  if command -v convert >/dev/null 2>&1; then
    convert /tmp/01-library.ppm /tmp/05-settings.ppm /tmp/09-reader.ppm \
            +append -resize 1500x build/host/frames.png
    echo "画面已导出: build/host/frames.png（书库 / 设置 / 阅读）"
  fi
}

case "$TARGET" in
  verify) build_verify ;;
  demo)   build_demo ;;
  all)    build_verify; build_demo ;;
  *) echo "用法: $0 [verify|demo|all]" >&2; exit 1 ;;
esac
