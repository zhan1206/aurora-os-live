# AuroraOS 新手贡献指南

> 欢迎来到 AuroraOS！无论你是操作系统开发的新手，还是经验丰富的内核开发者，这里都有适合你的任务。本文档将带你从零开始，逐步成长为 AuroraOS 的贡献者。

---

## 目录

- [1. 欢迎与概述](#1-欢迎与概述)
- [2. 快速自检](#2-快速自检)
- [3. 环境准备](#3-环境准备)
- [4. 难度分级入门任务](#4-难度分级入门任务)
  - [4.1 文档改进类（good-first-issue 难度 ★☆☆☆☆）](#41-文档改进类good-first-issue-难度-)
  - [4.2 命令行工具类（beginner-friendly 难度 ★★☆☆☆）](#42-命令行工具类beginner-friendly-难度-)
  - [4.3 应用开发类（beginner-friendly 难度 ★★★☆☆）](#43-应用开发类beginner-friendly-难度-)
  - [4.4 系统增强类（help-wanted 难度 ★★★★☆）](#44-系统增强类help-wanted-难度-)
- [5. 贡献工作流](#5-贡献工作流)
- [6. 常见问题](#6-常见问题)
- [7. 获取帮助](#7-获取帮助)

---

## 1. 欢迎与概述

### 欢迎加入 AuroraOS！

AuroraOS 是一个**完全从零构建**的 x86_64 hobby 操作系统，所有代码**100% 自研**，不包含任何 Linux 内核代码或第三方内核组件。项目涵盖了操作系统内核的几乎所有核心子系统：

| 子系统 | 核心模块 | 说明 |
|--------|----------|------|
| 内存管理 | `kernel/mem.c`, `kernel/pagetable.c` | Buddy 伙伴系统 + Slab 分配器 + COW 写时复制 |
| 进程管理 | `kernel/sched.c` | 五状态进程模型 + 轮转调度 + fork/exec |
| 文件系统 | `kernel/vfs.c`, `kernel/ramfs.c`, `kernel/ext2.c` | VFS 抽象层 + RamFS + Ext2 支持 |
| 系统调用 | `kernel/syscall.c`, `kernel/syscall.h` | 22 个系统调用，兼容 Linux x86_64 ABI |
| 信号机制 | `kernel/signal.c` | POSIX 信号框架，5 种信号类型 |
| 设备驱动 | `kernel/console.c`, `kernel/keyboard.c`, `kernel/pit.c` | VGA 控制台、PS/2 键盘、PIT 定时器 |
| 网络 | `kernel/netdev.c`, `kernel/virtio_net.c` | 网络设备抽象 + VirtIO 网络驱动 |
| 安全 | `kernel/capability.c`, `kernel/seccomp.c`, `kernel/aslr.c` | 能力系统、Seccomp、ASLR 地址随机化 |

### 为什么 AuroraOS 是学习操作系统开发的绝佳项目？

1. **代码量适中**：内核约 8,000 行 C 代码，单个开发者可以完整阅读和理解
2. **模块化设计**：每个子系统都有清晰的接口边界，可以单独学习和修改
3. **零依赖**：不依赖任何第三方内核代码，你能看到从硬件到应用层的完整实现
4. **丰富的文档**：`docs/` 目录下有架构设计、API 文档、模块说明、调试报告
5. **活跃的 CI**：GitHub Actions 自动构建验证，确保每次提交的质量
6. **友好的社区**：我们欢迎所有水平的贡献者，特别是初学者

### 项目结构速览

```
AuroraOS/
├── kernel/               # 内核源码（30+ C 文件，22 个头文件）
│   ├── include/          # 公共头文件
│   ├── main.c            # 内核入口
│   ├── mem.c / mem.h     # 物理内存管理（Buddy + Slab）
│   ├── pagetable.c/.h    # 四级页表 + COW
│   ├── sched.c / sched.h # 进程调度器
│   ├── syscall.c / .h    # 系统调用分发器
│   ├── vfs.c / vfs.h     # 虚拟文件系统
│   ├── ramfs.c           # 内存文件系统
│   ├── ext2.c / ext2.h   # Ext2 文件系统
│   ├── shell.c           # 内核 Shell
│   ├── console.c         # VGA 控制台
│   ├── keyboard.c        # PS/2 键盘驱动
│   ├── signal.c / .h     # 信号处理
│   ├── pipe.c            # 管道通信
│   ├── elfloader.c       # ELF 加载器
│   ├── selftest.c        # 内核自测试（16 项）
│   └── ...
├── arch/x86_64/          # 架构相关汇编（10 个 .S 文件）
│   ├── context.S         # 上下文切换
│   ├── syscall.S         # 系统调用入口
│   ├── gdt.S / idt.S     # GDT / IDT 设置
│   └── ...
├── boot/                 # UEFI 引导加载器
├── userspace/            # 用户态程序
│   ├── libc.c            # 自研 libc（syscall 封装 + malloc/free）
│   ├── hello.c           # 示例程序
│   ├── shell.c           # 用户态 Shell
│   └── Makefile.user     # 用户态构建
├── docs/                 # 文档
│   ├── architecture.md   # 系统架构设计文档
│   ├── api.md            # 系统调用 API 文档
│   ├── modules.md        # 模块功能说明
│   ├── user_manual.md    # 用户手册
│   ├── debug_report.md   # 调试报告
│   └── first-contributions.md  # 本文档
├── scripts/              # 构建辅助脚本
├── Makefile              # 构建系统
├── README.md             # 项目说明
├── CONTRIBUTING.md       # 贡献指南
└── CHANGELOG.md          # 更新日志
```

---

## 2. 快速自检

在开始贡献之前，请对照以下清单评估你的准备情况。**不要担心不满足所有条件**——我们为不同水平准备了不同难度的任务！

### 基础能力

| 能力项 | 最低要求 | 推荐水平 | 自评 |
|--------|----------|----------|------|
| C 语言 | 掌握指针、结构体、数组 | 熟悉内存管理、位运算、函数指针 | ☐ |
| 命令行操作 | 基本的 cd/ls/git 操作 | 熟练使用 git、make、gcc | ☐ |
| 操作系统概念 | 了解进程、内存、文件系统基本概念 | 理解系统调用、中断、虚拟内存 | ☐ |
| 英语阅读 | 能阅读英文技术文档和错误信息 | 无障碍阅读 Intel/AMD 手册 | ☐ |

### 工具链熟悉度

| 工具 | 用途 | 是否熟悉？ |
|------|------|-----------|
| Git / GitHub | 版本控制、Fork、PR | ☐ |
| GCC / Make | 编译构建 | ☐ |
| QEMU | 虚拟机运行和调试 | ☐ |
| GDB | 调试（可选） | ☐ |

### 根据自评结果选择任务

| 你的水平 | 推荐任务难度 | 从哪里开始 |
|----------|-------------|-----------|
| 刚开始学 C 语言 | ★☆☆☆☆ | [文档改进类任务](#41-文档改进类good-first-issue-难度-) |
| 熟悉 C 语言，了解 OS 概念 | ★★☆☆☆ | [命令行工具类任务](#42-命令行工具类beginner-friendly-难度-) |
| 熟练 C 编程，写过小型项目 | ★★★☆☆ | [应用开发类任务](#43-应用开发类beginner-friendly-难度-) |
| 深入理解 OS 原理，有内核开发经验 | ★★★★☆ | [系统增强类任务](#44-系统增强类help-wanted-难度-) |

---

## 3. 环境准备

### 3.1 Ubuntu 22.04+（推荐）

这是最推荐的开发环境，所有功能完整支持。

```bash
# 第一步：更新包管理器
sudo apt update

# 第二步：安装构建依赖
sudo apt install -y build-essential xorriso grub-pc-bin mtools qemu-system-x86 python3

# 第三步：安装交叉编译器
sudo apt install -y gcc-x86-64-linux-gnu binutils-x86-64-linux-gnu

# 第四步：创建符号链接（让 Makefile 能找到交叉编译器）
sudo ln -sf /usr/bin/x86_64-linux-gnu-gcc /usr/local/bin/x86_64-elf-gcc
sudo ln -sf /usr/bin/x86_64-linux-gnu-ld /usr/local/bin/x86_64-elf-ld

# 第五步：克隆仓库
git clone https://github.com/zhan1206/aurora-os.git
cd aurora-os

# 第六步：验证构建
make clean && make iso
```

> **常见坑**：如果 `grub-mkrescue` 报错 "xorriso not found"，请确保 `xorriso` 和 `mtools` 都已安装：
> ```bash
> sudo apt install -y xorriso mtools
> ```

### 3.2 WSL2 (Windows)

WSL2 是 Windows 用户的最佳选择，无需双系统即可拥有完整的 Linux 开发环境。

```bash
# 第一步：在 Windows PowerShell（管理员）中安装 WSL2
# wsl --install -d Ubuntu-22.04
# 重启电脑后进入 Ubuntu 终端

# 第二步：使用项目自带的快速安装脚本
# 假设你的项目在 D:\自制操作系统
cd /mnt/d/自制操作系统
chmod +x scripts/wsl-setup.sh
./scripts/wsl-setup.sh

# 第三步：安装交叉编译器
sudo apt install -y gcc-x86-64-linux-gnu binutils-x86-64-linux-gnu
sudo ln -sf /usr/bin/x86_64-linux-gnu-gcc /usr/local/bin/x86_64-elf-gcc
sudo ln -sf /usr/bin/x86_64-linux-gnu-ld /usr/local/bin/x86_64-elf-ld

# 第四步：验证构建
make clean && make iso
```

> **常见坑**：
> - WSL2 默认没有 GUI，QEMU 需要添加 `-nographic` 参数（`make run` 已自动添加）
> - 如果 WSL2 访问 Windows 文件系统很慢，建议将项目克隆到 WSL 原生文件系统（`~/aurora-os`）
> - 如果 QEMU 报 "Could not initialize SDL"，请确认 WSL2 已安装 WSLg 支持

### 3.3 macOS

使用 Homebrew 安装所有依赖。

```bash
# 第一步：安装 Homebrew（如未安装）
# /bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"

# 第二步：安装依赖
brew install x86_64-elf-gcc x86_64-elf-binutils qemu xorriso mtools make

# 第三步：克隆仓库
git clone https://github.com/zhan1206/aurora-os.git
cd aurora-os

# 第四步：验证构建
make clean && make iso
```

> **常见坑**：
> - macOS 的 `grub-mkrescue` 可能不完整。如果 `make iso` 失败，可以尝试 `brew install grub` 或使用 `--HEAD` 安装最新版
> - Apple Silicon (M1/M2/M3) Mac 用户：QEMU 通过 Rosetta 2 模拟 x86_64，运行速度较慢但功能正常

### 3.4 验证安装

完成环境搭建后，运行以下命令验证一切正常：

```bash
# 1. 检查交叉编译器
x86_64-elf-gcc --version
# 预期输出：x86_64-elf-gcc (GCC) 12.x.x 或更高

# 2. 检查 QEMU
qemu-system-x86_64 --version
# 预期输出：QEMU emulator version 6.x.x 或更高

# 3. 构建项目
make clean && make iso
# 预期输出：无错误，最终生成 os.iso

# 4. 运行测试
make run
# 预期输出：QEMU 启动，显示 AuroraOS 内核自测试全部通过，进入登录界面
```

### 3.5 构建目标速查

| 命令 | 说明 |
|------|------|
| `make` | Release 构建（-O2 优化） |
| `make debug` | Debug 构建（-g -O0，含调试符号） |
| `make iso` | 构建 + 生成可启动 ISO 镜像 |
| `make run` | 构建 ISO + 在 QEMU 中运行 |
| `make uefi` | 构建 UEFI 引导加载器 |
| `make clean` | 清理所有构建产物 |
| `make help` | 显示帮助信息 |

---

## 4. 难度分级入门任务

以下是精心挑选的 10 个入门任务，按难度从低到高排列。每个任务都标注了涉及的文件、所需技能和预期成果。

---

### 4.1 文档改进类（good-first-issue 难度 ★☆☆☆☆）

这类任务不需要任何编程经验，只需要细心和耐心。适合作为你的第一个 Pull Request。

---

#### 任务 1：修复文档拼写和语法错误

| 属性 | 内容 |
|------|------|
| **标签** | `good first issue` |
| **难度** | ★☆☆☆☆（入门） |
| **目标文件** | `README.md`, `CONTRIBUTING.md`, `docs/*.md` |
| **所需技能** | 基本的中文/英文阅读能力 |
| **预计时间** | 1-2 小时 |

**任务描述**：

仔细阅读项目中的文档文件，找出并修复以下类型的问题：
- 中文错别字（如"的/地/得"混用）
- 英文拼写错误
- 标点符号不一致（中英文标点混用）
- Markdown 格式问题（如标题层级不正确）
- 链接失效或指向错误

**预期成果**：

提交一个 Pull Request，其中包含对文档文件的修正。修改应当是原子性的——每个 commit 专注于一类问题。

**提示与参考资源**：

- 先运行 `git grep -n "xxx" -- "*.md"` 搜索可能的错误模式
- 重点关注 `README.md`（项目门面）和 `docs/user_manual.md`（用户手册）
- 注意不要改变代码块中的内容（那是命令示例，需要保持原样）
- 参考 `CONTRIBUTING.md` 中的[提交规范](CONTRIBUTING.md#提交规范)

---

#### 任务 2：优化文档格式和排版

| 属性 | 内容 |
|------|------|
| **标签** | `good first issue` |
| **难度** | ★☆☆☆☆（入门） |
| **目标文件** | 所有 Markdown 文件 |
| **所需技能** | Markdown 格式化 |
| **预计时间** | 1-2 小时 |

**任务描述**：

改进项目文档的格式和排版，使之更加规范和易读：
- 统一标题层级（确保每个文档只有一个 `#` 一级标题）
- 为长文档添加内部目录（TOC）
- 统一代码块的语言标注（如 ` ```c ` 替代 ` ``` `）
- 表格对齐和格式化
- 确保空行使用一致（段落之间一个空行，章节之间两个空行）

**预期成果**：

提交一个 Pull Request，改进文档的可读性和一致性。

**提示与参考资源**：

- 使用 VS Code 的 Markdown 预览功能检查排版效果
- 当前 `docs/` 目录下有 5 个文档：`architecture.md`, `api.md`, `modules.md`, `user_manual.md`, `debug_report.md`
- 可以在 `docs/` 目录下为文档添加交叉引用链接

---

### 4.2 命令行工具类（beginner-friendly 难度 ★★☆☆☆）

这类任务需要基本的 C 语言编程能力，但不需要理解内核内部机制。所有修改集中在 `kernel/shell.c` 一个文件中。

---

#### 任务 3：添加 head/tail 命令

| 属性 | 内容 |
|------|------|
| **标签** | `beginner-friendly` |
| **难度** | ★★☆☆☆（初级） |
| **目标文件** | `kernel/shell.c` |
| **所需技能** | 基本 C 语言编程 |
| **预计时间** | 2-4 小时 |

**任务描述**：

在 AuroraOS Shell 中添加 `head` 和 `tail` 命令，用于显示文件的前 N 行或后 N 行。

**功能要求**：

```
head [-n N] <filename>    显示文件前 N 行（默认 10 行）
tail [-n N] <filename>    显示文件后 N 行（默认 10 行）
```

**实现指南**：

1. 在 `kernel/shell.c` 中找到命令处理函数（搜索 `cmd_` 前缀的函数）
2. 参考现有的 `cmd_cat` 实现，了解如何打开和读取文件
3. 实现思路：
   - `head`：使用 `vfs_open()` 打开文件，逐行读取，输出前 N 行后停止
   - `tail`：先读取所有行到循环缓冲区（大小 N），最后输出缓冲区内容
4. 在命令注册表中添加 `"head"` 和 `"tail"` 条目
5. 在 `help` 输出中添加新命令的说明

**预期成果**：

提交一个 Pull Request，包含可工作的 `head` 和 `tail` 命令，支持 `-n` 参数。

**提示与参考资源**：

- 查看 `cmd_cat` 函数的实现，了解文件读取流程
- `vfs_open(path, flags)` 返回 `struct file *`，使用 `vfs_read()` 读取内容
- 行结束符是 `\n`
- 在 `shell.c` 中搜索 `"cat"` 可以找到命令注册的位置

---

#### 任务 4：添加 wc 命令

| 属性 | 内容 |
|------|------|
| **标签** | `beginner-friendly` |
| **难度** | ★★☆☆☆（初级） |
| **目标文件** | `kernel/shell.c` |
| **所需技能** | 基本 C 语言，字符串处理 |
| **预计时间** | 2-4 小时 |

**任务描述**：

实现 `wc`（word count）命令，统计文件的行数、单词数和字符数。

**功能要求**：

```
wc [-l] [-w] [-c] <filename>
  -l  只显示行数
  -w  只显示单词数
  -c  只显示字节数
  无参数时显示全部三项
```

**实现指南**：

1. 在 `kernel/shell.c` 中添加 `cmd_wc` 函数
2. 实现思路：
   - 遍历文件内容，统计 `\n`（行数）、空格/制表符边界（单词数）、总字节数（字符数）
   - 单词边界：从非空白字符到空白字符的转换计为一个单词
3. 支持组合参数（如 `wc -l -c` 同时显示行数和字符数）
4. 在命令注册表和帮助中添加 `wc` 条目

**预期成果**：

提交一个 Pull Request，包含完整的 `wc` 命令实现。

**提示与参考资源**：

- 参考 `cmd_cat` 的文件读取方式
- 使用 `strcmp` 解析参数
- 注意处理空文件（0 行 0 单词 0 字符）和只有空格的文件

---

#### 任务 5：改进 history 命令

| 属性 | 内容 |
|------|------|
| **标签** | `beginner-friendly` |
| **难度** | ★★☆☆☆（初级） |
| **目标文件** | `kernel/shell.c` |
| **所需技能** | 基本 C 语言 |
| **预计时间** | 2-3 小时 |

**任务描述**：

增强现有的 `history` 命令，添加搜索和清除功能。

**当前状态**：

`history` 命令已能显示最近 32 条命令历史，但功能有限。

**功能要求**：

```
history                显示完整历史列表（带编号）
history search <keyword>  搜索包含关键词的历史记录
history clear           清除所有历史记录
history <N>             执行历史中第 N 条命令
```

**实现指南**：

1. 在 `cmd_history` 函数中添加参数解析
2. 搜索功能：遍历历史环形缓冲区，匹配包含关键词的条目
3. 清除功能：重置 `history_count` 和 `history_idx`
4. 执行功能：获取第 N 条命令并重新执行（类似 `!N` 功能）
5. 在 `help` 中更新 `history` 命令的说明

**预期成果**：

提交一个 Pull Request，包含增强的 `history` 命令，支持搜索和清除。

**提示与参考资源**：

- 历史记录存储在 `history[HISTORY_MAX][HISTORY_LEN]` 环形缓冲区中
- `history_count` 记录当前条目数，`history_idx` 是写入位置
- 搜索时遍历 `history_count` 个条目，使用 `strstr` 或简单的字符串匹配
- 显示编号时，最新的命令编号为 1

---

### 4.3 应用开发类（beginner-friendly 难度 ★★★☆☆）

这类任务需要编写独立的用户态程序，编译后通过 `exec` 命令在 AuroraOS 中运行。涉及对系统调用的实际使用。

---

#### 任务 6：编写猜数字游戏

| 属性 | 内容 |
|------|------|
| **标签** | `beginner-friendly` |
| **难度** | ★★★☆☆（初级+） |
| **目标文件** | `userspace/guess.c`（新建） |
| **所需技能** | C 语言编程 |
| **预计时间** | 2-4 小时 |

**任务描述**：

编写一个交互式猜数字游戏，作为用户态程序运行在 AuroraOS 中。

**游戏规则**：

1. 程序启动时随机生成一个 1-100 之间的整数
2. 提示用户输入猜测的数字
3. 每次猜测后反馈"太大了"、"太小了"或"恭喜猜对了"
4. 记录猜测次数，猜对后显示统计
5. 支持 `guess --help` 显示帮助信息

**实现指南**：

1. 在 `userspace/` 目录下创建 `guess.c`
2. 参考 `userspace/hello.c` 和 `userspace/libc.c` 了解用户态程序的结构
3. 使用 `userspace/libc.c` 中的函数：
   - `read(fd, buf, count)` — 读取用户输入
   - `write(fd, buf, count)` — 输出文本
   - `printf(fmt, ...)` — 格式化输出
   - `atoi(s)` — 字符串转整数
   - `exit(code)` — 退出程序
4. 由于 AuroraOS 没有 `rand()` 函数，可以使用简单的伪随机数生成器（如 LCG 算法）

**编译方式**：

```bash
cd userspace
make -f Makefile.user guess   # 需要在 Makefile.user 中添加 guess 目标
```

**预期成果**：

提交一个 Pull Request，包含：
- `userspace/guess.c` — 游戏源代码
- 更新 `userspace/Makefile.user` — 添加 `guess` 构建目标
- 在 `kernel/shell.c` 的嵌入式文件列表中注册 `guess` 程序（可选）

**提示与参考资源**：

- 简单的 LCG 伪随机数生成器：`seed = (seed * 1103515245 + 12345) & 0x7fffffff`
- 使用 `getpid()` 作为随机种子
- 参考 `userspace/libc.c` 中的 `printf` 和 `read` 实现
- 用户态程序通过 `exec guess` 在 Shell 中运行

---

#### 任务 7：实现简单计算器

| 属性 | 内容 |
|------|------|
| **标签** | `beginner-friendly` |
| **难度** | ★★★☆☆（初级+） |
| **目标文件** | `userspace/calc.c`（新建） |
| **所需技能** | C 语言，基本表达式解析 |
| **预计时间** | 3-5 小时 |

**任务描述**：

编写一个命令行计算器程序，支持基本四则运算。

**功能要求**：

```
calc 3 + 5          输出：8
calc 10 - 4         输出：6
calc 6 * 7          输出：42
calc 100 / 3        输出：33 （整数除法）
calc 2 + 3 * 4      输出：14 （支持运算符优先级）
calc --help         显示帮助信息
```

**实现指南**：

1. 在 `userspace/` 目录下创建 `calc.c`
2. 实现思路：
   - 从命令行参数读取表达式（或通过 `read()` 从标准输入读取）
   - 解析数字和运算符（支持 `+`, `-`, `*`, `/`）
   - 处理运算符优先级（乘除优先于加减）
   - 处理除零错误
3. 支持两种使用模式：
   - 命令行参数模式：`calc 3 + 5`
   - 交互模式：直接运行 `calc`，然后逐行输入表达式

**预期成果**：

提交一个 Pull Request，包含：
- `userspace/calc.c` — 计算器源代码
- 更新 `userspace/Makefile.user` — 添加 `calc` 构建目标

**提示与参考资源**：

- 使用 `atoi()` 解析数字字符串
- 使用 `strcmp()` 比较运算符
- 对于简单的四则运算，可以使用两遍扫描法：第一遍处理乘除，第二遍处理加减
- 不需要实现括号，那会大大增加复杂度

---

#### 任务 8：编写文件查看器

| 属性 | 内容 |
|------|------|
| **标签** | `beginner-friendly` |
| **难度** | ★★★☆☆（初级+） |
| **目标文件** | `userspace/viewer.c`（新建） |
| **所需技能** | C 语言，系统调用 |
| **预计时间** | 3-5 小时 |

**任务描述**：

编写一个增强的文件查看器，支持分页和搜索功能。

**功能要求**：

```
viewer <filename>              分页显示文件内容（每页 20 行，按任意键继续）
viewer <filename> --search <keyword>  搜索并高亮显示关键词
viewer --help                  显示帮助信息
```

**分页功能**：
- 每显示 20 行后暂停，提示 `-- More (press any key to continue, q to quit) --`
- 按 `q` 退出，按其他键继续显示下一页

**搜索功能**（可选）：
- 搜索包含关键词的行，显示时用 `>>>` 标记

**实现指南**：

1. 在 `userspace/` 目录下创建 `viewer.c`
2. 使用 `open()` / `read()` / `write()` / `close()` 系统调用
3. 注意：用户态程序需要通过 `userspace/libc.c` 中的封装来调用系统调用
4. 如果 libc 中缺少 `open()` 封装，需要先添加

**预期成果**：

提交一个 Pull Request，包含：
- `userspace/viewer.c` — 文件查看器源代码
- 更新 `userspace/Makefile.user` — 添加 `viewer` 构建目标
- 如果需要，在 `userspace/libc.c` 中添加 `open()` 和 `close()` 的封装

**提示与参考资源**：

- 参考 `docs/api.md` 了解系统调用接口
- 参考 `userspace/libc.c` 了解现有的 syscall 封装模式
- 分页逻辑：使用循环读取文件，内部计数器跟踪行数
- 对于暂停，使用 `read(0, buf, 1)` 读取单个字符输入

---

### 4.4 系统增强类（help-wanted 难度 ★★★★☆）

这类任务需要深入理解内核架构，涉及多个文件的修改。建议在完成前述任务后再尝试。

---

#### 任务 9：添加 uptime 系统调用

| 属性 | 内容 |
|------|------|
| **标签** | `help wanted` |
| **难度** | ★★★★☆（中级） |
| **目标文件** | `kernel/syscall.c`, `kernel/syscall.h`, `userspace/libc.c`, `kernel/shell.c` |
| **所需技能** | C 语言，系统调用 ABI 理解 |
| **预计时间** | 3-6 小时 |

**任务描述**：

添加 `SYS_UPTIME` 系统调用，返回系统自启动以来经过的时钟滴答数。

**实现步骤**：

**第一步：定义系统调用号**

在 `kernel/syscall.h` 的 `enum` 中添加：

```c
SYS_UPTIME = 100,  /* 选择一个未使用的编号 */
```

**第二步：实现系统调用处理函数**

在 `kernel/syscall.c` 中添加：

```c
/*
 * 获取系统启动以来的 tick 计数。
 * 每个 tick 由 PIT 定时器产生，频率约 100Hz。
 */
static long sys_uptime(void) {
    extern volatile uint64_t g_ticks;  /* 在 pit.c 中定义 */
    return (long)g_ticks;
}
```

然后在 `handle_syscall()` 的 switch 语句中添加对应的 case。

**第三步：添加用户态封装**

在 `userspace/libc.c` 中添加：

```c
int uptime(void) {
    return (int)sys_call(SYS_UPTIME, 0, 0, 0);
}
```

**第四步：在 Shell 中使用**

在 `kernel/shell.c` 中添加 `uptime` 命令，显示格式如：

```
Up 5 minutes, 32 seconds
```

**预期成果**：

提交一个 Pull Request，包含：
- `kernel/syscall.h` — 添加 `SYS_UPTIME` 编号
- `kernel/syscall.c` — 添加 `sys_uptime()` 实现和分发
- `userspace/libc.c` — 添加 `uptime()` 封装
- `kernel/shell.c` — 添加 `uptime` 命令

**提示与参考资源**：

- 系统 tick 计数器 `g_ticks` 在 `kernel/pit.c` 中定义，PIT 频率为 100Hz（每秒 100 个 tick）
- 参考 `docs/api.md` 和 `docs/modules.md` 了解系统调用实现流程
- 参考 `CONTRIBUTING.md` 中的"添加新的系统调用"章节
- 注意：`syscall.h` 中选择的系统调用号不要与现有的冲突（当前最大编号为 78）

---

#### 任务 10：实现 /dev/null 设备

| 属性 | 内容 |
|------|------|
| **标签** | `help wanted` |
| **难度** | ★★★★☆（中级） |
| **目标文件** | `kernel/ramfs.c`, `kernel/vfs.c`, `kernel/fs.h` |
| **所需技能** | C 语言，VFS 内部机制理解 |
| **预计时间** | 4-8 小时 |

**任务描述**：

实现 `/dev/null` 设备文件，这是一个特殊的字符设备：
- 写入 `/dev/null`：数据被丢弃，返回写入的字节数（成功）
- 读取 `/dev/null`：总是返回 0（EOF）

**实现步骤**：

**第一步：理解 VFS 架构**

阅读以下文件，理解 VFS 的工作原理：
- `kernel/fs.h` — 核心数据结构：`struct file_ops`, `struct inode`, `struct file`
- `kernel/vfs.c` — VFS 层的路径解析和文件操作
- `kernel/ramfs.c` — RamFS 的实现，作为参考

**第二步：创建 /dev/null 的 file_ops**

在 `kernel/ramfs.c`（或新建 `kernel/devnull.c`）中实现：

```c
/* /dev/null 的写入操作：丢弃所有数据，返回写入字节数 */
static ssize_t devnull_write(struct file *filp, const void *buf,
                              size_t count, off_t *offset) {
    (void)filp; (void)buf; (void)offset;
    return (ssize_t)count;  /* 假装写入成功 */
}

/* /dev/null 的读取操作：总是返回 EOF */
static ssize_t devnull_read(struct file *filp, void *buf,
                             size_t count, off_t *offset) {
    (void)filp; (void)buf; (void)count; (void)offset;
    return 0;  /* 永远返回 EOF */
}

static struct file_ops devnull_ops = {
    .open  = NULL,   /* 或实现一个简单的 open 返回 0 */
    .read  = devnull_read,
    .write = devnull_write,
    .close = NULL,
};
```

**第三步：创建 /dev/null 的 inode 并注册到 VFS**

在 `kernel/ramfs.c` 的 `ramfs_init()` 或 VFS 初始化过程中：

```c
/* 创建 /dev 目录和 /dev/null 文件 */
struct inode *devnull_inode = /* 分配并初始化 inode */;
devnull_inode->name = "null";
devnull_inode->ops = &devnull_ops;
devnull_inode->is_dir = 0;
/* 将 inode 挂载到 /dev/null 路径 */
```

**预期成果**：

提交一个 Pull Request，实现 `/dev/null` 设备文件，可以通过以下方式验证：

```bash
# 在 AuroraOS Shell 中测试
echo hello > /dev/null    # 应当无错误，数据被丢弃
cat /dev/null             # 应当无输出（EOF）
```

**提示与参考资源**：

- 阅读 `kernel/fs.h` 理解 `struct file_ops` 每个字段的含义
- 阅读 `kernel/ramfs.c` 理解如何创建 inode 和注册到文件系统
- 阅读 `docs/modules.md` 中关于 VFS 和 RamFS 的说明
- 阅读 `docs/architecture.md` 了解文件系统在整体架构中的位置
- 如果创建 `/dev` 目录，需要先创建一个 `is_dir=1` 的 inode 作为目录

---

## 5. 贡献工作流

本节详细介绍从 Fork 到合并的完整贡献流程，包含每一步的具体命令。

### 5.1 完整流程图

```
Fork 仓库 → Clone 到本地 → 创建分支 → 编写代码 → 构建验证
    → 运行测试 → 提交代码 → 推送到 Fork → 创建 Pull Request
    → 代码审查 → 修改（如有需要） → 合并！
```

### 5.2 详细步骤

#### 第一步：Fork 仓库

1. 访问 [https://github.com/zhan1206/aurora-os](https://github.com/zhan1206/aurora-os)
2. 点击右上角的 **Fork** 按钮
3. 选择 Fork 到你的个人 GitHub 账户

#### 第二步：Clone 到本地

```bash
# 将 YOUR_USERNAME 替换为你的 GitHub 用户名
git clone https://github.com/YOUR_USERNAME/aurora-os.git
cd aurora-os

# 添加原仓库为 upstream（用于后续同步）
git remote add upstream https://github.com/zhan1206/aurora-os.git
git remote -v   # 验证远程仓库配置
```

#### 第三步：创建特性分支

```bash
# 确保在 main 分支上
git checkout main

# 同步上游仓库的最新代码
git fetch upstream
git merge upstream/main

# 创建新分支（分支名应描述你的修改）
git checkout -b feat/add-head-tail-command
# 或
git checkout -b fix/typo-in-readme
# 或
git checkout -b docs/improve-formatting
```

**分支命名建议**：

| 前缀 | 用途 | 示例 |
|------|------|------|
| `feat/` | 新功能 | `feat/add-wc-command` |
| `fix/` | Bug 修复 | `fix/shell-memory-leak` |
| `docs/` | 文档改进 | `docs/fix-typos` |
| `refactor/` | 代码重构 | `refactor/simplify-vfs` |
| `test/` | 测试相关 | `test/add-pipe-tests` |

#### 第四步：编写代码

1. 阅读相关源文件，理解现有代码风格
2. 遵循 `CONTRIBUTING.md` 中的[代码风格规范](CONTRIBUTING.md#代码风格规范)
3. 关键规范速查：
   - 缩进：4 空格，不使用 Tab
   - 函数命名：`snake_case`（如 `cmd_head`, `sys_uptime`）
   - 宏命名：`UPPER_CASE`（如 `PAGE_SIZE`, `MAX_ORDER`）
   - 注释：使用 `/* */` 风格
   - 大括号：K&R 风格

#### 第五步：构建验证

```bash
# 清理并重新构建
make clean && make

# 如果构建失败，检查错误信息并修正
# 常见错误：语法错误、未声明的函数、头文件缺失

# 构建 Debug 版本（便于调试）
make clean && make debug && make iso
```

#### 第六步：运行测试

```bash
# 启动 QEMU 运行系统
make run

# 在 QEMU 中验证：
# 1. 内核自测试全部通过（16/16 PASS）
# 2. 你的新功能正常工作
# 3. 现有功能未受影响（回归测试）
```

#### 第七步：提交代码

```bash
# 查看修改状态
git status

# 添加修改的文件（使用具体文件名，避免 git add .）
git add kernel/shell.c
git add docs/first-contributions.md

# 提交（遵循提交规范）
git commit -m "$(cat <<'EOF'
feat: add head and tail commands to shell

Implement head and tail commands with -n flag support.
head displays first N lines, tail displays last N lines.
Both default to 10 lines when -n is not specified.

Closes #XX
EOF
)"
```

**提交信息格式**（详见 `CONTRIBUTING.md`）：

```
<type>: <简短描述>

<详细说明（可选）>

<关联 issue（可选）>
```

#### 第八步：推送到 Fork

```bash
# 推送你的分支到 GitHub
git push origin feat/add-head-tail-command

# 如果是第一次推送该分支，可能需要设置上游
# git push -u origin feat/add-head-tail-command
```

#### 第九步：创建 Pull Request

1. 访问你 Fork 的仓库页面（`https://github.com/YOUR_USERNAME/aurora-os`）
2. 点击 **Compare & pull request** 按钮
3. 填写 PR 描述，包含以下内容：
   - **变更说明**：做了什么，为什么这样做
   - **测试验证**：如何验证修改正确，附上测试结果
   - **关联 Issue**：`Closes #XX` 或 `Related to #XX`
   - **截图**（如适用）：QEMU 运行截图

4. 点击 **Create pull request** 提交

#### 第十步：代码审查与修改

1. 维护者会审查你的代码，可能提出修改建议
2. 如果需要修改，在本地分支上修改后：
   ```bash
   git add <修改的文件>
   git commit -m "fix: address review comments"
   git push origin feat/add-head-tail-command
   ```
   PR 会自动更新，无需重新创建

3. 审查通过后，维护者会合并你的 PR

#### 第十一步：同步 Fork（可选）

PR 合并后，你的 Fork 会落后于上游仓库。定期同步：

```bash
git checkout main
git fetch upstream
git merge upstream/main
git push origin main
```

### 5.3 保持 PR 简洁

- **一个 PR 做一件事**：不要在一个 PR 中混合多个不相关的修改
- **Commit 粒度合理**：每个 commit 是一个逻辑上完整的修改
- **PR 描述清晰**：让审查者能快速理解你的修改

---

## 6. 常见问题

### 6.1 构建相关

#### Q: 构建失败，提示 `x86_64-elf-gcc: command not found`

**A**: 交叉编译器未安装或未正确配置。

```bash
# 检查是否已安装
which x86_64-elf-gcc

# 如果未安装，执行以下命令
sudo apt install -y gcc-x86-64-linux-gnu binutils-x86-64-linux-gnu
sudo ln -sf /usr/bin/x86_64-linux-gnu-gcc /usr/local/bin/x86_64-elf-gcc
sudo ln -sf /usr/bin/x86_64-linux-gnu-ld /usr/local/bin/x86_64-elf-ld
```

#### Q: `make iso` 失败，提示 `grub-mkrescue: error: xorriso not found`

**A**: 缺少 `xorriso` 和 `mtools` 工具。

```bash
sudo apt install -y xorriso mtools
```

#### Q: 构建时出现大量 `undefined reference` 错误

**A**: 通常是链接阶段缺少源文件。确保：
- 你新增的 `.c` 文件在 `kernel/` 目录下（Makefile 会自动发现）
- 如果新增了 `.S` 汇编文件，确保放在 `kernel/` 或 `arch/` 目录下
- 运行 `make clean && make` 进行完整重新构建

#### Q: macOS 上 `grub-mkrescue` 报错

**A**: macOS 的 GRUB 工具可能不完整。尝试：

```bash
brew install grub
# 或安装最新开发版
brew install grub --HEAD
```

### 6.2 QEMU 相关

#### Q: QEMU 启动后显示黑屏

**A**: 检查以下几点：
- QEMU 版本 >= 6.0：`qemu-system-x86_64 --version`
- 如果使用 WSL2，确保 WSLg 已安装（Windows 11 默认支持）
- 尝试添加 `-nographic` 参数（`make run` 已自动添加）

#### Q: QEMU 报 "Could not initialize SDL"

**A**: 在 WSL2 或无图形界面的服务器上运行。`make run` 默认使用 `-nographic` 参数，应该不会出现此问题。如果手动运行 QEMU，添加 `-nographic` 参数。

#### Q: 如何在 QEMU 中调试内核？

**A**: 使用 GDB 远程调试：

```bash
# 终端 1：启动 QEMU 并等待 GDB 连接
make debug && make iso
qemu-system-x86_64 -m 256M -cdrom os.iso -s -S

# 终端 2：连接 GDB
x86_64-elf-gdb build/kernel.elf \
  -ex "target remote localhost:1234" \
  -ex "break kmain" \
  -ex "continue"
```

### 6.3 PR 流程相关

#### Q: 我的 PR 提交后多久能收到审查？

**A**: 通常在 1-3 天内。如果超过一周没有回应，可以在 PR 中 @ 维护者或通过 GitHub Issues 提醒。

#### Q: 审查者要求我修改代码，我该怎么做？

**A**: 在同一分支上修改代码后，直接 commit 并 push。PR 会自动更新，无需关闭重建。

```bash
# 修改代码后
git add <修改的文件>
git commit -m "fix: address review feedback"
git push origin feat/my-feature
```

#### Q: 我的 PR 被拒绝了，怎么办？

**A**: 不要灰心！审查者会附上拒绝的原因。你可以：
- 仔细阅读反馈，理解问题所在
- 如果不同意，可以在 PR 中友好讨论
- 修改后重新提交

#### Q: 可以同时提交多个 PR 吗？

**A**: 可以，但建议为每个 PR 创建独立的分支，避免相互干扰。

### 6.4 代码相关

#### Q: 为什么我的代码编译通过但在 QEMU 中运行崩溃？

**A**: 常见原因：
- 访问了空指针或无效地址
- 栈溢出（内核栈空间有限）
- 忘记初始化变量
- 使用 `make debug` 构建 Debug 版本，可以获得更多调试信息
- 添加 `log_printf(LOG_LEVEL_DEBUG, ...)` 输出调试日志

#### Q: 内核代码和用户态代码有什么区别？

**A**: 关键区别：

| 特性 | 内核代码（`kernel/`） | 用户态代码（`userspace/`） |
|------|----------------------|---------------------------|
| 编译方式 | 由顶层 `Makefile` 编译，链接为 `kernel.elf` | 由 `userspace/Makefile.user` 单独编译 |
| 可用函数 | 直接调用内核函数（`vfs_open`, `kmalloc` 等） | 只能通过系统调用（`read`, `write`, `exit` 等） |
| 头文件 | 可包含 `kernel/include/` 下的所有头文件 | 只能使用 `userspace/libc.c` 中提供的函数 |
| 运行模式 | Ring 0（最高权限） | Ring 3（受限权限） |

#### Q: 如何添加新的源文件到内核？

**A**: 将 `.c` 文件放在 `kernel/` 目录下，Makefile 会自动发现并编译。如果是头文件，放在 `kernel/include/` 或 `kernel/` 目录下。

---

## 7. 获取帮助

### 7.1 提问前

在提问之前，请先：
1. 阅读本文档和相关文档（`docs/` 目录）
2. 搜索已有的 [GitHub Issues](https://github.com/zhan1206/aurora-os/issues) 看是否有人遇到过相同问题
3. 尝试自己调试 15 分钟以上

### 7.2 如何高效提问

一个好的问题应该包含：

```
标题：简明扼要地描述问题

内容：
- 环境信息：操作系统版本、编译器版本、QEMU 版本
- 复现步骤：详细的操作步骤，让他人能重现问题
- 预期行为：期望发生什么
- 实际行为：实际发生了什么
- 错误日志：完整的错误输出（包括编译错误和 QEMU 输出）
- 已尝试的解决方案：列出你已经尝试过的方法
```

### 7.3 沟通渠道

| 渠道 | 用途 | 链接 |
|------|------|------|
| **GitHub Issues** | Bug 报告、功能请求、任务认领 | [Issues](https://github.com/zhan1206/aurora-os/issues) |
| **GitHub Discussions** | 技术讨论、问答、想法交流 | [Discussions](https://github.com/zhan1206/aurora-os/discussions) |
| **Pull Requests** | 代码贡献 | [Pull Requests](https://github.com/zhan1206/aurora-os/pulls) |

### 7.4 任务认领

在开始工作前，建议先在对应的 Issue 下留言表明你打算认领该任务，格式如下：

```
你好！我想认领这个任务，预计在 X 天内完成。我是第一次贡献 AuroraOS，请多指教！
```

这样可以避免多人同时做同一个任务。

### 7.5 项目维护者

当前项目由 [@zhan1206](https://github.com/zhan1206) 维护。如果你有任何问题或建议，欢迎通过 GitHub 联系。

### 7.6 推荐学习资源

| 资源 | 说明 | 链接 |
|------|------|------|
| **OSDev Wiki** | 操作系统开发知识库 | [wiki.osdev.org](https://wiki.osdev.org/) |
| **Intel SDM** | Intel 64 和 IA-32 架构开发手册 | [Intel SDM](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html) |
| **AMD APM** | AMD64 架构程序员手册 | [AMD APM](https://www.amd.com/en/support/tech-docs) |
| **The Little Book About OS Development** | 入门级 OS 开发教程 | [littleosbook.github.io](https://littleosbook.github.io/) |
| **Writing an OS in Rust** | 使用 Rust 编写操作系统的博客系列 | [os.phil-opp.com](https://os.phil-opp.com/) |

---

## 附录：任务难度对照表

| 难度 | 标签 | 适合人群 | 时间投入 |
|------|------|----------|----------|
| ★☆☆☆☆ | `good first issue` | 无需编程经验，细心即可 | 1-2 小时 |
| ★★☆☆☆ | `beginner-friendly` | 熟悉 C 语言基础 | 2-4 小时 |
| ★★★☆☆ | `beginner-friendly` | 熟练 C 编程，了解系统调用 | 3-5 小时 |
| ★★★★☆ | `help wanted` | 理解 OS 内核原理，有内核开发经验 | 4-8 小时 |

---

> **最后的话**：操作系统开发看似高不可攀，但 AuroraOS 的设计理念就是"简单至上"。每一行代码都是从零开始写的，每一个模块都是可以理解的。不要害怕犯错，大胆地提交你的第一个 PR 吧！我们期待你的贡献。🚀

---

*本文档最后更新于 2026 年 6 月。如有疑问或建议，欢迎提交 Issue 或 PR 改进本文档。*