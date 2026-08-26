/**
 * @file ScreenSpaceOutline.cpp
 * @brief Typed HDR toon silhouette and crease implementation.
 */

#include "Render/PostProcess/ScreenSpaceOutline.h"

#include "Core/Log.h"
#include "Render/Graph/ResourceViewCache.h"
#include "Render/Material/MaterialGPUData.h"
#include "Render/PipelineCache.h"
#include "Resources/RenderSubmissionTracker.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace RVX
{
namespace
{
    constexpr uint64 RVX_OUTLINE_CONSTANT_BUFFER_ALIGNMENT = 256;

    struct ScreenSpaceOutlineGPUConstants
    {
        /** xy = extent, zw = inverse extent. */
        float renderSize[4] = {1.0f, 1.0f, 1.0f, 1.0f};
        /** x = relative depth threshold, y = normal discontinuity threshold. */
        float thresholds[4] = {0.015f, 0.20f, 0.0f, 0.0f};
        uint32 materialTableSlotCount = 0;
        uint32 padding[3] = {0, 0, 0};
    };

    static_assert(sizeof(ScreenSpaceOutlineGPUConstants) == 48);

    struct ScreenSpaceOutlineVerifierGPUConstants
    {
        /** xy = extent, zw = inverse extent. */
        float renderSize[4] = {1.0f, 1.0f, 1.0f, 1.0f};
        uint32 materialTableSlotCount = 0;
        float changeEpsilon = 1.0e-4f;
        uint32 padding[2] = {0, 0};
    };

    static_assert(sizeof(ScreenSpaceOutlineVerifierGPUConstants) == 32);

    struct ScreenSpaceOutlineQualificationCopyData
    {
        RGTextureHandle mask{};
        RGBufferHandle readback{};
        RHIBufferRef readbackOwner;
        ScreenSpaceOutlineQualificationProbeIdentity identity{};
        uint64 generation = 0;
        uint32 width = 0;
        uint32 height = 0;
        uint32 rowPitch = 0;
    };

    [[nodiscard]] uint64 AlignConstantBufferSize(uint64 size) noexcept
    {
        return (size + RVX_OUTLINE_CONSTANT_BUFFER_ALIGNMENT - 1) &
               ~(RVX_OUTLINE_CONSTANT_BUFFER_ALIGNMENT - 1);
    }

    [[nodiscard]] uint32 AlignReadbackRowPitch(
        RHIBackendType backend,
        uint32 width) noexcept
    {
        const uint32 alignment = backend == RHIBackendType::DX12 ||
                                      backend == RHIBackendType::Metal
                                  ? 256U
                                  : 1U;
        const uint64 aligned = (static_cast<uint64>(width) + alignment - 1U) &
            ~static_cast<uint64>(alignment - 1U);
        return aligned != 0 && aligned <= std::numeric_limits<uint32>::max()
            ? static_cast<uint32>(aligned)
            : 0U;
    }

    [[nodiscard]] bool IsExactCurrentGraphicsSubmissionToken(
        const RenderSubmissionTracker& tracker,
        const GPUCompletionToken& token,
        GPUCompletionPoint& outPoint) noexcept
    {
        outPoint = {};
        GPUCompletionToken normalized;
        if (!MergeGPUCompletionToken(normalized, token) || normalized.count == 0 ||
            tracker.Query(normalized) == GPUCompletionStatus::Lost)
        {
            return false;
        }

        bool hasGraphicsPoint = false;
        for (uint32 index = 0; index < normalized.count; ++index)
        {
            const GPUCompletionPoint point = normalized.points[index];
            if (!IsDeclaredGPUQueueDomain(point.domain) || point.value == 0 ||
                point.value != tracker.GetLastSubmittedValue(point.domain))
            {
                return false;
            }
            if (point.domain == GPUQueueDomain::Graphics)
            {
                if (hasGraphicsPoint)
                {
                    return false;
                }
                hasGraphicsPoint = true;
                outPoint = point;
            }
        }
        return hasGraphicsPoint;
    }

    [[nodiscard]] bool IsCompletionSatisfied(
        GPUCompletionStatus status) noexcept
    {
        return status == GPUCompletionStatus::Completed ||
               status == GPUCompletionStatus::CompatibilityWaitIdle;
    }

    [[nodiscard]] uint64 HashBytes(uint64 hash,
                                   const void* data,
                                   size_t size) noexcept
    {
        constexpr uint64 OffsetBasis = 1469598103934665603ULL;
        constexpr uint64 Prime = 1099511628211ULL;
        if (hash == 0)
        {
            hash = OffsetBasis;
        }
        const auto* const bytes = static_cast<const uint8*>(data);
        for (size_t index = 0; index < size; ++index)
        {
            hash ^= bytes[index];
            hash *= Prime;
        }
        return hash;
    }

    [[nodiscard]] uint64 HashQualificationSettings(
        const ScreenSpaceOutlineConfig& config) noexcept
    {
        uint64 hash = 0;
        hash = HashBytes(hash, &config.depthThreshold,
                         sizeof(config.depthThreshold));
        return HashBytes(hash, &config.normalThreshold,
                         sizeof(config.normalThreshold));
    }

    [[nodiscard]] bool IsFinitePositive(float value) noexcept
    {
        return std::isfinite(value) && value > 0.0f;
    }

    [[nodiscard]] bool IsSameTextureHandle(const RGTextureHandle& lhs,
                                           const RGTextureHandle& rhs) noexcept
    {
        const RHISubresourceRange& lhsRange = lhs.subresourceRange;
        const RHISubresourceRange& rhsRange = rhs.subresourceRange;
        return lhs.index == rhs.index &&
               lhs.hasSubresourceRange == rhs.hasSubresourceRange &&
               lhs.graphIdentity == rhs.graphIdentity &&
               lhs.recordingGeneration == rhs.recordingGeneration &&
               lhsRange.baseMipLevel == rhsRange.baseMipLevel &&
               lhsRange.mipLevelCount == rhsRange.mipLevelCount &&
               lhsRange.baseArrayLayer == rhsRange.baseArrayLayer &&
               lhsRange.arrayLayerCount == rhsRange.arrayLayerCount &&
               lhsRange.aspect == rhsRange.aspect;
    }

    [[nodiscard]] bool HasSameGraphIdentity(const RGTextureHandle& reference,
                                            const RGTextureHandle& candidate) noexcept
    {
        return reference.IsValid() && candidate.IsValid() &&
               reference.graphIdentity == candidate.graphIdentity &&
               reference.recordingGeneration == candidate.recordingGeneration;
    }

    [[nodiscard]] bool HasSameGraphIdentity(const RGTextureHandle& reference,
                                            const RGBufferHandle& candidate) noexcept
    {
        return reference.IsValid() && candidate.IsValid() &&
               reference.graphIdentity == candidate.graphIdentity &&
               reference.recordingGeneration == candidate.recordingGeneration;
    }

    [[nodiscard]] bool MatchesMetadata(const RHITextureDesc& desc,
                                       const PostProcessTextureMetadata& metadata) noexcept
    {
        return metadata.IsValid() && desc.width == metadata.extent.width &&
               desc.height == metadata.extent.height &&
               desc.format == metadata.format &&
               desc.sampleCount == metadata.sampleCount;
    }

    [[nodiscard]] RHIBufferRef CreateImmutablePassConstants(
        IRHIDevice& device,
        const ScreenSpaceOutlineGPUConstants& constants)
    {
        RHIBufferDesc desc;
        desc.size = AlignConstantBufferSize(sizeof(constants));
        desc.usage = RHIBufferUsage::Constant;
        desc.memoryType = RHIMemoryType::Upload;
        desc.debugName = "ScreenSpaceOutlineConstants";
        RHIBufferRef buffer = device.CreateBuffer(desc);
        if (!buffer)
        {
            return {};
        }

        void* const mapped = buffer->Map();
        if (mapped == nullptr)
        {
            return {};
        }
        std::memcpy(mapped, &constants, sizeof(constants));
        if (!buffer->CommitMappedWrite())
        {
            return {};
        }
        return buffer;
    }

    [[nodiscard]] RHIBufferRef CreateImmutableVerifierConstants(
        IRHIDevice& device,
        const ScreenSpaceOutlineVerifierGPUConstants& constants)
    {
        RHIBufferDesc desc;
        desc.size = AlignConstantBufferSize(sizeof(constants));
        desc.usage = RHIBufferUsage::Constant;
        desc.memoryType = RHIMemoryType::Upload;
        desc.debugName = "ScreenSpaceOutlineVerifierConstants";
        RHIBufferRef buffer = device.CreateBuffer(desc);
        if (!buffer)
        {
            return {};
        }

        void* const mapped = buffer->Map();
        if (mapped == nullptr)
        {
            return {};
        }
        std::memcpy(mapped, &constants, sizeof(constants));
        return buffer->CommitMappedWrite() ? buffer : RHIBufferRef{};
    }

    [[nodiscard]] RGTextureViewHandle DeclareSRV(
        RenderGraphBuilder& builder,
        RGTextureHandle texture,
        RHITextureAspect aspect,
        const char* debugName)
    {
        const RHITextureDesc* const desc = builder.GetTextureDesc(texture);
        if (desc == nullptr)
        {
            return {};
        }

        RHITextureViewDesc viewDesc;
        viewDesc.format = desc->format;
        viewDesc.dimension = desc->dimension;
        viewDesc.subresourceRange = RHISubresourceRange::All();
        viewDesc.subresourceRange.aspect = aspect;
        viewDesc.type = RHITextureViewType::ShaderResource;
        viewDesc.debugName = debugName;
        return builder.Read(
            builder.CreateTextureView(texture, viewDesc),
            MakeRGAccessDesc(RHIResourceState::ShaderResource,
                             RHIShaderStage::Pixel));
    }

    [[nodiscard]] RGTextureViewHandle DeclareRTV(
        RenderGraphBuilder& builder,
        RGTextureHandle texture,
        const char* debugName)
    {
        const RHITextureDesc* const desc = builder.GetTextureDesc(texture);
        if (desc == nullptr)
        {
            return {};
        }

        RHITextureViewDesc viewDesc;
        viewDesc.format = desc->format;
        viewDesc.dimension = desc->dimension;
        viewDesc.subresourceRange = RHISubresourceRange::All();
        viewDesc.type = RHITextureViewType::RenderTarget;
        viewDesc.debugName = debugName;
        return builder.Write(
            builder.CreateTextureView(texture, viewDesc),
            MakeRGAccessDesc(RHIResourceState::RenderTarget,
                             RHIShaderStage::Pixel,
                             RHIDiscardIntent::Discard));
    }
} // namespace

ScreenSpaceOutlinePass::ScreenSpaceOutlinePass()
{
    m_enabled = false;
    MarkUnsupported(
        "ScreenSpaceOutline requires cooked pipeline and typed scene inputs");
}

PostProcessPassDescriptor ScreenSpaceOutlinePass::Describe() const
{
    return {PostProcessPassId::ScreenSpaceOutline,
            "ScreenSpaceOutline",
            PostProcessStage::HDRPost,
            800,
            PostProcessColorDomain::SceneLinearHDR,
            PostProcessColorDomain::SceneLinearHDR,
            PostProcessTransfer::None};
}

void ScreenSpaceOutlinePass::Configure(const PostProcessSettings& settings)
{
    m_enabled = settings.enableToonOutline;
    m_config.depthThreshold = settings.toonOutlineDepthThreshold;
    m_config.normalThreshold = settings.toonOutlineNormalThreshold;
    RefreshSupportState();
}

PostProcessFrameInputRequirements
ScreenSpaceOutlinePass::GetFrameInputRequirements() const
{
    PostProcessFrameInputRequirements requirements;
    requirements.requiresDepth = true;
    requirements.requiresNormal = true;
    requirements.requiresMaterialIdentity = true;
    requirements.requiresMaterialParameterTable = true;
    return requirements;
}

uint64 ScreenSpaceOutlinePass::GetSettingsHash() const noexcept
{
    return HashQualificationSettings(m_config);
}

std::optional<ScreenSpaceOutlineExecutionIdentity>
ScreenSpaceOutlinePass::GetLastExecutionIdentity() const noexcept
{
    return m_lastExecutionIdentity.IsValid()
        ? std::optional<ScreenSpaceOutlineExecutionIdentity>{
              m_lastExecutionIdentity}
        : std::nullopt;
}

bool ScreenSpaceOutlinePass::ValidateFrameInputs(
    const PostProcessFrameInputs& inputs,
    std::string& outReason) const
{
    outReason.clear();
    if (!inputs.sceneColor.IsValid() || !inputs.HasDepth() ||
        !inputs.HasNormal() || !inputs.HasMaterialIdentity() ||
        !inputs.HasMaterialParameterTable())
    {
        outReason = "ScreenSpaceOutline requires scene color, depth, oct-normal, material identity, and material table";
        return false;
    }
    if (!inputs.sceneColorMetadata.IsValid() ||
        !inputs.depthMetadata.IsValid() || !inputs.normalMetadata.IsValid() ||
        !inputs.materialIdentityMetadata.IsValid() ||
        !inputs.materialParameterTableMetadata.IsValid())
    {
        outReason = "ScreenSpaceOutline requires complete typed input metadata";
        return false;
    }
    if (!HasSameGraphIdentity(inputs.sceneColor, inputs.depth) ||
        !HasSameGraphIdentity(inputs.sceneColor, inputs.normal) ||
        !HasSameGraphIdentity(inputs.sceneColor, inputs.materialIdentity) ||
        !HasSameGraphIdentity(inputs.sceneColor,
                              inputs.materialParameterTable))
    {
        outReason = "ScreenSpaceOutline inputs belong to different graph generations";
        return false;
    }
    const RenderOutputExtent extent = inputs.sceneColorMetadata.extent;
    if (inputs.sceneColorMetadata.format != RHIFormat::RGBA16_FLOAT ||
        inputs.sceneColorMetadata.sampleCount != RHISampleCount::Count1 ||
        inputs.depthMetadata.format != RHIFormat::D32_FLOAT ||
        inputs.depthMetadata.sampleCount != RHISampleCount::Count1 ||
        inputs.normalMetadata.format != RHIFormat::RGBA8_UNORM ||
        inputs.normalMetadata.sampleCount != RHISampleCount::Count1 ||
        inputs.materialIdentityMetadata.format != RHIFormat::R32_UINT ||
        inputs.materialIdentityMetadata.sampleCount != RHISampleCount::Count1 ||
        inputs.depthMetadata.extent != extent ||
        inputs.normalMetadata.extent != extent ||
        inputs.materialIdentityMetadata.extent != extent ||
        inputs.materialParameterTableMetadata.sourceExtent != extent ||
        inputs.materialParameterTableMetadata.sourceSampleCount !=
            RHISampleCount::Count1 ||
        inputs.materialParameterTableMetadata.schemaVersion !=
            PostProcessMaterialTableMetadata::SchemaVersion ||
        inputs.materialParameterTableMetadata.stride !=
            sizeof(MaterialGPUConstants))
    {
        outReason = "ScreenSpaceOutline typed inputs violate the full-resolution material-table contract";
        return false;
    }
    if (!IsFinitePositive(m_config.depthThreshold) ||
        !IsFinitePositive(m_config.normalThreshold) ||
        m_config.normalThreshold > 2.0f)
    {
        outReason = "ScreenSpaceOutline settings contain invalid discontinuity thresholds";
        return false;
    }
    return true;
}

void ScreenSpaceOutlinePass::AddToGraph(RenderGraph& graph,
                                        RGTextureHandle input,
                                        RGTextureHandle output)
{
    BeginExecutionRecording();
    static_cast<void>(graph);
    static_cast<void>(input);
    static_cast<void>(output);
    MarkUnsupported(
        "ScreenSpaceOutline requires the typed post-process frame inputs");
}

void ScreenSpaceOutlinePass::AddToGraph(
    RenderGraph& graph,
    const PostProcessFrameInputs& frameInputs,
    RGTextureHandle output)
{
    BeginExecutionRecording();
    if (!IsEnabled())
    {
        if (m_probeRequested)
        {
            m_lastProbeStats.failure =
                "ScreenSpaceOutline qualification requires the visible outline pass to be enabled";
            ClearQualificationProbeState(true);
        }
        return;
    }

    std::string validationReason;
    if (!ValidateFrameInputs(frameInputs, validationReason) ||
        IsSameTextureHandle(frameInputs.sceneColor, output) ||
        IsSameTextureHandle(frameInputs.depth, output) ||
        IsSameTextureHandle(frameInputs.normal, output) ||
        IsSameTextureHandle(frameInputs.materialIdentity, output))
    {
        RVX_CORE_WARN("ScreenSpaceOutline: typed input validation failed: {}",
                      validationReason.empty()
                          ? "outline input and output must be distinct"
                          : validationReason);
        if (m_probeRequested)
        {
            m_lastProbeStats.failure = validationReason.empty()
                ? "ScreenSpaceOutline qualification input and output alias"
                : validationReason;
            ClearQualificationProbeState(true);
        }
        return;
    }

    const RHITextureDesc* const sceneDesc =
        graph.GetTextureDesc(frameInputs.sceneColor);
    const RHITextureDesc* const depthDesc = graph.GetTextureDesc(frameInputs.depth);
    const RHITextureDesc* const normalDesc =
        graph.GetTextureDesc(frameInputs.normal);
    const RHITextureDesc* const materialIdentityDesc =
        graph.GetTextureDesc(frameInputs.materialIdentity);
    const RHITextureDesc* const outputDesc = graph.GetTextureDesc(output);
    const RHIBufferDesc* const materialTableDesc =
        graph.GetBufferDesc(frameInputs.materialParameterTable);
    if (sceneDesc == nullptr || depthDesc == nullptr || normalDesc == nullptr ||
        materialIdentityDesc == nullptr || outputDesc == nullptr ||
        materialTableDesc == nullptr ||
        !MatchesMetadata(*sceneDesc, frameInputs.sceneColorMetadata) ||
        !MatchesMetadata(*depthDesc, frameInputs.depthMetadata) ||
        !MatchesMetadata(*normalDesc, frameInputs.normalMetadata) ||
        !MatchesMetadata(*materialIdentityDesc,
                         frameInputs.materialIdentityMetadata) ||
        outputDesc->format != RHIFormat::RGBA16_FLOAT ||
        outputDesc->sampleCount != RHISampleCount::Count1 ||
        outputDesc->width != sceneDesc->width ||
        outputDesc->height != sceneDesc->height ||
        !HasFlag(outputDesc->usage, RHITextureUsage::RenderTarget) ||
        !HasFlag(sceneDesc->usage, RHITextureUsage::ShaderResource) ||
        !HasFlag(depthDesc->usage, RHITextureUsage::ShaderResource) ||
        !HasFlag(normalDesc->usage, RHITextureUsage::ShaderResource) ||
        !HasFlag(materialIdentityDesc->usage,
                 RHITextureUsage::ShaderResource) ||
        !HasFlag(materialTableDesc->usage, RHIBufferUsage::Structured) ||
        !HasFlag(materialTableDesc->usage, RHIBufferUsage::ShaderResource) ||
        materialTableDesc->stride !=
            frameInputs.materialParameterTableMetadata.stride ||
        materialTableDesc->stride == 0 ||
        materialTableDesc->size /
                static_cast<uint64>(materialTableDesc->stride) <
            frameInputs.materialParameterTableMetadata.slotCount)
    {
        RVX_CORE_WARN("ScreenSpaceOutline: graph resources violate the typed outline contract");
        if (m_probeRequested)
        {
            m_lastProbeStats.failure =
                "ScreenSpaceOutline qualification graph resources violate the typed contract";
            ClearQualificationProbeState(true);
        }
        return;
    }

    const ScreenSpaceOutlineExecutionIdentity executionIdentity{
        frameInputs.frameIndex,
        frameInputs.historyIdentity,
        frameInputs.sceneColorMetadata.extent,
        GetSettingsHash(),
        frameInputs.materialParameterTableMetadata.contentHash};
    if (!executionIdentity.IsValid())
    {
        RVX_CORE_WARN(
            "ScreenSpaceOutline: visible execution identity is incomplete");
        if (m_probeRequested)
        {
            m_lastProbeStats.failure =
                "ScreenSpaceOutline qualification identity is incomplete";
            ClearQualificationProbeState(true);
        }
        return;
    }

    bool qualificationRequested = false;
    ScreenSpaceOutlineQualificationProbeIdentity qualificationIdentity{};
    uint64 qualificationGeneration = 0;
    if (m_probeRequested && !m_probeIdentityLocked)
    {
        qualificationIdentity = executionIdentity;
        if (!EnsureQualificationReadback(
                     qualificationIdentity.extent.width,
                     qualificationIdentity.extent.height))
        {
            m_lastProbeStats.failure =
                "ScreenSpaceOutline qualification readback allocation failed";
            ClearQualificationProbeState(true);
        }
        else
        {
            m_probeIdentity = qualificationIdentity;
            m_probeIdentityLocked = true;
            qualificationRequested = true;
            qualificationGeneration = m_probeGeneration;
            m_lastProbeStats.identity = qualificationIdentity;
            m_lastProbeStats.resolutionAware = true;
        }
    }

    struct OutlinePassData
    {
        RGTextureHandle sceneColor{};
        RGTextureHandle depth{};
        RGTextureHandle normal{};
        RGTextureHandle materialIdentity{};
        RGBufferHandle materialParameterTable{};
        RGTextureHandle output{};
        RGTextureViewHandle sceneColorView{};
        RGTextureViewHandle depthView{};
        RGTextureViewHandle normalView{};
        RGTextureViewHandle materialIdentityView{};
        RGTextureViewHandle outputView{};
        uint32 materialTableSlotCount = 0;
        float depthThreshold = 0.015f;
        float normalThreshold = 0.20f;
        ScreenSpaceOutlineExecutionIdentity executionIdentity{};
        uint64 executionGeneration = 0;
        ScreenSpaceOutlineQualificationProbeIdentity qualificationIdentity{};
        uint64 qualificationGeneration = 0;
        bool qualificationExpected = false;
        bool inputsDeclared = false;
    };

    graph.AddPass<OutlinePassData>(
        "ScreenSpaceOutline",
        RenderGraphPassType::Graphics,
        [frameInputs, output, executionIdentity,
         executionGeneration = m_executionGeneration,
         qualificationIdentity, qualificationGeneration,
         qualificationRequested, this](RenderGraphBuilder& builder,
                                        OutlinePassData& data)
        {
            data.sceneColor = builder.Read(
                frameInputs.sceneColor,
                MakeRGAccessDesc(RHIResourceState::ShaderResource,
                                 RHIShaderStage::Pixel));
            data.depth = builder.Read(
                frameInputs.depth,
                MakeRGAccessDesc(RHIResourceState::ShaderResource,
                                 RHIShaderStage::Pixel));
            data.normal = builder.Read(
                frameInputs.normal,
                MakeRGAccessDesc(RHIResourceState::ShaderResource,
                                 RHIShaderStage::Pixel));
            data.materialIdentity = builder.Read(
                frameInputs.materialIdentity,
                MakeRGAccessDesc(RHIResourceState::ShaderResource,
                                 RHIShaderStage::Pixel));
            data.materialParameterTable = builder.Read(
                frameInputs.materialParameterTable,
                MakeRGAccessDesc(RHIResourceState::ShaderResource,
                                 RHIShaderStage::Pixel));
            data.output = output;
            data.sceneColorView = DeclareSRV(
                builder, frameInputs.sceneColor, RHITextureAspect::Color,
                "ScreenSpaceOutlineSceneColorSRV");
            data.depthView = DeclareSRV(builder, frameInputs.depth,
                                        RHITextureAspect::Depth,
                                        "ScreenSpaceOutlineDepthSRV");
            data.normalView = DeclareSRV(builder, frameInputs.normal,
                                         RHITextureAspect::Color,
                                         "ScreenSpaceOutlineNormalSRV");
            data.materialIdentityView = DeclareSRV(
                builder, frameInputs.materialIdentity,
                RHITextureAspect::Color,
                "ScreenSpaceOutlineMaterialIdentitySRV");
            data.outputView = DeclareRTV(builder, output,
                                         "ScreenSpaceOutlineOutputRTV");
            data.materialTableSlotCount =
                frameInputs.materialParameterTableMetadata.slotCount;
            data.depthThreshold = m_config.depthThreshold;
            data.normalThreshold = m_config.normalThreshold;
            data.executionIdentity = executionIdentity;
            data.executionGeneration = executionGeneration;
            data.qualificationIdentity = qualificationIdentity;
            data.qualificationGeneration = qualificationGeneration;
            data.qualificationExpected = qualificationRequested;
            data.inputsDeclared = data.sceneColor.IsValid() &&
                data.depth.IsValid() && data.normal.IsValid() &&
                data.materialIdentity.IsValid() &&
                data.materialParameterTable.IsValid() && data.output.IsValid() &&
                data.sceneColorView.IsValid() && data.depthView.IsValid() &&
                data.normalView.IsValid() && data.materialIdentityView.IsValid() &&
                data.outputView.IsValid() && data.materialTableSlotCount != 0;
        },
        [this](const OutlinePassData& data, RenderGraphPassContext& context)
        {
            if (!data.inputsDeclared || m_pipelineCache == nullptr)
            {
                context.MarkExecutionFailed(
                    "ScreenSpaceOutline graph inputs or resources are unavailable");
                return;
            }

            RHIPipeline* const pipeline =
                m_pipelineCache->GetScreenSpaceOutlinePipeline(
                    RHIFormat::RGBA16_FLOAT);
            RHIDescriptorSetLayout* const setLayout =
                m_pipelineCache->GetScreenSpaceOutlineSetLayout();
            IRHIDevice* const device = m_pipelineCache->GetDevice();
            RHITexture* const sceneColor = context.GetTexture(data.sceneColor);
            RHITexture* const output = context.GetTexture(data.output);
            RHIBuffer* const materialTable =
                context.GetBuffer(data.materialParameterTable);
            RHITextureView* const sceneColorView =
                context.GetTextureView(data.sceneColorView);
            RHITextureView* const depthView = context.GetTextureView(data.depthView);
            RHITextureView* const normalView = context.GetTextureView(data.normalView);
            RHITextureView* const materialIdentityView =
                context.GetTextureView(data.materialIdentityView);
            RHITextureView* const outputView = context.GetTextureView(data.outputView);
            if (pipeline == nullptr || setLayout == nullptr || device == nullptr ||
                sceneColor == nullptr || output == nullptr ||
                materialTable == nullptr || sceneColorView == nullptr ||
                depthView == nullptr || normalView == nullptr ||
                materialIdentityView == nullptr || outputView == nullptr ||
                !m_pointClampSampler)
            {
                context.MarkExecutionFailed(
                    "ScreenSpaceOutline runtime pipeline, views, or table are unavailable");
                return;
            }

            ScreenSpaceOutlineGPUConstants constants;
            constants.renderSize[0] = static_cast<float>(output->GetWidth());
            constants.renderSize[1] = static_cast<float>(output->GetHeight());
            constants.renderSize[2] = constants.renderSize[0] > 0.0f
                ? 1.0f / constants.renderSize[0] : 1.0f;
            constants.renderSize[3] = constants.renderSize[1] > 0.0f
                ? 1.0f / constants.renderSize[1] : 1.0f;
            constants.thresholds[0] = data.depthThreshold;
            constants.thresholds[1] = data.normalThreshold;
            constants.materialTableSlotCount = data.materialTableSlotCount;
            RHIBufferRef constantBuffer = CreateImmutablePassConstants(
                *device, constants);
            if (!constantBuffer ||
                !context.RetainSubmissionResource(
                    Ref<RefCounted>(constantBuffer)))
            {
                context.MarkExecutionFailed(
                    "ScreenSpaceOutline immutable constants are unavailable");
                return;
            }

            RHIDescriptorSetDesc descriptorDesc;
            descriptorDesc.layout = setLayout;
            descriptorDesc.debugName = "ScreenSpaceOutlineDescriptorSet";
            descriptorDesc.BindBuffer(0, constantBuffer.Get(), 0,
                                      AlignConstantBufferSize(sizeof(constants)));
            descriptorDesc.BindTexture(1, sceneColorView);
            descriptorDesc.BindTexture(2, depthView);
            descriptorDesc.BindTexture(3, normalView);
            descriptorDesc.BindTexture(4, materialIdentityView);
            descriptorDesc.BindBuffer(5, materialTable);
            descriptorDesc.BindSampler(6, m_pointClampSampler.Get());
            RHIDescriptorSetRef descriptorSet =
                device->CreateDescriptorSet(descriptorDesc);
            if (!descriptorSet ||
                !context.RetainSubmissionResource(
                    Ref<RefCounted>(descriptorSet)))
            {
                context.MarkExecutionFailed(
                    "ScreenSpaceOutline descriptor set creation failed");
                return;
            }

            RHIRenderPassDesc renderPassDesc;
            renderPassDesc.AddColorAttachment(outputView, RHILoadOp::DontCare,
                                              RHIStoreOp::Store);
            renderPassDesc.SetRenderArea(0, 0, output->GetWidth(),
                                         output->GetHeight());

            RHICommandContext& commands = context.Commands();
            commands.BeginRenderPass(renderPassDesc);
            commands.SetPipeline(pipeline);
            commands.SetDescriptorSet(0, descriptorSet.Get());

            RHIViewport viewport;
            viewport.width = static_cast<float>(output->GetWidth());
            viewport.height = static_cast<float>(output->GetHeight());
            viewport.minDepth = 0.0f;
            viewport.maxDepth = 1.0f;
            commands.SetViewport(viewport);
            RHIRect scissor;
            scissor.width = output->GetWidth();
            scissor.height = output->GetHeight();
            commands.SetScissor(scissor);
            commands.Draw(3, 1, 0, 0);
            commands.EndRenderPass();

            if (m_executionGeneration == data.executionGeneration &&
                data.executionIdentity.IsValid())
            {
                m_lastExecutionIdentity = data.executionIdentity;
            }

            if (data.qualificationExpected && m_probeRequested &&
                m_probeIdentityLocked &&
                m_probeGeneration == data.qualificationGeneration &&
                m_probeIdentity == data.qualificationIdentity)
            {
                m_probeOutlineDrawRecorded = true;
                m_lastProbeStats.outlineDrawRecorded = true;
            }
        });

    // The verifier is an opt-in, independent physical observation. It is not
    // part of the visible pipeline and cannot make a scheduled graph mutation
    // look like an executed outline: the three callback flags below are only
    // raised after the respective draw/copy commands have been encoded.
    if (!qualificationRequested)
    {
        return;
    }

    if (!HasFlag(outputDesc->usage, RHITextureUsage::ShaderResource))
    {
        m_lastProbeStats.failure =
            "ScreenSpaceOutline qualification requires a shader-readable post-outline HDR target";
        ClearQualificationProbeState(true);
        return;
    }

    RHITextureDesc maskDesc = RHITextureDesc::Texture2D(
        qualificationIdentity.extent.width, qualificationIdentity.extent.height,
        RHIFormat::R8_UNORM,
        RHITextureUsage::RenderTarget | RHITextureUsage::ShaderResource |
            RHITextureUsage::CopySrc);
    maskDesc.debugName = "ScreenSpaceOutlineQualificationMask";
    const RGTextureHandle qualificationMask = graph.CreateTexture(maskDesc);
    const RGBufferHandle probeReadback = graph.ImportBuffer(
        RHIBufferRef(m_probeReadback), RHIResourceState::CopyDest);
    if (!qualificationMask.IsValid() || !probeReadback.IsValid())
    {
        m_lastProbeStats.failure =
            "ScreenSpaceOutline qualification graph resources could not be created";
        ClearQualificationProbeState(true);
        return;
    }

    struct VerifierPassData
    {
        RGTextureHandle preOutline{};
        RGTextureHandle postOutline{};
        RGTextureHandle materialIdentity{};
        RGBufferHandle materialParameterTable{};
        RGTextureHandle mask{};
        RGTextureViewHandle preOutlineView{};
        RGTextureViewHandle postOutlineView{};
        RGTextureViewHandle materialIdentityView{};
        RGTextureViewHandle maskView{};
        uint32 materialTableSlotCount = 0;
        ScreenSpaceOutlineQualificationProbeIdentity identity{};
        uint64 generation = 0;
        bool inputsDeclared = false;
    };

    graph.AddPass<VerifierPassData>(
        "ScreenSpaceOutlineQualificationVerifier",
        RenderGraphPassType::Graphics,
        [frameInputs, output, qualificationMask, qualificationIdentity,
         qualificationGeneration](RenderGraphBuilder& builder,
                                  VerifierPassData& data)
        {
            data.preOutline = builder.Read(
                frameInputs.sceneColor,
                MakeRGAccessDesc(RHIResourceState::ShaderResource,
                                 RHIShaderStage::Pixel));
            data.postOutline = builder.Read(
                output,
                MakeRGAccessDesc(RHIResourceState::ShaderResource,
                                 RHIShaderStage::Pixel));
            data.materialIdentity = builder.Read(
                frameInputs.materialIdentity,
                MakeRGAccessDesc(RHIResourceState::ShaderResource,
                                 RHIShaderStage::Pixel));
            data.materialParameterTable = builder.Read(
                frameInputs.materialParameterTable,
                MakeRGAccessDesc(RHIResourceState::ShaderResource,
                                 RHIShaderStage::Pixel));
            data.mask = qualificationMask;
            data.preOutlineView = DeclareSRV(
                builder, frameInputs.sceneColor, RHITextureAspect::Color,
                "ScreenSpaceOutlineVerifierPreOutlineSRV");
            data.postOutlineView = DeclareSRV(
                builder, output, RHITextureAspect::Color,
                "ScreenSpaceOutlineVerifierPostOutlineSRV");
            data.materialIdentityView = DeclareSRV(
                builder, frameInputs.materialIdentity, RHITextureAspect::Color,
                "ScreenSpaceOutlineVerifierMaterialIdentitySRV");
            data.maskView = DeclareRTV(
                builder, qualificationMask,
                "ScreenSpaceOutlineQualificationMaskRTV");
            data.materialTableSlotCount =
                frameInputs.materialParameterTableMetadata.slotCount;
            data.identity = qualificationIdentity;
            data.generation = qualificationGeneration;
            data.inputsDeclared = data.preOutline.IsValid() &&
                data.postOutline.IsValid() && data.materialIdentity.IsValid() &&
                data.materialParameterTable.IsValid() && data.mask.IsValid() &&
                data.preOutlineView.IsValid() && data.postOutlineView.IsValid() &&
                data.materialIdentityView.IsValid() && data.maskView.IsValid() &&
                data.materialTableSlotCount != 0;
        },
        [this](const VerifierPassData& data, RenderGraphPassContext& context)
        {
            if (!data.inputsDeclared || m_pipelineCache == nullptr)
            {
                context.MarkExecutionFailed(
                    "ScreenSpaceOutline qualification verifier inputs or resources are unavailable");
                return;
            }

            RHIPipeline* const pipeline =
                m_pipelineCache->GetScreenSpaceOutlineVerifierPipeline(
                    RHIFormat::R8_UNORM);
            RHIDescriptorSetLayout* const setLayout =
                m_pipelineCache->GetScreenSpaceOutlineVerifierSetLayout();
            IRHIDevice* const device = m_pipelineCache->GetDevice();
            RHITexture* const preOutline = context.GetTexture(data.preOutline);
            RHITexture* const postOutline = context.GetTexture(data.postOutline);
            RHITexture* const mask = context.GetTexture(data.mask);
            RHIBuffer* const materialTable =
                context.GetBuffer(data.materialParameterTable);
            RHITextureView* const preOutlineView =
                context.GetTextureView(data.preOutlineView);
            RHITextureView* const postOutlineView =
                context.GetTextureView(data.postOutlineView);
            RHITextureView* const materialIdentityView =
                context.GetTextureView(data.materialIdentityView);
            RHITextureView* const maskView = context.GetTextureView(data.maskView);
            if (pipeline == nullptr || setLayout == nullptr || device == nullptr ||
                preOutline == nullptr || postOutline == nullptr || mask == nullptr ||
                materialTable == nullptr || preOutlineView == nullptr ||
                postOutlineView == nullptr || materialIdentityView == nullptr ||
                maskView == nullptr || !m_pointClampSampler)
            {
                context.MarkExecutionFailed(
                    "ScreenSpaceOutline qualification verifier pipeline, views, or table are unavailable");
                return;
            }

            ScreenSpaceOutlineVerifierGPUConstants constants;
            constants.renderSize[0] = static_cast<float>(mask->GetWidth());
            constants.renderSize[1] = static_cast<float>(mask->GetHeight());
            constants.renderSize[2] = constants.renderSize[0] > 0.0f
                ? 1.0f / constants.renderSize[0] : 1.0f;
            constants.renderSize[3] = constants.renderSize[1] > 0.0f
                ? 1.0f / constants.renderSize[1] : 1.0f;
            constants.materialTableSlotCount = data.materialTableSlotCount;
            RHIBufferRef constantBuffer = CreateImmutableVerifierConstants(
                *device, constants);
            if (!constantBuffer ||
                !context.RetainSubmissionResource(
                    Ref<RefCounted>(constantBuffer)))
            {
                context.MarkExecutionFailed(
                    "ScreenSpaceOutline qualification verifier constants are unavailable");
                return;
            }

            RHIDescriptorSetDesc descriptorDesc;
            descriptorDesc.layout = setLayout;
            descriptorDesc.debugName = "ScreenSpaceOutlineVerifierDescriptorSet";
            descriptorDesc.BindBuffer(0, constantBuffer.Get(), 0,
                                      AlignConstantBufferSize(sizeof(constants)));
            descriptorDesc.BindTexture(1, preOutlineView);
            descriptorDesc.BindTexture(2, postOutlineView);
            descriptorDesc.BindTexture(3, materialIdentityView);
            descriptorDesc.BindBuffer(4, materialTable);
            descriptorDesc.BindSampler(5, m_pointClampSampler.Get());
            RHIDescriptorSetRef descriptorSet =
                device->CreateDescriptorSet(descriptorDesc);
            if (!descriptorSet ||
                !context.RetainSubmissionResource(
                    Ref<RefCounted>(descriptorSet)))
            {
                context.MarkExecutionFailed(
                    "ScreenSpaceOutline qualification verifier descriptor set creation failed");
                return;
            }

            RHIRenderPassDesc renderPassDesc;
            renderPassDesc.AddColorAttachment(maskView, RHILoadOp::DontCare,
                                              RHIStoreOp::Store);
            renderPassDesc.SetRenderArea(0, 0, mask->GetWidth(), mask->GetHeight());
            RHICommandContext& commands = context.Commands();
            commands.BeginRenderPass(renderPassDesc);
            commands.SetPipeline(pipeline);
            commands.SetDescriptorSet(0, descriptorSet.Get());
            RHIViewport viewport;
            viewport.width = static_cast<float>(mask->GetWidth());
            viewport.height = static_cast<float>(mask->GetHeight());
            viewport.minDepth = 0.0f;
            viewport.maxDepth = 1.0f;
            commands.SetViewport(viewport);
            RHIRect scissor;
            scissor.width = mask->GetWidth();
            scissor.height = mask->GetHeight();
            commands.SetScissor(scissor);
            commands.Draw(3, 1, 0, 0);
            commands.EndRenderPass();

            if (m_probeRequested && m_probeIdentityLocked &&
                m_probeGeneration == data.generation &&
                m_probeIdentity == data.identity)
            {
                m_probeVerifierDrawRecorded = true;
                m_lastProbeStats.verifierDrawRecorded = true;
            }
        });

    graph.AddPass<ScreenSpaceOutlineQualificationCopyData>(
        "ScreenSpaceOutlineQualificationProbeCopy",
        RenderGraphPassType::Copy,
        RenderGraphQueueAffinity::Graphics,
        [qualificationMask, probeReadback,
         readbackOwner = RHIBufferRef(m_probeReadback), qualificationIdentity,
         qualificationGeneration, rowPitch = m_probeRowPitch](
            RenderGraphBuilder& builder,
            ScreenSpaceOutlineQualificationCopyData& data)
        {
            data.mask = builder.Read(
                qualificationMask,
                MakeRGAccessDesc(RHIResourceState::CopySource,
                                 RHIShaderStage::All));
            data.readback = builder.Write(
                probeReadback,
                MakeRGAccessDesc(RHIResourceState::CopyDest,
                                 RHIShaderStage::All));
            data.readbackOwner = readbackOwner;
            data.identity = qualificationIdentity;
            data.generation = qualificationGeneration;
            data.width = qualificationIdentity.extent.width;
            data.height = qualificationIdentity.extent.height;
            data.rowPitch = rowPitch;
        },
        [this](const ScreenSpaceOutlineQualificationCopyData& data,
               RenderGraphPassContext& context)
        {
            RHITexture* const mask = context.GetTexture(data.mask);
            RHIBuffer* const readback = context.GetBuffer(data.readback);
            if (mask == nullptr || readback == nullptr || !data.readbackOwner ||
                readback != data.readbackOwner.Get() ||
                !context.RetainSubmissionResource(
                    Ref<RefCounted>(data.readbackOwner)))
            {
                context.MarkExecutionFailed(
                    "ScreenSpaceOutline qualification readback resources are unavailable");
                return;
            }

            RHIBufferTextureCopyDesc copyDesc;
            copyDesc.bufferRowPitch = data.rowPitch;
            copyDesc.bufferImageHeight = data.height;
            copyDesc.textureRegion = {0, 0, data.width, data.height};
            context.Commands().CopyTextureToBuffer(mask, readback, copyDesc);

            if (m_probeRequested && m_probeIdentityLocked &&
                m_probeGeneration == data.generation &&
                m_probeIdentity == data.identity)
            {
                m_probeCopyRecorded = true;
                m_lastProbeStats.copyRecorded = true;
            }
        });
}

bool ScreenSpaceOutlinePass::ArmQualificationProbe() noexcept
{
    if (HasPendingQualificationProbe())
    {
        return false;
    }

    ++m_probeGeneration;
    if (m_probeGeneration == 0)
    {
        ++m_probeGeneration;
    }
    ClearQualificationProbeState(true);
    m_probeRequested = true;
    m_lastProbeStats = {};
    m_lastProbeStats.requested = true;
    return true;
}

bool ScreenSpaceOutlinePass::NotifyQualificationProbeSubmission(
    const ScreenSpaceOutlineQualificationProbeIdentity& identity,
    const GPUCompletionToken& completion,
    const RenderSubmissionTracker& tracker) noexcept
{
    if (!m_probeRequested || !m_probeIdentityLocked ||
        !m_probeOutlineDrawRecorded || !m_probeVerifierDrawRecorded ||
        !m_probeCopyRecorded || !m_probeReadback ||
        m_probeSubmissionAccepted || !identity.IsValid() ||
        identity != m_probeIdentity)
    {
        m_lastProbeStats.failure =
            "ScreenSpaceOutline qualification submission does not match a fully recorded probe";
        return false;
    }

    GPUCompletionPoint graphicsPoint;
    if (!IsExactCurrentGraphicsSubmissionToken(tracker, completion, graphicsPoint))
    {
        m_lastProbeStats.failure =
            "ScreenSpaceOutline qualification submission token is not the exact Graphics completion";
        return false;
    }

    m_probeCompletionValue = graphicsPoint.value;
    m_probeSubmissionAccepted = true;
    m_lastProbeStats.submissionAccepted = true;
    return true;
}

bool ScreenSpaceOutlinePass::MarkQualificationProbePresented(
    const ScreenSpaceOutlineQualificationProbeIdentity& identity) noexcept
{
    if (!m_probeRequested || !m_probeSubmissionAccepted ||
        !m_probeIdentityLocked || !identity.IsValid() ||
        identity != m_probeIdentity)
    {
        m_lastProbeStats.failure =
            "ScreenSpaceOutline qualification present identity does not match the submitted probe";
        return false;
    }

    m_probePresented = true;
    m_lastProbeStats.presented = true;
    return true;
}

bool ScreenSpaceOutlinePass::PollQualificationProbe(
    const RenderSubmissionTracker& tracker,
    ScreenSpaceOutlineQualificationProbeStats& outStats) noexcept
{
    outStats = m_lastProbeStats;
    if (!m_probeRequested || !m_probeSubmissionAccepted ||
        (!m_probePresented && !m_probeAbandoned) ||
        !m_probeReadback || !m_probeIdentity.IsValid() ||
        m_probeCompletionValue == 0)
    {
        return false;
    }

    const GPUCompletionPoint completionPoint{
        GPUQueueDomain::Graphics, m_probeCompletionValue};
    const GPUCompletionStatus status = tracker.Query(completionPoint);
    if (status == GPUCompletionStatus::Pending)
    {
        return false;
    }
    if (!IsCompletionSatisfied(status))
    {
        m_lastProbeStats.failure =
            "ScreenSpaceOutline qualification GPU completion was lost";
        outStats = m_lastProbeStats;
        ClearQualificationProbeState(true);
        return false;
    }
    m_lastProbeStats.gpuCompleted = true;

    if (m_probeAbandoned)
    {
        m_lastProbeStats.failure =
            "ScreenSpaceOutline qualification probe was abandoned after submission";
        outStats = m_lastProbeStats;
        ClearQualificationProbeState(true);
        return false;
    }

    const uint64 sampleCount = static_cast<uint64>(m_probeWidth) *
        static_cast<uint64>(m_probeHeight);
    const uint64 requiredBytes = static_cast<uint64>(m_probeRowPitch) *
        static_cast<uint64>(m_probeHeight);
    if (sampleCount == 0 || m_probeRowPitch < m_probeWidth ||
        requiredBytes > m_probeReadback->GetSize())
    {
        m_lastProbeStats.failure =
            "ScreenSpaceOutline qualification readback layout is invalid";
        outStats = m_lastProbeStats;
        ClearQualificationProbeState(true);
        return false;
    }

    void* const mapped = m_probeReadback->Map();
    if (mapped == nullptr)
    {
        m_lastProbeStats.failure =
            "ScreenSpaceOutline qualification readback map failed";
        outStats = m_lastProbeStats;
        ClearQualificationProbeState(true);
        return false;
    }

    const auto* const samples = static_cast<const uint8*>(mapped);
    uint64 outlinedPixelCount = 0;
    uint64 nonToonAffectedPixelCount = 0;
    bool invalidMaskValue = false;
    for (uint32 y = 0; y < m_probeHeight; ++y)
    {
        const uint8* const row = samples + static_cast<uint64>(y) *
            static_cast<uint64>(m_probeRowPitch);
        for (uint32 x = 0; x < m_probeWidth; ++x)
        {
            switch (row[x])
            {
                case 0: break;
                case 1: ++outlinedPixelCount; break;
                case 2: ++nonToonAffectedPixelCount; break;
                default: invalidMaskValue = true; break;
            }
        }
    }
    m_probeReadback->Unmap();

    if (invalidMaskValue)
    {
        m_lastProbeStats.failure =
            "ScreenSpaceOutline qualification mask contains an invalid classification";
        outStats = m_lastProbeStats;
        ClearQualificationProbeState(true);
        return false;
    }

    m_lastProbeStats.sampleCount = sampleCount;
    m_lastProbeStats.outlinedPixelCount = outlinedPixelCount;
    m_lastProbeStats.nonToonAffectedPixelCount = nonToonAffectedPixelCount;
    m_lastProbeStats.ready = true;
    m_lastProbeStats.failure.clear();
    outStats = m_lastProbeStats;
    if (!outStats.IsValid())
    {
        m_lastProbeStats.ready = false;
        m_lastProbeStats.failure =
            "ScreenSpaceOutline qualification produced incomplete statistics";
        outStats = m_lastProbeStats;
        ClearQualificationProbeState(true);
        return false;
    }

    ClearQualificationProbeState(true);
    return true;
}

bool ScreenSpaceOutlinePass::HasPendingQualificationProbe() const noexcept
{
    return m_probeRequested || m_probeIdentityLocked ||
           m_probeOutlineDrawRecorded || m_probeVerifierDrawRecorded ||
           m_probeCopyRecorded || m_probeSubmissionAccepted;
}

void ScreenSpaceOutlinePass::AbandonQualificationProbe() noexcept
{
    if (!HasPendingQualificationProbe())
    {
        return;
    }

    if (m_probeSubmissionAccepted)
    {
        // The Graphics queue may still write the imported readback buffer.
        // Keep it alive until PollQualificationProbe observes that exact point.
        m_probeAbandoned = true;
        return;
    }

    m_lastProbeStats.failure =
        "ScreenSpaceOutline qualification probe was abandoned before submission";
    ClearQualificationProbeState(true);
}

std::optional<ScreenSpaceOutlineQualificationProbeIdentity>
ScreenSpaceOutlinePass::GetPendingQualificationProbeIdentity() const noexcept
{
    if (!m_probeRequested || !m_probeIdentityLocked ||
        !m_probeOutlineDrawRecorded || !m_probeVerifierDrawRecorded ||
        !m_probeCopyRecorded || m_probeAbandoned || !m_probeIdentity.IsValid())
    {
        return std::nullopt;
    }
    return m_probeIdentity;
}

bool ScreenSpaceOutlinePass::EnsureQualificationReadback(
    uint32 width,
    uint32 height) noexcept
{
    if (m_resourceDevice == nullptr || width == 0 || height == 0)
    {
        return false;
    }

    const uint32 rowPitch = AlignReadbackRowPitch(
        m_resourceDevice->GetBackendType(), width);
    if (rowPitch == 0 || m_probeSubmissionAccepted ||
        static_cast<uint64>(height) >
            std::numeric_limits<uint64>::max() /
                static_cast<uint64>(rowPitch))
    {
        return false;
    }
    const uint64 size = static_cast<uint64>(rowPitch) *
        static_cast<uint64>(height);
    if (size == 0)
    {
        return false;
    }
    if (m_probeReadback && m_probeWidth == width && m_probeHeight == height &&
        m_probeRowPitch == rowPitch && m_probeReadback->GetSize() == size)
    {
        return true;
    }

    RHIBufferDesc desc;
    desc.size = size;
    desc.usage = RHIBufferUsage::CopyDst;
    desc.memoryType = RHIMemoryType::Readback;
    desc.debugName = "ScreenSpaceOutlineQualificationReadback";
    RHIBufferRef readback = m_resourceDevice->CreateBuffer(desc);
    if (!readback || readback->GetSize() != size)
    {
        return false;
    }

    m_probeReadback = std::move(readback);
    m_probeWidth = width;
    m_probeHeight = height;
    m_probeRowPitch = rowPitch;
    return true;
}

void ScreenSpaceOutlinePass::BeginExecutionRecording() noexcept
{
    ++m_executionGeneration;
    if (m_executionGeneration == 0)
    {
        ++m_executionGeneration;
    }
    m_lastExecutionIdentity = {};
}

void ScreenSpaceOutlinePass::ClearQualificationProbeState(
    bool retainReadback) noexcept
{
    m_probeIdentity = {};
    m_probeCompletionValue = 0;
    m_probeRequested = false;
    m_probeIdentityLocked = false;
    m_probeOutlineDrawRecorded = false;
    m_probeVerifierDrawRecorded = false;
    m_probeCopyRecorded = false;
    m_probeSubmissionAccepted = false;
    m_probePresented = false;
    m_probeAbandoned = false;
    if (!retainReadback)
    {
        m_probeReadback.Reset();
        m_probeWidth = 0;
        m_probeHeight = 0;
        m_probeRowPitch = 0;
    }
}

void ScreenSpaceOutlinePass::SetResources(PipelineCache* pipelineCache,
                                          ResourceViewCache* viewCache)
{
    m_pipelineCache = pipelineCache;
    m_viewCache = viewCache;
    IRHIDevice* const device =
        m_pipelineCache != nullptr ? m_pipelineCache->GetDevice() : nullptr;
    if (device != m_resourceDevice)
    {
        BeginExecutionRecording();
        ClearQualificationProbeState(false);
        m_lastProbeStats = {};
        m_pointClampSampler.Reset();
        m_resourceDevice = device;
    }
    RefreshSupportState();
}

void ScreenSpaceOutlinePass::RefreshSupportState()
{
    if (!IsFinitePositive(m_config.depthThreshold) ||
        !IsFinitePositive(m_config.normalThreshold) ||
        m_config.normalThreshold > 2.0f)
    {
        MarkUnsupported(
            "ScreenSpaceOutline requires finite positive depth and normal thresholds");
        return;
    }
    if (m_pipelineCache == nullptr || !m_pipelineCache->IsInitialized())
    {
        MarkUnsupported("ScreenSpaceOutline requires an initialized PipelineCache");
        return;
    }
    if (m_viewCache == nullptr || !m_viewCache->IsInitialized())
    {
        MarkUnsupported("ScreenSpaceOutline requires an initialized ResourceViewCache");
        return;
    }
    if (m_pipelineCache->GetScreenSpaceOutlinePipeline(
            RHIFormat::RGBA16_FLOAT) == nullptr ||
        m_pipelineCache->GetScreenSpaceOutlineLayout() == nullptr ||
        m_pipelineCache->GetScreenSpaceOutlineSetLayout() == nullptr ||
        !EnsureRuntimeResources())
    {
        MarkUnsupported("ScreenSpaceOutline cooked fullscreen resources are unavailable");
        return;
    }

    m_supported = true;
    m_unsupportedReason.clear();
}

bool ScreenSpaceOutlinePass::EnsureRuntimeResources()
{
    IRHIDevice* const device =
        m_pipelineCache != nullptr ? m_pipelineCache->GetDevice() : nullptr;
    if (device == nullptr)
    {
        return false;
    }
    if (!m_pointClampSampler)
    {
        RHISamplerDesc samplerDesc = RHISamplerDesc::PointClamp();
        samplerDesc.debugName = "ScreenSpaceOutlinePointClampSampler";
        m_pointClampSampler = device->CreateSampler(samplerDesc);
    }
    return m_pointClampSampler != nullptr;
}
} // namespace RVX
