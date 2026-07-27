# AuroraOS 系统调用 API 文档

<!-- NOTE: 18 syscalls are documented but not yet implemented. ~32 implemented syscalls are not yet documented. -->

## 1. 概述

AuroraOS 提供 110 个兼容 Linux x86_64 ABI 的系统调用接口。系统调用通过 `syscall` 指令触发，参数传递遵循 System V AMD64 ABI 约定。本文档完整列出所有系统调用，完整列表见 `kernel/syscall.h`。

### 调用约定

| 寄存器 | 用途 |
|--------|------|
| RAX | 系统调用号 |
| RDI | 第 1 参数 |
| RSI | 第 2 参数 |
| RDX | 第 3 参数 |
| R10 | 第 4 参数 |
| R8 | 第 5 参数 |
| R9 | 第 6 参数 |
| RAX (返回) | 返回值，错误时返回 -1 |
| errno | 错误码存储在 `t_errno` |

---

## 2. I/O 系统调用

### 2.1 SYS_READ (0) — 读取文件

```c
ssize_t read(int fd, void *buf, size_t count);
```

**参数**:
- `fd`: 文件描述符
- `buf`: 用户空间缓冲区指针
- `count`: 最大读取字节数

**返回值**: 实际读取字节数，EOF 返回 0，错误返回 -1

**错误码**:
- `EBADF`: 无效的文件描述符
- `EFAULT`: 缓冲区地址无效

---

### 2.2 SYS_WRITE (1) — 写入文件

```c
ssize_t write(int fd, const void *buf, size_t count);
```

**参数**:
- `fd`: 文件描述符
- `buf`: 用户空间缓冲区指针
- `count`: 写入字节数

**返回值**: 实际写入字节数，错误返回 -1

**错误码**:
- `EBADF`: 无效的文件描述符
- `EFAULT`: 缓冲区地址无效

---

### 2.3 SYS_OPEN (2) — 打开文件

```c
int open(const char *pathname, int flags);
```

**参数**:
- `pathname`: 文件路径字符串
- `flags`: 打开标志（O_RDONLY=0, O_WRONLY=1, O_RDWR=2, O_CREAT=64）

**返回值**: 文件描述符，错误返回 -1

**错误码**:
- `ENOENT`: 文件不存在
- `EFAULT`: 路径地址无效

---

### 2.4 SYS_CLOSE (3) — 关闭文件

```c
int close(int fd);
```

**参数**:
- `fd`: 文件描述符

**返回值**: 成功返回 0，错误返回 -1

**错误码**:
- `EBADF`: 无效的文件描述符

---

### 2.5 SYS_STAT (4) — 获取文件状态（按路径）

```c
int stat(const char *pathname, struct kstat_ext *statbuf);
```

**参数**:
- `pathname`: 文件路径
- `statbuf`: 扩展 stat 结构体指针

**返回值**: 成功返回 0，错误返回 -1

**错误码**:
- `ENOENT`: 文件不存在
- `EFAULT`: 地址无效

---

### 2.6 SYS_FSTAT (5) — 获取文件状态（按 fd）

```c
int fstat(int fd, struct kstat *statbuf);
```

**参数**:
- `fd`: 文件描述符
- `statbuf`: stat 结构体指针

**stat 结构体**:
```c
struct kstat {
    uint64_t st_dev;     /* 设备号 */
    uint64_t st_ino;     /* inode 号 */
    uint32_t st_mode;    /* 文件模式 */
    uint32_t st_nlink;   /* 硬链接数 */
    uint32_t st_uid;     /* 用户 ID */
    uint32_t st_gid;     /* 组 ID */
    uint64_t st_size;    /* 文件大小 */
    uint64_t st_blksize; /* 块大小 */
    uint64_t st_blocks;  /* 块数量 */
};
```

**返回值**: 成功返回 0，错误返回 -1

---

### 2.7 SYS_POLL (7) — I/O 多路复用

```c
int poll(struct pollfd *fds, int nfds, int timeout);
```

**参数**:
- `fds`: pollfd 结构体数组
- `nfds`: 文件描述符数量（最大 16）
- `timeout`: 超时毫秒数（0=立即返回，>0=阻塞等待）

**返回值**: 就绪的文件描述符数量，错误返回 -1

**错误码**:
- `EINVAL`: 参数无效
- `EFAULT`: 地址无效

---

### 2.8 SYS_LSEEK (8) — 文件定位

```c
off_t lseek(int fd, off_t offset, int whence);
```

**参数**:
- `fd`: 文件描述符
- `offset`: 偏移量
- `whence`: SEEK_SET(0), SEEK_CUR(1), SEEK_END(2)

**返回值**: 新的文件偏移量，错误返回 -1

---

### 2.9 SYS_IOCTL (16) — 设备控制

```c
int ioctl(int fd, int request, void *arg);
```

**参数**:
- `fd`: 文件描述符
- `request`: 设备特定请求码
- `arg`: 请求参数指针

**返回值**: 成功返回 0，错误返回 -1

**错误码**:
- `EBADF`: 无效的文件描述符
- `ENOTTY`: 不支持的 ioctl 请求
- `EFAULT`: 参数地址无效

---

### 2.10 SYS_ACCESS (21) — 检查文件访问权限

```c
int access(const char *pathname, int mode);
```

**参数**:
- `pathname`: 文件路径
- `mode`: F_OK(0)=存在性, R_OK(4)=可读, W_OK(2)=可写, X_OK(1)=可执行

**返回值**: 成功返回 0，错误返回 -1

**错误码**:
- `ENOENT`: 文件不存在
- `EACCES`: 权限不足
- `EFAULT`: 路径地址无效

---

### 2.11 SYS_GETDENTS (78) — 读取目录项

```c
int getdents(int fd, struct dirent *dirp, unsigned int count);
```

**参数**:
- `fd`: 目录文件描述符
- `dirp`: 目录项缓冲区
- `count`: 缓冲区大小

**返回值**: 读取的字节数，错误返回 -1

---

## 3. 进程管理系统调用

### 3.1 SYS_FORK (57) — 创建子进程

```c
pid_t fork(void);
```

**描述**: 创建调用进程的副本（COW）。子进程返回 0，父进程返回子进程 PID。

**返回值**: 子进程 PID（父进程），0（子进程），-1（错误）

**错误码**:
- `ENOMEM`: 内存不足

---

### 3.2 SYS_EXECVE (59) — 执行程序

```c
int execve(const char *filename, char *const argv[], char *const envp[]);
```

**描述**: 加载并执行 ELF 可执行文件，替换当前进程映像（POSIX exec 语义）。

**参数**:
- `filename`: ELF 文件路径
- `argv`: 参数数组
- `envp`: 环境变量数组

**返回值**: 成功不返回，错误返回 -1

**错误码**:
- `ENOENT`: 文件不存在
- `ENOEXEC`: 不是有效的 ELF 文件
- `EFAULT`: 入口点超出用户空间

---

### 3.3 SYS_EXIT (60) — 退出进程

```c
void exit(int status);
```

**参数**:
- `status`: 退出状态码

**描述**: 终止当前进程，状态码传递给父进程的 waitpid。

---

### 3.4 SYS_GETPID (39) — 获取进程 ID

```c
pid_t getpid(void);
```

**返回值**: 当前进程 PID

---

### 3.5 SYS_WAITPID (61) — 等待子进程

```c
pid_t waitpid(pid_t pid, int *status, int options);
```

**参数**:
- `pid`: -1 表示等待任意子进程
- `status`: 退出状态输出指针
- `options`: 等待选项（0=阻塞）

**返回值**: 子进程 PID，错误返回 -1

**错误码**:
- `ECHILD`: 没有子进程

---

### 3.6 SYS_GETPPID (110) — 获取父进程 ID

```c
pid_t getppid(void);
```

**返回值**: 父进程 PID，init 进程返回 0

---

### 3.7 SYS_NICE (34) — 调整进程优先级

```c
int nice(int inc);
```

**参数**:
- `inc`: 优先级增量（0-255）

**返回值**: 新的优先级值

---

### 3.8 SYS_SCHED_YIELD (24) — 让出处理器

```c
int sched_yield(void);
```

**描述**: 主动让出 CPU 时间片，触发调度器上下文切换。

**返回值**: 成功返回 0

---

## 4. 内存管理系统调用

### 4.1 SYS_MMAP (9) — 内存映射

```c
void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset);
```

**参数**:
- `addr`: 建议地址（忽略，使用 ASLR 随机基址）
- `length`: 映射长度
- `prot`: 保护标志（PROT_READ=1, PROT_WRITE=2, PROT_EXEC=4）
- `flags`: MAP_ANONYMOUS=0x20（当前仅支持匿名映射）
- `fd`: 文件描述符（匿名映射时忽略）
- `offset`: 文件偏移（匿名映射时忽略）

**返回值**: 映射的虚拟地址，错误返回 -1

**错误码**:
- `ENOMEM`: 内存不足
- `EINVAL`: 参数无效
- `ENOSYS`: 不支持的功能

---

### 4.2 SYS_MPROTECT (10) — 修改内存保护

```c
int mprotect(void *addr, size_t length, int prot);
```

**参数**:
- `addr`: 起始地址（页对齐）
- `length`: 区域长度
- `prot`: 新保护标志（PROT_READ=1, PROT_WRITE=2, PROT_EXEC=4）

**返回值**: 成功返回 0，错误返回 -1

**错误码**:
- `EINVAL`: 参数无效
- `EFAULT`: 地址无效

---

### 4.3 SYS_BRK (12) — 修改数据段末尾

```c
void *brk(void *addr);
```

**参数**:
- `addr`: 新的 brk 地址（NULL 表示查询当前 brk）

**返回值**: 新的 brk 地址，错误返回 -1

---

### 4.4 SYS_SBRK (256) — 调整堆大小（AuroraOS 自定义）

```c
void *sbrk(intptr_t increment);
```

**参数**:
- `increment`: 堆大小增量（正=扩展，0=查询，负=不支持）

**返回值**: 旧的 brk 地址，错误返回 -1

**错误码**:
- `ENOMEM`: 内存不足
- `EINVAL`: 参数无效（溢出或越界）

---

### 4.5 SYS_MADVISE (28) — 内存使用建议

```c
int madvise(void *addr, size_t length, int advice);
```

**参数**:
- `addr`: 起始地址
- `length`: 区域长度
- `advice`: 建议类型

**描述**: 当前为 no-op，接受所有建议但不执行任何操作。

**返回值**: 成功返回 0

---

## 5. 信号系统调用

### 5.1 SYS_KILL (62) — 发送信号

```c
int kill(pid_t pid, int sig);
```

**参数**:
- `pid`: 目标进程 PID
- `sig`: 信号编号

**支持的信号**:
| 信号 | 编号 | 默认动作 |
|------|------|----------|
| SIGINT | 2 | 终止 |
| SIGKILL | 9 | 强制终止 |
| SIGSEGV | 11 | 终止+核心转储 |
| SIGCHLD | 17 | 忽略 |
| SIGTERM | 15 | 终止 |

**返回值**: 成功返回 0，错误返回 -1

---

### 5.2 SYS_SIGACTION (13) — 设置信号处理

```c
int sigaction(int signum, const struct sigaction *act, struct sigaction *oldact);
```

**参数**:
- `signum`: 信号编号
- `act`: 新的信号处理动作
- `oldact`: 旧的信号处理动作（可为 NULL）

**sigaction 结构体**:
```c
struct sigaction {
    void (*sa_handler)(int);  /* 信号处理函数 */
    uint64_t sa_flags;        /* 标志位 */
    uint64_t sa_restorer;     /* 信号恢复函数 */
    uint64_t sa_mask;         /* 信号屏蔽集 */
};
```

**返回值**: 成功返回 0，错误返回 -1

---

### 5.3 SYS_SIGRETURN (15) — 从信号处理返回

```c
int sigreturn(void);
```

**描述**: 从信号处理函数返回到被中断的代码。由内核在信号处理完成后自动调用。

---

## 6. 管道系统调用

### 6.1 SYS_PIPE (22) — 创建管道

```c
int pipe(int pipefd[2]);
```

**参数**:
- `pipefd`: 两个文件描述符的输出数组（pipefd[0]=读端, pipefd[1]=写端）

**返回值**: 成功返回 0，错误返回 -1

---

### 6.2 SYS_PIPE2 (293) — 创建管道（带标志）

```c
int pipe2(int pipefd[2], int flags);
```

**参数**:
- `pipefd`: 两个文件描述符的输出数组
- `flags`: 管道标志（当前未完全支持）

**返回值**: 成功返回 0，错误返回 -1

---

## 7. 文件描述符系统调用

### 7.1 SYS_DUP (32) — 复制文件描述符

```c
int dup(int oldfd);
```

**参数**:
- `oldfd`: 要复制的文件描述符

**返回值**: 新的文件描述符，错误返回 -1

---

### 7.2 SYS_DUP2 (33) — 复制到指定文件描述符

```c
int dup2(int oldfd, int newfd);
```

**参数**:
- `oldfd`: 源文件描述符
- `newfd`: 目标文件描述符

**返回值**: 新的文件描述符，错误返回 -1

---

### 7.3 SYS_FCNTL (72) — 文件控制操作

```c
int fcntl(int fd, int cmd, long arg);
```

**参数**:
- `fd`: 文件描述符
- `cmd`: F_DUPFD(0)=复制fd, F_GETFD(1)=获取标志, F_SETFD(2)=设置标志, F_GETFL(3)=获取文件状态, F_SETFL(4)=设置文件状态
- `arg`: 命令参数

**返回值**: 取决于 cmd，错误返回 -1

---

## 8. 文件系统操作

### 8.1 SYS_MKDIR (83) — 创建目录

```c
int mkdir(const char *pathname, int mode);
```

**参数**:
- `pathname`: 目录路径
- `mode`: 权限模式（当前忽略）

**返回值**: 成功返回 0，错误返回 -1

**错误码**:
- `EEXIST`: 目录已存在
- `EFAULT`: 路径地址无效

---

### 8.2 SYS_RMDIR (84) — 删除目录

```c
int rmdir(const char *pathname);
```

**参数**:
- `pathname`: 目录路径

**返回值**: 成功返回 0，错误返回 -1

**错误码**:
- `ENOTEMPTY`: 目录非空
- `EFAULT`: 路径地址无效

---

### 8.3 SYS_UNLINK (87) — 删除文件

```c
int unlink(const char *pathname);
```

**参数**:
- `pathname`: 文件路径

**返回值**: 成功返回 0，错误返回 -1

**错误码**:
- `ENOENT`: 文件不存在
- `EFAULT`: 路径地址无效

---

### 8.4 SYS_RENAME (82) — 重命名文件/目录

```c
int rename(const char *oldpath, const char *newpath);
```

**参数**:
- `oldpath`: 旧路径
- `newpath`: 新路径

**返回值**: 成功返回 0，错误返回 -1

**错误码**:
- `EXDEV`: 跨文件系统重命名
- `EFAULT`: 路径地址无效

---

### 8.5 SYS_CHMOD (90) — 修改文件权限

```c
int chmod(const char *pathname, int mode);
```

**参数**:
- `pathname`: 文件路径
- `mode`: 权限模式

**返回值**: 成功返回 0，错误返回 -1

**错误码**:
- `EACCES`: 权限不足
- `EFAULT`: 路径地址无效

---

### 8.6 SYS_FCHMOD (91) — 修改文件权限（按 fd）

```c
int fchmod(int fd, int mode);
```

**参数**:
- `fd`: 文件描述符
- `mode`: 权限模式

**返回值**: 成功返回 0，错误返回 -1

---

### 8.7 SYS_CHOWN (92) — 修改文件所有者（按路径）

```c
int chown(const char *pathname, int owner, int group);
```

**参数**:
- `pathname`: 文件路径
- `owner`: 用户 ID
- `group`: 组 ID

**描述**: 当前为简化实现（no-op），单用户系统始终允许。

**返回值**: 成功返回 0

---

### 8.8 SYS_FCHOWN (93) — 修改文件所有者（按 fd）

```c
int fchown(int fd, int owner, int group);
```

**参数**:
- `fd`: 文件描述符
- `owner`: 用户 ID
- `group`: 组 ID

**描述**: 当前为简化实现（no-op），单用户系统始终允许。

**返回值**: 成功返回 0

---

### 8.9 SYS_FSYNC (74) — 同步文件到磁盘

```c
int fsync(int fd);
```

**参数**:
- `fd`: 文件描述符

**描述**: 当前为 no-op（ramfs 始终"已同步"）。

**返回值**: 成功返回 0

---

### 8.10 SYS_FTRUNCATE (77) — 截断文件

```c
int ftruncate(int fd, off_t length);
```

**参数**:
- `fd`: 文件描述符
- `length`: 新的文件长度

**返回值**: 成功返回 0，错误返回 -1

---

### 8.11 SYS_SYMLINK (88) — 创建符号链接

```c
int symlink(const char *target, const char *linkpath);
```

**参数**:
- `target`: 链接目标
- `linkpath`: 链接路径

**描述**: 简化实现，创建包含目标路径的普通文件。

**返回值**: 成功返回 0，错误返回 -1

---

### 8.12 SYS_READLINK (89) — 读取符号链接目标

```c
ssize_t readlink(const char *pathname, char *buf, size_t bufsize);
```

**参数**:
- `pathname`: 链接路径
- `buf`: 输出缓冲区
- `bufsize`: 缓冲区大小

**描述**: 简化实现，返回路径本身。

**返回值**: 写入的字节数，错误返回 -1

---

### 8.13 SYS_GETCWD (79) — 获取当前工作目录

```c
char *getcwd(char *buf, size_t size);
```

**参数**:
- `buf`: 输出缓冲区
- `size`: 缓冲区大小

**返回值**: 路径长度，错误返回 -1

**错误码**:
- `ERANGE`: 缓冲区太小

---

### 8.14 SYS_CHDIR (80) — 切换当前工作目录

```c
int chdir(const char *path);
```

**参数**:
- `path`: 目标目录路径

**返回值**: 成功返回 0，错误返回 -1

---

## 9. 网络 Socket 系统调用

### 9.1 SYS_SOCKET (41) — 创建 socket

```c
int socket(int domain, int type, int protocol);
```

**参数**:
- `domain`: AF_INET(2) IPv4
- `type`: SOCK_STREAM(1)=TCP, SOCK_DGRAM(2)=UDP
- `protocol`: 协议（当前忽略）

**返回值**: socket 文件描述符，错误返回 -1

**错误码**:
- `EAFNOSUPPORT`: 不支持的地址族
- `EPROTONOSUPPORT`: 不支持的协议
- `EMFILE`: 文件描述符耗尽

---

### 9.2 SYS_BIND (49) — 绑定地址

```c
int bind(int sockfd, const struct sockaddr_in *addr, int addrlen);
```

**参数**:
- `sockfd`: socket 文件描述符
- `addr`: sockaddr_in 结构体指针
- `addrlen`: 地址结构体大小

**返回值**: 成功返回 0，错误返回 -1

**错误码**:
- `EADDRINUSE`: 地址已占用
- `EFAULT`: 地址无效

---

### 9.3 SYS_LISTEN (50) — 监听连接

```c
int listen(int sockfd, int backlog);
```

**参数**:
- `sockfd`: socket 文件描述符
- `backlog`: 等待队列最大长度

**返回值**: 成功返回 0，错误返回 -1

---

### 9.4 SYS_CONNECT (42) — 连接远程地址

```c
int connect(int sockfd, const struct sockaddr_in *addr, int addrlen);
```

**参数**:
- `sockfd`: socket 文件描述符
- `addr`: 远程地址
- `addrlen`: 地址结构体大小

**返回值**: 成功返回 0，错误返回 -1

**错误码**:
- `ECONNREFUSED`: 连接被拒绝
- `EFAULT`: 地址无效

---

### 9.5 SYS_ACCEPT (43) — 接受连接

```c
int accept(int sockfd, struct sockaddr_in *addr, int *addrlen);
```

**参数**:
- `sockfd`: 监听 socket
- `addr`: 远程地址输出（可为 NULL）
- `addrlen`: 地址长度输出（可为 NULL）

**返回值**: 新的 socket 文件描述符，错误返回 -1

**错误码**:
- `EAGAIN`: 无可用连接

---

### 9.6 SYS_SEND (46) — 发送数据（TCP）

```c
ssize_t send(int sockfd, const void *buf, size_t len, int flags);
```

**参数**:
- `sockfd`: socket 文件描述符
- `buf`: 数据缓冲区
- `len`: 数据长度
- `flags`: 发送标志（当前忽略）

**返回值**: 实际发送字节数，错误返回 -1

**错误码**:
- `ECONNRESET`: 连接已重置
- `EFAULT`: 缓冲区地址无效

---

### 9.7 SYS_RECV (47) — 接收数据（TCP）

```c
ssize_t recv(int sockfd, void *buf, size_t len, int flags);
```

**参数**:
- `sockfd`: socket 文件描述符
- `buf`: 接收缓冲区
- `len`: 缓冲区大小
- `flags`: 接收标志（当前忽略）

**返回值**: 实际接收字节数，错误返回 -1

**错误码**:
- `ECONNRESET`: 连接已重置
- `EFAULT`: 缓冲区地址无效

---

### 9.8 SYS_SENDTO (44) — 发送数据报（UDP）

```c
ssize_t sendto(int sockfd, const void *buf, size_t len, int flags,
               const struct sockaddr_in *dest_addr, int addrlen);
```

**参数**:
- `sockfd`: socket 文件描述符
- `buf`: 数据缓冲区
- `len`: 数据长度
- `flags`: 发送标志（当前忽略）
- `dest_addr`: 目标地址
- `addrlen`: 地址结构体大小

**返回值**: 发送字节数，错误返回 -1

**错误码**:
- `ENETUNREACH`: 网络不可达
- `EFAULT`: 缓冲区地址无效

---

### 9.9 SYS_RECVFROM (45) — 接收数据报（UDP）

```c
ssize_t recvfrom(int sockfd, void *buf, size_t len, int flags,
                 struct sockaddr_in *src_addr, int *addrlen);
```

**参数**:
- `sockfd`: socket 文件描述符
- `buf`: 接收缓冲区
- `len`: 缓冲区大小
- `flags`: 接收标志（当前忽略）
- `src_addr`: 源地址输出（可为 NULL）
- `addrlen`: 地址长度输出（可为 NULL）

**返回值**: 接收字节数，无数据返回 0，错误返回 -1

---

### 9.10 SYS_SHUTDOWN (48) — 关闭 socket 连接

```c
int shutdown(int sockfd, int how);
```

**参数**:
- `sockfd`: socket 文件描述符
- `how`: SHUT_RD(0)=关闭读, SHUT_WR(1)=关闭写, SHUT_RDWR(2)=关闭读写

**返回值**: 成功返回 0，错误返回 -1

**错误码**:
- `ENOTCONN`: socket 未连接

---

### 9.11 SYS_GETSOCKNAME (51) — 获取 socket 本地地址

```c
int getsockname(int sockfd, struct sockaddr_in *addr, int *addrlen);
```

**参数**:
- `sockfd`: socket 文件描述符
- `addr`: 地址输出
- `addrlen`: 地址长度输出

**返回值**: 成功返回 0，错误返回 -1

**错误码**:
- `ENOTSOCK`: 不是 socket

---

## 10. 时间系统调用

### 10.1 SYS_GETTIMEOFDAY (96) — 获取当前时间

```c
int gettimeofday(struct timeval *tv, void *tz);
```

**参数**:
- `tv`: timeval 结构体指针
- `tz`: 时区（当前忽略）

**返回值**: 成功返回 0，错误返回 -1

---

### 10.2 SYS_NANOSLEEP (35) — 高精度睡眠

```c
int nanosleep(const struct timespec *req, struct timespec *rem);
```

**参数**:
- `req`: 请求的睡眠时间
- `rem`: 剩余时间（被信号中断时输出）

**返回值**: 成功返回 0，被信号中断返回 -1

**错误码**:
- `EINTR`: 被信号中断
- `EFAULT`: 地址无效

---

### 10.3 SYS_CLOCK_GETTIME (228) — 获取时钟时间

```c
int clock_gettime(int clock_id, struct timespec *tp);
```

**参数**:
- `clock_id`: CLOCK_REALTIME(0)=实时时钟, CLOCK_MONOTONIC(1)=单调时钟
- `tp`: timespec 输出

**返回值**: 成功返回 0，错误返回 -1

---

### 10.4 SYS_TIMES (100) — 获取进程时间

```c
clock_t times(struct tms *buf);
```

**参数**:
- `buf`: tms 结构体指针

**描述**: 当前返回空值（时间统计未完全实现）。

**返回值**: 成功返回 0，错误返回 -1

---

## 11. 系统信息系统调用

### 11.1 SYS_UNAME (63) — 获取系统信息

```c
int uname(struct utsname *buf);
```

**参数**:
- `buf`: utsname 结构体指针

**返回值**: 成功返回 0，错误返回 -1

---

### 11.2 SYS_SYSINFO (99) — 获取系统统计信息

```c
int sysinfo(struct sysinfo *info);
```

**参数**:
- `info`: sysinfo 结构体指针

**sysinfo 结构体**:
```c
struct sysinfo {
    uint64_t uptime;       /* 启动秒数 */
    uint64_t loads[3];     /* 1, 5, 15 分钟负载 */
    uint64_t totalram;     /* 总内存 */
    uint64_t freeram;      /* 可用内存 */
    uint64_t sharedram;    /* 共享内存 */
    uint64_t bufferram;    /* 缓冲内存 */
    uint64_t totalswap;    /* 总交换空间 */
    uint64_t freeswap;     /* 可用交换空间 */
    uint16_t procs;        /* 进程数 */
    uint64_t totalhigh;    /* 高端内存总量 */
    uint64_t freehigh;     /* 可用高端内存 */
    uint32_t mem_unit;     /* 内存单位 */
};
```

**返回值**: 成功返回 0，错误返回 -1

---

## 12. 用户/组信息系统调用

### 12.1 SYS_GETUID (102) / SYS_GETEUID (107)

```c
uid_t getuid(void);
uid_t geteuid(void);
```

**描述**: 单用户系统，始终返回 0（root）。

---

### 12.2 SYS_GETGID (104) / SYS_GETEGID (108)

```c
gid_t getgid(void);
gid_t getegid(void);
```

**描述**: 单用户系统，始终返回 0（root）。

---

### 12.3 SYS_SETUID (105) / SYS_SETGID (106)

```c
int setuid(uid_t uid);
int setgid(gid_t gid);
```

**描述**: 简化实现，单用户系统始终允许。

---

### 12.4 SYS_GETPGID (121) — 获取进程组 ID

```c
pid_t getpgid(pid_t pid);
```

**参数**:
- `pid`: 进程 ID（0=当前进程）

**返回值**: 进程组 ID（当前简化为 PID），错误返回 -1

---

### 12.5 SYS_SETPGID (109) — 设置进程组 ID

```c
int setpgid(pid_t pid, pid_t pgid);
```

**描述**: 当前为简化实现（no-op），进程组未完全实现。

---

### 12.6 SYS_SETSID (112) — 创建新会话

```c
pid_t setsid(void);
```

**描述**: 简化实现，返回当前 PID 作为会话 ID。

---

## 13. 资源限制系统调用

### 13.1 SYS_GETRLIMIT (97) — 获取资源限制

```c
int getrlimit(int resource, struct rlimit *rlim);
```

**参数**:
- `resource`: 资源类型（RLIMIT_CPU/DATA/STACK/NOFILE/AS）
- `rlim`: rlimit 结构体输出

**返回值**: 成功返回 0，错误返回 -1

---

### 13.2 SYS_SETRLIMIT (160) — 设置资源限制

```c
int setrlimit(int resource, const struct rlimit *rlim);
```

**参数**:
- `resource`: 资源类型
- `rlim`: rlimit 结构体指针

**返回值**: 成功返回 0，错误返回 -1

---

## 14. 环境变量系统调用

### 14.1 SYS_GETENV (257) — 获取环境变量（AuroraOS 自定义）

```c
int getenv(const char *name, char *value, size_t size);
```

**参数**:
- `name`: 环境变量名
- `value`: 输出缓冲区
- `size`: 缓冲区大小

**返回值**: 成功返回 0，错误返回 -1

**错误码**:
- `ENOENT`: 环境变量不存在
- `EFAULT`: 地址无效

---

<!-- FIXED (v4.3.0): DOC-API-SYSCALL -->
### 14.2 SYS_SETENV (325) — 设置环境变量（AuroraOS 自定义）

```c
int setenv(const char *name, const char *value);
```

**参数**:
- `name`: 环境变量名
- `value`: 环境变量值

**返回值**: 成功返回 0，错误返回 -1

**错误码**:
- `ENOMEM`: 环境变量已满（最大 16 个）
- `EFAULT`: 地址无效

---

## 15. 随机数系统调用

### 15.1 SYS_GETRANDOM (318) — 获取随机字节

```c
ssize_t getrandom(void *buf, size_t buflen, unsigned int flags);
```

**参数**:
- `buf`: 输出缓冲区
- `buflen`: 请求字节数
- `flags`: 标志（当前忽略）

**描述**: 使用 ChaCha20 CSPRNG，多源熵（TSC + RDRAND），提供密码学安全的随机数。

**返回值**: 写入的字节数，错误返回 -1

---

## 16. 错误码参考

| 错误码 | 值 | 含义 |
|--------|-----|------|
| EPERM | 1 | 操作不允许 |
| ENOENT | 2 | 文件不存在 |
| ESRCH | 3 | 进程不存在 |
| EINTR | 4 | 被信号中断 |
| EIO | 5 | I/O 错误 |
| ENXIO | 6 | 设备不存在 |
| E2BIG | 7 | 参数列表过长 |
| ENOEXEC | 8 | 无效的可执行文件格式 |
| EBADF | 9 | 无效的文件描述符 |
| ECHILD | 10 | 没有子进程 |
| EAGAIN | 11 | 资源暂时不可用 |
| ENOMEM | 12 | 内存不足 |
| EACCES | 13 | 权限不足 |
| EFAULT | 14 | 地址无效 |
| EBUSY | 16 | 设备忙 |
| EEXIST | 17 | 文件已存在 |
| EXDEV | 18 | 跨设备链接 |
| ENODEV | 19 | 设备不存在 |
| ENOTDIR | 20 | 不是目录 |
| EISDIR | 21 | 是目录 |
| EINVAL | 22 | 参数无效 |
| ENFILE | 23 | 文件表溢出 |
| EMFILE | 24 | 打开文件过多 |
| ENOTTY | 25 | 不支持的 ioctl |
| EFBIG | 27 | 文件过大 |
| ENOSPC | 28 | 设备空间不足 |
| ESPIPE | 29 | 非法 seek |
| EROFS | 30 | 只读文件系统 |
| EMLINK | 31 | 链接过多 |
| EPIPE | 32 | 管道破裂 |
| ERANGE | 34 | 结果超出范围 |
| ENOSYS | 38 | 系统调用不支持 |
| ENOTEMPTY | 39 | 目录非空 |
| ENAMETOOLONG | 36 | 文件名过长 |
| ELOOP | 40 | 符号链接层级过多 |
| EADDRINUSE | 48 | 地址已占用 |
| EADDRNOTAVAIL | 49 | 地址不可用 |
| ENETDOWN | 50 | 网络已关闭 |
| ENETUNREACH | 51 | 网络不可达 |
| ECONNRESET | 54 | 连接已重置 |
| ECONNREFUSED | 61 | 连接被拒绝 |
| EAFNOSUPPORT | 47 | 地址族不支持 |
| EPROTONOSUPPORT | 43 | 协议不支持 |
| ENOTSOCK | 38 | 不是 socket |
| ENOTCONN | 57 | socket 未连接 |

---

## 17. 挂载系统调用（v4.2.6 新增）

### 17.1 SYS_MOUNT (165) — 挂载文件系统

```c
int mount(const char *source, const char *target, const char *filesystemtype,
          unsigned long mountflags, const void *data);
```

**参数**:
- `source`: 设备路径（devtmpfs/sysfs 时可忽略）
- `target`: 挂载点路径
- `filesystemtype`: 文件系统类型（"ext2", "ramfs", "procfs", "sysfs", "devtmpfs"）
- `mountflags`: 挂载标志（MS_RDONLY=1, MS_NOSUID=2 等）
- `data`: 文件系统特定数据（当前忽略）

**返回值**: 成功返回 0，错误返回 -1

**错误码**:
- `ENODEV`: 文件系统类型不支持
- `ENOENT`: 挂载点不存在
- `EBUSY`: 挂载点已被占用

---

### 17.2 SYS_UMOUNT (166) — 卸载文件系统

```c
int umount(const char *target);
```

**参数**:
- `target`: 挂载点路径

**返回值**: 成功返回 0，错误返回 -1

**错误码**:
- `EINVAL`: 路径未挂载
- `EBUSY`: 文件系统正在使用中

---

## 18. 进程/线程扩展系统调用（v4.2.6 新增）

### 18.1 SYS_CLONE (56) — 创建子进程/线程

```c
pid_t clone(unsigned long flags, void *child_stack, int *ptid, int *ctid, unsigned long newtls);
```

**参数**:
- `flags`: 克隆标志（CLONE_VM=0x100, CLONE_FS=0x200, CLONE_FILES=0x400, CLONE_THREAD=0x10000 等）
- `child_stack`: 子进程/线程栈指针（CLONE_VM 时必需）
- `ptid`: 父进程 TID 输出指针
- `ctid`: 子进程 TID 输出指针
- `newtls`: 新线程 TLS 基址

**描述**: 当前简化实现，支持基本 fork 语义和线程创建标志。

**返回值**: 子进程/线程 PID，错误返回 -1

**错误码**:
- `ENOMEM`: 内存不足
- `EINVAL`: 参数无效

---

### 18.2 SYS_EXIT_GROUP (231) — 退出所有线程

```c
void exit_group(int status);
```

**参数**:
- `status`: 退出状态码

**描述**: 终止当前线程组的所有线程。单线程进程等价于 exit()。

---

### 18.3 SYS_GETTID (186) — 获取线程 ID

```c
pid_t gettid(void);
```

**返回值**: 当前线程 TID（在当前实现中等同于 PID）

---

### 18.4 SYS_SET_TID_ADDRESS (218) — 设置 TID 地址

```c
int set_tid_address(int *tidptr);
```

**参数**:
- `tidptr`: 用户空间 TID 存储地址指针

**描述**: 设置线程退出时清除的 TID 地址，用于 pthread 同步。

**返回值**: 当前 TID

---

## 19. 信号扩展系统调用（v4.2.6 新增）

### 19.1 SYS_SIGPROCMASK (14) — 设置/获取信号屏蔽字

```c
int sigprocmask(int how, const sigset_t *set, sigset_t *oldset);
```

**参数**:
- `how`: SIG_BLOCK(0)=阻塞, SIG_UNBLOCK(1)=解除阻塞, SIG_SETMASK(2)=设置
- `set`: 新的信号屏蔽集
- `oldset`: 旧的信号屏蔽集输出（可为 NULL）

**返回值**: 成功返回 0，错误返回 -1

---

### 19.2 SYS_RT_SIGACTION (13) — 实时信号处理

```c
int rt_sigaction(int signum, const struct sigaction *act, struct sigaction *oldact, size_t sigsetsize);
```

**参数**:
- `signum`: 信号编号
- `act`: 新的信号处理动作
- `oldact`: 旧的信号处理动作
- `sigsetsize`: sigset_t 大小（通常为 8）

**描述**: 与 sigaction 功能相同，额外支持 sigset_t 大小参数。

**返回值**: 成功返回 0，错误返回 -1

---

### 19.3 SYS_TGKILL (234) — 向线程发送信号

```c
int tgkill(int tgid, int tid, int sig);
```

**参数**:
- `tgid`: 线程组 ID
- `tid`: 线程 ID
- `sig`: 信号编号

**描述**: 向指定线程组的特定线程发送信号。

**返回值**: 成功返回 0，错误返回 -1

**错误码**:
- `ESRCH`: 进程/线程不存在

---

## 20. 网络扩展系统调用（v4.2.6 新增）

### 20.1 SYS_SOCKETPAIR (53) — 创建 socket 对

```c
int socketpair(int domain, int type, int protocol, int sv[2]);
```

**参数**:
- `domain`: AF_UNIX(1) 本地域（当前仅支持）
- `type`: SOCK_STREAM(1)=TCP, SOCK_DGRAM(2)=UDP
- `protocol`: 协议（当前忽略）
- `sv`: 两个 socket 文件描述符的输出数组

**返回值**: 成功返回 0，错误返回 -1

**错误码**:
- `EAFNOSUPPORT`: 不支持的地址族
- `EMFILE`: 文件描述符耗尽

---

### 20.2 SYS_GETSOCKOPT (55) — 获取 socket 选项

```c
int getsockopt(int sockfd, int level, int optname, void *optval, int *optlen);
```

**参数**:
- `sockfd`: socket 文件描述符
- `level`: SOL_SOCKET(1)=socket 层
- `optname`: SO_REUSEADDR(2)=地址复用, SO_KEEPALIVE(9)=保活, SO_RCVBUF(8)=接收缓冲
- `optval`: 选项值输出
- `optlen`: 选项值长度输出

**返回值**: 成功返回 0，错误返回 -1

---

### 20.3 SYS_SETSOCKOPT (54) — 设置 socket 选项

```c
int setsockopt(int sockfd, int level, int optname, const void *optval, int optlen);
```

**参数**:
- `sockfd`: socket 文件描述符
- `level`: SOL_SOCKET(1)=socket 层
- `optname`: 选项名称
- `optval`: 选项值
- `optlen`: 选项值长度

**返回值**: 成功返回 0，错误返回 -1

---

### 20.4 SYS_GETPEERNAME (52) — 获取对端地址

```c
int getpeername(int sockfd, struct sockaddr_in *addr, int *addrlen);
```

**参数**:
- `sockfd`: socket 文件描述符
- `addr`: 对端地址输出
- `addrlen`: 地址长度输出

**返回值**: 成功返回 0，错误返回 -1

**错误码**:
- `ENOTCONN`: socket 未连接

---

## 21. 内存管理扩展（v4.2.6 新增）

### 21.1 SYS_MUNMAP (11) — 取消内存映射

```c
int munmap(void *addr, size_t length);
```

**参数**:
- `addr`: 映射起始地址（页对齐）
- `length`: 映射长度

**返回值**: 成功返回 0，错误返回 -1

**错误码**:
- `EINVAL`: 参数无效

---

## 22. 文件系统扩展（v4.2.6 新增）

### 22.1 SYS_LINK (86) — 创建硬链接

```c
int link(const char *oldpath, const char *newpath);
```

**参数**:
- `oldpath`: 现有文件路径
- `newpath`: 新链接路径

**返回值**: 成功返回 0，错误返回 -1

**错误码**:
- `ENOENT`: 文件不存在
- `EXDEV`: 跨文件系统链接

---

### 22.2 SYS_MKNOD (133) — 创建设备节点

```c
int mknod(const char *pathname, mode_t mode, dev_t dev);
```

**参数**:
- `pathname`: 节点路径
- `mode`: 文件类型与权限（S_IFCHR=字符设备, S_IFBLK=块设备, S_IFIFO=FIFO）
- `dev`: 设备号（主设备号 << 8 | 次设备号）

**返回值**: 成功返回 0，错误返回 -1

**错误码**:
- `EEXIST`: 节点已存在
- `EFAULT`: 路径地址无效

---

### 22.3 SYS_SELECT (23) — I/O 多路复用

```c
int select(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds, struct timeval *timeout);
```

**参数**:
- `nfds`: 最大文件描述符 + 1
- `readfds`: 可读集合
- `writefds`: 可写集合
- `exceptfds`: 异常集合
- `timeout`: 超时时间（NULL=无限等待）

**返回值**: 就绪的文件描述符数量，超时返回 0，错误返回 -1

---

## 23. 资源与控制系统调用（v4.2.6 新增）

### 23.1 SYS_GETRUSAGE (98) — 获取资源使用情况

```c
int getrusage(int who, struct rusage *usage);
```

**参数**:
- `who`: RUSAGE_SELF(0)=当前进程, RUSAGE_CHILDREN(-1)=子进程
- `usage`: rusage 结构体输出

**rusage 结构体**:
```c
struct rusage {
    struct timeval ru_utime;    /* 用户态 CPU 时间 */
    struct timeval ru_stime;    /* 内核态 CPU 时间 */
    long ru_maxrss;             /* 最大 RSS */
    long ru_minflt;             /* 次要缺页 */
    long ru_majflt;             /* 主要缺页 */
    long ru_nvcsw;              /* 自愿上下文切换 */
    long ru_nivcsw;             /* 非自愿上下文切换 */
};
```

**返回值**: 成功返回 0，错误返回 -1

---

### 23.2 SYS_PRCTL (157) — 进程控制

```c
int prctl(int option, unsigned long arg2, unsigned long arg3, unsigned long arg4, unsigned long arg5);
```

**参数**:
- `option`: 控制选项（PR_SET_NAME=设置进程名, PR_GET_NAME=获取进程名, PR_SET_SECCOMP=设置 seccomp 等）
- `arg2-arg5`: 选项特定参数

**描述**: 当前简化实现，支持基本操作。

**返回值**: 成功返回 0，错误返回 -1

---

### 23.3 SYS_ARCH_PRCTL (158) — 架构特定控制

```c
int arch_prctl(int code, unsigned long addr);
```

**参数**:
- `code`: ARCH_SET_FS=0x1002（设置 FS 段基址）, ARCH_GET_FS=0x1003（获取 FS 段基址）
- `addr`: 段基址

**描述**: x86_64 架构特定控制，用于线程本地存储（TLS）。

**返回值**: 成功返回 0，错误返回 -1

---

### 23.4 SYS_FUTEX (202) — 快速用户空间互斥锁

```c
int futex(int *uaddr, int futex_op, int val, const struct timespec *timeout, int *uaddr2, int val3);
```

**参数**:
- `uaddr`: 用户空间 futex 字地址
- `futex_op`: 操作（FUTEX_WAIT=0=等待, FUTEX_WAKE=1=唤醒）
- `val`: 期望值
- `timeout`: 超时时间（FUTEX_WAIT 时可用）
- `uaddr2`: 第二个 futex 地址（FUTEX_REQUEUE 时使用）
- `val3`: 附加参数

**描述**: 当前简化实现，支持基本 WAIT/WAKE 操作。

**返回值**: 成功返回 0 或唤醒的线程数，错误返回 -1

---

### 23.5 SYS_CLOCK_GETRES (229) — 获取时钟分辨率

```c
int clock_getres(int clock_id, struct timespec *res);
```

**参数**:
- `clock_id`: CLOCK_REALTIME(0)=实时时钟, CLOCK_MONOTONIC(1)=单调时钟
- `res`: timespec 分辨率输出

**返回值**: 成功返回 0，错误返回 -1

---

## 24. 共享内存与 IPC（v4.2.6 新增）

### 24.1 SYS_SHMAT (30) — 附加共享内存

```c
void *shmat(int shmid, const void *shmaddr, int shmflg);
```

**参数**:
- `shmid`: 共享内存段 ID
- `shmaddr`: 建议地址（NULL=自动选择）
- `shmflg`: 附加标志（SHM_RDONLY=0x1000=只读）

**描述**: 当前简化实现，支持基本共享内存附加。

**返回值**: 附加的虚拟地址，错误返回 -1

---

### 24.2 SYS_SHMDT (67) — 分离共享内存

```c
int shmdt(const void *shmaddr);
```

**参数**:
- `shmaddr`: 共享内存附加地址

**返回值**: 成功返回 0，错误返回 -1

---

### 24.3 SYS_SEMGET (64) — 获取信号量集

```c
int semget(key_t key, int nsems, int semflg);
```

**参数**:
- `key`: 信号量键值（IPC_PRIVATE=0=新建）
- `nsems`: 信号量数量
- `semflg`: 创建标志（IPC_CREAT=0x1000, IPC_EXCL=0x2000）

**描述**: 当前简化实现，支持基本信号量集创建。

**返回值**: 信号量集 ID，错误返回 -1

---

### 24.4 SYS_SEMOP (65) — 信号量操作

```c
int semop(int semid, struct sembuf *sops, size_t nsops);
```

**参数**:
- `semid`: 信号量集 ID
- `sops`: 操作数组
- `nsops`: 操作数量

**sembuf 结构体**:
```c
struct sembuf {
    unsigned short sem_num;  /* 信号量编号 */
    short sem_op;             /* 操作（-1=P, +1=V） */
    short sem_flg;            /* 标志（SEM_UNDO=0x1000, IPC_NOWAIT=0x800） */
};
```

**返回值**: 成功返回 0，错误返回 -1

---

## 25. 系统信息扩展（v4.2.6 新增）

### 25.1 SYS_SYSLOG (103) — 读取内核日志

```c
int syslog(int type, char *buf, int len);
```

**参数**:
- `type`: SYSLOG_ACTION_READ(2)=读取, SYSLOG_ACTION_READ_CLEAR(4)=读取并清除
- `buf`: 输出缓冲区
- `len`: 缓冲区大小

**描述**: 读取内核环形缓冲区日志，通过 /proc/kmsg 也可访问。

**返回值**: 读取的字节数，错误返回 -1

---

---

## 27. I/O 扩展系统调用（v4.2.7 新增）

### 27.1 SYS_READV (19) — 分散读取

```c
ssize_t readv(int fd, const struct iovec *iov, int iovcnt);
```

**参数**:
- `fd`: 文件描述符
- `iov`: iovec 结构体数组
- `iovcnt`: iovec 数组元素数量

**iovec 结构体**:
```c
struct iovec {
    void   *iov_base;   /* 缓冲区起始地址 */
    size_t  iov_len;    /* 缓冲区长度 */
};
```

**描述**: 从文件描述符读取数据到多个分散缓冲区（scatter read）。等价于多次 read() 调用但原子性更好。

**返回值**: 实际读取字节数，EOF 返回 0，错误返回 -1

**错误码**:
- `EBADF`: 无效的文件描述符
- `EFAULT`: 缓冲区地址无效
- `EINVAL`: iovcnt 无效

---

### 27.2 SYS_WRITEV (20) — 聚集写入

```c
ssize_t writev(int fd, const struct iovec *iov, int iovcnt);
```

**参数**:
- `fd`: 文件描述符
- `iov`: iovec 结构体数组
- `iovcnt`: iovec 数组元素数量

**描述**: 将多个分散缓冲区的数据聚集写入文件描述符（gather write）。等价于多次 write() 调用但原子性更好。

**返回值**: 实际写入字节数，错误返回 -1

**错误码**:
- `EBADF`: 无效的文件描述符
- `EFAULT`: 缓冲区地址无效
- `EINVAL`: iovcnt 无效

---

### 27.3 SYS_GETDENTS64 (217) — 读取64位目录项

```c
ssize_t getdents64(int fd, struct linux_dirent64 *dirp, unsigned int count);
```

**参数**:
- `fd`: 目录文件描述符
- `dirp`: 目录项缓冲区
- `count`: 缓冲区大小

**linux_dirent64 结构体**:
```c
struct linux_dirent64 {
    uint64_t d_ino;      /* 64位 inode 号 */
    int64_t  d_off;      /* 64位目录偏移 */
    uint16_t d_reclen;   /* 记录长度 */
    uint8_t  d_type;     /* 文件类型 */
    char     d_name[];   /* 文件名（柔性数组） */
};
```

**描述**: 读取目录项，使用 64 位 inode 和偏移量，适用于大文件系统。与 getdents 功能相同但使用 64 位结构体。

**返回值**: 读取的字节数，错误返回 -1

---

## 28. 路径操作扩展（v4.2.7 新增）

### 28.1 SYS_FCHDIR (81) — 按文件描述符切换目录

```c
int fchdir(int fd);
```

**参数**:
- `fd`: 已打开目录的文件描述符

**描述**: 将当前工作目录切换到 fd 对应的目录。与 chdir(path) 功能相同但使用文件描述符而非路径名。

**返回值**: 成功返回 0，错误返回 -1

**错误码**:
- `EBADF`: 无效的文件描述符
- `ENOTDIR`: fd 不是目录

---

## 29. UID/GID 扩展（v4.2.7 新增）

### 29.1 SYS_SETRESUID (117) — 设置真实/有效/保存UID

```c
int setresuid(uid_t ruid, uid_t euid, uid_t suid);
```

**参数**:
- `ruid`: 新的真实用户 ID（-1 表示不变）
- `euid`: 新的有效用户 ID（-1 表示不变）
- `suid`: 新的保存用户 ID（-1 表示不变）

**描述**: 原子地设置进程的真实、有效和保存 set-user-ID。Linux 兼容，单用户系统简化实现。

**返回值**: 成功返回 0，错误返回 -1

---

### 29.2 SYS_GETRESUID (118) — 获取真实/有效/保存UID

```c
int getresuid(uid_t *ruid, uid_t *euid, uid_t *suid);
```

**参数**:
- `ruid`: 真实 UID 输出指针
- `euid`: 有效 UID 输出指针
- `suid`: 保存 UID 输出指针

**描述**: 获取进程的真实、有效和保存 set-user-ID。单用户系统均返回 0。

**返回值**: 成功返回 0，错误返回 -1

---

### 29.3 SYS_SETRESGID (119) — 设置真实/有效/保存GID

```c
int setresgid(gid_t rgid, gid_t egid, gid_t sgid);
```

**参数**:
- `rgid`: 新的真实组 ID（-1 表示不变）
- `egid`: 新的有效组 ID（-1 表示不变）
- `sgid`: 新的保存组 ID（-1 表示不变）

**描述**: 原子地设置进程的真实、有效和保存 set-group-ID。单用户系统简化实现。

**返回值**: 成功返回 0，错误返回 -1

---

### 29.4 SYS_GETRESGID (120) — 获取真实/有效/保存GID

```c
int getresgid(gid_t *rgid, gid_t *egid, gid_t *sgid);
```

**参数**:
- `rgid`: 真实 GID 输出指针
- `egid`: 有效 GID 输出指针
- `sgid`: 保存 GID 输出指针

**描述**: 获取进程的真实、有效和保存 set-group-ID。单用户系统均返回 0。

**返回值**: 成功返回 0，错误返回 -1

---

## 30. CPU 亲和性（v4.2.7 新增）

### 30.1 SYS_SCHED_SETAFFINITY (203) — 设置CPU亲和性

```c
int sched_setaffinity(pid_t pid, size_t cpusetsize, const cpu_set_t *mask);
```

**参数**:
- `pid`: 目标进程 PID（0=当前进程）
- `cpusetsize`: CPU 掩码大小（字节）
- `mask`: CPU 亲和性掩码

**描述**: 设置进程的 CPU 亲和性，限制进程只能在指定 CPU 核心上运行。SMP 系统中用于线程绑定。

**返回值**: 成功返回 0，错误返回 -1

**错误码**:
- `ESRCH`: 进程不存在
- `EINVAL`: 掩码无效

---

### 30.2 SYS_SCHED_GETAFFINITY (204) — 获取CPU亲和性

```c
int sched_getaffinity(pid_t pid, size_t cpusetsize, cpu_set_t *mask);
```

**参数**:
- `pid`: 目标进程 PID（0=当前进程）
- `cpusetsize`: CPU 掩码大小（字节）
- `mask`: CPU 亲和性掩码输出

**描述**: 获取进程当前的 CPU 亲和性掩码。

**返回值**: 成功返回 0，错误返回 -1

---

## 31. *at 系列系统调用（v4.2.7 新增）

*at 系列系统调用允许以相对目录文件描述符为基准进行路径操作，避免 TOCTOU 竞态条件，支持线程安全的路径解析。

### 31.1 SYS_MKDIRAT (258) — 相对目录创建目录

```c
int mkdirat(int dirfd, const char *pathname, int mode);
```

**参数**:
- `dirfd`: 基准目录文件描述符（AT_FDCWD=-100=当前工作目录）
- `pathname`: 相对路径名
- `mode`: 权限模式

**返回值**: 成功返回 0，错误返回 -1

---

### 31.2 SYS_MKNODAT (259) — 相对目录创建设备节点

```c
int mknodat(int dirfd, const char *pathname, mode_t mode, dev_t dev);
```

**参数**:
- `dirfd`: 基准目录文件描述符
- `pathname`: 相对路径名
- `mode`: 文件类型与权限
- `dev`: 设备号

**返回值**: 成功返回 0，错误返回 -1

---

### 31.3 SYS_FCHOWNAT (260) — 相对目录修改所有者

```c
int fchownat(int dirfd, const char *pathname, uid_t owner, gid_t group, int flags);
```

**参数**:
- `dirfd`: 基准目录文件描述符
- `pathname`: 相对路径名
- `owner`: 用户 ID
- `group`: 组 ID
- `flags`: AT_SYMLINK_NOFOLLOW=0x100（不跟随符号链接）

**返回值**: 成功返回 0，错误返回 -1

---

### 31.4 SYS_UNLINKAT (263) — 相对目录删除文件

```c
int unlinkat(int dirfd, const char *pathname, int flags);
```

**参数**:
- `dirfd`: 基准目录文件描述符
- `pathname`: 相对路径名
- `flags`: AT_REMOVEDIR=0x200（删除目录而非文件）

**返回值**: 成功返回 0，错误返回 -1

---

### 31.5 SYS_LINKAT (265) — 相对目录创建硬链接

```c
int linkat(int olddirfd, const char *oldpath, int newdirfd, const char *newpath, int flags);
```

**参数**:
- `olddirfd`: 源基准目录文件描述符
- `oldpath`: 源相对路径
- `newdirfd`: 目标基准目录文件描述符
- `newpath`: 目标相对路径
- `flags`: AT_SYMLINK_FOLLOW=0x400（跟随符号链接）

**返回值**: 成功返回 0，错误返回 -1

---

### 31.6 SYS_SYMLINKAT (266) — 相对目录创建符号链接

```c
int symlinkat(const char *target, int newdirfd, const char *linkpath);
```

**参数**:
- `target`: 链接目标
- `newdirfd`: 基准目录文件描述符
- `linkpath`: 相对链接路径

**返回值**: 成功返回 0，错误返回 -1

---

### 31.7 SYS_READLINKAT (267) — 相对目录读取符号链接

```c
ssize_t readlinkat(int dirfd, const char *pathname, char *buf, size_t bufsize);
```

**参数**:
- `dirfd`: 基准目录文件描述符
- `pathname`: 相对链接路径
- `buf`: 输出缓冲区
- `bufsize`: 缓冲区大小

**返回值**: 写入的字节数，错误返回 -1

---

### 31.8 SYS_FCHMODAT (268) — 相对目录修改权限

```c
int fchmodat(int dirfd, const char *pathname, int mode, int flags);
```

**参数**:
- `dirfd`: 基准目录文件描述符
- `pathname`: 相对路径名
- `mode`: 权限模式
- `flags`: AT_SYMLINK_NOFOLLOW=0x100

**返回值**: 成功返回 0，错误返回 -1

---

### 31.9 SYS_FACCESSAT (269) — 相对目录检查访问权限

```c
int faccessat(int dirfd, const char *pathname, int mode, int flags);
```

**参数**:
- `dirfd`: 基准目录文件描述符
- `pathname`: 相对路径名
- `mode`: F_OK/R_OK/W_OK/X_OK
- `flags`: AT_EACCESS=0x200（使用有效UID/GID检查）

**返回值**: 成功返回 0，错误返回 -1

---

## 32. 资源与内核接口（v4.2.7 新增）

### 32.1 SYS_PRLIMIT64 (302) — 获取/设置资源限制（64位）

```c
int prlimit64(pid_t pid, int resource, const struct rlimit64 *new_limit, struct rlimit64 *old_limit);
```

**参数**:
- `pid`: 目标进程 PID（0=当前进程）
- `resource`: 资源类型（RLIMIT_CPU/DATA/STACK/NOFILE/AS/...）
- `new_limit`: 新的资源限制（NULL=仅查询）
- `old_limit`: 旧的资源限制输出（可为 NULL）

**描述**: 64 位版本的资源限制操作，支持查询和设置任意进程的资源限制。比 getrlimit/setrlimit 更通用。

**返回值**: 成功返回 0，错误返回 -1

---

### 32.2 SYS_NAME_TO_HANDLE_AT (303) — 按名称获取文件句柄

```c
int name_to_handle_at(int dirfd, const char *pathname, struct file_handle *handle, int *mount_id, int flags);
```

**参数**:
- `dirfd`: 基准目录文件描述符
- `pathname`: 相对路径名
- `handle`: 文件句柄输出
- `mount_id`: 挂载点 ID 输出
- `flags`: AT_SYMLINK_FOLLOW=0x400

**描述**: 获取文件系统级别的文件句柄，用于在不依赖路径名的情况下追踪文件。简化实现。

**返回值**: 成功返回 0，错误返回 -1

---

### 32.3 SYS_GETCPU (309) — 获取当前CPU和NUMA节点

```c
int getcpu(unsigned int *cpu, unsigned int *node);
```

**参数**:
- `cpu`: 当前 CPU 编号输出（可为 NULL）
- `node`: 当前 NUMA 节点编号输出（可为 NULL）

**描述**: 获取调用线程当前正在执行的核心和 NUMA 节点编号。简化实现，NUMA 节点始终返回 0。

**返回值**: 成功返回 0，错误返回 -1

---

### 32.4 SYS_MEMBARRIER (324) — 内存屏障

```c
int membarrier(int cmd, int flags);
```

**参数**:
- `cmd`: 屏障命令（MEMBARRIER_CMD_GLOBAL=1 等）
- `flags`: 标志（当前忽略）

**描述**: 在所有运行线程上执行内存屏障，确保跨核心的内存顺序一致性。简化实现。

**返回值**: 成功返回 0，错误返回 -1

---

## 33. ACPI 电源管理（v4.2.7 新增）

### 33.1 SYS_ACPI_SHUTDOWN (319) — ACPI 关机

```c
int acpi_shutdown(void);
```

**描述**: 通过 ACPI 机制执行系统关机。内核调用 ACPI \_S5 对象的 SLP_TYP 值写入 PM1a_CNT 寄存器，触发硬件关机。

**返回值**: 成功不返回，错误返回 -1

**错误码**:
- `ENOSYS`: ACPI 不可用

---

### 33.2 SYS_ACPI_REBOOT (320) — ACPI 重启

```c
int acpi_reboot(void);
```

**描述**: 通过 ACPI 机制执行系统重启。内核尝试 ACPI 复位寄存器（FADT RESET_REG），若失败则回退到键盘控制器（8042）或三击（triple fault）方式。

**返回值**: 成功不返回，错误返回 -1

**错误码**:
- `ENOSYS`: ACPI 及备用重启方式均不可用

---

## 34. 错误码参考（续）