# AuroraOS 已知限制 (v4.4.1)

## Audit Trail (v4.4.1)

The following items were reported as limitations but have been verified as fixed:

| Item | Reported | Fixed in | Verification |
|------|----------|----------|-------------|
| sysfs SMAP bug | "未修" | v4.2.7 + v4.3.3 | stac/clac wrappers in sysfs_read/write |
| ld-so not integrated | "未集成到 exec" | v4.3.3 | elfloader.c: PT_INTERP + exec_elf_interp() |
| Shell only 8 commands | "仅 8 命令" | v4.3.8+ | 59+ commands including ping/gui/ls/cat |
| USB nested comments | "编译失败" | v4.4.0 | 6 files fixed |
| seccomp "always passes" | "始终通过" | v4.3.2 | seccomp_check() in syscall entry |
| Capability "not enforced" | "未接入" | v4.3.2 | cap_check() in setuid/setgid/chown |

本文档诚实记录 AuroraOS 当前的所有已知限制、未完成功能、及架构局限。
这不是bug列表，而是对项目成熟度的诚实评估。

## 已修复 (v4.3.2)

### BSS-001 (v4.3.2) — 根因修复
- **问题**: 内核栈(32KB)在.bss中，selftest的多个1024字节栈缓冲区导致栈溢出，踩穿`current`/`idle_task`/`kernel_cr3`
- **修复**: 栈移至独立`.stack`段(64KB)，在.bss之后，加4KB guard page + 栈底部canary(0xDEAD0000BEEFCAFE)
- **影响**: 解决了BUG-CURRENT-NULL和BUG-CR3-CACHE的根因

### SEC-001 (v4.3.2) — seccomp prctl
- **问题**: BPF解释器存在但无设置接口，始终通过
- **修复**: 添加`prctl(PR_SET_SECCOMP, filter)`调用路径

### CAP-001 (v4.3.2) — Capability权限检查
- **问题**: Capability框架存在但未接入syscall
- **修复**: setuid/setgid/chown增加CAP_SETUID/CAP_SETGID/CAP_CHOWN检查

### FAT-001 (v4.3.2) — FAT32簇链验证
- **问题**: 集群链遍历无边界检查，损坏文件系统可致无限循环
- **修复**: 添加`fat32_valid_cluster()` + 4096集群链上限

### ARP-001 (v4.3.2) — ARP缓存老化
- **问题**: ARP条目永不过期
- **修复**: 10分钟超时老化机制

### USB-001 (v4.3.2) — /dev/usb节点
- **问题**: /dev/usb/目录计划中但未创建
- **修复**: 创建/dev/usb/目录及kbd0/mouse0节点

### USB-002 (v4.4.0) — USB嵌套注释Bug
- **问题**: USB驱动代码中嵌套`/* */`注释导致编译警告
- **修复**: 将嵌套注释改为`//`行注释，添加`-Werror=comment`编译选项

### XHCI-001 (v4.4.0) — xHCI 拼写错误
- **问题**: xhci驱动中多处拼写错误（如`xchci`→`xhci`）
- **修复**: 统一修正所有拼写错误

### XHCI-002 (v4.4.0) — xHCI 重复定义
- **问题**: xhci驱动中符号重复定义导致链接错误
- **修复**: 移除重复定义，统一为单一定义

### BUILD-001 (v4.4.0) — 构建改进
- **改进**: 添加 `-Werror=comment` 编译选项，将嵌套注释警告提升为错误
- **改进**: CI 流水线添加嵌套注释门禁检查，防止 `/* */` 嵌套注释合入

## 可以正常工作的功能

- x86_64 引导 (Multiboot1)
- 内核自检 (60+ 通过)
- 内存管理 (Buddy + SLAB)
- 5个基础文件系统 (ext2/fat32/ramfs/sysfs/devtmpfs) 挂载读写
- 内核Shell (必须在恢复路径运行)
- PIT + APIC 基础中断
- ACPI 关机 (SLP_TYP 回退)

## 根因级限制

### BUG-CURRENT-NULL
- **状态**: 已修复 (v4.3.2, BSS-001)
- **描述**: 内核自检后 `current` 和 `idle_task` 被 BSS 溢出清零，调度器停摆
- **根因**: 内核栈(32KB)在.bss段中，selftest多个1024字节栈缓冲区导致溢出
- **修复**: 栈移至独立.stack段(64KB)，在.bss之后，加4KB guard page + canary

### BUG-CR3-CACHE
- **状态**: 已修复 (v4.3.2, BSS-001)
- **描述**: `kernel_cr3` (.bss) 被踩穿，每次重新 walk 规避
- **根因**: 与BUG-CURRENT-NULL同源，栈溢出踩穿相邻BSS变量
- **修复**: 栈移出BSS后不再影响kernel_cr3

### BSS 段溢出
- **状态**: 已修复 (v4.3.2, BSS-001)
- **描述**: BUG-CURRENT-NULL 和 BUG-CR3-CACHE 指向同一 BSS 溢出
- **修复**: 栈完全移出BSS，source of overflow eliminated

## 架构限制

### GUI / 窗口系统
- 窗口管理 API 存在但无应用调用
- 鼠标光标无法联动（HID驱动存在，GUI未接入）
- 无任何 GUI 应用
- compositor.c/h 添加于 v4.3.8，仍为框架
- 内核 Shell 支持 59+ 个命令

### 多架构
- RISC-V64 / ARM64 / LoongArch64: 仅启动桩，kmain直接halt
- 真正可用的架构只有 x86_64

### 安全子系统
- seccomp: BPF 解释器存在，prctl 接口已接入 (v4.3.2) /* FIXED (v4.3.6) */
- Capability: 框架存在，已接入 setuid/chown 等调用点 (v4.3.2) /* FIXED (v4.3.6) */
- 模块签名: ECDSA P-256 (secp256r1) 公钥验证，已实现 (v4.2.0) /* FIXED (v4.3.6) */
- KASLR: 仅堆/栈/模块随机化，.text/.data 已随机化 (v4.3.4) /* FIXED (v4.3.6) */
- SMAP/SMEP: CPUID检测后启用，QEMU默认不支持
- Stack Protector: 工作中

### 调度器与 SMP
- CPU0 + AP 核均参与调度 (v4.3.4 SMP-001) /* FIXED (v4.3.6) */
- AP 核运行 ap_idle_loop + schedule() (v4.3.4) /* FIXED (v4.3.6) */
- 负载均衡: 任务窃取 smp_steal_task (v4.3.4) /* FIXED (v4.3.6) */
- VMPair/CFS/EVDF 代码存在，受 BUG-CURRENT-NULL 影响 (已修复 BSS-001) /* FIXED (v4.3.6) */

### 网络栈
- ARP: 有10分钟超时老化机制 (v4.3.2) /* FIXED (v4.3.6) */
- IPv4: 无分片重组、无 IP 选项
- TCP: 完整实现，含 SACK (v4.3.4 TCP-002) + NewReno 拥塞控制 (v4.3.4 TCP-003) /* FIXED (v4.3.6) */
- TCP 重传: 已接入 PIT 定时器，未经压力测试
- IPv6: 最小实现（邻居表 + 老化）
- DHCP: 有 REBIND，不完整
- DNS: 有 LRU cache/TTL，解析流程不完整
- AF_UNIX: ~800行，STREAM/DGRAM/close 有，引用计数有已知问题
- virtio_net: MMIO 驱动存在，未经实测

### 文件系统
- EXT2: 无三重间接块，大文件不安全
- FAT32: 簇链验证代码存在(fat32_valid_cluster)，未经压力测试
- sysfs: 只读，SMAP bug 已修复 (v4.2.7 stac/clac + v4.3.3 copy_from_user) /* FIXED (v4.4.1) */
- devtmpfs: /dev/usb/ 节点已创建 (v4.3.2) /* FIXED (v4.3.6) */
- Journal (WAL): 代码存在，未经崩溃测试
- fsck: 能通过自检，真实故障注入未测试
- squashfs: 代码存在，未经实测
- tmpfs: 只有 /tmp 挂载，实际使用 RamFS

### USB 子系统
- xHCI: 已合入，已修 13 个 bug (含拼写/重复定义修复 v4.4.0)
- HID 键鼠: 事件 dequeue + cycle bit 已修复
- USB 设备文件节点: /dev/usb/* 已创建 (v4.3.2)
- NVMe: BARRIER/CID/PRP 修过 6 处，未经实测

### 用户态 / ELF / 动态链接
- 用户态 Shell: 59+ 个命令（含 ping/gui/ls/cat 等）
- 动态链接器 ld-so: ~900行代码，已集成到 exec (v4.3.3, PT_INTERP + exec_elf_interp) /* FIXED (v4.4.1) */
- 自研 libc: 只有 printf/puts/malloc/strcmp 等基础函数
- 仓库只有 hello.c 一个用户态程序

### 内核模块系统
- mod load: 需要签名，签名为 ECDSA P-256 公钥验证 (v4.2.0) /* FIXED (v4.3.6) */
- mod unload: 基于引用计数的安全卸载 (v4.3.3 MOD-003) /* FIXED (v4.3.6) */
- mod_sample.c: 已接入构建系统 (v4.3.3 MOD-004) /* FIXED (v4.3.6) */

### 引导与硬件初始化
- Multiboot2: 代码自称支持，ISO 走 Multiboot1
- UEFI 启动: boot/efi_main.c 编过，ISO 未使用
- GOP framebuffer: drm_init_gop 走通，BIOS 引导不会用上
- HPET: acpi.c 能解析，未实际做定时器
- TSC 校准: 使用 PIT 轮询方式，回退值 2GHz

## 项目自承限制

| 编号 | 限制 | 状态 |
|------|------|------|
| LIM-001 | SMAP/SMEP 未启用 | QEMU不支持 |
| LIM-002 | SEEK_END 仅 RamFS 有效 | 未修复 |
| LIM-003 | EXT2 三重间接块不支持 | 未修复 |
| LIM-004 | VIRTIO_NET O(n) 扫描 | 性能差 |
| LIM-005 | 无自动化压力/网络测试框架 | 未实现 |
| LIM-006 | /dev/usb 节点未创建 | 已修复 (v4.4.0) |
| LIM-007 | 三 RISC 架构仅启动核 | 未实现 |
| LIM-008 | GUI 无上层应用 | 未实现 |

## 统计

| 状态 | 数量 | 说明 |
|------|------|------|
| CRITICAL (不可用) | 5 | GUI应用、RISC架构、TLS、UEFI启动、journal |
| PARTIAL (框架存在) | 11 | seccomp、capability、virtio_net、TCP、KASLR、ramfs、USB、动态链接(已集成)、大文件、用户态shell(59+命令) |
| STUB (占位) | 3 | 3 RISC启动核、演示任务、三重间接块 |