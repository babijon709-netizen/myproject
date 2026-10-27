// probe.c — can this process load the game's native code at all?
//
// The whole dual-space plan lives or dies on one thing: on Android 10+ an app cannot dlopen a
// file it can write to (W^X), and it cannot dlopen another app's lib out of /data/app either
// (SELinux, apk_data_file is read-only for untrusted_app). This measures every candidate on the
// actual device instead of guessing:
//
//   1. control       dlopen our own lib (must work)
//   2. game lib dir  dlopen /data/app/.../lib/arm64/libmain.so directly
//   3. app copy      dlopen libmain.so copied into our files dir
//   4. memfd         android_dlopen_ext() over an in-memory fd
//
// Every step reports its own error, so one screenshot says which way the container goes.

#include <jni.h>
#include <android/dlext.h>
#include <android/log.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 1
#endif
#ifndef __NR_memfd_create
#define __NR_memfd_create 279
#endif

#define TAG "oxhk.probe"

static void add(char *b, size_t cap, const char *fmt, ...) {
    size_t n = strlen(b);
    if (n + 1 >= cap) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b + n, cap - n, fmt, ap);
    va_end(ap);
    __android_log_print(ANDROID_LOG_INFO, TAG, "%s", b + n);
}

static const char *err(void) {
    const char *e = dlerror();
    return e ? e : "(no dlerror)";
}

static void step_dlopen(char *b, size_t cap, const char *what, const char *path) {
    errno = 0;
    dlerror();
    void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (h) {
        add(b, cap, "%-10s OK      %s\n", what, path);
    } else {
        add(b, cap, "%-10s FAIL    errno=%d (%s)\n           %s\n", what, errno,
            strerror(errno), err());
    }
}

static int memfd_write(const char *name, const void *data, size_t len) {
    int fd = (int) syscall(__NR_memfd_create, name, MFD_CLOEXEC);
    if (fd < 0) return -1;
    const char *p = (const char *) data;
    size_t off = 0;
    while (off < len) {
        ssize_t w = write(fd, p + off, len - off);
        if (w <= 0) {
            close(fd);
            return -1;
        }
        off += (size_t) w;
    }
    lseek(fd, 0, SEEK_SET);
    return fd;
}

// maps is what the packer can read about us, so print what it would see.
static void step_maps(char *b, size_t cap) {
    FILE *f = fopen("/proc/self/maps", "r");
    if (!f) {
        add(b, cap, "%-10s FAIL    /proc/self/maps: %s\n", "maps", strerror(errno));
        return;
    }
    char line[512];
    int total = 0, ours = 0, exec = 0;
    while (fgets(line, sizeof line, f)) {
        total++;
        if (strstr(line, "libprobe")) ours++;
        if (strstr(line, "/data/app/") && strstr(line, "libmain")) exec++;
    }
    fclose(f);
    add(b, cap, "%-10s %d lines, libprobe %d, foreign libmain %d\n", "maps", total, ours, exec);
}

JNIEXPORT jstring JNICALL
Java_oxhk_sandbox_MainActivity_nativeProbe(JNIEnv *env, jclass cls, jstring jGameLib,
                                           jstring jCopy, jbyteArray jBytes) {
    char out[12288];
    out[0] = 0;
    const char *gameLib = (*env)->GetStringUTFChars(env, jGameLib, NULL);
    const char *copy = (*env)->GetStringUTFChars(env, jCopy, NULL);

    // what the filesystem lets us see, before any loader call
    struct stat st;
    if (stat(gameLib, &st) == 0) {
        add(out, sizeof out, "%-10s mode %04o size %lld\n", "game lib", st.st_mode & 07777,
            (long long) st.st_size);
    } else {
        add(out, sizeof out, "%-10s stat failed: %s\n", "game lib", strerror(errno));
    }
    errno = 0;
    add(out, sizeof out, "%-10s access(X_OK) %s (errno=%d %s)\n", "game lib",
        access(gameLib, X_OK) == 0 ? "allowed" : "denied", errno, strerror(errno));

    step_dlopen(out, sizeof out, "control", "libprobe.so");
    step_dlopen(out, sizeof out, "game dir", gameLib);
    step_dlopen(out, sizeof out, "app copy", copy);

    // memfd: a lib that never touches the filesystem
    jsize len = (*env)->GetArrayLength(env, jBytes);
    char *bytes = (char *) (*env)->GetByteArrayElements(env, jBytes, NULL);
    int fd = memfd_write("libmain.so", bytes, (size_t) len);
    if (fd < 0) {
        add(out, sizeof out, "%-10s FAIL    memfd_create: %s\n", "memfd", strerror(errno));
    } else {
        android_dlextinfo info;
        memset(&info, 0, sizeof info);
        info.flags = ANDROID_DLEXT_USE_LIBRARY_FD;
        info.library_fd = fd;
        errno = 0;
        dlerror();
        void *h = android_dlopen_ext("libmain.so", RTLD_NOW | RTLD_LOCAL, &info);
        if (h) {
            add(out, sizeof out, "%-10s OK      %d bytes from memfd\n", "memfd", (int) len);
        } else {
            add(out, sizeof out, "%-10s FAIL    errno=%d (%s)\n           %s\n", "memfd", errno,
                strerror(errno), err());
        }
        close(fd);
    }
    (*env)->ReleaseByteArrayElements(env, jBytes, bytes, JNI_ABORT);

    step_maps(out, sizeof out);

    (*env)->ReleaseStringUTFChars(env, jGameLib, gameLib);
    (*env)->ReleaseStringUTFChars(env, jCopy, copy);
    return (*env)->NewStringUTF(env, out);
}
