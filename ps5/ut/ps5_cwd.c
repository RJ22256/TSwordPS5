// The working directory on the console (TSword PS5; ps5/tsword/build.sh wraps the calls below). A title may not
// chdir (EPERM) and its getcwd knows no directory, while the runtime works relative to its data folder, as on every
// PC (runtime/paths.c: chdir to the data root, then "extracted/...", "build/...", the settings file). So the working
// directory is kept here: chdir checks the folder and records it, getcwd reports it, and every path-taking call the
// runtime and its dependencies make resolves a relative path against it. It starts as the title folder, /app0.
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utime.h>

#include <ps5platform/libc.h>

static char g_cwd[1024] = "/app0";
static pthread_rwlock_t g_cwd_lock = PTHREAD_RWLOCK_INITIALIZER;

// path as given when absolute, else the working directory's: "." and "./" dropped, ".." taking a component off
static const char *abs_path(const char *path, char *buf, size_t n) {
    if (!path || path[0] == '/' || !path[0]) return path;
    pthread_rwlock_rdlock(&g_cwd_lock);
    snprintf(buf, n, "%s", g_cwd);
    pthread_rwlock_unlock(&g_cwd_lock);
    size_t len = strlen(buf);
    const char *p = path;
    while (*p) {
        const char *e = strchr(p, '/');
        size_t k = e ? (size_t)(e - p) : strlen(p);
        if (k == 0 || (k == 1 && p[0] == '.')) {
        } else if (k == 2 && p[0] == '.' && p[1] == '.') {
            char *s = strrchr(buf, '/');
            if (s && s != buf) *s = 0, len = (size_t)(s - buf);
            else buf[1] = 0, len = 1;
        } else if (len + 1 + k < n) {
            if (len > 1) buf[len++] = '/';
            memcpy(buf + len, p, k);
            len += k;
            buf[len] = 0;
        }
        p += k;
        if (*p == '/') p++;
    }
    return buf;
}
#define ABS(p) char abs_buf_[1100]; const char *ap_ = abs_path((p), abs_buf_, sizeof abs_buf_)

int __wrap_chdir(const char *path) {
    ABS(path);
    struct stat st;
    if (stat(ap_, &st)) return -1;
    if (!S_ISDIR(st.st_mode)) { errno = ENOTDIR; return -1; }
    pthread_rwlock_wrlock(&g_cwd_lock);
    snprintf(g_cwd, sizeof g_cwd, "%s", ap_);
    pthread_rwlock_unlock(&g_cwd_lock);
    return 0;
}

char *__wrap_getcwd(char *buf, size_t size) {
    pthread_rwlock_rdlock(&g_cwd_lock);
    size_t len = strlen(g_cwd);
    if (!buf) buf = malloc(size > len ? size : len + 1), size = size > len ? size : len + 1;
    char *r = NULL;
    if (!buf) errno = ENOMEM;
    else if (len + 1 > size) errno = ERANGE;
    else memcpy(buf, g_cwd, len + 1), r = buf;
    pthread_rwlock_unlock(&g_cwd_lock);
    return r;
}

FILE *__real_fopen(const char *, const char *);
FILE *__wrap_fopen(const char *path, const char *mode) { ABS(path); return __real_fopen(ap_, mode); }
FILE *__real_freopen(const char *, const char *, FILE *);
FILE *__wrap_freopen(const char *path, const char *mode, FILE *f) { ABS(path); return __real_freopen(ap_, mode, f); }
int __real_open(const char *, int, ...);
int __wrap_open(const char *path, int flags, ...) {
    ABS(path);
    int mode = 0;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, int);
        va_end(ap);
    }
    return __real_open(ap_, flags, mode);
}
int __real_stat(const char *, struct stat *);
int __wrap_stat(const char *path, struct stat *st) { ABS(path); return __real_stat(ap_, st); }
int __real_lstat(const char *, struct stat *);
int __wrap_lstat(const char *path, struct stat *st) { ABS(path); return __real_lstat(ap_, st); }
int __real_mkdir(const char *, mode_t);
int __wrap_mkdir(const char *path, mode_t m) { ABS(path); return __real_mkdir(ap_, m); }
int __real_rmdir(const char *);
int __wrap_rmdir(const char *path) { ABS(path); return __real_rmdir(ap_); }
int __real_remove(const char *);
int __wrap_remove(const char *path) { ABS(path); return __real_remove(ap_); }
int __real_unlink(const char *);
int __wrap_unlink(const char *path) { ABS(path); return __real_unlink(ap_); }
int __real_rename(const char *, const char *);
int __wrap_rename(const char *from, const char *to) {
    char b1[1100], b2[1100];
    return __real_rename(abs_path(from, b1, sizeof b1), abs_path(to, b2, sizeof b2));
}
int __real_utime(const char *, const struct utimbuf *);
int __wrap_utime(const char *path, const struct utimbuf *t) { ABS(path); return __real_utime(ap_, t); }
// (the platform layer's own: the console's are refused or missing for a title)
int __wrap_access(const char *path, int mode) { ABS(path); return ps5_access(ap_, mode); }
DIR *__wrap_opendir(const char *path) { ABS(path); return ps5_opendir(ap_); }
char *__wrap_realpath(const char *path, char *out) { ABS(path); return ps5_realpath(ap_, out); }
int __wrap_mkstemp(char *tmpl) {
    if (tmpl && tmpl[0] != '/') {  // (the template is filled in place: resolved, then the name copied back)
        char b[1100];
        const char *a = abs_path(tmpl, b, sizeof b);
        size_t n = strlen(a), t = strlen(tmpl);
        if (n >= sizeof b) { errno = ENAMETOOLONG; return -1; }
        int fd = ps5_mkstemp(b);
        if (fd >= 0) memcpy(tmpl + t - 6, b + n - 6, 6);
        return fd;
    }
    return ps5_mkstemp(tmpl);
}
