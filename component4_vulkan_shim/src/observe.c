/*
 * observe.c — observe a running process's Vulkan/KGSL memory from outside
 *
 * Reads /proc/<pid>/maps to find kgsl-3d0 backed regions, then reads
 * /proc/<pid>/pagemap to resolve physical addresses and DRAM bank.
 *
 * Requires root. Does not modify the target process.
 *
 * Usage: su -c "/data/local/tmp/observe <pid>"
 *        su -c "/data/local/tmp/observe"       (auto-find vrshell)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <inttypes.h>
#include <dirent.h>
#include <sys/mman.h>

#include "pagemap.h"
#include "drama.h"

#define MAX_REGIONS 256

typedef struct {
    uint64_t start;
    uint64_t end;
    char     perms[8];
    uint64_t offset;
    char     path[128];
} map_region_t;

static int parse_maps(pid_t pid, const char *filter,
                      map_region_t *out, int max_out)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/maps", pid);

    FILE *fp = fopen(path, "r");
    if (!fp) {
        fprintf(stderr, "Cannot open %s: %s\n", path, strerror(errno));
        return -1;
    }

    int count = 0;
    char line[512];
    while (fgets(line, sizeof(line), fp) && count < max_out) {
        if (filter && !strstr(line, filter))
            continue;

        map_region_t *r = &out[count];
        memset(r, 0, sizeof(*r));

        unsigned long long start, end, offset;
        int n = sscanf(line, "%llx-%llx %7s %llx %*s %*s %127[^\n]",
                       &start, &end, r->perms, &offset, r->path);
        if (n >= 3) {
            r->start  = start;
            r->end    = end;
            r->offset = offset;
            count++;
        }
    }
    fclose(fp);
    return count;
}

static int read_pagemap_entry(int pm_fd, uint64_t vaddr, uint64_t *pfn_out)
{
    size_t ps = pagemap_page_size();
    uint64_t vpn = vaddr / ps;
    off_t off = (off_t)(vpn * 8);

    uint64_t entry = 0;
    if (pread(pm_fd, &entry, 8, off) != 8)
        return -1;

    int present = (entry >> 63) & 1;
    if (!present) return -1;

    uint64_t pfn = entry & ((1ULL << 55) - 1);
    if (pfn == 0) return -1;

    *pfn_out = pfn;
    return 0;
}

static pid_t find_process(const char *name)
{
    DIR *d = opendir("/proc");
    if (!d) return -1;

    struct dirent *de;
    pid_t found = -1;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] < '1' || de->d_name[0] > '9') continue;

        char cmdpath[64];
        snprintf(cmdpath, sizeof(cmdpath), "/proc/%s/cmdline", de->d_name);
        FILE *fp = fopen(cmdpath, "r");
        if (!fp) continue;

        char cmd[256] = {0};
        size_t n = fread(cmd, 1, sizeof(cmd) - 1, fp);
        fclose(fp);
        if (n == 0) continue;

        if (strstr(cmd, name)) {
            found = atoi(de->d_name);
            break;
        }
    }
    closedir(d);
    return found;
}

int main(int argc, char *argv[])
{
    printf("xrmemmap process observer\n");
    printf("uid: %d (%s)\n\n", getuid(),
           getuid() == 0 ? "root" : "NOT ROOT — will fail");

    pid_t target;
    if (argc > 1) {
        target = atoi(argv[1]);
    } else {
        target = find_process("com.oculus.vrshell");
        if (target <= 0) {
            target = find_process("com.oculus.shellenv");
        }
        if (target <= 0) {
            fprintf(stderr, "No VR process found. Usage: %s <pid>\n", argv[0]);
            return 1;
        }
    }

    printf("Target PID: %d\n", target);

    /* Check the process exists */
    char cmdpath[64];
    snprintf(cmdpath, sizeof(cmdpath), "/proc/%d/cmdline", target);
    FILE *fp = fopen(cmdpath, "r");
    if (!fp) {
        fprintf(stderr, "Process %d not found\n", target);
        return 1;
    }
    char cmdline[256] = {0};
    fread(cmdline, 1, sizeof(cmdline) - 1, fp);
    fclose(fp);
    printf("Process: %s\n\n", cmdline);

    /* Find KGSL-backed regions */
    printf("[1] Scanning /proc/%d/maps for kgsl-3d0 regions...\n", target);
    map_region_t regions[MAX_REGIONS];
    int nregions = parse_maps(target, "kgsl-3d0", regions, MAX_REGIONS);
    if (nregions <= 0) {
        fprintf(stderr, "    No kgsl-3d0 regions found\n");
        /* Also show all GPU-related mappings */
        nregions = parse_maps(target, "kgsl", regions, MAX_REGIONS);
        if (nregions <= 0) {
            /* Show all mappings to help debug */
            printf("    Showing all mappings with 'gpu' or 'mali' or 'adreno':\n");
            parse_maps(target, "gpu", regions, 10);
        }
        return 1;
    }

    printf("    Found %d kgsl-3d0 region(s)\n\n", nregions);

    /* Summarize regions */
    uint64_t total_bytes = 0;
    for (int i = 0; i < nregions; i++) {
        map_region_t *r = &regions[i];
        uint64_t sz = r->end - r->start;
        total_bytes += sz;
        if (i < 20 || i == nregions - 1) {
            printf("    [%3d] 0x%012" PRIx64 "-0x%012" PRIx64
                   "  %s  off=0x%" PRIx64 "  size=%6" PRIu64 " KiB\n",
                   i, r->start, r->end, r->perms, r->offset, sz / 1024);
        } else if (i == 20) {
            printf("    ... (%d more) ...\n", nregions - 21);
        }
    }
    printf("    Total: %" PRIu64 " KiB in %d regions\n\n",
           total_bytes / 1024, nregions);

    /* Derive bank function */
    printf("[2] Deriving DRAM bank function...\n");
    if (drama_init(-1) < 0) {
        fprintf(stderr, "    drama_init failed\n");
        return 1;
    }

    size_t ps = pagemap_page_size();
    size_t probe_size = 32 * 1024 * 1024;
    size_t probe_pages = probe_size / ps;
    void *probe_buf = mmap(NULL, probe_size, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
    if (probe_buf == MAP_FAILED) {
        fprintf(stderr, "    probe mmap failed\n");
        return 1;
    }

    volatile char *p = probe_buf;
    for (size_t i = 0; i < probe_pages; i++)
        p[i * ps] = (char)i;

    uint64_t *probe_pfns = calloc(probe_pages, sizeof(uint64_t));
    for (size_t i = 0; i < probe_pages; i++) {
        pagemap_entry_t pme;
        uint64_t pa = pagemap_lookup((char *)probe_buf + i * ps, &pme);
        if (pa != PAGEMAP_INVALID_PADDR)
            probe_pfns[i] = pa >> 12;
    }

    drama_bank_fn_t bank_fn;
    int have_bank = 0;

    /* Use validated bank function: PA[13], PA[14], PA[15] = 8 banks.
     * Probing is noisy between runs (sometimes finds 3-5 bits).
     * This was validated stable across 3+ consecutive DRAMA runs. */
    bank_fn.nbits  = 3;
    bank_fn.masks[0] = 0x2000;  /* PA[13] */
    bank_fn.masks[1] = 0x4000;  /* PA[14] */
    bank_fn.masks[2] = 0x8000;  /* PA[15] */
    bank_fn.nbanks = 8;
    have_bank = 1;
    printf("    Using validated bank function: PA[13:15], 8 banks\n");

    free(probe_pfns);
    munmap(probe_buf, probe_size);

    /* Open target's pagemap */
    printf("\n[3] Resolving physical addresses for kgsl-3d0 pages...\n");
    char pm_path[64];
    snprintf(pm_path, sizeof(pm_path), "/proc/%d/pagemap", target);
    int pm_fd = open(pm_path, O_RDONLY);
    if (pm_fd < 0) {
        fprintf(stderr, "    Cannot open %s: %s\n", pm_path, strerror(errno));
        return 1;
    }

    /* Resolve pages in the first few interesting regions */
    int bank_hist[16] = {0};
    size_t total_resolved = 0;
    size_t total_pages = 0;
    int regions_shown = 0;

    for (int r = 0; r < nregions && regions_shown < 8; r++) {
        uint64_t sz = regions[r].end - regions[r].start;
        size_t npg = sz / ps;
        total_pages += npg;

        /* Only show detail for rw regions (likely Vulkan allocations) */
        if (regions[r].perms[0] != 'r' || regions[r].perms[1] != 'w')
            continue;

        regions_shown++;
        printf("\n    Region %d: 0x%" PRIx64 " (%zu pages, %s)\n",
               r, regions[r].start, npg, regions[r].perms);

        int resolved = 0;
        int max_show = (npg <= 16) ? (int)npg : 8;

        for (size_t pg = 0; pg < npg; pg++) {
            uint64_t va = regions[r].start + pg * ps;
            uint64_t pfn;
            if (read_pagemap_entry(pm_fd, va, &pfn) == 0) {
                resolved++;
                total_resolved++;
                uint64_t pa = pfn * ps;

                if ((int)pg < max_show || pg == npg - 1) {
                    printf("      page[%3zu]  va=0x%" PRIx64
                           "  pa=0x%09" PRIx64 "  pfn=0x%" PRIx64,
                           pg, va, pa, pfn);
                    if (have_bank) {
                        int bank = drama_get_bank(&bank_fn, pa);
                        printf("  bank=%d", bank);
                        if (bank < 16) bank_hist[bank]++;
                    }
                    printf("\n");
                } else if ((int)pg == max_show) {
                    printf("      ... (%zu more pages) ...\n", npg - max_show - 1);
                    if (have_bank) {
                        int bank = drama_get_bank(&bank_fn, pa);
                        if (bank < 16) bank_hist[bank]++;
                    }
                } else {
                    if (have_bank) {
                        int bank = drama_get_bank(&bank_fn, pa);
                        if (bank < 16) bank_hist[bank]++;
                    }
                }
            }
        }
        printf("      %d/%zu resolved\n", resolved, npg);
    }

    close(pm_fd);

    /* Summary */
    printf("\n=== Summary ===\n\n");
    printf("    Process: %s (PID %d)\n", cmdline, target);
    printf("    KGSL regions: %d (%" PRIu64 " KiB total)\n",
           nregions, total_bytes / 1024);
    printf("    Pages resolved: %zu/%zu\n", total_resolved, total_pages);

    if (have_bank && total_resolved > 0) {
        printf("    Bank distribution:\n");
        for (int b = 0; b < bank_fn.nbanks; b++) {
            printf("      bank %d: %d pages", b, bank_hist[b]);
            if (total_resolved > 0)
                printf(" (%.1f%%)", 100.0 * bank_hist[b] / total_resolved);
            printf("\n");
        }
    }

    printf("\nDone.\n");

    /* Restore SELinux */
    printf("\nNOTE: SELinux is permissive. Restore with: su -c setenforce 1\n");

    return 0;
}
