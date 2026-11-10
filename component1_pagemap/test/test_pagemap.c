/*
 * test_pagemap.c — standalone exerciser for the pagemap reader.
 *
 * Not a unit-test framework (none on the target); a self-checking CLI that
 * prints what it resolves and exits non-zero on a hard failure. Run on the
 * Quest under root:
 *
 *   ./adb.exe push test_pagemap /data/local/tmp/
 *   ./adb.exe shell su -c /data/local/tmp/test_pagemap
 *
 * Without root, the PFN checks are expected to report EACCES; the test treats
 * that as a soft skip and says so, so it still runs usefully unprivileged.
 */

#include "pagemap.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static int failures = 0;

static void report(const char *label, const void *va)
{
    pagemap_entry_t e;
    errno = 0;
    uint64_t pa = pagemap_lookup(va, &e);

    printf("  %-18s VA=%p  present=%d swap=%d excl=%d dirty=%d",
           label, va, e.present, e.swapped, e.exclusive, e.soft_dirty);

    if (pa != PAGEMAP_INVALID_PADDR) {
        printf("  PFN=0x%llx  PA=0x%llx\n",
               (unsigned long long)e.pfn, (unsigned long long)pa);
    } else {
        printf("  PA=unresolved (%s)\n", strerror(errno));
        if (errno == EACCES)
            printf("      -> present but PFN hidden: run as root (CAP_SYS_ADMIN)\n");
    }
}

int main(void)
{
    const size_t ps = pagemap_page_size();
    printf("xrmemmap Component 1 — pagemap reader self-test\n");
    printf("page size: %zu bytes\n", ps);
    printf("uid: %d (%s)\n\n", (int)getuid(),
           getuid() == 0 ? "root" : "unprivileged — PFNs may be hidden");

    /* 1. A multi-page anonymous mapping, touched so every page faults in. */
    const size_t npages = 4;
    size_t len = npages * ps;
    unsigned char *buf = mmap(NULL, len, PROT_READ | PROT_WRITE,
                              MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (buf == MAP_FAILED) {
        perror("mmap");
        return 2;
    }
    for (size_t i = 0; i < len; i++)
        buf[i] = (unsigned char)i;   /* dirty every page */

    printf("[anon mmap, %zu pages, touched]\n", npages);
    for (size_t i = 0; i < npages; i++) {
        char lbl[32];
        snprintf(lbl, sizeof(lbl), "page[%zu]", i);
        report(lbl, buf + i * ps);
    }

    /* 2. An untouched page: reserve but never fault in, expect not-present. */
    unsigned char *lazy = mmap(NULL, ps, PROT_READ | PROT_WRITE,
                               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (lazy != MAP_FAILED) {
        printf("\n[anon mmap, untouched — expect not-present]\n");
        pagemap_entry_t e;
        errno = 0;
        uint64_t pa = pagemap_lookup(lazy, &e);
        if (pa == PAGEMAP_INVALID_PADDR && !e.present) {
            printf("  OK: untouched page reported not-present (%s)\n",
                   strerror(errno));
        } else if (getuid() != 0 && errno == EACCES) {
            printf("  SKIP: PFNs hidden unprivileged, cannot distinguish\n");
        } else {
            printf("  UNEXPECTED: pa=0x%llx present=%d\n",
                   (unsigned long long)pa, e.present);
            failures++;
        }
        munmap(lazy, ps);
    }

    /* 3. Range API over the touched buffer; sanity-check page count. */
    printf("\n[range lookup over touched buffer]\n");
    pagemap_entry_t entries[8];
    size_t got = sizeof(entries) / sizeof(entries[0]);
    if (pagemap_lookup_range(buf, len, entries, &got) == 0) {
        printf("  resolved %zu entries\n", got);
        if (got != npages) {
            printf("  FAIL: expected %zu entries, got %zu\n", npages, got);
            failures++;
        }
        if (getuid() == 0) {
            /* With root, confirm virtual-contiguous != physical-contiguous is
             * at least observable (informational, not a hard assertion). */
            int contig = 1;
            for (size_t i = 1; i < got; i++) {
                if (!entries[i].present || !entries[i - 1].present) { contig = 0; break; }
                if (entries[i].pfn != entries[i - 1].pfn + 1) contig = 0;
            }
            printf("  physically %scontiguous\n", contig ? "" : "NOT ");
        }
    } else {
        printf("  FAIL: range lookup errored (%s, need=%zu)\n",
               strerror(errno), got);
        failures++;
    }

    munmap(buf, len);

    printf("\n%s (%d hard failure%s)\n",
           failures == 0 ? "PASS" : "FAIL", failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
