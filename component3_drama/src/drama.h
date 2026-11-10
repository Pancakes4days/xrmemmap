#ifndef XRMEMMAP_DRAMA_H
#define XRMEMMAP_DRAMA_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * drama_init — pin to a single core and verify cache flush works.
 * core: CPU core to pin to (-1 for default big core).
 * Returns 0 on success.
 */
int drama_init(int core);

/*
 * drama_flush — flush a cache line from all levels (DC CIVAC + DSB).
 */
void drama_flush(volatile void *addr);

/*
 * drama_time_pair — time alternating accesses between two addresses.
 *
 * Flushes both from cache, then alternates load A / load B for `rounds`
 * iterations, measuring total elapsed time.
 *
 * Returns average nanoseconds per access-pair (A+B+flush+flush).
 */
uint64_t drama_time_pair(volatile void *a, volatile void *b, int rounds);

typedef struct {
    int      page_a;
    int      page_b;
    uint64_t pa_a;
    uint64_t pa_b;
    uint64_t avg_ns;
    int      conflict;
} drama_pair_t;

/*
 * drama_scan — scan pairs of pages for row-buffer conflicts.
 *
 * buf:        base of a large mmap'd region
 * page_size:  system page size
 * pages[]:    indices of pages to test (into buf)
 * npick:      number of entries in pages[]
 * phys[]:     physical address for each page index
 * threshold:  nanoseconds above which a pair is classified as conflicting
 * out[]:      results (caller provides space for npick*(npick-1)/2)
 * max_out:    capacity of out[]
 *
 * Returns number of results written.
 */
int drama_scan(void *buf, size_t page_size,
               const int *pages, int npick,
               const uint64_t *phys, uint64_t threshold,
               drama_pair_t *out, int max_out);

/*
 * Derived bank addressing function.
 * Each masks[i] is a bitmask over the physical address: XOR of the bits
 * selected by the mask gives one bit of the bank index.
 */
typedef struct {
    int      nbits;
    uint64_t masks[16];
    int      nbanks;
} drama_bank_fn_t;

/*
 * drama_derive_bank_fn — from scan results, find the address bits that
 * determine bank selection (legacy brute-force approach).
 *
 * Returns 0 on success, -1 if insufficient data.
 */
int drama_derive_bank_fn(const drama_pair_t *pairs, int npairs,
                         const uint64_t *phys, int npages,
                         drama_bank_fn_t *out);

/*
 * drama_probe_bank_fn — targeted bit-probing approach.
 *
 * For each physical address bit PA[13]-PA[30], finds page pairs that
 * differ in exactly that bit, times them, and classifies bank vs row.
 * Then tests two-bit XOR combinations for XOR bank functions.
 *
 * buf:        base of a large mmap'd region (>= 16 MiB recommended)
 * buf_size:   size in bytes
 * pfns[]:     PFN for each page (pa >> 12), indexed by page number.
 *             Entry 0 means unresolved.
 * npages:     number of entries in pfns[]
 * rounds:     timing iterations per probe (2000+ recommended)
 * out:        result
 *
 * Returns 0 on success, -1 if not enough matching pairs found.
 */
int drama_probe_bank_fn(void *buf, size_t buf_size,
                        const uint64_t *pfns, int npages,
                        int rounds, drama_bank_fn_t *out);

/* Apply derived bank function to a physical address. */
int drama_get_bank(const drama_bank_fn_t *fn, uint64_t pa);

#ifdef __cplusplus
}
#endif

#endif /* XRMEMMAP_DRAMA_H */
