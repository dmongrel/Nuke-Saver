// Vulkan instance, device and allocator (implementation plan, M2).
//
// One of these is shared by every window. Spec 9.2 is explicit that the meshes, pipelines and
// simulation buffers are created once and presented from many swapchains; only the swapchain
// and its per-image resources are per window.
#ifndef NUKE_SAVER_VK_CONTEXT_H
#define NUKE_SAVER_VK_CONTEXT_H

#include <volk.h>

#include <vk_mem_alloc.h>

#include <memory>

namespace render::vk {

class Context {
public:
    ~Context();

    Context(const Context&)            = delete;
    Context& operator=(const Context&) = delete;

    // Returns nullptr on any failure at all: a missing loader, no suitable device, a format the
    // machine does not support. The caller falls back to GDI (spec section 12), so failure here
    // is ordinary rather than exceptional and must never surface to the user.
    static std::unique_ptr<Context> Create(bool enableValidation);

    VkInstance       instance() const { return instance_; }
    VkPhysicalDevice physicalDevice() const { return physical_; }
    VkDevice         device() const { return device_; }
    VkQueue          graphicsQueue() const { return graphicsQueue_; }
    uint32_t         graphicsFamily() const { return graphicsFamily_; }
    VmaAllocator     allocator() const { return allocator_; }

    const VkPhysicalDeviceProperties& properties() const { return properties_; }

    // Waits for the device to go idle, tolerating a device that has already been lost.
    void WaitIdle() const;

    // True once any submission or present has reported VK_ERROR_DEVICE_LOST. The renderer uses
    // this to decide whether to attempt one re-initialisation before dropping to GDI.
    bool deviceLost() const { return deviceLost_; }
    void NoteDeviceLost() { deviceLost_ = true; }

private:
    Context() = default;

    VkInstance               instance_       = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger_      = VK_NULL_HANDLE;
    VkPhysicalDevice         physical_       = VK_NULL_HANDLE;
    VkDevice                 device_         = VK_NULL_HANDLE;
    VkQueue                  graphicsQueue_  = VK_NULL_HANDLE;
    uint32_t                 graphicsFamily_ = 0;
    VmaAllocator             allocator_      = VK_NULL_HANDLE;

    VkPhysicalDeviceProperties properties_{};
    bool                       deviceLost_ = false;
};

// Formats the project depends on, resolved once at startup.
constexpr VkFormat kHdrFormat       = VK_FORMAT_R16G16B16A16_SFLOAT;  // spec 8.1
constexpr VkFormat kSwapchainFormat = VK_FORMAT_B8G8R8A8_UNORM;       // spec 8.1

}  // namespace render::vk

#endif
