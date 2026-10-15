/*
 * initconf.c — 解析 ext4 上的 /trans/init，决定开机跑什么。
 *
 * /trans/init 可以是：
 *   不存在 / 空文件 / 只有注释      → 直接运行 trans-cli
 *   可执行文件（ELF 或 #! 脚本）    → 一个程序，弹菜单让用户在它和 trans 之间选
 *   目录                             → 目录里每个可执行文件都是一个菜单项
 *   文本清单                         → 每行一个程序：
 *         名称 | 命令行 [| direct]
 *         命令行
 *         @timeout=5     菜单倒计时（秒），0 = 一直等
 *         @default=1     倒计时结束后运行第几项（0 = trans-cli）
 */
#define _GNU_SOURCE
#include "realinit.h"

#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int file_is_program(const char *path)
{
    struct stat st;
    if (stat(path, &st) < 0 || !S_ISREG(st.st_mode) || !(st.st_mode & 0111))
        return 0;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return 0;
    unsigned char m[4] = {0};
    ssize_t r = read(fd, m, 4);
    close(fd);
    if (r >= 4 && m[0] == 0x7f && m[1] == 'E' && m[2] == 'L' && m[3] == 'F')
        return 1;
    if (r >= 2 && m[0] == '#' && m[1] == '!')
        return 1;
    return 0;
}

static void add(struct init_conf *c, const char *name, const char *cmd, int direct, int is_file)
{
    if (c->count >= INITCONF_MAX)
        return;
    struct init_entry *e = &c->e[c->count++];
    snprintf(e->name, sizeof(e->name), "%s", name);
    snprintf(e->cmd, sizeof(e->cmd), "%s", cmd);
    e->direct = direct;
    e->is_file = is_file;
}

static int cmp_names(const void *a, const void *b)
{
    return strcmp((const char *)a, (const char *)b);
}

int initconf_load(const char *root, struct init_conf *c)
{
    memset(c, 0, sizeof(*c));
    char path[512];
    snprintf(path, sizeof(path), "%s" TRANS_INIT, root);
    struct stat st;
    if (stat(path, &st) < 0) {
        snprintf(c->source, sizeof(c->source), "missing");
        return 0;
    }

    if (S_ISDIR(st.st_mode)) {
        snprintf(c->source, sizeof(c->source), "dir");
        DIR *d = opendir(path);
        if (!d)
            return 0;
        char names[INITCONF_MAX][96];
        int n = 0;
        struct dirent *de;
        while ((de = readdir(d)) && n < INITCONF_MAX) {
            if (de->d_name[0] == '.')
                continue;
            char p[700];
            snprintf(p, sizeof(p), "%s/%s", path, de->d_name);
            if (file_is_program(p))
                snprintf(names[n++], sizeof(names[0]), "%s", de->d_name);
        }
        closedir(d);
        qsort(names, (size_t)n, sizeof(names[0]), cmp_names);
        for (int i = 0; i < n; i++) {
            char cmd[256];
            snprintf(cmd, sizeof(cmd), TRANS_INIT "/%s", names[i]);
            add(c, names[i], cmd, 0, 1);
        }
        return c->count;
    }

    if (file_is_program(path)) {
        snprintf(c->source, sizeof(c->source), "file");
        add(c, TRANS_INIT, TRANS_INIT, 0, 1);
        return 1;
    }

    snprintf(c->source, sizeof(c->source), "list");
    char *buf = malloc(65536);
    if (!buf)
        return 0;
    if (read_file(path, buf, 65536) <= 0) {
        snprintf(c->source, sizeof(c->source), "empty");
        free(buf);
        return 0;
    }
    char *save = NULL;
    for (char *line = strtok_r(buf, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        str_trim(line);
        if (!line[0] || line[0] == '#')
            continue;
        if (line[0] == '@') {
            if (strncmp(line, "@timeout=", 9) == 0)
                c->timeout = atoi(line + 9);
            else if (strncmp(line, "@default=", 9) == 0)
                c->def = atoi(line + 9);
            continue;
        }
        char *f[3] = {line, NULL, NULL};
        int nf = 1;
        for (char *p = line; *p && nf < 3; p++)
            if (*p == '|') {
                *p = 0;
                f[nf++] = p + 1;
            }
        for (int i = 0; i < nf; i++)
            str_trim(f[i]);
        const char *name = f[0], *cmd = f[0];
        int direct = 0;
        if (nf >= 2) {
            cmd = f[1];
            if (nf >= 3 && strstr(f[2], "direct"))
                direct = 1;
        }
        if (cmd[0])
            add(c, name[0] ? name : cmd, cmd, direct, 0);
    }
    free(buf);
    if (c->count == 0)
        snprintf(c->source, sizeof(c->source), "empty");
    if (c->def < 0 || c->def > c->count)
        c->def = 0;
    if (c->timeout < 0)
        c->timeout = 0;
    return c->count;
}
