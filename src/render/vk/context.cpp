#include "render/vk/context.h"

#include "app/log.h"

#include <cstring>
#include <iterator>
#include <vector>

namespace render::vk {
namespace {

// The only extensions the project requires. Spec 8.1: Vulkan 1.2 core, nothing beyond a
// swapchain — so anything added here is a decision to stop working on some machines.
const char* const kDeviceExtensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};

VKAPI_ATTR VkBool32 VKAPI_CALL DebugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                             VkDebugUtilsMessageTypeFlagsEXT,
                                             const VkDebugUtilsMessengerCallbackDataEXT* data,
                                             void*) {
    if (data && data->pMessage) {
        const char* level = (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) ? "ERROR"
                            : (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
                                ? "WARN"
                                : "info";
        app::Log("vulkan %s: %s", level, data->pMessage);
    }
    return VK_FALSE;
}

bool HasLayer(const char* name) {
    uint32_t count = 0;
    vkEnumerateInstanceLayerProperties(&count, nullptr);
    std::vector<VkLayerProperties> layers(count);
    vkEnumerateInstanceLayerProperties(&count, layers.data());
    for (const auto& l : layers) {
        if (std::strcmp(l.layerName, name) == 0) return true;
    }
    return false;
}

bool HasInstanceExtension(const char* name) {
    uint32_t count = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> exts(count);
    vkEnumerateInstanceExtensionProperties(nullptr, &count, exts.data());
    for (const auto& e : exts) {
        if (std::strcmp(e.extensionName, name) == 0) return true;
    }
    return false;
}

bool DeviceSupportsExtensions(VkPhysicalDevice device) {
    uint32_t count = 0;
    vkEnumerateDeviceExtensionProperties(device, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> exts(count);
    vkEnumerateDeviceExtensionProperties(device, nullptr, &count, exts.data());

    for (const char* needed : kDeviceExtensions) {
        bool found = false;
        for (const auto& e : exts) {
            if (std::strcmp(e.extensionName, needed) == 0) {
                found = true;
                break;
            }
        }
        if (!found) return false;
    }
    return true;
}

// Prefers discrete, then integrated. The reference machine is an iGPU (spec 11.2), so an
// integrated device is the expected case and not a second-class one.
int ScoreDevice(const VkPhysicalDeviceProperties& props) {
    switch (props.deviceType) {
        case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:   return 3;
        case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return 2;
        case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:    return 1;
        default:                                     return 0;
    }
}

// A queue family that can both render and present to a Win32 surface. Keeping them the same
// family avoids concurrent sharing on every swapchain image for no benefit — there is no
// machine this project targets where a graphics queue cannot present.
bool FindGraphicsPresentFamily(VkPhysicalDevice device, uint32_t* outFamily) {
    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, families.data());

    for (uint32_t i = 0; i < count; ++i) {
        if (!(families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) continue;
        if (!vkGetPhysicalDeviceWin32PresentationSupportKHR(device, i)) continue;
        *outFamily = i;
        return true;
    }
    return false;
}

}  // namespace

std::unique_ptr<Context> Context::Create(bool enableValidation) {
    if (volkInitialize() != VK_SUCCESS) {
        app::Log("vulkan: no loader");
        return nullptr;
    }

    std::unique_ptr<Context> ctx(new Context());

    std::vector<const char*> instanceExtensions{VK_KHR_SURFACE_EXTENSION_NAME,
                                                VK_KHR_WIN32_SURFACE_EXTENSION_NAME};
    std::vector<const char*> layers;

    const bool wantDebug = enableValidation && HasInstanceExtension(VK_EXT_DEBUG_UTILS_EXTENSION_NAME) &&
                           HasLayer("VK_LAYER_KHRONOS_validation");
    if (wantDebug) {
        instanceExtensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        layers.push_back("VK_LAYER_KHRONOS_validation");
        app::Log("vulkan: validation enabled");
    }

    VkApplicationInfo app{};
    app.sType              = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName   = "nuke-saver";
    app.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
    app.pEngineName        = "nuke-saver";
    app.apiVersion         = VK_API_VERSION_1_2;

    VkInstanceCreateInfo ici{};
    ici.sType                   = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo        = &app;
    ici.enabledExtensionCount   = static_cast<uint32_t>(instanceExtensions.size());
    ici.ppEnabledExtensionNames = instanceExtensions.data();
    ici.enabledLayerCount       = static_cast<uint32_t>(layers.size());
    ici.ppEnabledLayerNames     = layers.empty() ? nullptr : layers.data();

    if (vkCreateInstance(&ici, nullptr, &ctx->instance_) != VK_SUCCESS) {
        app::Log("vulkan: vkCreateInstance failed");
        return nullptr;
    }
    volkLoadInstance(ctx->instance_);

    if (wantDebug && vkCreateDebugUtilsMessengerEXT) {
        VkDebugUtilsMessengerCreateInfoEXT dci{};
        dci.sType           = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
        dci.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                              VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        dci.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                          VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                          VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        dci.pfnUserCallback = DebugCallback;
        vkCreateDebugUtilsMessengerEXT(ctx->instance_, &dci, nullptr, &ctx->messenger_);
    }

    uint32_t deviceCount = 0;
    vkEnumeratePhysicalDevices(ctx->instance_, &deviceCount, nullptr);
    if (deviceCount == 0) {
        app::Log("vulkan: no physical devices");
        return nullptr;
    }
    std::vector<VkPhysicalDevice> devices(deviceCount);
    vkEnumeratePhysicalDevices(ctx->instance_, &deviceCount, devices.data());

    int best = -1;
    for (VkPhysicalDevice candidate : devices) {
        if (!DeviceSupportsExtensions(candidate)) continue;

        uint32_t family = 0;
        if (!FindGraphicsPresentFamily(candidate, &family)) continue;

        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(candidate, &props);

        const int score = ScoreDevice(props);
        if (score > best) {
            best                 = score;
            ctx->physical_       = candidate;
            ctx->graphicsFamily_ = family;
            ctx->properties_     = props;
        }
    }
    if (ctx->physical_ == VK_NULL_HANDLE) {
        app::Log("vulkan: no device with swapchain support and a presenting graphics queue");
        return nullptr;
    }
    app::Log("vulkan: using %s (queue family %u)", ctx->properties_.deviceName,
             ctx->graphicsFamily_);

    const float             priority = 1.0f;
    VkDeviceQueueCreateInfo qci{};
    qci.sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = ctx->graphicsFamily_;
    qci.queueCount       = 1;
    qci.pQueuePriorities = &priority;

    VkPhysicalDeviceFeatures features{};

    VkDeviceCreateInfo dci{};
    dci.sType                   = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount    = 1;
    dci.pQueueCreateInfos       = &qci;
    dci.enabledExtensionCount   = static_cast<uint32_t>(std::size(kDeviceExtensions));
    dci.ppEnabledExtensionNames = kDeviceExtensions;
    dci.pEnabledFeatures        = &features;

    if (vkCreateDevice(ctx->physical_, &dci, nullptr, &ctx->device_) != VK_SUCCESS) {
        app::Log("vulkan: vkCreateDevice failed");
        return nullptr;
    }
    volkLoadDevice(ctx->device_);
    vkGetDeviceQueue(ctx->device_, ctx->graphicsFamily_, 0, &ctx->graphicsQueue_);

    VmaVulkanFunctions     fns{};
    VmaAllocatorCreateInfo aci{};
    aci.physicalDevice   = ctx->physical_;
    aci.device           = ctx->device_;
    aci.instance         = ctx->instance_;
    aci.vulkanApiVersion = VK_API_VERSION_1_2;
    if (vmaImportVulkanFunctionsFromVolk(&aci, &fns) != VK_SUCCESS) {
        app::Log("vulkan: vmaImportVulkanFunctionsFromVolk failed");
        return nullptr;
    }
    aci.pVulkanFunctions = &fns;
    if (vmaCreateAllocator(&aci, &ctx->allocator_) != VK_SUCCESS) {
        app::Log("vulkan: vmaCreateAllocator failed");
        return nullptr;
    }

    return ctx;
}

Context::~Context() {
    if (allocator_) vmaDestroyAllocator(allocator_);
    if (device_) vkDestroyDevice(device_, nullptr);
    if (messenger_ && vkDestroyDebugUtilsMessengerEXT) {
        vkDestroyDebugUtilsMessengerEXT(instance_, messenger_, nullptr);
    }
    if (instance_) vkDestroyInstance(instance_, nullptr);
}

void Context::WaitIdle() const {
    if (device_ && !deviceLost_) vkDeviceWaitIdle(device_);
}

}  // namespace render::vk
