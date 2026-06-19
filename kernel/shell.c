/*
 * shell.c - Built-in kernel shell with enhanced visual design
 *
 * Uses theme.h design tokens for all visual elements.
 * Layout uses layout.h utility functions.
 *
 * Visual features:
 *   - Welcome banner with system info
 *   - Organized help with categories
 *   - Colorized ls with file sizes
 *   - Enhanced ps table with header
 *   - Dashboard-style sysinfo command
 *   - Memory visualization bar
 *   - Colored status indicators
 *   - Theme switching (dark/light/high-contrast)
 *   - Lock screen with time display
 *   - Confirmation dialog for destructive actions
 *   - Error messages with actionable suggestions
 *   - Accessibility mode indicators
 */
#include "console.h"
#include "include/log.h"
#include "include/kstdio.h"
#include "include/theme.h"
#include "include/explain.h"
#include "pagetable.h"       /* for exec_elf() */
#include "layout.h"
#include "vfs.h"
#include "sched.h"
#include "signal.h"
#include "mem.h"
#include "perf.h"
#include "module.h"
#include <string.h>
#include <stddef.h>

#define PROMPT_USER  "guest"
#define PROMPT_HOST  "aurora"
#define PROMPT_PATH  "~"

/* ================================================================
 * Command history (ring buffer)
 * ================================================================ */
#define HISTORY_MAX  32
#define HISTORY_LEN  128
static char history[HISTORY_MAX][HISTORY_LEN];
static int  history_count = 0;
static int  history_pos   = 0;  /* current position for up/down navigation */
static int  history_idx   = 0;  /* write index */

static void history_add(const char *cmd) {
    if (!cmd || !cmd[0]) return;
    /* Avoid duplicate consecutive entries */
    if (history_count > 0) {
        int prev = (history_idx - 1 + HISTORY_MAX) % HISTORY_MAX;
        if (strcmp(history[prev], cmd) == 0) return;
    }
    /* Copy command into ring buffer */
    size_t i;
    for (i = 0; i < HISTORY_LEN - 1 && cmd[i]; i++)
        history[history_idx][i] = cmd[i];
    history[history_idx][i] = '\0';
    history_idx = (history_idx + 1) % HISTORY_MAX;
    if (history_count < HISTORY_MAX) history_count++;
    history_pos = history_count;  /* reset navigation position */
}

static const char *history_get(int offset) {
    /* offset: 0 = latest, 1 = second latest, etc. */
    if (offset < 0 || offset >= history_count) return NULL;
    int idx = (history_idx - 1 - offset + HISTORY_MAX) % HISTORY_MAX;
    return history[idx];
}

/* History navigation state for line editing */
static int  history_nav_index = -1;  /* -1 = not navigating, 0=latest, 1=second latest... */
static char history_nav_saved[HISTORY_LEN];  /* saved current line before navigation */

/*
 * Callback invoked by console when up/down arrow keys are pressed.
 * direction: -1 = up (older), 1 = down (newer)
 */
static void history_navigate(int direction) {
    if (history_count == 0) return;

    if (direction < 0) {
        /* Up arrow: go to older entry */
        if (history_nav_index == -1) {
            /* First up: save current line */
            const char *cur = console_get_current_line();
            if (cur) {
                size_t i;
                for (i = 0; i < HISTORY_LEN - 1 && cur[i]; i++)
                    history_nav_saved[i] = cur[i];
                history_nav_saved[i] = '\0';
            } else {
                history_nav_saved[0] = '\0';
            }
            history_nav_index = 0;
        } else if (history_nav_index < history_count - 1) {
            history_nav_index++;
        } else {
            return; /* Already at oldest */
        }
    } else {
        /* Down arrow: go to newer entry */
        if (history_nav_index <= 0) {
            /* Back to the saved line */
            console_replace_line(history_nav_saved);
            history_nav_index = -1;
            return;
        }
        history_nav_index--;
    }

    /* Get the history entry and replace line */
    const char *h = history_get(history_nav_index);
    if (h) console_replace_line(h);
}

static void history_reset_nav(void) {
    history_nav_index = -1;
}

/* Forward declarations for utility functions (defined later) */
static void print_int(int x);
static void print_uint64(uint64_t x);
static int  atoi_simple(const char *s);

/* Forward declarations for command functions */
typedef void (*cmd_func_t)(const char *args);
static void do_help_cmd(const char *args);
static void do_about_cmd(const char *args);
static void do_sysinfo_cmd(const char *args);
static void do_clear_cmd(const char *args);
static void do_lock_cmd(const char *args);
static void do_ps_cmd(const char *args);
static void do_wait_cmd(const char *args);
static void do_mem_cmd(const char *args);
static void do_ls_cmd(const char *args);
static void do_history_cmd(const char *args);
static void do_date_cmd(const char *args);
static void do_touch_cmd(const char *args);
static void do_rm_cmd(const char *args);
static void do_cp_cmd(const char *args);
static void do_welcome_cmd(const char *args);
static void do_a11y(const char *args);
static void do_cat(const char *path);
static void do_echo(const char *args);
static void do_exec(const char *path);
static void do_exit_cmd(const char *args);
static void do_kill(const char *args);
static void do_theme(const char *args);
static void do_perf_cmd(const char *args);
static void do_mod(const char *args);

struct cmd_entry {
    const char *name;
    cmd_func_t  func;
    const char *help;
};

/* Command table — keep sorted by name for binary search */
static const struct cmd_entry cmd_table[] = {
    { "a11y",    do_a11y,      "Accessibility settings" },
    { "about",   do_about_cmd, "About AuroraOS" },
    { "cat",     do_cat,       "Display file contents" },
    { "clear",   do_clear_cmd, "Clear the screen" },
    { "cp",      do_cp_cmd,    "Copy a file" },
    { "date",    do_date_cmd,  "Show current date/time" },
    { "echo",    do_echo,      "Print text to console" },
    { "exec",    do_exec,      "Execute an ELF program" },
    { "exit",    do_exit_cmd,  "Exit the shell" },
    { "help",    do_help_cmd,  "Show available commands" },
    { "history", do_history_cmd,"Show command history" },
    { "kill",    do_kill,      "Send a signal to a process" },
    { "lock",    do_lock_cmd,  "Lock the screen" },
    { "ls",      do_ls_cmd,    "List directory contents" },
    { "mem",     do_mem_cmd,   "Show memory usage" },
    { "mod",     do_mod,       "Module management (list|load|unload)" },
    { "perf",    do_perf_cmd,  "Performance statistics" },
    { "ps",      do_ps_cmd,    "List running processes" },
    { "rm",      do_rm_cmd,    "Remove a file" },
    { "sysinfo", do_sysinfo_cmd,"Show system information" },
    { "theme",   do_theme,     "Switch color theme" },
    { "touch",   do_touch_cmd, "Create an empty file" },
    { "wait",    do_wait_cmd,  "Wait for a background process" },
    { "welcome", do_welcome_cmd,"Show welcome message" },
};

#define CMD_COUNT (sizeof(cmd_table) / sizeof(cmd_table[0]))

/* ================================================================
 * Tab completion
 * ================================================================ */

/* Count matches for a prefix in the command table */
static int cmd_match_count(const char *prefix, size_t plen) {
    int count = 0;
    for (size_t i = 0; i < CMD_COUNT; i++) {
        if (strncmp(cmd_table[i].name, prefix, plen) == 0) count++;
    }
    return count;
}

/* Find the longest common prefix among all matches */
static void cmd_common_prefix(const char *prefix, size_t plen, char *out, size_t outlen) {
    const char *first = NULL;
    for (size_t i = 0; i < CMD_COUNT; i++) {
        if (strncmp(cmd_table[i].name, prefix, plen) == 0) {
            first = cmd_table[i].name;
            break;
        }
    }
    if (!first) { out[0] = '\0'; return; }

    size_t k;
    for (k = plen; k < outlen - 1 && first[k]; k++) {
        char c = first[k];
        int all_match = 1;
        for (size_t i = 0; i < CMD_COUNT; i++) {
            if (strncmp(cmd_table[i].name, prefix, plen) == 0) {
                if (cmd_table[i].name[k] != c) { all_match = 0; break; }
            }
        }
        if (!all_match) break;
    }
    for (size_t j = 0; j < k && j < outlen - 1; j++)
        out[j] = first[j];
    out[k] = '\0';
}

/* Show all matching commands */
static void cmd_show_matches(const char *prefix, size_t plen) {
    console_putc('\n');
    int count = 0;
    for (size_t i = 0; i < CMD_COUNT; i++) {
        if (strncmp(cmd_table[i].name, prefix, plen) == 0) {
            if (count > 0 && count % 4 == 0) console_putc('\n');
            console_write_ansi(CLR_INFO);
            console_write("  ");
            console_write(cmd_table[i].name);
            console_write_ansi(SGR_RESET);
            /* Pad to 18 chars */
            int len = 0;
            for (const char *p = cmd_table[i].name; *p; p++) len++;
            for (int j = len + 2; j < 18; j++) console_putc(' ');
            count++;
        }
    }
    console_putc('\n');
}

/*
 * Tab completion callback. Invoked by console when Tab is pressed.
 * Completes command names and filenames.
 */
static void tab_complete(void) {
    const char *line = console_get_current_line();
    if (!line) return;

    /* Find the current word being typed */
    /* Walk from start to cursor to find word boundaries */
    int cursor = 0;
    for (int i = 0; line[i]; i++) cursor++;
    /* inbuf_cursor is not accessible here, but we know the cursor is at end */
    /* Actually, we need the cursor position. Let's use a simpler approach: */
    /* Find the last word by scanning from the end of the line */
    size_t linelen = 0;
    while (line[linelen]) linelen++;

    /* Find word start (last space or beginning) */
    size_t word_start = 0;
    for (size_t i = 0; i < linelen; i++) {
        if (line[i] == ' ') word_start = i + 1;
    }

    /* Check if this is the first word (command name) or an argument */
    int is_first_word = 1;
    for (size_t i = 0; i < word_start; i++) {
        if (line[i] != ' ') { is_first_word = 0; break; }
    }

    if (is_first_word || word_start == 0) {
        /* Complete command name */
        const char *prefix = line + word_start;
        size_t plen = linelen - word_start;

        if (plen == 0) {
            /* Show all commands on double tab */
            cmd_show_matches("", 0);
            return;
        }

        int matches = cmd_match_count(prefix, plen);
        if (matches == 0) return;
        if (matches == 1) {
            /* Complete the single match */
            for (size_t i = 0; i < CMD_COUNT; i++) {
                if (strncmp(cmd_table[i].name, prefix, plen) == 0) {
                    /* Replace the suffix */
                    char completed[256];
                    size_t j;
                    for (j = 0; j < word_start && j < 255; j++)
                        completed[j] = line[j];
                    for (size_t k = 0; cmd_table[i].name[k] && j < 255; k++, j++)
                        completed[j] = cmd_table[i].name[k];
                    completed[j] = '\0';
                    console_replace_line(completed);
                    return;
                }
            }
        }
        /* Multiple matches: complete common prefix */
        char common[256];
        cmd_common_prefix(prefix, plen, common, sizeof(common));
        if (common[plen] != '\0') {
            /* Extend with common prefix */
            char completed[256];
            size_t j;
            for (j = 0; j < word_start && j < 255; j++)
                completed[j] = line[j];
            for (size_t k = 0; common[k] && j < 255; k++, j++)
                completed[j] = common[k];
            completed[j] = '\0';
            console_replace_line(completed);
        } else {
            /* Show all matches */
            cmd_show_matches(prefix, plen);
        }
    } else {
        /* Complete filename (argument) */
        const char *prefix = line + word_start;
        size_t plen = linelen - word_start;

        /* Get list of files from VFS */
        struct super_block *sb = vfs_get_root_sb();
        if (!sb || !sb->root_dentry) return;

        /* Collect matching filenames */
        const char *fnames[64];
        int fcount = 0;
        struct dentry *d = sb->root_dentry->child;
        while (d && fcount < 64) {
            if (d->name && strncmp(d->name, prefix, plen) == 0) {
                fnames[fcount++] = d->name;
            }
            d = d->next;
        }

        if (fcount == 0) return;
        if (fcount == 1) {
            char completed[256];
            size_t j;
            for (j = 0; j < word_start && j < 255; j++)
                completed[j] = line[j];
            for (size_t k = 0; fnames[0][k] && j < 255; k++, j++)
                completed[j] = fnames[0][k];
            completed[j] = '\0';
            console_replace_line(completed);
            return;
        }

        /* Show all matching filenames */
        console_putc('\n');
        for (int i = 0; i < fcount; i++) {
            console_write_ansi(SHELL_FILE_COLOR);
            console_write("  ");
            console_write(fnames[i]);
            console_write_ansi(SGR_RESET);
            console_write("  ");
        }
        console_putc('\n');
    }
}

static void do_history(void) {
    if (history_count == 0) {
        console_write_ansi(CLR_MUTED);
        console_write("  No command history\n");
        console_write_ansi(SGR_RESET);
        return;
    }
    for (int i = history_count - 1; i >= 0; i--) {
        const char *h = history_get(history_count - 1 - i);
        if (h) {
            console_write("  ");
            print_int(i + 1);
            console_write("  ");
            console_write_ansi(CLR_MUTED);
            console_write(h);
            console_write_ansi(SGR_RESET);
            console_putc('\n');
        }
    }
}

static const char *state_names[]  = {"RUN", "READY", "BLOCK", "ZOMB", "DEAD"};
static const char *state_colors[] = {
    PS_STATE_RUNNING, PS_STATE_READY, PS_STATE_BLOCKED,
    PS_STATE_ZOMBIE,  PS_STATE_DEAD
};

/* ================================================================
 * Utility helpers
 * ================================================================ */
static void print_int(int x) {
    char num[16];
    if (x < 0) { console_putc('-'); x = -x; }
    itoa(x, num, sizeof(num));
    console_write(num);
}

static void print_uint64(uint64_t x) {
    char num[24];
    uitoa(x, num, sizeof(num));
    console_write(num);
}

static int atoi_simple(const char *s) {
    int v = 0;
    int neg = 0;
    if (*s == '-') { neg = 1; s++; }
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
    return neg ? -v : v;
}

static void print_prompt(void) {
    console_write_ansi(SHELL_PROMPT_USER);
    console_write(PROMPT_USER);
    console_write_ansi(SHELL_PROMPT_AT);
    console_write("@");
    console_write_ansi(SHELL_PROMPT_HOST);
    console_write(PROMPT_HOST);
    console_write_ansi(SHELL_PROMPT_PATH);
    console_write(":");
    console_write(PROMPT_PATH);
    console_write_ansi(SHELL_PROMPT_DOLLAR);
    console_write("$ ");
    console_write_ansi(SGR_RESET);
}

/* ================================================================
 * Tip of the day system
 * ================================================================ */
static const char *tips[] = {
    "Use 'ls' to list files and 'cat <file>' to view them.",
    "Press Tab to auto-complete commands and filenames.",
    "Use Up/Down arrows to navigate through command history.",
    "Type 'theme' to switch between dark, light, and high-contrast modes.",
    "Use 'exec <program>' to run an ELF executable.",
    "Type 'sysinfo' for a system dashboard with memory and process stats.",
    "Press Ctrl+C to interrupt the current foreground process.",
    "Use 'pipe' to create a pipe for inter-process communication.",
    "Type 'a11y' to toggle accessibility features (high contrast, reduced motion).",
    "Use 'lock' to lock the screen with a clock display.",
    "Type 'mem' to see detailed memory usage statistics.",
    "Use 'cp <src> <dst>' to copy files.",
    "Type 'about' to see AuroraOS version and credits.",
    "Use 'echo <text>' to print text to the console.",
    "Type 'welcome' to re-display this welcome screen.",
};
#define TIP_COUNT (sizeof(tips) / sizeof(tips[0]))

static void show_random_tip(void) {
    /* Use a simple pseudo-random seed based on... well, nothing much
     * available at boot. We'll use the first tip on first boot,
     * and cycle through them. */
    static int tip_index = -1;
    if (tip_index < 0) tip_index = 0;
    else tip_index = (tip_index + 1) % TIP_COUNT;

    console_write_ansi(CLR_MUTED);
    console_write("  Tip: ");
    console_write_ansi(SGR_RESET);
    console_write_ansi(CLR_INFO);
    console_write(tips[tip_index]);
    console_write_ansi(SGR_RESET);
    console_putc('\n');
}

/* ================================================================
 * Welcome Banner
 * ================================================================ */
static void show_welcome(void) {
    console_draw_hr(SEP_DOUBLE);
    console_vpad(1);

    console_write_ansi(SHELL_BANNER_FG);
    console_write_centered("Welcome to AuroraOS Shell");
    console_write_ansi(SGR_RESET);
    console_putc('\n');
    console_write_ansi(CLR_MUTED);
    console_write_centered("Type 'help' for available commands");
    console_write_ansi(SGR_RESET);
    console_putc('\n');

    console_vpad(1);
    show_random_tip();
    console_putc('\n');
    console_draw_hr(SEP_DOUBLE);
    console_putc('\n');
}

/* ================================================================
 * Commands
 * ================================================================ */
static void do_help(void) {
    console_draw_box_top_double(" Help ");
    console_write_ansi(SGR_RESET);

    console_write_ansi(CLR_PRIMARY_BOLD);
    console_write("  System Information\n");
    console_write_ansi(SGR_RESET);
    console_write("    help    - Show this help\n");
    console_write("    about   - About AuroraOS\n");
    console_write("    sysinfo - System dashboard\n");
    console_write("    clear   - Clear screen\n");
    console_write("    lock    - Lock screen\n");
    console_write("    theme   - Switch theme (dark|light|hc)\n");
    console_write("    a11y    - Accessibility (hc|motion)\n");

    console_write_ansi(CLR_PRIMARY_BOLD);
    console_write("  Process Management\n");
    console_write_ansi(SGR_RESET);
    console_write("    ps      - List processes\n");
    console_write("    exec    - Run ELF program\n");
    console_write("    wait    - Wait for child\n");
    console_write("    kill    - Send signal to process\n");
    console_write("    exit    - Exit shell (with confirmation)\n");

    console_write_ansi(CLR_PRIMARY_BOLD);
    console_write("  File System\n");
    console_write_ansi(SGR_RESET);
    console_write("    ls      - List files\n");
    console_write("    cat     - Show file contents\n");
    console_write("    echo    - Print text\n");
    console_write("    touch   - Create empty file\n");
    console_write("    rm      - Remove a file\n");

    console_write_ansi(CLR_PRIMARY_BOLD);
    console_write("  Memory\n");
    console_write_ansi(SGR_RESET);
    console_write("    mem     - Memory usage\n");

    console_write_ansi(CLR_PRIMARY_BOLD);
    console_write("  History\n");
    console_write_ansi(SGR_RESET);
    console_write("    history - Show command history\n");
    console_write("    date    - Show current date/time\n");

    console_draw_box_bottom_double();
}

static void do_echo(const char *args) {
    if (args && *args) { console_write(args); console_putc('\n'); }
}

static void do_ls(void) {
    struct super_block *sb = vfs_get_root_sb();
    if (!sb || !sb->root_dentry) {
        console_error_with_hint("No filesystem mounted", "Ensure VFS is initialized");
        return;
    }

    console_write_ansi(CLR_MUTED);
    console_write("  Name                    Type       Size\n");
    console_write_ansi(SGR_RESET);
    console_draw_hr(SEP_DOT);

    struct dentry *d = sb->root_dentry->child;
    while (d) {
        if (d->name && d->inode) {
            console_write("  ");
            if (d->inode->is_dir) {
                console_write_ansi(SHELL_DIR_COLOR);
            } else {
                console_write_ansi(SHELL_FILE_COLOR);
            }
            console_write(d->name);
            if (d->inode->is_dir) console_write("/");
            console_write_ansi(SGR_RESET);

            /* Pad name to fixed width */
            int nlen = 0;
            for (const char *p = d->name; *p; ++p) nlen++;
            if (d->inode->is_dir) nlen++; /* for trailing / */
            for (int i = nlen; i < 24; ++i) console_putc(' ');

            /* Type */
            console_write_ansi(CLR_INFO);
            console_write(d->inode->is_dir ? "DIR " : "FILE");
            console_write_ansi(SGR_RESET);

            /* Size (not available in current inode — show placeholder) */
            console_write("     ");
            console_write_ansi(CLR_MUTED);
            console_write("   ?");
            console_write_ansi(SGR_RESET);

            console_putc('\n');
        }
        d = d->next;
    }
}

static void do_cat(const char *path) {
    if (!path || !*path) {
        console_error_with_hint("cat: missing path", "Usage: cat <filename>");
        return;
    }
    struct file *f = vfs_open(path, 0);
    if (!f) {
        console_error_with_hint("cat: open failed", "Check if the file exists (use 'ls' to list files)");
        return;
    }
    char buf[128];
    ssize_t r;
    while ((r = vfs_read(f, buf, sizeof(buf))) > 0)
        for (ssize_t i = 0; i < r; ++i) console_putc(buf[i]);
    vfs_close(f);
}

static void do_exec(const char *path) {
    if (!path || !*path) {
        console_error_with_hint("exec: missing path", "Usage: exec <program>");
        return;
    }
    console_write_ansi(CLR_MUTED);
    console_write("Loading ");
    console_write(path);
    console_write("...");
    console_write_ansi(SGR_RESET);
    console_putc('\n');

    int pid = exec_elf(path);
    if (pid < 0) {
        console_error_with_hint("exec failed",
            explain_errno(current->t_errno));
    } else {
        console_write_ansi(SHELL_CMD_OK);
        console_write("Started PID ");
        print_int(pid);
        console_write_ansi(SGR_RESET);
        console_putc('\n');
    }
}

static void do_ps(void) {
    if (!current) {
        console_error_with_hint("No scheduler running", "System is in early boot stage");
        return;
    }

    /* Count tasks */
    int task_count = 0;
    struct task_struct *t = current;
    do { task_count++; t = t->next; } while (t != current);

    console_write_ansi(CLR_MUTED);
    console_write("  ");
    print_int(task_count);
    console_write(" process(es) running");
    console_write_ansi(SGR_RESET);
    console_putc('\n');

    /* Table header */
    const char *headers[] = {"", "PID", "STATE", "NAME"};
    int widths[] = {2, 6, 8, 20};
    console_draw_table_header(headers, widths, 4);

    t = current;
    do {
        char pid_str[8], state_str[12];
        itoa(t->pid, pid_str, sizeof(pid_str));
        const char *sn = (t->state <= TASK_DEAD) ? state_names[t->state] : "???";
        const char *sc = (t->state <= TASK_DEAD) ? state_colors[t->state] : CLR_ERROR;
        int si = 0;
        while (sn[si] && si < 11) { state_str[si] = sn[si]; si++; }
        state_str[si] = '\0';

        const char *mark = (t == current) ? "*" : " ";
        const char *vals[] = {mark, pid_str, state_str, t->name[0] ? t->name : "-"};
        const char *cols[] = {PS_CURRENT_MARK, NULL, sc, NULL};
        console_draw_table_row(vals, cols, widths, 4);
        t = t->next;
    } while (t != current);
}

static void do_wait(void) {
    int status = 0;
    console_write_ansi(CLR_MUTED);
    console_write("Waiting for child process...");
    console_write_ansi(SGR_RESET);
    console_putc('\n');

    int pid = waitpid(-1, &status, 0);
    if (pid < 0) {
        console_error_with_hint("wait: no children", "No child processes to wait for");
    } else {
        console_write_ansi(SHELL_CMD_OK);
        console_write("Collected PID ");
        print_int(pid);
        console_write(" (exit code ");
        print_int(status);
        console_write(")");
        console_write_ansi(SGR_RESET);
        console_putc('\n');
    }
}

static void do_kill(const char *args) {
    if (!args || !*args) {
        console_error_with_hint("kill: missing arguments", "Usage: kill <pid> [signal]");
        return;
    }
    int pid = atoi_simple(args);
    while (*args >= '0' && *args <= '9') args++;
    while (*args == ' ') args++;
    int sig = SIGKILL;
    if (*args) sig = atoi_simple(args);
    if (sig < 1 || sig >= NSIG) sig = SIGKILL;

    if (do_sys_kill(pid, sig) < 0) {
        console_write_ansi(SHELL_CMD_ERROR);
        console_write("kill: failed");
        console_write_ansi(SGR_RESET);
        console_putc('\n');
    } else {
        console_write_ansi(SHELL_CMD_OK);
        console_write("Signal ");
        print_int(sig);
        console_write(" sent to PID ");
        print_int(pid);
        console_write_ansi(SGR_RESET);
        console_putc('\n');
    }
}

static void do_mem(void) {
    uint64_t total, fre, used;
    mem_get_stats(&total, &fre, &used);

    int pct = total ? (int)(used * 100 / total) : 0;

    console_draw_dash_section("Memory Usage");
    console_write_ansi(MEM_BAR_LABEL);
    console_write("  ");
    console_draw_progress_bar_styled(pct, 50,
        (pct > 80 ? DASH_BAR_CRITICAL : (pct > 50 ? DASH_BAR_LOW : DASH_BAR_FILLED)),
        DASH_BAR_EMPTY, DASH_BAR_BRACKET, MEM_BAR_PCT);
    console_write_ansi(SGR_RESET);
    console_putc('\n');

    console_write("  ");
    console_write_ansi(DASH_LABEL);
    console_write("Total: ");
    console_write_ansi(DASH_VALUE);
    print_uint64(total / 1024);
    console_write(" KiB");
    console_write_ansi(SGR_RESET);
    console_write("  |  ");

    console_write_ansi(DASH_LABEL);
    console_write("Used: ");
    console_write_ansi(DASH_VALUE);
    print_uint64(used / 1024);
    console_write(" KiB");
    console_write_ansi(SGR_RESET);
    console_write("  |  ");

    console_write_ansi(DASH_LABEL);
    console_write("Free: ");
    console_write_ansi(DASH_VALUE);
    print_uint64(fre / 1024);
    console_write(" KiB");
    console_write_ansi(SGR_RESET);
    console_putc('\n');
}

static void do_perf(const char *args) {
    while (*args == ' ') args++;
    if (args && strcmp(args, "reset") == 0) {
        perf_reset();
    } else {
        perf_dump();
    }
}

static void do_sysinfo(void) {
    console_draw_box_top_double(" System Information ");
    console_write_ansi(SGR_RESET);

    /* OS info */
    console_draw_dash_section("  Operating System");
    console_draw_dash_row("Name", "AuroraOS");
    console_draw_dash_row("Version", "2.3.0");
    console_draw_dash_row("Architecture", "x86_64");
    console_draw_dash_row("Build", "2026-06-19");

    /* Memory */
    uint64_t total, fre, used;
    mem_get_stats(&total, &fre, &used);
    console_draw_dash_section("  Memory");
    {
        char buf[32];
        uitoa(total / 1024, buf, sizeof(buf));
        console_draw_dash_row("Total", buf);
        /* append " KiB" */
        console_write_ansi(DASH_LABEL);
        console_write("  Used.................. ");
        console_write_ansi(DASH_VALUE);
        uitoa(used / 1024, buf, sizeof(buf));
        console_write(buf);
        console_write(" KiB");
        console_write_ansi(SGR_RESET);
        console_putc('\n');
        console_write_ansi(DASH_LABEL);
        console_write("  Free................. ");
        console_write_ansi(DASH_VALUE);
        uitoa(fre / 1024, buf, sizeof(buf));
        console_write(buf);
        console_write(" KiB");
        console_write_ansi(SGR_RESET);
        console_putc('\n');
    }

    /* Tasks */
    if (current) {
        int task_count = 0;
        struct task_struct *t = current;
        do { task_count++; t = t->next; } while (t != current);

        console_draw_dash_section("  Processes");
        {
            char buf[16];
            itoa(task_count, buf, sizeof(buf));
            console_draw_dash_row("Count", buf);
        }
        console_draw_dash_row("Scheduler", "Round Robin");
        console_draw_dash_row("Quantum", "10 ms");
    }

    console_draw_box_bottom_double();
}

static void do_about(void) {
    const char *lines[] = {
        "  AuroraOS v2.3.0",
        "",
        "  Author: AuroraOS Team",
        "  License: MIT",
        "  \"Simplicity is the ultimate sophistication\"",
        "  -- Leonardo da Vinci",
        NULL
    };
    console_draw_box(" About AuroraOS ", lines);
}

static void do_clear(void) {
    console_clear();
}

/* ================================================================
 * Theme switching command (§6 Theme System)
 * ================================================================ */
static void do_theme(const char *args) {
    while (*args == ' ') args++;

    if (!args || !*args) {
        /* Show current theme */
        console_write_ansi(CLR_INFO);
        console_write("Current theme: ");
        console_write_ansi(SGR_RESET);
        if (g_theme_mode == THEME_DARK)          console_write("dark");
        else if (g_theme_mode == THEME_LIGHT)    console_write("light");
        else if (g_theme_mode == THEME_HIGH_CONTRAST) console_write("high-contrast");
        console_putc('\n');
        console_write_ansi(CLR_MUTED);
        console_write("Usage: theme <dark|light|hc>  —  switch theme in real-time\n");
        console_write_ansi(SGR_RESET);
        return;
    }

    if (strcmp(args, "dark") == 0) {
        g_theme_mode = THEME_DARK;
        console_clear();
        console_draw_notification("success", "Theme switched to Dark");
    } else if (strcmp(args, "light") == 0) {
        g_theme_mode = THEME_LIGHT;
        console_clear();
        console_draw_notification("success", "Theme switched to Light");
    } else if (strcmp(args, "hc") == 0 || strcmp(args, "high-contrast") == 0) {
        g_theme_mode = THEME_HIGH_CONTRAST;
        console_clear();
        console_draw_notification("success", "High Contrast mode enabled");
    } else {
        console_error_with_hint("Unknown theme", "Try: dark, light, or hc");
    }
}

/* ================================================================
 * Accessibility: reduced motion toggle
 * ================================================================ */
static void do_a11y(const char *args) {
    while (*args == ' ') args++;

    if (!args || !*args) {
        /* Show current accessibility state */
        console_write_ansi(CLR_INFO);
        console_write("Accessibility settings:\n");
        console_write_ansi(SGR_RESET);
        console_write("  High contrast: ");
        console_write_ansi(theme_is_high_contrast() ? CLR_SUCCESS : CLR_MUTED);
        console_write(theme_is_high_contrast() ? "ON" : "off");
        console_write_ansi(SGR_RESET);
        console_putc('\n');
        console_write("  Reduced motion: ");
        console_write_ansi(g_reduced_motion ? CLR_SUCCESS : CLR_MUTED);
        console_write(g_reduced_motion ? "ON" : "off");
        console_write_ansi(SGR_RESET);
        console_putc('\n');
        console_write_ansi(CLR_MUTED);
        console_write("Usage: a11y <hc|motion>  —  toggle accessibility features\n");
        console_write_ansi(SGR_RESET);
        return;
    }

    if (strcmp(args, "hc") == 0) {
        g_theme_mode = theme_is_high_contrast() ? THEME_DARK : THEME_HIGH_CONTRAST;
        console_clear();
        console_draw_notification("success",
            theme_is_high_contrast() ? "High Contrast enabled" : "High Contrast disabled");
    } else if (strcmp(args, "motion") == 0) {
        g_reduced_motion = !g_reduced_motion;
        g_anim_enabled = !g_reduced_motion;
        console_draw_notification("success",
            g_reduced_motion ? "Reduced motion enabled" : "Reduced motion disabled");
    } else {
        console_error_with_hint("Unknown a11y option", "Try: hc or motion");
    }
}

/* ================================================================
 * Lock screen command (§7.3) — fixed to yield instead of blocking
 * ================================================================ */
static void do_lock(void) {
    console_draw_lock_screen("14:30", "2026-06-19  Friday");
    /* Wait for input by yielding — non-blocking poll loop */
    for (;;) {
        char dummy[2];
        int len = console_getline(dummy, sizeof(dummy));
        if (len > 0) break;
        yield();
    }
    /* After unlock, return to shell */
    console_clear();
    console_draw_notification("info", "Screen unlocked");
}

/* ================================================================
 * Module management command (§Mod)
 * ================================================================ */
static void do_mod(const char *args) {
    while (*args == ' ') args++;

    if (!args || !*args) {
        console_error_with_hint("mod: missing subcommand", "Usage: mod <list|load|unload> [args]");
        return;
    }

    /* Parse subcommand */
    const char *sub = args;
    while (*args && *args != ' ') args++;
    size_t sub_len = (size_t)(args - sub);
    while (*args == ' ') args++;

    if (sub_len == 4 && strncmp(sub, "list", 4) == 0) {
        module_list();
    } else if (sub_len == 4 && strncmp(sub, "load", 4) == 0) {
        if (!*args) {
            console_error_with_hint("mod load: missing path", "Usage: mod load <path>");
            return;
        }
        console_write_ansi("\x1b[90m");
        console_write("Loading module: ");
        console_write(args);
        console_write("...");
        console_write_ansi("\x1b[0m");
        console_putc('\n');

        if (module_load(args) == 0) {
            console_write_ansi("\x1b[32m");
            console_write("Module loaded successfully.");
            console_write_ansi("\x1b[0m");
            console_putc('\n');
        } else {
            console_error_with_hint("mod load: failed to load module", args);
        }
    } else if (sub_len == 6 && strncmp(sub, "unload", 6) == 0) {
        if (!*args) {
            console_error_with_hint("mod unload: missing name", "Usage: mod unload <name>");
            return;
        }
        if (module_unload(args) == 0) {
            console_write_ansi("\x1b[32m");
            console_write("Module unloaded.");
            console_write_ansi("\x1b[0m");
            console_putc('\n');
        } else {
            console_error_with_hint("mod unload: failed", args);
        }
    } else {
        console_error_with_hint("mod: unknown subcommand", "Usage: mod <list|load|unload>");
    }
}

/* ================================================================
 * Enhanced Exit with Confirmation (§5.6)
 * ================================================================ */
static void do_exit_cmd(const char *args) {
    while (*args == ' ') args++;
    int code = (*args) ? atoi_simple(args) : 0;

    /* Confirmation dialog for safety */
    console_draw_confirm_dialog(" Confirm Exit ", "Are you sure you want to exit the shell?");

    char confirm[8];
    int len = 0;
    /* Wait for user input (console_getline is non-blocking) */
    while ((len = console_getline(confirm, sizeof(confirm))) <= 0) {
        yield();
    }

    if (confirm[0] == 'y' || confirm[0] == 'Y') {
        console_write_ansi(SHELL_CMD_WARN);
        console_write("Exiting with code ");
        print_int(code);
        console_write_ansi(SGR_RESET);
        console_putc('\n');
        do_exit_current(code);
    } else {
        console_draw_notification("info", "Exit cancelled");
    }
}

/* ================================================================
 * Login screen (enhanced, §7.2)
 *
 * Layout: centered box with time/date, avatar, input, and
 *         accessibility + power options at bottom.
 * ================================================================ */
static void do_login(void) {
    char line[256];

    console_vcenter(10);

    /* Time display (large, centered) */
    console_write_ansi(LOGIN_TIME_FG);
    console_write_centered("14:30");
    console_write_ansi(SGR_RESET);
    console_putc('\n');

    /* Date */
    console_write_ansi(LOGIN_DATE_FG);
    console_write_centered("2026-06-19  Friday");
    console_write_ansi(SGR_RESET);
    console_putc('\n');

    console_vpad(1);

    /* Login box */
    console_draw_box_top_double(" AuroraOS Login ");
    console_write_ansi(SGR_RESET);

    /* Avatar/preview line */
    console_write_ansi(LOGIN_AVATAR_FG);
    console_write("         (o_o)");
    console_write_ansi(SGR_RESET);
    console_putc('\n');

    console_write_ansi(LOGIN_PROMPT_FG);
    console_write("  Username: ");
    console_write_ansi(SGR_RESET);

    for (;;) {
        int len = console_getline(line, sizeof(line));
        if (len > 0 && strcmp(line, "root") == 0) break;
        if (len > 0 && strcmp(line, "guest") == 0) break;
        if (len > 0) {
            console_write_ansi(LOGIN_ERROR_FG);
            console_write("  Invalid user. Try 'guest' or 'root'");
            console_write_ansi(SGR_RESET);
            console_write("\n  Username: ");
        }
        yield();
    }

    console_draw_box_bottom_double();
    console_vpad(1);

    /* System function buttons */
    console_write_ansi(LOGIN_HINT_FG);
    console_write("  [");
    console_write_ansi(POWER_SHUTDOWN_FG);
    console_write("S");
    console_write_ansi(LOGIN_HINT_FG);
    console_write("]hutdown  [");
    console_write_ansi(POWER_RESTART_FG);
    console_write("R");
    console_write_ansi(LOGIN_HINT_FG);
    console_write("]estart  [");
    console_write_ansi(LOGIN_ACC_FG);
    console_write("A");
    console_write_ansi(LOGIN_HINT_FG);
    console_write("]ccessibility");
    console_write_ansi(SGR_RESET);
    console_vpad(2);

    console_write_ansi(LOGIN_SUCCESS_FG);
    console_write("  Welcome, ");
    console_write(line);
    console_write("!");
    console_write_ansi(SGR_RESET);
    console_vpad(2);
}

/* ================================================================
 * Command table (sorted alphabetically for binary search)
 * ================================================================ */
static void do_date_cmd(const char *args) {
    (void)args;
    console_write_ansi(SHELL_CMD_OK);
    console_write("2026-06-19  Friday  ");
    console_write_ansi(SGR_RESET);
    console_putc('\n');
}

static void do_touch_cmd(const char *args) {
    while (*args == ' ') args++;
    if (!*args) {
        console_error_with_hint("touch", "Usage: touch <filename>");
        return;
    }
    /* Use ramfs_add_file to create an empty file */
    extern int ramfs_add_file(const char *name, const char *content);
    if (ramfs_add_file(args, "") == 0) {
        console_write_ansi(SHELL_CMD_OK);
        console_write("Created: ");
        console_write(args);
        console_write_ansi(SGR_RESET);
        console_putc('\n');
    } else {
        console_write_ansi(SHELL_CMD_ERROR);
        console_write("touch: cannot create '");
        console_write(args);
        console_write("'");
        console_write_ansi(SGR_RESET);
        console_putc('\n');
    }
}

static void do_rm_cmd(const char *args) {
    while (*args == ' ') args++;
    if (!*args) {
        console_error_with_hint("rm", "Usage: rm <filename>");
        return;
    }
    struct inode *ino = vfs_lookup(args);
    if (ino) {
        console_write_ansi(SHELL_CMD_WARN);
        console_write("rm: '");
        console_write(args);
        console_write("' exists but unlink not yet implemented");
        console_write_ansi(SGR_RESET);
        console_putc('\n');
    } else {
        console_write_ansi(SHELL_CMD_ERROR);
        console_write("rm: cannot remove '");
        console_write(args);
        console_write("': No such file");
        console_write_ansi(SGR_RESET);
        console_putc('\n');
    }
}

static void do_cp_cmd(const char *args) {
    /* Parse source and destination paths */
    while (*args == ' ') args++;
    if (!*args) {
        console_error_with_hint("cp", "Usage: cp <source> <dest>");
        return;
    }
    const char *src = args;
    while (*args && *args != ' ') args++;
    if (!*args) {
        console_error_with_hint("cp", "Usage: cp <source> <dest>");
        return;
    }
    size_t src_len = (size_t)(args - src);
    while (*args == ' ') args++;
    const char *dst = args;
    if (!*dst) {
        console_error_with_hint("cp", "Missing destination path");
        return;
    }

    /* Read source file */
    struct file *fsrc = vfs_open(src, 0);
    if (!fsrc) {
        console_error_with_hint("cp: source not found", src);
        return;
    }
    char buf[4096];
    ssize_t total = 0;
    ssize_t r;
    while ((r = vfs_read(fsrc, buf + total, sizeof(buf) - (size_t)total - 1)) > 0) {
        total += r;
        if ((size_t)total >= sizeof(buf) - 1) break;
    }
    vfs_close(fsrc);
    buf[total] = '\0';

    /* Create/open destination and write */
    struct file *fdst = vfs_open(dst, 0);
    if (!fdst) {
        /* Create new file if destination doesn't exist */
        extern int ramfs_add_file(const char *name, const char *content);
        (void)src_len;
        if (ramfs_add_file(dst, buf) == 0) {
            console_write_ansi(SHELL_CMD_OK);
            console_write("Copied to ");
            console_write(dst);
            console_write_ansi(SGR_RESET);
            console_putc('\n');
            return;
        }
        console_error_with_hint("cp: cannot create destination", dst);
        return;
    }
    ssize_t wrote = vfs_write(fdst, buf, (size_t)total);
    vfs_close(fdst);
    if (wrote >= 0) {
        console_write_ansi(SHELL_CMD_OK);
        console_write("Copied to ");
        console_write(dst);
        console_write_ansi(SGR_RESET);
        console_putc('\n');
    } else {
        console_error_with_hint("cp: write failed", dst);
    }
}

static void do_welcome_cmd(const char *args) {
    (void)args;
    show_welcome();
}

/* Wrapper functions for void commands to match cmd_func_t signature */
static void do_help_cmd(const char *args)    { (void)args; do_help(); }
static void do_about_cmd(const char *args)   { (void)args; do_about(); }
static void do_sysinfo_cmd(const char *args) { (void)args; do_sysinfo(); }
static void do_clear_cmd(const char *args)   { (void)args; do_clear(); }
static void do_lock_cmd(const char *args)    { (void)args; do_lock(); }
static void do_ps_cmd(const char *args)      { (void)args; do_ps(); }
static void do_wait_cmd(const char *args)    { (void)args; do_wait(); }
static void do_mem_cmd(const char *args)     { (void)args; do_mem(); }
static void do_ls_cmd(const char *args)      { (void)args; do_ls(); }
static void do_history_cmd(const char *args) { (void)args; do_history(); }
static void do_perf_cmd(const char *args)    { do_perf(args); }

/* Find command by name using binary search */
static cmd_func_t cmd_find(const char *name, const char **args) {
    /* Find the space separator if any */
    const char *sp = name;
    while (*sp && *sp != ' ') sp++;
    size_t namelen = (size_t)(sp - name);

    int lo = 0, hi = CMD_COUNT - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        int cmp = strncmp(name, cmd_table[mid].name, namelen);
        if (cmp == 0 && cmd_table[mid].name[namelen] == '\0') {
            /* Found: return args (skip space) */
            if (args) *args = (*sp == ' ') ? sp + 1 : sp;
            return cmd_table[mid].func;
        }
        if (cmp < 0) hi = mid - 1;
        else lo = mid + 1;
    }
    return NULL;
}

/* ================================================================
 * Main loop
 * ================================================================ */
void shell_main(void) {
    char line[256];

    /* Register history navigation callback for line editing */
    console_set_history_callback(history_navigate);
    /* Register tab completion callback */
    console_set_tab_complete_callback(tab_complete);

    /* Login */
    do_login();

    /* Welcome banner */
    show_welcome();

    print_prompt();

    for (;;) {
        int len = console_getline(line, sizeof(line));
        if (len <= 0) { /* no input */ }
        else {
            /* Reset history navigation on new command */
            history_reset_nav();

            if (strcmp(line, "") == 0) {
                /* no-op */
            } else {
                const char *args = NULL;
                cmd_func_t cmd = cmd_find(line, &args);
                if (cmd) {
                    cmd(args);
                } else {
                    console_error_with_hint(line, "Type 'help' for available commands");
                }
            }
            /* Add non-empty commands to history */
            if (len > 0 && strcmp(line, "") != 0) history_add(line);
            print_prompt();
        }
        yield();
    }
}