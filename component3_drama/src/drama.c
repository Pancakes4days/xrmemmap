#define _GNU_SOURCE
#include "drama.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sched.h>
#include <signal.h>
#include <setjmp.h>
#include <unistd.h>

/* --- Cache flush --- */

static sigjmp_buf s_flush_jmp;
static volatile int s_flush_works = -1; /* -1 = untested */

static void sigill_handler(int sig)
{
    (void)sig;
    siglongjmp(s_flush_jmp, 1);
}

void drama_flush(volatile void *addr)
{
    __asm__ volatile(
        "dc civac, %0\n"
        "dsb sy\n"
        :
        : "r"(addr)
        : "memory"
    );
}

static int test_flush(void)
{
    volatile char dummy = 0;
    struct sigaction sa, old;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigill_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGILL, &sa, &old);

    if (sigsetjmp(s_flush_jmp, 1) == 0) {
        drama_flush(&dummy);
        s_flush_works = 1;
    } else {
        s_flush_works = 0;
    }

    sigaction(SIGILL, &old, NULL);
    return s_flush_works;
}

/* --- Timing --- */

static inline uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

uint64_t drama_time_pair(volatile void *a, volatile void *b, int rounds)
{
    volatile char *pa = (volatile char *)a;
    volatile char *pb = (volatile char *)b;

    /* Warmup */
    for (int i = 0; i < 10; i++) {
        drama_flush(a);
        drama_flush(b);
        (void)*pa;
        (void)*pb;
    }

    uint64_t start = now_ns();
    for (int i = 0; i < rounds; i++) {
        (void)*pa;
        (void)*pb;
        drama_flush(a);
        drama_flush(b);
    }
    uint64_t elapsed = now_ns() - start;

    return elapsed / (uint64_t)rounds;
}

/* --- Init --- */

int drama_init(int core)
{
    /* Pin to a single core to avoid migration noise */
    if (core < 0) {
        /* Default: pick a big core. On SD835, cores 4-7 are typically big
         * (Kryo 280 Gold / A73). Check /proc/cpuinfo part 0x800. */
        core = 4;
    }

    cpu_set_t mask;
    CPU_ZERO(&mask);
    CPU_SET(core, &mask);
    if (sched_setaffinity(0, sizeof(mask), &mask) < 0) {
        fprintf(stderr, "[drama] WARNING: sched_setaffinity(core=%d) failed\n", core);
    }

    /* Set real-time priority for stable timing */
    struct sched_param sp = { .sched_priority = 50 };
    if (sched_setscheduler(0, SCHED_FIFO, &sp) < 0) {
        fprintf(stderr, "[drama] WARNING: SCHED_FIFO failed (timing may be noisy)\n");
    }

    if (!test_flush()) {
        fprintf(stderr, "[drama] FATAL: DC CIVAC not available at EL0\n");
        return -1;
    }

    fprintf(stderr, "[drama] init: pinned to core %d, DC CIVAC ok\n", core);
    return 0;
}

/* --- Scan --- */

int drama_scan(void *buf, size_t page_size,
               const int *pages, int npick,
               const uint64_t *phys, uint64_t threshold,
               drama_pair_t *out, int max_out)
{
    int count = 0;

    for (int i = 0; i < npick && count < max_out; i++) {
        for (int j = i + 1; j < npick && count < max_out; j++) {
            volatile char *a = (volatile char *)buf + (size_t)pages[i] * page_size;
            volatile char *b = (volatile char *)buf + (size_t)pages[j] * page_size;

            uint64_t t = drama_time_pair(a, b, 1000);

            out[count].page_a   = pages[i];
            out[count].page_b   = pages[j];
            out[count].pa_a     = phys[pages[i]];
            out[count].pa_b     = phys[pages[j]];
            out[count].avg_ns   = t;
            out[count].conflict = (t >= threshold) ? 1 : 0;
            count++;
        }
    }

    return count;
}

/* --- Bank function derivation --- */

static int popcount64(uint64_t x)
{
    int c = 0;
    while (x) { c += x & 1; x >>= 1; }
    return c;
}

static int xor_bits(uint64_t addr, uint64_t mask)
{
    return popcount64(addr & mask) & 1;
}

int drama_derive_bank_fn(const drama_pair_t *pairs, int npairs,
                         const uint64_t *phys, int npages,
                         drama_bank_fn_t *out)
{
    (void)phys;
    (void)npages;
    memset(out, 0, sizeof(*out));

    /*
     * For each candidate bit position (6..20), test if it distinguishes
     * conflicting from non-conflicting pairs. A bit is a bank-select bit
     * if pairs that differ in that bit (XOR=1) conflict less often than
     * pairs that agree on it.
     *
     * More precisely: for a single-bit mask, if flipping that bit changes
     * the bank, then pairs differing in that bit are more likely to be in
     * different banks (no conflict). Pairs agreeing on all bank bits are
     * in the same bank (conflict if different row).
     *
     * We also test 2-bit XOR masks (the common DRAM scheme).
     */

    /* Collect candidate single bits and 2-bit XORs */
    uint64_t candidates[256];
    int ncand = 0;

    /* Single bits 13..30 (bits 0-11 are page offset, always 0 for 4K pages;
     * bit 12 is the first that varies between pages) */
    for (int b = 13; b <= 30 && ncand < 256; b++)
        candidates[ncand++] = 1ULL << b;

    /* Two-bit XOR masks: common DRAM addressing uses row XOR bank */
    for (int b1 = 13; b1 <= 22 && ncand < 256; b1++)
        for (int b2 = b1 + 1; b2 <= 30 && ncand < 256; b2++)
            candidates[ncand++] = (1ULL << b1) | (1ULL << b2);

    /* Score each candidate: how well does it separate conflicts from non-conflicts */
    int nbits = 0;

    for (int c = 0; c < ncand && nbits < 16; c++) {
        uint64_t mask = candidates[c];

        /* Count: how many conflict pairs have xor_bits=0 vs =1?
         * And how many non-conflict pairs? */
        int conf_same = 0, conf_diff = 0;

        for (int p = 0; p < npairs; p++) {
            int xb = xor_bits(pairs[p].pa_a ^ pairs[p].pa_b, mask);
            if (pairs[p].conflict) {
                if (xb == 0) conf_same++;
                else          conf_diff++;
            }
        }

        /* A good bank bit: conflicts happen mostly when this bit-function
         * gives the same value for both addresses (same bank).
         * So conf_same >> conf_diff, and the bit also separates non-conflicts. */
        int total_conf = conf_same + conf_diff;
        if (total_conf < 5)
            continue;

        double conf_ratio = (double)conf_same / (double)total_conf;

        /* Check it's not redundant with already-found bits */
        int redundant = 0;
        for (int b = 0; b < nbits; b++) {
            /* If XOR of this candidate with an existing mask gives a mask
             * with no additional info, skip */
            if (candidates[c] == out->masks[b]) {
                redundant = 1;
                break;
            }
        }

        if (!redundant && conf_ratio > 0.70) {
            out->masks[nbits++] = mask;
        }
    }

    out->nbits  = nbits;
    out->nbanks = 1 << nbits;
    return nbits > 0 ? 0 : -1;
}

int drama_get_bank(const drama_bank_fn_t *fn, uint64_t pa)
{
    int bank = 0;
    for (int i = 0; i < fn->nbits; i++)
        bank |= xor_bits(pa, fn->masks[i]) << i;
    return bank;
}
