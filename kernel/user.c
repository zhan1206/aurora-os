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

int create_user_task_from_entry(void (*entry)(void), uint64_t pml4_phys,
                                 uint64_t user_stack) {
    if (!entry || !pml4_phys) return -1;

    uint64_t USER_STACK_TOP = user_stack;
    void *p1 = NULL;
    void *p2 = NULL;

    if (user_stack == 0) {
        /* Allocate a small 2-page stack for simple tasks */
        p1 = alloc_page();
        p2 = alloc_page();
        if (!p1 || !p2) {
            if (p1) free_page(p1);
            if (p2) free_page(p2);
            return -1;
        }

        USER_STACK_TOP = aslr_randomize_stack();
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
    }

    struct task_struct *t = create_task(user_trampoline);
    if (!t) {
        if (p1) free_page(p1);
        if (p2) free_page(p2);
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

    /* FIXED (v4.1.4): Reset signal handlers on exec (BUG 4.5).
     * POSIX requires caught signals to reset to SIG_DFL on exec. */
    extern void signal_reset_on_exec(struct task_struct *task);
    signal_reset_on_exec(t);

    /* FIXED (v4.1.4): Transfer VMAs from the calling task (current) to
     * the new task.  The VMAs were registered during elf_load_core for
     * each PT_LOAD segment.  This ensures the page fault handler can
     * validate lazy allocations.  (BUG 3.1) */
    extern void vma_free_all(struct task_struct *task);
    t->vm_areas = current->vm_areas;
    current->vm_areas = NULL;

    /* Register VMA for the user stack (if we allocated one) */
    if (p1 && p2 && user_stack == 0) {
        extern int vma_register(struct task_struct *task, uint64_t start, uint64_t end, uint64_t flags);
        uint64_t stack_base = USER_STACK_TOP - 2 * 4096;
        vma_register(t, stack_base, USER_STACK_TOP, VM_READ | VM_WRITE | VM_GROWSDOWN);
    }

    log_printf(LOG_LEVEL_INFO, "Created user task pid=%d entry=%p cr3=%p\n",
               t->pid, entry, (void *)(uintptr_t)pml4_phys);
    return t->pid;
}
