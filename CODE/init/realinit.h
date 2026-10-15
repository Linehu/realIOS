/*
 * realinit — realIOS 的 PID 1。
 *
 * 它是这台机器上唯一不属于用户的用户态程序：挂 ext4、拉起网络、
 * 把屏幕变成能显示中文的终端、读 /trans/init 决定跑什么，
 * 并保证无论用户把 ext4 改成什么样，trans-cli 永远叫得出来。
 */
#ifndef REALINIT_H
#define REALINIT_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define REALIOS_VERSION   "0.1.0"
#define REALIOS_EXT4_UUID "7265616c-494f-5300-8000-000000000002"
#define REALIOS_EXT4_LABEL "REALIOS"

#define NEWROOT       "/newroot"
#define BUILTIN_TRANS "/realios/trans"
#define BUILTIN_FONT  "/realios/font.rfnt"
#define BUILTIN_GUIDE "/realios/AI_GUIDE.md"
#define BUILTIN_AGENTS "/realios/AGENTS.md"
#define BUILTIN_KNOWLEDGE_README "/realios/knowledge.README.md"
#define BUILTIN_KNOWLEDGE_INDEX "/realios/knowledge.index.md"
#define BUILTIN_INIT_TEMPLATE "/realios/init.template"
#define BUILTIN_BUSYBOX "/bin/busybox"
#define BUILTIN_CERTS "/etc/ssl/certs/ca-certificates.crt"
#define BUILTIN_WPA   "/realios/wpa_supplicant"

/* ext4 根目录下 realinit 只认 /trans 这一个目录，其余全部归用户 */
#define TRANS_DIR      "/trans"
#define TRANS_EXE      TRANS_DIR "/trans"
#define TRANS_BINDIR   TRANS_DIR "/bin"
#define TRANS_SH       TRANS_BINDIR "/sh"
#define TRANS_BUSYBOX  TRANS_BINDIR "/busybox"
#define TRANS_CERTS    TRANS_DIR "/certs/ca-certificates.crt"
#define TRANS_CONFIG   TRANS_DIR "/config"          /* XDG_CONFIG_HOME */
#define TRANS_INIT     TRANS_DIR "/init"
#define TRANS_NETCONF  TRANS_DIR "/net.conf"
#define TRANS_WELCOMED TRANS_DIR "/.welcomed"
#define TRANS_GUIDE      TRANS_DIR "/AI_GUIDE.md"
#define TRANS_AGENTS     TRANS_DIR "/AGENTS.md"
#define TRANS_KNOWLEDGE  TRANS_DIR "/knowledge"

/* ---- util.c ---- */
void  klog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int   read_file(const char *path, char *buf, size_t cap);
int   write_file(const char *path, const char *data, size_t len, mode_t mode);
int   copy_file(const char *src, const char *dst, mode_t mode);
int   path_exists(const char *path);
int   mkdir_p(const char *path, mode_t mode);
const char *cmdline_get(const char *key);   /* realios.xxx=value，无则 NULL */
int   cmdline_flag(const char *key);        /* realios.xxx 或 realios.xxx=1 */
uint64_t now_ms(void);
void  str_trim(char *s);

/* ---- term.c ---- */
enum {
    KEY_NONE = -1,
    KEY_ENTER = 1000, KEY_ESC, KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT,
    KEY_BACKSPACE, KEY_TAB, KEY_F12, KEY_HOME, KEY_END, KEY_DEL,
    KEY_PGUP, KEY_PGDN,
};

#define RUN_HOTKEY (-1000)   /* term_run_pty: 用户按 F12 强制返回 */

int   term_init(void);
void  term_write(const char *s, size_t n);
void  term_puts(const char *s);
void  term_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void  term_clear(void);
void  term_flush(void);
int   term_cols(void);
int   term_rows(void);
int   term_has_fb(void);
int   term_read_key(int timeout_ms);
int   term_readline(char *buf, size_t cap, int secret);
void  term_suspend(void);
void  term_resume(void);
int   term_run_pty(const char *root, const char *cwd, int exe_fd,
                   char *const argv[], char *const envp[]);
int   term_run_direct(const char *root, const char *cwd, int exe_fd,
                      char *const argv[], char *const envp[]);
void  term_set_idle_hook(void (*hook)(void));

/* ---- net.c ---- */
struct net_status {
    int  up;                 /* 已有可用 IPv4 + 默认路由 */
    char ifname[32];
    char addr[32];
    char gateway[32];
    char dns[64];
    char how[16];            /* dhcp / static / wifi */
    char detail[160];        /* 失败原因或网卡列表 */
};
#define WIFI_SCAN_MAX 48
struct wifi_ap {
    char ssid[64];
    int  signal;             /* dBm，越大越好（常为负数） */
    int  locked;             /* 1 = 需要密码 */
};

int   net_bringup(const char *conf_path, int verbose);
void  net_tick(void);        /* 主循环里调用：DHCP 续租 */
const struct net_status *net_get(void);
int   net_write_resolv(const char *path);
int   net_list_ifaces(char names[][32], int max);
int   net_wifi_scan(struct wifi_ap *out, int max, char *err, size_t errcap);

/* ---- dns.c ---- */
int   dns_start(void);       /* 127.0.0.1:53 → /etc/resolv.conf（initramfs 里那份）的上游 */

/* ---- rootfs.c ---- */
enum root_state { ROOT_NONE = 0, ROOT_EXT4, ROOT_EMERGENCY };
int   rootfs_mount(int wait_seconds, char *devname, size_t cap);
int   rootfs_prepare(const char *root);
int   rootfs_emergency(void);

/* ---- initconf.c ---- */
#define INITCONF_MAX 32
struct init_entry {
    char name[96];
    char cmd[512];
    int  direct;             /* 直接占用控制台，不经 realinit 终端 */
    int  is_file;            /* cmd 是一个可执行文件路径（目录模式 / 单文件模式）*/
};
struct init_conf {
    int  count;
    int  timeout;            /* 菜单倒计时秒数，0 = 一直等 */
    int  def;                /* 倒计时默认项：0 = trans，1..count = 用户程序 */
    char source[64];         /* 描述：missing / empty / file / dir / list */
    struct init_entry e[INITCONF_MAX];
};
int   initconf_load(const char *root, struct init_conf *c);

/* ---- ui.c ---- */
extern char g_root[];        /* 当前用户根：/newroot 或 /emergency */
enum { ACT_REBOOT = -2, ACT_POWEROFF = -3, ACT_NET = -4, ACT_SHELL = -5 };
void  ui_banner(void);
void  ui_status_line(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int   ui_first_boot(const char *root);
int   ui_menu(const struct init_conf *c, int safe_mode, const char *note);
void  ui_message(const char *title, const char *body);
int   ui_net_screen(void);
void  ui_pause(const char *hint);

#endif
