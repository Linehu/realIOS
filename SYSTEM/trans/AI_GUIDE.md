# realIOS — 写给 AI 的系统说明

你正运行在 **realIOS** 里。这是一台几乎什么都没有的电脑，用户希望你和他一起，
在这台机器上从零造出他想要的整个操作系统。**这台机器完全属于用户**：你以 root 身份运行，
工作目录围栏是 `/`，没有任何权限限制，也没有任何"官方推荐"的发行版路线。

## 根目录下只有一个 `/trans`

ext4 的根目录一开始只有 `/trans`（加上 mkfs 自带的 `lost+found`）。没有写在盘上的 `/bin`、`/etc`、
`/usr`——这些在进程里由 realinit 从 `/trans` 绑出来（所以 `/bin/sh`、`/usr/bin/gcc` 能用），不落在用户盘根上。目录结构怎么长，完全由用户决定。

`/trans` 是 trans-cli（也就是你）的家，realinit 只认这一个目录：

| 路径 | 说明 |
|------|------|
| `/trans/trans` | trans-cli。存在且可执行时优先用它，否则用引导程序里内置的那一份 |
| `/trans/bin/sh` → `busybox` | 给 `run_shell` 用。busybox 静态版里还有 `wget`(HTTPS)、`tar`、`xz`、`gzip`、`vi`、`mkdir`、`mount` 等，`busybox --list` 查看 |
| `/trans/certs/ca-certificates.crt` | HTTPS 证书（`SSL_CERT_FILE`） |
| `/trans/config/` | trans-cli 配置（`XDG_CONFIG_HOME`），API Key 在 `trans-cli/config.json` |
| `/trans/init` | 开机菜单清单，见下文 |
| `/trans/net.conf` | 静态 IP 配置（没有就用 DHCP） |
| `/trans/AI_GUIDE.md` | 本文件 |
| `/trans/AGENTS.md` | 每次会话的初始上下文（运行时也会出现在 `/AGENTS.md`） |
| `/trans/knowledge/` | AI 的长期记忆；`index.md` 是目录 |

开机时 realinit 只会补回 `/trans` 里**缺失**的东西，绝不覆盖已存在的文件，也不会在 `/trans` 以外创建任何东西。

## 现在有什么

| 东西 | 位置 | 说明 |
|------|------|------|
| 核心 | 嵌在引导程序 `EFI/BOOT/BOOTX64.EFI` 里 | ext4、有线/USB 网卡、Wi-Fi、键盘鼠标、EFI 帧缓冲 |
| realinit | PID 1，在内置 initramfs 里 | 开机、联网、中文帧缓冲终端、读 `/trans/init` 弹菜单 |
| DNS | realinit 在 `127.0.0.1:53` 转发 | 没有 `/etc/resolv.conf` 时程序默认就会问 127.0.0.1，所以开箱能解析域名 |

**已经有的工具链**（都在 `/trans` 里，开机绑到 `/usr` `/lib` `/etc`，盘上根目录仍然只有 `/trans`）：

| 路径 | 说明 |
|------|------|
| `/usr/bin/gcc` `/usr/bin/g++` | 编 C / C++ |
| `/usr/bin/make` `/usr/bin/cmake` `/usr/bin/ninja` | 构建 |
| `/usr/include` `/usr/lib` | 头文件和库（zlib、OpenSSL、ncurses、curl、sqlite、libpng、pcre2、libxml2 等） |

**还没有的东西**：桌面、浏览器、声音、中文输入法、剪贴板、FAT32。这些等你来造或者引入。

## 真机怎么上网

联网是 **realinit 的事，不是 AI 的事**。不要给用户写 Wi-Fi 程序，也不要向他要密码。

用户可以：插网线、手机 USB 共享、USB 网卡，或在 realinit 联网界面里从扫描列表用上下键选 Wi-Fi 再自己输入密码。配置存在 `/trans/net.conf`。

## `/proc` `/sys` `/dev`

`/proc`、`/sys`、`/dev` 默认不存在，所以默认没有挂载。只要用户同意建这些目录
（`busybox mkdir /proc /sys /dev`），**下次开机** realinit 会把 proc、sysfs 和 devtmpfs 挂上去；
当场就要用的话，也可以自己 `busybox mount -t proc proc /proc` 等。
没有 `/dev` 时 `>/dev/null` 这类重定向会失败，写脚本时注意。

## 磁盘

```
GPT
  p1  FAT32 ESP   EFI/BOOT/BOOTX64.EFI（realboot + 内核 + initramfs）、realIOS/boot.cfg
  p2  ext4        uuid=7265616c-494f-5300-8000-000000000002 label=REALIOS → 挂载为 /
```

内核没有 vfat，所以 ESP 默认看不到。想更新引导程序或内核，可以写一个 FAT 驱动/工具，
或者用 `kexec` 直接从 ext4 启动你自己编译的内核（不用碰 ESP）。

## 显示与输入

- 屏幕是 realinit 自己实现的终端（UTF-8、中文双宽、xterm-256color 子集、真彩色），
  默认 16px 点阵（GNU Unifont），窗口大小通过 pty 的 `TIOCGWINSZ` 告知。
- 帧缓冲是 `/dev/fb0`，键盘鼠标是 `/dev/input/event*`（evdev），前提是有 `/dev`。
  需要直接接管屏幕和键盘的程序（比如图形桌面），在 `/trans/init` 里加上 `direct` 选项，
  realinit 会把 tty1 交还给它，它退出后再接管回来。
- 用户还没有中文输入法，可能会用英文或拼音和你交流。

## 让程序开机出现：`/trans/init`

```
# 名称 | 命令行 [| direct]
我的 Shell | /trans/bin/sh -l
我的桌面   | /usr/local/bin/desktop | direct
@timeout=5        # 菜单 5 秒后自动运行 @default 那一项
@default=2        # 0 = trans-cli，1 起为上面的程序
```

`/trans/init` 也可以直接是一个可执行文件，或者一个目录（里面每个可执行文件都是一个菜单项）。
文件为空或只有注释时，开机直接进 trans-cli。命令行通过 `/trans/bin/sh -c` 执行。

## 兜底机制（放心大胆地改）

- 菜单第一项永远是 trans-cli；内置的那一份在引导程序里，ext4 怎么折腾都删不掉它。
- **F12**：随时结束当前程序，回到菜单。
- **开机时按 S**：安全模式，忽略 `/trans/init`、忽略 `/trans/trans`，直接进内置 trans-cli。
- ext4 挂不上时，realinit 提供内存里的应急根，trans-cli 照样能跑，可以用来修盘。

## 环境变量

`HOME=/trans`、`PATH=/usr/bin:/usr/sbin:/usr/local/bin:/trans/usr/bin:/trans/bin:/bin:/sbin`、
`SSL_CERT_FILE=/trans/certs/ca-certificates.crt`、`XDG_CONFIG_HOME=/trans/config`、
`TERM=xterm-256color`、`LANG=C.UTF-8`、`REALIOS=1`、`REALIOS_ROOT=ext4|emergency`、
`TRANS_WORKSPACE=/`、`TRANS_TOOLS=on`。程序的工作目录是 `/`。

## 自举思路（仅供参考，用户说了算）

1. 用 `busybox wget` 下载一个静态工具链（例如 musl 的 native 工具链），或者 Alpine 的
   `apk.static` 把整套用户态装进 `/`；也可以完全不用现成发行版，自己一点一点写。
2. 有了编译器，什么都能造：shell、编辑器、文件管理器、输入法、图形桌面……（Wi-Fi 由用户在 realinit 里自己连，不要写无线联网程序。）
3. 每做出一个程序，就记得把它登记到 `/trans/init`。
