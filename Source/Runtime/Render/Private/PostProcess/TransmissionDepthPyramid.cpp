#include "Render/PostProcess/TransmissionDepthPyramid.h"

#include "Render/PipelineCache.h"
#include "RenderContracts/TransmissionFootprint.h"
#include "RHI/RHIRenderPass.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <memory>
#include <string>

namespace RVX
{
namespace
{
    constexpr uint64 RVX_TRANSMISSION_DEPTH_CONSTANT_ALIGNMENT = 256;

    struct TransmissionDepthPyramidGPUConstants
    {
        float sourceIsFullResolution = 0.0f;
        float reverseZ = 0.0f;
        float sourceCanonical = 0.0f;
        float padding = 0.0f;
    };

    struct TransmissionDepthPyramidPassData
    {
        RGTextureHandle source{};
        RGTextureHandle destination{};
        RGTextureViewHandle sourceView{};
        RGTextureViewHandle destinationView{};
        RenderOutputExtent destinationExtent{};
        uint32 mipIndex = 0;
        uint32 mipCount = 0;
        bool sourceIsFullResolution = false;
        bool reverseZ = false;
        bool sourceCanonical = false;
        IRHIDevice* device = nullptr;
        RHIPipelineRef pipeline{};
        RHIDescriptorSetLayoutRef setLayout{};
        RHISamplerRef sampler{};
        std::shared_ptr<SceneDepthSnapshotCopyReceipt> sourceCopyReceipt{};
        std::shared_ptr<TransmissionDepthPyramidBuildReceipt> receipt{};
    };

    [[nodiscard]] uint64 AlignConstantBufferSize(uint64 size) noexcept
    {
        return (size + RVX_TRANSMISSION_DEPTH_CONSTANT_ALIGNMENT - 1u) &
            ~(RVX_TRANSMISSION_DEPTH_CONSTANT_ALIGNMENT - 1u);
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
        bool sourceIsFullResolution,
        bool reverseZ,
        bool sourceCanonical)
    {
        RHIBufferDesc description;
        description.size = AlignConstantBufferSize(
            sizeof(TransmissionDepthPyramidGPUConstants));
        description.usage = RHIBufferUsage::Constant;
        description.memoryType = RHIMemoryType::Upload;
        description.debugName = "TransmissionDepthPyramidConstants";
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
        const TransmissionDepthPyramidGPUConstants values{
            sourceIsFullResolution ? 1.0f : 0.0f,
            reverseZ ? 1.0f : 0.0f,
            sourceCanonical ? 1.0f : 0.0f};
        std::memcpy(mapped, &values, sizeof(values));
        constants->Unmap();
        return constants;
    }

    [[nodiscard]] bool IsFullFrameDepthSnapshot(
        const RHITextureDesc* description,
        RenderOutputExtent expectedExtent) noexcept
    {
        return description != nullptr &&
               description->format == RHIFormat::D32_FLOAT &&
               description->dimension == RHITextureDimension::Texture2D &&
               description->arraySize == 1 && description->mipLevels == 1 &&
               description->depth == 1 &&
               description->width == expectedExtent.width &&
               description->height == expectedExtent.height &&
               description->sampleCount == RHISampleCount::Count1 &&
               HasFlag(description->usage, RHITextureUsage::ShaderResource);
    }

    [[nodiscard]] RenderPassRecordIdentity MakeExpectedIdentity(
        const RenderFeatureGraphBuilder& builder) noexcept
    {
        const RenderGraphBlackboardProvenance& provenance =
            builder.GetProvenance();
        return {const_cast<RenderGraph*>(&builder.GetGraph()),
                builder.GetGraphIdentity(),
                builder.GetRecordingGeneration(),
                provenance.frameSequence,
                provenance.viewOrdinal,
                provenance.recordEpoch};
    }
} // namespace

bool BuildTransmissionDepthPyramid(
    RenderFeatureGraphBuilder& builder,
    PipelineCache& pipelineCache,
    const SceneDepthSnapshotRecordInput& source,
    RenderQualityTier quality,
    bool reverseZ,
    TransmissionDepthPyramidOutput& outPyramid,
    std::string& outFailure)
{
    outPyramid = {};
    outFailure.clear();

    if (!builder.IsValid() || !source.extent.IsValid() ||
        source.extent.width > RVX_TRANSMISSION_FOOTPRINT_MAX_EXTENT ||
        source.extent.height > RVX_TRANSMISSION_FOOTPRINT_MAX_EXTENT)
    {
        outFailure = "TransmissionDepthPyramid requires a live render graph";
        return false;
    }

    const RenderPassRecordIdentity expectedIdentity =
        MakeExpectedIdentity(builder);
    const bool fullResolutionBase = quality == RenderQualityTier::Ultra;
    const RenderOutputExtent baseExtent = fullResolutionBase
        ? source.extent
        : RenderOutputExtent{
              std::max(1u, (source.extent.width + 1u) / 2u),
              std::max(1u, (source.extent.height + 1u) / 2u)};
    const uint32 mipCount = CalculateOpaqueSceneColorPyramidMipCount(
        baseExtent);
    const RHITextureDesc* const sourceDescription =
        builder.GetGraph().GetTextureDesc(source.texture);
    RHIPipelineRef pipeline(
        pipelineCache.GetTransmissionDepthPyramidPipeline());
    RHIDescriptorSetLayoutRef setLayout(
        pipelineCache.GetOpaqueSceneColorPyramidSetLayout());
    const RHISamplerRef sampler = pipelineCache.GetEnvironmentIBLSamplerRef();
    IRHIDevice* const device = pipelineCache.GetDevice();

    if (!expectedIdentity.IsValid() ||
        !source.IsCompatibleWith(expectedIdentity, source.extent) ||
        source.texture.hasSubresourceRange ||
        !IsFullFrameDepthSnapshot(sourceDescription, source.extent) ||
        !IsDeclaredRenderQualityTier(quality) || !baseExtent.IsValid() ||
        mipCount == 0 ||
        mipCount > RVX_OPAQUE_SCENE_COLOR_PYRAMID_MAX_MIP_COUNT ||
        pipeline == nullptr || setLayout == nullptr || sampler == nullptr ||
        device == nullptr)
    {
        outFailure =
            "TransmissionDepthPyramid requires the current full-frame D32 snapshot and sealed R32 pipeline resources";
        return false;
    }

    RHITextureDesc pyramidDescription = RHITextureDesc::RenderTarget(
        baseExtent.width, baseExtent.height, RHIFormat::R32_FLOAT);
    pyramidDescription.usage = pyramidDescription.usage |
        RHITextureUsage::ShaderResource | RHITextureUsage::CopySrc;
    pyramidDescription.mipLevels = mipCount;
    pyramidDescription.debugName = "TransmissionDepthPyramid";
    const RGTextureHandle pyramid = builder.CreateTexture(pyramidDescription);
    const std::shared_ptr<TransmissionDepthPyramidBuildReceipt> receipt =
        std::make_shared<TransmissionDepthPyramidBuildReceipt>();
    if (!pyramid.IsValid() || pyramid.index == source.texture.index ||
        !receipt)
    {
        outFailure =
            "TransmissionDepthPyramid could not allocate a distinct R32 mip texture";
        return false;
    }

    for (uint32 mipIndex = 0; mipIndex < mipCount; ++mipIndex)
    {
        const RGTextureHandle sourceMip = mipIndex == 0
            ? source.texture : pyramid.Subresource(mipIndex - 1u);
        const RGTextureHandle destinationMip = pyramid.Subresource(mipIndex);
        const RenderOutputExtent destinationExtent =
            CalculateMipExtent(baseExtent, mipIndex);
        const bool sourceIsFullResolution =
            mipIndex == 0 && fullResolutionBase;
        const bool sourceCanonical = mipIndex != 0;
        const std::string passName =
            "TransmissionDepthPyramidMip" + std::to_string(mipIndex);
        if (!builder.AddPass<TransmissionDepthPyramidPassData>(
                passName.c_str(),
                RenderGraphPassType::Graphics,
                [sourceMip,
                 destinationMip,
                 destinationExtent,
                 mipIndex,
                 mipCount,
                 sourceIsFullResolution,
                 reverseZ,
                 sourceCanonical,
                 device,
                 pipeline,
                 setLayout,
                 sampler,
                 sourceCopyReceipt = source.receipt,
                 receipt](RenderGraphBuilder& graphBuilder,
                          TransmissionDepthPyramidPassData& data)
                {
                    data.source = sourceMip;
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
                            sourceMip.hasSubresourceRange ? sourceMip.subresourceRange
                                                         : RHISubresourceRange::All();
                        sourceViewDesc.subresourceRange.aspect = mipIndex == 0
                            ? RHITextureAspect::Depth : RHITextureAspect::Color;
                        sourceViewDesc.type = RHITextureViewType::ShaderResource;
                        sourceViewDesc.debugName =
                            "TransmissionDepthPyramidSourceSRV";
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
                            "TransmissionDepthPyramidRTV";
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
                    data.reverseZ = reverseZ;
                    data.sourceCanonical = sourceCanonical;
                    data.device = device;
                    data.pipeline = pipeline;
                    data.setLayout = setLayout;
                    data.sampler = sampler;
                    data.sourceCopyReceipt = sourceCopyReceipt;
                    data.receipt = receipt;
                },
                [](const TransmissionDepthPyramidPassData& data,
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
                    if (data.receipt == nullptr || data.device == nullptr ||
                        !data.pipeline || !data.setLayout || !data.sampler ||
                        (data.mipIndex == 0 &&
                         (data.sourceCopyReceipt == nullptr ||
                          !data.sourceCopyReceipt->copied.load(
                              std::memory_order_acquire))) ||
                        data.receipt->failed.load(
                            std::memory_order_acquire) ||
                        data.receipt->completedMipCount.load(
                            std::memory_order_acquire) != data.mipIndex)
                    {
                        markFailed(
                            "TransmissionDepthPyramid observed unavailable source copy or out-of-order predecessor mip");
                        return;
                    }

                    RHITextureView* const sourceView =
                        graphContext.GetTextureView(data.sourceView);
                    RHITextureView* const destinationView =
                        graphContext.GetTextureView(data.destinationView);
                    if (sourceView == nullptr || destinationView == nullptr ||
                        !data.destinationExtent.IsValid())
                    {
                        markFailed(
                            "TransmissionDepthPyramid graph resources are unavailable");
                        return;
                    }

                    RHIBufferRef constants = CreateConstants(
                        *data.device,
                        data.sourceIsFullResolution,
                        data.reverseZ,
                        data.sourceCanonical);
                    if (!constants)
                    {
                        markFailed(
                            "TransmissionDepthPyramid constants allocation failed");
                        return;
                    }

                    RHIDescriptorSetDesc descriptorDesc;
                    descriptorDesc.layout = data.setLayout.Get();
                    descriptorDesc.debugName =
                        "TransmissionDepthPyramidDescriptorSet";
                    descriptorDesc.BindBuffer(
                        0,
                        constants.Get(),
                        0,
                        AlignConstantBufferSize(
                            sizeof(TransmissionDepthPyramidGPUConstants)));
                    descriptorDesc.BindTexture(1, sourceView);
                    descriptorDesc.BindSampler(2, data.sampler.Get());
                    RHIDescriptorSetRef descriptorSet =
                        data.device->CreateDescriptorSet(descriptorDesc);
                    if (!descriptorSet ||
                        !descriptorSet->IsReadyForBinding(data.setLayout.Get()) ||
                        !graphContext.RetainSubmissionResource(
                            Ref<RefCounted>(constants)) ||
                        !graphContext.RetainSubmissionResource(
                            Ref<RefCounted>(descriptorSet)) ||
                        !graphContext.RetainSubmissionResource(
                            Ref<RefCounted>(data.pipeline)) ||
                        !graphContext.RetainSubmissionResource(
                            Ref<RefCounted>(data.setLayout)) ||
                        !graphContext.RetainSubmissionResource(
                            Ref<RefCounted>(data.sampler)))
                    {
                        markFailed(
                            "TransmissionDepthPyramid descriptor creation or submission retention failed");
                        return;
                    }

                    RHIRenderPassDesc renderPassDesc;
                    renderPassDesc.AddColorAttachment(
                        destinationView,
                        RHILoadOp::DontCare,
                        RHIStoreOp::Store);
                    renderPassDesc.SetRenderArea(
                        0,
                        0,
                        data.destinationExtent.width,
                        data.destinationExtent.height);
                    RHICommandContext& commands = graphContext.Commands();
                    commands.BeginRenderPass(renderPassDesc);
                    commands.SetPipeline(data.pipeline.Get());
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
                    if (completedMipCount == data.mipCount &&
                        !data.receipt->failed.load(
                            std::memory_order_acquire))
                    {
                        data.receipt->completed.store(
                            true, std::memory_order_release);
                    }
                }))
        {
            outFailure =
                "TransmissionDepthPyramid graph pass registration failed";
            return false;
        }
    }

    outPyramid.texture = pyramid;
    outPyramid.baseExtent = baseExtent;
    outPyramid.mipCount = mipCount;
    outPyramid.receipt = receipt;
    return true;
}
} // namespace RVX
