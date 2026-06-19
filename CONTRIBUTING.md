# AuroraOS 贡献指南

感谢你对 AuroraOS 的关注！本文档将帮助你了解如何参与项目贡献。

---

## 目录

- [行为准则](#行为准则)
- [如何贡献](#如何贡献)
- [开发环境搭建](#开发环境搭建)
- [代码风格规范](#代码风格规范)
- [提交规范](#提交规范)
- [Pull Request 流程](#pull-request-流程)
- [测试要求](#测试要求)
- [添加新功能](#添加新功能)
- [问题反馈](#问题反馈)
- [社区沟通](#社区沟通)

---

## 行为准则

- 尊重所有贡献者，无论其经验水平如何
- 使用友好、包容的语言
- 接受建设性批评，专注于技术讨论
- 优先考虑项目的最佳利益

---
<!-- highlight-start -->
## 新手入门 (Beginner-Friendly)

欢迎初学者参与！我们为不同经验水平的开发者准备了标签化的入门任务。

### 任务标签说明

| 标签 | 难度 | 说明 |
|------|------|------|
| `good first issue` | 入门 | 无需操作系统开发经验，适合首次贡献者 |
| `beginner-friendly` | 初级 | 需要基本的 C 语言和系统编程知识 |
| `help wanted` | 中级 | 需要一定的操作系统概念理解 |
| `enhancement` | 高级 | 需要深入理解内核架构 |

### 推荐入门任务

以下是为新手准备的典型入门任务，标记为 `good first issue`：

1. **实现一个简单的设备驱动** (`good first issue`)
   - 难度：入门
   - 描述：为 AuroraOS 实现一个简单的字符设备驱动（如 /dev/null 或 /dev/zero）
   - 涉及模块：`kernel/vfs.c`, `kernel/file.c`
   - 预计时间：2-4 小时
   - 参考文件：`docs/modules.md`

2. **添加系统调用示例** (`good first issue`)
   - 难度：入门
   - 描述：添加一个简单的系统调用（如 `SYS_UPTIME` 获取系统运行时间）
   - 涉及模块：`kernel/syscall.c`, `kernel/syscall.h`, `userspace/libc.c`
   - 预计时间：1-3 小时
   - 参考文档：本文件的"添加新的系统调用"章节

3. **改进 Shell 命令** (`beginner-friendly`)
   - 难度：初级
   - 描述：为 Shell 添加新命令或改进现有命令（如添加 `wc` 或 `head` 命令）
   - 涉及模块：`kernel/shell.c`
   - 预计时间：2-5 小时

4. **编写单元测试** (`beginner-friendly`)
   - 难度：初级
   - 描述：为现有模块添加更多的自测试用例
   - 涉及模块：`kernel/selftest.c`
   - 预计时间：1-3 小时
   - 参考：现有的 16 项自测试

5. **文档翻译与改进** (`good first issue`)
   - 难度：入门
   - 描述：改进现有文档的清晰度，或添加代码注释
   - 涉及模块：`docs/`, 各源文件
   - 预计时间：1-2 小时

6. **实现简单的用户态程序** (`beginner-friendly`)
   - 难度：初级
   - 描述：编写一个用户态程序（如计算器、猜数字游戏等）
   - 涉及模块：`userspace/`
   - 预计时间：2-4 小时

### 新手参与流程

1. 在 [GitHub Issues](https://github.com/zhan1206/aurora-os/issues) 中查找带 `good first issue` 或 `beginner-friendly` 标签的任务
2. 在 Issue 下留言表明你想认领该任务
3. Fork 仓库并创建分支
4. 完成后提交 Pull Request（参考 [Pull Request 流程](#pull-request-流程)）
5. 维护者会尽快审查并提供反馈

**提示**：如果你对任务有疑问，请在 Issue 中直接提问，我们会尽力帮助你！

---
<!-- highlight-end -->

## 如何贡献

贡献方式包括但不限于：

| 类型 | 说明 |
|------|------|
| **Bug 修复** | 修复现有功能中的缺陷 |
| **新功能** | 添加新的系统调用、驱动、文件系统等 |
| **文档改进** | 修正错误、补充说明、添加示例 |
| **代码优化** | 重构、性能优化、内存优化 |
| **测试补充** | 添加单元测试、集成测试用例 |
| **代码审查** | 审查其他贡献者的 PR |

---

## 开发环境搭建

### 前置要求

- **操作系统**: Linux (推荐 Ubuntu 22.04+)、WSL2、macOS
- **编译器**: `x86_64-elf-gcc` 12.0+ 或 `gcc` 12.0+
- **链接器**: `x86_64-elf-ld` 或 `ld` 2.38+
- **构建工具**: GNU Make 4.0+
- **模拟器**: QEMU 6.0+ (用于测试运行)
- **ISO 工具**: GRUB2 + xorriso + mtools (用于生成启动镜像)

### 快速安装 (Ubuntu/Debian)

```bash
sudo apt update
sudo apt install -y build-essential xorriso grub-pc-bin mtools qemu-system-x86 python3
sudo apt install -y gcc-x86-64-linux-gnu binutils-x86-64-linux-gnu
sudo ln -sf /usr/bin/x86_64-linux-gnu-gcc /usr/local/bin/x86_64-elf-gcc
sudo ln -sf /usr/bin/x86_64-linux-gnu-ld /usr/local/bin/x86_64-elf-ld
```

### 克隆仓库

```bash
git clone https://github.com/zhan1206/aurora-os.git
cd AuroraOS
```

### 构建与运行

```bash
make          # Release 构建
make debug    # Debug 构建（含调试符号）
make iso      # 生成可启动 ISO 镜像
make run      # 构建 ISO 并在 QEMU 中运行
make clean    # 清理构建产物
```

---

## 代码风格规范

### C 语言规范

- **标准**: C17 (GNU `-std=gnu17`)
- **缩进**: 4 空格缩进，不使用 Tab
- **行宽**: 建议不超过 100 字符
- **大括号**: K&R 风格（左大括号在行尾）
- **命名规范**:
  - 函数: `snake_case` (如 `alloc_page`, `vfs_lookup`)
  - 结构体: `snake_case` (如 `task_struct`, `super_block`)
  - 宏常量: `UPPER_CASE` (如 `PAGE_SIZE`, `MAX_ORDER`)
  - 全局变量: `g_` 前缀 (如 `g_theme_mode`)
- **注释**: 使用 `/* */` 风格，每个源文件顶部包含文件说明
- **头文件保护**: 使用 `#pragma once` 或 `#ifndef` 守卫

### 示例

```c
/*
 * example.c - Brief description of this module
 *
 * Detailed explanation of design decisions and architecture.
 */

#include "example.h"
#include <stdint.h>

/* Public API implementation */

int do_something(int param) {
    if (param < 0) return -1;  /* Validate input */

    int result = 0;
    /* ... implementation ... */

    return result;
}
```

### 汇编规范 (.S 文件)

- 使用 AT&T 语法 (GAS)
- 每个函数前添加简短注释说明用途
- 寄存器使用明确标注

---

## 提交规范

### Commit Message 格式

```
<type>: <简短描述>

<详细说明（可选）>

<关联 issue（可选）>
```

### Type 类型

| 类型 | 说明 |
|------|------|
| `feat` | 新功能 |
| `fix` | Bug 修复 |
| `docs` | 文档更新 |
| `style` | 代码风格调整（不影响功能） |
| `refactor` | 代码重构 |
| `perf` | 性能优化 |
| `test` | 测试相关 |
| `chore` | 构建/工具链相关 |

### 示例

```
feat: add SYS_GETDENTS system call for directory listing

Implement getdents64 syscall to support reading directory entries.
Uses RamFS directory iteration to fill the user buffer.

Closes #42
```

---

## Pull Request 流程

1. **Fork 本仓库** 并创建你的特性分支

   ```bash
   git checkout -b feat/my-feature
   ```

2. **编写代码** 并确保通过构建

   ```bash
   make clean && make
   ```

3. **运行测试** 验证功能正常

   ```bash
   make iso && make run
   ```

   确认内核自测试全部通过（15/15 PASS）。

4. **提交代码** 遵循[提交规范](#提交规范)

   ```bash
   git add <files>
   git commit -m "feat: description"
   ```

5. **推送分支** 到你的 Fork

   ```bash
   git push origin feat/my-feature
   ```

6. **创建 Pull Request** 到主仓库的 `main` 分支

   PR 描述应包含：
   - 变更说明（做了什么、为什么这样做）
   - 测试验证结果
   - 关联的 Issue 编号

7. **代码审查**: 维护者会审查你的代码，可能需要修改

8. **合并**: 审查通过后，维护者会合并你的 PR

---

## 测试要求

### 内核自测试

所有 PR 必须确保内核自测试（15 项）全部通过：

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
======== All Tests Passed ========
```

### 添加新测试

在 `kernel/selftest.c` 中添加测试函数，遵循以下模式：

```c
static void test_my_feature(void) {
    log_printf(LOG_LEVEL_INFO, "--- My Feature Tests ---\n");

    /* Test case 1 */
    int result = my_function();
    if (result != expected) TEST_FAIL("my_function returned unexpected value");

    TEST_PASS("my_function basic test");
}
```

然后在 `kernel_selftest()` 中调用 `test_my_feature()`。

### CI 自动化

GitHub Actions 在每次 push 和 PR 时自动执行构建验证。详见 [.github/workflows/build.yml](.github/workflows/build.yml)。

---

## 添加新功能

### 添加新的系统调用

1. 在 `kernel/syscall.h` 中定义系统调用号：

   ```c
   #define SYS_MYFEATURE  100
   ```

2. 在 `kernel/syscall.c` 中实现处理函数：

   ```c
   static long sys_myfeature(int arg1, const char *arg2) {
       /* 参数验证 */
       if (arg1 < 0) { current->t_errno = EINVAL; return -1; }

       /* 实现逻辑 */
       /* ... */

       return 0;  /* 成功返回 */
   }
   ```

3. 在 `handle_syscall()` 的 switch 中添加 case：

   ```c
   case SYS_MYFEATURE:
       return sys_myfeature((int)a1, (const char *)a2);
   ```

4. 在 `userspace/libc.c` 中添加用户态封装：

   ```c
   int myfeature(int arg1, const char *arg2) {
       return syscall3(SYS_MYFEATURE, arg1, (long)arg2, 0);
   }
   ```

### 添加新的文件系统

实现以下接口：

- `struct super_block_operations` — 超级块操作
- `struct inode_operations` — 索引节点操作
- 在 `kernel/fs.c` 中注册文件系统类型

---

## 问题反馈

### 提交 Bug 报告

在 GitHub Issues 中提交 Bug 报告时，请包含以下信息：

- **操作系统版本**: 构建环境（Ubuntu 22.04, WSL2, macOS 等）
- **编译器版本**: `x86_64-elf-gcc --version` 输出
- **QEMU 版本**: `qemu-system-x86_64 --version` 输出
- **复现步骤**: 详细的操作步骤
- **预期行为**: 期望发生什么
- **实际行为**: 实际发生了什么
- **日志输出**: 相关的调试日志（如果是 Debug 构建）
- **截图**: 如果适用

### 提交功能请求

请描述：
- 功能的使用场景
- 期望的行为
- 是否有参考实现

---

## 社区沟通

- **GitHub Issues**: Bug 报告和功能请求
- **GitHub Discussions**: 技术讨论和问答
- **Pull Requests**: 代码贡献

---

## 项目结构速查

```
AuroraOS/
├── kernel/              # 内核源码（30+ C 文件）
│   ├── include/         # 公共头文件
│   ├── mem.c/h          # 内存管理（伙伴系统 + Slab）
│   ├── pagetable.c/h    # 四级页表 + COW
│   ├── sched.c/h        # 进程调度器
│   ├── syscall.c/h      # 系统调用（22 个）
│   ├── signal.c/h       # POSIX 信号
│   ├── vfs.c/h          # 虚拟文件系统
│   ├── console.c/h      # VGA 终端
│   ├── selftest.c       # 内核自测试
│   └── ...
├── arch/x86_64/          # 架构相关汇编
├── userspace/            # 用户态程序
├── docs/                 # 设计文档
├── scripts/              # 构建辅助脚本
├── .github/workflows/    # CI/CD
├── Makefile              # 构建系统
└── README.md             # 项目文档
```

详细架构说明见 [docs/architecture.md](docs/architecture.md)。

---

## 许可证

贡献的代码将采用与项目相同的 [MIT License](LICENSE)。

---

*AuroraOS 是一个学习型项目，欢迎所有水平的开发者参与