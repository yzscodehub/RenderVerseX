/**
 * @file SSR.cpp
 * @brief RenderGraph-backed Hi-Z screen-space reflection implementation.
 */

#include "Render/PostProcess/SSR.h"

#include "Render/PipelineCache.h"
#include "Render/Renderer/ViewData.h"
#include "Resources/RenderSubmissionTracker.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <limits>
#include <memory>
#include <utility>

namespace RVX
{
    struct SSRExecutionState
    {
        uint32 expectedPassCount = 0;
        uint32 completedPassCount = 0;
        uint32 completedHiZPassCount = 0;
        bool neutralInputsCompleted = false;
        bool traceCompleted = false;
        bool resolveCompleted = false;
        bool qualificationCopyExpected = false;
        bool qualificationCopyCompleted = false;
        bool failed = false;
    };

namespace
{
    constexpr uint64 RVX_SSR_FNV1A_OFFSET = 14695981039346656037ULL;
    constexpr uint64 RVX_SSR_FNV1A_PRIME = 1099511628211ULL;
    constexpr uint64 RVX_SSR_CONSTANT_BUFFER_ALIGNMENT = 256;
    constexpr uint32 RVX_SSR_FLAG_REVERSE_Z = 1U << 0U;
    constexpr uint32 RVX_SSR_FLAG_NORMAL_SOURCE = 1U << 1U;
    constexpr uint32 RVX_SSR_FLAG_FIRST_PROBE_READY = 1U << 2U;
    constexpr uint32 RVX_SSR_FLAG_SCREEN_SPACE_REQUESTED = 1U << 10U;
    /**
     * Qualification-only protocol switch. The normal R8 route-mask remains
     * diagnostic-only and retains its existing classification semantics;
     * an armed physical readback opts into radiance-qualified fallback tags.
     */
    constexpr uint32 RVX_SSR_FLAG_QUALIFICATION_RADIANCE_ROUTE = 1U << 11U;

    enum class SSRStage : uint8
    {
        HiZBase = 0,
        HiZReduce,
        Trace,
        Resolve,
    };

    struct SSRGPUConstants
    {
        float renderSize[4] = {1.0f, 1.0f, 1.0f, 1.0f};
        float sourceSize[4] = {1.0f, 1.0f, 1.0f, 1.0f};
        float traceParameters[4] = {100.0f, 0.1f, 0.65f, 0.08f};
        uint32 traceControl[4] = {64U, 0U, 1U, 0U};
        float cameraPosition[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        float cameraForward[4] = {0.0f, 0.0f, -1.0f, 0.0f};
        float probeControl[4] = {1.0f, 0.0f, 0.0f, 0.0f};
        float probeWeights[2][4] = {};
        int32 probePriorities[2][4] = {};
        float probePositions[RVX_SSR_LOCAL_PROBE_CAPACITY][4] = {};
        float probeExtents[RVX_SSR_LOCAL_PROBE_CAPACITY][4] = {};
        Mat4 probeWorldTransforms[RVX_SSR_LOCAL_PROBE_CAPACITY]{};
        Mat4 probeInverseWorldTransforms[RVX_SSR_LOCAL_PROBE_CAPACITY]{};
        float probeBoxProjectionSizes[RVX_SSR_LOCAL_PROBE_CAPACITY][4] = {};
        float probeBoxProjectionOffsets[RVX_SSR_LOCAL_PROBE_CAPACITY][4] = {};
        float probeInfluenceModes[RVX_SSR_LOCAL_PROBE_CAPACITY][4] = {};
        Mat4 inverseViewProjection = Mat4Identity();
        Mat4 viewProjection = Mat4Identity();
    };

    static_assert(offsetof(SSRGPUConstants, sourceSize) == 16U);
    static_assert(offsetof(SSRGPUConstants, traceControl) == 48U);
    static_assert(offsetof(SSRGPUConstants, probeControl) == 96U);
    static_assert(offsetof(SSRGPUConstants, inverseViewProjection) == 1840U);
    static_assert(sizeof(SSRGPUConstants) == 1968U,
                  "SSR constants must match the HLSL cbuffer ABI");

    struct SSRPassData
    {
        RGTextureHandle sceneColor{};
        RGTextureHandle stageSource{};
        RGTextureHandle normalRoughness{};
        RGTextureHandle hiZ{};
        RGTextureHandle rawReflection{};
        RGTextureHandle confidence{};
        std::array<RGTextureHandle, RVX_SSR_LOCAL_PROBE_CAPACITY> probes{};
        RGTextureHandle baseIndirectSpecular{};
        RGTextureHandle specularResponse{};
        RGBufferHandle exposureState{};
        RGTextureHandle output0{};
        RGTextureHandle output1{};
        RGTextureViewHandle sceneColorView{};
        RGTextureViewHandle stageSourceView{};
        RGTextureViewHandle normalRoughnessView{};
        RGTextureViewHandle hiZView{};
        RGTextureViewHandle rawReflectionView{};
        RGTextureViewHandle confidenceView{};
        std::array<RGTextureViewHandle, RVX_SSR_LOCAL_PROBE_CAPACITY>
            probeViews{};
        RGTextureViewHandle baseIndirectSpecularView{};
        RGTextureViewHandle specularResponseView{};
        RGTextureViewHandle output0View{};
        RGTextureViewHandle output1View{};
        SSRGPUConstants constants{};
        SSRStage stage = SSRStage::HiZBase;
        IRHIDevice* device = nullptr;
        RHIPipelineRef pipeline;
        RHIPipelineLayoutRef pipelineLayout;
        RHIDescriptorSetLayoutRef setLayout;
        RHISamplerRef sampler;
        std::shared_ptr<SSRExecutionState> executionState;
        uint64 exposureStateContentHash = 0;
        bool resourcesRetained = false;
    };

    struct SSRNeutralInputsPassData
    {
        RGTextureHandle rawReflection{};
        RGTextureHandle confidence{};
        RGTextureViewHandle rawReflectionView{};
        RGTextureViewHandle confidenceView{};
        std::shared_ptr<SSRExecutionState> executionState;
    };

    struct SSRQualificationCopyData
    {
        RGTextureHandle routeMask{};
        RGBufferHandle readback{};
        RHIBufferRef readbackOwner;
        SSRQualificationProbeIdentity identity{};
        uint64 generation = 0;
        uint32 width = 0;
        uint32 height = 0;
        uint32 rowPitch = 0;
        std::shared_ptr<SSRExecutionState> executionState;
    };

    [[nodiscard]] uint64 AlignConstantBufferSize(uint64 size) noexcept
    {
        return (size + RVX_SSR_CONSTANT_BUFFER_ALIGNMENT - 1U) &
               ~(RVX_SSR_CONSTANT_BUFFER_ALIGNMENT - 1U);
    }

    [[nodiscard]] uint32 AlignReadbackRowPitch(
        RHIBackendType backend,
        uint32 width) noexcept
    {
        const uint32 alignment = backend == RHIBackendType::DX12 ||
                                      backend == RHIBackendType::Metal
                                  ? 256U
                                  : 1U;
        const uint64 aligned =
            (static_cast<uint64>(width) + alignment - 1U) &
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
        if (!MergeGPUCompletionToken(normalized, token) ||
            normalized.count == 0 ||
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

    [[nodiscard]] uint32 CalculateMipCount(uint32 width, uint32 height) noexcept
    {
        uint32 extent = std::max(width, height);
        uint32 count = 0;
        while (extent != 0)
        {
            ++count;
            extent >>= 1U;
        }
        return std::max(1U, count);
    }

    [[nodiscard]] bool IsFinitePositive(float value) noexcept
    {
        return std::isfinite(value) && value > 0.0f;
    }

    [[nodiscard]] bool IsFiniteNonNegative(float value) noexcept
    {
        return std::isfinite(value) && value >= 0.0f;
    }

    [[nodiscard]] bool IsFiniteVec3(const Vec3& value) noexcept
    {
        return std::isfinite(value.x) && std::isfinite(value.y) &&
               std::isfinite(value.z);
    }

    [[nodiscard]] bool IsFiniteMat4(const Mat4& value) noexcept
    {
        for (uint32 column = 0; column < 4U; ++column)
        {
            for (uint32 row = 0; row < 4U; ++row)
            {
                if (!std::isfinite(value[column][row]))
                {
                    return false;
                }
            }
        }
        return true;
    }

    [[nodiscard]] RHIBufferRef CreateImmutableConstants(
        IRHIDevice& device,
        const SSRGPUConstants& constants,
        const char* debugName)
    {
        RHIBufferDesc desc;
        desc.size = AlignConstantBufferSize(sizeof(SSRGPUConstants));
        desc.usage = RHIBufferUsage::Constant;
        desc.memoryType = RHIMemoryType::Upload;
        desc.debugName = debugName;
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
        buffer->Unmap();
        return buffer;
    }

    [[nodiscard]] RGTextureViewHandle AddTextureSRV(
        RenderGraphBuilder& builder,
        RGTextureHandle texture,
        const char* debugName,
        const RHISubresourceRange& range = RHISubresourceRange::All())
    {
        const RHITextureDesc* const desc = builder.GetTextureDesc(texture);
        if (desc == nullptr)
        {
            return {};
        }
        RHITextureViewDesc viewDesc;
        viewDesc.format = desc->format;
        viewDesc.dimension = desc->dimension;
        viewDesc.subresourceRange = range;
        if (IsDepthFormat(viewDesc.format))
        {
            viewDesc.subresourceRange.aspect = RHITextureAspect::Depth;
        }
        viewDesc.type = RHITextureViewType::ShaderResource;
        viewDesc.debugName = debugName;
        return builder.Read(
            builder.CreateTextureView(texture, viewDesc),
            MakeRGAccessDesc(RHIResourceState::ShaderResource,
                             RHIShaderStage::Pixel));
    }

    [[nodiscard]] RGTextureViewHandle AddTextureRTV(
        RenderGraphBuilder& builder,
        RGTextureHandle texture,
        const char* debugName,
        const RHISubresourceRange& range = RHISubresourceRange::All())
    {
        const RHITextureDesc* const desc = builder.GetTextureDesc(texture);
        if (desc == nullptr)
        {
            return {};
        }
        RHITextureViewDesc viewDesc;
        viewDesc.format = desc->format;
        viewDesc.dimension = desc->dimension;
        viewDesc.subresourceRange = range;
        viewDesc.type = RHITextureViewType::RenderTarget;
        viewDesc.debugName = debugName;
        return builder.Write(
            builder.CreateTextureView(texture, viewDesc),
            MakeRGAccessDesc(RHIResourceState::RenderTarget,
                             RHIShaderStage::Pixel,
                             RHIDiscardIntent::Discard));
    }

    [[nodiscard]] RHIPipeline* ResolvePipeline(
        PipelineCache& cache,
        SSRStage stage) noexcept
    {
        switch (stage)
        {
            case SSRStage::HiZBase: return cache.GetSSRHiZBasePipeline();
            case SSRStage::HiZReduce: return cache.GetSSRHiZReducePipeline();
            case SSRStage::Trace: return cache.GetSSRTracePipeline();
            case SSRStage::Resolve: return cache.GetSSRResolvePipeline();
        }
        return nullptr;
    }

    [[nodiscard]] const char* GetPassConstantsName(SSRStage stage) noexcept
    {
        switch (stage)
        {
            case SSRStage::HiZBase: return "SSRHiZBaseConstants";
            case SSRStage::HiZReduce: return "SSRHiZReduceConstants";
            case SSRStage::Trace: return "SSRTraceConstants";
            case SSRStage::Resolve: return "SSRResolveConstants";
        }
        return "SSRConstants";
    }

    [[nodiscard]] const char* GetDescriptorName(SSRStage stage) noexcept
    {
        switch (stage)
        {
            case SSRStage::HiZBase: return "SSRHiZBaseDescriptorSet";
            case SSRStage::HiZReduce: return "SSRHiZReduceDescriptorSet";
            case SSRStage::Trace: return "SSRTraceDescriptorSet";
            case SSRStage::Resolve: return "SSRResolveDescriptorSet";
        }
        return "SSRDescriptorSet";
    }

    [[nodiscard]] bool IsFullResolution2D(
        const RHITextureDesc& desc,
        uint32 width,
        uint32 height,
        RHIFormat format) noexcept
    {
        return desc.dimension == RHITextureDimension::Texture2D &&
               desc.width == width && desc.height == height &&
               desc.sampleCount == RHISampleCount::Count1 &&
               desc.format == format;
    }

    void MixIdentity(uint64& hash, uint64 value) noexcept
    {
        for (uint32 byteIndex = 0; byteIndex < 8U; ++byteIndex)
        {
            hash ^= static_cast<uint8>(value >> (byteIndex * 8U));
            hash *= RVX_SSR_FNV1A_PRIME;
        }
    }

    [[nodiscard]] uint64 FinalizeIdentity(uint64 hash) noexcept
    {
        return hash == 0 ? 1 : hash;
    }

    [[nodiscard]] uint64 ComputeReflectionSurfaceIdentity(
        RGTextureHandle baseIndirectSpecular,
        RGTextureHandle specularResponse,
        RGBufferHandle exposureState,
        uint64 exposureStateContentHash) noexcept
    {
        if (!baseIndirectSpecular.IsValid() || !specularResponse.IsValid() ||
            !exposureState.IsValid() || exposureStateContentHash == 0)
        {
            return 0;
        }
        uint64 hash = RVX_SSR_FNV1A_OFFSET;
        MixIdentity(hash, 2U);
        for (const uint64 value : std::array<uint64, 10>{
                 baseIndirectSpecular.index,
                 baseIndirectSpecular.graphIdentity,
                 baseIndirectSpecular.recordingGeneration,
                 specularResponse.index,
                 specularResponse.graphIdentity,
                 specularResponse.recordingGeneration,
                 exposureState.index,
                 exposureState.graphIdentity,
                 exposureState.recordingGeneration,
                 exposureStateContentHash})
        {
            MixIdentity(hash, value);
        }
        return FinalizeIdentity(hash);
    }
} // namespace

uint64 ComputeSSRConfigurationIdentity(const SSRConfig& config) noexcept
{
    uint64 hash = RVX_SSR_FNV1A_OFFSET;
    MixIdentity(hash, 1U);
    MixIdentity(hash, static_cast<uint64>(config.quality));
    MixIdentity(hash, std::bit_cast<uint32>(config.maxDistance));
    MixIdentity(hash, std::bit_cast<uint32>(config.thickness));
    MixIdentity(hash, config.maxSteps);
    MixIdentity(hash, config.binarySearchSteps);
    MixIdentity(hash, std::bit_cast<uint32>(config.roughnessThreshold));
    MixIdentity(hash, std::bit_cast<uint32>(config.edgeFade));
    MixIdentity(hash, config.halfResolution ? 1U : 0U);
    MixIdentity(hash, config.temporalFilter ? 1U : 0U);
    MixIdentity(hash, config.stochastic ? 1U : 0U);
    return FinalizeIdentity(hash);
}

uint64 ComputeSSRPipelineIdentity(
    uint64 hiZBasePipelineHash,
    uint64 hiZReducePipelineHash,
    uint64 tracePipelineHash,
    uint64 resolvePipelineHash) noexcept
{
    if (hiZBasePipelineHash == 0 || hiZReducePipelineHash == 0 ||
        tracePipelineHash == 0 || resolvePipelineHash == 0)
    {
        return 0;
    }

    uint64 hash = RVX_SSR_FNV1A_OFFSET;
    MixIdentity(hash, 1U);
    MixIdentity(hash, hiZBasePipelineHash);
    MixIdentity(hash, hiZReducePipelineHash);
    MixIdentity(hash, tracePipelineHash);
    MixIdentity(hash, resolvePipelineHash);
    return FinalizeIdentity(hash);
}

std::optional<SSRConfig> ResolveSSRQualityPolicy(
    RenderQualityTier quality) noexcept
{
    // Keep every non-budget quality parameter at the established High
    // defaults. The resolver is the only place where a quality tier changes
    // trace work, so a plan and its graph execution cannot silently diverge.
    SSRConfig config;
    switch (quality)
    {
        case RenderQualityTier::Low:
            config.quality = SSRQuality::Low;
            config.maxSteps = 32U;
            config.binarySearchSteps = 4U;
            break;
        case RenderQualityTier::Medium:
            config.quality = SSRQuality::Medium;
            config.maxSteps = 48U;
            config.binarySearchSteps = 5U;
            break;
        case RenderQualityTier::High:
            config.quality = SSRQuality::High;
            config.maxSteps = 64U;
            config.binarySearchSteps = 6U;
            break;
        case RenderQualityTier::Ultra:
            config.quality = SSRQuality::Ultra;
            config.maxSteps = 96U;
            config.binarySearchSteps = 8U;
            break;
        case RenderQualityTier::Count:
        default:
            // An unknown public tier must never inherit the historical High
            // trace budget. Callers reject this before plan or graph work.
            return std::nullopt;
    }
    return config;
}

uint64 ComputeSSRDepthPyramidIdentity(
    const RenderFrameIdentity& frameIdentity,
    const RenderViewIdentity& viewIdentity,
    const RenderOutputExtent& extent,
    uint64 configurationIdentity,
    uint64 pipelineIdentity) noexcept
{
    if (!frameIdentity.IsValid() || !viewIdentity.IsValid() ||
        !extent.IsValid() || configurationIdentity == 0 ||
        pipelineIdentity == 0)
    {
        return 0;
    }
    uint64 hash = RVX_SSR_FNV1A_OFFSET;
    MixIdentity(hash, 1U);
    MixIdentity(hash, frameIdentity.frameSequence);
    MixIdentity(hash, frameIdentity.sceneRevision);
    MixIdentity(hash, frameIdentity.surfaceGeneration);
    MixIdentity(hash, frameIdentity.configurationRevision);
    MixIdentity(hash, viewIdentity.sceneRuntimeId);
    MixIdentity(hash, viewIdentity.cameraEntityPacked);
    MixIdentity(hash, viewIdentity.subViewIndex);
    MixIdentity(hash, static_cast<uint64>(viewIdentity.role));
    MixIdentity(hash, extent.width);
    MixIdentity(hash, extent.height);
    MixIdentity(hash, configurationIdentity);
    MixIdentity(hash, pipelineIdentity);
    return FinalizeIdentity(hash);
}

const char* GetSSRImplementationTierName(SSRImplementationTier tier) noexcept
{
    switch (tier)
    {
        case SSRImplementationTier::Unsupported: return "Unsupported";
        case SSRImplementationTier::HiZRayMarch: return "HiZRayMarch";
    }
    return "Unknown";
}

const char* GetReflectionResolveRouteName(
    ReflectionResolveRoute route) noexcept
{
    switch (route)
    {
        case ReflectionResolveRoute::Unresolved: return "Unresolved";
        case ReflectionResolveRoute::Background: return "Background";
        case ReflectionResolveRoute::NonReflective: return "NonReflective";
        case ReflectionResolveRoute::ScreenSpace: return "ScreenSpace";
        case ReflectionResolveRoute::LocalProbe: return "LocalProbe";
        case ReflectionResolveRoute::GlobalIBL: return "GlobalIBL";
    }
    return "Unknown";
}

SSR::~SSR()
{
    Shutdown();
}

void SSR::SetResources(PipelineCache* pipelineCache)
{
    IRHIDevice* const device = pipelineCache != nullptr
        ? pipelineCache->GetDevice() : nullptr;
    if (device != m_resourceDevice)
    {
        ClearQualificationProbeState(false);
        m_lastProbeStats = {};
        m_sampler.Reset();
        m_resourceDevice = device;
    }
    if (pipelineCache == m_pipelineCache &&
        (device == nullptr || m_sampler))
    {
        return;
    }
    m_sampler.Reset();
    m_pipelineCache = pipelineCache;
    if (device != nullptr)
    {
        RHISamplerDesc desc = RHISamplerDesc::LinearClamp();
        desc.debugName = "SSRLinearClampSampler";
        m_sampler = device->CreateSampler(desc);
    }
}

void SSR::Shutdown() noexcept
{
    m_pendingExecutionState.reset();
    ClearQualificationProbeState(false);
    m_lastProbeStats = {};
    m_sampler.Reset();
    m_pipelineCache = nullptr;
    m_resourceDevice = nullptr;
    m_config = {};
    m_hasResolvedQualityPolicy = false;
    m_lastComputeStats = {};
    m_unsupportedReason = "SSR renderer resources are unavailable";
}

bool SSR::SetResolvedQualityPolicy(
    RenderQualityTier quality,
    const SSRConfig& config) noexcept
{
    const std::optional<SSRConfig> expected =
        ResolveSSRQualityPolicy(quality);
    if (!expected.has_value() || config != *expected)
    {
        m_config = {};
        m_hasResolvedQualityPolicy = false;
        return false;
    }
    m_config = config;
    m_hasResolvedQualityPolicy = true;
    return true;
}

bool SSR::ValidateConfig() const noexcept
{
    return m_hasResolvedQualityPolicy &&
           m_config.quality >= SSRQuality::Low &&
           m_config.quality <= SSRQuality::Ultra &&
           IsFinitePositive(m_config.maxDistance) &&
           IsFinitePositive(m_config.thickness) &&
           m_config.maxSteps >= 1U && m_config.maxSteps <= 256U &&
           m_config.binarySearchSteps <= 16U &&
           IsFinitePositive(m_config.roughnessThreshold) &&
           m_config.roughnessThreshold <= 1.0f &&
           IsFiniteNonNegative(m_config.edgeFade) &&
           m_config.edgeFade <= 0.5f && !m_config.halfResolution &&
           !m_config.temporalFilter && !m_config.stochastic;
}

bool SSR::HasPipelineResources() const noexcept
{
    return m_pipelineCache != nullptr && m_pipelineCache->IsInitialized() &&
           m_pipelineCache->GetDevice() != nullptr && m_sampler &&
           m_pipelineCache->GetSSRHiZBasePipeline() != nullptr &&
           m_pipelineCache->GetSSRHiZReducePipeline() != nullptr &&
           m_pipelineCache->GetSSRTracePipeline() != nullptr &&
           m_pipelineCache->GetSSRResolvePipeline() != nullptr &&
           m_pipelineCache->GetSSRLayout() != nullptr &&
           m_pipelineCache->GetSSRSetLayout() != nullptr;
}

bool SSR::IsSupported() const noexcept
{
    if (!m_hasResolvedQualityPolicy)
    {
        m_unsupportedReason =
            "SSR requires an injected resolved quality policy";
        return false;
    }
    if (!ValidateConfig())
    {
        m_unsupportedReason =
            "SSR configuration requests an unimplemented or invalid mode";
        return false;
    }
    if (!HasPipelineResources())
    {
        m_unsupportedReason =
            "SSR cooked pipeline family, descriptor layout, or sampler is unavailable";
        return false;
    }
    m_unsupportedReason.clear();
    return true;
}

void SSR::RecordUnsupported(std::string diagnostic)
{
    m_lastComputeStats.supported = false;
    m_lastComputeStats.executed = false;
    m_lastComputeStats.fallbackReason = std::move(diagnostic);
    m_unsupportedReason = m_lastComputeStats.fallbackReason;
}

SSROutput SSR::AddToGraph(
    RenderGraph& graph,
    const ViewData& view,
    const SSRFrameInputs& inputs)
{
    m_pendingExecutionState.reset();
    m_lastComputeStats = {};
    // Keep immutable frame intent distinct from the subset of inputs that
    // permits a Hi-Z trace. A plan may intentionally request SSR yet be
    // unready for this frame; that is a visible typed-IBL fallback, never an
    // implicit withdrawal of the request.
    const bool screenSpaceRequested = inputs.screenSpaceRequested;
    const bool screenSpaceTraceInputsReady = screenSpaceRequested &&
        inputs.screenSpaceTraceInputsReady;
    const bool screenSpaceTraceScheduled = screenSpaceTraceInputsReady &&
        m_enabled;
    uint32 readyProbeCount = 0;
    for (const bool ready : inputs.probeReady)
    {
        readyProbeCount += ready ? 1U : 0U;
    }
    m_lastComputeStats.requested = screenSpaceRequested || readyProbeCount != 0;
    m_lastComputeStats.screenSpaceRequested = screenSpaceRequested;
    m_lastComputeStats.screenSpaceTraceInputsReady =
        screenSpaceTraceInputsReady;
    m_lastComputeStats.readyProbeCount = readyProbeCount;
    m_lastComputeStats.colorAvailable = inputs.sceneColor.IsValid();
    m_lastComputeStats.depthAvailable = inputs.deviceDepth.IsValid();
    m_lastComputeStats.normalAvailable = inputs.normalRoughness.IsValid();
    m_lastComputeStats.roughnessAvailable = inputs.normalRoughness.IsValid();
    m_lastComputeStats.temporalHistoryRequired =
        screenSpaceTraceScheduled && m_config.temporalFilter;
    m_lastComputeStats.globalIBLReady =
        inputs.baseIndirectSpecular.IsValid() &&
        inputs.specularResponse.IsValid() && inputs.exposureState.IsValid() &&
        inputs.exposureStateContentHash != 0;
    m_lastComputeStats.reflectionPlanIdentity = inputs.reflectionPlanIdentity;
    m_lastComputeStats.lightingPlanIdentity = inputs.lightingPlanIdentity;
    m_lastComputeStats.configurationIdentity =
        m_hasResolvedQualityPolicy ? inputs.configurationIdentity : 0;
    m_lastComputeStats.pipelineIdentity =
        m_hasResolvedQualityPolicy ? inputs.pipelineIdentity : 0;
    m_lastComputeStats.depthPyramidIdentity =
        screenSpaceTraceScheduled ? inputs.depthPyramidIdentity : 0;
    m_lastComputeStats.reflectionSurfaceIdentity =
        ComputeReflectionSurfaceIdentity(
            inputs.baseIndirectSpecular, inputs.specularResponse,
            inputs.exposureState, inputs.exposureStateContentHash);
    m_lastComputeStats.exposureStateContentHash =
        inputs.exposureStateContentHash;
    if (!m_lastComputeStats.requested)
    {
        RecordUnsupported(
            "Reflection resolve has neither screen-space nor local-probe work");
        return {};
    }
    if (screenSpaceRequested && !screenSpaceTraceScheduled &&
        readyProbeCount == 0)
    {
        m_lastComputeStats.missingInputReason =
            inputs.screenSpaceTraceInputsReady
                ? "SSR trace is disabled; preserving typed IBL fallback"
                : "SSR plan trace inputs are unavailable; preserving typed IBL fallback";
        RecordUnsupported(m_lastComputeStats.missingInputReason);
        return {};
    }
    if (screenSpaceTraceScheduled)
    {
        const uint64 expectedConfigurationIdentity =
            ComputeSSRConfigurationIdentity(m_config);
        const uint64 expectedDepthPyramidIdentity =
            ComputeSSRDepthPyramidIdentity(
                inputs.frameIdentity, inputs.viewIdentity,
                RenderOutputExtent{view.viewportWidth, view.viewportHeight},
                expectedConfigurationIdentity, inputs.pipelineIdentity);
        if (inputs.configurationIdentity == 0 ||
            inputs.configurationIdentity != expectedConfigurationIdentity ||
            inputs.pipelineIdentity == 0 ||
            inputs.depthPyramidIdentity == 0 ||
            expectedDepthPyramidIdentity == 0 ||
            inputs.depthPyramidIdentity != expectedDepthPyramidIdentity)
        {
            m_lastComputeStats.missingInputReason =
                "SSR policy, pipeline, or depth-pyramid identity is stale or mismatched";
            RecordUnsupported(m_lastComputeStats.missingInputReason);
            return {};
        }
    }
    if (!IsSupported())
    {
        RecordUnsupported(m_unsupportedReason);
        return {};
    }
    m_lastComputeStats.supported = true;
    m_lastComputeStats.implementationTier = SSRImplementationTier::HiZRayMarch;

    const uint32 width = view.viewportWidth;
    const uint32 height = view.viewportHeight;
    const RHITextureDesc* const sceneDesc = graph.GetTextureDesc(inputs.sceneColor);
    const RHITextureDesc* const depthDesc = graph.GetTextureDesc(inputs.deviceDepth);
    if (!inputs.frameIdentity.IsValid() || !inputs.viewIdentity.IsValid() ||
        inputs.reflectionPlanIdentity == 0 || inputs.lightingPlanIdentity == 0 ||
        (screenSpaceTraceScheduled && inputs.depthPyramidIdentity == 0) ||
        width == 0 || height == 0 || sceneDesc == nullptr || depthDesc == nullptr ||
        !IsFullResolution2D(*sceneDesc, width, height, RHIFormat::RGBA16_FLOAT) ||
        !IsFullResolution2D(*depthDesc, width, height, RHIFormat::D32_FLOAT) ||
        !HasFlag(sceneDesc->usage, RHITextureUsage::ShaderResource) ||
        !HasFlag(depthDesc->usage, RHITextureUsage::ShaderResource))
    {
        m_lastComputeStats.missingInputReason =
            "SSR requires current-frame RGBA16F scene color, shader-readable D32 depth, and valid identities";
        RecordUnsupported(m_lastComputeStats.missingInputReason);
        return {};
    }

    RGTextureHandle normalRoughness = inputs.normalRoughness;
    if (normalRoughness.IsValid())
    {
        const RHITextureDesc* const normalDesc = graph.GetTextureDesc(normalRoughness);
        if (normalDesc == nullptr ||
            !IsFullResolution2D(*normalDesc, width, height,
                                RHIFormat::RGBA8_UNORM) ||
            !HasFlag(normalDesc->usage, RHITextureUsage::ShaderResource))
        {
            m_lastComputeStats.missingInputReason =
                "SSR normal/roughness input must be full-resolution RGBA8_UNORM GBuffer1";
            RecordUnsupported(m_lastComputeStats.missingInputReason);
            return {};
        }
    }
    else
    {
        normalRoughness = inputs.sceneColor;
    }

    const RHITextureDesc* const baseIndirectSpecularDesc =
        graph.GetTextureDesc(inputs.baseIndirectSpecular);
    const RHITextureDesc* const specularResponseDesc =
        graph.GetTextureDesc(inputs.specularResponse);
    const RHIBufferDesc* const exposureStateDesc =
        graph.GetBufferDesc(inputs.exposureState);
    if (baseIndirectSpecularDesc == nullptr ||
        specularResponseDesc == nullptr || exposureStateDesc == nullptr ||
        inputs.exposureStateContentHash == 0 ||
        !IsFullResolution2D(*baseIndirectSpecularDesc, width, height,
                            RHIFormat::RGBA16_FLOAT) ||
        !IsFullResolution2D(*specularResponseDesc, width, height,
                            RHIFormat::RGBA16_FLOAT) ||
        !HasFlag(baseIndirectSpecularDesc->usage,
                 RHITextureUsage::ShaderResource) ||
        !HasFlag(specularResponseDesc->usage,
                 RHITextureUsage::ShaderResource) ||
        exposureStateDesc->size < 48U || exposureStateDesc->stride != 48U ||
        !HasFlag(exposureStateDesc->usage, RHIBufferUsage::ShaderResource))
    {
        RecordUnsupported(
            "SSR requires the exact full-resolution RGBA16F base-indirect/specular-response surfaces and committed exposure state");
        return {};
    }
    for (uint32 probeIndex = 0;
         probeIndex < RVX_SSR_LOCAL_PROBE_CAPACITY;
         ++probeIndex)
    {
        if (!IsFiniteVec3(inputs.probePositions[probeIndex]) ||
            !IsFiniteVec3(inputs.probeExtents[probeIndex]) ||
            !IsFiniteVec3(inputs.probeBoxProjectionSizes[probeIndex]) ||
            !IsFiniteVec3(inputs.probeBoxProjectionOffsets[probeIndex]) ||
            !IsFiniteMat4(inputs.probeWorldTransforms[probeIndex]) ||
            !IsFiniteMat4(inputs.probeInverseWorldTransforms[probeIndex]) ||
            !IsFiniteNonNegative(inputs.probeWeights[probeIndex]) ||
            !IsFiniteNonNegative(
                inputs.probeNormalizedBlendDistances[probeIndex]) ||
            inputs.probeNormalizedBlendDistances[probeIndex] > 1.0f ||
            (inputs.probeShapes[probeIndex] !=
                 SSRProbeInfluenceShape::Box &&
             inputs.probeShapes[probeIndex] !=
                 SSRProbeInfluenceShape::Sphere) ||
            (inputs.probeReady[probeIndex] &&
             (inputs.probeExtents[probeIndex].x <= 0.0f ||
              inputs.probeExtents[probeIndex].y <= 0.0f ||
              inputs.probeExtents[probeIndex].z <= 0.0f)) ||
            (inputs.probeReady[probeIndex] &&
             inputs.probeUsesBoxProjection[probeIndex] &&
             (inputs.probeBoxProjectionSizes[probeIndex].x <= 0.0f ||
              inputs.probeBoxProjectionSizes[probeIndex].y <= 0.0f ||
              inputs.probeBoxProjectionSizes[probeIndex].z <= 0.0f)))
        {
            RecordUnsupported(
                "SSR local-probe parameters are non-finite or negative");
            return {};
        }
    }

    RHITextureRef descriptorFallback =
        m_pipelineCache->GetFallbackEnvironmentIBLCubemapTexture();
    if (!descriptorFallback)
    {
        RecordUnsupported(
            "SSR local-probe descriptor filler cubemap is unavailable");
        return {};
    }
    const RGTextureHandle descriptorFallbackHandle = graph.ImportTexture(
        std::move(descriptorFallback), RHIResourceState::ShaderResource);
    if (!descriptorFallbackHandle.IsValid())
    {
        RecordUnsupported("SSR local-probe descriptor filler import failed");
        return {};
    }
    m_lastComputeStats.globalIBLReady = true;

    std::array<RGTextureHandle, RVX_SSR_LOCAL_PROBE_CAPACITY> probes{};
    uint32 localProbeMipLevels = std::numeric_limits<uint32>::max();
    for (uint32 probeIndex = 0;
         probeIndex < RVX_SSR_LOCAL_PROBE_CAPACITY;
         ++probeIndex)
    {
        probes[probeIndex] = inputs.probeReady[probeIndex]
            ? inputs.probeTextures[probeIndex] : descriptorFallbackHandle;
        if (inputs.probeReady[probeIndex])
        {
            const RHITextureDesc* const probeDesc =
                graph.GetTextureDesc(inputs.probeTextures[probeIndex]);
            localProbeMipLevels = probeDesc != nullptr
                ? std::min(localProbeMipLevels,
                           std::max(1U, probeDesc->mipLevels))
                : 0U;
        }
    }
    const auto validProbe = [&graph](RGTextureHandle probe)
    {
        const RHITextureDesc* const desc = graph.GetTextureDesc(probe);
        return desc != nullptr &&
               desc->dimension == RHITextureDimension::TextureCube &&
               HasFlag(desc->usage, RHITextureUsage::ShaderResource);
    };
    if (!std::all_of(probes.begin(), probes.end(), validProbe))
    {
        RecordUnsupported("SSR admitted reflection probe is not a shader-readable cubemap");
        return {};
    }
    if (readyProbeCount == 0)
    {
        localProbeMipLevels = 1U;
    }
    if (localProbeMipLevels == 0 ||
        localProbeMipLevels == std::numeric_limits<uint32>::max())
    {
        RecordUnsupported("SSR local-probe mip contract is invalid");
        return {};
    }

    const uint32 mipCount = screenSpaceTraceScheduled
        ? CalculateMipCount(width, height) : 0U;
    RGTextureHandle hiZ{};
    if (screenSpaceTraceScheduled)
    {
        RHITextureDesc hiZDesc = RHITextureDesc::Texture2D(
            width, height, RHIFormat::R32_FLOAT,
            RHITextureUsage::RenderTarget | RHITextureUsage::ShaderResource);
        hiZDesc.mipLevels = mipCount;
        hiZDesc.debugName = "SSRHiZPyramid";
        hiZ = graph.CreateTexture(hiZDesc);
    }

    RHITextureDesc reflectionDesc = RHITextureDesc::Texture2D(
        width, height, RHIFormat::RGBA16_FLOAT,
        RHITextureUsage::RenderTarget | RHITextureUsage::ShaderResource);
    reflectionDesc.debugName = "SSRRawReflection";
    const RGTextureHandle rawReflection = graph.CreateTexture(reflectionDesc);
    reflectionDesc.debugName = "SSRResolvedSceneColor";
    const RGTextureHandle resolvedSceneColor = graph.CreateTexture(reflectionDesc);
    RHITextureDesc confidenceDesc = RHITextureDesc::Texture2D(
        width, height, RHIFormat::R8_UNORM,
        RHITextureUsage::RenderTarget | RHITextureUsage::ShaderResource);
    confidenceDesc.debugName = "SSRConfidence";
    const RGTextureHandle confidence = graph.CreateTexture(confidenceDesc);
    RHITextureDesc routeMaskDesc = confidenceDesc;
    routeMaskDesc.usage =
        routeMaskDesc.usage | RHITextureUsage::CopySrc;
    routeMaskDesc.debugName = "SSRReflectionRouteMask";
    const RGTextureHandle routeMask = graph.CreateTexture(routeMaskDesc);
    if ((screenSpaceTraceScheduled && !hiZ.IsValid()) ||
        !rawReflection.IsValid() ||
        !resolvedSceneColor.IsValid() || !confidence.IsValid() ||
        !routeMask.IsValid())
    {
        RecordUnsupported("SSR graph resource allocation failed");
        return {};
    }

    bool qualificationRequested = false;
    SSRQualificationProbeIdentity qualificationIdentity{};
    uint64 qualificationGeneration = 0;
    RGBufferHandle qualificationReadback{};
    if (screenSpaceTraceScheduled && m_probeRequested && !m_probeIdentityLocked)
    {
        qualificationIdentity.frameIdentity = inputs.frameIdentity;
        qualificationIdentity.viewIdentity = inputs.viewIdentity;
        qualificationIdentity.extent = RenderOutputExtent{width, height};
        qualificationIdentity.reflectionPlanIdentity =
            inputs.reflectionPlanIdentity;
        qualificationIdentity.lightingPlanIdentity =
            inputs.lightingPlanIdentity;
        qualificationIdentity.depthPyramidIdentity =
            inputs.depthPyramidIdentity;
        qualificationIdentity.configurationIdentity =
            inputs.configurationIdentity;
        qualificationIdentity.pipelineIdentity = inputs.pipelineIdentity;
        qualificationIdentity.reflectionSurfaceIdentity =
            m_lastComputeStats.reflectionSurfaceIdentity;
        qualificationIdentity.exposureStateContentHash =
            inputs.exposureStateContentHash;
        if (!qualificationIdentity.IsValid() ||
            !EnsureQualificationReadback(width, height))
        {
            m_lastProbeStats.failure =
                "SSR qualification route readback allocation or identity failed";
            ClearQualificationProbeState(true);
        }
        else
        {
            qualificationReadback = graph.ImportBuffer(
                RHIBufferRef(m_probeReadback), RHIResourceState::CopyDest);
            if (!qualificationReadback.IsValid())
            {
                m_lastProbeStats.failure =
                    "SSR qualification readback import failed";
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
    }

    SSRGPUConstants baseConstants;
    baseConstants.renderSize[0] = static_cast<float>(width);
    baseConstants.renderSize[1] = static_cast<float>(height);
    baseConstants.renderSize[2] = 1.0f / baseConstants.renderSize[0];
    baseConstants.renderSize[3] = 1.0f / baseConstants.renderSize[1];
    baseConstants.sourceSize[0] = baseConstants.renderSize[0];
    baseConstants.sourceSize[1] = baseConstants.renderSize[1];
    baseConstants.sourceSize[2] = baseConstants.renderSize[2];
    baseConstants.sourceSize[3] = baseConstants.renderSize[3];
    baseConstants.traceParameters[0] = m_config.maxDistance;
    baseConstants.traceParameters[1] = m_config.thickness;
    baseConstants.traceParameters[2] = m_config.roughnessThreshold;
    baseConstants.traceParameters[3] = m_config.edgeFade;
    baseConstants.traceControl[0] = m_config.maxSteps;
    baseConstants.traceControl[1] = m_config.binarySearchSteps;
    baseConstants.traceControl[2] = std::max(1U, mipCount);
    baseConstants.traceControl[3] =
        (m_pipelineCache->IsReverseZ() ? RVX_SSR_FLAG_REVERSE_Z : 0U) |
        (inputs.normalRoughness.IsValid() ? RVX_SSR_FLAG_NORMAL_SOURCE : 0U) |
        (screenSpaceTraceScheduled
             ? RVX_SSR_FLAG_SCREEN_SPACE_REQUESTED : 0U) |
        (qualificationRequested
             ? RVX_SSR_FLAG_QUALIFICATION_RADIANCE_ROUTE : 0U);
    baseConstants.cameraPosition[0] = view.cameraPosition.x;
    baseConstants.cameraPosition[1] = view.cameraPosition.y;
    baseConstants.cameraPosition[2] = view.cameraPosition.z;
    baseConstants.cameraForward[0] = view.cameraForward.x;
    baseConstants.cameraForward[1] = view.cameraForward.y;
    baseConstants.cameraForward[2] = view.cameraForward.z;
    baseConstants.probeControl[0] = 0.0f;
    baseConstants.probeControl[1] = static_cast<float>(readyProbeCount);
    baseConstants.probeControl[2] =
        screenSpaceTraceScheduled ? 1.0f : 0.0f;
    baseConstants.probeControl[3] =
        static_cast<float>(localProbeMipLevels);
    const auto setVector = [](float (&target)[4], const Vec3& value, float w)
    {
        target[0] = value.x;
        target[1] = value.y;
        target[2] = value.z;
        target[3] = w;
    };
    for (uint32 probeIndex = 0;
         probeIndex < RVX_SSR_LOCAL_PROBE_CAPACITY;
         ++probeIndex)
    {
        const uint32 weightVector = probeIndex / 4U;
        const uint32 weightElement = probeIndex % 4U;
        baseConstants.probeWeights[weightVector][weightElement] =
            inputs.probeWeights[probeIndex];
        baseConstants.probePriorities[weightVector][weightElement] =
            inputs.probePriorities[probeIndex];
        setVector(baseConstants.probePositions[probeIndex],
                  inputs.probePositions[probeIndex], 1.0f);
        setVector(baseConstants.probeExtents[probeIndex],
                  inputs.probeExtents[probeIndex], 0.0f);
        baseConstants.probeWorldTransforms[probeIndex] =
            inputs.probeWorldTransforms[probeIndex];
        baseConstants.probeInverseWorldTransforms[probeIndex] =
            inputs.probeInverseWorldTransforms[probeIndex];
        setVector(baseConstants.probeBoxProjectionSizes[probeIndex],
                  inputs.probeBoxProjectionSizes[probeIndex], 0.0f);
        setVector(baseConstants.probeBoxProjectionOffsets[probeIndex],
                  inputs.probeBoxProjectionOffsets[probeIndex], 0.0f);
        baseConstants.probeInfluenceModes[probeIndex][0] =
            inputs.probeNormalizedBlendDistances[probeIndex];
        baseConstants.probeInfluenceModes[probeIndex][1] =
            inputs.probeShapes[probeIndex] == SSRProbeInfluenceShape::Sphere
                ? 1.0f : 0.0f;
        baseConstants.probeInfluenceModes[probeIndex][2] =
            inputs.probeUsesBoxProjection[probeIndex] ? 1.0f : 0.0f;
        if (inputs.probeReady[probeIndex])
        {
            baseConstants.traceControl[3] |=
                RVX_SSR_FLAG_FIRST_PROBE_READY << probeIndex;
        }
    }
    baseConstants.inverseViewProjection =
        view.inverseJitteredViewProjectionMatrix;
    baseConstants.viewProjection = view.jitteredViewProjectionMatrix;

    auto executionState = std::make_shared<SSRExecutionState>();
    executionState->expectedPassCount =
        (screenSpaceTraceScheduled ? mipCount + 2U : 2U) +
        (qualificationRequested ? 1U : 0U);
    executionState->qualificationCopyExpected = qualificationRequested;
    m_pendingExecutionState = executionState;
    IRHIDevice* const device = m_pipelineCache->GetDevice();
    const RHIPipelineLayoutRef pipelineLayout(m_pipelineCache->GetSSRLayout());
    const RHIDescriptorSetLayoutRef setLayout(
        m_pipelineCache->GetSSRSetLayout());
    const RHISamplerRef sampler = m_sampler;

    const auto addStage = [this, &graph, inputs, normalRoughness, hiZ,
                           rawReflection, confidence, probes,
                           resolvedSceneColor, routeMask,
                           baseConstants, executionState, device,
                           pipelineLayout, setLayout, sampler](
                              const char* passName,
                              SSRStage stage,
                              uint32 sourceMip,
                              RGTextureHandle stageSource,
                              RGTextureHandle output0,
                              RGTextureHandle output1)
    {
        SSRGPUConstants constants = baseConstants;
        if (stage == SSRStage::HiZReduce)
        {
            const uint32 sourceWidth =
                std::max(1U, static_cast<uint32>(constants.renderSize[0]) >> sourceMip);
            const uint32 sourceHeight =
                std::max(1U, static_cast<uint32>(constants.renderSize[1]) >> sourceMip);
            constants.sourceSize[0] = static_cast<float>(sourceWidth);
            constants.sourceSize[1] = static_cast<float>(sourceHeight);
            constants.sourceSize[2] = 1.0f / constants.sourceSize[0];
            constants.sourceSize[3] = 1.0f / constants.sourceSize[1];
        }
        const RHIPipelineRef pipeline(
            ResolvePipeline(*m_pipelineCache, stage));
        graph.AddPass<SSRPassData>(
            passName,
            RenderGraphPassType::Graphics,
            [inputs, normalRoughness, hiZ, rawReflection, confidence,
             probes, resolvedSceneColor, routeMask,
             constants, executionState, device, pipeline, pipelineLayout,
             setLayout, sampler, stage, sourceMip, stageSource, output0,
             output1](RenderGraphBuilder& builder, SSRPassData& data)
            {
                data.stage = stage;
                data.constants = constants;
                data.executionState = executionState;
                data.device = device;
                data.pipeline = pipeline;
                data.pipelineLayout = pipelineLayout;
                data.setLayout = setLayout;
                data.sampler = sampler;

                data.sceneColor = inputs.sceneColor;
                data.stageSource = stageSource;
                data.normalRoughness = normalRoughness;
                data.hiZ = stage == SSRStage::Trace ? hiZ : stageSource;
                data.rawReflection = stage == SSRStage::Resolve
                    ? rawReflection : inputs.sceneColor;
                data.confidence = stage == SSRStage::Resolve
                    ? confidence : inputs.deviceDepth;
                data.probes = probes;
                data.baseIndirectSpecular = inputs.baseIndirectSpecular;
                data.specularResponse = inputs.specularResponse;
                data.exposureState = builder.Read(
                    inputs.exposureState,
                    MakeRGAccessDesc(RHIResourceState::ShaderResource,
                                     RHIShaderStage::Pixel));
                data.exposureStateContentHash =
                    inputs.exposureStateContentHash;
                data.output0 = output0;
                data.output1 = output1;

                const RHISubresourceRange sourceRange =
                    stage == SSRStage::HiZReduce
                        ? RHISubresourceRange::Mip(sourceMip)
                        : RHISubresourceRange::All();
                data.sceneColorView = AddTextureSRV(
                    builder, data.sceneColor, "SSRSceneColorSRV");
                data.stageSourceView = AddTextureSRV(
                    builder, data.stageSource, "SSRStageSourceSRV", sourceRange);
                data.normalRoughnessView = AddTextureSRV(
                    builder, data.normalRoughness, "SSRNormalRoughnessSRV");
                data.hiZView = AddTextureSRV(
                    builder, data.hiZ, "SSRHiZSRV",
                    stage == SSRStage::Trace
                        ? RHISubresourceRange::All() : sourceRange);
                data.rawReflectionView = AddTextureSRV(
                    builder, data.rawReflection, "SSRRawReflectionSRV");
                data.confidenceView = AddTextureSRV(
                    builder, data.confidence, "SSRConfidenceSRV");
                for (uint32 probeIndex = 0;
                     probeIndex < RVX_SSR_LOCAL_PROBE_CAPACITY;
                     ++probeIndex)
                {
                    data.probeViews[probeIndex] = AddTextureSRV(
                        builder, data.probes[probeIndex],
                        "SSRLocalProbeSRV");
                }
                data.baseIndirectSpecularView = AddTextureSRV(
                    builder, data.baseIndirectSpecular,
                    "SSRBaseIndirectSpecularSRV");
                data.specularResponseView = AddTextureSRV(
                    builder, data.specularResponse,
                    "SSRSpecularResponseSRV");

                RHISubresourceRange outputRange = RHISubresourceRange::All();
                if (stage == SSRStage::HiZBase)
                {
                    outputRange = RHISubresourceRange::Mip(0);
                }
                else if (stage == SSRStage::HiZReduce)
                {
                    outputRange = RHISubresourceRange::Mip(sourceMip + 1U);
                }
                data.output0View = AddTextureRTV(
                    builder, data.output0, "SSROutput0RTV", outputRange);
                if (data.output1.IsValid())
                {
                    data.output1View = AddTextureRTV(
                        builder, data.output1, "SSROutput1RTV");
                }
                data.resourcesRetained = data.device != nullptr && data.pipeline &&
                    data.pipelineLayout && data.setLayout && data.sampler &&
                    builder.RetainSubmissionResource(
                        Ref<RefCounted>(data.pipeline)) &&
                    builder.RetainSubmissionResource(
                        Ref<RefCounted>(data.pipelineLayout)) &&
                    builder.RetainSubmissionResource(
                        Ref<RefCounted>(data.setLayout)) &&
                    builder.RetainSubmissionResource(
                        Ref<RefCounted>(data.sampler));
                if (stage == SSRStage::Resolve)
                {
                    builder.SetExportState(
                        resolvedSceneColor, RHIResourceState::ShaderResource);
                    builder.SetExportState(routeMask, RHIResourceState::ShaderResource);
                }
                else if (stage == SSRStage::Trace)
                {
                    builder.SetExportState(
                        rawReflection, RHIResourceState::ShaderResource);
                    builder.SetExportState(confidence, RHIResourceState::ShaderResource);
                }
            },
            [](const SSRPassData& data, RenderGraphPassContext& context)
            {
                if (data.executionState == nullptr ||
                    data.executionState->failed || !data.resourcesRetained ||
                    data.device == nullptr || !data.pipeline ||
                    !data.pipelineLayout || !data.setLayout || !data.sampler)
                {
                    if (data.executionState)
                    {
                        data.executionState->failed = true;
                    }
                    context.MarkExecutionFailed(
                        "SSR immutable recording resources are unavailable");
                    return;
                }
                RHITexture* const output0 = context.GetTexture(data.output0);
                RHITextureView* const output0View =
                    context.GetTextureView(data.output0View);
                RHITextureView* const output1View = data.output1.IsValid()
                    ? context.GetTextureView(data.output1View) : nullptr;
                RHITextureView* const sceneColorView =
                    context.GetTextureView(data.sceneColorView);
                RHITextureView* const stageSourceView =
                    context.GetTextureView(data.stageSourceView);
                RHITextureView* const normalView =
                    context.GetTextureView(data.normalRoughnessView);
                RHITextureView* const hiZView =
                    context.GetTextureView(data.hiZView);
                RHITextureView* const rawView =
                    context.GetTextureView(data.rawReflectionView);
                RHITextureView* const confidenceView =
                    context.GetTextureView(data.confidenceView);
                std::array<RHITextureView*, RVX_SSR_LOCAL_PROBE_CAPACITY>
                    probeViews{};
                bool probesRealized = true;
                for (uint32 probeIndex = 0;
                     probeIndex < RVX_SSR_LOCAL_PROBE_CAPACITY;
                     ++probeIndex)
                {
                    probeViews[probeIndex] =
                        context.GetTextureView(data.probeViews[probeIndex]);
                    probesRealized =
                        probesRealized && probeViews[probeIndex] != nullptr;
                }
                RHITextureView* const baseIndirectSpecularView =
                    context.GetTextureView(data.baseIndirectSpecularView);
                RHITextureView* const specularResponseView =
                    context.GetTextureView(data.specularResponseView);
                RHIBuffer* const exposureState =
                    context.GetBuffer(data.exposureState);
                if (output0 == nullptr || output0View == nullptr ||
                    (data.output1.IsValid() && output1View == nullptr) ||
                    sceneColorView == nullptr || stageSourceView == nullptr ||
                    normalView == nullptr || hiZView == nullptr || rawView == nullptr ||
                    confidenceView == nullptr || !probesRealized ||
                    baseIndirectSpecularView == nullptr ||
                    specularResponseView == nullptr || exposureState == nullptr ||
                    data.exposureStateContentHash == 0)
                {
                    data.executionState->failed = true;
                    context.MarkExecutionFailed(
                        "SSR graph texture views could not be realized");
                    return;
                }

                RHIBufferRef constants = CreateImmutableConstants(
                    *data.device, data.constants,
                    GetPassConstantsName(data.stage));
                if (!constants || !context.RetainSubmissionResource(
                        Ref<RefCounted>(constants)))
                {
                    data.executionState->failed = true;
                    context.MarkExecutionFailed(
                        "SSR immutable constants could not be retained");
                    return;
                }

                RHIDescriptorSetDesc descriptorDesc;
                descriptorDesc.layout = data.setLayout.Get();
                descriptorDesc.debugName = GetDescriptorName(data.stage);
                descriptorDesc.BindBuffer(
                    0, constants.Get(), 0,
                    AlignConstantBufferSize(sizeof(SSRGPUConstants)));
                descriptorDesc.BindTexture(1, sceneColorView);
                descriptorDesc.BindTexture(2, stageSourceView);
                descriptorDesc.BindTexture(3, normalView);
                descriptorDesc.BindTexture(4, hiZView);
                descriptorDesc.BindTexture(5, rawView);
                descriptorDesc.BindTexture(6, confidenceView);
                for (uint32 probeIndex = 0;
                     probeIndex < RVX_SSR_LOCAL_PROBE_CAPACITY;
                     ++probeIndex)
                {
                    descriptorDesc.BindTexture(
                        7, probeViews[probeIndex], probeIndex);
                }
                descriptorDesc.BindTexture(15, baseIndirectSpecularView);
                descriptorDesc.BindBuffer(16, exposureState);
                descriptorDesc.BindTexture(17, specularResponseView);
                descriptorDesc.BindSampler(19, data.sampler.Get());
                RHIDescriptorSetRef descriptorSet =
                    data.device->CreateDescriptorSet(descriptorDesc);
                if (!descriptorSet ||
                    !descriptorSet->IsReadyForBinding(data.setLayout.Get()) ||
                    !context.RetainSubmissionResource(
                        Ref<RefCounted>(descriptorSet)))
                {
                    data.executionState->failed = true;
                    context.MarkExecutionFailed(
                        "SSR descriptor set creation failed");
                    return;
                }

                RHIRenderPassDesc passDesc;
                passDesc.AddColorAttachment(
                    output0View, RHILoadOp::DontCare, RHIStoreOp::Store);
                if (output1View != nullptr)
                {
                    passDesc.AddColorAttachment(
                        output1View, RHILoadOp::DontCare, RHIStoreOp::Store);
                }
                const uint32 outputWidth =
                    data.stage == SSRStage::HiZReduce
                        ? std::max(1U, static_cast<uint32>(
                              data.constants.sourceSize[0]) / 2U)
                        : output0->GetWidth();
                const uint32 outputHeight =
                    data.stage == SSRStage::HiZReduce
                        ? std::max(1U, static_cast<uint32>(
                              data.constants.sourceSize[1]) / 2U)
                        : output0->GetHeight();
                passDesc.SetRenderArea(0, 0, outputWidth, outputHeight);
                RHICommandContext& commands = context.Commands();
                commands.BeginRenderPass(passDesc);
                commands.SetPipeline(data.pipeline.Get());
                commands.SetDescriptorSet(0, descriptorSet.Get());
                RHIViewport viewport;
                viewport.width = static_cast<float>(outputWidth);
                viewport.height = static_cast<float>(outputHeight);
                viewport.minDepth = 0.0f;
                viewport.maxDepth = 1.0f;
                commands.SetViewport(viewport);
                RHIRect scissor;
                scissor.width = outputWidth;
                scissor.height = outputHeight;
                commands.SetScissor(scissor);
                commands.Draw(3, 1, 0, 0);
                commands.EndRenderPass();

                ++data.executionState->completedPassCount;
                if (data.stage == SSRStage::HiZBase ||
                    data.stage == SSRStage::HiZReduce)
                {
                    ++data.executionState->completedHiZPassCount;
                }
                else if (data.stage == SSRStage::Trace)
                {
                    data.executionState->traceCompleted = true;
                }
                else if (data.stage == SSRStage::Resolve)
                {
                    data.executionState->resolveCompleted = true;
                }
            });
    };

    if (screenSpaceTraceScheduled)
    {
        addStage("SSRHiZBase", SSRStage::HiZBase, 0U,
                 inputs.deviceDepth, hiZ, {});
        for (uint32 mip = 1U; mip < mipCount; ++mip)
        {
            addStage("SSRHiZReduce", SSRStage::HiZReduce, mip - 1U,
                     hiZ, hiZ, {});
        }
        addStage("SSRHiZTrace", SSRStage::Trace, 0U,
                 inputs.deviceDepth, rawReflection, confidence);
    }
    else
    {
        graph.AddPass<SSRNeutralInputsPassData>(
            "SSRNeutralReflectionInputs",
            RenderGraphPassType::Graphics,
            [rawReflection, confidence, executionState](
                RenderGraphBuilder& builder,
                SSRNeutralInputsPassData& data)
            {
                data.rawReflection = rawReflection;
                data.confidence = confidence;
                data.executionState = executionState;
                data.rawReflectionView = AddTextureRTV(
                    builder, rawReflection, "SSRNeutralReflectionRTV");
                data.confidenceView = AddTextureRTV(
                    builder, confidence, "SSRNeutralConfidenceRTV");
                builder.SetExportState(
                    rawReflection, RHIResourceState::ShaderResource);
                builder.SetExportState(
                    confidence, RHIResourceState::ShaderResource);
            },
            [](const SSRNeutralInputsPassData& data,
               RenderGraphPassContext& context)
            {
                RHITexture* const reflection =
                    context.GetTexture(data.rawReflection);
                RHITextureView* const reflectionView =
                    context.GetTextureView(data.rawReflectionView);
                RHITextureView* const confidenceView =
                    context.GetTextureView(data.confidenceView);
                if (data.executionState == nullptr ||
                    reflection == nullptr || reflectionView == nullptr ||
                    confidenceView == nullptr)
                {
                    if (data.executionState)
                    {
                        data.executionState->failed = true;
                    }
                    context.MarkExecutionFailed(
                        "Reflection fallback neutral inputs could not be realized");
                    return;
                }
                RHIRenderPassDesc passDesc;
                passDesc.AddColorAttachment(
                    reflectionView, RHILoadOp::Clear, RHIStoreOp::Store,
                    {0.0f, 0.0f, 0.0f, 0.0f});
                passDesc.AddColorAttachment(
                    confidenceView, RHILoadOp::Clear, RHIStoreOp::Store,
                    {0.0f, 0.0f, 0.0f, 0.0f});
                passDesc.SetRenderArea(
                    0, 0, reflection->GetWidth(), reflection->GetHeight());
                RHICommandContext& commands = context.Commands();
                commands.BeginRenderPass(passDesc);
                commands.EndRenderPass();
                ++data.executionState->completedPassCount;
                data.executionState->neutralInputsCompleted = true;
            });
    }
    addStage("SSRProbeIBLResolve", SSRStage::Resolve, 0U,
             inputs.deviceDepth, resolvedSceneColor, routeMask);

    if (qualificationRequested)
    {
        graph.AddPass<SSRQualificationCopyData>(
            "SSRQualificationRouteMaskCopy",
            RenderGraphPassType::Copy,
            RenderGraphQueueAffinity::Graphics,
            [routeMask, qualificationReadback,
             readbackOwner = RHIBufferRef(m_probeReadback),
             qualificationIdentity, qualificationGeneration,
             rowPitch = m_probeRowPitch, executionState](
                RenderGraphBuilder& builder,
                SSRQualificationCopyData& data)
            {
                data.routeMask = builder.Read(
                    routeMask,
                    MakeRGAccessDesc(RHIResourceState::CopySource,
                                     RHIShaderStage::All));
                data.readback = builder.Write(
                    qualificationReadback,
                    MakeRGAccessDesc(RHIResourceState::CopyDest,
                                     RHIShaderStage::All));
                data.readbackOwner = readbackOwner;
                data.identity = qualificationIdentity;
                data.generation = qualificationGeneration;
                data.width = qualificationIdentity.extent.width;
                data.height = qualificationIdentity.extent.height;
                data.rowPitch = rowPitch;
                data.executionState = executionState;
            },
            [this](const SSRQualificationCopyData& data,
                   RenderGraphPassContext& context)
            {
                RHITexture* const routeMaskTexture =
                    context.GetTexture(data.routeMask);
                RHIBuffer* const readback = context.GetBuffer(data.readback);
                if (data.executionState == nullptr ||
                    !data.executionState->traceCompleted ||
                    !data.executionState->resolveCompleted ||
                    routeMaskTexture == nullptr || readback == nullptr ||
                    !data.readbackOwner ||
                    readback != data.readbackOwner.Get() ||
                    !context.RetainSubmissionResource(
                        Ref<RefCounted>(data.readbackOwner)))
                {
                    if (data.executionState != nullptr)
                    {
                        data.executionState->failed = true;
                    }
                    context.MarkExecutionFailed(
                        "SSR qualification route readback resources or prerequisite draws are unavailable");
                    return;
                }

                RHIBufferTextureCopyDesc copyDesc;
                copyDesc.bufferRowPitch = data.rowPitch;
                copyDesc.bufferImageHeight = data.height;
                copyDesc.textureRegion =
                    {0, 0, data.width, data.height};
                context.Commands().CopyTextureToBuffer(
                    routeMaskTexture, readback, copyDesc);
                ++data.executionState->completedPassCount;
                data.executionState->qualificationCopyCompleted = true;

                if (m_probeRequested && m_probeIdentityLocked &&
                    m_probeGeneration == data.generation &&
                    m_probeIdentity == data.identity)
                {
                    m_probeTraceDrawRecorded = true;
                    m_probeResolveDrawRecorded = true;
                    m_probeCopyRecorded = true;
                    m_lastProbeStats.traceDrawRecorded = true;
                    m_lastProbeStats.resolveDrawRecorded = true;
                    m_lastProbeStats.copyRecorded = true;
                }
            });
    }

    m_lastComputeStats.scheduled = true;
    m_lastComputeStats.hiZMipCount = mipCount;
    m_lastComputeStats.hiZGraphPassCount =
        screenSpaceTraceScheduled ? mipCount : 0U;
    m_lastComputeStats.traceGraphPassCount =
        screenSpaceTraceScheduled ? 1U : 0U;
    m_lastComputeStats.resolveGraphPassCount = 1U;
    m_lastComputeStats.screenSpaceTraceScheduled =
        screenSpaceTraceScheduled;
    m_lastComputeStats.minimumFallbackRoute =
        readyProbeCount != 0
            ? ReflectionResolveRoute::LocalProbe
            : ReflectionResolveRoute::GlobalIBL;

    SSROutput output;
    output.hiZPyramid = hiZ;
    output.rawReflection = rawReflection;
    output.confidence = confidence;
    output.resolvedSceneColor = resolvedSceneColor;
    output.routeMask = routeMask;
    output.hiZMipCount = mipCount;
    output.reflectionPlanIdentity = inputs.reflectionPlanIdentity;
    output.lightingPlanIdentity = inputs.lightingPlanIdentity;
    output.configurationIdentity = inputs.configurationIdentity;
    output.pipelineIdentity = inputs.pipelineIdentity;
    output.depthPyramidIdentity = inputs.depthPyramidIdentity;
    output.screenSpaceScheduled = screenSpaceTraceScheduled;
    output.probeResolveScheduled = true;
    return output;
}

void SSR::FinalizeGraphExecution(bool executionAccepted) noexcept
{
    const bool screenSpaceTraceScheduled =
        m_lastComputeStats.screenSpaceTraceScheduled;
    const bool complete = executionAccepted && m_pendingExecutionState != nullptr &&
        !m_pendingExecutionState->failed &&
        m_pendingExecutionState->expectedPassCount != 0 &&
        m_pendingExecutionState->completedPassCount ==
            m_pendingExecutionState->expectedPassCount &&
        (screenSpaceTraceScheduled
             ? (m_pendingExecutionState->completedHiZPassCount ==
                    m_lastComputeStats.hiZMipCount &&
                m_pendingExecutionState->traceCompleted)
             : m_pendingExecutionState->neutralInputsCompleted) &&
        m_pendingExecutionState->resolveCompleted &&
        (!m_pendingExecutionState->qualificationCopyExpected ||
         m_pendingExecutionState->qualificationCopyCompleted);
    m_lastComputeStats.executed = complete;
    m_lastComputeStats.hiZBuilt = complete && screenSpaceTraceScheduled;
    m_lastComputeStats.fallbackChainResolved = complete;
    // Graph callback completion proves that the work was recorded and
    // accepted.  Applied-route claims require the physical R8 route-mask
    // readback observed after the exact Graphics completion and present.
    m_lastComputeStats.screenSpaceApplied = false;
    m_lastComputeStats.probesApplied = false;
    if (!complete && m_lastComputeStats.scheduled &&
        m_lastComputeStats.fallbackReason.empty())
    {
        m_lastComputeStats.fallbackReason =
            "SSR graph execution evidence was incomplete or rejected";
    }
    m_pendingExecutionState.reset();
}

bool SSR::ArmQualificationProbe() noexcept
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

bool SSR::NotifyQualificationProbeSubmission(
    const SSRQualificationProbeIdentity& identity,
    const GPUCompletionToken& completion,
    const RenderSubmissionTracker& tracker) noexcept
{
    if (!m_probeRequested || !m_probeIdentityLocked ||
        !m_probeTraceDrawRecorded || !m_probeResolveDrawRecorded ||
        !m_probeCopyRecorded || !m_probeReadback ||
        m_probeSubmissionAccepted || !identity.IsValid() ||
        identity != m_probeIdentity)
    {
        m_lastProbeStats.failure =
            "SSR qualification submission does not match a fully recorded route probe";
        return false;
    }

    GPUCompletionPoint graphicsPoint;
    if (!IsExactCurrentGraphicsSubmissionToken(
            tracker, completion, graphicsPoint))
    {
        m_lastProbeStats.failure =
            "SSR qualification submission token is not the exact Graphics completion";
        return false;
    }

    m_probeCompletionValue = graphicsPoint.value;
    m_probeSubmissionAccepted = true;
    m_lastProbeStats.submissionAccepted = true;
    return true;
}

bool SSR::MarkQualificationProbePresented(
    const SSRQualificationProbeIdentity& identity) noexcept
{
    if (!m_probeRequested || !m_probeSubmissionAccepted ||
        !m_probeIdentityLocked || !identity.IsValid() ||
        identity != m_probeIdentity)
    {
        m_lastProbeStats.failure =
            "SSR qualification present identity does not match the submitted route probe";
        return false;
    }

    m_probePresented = true;
    m_lastProbeStats.presented = true;
    return true;
}

bool SSR::PollQualificationProbe(
    const RenderSubmissionTracker& tracker,
    SSRQualificationProbeStats& outStats) noexcept
{
    outStats = m_lastProbeStats;
    if (!m_probeRequested || !m_probeSubmissionAccepted ||
        (!m_probePresented && !m_probeAbandoned) || !m_probeReadback ||
        !m_probeIdentity.IsValid() || m_probeCompletionValue == 0)
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
            "SSR qualification Graphics completion was lost";
        outStats = m_lastProbeStats;
        ClearQualificationProbeState(true);
        return false;
    }
    m_lastProbeStats.gpuCompleted = true;

    if (m_probeAbandoned)
    {
        m_lastProbeStats.failure =
            "SSR qualification route probe was abandoned after submission";
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
            "SSR qualification route readback layout is invalid";
        outStats = m_lastProbeStats;
        ClearQualificationProbeState(true);
        return false;
    }

    void* const mapped = m_probeReadback->Map();
    if (mapped == nullptr)
    {
        m_lastProbeStats.failure =
            "SSR qualification route readback map failed";
        outStats = m_lastProbeStats;
        ClearQualificationProbeState(true);
        return false;
    }

    const auto* const samples = static_cast<const uint8*>(mapped);
    uint64 backgroundPixels = 0;
    uint64 nonReflectivePixels = 0;
    uint64 screenSpaceHits = 0;
    uint64 localProbePixels = 0;
    uint64 globalIBLPixels = 0;
    bool invalidClassification = false;
    for (uint32 y = 0; y < m_probeHeight; ++y)
    {
        const uint8* const row = samples + static_cast<uint64>(y) *
            static_cast<uint64>(m_probeRowPitch);
        for (uint32 x = 0; x < m_probeWidth; ++x)
        {
            switch (row[x])
            {
                case 0: ++backgroundPixels; break;
                case 17: ++nonReflectivePixels; break;
                case 85: ++screenSpaceHits; break;
                case 170: ++localProbePixels; break;
                case 255: ++globalIBLPixels; break;
                default: invalidClassification = true; break;
            }
        }
    }
    m_probeReadback->Unmap();

    if (invalidClassification)
    {
        m_lastProbeStats.failure =
            "SSR qualification route mask contains an invalid classification";
        outStats = m_lastProbeStats;
        ClearQualificationProbeState(true);
        return false;
    }

    m_lastProbeStats.sampleCount = sampleCount;
    m_lastProbeStats.backgroundPixelCount = backgroundPixels;
    m_lastProbeStats.nonReflectivePixelCount = nonReflectivePixels;
    m_lastProbeStats.receiverSampleCount =
        sampleCount - backgroundPixels - nonReflectivePixels;
    m_lastProbeStats.screenSpaceHitPixelCount = screenSpaceHits;
    m_lastProbeStats.localProbePixelCount = localProbePixels;
    m_lastProbeStats.globalIBLPixelCount = globalIBLPixels;
    // The decoder accepts the 170/255 classes only from an armed graph that
    // set the shader's qualification-radiance bit. The schema and booleans
    // bind that byte protocol to the completion-owned CPU evidence without
    // requiring every ordinary SSR scene to contain both fallback routes.
    m_lastProbeStats.routeMaskRadianceSchemaVersion =
        RVX_SSR_QUALIFICATION_ROUTE_MASK_RADIANCE_SCHEMA_VERSION;
    m_lastProbeStats.localProbeRadianceQualified =
        localProbePixels != 0;
    m_lastProbeStats.globalIBLRadianceQualified =
        globalIBLPixels != 0;
    if (m_lastProbeStats.receiverSampleCount == 0)
    {
        m_lastProbeStats.failure =
            "SSR qualification route mask contains no reflection receivers";
        outStats = m_lastProbeStats;
        ClearQualificationProbeState(true);
        return false;
    }
    m_lastProbeStats.ready = true;
    m_lastProbeStats.failure.clear();
    outStats = m_lastProbeStats;
    if (!outStats.IsValid())
    {
        m_lastProbeStats.ready = false;
        m_lastProbeStats.failure =
            "SSR qualification route readback produced incomplete statistics";
        outStats = m_lastProbeStats;
        ClearQualificationProbeState(true);
        return false;
    }

    // Do not write this asynchronous result into m_lastComputeStats: later
    // frames may already have replaced that snapshot.  The caller publishes
    // the returned, identity-keyed probe result with the frozen frame receipt.
    ClearQualificationProbeState(true);
    return true;
}

bool SSR::HasPendingQualificationProbe() const noexcept
{
    return m_probeRequested || m_probeIdentityLocked ||
           m_probeTraceDrawRecorded || m_probeResolveDrawRecorded ||
           m_probeCopyRecorded || m_probeSubmissionAccepted;
}

void SSR::AbandonQualificationProbe() noexcept
{
    if (!HasPendingQualificationProbe())
    {
        return;
    }
    if (m_probeSubmissionAccepted)
    {
        // The Graphics queue may still write the imported readback buffer.
        // Retain it until PollQualificationProbe observes the exact point.
        m_probeAbandoned = true;
        return;
    }

    m_lastProbeStats.failure =
        "SSR qualification route probe was abandoned before submission";
    ClearQualificationProbeState(true);
}

std::optional<SSRQualificationProbeIdentity>
SSR::GetPendingQualificationProbeIdentity() const noexcept
{
    if (!m_probeRequested || !m_probeIdentityLocked ||
        !m_probeTraceDrawRecorded || !m_probeResolveDrawRecorded ||
        !m_probeCopyRecorded || m_probeAbandoned ||
        !m_probeIdentity.IsValid())
    {
        return std::nullopt;
    }
    return m_probeIdentity;
}

bool SSR::EnsureQualificationReadback(uint32 width, uint32 height) noexcept
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
    if (m_probeReadback && m_probeWidth == width &&
        m_probeHeight == height && m_probeRowPitch == rowPitch &&
        m_probeReadback->GetSize() == size)
    {
        return true;
    }

    RHIBufferDesc desc;
    desc.size = size;
    desc.usage = RHIBufferUsage::CopyDst;
    desc.memoryType = RHIMemoryType::Readback;
    desc.debugName = "SSRQualificationRouteMaskReadback";
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

void SSR::ClearQualificationProbeState(bool retainReadback) noexcept
{
    m_probeIdentity = {};
    m_probeCompletionValue = 0;
    m_probeRequested = false;
    m_probeIdentityLocked = false;
    m_probeTraceDrawRecorded = false;
    m_probeResolveDrawRecorded = false;
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
} // namespace RVX
