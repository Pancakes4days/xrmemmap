/*
 * test_kgsl_ftrace.c — self-test for Component 2 (KGSL ftrace GPU VA capture)
 *
 * Does a Vulkan allocation while ftrace is active, then reads back the
 * trace and verifies that the GPU virtual address was captured and that
 * the CPU VA from kgsl_mem_mmap matches the one from vkMapMemory.
 *
 * Must run as root (su -c).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <dlfcn.h>

#include "pagemap.h"
#include "kgsl_ftrace.h"

/* Vulkan types — minimal subset to avoid pulling in the full header */
#define VK_MAKE_API_VERSION(v,ma,mi,p) ((((uint32_t)(v))<<29)|(((uint32_t)(ma))<<22)|(((uint32_t)(mi))<<12)|((uint32_t)(p)))

typedef enum { VK_SUCCESS = 0 } VkResult;
typedef enum { VK_STRUCTURE_TYPE_APPLICATION_INFO = 0,
               VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO = 1,
               VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO = 2,
               VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO = 3,
               VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO = 5 } VkStructureType;
typedef enum { VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT = 0x02,
               VK_MEMORY_PROPERTY_HOST_COHERENT_BIT = 0x04 } VkMemoryPropertyFlagBits;

typedef struct VkInstance_T *VkInstance;
typedef struct VkPhysicalDevice_T *VkPhysicalDevice;
typedef struct VkDevice_T *VkDevice;
typedef struct VkDeviceMemory_T *VkDeviceMemory;
typedef uint64_t VkDeviceSize;

typedef struct { VkStructureType sType; const void *pNext;
    const char *pApplicationName; uint32_t applicationVersion;
    const char *pEngineName; uint32_t engineVersion; uint32_t apiVersion;
} VkApplicationInfo;

typedef struct { VkStructureType sType; const void *pNext; uint32_t flags;
    const VkApplicationInfo *pApplicationInfo;
    uint32_t enabledLayerCount; const char *const *ppEnabledLayerNames;
    uint32_t enabledExtensionCount; const char *const *ppEnabledExtensionNames;
} VkInstanceCreateInfo;

typedef struct { VkStructureType sType; const void *pNext; uint32_t flags;
    uint32_t queueFamilyIndex; uint32_t queueCount; const float *pQueuePriorities;
} VkDeviceQueueCreateInfo;

typedef struct { VkStructureType sType; const void *pNext; uint32_t flags;
    uint32_t queueCreateInfoCount; const VkDeviceQueueCreateInfo *pQueueCreateInfos;
    uint32_t enabledLayerCount; const char *const *ppEnabledLayerNames;
    uint32_t enabledExtensionCount; const char *const *ppEnabledExtensionNames;
    const void *pEnabledFeatures;
} VkDeviceCreateInfo;

typedef struct { VkStructureType sType; const void *pNext;
    VkDeviceSize allocationSize; uint32_t memoryTypeIndex;
} VkMemoryAllocateInfo;

typedef struct { uint32_t memoryTypeCount;
    struct { uint32_t propertyFlags; uint32_t heapIndex; } memoryTypes[32];
    uint32_t memoryHeapCount;
    struct { VkDeviceSize size; uint32_t flags; } memoryHeaps[16];
} VkPhysicalDeviceMemoryProperties;

/* Function pointers loaded at runtime */
typedef VkResult (*PFN_vkCreateInstance)(const VkInstanceCreateInfo*, const void*, VkInstance*);
typedef VkResult (*PFN_vkEnumeratePhysicalDevices)(VkInstance, uint32_t*, VkPhysicalDevice*);
typedef void (*PFN_vkGetPhysicalDeviceMemoryProperties)(VkPhysicalDevice, VkPhysicalDeviceMemoryProperties*);
typedef VkResult (*PFN_vkCreateDevice)(VkPhysicalDevice, const VkDeviceCreateInfo*, const void*, VkDevice*);
typedef VkResult (*PFN_vkAllocateMemory)(VkDevice, const VkMemoryAllocateInfo*, const void*, VkDeviceMemory*);
typedef VkResult (*PFN_vkMapMemory)(VkDevice, VkDeviceMemory, VkDeviceSize, VkDeviceSize, uint32_t, void**);
typedef void (*PFN_vkUnmapMemory)(VkDevice, VkDeviceMemory);
typedef void (*PFN_vkFreeMemory)(VkDevice, VkDeviceMemory, const void*);
typedef void (*PFN_vkDestroyDevice)(VkDevice, const void*);
typedef void (*PFN_vkDestroyInstance)(VkInstance, const void*);

#define ALLOC_SIZE (256 * 1024)  /* 256 KiB, same as test_vulkan */

static int failures = 0;

#define CHECK(cond, ...) do { \
    if (!(cond)) { \
        printf("  FAIL: "); \
        printf(__VA_ARGS__); \
        printf("\n"); \
        failures++; \
    } \
} while(0)

int main(void)
{
    printf("xrmemmap Component 2 — KGSL ftrace GPU VA capture self-test\n");
    printf("uid: %d (%s)\n\n", getuid(), getuid() == 0 ? "root" : "NOT ROOT — PFN and ftrace will fail");

    pid_t my_pid = getpid();
    printf("test PID: %d\n\n", my_pid);

    /* Load Vulkan */
    void *vk = dlopen("libvulkan.so", RTLD_NOW);
    if (!vk) {
        printf("FATAL: cannot load libvulkan.so: %s\n", dlerror());
        return 1;
    }

    #define LOAD_VK(type, var, sym) \
        type var = (type)dlsym(vk, sym); \
        if (!var) { printf("FATAL: missing %s\n", sym); return 1; }

    LOAD_VK(PFN_vkCreateInstance,           pvkCreateInstance,           "vkCreateInstance")
    LOAD_VK(PFN_vkEnumeratePhysicalDevices, pvkEnumeratePhysicalDevices, "vkEnumeratePhysicalDevices")
    LOAD_VK(PFN_vkGetPhysicalDeviceMemoryProperties, pvkGetPhysicalDeviceMemoryProperties, "vkGetPhysicalDeviceMemoryProperties")
    LOAD_VK(PFN_vkCreateDevice,             pvkCreateDevice,             "vkCreateDevice")
    LOAD_VK(PFN_vkAllocateMemory,           pvkAllocateMemory,           "vkAllocateMemory")
    LOAD_VK(PFN_vkMapMemory,                pvkMapMemory,                "vkMapMemory")
    LOAD_VK(PFN_vkUnmapMemory,              pvkUnmapMemory,              "vkUnmapMemory")
    LOAD_VK(PFN_vkFreeMemory,               pvkFreeMemory,               "vkFreeMemory")
    LOAD_VK(PFN_vkDestroyDevice,            pvkDestroyDevice,            "vkDestroyDevice")
    LOAD_VK(PFN_vkDestroyInstance,          pvkDestroyInstance,          "vkDestroyInstance")
    #undef LOAD_VK

    /* Create Vulkan instance */
    VkApplicationInfo app_info = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "xrmemmap_kgsl_test",
        .applicationVersion = 1,
        .apiVersion = VK_MAKE_API_VERSION(0, 1, 1, 0)
    };
    VkInstanceCreateInfo inst_ci = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &app_info
    };
    VkInstance instance = NULL;
    VkResult r = pvkCreateInstance(&inst_ci, NULL, &instance);
    if (r != VK_SUCCESS) {
        printf("FATAL: vkCreateInstance failed (%d)\n", r);
        return 1;
    }

    /* Pick first physical device */
    uint32_t gpu_count = 1;
    VkPhysicalDevice phys_dev = NULL;
    pvkEnumeratePhysicalDevices(instance, &gpu_count, &phys_dev);

    /* Find HOST_VISIBLE | HOST_COHERENT memory type */
    VkPhysicalDeviceMemoryProperties mem_props;
    pvkGetPhysicalDeviceMemoryProperties(phys_dev, &mem_props);
    uint32_t mem_type_idx = UINT32_MAX;
    for (uint32_t i = 0; i < mem_props.memoryTypeCount; i++) {
        uint32_t flags = mem_props.memoryTypes[i].propertyFlags;
        if ((flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) &&
            (flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
            mem_type_idx = i;
            break;
        }
    }
    if (mem_type_idx == UINT32_MAX) {
        printf("FATAL: no HOST_VISIBLE|HOST_COHERENT memory type\n");
        return 1;
    }

    /* Create logical device */
    float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_ci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueCount = 1,
        .pQueuePriorities = &priority
    };
    VkDeviceCreateInfo dev_ci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &queue_ci
    };
    VkDevice device = NULL;
    r = pvkCreateDevice(phys_dev, &dev_ci, NULL, &device);
    if (r != VK_SUCCESS) {
        printf("FATAL: vkCreateDevice failed (%d)\n", r);
        return 1;
    }

    printf("[1] Starting KGSL ftrace capture...\n");
    if (kgsl_trace_start(my_pid) < 0) {
        printf("FATAL: kgsl_trace_start failed (are you root?): %s\n", strerror(errno));
        return 1;
    }

    /* Allocate Vulkan memory */
    VkMemoryAllocateInfo alloc_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = ALLOC_SIZE,
        .memoryTypeIndex = mem_type_idx
    };
    VkDeviceMemory vk_mem = NULL;
    r = pvkAllocateMemory(device, &alloc_info, NULL, &vk_mem);
    if (r != VK_SUCCESS) {
        printf("FATAL: vkAllocateMemory failed (%d)\n", r);
        kgsl_trace_stop();
        return 1;
    }

    /* Map memory */
    void *mapped = NULL;
    r = pvkMapMemory(device, vk_mem, 0, ALLOC_SIZE, 0, &mapped);
    if (r != VK_SUCCESS) {
        printf("FATAL: vkMapMemory failed (%d)\n", r);
        kgsl_trace_stop();
        return 1;
    }

    uint64_t cpu_va = (uint64_t)(uintptr_t)mapped;
    printf("[2] Vulkan allocation: CPU VA=0x%llx, size=%d\n",
           (unsigned long long)cpu_va, ALLOC_SIZE);

    /* Touch every page so pagemap resolves */
    volatile char *p = (volatile char *)mapped;
    size_t page_size = pagemap_page_size();
    size_t npages = ALLOC_SIZE / page_size;
    for (size_t i = 0; i < npages; i++)
        p[i * page_size] = (char)i;

    /* Small delay so ftrace events are flushed */
    usleep(50000);

    printf("[3] Stopping ftrace and reading events...\n");
    kgsl_trace_stop();

    /* Read captured events */
    kgsl_mapping_t mappings[128];
    int nmappings = kgsl_trace_read(mappings, 128);
    CHECK(nmappings > 0, "no KGSL events captured (got %d)", nmappings);

    printf("    captured %d KGSL mapping(s)\n\n", nmappings);

    /* Print all captured mappings */
    printf("    %-4s  %-18s  %-18s  %-10s  %-18s  %s\n",
           "id", "GPU VA", "CPU VA", "size", "usage", "freed");
    for (int i = 0; i < nmappings; i++) {
        printf("    %-4u  0x%016llx  0x%016llx  %-10llu  %-18s  %s\n",
               mappings[i].kgsl_id,
               (unsigned long long)mappings[i].gpuaddr,
               (unsigned long long)mappings[i].useraddr,
               (unsigned long long)mappings[i].size,
               mappings[i].usage,
               mappings[i].freed ? "yes" : "no");
    }
    printf("\n");

    /* Find our allocation by CPU VA */
    printf("[4] Correlating: looking up CPU VA 0x%llx in KGSL mappings...\n",
           (unsigned long long)cpu_va);
    const kgsl_mapping_t *match = kgsl_trace_find_by_useraddr(mappings, nmappings, cpu_va);

    CHECK(match != NULL, "our Vulkan allocation not found in KGSL trace by CPU VA");

    if (match) {
        printf("    MATCH: kgsl_id=%u  GPU VA=0x%llx  CPU VA=0x%llx  size=%llu  usage=%s\n",
               match->kgsl_id,
               (unsigned long long)match->gpuaddr,
               (unsigned long long)match->useraddr,
               (unsigned long long)match->size,
               match->usage);

        CHECK(match->gpuaddr != 0, "GPU VA is zero");
        CHECK(match->size == ALLOC_SIZE, "size mismatch: expected %d, got %llu",
              ALLOC_SIZE, (unsigned long long)match->size);
        CHECK(strcmp(match->usage, "vk_devicememory") == 0,
              "unexpected usage: %s", match->usage);

        /* Now do the full chain: CPU VA → GPU VA → Physical pages */
        printf("\n[5] Full address chain (first 4 + last page):\n");
        printf("    %-6s  %-18s  %-18s  %-18s\n", "page", "CPU VA", "GPU VA", "Physical PA");

        size_t show_pages[] = {0, 1, 2, 3, npages - 1};
        for (size_t s = 0; s < sizeof(show_pages) / sizeof(show_pages[0]); s++) {
            size_t pg = show_pages[s];
            uint64_t va = cpu_va + pg * page_size;
            uint64_t gpu = match->gpuaddr + pg * page_size;

            pagemap_entry_t pme;
            uint64_t pa = pagemap_lookup((const void *)(uintptr_t)va, &pme);

            if (pa != PAGEMAP_INVALID_PADDR) {
                printf("    %-6zu  0x%016llx  0x%016llx  0x%016llx\n",
                       pg, (unsigned long long)va,
                       (unsigned long long)gpu,
                       (unsigned long long)pa);
            } else {
                printf("    %-6zu  0x%016llx  0x%016llx  (unresolved)\n",
                       pg, (unsigned long long)va, (unsigned long long)gpu);
            }
        }

        /* Verify GPU VA is in the expected range for Adreno 540 */
        CHECK(match->gpuaddr >= 0x500000000ULL && match->gpuaddr < 0x600000000ULL,
              "GPU VA 0x%llx outside expected Adreno range [0x500000000, 0x600000000)",
              (unsigned long long)match->gpuaddr);
    }

    /* Cleanup */
    pvkUnmapMemory(device, vk_mem);
    pvkFreeMemory(device, vk_mem, NULL);
    pvkDestroyDevice(device, NULL);
    pvkDestroyInstance(instance, NULL);
    dlclose(vk);

    printf("\n%s (%d failure%s)\n",
           failures == 0 ? "PASS" : "FAIL",
           failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
