# BasicLinux 3.5.1 适配与测试环境

目标:让 ccode 在 **BasicLinux 3.5.1**(内核 2.2.26 / libc5 / gcc 2.7.2.3 + egcs 1.1.2 / i586 + 24MB RAM)上编译并运行。

本文档给出**可复现的测试路径**,供后续所有做这项适配的人使用。所有结论均已在真机模拟器上验证(2026-08)。

## 目标环境画像(必须对照)

| 组件 | 版本 |
|---|---|
| CPU | Pentium 133 (i586,无 CMOV/MMX),24MB RAM |
| 内核 | Linux 2.2.26 (2004,2.2 系列最终版) |
| C 库 | libc5 5.4.46(非 glibc,1998 年停产) |
| 编译器 | gcc 2.7.2.3(1995)+ egcs 1.1.2(gcc 2.95 前身) |
| 系统 | BasicLinux 3.5.1(Slackware 4.0 基座,i486 指令集) |

## 素材清单(仓库内开箱即用)

以下文件已直接放在 **ccode 仓库根目录**(gitignore 排除,见 `.gitignore`):

| 文件 | 大小 | 说明 |
|---|---|---|
| `blgrub2.iso` | 327MB | 成品引导 ISO(grub2 + 2.2.26 内核 + initrd) |
| `fshd.img` | 380MB | ext2 容器盘(含 `baslin/fs.img`,linuxrc 引导必需) |
| `zimage` | 458KB | 2.2.26 内核 zImage(重新制作 ISO 时用) |
| `initfs.gz` | 104KB | initrd(重新制作 ISO 时用) |

原始素材(仅在需要重新制作时下载):

| 素材 | 说明 | 来源 |
|---|---|---|
| `basiclinux-3.5.1-b1r5-livecd.iso`(678MB) | 官方 LiveCD | https://github.com/queenkjuul/basiclinux-lcars/releases/tag/v3.5.1-b1r5 |
| ISO 内 `baslin/fs.img`(314MB) | 根文件系统镜像 | 从 ISO 提取(7z) |
| `grub4dos-0.4.5/` | 备选引导器(grub2 方案已够用) | https://github.com/grub4dos/grub4dos |

提取命令:
```sh
7z e -y basiclinux-3.5.1-b1r5-livecd.iso "baslin/ZIMAGE" -o./bl
7z e -y basiclinux-3.5.1-b1r5-livecd.iso "baslin/INITFS.GZ" -o./bl
7z e -y basiclinux-3.5.1-b1r5-livecd.iso "baslin/fs.img" -o./bl
```

## 依赖(宿主需安装)

```sh
sudo apt install qemu-system-x86 xorriso mtools   # 测试虚拟机 + ISO 制作
```

## 路径 A:QEMU 完整系统(推荐,验收用)

完整模拟 Pentium + 2.2.26 内核 + BasicLinux,能编译、运行、验证一切。

### 快速启动(素材已在仓库根目录)

```sh
qemu-system-i386 -m 24 -cdrom blgrub2.iso -boot d -hda fshd.img -display curses
```

- grub 菜单 5 秒自动进入,约 30 秒后出现 `Login as root with no password`
- 登录:`root`(无密码),提示符 `/<#>`
- root 只读,先 `mount -o remount,rw /`(若 grub.cfg 写入了 ro)

### 重新制作引导 ISO(仅素材变更时需要)

```sh
mkdir -p isoroot/boot/grub isoroot/baslin
cp zimage isoroot/zimage
cp initfs.gz isoroot/initfs.gz
cp fshd-fs.img isoroot/baslin/fs.img   # 从 LiveCD ISO 提取的原始 fs.img
```

`isoroot/boot/grub/grub.cfg` **必须用 `linux16`/`initrd16`**(2.2 内核 boot protocol 太老,`linux` 命令报 "version too old for 32-bit boot (try with `linux16')"):

```
set timeout=5
set default=0
menuentry "BasicLinux 3.5.1" {
    linux16 /zimage root=/dev/loop0 ide-cd
    initrd16 /initfs.gz
}
```

> 不要写 `ro`(否则 root 只读,编译要手动 remount);不需要 `console=ttyS0`(VGA 输出用 curses 看即可)。

```sh
grub-mkrescue -o blgrub2.iso isoroot   # 不要加 --format=i386-pc,老 xorriso 不认
```

### 重新制作 fs.img 容器盘(仅损坏时需要)

linuxrc 会依次挂载 /dev/hda~hdd 寻找 `baslin/fs.img`。BusyBox 1.01 的 mount 自动探测**不认 iso9660**,所以必须用 ext2 容器把 fs.img 以"文件"形式提供:

```sh
dd if=/dev/zero of=fshd.img bs=1M count=380
mkfs.ext2 -F -q -I 128 fshd.img    # 必须 -I 128!2.2 内核不支持 256 字节 inode("unsupported inode size: 256")
debugfs -w -R "mkdir /baslin" fshd.img
debugfs -w -R "write fs.img /baslin/fs.img" fshd.img
```

### 无 TTY 环境下自动化交互

`-display curses` 需要终端。无 TTY 时用 `script` 分配 pty + 管道注入按键(注意 heredoc 写文件避免引号转义地狱):

```sh
( sleep 30; printf 'root\r'; sleep 3
  printf 'mount -o remount,rw /\r'; sleep 2
  printf 'cat > h.c <<XEOF\r'
  printf 'int main(){printf("hello-271\\n");return 0;}\r'
  printf 'XEOF\r'; sleep 1
  printf 'gcc-2.7.2.3 h.c -o h271 && ./h271\r'; sleep 3 ) \
  | script -qec "timeout 120 qemu-system-i386 -m 24 -cdrom blgrub2.iso -boot d -hda fshd.img -display curses" /tmp/boot.log
```

### 往 guest 传文件(免交互)

文件写进 `fshd.img` 后,guest 里路径是 `/DOS/<file>`(fshd.img 挂载点为 /DOS,root 是 fs.img):

```sh
debugfs -w -R "write ./myprog /myprog" fshd.img   # guest 里在 /DOS/myprog
```

## 路径 B:宿主 chroot 快速验证(迭代用,不用虚拟机)

只验证 libc5 库行为 + 现代 gcc -m32 交叉编译,适合改代码时快速回归。**已验证:chroot 环境与真机行为完全一致。**

> 为什么不用 qemu-i386-static 跑老 gcc:libc5 的 malloc 与现代内核(6.x)堆布局冲突,老 gcc 直接 "virtual memory exhausted"。这是**现代内核**的问题,2.2.26 内核上正常。chroot 方案用现代 gcc 编译 + 静态链接 libc5,绕开该问题。

```sh
# 1. 复制 rootfs(原目录只读):cp -r ~/BasicLinux/mnt_root /tmp/opencode/blroot
# 2. 现代 gcc -m32 + libc5 静态库交叉链接(免 sudo,免 root):
B=/tmp/opencode/blroot
GCCH=/usr/lib/gcc/x86_64-linux-gnu/11/include   # 宿主 gcc include(stddef.h 等)
gcc -m32 -static -nostdlib -nostdinc \
  -Wno-builtin-declaration-mismatch -Wno-unused-parameter -Wno-format-truncation \
  -isystem $GCCH -isystem $B/usr/include \
  prog.c -o prog \
  $B/usr/lib/crt1.o $B/usr/lib/crti.o $B/usr/lib/crtbegin.o \
  $B/usr/lib/libc.a $B/usr/lib/gcc-lib/i486-linux/2.7.2.3/libgcc.a \
  $B/usr/lib/crtend.o $B/usr/lib/crtn.o
# 3. 运行:
unshare -Ur chroot /tmp/opencode/blroot /bin/sh -c 'cd /tmp && ./prog'
```

注意:`crtbegin.o/crtend.o` 必须是 libc5 自带的(在 `$B/usr/lib/`),不能用宿主的;`libgcc.a` 用 libc5 工具链里的。

## 已验证事实(不用重复验证,但适配时务必遵守)

| 事实 | 结论 |
|---|---|
| `snprintf` 截断时 | 返回 **-1**(非 C99 "应有长度"),NUL 结尾正常 |
| `snprintf` 未截断 | 返回值正常(与 C99 一致) |
| `printf` 格式 `%zu`/`%lld`/`%llu` | **不支持**(原样输出/错位) |
| `printf` 格式 `%lu`/`%ld`/`%d` | 正常 |
| `getaddrinfo` | **不存在**,只有 `gethostbyname`(http.c/webfetch.c 需适配层) |
| `openat`/`fstatat` | 不存在(内核 2.6.16 才有;代码 22 处调用需封装) |
| `O_CLOEXEC` | 不存在(内核 2.6.23 才有;需 fallback `fcntl(FD_CLOEXEC)`) |
| `O_PATH` | 不存在(sandbox.c 已有 O_RDONLY 回退) |
| `<stdint.h>` | 不存在(需自带兼容头) |
| mbedTLS 2.28 | 老编译器编不了 → **HTTP_ONLY=1 构建** |
| Landlock | 无,`ccode_landlock_apply` 已优雅降级 ✓ |
| 编译器选项 | `-Wextra`/`-Wpedantic` 是 gcc 3.4/4.3+;老 gcc 只有 `-pedantic` |
| 代码 C99 语法依赖 | 极低(`for(int i;...)`/设计初始化器/`__func__`/VA_ARGS 均为 0 处),gcc 2.7.2.3 可编 |
| `/proc/<pid>/stat` | 2.2 内核有 ✓ |
| 内存 | 24MB 下 22700k 可用,运行流畅 ✓ |

## 踩坑记录

1. **loadlin(官方 BOOT.BAT 引导)的 initrd 高位冲突**:`initrd extends beyond end of memory`——loadlin 把 initrd 压到物理内存顶(距顶 2.5KB),2.2 内核保留顶部 128K,任何内存大小下都会冲突。**解法:改用 grub2(initrd16)加载,initrd 放低地址,无此问题。**
2. **qemu `-kernel` 直启 2.2 内核黑屏**:SeaBIOS 的 fw_cfg 引导与 2005 年内核不兼容,"Booting from ROM" 后无输出。**解法:必须走完整引导流程(grub 从 CD)。**
3. **pycdlib 手工拼 El Torito 失败**:grub 的 cdboot.S 需要自定义格式 boot info table,pycdlib 写的是标准格式,字段对不上(黑屏)。**解法:用 grub-mkrescue + xorriso 官方流程。**
4. **`mkfs.ext2` 默认 inode size 256**:2.2 内核报 `EXT2-fs: unsupported inode size: 256`,必须 `-I 128`。
5. **grub-mkrescue 不能加 `--format=i386-pc`**:该参数被透传给 xorriso 1.5.4,报 `Unrecognized option`。
6. **grub.cfg 里 `linux` 命令拒绝 2.2 内核**:报 `version too old for 32-bit boot`,必须 `linux16`。
7. **老 gcc 在现代内核上 `virtual memory exhausted`**:libc5 malloc 与现代内核堆布局不兼容,与 qemu 无关;2.2.26 内核上正常。
8. **guest 里写文件**:登录后的 `/` 是 fs.img(loop0),fshd.img 挂载在 `/DOS`;root 只读需 `mount -o remount,rw /`。
9. **自动化交互**:script 管道注入按键,heredoc 写文件(printf 的引号转义在 guest ash 里会坏)。

## 验收标准(适配完成后)

1. `gcc-2.7.2.3` 与 `gcc-egcs-1.1.2` 都能在 guest 内编译 ccode(HTTP_ONLY=1)
2. `ccode-cli -p "..."` 在 guest 内可发起 HTTP 请求(需要先配网络:`qemu ... -netdev user,id=n1 -device ne2k_isa,netdev=n1`,guest 侧 `ifconfig eth0 10.0.2.15` + 路由,下一步验证项)
3. 24MB 内存下 TUI 可启动
4. 宿主 chroot 环境回归无差异
