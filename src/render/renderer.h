// The boundary between the Win32 host and whatever is drawing (implementation plan, M1).
//
// Two backends implement this: the GDI fallback, which paints black and always works, and the
// Vulkan renderer from M2 onward. The host never knows which it has — spec section 12 requires
// that a machine with no Vulkan runtime still gets a working screen saver, and the only way to
// keep that true is for the fallback to sit behind the same interface from the start rather
// than being bolted on at the end when nothing exercises it.
#ifndef NUKE_SAVER_RENDERER_H
#define NUKE_SAVER_RENDERER_H

#include <windows.h>

#include <memory>

namespace app {
struct Settings;
}

namespace render {

class Renderer {
public:
    virtual ~Renderer() = default;

    // Attach a full-screen window. Returns false if this backend cannot present to it, which
    // the host treats as a reason to fall back rather than to fail.
    virtual bool AttachWindow(HWND hwnd, int width, int height) = 0;

    // The monitor changed shape, or its swapchain went out of date.
    virtual void ResizeWindow(HWND hwnd, int width, int height) = 0;

    // The monitor went away (spec A12). Must not disturb the remaining windows.
    virtual void DetachWindow(HWND hwnd) = 0;

    // Advance the simulation and draw every attached window. `elapsed` is seconds since the
    // cycle began and `delta` is the clamped frame delta (spec 4.2).
    virtual void RenderFrame(double elapsed, double delta) = 0;

    // Block until the GPU is finished with everything. Called once, before teardown (spec 9.2).
    virtual void WaitIdle() {}

    // For diagnostics only.
    virtual const char* Name() const = 0;
};

// Builds the Vulkan renderer, or returns nullptr if anything at all goes wrong. Never throws,
// never shows UI, never logs to the user.
std::unique_ptr<Renderer> CreateVulkanRenderer(const app::Settings& settings);

// Always succeeds.
std::unique_ptr<Renderer> CreateGdiFallbackRenderer();

// Tries Vulkan, falls back to GDI. This is the only entry point the host should use.
std::unique_ptr<Renderer> CreateRenderer(const app::Settings& settings);

}  // namespace render

#endif
