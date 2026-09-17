// M0 toolchain probe (implementation plan, M0).
//
// Brings the whole Vulkan chain up and tears it straight back down: volk's dynamic loader,
// an instance, a physical device, a logical device, and a VMA allocator. It proves the build
// and the runtime before a line of renderer code depends on either.
//
// It is NOT the renderer and MUST NOT grow into one. M2 replaces it.
#ifndef NUKE_SAVER_VK_PROBE_H
#define NUKE_SAVER_VK_PROBE_H

#include <string>

namespace probe {

struct Result {
    bool        ok = false;
    std::string stage;        // the furthest stage reached, or the one that failed
    std::string detail;       // device name on success, error text on failure
    std::string apiVersion;   // e.g. "1.4.312"
    unsigned    deviceCount = 0;
    unsigned    shaderCount = 0;
    unsigned    shaderBytes = 0;
};

// Never throws, never shows UI. Safe to call on a machine with no Vulkan runtime at all —
// that is one of the things it is here to check (spec section 12).
Result Run();

}  // namespace probe

#endif
