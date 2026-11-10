/*
 * test_vulkan.c — minimal compute-only Vulkan allocation probe for xrmemmap.
 *
 * No graphics, no swapchain, no window. It:
 *   1. creates a VkInstance and picks the first physical device (Adreno 540);
 *   2. enumerates and prints all device extensions, flagging the four the
 *      later components care about (priority #5 of the Week 1 plan);
 *   3. allocates HOST_VISIBLE | HOST_COHERENT device memory, maps it with
 *      vkMapMemory, touches every page, and resolves each mapped page to a
 *      physical address with Component 1's virt_to_phys() (priority #4).
 *
 * Output is the Nov 4 demo artifact: Vulkan handle -> CPU VA -> physical pages.
 *
 * Build links libpagemap (Component 1) and libvulkan (NDK). Run under root on
 * the Quest for real PFNs:
 *   ./adb.exe shell su -c /data/local/tmp/test_vulkan
 *
 * An optional size in KiB may be passed as argv[1] (default 256 KiB).
 */

#include "pagemap.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <inttypes.h>

#include <vulkan/vulkan.h>

#define CHECK(expr) do {                                              \
    VkResult _r = (expr);                                             \
    if (_r != VK_SUCCESS) {                                           \
        fprintf(stderr, "Vulkan call failed (%d) at %s:%d: %s\n",     \
                (int)_r, __FILE__, __LINE__, #expr);                  \
        return EXIT_FAILURE;                                          \
    }                                                                 \
} while (0)

/* Extensions the later components may depend on; we report presence explicitly. */
static const char *WATCH_EXTS[] = {
    "VK_KHR_buffer_device_address",
    "VK_EXT_external_memory_host",
    "VK_KHR_shader_clock",
    "VK_EXT_host_query_reset",
};

static void enumerate_extensions(VkPhysicalDevice phys)
{
    uint32_t count = 0;
    if (vkEnumerateDeviceExtensionProperties(phys, NULL, &count, NULL) != VK_SUCCESS) {
        fprintf(stderr, "  (failed to query extension count)\n");
        return;
    }
    VkExtensionProperties *props = calloc(count, sizeof(*props));
    if (!props) { fprintf(stderr, "  (oom)\n"); return; }
    vkEnumerateDeviceExtensionProperties(phys, NULL, &count, props);

    printf("device extensions (%u):\n", count);
    for (uint32_t i = 0; i < count; i++)
        printf("  %s (rev %u)\n", props[i].extensionName, props[i].specVersion);

    printf("\nwatched extensions:\n");
    for (size_t w = 0; w < sizeof(WATCH_EXTS)/sizeof(WATCH_EXTS[0]); w++) {
        int found = 0;
        for (uint32_t i = 0; i < count && !found; i++)
            found = (strcmp(props[i].extensionName, WATCH_EXTS[w]) == 0);
        printf("  [%s] %s\n", found ? "x" : " ", WATCH_EXTS[w]);
    }
    free(props);
}

/* Confirm the Vulkan driver is talking to the Qualcomm GPU via /dev/kgsl-3d0
 * (Week 1 priority #6). Scans this process's own /proc entries — no root
 * needed for self. Reports every open fd and every mapping backed by a kgsl
 * node. If none appear, Component 2's ftrace design assumption is wrong and we
 * need to know now. */
static void report_kgsl_usage(void)
{
    int hits = 0;
    char path[256];

    /* Open file descriptors pointing at a kgsl node. */
    FILE *fd_ls = popen("ls -l /proc/self/fd 2>/dev/null", "r");
    if (fd_ls) {
        char line[512];
        while (fgets(line, sizeof(line), fd_ls)) {
            if (strstr(line, "kgsl")) {
                /* trim trailing newline for tidy output */
                size_t n = strlen(line);
                while (n && (line[n-1] == '\n' || line[n-1] == '\r')) line[--n] = 0;
                printf("  fd:  %s\n", line);
                hits++;
            }
        }
        pclose(fd_ls);
    }

    /* Memory mappings backed by a kgsl node (the driver mmaps GPU memory). */
    FILE *maps = fopen("/proc/self/maps", "r");
    if (maps) {
        char line[512];
        int shown = 0;
        while (fgets(line, sizeof(line), maps)) {
            if (strstr(line, "kgsl") && shown < 4) {
                size_t n = strlen(line);
                while (n && (line[n-1] == '\n' || line[n-1] == '\r')) line[--n] = 0;
                printf("  map: %s\n", line);
                shown++; hits++;
            }
        }
        fclose(maps);
    }

    (void)path;
    if (hits == 0)
        printf("  WARNING: no /dev/kgsl-3d0 fd or mapping found — Vulkan may not\n"
               "           be going through KGSL; Component 2 design must change.\n");
    else
        printf("  => confirmed: Vulkan is using /dev/kgsl-3d0 (%d reference(s))\n", hits);
}

/* Find a memory type index that is HOST_VISIBLE and HOST_COHERENT. Returns
 * index, or UINT32_MAX if none (should not happen on a conformant device). */
static uint32_t find_host_visible_type(VkPhysicalDevice phys)
{
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(phys, &mp);
    const VkMemoryPropertyFlags want =
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((mp.memoryTypes[i].propertyFlags & want) == want)
            return i;
    return UINT32_MAX;
}

int main(int argc, char **argv)
{
    size_t kib = (argc > 1) ? (size_t)strtoul(argv[1], NULL, 10) : 256;
    if (kib == 0) kib = 256;
    VkDeviceSize alloc_size = (VkDeviceSize)kib * 1024;

    printf("xrmemmap Vulkan allocation probe\n");
    printf("requested allocation: %zu KiB\n\n", kib);

    /* --- instance --- */
    VkApplicationInfo app = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "xrmemmap-probe",
        .apiVersion = VK_API_VERSION_1_1,   /* Adreno 540 on Quest 1 targets 1.1 */
    };
    VkInstanceCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &app,
    };
    VkInstance inst;
    CHECK(vkCreateInstance(&ici, NULL, &inst));

    /* --- pick first physical device --- */
    uint32_t ndev = 0;
    CHECK(vkEnumeratePhysicalDevices(inst, &ndev, NULL));
    if (ndev == 0) { fprintf(stderr, "no Vulkan physical devices\n"); return EXIT_FAILURE; }
    VkPhysicalDevice *devs = calloc(ndev, sizeof(*devs));
    CHECK(vkEnumeratePhysicalDevices(inst, &ndev, devs));
    VkPhysicalDevice phys = devs[0];

    VkPhysicalDeviceProperties pprops;
    vkGetPhysicalDeviceProperties(phys, &pprops);
    printf("device: %s (Vulkan %u.%u.%u, driver 0x%08x)\n\n",
           pprops.deviceName,
           VK_VERSION_MAJOR(pprops.apiVersion),
           VK_VERSION_MINOR(pprops.apiVersion),
           VK_VERSION_PATCH(pprops.apiVersion),
           pprops.driverVersion);

    enumerate_extensions(phys);
    printf("\n");

    /* --- logical device with a single queue (required to allocate memory) --- */
    uint32_t nqf = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &nqf, NULL);
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = 0,          /* family 0 is universal on Adreno */
        .queueCount = 1,
        .pQueuePriorities = &prio,
    };
    VkDeviceCreateInfo dci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &qci,
    };
    VkDevice dev;
    CHECK(vkCreateDevice(phys, &dci, NULL, &dev));

    printf("KGSL backing check (priority #6):\n");
    report_kgsl_usage();
    printf("\n");

    uint32_t mem_type = find_host_visible_type(phys);
    if (mem_type == UINT32_MAX) {
        fprintf(stderr, "no HOST_VISIBLE|HOST_COHERENT memory type found\n");
        return EXIT_FAILURE;
    }

    VkMemoryAllocateInfo mai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = alloc_size,
        .memoryTypeIndex = mem_type,
    };
    VkDeviceMemory mem;
    CHECK(vkAllocateMemory(dev, &mai, NULL, &mem));
    printf("vkAllocateMemory: handle=0x%" PRIx64 "  size=%" PRIu64 "  memType=%u\n",
           (uint64_t)mem, (uint64_t)alloc_size, mem_type);

    void *mapped = NULL;
    CHECK(vkMapMemory(dev, mem, 0, alloc_size, 0, &mapped));
    printf("vkMapMemory:     CPU VA=%p\n\n", mapped);

    /* Touch every page so it is resident before resolving. */
    const size_t ps = pagemap_page_size();
    volatile unsigned char *p = mapped;
    for (VkDeviceSize i = 0; i < alloc_size; i += ps)
        p[i] = (unsigned char)(i >> 12);

    /* Resolve each page: Vulkan handle -> CPU VA -> physical page. */
    size_t npages = (size_t)((alloc_size + ps - 1) / ps);
    pagemap_entry_t *entries = calloc(npages, sizeof(*entries));
    size_t got = npages;
    int unresolved = 0;

    printf("page-by-page resolution (page size %zu):\n", ps);
    if (pagemap_lookup_range(mapped, (size_t)alloc_size, entries, &got) == 0) {
        for (size_t i = 0; i < got; i++) {
            void *va = (char *)mapped + i * ps;
            if (entries[i].present && entries[i].pfn) {
                uint64_t pa = entries[i].pfn * ps;
                printf("  page %3zu  VA=%p  PFN=0x%-10" PRIx64 " PA=0x%" PRIx64 "\n",
                       i, va, entries[i].pfn, pa);
            } else {
                unresolved++;
                printf("  page %3zu  VA=%p  unresolved (present=%d)\n",
                       i, va, entries[i].present);
            }
        }
    } else {
        fprintf(stderr, "  pagemap_lookup_range failed: %s (need=%zu)\n",
                strerror(errno), got);
    }

    if (unresolved > 0)
        printf("\n%d/%zu pages unresolved — run under root (su) for physical PFNs\n",
               unresolved, got);

    /* --- teardown --- */
    free(entries);
    vkUnmapMemory(dev, mem);
    vkFreeMemory(dev, mem, NULL);
    vkDestroyDevice(dev, NULL);
    free(devs);
    vkDestroyInstance(inst, NULL);
    return EXIT_SUCCESS;
}
