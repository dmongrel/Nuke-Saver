// VMA-backed buffers (implementation plan, M3).
//
// A thin wrapper, not an abstraction layer: it exists so that the twenty-odd buffers this
// project ends up with do not each repeat the create/map/destroy dance, and so that a leak is a
// missing Destroy() rather than a missing vmaFreeMemory.
#ifndef NUKE_SAVER_VK_BUFFER_H
#define NUKE_SAVER_VK_BUFFER_H

#include "render/vk/context.h"

#include <cstddef>

namespace render::vk {

// How the CPU expects to reach a buffer. This is the decision that actually matters on the
// reference machine: it is an iGPU, so device-local memory is host-visible anyway and a staging
// copy would be pure overhead for the small per-frame data. Large static geometry still gets one.
enum class BufferUse {
    GpuOnly,      // device-local, written once through a staging copy
    HostWritable  // mapped for the lifetime of the buffer, written every frame
};

struct Buffer {
    VkBuffer      handle = VK_NULL_HANDLE;
    VmaAllocation alloc  = VK_NULL_HANDLE;
    void*         mapped = nullptr;  // non-null only for HostWritable
    VkDeviceSize  size   = 0;

    explicit operator bool() const { return handle != VK_NULL_HANDLE; }
};

bool CreateBuffer(Context& ctx, VkDeviceSize size, VkBufferUsageFlags usage, BufferUse use,
                  Buffer* out);
void DestroyBuffer(Context& ctx, Buffer* buffer);

// Uploads through a temporary staging buffer and a one-shot command buffer, waiting for it to
// complete. Only ever called during world generation, where a stall costs nothing and the
// alternative is a transfer queue nobody needs.
bool UploadBuffer(Context& ctx, Buffer& dst, const void* data, VkDeviceSize size);

// Creates a device-local buffer and fills it in one step. The common case for static geometry.
bool CreateBufferWithData(Context& ctx, const void* data, VkDeviceSize size,
                          VkBufferUsageFlags usage, Buffer* out);

}  // namespace render::vk

#endif
