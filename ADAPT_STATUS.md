# BasicLinux i386 适配 — 当前情况与后续计划

分支: `retro/i386-basiclinux`
最后更新: 2026-08-05

---

## 一、已完成(代码层适配,宿主回归全绿)

### 1. 兼容层 `src/compat/`(新增文件,仅 `CCODE_RETRO` 激活)
- `compat.h` / `compat.c`:
  - `openat/fstatat/renameat/unlinkat` → 经 `/proc/self/fd/<n>` readlink 恢复目录路径再走老调用
  - `O_CLOEXEC`(内核 2.6.23+) → 自定义位 0x80000 + `fcntl(FD_CLOEXEC)` 回退
  - `O_PATH` → `O_RDONLY`;`O_NOFOLLOW`/`O_DIRECTORY` 补定义
  - `getaddrinfo/freeaddrinfo/gai_strerror` → `gethostbyname` 适配器(仅 IPv4)
  - `clock_gettime` + `CLOCK_MONOTONIC` → `gettimeofday`
  - 补 `socklen_t/intptr_t/uintptr_t`(libc5 缺;`__GLIBC__` 守卫防与 glibc typedef 冲突)
  - `_GNU_SOURCE` 前置(因 -include 先于源码自身 define)
- `poll.h`、`stdint.h` shim 头(libc5 没有,`-Isrc/compat` 注入;glibc 下转发)

### 2. 源码适配
- 全部 `%zu/%lld/%llu` → `%lu/%ld` + 显式强转(libc5 printf 不支持)
- **~160 处 C89 mid-block 声明** 用 `scripts/c89ify.py` 上移(数组/static/多声明行/
  `}else{` 括号配对,迭代收敛);宿主 224 测试全绿证明无损
- `src/cli/main.c:464` 修复 c89ify 数组初始化越界缺陷(`model[256]=""` → `model[0]='\0'`)

### 3. Makefile
- `RETRO=1`:宿主 glibc -m32 冒烟(兼容层生效)
- `RETRO_NATIVE=1`:guest 原生 i386 工具链(去 `-std=c99/-pedantic/-m32`,老 gcc 不认)
- 测试目标接入 retro 模式

### 4. QEMU 内已验证
- ghost 式整盘镜像(boot → shell → swap 激活)多次成功
- `gcc-egcs-1.1.2` 存在;编译适配尚未在 guest 内跑通(见"进行中")

## 二、进行中

### 1. ghost 式整盘镜像(路径 A,已可用)
- `scripts/make_ghost_disk.sh` 一键重建 `vm/bl3-disk.img`
- 布局:grldr.mbr + FAT16(grldr/menu/zimage)+ ext2(root,定制 rc/inittab)+ swap
- boot ~10s 到 shell `/<#>`(root 直进),swap 自动激活
- 关键历史:GRUB2 嵌入区 31KB 限制 → GRUB4DOS;grldr ext2 碎片问题 → FAT 分区

### 2. guest 内构建 ccode(已跑通,egcs 1.1.2)
- `scripts/guest_build.py` 一键流程:tar 注入 → boot → 解包 → make →
  sync → 宿主机 debugfs 抽取 build.log + 校验 /root/ccode-cli 存在
- 端到端 rc=0,guest 内生成 ccode-cli(剩余 stdint 重定义等警告,无害)
- 新增 compat 补钉:sockaddr_in6/in6_addr、fdopendir、strcasestr

## 三、阻塞点(已解决,2026-08-05 复盘)

### "boot 偶发卡死" —— 真相:从未卡死,是观测层假象
- 用户判断正确:TCG 是确定性 ISA 模拟,"偶发"本身就可疑。
- 实际每次 boot 都 ~10s 成功。所谓"卡死在 Freeing unused kernel memory"
  是屏幕读取假象:2.2 内核 vgacon 用**硬件滚动**(CRTC start address),
  可见窗口滚离 0xB8000+0 之后:
  - `pmemsave 0xB8000 前4KB` 永远显示滚动前的旧画面(恰好停在 Freeing 那行)
  - curses pty 抓取流同样不再更新
- "CPU 0%" 证据是误读:健康地停在 shell 提示符的 idle 系统就是
  HLT + IF=1 + PIC 无未决中断,与"卡死"签名完全相同。寄存器采样显示
  CPU 在 idle 循环且 EDI 在变化 = 一直在被中断唤醒、正常工作。
- diag 5/5 vs guest_build 0/2 的"相关性"也是假的:diag 的成功判据
  'init started' 出现在滚动点之前;guest_build 等的 '/<#>' 在滚动点之后。
- "strace 下反而成功" = 安慰剂效应。
- **修复(观测层,非 QEMU 参数)**:读全部 32KB VGA 文本内存,取
  "最后一个非空行及其上 24 行"为可见屏(CRTC 读数与 pmemsave 有竞争,
  不采用);屏幕等待只用于活性判断,结果一律以 debugfs 抽取的文件为准。
- 连带的真问题:hmp quit 不同步 guest fs(日志丢/读到已删除 inode);
  sendkey 偶发丢 Enter(命令未被执行为"卡死"的另一假来源)。

## 四、后续计划

1. ~~深究 boot 卡死~~ 已复盘:观测假象,非 guest 问题(见三)
2. ~~guest 内跑通 `ccode-cli` 构建~~ 已完成(egcs 1.1.2,rc=0,`--help` 38 行输出)
3. guest 内跑 `gcc-2.7.2.3` 编译(同一条 guest_build 流水线,`--cc gcc-2.7.2.3`)
4. guest 内跑测试二进制(test-json 等 `--targets`)
5. guest 内 HTTP 请求验证(需网络:QEMU user-net + guest ppp/slip,未搭)
6. 收尾:skill 清单(用户要求的 c89ify/QMP/debugfs 技巧打包)

## 五、关键文件与命令速查

```sh
# 重建整盘镜像(一键)
bash scripts/make_ghost_disk.sh

# guest 构建(一键;结果看 logs/guest/ 下的 .log/.rc 与 p2.img)
python3 scripts/guest_build.py --cc gcc-egcs-1.1.2 --targets 'ccode-cli'

# boot 诊断(32KB VGA 可见窗,不会再假卡死)
python3 scripts/qemu_diag.py --disk-boot --hda vm/bl3-disk.img

# 宿主 retro 冒烟
make RETRO=1 ccode-cli && make RETRO=1 test-json test-agent

# 修改 fs7.img 后同步到磁盘
dd if=vm/fs7.img of=vm/bl3-disk.img bs=512 seek=133120 conv=notrunc

# 手工提取分区 2(guest_build 已自动产出 logs/guest/p2.img)
dd if=vm/bl3-disk.img of=/tmp/p2.img bs=512 skip=133120 count=718847
debugfs -R 'ls /root' /tmp/p2.img
```

## 六、陷阱清单(详见 docs/BASICLINUX.md)

| 陷阱 | 解法 |
|---|---|
| debugfs 多 -R 一次调用失效 | 分开调用 |
| debugfs write 拒绝覆盖 | 先 rm 再 write |
| debugfs 写回丢执行权限 | `sif ... mode 0100755` |
| grldr ext2 碎片 "No grldr" | grldr 放 FAT 分区 |
| QEMU -kernel 不支持 zImage | GRUB4DOS/grub2 完整引导 |
| pkill -f 匹配自身自杀 | fuser -k 或 `pgrep -f 'qemu-system-i38[6]'` |
| /tmp 重启被清空 | 镜像/脚本放仓库 vm/ |
| boot 偶发卡死 | 假象:vgacon 硬件滚动后 0xB8000+0 是旧画面;读 32KB 取最后非空行窗口 |
| 屏幕与真实状态不符 | 结果判定用 debugfs 抽文件,别信屏幕;屏幕仅作活性信号 |
| pmemsave+CRTC 竞争 | 滚屏恰在两次操作间发生则窗口错位;用"最后非空行"启发式代替 |
| hmp quit 丢 guest 数据 | 退出前 guest 内 `sync` 并等提示符返回,否则读到已删除 inode |
| sendkey 丢 Enter | 命令未执行假卡死;打完命令后屏未变则补按 ret |
| tail -N 输出冲掉标记行 | N 远小于 25(如 10),否则 echo 的完成标记被滚出可见屏 |
| script(1) PIPE 块缓冲假卡死 | pty.openpty() 直跑 QEMU |
| screendump 早期全黑 | 等待 3-5s 或 pmemsave 直读 0xB8000 |
