# scripts/ — BasicLinux i386 retro 适配工具

所有开发/测试工具集中在此。Python 3 标准库即可,无第三方依赖。

保留下来的四个工具就是当前完整工作流:**建镜像 → 诊断 boot →
guest 内构建 → (需要时)C89 转换**。其它旧脚本(daemon 常驻 VM、
视觉模型看屏、旧 ISO 驱动等)已删除——它们的屏幕抓取方式有缺陷
(VGA 硬件滚动后读到旧画面),容易把人引向"boot 卡死"的错误结论,
见 docs/BASICLINUX.md 坑 12 及其复盘章节。

## 镜像构建与构建驱动(主流程,优先用这两个)

### make_ghost_disk.sh
一键重建"ghost 式"整盘镜像 `vm/bl3-disk.img`(GRUB4DOS + FAT 引导分区 +
ext2 根分区 + swap 分区,无 root):

```sh
bash scripts/make_ghost_disk.sh
```

- 若 `vm/fs7.img` 不存在会自动从 `~/BasicLinux/fs.img` 定制(rc 禁 fsck /
  加 hda3 swap;inittab 改 respawn)
- 输出:`vm/bl3-disk.img`(480MB,FAT p1 + ext2 p2 + swap p3)
- 详见 docs/BASICLINUX.md 路径 A

### guest_build.py
一键在 guest 内构建/测试 ccode(单次前台运行,每次完整 boot ~10s):

```sh
python3 scripts/guest_build.py                        # 默认 egcs 1.1.2，构建 ccode(TUI)+ccode-cli
python3 scripts/guest_build.py --cc gcc-2.7.2.3       # 换编译器
python3 scripts/guest_build.py --skip-prep ...        # 跳过源码重新注入
```

流程:tar 源码 → debugfs 注入 fs7.img → dd 回分区 → boot(gtk 窗口可
肉眼监督)→ 逐步执行,每步 `<cmd> > NAME.log 2>&1; echo $? > NAME.rc`
→ `sync` → 关 QEMU → 宿主机 dd 出整个根分区 + debugfs 抽全部日志。

**结果判定只看抽出的文件**(`logs/guest/`:`p2.img` 完整根分区 +
每步 `.log/.rc`);屏幕只作活性信号。纯文本链路,不依赖 OCR/多模态。

## QEMU guest 诊断与冒烟

### qemu_bootcheck.py(事件驱动 boot 监视/冒烟)
每个 boot 阶段独立匹配 + 独立超时,1s tick 采样 guest CPU/QMP 状态,
经 QMP `pmemsave 0xB8000`(全部 32KB,按"最后非空行"定位可见窗——
2.2 内核硬件滚动后偏移 0 是旧画面)直接读 VGA 文本,
事故时立刻输出:时间线、现场屏幕、最近行、CPU(判定 halted/spinning)、
针对性建议。定制 rc 打印 BL3 欢迎 banner("The BL3 kernel has")
即视为 boot 成功,立即以 0 退出。

```sh
python3 scripts/qemu_bootcheck.py --disk-boot --hda vm/bl3-disk.img   # 整盘模式
python3 scripts/qemu_bootcheck.py --attach /tmp/qmp.sock              # 诊断运行中 VM
```

退出码:0 到达 shell,1 阶段超时/卡住,2 QEMU 退出,3 用法错误。

已知卡点(内置建议):
- `building RW /etc ramdisk` 卡 → mke2fs /dev/ram2 内存不足 → 加 -m / lowmem / 改 rc
- `Searching for swapfile` + `attempt to access beyond end of device 03:01`
  → fshd.img 分区表过期,重建 fshd.img
- `Checking root filesystem` 交互 fsck 卡死 → 跳过 fsck 的 /etc/rc

## C89 转换工具

### c89ify.py
把 C99 mid-block 声明转成 C89 块顶声明的迭代式转换器(数组/static/
多声明行/`}else{` 括号配对,多轮迭代收敛)。

```sh
python3 scripts/c89ify.py <文件>          # 原地转换
python3 scripts/c89ify.py --check <文件>  # 只检查不修改
```

已知缺陷:数组带 `= "..."` 初始化会被转成 `name[N] = "..."`(越界写),
见 src/cli/main.c:464 的历史修复。此类初始化应整行上移或改 `name[0]='\0'`。

## 注意
- 全部假设 QMP socket 在 /tmp/qmp.sock(QEMU 参数
  `-qmp unix:/tmp/qmp.sock,server,nowait`)
- 磁盘/镜像默认路径见各脚本参数(--iso/--hda)
- 构建/驱动命令必须以 `RETRO_NATIVE=1`(guest 内)运行,见 Makefile
- 清理 QEMU 进程勿用 `pkill -f 'qemu...'`(会匹配到 bash 自身命令行,
  命令被自杀挂起);用 `fuser -k <镜像>` 或 `pgrep -f 'qemu-system-i38[6]'`
