// The Vulkan renderer (implementation plan, M2 and M3).
//
// Owns the device, one target per monitor, the shared pipelines, and the generated world every
// monitor presents. The world is generated once and only read by rendering — which is what lets
// a second monitor show the same cycle from its own aspect ratio without a second generation
// pass.
//
// Construction failure is ordinary, not exceptional. Spec section 12 requires that a machine
// without a working Vulkan runtime still gets a screen saver, so every failure path here returns
// nullptr and the host quietly uses GDI instead.

#include "render/renderer.h"

#include "app/capture.h"
#include "app/log.h"
#include "app/settings.h"
#include "render/scene_uniforms.h"
#include "render/shaders_embedded.h"
#include "render/vk/buffer.h"
#include "render/vk/context.h"
#include "render/vk/window_target.h"
#include "world/world.h"

#include <cstring>
#include <vector>

namespace render {
namespace {

using vk::Buffer;
using vk::Context;
using vk::kFramesInFlight;
using vk::RenderPasses;
using vk::WindowTarget;

struct TonemapPush {
    float exposure;
    float ditherAmp;
};

VkShaderModule LoadShader(VkDevice device, const char* name) {
    const ShaderBlob* blob = shader_blob(name);
    if (!blob) {
        app::Log("vulkan: shader '%s' is not embedded", name);
        return VK_NULL_HANDLE;
    }

    VkShaderModuleCreateInfo ci{};
    ci.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = blob->size;
    ci.pCode    = reinterpret_cast<const uint32_t*>(blob->data);

    VkShaderModule module = VK_NULL_HANDLE;
    if (vkCreateShaderModule(device, &ci, nullptr, &module) != VK_SUCCESS) {
        app::Log("vulkan: shader '%s' failed to create", name);
        return VK_NULL_HANDLE;
    }
    return module;
}

// Everything a graphics pipeline here has in common. Only the parts that genuinely differ between
// passes are parameters; the rest would otherwise be sixty lines repeated per pipeline, which is
// how a subtle difference gets introduced by accident.
struct PipelineDesc {
    const char*      vert       = nullptr;
    const char*      frag       = nullptr;
    VkRenderPass     renderPass = VK_NULL_HANDLE;
    VkPipelineLayout layout     = VK_NULL_HANDLE;
    bool             depthTest  = false;
    bool             depthWrite = false;
};

VkPipeline CreateGraphicsPipeline(VkDevice dev, const PipelineDesc& desc) {
    VkShaderModule vert = LoadShader(dev, desc.vert);
    VkShaderModule frag = LoadShader(dev, desc.frag);
    if (!vert || !frag) {
        if (vert) vkDestroyShaderModule(dev, vert, nullptr);
        if (frag) vkDestroyShaderModule(dev, frag, nullptr);
        return VK_NULL_HANDLE;
    }

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert;
    stages[0].pName  = "main";
    stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = frag;
    stages[1].pName  = "main";

    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

    VkPipelineInputAssemblyStateCreateInfo assembly{};
    assembly.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewport{};
    viewport.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport.viewportCount = 1;
    viewport.scissorCount  = 1;

    VkPipelineRasterizationStateCreateInfo raster{};
    raster.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode    = VK_CULL_MODE_NONE;
    raster.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth   = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depth{};
    depth.sType            = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depth.depthTestEnable  = desc.depthTest ? VK_TRUE : VK_FALSE;
    depth.depthWriteEnable = desc.depthWrite ? VK_TRUE : VK_FALSE;
    depth.depthCompareOp   = VK_COMPARE_OP_LESS_OR_EQUAL;
    depth.maxDepthBounds   = 1.0f;

    VkPipelineColorBlendAttachmentState blendAttachment{};
    blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                     VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendStateCreateInfo blend{};
    blend.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount = 1;
    blend.pAttachments    = &blendAttachment;

    // Viewport and scissor are dynamic so one pipeline serves monitors of different sizes.
    const VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};

    VkPipelineDynamicStateCreateInfo dynamic{};
    dynamic.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates    = dynamicStates;

    VkGraphicsPipelineCreateInfo gpi{};
    gpi.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    gpi.stageCount          = 2;
    gpi.pStages             = stages;
    gpi.pVertexInputState   = &vertexInput;
    gpi.pInputAssemblyState = &assembly;
    gpi.pViewportState      = &viewport;
    gpi.pRasterizationState = &raster;
    gpi.pMultisampleState   = &multisample;
    gpi.pDepthStencilState  = &depth;
    gpi.pColorBlendState    = &blend;
    gpi.pDynamicState       = &dynamic;
    gpi.layout              = desc.layout;
    gpi.renderPass          = desc.renderPass;
    gpi.subpass             = 0;

    VkPipeline     pipeline = VK_NULL_HANDLE;
    const VkResult res =
        vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpi, nullptr, &pipeline);

    vkDestroyShaderModule(dev, vert, nullptr);
    vkDestroyShaderModule(dev, frag, nullptr);

    if (res != VK_SUCCESS) {
        app::Log("vulkan: pipeline %s/%s failed (%d)", desc.vert, desc.frag, static_cast<int>(res));
        return VK_NULL_HANDLE;
    }
    return pipeline;
}

class VulkanRenderer final : public Renderer {
public:
    VulkanRenderer(std::unique_ptr<Context> ctx, const world::World& world)
        : ctx_(std::move(ctx)), world_(world) {}

    ~VulkanRenderer() override {
        if (!ctx_) return;
        ctx_->WaitIdle();

        vk::DestroyBuffer(*ctx_, &captureBuffer_);
        for (auto& w : windows_) DestroyAttached(w);
        windows_.clear();

        VkDevice dev = ctx_->device();
        if (skyPipeline_) vkDestroyPipeline(dev, skyPipeline_, nullptr);
        if (scenePipelineLayout_) vkDestroyPipelineLayout(dev, scenePipelineLayout_, nullptr);
        if (sceneSetLayout_) vkDestroyDescriptorSetLayout(dev, sceneSetLayout_, nullptr);

        if (tonemapPipeline_) vkDestroyPipeline(dev, tonemapPipeline_, nullptr);
        if (tonemapPipelineLayout_) vkDestroyPipelineLayout(dev, tonemapPipelineLayout_, nullptr);
        if (tonemapSetLayout_) vkDestroyDescriptorSetLayout(dev, tonemapSetLayout_, nullptr);
        if (sampler_) vkDestroySampler(dev, sampler_, nullptr);

        if (descriptorPool_) vkDestroyDescriptorPool(dev, descriptorPool_, nullptr);
        vk::DestroyRenderPasses(*ctx_, &passes_);
    }

    bool Init() {
        if (!vk::CreateRenderPasses(*ctx_, &passes_)) return false;
        if (!CreateDescriptorPool()) return false;
        if (!CreateSceneLayout()) return false;
        if (!CreateTonemapLayout()) return false;

        VkDevice dev = ctx_->device();

        // The sky neither tests nor writes depth: it is behind everything by definition, and it
        // is drawn first so terrain and city simply overwrite it.
        skyPipeline_ = CreateGraphicsPipeline(
            dev, {"sky.vert", "sky.frag", passes_.hdr, scenePipelineLayout_, false, false});
        if (!skyPipeline_) return false;

        tonemapPipeline_ = CreateGraphicsPipeline(
            dev, {"fullscreen.vert", "tonemap.frag", passes_.present, tonemapPipelineLayout_, false,
                  false});
        return tonemapPipeline_ != VK_NULL_HANDLE;
    }

    bool AttachWindow(HWND hwnd, int width, int height) override {
        if (!ctx_ || ctx_->deviceLost()) return false;

        Attached a;
        a.target = WindowTarget::Create(*ctx_, passes_, hwnd, static_cast<uint32_t>(width),
                                        static_cast<uint32_t>(height));
        if (!a.target) return false;

        // One uniform buffer per frame in flight. Sharing a single buffer would mean writing the
        // copy the GPU is still reading, which shows up as a frame of the previous camera - or,
        // worse, half of each.
        for (uint32_t i = 0; i < kFramesInFlight; ++i) {
            if (!vk::CreateBuffer(*ctx_, sizeof(SceneUniforms), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                  vk::BufferUse::HostWritable, &a.sceneUbo[i]) ||
                !AllocateSet(sceneSetLayout_, &a.sceneSet[i])) {
                DestroyAttached(a);
                return false;
            }
            BindUniformBuffer(a.sceneSet[i], a.sceneUbo[i]);
        }

        if (!AllocateSet(tonemapSetLayout_, &a.tonemapSet)) {
            DestroyAttached(a);
            return false;
        }

        windows_.push_back(std::move(a));
        return true;
    }

    void ResizeWindow(HWND hwnd, int, int) override {
        for (auto& w : windows_) {
            if (w.target->hwnd() == hwnd) {
                w.target->Invalidate();
                return;
            }
        }
    }

    void DetachWindow(HWND hwnd) override {
        for (auto it = windows_.begin(); it != windows_.end(); ++it) {
            if (it->target->hwnd() == hwnd) {
                ctx_->WaitIdle();
                DestroyAttached(*it);
                windows_.erase(it);
                return;
            }
        }
    }

    void RenderFrame(double elapsed, double) override {
        if (!ctx_ || ctx_->deviceLost()) return;

        const float t = static_cast<float>(elapsed);

        for (auto& w : windows_) {
            WindowTarget::Frame frame = w.target->Begin(*ctx_, passes_);
            if (!frame.valid) continue;

            UpdateSceneUniforms(w, frame.frameSlot, t);
            RefreshTonemapBinding(w);

            RecordScene(frame, w);
            RecordTonemap(frame, w);

            const bool capturing = capture_.enabled && frameCounter_ == capture_.atFrame;
            if (capturing) RecordCaptureCopy(frame, w);

            if (!w.target->EndAndPresent(*ctx_, frame)) return;  // device lost
            if (capturing) FinishCapture(w);
        }

        ++frameCounter_;
    }

    void WaitIdle() override {
        if (ctx_) ctx_->WaitIdle();
    }

    // The swapchain is FIFO, so EndAndPresent blocks until the next vblank.
    bool PacesItself() const override { return true; }

    const char* Name() const override { return "vulkan"; }

private:
    struct Attached {
        std::unique_ptr<WindowTarget> target;
        Buffer                        sceneUbo[kFramesInFlight]{};
        VkDescriptorSet               sceneSet[kFramesInFlight]{};
        VkDescriptorSet               tonemapSet   = VK_NULL_HANDLE;
        VkImageView                   boundHdrView = VK_NULL_HANDLE;
    };

    void DestroyAttached(Attached& a) {
        for (uint32_t i = 0; i < kFramesInFlight; ++i) vk::DestroyBuffer(*ctx_, &a.sceneUbo[i]);
        a.target.reset();
        // Descriptor sets are freed with the pool, which outlives every window.
    }

    bool CreateDescriptorPool() {
        // Sized for eight monitors: more than anyone attaches, and still trivially small.
        const VkDescriptorPoolSize sizes[] = {
            {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 8 * kFramesInFlight},
            {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 8},
        };

        VkDescriptorPoolCreateInfo dpi{};
        dpi.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        dpi.maxSets       = 8 * (kFramesInFlight + 1);
        dpi.poolSizeCount = 2;
        dpi.pPoolSizes    = sizes;
        return vkCreateDescriptorPool(ctx_->device(), &dpi, nullptr, &descriptorPool_) == VK_SUCCESS;
    }

    bool CreateSceneLayout() {
        VkDescriptorSetLayoutBinding binding{};
        binding.binding         = 0;
        binding.descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        binding.descriptorCount = 1;
        // Visible to both stages: the vertex stage needs viewProj for the geometry M3b adds, the
        // fragment stage needs the lighting.
        binding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

        VkDescriptorSetLayoutCreateInfo dsl{};
        dsl.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        dsl.bindingCount = 1;
        dsl.pBindings    = &binding;
        if (vkCreateDescriptorSetLayout(ctx_->device(), &dsl, nullptr, &sceneSetLayout_) !=
            VK_SUCCESS) {
            return false;
        }

        VkPipelineLayoutCreateInfo pli{};
        pli.sType          = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pli.setLayoutCount = 1;
        pli.pSetLayouts    = &sceneSetLayout_;
        return vkCreatePipelineLayout(ctx_->device(), &pli, nullptr, &scenePipelineLayout_) ==
               VK_SUCCESS;
    }

    bool CreateTonemapLayout() {
        VkDevice dev = ctx_->device();

        VkSamplerCreateInfo sci{};
        sci.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        sci.magFilter    = VK_FILTER_LINEAR;
        sci.minFilter    = VK_FILTER_LINEAR;
        sci.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.maxLod       = VK_LOD_CLAMP_NONE;
        if (vkCreateSampler(dev, &sci, nullptr, &sampler_) != VK_SUCCESS) return false;

        VkDescriptorSetLayoutBinding binding{};
        binding.binding         = 0;
        binding.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        binding.descriptorCount = 1;
        binding.stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;

        VkDescriptorSetLayoutCreateInfo dsl{};
        dsl.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        dsl.bindingCount = 1;
        dsl.pBindings    = &binding;
        if (vkCreateDescriptorSetLayout(dev, &dsl, nullptr, &tonemapSetLayout_) != VK_SUCCESS) {
            return false;
        }

        VkPushConstantRange push{};
        push.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        push.size       = sizeof(TonemapPush);

        VkPipelineLayoutCreateInfo pli{};
        pli.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pli.setLayoutCount         = 1;
        pli.pSetLayouts            = &tonemapSetLayout_;
        pli.pushConstantRangeCount = 1;
        pli.pPushConstantRanges    = &push;
        return vkCreatePipelineLayout(dev, &pli, nullptr, &tonemapPipelineLayout_) == VK_SUCCESS;
    }

    bool AllocateSet(VkDescriptorSetLayout layout, VkDescriptorSet* out) {
        VkDescriptorSetAllocateInfo ai{};
        ai.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ai.descriptorPool     = descriptorPool_;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts        = &layout;
        return vkAllocateDescriptorSets(ctx_->device(), &ai, out) == VK_SUCCESS;
    }

    void BindUniformBuffer(VkDescriptorSet set, const Buffer& buffer) {
        VkDescriptorBufferInfo info{};
        info.buffer = buffer.handle;
        info.range  = buffer.size;

        VkWriteDescriptorSet write{};
        write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet          = set;
        write.dstBinding      = 0;
        write.descriptorCount = 1;
        write.descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        write.pBufferInfo     = &info;

        vkUpdateDescriptorSets(ctx_->device(), 1, &write, 0, nullptr);
    }

    // The HDR view changes whenever the swapchain is rebuilt. Rebuild waits for the device to go
    // idle first, so rebinding here cannot race an in-flight frame.
    void RefreshTonemapBinding(Attached& w) {
        const VkImageView view = w.target->hdrView();
        if (view == w.boundHdrView) return;

        VkDescriptorImageInfo info{};
        info.sampler     = sampler_;
        info.imageView   = view;
        info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkWriteDescriptorSet write{};
        write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet          = w.tonemapSet;
        write.dstBinding      = 0;
        write.descriptorCount = 1;
        write.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo      = &info;

        vkUpdateDescriptorSets(ctx_->device(), 1, &write, 0, nullptr);
        w.boundHdrView = view;
    }

    void UpdateSceneUniforms(Attached& w, uint32_t slot, float t) {
        const VkExtent2D extent = w.target->extent();
        const float      aspect =
            extent.height
                     ? static_cast<float>(extent.width) / static_cast<float>(extent.height)
                     : 1.0f;

        // The camera is shared, the projection is not. Spec 11.1 requires the shot to work at
        // 16:9 and 21:9 without cropping, so two monitors of different shapes need different
        // projections of the same camera.
        const world::CameraState cam = world_.CameraAt(t);

        // The far plane has to clear the horizon range, which stands well beyond the 8 km basin.
        const core::Mat4 proj = cam.Projection(aspect, 0.5f, 60000.0f);

        SceneUniforms uniforms{};
        FillSceneUniforms(&uniforms, world_.sky, cam.View(), proj, cam.eye, t);

        std::memcpy(w.sceneUbo[slot].mapped, &uniforms, sizeof(uniforms));
    }

    void SetViewport(VkCommandBuffer cmd, VkExtent2D extent) {
        VkViewport vp{};
        vp.width    = static_cast<float>(extent.width);
        vp.height   = static_cast<float>(extent.height);
        vp.maxDepth = 1.0f;
        vkCmdSetViewport(cmd, 0, 1, &vp);

        VkRect2D scissor{{0, 0}, extent};
        vkCmdSetScissor(cmd, 0, 1, &scissor);
    }

    void RecordScene(const WindowTarget::Frame& frame, Attached& w) {
        // The colour clear is a formality now that the sky covers every pixel, but it stays: a
        // clear costs less than the LOAD_OP_LOAD that dropping it would imply.
        VkClearValue clears[2]{};
        clears[0].color        = {{0.0f, 0.0f, 0.0f, 1.0f}};
        clears[1].depthStencil = {1.0f, 0};

        VkRenderPassBeginInfo bi{};
        bi.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        bi.renderPass        = passes_.hdr;
        bi.framebuffer       = frame.hdrFbo;
        bi.renderArea.extent = w.target->extent();
        bi.clearValueCount   = 2;
        bi.pClearValues      = clears;

        vkCmdBeginRenderPass(frame.cmd, &bi, VK_SUBPASS_CONTENTS_INLINE);
        SetViewport(frame.cmd, w.target->extent());

        vkCmdBindDescriptorSets(frame.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, scenePipelineLayout_, 0,
                                1, &w.sceneSet[frame.frameSlot], 0, nullptr);

        vkCmdBindPipeline(frame.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, skyPipeline_);
        vkCmdDraw(frame.cmd, 3, 1, 0, 0);

        // M3b draws terrain, the horizon range and the city here, over the sky and with depth.

        vkCmdEndRenderPass(frame.cmd);
    }

    void RecordTonemap(const WindowTarget::Frame& frame, Attached& w) {
        VkRenderPassBeginInfo bi{};
        bi.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        bi.renderPass        = passes_.present;
        bi.framebuffer       = frame.presentFbo;
        bi.renderArea.extent = w.target->extent();

        vkCmdBeginRenderPass(frame.cmd, &bi, VK_SUBPASS_CONTENTS_INLINE);
        SetViewport(frame.cmd, w.target->extent());

        vkCmdBindPipeline(frame.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, tonemapPipeline_);
        vkCmdBindDescriptorSets(frame.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, tonemapPipelineLayout_,
                                0, 1, &w.tonemapSet, 0, nullptr);

        // Base exposure comes from the time of day. Night is not a darker noon, it is a different
        // exposure, or the city's own windows read as dim rather than as the light. M5 replaces
        // the fixed value with the histogram-driven adaptation of spec 8.2.
        TonemapPush push{world_.sky.baseExposure, 1.0f};
        vkCmdPushConstants(frame.cmd, tonemapPipelineLayout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                           sizeof(push), &push);

        vkCmdDraw(frame.cmd, 3, 1, 0, 0);
        vkCmdEndRenderPass(frame.cmd);
    }

    // Copies the image that was just presented into a host-visible buffer. Recorded into the
    // frame's own command buffer, after the present pass has left the image in PRESENT_SRC, so
    // what lands in the file is exactly what went to the display - not a re-render that might
    // differ.
    void RecordCaptureCopy(const WindowTarget::Frame& frame, Attached& w) {
        const VkExtent2D   extent = w.target->extent();
        const VkDeviceSize needed = static_cast<VkDeviceSize>(extent.width) * extent.height * 4;

        if (!captureBuffer_ &&
            !vk::CreateBuffer(*ctx_, needed, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                              vk::BufferUse::HostWritable, &captureBuffer_)) {
            app::Log("capture: buffer allocation failed");
            capture_.enabled = false;
            return;
        }

        VkImage image = w.target->swapchainImage(frame.imageIndex);
        if (!image) return;

        VkImageMemoryBarrier toSrc{};
        toSrc.sType                       = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        toSrc.oldLayout                   = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        toSrc.newLayout                   = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        toSrc.srcAccessMask               = 0;
        toSrc.dstAccessMask               = VK_ACCESS_TRANSFER_READ_BIT;
        toSrc.srcQueueFamilyIndex         = VK_QUEUE_FAMILY_IGNORED;
        toSrc.dstQueueFamilyIndex         = VK_QUEUE_FAMILY_IGNORED;
        toSrc.image                       = image;
        toSrc.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        toSrc.subresourceRange.levelCount = 1;
        toSrc.subresourceRange.layerCount = 1;

        vkCmdPipelineBarrier(frame.cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toSrc);

        VkBufferImageCopy copy{};
        copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copy.imageSubresource.layerCount = 1;
        copy.imageExtent                 = {extent.width, extent.height, 1};
        vkCmdCopyImageToBuffer(frame.cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               captureBuffer_.handle, 1, &copy);

        // Back to PRESENT_SRC, because this image is about to be presented.
        VkImageMemoryBarrier toPresent = toSrc;
        toPresent.oldLayout            = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        toPresent.newLayout            = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        toPresent.srcAccessMask        = VK_ACCESS_TRANSFER_READ_BIT;
        toPresent.dstAccessMask        = 0;

        vkCmdPipelineBarrier(frame.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1,
                             &toPresent);
    }

    void FinishCapture(Attached& w) {
        if (!captureBuffer_) return;

        // A full stall. This runs once, off a debug environment variable, and the alternative is
        // threading a fence through the frame loop for a path nobody ships.
        ctx_->WaitIdle();

        const VkExtent2D extent = w.target->extent();
        app::WritePngFromBgra(capture_.path.c_str(),
                              static_cast<const uint8_t*>(captureBuffer_.mapped), extent.width,
                              extent.height, extent.width * 4);

        vk::DestroyBuffer(*ctx_, &captureBuffer_);
        capture_.enabled = false;  // once is the point
    }

    std::unique_ptr<Context> ctx_;
    world::World             world_;
    app::CaptureRequest      capture_ = app::CaptureRequestFromEnvironment();
    Buffer                   captureBuffer_{};
    int                      frameCounter_ = 0;
    RenderPasses             passes_{};
    std::vector<Attached>    windows_;

    VkDescriptorPool descriptorPool_ = VK_NULL_HANDLE;

    VkDescriptorSetLayout sceneSetLayout_      = VK_NULL_HANDLE;
    VkPipelineLayout      scenePipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline            skyPipeline_         = VK_NULL_HANDLE;

    VkSampler             sampler_               = VK_NULL_HANDLE;
    VkDescriptorSetLayout tonemapSetLayout_      = VK_NULL_HANDLE;
    VkPipelineLayout      tonemapPipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline            tonemapPipeline_       = VK_NULL_HANDLE;
};

// Validation is expensive and noisy, and a screen saver has no business loading a layer on a
// user's machine. It comes on only when debug logging is already on.
bool WantValidation() { return app::LogEnabled(); }

}  // namespace

std::unique_ptr<Renderer> CreateVulkanRenderer(const app::Settings& settings) {
    auto ctx = Context::Create(WantValidation());
    if (!ctx) return nullptr;

    auto renderer = std::make_unique<VulkanRenderer>(std::move(ctx), world::Generate(settings));
    if (!renderer->Init()) {
        app::Log("vulkan: renderer init failed, falling back");
        return nullptr;
    }
    return renderer;
}

}  // namespace render
