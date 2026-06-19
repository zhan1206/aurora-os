#ifndef USER_H
#define USER_H

#include <stdint.h>

/* create user task with given entry and physical pml4 base */
int create_user_task_from_entry(void (*entry)(void), uint64_t pml4_phys);

#endif
