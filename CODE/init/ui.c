/*
 * ui.c — realinit 的全部界面：开机横幅、首次引导、程序菜单、网络设置。
 * 所有界面都只是往 term 写 ANSI 文本，帧缓冲和串口看到的是同一份内容。
 */
#define _GNU_SOURCE
#include "realinit.h"

#include <arpa/inet.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define C_RST  "\x1b[0m"
#define C_DIM  "\x1b[90m"
#define C_ACC  "\x1b[38;2;120;200;255m"
#define C_OK   "\x1b[38;2;152;195;121m"
#define C_WARN "\x1b[38;2;229;192;123m"
#define C_BAD  "\x1b[38;2;224;108;117m"
#define C_HI   "\x1b[1m\x1b[97m"
#define C_SEL  "\x1b[48;2;40;70;110m\x1b[97m"

static const char *k_logo[] = {
    "                  _ ___ ___  ____  ",
    "  _ __ ___  __ _ | |_ _/ _ \\/ ___| ",
    " | '__/ _ \\/ _` || || | | | \\___ \\ ",
    " | | |  __/ (_| || || | |_| |___) |",
    " |_|  \\___|\\__,_||_|___\\___/|____/ ",
};

static void center(const char *text, int vis_width)
{
    int pad = (term_cols() - vis_width) / 2;
    if (pad < 0)
        pad = 0;
    term_printf("%*s%s\n", pad, "", text);
}

void ui_banner(void)
{
    term_clear();
    term_puts("\n");
    for (size_t i = 0; i < sizeof(k_logo) / sizeof(k_logo[0]); i++) {
        char line[160];
        snprintf(line, sizeof(line), C_ACC "%s" C_RST, k_logo[i]);
        center(line, 36);
    }
    term_puts("\n");
    center(C_DIM "一台什么都没有的电脑，和一个能帮你造出一切的 AI。" C_RST, 50);
    center(C_DIM "realIOS " REALIOS_VERSION " · Deaicup" C_RST, 22);
    term_puts("\n");
}

void ui_status_line(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    term_printf("\r\x1b[K  " C_DIM "·" C_RST " %s", buf);
}

void ui_pause(const char *hint)
{
    term_printf("\n  " C_DIM "%s" C_RST, hint ? hint : "按 Enter 继续");
    for (;;) {
        int k = term_read_key(-1);
        if (k == KEY_ENTER || k == ' ' || k == KEY_ESC)
            break;
    }
    term_puts("\n");
}

void ui_message(const char *title, const char *body)
{
    term_printf("\n  " C_HI "%s" C_RST "\n\n", title);
    term_puts(body);
    ui_pause(NULL);
}

static void step_header(int n, int total, const char *title)
{
    ui_banner();
    term_printf("  " C_DIM "首次启动引导 %d / %d" C_RST "    " C_HI "%s" C_RST "\n\n", n, total, title);
}

static void net_summary(void)
{
    const struct net_status *ns = net_get();
    if (ns->up)
        term_printf("  " C_OK "● 已联网" C_RST "  %s  %s  网关 %s  DNS %s  (%s)\n", ns->ifname, ns->addr,
                    ns->gateway, ns->dns, ns->how);
    else
        term_printf("  " C_BAD "● 未联网" C_RST "  %s\n", ns->detail[0] ? ns->detail : "尚未配置");
}

/* ------------------------------------------------------------------ */

static int save_wifi(const char *ssid, const char *psk)
{
    char conf[512], path[300];
    int len = snprintf(conf, sizeof(conf),
                       "# realIOS 网络。mode=dhcp|wifi|static|off\n"
                       "mode=wifi\nssid=%s\npsk=%s\n",
                       ssid, psk);
    snprintf(path, sizeof(path), "%s" TRANS_NETCONF, g_root);
    return write_file(path, conf, (size_t)len, 0644);
}

static int ask_wifi_password(const char *ssid, int locked)
{
    char psk[64] = "";
    term_printf("\n  已选：" C_HI "%s" C_RST "%s\n", ssid, locked ? "  （需要密码）" : "  （开放网络）");
    term_printf("  密码%s: ", locked ? "" : "（开放网络请直接 Enter）");
    if (term_readline(psk, sizeof(psk), 1) < 0)
        return -1;
    return save_wifi(ssid, psk);
}

static int ask_wifi(void)
{
    for (;;) {
        ui_banner();
        term_printf("  " C_HI "Wi-Fi" C_RST "  " C_DIM "↑↓ 选择热点，Enter 连接，R 重新扫描，M 手敲名称，Esc 返回" C_RST "\n\n");
        term_printf("  名称和密码只在这一步由你输入，AI 看不到。\n\n");
        ui_status_line("正在搜索附近的 Wi-Fi…");
        term_puts("\n");
        struct wifi_ap ap[WIFI_SCAN_MAX];
        char err[160] = "";
        int n = net_wifi_scan(ap, WIFI_SCAN_MAX, err, sizeof(err));
        term_puts("\r\x1b[K");

        if (n < 0) {
            term_printf("  " C_BAD "%s" C_RST "\n\n  [R] 重试    [M] 手敲名称    [Esc] 返回\n", err);
            int k = term_read_key(-1);
            if (k == 'r' || k == 'R')
                continue;
            if (k == 'm' || k == 'M')
                goto manual;
            return -1;
        }
        if (n == 0) {
            term_printf("  " C_WARN "%s" C_RST "\n\n  [R] 重新扫描    [M] 手敲名称    [Esc] 返回\n",
                        err[0] ? err : "附近没有搜到 Wi-Fi");
            int k = term_read_key(-1);
            if (k == 'r' || k == 'R')
                continue;
            if (k == 'm' || k == 'M')
                goto manual;
            return -1;
        }

        int sel = 0, top = 0;
        for (;;) {
            int rows = term_rows() - 12;
            if (rows < 6)
                rows = 6;
            if (sel < top)
                top = sel;
            if (sel >= top + rows)
                top = sel - rows + 1;
            ui_banner();
            term_printf("  " C_HI "Wi-Fi" C_RST "  " C_DIM "↑↓ 选择，Enter 连接，R 重新扫描，M 手敲名称，Esc 返回" C_RST "\n\n");
            term_printf("  搜到 %d 个热点。选中后输入密码（AI 不会看到这一步）。\n\n", n);
            for (int i = top; i < n && i < top + rows; i++) {
                char line[160];
                snprintf(line, sizeof(line), "%s  %s  %d dBm",
                         ap[i].ssid, ap[i].locked ? "锁" : "开", ap[i].signal);
                if (i == sel)
                    term_printf("  " C_SEL " %s " C_RST "\n", line);
                else
                    term_printf("   %s\n", line);
            }
            int k = term_read_key(-1);
            if (k == KEY_UP)
                sel = (sel + n - 1) % n;
            else if (k == KEY_DOWN || k == KEY_TAB)
                sel = (sel + 1) % n;
            else if (k == 'r' || k == 'R')
                break;
            else if (k == 'm' || k == 'M')
                goto manual;
            else if (k == KEY_ESC)
                return -1;
            else if (k == KEY_ENTER)
                return ask_wifi_password(ap[sel].ssid, ap[sel].locked);
        }
        continue;

    manual:
        {
            char ssid[64] = "";
            term_printf("\n  名称: ");
            if (term_readline(ssid, sizeof(ssid), 0) <= 0)
                return -1;
            return ask_wifi_password(ssid, 1);
        }
    }
}

static int ask_static(void)
{
    char ifn[32] = "", addr[32], mask[32], gw[32], dns[64];
    char names[16][32];
    int n = net_list_ifaces(names, 16);
    if (n > 0)
        snprintf(ifn, sizeof(ifn), "%s", names[0]);
    term_printf("\n  手动配置静态 IP（Esc 取消）\n");
    term_printf("  网卡 [%s]: ", ifn);
    char tmp[32];
    if (term_readline(tmp, sizeof(tmp), 0) < 0) return -1;
    if (tmp[0]) snprintf(ifn, sizeof(ifn), "%s", tmp);
    term_printf("  IP 地址 (例 192.168.1.50): ");
    if (term_readline(addr, sizeof(addr), 0) <= 0) return -1;
    term_printf("  子网掩码 [255.255.255.0]: ");
    if (term_readline(mask, sizeof(mask), 0) < 0) return -1;
    if (!mask[0]) snprintf(mask, sizeof(mask), "255.255.255.0");
    term_printf("  网关 (例 192.168.1.1): ");
    if (term_readline(gw, sizeof(gw), 0) <= 0) return -1;
    term_printf("  DNS [223.5.5.5,119.29.29.29]: ");
    if (term_readline(dns, sizeof(dns), 0) < 0) return -1;
    if (!dns[0]) snprintf(dns, sizeof(dns), "223.5.5.5,119.29.29.29");

    char conf[512], path[300];
    int len = snprintf(conf, sizeof(conf),
                       "# realIOS 网络配置（realinit 读取）。mode=dhcp|static|off\n"
                       "mode=static\niface=%s\naddress=%s\nnetmask=%s\ngateway=%s\ndns=%s\n",
                       ifn, addr, mask, gw, dns);
    snprintf(path, sizeof(path), "%s" TRANS_NETCONF, g_root);
    write_file(path, conf, (size_t)len, 0644);
    return 0;
}

static void reconnect(void)
{
    char path[300];
    snprintf(path, sizeof(path), "%s" TRANS_NETCONF, g_root);
    term_puts("\n");
    net_bringup(path, 1);
    term_puts("\r\x1b[K");
    net_write_resolv("/etc/resolv.conf");
}

int ui_net_screen(void)
{
    for (;;) {
        ui_banner();
        term_printf("  " C_HI "联网" C_RST "\n\n");
        term_puts("  AI 住在云端，必须先联网。这一步由你自己完成，不要让 AI 去连网。\n\n"
                  "    " C_ACC "◆" C_RST " 网线 / USB 网卡 / 手机 USB 网络共享：插上后按 [R] 自动获取地址\n"
                  "    " C_ACC "◆" C_RST " 只用 Wi-Fi：按 [W]，从搜到的名称里用 ↑↓ 选择，再输入密码\n\n");
        net_summary();
        term_printf("\n  [R] 重新获取 (DHCP)    [W] 搜索 Wi-Fi    [S] 静态 IP    [D] 恢复自动    [Enter] 继续\n");
        int k = term_read_key(-1);
        if (k == 'r' || k == 'R') {
            reconnect();
        } else if (k == 'w' || k == 'W') {
            if (ask_wifi() == 0)
                reconnect();
        } else if (k == 's' || k == 'S') {
            if (ask_static() == 0)
                reconnect();
        } else if (k == 'd' || k == 'D') {
            char path[300];
            snprintf(path, sizeof(path), "%s" TRANS_NETCONF, g_root);
            unlink(path);
            reconnect();
        } else if (k == KEY_ENTER || k == KEY_ESC) {
            return net_get()->up;
        }
    }
}

/* ------------------------------------------------------------------ */

int ui_first_boot(const char *root)
{
    (void)root;
    const int total = 4;

    step_header(1, total, "欢迎来到 realIOS");
    term_puts("  这台电脑上几乎什么都没有：\n\n"
              "    " C_ACC "◆" C_RST " 一块只留了磁盘、网卡、键盘和屏幕的核心\n"
              "    " C_ACC "◆" C_RST " 一个 ext4 分区 —— 你的整个世界，根目录下只有一个 /trans\n"
              "    " C_ACC "◆" C_RST " 一个能联网的 AI：trans-cli\n"
              "    " C_ACC "◆" C_RST " 我，realinit，负责开机、联网和兜底\n\n"
              "  没有桌面，没有浏览器，没有包管理器，没有 vim，甚至没有 ls。\n"
              "  你想要的一切 —— 文件管理、编辑器、桌面、游戏 ——\n"
              "  都由你用一句话描述，让 AI 在这台机器上亲手写出来。\n\n"
              "  " C_HI "这台电脑完全属于你。" C_RST "没有权限弹窗，没有\"此操作不被允许\"。\n");
    ui_pause("按 Enter 进入联网");
    ui_net_screen();

    step_header(3, total, "AI 的钥匙");
    term_puts("  trans-cli 需要一个 Deaicup API Key：\n\n"
              "    1. 在另一台设备（手机即可）打开 " C_ACC "https://deaicup.com/trans-1/console.php" C_RST "\n"
              "    2. 注册 / 登录（每月有免费额度），在「API」标签页生成 Key\n"
              "    3. 稍后 trans-cli 第一次启动时会请你输入它，之后自动记住\n"
              "       （保存在 /trans/config/trans-cli/config.json，可随时用 /key 更换）\n\n"
              "  " C_HI "懒得手敲？" C_RST "在 Windows 上打开这个 U 盘的 FAT32 分区，把 Key 存成\n"
              "  \\realIOS\\apikey.txt，下次开机会自动交给 trans-cli。\n\n"
              "  " C_DIM "这里没有剪贴板，也还没有中文输入法 —— 先用英文或拼音和 AI 说话，\n"
              "  然后让它给你写一个输入法。这就是 realIOS 的玩法。" C_RST "\n");
    ui_pause("按 Enter 继续");

    step_header(4, total, "几条规矩（都是为了让你放心折腾）");
    term_puts("  " C_HI "F12" C_RST "          任何时候结束当前程序，回到 realinit 菜单\n"
              "  " C_HI "/trans/init" C_RST "  写上你的程序，开机就会出现在菜单里（写法见文件内注释）；\n"
              "               空着就开机直接进 AI\n"
              "  " C_HI "开机按 S" C_RST "     安全模式：无视 /trans/init，直接进内置的 trans-cli\n"
              "  " C_HI "trans-cli" C_RST "    永远在菜单第一项。引导程序里自带一份，ext4 搞坏了也叫得出来\n"
              "  " C_HI "/trans" C_RST "       trans-cli 的家。除它以外，根目录下的一切都由你决定\n\n"
              "  不知道从哪开始？对 AI 说：\n\n"
              "    " C_ACC "\"Read /trans/AGENTS.md and /trans/knowledge, then write me a file manager in C.\"" C_RST "\n"
              "    " C_ACC "\"bang wo xie yi ge zhong wen shu ru fa\"" C_RST "\n");
    ui_pause("按 Enter 启动 AI");
    return 0;
}

/* ------------------------------------------------------------------ */

int ui_menu(const struct init_conf *c, int safe_mode, const char *note)
{
    int total = 1 + (safe_mode ? 0 : c->count);
    struct { const char *label; int act; } extra[] = {
        {"网络设置", ACT_NET}, {"Shell（" TRANS_SH "）", ACT_SHELL},
        {"重启", ACT_REBOOT}, {"关机", ACT_POWEROFF},
    };
    int nextra = 4;
    int items = total + nextra;
    int sel = (!safe_mode && c->def >= 0 && c->def < total) ? c->def : 0;
    int countdown = (!safe_mode && c->timeout > 0) ? c->timeout : 0;
    uint64_t deadline = countdown ? now_ms() + (uint64_t)countdown * 1000u : 0;

    for (;;) {
        ui_banner();
        if (safe_mode)
            term_printf("  " C_WARN "安全模式：已忽略 /trans/init，只提供内置的 trans-cli。" C_RST "\n\n");
        if (note && note[0])
            term_printf("  %s\n\n", note);
        term_printf("  " C_HI "选择要运行的程序" C_RST "   " C_DIM "↑↓ / 数字键选择，Enter 运行" C_RST "\n\n");
        for (int i = 0; i < items; i++) {
            char label[200];
            if (i == 0)
                snprintf(label, sizeof(label), "trans-cli  " C_DIM "AI · 永远在这里" C_RST);
            else if (i < total)
                snprintf(label, sizeof(label), "%s%s", c->e[i - 1].name,
                         c->e[i - 1].direct ? "  " C_DIM "(direct)" C_RST : "");
            else
                snprintf(label, sizeof(label), C_DIM "%s" C_RST, extra[i - total].label);
            if (i == total)
                term_puts("\n");
            const char *key = i < 9 ? (char[]){(char)('1' + i), 0} : " ";
            if (i == sel)
                term_printf("  " C_SEL " %s  %s " C_RST "\n", key, label);
            else
                term_printf("   %s  %s\n", key, label);
        }
        net_summary();
        if (deadline) {
            int left = (int)((deadline - now_ms() + 999) / 1000);
            if (left < 0) left = 0;
            term_printf("\n  " C_WARN "%d 秒后自动运行第 %d 项，按任意键取消" C_RST "\n", left, sel + 1);
        }

        int k = term_read_key(deadline ? 250 : -1);
        if (k == KEY_NONE) {
            if (deadline && now_ms() >= deadline)
                return sel;
            continue;
        }
        deadline = 0;
        if (k == KEY_UP)
            sel = (sel + items - 1) % items;
        else if (k == KEY_DOWN || k == KEY_TAB)
            sel = (sel + 1) % items;
        else if (k >= '1' && k <= '9' && k - '1' < items)
            sel = k - '1';
        else if (k == KEY_ENTER) {
            if (sel < total)
                return sel;
            return extra[sel - total].act;
        }
    }
}
