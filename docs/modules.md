# AuroraOS 模块功能说明文档

## 1. 内存管理模块 (mem.c / pagetable.c)

### 1.1 物理内存管理 (mem.c)

**职责**: 管理物理内存的分配与回收。

**核心组件**:
- **Buddy 伙伴系统**: 管理 0~MAX_ORDER 阶的连续物理页分配
  - `alloc_pages(order)`: 分配 2^order 个连续页
  - `free_pages(ptr, order)`: 释放连续页
  - `alloc_page()` / `free_page()`: 单页分配/释放便捷接口
- **Slab 分配器**: 内核堆小对象分配
  - `kmalloc(size)`: 分配内核堆内存（≤4096 字节使用 Slab，>4096 使用页分配）
  - `kfree(ptr)`: 释放内核堆内存
- **页描述符**: `struct page` 跟踪每页的状态、引用计数、所属 Slab 缓存

**依赖关系**:
- 依赖 Multiboot 信息结构获取物理内存布局
- 被几乎所有内核模块依赖

**关键数据结构**:
```
struct page {
    flags     (FREE/RESERVED/KERNEL/SLAB)
    order     (分配阶)
    ref_count (COW 引用计数)
    phys_addr (物理地址)
    buddy     (伙伴指针)
    next      (空闲链表)
    slab_cache(所属 Slab)
}
```

### 1.2 页表管理 (pagetable.c)

**职责**: 管理 x86_64 4 级分页，支持 COW 和缺页处理。

**核心功能**:
- `page_table_init()`: 初始化内核页表，建立 0~1GB 恒等映射
- `map_page(pml4, va, pa, flags)`: 映射单个 4KB 页面
- `map_user_page()`: 映射用户可访问页面
- `map_range()`: 映射连续地址范围
- `clone_current_pml4()`: COW 感知深拷贝用户页表
- `free_pagetable()`: COW 感知递归释放页表
- `pf_handler_c()`: 缺页异常处理（COW 复制、延迟分配）
- `split_huge_page()`: 拆分 2MB 大页为 4KB 页表
- `exec_elf()`: 加载 ELF 可执行文件并创建进程
- `rodata_protect()`: 标记只读数据段为只读

**页表层级**:
```
PML4 (Level 4) → PDPT (Level 3) → PD (Level 2) → PT (Level 1) → Page (4KB)
```

**COW 机制**:
1. fork() 调用 `clone_current_pml4()` 复制页表
2. 所有用户页标记为只读，ref_count 递增
3. 写入触发 #PF 异常
4. `pf_handler_c()` 分配新页，复制数据，更新页表
5. ref_count 递减，为 0 时释放原页

**关键标志位**:
```
PTE_PRESENT (0x001)  - 页面存在
PTE_RW      (0x002)  - 可写
PTE_USER    (0x004)  - 用户可访问
PTE_PS      (0x080)  - 大页标志
PTE_NX      (1<<63)  - 禁止执行
```

---

## 2. 进程调度模块 (sched.c)

**职责**: 进程创建、调度、销毁。

**核心功能**:
- `sched_init()`: 初始化调度器，创建 idle 和 init 任务
- `fork()`: 创建进程副本（COW）
- `schedule()`: 调度器主函数，选择下一个运行任务
- `yield()`: 主动让出 CPU
- `do_exit_current()`: 退出当前进程
- `waitpid()`: 等待子进程退出

**任务状态机**:
```
READY ──→ RUNNING ──→ BLOCKED
  ↑                    │
  └────────────────────┘
         │
         └──→ ZOMBIE ──→ DEAD
```

**调度策略**: 轮转调度 (Round Robin)
- 时间片: 10ms
- 所有 READY 状态任务按链表顺序轮转

**关键数据结构**:
```c
struct task_struct {
    pid, name, state
    cr3 (页表物理地址)
    rsp (内核栈指针)
    context (保存的寄存器)
    next (链表指针)
    files (文件描述符表)
    sigactions (信号处理表)
    pending_signals (待处理信号)
    t_errno (线程 errno)
};
```

---

## 3. 虚拟文件系统模块 (vfs.c / fs.c / ramfs.c)

**职责**: 提供统一的文件系统接口，支持多种文件系统实现。

### 3.1 VFS 抽象层 (vfs.c)

**核心接口**:
- `vfs_open(path, flags)`: 打开文件
- `vfs_read(file, buf, len)`: 读取文件
- `vfs_write(file, buf, len)`: 写入文件
- `vfs_close(file)`: 关闭文件
- `vfs_readdir()`: 读取目录
- `vfs_get_root_sb()`: 获取根超级块

**安全特性**:
- 路径遍历防护：拒绝 "." 和 ".." 路径组件
- 组件名长度限制：最大 255 字节

**关键数据结构**:
```
super_block (超级块)
  └── root_dentry (根目录项)
        └── child → next → ... (目录项链表)
              └── inode (索引节点)
```

### 3.2 RamFS 实现 (ramfs.c)

**职责**: 基于内存的简单文件系统实现。

**核心功能**:
- `ramfs_init()`: 初始化 RamFS
- `ramfs_create()`: 创建文件
- `ramfs_read()`: 读取文件内容
- `ramfs_write()`: 写入文件内容
- `ramfs_lookup()`: 查找目录项

### 3.3 文件描述符管理 (file.c)

**核心功能**:
- `fd_alloc()`: 分配文件描述符
- `fd_install()`: 安装文件描述符
- `fd_get()`: 获取文件对象
- `fd_free()`: 释放文件描述符
- `dup_fd_table()`: 复制文件描述符表（fork 时使用）

---

## 4. 系统调用模块 (syscall.c / syscall_entry.c)

**职责**: 系统调用分发与实现。

**核心功能**:
- `syscall_init()`: 配置 MSR 寄存器（STAR/LSTAR/CSTAR/SFMASK）
- `handle_syscall()`: 系统调用分发器
- 各 sys_* 函数实现

**系统调用分发流程**:
```
用户程序: syscall 指令
    ↓
syscall_entry.S: 保存上下文
    ↓
syscall.c: handle_syscall(num, a1..a6)
    ↓
switch(num) → sys_read/sys_write/sys_fork/...
    ↓
返回用户空间
```

**支持的完整系统调用列表**:
- I/O: read, write, open, close, fstat, lseek, getdents
- 进程: fork, execve, exit, getpid, waitpid
- 内存: mmap, mprotect
- 信号: kill, sigaction, sigreturn
- 管道: pipe
- 文件描述符: dup, dup2

---

## 5. 信号模块 (signal.c)

**职责**: POSIX 信号递送与处理。

**核心功能**:
- `do_sys_kill()`: 向进程发送信号
- `do_sys_sigaction()`: 设置信号处理函数
- `signal_deliver()`: 在返回用户态前递送信号
- `signal_setup_frame()`: 在用户栈设置信号处理帧
- `signal_restore()`: 从信号处理返回

**信号处理流程**:
```
1. 发送者调用 kill(pid, sig)
2. 目标进程 pending_signals 置位
3. 目标进程从内核态返回用户态时检查
4. 如果有待处理信号:
   a. 在用户栈保存上下文
   b. 设置 RIP 为信号处理函数
   c. 返回用户态执行处理函数
5. 处理函数返回时调用 sigreturn
6. 恢复原始上下文继续执行
```

---

## 6. ELF 加载器 (elfloader.c)

**职责**: 解析并加载 ELF 可执行文件。

**核心功能**:
- `elf_load()`: 加载 ELF 文件，创建进程页表，映射段
- 验证 ELF 魔数、架构、入口点
- 支持 ET_EXEC 和 ET_DYN 类型
- 段权限映射（R/W/X → PTE 标志）

**安全验证**:
- ELF 魔数验证（0x7F 'E' 'L' 'F'）
- 架构验证（x86_64）
- 入口点验证（必须在用户地址空间内）
- 段对齐检查

---

## 7. 控制台模块 (console.c)

**职责**: VGA 文本模式显示，ANSI 转义序列支持，行编辑输入。

**核心功能**:
- `console_init()`: 初始化 VGA 缓冲区
- `console_putc()`: 输出单个字符（支持 \n, \r, \b, \t）
- `console_write()`: 输出字符串
- `console_write_ansi()`: 输出带 ANSI 转义序列的字符串
- `console_clear()`: 清屏
- `console_set_cursor()` / `console_get_cursor()`: 光标控制
- `console_input_char()`: 输入字符处理（行编辑）
- `console_getline()`: 获取完整输入行
- `console_replace_line()`: 替换当前输入行（用于历史导航和补全）
- `console_set_history_callback()`: 设置历史导航回调
- `console_set_tab_complete_callback()`: 设置 Tab 补全回调

**行编辑功能**:
- 左右箭头: 移动光标
- Home/End: 行首/行尾
- Backspace/Delete: 删除字符
- 插入模式: 在光标位置插入字符
- 上下箭头: 历史命令导航

**ANSI 转义序列支持**:
- SGR: 颜色/样式（前景色、背景色、粗体、重置）
- 光标移动: 上下左右、定位
- 屏幕操作: 清屏、清行
- 模式设置: 显示/隐藏光标

---

## 8. 键盘驱动模块 (keyboard.c)

**职责**: PS/2 键盘中断处理，修饰键支持，多字节扫描码处理。

**核心功能**:
- `keyboard_init()`: 初始化键盘状态
- `keyboard_c_handler()`: 键盘中断处理函数

**修饰键支持**:
- Shift (左/右): 大小写和符号映射
- Ctrl (左/右): 控制字符生成（Ctrl+C → SIGINT）
- Alt (左/右): 状态跟踪
- Caps Lock: 字母大小写反转

**E0 前缀处理**:
- 箭头键: 生成 ANSI 转义序列
- Home/End/Delete: 生成对应转义序列
- 右 Ctrl/右 Alt: 修饰键状态更新

---

## 9. Shell 模块 (shell.c)

**职责**: 内核内置命令行 Shell。

**核心功能**:
- 登录界面（带时间显示、用户验证）
- 欢迎横幅
- 命令解析与执行
- 命令历史（环形缓冲区，32 条）
- Tab 命令名/文件名补全
- 主题切换（dark/light/high-contrast）
- 锁屏功能
- 退出确认对话框
- 每日提示系统（15 条操作提示）
- 文件复制命令（cp）

**支持的命令**:

| 类别 | 命令 | 功能 |
|------|------|------|
| 系统信息 | help, about, sysinfo, uname, uptime, date, free, df, clear, welcome | 帮助、关于、系统仪表盘、清屏、欢迎界面 |
| 进程管理 | ps, exec, wait, kill, exit | 进程列表、执行程序、等待子进程、发送信号、退出 |
| 文件系统 | ls, ll, la, cat, echo, cp, touch, rm, pwd, cd, mkdir, wc, head, tail | 文件操作、目录管理、文本统计 |
| 内存 | mem | 内存使用情况 |
| 调试 | perf, mod, env, which | 性能统计、模块管理、环境变量、命令定位 |
| 个性化 | theme, a11y, history, lock, date | 主题切换、无障碍、历史、锁屏、日期时间 |

---

## 10. procfs 虚拟文件系统模块 (procfs.c)

**职责**: 提供 `/proc` 虚拟文件系统，运行时导出内核信息（受 CoolPotOS 启发）。

**核心功能**:
- `procfs_init()`: 注册 procfs 文件系统类型
- 静态文件读取：cpuinfo, meminfo, uptime, version, mounts
- **IRQ 追踪**: `/proc/interrupts` — 256 向量中断计数器（受 CoolPotOS 启发）
- **文件系统列表**: `/proc/filesystems` — 已注册的文件系统类型
- **内核命令行**: `/proc/cmdline` — 引导参数
- **内核日志**: `/proc/kmsg` — 日志环形缓冲区内容（受 CoolPotOS 内核日志子系统启发）
- **进程信息**: `/proc/self/stat` — 当前运行进程状态

**关键数据结构**:
```c
struct procfs_entry {
    char *name;
    procfs_read_fn read;
    int is_dir;
};
```

---

## 11. 性能监控模块 (perf.c)

**职责**: 内核性能计数器与 IRQ 追踪（受 CoolPotOS /proc/interrupts 启发）。

**核心功能**:
- `perf_init()`: 初始化性能计数器，TSC 频率校准
- `perf_start()` / `perf_end()`: 单次操作延迟测量
- `perf_irq_inc()`: 递增 IRQ 向量计数器
- `perf_irq_dump()`: 输出所有 IRQ 计数（用于 `/proc/interrupts`）
- 8 类性能事件：上下文切换、系统调用、缺页、COW、内存分配/释放、中断

**关键数据结构**:
```c
struct perf_counter {
    uint64_t count;        // 事件计数
    uint64_t total_latency; // 累积延迟
    uint64_t min_latency;   // 最小延迟
    uint64_t max_latency;   // 最大延迟
    const char *name;       // 计数器名称
};

struct irq_counter {
    uint64_t count;        // 中断计数
    const char *name;      // IRQ 名称
};
```

---

## 12. 内核模块签名模块 (module_sign.c)

**职责**: 内核模块签名验证演示（受 CoolPotOS ECC 模块密钥验证机制启发）。

**重要说明**: 当前为演示/占位实现，尚未启用：
- 使用 XOR 滚动哈希（非 SHA-256），硬编码占位密钥
- `MODULE_SIGN_CHECK` 宏未在任何构建配置中定义
- `module_sign_verify()` 未接入 `module_load()` 流程
- 当前状态下不提供任何实际安全保护

**核心功能**:
- `module_sign_verify()`: 验证模块签名头（演示用）
- `module_sign_is_enabled()`: 检查签名验证是否启用（当前恒返回 0）
- 编译时可选：`MODULE_SIGN_CHECK` 宏控制（未启用）

**签名格式**:
```
+------------------+
| 模块代码 (.text)  |
+------------------+
| 模块数据 (.data)  |
+------------------+
| 签名头 (64 bytes) |
|  - magic          |
|  - version        |
|  - signature[64]  |
+------------------+
```

---

## 13. 内核日志模块 (log.c)

**职责**: 分级日志输出与环形缓冲区管理（受 CoolPotOS 终端会话子系统启发）。

**核心功能**:
- `log_printf()`: 分级日志输出（DEBUG/INFO/WARN/ERR）
- `log_ring_read()`: 读取环形缓冲区内容（用于 `/proc/kmsg`）
- 环形缓冲区：LOG_BUF_SIZE 字节，循环写入

---

## 14. 模块依赖关系图

```
main.c
 ├── console.c / console.h
 ├── mem.c / mem.h
 │    └── pagetable.c / pagetable.h
 │         └── elfloader.c / elf.h
 ├── sched.c / sched.h
 │    └── signal.c / signal.h
 ├── irq.c
 │    ├── keyboard.c
 │    ├── pit.c / pit_handler.c
 │    └── exception.c
 ├── vfs.c / vfs.h
 │    ├── fs.c / fs.h
 │    ├── ramfs.c
 │    ├── file.c
 │    └── pipe.c
 ├── syscall.c / syscall.h / syscall_entry.c
 ├── shell.c / shell.h
 │    ├── layout.h
 │    ├── include/theme.h
 │    └── explain.c
 ├── capability.c / capability.h
 ├── log.c
 ├── print.c
 ├── string.c
 └── selftest.c
```