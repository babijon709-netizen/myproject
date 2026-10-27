#!/usr/bin/env bash
# build.sh — builds liboxhk.so + oxhk dex, then repacks the game's APK around them.
#
# usage:
#   NDK=/opt/android-ndk SDK=/opt/android-sdk ./build.sh base.apk [split...] 
#   NDK=... SDK=... ./build.sh --no-apk      # only build artifacts
#
# needs: android NDK r26+ , android build-tools (d8, zipalign, apksigner), platform android.jar,
#        apktool on PATH, java, zip
set -euo pipefail

HERE="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$HERE/build"
: "${NDK:?set NDK to your ndk dir}"
: "${SDK:?set SDK to your android sdk dir}"
: "${API:=26}"
: "${BUILD_TOOLS:=$(ls -d "$SDK/build-tools"/* 2>/dev/null | sort -V | tail -1)}"
: "${ANDROID_JAR:=$(ls -d "$SDK/platforms"/android-*/android.jar 2>/dev/null | sort -V | tail -1)}"
: "${MIN_API:=24}"
: "${KEYSTORE:=$HERE/build/oxhk.jks}"
: "${ALIAS:=oxhk}"
: "${STOREPASS:=oxhkpass}"

CC="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android$API-clang++"
mkdir -p "$OUT"

echo "== native"
"$CC" -shared -fPIC -O2 -std=c++17 -fvisibility=hidden -Wl,--hash-style=both \
      -Wl,-soname,liboxhk.so \
      -o "$OUT/liboxhk.so" "$HERE/native/oxhk.cpp" -llog -landroid

echo "== java"
rm -rf "$OUT/javac" "$OUT/dex" && mkdir -p "$OUT/javac" "$OUT/dex"
javac -source 8 -target 8 -nowarn -bootclasspath "$ANDROID_JAR" \
      -d "$OUT/javac" "$HERE"/java/oxhk/*.java
"$BUILD_TOOLS/d8" --release --min-api "$MIN_API" --lib "$ANDROID_JAR" \
      --output "$OUT/dex" $(find "$OUT/javac" -name '*.class')

if [ "${1:-}" = "--no-apk" ]; then
  echo "artifacts: $OUT/liboxhk.so  $OUT/dex/classes.dex"
  exit 0
fi

APK_IN="$1"; shift || true
WORK="$OUT/apk"
rm -rf "$WORK"
echo "== apktool decode $APK_IN"
apktool d -f --use-aapt2 -o "$WORK" "$APK_IN" >/dev/null

echo "== dex slot"
max=1
for f in "$WORK"/classes*.dex; do
  [ -e "$f" ] || continue
  n="${f##*/classes}"; n="${n%.dex}"
  max=$(( n > max ? n : max ))
done
cp "$OUT/dex/classes.dex" "$WORK/classes$((max + 1)).dex"
echo "   -> classes$((max + 1)).dex"

echo "== manifest: swap Application for oxhk.App"
python3 - "$WORK/AndroidManifest.xml" <<'PY'
import re, sys
p = sys.argv[1]
s = open(p).read()
m = re.search(r'<application\b[^>]*>', s)
if not m:
    sys.exit("no <application> tag")
tag = m.group(0)
orig = re.search(r'android:name="([^"]+)"', tag)
orig_cls = orig.group(1) if orig else "android.app.Application"
smali_orig = "L" + orig_cls.replace(".", "/") + ";"
if orig:
    new = tag.replace(orig.group(0), 'android:name="oxhk.App"')
else:
    new = tag[:-1] + ' android:name="oxhk.App">'
s = s.replace(tag, new, 1)
open(p, "w").write(s)
open(p + ".origapp", "w").write(smali_orig)
print("   original Application:", orig_cls)
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

echo "== apktool build"
apktool b --use-aapt2 "$WORK" -o "$OUT/unsigned.apk" >/dev/null

echo "== stuff liboxhk.so (stored, so extractNativeLibs=false still works)"
rm -rf "$OUT/libstage" && mkdir -p "$OUT/libstage/lib/arm64-v8a"
cp "$OUT/liboxhk.so" "$OUT/libstage/lib/arm64-v8a/"
( cd "$OUT/libstage" && zip -0 -X -q "$OUT/unsigned.apk" lib/arm64-v8a/liboxhk.so )

echo "== sign"
if [ ! -f "$KEYSTORE" ]; then
  keytool -genkeypair -keystore "$KEYSTORE" -alias "$ALIAS" -keyalg RSA -keysize 2048 \
          -validity 10950 -storepass "$STOREPASS" -keypass "$STOREPASS" \
          -dname "CN=oxhk" >/dev/null 2>&1
fi
NAME="$(basename "$APK_IN" .apk)"
"$BUILD_TOOLS/zipalign" -p -f 4 "$OUT/unsigned.apk" "$OUT/$NAME-aligned.apk"
"$BUILD_TOOLS/apksigner" sign --ks "$KEYSTORE" --ks-pass "pass:$STOREPASS" \
    --ks-key-alias "$ALIAS" --out "$OUT/$NAME-oxhk.apk" "$OUT/$NAME-aligned.apk"

echo
echo "base   : $OUT/$NAME-oxhk.apk"
for s in "$@"; do
  n="$(basename "$s" .apk)"
  cp "$s" "$OUT/$n-oxhk.apk"
  "$BUILD_TOOLS/apksigner" sign --ks "$KEYSTORE" --ks-pass "pass:$STOREPASS" \
      --ks-key-alias "$ALIAS" --out "$OUT/$n-oxhk.apk" "$s" >/dev/null
  echo "split  : $OUT/$n-oxhk.apk"
done

cat <<'EOT'

install (all splits in one session, old package removed first):
  adb uninstall <pkg>
  adb install-multiple -r out/*-oxhk.apk
log:
  adb logcat -s oxhk
EOT
