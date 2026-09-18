#include "render/vk/window_target.h"

#include "app/log.h"

#include <algorithm>

namespace render::vk {
namespace {

VkSurfaceFormatKHR ChooseSurfaceFormat(Context& ctx, VkSurfaceKHR surface) {
    uint32_t count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(ctx.physicalDevice(), surface, &count, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(count);
    vkGetPhysicalDeviceSurfaceFormatsKHR(ctx.physicalDevice(), surface, &count, formats.data());

    // Spec 8.1 asks for B8G8R8A8_UNORM specifically: the tonemap shader owns the sRGB transfer
    // function, so handing the hardware an _SRGB view would apply it twice.
    for (const auto& f : formats) {
        if (f.format == kSwapchainFormat && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            return f;
        }
    }
    for (const auto& f : formats) {
        if (f.format == VK_FORMAT_R8G8B8A8_UNORM) return f;
    }
    return formats.empty() ? VkSurfaceFormatKHR{kSwapchainFormat,
                                                VK_COLOR_SPACE_SRGB_NONLINEAR_KHR}
                           : formats[0];
}

// Spec 11.2: prefer FIFO, and never busy-wait. FIFO is always supported, so this is less a
// choice than a statement of intent — a screen saver has no reason to tear or to render faster
// than the display.
VkPresentModeKHR ChoosePresentMode() { return VK_PRESENT_MODE_FIFO_KHR; }

}  // namespace

bool CreateImage2D(Context& ctx, uint32_t w, uint32_t h, VkFormat format, VkImageUsageFlags usage,
                   VkImageAspectFlags aspect, VkImage* image, VmaAllocation* alloc,
                   VkImageView* view) {
    VkImageCreateInfo ici{};
    ici.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.imageType     = VK_IMAGE_TYPE_2D;
    ici.format        = format;
    ici.extent        = {w, h, 1};
    ici.mipLevels     = 1;
    ici.arrayLayers   = 1;
    ici.samples       = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling        = VK_IMAGE_TILING_OPTIMAL;
    ici.usage         = usage;
    ici.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo aci{};
    aci.usage    = VMA_MEMORY_USAGE_AUTO;
    aci.priority = 1.0f;

    if (vmaCreateImage(ctx.allocator(), &ici, &aci, image, alloc, nullptr) != VK_SUCCESS) {
        return false;
    }

    VkImageViewCreateInfo vci{};
    vci.sType                       = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vci.image                       = *image;
    vci.viewType                    = VK_IMAGE_VIEW_TYPE_2D;
    vci.format                      = format;
    vci.subresourceRange.aspectMask = aspect;
    vci.subresourceRange.levelCount = 1;
    vci.subresourceRange.layerCount = 1;

    return vkCreateImageView(ctx.device(), &vci, nullptr, view) == VK_SUCCESS;
}

VkFormat ChooseDepthFormat(Context& ctx) {
    const VkFormat candidates[] = {VK_FORMAT_D32_SFLOAT, VK_FORMAT_D32_SFLOAT_S8_UINT,
                                   VK_FORMAT_D24_UNORM_S8_UINT};
    for (VkFormat f : candidates) {
        VkFormatProperties props{};
        vkGetPhysicalDeviceFormatProperties(ctx.physicalDevice(), f, &props);
        // SAMPLED as well as ATTACHMENT: the same format backs the shadow map, which is read
        // by four fragment shaders (spec 8.2). Every desktop driver offers both on all three.
        const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT |
                                          VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
        if ((props.optimalTilingFeatures & need) == need) return f;
    }
    return VK_FORMAT_UNDEFINED;
}

bool CreateRenderPasses(Context& ctx, RenderPasses* out) {
    const VkFormat depthFormat = ChooseDepthFormat(ctx);
    if (depthFormat == VK_FORMAT_UNDEFINED) {
        app::Log("vulkan: no usable depth format");
        return false;
    }

    // --- scene pass: HDR colour plus depth -------------------------------------------------
    VkAttachmentDescription hdrAttachments[2]{};
    hdrAttachments[0].format         = kHdrFormat;
    hdrAttachments[0].samples        = VK_SAMPLE_COUNT_1_BIT;
    hdrAttachments[0].loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
    hdrAttachments[0].storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
    hdrAttachments[0].stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    hdrAttachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    hdrAttachments[0].initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
    hdrAttachments[0].finalLayout    = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    hdrAttachments[1].format         = depthFormat;
    hdrAttachments[1].samples        = VK_SAMPLE_COUNT_1_BIT;
    hdrAttachments[1].loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
    hdrAttachments[1].storeOp        = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    hdrAttachments[1].stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    hdrAttachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    hdrAttachments[1].initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
    hdrAttachments[1].finalLayout    = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkAttachmentReference hdrColorRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference hdrDepthRef{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};

    VkSubpassDescription hdrSubpass{};
    hdrSubpass.pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS;
    hdrSubpass.colorAttachmentCount    = 1;
    hdrSubpass.pColorAttachments       = &hdrColorRef;
    hdrSubpass.pDepthStencilAttachment = &hdrDepthRef;

    // The tonemap pass samples this attachment and the auto-exposure histogram of spec 8.2 reads
    // it from compute, so the write has to be visible to both before either runs.
    VkSubpassDependency hdrDeps[2]{};
    hdrDeps[0].srcSubpass    = VK_SUBPASS_EXTERNAL;
    hdrDeps[0].dstSubpass    = 0;
    hdrDeps[0].srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                              VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    hdrDeps[0].dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                              VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    hdrDeps[0].srcAccessMask = 0;
    hdrDeps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                               VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

    hdrDeps[1].srcSubpass    = 0;
    hdrDeps[1].dstSubpass    = VK_SUBPASS_EXTERNAL;
    hdrDeps[1].srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    hdrDeps[1].dstStageMask =
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    hdrDeps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    hdrDeps[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    VkRenderPassCreateInfo hdrInfo{};
    hdrInfo.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    hdrInfo.attachmentCount = 2;
    hdrInfo.pAttachments    = hdrAttachments;
    hdrInfo.subpassCount    = 1;
    hdrInfo.pSubpasses      = &hdrSubpass;
    hdrInfo.dependencyCount = 2;
    hdrInfo.pDependencies   = hdrDeps;

    if (vkCreateRenderPass(ctx.device(), &hdrInfo, nullptr, &out->hdr) != VK_SUCCESS) {
        app::Log("vulkan: hdr render pass failed");
        return false;
    }

    // --- shadow pass: the key light's depth map, no colour at all ---------------------------
    VkAttachmentDescription shadowAttachment{};
    shadowAttachment.format         = depthFormat;
    shadowAttachment.samples        = VK_SAMPLE_COUNT_1_BIT;
    shadowAttachment.loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
    shadowAttachment.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;  // the whole point of the pass
    shadowAttachment.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    shadowAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    shadowAttachment.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
    shadowAttachment.finalLayout    = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkAttachmentReference shadowDepthRef{0, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};

    VkSubpassDescription shadowSubpass{};
    shadowSubpass.pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS;
    shadowSubpass.colorAttachmentCount    = 0;
    shadowSubpass.pDepthStencilAttachment = &shadowDepthRef;

    // Both directions matter here, and for different reasons. Before: the surface passes of the
    // previous frame are still sampling this image, and the clear must not start until they have
    // finished with it. After: every surface pass of this frame reads it, and they must not start
    // until it is written.
    VkSubpassDependency shadowDeps[2]{};
    shadowDeps[0].srcSubpass    = VK_SUBPASS_EXTERNAL;
    shadowDeps[0].dstSubpass    = 0;
    shadowDeps[0].srcStageMask  = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    shadowDeps[0].dstStageMask  = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    shadowDeps[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    shadowDeps[0].dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

    shadowDeps[1].srcSubpass    = 0;
    shadowDeps[1].dstSubpass    = VK_SUBPASS_EXTERNAL;
    shadowDeps[1].srcStageMask  = VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    shadowDeps[1].dstStageMask  = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    shadowDeps[1].srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    shadowDeps[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    VkRenderPassCreateInfo shadowInfo{};
    shadowInfo.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    shadowInfo.attachmentCount = 1;
    shadowInfo.pAttachments    = &shadowAttachment;
    shadowInfo.subpassCount    = 1;
    shadowInfo.pSubpasses      = &shadowSubpass;
    shadowInfo.dependencyCount = 2;
    shadowInfo.pDependencies   = shadowDeps;

    if (vkCreateRenderPass(ctx.device(), &shadowInfo, nullptr, &out->shadow) != VK_SUCCESS) {
        app::Log("vulkan: shadow render pass failed");
        return false;
    }

    // --- present pass: tonemap into the swapchain ------------------------------------------
    VkAttachmentDescription presentAttachment{};
    presentAttachment.format         = kSwapchainFormat;
    presentAttachment.samples        = VK_SAMPLE_COUNT_1_BIT;
    presentAttachment.loadOp         = VK_ATTACHMENT_LOAD_OP_DONT_CARE;  // fully overwritten
    presentAttachment.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
    presentAttachment.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    presentAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    presentAttachment.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
    presentAttachment.finalLayout    = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentReference presentRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};

    VkSubpassDescription presentSubpass{};
    presentSubpass.pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS;
    presentSubpass.colorAttachmentCount = 1;
    presentSubpass.pColorAttachments    = &presentRef;

    VkSubpassDependency presentDep{};
    presentDep.srcSubpass    = VK_SUBPASS_EXTERNAL;
    presentDep.dstSubpass    = 0;
    presentDep.srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    presentDep.dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    presentDep.srcAccessMask = 0;
    presentDep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo presentInfo{};
    presentInfo.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    presentInfo.attachmentCount = 1;
    presentInfo.pAttachments    = &presentAttachment;
    presentInfo.subpassCount    = 1;
    presentInfo.pSubpasses      = &presentSubpass;
    presentInfo.dependencyCount = 1;
    presentInfo.pDependencies   = &presentDep;

    if (vkCreateRenderPass(ctx.device(), &presentInfo, nullptr, &out->present) != VK_SUCCESS) {
        app::Log("vulkan: present render pass failed");
        return false;
    }

    return true;
}

void DestroyRenderPasses(Context& ctx, RenderPasses* passes) {
    if (passes->hdr) vkDestroyRenderPass(ctx.device(), passes->hdr, nullptr);
    if (passes->present) vkDestroyRenderPass(ctx.device(), passes->present, nullptr);
    if (passes->shadow) vkDestroyRenderPass(ctx.device(), passes->shadow, nullptr);
    *passes = {};
}

std::unique_ptr<WindowTarget> WindowTarget::Create(Context& ctx, const RenderPasses& passes,
                                                   HWND hwnd, uint32_t width, uint32_t height) {
    std::unique_ptr<WindowTarget> t(new WindowTarget());
    t->ctx_  = &ctx;
    t->hwnd_ = hwnd;

    VkWin32SurfaceCreateInfoKHR sci{};
    sci.sType     = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
    sci.hinstance = GetModuleHandleW(nullptr);
    sci.hwnd      = hwnd;
    if (vkCreateWin32SurfaceKHR(ctx.instance(), &sci, nullptr, &t->surface_) != VK_SUCCESS) {
        app::Log("vulkan: win32 surface failed");
        return nullptr;
    }

    VkBool32 supported = VK_FALSE;
    vkGetPhysicalDeviceSurfaceSupportKHR(ctx.physicalDevice(), ctx.graphicsFamily(), t->surface_,
                                         &supported);
    if (!supported) {
        app::Log("vulkan: queue family cannot present to this surface");
        vkDestroySurfaceKHR(ctx.instance(), t->surface_, nullptr);
        t->surface_ = VK_NULL_HANDLE;
        return nullptr;
    }

    VkCommandPoolCreateInfo pci{};
    pci.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pci.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = ctx.graphicsFamily();
    if (vkCreateCommandPool(ctx.device(), &pci, nullptr, &t->commandPool_) != VK_SUCCESS) {
        app::Log("vulkan: command pool creation failed");
        return nullptr;
    }

    VkCommandBufferAllocateInfo cbi{};
    cbi.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbi.commandPool        = t->commandPool_;
    cbi.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbi.commandBufferCount = kFramesInFlight;
    if (vkAllocateCommandBuffers(ctx.device(), &cbi, t->commandBuffers_) != VK_SUCCESS) {
        app::Log("vulkan: command buffer allocation failed");
        return nullptr;
    }

    for (uint32_t i = 0; i < kFramesInFlight; ++i) {
        VkSemaphoreCreateInfo sem{};
        sem.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        if (vkCreateSemaphore(ctx.device(), &sem, nullptr, &t->imageAvailable_[i]) != VK_SUCCESS) {
            app::Log("vulkan: semaphore creation failed");
            return nullptr;
        }
        VkFenceCreateInfo fence{};
        fence.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;  // first wait must not block forever
        if (vkCreateFence(ctx.device(), &fence, nullptr, &t->inFlight_[i]) != VK_SUCCESS) {
            return nullptr;
        }
    }

    // A window with no area yet is fine; anything else is not, and Rebuild has already said so in
    // the log. The two are told apart by needsRebuild_, which only the no-area path sets.
    //
    // No-area is transient -- it is the gap between CreateWindowExW and the window actually having
    // a client rect -- so it is waited out here rather than reported. It cannot be deferred to the
    // first frame: the caller sizes its bloom descriptor sets from bloomLevels(), which is a
    // property of the extent, so a target handed back without one would be wired up for a surface
    // it does not have. And it must not be reported as a failure either: that turns a few
    // milliseconds of window creation into a screen saver that runs on the GDI fallback and draws
    // nothing for the rest of the session.
    for (int attempt = 0; attempt < 40; ++attempt) {
        if (t->Rebuild(ctx, passes, width, height)) return t;
        if (!t->needsRebuild_) return nullptr;  // a real failure, already logged
        Sleep(5);
    }

    app::Log("vulkan: the window still had no area after 200 ms");
    return nullptr;
}

void WindowTarget::DestroySizedResources(Context& ctx) {
    VkDevice dev = ctx.device();

    if (hdrFbo_) vkDestroyFramebuffer(dev, hdrFbo_, nullptr);
    hdrFbo_ = VK_NULL_HANDLE;

    for (VkFramebuffer fb : presentFbos_) vkDestroyFramebuffer(dev, fb, nullptr);
    presentFbos_.clear();

    for (VkSemaphore s : renderFinished_) vkDestroySemaphore(dev, s, nullptr);
    renderFinished_.clear();

    for (VkImageView v : imageViews_) vkDestroyImageView(dev, v, nullptr);
    imageViews_.clear();
    images_.clear();

    for (VkImageView v : bloomViews_) vkDestroyImageView(dev, v, nullptr);
    bloomViews_.clear();
    if (bloomImage_) vmaDestroyImage(ctx.allocator(), bloomImage_, bloomAlloc_);
    bloomImage_ = VK_NULL_HANDLE;
    bloomAlloc_ = VK_NULL_HANDLE;

    if (hdrView_) vkDestroyImageView(dev, hdrView_, nullptr);
    if (hdrImage_) vmaDestroyImage(ctx.allocator(), hdrImage_, hdrAlloc_);
    hdrView_  = VK_NULL_HANDLE;
    hdrImage_ = VK_NULL_HANDLE;
    hdrAlloc_ = VK_NULL_HANDLE;

    if (depthView_) vkDestroyImageView(dev, depthView_, nullptr);
    if (depthImage_) vmaDestroyImage(ctx.allocator(), depthImage_, depthAlloc_);
    depthView_  = VK_NULL_HANDLE;
    depthImage_ = VK_NULL_HANDLE;
    depthAlloc_ = VK_NULL_HANDLE;
}

// Spec 8.1 asks for five or six mips. Six on a 4K display, five on 1080p — the chain stops when a
// level would be narrower than four texels, below which the tent filter is reading mostly itself.
bool WindowTarget::CreateBloomChain(Context& ctx) {
    uint32_t levels = 1;
    {
        VkExtent2D e = bloomExtent(0);
        while (levels < 6 && e.width > 8 && e.height > 8) {
            ++levels;
            e = bloomExtent(levels - 1);
        }
    }

    VkImageCreateInfo ici{};
    ici.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.imageType     = VK_IMAGE_TYPE_2D;
    ici.format        = ctx.bloomFormat();
    ici.extent        = {bloomExtent(0).width, bloomExtent(0).height, 1};
    ici.mipLevels     = levels;
    ici.arrayLayers   = 1;
    ici.samples       = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling        = VK_IMAGE_TILING_OPTIMAL;
    ici.usage         = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ici.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo aci{};
    aci.usage    = VMA_MEMORY_USAGE_AUTO;
    aci.priority = 1.0f;

    if (vmaCreateImage(ctx.allocator(), &ici, &aci, &bloomImage_, &bloomAlloc_, nullptr) !=
        VK_SUCCESS) {
        return false;
    }

    // One view per level. A storage image binding names a single mip, and the downsample writes
    // one while sampling the one above it, so the two cannot share a view.
    bloomViews_.resize(levels, VK_NULL_HANDLE);
    for (uint32_t i = 0; i < levels; ++i) {
        VkImageViewCreateInfo vci{};
        vci.sType                           = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vci.image                           = bloomImage_;
        vci.viewType                        = VK_IMAGE_VIEW_TYPE_2D;
        vci.format                          = ctx.bloomFormat();
        vci.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        vci.subresourceRange.baseMipLevel   = i;
        vci.subresourceRange.levelCount     = 1;
        vci.subresourceRange.layerCount     = 1;
        if (vkCreateImageView(ctx.device(), &vci, nullptr, &bloomViews_[i]) != VK_SUCCESS) {
            return false;
        }
    }
    return true;
}

bool WindowTarget::Rebuild(Context& ctx, const RenderPasses& passes, uint32_t width,
                           uint32_t height) {
    ctx.WaitIdle();

    VkSurfaceCapabilitiesKHR caps{};
    const VkResult capsResult =
        vkGetPhysicalDeviceSurfaceCapabilitiesKHR(ctx.physicalDevice(), surface_, &caps);
    if (capsResult != VK_SUCCESS) {
        app::Log("vulkan: surface capabilities query failed (%d)", static_cast<int>(capsResult));
        return false;
    }

    VkExtent2D extent = caps.currentExtent;
    if (extent.width == UINT32_MAX) {  // the surface defers to us
        extent.width  = std::clamp(width, caps.minImageExtent.width, caps.maxImageExtent.width);
        extent.height = std::clamp(height, caps.minImageExtent.height, caps.maxImageExtent.height);
    }
    if (extent.width == 0 || extent.height == 0) {
        needsRebuild_ = true;  // nothing to draw into yet; try again next frame
        return false;
    }

    DestroySizedResources(ctx);

    const VkSurfaceFormatKHR surfaceFormat = ChooseSurfaceFormat(ctx, surface_);

    uint32_t imageCount = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && imageCount > caps.maxImageCount) imageCount = caps.maxImageCount;

    VkSwapchainKHR old = swapchain_;

    VkSwapchainCreateInfoKHR sci{};
    sci.sType            = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    sci.surface          = surface_;
    sci.minImageCount    = imageCount;
    sci.imageFormat      = surfaceFormat.format;
    sci.imageColorSpace  = surfaceFormat.colorSpace;
    sci.imageExtent      = extent;
    sci.imageArrayLayers = 1;
    // TRANSFER_SRC is what lets the capture path read a presented frame back. It is asked for
    // only when the surface advertises it, because a surface is not obliged to allow it and
    // demanding it unconditionally would fail swapchain creation on hardware that does not -
    // trading a diagnostic for the whole screen saver.
    sci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) {
        sci.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    }
    captureable_ = (sci.imageUsage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0;
    sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    sci.preTransform     = caps.currentTransform;
    sci.compositeAlpha   = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    sci.presentMode      = ChoosePresentMode();
    sci.clipped          = VK_TRUE;
    sci.oldSwapchain     = old;

    VkSwapchainKHR fresh = VK_NULL_HANDLE;
    const VkResult res   = vkCreateSwapchainKHR(ctx.device(), &sci, nullptr, &fresh);
    if (old) vkDestroySwapchainKHR(ctx.device(), old, nullptr);
    swapchain_ = VK_NULL_HANDLE;

    if (res != VK_SUCCESS) {
        app::Log("vulkan: vkCreateSwapchainKHR failed (%d)", static_cast<int>(res));
        return false;
    }
    swapchain_ = fresh;
    extent_    = extent;

    uint32_t count = 0;
    vkGetSwapchainImagesKHR(ctx.device(), swapchain_, &count, nullptr);
    images_.resize(count);
    vkGetSwapchainImagesKHR(ctx.device(), swapchain_, &count, images_.data());

    imageViews_.resize(count);
    presentFbos_.resize(count);
    renderFinished_.resize(count);

    for (uint32_t i = 0; i < count; ++i) {
        VkImageViewCreateInfo vci{};
        vci.sType                       = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vci.image                       = images_[i];
        vci.viewType                    = VK_IMAGE_VIEW_TYPE_2D;
        vci.format                      = surfaceFormat.format;
        vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        vci.subresourceRange.levelCount = 1;
        vci.subresourceRange.layerCount = 1;
        if (vkCreateImageView(ctx.device(), &vci, nullptr, &imageViews_[i]) != VK_SUCCESS) {
            app::Log("vulkan: swapchain image view %u failed", i);
            return false;
        }

        VkFramebufferCreateInfo fci{};
        fci.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fci.renderPass      = passes.present;
        fci.attachmentCount = 1;
        fci.pAttachments    = &imageViews_[i];
        fci.width           = extent_.width;
        fci.height          = extent_.height;
        fci.layers          = 1;
        if (vkCreateFramebuffer(ctx.device(), &fci, nullptr, &presentFbos_[i]) != VK_SUCCESS) {
            app::Log("vulkan: present framebuffer %u failed", i);
            return false;
        }

        VkSemaphoreCreateInfo sem{};
        sem.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        if (vkCreateSemaphore(ctx.device(), &sem, nullptr, &renderFinished_[i]) != VK_SUCCESS) {
            return false;
        }
    }

    if (!CreateImage2D(ctx, extent_.width, extent_.height, kHdrFormat,
                       VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                       VK_IMAGE_ASPECT_COLOR_BIT, &hdrImage_, &hdrAlloc_, &hdrView_)) {
        app::Log("vulkan: hdr target allocation failed");
        return false;
    }

    if (!CreateBloomChain(ctx)) {
        app::Log("vulkan: bloom chain allocation failed");
        return false;
    }

    const VkFormat depthFormat = ChooseDepthFormat(ctx);
    if (!CreateImage2D(ctx, extent_.width, extent_.height, depthFormat,
                       VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_IMAGE_ASPECT_DEPTH_BIT,
                       &depthImage_, &depthAlloc_, &depthView_)) {
        app::Log("vulkan: depth allocation failed");
        return false;
    }

    const VkImageView    hdrAttachments[2] = {hdrView_, depthView_};
    VkFramebufferCreateInfo fci{};
    fci.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fci.renderPass      = passes.hdr;
    fci.attachmentCount = 2;
    fci.pAttachments    = hdrAttachments;
    fci.width           = extent_.width;
    fci.height          = extent_.height;
    fci.layers          = 1;
    if (vkCreateFramebuffer(ctx.device(), &fci, nullptr, &hdrFbo_) != VK_SUCCESS) {
        app::Log("vulkan: hdr framebuffer failed");
        return false;
    }

    needsRebuild_ = false;
    app::Log("vulkan: swapchain %ux%u, %u images", extent_.width, extent_.height, count);
    return true;
}

WindowTarget::Frame WindowTarget::Begin(Context& ctx, const RenderPasses& passes) {
    Frame frame;

    if (needsRebuild_ && !Rebuild(ctx, passes, extent_.width, extent_.height)) return frame;
    if (swapchain_ == VK_NULL_HANDLE) return frame;

    const uint32_t slot = frameIndex_;
    vkWaitForFences(ctx.device(), 1, &inFlight_[slot], VK_TRUE, UINT64_MAX);

    uint32_t imageIndex = 0;
    const VkResult acquired = vkAcquireNextImageKHR(ctx.device(), swapchain_, UINT64_MAX,
                                                    imageAvailable_[slot], VK_NULL_HANDLE,
                                                    &imageIndex);
    if (acquired == VK_ERROR_OUT_OF_DATE_KHR) {
        needsRebuild_ = true;
        return frame;
    }
    if (acquired == VK_ERROR_DEVICE_LOST) {
        ctx.NoteDeviceLost();
        return frame;
    }
    if (acquired != VK_SUCCESS && acquired != VK_SUBOPTIMAL_KHR) return frame;
    if (acquired == VK_SUBOPTIMAL_KHR) needsRebuild_ = true;  // present anyway, rebuild after

    vkResetFences(ctx.device(), 1, &inFlight_[slot]);
    vkResetCommandBuffer(commandBuffers_[slot], 0);

    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(commandBuffers_[slot], &bi) != VK_SUCCESS) return frame;

    frame.cmd        = commandBuffers_[slot];
    frame.hdrFbo     = hdrFbo_;
    frame.presentFbo = presentFbos_[imageIndex];
    frame.imageIndex = imageIndex;
    frame.frameSlot  = slot;
    frame.valid      = true;
    return frame;
}

bool WindowTarget::EndAndPresent(Context& ctx, const Frame& frame) {
    if (!frame.valid) return true;

    const uint32_t slot = frameIndex_;
    if (vkEndCommandBuffer(frame.cmd) != VK_SUCCESS) return true;

    const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;

    VkSubmitInfo si{};
    si.sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.waitSemaphoreCount   = 1;
    si.pWaitSemaphores      = &imageAvailable_[slot];
    si.pWaitDstStageMask    = &waitStage;
    si.commandBufferCount   = 1;
    si.pCommandBuffers      = &frame.cmd;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores    = &renderFinished_[frame.imageIndex];

    VkResult res = vkQueueSubmit(ctx.graphicsQueue(), 1, &si, inFlight_[slot]);
    if (res == VK_ERROR_DEVICE_LOST) {
        ctx.NoteDeviceLost();
        return false;
    }
    if (res != VK_SUCCESS) return true;

    VkPresentInfoKHR pi{};
    pi.sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores    = &renderFinished_[frame.imageIndex];
    pi.swapchainCount     = 1;
    pi.pSwapchains        = &swapchain_;
    pi.pImageIndices      = &frame.imageIndex;

    res = vkQueuePresentKHR(ctx.graphicsQueue(), &pi);
    if (res == VK_ERROR_OUT_OF_DATE_KHR || res == VK_SUBOPTIMAL_KHR) {
        needsRebuild_ = true;
    } else if (res == VK_ERROR_DEVICE_LOST) {
        ctx.NoteDeviceLost();
        return false;
    }

    frameIndex_ = (frameIndex_ + 1) % kFramesInFlight;
    return true;
}

WindowTarget::~WindowTarget() {
    if (!ctx_) return;
    ctx_->WaitIdle();

    DestroySizedResources(*ctx_);

    VkDevice dev = ctx_->device();
    for (uint32_t i = 0; i < kFramesInFlight; ++i) {
        if (imageAvailable_[i]) vkDestroySemaphore(dev, imageAvailable_[i], nullptr);
        if (inFlight_[i]) vkDestroyFence(dev, inFlight_[i], nullptr);
    }
    if (commandPool_) vkDestroyCommandPool(dev, commandPool_, nullptr);
    if (swapchain_) vkDestroySwapchainKHR(dev, swapchain_, nullptr);
    if (surface_) vkDestroySurfaceKHR(ctx_->instance(), surface_, nullptr);
}

}  // namespace render::vk
