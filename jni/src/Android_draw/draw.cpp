#include "Android_draw/draw.h"
#include "ImGui/Font/Font.h"
#include <dirent.h>
#include <sys/system_properties.h>
#include <time.h>
#include <cmath>
#include <cstdlib>
#include <cstring>

EGLDisplay display = EGL_NO_DISPLAY;
EGLConfig  config;
EGLSurface surface = EGL_NO_SURFACE;
EGLContext context = EGL_NO_CONTEXT;

ANativeWindow *native_window;

int native_window_screen_x = 0;
int native_window_screen_y = 0;
anwc::ANativeWindowCreator::DisplayInfo displayInfo{0};
uint32_t orientation  = 0;
bool     g_Initialized = false;
ImGuiWindow *g_window  = nullptr;
static float g_peak_hz = 60.f;
static float g_overlay_fps = 60.f;
static double g_prev_swap = 0.0;
static int g_refresh_tick = 0;

static float SnapHz(float h) {
    const float c[] = {60.f, 90.f, 120.f, 144.f, 165.f, 180.f, 240.f};
    float best = 60.f, bd = 1e9f;
    for (float x : c) {
        float d = fabsf(h - x);
        if (d < bd) { bd = d; best = x; }
    }
    if (bd <= 8.f) return best;
    if (h >= 50.f && h <= 240.f) return h;
    return 60.f;
}

static void ConsiderHz(float& best, float h) {
    if (h >= 50.f && h <= 240.f && h > best) best = h;
}

static void ScanModesFile(float& best, const char* path) {
    FILE* f = fopen(path, "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        float hz = 0.f;
        const char* p = line;
        while (*p) {
            if ((*p == '-' || *p == 'p' || *p == 'P' || *p == 'R' || *p == '@' || *p == ' ') && p[1] >= '0' && p[1] <= '9') {
                hz = strtof(p + 1, nullptr);
                ConsiderHz(best, hz);
            }
            ++p;
        }
    }
    fclose(f);
}

static float DetectPeakRefresh() {
    float best = 0.f;
    const char* props[] = {
        "persist.sys.sf.peak_refresh_rate",
        "ro.surface_flinger.max_frame_rate",
        "ro.vendor.display.default_fps",
        "persist.vendor.display.refresh_rate",
        "ro.vendor.display.refresh_rate",
        "vendor.display.refresh_rate",
        "persist.sys.display.refresh_rate",
        "ro.sf.lcd_fps"
    };
    char buf[128];
    for (const char* k : props) {
        memset(buf, 0, sizeof(buf));
        if (__system_property_get(k, buf) > 0)
            ConsiderHz(best, strtof(buf, nullptr));
    }
    DIR* d = opendir("/sys/class/drm");
    if (d) {
        while (dirent* e = readdir(d)) {
            if (e->d_name[0] == '.') continue;
            char path[320];
            snprintf(path, sizeof(path), "/sys/class/drm/%s/modes", e->d_name);
            ScanModesFile(best, path);
        }
        closedir(d);
    }
    ScanModesFile(best, "/sys/class/graphics/fb0/modes");
    ScanModesFile(best, "/sys/class/lcd/panel/panel_refresh_rate");
    if (best < 50.f) best = 60.f;
    return SnapHz(best);
}

static void ApplyOverlayRefresh(ANativeWindow* w, float hz) {
    if (!w) return;
    using SetFR2 = int32_t(*)(ANativeWindow*, float, int8_t, int8_t);
    using SetFR  = int32_t(*)(ANativeWindow*, float, int8_t);
    using SetBC  = int32_t(*)(ANativeWindow*, size_t);
    static SetFR2 setfr2 = (SetFR2)dlsym(RTLD_DEFAULT, "ANativeWindow_setFrameRateWithChangeStrategy");
    static SetFR  setfr  = (SetFR)dlsym(RTLD_DEFAULT, "ANativeWindow_setFrameRate");
    static SetBC  setbc  = (SetBC)dlsym(RTLD_DEFAULT, "ANativeWindow_setBufferCount");
    static bool buffers_set = false;
    if (setbc && !buffers_set) { setbc(w, 4); buffers_set = true; }
    if (hz > 61.f) {
        if (setfr2) setfr2(w, hz, 0, 1);
        else if (setfr) setfr(w, hz, 0);
    }
    if (display != EGL_NO_DISPLAY) eglSwapInterval(display, 0);
}

float overlay_fps() {
    return g_overlay_fps;
}

bool initGUI_draw(uint32_t _screen_x, uint32_t _screen_y, bool log) {
    orientation = displayInfo.orientation;
    if (!init_egl(_screen_x, _screen_y, log)) return false;
    if (!ImGui_init()) return false;
    return true;
}

bool init_egl(uint32_t _screen_x, uint32_t _screen_y, bool log) {
    static const char wn[] = {0x53,0x75,0x72,0x66,0x61,0x63,0x65,0x00};
    ::native_window = anwc::ANativeWindowCreator::Create(wn, _screen_x, _screen_y, false);
    ANativeWindow_acquire(native_window);

    display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (display == EGL_NO_DISPLAY) return false;
    if (eglInitialize(display, 0, 0) != EGL_TRUE) return false;

    EGLint num_config = 0;
    const EGLint attribList[] = {
        EGL_SURFACE_TYPE,      EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE,   EGL_OPENGL_ES2_BIT,
        EGL_BLUE_SIZE,  5,
        EGL_GREEN_SIZE, 6,
        EGL_RED_SIZE,   5,
        EGL_BUFFER_SIZE, 32,
        EGL_DEPTH_SIZE,  16,
        EGL_STENCIL_SIZE, 8,
        EGL_NONE
    };
    const EGLint attrib_list[] = {
        EGL_CONTEXT_CLIENT_VERSION, 3,
        EGL_NONE
    };

    if (eglChooseConfig(display, attribList, &config, 1, &num_config) != EGL_TRUE) return false;

    EGLint egl_format;
    eglGetConfigAttrib(display, config, EGL_NATIVE_VISUAL_ID, &egl_format);
    ANativeWindow_setBuffersGeometry(native_window, 0, 0, egl_format);

    context = eglCreateContext(display, config, EGL_NO_CONTEXT, attrib_list);
    if (context == EGL_NO_CONTEXT) return false;

    surface = eglCreateWindowSurface(display, config, native_window, nullptr);
    if (surface == EGL_NO_SURFACE) return false;

    if (!eglMakeCurrent(display, surface, surface, context)) return false;

    eglSwapInterval(display, 0);
    g_peak_hz = DetectPeakRefresh();
    ApplyOverlayRefresh(::native_window, g_peak_hz);

    return true;
}

void screen_config() {
    displayInfo = anwc::ANativeWindowCreator::GetDisplayInfo();
}

bool ImGui_init() {
    if (g_Initialized) return true;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();

    ImGui_ImplAndroid_Init(native_window);
    ImGui_ImplOpenGL3_Init("#version 300 es");

    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = NULL;
    io.DisplaySize = ImVec2((float)native_window_screen_x, (float)native_window_screen_y);

    io.Fonts->AddFontFromMemoryTTF(
        (void*)RobotoFont,
        (int)RobotoFont_size,
        28.0f
    );

    ImGui::GetStyle().ScaleAllSizes(1.5f);
    ImGui::GetStyle().WindowRounding = 14.0f;

    g_Initialized = true;
    return true;
}

void drawBegin() {
    screen_config();

    if ((++g_refresh_tick % 180) == 0)
        ApplyOverlayRefresh(::native_window, g_peak_hz);

    if (::orientation != displayInfo.orientation) {
        ::orientation = displayInfo.orientation;
        UpdateScreenData(displayInfo.width, displayInfo.height, displayInfo.orientation);
    }

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplAndroid_NewFrame();
    {
        float sx = (float)native_window_screen_x;
        float a = (float)displayInfo.width;
        float b = (float)displayInfo.height;
        if (a >= 100.f && b >= 100.f) sx = a > b ? a : b;
        if (sx < 100.f) sx = 1080.f;
        ImGui::GetIO().DisplaySize = ImVec2(sx, sx);
    }
    ImGui::NewFrame();
}

void drawEnd() {
    ImGui::Render();
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    eglSwapBuffers(display, surface);

    struct timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    double now = (double)ts.tv_sec + (double)ts.tv_nsec / 1000000000.0;
    float pace = g_peak_hz > 61.f ? g_peak_hz : 120.f;
    if (g_prev_swap > 0.0) {
        double target = 1.0 / (double)pace;
        double elapsed = now - g_prev_swap;
        if (elapsed > 0.0 && elapsed < target - 0.00035) {
            double sl = target - elapsed;
            struct timespec req{};
            req.tv_sec = (time_t)sl;
            req.tv_nsec = (long)((sl - (double)req.tv_sec) * 1000000000.0);
            clock_nanosleep(CLOCK_MONOTONIC, 0, &req, nullptr);
            clock_gettime(CLOCK_MONOTONIC, &ts);
            now = (double)ts.tv_sec + (double)ts.tv_nsec / 1000000000.0;
        }
    }
    if (g_prev_swap > 0.0) {
        float d = (float)(now - g_prev_swap);
        if (d > 0.0002f && d < 0.25f)
            g_overlay_fps += (1.f / d - g_overlay_fps) * 0.12f;
    }
    g_prev_swap = now;
}

void shutdown() {
    if (!g_Initialized) return;

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplAndroid_Shutdown();
    ImGui::DestroyContext();

    if (display != EGL_NO_DISPLAY) {
        eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (context != EGL_NO_CONTEXT) eglDestroyContext(display, context);
        if (surface != EGL_NO_SURFACE) eglDestroySurface(display, surface);
        eglTerminate(display);
    }
    display = EGL_NO_DISPLAY;
    context = EGL_NO_CONTEXT;
    surface = EGL_NO_SURFACE;

    ANativeWindow_release(native_window);
    anwc::ANativeWindowCreator::Destroy(native_window);
    g_Initialized = false;
}