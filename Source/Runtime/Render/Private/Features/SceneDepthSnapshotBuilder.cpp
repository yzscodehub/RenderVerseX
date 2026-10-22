#include "Features/SceneDepthSnapshotBuilder.h"
#include "Render/PipelineCache.h"
#include "Render/PostProcess/TransmissionDepthPyramid.h"

#include <atomic>
#include <memory>
#include <utility>

namespace RVX
{
namespace
{
    struct SceneDepthSnapshotCopyData
    {
        RGTextureHandle source{};
        RGTextureHandle destination{};
        std::shared_ptr<SceneDepthSnapshotCopyReceipt> receipt{};
    };

    [[nodiscard]] bool IsValidSourceDepth(
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
               HasFlag(description->usage, RHITextureUsage::CopySrc);
    }
} // namespace

SceneDepthSnapshotDemand ResolveSceneDepthSnapshotDemand(
    bool waterDepthRequired,
    bool waterExecutable,
    bool particleDepthRequested,
    bool particleDepthRequired,
    bool particleExecutable,
    bool transmissionRequired,
    bool transmissionExecutable)
{
    SceneDepthSnapshotDemand result;
    if (transmissionRequired && !transmissionExecutable)
    {
        result.failureReason = "ThinTransmission requires an executable sampled-depth consumer";
        return result;
    }
    if (waterDepthRequired && !waterExecutable)
    {
        result.failureReason =
            "Water requires an executable sampled-depth consumer";
        return result;
    }
    if (particleDepthRequired && !particleDepthRequested)
    {
        result.failureReason =
            "Required particle soft depth was not requested";
        return result;
    }
    if (particleDepthRequired && !particleExecutable)
    {
        result.failureReason =
            "Particle soft depth requires an executable consumer";
        return result;
    }

    result.valid = true;
    result.requested = (waterDepthRequired && waterExecutable) ||
        (particleDepthRequested && particleExecutable) || transmissionRequired;
    result.required = waterDepthRequired ||
        (particleDepthRequired && particleExecutable) || transmissionRequired;
    return result;
}

bool HasThinTransmissionDraws(const RenderPassRecordContext& context) noexcept
{
    const auto* items = context.frameSnapshot
        ? &context.frameSnapshot->transparentDrawItems : context.transparentDrawItems;
    if (items == nullptr) return false;
    for (const auto& item : *items)
    {
        if (item.packet.pipelineKey.shadingModel == MaterialShadingModel::ThinTransmission)
            return true;
    }
    return false;
}

bool BuildSceneTransmissionDepth(
    RenderFeatureGraphBuilder& builder, PipelineCache& pipelineCache,
    const RenderPassRecordContext& context, RenderQualityTier quality,
    SceneDepthSnapshot& snapshot, SceneDepthSnapshotRecordInput& input,
    std::string& outFailure)
{
    if (!HasThinTransmissionDraws(context)) return true;
    if (!snapshot.available)
    {
        outFailure = "ThinTransmission requires the current shared depth snapshot";
        return false;
    }
    TransmissionDepthPyramidOutput guard;
    if (!BuildTransmissionDepthPyramid(
            builder, pipelineCache, input, quality,
            pipelineCache.GetDepthClearValue() == 0.0f, guard, outFailure))
        return false;
    snapshot.transmissionRequested = true;
    snapshot.transmissionPyramid = guard.texture;
    snapshot.transmissionMetadata = {
        guard.baseExtent, RHIFormat::R32_FLOAT, RHISampleCount::Count1};
    snapshot.transmissionMipCount = guard.mipCount;
    snapshot.transmissionReceipt = guard.receipt;
    input.transmissionPyramid = guard.texture;
    input.transmissionBaseExtent = guard.baseExtent;
    input.transmissionMipCount = guard.mipCount;
    input.transmissionReceipt = guard.receipt;
    return true;
}

bool BuildSceneDepthSnapshot(
    RenderFeatureGraphBuilder& builder,
    const RenderPassRecordContext& context,
    RenderFrameIdentity frameIdentity,
    RenderViewIdentity viewIdentity,
    RenderOutputExtent expectedExtent,
    bool required,
    SceneDepthSnapshot& outSnapshot,
    SceneDepthSnapshotRecordInput& outRecordInput,
    std::string& outFailure)
{
    outSnapshot = {};
    outRecordInput = {};
    outFailure.clear();

    const RGTextureHandle source = context.view.depthTarget;
    const RHITextureDesc* const sourceDescription =
        builder.GetGraph().GetTextureDesc(source);
    if (!builder.IsValid() || !context.identity.IsValid() ||
        !frameIdentity.IsValid() || !viewIdentity.IsValid() ||
        !expectedExtent.IsValid() || !source.IsValid() ||
        !HasCurrentGraphProvenance(source, context.identity) ||
        !IsValidSourceDepth(sourceDescription, expectedExtent))
    {
        outFailure =
            "SceneDepthSnapshot requires the current D32 CopySrc depth surface at the exact view extent";
        return false;
    }

    RHITextureDesc destinationDescription = RHITextureDesc::DepthStencil(
        expectedExtent.width, expectedExtent.height, RHIFormat::D32_FLOAT);
    destinationDescription.usage = destinationDescription.usage |
        RHITextureUsage::ShaderResource | RHITextureUsage::CopyDst;
    destinationDescription.debugName = "SceneDepthSnapshot";
    const RGTextureHandle destination =
        builder.CreateTexture(destinationDescription);
    const std::shared_ptr<SceneDepthSnapshotCopyReceipt> receipt =
        std::make_shared<SceneDepthSnapshotCopyReceipt>();
    if (!destination.IsValid() || destination.index == source.index ||
        !receipt ||
        !builder.AddPass<SceneDepthSnapshotCopyData>(
            "SceneDepthSnapshotCopy",
            RenderGraphPassType::Graphics,
            [source, destination, receipt](RenderGraphBuilder& graphBuilder,
                                           SceneDepthSnapshotCopyData& data)
            {
                data.source = graphBuilder.Read(
                    source, RHIResourceState::CopySource);
                data.destination = graphBuilder.Write(
                    destination, RHIResourceState::CopyDest);
                // Blackboard publication is semantic only. Preserve this
                // destination for consumers that are absent in a given frame;
                // actual Water/Particle sampling still registers explicit reads.
                graphBuilder.SetExportState(
                    data.destination, RHIResourceState::ShaderResource);
                data.receipt = receipt;
            },
            [](const SceneDepthSnapshotCopyData& data,
               RenderGraphPassContext& graphContext)
            {
                RHITexture* const sourceTexture =
                    graphContext.GetTexture(data.source);
                RHITexture* const destinationTexture =
                    graphContext.GetTexture(data.destination);
                if (sourceTexture == nullptr || destinationTexture == nullptr ||
                    sourceTexture == destinationTexture ||
                    sourceTexture->GetWidth() != destinationTexture->GetWidth() ||
                    sourceTexture->GetHeight() != destinationTexture->GetHeight() ||
                    sourceTexture->GetFormat() != RHIFormat::D32_FLOAT ||
                    destinationTexture->GetFormat() != RHIFormat::D32_FLOAT ||
                    sourceTexture->GetSampleCount() != RHISampleCount::Count1 ||
                    destinationTexture->GetSampleCount() !=
                        RHISampleCount::Count1)
                {
                    graphContext.MarkExecutionFailed(
                        "Scene depth snapshot copy has invalid D32 source/destination provenance");
                    return;
                }

                graphContext.Commands().CopyTexture(
                    sourceTexture, destinationTexture);
                data.receipt->copied.store(true, std::memory_order_release);
            }))
    {
        outFailure =
            "SceneDepthSnapshot allocation/copy registration failed";
        return false;
    }

    const SceneTextures::Metadata metadata{
        expectedExtent, RHIFormat::D32_FLOAT, RHISampleCount::Count1};
    outSnapshot.provenance = builder.GetProvenance();
    outSnapshot.frameIdentity = frameIdentity;
    outSnapshot.viewIdentity = viewIdentity;
    outSnapshot.outputExtent = expectedExtent;
    outSnapshot.requested = true;
    outSnapshot.required = required;
    outSnapshot.available = true;
    outSnapshot.depthSnapshot = destination;
    outSnapshot.depthSnapshotMetadata = metadata;
    outSnapshot.receipt = receipt;

    outRecordInput.identity = context.identity;
    outRecordInput.texture = destination;
    outRecordInput.extent = expectedExtent;
    outRecordInput.format = RHIFormat::D32_FLOAT;
    outRecordInput.sampleCount = RHISampleCount::Count1;
    outRecordInput.receipt = receipt;
    return true;
}
} // namespace RVX
