#include "render/vk/buffer.h"

#include "app/log.h"

#include <cstring>

namespace render::vk {

bool CreateBuffer(Context& ctx, VkDeviceSize size, VkBufferUsageFlags usage, BufferUse use,
                  Buffer* out) {
    if (size == 0) return false;

    VkBufferCreateInfo bci{};
    bci.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size        = size;
    bci.usage       = usage;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo aci{};
    if (use == BufferUse::HostWritable) {
        // SEQUENTIAL_WRITE promises the driver the mapping is never read back, which lets it
        // hand out write-combined memory. Reading such a buffer is catastrophically slow, so the
        // promise has to be kept: nothing here reads from `mapped`.
        aci.usage = VMA_MEMORY_USAGE_AUTO;
        aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                    VMA_ALLOCATION_CREATE_MAPPED_BIT;
    } else {
        aci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    }

    VmaAllocationInfo info{};
    if (vmaCreateBuffer(ctx.allocator(), &bci, &aci, &out->handle, &out->alloc, &info) !=
        VK_SUCCESS) {
        app::Log("vulkan: buffer allocation failed (%llu bytes)",
                 static_cast<unsigned long long>(size));
        return false;
    }

    out->mapped = (use == BufferUse::HostWritable) ? info.pMappedData : nullptr;
    out->size   = size;
    return true;
}

void DestroyBuffer(Context& ctx, Buffer* buffer) {
    if (!buffer || !buffer->handle) return;
    vmaDestroyBuffer(ctx.allocator(), buffer->handle, buffer->alloc);
    *buffer = Buffer{};
}

bool UploadBuffer(Context& ctx, Buffer& dst, const void* data, VkDeviceSize size) {
    if (!dst || size == 0 || size > dst.size) return false;

    Buffer staging;
    if (!CreateBuffer(ctx, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, BufferUse::HostWritable,
                      &staging)) {
        return false;
    }
    std::memcpy(staging.mapped, data, static_cast<size_t>(size));

    VkCommandPoolCreateInfo pci{};
    pci.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pci.flags            = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    pci.queueFamilyIndex = ctx.graphicsFamily();

    VkCommandPool pool = VK_NULL_HANDLE;
    if (vkCreateCommandPool(ctx.device(), &pci, nullptr, &pool) != VK_SUCCESS) {
        DestroyBuffer(ctx, &staging);
        return false;
    }

    VkCommandBufferAllocateInfo cbi{};
    cbi.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbi.commandPool        = pool;
    cbi.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbi.commandBufferCount = 1;

    VkCommandBuffer cmd = VK_NULL_HANDLE;
    bool            ok  = vkAllocateCommandBuffers(ctx.device(), &cbi, &cmd) == VK_SUCCESS;

    if (ok) {
        VkCommandBufferBeginInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &bi);

        VkBufferCopy copy{};
        copy.size = size;
        vkCmdCopyBuffer(cmd, staging.handle, dst.handle, 1, &copy);
        vkEndCommandBuffer(cmd);

        VkSubmitInfo si{};
        si.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.commandBufferCount = 1;
        si.pCommandBuffers    = &cmd;

        // Generation-time only, so blocking on the queue is the right trade: a fence plus a
        // transfer queue would buy nothing at a cost nobody would maintain.
        ok = vkQueueSubmit(ctx.graphicsQueue(), 1, &si, VK_NULL_HANDLE) == VK_SUCCESS;
        if (ok) vkQueueWaitIdle(ctx.graphicsQueue());
    }

    vkDestroyCommandPool(ctx.device(), pool, nullptr);
    DestroyBuffer(ctx, &staging);
    return ok;
}

bool CreateBufferWithData(Context& ctx, const void* data, VkDeviceSize size,
                          VkBufferUsageFlags usage, Buffer* out) {
    if (!CreateBuffer(ctx, size, usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT, BufferUse::GpuOnly,
                      out)) {
        return false;
    }
    if (!UploadBuffer(ctx, *out, data, size)) {
        DestroyBuffer(ctx, out);
        return false;
    }
    return true;
}

}  // namespace render::vk
