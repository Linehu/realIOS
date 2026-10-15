/*
 * dns.c — 127.0.0.1:53 上的 DNS 转发。
 *
 * ext4 上没有 /etc（realinit 不替用户建目录），而 Go 写的 trans-cli 找不到
 * /etc/resolv.conf 时会默认去问 127.0.0.1:53。所以 PID 1 在这里开一个 UDP 转发，
 * 上游取 initramfs 里 realinit 自己维护的 /etc/resolv.conf。用户哪天自己写了
 * /etc/resolv.conf，程序就直接用他的，这里也不碍事。
 */
#define _GNU_SOURCE
#include "realinit.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define DNS_PENDING 128

struct pending {
    int used;
    uint16_t orig_id, new_id;
    struct sockaddr_in client;
    uint64_t t;
};

static struct pending g_pend[DNS_PENDING];
static uint16_t g_next_id = 0x5200;

static int upstream(struct sockaddr_in *out)
{
    char buf[1024];
    struct in_addr a = {0};
    if (read_file("/etc/resolv.conf", buf, sizeof(buf)) > 0) {
        char *save = NULL;
        for (char *l = strtok_r(buf, "\n", &save); l; l = strtok_r(NULL, "\n", &save)) {
            char ip[64];
            if (sscanf(l, "nameserver %63s", ip) == 1 && inet_aton(ip, &a) &&
                a.s_addr != htonl(INADDR_LOOPBACK))
                break;
            a.s_addr = 0;
        }
    }
    if (!a.s_addr)
        inet_aton("223.5.5.5", &a);
    memset(out, 0, sizeof(*out));
    out->sin_family = AF_INET;
    out->sin_port = htons(53);
    out->sin_addr = a;
    return 0;
}

static void *dns_loop(void *arg)
{
    (void)arg;
    int lfd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    int ufd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (lfd < 0 || ufd < 0)
        return NULL;
    struct sockaddr_in la = {.sin_family = AF_INET, .sin_port = htons(53)};
    la.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int one = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    if (bind(lfd, (struct sockaddr *)&la, sizeof(la)) < 0) {
        klog("dns: bind 127.0.0.1:53 failed");
        close(lfd);
        close(ufd);
        return NULL;
    }
    klog("dns: forwarder on 127.0.0.1:53");

    unsigned char pkt[4096];
    for (;;) {
        struct pollfd p[2] = {{.fd = lfd, .events = POLLIN}, {.fd = ufd, .events = POLLIN}};
        if (poll(p, 2, 1000) < 0)
            continue;
        uint64_t now = now_ms();

        if (p[0].revents & POLLIN) {
            struct sockaddr_in from;
            socklen_t fl = sizeof(from);
            ssize_t n = recvfrom(lfd, pkt, sizeof(pkt), 0, (struct sockaddr *)&from, &fl);
            if (n >= 12) {
                int slot = -1;
                for (int i = 0; i < DNS_PENDING; i++)
                    if (!g_pend[i].used || now - g_pend[i].t > 5000) { slot = i; break; }
                if (slot >= 0) {
                    struct pending *e = &g_pend[slot];
                    e->used = 1;
                    e->orig_id = (uint16_t)(pkt[0] << 8 | pkt[1]);
                    e->new_id = g_next_id++;
                    e->client = from;
                    e->t = now;
                    pkt[0] = (unsigned char)(e->new_id >> 8);
                    pkt[1] = (unsigned char)e->new_id;
                    struct sockaddr_in up;
                    upstream(&up);
                    sendto(ufd, pkt, (size_t)n, 0, (struct sockaddr *)&up, sizeof(up));
                }
            }
        }

        if (p[1].revents & POLLIN) {
            ssize_t n = recv(ufd, pkt, sizeof(pkt), 0);
            if (n >= 12) {
                uint16_t id = (uint16_t)(pkt[0] << 8 | pkt[1]);
                for (int i = 0; i < DNS_PENDING; i++) {
                    struct pending *e = &g_pend[i];
                    if (!e->used || e->new_id != id)
                        continue;
                    pkt[0] = (unsigned char)(e->orig_id >> 8);
                    pkt[1] = (unsigned char)e->orig_id;
                    sendto(lfd, pkt, (size_t)n, 0, (struct sockaddr *)&e->client, sizeof(e->client));
                    e->used = 0;
                    break;
                }
            }
        }
    }
    return NULL;
}

int dns_start(void)
{
    pthread_t th;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    int r = pthread_create(&th, &at, dns_loop, NULL);
    pthread_attr_destroy(&at);
    return r == 0 ? 0 : -1;
}
