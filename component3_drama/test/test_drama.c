/*
 * test_drama.c — DRAMA timing attack for DRAM bank reverse-engineering
 *
 * Three-phase approach:
 *   Phase 1: Single-bit probing with cluster-based threshold
 *   Phase 2: XOR function detection (both-fast-alone, slow-together)
 *   Phase 3: Validate with increased rounds on key pairs
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

#define BUF_SIZE    (32 * 1024 * 1024)
#define ROUNDS      2000
#define ROUNDS_HI   5000              /* validation rounds */
#define MAX_PROBES  8
#define MIN_BIT     1                 /* PFN bit 1 = PA[13] */
#define MAX_BIT     18                /* PFN bit 18 = PA[30] */

typedef struct {
    int page_a;
    int page_b;
    uint64_t pfn_a;
    uint64_t pfn_b;
} probe_pair_t;

static int find_pairs_for_xor(const uint64_t *pfns, int npages,
                              uint64_t target_xor,
                              probe_pair_t *out, int max_out)
{
    int count = 0;
    for (int i = 0; i < npages && count < max_out; i++) {
        if (pfns[i] == 0) continue;
        uint64_t wanted = pfns[i] ^ target_xor;
        for (int j = i + 1; j < npages && count < max_out; j++) {
            if (pfns[j] == wanted) {
                out[count].page_a = i;
                out[count].page_b = j;
                out[count].pfn_a  = pfns[i];
                out[count].pfn_b  = pfns[j];
                count++;
            }
        }
    }
    return count;
}

static int compare_u64(const void *a, const void *b)
{
    uint64_t va = *(const uint64_t *)a;
    uint64_t vb = *(const uint64_t *)b;
    return (va > vb) - (va < vb);
}

static uint64_t probe_bit(void *buf, size_t page_size,
                          const uint64_t *pfns, int npages,
                          uint64_t target_xor, int rounds)
{
    probe_pair_t pairs[MAX_PROBES];
    int np = find_pairs_for_xor(pfns, npages, target_xor, pairs, MAX_PROBES);
    if (np == 0) return 0;

    uint64_t times[MAX_PROBES];
    for (int i = 0; i < np; i++) {
        volatile char *va = (volatile char *)buf + (size_t)pairs[i].page_a * page_size;
        volatile char *vb = (volatile char *)buf + (size_t)pairs[i].page_b * page_size;
        times[i] = drama_time_pair(va, vb, rounds);
    }
    qsort(times, np, sizeof(uint64_t), compare_u64);
    return times[np / 2];  /* median */
}

/*
 * Find the bank/row threshold in sorted timing values.
 *
 * LPDDR4 has 3 bank bits (8 banks), possibly +1 channel bit → 2-6 fast bits.
 * We look for the largest relative gap among splits that leave 2-6 values
 * below, which is the physically meaningful bank/row boundary.
 */
static uint64_t find_bank_threshold(const uint64_t *sorted, int n)
{
    if (n < 3) return sorted[0] + sorted[0] / 5;

    double best_ratio = 0;
    int best_idx = 1;
    int lo = 1;           /* at least 2 bank bits */
    int hi = n - 2;       /* at least 2 row bits */
    if (hi > 6) hi = 6;   /* at most 6 bank bits (channel+bank) */

    for (int i = lo; i <= hi && i < n - 1; i++) {
        if (sorted[i] == 0) continue;
        double ratio = (double)sorted[i + 1] / (double)sorted[i];
        if (ratio > best_ratio) {
            best_ratio = ratio;
            best_idx = i;
        }
    }
    return (sorted[best_idx] + sorted[best_idx + 1]) / 2;
}

int main(void)
{
    printf("xrmemmap Component 3 — DRAMA bank function reverse-engineering\n");
    printf("uid: %d (%s)\n\n", getuid(),
           getuid() == 0 ? "root" : "NOT ROOT — will fail");

    if (drama_init(-1) < 0) {
        fprintf(stderr, "FATAL: drama_init failed\n");
        return 1;
    }

    printf("[1] Allocating %d MiB buffer...\n", BUF_SIZE / (1024 * 1024));
    void *buf = mmap(NULL, BUF_SIZE, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
    if (buf == MAP_FAILED) {
        perror("mmap");
        return 1;
    }

    size_t page_size = pagemap_page_size();
    size_t npages = BUF_SIZE / page_size;
    volatile char *p = buf;
    for (size_t i = 0; i < npages; i++)
        p[i * page_size] = (char)i;

    printf("[2] Resolving %zu pages via pagemap...\n", npages);
    uint64_t *pfns = calloc(npages, sizeof(uint64_t));
    int resolved = 0;
    for (size_t i = 0; i < npages; i++) {
        pagemap_entry_t pme;
        void *va = (char *)buf + i * page_size;
        uint64_t pa = pagemap_lookup(va, &pme);
        if (pa != PAGEMAP_INVALID_PADDR) {
            pfns[i] = pa >> 12;
            resolved++;
        }
    }
    printf("    %d/%zu pages resolved\n", resolved, npages);

    uint64_t pfn_min = UINT64_MAX, pfn_max = 0;
    for (size_t i = 0; i < npages; i++) {
        if (pfns[i] == 0) continue;
        if (pfns[i] < pfn_min) pfn_min = pfns[i];
        if (pfns[i] > pfn_max) pfn_max = pfns[i];
    }
    printf("    PFN range: 0x%" PRIx64 " - 0x%" PRIx64 " (span 0x%" PRIx64
           " = %" PRIu64 " MiB)\n\n",
           pfn_min, pfn_max, pfn_max - pfn_min,
           (pfn_max - pfn_min) * page_size / (1024 * 1024));

    /* ================================================================
     * Phase 1: Single-bit probing
     * ================================================================ */
    printf("=== Phase 1: Single-bit probing (PA[13]-PA[30], %d rounds) ===\n\n",
           ROUNDS);

    uint64_t bit_times[32] = {0};
    int bit_npairs[32] = {0};

    for (int bit = MIN_BIT; bit <= MAX_BIT; bit++) {
        probe_pair_t pairs[MAX_PROBES];
        int np = find_pairs_for_xor(pfns, (int)npages, 1ULL << bit,
                                    pairs, MAX_PROBES);
        bit_npairs[bit] = np;
        if (np == 0) continue;

        uint64_t times[MAX_PROBES];
        for (int i = 0; i < np; i++) {
            volatile char *va = (volatile char *)buf + (size_t)pairs[i].page_a * page_size;
            volatile char *vb = (volatile char *)buf + (size_t)pairs[i].page_b * page_size;
            times[i] = drama_time_pair(va, vb, ROUNDS);
        }
        qsort(times, np, sizeof(uint64_t), compare_u64);
        bit_times[bit] = times[np / 2];
    }

    /* Cluster-based threshold: sort the medians and find the biggest gap */
    uint64_t sorted_times[32];
    int nsorted = 0;
    for (int bit = MIN_BIT; bit <= MAX_BIT; bit++) {
        if (bit_times[bit] > 0)
            sorted_times[nsorted++] = bit_times[bit];
    }
    qsort(sorted_times, nsorted, sizeof(uint64_t), compare_u64);

    uint64_t threshold = find_bank_threshold(sorted_times, nsorted);
    printf("    Auto-threshold: %" PRIu64 " ns (bank/row gap, expecting 2-6 bank bits)\n\n",
           threshold);

    printf("    %-8s  %-6s  %-10s  %s\n", "Bit", "Pairs", "Median(ns)", "Class");
    printf("    %-8s  %-6s  %-10s  %s\n", "---", "-----", "---------", "-----");

    int is_bank_bit[32] = {0};
    int nbank = 0;
    for (int bit = MIN_BIT; bit <= MAX_BIT; bit++) {
        int pa_bit = bit + 12;
        if (bit_npairs[bit] == 0) {
            printf("    PA[%-3d]  0       -           (no pairs)\n", pa_bit);
            continue;
        }
        int fast = (bit_times[bit] < threshold);
        if (fast) {
            is_bank_bit[pa_bit] = 1;
            nbank++;
        }
        printf("    PA[%-3d]  %-6d  %-10" PRIu64 "  %s\n",
               pa_bit, bit_npairs[bit], bit_times[bit],
               fast ? "BANK (fast)" : "ROW  (slow)");
    }

    printf("\n    Bank bits found: %d", nbank);
    if (nbank > 0) {
        printf(" →");
        for (int b = 13; b <= 30; b++)
            if (is_bank_bit[b]) printf(" PA[%d]", b);
    }
    printf("\n\n");

    /* ================================================================
     * Phase 2: XOR function detection
     *
     * For each bank bit B, test B^X for every other bit X.
     * If B alone is fast but B^X is slow, then X is the XOR partner
     * of B in the bank function: bank_bit = PA[B] XOR PA[X].
     * ================================================================ */
    printf("=== Phase 2: XOR partner search (%d rounds) ===\n\n", ROUNDS);

    typedef struct {
        int bank_bit;
        int xor_partner;
        uint64_t time_ns;
    } xor_hit_t;
    xor_hit_t xor_hits[64];
    int nxor = 0;

    for (int b = MIN_BIT; b <= MAX_BIT; b++) {
        int pa_b = b + 12;
        if (!is_bank_bit[pa_b]) continue;

        printf("    Testing PA[%d] ^ PA[X]:\n", pa_b);

        for (int x = MIN_BIT; x <= MAX_BIT; x++) {
            if (x == b) continue;
            int pa_x = x + 12;

            uint64_t target_xor = (1ULL << b) | (1ULL << x);
            uint64_t med = probe_bit(buf, page_size, pfns, (int)npages,
                                     target_xor, ROUNDS);
            if (med == 0) continue;

            if (med >= threshold) {
                printf("      ^ PA[%d]: %" PRIu64 " ns  *** SLOW (XOR partner candidate)\n",
                       pa_x, med);
                if (nxor < 64) {
                    xor_hits[nxor].bank_bit = pa_b;
                    xor_hits[nxor].xor_partner = pa_x;
                    xor_hits[nxor].time_ns = med;
                    nxor++;
                }
            }
        }
    }

    printf("\n");

    /* ================================================================
     * Phase 3: Validate XOR candidates with high-round timing
     * ================================================================ */
    if (nxor > 0) {
        printf("=== Phase 3: Validating %d XOR candidates (%d rounds) ===\n\n",
               nxor, ROUNDS_HI);

        for (int i = 0; i < nxor; i++) {
            int b = xor_hits[i].bank_bit - 12;
            int x = xor_hits[i].xor_partner - 12;
            uint64_t target_xor = (1ULL << b) | (1ULL << x);

            uint64_t med = probe_bit(buf, page_size, pfns, (int)npages,
                                     target_xor, ROUNDS_HI);
            if (med == 0) {
                printf("    PA[%d]^PA[%d]: no pairs\n",
                       xor_hits[i].bank_bit, xor_hits[i].xor_partner);
                continue;
            }

            const char *verdict;
            if (med >= threshold)
                verdict = "CONFIRMED — XOR bank function";
            else
                verdict = "rejected (now fast)";

            printf("    PA[%d]^PA[%d]: %" PRIu64 " ns → %s\n",
                   xor_hits[i].bank_bit, xor_hits[i].xor_partner,
                   med, verdict);
        }
    } else {
        printf("=== Phase 3: No XOR candidates to validate ===\n");
        printf("    Bank function appears to use simple (non-XOR) bit addressing.\n");
    }

    printf("\n");

    /* ================================================================
     * Summary
     * ================================================================ */
    printf("=== Derived DRAM Bank Function ===\n\n");
    printf("    Threshold: %" PRIu64 " ns\n", threshold);
    printf("    Simple bank bits:");
    for (int b = 13; b <= 30; b++)
        if (is_bank_bit[b]) printf(" PA[%d]", b);
    printf("\n");

    if (nxor > 0) {
        printf("\n    XOR bank function candidates:\n");
        for (int i = 0; i < nxor; i++) {
            printf("      bank_bit[?] = PA[%d] XOR PA[%d]  (%" PRIu64 " ns)\n",
                   xor_hits[i].bank_bit, xor_hits[i].xor_partner,
                   xor_hits[i].time_ns);
        }
    }

    printf("\n    Expected for LPDDR4: 3 bank bits (8 banks), possibly with XOR.\n");
    printf("    Snapdragon 835: 2 channels × 8 banks = 16 total.\n\n");

    /* Validate: run the library function and compare */
    printf("=== Library validation: drama_probe_bank_fn() ===\n\n");
    drama_bank_fn_t fn;
    int rc = drama_probe_bank_fn(buf, BUF_SIZE, pfns, (int)npages, ROUNDS, &fn);
    if (rc == 0) {
        printf("    Bank bits: %d, banks: %d\n", fn.nbits, fn.nbanks);
        printf("    Masks:");
        for (int i = 0; i < fn.nbits; i++)
            printf(" 0x%" PRIx64, fn.masks[i]);
        printf("\n");

        /* Show bank assignment for a few pages */
        printf("\n    Sample bank assignments (first 16 resolved pages):\n");
        int shown = 0;
        for (size_t i = 0; i < npages && shown < 16; i++) {
            if (pfns[i] == 0) continue;
            uint64_t pa = pfns[i] << 12;
            int bank = drama_get_bank(&fn, pa);
            printf("      page %5zu  PA=0x%09" PRIx64 "  bank=%d\n",
                   i, pa, bank);
            shown++;
        }
    } else {
        printf("    drama_probe_bank_fn() returned %d (failed)\n", rc);
    }

    free(pfns);
    munmap(buf, BUF_SIZE);

    printf("\nDone.\n");
    return 0;
}
