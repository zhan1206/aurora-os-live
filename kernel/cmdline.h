/*
 * cmdline.h - Kernel command line interface
 *
 * Provides a global buffer for the kernel command line,
 * parsed from the bootloader (Multiboot or UEFI).
 */
#ifndef CMDLINE_H
#define CMDLINE_H

#include <stddef.h>

/* Maximum length of the kernel command line */
#define CMDLINE_MAX_LEN  256

/* Get the kernel command line string */
const char *cmdline_get(void);

/* Set the kernel command line (called during boot) */
void cmdline_set(const char *cmdline);

/* Initialize the command line from bootloader info */
void cmdline_init(const char *default_cmdline);

/* Check if a command line option is present */
int cmdline_has_flag(const char *flag);

/* Get the value of a command line option (key=value format) */
const char *cmdline_get_option(const char *key);

#endif /* CMDLINE_H */