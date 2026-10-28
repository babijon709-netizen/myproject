#!/usr/bin/env bash
# build.sh — builds liboxhk.so + oxhk dex and repacks the game around them.
#
#   ./build.sh --no-apk                       build artifacts only
#   ./build.sh --probe base.apk split.apk     repack + resign, NOTHING injected (control test:
#                                             does the pglarmor/loader chain enforce signature?)
#   ./build.sh base.apk split.apk             full injection: dex -> base, liboxhk.so -> split
#
# env: NDK, SDK, API (compiler api level, default 26), MIN_API (d8, default 24),
#      KEYSTORE/ALIAS/STOREPASS (defaults: build/oxhk.jks, oxhk, oxhkpass)
set -euo pipefail

HERE="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$HERE/build"
: "${SDK:=${ANDROID_SDK_ROOT:-${ANDROID_HOME:-}}}"
if [ -z "${NDK:-}" ]; then
  for cand in "${ANDROID_NDK_HOME:-}" "$SDK/ndk/"*; do
    [ -n "$cand" ] && [ -d "$cand" ] && NDK="$cand"
  done
fi
[ -n "$SDK" ] && [ -n "$NDK" ] || { echo "set SDK (or ANDROID_HOME) and NDK (or ANDROID_NDK_HOME)"; exit 1; }
: "${API:=26}"
: "${BUILD_TOOLS:=$(ls -d "$SDK/build-tools"/* 2>/dev/null | sort -V | tail -1)}"
: "${ANDROID_JAR:=$(ls -d "$SDK/platforms"/android-*/android.jar 2>/dev/null | sort -V | tail -1)}"
: "${MIN_API:=24}"
: "${KEYSTORE:=$OUT/oxhk.jks}"
: "${ALIAS:=oxhk}"
: "${STOREPASS:=oxhkpass}"

MODE=full
case "${1:-}" in
  --no-apk) MODE=artifacts; shift ;;
  --probe)  MODE=probe; shift ;;
esac

mkdir -p "$OUT"

verify_artifacts() {
  echo "== verify"
  local so="$OUT/liboxhk.so" dex="$OUT/dex/classes.dex"
  readelf -h "$so" | grep -q 'AArch64' || { echo "liboxhk.so is not AArch64"; exit 1; }
  readelf --dyn-syms -W "$so" | grep -q 'Java_oxhk_Oxhk_nativeTick' \
      || { echo "JNI entry points missing from liboxhk.so"; exit 1; }
  readelf --dyn-syms -W "$so" | grep -q 'Java_oxhk_Oxhk_nativeInit' \
      || { echo "nativeInit missing"; exit 1; }
  [ "$(head -c 3 "$dex")" = "dex" ] || { echo "classes.dex has no dex magic"; exit 1; }
  grep -qa 'oxhk/Oxhk' "$dex" || { echo "classes.dex does not contain oxhk/Oxhk"; exit 1; }
  # only system libs may be needed: anything else has to be inside our apk
  local bad
  bad=$(readelf -d "$so" | sed -n 's/.*NEEDED.*\[\(.*\)\]/\1/p' \
        | grep -Ev '^(liblog|libandroid|libm|libdl|libc|libz)\.so$' || true)
  [ -z "$bad" ] || { echo "liboxhk.so depends on a non-system lib: $bad"; exit 1; }
  echo "   liboxhk.so $(stat -c%s "$so") B, classes.dex $(stat -c%s "$dex") B"
}


if [ "$MODE" != probe ]; then
  CC="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android$API-clang++"
  echo "== native"
  # -static-libstdc++: without it the .so depends on libc++_shared.so, which is not in the game's
  # lib dir on every build, and dlopen then fails with "library libc++_shared.so not found"
  "$CC" -shared -fPIC -O2 -std=c++17 -fvisibility=hidden -static-libstdc++ \
        -Wl,-soname,liboxhk.so -Wl,--no-undefined \
        -o "$OUT/liboxhk.so" "$HERE/native/oxhk.cpp" -llog -landroid

  echo "== java"
  rm -rf "$OUT/javac" "$OUT/dex" && mkdir -p "$OUT/javac" "$OUT/dex"
  javac -source 8 -target 8 -nowarn -bootclasspath "$ANDROID_JAR" \
        -d "$OUT/javac" "$HERE"/java/oxhk/*.java
  "$BUILD_TOOLS/d8" --release --min-api "$MIN_API" --lib "$ANDROID_JAR" \
        --output "$OUT/dex" $(find "$OUT/javac" -name '*.class')
fi

verify_artifacts

if [ "$MODE" = artifacts ]; then
  echo "artifacts: $OUT/liboxhk.so  $OUT/dex/classes.dex"
  exit 0
fi

if [ ! -f "$KEYSTORE" ]; then
  keytool -genkeypair -keystore "$KEYSTORE" -alias "$ALIAS" -keyalg RSA -keysize 2048 \
          -validity 10950 -storepass "$STOREPASS" -keypass "$STOREPASS" -dname "CN=oxhk"
fi
sign() { "$BUILD_TOOLS/apksigner" sign --ks "$KEYSTORE" --ks-pass "pass:$STOREPASS" \
         --ks-key-alias "$ALIAS" --out "$2" "$1"; }

BASE_IN="$1"; SPLIT_IN="${2:-}"
WORK="$OUT/apk"
rm -rf "$WORK"
echo "== apktool decode $(basename "$BASE_IN")"
apktool d -f --use-aapt2 -o "$WORK" "$BASE_IN" >/dev/null

if [ "$MODE" = full ]; then
  max=1
  for f in "$WORK"/classes*.dex; do
    [ -e "$f" ] || continue
    n="${f##*/classes}"; n="${n%.dex}"
    max=$(( n > max ? n : max ))
  done
  slot=$((max + 1))
  cp "$OUT/dex/classes.dex" "$WORK/classes$slot.dex"
  echo "== dex -> classes$slot.dex"

  echo "== Application -> oxhk.App"
  python3 - "$WORK/AndroidManifest.xml" <<'PY'
import re, sys
p = sys.argv[1]
s = open(p).read()
m = re.search(r'<application\b[^>]*>', s)
tag = m.group(0)
orig = re.search(r'android:name="([^"]+)"', tag)
orig_cls = orig.group(1) if orig else "android.app.Application"
if orig:
    s = s.replace(tag, tag.replace(orig.group(0), 'android:name="oxhk.App"'), 1)
else:
    s = s.replace(tag, tag[:-1] + ' android:name="oxhk.App">', 1)
open(p, "w").write(s)
open(p + ".origapp", "w").write("L" + orig_cls.replace(".", "/") + ";")
print("   was:", orig_cls)
PY

  SMALI_ORIG="$(cat "$WORK/AndroidManifest.xml.origapp")"
  mkdir -p "$WORK/smali/oxhk"
  cat > "$WORK/smali/oxhk/App.smali" <<EOF
.class public Loxhk/App;
.super $SMALI_ORIG
.source "App.java"

.method public constructor <init>()V
    .locals 0
    invoke-direct {p0}, $SMALI_ORIG-><init>()V
    return-void
.end method

.method public onCreate()V
    .locals 0
    invoke-super {p0}, $SMALI_ORIG->onCreate()V
    invoke-static {p0}, Loxhk/Oxhk;->install(Landroid/content/Context;)V
    return-void
.end method
EOF
fi

echo "== apktool build"
apktool b --use-aapt2 "$WORK" -o "$OUT/base-unsigned.apk" >/dev/null

NAME="$(basename "$BASE_IN" .apk)"
"$BUILD_TOOLS/zipalign" -p -f 4 "$OUT/base-unsigned.apk" "$OUT/base-aligned.apk"
sign "$OUT/base-aligned.apk" "$OUT/$NAME-oxhk.apk"
echo "base  : $OUT/$NAME-oxhk.apk"

if [ -n "$SPLIT_IN" ]; then
  SNAME="$(basename "$SPLIT_IN" .apk)"
  if [ "$MODE" = full ]; then
    echo "== liboxhk.so -> split lib/arm64-v8a (stored, page aligned)"
    cp "$SPLIT_IN" "$OUT/$SNAME-oxhk.apk"
    rm -rf "$OUT/libstage" && mkdir -p "$OUT/libstage/lib/arm64-v8a"
    cp "$OUT/liboxhk.so" "$OUT/libstage/lib/arm64-v8a/"
    ( cd "$OUT/libstage" && zip -0 -X -q "$OUT/$SNAME-oxhk.apk" lib/arm64-v8a/liboxhk.so )
    "$BUILD_TOOLS/zipalign" -p -f 4 "$OUT/$SNAME-oxhk.apk" "$OUT/$SNAME-aligned.apk"
    sign "$OUT/$SNAME-aligned.apk" "$OUT/$SNAME-oxhk.apk"
  else
    cp "$SPLIT_IN" "$OUT/$SNAME-oxhk.apk"
    sign "$SPLIT_IN" "$OUT/$SNAME-oxhk.apk"
  fi
  echo "split : $OUT/$SNAME-oxhk.apk"
fi

cat <<'EOT'

install (all in one session, old package removed first):
  adb uninstall com.catsbit.oxidesurvivalisland
  adb install-multiple -r build/base-oxhk.apk build/split_config.arm64_v8a-oxhk.apk
log:
  adb logcat -s oxhk Unity AndroidRuntime
EOT
