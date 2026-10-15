#!/usr/bin/env bash
#
# 构建 realIOS 内核：Linux 6.6，allnoconfig + configs/realios_x86_64.config，
# initramfs（realinit + 内置 trans-cli + busybox + 证书 + 字体）直接编进内核镜像。
# 产物是一个带 EFI stub 的 bzImage，随后被 CODE/boot 的 realboot 整个嵌进 BOOTX64.EFI。
#
# 用法：bash CODE/kernel/build_kernel.sh <项目根> <输出 vmlinuz.efi>
#
# 内核源码（项目内，二选一）：
#   CODE/kernel/linux-6.6/        已解压的源码树（优先；可以直接改内核代码；
#                                 多套一层 linux-6.6/linux-6.6/ 也认）
#   CODE/kernel/linux-6.6.tar.xz  源码包
# 编译在 WSL/Linux 的本地 ext4 上进行（$REALIOS_KBUILD，默认 ~/.realios-build）：
# Windows 盘大小写不敏感且经 9p 访问很慢，源码会被同步过去再编。
set -Eeuo pipefail

ROOT="$(cd "${1:?usage: build_kernel.sh <root> <out>}" && pwd)"
OUT="${2:?usage: build_kernel.sh <root> <out>}"
KDIR="$ROOT/CODE/kernel"
FRAGMENT="$KDIR/configs/realios_x86_64.config"
WORK="${REALIOS_KBUILD:-$HOME/.realios-build}"
SRC="$WORK/linux-6.6"
OBJ="$WORK/obj"
BTMP="$ROOT/.build_tmp"
JOBS="${JOBS:-$(nproc)}"

log() { echo "[kernel] $*"; }
die() { echo "[kernel] ERROR: $*" >&2; exit 1; }

for t in make gcc flex bison bc rsync xz; do
    command -v "$t" >/dev/null || die "缺少工具 $t（apt install build-essential flex bison bc rsync xz-utils libelf-dev libssl-dev）"
done

# ---- 1. 内核源码 ----------------------------------------------------------
mkdir -p "$WORK"
TREE=""
for cand in "$KDIR/linux-6.6" "$KDIR/linux-6.6/linux-6.6"; do
    [[ -f "$cand/Makefile" && -d "$cand/arch/x86" ]] && { TREE="$cand"; break; }
done
if [[ -n "$TREE" ]]; then
    # 大小写敏感检查：Windows 上直接解压会让这对文件互相覆盖。
    # 不能用 -f（大小写不敏感的盘上两个名字都“存在”），要看目录里真实的文件名。
    nf="$TREE/include/uapi/linux/netfilter"
    if [[ -d "$nf" ]] && [[ "$(ls "$nf" | grep -ixc '^xt_connmark\.h$')" -lt 2 ]]; then
        log "警告：${TREE#$ROOT/} 是在大小写不敏感的目录里解压的（xt_CONNMARK.h/xt_connmark.h 只剩一个），改用 linux-6.6.tar.xz"
        TREE=""
    fi
fi
if [[ -n "$TREE" ]]; then
    log "源码：${TREE#$ROOT/}/（同步到 $SRC）"
    rsync -a --delete --exclude '.git' --exclude '*.o' --exclude '.*.cmd' \
        "$TREE/" "$SRC/"
    rm -f "$WORK/.src-stamp"
elif [[ -f "$KDIR/linux-6.6.tar.xz" ]]; then
    stamp="$WORK/.src-stamp"
    sum="$(sha256sum "$KDIR/linux-6.6.tar.xz" | cut -d' ' -f1)"
    if [[ ! -f "$SRC/Makefile" || "$(cat "$stamp" 2>/dev/null)" != "$sum" ]]; then
        log "源码：解压 CODE/kernel/linux-6.6.tar.xz → $SRC"
        rm -rf "$SRC"
        tar -C "$WORK" -xf "$KDIR/linux-6.6.tar.xz"
        echo "$sum" > "$stamp"
    else
        log "源码：$SRC（已与 linux-6.6.tar.xz 一致）"
    fi
else
    die "找不到内核源码：需要 CODE/kernel/linux-6.6/ 或 CODE/kernel/linux-6.6.tar.xz"
fi
if [[ -d "$KDIR/patches" ]]; then
    for p in "$KDIR"/patches/*.patch; do
        [[ -f "$p" ]] || continue
        if patch -d "$SRC" -p1 -N --dry-run -s < "$p" >/dev/null 2>&1; then
            log "补丁：$(basename "$p")"
            patch -d "$SRC" -p1 -N -s < "$p"
        fi
    done
fi

# ---- 2. initramfs（会被编进内核镜像，也就是编进引导程序）-----------------
need() { [[ -f "$1" ]] || die "缺少 $1 —— $2"; }
REALINIT="$BTMP/realinit"
FONT="$BTMP/font.rfnt"
TRANS="$ROOT/SYSTEM/trans/trans"
BUSYBOX="$ROOT/SYSTEM/trans/bin/busybox"
CERTS="$ROOT/SYSTEM/trans/certs/ca-certificates.crt"
GUIDE="$ROOT/SYSTEM/trans/AI_GUIDE.md"
AGENTS="$ROOT/SYSTEM/trans/AGENTS.md"
KREADME="$ROOT/SYSTEM/trans/knowledge/README.md"
KINDEX="$ROOT/SYSTEM/trans/knowledge/index.md"
INITTPL="$ROOT/SYSTEM/trans/init"
WPA="$BTMP/wpa_supplicant"
need "$REALINIT" "先编译 CODE/init"
need "$FONT"     "先运行 CODE/font/mkfont.py"
need "$TRANS"    "运行 CODE/deps/fetch_deps.ps1（或 make deps）下载 trans-linux-amd64"
need "$BUSYBOX"  "运行 CODE/deps/fetch_deps.ps1 获取静态 busybox"
need "$CERTS"    "运行 CODE/deps/fetch_deps.ps1 获取 CA 证书"
need "$GUIDE"    "SYSTEM/trans/AI_GUIDE.md 丢了"
need "$AGENTS"   "SYSTEM/trans/AGENTS.md 丢了"
need "$KREADME"  "SYSTEM/trans/knowledge/README.md 丢了"
need "$KINDEX"   "SYSTEM/trans/knowledge/index.md 丢了"
need "$INITTPL"  "SYSTEM/trans/init 丢了"
need "$WPA"      "先编译 wpa_supplicant（CODE/deps/build_wpa.sh）"

STAGE="$WORK/initramfs"
rm -rf "$STAGE"
mkdir -p "$STAGE"
cp "$REALINIT" "$STAGE/init"
cp "$FONT" "$STAGE/font.rfnt"
cp "$TRANS" "$STAGE/trans"
cp "$BUSYBOX" "$STAGE/busybox"
cp "$CERTS" "$STAGE/ca-certificates.crt"
cp "$GUIDE" "$STAGE/AI_GUIDE.md"
cp "$AGENTS" "$STAGE/AGENTS.md"
cp "$KREADME" "$STAGE/knowledge.README.md"
cp "$KINDEX" "$STAGE/knowledge.index.md"
cp "$INITTPL" "$STAGE/init.template"
cp "$WPA" "$STAGE/wpa_supplicant"
chmod 755 "$STAGE/init" "$STAGE/trans" "$STAGE/busybox" "$STAGE/wpa_supplicant"

LIST="$WORK/initramfs.list"
cat > "$LIST" <<EOF
dir /dev 755 0 0
nod /dev/console 600 0 0 c 5 1
nod /dev/null 666 0 0 c 1 3
nod /dev/tty1 620 0 0 c 4 1
dir /proc 555 0 0
dir /sys 555 0 0
dir /tmp 1777 0 0
dir /root 700 0 0
dir /newroot 755 0 0
dir /bin 755 0 0
dir /etc 755 0 0
dir /etc/ssl 755 0 0
dir /etc/ssl/certs 755 0 0
dir /realios 755 0 0
file /init $STAGE/init 755 0 0
file /bin/busybox $STAGE/busybox 755 0 0
slink /bin/sh busybox 777 0 0
file /etc/ssl/certs/ca-certificates.crt $STAGE/ca-certificates.crt 644 0 0
file /realios/trans $STAGE/trans 755 0 0
file /realios/wpa_supplicant $STAGE/wpa_supplicant 755 0 0
file /realios/font.rfnt $STAGE/font.rfnt 644 0 0
file /realios/AI_GUIDE.md $STAGE/AI_GUIDE.md 644 0 0
file /realios/AGENTS.md $STAGE/AGENTS.md 644 0 0
file /realios/knowledge.README.md $STAGE/knowledge.README.md 644 0 0
file /realios/knowledge.index.md $STAGE/knowledge.index.md 644 0 0
file /realios/init.template $STAGE/init.template 644 0 0
EOF
echo "127.0.0.1 localhost realios" > "$STAGE/hosts"
echo "file /etc/hosts $STAGE/hosts 644 0 0" >> "$LIST"

# ---- 3. 配置：allnoconfig + 片段，再逐行核对 -----------------------------
mkdir -p "$OBJ"
log "配置：allnoconfig + $(basename "$FRAGMENT")"
make -s -C "$SRC" O="$OBJ" ARCH=x86_64 KCONFIG_ALLCONFIG="$FRAGMENT" allnoconfig
"$SRC/scripts/config" --file "$OBJ/.config" --set-str INITRAMFS_SOURCE "$LIST"
make -s -C "$SRC" O="$OBJ" ARCH=x86_64 olddefconfig

missing=0
while IFS= read -r line; do
    line="${line%%#*}"
    line="$(echo "$line" | tr -d '[:space:]')"
    [[ -z "$line" ]] && continue
    if ! grep -qx -- "$line" "$OBJ/.config"; then
        sym="${line%%=*}"
        echo "[kernel]   未生效：$line（实际：$(grep -E "^$sym=|^# $sym is not set" "$OBJ/.config" || echo 不存在)）" >&2
        missing=$((missing + 1))
    fi
done < "$FRAGMENT"
[[ $missing -eq 0 ]] || die "$missing 个配置项没有生效（依赖没满足？），见上"
log "配置核对通过：$(grep -c '=y' "$OBJ/.config") 项 =y，$(grep -c '=m' "$OBJ/.config" || true) 项 =m"
cp "$OBJ/.config" "$BTMP/kernel.config"

# ---- 4. 编译 ----------------------------------------------------------------
log "编译 bzImage（-j$JOBS）…"
make -s -C "$SRC" O="$OBJ" ARCH=x86_64 -j"$JOBS" bzImage
mkdir -p "$(dirname "$OUT")"
cp "$OBJ/arch/x86/boot/bzImage" "$OUT"
log "完成：$OUT（$(du -h "$OUT" | cut -f1)）"
