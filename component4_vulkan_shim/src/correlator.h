/*
 * correlator.h — full address-chain resolution for Vulkan allocations
 *
 * Ties Components 1 (pagemap) and 2 (KGSL ftrace) together:
 *   CPU VA → GPU VA (from KGSL) → Physical PA (from pagemap)
 *
 * Used by the LD_PRELOAD shim and can be called directly.
 */

#ifndef XRMEMMAP_CORRELATOR_H
#define XRMEMMAP_CORRELATOR_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

#include "kgsl_ftrace.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint64_t cpu_va;
    uint64_t gpu_va;
    uint64_t phys_addr;
    uint64_t pfn;
    int      present;
} xrmemmap_page_t;

typedef struct {
    uint64_t         cpu_va;
    uint64_t         gpu_va;
    uint64_t         size;
    unsigned int     kgsl_id;
    char             usage[16];
    size_t           npages;
    xrmemmap_page_t *pages;     /* caller frees with xrmemmap_free() */
} xrmemmap_alloc_t;

/*
 * xrmemmap_resolve — resolve the full chain for a mapped allocation.
 *
 * cpu_va/size: the CPU virtual address range (from vkMapMemory).
 * mappings/nmappings: KGSL ftrace events (from kgsl_trace_read).
 *
 * Fills out->gpu_va from the KGSL mapping, allocates out->pages[]
 * and populates each page's cpu_va, gpu_va, and phys_addr via pagemap.
 *
 * Returns 0 on success, -1 if the CPU VA is not found in KGSL mappings.
 */
int xrmemmap_resolve(uint64_t cpu_va, uint64_t size,
                     const kgsl_mapping_t *mappings, int nmappings,
                     xrmemmap_alloc_t *out);

/* Free the pages array inside an alloc struct. */
void xrmemmap_free(xrmemmap_alloc_t *alloc);

/* Print the full chain in human-readable format. */
void xrmemmap_print(const xrmemmap_alloc_t *alloc, FILE *fp);

#ifdef __cplusplus
}
#endif

#endif /* XRMEMMAP_CORRELATOR_H */
