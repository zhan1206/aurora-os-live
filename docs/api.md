# AuroraOS 系统调用 API 文档

## 1. 概述

AuroraOS 提供 45 个兼容 Linux x86_64 ABI 的系统调用接口。系统调用通过 `syscall` 指令触发，参数传递遵循 System V AMD64 ABI 约定。本文档列出常用系统调用，完整列表见 `kernel/syscall.h`。

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
- `flags`: 打开标志（O_RDONLY=0, O_WRONLY=1, O_RDWR=2）

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

### 2.5 SYS_FSTAT (5) — 获取文件状态

```c
int fstat(int fd, struct stat *statbuf);
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

### 2.6 SYS_LSEEK (8) — 文件定位

```c
off_t lseek(int fd, off_t offset, int whence);
```

**参数**:
- `fd`: 文件描述符
- `offset`: 偏移量
- `whence`: SEEK_SET(0), SEEK_CUR(1), SEEK_END(2)

**返回值**: 新的文件偏移量，错误返回 -1

---

### 2.7 SYS_GETDENTS (78) — 读取目录项

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

**描述**: 加载并执行 ELF 可执行文件，替换当前进程映像。

**参数**:
- `filename`: ELF 文件路径
- `argv`: 参数数组（当前仅支持 NULL）
- `envp`: 环境变量数组（当前仅支持 NULL）

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

## 4. 内存管理系统调用

### 4.1 SYS_MMAP (9) — 内存映射

```c
void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset);
```

**参数**:
- `addr`: 建议地址（当前忽略）
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

## 8. 错误码参考

| 错误码 | 值 | 含义 |
|--------|-----|------|
| EPERM | 1 | 操作不允许 |
| ENOENT | 2 | 文件不存在 |
| ESRCH | 3 | 进程不存在 |
| EINTR | 4 | 被信号中断 |
| EIO | 5 | I/O 错误 |
| EBADF | 9 | 无效的文件描述符 |
| ECHILD | 10 | 没有子进程 |
| ENOMEM | 12 | 内存不足 |
| EFAULT | 14 | 地址无效 |
| EBUSY | 16 | 设备忙 |
| EINVAL | 22 | 参数无效 |
| ENOSYS | 38 | 系统调用不支持 |
| ENOEXEC | 8 | 无效的可执行文件格式 |