# AuroraOS 用户手册

## 1. 快速入门

### 1.1 启动系统

AuroraOS 通过 GRUB2 引导。启动后，您将看到登录界面：

```
                   14:30
              2026-07-09  Wednesday

        ╔══════════════════════════════╗
        ║     AuroraOS Login          ║
        ║                            ║
        ║         (o_o)              ║
        ║                            ║
        ║  Username: _               ║
        ╚══════════════════════════════╝

  [S]hutdown  [R]estart  [A]ccessibility
```

### 1.2 登录

输入用户名并按 Enter：
- `guest` — 游客用户（推荐）
- `root` — 超级用户

登录成功后，显示欢迎横幅和命令提示符：

```
guest@aurora:~$ _
```

### 1.3 基本操作

- 输入命令后按 **Enter** 执行
- 使用 **上下箭头键** 浏览命令历史
- 使用 **左右箭头键** 移动光标编辑命令
- 按 **Tab 键** 自动补全命令名和文件名
- 按 **Home/End** 跳转到行首/行尾
- 按 **Backspace** 删除光标前字符，**Delete** 删除光标后字符

---

## 2. 命令参考

### 2.1 系统信息

#### help — 显示帮助
```
guest@aurora:~$ help
```
显示所有可用命令，按类别分组。

#### about — 关于系统
```
guest@aurora:~$ about
```
显示 AuroraOS 版本和作者信息。

#### sysinfo — 系统仪表盘
```
guest@aurora:~$ sysinfo
```
显示系统概览，包括 OS 信息、内存使用、进程统计。

#### clear — 清屏
```
guest@aurora:~$ clear
```
清除屏幕内容。

---

### 2.2 进程管理

#### ps — 进程列表
```
guest@aurora:~$ ps
```
显示所有运行中的进程，包括 PID、状态、名称。当前进程标记为 `*`。

示例输出：
```
  3 process(es) running
  ┌────────┬──────┬────────┬──────────────────────┐
  │        │ PID  │ STATE  │ NAME                 │
  ├────────┼──────┼────────┼──────────────────────┤
  │  *     │  1   │ RUN    │ shell                │
  │        │  2   │ READY  │ idle                 │
  │        │  3   │ READY  │ init                 │
  └────────┴──────┴────────┴──────────────────────┘
```

#### exec — 执行程序
```
guest@aurora:~$ exec <program>
```
加载并执行 ELF 可执行文件。

示例：
```
guest@aurora:~$ exec hello
```

#### wait — 等待子进程
```
guest@aurora:~$ wait
```
等待任意子进程结束并收集退出状态。

#### kill — 发送信号
```
guest@aurora:~$ kill <pid> [signal]
```
向指定进程发送信号。默认信号为 SIGKILL (9)。

示例：
```
guest@aurora:~$ kill 5        # 终止 PID 5
guest@aurora:~$ kill 5 2      # 向 PID 5 发送 SIGINT
```

#### exit — 退出 Shell
```
guest@aurora:~$ exit [code]
```
退出 Shell（需要确认）。

---

### 2.3 文件系统

#### ls — 文件列表
```
guest@aurora:~$ ls
```
列出当前目录下的所有文件和目录。

示例输出：
```
  Name                    Type       Size
  ─────────────────────────────────────────
  hello                  FILE        ?
  readme.txt             FILE        ?
  testdir/               DIR         ?
```

#### cat — 查看文件
```
guest@aurora:~$ cat <filename>
```
显示文件内容。

示例：
```
guest@aurora:~$ cat readme.txt
```

#### echo — 打印文本
```
guest@aurora:~$ echo <text>
```
打印指定文本。

示例：
```
guest@aurora:~$ echo Hello, AuroraOS!
```

#### touch — 创建空文件
```
guest@aurora:~$ touch <filename>
```
创建一个空文件。

示例：
```
guest@aurora:~$ touch newfile.txt
```

#### rm — 删除文件
```
guest@aurora:~$ rm <filename>
```
删除指定文件。

示例：
```
guest@aurora:~$ rm oldfile.txt
```

#### cp — 复制文件
```
guest@aurora:~$ cp <source> <dest>
```
复制文件到目标路径。

示例：
```
guest@aurora:~$ cp hello.txt backup.txt
```

---
### 2.4 内存

#### mem — 内存使用
```
guest@aurora:~$ mem
```
显示内存使用情况，包括进度条和数值。

示例输出：
```
----- Memory Usage -----
  [████████░░░░░░░░░░░░░░░░░░░░] 42%
  Total: 65536 KiB  |  Used: 27520 KiB  |  Free: 38016 KiB
```

---

### 2.5 个性化

#### theme — 主题切换
```
guest@aurora:~$ theme                 # 显示当前主题
guest@aurora:~$ theme dark            # 切换到暗色主题
guest@aurora:~$ theme light           # 切换到亮色主题
guest@aurora:~$ theme hc              # 切换到高对比度模式
```

#### a11y — 无障碍设置
```
guest@aurora:~$ a11y                  # 显示当前设置
guest@aurora:~$ a11y hc               # 切换高对比度
guest@aurora:~$ a11y motion           # 切换减少动画
```

#### history — 命令历史
```
guest@aurora:~$ history
```
显示最近执行的命令历史（最多 32 条）。

#### lock — 锁屏
```
guest@aurora:~$ lock
```
锁定屏幕，按任意键解锁。

#### date — 显示日期时间
```
guest@aurora:~$ date
```
显示当前日期和时间。

#### welcome — 显示欢迎界面
```
guest@aurora:~$ welcome
```
重新显示登录后的欢迎横幅和每日提示。

---

## 3. 行编辑功能

AuroraOS Shell 支持丰富的行编辑功能：

| 按键 | 功能 |
|------|------|
| ← → | 左右移动光标 |
| Home | 跳转到行首 |
| End | 跳转到行尾 |
| Backspace | 删除光标前字符 |
| Delete | 删除光标后字符 |
| ↑ | 上一条历史命令 |
| ↓ | 下一条历史命令 |
| Tab | 自动补全命令名/文件名 |

### 3.1 Tab 补全

- **命令补全**: 在行首输入命令前缀后按 Tab
  - 唯一匹配：自动完成命令名
  - 多个匹配：显示所有匹配的命令
  - 公共前缀：自动扩展到公共前缀
  - 空输入按 Tab：显示所有可用命令

- **文件名补全**: 在命令参数位置按 Tab
  - 自动匹配当前目录下的文件名

---

## 4. 常见问题

### Q: 如何退出 Shell？
A: 输入 `exit` 并按 Enter，在确认对话框中输入 `y` 确认。

### Q: 如何查看文件大小？
A: 使用 `ls -l`（或 `ll`）命令查看文件大小，或使用 `fstat` 系统调用获取文件元数据。

### Q: 命令执行后如何回到提示符？
A: 每个命令执行完毕后会自动显示新的提示符，无需额外操作。

### Q: 如何查看之前执行的命令？
A: 使用上箭头键浏览历史命令，或输入 `history` 查看完整列表。

### Q: 支持哪些可执行文件格式？
A: 仅支持 x86_64 ELF 格式的可执行文件。

### Q: 系统支持多任务吗？
A: 是的，支持多进程并发执行。使用 `ps` 查看所有进程，使用 `exec` 启动新程序。

### Q: 如何创建新文件？
A: 使用 `touch <filename>` 命令创建空文件。例如：`touch myfile.txt`。

---

## 5. 故障排除

### 5.1 命令无响应
- 检查命令拼写是否正确
- 使用 `help` 查看可用命令列表
- 确认 Shell 未处于锁屏状态

### 5.2 程序执行失败
- 使用 `ls` 确认文件存在
- 确认文件是有效的 x86_64 ELF 格式
- 使用 `ps` 检查进程状态

### 5.3 显示异常
- 尝试使用 `clear` 清屏
- 切换主题：`theme dark` 或 `theme light`
- 检查终端模拟器是否支持 ANSI 转义序列

### 5.4 内存不足
- 使用 `mem` 查看内存使用情况
- 使用 `kill` 终止不必要的进程
- 减少同时运行的进程数量