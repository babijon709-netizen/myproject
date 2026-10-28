// intercept.c — два inline-хука (Dobby), которых не хватало контейнеру.
//
// 1) sigaction gate. Рекордер падений (probe.c) ставит свои обработчики на SIGSEGV/…, но
//    Crashlytics из игры ставит свои поверх через sigaction уже через несколько секунд после
//    старта — и тогда нативный трейс не пишется вообще. Раньше это лечили переустановкой
//    обработчика каждые 500 мс (гонка, которую можно проиграть). Теперь перехватывается сам
//    sigaction: любой, кто пытается занять фатальный сигнал, получает отказ (вызывается наш
//    обработчик), а в лог пишется адрес того, кто лез.
//
// 2) dlopen gate. Трюк из deavision-загрузчика: не пускать libvulkan.so, чтобы движок ушёл на
//    GLES. У нас Vulkan инициализируется в чужом процессе (наш контейнер), и падение после
//    инициализации сцены — ровно то место, где это может стрелять. Включается галочкой в UI,
//    чтобы можно было сравнить два прогона одним apk.
//
// Оба хука ставятся один раз, из MainActivity до запуска движка.

#include <android/log.h>
#include <android/dlext.h>
#include <dlfcn.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>

#include "dobby.h"

#define TAG "oxhk.hook"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

typedef int (*sigaction_t)(int, const struct sigaction*, struct sigaction*);
typedef void* (*dlopen_t)(const char*, int);
typedef void* (*dlopen_ext_t)(const char*, int, const struct android_dlextinfo*);

static sigaction_t   real_sigaction;
static dlopen_t      real_dlopen;
static dlopen_ext_t  real_dlopen_ext;

static struct sigaction g_ours;       // обработчик probe.c
static int  g_have_ours;
static volatile int g_block_vulkan = 0;
static volatile int g_busy = 0;       // мы внутри своего хука: не логировать/не блокировать
static volatile int g_installed = 0;
static volatile int g_denied = 0;

static int fatal_sig(int s) {
    return s == SIGSEGV || s == SIGBUS || s == SIGABRT || s == SIGILL || s == SIGFPE;
}

static const char* base_of(const char* p) {
    if (!p) return "?";
    const char* s = strrchr(p, '/');
    return s ? s + 1 : p;
}

// только те библиотеки, которые нам интересны: иначе dlopen-лог зальёт весь logcat
static int interesting(const char* name) {
    static const char* keys[] = {
        "libil2cpp", "libunity", "libmain", "libloader", "libpglarmor", "lib_burst",
        "libmonobdwgc", "libcrashlytics", "libgfx", "libvulkan", "libhwui", "libEGL",
        "libGLES", "libapp.so", "libflutter",
    };
    for (unsigned i = 0; i < sizeof keys / sizeof keys[0]; ++i)
        if (strstr(name, keys[i])) return 1;
    return 0;
}

// ── sigaction ───────────────────────────────────────────────────────────────────────────────
static int my_sigaction(int sig, const struct sigaction* act, struct sigaction* old) {
    if (act && g_have_ours && fatal_sig(sig) && act->sa_sigaction != g_ours.sa_sigaction) {
        if (old) real_sigaction(sig, NULL, old);
        int rc = real_sigaction(sig, &g_ours, NULL);
        LOGI("sigaction(%d) takeover blocked: %p wanted it, recorder kept (rc=%d)",
             sig, (void*) act->sa_sigaction, rc);
        return rc;
    }
    return real_sigaction(sig, act, old);
}

// ── dlopen ──────────────────────────────────────────────────────────────────────────────────
static const char* vulkan_wanted(const char* name) {
    if (!name || !g_block_vulkan) return NULL;
    return strstr(name, "libvulkan") ? name : NULL;
}

static void* my_dlopen(const char* name, int flags) {
    if (!g_busy) {
        if (vulkan_wanted(name)) {
            g_denied++;
            LOGI("dlopen(%s) blocked -> force gles (%d denied)", name, g_denied);
            return NULL;
        }
    }
    void* h = real_dlopen(name, flags);
    if (name && !g_busy && interesting(name))
        LOGI("dlopen %s -> %p %s", base_of(name), h, h ? "ok" : "fail");
    return h;
}

static void* my_dlopen_ext(const char* name, int flags, const struct android_dlextinfo* info) {
    if (!g_busy) {
        if (vulkan_wanted(name)) {
            g_denied++;
            LOGI("android_dlopen_ext(%s) blocked -> force gles (%d denied)", name, g_denied);
            return NULL;
        }
    }
    void* h = real_dlopen_ext(name, flags, info);
    if (name && !g_busy && interesting(name))
        LOGI("dlopen_ext %s -> %p %s", base_of(name), h, h ? "ok" : "fail");
    return h;
}

// ── установка ───────────────────────────────────────────────────────────────────────────────
// Вызывается из probe.c после того, как рекордер поставлен: обработчик из sa становится
// «нашим», и с этого момента его никто не может заменить.
void oxhk_intercept_arm(const struct sigaction* sa, int block_vulkan) {
    if (sa) {
        g_ours = *sa;
        g_have_ours = 1;
    }
    g_block_vulkan = block_vulkan ? 1 : 0;
    if (g_installed) return;

    void* sym = (void*) dlsym(RTLD_DEFAULT, "sigaction");
    if (!sym) sym = DobbySymbolResolver("/system/lib64/libc.so", "sigaction");
    void* dl = (void*) dlsym(RTLD_DEFAULT, "dlopen");
    if (!dl) dl = DobbySymbolResolver("/system/lib64/libdl.so", "dlopen");
    void* dlext = (void*) dlsym(RTLD_DEFAULT, "android_dlopen_ext");
    if (!dlext) dlext = DobbySymbolResolver("/system/lib64/libdl.so", "android_dlopen_ext");

    int rc_sa = -1, rc_dl = -1, rc_ext = -1;
    if (sym) {
        rc_sa = DobbyHook(sym, (void*) my_sigaction, (void**) &real_sigaction);
    }
    if (dl) {
        rc_dl = DobbyHook(dl, (void*) my_dlopen, (void**) &real_dlopen);
    }
    if (dlext) {
        rc_ext = DobbyHook(dlext, (void*) my_dlopen_ext, (void**) &real_dlopen_ext);
    }
    g_installed = 1;
    LOGI("intercept armed: sigaction=%p rc=%d, dlopen=%p rc=%d, dlopen_ext=%p rc=%d, vulkan_block=%d",
         sym, rc_sa, (void*) dl, rc_dl, (void*) dlext, rc_ext, (int) g_block_vulkan);
    if (rc_sa != 0 && rc_dl != 0 && rc_ext != 0)
        LOGE("intercept: no hook took — falling back to re-arming the recorder from java");
}

int oxhk_intercept_ok(void) {
    return g_installed && real_sigaction != NULL;
}
