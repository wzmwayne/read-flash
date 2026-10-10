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
D8="${D8:-$(probe_tool d8 --version)}"       || { echo "找不到可执行的 d8" >&2; exit 1; }
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

APP="$ROOT/android/app/src/main"
OUT="$ROOT/build/android"
KS="$ROOT/build/debug.keystore"
APK="$ROOT/build/readflash-verify.apk"

echo "aapt2       = $AAPT2"
echo "d8          = $D8"
echo "apksigner   = $APKSIGNER"
echo "zipalign    = ${ZIPALIGN:-（本机无可用版本，跳过对齐）}"
echo "platform    = $PLATFORM"
echo "NDK         = $NDK ($PRE)"
echo "CC          = $CC"
echo "ABI / API   = $ABI / $API"

rm -rf "$OUT"
mkdir -p "$OUT/classes" "$OUT/dex" "$OUT/stage/lib/$ABI" "$OUT/lib/$ABI"

echo "[1/6] javac"
javac -nowarn -source 8 -target 8 -bootclasspath "$PLATFORM" \
      -d "$OUT/classes" "$APP/java/com/readflash/verify/MainActivity.java"

echo "[2/6] d8 -> classes.dex"
"$D8" --lib "$PLATFORM" --min-api "$API" --output "$OUT/dex" \
      "$OUT/classes/com/readflash/verify/MainActivity.class"
cp "$OUT/dex/classes.dex" "$OUT/stage/classes.dex"

echo "[3/6] clang -> librfverify.so ($ABI)"
"$CC" -O2 -fPIC -shared -Wall \
      --sysroot="$SYSROOT" \
      -I"$ROOT/src/core" \
      -o "$OUT/lib/$ABI/librfverify.so" \
      "$ROOT"/src/core/*.c "$APP/jni/rf_jni.c"
cp "$OUT/lib/$ABI/librfverify.so" "$OUT/stage/lib/$ABI/"

echo "[4/6] aapt2 link"
"$AAPT2" link -I "$PLATFORM" --manifest "$APP/AndroidManifest.xml" \
      --min-sdk-version "$API" --target-sdk-version 34 \
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
