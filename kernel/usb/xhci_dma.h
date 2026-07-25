/*
 * xhci_dma.h - xHCI DMA memory allocation helpers
 * /* XHCI_DMA (v4.2.7) */
 *
 * Provides DMA-safe allocation routines for xHCI data structures.
 * Ensures physical contiguity and identity mapping (virtual == physical)
 * which is required by the xHCI hardware for TRB rings, device contexts,
 * DCBAA, event rings, and scratchpad buffers.
 *
 * All allocations are zero-initialized and come from the kernel heap.
 * The identity-mapping guarantee relies on the kernel's memory model
 * where the first KERNEL_PHYS_MAX (1 GB) is identity-mapped.
 */
#ifndef XHCI_DMA_H
#define XHCI_DMA_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "../mem.h"

/*
 * xhci_dma_alloc: Allocate a DMA buffer for xHCI hardware use.
 *
 * @size:      size in bytes to allocate.
 * @phys_out:  output parameter — receives the physical address of the
 *             allocated buffer.  The caller must pass this to the
 *             xHCI hardware registers.
 *
 * Returns: virtual address of the zero-initialized buffer, or NULL on
 *          allocation failure.  The buffer is identity-mapped (virt == phys).
 */
static inline void *xhci_dma_alloc(size_t size, uint64_t *phys_out) {
    void *virt = kmalloc(size);
    if (!virt) return NULL;
    memset(virt, 0, size);
    /* Identity mapping: virtual == physical */
    *phys_out = (uint64_t)(uintptr_t)virt;
    return virt;
}

/*
 * xhci_dma_free: Free a DMA buffer previously allocated by xhci_dma_alloc.
 *
 * @virt: virtual address returned by xhci_dma_alloc.  NULL is safe.
 */
static inline void xhci_dma_free(void *virt) {
    if (virt) kfree(virt);
}

#endif /* XHCI_DMA_H */