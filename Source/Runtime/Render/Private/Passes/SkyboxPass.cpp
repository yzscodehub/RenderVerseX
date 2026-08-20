/**
 * @file SkyboxPass.cpp
 * @brief Recording-isolated skybox render pass implementation.
 */

#include "Render/Passes/SkyboxPass.h"

#include "Core/Log.h"
#include "Render/PipelineCache.h"
#include "Render/Renderer/ViewData.h"
#include "Resources/RenderResourceRegistry.h"
#include "RHI/RHIRenderPass.h"

#include <algorithm>
#include <cstring>
#include <memory>

namespace RVX
{
namespace
{
    constexpr uint64 RVX_SKYBOX_CONSTANT_BUFFER_ALIGNMENT = 256;

    bool PrepareVulkanSampledTexture(IRHIDevice* device, RHITexture* texture)
    {
        if (!device || !texture ||
            device->GetBackendType() != RHIBackendType::Vulkan)
        {
            return device != nullptr && texture != nullptr;
        }

        RHICommandContextRef context =
            device->CreateCommandContext(RHICommandQueueType::Graphics);
        if (!context)
        {
            return !device->GetCapabilities().supportsExplicitResourceBarriers;
        }
        context->Begin();
        context->TextureBarrier(texture,
                                RHIResourceState::Undefined,
                                RHIResourceState::ShaderResource);
        context->End();
        device->SubmitCommandContext(context.Get());
        device->WaitIdle();
        return device->QueryRuntimeStatus() == RHIDeviceRuntimeStatus::Ready;
    }

    uint64 AlignSkyboxConstantBufferSize(uint64 size)
    {
        return (size + RVX_SKYBOX_CONSTANT_BUFFER_ALIGNMENT - 1) &
               ~(RVX_SKYBOX_CONSTANT_BUFFER_ALIGNMENT - 1);
    }

    struct SkyboxGPUConstants
    {
        float zenithColor[4] = {0.4f, 0.6f, 1.0f, 1.0f};
        float horizonColor[4] = {0.8f, 0.85f, 0.9f, 1.0f};
        float groundColor[4] = {0.3f, 0.25f, 0.2f, 0.999f};
        float sunDirection[4] = {0.5f, 0.5f, 0.5f, 1.0f};
        float sunColor[4] = {1.0f, 0.95f, 0.9f, 0.0f};
        float textureParams[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        float cameraPosition[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        Mat4 inverseViewProjection = Mat4Identity();
    };

    struct GraphPassData
    {
        RenderPassExecutionData execution{};
        RenderPassRecordIdentity identity{};
        std::shared_ptr<const RenderPassFrameSnapshot> frameSnapshot;
        const RenderResourceRegistry* resourceRegistry = nullptr;
        IRHIDevice* device = nullptr;
        RHIPipelineRef pipeline;
        RHIPipelineLayoutRef pipelineLayout;
        RHIDescriptorSetLayoutRef setLayout;
        RHIBufferRef constantBuffer;
        RHITextureRef cubemapTexture;
        RHISamplerRef sampler;
        RHITextureRef fallbackCubemap;
        RGTextureHandle skyTextureHandle{};
        RGTextureHandle colorHandle{};
        RGTextureHandle depthHandle{};
        RGTextureViewHandle skyTextureViewHandle{};
        RGTextureViewHandle colorViewHandle{};
        RGTextureViewHandle depthViewHandle{};
        bool depthAvailable = false;
        bool usingCubemap = false;
        bool reverseZ = false;
        bool requestedEnabled = false;
        bool supported = false;
        bool contextValid = false;
    };

    [[nodiscard]] bool RetainResource(RenderGraphBuilder& builder,
                                      RefCounted* resource)
    {
        return resource == nullptr ||
               builder.RetainSubmissionResource(Ref<RefCounted>(resource));
    }

    [[nodiscard]] bool RetainRecordingResources(
        RenderGraphBuilder& builder,
        const GraphPassData& data)
    {
        return RetainResource(builder, data.constantBuffer.Get()) &&
               RetainResource(builder, data.sampler.Get()) &&
               RetainResource(builder, data.cubemapTexture.Get()) &&
               RetainResource(builder, data.pipeline.Get()) &&
               RetainResource(builder, data.pipelineLayout.Get()) &&
               RetainResource(builder, data.setLayout.Get());
    }

    [[nodiscard]] bool ResolveSkyTexture(GraphPassData& data,
                                         RenderGraphBuilder& builder)
    {
        data.cubemapTexture = data.fallbackCubemap;
        data.usingCubemap = false;
        data.skyTextureHandle = builder.ImportTexture(
            data.cubemapTexture, RHIResourceState::ShaderResource);

        if (!data.frameSnapshot ||
            data.frameSnapshot->sky.mode != RenderSkyMode::Cubemap ||
            !data.frameSnapshot->sky.skyTexture.IsValid() ||
            data.resourceRegistry == nullptr)
        {
            return data.cubemapTexture && data.skyTextureHandle.IsValid();
        }

        RHITexture* texture = data.resourceRegistry->ResolveTextureObject(
            data.frameSnapshot->sky.skyTexture);
        if (texture == nullptr || texture->GetDimension() != RHITextureDimension::TextureCube)
        {
            // A packet can legitimately name a texture that is not ready for
            // this exact registry generation. Preserve the established solid
            // tint/intensity fallback instead of borrowing pass setter state.
            return data.cubemapTexture && data.skyTextureHandle.IsValid();
        }

        const RGTextureHandle skyTextureHandle =
            data.execution.view.environmentSkyTexture;
        if (!skyTextureHandle.IsValid() || data.identity.graph == nullptr)
        {
            // The descriptor and RenderGraph access must identify the same
            // frame-owned texture. Fall back when that ownership is absent.
            return data.cubemapTexture && data.skyTextureHandle.IsValid();
        }

        data.cubemapTexture = RHITextureRef(texture);
        data.skyTextureHandle = skyTextureHandle;
        data.usingCubemap = true;
        return true;
    }

    [[nodiscard]] bool CreateRecordConstants(GraphPassData& data)
    {
        if (data.device == nullptr || !data.frameSnapshot)
        {
            return false;
        }

        RHIBufferDesc bufferDesc;
        bufferDesc.size = AlignSkyboxConstantBufferSize(sizeof(SkyboxGPUConstants));
        bufferDesc.usage = RHIBufferUsage::Constant;
        bufferDesc.memoryType = RHIMemoryType::Upload;
        bufferDesc.debugName = "SkyboxRecordConstants";
        data.constantBuffer = data.device->CreateBuffer(bufferDesc);
        if (!data.constantBuffer)
        {
            return false;
        }

        const RenderSkySnapshot& sky = data.frameSnapshot->sky;
        const ViewData& view = data.execution.view;
        const float exposure = std::max(0.0f, sky.intensity);
        const float farDepth = data.reverseZ ? 0.001f : 0.999f;
        SkyboxGPUConstants constants;

        const auto setPureTintFallback = [&constants, &sky, exposure, farDepth]
        {
            constants.zenithColor[0] = sky.tint.x;
            constants.zenithColor[1] = sky.tint.y;
            constants.zenithColor[2] = sky.tint.z;
            constants.zenithColor[3] = exposure;
            constants.horizonColor[0] = sky.tint.x;
            constants.horizonColor[1] = sky.tint.y;
            constants.horizonColor[2] = sky.tint.z;
            constants.horizonColor[3] = 0.0f;
            constants.groundColor[0] = sky.tint.x;
            constants.groundColor[1] = sky.tint.y;
            constants.groundColor[2] = sky.tint.z;
            constants.groundColor[3] = farDepth;
            constants.sunDirection[0] = 0.0f;
            constants.sunDirection[1] = 1.0f;
            constants.sunDirection[2] = 0.0f;
            constants.sunDirection[3] = 0.0f;
            constants.sunColor[0] = 0.0f;
            constants.sunColor[1] = 0.0f;
            constants.sunColor[2] = 0.0f;
            constants.textureParams[0] = 0.0f;
            constants.textureParams[1] = 0.0f;
            constants.textureParams[2] = 0.0f;
        };

        if (sky.mode == RenderSkyMode::Cubemap && data.usingCubemap)
        {
            constants.zenithColor[3] = exposure;
            constants.groundColor[3] = farDepth;
            constants.textureParams[0] = 1.0f;
            constants.textureParams[1] = std::max(0.0f, sky.blurLevel);
            constants.textureParams[2] = sky.rotationRadians;
        }
        else if (sky.mode == RenderSkyMode::Procedural)
        {
            constants.zenithColor[0] = sky.zenithColor.x;
            constants.zenithColor[1] = sky.zenithColor.y;
            constants.zenithColor[2] = sky.zenithColor.z;
            constants.zenithColor[3] = exposure;
            constants.horizonColor[0] = sky.horizonColor.x;
            constants.horizonColor[1] = sky.horizonColor.y;
            constants.horizonColor[2] = sky.horizonColor.z;
            constants.horizonColor[3] = std::max(0.0f, sky.scatteringIntensity);
            constants.groundColor[0] = sky.groundColor.x;
            constants.groundColor[1] = sky.groundColor.y;
            constants.groundColor[2] = sky.groundColor.z;
            constants.groundColor[3] = farDepth;
            constants.sunDirection[0] = sky.sunDirection.x;
            constants.sunDirection[1] = sky.sunDirection.y;
            constants.sunDirection[2] = sky.sunDirection.z;
            constants.sunDirection[3] = 1.0f;
            constants.sunColor[0] = sky.sunColor.x;
            constants.sunColor[1] = sky.sunColor.y;
            constants.sunColor[2] = sky.sunColor.z;
            constants.textureParams[0] = 0.0f;
            constants.textureParams[1] = 0.0f;
            constants.textureParams[2] = 0.0f;
        }
        else
        {
            // SolidColor plus unavailable Cubemap/Equirectangular inputs are
            // packet-owned tint/intensity fallbacks. Never consult setter
            // state here: it may describe a later frame or another graph.
            setPureTintFallback();
        }
        constants.cameraPosition[0] = view.cameraPosition.x;
        constants.cameraPosition[1] = view.cameraPosition.y;
        constants.cameraPosition[2] = view.cameraPosition.z;
        constants.inverseViewProjection =
            view.inverseViewMatrix * view.inverseProjectionMatrix;

        void* mapped = data.constantBuffer->Map();
        if (mapped == nullptr)
        {
            return false;
        }
        std::memcpy(mapped, &constants, sizeof(constants));
        return data.constantBuffer->CommitMappedWrite();
    }

    [[nodiscard]] bool PrepareRecording(GraphPassData& data,
                                        RenderGraphBuilder& builder)
    {
        if (!data.contextValid || !data.frameSnapshot || !data.requestedEnabled ||
            !data.supported || !data.device ||
            !data.fallbackCubemap || !data.sampler)
        {
            return false;
        }

        if (data.frameSnapshot->sky.mode == RenderSkyMode::Disabled)
        {
            return false;
        }

        const ViewData& view = data.execution.view;
        if (!view.AllowsSkybox())
        {
            return false;
        }
        if (!view.colorTarget.IsValid())
        {
            return false;
        }
        const RHITextureDesc* colorDesc =
            builder.GetTextureDesc(view.colorTarget);
        if (colorDesc == nullptr)
        {
            return false;
        }
        if (view.depthTarget.IsValid() &&
            builder.GetTextureDesc(view.depthTarget) == nullptr)
        {
            return false;
        }
        data.depthAvailable = view.depthTarget.IsValid();

        if (!data.pipeline || !data.pipelineLayout || !data.setLayout)
        {
            return false;
        }

        if (!ResolveSkyTexture(data, builder) || !CreateRecordConstants(data))
        {
            return false;
        }

        // Graph declarations cannot be rolled back. Establish the entire
        // submission lifetime first; a sealed/rejected production batch is a
        // genuine no-op rather than a graph-visible attachment access.
        if (!RetainRecordingResources(builder, data))
        {
            return false;
        }

        if (const RHITextureDesc* skyDesc =
                builder.GetTextureDesc(data.skyTextureHandle))
        {
            RHITextureViewDesc viewDesc;
            viewDesc.format = skyDesc->format;
            viewDesc.dimension = RHITextureDimension::TextureCube;
            viewDesc.subresourceRange = RHISubresourceRange::All();
            viewDesc.type = RHITextureViewType::ShaderResource;
            viewDesc.debugName = "SkyboxCubemapSRV";
            data.skyTextureViewHandle = builder.CreateTextureView(
                data.skyTextureHandle, viewDesc);
            data.skyTextureViewHandle = builder.Read(
                data.skyTextureViewHandle,
                MakeRGAccessDesc(
                    RHIResourceState::ShaderResource,
                    RHIShaderStage::Pixel));
        }
        if (!data.skyTextureViewHandle.IsValid())
            return false;

        data.colorHandle = view.colorTarget;
        RHITextureViewDesc colorViewDesc;
        colorViewDesc.format = colorDesc->format;
        colorViewDesc.dimension = colorDesc->dimension;
        colorViewDesc.subresourceRange = RHISubresourceRange::All();
        colorViewDesc.type = RHITextureViewType::RenderTarget;
        colorViewDesc.debugName = "SkyboxColorRTV";
        data.colorViewHandle = builder.CreateTextureView(
            data.colorHandle, colorViewDesc);
        data.colorViewHandle = builder.ReadWrite(
            data.colorViewHandle,
            MakeRGAccessDesc(
                RHIResourceState::RenderTarget,
                RHIShaderStage::Pixel));
        if (!data.colorViewHandle.IsValid())
        {
            return false;
        }
        if (data.depthAvailable)
        {
            data.depthHandle = view.depthTarget;
            const RHITextureDesc* depthDesc =
                builder.GetTextureDesc(data.depthHandle);
            if (!depthDesc)
            {
                data.colorHandle = {};
                return false;
            }
            RHITextureViewDesc depthViewDesc;
            depthViewDesc.format = depthDesc->format;
            depthViewDesc.dimension = depthDesc->dimension;
            depthViewDesc.subresourceRange = RHISubresourceRange::All();
            depthViewDesc.subresourceRange.aspect = RHITextureAspect::Depth;
            depthViewDesc.type = RHITextureViewType::DepthStencil;
            depthViewDesc.debugName = "SkyboxDepthDSV";
            data.depthViewHandle = builder.CreateTextureView(
                data.depthHandle, depthViewDesc);
            data.depthViewHandle = builder.Read(
                data.depthViewHandle,
                MakeRGAccessDesc(
                    RHIResourceState::DepthRead,
                    RHIShaderStage::Vertex | RHIShaderStage::Pixel));
            if (!data.depthViewHandle.IsValid())
                return false;
        }
        return true;
    }

    void ExecuteRecording(const GraphPassData& data,
                          RenderGraphPassContext& context)
    {
        RHICommandContext& ctx = context.Commands();
        if (!data.contextValid || !data.frameSnapshot || !data.identity.IsValid() ||
            data.identity.graph == nullptr ||
            !data.identity.Matches(*data.identity.graph) ||
            !data.colorHandle.IsValid() || !data.pipeline ||
            !data.pipelineLayout || !data.setLayout || !data.constantBuffer ||
            !data.cubemapTexture || !data.skyTextureViewHandle.IsValid() ||
            !data.sampler ||
            data.colorHandle.graphIdentity != data.identity.graphIdentity ||
            data.colorHandle.recordingGeneration !=
                data.identity.graphRecordingGeneration ||
            (data.usingCubemap &&
             (!data.skyTextureHandle.IsValid() ||
              data.skyTextureHandle.graphIdentity != data.identity.graphIdentity ||
              data.skyTextureHandle.recordingGeneration !=
                  data.identity.graphRecordingGeneration)) ||
            (data.depthAvailable &&
             (!data.depthHandle.IsValid() ||
              data.depthHandle.graphIdentity != data.identity.graphIdentity ||
              data.depthHandle.recordingGeneration !=
                  data.identity.graphRecordingGeneration)))
        {
            return;
        }

        RHITexture* colorTexture = context.GetTexture(data.colorHandle);
        RHITexture* depthTexture = data.depthAvailable
            ? context.GetTexture(data.depthHandle) : nullptr;
        if (colorTexture == nullptr || (data.depthAvailable && depthTexture == nullptr))
        {
            return;
        }

        RHITextureView* colorView =
            context.GetTextureView(data.colorViewHandle);
        RHITextureView* depthView = data.depthAvailable
            ? context.GetTextureView(data.depthViewHandle) : nullptr;
        RHITextureView* skyView =
            context.GetTextureView(data.skyTextureViewHandle);
        if (!colorView || !skyView || (data.depthAvailable && !depthView))
        {
            return;
        }

        RHIDescriptorSetDesc descriptorDesc;
        descriptorDesc.layout = data.setLayout.Get();
        descriptorDesc.debugName = "SkyboxRecordDescriptorSet";
        descriptorDesc.BindBuffer(0,
                                  data.constantBuffer.Get(),
                                  0,
                                  AlignSkyboxConstantBufferSize(
                                      sizeof(SkyboxGPUConstants)));
        descriptorDesc.BindTexture(1, skyView);
        descriptorDesc.BindSampler(2, data.sampler.Get());
        RHIDescriptorSetRef descriptorSet =
            data.device->CreateDescriptorSet(descriptorDesc);
        if (!descriptorSet ||
            !descriptorSet->IsReadyForBinding(data.setLayout.Get()) ||
            !context.RetainSubmissionResource(
                Ref<RefCounted>(descriptorSet)))
        {
            return;
        }

        RHIRenderPassDesc renderPassDesc;
        renderPassDesc.AddColorAttachment(
            colorView, RHILoadOp::Load, RHIStoreOp::Store);
        if (depthView)
        {
            renderPassDesc.SetDepthStencil(
                depthView, RHILoadOp::Load, RHIStoreOp::Store, 1.0f, 0);
            renderPassDesc.depthStencilAttachment.readOnly = true;
        }
        renderPassDesc.SetRenderArea(
            0, 0, colorTexture->GetWidth(), colorTexture->GetHeight());

        ctx.BeginRenderPass(renderPassDesc);
        ctx.SetPipeline(data.pipeline.Get());
        ctx.SetDescriptorSet(0, descriptorSet.Get());
        ctx.SetViewport(data.execution.view.GetRHIViewport());
        ctx.SetScissor(data.execution.view.GetRHIScissor());
        ctx.Draw(3, 1, 0, 0);
        ctx.EndRenderPass();
    }
} // namespace

SkyboxPass::SkyboxPass() = default;

void SkyboxPass::SetResources(PipelineCache* pipelineCache)
{
    m_pipelineCache = pipelineCache;

    IRHIDevice* device = m_pipelineCache ? m_pipelineCache->GetDevice() : nullptr;
    if (device != m_resourceDevice)
    {
        m_fallbackCubemap.Reset();
        m_sampler.Reset();
        m_resourceDevice = device;
    }

    RefreshSupport();
}

void SkyboxPass::RefreshSupport()
{
    m_drawReady = false;

    if (!m_pipelineCache || !m_pipelineCache->IsInitialized())
    {
        m_unsupportedReason = "Skybox requires an initialized PipelineCache";
        return;
    }
    if (!m_pipelineCache->GetSkyboxPipeline() ||
        !m_pipelineCache->GetSkyboxLayout() ||
        !m_pipelineCache->GetSkyboxSetLayout())
    {
        m_unsupportedReason = "Skybox pipeline resources are not available";
        return;
    }
    if (!EnsureRuntimeResources())
    {
        return;
    }

    m_drawReady = true;
    m_unsupportedReason.clear();
}

void SkyboxPass::Setup(RenderGraphBuilder& builder, const ViewData& view)
{
    (void)builder;
    (void)view;
    // Legacy Setup must stay inert: typed AddToGraph owns all declarations.
}

void SkyboxPass::Execute(RHICommandContext& ctx, const ViewData& view)
{
    (void)ctx;
    (void)view;
    // Legacy Execute must stay inert: typed GraphPassData owns all state.
}

void SkyboxPass::AddToGraph(RenderGraph& graph, const ViewData& view)
{
    AddToGraph(graph, MakeRenderPassRecordContext(graph, view));
}

void SkyboxPass::AddToGraph(RenderGraph& graph,
                            const RenderPassRecordContext& context)
{
    // Do not call MakeRenderPassExecutionData until this supplied source has
    // been proven to be the exact graph's already-paired frame snapshot and
    // results. The helper can initialize results, which must never touch a
    // foreign or forged recording before rejection.
    const bool sourceContextValid = !context.legacyAdapter &&
        context.MatchesTargetGraph(graph) && context.IsFrameIdentityValid() &&
        context.frameSnapshot != nullptr && context.results != nullptr &&
        context.frameSnapshot->identity == context.identity &&
        context.results->identity == context.identity;
    const bool suppliedResultsCompatible =
        context.results == nullptr ||
        context.results->identity == RenderPassRecordIdentity{} ||
        context.results->identity == context.identity;
    const bool suppliedSnapshotCompatible =
        context.frameSnapshot == nullptr ||
        context.frameSnapshot->identity == context.identity;

    RenderPassExecutionData execution;
    if (sourceContextValid && suppliedResultsCompatible &&
        suppliedSnapshotCompatible)
    {
        execution = MakeRenderPassExecutionData(context);
    }
    else
    {
        execution.view = context.view;
        execution.identity = context.identity;
    }

    const bool colorValid = execution.view.colorTarget.IsValid() &&
        HasCurrentGraphProvenance(execution.view.colorTarget, execution.identity) &&
        graph.GetTextureDesc(execution.view.colorTarget) != nullptr;
    const bool depthValid = !execution.view.depthTarget.IsValid() ||
        (HasCurrentGraphProvenance(execution.view.depthTarget, execution.identity) &&
         graph.GetTextureDesc(execution.view.depthTarget) != nullptr);
    const bool contextValid = !context.legacyAdapter &&
        context.MatchesTargetGraph(graph) && context.IsFrameIdentityValid() &&
        execution.MatchesTargetGraph(graph) && execution.IsFrameIdentityValid() &&
        execution.frameSnapshot != nullptr && execution.results != nullptr &&
        colorValid && depthValid;
    const bool resultOwnershipValid = execution.identity.Matches(graph) &&
        execution.results != nullptr &&
        execution.results->identity == execution.identity &&
        execution.frameSnapshot != nullptr &&
        execution.frameSnapshot->identity == execution.identity;

    const bool requestedEnabled = m_enabled;
    const bool supported = m_drawReady;
    PipelineCache* const pipelineCache = m_pipelineCache;
    const RenderResourceRegistry* const resourceRegistry = m_resourceRegistry;
    IRHIDevice* const device = pipelineCache ? pipelineCache->GetDevice() : nullptr;
    const RHITextureRef fallbackCubemap = m_fallbackCubemap;
    const RHISamplerRef sampler = m_sampler;
    const std::shared_ptr<const RenderPassFrameSnapshot> frameSnapshot =
        resultOwnershipValid ? execution.frameSnapshot : nullptr;

    graph.AddPass<GraphPassData>(
        GetName(),
        GetPassType(),
        [execution,
         contextValid,
         requestedEnabled,
         supported,
         pipelineCache,
         resourceRegistry,
         device,
         fallbackCubemap,
         sampler,
         frameSnapshot](RenderGraphBuilder& builder, GraphPassData& data)
        {
            data.execution = execution;
            data.identity = execution.identity;
            data.contextValid = contextValid;
            data.requestedEnabled = requestedEnabled;
            data.supported = supported;
            data.resourceRegistry = resourceRegistry;
            data.device = device;
            data.fallbackCubemap = fallbackCubemap;
            data.sampler = sampler;
            data.frameSnapshot = frameSnapshot;
            data.reverseZ = pipelineCache != nullptr &&
                pipelineCache->GetConfig().reverseZ;

            if (pipelineCache != nullptr && data.contextValid && data.frameSnapshot &&
                data.execution.view.colorTarget.IsValid())
            {
                const RHITextureDesc* colorDesc = builder.GetTextureDesc(
                    data.execution.view.colorTarget);
                const bool hasDepth = data.execution.view.depthTarget.IsValid();
                if (colorDesc != nullptr)
                {
                    data.pipeline = RHIPipelineRef(pipelineCache->GetSkyboxPipeline(
                        colorDesc->format, hasDepth));
                    data.pipelineLayout = RHIPipelineLayoutRef(
                        pipelineCache->GetSkyboxLayout());
                    data.setLayout = RHIDescriptorSetLayoutRef(
                        pipelineCache->GetSkyboxSetLayout());
                }
            }

            if (!PrepareRecording(data, builder))
            {
                data.colorHandle = {};
                data.depthHandle = {};
                data.depthAvailable = false;
                data.constantBuffer.Reset();
                data.cubemapTexture.Reset();
                data.pipeline.Reset();
                data.pipelineLayout.Reset();
                data.setLayout.Reset();
            }
        },
        [](const GraphPassData& data, RenderGraphPassContext& context)
        {
            ExecuteRecording(data, context);
        });
}

bool SkyboxPass::EnsureRuntimeResources()
{
    IRHIDevice* device = m_pipelineCache ? m_pipelineCache->GetDevice() : nullptr;
    if (!device)
    {
        m_unsupportedReason = "Skybox requires an RHI device";
        return false;
    }

    if (!m_sampler)
    {
        RHISamplerDesc samplerDesc = RHISamplerDesc::LinearWrap();
        samplerDesc.debugName = "SkyboxLinearWrapSampler";
        m_sampler = device->CreateSampler(samplerDesc);
        if (!m_sampler)
        {
            m_unsupportedReason = "Skybox sampler creation failed";
            return false;
        }
    }

    if (!m_fallbackCubemap)
    {
        RHITextureDesc fallbackDesc =
            RHITextureDesc::Texture2D(1, 1, RHIFormat::RGBA8_UNORM);
        fallbackDesc.dimension = RHITextureDimension::TextureCube;
        fallbackDesc.arraySize = 1;
        fallbackDesc.usage = RHITextureUsage::ShaderResource;
        fallbackDesc.debugName = "SkyboxFallbackCubemap";
        m_fallbackCubemap = device->CreateTexture(fallbackDesc);
        if (!m_fallbackCubemap ||
            !PrepareVulkanSampledTexture(device, m_fallbackCubemap.Get()))
        {
            m_unsupportedReason = "Skybox fallback cubemap creation failed";
            return false;
        }
    }

    return true;
}

} // namespace RVX
