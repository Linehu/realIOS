#!/usr/bin/env bash
# 把 gcc / g++ 和常用开发库装进 SYSTEM/trans/{usr,lib,etc}。
# 文件只落在 /trans 下；开机时 realinit 把 usr/lib/etc 绑到 chroot 的 /usr /lib /etc。
set -Eeuo pipefail

ROOT="$(cd "${1:?usage: install_cc.sh <项目根>}" && pwd)"
WORK="${REALIOS_KBUILD:-$HOME/.realios-build}/cc"
DEST="$WORK/prefix"
ALPINE_VER="${ALPINE_VER:-v3.21}"
MIRROR="${ALPINE_MIRROR:-https://dl-cdn.alpinelinux.org/alpine}"
STAMP="$DEST/.cc-stamp"
WANT="gcc g++ musl-dev make binutils pkgconf linux-headers file patch \
diffutils autoconf automake libtool cmake samurai \
zlib-dev openssl-dev ncurses-dev readline-dev libffi-dev \
sqlite-dev xz-dev bzip2-dev pcre2-dev libpng-dev curl-dev \
libxml2-dev gmp-dev mpfr-dev mpc1-dev \
ca-certificates"

log() { echo "[cc] $*"; }
die() { echo "[cc] ERROR: $*" >&2; exit 1; }

if [[ -f "$STAMP" && -x "$DEST/usr/bin/gcc" && -x "$DEST/usr/bin/g++" ]]; then
    log "已有 $DEST/usr/bin/gcc，跳过（删 $STAMP 可重装）"
    exit 0
fi

mkdir -p "$WORK" "$DEST"
APK="$WORK/apk.static"
if [[ ! -x "$APK" ]]; then
    idx="$ROOT/.build_tmp/APKINDEX.tar.gz"
    static_apk="$ROOT/.build_tmp/apk-tools-static.apk"
    [[ -f "$idx" && -f "$static_apk" ]] || die "缺少 $idx 或 $static_apk（先跑 fetch_deps.ps1）"
    tar -C "$WORK" -xzf "$static_apk" sbin/apk.static 2>/dev/null || tar -C "$WORK" -xzf "$static_apk"
    if [[ -f "$WORK/sbin/apk.static" ]]; then
        mv "$WORK/sbin/apk.static" "$APK"
    elif [[ -f "$WORK/apk.static" ]]; then
        mv "$WORK/apk.static" "$APK"
    else
        die "apk-tools-static.apk 里没有 apk.static"
    fi
    chmod +x "$APK"
fi

STAGING="$WORK/staging"
if [[ ! -x "$STAGING/usr/bin/gcc" ]]; then
    rm -rf "$STAGING"
    mkdir -p "$STAGING"
    "$APK" --allow-untrusted --initdb --root "$STAGING" --no-scripts \
        --repository "$MIRROR/$ALPINE_VER/main" \
        --repository "$MIRROR/$ALPINE_VER/community" \
        add $WANT
fi

rm -rf "$DEST/usr" "$DEST/lib" "$DEST/etc"
mkdir -p "$DEST"
[[ -d "$STAGING/usr" ]] || die "apk 没有装出 usr/"
[[ -d "$STAGING/lib" ]] || die "apk 没有装出 lib/"
cp -a "$STAGING/usr" "$DEST/usr"
cp -a "$STAGING/lib" "$DEST/lib"
if [[ -d "$STAGING/etc" ]]; then
    cp -a "$STAGING/etc" "$DEST/etc"
fi
# 不把 alpine 的 /bin 覆盖 /trans/bin（busybox/sh 是我们自己的）

{
    echo "alpine=$ALPINE_VER"
    echo "gcc=$("$DEST/usr/bin/gcc" -dumpfullversion 2>/dev/null || "$DEST/usr/bin/gcc" -dumpversion)"
    echo "packages=$WANT"
} > "$STAMP"

log "gcc $($DEST/usr/bin/gcc -dumpversion)  -> $DEST/usr/bin/gcc"
log "g++ $($DEST/usr/bin/g++ -dumpversion)  -> $DEST/usr/bin/g++"
du -sh "$DEST/usr" "$DEST/lib" | sed 's/^/[cc] /'
