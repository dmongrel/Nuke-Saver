// The Vulkan renderer (implementation plan, M2).
//
// At this milestone it clears the HDR target and resolves it to every swapchain through the
// tonemap pass. That is deliberately the whole of it: the point of M2 is to prove the device,
// the per-window swapchains, the frames in flight and the HDR path, so that M3 can add geometry
// without also debugging presentation.
//
// Construction failure is ordinary, not exceptional. Spec section 12 requires that a machine
// without a working Vulkan runtime still gets a screen saver, so every failure path here returns
// nullptr and the host quietly uses GDI instead.

#include "render/renderer.h"

#include "app/log.h"
#include "app/settings.h"
#include "render/shaders_embedded.h"
#include "render/vk/context.h"
#include "render/vk/window_target.h"

#include <cmath>
#include <vector>

namespace render {
namespace {

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

class VulkanRenderer final : public Renderer {
public:
    explicit VulkanRenderer(std::unique_ptr<Context> ctx) : ctx_(std::move(ctx)) {}

    ~VulkanRenderer() override {
        if (!ctx_) return;
        ctx_->WaitIdle();

        windows_.clear();  // targets free their own GPU objects

        VkDevice dev = ctx_->device();
        if (pipeline_) vkDestroyPipeline(dev, pipeline_, nullptr);
        if (pipelineLayout_) vkDestroyPipelineLayout(dev, pipelineLayout_, nullptr);
        if (descriptorPool_) vkDestroyDescriptorPool(dev, descriptorPool_, nullptr);
        if (setLayout_) vkDestroyDescriptorSetLayout(dev, setLayout_, nullptr);
        if (sampler_) vkDestroySampler(dev, sampler_, nullptr);
        vk::DestroyRenderPasses(*ctx_, &passes_);
    }

    bool Init() {
        if (!vk::CreateRenderPasses(*ctx_, &passes_)) return false;
        if (!CreateTonemapPipeline()) return false;
        return true;
    }

    bool AttachWindow(HWND hwnd, int width, int height) override {
        if (!ctx_ || ctx_->deviceLost()) return false;

        auto target = WindowTarget::Create(*ctx_, passes_, hwnd, static_cast<uint32_t>(width),
                                           static_cast<uint32_t>(height));
        if (!target) return false;

        Attached a;
        a.target = std::move(target);
        if (!AllocateSet(&a.set)) return false;

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
                windows_.erase(it);
                return;
            }
        }
    }

    void RenderFrame(double elapsed, double) override {
        if (!ctx_ || ctx_->deviceLost()) return;

        for (auto& w : windows_) {
            WindowTarget::Frame frame = w.target->Begin(*ctx_, passes_);
            if (!frame.valid) continue;

            RefreshDescriptorIfNeeded(w);
            RecordScene(frame, w, elapsed);
            RecordTonemap(frame, w);

            if (!w.target->EndAndPresent(*ctx_, frame)) return;  // device lost
        }
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
        VkDescriptorSet               set        = VK_NULL_HANDLE;
        VkImageView                   boundView  = VK_NULL_HANDLE;
    };

    bool CreateTonemapPipeline() {
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
        if (vkCreateDescriptorSetLayout(dev, &dsl, nullptr, &setLayout_) != VK_SUCCESS) return false;

        // Generous enough for every monitor anyone plugs in, and trivially small.
        VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 16};
        VkDescriptorPoolCreateInfo dpi{};
        dpi.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        dpi.maxSets       = 16;
        dpi.poolSizeCount = 1;
        dpi.pPoolSizes    = &poolSize;
        if (vkCreateDescriptorPool(dev, &dpi, nullptr, &descriptorPool_) != VK_SUCCESS) return false;

        VkPushConstantRange push{};
        push.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        push.size       = sizeof(TonemapPush);

        VkPipelineLayoutCreateInfo pli{};
        pli.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pli.setLayoutCount         = 1;
        pli.pSetLayouts            = &setLayout_;
        pli.pushConstantRangeCount = 1;
        pli.pPushConstantRanges    = &push;
        if (vkCreatePipelineLayout(dev, &pli, nullptr, &pipelineLayout_) != VK_SUCCESS) return false;

        VkShaderModule vert = LoadShader(dev, "fullscreen.vert");
        VkShaderModule frag = LoadShader(dev, "tonemap.frag");
        if (!vert || !frag) {
            if (vert) vkDestroyShaderModule(dev, vert, nullptr);
            if (frag) vkDestroyShaderModule(dev, frag, nullptr);
            return false;
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

        VkPipelineColorBlendAttachmentState blendAttachment{};
        blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                         VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

        VkPipelineColorBlendStateCreateInfo blend{};
        blend.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        blend.attachmentCount = 1;
        blend.pAttachments    = &blendAttachment;

        // Viewport and scissor are dynamic so one pipeline serves monitors of different sizes.
        const VkDynamicState        dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT,
                                                       VK_DYNAMIC_STATE_SCISSOR};
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
        gpi.pColorBlendState    = &blend;
        gpi.pDynamicState       = &dynamic;
        gpi.layout              = pipelineLayout_;
        gpi.renderPass          = passes_.present;
        gpi.subpass             = 0;

        const VkResult res =
            vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpi, nullptr, &pipeline_);

        vkDestroyShaderModule(dev, vert, nullptr);
        vkDestroyShaderModule(dev, frag, nullptr);

        if (res != VK_SUCCESS) {
            app::Log("vulkan: tonemap pipeline failed (%d)", static_cast<int>(res));
            return false;
        }
        return true;
    }

    bool AllocateSet(VkDescriptorSet* out) {
        VkDescriptorSetAllocateInfo ai{};
        ai.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ai.descriptorPool     = descriptorPool_;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts        = &setLayout_;
        return vkAllocateDescriptorSets(ctx_->device(), &ai, out) == VK_SUCCESS;
    }

    // The HDR view changes whenever the swapchain is rebuilt. Rebuild waits for the device to go
    // idle first, so rebinding here cannot race an in-flight frame.
    void RefreshDescriptorIfNeeded(Attached& w) {
        const VkImageView view = w.target->hdrView();
        if (view == w.boundView) return;

        VkDescriptorImageInfo info{};
        info.sampler     = sampler_;
        info.imageView   = view;
        info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkWriteDescriptorSet write{};
        write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet          = w.set;
        write.dstBinding      = 0;
        write.descriptorCount = 1;
        write.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo      = &info;

        vkUpdateDescriptorSets(ctx_->device(), 1, &write, 0, nullptr);
        w.boundView = view;
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

    // M2 draws no geometry. The clear alone exercises the HDR attachment, and animating it
    // slowly makes it obvious at a glance that frames are really being produced rather than one
    // stale image being presented over and over.
    void RecordScene(const WindowTarget::Frame& frame, Attached& w, double elapsed) {
        const float pulse = 0.14f + 0.06f * static_cast<float>(std::sin(elapsed * 0.6));

        VkClearValue clears[2]{};
        clears[0].color        = {{pulse * 0.55f, pulse * 0.62f, pulse, 1.0f}};  // linear
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

        vkCmdBindPipeline(frame.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
        vkCmdBindDescriptorSets(frame.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 0, 1,
                                &w.set, 0, nullptr);

        // Fixed for now. M5 replaces this with the histogram-driven adaptation of spec 8.2,
        // which is what actually sells the detonation.
        TonemapPush push{1.0f, 1.0f};
        vkCmdPushConstants(frame.cmd, pipelineLayout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                           sizeof(push), &push);

        vkCmdDraw(frame.cmd, 3, 1, 0, 0);
        vkCmdEndRenderPass(frame.cmd);
    }

    std::unique_ptr<Context> ctx_;
    RenderPasses             passes_{};
    std::vector<Attached>    windows_;

    VkSampler             sampler_        = VK_NULL_HANDLE;
    VkDescriptorSetLayout setLayout_      = VK_NULL_HANDLE;
    VkDescriptorPool      descriptorPool_ = VK_NULL_HANDLE;
    VkPipelineLayout      pipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline            pipeline_       = VK_NULL_HANDLE;
};

// Validation is expensive and noisy, and a screen saver has no business loading it on a user's
// machine. It comes on only when debug logging is already on.
bool WantValidation() { return app::LogEnabled(); }

}  // namespace

std::unique_ptr<Renderer> CreateVulkanRenderer(const app::Settings&) {
    auto ctx = Context::Create(WantValidation());
    if (!ctx) return nullptr;

    auto renderer = std::make_unique<VulkanRenderer>(std::move(ctx));
    if (!renderer->Init()) {
        app::Log("vulkan: renderer init failed, falling back");
        return nullptr;
    }
    return renderer;
}

}  // namespace render
