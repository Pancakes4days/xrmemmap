/*
 * pagemap.c — implementation of CPU virtual-to-physical resolution.
 *
 * See pagemap.h for the public contract. All reads target /proc/self/pagemap,
 * the calling process's own mapping, which is all Component 1 needs: the
 * Vulkan test program (Component 4) links this in-process and resolves its own
 * mapped allocations.
 */

#include "pagemap.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

/* Pagemap entry bit positions (kernel ABI, stable). */
#define PM_PRESENT_BIT   63
#define PM_SWAP_BIT      62
#define PM_FILE_BIT      61
#define PM_EXCLUSIVE_BIT 56
#define PM_SOFTDIRTY_BIT 55
#define PM_PFN_MASK      ((1ULL << 55) - 1)   /* bits 0..54 */

#define BIT(x, n) (((x) >> (n)) & 1ULL)

static size_t g_page_size = 0;

size_t pagemap_page_size(void)
{
    if (g_page_size == 0) {
        long ps = sysconf(_SC_PAGESIZE);
        g_page_size = (ps > 0) ? (size_t)ps : 4096;
    }
    return g_page_size;
}

static void decode_entry(uint64_t raw, pagemap_entry_t *e)
{
    e->raw        = raw;
    e->present    = (int)BIT(raw, PM_PRESENT_BIT);
    e->swapped    = (int)BIT(raw, PM_SWAP_BIT);
    e->file_page  = (int)BIT(raw, PM_FILE_BIT);
    e->exclusive  = (int)BIT(raw, PM_EXCLUSIVE_BIT);
    e->soft_dirty = (int)BIT(raw, PM_SOFTDIRTY_BIT);
    e->pfn        = e->present ? (raw & PM_PFN_MASK) : 0;
}

/* Read a single raw pagemap entry for the page containing vaddr.
 * Returns 0 on success (raw filled), -1 on error (errno set). */
static int read_raw_entry(int fd, const void *vaddr, uint64_t *raw)
{
    const size_t ps = pagemap_page_size();
    uint64_t vpn = (uint64_t)(uintptr_t)vaddr / ps;
    off_t offset = (off_t)(vpn * sizeof(uint64_t));

    uint64_t entry = 0;
    ssize_t n = pread(fd, &entry, sizeof(entry), offset);
    if (n < 0)
        return -1;                 /* errno from pread */
    if (n != (ssize_t)sizeof(entry)) {
        errno = EIO;               /* short read: unexpected for pagemap */
        return -1;
    }
    *raw = entry;
    return 0;
}

uint64_t pagemap_lookup(const void *vaddr, pagemap_entry_t *out)
{
    pagemap_entry_t local;
    pagemap_entry_t *e = out ? out : &local;

    int fd = open("/proc/self/pagemap", O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        /* leave errno from open; zero the entry so callers don't read stale */
        decode_entry(0, e);
        return PAGEMAP_INVALID_PADDR;
    }

    uint64_t raw = 0;
    int rc = read_raw_entry(fd, vaddr, &raw);
    int saved = errno;
    close(fd);

    if (rc != 0) {
        decode_entry(0, e);
        errno = saved;
        return PAGEMAP_INVALID_PADDR;
    }

    decode_entry(raw, e);

    if (!e->present) {
        /* Not resident. Swapped is a distinct, non-fatal-but-unresolvable state. */
        errno = ENODATA;
        return PAGEMAP_INVALID_PADDR;
    }

    if (e->pfn == 0) {
        /* Present but PFN zeroed: almost always the unprivileged-hardening
         * path (kernel >= 4.0). Genuine PFN 0 is not user-mappable, so this
         * is a reliable "need root" signal on this platform. */
        errno = EACCES;
        return PAGEMAP_INVALID_PADDR;
    }

    const size_t ps = pagemap_page_size();
    uint64_t page_offset = (uint64_t)(uintptr_t)vaddr & (ps - 1);
    return e->pfn * ps + page_offset;
}

uint64_t virt_to_phys(const void *vaddr)
{
    return pagemap_lookup(vaddr, NULL);
}

int pagemap_lookup_range(const void *vaddr, size_t length,
                         pagemap_entry_t *out, size_t *npages)
{
    if (out == NULL || npages == NULL) {
        errno = EINVAL;
        return -1;
    }

    const size_t ps = pagemap_page_size();
    uintptr_t start = (uintptr_t)vaddr;
    uintptr_t end   = start + length;                 /* exclusive */
    uintptr_t first_page = start & ~(uintptr_t)(ps - 1);
    uintptr_t last_page  = (end - 1) & ~(uintptr_t)(ps - 1);
    size_t need = (length == 0) ? 0 : (size_t)((last_page - first_page) / ps) + 1;

    if (need > *npages) {
        *npages = need;
        errno = ERANGE;
        return -1;
    }

    int fd = open("/proc/self/pagemap", O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;

    int rc = 0;
    size_t i = 0;
    for (uintptr_t p = first_page; i < need; p += ps, i++) {
        uint64_t raw = 0;
        if (read_raw_entry(fd, (const void *)p, &raw) != 0) {
            rc = -1;
            break;
        }
        decode_entry(raw, &out[i]);
    }
    int saved = errno;
    close(fd);

    *npages = i;
    if (rc != 0)
        errno = saved;
    return rc;
}
