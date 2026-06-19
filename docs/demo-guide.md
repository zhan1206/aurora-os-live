# AuroraOS 多媒体教程与演示指南

> **版本**: v3.0.0 | **更新日期**: 2026-06-19 | **语言**: 简体中文

本文档提供 AuroraOS 功能演示的多媒体制作指南，包括 GIF 录制工具的使用方法、核心功能演示脚本、以及常见问题演示场景。面向项目维护者、内容创作者和开源布道者。

---

## 目录

1. [如何制作功能演示 GIF](#1-如何制作功能演示-gif)
2. [核心功能演示脚本](#2-核心功能演示脚本)
3. [常见问题演示](#3-常见问题演示)

---

## 1. 如何制作功能演示 GIF

### 1.1 推荐工具链

#### 方案 A: peek + screenkey（Linux 推荐）

**peek** 是一款简单易用的 GIF 屏幕录制工具，**screenkey** 可以在屏幕上实时显示按键操作。

```bash
# Ubuntu / Debian 安装
sudo apt install peek screenkey

# 使用步骤
# 1. 启动 screenkey（显示按键）
screenkey --font-size medium --position bottom &

# 2. 启动 peek
peek

# 3. 在 peek 窗口中调整录制区域，点击"录制"
# 4. 在 QEMU 窗口中执行演示操作
# 5. 点击"停止"，保存为 GIF
```

**peek 推荐设置**:
- 帧率: 10 FPS（终端操作足够）
- 窗口大小: 720×480 或 800×600
- 鼠标跟随: 关闭（终端操作不需要）
- 延迟启动: 3 秒（给你准备时间）

#### 方案 B: asciinema + agg（终端录制，跨平台）

**asciinema** 录制纯文本终端会话，**agg** 将录制的 `.cast` 文件转换为 GIF。

```bash
# Ubuntu / Debian 安装
sudo apt install asciinema

# macOS 安装
brew install asciinema

# 安装 agg（cast → GIF 转换器）
# 从 https://github.com/asciinema/agg/releases 下载

# 录制
asciinema rec auroraos-demo.cast

# 转换为 GIF（带主题）
agg --theme dracula --font-size 14 auroraos-demo.cast auroraos-demo.gif
```

**优点**: 文件极小（`.cast` 是文本格式），可以后期编辑，跨平台。

#### 方案 C: FFmpeg + x11grab（高级用户）

```bash
# 录制 QEMU 窗口区域
ffmpeg -f x11grab -video_size 720x480 -i :0.0+100,100 \
       -framerate 10 -t 30 output.mp4

# 转换为 GIF
ffmpeg -i output.mp4 -vf "fps=10,scale=720:-1:flags=lanczos" output.gif
```

#### 方案 D: Windows 用户

推荐使用 **ScreenToGif**（免费开源）:
1. 下载: https://www.screentogif.com/
2. 选择"录像机"模式
3. 框选 QEMU 窗口区域
4. 录制完成后可编辑裁剪、调整帧率、添加文字
5. 导出为 GIF

### 1.2 录制最佳实践

1. **分辨率**: 推荐 720×480 或 800×600，确保文字清晰可读
2. **帧率**: 10 FPS 足够，终端动画不需要高帧率
3. **配色**: 使用 AuroraOS 的 Dark 主题（`theme dark`），在黑色背景上录制效果最佳
4. **字幕**: 在 GIF 底部添加文本说明操作步骤
5. **时长**: 单个 GIF 控制在 15-30 秒，文件大小 < 5MB
6. **光标**: 在 QEMU 中可以使用 `-nographic` 模式通过串口操作，避免光标干扰

### 1.3 QEMU 录制配置

```bash
# 启动 QEMU 的推荐录制配置
qemu-system-x86_64 -m 256M -cdrom os.iso \
    -vga std \
    -display gtk,zoom-to-fit=on \
    -name "AuroraOS v3.0.0 Demo"

# 无图形模式（用于 asciinema 录制）
qemu-system-x86_64 -m 256M -cdrom os.iso \
    -nographic \
    -serial mon:stdio
```

---

## 2. 核心功能演示脚本

### Demo 1: 系统启动全过程

#### 目的

展示 AuroraOS 从 GRUB 引导到 Shell 就绪的完整启动过程，包括进度条、自检结果和欢迎界面。

#### 操作步骤

```
1. 启动 QEMU（或插入 ISO 后启动虚拟机）
2. 等待 GRUB 菜单自动选择（或按 Enter）
3. 观察启动画面:
   - Aurora 极光主题 Logo
   - 版本号: AuroraOS v3.0.0
   - 构建日期: 2026-06-19
4. 观察初始化进度条:
   - 串口 (COM1 115200 8N1)
   - 物理内存: XXX MiB
   - Slab 分配器 (8 size classes)
   - ASLR initialized (xorshift64 PRNG)
   - Page tables (4-level, NX enabled)
   - Scheduler (RR + idle task + PID bitmap)
   - SYSCALL/SYSRET MSRs configured
   - IDT + PIC remap + keyboard driver
   - SMP (detected CPUs)
   - Read-only data segment
   - PIT timer (100 Hz)
   - Performance counters
   - Sysctl interface
   - VFS + RamFS mounted
   - Module loader (kernel symbol table)
5. 观察自检结果:
   - Buddy Allocator Tests: 3 项
   - Slab Allocator Tests: 2 项
   - Page Table Tests: 3 项
   - Scheduler Tests: 3 项
   - VFS / RamFS Tests: 3 项
   - Pipe Tests: 2 项
   - 总计: 16/16 PASS
6. 进入 Shell 登录界面
```

#### 预期输出

```
   .  *  ~  .  *  ~  .  *  ~  .  *  ~  .  *  ~  .
  *                                               *
 ~         A   U   R   O   R   A   O  S          ~
  *                                               *
   ~  *  .  ~  *  .  ~  *  .  ~  *  .  ~  *  .  ~

    A Self-Built x86_64 Operating System

              AuroraOS v3.0.0
              2026-06-19 08:00
    (c) 2026 AuroraOS Contributors — MIT License

··························································

             Initializing system...

[OK] Serial port (COM1 115200 8N1)
[████░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░]  8%
[OK] Physical memory: 256 MiB
[██████░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░] 15%
[OK] Slab allocator (8 size classes)
[████████░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░] 23%
[OK] ASLR initialized (xorshift64 PRNG)
[████████░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░] 23%
[OK] Page tables (4-level, NX enabled)
[██████████░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░] 38%
[OK] Scheduler (RR + idle task + PID bitmap)
[████████████░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░] 46%
[OK] SYSCALL/SYSRET MSRs configured
[████████████░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░] 46%
[OK] IDT + PIC remap + keyboard driver
[██████████████░░░░░░░░░░░░░░░░░░░░░░░░░░░░] 54%
[OK] SMP (detected CPUs)
[████████████████░░░░░░░░░░░░░░░░░░░░░░░░░░] 62%
[OK] Read-only data segment
[████████████████░░░░░░░░░░░░░░░░░░░░░░░░░░] 69%
[OK] PIT timer (100 Hz)
[████████████████░░░░░░░░░░░░░░░░░░░░░░░░░░] 69%
[OK] Performance counters
[██████████████████░░░░░░░░░░░░░░░░░░░░░░░░] 77%
[OK] Sysctl interface
[██████████████████░░░░░░░░░░░░░░░░░░░░░░░░] 77%
[OK] VFS + RamFS mounted
[████████████████████░░░░░░░░░░░░░░░░░░░░░░] 85%
[OK] Module loader (kernel symbol table)
[████████████████████░░░░░░░░░░░░░░░░░░░░░░] 85%

              [ System Ready ]

──────────────────────────────────────────────────────────

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

AuroraOS Shell — Type 'help' for commands
```

#### 关键截图点

| 时间点 | 截图内容 | 说明 |
|--------|----------|------|
| 0s | GRUB 启动菜单 | 展示 Multiboot1 引导 |
| 2s | Aurora 极光 Logo | 展示品牌视觉 |
| 5s | 初始化进度条 50% | 展示子系统初始化 |
| 8s | [ System Ready ] | 启动完成 |
| 10s | 自检结果 | 16/16 PASS |
| 12s | Shell 提示符 | 系统就绪 |

---

### Demo 2: 文件系统操作

#### 目的

展示 AuroraOS 文件系统的基本操作：查看、创建、读写、复制、删除文件。

#### 操作步骤

```
# 1. 查看根目录
ls /

# 2. 创建文件
touch hello.txt
touch readme.txt
touch config.cfg

# 3. 再次查看目录
ls /

# 4. 写入文件内容
echo "Hello from AuroraOS!" > hello.txt

# 5. 查看文件内容
cat hello.txt

# 6. 追加内容
echo "This is line 2" > hello.txt

# 7. 再次查看
cat hello.txt

# 8. 复制文件
cp hello.txt hello_backup.txt

# 9. 查看复制结果
cat hello_backup.txt

# 10. 删除原文件
rm hello.txt

# 11. 确认删除
ls /

# 12. 查看系统信息
sysinfo
```

#### 预期输出

```
aurora> ls /
  hello.txt          (12 bytes)
  readme.txt         (0 bytes)
  config.cfg         (0 bytes)

aurora> cat hello.txt
Hello from AuroraOS!

aurora> cat hello.txt
This is line 2

aurora> cat hello_backup.txt
This is line 2

aurora> ls /
  readme.txt         (0 bytes)
  hello_backup.txt   (15 bytes)
  config.cfg         (0 bytes)
```

#### 关键截图点

| 时间点 | 截图内容 | 说明 |
|--------|----------|------|
| 0s | 空目录 ls 结果 | 初始状态 |
| 5s | touch 创建文件后的 ls | 文件创建 |
| 10s | echo 写入 + cat 查看 | 文件读写 |
| 15s | cp 复制 + cat 验证 | 文件复制 |
| 20s | rm 删除后的 ls | 文件删除 |

---

### Demo 3: 进程管理

#### 目的

展示 AuroraOS 进程管理功能：查看进程列表、执行程序、等待子进程、发送信号。

#### 操作步骤

```
# 1. 查看当前进程列表
ps

# 2. 执行用户态程序
exec /hello

# 3. 再次查看进程列表（观察新进程）
ps

# 4. 查看进程详细信息
sysinfo

# 5. 执行多个程序
exec /hello

# 6. 等待所有子进程结束
wait

# 7. 查看内存使用
mem

# 8. 使用 kill 发送信号
# (先 ps 查看某个进程 PID，然后 kill)
ps
kill 3
ps
```

#### 预期输出

```
aurora> ps
PID   STATE   NAME
0     R       idle
1     R       init
2     R       shell
3     R       task1
4     R       task2

aurora> exec /hello
Hello from user space!
Process 5 exited with code 0

aurora> ps
PID   STATE   NAME
0     R       idle
1     R       init
2     R       shell
3     Z       task1    (zombie)
4     Z       task2    (zombie)

aurora> mem
Total: 256 MiB | Free: 224 MiB | Used: 32 MiB
```

#### 关键截图点

| 时间点 | 截图内容 | 说明 |
|--------|----------|------|
| 0s | 初始 ps 列表 | 展示 idle/init/shell/task1/task2 |
| 5s | exec /hello 输出 | 用户态程序执行 |
| 10s | ps 显示 ZOMBIE 状态 | 进程生命周期 |
| 15s | mem 内存统计 | 内存使用情况 |
| 20s | kill 后的 ps | 信号处理效果 |

---

### Demo 4: 性能监控

#### 目的

展示 AuroraOS v3.0 新增的性能计数器功能，包括上下文切换、系统调用、缺页等统计。

#### 操作步骤

```
# 1. 执行一些操作产生负载
exec /hello
exec /hello
echo "test" > test.txt
cat test.txt
ls /

# 2. 查看性能统计
perf

# 3. 继续执行更多操作
exec /hello
cp test.txt test2.txt
rm test.txt
rm test2.txt

# 4. 再次查看性能统计
perf

# 5. 重置计数器
perf reset

# 6. 确认重置
perf
```

#### 预期输出

```
aurora> perf
=== Performance Counters ===
Context Switches:    156
Syscall Count:       89
Syscall Latency:     avg=1.2us  max=15us  min=0.4us
Page Faults:         23
COW Count:           5
Malloc Count:        42
Free Count:          38
IRQ Count:           1204
Uptime:              12.5s

aurora> perf reset
Performance counters reset.

aurora> perf
=== Performance Counters ===
Context Switches:    0
Syscall Count:       0
Syscall Latency:     avg=0us  max=0us  min=0us
Page Faults:         0
COW Count:           0
Malloc Count:        0
Free Count:          0
IRQ Count:           3
Uptime:              15.2s
```

#### 关键截图点

| 时间点 | 截图内容 | 说明 |
|--------|----------|------|
| 0s | 执行操作产生负载 | 触发性能事件 |
| 5s | perf 命令输出 | 完整性能统计 |
| 10s | perf reset 后输出 | 清零效果 |

---

### Demo 5: 模块加载

#### 目的

展示 AuroraOS v3.0 新增的动态模块加载功能：列出模块、加载模块、卸载模块。

#### 操作步骤

```
# 1. 查看当前已加载模块
mod list

# 2. 加载示例模块
mod load /mod_sample

# 3. 再次查看模块列表
mod list

# 4. 查看模块加载后的效果
# (mod_sample 会打印初始化消息)

# 5. 卸载模块
mod unload mod_sample

# 6. 确认卸载
mod list
```

#### 预期输出

```
aurora> mod list
No modules loaded.

aurora> mod load /mod_sample
Loading module: /mod_sample
  Module loaded at 0xFFFF800000200000
  Resolving symbols...
  Module 'mod_sample' initialized successfully.
  [mod_sample] Hello from kernel module!

aurora> mod list
  Name            State    Base Address        Size
  mod_sample      LIVE     0xFFFF800000200000  4096

aurora> mod unload mod_sample
Unloading module: mod_sample
  [mod_sample] Goodbye from kernel module!
  Module 'mod_sample' unloaded successfully.

aurora> mod list
No modules loaded.
```

#### 关键截图点

| 时间点 | 截图内容 | 说明 |
|--------|----------|------|
| 0s | 空模块列表 | 初始状态 |
| 5s | mod load 输出 | 加载过程 |
| 10s | 模块列表显示 LIVE | 加载成功 |
| 15s | mod unload + 空列表 | 卸载成功 |

---

### Demo 6: 主题切换

#### 目的

展示 AuroraOS 的主题系统：Dark、Light、High Contrast 三种模式的切换效果。

#### 操作步骤

```
# 1. 默认 Dark 主题下显示一些内容
help
echo "AuroraOS Theme Demo"
sysinfo

# 2. 切换到 Light 主题
theme light
help
echo "AuroraOS Theme Demo"
sysinfo

# 3. 切换到 High Contrast 主题
theme hc
help
echo "AuroraOS Theme Demo"
sysinfo

# 4. 切换回 Dark 主题
theme dark
help

# 5. 查看无障碍设置
a11y
```

#### 预期输出

```
aurora> theme light
Theme switched to: Light

aurora> theme hc
Theme switched to: High Contrast

aurora> theme dark
Theme switched to: Dark

aurora> a11y
Accessibility settings:
  High Contrast: OFF
  Reduced Motion: OFF
```

#### 关键截图点

| 时间点 | 截图内容 | 说明 |
|--------|----------|------|
| 0s | Dark 主题效果 | 深色背景 + 极光配色 |
| 5s | Light 主题效果 | 浅色背景 |
| 10s | HC 主题效果 | 高对比度黑白 |
| 15s | 三主题并排对比 | 展示所有模式 |

---

## 3. 常见问题演示

### FAQ Demo 1: 命令不存在时的错误处理

#### 目的

展示当用户输入不存在的命令时，Shell 如何优雅地处理错误。

#### 操作步骤

```
# 1. 输入不存在的命令
foobar

# 2. 输入空命令（直接按 Enter）

# 3. 输入部分命令后按 Tab（无匹配）
xyz

# 4. 查看帮助
help

# 5. 输入正确命令
ls /
```

#### 预期输出

```
aurora> foobar
Unknown command: foobar
Type 'help' for available commands.

aurora>
aurora> xyz
Unknown command: xyz
Type 'help' for available commands.

aurora> help
Available commands:
  help      - Show this help
  about     - About AuroraOS
  sysinfo   - System dashboard
  clear     - Clear screen
  welcome   - Show welcome screen
  ps        - Process list
  exec      - Execute program
  wait      - Wait for child
  kill      - Send signal
  exit      - Exit shell
  ls        - List files
  cat       - Show file content
  echo      - Print text
  touch     - Create empty file
  rm        - Remove file
  cp        - Copy file
  mem       - Memory usage
  theme     - Switch theme (dark/light/hc)
  history   - Command history
  lock      - Lock screen
  date      - Show date/time
  perf      - Performance counters
  mod       - Module management

Type 'help <command>' for detailed help.
```

#### 关键截图点

| 时间点 | 截图内容 | 说明 |
|--------|----------|------|
| 0s | 输入错误命令 | 错误输入 |
| 3s | 错误提示信息 | Unknown command |
| 5s | help 命令输出 | 可用命令列表 |
| 8s | 正确命令执行 | 正常操作 |

---

### FAQ Demo 2: 文件不存在时的错误处理

#### 目的

展示文件操作中的错误处理机制。

#### 操作步骤

```
# 1. 尝试查看不存在的文件
cat nonexistent.txt

# 2. 尝试复制不存在的文件
cp nonexistent.txt backup.txt

# 3. 尝试删除不存在的文件
rm nonexistent.txt

# 4. 创建文件后正常操作
touch real.txt
echo "content" > real.txt
cat real.txt

# 5. 重复创建同名文件
touch real.txt
```

#### 预期输出

```
aurora> cat nonexistent.txt
Error: File not found: nonexistent.txt

aurora> cp nonexistent.txt backup.txt
Error: Source file not found: nonexistent.txt

aurora> rm nonexistent.txt
Error: File not found: nonexistent.txt

aurora> touch real.txt
File created: real.txt

aurora> echo "content" > real.txt
aurora> cat real.txt
content

aurora> touch real.txt
File already exists: real.txt
```

#### 关键截图点

| 时间点 | 截图内容 | 说明 |
|--------|----------|------|
| 0s | cat 不存在的文件 | 文件不存在错误 |
| 5s | cp/rm 不存在的文件 | 统一错误处理 |
| 10s | 正常文件操作 | 对比正确操作 |
| 15s | 重复创建文件 | 重复文件检测 |

---

### FAQ Demo 3: 进程信号处理

#### 目的

展示信号发送和进程终止的完整流程。

#### 操作步骤

```
# 1. 查看当前进程
ps

# 2. 执行一个程序（让它运行一段时间）
exec /hello

# 3. 查看进程状态
ps

# 4. 向进程发送信号
kill 5

# 5. 确认进程状态变化
ps

# 6. 等待子进程回收
wait

# 7. 再次查看
ps
```

#### 预期输出

```
aurora> ps
PID   STATE   NAME
0     R       idle
1     R       init
2     R       shell

aurora> exec /hello
Hello from user space!
Process 5 started

aurora> ps
PID   STATE   NAME
0     R       idle
1     R       init
2     R       shell
5     R       hello

aurora> kill 5
Signal sent to process 5

aurora> ps
PID   STATE   NAME
0     R       idle
1     R       init
2     R       shell
5     Z       hello    (zombie)

aurora> wait
Process 5 collected (exit code: 9)

aurora> ps
PID   STATE   NAME
0     R       idle
1     R       init
2     R       shell
```

#### 关键截图点

| 时间点 | 截图内容 | 说明 |
|--------|----------|------|
| 0s | 初始 ps | 进程列表 |
| 5s | exec 启动新进程 | 进程创建 |
| 10s | kill 后的 ZOMBIE | 僵尸状态 |
| 15s | wait 回收后 | 进程清理 |

---

### FAQ Demo 4: 锁屏功能

#### 目的

展示 Shell 的锁屏安全功能。

#### 操作步骤

```
# 1. 正常使用 Shell
ls /
echo "Before lock"

# 2. 锁屏
lock

# 3. 尝试在锁屏状态下输入
# (输入会被拦截，仅显示锁屏界面)

# 4. 解锁（输入密码）
# AuroraOS 默认密码: aurora

# 5. 解锁后继续使用
echo "After unlock"
ls /
```

#### 预期输出

```
aurora> echo "Before lock"
Before lock

aurora> lock
╔══════════════════════════════════════════════════╗
║                                                  ║
║              🔒  Screen Locked  🔒               ║
║                                                  ║
║         Press Enter and type password            ║
║              to unlock the screen                ║
║                                                  ║
╚══════════════════════════════════════════════════╝
Password: ******

aurora> echo "After unlock"
After unlock
```

#### 关键截图点

| 时间点 | 截图内容 | 说明 |
|--------|----------|------|
| 0s | 正常 Shell 操作 | 锁屏前状态 |
| 3s | 锁屏界面 | 安全锁屏 |
| 8s | 密码输入提示 | 交互体验 |
| 12s | 解锁后恢复 | 解锁成功 |

---

### FAQ Demo 5: 命令历史与 Tab 补全

#### 目的

展示 Shell 的命令历史导航和 Tab 自动补全功能。

#### 操作步骤

```
# 1. 执行一些命令
ls /
echo "hello"
sysinfo
mem
ps

# 2. 按上箭头浏览历史命令
# (按 ↑ 多次，展示历史记录回滚)

# 3. 输入部分命令后按 Tab
# 例如: 输入 "he" 然后按 Tab → 自动补全为 "help"
# 输入 "th" 然后按 Tab → 自动补全为 "theme"

# 4. 查看历史列表
history
```

#### 预期输出

```
aurora> history
  1  ls /
  2  echo "hello"
  3  sysinfo
  4  mem
  5  ps
  6  history
```

#### 关键截图点

| 时间点 | 截图内容 | 说明 |
|--------|----------|------|
| 0s | 执行多条命令 | 构建历史记录 |
| 5s | 按 ↑ 浏览历史 | 历史导航 |
| 8s | Tab 补全效果 | 自动补全 |
| 12s | history 命令输出 | 历史列表 |

---

## 附录

### 附录 A: 演示环境检查清单

录制演示前，请确认以下项目:

- [ ] QEMU 版本 >= 6.0
- [ ] 构建 Release 版本: `make iso`
- [ ] 分配内存 >= 256MB: `-m 256M`
- [ ] 录制工具已安装并测试
- [ ] 关闭系统通知（避免弹窗干扰）
- [ ] 设置终端字体为等宽字体（推荐 JetBrains Mono 或 Fira Code）
- [ ] 终端字体大小 >= 14px（确保可读性）
- [ ] 录制区域包含完整 QEMU 窗口
- [ ] 准备演示文本脚本（避免拼写错误）

### 附录 B: GIF 文件大小优化

```bash
# 使用 gifsicle 优化 GIF 文件大小
sudo apt install gifsicle

# 优化（减少颜色数 + 优化帧间差异）
gifsicle -O3 --colors 64 input.gif -o output.gif

# 缩放
gifsicle --resize 720x480 input.gif -o output.gif
```

### 附录 C: 演示脚本速查卡

```
# Demo 1: 启动过程 — 仅需录制 QEMU 启动
make run

# Demo 2: 文件系统
ls / && touch demo.txt && echo "Hello" > demo.txt && cat demo.txt && cp demo.txt backup.txt && rm demo.txt && ls /

# Demo 3: 进程管理
ps && exec /hello && ps && wait && ps && mem

# Demo 4: 性能监控
exec /hello && exec /hello && echo "test" > t.txt && cat t.txt && perf && perf reset && perf

# Demo 5: 模块加载
mod list && mod load /mod_sample && mod list && mod unload mod_sample && mod list

# Demo 6: 主题切换
theme light && help && theme hc && help && theme dark && help
```

---

> **文档版本**: v3.0.0 | **最后更新**: 2026-06-19
> 配套文档: [architecture.md](architecture.md) | [architecture-visual.md](architecture-visual.md) | [user_manual.md](user_manual.md)