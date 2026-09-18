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
#include "render/building_data.h"
#include "render/fragment_data.h"
#include "render/particle_data.h"
#include "render/quality.h"
#include "render/scene_uniforms.h"
#include "render/shadow.h"
#include "render/shaders_embedded.h"
#include "render/vk/buffer.h"
#include "render/vk/context.h"
#include "render/vk/window_target.h"
#include "world/world.h"

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace render {
namespace {

using vk::Buffer;
using vk::Context;
using vk::kFramesInFlight;
using vk::RenderPasses;
using vk::WindowTarget;

// The board's per-frame state (spec 7.6). A push constant rather than a uniform because it is two
// words that change every frame, and because pushing it with the draw is what makes "all four
// faces update in the same frame" true by construction.
struct BoardPush {
    uint32_t litLow  = 0;
    uint32_t litHigh = 0;
    float    rise    = 0.0f;
    float    emissive = 0.0f;
};

// Everything the fragment simulation needs, in one 96-byte push block (spec 7.3 to 7.7). A push
// constant rather than a uniform buffer because it changes every frame, is read by every one of
// 150,000 invocations, and is small enough that the driver keeps it in registers.
struct FragmentPush {
    uint32_t counts[4]{};   // box count, total fragments, unused, unused
    float    blast[4]{};    // xyz impact point, w shell radius
    float    timing[4]{};   // x cycle seconds, y delta, z gather start, w gather end
    float    release[4]{};  // x disperse start, y disperse end, z cloud growth, w city radius
    float    wind[4]{};     // xyz m/s, w gravity
    float    cloud[4]{};    // x stem height, y cap height, z cap radius, w cap tube
};

static_assert(sizeof(FragmentPush) == 96, "FragmentPush must fit the guaranteed push range");

// The missile's model matrix (spec 7.1). The only pipeline here with one: everything else in the
// scene is either world-space geometry or an instance stream carrying its own placement.
struct MissilePush {
    float model[16]{};
    float exhaust[4]{};  // x = plume emissive magnitude, yzw reserved
};

static_assert(sizeof(MissilePush) == 80, "MissilePush must fit the guaranteed push range");

// The fireball's sphere is built from gl_VertexIndex alone, so the draw has to name the vertex
// count the shader's grid implies. These MUST match shaders/fireball.vert.
constexpr uint32_t kFireballRings    = 40;
constexpr uint32_t kFireballSegments = 72;
constexpr uint32_t kFireballVertices = kFireballRings * kFireballSegments * 6;

// Shared by the tonemap and by the two auto-exposure compute passes, because all three want most
// of the same numbers and a second block would be two places to keep the extent in step.
struct TonemapPush {
    float exposure    = 1.0f;  // the base exposure of the time of day (spec 5.4)
    float ditherAmp   = 1.0f;
    float nightShift  = 0.0f;
    float delta       = 0.0f;
    float width       = 0.0f;
    float height      = 0.0f;
    float minExposure = 0.0f;
    float maxExposure = 0.0f;
    float bloom       = 0.0f;  // how much of the bloom chain is added back (spec 8.1)
    float pad0        = 0.0f;
    float pad1        = 0.0f;
    float pad2        = 0.0f;
};

static_assert(sizeof(TonemapPush) == 48, "TonemapPush must fit the guaranteed push range");

// The bloom chain (spec 8.1). Both halves share one block so they can share one layout.
struct BloomPush {
    float destinationWidth  = 0.0f;
    float destinationHeight = 0.0f;
    float threshold         = 0.0f;
    float knee              = 0.0f;
    float firstLevel        = 0.0f;
    float intensity         = 0.0f;
    float pad0              = 0.0f;
    float pad1              = 0.0f;
};

static_assert(sizeof(BloomPush) == 32, "BloomPush must fit the guaranteed push range");

// The particle systems (spec 8.3), in one 128-byte push block shared by the three sort passes and
// both draws. 128 is the guaranteed push range and this is exactly at it, which is why the wind
// here is two floats rather than three and why several fields carry an unrelated scalar in a spare
// component. Going one vec4 over would mean a uniform buffer, and a uniform buffer written once a
// frame and read by two windows with independent frame slots is a hazard this does not need.
struct ParticlePush {
    uint32_t caps[4]{};    // slot counts of all four systems
    float    tail[4]{};    // x spare, y gravity, zw horizontal wind
    float    timing[4]{};  // x cycle time, yz spare, w city radius
    float    mStart[4]{};  // xyz missile entry point, w missile phase start
    float    mDir[4]{};    // xyz missile direction, w missile phase end
    float    blast[4]{};   // xyz impact point, w the shell's final reach
    float    phases[4]{};  // x blast start, y blast duration, z gather start, w disperse end
    float    cloud[4]{};   // x stem height, y cap radius, z missile length, w sort range
};

static_assert(sizeof(ParticlePush) == 128, "ParticlePush must fit the guaranteed push range");

// The slot budgets and the two padding words that go with the bins live in
// render/particle_data.h, so that the emission schedule they belong to can be tested.
constexpr size_t kParticleBinBytes = (2 * kSortBuckets + 4) * sizeof(uint32_t);

// Spec 5.1 puts the countdown segments at 30-60 linear and the fireball in the thousands, all
// against a scene whose middle grey is 1. The threshold sits above the brightest ordinary surface
// and below the dimmest thing that is meant to glow — at 1.4 the twilight horizon band itself
// crossed it and the whole sky glowed.
constexpr float kBloomThreshold = 3.0f;
constexpr float kBloomKnee      = 1.5f;
constexpr float kBloomIntensity = 0.75f;  // per upsample step

// How much of the finished chain goes back into the frame. Small, and it has to be: level 0 of the
// chain is a half-resolution copy of everything above the threshold, so adding it at anything near
// 1 doubles every bright surface rather than haloing it. At 0.9 the countdown board stopped being
// eight digits and became one white rectangle.
constexpr float kBloomMix = 0.08f;

// What the descriptor pool is sized for. The window target stops the chain when a level would be
// too small to filter, so a given monitor may use fewer.
constexpr uint32_t kMaxBloomLevels = 6;

// Mirrors the Exposure block in shaders/exposure.glsl.
constexpr uint32_t kExposureBins  = 256;
constexpr size_t   kExposureBytes = 4 * sizeof(float) + kExposureBins * sizeof(uint32_t);

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
    // Which vertex input the pipeline expects. Fullscreen passes build their own vertices from
    // gl_VertexIndex and want none; the ground reads world::Vertex; the city reads one unit cube
    // plus a per-instance stream.
    enum class Vertices { None, World, Building, Board };
    Vertices         vertices     = Vertices::None;
    bool             backfaceCull = false;
    // A second set, for the one pipeline that needs the scene uniforms and the fragment buffers
    // at the same time. Nothing else here has more than one.
    uint32_t         setCount     = 1;
    // Additive blending, for the emissive passes that add light to what is already there rather
    // than replacing it: the flash, the fireball, the ground ring.
    bool             additive     = false;
    // Ordinary source-alpha blending, for the one pass with genuinely translucent geometry: the
    // dust half of the particle systems (spec 8.3).
    bool             alphaBlend   = false;
    // Slope-scaled depth bias, for the shadow casters. A surface nearly edge-on to the light
    // spans many times its own thickness in one texel, and no constant bias covers both that and
    // a surface facing the light squarely (spec 8.2).
    bool             depthBias    = false;
    // A triangle strip rather than a list, for the particle billboards: each instance is its own
    // strip, and a six-vertex strip draws a hexagon where a six-vertex list draws only a quad.
    bool             strip        = false;
};

VkPipeline CreateComputePipeline(VkDevice dev, const char* name, VkPipelineLayout layout) {
    VkShaderModule module = LoadShader(dev, name);
    if (!module) return VK_NULL_HANDLE;

    VkComputePipelineCreateInfo ci{};
    ci.sType        = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    ci.stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    ci.stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
    ci.stage.module = module;
    ci.stage.pName  = "main";
    ci.layout       = layout;

    VkPipeline pipeline = VK_NULL_HANDLE;
    if (vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &ci, nullptr, &pipeline) != VK_SUCCESS) {
        app::Log("vulkan: compute pipeline '%s' failed", name);
        pipeline = VK_NULL_HANDLE;
    }
    vkDestroyShaderModule(dev, module, nullptr);
    return pipeline;
}

VkPipeline CreateGraphicsPipeline(VkDevice dev, const PipelineDesc& desc) {
    // No fragment shader at all is the shadow pass: it writes depth and nothing else, so there is
    // no second stage to compile and no colour attachment for a blend state to describe.
    const bool depthOnly = desc.frag == nullptr;

    VkShaderModule vert = LoadShader(dev, desc.vert);
    VkShaderModule frag = depthOnly ? VK_NULL_HANDLE : LoadShader(dev, desc.frag);
    if (!vert || (!depthOnly && !frag)) {
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

    // Matches world::Vertex in src/world/mesh.h.
    const VkVertexInputBindingDescription worldBinding{0, sizeof(world::Vertex),
                                                       VK_VERTEX_INPUT_RATE_VERTEX};

    const VkVertexInputAttributeDescription worldAttributes[] = {
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(world::Vertex, position)},
        {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(world::Vertex, normal)},
        {2, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(world::Vertex, albedo)},
        {3, 0, VK_FORMAT_R32_SFLOAT, offsetof(world::Vertex, rockiness)},
    };

    // Matches render::BoxVertex and render::BuildingInstance. Two bindings: binding 0 steps per
    // vertex and holds the one cube every building is drawn from, binding 1 steps per instance.
    const VkVertexInputBindingDescription buildingBindings[] = {
        {0, sizeof(BoxVertex), VK_VERTEX_INPUT_RATE_VERTEX},
        {1, sizeof(BuildingInstance), VK_VERTEX_INPUT_RATE_INSTANCE},
    };

    const VkVertexInputAttributeDescription buildingAttributes[] = {
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(BoxVertex, position)},
        {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(BoxVertex, normal)},
        {2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(BoxVertex, uv)},
        {3, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(BuildingInstance, centerRotation)},
        {4, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(BuildingInstance, extentGrowth)},
        {5, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(BuildingInstance, bodyColor)},
        {6, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(BuildingInstance, windowColor)},
    };

    // Matches render::BoardInstance. Same cube, a smaller instance.
    const VkVertexInputBindingDescription boardBindings[] = {
        {0, sizeof(BoxVertex), VK_VERTEX_INPUT_RATE_VERTEX},
        {1, sizeof(BoardInstance), VK_VERTEX_INPUT_RATE_INSTANCE},
    };

    const VkVertexInputAttributeDescription boardAttributes[] = {
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(BoxVertex, position)},
        {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(BoxVertex, normal)},
        {2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(BoxVertex, uv)},
        {3, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(BoardInstance, baseYaw)},
        {4, 1, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(BoardInstance, sizeId)},
    };

    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    if (desc.vertices == PipelineDesc::Vertices::World) {
        vertexInput.vertexBindingDescriptionCount   = 1;
        vertexInput.pVertexBindingDescriptions      = &worldBinding;
        vertexInput.vertexAttributeDescriptionCount = 4;
        vertexInput.pVertexAttributeDescriptions    = worldAttributes;
    } else if (desc.vertices == PipelineDesc::Vertices::Building) {
        vertexInput.vertexBindingDescriptionCount   = 2;
        vertexInput.pVertexBindingDescriptions      = buildingBindings;
        vertexInput.vertexAttributeDescriptionCount = 7;
        vertexInput.pVertexAttributeDescriptions    = buildingAttributes;
    } else if (desc.vertices == PipelineDesc::Vertices::Board) {
        vertexInput.vertexBindingDescriptionCount   = 2;
        vertexInput.pVertexBindingDescriptions      = boardBindings;
        vertexInput.vertexAttributeDescriptionCount = 5;
        vertexInput.pVertexAttributeDescriptions    = boardAttributes;
    }

    VkPipelineInputAssemblyStateCreateInfo assembly{};
    assembly.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    assembly.topology =
        desc.strip ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewport{};
    viewport.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport.viewportCount = 1;
    viewport.scissorCount  = 1;

    VkPipelineRasterizationStateCreateInfo raster{};
    raster.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode    = desc.backfaceCull ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE;
    raster.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth   = 1.0f;
    if (desc.depthBias) {
        raster.depthBiasEnable         = VK_TRUE;
        raster.depthBiasConstantFactor = 2.0f;
        raster.depthBiasSlopeFactor    = 3.0f;
    }

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
    if (desc.additive) {
        // Source alpha as the coverage term, so one pipeline serves both a hard add (alpha 1) and
        // a soft-edged one (the fireball's rim, the ring's falloff) without a second blend state.
        blendAttachment.blendEnable         = VK_TRUE;
        blendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        blendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
        blendAttachment.colorBlendOp        = VK_BLEND_OP_ADD;
        blendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        blendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        blendAttachment.alphaBlendOp        = VK_BLEND_OP_ADD;
    } else if (desc.alphaBlend) {
        blendAttachment.blendEnable         = VK_TRUE;
        blendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        blendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blendAttachment.colorBlendOp        = VK_BLEND_OP_ADD;
        blendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        blendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        blendAttachment.alphaBlendOp        = VK_BLEND_OP_ADD;
    }

    VkPipelineColorBlendStateCreateInfo blend{};
    blend.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount = depthOnly ? 0 : 1;
    blend.pAttachments    = depthOnly ? nullptr : &blendAttachment;

    // Viewport and scissor are dynamic so one pipeline serves monitors of different sizes.
    const VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};

    VkPipelineDynamicStateCreateInfo dynamic{};
    dynamic.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates    = dynamicStates;

    VkGraphicsPipelineCreateInfo gpi{};
    gpi.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    gpi.stageCount          = depthOnly ? 1u : 2u;
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
    if (frag) vkDestroyShaderModule(dev, frag, nullptr);

    if (res != VK_SUCCESS) {
        app::Log("vulkan: pipeline %s/%s failed (%d)", desc.vert,
                 desc.frag ? desc.frag : "(depth only)", static_cast<int>(res));
        return VK_NULL_HANDLE;
    }
    return pipeline;
}

class VulkanRenderer final : public Renderer {
public:
    VulkanRenderer(std::unique_ptr<Context> ctx, const app::Settings& settings,
                   const world::World& world)
        : ctx_(std::move(ctx)),
          settings_(settings),
          world_(world),
          quality_(QualityController::FromSettings(settings)) {}

    ~VulkanRenderer() override {
        if (!ctx_) return;
        ctx_->WaitIdle();

        vk::DestroyBuffer(*ctx_, &captureBuffer_);
        for (auto& w : windows_) DestroyAttached(w);
        windows_.clear();

        VkDevice dev = ctx_->device();
        vk::DestroyBuffer(*ctx_, &terrainVertices_);
        vk::DestroyBuffer(*ctx_, &terrainIndices_);
        vk::DestroyBuffer(*ctx_, &horizonVertices_);
        vk::DestroyBuffer(*ctx_, &horizonIndices_);
        vk::DestroyBuffer(*ctx_, &missileVertices_);
        vk::DestroyBuffer(*ctx_, &missileIndices_);
        vk::DestroyBuffer(*ctx_, &boxVertices_);
        vk::DestroyBuffer(*ctx_, &boxIndices_);
        vk::DestroyBuffer(*ctx_, &buildingInstances_);
        vk::DestroyBuffer(*ctx_, &boardInstances_);
        vk::DestroyBuffer(*ctx_, &shatterBoxes_);
        vk::DestroyBuffer(*ctx_, &fragmentRest_);
        vk::DestroyBuffer(*ctx_, &fragmentState_);

        if (particleAddPipeline_) vkDestroyPipeline(dev, particleAddPipeline_, nullptr);
        if (particleAlphaPipeline_) vkDestroyPipeline(dev, particleAlphaPipeline_, nullptr);
        if (particleScatterPipeline_) vkDestroyPipeline(dev, particleScatterPipeline_, nullptr);
        if (particlePrefixPipeline_) vkDestroyPipeline(dev, particlePrefixPipeline_, nullptr);
        if (particleCountPipeline_) vkDestroyPipeline(dev, particleCountPipeline_, nullptr);
        if (particleDrawLayout_) vkDestroyPipelineLayout(dev, particleDrawLayout_, nullptr);
        if (particleComputeLayout_) vkDestroyPipelineLayout(dev, particleComputeLayout_, nullptr);
        if (particleSetLayout_) vkDestroyDescriptorSetLayout(dev, particleSetLayout_, nullptr);

        if (fragmentPipeline_) vkDestroyPipeline(dev, fragmentPipeline_, nullptr);
        if (fragmentSimPipeline_) vkDestroyPipeline(dev, fragmentSimPipeline_, nullptr);
        if (fragmentInitPipeline_) vkDestroyPipeline(dev, fragmentInitPipeline_, nullptr);
        if (fragmentDrawLayout_) vkDestroyPipelineLayout(dev, fragmentDrawLayout_, nullptr);
        if (fragmentComputeLayout_) vkDestroyPipelineLayout(dev, fragmentComputeLayout_, nullptr);
        if (fragmentSetLayout_) vkDestroyDescriptorSetLayout(dev, fragmentSetLayout_, nullptr);

        if (flashPipeline_) vkDestroyPipeline(dev, flashPipeline_, nullptr);
        if (fireballPipeline_) vkDestroyPipeline(dev, fireballPipeline_, nullptr);
        if (missilePipeline_) vkDestroyPipeline(dev, missilePipeline_, nullptr);
        if (missilePipelineLayout_) vkDestroyPipelineLayout(dev, missilePipelineLayout_, nullptr);

        if (boardPipeline_) vkDestroyPipeline(dev, boardPipeline_, nullptr);
        if (boardPipelineLayout_) vkDestroyPipelineLayout(dev, boardPipelineLayout_, nullptr);
        if (buildingPipeline_) vkDestroyPipeline(dev, buildingPipeline_, nullptr);
        if (groundPipeline_) vkDestroyPipeline(dev, groundPipeline_, nullptr);
        if (skyPipeline_) vkDestroyPipeline(dev, skyPipeline_, nullptr);
        if (scenePipelineLayout_) vkDestroyPipelineLayout(dev, scenePipelineLayout_, nullptr);
        if (sceneSetLayout_) vkDestroyDescriptorSetLayout(dev, sceneSetLayout_, nullptr);

        if (bloomUpPipeline_) vkDestroyPipeline(dev, bloomUpPipeline_, nullptr);
        if (bloomDownPipeline_) vkDestroyPipeline(dev, bloomDownPipeline_, nullptr);
        if (bloomPipelineLayout_) vkDestroyPipelineLayout(dev, bloomPipelineLayout_, nullptr);
        if (bloomSetLayout_) vkDestroyDescriptorSetLayout(dev, bloomSetLayout_, nullptr);

        if (adaptPipeline_) vkDestroyPipeline(dev, adaptPipeline_, nullptr);
        if (histogramPipeline_) vkDestroyPipeline(dev, histogramPipeline_, nullptr);
        if (tonemapPipeline_) vkDestroyPipeline(dev, tonemapPipeline_, nullptr);
        if (tonemapPipelineLayout_) vkDestroyPipelineLayout(dev, tonemapPipelineLayout_, nullptr);
        if (tonemapSetLayout_) vkDestroyDescriptorSetLayout(dev, tonemapSetLayout_, nullptr);
        if (sampler_) vkDestroySampler(dev, sampler_, nullptr);

        DestroyShadowMap();
        if (shadowSampler_) vkDestroySampler(dev, shadowSampler_, nullptr);
        if (shadowBuildingPipeline_) vkDestroyPipeline(dev, shadowBuildingPipeline_, nullptr);
        if (shadowFragmentPipeline_) vkDestroyPipeline(dev, shadowFragmentPipeline_, nullptr);

        if (descriptorPool_) vkDestroyDescriptorPool(dev, descriptorPool_, nullptr);
        vk::DestroyRenderPasses(*ctx_, &passes_);
    }

    bool Init() {
        if (!vk::CreateRenderPasses(*ctx_, &passes_)) return false;
        if (!CreateDescriptorPool()) return false;
        if (!CreateSceneLayout()) return false;
        if (!CreateBoardLayout()) return false;
        if (!CreateMissileLayout()) return false;
        if (!CreateBloomLayout()) return false;
        if (!CreateFragmentLayout()) return false;
        if (!CreateParticleLayout()) return false;
        if (!CreateTonemapLayout()) return false;

        VkDevice dev = ctx_->device();

        // The shadow map's sampler compares rather than returns: the hardware does the depth test
        // and filters the *result* over its 2x2 neighbourhood, which is what makes the receiver's
        // 3x3 tap pattern cover four times the area it looks like it does. Clamped to the border
        // and to a lit border, so a lookup that falls off the map is lit rather than dark -- the
        // alternative smears whatever was on the edge texel across the desert (spec 8.2).
        {
            VkSamplerCreateInfo sci{};
            sci.sType            = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
            sci.magFilter        = VK_FILTER_LINEAR;
            sci.minFilter        = VK_FILTER_LINEAR;
            sci.mipmapMode       = VK_SAMPLER_MIPMAP_MODE_NEAREST;
            sci.addressModeU     = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
            sci.addressModeV     = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
            sci.addressModeW     = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
            sci.borderColor      = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
            sci.compareEnable    = VK_TRUE;
            sci.compareOp        = VK_COMPARE_OP_LESS_OR_EQUAL;
            if (vkCreateSampler(dev, &sci, nullptr, &shadowSampler_) != VK_SUCCESS) {
                app::Log("vulkan: shadow sampler failed");
                return false;
            }
        }

        shadowFormat_ = vk::ChooseDepthFormat(*ctx_);
        if (!CreateShadowMap(ShadowMapSize(QualityLevel()))) return false;

        // The two casters (spec 8.2). Depth only -- no fragment shader, so no colour attachment
        // and nothing to blend -- and back-face culled for the city exactly as the scene pass
        // culls it, so a box contributes the same silhouette to both.
        shadowBuildingPipeline_ = CreateGraphicsPipeline(
            dev, {"shadow_building.vert", nullptr, passes_.shadow, scenePipelineLayout_, true,
                  true, PipelineDesc::Vertices::Building, true, 1, false, false, true});
        if (!shadowBuildingPipeline_) return false;

        // The fragments are single triangles with no inside, so there is no back face to cull and
        // the slope bias has to carry the whole job on its own.
        shadowFragmentPipeline_ = CreateGraphicsPipeline(
            dev, {"shadow_fragment.vert", nullptr, passes_.shadow, fragmentDrawLayout_, true, true,
                  PipelineDesc::Vertices::None, false, 2, false, false, true});
        if (!shadowFragmentPipeline_) return false;

        // The sky tests depth but does not write it. It sits on the far plane and is drawn after
        // everything opaque, so it shades only the pixels nothing covered. Drawn first, it shaded
        // the whole frame and the terrain and city then overwrote more than half of it.
        skyPipeline_ = CreateGraphicsPipeline(
            dev, {"sky.vert", "sky.frag", passes_.hdr, scenePipelineLayout_, true, false,
                  PipelineDesc::Vertices::None, false});
        if (!skyPipeline_) return false;

        // Terrain and the horizon range: depth tested and written, and back-face culled, which
        // halves the triangles rasterised for the mountains.
        groundPipeline_ = CreateGraphicsPipeline(
            dev, {"ground.vert", "ground.frag", passes_.hdr, scenePipelineLayout_, true, true,
                  PipelineDesc::Vertices::World, true});
        if (!groundPipeline_) return false;

        // The city. Same depth and culling as the ground; a different vertex input, because these
        // are instances of one cube rather than a mesh.
        buildingPipeline_ = CreateGraphicsPipeline(
            dev, {"building.vert", "building.frag", passes_.hdr, scenePipelineLayout_, true, true,
                  PipelineDesc::Vertices::Building, true});
        if (!buildingPipeline_) return false;

        // The board. Its own layout, because it is the only scene pipeline with push constants.
        boardPipeline_ = CreateGraphicsPipeline(
            dev, {"board.vert", "board.frag", passes_.hdr, boardPipelineLayout_, true, true,
                  PipelineDesc::Vertices::Board, true});
        if (!boardPipeline_) return false;

        // The missile (spec 7.1). Its own layout, for the model matrix, and not back-face culled:
        // it is four hundred triangles, so culling saves nothing measurable, and getting the
        // winding of a hand-built body of revolution backwards would make the whole missile
        // invisible rather than wrong — a failure with no symptom to debug from.
        missilePipeline_ = CreateGraphicsPipeline(
            dev, {"missile.vert", "missile.frag", passes_.hdr, missilePipelineLayout_, true, true,
                  PipelineDesc::Vertices::World, false});
        if (!missilePipeline_) return false;

        // The fragments. No vertex buffer: three vertices a fragment, everything else read out
        // of the storage buffers. Double-sided, because spec 7.3 says a tumbling chip must show
        // both of its faces.
        fragmentPipeline_ = CreateGraphicsPipeline(
            dev, {"fragment.vert", "fragment.frag", passes_.hdr, fragmentDrawLayout_, true, true,
                  PipelineDesc::Vertices::None, false, 2});
        if (!fragmentPipeline_) return false;

        // The fireball (spec 7.2): additive, depth tested against the scene so buildings in front
        // of it occlude it, and not depth written, because it is light rather than surface. Not
        // culled — it decides its own near hemisphere in the fragment shader, which is the one
        // way to be sure of it regardless of the viewport's handedness.
        fireballPipeline_ = CreateGraphicsPipeline(
            dev, {"fireball.vert", "fireball.frag", passes_.hdr, scenePipelineLayout_, true, false,
                  PipelineDesc::Vertices::None, false, 1, true});
        if (!fireballPipeline_) return false;

        // The flash (spec 7.2). Fullscreen, additive, no depth at all: it is in front of
        // everything including the fireball, which is the point of a flash.
        flashPipeline_ = CreateGraphicsPipeline(
            dev, {"fullscreen.vert", "flash.frag", passes_.hdr, scenePipelineLayout_, false, false,
                  PipelineDesc::Vertices::None, false, 1, true});
        if (!flashPipeline_) return false;

        // The particles (spec 8.3). Two pipelines over one shader pair: dust blends, emitters
        // add, and which one a particle belongs to is decided by the sort rather than by a branch
        // in the fragment stage, because blend mode is pipeline state and cannot vary per draw.
        // Neither writes depth — a translucent sprite that occludes what is behind it is not
        // translucent — and neither is culled, because a billboard has no back.
        particleAlphaPipeline_ = CreateGraphicsPipeline(
            dev, {"particle.vert", "particle.frag", passes_.hdr, particleDrawLayout_, true, false,
                  PipelineDesc::Vertices::None, false, 2, false, true, false, true});
        particleAddPipeline_ = CreateGraphicsPipeline(
            dev, {"particle.vert", "particle.frag", passes_.hdr, particleDrawLayout_, true, false,
                  PipelineDesc::Vertices::None, false, 2, true, false, false, true});
        if (!particleAlphaPipeline_ || !particleAddPipeline_) return false;

        particleCountPipeline_ =
            CreateComputePipeline(dev, "particle_count.comp", particleComputeLayout_);
        particlePrefixPipeline_ =
            CreateComputePipeline(dev, "particle_prefix.comp", particleComputeLayout_);
        particleScatterPipeline_ =
            CreateComputePipeline(dev, "particle_scatter.comp", particleComputeLayout_);
        if (!particleCountPipeline_ || !particlePrefixPipeline_ || !particleScatterPipeline_) {
            return false;
        }

        fragmentInitPipeline_ = CreateComputePipeline(dev, "fragment_init.comp",
                                                      fragmentComputeLayout_);
        fragmentSimPipeline_  = CreateComputePipeline(dev, "fragment_sim.comp",
                                                      fragmentComputeLayout_);
        if (!fragmentInitPipeline_ || !fragmentSimPipeline_) return false;

        tonemapPipeline_ = CreateGraphicsPipeline(
            dev, {"fullscreen.vert", "tonemap.frag", passes_.present, tonemapPipelineLayout_, false,
                  false, PipelineDesc::Vertices::None, false});
        if (!tonemapPipeline_) return false;

        // Auto-exposure (spec 8.2). Both passes share the tonemap's set and push block.
        histogramPipeline_ =
            CreateComputePipeline(dev, "exposure_histogram.comp", tonemapPipelineLayout_);
        adaptPipeline_ = CreateComputePipeline(dev, "exposure_adapt.comp", tonemapPipelineLayout_);
        if (!histogramPipeline_ || !adaptPipeline_) return false;

        bloomDownPipeline_ = CreateComputePipeline(dev, "bloom_down.comp", bloomPipelineLayout_);
        bloomUpPipeline_   = CreateComputePipeline(dev, "bloom_up.comp", bloomPipelineLayout_);
        if (!bloomDownPipeline_ || !bloomUpPipeline_) return false;

        // The first cycle's world already exists by now; every later one goes through ResetCycle,
        // which fits the map again. Without this the opening cycle would light a map fitted to the
        // default one-metre box, and nothing in the world would be inside it.
        FitShadowMap();

        return UploadStaticGeometry();
    }

    bool AttachWindow(HWND hwnd, int width, int height) override {
        if (!ctx_ || ctx_->deviceLost()) {
            app::Log("vulkan: attach refused, no context or the device is lost");
            return false;
        }

        Attached a;
        a.target = WindowTarget::Create(*ctx_, passes_, hwnd, static_cast<uint32_t>(width),
                                        static_cast<uint32_t>(height));
        if (!a.target) {
            app::Log("vulkan: attach failed, no swapchain for %dx%d", width, height);
            return false;
        }

        // One uniform buffer per frame in flight. Sharing a single buffer would mean writing the
        // copy the GPU is still reading, which shows up as a frame of the previous camera - or,
        // worse, half of each.
        for (uint32_t i = 0; i < kFramesInFlight; ++i) {
            if (!vk::CreateBuffer(*ctx_, sizeof(SceneUniforms), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                  vk::BufferUse::HostWritable, &a.sceneUbo[i]) ||
                !AllocateSet(sceneSetLayout_, &a.sceneSet[i])) {
                app::Log("vulkan: attach failed on scene uniform %u", i);
                DestroyAttached(a);
                return false;
            }
            BindUniformBuffer(a.sceneSet[i], a.sceneUbo[i]);
            BindShadowMap(a.sceneSet[i]);
        }

        // Host visible, and zeroed here rather than by a staging copy. The shader reads this buffer
        // before it writes it — `initialised` at 0 is what tells the first adapt to jump straight
        // to the measured value instead of easing up from black — and being able to read it back
        // is what makes the adaptation debuggable at all: without it, "the frame is too bright" has
        // the exposure, the histogram, the bloom and four surface shaders as suspects.
        //
        // The reference machine is an iGPU, where device-local memory is host-visible anyway, so
        // this costs nothing on it. On a discrete card it would put a per-frame compute write over
        // PCIe; if that ever matters, the readback is the part to drop, not the buffer.
        if (!vk::CreateBuffer(*ctx_, kExposureBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                              vk::BufferUse::HostWritable, &a.exposure) ||
            !AllocateSet(tonemapSetLayout_, &a.tonemapSet)) {
            app::Log("vulkan: attach failed on the exposure buffer");
            DestroyAttached(a);
            return false;
        }
        if (a.exposure.mapped) std::memset(a.exposure.mapped, 0, kExposureBytes);
        BindExposureBuffer(a.tonemapSet, a.exposure);

        // The sort's output, sized for quality level 0 so that changing quality is a push
        // constant and never a reallocation. Two ranges of `total`: the alpha-blended particles
        // ordered far to near, then the additive ones packed after them.
        const size_t sortBytes = size_t(ParticleTotal(0)) * 2 * sizeof(uint32_t);
        if (!vk::CreateBuffer(*ctx_, sortBytes,
                              VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                              vk::BufferUse::GpuOnly, &a.particleSort) ||
            !vk::CreateBuffer(*ctx_, kParticleBinBytes,
                              VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                              vk::BufferUse::GpuOnly, &a.particleBins) ||
            !AllocateSet(particleSetLayout_, &a.particleSet)) {
            app::Log("vulkan: attach failed on the particle sort buffers (%zu bytes)", sortBytes);
            DestroyAttached(a);
            return false;
        }
        BindParticleBuffers(a);

        const uint32_t levels = a.target->bloomLevels();
        a.bloomDown.assign(levels, VK_NULL_HANDLE);
        a.bloomUp.assign(levels, VK_NULL_HANDLE);
        for (uint32_t i = 0; i < levels; ++i) {
            if (!AllocateSet(bloomSetLayout_, &a.bloomDown[i])) {
                app::Log("vulkan: attach failed on bloom level %u of %u (down)", i, levels);
                DestroyAttached(a);
                return false;
            }
            // Only the first step writes the histogram, but the shader declares the buffer at
            // every level, so every downsample set must name it.
            BindExposureBuffer(a.bloomDown[i], a.exposure, 2);
            if (i > 0 && !AllocateSet(bloomSetLayout_, &a.bloomUp[i])) {
                app::Log("vulkan: attach failed on bloom level %u of %u (up)", i, levels);
                DestroyAttached(a);
                return false;
            }
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

    void RenderFrame(double elapsed, double delta) override {
        if (!ctx_ || ctx_->deviceLost()) return;

        // Spec 4.1: one cycle runs, then repeats with a new seed. The host's clock never resets,
        // so the cycle's own time is measured from where this cycle began.
        if (elapsed - cycleBase_ >= static_cast<double>(world_.cycleSeconds)) {
            cycleBase_ = elapsed;
            // The one moment the fragment budget is allowed to move: everything it touches is
            // being rebuilt anyway.
            fragmentQuality_ = QualityLevel();
            ResetCycle();
        }

        const float t = static_cast<float>(elapsed - cycleBase_);

        // Spec 11.2's controller. Fed the raw delta rather than the clamped one below: what it is
        // measuring is how long the frame actually took, and a clamp would hide exactly the
        // frames it exists to notice.
        {
            const int before = quality_.level();
            quality_.Update(static_cast<float>(delta));
            if (quality_.level() != before) {
                app::Log("quality: %s to level %d (%.1f fps average)",
                         quality_.level() > before ? "down" : "up", quality_.level(),
                         quality_.average() > 0.0f ? 1.0 / double(quality_.average()) : 0.0);
            }
        }

        // The particle budget follows the level immediately; the fragment budget waits for the
        // reset above, which is why this is read every frame and FragmentQuality is not.
        particleTotal_ = ParticleTotal(QualityLevel());

        // Spec 4.2 clamps the delta before it reaches the simulation; the host already did, and
        // this floor keeps a 1000 fps frame from stepping the springs by nothing at all.
        const float dt = static_cast<float>(delta) < 1.0f / 480.0f ? 1.0f / 480.0f
                                                                   : static_cast<float>(delta);

        // The fragment simulation belongs to the world, not to a window: it is stepped once, in
        // whichever command buffer opens first, and every monitor then draws the same state.
        bool simulated = false;
        bool shadowed  = false;

        for (auto& w : windows_) {
            WindowTarget::Frame frame = w.target->Begin(*ctx_, passes_);
            if (!frame.valid) continue;

            frameTime_ = t;

            if (!simulated) {
                RecordFragmentSim(frame.cmd, t, dt);
                simulated = true;
            }
            UpdateSceneUniforms(w, frame.frameSlot, t);
            RefreshTonemapBinding(w);

            // After the uniforms, because the pass reads the light matrix out of them, and once
            // for all windows, because the map does not depend on a camera.
            if (!shadowed) {
                RecordShadow(frame, w);
                shadowed = true;
            }

            // Per window, not per frame: the sort is by distance from a camera, and two monitors
            // do not share one (spec 8.3).
            RecordParticleSort(frame, w);

            RecordScene(frame, w);
            const bool binned = RecordBloom(frame, w);
            RecordExposure(frame, w, dt, binned);
            RecordTonemap(frame, w, dt);

            const bool capturing = capture_.enabled && frameCounter_ == capture_.atFrame;
            if (capturing) {
                if (w.exposure.mapped) {
                    const float* state = static_cast<const float*>(w.exposure.mapped);
                    app::Log("capture: exposure %.4f (base %.4f), measured luminance %.5f, "
                             "initialised %.0f",
                             state[0], world_.sky.baseExposure, state[2], state[1]);
                }
                if (world_.MissileVisible(frameTime_)) {
                    const core::Vec3 at = world_.detonation.MissileAt(world_.timeline, frameTime_);
                    app::Log("capture: missile at %.0f %.0f %.0f, %u indices", at.x, at.y, at.z,
                             missileIndexCount_);
                }
                app::Log("capture: frame %d t=%.3fs phase=%d rise=%.2f boardSeconds=%d mask=%llx",
                         frameCounter_, t, static_cast<int>(world_.timeline.Primary(t)),
                         world_.BoardRise(t), world_.timeline.BoardSeconds(t),
                         static_cast<unsigned long long>(
                             world::BoardMask(world_.timeline.BoardSeconds(t))));
                RecordCaptureCopy(frame, w);
            }

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

        // Auto-exposure state (spec 8.2). Per window, because two monitors showing the same world
        // from different aspect ratios do not see the same average luminance, and because the
        // adaptation has to carry across frames.
        Buffer exposure{};

        // The particle sort (spec 8.3). Per window for the same reason the exposure is: back to
        // front is a property of a camera, and two monitors do not share one.
        Buffer          particleSort{};
        Buffer          particleBins{};
        VkDescriptorSet particleSet = VK_NULL_HANDLE;

        // Bloom, one descriptor set per step. `bloomDown[i]` reads level i-1 (or the HDR image at
        // i == 0) and writes level i; `bloomUp[i]` reads level i and adds into level i-1.
        std::vector<VkDescriptorSet> bloomDown;
        std::vector<VkDescriptorSet> bloomUp;
    };

    void DestroyAttached(Attached& a) {
        for (uint32_t i = 0; i < kFramesInFlight; ++i) vk::DestroyBuffer(*ctx_, &a.sceneUbo[i]);
        vk::DestroyBuffer(*ctx_, &a.exposure);
        vk::DestroyBuffer(*ctx_, &a.particleSort);
        vk::DestroyBuffer(*ctx_, &a.particleBins);
        a.target.reset();
        // Descriptor sets are freed with the pool, which outlives every window.
    }

    bool CreateDescriptorPool() {
        // Sized for eight monitors: more than anyone attaches, and still trivially small.
        // Eleven bloom steps a window at six levels, each reading one image and writing another;
        // the downsample steps also carry the window's exposure buffer.
        const uint32_t bloomSets = 8 * (2 * kMaxBloomLevels);

        const VkDescriptorPoolSize sizes[] = {
            {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 8 * kFramesInFlight},
            // Two per window for the tonemap (the HDR image and the finished bloom), one per
            // bloom step, and the shadow map in every scene set.
            {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
             8 * 2 + bloomSets + 8 * kFramesInFlight},
            // The fragment buffers, one set for the whole renderer, plus one auto-exposure buffer
            // and two particle buffers per window: the first are the world's, the rest are a
            // window's, because both the exposure and the sort order belong to a view. Plus the
            // exposure buffer again in each of a window's downsample steps.
            {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4 + 8 + 8 * 2 + 8 * kMaxBloomLevels},
            {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, bloomSets},
        };

        VkDescriptorPoolCreateInfo dpi{};
        dpi.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        dpi.maxSets       = 8 * (kFramesInFlight + 2) + 1 + bloomSets;
        dpi.poolSizeCount = 4;
        dpi.pPoolSizes    = sizes;
        return vkCreateDescriptorPool(ctx_->device(), &dpi, nullptr, &descriptorPool_) == VK_SUCCESS;
    }

    bool CreateSceneLayout() {
        VkDescriptorSetLayoutBinding bindings[2]{};
        bindings[0].binding         = 0;
        bindings[0].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        bindings[0].descriptorCount = 1;
        // Visible to all three stages: the vertex stage needs viewProj, the fragment stage needs
        // the lighting, and the particle sort needs the camera position, because which particle is
        // behind which is a question about a view rather than about the world (spec 8.3).
        bindings[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT |
                                 VK_SHADER_STAGE_COMPUTE_BIT;

        // The key light's depth map (spec 8.2). In the scene set rather than threaded through each
        // surface pipeline for the same reason the fireball is: four pipelines read it and they
        // all already have this set bound. Pipelines whose shaders never declare it are unharmed
        // by its being here.
        bindings[1].binding         = 1;
        bindings[1].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[1].descriptorCount = 1;
        bindings[1].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;

        VkDescriptorSetLayoutCreateInfo dsl{};
        dsl.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        dsl.bindingCount = 2;
        dsl.pBindings    = bindings;
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

        // One set serves three pipelines: the two exposure compute passes and the tonemap. They
        // want the same two things — the frame that was just rendered and the exposure state — so
        // a second layout would be a second copy of the same pair.
        VkDescriptorSetLayoutBinding bindings[3]{};
        bindings[0].binding         = 0;
        bindings[0].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[0].descriptorCount = 1;
        bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;

        bindings[1].binding         = 1;
        bindings[1].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[1].descriptorCount = 1;
        bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;

        // The finished bloom chain (spec 8.1), read only by the tonemap.
        bindings[2].binding         = 2;
        bindings[2].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[2].descriptorCount = 1;
        bindings[2].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;

        VkDescriptorSetLayoutCreateInfo dsl{};
        dsl.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        dsl.bindingCount = 3;
        dsl.pBindings    = bindings;
        if (vkCreateDescriptorSetLayout(dev, &dsl, nullptr, &tonemapSetLayout_) != VK_SUCCESS) {
            return false;
        }

        VkPushConstantRange push{};
        push.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;
        push.size       = sizeof(TonemapPush);

        // Two sets: its own image and exposure buffer, then the scene, which the blast refraction
        // of spec 7.2 needs for the camera and the shell. The compute passes bind only set 0 and
        // are content with a layout that declares more than they use.
        const VkDescriptorSetLayout sets[2] = {tonemapSetLayout_, sceneSetLayout_};

        VkPipelineLayoutCreateInfo pli{};
        pli.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pli.setLayoutCount         = 2;
        pli.pSetLayouts            = sets;
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

    // The shadow map itself. One image for the whole renderer, because the sun is the same on
    // every monitor (spec 8.2) -- unlike the sort order or the exposure, which belong to a view.
    bool CreateShadowMap(uint32_t size) {
        if (size == shadowSize_ && shadowImage_) return true;

        DestroyShadowMap();
        if (size == 0) return true;

        VkDevice dev = ctx_->device();

        if (!vk::CreateImage2D(*ctx_, size, size, shadowFormat_,
                               VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                                   VK_IMAGE_USAGE_SAMPLED_BIT,
                               VK_IMAGE_ASPECT_DEPTH_BIT, &shadowImage_, &shadowAlloc_,
                               &shadowView_)) {
            app::Log("vulkan: shadow map %ux%u failed", size, size);
            return false;
        }

        VkFramebufferCreateInfo fci{};
        fci.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fci.renderPass      = passes_.shadow;
        fci.attachmentCount = 1;
        fci.pAttachments    = &shadowView_;
        fci.width           = size;
        fci.height          = size;
        fci.layers          = 1;
        if (vkCreateFramebuffer(dev, &fci, nullptr, &shadowFbo_) != VK_SUCCESS) {
            app::Log("vulkan: shadow framebuffer failed");
            DestroyShadowMap();
            return false;
        }

        shadowSize_ = size;
        app::Log("vulkan: shadow map %ux%u", size, size);
        for (auto& w : windows_) {
            for (uint32_t i = 0; i < kFramesInFlight; ++i) BindShadowMap(w.sceneSet[i]);
        }
        return true;
    }

    void DestroyShadowMap() {
        VkDevice dev = ctx_->device();
        if (shadowFbo_) vkDestroyFramebuffer(dev, shadowFbo_, nullptr);
        if (shadowView_) vkDestroyImageView(dev, shadowView_, nullptr);
        if (shadowImage_) vmaDestroyImage(ctx_->allocator(), shadowImage_, shadowAlloc_);
        shadowFbo_   = VK_NULL_HANDLE;
        shadowView_  = VK_NULL_HANDLE;
        shadowImage_ = VK_NULL_HANDLE;
        shadowAlloc_ = VK_NULL_HANDLE;
        shadowSize_  = 0;
    }

    void BindShadowMap(VkDescriptorSet set) {
        if (!shadowView_ || !shadowSampler_) return;

        VkDescriptorImageInfo info{};
        info.sampler     = shadowSampler_;
        info.imageView   = shadowView_;
        info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkWriteDescriptorSet write{};
        write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet          = set;
        write.dstBinding      = 1;
        write.descriptorCount = 1;
        write.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo      = &info;

        vkUpdateDescriptorSets(ctx_->device(), 1, &write, 0, nullptr);
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

    void BindExposureBuffer(VkDescriptorSet set, const Buffer& buffer, uint32_t binding = 1) {
        VkDescriptorBufferInfo info{};
        info.buffer = buffer.handle;
        info.range  = buffer.size;

        VkWriteDescriptorSet write{};
        write.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet          = set;
        write.dstBinding      = binding;
        write.descriptorCount = 1;
        write.descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        write.pBufferInfo     = &info;

        vkUpdateDescriptorSets(ctx_->device(), 1, &write, 0, nullptr);
    }

    // `sourceLayout` is not always GENERAL: the first downsample reads the HDR attachment, which
    // the render pass leaves in SHADER_READ_ONLY_OPTIMAL. A descriptor that names the wrong layout
    // is undefined behaviour rather than an error, which is the kind of bug that works on one
    // driver and produces a black chain on the next.
    void BindBloomStep(VkDescriptorSet set, VkImageView source, VkImageLayout sourceLayout,
                       VkImageView destination) {
        VkDescriptorImageInfo infos[2]{};
        infos[0].sampler     = sampler_;
        infos[0].imageView   = source;
        infos[0].imageLayout = sourceLayout;

        infos[1].imageView   = destination;
        infos[1].imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkWriteDescriptorSet writes[2]{};
        writes[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet          = set;
        writes[0].dstBinding      = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[0].pImageInfo      = &infos[0];

        writes[1].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet          = set;
        writes[1].dstBinding      = 1;
        writes[1].descriptorCount = 1;
        writes[1].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[1].pImageInfo      = &infos[1];

        vkUpdateDescriptorSets(ctx_->device(), 2, writes, 0, nullptr);
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

        // The finished chain, which the tonemap adds back (spec 8.1). Level 0 is the widest, and
        // it is where every level above it has already been summed by the time it is read.
        VkDescriptorImageInfo bloomInfo{};
        bloomInfo.sampler     = sampler_;
        bloomInfo.imageView   = w.target->bloomView(0);
        bloomInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkWriteDescriptorSet bloomWrite{};
        bloomWrite.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        bloomWrite.dstSet          = w.tonemapSet;
        bloomWrite.dstBinding      = 2;
        bloomWrite.descriptorCount = 1;
        bloomWrite.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bloomWrite.pImageInfo      = &bloomInfo;
        vkUpdateDescriptorSets(ctx_->device(), 1, &bloomWrite, 0, nullptr);

        // And the chain's own steps, which name the same views.
        const uint32_t levels = w.target->bloomLevels();
        for (uint32_t i = 0; i < levels; ++i) {
            BindBloomStep(w.bloomDown[i], i == 0 ? view : w.target->bloomView(i - 1),
                          i == 0 ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                 : VK_IMAGE_LAYOUT_GENERAL,
                          w.target->bloomView(i));
            if (i > 0) {
                BindBloomStep(w.bloomUp[i], w.target->bloomView(i), VK_IMAGE_LAYOUT_GENERAL,
                              w.target->bloomView(i - 1));
            }
        }

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

        const float rise = world_.BoardRise(t);
        SetSceneTiming(&uniforms, world_.GrowthTime(t), rise);

        // Spec 8.2. The depth epsilon is small because the real work is done by the slope-scaled
        // bias in the caster's rasteriser and the normal offset on the receiver; this is only what
        // is left over, and a larger one here is what makes a shadow crawl out from under the
        // thing casting it.
        SetShadow(&uniforms, ShadowViewProj(world_.sky.keyDirection, shadowBounds_), shadowSize_,
                  ShadowTexelWorldSize(shadowBounds_, shadowSize_), 0.0008f);

        // The board lights the city only once it is lit (spec 7.6). Intensity is scaled by how far
        // it has risen as well, so the light does not arrive before the object casting it.
        const bool lit = world_.timeline.BoardSeconds(t) >= 0;

        // The falloff radius is the glyph height, not the board's width. Sized by the width it
        // reaches past the city and pools on open desert, which reads as a second sunset rather
        // than as a sign lighting the roofs under it.
        SetBoardLight(&uniforms,
                      core::Vec3{world_.board.origin.x,
                                 (world_.board.bandBottom + world_.board.bandTop) * 0.5f,
                                 world_.board.origin.y},
                      lit ? rise * 2.0f : 0.0f, world_.board.glyphHeight * 1.5f);

        SetBlast(&uniforms, world_.detonation.center, world_.ShellRadius(t));
        SetFire(&uniforms, world_.FireCenter(t), world_.FireRadius(t), world_.FireColor(t),
                world_.FlashIntensity(t));

        std::memcpy(w.sceneUbo[slot].mapped, &uniforms, sizeof(uniforms));
    }

    // The three storage buffers, and the two pipeline layouts that reach them. The compute passes
    // bind them as set 0; the draw already spends set 0 on the scene uniforms and takes them as
    // set 1, which is why shaders/fragment_common.glsl takes its set index from whoever includes it.
    bool CreateFragmentLayout() {
        VkDevice dev = ctx_->device();

        VkDescriptorSetLayoutBinding bindings[3]{};
        for (uint32_t i = 0; i < 3; ++i) {
            bindings[i].binding         = i;
            bindings[i].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            bindings[i].descriptorCount = 1;
            bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_VERTEX_BIT;
        }

        VkDescriptorSetLayoutCreateInfo dsl{};
        dsl.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        dsl.bindingCount = 3;
        dsl.pBindings    = bindings;
        if (vkCreateDescriptorSetLayout(dev, &dsl, nullptr, &fragmentSetLayout_) != VK_SUCCESS) {
            return false;
        }

        VkPushConstantRange push{};
        push.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        push.size       = sizeof(FragmentPush);

        VkPipelineLayoutCreateInfo cli{};
        cli.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        cli.setLayoutCount         = 1;
        cli.pSetLayouts            = &fragmentSetLayout_;
        cli.pushConstantRangeCount = 1;
        cli.pPushConstantRanges    = &push;
        if (vkCreatePipelineLayout(dev, &cli, nullptr, &fragmentComputeLayout_) != VK_SUCCESS) {
            return false;
        }

        const VkDescriptorSetLayout sets[2] = {sceneSetLayout_, fragmentSetLayout_};

        VkPipelineLayoutCreateInfo gli{};
        gli.sType          = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        gli.setLayoutCount = 2;
        gli.pSetLayouts    = sets;
        return vkCreatePipelineLayout(dev, &gli, nullptr, &fragmentDrawLayout_) == VK_SUCCESS;
    }

    bool CreateBoardLayout() {
        VkPushConstantRange push{};
        push.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        push.size       = sizeof(BoardPush);

        VkPipelineLayoutCreateInfo pli{};
        pli.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pli.setLayoutCount         = 1;
        pli.pSetLayouts            = &sceneSetLayout_;
        pli.pushConstantRangeCount = 1;
        pli.pPushConstantRanges    = &push;

        return vkCreatePipelineLayout(ctx_->device(), &pli, nullptr, &boardPipelineLayout_) ==
               VK_SUCCESS;
    }

    bool CreateBloomLayout() {
        VkDevice dev = ctx_->device();

        // The source, the destination, and the exposure buffer the first downsample bins the
        // histogram into. The upsample shares the layout and declares only the first two.
        VkDescriptorSetLayoutBinding bindings[3]{};
        bindings[0].binding         = 0;
        bindings[0].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[0].descriptorCount = 1;
        bindings[0].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;

        bindings[1].binding         = 1;
        bindings[1].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        bindings[1].descriptorCount = 1;
        bindings[1].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;

        bindings[2].binding         = 2;
        bindings[2].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[2].descriptorCount = 1;
        bindings[2].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;

        VkDescriptorSetLayoutCreateInfo dsl{};
        dsl.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        dsl.bindingCount = 3;
        dsl.pBindings    = bindings;
        if (vkCreateDescriptorSetLayout(dev, &dsl, nullptr, &bloomSetLayout_) != VK_SUCCESS) {
            return false;
        }

        VkPushConstantRange push{};
        push.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        push.size       = sizeof(BloomPush);

        VkPipelineLayoutCreateInfo pli{};
        pli.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pli.setLayoutCount         = 1;
        pli.pSetLayouts            = &bloomSetLayout_;
        pli.pushConstantRangeCount = 1;
        pli.pPushConstantRanges    = &push;
        return vkCreatePipelineLayout(dev, &pli, nullptr, &bloomPipelineLayout_) == VK_SUCCESS;
    }

    bool CreateMissileLayout() {
        VkPushConstantRange push{};
        push.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        push.size       = sizeof(MissilePush);

        VkPipelineLayoutCreateInfo pli{};
        pli.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pli.setLayoutCount         = 1;
        pli.pSetLayouts            = &sceneSetLayout_;
        pli.pushConstantRangeCount = 1;
        pli.pPushConstantRanges    = &push;

        return vkCreatePipelineLayout(ctx_->device(), &pli, nullptr, &missilePipelineLayout_) ==
               VK_SUCCESS;
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

    // The key light's depth map (spec 8.2). Once a frame, not once a window: the sun does not
    // move between monitors, so this follows the fragment simulation's pattern and is recorded
    // into whichever command buffer opens first.
    //
    // Casters are the city and the fragments, which is what a cloud made of a city has to cast.
    // The terrain does not cast -- it is a nearly flat basin whose own relief is dune-scale -- and
    // neither does the board.
    void RecordShadow(const WindowTarget::Frame& frame, Attached& w) {
        if (!shadowFbo_ || !shadowSize_) return;

        VkClearValue clear{};
        clear.depthStencil = {1.0f, 0};

        VkRenderPassBeginInfo bi{};
        bi.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        bi.renderPass        = passes_.shadow;
        bi.framebuffer       = shadowFbo_;
        bi.renderArea.extent = {shadowSize_, shadowSize_};
        bi.clearValueCount   = 1;
        bi.pClearValues      = &clear;

        vkCmdBeginRenderPass(frame.cmd, &bi, VK_SUBPASS_CONTENTS_INLINE);
        SetViewport(frame.cmd, {shadowSize_, shadowSize_});

        if (buildingCount_) {
            vkCmdBindPipeline(frame.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, shadowBuildingPipeline_);
            vkCmdBindDescriptorSets(frame.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    scenePipelineLayout_, 0, 1, &w.sceneSet[frame.frameSlot], 0,
                                    nullptr);

            const VkBuffer     buffers[2] = {boxVertices_.handle, buildingInstances_.handle};
            const VkDeviceSize offsets[2] = {0, 0};
            vkCmdBindVertexBuffers(frame.cmd, 0, 2, buffers, offsets);
            vkCmdBindIndexBuffer(frame.cmd, boxIndices_.handle, 0, VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexed(frame.cmd, boxIndexCount_, buildingCount_, 0, 0, 0);
        }

        if (fragmentSet_ && fragmentLayout_.total && world_.ShellRadius(frameTime_) > 0.0f) {
            vkCmdBindPipeline(frame.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, shadowFragmentPipeline_);

            const VkDescriptorSet sets[2] = {w.sceneSet[frame.frameSlot], fragmentSet_};
            vkCmdBindDescriptorSets(frame.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, fragmentDrawLayout_,
                                    0, 2, sets, 0, nullptr);
            vkCmdDraw(frame.cmd, 3 * fragmentLayout_.total, 1, 0, 0);
        }

        vkCmdEndRenderPass(frame.cmd);
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

        vkCmdBindPipeline(frame.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, groundPipeline_);

        // The range is drawn before the terrain rather than after. Both write depth, so the order
        // does not change the image, but the mountains cover most of the upper band and filling
        // that depth first rejects a good share of the terrain's distant fragments.
        DrawMesh(frame.cmd, horizonVertices_, horizonIndices_, horizonIndexCount_);
        DrawMesh(frame.cmd, terrainVertices_, terrainIndices_, terrainIndexCount_);

        if (buildingCount_) {
            vkCmdBindPipeline(frame.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, buildingPipeline_);

            const VkBuffer     buffers[2] = {boxVertices_.handle, buildingInstances_.handle};
            const VkDeviceSize offsets[2] = {0, 0};
            vkCmdBindVertexBuffers(frame.cmd, 0, 2, buffers, offsets);
            vkCmdBindIndexBuffer(frame.cmd, boxIndices_.handle, 0, VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexed(frame.cmd, boxIndexCount_, buildingCount_, 0, 0, 0);
        }

        if (boardBoxCount_ && world_.BoardRise(frameTime_) > 0.0f) {
            vkCmdBindPipeline(frame.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, boardPipeline_);
            vkCmdBindDescriptorSets(frame.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    boardPipelineLayout_, 0, 1, &w.sceneSet[frame.frameSlot], 0,
                                    nullptr);

            const uint64_t mask = world::BoardMask(world_.timeline.BoardSeconds(frameTime_));

            BoardPush push;
            push.litLow  = static_cast<uint32_t>(mask & 0xFFFFFFFFull);
            push.litHigh = static_cast<uint32_t>(mask >> 32);
            push.rise    = world_.BoardRise(frameTime_);
            // Spec 5.1 puts a lit segment at 30 to 60 linear. The shader divides by the base
            // exposure, so this is the figure the viewer sees rather than the one the scene holds.
            push.emissive = 42.0f;

            vkCmdPushConstants(frame.cmd, boardPipelineLayout_,
                               VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                               sizeof(push), &push);

            const VkBuffer     buffers[2] = {boxVertices_.handle, boardInstances_.handle};
            const VkDeviceSize offsets[2] = {0, 0};
            vkCmdBindVertexBuffers(frame.cmd, 0, 2, buffers, offsets);
            vkCmdBindIndexBuffer(frame.cmd, boxIndices_.handle, 0, VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexed(frame.cmd, boxIndexCount_, boardBoxCount_, 0, 0, 0);
        }

        if (missileIndexCount_ && world_.MissileVisible(frameTime_)) {
            vkCmdBindPipeline(frame.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, missilePipeline_);
            vkCmdBindDescriptorSets(frame.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    missilePipelineLayout_, 0, 1, &w.sceneSet[frame.frameSlot], 0,
                                    nullptr);

            MissilePush       push;
            const core::Mat4  model = world_.MissileTransform(frameTime_);
            std::memcpy(push.model, model.m, sizeof(push.model));

            // Spec 5.1 puts the exhaust at 20 to 40 linear. Like the board and the windows, the
            // shader divides by the base exposure, so this is what the viewer sees.
            //
            // At the bottom of that band rather than in the middle, and the shader dims it further
            // with distance. The missile now enters kilometres out and kilometres up (spec 7.1),
            // and a 30-linear emitter that small and that far away is a bloom bead with nothing
            // legible inside it. The airframe carries the visibility instead, lit rather than
            // emissive, which is what "bright, but not a glowing dot" asks for.
            push.exhaust[0] = 9.0f;

            vkCmdPushConstants(frame.cmd, missilePipelineLayout_,
                               VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                               sizeof(push), &push);

            DrawMesh(frame.cmd, missileVertices_, missileIndices_, missileIndexCount_);
        }

        if (fragmentSet_ && fragmentLayout_.total && world_.ShellRadius(frameTime_) > 0.0f) {
            vkCmdBindPipeline(frame.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, fragmentPipeline_);

            const VkDescriptorSet sets[2] = {w.sceneSet[frame.frameSlot], fragmentSet_};
            vkCmdBindDescriptorSets(frame.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, fragmentDrawLayout_,
                                    0, 2, sets, 0, nullptr);

            // Three vertices a fragment, no vertex buffer bound at all (spec 7.3). One instance
            // of 3N vertices rather than N instances of three: see fragment.vert.
            vkCmdDraw(frame.cmd, 3 * fragmentLayout_.total, 1, 0, 0);
        }

        // The sky, last of the opaque passes and before anything that blends. Its triangle is on
        // the far plane, so the depth test passes only where nothing above was drawn.
        vkCmdBindPipeline(frame.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, skyPipeline_);
        vkCmdBindDescriptorSets(frame.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, scenePipelineLayout_, 0,
                                1, &w.sceneSet[frame.frameSlot], 0, nullptr);
        vkCmdDraw(frame.cmd, 3, 1, 0, 0);

        // The particles (spec 8.3). After everything that writes depth, so they are occluded
        // correctly, and before the emissive passes, so the fireball's glow goes over the dust it
        // is lighting rather than under it.
        //
        // Both draws run the full instance count whatever is actually alive. The live count is a
        // number only the GPU knows, and the choice is between an indirect draw plus the barrier
        // it needs or a few thousand vertex invocations that clip themselves away immediately.
        if (particleTotal_ && w.particleSet) {
            const VkDescriptorSet sets[2] = {w.sceneSet[frame.frameSlot], w.particleSet};
            vkCmdBindDescriptorSets(frame.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, particleDrawLayout_,
                                    0, 2, sets, 0, nullptr);

            const ParticlePush push = BuildParticlePush(frameTime_);
            vkCmdPushConstants(frame.cmd, particleDrawLayout_, VK_SHADER_STAGE_VERTEX_BIT, 0,
                               sizeof(push), &push);

            // The dust, far to near, then the emitters. firstInstance is what picks the half of
            // the sorted buffer each draw reads, which is why one vertex shader serves both.
            vkCmdBindPipeline(frame.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, particleAlphaPipeline_);
            vkCmdDraw(frame.cmd, 6, particleTotal_, 0, 0);

            vkCmdBindPipeline(frame.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, particleAddPipeline_);
            vkCmdDraw(frame.cmd, 6, particleTotal_, 0, particleTotal_);
        }

        // Emissive, after the opaque geometry and in the order spec 8.1 gives: the fireball is
        // occluded by what is in front of it, the flash is occluded by nothing.
        if (world_.FireRadius(frameTime_) > 0.0f) {
            vkCmdBindPipeline(frame.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, fireballPipeline_);
            vkCmdBindDescriptorSets(frame.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    scenePipelineLayout_, 0, 1, &w.sceneSet[frame.frameSlot], 0,
                                    nullptr);
            vkCmdDraw(frame.cmd, kFireballVertices, 1, 0, 0);
        }

        if (world_.FlashIntensity(frameTime_) > 0.0f) {
            vkCmdBindPipeline(frame.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, flashPipeline_);
            vkCmdBindDescriptorSets(frame.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    scenePipelineLayout_, 0, 1, &w.sceneSet[frame.frameSlot], 0,
                                    nullptr);
            vkCmdDraw(frame.cmd, 3, 1, 0, 0);
        }

        vkCmdEndRenderPass(frame.cmd);
    }

    TonemapPush BuildTonemapPush(const Attached& w, float dt) const {
        const VkExtent2D extent = w.target->extent();

        TonemapPush push;
        // Base exposure comes from the time of day. Night is not a darker noon, it is a different
        // exposure, or the city's own windows read as dim rather than as the light. The adaptation
        // of spec 8.2 moves around this rather than replacing it.
        push.exposure   = world_.sky.baseExposure;
        push.ditherAmp  = 1.0f;
        push.nightShift = world_.sky.bodyIsMoon ? 1.0f : 0.0f;
        push.delta      = dt;
        push.width      = static_cast<float>(extent.width);
        push.height     = static_cast<float>(extent.height);

        // The range the adaptation is allowed to reach. Wide enough that the flash can take the
        // whole scene down to nothing and the recovery can bring a night desert back, and bounded
        // so a frame that is entirely fireball or entirely sky cannot run away.
        push.minExposure = world_.sky.baseExposure * 0.004f;
        push.maxExposure = world_.sky.baseExposure * 4.0f;
        push.bloom       = kBloomMix;
        return push;
    }

    // The bloom chain (spec 8.1). Runs between the two render passes, on the finished HDR image:
    // down to the smallest level with a thresholded first step, then back up with a tent filter,
    // summing as it goes, so level 0 ends up holding the whole chain.
    // Returns whether the chain ran, because its first step also bins the exposure histogram and
    // RecordExposure has to do that itself when it did not.
    bool RecordBloom(const WindowTarget::Frame& frame, Attached& w) {
        const uint32_t levels = BloomLevelsFor(w);
        if (!levels) return false;

        // The whole chain is rewritten every frame, so the previous contents are worth nothing and
        // UNDEFINED is the honest old layout: it lets the driver skip a decompress it would
        // otherwise have to do to preserve data nothing will read.
        VkImageMemoryBarrier toGeneral{};
        toGeneral.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        toGeneral.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
        toGeneral.newLayout           = VK_IMAGE_LAYOUT_GENERAL;
        toGeneral.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toGeneral.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toGeneral.image               = w.target->bloomImage();
        toGeneral.subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, 1};
        toGeneral.dstAccessMask       = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(frame.cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                             &toGeneral);

        // `group` is the shader's workgroup edge: 16 for the downsample, which bins the histogram
        // with one bin per invocation, and 8 for the upsample.
        const auto dispatch = [&](VkDescriptorSet set, const BloomPush& push, VkExtent2D extent,
                                  uint32_t group) {
            vkCmdBindDescriptorSets(frame.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, bloomPipelineLayout_,
                                    0, 1, &set, 0, nullptr);
            vkCmdPushConstants(frame.cmd, bloomPipelineLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                               sizeof(push), &push);
            vkCmdDispatch(frame.cmd, (extent.width + group - 1) / group,
                          (extent.height + group - 1) / group, 1);
            Barrier(frame.cmd);
        };

        vkCmdBindPipeline(frame.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, bloomDownPipeline_);
        for (uint32_t i = 0; i < levels; ++i) {
            const VkExtent2D extent = w.target->bloomExtent(i);

            BloomPush push;
            push.destinationWidth  = static_cast<float>(extent.width);
            push.destinationHeight = static_cast<float>(extent.height);
            push.threshold         = kBloomThreshold;
            push.knee              = kBloomKnee;
            push.firstLevel        = i == 0 ? 1.0f : 0.0f;

            // The first step also bins the histogram from every second HDR texel, which on an
            // odd-sized image reaches one column or row past the halved extent (rounded down).
            VkExtent2D grid = extent;
            if (i == 0) {
                const VkExtent2D hdr = w.target->extent();
                grid.width  = std::max(grid.width, (hdr.width + 1) / 2);
                grid.height = std::max(grid.height, (hdr.height + 1) / 2);
            }
            dispatch(w.bloomDown[i], push, grid, 16);
        }

        vkCmdBindPipeline(frame.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, bloomUpPipeline_);
        for (uint32_t i = levels; i-- > 1;) {
            const VkExtent2D extent = w.target->bloomExtent(i - 1);

            BloomPush push;
            push.destinationWidth  = static_cast<float>(extent.width);
            push.destinationHeight = static_cast<float>(extent.height);
            push.intensity         = kBloomIntensity;
            dispatch(w.bloomUp[i], push, extent, 8);
        }

        // The tonemap samples level 0 from the fragment stage.
        VkMemoryBarrier barrier{};
        barrier.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(frame.cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 1, &barrier, 0, nullptr, 0,
                             nullptr);
        return true;
    }

    // Auto-exposure (spec 8.2). Runs between the two render passes: the HDR image is complete and
    // in SHADER_READ_ONLY_OPTIMAL by then, and the tonemap that reads the result has not started.
    // `binned` says the bloom chain already filled the histogram this frame.
    void RecordExposure(const WindowTarget::Frame& frame, Attached& w, float dt, bool binned) {
        const VkExtent2D  extent = w.target->extent();
        const TonemapPush push   = BuildTonemapPush(w, dt);

        vkCmdBindDescriptorSets(frame.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, tonemapPipelineLayout_,
                                0, 1, &w.tonemapSet, 0, nullptr);
        vkCmdPushConstants(frame.cmd, tonemapPipelineLayout_,
                           VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT, 0,
                           sizeof(push), &push);

        if (!binned) {
            // One invocation per second texel each way, so the group count is over half the
            // extent.
            const uint32_t groupsX = (extent.width / 2 + 15) / 16;
            const uint32_t groupsY = (extent.height / 2 + 15) / 16;

            vkCmdBindPipeline(frame.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, histogramPipeline_);
            vkCmdDispatch(frame.cmd, groupsX, groupsY, 1);

            Barrier(frame.cmd);
        }

        vkCmdBindPipeline(frame.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, adaptPipeline_);
        vkCmdDispatch(frame.cmd, 1, 1, 1);

        // The tonemap reads what the adapt pass just wrote, from the fragment stage.
        VkMemoryBarrier barrier{};
        barrier.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(frame.cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 1, &barrier, 0, nullptr, 0,
                             nullptr);
    }

    void RecordTonemap(const WindowTarget::Frame& frame, Attached& w, float dt) {
        VkRenderPassBeginInfo bi{};
        bi.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        bi.renderPass        = passes_.present;
        bi.framebuffer       = frame.presentFbo;
        bi.renderArea.extent = w.target->extent();

        vkCmdBeginRenderPass(frame.cmd, &bi, VK_SUBPASS_CONTENTS_INLINE);
        SetViewport(frame.cmd, w.target->extent());

        vkCmdBindPipeline(frame.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, tonemapPipeline_);

        const VkDescriptorSet sets[2] = {w.tonemapSet, w.sceneSet[frame.frameSlot]};
        vkCmdBindDescriptorSets(frame.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, tonemapPipelineLayout_,
                                0, 2, sets, 0, nullptr);

        const TonemapPush push = BuildTonemapPush(w, dt);
        vkCmdPushConstants(frame.cmd, tonemapPipelineLayout_,
                           VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT, 0,
                           sizeof(push), &push);

        vkCmdDraw(frame.cmd, 3, 1, 0, 0);
        vkCmdEndRenderPass(frame.cmd);
    }

    // Spec 4.1: "State torn down, new seed drawn, empty land again." Everything the world owns on
    // the device is destroyed and rebuilt, because the sizes change with the seed — a different
    // city is a different number of buildings and a different number of fragments.
    //
    // It costs a stall of a few tens of milliseconds, and it is spent during the black at the end
    // of phase 10, which is the one moment in the cycle where a dropped frame is invisible.
    void ResetCycle() {
        ctx_->WaitIdle();

        vk::DestroyBuffer(*ctx_, &terrainVertices_);
        vk::DestroyBuffer(*ctx_, &terrainIndices_);
        vk::DestroyBuffer(*ctx_, &horizonVertices_);
        vk::DestroyBuffer(*ctx_, &horizonIndices_);
        vk::DestroyBuffer(*ctx_, &missileVertices_);
        vk::DestroyBuffer(*ctx_, &missileIndices_);
        vk::DestroyBuffer(*ctx_, &boxVertices_);
        vk::DestroyBuffer(*ctx_, &boxIndices_);
        vk::DestroyBuffer(*ctx_, &buildingInstances_);
        vk::DestroyBuffer(*ctx_, &boardInstances_);
        vk::DestroyBuffer(*ctx_, &shatterBoxes_);
        vk::DestroyBuffer(*ctx_, &fragmentRest_);
        vk::DestroyBuffer(*ctx_, &fragmentState_);

        terrainIndexCount_ = horizonIndexCount_ = missileIndexCount_ = 0;
        boxIndexCount_ = buildingCount_ = boardBoxCount_ = 0;
        fragmentLayout_       = FragmentLayout{};
        fragmentsInitialised_ = false;
        // fragmentSet_ is deliberately kept: it is rewritten by UploadFragments below, and the
        // descriptor pool has no free.

        // Seed 0 means draw one from the clock, which is what makes the next cycle a different
        // city rather than the same one again (spec 6.4).
        world_ = world::Generate(settings_, 0);

        FitShadowMap();

        // UploadStaticGeometry rebuilds the city and the fragments as well, which is exactly what
        // is wanted here: everything the world owns on the device, from one call.
        if (!UploadStaticGeometry()) {
            app::Log("vulkan: cycle reset failed to rebuild the world");
        }
    }

    // The box the shadow map covers, and the map's size. Both belong to a cycle rather than to a
    // frame: the box is the reach of things this world will produce, and resizing the image is a
    // device-idle operation, which is exactly what a cycle reset already is.
    void FitShadowMap() {
        const world::Detonation& det = world_.detonation;

        // Wide enough for the city, the debris field the shell throws, and the cloud's cap. The
        // margin is for the wind, which drifts the whole cloud for the back half of the cycle.
        shadowBounds_.radius =
            std::fmax(std::fmax(world_.cityRadius, det.reach), det.capRadius + det.capTube) * 1.2f;

        // Tall enough for the cloud, which is the highest thing that casts by a wide margin: the
        // tallest building is a tenth of it.
        shadowBounds_.top =
            std::fmax(world_.city.tallest, det.capHeight + det.capTube) * 1.1f + 10.0f;

        if (!CreateShadowMap(ShadowMapSize(QualityLevel()))) {
            app::Log("vulkan: shadow map resize failed, shadows are off for this cycle");
        }
    }

    bool UploadMesh(const world::Mesh& mesh, Buffer* vertices, Buffer* indices,
                    uint32_t* indexCount) {
        *indexCount = 0;
        if (mesh.empty()) return true;  // an empty mesh is legitimate, not a failure

        if (!vk::CreateBufferWithData(*ctx_, mesh.vertices.data(), mesh.vertexBytes(),
                                      VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, vertices) ||
            !vk::CreateBufferWithData(*ctx_, mesh.indices.data(), mesh.indexBytes(),
                                      VK_BUFFER_USAGE_INDEX_BUFFER_BIT, indices)) {
            return false;
        }

        *indexCount = static_cast<uint32_t>(mesh.indices.size());
        return true;
    }

    bool UploadStaticGeometry() {
        if (!UploadMesh(world_.terrainMesh, &terrainVertices_, &terrainIndices_,
                        &terrainIndexCount_) ||
            !UploadMesh(world_.horizonMesh, &horizonVertices_, &horizonIndices_,
                        &horizonIndexCount_) ||
            !UploadMesh(world_.missileMesh, &missileVertices_, &missileIndices_,
                        &missileIndexCount_)) {
            app::Log("vulkan: static geometry upload failed");
            return false;
        }

        if (!UploadCity()) return false;
        if (!UploadFragments()) return false;

        app::Log("vulkan: uploaded %u terrain indices, %u horizon indices, %u missile indices, "
                 "%u buildings",
                 terrainIndexCount_, horizonIndexCount_, missileIndexCount_, buildingCount_);
        return true;
    }

    bool UploadCity() {
        std::vector<BoxVertex> boxVerts;
        std::vector<uint32_t>  boxIdx;
        BuildUnitCube(&boxVerts, &boxIdx);

        std::vector<BuildingInstance> instances;
        PackBuildings(world_.city, &instances);
        if (instances.empty()) return true;  // a city with no buildings is not a failure

        if (!vk::CreateBufferWithData(*ctx_, boxVerts.data(), boxVerts.size() * sizeof(BoxVertex),
                                      VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, &boxVertices_) ||
            !vk::CreateBufferWithData(*ctx_, boxIdx.data(), boxIdx.size() * sizeof(uint32_t),
                                      VK_BUFFER_USAGE_INDEX_BUFFER_BIT, &boxIndices_) ||
            !vk::CreateBufferWithData(*ctx_, instances.data(),
                                      instances.size() * sizeof(BuildingInstance),
                                      VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, &buildingInstances_)) {
            app::Log("vulkan: city upload failed");
            return false;
        }

        boxIndexCount_ = static_cast<uint32_t>(boxIdx.size());
        buildingCount_ = static_cast<uint32_t>(instances.size());

        std::vector<BoardInstance> board;
        PackBoard(world_.board, &board);
        if (!board.empty()) {
            if (!vk::CreateBufferWithData(*ctx_, board.data(),
                                          board.size() * sizeof(BoardInstance),
                                          VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, &boardInstances_)) {
                app::Log("vulkan: board upload failed");
                return false;
            }
            boardBoxCount_ = static_cast<uint32_t>(board.size());
        }

        return true;
    }

    // Allocates the fragment buffers and uploads the one thing the CPU knows about them: the box
    // list. Nine megabytes of triangles are produced on the device by fragment_init.comp on the
    // first frame and never travel over the bus (spec 7.3).
    bool UploadFragments() {
        std::vector<ShatterBox> boxes;
        PackShatterBoxes(world_.city, world_.board, FragmentQuality(), &boxes, &fragmentLayout_);
        if (boxes.empty()) return true;

        const uint32_t count = fragmentLayout_.total;

        const VkBufferUsageFlags storage =
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;

        if (!vk::CreateBufferWithData(*ctx_, boxes.data(), boxes.size() * sizeof(ShatterBox),
                                      storage, &shatterBoxes_) ||
            !vk::CreateBuffer(*ctx_, VkDeviceSize(count) * 64, storage, vk::BufferUse::GpuOnly,
                              &fragmentRest_) ||
            !vk::CreateBuffer(*ctx_, VkDeviceSize(count) * 64, storage, vk::BufferUse::GpuOnly,
                              &fragmentState_)) {
            app::Log("vulkan: fragment buffers failed");
            return false;
        }

        // Allocated once and rewritten on every cycle reset. Allocating a fresh one per cycle
        // would drain the pool after a handful of them, and the symptom would be a screen saver
        // that worked perfectly for ten minutes and then stopped drawing fragments.
        if (!fragmentSet_ && !AllocateSet(fragmentSetLayout_, &fragmentSet_)) return false;

        const Buffer* buffers[3] = {&shatterBoxes_, &fragmentRest_, &fragmentState_};

        VkDescriptorBufferInfo infos[3]{};
        VkWriteDescriptorSet   writes[3]{};
        for (uint32_t i = 0; i < 3; ++i) {
            infos[i].buffer = buffers[i]->handle;
            infos[i].range  = buffers[i]->size;

            writes[i].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet          = fragmentSet_;
            writes[i].dstBinding      = i;
            writes[i].descriptorCount = 1;
            writes[i].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[i].pBufferInfo     = &infos[i];
        }
        vkUpdateDescriptorSets(ctx_->device(), 3, writes, 0, nullptr);

        app::Log("vulkan: %u fragments from %u boxes, %.1f MB",
                 count, fragmentLayout_.boxes, double(count) * 128.0 / (1024.0 * 1024.0));
        return true;
    }

    void BindParticleBuffers(Attached& a) {
        const Buffer* buffers[2] = {&a.particleSort, &a.particleBins};

        VkDescriptorBufferInfo infos[2]{};
        VkWriteDescriptorSet   writes[2]{};
        for (uint32_t i = 0; i < 2; ++i) {
            infos[i].buffer = buffers[i]->handle;
            infos[i].range  = VK_WHOLE_SIZE;

            writes[i].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet          = a.particleSet;
            writes[i].dstBinding      = i;
            writes[i].descriptorCount = 1;
            writes[i].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[i].pBufferInfo     = &infos[i];
        }
        vkUpdateDescriptorSets(ctx_->device(), 2, writes, 0, nullptr);
    }

    // The sort buffers and the two layouts that reach them. Same split as the fragments: compute
    // takes them as set 0 and the draw, which already spends set 0 on the scene, takes them as
    // set 1 - which is why shaders/particle_common.glsl parameterises its set index.
    bool CreateParticleLayout() {
        VkDevice dev = ctx_->device();

        VkDescriptorSetLayoutBinding bindings[2]{};
        for (uint32_t i = 0; i < 2; ++i) {
            bindings[i].binding         = i;
            bindings[i].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            bindings[i].descriptorCount = 1;
            bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_VERTEX_BIT;
        }

        VkDescriptorSetLayoutCreateInfo dsl{};
        dsl.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        dsl.bindingCount = 2;
        dsl.pBindings    = bindings;
        if (vkCreateDescriptorSetLayout(dev, &dsl, nullptr, &particleSetLayout_) != VK_SUCCESS) {
            return false;
        }

        const VkDescriptorSetLayout computeSets[2] = {particleSetLayout_, sceneSetLayout_};

        VkPushConstantRange computePush{};
        computePush.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        computePush.size       = sizeof(ParticlePush);

        VkPipelineLayoutCreateInfo cli{};
        cli.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        cli.setLayoutCount         = 2;
        cli.pSetLayouts            = computeSets;
        cli.pushConstantRangeCount = 1;
        cli.pPushConstantRanges    = &computePush;
        if (vkCreatePipelineLayout(dev, &cli, nullptr, &particleComputeLayout_) != VK_SUCCESS) {
            return false;
        }

        const VkDescriptorSetLayout drawSets[2] = {sceneSetLayout_, particleSetLayout_};

        // Vertex only: the fragment stage is handed everything it needs as varyings, so it has no
        // reason to see a block that is already at the guaranteed size limit.
        VkPushConstantRange drawPush{};
        drawPush.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        drawPush.size       = sizeof(ParticlePush);

        VkPipelineLayoutCreateInfo gli{};
        gli.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        gli.setLayoutCount         = 2;
        gli.pSetLayouts            = drawSets;
        gli.pushConstantRangeCount = 1;
        gli.pPushConstantRanges    = &drawPush;
        return vkCreatePipelineLayout(dev, &gli, nullptr, &particleDrawLayout_) == VK_SUCCESS;
    }

    // Fills the particle push block. Like the fragment one, everything in it is a pure function of
    // the world and the clock, so the field a capture shows is the field that same `t` always has.
    ParticlePush BuildParticlePush(float t) const {
        const world::Detonation& det = world_.detonation;
        const world::Timeline&   tl  = world_.timeline;
        const int                q   = QualityLevel();

        ParticlePush push;
        for (uint32_t i = 0; i < 4; ++i) push.caps[i] = ParticleCapacity(static_cast<int>(i), q);
        push.tail[1] = det.gravity;
        push.tail[2] = det.wind.x;
        push.tail[3] = det.wind.z;

        push.timing[0] = t;
        push.timing[3] = world_.cityRadius;

        const core::Vec3 dir = det.MissileDirection();
        push.mStart[0] = det.missileStart.x;
        push.mStart[1] = det.missileStart.y;
        push.mStart[2] = det.missileStart.z;
        push.mStart[3] = tl.Start(world::Phase::Missile);
        push.mDir[0]   = dir.x;
        push.mDir[1]   = dir.y;
        push.mDir[2]   = dir.z;
        push.mDir[3]   = tl.End(world::Phase::Missile);

        push.blast[0] = det.center.x;
        push.blast[1] = det.center.y;
        push.blast[2] = det.center.z;
        push.blast[3] = det.reach;

        push.phases[0] = tl.Start(world::Phase::Blast);
        push.phases[1] = tl.Duration(world::Phase::Blast);
        push.phases[2] = tl.Start(world::Phase::Gather);
        push.phases[3] = tl.End(world::Phase::Disperse);

        push.cloud[0] = det.stemHeight;
        push.cloud[1] = det.capRadius;
        push.cloud[2] = det.missileLength;
        // When the cloud starts letting go, which is when the embers stop burning and settle.
        // The sort range used to live here; it is the city radius times a constant, so the shader
        // works it out from timing.w rather than spending a second push slot on it.
        push.cloud[3] = tl.Start(world::Phase::Disperse);
        return push;
    }

    // The three sort passes, recorded per window before its render pass opens (spec 8.3).
    //
    // Clear, count, scan, scatter. The two clears are vkCmdFillBuffer rather than a clearing
    // dispatch because a fill is fixed-function DMA and a dispatch is not.
    void RecordParticleSort(const WindowTarget::Frame& frame, Attached& w) {
        const uint32_t total = particleTotal_;
        if (!total || !w.particleSet) return;

        VkCommandBuffer cmd = frame.cmd;

        // kParticleNone, so anything the scatter does not write draws as a degenerate triangle.
        vkCmdFillBuffer(cmd, w.particleSort.handle, 0, VK_WHOLE_SIZE, 0xffffffffu);
        vkCmdFillBuffer(cmd, w.particleBins.handle, 0, VK_WHOLE_SIZE, 0u);
        FillBarrier(cmd);

        const VkDescriptorSet sets[2] = {w.particleSet, w.sceneSet[frame.frameSlot]};
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, particleComputeLayout_, 0, 2,
                                sets, 0, nullptr);

        const ParticlePush push = BuildParticlePush(frameTime_);
        vkCmdPushConstants(cmd, particleComputeLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                           sizeof(push), &push);

        const uint32_t groups = (total + 63) / 64;

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, particleCountPipeline_);
        vkCmdDispatch(cmd, groups, 1, 1);
        Barrier(cmd);

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, particlePrefixPipeline_);
        vkCmdDispatch(cmd, 1, 1, 1);
        Barrier(cmd);

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, particleScatterPipeline_);
        vkCmdDispatch(cmd, groups, 1, 1);
        Barrier(cmd);
    }

    // The two fills above are transfer writes and everything after them is a shader access, which
    // is a different pipeline stage and a different access mask than Barrier covers.
    static void FillBarrier(VkCommandBuffer cmd) {
        VkMemoryBarrier barrier{};
        barrier.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;

        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &barrier, 0, nullptr, 0,
                             nullptr);
    }

    // The level to render at, 0 being the best. The environment override wins over everything,
    // including `auto`, so a run can be measured at a level it would not have chosen.
    int QualityLevel() const {
        const char* env = std::getenv("NUKE_SAVER_QUALITY");
        if (env && env[0] >= '0' && env[0] <= '3') return env[0] - '0';
        return quality_.level();
    }

    // The fragment budget, which is spec 11.2's first lever and the one that cannot move inside a
    // cycle: changing it re-cuts every building, which means repacking 558 boxes, reallocating
    // nine megabytes and re-running the init pass with the device idle. That is a visible stall,
    // and stalling to recover from a stall is not a trade worth making. So it is sampled once per
    // cycle, at the reset, and the levers that are free — particle counts and the bloom chain —
    // carry the frame in between.
    int FragmentQuality() const { return fragmentQuality_; }

    // Spec 11.2 gives up bloom mip count after particle counts. Cheap to change and free to change
    // mid-frame: it is a loop bound and nothing else.
    uint32_t BloomLevelsFor(const Attached& w) const {
        const uint32_t have = w.target->bloomLevels();
        const uint32_t caps[kQualityLevels] = {kMaxBloomLevels, kMaxBloomLevels, 4, 3};
        const uint32_t cap  = caps[QualityLevel() < 0 ? 0
                                   : (QualityLevel() >= kQualityLevels ? kQualityLevels - 1
                                                                       : QualityLevel())];
        return have < cap ? have : cap;
    }

    // Fills the push block from the world and the clock. Everything in it is a pure function of
    // the two, which is what keeps the simulation reproducible from a seed (spec 4.2).
    FragmentPush BuildFragmentPush(float t, float dt) const {
        const world::Detonation& det = world_.detonation;

        FragmentPush push;
        push.counts[0] = fragmentLayout_.boxes;
        push.counts[1] = fragmentLayout_.total;

        push.blast[0] = det.center.x;
        push.blast[1] = det.center.y;
        push.blast[2] = det.center.z;
        push.blast[3] = world_.ShellRadius(t);

        push.timing[0] = t;
        push.timing[1] = dt;
        push.timing[2] = world_.timeline.Start(world::Phase::Gather);
        push.timing[3] = world_.timeline.End(world::Phase::Gather);

        push.release[0] = world_.timeline.Start(world::Phase::Disperse);
        push.release[1] = world_.timeline.End(world::Phase::Disperse);
        push.release[2] = world_.CloudGrow(t);
        push.release[3] = world_.cityRadius;

        push.wind[0] = det.wind.x;
        push.wind[1] = det.wind.y;
        push.wind[2] = det.wind.z;
        push.wind[3] = det.gravity;

        push.cloud[0] = det.stemHeight;
        push.cloud[1] = det.capHeight;
        push.cloud[2] = det.capRadius;
        push.cloud[3] = det.capTube;
        return push;
    }

    // One dispatch a frame, recorded into the first window's command buffer before its render
    // pass. The simulation is the world's, so a second monitor draws what the first one stepped
    // rather than stepping it again.
    void RecordFragmentSim(VkCommandBuffer cmd, float t, float dt) {
        const uint32_t count = fragmentLayout_.total;
        if (!count || !fragmentSet_) return;

        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, fragmentComputeLayout_, 0, 1,
                                &fragmentSet_, 0, nullptr);

        const FragmentPush push = BuildFragmentPush(t, dt);
        vkCmdPushConstants(cmd, fragmentComputeLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                           sizeof(push), &push);

        const uint32_t groups = (count + 63) / 64;

        if (!fragmentsInitialised_) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, fragmentInitPipeline_);
            vkCmdDispatch(cmd, groups, 1, 1);
            Barrier(cmd);
            fragmentsInitialised_ = true;
        }

        // Before the shell exists every fragment is intact, and an intact fragment's record is
        // the one the init pass just wrote. The simulation would read all of them and change none.
        if (world_.ShellRadius(t) <= 0.0f) return;

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, fragmentSimPipeline_);
        vkCmdDispatch(cmd, groups, 1, 1);
        Barrier(cmd);
    }

    // Compute writes, then the vertex stage reads. Without this the first frames of the blast draw
    // whatever the previous dispatch had got to, which on a tile-based part is anything at all.
    static void Barrier(VkCommandBuffer cmd) {
        VkMemoryBarrier barrier{};
        barrier.sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                                 VK_PIPELINE_STAGE_VERTEX_SHADER_BIT,
                             0, 1, &barrier, 0, nullptr, 0, nullptr);
    }

    void DrawMesh(VkCommandBuffer cmd, const Buffer& vertices, const Buffer& indices,
                  uint32_t indexCount) {
        if (!indexCount) return;

        const VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &vertices.handle, &offset);
        vkCmdBindIndexBuffer(cmd, indices.handle, 0, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(cmd, indexCount, 1, 0, 0, 0);
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
    app::Settings            settings_{};
    world::World             world_;

    // Where the current cycle began on the host's clock (spec 4.1). The host hands out seconds
    // since the screen saver started; a cycle is what the world is a function of, and there are
    // many cycles in a run.
    double cycleBase_ = 0.0;
    app::CaptureRequest      capture_ = app::CaptureRequestFromEnvironment();
    Buffer                   captureBuffer_{};
    int                      frameCounter_ = 0;
    RenderPasses             passes_{};
    std::vector<Attached>    windows_;

    VkDescriptorPool descriptorPool_ = VK_NULL_HANDLE;

    VkDescriptorSetLayout sceneSetLayout_      = VK_NULL_HANDLE;
    VkPipelineLayout      scenePipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline            skyPipeline_         = VK_NULL_HANDLE;
    VkPipeline            groundPipeline_      = VK_NULL_HANDLE;
    VkPipeline            buildingPipeline_    = VK_NULL_HANDLE;
    VkPipelineLayout      boardPipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline            boardPipeline_       = VK_NULL_HANDLE;

    Buffer   terrainVertices_{}, terrainIndices_{};
    Buffer   horizonVertices_{}, horizonIndices_{};
    uint32_t terrainIndexCount_ = 0;
    uint32_t horizonIndexCount_ = 0;

    // One cube, and the per-instance stream that turns it into a city (spec 6.4).
    Buffer   boxVertices_{}, boxIndices_{}, buildingInstances_{};
    uint32_t boxIndexCount_ = 0;
    uint32_t buildingCount_ = 0;

    Buffer   boardInstances_{};
    uint32_t boardBoxCount_ = 0;

    // The missile, the fireball and the flash (spec 7.1, 7.2). The fireball and the flash have no
    // geometry at all: one builds its sphere from gl_VertexIndex, the other is a fullscreen
    // triangle, so both are a pipeline and nothing else.
    VkPipelineLayout missilePipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline       missilePipeline_       = VK_NULL_HANDLE;
    VkPipeline       fireballPipeline_      = VK_NULL_HANDLE;
    VkPipeline       flashPipeline_         = VK_NULL_HANDLE;
    Buffer           missileVertices_{}, missileIndices_{};
    uint32_t         missileIndexCount_ = 0;

    // The fragment system (spec 7.3). One descriptor set for the whole renderer, because the
    // simulation belongs to the world rather than to a window.
    VkDescriptorSetLayout fragmentSetLayout_    = VK_NULL_HANDLE;
    VkPipelineLayout      fragmentComputeLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout      fragmentDrawLayout_   = VK_NULL_HANDLE;
    VkPipeline            fragmentInitPipeline_ = VK_NULL_HANDLE;
    VkPipeline            fragmentSimPipeline_  = VK_NULL_HANDLE;
    VkPipeline            fragmentPipeline_     = VK_NULL_HANDLE;
    VkDescriptorSet       fragmentSet_          = VK_NULL_HANDLE;

    // The particle systems (spec 8.3). No particle buffer: a particle is a closed-form function
    // of its index and the clock, so the only memory here is the per-window sort order, which
    // lives with the window. What is renderer-wide is the pipelines and how many slots there are.
    VkDescriptorSetLayout particleSetLayout_       = VK_NULL_HANDLE;
    VkPipelineLayout      particleComputeLayout_   = VK_NULL_HANDLE;
    VkPipelineLayout      particleDrawLayout_      = VK_NULL_HANDLE;
    VkPipeline            particleCountPipeline_   = VK_NULL_HANDLE;
    VkPipeline            particlePrefixPipeline_  = VK_NULL_HANDLE;
    VkPipeline            particleScatterPipeline_ = VK_NULL_HANDLE;
    VkPipeline            particleAlphaPipeline_   = VK_NULL_HANDLE;
    VkPipeline            particleAddPipeline_     = VK_NULL_HANDLE;
    uint32_t              particleTotal_           = 0;

    // Spec 11.2's auto-quality controller, and the fragment budget it is allowed to change only
    // at a cycle boundary.
    QualityController quality_;
    int               fragmentQuality_ = kQualityDefault;

    Buffer         shatterBoxes_{}, fragmentRest_{}, fragmentState_{};
    FragmentLayout fragmentLayout_{};
    bool           fragmentsInitialised_ = false;

    // The cycle time the frame being recorded belongs to. Stored rather than threaded through
    // RecordScene, so the board's digits and the scene uniforms cannot disagree about what time
    // it is — which for a countdown is the whole point.
    float frameTime_ = 0.0f;

    // The key light's shadow map (spec 8.2). One image, one framebuffer and one sampler for the
    // whole renderer: the sun does not vary per monitor, so unlike the exposure and the particle
    // sort this is not a per-window resource. The size is one of spec 11.2's quality levers and
    // moves only at a cycle reset, where the device is idle and nothing is in flight.
    VkFormat      shadowFormat_ = VK_FORMAT_UNDEFINED;
    uint32_t      shadowSize_   = 0;
    VkImage       shadowImage_  = VK_NULL_HANDLE;
    VmaAllocation shadowAlloc_  = VK_NULL_HANDLE;
    VkImageView   shadowView_   = VK_NULL_HANDLE;
    VkFramebuffer shadowFbo_    = VK_NULL_HANDLE;
    VkSampler     shadowSampler_ = VK_NULL_HANDLE;
    VkPipeline    shadowBuildingPipeline_ = VK_NULL_HANDLE;
    VkPipeline    shadowFragmentPipeline_ = VK_NULL_HANDLE;

    // The box the map is fitted to, settled with the world at each cycle reset.
    ShadowBounds  shadowBounds_{};

    VkSampler             sampler_               = VK_NULL_HANDLE;
    VkDescriptorSetLayout tonemapSetLayout_      = VK_NULL_HANDLE;
    VkPipelineLayout      tonemapPipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline            tonemapPipeline_       = VK_NULL_HANDLE;
    VkPipeline            histogramPipeline_     = VK_NULL_HANDLE;
    VkPipeline            adaptPipeline_         = VK_NULL_HANDLE;

    // The bloom chain (spec 8.1). One set per level per direction, allocated with the window
    // because the number of levels follows the monitor's size.
    VkDescriptorSetLayout bloomSetLayout_     = VK_NULL_HANDLE;
    VkPipelineLayout      bloomPipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline            bloomDownPipeline_  = VK_NULL_HANDLE;
    VkPipeline            bloomUpPipeline_    = VK_NULL_HANDLE;
};

// Validation is expensive and noisy, and a screen saver has no business loading a layer on a
// user's machine. It comes on only when debug logging is already on.
bool WantValidation() { return app::LogEnabled(); }

}  // namespace

std::unique_ptr<Renderer> CreateVulkanRenderer(const app::Settings& settings) {
    auto ctx = Context::Create(WantValidation());
    if (!ctx) return nullptr;

    auto renderer =
        std::make_unique<VulkanRenderer>(std::move(ctx), settings, world::Generate(settings));
    if (!renderer->Init()) {
        app::Log("vulkan: renderer init failed, falling back");
        return nullptr;
    }
    return renderer;
}

}  // namespace render
