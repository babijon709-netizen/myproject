// liboxhk.so — internal (in-process) tool for Oxide / HyperHug, build 11978.
//
// Offsets come from dump11978.7z -> Il2CppDumper: dump.cs (field offsets, layouts) and
// script.json (method RVAs, TypeInfoPointers metadata-usage slots). Nothing is guessed
// from a different build.
//
// Why there is no il2cpp API here:
//   this libil2cpp.so exports zero `il2cpp_*` symbols (dynsym = libc + Brotli/ICU only) and
//   its code sits in a renamed PT_LOAD section named "il2cpp" instead of ".text" — the build
//   is export-stripped / section-renamed. So no dlsym for il2cpp_domain_get & friends.
//   Everything is `il2cpp_base + RVA`; fields are `obj + offset`; the handful of managed
//   calls we need (Transform.get_position, Camera matrices, Animator.GetBoneTransform) are
//   direct calls into their AOT entry points with MethodInfo = nullptr.
//
// Execution model:
//   no inline hooks, no code patching inside the game's libs. Java posts a per-frame callback
//   on the Android UI thread and Unity runs its script loop on that same thread, so every
//   managed call happens on the thread the game expects.

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

#define LOG_TAG "oxhk"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// ── libil2cpp.so RVAs (dump.cs "VA" column == base + RVA) ────────────────────────────────
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
constexpr uint64_t TI_PlayerVitals  = 0xE66DF50;  // Oxide.PlayerVitals
constexpr uint64_t TI_Camera        = 0xE666348;  // UnityEngine.Camera
constexpr uint64_t TI_Animator      = 0xE6651F0;  // UnityEngine.Animator
constexpr uint64_t TI_ListPlayerMgr = 0xE65E8A8;  // List<Oxide.PlayerManager>
}  // namespace il2dump

namespace il2 = il2dump;

// ── offsets (dump.cs) ────────────────────────────────────────────────────────────────────
namespace off {
// Oxide.PlayerManager : Mirror.NetworkBehaviour
constexpr size_t pm_vitals         = 0xC8;   // Oxide.PlayerVitals
constexpr size_t pm_team           = 0x120;  // kai
constexpr size_t pm_nicklabel      = 0x130;  // Di
constexpr size_t pm_characterModel = 0x150;  // GameObject
constexpr size_t pm_animator       = 0x1B0;  // Animator
// statics of PlayerManager, offsets inside static_fields
constexpr size_t pm_static_client  = 0x10;   // List<PlayerManager> clientPlayerList
// Oxide.GenericVitals <- Oxide.kAg <- Mirror.NetworkBehaviour
constexpr size_t vitals_maxHealth  = 0x88;
constexpr size_t vitals_health     = 0xB8;
// kai (team component)
constexpr size_t kai_team          = 0x98;   // Cu
// Di (nick label)
constexpr size_t di_nickname       = 0x38;   // UnityEngine.UI.Text
// UnityEngine.UI.Text
constexpr size_t text_m_Text       = 0xE0;   // string
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
  kFlagEsp   = 1 << 0,
  kFlagBoxes = 1 << 1,
  kFlagClean = 1 << 2,  // head dot + distance only
  kFlagAim   = 1 << 3,
  kFlagTeam  = 1 << 4,  // team check on
  kFlagMenu  = 1 << 5,
};

struct Config {
  int   flags         = kFlagEsp | kFlagBoxes | kFlagTeam | kFlagMenu;
  float fovPx         = 260.f;
  float smoothing     = 4.f;
  float gain          = 1.f;
  int   touchMs       = 40;
  int   headBone      = 10;   // HumanBodyBones.Head
  int   aimCooldownMs = 30;
};
static Config g_cfg;

// ── state ───────────────────────────────────────────────────────────────────────────────
static uintptr_t g_base = 0;
static void* g_klass_pm = nullptr;
static void* g_klass_camera = nullptr;
static void* g_klass_animator = nullptr;
static void* g_camera = nullptr;
static int   g_screenW = 1080, g_screenH = 2400;
static long long g_lastAimMs = 0;
static bool  g_ready = false;
static std::vector<std::string> g_lastNames;

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

// metadata-usage slot -> Il2CppClass*; the slot is filled lazily by the game, so retry each tick
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

// ── managed call shims ──────────────────────────────────────────────────────────────────
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
static Mat4 mat_mul(const Mat4& a, const Mat4& b) {  // column-major a * b
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

// Convention: clip = (P*V) * p with Unity's GL-style projection (NDC y up, screen y bottom-up).
// The overlay is an Android view, so y is flipped to top-left here.
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
  LOGI("il2cpp base %p  PlayerManager %p  Camera %p  Animator %p",
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

// ── frame build ─────────────────────────────────────────────────────────────────────────
static long long now_ms() {
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<long long>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

struct FrameOut {
  int   aim = 0;
  float aimDx = 0, aimDy = 0;
  std::vector<float> ent;                 // 10 floats per entity
  std::vector<std::string> names;
};

struct Candidate {
  void* pm;
  Vec3  head, feet;
  float hp, hpMax, dist;
  bool  team;
};

static void build_frame(FrameOut& fo) {
  if (!g_ready) {
    if (!resolve_symbols()) return;
    g_ready = true;
  }

  static int frame = 0;
  if (!inproc(g_camera) || (++frame % 120) == 0) g_camera = main_camera();  // re-resolve on scene load
  void* cam = g_camera;
  if (!inproc(cam)) return;

  int w = camera_dim(il2::Camera_pixelWidth,  cam, g_screenW);
  int h = camera_dim(il2::Camera_pixelHeight, cam, g_screenH);
  const Mat4 vp = mat_mul(camera_matrix(il2::Camera_projectionMatrix, cam),
                          camera_matrix(il2::Camera_worldToCameraMatrix, cam));

  auto players = player_list();
  if (players.empty()) return;

  void* local = nullptr;
  for (void* pm : players) if (is_local_player(pm)) { local = pm; break; }
  void* localCu = local ? player_cu(local) : nullptr;

  Vec3 localPos{};
  if (local) {
    void* anim = player_animator(local);
    void* t = anim ? animator_bone(anim, g_cfg.headBone) : nullptr;
    if (!t) {
      void* go = rd<void*>((uintptr_t)local + off::pm_characterModel);
      t = gameobject_transform(go);
    }
    if (t) localPos = transform_position(t);
  }

  std::vector<Candidate> cands;
  cands.reserve(players.size());
  for (void* pm : players) {
    if (pm == local) continue;
    void* anim = player_animator(pm);
    if (!anim) continue;
    void* headT = animator_bone(anim, g_cfg.headBone);
    if (!headT) continue;
    void* hipsT = animator_bone(anim, 0);  // HumanBodyBones.Hips

    Candidate c{};
    c.pm   = pm;
    c.head = transform_position(headT);
    c.feet = hipsT ? transform_position(hipsT) : Vec3{c.head.x, c.head.y - 1.6f, c.head.z};
    c.team = (g_cfg.flags & kFlagTeam) != 0 && localCu && player_cu(pm) == localCu;
    c.dist = local ? dist3(localPos, c.head) : 0.f;
    c.hpMax = 100.f;
    c.hp = 0.f;
    void* vit = rd<void*>((uintptr_t)pm + off::pm_vitals);
    if (inproc(vit)) {
      float mx = rd<float>((uintptr_t)vit + off::vitals_maxHealth);
      float hp = rd<float>((uintptr_t)vit + off::vitals_health);
      if (mx > 0.f && mx < 1e5f) c.hpMax = mx;
      if (hp >= 0.f && hp < 1e5f) c.hp = hp;
    }
    cands.push_back(c);
  }

  float bestAim = 1e9f, bestDx = 0, bestDy = 0;

  for (const Candidate& c : cands) {
    ScreenPt sh = world_to_screen(vp, c.head, w, h);
    ScreenPt sf = world_to_screen(vp, c.feet, w, h);
    if (sh.valid && sh.x > -w * 0.2f && sh.x < w * 1.2f && sh.y > -h * 0.2f && sh.y < h * 1.2f) {
      if (g_cfg.flags & kFlagEsp) {
        float boxH = (sf.valid ? sf.y - sh.y : 0.f);
        if (boxH < 8.f || boxH > h * 0.85f) boxH = h * 0.11f;
        const float boxW = boxH * 0.45f;
        float rec[10] = {sh.x - boxW * 0.5f, sh.y, sh.x + boxW * 0.5f, sh.y + boxH,
                         sh.x, sh.y, c.dist, c.hp, c.hpMax, c.team ? 1.f : 0.f};
        fo.ent.insert(fo.ent.end(), rec, rec + 10);
        fo.names.push_back(player_name(c.pm));
      }
    }
    if (c.team || !(g_cfg.flags & kFlagAim)) continue;
    if (!sh.valid) continue;
    float ddx = sh.x - g_screenW * 0.5f;
    float ddy = sh.y - g_screenH * 0.5f;
    float d = sqrtf(ddx * ddx + ddy * ddy);
    if (d > g_cfg.fovPx) continue;
    if (d < bestAim) { bestAim = d; bestDx = ddx; bestDy = ddy; }
  }

  if ((g_cfg.flags & kFlagAim) && bestAim < 1e9f) {
    long long t = now_ms();
    if (t - g_lastAimMs >= g_cfg.aimCooldownMs) {
      g_lastAimMs = t;
      const float s = g_cfg.smoothing > 0.01f ? g_cfg.smoothing : 1.f;
      fo.aim = 1;
      fo.aimDx = bestDx / s * g_cfg.gain;
      fo.aimDy = bestDy / s * g_cfg.gain;
    }
  }
}

// ── JNI ─────────────────────────────────────────────────────────────────────────────────
extern "C" JNIEXPORT void JNICALL
Java_oxhk_Oxhk_nativeInit(JNIEnv*, jclass, jstring, jint w, jint h) {
  g_screenW = w > 0 ? w : g_screenW;
  g_screenH = h > 0 ? h : g_screenH;
  g_base = find_module_base("libil2cpp.so");
  LOGI("init: libil2cpp base %p, screen %dx%d", (void*)g_base, g_screenW, g_screenH);
}

extern "C" JNIEXPORT void JNICALL
Java_oxhk_Oxhk_nativeSetConfig(JNIEnv*, jclass, jint flags, jfloat fovPx, jfloat smoothing,
                               jfloat gain, jint touchMs, jint headBone, jint cooldownMs) {
  g_cfg.flags = flags;
  g_cfg.fovPx = fovPx;
  g_cfg.smoothing = smoothing;
  g_cfg.gain = gain;
  g_cfg.touchMs = touchMs;
  g_cfg.headBone = headBone;
  g_cfg.aimCooldownMs = cooldownMs;
}

// out = [0]aim [1]dx [2]dy [3]count, then count * 10 floats; returns count
extern "C" JNIEXPORT jint JNICALL
Java_oxhk_Oxhk_nativeTick(JNIEnv* env, jclass, jfloatArray out, jint screenW, jint screenH) {
  if (!out) return 0;
  g_screenW = screenW > 0 ? screenW : g_screenW;
  g_screenH = screenH > 0 ? screenH : g_screenH;

  const jsize len = env->GetArrayLength(out);
  if (len < 14) return 0;

  FrameOut fo;
  build_frame(fo);
  g_lastNames = fo.names;

  int n = static_cast<int>(fo.names.size());
  const int maxEnt = (len - 4) / 10;
  if (n > maxEnt) n = maxEnt;

  jfloat* buf = env->GetFloatArrayElements(out, nullptr);
  if (!buf) return 0;
  buf[0] = static_cast<float>(fo.aim);
  buf[1] = fo.aimDx;
  buf[2] = fo.aimDy;
  buf[3] = static_cast<float>(n);
  if (n > 0) memcpy(buf + 4, fo.ent.data(), sizeof(float) * 10 * n);
  env->ReleaseFloatArrayElements(out, buf, 0);
  return n;
}

extern "C" JNIEXPORT jint JNICALL
Java_oxhk_Oxhk_nativeNames(JNIEnv* env, jclass, jobjectArray out) {
  if (!out) return 0;
  const jsize cap = env->GetArrayLength(out);
  int n = 0;
  for (const std::string& s : g_lastNames) {
    if (n >= cap) break;
    jstring js = env->NewStringUTF(s.c_str());
    env->SetObjectArrayElement(out, n, js);
    env->DeleteLocalRef(js);
    ++n;
  }
  return n;
}
