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
land where the game expects them. Only the menu consumes touches; everything else goes to the
game. Nothing is ever written into game memory.


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


## what the two "armor" libs actually are

`libpglarmor.so` — **not** anti-cheat. Its only JNI export is
`Java_dtGga4_xpqSb3_qbL4k4_dnIeo4_s3o1l7_eaokX6_bc` and the java side that calls it is
`...dnIeo4.s3o1l7.eaokX6` with `cypher4Encrypt/cypher4Decrypt`, log tag `pangle-encrypt`,
`ARMOR`, and Pangle payload classes around it. It's the Pangle/TikTok **ads** crypto, keys and
all. Ignore it.

`libloader.so` — the only unknown left. Encrypted `.text` (entropy 7.16), `.gcc_r` `.dyncl`
`.ctx` `.eh_trace` `.adi` sections, one export (`JNI_OnLoad`). Its import table is the map of
what it can see:

| import | meaning |
|---|---|
| `android_dlopen_ext`, `dl_iterate_phdr`, `dladdr`, `dlsym`, `dlerror` | loads/inspects libs itself (this is who maps libil2cpp + libunity) |
| `inflate/inflateInit2_` | unpacks the encrypted `assets/bin/Data/c1a4ee66...` blob → global-metadata |
| `nftw`, `opendir/readdir/stat/lstat`, `access` | walks directories — apk contents, lib dir, or /proc |
| `getxattr/fgetxattr/listxattr/flistxattr` | reads file attributes on the apk/lib files |
| `fork`, `kill`, `getppid`, `prctl`, `syscall`, `getauxval` | classic anti-debug / anti-hook moves |
| `__system_property_get/find`, `uname`, `getenv` | device + process fingerprinting |
| `socket/connect/getaddrinfo`, `mmap/mprotect/mremap`, `sigaction`, `epoll*` | full runtime inside the lib |

Whether *signature* enforcement lives in there is still an open question. Nothing in java asks
for it: `qCXAY9.*` (classes7.dex) enumerates installed packages, installer package and dumps
`PackageInfo` to JSON — that is ad attribution, and `dtGga4...jPsie4.updateUnityCommandLineArguments`
just rewrites launch args.

## finding out instead of guessing

```bash
# 1. baseline on the store build — what the game asks of PackageManager when nothing is patched
frida -U -f com.catsbit.oxidesurvivalisland -l tools/probe_sig.js --no-pause

# 2. same script on the repack
frida -U -f com.catsbit.oxidesurvivalisland -l tools/probe_sig.js --no-pause
```

`tools/probe_sig.js` logs every `getPackageInfo` (marks `GET_SIGNATURES` = 0x40), every
`MessageDigest.digest`, every `System.loadLibrary`/`load`, and every native `dlopen` with its
caller. Diff the two runs:

* no `getPackageInfo` with the signature flag in either run → nothing in the process cares
  about certs, the probe build will boot
* the store build asks, the repack throws / the call disappears → something native is parsing
  the apk itself, and the check sits in `libloader`


## libloader.so, actually opened

static pass over `split_config.arm64_v8a/lib/arm64-v8a/libloader.so` (15 396 440 B, no exec):

**it is a packer, not "just a loader".** Everything prose-like is encrypted:

| region | what | state |
|---|---|---|
| `.text` 0x8ce00 | 12 MB of code | mixed: partly plaintext (disassembles cleanly), partly not |
| `.rodata` 0xc9c000 (0xa0056) | strings/consts | encrypted, no ASCII at all |
| `.gcc_abi` 0xdf1de5 (0x15eb1) | ? | encrypted, 0.4 % zero bytes |
| `.gnu.plt` 0xdced90 (0x23055) | — | **100 % zeros**, 143 KB of reserved space (decrypt landing area) |
| `.dyncl` / `.ctx` / `.eh_trace` / `.adi` / `.gcc_r` | tables | binary, encrypted |

Consequences: keyword search for `frida`/`xposed`/`maps`/`TracerPid` finds nothing — by design.
`strings` on this lib is a dead end, and any "it contains X" claim from strings is false.

Facts worth knowing:

* `.hash` says 224 dynsyms. The section-based symbol table beyond that is garbage — that is why
  `readelf` prints nonsense names (`nflateInit2_`, `teInit2_`). Read the dynamic segment
  (`DT_SYMTAB`/`DT_HASH`), not the section table.
* `.init_array` (5 slots, all `R_AARCH64_RELATIVE`) → constructors at `0x5bd248`, `0xc931fc`,
  `0xc936cc`, `0x5bd270`, `0xb0c72c`. `0x5bd248` is `getpagesize()` + tail call;
  `0x5bd270` tail-calls `sub_746554(.bss 0xf1941c, func 0x5bd4c8)` — a callback registration.
* **pointer mangling**: `adrp x27, #0xccb000 ; eor x27, x27, x18 ; eor x27, x27, x18`
  — `x18` is `TPIDR_EL0`, so every table pointer is XORed with the thread pointer. Static xrefs
  into `.rodata` do not exist; they're computed at runtime.
* JNI_OnLoad (`0x89b6f0`, 3680 B) is plaintext, 0x500-byte frame, calls `sub_197ea4` and the
  thunk at `0x8cdd0` before anything else.
* **anomaly**: `JNI_OnLoad` sits at `0x89b6f0`, i.e. inside `PT_LOAD #1`, which the program
  headers mark `R` — not `R E`. Valid AArch64 code in a non-executable segment means the file
  as shipped in the APK is not the image the process runs (something re-maps / rebuilds it at
  load). Compare file vs memory at runtime if you want to know which.

**its own syscall layer** — 60 hand-written `mov x8, #N ; svc #0 ; ret` thunks, verified:

```
0x6ae2d8..0x6ae834 (main table) + 0x64fab4, 0x667694, 0x6676a4
ptrace 117 · kill 129 · tgkill 131 · process_vm_writev 270 · execve 221 · getppid 173
wait4 260 · prctl 167 · socket/bind/listen/accept/sendmsg/recvmsg 198..212
mmap/mprotect/mremap 222/226/216 · getrandom 278 · futex 98 · uname 160
```

That is the answer to "does it detect injects": yes, the toolkit is there — direct `svc` for
`ptrace`/`kill`/`process_vm_writev`/`execve` (bypasses libc, so libc hooks see nothing), plus
`dl_iterate_phdr` (13 call sites) to walk the module list, plus the xattr family
(`getxattr`/`fgetxattr`/`listxattr`/`flistxattr`, 12 sites) and `stat`/`lstat`/`access`/`fopen`
(25/4/6/14 sites) to look at files, plus `fork`/`getppid`/`prctl`.

What it *flags* and when is still unknown, because every string it compares against is
decrypted at runtime. Static analysis stops here.

## watching it instead of guessing

```bash
frida -U -f com.catsbit.oxidesurvivalisland -l tools/probe_loader.js --no-pause
```

`tools/probe_loader.js` resolves `libloader.so`, attaches to all 60 syscall thunks by RVA and
logs every direct syscall with args + caller (works even though the lib bypasses libc), and
flags `PTRACE` / `KILL` / `TGKILL` / `VM_WRITEV` / `EXECVE` / `GETPPID` / `WAIT4` / `PRCTL`
with a backtrace. Two runs: clean install, then the repack. If the repack produces `PTRACE`,
`KILL` or module-list walks that don't exist on the clean build, that's the detector firing —
and the backtrace points straight at the function to patch.

Defensive knobs already in the code:

* `Oxhk.ATTACH_DELAY_MS` — keep our lib out of the process until the loader's boot scans are
  done (0 = immediate; 2500–4000 is the interesting range).
* no ptrace, no inline hooks, no writes into game libs → the `ptrace`/`prctl`/`mprotect` checks
  in that table have nothing to see. Our only exposure is the module list
  (`dl_iterate_phdr`) and the files on disk (apk content/xattrs).
* name and path of our lib are the remaining fingerprint. `liboxhk.so` is a bad name; anything
  that isn't in the game's own lib set is suspicious to a whitelist check.

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
Build is **ESP only** right now (no aim, no touch injection at all). Menu: `oxhk` badge top-left
opens it, drag by the title bar, rows are

`esp` · `boxes` · `names` · `health` · `distance` · `team check` · `range` · `save` · `reload` · `close`

Config lives in `/data/data/com.catsbit.oxidesurvivalisland/files/oxhk.json`.

Boxes come from `Animator.GetBoneTransform(Head)` and `(Hips)`, so they follow the rig instead of
being a fixed-size square. Team colour is `kai.team` pointer equality — team members draw blue
and are never counted in the `Ne` counter in the title bar.


## CI

`.github/workflows/build.yml`, triggered on **every push** (and on `v*` tags, plus manual runs):

* `payload` — sets up JDK 17, takes the runner's preinstalled SDK/NDK, runs
  `oxide_int/tools/build.sh --no-apk`, uploads `liboxhk.so` + `classes.dex` as the
  `oxhk-payload` artifact. This is the green build you get from any push.
* `repack` — only when the game apks are reachable. Pulls latest apktool, downloads both apks,
  sanity-checks that the split really contains `lib/arm64-v8a/libil2cpp.so`, then runs the full
  `build.sh base.apk split.apk` and uploads `oxhk-apk`. On a `v*` tag it also attaches the apks
  to the GitHub release.

Where the apks come from, in order: the `base_apk_url` / `split_apk_url` inputs on a manual run,
or a repo secret `GAME_APKS_URL` pointing at a two-line manifest (line 1 = base.apk url,
line 2 = split url). Nothing game-sized is committed to the repo.

```bash
gh workflow run build -f base_apk_url=https://... -f split_apk_url=https://...
gh run watch
```

`probe: true` on a manual run builds the re-signed-but-clean variant — the signature test from
the section above, straight from CI.


## injector APK (patch on the phone, no PC)

`tools/build_injector.sh` → `build/injector/oXhk-injector.apk` (also built by CI on every push,
artifact `oxhk-injector-apk`). One screen, one button:

1. reads the installed game: `applicationInfo.sourceDir` + `splitSourceDirs`
   (`/data/app/.../base.apk` is world-readable, no root needed). This game ships **four**
   splits — `split_config.arm64_v8a`, `split_config.xxhdpi`, `split_UnityDataAssetPack`
   (92 MB) — so the arm64 one is picked by content (`lib/arm64-v8a/libil2cpp.so` inside),
   never by position.
2. **base.apk**: rewrites the manifest's `<application android:name>` string in place — the
   binary XML string pool entry for `dtGga4.sNBtq9.vRXcl4.pm7o47` becomes `oxhk.App` (shorter,
   so no offsets move, tail is zero-filled). Adds the payload dex as `classes<N+1>.dex`.
3. **split**: adds `lib/arm64-v8a/liboxhk.so` (STORED)
4. drops old `META-INF/*.SF|RSA|DSA|EC`, signs both with **apksig** (the lib inside
   `build-tools/*/lib/apksigner.jar`, v1+v2+v3) using a PKCS12 key baked into the injector's
   assets at build time
5. hands base **plus every split** to `PackageInstaller` in one session (`MODE_FULL_INSTALL`) —
   a split app will not install from a partial set. Untouched splits are streamed through
   (64 KB buffer, original STORED/DEFLATED method kept) so the 92 MB asset pack never enters the
   heap, then re-signed — one certificate for the whole app, no way around it.

`oxhk.App` is compiled against a stub of the game's Application class
(`injector/stub/java/dtGga4/.../pm7o47.java`); the stub is kept out of the payload dex, so at
runtime the superclass resolves to the real one and the packer's `<clinit>`
(`System.loadLibrary("loader")`) still runs. `super.onCreate()` first, then `Oxhk.install()`.

Install:

```bash
adb install -r oxide_int/build/injector/oXhk-injector.apk
# on the phone: allow "install unknown apps", open it, tap patch + install
# it will ask to uninstall the store build first — signature changed
```

The keystore lives at `build/injector/oxhk.p12` (pass `oxhkpass`, alias `oxhk`) — **keep it**,
every later patch must be signed with the same key to install over the previous one. The
certificate SHA-256 is printed on the injector's screen (that's what the game/server sees
instead of the store signature).

CI additions: the `injector` job runs `bash -x tools/build_injector.sh`, uploads
`oxhk-injector-apk`, and on failure commits the log back as `ci-last-failure.log` (same trick as
the payload job). Two pitfalls already fixed there: `javac` against `android.jar` has no
`LambdaMetafactory` (no lambdas anywhere in this tree), and `apksig` must be dexed together with
the injector classes.

## known gaps / FILL

* `vitals_health = 0xB8` (`Ksd`, float right after `m_DamageMultipliers`) — the property
  `PlayerVitals.XTp/XTJ/XTa` accessors are not named `get_*` in this dump, so the field was
  picked by layout. Verify once: read `vitals+0xB8` and compare with the HUD health number,
  or call `kAr<float>::Lbu` (RVA 0xBD36858) if health turns out to be a reactive value.
* no visibility raycast — ESP draws everyone in `clientPlayerList`, through walls.
* names come from `Di.nickname` → `Text.m_Text`; if the game swaps the label component the row
  goes blank instead of crashing (checked every 300 ms, `null` entries are skipped).

## risk

`Google.Play.Integrity.dll` is in the asset list and the repack changes the signature, so any
server-side signature/Integrity verdict on login is fatal for the account and no client patch
touches it. Test on a throwaway account on an offline/event server, not on the main one.
Google/Facebook sign-in binds the app signature too — expect it to fail or to require the
signature hash re-patched in the client.
