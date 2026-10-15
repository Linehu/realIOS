/*
 * net.c — 有线网络：网卡驱动全部编在内核里，这里只做用户态最小闭环：
 *   lo 起来 → 有线 DHCP / 静态 / Wi-Fi（用户自己填 SSID+密码，wpa_supplicant）
 *   → 地址 / 掩码 / 默认路由 → resolv.conf。租期过半自动续租。
 */
#define _GNU_SOURCE
#include "realinit.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <net/route.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <linux/wireless.h>

static struct net_status g_ns;
static uint64_t g_renew_at;
static char g_conf_path[256];

const struct net_status *net_get(void) { return &g_ns; }

static int if_ioctl(unsigned long req, struct ifreq *ifr)
{
    int s = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (s < 0)
        return -1;
    int r = ioctl(s, req, ifr);
    close(s);
    return r;
}

static int if_up(const char *ifname)
{
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    snprintf(ifr.ifr_name, IFNAMSIZ, "%s", ifname);
    if (if_ioctl(SIOCGIFFLAGS, &ifr) < 0)
        return -1;
    ifr.ifr_flags |= IFF_UP | IFF_RUNNING;
    return if_ioctl(SIOCSIFFLAGS, &ifr);
}

static int if_set_addr(const char *ifname, unsigned long req, uint32_t a)
{
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    snprintf(ifr.ifr_name, IFNAMSIZ, "%s", ifname);
    struct sockaddr_in *sin = (struct sockaddr_in *)&ifr.ifr_addr;
    sin->sin_family = AF_INET;
    sin->sin_addr.s_addr = a;
    return if_ioctl(req, &ifr);
}

static int route_default(const char *ifname, uint32_t gw)
{
    struct rtentry rt;
    memset(&rt, 0, sizeof(rt));
    struct sockaddr_in *d = (struct sockaddr_in *)&rt.rt_dst;
    struct sockaddr_in *g = (struct sockaddr_in *)&rt.rt_gateway;
    struct sockaddr_in *m = (struct sockaddr_in *)&rt.rt_genmask;
    d->sin_family = g->sin_family = m->sin_family = AF_INET;
    g->sin_addr.s_addr = gw;
    rt.rt_flags = RTF_UP | RTF_GATEWAY;
    rt.rt_dev = (char *)ifname;
    int s = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (s < 0)
        return -1;
    ioctl(s, SIOCDELRT, &rt);
    int r = ioctl(s, SIOCADDRT, &rt);
    close(s);
    return (r < 0 && errno != EEXIST) ? -1 : 0;
}

static int read_sys_int(const char *ifname, const char *attr)
{
    char p[128], b[32];
    snprintf(p, sizeof(p), "/sys/class/net/%s/%s", ifname, attr);
    if (read_file(p, b, sizeof(b)) <= 0)
        return -1;
    return atoi(b);
}

int net_list_ifaces(char names[][32], int max)
{
    DIR *d = opendir("/sys/class/net");
    if (!d)
        return 0;
    int n = 0;
    struct dirent *de;
    while ((de = readdir(d)) && n < max) {
        if (de->d_name[0] == '.' || strcmp(de->d_name, "lo") == 0)
            continue;
        if (read_sys_int(de->d_name, "type") != 1)      /* ARPHRD_ETHER */
            continue;
        snprintf(names[n++], 32, "%s", de->d_name);
    }
    closedir(d);
    for (int i = 0; i < n; i++)        /* 稳定顺序：eth0 先于 eth1 */
        for (int j = i + 1; j < n; j++)
            if (strcmp(names[i], names[j]) > 0) {
                char t[32];
                memcpy(t, names[i], 32);
                memcpy(names[i], names[j], 32);
                memcpy(names[j], t, 32);
            }
    return n;
}

static int get_mac(const char *ifname, uint8_t mac[6])
{
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    snprintf(ifr.ifr_name, IFNAMSIZ, "%s", ifname);
    if (if_ioctl(SIOCGIFHWADDR, &ifr) < 0)
        return -1;
    memcpy(mac, ifr.ifr_hwaddr.sa_data, 6);
    return 0;
}

/* ------------------------------------------------------------------ */
/* DHCP（RFC 2131 最小实现：DISCOVER/OFFER/REQUEST/ACK，广播标志位） */

struct dhcp_pkt {
    uint8_t op, htype, hlen, hops;
    uint32_t xid;
    uint16_t secs, flags;
    uint32_t ciaddr, yiaddr, siaddr, giaddr;
    uint8_t chaddr[16];
    uint8_t sname[64];
    uint8_t file[128];
    uint32_t magic;
    uint8_t opts[312];
} __attribute__((packed));

struct lease {
    uint32_t addr, mask, router, server, dns[2];
    uint32_t seconds;
};

static int dhcp_build(struct dhcp_pkt *p, uint32_t xid, const uint8_t mac[6], int type,
                      uint32_t req_ip, uint32_t server)
{
    memset(p, 0, sizeof(*p));
    p->op = 1;
    p->htype = 1;
    p->hlen = 6;
    p->xid = xid;
    p->flags = htons(0x8000);
    memcpy(p->chaddr, mac, 6);
    p->magic = htonl(0x63825363);
    uint8_t *o = p->opts;
    *o++ = 53; *o++ = 1; *o++ = (uint8_t)type;
    *o++ = 61; *o++ = 7; *o++ = 1; memcpy(o, mac, 6); o += 6;
    *o++ = 12; *o++ = 7; memcpy(o, "realios", 7); o += 7;
    if (req_ip) { *o++ = 50; *o++ = 4; memcpy(o, &req_ip, 4); o += 4; }
    if (server) { *o++ = 54; *o++ = 4; memcpy(o, &server, 4); o += 4; }
    *o++ = 55; *o++ = 4; *o++ = 1; *o++ = 3; *o++ = 6; *o++ = 51;
    *o++ = 255;
    return (int)(offsetof(struct dhcp_pkt, opts) + (size_t)(o - p->opts));
}

static int dhcp_parse(const struct dhcp_pkt *p, int len, uint32_t xid, struct lease *l)
{
    if (len < (int)offsetof(struct dhcp_pkt, opts) || p->op != 2 || p->xid != xid ||
        p->magic != htonl(0x63825363))
        return -1;
    int type = 0;
    l->addr = p->yiaddr;
    const uint8_t *o = p->opts, *end = (const uint8_t *)p + len;
    while (o < end && *o != 255) {
        if (*o == 0) { o++; continue; }
        if (o + 2 > end || o + 2 + o[1] > end)
            break;
        uint8_t code = o[0], ol = o[1];
        const uint8_t *v = o + 2;
        switch (code) {
        case 53: if (ol >= 1) type = v[0]; break;
        case 1:  if (ol >= 4) memcpy(&l->mask, v, 4); break;
        case 3:  if (ol >= 4) memcpy(&l->router, v, 4); break;
        case 6:
            if (ol >= 4) memcpy(&l->dns[0], v, 4);
            if (ol >= 8) memcpy(&l->dns[1], v + 4, 4);
            break;
        case 51: if (ol >= 4) { uint32_t t; memcpy(&t, v, 4); l->seconds = ntohl(t); } break;
        case 54: if (ol >= 4) memcpy(&l->server, v, 4); break;
        default: break;
        }
        o += 2 + ol;
    }
    return type;
}

static int dhcp_exchange(int s, struct dhcp_pkt *out, int outlen, uint32_t xid, int want,
                         struct lease *l, int timeout_ms)
{
    struct sockaddr_in to = {.sin_family = AF_INET, .sin_port = htons(67),
                             .sin_addr.s_addr = htonl(INADDR_BROADCAST)};
    uint64_t deadline = now_ms() + (uint64_t)timeout_ms;
    uint64_t next_send = 0;
    int tries = 0;
    while (now_ms() < deadline) {
        if (now_ms() >= next_send) {
            sendto(s, out, (size_t)outlen, 0, (struct sockaddr *)&to, sizeof(to));
            tries++;
            next_send = now_ms() + (uint64_t)(tries < 4 ? 1000 * tries : 4000);
        }
        struct pollfd p = {.fd = s, .events = POLLIN};
        if (poll(&p, 1, 250) <= 0)
            continue;
        struct dhcp_pkt in;
        ssize_t n = recv(s, &in, sizeof(in), 0);
        if (n <= 0)
            continue;
        memset(l, 0, sizeof(*l));
        int t = dhcp_parse(&in, (int)n, xid, l);
        if (t == want)
            return 0;
        if (t == 6)            /* NAK */
            return -2;
    }
    return -1;
}

static int dhcp_run(const char *ifname, int timeout_ms, struct lease *l)
{
    uint8_t mac[6];
    if (get_mac(ifname, mac) < 0)
        return -1;
    int s = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (s < 0)
        return -1;
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one));
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    setsockopt(s, SOL_SOCKET, SO_BINDTODEVICE, ifname, (socklen_t)strlen(ifname) + 1);
    struct sockaddr_in me = {.sin_family = AF_INET, .sin_port = htons(68)};
    if (bind(s, (struct sockaddr *)&me, sizeof(me)) < 0) {
        close(s);
        return -1;
    }
    uint32_t xid;
    int ufd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (ufd < 0 || read(ufd, &xid, sizeof(xid)) != sizeof(xid))
        xid = (uint32_t)now_ms() ^ ((uint32_t)mac[5] << 24);
    if (ufd >= 0)
        close(ufd);

    struct dhcp_pkt pkt;
    struct lease offer;
    int len = dhcp_build(&pkt, xid, mac, 1, 0, 0);
    int r = dhcp_exchange(s, &pkt, len, xid, 2, &offer, timeout_ms);
    if (r == 0) {
        len = dhcp_build(&pkt, xid, mac, 3, offer.addr, offer.server);
        r = dhcp_exchange(s, &pkt, len, xid, 5, l, 6000);
        if (r == 0 && !l->addr)
            l->addr = offer.addr;
        if (r == 0 && !l->server)
            l->server = offer.server;
    }
    close(s);
    return r;
}

/* ------------------------------------------------------------------ */

static void ip_str(uint32_t a, char *out, size_t cap)
{
    struct in_addr in = {.s_addr = a};
    snprintf(out, cap, "%s", inet_ntoa(in));
}

static int apply(const char *ifname, uint32_t addr, uint32_t mask, uint32_t gw,
                 uint32_t dns0, uint32_t dns1, const char *how)
{
    if (!mask)
        mask = htonl(0xFFFFFF00);
    if (if_set_addr(ifname, SIOCSIFADDR, addr) < 0)
        return -1;
    if_set_addr(ifname, SIOCSIFNETMASK, mask);
    if_set_addr(ifname, SIOCSIFBRDADDR, (addr & mask) | ~mask);
    if (gw && route_default(ifname, gw) < 0)
        klog("net: default route via %s failed: %s", ifname, strerror(errno));
    memset(&g_ns, 0, sizeof(g_ns));
    g_ns.up = gw != 0;
    snprintf(g_ns.ifname, sizeof(g_ns.ifname), "%s", ifname);
    ip_str(addr, g_ns.addr, sizeof(g_ns.addr));
    ip_str(gw, g_ns.gateway, sizeof(g_ns.gateway));
    char d0[20] = "", d1[20] = "";
    if (dns0) ip_str(dns0, d0, sizeof(d0));
    if (dns1) ip_str(dns1, d1, sizeof(d1));
    if (!dns0)
        snprintf(d0, sizeof(d0), "223.5.5.5");
    snprintf(g_ns.dns, sizeof(g_ns.dns), "%s%s%s", d0, d1[0] ? " " : "", d1);
    snprintf(g_ns.how, sizeof(g_ns.how), "%s", how);
    klog("net: %s %s addr=%s gw=%s dns=%s", how, ifname, g_ns.addr, g_ns.gateway, g_ns.dns);
    return 0;
}

int net_write_resolv(const char *path)
{
    if (!g_ns.up && !g_ns.dns[0])
        return -1;
    char buf[256];
    int n = snprintf(buf, sizeof(buf), "# realinit 生成（每次联网覆盖）\n");
    char dns[64];
    snprintf(dns, sizeof(dns), "%s", g_ns.dns);
    for (char *t = strtok(dns, " "); t; t = strtok(NULL, " "))
        n += snprintf(buf + n, sizeof(buf) - (size_t)n, "nameserver %s\n", t);
    return write_file(path, buf, (size_t)n, 0644);
}

/* /trans/net.conf：
 *   mode=dhcp|static|off   iface=eth0   address=  netmask=  gateway=  dns=a,b
 */
struct netconf {
    char mode[16], iface[32], address[32], netmask[32], gateway[32], dns[64];
    char ssid[64], psk[64];
};

static void netconf_load(const char *path, struct netconf *c)
{
    memset(c, 0, sizeof(*c));
    snprintf(c->mode, sizeof(c->mode), "dhcp");
    char buf[2048];
    if (!path || read_file(path, buf, sizeof(buf)) <= 0)
        return;
    for (char *line = strtok(buf, "\n"); line; line = strtok(NULL, "\n")) {
        str_trim(line);
        if (!line[0] || line[0] == '#')
            continue;
        char *eq = strchr(line, '=');
        if (!eq)
            continue;
        *eq = 0;
        char *k = line, *v = eq + 1;
        str_trim(k);
        str_trim(v);
#define SET(f) if (strcmp(k, #f) == 0) snprintf(c->f, sizeof(c->f), "%s", v)
        SET(mode); SET(iface); SET(address); SET(netmask); SET(gateway); SET(dns);
        SET(ssid); SET(psk);
#undef SET
    }
}

static int wait_carrier(const char *ifname, int ms)
{
    uint64_t end = now_ms() + (uint64_t)ms;
    do {
        if (read_sys_int(ifname, "carrier") == 1)
            return 1;
        usleep(100000);
    } while (now_ms() < end);
    return 0;
}

static int is_wifi(const char *ifname)
{
    char p[160];
    snprintf(p, sizeof(p), "/sys/class/net/%s/wireless", ifname);
    if (path_exists(p))
        return 1;
    snprintf(p, sizeof(p), "/sys/class/net/%s/phy80211", ifname);
    return path_exists(p);
}

static void rfkill_unblock(void)
{
    DIR *d = opendir("/sys/class/rfkill");
    if (!d)
        return;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.')
            continue;
        char p[160], t[32];
        snprintf(p, sizeof(p), "/sys/class/rfkill/%s/type", de->d_name);
        if (read_file(p, t, sizeof(t)) <= 0)
            continue;
        str_trim(t);
        if (strcmp(t, "wlan") != 0 && strcmp(t, "all") != 0)
            continue;
        snprintf(p, sizeof(p), "/sys/class/rfkill/%s/soft", de->d_name);
        write_file(p, "0\n", 2, 0644);
    }
    closedir(d);
}

static const char *wifi_iface(void)
{
    static char name[32];
    char names[16][32];
    int n = net_list_ifaces(names, 16);
    for (int i = 0; i < n; i++) {
        if (is_wifi(names[i])) {
            snprintf(name, sizeof(name), "%s", names[i]);
            return name;
        }
    }
    return NULL;
}

static int ap_upsert(struct wifi_ap *out, int *n, int max, const char *ssid, int signal, int locked)
{
    if (!ssid[0])
        return 0;
    for (int i = 0; i < *n; i++) {
        if (strcmp(out[i].ssid, ssid) == 0) {
            if (signal > out[i].signal)
                out[i].signal = signal;
            if (locked)
                out[i].locked = 1;
            return 0;
        }
    }
    if (*n >= max)
        return 0;
    snprintf(out[*n].ssid, sizeof(out[*n].ssid), "%s", ssid);
    out[*n].signal = signal;
    out[*n].locked = locked;
    (*n)++;
    return 1;
}

int net_wifi_scan(struct wifi_ap *out, int max, char *err, size_t errcap)
{
    if (max <= 0)
        return 0;
    memset(out, 0, (size_t)max * sizeof(*out));
    rfkill_unblock();
    const char *ifn = wifi_iface();
    if (!ifn) {
        if (err)
            snprintf(err, errcap, "没有发现无线网卡（没有驱动，或固件还没放到 /trans/firmware）");
        return -1;
    }
    if_up(ifn);
    usleep(300000);

    int s = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (s < 0) {
        if (err)
            snprintf(err, errcap, "打不开扫描套接字");
        return -1;
    }
    struct iwreq wrq;
    memset(&wrq, 0, sizeof(wrq));
    snprintf(wrq.ifr_name, IFNAMSIZ, "%s", ifn);
    if (ioctl(s, SIOCSIWSCAN, &wrq) < 0 && errno != EAGAIN && errno != EBUSY) {
        if (err)
            snprintf(err, errcap, "网卡 %s 不支持扫描（%s）", ifn, strerror(errno));
        close(s);
        return -1;
    }

    char buf[65535];
    int got = 0;
    for (int i = 0; i < 40; i++) {
        usleep(150000);
        memset(&wrq, 0, sizeof(wrq));
        snprintf(wrq.ifr_name, IFNAMSIZ, "%s", ifn);
        wrq.u.data.pointer = buf;
        wrq.u.data.length = sizeof(buf);
        wrq.u.data.flags = 0;
        if (ioctl(s, SIOCGIWSCAN, &wrq) == 0) {
            got = 1;
            break;
        }
        if (errno != EAGAIN && errno != E2BIG)
            break;
    }
    close(s);
    if (!got) {
        if (err)
            snprintf(err, errcap, "扫描超时（周围没有热点，或网卡还没就绪）");
        return 0;
    }

    int n = 0;
    char ssid[64] = "";
    int signal = -100, locked = 0, have = 0;
    char *pos = buf;
    char *end = buf + wrq.u.data.length;
    while (pos + IW_EV_LCP_LEN <= end) {
        struct iw_event ev;
        memset(&ev, 0, sizeof(ev));
        memcpy(&ev, pos, IW_EV_LCP_LEN);
        if (ev.len < IW_EV_LCP_LEN || pos + ev.len > end)
            break;
        if (ev.cmd == SIOCGIWAP) {
            if (have && ssid[0])
                ap_upsert(out, &n, max, ssid, signal, locked);
            ssid[0] = 0;
            signal = -100;
            locked = 0;
            have = 1;
        } else if (ev.cmd == SIOCGIWESSID) {
            struct iw_point pt;
            memset(&pt, 0, sizeof(pt));
            size_t plen = sizeof(struct iw_point) - IW_EV_POINT_OFF;
            if (pos + IW_EV_LCP_LEN + plen <= end)
                memcpy((char *)&pt + IW_EV_POINT_OFF, pos + IW_EV_LCP_LEN, plen);
            char *ess = pos + IW_EV_LCP_LEN + plen;
            int elen = pt.length;
            if (elen < 0)
                elen = 0;
            if (elen > (int)sizeof(ssid) - 1)
                elen = (int)sizeof(ssid) - 1;
            if (ess + elen <= end && elen > 0) {
                memcpy(ssid, ess, (size_t)elen);
                ssid[elen] = 0;
            }
        } else if (ev.cmd == IWEVQUAL) {
            if (pos + ev.len <= end)
                memcpy(&ev.u, pos + IW_EV_LCP_LEN, ev.len - IW_EV_LCP_LEN < (int)sizeof(ev.u)
                       ? (size_t)(ev.len - IW_EV_LCP_LEN) : sizeof(ev.u));
            int lv = ev.u.qual.level;
            if (ev.u.qual.updated & IW_QUAL_DBM)
                signal = (lv >= 64) ? lv - 256 : lv;
            else
                signal = lv;
        } else if (ev.cmd == SIOCGIWENCODE) {
            struct iw_point pt;
            memset(&pt, 0, sizeof(pt));
            size_t plen = sizeof(struct iw_point) - IW_EV_POINT_OFF;
            if (pos + IW_EV_LCP_LEN + plen <= end)
                memcpy((char *)&pt + IW_EV_POINT_OFF, pos + IW_EV_LCP_LEN, plen);
            locked = !(pt.flags & IW_ENCODE_DISABLED);
        }
        pos += ev.len;
    }
    if (have && ssid[0])
        ap_upsert(out, &n, max, ssid, signal, locked);

    for (int i = 0; i < n; i++)
        for (int j = i + 1; j < n; j++)
            if (out[j].signal > out[i].signal) {
                struct wifi_ap t = out[i];
                out[i] = out[j];
                out[j] = t;
            }
    if (n == 0 && err)
        snprintf(err, errcap, "附近没有搜到 Wi-Fi 名称");
    return n;
}

static int wpa_escape(char *dst, size_t cap, const char *s)
{
    size_t n = 0;
    if (n + 1 < cap) dst[n++] = '"';
    for (; *s && n + 2 < cap; s++) {
        if (*s == '"' || *s == '\\')
            dst[n++] = '\\';
        dst[n++] = *s;
    }
    if (n + 1 < cap) dst[n++] = '"';
    dst[n] = 0;
    return (int)n;
}

static int wifi_associate(const char *ssid, const char *psk, int verbose, char *ifname, size_t cap)
{
    if (access(BUILTIN_WPA, X_OK) != 0) {
        snprintf(g_ns.detail, sizeof(g_ns.detail), "这个镜像没有无线连接程序");
        return -1;
    }
    rfkill_unblock();
    const char *ifn = wifi_iface();
    if (!ifn) {
        snprintf(g_ns.detail, sizeof(g_ns.detail), "没有发现无线网卡（没有驱动，或固件还没加载）");
        return -1;
    }
    snprintf(ifname, cap, "%s", ifn);
    if_up(ifn);

    char essid[160], key[160];
    wpa_escape(essid, sizeof(essid), ssid);
    char body[512];
    int len;
    if (psk && psk[0]) {
        wpa_escape(key, sizeof(key), psk);
        len = snprintf(body, sizeof(body),
                       "ctrl_interface=/tmp/wpa\nupdate_config=0\nnetwork={\n"
                       "\tssid=%s\n\tpsk=%s\n}\n", essid, key);
    } else {
        len = snprintf(body, sizeof(body),
                       "ctrl_interface=/tmp/wpa\nupdate_config=0\nnetwork={\n"
                       "\tssid=%s\n\tkey_mgmt=NONE\n}\n", essid);
    }
    write_file("/tmp/wpa.conf", body, (size_t)len, 0600);

    int fd = open("/tmp/wpa.pid", O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        char b[16] = {0};
        if (read(fd, b, sizeof(b) - 1) > 0) {
            int pid = atoi(b);
            if (pid > 1) { kill(pid, SIGTERM); usleep(200000); kill(pid, SIGKILL); }
        }
        close(fd);
        unlink("/tmp/wpa.pid");
    }

    if (verbose)
        ui_status_line("Wi-Fi %s：正在连接 %s…", ifn, ssid);
    pid_t pid = fork();
    if (pid == 0) {
        int nfd = open("/dev/null", O_RDWR);
        if (nfd >= 0) { dup2(nfd, 0); dup2(nfd, 1); dup2(nfd, 2); if (nfd > 2) close(nfd); }
        char *argv[] = {BUILTIN_WPA, "-B", "-D", "wext", "-P", "/tmp/wpa.pid", "-i", (char *)ifn,
                        "-c", "/tmp/wpa.conf", NULL};
        execv(BUILTIN_WPA, argv);
        _exit(127);
    }
    if (pid > 0) {
        int st;
        waitpid(pid, &st, 0);
    }
    if (!wait_carrier(ifn, 20000)) {
        snprintf(g_ns.detail, sizeof(g_ns.detail), "Wi-Fi 没连上（名称/密码不对，或没有信号）");
        return -1;
    }
    return 0;
}

int net_bringup(const char *conf_path, int verbose)
{
    if (conf_path)
        snprintf(g_conf_path, sizeof(g_conf_path), "%s", conf_path);
    if_up("lo");
    if_set_addr("lo", SIOCSIFADDR, htonl(INADDR_LOOPBACK));

    struct netconf nc;
    netconf_load(g_conf_path[0] ? g_conf_path : NULL, &nc);
    memset(&g_ns, 0, sizeof(g_ns));
    if (strcmp(nc.mode, "off") == 0) {
        snprintf(g_ns.detail, sizeof(g_ns.detail), "net.conf: mode=off");
        return -1;
    }

    char names[16][32];
    int n = 0;
    /* USB 网卡 / 手机共享网络枚举得晚，最多再等 3 秒 */
    for (int i = 0; i < 30 && (n = net_list_ifaces(names, 16)) == 0; i++)
        usleep(100000);
    if (n == 0) {
        snprintf(g_ns.detail, sizeof(g_ns.detail), "没有发现网卡（没有匹配的驱动，或网卡还没插上）");
        return -1;
    }
    char list[160] = "";
    for (int i = 0; i < n; i++) {
        if_up(names[i]);
        size_t l = strlen(list);
        snprintf(list + l, sizeof(list) - l, "%s%s", i ? " " : "", names[i]);
    }

    if (strcmp(nc.mode, "wifi") == 0) {
        if (!nc.ssid[0]) {
            snprintf(g_ns.detail, sizeof(g_ns.detail), "net.conf：mode=wifi 但没有 ssid");
            return -1;
        }
        char ifn[32];
        if (wifi_associate(nc.ssid, nc.psk, verbose, ifn, sizeof(ifn)) < 0)
            return -1;
        if (verbose)
            ui_status_line("Wi-Fi %s：DHCP 获取地址…", ifn);
        struct lease l;
        if (dhcp_run(ifn, 12000, &l) == 0 && l.addr) {
            apply(ifn, l.addr, l.mask, l.router, l.dns[0], l.dns[1], "wifi");
            uint32_t secs = l.seconds ? l.seconds : 3600;
            g_renew_at = now_ms() + (uint64_t)secs * 500u;
            return g_ns.up ? 0 : -1;
        }
        snprintf(g_ns.detail, sizeof(g_ns.detail), "Wi-Fi 已关联，但 DHCP 没拿到地址");
        return -1;
    }

    if (strcmp(nc.mode, "static") == 0) {
        const char *ifn = nc.iface[0] ? nc.iface : names[0];
        struct in_addr a, m, g, d0 = {0}, d1 = {0};
        if (!inet_aton(nc.address, &a) || !inet_aton(nc.gateway, &g)) {
            snprintf(g_ns.detail, sizeof(g_ns.detail), "net.conf 静态配置不完整（需要 address 与 gateway）");
            return -1;
        }
        if (!inet_aton(nc.netmask[0] ? nc.netmask : "255.255.255.0", &m))
            m.s_addr = htonl(0xFFFFFF00);
        char dns[64];
        snprintf(dns, sizeof(dns), "%s", nc.dns);
        char *c = strchr(dns, ',');
        if (c) { *c = 0; inet_aton(c + 1, &d1); }
        inet_aton(dns, &d0);
        wait_carrier(ifn, 3000);
        return apply(ifn, a.s_addr, m.s_addr, g.s_addr, d0.s_addr, d1.s_addr, "static");
    }

    for (int i = 0; i < n; i++) {
        if (nc.iface[0] && strcmp(nc.iface, names[i]) != 0)
            continue;
        if (is_wifi(names[i]))
            continue;
        if (verbose)
            ui_status_line("网卡 %s：等待网线 / 载波…", names[i]);
        if (!wait_carrier(names[i], 4000))
            continue;
        if (verbose)
            ui_status_line("网卡 %s：DHCP 获取地址…", names[i]);
        struct lease l;
        if (dhcp_run(names[i], 10000, &l) == 0 && l.addr) {
            apply(names[i], l.addr, l.mask, l.router, l.dns[0], l.dns[1], "dhcp");
            uint32_t secs = l.seconds ? l.seconds : 3600;
            g_renew_at = now_ms() + (uint64_t)secs * 500u;
            return g_ns.up ? 0 : -1;
        }
    }
    snprintf(g_ns.detail, sizeof(g_ns.detail), "网卡 [%s] 都没拿到 DHCP 地址（没插网线？路由器没开 DHCP？）", list);
    return -1;
}

void net_tick(void)
{
    if (!g_renew_at || now_ms() < g_renew_at || strcmp(g_ns.how, "dhcp") != 0)
        return;
    char ifn[32];
    snprintf(ifn, sizeof(ifn), "%s", g_ns.ifname);
    struct lease l;
    g_renew_at = now_ms() + 60000;
    if (dhcp_run(ifn, 5000, &l) == 0 && l.addr) {
        apply(ifn, l.addr, l.mask, l.router, l.dns[0], l.dns[1], "dhcp");
        g_renew_at = now_ms() + (uint64_t)(l.seconds ? l.seconds : 3600) * 500u;
    }
}
