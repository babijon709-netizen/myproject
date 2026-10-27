# oxide_int — internal (non-root) tool for Oxide / HyperHug, build 11978

Repack of the game APK with `liboxhk.so` inside the process. No root, no external app,
no `SYSTEM_ALERT_WINDOW` — the overlay is a view added to the game's own window.

## What the build looks like

From `dump11978.7z` (`libil2cpp.so`, 235 MB):

* code lives in a PT_LOAD section called **`il2cpp`** instead of `.text` (0x5D5E498, +0x7DFD440)
* `.dynsym` has 2427 defined symbols and **zero `il2cpp_*`** — no `il2cpp_domain_get`,
  `il2cpp_class_from_name`, nothing to `dlsym`. Export-stripped + section-renamed build.
* `.rela.dyn` is 0x209FD00 byte-aligned, `.init_array` has 22 entries, `.comment` = LLD 18.0.3

So the whole tool is **base + RVA**, not API-based:

| what | where it came from |
|---|---|
| `il2cpp_base` | `/proc/self/maps`, lowest mapping of `libil2cpp.so` |
| managed method address | `script.json` → `Address` (== `dump.cs` VA == base + RVA) |
| field offsets | `dump.cs` (`obj + offset`, il2cpp object header is 0x10) |
| `Il2CppClass*` | `script.json` → `TypeInfoPointers` metadata-usage slot, `*(void**)(base + slot)` |
| `static_fields` | `Il2CppClass + 0xB8`, then the static field's offset from `dump.cs` |

Managed calls used (direct AOT entry points, `MethodInfo = nullptr`):
`Transform::get_position` 0xD4CFB20 · `Camera::get_main` 0xD43AB4C ·
`Camera::get_worldToCameraMatrix` 0xD43929C · `Camera::get_projectionMatrix` 0xD43944C ·
`Camera::get_pixelWidth/Height` 0xD4387D4 / 0xD438888 ·
`Animator::GetBoneTransform` 0xD40FE38 · `NetworkBehaviour::get_isLocalPlayer` 0xC8D8774

Game side:

| symbol | offset |
|---|---|
| `PlayerManager.clientPlayerList` (static) | static_fields + 0x10 |
| `PlayerManager.vitals` | 0xC8 |
| `PlayerManager.team` (kai) → `kai.team` (Cu) | 0x120 → 0x98 |
| `PlayerManager.nicklabel` (Di) → `nickname` (Text) → `m_Text` | 0x130 → 0x38 → 0xE0 |
| `PlayerManager.characterModel` / `.animator` | 0x150 / 0x1B0 |
| `GenericVitals.m_MaxHealth` / current health | 0x88 / 0xB8 |

No inline hooks and no patching of the game's own libs. Java posts a per-frame callback on
the Android UI thread; Unity runs its script loop on that same thread, so all managed calls
land where the game expects them. Touches for aim are injected as `MotionEvent`s into the
game's own view — nothing is written into game memory.


## the APK this targets

`com.catsbit.oxidesurvivalisland`, versionName 1.16.11978, versionCode 11978, minSdk 25,
target 36, `extractNativeLibs=true`, split install (`base.apk` + `split_config.arm64_v8a.apk`).

* base.apk: 7 dex (`classes.dex` .. `classes7.dex`), **no** `lib/` at all, 1304 entries
* split: 29 arm64 libs, `libil2cpp.so` is **byte-identical to the dump**
  (md5 `e3c232f144a398d2c60c643aa2663b65`, 245 567 024 B) — every RVA in `oxhk.cpp` applies
* `assets/bin/Data/Managed/Metadata/global-metadata.dat` is a 4 KiB stub, the real thing is
  decrypted at runtime by the loader; `assets/bin/Data/c1a4ee663e7b7702978e12f1c5b5f948`
  (9.4 MB) is the encrypted blob

protection chain (what a repack runs into):

| piece | what it is |
|---|---|
| `libloader.so` (15 MB) | packed loader, only `JNI_OnLoad` exported, sections `.gcc_r` `.dyncl` `.ctx` `.eh_trace` `.adi`, .text entropy 7.16 → encrypted/VM-ed |
| `libpglarmor.so` (68 KB) | one JNI export `Java_dtGga4_..._bc`, second half of the same armor |
| app class `dtGga4.sNBtq9.vRXcl4.pm7o47` | `<clinit>` → `qCXAY9.uIbdQ6.jp8Aw0.ejRrR3.hsOtW6()` → `System.loadLibrary("loader")` |
| launcher `dtGga4.sNBtq9.vRXcl4.jPsie4` | extends `com.google.firebase.MessagingUnityPlayerActivity`, overrides `updateUnityCommandLineArguments` with obfuscated string compares |
| `libapminsighta/b.so` | crash/anti-debug sidecar (ptrace, `/proc/*/maps` dumps) |
| `qCXAY9.*` (classes7.dex) | ad-attribution SDK: enumerates installed apps, installer package, dumps PackageInfo to JSON — not the tamper check |

Nothing in Java verifies the signature. If anything enforces it, it is inside `libloader` /
`libpglarmor`, i.e. native and packed. That is exactly what `--probe` answers.

## stage 0 — probe before anything else

```bash
./tools/build.sh --probe base.apk split_config.arm64_v8a.apk
adb uninstall com.catsbit.oxidesurvivalisland
adb install-multiple -r build/base-oxhk.apk build/split_config.arm64_v8a-oxhk.apk
```

same APK, same libs, only re-signed. Boots to the lobby → the armor does not police the
signature locally, go straight to the full build. Dies silently / black screen / closes on
splash → the check is in `libloader`, and that is a separate reverse job (`JNI_OnLoad`,
`.gcc_r`/`.dyncl` tables) before any of this is usable.

## build

```bash
export NDK=/path/android-ndk-r27
export SDK=/path/android-sdk
./tools/build.sh base.apk config.arm64_v8a.apk other-split.apk
```

artifacts only (no repack): `./tools/build.sh --no-apk`
needs `apktool`, `java`, `zip` on PATH; NDK r26+, any recent build-tools.

The script:
1. compiles `native/oxhk.cpp` → `liboxhk.so` (arm64 only, this build is arm64-only)
2. compiles `java/oxhk/Oxhk.java` → `classes.dex` via `javac` + `d8`
3. decodes the APK, adds the dex as `classes<N+1>.dex`
4. generates `oxhk/App.smali` extending the game's current `Application`, points
   `android:name` at it, so `System.loadLibrary("oxhk")` happens before Unity starts
5. stores `liboxhk.so` uncompressed (`zip -0`) so `extractNativeLibs=false` still works
6. zipalign + apksigner (keystore auto-generated at `build/oxhk.jks`, pass `oxhkpass`)
7. puts `liboxhk.so` into the **split's** `lib/arm64-v8a` (stored, `zip -0`) and re-signs the
   split with the same key — that's where every other lib of this game already lives

## install

```bash
adb uninstall <pkg>
adb install-multiple -r build/*-oxhk.apk
adb logcat -s oxhk
```

## use

`oxhk` badge, top-left → tap = menu (drag it by the title bar).
Tabs: **ESP** (esp / boxes / clean / teamcheck) · **AIM** (aim / fov / smooth / gain / touchms /
cooldown) · **CFG** (save / load / reset / close). Config lives in
`/data/data/<pkg>/files/oxhk.json`.

Boxes are drawn from `Animator.GetBoneTransform(Head)` → `Hips`, so the box follows the rig.
`clean` = head dot + distance only. Team colour comes from `kai.team` pointer equality.

## known gaps / FILL

* `vitals_health = 0xB8` (`Ksd`, float right after `m_DamageMultipliers`) — the property
  `PlayerVitals.XTp/XTJ/XTa` accessors are not named `get_*` in this dump, so the field was
  picked by layout. Verify once: read `vitals+0xB8` and compare with the HUD health number,
  or call `kAr<float>::Lbu` (RVA 0xBD36858) if health turns out to be a reactive value.
* aim = injected single-pointer swipe. While He is holding the fire button with the same
  finger the game sees one pointer, so aim and fire can't share a finger — that's a real
  limitation of touch synthesis inside the same view, not a bug in the offsets.
* `gain` (touch px per screen px) is per-game sensitivity. Default 1.0 is "aim moves as far
  as the finger would"; tune it in AIM tab until a 100 px delta lands on the head.
* no visibility raycast — ESP draws everyone in `clientPlayerList`, through walls.

## risk

`Google.Play.Integrity.dll` is in the asset list and the repack changes the signature, so any
server-side signature/Integrity verdict on login is fatal for the account and no client patch
touches it. Test on a throwaway account on an offline/event server, not on the main one.
Google/Facebook sign-in binds the app signature too — expect it to fail or to require the
signature hash re-patched in the client.
