# AuroraOS Changelog

## v2.5.1 (2026-06-19) — Code Robustness & Error Handling Enhancement

### Bug Fixes
- **ramfs**: 为 `ramfs_add_file` 和 `ramfs_add_file_data` 添加 `name` 参数 NULL 检查
- **ramfs**: 添加重复文件名检测，防止同一文件被多次创建导致查找歧义
- **syscall**: 修复 `sys_open` 和 `sys_execve` 中 `kpath` 缓冲区可能未空终止的问题
- **user**: 修复 `create_user_task_from_entry` 部分映射失败时残留的页表映射（dangling PTE）
- **signal**: 为 `do_sys_kill` 添加 `pid < 0` 边界检查

### New Features
- **pagetable**: 新增 `unmap_page()` 函数，支持安全地解除单个页面的映射而不释放物理页

### Documentation
- **capability**: 添加关于 `cap_fd_*` 和 `fd_*` 两套 fd 系统共存风险的重要警告文档
- **syscall**: 为 `mmap` 固定映射区域限制添加文档说明

### Performance
- **console**: 优化 `console_clear_to_end` 使用批量 VGA 操作替代逐字符写入

### Build
- Release 和 Debug 构建均通过，零警告零错误

---

## v2.5.0 (2026-06-19) — Build Cleanliness & Robustness Audit

### Build System
- 修复 `entry.S` 汇编警告：`mov` 指令缺少后缀，改为 `movl`
- 为所有 12 个 `.S` 汇编文件添加 `.note.GNU-stack` 标记，消除链接器警告
- Release 和 Debug 构建均实现零警告零错误

### Robustness Audit
- 审计所有核心模块（18 个 C 文件）的 NULL 检查、边界条件和错误处理
- 验证内存分配器（buddy + slab）的 OOM 处理路径正确性
- 验证 COW 页面错误处理中 SIGSEGV 发送代替 panic 的修复
- 确认 VFS 路径遍历防护（`.` 和 `..` 拒绝）正常工作
- 确认管道实现的环形缓冲区边界处理正确
- 确认控制台输入缓冲区（INBUF_SIZE=256）的边界检查完整性

### Documentation
- 更新 CHANGELOG 记录所有构建修复和审计结果
- 更新调试报告记录编译警告修复过程

---

## v2.4.0 (2026-06-19) — Quality & Security Enhancement

### Code Quality
- 消除未使用的 `PTE_INTERMEDIATE_FLAGS` 宏
- 修复 10+ 处 `extern` 声明，改用正确的头文件包含（keyboard.c、elfloader.c、panic.c、print.c、exception.c、selftest.c、signal.c、syscall.c）
- 修复 `scancode_shifted` 数组声明大小不匹配问题
- 修复 `page_table_init` 中 EFER 读取的严格别名警告
- 为 `signal.h` 添加 `struct task_struct` 前向声明，消除参数列表中的声明警告

### Security
- **VFS 路径遍历防护**: `vfs_lookup` 拒绝 `.` 和 `..` 路径组件，防止目录遍历攻击
- **phys_to_virt 边界检查**: 添加身份映射范围验证（0-1GB），超出范围时 panic
- **ELF 段边界验证**: 验证 ELF 段虚拟地址在用户空间范围内（0x0-0x7FFFFFFFFFFF）

### New Features
- **cp 命令**: 新增文件复制命令，支持源文件读取并写入目标文件
- **welcome 命令**: 新增 `welcome` 命令，重新显示欢迎界面
- **每日提示系统**: Shell 启动时显示随机操作提示，共 15 条提示

### CI/CD
- 增强 GitHub Actions CI 工作流，添加详细的自检验证
- 新增 macOS 构建任务
- 添加构建产物保留策略

### Documentation
- 更新架构文档、API 文档和模块文档，反映最新安全增强
- 更新 CHANGELOG 记录所有变更

---

## v2.3.0 (2026-06-19) — Open Source Ready

### Documentation
- **CONTRIBUTING.md**: 新增贡献指南，包含代码风格规范、提交规范、PR 流程、测试要求、新功能添加指南
- **README.md**: 全面重写，新增详细安装步骤（Ubuntu/Debian/WSL2/macOS/Arch Linux）、环境要求、快速开始、调试模式、FAQ
- **CHANGELOG.md**: 更新至 v2.3.0，记录所有最近变更

### Bug Fixes
- **pagetable.c**: 修复 `clone_current_pml4` 使用 `read_cr3()` 而非 `kernel_cr3`，确保 COW 克隆使用当前进程的页表
- **pagetable.c**: 修复 PD 条目设置保留脏位导致 #PF 的问题，新增 `PTE_STRUCT_FLAGS` 宏用于中间表条目
- **syscall.c**: 修复 `mmap`/`mprotect` PROT 标志位掩码错误（PROT_WRITE→PTE_RW, PROT_EXEC→PTE_NX）
- **syscall.c**: 修复 `fork` 不继承信号处理器的问题，子进程现在分配 `signal_state` 并复制父进程的信号动作
- **signal.c**: 新增信号跳板代码写入用户栈，修复 `sigreturn` 上下文恢复
- **signal.c**: 添加 `#include "syscall.h"` 修复 `SYS_SIGRETURN` 未定义
- **keyboard.c**: 修复 Ctrl+C 向 shell 发送 SIGINT 的问题，改为仅发送给前台进程
- **shell.c**: 修复 `do_exit_cmd` 非阻塞 `console_getline` 读取残留数据
- **shell.c**: 添加 `#include "pagetable.h"` 修复 `exec_elf` 未定义
- **main.c**: 修复 `printk` 格式字符串问题

### Code Quality
- **console.c**: 优化 `console_clear`、`scroll_up`、`console_init` 为批量内存操作
- **elfloader.c**: 移除冗余 `extern` 声明，修复异常缩进
- **pagetable.c**: 移除重复宏定义，统一使用 `PTE_STRUCT_FLAGS`
- **selftest.c**: 优化自测试框架，添加 COW 克隆测试、页面表释放测试

### CI/CD
- **build.yml**: 增强 CI workflow，添加并行构建和产物上传

### Build System
- **Makefile**: 支持 `debug`/`release` 构建目标，自动检测交叉编译器

---

## v2.2.0 (2026-06-19) — Self-Reliance & Visual Design

### Third-Party Code Removal
- **Removed Limine bootloader** (`limine/` directory): AuroraOS uses GRUB2+Multiboot1,
  Limine was never actually used. All boot code is now 100% self-written.

### Multiboot1 Native Support
- **mem.c**: Added full Multiboot1 memory info parsing (E820 map + mem_lower/mem_upper).
  Previously only Multiboot2 was supported, causing fallback to hardcoded 64 MiB.
  Auto-detects MB1 vs MB2 from info structure header.
- **main.c**: Simplified boot sequence — phys_mem_init now handles MB1/MB2 detection internally.
- **mem.h**: Updated documentation to reflect MB1+MB2 dual support.

### Visual Design System (from Spec)
- **theme.h**: 3-layer design token system (VGA raw→semantic→component), 30+ SGR macros,
  spacing tokens, border characters, `console_color()` convenience macro
- **layout.h**: Reusable UI components — centered text, dividers, ASCII box drawing,
  status labels, progress bar, table layout, vertical padding/centering
- **main.c**: Fully tokenized boot screen (BOOT_*/STATUS_* tokens)
- **shell.c**: Fully tokenized shell (SHELL_*/PS_*/MEM_*/LOGIN_* tokens)
- **panic.c**: Fully tokenized panic screen (PANIC_* tokens)
- **console.c**: Magic number 0x07 → DEFAULT_VGA_ATTR macro

### Script Cleanup
- **embed_binary.py**: Fixed code duplication (import/argparse repeated twice)

### Documentation
- **README.md**: Complete rewrite with architecture diagram, feature list, design principles
- **CHANGELOG.md**: This file

### Build
- Zero C errors, zero C warnings (only harmless linker `.note.GNU-stack`)
- QEMU stable, 15/15 self-tests pass

---

## v2.1.0 (2026-06-19) — Quality & Stability Release

### Critical Fixes
- **fork**: Fixed child return path — child now correctly returns 0 via `is_fork_child`
  flag in task_struct and proper kernel stack setup with `syscall_return_point`.
- **sigreturn**: Moved saved RIP/RSP from global variables into per-task `signal_state`
  struct, eliminating thread-safety issue.
- **errno**: Added full POSIX errno definitions (`EINTR`, `EFAULT`, `EBADF`, `ENOENT`,
  `ENOMEM`, `ECHILD`, `EPIPE`, `ESRCH`) with per-thread errno variable.
- **vfs_lookup**: Fixed potential memory leak when kmalloc succeeds for name but later
  lookup fails — added name_consumed tracking.
- **pipe**: EINTR detection now properly sets `errno = EINTR` before returning -1.

### Code Quality
- **portio.h**: Extracted `outb`/`inb` from 5 duplicate definitions into shared header
  (`console.c`, `print.c`, `keyboard.c`, `irq.c`, `pit.c`).
- **kstdio.h**: Extracted `itoa`/`uitoa`/`uitoa_hex` from 4+ duplicate implementations
  (`main.c`, `shell.c`, `explain.c`, `panic.c`).
- **explain.c**: Refactored all manual itoa loops to use kstdio.h utilities.
- **syscall.c**: Removed unused `extern current_tf_signal` declaration.
- **elfloader.c**: Fixed abnormal indentation on `register_elf_pml4` call.

### New Features
- **libc**: Added `malloc`/`free`/`calloc`/`realloc` with coalescing heap allocator.
  Added `sprintf`, `atoi`, `puts`, `getpid`, `fork`, `waitpid`, `exit` wrappers.
  Extended `printf` with `%u`, `%x` format specifiers.
- **userspace shell**: Enhanced with `fork`, `ps`, `getpid`, `clear`, `exit` commands
  and improved prompt.
- **Build system**: Added `debug`/`release` targets, automatic cross-compiler detection,
  and `help` target.

### Documentation
- Added `CHANGELOG.md` (this file).

---

## v2.0.0 (2026-06-13) — Production Readiness

### Architecture
- x86_64 Multiboot1 boot with 32→64-bit mode self-transition
- Custom GDT with TSS descriptors (runtime-built)
- 4-level page tables with NX bit and COW support

### Memory Management
- Buddy system physical page allocator (MAX_ORDER=10, 256 MiB)
- Slab allocator (8 size classes, 32B–4096B)
- E820 memory map parsing

### Process Management
- Preemptive round-robin scheduler with priority/time_slice fields
- 5-state process model (RUNNING/READY/BLOCKED/ZOMBIE/DEAD)
- Process tree with parent→child tracking and init adoption
- Blocking waitpid
- Fork with COW page table cloning

### System Calls
- read(0), write(1), open(2), close(3), fork(57), pipe(22)
- getpid(39), kill(62), sigaction(13), sigreturn(15)
- execve(59), exit(60), waitpid(61)

### Signals
- SIGINT(2), SIGKILL(9), SIGTERM(15), SIGCHLD(17)
- User-defined handlers with sigframe on user stack
- sigreturn restoration

### File System
- VFS with dentry cache and multi-level path resolution
- RamFS with read/write support
- Anonymous pipes with ring buffer

### Drivers
- PS/2 keyboard with modifier keys and multi-byte scancodes
- VGA text mode console with ANSI escape sequences
- PIT timer (100 Hz)
- Serial port (COM1 115200)

### Shell
- Kernel shell with ANSI colors, login prompt
- Commands: help, ls, cat, echo, exec, ps, exit, wait, kill, mem, clear, about

### Testing
- 15/15 self-tests passing
- Zero exceptions, zero panics

---

## v1.0.0 — Initial Release

- Basic kernel with Multiboot1 boot
- Bitmap physical memory allocator
- Simple task switching
- VGA text mode output
- Basic keyboard input
