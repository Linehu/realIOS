/*
 * main.c — realinit 主流程（PID 1，住在引导程序内置的 initramfs 里）。
 *
 *   挂 /proc /sys /dev → 帧缓冲终端 → 找 ext4 → 补齐 /trans 里的运行依赖
 *   → 联网 → 首次引导 → 读 /trans/init：
 *        空        → 直接运行 trans-cli
 *        有程序    → 菜单：trans-cli（永远第一项）+ 用户程序
 *   程序退出 / F12 → 回菜单。PID 1 永不退出。
 */
#define _GNU_SOURCE
#include "realinit.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

char g_root[64] = NEWROOT;
static int g_root_state = ROOT_NONE;
static int g_safe;
static char g_rootdev[300];

static void mount_api(void)
{
    mount("proc", "/proc", "proc", MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL);
    mount("sysfs", "/sys", "sysfs", MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL);
    mount("devtmpfs", "/dev", "devtmpfs", MS_NOSUID, "mode=0755");
    mkdir("/dev/pts", 0755);
    mkdir("/dev/shm", 0755);
    mount("devpts", "/dev/pts", "devpts", MS_NOSUID | MS_NOEXEC, "newinstance,ptmxmode=0666,mode=0620");
    unlink("/dev/ptmx");
    (void)!symlink("pts/ptmx", "/dev/ptmx");
    mount("tmpfs", "/dev/shm", "tmpfs", MS_NOSUID | MS_NODEV, "mode=1777");
    mount("tmpfs", "/tmp", "tmpfs", MS_NOSUID | MS_NODEV, "mode=1777");
}

static void reopen_stdio(void)
{
    int fd = open("/dev/console", O_RDWR | O_NOCTTY);
    if (fd < 0)
        fd = open("/dev/null", O_RDWR);
    if (fd >= 0) {
        dup2(fd, 0);
        dup2(fd, 1);
        dup2(fd, 2);
        if (fd > 2)
            close(fd);
    }
}

static volatile sig_atomic_t g_cad;
static void on_sigint(int s) { (void)s; g_cad = 1; }

static void signals_pid1(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = SIG_IGN;
    sigaction(SIGPIPE, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGTSTP, &sa, NULL);
    sigaction(SIGTTOU, &sa, NULL);
    sigaction(SIGTTIN, &sa, NULL);
    sa.sa_handler = on_sigint;
    sigaction(SIGINT, &sa, NULL);          /* Ctrl+Alt+Del → 体面重启 */
    reboot(RB_DISABLE_CAD);
}

static void power(int how)
{
    term_puts("\n  正在同步磁盘…\n");
    sync();
    kill(-1, SIGTERM);
    sleep(1);
    kill(-1, SIGKILL);
    sync();
    umount2(NEWROOT "/dev", MNT_DETACH);
    umount2(NEWROOT "/proc", MNT_DETACH);
    umount2(NEWROOT "/sys", MNT_DETACH);
    if (umount(NEWROOT) < 0)
        mount(NULL, NEWROOT, NULL, MS_REMOUNT | MS_RDONLY, NULL);
    sync();
    reboot(how);
    for (;;)
        pause();
}

/* 只写 initramfs 里的那份：ext4 上的 /etc 是用户的，程序走 dns.c 的 127.0.0.1:53 */
static void write_resolv(void)
{
    net_write_resolv("/etc/resolv.conf");
}

static void idle_hook(void)
{
    char before[32];
    snprintf(before, sizeof(before), "%s", net_get()->addr);
    net_tick();
    if (strcmp(before, net_get()->addr) != 0)
        write_resolv();
    if (g_cad)
        power(RB_AUTOBOOT);
}

/* ------------------------------------------------------------------ */

static void mount_root(void)
{
    for (;;) {
        ui_banner();
        ui_status_line("寻找 realIOS 的 ext4 分区…");
        int r = rootfs_mount(15, g_rootdev, sizeof(g_rootdev));
        if (r == 0) {
            rootfs_prepare(NEWROOT);
            snprintf(g_root, sizeof(g_root), "%s", NEWROOT);
            g_root_state = ROOT_EXT4;
            ui_status_line("ext4：%s 已挂载", g_rootdev);
            term_puts("\n");
            return;
        }
        term_printf("\n\n  \x1b[38;2;224;108;117m没找到可用的 ext4 分区\x1b[0m（%s）。\n\n",
                    r == -2 ? "找到了，但挂载失败 —— 文件系统可能损坏" :
                              "没有 UUID " REALIOS_EXT4_UUID " / 标签 REALIOS 的分区");
        term_puts("  [R] 重试    [E] 应急模式（内存里的临时根，trans-cli 照样能用，可以让它帮你修盘）\n"
                  "  [B] 重启\n");
        for (;;) {
            int k = term_read_key(-1);
            if (k == 'r' || k == 'R')
                break;
            if (k == 'b' || k == 'B')
                power(RB_AUTOBOOT);
            if (k == 'e' || k == 'E') {
                if (rootfs_emergency() == 0) {
                    snprintf(g_root, sizeof(g_root), "/emergency");
                    g_root_state = ROOT_EMERGENCY;
                    return;
                }
                term_puts("  应急根也挂不上（内存不足？）\n");
            }
        }
    }
}

static void build_env(char **envp, char store[][160], int *n)
{
    static const char *fixed[] = {
        "HOME=" TRANS_DIR, "USER=root", "LOGNAME=root", "SHELL=" TRANS_SH,
        "PATH=/bin:/sbin:/usr/bin:/usr/sbin:/usr/local/bin:" TRANS_BINDIR,
        "SSL_CERT_FILE=" TRANS_CERTS, "XDG_CONFIG_HOME=" TRANS_CONFIG,
        "TERM=xterm-256color", "COLORTERM=truecolor", "LANG=C.UTF-8", "LC_ALL=C.UTF-8",
        "REALIOS=1", "REALIOS_VERSION=" REALIOS_VERSION,
    };
    int i = 0;
    for (; i < (int)(sizeof(fixed) / sizeof(fixed[0])); i++)
        snprintf(store[i], 160, "%s", fixed[i]);
    snprintf(store[i++], 160, "REALIOS_ROOT=%s",
             g_root_state == ROOT_EMERGENCY ? "emergency" : "ext4");
    snprintf(store[i++], 160, "REALIOS_SAFE=%d", g_safe);
    /* trans-cli：工具开、文件围栏根 = /（整台机器都归用户和 AI）*/
    snprintf(store[i++], 160, "TRANS_WORKSPACE=/");
    snprintf(store[i++], 160, "TRANS_TOOLS=on");
    /* ESP 上的 \realIOS\apikey.txt 由 realboot 转交；trans 自己存过 Key 之后就不再注入，
     * 免得盖住用户用 /key 换的新 Key */
    const char *key = cmdline_get("realios.apikey");
    char cfg[128];
    snprintf(cfg, sizeof(cfg), "%s" TRANS_CONFIG "/trans-cli/config.json", g_root);
    if (key && key[0] && !path_exists(cfg)) {
        char clean[160];
        size_t n = 0;
        for (const char *p = key; *p && n + 1 < sizeof(clean); p++) {
            unsigned char c = (unsigned char)*p;
            if (c < 0x20 || c == 0x7f)
                continue;
            clean[n++] = (char)c;
        }
        clean[n] = 0;
        if (n)
            snprintf(store[i++], 160, "TRANS_API_KEY=%s", clean);
    }
    for (int j = 0; j < i; j++)
        envp[j] = store[j];
    envp[i] = NULL;
    *n = i;
}

static int run_trans(void)
{
    char *envp[32], store[32][160];
    int ne;
    build_env(envp, store, &ne);

    char user_trans[128];
    snprintf(user_trans, sizeof(user_trans), "%s" TRANS_EXE, g_root);
    int exe_fd = -1;
    struct stat st;
    int use_user = !g_safe && stat(user_trans, &st) == 0 && S_ISREG(st.st_mode) && (st.st_mode & 0111);
    if (use_user)
        exe_fd = open(user_trans, O_RDONLY | O_CLOEXEC);
    else
        exe_fd = open(BUILTIN_TRANS, O_RDONLY | O_CLOEXEC);
    if (exe_fd < 0) {
        ui_message("找不到 trans-cli",
                   use_user ? "  打不开 /trans/trans。\n"
                            : "  引导程序里内置的 trans-cli 也不见了 —— 这个镜像构建得不完整。\n");
        return 255;
    }
    char *argv[] = {TRANS_EXE, NULL};
    klog("run trans-cli (%s)", use_user ? "ext4 " TRANS_EXE : "builtin");
    term_clear();
    int rc = term_run_pty(g_root, "/", exe_fd, argv, envp);
    if (exe_fd >= 0)
        close(exe_fd);
    return rc;
}

static int run_shell(void)
{
    char *envp[32], store[32][160];
    int ne;
    build_env(envp, store, &ne);
    char sh[128];
    snprintf(sh, sizeof(sh), "%s" TRANS_SH, g_root);
    int exe_fd = -1;
    if (!path_exists(sh))
        exe_fd = open(BUILTIN_BUSYBOX, O_RDONLY | O_CLOEXEC);
    char *argv[] = {exe_fd >= 0 ? "sh" : TRANS_SH, NULL};
    term_clear();
    term_puts("\x1b[90m[realinit] " TRANS_SH " —— 输入 exit 或按 F12 返回菜单\x1b[0m\n");
    int rc = term_run_pty(g_root, "/", exe_fd, argv, envp);
    if (exe_fd >= 0)
        close(exe_fd);
    return rc;
}

static int run_entry(const struct init_entry *e)
{
    char *envp[32], store[32][160];
    int ne;
    build_env(envp, store, &ne);
    char cmdbuf[512];
    snprintf(cmdbuf, sizeof(cmdbuf), "%s", e->cmd);
    char *argv[64];
    int ac = 0;
    char sh[128];
    snprintf(sh, sizeof(sh), "%s" TRANS_SH, g_root);
    if (e->is_file) {
        argv[ac++] = cmdbuf;
    } else if (path_exists(sh)) {
        argv[ac++] = TRANS_SH;
        argv[ac++] = "-c";
        argv[ac++] = cmdbuf;
    } else {
        for (char *t = strtok(cmdbuf, " \t"); t && ac < 63; t = strtok(NULL, " \t"))
            argv[ac++] = t;
    }
    argv[ac] = NULL;
    if (ac == 0)
        return 255;
    klog("run entry '%s': %s%s", e->name, e->cmd, e->direct ? " (direct)" : "");
    term_clear();
    if (e->direct)
        return term_run_direct(g_root, "/", -1, argv, envp);
    return term_run_pty(g_root, "/", -1, argv, envp);
}

static void after_run(const char *what, int rc, char *note, size_t cap)
{
    if (rc == RUN_HOTKEY) {
        snprintf(note, cap, "\x1b[90m%s 已被 F12 结束。\x1b[0m", what);
        return;
    }
    snprintf(note, cap, "\x1b[90m%s 已退出（状态 %d）。\x1b[0m", what, rc);
    term_printf("\n\x1b[0m\x1b[90m[realinit] %s 已退出（状态 %d）。按 Enter 返回菜单\x1b[0m", what, rc);
    for (;;) {
        int k = term_read_key(-1);
        if (k == KEY_ENTER || k == KEY_ESC || k == ' ')
            break;
    }
}

int main(void)
{
    if (getpid() != 1)
        fprintf(stderr, "realinit: 警告：不是 PID 1，仅供调试\n");
    mount_api();
    reopen_stdio();
    signals_pid1();
    (void)!sethostname("realios", 7);
    setenv("PATH", "/bin", 1);
    g_safe = cmdline_flag("realios.safe");
    klog("realIOS %s starting%s", REALIOS_VERSION, g_safe ? " (safe mode)" : "");

    term_init();
    term_set_idle_hook(idle_hook);
    mount_root();

    char netconf[128];
    snprintf(netconf, sizeof(netconf), "%s" TRANS_NETCONF, g_root);
    ui_status_line("正在联网…");
    term_puts("\n");
    net_bringup(netconf, 1);
    term_puts("\r\x1b[K");
    write_resolv();
    dns_start();

    char marker[128];
    snprintf(marker, sizeof(marker), "%s" TRANS_WELCOMED, g_root);
    char welcomed[16] = "";
    int need_intro = g_root_state == ROOT_EXT4 && !g_safe &&
                     (read_file(marker, welcomed, sizeof(welcomed)) <= 0 || atoi(welcomed) < 2);
    if (need_intro) {
        ui_first_boot(g_root);
        write_file(marker, "2\n", 2, 0644);
        write_resolv();
        sync();
    } else if (!g_safe && !net_get()->up) {
        ui_net_screen();
        write_resolv();
    }

    struct init_conf conf;
    int autorun = 1;
    char note[256] = "";
    if (g_root_state == ROOT_EMERGENCY)
        snprintf(note, sizeof(note),
                 "\x1b[38;2;229;192;123m应急模式：根目录在内存里，关机即丢失。ext4 设备可在 /dev 下找到。\x1b[0m");

    for (;;) {
        memset(&conf, 0, sizeof(conf));
        if (!g_safe && g_root_state == ROOT_EXT4)
            initconf_load(g_root, &conf);
        int choice;
        if (autorun && (g_safe || conf.count == 0))
            choice = 0;
        else
            choice = ui_menu(&conf, g_safe, note);
        autorun = 0;

        int rc;
        switch (choice) {
        case 0:
            rc = run_trans();
            after_run("trans-cli", rc, note, sizeof(note));
            break;
        case ACT_SHELL:
            rc = run_shell();
            after_run(TRANS_SH, rc, note, sizeof(note));
            break;
        case ACT_NET:
            ui_net_screen();
            write_resolv();
            break;
        case ACT_REBOOT:
            power(RB_AUTOBOOT);
            break;
        case ACT_POWEROFF:
            power(RB_POWER_OFF);
            break;
        default:
            if (choice >= 1 && choice <= conf.count) {
                rc = run_entry(&conf.e[choice - 1]);
                after_run(conf.e[choice - 1].name, rc, note, sizeof(note));
            }
            break;
        }
        sync();
    }
}
