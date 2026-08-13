# BasicLinux 3.5.1 适配记录

目标：让 ccode 在 BasicLinux 3.5.1（内核 2.2.26 / libc5 / gcc 2.7.2.3 + egcs 1.1.2）上编译运行。这份记录给后续做同类适配的人当参考。

## 目标环境

| 组件 | 版本 |
|------|------|
| CPU | i586，24MB 内存（测试用 64MB） |
| 内核 | Linux 2.2.26（2.2 系列最后一版，2004 年） |
| C 库 | libc5 5.4.46（非 glibc，1998 年停产） |
| 编译器 | gcc 2.7.2.3 + egcs 1.1.2 |
| 系统 | BasicLinux 3.5.1（Slackware 4.0 基座） |

## 素材

仓库里（已 gitignore）：`blgrub2.iso`、`fshd.img` 是早期废弃方案，留作备查；`zimage`（内核）、`initfs.gz`（initrd）、`vm/fs7.img`（300MB 根文件系统镜像）、`vm/bl3-disk.img`（成品整盘镜像，一键重建）。

用户机器上的原始素材：`~/BasicLinux/fs.img`（原始根文件系统）、`~/BasicLinux/grub4dos-0.4.5/`、`~/Downloads/bl3-*.zip`（官方发行包）。

宿主依赖：

```sh
sudo apt install qemu-system-x86 mtools e2fsprogs
```

## 推荐路径：ghost 式整盘镜像

像从真机拆硬盘 dd 出来一样，做一块带 MBR + 三个分区的完整磁盘镜像，QEMU 用 `-hda` 直接启动，不依赖 ISO / initrd / GRUB 菜单。

磁盘布局：

```
MBR: grldr.mbr（GRUB4DOS 引导）
p1:  FAT16  64MB   grldr + menu.lst + zimage
p2:  ext2  355MB   BasicLinux 根文件系统
p3:  swap   64MB   启动时 swapon /dev/hda3
```

一键重建：

```sh
bash scripts/make_ghost_disk.sh          # 输出 vm/bl3-disk.img
```

启动：

```sh
qemu-system-i386 -m 64 -hda vm/bl3-disk.img -display curses
# 2 秒进菜单 → 自动 boot → 约 10 秒到 shell，root 直进无密码
```

在 guest 里构建（已跑通）：

```sh
python3 scripts/guest_build.py --cc gcc-egcs-1.1.2 --targets 'ccode-cli'
```

流程：宿主 tar 源码 → debugfs 写进镜像 → 启动 QEMU → 逐步执行，**每步的输出和退出码都写进 guest 内文件**（`.log` / `.rc`）→ `sync` → 关 QEMU → 宿主机把整个根分区 dd 出来（`logs/guest/p2.img`），再用 debugfs 抽日志。

判定规则（务必遵守）：

- 屏幕只是活性信号，**结果一律以抽出的文件为准**（`logs/guest/` 下的 `.log`/`.rc` 和 `p2.img`）。
- 纯文本链路，不需要 OCR 或视觉模型。
- 关 QEMU 前必须 guest 内 `sync`——`hmp quit` 不会同步 guest 文件系统，不 sync 这次写入全丢。

## retro 构建模式

```sh
make RETRO=1 ccode-cli                                    # 宿主 glibc -m32 冒烟（兼容层生效）
make RETRO=1 RETRO_NATIVE=1 CC=gcc-egcs-1.1.2 ccode-cli   # guest 原生 i386 工具链
```

- `RETRO=1`：强制包含 `src/compat/compat.h`、`-Isrc/compat`，目标 i586，TLS 后端换成 vendored PolarSSL 1.3.9（老编译器编不了 mbedTLS 2.28）。
- `RETRO_NATIVE=1`：跳过 `-m32/-march/-isystem`（老 gcc 不认），过滤 `-std=c99/-Wextra/-Wpedantic/-pedantic`（gcc 2.7 没有前三者，`-pedantic` 会呛 GNU 扩展 `long long`）。源码已是 C89（`scripts/c89ify.py` 把 mid-block 声明都上移了）。
- 宿主冒烟：`make RETRO=1 test-json test-agent test-permissions test-markdown`。
- 体积优化：宿主冒烟（现代 gcc -m32）开函数分节 + 链接期垃圾回收 + 符号裁剪；guest 原生（egcs 1.1.2 / gcc 2.7.2.3）只做 `-s` 符号裁剪——老 gcc 缺 `-fdata-sections`，libc5 静态链接配老 binutils 的 `--gc-sections` 不可靠。guest 实测产物：`ccode` 约 324K、`ccode-cli` 约 306K。

## 已验证的事实（适配时务必遵守）

| 事实 | 结论 / 现状 |
|------|------------|
| `snprintf` 截断时 | 返回 -1（不是 C99 的返回所需长度），但 NUL 结尾正常 |
| `%zu` / `%lld` / `%llu` | 不支持 → 已全改 `%lu`/`%ld` + 显式强转 |
| 函数中部声明变量 | egcs 1.1.2 不支持 → `scripts/c89ify.py` 已把约 160 处上移 |
| `openat`/`fstatat`/`renameat`/`unlinkat` | 不存在 → `src/compat/compat.c` 经 `/proc/self/fd/<n>` 重构 |
| `O_CLOEXEC` / `O_PATH` | 不存在 → 自定义位 + `fcntl` / `O_RDONLY` 回退 |
| `getaddrinfo` | 不存在 → `gethostbyname` 适配器（仅 IPv4） |
| `clock_gettime` | 不存在 → `gettimeofday` 回退 |
| `<stdint.h>` / `<poll.h>` | 不存在 → `src/compat/` 自带 shim 头 |
| mbedTLS 2.28 | 老编译器编不了 → retro 用 PolarSSL 1.3.9 |
| Landlock | 无 → 优雅降级到命令过滤 |
| `/proc/<pid>/stat` | 2.2 内核有 ✓ |
| 根文件系统 | `mkfs.ext2` 必须加 `-I 128`（2.2 内核不认 256 字节 inode） |

## 踩坑记录

### 引导与镜像

1. QEMU `-kernel` 直启 2.2 内核黑屏：QEMU 6.2 的 fw_cfg 引导不兼容 2005 年的 zImage，必须走完整引导流程。
2. grub2 BIOS 硬盘嵌入区只有约 31KB，`linux16`+`normal` 模块塞不下，掉 rescue shell → 改用 GRUB4DOS。
3. grub4dos 的 `grldr` 在 ext2 上要求连续存储，碎片化就报 `No grldr` → 把 grldr 放 FAT 分区。
4. grldr.mbr（18 扇区）写 MBR 后分区表丢失 → 先 dd grldr.mbr，再用 sfdisk 重建分区表（顺序不能反）。

### debugfs 操作（无 root 改镜像）

5. `debugfs -R` 一条命令里串多个操作不可靠（`rm` 后紧跟 `write` 不生效）→ 分开调用。
6. `debugfs write` 拒绝覆盖已存在文件 → 先 `rm` 再 `write`。
7. debugfs 写回的文件丢执行权限（默认 0644）→ 改 `/etc/rc` 后必须 `sif /etc/rc mode 0100755`。
8. guest 侧 `e2fsck -pf` 死锁 → rc 已禁用 fsck。

### QEMU guest 行为

9. "boot 偶发卡死"是观测假象（2026-08-05 复盘）：2.2 内核 vgacon 用硬件滚动，滚屏后可见窗口离开了 VGA 内存起始处，只读前 4KB 就会永远停在 `Freeing unused kernel memory` 那行。解法：读全部 32KB VGA 文本内存，取"最后一个非空行 + 其上 24 行"当可见屏；结果判定用文件，不用屏幕。
10. `-display none` 时 screendump 早期全黑：时序问题，等 3-5 秒即可；文本场景用 `pmemsave 0xB8000` 直读 VGA 文本平面。
11. `script(1)` 在 stdout 是管道时块缓冲，造成"屏幕假卡死" → 直接用 `pty.openpty()` 给 QEMU。
12. `hmp quit` 不同步 guest 文件系统 → 退出前 guest 内 `sync` 并等它完成。
13. QMP `sendkey` 偶发丢键（尤其 Enter）→ 自动化要加完成标记 + 重试。
14. 屏幕上的完成标记会"自匹配"（`echo DONE_x` 的标记也出现在已键入的命令行上）→ 屏幕标记只当活性提示，落锤靠 `.rc` 文件。
15. guest 内 `tail -N` 别把 N 开太大（80x25 屏幕，N=25 会把前面的标记滚出可见屏）。

### 宿主环境

16. `pkill`/`pgrep -f` 会匹配到自身命令行自杀 → 清 QEMU 用 `fuser -k <镜像>` 或 `pgrep -f 'qemu-system-i38[6]'`。
17. `/tmp` 重启就清空 → 镜像/脚本放仓库（镜像放 `vm/`，已 gitignore）。
18. 老 gcc 在现代内核上 `virtual memory exhausted`：libc5 malloc 与现代内核堆布局不兼容 → chroot 方案绕开。

## 自动化工具（scripts/）

| 工具 | 用途 |
|------|------|
| `make_ghost_disk.sh` | 一键重建整盘镜像（无 root） |
| `guest_build.py` | 一键 guest 构建/测试，结果以 `logs/guest/` 抽出文件为准 |
| `qemu_bootcheck.py` | 事件驱动 boot 监视/冒烟 |
| `c89ify.py` | C89 mid-block 声明转换器 |
