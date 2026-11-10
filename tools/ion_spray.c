/* ion_spray.c — hold Vulkan/ION allocations open to create slab pressure.
 * Allocates many small GPU memory blocks via Vulkan and sleeps,
 * keeping the ION slab entries alive until killed. */

#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>

#define NUM_ALLOCS 200
#define ALLOC_SIZE 4096  /* small allocs = more slab objects */

static volatile int running = 1;
static void on_signal(int s) { (void)s; running = 0; }

int main(void) {
    VkInstance inst;
    VkDevice dev;
    VkPhysicalDevice phys;
    VkDeviceMemory mem[NUM_ALLOCS];
    int allocated = 0;

    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);

    /* Create instance */
    VkApplicationInfo ai = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .apiVersion = VK_MAKE_VERSION(1,1,0)
    };
    VkInstanceCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &ai
    };
    if (vkCreateInstance(&ici, NULL, &inst) != VK_SUCCESS) {
        fprintf(stderr, "spray: vkCreateInstance failed\n");
        return 1;
    }

    /* Pick first physical device */
    uint32_t cnt = 1;
    vkEnumeratePhysicalDevices(inst, &cnt, &phys);

    /* Find a HOST_VISIBLE memory type */
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(phys, &mp);
    uint32_t mem_type = UINT32_MAX;
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
        if (mp.memoryTypes[i].propertyFlags &
            (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
            mem_type = i;
            break;
        }
    }
    if (mem_type == UINT32_MAX) {
        fprintf(stderr, "spray: no HOST_VISIBLE memory type\n");
        return 1;
    }

    /* Create logical device */
    VkDeviceQueueCreateInfo qci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueCount = 1,
        .pQueuePriorities = (float[]){1.0f}
    };
    VkDeviceCreateInfo dci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &qci
    };
    if (vkCreateDevice(phys, &dci, NULL, &dev) != VK_SUCCESS) {
        fprintf(stderr, "spray: vkCreateDevice failed\n");
        return 1;
    }

    /* Allocate many small GPU memory blocks */
    VkMemoryAllocateInfo mai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = ALLOC_SIZE,
        .memoryTypeIndex = mem_type
    };
    for (int i = 0; i < NUM_ALLOCS; i++) {
        if (vkAllocateMemory(dev, &mai, NULL, &mem[i]) == VK_SUCCESS) {
            allocated++;
        } else {
            break;
        }
    }

    fprintf(stderr, "spray: pid=%d holding %d ION allocs (%d bytes each), waiting...\n",
            getpid(), allocated, ALLOC_SIZE);

    /* Hold allocations open until killed */
    while (running) {
        sleep(1);
    }

    /* Cleanup on signal */
    for (int i = 0; i < allocated; i++)
        vkFreeMemory(dev, mem[i], NULL);
    vkDestroyDevice(dev, NULL);
    vkDestroyInstance(inst, NULL);

    fprintf(stderr, "spray: pid=%d freed %d allocs, exiting\n", getpid(), allocated);
    return 0;
}
