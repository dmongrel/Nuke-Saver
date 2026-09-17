#include "render/vk_probe.h"

#include <volk.h>

#include <vk_mem_alloc.h>

#include "render/shaders_embedded.h"

#include <cstdio>
#include <vector>

namespace probe {
namespace {

std::string VersionString(uint32_t v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%u.%u.%u", VK_VERSION_MAJOR(v), VK_VERSION_MINOR(v),
                  VK_VERSION_PATCH(v));
    return buf;
}

// Prefers a discrete GPU, then an integrated one, then whatever is left. The reference
// machine is an iGPU (spec 11.2), so "no discrete device" is the normal case, not a failure.
int ScoreDevice(const VkPhysicalDeviceProperties& props) {
    switch (props.deviceType) {
        case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:   return 3;
        case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return 2;
        case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:    return 1;
        default:                                     return 0;
    }
}

}  // namespace

Result Run() {
    Result r;

    // Shader blobs are a build-time check, not a runtime one, so answer that first: it holds
    // even on a machine where every Vulkan call below fails.
    r.shaderCount = g_shader_count;
    for (unsigned i = 0; i < g_shader_count; ++i) r.shaderBytes += g_shaders[i].size;

    r.stage = "volkInitialize";
    if (volkInitialize() != VK_SUCCESS) {
        r.detail = "no Vulkan loader (vulkan-1.dll not present or not loadable)";
        return r;
    }

    r.stage = "vkCreateInstance";
    VkApplicationInfo app{};
    app.sType              = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName   = "nuke-saver";
    app.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
    app.pEngineName        = "nuke-saver";
    app.engineVersion      = VK_MAKE_VERSION(0, 1, 0);
    app.apiVersion         = VK_API_VERSION_1_2;

    VkInstanceCreateInfo ici{};
    ici.sType            = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app;

    VkInstance instance = VK_NULL_HANDLE;
    if (vkCreateInstance(&ici, nullptr, &instance) != VK_SUCCESS) {
        r.detail = "vkCreateInstance failed";
        return r;
    }
    volkLoadInstance(instance);

    r.stage = "vkEnumeratePhysicalDevices";
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance, &count, nullptr);
    if (count == 0) {
        r.detail = "no Vulkan physical devices";
        vkDestroyInstance(instance, nullptr);
        return r;
    }
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(instance, &count, devices.data());
    r.deviceCount = count;

    VkPhysicalDevice           chosen = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties chosenProps{};
    int                        best = -1;
    for (VkPhysicalDevice d : devices) {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(d, &props);
        int score = ScoreDevice(props);
        if (score > best) {
            best        = score;
            chosen      = d;
            chosenProps = props;
        }
    }
    r.detail     = chosenProps.deviceName;
    r.apiVersion = VersionString(chosenProps.apiVersion);

    r.stage = "queue family";
    uint32_t qCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(chosen, &qCount, nullptr);
    std::vector<VkQueueFamilyProperties> qProps(qCount);
    vkGetPhysicalDeviceQueueFamilyProperties(chosen, &qCount, qProps.data());

    uint32_t graphicsFamily = UINT32_MAX;
    for (uint32_t i = 0; i < qCount; ++i) {
        if (qProps[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            graphicsFamily = i;
            break;
        }
    }
    if (graphicsFamily == UINT32_MAX) {
        r.detail = "no graphics queue family";
        vkDestroyInstance(instance, nullptr);
        return r;
    }

    r.stage = "vkCreateDevice";
    float                   priority = 1.0f;
    VkDeviceQueueCreateInfo qci{};
    qci.sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = graphicsFamily;
    qci.queueCount       = 1;
    qci.pQueuePriorities = &priority;

    VkDeviceCreateInfo dci{};
    dci.sType                = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos    = &qci;

    VkDevice device = VK_NULL_HANDLE;
    if (vkCreateDevice(chosen, &dci, nullptr, &device) != VK_SUCCESS) {
        r.detail = "vkCreateDevice failed";
        vkDestroyInstance(instance, nullptr);
        return r;
    }
    volkLoadDevice(device);

    r.stage = "vmaCreateAllocator";
    VmaVulkanFunctions fns{};
    VmaAllocatorCreateInfo aci{};
    aci.physicalDevice   = chosen;
    aci.device           = device;
    aci.instance         = instance;
    aci.vulkanApiVersion = VK_API_VERSION_1_2;

    if (vmaImportVulkanFunctionsFromVolk(&aci, &fns) != VK_SUCCESS) {
        r.detail = "vmaImportVulkanFunctionsFromVolk failed";
        vkDestroyDevice(device, nullptr);
        vkDestroyInstance(instance, nullptr);
        return r;
    }
    aci.pVulkanFunctions = &fns;

    VmaAllocator allocator = VK_NULL_HANDLE;
    if (vmaCreateAllocator(&aci, &allocator) != VK_SUCCESS) {
        r.detail = "vmaCreateAllocator failed";
        vkDestroyDevice(device, nullptr);
        vkDestroyInstance(instance, nullptr);
        return r;
    }

    // Round trip one small buffer, so the allocator is exercised rather than merely created.
    r.stage = "vmaCreateBuffer";
    VkBufferCreateInfo bci{};
    bci.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size        = 64 * 1024;
    bci.usage       = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo alloc{};
    alloc.usage = VMA_MEMORY_USAGE_AUTO;

    VkBuffer      buffer = VK_NULL_HANDLE;
    VmaAllocation vmaAlloc = VK_NULL_HANDLE;
    if (vmaCreateBuffer(allocator, &bci, &alloc, &buffer, &vmaAlloc, nullptr) != VK_SUCCESS) {
        r.detail = "vmaCreateBuffer failed";
        vmaDestroyAllocator(allocator);
        vkDestroyDevice(device, nullptr);
        vkDestroyInstance(instance, nullptr);
        return r;
    }

    // Teardown, in order. The whole point of the probe is that this path is clean.
    vmaDestroyBuffer(allocator, buffer, vmaAlloc);
    vmaDestroyAllocator(allocator);
    vkDestroyDevice(device, nullptr);
    vkDestroyInstance(instance, nullptr);

    r.stage = "complete";
    r.ok    = true;
    return r;
}

}  // namespace probe
