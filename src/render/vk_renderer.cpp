// Vulkan renderer construction.
//
// M1 placeholder: returns nullptr so the host exercises the GDI fallback path on every run.
// That is deliberate. The fallback is a hard requirement (spec section 12) and the cheapest way
// to keep it working is to make it the only thing running until M2 replaces this.
//
// M2 fills this in: instance, device, per-window swapchain, HDR target, tonemap.

#include "render/renderer.h"

namespace render {

std::unique_ptr<Renderer> CreateVulkanRenderer(const app::Settings&) {
    return nullptr;
}

}  // namespace render
