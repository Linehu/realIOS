#define _GNU_SOURCE
#include "realinit.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

void klog(const char *fmt, ...)
{
    char buf[512];
    int n = snprintf(buf, sizeof(buf), "<6>realinit: ");
    va_list ap;
    va_start(ap, fmt);
    n += vsnprintf(buf + n, sizeof(buf) - (size_t)n - 2, fmt, ap);
    va_end(ap);
    if (n > (int)sizeof(buf) - 2)
        n = (int)sizeof(buf) - 2;
    buf[n++] = '\n';
    int fd = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);
    if (fd >= 0) {
        (void)!write(fd, buf, (size_t)n);
        close(fd);
    }
}

int read_file(const char *path, char *buf, size_t cap)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    size_t off = 0;
    while (off + 1 < cap) {
        ssize_t r = read(fd, buf + off, cap - 1 - off);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0)
            break;
        off += (size_t)r;
    }
    buf[off] = 0;
    close(fd);
    return (int)off;
}

int write_file(const char *path, const char *data, size_t len, mode_t mode)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
    if (fd < 0)
        return -1;
    size_t off = 0;
    while (off < len) {
        ssize_t w = write(fd, data + off, len - off);
        if (w < 0 && errno == EINTR)
            continue;
        if (w <= 0) {
            close(fd);
            return -1;
        }
        off += (size_t)w;
    }
    fsync(fd);
    close(fd);
    return 0;
}

int copy_file(const char *src, const char *dst, mode_t mode)
{
    int in = open(src, O_RDONLY | O_CLOEXEC);
    if (in < 0)
        return -1;
    char tmp[4096];
    snprintf(tmp, sizeof(tmp), "%s.realinit-tmp", dst);
    int out = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
    if (out < 0) {
        close(in);
        return -1;
    }
    char buf[65536];
    int ok = 0;
    for (;;) {
        ssize_t r = read(in, buf, sizeof(buf));
        if (r < 0 && errno == EINTR)
            continue;
        if (r < 0) {
            ok = -1;
            break;
        }
        if (r == 0)
            break;
        ssize_t off = 0;
        while (off < r) {
            ssize_t w = write(out, buf + off, (size_t)(r - off));
            if (w <= 0) {
                ok = -1;
                break;
            }
            off += w;
        }
        if (ok)
            break;
    }
    fsync(out);
    close(out);
    close(in);
    if (ok == 0 && rename(tmp, dst) == 0) {
        chmod(dst, mode);
        return 0;
    }
    unlink(tmp);
    return -1;
}

int path_exists(const char *path)
{
    struct stat st;
    return lstat(path, &st) == 0;
}

int mkdir_p(const char *path, mode_t mode)
{
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            mkdir(tmp, mode);
            *p = '/';
        }
    }
    if (mkdir(tmp, mode) < 0 && errno != EEXIST)
        return -1;
    return 0;
}

static char g_cmdline[4096];
static int g_cmdline_loaded;

static void cmdline_load(void)
{
    if (g_cmdline_loaded)
        return;
    g_cmdline_loaded = 1;
    if (read_file("/proc/cmdline", g_cmdline, sizeof(g_cmdline)) < 0)
        g_cmdline[0] = 0;
    str_trim(g_cmdline);
}

const char *cmdline_get(const char *key)
{
    static char val[256];
    cmdline_load();
    size_t kl = strlen(key);
    const char *p = g_cmdline;
    while (*p) {
        while (*p == ' ')
            p++;
        const char *tok = p;
        while (*p && *p != ' ')
            p++;
        size_t tl = (size_t)(p - tok);
        if (tl > kl && strncmp(tok, key, kl) == 0 && tok[kl] == '=') {
            size_t vl = tl - kl - 1;
            if (vl >= sizeof(val))
                vl = sizeof(val) - 1;
            memcpy(val, tok + kl + 1, vl);
            val[vl] = 0;
            return val;
        }
        if (tl == kl && strncmp(tok, key, kl) == 0) {
            val[0] = 0;
            return val;
        }
    }
    return NULL;
}

int cmdline_flag(const char *key)
{
    const char *v = cmdline_get(key);
    if (!v)
        return 0;
    return v[0] == 0 || strcmp(v, "1") == 0 || strcmp(v, "yes") == 0 || strcmp(v, "on") == 0;
}

uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

void str_trim(char *s)
{
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r' || s[n - 1] == '\n'))
        s[--n] = 0;
    size_t i = 0;
    while (s[i] == ' ' || s[i] == '\t')
        i++;
    if (i)
        memmove(s, s + i, n - i + 1);
}
