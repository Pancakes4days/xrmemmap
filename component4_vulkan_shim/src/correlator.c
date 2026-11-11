/*
 * correlator.c — full address-chain resolution for Vulkan allocations
 */

#include "correlator.h"
#include "pagemap.h"

#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

static drama_bank_fn_t s_bank_fn;
static int s_bank_fn_set;

void xrmemmap_set_bank_fn(const drama_bank_fn_t *fn)
{
    s_bank_fn = *fn;
    s_bank_fn_set = 1;
}

int xrmemmap_resolve(uint64_t cpu_va, uint64_t size,
                     const kgsl_mapping_t *mappings, int nmappings,
                     xrmemmap_alloc_t *out)
{
    memset(out, 0, sizeof(*out));
    out->cpu_va = cpu_va;
    out->size   = size;

    const kgsl_mapping_t *m =
        kgsl_trace_find_by_useraddr(mappings, nmappings, cpu_va);
    if (!m)
        return -1;

    out->gpu_va  = m->gpuaddr;
    out->kgsl_id = m->kgsl_id;
    memcpy(out->usage, m->usage, sizeof(out->usage));

    size_t page_size = pagemap_page_size();
    out->npages = (size + page_size - 1) / page_size;
    out->pages  = calloc(out->npages, sizeof(xrmemmap_page_t));
    if (!out->pages)
        return -1;

    uint64_t gpu_base = m->gpuaddr;
    uint64_t cpu_offset = cpu_va - m->useraddr;

    for (size_t i = 0; i < out->npages; i++) {
        uint64_t va = cpu_va + i * page_size;
        xrmemmap_page_t *pg = &out->pages[i];

        pg->cpu_va = va;
        pg->gpu_va = gpu_base + cpu_offset + i * page_size;
        pg->bank   = -1;

        pagemap_entry_t pme;
        uint64_t pa = pagemap_lookup((const void *)(uintptr_t)va, &pme);
        if (pa != PAGEMAP_INVALID_PADDR) {
            pg->phys_addr = pa;
            pg->pfn       = pme.pfn;
            pg->present   = 1;
            if (s_bank_fn_set)
                pg->bank = drama_get_bank(&s_bank_fn, pa);
        }
    }

    return 0;
}

void xrmemmap_free(xrmemmap_alloc_t *alloc)
{
    free(alloc->pages);
    alloc->pages  = NULL;
    alloc->npages = 0;
}

void xrmemmap_print(const xrmemmap_alloc_t *alloc, FILE *fp)
{
    fprintf(fp, "[xrmemmap] allocation: cpu_va=0x%" PRIx64
            "  gpu_va=0x%" PRIx64 "  size=%" PRIu64
            "  kgsl_id=%u  usage=%s\n",
            alloc->cpu_va, alloc->gpu_va, alloc->size,
            alloc->kgsl_id, alloc->usage);

    size_t resolved = 0;
    for (size_t i = 0; i < alloc->npages; i++) {
        const xrmemmap_page_t *pg = &alloc->pages[i];
        if (pg->present) {
            fprintf(fp, "[xrmemmap]   page[%3zu]  cpu=0x%" PRIx64
                    "  gpu=0x%" PRIx64 "  pa=0x%" PRIx64
                    "  pfn=0x%" PRIx64,
                    i, pg->cpu_va, pg->gpu_va, pg->phys_addr, pg->pfn);
            if (pg->bank >= 0)
                fprintf(fp, "  bank=%d", pg->bank);
            fprintf(fp, "\n");
            resolved++;
        } else {
            fprintf(fp, "[xrmemmap]   page[%3zu]  cpu=0x%" PRIx64
                    "  gpu=0x%" PRIx64 "  (unresolved)\n",
                    i, pg->cpu_va, pg->gpu_va);
        }
    }
    fprintf(fp, "[xrmemmap] %zu/%zu pages resolved\n", resolved, alloc->npages);
}
