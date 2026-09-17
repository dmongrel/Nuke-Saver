// Everything that belongs to one monitor (spec 9.2).
//
// Surface, swapchain, the HDR offscreen target the scene is drawn into, the depth buffer, and
// the per-frame synchronisation. The device, allocator and pipelines are shared and live
// elsewhere: displays can differ in size, DPI and refresh rate, and only this part varies with
// them.
#ifndef NUKE_SAVER_VK_WINDOW_TARGET_H
#define NUKE_SAVER_VK_WINDOW_TARGET_H

#include "render/vk/context.h"

#include <windows.h>

#include <memory>
#include <vector>

namespace render::vk {

// Frames the CPU may run ahead of the GPU. Two is enough to keep the queue fed without adding
// the latency a third would.
constexpr uint32_t kFramesInFlight = 2;

struct RenderPasses {
    VkRenderPass hdr     = VK_NULL_HANDLE;  // scene -> R16G16B16A16_SFLOAT + depth
    VkRenderPass present = VK_NULL_HANDLE;  // tonemap -> swapchain
};

class WindowTarget {
public:
    ~WindowTarget();

    WindowTarget(const WindowTarget&)            = delete;
    WindowTarget& operator=(const WindowTarget&) = delete;

    static std::unique_ptr<WindowTarget> Create(Context& ctx, const RenderPasses& passes,
                                                HWND hwnd, uint32_t width, uint32_t height);

    HWND     hwnd() const { return hwnd_; }
    uint32_t width() const { return extent_.width; }
    uint32_t height() const { return extent_.height; }

    VkImageView hdrView() const { return hdrView_; }

    // The bloom chain of spec 8.1: one image, mip 0 at half the swapchain, five or six levels.
    // Sized with the swapchain, so it lives here rather than with the pipelines.
    //
    // Every level is kept in VK_IMAGE_LAYOUT_GENERAL for the whole frame. The chain is written by
    // compute and read by compute, level by level, and alternating each mip between GENERAL and
    // SHADER_READ_ONLY would be a dozen barriers a frame to buy a layout optimisation that does
    // not exist on a tiler-free desktop GPU.
    uint32_t    bloomLevels() const { return static_cast<uint32_t>(bloomViews_.size()); }
    VkImageView bloomView(uint32_t level) const { return bloomViews_[level]; }
    VkImage     bloomImage() const { return bloomImage_; }
    VkExtent2D  bloomExtent(uint32_t level) const {
        VkExtent2D e{extent_.width / 2, extent_.height / 2};
        for (uint32_t i = 0; i < level; ++i) {
            e.width  = e.width > 1 ? e.width / 2 : 1;
            e.height = e.height > 1 ? e.height / 2 : 1;
        }
        return {e.width ? e.width : 1, e.height ? e.height : 1};
    }

    // Marks the swapchain as needing rebuild before the next frame, after a resize or an
    // out-of-date present.
    void Invalidate() { needsRebuild_ = true; }
    bool needsRebuild() const { return needsRebuild_; }

    // Rebuilds the swapchain and everything sized with it. Returns false when the window has no
    // area yet (minimised, or mid-resize), which is a reason to skip the frame, not to fail.
    bool Rebuild(Context& ctx, const RenderPasses& passes, uint32_t width, uint32_t height);

    // One frame's worth of state, handed to the renderer to record into.
    struct Frame {
        VkCommandBuffer cmd         = VK_NULL_HANDLE;
        VkFramebuffer   hdrFbo      = VK_NULL_HANDLE;
        VkFramebuffer   presentFbo  = VK_NULL_HANDLE;
        uint32_t        imageIndex  = 0;
        // Which frame-in-flight slot this is. The renderer needs it to pick the matching copy of
        // any per-frame resource it owns - uniform buffers above all - because writing the copy
        // the GPU is still reading is the classic way to get a frame of stale camera.
        uint32_t        frameSlot   = 0;
        bool            valid       = false;
    };

    // Waits for the slot, acquires an image, and begins recording. An invalid Frame means skip
    // this window this frame — the swapchain is being rebuilt or the window has no area.
    Frame Begin(Context& ctx, const RenderPasses& passes);

    // Ends recording, submits and presents. Returns false if the device was lost.
    bool EndAndPresent(Context& ctx, const Frame& frame);

    VkExtent2D extent() const { return extent_; }

    // The swapchain image behind an acquired frame. Only the capture path wants this: reading
    // back what was actually presented is the one check that cannot be faked by re-rendering.
    // Null unless captureable(), so a caller that forgets to check cannot silently copy from an
    // image the surface never agreed to allow transfers from.
    VkImage swapchainImage(uint32_t index) const {
        if (!captureable_ || index >= images_.size()) return VK_NULL_HANDLE;
        return images_[index];
    }

    // Whether the surface allowed TRANSFER_SRC on its images. Capture is impossible without it.
    bool captureable() const { return captureable_; }

private:
    WindowTarget() = default;

    void DestroySizedResources(Context& ctx);
    bool CreateBloomChain(Context& ctx);

    // Held so the destructor can free GPU objects. The Context always outlives every target:
    // the renderer owns both and destroys the targets first.
    Context*       ctx_     = nullptr;
    HWND           hwnd_    = nullptr;
    VkSurfaceKHR   surface_ = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkExtent2D     extent_{0, 0};
    bool           needsRebuild_ = false;
    bool           captureable_  = false;

    std::vector<VkImage>     images_;
    std::vector<VkImageView> imageViews_;
    std::vector<VkFramebuffer> presentFbos_;
    // One per swapchain image: signalling completion per image rather than per frame-in-flight
    // is what keeps a present from waiting on a semaphore another image is still using.
    std::vector<VkSemaphore> renderFinished_;

    VkImage       hdrImage_ = VK_NULL_HANDLE;
    VmaAllocation hdrAlloc_ = VK_NULL_HANDLE;
    VkImageView   hdrView_  = VK_NULL_HANDLE;

    VkImage                  bloomImage_ = VK_NULL_HANDLE;
    VmaAllocation            bloomAlloc_ = VK_NULL_HANDLE;
    std::vector<VkImageView> bloomViews_;

    VkImage       depthImage_ = VK_NULL_HANDLE;
    VmaAllocation depthAlloc_ = VK_NULL_HANDLE;
    VkImageView   depthView_  = VK_NULL_HANDLE;
    VkFramebuffer hdrFbo_     = VK_NULL_HANDLE;

    VkCommandPool   commandPool_ = VK_NULL_HANDLE;
    VkCommandBuffer commandBuffers_[kFramesInFlight]{};
    VkSemaphore     imageAvailable_[kFramesInFlight]{};
    VkFence         inFlight_[kFramesInFlight]{};
    uint32_t        frameIndex_ = 0;
};

// Both render passes, created once and shared by every window because every window uses the
// same formats.
bool CreateRenderPasses(Context& ctx, RenderPasses* out);
void DestroyRenderPasses(Context& ctx, RenderPasses* passes);

VkFormat ChooseDepthFormat(Context& ctx);

}  // namespace render::vk

#endif
