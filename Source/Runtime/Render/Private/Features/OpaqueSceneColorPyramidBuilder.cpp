#include "Features/OpaqueSceneColorPyramidBuilder.h"
#include "Features/OpaqueSceneColorHistory.h"

#include "Core/Log.h"
#include "Render/PipelineCache.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <memory>
#include <string>

namespace RVX
{
namespace
{
    constexpr uint64 RVX_PYRAMID_CONSTANT_BUFFER_ALIGNMENT = 256;

    struct OpaqueSceneColorPyramidGPUConstants
    {
        float sourceIsFullResolution = 0.0f;
        float padding[3] = {0.0f, 0.0f, 0.0f};
    };

    struct OpaqueSceneColorPyramidPassData
    {
        RGTextureHandle source{};
        RGTextureHandle destination{};
        RGTextureViewHandle sourceView{};
        RGTextureViewHandle destinationView{};
        RenderOutputExtent destinationExtent{};
        uint32 mipIndex = 0;
        uint32 mipCount = 0;
        bool sourceIsFullResolution = false;
        OpaqueSceneColorHistory* temporalHistory = nullptr;
        std::shared_ptr<OpaqueSceneColorPyramidBuildReceipt> receipt{};
    };

    [[nodiscard]] uint64 AlignConstantBufferSize(uint64 size) noexcept
    {
        return (size + RVX_PYRAMID_CONSTANT_BUFFER_ALIGNMENT - 1u) &
            ~(RVX_PYRAMID_CONSTANT_BUFFER_ALIGNMENT - 1u);
    }

    [[nodiscard]] RenderOutputExtent CalculateMipExtent(
        RenderOutputExtent baseExtent,
        uint32 mipIndex) noexcept
    {
        RenderOutputExtent extent = baseExtent;
        for (uint32 mip = 0; mip < mipIndex; ++mip)
        {
            extent.width = std::max(1u, extent.width / 2u);
            extent.height = std::max(1u, extent.height / 2u);
        }
        return extent;
    }

    [[nodiscard]] RHIBufferRef CreateConstants(
        IRHIDevice& device,
        bool sourceIsFullResolution)
    {
        RHIBufferDesc description;
        description.size = AlignConstantBufferSize(
            sizeof(OpaqueSceneColorPyramidGPUConstants));
        description.usage = RHIBufferUsage::Constant;
        description.memoryType = RHIMemoryType::Upload;
        description.debugName = "OpaqueSceneColorPyramidConstants";
        RHIBufferRef constants = device.CreateBuffer(description);
        if (!constants)
        {
            return {};
        }

        void* const mapped = constants->Map();
        if (mapped == nullptr)
        {
            return {};
        }
        const OpaqueSceneColorPyramidGPUConstants values{
            sourceIsFullResolution ? 1.0f : 0.0f};
        std::memcpy(mapped, &values, sizeof(values));
        constants->Unmap();
        return constants;
    }

    [[nodiscard]] bool IsValidSource(
        const RHITextureDesc* description,
        RenderOutputExtent expectedExtent) noexcept
    {
        return description != nullptr &&
               description->format == RHIFormat::RGBA16_FLOAT &&
               description->width == expectedExtent.width &&
               description->height == expectedExtent.height &&
               description->sampleCount == RHISampleCount::Count1 &&
               HasFlag(description->usage, RHITextureUsage::ShaderResource);
    }
} // namespace

bool BuildOpaqueSceneColorPyramid(
    RenderFeatureGraphBuilder& builder,
    PipelineCache& pipelineCache,
    const RenderPassRecordContext& context,
    RenderFrameIdentity frameIdentity,
    RenderViewIdentity viewIdentity,
    RenderOutputExtent sourceExtent,
    RenderQualityTier quality,
    OpaqueSceneColorPyramid& outPyramid,
    OpaqueSceneColorPyramidRecordInput& outRecordInput,
    std::string& outFailure, OpaqueSceneColorHistory* temporalHistory)
{
    outPyramid = {};
    outRecordInput = {};
    outFailure.clear();

    const bool fullResolutionBase = quality == RenderQualityTier::Ultra;
    const RenderOutputExtent baseExtent = fullResolutionBase
        ? sourceExtent
        : RenderOutputExtent{
            std::max(1u, (sourceExtent.width + 1u) / 2u),
            std::max(1u, (sourceExtent.height + 1u) / 2u)};
    const uint32 mipCount =
        CalculateOpaqueSceneColorPyramidMipCount(baseExtent);
    RGTextureHandle source = context.view.colorTarget;
    const RHITextureDesc* const sourceDescription =
        builder.GetGraph().GetTextureDesc(source);
    if (!builder.IsValid() || !context.identity.IsValid() ||
        !frameIdentity.IsValid() || !viewIdentity.IsValid() ||
        !sourceExtent.IsValid() || !IsDeclaredRenderQualityTier(quality) ||
        !source.IsValid() ||
        !HasCurrentGraphProvenance(source, context.identity) ||
        !IsValidSource(sourceDescription, sourceExtent) || mipCount == 0 ||
        mipCount > OpaqueSceneColorPyramid::MaxMipCount ||
        pipelineCache.GetDevice() == nullptr ||
        pipelineCache.GetOpaqueSceneColorPyramidPipeline(
            RHIFormat::RGBA16_FLOAT) == nullptr ||
        pipelineCache.GetOpaqueSceneColorPyramidSetLayout() == nullptr ||
        !pipelineCache.GetEnvironmentIBLSamplerRef())
    {
        outFailure =
            "OpaqueSceneColorPyramid requires current RGBA16F HDR source and pyramid pipeline resources";
        return false;
    }

    const bool temporalRequested = context.view.currentTemporalJitter.IsValid();
    if (temporalRequested)
    {
        if (!temporalHistory)
        {
            outFailure = "Jittered opaque refraction requires composition-owned temporal history";
            return false;
        }
        if (!temporalHistory->Record(builder, context, frameIdentity, viewIdentity, source, outFailure))
            return false;
    }
    RHITextureDesc pyramidDescription = RHITextureDesc::RenderTarget(
        baseExtent.width, baseExtent.height, RHIFormat::RGBA16_FLOAT);
    pyramidDescription.usage = pyramidDescription.usage |
        RHITextureUsage::ShaderResource;
    pyramidDescription.mipLevels = mipCount;
    pyramidDescription.debugName = "OpaqueSceneColorPyramid";
    const RGTextureHandle pyramid = builder.CreateTexture(pyramidDescription);
    const std::shared_ptr<OpaqueSceneColorPyramidBuildReceipt> receipt =
        std::make_shared<OpaqueSceneColorPyramidBuildReceipt>();
    if (!pyramid.IsValid() || pyramid.index == source.index || !receipt)
    {
        outFailure =
            "OpaqueSceneColorPyramid could not allocate a distinct mip texture";
        return false;
    }

    for (uint32 mipIndex = 0; mipIndex < mipCount; ++mipIndex)
    {
        const RGTextureHandle sourceMip = mipIndex == 0
            ? source
            : pyramid.Subresource(mipIndex - 1u);
        const RGTextureHandle destinationMip = pyramid.Subresource(mipIndex);
        const RenderOutputExtent destinationExtent =
            CalculateMipExtent(baseExtent, mipIndex);
        const bool sourceIsFullResolution =
            mipIndex == 0 && fullResolutionBase;
        const std::string passName = mipIndex == 0
            ? "OpaqueSceneColorPyramidBase"
            : "OpaqueSceneColorPyramidMip" + std::to_string(mipIndex);
        if (!builder.AddPass<OpaqueSceneColorPyramidPassData>(
                passName.c_str(),
                RenderGraphPassType::Graphics,
                [sourceMip,
                 destinationMip,
                 destinationExtent,
                 mipIndex,
                 mipCount,
                 sourceIsFullResolution,
                 receipt, temporalRequested, temporalHistory](RenderGraphBuilder& graphBuilder,
                          OpaqueSceneColorPyramidPassData& data)
                {
                    // The texture-view declarations below are the sole graph
                    // accesses for these subresources. Retaining the raw
                    // handles only lets the execution callback verify view
                    // provenance without creating duplicate same-pass usage.
                    data.source = sourceMip;
                    data.temporalHistory = temporalRequested ? temporalHistory : nullptr;
                    data.destination = destinationMip;

                    const RHITextureDesc* const sourceDesc =
                        graphBuilder.GetTextureDesc(sourceMip);
                    const RHITextureDesc* const destinationDesc =
                        graphBuilder.GetTextureDesc(destinationMip);
                    if (sourceDesc != nullptr)
                    {
                        RHITextureViewDesc sourceViewDesc;
                        sourceViewDesc.format = sourceDesc->format;
                        sourceViewDesc.dimension = sourceDesc->dimension;
                        sourceViewDesc.subresourceRange =
                            sourceMip.subresourceRange;
                        sourceViewDesc.type = RHITextureViewType::ShaderResource;
                        sourceViewDesc.debugName =
                            "OpaqueSceneColorPyramidSourceSRV";
                        data.sourceView = graphBuilder.Read(
                            graphBuilder.CreateTextureView(
                                sourceMip, sourceViewDesc),
                            MakeRGAccessDesc(
                                RHIResourceState::ShaderResource,
                                RHIShaderStage::Pixel));
                    }
                    if (destinationDesc != nullptr)
                    {
                        RHITextureViewDesc destinationViewDesc;
                        destinationViewDesc.format = destinationDesc->format;
                        destinationViewDesc.dimension =
                            destinationDesc->dimension;
                        destinationViewDesc.subresourceRange =
                            destinationMip.subresourceRange;
                        destinationViewDesc.type =
                            RHITextureViewType::RenderTarget;
                        destinationViewDesc.debugName =
                            "OpaqueSceneColorPyramidRTV";
                        data.destinationView = graphBuilder.Write(
                            graphBuilder.CreateTextureView(
                                destinationMip, destinationViewDesc),
                            MakeRGAccessDesc(
                                RHIResourceState::RenderTarget,
                                RHIShaderStage::Pixel,
                                RHIDiscardIntent::Discard));
                    }
                    graphBuilder.SetExportState(
                        destinationMip, RHIResourceState::ShaderResource);
                    data.destinationExtent = destinationExtent;
                    data.mipIndex = mipIndex;
                    data.mipCount = mipCount;
                    data.sourceIsFullResolution = sourceIsFullResolution;
                    data.receipt = receipt;
                },
                [&pipelineCache](const OpaqueSceneColorPyramidPassData& data,
                                 RenderGraphPassContext& graphContext)
                {
                    const auto markFailed = [&data, &graphContext](
                        const char* diagnostic)
                    {
                        if (data.receipt != nullptr)
                        {
                            data.receipt->failed.store(
                                true, std::memory_order_release);
                        }
                        graphContext.MarkExecutionFailed(diagnostic);
                    };
                    if (data.receipt == nullptr ||
                        (data.temporalHistory && !data.temporalHistory->WasExecuted()) ||
                        data.receipt->failed.load(std::memory_order_acquire) ||
                        data.receipt->completedMipCount.load(
                            std::memory_order_acquire) != data.mipIndex)
                    {
                        markFailed(
                            "OpaqueSceneColorPyramid observed a failed or out-of-order predecessor mip");
                        return;
                    }

                    RHIPipeline* const pipeline =
                        pipelineCache.GetOpaqueSceneColorPyramidPipeline(
                            RHIFormat::RGBA16_FLOAT);
                    RHIDescriptorSetLayout* const setLayout =
                        pipelineCache.GetOpaqueSceneColorPyramidSetLayout();
                    IRHIDevice* const device = pipelineCache.GetDevice();
                    RHITextureView* const sourceView =
                        graphContext.GetTextureView(data.sourceView);
                    RHITextureView* const destinationView =
                        graphContext.GetTextureView(data.destinationView);
                    if (pipeline == nullptr || setLayout == nullptr ||
                        device == nullptr || sourceView == nullptr ||
                        destinationView == nullptr || !data.destinationExtent.IsValid() ||
                        data.receipt == nullptr)
                    {
                        markFailed(
                            "OpaqueSceneColorPyramid graph resources are unavailable");
                        return;
                    }

                    RHIBufferRef constants = CreateConstants(
                        *device, data.sourceIsFullResolution);
                    if (!constants ||
                        !graphContext.RetainSubmissionResource(
                            Ref<RefCounted>(constants)))
                    {
                        markFailed(
                            "OpaqueSceneColorPyramid constants allocation failed");
                        return;
                    }

                    RHIDescriptorSetDesc descriptorDesc;
                    descriptorDesc.layout = setLayout;
                    descriptorDesc.debugName =
                        "OpaqueSceneColorPyramidDescriptorSet";
                    descriptorDesc.BindBuffer(
                        0,
                        constants.Get(),
                        0,
                        AlignConstantBufferSize(
                            sizeof(OpaqueSceneColorPyramidGPUConstants)));
                    descriptorDesc.BindTexture(1, sourceView);
                    descriptorDesc.BindSampler(
                        2, pipelineCache.GetEnvironmentIBLSamplerRef().Get());
                    RHIDescriptorSetRef descriptorSet =
                        device->CreateDescriptorSet(descriptorDesc);
                    if (!descriptorSet ||
                        !graphContext.RetainSubmissionResource(
                            Ref<RefCounted>(descriptorSet)))
                    {
                        markFailed(
                            "OpaqueSceneColorPyramid descriptor creation failed");
                        return;
                    }

                    RHIRenderPassDesc renderPassDesc;
                    renderPassDesc.AddColorAttachment(
                        destinationView, RHILoadOp::DontCare, RHIStoreOp::Store);
                    renderPassDesc.SetRenderArea(
                        0,
                        0,
                        data.destinationExtent.width,
                        data.destinationExtent.height);
                    RHICommandContext& commands = graphContext.Commands();
                    commands.BeginRenderPass(renderPassDesc);
                    commands.SetPipeline(pipeline);
                    commands.SetDescriptorSet(0, descriptorSet.Get());
                    RHIViewport viewport;
                    viewport.width =
                        static_cast<float>(data.destinationExtent.width);
                    viewport.height =
                        static_cast<float>(data.destinationExtent.height);
                    viewport.minDepth = 0.0f;
                    viewport.maxDepth = 1.0f;
                    commands.SetViewport(viewport);
                    RHIRect scissor;
                    scissor.width = data.destinationExtent.width;
                    scissor.height = data.destinationExtent.height;
                    commands.SetScissor(scissor);
                    commands.Draw(3, 1, 0, 0);
                    commands.EndRenderPass();

                    const uint32 completedMipCount = data.mipIndex + 1u;
                    data.receipt->completedMipCount.store(
                        completedMipCount, std::memory_order_release);
                    if (completedMipCount == data.mipCount)
                    {
                        if (!data.receipt->failed.load(
                                std::memory_order_acquire))
                        {
                            data.receipt->completed.store(
                                true, std::memory_order_release);
                        }
                    }
                }))
        {
            outFailure =
                "OpaqueSceneColorPyramid graph pass registration failed";
            return false;
        }
    }

    const SceneTextures::Metadata metadata{
        baseExtent, RHIFormat::RGBA16_FLOAT, RHISampleCount::Count1};
    outPyramid.provenance = builder.GetProvenance();
    outPyramid.frameIdentity = frameIdentity;
    outPyramid.viewIdentity = viewIdentity;
    outPyramid.sourceExtent = sourceExtent;
    outPyramid.baseExtent = baseExtent;
    outPyramid.rasterToColorUV = temporalRequested
        ? Vec2(context.view.currentTemporalJitter.pixelOffset.x / sourceExtent.width,
               -context.view.currentTemporalJitter.pixelOffset.y / sourceExtent.height)
        : Vec2(0.0f);
    outPyramid.quality = quality;
    outPyramid.mipCount = mipCount;
    outPyramid.texture = pyramid;
    outPyramid.textureMetadata = metadata;
    outPyramid.receipt = receipt;

    outRecordInput.identity = context.identity;
    outRecordInput.texture = pyramid;
    outRecordInput.sourceExtent = sourceExtent;
    outRecordInput.baseExtent = baseExtent;
    outRecordInput.rasterToColorUV = outPyramid.rasterToColorUV;
    outRecordInput.quality = quality;
    outRecordInput.mipCount = mipCount;
    outRecordInput.receipt = receipt;
    return true;
}
} // namespace RVX
