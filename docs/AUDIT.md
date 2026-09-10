# ccode 代码审计与技术债

本文记录面向"长期可维护性"的代码审计结论。不做产品功能评估（那归 `FEATURES.md`），只记实现层面的技术债、重复、隐患和推荐的收敛方向。随代码演进持续更新，改代码前先看本文和 `AGENTS.md`。

## 一句话结论

代码写得认真、安全边界扎实（原子写、Landlock 写沙箱、会话严格校验、TLS 超时控制）。历次重构与 2026-09 压测已收敛重复实现、并行竞态、SSE 解析上限错位、Landlock 未真正生效（缺 `no_new_privs`）等问题，当前仅剩一项独立重构级技术债。

## 并发现状

**进程模型，非线程**：没有 `pthread`。同一轮的**只读子代理并行**（最多 8 个，fork + 管道回传答案），读写子代理保持串行（写目标未知，无法预分配不重叠文件范围，按 AGENTS.md 降级）。

`fork()` 的位置：

| 位置 | 用途 |
|------|------|
| `src/http.c` `resolve_with_deadline` | 真实主机名的 DNS 解析加超时，fork 子进程跑 `getaddrinfo`（数值 IP 直连不 fork） |
| `src/agent/agent_exec.c` | 执行命令，fork 出 shell / 命令子进程 |
| `src/webfetch.c` | 网页抓取 |
| `src/agent/agent.c` `run_pending_subagents` | 并行子代理：每轮最多 8 个只读子代理各 fork 一进程跑完整 agent loop，结果按 4 字节长度头 + 载荷经管道回传，父进程 poll 同时排空所有管道 |
| `src/tui/protocol.c` | 分离式 TUI 拉起 `ccode-cli` 后端 |

并行隔离靠 fork 的写时复制 + `struct agent_context` 派生拷贝（`run_subagent` 内 `sub_ctx = *ctx`），父代理的 change log / task list / 摘要缓存不会被子代理污染。

## 技术债清单

按严重程度排序。

### 1. 每请求 fork 一次做 DNS 超时

`src/http.c` `resolve_with_deadline()` 已收敛一半：数值 IP 字面量（IPv4/IPv6）直接构造 `sockaddr`，不再 fork `getaddrinfo` 子进程；只有真实主机名还需要 deadline-bounded DNS 子进程。完全去掉 fork 需要可移植的非阻塞 DNS（线程/c-ares/平台 API），属独立重构，暂缓。

## 改进方向

1. 真实主机名的 DNS 解析仍用 fork 子进程做截止时间；数值 IP 已直连。要彻底去掉 fork 需要引入线程/异步 DNS 依赖或平台特定 API，超出现阶段最小清理边界。
