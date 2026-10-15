# realIOS

一台几乎什么都没有的电脑，和一个能帮你把缺的都造出来的 AI。

## 建议：写入 U 盘再用

公开镜像是 **整盘磁盘镜像**（GPT：引导分区 + 用户盘），**不要只把文件复制进 U 盘**。请用写盘工具把镜像 **整盘写入** 容量 ≥ 8 GB 的 U 盘（会清空该盘原有数据）。

推荐公开文件：发行页里的 `realios-release.img`（或压缩包解压后的同名文件）。

| 系统 | 怎么写 |
| --- | --- |
| Windows | [Rufus](https://rufus.ie/)：选择 U 盘 → 选择 `realios-release.img` → 开始。或 [balenaEtcher](https://etcher.balena.io/) |
| macOS / 其他 | balenaEtcher，或确认盘符后：`dd if=realios-release.img of=/dev/磁盘 bs=4M status=progress` |

写完后用 **UEFI** 从该 U 盘启动（关闭安全启动若本机拦了第三方引导）。

开机后：先看欢迎说明，再自己联网（网线 / 手机 USB 共享，或在界面里用上下键选 Wi-Fi 并输入密码）。不要把 Wi-Fi 密码交给 AI。

`ISO/realios-dev.img` 仅供本机调试，**不要公开、不要随手发给别人**（可能带开发用钥匙）。

## 仓库里有什么

源码和构建脚本。镜像太大，放在 GitHub Releases，不进 git。

本机构建：`build.bat` 或 `build.ps1`（需要 WSL）。QEMU 试跑：`ISO\run_qemu.bat`。
