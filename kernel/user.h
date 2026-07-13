#ifndef USER_H
#define USER_H

#include <stdint.h>

/* create user task with given entry, physical pml4 base, and optional user stack.
 * If user_stack is 0, a small 2-page stack is allocated. Otherwise, user_stack
 * is used as the initial stack top (the stack must already be mapped in pml4_phys). */
int create_user_task_from_entry(void (*entry)(void), uint64_t pml4_phys,
                                 uint64_t user_stack);

#endif
