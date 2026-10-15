#!/usr/bin/env bash
# 静态编译 wpa_supplicant（WPA2-PSK），放进 .build_tmp 与 SYSTEM/trans/bin
set -Eeuo pipefail
ROOT="$(cd "${1:?}" && pwd)"
OUT="$ROOT/.build_tmp/wpa_supplicant"
SYS="$ROOT/SYSTEM/trans/bin/wpa_supplicant"
VER=2.10
SRC="$HOME/.realios-build/wpa_supplicant-$VER"
TARBALL="$ROOT/.build_tmp/wpa_supplicant-$VER.tar.gz"
need_tools() { command -v "$1" >/dev/null || { echo "缺少 $1" >&2; exit 1; }; }
need_tools gcc
need_tools make
if [[ -x "$OUT" && -x "$SYS" ]]; then
    echo "[wpa] 已有 $OUT"
    exit 0
fi
if [[ ! -f "$TARBALL" ]]; then
    echo "[wpa] 请先用 Windows curl 下载到 $TARBALL" >&2
    exit 1
fi
mkdir -p "$(dirname "$SRC")"
if [[ ! -f "$SRC/wpa_supplicant/Makefile" ]]; then
    tar -C "$(dirname "$SRC")" -xf "$TARBALL"
fi
cd "$SRC/wpa_supplicant"
cat > .config <<'EOF'
CONFIG_DRIVER_WEXT=y
CONFIG_TLS=internal
CONFIG_INTERNAL_LIBTOMMATH=y
CONFIG_CTRL_IFACE=y
CONFIG_WPA=y
EOF
make -s -j"$(nproc)" LDFLAGS='-static' wpa_supplicant
install -D -m 755 wpa_supplicant "$OUT"
install -D -m 755 wpa_supplicant "$SYS"
echo "[wpa] $OUT"
