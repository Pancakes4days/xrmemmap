/*
 * shim.c — LD_PRELOAD interceptor for Vulkan memory calls
 *
 * Usage:
 *   LD_PRELOAD=/data/local/tmp/libxrmemmap.so /data/local/tmp/test_vulkan
 *
 * Intercepts vkAllocateMemory, vkMapMemory, vkUnmapMemory, vkFreeMemory.
 * At each vkMapMemory, reads KGSL ftrace events and pagemap to print the
 * full address chain: Vulkan handle → CPU VA → GPU VA → Physical PA.
 *
 * Output goes to stderr so it doesn't mix with the target app's stdout.
 */

#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <inttypes.h>

#include <vulkan/vulkan.h>

#include "correlator.h"
#include "pagemap.h"
#include "kgsl_ftrace.h"
#include "drama.h"

#include <sys/mman.h>

#define TAG "[xrmemmap] "
#define MAX_ALLOCS 256
#define BANK_PROBE_SIZE (32 * 1024 * 1024)

typedef struct {
    VkDeviceMemory handle;
    VkDeviceSize   size;
    uint32_t       mem_type;
    void          *mapped_ptr;
    VkDeviceSize   mapped_offset;
    VkDeviceSize   mapped_size;
    int            active;
    int            mapped;
} tracked_alloc_t;

static tracked_alloc_t s_allocs[MAX_ALLOCS];
static int s_tracing;
static int s_initialized;
static int s_bank_fn_ready;

/* Real Vulkan functions, resolved lazily via RTLD_NEXT */
static PFN_vkAllocateMemory   s_real_alloc;
static PFN_vkMapMemory        s_real_map;
static PFN_vkUnmapMemory      s_real_unmap;
static PFN_vkFreeMemory       s_real_free;

static void resolve_real_functions(void)
{
    if (s_initialized)
        return;
    s_real_alloc = (PFN_vkAllocateMemory)dlsym(RTLD_NEXT, "vkAllocateMemory");
    s_real_map   = (PFN_vkMapMemory)dlsym(RTLD_NEXT, "vkMapMemory");
    s_real_unmap = (PFN_vkUnmapMemory)dlsym(RTLD_NEXT, "vkUnmapMemory");
    s_real_free  = (PFN_vkFreeMemory)dlsym(RTLD_NEXT, "vkFreeMemory");
    s_initialized = 1;
}

static tracked_alloc_t *find_by_handle(VkDeviceMemory mem)
{
    for (int i = 0; i < MAX_ALLOCS; i++)
        if (s_allocs[i].active && s_allocs[i].handle == mem)
            return &s_allocs[i];
    return NULL;
}

static tracked_alloc_t *find_free_slot(void)
{
    for (int i = 0; i < MAX_ALLOCS; i++)
        if (!s_allocs[i].active)
            return &s_allocs[i];
    return NULL;
}

static void ensure_bank_fn(void)
{
    if (s_bank_fn_ready)
        return;
    s_bank_fn_ready = 1;

    if (drama_init(-1) < 0) {
        fprintf(stderr, TAG "WARNING: drama_init failed, bank info unavailable\n");
        return;
    }

    size_t ps = pagemap_page_size();
    size_t np = BANK_PROBE_SIZE / ps;

    void *buf = mmap(NULL, BANK_PROBE_SIZE, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
    if (buf == MAP_FAILED) {
        fprintf(stderr, TAG "WARNING: bank probe mmap failed\n");
        return;
    }

    volatile char *p = buf;
    for (size_t i = 0; i < np; i++)
        p[i * ps] = (char)i;

    uint64_t *pfns = calloc(np, sizeof(uint64_t));
    if (!pfns) {
        munmap(buf, BANK_PROBE_SIZE);
        return;
    }

    for (size_t i = 0; i < np; i++) {
        pagemap_entry_t pme;
        uint64_t pa = pagemap_lookup((char *)buf + i * ps, &pme);
        if (pa != PAGEMAP_INVALID_PADDR)
            pfns[i] = pa >> 12;
    }

    drama_bank_fn_t fn;
    if (drama_probe_bank_fn(buf, BANK_PROBE_SIZE, pfns, (int)np, 2000, &fn) == 0) {
        xrmemmap_set_bank_fn(&fn);
        fprintf(stderr, TAG "bank function: %d bits, %d banks, masks:",
                fn.nbits, fn.nbanks);
        for (int i = 0; i < fn.nbits; i++)
            fprintf(stderr, " 0x%lx", (unsigned long)fn.masks[i]);
        fprintf(stderr, "\n");
    } else {
        fprintf(stderr, TAG "WARNING: bank function derivation failed\n");
    }

    free(pfns);
    munmap(buf, BANK_PROBE_SIZE);
}

static void ensure_tracing(void)
{
    if (s_tracing)
        return;
    pid_t pid = getpid();
    if (kgsl_trace_start(pid) == 0) {
        s_tracing = 1;
        fprintf(stderr, TAG "ftrace started (pid=%d)\n", pid);
    } else {
        fprintf(stderr, TAG "WARNING: kgsl_trace_start failed (root?)\n");
    }
}

static void stop_tracing(void)
{
    if (!s_tracing)
        return;
    kgsl_trace_stop();
    s_tracing = 0;
    fprintf(stderr, TAG "ftrace stopped\n");
}

static void do_correlation(tracked_alloc_t *t)
{
    usleep(20000);

    kgsl_mapping_t mappings[128];
    int n = kgsl_trace_read(mappings, 128);
    if (n <= 0) {
        fprintf(stderr, TAG "  no KGSL events captured\n");
        return;
    }

    xrmemmap_alloc_t resolved;
    uint64_t cpu_va = (uint64_t)(uintptr_t)t->mapped_ptr;
    uint64_t size = t->mapped_size ? t->mapped_size : t->size;

    if (xrmemmap_resolve(cpu_va, size, mappings, n, &resolved) == 0) {
        xrmemmap_print(&resolved, stderr);
        xrmemmap_free(&resolved);
    } else {
        fprintf(stderr, TAG "  CPU VA 0x%" PRIx64 " not found in KGSL mappings\n", cpu_va);
    }
}

__attribute__((destructor))
static void shim_fini(void)
{
    stop_tracing();

    int active = 0;
    for (int i = 0; i < MAX_ALLOCS; i++)
        if (s_allocs[i].active)
            active++;
    if (active > 0)
        fprintf(stderr, TAG "WARNING: %d allocation(s) still active at exit\n", active);
}

/* --- Vulkan interceptors --- */

VKAPI_ATTR VkResult VKAPI_CALL vkAllocateMemory(
    VkDevice                     device,
    const VkMemoryAllocateInfo  *pAllocateInfo,
    const VkAllocationCallbacks *pAllocator,
    VkDeviceMemory              *pMemory)
{
    resolve_real_functions();
    ensure_bank_fn();
    ensure_tracing();

    VkResult r = s_real_alloc(device, pAllocateInfo, pAllocator, pMemory);
    if (r != VK_SUCCESS)
        return r;

    tracked_alloc_t *t = find_free_slot();
    if (t) {
        memset(t, 0, sizeof(*t));
        t->handle   = *pMemory;
        t->size     = pAllocateInfo->allocationSize;
        t->mem_type = pAllocateInfo->memoryTypeIndex;
        t->active   = 1;
    }

    fprintf(stderr, TAG "vkAllocateMemory: handle=0x%" PRIx64
            "  size=%" PRIu64 "  memType=%u\n",
            (uint64_t)(uintptr_t)*pMemory,
            (uint64_t)pAllocateInfo->allocationSize,
            pAllocateInfo->memoryTypeIndex);

    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkMapMemory(
    VkDevice         device,
    VkDeviceMemory   memory,
    VkDeviceSize     offset,
    VkDeviceSize     size,
    VkMemoryMapFlags flags,
    void           **ppData)
{
    resolve_real_functions();

    VkResult r = s_real_map(device, memory, offset, size, flags, ppData);
    if (r != VK_SUCCESS)
        return r;

    tracked_alloc_t *t = find_by_handle(memory);
    if (t) {
        t->mapped_ptr    = *ppData;
        t->mapped_offset = offset;
        t->mapped_size   = (size == VK_WHOLE_SIZE) ? (t->size - offset) : size;
        t->mapped        = 1;

        /* Touch pages to ensure they're resident before resolving */
        volatile unsigned char *p = (volatile unsigned char *)*ppData;
        size_t ps = pagemap_page_size();
        for (VkDeviceSize i = 0; i < t->mapped_size; i += ps)
            p[i] = (unsigned char)(i >> 12);

        fprintf(stderr, TAG "vkMapMemory: handle=0x%" PRIx64
                "  cpu_va=%p  offset=%" PRIu64 "  size=%" PRIu64 "\n",
                (uint64_t)(uintptr_t)memory, *ppData,
                (uint64_t)offset, (uint64_t)t->mapped_size);

        do_correlation(t);
    }

    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkUnmapMemory(
    VkDevice       device,
    VkDeviceMemory memory)
{
    resolve_real_functions();

    tracked_alloc_t *t = find_by_handle(memory);
    if (t) {
        fprintf(stderr, TAG "vkUnmapMemory: handle=0x%" PRIx64 "\n",
                (uint64_t)(uintptr_t)memory);
        t->mapped_ptr = NULL;
        t->mapped     = 0;
    }

    s_real_unmap(device, memory);
}

VKAPI_ATTR void VKAPI_CALL vkFreeMemory(
    VkDevice                     device,
    VkDeviceMemory               memory,
    const VkAllocationCallbacks *pAllocator)
{
    resolve_real_functions();

    tracked_alloc_t *t = find_by_handle(memory);
    if (t) {
        fprintf(stderr, TAG "vkFreeMemory: handle=0x%" PRIx64
                "  size=%" PRIu64 "\n",
                (uint64_t)(uintptr_t)memory, (uint64_t)t->size);
        t->active = 0;
    }

    s_real_free(device, memory, pAllocator);
}
