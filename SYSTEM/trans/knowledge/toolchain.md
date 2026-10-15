# 工具链

用户要求在 realIOS 上提供 gcc、g++ 和一批开发库。它们装在 `/trans/usr`、`/trans/lib`、`/trans/etc`。开机后 realinit 把它们绑到 `/usr` `/lib` `/etc`，并把 `/trans/bin` 绑到 `/bin`（于是 `/bin/sh` 存在）。只在内存里的合并根上出现，不在盘的根目录新建文件夹。

常用命令：`sh` `gcc` `g++` `make` `cmake` `pkg-config`。可以在这台机器上直接编译。头文件在 `/usr/include`，库在 `/usr/lib`。
