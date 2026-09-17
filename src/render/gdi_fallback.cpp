// The renderer of last resort (spec section 12).
//
// Paints black. That is the whole feature. It exists so that a machine with no Vulkan runtime,
// no suitable device, or a driver that fell over still gets a screen saver that blanks the
// display and exits on input, rather than an error dialog or a process that dies on launch and
// looks like a crash to whoever set it.

#include "render/renderer.h"

#include <vector>

namespace render {
namespace {

class GdiFallbackRenderer final : public Renderer {
public:
    bool AttachWindow(HWND hwnd, int width, int height) override {
        windows_.push_back({hwnd, width, height});
        return true;
    }

    void ResizeWindow(HWND hwnd, int width, int height) override {
        for (auto& w : windows_) {
            if (w.hwnd == hwnd) {
                w.width  = width;
                w.height = height;
                return;
            }
        }
    }

    void DetachWindow(HWND hwnd) override {
        for (auto it = windows_.begin(); it != windows_.end(); ++it) {
            if (it->hwnd == hwnd) {
                windows_.erase(it);
                return;
            }
        }
    }

    void RenderFrame(double, double) override {
        // Nothing animates, so there is nothing to redraw after the first paint. Repainting
        // black sixty times a second would burn a core to no effect, which is the opposite of
        // what a fallback is for.
        if (painted_) return;

        for (const auto& w : windows_) {
            HDC dc = GetDC(w.hwnd);
            if (!dc) continue;
            RECT rc{0, 0, w.width, w.height};
            FillRect(dc, &rc, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
            ReleaseDC(w.hwnd, dc);
        }
        painted_ = true;
    }

    const char* Name() const override { return "gdi-fallback"; }

private:
    struct Window {
        HWND hwnd;
        int  width;
        int  height;
    };

    std::vector<Window> windows_;
    bool                painted_ = false;
};

}  // namespace

std::unique_ptr<Renderer> CreateGdiFallbackRenderer() {
    return std::make_unique<GdiFallbackRenderer>();
}

std::unique_ptr<Renderer> CreateRenderer(const app::Settings& settings) {
    if (auto vk = CreateVulkanRenderer(settings)) return vk;
    return CreateGdiFallbackRenderer();
}

}  // namespace render
