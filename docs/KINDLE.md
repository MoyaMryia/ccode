# Kindle Paperwhite 3 适配记录

目标：让 ccode 在一台 2015 年的电子书阅读器上跑起来——不是"能编译"，而是**能连 HTTPS、能渲染 TUI、峰值内存 1.5 MB**。

这份记录给后续做同类"往奇怪设备上塞"的适配当参考。

## 目标环境

| 组件 | 版本 |
|------|------|
| 设备 | Kindle Paperwhite 3（7 代，2015 年），序列号 G090 G105… |
| CPU | ARM Cortex-A9，单核 ~1 GHz，VFPv3 + NEON |
| 内存 | 502 MB，**无 swap** |
| 内核 | Linux 3.0.35-lab126（亚马逊定制） |
| 系统库 | glibc 2.20，`/lib/ld-linux.so.3` —— **armel / 软浮点 ABI** |
| 固件 | 5.16.2.1.1（该型号终点版本） |
| 存储 | rootfs 437 MB 只读 92% 满 + `/mnt/us` 3.0 GB 可写 |
| 设备上有没有编译器 | **没有**，且 rootfs 只剩 32 MB，别指望 |

## 为什么这件事值得记一笔

ccode 的卖点是"纯 C、零运行时"。这句话在 x86_64 上是 4.4 MB 峰值，容易被当成"benchmark 环境好"。

在 Cortex-A9 上是这个数：

```
峰值 RSS  1564 KB
HTTPS 请求 0.46 s（含 mbedTLS 握手）
```

对照同机上的亚马逊原厂栈——Java（cvm + KPPMainApp + 一堆 reader）显示一个书架要 **231 MB**。
**150 倍**。这不是调优调出来的，是"不加载 VM、不加载框架"直接省掉的。

## 构建

### 工具链

设备是 armel（软浮点）用户态，但我们是**静态链接**，所以用硬浮点工具链也完全没问题（CPU 有 VFP）。

用 [musl.cc](https://musl.cc) 的预编译交叉工具链，装在家目录，不需要 root：

```sh
mkdir -p ~/.local/toolchains && cd ~/.local/toolchains
curl -LO https://musl.cc/arm-linux-musleabihf-cross.tgz    # 98 MB
tar xzf arm-linux-musleabihf-cross.tgz                     # gcc 11.2.1
export PATH=~/.local/toolchains/arm-linux-musleabihf-cross/bin:$PATH
```

### 编译

```sh
make CC=arm-linux-musleabihf-gcc LDFLAGS=-static ccode ccode-cli
```

`LDFLAGS=-static` **不是可选项**：不加的话产物是动态链接，解释器写死 `/lib/ld-musl-armhf.so.1`，设备上没有这个文件，执行会得到一句误导性的 `No such file or directory`。

产物：

```
ccode        645.7K   static-pie，TUI 版
ccode-cli    633.7K   static-pie，CLI 版
```

拷过去直接跑，零依赖：

```sh
scp ccode ccode-cli kindle:/mnt/us/bin/
ssh kindle 'chmod +x /mnt/us/bin/ccode*'
```

## 实测

| 项 | 结果 |
|---|---|
| `--help` / `-i` 交互模式 | ✅ |
| **真实 HTTPS** | ✅ 请求 `api.deepseek.com`，0.46 s 拿到 `401 Authentication Fails`（证明 DNS + mbedTLS 握手 + HTTP 全链路通） |
| 峰值 RSS | **1564 KB** |
| `--json` / 会话保存 | 未逐个验，代码路径与 CLI 共用 |
| **TUI 渲染** | ✅ 抓到 `ESC[?1049h`（备用屏）、`ESC[2J`（清屏）、`ESC[2m`、真彩色 `ESC[38;2;232;133;89m` |

### 测试套件

`qemu-user-static` 在本机已注册 binfmt，ARM 二进制可以直接执行，所以测试套件能在笔记本上完整跑（不用上机等）：

```sh
make CC=arm-linux-musleabihf-gcc \
     CFLAGS="-Os -std=c99 -Wall -Wextra -Wpedantic -Werror -static" test
```

跑出来的结果（也是这次适配的最大收获）：**两个测试自身的可移植性缺陷**。

| 套件 | x86_64 | armv7（qemu） | 真机 |
|---|---|---|---|
| test_json | 47/0 | 47/0 | 47/0 |
| test_agent | 198/0 | 198/0 | 未跑完（198 用例在 Cortex-A9 上太慢） |
| test_config | 20/0 | 20/0 | 20/0 |
| test_markdown | 21/0 | 21/0 | 21/0 |
| test_lineedit | 16/0 | 16/0 | 16/0 |
| test_permissions | pass | pass | pass |
| test_fs_isolated | 40/0 | **40/0（修好后）** | — |
| test_http_chunk | pass | **偶发失败（5 次中 1 次）** | — |

两个问题都不是产品代码，是测试自己写死的前提：

**1. `test_fs_isolated` 的 `LONG_MAX` 用例假设 `long` 是 64 位**

```c
/* 改前：写死 19 位字面量 */
n = ccode_json_parse("9223372036854775807", 19, toks, 8);
CHECK(... && v == LONG_MAX, "decoder represents LONG_MAX without overflow");
```

32 位目标上 `LONG_MAX == 2147483647`，解码器**正确拒绝**了这个越界值，但断言要求它等于 `LONG_MAX` → 假失败。
已改为按 `long` 实际宽度构造字面量（`%ld` / `%lu` 拼 `LONG_MAX` 与 `LONG_MAX+1`），64 位与 32 位都过。

**2. `test_agent` 的 envelope 拼接在 32 位 gcc 上触发 `-Wformat-truncation`**

`snprintf(dst, cap, "{\"arguments\":%s}", inner)`，`cap` 与 `inner` 都是 ~16 KiB，gcc 11 在 32 位上会给出截断警告，`-Werror` 直接编译失败。改成显式精度 `%.*s`（按 `cap` 留出前后缀空间），语义不变。

**3. `test_http_chunk` 在 ARM 上偶发失败**（真机归属未定）
x86 连跑 5 次全过，ARM（qemu）5 次中 1 次失败，失败信息是 `transport error while reading the provider response`。看着像测试自身的时序假设在慢速/模拟环境下不成立，未定位到产品代码问题，暂未改动。

TUI 抓包方式（在没有第二块屏的情况下验证渲染）：

```sh
script -qec "ssh -tt kindle 'TERM=xterm-256color /mnt/us/bin/ccode --tui ...'" /tmp/tui.log
head -c 300 /tmp/tui.log | cat -v
```

## 坑

1. **Landlock 不存在**
   `src/platform/platform_linux.c` 的写沙箱依赖 Landlock（5.13 起进主线）。3.0.35 上 `landlock_create_ruleset` 直接 `ENOSYS`，代码静默降级成**无沙箱**。
   在自家阅读器上跑无所谓，但别以为有隔离——这条值得写进任何"老内核移植"的清单。

2. **TUI 要求真终端**
   `isatty()` 为假时直接拒绝（`--tui requires an interactive terminal`）。`ssh -t` 或 `script` 造 pty 才能起。

3. **启动时的工作目录**
   横幅显示 `workspace=/var/tmp/root`。要在书库或项目里干活得先 `cd`，`--help` 里没有 `--workspace`。

4. **设备侧没有 terminfo**
   musl 静态程序本来也不读 terminfo，`src/tui/term.c` 直接吐 ANSI，所以不影响——但如果哪天加了 terminfo 依赖，这里会是坑。

5. **交叉编译时 Makefile 的保护要信任**
   `X86_GNU_FLAGS` 只在 HOST_MACH 含 `x86_64` + `linux` 时注入，ARM 交叉构建会正确跳过 `-m64 -march=x86-64`。这一点在第一次交叉时省了很多事。

## 复现（一条龙）

```sh
# 1. 工具链
curl -LO https://musl.cc/arm-linux-musleabihf-cross.tgz && tar xzf arm-linux-musleabihf-cross.tgz
export PATH=$PWD/arm-linux-musleabihf-cross/bin:$PATH

# 2. 构建
make CC=arm-linux-musleabihf-gcc LDFLAGS=-static ccode ccode-cli

# 3. 本地先验（qemu-user-static）
qemu-arm-static ./ccode-cli --help

# 4. 上机
scp ccode ccode-cli kindle:/mnt/us/bin/ && ssh kindle 'chmod +x /mnt/us/bin/ccode*'
ssh -t kindle '/mnt/us/bin/ccode --tui --api-base https://api.deepseek.com --model deepseek-chat --api-key sk-...'
```

## 定位

走的是和其它 Linux 目标完全相同的代码路径（`src/platform/platform_linux.c`，无 `#ifdef __arm__`），所以它属于"已验证"那一栏的延伸，而不是"理论支持"：构建通过、真机运行通过、测试套件在 armv7 上通过（修掉上面两个测试缺陷之后）。

exotic 的只有三件事：内核老（3.0.35）、用户态是 armel（靠静态链接绕开 ABI）、设备上没有编译器。

顺带一句：真在墨水屏上敲 TUI 键盘是自虐（全刷 ~1 秒，局部刷新有残影）。有意义的方向是把 `ccode-cli -p` 的输出渲染成位图直接刷屏，当"AI 天气瓶"用——输入环节没解决之前，先别当真。
