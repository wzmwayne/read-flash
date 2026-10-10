#!/usr/bin/env bash
# GitHub 文件下载：国内镜像竞速（ghproxy.net 与 cdn.gh-proxy.cn 并行，取最快完成的）。
#
# 网络策略（本项目约定）：
#   * git clone / git push / GitHub API —— 直连
#   * 用 curl 从 GitHub 下载文件（releases / raw / archive）—— 必须走本脚本
#   * 其它依赖下载 —— 使用国内镜像（清华 TUNA / 阿里云 / 华为云 / npmmirror）
#
# 用法：
#   tools/gh-dl.sh <github-url> [输出文件]
#   tools/gh-dl.sh https://github.com/user/repo/releases/download/v1/a.zip
#   tools/gh-dl.sh https://raw.githubusercontent.com/user/repo/main/README.md out.md
set -euo pipefail

if [ $# -lt 1 ]; then
  sed -n '2,14p' "$0" | sed 's/^# \{0,1\}//'
  exit 1
fi

URL="$1"
OUT="${2:-$(basename "${URL%%\?*}")}"

# 三路候选：两个国内镜像 + 直连兜底
CANDIDATES=(
  "https://ghproxy.net/${URL}"
  "https://cdn.gh-proxy.cn/${URL}"
  "${URL}"
)

TMP="$(mktemp -d)"
cleanup() { rm -rf "$TMP"; }
trap cleanup EXIT

PIDS=()
for i in "${!CANDIDATES[@]}"; do
  (
    if curl -fL --connect-timeout 8 --max-time "${GH_DL_TIMEOUT:-600}" \
            --retry 2 --retry-delay 1 -o "$TMP/$i.part" "${CANDIDATES[$i]}" 2>"$TMP/$i.err"; then
      mv "$TMP/$i.part" "$TMP/$i.ok"          # 原子出现 = 该线路完成
    fi
  ) &
  PIDS+=("$!")
done

WINNER=""
while [ -z "$WINNER" ]; do
  for i in "${!CANDIDATES[@]}"; do
    if [ -f "$TMP/$i.ok" ]; then WINNER="$i"; break; fi
  done
  [ -n "$WINNER" ] && break
  ALIVE=0
  for p in "${PIDS[@]}"; do kill -0 "$p" 2>/dev/null && ALIVE=1; done
  [ "$ALIVE" -eq 0 ] && break
  sleep 0.2
done

if [ -z "$WINNER" ]; then
  echo "三条线路均失败：" >&2
  for i in "${!CANDIDATES[@]}"; do
    printf '  %-60s %s\n' "${CANDIDATES[$i]}" "$(head -1 "$TMP/$i.err" 2>/dev/null || echo '无输出')" >&2
  done
  exit 1
fi

for p in "${PIDS[@]}"; do kill "$p" 2>/dev/null || true; done
mv "$TMP/$WINNER.ok" "$OUT"
echo "完成: $OUT"
echo "线路: ${CANDIDATES[$WINNER]}"
