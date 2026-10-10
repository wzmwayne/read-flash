#!/usr/bin/env bash
# Read-Flash 安卓验证 APK 构建
#   全离线：只用本机已装的 SDK/NDK，不下载任何东西。
#   自动探测"能在本机执行"的 build-tools / platform / NDK（多架构宿主机友好）。
#
# 可用环境变量覆盖：ANDROID_SDK ANDROID_NDK BUILD_TOOLS ANDROID_PLATFORM ANDROID_API ANDROID_ABI
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"

# ---- 候选 SDK 根目录（按优先级） ----
SDK_ROOTS=()
[ -n "${ANDROID_SDK:-}" ]      && SDK_ROOTS+=("$ANDROID_SDK")
[ -n "${ANDROID_NDK_ROOT:-}" ] && SDK_ROOTS+=("$(dirname "$(dirname "$ANDROID_NDK_ROOT")")")
SDK_ROOTS+=(/usr/lib/android-sdk "${ANDROID_SDK_ROOT:-}" "${ANDROID_HOME:-}" /tmp/android-sdk)

# ---- 构建目标：app（正式应用） / verify（最小验证） ----
TARGET="${1:-app}"
case "$TARGET" in
  app)    SRC="$ROOT/android/app";    LIB=readflash; APKNAME=readflash.apk;        JNI=rf_app_jni.c ;;
  verify) SRC="$ROOT/android/verify"; LIB=rfverify;  APKNAME=readflash-verify.apk; JNI=rf_jni.c ;;
  *) echo "用法: $0 [app|verify]" >&2; exit 1 ;;
esac

API="${ANDROID_API:-26}"
ABI="${ANDROID_ABI:-arm64-v8a}"
case "$ABI" in
  arm64-v8a)   TRIPLE=aarch64-linux-android ;;
  armeabi-v7a) TRIPLE=armv7a-linux-androideabi ;;
  x86_64)      TRIPLE=x86_64-linux-android ;;
  *) echo "不支持的 ABI: $ABI" >&2; exit 1 ;;
esac

# ---- 探测：单个工具是否能在本机执行（SIGILL/找不到 = 不可用） ----
try_tool() {
  local exe="$1"; shift
  local rc
  if "$exe" "$@" >/dev/null 2>&1; then rc=0; else rc=$?; fi
  [ "$rc" -lt 126 ]
}
list_tools() {   # $1 = 工具名：列出所有候选（按版本新→旧）
  local name="$1" root d
  for root in "${SDK_ROOTS[@]}"; do
    [ -d "$root/build-tools" ] || continue
    for d in $(ls -d "$root"/build-tools/*/ 2>/dev/null | sort -Vr); do
      [ -x "${d}${name}" ] && echo "${d%/}/${name}"
    done
  done
}

probe_tool() {   # $1 = 工具名，其余为探测参数
  local name="$1"; shift
  local root d exe
  for root in "${SDK_ROOTS[@]}"; do
    [ -d "$root/build-tools" ] || continue
    for d in $(ls -d "$root"/build-tools/*/ 2>/dev/null | sort -Vr); do
      exe="${d}${name}"
      [ -x "$exe" ] || continue
      if try_tool "$exe" "$@"; then echo "${exe%/}"; return 0; fi
    done
  done
  return 1
}

# ---- 探测：android.jar ----
probe_platform() {
  local root p
  for root in "${SDK_ROOTS[@]}"; do
    [ -d "$root/platforms" ] || continue
    for p in $(ls -d "$root"/platforms/android-*/ 2>/dev/null | sort -Vr); do
      [ -f "${p}android.jar" ] && { echo "${p}android.jar"; return 0; }
    done
  done
  return 1
}

# ---- 探测：能在本机执行的 NDK clang ----
probe_ndk() {
  local root n c pre
  for root in "${SDK_ROOTS[@]}"; do
    [ -d "$root/ndk" ] || continue
    for n in $(ls -d "$root"/ndk/*/ 2>/dev/null | sort -Vr); do
      for pre in linux-x86_64 linux-aarch64; do
        c="${n}toolchains/llvm/prebuilt/${pre}/bin/${TRIPLE}${API}-clang"
        if [ -x "$c" ] && "$c" --version >/dev/null 2>&1; then
          echo "${n%/}|$pre|$c"; return 0
        fi
      done
    done
  done
  return 1
}

AAPT2="${AAPT2:-$(probe_tool aapt2 version)}" || { echo "找不到可执行的 aapt2" >&2; exit 1; }
D8="${D8:-$(probe_tool d8 --version || true)}"   # 仅作展示；实际在步骤 2 逐个实测
APKSIGNER="${APKSIGNER:-$(probe_tool apksigner version)}" || { echo "找不到可执行的 apksigner" >&2; exit 1; }
ZIPALIGN="$(probe_tool zipalign -h || true)"   # 可选：本机无可用 zipalign 时跳过对齐

if [ -n "${ANDROID_PLATFORM:-}" ]; then PLATFORM="$ANDROID_PLATFORM"; else PLATFORM="$(probe_platform)" || { echo "找不到 android.jar" >&2; exit 1; }; fi
if [ -n "${ANDROID_NDK:-}" ]; then
  NDK="$ANDROID_NDK"; PRE=linux-x86_64
  CC="$NDK/toolchains/llvm/prebuilt/$PRE/bin/${TRIPLE}${API}-clang"
else
  IFS='|' read -r NDK PRE CC <<< "$(probe_ndk)" || { echo "找不到可执行的 NDK clang" >&2; exit 1; }
fi
SYSROOT="$NDK/toolchains/llvm/prebuilt/$PRE/sysroot"

# ---- 版本号（格式：大版本.小版本 + d/r + 细碎修订，例 0.1.1d1） ----
VER="$(cat "$ROOT/VERSION" 2>/dev/null | tr -d '\n' || echo 0.0.0d0)"
MAJ="$(echo "$VER" | cut -d. -f1)"; MIN="$(echo "$VER" | cut -d. -f2)"
REST="$(echo "$VER" | cut -d. -f3)"
SUB="$(echo "$REST" | sed 's/[^0-9].*$//')"; PAT="$(echo "$REST" | sed 's/^[0-9]*[a-z]*//')"
[ -z "$SUB" ] && SUB=0; [ -z "$PAT" ] && PAT=0
VCODE=$(( MAJ * 10000 + MIN * 100 + SUB * 10 + PAT ))

APP="$SRC"
OUT="$ROOT/build/android"
KS="$ROOT/build/debug.keystore"
APK="$ROOT/build/$APKNAME"

echo "aapt2       = $AAPT2"
echo "d8          = $D8"
echo "apksigner   = $APKSIGNER"
echo "zipalign    = ${ZIPALIGN:-（本机无可用版本，跳过对齐）}"
echo "platform    = $PLATFORM"
echo "NDK         = $NDK ($PRE)"
echo "CC          = $CC"
echo "目标 / ABI  = $TARGET ($LIB) / $ABI, API $API"
echo "版本        = $VER (versionCode $VCODE)"

rm -rf "$OUT"
mkdir -p "$OUT/classes" "$OUT/dex" "$OUT/stage/lib/$ABI" "$OUT/lib/$ABI"

echo "[1/6] javac"
javac -nowarn -source 8 -target 8 -bootclasspath "$PLATFORM" \
      -d "$OUT/classes" $(find "$APP/java" -name '*.java')

echo "[2/6] d8 -> classes.dex"
# 必须把 javac 产出的全部 class（含匿名内部类 MainActivity$1）都交给 d8；
# 旧版 R8 处理嵌套匿名类会内部报错，故逐个候选实测，选第一个能成功的。
CLASSES="$(find "$OUT/classes" -name '*.class' | sort)"
D8_USED=""
for cand in $(list_tools d8); do
  rm -rf "$OUT/dex"; mkdir -p "$OUT/dex"
  if "$cand" --lib "$PLATFORM" --min-api "$API" --output "$OUT/dex" $CLASSES \
        >"$OUT/d8.log" 2>&1 && [ -s "$OUT/dex/classes.dex" ]; then
    D8_USED="$cand"; break
  fi
done
if [ -z "$D8_USED" ]; then
  echo "所有候选 d8 均失败，最后一个的日志：" >&2
  tail -20 "$OUT/d8.log" >&2
  exit 1
fi
echo "      d8 = $D8_USED"
cp "$OUT/dex/classes.dex" "$OUT/stage/classes.dex"

echo "[3/6] clang -> lib$LIB.so ($ABI)"
"$CC" -O2 -fPIC -shared -Wall \
      --sysroot="$SYSROOT" \
      -I"$ROOT/src/core" -DRF_VERSION=\"$VER\" \
      -o "$OUT/lib/$ABI/lib$LIB.so" \
      "$ROOT"/src/core/*.c "$ROOT"/src/pal/pal_posix.c "$APP/jni/$JNI"
cp "$OUT/lib/$ABI/lib$LIB.so" "$OUT/stage/lib/$ABI/"

echo "[4/6] aapt2 link"
"$AAPT2" link -I "$PLATFORM" --manifest "$APP/AndroidManifest.xml" \
      --min-sdk-version "$API" --target-sdk-version 34 \
      --version-name "$VER" --version-code "$VCODE" \
      -o "$OUT/base.apk"

echo "[5/6] 打包 + zipalign"
cp "$OUT/base.apk" "$OUT/unsigned.apk"
( cd "$OUT/stage" && zip -q -r "$OUT/unsigned.apk" classes.dex "lib" )
if [ -n "$ZIPALIGN" ]; then
  "$ZIPALIGN" -f 4 "$OUT/unsigned.apk" "$OUT/aligned.apk"
else
  cp "$OUT/unsigned.apk" "$OUT/aligned.apk"     # 无 zipalign：直接签名（adb install 可用）
fi

echo "[6/6] 签名"
if [ ! -f "$KS" ]; then
  keytool -genkeypair -keystore "$KS" -alias androiddebugkey \
          -storepass android -keypass android \
          -dname "CN=Android Debug,O=Android,C=US" \
          -keyalg RSA -keysize 2048 -validity 10000
fi
"$APKSIGNER" sign --ks "$KS" --ks-pass pass:android --key-pass pass:android \
      --out "$APK" "$OUT/aligned.apk"
"$APKSIGNER" verify --print-certs "$APK" | head -3

echo
echo "完成: $APK"
echo "安装: adb install -r $APK"
echo "看结果: adb logcat -s RFVERIFY:I"
