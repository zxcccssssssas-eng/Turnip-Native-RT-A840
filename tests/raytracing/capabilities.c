/* Query an Android Vulkan HAL in a separate process without replacing system drivers. */
#define VK_NO_PROTOTYPES
#include <hardware/hwvulkan.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(call) do { VkResult r = (call); if (r != VK_SUCCESS) { \
    fprintf(stderr, "%s returned %d\n", #call, r); return 2; } } while (0)

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc != 2) { fprintf(stderr, "Usage: %s HAL.so\n", argv[0]); return 2; }
    void *lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!lib) { fprintf(stderr, "%s\n", dlerror()); return 2; }
    hwvulkan_module_t *module = dlsym(lib, "HMI");
    hw_device_t *hal = NULL;
    if (!module || module->common.methods->open(&module->common, "vk0", &hal)) return 2;
    PFN_vkGetInstanceProcAddr gipa = ((hwvulkan_device_t *)hal)->GetInstanceProcAddr;
    PFN_vkCreateInstance create = (void *)gipa(NULL, "vkCreateInstance");
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "Banners Turnip RT probe", .apiVersion = VK_API_VERSION_1_2};
    VkInstanceCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
    VkInstance instance;
    CHECK(create(&ci, NULL, &instance));
#define IP(name) PFN_vk##name name = (void *)gipa(instance, "vk" #name)
    IP(EnumeratePhysicalDevices); IP(GetPhysicalDeviceProperties);
    IP(EnumerateDeviceExtensionProperties); IP(GetPhysicalDeviceFeatures2); IP(DestroyInstance);
    uint32_t count = 0;
    CHECK(EnumeratePhysicalDevices(instance, &count, NULL));
    if (!count) { fprintf(stderr, "No physical devices\n"); return 2; }
    VkPhysicalDevice *devices = calloc(count, sizeof(*devices));
    if (!devices) return 2;
    CHECK(EnumeratePhysicalDevices(instance, &count, devices));
    for (uint32_t i = 0; i < count; i++) {
        VkPhysicalDeviceProperties props;
        GetPhysicalDeviceProperties(devices[i], &props);
        printf("device=%s api=%u.%u.%u driver=0x%x\n", props.deviceName,
            VK_VERSION_MAJOR(props.apiVersion), VK_VERSION_MINOR(props.apiVersion),
            VK_VERSION_PATCH(props.apiVersion), props.driverVersion);
        uint32_t n = 0;
        CHECK(EnumerateDeviceExtensionProperties(devices[i], NULL, &n, NULL));
        VkExtensionProperties *exts = calloc(n, sizeof(*exts));
        if (!exts) return 2;
        CHECK(EnumerateDeviceExtensionProperties(devices[i], NULL, &n, exts));
        VkPhysicalDeviceFeatures2 features = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        VkPhysicalDeviceAccelerationStructureFeaturesKHR accel = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
        VkPhysicalDeviceRayQueryFeaturesKHR query = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR};
        VkPhysicalDeviceRayTracingPipelineFeaturesKHR pipeline = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR};
        for (uint32_t j = 0; j < n; j++) {
            const char *name = exts[j].extensionName;
            if (strstr(name, "ray") || strstr(name, "acceleration") || strstr(name, "deferred_host"))
                printf("extension=%s\n", name);
#define CHAIN(extension, node) if (!strcmp(name, extension)) { node.pNext = features.pNext; features.pNext = &node; }
            CHAIN(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME, accel);
            CHAIN(VK_KHR_RAY_QUERY_EXTENSION_NAME, query);
            CHAIN(VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME, pipeline);
        }
        GetPhysicalDeviceFeatures2(devices[i], &features);
        printf("accelerationStructure=%u rayQuery=%u rayTracingPipeline=%u\n",
            accel.accelerationStructure, query.rayQuery, pipeline.rayTracingPipeline);
        free(exts);
    }
    free(devices);
    DestroyInstance(instance, NULL);
    if (hal->close) hal->close(hal);
    /* Keep the HAL mapped for process lifetime: vendor profiling threads may
     * outlive DestroyInstance and retain code/data from this DSO. */
    return 0;
}
