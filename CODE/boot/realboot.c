/*
 * realboot — realIOS 的 UEFI 引导程序（EFI/BOOT/BOOTX64.EFI）。
 *
 * 内核（带 EFI stub 的 bzImage，initramfs 已编进去：realinit + 内置 trans-cli）
 * 整个嵌在本程序里。开机流程：
 *   横幅 → 读 ESP 上的 \realIOS\boot.cfg（内核命令行）与 \realIOS\apikey.txt
 *   → 等一小会儿按键（S = 安全模式，Enter = 立即启动）
 *   → LoadImage(内存里的内核) → 填 LoadOptions（命令行）→ StartImage
 *
 * boot.cfg：'#' 开头为注释；"wait=N" 设置等待秒数；其余行用空格拼成内核命令行。
 */
#include "efi.h"

#ifndef KERNEL_PATH
#error "KERNEL_PATH 未定义（MAKEFILE 传入 vmlinuz.efi 的路径）"
#endif

__asm__(".section .rdata,\"dr\"\n"
        ".balign 4096\n"
        ".globl realboot_kernel_start\n"
        "realboot_kernel_start:\n"
        ".incbin \"" KERNEL_PATH "\"\n"
        ".globl realboot_kernel_end\n"
        "realboot_kernel_end:\n"
        ".text\n");
extern const UINT8 realboot_kernel_start[], realboot_kernel_end[];

#define VERSION L"0.1.0"
#define CMDLINE_MAX 2048

static EFI_SYSTEM_TABLE *ST;
static EFI_BOOT_SERVICES *BS;

/* 编译器可能自己生成对这两个函数的调用 */
void *memset(void *d, int c, UINTN n)
{
    UINT8 *p = d;
    while (n--)
        *p++ = (UINT8)c;
    return d;
}

void *memcpy(void *d, const void *s, UINTN n)
{
    UINT8 *p = d;
    const UINT8 *q = s;
    while (n--)
        *p++ = *q++;
    return d;
}

static void color(UINTN c) { ST->ConOut->SetAttribute(ST->ConOut, c); }
static void puts16(const CHAR16 *s) { ST->ConOut->OutputString(ST->ConOut, s); }

static void put_ascii(const char *s)
{
    CHAR16 buf[128];
    while (*s) {
        UINTN n = 0;
        while (*s && n < 126) {
            if (*s == '\n')
                buf[n++] = L'\r';
            buf[n++] = (CHAR16)(UINT8)*s++;
        }
        buf[n] = 0;
        puts16(buf);
    }
}

static void put_hex(UINT64 v)
{
    CHAR16 b[19];
    b[0] = L'0';
    b[1] = L'x';
    for (int i = 0; i < 16; i++) {
        UINT8 d = (UINT8)((v >> (60 - 4 * i)) & 0xF);
        b[2 + i] = (CHAR16)(d < 10 ? L'0' + d : L'a' + d - 10);
    }
    b[18] = 0;
    puts16(b);
}

static void put_dec(UINT64 v)
{
    CHAR16 b[24];
    int i = 23;
    b[i] = 0;
    do {
        b[--i] = (CHAR16)(L'0' + v % 10);
        v /= 10;
    } while (v && i > 0);
    puts16(b + i);
}

/* 读 ESP（本程序所在卷）上的小文件；失败返回 -1 */
static int read_esp_file(EFI_HANDLE image, const CHAR16 *path, char *buf, UINTN cap)
{
    EFI_GUID lip_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
    EFI_GUID sfs_guid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    EFI_LOADED_IMAGE_PROTOCOL *li;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs;
    EFI_FILE_PROTOCOL *root, *f;
    if (BS->HandleProtocol(image, &lip_guid, (void **)&li) != EFI_SUCCESS)
        return -1;
    if (BS->HandleProtocol(li->DeviceHandle, &sfs_guid, (void **)&fs) != EFI_SUCCESS)
        return -1;
    if (fs->OpenVolume(fs, &root) != EFI_SUCCESS)
        return -1;
    int ret = -1;
    if (root->Open(root, &f, path, EFI_FILE_MODE_READ, 0) == EFI_SUCCESS) {
        UINTN n = cap - 1;
        if (f->Read(f, &n, buf) == EFI_SUCCESS) {
            buf[n] = 0;
            ret = (int)n;
        }
        f->Close(f);
    }
    root->Close(root);
    return ret;
}

static int is_space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

/* 往 ASCII 命令行末尾追加一段（自动补空格）*/
static void cmd_append(char *cmd, UINTN *len, const char *s, UINTN n)
{
    if (n == 0)
        return;
    if (*len && *len + 1 < CMDLINE_MAX)
        cmd[(*len)++] = ' ';
    for (UINTN i = 0; i < n && *len + 1 < CMDLINE_MAX; i++)
        cmd[(*len)++] = s[i];
    cmd[*len] = 0;
}

static void cmd_append_str(char *cmd, UINTN *len, const char *s)
{
    UINTN n = 0;
    while (s[n])
        n++;
    cmd_append(cmd, len, s, n);
}

static int starts_with(const char *s, const char *p)
{
    while (*p)
        if (*s++ != *p++)
            return 0;
    return 1;
}

static int parse_cfg(char *cfg, char *cmd, UINTN *len)
{
    int wait = 2;
    char *p = cfg;
    while (*p) {
        char *line = p;
        while (*p && *p != '\n')
            p++;
        char *end = p;
        if (*p)
            p++;
        while (line < end && is_space(*line))
            line++;
        while (end > line && is_space(end[-1]))
            end--;
        if (line == end || *line == '#')
            continue;
        if (starts_with(line, "wait=")) {
            int v = 0;
            for (char *q = line + 5; q < end && *q >= '0' && *q <= '9'; q++)
                v = v * 10 + (*q - '0');
            wait = v;
            continue;
        }
        cmd_append(cmd, len, line, (UINTN)(end - line));
    }
    return wait;
}

/* apikey.txt：只取第一段可见 ASCII，且只允许 Key 里可能出现的字符 */
static void add_apikey(char *key, char *cmd, UINTN *len)
{
    char *s = key;
    if ((UINT8)s[0] == 0xEF && (UINT8)s[1] == 0xBB && (UINT8)s[2] == 0xBF)
        s += 3;                                  /* 记事本的 UTF-8 BOM */
    while (*s && is_space(*s))
        s++;
    UINTN n = 0;
    while (s[n] && !is_space(s[n])) {
        char c = s[n];
        int ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                 c == '-' || c == '_' || c == '.';
        if (!ok)
            return;
        n++;
    }
    if (n < 8 || n > 200)
        return;
    char opt[240] = "realios.apikey=";
    UINTN o = 15;
    for (UINTN i = 0; i < n; i++)
        opt[o++] = s[i];
    opt[o] = 0;
    cmd_append_str(cmd, len, opt);
}

/* 0 = 正常，1 = 安全模式 */
static int wait_key(int seconds)
{
    if (seconds <= 0)
        return 0;
    for (int t = seconds * 10; t > 0; t--) {
        if (t % 10 == 0) {
            puts16(L"\r  Booting in ");
            put_dec((UINT64)(t / 10));
            puts16(L"s   [S] safe mode   [Enter] boot now   ");
        }
        EFI_INPUT_KEY k;
        if (ST->ConIn->ReadKeyStroke(ST->ConIn, &k) == EFI_SUCCESS) {
            if (k.UnicodeChar == L's' || k.UnicodeChar == L'S')
                return 1;
            if (k.UnicodeChar == L'\r' || k.UnicodeChar == L'\n')
                return 0;
        }
        BS->Stall(100000);
    }
    return 0;
}

static void banner(void)
{
    ST->ConOut->ClearScreen(ST->ConOut);
    color(EFI_LIGHTCYAN);
    put_ascii("\n"
              "                  _ ___ ___  ____\n"
              "   _ __ ___  __ _| |_ _/ _ \\/ ___|\n"
              "  | '__/ _ \\/ _` | || | | | \\___ \\\n"
              "  | | |  __/ (_| | || | |_| |___) |\n"
              "  |_|  \\___|\\__,_|_|___\\___/|____/\n\n");
    color(EFI_LIGHTGRAY);
    puts16(L"  realboot " VERSION L"  -  a computer with nothing on it, and an AI that can build everything.\r\n\r\n");
}

EFI_STATUS EFIAPI efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *st)
{
    ST = st;
    BS = st->BootServices;
    BS->SetWatchdogTimer(0, 0, 0, 0);
    ST->ConIn->Reset(ST->ConIn, 0);
    banner();

    static char cmd[CMDLINE_MAX];
    static char file[8192];
    UINTN len = 0;
    int wait = 2;
    if (read_esp_file(image, L"\\realIOS\\boot.cfg", file, sizeof(file)) >= 0)
        wait = parse_cfg(file, cmd, &len);
    if (read_esp_file(image, L"\\realIOS\\apikey.txt", file, sizeof(file)) > 0) {
        add_apikey(file, cmd, &len);
        color(EFI_DARKGRAY);
        puts16(L"  \\realIOS\\apikey.txt found - it will be handed to trans-cli.\r\n");
    }

    UINTN ksize = (UINTN)(realboot_kernel_end - realboot_kernel_start);
    color(EFI_DARKGRAY);
    puts16(L"  kernel ");
    put_dec(ksize / 1024);
    puts16(L" KiB embedded\r\n\r\n");
    color(EFI_LIGHTGRAY);

    if (wait_key(wait)) {
        cmd_append_str(cmd, &len, "realios.safe=1");
        color(EFI_YELLOW);
        puts16(L"\r\n\r\n  SAFE MODE: /trans/init and /trans/trans will be ignored.\r\n");
        color(EFI_LIGHTGRAY);
    }
    puts16(L"\r\n");

    EFI_HANDLE kh = 0;
    EFI_STATUS s = BS->LoadImage(0, image, 0, (void *)realboot_kernel_start, ksize, &kh);
    if (s != EFI_SUCCESS) {
        color(EFI_YELLOW);
        puts16(L"\r\n  LoadImage failed: ");
        put_hex(s);
        if (s == EFI_SECURITY_VIOLATION)
            puts16(L"\r\n  Secure Boot is on. Disable it in the firmware setup to run realIOS.");
        puts16(L"\r\n");
        BS->Stall(10 * 1000 * 1000);
        return s;
    }

    /* 内核 EFI stub 从 LoadOptions 读 UCS-2 命令行 */
    static CHAR16 wcmd[CMDLINE_MAX];
    for (UINTN i = 0; i <= len; i++)
        wcmd[i] = (CHAR16)(UINT8)cmd[i];
    EFI_GUID lip_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
    EFI_LOADED_IMAGE_PROTOCOL *kli;
    if (BS->HandleProtocol(kh, &lip_guid, (void **)&kli) == EFI_SUCCESS) {
        kli->LoadOptions = wcmd;
        kli->LoadOptionsSize = (UINT32)((len + 1) * sizeof(CHAR16));
    }

    s = BS->StartImage(kh, 0, 0);
    color(EFI_YELLOW);
    puts16(L"\r\n  kernel returned: ");
    put_hex(s);
    puts16(L"\r\n");
    BS->Stall(10 * 1000 * 1000);
    return s;
}
