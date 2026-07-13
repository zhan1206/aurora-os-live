/*
 * exception.c - CPU exception handlers (NEW)
 *
 * Handles critical x86_64 exceptions that previously had no handler,
 * causing triple faults. Each handler logs the exception type and
 * error code, then panics.
 *
 * (Report §5.1, issue #2: most exceptions unhandled)
 * (Report §11.1: no panic for unrecoverable exceptions)
 */
#include "include/log.h"
#include "include/assert.h"
#include <stdint.h>

/* Exception names for logging — all 32 entries explicitly initialized */
static const char * const exception_names[32] = {
    [0]  = "Divide Error (#DE)",
    [1]  = "Debug (#DB)",
    [2]  = "NMI Interrupt",
    [3]  = "Breakpoint (#BP)",
    [4]  = "Overflow (#OF)",
    [5]  = "Bound Range (#BR)",
    [6]  = "Invalid Opcode (#UD)",
    [7]  = "Device Not Available (#NM)",
    [8]  = "Double Fault (#DF)",
    [9]  = "Coprocessor Segment Overrun",
    [10] = "Invalid TSS (#TS)",
    [11] = "Segment Not Present (#NP)",
    [12] = "Stack Fault (#SS)",
    [13] = "General Protection (#GP)",
    [14] = "Page Fault (#PF)",
    [15] = "(Reserved #15)",
    [16] = "x87 FPU Error (#MF)",
    [17] = "Alignment Check (#AC)",
    [18] = "Machine Check (#MC)",
    [19] = "SIMD Exception (#XM)",
    [20] = "Virtualization (#VE)",
    [21] = "Control Protection (#CP)",
    [22] = "(Reserved #22)",
    [23] = "(Reserved #23)",
    [24] = "(Reserved #24)",
    [25] = "(Reserved #25)",
    [26] = "(Reserved #26)",
    [27] = "(Reserved #27)",
    [28] = "Hypervisor Injection (#HV)",
    [29] = "VMM Communication (#VC)",
    [30] = "Security Exception (#SX)",
    [31] = "(Reserved #31)",
};

/*
 * generic_exception_handler: Default handler for all unhandled exceptions.
 * @vector:  exception vector number (0-31).
 * @error_code: error code pushed by CPU (0 if exception doesn't push one).
 * @rip:       instruction pointer at fault.
 */
void generic_exception_handler(int vector, uint64_t error_code, uint64_t rip) {
    const char *name = (vector < 32 && exception_names[vector])
                       ? exception_names[vector] : "Unknown Exception";

    log_printf(LOG_LEVEL_ERR, "\n!!! EXCEPTION: %s (vector %d)\n", name, vector);
    log_printf(LOG_LEVEL_ERR, "    Error code: 0x%llx\n", (unsigned long long)error_code);
    log_printf(LOG_LEVEL_ERR, "    RIP:        0x%llx\n", (unsigned long long)rip);

    /* Read CR2 for page faults */
    if (vector == 14) {
        uint64_t cr2;
        asm volatile ("mov %%cr2, %0" : "=r"(cr2));
        log_printf(LOG_LEVEL_ERR, "    CR2:        0x%llx\n", (unsigned long long)cr2);
    }

    panic("Unhandled exception %d at RIP=0x%llx\n", vector, (unsigned long long)rip);
}
