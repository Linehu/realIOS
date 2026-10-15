/*
 * term.c — realinit 的显示层。
 *
 * 有 /dev/fb0 + 字体时：自己当终端模拟器（UTF-8、中文双宽、xterm 子集），
 * 画在 UEFI GOP 帧缓冲上；Linux 自带的 fbcon 显示不了中文，所以 VT 切到
 * KD_GRAPHICS，只借它的键盘（tty1 raw 模式）。
 * 没有帧缓冲时：直接把字节流写给 /dev/console（串口终端自己会渲染）。
 *
 * realios.mirror=ttyS0：画面字节同时镜像到串口，串口输入也当键盘用。
 */
#define _GNU_SOURCE
#include "realinit.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/kd.h>
#include <linux/vt.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/* 字体：RFNT = "RFNT" u32 ver u32 count | width[count] | glyph[count][32] */

static const uint8_t *g_font_w;
static const uint8_t *g_font_g;
static uint32_t g_font_n;

static int font_load(const char *path)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        klog("font: open %s: %s", path, strerror(errno));
        return -1;
    }
    struct stat st;
    if (fstat(fd, &st) < 0 || st.st_size < 12) {
        close(fd);
        return -1;
    }
    /* initramfs 是 ramfs，不支持 mmap，必须读进内存 */
    uint8_t *m = malloc((size_t)st.st_size);
    if (!m) {
        close(fd);
        return -1;
    }
    size_t off = 0;
    while (off < (size_t)st.st_size) {
        ssize_t r = read(fd, m + off, (size_t)st.st_size - off);
        if (r <= 0) {
            free(m);
            close(fd);
            return -1;
        }
        off += (size_t)r;
    }
    close(fd);
    if (memcmp(m, "RFNT", 4) != 0) {
        free(m);
        return -1;
    }
    uint32_t n;
    memcpy(&n, m + 8, 4);
    if ((uint64_t)12 + n + (uint64_t)n * 32 > (uint64_t)st.st_size) {
        free(m);
        return -1;
    }
    g_font_n = n;
    g_font_w = m + 12;
    g_font_g = m + 12 + n;
    return 0;
}

static int is_zero_width(uint32_t cp)
{
    return (cp >= 0x0300 && cp <= 0x036F) || (cp >= 0x200B && cp <= 0x200F) ||
           (cp >= 0x20D0 && cp <= 0x20FF) || (cp >= 0xFE00 && cp <= 0xFE0F) ||
           cp == 0x00AD || (cp >= 0x1AB0 && cp <= 0x1AFF);
}

static int cp_width(uint32_t cp)
{
    if (cp < 0x20)
        return 0;
    if (is_zero_width(cp))
        return 0;
    if (cp < g_font_n && g_font_w[cp])
        return g_font_w[cp] == 2 ? 2 : 1;
    if ((cp >= 0x1F300 && cp <= 0x1FAFF) || (cp >= 0x20000 && cp <= 0x3FFFD) ||
        (cp >= 0x2E80 && cp <= 0xA4CF) || (cp >= 0xAC00 && cp <= 0xD7A3) ||
        (cp >= 0xF900 && cp <= 0xFAFF) || (cp >= 0xFF00 && cp <= 0xFF60))
        return 2;
    return 1;
}

/* ------------------------------------------------------------------ */
/* 帧缓冲 */

static uint8_t *g_fb;
static size_t g_fb_len;
static int g_xres, g_yres, g_pitch, g_bpp;
static struct fb_bitfield g_fr, g_fg, g_fbb;
static int g_scale = 1;
static int g_cw = 8, g_ch = 16;

static inline uint32_t pack_rgb(uint32_t rgb)
{
    uint32_t r = (rgb >> 16) & 0xff, g = (rgb >> 8) & 0xff, b = rgb & 0xff;
    return ((r >> (8 - g_fr.length)) << g_fr.offset) |
           ((g >> (8 - g_fg.length)) << g_fg.offset) |
           ((b >> (8 - g_fbb.length)) << g_fbb.offset);
}

static inline void put_px(int x, int y, uint32_t pix)
{
    uint8_t *p = g_fb + (size_t)y * (size_t)g_pitch + (size_t)x * (size_t)(g_bpp / 8);
    switch (g_bpp) {
    case 32: *(uint32_t *)p = pix; break;
    case 24: p[0] = (uint8_t)pix; p[1] = (uint8_t)(pix >> 8); p[2] = (uint8_t)(pix >> 16); break;
    case 16: *(uint16_t *)p = (uint16_t)pix; break;
    default: break;
    }
}

static int fb_open(void)
{
    int fd = open("/dev/fb0", O_RDWR | O_CLOEXEC);
    if (fd < 0)
        return -1;
    struct fb_var_screeninfo v;
    struct fb_fix_screeninfo f;
    if (ioctl(fd, FBIOGET_VSCREENINFO, &v) < 0 || ioctl(fd, FBIOGET_FSCREENINFO, &f) < 0) {
        close(fd);
        return -1;
    }
    if (v.bits_per_pixel != 32 && v.bits_per_pixel != 24 && v.bits_per_pixel != 16) {
        close(fd);
        return -1;
    }
    g_fb_len = f.smem_len;
    g_fb = mmap(NULL, g_fb_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (g_fb == MAP_FAILED) {
        g_fb = NULL;
        return -1;
    }
    g_xres = (int)v.xres;
    g_yres = (int)v.yres;
    g_pitch = (int)f.line_length;
    g_bpp = (int)v.bits_per_pixel;
    g_fr = v.red;
    g_fg = v.green;
    g_fbb = v.blue;
    return 0;
}

/* ------------------------------------------------------------------ */
/* 终端状态 */

#define A_BOLD  1
#define A_UNDER 2
#define A_REV   4
#define A_DIM   8

struct cell {
    uint32_t cp;
    uint32_t fg, bg;
    uint8_t attr;
    uint8_t wide;   /* 0 普通, 1 双宽首格, 2 双宽次格 */
};

#define DEF_FG 0xD8DEE9u
#define DEF_BG 0x0E1116u

static int g_cols = 80, g_rows = 25;
static struct cell *g_grid, *g_alt;
static uint8_t *g_dirty;
static int g_cx, g_cy, g_wrap_pending;
static int g_sx, g_sy;
static uint32_t g_pen_fg = DEF_FG, g_pen_bg = DEF_BG;
static uint8_t g_pen_attr;
static int g_top, g_bot;
static int g_cursor_on = 1;
static int g_alt_active;
static int g_last_cx = -1, g_last_cy = -1;

static uint32_t g_pal[256];

static void palette_init(void)
{
    static const uint32_t base[16] = {
        0x1B1F27, 0xE06C75, 0x98C379, 0xE5C07B, 0x61AFEF, 0xC678DD, 0x56B6C2, 0xD8DEE9,
        0x5C6370, 0xFF7A85, 0xB5E890, 0xFFD68A, 0x8CC8FF, 0xE0A0FF, 0x7FE0EA, 0xFFFFFF,
    };
    for (int i = 0; i < 16; i++)
        g_pal[i] = base[i];
    static const int lv[6] = {0, 95, 135, 175, 215, 255};
    for (int i = 0; i < 216; i++)
        g_pal[16 + i] = ((uint32_t)lv[i / 36] << 16) | ((uint32_t)lv[(i / 6) % 6] << 8) | (uint32_t)lv[i % 6];
    for (int i = 0; i < 24; i++) {
        uint32_t g = (uint32_t)(8 + i * 10);
        g_pal[232 + i] = (g << 16) | (g << 8) | g;
    }
}

/* ------------------------------------------------------------------ */
/* I/O 端点 */

static int g_out_fd = -1;      /* 无帧缓冲时的输出（/dev/console）*/
static int g_kbd_fd = -1;      /* 键盘输入 */
static int g_ser_fd = -1;      /* 串口镜像（可选）*/
static int g_reply_fd = -1;    /* DSR/DA 应答写回当前子进程的 pty */
static int g_vt_fd = -1;       /* 用于 KDSETMODE 的 VT */
static struct termios g_kbd_saved, g_ser_saved;
static int g_kbd_saved_ok, g_ser_saved_ok;
static void (*g_idle_hook)(void);

void term_set_idle_hook(void (*hook)(void)) { g_idle_hook = hook; }
int term_cols(void) { return g_cols; }
int term_rows(void) { return g_rows; }
int term_has_fb(void) { return g_fb != NULL; }

static void raw_mode(int fd, struct termios *saved, int *saved_ok)
{
    struct termios t;
    if (tcgetattr(fd, &t) < 0)
        return;
    *saved = t;
    *saved_ok = 1;
    cfmakeraw(&t);
    t.c_cc[VMIN] = 1;
    t.c_cc[VTIME] = 0;
    tcsetattr(fd, TCSANOW, &t);
}

/* ------------------------------------------------------------------ */
/* 渲染 */

static void draw_cell(int r, int c, int with_cursor)
{
    struct cell *ce = &g_grid[r * g_cols + c];
    if (ce->wide == 2 && c > 0) {
        draw_cell(r, c - 1, with_cursor);
        return;
    }
    uint32_t fg = ce->fg, bg = ce->bg;
    if (ce->attr & A_DIM)
        fg = ((fg >> 1) & 0x7F7F7F) + ((bg >> 1) & 0x7F7F7F);
    if (ce->attr & A_REV) {
        uint32_t t = fg; fg = bg; bg = t;
    }
    int cur = with_cursor && g_cursor_on && r == g_cy && (c == g_cx || (ce->wide == 1 && c + 1 == g_cx));
    if (cur) {
        uint32_t t = fg; fg = bg; bg = t;
        if (fg == bg)
            fg = ~bg & 0xFFFFFF;
    }
    int cells = ce->wide == 1 ? 2 : 1;
    uint32_t pf = pack_rgb(fg), pb = pack_rgb(bg);
    int px0 = c * g_cw, py0 = r * g_ch;
    uint32_t cp = ce->cp;
    const uint8_t *gl = NULL;
    int gw = 0;
    if (cp > 0x20 && cp < g_font_n && g_font_w[cp]) {
        gl = g_font_g + (size_t)cp * 32;
        gw = g_font_w[cp] == 2 ? 16 : 8;
    } else if (cp > 0x20 && !is_zero_width(cp)) {
        gl = NULL;
        gw = -1;            /* 缺字：画空心框 */
    }
    int wpx = cells * 8;
    for (int gy = 0; gy < 16; gy++) {
        for (int gx = 0; gx < wpx; gx++) {
            int on = 0;
            if (gl && gx < gw) {
                if (gw == 8)
                    on = (gl[gy] >> (7 - gx)) & 1;
                else
                    on = (gl[gy * 2 + (gx >> 3)] >> (7 - (gx & 7))) & 1;
            } else if (gw == -1) {
                on = (gy == 2 || gy == 13) ? (gx >= 1 && gx < wpx - 1)
                                            : ((gx == 1 || gx == wpx - 2) && gy > 2 && gy < 13);
            }
            if ((ce->attr & A_UNDER) && gy == 15)
                on = 1;
            if ((ce->attr & A_BOLD) && !on && gl && gx > 0 && gx - 1 < gw) {
                if (gw == 8)
                    on = (gl[gy] >> (7 - (gx - 1))) & 1;
                else
                    on = (gl[gy * 2 + ((gx - 1) >> 3)] >> (7 - ((gx - 1) & 7))) & 1;
            }
            uint32_t pix = on ? pf : pb;
            for (int sy = 0; sy < g_scale; sy++)
                for (int sx = 0; sx < g_scale; sx++)
                    put_px(px0 + gx * g_scale + sx, py0 + gy * g_scale + sy, pix);
        }
    }
}

void term_flush(void)
{
    if (!g_fb)
        return;
    if (g_last_cy >= 0 && g_last_cy < g_rows)
        g_dirty[g_last_cy] = 1;
    if (g_cy >= 0 && g_cy < g_rows)
        g_dirty[g_cy] = 1;
    for (int r = 0; r < g_rows; r++) {
        if (!g_dirty[r])
            continue;
        g_dirty[r] = 0;
        for (int c = 0; c < g_cols; c++) {
            if (g_grid[r * g_cols + c].wide == 2)
                continue;
            draw_cell(r, c, 1);
        }
    }
    g_last_cx = g_cx;
    g_last_cy = g_cy;
}

static void mark_all(void)
{
    if (g_dirty)
        memset(g_dirty, 1, (size_t)g_rows);
}

/* ------------------------------------------------------------------ */
/* 网格操作 */

static void clear_cells(int r, int c0, int c1)
{
    if (r < 0 || r >= g_rows)
        return;
    if (c0 < 0) c0 = 0;
    if (c1 > g_cols) c1 = g_cols;
    for (int c = c0; c < c1; c++) {
        struct cell *ce = &g_grid[r * g_cols + c];
        ce->cp = ' ';
        ce->fg = g_pen_fg;
        ce->bg = g_pen_bg;
        ce->attr = 0;
        ce->wide = 0;
    }
    if (c0 > 0 && g_grid[r * g_cols + c0].wide == 0 && g_grid[r * g_cols + c0 - 1].wide == 1)
        g_grid[r * g_cols + c0 - 1].wide = 0;
    g_dirty[r] = 1;
}

static void scroll_up(int top, int bot, int n)
{
    if (n <= 0)
        return;
    if (n > bot - top + 1)
        n = bot - top + 1;
    memmove(&g_grid[top * g_cols], &g_grid[(top + n) * g_cols],
            sizeof(struct cell) * (size_t)g_cols * (size_t)(bot - top + 1 - n));
    for (int r = bot - n + 1; r <= bot; r++)
        clear_cells(r, 0, g_cols);
    for (int r = top; r <= bot; r++)
        g_dirty[r] = 1;
}

static void scroll_down(int top, int bot, int n)
{
    if (n <= 0)
        return;
    if (n > bot - top + 1)
        n = bot - top + 1;
    memmove(&g_grid[(top + n) * g_cols], &g_grid[top * g_cols],
            sizeof(struct cell) * (size_t)g_cols * (size_t)(bot - top + 1 - n));
    for (int r = top; r < top + n; r++)
        clear_cells(r, 0, g_cols);
    for (int r = top; r <= bot; r++)
        g_dirty[r] = 1;
}

static void linefeed(void)
{
    if (g_cy == g_bot)
        scroll_up(g_top, g_bot, 1);
    else if (g_cy < g_rows - 1)
        g_cy++;
}

static void put_cp(uint32_t cp)
{
    int w = cp_width(cp);
    if (w == 0)
        return;
    if (g_wrap_pending) {
        g_cx = 0;
        linefeed();
        g_wrap_pending = 0;
    }
    if (w == 2 && g_cx == g_cols - 1) {
        clear_cells(g_cy, g_cx, g_cols);
        g_cx = 0;
        linefeed();
    }
    struct cell *row = &g_grid[g_cy * g_cols];
    for (int i = 0; i < w; i++) {
        int c = g_cx + i;
        if (row[c].wide == 1 && c + 1 < g_cols && i == w - 1)
            row[c + 1].cp = ' ', row[c + 1].wide = 0;
        if (row[c].wide == 2 && c > 0 && i == 0)
            row[c - 1].cp = ' ', row[c - 1].wide = 0;
    }
    row[g_cx].cp = cp;
    row[g_cx].fg = g_pen_fg;
    row[g_cx].bg = g_pen_bg;
    row[g_cx].attr = g_pen_attr;
    row[g_cx].wide = w == 2 ? 1 : 0;
    if (w == 2) {
        row[g_cx + 1] = row[g_cx];
        row[g_cx + 1].cp = 0;
        row[g_cx + 1].wide = 2;
    }
    g_dirty[g_cy] = 1;
    if (g_cx + w >= g_cols) {
        g_cx = g_cols - 1;
        g_wrap_pending = 1;
    } else {
        g_cx += w;
    }
}

static void reset_state(void)
{
    g_pen_fg = DEF_FG;
    g_pen_bg = DEF_BG;
    g_pen_attr = 0;
    g_top = 0;
    g_bot = g_rows - 1;
    g_cursor_on = 1;
    g_wrap_pending = 0;
}

void term_clear(void)
{
    if (!g_fb) {
        term_puts("\x1b[0m\x1b[2J\x1b[H");
        return;
    }
    reset_state();
    for (int r = 0; r < g_rows; r++)
        clear_cells(r, 0, g_cols);
    g_cx = g_cy = 0;
    if (g_ser_fd >= 0)
        (void)!write(g_ser_fd, "\x1b[0m\x1b[2J\x1b[H", 11);
    term_flush();
}

/* ------------------------------------------------------------------ */
/* 解析器 */

enum { ST_GROUND, ST_ESC, ST_CSI, ST_OSC, ST_OSC_ESC, ST_SKIP1 };
static int g_st;
static int g_par[16], g_npar, g_priv;
static int g_u8_need;
static uint32_t g_u8_cp;

static void reply(const char *s)
{
    if (g_reply_fd >= 0)
        (void)!write(g_reply_fd, s, strlen(s));
}

static int par(int i, int def)
{
    return (i < g_npar && g_par[i] > 0) ? g_par[i] : def;
}

static uint32_t sgr_color(int *i, int count)
{
    /* 38;5;n 或 38;2;r;g;b */
    if (*i + 1 < count && g_par[*i + 1] == 5 && *i + 2 < count) {
        int n = g_par[*i + 2] & 255;
        *i += 2;
        return g_pal[n];
    }
    if (*i + 1 < count && g_par[*i + 1] == 2 && *i + 4 < count) {
        uint32_t c = ((uint32_t)(g_par[*i + 2] & 255) << 16) | ((uint32_t)(g_par[*i + 3] & 255) << 8) |
                     (uint32_t)(g_par[*i + 4] & 255);
        *i += 4;
        return c;
    }
    return 0xFFFFFFFFu;
}

static void do_sgr(void)
{
    int n = g_npar ? g_npar : 1;
    for (int i = 0; i < n; i++) {
        int p = i < g_npar ? g_par[i] : 0;
        if (p < 0) p = 0;
        if (p == 0) { g_pen_fg = DEF_FG; g_pen_bg = DEF_BG; g_pen_attr = 0; }
        else if (p == 1) g_pen_attr |= A_BOLD;
        else if (p == 2) g_pen_attr |= A_DIM;
        else if (p == 4) g_pen_attr |= A_UNDER;
        else if (p == 7) g_pen_attr |= A_REV;
        else if (p == 22) g_pen_attr &= (uint8_t)~(A_BOLD | A_DIM);
        else if (p == 24) g_pen_attr &= (uint8_t)~A_UNDER;
        else if (p == 27) g_pen_attr &= (uint8_t)~A_REV;
        else if (p >= 30 && p <= 37) g_pen_fg = g_pal[p - 30 + ((g_pen_attr & A_BOLD) ? 8 : 0)];
        else if (p == 38) { uint32_t c = sgr_color(&i, g_npar); if (c != 0xFFFFFFFFu) g_pen_fg = c; }
        else if (p == 39) g_pen_fg = DEF_FG;
        else if (p >= 40 && p <= 47) g_pen_bg = g_pal[p - 40];
        else if (p == 48) { uint32_t c = sgr_color(&i, g_npar); if (c != 0xFFFFFFFFu) g_pen_bg = c; }
        else if (p == 49) g_pen_bg = DEF_BG;
        else if (p >= 90 && p <= 97) g_pen_fg = g_pal[p - 90 + 8];
        else if (p >= 100 && p <= 107) g_pen_bg = g_pal[p - 100 + 8];
    }
}

static void clamp_cursor(void)
{
    if (g_cx < 0) g_cx = 0;
    if (g_cx >= g_cols) g_cx = g_cols - 1;
    if (g_cy < 0) g_cy = 0;
    if (g_cy >= g_rows) g_cy = g_rows - 1;
}

static void set_mode(int on)
{
    for (int i = 0; i < g_npar; i++) {
        int p = g_par[i];
        if (g_priv == '?') {
            if (p == 25)
                g_cursor_on = on;
            else if (p == 1049 || p == 47 || p == 1047) {
                if (on && !g_alt_active) {
                    memcpy(g_alt, g_grid, sizeof(struct cell) * (size_t)g_cols * (size_t)g_rows);
                    g_sx = g_cx; g_sy = g_cy;
                    for (int r = 0; r < g_rows; r++)
                        clear_cells(r, 0, g_cols);
                    g_alt_active = 1;
                } else if (!on && g_alt_active) {
                    memcpy(g_grid, g_alt, sizeof(struct cell) * (size_t)g_cols * (size_t)g_rows);
                    g_cx = g_sx; g_cy = g_sy;
                    g_alt_active = 0;
                    mark_all();
                }
            }
        }
    }
}

static void do_csi(int ch)
{
    struct cell *row;
    int n;
    g_wrap_pending = ch == 'm' ? g_wrap_pending : 0;
    switch (ch) {
    case 'A': g_cy -= par(0, 1); if (g_cy < g_top && g_cy + par(0, 1) >= g_top) g_cy = g_top; break;
    case 'B': case 'e': g_cy += par(0, 1); if (g_cy > g_bot && g_cy - par(0, 1) <= g_bot) g_cy = g_bot; break;
    case 'C': case 'a': g_cx += par(0, 1); break;
    case 'D': g_cx -= par(0, 1); break;
    case 'E': g_cy += par(0, 1); g_cx = 0; break;
    case 'F': g_cy -= par(0, 1); g_cx = 0; break;
    case 'G': case '`': g_cx = par(0, 1) - 1; break;
    case 'd': g_cy = par(0, 1) - 1; break;
    case 'H': case 'f': g_cy = par(0, 1) - 1; g_cx = par(1, 1) - 1; break;
    case 'J':
        n = g_npar ? g_par[0] : 0;
        if (n == 0) {
            clear_cells(g_cy, g_cx, g_cols);
            for (int r = g_cy + 1; r < g_rows; r++) clear_cells(r, 0, g_cols);
        } else if (n == 1) {
            for (int r = 0; r < g_cy; r++) clear_cells(r, 0, g_cols);
            clear_cells(g_cy, 0, g_cx + 1);
        } else {
            for (int r = 0; r < g_rows; r++) clear_cells(r, 0, g_cols);
        }
        break;
    case 'K':
        n = g_npar ? g_par[0] : 0;
        if (n == 0) clear_cells(g_cy, g_cx, g_cols);
        else if (n == 1) clear_cells(g_cy, 0, g_cx + 1);
        else clear_cells(g_cy, 0, g_cols);
        break;
    case 'X':
        clear_cells(g_cy, g_cx, g_cx + par(0, 1));
        break;
    case 'L':
        if (g_cy >= g_top && g_cy <= g_bot) scroll_down(g_cy, g_bot, par(0, 1));
        break;
    case 'M':
        if (g_cy >= g_top && g_cy <= g_bot) scroll_up(g_cy, g_bot, par(0, 1));
        break;
    case 'S': scroll_up(g_top, g_bot, par(0, 1)); break;
    case 'T': scroll_down(g_top, g_bot, par(0, 1)); break;
    case 'P':
        n = par(0, 1);
        row = &g_grid[g_cy * g_cols];
        if (n > g_cols - g_cx) n = g_cols - g_cx;
        memmove(&row[g_cx], &row[g_cx + n], sizeof(struct cell) * (size_t)(g_cols - g_cx - n));
        clear_cells(g_cy, g_cols - n, g_cols);
        break;
    case '@':
        n = par(0, 1);
        row = &g_grid[g_cy * g_cols];
        if (n > g_cols - g_cx) n = g_cols - g_cx;
        memmove(&row[g_cx + n], &row[g_cx], sizeof(struct cell) * (size_t)(g_cols - g_cx - n));
        clear_cells(g_cy, g_cx, g_cx + n);
        break;
    case 'm': do_sgr(); break;
    case 'r':
        g_top = par(0, 1) - 1;
        g_bot = par(1, g_rows) - 1;
        if (g_top < 0) g_top = 0;
        if (g_bot >= g_rows) g_bot = g_rows - 1;
        if (g_top >= g_bot) { g_top = 0; g_bot = g_rows - 1; }
        g_cx = 0; g_cy = 0;
        break;
    case 's': g_sx = g_cx; g_sy = g_cy; break;
    case 'u': g_cx = g_sx; g_cy = g_sy; break;
    case 'h': set_mode(1); break;
    case 'l': set_mode(0); break;
    case 'n':
        if (par(0, 0) == 6) {
            char b[32];
            snprintf(b, sizeof(b), "\x1b[%d;%dR", g_cy + 1, g_cx + 1);
            reply(b);
        } else if (par(0, 0) == 5) {
            reply("\x1b[0n");
        }
        break;
    case 'c':
        if (g_priv == 0) reply("\x1b[?62;22c");
        else if (g_priv == '>') reply("\x1b[>0;10;0c");
        break;
    case 't':
        if (par(0, 0) == 18) {
            char b[32];
            snprintf(b, sizeof(b), "\x1b[8;%d;%dt", g_rows, g_cols);
            reply(b);
        }
        break;
    default: break;
    }
    clamp_cursor();
}

static void feed(uint8_t b)
{
    switch (g_st) {
    case ST_GROUND:
        if (g_u8_need) {
            if ((b & 0xC0) == 0x80) {
                g_u8_cp = (g_u8_cp << 6) | (b & 0x3F);
                if (--g_u8_need == 0)
                    put_cp(g_u8_cp);
                return;
            }
            g_u8_need = 0;
            put_cp(0xFFFD);
        }
        if (b >= 0xC0 && b < 0xE0) { g_u8_need = 1; g_u8_cp = b & 0x1F; return; }
        if (b >= 0xE0 && b < 0xF0) { g_u8_need = 2; g_u8_cp = b & 0x0F; return; }
        if (b >= 0xF0 && b < 0xF8) { g_u8_need = 3; g_u8_cp = b & 0x07; return; }
        if (b >= 0x80) { put_cp(0xFFFD); return; }
        switch (b) {
        case 0x1b: g_st = ST_ESC; return;
        case '\r': g_cx = 0; g_wrap_pending = 0; return;
        case '\n': case 0x0b: case 0x0c: linefeed(); g_wrap_pending = 0; return;
        case '\b': if (g_cx > 0) g_cx--; g_wrap_pending = 0; return;
        case '\t': g_cx = (g_cx / 8 + 1) * 8; if (g_cx >= g_cols) g_cx = g_cols - 1; return;
        case 0x07: case 0x0e: case 0x0f: case 0: return;
        default:
            if (b >= 0x20 && b != 0x7f)
                put_cp(b);
            return;
        }
    case ST_ESC:
        g_st = ST_GROUND;
        switch (b) {
        case '[': g_st = ST_CSI; g_npar = 0; g_priv = 0; memset(g_par, 0, sizeof(g_par)); g_par[0] = -1; return;
        case ']': g_st = ST_OSC; return;
        case '(': case ')': case '*': case '+': case '#': case '%': g_st = ST_SKIP1; return;
        case '7': g_sx = g_cx; g_sy = g_cy; return;
        case '8': g_cx = g_sx; g_cy = g_sy; return;
        case 'D': linefeed(); return;
        case 'E': g_cx = 0; linefeed(); return;
        case 'M':
            if (g_cy == g_top) scroll_down(g_top, g_bot, 1);
            else if (g_cy > 0) g_cy--;
            return;
        case 'c':
            reset_state();
            for (int r = 0; r < g_rows; r++) clear_cells(r, 0, g_cols);
            g_cx = g_cy = 0;
            return;
        default: return;
        }
    case ST_SKIP1:
        g_st = ST_GROUND;
        return;
    case ST_CSI:
        if (b >= '0' && b <= '9') {
            if (g_npar == 0) g_npar = 1;
            if (g_par[g_npar - 1] < 0) g_par[g_npar - 1] = 0;
            if (g_par[g_npar - 1] < 100000)
                g_par[g_npar - 1] = g_par[g_npar - 1] * 10 + (b - '0');
            return;
        }
        if (b == ';' || b == ':') {
            if (g_npar == 0) g_npar = 1;
            if (g_npar < 16) { g_par[g_npar] = -1; g_npar++; }
            return;
        }
        if (b == '?' || b == '>' || b == '<' || b == '=') { g_priv = b; return; }
        if (b >= 0x20 && b <= 0x2f) return;      /* intermediate */
        if (b >= 0x40 && b <= 0x7e) {
            for (int i = 0; i < g_npar; i++)
                if (g_par[i] < 0) g_par[i] = 0;
            g_st = ST_GROUND;
            do_csi(b);
            return;
        }
        if (b == 0x1b) { g_st = ST_ESC; return; }
        g_st = ST_GROUND;
        return;
    case ST_OSC:
        if (b == 0x07) g_st = ST_GROUND;
        else if (b == 0x1b) g_st = ST_OSC_ESC;
        return;
    case ST_OSC_ESC:
        g_st = b == '\\' ? ST_GROUND : ST_OSC;
        return;
    }
}

void term_write(const char *s, size_t n)
{
    if (g_ser_fd >= 0)
        (void)!write(g_ser_fd, s, n);
    if (!g_fb) {
        if (g_out_fd >= 0)
            (void)!write(g_out_fd, s, n);
        return;
    }
    for (size_t i = 0; i < n; i++)
        feed((uint8_t)s[i]);
}

void term_puts(const char *s)
{
    /* 自家 UI 文本：\n 自动补 \r（子进程输出由 pty 的 ONLCR 负责）*/
    const char *p = s;
    while (*p) {
        const char *nl = strchr(p, '\n');
        if (!nl) {
            term_write(p, strlen(p));
            break;
        }
        term_write(p, (size_t)(nl - p));
        term_write("\r\n", 2);
        p = nl + 1;
    }
    term_flush();
}

void term_printf(const char *fmt, ...)
{
    char buf[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    term_puts(buf);
}

/* ------------------------------------------------------------------ */
/* 初始化 / 挂起 / 恢复 */

static int alloc_grid(void)
{
    g_grid = calloc((size_t)g_cols * (size_t)g_rows, sizeof(struct cell));
    g_alt = calloc((size_t)g_cols * (size_t)g_rows, sizeof(struct cell));
    g_dirty = calloc((size_t)g_rows, 1);
    return (g_grid && g_alt && g_dirty) ? 0 : -1;
}

int term_init(void)
{
    palette_init();
    const char *mirror = cmdline_get("realios.mirror");
    if (mirror && mirror[0]) {
        char path[64];
        snprintf(path, sizeof(path), "/dev/%s", mirror);
        g_ser_fd = open(path, O_RDWR | O_NOCTTY | O_CLOEXEC | O_NONBLOCK);
        if (g_ser_fd >= 0) {
            int fl = fcntl(g_ser_fd, F_GETFL);
            fcntl(g_ser_fd, F_SETFL, fl & ~O_NONBLOCK);
            raw_mode(g_ser_fd, &g_ser_saved, &g_ser_saved_ok);
        }
    }

    int want_fb = !cmdline_flag("realios.nofb");
    if (want_fb && font_load(BUILTIN_FONT) == 0 && fb_open() == 0) {
        const char *sc = cmdline_get("realios.fontscale");
        g_scale = sc ? atoi(sc) : (g_xres >= 2400 ? 2 : 1);
        if (g_scale < 1) g_scale = 1;
        if (g_scale > 4) g_scale = 4;
        g_cw = 8 * g_scale;
        g_ch = 16 * g_scale;
        g_cols = g_xres / g_cw;
        g_rows = g_yres / g_ch;
        if (alloc_grid() < 0) {
            munmap(g_fb, g_fb_len);
            g_fb = NULL;
            klog("display: alloc_grid failed");
        }
    } else if (want_fb) {
        klog("display: framebuffer unavailable (font or /dev/fb0)");
    }

    if (g_fb) {
        g_vt_fd = open("/dev/tty1", O_RDWR | O_NOCTTY | O_CLOEXEC);
        g_kbd_fd = g_vt_fd;
        if (g_vt_fd >= 0) {
            ioctl(g_vt_fd, VT_ACTIVATE, 1);
            ioctl(g_vt_fd, KDSETMODE, KD_GRAPHICS);
            raw_mode(g_kbd_fd, &g_kbd_saved, &g_kbd_saved_ok);
        }
        reset_state();
        term_clear();
        klog("display: framebuffer %dx%d %dbpp, grid %dx%d scale %d", g_xres, g_yres, g_bpp,
             g_cols, g_rows, g_scale);
    } else {
        g_out_fd = open("/dev/console", O_RDWR | O_NOCTTY | O_CLOEXEC);
        g_kbd_fd = g_out_fd;
        if (g_kbd_fd >= 0)
            raw_mode(g_kbd_fd, &g_kbd_saved, &g_kbd_saved_ok);
        struct winsize ws;
        if (g_out_fd >= 0 && ioctl(g_out_fd, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) {
            g_cols = ws.ws_col;
            g_rows = ws.ws_row;
        }
        klog("display: no framebuffer, text console %dx%d", g_cols, g_rows);
    }
    return 0;
}

void term_suspend(void)
{
    if (g_vt_fd >= 0)
        ioctl(g_vt_fd, KDSETMODE, KD_TEXT);
    if (g_kbd_saved_ok && g_kbd_fd >= 0)
        tcsetattr(g_kbd_fd, TCSANOW, &g_kbd_saved);
}

void term_resume(void)
{
    if (g_vt_fd >= 0) {
        ioctl(g_vt_fd, VT_ACTIVATE, 1);
        ioctl(g_vt_fd, KDSETMODE, KD_GRAPHICS);
    }
    if (g_kbd_fd >= 0)
        raw_mode(g_kbd_fd, &g_kbd_saved, &g_kbd_saved_ok);
    mark_all();
    term_flush();
}

/* ------------------------------------------------------------------ */
/* 键盘 */

static int input_fds(struct pollfd *p)
{
    int n = 0;
    if (g_kbd_fd >= 0) { p[n].fd = g_kbd_fd; p[n].events = POLLIN; n++; }
    if (g_ser_fd >= 0 && g_ser_fd != g_kbd_fd) { p[n].fd = g_ser_fd; p[n].events = POLLIN; n++; }
    return n;
}

static uint8_t g_kq[64];
static int g_kq_n;

static int kq_fill(int timeout_ms)
{
    struct pollfd p[2];
    int n = input_fds(p);
    if (n == 0) {
        usleep((useconds_t)(timeout_ms > 0 ? timeout_ms : 100) * 1000);
        return 0;
    }
    int r = poll(p, (nfds_t)n, timeout_ms);
    if (r <= 0)
        return 0;
    for (int i = 0; i < n; i++) {
        if (!(p[i].revents & POLLIN))
            continue;
        ssize_t got = read(p[i].fd, g_kq + g_kq_n, sizeof(g_kq) - (size_t)g_kq_n);
        if (got > 0)
            g_kq_n += (int)got;
    }
    return g_kq_n;
}

static int kq_pop(void)
{
    if (g_kq_n == 0)
        return -1;
    int b = g_kq[0];
    memmove(g_kq, g_kq + 1, (size_t)--g_kq_n);
    return b;
}

int term_read_key(int timeout_ms)
{
    if (g_kq_n == 0 && kq_fill(timeout_ms) == 0)
        return KEY_NONE;
    int b = kq_pop();
    if (b == '\r' || b == '\n') return KEY_ENTER;
    if (b == 0x7f || b == 0x08) return KEY_BACKSPACE;
    if (b == '\t') return KEY_TAB;
    if (b != 0x1b) return b;
    if (g_kq_n == 0)
        kq_fill(40);
    if (g_kq_n == 0)
        return KEY_ESC;
    int b1 = kq_pop();
    if (b1 != '[' && b1 != 'O')
        return KEY_ESC;
    if (g_kq_n == 0)
        kq_fill(40);
    int b2 = kq_pop();
    switch (b2) {
    case 'A': return KEY_UP;
    case 'B': return KEY_DOWN;
    case 'C': return KEY_RIGHT;
    case 'D': return KEY_LEFT;
    case 'H': return KEY_HOME;
    case 'F': return KEY_END;
    default: break;
    }
    if (b2 >= '0' && b2 <= '9') {
        int num = b2 - '0';
        for (;;) {
            if (g_kq_n == 0)
                kq_fill(40);
            int c = kq_pop();
            if (c < 0) return KEY_ESC;
            if (c >= '0' && c <= '9') { num = num * 10 + (c - '0'); continue; }
            if (c == '~') {
                switch (num) {
                case 1: case 7: return KEY_HOME;
                case 4: case 8: return KEY_END;
                case 3: return KEY_DEL;
                case 5: return KEY_PGUP;
                case 6: return KEY_PGDN;
                case 24: return KEY_F12;
                default: return KEY_NONE;
                }
            }
            return KEY_NONE;
        }
    }
    if (b2 == '[') {   /* Linux 控制台 F1-F5: ESC [ [ A..E */
        kq_pop();
        return KEY_NONE;
    }
    return KEY_NONE;
}

int term_readline(char *buf, size_t cap, int secret)
{
    size_t n = 0;
    buf[0] = 0;
    for (;;) {
        int k = term_read_key(-1);
        if (k == KEY_ENTER) {
            term_puts("\n");
            return (int)n;
        }
        if (k == KEY_ESC)
            return -1;
        if (k == KEY_BACKSPACE) {
            if (n == 0)
                continue;
            do {
                n--;
            } while (n > 0 && ((uint8_t)buf[n] & 0xC0) == 0x80);
            buf[n] = 0;
            term_puts("\b \b");
            continue;
        }
        if (k >= 0x20 && k < 0x100 && n + 1 < cap) {
            buf[n++] = (char)k;
            buf[n] = 0;
            if (!secret || (k & 0xC0) != 0x80) {
                char e = secret ? '*' : (char)k;
                term_write(&e, 1);
            }
            term_flush();
        }
    }
}

/* ------------------------------------------------------------------ */
/* 运行子进程 */

/* ext4 上可以没有 /proc /dev：挂到 chroot 里，目录建在内存 tmpfs 上，不往用户盘落目录。
 * 布局：tmpfs 新根 ← bind 用户盘的每个顶层项（现在只有 /trans）← 再挂 proc/sys/dev/tmp。 */
static int prepare_chroot(const char *root)
{
    static const char merged[] = "/run/merged";
    mkdir("/run", 0755);
    if (mount("tmpfs", "/run", "tmpfs", 0, "mode=0755") < 0 && errno != EBUSY)
        return -1;
    mkdir(merged, 0755);

    DIR *d = opendir(root);
    if (d) {
        struct dirent *de;
        while ((de = readdir(d))) {
            if (de->d_name[0] == '.' &&
                (de->d_name[1] == 0 || (de->d_name[1] == '.' && de->d_name[2] == 0)))
                continue;
            char src[512], dst[512];
            snprintf(src, sizeof(src), "%s/%s", root, de->d_name);
            snprintf(dst, sizeof(dst), "%s/%s", merged, de->d_name);
            struct stat st;
            if (lstat(src, &st) < 0)
                continue;
            if (S_ISDIR(st.st_mode)) {
                mkdir(dst, 0755);
                if (mount(src, dst, NULL, MS_BIND | MS_REC, NULL) < 0)
                    dprintf(2, "realinit: bind %s: %s\r\n", src, strerror(errno));
            } else if (S_ISLNK(st.st_mode) || S_ISREG(st.st_mode)) {
                int fd = open(dst, O_CREAT | O_WRONLY | O_CLOEXEC, 0644);
                if (fd >= 0)
                    close(fd);
                if (mount(src, dst, NULL, MS_BIND, NULL) < 0)
                    dprintf(2, "realinit: bind %s: %s\r\n", src, strerror(errno));
            }
        }
        closedir(d);
    }

    /* /trans/usr /trans/lib /trans/etc 只存在于盘上的 /trans 里；
     * 绑到 chroot 的 /usr /lib /etc，gcc 才能找到头文件和动态链接器。不往用户盘根上建目录。 */
    {
        static const char *pairs[][2] = {
            {TRANS_DIR "/usr", "/run/merged/usr"},
            {TRANS_DIR "/lib", "/run/merged/lib"},
            {TRANS_DIR "/etc", "/run/merged/etc"},
            {TRANS_BINDIR, "/run/merged/bin"},
        };
        for (size_t i = 0; i < sizeof(pairs) / sizeof(pairs[0]); i++) {
            char src[512];
            snprintf(src, sizeof(src), "%s%s", root, pairs[i][0]);
            if (!path_exists(src))
                continue;
            mkdir(pairs[i][1], 0755);
            if (mount(src, pairs[i][1], NULL, MS_BIND | MS_REC, NULL) < 0)
                dprintf(2, "realinit: bind %s: %s\r\n", src, strerror(errno));
        }
    }

    mkdir("/run/merged/proc", 0555);
    mkdir("/run/merged/sys", 0555);
    mkdir("/run/merged/dev", 0755);
    mkdir("/run/merged/tmp", 01777);
    mount("proc", "/run/merged/proc", "proc", MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL);
    mount("sysfs", "/run/merged/sys", "sysfs", MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL);
    mount("/dev", "/run/merged/dev", NULL, MS_BIND | MS_REC, NULL);
    mount("tmpfs", "/run/merged/tmp", "tmpfs", MS_NOSUID | MS_NODEV, "mode=1777");
    /* 工作区是 /：把 /trans/AGENTS.md 绑到根上，trans-cli 才能当初始上下文读到 */
    {
        char src[512], dst[512];
        snprintf(src, sizeof(src), "%s" TRANS_AGENTS, root);
        snprintf(dst, sizeof(dst), "%s/AGENTS.md", merged);
        if (path_exists(src)) {
            int fd = open(dst, O_CREAT | O_WRONLY | O_CLOEXEC, 0644);
            if (fd >= 0)
                close(fd);
            (void)!mount(src, dst, NULL, MS_BIND, NULL);
        }
    }
    if (chroot(merged) < 0)
        return -1;
    return 0;
}

static void child_common(const char *root, const char *cwd, int exe_fd,
                         char *const argv[], char *const envp[])
{
    sigset_t all;
    sigemptyset(&all);
    sigprocmask(SIG_SETMASK, &all, NULL);
    for (int s = 1; s < NSIG; s++)
        signal(s, SIG_DFL);
    if (root && root[0] && strcmp(root, "/") != 0) {
        if (prepare_chroot(root) < 0) {
            dprintf(2, "realinit: chroot %s: %s\r\n", root, strerror(errno));
            _exit(126);
        }
    }
    if (chdir(cwd && cwd[0] ? cwd : "/") < 0)
        (void)!chdir("/");
    if (exe_fd >= 0)
        fexecve(exe_fd, argv, envp);
    else
        execve(argv[0], argv, envp);
    dprintf(2, "\r\nrealinit: 无法执行 %s: %s\r\n", argv[0], strerror(errno));
    _exit(127);
}

static int wait_child(pid_t pid, int *status, int block)
{
    for (;;) {
        int st;
        pid_t r = waitpid(-1, &st, block ? 0 : WNOHANG);
        if (r == pid) {
            *status = st;
            return 1;
        }
        if (r > 0)
            continue;          /* 孤儿进程，PID 1 顺手收尸 */
        if (r < 0 && errno == EINTR)
            continue;
        return 0;
    }
}

static int decode_status(int st)
{
    if (WIFEXITED(st))
        return WEXITSTATUS(st);
    if (WIFSIGNALED(st))
        return 128 + WTERMSIG(st);
    return 255;
}

static void kill_tree(pid_t pid)
{
    kill(-pid, SIGTERM);
    kill(pid, SIGTERM);
    uint64_t t0 = now_ms();
    int st;
    while (now_ms() - t0 < 1500) {
        if (wait_child(pid, &st, 0))
            return;
        usleep(50000);
    }
    kill(-pid, SIGKILL);
    kill(pid, SIGKILL);
    wait_child(pid, &st, 1);
}

int term_run_pty(const char *root, const char *cwd, int exe_fd,
                 char *const argv[], char *const envp[])
{
    int master = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (master < 0 || grantpt(master) < 0 || unlockpt(master) < 0) {
        term_printf("\x1b[31mrealinit: 无法分配伪终端: %s\x1b[0m\n", strerror(errno));
        if (master >= 0) close(master);
        return 255;
    }
    char sname[64];
    if (ptsname_r(master, sname, sizeof(sname)) != 0) {
        close(master);
        return 255;
    }
    struct winsize ws = {.ws_row = (unsigned short)g_rows, .ws_col = (unsigned short)g_cols,
                         .ws_xpixel = (unsigned short)(g_cols * g_cw),
                         .ws_ypixel = (unsigned short)(g_rows * g_ch)};
    ioctl(master, TIOCSWINSZ, &ws);

    pid_t pid = fork();
    if (pid < 0) {
        close(master);
        return 255;
    }
    if (pid == 0) {
        setsid();
        int s = open(sname, O_RDWR);
        if (s < 0)
            _exit(125);
        ioctl(s, TIOCSCTTY, 0);
        dup2(s, 0);
        dup2(s, 1);
        dup2(s, 2);
        if (s > 2)
            close(s);
        child_common(root, cwd, exe_fd, argv, envp);
    }

    fcntl(master, F_SETFL, fcntl(master, F_GETFL) | O_NONBLOCK);
    g_reply_fd = master;
    static const char f12[] = "\x1b[24~";
    int status = 0, done = 0, hot = 0, reaped = 0;
    uint64_t exited_at = 0;
    char buf[65536];

    while (!done) {
        struct pollfd p[3];
        int n = input_fds(p);
        p[n].fd = master;
        p[n].events = POLLIN;
        int r = poll(p, (nfds_t)n + 1, exited_at ? 50 : 1000);
        if (r == 0 && g_idle_hook && !exited_at)
            g_idle_hook();
        for (int i = 0; i < n; i++) {
            if (!(p[i].revents & POLLIN))
                continue;
            ssize_t got = read(p[i].fd, buf, sizeof(buf));
            if (got <= 0)
                continue;
            if (memmem(buf, (size_t)got, f12, sizeof(f12) - 1)) {
                hot = 1;
                break;
            }
            /* 回车在原始键盘上是 \\r；Go 读进 API Key 时若带着 \\r，HTTP 头会直接报废 */
            for (ssize_t j = 0; j < got; j++) {
                char ch = buf[j] == '\r' ? '\n' : buf[j];
                (void)!write(master, &ch, 1);
            }
        }
        if (hot)
            break;
        if (p[n].revents & (POLLIN | POLLHUP | POLLERR)) {
            int any = 0;
            for (;;) {
                ssize_t got = read(master, buf, sizeof(buf));
                if (got > 0) {
                    term_write(buf, (size_t)got);
                    any = 1;
                    continue;
                }
                if (got < 0 && errno == EAGAIN)
                    break;
                if (!exited_at)
                    exited_at = now_ms();
                break;
            }
            if (any)
                term_flush();
        }
        if (!reaped && wait_child(pid, &status, 0)) {
            reaped = 1;         /* 进程已收，再排空一下 pty 里剩余的输出 */
            if (!exited_at)
                exited_at = now_ms();
        }
        if (exited_at && now_ms() - exited_at > 200)
            done = 1;
    }

    if (hot) {
        term_puts("\r\n\x1b[0m\x1b[33m[realinit] F12：正在结束程序并返回菜单…\x1b[0m\n");
        if (!reaped)
            kill_tree(pid);
        status = 0;
    } else {
        kill(-pid, SIGHUP);
        if (!reaped)
            wait_child(pid, &status, 1);
    }
    g_reply_fd = -1;
    close(master);
    term_flush();
    return hot ? RUN_HOTKEY : decode_status(status);
}

int term_run_direct(const char *root, const char *cwd, int exe_fd,
                    char *const argv[], char *const envp[])
{
    term_suspend();
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        int t = open(g_fb ? "/dev/tty1" : "/dev/console", O_RDWR);
        if (t >= 0) {
            ioctl(t, TIOCSCTTY, 1);
            dup2(t, 0);
            dup2(t, 1);
            dup2(t, 2);
            if (t > 2)
                close(t);
        }
        child_common(root, cwd, exe_fd, argv, envp);
    }
    int status = 255;
    if (pid > 0)
        wait_child(pid, &status, 1);
    term_resume();
    return pid > 0 ? decode_status(status) : 255;
}
