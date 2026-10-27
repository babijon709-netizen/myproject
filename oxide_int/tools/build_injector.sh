#!/usr/bin/env bash
# build_injector.sh — builds oXhk-injector.apk: the on-device patcher GUI.
#
# What goes in:
#   liboxhk.so                     (native/oxhk.cpp, NDK)
#   payload/classes.dex            oxhk.Oxhk + oxhk.App (App compiled against the game's
#                                  Application class stub, stub excluded from the dex)
#   assets/oxhk.p12                signing key for the patched game apks
#   classes.dex                    the injector itself + apksig (taken from build-tools)
#
# usage: ./build_injector.sh        (env: SDK/NDK as in build.sh)
set -euo pipefail

HERE="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$HERE/build/injector"
: "${SDK:=${ANDROID_SDK_ROOT:-${ANDROID_HOME:-}}}"
if [ -z "${NDK:-}" ]; then
  for cand in "${ANDROID_NDK_HOME:-}" "$SDK/ndk/"*; do
    [ -n "$cand" ] && [ -d "$cand" ] && NDK="$cand"
  done
fi
: "${API:=26}"
: "${MIN_API:=26}"
: "${BUILD_TOOLS:=$(ls -d "$SDK/build-tools"/* 2>/dev/null | sort -V | tail -1)}"
: "${ANDROID_JAR:=$(ls -d "$SDK/platforms"/android-*/android.jar 2>/dev/null | sort -V | tail -1)}"
: "${APKSIG_JAR:=$(ls "$BUILD_TOOLS"/lib/apksigner.jar 2>/dev/null | head -1)}"
: "${KEYSTORE:=$OUT/oxhk.p12}"
: "${STOREPASS:=oxhkpass}"
: "${ALIAS:=oxhk}"
[ -n "$SDK" ] && [ -n "$NDK" ] && [ -n "$APKSIG_JAR" ] || {
  echo "need SDK, NDK and $BUILD_TOOLS/lib/apksigner.jar (set SDK/NDK/APKSIG_JAR)"; exit 1; }

CC="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android$API-clang++"
rm -rf "$OUT" && mkdir -p "$OUT"/{javac-payload,stub,dex,assets,payload,apk}

echo "== native"
"$CC" -shared -fPIC -O2 -std=c++17 -fvisibility=hidden -Wl,-soname,liboxhk.so \
      -o "$OUT/payload/liboxhk.so" "$HERE/native/oxhk.cpp" -llog -landroid

echo "== payload dex (oxhk + oxhk.App)"
javac -source 8 -target 8 -nowarn -bootclasspath "$ANDROID_JAR" -d "$OUT/stub" \
      "$HERE"/injector/stub/java/dtGga4/sNBtq9/vRXcl4/pm7o47.java
mkdir -p "$OUT/payload-classes"
javac -source 8 -target 8 -nowarn -bootclasspath "$ANDROID_JAR" -classpath "$OUT/stub" \
      -d "$OUT/payload-classes" "$HERE"/java/oxhk/Oxhk.java "$HERE"/injector/payload/java/oxhk/App.java
"$BUILD_TOOLS/d8" --release --min-api "$MIN_API" --lib "$ANDROID_JAR" \
      --output "$OUT/payload" $(find "$OUT/payload-classes" -name '*.class')
mv "$OUT/payload/classes.dex" "$OUT/assets/classes.dex" 2>/dev/null || true
cp "$OUT/payload/liboxhk.so" "$OUT/assets/liboxhk.so"

echo "== signing key"
rm -f "$KEYSTORE"
keytool -genkeypair -keystore "$KEYSTORE" -storetype PKCS12 -alias "$ALIAS" -keyalg RSA \
        -keysize 2048 -validity 10950 -storepass "$STOREPASS" -keypass "$STOREPASS" \
        -dname "CN=oxhk injector" >/dev/null
cp "$KEYSTORE" "$OUT/assets/oxhk.p12"

echo "== injector dex"
javac -source 8 -target 8 -nowarn -bootclasspath "$ANDROID_JAR" -classpath "$APKSIG_JAR" \
      -d "$OUT/javac-payload" "$HERE"/injector/java/oxhk/injector/*.java
"$BUILD_TOOLS/d8" --release --min-api "$MIN_API" --lib "$ANDROID_JAR" \
      --output "$OUT/dex" "$APKSIG_JAR" $(find "$OUT/javac-payload" -name '*.class')

echo "== link apk"
"$BUILD_TOOLS/aapt2" link -o "$OUT/apk/unsigned.apk" \
      --manifest "$HERE/injector/AndroidManifest.xml" \
      -I "$ANDROID_JAR" --min-sdk-version 26 --target-sdk-version 34 --version-code 1 \
      --version-name 1.0

echo "== stuff dex + assets"
( cd "$OUT/dex" && zip -0 -X -q "$OUT/apk/unsigned.apk" classes.dex )
( cd "$OUT/assets" && zip -0 -X -q "$OUT/apk/unsigned.apk" classes.dex liboxhk.so oXhk.p12 )
python3 - "$OUT/apk/unsigned.apk" "$OUT/assets" <<'PY'
import sys, zipfile
apk, assets = sys.argv[1], sys.argv[2]
mapping = {
    'classes.dex': 'assets/payload/classes.dex',
    'liboxhk.so':  'assets/payload/liboxhk.so',
    'oXhk.p12':    'assets/oxhk.p12',
}
with zipfile.ZipFile(apk, 'a', zipfile.ZIP_STORED) as z:
    for src, dst in mapping.items():
        z.write(assets + '/' + src, dst)
print('assets:', ', '.join(mapping.values()))
PY

echo "== align + sign"
"$BUILD_TOOLS/zipalign" -p -f 4 "$OUT/apk/unsigned.apk" "$OUT/apk/aligned.apk"
"$BUILD_TOOLS/apksigner" sign --ks "$KEYSTORE" --ks-pass "pass:$STOREPASS" --ks-key-alias "$ALIAS" \
      --out "$OUT/oXhk-injector.apk" "$OUT/apk/aligned.apk"

echo
echo "apk: $OUT/oXhk-injector.apk"
"$BUILD_TOOLS/apksigner" verify --print-certs "$OUT/oXhk-injector.apk" | head -4
echo
echo "install:  adb install -r $OUT/oXhk-injector.apk"
echo "(keep $KEYSTORE — every future patch must be signed with it to update in place)"
