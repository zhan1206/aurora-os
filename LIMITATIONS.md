# AuroraOS 已知限制 (v4.3.2)

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

## 可以正常工作的功能

- x86_64 引导 (Multiboot1)
- 内核自检 (13/13 通过)
- 内存管理 (Buddy + SLAB)
- 5个基础文件系统 (ext2/fat32/ramfs/sysfs/devtmpfs) 挂载读写
- 内核Shell (必须在恢复路径运行)
- PIT + APIC 基础中断
- ACPI 关机 (SLP_TYP 回退)

## 根因级限制

### BUG-CURRENT-NULL
- **状态**: 未修复
- **描述**: 内核自检后 `current` 和 `idle_task` 被 BSS 溢出清零，调度器停摆
- **绕过**: 恢复路径 inline 运行 Shell，跳过调度器
- **影响**: 所有依赖于调度器的功能（用户态进程、SMP、网络等）无法正常工作

### BUG-CR3-CACHE
- **状态**: 未修复
- **描述**: `kernel_cr3` (.bss) 被踩穿，每次重新 walk 规避
- **绕过**: 每次都从 CR3 寄存器重新读取
- **影响**: 性能下降，BSS 溢出根本原因未定位

### BSS 段溢出
- **状态**: 未定位
- **描述**: BUG-CURRENT-NULL 和 BUG-CR3-CACHE 指向同一 BSS 溢出
- **影响**: 所有 BSS 变量不可靠

## 架构限制

### GUI / 窗口系统
- 窗口管理 API 存在但无应用调用
- 鼠标光标无法联动（HID驱动存在，GUI未接入）
- 无任何 GUI 应用
- 用户态 Shell 仅 8 个命令

### 多架构
- RISC-V64 / ARM64 / LoongArch64: 仅启动桩，kmain直接halt
- 真正可用的架构只有 x86_64

### 安全子系统
- seccomp: BPF 解释器存在，始终通过（无实际过滤）
- Capability: 框架存在，未接入 setuid/chown 等调用点
- 模块签名: XOR 哈希 + 硬编码密钥（占位实现）
- KASLR: 仅堆/栈/模块随机化，.text/.data 未随机化
- SMAP/SMEP: CPUID检测后启用，QEMU默认不支持
- Stack Protector: 工作中

### 调度器与 SMP
- 只有 CPU0 在运行调度器
- AP 核收到 IPI 后自旋
- 负载均衡为死代码
- VMPair/CFS/EVDF 代码存在，受 BUG-CURRENT-NULL 影响无法工作

### 网络栈
- ARP: 无老化机制、无广播缓存
- IPv4: 无分片重组、无 IP 选项
- TCP: 骨架实现，无 SACK、无高级拥塞控制
- TCP 重传: 已接入 PIT 定时器，未经压力测试
- IPv6: 最小实现（邻居表 + 老化）
- DHCP: 有 REBIND，不完整
- DNS: 有 LRU cache/TTL，解析流程不完整
- AF_UNIX: ~800行，STREAM/DGRAM/close 有，引用计数有已知问题
- virtio_net: MMIO 驱动存在，未经实测

### 文件系统
- EXT2: 无三重间接块，大文件不安全
- FAT32: 簇链未检查，大文件不安全
- sysfs: 只读，写路径有 SMAP bug 绕过
- devtmpfs: /dev/usb/ 节点尚未创建
- Journal (WAL): 代码存在，未经崩溃测试
- fsck: 能通过自检，真实故障注入未测试
- squashfs: 代码存在，未经实测
- tmpfs: 只有 /tmp 挂载，实际使用 RamFS

### USB 子系统
- xHCI: 已合入，已修 11 个 bug
- HID 键鼠: 事件 dequeue + cycle bit 已修复
- USB 设备文件节点: /dev/usb/* 计划中，尚未创建
- NVMe: BARRIER/CID/PRP 修过 6 处，未经实测

### 用户态 / ELF / 动态链接
- 用户态 Shell: 仅 8 个命令
- 动态链接器 ld-so: ~900行代码存在，未集成到 exec
- 自研 libc: 只有 printf/puts/malloc/strcmp 等基础函数
- 仓库只有 hello.c 一个用户态程序

### 内核模块系统
- mod load: 需要签名，签名为 XOR 占位，无法通过
- mod unload: 依赖 REFCOUNT，未完成
- mod_sample.c: 存在但未接入构建系统

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
| LIM-006 | /dev/usb 节点未创建 | 计划中 |
| LIM-007 | 三 RISC 架构仅启动核 | 未实现 |
| LIM-008 | GUI 无上层应用 | 未实现 |

## 统计

| 状态 | 数量 | 说明 |
|------|------|------|
| CRITICAL (不可用) | 9 | GUI应用、RISC架构、TLS、UEFI启动、ld-so、journal、模块签名、SMP调度、网络高级特性 |
| PARTIAL (框架存在) | 11 | seccomp、capability、virtio_net、TCP、KASLR、ramfs、USB、动态链接、大文件、用户态shell |
| STUB (占位) | 7 | 3 RISC启动核、用户态shell、模块签名、ld-so、演示任务、三重间接块 |