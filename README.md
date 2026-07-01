# AuroraOS — 自研 x86_64 操作系统

[![CI Build](https://github.com/zhan1206/aurora-os/actions/workflows/build.yml/badge.svg)](https://github.com/zhan1206/aurora-os/actions/workflows/build.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
[![Lines of Code](https://img.shields.io/badge/code-~8,500%20lines-blue)](kernel/)
[![Self Tests](https://img.shields.io/badge/tests-20/20-brightgreen)](kernel/selftest.c)
[![Version](https://img.shields.io/badge/version-v3.4.0-blue)](CHANGELOG.md)

**100% 自研代码** | 无 Linux 内核代码 | 无第三方内核组件

一个完全从零构建的 x86_64 hobby 操作系统，涵盖引导、分页、进程管理、系统调用、VFS、信号、管道等核心子系统。

---

## 目录

- [快速开始](#快速开始)
- [环境要求](#环境要求)
- [安装与构建](#安装与构建)
- [运行](#运行)
- [架构概览](#架构概览)
- [核心特性](#核心特性)
- [Shell 命令参考](#shell-命令参考)
- [常见问题 (FAQ)](#常见问题-faq)
- [项目结构](#项目结构)
- [测试](#测试)
- [贡献指南](#贡献指南)
- [许可证](#许可证)

---

## 快速开始

```bash
# 1. 安装依赖
sudo apt install -y build-essential xorriso grub-pc-bin mtools qemu-system-x86

# 2. 构建并运行
git clone https://github.com/zhan1206/aurora-os.git
cd AuroraOS
make iso
make run
```

---

## 环境要求

### 操作系统

| 环境 | 状态 | 说明 |
|------|------|------|
| **Ubuntu 22.04+** | 推荐 | 原生运行，全功能支持 |
| **Debian 12+** | 支持 | 与 Ubuntu 类似 |
| **WSL2 (Windows)** | 支持 | 通过 WSL Ubuntu 运行 |
| **macOS** | 支持 | 需要安装交叉编译器和 QEMU |
| **Arch Linux** | 支持 | 通过 pacman 安装依赖 |

### 编译工具链

| 工具 | 最低版本 | 用途 |
|------|----------|------|
| `x86_64-elf-gcc` | 12.0+ | 交叉编译器（推荐） |
| 或 `gcc` | 12.0+ | 系统编译器（需 x86_64 架构） |
| `x86_64-elf-ld` 或 `ld` | 2.38+ | 链接器 |
| **GNU Make** | 4.0+ | 构建系统 |

### 运行依赖

| 工具 | 最低版本 | 用途 |
|------|----------|------|
| `qemu-system-x86_64` | 6.0+ | 虚拟机运行 |
| `grub-mkrescue` | 2.06+ | 创建 ISO 镜像 |

### 可选依赖

| 工具 | 用途 |
|------|------|
| Python 3.8+ | 嵌入式文件生成脚本 |
| nasm | 汇编语言测试（可选） |

---

## 安装与构建

### Ubuntu / Debian（原生）

```bash
# 安装依赖
sudo apt update
sudo apt install -y build-essential xorriso grub-pc-bin mtools qemu-system-x86 python3

# 安装交叉编译器（推荐）
# 方法 1: 从 apt 安装
sudo apt install -y gcc-x86-64-linux-gnu binutils-x86-64-linux-gnu
# 创建符号链接
sudo ln -sf /usr/bin/x86_64-linux-gnu-gcc /usr/local/bin/x86_64-elf-gcc
sudo ln -sf /usr/bin/x86_64-linux-gnu-ld /usr/local/bin/x86_64-elf-ld

# 方法 2: 手动构建交叉编译器（参考 osdev wiki）
# https://wiki.osdev.org/GCC_Cross-Compiler

# 构建项目
git clone https://github.com/zhan1206/aurora-os.git
cd AuroraOS
make        # Release 构建
make iso    # 生成 ISO 镜像
```

### WSL2 (Windows)

```bash
# 1. 在 Windows 上安装 WSL2
#    wsl --install -d Ubuntu-22.04

# 2. 进入 WSL 终端
wsl

# 3. 运行快速安装脚本
cd /mnt/d/自制操作系统
chmod +x scripts/wsl-setup.sh
./scripts/wsl-setup.sh

# 4. 构建项目
make iso
```

### macOS

```bash
# 安装 Homebrew（如未安装）
# /bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"

# 安装依赖
brew install x86_64-elf-gcc x86_64-elf-binutils qemu xorriso mtools make

# 构建项目
git clone https://github.com/zhan1206/aurora-os.git
cd AuroraOS
make iso
```

### 构建目标

```bash
make          # Release 构建（-O2 优化）
make debug    # Debug 构建（-g -O0，含调试符号）
make iso      # 构建 + 生成 bootable ISO
make run      # 构建 ISO + 在 QEMU 中运行
make clean    # 清理所有构建产物
make help     # 显示帮助信息
```

### Docker Build（可复现构建环境）

使用 Docker 在隔离环境中构建，无需手动安装依赖：

```bash
# 构建 Docker 镜像
docker build -t aurora-os .

# 运行构建并提取 ISO 产物
docker run --rm -v $(pwd)/output:/output aurora-os
```

### CMake Build（可选构建系统）

项目同时支持 Makefile 和 CMake 两种构建系统：

```bash
# 配置并构建（Release 模式）
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build

# Debug 模式
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build

# 生成 ISO 镜像
cmake --build build --target iso

# 在 QEMU 中运行
cmake --build build --target run

# 导出 compile_commands.json（供 clang-tidy 等工具使用）
cmake -S . -B build -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
```

> **注意**: Makefile 和 CMake 两种构建方式互不冲突，可根据个人偏好选用。

---

## 运行

### QEMU（推荐）

```bash
# 一键构建并运行
make run

# 或手动运行
qemu-system-x86_64 -m 256M -cdrom os.iso
```

### QEMU 参数说明

| 参数 | 说明 |
|------|------|
| `-m 256M` | 分配 256MB 内存 |
| `-cdrom os.iso` | 从 ISO 镜像启动 |
| `-nographic` | 无图形模式（串口输出） |
| `-no-reboot` | 关机后不重启 |
| `-d int,cpu_reset` | 调试中断和 CPU 复位 |
| `-s -S` | 启动 GDB 调试服务器（端口 1234） |

### 调试模式

```bash
# 启动带调试符号的 QEMU
make debug
make iso
qemu-system-x86_64 -m 256M -cdrom os.iso -s -S

# 在另一个终端连接 GDB
x86_64-elf-gdb build/kernel.elf \
  -ex "target remote localhost:1234" \
  -ex "break kmain" \
  -ex "continue"
```

---

## 架构概览

```
AuroraOS
├── kernel/           # 内核源码（30 个 C 文件，22 个头文件，2 个汇编）
│   ├── entry.S       # Multiboot1 入口 + 32→64 位模式自切换
│   ├── mem.c/h       # 伙伴系统物理页分配器 + Slab 内核堆
│   ├── pagetable.c/h # x86_64 四级页表 + COW 写时复制
│   ├── sched.c/h     # 轮转调度器 + 进程树 + 五状态机
│   ├── syscall.c/h   # 22 个系统调用 + 6 参数 ABI
│   ├── signal.c/h    # POSIX 信号框架（5 种信号）
│   ├── vfs.c/h       # VFS 层 + dentry 缓存
│   ├── console.c/h   # VGA 文本模式 + ANSI + 行编辑
│   ├── keyboard.c    # PS/2 键盘驱动 + E0 键处理
│   ├── selftest.c    # 内核自测试（15 项）
│   └── include/      # 公共头文件
├── arch/x86_64/      # 架构相关汇编（10 个文件）
├── userspace/        # 用户态程序
├── docs/             # 设计文档 + API 文档 + 调试报告
├── scripts/          # 构建辅助脚本
└── Makefile          # 构建系统
```

详细架构说明见 [docs/architecture.md](docs/architecture.md)。

---

## 核心特性

### 内存管理
- **伙伴系统**: MAX_ORDER=10，管理 256 MiB 物理内存
- **Slab 分配器**: 8 个大小类（32B–4096B），小对象高效复用
- **E820 解析**: 支持 Multiboot1 和 Multiboot2 内存信息
- **COW**: 写时复制页面克隆，按需分配
- **ASLR**: 地址空间布局随机化（xorshift64 PRNG）

### 进程管理
- **五状态模型**: RUNNING → READY → BLOCKED → ZOMBIE → DEAD
- **VRFair 调度器**: CFS/EEVDF 启发式调度，基于 vruntime 公平调度
- **进程树**: 父子进程链表 + init 收养孤儿
- **阻塞 waitpid**: 真正阻塞等待，子进程退出时唤醒父进程
- **Fork**: COW 页面克隆 + 完整寄存器状态复制
- **SMP 支持**: 多核 CPU 支持，per-CPU 运行队列，负载均衡

### 系统调用（35+ 个）

| 类别 | 系统调用 |
|------|----------|
| I/O | `read`, `write`, `open`, `close`, `fstat`, `lseek`, `getdents` |
| 进程 | `fork`, `execve`, `exit`, `getpid`, `waitpid` |
| 内存 | `mmap`, `mprotect` |
| 信号 | `kill`, `sigaction`, `sigreturn` |
| 管道 | `pipe` |
| 文件描述符 | `dup`, `dup2` |
| 文件系统 | `mkdir`, `rmdir`, `unlink`, `rename`, `chmod`, `stat` |
| 网络 | `socket`, `bind`, `connect`, `listen`, `accept`, `send`, `recv`, `sendto`, `recvfrom`, `shutdown`, `getsockname` |
| 时间 | `gettimeofday`, `nanosleep`, `times` |
| 设备控制 | `ioctl` |
| I/O 多路复用 | `poll` |

详见 [docs/api.md](docs/api.md)。

### 文件系统
- **VFS 层**: 统一文件系统接口，dentry 缓存 + LRU 回收
- **RamFS**: 内存文件系统（读/写/创建/目录操作）
- **EXT2**: 持久化文件系统（基本读/写/目录操作 + 日志 + fsck）
- **devtmpfs**: 设备文件系统（受 CoolPotOS 启发）
  - `/dev/null` - 数据黑洞
  - `/dev/zero` - 零字节源
  - `/dev/console` - 系统控制台
  - `/dev/tty` - 当前终端
  - `/dev/random` - 硬件随机数（RDRAND，阻塞）
  - `/dev/urandom` - 硬件随机数（RDRAND，非阻塞）
- **procfs**: 虚拟文件系统（受 CoolPotOS 启发）
  - `/proc/cpuinfo` - CPU 信息
  - `/proc/meminfo` - 内存统计
  - `/proc/uptime` - 系统运行时间
  - `/proc/version` - 内核版本
  - `/proc/mounts` - 挂载点信息
  - `/proc/interrupts` - IRQ 向量计数
  - `/proc/filesystems` - 支持的文件系统类型
  - `/proc/cmdline` - 内核命令行参数
  - `/proc/kmsg` - 内核日志环形缓冲区
  - `/proc/self/stat` - 当前进程状态
  - `/proc/self/maps` - 当前进程内存映射
  - `/proc/self/cmdline` - 当前进程命令行
- **管道**: 匿名管道，环形缓冲区（4096 字节）

### 网络栈
- **TCP/IP 协议栈**: 完整的网络协议支持
  - ARP（地址解析协议）
  - IPv4（互联网协议）
  - ICMP（互联网控制消息协议，含 ping 支持）
  - UDP（用户数据报协议）
  - TCP（传输控制协议，含 listen/accept 服务器端支持）
- **Socket API**: 11 个 Berkeley 风格 socket 系统调用
- **VirtIO 网络驱动**: 支持 QEMU VirtIO 网络设备

### 信号
- **信号类型**: SIGINT(2), SIGKILL(9), SIGSEGV(11), SIGTERM(15), SIGCHLD(17)
- **用户态 handler**: sigframe 压栈 + 跳板代码 + sigreturn 恢复
- **默认动作**: 终止/忽略/核心转储

### 安全机制
- **ASLR**: 地址空间布局随机化
- **Stack Protector**: 栈溢出保护（canary 检查）
- **SMAP/SMEP**: 内核访问/执行用户空间内存保护
- **seccomp**: 系统调用过滤
- **Capability**: 权能安全机制
- **内核模块签名**: 模块签名验证（受 CoolPotOS 启发）
- **整数溢出保护**: 关键内存分配路径溢出检查
- **NULL 指针保护**: 系统调用关键路径 NULL 检查

### 性能监控
- **性能计数器**: 上下文切换、系统调用、缺页、COW、IRQ 等指标
- **IRQ 追踪**: 256 向量中断计数器（受 CoolPotOS /proc/interrupts 启发）
- **TSC 校准**: 基于 PIT 的高精度时间戳计数器校准
- **内核日志环形缓冲**: 持久化日志存储，支持 /proc/kmsg 导出

### 内核模块系统
- **ELF 可重定位模块加载**: 支持 .ko 文件动态加载
- **符号解析**: 内核符号表 + 模块间符号引用
- **x86_64 重定位**: R_X86_64_64/PC32/32/32S/RELATIVE
- **模块签名**: HMAC 风格签名验证（可选）

### 终端与 Shell
- **VGA 文本模式**: 80×25 彩色字符
- **ANSI 转义序列**: SGR 颜色 + 光标控制 + 清屏
- **行编辑**: 光标移动、插入/删除、Home/End
- **历史记录**: 32 条环形缓冲区，上下箭头导航
- **Tab 补全**: 命令名和文件名自动补全
- **主题系统**: 3 种模式（Dark/Light/High Contrast）
- **无障碍**: 高对比度模式、减少动画

---

## Shell 命令参考

| 类别 | 命令 | 功能 |
|------|------|------|
| 系统信息 | `help` | 显示帮助 |
| | `about` | 关于 AuroraOS |
| | `sysinfo` | 系统仪表盘 |
| | `uname` | 系统信息 |
| | `uptime` | 系统运行时间 |
| | `date` | 显示日期时间 |
| | `free` | 内存使用情况 |
| | `df` | 磁盘空间使用 |
| | `clear` | 清屏 |
| | `welcome` | 显示欢迎界面 |
| 进程管理 | `ps` | 进程列表 |
| | `exec <prog>` | 执行程序 |
| | `wait` | 等待子进程 |
| | `kill <pid>` | 发送信号 |
| | `exit` | 退出 Shell |
| 文件系统 | `ls` / `ll` / `la` | 文件列表 |
| | `cat <file>` | 查看文件内容 |
| | `echo <text>` | 打印文本 |
| | `touch <file>` | 创建空文件 |
| | `rm <file>` | 删除文件 |
| | `cp <src> <dst>` | 复制文件 |
| | `pwd` | 当前工作目录 |
| | `cd <dir>` | 切换目录 |
| | `mkdir <dir>` | 创建目录 |
| | `wc <file>` | 行/词/字符计数 |
| | `head <file>` | 查看文件头部 |
| | `tail <file>` | 查看文件尾部 |
| 内存 | `mem` | 内存使用情况 |
| 调试 | `perf` | 性能统计 |
| | `mod list/load` | 模块管理 |
| | `env` | 环境变量 |
| | `which` | 命令定位 |
| 个性化 | `theme [dark\|light\|hc]` | 切换主题 |
| | `a11y [hc\|motion]` | 无障碍设置 |
| | `history` | 命令历史 |
| | `lock` | 锁屏 |

---

## 常见问题 (FAQ)

### Q: 构建失败，提示 `x86_64-elf-gcc: command not found`
A: 需要安装交叉编译器。运行 `which x86_64-elf-gcc` 检查。如果未安装，参考[环境要求](#环境要求)中的安装方法。

### Q: macOS 上 `grub-mkrescue` 失败
A: macOS 上的 GRUB 工具可能不完整。建议使用 `brew install xorriso mtools` 并确保 `grub-mkrescue` 在 PATH 中。

### Q: QEMU 启动后显示黑屏
A: 检查 QEMU 版本（`qemu-system-x86_64 --version`），确保 >= 6.0。尝试添加 `-vga std` 参数。

### Q: 如何在实体机上运行？
A: 将 `os.iso` 写入 U 盘：
```bash
sudo dd if=os.iso of=/dev/sdX bs=4M status=progress
```
**注意**: 当前版本仅支持传统 BIOS 启动，不支持 UEFI。

### Q: 如何添加新的系统调用？
A: 1) 在 `kernel/syscall.h` 中添加编号；2) 在 `kernel/syscall.c` 中实现处理函数；3) 在 `handle_syscall` 的 switch 中添加 case。

### Q: 如何添加新的文件系统？
A: 实现 `struct super_block_operations` 和 `struct inode_operations` 接口，在 `kernel/fs.c` 中注册。

### Q: 如何查看调试日志？
A: 构建 Debug 版本：`make debug && make iso`。日志级别可通过 `LOG_LEVEL` 宏控制。

---

## 项目结构

```
AuroraOS/
├── kernel/              # 内核源码
│   ├── include/         # 公共头文件（theme, errno, portio, log 等）
│   ├── *.c, *.h         # 核心模块（30+ C 文件）
│   └── *.S              # 汇编文件（entry.S, irq_handler.S）
├── arch/x86_64/          # 架构相关汇编
│   ├── context.S         # 上下文切换
│   ├── syscall.S         # 系统调用入口/返回
│   ├── gdt.S, tss.S      # GDT + TSS（IST 独立栈）
│   └── ...               # 异常处理、缺页、键盘中断桩
├── userspace/            # 用户态程序
│   ├── libc.c            # 自研 libc
│   ├── shell.c           # 用户态 Shell
│   ├── hello.c           # 测试程序
│   └── Makefile          # 用户态构建
├── docs/                 # 文档
│   ├── architecture.md   # 系统架构设计文档
│   ├── api.md            # 系统调用 API 文档
│   ├── modules.md        # 模块功能说明
│   ├── user_manual.md    # 用户手册
│   └── debug_report.md   # 调试报告
├── scripts/              # 构建辅助脚本
│   ├── wsl-setup.sh      # WSL 环境安装
│   ├── embed_binary.py   # 二进制文件嵌入
│   └── run_qemu_test.py  # QEMU 自动化测试
├── .github/workflows/    # CI/CD
│   └── build.yml         # GitHub Actions 构建
├── Makefile              # 构建系统
├── linker.ld             # 链接脚本
├── README.md             # 本文件
├── CHANGELOG.md          # 版本更新日志
├── CONTRIBUTING.md       # 贡献指南
└── LICENSE               # MIT 许可证
```

---

## 测试

### 内核自测试

项目内置 16 项自测试，在启动时自动运行：

```
======== Kernel Self-Test ========
--- Buddy Allocator Tests ---
  [PASS] alloc_page/free_page single page
  [PASS] stress alloc: N pages, all freed
  [PASS] re-alloc after stress
--- Slab Allocator Tests ---
  [PASS] kmalloc/kfree all size classes
  [PASS] kmalloc(0) handled
--- Page Table Tests ---
  [PASS] get_kernel_cr3
  [PASS] clone_current_pml4 (COW deep copy)
  [PASS] free_pagetable (COW-aware)
--- Scheduler Tests ---
  [PASS] current task exists
  [PASS] create_task
  [PASS] waitpid + exit code collection
--- VFS / RamFS Tests ---
  [PASS] root filesystem mounted
  [PASS] vfs_lookup(/)
  [PASS] vfs_lookup nonexistent returns NULL
--- Pipe Tests ---
  [PASS] sys_pipe create
  [PASS] pipe write + read roundtrip
======== All Tests Passed ========
```

### CI 自动化测试

GitHub Actions 在每次 push 和 PR 时自动执行：
- 安装依赖（交叉编译器 + GRUB + QEMU）
- Release + Debug 双构建验证
- QEMU 启动测试 + 自检验证
- 上传 ISO 构建产物

---

## 贡献指南

欢迎贡献！请阅读 [CONTRIBUTING.md](CONTRIBUTING.md) 了解：
- 代码风格规范
- 提交 PR 流程
- 如何添加新功能
- 测试要求

---

## 版本历史

详见 [CHANGELOG.md](CHANGELOG.md)。

---

## 设计原则

1. **100% 自研**: 所有代码从零编写，不包含 Linux/BSD/任何第三方内核代码
2. **无外部依赖**: 仅依赖 GCC/LD 交叉编译器 + GRUB2 引导器（行业标准）
3. **模块化**: 清晰的模块边界，头文件接口明确
4. **可测试**: 内置自测试框架，CI 自动化验证
5. **可移植**: 支持 Linux/WSL/macOS 多平台开发

---

## 免责声明

**AuroraOS 是一个实验性的业余操作系统项目 (Hobby OS)，旨在用于学习和研究目的。**

- 本软件按"原样"提供，不提供任何形式的明示或暗示担保，包括但不限于适销性、特定用途适用性和非侵权性的担保。
- 在任何情况下，作者或版权持有人均不对因使用本软件而产生的任何索赔、损害或其他责任负责。
- 本操作系统不适用于生产环境、关键任务系统或任何可能造成人身伤害或财产损失的场景。
- 用户应自行承担在虚拟机或物理硬件上运行本操作系统的所有风险。

详细的许可条款请参阅 [LICENSE](LICENSE) 文件。

## 许可证

MIT License — 详见 [LICENSE](LICENSE)

---

## 致谢

- [OSDev Wiki](https://wiki.osdev.org/) — 操作系统开发知识库
- [Intel® 64 and IA-32 Architectures Software Developer Manuals](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html)
- [AMD64 Architecture Programmer's Manual](https://www.amd.com/en/support/tech-docs)

---

*"Simplicity is the ultimate sophistication."* — Leonardo da Vinci