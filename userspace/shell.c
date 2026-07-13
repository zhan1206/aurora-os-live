/* Userspace shell with enhanced builtins and malloc support */
#include <stddef.h>

/* Forward declarations from libc */
int printf(const char *fmt, ...);
int puts(const char *s);
int read(int fd, void *buf, unsigned long count);
int execve(const char *path, char *const argv[], char *const envp[]);
int getpid(void);
int fork(void);
int waitpid(int pid, int *status, int options);
void exit(int code);
int atoi(const char *s);
void *malloc(unsigned long size);
void free(void *ptr);
int sprintf(char *buf, const char *fmt, ...);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, unsigned long n);
unsigned long strlen(const char *s);

/* ================================================================
 * Shell builtins
 * ================================================================ */
static void trim_newline(char *s) {
    int len = (int)strlen(s);
    while (len > 0 && (s[len-1] == '\n' || s[len-1] == '\r')) s[--len] = '\0';
}

static void cmd_help(void) {
    puts("Commands: help echo exec hello fork ps getpid exit clear");
    puts("  echo <msg>  - print a message");
    puts("  exec <path> - execute a program");
    puts("  hello       - print greeting");
    puts("  fork        - fork and demonstrate");
    puts("  ps          - print process info");
    puts("  getpid      - print current PID");
    puts("  exit [code] - exit shell");
    puts("  clear       - clear screen (ANSI)");
}

static void cmd_echo(const char *args) {
    if (args && *args) puts(args);
}

static void cmd_exec(const char *path) {
    if (!path || !*path) { puts("exec: missing path"); return; }
    char *const argv[] = { (char *)path, NULL };
    int ret = execve(path, argv, NULL);
    if (ret < 0) printf("exec failed: %d\n", ret);
}

static void cmd_fork(void) {
    int pid = fork();
    if (pid < 0) {
        puts("fork failed");
    } else if (pid == 0) {
        printf("[child]  PID=%d  parent fork returned %d\n", getpid(), pid);
        exit(0);
    } else {
        printf("[parent] PID=%d  fork returned child PID=%d\n", getpid(), pid);
        int status = 0;
        waitpid(pid, &status, 0);
        printf("[parent] child %d exited with status %d\n", pid, status);
    }
}

static void cmd_ps(void) {
    printf("PID=%d (userspace ps - limited info)\n", getpid());
}

int main(void) {
    char buf[256];

    printf("\n  AuroraOS Userspace Shell v2.0\n");
    printf("  Type 'help' for commands.\n\n");

    for (;;) {
        printf("user$ ");
        int n = 0;
        /* Block until a line is available (the kernel's read blocks) */
        n = read(0, buf, sizeof(buf) - 1);
        if (n <= 0) continue;
        buf[n] = '\0';
        trim_newline(buf);
        if (buf[0] == '\0') continue;

        /* Parse command */
        if (strcmp(buf, "help") == 0) {
            cmd_help();
        } else if (strncmp(buf, "echo ", 5) == 0) {
            cmd_echo(buf + 5);
        } else if (strncmp(buf, "exec ", 5) == 0) {
            cmd_exec(buf + 5);
        } else if (strcmp(buf, "hello") == 0) {
            puts("Hello from userspace shell!");
        } else if (strcmp(buf, "fork") == 0) {
            cmd_fork();
        } else if (strcmp(buf, "ps") == 0) {
            cmd_ps();
        } else if (strcmp(buf, "getpid") == 0) {
            printf("PID=%d\n", getpid());
        } else if (strcmp(buf, "clear") == 0) {
            printf("\x1b[2J\x1b[H");
        } else if (strncmp(buf, "exit", 4) == 0 && (buf[4] == '\0' || buf[4] == ' ')) {
            int code = buf[4] ? atoi(buf + 5) : 0;
            printf("Exiting with code %d\n", code);
            exit(code);
        } else {
            printf("Unknown: %s\n", buf);
        }
    }
    return 0;
}
