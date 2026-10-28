// liboxhk.so — in-process ESP for Oxide / HyperHug, build 11978. No root, no hooks.
//
// Offsets come from dump11978.7z (Il2CppDumper): dump.cs gives field offsets, script.json gives
// method RVAs and the TypeInfoPointers metadata-usage slots. libil2cpp.so in the split is the
// same binary as in the dump (md5 e3c232f144a398d2c60c643aa2663b65), so every RVA applies.
//
// This build's libil2cpp.so exports no il2cpp_* symbols at all (its code even lives in a PT_LOAD
// section named "il2cpp" instead of .text), so there is no API to call: everything is
// il2cpp_base + RVA, fields are obj + offset, and the handful of managed functions we need are
// entered directly with MethodInfo = nullptr. No inline hooks, nothing written into game code.
//
// Threading: Java drives nativeTick() from a per-frame callback on the Android UI thread, which
// is the same thread Unity runs its script loop on.

#include <jni.h>
#include <android/log.h>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <ctime>
#include <string>
#include <vector>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <ucontext.h>
#include <sys/stat.h>

#define LOG_TAG "oxhk"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// ── libil2cpp.so RVAs (dump.cs "VA" == base + RVA) ──────────────────────────────────────
namespace il2dump {
constexpr uint64_t Camera_get_main                = 0xD43AB4C;  // static Camera FindMainCamera()
constexpr uint64_t Camera_worldToCameraMatrix     = 0xD43929C;
constexpr uint64_t Camera_projectionMatrix        = 0xD43944C;
constexpr uint64_t Camera_pixelWidth              = 0xD4387D4;
constexpr uint64_t Camera_pixelHeight             = 0xD438888;
constexpr uint64_t Transform_get_position         = 0xD4CFB20;
constexpr uint64_t Animator_GetBoneTransform      = 0xD40FE38;  // Transform GetBoneTransform(HumanBodyBones)
constexpr uint64_t GameObject_get_transform       = 0xD4BB438;
constexpr uint64_t NetworkBehaviour_isLocalPlayer = 0xC8D8774;

// metadata-usage slots holding Il2CppClass* (script.json -> TypeInfoPointers)
constexpr uint64_t TI_PlayerManager = 0xE66DEE8;  // Oxide.PlayerManager
constexpr uint64_t TI_Camera        = 0xE666348;  // UnityEngine.Camera
constexpr uint64_t TI_Animator      = 0xE6651F0;  // UnityEngine.Animator
}  // namespace il2dump

namespace il2 = il2dump;

// ── field offsets (dump.cs) ─────────────────────────────────────────────────────────────
namespace off {
constexpr size_t pm_vitals         = 0xC8;   // Oxide.PlayerVitals
constexpr size_t pm_team           = 0x120;  // kai
constexpr size_t pm_nicklabel      = 0x130;  // Di
constexpr size_t pm_characterModel = 0x150;  // GameObject
constexpr size_t pm_animator       = 0x1B0;  // Animator
constexpr size_t pm_static_client  = 0x10;   // static List<PlayerManager> clientPlayerList
constexpr size_t vitals_maxHealth  = 0x88;   // Oxide.GenericVitals.m_MaxHealth
constexpr size_t vitals_health     = 0xB8;   // current health, see FILL in README
constexpr size_t kai_team          = 0x98;   // kai.Cu
constexpr size_t di_nickname       = 0x38;   // Di.nickname (UnityEngine.UI.Text)
constexpr size_t text_m_Text       = 0xE0;   // Text.m_Text (System.String)
}  // namespace off

// ── il2cpp layout ───────────────────────────────────────────────────────────────────────
struct Il2CppString { void* klass; void* monitor; int32_t length; uint16_t chars[1]; };
struct Il2CppArray  { void* klass; void* monitor; void* bounds; uintptr_t max_length; char data[1]; };
struct Il2CppList   { void* klass; void* monitor; void* items; int32_t size; int32_t version; };

struct Il2CppClassLayout {
  void* image;
  void* gc_desc;
  const char* name;
  const char* namespaze;
  char pad[0xB8 - 0x20];
  void* static_fields;
};
static_assert(offsetof(Il2CppClassLayout, name) == 0x10, "Il2CppClass::name");
static_assert(offsetof(Il2CppClassLayout, static_fields) == 0xB8, "Il2CppClass::static_fields");

struct Vec3 { float x, y, z; };
struct Mat4 { float d[16]; };  // Unity column-major: M[row][col] == d[col * 4 + row]

// ── config, mirrored from the Java menu ─────────────────────────────────────────────────
enum : int {
  kFlagEsp  = 1 << 0,
  kFlagBox  = 1 << 1,
  kFlagName = 1 << 2,
  kFlagHp   = 1 << 3,
  kFlagDist = 1 << 4,
  kFlagTeam = 1 << 5,  // team check: draw team members blue, never counted as "enemy"
  kFlagMenu = 1 << 6,
};

struct Config {
  int  flags   = kFlagEsp | kFlagBox | kFlagHp | kFlagDist | kFlagTeam | kFlagMenu;
  int  headBone = 10;  // HumanBodyBones.Head
  float maxDist = 0.f; // 0 = no limit
};
static Config g_cfg;

// ── state ───────────────────────────────────────────────────────────────────────────────
static uintptr_t g_base = 0;
static void* g_klass_pm = nullptr;
static void* g_klass_camera = nullptr;
static void* g_klass_animator = nullptr;
static void* g_camera = nullptr;
static int   g_screenW = 1080, g_screenH = 2400;
static bool  g_ready = false;
static std::vector<std::string> g_names;

// ── helpers ─────────────────────────────────────────────────────────────────────────────
static inline bool inproc(const void* p) { return (uintptr_t)p > 0x10000; }

static uintptr_t find_module_base(const char* name) {
  FILE* f = fopen("/proc/self/maps", "r");
  if (!f) return 0;
  char line[512];
  uintptr_t best = 0;
  while (fgets(line, sizeof(line), f)) {
    if (!strstr(line, name)) continue;
    uintptr_t start = strtoull(line, nullptr, 16);
    if (!best || start < best) best = start;
  }
  fclose(f);
  return best;
}

template <typename T>
static inline T rd(uintptr_t addr) { return *reinterpret_cast<T*>(addr); }

static inline bool klass_name_is(void* klass, const char* want) {
  if (!inproc(klass)) return false;
  const char* n = reinterpret_cast<Il2CppClassLayout*>(klass)->name;
  if (!inproc(n)) return false;
  return strcmp(n, want) == 0;
}

// metadata-usage slot -> Il2CppClass*; the slot is filled lazily by the runtime, so retry
static void* klass_from_slot(uint64_t slotRva, const char* name, const char* ns) {
  if (!g_base) return nullptr;
  void* k = rd<void*>(g_base + slotRva);
  if (!inproc(k)) return nullptr;
  auto* kc = reinterpret_cast<Il2CppClassLayout*>(k);
  if (!inproc(kc->name) || !inproc(kc->namespaze)) return nullptr;
  if (strcmp(kc->name, name) != 0) return nullptr;
  if (ns && strcmp(kc->namespaze, ns) != 0) return nullptr;
  return k;
}

static void* static_fields(void* klass) {
  if (!inproc(klass)) return nullptr;
  void* sf = reinterpret_cast<Il2CppClassLayout*>(klass)->static_fields;
  return inproc(sf) ? sf : nullptr;
}

static std::string read_string(void* str) {
  if (!inproc(str)) return {};
  auto* s = reinterpret_cast<Il2CppString*>(str);
  if (s->length <= 0 || s->length > 48) return {};
  std::string out;
  out.reserve(s->length);
  for (int i = 0; i < s->length; ++i) {
    uint16_t c = s->chars[i];
    out.push_back(c < 0x80 ? static_cast<char>(c) : '?');
  }
  return out;
}

// ── managed call shims (AOT entry points, MethodInfo = nullptr) ─────────────────────────
static Vec3 transform_position(void* transform) {
  if (!inproc(transform)) return {};
  auto fn = reinterpret_cast<Vec3 (*)(void*, void*)>(g_base + il2::Transform_get_position);
  return fn(transform, nullptr);
}

static void* animator_bone(void* animator, int bone) {
  if (!inproc(animator)) return nullptr;
  auto fn = reinterpret_cast<void* (*)(void*, int, void*)>(g_base + il2::Animator_GetBoneTransform);
  void* t = fn(animator, bone, nullptr);
  return inproc(t) ? t : nullptr;
}

static void* gameobject_transform(void* go) {
  if (!inproc(go)) return nullptr;
  auto fn = reinterpret_cast<void* (*)(void*, void*)>(g_base + il2::GameObject_get_transform);
  void* t = fn(go, nullptr);
  return inproc(t) ? t : nullptr;
}

static bool is_local_player(void* playerManager) {
  auto fn = reinterpret_cast<bool (*)(void*, void*)>(g_base + il2::NetworkBehaviour_isLocalPlayer);
  return fn(playerManager, nullptr);
}

static void* main_camera() {
  auto fn = reinterpret_cast<void* (*)(void*)>(g_base + il2::Camera_get_main);
  void* c = fn(nullptr);
  if (!inproc(c)) return nullptr;
  void* klass = rd<void*>(reinterpret_cast<uintptr_t>(c));
  if (g_klass_camera && klass != g_klass_camera && !klass_name_is(klass, "Camera")) return nullptr;
  return c;
}

static Mat4 camera_matrix(uint64_t rva, void* cam) {
  Mat4 m{};
  auto fn = reinterpret_cast<void (*)(Mat4*, void*, void*)>(g_base + rva);
  fn(&m, cam, nullptr);
  return m;
}

static int camera_dim(uint64_t rva, void* cam, int fallback) {
  auto fn = reinterpret_cast<int (*)(void*, void*)>(g_base + rva);
  int v = fn(cam, nullptr);
  return v > 0 ? v : fallback;
}

// ── math ────────────────────────────────────────────────────────────────────────────────
static Mat4 mat_mul(const Mat4& a, const Mat4& b) {  // column-major a*b
  Mat4 r{};
  for (int c = 0; c < 4; ++c)
    for (int row = 0; row < 4; ++row) {
      float s = 0.f;
      for (int k = 0; k < 4; ++k) s += a.d[k * 4 + row] * b.d[c * 4 + k];
      r.d[c * 4 + row] = s;
    }
  return r;
}

struct ScreenPt { float x, y; bool valid; };

// Unity GL projection: clip = (P*V)*p, NDC y up, screen y bottom-up. The overlay is a normal
// Android view (y down), so y is flipped here.
static ScreenPt world_to_screen(const Mat4& vp, const Vec3& p, int w, int h) {
  const float* m = vp.d;
  float cx = m[0] * p.x + m[4] * p.y + m[8]  * p.z + m[12];
  float cy = m[1] * p.x + m[5] * p.y + m[9]  * p.z + m[13];
  float cw = m[3] * p.x + m[7] * p.y + m[11] * p.z + m[15];
  if (cw < 0.01f) return {0, 0, false};
  float nx = cx / cw, ny = cy / cw;
  return {(nx * 0.5f + 0.5f) * w, (1.f - (ny * 0.5f + 0.5f)) * h, true};
}

static float dist3(const Vec3& a, const Vec3& b) {
  float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
  return sqrtf(dx * dx + dy * dy + dz * dz);
}

// ── game access ─────────────────────────────────────────────────────────────────────────
static bool resolve_symbols() {
  if (!g_base) g_base = find_module_base("libil2cpp.so");
  if (!g_base) return false;
  g_klass_pm       = klass_from_slot(il2::TI_PlayerManager, "PlayerManager", nullptr);
  g_klass_camera   = klass_from_slot(il2::TI_Camera, "Camera", "UnityEngine");
  g_klass_animator = klass_from_slot(il2::TI_Animator, "Animator", "UnityEngine");
  if (!g_klass_pm || !g_klass_camera) return false;
  LOGI("il2cpp %p  PlayerManager %p  Camera %p  Animator %p",
       (void*)g_base, g_klass_pm, g_klass_camera, g_klass_animator);
  return true;
}

static std::vector<void*> player_list() {
  std::vector<void*> out;
  void* sf = static_fields(g_klass_pm);
  if (!sf) return out;
  void* list = rd<void*>((uintptr_t)sf + off::pm_static_client);
  if (!inproc(list)) return out;
  auto* l = reinterpret_cast<Il2CppList*>(list);
  if (!inproc(l->items) || l->size <= 0 || l->size > 256) return out;
  auto* arr = reinterpret_cast<Il2CppArray*>(l->items);
  if (arr->max_length > 4096) return out;
  auto** data = reinterpret_cast<void**>(arr->data);
  int n = l->size < static_cast<int>(arr->max_length) ? l->size : static_cast<int>(arr->max_length);
  for (int i = 0; i < n; ++i)
    if (inproc(data[i])) out.push_back(data[i]);
  return out;
}

static void* player_cu(void* pm) {
  void* kai = rd<void*>((uintptr_t)pm + off::pm_team);
  if (!inproc(kai)) return nullptr;
  void* cu = rd<void*>((uintptr_t)kai + off::kai_team);
  return inproc(cu) ? cu : nullptr;
}

static void* player_animator(void* pm) {
  void* anim = rd<void*>((uintptr_t)pm + off::pm_animator);
  if (!inproc(anim)) return nullptr;
  void* ak = rd<void*>(reinterpret_cast<uintptr_t>(anim));
  if (g_klass_animator && ak != g_klass_animator) return nullptr;
  return anim;
}

static std::string player_name(void* pm) {
  void* di = rd<void*>((uintptr_t)pm + off::pm_nicklabel);
  if (!inproc(di)) return {};
  void* text = rd<void*>((uintptr_t)di + off::di_nickname);
  if (!inproc(text)) return {};
  return read_string(rd<void*>((uintptr_t)text + off::text_m_Text));
}

// ── frame ───────────────────────────────────────────────────────────────────────────────
struct Box {
  float x1, y1, x2, y2, headX, headY, dist, hp, hpMax, flags;
};

static void build_frame(std::vector<Box>& out) {
  out.clear();
  g_names.clear();
  if (!g_ready) {
    if (!resolve_symbols()) return;
    g_ready = true;
  }
  if (!(g_cfg.flags & kFlagEsp)) return;

  static int frame = 0;
  if (!inproc(g_camera) || (++frame % 120) == 0) g_camera = main_camera();  // re-resolve on scene load
  void* cam = g_camera;
  if (!inproc(cam)) return;

  int w = camera_dim(il2::Camera_pixelWidth,  cam, g_screenW);
  int h = camera_dim(il2::Camera_pixelHeight, cam, g_screenH);
  const Mat4 vp = mat_mul(camera_matrix(il2::Camera_projectionMatrix, cam),
                          camera_matrix(il2::Camera_worldToCameraMatrix, cam));

  auto players = player_list();

  // throttled telemetry: without adb this log is the only way to tell "no match yet" from
  // "wrong offsets" — it says how many PlayerManager instances the list holds and whether the
  // camera/il2cpp base resolved.
  static uint64_t lastLog = 0;
  const uint64_t now = static_cast<uint64_t>(time(nullptr));
  if (now != lastLog) {
    lastLog = now;
    void* sf = g_klass_pm ? static_fields(g_klass_pm) : nullptr;
    void* list = sf ? rd<void*>((uintptr_t)sf + off::pm_static_client) : nullptr;
    LOGI("esp: players=%zu camera=%p list=%p sf=%p", players.size(), cam, list, sf);
  }

  if (players.empty()) return;

  void* local = nullptr;
  for (void* pm : players) if (is_local_player(pm)) { local = pm; break; }
  void* localCu = local ? player_cu(local) : nullptr;

  Vec3 localPos{};
  if (local) {
    void* anim = player_animator(local);
    void* t = anim ? animator_bone(anim, g_cfg.headBone) : nullptr;
    if (!t) t = gameobject_transform(rd<void*>((uintptr_t)local + off::pm_characterModel));
    if (t) localPos = transform_position(t);
  }

  for (void* pm : players) {
    if (pm == local) continue;
    void* anim = player_animator(pm);
    if (!anim) continue;
    void* headT = animator_bone(anim, g_cfg.headBone);
    if (!headT) continue;
    void* hipsT = animator_bone(anim, 0);  // HumanBodyBones.Hips

    Vec3 head = transform_position(headT);
    Vec3 hips = hipsT ? transform_position(hipsT) : Vec3{head.x, head.y - 1.6f, head.z};
    float dist = local ? dist3(localPos, head) : 0.f;
    if (g_cfg.maxDist > 0.f && dist > g_cfg.maxDist) continue;

    ScreenPt sh = world_to_screen(vp, head, w, h);
    ScreenPt sf = world_to_screen(vp, hips, w, h);
    if (!sh.valid) continue;
    if (sh.x < -w * 0.2f || sh.x > w * 1.2f || sh.y < -h * 0.2f || sh.y > h * 1.2f) continue;

    float boxH = sf.valid ? sf.y - sh.y : 0.f;
    if (boxH < 8.f || boxH > h * 0.85f) boxH = h * 0.11f;
    const float boxW = boxH * 0.45f;

    float hpMax = 100.f, hp = 0.f;
    void* vit = rd<void*>((uintptr_t)pm + off::pm_vitals);
    if (inproc(vit)) {
      float mx = rd<float>((uintptr_t)vit + off::vitals_maxHealth);
      float cur = rd<float>((uintptr_t)vit + off::vitals_health);
      if (mx > 0.f && mx < 1e5f) hpMax = mx;
      if (cur >= 0.f && cur < 1e5f) hp = cur;
    }

    const bool team = localCu && player_cu(pm) == localCu && (g_cfg.flags & kFlagTeam);
    out.push_back(Box{sh.x - boxW * 0.5f, sh.y, sh.x + boxW * 0.5f, sh.y + boxH,
                      sh.x, sh.y, dist, hp, hpMax, team ? 1.f : 0.f});

    g_names.push_back((g_cfg.flags & kFlagName) ? player_name(pm) : std::string());
  }
}


// ── crash guard ─────────────────────────────────────────────────────────────────────────
// The ESP reads game memory through offsets from a dump; one wrong offset is a SIGSEGV and no
// Java handler can see it. So: a signal handler that records the fault, and a marker file that
// tells the next run "the previous one died with the ESP active" — that run then starts with the
// overlay off instead of crash-looping.
static char g_dir[400];
static char g_crashPath[512];
static char g_armPath[512];
static volatile sig_atomic_t g_ticks = 0;

static void crash_signal(int sig, siginfo_t* si, void*) {
  char buf[256];
  int n = snprintf(buf, sizeof buf, "signal %d (%s) addr=%p code=%d ticks=%d\n",
                   sig, strsignal(sig), si ? si->si_addr : nullptr, si ? si->si_code : 0,
                   (int) g_ticks);
  int fd = open(g_crashPath, O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (fd >= 0) {
    ssize_t ignored = write(fd, buf, (size_t) n);
    (void) ignored;
    close(fd);
  }
  // let the system do its own thing too: tombstone in logcat, process death as usual
  signal(sig, SIG_DFL);
  raise(sig);
}

static void install_guard(const char* dir) {
  snprintf(g_dir, sizeof g_dir, "%s", dir);
  snprintf(g_crashPath, sizeof g_crashPath, "%s/native-crash.txt", dir);
  snprintf(g_armPath, sizeof g_armPath, "%s/esp-armed", dir);

#ifdef SIGSTKSZ
  static char altstack[SIGSTKSZ * 4];
#else
  static char altstack[64 * 1024];
#endif
  stack_t ss{};
  ss.ss_size = sizeof altstack;
  ss.ss_sp = altstack;
  sigaltstack(&ss, nullptr);

  struct sigaction sa{};
  sa.sa_sigaction = crash_signal;
  sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
  sigemptyset(&sa.sa_mask);
  for (int sig : {SIGSEGV, SIGBUS, SIGABRT, SIGILL, SIGFPE}) sigaction(sig, &sa, nullptr);

  // previous run died while the overlay was ticking?
  struct stat st{};
  if (stat(g_armPath, &st) == 0) {
    LOGI("previous run died with the esp active — starting with the overlay disabled");
    g_cfg.flags &= ~kFlagEsp;
    unlink(g_armPath);
  }
  LOGI("guard installed: %s", g_crashPath);
}

static void arm_marker(bool on) {
  if (on) {
    int fd = open(g_armPath, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd >= 0) close(fd);
  } else {
    unlink(g_armPath);
  }
}

// ── JNI ─────────────────────────────────────────────────────────────────────────────────
extern "C" JNIEXPORT void JNICALL
Java_oxhk_Oxhk_nativeInit(JNIEnv* env, jclass, jstring filesDir, jint w, jint h) {
  g_screenW = w > 0 ? w : g_screenW;
  g_screenH = h > 0 ? h : g_screenH;
  if (filesDir) {
    const char* d = env->GetStringUTFChars(filesDir, nullptr);
    if (d) {
      install_guard(d);
      env->ReleaseStringUTFChars(filesDir, d);
    }
  }
  g_base = find_module_base("libil2cpp.so");
  LOGI("init: libil2cpp %p, screen %dx%d", (void*)g_base, g_screenW, g_screenH);
}

extern "C" JNIEXPORT void JNICALL
Java_oxhk_Oxhk_nativeSetConfig(JNIEnv*, jclass, jint flags, jint headBone, jfloat maxDist) {
  g_cfg.flags = flags;
  g_cfg.headBone = headBone;
  g_cfg.maxDist = maxDist;
}

// out[0] = count, then 10 floats per entity; returns count
extern "C" JNIEXPORT jint JNICALL
Java_oxhk_Oxhk_nativeTick(JNIEnv* env, jclass, jfloatArray out, jint screenW, jint screenH) {
  if (!out) return 0;
  g_screenW = screenW > 0 ? screenW : g_screenW;
  g_screenH = screenH > 0 ? screenH : g_screenH;

  // arm while the overlay is live; after ~600 frames (10 s) we trust the offsets enough
  if (g_ticks == 0) arm_marker(true);
  if (g_ticks < 600 && ++g_ticks == 600) arm_marker(false);

  static std::vector<Box> boxes;
  if (g_cfg.flags & kFlagEsp) build_frame(boxes);
  else boxes.clear();

  const jsize len = env->GetArrayLength(out);
  const int maxEnt = (len - 1) / 10;
  int n = static_cast<int>(boxes.size());
  if (n > maxEnt) n = maxEnt;

  jfloat* buf = env->GetFloatArrayElements(out, nullptr);
  if (!buf) return 0;
  buf[0] = static_cast<float>(n);
  for (int i = 0; i < n; ++i) {
    const Box& b = boxes[i];
    float* p = buf + 1 + i * 10;
    p[0] = b.x1; p[1] = b.y1; p[2] = b.x2; p[3] = b.y2;
    p[4] = b.headX; p[5] = b.headY;
    p[6] = b.dist; p[7] = b.hp; p[8] = b.hpMax; p[9] = b.flags;
  }
  env->ReleaseFloatArrayElements(out, buf, 0);
  return n;
}

extern "C" JNIEXPORT jint JNICALL
Java_oxhk_Oxhk_nativeNames(JNIEnv* env, jclass, jobjectArray out) {
  if (!out) return 0;
  const jsize cap = env->GetArrayLength(out);
  int n = 0;
  for (const std::string& s : g_names) {
    if (n >= cap) break;
    if (s.empty()) { env->SetObjectArrayElement(out, n, nullptr); ++n; continue; }
    jstring js = env->NewStringUTF(s.c_str());
    env->SetObjectArrayElement(out, n, js);
    env->DeleteLocalRef(js);
    ++n;
  }
  return n;
}
