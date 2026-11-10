/*
 * pagemap.h — CPU virtual-to-physical address resolution via /proc/self/pagemap
 *
 * Part of xrmemmap. Resolves a userspace virtual address to its backing
 * physical address by parsing the kernel's pagemap interface.
 *
 * Requires CAP_SYS_ADMIN (root) to observe non-zero PFNs. Since Linux 4.0,
 * unprivileged reads of pagemap return a zeroed PFN field (present/swap bits
 * remain visible) as a side-channel hardening measure. See
 * Documentation/admin-guide/mm/pagemap.rst in the kernel tree.
 */

#ifndef XRMEMMAP_PAGEMAP_H
#define XRMEMMAP_PAGEMAP_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Sentinel returned by virt_to_phys() when a physical address cannot be
 * resolved (page not present, swapped out, permission denied, or read error).
 * Callers must check against this before using the result. errno is set. */
#define PAGEMAP_INVALID_PADDR ((uint64_t)-1)

/* Decoded fields of a single 64-bit pagemap entry.
 * Bit layout per kernel ABI:
 *   bit  63      page present (in RAM)
 *   bit  62      page swapped
 *   bit  61      page is file-backed or shared-anon
 *   bit  57      pte is uffd-wp write-protected (kernel >= 5.7)
 *   bit  56      page exclusively mapped (kernel >= 4.2)
 *   bit  55      pte is soft-dirty
 *   bits 0-54    PFN if present; swap type/offset if swapped
 */
typedef struct {
    uint64_t pfn;        /* page frame number (valid only if present) */
    uint64_t raw;        /* raw 64-bit entry, for callers that want more */
    int      present;    /* bit 63 */
    int      swapped;    /* bit 62 */
    int      file_page;  /* bit 61 */
    int      exclusive;  /* bit 56 */
    int      soft_dirty; /* bit 55 */
} pagemap_entry_t;

/*
 * virt_to_phys — resolve the physical address backing a virtual address.
 *
 * vaddr: any virtual address in the calling process's address space. The page
 *        it falls in should be resident; touch it first (read or write) to
 *        fault it in, as unmapped/lazy pages report not-present.
 *
 * Returns the physical address (PFN * page_size + page offset) on success, or
 * PAGEMAP_INVALID_PADDR on failure with errno set:
 *   EACCES  - PFN read back as zero while present (likely unprivileged: need root)
 *   ENODATA - page not present and not swapped (never faulted in)
 *   EIO     - short or failed read of /proc/self/pagemap
 *   others  - as set by open()/pread()
 */
uint64_t virt_to_phys(const void *vaddr);

/*
 * pagemap_lookup — like virt_to_phys but also returns the decoded entry.
 *
 * Fills *out with the decoded pagemap entry regardless of success, so callers
 * can inspect present/swapped state even when no physical address is available.
 * out may be NULL if only the return value is wanted.
 *
 * Returns physical address on success, PAGEMAP_INVALID_PADDR otherwise (errno set).
 */
uint64_t pagemap_lookup(const void *vaddr, pagemap_entry_t *out);

/*
 * pagemap_lookup_range — resolve physical addresses for a contiguous VA range.
 *
 * Resolves one entry per page spanning [vaddr, vaddr + length). The caller
 * provides out[] with capacity for at least npages = (length rounded up to a
 * page, plus any straddle) entries; pass the count via *npages, which is
 * updated to the number actually written.
 *
 * Each out[i].raw holds the raw entry; unresolved pages carry present=0.
 * Returns 0 on success, -1 on error (errno set). Individual not-present pages
 * are not treated as errors — inspect per-entry flags.
 */
int pagemap_lookup_range(const void *vaddr, size_t length,
                         pagemap_entry_t *out, size_t *npages);

/* Return the system page size (cached). Thin wrapper over sysconf(_SC_PAGESIZE). */
size_t pagemap_page_size(void);

#ifdef __cplusplus
}
#endif

#endif /* XRMEMMAP_PAGEMAP_H */
