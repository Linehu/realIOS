#!/usr/bin/env bash
#
# 把 SYSTEM/ 打成 realIOS 的 GPT 磁盘镜像：
#   p1  FAT32 ESP  EFI/BOOT/BOOTX64.EFI（realboot + 内核 + initramfs）、realIOS/boot.cfg
#   p2  ext4       SYSTEM/ 里其余的一切 —— 现在只有 trans/，即 ext4 上的 /trans
#
# ext4 根目录下除了 /trans 什么都不放（连 lost+found 也删掉），目录结构由用户和 AI 决定。
#
# 用法：sudo bash CODE/pack/pack_system_image.sh <SYSTEM_DIR> <OUTPUT_IMG>
set -Eeuo pipefail

if [[ $# -ne 2 ]]; then
    echo "Usage: $0 <SYSTEM_DIR> <OUTPUT_IMG>" >&2
    exit 2
fi

SYSTEM="$(cd "$1" && pwd)"
OUT="$2"
OUT_DIR="$(mkdir -p "$(dirname "$OUT")" && cd "$(dirname "$OUT")" && pwd)"
OUT="$OUT_DIR/$(basename "$OUT")"

# 必须与 CODE/init/realinit.h 的 REALIOS_EXT4_UUID / REALIOS_EXT4_LABEL 一致
EXT4_UUID="7265616c-494f-5300-8000-000000000002"
EXT4_LABEL="REALIOS"
DISK_GUID="7265616c-494f-5300-8000-000000000001"
ESP_PARTGUID="7265616c-494f-5300-8000-0000000000b1"
EXT4_PARTGUID="7265616c-494f-5300-8000-0000000000e2"
ESP_CHILDREN=(EFI realIOS)

log() { echo "[pack] $*"; }
die() { echo "[pack] ERROR: $*" >&2; exit 1; }

for tool in sfdisk mkfs.vfat mkfs.ext4 rsync losetup du; do
    command -v "$tool" >/dev/null 2>&1 || die "缺少工具 $tool"
done
[[ -f "$SYSTEM/EFI/BOOT/BOOTX64.EFI" ]] || die "SYSTEM/EFI/BOOT/BOOTX64.EFI 不存在（先编 CODE/boot）"
[[ -x "$SYSTEM/trans/trans" || -f "$SYSTEM/trans/trans" ]] || die "SYSTEM/trans/trans 不存在（先运行 CODE/deps/fetch_deps.ps1）"

bytes_of() { [[ -e "$1" ]] && du -sb --apparent-size "$1" | awk '{print $1}' || echo 0; }
align_up() { echo $(( ($1 + $2 - 1) / $2 * $2 )); }
is_esp_child() { local n; for n in "${ESP_CHILDREN[@]}"; do [[ "$1" == "$n" ]] && return 0; done; return 1; }

esp_content=0
rest_content=0
shopt -s dotglob nullglob
for child in "$SYSTEM"/*; do
    name="$(basename "$child")"
    if is_esp_child "$name"; then
        esp_content=$((esp_content + $(bytes_of "$child")))
    else
        rest_content=$((rest_content + $(bytes_of "$child")))
    fi
done
shopt -u dotglob nullglob

MIB=$((1024 * 1024))
# ESP 固定 256MiB：配合 -s 4（2KiB 簇）得到 131072 个簇，远离 FAT32 的 65525 下限与 0xFFF5 边界
esp_bytes=$((256 * MIB))
[[ $esp_content -lt $((esp_bytes - 32 * MIB)) ]] || die "ESP 内容 $((esp_content / MIB))MiB 超过 256MiB 分区"
# ext4 是用户的整个世界：至少 4GiB（镜像是稀疏文件，实际占用很小）；写到 U 盘后可自行扩容
ext4_need=$((rest_content + rest_content / 7 + 64 * MIB))
[[ $ext4_need -lt $((4096 * MIB)) ]] && ext4_need=$((4096 * MIB))
ext4_bytes="$(align_up "$ext4_need" "$MIB")"

SECTOR=512
esp_sectors=$((esp_bytes / SECTOR))
ext4_sectors=$((ext4_bytes / SECTOR))
gpt_front=2048
gpt_back=2048
total_bytes=$(( (gpt_front + esp_sectors + ext4_sectors + gpt_back) * SECTOR ))

log "SYSTEM=$SYSTEM"
log "p1 FAT32 ESP  内容 $((esp_content / 1024)) KiB / 分区 $((esp_bytes / MIB)) MiB  (${ESP_CHILDREN[*]})"
log "p2 ext4       内容 $((rest_content / 1024)) KiB / 分区 $((ext4_bytes / MIB)) MiB  uuid=$EXT4_UUID"
log "disk $((total_bytes / MIB)) MiB -> $OUT"

WORK="$(mktemp -d /var/tmp/realios_pack.XXXXXX)"
IMG="$WORK/disk.img"
MNT_ESP="$WORK/esp"
MNT_EXT="$WORK/ext4"
LOOP=""
cleanup() {
    set +e
    mountpoint -q "$MNT_ESP" && umount "$MNT_ESP"
    mountpoint -q "$MNT_EXT" && umount "$MNT_EXT"
    [[ -n "$LOOP" ]] && losetup -d "$LOOP"
    rm -rf "$WORK"
}
trap cleanup EXIT
mkdir -p "$MNT_ESP" "$MNT_EXT"
truncate -s "$total_bytes" "$IMG"

esp_start=$gpt_front
ext4_start=$((esp_start + esp_sectors))
sfdisk --no-reread -q "$IMG" <<EOF
label: gpt
label-id: ${DISK_GUID}
first-lba: ${gpt_front}
sector-size: 512

start=${esp_start}, size=${esp_sectors}, type=C12A7328-F81F-11D2-BA4B-00A0C93EC93B, uuid=${ESP_PARTGUID}, name=REALBOOT
start=${ext4_start}, size=${ext4_sectors}, type=0FC63DAF-8483-4772-8E79-3D69D8477DE4, uuid=${EXT4_PARTGUID}, name=REALIOS
EOF

LOOP="$(losetup --find --show --partscan "$IMG")"
p1="" p2=""
for _ in $(seq 1 25); do
    if [[ -b "${LOOP}p1" && -b "${LOOP}p2" ]]; then p1="${LOOP}p1"; p2="${LOOP}p2"; break; fi
    command -v partx >/dev/null && partx --update "$LOOP" 2>/dev/null || true
    sleep 0.2
done
[[ -n "$p1" && -n "$p2" ]] || die "loop 分区没有出现：$LOOP"

mkfs.vfat -F 32 -s 4 -n REALBOOT "$p1" >/dev/null
# 标准 ext4（带日志）：内核原生 ext4 读写，不需要像 Deshab 那样关 feature
mkfs.ext4 -F -q -L "$EXT4_LABEL" -U "$EXT4_UUID" -m 0 "$p2"

mount "$p1" "$MNT_ESP"
mount "$p2" "$MNT_EXT"

log "复制 ESP"
for n in "${ESP_CHILDREN[@]}"; do
    [[ -d "$SYSTEM/$n" ]] || continue
    # vfat 没有 Unix 属主/权限，不能用 -a
    rsync -rltD "$SYSTEM/$n/" "$MNT_ESP/$n/"
done

log "复制 ext4"
rmdir "$MNT_EXT/lost+found"
shopt -s dotglob nullglob
for child in "$SYSTEM"/*; do
    is_esp_child "$(basename "$child")" && continue
    rsync -rlt --chown=0:0 "$child" "$MNT_EXT/"
done
shopt -u dotglob nullglob

# /mnt/d 上的文件权限都是 777，这里按用途重设
if [[ -d "$MNT_EXT/trans" ]]; then
    T="$MNT_EXT/trans"
    find "$T" -type d -exec chmod 755 {} +
    find "$T" -type f -exec chmod 644 {} +
    chmod 755 "$T/trans"
    [[ -f "$T/bin/busybox" ]] && chmod 755 "$T/bin/busybox" && ln -sfn busybox "$T/bin/sh"
    mkdir -p "$T/config"
    chmod 700 "$T/config"
fi

log "ext4 根目录：$(ls -A "$MNT_EXT" | tr '\n' ' ')"
log "/trans：$(cd "$MNT_EXT/trans" && find . -mindepth 1 | sort | tr '\n' ' ')"

sync
umount "$MNT_ESP"
umount "$MNT_EXT"
losetup -d "$LOOP"
LOOP=""

cp --sparse=always "$IMG" "$OUT"
sync
log "完成：$OUT"
