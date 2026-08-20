#include "main.h"
#include "game.h"
#include <cmath>
#include <atomic>
#include <chrono>
#include <string>
#include <GLES3/gl3.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fstream>
#include <vector>
#include <algorithm>
#include <random>
#include <sys/inotify.h>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <time.h>

#if __has_include("media/anime.h")
#  include "media/anime.h"
#  define ANIME_IMAGE_AVAILABLE
#endif

#if __has_include("media/icons.h")
#  include "media/icons.h"
#  define ICONS_AVAILABLE
#endif

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#include "stb_image/stb_image.h"
#include "Blur/Blur.h"

#if __has_include("media/audio.h")
#  include "media/audio.h"
#  define AUDIO_AVAILABLE
#endif

#ifdef AUDIO_AVAILABLE
#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_Android.h>

enum SoundId { SND_CLICK = 0, SND_SUCCESS, SND_COUNT };

struct AudioState {
    SLObjectItf engineObj  = nullptr;
    SLEngineItf engineItf  = nullptr;
    SLObjectItf outputMix  = nullptr;
    struct Player {
        SLObjectItf                   obj  = nullptr;
        SLPlayItf                     play = nullptr;
        SLAndroidSimpleBufferQueueItf bq   = nullptr;
    } players[SND_COUNT];
};
static AudioState g_audio;

static void AudioInit() {
    SLEngineOption opt[] = {{ SL_ENGINEOPTION_THREADSAFE, SL_BOOLEAN_TRUE }};
    if (slCreateEngine(&g_audio.engineObj, 1, opt, 0, nullptr, nullptr) != SL_RESULT_SUCCESS) return;
    if ((*g_audio.engineObj)->Realize(g_audio.engineObj, SL_BOOLEAN_FALSE) != SL_RESULT_SUCCESS) return;
    if ((*g_audio.engineObj)->GetInterface(g_audio.engineObj, SL_IID_ENGINE, &g_audio.engineItf) != SL_RESULT_SUCCESS) return;
    if ((*g_audio.engineItf)->CreateOutputMix(g_audio.engineItf, &g_audio.outputMix, 0, nullptr, nullptr) != SL_RESULT_SUCCESS) return;
    if ((*g_audio.outputMix)->Realize(g_audio.outputMix, SL_BOOLEAN_FALSE) != SL_RESULT_SUCCESS) return;

    const struct { const uint8_t* data; size_t len; } srcs[SND_COUNT] = {
        { click_wav,   click_wav_len   },
        { success_wav, success_wav_len },
    };

    for (int i = 0; i < SND_COUNT; i++) {
        const uint8_t* wav = srcs[i].data;
        uint16_t channels = 0; uint32_t sampleRate = 0; uint16_t bps = 0;
        memcpy(&channels,   wav + 22, 2);
        memcpy(&sampleRate, wav + 24, 4);
        memcpy(&bps,        wav + 34, 2);

        SLDataLocator_AndroidSimpleBufferQueue loc = { SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE, 1 };
        SLDataFormat_PCM fmt = {
            SL_DATAFORMAT_PCM, (SLuint32)channels, (SLuint32)sampleRate * 1000,
            (SLuint32)bps, (SLuint32)bps,
            channels == 2 ? SL_SPEAKER_FRONT_LEFT | SL_SPEAKER_FRONT_RIGHT : SL_SPEAKER_FRONT_CENTER,
            SL_BYTEORDER_LITTLEENDIAN
        };
        SLDataSource src  = { &loc, &fmt };
        SLDataLocator_OutputMix outLoc = { SL_DATALOCATOR_OUTPUTMIX, g_audio.outputMix };
        SLDataSink   sink = { &outLoc, nullptr };

        const SLInterfaceID ids[] = { SL_IID_BUFFERQUEUE };
        const SLboolean     req[] = { SL_BOOLEAN_TRUE };
        if ((*g_audio.engineItf)->CreateAudioPlayer(g_audio.engineItf, &g_audio.players[i].obj, &src, &sink, 1, ids, req) != SL_RESULT_SUCCESS) continue;
        if ((*g_audio.players[i].obj)->Realize(g_audio.players[i].obj, SL_BOOLEAN_FALSE) != SL_RESULT_SUCCESS) continue;
        (*g_audio.players[i].obj)->GetInterface(g_audio.players[i].obj, SL_IID_PLAY,        &g_audio.players[i].play);
        (*g_audio.players[i].obj)->GetInterface(g_audio.players[i].obj, SL_IID_BUFFERQUEUE, &g_audio.players[i].bq);
        (*g_audio.players[i].play)->SetPlayState(g_audio.players[i].play, SL_PLAYSTATE_PLAYING);
    }
}

static void PlaySound(SoundId id) {
    static const struct { const uint8_t* data; size_t len; } srcs[SND_COUNT] = {
        { click_wav,   click_wav_len   },
        { success_wav, success_wav_len },
    };
    auto& p = g_audio.players[id];
    if (!p.bq || !p.play) return;
    (*p.play)->SetPlayState(p.play, SL_PLAYSTATE_STOPPED);
    (*p.bq)->Clear(p.bq);
    (*p.bq)->Enqueue(p.bq, (void*)(srcs[id].data + 44), (SLuint32)(srcs[id].len - 44));
    (*p.play)->SetPlayState(p.play, SL_PLAYSTATE_PLAYING);
}

static void AudioFree() {
    for (int i = 0; i < SND_COUNT; i++) {
        if (g_audio.players[i].obj) { (*g_audio.players[i].obj)->Destroy(g_audio.players[i].obj); g_audio.players[i].obj = nullptr; }
    }
    if (g_audio.outputMix) { (*g_audio.outputMix)->Destroy(g_audio.outputMix); g_audio.outputMix = nullptr; }
    if (g_audio.engineObj) { (*g_audio.engineObj)->Destroy(g_audio.engineObj); g_audio.engineObj = nullptr; }
}
#else
enum SoundId { SND_CLICK = 0, SND_SUCCESS, SND_COUNT };
static void AudioInit() {}
static void PlaySound(SoundId) {}
static void AudioFree() {}
#endif

namespace xp {

static constexpr uint64_t _ks0 = 0xC3A1F72B9D4E058BULL;
static constexpr uint64_t _ks1 = 0x2B7E3D48F1C96A30ULL;

static constexpr uint8_t _xk(size_t i) noexcept {
    uint64_t s = _ks0 ^ (_ks1 * (i + 1));
    s ^= s >> 33; s *= 0xff51afd7ed558ccdULL;
    s ^= s >> 33; s *= 0xc4ceb9fe1a85ec53ULL;
    s ^= s >> 33;
    return static_cast<uint8_t>(s & 0xFF);
}

template<size_t N>
struct _XS {
    char b[N]{};
    constexpr _XS(const char (&s)[N]) noexcept {
        for (size_t i = 0; i < N; ++i) b[i] = static_cast<char>(static_cast<uint8_t>(s[i]) ^ _xk(i));
    }
    __attribute__((noinline)) const char* d() const noexcept {
        static char o[N];
        for (size_t i = 0; i < N; ++i) o[i] = static_cast<char>(static_cast<uint8_t>(b[i]) ^ _xk(i));
        return o;
    }
};
template<size_t N> constexpr auto _mk(const char (&s)[N]) noexcept { return _XS<N>(s); }

}

#define XS(s) ([]() noexcept -> const char* { static constexpr auto _x = xp::_mk(s); return _x.d(); }())

namespace prot {
static void Init() {}
}

static float g_sw = 1920.f;
static float g_sh = 1080.f;

static void VisibleScreen(float& w, float& h);

static bool g_noRecoilEnabled = false;

namespace ui { namespace bar {
    inline float g_game_alpha = 1.f;
    inline void  set_game_alpha(float a){ g_game_alpha=a; }
    inline float game_alpha(){ return g_game_alpha; }
}}

namespace cfg { namespace esp {
    inline ImVec4 box_col         = {0.20f, 0.85f, 0.35f, 1.f};
    inline ImVec4 box_col_invis   = {1.00f, 0.20f, 0.20f, 1.f};
    inline ImVec4 name_col        = {1.00f, 1.00f, 1.00f, 1.f};
    inline ImVec4 health_col      = {0.20f, 0.85f, 0.35f, 1.f};
    inline ImVec4 distance_col    = {0.70f, 0.70f, 0.70f, 1.f};
    inline ImVec4 weapon_col      = {1.00f, 0.95f, 0.10f, 1.f};
    inline ImVec4 weapon_icon_col = {1.00f, 0.95f, 0.10f, 1.f};
    inline ImVec4 tracer_col      = {1.00f, 0.20f, 0.20f, 1.f};
    inline ImVec4 skeleton_col    = {0.20f, 0.85f, 0.35f, 1.f};
    inline ImVec4 money_col       = {1.00f, 0.95f, 0.10f, 1.f};
    inline ImVec4 ping_col        = {0.40f, 0.85f, 1.00f, 1.f};

    inline bool box          = false;
    inline bool name_esp     = false;
    inline bool health       = false;
    inline bool distance     = false;
    inline bool weapon       = false;
    inline bool weapon_icon  = false;
    inline bool tracer       = false;
    inline bool skeleton     = false;
    inline bool money        = false;
    inline bool  ping_show        = false;
    inline bool  vis_check        = false;
    inline bool  fill             = false;
    inline float stroke           = 2.f;
    inline float rounding         = 0.f;
    inline float fill_pct         = 50.f;
    inline int   box_type         = 0;
    inline float box_rounding     = 0.f;
    inline bool  hp_outline       = true;
    inline bool  hp_gradient      = false;
    inline float tracer_thickness = 1.5f;
    inline ImVec4 hp_min_col      = {1.00f, 0.20f, 0.10f, 1.f};
    inline ImVec4 hp_max_col      = {0.20f, 0.85f, 0.35f, 1.f};
}}

namespace cfg { namespace aim {
    inline bool  enabled           = false;
    inline bool  vis_check         = false;
    inline bool  draw_fov          = false;
    inline float fov               = 80.f;
    inline float smoothness        = 0.35f;
    inline int   bone              = 0;
    inline bool  trigger_bot       = false;
    inline bool  knife_bot         = false;
    inline float trigger_delay     = 0.0f;
    inline float fov_color[4]      = {1.0f, 1.0f, 1.0f, 1.0f};
}}

static std::mt19937& GetRNG() {
    static std::mt19937 rng(std::random_device{}());
    return rng;
}

std::atomic<bool> main_thread_flag{true};
std::atomic<bool> g_frame_done{true};

#define TARGET_PACKAGE "com.catsbit.oxidesurvivalisland"

static pid_t g_target_pid = -1;
static bool  g_esp_attached = false;
static std::thread g_attach_thread;
static std::atomic<bool> g_attach_running{false};

static pid_t find_pid(const char* pkg) {
    DIR* directory = opendir("/proc");
    if (!directory) return -1;
    struct dirent* entry;
    char path[256], command[256];
    pid_t child_fallback = -1;
    while ((entry = readdir(directory))) {
        pid_t pid = (pid_t)atoi(entry->d_name);
        if (pid <= 0) continue;
        snprintf(path, sizeof(path), "/proc/%d/cmdline", pid);
        FILE* file = fopen(path, "r");
        if (!file) continue;
        memset(command, 0, sizeof(command));
        size_t count = fread(command, 1, sizeof(command) - 1, file);
        fclose(file);
        if (count == 0) continue;
        if (strcmp(command, pkg) == 0) { closedir(directory); return pid; }
        size_t pkg_len = strlen(pkg);
        if (child_fallback <= 0 && strncmp(command, pkg, pkg_len) == 0 && command[pkg_len] == ':')
            child_fallback = pid;
    }
    closedir(directory);
    return child_fallback;
}

static void start_attach_thread() {
    g_attach_running.store(true);
    g_attach_thread = std::thread([]() {
        while (g_attach_running.load()) {
            if (!g_esp_attached) {
                pid_t pid = find_pid(TARGET_PACKAGE);
                if (pid > 0 && esp_init(pid)) {
                    g_target_pid = pid;
                    g_esp_attached = true;
                }
            } else {
                pid_t current = find_pid(TARGET_PACKAGE);
                if (current != g_target_pid) {
                    esp_reset();
                    g_esp_attached = false;
                    g_target_pid = -1;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        }
    });
}

static void stop_attach_thread() {
    g_attach_running.store(false);
    if (g_attach_thread.joinable()) g_attach_thread.join();
}

struct ToastState {
    char  msg[128]     = {};
    char  nextMsg[128] = {};
    bool  hasNext      = false;
    float slidePos = 0.f, slideVel = 0.f;
    float life = 0.f;
    float textA = 0.f, textAVel = 0.f;
    float barW = 0.f, barVel = 0.f;
    float widthAnim = 200.f, widthVel = 0.f;
    bool  visible = false, closing = false;
};
static ToastState g_toast;

static void ShowToast(const char* msg) {
    if (!g_toast.visible || g_toast.closing) {
        snprintf(g_toast.msg, sizeof(g_toast.msg), "%s", msg);
        g_toast.life = 0.f; g_toast.closing = false; g_toast.visible = true;
        g_toast.slidePos = 0.f; g_toast.slideVel = 0.f;
        g_toast.textA = 0.f; g_toast.textAVel = 0.f;
        g_toast.barW = 1.f; g_toast.barVel = 0.f;
        g_toast.widthVel = 0.f;
        g_toast.hasNext = false;
    } else {
        snprintf(g_toast.nextMsg, sizeof(g_toast.nextMsg), "%s", msg);
        g_toast.hasNext = true; g_toast.life = 0.f;
    }
}

struct SpriteState {
    GLuint texture = 0;
    static constexpr int   Cols  = 8;
    static constexpr int   Rows  = 10;
    static constexpr int   Total = 74;
    static constexpr float FPS   = 30.f;
    float timer = 0.f;
    int   frame = 0;
};
static SpriteState g_sprite;
static GLuint g_tabIcons[5] = {};

static GLuint LoadTexFromMemory(const unsigned char* data, int len) {
    int w, h, ch;
    unsigned char* px = stbi_load_from_memory(data, len, &w, &h, &ch, 4);
    if (!px) return 0;
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,     GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,     GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    stbi_image_free(px);
    return tex;
}

void LoadTabIcons() {
#ifdef ICONS_AVAILABLE
    g_tabIcons[0] = LoadTexFromMemory(main_tab_png, (int)main_tab_png_len);
    g_tabIcons[1] = LoadTexFromMemory(aimbot_png,   (int)aimbot_png_len);
    g_tabIcons[2] = LoadTexFromMemory(visuals_png,  (int)visuals_png_len);
    g_tabIcons[3] = LoadTexFromMemory(misc_png,     (int)misc_png_len);
    g_tabIcons[4] = LoadTexFromMemory(settings_png, (int)settings_png_len);
#endif
}

void LoadAnimeImage() {
#ifdef ANIME_IMAGE_AVAILABLE
    int w, h, ch;
    unsigned char* px = stbi_load_from_memory(anime_png, (int)anime_png_len, &w, &h, &ch, 4);
    if (!px) return;
    glGenTextures(1, &g_sprite.texture);
    glBindTexture(GL_TEXTURE_2D, g_sprite.texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,     GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,     GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    stbi_image_free(px);
#endif
}

static inline float Lerpf(float a, float b, float t)   { return a + (b - a) * t; }
static inline float Clamp01(float t)                    { return t < 0.f ? 0.f : t > 1.f ? 1.f : t; }
static inline float EaseOut3(float t)                   { float i = 1 - t; return 1 - i * i * i; }
static inline float EaseInOut(float t)                  { return t * t * (3 - 2 * t); }



static inline bool WasTappedHere() {
    const auto& io = ImGui::GetIO();
    if (!io.MouseReleased[0]) return false;
    auto min = ImGui::GetItemRectMin();
    auto max = ImGui::GetItemRectMax();
    auto cp  = io.MouseClickedPos[0];
    auto mp  = io.MousePos;
    bool started = cp.x >= min.x && cp.x <= max.x && cp.y >= min.y && cp.y <= max.y;
    bool ended   = mp.x >= min.x - 12.f && mp.x <= max.x + 12.f && mp.y >= min.y - 12.f && mp.y <= max.y + 12.f;
    float dx = mp.x - cp.x, dy = mp.y - cp.y;
    return started && ended && (dx * dx + dy * dy) <= 30.f * 30.f;
}

static void SpringTick(float& pos, float& vel, float target, float dt) {
    static constexpr float kStiffness = 180.f;
    static constexpr float kDamping   = 26.f;
    static constexpr float kPosTol    = 0.3f;
    static constexpr float kVelTol    = 1.f;
    float force = kStiffness * (target - pos) - kDamping * vel;
    vel += force * dt;
    pos += vel * dt;
    if (fabsf(target - pos) < kPosTol && fabsf(vel) < kVelTol) { pos = target; vel = 0.f; }
}

static void Tick(float& a, bool v, float dt, float spd = 10.f) {
    if (dt > 0.032f) dt = 0.032f;
    a += (float(v) - a) * spd * dt;
    if (!v && a < .002f) a = 0.f;
    if (v && a > .998f) a = 1.f;
}

namespace R {
    static constexpr float Card  = 24.f;
    static constexpr float Pill  = 16.f;
    static constexpr float Sheet = 26.f;
    static constexpr float Btn   = 18.f;
}

static bool  g_darkTheme = false;
static float g_themeT    = 0.f;
static float g_menuFadeIn = 0.f;

namespace C {
    namespace Light {
        static constexpr ImVec4 Bg     = {0.949f, 0.949f, 0.969f, 1};
        static constexpr ImVec4 LeftBg = {0.969f, 0.969f, 0.980f, 1};
        static constexpr ImVec4 Card   = {1.000f, 1.000f, 1.000f, 1};
        static constexpr ImVec4 Acc    = {0.000f, 0.478f, 1.000f, 1};
        static constexpr ImVec4 AccDk  = {0.000f, 0.380f, 0.850f, 1};
        static constexpr ImVec4 Red    = {1.000f, 0.231f, 0.188f, 1};
        static constexpr ImVec4 Txt    = {0.000f, 0.000f, 0.000f, 1};
        static constexpr ImVec4 Dim    = {0.557f, 0.557f, 0.576f, 1};
        static constexpr ImVec4 TrkOff = {0.776f, 0.776f, 0.800f, 1};
        static constexpr ImVec4 Sep    = {0.820f, 0.820f, 0.840f, 1};
    }
    namespace Dark {
        static constexpr ImVec4 Bg     = {0.15f, 0.15f, 0.16f, 1};
        static constexpr ImVec4 LeftBg = {0.137f, 0.137f, 0.145f, 1};
        static constexpr ImVec4 Card   = {0.173f, 0.173f, 0.180f, 1};
        static constexpr ImVec4 Acc    = {0.039f, 0.518f, 1.000f, 1};
        static constexpr ImVec4 AccDk  = {0.000f, 0.400f, 0.870f, 1};
        static constexpr ImVec4 Red    = {1.000f, 0.271f, 0.227f, 1};
        static constexpr ImVec4 Txt    = {1.000f, 1.000f, 1.000f, 1};
        static constexpr ImVec4 Dim    = {0.557f, 0.557f, 0.576f, 1};
        static constexpr ImVec4 TrkOff = {0.231f, 0.231f, 0.251f, 1};
        static constexpr ImVec4 Sep    = {0.260f, 0.260f, 0.290f, 1};
    }

    static inline ImVec4 Lerp4(ImVec4 a, ImVec4 b) {
        float t = EaseInOut(g_themeT);
        return {Lerpf(a.x,b.x,t), Lerpf(a.y,b.y,t), Lerpf(a.z,b.z,t), 1.f};
    }

    static inline ImVec4 Bg()     { return Lerp4(Light::Bg,     Dark::Bg);     }
    static inline ImVec4 LeftBg() { return Lerp4(Light::LeftBg, Dark::LeftBg); }
    static inline ImVec4 Card()   { return Lerp4(Light::Card,   Dark::Card);   }
    static inline ImVec4 Acc()    { return Lerp4(Light::Acc,    Dark::Acc);    }
    static inline ImVec4 AccDk()  { return Lerp4(Light::AccDk,  Dark::AccDk);  }
    static inline ImVec4 Red()    { return Lerp4(Light::Red,    Dark::Red);    }
    static inline ImVec4 Txt()    { return Lerp4(Light::Txt,    Dark::Txt);    }
    static inline ImVec4 Dim()    { return Lerp4(Light::Dim,    Dark::Dim);    }
    static inline ImVec4 TrkOff() { return Lerp4(Light::TrkOff, Dark::TrkOff); }
    static inline ImVec4 Sep()    { return Lerp4(Light::Sep,    Dark::Sep);    }

    static ImU32 Mix(ImVec4 a, ImVec4 b, float t) {
        return IM_COL32(
            int(Lerpf(a.x, b.x, t) * 255), int(Lerpf(a.y, b.y, t) * 255),
            int(Lerpf(a.z, b.z, t) * 255), int(Lerpf(a.w, b.w, t) * 255));
    }
    static ImU32 U(ImVec4 v)               { return ImGui::ColorConvertFloat4ToU32(v); }
    static ImU32 UA(ImVec4 v, float alpha) { v.w = alpha; return ImGui::ColorConvertFloat4ToU32(v); }
}

void ApplyTheme() {
    ImGui::GetIO().ConfigDebugHighlightIdConflicts = false;
    auto& s = ImGui::GetStyle();
    s.WindowRounding = 24; s.ChildRounding  = 20; s.FrameRounding = 16;
    s.GrabRounding   = 16; s.PopupRounding  = 16; s.TabRounding   = 16;
    s.WindowBorderSize = 0; s.FrameBorderSize = 0;
    s.ItemSpacing  = {10, 0}; s.FramePadding  = {14, 12}; s.WindowPadding = {0, 0};
    s.ScrollbarSize = 0; s.GrabMinSize = 20;
    auto* c = s.Colors;
    c[ImGuiCol_WindowBg]              = C::Bg();
    c[ImGuiCol_ChildBg]               = {0, 0, 0, 0};
    c[ImGuiCol_Border]                = C::Sep();
    c[ImGuiCol_BorderShadow]          = {0, 0, 0, 0};
    c[ImGuiCol_Text]                  = C::Txt();
    c[ImGuiCol_TextDisabled]          = C::Dim();
    c[ImGuiCol_ScrollbarBg]           = {0, 0, 0, 0};
    c[ImGuiCol_ScrollbarGrab]         = {0, 0, 0, 0};
    c[ImGuiCol_ScrollbarGrabHovered]  = {0, 0, 0, 0};
    c[ImGuiCol_ScrollbarGrabActive]   = {0, 0, 0, 0};
    c[ImGuiCol_SliderGrab]            = {0, 0, 0, 0};
    c[ImGuiCol_SliderGrabActive]      = {0, 0, 0, 0};
    c[ImGuiCol_FrameBg]               = {0, 0, 0, 0};
    c[ImGuiCol_FrameBgHovered]        = {0, 0, 0, 0};
    c[ImGuiCol_FrameBgActive]         = {0, 0, 0, 0};
    c[ImGuiCol_Button]                = {0, 0, 0, 0};
    c[ImGuiCol_ButtonHovered]         = {0, 0, 0, 0};
    c[ImGuiCol_ButtonActive]          = {0, 0, 0, 0};
    c[ImGuiCol_Header]                = {0, 0, 0, 0};
    c[ImGuiCol_HeaderHovered]         = {0, 0, 0, 0};
    c[ImGuiCol_HeaderActive]          = {0, 0, 0, 0};
    c[ImGuiCol_NavHighlight]          = {0, 0, 0, 0};
    c[ImGuiCol_NavWindowingHighlight] = {0, 0, 0, 0};
    c[ImGuiCol_Separator]             = C::Sep();
}

namespace Layout {
    static constexpr float RowH      = 78.f;
    static constexpr float SliderH   = 108.f;
    static constexpr float HeaderH   = 66.f;
    static constexpr float Inset     = 16.f;
    static constexpr float PadX      = 20.f;
    static constexpr float TabH      = 70.f;
    static constexpr float TabPad    = 6.f;
    static constexpr float BtnH      = 62.f;
}

struct InputState {
    bool touchConsumed = false;
};
static InputState g_input;

struct AppState {
    struct SliderAnim { float pos = -1.f; float vel = 0.f; };
    struct RadioAnim  { float scale = 1.f, scaleVel = 0.f, ring = 0.f, ringVel = 0.f; };

    int   cur_tab = 0;
    bool  aim_touch = false, aim_pos = false, aim_special = false;
    int   aim_bone = 0;
    bool  esp_box = false, esp_name = false, esp_hp = false, esp_wall = false, esp_chams = false;
    bool  esp_weapon = false, esp_weapon_icon = false, esp_tracer = false, esp_skeleton = false;
    bool  esp_money = false, esp_ping = false;
    float esp_thick = 1.5f;
    float gun_str = 0.35f, gun_fov = 80.f, gun_trigger_delay = 0.0f;
    bool  ui_fps = false, ui_dark_mode = false, ui_show_sep = false;

    float tab_alpha = 1.f, tab_slide = 0.f, tab_slide_vel = 0.f;
    float a_aim_touch = 0, a_aim_pos = 0, a_aim_spec = 0;
    float a_aim_head  = 1, a_aim_chest = 0, a_aim_pelvis = 0;
    RadioAnim ra_aim_head, ra_aim_chest, ra_aim_pelvis;
    float a_esp_box = 0, a_esp_name = 0, a_esp_hp = 0, a_esp_wall = 0, a_esp_chams = 0;
    float a_esp_weapon = 0, a_esp_weapon_icon = 0, a_esp_tracer = 0, a_esp_skeleton = 0;
    float a_esp_money = 0, a_esp_ping = 0;
    float a_ui_fps = 0, a_ui_dark = 0, a_ui_sep = 0;

    SliderAnim sl_gun_str, sl_gun_fov, sl_esp_thick, sl_gun_trig;
};
static AppState g_state;

static ImU32 ColU32(const ImVec4& c) {
    return IM_COL32((int)(c.x * 255), (int)(c.y * 255), (int)(c.z * 255), (int)(c.w * 255));
}

static void DrawEspOverlay() {
    float sw = (float)native_window_screen_x;
    float sh = (float)native_window_screen_y;
    if (displayInfo.width > displayInfo.height && displayInfo.width >= 100 && displayInfo.height >= 100) {
        sw = (float)displayInfo.width;
        sh = (float)displayInfo.height;
    } else if (displayInfo.height > displayInfo.width && displayInfo.height >= 100 && displayInfo.width >= 100) {
        sw = (float)displayInfo.height;
        sh = (float)displayInfo.width;
    }
    ImDrawList* dl = ImGui::GetBackgroundDrawList();

    if (!g_esp_attached) return;
    if (!g_state.esp_box && !g_state.esp_chams && !g_state.esp_wall && !g_state.esp_skeleton) return;

    std::vector<EspBox> boxes = esp_get_boxes((int)sw, (int)sh, g_state.esp_skeleton);
    constexpr int BOX_EDGES[][2] = {
        {0,1},{1,2},{2,3},{3,0},
        {4,5},{5,6},{6,7},{7,4},
        {0,4},{1,5},{2,6},{3,7}
    };
    float thick = g_state.esp_thick;
    if (thick < 0.5f) thick = 0.5f;

    for (const EspBox& box : boxes) {
        if (!std::isfinite(box.x1) || !std::isfinite(box.y1) || !std::isfinite(box.x2) || !std::isfinite(box.y2)) continue;

        if (g_state.esp_chams) {
            bool all_valid = true;
            for (int c = 0; c < 8; ++c) {
                if (!box.corner_visible[c] || !std::isfinite(box.corners[c][0]) || !std::isfinite(box.corners[c][1]) || box.corners[c][0] < 0 || box.corners[c][1] < 0) {
                    all_valid = false;
                    break;
                }
            }
            if (all_valid) {
                for (const auto& edge : BOX_EDGES) {
                    int a = edge[0], b = edge[1];
                    dl->AddLine(
                        ImVec2(box.corners[a][0], box.corners[a][1]),
                        ImVec2(box.corners[b][0], box.corners[b][1]),
                        ColU32(cfg::esp::box_col_invis), thick
                    );
                }
            }
        }

        if (g_state.esp_box)
            dl->AddRect(ImVec2(box.x1, box.y1), ImVec2(box.x2, box.y2), ColU32(cfg::esp::box_col), cfg::esp::box_rounding, 0, thick);

        if (g_state.esp_wall) {
            char label[32];
            if (box.distance >= 0.0f) snprintf(label, sizeof(label), "%.1fm", box.distance);
            else snprintf(label, sizeof(label), "PLAYER");
            dl->AddText(ImVec2(box.x1, box.y1 - 22.0f), ColU32(cfg::esp::distance_col), label);
        }

        if (g_state.esp_skeleton) {
            ImU32 skeleton_color = ColU32(cfg::esp::skeleton_col);
            for (int edge = 0; edge < box.bone_edge_count; ++edge) {
                int a = box.bone_edges[edge][0];
                int b = box.bone_edges[edge][1];
                if (a < 0 || b < 0 || a >= box.bone_count || b >= box.bone_count) continue;
                if (!box.bone_valid[a] || !box.bone_valid[b]) continue;
                float ax = box.bones[a][0], ay = box.bones[a][1];
                float bx = box.bones[b][0], by = box.bones[b][1];
                if (!std::isfinite(ax) || !std::isfinite(ay) || !std::isfinite(bx) || !std::isfinite(by)) continue;
                dl->AddLine(ImVec2(ax, ay), ImVec2(bx, by), skeleton_color, thick);
            }
            for (int bone = 0; bone < box.bone_count; ++bone) {
                if (!box.bone_valid[bone]) continue;
                float bx = box.bones[bone][0], by = box.bones[bone][1];
                if (!std::isfinite(bx) || !std::isfinite(by)) continue;
                dl->AddCircleFilled(ImVec2(bx, by), 2.0f, skeleton_color);
            }
        }
    }
}


static constexpr int  kMaxConfigs  = 12;
static constexpr uint8_t kXorKey   = 0xA7;

static const char* kCfgDir_() noexcept {
    static constexpr auto _s = xp::_mk("/storage/emulated/0/xvcen/");
    return _s.d();
}
#define kCfgDir (kCfgDir_())

struct ConfigEntry { char name[64] = {}; };
static ConfigEntry g_configs[kMaxConfigs] = {};
static int         g_configCount    = 0;
static int         g_configToDelete = -1;
static char        g_loadedConfigName[64] = {};
static float       g_cfgLoadAnim[kMaxConfigs] = {};
static int         g_cfgLoadedIdx = -1;

static std::string CfgPath(const char* name) {
    return std::string(kCfgDir) + name + XS(".cfg");
}

static void XorBuf(uint8_t* buf, size_t sz) {
    for (size_t i = 0; i < sz; i++) buf[i] ^= (kXorKey ^ (uint8_t)(i * 0x1D + 0x3B));
}

static void CfgScanDir() {
    mkdir(kCfgDir, 0777);
    g_configCount = 0;
    DIR* d = opendir(kCfgDir);
    if (!d) return;
    struct dirent* e;
    std::vector<std::string> names;
    while ((e = readdir(d)) != nullptr) {
        std::string fn = e->d_name;
        if (fn.size() > 4 && fn.substr(fn.size() - 4) == XS(".cfg"))
            names.push_back(fn.substr(0, fn.size() - 4));
    }
    closedir(d);
    std::sort(names.begin(), names.end());
    for (auto& n : names) {
        if (g_configCount >= kMaxConfigs) break;
        snprintf(g_configs[g_configCount++].name, 64, "%s", n.c_str());
    }
}

static int g_inotifyFd  = -1;
static int g_inotifyWd  = -1;

static void CfgWatchInit() {
    g_inotifyFd = inotify_init1(IN_NONBLOCK);
    if (g_inotifyFd < 0) return;
    mkdir(kCfgDir, 0777);
    g_inotifyWd = inotify_add_watch(g_inotifyFd, kCfgDir,
        IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO | IN_MODIFY);
}

static void CfgWatchFree() {
    if (g_inotifyFd >= 0) {
        if (g_inotifyWd >= 0) inotify_rm_watch(g_inotifyFd, g_inotifyWd);
        close(g_inotifyFd);
        g_inotifyFd = -1;
        g_inotifyWd = -1;
    }
}

static void CfgWatchTick() {
    if (g_inotifyFd < 0) return;
    char buf[512];
    ssize_t n = read(g_inotifyFd, buf, sizeof(buf));
    if (n > 0) CfgScanDir();
}

struct CfgBlob {
    uint32_t magic;
    uint32_t version;
    bool  aim_touch, aim_pos, aim_special;
    int   aim_bone;
    bool  aim_vis_check, aim_draw_fov;
    float aim_fov, aim_smoothness;
    bool  esp_box, esp_name, esp_hp, esp_wall, esp_chams;
    bool  esp_weapon, esp_weapon_icon, esp_tracer, esp_skeleton;
    bool  esp_money, esp_ping, esp_vis_check, esp_fill;
    float esp_thick, esp_stroke, esp_rounding, esp_fill_pct;
    float gun_str, gun_fov, gun_trigger_delay;
    bool  ui_fps, ui_dark_mode, ui_show_sep;
    ImVec4 esp_box_col, esp_box_col_invis, esp_name_col, esp_health_col, esp_distance_col;
    ImVec4 esp_weapon_col, esp_weapon_icon_col, esp_tracer_col, esp_skeleton_col, esp_money_col, esp_ping_col;
    int   esp_box_type;
    float esp_box_rounding;
};

static void ConfigSaveToPath(const std::string& path) {
    CfgBlob s;
    s.magic   = 0x58564345U;
    s.version = 4;
    s.aim_touch   = g_state.aim_touch;   s.aim_pos     = g_state.aim_pos;
    s.aim_special = g_state.aim_special;
    s.aim_bone    = g_state.aim_bone;
    s.aim_vis_check  = cfg::aim::vis_check;
    s.aim_draw_fov   = cfg::aim::draw_fov;
    s.aim_fov        = cfg::aim::fov;
    s.aim_smoothness = cfg::aim::smoothness;
    s.esp_box     = g_state.esp_box;     s.esp_name    = g_state.esp_name;
    s.esp_hp      = g_state.esp_hp;      s.esp_wall    = g_state.esp_wall;
    s.esp_chams   = g_state.esp_chams;
    s.esp_weapon      = g_state.esp_weapon;
    s.esp_weapon_icon = g_state.esp_weapon_icon;
    s.esp_tracer      = g_state.esp_tracer;
    s.esp_skeleton    = g_state.esp_skeleton;
    s.esp_money       = g_state.esp_money;
    s.esp_ping        = g_state.esp_ping;
    s.esp_vis_check   = cfg::esp::vis_check;
    s.esp_fill        = cfg::esp::fill;
    s.esp_thick       = g_state.esp_thick;
    s.esp_stroke      = cfg::esp::stroke;
    s.esp_rounding    = cfg::esp::rounding;
    s.esp_fill_pct    = cfg::esp::fill_pct;
    s.gun_str     = g_state.gun_str;
    s.gun_fov     = g_state.gun_fov;
    s.gun_trigger_delay     = g_state.gun_trigger_delay;
    s.ui_fps      = g_state.ui_fps;      s.ui_dark_mode= g_state.ui_dark_mode;
    s.ui_show_sep = g_state.ui_show_sep;
    s.esp_box_col          = cfg::esp::box_col;
    s.esp_box_col_invis    = cfg::esp::box_col_invis;
    s.esp_name_col         = cfg::esp::name_col;
    s.esp_health_col       = cfg::esp::health_col;
    s.esp_distance_col     = cfg::esp::distance_col;
    s.esp_weapon_col       = cfg::esp::weapon_col;
    s.esp_weapon_icon_col  = cfg::esp::weapon_icon_col;
    s.esp_tracer_col       = cfg::esp::tracer_col;
    s.esp_skeleton_col     = cfg::esp::skeleton_col;
    s.esp_money_col        = cfg::esp::money_col;
    s.esp_ping_col         = cfg::esp::ping_col;
    s.esp_box_type         = cfg::esp::box_type;
    s.esp_box_rounding     = cfg::esp::box_rounding;
    uint8_t buf[sizeof(s)];
    memcpy(buf, &s, sizeof(s));
    XorBuf(buf, sizeof(s));
    std::ofstream f(path, std::ios::binary);
    if (f) { f.write((char*)buf, sizeof(buf)); f.close(); }
}

static void ConfigSave() {
    mkdir(kCfgDir, 0777);
    int idx = 1;
    char baseName[64];
    std::string cfgpath;
    do {
        snprintf(baseName, sizeof(baseName), XS("config%d"), idx++);
        cfgpath = CfgPath(baseName);
    } while (access(cfgpath.c_str(), F_OK) == 0);
    ConfigSaveToPath(cfgpath);
    CfgScanDir();
    ShowToast(XS("Конфиг создан"));
    PlaySound(SND_SUCCESS);
}

static void ConfigUpdate(int idx) {
    if (idx < 0 || idx >= g_configCount) return;
    std::string path = CfgPath(g_configs[idx].name);
    ConfigSaveToPath(path);
    ShowToast(XS("Конфиг сохранён"));
    PlaySound(SND_SUCCESS);
}

static void ConfigLoad(int idx) {
    if (idx < 0 || idx >= g_configCount) return;
    std::string path = CfgPath(g_configs[idx].name);
    std::ifstream f(path, std::ios::binary);
    if (!f) { ShowToast(XS("Файл не найден")); return; }

    CfgBlob s;

    uint8_t buf[sizeof(s)];
    f.read((char*)buf, sizeof(buf));
    size_t got = (size_t)f.gcount();
    f.close();
    if (got != sizeof(buf)) { ShowToast(XS("Несовместимый конфиг")); return; }
    XorBuf(buf, sizeof(s));
    memcpy(&s, buf, sizeof(s));
    if (s.magic != 0x58564345U || s.version != 4) { ShowToast(XS("Старый конфиг — пересохрани")); return; }

    g_state.aim_touch   = s.aim_touch;   g_state.aim_pos     = s.aim_pos;
    g_state.aim_special = s.aim_special;
    g_state.aim_bone    = s.aim_bone;
    cfg::aim::vis_check  = s.aim_vis_check;
    cfg::aim::draw_fov   = s.aim_draw_fov;
    cfg::aim::fov        = s.aim_fov;
    cfg::aim::smoothness = s.aim_smoothness;
    g_state.esp_box     = s.esp_box;     g_state.esp_name    = s.esp_name;
    g_state.esp_hp      = s.esp_hp;      g_state.esp_wall    = s.esp_wall;
    g_state.esp_chams   = s.esp_chams;
    g_state.esp_weapon      = s.esp_weapon;
    g_state.esp_weapon_icon = s.esp_weapon_icon;
    g_state.esp_tracer      = s.esp_tracer;
    g_state.esp_skeleton    = s.esp_skeleton;
    g_state.esp_money       = s.esp_money;
    g_state.esp_ping        = s.esp_ping;
    g_state.esp_thick   = s.esp_thick;
    g_state.gun_str     = s.gun_str;
    g_state.gun_fov     = s.gun_fov;
    g_state.gun_trigger_delay     = s.gun_trigger_delay;
    g_state.ui_fps      = s.ui_fps;      g_state.ui_dark_mode= s.ui_dark_mode;
    g_state.ui_show_sep = s.ui_show_sep;
    cfg::esp::box_col          = s.esp_box_col;
    cfg::esp::box_col_invis    = s.esp_box_col_invis;
    cfg::esp::name_col         = s.esp_name_col;
    cfg::esp::health_col       = s.esp_health_col;
    cfg::esp::distance_col     = s.esp_distance_col;
    cfg::esp::weapon_col       = s.esp_weapon_col;
    cfg::esp::weapon_icon_col  = s.esp_weapon_icon_col;
    cfg::esp::tracer_col       = s.esp_tracer_col;
    cfg::esp::skeleton_col     = s.esp_skeleton_col;
    cfg::esp::money_col        = s.esp_money_col;
    cfg::esp::ping_col         = s.esp_ping_col;
    cfg::esp::box_type         = s.esp_box_type;
    cfg::esp::box_rounding     = s.esp_box_rounding;
    g_darkTheme = g_state.ui_dark_mode;
    snprintf(g_loadedConfigName, sizeof(g_loadedConfigName), "%s", g_configs[idx].name);
    g_cfgLoadedIdx = idx;
    for (int i = 0; i < kMaxConfigs; i++) g_cfgLoadAnim[i] = 0.f;
    ShowToast(XS("Конфиг загружен"));
    PlaySound(SND_SUCCESS);
}

static void ConfigDelete(int idx) {
    if (idx < 0 || idx >= g_configCount) return;
    std::string path = CfgPath(g_configs[idx].name);
    if (strcmp(g_configs[idx].name, g_loadedConfigName) == 0) {
        g_loadedConfigName[0] = '\0';
        g_cfgLoadedIdx = -1;
        for (int i = 0; i < kMaxConfigs; i++) g_cfgLoadAnim[i] = 0.f;
    }
    remove(path.c_str());
    CfgScanDir();
    ShowToast(XS("Конфиг удалён"));
    PlaySound(SND_CLICK);
}

struct ScrollState {
    float off      = 0.f;
    float vel      = 0.f;
    float lastTy   = 0.f;
    bool  drag     = false;
    bool  dragging = false;
    int   axis     = 0;
    float sb_alpha = 0.f;
    float sb_idle  = 0.f;
    float sb_w     = 3.f;
    float sb_y     = 0.f;
    float sb_h     = 48.f;
    bool  sb_hot   = false;
};

static float pill_y   = -1.f;
static float pill_vel =  0.f;

struct TabRect { float sy; };
static TabRect tab_rects[5];

static void ScrollTick(ScrollState& s, bool mIn, bool blocked, float& maxScroll, float dt) {
    auto& io = ImGui::GetIO();

    if (!io.MouseDown[0]) {
        s.drag     = false;
        s.dragging = false;
        s.axis     = 0;
    }

    if (!blocked && mIn && io.MouseDown[0]) {
        if (io.MouseClicked[0]) {
            s.lastTy = io.MousePos.y;
            s.vel    = 0.f;
            s.axis   = 0;
        }

        if (s.axis == 0) {
            float dx = fabsf(io.MousePos.x - io.MouseClickedPos[0].x);
            float dy = fabsf(io.MousePos.y - io.MouseClickedPos[0].y);
            if (dx > 22.f || dy > 22.f)
                s.axis = (dy >= dx) ? 1 : 2;
            s.lastTy = io.MousePos.y;
        }

        if (s.axis == 1) {
            if (!s.drag) {
                s.drag     = true;
                s.dragging = true;
                s.lastTy   = io.MousePos.y;
                s.vel      = 0.f;
            }
            float dy = io.MousePos.y - s.lastTy;
            s.lastTy = io.MousePos.y;
            s.off    = ImClamp(s.off - dy, 0.f, maxScroll);
            float targetV = -dy * 18.f;
            s.vel = s.vel * 0.8f + targetV * 0.2f;
        }

        if (s.axis == 2) {
            s.lastTy = io.MousePos.y;
            s.vel    = 0.f;
        }
    }

    if (mIn && !blocked && io.MouseWheel != 0.f) {
        s.vel = 0.f;
        s.off = ImClamp(s.off - io.MouseWheel * 50.f, 0.f, maxScroll);
    }

    if (!s.drag && fabsf(s.vel) > 0.3f) {
        s.off += s.vel * dt;
        s.vel *= 0.88f;
    }

    s.off = ImClamp(s.off, 0.f, maxScroll);
    if (s.off <= 0.f || s.off >= maxScroll) s.vel = 0.f;

    bool isScrolling = s.drag || fabsf(s.vel) > 0.5f;
    if (isScrolling) s.sb_idle = 0.f;
    else s.sb_idle += dt;
    float sbTarget = (isScrolling || s.sb_idle < 0.8f) ? 1.f : 0.f;
    float sbSpd = sbTarget > s.sb_alpha ? 6.f : 3.5f;
    s.sb_alpha += (sbTarget - s.sb_alpha) * sbSpd * dt;
    s.sb_alpha = ImClamp(s.sb_alpha, 0.f, 1.f);
}

static ScrollState g_scrollMain;
static ScrollState g_scrollPop;

static inline bool IsScrollDragging() { return g_scrollMain.dragging || g_scrollPop.dragging; }

static ImVec2 g_popOpenClickPos = {-9999.f, -9999.f};

static const float WW_MIN = 840.f;
static const float WH_MIN = 720.f;

struct WindowState {
    ImVec2 pos              = {-1.f, -1.f};
    float  w                = WW_MIN;
    float  h                = WH_MIN;
    bool   dragging         = false;
    ImVec2 touchStart       = {0, 0};
    ImVec2 posStart         = {0, 0};
    bool   resizing         = false;
    ImVec2 resizeTouchStart = {0, 0};
    ImVec2 sizeStart        = {WW_MIN, WH_MIN};
};
static WindowState g_win;

struct Sheet {
    bool   visible    = false;
    float  anim       = 0.f;
    float  vel        = 0.f;
    bool   closing    = false;
    int    openFrames = 0;
    char   title[64]  = {};
    int    type       = 0;
    bool*  boolP      = nullptr;
    float* animP      = nullptr;
    float* slP        = nullptr;
    float  slMin      = 0;
    float  slMax      = 1;
    const char* slFmt = "%.1f";
};
static Sheet g_sheet;

void SheetOpen(const char* title, int type, bool* bp = nullptr, float* ap = nullptr,
               float* sp = nullptr, float mn = 0, float mx = 1, const char* fmt = "%.1f") {
    if (g_sheet.visible) return;
    snprintf(g_sheet.title, sizeof(g_sheet.title), "%s", title);
    g_sheet.type    = type; g_sheet.boolP = bp; g_sheet.animP = ap;
    g_sheet.slP     = sp;   g_sheet.slMin = mn; g_sheet.slMax = mx; g_sheet.slFmt = fmt;
    g_sheet.visible = true;  g_sheet.anim  = 0.f; g_sheet.vel = 0.f;
    g_sheet.closing = false; g_sheet.openFrames = 0;
}

void SheetClose() {
    if (!g_sheet.visible || g_sheet.closing) return;
    if (g_sheet.openFrames < 4) return;
    g_sheet.closing = true;
}

struct Popover {
    bool  visible    = false;
    bool  closing    = false;
    float anim       = 0.f;
    float vel        = 0.f;
    int   openFrames = 0;
    int   sectionId  = 0;
    char  title[64]  = {};
};
static Popover g_pop;

static ImVec4* g_colP = nullptr;
static bool    g_palSelReset = false;


static const ImVec4 kColorPalette[] = {
    {1.00f, 0.20f, 0.20f, 1.f}, {1.00f, 0.50f, 0.50f, 1.f}, {0.80f, 0.05f, 0.05f, 1.f},
    {1.00f, 0.60f, 0.10f, 1.f}, {1.00f, 0.75f, 0.30f, 1.f}, {0.90f, 0.40f, 0.00f, 1.f},
    {1.00f, 0.95f, 0.10f, 1.f}, {1.00f, 0.85f, 0.50f, 1.f}, {0.85f, 0.75f, 0.00f, 1.f},
    {0.20f, 0.85f, 0.35f, 1.f}, {0.50f, 1.00f, 0.50f, 1.f}, {0.05f, 0.55f, 0.15f, 1.f},
    {0.10f, 0.90f, 0.90f, 1.f}, {0.40f, 0.85f, 1.00f, 1.f}, {0.00f, 0.60f, 0.70f, 1.f},
    {0.10f, 0.50f, 1.00f, 1.f}, {0.50f, 0.70f, 1.00f, 1.f}, {0.00f, 0.25f, 0.75f, 1.f},
    {0.65f, 0.20f, 1.00f, 1.f}, {0.80f, 0.55f, 1.00f, 1.f}, {0.45f, 0.05f, 0.70f, 1.f},
    {1.00f, 0.30f, 0.80f, 1.f}, {1.00f, 0.60f, 0.90f, 1.f}, {0.80f, 0.05f, 0.50f, 1.f},
    {1.00f, 1.00f, 1.00f, 1.f}, {0.70f, 0.70f, 0.70f, 1.f}, {0.35f, 0.35f, 0.35f, 1.f},
    {0.12f, 0.12f, 0.12f, 1.f},
};
static constexpr int kPaletteCount = (int)(sizeof(kColorPalette)/sizeof(kColorPalette[0]));

void PopoverOpenColor(const char* title, ImVec4* cp) {
    if (g_pop.visible) return;
    snprintf(g_pop.title, sizeof(g_pop.title), "%s", title);
    g_pop.sectionId  = 4;
    g_pop.visible    = true;
    g_pop.closing    = false;
    g_pop.anim       = 0.f;
    g_pop.vel        = 0.f;
    g_pop.openFrames = 0;
    g_scrollPop      = {};
    g_colP = cp;
    g_palSelReset = true;
    g_popOpenClickPos = ImGui::GetIO().MousePos;
    auto& _io = ImGui::GetIO();
    Blur::Freeze((int)_io.DisplaySize.x, (int)_io.DisplaySize.y,
                 (int)g_win.pos.x, (int)g_win.pos.y,
                 (int)g_win.w,     (int)g_win.h);
}

void PopoverOpen(const char* title, int sid) {
    if (g_pop.visible) return;
    snprintf(g_pop.title, sizeof(g_pop.title), "%s", title);
    g_pop.sectionId  = sid;
    g_pop.visible    = true;
    g_pop.closing    = false;
    g_pop.anim       = 0.f;
    g_pop.vel        = 0.f;
    g_pop.openFrames = 0;
    g_scrollPop      = {};
    auto& _io = ImGui::GetIO();
    Blur::Freeze((int)_io.DisplaySize.x, (int)_io.DisplaySize.y,
                 (int)g_win.pos.x, (int)g_win.pos.y,
                 (int)g_win.w,     (int)g_win.h);
}

void PopoverClose() {
    if (!g_pop.visible || g_pop.closing) return;
    if (g_pop.openFrames < 1) return;
    g_pop.closing = true;
    g_input.touchConsumed = true;
}

static void DrawToggle(ImDrawList* dl, float ax, float cy, float t) {
    const float tW = 72.f, tH = 42.f, tR = tH * 0.5f, kR = tR - 3.5f;
    float tx = ax, ty = cy - tH * 0.5f;
    dl->AddRectFilled({tx, ty}, {tx + tW, ty + tH}, C::Mix(C::TrkOff(), C::Acc(), t), tR);
    float kx = Lerpf(tx + tR + 2.f, tx + tW - tR - 2.f, t);
    dl->AddCircleFilled({kx + 0.5f, cy + 1.5f}, kR + 1.f, IM_COL32(0, 0, 0, 35));
    dl->AddCircleFilled({kx, cy}, kR, IM_COL32(255, 255, 255, 255));
}

static void RenderToggleRowVisuals(ImDrawList* dl, ImFont* fn, float fs,
                                    float rowX, float rowY, float rowW,
                                    const char* lbl, float animT, float alpha,
                                    bool showSep, bool last) {
    const float inset = Layout::Inset, padX = Layout::PadX;
    float cX = rowX + inset, cW = rowW - inset * 2.f;
    const float rowH = Layout::RowH;
    float textX    = cX + padX;
    float textMaxX = cX + cW - 72.f - padX - 16.f;
    float textY    = rowY + (rowH - fs * 1.15f) * 0.5f;

    dl->PushClipRect({textX, rowY}, {textMaxX, rowY + rowH}, true);
    dl->AddText(fn, fs * 1.15f, {textX, textY}, C::UA(C::Txt(), alpha), lbl);
    dl->PopClipRect();

    DrawToggle(dl, cX + cW - 72.f - padX, rowY + rowH * 0.5f, EaseInOut(animT));

    if (!last && showSep)
        dl->AddLine({cX + padX, rowY + rowH - 0.5f},
                    {cX + cW - padX, rowY + rowH - 0.5f}, C::UA(C::Sep(), alpha), 0.8f);
}

static void RenderSliderVisuals(ImDrawList* dl, ImFont* fn, float fs,
                                 float rowX, float rowY, float rowW, float rowH,
                                 const char* lbl, const char* fmt, float v, float animPos,
                                 bool active, float alpha, bool showSep, bool last) {
    const float inset = Layout::Inset, padX = Layout::PadX;
    const float tH = 11.f, kR = 22.f, padH = 38.f;
    float cX = rowX + inset, cW = rowW - inset * 2.f;

    char valBuf[24]; snprintf(valBuf, 24, fmt, v);
    auto vsz   = fn->CalcTextSizeA(fs, FLT_MAX, 0, valBuf);
    float textY = rowY + padH * 0.5f;

    float sX = cX + padX + kR, sW = cW - padX * 2.f - kR * 2.f;
    float tY = rowY + rowH - padH;
    float kx = sX + animPos * sW;

    dl->PushClipRect({cX + padX, rowY}, {cX + cW - vsz.x - padX * 2.f, rowY + rowH * 0.55f}, true);
    dl->AddText(fn, fs * 1.15f, {cX + padX, textY}, C::UA(C::Txt(), alpha), lbl);
    dl->PopClipRect();
    dl->AddText(fn, fs * 1.15f, {cX + cW - vsz.x - padX, textY}, C::UA(C::Acc(), alpha), valBuf);

    dl->AddRectFilled({sX, tY - tH * 0.5f}, {sX + sW, tY + tH * 0.5f}, C::UA(C::TrkOff(), alpha), tH);
    dl->AddRectFilled({sX, tY - tH * 0.5f}, {kx,      tY + tH * 0.5f}, C::UA(C::Acc(),    alpha), tH);

    const float knobR = 16.f;
    dl->AddCircleFilled({kx + 0.5f, tY + 1.5f}, knobR + 1.f, IM_COL32(0, 0, 0, int(30 * alpha)));
    dl->AddCircleFilled({kx, tY}, knobR, IM_COL32(255, 255, 255, int(255 * alpha)));
    if (active)
        dl->AddCircle({kx, tY}, knobR + 4.f, C::UA(C::Light::Acc, 0.85f * alpha), 48, 3.f);

    if (!last && showSep)
        dl->AddLine({cX + padX, rowY + rowH - 0.5f},
                    {cX + cW - padX, rowY + rowH - 0.5f}, C::UA(C::Sep(), alpha), 0.8f);
}

bool ToggleRow(const char* id, const char* lbl, bool* v, float& anim,
               bool last = false, bool first = false) {
    auto* dl  = ImGui::GetWindowDrawList();
    float avW = ImGui::GetContentRegionAvail().x;
    const float rowH = Layout::RowH;
    auto pos = ImGui::GetCursorScreenPos();

    ImGui::InvisibleButton(id, {avW, rowH});
    bool popBlocking = (g_pop.visible && !g_pop.closing) || g_sheet.visible;

    bool triggered = WasTappedHere() && !popBlocking && !IsScrollDragging() && !g_input.touchConsumed;
    if (triggered) { *v=!*v; char _b[160]; snprintf(_b,sizeof(_b),XS("%s|%s"),lbl,*v?XS("ON"):XS("OFF")); ShowToast(_b); PlaySound(SND_CLICK); }

    RenderToggleRowVisuals(dl, ImGui::GetFont(), ImGui::GetFontSize(),
                           pos.x, pos.y, avW, lbl, anim, 1.f,
                           g_state.ui_show_sep, last);
    return triggered;
}

static void TickSliderAnim(AppState::SliderAnim& anim, float target, bool act, float dt) {
    if (anim.pos < 0.f) { anim.pos = target; anim.vel = 0.f; }
    const float stiff = act ? 900.f : 280.f;
    const float damp  = act ?  60.f :  28.f;
    float force = stiff * (target - anim.pos) - damp * anim.vel;
    anim.vel += force * dt;
    anim.pos += anim.vel * dt;
    anim.pos  = ImClamp(anim.pos, 0.f, 1.f);
    if (fabsf(target - anim.pos) < 0.0002f && fabsf(anim.vel) < 0.001f) {
        anim.pos = target; anim.vel = 0.f;
    }
}

bool SliderRow(const char* id, const char* lbl, float* v, float mn, float mx,
               const char* fmt, bool last, bool first,
               AppState::SliderAnim& anim, float dt) {
    auto* dl  = ImGui::GetWindowDrawList();
    auto& io  = ImGui::GetIO();
    float avW = ImGui::GetContentRegionAvail().x;
    const float rowH = Layout::SliderH, tH = 11.f, kR = 22.f, padH = 38.f;
    const float inset = Layout::Inset, padX = Layout::PadX;
    auto pos = ImGui::GetCursorScreenPos();

    ImGui::InvisibleButton(id, {avW, rowH});
    bool itemActive = ImGui::IsItemActive();

    int scrollAxis = g_scrollMain.axis;
    bool scrollVertical = g_scrollMain.dragging || (itemActive && scrollAxis == 1);
    bool axisUndecided  = itemActive && scrollAxis == 0 && io.MouseDown[0];

    bool act = itemActive && !scrollVertical && !axisUndecided;

    if (act)
        *v = mn + Clamp01((io.MousePos.x - (pos.x + inset + padX + kR)) / (avW - inset * 2.f - padX * 2.f - kR * 2.f)) * (mx - mn);

    float target = Clamp01((*v - mn) / (mx - mn));

    TickSliderAnim(anim, target, act, dt);

    RenderSliderVisuals(dl, ImGui::GetFont(), ImGui::GetFontSize(),
                        pos.x, pos.y, avW, rowH,
                        lbl, fmt, *v, anim.pos, act, 1.f,
                        g_state.ui_show_sep, last);
    return act;
}

void CardBg(float h, ImDrawFlags flags = ImDrawFlags_RoundCornersAll) {
    auto* dl  = ImGui::GetWindowDrawList();
    auto  p0  = ImGui::GetCursorScreenPos();
    float w   = ImGui::GetContentRegionAvail().x;
    const float inset = Layout::Inset;
    ImVec2 bMin = {p0.x + inset, p0.y};
    ImVec2 bMax = {p0.x + w - inset, p0.y + h};
    dl->AddRectFilled(bMin, bMax, C::U(C::Card()), R::Card, flags);
    if (g_state.ui_show_sep)
        dl->AddRect(bMin, bMax, C::U(C::Sep()), R::Card, flags, 1.2f);
}

static bool TickSlideAnim(float& anim, float& vel, bool closing, float dt);

void SHdr(const char* t, float top = 18.f) {
    ImGui::Dummy({1.f, top});
    if (t && t[0]) {
        auto*  dl  = ImGui::GetWindowDrawList();
        auto*  fn  = ImGui::GetFont();
        float  fs  = ImGui::GetFontSize() * 1.15f;
        auto   pos = ImGui::GetCursorScreenPos();
        auto   tsz = fn->CalcTextSizeA(fs, FLT_MAX, 0, t);
        dl->AddText(fn, fs, {pos.x + 32.f, pos.y}, C::U(C::Dim()), t);
        ImGui::Dummy({1.f, tsz.y});
    }
    ImGui::Dummy({1.f, 8.f});
}

static void DrawToast(float dt) {
    if (!g_toast.visible) return;
    g_toast.life += dt;

    if (g_toast.hasNext && g_toast.textA < 0.04f) {
        snprintf(g_toast.msg, sizeof(g_toast.msg), "%s", g_toast.nextMsg);
        g_toast.hasNext = false;
        g_toast.barW = 1.f; g_toast.barVel = 0.f;
    }

    if (g_toast.life > 2.6f && !g_toast.closing && !g_toast.hasNext) g_toast.closing = true;

    if (!TickSlideAnim(g_toast.slidePos, g_toast.slideVel, g_toast.closing, dt)) {
        g_toast.slidePos = 0.f; g_toast.slideVel = 0.f; g_toast.visible = false; return;
    }

    float textTgt = (g_toast.hasNext ? 0.f : 1.f);
    g_toast.textA += (textTgt - g_toast.textA) * 14.f * dt;
    g_toast.textA = ImClamp(g_toast.textA, 0.f, 1.f);

    { float bt = g_toast.hasNext ? 1.f : ImMax(0.f, 1.f - g_toast.life / 2.6f);
      float f = 220.f * (bt - g_toast.barW) - 30.f * g_toast.barVel;
      g_toast.barVel += f * dt; g_toast.barW += g_toast.barVel * dt;
      g_toast.barW = ImClamp(g_toast.barW, 0.f, 1.f); }

    auto* fg = ImGui::GetForegroundDrawList();
    auto* fn = ImGui::GetFont();
    float fs  = ImGui::GetFontSize() * 1.55f;
    float scrW = 0.f, scrH = 0.f;
    VisibleScreen(scrW, scrH);

    const char* sep = strchr(g_toast.msg, 124);
    char np[128] = {}, sp[8] = {}; bool isOn = false;
    if (sep) {
        snprintf(np, sizeof(np), "%.*s", (int)(sep - g_toast.msg), g_toast.msg);
        snprintf(sp, sizeof(sp), "%s", sep + 1);
        isOn = (sp[0] == 'O' && sp[1] == 'N' && sp[2] == 0);
    } else {
        snprintf(np, sizeof(np), "%s", g_toast.msg);
    }

    auto nsz = fn->CalcTextSizeA(fs, FLT_MAX, 0, np);
    auto ssz = fn->CalcTextSizeA(fs, FLT_MAX, 0, XS(" - "));
    auto asz = fn->CalcTextSizeA(fs, FLT_MAX, 0, sp);

    float padH = 28.f, padV = 20.f;
    float tWTarget = padH + nsz.x + (sep ? ssz.x + asz.x : 0.f) + padH;
    float tH = nsz.y + padV * 2.f + 10.f;

    if (g_toast.widthAnim < 8.f) { g_toast.widthAnim = tWTarget; g_toast.widthVel = 0.f; }
    {
        float dw = tWTarget - g_toast.widthAnim;
        g_toast.widthVel += (dw * 220.f - g_toast.widthVel * 28.f) * dt;
        g_toast.widthAnim += g_toast.widthVel * dt;
        if (fabsf(dw) < 0.6f && fabsf(g_toast.widthVel) < 4.f) {
            g_toast.widthAnim = tWTarget; g_toast.widthVel = 0.f;
        }
    }
    float tW = g_toast.widthAnim;

    float ease  = EaseInOut(ImClamp(g_toast.slidePos, 0.f, 1.f));
    float offY  = (1.f - ease) * (-(tH + 20.f));
    float alpha = ease;

    float tx = scrW - tW - 16.f;
    float ty = 16.f + offY;

    ImVec4 card = C::Card(), txt = C::Txt(), dim = C::Dim(), acc = C::Acc();

    fg->AddRectFilled({tx + 2.f, ty + 4.f}, {tx + tW + 2.f, ty + tH + 4.f},
        IM_COL32(0, 0, 0, int(26 * alpha)), 26.f);
    fg->AddRectFilled({tx, ty}, {tx + tW, ty + tH},
        IM_COL32(int(card.x*255), int(card.y*255), int(card.z*255), int(alpha * 245)), 26.f);
    fg->AddRect({tx, ty}, {tx + tW, ty + tH},
        C::UA(acc, 0.32f * alpha), 26.f, 0, 1.3f);

    fg->PushClipRect({tx + 1.f, ty + 1.f}, {tx + tW - 1.f, ty + tH - 1.f}, true);

    float cx0 = tx + padH;
    float textY = ty + padV;
    float ta = g_toast.textA * alpha;

    fg->AddText(fn, fs, {cx0, textY},
        IM_COL32(int(txt.x*255), int(txt.y*255), int(txt.z*255), int(ta*255)), np);
    cx0 += nsz.x;

    if (sep) {
        fg->AddText(fn, fs, {cx0, textY},
            IM_COL32(int(dim.x*255), int(dim.y*255), int(dim.z*255), int(ta*170)), XS(" - "));
        cx0 += ssz.x;
        ImVec4 stCol = isOn ? ImVec4{0.196f, 0.843f, 0.294f, 1.f} : ImVec4{1.f, 0.231f, 0.188f, 1.f};
        fg->AddText(fn, fs, {cx0, textY},
            IM_COL32(int(stCol.x*255), int(stCol.y*255), int(stCol.z*255), int(ta*255)), sp);
    }

    {
        float bH  = 10.f, bR = 5.f;
        float bY  = ty + tH - 8.f - bH;
        float bx0 = tx + 22.f, bx1 = tx + tW - 22.f;
        float fx1 = bx0 + g_toast.barW * (bx1 - bx0);
        int ar = int(acc.x*255), ag = int(acc.y*255), ab = int(acc.z*255);
        if (fx1 > bx0 + bR * 2.f)
            fg->AddRectFilled({bx0, bY}, {fx1, bY + bH},
                IM_COL32(ar, ag, ab, int(alpha * 215)), bR);
    }

    fg->PopClipRect();
}

static float wm_ring   = 2.f;
static float wm_fps_sm = 0.f;
static float wm_w      = 0.f;
static float wm_wvel   = 0.f;
static bool  menu_open = true;

void DrawWatermark(float dt) {
    auto& io  = ImGui::GetIO();
    auto* fn  = ImGui::GetFont();
    auto* fg  = ImGui::GetForegroundDrawList();

    float rawFps = overlay_fps();
    if (rawFps < 1.f) rawFps = io.Framerate;
    const float rates[] = {60.f, 90.f, 120.f, 144.f, 165.f, 180.f, 240.f};
    float nearest = rawFps, nd = 1e9f;
    for (float r : rates) {
        float d = fabsf(rawFps - r);
        if (d < nd) { nd = d; nearest = r; }
    }
    if (nd <= 8.f) rawFps = nearest;
    wm_fps_sm += (rawFps - wm_fps_sm) * 8.f * dt;
    if (wm_fps_sm < 1.f) wm_fps_sm = rawFps;
    wm_ring   += dt * 3.5f;

    ImVec4 acc   = C::Acc();
    ImVec4 cardV = C::Card();
    ImU32 bgCol  = C::U(cardV);
    ImU32 brdCol = C::UA(acc, 0.6f);
    ImU32 nmCol  = C::U(C::Txt());

    const float fs = 40.f, pad = 22.f, bR = 46.f;
    const char* name = XS("xvcen");

    auto  nSz = fn->CalcTextSizeA(fs, FLT_MAX, 0, name);
    char  fpsBuf[12]; snprintf(fpsBuf, 12, "%.0f", wm_fps_sm);
    auto  fSz = fn->CalcTextSizeA(fs, FLT_MAX, 0, fpsBuf);

    TickSlideAnim(wm_w, wm_wvel, !g_state.ui_fps, dt);
    float fpsA = EaseInOut(wm_w);
    float fpsSection = fpsA * (pad * 0.7f + 1.5f + pad * 0.7f + fSz.x);
    float bW = pad + nSz.x + pad + fpsSection;
    float bH = nSz.y + pad;
    const float bX = 16.f, bY = 16.f;

    bool in = (io.MousePos.x >= bX && io.MousePos.x <= bX + bW &&
               io.MousePos.y >= bY && io.MousePos.y <= bY + bH);
    if (in && io.MouseClicked[0]) { menu_open = !menu_open; wm_ring = 0.f; }

    if (wm_ring < 1.f) {
        float r = EaseOut3(wm_ring), ex = r * 22.f;
        fg->AddRectFilled({bX - ex, bY - ex}, {bX + bW + ex, bY + bH + ex},
            C::UA(acc, 0.18f * (1.f - r)), bR + ex);
    }

    fg->AddRectFilled({bX + 2.f, bY + 3.f}, {bX + bW + 2.f, bY + bH + 3.f},
        IM_COL32(0, 0, 0, 20), bR);
    fg->AddRectFilled({bX, bY}, {bX + bW, bY + bH}, bgCol, bR);
    fg->AddRect      ({bX, bY}, {bX + bW, bY + bH}, brdCol, bR, 0, 1.5f);

    float textY = bY + (bH - nSz.y) * 0.5f;
    fg->AddText(fn, fs, {bX + pad, textY}, nmCol, name);

    if (fpsA > 0.01f) {
        float sx = bX + pad + nSz.x + pad * 0.7f;
        fg->AddLine({sx, bY + bH * 0.18f}, {sx, bY + bH * 0.82f}, C::UA(C::Acc(), fpsA * 0.35f), 1.5f);
        fg->AddText(fn, fs, {sx + pad * 0.7f, textY}, C::UA(C::Acc(), fpsA), fpsBuf);
    }
}

bool CollapsibleHeader(const char* id, const char* lbl, int secId = 0) {
    auto* dl  = ImGui::GetWindowDrawList();
    float avW = ImGui::GetContentRegionAvail().x;
    const float rowH = Layout::RowH, inset = Layout::Inset, padX = Layout::PadX;
    auto pos = ImGui::GetCursorScreenPos();

    dl->AddRectFilled({pos.x + inset, pos.y}, {pos.x + avW - inset, pos.y + rowH}, C::U(C::Card()), R::Card);
    if (g_state.ui_show_sep)
        dl->AddRect({pos.x + inset, pos.y}, {pos.x + avW - inset, pos.y + rowH}, C::U(C::Sep()), R::Card, 0, 1.2f);

    ImGui::InvisibleButton(id, {avW, rowH});
    bool clicked = WasTappedHere() && !IsScrollDragging() && !g_input.touchConsumed;

    float fs    = ImGui::GetFontSize() * 1.15f;
    float textY = pos.y + (rowH - fs) * 0.5f;
    dl->AddText(ImGui::GetFont(), fs, {pos.x + inset + padX, textY}, C::U(C::Txt()), lbl);

    const float cBtnR = 22.f;
    float cx2 = pos.x + avW - inset - cBtnR - padX * 0.5f, cy2 = pos.y + rowH * 0.5f;
    dl->AddCircleFilled({cx2 + 0.5f, cy2 + 1.5f}, cBtnR + 1.f, IM_COL32(0, 0, 0, 28), 48);
    dl->AddCircleFilled({cx2, cy2}, cBtnR, C::U(C::Acc()), 48);
    {
        float t = EaseInOut(g_themeT);
        ImU32 ringCol = IM_COL32(
            int(Lerpf(0,   255, t)),
            int(Lerpf(50,  255, t)),
            int(Lerpf(170, 255, t)),
            int(Lerpf(130,  60, t))
        );
        dl->AddCircle({cx2, cy2}, cBtnR + 2.5f, ringCol, 48, 1.5f);
    }

    float chs = 10.f, thk = 3.f;
    dl->AddLine({cx2 - chs * 0.45f, cy2 - chs * 0.8f}, {cx2 + chs * 0.55f, cy2}, IM_COL32(255, 255, 255, 250), thk);
    dl->AddLine({cx2 + chs * 0.55f, cy2}, {cx2 - chs * 0.45f, cy2 + chs * 0.8f}, IM_COL32(255, 255, 255, 250), thk);

    if (clicked && !(g_pop.visible && !g_pop.closing)) {
        PopoverOpen(lbl, secId);
        g_popOpenClickPos = ImGui::GetIO().MousePos;
        PlaySound(SND_CLICK);
    }
    return clicked;
}

static bool TickSlideAnim(float& anim, float& vel, bool closing, float dt) {
    if (dt > 0.032f) dt = 0.032f;
    const float stiff = 120.f, damp = 22.f;
    if (!closing) {
        float acc = (1.f - anim) * stiff - vel * damp;
        vel  += acc * dt;
        anim += vel * dt;
        if (anim >= 0.9998f) { anim = 1.f; vel = 0.f; }
        anim = ImClamp(anim, 0.f, 1.f);
        return true;
    } else {
        float acc = (0.f - anim) * stiff - vel * damp;
        vel  += acc * dt;
        anim += vel * dt;
        if (anim <= 0.001f) { anim = 0.f; vel = 0.f; return false; }
        anim = ImClamp(anim, 0.f, 1.f);
        return true;
    }
}

static void DrawExitButtons(ImDrawList* fg, ImFont* fn, float fs, float alpha,
                             float b1X, float b2X, float bW, float bY, float btnH,
                             bool blocked,
                             const ImVec2& mousePos, const ImVec2& clickedPos,
                             bool mouseReleased,
                             bool isSheet) {
    fg->AddRectFilled({b1X, bY}, {b1X + bW, bY + btnH}, C::UA(C::Txt(), 0.08f * alpha), R::Btn);
    fg->AddRectFilled({b2X, bY}, {b2X + bW, bY + btnH}, C::UA(C::Red(), alpha), R::Btn);
    if (g_state.ui_show_sep) {
        fg->AddRect({b1X, bY}, {b1X + bW, bY + btnH}, C::UA(C::Txt(), 0.22f * alpha), R::Btn, 0, 1.2f);
        fg->AddRect({b2X, bY}, {b2X + bW, bY + btnH}, C::UA(C::Txt(), 0.22f * alpha), R::Btn, 0, 1.2f);
    }
    auto c1Sz = fn->CalcTextSizeA(fs * 1.15f, FLT_MAX, 0, XS("Отмена"));
    auto c2Sz = fn->CalcTextSizeA(fs * 1.15f, FLT_MAX, 0, XS("Выйти"));
    fg->AddText(fn, fs * 1.15f, {b1X + bW * 0.5f - c1Sz.x * 0.5f, bY + (btnH - fs * 1.15f) * 0.5f},
        C::UA(C::Txt(), alpha), XS("Отмена"));
    fg->AddText(fn, fs * 1.15f, {b2X + bW * 0.5f - c2Sz.x * 0.5f, bY + (btnH - fs * 1.15f) * 0.5f},
        IM_COL32(255, 255, 255, int(255 * alpha)), XS("Выйти"));
    if (!blocked && mouseReleased) {
        bool dH1 = clickedPos.x >= b1X && clickedPos.x <= b1X+bW && clickedPos.y >= bY && clickedPos.y <= bY+btnH;
        bool dH2 = clickedPos.x >= b2X && clickedPos.x <= b2X+bW && clickedPos.y >= bY && clickedPos.y <= bY+btnH;
        bool uH1 = mousePos.x >= b1X && mousePos.x <= b1X+bW && mousePos.y >= bY && mousePos.y <= bY+btnH;
        bool uH2 = mousePos.x >= b2X && mousePos.x <= b2X+bW && mousePos.y >= bY && mousePos.y <= bY+btnH;
        if (uH1 && dH1) { if (isSheet) SheetClose(); else PopoverClose(); }
        if (uH2 && dH2) main_thread_flag = false;
    }
}

void DrawSheet(float dt, ImVec2 menuPos, float WW, float WH) {
    if (!g_sheet.visible) return;

    auto* fg = ImGui::GetForegroundDrawList();
    auto& io = ImGui::GetIO();
    auto* fn = ImGui::GetFont();
    float fs = ImGui::GetFontSize();

    g_sheet.openFrames++;

    if (!TickSlideAnim(g_sheet.anim, g_sheet.vel, g_sheet.closing, dt)) {
        g_sheet.anim = 0.f; g_sheet.vel = 0.f;
        g_sheet.visible = false; g_sheet.closing = false;
        return;
    }

    float easeT  = EaseInOut(g_sheet.anim);
    float sheetH = (g_sheet.type == 2) ? 280.f : 380.f;
    float sheetW = WW - 24.f;
    float sheetX = menuPos.x + (WW - sheetW) * 0.5f;
    float sheetYFull   = (g_sheet.type == 2)
        ? menuPos.y + (WH - sheetH) * 0.5f
        : menuPos.y + WH - sheetH - 16.f;
    float sheetYHidden = menuPos.y + WH;
    float sheetY = Lerpf(sheetYHidden, sheetYFull, easeT);

    {
        const float fgR = R::Sheet;
        int ba = int(Lerpf(0.f, 180.f, easeT));
        fg->AddRectFilled(menuPos, {menuPos.x + WW, menuPos.y + WH}, C::UA(C::Bg(), ba / 255.f), fgR);
        fg->AddRectFilled({menuPos.x + 2, menuPos.y + 2}, {menuPos.x + WW - 2, menuPos.y + WH - 2}, C::UA(C::Bg(), ba * 0.6f / 255.f), fgR - 1);
        fg->AddRectFilled({menuPos.x + 4, menuPos.y + 4}, {menuPos.x + WW - 4, menuPos.y + WH - 4}, C::UA(C::Bg(), ba * 0.4f / 255.f), fgR - 2);
        fg->AddRectFilled(menuPos, {menuPos.x + WW, menuPos.y + WH}, IM_COL32(20, 20, 40, int(Lerpf(0.f, 60.f, easeT))), fgR);
        fg->AddRect(menuPos, {menuPos.x + WW, menuPos.y + WH}, IM_COL32(255, 255, 255, int(Lerpf(0.f, 40.f, easeT))), fgR, 0, 1.5f);
    }

    fg->PushClipRect({menuPos.x - 40.f, menuPos.y + 1.f}, {menuPos.x + WW + 40.f, menuPos.y + WH - 1.f}, true);

    fg->AddRectFilled({sheetX, sheetY}, {sheetX + sheetW, sheetY + sheetH}, C::U(C::Card()), R::Sheet);

    {
        float dhW = 60.f, dhH = 6.f;
        float dhX = sheetX + sheetW * 0.5f - dhW * 0.5f, dhY = sheetY + 12.f;
        fg->AddRectFilled({dhX, dhY}, {dhX + dhW, dhY + dhH}, C::UA(C::Dim(), 0.4f * easeT), dhH * 0.5f);
        bool inHandle = io.MousePos.x >= sheetX && io.MousePos.x <= sheetX + sheetW
                     && io.MousePos.y >= sheetY  && io.MousePos.y <= sheetY + 44.f;
        if (g_sheet.type != 2) {
            if (!g_sheet.closing && inHandle && io.MouseDown[0] && io.MouseDelta.y > 3.f)
                SheetClose();
        } else {
            if (io.MouseClicked[0] && inHandle) {
                g_win.dragging   = true;
                g_win.touchStart = io.MousePos;
                g_win.posStart   = g_win.pos;
            }
        }
    }

    float titleFS = fs * 1.45f;
    {
        auto tsz   = fn->CalcTextSizeA(titleFS, FLT_MAX, 0, g_sheet.title);
        fg->AddText(fn, titleFS, {sheetX + sheetW * 0.5f - tsz.x * 0.5f, sheetY + 32.f}, C::U(C::Txt()), g_sheet.title);
    }

    if (g_sheet.type != 2) {
        float cR  = 17.f, cx2 = sheetX + sheetW - cR - 16.f, cy2 = sheetY + 32.f + titleFS * 0.5f;
        fg->AddCircleFilled({cx2, cy2}, cR, C::U(C::Sep()), 32);
        float cs = 6.f;
        fg->AddLine({cx2 - cs, cy2 - cs}, {cx2 + cs, cy2 + cs}, C::U(C::Dim()), 2.4f);
        fg->AddLine({cx2 + cs, cy2 - cs}, {cx2 - cs, cy2 + cs}, C::U(C::Dim()), 2.4f);
        if (!g_sheet.closing && io.MouseClicked[0] &&
            fabsf(io.MousePos.x - cx2) < cR + 8.f && fabsf(io.MousePos.y - cy2) < cR + 8.f)
            SheetClose();
    }

    {
        float lineY = sheetY + 32.f + titleFS + 14.f;
        fg->AddLine({sheetX + 18.f, lineY}, {sheetX + sheetW - 18.f, lineY}, C::U(C::Sep()), 0.8f);
    }

    float cY = sheetY + 32.f + titleFS + 26.f;
    float padX = 28.f;

    if (g_sheet.type == 0 && g_sheet.boolP) {
        bool*  v  = g_sheet.boolP;
        float* a  = g_sheet.animP;
        float  t  = EaseInOut(*a);
        float  tW = 72.f, tH = 44.f;
        float  tx = sheetX + sheetW * 0.5f - tW * 0.5f, tcy = cY + 20.f + tH * 0.5f;
        DrawToggle(fg, tx, tcy, t);
        if (!g_sheet.closing && io.MouseClicked[0]) {
            if (io.MousePos.x >= tx - 12 && io.MousePos.x <= tx + tW + 12
             && io.MousePos.y >= cY + 8.f && io.MousePos.y <= cY + tH + 32.f)
                { *v=!*v; char _b[160]; snprintf(_b,sizeof(_b),XS("%s|%s"),g_sheet.title,*v?XS("ON"):XS("OFF")); ShowToast(_b); }
        }
        const char* st = *v ? XS("Включено") : XS("Выключено");
        auto stSz = fn->CalcTextSizeA(fs * 1.1f, FLT_MAX, 0, st);
        fg->AddText(fn, fs * 1.1f, {sheetX + sheetW * 0.5f - stSz.x * 0.5f, cY + tH + 26.f},
            *v ? C::U(C::Acc()) : C::U(C::Dim()), st);
    } else if (g_sheet.type == 1 && g_sheet.slP) {
        float* v = g_sheet.slP; float mn = g_sheet.slMin, mx = g_sheet.slMax;
        char vb[24]; snprintf(vb, 24, g_sheet.slFmt, *v);
        auto vSz = fn->CalcTextSizeA(fs * 2.0f, FLT_MAX, 0, vb);
        fg->AddText(fn, fs * 2.0f, {sheetX + sheetW * 0.5f - vSz.x * 0.5f, cY + 10.f}, C::U(C::Acc()), vb);
        float trkY = cY + 78.f, trkX = sheetX + padX + 16.f;
        float trkW = sheetW - padX * 2.f - 32.f, kR2 = 20.f, trkH = 8.f;
        float frac = Clamp01((*v - mn) / (mx - mn)), kx = trkX + frac * trkW;
        fg->AddRectFilled({trkX, trkY - trkH * 0.5f}, {trkX + trkW, trkY + trkH * 0.5f}, C::U(C::TrkOff()), trkH);
        fg->AddRectFilled({trkX, trkY - trkH * 0.5f}, {kx,           trkY + trkH * 0.5f}, C::U(C::Acc()), trkH);
        static bool s_sheetSliderActive = false;
        if (!g_sheet.closing && io.MouseClicked[0] && fabsf(io.MousePos.y - trkY) < 56.f
            && io.MousePos.x >= trkX - 24.f && io.MousePos.x <= trkX + trkW + 24.f)
            s_sheetSliderActive = true;
        if (!io.MouseDown[0]) s_sheetSliderActive = false;
        bool near = s_sheetSliderActive && !g_sheet.closing && io.MouseDown[0];
        fg->AddCircleFilled({kx + 0.7f, trkY + 1.5f}, kR2, IM_COL32(0, 0, 0, 40));
        fg->AddCircleFilled({kx, trkY}, kR2, IM_COL32(255, 255, 255, 255));
        if (near) fg->AddCircle({kx, trkY}, kR2 + 4.f, C::UA(C::Light::Acc, 0.85f), 48, 3.f);
        if (near) { float dx = io.MousePos.x - trkX; *v = mn + Clamp01(dx / trkW) * (mx - mn); }
        char minB[16], maxB[16];
        snprintf(minB, 16, g_sheet.slFmt, mn); snprintf(maxB, 16, g_sheet.slFmt, mx);
        fg->AddText(fn, fs * 0.85f, {trkX, trkY + kR2 + 8.f}, C::U(C::Dim()), minB);
        auto mxSz = fn->CalcTextSizeA(fs * 0.85f, FLT_MAX, 0, maxB);
        fg->AddText(fn, fs * 0.85f, {trkX + trkW - mxSz.x, trkY + kR2 + 8.f}, C::U(C::Dim()), maxB);
    } else if (g_sheet.type == 2) {
        auto dSz = fn->CalcTextSizeA(fs * 1.15f, FLT_MAX, 0, XS("Приложение будет закрыто."));
        fg->AddText(fn, fs * 1.15f, {sheetX + sheetW * 0.5f - dSz.x * 0.5f, cY + 16.f},
            C::U(C::Dim()), XS("Приложение будет закрыто."));
    }

    {
        float btnH = 62.f, btnY = sheetY + sheetH - btnH - 24.f;
        if (g_sheet.type == 2) {
            float bW  = (sheetW - padX * 2.f - 12.f) * 0.5f;
            float b1X = sheetX + padX, b2X = b1X + bW + 12.f;
            DrawExitButtons(fg, fn, fs, 1.f,
                b1X, b2X, bW, btnY, btnH,
                g_sheet.closing,
                io.MousePos, io.MouseClickedPos[0],
                io.MouseReleased[0], true);
        } else {
            float bW = sheetW - padX * 2.f, bX = sheetX + padX;
            fg->AddRectFilled({bX, btnY}, {bX + bW, btnY + btnH}, C::U(C::Acc()), R::Btn);
            auto bSz = fn->CalcTextSizeA(fs * 1.15f, FLT_MAX, 0, XS("Готово"));
            fg->AddText(fn, fs * 1.15f, {bX + bW * 0.5f - bSz.x * 0.5f, btnY + (btnH - fs * 1.15f) * 0.5f},
                IM_COL32(255, 255, 255, 255), XS("Готово"));
            if (!g_sheet.closing && io.MouseClicked[0]
                && io.MousePos.x >= bX && io.MousePos.x <= bX + bW
                && io.MousePos.y >= btnY && io.MousePos.y <= btnY + btnH)
                SheetClose();
        }
    }
    fg->PopClipRect();
}

static float DrawPopoverContentFG(ImDrawList* fg, ImFont* fn, float fs, int secId, float dt,
                                   float cX, float cW, float curY, float alpha,
                                   bool blocked, const ImVec2& mousePos, const ImVec2& clickedPos,
                                   bool mouseReleased, bool mouseDown)
{
    const float inset = Layout::Inset, padX = Layout::PadX;
    const float rH = Layout::RowH;

    auto FgSHdr = [&](const char* t, float topPad = 18.f) {
        curY += topPad;
        if (t && t[0]) {
            auto tsz = fn->CalcTextSizeA(fs * 1.15f, FLT_MAX, 0, t);
            fg->AddText(fn, fs * 1.15f, {cX + 32.f, curY}, C::UA(C::Dim(), alpha), t);
            curY += tsz.y + 8.f;
        } else {
            curY += 8.f;
        }
    };

    auto FgCardBg = [&](float h, ImDrawFlags flags = ImDrawFlags_RoundCornersAll) {
        fg->AddRectFilled({cX + inset, curY}, {cX + cW - inset, curY + h}, C::UA(C::Card(), alpha), R::Card, flags);
        if (g_state.ui_show_sep)
            fg->AddRect({cX + inset, curY}, {cX + cW - inset, curY + h}, C::UA(C::Sep(), alpha), R::Card, flags, 1.2f);
    };

    auto FgToggleRow = [&](const char* lbl, bool* v, float& anim, bool last) {
        Tick(anim, *v, dt, 12.f);
        RenderToggleRowVisuals(fg, fn, fs, cX, curY, cW, lbl, anim, alpha,
                               g_state.ui_show_sep, last);
        if (!blocked && !g_scrollPop.dragging && mouseReleased
            && mousePos.x >= cX + inset && mousePos.x <= cX + cW - inset
            && mousePos.y >= curY && mousePos.y <= curY + rH
            && clickedPos.x >= cX + inset && clickedPos.x <= cX + cW - inset
            && clickedPos.y >= curY && clickedPos.y <= curY + rH) {
            *v = !*v;
            char _b[160]; snprintf(_b, sizeof(_b), XS("%s|%s"), lbl, *v ? XS("ON") : XS("OFF")); ShowToast(_b);
            PlaySound(SND_CLICK);
        }
        curY += rH;
    };

    auto FgRadioRow = [&](int idx, int* cur, AppState::RadioAnim& ra, float* fillAnim,
                           const char* lbl, bool last) {
        bool isSelected = (*cur == idx);
        Tick(*fillAnim, isSelected, dt, 10.f);
        float fill = EaseInOut(*fillAnim);

        if (!blocked && mouseReleased
            && mousePos.x >= cX + inset && mousePos.x <= cX + cW - inset
            && mousePos.y >= curY && mousePos.y <= curY + rH
            && clickedPos.y >= curY && clickedPos.y <= curY + rH
            && !isSelected) {
            ra.scaleVel = -8.f;
            ra.ring = 0.f;
            ra.ringVel = 5.5f;
            *cur = idx;
            PlaySound(SND_CLICK);
        }
        SpringTick(ra.scale, ra.scaleVel, 1.0f, dt);
        ra.scale = ImMax(0.6f, ra.scale);
        SpringTick(ra.ring, ra.ringVel, 3.5f, dt);

        const float rR = 22.f;
        float textX = cX + inset + padX, textY = curY + (rH - fs * 1.15f) * 0.5f;
        float textMaxX = cX + cW - inset - rR * 2.f - padX - 16.f;
        fg->PushClipRect({textX, curY}, {textMaxX, curY + rH}, true);
        fg->AddText(fn, fs * 1.15f, {textX, textY}, C::UA(C::Txt(), alpha), lbl);
        fg->PopClipRect();

        float cx2 = cX + cW - inset - rR - padX, cy2 = curY + rH * 0.5f;
        float r2 = rR * ra.scale;

        if (ra.ring > 0.05f && ra.ring < 3.4f) {
            float ringR  = rR + ra.ring * 10.f;
            float ringA  = (1.f - ra.ring / 3.5f) * 0.55f * alpha;
            fg->AddCircle({cx2, cy2}, ringR, C::UA(C::Acc(), ringA), 48, 2.5f);
        }

        ImU32 radioCol = IM_COL32(
            int(Lerpf(C::TrkOff().x, C::Acc().x, fill)*255),
            int(Lerpf(C::TrkOff().y, C::Acc().y, fill)*255),
            int(Lerpf(C::TrkOff().z, C::Acc().z, fill)*255),
            int(alpha*255));
        fg->AddCircleFilled({cx2, cy2}, r2, radioCol, 40);

        if (fill > 0.01f)
            fg->AddCircleFilled({cx2, cy2}, r2 * 0.46f * fill,
                IM_COL32(255,255,255,int(255*fill*alpha)), 32);

        if (fill < 0.99f)
            fg->AddCircle({cx2, cy2}, r2, C::UA(C::Sep(), (1.f-fill)*alpha), 40, 1.8f);

        if (!last && g_state.ui_show_sep)
            fg->AddLine({cX + inset + padX, curY + rH - 0.5f},
                        {cX + cW - inset - padX, curY + rH - 0.5f}, C::UA(C::Sep(), alpha), 0.8f);
        curY += rH;
    };

    auto FgSliderRow = [&](const char* lbl, float* v, float mn, float mx, const char* fmt,
                            bool last, AppState::SliderAnim& anim) {
        const float rowH = Layout::SliderH;
        float cX2 = cX + inset, cW2 = cW - inset * 2.f;

        bool act = !blocked && mouseDown && !g_scrollPop.dragging
            && clickedPos.x >= cX2 && clickedPos.x <= cX2 + cW2
            && clickedPos.y >= curY && clickedPos.y <= curY + rowH;

        if (act)
            *v = mn + Clamp01((mousePos.x - (cX2 + padX + 22.f)) / (cW2 - padX * 2.f - 44.f)) * (mx - mn);

        float target = Clamp01((*v - mn) / (mx - mn));
        TickSliderAnim(anim, target, act, dt);

        RenderSliderVisuals(fg, fn, fs, cX, curY, cW, rowH,
                            lbl, fmt, *v, anim.pos, act, alpha,
                            g_state.ui_show_sep, last);
        curY += rowH;
    };

    if (secId == 0) {
        FgSHdr(XS("Кость прицела"));
        FgCardBg(rH * 3);
        FgRadioRow(0, &g_state.aim_bone, g_state.ra_aim_head,   &g_state.a_aim_head,   XS("Голова"), false);
        FgRadioRow(1, &g_state.aim_bone, g_state.ra_aim_chest,  &g_state.a_aim_chest,  XS("Тело"), false);
        FgRadioRow(2, &g_state.aim_bone, g_state.ra_aim_pelvis, &g_state.a_aim_pelvis, XS("Ноги"), true);

    } else if (secId == 1) {
        FgSHdr(XS("Внешний вид"));
        FgCardBg(Layout::SliderH);
        FgSliderRow(XS("Толщина"),   &g_state.esp_thick, 0.5f, 5.f,   "%.1f",     true, g_state.sl_esp_thick);

    } else if (secId == 2) {
        FgSHdr(nullptr, 12.f);
        auto dSz = fn->CalcTextSizeA(fs * 1.15f, FLT_MAX, 0, XS("Приложение будет закрыто."));
        fg->AddText(fn, fs * 1.15f,
            {cX + (cW - dSz.x) * 0.5f, curY},
            C::UA(C::Dim(), alpha), XS("Приложение будет закрыто."));
        curY += dSz.y + 28.f;

        const float btnH = 62.f, bPad = 28.f;
        float bW  = (cW - inset * 2.f - bPad * 2.f - 12.f) * 0.5f;
        float b1X = cX + inset + bPad, b2X = b1X + bW + 12.f;
        DrawExitButtons(fg, fn, fs, alpha,
            b1X, b2X, bW, curY, btnH,
            blocked, mousePos, clickedPos,
            mouseReleased, false);
        curY += btnH;
    } else if (secId == 3) {
        const char* cfgName = (g_configToDelete >= 0 && g_configToDelete < g_configCount)
            ? g_configs[g_configToDelete].name : XS("Конфиг");

        FgSHdr(nullptr, 40.f);

        {
            char nameBuf[96];
            snprintf(nameBuf, sizeof(nameBuf), XS("«%s»"), cfgName);
            float nameFS = fs * 1.55f;
            auto  nameSz = fn->CalcTextSizeA(nameFS, FLT_MAX, 0, nameBuf);
            fg->AddText(fn, nameFS,
                {cX + (cW - nameSz.x) * 0.5f, curY},
                C::UA(C::Txt(), alpha), nameBuf);
            curY += nameSz.y + 12.f;
        }

        {
            const char* sub = XS("Будет удалён безвозвратно");
            auto sSz = fn->CalcTextSizeA(fs * 1.05f, FLT_MAX, 0, sub);
            fg->AddText(fn, fs * 1.05f,
                {cX + (cW - sSz.x) * 0.5f, curY},
                C::UA(C::Red(), alpha * 0.85f), sub);
            curY += sSz.y + 36.f;
        }

        {
            const float btnH = 58.f, bPad = 28.f, gap = 12.f;
            float bW  = (cW - inset * 2.f - bPad * 2.f - gap) * 0.5f;
            float b1X = cX + inset + bPad;
            float b2X = b1X + bW + gap;

            fg->AddRectFilled({b1X, curY}, {b1X + bW, curY + btnH}, C::UA(C::Card(), alpha), R::Btn);
            fg->AddRect({b1X, curY}, {b1X + bW, curY + btnH}, C::UA(C::Sep(), alpha), R::Btn, 0, 1.5f);
            {
                const char* ct = XS("Отмена");
                auto ctsz = fn->CalcTextSizeA(fs * 1.1f, FLT_MAX, 0, ct);
                fg->AddText(fn, fs * 1.1f,
                    {b1X + bW * 0.5f - ctsz.x * 0.5f, curY + (btnH - fs * 1.1f) * 0.5f},
                    C::UA(C::Txt(), alpha), ct);
            }

            fg->AddRectFilled({b2X, curY}, {b2X + bW, curY + btnH}, C::UA(C::Red(), alpha * 0.9f), R::Btn);
            fg->AddRect({b2X, curY}, {b2X + bW, curY + btnH}, C::UA(C::Red(), alpha), R::Btn, 0, 1.5f);
            {
                const char* dt2 = XS("Удалить");
                auto dtsz = fn->CalcTextSizeA(fs * 1.1f, FLT_MAX, 0, dt2);
                fg->AddText(fn, fs * 1.1f,
                    {b2X + bW * 0.5f - dtsz.x * 0.5f, curY + (btnH - fs * 1.1f) * 0.5f},
                    IM_COL32(255, 255, 255, int(255 * alpha)), dt2);
            }

            if (!blocked && mouseReleased) {
                if (mousePos.x >= b1X && mousePos.x <= b1X + bW
                 && mousePos.y >= curY && mousePos.y <= curY + btnH
                 && clickedPos.x >= b1X && clickedPos.x <= b1X + bW
                 && clickedPos.y >= curY && clickedPos.y <= curY + btnH)
                    PopoverClose();

                if (mousePos.x >= b2X && mousePos.x <= b2X + bW
                 && mousePos.y >= curY && mousePos.y <= curY + btnH
                 && clickedPos.x >= b2X && clickedPos.x <= b2X + bW
                 && clickedPos.y >= curY && clickedPos.y <= curY + btnH) {
                    ConfigDelete(g_configToDelete);
                    g_configToDelete = -1;
                    PopoverClose();
                }
            }
            curY += btnH;
        }
    }

    curY += 12.f;
    return curY;
}

void DrawPopover(float dt, ImVec2 menuPos, float WW, float WH) {
    if (!g_pop.visible) return;

    g_pop.openFrames++;

    if (!TickSlideAnim(g_pop.anim, g_pop.vel, g_pop.closing, dt)) {
        g_pop.anim = 0.f; g_pop.vel = 0.f;
        g_pop.visible = false; g_pop.closing = false;
        g_popOpenClickPos = {-9999.f, -9999.f};
        g_scrollPop = {};
        Blur::Unfreeze();
        return;
    }

    auto& io    = ImGui::GetIO();
    float easeT = EaseInOut(g_pop.anim);
    auto* fn    = ImGui::GetFont();
    float fs    = ImGui::GetFontSize();

    float titleFS  = fs * 1.45f;
    float headerH  = 28.f + titleFS + 16.f + 8.f;

    float popW = WW * 0.92f, popH = WH * 0.88f;

    float sX     = menuPos.x + (WW - popW) * 0.5f;
    float sYFull = menuPos.y + (WH - popH) * 0.5f;
    float sY     = Lerpf(menuPos.y + WH, sYFull, easeT);
    float sW = popW, sH = popH;

    float contentY = sY + headerH;
    float areaH    = sH - headerH;
    float lineY    = sY + 28.f + titleFS + 16.f;

    auto* fg = ImGui::GetForegroundDrawList();

    Blur::Draw(fg, g_win.pos, {g_win.pos.x+g_win.w, g_win.pos.y+g_win.h}, easeT, R::Card);

    fg->PushClipRect({menuPos.x, menuPos.y}, {menuPos.x + WW, menuPos.y + WH}, true);

    fg->AddRectFilled({sX, sY}, {sX + sW, sY + sH}, C::U(C::Bg()), R::Sheet);

    {
        float dhW = 72.f, dhH = 7.f;
        float dhX = sX + sW * 0.5f - dhW * 0.5f, dhY = sY + 14.f;
        fg->AddRectFilled({dhX, dhY}, {dhX + dhW, dhY + dhH},
            C::UA(C::Dim(), 0.45f * easeT), dhH * 0.5f);
        if (io.MouseClicked[0]
            && io.MousePos.x >= sX && io.MousePos.x <= sX + sW
            && io.MousePos.y >= sY && io.MousePos.y <= sY + 44.f) {
            g_win.dragging   = true;
            g_win.touchStart = io.MousePos;
            g_win.posStart   = g_win.pos;
        }
    }

    bool inPop = io.MousePos.x >= sX && io.MousePos.x <= sX + sW
              && io.MousePos.y >= contentY && io.MousePos.y <= sY + sH - 4.f;
    bool clickInPop = io.MouseClickedPos[0].x >= sX && io.MouseClickedPos[0].x <= sX + sW
                   && io.MouseClickedPos[0].y >= contentY && io.MouseClickedPos[0].y <= sY + sH - 4.f;
    bool mIn = inPop && clickInPop;

    static float s_popMaxScroll = 0.f;
    bool colorPop = (g_pop.sectionId == 4);
    if (!colorPop) ScrollTick(g_scrollPop, mIn, g_pop.closing, s_popMaxScroll, dt);
    else { g_scrollPop.off = 0.f; g_scrollPop.vel = 0.f; s_popMaxScroll = 0.f; }

    if (colorPop && g_colP) {

        const float PAD   = 16.f;
        const float btnH  = 62.f, btnMar = 10.f;
        float X  = sX + PAD, W = sW - PAD * 2.f;
        float bY = sY + sH - btnH - btnMar;

        const int   COLS     = 7;
        const float CELL_GAP = 10.f;
        int   ROWS   = (kPaletteCount + COLS - 1) / COLS;
        float availH = bY - contentY - 16.f;
        float cellW = (W - CELL_GAP * (COLS - 1)) / (float)COLS;
        float cellH = (availH - CELL_GAP * (ROWS - 1)) / (float)ROWS;
        float cellSz = cellW < cellH ? cellW : cellH;
        if (cellSz < 22.f) cellSz = 22.f;
        float gridW  = COLS * cellSz + (COLS - 1) * CELL_GAP;
        float gridH  = ROWS * cellSz + (ROWS - 1) * CELL_GAP;
        float gridX  = X + (W - gridW) * 0.5f;
        float gridY  = contentY + (bY - contentY - gridH) * 0.5f;
        if (gridY + gridH > bY - 8.f) gridY = bY - 8.f - gridH;
        if (gridY < contentY + 8.f) gridY = contentY + 8.f;
        fg->PushClipRect({sX + 8.f, contentY}, {sX + sW - 8.f, bY - 6.f}, true);

        const float CELL_R = 10.f;

        static int   s_palSel  = -1;
        static float s_selOffX = 0.f, s_selOffY = 0.f;
        static float s_selVelX = 0.f, s_selVelY = 0.f;
        static float s_selSz   = 0.f, s_selVelSz = 0.f;
        if (g_colP) {
            float bestDist = 1e9f;
            int   bestIdx  = -1;
            for (int pi = 0; pi < kPaletteCount; pi++) {
                float dr = kColorPalette[pi].x - g_colP->x;
                float dg = kColorPalette[pi].y - g_colP->y;
                float db = kColorPalette[pi].z - g_colP->z;
                float d  = dr*dr + dg*dg + db*db;
                if (d < bestDist) { bestDist = d; bestIdx = pi; }
            }
            if (bestDist < 0.02f) s_palSel = bestIdx; else s_palSel = -1;
        }

        auto cellCenter = [&](int pi) -> ImVec2 {
            int r = pi / COLS, c = pi % COLS;
            return { c * (cellSz + CELL_GAP) + cellSz * 0.5f,
                     r * (cellSz + CELL_GAP) + cellSz * 0.5f };
        };

        if (s_palSel >= 0) {
            ImVec2 tc = cellCenter(s_palSel);
            if (s_selSz < 0.001f || g_palSelReset) {
                s_selOffX = tc.x; s_selOffY = tc.y;
                s_selVelX = 0.f;  s_selVelY = 0.f;
                if (g_palSelReset) { s_selSz = 1.f; s_selVelSz = 0.f; }
            }
            SpringTick(s_selOffX, s_selVelX, tc.x, dt);
            SpringTick(s_selOffY, s_selVelY, tc.y, dt);
            SpringTick(s_selSz, s_selVelSz, 1.f, dt);
        } else {
            SpringTick(s_selSz, s_selVelSz, 0.f, dt);
        }
        g_palSelReset = false;

        for (int pi = 0; pi < kPaletteCount; pi++) {
            int row = pi / COLS, col = pi % COLS;
            float cx0 = gridX + col * (cellSz + CELL_GAP);
            float cy0 = gridY + row * (cellSz + CELL_GAP);
            float cx1 = cx0 + cellSz, cy1 = cy0 + cellSz;

            ImVec4 cv = kColorPalette[pi];
            ImU32  fill = IM_COL32(int(cv.x*255), int(cv.y*255), int(cv.z*255), 255);

            fg->AddRectFilled({cx0+1.5f, cy0+1.5f}, {cx1+1.5f, cy1+1.5f},
                IM_COL32(0,0,0, int(18*easeT)), CELL_R);
            fg->AddRectFilled({cx0, cy0}, {cx1, cy1}, fill, CELL_R);

            if (!g_pop.closing && io.MouseReleased[0]) {
                ImVec2 mp = io.MousePos, cp = io.MouseClickedPos[0];
                bool hit = mp.x >= cx0 && mp.x <= cx1 && mp.y >= cy0 && mp.y <= cy1
                        && cp.x >= cx0 && cp.x <= cx1 && cp.y >= cy0 && cp.y <= cy1;
                if (hit) {
                    *g_colP = kColorPalette[pi];
                    s_palSel = pi;
                    g_input.touchConsumed = true;
                }
            }
        }

        if (s_selSz > 0.001f) {
            float ax = gridX + s_selOffX, ay = gridY + s_selOffY;
            float r = (cellSz * 0.5f + 5.f) * s_selSz;
            fg->AddRect({ax - r, ay - r}, {ax + r, ay + r},
                IM_COL32(0,0,0, int(220*easeT*s_selSz)), CELL_R + 5.f * s_selSz, 0, 4.f);
            fg->AddRect({ax - r + 2.5f, ay - r + 2.5f}, {ax + r - 2.5f, ay + r - 2.5f},
                IM_COL32(255,255,255, int(255*easeT*s_selSz)), CELL_R + 2.5f * s_selSz, 0, 1.5f);
        }
        fg->PopClipRect();

        fg->AddRectFilled({X, bY}, {X+W, bY+btnH}, C::UA(C::Acc(), easeT), R::Btn);
        auto bsz = fn->CalcTextSizeA(fs*1.2f, FLT_MAX, 0, XS("Готово"));
        fg->AddText(fn, fs*1.2f,
            {X + W*0.5f - bsz.x*0.5f, bY + (btnH-bsz.y)*0.5f},
            IM_COL32(255,255,255,int(255*easeT)), XS("Готово"));

        if (io.MouseDown[0] || io.MouseReleased[0])
            g_input.touchConsumed = true;

        if (!g_pop.closing && io.MouseReleased[0]) {
            ImVec2 mp = io.MousePos, cp = io.MouseClickedPos[0];
            bool doneTap = mp.x >= X && mp.x <= X+W && mp.y >= bY && mp.y <= bY+btnH
                        && cp.x >= X && cp.x <= X+W && cp.y >= bY && cp.y <= bY+btnH;
            if (doneTap) PopoverClose();
        }

    } else {
        fg->PushClipRect({sX, contentY}, {sX + sW, sY + sH - 4.f}, true);

        float drawY = contentY - g_scrollPop.off;
        float endY  = DrawPopoverContentFG(fg, fn, fs, g_pop.sectionId, dt,
                                            sX, sW, drawY, easeT,
                                            g_pop.closing,
                                            io.MousePos, io.MouseClickedPos[0],
                                            io.MouseReleased[0] && !g_scrollPop.dragging,
                                            io.MouseDown[0]);

        float contentTotalH = endY - drawY;
        s_popMaxScroll = ImMax(0.f, contentTotalH - (areaH - 4.f));

        fg->PopClipRect();
    }

    {
        auto tsz = fn->CalcTextSizeA(titleFS, FLT_MAX, 0, g_pop.title);
        fg->AddText(fn, titleFS,
            {sX + sW * 0.5f - tsz.x * 0.5f, sY + 28.f},
            C::UA(C::Txt(), easeT), g_pop.title);
    }
    {
        float cR = 26.f, bcx = sX + sW - cR - 16.f, bcy = sY + 28.f + titleFS * 0.5f;
        fg->AddCircleFilled({bcx, bcy}, cR, C::UA(C::Sep(), easeT), 32);
        float cs = 9.f;
        fg->AddLine({bcx-cs, bcy-cs}, {bcx+cs, bcy+cs}, C::UA(C::Dim(), easeT), 3.2f);
        fg->AddLine({bcx+cs, bcy-cs}, {bcx-cs, bcy+cs}, C::UA(C::Dim(), easeT), 3.2f);

        if (!g_pop.closing && io.MouseReleased[0]
            && fabsf(io.MousePos.x - bcx) < cR + 12.f
            && fabsf(io.MousePos.y - bcy) < cR + 12.f) {
            PopoverClose();
            g_input.touchConsumed = true;
        }
    }
    fg->AddLine({sX + 18.f, lineY}, {sX + sW - 18.f, lineY}, C::UA(C::Sep(), easeT), 2.5f);

    fg->PopClipRect();
}

float TabContent(int tab, float dt, float cW) {
    float sY = ImGui::GetCursorPosY();

    if (tab == 0) {
        auto* dl  = ImGui::GetWindowDrawList();
        auto* fn  = ImGui::GetFont();
        float avW = ImGui::GetContentRegionAvail().x;
        const float inset = Layout::Inset, padX = Layout::PadX;
        const float fs = ImGui::GetFontSize();

        SHdr(XS("\xd0\xa0\xd0\xb0\xd0\xb7\xd1\x80\xd0\xb0\xd0\xb1\xd0\xbe\xd1\x82\xd1\x87\xd0\xb8\xd0\xba"));
        {
            const float avatarR = 44.f;
            const float cardH   = 152.f;

            auto pos  = ImGui::GetCursorScreenPos();
            float cx0 = pos.x + inset;
            float cx1 = pos.x + avW - inset;
            float cardW = cx1 - cx0;

            dl->AddRectFilled({cx0, pos.y}, {cx1, pos.y + cardH}, C::U(C::Card()), R::Card);
            if (g_state.ui_show_sep)
                dl->AddRect({cx0, pos.y}, {cx1, pos.y + cardH}, C::U(C::Sep()), R::Card, 0, 1.2f);

            float avCX = cx0 + padX + avatarR + 2.f;
            float avCY = pos.y + cardH * 0.5f;

            dl->AddCircleFilled({avCX, avCY}, avatarR + 6.f, C::UA(C::Acc(), 0.12f), 64);
            dl->AddCircleFilled({avCX, avCY}, avatarR + 2.5f, C::U(C::Card()), 64);

            {
                const int segs = 60;
                ImVec4 ac = C::Acc();
                ImVec4 ac2 = {Lerpf(ac.x,0.3f,0.55f), Lerpf(ac.y,0.05f,0.55f), Lerpf(ac.z,0.9f,0.42f), 1.f};
                for (int si = 0; si < segs; si++) {
                    float a0 = (float)si/segs*IM_PI*2.f, a1 = (float)(si+1)/segs*IM_PI*2.f;
                    float tt = (float)si/segs;
                    ImVec4 ca = {Lerpf(ac2.x,ac.x,tt), Lerpf(ac2.y,ac.y,tt), Lerpf(ac2.z,ac.z,tt), 1.f};
                    dl->AddTriangleFilled({avCX,avCY},
                        {avCX+avatarR*cosf(a0),avCY+avatarR*sinf(a0)},
                        {avCX+avatarR*cosf(a1),avCY+avatarR*sinf(a1)},
                        IM_COL32(int(ca.x*255),int(ca.y*255),int(ca.z*255),255));
                }
                float lfs = avatarR * 1.05f;
                auto lsz = fn->CalcTextSizeA(lfs, FLT_MAX, 0, XS("\xd0\xa1"));
                dl->AddText(fn, lfs, {avCX-lsz.x*0.5f, avCY-lsz.y*0.5f}, IM_COL32(255,255,255,245), XS("\xd0\xa1"));
            }

            dl->AddCircle({avCX,avCY}, avatarR+2.5f, C::UA(C::Acc(),0.9f), 64, 2.5f);

            {
                float dx = avCX+avatarR*0.72f, dy = avCY+avatarR*0.72f;
                dl->AddCircleFilled({dx,dy}, 9.f, C::U(C::Card()), 24);
                dl->AddCircleFilled({dx,dy}, 6.5f, IM_COL32(52,199,89,255), 24);
            }

            float nameFS = fs * 1.75f;
            float tagFS  = fs * 1.1f;
            auto nameSz = fn->CalcTextSizeA(nameFS, FLT_MAX, 0, XS("\xd0\xa1\xd0\xb0\xd0\xbd\xd1\x8f"));
            float textX = avCX + avatarR + padX + 8.f;
            float nameY = avCY - nameSz.y - 3.f;
            float tagY  = avCY + 3.f;
            dl->AddText(fn, nameFS, {textX, nameY}, C::U(C::Txt()), XS("\xd0\xa1\xd0\xb0\xd0\xbd\xd1\x8f"));
            dl->AddText(fn, tagFS,  {textX, tagY},  C::U(C::Dim()), XS("@xvcey"));

            const float btnH = 42.f;
            const float btnW = 132.f;
            float btnX = cx1 - btnW - padX;
            float btnY = pos.y + cardH - btnH - 14.f;

            dl->AddRectFilled({btnX,btnY},{btnX+btnW,btnY+btnH}, C::U(C::Acc()), btnH*0.5f);

            auto btsz = fn->CalcTextSizeA(fs*1.05f, FLT_MAX, 0, XS("\xd0\x9d\xd0\xb0\xd0\xbf\xd0\xb8\xd1\x81\xd0\xb0\xd1\x82\xd1\x8c"));
            dl->AddText(fn, fs*1.05f,
                {btnX+btnW*0.5f-btsz.x*0.5f, btnY+(btnH-btsz.y)*0.5f},
                IM_COL32(255,255,255,255),
                XS("\xd0\x9d\xd0\xb0\xd0\xbf\xd0\xb8\xd1\x81\xd0\xb0\xd1\x82\xd1\x8c"));

            ImGui::SetCursorScreenPos({cx0, pos.y});
            ImGui::InvisibleButton("##devcard", {cardW, cardH});
            bool popBlk = (g_pop.visible && !g_pop.closing) || g_sheet.visible;
            if (!popBlk && !IsScrollDragging() && !g_input.touchConsumed && ImGui::GetIO().MouseReleased[0]) {
                auto mp = ImGui::GetIO().MousePos, cp = ImGui::GetIO().MouseClickedPos[0];
                if (mp.x>=btnX&&mp.x<=btnX+btnW&&mp.y>=btnY&&mp.y<=btnY+btnH
                 &&cp.x>=btnX&&cp.x<=btnX+btnW&&cp.y>=btnY&&cp.y<=btnY+btnH) {
                    system(XS("am start -a android.intent.action.VIEW -d \"https://t.me/xvcey\""));
                    PlaySound(SND_CLICK);
                }
            }
            ImGui::SetCursorScreenPos({pos.x, pos.y + cardH});
            ImGui::Dummy({avW, 0.f});
        }

    } else if (tab == 1) {

        cfg::aim::enabled    = g_state.aim_touch;
        cfg::aim::vis_check  = g_state.aim_pos;
        cfg::aim::draw_fov   = g_state.aim_special;
        cfg::aim::fov        = g_state.gun_fov;
        cfg::aim::smoothness = g_state.gun_str;
        cfg::aim::bone       = g_state.aim_bone;
        cfg::aim::trigger_delay  = g_state.gun_trigger_delay;

        SHdr(XS("Аимбот"));
        CardBg(Layout::RowH * 3);
        ToggleRow("##ta1", XS("Включить аимбот"),    &g_state.aim_touch,   g_state.a_aim_touch, false, true);
        ToggleRow("##ta2", XS("Проверка видимости"),  &g_state.aim_pos,     g_state.a_aim_pos,   false);
        ToggleRow("##ta3", XS("Показывать FOV круг"), &g_state.aim_special, g_state.a_aim_spec,  true);

        SHdr(XS("FOV аимбота"));
        CardBg(Layout::SliderH);
        SliderRow("##afov", XS("Радиус FOV"), &g_state.gun_fov, 5.f, 360.f, XS("%.0f°"), true, true, g_state.sl_gun_fov, dt);

        SHdr(XS("Плавность"));
        CardBg(Layout::SliderH);
        SliderRow("##asmt", XS("Плавность"), &g_state.gun_str, 0.f, 1.f, "%.2f", true, true, g_state.sl_gun_str, dt);

        ImGui::Dummy({1.f, 8.f});
        CollapsibleHeader("##cah1", XS("Дополнительные настройки"), 0);

        SHdr(XS("Триггер бот"));
        CardBg(Layout::RowH * 1);
        { static float _a_tbot = 0.f; Tick(_a_tbot, cfg::aim::trigger_bot, dt);
          ToggleRow("##ta5", XS("Триггер бот"), &cfg::aim::trigger_bot, _a_tbot, true); }

        SHdr(XS("Задержка триггера"));
        CardBg(Layout::SliderH);
        SliderRow("##atrig", XS("Задержка"), &g_state.gun_trigger_delay, 0.0f, 1.0f, "%.1f", true, true, g_state.sl_gun_trig, dt);

        ImGui::Dummy({1.f, 16.f});

        SHdr(XS("Нет отдачи"));
        CardBg(Layout::RowH * 1);
        { static float _a_nr = 0.f; Tick(_a_nr, g_noRecoilEnabled, dt);
          ToggleRow("##nr1", XS("Нет отдачи"), &g_noRecoilEnabled, _a_nr, true); }

        ImGui::Dummy({1.f, 12.f});

} else if (tab == 2) {
        cfg::esp::box          = g_state.esp_box;
        cfg::esp::name_esp     = g_state.esp_name;
        cfg::esp::health       = g_state.esp_hp;
        cfg::esp::distance     = g_state.esp_wall;
        cfg::esp::weapon       = g_state.esp_weapon;
        cfg::esp::weapon_icon  = g_state.esp_weapon_icon;
        cfg::esp::tracer       = g_state.esp_tracer;
        cfg::esp::skeleton     = g_state.esp_skeleton;
        cfg::esp::money        = g_state.esp_money;
        cfg::esp::ping_show    = g_state.esp_ping;



        auto* dl  = ImGui::GetWindowDrawList();
        auto* fn  = ImGui::GetFont();
        float avW = ImGui::GetContentRegionAvail().x;
        float fs  = ImGui::GetFontSize();
        const float inset = Layout::Inset, padX = Layout::PadX, rowH = Layout::RowH;
        bool popBlk = (g_pop.visible && !g_pop.closing) || g_sheet.visible;

        static const char* s_colorHoldId = nullptr;

        auto EspToggleColorRow = [&](const char* id, const char* lbl, bool* v, float* a, ImVec4* col, bool last) {
            auto pos = ImGui::GetCursorScreenPos();
            float cX = pos.x + inset, cW2 = avW - inset * 2.f;
            float cy = pos.y + rowH * 0.5f;
            auto& io2 = ImGui::GetIO();

            ImGui::InvisibleButton(id, {avW, rowH});

            float dotR = 13.f;
            float dotX = 0.f;
            if (col) {
                auto lblSz = fn->CalcTextSizeA(fs*1.15f, FLT_MAX, 0, lbl);
                dotX = cX + padX + lblSz.x + 14.f + dotR;
            }
            bool openedColor = false;
            if (col && !popBlk && !g_pop.visible) {
                float cx0 = io2.MouseClickedPos[0].x, cy0 = io2.MouseClickedPos[0].y;
                float dx = io2.MousePos.x - cx0, dy = io2.MousePos.y - cy0;
                float moved = dx*dx + dy*dy;
                bool onDot = (cx0 - dotX)*(cx0 - dotX) + (cy0 - cy)*(cy0 - cy) <= (dotR + 18.f)*(dotR + 18.f);
                if (io2.MouseReleased[0] && onDot && moved < 28.f*28.f && s_colorHoldId != id) {
                    s_colorHoldId = id;
                    PopoverOpenColor(lbl, col);
                    PlaySound(SND_CLICK);
                    g_input.touchConsumed = true;
                    openedColor = true;
                }
            }
            if (!io2.MouseDown[0] && s_colorHoldId == id) s_colorHoldId = nullptr;

            bool tapped = !openedColor && WasTappedHere() && !popBlk && !IsScrollDragging() && !g_input.touchConsumed;
            if (tapped) { *v = !*v; char b[160]; snprintf(b,sizeof(b),XS("%s|%s"),lbl,*v?XS("ON"):XS("OFF")); ShowToast(b); PlaySound(SND_CLICK); }
            if (col) {
                ImVec4& c = *col;
                dl->AddCircleFilled({dotX, cy}, dotR, IM_COL32((int)(c.x*255),(int)(c.y*255),(int)(c.z*255),255), 32);
                dl->AddCircle({dotX, cy}, dotR + 2.f, IM_COL32(0,0,0,80), 32, 2.f);
                dl->AddCircle({dotX, cy}, dotR + 1.f, IM_COL32(255,255,255,180), 32, 1.2f);
            }
            float textY = cy - fs * 1.15f * 0.5f;
            dl->AddText(fn, fs * 1.15f, {cX + padX, textY}, C::U(C::Txt()), lbl);
            DrawToggle(dl, cX + cW2 - 72.f - padX, cy, EaseInOut(*a));
            if (!last && g_state.ui_show_sep)
                dl->AddLine({cX+padX, pos.y+rowH-0.5f},{cX+cW2-padX, pos.y+rowH-0.5f}, C::UA(C::Sep(),0.35f), 0.8f);
        };

        SHdr(XS("ESP"));
        {
            struct ERow { const char* id; const char* lbl; bool* v; float* a; ImVec4* col; };
            ERow rows[] = {
                {"##vb",  XS("Бокс"),           &g_state.esp_box,          &g_state.a_esp_box,          &cfg::esp::box_col},
                {"##v3",  XS("3D рамка"),       &g_state.esp_chams,        &g_state.a_esp_chams,        &cfg::esp::box_col_invis},
                {"##vn",  XS("Имена"),          &g_state.esp_name,         &g_state.a_esp_name,         &cfg::esp::name_col},
                {"##vh",  XS("HP бар"),         &g_state.esp_hp,           &g_state.a_esp_hp,           &cfg::esp::health_col},
                {"##vd",  XS("Дистанция"),      &g_state.esp_wall,         &g_state.a_esp_wall,         &cfg::esp::distance_col},
                {"##vw",  XS("Оружие"),         &g_state.esp_weapon,       &g_state.a_esp_weapon,       &cfg::esp::weapon_col},
                {"##vi",  XS("Иконка оружия"),  &g_state.esp_weapon_icon,  &g_state.a_esp_weapon_icon,  &cfg::esp::weapon_icon_col},
                {"##vtr", XS("Трейсеры"),       &g_state.esp_tracer,       &g_state.a_esp_tracer,       &cfg::esp::tracer_col},
                {"##vsk", XS("Скелет"),         &g_state.esp_skeleton,     &g_state.a_esp_skeleton,     &cfg::esp::skeleton_col},
                {"##vm",  XS("Деньги"),         &g_state.esp_money,        &g_state.a_esp_money,        &cfg::esp::money_col},
                {"##vpi", XS("Пинг"),           &g_state.esp_ping,         &g_state.a_esp_ping,         &cfg::esp::ping_col},
            };
            constexpr int N = 11;
            CardBg(rowH * N);
            for (int i = 0; i < N; i++) {
                EspToggleColorRow(rows[i].id, rows[i].lbl, rows[i].v, rows[i].a, rows[i].col, i == N-1);
            }
        }

        ImGui::Dummy({1.f, 8.f});
        CollapsibleHeader("##veh1", XS("Дополнительные настройки"), 1);

        ImGui::Dummy({1.f, 12.f});

    } else if (tab == 3) {

        auto* dl  = ImGui::GetWindowDrawList();
        auto* fn  = ImGui::GetFont();
        float avW = ImGui::GetContentRegionAvail().x;
        const float inset = Layout::Inset, padX = Layout::PadX;
        const float fs = ImGui::GetFontSize();

        SHdr(XS("Управление"));
        {
            const float btnH = 72.f;
            auto pos = ImGui::GetCursorScreenPos();
            float cardX0 = pos.x + inset, cardX1 = pos.x + avW - inset;

            dl->AddRectFilled({cardX0, pos.y}, {cardX1, pos.y + btnH},
                C::U(C::Card()), R::Btn);
            if (g_state.ui_show_sep)
                dl->AddRect({cardX0, pos.y}, {cardX1, pos.y + btnH},
                    C::UA(C::Sep(), 0.9f), R::Btn, 0, 1.2f);

            ImGui::InvisibleButton("##cfgcreate", {avW, btnH});
            auto& io2 = ImGui::GetIO();
            bool popBlocking2 = (g_pop.visible && !g_pop.closing) || g_sheet.visible;
            bool tapped = WasTappedHere() && !popBlocking2 && !IsScrollDragging() && !g_input.touchConsumed;
            if (tapped) ConfigSave();

            float cy2 = pos.y + btnH * 0.5f;
            float icX = cardX0 + 28.f + 20.f;
            dl->AddCircleFilled({icX, cy2}, 20.f, C::UA(C::Acc(), 0.15f), 32);
            dl->AddCircle({icX, cy2}, 20.f, C::UA(C::Acc(), 0.7f), 32, 1.5f);
            dl->AddLine({icX - 9.f, cy2}, {icX + 9.f, cy2}, C::U(C::Acc()), 3.f);
            dl->AddLine({icX, cy2 - 9.f}, {icX, cy2 + 9.f}, C::U(C::Acc()), 3.f);

            const char* createTxt = XS("Создать конфиг");
            float textY = pos.y + (btnH - fs * 1.25f) * 0.5f;
            dl->AddText(fn, fs * 1.25f, {icX + 32.f, textY}, C::U(C::Acc()), createTxt);
        }

        SHdr(XS("Конфиги"));

        if (g_configCount == 0) {
            const float emptyH = 120.f;
            auto ep = ImGui::GetCursorScreenPos();
            dl->AddRectFilled({ep.x + inset, ep.y}, {ep.x + avW - inset, ep.y + emptyH},
                C::U(C::Card()), R::Card);
            if (g_state.ui_show_sep)
                dl->AddRect({ep.x + inset, ep.y}, {ep.x + avW - inset, ep.y + emptyH},
                    C::UA(C::Sep(), 0.9f), R::Card, 0, 1.2f);
            ImGui::Dummy({avW, emptyH});
            const char* emptyTxt = XS("Нет конфигов");
            auto eSz = fn->CalcTextSizeA(fs * 1.05f, FLT_MAX, 0, emptyTxt);
            dl->AddText(fn, fs * 1.05f,
                {ep.x + (avW - eSz.x) * 0.5f, ep.y + (emptyH - fs * 1.05f) * 0.5f},
                C::U(C::Dim()), emptyTxt);
        } else {
            auto& io2 = ImGui::GetIO();
            bool popBlocking2 = (g_pop.visible && !g_pop.closing) || g_sheet.visible;

            const float padH    = 12.f;
            const float padTop  = 14.f;
            const float padBot  = 14.f;
            const float icSz    = 62.f;
            const float nameGap = 12.f;
            const float midGap  = 10.f;
            const float btnH2   = 46.f;
            const float btnGapX =  6.f;
            const float cardGap = 10.f;

            float nameFS = fs * 1.15f;
            float subFS  = fs * 0.78f;
            float textH  = fn->CalcTextSizeA(nameFS, FLT_MAX, 0, XS("A")).y;
            float subH   = fn->CalcTextSizeA(subFS,  FLT_MAX, 0, XS("A")).y;
            float topH   = ImMax(icSz, textH + 6.f + subH);
            float cardH  = padTop + topH + midGap + btnH2 + padBot;

            for (int ci = 0; ci < g_configCount; ci++) {
                float loadA = (ci < kMaxConfigs) ? g_cfgLoadAnim[ci] : 0.f;
                float easedA = loadA * loadA * (3.f - 2.f * loadA);

                auto pos = ImGui::GetCursorScreenPos();
                float cx0 = pos.x + inset, cx1 = pos.x + avW - inset;
                float cw  = cx1 - cx0;

                {
                    ImVec4 bg = C::Card(), ac = C::Acc();
                    float mix = (g_darkTheme ? 0.14f : 0.06f) * easedA;
                    ImU32 cardFill = IM_COL32(
                        int(bg.x*255*(1.f-mix) + ac.x*255*mix),
                        int(bg.y*255*(1.f-mix) + ac.y*255*mix),
                        int(bg.z*255*(1.f-mix) + ac.z*255*mix), 255);
                    dl->AddRectFilled({cx0, pos.y}, {cx1, pos.y + cardH}, cardFill, R::Card);

                    float borderA = Lerpf(g_state.ui_show_sep ? 0.9f : 0.f, 0.65f, easedA);
                    ImVec4 borderCol = g_state.ui_show_sep
                        ? ImVec4{Lerpf(C::Sep().x, ac.x, easedA), Lerpf(C::Sep().y, ac.y, easedA), Lerpf(C::Sep().z, ac.z, easedA), borderA}
                        : ImVec4{ac.x, ac.y, ac.z, borderA};
                    if (borderA > 0.01f)
                        dl->AddRect({cx0, pos.y}, {cx1, pos.y + cardH}, C::U(borderCol), R::Card, 0, Lerpf(1.2f, 2.2f, easedA));
                }

                float rowCY = pos.y + padTop + topH * 0.5f;
                float icX  = cx0 + padH;
                float icY  = rowCY - icSz * 0.5f;
                float icCX = icX + icSz * 0.5f;

                if (g_tabIcons[3]) {
                    dl->AddImageRounded((ImTextureID)(intptr_t)g_tabIcons[3],
                        {icX, icY}, {icX + icSz, icY + icSz},
                        {0,0}, {1,1}, IM_COL32(255,255,255,255), 12.f);
                } else {
                    float icR = 14.f;
                    ImU32 icBg = g_darkTheme
                        ? IM_COL32(55, 120, 220, 255)
                        : IM_COL32(10, 122, 255, 255);
                    dl->AddRectFilled({icX, icY}, {icX + icSz, icY + icSz}, icBg, icR);
                    float lh = icSz * 0.20f;
                    float lx = icCX - lh * 0.45f, ly = rowCY + lh * 0.42f;
                    dl->AddLine({icCX - lh, rowCY}, {lx, ly},      IM_COL32(255,255,255,245), 3.2f);
                    dl->AddLine({lx, ly}, {icCX + lh, rowCY - lh}, IM_COL32(255,255,255,245), 3.2f);
                }

                const char* badgeTxt = XS("Загружен");
                auto btsz    = fn->CalcTextSizeA(subFS, FLT_MAX, 0, badgeTxt);
                float dotR   = 3.5f;
                float bPadX  = 8.f, bPadY = 5.f;
                float bW     = dotR*2.f + 5.f + btsz.x + bPadX * 2.f;
                float bH     = btsz.y + bPadY * 2.f;

                float nameAreaR = easedA > 0.01f ? (cx1 - bW * easedA - 10.f) : (cx1 - padH);
                float nameX = icX + icSz + nameGap;
                float nameY = rowCY - (textH + 6.f + subH) * 0.5f;

                dl->PushClipRect({nameX, pos.y}, {nameAreaR, pos.y + cardH}, true);
                dl->AddText(fn, nameFS, {nameX, nameY}, C::U(C::Txt()), g_configs[ci].name);
                dl->AddText(fn, subFS, {nameX, nameY + textH + 6.f}, C::UA(C::Dim(), 0.7f), XS("config"));
                dl->PopClipRect();

                if (easedA > 0.01f) {
                    ImVec4 ac = C::Acc();
                    float bX  = cx1 - bW - 10.f;
                    float bY2 = rowCY - bH * 0.5f;
                    float a   = easedA;

                    dl->AddRectFilled({bX, bY2}, {bX + bW, bY2 + bH},
                        C::UA(ac, (g_darkTheme ? 0.20f : 0.10f) * a), bH * 0.5f);
                    dl->AddRect({bX, bY2}, {bX + bW, bY2 + bH},
                        C::UA(ac, 0.55f * a), bH * 0.5f, 0, 1.3f);
                    dl->AddCircleFilled({bX + bPadX + dotR, bY2 + bH*0.5f}, dotR, C::UA(ac, 0.95f * a), 16);
                    dl->AddText(fn, subFS,
                        {bX + bPadX + dotR*2.f + 5.f, bY2 + bPadY},
                        C::UA(ac, 0.95f * a), badgeTxt);
                }

                float bY  = pos.y + padTop + topH + midGap;
                float totalBtnW = cw - padH * 2.f;
                float bw3 = (totalBtnW - btnGapX * 2.f) / 3.f;
                float b1X = cx0 + padH;
                float b2X = b1X + bw3 + btnGapX;
                float b3X = b2X + bw3 + btnGapX;

                auto DrawBtn = [&](float bx, float bw, ImU32 fillL, ImU32 fillD, ImU32 txtCol, const char* label) {
                    ImU32 fill = g_darkTheme ? fillD : fillL;
                    dl->AddRectFilled({bx, bY}, {bx + bw, bY + btnH2}, fill, R::Btn);
                    auto lsz = fn->CalcTextSizeA(fs * 0.93f, FLT_MAX, 0, label);
                    dl->PushClipRect({bx+3.f, bY}, {bx+bw-3.f, bY+btnH2}, true);
                    dl->AddText(fn, fs * 0.93f,
                        {bx + bw*0.5f - lsz.x*0.5f, bY + (btnH2 - lsz.y)*0.5f},
                        txtCol, label);
                    dl->PopClipRect();
                };

                DrawBtn(b1X, bw3,
                    IM_COL32(235, 242, 255, 255),
                    IM_COL32(30,  80,  180, 255),
                    g_darkTheme ? IM_COL32(120, 175, 255, 255) : IM_COL32(10, 100, 220, 255),
                    XS("Загрузить"));

                DrawBtn(b2X, bw3,
                    IM_COL32(232, 248, 237, 255),
                    IM_COL32(20,  80,  40,  255),
                    g_darkTheme ? IM_COL32(80, 210, 120, 255) : IM_COL32(25, 140, 60, 255),
                    XS("Сохранить"));

                DrawBtn(b3X, bw3,
                    IM_COL32(255, 237, 236, 255),
                    IM_COL32(100, 20,  20,  255),
                    g_darkTheme ? IM_COL32(255, 100, 90, 255) : IM_COL32(210, 30, 20, 255),
                    XS("Удалить"));

                ImGui::InvisibleButton(("##cfg_" + std::to_string(ci)).c_str(), {avW, cardH});

                if (!popBlocking2 && !IsScrollDragging() && !g_input.touchConsumed && io2.MouseReleased[0]) {
                    auto mp  = io2.MousePos;
                    auto cp2 = io2.MouseClickedPos[0];
                    if (mp.x  >= b1X && mp.x  <= b1X+bw3 && mp.y  >= bY && mp.y  <= bY+btnH2
                     && cp2.x >= b1X && cp2.x <= b1X+bw3 && cp2.y >= bY && cp2.y <= bY+btnH2)
                        ConfigLoad(ci);
                    else if (mp.x  >= b2X && mp.x  <= b2X+bw3 && mp.y  >= bY && mp.y  <= bY+btnH2
                     && cp2.x >= b2X && cp2.x <= b2X+bw3 && cp2.y >= bY && cp2.y <= bY+btnH2)
                        ConfigUpdate(ci);
                    else if (mp.x  >= b3X && mp.x  <= b3X+bw3 && mp.y  >= bY && mp.y  <= bY+btnH2
                     && cp2.x >= b3X && cp2.x <= b3X+bw3 && cp2.y >= bY && cp2.y <= bY+btnH2) {
                        g_configToDelete = ci;
                        PopoverOpen(XS("Удалить конфиг?"), 3);
                    }
                }

                if (ci < g_configCount - 1)
                    ImGui::Dummy({1.f, cardGap});
            }
        }

    } else {
        SHdr(XS("Интерфейс"));
        CardBg(Layout::RowH * 3);
        ToggleRow("##uf2", XS("Показать фпс"),  &g_state.ui_fps,       g_state.a_ui_fps,  false, true);
        if (ToggleRow("##ud2", XS("Тёмная тема"), &g_state.ui_dark_mode, g_state.a_ui_dark, false))
            g_darkTheme = g_state.ui_dark_mode;
        ToggleRow("##usep", XS("Разделители строк"), &g_state.ui_show_sep, g_state.a_ui_sep, true);

        SHdr(XS("Система"));
        {
            auto* dl  = ImGui::GetWindowDrawList();
            float avW = ImGui::GetContentRegionAvail().x;
            const float rowH = Layout::RowH, inset = Layout::Inset;
            auto pos = ImGui::GetCursorScreenPos();
            dl->AddRectFilled({pos.x + inset, pos.y}, {pos.x + avW - inset, pos.y + rowH}, C::U(C::Card()), R::Card);
            if (g_state.ui_show_sep)
                dl->AddRect({pos.x + inset, pos.y}, {pos.x + avW - inset, pos.y + rowH}, C::U(C::Sep()), R::Card, 0, 1.2f);
            ImGui::InvisibleButton("##exit", {avW, rowH});
            if (WasTappedHere() && !IsScrollDragging())
                PopoverOpen(XS("Выйти из приложения?"), 2);
            const char* exitTxt = XS("Выйти из приложения");
            float exitFS = ImGui::GetFontSize() * 1.15f;
            auto  tsz = ImGui::GetFont()->CalcTextSizeA(exitFS, FLT_MAX, 0, exitTxt);
            float tx  = pos.x + (avW - tsz.x) * 0.5f;
            float ty  = pos.y + (rowH - exitFS) * 0.5f;
            dl->AddText(ImGui::GetFont(), exitFS, {tx, ty}, C::U(C::Red()), exitTxt);
        }
    }

    ImGui::Dummy({1.f, 12.f});
    return ImGui::GetCursorPosY() - sY;
}

static uint32_t g_menu_orient = 0xFFFFFFFFu;
static int g_menu_dw = 0, g_menu_dh = 0;

static void VisibleScreen(float& w, float& h) {
    float dw = (float)displayInfo.width;
    float dh = (float)displayInfo.height;
    if (dw < 100.f) dw = (float)native_window_screen_x;
    if (dh < 100.f) dh = (float)native_window_screen_y;
    float mx = dw > dh ? dw : dh;
    float mn = dw < dh ? dw : dh;
    if (mx < 100.f) mx = 1080.f;
    if (mn < 100.f) mn = mx;
    bool land = (displayInfo.orientation == 1 || displayInfo.orientation == 3);
    if (dw > dh) land = true;
    else if (dh > dw && (displayInfo.orientation == 0 || displayInfo.orientation == 2)) land = false;
    w = land ? mx : mn;
    h = land ? mn : mx;
}

static void CenterMenuOnDisplay() {
    float dw = 0.f, dh = 0.f;
    VisibleScreen(dw, dh);
    if (dw < 100.f || dh < 100.f) return;
    if (g_win.w > dw - 16.f) g_win.w = ImMax(160.f, dw - 16.f);
    if (g_win.h > dh - 16.f) g_win.h = ImMax(160.f, dh - 16.f);
    g_win.pos.x = (dw - g_win.w) * 0.5f;
    g_win.pos.y = (dh - g_win.h) * 0.5f;
    g_win.dragging = false;
    g_win.resizing = false;
}

void RenderMenu() {
    auto& io = ImGui::GetIO();
    float dt = io.DeltaTime;
    if (dt > 0.1f) dt = 0.1f;

    if (g_menu_orient != displayInfo.orientation || g_menu_dw != (int)displayInfo.width || g_menu_dh != (int)displayInfo.height) {
        g_menu_orient = displayInfo.orientation;
        g_menu_dw = (int)displayInfo.width;
        g_menu_dh = (int)displayInfo.height;
        CenterMenuOnDisplay();
    }

    g_menuFadeIn += dt * 1.2f;
    if (g_menuFadeIn > 1.f) g_menuFadeIn = 1.f;

    CfgWatchTick();
    Tick(g_themeT, g_darkTheme, dt, 6.f);
    Tick(g_state.a_aim_touch,  g_state.aim_touch,          dt);
    Tick(g_state.a_aim_pos,    g_state.aim_pos,            dt);
    Tick(g_state.a_aim_head,   g_state.aim_bone == 0,      dt);
    Tick(g_state.a_aim_chest,  g_state.aim_bone == 1,      dt);
    Tick(g_state.a_aim_pelvis, g_state.aim_bone == 2,      dt);
    Tick(g_state.a_aim_spec,   g_state.aim_special,        dt);
    Tick(g_state.a_esp_box,    g_state.esp_box,            dt);
    Tick(g_state.a_esp_name,   g_state.esp_name,           dt);
    Tick(g_state.a_esp_hp,     g_state.esp_hp,             dt);
    Tick(g_state.a_esp_wall,   g_state.esp_wall,           dt);
    Tick(g_state.a_esp_chams,  g_state.esp_chams,          dt);
    Tick(g_state.a_esp_weapon, g_state.esp_weapon,         dt);
    Tick(g_state.a_esp_weapon_icon, g_state.esp_weapon_icon, dt);
    Tick(g_state.a_esp_tracer, g_state.esp_tracer,         dt);
    Tick(g_state.a_esp_skeleton, g_state.esp_skeleton,     dt);
    Tick(g_state.a_esp_money,  g_state.esp_money,          dt);
    Tick(g_state.a_esp_ping,   g_state.esp_ping,           dt);
    Tick(g_state.a_ui_fps,     g_state.ui_fps,             dt);
    Tick(g_state.a_ui_dark,    g_state.ui_dark_mode,       dt);
    Tick(g_state.a_ui_sep,     g_state.ui_show_sep,        dt);
    ApplyTheme();

    if (g_cfgLoadedIdx >= 0 && g_cfgLoadedIdx < kMaxConfigs)
        g_cfgLoadAnim[g_cfgLoadedIdx] += (1.f - g_cfgLoadAnim[g_cfgLoadedIdx]) * 10.f * dt;

    if (g_sprite.texture && SpriteState::Total > 1) {
        g_sprite.timer += dt;
        if (g_sprite.timer >= 1.f / SpriteState::FPS) {
            g_sprite.timer -= 1.f / SpriteState::FPS;
            g_sprite.frame = (g_sprite.frame + 1) % SpriteState::Total;
        }
    }

    bool anyOverlayOpen = g_sheet.visible || (g_pop.visible && !g_pop.closing);

    if (io.MouseClicked[0] && anyOverlayOpen) g_input.touchConsumed = true;

    bool anyOverlayVisible = g_sheet.visible || g_pop.visible;
    if (!io.MouseDown[0] && !anyOverlayVisible) g_input.touchConsumed = false;

    DrawWatermark(dt);
    DrawToast(dt);
    if (!menu_open) return;

    const float WW = g_win.w, WH = g_win.h;
    const float lW_ = 265, hH_ = 66;

    {
        bool inResize = io.MousePos.x >= g_win.pos.x + g_win.w - R::Card - 14.f
                     && io.MousePos.x <= g_win.pos.x + g_win.w + 14.f
                     && io.MousePos.y >= g_win.pos.y + g_win.h - R::Card - 14.f
                     && io.MousePos.y <= g_win.pos.y + g_win.h + 14.f;

        if (!io.MouseDown[0]) g_win.resizing = false;

        bool anyOverlay = g_sheet.visible || (g_pop.visible && !g_pop.closing);
        if (!anyOverlay && io.MouseClicked[0] && inResize) {
            g_win.resizing         = true;
            g_win.resizeTouchStart = io.MousePos;
            g_win.sizeStart        = {g_win.w, g_win.h};
        }

        if (g_win.resizing && io.MouseDown[0]) {
            float dx = io.MousePos.x - g_win.resizeTouchStart.x;
            float dy = io.MousePos.y - g_win.resizeTouchStart.y;
            g_win.w = ImMax(WW_MIN, g_win.sizeStart.x + dx);
            g_win.h = ImMax(WH_MIN, g_win.sizeStart.y + dy);
        }
    }

    {
        if (!io.MouseDown[0]) g_win.dragging = false;

        const float tabsStartY = g_win.pos.y + 120.f;

        if (!g_win.dragging && io.MouseDown[0] && !(g_sheet.visible || (g_pop.visible && !g_pop.closing)) && !g_win.resizing) {
            float tx = io.MouseClickedPos[0].x, ty = io.MouseClickedPos[0].y;
            bool inLeftHeader = tx >= g_win.pos.x      && tx < g_win.pos.x + lW_
                             && ty >= g_win.pos.y       && ty < tabsStartY;
            bool inRHdr       = tx >= g_win.pos.x + lW_ && tx < g_win.pos.x + g_win.w
                             && ty >= g_win.pos.y        && ty < g_win.pos.y + hH_;
            if (inLeftHeader || inRHdr) {
                float dx = io.MousePos.x - io.MouseClickedPos[0].x;
                float dy = io.MousePos.y - io.MouseClickedPos[0].y;
                if (fabsf(dx) + fabsf(dy) > 8.f) {
                    g_win.dragging   = true;
                    g_win.touchStart = io.MouseClickedPos[0];
                    g_win.posStart   = g_win.pos;
                }
            }
        }

        if (g_win.dragging && io.MouseDown[0]) {
            float dx   = io.MousePos.x - g_win.touchStart.x;
            float dy   = io.MousePos.y - g_win.touchStart.y;
            g_win.pos.x = g_win.posStart.x + dx;
            g_win.pos.y = g_win.posStart.y + dy;
        }
        float dw = 0.f, dh = 0.f;
        VisibleScreen(dw, dh);
        if (dw > 100.f && dh > 100.f) {
            g_win.pos.x = ImClamp(g_win.pos.x, 0.f, ImMax(0.f, dw - g_win.w));
            g_win.pos.y = ImClamp(g_win.pos.y, 0.f, ImMax(0.f, dh - g_win.h));
        }
    }

    ImGui::SetNextWindowSize({g_win.w, g_win.h}, ImGuiCond_Always);
    ImGui::SetNextWindowPos(g_win.pos, ImGuiCond_Always);

    ImGuiWindowFlags wf = ImGuiWindowFlags_NoResize    | ImGuiWindowFlags_NoCollapse
                        | ImGuiWindowFlags_NoTitleBar  | ImGuiWindowFlags_NoScrollbar
                        | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBackground
                        | ImGuiWindowFlags_NoMove      | ImGuiWindowFlags_NoNav;

    ImGui::Begin("##menu", nullptr, wf);
    {
        auto  bgP  = g_win.pos;
        auto* bgDL = ImGui::GetWindowDrawList();
        bgDL->AddRectFilled(bgP, {bgP.x + WW, bgP.y + WH}, C::U(C::Bg()), R::Card);
    }

    ImVec2 wp = g_win.pos;
    auto*  dl = ImGui::GetWindowDrawList();
    const float lW = 265, cH = g_win.h, cW = g_win.w - lW;

    g_state.tab_alpha += (1.f - g_state.tab_alpha) * 5.f * dt;
    if (g_state.tab_alpha > .999f) g_state.tab_alpha = 1.f;
    SpringTick(g_state.tab_slide, g_state.tab_slide_vel, 0.f, dt);

    ImGui::SetCursorPos({0, 0});
    ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(0, 0, 0, 0));
    ImGui::BeginChild("##lp", {lW, cH}, false, ImGuiWindowFlags_NoScrollbar);
    auto  lpPos = ImGui::GetWindowPos();
    auto* ldl   = ImGui::GetWindowDrawList();
    ldl->AddRectFilled(lpPos, {lpPos.x + lW, lpPos.y + cH}, C::U(C::LeftBg()), R::Card);

    {
        float t = EaseInOut(g_themeT);
        ImU32 fill[3] = {IM_COL32(255, 95, 87, 255), IM_COL32(255, 189, 46, 255), IM_COL32(40, 200, 64, 255)};
        for (int i = 0; i < 3; i++) {
            float cx2 = lpPos.x + 28.f + i * 34.f, cy2 = lpPos.y + 32.f;
            ldl->AddCircleFilled({cx2, cy2}, 13.f, fill[i], 48);
            ImU32 bordCol = IM_COL32(
                int(Lerpf(0,   255, t)),
                int(Lerpf(0,   255, t)),
                int(Lerpf(0,   255, t)),
                int(Lerpf(180, 210, t))
            );
            ldl->AddCircle({cx2, cy2}, 14.2f, bordCol, 48, 1.5f);
        }
    }

    {
        const float Rad = lW * 0.28f;
        float cx = lpPos.x + lW * 0.5f, cy = lpPos.y + 30.f + 20.f + Rad + 10.f;
        ldl->AddCircleFilled({cx + 2.f, cy + 3.f}, Rad, IM_COL32(0, 0, 0, 20), 64);
        ldl->AddCircleFilled({cx, cy}, Rad, C::U(C::Card()), 64);
        if (g_sprite.texture) {
            int col = g_sprite.frame % SpriteState::Cols;
            int row = g_sprite.frame / SpriteState::Cols;
            float u0 = col       / (float)SpriteState::Cols;
            float u1 = (col + 1) / (float)SpriteState::Cols;
            float v0 = row       / (float)SpriteState::Rows;
            float v1 = (row + 1) / (float)SpriteState::Rows;
            ldl->AddImageRounded((ImTextureID)(intptr_t)g_sprite.texture,
                {cx - Rad, cy - Rad}, {cx + Rad, cy + Rad}, {u0, v0}, {u1, v1}, IM_COL32(255, 255, 255, 255), Rad);
        } else {
            ImU32 avatarBg   = g_darkTheme ? IM_COL32(40, 60, 90, 255)   : IM_COL32(200, 218, 240, 255);
            ImU32 avatarFig  = g_darkTheme ? IM_COL32(70, 100, 145, 255)  : IM_COL32(120, 148, 185, 255);
            ldl->AddCircleFilled({cx, cy}, Rad, avatarBg, 64);
            float hR = Rad * 0.30f;
            ldl->AddCircleFilled({cx, cy - Rad * 0.22f}, hR, avatarFig, 32);
            ldl->AddRectFilled({cx - Rad * 0.42f, cy + Rad * 0.08f}, {cx + Rad * 0.42f, cy + Rad * 0.82f}, avatarFig, Rad * 0.22f);
        }
        {
            float t = EaseInOut(g_themeT);
            float ringAlpha = Lerpf(0.72f, 0.90f, t);
            ldl->AddCircle({cx, cy}, Rad + 2.5f, C::UA(C::Acc(), ringAlpha), 64, 2.f);
        }
        ImGui::SetCursorPosY(30.f + 20.f + Rad * 2.f + 10.f + 18.f);
    }

    {
        float y = ImGui::GetCursorScreenPos().y;
        ldl->AddLine({lpPos.x + 20.f, y}, {lpPos.x + lW - 20.f, y}, C::U(C::Sep()), 0.8f);
        ImGui::Dummy({1.f, 2.f});
    }

    const char* tabNames[5] = {
        XS("Главная"), XS("Аимбот"), XS("Визуалы"), XS("Аимбот"), XS("Настройки")
    };
    const float tabH = Layout::TabH, tabPad = Layout::TabPad;

    ImGui::Dummy({1.f, 6.f});
    for (int i = 0; i < 5; i++) {
        auto pos2 = ImGui::GetCursorScreenPos();
        tab_rects[i] = {pos2.y};
        char tabId[16];
        snprintf(tabId, sizeof(tabId), "##tab%d", i);
        ImGui::InvisibleButton(tabId, {lW, tabH});
        if (WasTappedHere() && !IsScrollDragging() && !g_input.touchConsumed && g_state.cur_tab != i) {
            g_state.cur_tab       = i;
            g_state.tab_alpha     = 0.f;
            g_state.tab_slide     = 50.f;
            g_state.tab_slide_vel = 0.f;
            g_scrollMain          = {};
        }
    }

    {
        float targetRelY = tab_rects[g_state.cur_tab].sy - wp.y;
        if (pill_y < 0.f) { pill_y = targetRelY; pill_vel = 0.f; }
        SpringTick(pill_y, pill_vel, targetRelY, dt);
    }
    float pill_screen_y = wp.y + pill_y;

    {
        auto*       fdl = ImGui::GetForegroundDrawList();
        const float pad = 8.f, pR = R::Pill;
        float px0 = wp.x + pad, px1 = wp.x + lW - pad;
        float py   = pill_screen_y, ph = tabH;
        float py0  = py + tabPad - 4.f, py1 = py + ph - tabPad + 4.f;
        fdl->AddRectFilled({px0, py0}, {px1, py1}, C::U(C::Card()), pR);
        fdl->AddRect({px0, py0}, {px1, py1}, C::U(C::Acc()), pR, 0, 4.f);
    }

    {
        auto* fdl = ImGui::GetForegroundDrawList();
        const float iconSize = 52.f;
        const float gap      = 16.f;
        const float leftPad  = 18.f;
        const float startX   = wp.x + leftPad;
        for (int i = 0; i < 5; i++) {
            float    py  = tab_rects[i].sy;
            ImVec4   col = (i == g_state.cur_tab) ? C::Acc() : C::Dim();
            const float tfs = ImGui::GetFontSize() * 1.15f;
            auto tsz = ImGui::GetFont()->CalcTextSizeA(tfs, FLT_MAX, 0, tabNames[i]);
            float centerY = py + tabH * 0.5f;
            if (g_tabIcons[i]) {
                ImVec2 iMin = {startX, centerY - iconSize * 0.5f};
                ImVec2 iMax = {startX + iconSize, centerY + iconSize * 0.5f};
                fdl->AddImageRounded((ImTextureID)(intptr_t)g_tabIcons[i], iMin, iMax, {0,0}, {1,1}, IM_COL32(255,255,255,255), 10.f);
            }
            fdl->AddText(ImGui::GetFont(), tfs,
                {startX + iconSize + gap, centerY - tsz.y * 0.5f}, C::U(col), tabNames[i]);
        }
    }

    ImGui::EndChild();
    ImGui::PopStyleColor();

    ImGui::SetCursorPos({lW, 0});
    ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(0, 0, 0, 0));
    ImGui::BeginChild("##cp", {cW, cH}, false,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    {
        const char* titles[5] = {XS("Главная"), XS("Аимбот"), XS("Визуалы"), XS("Аимбот"), XS("Настройки")};
        auto*  cdl = ImGui::GetWindowDrawList();
        auto   hp  = ImGui::GetWindowPos();
        const float hH = Layout::HeaderH;
        const float titleFS = ImGui::GetFontSize() * 1.45f;
        auto tsz = ImGui::GetFont()->CalcTextSizeA(titleFS, FLT_MAX, 0, titles[g_state.cur_tab]);
        cdl->AddText(ImGui::GetFont(), titleFS,
            {hp.x + (cW - tsz.x) * 0.5f, hp.y + (hH - tsz.y) * 0.5f}, C::U(C::Txt()), titles[g_state.cur_tab]);
        ImGui::Dummy({cW, hH});
    }

    {
        const float areaH = cH - Layout::HeaderH;
        auto cpPos  = ImGui::GetWindowPos(); cpPos.y += Layout::HeaderH;
        const float cpRight = cpPos.x + cW;

        bool sheetBlocking = g_sheet.visible || (g_pop.visible && !g_pop.closing);
        static float s_maxScroll = 0.f;

        bool clickInRight = (io.MouseClickedPos[0].x >= cpPos.x)
                         && (io.MouseClickedPos[0].x <= cpRight)
                         && (io.MouseClickedPos[0].y >= cpPos.y)
                         && (io.MouseClickedPos[0].y <= cpPos.y + areaH);
        bool mIn = clickInRight
            && io.MousePos.x >= cpPos.x  && io.MousePos.x <= cpRight
            && io.MousePos.y >= cpPos.y  && io.MousePos.y <= cpPos.y + areaH;

        auto* cdl = ImGui::GetWindowDrawList();
        cdl->PushClipRect(cpPos, {cpRight, cpPos.y + areaH - 4.f}, true);

        float slideOffset = g_state.tab_slide;
        ImGui::SetCursorPosY(Layout::HeaderH - g_scrollMain.off + slideOffset);
        ImGui::SetCursorPosX(12.f);

        if (sheetBlocking) ImGui::BeginDisabled(true);
        float contentH = TabContent(g_state.cur_tab, dt, cW);
        if (sheetBlocking) ImGui::EndDisabled();

        if (g_state.tab_alpha < 0.999f) {
            float fadeA = 1.f - g_state.tab_alpha;
            cdl->AddRectFilled(cpPos, {cpRight, cpPos.y + areaH},
                C::UA(C::Bg(), fadeA), R::Card, ImDrawFlags_RoundCornersBottomRight);
        }

        s_maxScroll = ImMax(0.f, contentH - (areaH - 4.f));
        ScrollTick(g_scrollMain, mIn, sheetBlocking, s_maxScroll, dt);

        if (s_maxScroll > 0.f) {
            const float sbPad  = 5.f;
            const float sbVPad = 8.f;
            const float sbMinH = 36.f;
            const float sbW    = 8.f;

            float ratio     = (areaH * 0.28f) / (areaH + s_maxScroll);
            float sbHTarget = ImMax(sbMinH, areaH * ratio);
            float sbTrack   = areaH - sbHTarget - sbVPad * 2.f;
            float sbTTarget = (s_maxScroll > 0.f) ? (g_scrollMain.off / s_maxScroll) * sbTrack : 0.f;

            g_scrollMain.sb_w  = sbW;
            g_scrollMain.sb_y += (sbTTarget - g_scrollMain.sb_y) * 10.f * dt;
            g_scrollMain.sb_h += (sbHTarget - g_scrollMain.sb_h) * 10.f * dt;

            float sbX0 = cpRight - sbPad - sbW;
            float sbX1 = sbX0 + sbW;
            float sbY0 = cpPos.y + sbVPad + g_scrollMain.sb_y;
            float sbY1 = sbY0 + g_scrollMain.sb_h;

            ImU32 thumbCol = C::UA(C::Dim(), 0.55f);

            auto* fdl = ImGui::GetForegroundDrawList();
            fdl->PushClipRect(cpPos, {cpRight, cpPos.y + areaH}, true);
            fdl->AddRectFilled({sbX0, sbY0}, {sbX1, sbY1}, thumbCol, sbW * 0.5f);
            fdl->PopClipRect();
        }

        cdl->PopClipRect();
    }

    ImGui::EndChild();
    ImGui::PopStyleColor();

    {
        static constexpr float kResizePad       = 22.f;
        static constexpr float kResizeStroke     = 10.f;
        static constexpr float kResizeGlowStroke = 6.f;
        static constexpr float kResizeGlowAlpha  = 0.13f;
        static constexpr float kArcStroke        = 14.f;
        static constexpr float kArcA0            = -0.25f;
        static constexpr float kArcA1            = IM_PI * 0.5f + 0.25f;
        static constexpr float kCornerOffset     = 10.f;

        auto* rdl = ImGui::GetForegroundDrawList();

        if (g_win.resizing) {
            const float pad = kResizePad, rr = R::Card + pad;
            float t = EaseInOut(g_themeT);
            ImVec4 bc = {Lerpf(1.f, C::Dark::Acc.x, t), Lerpf(1.f, C::Dark::Acc.y, t), Lerpf(1.f, C::Dark::Acc.z, t), Lerpf(0.9f, 0.8f, t)};
            rdl->AddRect({wp.x - pad,       wp.y - pad},
                         {wp.x + g_win.w + pad, wp.y + g_win.h + pad},
                         C::U(bc), rr, 0, kResizeStroke);
            rdl->AddRect({wp.x - pad - 8.f,       wp.y - pad - 8.f},
                         {wp.x + g_win.w + pad + 8.f, wp.y + g_win.h + pad + 8.f},
                         C::UA(bc, kResizeGlowAlpha), rr + 8.f, 0, kResizeGlowStroke);
        }

        float cx = wp.x + g_win.w - R::Card - kCornerOffset;
        float cy = wp.y + g_win.h - R::Card - kCornerOffset;

        ImU32       col   = g_darkTheme ? IM_COL32(200, 200, 210, 255) : IM_COL32(22, 22, 24, 255);
        const float r     = R::Card;
        const float thick = kArcStroke;
        const float a0    = kArcA0;
        const float a1    = kArcA1;

        rdl->PathArcTo({cx, cy}, r, a0, a1, 48);
        rdl->PathStroke(col, 0, thick);

        float capR = thick * 0.5f;
        rdl->AddCircleFilled({cx + r * cosf(a0), cy + r * sinf(a0)}, capR, col, 24);
        rdl->AddCircleFilled({cx + r * cosf(a1), cy + r * sinf(a1)}, capR, col, 24);
    }

    DrawSheet(dt, wp, WW, WH);
    DrawPopover(dt, wp, WW, WH);
    ImGui::End();
    if (g_menuFadeIn < 1.f) {
        float mA = 1.f - EaseInOut(g_menuFadeIn);
        auto* ofg = ImGui::GetForegroundDrawList();
        ImVec4 bgC = C::Bg();
        ofg->AddRectFilled(g_win.pos, {g_win.pos.x + WW, g_win.pos.y + WH},
            IM_COL32(int(bgC.x*255), int(bgC.y*255), int(bgC.z*255), int(mA * 255)), R::Card);
    }
}

int main(int argc, char* argv[]) {
    prot::Init();
    screen_config();
    int abs_ScreenX = displayInfo.height > displayInfo.width ? displayInfo.height : displayInfo.width;
    int abs_ScreenY = displayInfo.height < displayInfo.width ? displayInfo.height : displayInfo.width;

    g_sw = static_cast<float>(abs_ScreenX);
    g_sh = static_cast<float>(abs_ScreenY);

    native_window_screen_x = abs_ScreenX;
    native_window_screen_y = abs_ScreenY;
    if (!initGUI_draw(native_window_screen_x, native_window_screen_x, true)) return -1;
    Blur::Init();
    CfgWatchInit();
    AudioInit();
    Touch_Init(displayInfo.width, displayInfo.height, displayInfo.orientation, true);
    start_attach_thread();
    LoadAnimeImage();
    LoadTabIcons();
    ApplyTheme();
    CfgScanDir();
    CenterMenuOnDisplay();
    g_menuFadeIn = 0.f;

    while (main_thread_flag) {
        g_frame_done.store(false);
        drawBegin();

        ui::bar::set_game_alpha(0.f);
        DrawEspOverlay();
        RenderMenu();
        drawEnd();
        g_frame_done.store(true);
    }
    while (!g_frame_done.load()) {}
    if (g_sprite.texture) { glDeleteTextures(1, &g_sprite.texture); g_sprite.texture = 0; }
    stop_attach_thread();
    if (g_esp_attached) {
        esp_reset();
        g_esp_attached = false;
    }
    Blur::Free();
    CfgWatchFree();
    AudioFree();
    shutdown(); Touch_Close(); return 0;
}
