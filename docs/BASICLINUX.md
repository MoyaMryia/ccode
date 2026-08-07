# BasicLinux 3.5.1 适配与测试环境

目标:让 ccode 在 **BasicLinux 3.5.1**(内核 2.2.26 / libc5 / gcc 2.7.2.3 + egcs 1.1.2 / i586 / 24MB 内存等级)上编译并运行。

本文档给出**可复现的测试路径**,供后续所有做这项适配的人使用。

## 目标环境画像(必须对照)

| 组件 | 版本 |
|---|---|
| CPU | Pentium 133 (i586,无 CMOV/MMX),24MB RAM(测试用 64MB) |
| 内核 | Linux 2.2.26 (2004,2.2 系列最终版) |
| C 库 | libc5 5.4.46(非 glibc,1998 年停产) |
| 编译器 | gcc 2.7.2.3(1995)+ egcs 1.1.2(gcc 2.95 前身) |
| 系统 | BasicLinux 3.5.1(Slackware 4.0 基座,i486 指令集) |

## 素材清单

仓库内(`.gitignore` 已排除):

| 文件 | 说明 |
|---|---|
| `blgrub2.iso` | 旧式引导 ISO(grub2 + 内核 + initrd,已废弃,保留备查) |
| `fshd.img` | 旧式 ext2 容器盘(已废弃,保留备查) |
| `zimage` | 2.2.26 内核 zImage(458KB) |
| `initfs.gz` | initrd(旧式方案用) |
| `vm/fs7.img` | 300MB ext2 根文件系统镜像(rc/inittab 已定制,见下) |
| `vm/bl3-disk.img` | **成品整盘镜像(ghost 式,480MB,一键重建)** |

用户机器原始素材:

| 素材 | 说明 |
|---|---|
| `~/BasicLinux/fs.img` | 原始根文件系统(300MB ext2,未修改版) |
| `~/BasicLinux/grub4dos-0.4.5/` | grub4dos 引导器(grldr + grldr.mbr) |
| `~/Downloads/bl3-50fd.zip` / `bl3-50.zip` | 官方软盘/光盘发行包(SWAP.ZIP 含官方 swap.img) |

## 依赖(宿主)

```sh
sudo apt install qemu-system-x86 mtools e2fsprogs xorriso   # xorriso 仅旧式 ISO 方案需要
```

## 路径 A(推荐):ghost 式整盘镜像

像"从真机拆硬盘 dd 出来"一样:一个带 MBR(GRUB4DOS)+ 三个分区的完整磁盘镜像,
QEMU `-hda` 直接启动,不依赖 ISO/initrd/GRUB 菜单。

### 磁盘布局

```
MBR: grldr.mbr(GRUB4DOS 引导,18 扇区)
p1:  FAT16  64MB  grldr + menu.lst + zimage   (grub4dos 引导文件)
p2:  ext2  355MB  BasicLinux 根文件系统(定制 fs7.img)
p3:  swap   64MB  rc 启动时 swapon /dev/hda3
```

- `menu.lst`:`kernel /zimage root=/dev/hda2 rw`(root 直接挂 hda2,无 initrd)
- 启动后 root **rw**,shell 提示符 `/<#>`,swap 自动激活(`Adding Swap: 65528k`)

### 一键重建

```sh
bash scripts/make_ghost_disk.sh          # 输出 vm/bl3-disk.img
```

脚本做的事(全部无 root):
1. 若 `vm/fs7.img` 不存在:从 `~/BasicLinux/fs.img` 复制并定制:
   - `/etc/rc`:禁用 fsck、禁用 hda1 探测(旧卡点)、增加 `swapon /dev/hda3`
   - `/etc/inittab`:`askfirst` → `respawn`(免按键自动出 shell)
2. 新建 480MB 镜像,sfdisk 写三分区表
3. p1:FAT16(mkfs.fat + mcopy 写 grldr/menu.lst/zimage)
4. p2:dd fs7.img
5. p3:mkswap
6. grldr.mbr 写 MBR(18 扇区),再重建分区表(grldr.mbr 自带分区表模板会覆盖)

### 启动与登录

```sh
qemu-system-i386 -m 64 -hda vm/bl3-disk.img -display curses
# 2 秒自动进菜单 → 自动 boot → ~10 秒到 shell /<#>(root 直进,无密码)
```

### 在 guest 里构建 ccode(一键,已跑通)

```sh
python3 scripts/guest_build.py --cc gcc-egcs-1.1.2 --targets 'ccode-cli'
```

流程:宿主 tar 源码 → debugfs 写进 fs7.img → dd 回分区 → 启动 QEMU
(gtk 窗口,肉眼可监督)→ 等 shell → 逐步执行,**每步输出与退出码都写成
guest 内文件**(`<name>.log` / `<name>.rc`)→ `sync` → 关 QEMU →
宿主机把整个根分区 dd 出来(`logs/guest/p2.img`)+ debugfs 抽出全部日志。

判定规则(**务必遵守**):
- 屏幕只是活性信号(屏停变化=guest 空闲),**结果一律以抽出的文件为准**
- 完整证据在 `logs/guest/`:`p2.img`(整个根分区,`debugfs -R 'ls /root'`
  随便翻)+ 每步的 `.log/.rc`
- 纯文本链路,不需要 OCR/多模态模型

### 与宿主交换文件

- 进 guest 前:`debugfs` 写进 `vm/fs7.img`,再 `dd` 回 `vm/bl3-disk.img` 分区 2 偏移(133120 扇区)
- guest 运行后:`logs/guest/p2.img` 就是整个根分区(guest_build 自动产出);
  手工提取用 `dd if=vm/bl3-disk.img of=/tmp/p2.img bs=512 skip=133120 count=718847`
  再 debugfs 读
- **关 QEMU 前必须 guest 内 `sync`**:`hmp quit` 不同步 guest 文件系统,
  不 sync 则本次写入全部丢失(会读到已删除 inode 的垃圾数据)

## 路径 B(旧式,保留备查):ISO + initrd + fshd.img 容器

已被路径 A 取代,仅当需要验证原版 linuxrc/loop0 流程时使用:

```sh
qemu-system-i386 -m 24 -cdrom blgrub2.iso -boot d -hda fshd.img -display curses
```

- 登录 `root`(无密码),提示符 `/<#>`;root 只读需 `mount -o remount,rw /`
- 该路径已知卡点(均已绕过):rc 的 hda1 探测挂起、fsck 死锁、24MB 下 mke2fs ramdisk 卡死
- 重建 ISO/容器盘的旧命令保留在 git 历史与本文档早期版本

## 路径 C:宿主 chroot 快速验证(迭代用,不用虚拟机)

只验证 libc5 库行为 + 现代 gcc -m32 交叉编译。**已验证:chroot 环境与真机行为一致。**

> 为什么不用 qemu-i386-static 跑老 gcc:libc5 的 malloc 与现代内核(6.x)堆布局冲突,
> 老 gcc 直接 "virtual memory exhausted"。chroot 方案用现代 gcc 编译 + 静态链接 libc5 绕开。

```sh
B=/tmp/opencode/blroot   # cp -r ~/BasicLinux/mnt_root 的副本
GCCH=/usr/lib/gcc/x86_64-linux-gnu/11/include
gcc -m32 -static -nostdlib -nostdinc \
  -Wno-builtin-declaration-mismatch -Wno-unused-parameter -Wno-format-truncation \
  -isystem $GCCH -isystem $B/usr/include \
  prog.c -o prog \
  $B/usr/lib/crt1.o $B/usr/lib/crti.o $B/usr/lib/crtbegin.o \
  $B/usr/lib/libc.a $B/usr/lib/gcc-lib/i486-linux/2.7.2.3/libgcc.a \
  $B/usr/lib/crtend.o $B/usr/lib/crtn.o
unshare -Ur chroot /tmp/opencode/blroot /bin/sh -c 'cd /tmp && ./prog'
```

## Makefile retro 模式(target matrix)

```sh
make RETRO=1 ccode-cli                    # 宿主 glibc -m32 冒烟测试(兼容层生效)
make RETRO=1 RETRO_NATIVE=1 CC=gcc-egcs-1.1.2 ccode-cli   # guest 原生 i386 工具链
```

- `RETRO=1`:`-include src/compat/compat.h`、`-Isrc/compat`(shim 头)、i586、PolarSSL 1.3.9 TLS 后端(不再强制 HTTP_ONLY;`CCODE_TLS_BACKEND` 由 `src/tls_backend.h` 按 `CCODE_RETRO/CCODE_HTTP_ONLY` 推导)
- `RETRO_NATIVE=1`:跳过 `-m32/-march/-isystem`(老 gcc 不认),过滤 `-std=c99/-Wextra/-Wpedantic/-pedantic`(gcc 2.7 无前三者,`-pedantic` 会 choke GNU 扩展 `long long`;mid-block 声明已由 `scripts/c89ify.py` 上移,源码为 C89)
- 宿主冒烟测试:`make RETRO=1 test-json test-agent ...`(glibc 下验证兼容层无回归)

## 已验证事实(适配时务必遵守)

| 事实 | 结论 / 现状 |
|---|---|
| `snprintf` 截断时 | 返回 **-1**(非 C99),NUL 结尾正常 |
| `printf` 格式 `%zu`/`%lld`/`%llu` | **不支持** → 已全部改为 `%lu`/`%ld` + 显式强转(agent.c、message.c、http.c 等) |
| mid-block 声明(函数中部 `char *x = ...`) | egcs 1.1.2 **不支持**(gcc 2.7 更不支持)→ 已用 `scripts/c89ify.py` 将 ~160 处全部上移到块顶;C89 转换后宿主全套测试全绿验证无损(当前 133 agent + 38 json + 28 http + 13 tui + 21 markdown + 5 tty + 5 e2e + 2 streaming) |
| `openat`/`fstatat`/`renameat`/`unlinkat` | 不存在 → `src/compat/compat.c` 经 `/proc/self/fd/<n>` 路径重构封装 |
| `O_CLOEXEC` | 不存在 → 自定义位 0x80000 + `fcntl(FD_CLOEXEC)` 回退 |
| `O_PATH` | 不存在 → `O_RDONLY` 回退 |
| `getaddrinfo` | 不存在 → `gethostbyname` 适配器(仅 IPv4,http.c/webfetch.c 使用) |
| `clock_gettime`/`CLOCK_MONOTONIC` | 不存在 → `gettimeofday` 回退 |
| `<stdint.h>` / `<poll.h>` | 不存在 → `src/compat/` 自带 shim 头(`-Isrc/compat` 注入) |
| `socklen_t`/`intptr_t`/`uintptr_t` | 缺失 → compat.h 补 typedef(仅 libc5,`__GLIBC__` 守卫) |
| mbedTLS 2.28 | 老编译器编不了 → retro 构建使用 vendored PolarSSL 1.3.9 提供 TLS(`CCODE_TLS_POLARSSL`;guest 构建流程含 `make -C vendor/polarssl-1.3.9/library` 步骤,见 `scripts/guest_build.py`) |
| Landlock | 无 → `ccode_landlock_apply` 优雅降级 |
| 编译器选项 | 老 gcc 只有 `-pedantic`,无 `-Wextra/-Wpedantic/-std=c99` |
| `/proc/<pid>/stat` | 2.2 内核有 ✓ |
| 根文件系统 | `mkfs.ext2` 必须 `-I 128`(2.2 内核不认 256 字节 inode) |
| swap | hda3 swap 分区 + `swapon /dev/hda3` 正常(64MB 下验证) |

## 踩坑记录

### 引导与镜像

1. **QEMU `-kernel` 直启 2.2 内核黑屏/无输出**:QEMU 6.2 的 fw_cfg 引导不兼容 2005 年 zImage(且 zImage 非 bzImage,setup 读盘失败)。**必须走完整引导流程。**
2. **grub2 BIOS 硬盘嵌入区限制 ~31KB**:core.img 里 `linux16`(107KB)+ `normal`(77KB)模块塞不下,掉 rescue shell。**改用 GRUB4DOS。**
3. **grub4dos 的 `grldr` 在 ext2 上要求连续存储**:碎片化/间接块 → `Try (hd0,0): EXT2: No grldr`。**解法:grldr 放 FAT 分区**(grldr.mbr 的 FAT 支持成熟)。
4. **grldr.mbr(18 扇区)覆盖 MBR 后分区表丢失**:先 dd grldr.mbr,再用 sfdisk 重建分区表(顺序不能反)。
5. **grub2 core.img 手装**:`grub-mkimage` 生成的 boot.img 从 LBA 1 起读**固定窗口**,core.img 超过即截断;无 root 手装 GRUB2 硬盘引导不可靠,放弃。
6. **官方 bl3 软盘版(disk1.img)与 LOADLIN**:loadlin 的 initrd 高位冲突(`initrd extends beyond end of memory`),不用。

### debugfs 操作(无 root 改镜像)

7. **`debugfs -R` 多个命令一次调用不可靠**(如 `rm` 后紧跟 `write` 不生效):**必须分开调用**。
8. **`debugfs write` 拒绝覆盖已存在文件**(`Ext2 file already exists`):**先 `rm` 再 `write`**。
9. **debugfs 写回文件丢失执行权限**(默认 0644):改 `/etc/rc` 后必须 `sif /etc/rc mode 0100755`。
10. **300MB 镜像写回慢但可用**(fshd.img 容器盘方案已废弃,现在直接 dd 分区)。
11. **guest 侧 e2fsck 死锁**:rc 里的 `e2fsck -pf` 会卡死(发 y 无效)→ rc 已禁用 fsck。

### QEMU guest 行为

12. **"boot 偶发卡死"是观测假象(已复盘,2026-08-05)**:从未卡死。2.2 内核 vgacon 用**硬件滚动**,滚屏后可见窗口离开 0xB8000+0,只读前 4KB 就永远停在 `Freeing unused kernel memory` 那行;"CPU 0%" 只是 idle shell 的正常特征(HLT+IF=1);diag 成功/guest_build 失败是因为成功判据分别在滚动点之前/之后;"strace 下成功"是安慰剂。**解法:读全部 32KB VGA 文本内存,取"最后一个非空行+其上 24 行"为可见屏;结果判定用文件不用屏幕。**
13. **`-display none` 时 screendump 早期全黑**:时序问题(dump 早于内核写 VGA),等 3-5 秒或轮询即可;文本场景用 `pmemsave 0xB8000`(整 32KB)直读 VGA 文本平面(无此问题)。注意 pmemsave 与 CRTC 寄存器读取有竞争(滚屏夹在两次操作间则窗口错位),不要用 CRTC start address 定位,用"最后非空行"启发式。
14. **`script(1)` 在 stdout=PIPE 时块缓冲**:造成"屏幕假卡死"。**直接用 `pty.openpty()` 给 QEMU。**
15. **rc 的 hda1 swap 探测挂起**(`mount -t msdos /dev/hda1` + `attempt to access beyond end of device` 后卡死):已从 rc 注释掉;改用 hda3 swap 分区。
16. **`hmp quit` 不同步 guest 文件系统**:直接 quit 后宿主机读到的是旧数据甚至已删除 inode(guest 写入全丢)。**退出前 guest 内 `sync` 并等其完成。**
17. **QMP `sendkey` 偶发丢键(尤其 Enter)**:命令整行未执行,外观像卡死。打完命令后屏幕无变化要补按 ret;自动化要加完成标记 + 重试。
18. **屏幕上的完成标记会"自匹配"**:`echo DONE_x` 的标记文本也出现在**已键入的命令行**上,屏幕搜 `DONE_x` 会在命令未执行时就命中。屏幕标记只能当活性提示,落锤靠 `.rc` 文件。
19. **guest 内 `tail -N` 别把 N 开太大**:80x25 屏幕,N=25 会把前面的完成标记/提示符全部滚出可见屏;N≤10 合适(完整内容反正走日志文件)。

### 宿主环境

16. **`pkill`/`pgrep -f` 匹配自身命令行自杀**(bash -c 的 cmdline 含匹配串,命令被自己杀掉/挂起):清理 QEMU 用 `fuser -k <镜像>` 或 `pgrep -f 'qemu-system-i38[6]'`(字符类技巧)。
17. **`/tmp` 目录系统重启后被清空**:镜像/脚本放仓库(镜像放 `vm/`,已 gitignore)。
18. **老 gcc 在现代内核上 `virtual memory exhausted`**:libc5 malloc 与现代内核堆布局不兼容(2.2.26 内核上正常,chroot 方案绕开)。

## 自动化工具(scripts/)

| 工具 | 用途 | 备注 |
|---|---|---|
| `make_ghost_disk.sh` | 一键重建整盘镜像 | 无 root,见路径 A |
| `guest_build.py` | 一键 guest 构建/测试(前台) | 每次完整 boot(~10s);结果以 `logs/guest/` 抽出文件为准 |
| `qemu_bootcheck.py` | 事件驱动 boot 监视/冒烟 | 阶段独立超时 + 现场诊断;`--disk-boot` 整盘模式;VGA 读 32KB 可见窗;banner 即成功 |
| `c89ify.py` | C89 mid-block 声明转换器 | 迭代式,处理数组/static/多声明行 |

> 旧脚本(qemu_vm/qemu_drive/qemu_vision/qmp_progress/ppm2txt)已删除:
> 屏幕抓取方式有缺陷(坑 12),有误导性。需要时从 git 历史找回。

## 当前适配状态

### 已完成

- ✅ 平台抽象层 `src/platform/`（exe 路径、escaped 检测、Landlock 沙箱封装）
- ✅ 兼容层 `src/compat/`（openat/O_CLOEXEC/O_PATH/getaddrinfo/clock_gettime 等适配）
- ✅ 源码适配（printf 格式、~160 处 C89 mid-block 声明上移）
- ✅ Makefile retro 模式（`RETRO=1`/`RETRO_NATIVE=1`）
- ✅ QEMU ghost 式整盘镜像一键重建
- ✅ guest 内 `gcc-egcs-1.1.2` 编译 ccode-cli（rc=0，`--help` 38 行输出）
- ✅ 宿主 224 测试全绿（133 agent + 38 json + 28 http + 13 tui + 21 markdown + 5 tty + 5 e2e + 2 streaming）

### 后续计划

1. guest 内跑 `gcc-2.7.2.3` 编译
2. guest 内跑测试二进制（test-json 等 `--targets`）
3. guest 内 HTTP 请求验证（需网络：QEMU user-net + guest ppp/slip，未搭）
4. 收尾：skill 清单（c89ify/QMP/debugfs 技巧打包）

## 命令速查

```sh
# 重建整盘镜像（一键）
bash scripts/make_ghost_disk.sh

# guest 构建（一键；默认 egcs 1.1.2 构建 ccode(TUI)+ccode-cli，结果看 logs/guest/）
python3 scripts/guest_build.py

# boot 诊断/冒烟（32KB VGA 可见窗，不会假卡死；banner 即成功）
python3 scripts/qemu_bootcheck.py --disk-boot --hda vm/bl3-disk.img

# 宿主 retro 冒烟
make RETRO=1 ccode-cli && make RETRO=1 test-json test-agent

# 修改 fs7.img 后同步到磁盘
dd if=vm/fs7.img of=vm/bl3-disk.img bs=512 seek=133120 conv=notrunc

# 手工提取分区 2（guest_build 已自动产出 logs/guest/p2.img）
dd if=vm/bl3-disk.img of=/tmp/p2.img bs=512 skip=133120 count=718847
debugfs -R 'ls /root' /tmp/p2.img
```

## 验收标准

1. ✅ `gcc-egcs-1.1.2` guest 内编译 ccode(HTTPS,PolarSSL 1.3.9 TLS 后端)+ `ccode-cli --help` 正常输出(`guest_build.py` rc=0,证据 `logs/guest/`);`gcc-2.7.2.3` 待跑
2. ✅ guest 内 `ccode-cli --help` 正常输出(38 行,见 `logs/guest/help.log`)
3. 宿主 `make RETRO=1 test-json test-agent test-permissions test-markdown` 冒烟全绿(兼容层无回归)
4. 宿主 `make HTTP_ONLY=1 test` 全绿(现代路径无回归)
5. 24MB-64MB 内存下 boot + TUI 可启动(swap 分区兜底)
