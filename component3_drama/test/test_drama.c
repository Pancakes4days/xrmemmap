/*
 * test_drama.c — DRAMA timing attack for DRAM bank reverse-engineering
 *
 * Allocates a large buffer, resolves physical addresses, scans page pairs
 * for row-buffer conflicts, and derives the bank addressing function.
 *
 * Must run as root on Quest 1:
 *   su -c /data/local/tmp/test_drama
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/mman.h>
#include <inttypes.h>

#include "pagemap.h"
#include "drama.h"

#define BUF_SIZE    (4 * 1024 * 1024)  /* 4 MiB */
#define NPICK       48                  /* pages to sample */
#define MAX_PAIRS   (NPICK * (NPICK - 1) / 2)
#define ROUNDS      1000

static int compare_pair_by_ns(const void *a, const void *b)
{
    const drama_pair_t *pa = a, *pb = b;
    if (pa->avg_ns < pb->avg_ns) return -1;
    if (pa->avg_ns > pb->avg_ns) return 1;
    return 0;
}

int main(void)
{
    printf("xrmemmap Component 3 — DRAMA timing attack\n");
    printf("uid: %d (%s)\n\n", getuid(),
           getuid() == 0 ? "root" : "NOT ROOT — will fail");

    /* Step 1: Initialize timing infrastructure */
    printf("[1] Initializing...\n");
    if (drama_init(-1) < 0) {
        fprintf(stderr, "FATAL: drama_init failed\n");
        return 1;
    }

    /* Step 2: Allocate a large buffer */
    printf("[2] Allocating %d KiB buffer...\n", BUF_SIZE / 1024);
    void *buf = mmap(NULL, BUF_SIZE, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
    if (buf == MAP_FAILED) {
        perror("mmap");
        return 1;
    }

    /* Touch every page */
    size_t page_size = pagemap_page_size();
    size_t npages = BUF_SIZE / page_size;
    volatile char *p = buf;
    for (size_t i = 0; i < npages; i++)
        p[i * page_size] = (char)i;

    /* Step 3: Resolve physical addresses */
    printf("[3] Resolving %zu pages via pagemap...\n", npages);
    uint64_t *phys = calloc(npages, sizeof(uint64_t));
    int resolved = 0;
    for (size_t i = 0; i < npages; i++) {
        pagemap_entry_t pme;
        void *va = (char *)buf + i * page_size;
        uint64_t pa = pagemap_lookup(va, &pme);
        if (pa != PAGEMAP_INVALID_PADDR) {
            phys[i] = pa;
            resolved++;
        }
    }
    printf("    %d/%zu pages resolved to physical addresses\n", resolved, npages);
    if (resolved < NPICK * 2) {
        fprintf(stderr, "FATAL: not enough resolved pages (need root)\n");
        munmap(buf, BUF_SIZE);
        free(phys);
        return 1;
    }

    /* Step 4: Pick a diverse sample of pages */
    printf("[4] Selecting %d sample pages...\n", NPICK);
    int pick[NPICK];
    int stride = (int)(npages / NPICK);
    for (int i = 0; i < NPICK; i++)
        pick[i] = i * stride;

    printf("    sample physical addresses:\n");
    for (int i = 0; i < NPICK; i++) {
        printf("      page[%3d] PA=0x%012" PRIx64 "\n", pick[i], phys[pick[i]]);
    }
    printf("\n");

    /* Step 5: Full scan (threshold 0 = classify later) */
    printf("[5] Scanning %d pairs (%d rounds each)...\n", MAX_PAIRS, ROUNDS);
    drama_pair_t *results = calloc(MAX_PAIRS, sizeof(drama_pair_t));
    int nresults = drama_scan(buf, page_size, pick, NPICK,
                              phys, 0, results, MAX_PAIRS);
    printf("    scanned %d pairs\n\n", nresults);

    /* Step 7: Timing histogram */
    printf("[7] Timing distribution:\n");
    qsort(results, nresults, sizeof(drama_pair_t), compare_pair_by_ns);

    /* Build histogram buckets of 10ns */
    uint64_t min_ns = results[0].avg_ns;
    uint64_t max_ns = results[nresults - 1].avg_ns;
    int nbuckets = (int)((max_ns - min_ns) / 10) + 1;
    if (nbuckets > 100) nbuckets = 100;
    uint64_t bwidth = (max_ns - min_ns + nbuckets - 1) / nbuckets;
    if (bwidth < 1) bwidth = 1;

    int *hist = calloc(nbuckets, sizeof(int));
    for (int i = 0; i < nresults; i++) {
        int b = (int)((results[i].avg_ns - min_ns) / bwidth);
        if (b >= nbuckets) b = nbuckets - 1;
        hist[b]++;
    }

    int max_count = 0;
    for (int i = 0; i < nbuckets; i++)
        if (hist[i] > max_count) max_count = hist[i];

    for (int i = 0; i < nbuckets; i++) {
        if (hist[i] == 0) continue;
        int bar = (hist[i] * 40) / (max_count > 0 ? max_count : 1);
        printf("    %4" PRIu64 "-%4" PRIu64 " ns  [%3d] ",
               min_ns + i * bwidth,
               min_ns + (i + 1) * bwidth - 1,
               hist[i]);
        for (int b = 0; b < bar; b++) putchar('#');
        putchar('\n');
    }
    /* Auto-detect threshold: find where the main cluster ends.
     * Walk rightward from the peak bucket. When a bucket's count drops
     * below 2% of the peak count, that's the cluster boundary. */
    int peak_bucket = 0;
    for (int i = 1; i < nbuckets; i++)
        if (hist[i] > hist[peak_bucket]) peak_bucket = i;

    int cutoff = hist[peak_bucket] / 50;  /* 2% of peak */
    if (cutoff < 2) cutoff = 2;

    int boundary = peak_bucket;
    for (int i = peak_bucket + 1; i < nbuckets; i++) {
        if (hist[i] < cutoff) {
            boundary = i;
            break;
        }
    }

    uint64_t threshold = min_ns + (uint64_t)(boundary + 1) * bwidth;
    printf("\n    peak bucket: %" PRIu64 "-%" PRIu64 " ns (%d pairs)\n",
           min_ns + (uint64_t)peak_bucket * bwidth,
           min_ns + (uint64_t)(peak_bucket + 1) * bwidth - 1,
           hist[peak_bucket]);
    printf("    auto threshold: %" PRIu64 " ns\n", threshold);

    free(hist);

    /* Reclassify with auto threshold */
    int nconflicts = 0;
    for (int i = 0; i < nresults; i++) {
        results[i].conflict = (results[i].avg_ns >= threshold) ? 1 : 0;
        if (results[i].conflict) nconflicts++;
    }

    printf("    total: %d pairs, %d conflicts (%.1f%%)\n\n",
           nresults, nconflicts,
           nresults > 0 ? 100.0 * nconflicts / nresults : 0.0);

    /* Step 8: Show conflict pairs */
    if (nconflicts > 0) {
        printf("[8] Conflict pairs (same bank, different row):\n");
        int shown = 0;
        for (int i = 0; i < nresults && shown < 20; i++) {
            if (!results[i].conflict) continue;
            uint64_t diff = results[i].pa_a ^ results[i].pa_b;
            printf("    PA=0x%012" PRIx64 " vs 0x%012" PRIx64
                   "  XOR=0x%012" PRIx64 "  %3" PRIu64 " ns\n",
                   results[i].pa_a, results[i].pa_b, diff,
                   results[i].avg_ns);
            shown++;
        }
        if (nconflicts > 20)
            printf("    ... (%d more)\n", nconflicts - 20);
        printf("\n");
    }

    /* Step 9: Derive bank function using only strong conflicts (>2.5x baseline) */
    uint64_t strong_thresh = results[0].avg_ns * 5 / 2;
    printf("[9] Deriving bank function (strong conflicts only, >%" PRIu64 " ns)...\n",
           strong_thresh);

    /* Build array of strong-only pairs for derivation */
    drama_pair_t *strong = calloc(nresults, sizeof(drama_pair_t));
    int nstrong = 0;
    for (int i = 0; i < nresults; i++) {
        strong[i] = results[i];
        strong[i].conflict = (results[i].avg_ns >= strong_thresh) ? 1 : 0;
        if (strong[i].conflict) nstrong++;
    }
    printf("    using %d strong conflicts out of %d total\n", nstrong, nconflicts);

    drama_bank_fn_t bank_fn;
    if (drama_derive_bank_fn(strong, nresults, phys, (int)npages, &bank_fn) == 0) {
        printf("    found %d bank-select bit(s) => %d banks\n",
               bank_fn.nbits, bank_fn.nbanks);
        for (int i = 0; i < bank_fn.nbits; i++) {
            printf("    bit %d: mask=0x%08" PRIx64 " (", i, bank_fn.masks[i]);
            int first = 1;
            for (int b = 0; b < 64; b++) {
                if (bank_fn.masks[i] & (1ULL << b)) {
                    if (!first) printf(" XOR ");
                    printf("PA[%d]", b);
                    first = 0;
                }
            }
            printf(")\n");
        }
        printf("\n");

        /* Show bank assignment for sample pages */
        printf("    sample page bank assignments:\n");
        for (int i = 0; i < NPICK; i++) {
            int bank = drama_get_bank(&bank_fn, phys[pick[i]]);
            printf("      page[%3d] PA=0x%012" PRIx64 " => bank %d\n",
                   pick[i], phys[pick[i]], bank);
        }
    } else {
        printf("    could not derive bank function (need more data or lower threshold)\n");
        printf("    manual analysis: examine XOR of conflicting PA pairs above\n");
    }

    /* Cleanup */
    free(strong);
    free(results);
    free(phys);
    munmap(buf, BUF_SIZE);

    printf("\nDone.\n");
    return 0;
}
