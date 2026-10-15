/*
 * rootfs.c — 找到 realIOS 的 ext4 分区并挂到 /newroot。
 *
 * ext4 是用户的世界：realinit 只碰根目录下的 /trans（trans-cli 的 sh、证书、配置、
 * 菜单清单都在里面），并且只在“缺了就补”的前提下放回，已经存在的东西一律不碰。
 * /trans 以外一个目录都不建。trans-cli 本体永远有一份在引导程序内置的 initramfs 里。
 */
#define _GNU_SOURCE
#include "realinit.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>

static int parse_uuid(const char *s, uint8_t out[16])
{
    int n = 0;
    for (const char *p = s; *p && n < 32; p++) {
        if (*p == '-')
            continue;
        int v;
        if (*p >= '0' && *p <= '9') v = *p - '0';
        else if (*p >= 'a' && *p <= 'f') v = *p - 'a' + 10;
        else if (*p >= 'A' && *p <= 'F') v = *p - 'A' + 10;
        else return -1;
        if (n & 1) out[n / 2] |= (uint8_t)v;
        else out[n / 2] = (uint8_t)(v << 4);
        n++;
    }
    return n == 32 ? 0 : -1;
}

/* 0 = 不是 ext4，1 = ext4 但不是我们的，2 = 标签匹配，3 = UUID 匹配 */
static int probe(const char *dev, const uint8_t want[16])
{
    int fd = open(dev, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return 0;
    uint8_t sb[1024];
    ssize_t r = pread(fd, sb, sizeof(sb), 1024);
    close(fd);
    if (r != (ssize_t)sizeof(sb))
        return 0;
    if (sb[56] != 0x53 || sb[57] != 0xEF)
        return 0;
    if (memcmp(sb + 104, want, 16) == 0)
        return 3;
    char label[17];
    memcpy(label, sb + 120, 16);
    label[16] = 0;
    if (strcmp(label, REALIOS_EXT4_LABEL) == 0)
        return 2;
    return 1;
}

static int scan(char *best, size_t cap, const uint8_t want[16])
{
    DIR *d = opendir("/sys/class/block");
    if (!d)
        return 0;
    int best_score = 0;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.' || strncmp(de->d_name, "loop", 4) == 0 ||
            strncmp(de->d_name, "ram", 3) == 0)
            continue;
        char dev[300];
        snprintf(dev, sizeof(dev), "/dev/%s", de->d_name);
        int s = probe(dev, want);
        if (s > best_score && s >= 2) {
            best_score = s;
            snprintf(best, cap, "%s", dev);
        }
    }
    closedir(d);
    return best_score;
}

int rootfs_mount(int wait_seconds, char *devname, size_t cap)
{
    uint8_t want[16] = {0};
    const char *u = cmdline_get("realios.root");
    parse_uuid(u && u[0] ? u : REALIOS_EXT4_UUID, want);
    mkdir(NEWROOT, 0755);

    uint64_t end = now_ms() + (uint64_t)wait_seconds * 1000u;
    char dev[300] = "";
    int score = 0;
    do {
        score = scan(dev, sizeof(dev), want);
        if (score)
            break;
        usleep(200000);           /* USB 盘枚举得慢 */
    } while (now_ms() < end);
    if (!score)
        return -1;
    snprintf(devname, cap, "%s", dev);
    if (mount(dev, NEWROOT, "ext4", MS_NOATIME, NULL) < 0) {
        klog("rootfs: mount %s failed: %s", dev, strerror(errno));
        return -2;
    }
    klog("rootfs: %s mounted at " NEWROOT " (%s match)", dev, score == 3 ? "uuid" : "label");
    return 0;
}

static void ensure_dir(const char *root, const char *rel)
{
    char p[512];
    snprintf(p, sizeof(p), "%s%s", root, rel);
    mkdir(p, 0755);
}

static void ensure_copy(const char *root, const char *rel, const char *src, mode_t mode)
{
    char p[512];
    snprintf(p, sizeof(p), "%s%s", root, rel);
    if (path_exists(p))
        return;
    if (copy_file(src, p, mode) == 0)
        klog("rootfs: restored missing %s", rel);
}

static int is_dir(const char *root, const char *rel, char *out, size_t cap)
{
    struct stat st;
    snprintf(out, cap, "%s%s", root, rel);
    return lstat(out, &st) == 0 && S_ISDIR(st.st_mode);
}

/* /proc /sys /dev 只在用户（或 AI）自己建了目录时才挂上去 */
static void bind_api_fs(const char *root)
{
    char p[512];
    if (is_dir(root, "/proc", p, sizeof(p)))
        mount("proc", p, "proc", MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL);
    if (is_dir(root, "/sys", p, sizeof(p)))
        mount("sysfs", p, "sysfs", MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL);
    if (is_dir(root, "/dev", p, sizeof(p)))
        mount("/dev", p, NULL, MS_BIND | MS_REC, NULL);
}

int rootfs_prepare(const char *root)
{
    ensure_dir(root, TRANS_DIR);
    ensure_dir(root, TRANS_BINDIR);
    ensure_dir(root, TRANS_DIR "/certs");
    ensure_dir(root, TRANS_KNOWLEDGE);

    /* trans-cli 的 run_shell 按 PATH 找 sh：缺了就用内置 busybox 补一个 */
    char sh[512];
    snprintf(sh, sizeof(sh), "%s" TRANS_SH, root);
    if (!path_exists(sh)) {
        ensure_copy(root, TRANS_BUSYBOX, BUILTIN_BUSYBOX, 0755);
        if (symlink("busybox", sh) == 0)
            klog("rootfs: restored missing " TRANS_SH " -> busybox");
    }
    ensure_copy(root, TRANS_CERTS, BUILTIN_CERTS, 0644);
    ensure_copy(root, TRANS_GUIDE, BUILTIN_GUIDE, 0644);
    ensure_copy(root, TRANS_AGENTS, BUILTIN_AGENTS, 0644);
    ensure_copy(root, TRANS_KNOWLEDGE "/README.md", BUILTIN_KNOWLEDGE_README, 0644);
    ensure_copy(root, TRANS_KNOWLEDGE "/index.md", BUILTIN_KNOWLEDGE_INDEX, 0644);
    ensure_copy(root, TRANS_INIT, BUILTIN_INIT_TEMPLATE, 0644);

    /* 记事本/回车会把 \\r 写进 Key，Go 的 HTTP 头不允许控制字符 */
    char cfg[512], js[4096];
    snprintf(cfg, sizeof(cfg), "%s" TRANS_CONFIG "/trans-cli/config.json", root);
    int n = read_file(cfg, js, sizeof(js));
    if (n > 0) {
        int w = 0, dirty = 0;
        for (int i = 0; i < n; i++) {
            if (js[i] == '\r') { dirty = 1; continue; }
            js[w++] = js[i];
        }
        if (dirty)
            write_file(cfg, js, (size_t)w, 0644);
    }

    bind_api_fs(root);

    mkdir("/lib", 0755);
    mkdir("/lib/firmware", 0755);
    char fw[512];
    snprintf(fw, sizeof(fw), "%s/trans/firmware", root);
    if (is_dir(root, "/trans/firmware", fw, sizeof(fw)))
        mount(fw, "/lib/firmware", NULL, MS_BIND, NULL);
    return 0;
}

/* ext4 不可用时：tmpfs 当根，至少让 trans-cli 起来帮用户修盘（改动不落盘）。 */
int rootfs_emergency(void)
{
    const char *root = "/emergency";
    mkdir(root, 0755);
    if (mount("tmpfs", root, "tmpfs", 0, "mode=0755,size=50%") < 0)
        return -1;
    rootfs_prepare(root);
    copy_file(BUILTIN_TRANS, "/emergency" TRANS_EXE, 0755);
    return 0;
}
