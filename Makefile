# realIOS — Linux / WSL 下的构建入口（Windows 用 build.bat）。
#
#   make            realinit + 字体 + 内核 + realboot + dev/release 两个镜像
#   make kernel     只编内核（含 initramfs）
#   make image      只打包（假设各产物已存在）
#   make qemu       用 WSL 里的 QEMU + OVMF 跑 dev 镜像（串口在当前终端）
#
# 依赖文件（SYSTEM/trans/{trans,bin/busybox,certs/ca-certificates.crt}）在 Windows 上由
# CODE/deps/fetch_deps.ps1 准备；纯 Linux 环境请手动放好。

ROOT    := $(abspath .)
TMP     := $(ROOT)/.build_tmp
KERNEL  := $(TMP)/vmlinuz.efi
BOOTEFI := $(ROOT)/SYSTEM/EFI/BOOT/BOOTX64.EFI
OVMF    ?= /usr/share/OVMF/OVMF_CODE_4M.fd

all: image

realinit:
	$(MAKE) -C CODE/init -f MAKEFILE

$(TMP)/font.rfnt: CODE/font/unifont-16.0.04.hex.gz CODE/font/mkfont.py
	python3 CODE/font/mkfont.py $< $@

kernel: realinit $(TMP)/font.rfnt
	bash CODE/kernel/build_kernel.sh $(ROOT) $(KERNEL)

boot: kernel
	$(MAKE) -C CODE/boot -f MAKEFILE KERNEL=$(KERNEL) OUT=$(BOOTEFI)

define pack
	mkdir -p SYSTEM/realIOS ISO
	cp build/configs/$(1)/boot.cfg SYSTEM/realIOS/boot.cfg
	sudo bash CODE/pack/pack_system_image.sh SYSTEM ISO/realios-$(1).img
endef

image: boot
	$(call pack,release)
	$(call pack,dev)

qemu:
	qemu-system-x86_64 -enable-kvm -machine q35 -m 4G -smp 2 -serial stdio \
	    -drive if=pflash,format=raw,readonly=on,file=$(OVMF) \
	    -drive if=none,id=osdisk,format=raw,file=ISO/realios-dev.img \
	    -device ide-hd,drive=osdisk,bus=ide.0,bootindex=1 \
	    -netdev user,id=net0 -device e1000,netdev=net0 -vga std

clean:
	$(MAKE) -C CODE/init -f MAKEFILE clean
	$(MAKE) -C CODE/boot -f MAKEFILE clean
	rm -f $(KERNEL) $(TMP)/font.rfnt ISO/realios-*.img

.PHONY: all realinit kernel boot image qemu clean
