#!/usr/bin/env bash
# build_container.sh — builds the sandbox probe apk (stage 0 of the container plan).
#
# This one does NOT patch anything. It measures what the phone allows a container to do:
# dlopen the game's libs from /data/app, from app data, from a memfd; hidden-api reflection;
# loading the game's Application class out of its own dex. One run answers whether the
# dual-space route is viable here, before any of it gets written.
#
# usage: ./build_container.sh     (env: SDK/NDK as in build.sh)
set -euo pipefail

HERE="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$HERE/build/sandbox"
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
: "${KEYSTORE:=$HERE/build/injector/oxhk.p12}"
: "${STOREPASS:=oxhkpass}"
: "${ALIAS:=oxhk}"
[ -n "$SDK" ] && [ -n "$NDK" ] && [ -n "$BUILD_TOOLS" ] || {
  echo "need SDK and NDK (set SDK/NDK)"; exit 1; }

CC="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android$API-clang"
rm -rf "$OUT" && mkdir -p "$OUT"/{jni,javac,dex,apk,lib/arm64-v8a}

echo "== native probe (+ intercept.c + vendored dobby)"
# probe.c и intercept.c — это C, поэтому компилируем их clang'ом, а линкуем clang++: libdobby.a
# собрана под C++ (_Znwm, __cxa_*, std::logic_error), ей нужен -static-libstdc++.
DOBBY="$HERE/vendor/dobby"
CXX="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android$API-clang++"
NM="$(dirname "$CC")/llvm-nm"
"$CC" -c -fPIC -O2 -fvisibility=hidden -I "$DOBBY" \
      -o "$OUT/jni/probe.o" "$HERE/container/jni/probe.c"
"$CC" -c -fPIC -O2 -fvisibility=hidden -I "$DOBBY" \
      -o "$OUT/jni/intercept.o" "$HERE/container/jni/intercept.c"
"$CXX" -shared -Wl,-soname,libprobe.so -Wl,--no-undefined -static-libstdc++ \
      -o "$OUT/lib/arm64-v8a/libprobe.so" \
      "$OUT/jni/probe.o" "$OUT/jni/intercept.o" \
      "$DOBBY/arm64-v8a/libdobby.a" -ldl -llog -lm

# dobby обязан был прилинковаться: без него intercept молча ничего не перехватит
"$NM" -D --defined-only "$OUT/lib/arm64-v8a/libprobe.so" | grep -q nativeGuard || {
  echo "libprobe.so: JNI symbols hidden — проверь JNIEXPORT/визибилити"; exit 1; }
"$NM" "$OUT/lib/arm64-v8a/libprobe.so" | grep -q " T DobbyHook" || {
  echo "libprobe.so: DobbyHook не прилинкован"; exit 1; }
echo "   libprobe.so $(stat -c %s "$OUT/lib/arm64-v8a/libprobe.so") bytes, dobby ok"

echo "== native esp payload"
CXX="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android$API-clang++"
"$CXX" -shared -fPIC -O2 -std=c++17 -fvisibility=hidden -static-libstdc++ \
      -Wl,-soname,liboxhk.so -o "$OUT/lib/arm64-v8a/liboxhk.so" \
      "$HERE/native/oxhk.cpp" -llog -landroid

echo "== check deps"
bad=$(readelf -d "$OUT/lib/arm64-v8a/liboxhk.so" | sed -n 's/.*NEEDED.*\[\(.*\)\]/\1/p' \
      | grep -Ev '^(liblog|libandroid|libm|libdl|libc|libz)\.so$' || true)
[ -z "$bad" ] || { echo "liboxhk.so needs non-system libs: $bad"; exit 1; }

echo "== java"
javac -source 8 -target 8 -nowarn -bootclasspath "$ANDROID_JAR" -d "$OUT/javac" \
      "$HERE"/container/java/oxhk/sandbox/*.java "$HERE"/java/oxhk/Oxhk.java
"$BUILD_TOOLS/d8" --release --min-api "$MIN_API" --lib "$ANDROID_JAR" \
      --output "$OUT/dex" $(find "$OUT/javac" -name '*.class')

echo "== link apk"
"$BUILD_TOOLS/aapt2" link -o "$OUT/apk/unsigned.apk" \
      --manifest "$HERE/container/AndroidManifest.xml" \
      -I "$ANDROID_JAR" --min-sdk-version 26 --target-sdk-version 34 \
      --version-code 1 --version-name 1.0

echo "== dex + lib"
( cd "$OUT/dex" && zip -0 -X -q "$OUT/apk/unsigned.apk" classes.dex )
(cd "$OUT" && zip -0 -X -q apk/unsigned.apk \
        lib/arm64-v8a/libprobe.so lib/arm64-v8a/liboxhk.so)

echo "== align + sign"
if [ ! -f "$KEYSTORE" ]; then
  mkdir -p "$(dirname "$KEYSTORE")"
  keytool -genkeypair -keystore "$KEYSTORE" -storetype PKCS12 -alias "$ALIAS" -keyalg RSA \
          -keysize 2048 -validity 10950 -storepass "$STOREPASS" -keypass "$STOREPASS" \
          -dname "CN=oxhk" >/dev/null
  echo "generated $KEYSTORE (keep it: updates must be signed with the same key)"
fi
"$BUILD_TOOLS/zipalign" -p -f 4 "$OUT/apk/unsigned.apk" "$OUT/apk/aligned.apk"
"$BUILD_TOOLS/apksigner" sign --ks "$KEYSTORE" --ks-pass "pass:$STOREPASS" \
      --ks-key-alias "$ALIAS" --out "$OUT/oXhk-sandbox.apk" "$OUT/apk/aligned.apk"

echo
echo "apk: $OUT/oXhk-sandbox.apk  ($(du -h "$OUT/oXhk-sandbox.apk" | cut -f1))"
"$BUILD_TOOLS/apksigner" verify --print-certs "$OUT/oXhk-sandbox.apk" | head -3
