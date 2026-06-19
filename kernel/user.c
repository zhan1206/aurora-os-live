/*
 * user.c - User-space task creation (FIXED: resource leak on error)
 */
#include "user.h"
#include "sched.h"
#include "mem.h"
#include "include/log.h"
#include "pagetable.h"
#include "aslr.h"

static void user_trampoline(void) {
    for (;;) asm volatile ("hlt");
}

extern void enter_user(void *entry, void *stack_top);

int create_user_task_from_entry(void (*entry)(void), uint64_t pml4_phys) {
    if (!entry || !pml4_phys) return -1;

    void *p1 = alloc_page();
    void *p2 = alloc_page();
    if (!p1 || !p2) {
        if (p1) free_page(p1);
        if (p2) free_page(p2);
        return -1;
    }

    const uint64_t USER_STACK_TOP = aslr_randomize_stack();
    const int pages = 2;
    uint64_t stack_base_v = USER_STACK_TOP - pages * 4096;

    uint64_t phys_p1 = (uint64_t)(uintptr_t)p1;
    uint64_t phys_p2 = (uint64_t)(uintptr_t)p2;

    if (map_user_page(pml4_phys, stack_base_v, phys_p1, PTE_RW) != 0) {
        free_page(p1); free_page(p2);
        return -1;
    }
    if (map_user_page(pml4_phys, stack_base_v + 4096, phys_p2, PTE_RW) != 0) {
        /* Unmap p1 before freeing, to avoid dangling page table entry */
        extern void unmap_page(uint64_t pml4_phys, uint64_t vaddr);
        unmap_page(pml4_phys, stack_base_v);
        free_page(p1); free_page(p2);
        return -1;
    }

    struct task_struct *t = create_task(user_trampoline);
    if (!t) {
        free_page(p1); free_page(p2);
        return -1;
    }

    uint64_t *sp = (uint64_t *)t->rsp;
    *(--sp) = (uint64_t)entry;
    *(--sp) = (uint64_t)USER_STACK_TOP;
    extern void start_user(void);
    *(--sp) = (uint64_t)start_user;
    t->rsp = sp;

    t->cr3 = pml4_phys;
    t->stack_phys  = p1;
    t->stack_phys2 = p2;
    log_printf(LOG_LEVEL_INFO, "Created user task pid=%d entry=%p cr3=%p\n",
               t->pid, entry, (void *)(uintptr_t)pml4_phys);
    return t->pid;
}
