#include "Render/Passes/RayTracedShadowPass.h"

#include "RHI/RHIRayTracing.h"
#include "Render/Graph/ResourceViewCache.h"
#include "Render/PipelineCache.h"
#include "Render/RayTracing/RayTracingResourceBindings.h"
#include "Render/RayTracing/RayTracingSceneManager.h"
#include "Render/Renderer/ViewData.h"
#include "Resources/RenderOwnerSnapshotRetirement.h"
#include "Resources/RenderResourceRegistry.h"
#include "Resources/RenderSubmissionTracker.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace RVX
{
    namespace
    {
        namespace RTShadowBindings = RayTracingResourceBindings::Shadow;

        constexpr uint64 RVX_RAY_TRACED_SHADOW_CONSTANT_BUFFER_ALIGNMENT = 256;
        constexpr uint32 RVX_RAY_TRACED_SHADOW_TIMING_QUERY_COUNT_PER_FRAME = 2;
        constexpr uint32 RVX_RAY_TRACED_SHADOW_TIMING_START_QUERY_OFFSET = 0;
        constexpr uint32 RVX_RAY_TRACED_SHADOW_TIMING_END_QUERY_OFFSET = 1;
        constexpr uint64 RVX_RAY_TRACED_SHADOW_TIMING_READBACK_BYTES = sizeof(uint64) * 2;
        constexpr uint32 RVX_RAY_TRACED_SHADOW_HISTORY_SLOT_COUNT = RVX_MAX_FRAME_COUNT + 1;

        RenderResourceHandle UnpackRenderResourceHandle(uint64 value)
        {
            return RenderResourceHandle{static_cast<uint32>(value >> 32U),
                                        static_cast<uint32>(value)};
        }

        uint64 AlignRayTracedShadowConstantBufferSize(uint64 size)
        {
            return (size + RVX_RAY_TRACED_SHADOW_CONSTANT_BUFFER_ALIGNMENT - 1) &
                   ~(RVX_RAY_TRACED_SHADOW_CONSTANT_BUFFER_ALIGNMENT - 1);
        }

        Vec3 NormalizeOr(const Vec3& value, const Vec3& fallback)
        {
            const float32 lengthSquared = value.x * value.x + value.y * value.y + value.z * value.z;
            if (!std::isfinite(lengthSquared) || lengthSquared <= 1.0e-8f)
            {
                return fallback;
            }
            return value / std::sqrt(lengthSquared);
        }

        bool NearlyEqual(const Vec3& lhs, const Vec3& rhs, float32 epsilon)
        {
            return std::abs(lhs.x - rhs.x) <= epsilon &&
                   std::abs(lhs.y - rhs.y) <= epsilon &&
                   std::abs(lhs.z - rhs.z) <= epsilon;
        }

        float32 ClampFiniteNonNegative(float32 value, float32 fallback)
        {
            return std::isfinite(value) ? std::max(0.0f, value) : fallback;
        }

        bool ConfigFloatChanged(float32 lhs, float32 rhs)
        {
            constexpr float32 epsilon = 1.0e-5f;
            return std::abs(lhs - rhs) > epsilon;
        }

        bool ShadowHistoryConfigChanged(const ShadowPassConfig& lhs, const ShadowPassConfig& rhs)
        {
            return ConfigFloatChanged(lhs.normalBias, rhs.normalBias) ||
                   ConfigFloatChanged(lhs.rayTracedLightAngularRadius, rhs.rayTracedLightAngularRadius) ||
                   ConfigFloatChanged(lhs.rayTracedTemporalBlendFactor, rhs.rayTracedTemporalBlendFactor) ||
                   ConfigFloatChanged(lhs.rayTracedHistoryDepthThreshold, rhs.rayTracedHistoryDepthThreshold) ||
                   ConfigFloatChanged(lhs.rayTracedHistoryNormalThreshold, rhs.rayTracedHistoryNormalThreshold) ||
                   ConfigFloatChanged(lhs.rayTracedHistoryVelocityRejectionScale, rhs.rayTracedHistoryVelocityRejectionScale) ||
                   lhs.rayTracedTemporalAccumulation != rhs.rayTracedTemporalAccumulation ||
                   lhs.rayTracedSamplesPerPixel != rhs.rayTracedSamplesPerPixel ||
                   lhs.rayTracedInstanceMask != rhs.rayTracedInstanceMask;
        }

        bool IsNewerRecording(const RenderPassRecordIdentity& candidate,
                              const RenderPassRecordIdentity& previous)
        {
            if (!previous.IsValid())
            {
                return true;
            }
            if (candidate.frameSequence != previous.frameSequence)
            {
                return candidate.frameSequence > previous.frameSequence;
            }
            if (candidate.viewOrdinal != previous.viewOrdinal)
            {
                return candidate.viewOrdinal > previous.viewOrdinal;
            }
            return candidate.recordEpoch > previous.recordEpoch;
        }

        struct RayTracedShadowGPUConstants
        {
            Mat4 inverseViewProjection = Mat4Identity();
            Mat4 previousViewProjection = Mat4Identity();
            Vec4 lightDirectionAndTMax{};
            Vec4 viewportSizeAndInvSize{};
            Vec4 depthAndBiasParams{};
            Vec4 historyReprojectionParams{};
            Vec4 softShadowParams{};
            Vec4 rayOptions{};
        };
    } // namespace

    struct RayTracedShadowHistoryReservation
    {
        RenderPassRecordIdentity identity{};
        uint64 allocationGeneration = 0;
        uint32 readSlot = RVX_INVALID_INDEX;
        uint32 writeSlot = RVX_INVALID_INDEX;
        bool historyAvailable = false;
        bool historyViewValid = false;
        Mat4 previousViewProjection = Mat4Identity();
        Vec3 previousRayDirection{0.0f, 1.0f, 0.0f};
        ShadowPassConfig config{};
        Mat4 currentViewProjection = Mat4Identity();
        Vec3 currentRayDirection{0.0f, 1.0f, 0.0f};
        RayTracedShadowPassStats recordedStats{};
        RHIQueryPoolRef timingQueryPool;
        RHIBufferRef timingReadbackBuffer;
        uint64 timingTimestampFrequency = 0;
        // These are the graph-owned exports for the physical history textures.
        // Submission projects their realized access snapshots back to the owner.
        RGTextureHandle historyReadHandle{};
        RGTextureHandle historyDepthReadHandle{};
        RGTextureHandle historyNormalReadHandle{};
        RGTextureHandle shadowMaskHandle{};
        RGTextureHandle historyDepthWriteHandle{};
        RGTextureHandle historyNormalWriteHandle{};
        bool graphPassExecuted = false;
        bool recorded = false;
        bool timingResolveRecorded = false;
        bool cancelled = false;
    };

    /** @brief Final diagnostic payload for one identity-addressable recording. */
    struct RayTracedShadowSubmissionRecord
    {
        RenderPassRecordIdentity identity{};
        RayTracedShadowPassStats stats{};
        bool legacyAdapter = false;
    };

    struct RayTracedShadowHistoryOwner
    {
        struct PendingTimingSample
        {
            RenderPassRecordIdentity identity{};
            GPUCompletionToken completion{};
            RHIQueryPoolRef queryPool;
            RHIBufferRef readbackBuffer;
            uint64 timestampFrequency = 0;
        };

        std::array<RHITextureRef, RVX_RAY_TRACED_SHADOW_HISTORY_SLOT_COUNT> masks{};
        std::array<RHITextureRef, RVX_RAY_TRACED_SHADOW_HISTORY_SLOT_COUNT> depths{};
        std::array<RHITextureRef, RVX_RAY_TRACED_SHADOW_HISTORY_SLOT_COUNT> normals{};
        std::array<RHITextureAccessSnapshot, RVX_RAY_TRACED_SHADOW_HISTORY_SLOT_COUNT> maskAccesses{};
        std::array<RHITextureAccessSnapshot, RVX_RAY_TRACED_SHADOW_HISTORY_SLOT_COUNT> depthAccesses{};
        std::array<RHITextureAccessSnapshot, RVX_RAY_TRACED_SHADOW_HISTORY_SLOT_COUNT> normalAccesses{};
        std::vector<std::shared_ptr<RayTracedShadowHistoryReservation>> reservations;
        std::vector<std::shared_ptr<RayTracedShadowSubmissionRecord>> submissionRecords;
        std::vector<PendingTimingSample> pendingTimingSamples;
        std::vector<Ref<RefCounted>> pendingOwnerRetirements;
        uint32 width = 0;
        uint32 height = 0;
        uint32 committedSlot = RVX_INVALID_INDEX;
        bool historyValid = false;
        bool historyViewValid = false;
        bool lastHistoryConfigValid = false;
        ShadowPassConfig lastHistoryConfig{};
        Mat4 lastHistoryViewProjection = Mat4Identity();
        Vec3 lastHistoryRayDirection{0.0f, 1.0f, 0.0f};
        RenderPassRecordIdentity lastCommittedIdentity{};
        RenderPassRecordIdentity lastSubmittedIdentity{};
        uint64 allocationGeneration = 1;
    };

    struct RayTracedShadowFrameState
    {
        RenderPassExecutionData execution{};
        std::shared_ptr<RayTracedShadowHistoryOwner> historyOwner;
        std::shared_ptr<RayTracedShadowHistoryReservation> reservation;
        std::shared_ptr<RayTracedShadowSubmissionRecord> submissionRecord;
        std::shared_ptr<RayTracedShadowExecutionState> executionState;
        RayTracedShadowPassStats stats{};
        RayTracedShadowRecordOutput output{};
        ShadowPassConfig config{};
        PrimaryDirectionalLightRecordInput primaryDirectionalLight{};
        bool contextValid = false;

        RHIAccelerationStructureRef tlas;
        RHIBufferRef materialMetadata;
        RHIBufferRef alphaMetadata;
        std::vector<RHITextureViewRef> materialTextureViews;
        std::vector<RHITextureViewRef> alphaTextureViews;
        std::vector<RHIBufferRef> alphaIndexBuffers;
        std::vector<RHIBufferRef> alphaUVBuffers;
        RHITextureRef depthTexture;
        RHITextureRef velocityTexture;
        RHITextureRef historyReadTexture;
        RHITextureRef historyDepthReadTexture;
        RHITextureRef historyDepthWriteTexture;
        RHITextureRef historyNormalReadTexture;
        RHITextureRef historyNormalWriteTexture;
        RHITextureRef shadowMaskTexture;
        RHITextureRef fallbackVelocityTexture;
        RHITextureRef fallbackHistoryMaskTexture;
        RHITextureRef fallbackHistoryDepthTexture;
        RHITextureRef fallbackHistoryNormalTexture;
        RHIBufferRef constantBuffer;
        RHIQueryPoolRef timingQueryPool;
        RHIBufferRef timingReadbackBuffer;
        RHIPipelineRef pipeline;
        RHIShaderTableRef shaderTable;
        RHIDescriptorSetLayoutRef setLayout;
        RHIDescriptorSetRef descriptorSet;
        ResourceViewCache* viewCache = nullptr;
        IRHIDevice* descriptorDevice = nullptr;
        bool reverseZ = false;
        RGTextureHandle depthHandle{};
        RGTextureHandle velocityHandle{};
        RGTextureHandle historyReadHandle{};
        RGTextureHandle historyDepthReadHandle{};
        RGTextureHandle historyDepthWriteHandle{};
        RGTextureHandle historyNormalReadHandle{};
        RGTextureHandle historyNormalWriteHandle{};
        RGTextureHandle shadowMaskHandle{};
        RGTextureViewHandle shadowMaskViewHandle{};
        RGTextureViewHandle depthViewHandle{};
        RGTextureViewHandle velocityViewHandle{};
        RGTextureViewHandle historyReadViewHandle{};
        RGTextureViewHandle historyDepthReadViewHandle{};
        RGTextureViewHandle historyDepthWriteViewHandle{};
        RGTextureViewHandle historyNormalReadViewHandle{};
        RGTextureViewHandle historyNormalWriteViewHandle{};
    };

    namespace
    {
        void QueueHistoryOwnerRetirement(RayTracedShadowHistoryOwner& owner,
                                         RHITextureRef& texture)
        {
            QueueRenderOwnerRetirement(texture, owner.pendingOwnerRetirements);
        }

        void ResetHistoryTextures(RayTracedShadowHistoryOwner& owner,
                                  ResourceViewCache* viewCache)
        {
            for (uint32 index = 0; index < RVX_RAY_TRACED_SHADOW_HISTORY_SLOT_COUNT; ++index)
            {
                if (viewCache)
                {
                    if (owner.masks[index]) viewCache->InvalidateTexture(owner.masks[index].Get());
                    if (owner.depths[index]) viewCache->InvalidateTexture(owner.depths[index].Get());
                    if (owner.normals[index]) viewCache->InvalidateTexture(owner.normals[index].Get());
                }
                QueueHistoryOwnerRetirement(owner, owner.masks[index]);
                QueueHistoryOwnerRetirement(owner, owner.depths[index]);
                QueueHistoryOwnerRetirement(owner, owner.normals[index]);
                owner.maskAccesses[index] = MakeRHITextureAccessSnapshot(RHIResourceState::Common);
                owner.depthAccesses[index] = MakeRHITextureAccessSnapshot(RHIResourceState::Common);
                owner.normalAccesses[index] = MakeRHITextureAccessSnapshot(RHIResourceState::Common);
            }
            owner.width = 0;
            owner.height = 0;
            owner.committedSlot = RVX_INVALID_INDEX;
            owner.historyValid = false;
            owner.historyViewValid = false;
            owner.lastHistoryConfigValid = false;
            owner.lastCommittedIdentity = {};
            ++owner.allocationGeneration;
            if (owner.allocationGeneration == 0)
            {
                ++owner.allocationGeneration;
            }
        }

        bool EnsureHistoryTextures(RayTracedShadowHistoryOwner& owner,
                                   IRHIDevice* device,
                                   ResourceViewCache* viewCache,
                                   uint32 width,
                                   uint32 height,
                                   RayTracedShadowPassStats& stats)
        {
            if (!device || width == 0 || height == 0)
            {
                return false;
            }
            const bool complete = std::all_of(
                owner.masks.begin(), owner.masks.end(), [](const RHITextureRef& texture) { return texture != nullptr; }) &&
                std::all_of(
                    owner.depths.begin(), owner.depths.end(), [](const RHITextureRef& texture) { return texture != nullptr; }) &&
                std::all_of(
                    owner.normals.begin(), owner.normals.end(), [](const RHITextureRef& texture) { return texture != nullptr; });
            const bool resolutionChanged = complete && (owner.width != width || owner.height != height);
            if (complete && !resolutionChanged)
            {
                return true;
            }

            // Reallocating the ring while another graph still owns one of its
            // slots invalidates that graph's imported resources.  Leave the
            // current generation intact and reject the new recording until the
            // outstanding reservation is submitted or released.
            if (resolutionChanged && std::any_of(
                    owner.reservations.begin(), owner.reservations.end(),
                    [&owner](const std::shared_ptr<RayTracedShadowHistoryReservation>& reservation)
                    {
                        return reservation && !reservation->cancelled &&
                               reservation->allocationGeneration == owner.allocationGeneration;
                    }))
            {
                return false;
            }

            stats.historyRecreated = true;
            stats.historyResolutionChanged = resolutionChanged;
            stats.historyReset = true;
            ResetHistoryTextures(owner, viewCache);

            RHITextureDesc desc;
            desc.width = width;
            desc.height = height;
            desc.depth = 1;
            desc.mipLevels = 1;
            desc.arraySize = 1;
            desc.format = RHIFormat::R8_UNORM;
            desc.dimension = RHITextureDimension::Texture2D;
            desc.usage = RHITextureUsage::ShaderResource | RHITextureUsage::UnorderedAccess;
            for (uint32 index = 0; index < RVX_RAY_TRACED_SHADOW_HISTORY_SLOT_COUNT; ++index)
            {
                desc.debugName = "RayTracedShadowHistory";
                owner.masks[index] = device->CreateTexture(desc);
                desc.format = RHIFormat::R32_FLOAT;
                desc.debugName = "RayTracedShadowDepthHistory";
                owner.depths[index] = device->CreateTexture(desc);
                desc.format = RHIFormat::RGBA16_FLOAT;
                desc.debugName = "RayTracedShadowNormalHistory";
                owner.normals[index] = device->CreateTexture(desc);
                desc.format = RHIFormat::R8_UNORM;
                if (!owner.masks[index] || !owner.depths[index] || !owner.normals[index])
                {
                    ResetHistoryTextures(owner, viewCache);
                    return false;
                }
            }
            owner.width = width;
            owner.height = height;
            return true;
        }

        std::shared_ptr<RayTracedShadowHistoryReservation> ReserveHistory(
            RayTracedShadowHistoryOwner& owner,
            const RenderPassRecordIdentity& identity,
            const ShadowPassConfig& config,
            const Mat4& currentViewProjection,
            const Vec3& currentRayDirection,
            bool resetRequested,
            RayTracedShadowPassStats& stats)
        {
            const bool configChanged = owner.lastHistoryConfigValid &&
                ShadowHistoryConfigChanged(owner.lastHistoryConfig, config);
            stats.historyConfigChanged = configChanged;
            stats.historyReset = stats.historyReset || resetRequested || configChanged;
            const bool reusableHistory = owner.historyValid && !stats.historyReset &&
                owner.committedSlot != RVX_INVALID_INDEX;

            uint32 writeSlot = RVX_INVALID_INDEX;
            for (uint32 candidate = 0; candidate < RVX_RAY_TRACED_SHADOW_HISTORY_SLOT_COUNT; ++candidate)
            {
                if (candidate == owner.committedSlot)
                {
                    continue;
                }
                const bool reserved = std::any_of(
                    owner.reservations.begin(), owner.reservations.end(),
                    [&owner, candidate](const std::shared_ptr<RayTracedShadowHistoryReservation>& reservation)
                    {
                        return reservation && !reservation->cancelled &&
                               reservation->allocationGeneration == owner.allocationGeneration &&
                               reservation->writeSlot == candidate;
                    });
                if (!reserved)
                {
                    writeSlot = candidate;
                    break;
                }
            }
            if (writeSlot == RVX_INVALID_INDEX)
            {
                return nullptr;
            }

            auto reservation = std::make_shared<RayTracedShadowHistoryReservation>();
            reservation->identity = identity;
            reservation->allocationGeneration = owner.allocationGeneration;
            reservation->writeSlot = writeSlot;
            // A recording may only read a submitted history slot.  In-flight
            // writers intentionally have no read slot: using another ring slot
            // here would create an undeclared cross-graph dependency when
            // recordings are compiled or submitted out of order.
            reservation->readSlot = reusableHistory ? owner.committedSlot : RVX_INVALID_INDEX;
            reservation->historyAvailable = reusableHistory;
            reservation->historyViewValid = reusableHistory && owner.historyViewValid;
            reservation->previousViewProjection = owner.lastHistoryViewProjection;
            reservation->previousRayDirection = owner.lastHistoryRayDirection;
            reservation->config = config;
            reservation->currentViewProjection = currentViewProjection;
            reservation->currentRayDirection = currentRayDirection;
            owner.reservations.push_back(reservation);
            return reservation;
        }

        void PublishFrameResults(const std::shared_ptr<RayTracedShadowFrameState>& state)
        {
            if (!state)
            {
                return;
            }
            if (state->submissionRecord)
            {
                state->submissionRecord->stats = state->stats;
            }
            if (!state->execution.results ||
                state->execution.results->identity != state->execution.identity)
            {
                return;
            }
            state->execution.results->rayTracedShadowOutput = state->output;
            state->execution.results->rayTracedShadowStats = state->stats;
        }

        template<typename T>
        bool RetainSnapshot(RenderGraphBuilder& builder,
                            const Ref<T>& resource)
        {
            return !resource || builder.RetainSubmissionResource(
                Ref<RefCounted>(resource));
        }

        bool RetainFrameResources(const RayTracedShadowFrameState& state,
                                  RenderGraphBuilder& builder)
        {
            const auto retain = [&builder]<typename T>(const Ref<T>& resource)
            {
                return RetainSnapshot(builder, resource);
            };
            if (!retain(state.tlas) || !retain(state.materialMetadata) || !retain(state.alphaMetadata) ||
                !retain(state.depthTexture) || !retain(state.velocityTexture) ||
                !retain(state.fallbackVelocityTexture) ||
                !retain(state.historyReadTexture) || !retain(state.historyDepthReadTexture) ||
                !retain(state.historyDepthWriteTexture) || !retain(state.historyNormalReadTexture) ||
                !retain(state.historyNormalWriteTexture) || !retain(state.shadowMaskTexture) ||
                !retain(state.constantBuffer) || !retain(state.timingQueryPool) ||
                !retain(state.timingReadbackBuffer) || !retain(state.pipeline) ||
                !retain(state.shaderTable) || !retain(state.setLayout))
            {
                return false;
            }
            for (const RHITextureViewRef& view : state.materialTextureViews)
            {
                if (!retain(view)) return false;
            }
            for (const RHITextureViewRef& view : state.alphaTextureViews)
            {
                if (!retain(view)) return false;
            }
            for (const RHIBufferRef& buffer : state.alphaIndexBuffers)
            {
                if (!retain(buffer)) return false;
            }
            for (const RHIBufferRef& buffer : state.alphaUVBuffers)
            {
                if (!retain(buffer)) return false;
            }
            return true;
        }
    } // namespace

    void RayTracedShadowPass::OnAdd(IRHIDevice* device)
    {
        if (!m_historyOwner)
        {
            m_historyOwner = std::make_shared<RayTracedShadowHistoryOwner>();
        }
        if (m_device != device)
        {
            ResetHistoryTextures(*m_historyOwner, m_viewCache);
            m_historyOwner->pendingTimingSamples.clear();
            m_historyOwner->reservations.clear();
            m_historyOwner->submissionRecords.clear();
            m_historyOwner->lastSubmittedIdentity = {};
            m_lastSubmittedStats = {};
        }
        m_device = device;
    }

    void RayTracedShadowPass::OnRemove()
    {
        if (m_historyOwner)
        {
            ResetHistoryTextures(*m_historyOwner, m_viewCache);
            m_historyOwner->pendingTimingSamples.clear();
            m_historyOwner->reservations.clear();
            m_historyOwner->submissionRecords.clear();
            m_historyOwner->lastSubmittedIdentity = {};
        }
        m_device = nullptr;
        m_pipelineCache = nullptr;
        m_viewCache = nullptr;
        m_resourceRegistry = nullptr;
        m_sceneManager = nullptr;
        m_submissionTracker = nullptr;
        m_lastSubmittedStats = {};
        m_enabled = false;
    }

    void RayTracedShadowPass::AddToGraph(RenderGraph& graph, const ViewData& view)
    {
        AddToGraph(graph, MakeRenderPassRecordContext(graph, view));
    }

    void RayTracedShadowPass::AddToGraph(RenderGraph& graph,
                                         const RenderPassRecordContext& context)
    {
        const RenderPassExecutionData execution = MakeRenderPassExecutionData(context);
        auto state = std::make_shared<RayTracedShadowFrameState>();
        state->execution = execution;
        state->output.identity = execution.identity;
        state->executionState = std::make_shared<RayTracedShadowExecutionState>();
        state->executionState->identity = execution.identity;
        state->output.executionState = state->executionState;
        state->config = m_config;
        state->primaryDirectionalLight = execution.frameSnapshot
            ? execution.frameSnapshot->primaryDirectionalLight
            : PrimaryDirectionalLightRecordInput{};
        state->stats.requested = m_enabled;

        // Every invocation first publishes a disabled value for this exact
        // recording. Feature and light eligibility are authoritative record
        // gates: neither may inspect RT support/resources, reserve history,
        // register graph work, or dispatch rays.
        PublishFrameResults(state);
        if (!m_enabled || !state->primaryDirectionalLight.IsShadowEligible())
        {
            return;
        }

        PollCompletedTimingSamples();
        state->historyOwner = m_historyOwner;
        state->viewCache = m_viewCache;
        state->descriptorDevice = m_pipelineCache ? m_pipelineCache->GetDevice() : nullptr;
        state->reverseZ = m_pipelineCache && m_pipelineCache->GetConfig().reverseZ;
        state->contextValid = context.MatchesTargetGraph(graph) &&
            execution.MatchesTargetGraph(graph) && execution.IsFrameIdentityValid() &&
            state->historyOwner != nullptr;
        state->stats.supported = state->contextValid && IsSupported();

        // The legacy ViewData overload has no identity at its completion
        // boundary.  Keep that bounded adapter usable for one graph only:
        // accepting a second reservation would make its completion ambiguous
        // and permanently occupy ring slots when the no-identity completion is
        // intentionally rejected below.
        const bool legacyRecordingAlreadyActive = context.legacyAdapter &&
            state->historyOwner && std::any_of(
                state->historyOwner->submissionRecords.begin(),
                state->historyOwner->submissionRecords.end(),
                [](const std::shared_ptr<RayTracedShadowSubmissionRecord>& record)
                {
                    return record && record->legacyAdapter;
                });
        if (legacyRecordingAlreadyActive)
        {
            RVX_RENDER_WARN("RayTracedShadowPass: rejecting a concurrent legacy recording; use the identity-aware record context");
            PublishFrameResults(state);
            return;
        }

        // Every identity-aware Add attempt owns a final diagnostic record,
        // including attempts that later reject setup or cannot reserve history.
        // NotifySubmission publishes this immutable identity ordering rather
        // than treating only successful dispatches as submissions.
        if (state->contextValid)
        {
            const auto existingRecord = std::find_if(
                state->historyOwner->submissionRecords.begin(),
                state->historyOwner->submissionRecords.end(),
                [&execution](const std::shared_ptr<RayTracedShadowSubmissionRecord>& record)
                {
                    return record && record->identity == execution.identity;
                });
            if (existingRecord != state->historyOwner->submissionRecords.end())
            {
                state->submissionRecord = *existingRecord;
            }
            else
            {
                state->submissionRecord = std::make_shared<RayTracedShadowSubmissionRecord>();
                state->submissionRecord->identity = execution.identity;
                state->submissionRecord->legacyAdapter = context.legacyAdapter;
                state->historyOwner->submissionRecords.push_back(state->submissionRecord);
            }
        }

        if (!state->contextValid || !state->execution.view.depthTarget.IsValid() ||
            state->execution.view.viewportWidth == 0 || state->execution.view.viewportHeight == 0 ||
            !m_device || !m_sceneManager || !m_pipelineCache || !m_viewCache)
        {
            PublishFrameResults(state);
            return;
        }

        state->stats.tlasAvailable = m_sceneManager->GetTopLevelAS() != nullptr;
        state->stats.materialMetadataAvailable = m_sceneManager->GetInstanceMaterialMetadataBuffer() != nullptr;
        state->stats.alphaMetadataAvailable = m_sceneManager->GetInstanceAlphaMetadataBuffer() != nullptr;
        state->stats.depthAvailable = true;
        state->stats.velocityAvailable = state->execution.view.velocityTarget.IsValid();
        state->stats.width = state->execution.view.viewportWidth;
        state->stats.height = state->execution.view.viewportHeight;
        state->stats.samplesPerPixel = std::clamp(state->config.rayTracedSamplesPerPixel, 1u, 8u);
        if (!TryGetRHIRayTracingDispatchRayCount(state->stats.width, state->stats.height, 1, state->stats.dispatchPixelCount) ||
            !TryMultiplyRHIRayTracingCount(state->stats.dispatchPixelCount,
                                           static_cast<uint64>(state->stats.samplesPerPixel),
                                           state->stats.estimatedRayCount))
        {
            PublishFrameResults(state);
            return;
        }

        const std::vector<uint64> materialTextureIds = m_sceneManager->GetInstanceMaterialTextureTable();
        const std::vector<uint64> alphaTextureIds = m_sceneManager->GetInstanceAlphaTextureTable();
        const std::vector<RHIBuffer*> alphaIndexBuffers = m_sceneManager->GetInstanceAlphaIndexBufferTable();
        const std::vector<RHIBuffer*> alphaUVBuffers = m_sceneManager->GetInstanceAlphaUVBufferTable();
        state->stats.materialTextureCount = static_cast<uint32>(materialTextureIds.size());
        state->stats.alphaTextureCount = static_cast<uint32>(alphaTextureIds.size());
        state->stats.alphaIndexBufferCount = static_cast<uint32>(alphaIndexBuffers.size());
        state->stats.alphaUVBufferCount = static_cast<uint32>(alphaUVBuffers.size());
        state->stats.materialTextureTableAvailable = materialTextureIds.empty() ||
            (m_resourceRegistry != nullptr && materialTextureIds.size() <=
                RTShadowBindings::RVX_RT_SHADOW_MAX_MATERIAL_TEXTURES);
        state->stats.alphaTextureTableAvailable = alphaTextureIds.empty() ||
            (m_resourceRegistry != nullptr && alphaTextureIds.size() <=
                RTShadowBindings::RVX_RT_SHADOW_MAX_ALPHA_TEXTURES);
        state->stats.alphaGeometryTableAvailable =
            alphaIndexBuffers.size() <= RTShadowBindings::RVX_RT_SHADOW_MAX_ALPHA_GEOMETRY_BUFFERS &&
            alphaUVBuffers.size() <= RTShadowBindings::RVX_RT_SHADOW_MAX_ALPHA_GEOMETRY_BUFFERS;
        if (materialTextureIds.size() > RTShadowBindings::RVX_RT_SHADOW_MAX_MATERIAL_TEXTURES ||
            alphaTextureIds.size() > RTShadowBindings::RVX_RT_SHADOW_MAX_ALPHA_TEXTURES ||
            alphaIndexBuffers.size() > RTShadowBindings::RVX_RT_SHADOW_MAX_ALPHA_GEOMETRY_BUFFERS ||
            alphaUVBuffers.size() > RTShadowBindings::RVX_RT_SHADOW_MAX_ALPHA_GEOMETRY_BUFFERS)
        {
            PublishFrameResults(state);
            return;
        }

        state->tlas = RHIAccelerationStructureRef(m_sceneManager->GetTopLevelAS());
        state->materialMetadata = RHIBufferRef(m_sceneManager->GetInstanceMaterialMetadataBuffer());
        state->alphaMetadata = RHIBufferRef(m_sceneManager->GetInstanceAlphaMetadataBuffer());
        if (!state->tlas || !state->materialMetadata || !state->alphaMetadata)
        {
            PublishFrameResults(state);
            return;
        }
        if (!state->stats.supported)
        {
            PublishFrameResults(state);
            return;
        }
        if ((!materialTextureIds.empty() || !alphaTextureIds.empty()) && !m_resourceRegistry)
        {
            PublishFrameResults(state);
            return;
        }
        for (uint64 id : materialTextureIds)
        {
            RHITexture* texture = m_resourceRegistry->ResolveTextureObject(UnpackRenderResourceHandle(id));
            RHITextureView* view = texture ? m_viewCache->GetDefaultSRV(texture) : nullptr;
            if (!view)
            {
                PublishFrameResults(state);
                return;
            }
            state->materialTextureViews.emplace_back(view);
        }
        for (uint64 id : alphaTextureIds)
        {
            RHITexture* texture = m_resourceRegistry->ResolveTextureObject(UnpackRenderResourceHandle(id));
            RHITextureView* view = texture ? m_viewCache->GetDefaultSRV(texture) : nullptr;
            if (!view)
            {
                PublishFrameResults(state);
                return;
            }
            state->alphaTextureViews.emplace_back(view);
        }
        for (RHIBuffer* buffer : alphaIndexBuffers)
        {
            if (!buffer)
            {
                PublishFrameResults(state);
                return;
            }
            state->alphaIndexBuffers.emplace_back(buffer);
        }
        for (RHIBuffer* buffer : alphaUVBuffers)
        {
            if (!buffer)
            {
                PublishFrameResults(state);
                return;
            }
            state->alphaUVBuffers.emplace_back(buffer);
        }

        const Vec3 rayDirection = NormalizeOr(
            -state->primaryDirectionalLight.direction, Vec3(0.0f, 1.0f, 0.0f));
        // Physical allocation is established before the logical reservation.
        // This prevents a resize from invalidating a reservation that another
        // graph has already observed, while keeping history validity a
        // submission-time decision.
        if (!EnsureHistoryTextures(*state->historyOwner,
                                   m_device,
                                   m_viewCache,
                                   state->stats.width,
                                   state->stats.height,
                                   state->stats))
        {
            PublishFrameResults(state);
            return;
        }

        state->reservation = ReserveHistory(*state->historyOwner,
                                            state->execution.identity,
                                            state->config,
                                            state->execution.view.viewProjectionMatrix,
                                            rayDirection,
                                            state->execution.view.resetTemporalHistory,
                                            state->stats);
        if (!state->reservation)
        {
            PublishFrameResults(state);
            return;
        }
        state->stats.historyAvailable = state->reservation->historyAvailable;
        state->stats.depthHistoryAvailable = state->reservation->historyAvailable;
        state->stats.normalHistoryAvailable = state->reservation->historyAvailable;
        if (!CreateFrameConstantBuffer(*state) || !CreateFrameTimingResources(*state) ||
            !CreateFrameFallbackTextures(*state))
        {
            state->reservation->cancelled = true;
            PublishFrameResults(state);
            return;
        }

        graph.AddPass<std::shared_ptr<RayTracedShadowFrameState>>(
            GetName(),
            GetPassType(),
            [this, state](RenderGraphBuilder& builder, std::shared_ptr<RayTracedShadowFrameState>& data)
            {
                data = state;
                const auto publishRejectedSetup = [&data]()
                {
                    if (data && data->reservation)
                    {
                        // Setup can fail after history-slot reservation but
                        // before a command was recorded (for example, when a
                        // mapped constant write cannot be committed). Keep
                        // that slot out of both submission and temporal
                        // promotion paths.
                        data->reservation->cancelled = true;
                    }
                    PublishFrameResults(data);
                };
                if (!data || !data->reservation || data->reservation->cancelled)
                {
                    publishRejectedSetup();
                    return;
                }
                RayTracedShadowHistoryOwner& owner = *data->historyOwner;
                const uint32 read = data->reservation->readSlot;
                const uint32 write = data->reservation->writeSlot;
                ViewData& view = data->execution.view;
                RGTextureHandle depth = view.depthTarget;
                depth.hasSubresourceRange = true;
                depth.subresourceRange = RHISubresourceRange{0, RVX_ALL_MIPS, 0, RVX_ALL_LAYERS, RHITextureAspect::Depth};
                const RGTextureHandle velocityHandle = view.velocityTarget.IsValid()
                    ? view.velocityTarget
                    : builder.ImportTexture(
                        data->fallbackVelocityTexture,
                        RHIResourceState::Common);
                const RGTextureHandle historyRead = read != RVX_INVALID_INDEX
                    ? builder.ImportTexture(owner.masks[read], owner.maskAccesses[read])
                    : builder.ImportTexture(data->fallbackHistoryMaskTexture, RHIResourceState::Common);
                const RGTextureHandle historyDepthRead = read != RVX_INVALID_INDEX
                    ? builder.ImportTexture(owner.depths[read], owner.depthAccesses[read])
                    : builder.ImportTexture(data->fallbackHistoryDepthTexture, RHIResourceState::Common);
                const RGTextureHandle historyNormalRead = read != RVX_INVALID_INDEX
                    ? builder.ImportTexture(owner.normals[read], owner.normalAccesses[read])
                    : builder.ImportTexture(data->fallbackHistoryNormalTexture, RHIResourceState::Common);
                const RGTextureHandle shadowMask = builder.ImportTexture(
                    owner.masks[write], owner.maskAccesses[write]);
                const RGTextureHandle historyDepthWrite = builder.ImportTexture(
                    owner.depths[write], owner.depthAccesses[write]);
                const RGTextureHandle historyNormalWrite = builder.ImportTexture(
                    owner.normals[write], owner.normalAccesses[write]);

                const auto declareView = [&builder](
                    RGTextureHandle texture,
                    RHITextureViewType type,
                    const RGAccessDesc& access,
                    const char* debugName)
                {
                    const RHITextureDesc* desc =
                        builder.GetTextureDesc(texture);
                    if (!desc)
                        return RGTextureViewHandle{};
                    RHITextureViewDesc viewDesc;
                    viewDesc.format = desc->format;
                    viewDesc.dimension = desc->dimension;
                    viewDesc.subresourceRange = texture.hasSubresourceRange
                        ? texture.subresourceRange
                        : RHISubresourceRange::All();
                    if (IsDepthFormat(viewDesc.format))
                        viewDesc.subresourceRange.aspect =
                            RHITextureAspect::Depth;
                    viewDesc.type = type;
                    viewDesc.debugName = debugName;
                    const RGTextureViewHandle handle =
                        builder.CreateTextureView(texture, viewDesc);
                    return type == RHITextureViewType::UnorderedAccess
                        ? builder.Write(handle, access)
                        : builder.Read(handle, access);
                };
                const RGAccessDesc rayRead = MakeRGAccessDesc(
                    RHIResourceState::ShaderResource,
                    RHIShaderStage::AllRayTracing);
                const RGAccessDesc rayWrite = MakeRGAccessDesc(
                    RHIResourceState::UnorderedAccess,
                    RHIShaderStage::AllRayTracing,
                    RHIDiscardIntent::Discard);

                data->depthViewHandle = declareView(
                    depth,
                    RHITextureViewType::ShaderResource,
                    rayRead,
                    "RayTracedShadowDepthSRV");
                data->velocityViewHandle = declareView(
                    velocityHandle,
                    RHITextureViewType::ShaderResource,
                    rayRead,
                    "RayTracedShadowVelocitySRV");
                data->historyReadViewHandle = declareView(
                    historyRead,
                    RHITextureViewType::ShaderResource,
                    rayRead,
                    "RayTracedShadowHistoryMaskSRV");
                data->historyDepthReadViewHandle = declareView(
                    historyDepthRead,
                    RHITextureViewType::ShaderResource,
                    rayRead,
                    "RayTracedShadowHistoryDepthSRV");
                data->historyNormalReadViewHandle = declareView(
                    historyNormalRead,
                    RHITextureViewType::ShaderResource,
                    rayRead,
                    "RayTracedShadowHistoryNormalSRV");
                data->shadowMaskViewHandle = declareView(
                    shadowMask,
                    RHITextureViewType::UnorderedAccess,
                    rayWrite,
                    "RayTracedShadowMaskUAV");
                data->historyDepthWriteViewHandle = declareView(
                    historyDepthWrite,
                    RHITextureViewType::UnorderedAccess,
                    rayWrite,
                    "RayTracedShadowHistoryDepthUAV");
                data->historyNormalWriteViewHandle = declareView(
                    historyNormalWrite,
                    RHITextureViewType::UnorderedAccess,
                    rayWrite,
                    "RayTracedShadowHistoryNormalUAV");
                builder.SetExportState(
                    shadowMask, RHIResourceState::ShaderResource);
                builder.SetExportState(
                    historyDepthWrite, RHIResourceState::ShaderResource);
                builder.SetExportState(
                    historyNormalWrite, RHIResourceState::ShaderResource);

                // Transient graph resources do not exist until Compile. Keep
                // only graph handles here; the execution callback resolves them
                // from its immutable recording state after graph realization.
                data->depthHandle = depth;
                data->velocityHandle = velocityHandle;
                data->historyReadHandle = historyRead;
                data->historyDepthReadHandle = historyDepthRead;
                data->historyDepthWriteHandle = historyDepthWrite;
                data->historyNormalReadHandle = historyNormalRead;
                data->historyNormalWriteHandle = historyNormalWrite;
                data->shadowMaskHandle = shadowMask;
                data->reservation->historyReadHandle = read != RVX_INVALID_INDEX
                    ? historyRead : RGTextureHandle{};
                data->reservation->historyDepthReadHandle = read != RVX_INVALID_INDEX
                    ? historyDepthRead : RGTextureHandle{};
                data->reservation->historyNormalReadHandle = read != RVX_INVALID_INDEX
                    ? historyNormalRead : RGTextureHandle{};
                data->reservation->shadowMaskHandle = shadowMask;
                data->reservation->historyDepthWriteHandle = historyDepthWrite;
                data->reservation->historyNormalWriteHandle = historyNormalWrite;
                data->historyReadTexture = read != RVX_INVALID_INDEX
                    ? owner.masks[read] : data->fallbackHistoryMaskTexture;
                data->historyDepthReadTexture = read != RVX_INVALID_INDEX
                    ? owner.depths[read] : data->fallbackHistoryDepthTexture;
                data->historyNormalReadTexture = read != RVX_INVALID_INDEX
                    ? owner.normals[read] : data->fallbackHistoryNormalTexture;
                data->shadowMaskTexture = owner.masks[write];
                data->historyDepthWriteTexture = owner.depths[write];
                data->historyNormalWriteTexture = owner.normals[write];
                data->pipeline = RHIPipelineRef(m_pipelineCache->GetRayTracedShadowPipeline());
                data->shaderTable = RHIShaderTableRef(m_pipelineCache->GetRayTracedShadowShaderTable());
                data->setLayout = RHIDescriptorSetLayoutRef(m_pipelineCache->GetRayTracedShadowSetLayout());
                data->stats.gpuTimingSupported = data->timingQueryPool != nullptr;
                data->stats.gpuTimingReadbackBufferAvailable = data->timingReadbackBuffer != nullptr;
                data->stats.gpuTimingReadbackBufferCount =
                    data->timingReadbackBuffer ? 1u : 0u;
                data->stats.gpuTimingReadbackBytes = data->timingReadbackBuffer
                    ? RVX_RAY_TRACED_SHADOW_TIMING_READBACK_BYTES : 0u;
                data->stats.gpuTimingReadbackFrameIndex = RVX_INVALID_INDEX;
                data->stats.gpuTimingStartQueryIndex = RVX_RAY_TRACED_SHADOW_TIMING_START_QUERY_OFFSET;
                data->stats.gpuTimingEndQueryIndex = RVX_RAY_TRACED_SHADOW_TIMING_END_QUERY_OFFSET;
                data->stats.gpuTimestampFrequency = data->timingQueryPool ? data->timingQueryPool->GetTimestampFrequency() : 0;
                if (!data->pipeline || !data->shaderTable || !data->setLayout || !data->descriptorDevice ||
                    !data->viewCache || !data->historyReadTexture || !data->historyDepthReadTexture ||
                    !data->historyDepthWriteTexture || !data->historyNormalReadTexture ||
                    !data->historyNormalWriteTexture || !data->shadowMaskTexture || !data->constantBuffer ||
                    !data->depthViewHandle.IsValid() ||
                    !data->velocityViewHandle.IsValid() ||
                    !data->historyReadViewHandle.IsValid() ||
                    !data->historyDepthReadViewHandle.IsValid() ||
                    !data->historyNormalReadViewHandle.IsValid() ||
                    !data->shadowMaskViewHandle.IsValid() ||
                    !data->historyDepthWriteViewHandle.IsValid() ||
                    !data->historyNormalWriteViewHandle.IsValid())
                {
                    publishRejectedSetup();
                    return;
                }
                if (!UpdateConstants(*data))
                {
                    publishRejectedSetup();
                    return;
                }
                if (!RetainFrameResources(*data, builder))
                {
                    publishRejectedSetup();
                    return;
                }
                data->output.enabled = true;
                data->output.shadowMask = shadowMask;
                data->output.filterRadiusTexels = std::max(0.0f, data->config.filterRadiusTexels);
                data->output.mode = data->config.rayTracedShadowMode;
                data->stats.outputDeclared = true;
                PublishFrameResults(data);
            },
            [](const std::shared_ptr<RayTracedShadowFrameState>& data,
               RenderGraphPassContext& context)
            {
                if (data && data->reservation)
                {
                    data->reservation->graphPassExecuted = true;
                }
                const auto failClosed = [&data](const char* reason)
                {
                    if (!data)
                    {
                        return;
                    }
                    data->output.enabled = false;
                    data->stats.executionFailed = true;
                    if (data->executionState)
                    {
                        data->executionState->dispatchReady.store(false, std::memory_order_release);
                        data->executionState->executionFailed.store(true, std::memory_order_release);
                    }
                    RVX_RENDER_WARN("RayTracedShadowPass: execution failed closed: {}", reason);
                    PublishFrameResults(data);
                };
                if (!data || !data->output.enabled || !data->reservation || data->reservation->cancelled ||
                    !data->pipeline || !data->shaderTable || !data->setLayout || !data->descriptorDevice ||
                    !data->viewCache)
                {
                    failClosed("recorded execution prerequisites are unavailable");
                    return;
                }

                data->depthTexture = RHITextureRef(
                    context.GetTexture(data->depthHandle));
                data->velocityTexture = RHITextureRef(
                    context.GetTexture(data->velocityHandle));
                if (!data->depthTexture || !data->velocityTexture)
                {
                    failClosed("compiled transient depth or velocity texture is unavailable");
                    return;
                }
                RHITextureView* shadowMaskUAV = context.GetTextureView(
                    data->shadowMaskViewHandle);
                RHITextureView* depthSRV = context.GetTextureView(
                    data->depthViewHandle);
                RHITextureView* velocitySRV = context.GetTextureView(
                    data->velocityViewHandle);
                RHITextureView* previousShadowMaskSRV = context.GetTextureView(
                    data->historyReadViewHandle);
                RHITextureView* previousDepthHistorySRV = context.GetTextureView(
                    data->historyDepthReadViewHandle);
                RHITextureView* currentDepthHistoryUAV = context.GetTextureView(
                    data->historyDepthWriteViewHandle);
                RHITextureView* previousNormalHistorySRV = context.GetTextureView(
                    data->historyNormalReadViewHandle);
                RHITextureView* currentNormalHistoryUAV = context.GetTextureView(
                    data->historyNormalWriteViewHandle);
                if (!shadowMaskUAV || !depthSRV || !velocitySRV ||
                    !previousShadowMaskSRV || !previousDepthHistorySRV ||
                    !currentDepthHistoryUAV || !previousNormalHistorySRV ||
                    !currentNormalHistoryUAV)
                {
                    failClosed("ray-traced shadow texture view creation failed");
                    return;
                }
                data->stats.resourceViewsAvailable = true;
                RHIDescriptorSetDesc descriptorDesc;
                descriptorDesc.layout = data->setLayout.Get();
                descriptorDesc.debugName = "RayTracedShadowDescriptorSet";
                descriptorDesc.BindAccelerationStructure(RTShadowBindings::RVX_RT_SHADOW_TLAS_BINDING, data->tlas.Get());
                descriptorDesc.BindTexture(RTShadowBindings::RVX_RT_SHADOW_OUTPUT_MASK_BINDING, shadowMaskUAV);
                descriptorDesc.BindTexture(RTShadowBindings::RVX_RT_SHADOW_SCENE_DEPTH_BINDING, depthSRV);
                descriptorDesc.BindBuffer(RTShadowBindings::RVX_RT_SHADOW_CONSTANTS_BINDING, data->constantBuffer.Get(), 0,
                                          AlignRayTracedShadowConstantBufferSize(sizeof(RayTracedShadowGPUConstants)));
                descriptorDesc.BindTexture(RTShadowBindings::RVX_RT_SHADOW_PREVIOUS_MASK_BINDING, previousShadowMaskSRV);
                descriptorDesc.BindTexture(RTShadowBindings::RVX_RT_SHADOW_PREVIOUS_DEPTH_BINDING, previousDepthHistorySRV);
                descriptorDesc.BindTexture(RTShadowBindings::RVX_RT_SHADOW_OUTPUT_DEPTH_BINDING, currentDepthHistoryUAV);
                descriptorDesc.BindTexture(RTShadowBindings::RVX_RT_SHADOW_PREVIOUS_NORMAL_BINDING, previousNormalHistorySRV);
                descriptorDesc.BindTexture(RTShadowBindings::RVX_RT_SHADOW_OUTPUT_NORMAL_BINDING, currentNormalHistoryUAV);
                descriptorDesc.BindBuffer(RTShadowBindings::RVX_RT_SHADOW_ALPHA_METADATA_BINDING, data->alphaMetadata.Get());
                RHITextureView* alphaFallback = data->alphaTextureViews.empty()
                    ? previousNormalHistorySRV : data->alphaTextureViews.front().Get();
                for (uint32 index = 0; index < RTShadowBindings::RVX_RT_SHADOW_MAX_ALPHA_TEXTURES; ++index)
                {
                    descriptorDesc.BindTexture(RTShadowBindings::RVX_RT_SHADOW_ALPHA_TEXTURES_BINDING,
                                               index < data->alphaTextureViews.size() ? data->alphaTextureViews[index].Get() : alphaFallback,
                                               index);
                }
                for (uint32 index = 0; index < RTShadowBindings::RVX_RT_SHADOW_MAX_ALPHA_GEOMETRY_BUFFERS; ++index)
                {
                    RHIBuffer* buffer = index < data->alphaIndexBuffers.size() ? data->alphaIndexBuffers[index].Get() : data->alphaMetadata.Get();
                    descriptorDesc.BindBuffer(RTShadowBindings::RVX_RT_SHADOW_ALPHA_INDEX_BUFFERS_BINDING, buffer, 0, RVX_WHOLE_SIZE, index);
                    buffer = index < data->alphaUVBuffers.size() ? data->alphaUVBuffers[index].Get() : data->alphaMetadata.Get();
                    descriptorDesc.BindBuffer(RTShadowBindings::RVX_RT_SHADOW_ALPHA_UV_BUFFERS_BINDING, buffer, 0, RVX_WHOLE_SIZE, index);
                }
                descriptorDesc.BindBuffer(RTShadowBindings::RVX_RT_SHADOW_MATERIAL_METADATA_BINDING, data->materialMetadata.Get());
                RHITextureView* materialFallback = data->materialTextureViews.empty()
                    ? previousNormalHistorySRV : data->materialTextureViews.front().Get();
                for (uint32 index = 0; index < RTShadowBindings::RVX_RT_SHADOW_MAX_MATERIAL_TEXTURES; ++index)
                {
                    descriptorDesc.BindTexture(RTShadowBindings::RVX_RT_SHADOW_MATERIAL_TEXTURES_BINDING,
                                               index < data->materialTextureViews.size() ? data->materialTextureViews[index].Get() : materialFallback,
                                               index);
                }
                descriptorDesc.BindTexture(RTShadowBindings::RVX_RT_SHADOW_SCENE_VELOCITY_BINDING, velocitySRV);
                data->descriptorSet = data->descriptorDevice->CreateDescriptorSet(descriptorDesc);
                if (!data->descriptorSet)
                {
                    failClosed("ray-traced shadow descriptor creation failed");
                    return;
                }
                if (!context.RetainSubmissionResource(
                        Ref<RefCounted>(data->descriptorSet)))
                {
                    data->descriptorSet.Reset();
                    failClosed("ray-traced shadow submission resource retention failed");
                    return;
                }
                data->stats.descriptorSetAvailable = true;
                RHICommandContext& ctx = context.Commands();
                ctx.SetPipeline(data->pipeline.Get());
                ctx.SetDescriptorSet(0, data->descriptorSet.Get());
                if (data->timingQueryPool)
                {
                    ctx.WriteTimestamp(data->timingQueryPool.Get(), data->stats.gpuTimingStartQueryIndex);
                }
                RHIDispatchRaysDesc dispatchDesc;
                dispatchDesc.shaderTable = data->shaderTable.Get();
                dispatchDesc.width = data->shadowMaskTexture->GetWidth();
                dispatchDesc.height = data->shadowMaskTexture->GetHeight();
                dispatchDesc.depth = 1;
                ctx.DispatchRays(dispatchDesc);
                if (data->timingQueryPool)
                {
                    ctx.WriteTimestamp(data->timingQueryPool.Get(), data->stats.gpuTimingEndQueryIndex);
                    data->stats.gpuTimingQueriesRecorded = true;
                    if (data->timingReadbackBuffer)
                    {
                        ctx.ResolveQueries(data->timingQueryPool.Get(), data->stats.gpuTimingStartQueryIndex,
                                           RVX_RAY_TRACED_SHADOW_TIMING_QUERY_COUNT_PER_FRAME,
                                           data->timingReadbackBuffer.Get(), 0);
                        data->stats.gpuTimingResolveRecorded = true;
                        data->reservation->timingResolveRecorded = true;
                        data->reservation->timingQueryPool = data->timingQueryPool;
                        data->reservation->timingReadbackBuffer = data->timingReadbackBuffer;
                        data->reservation->timingTimestampFrequency =
                            data->stats.gpuTimestampFrequency;
                    }
                }
                data->reservation->recorded = true;
                data->stats.materialTexturesBound = static_cast<uint32>(data->materialTextureViews.size());
                data->stats.alphaTexturesBound = static_cast<uint32>(data->alphaTextureViews.size());
                data->stats.dispatchRecorded = true;
                data->reservation->recordedStats = data->stats;
                if (data->executionState)
                {
                    data->executionState->dispatchReady.store(true, std::memory_order_release);
                }
                PublishFrameResults(data);
            });
    }

    void RayTracedShadowPass::Setup(RenderGraphBuilder&, const ViewData&)
    {
        // Typed AddToGraph owns all per-recording setup. The virtual hook stays
        // inert solely for IRenderPass compatibility.
    }

    void RayTracedShadowPass::Execute(RHICommandContext&, const ViewData&)
    {
        // See Setup: graph callbacks execute a RayTracedShadowFrameState.
    }

    void RayTracedShadowPass::NotifySubmission(
        const RenderPassRecordIdentity& identity,
        const GPUCompletionToken& completion)
    {
        if (!m_historyOwner || !identity.IsValid())
        {
            return;
        }
        RayTracedShadowHistoryOwner& owner = *m_historyOwner;
        const auto submission = std::find_if(
            owner.submissionRecords.begin(), owner.submissionRecords.end(),
            [&identity](const std::shared_ptr<RayTracedShadowSubmissionRecord>& record)
            {
                return record && record->identity == identity;
            });
        const bool isNewestSubmittedRecording = submission != owner.submissionRecords.end() &&
            IsNewerRecording(identity, owner.lastSubmittedIdentity);
        if (isNewestSubmittedRecording)
        {
            // A submission's diagnostic payload is final even if recording
            // rejected setup or execution failed.  Advancing this watermark on
            // all submitted identities prevents a delayed older success from
            // presenting stale diagnostics or regressing temporal history.
            owner.lastSubmittedIdentity = identity;
            m_lastSubmittedStats = (*submission)->stats;
        }
        for (auto it = owner.reservations.begin(); it != owner.reservations.end(); )
        {
            const std::shared_ptr<RayTracedShadowHistoryReservation>& reservation = *it;
            if (!reservation || reservation->identity != identity)
            {
                ++it;
                continue;
            }
            if (!reservation->cancelled && reservation->graphPassExecuted &&
                reservation->allocationGeneration == owner.allocationGeneration)
            {
                RenderGraph* const recordedGraph = reservation->identity.graph;
                if (recordedGraph && reservation->readSlot != RVX_INVALID_INDEX)
                {
                    owner.maskAccesses[reservation->readSlot] =
                        recordedGraph->GetRealizedAccess(reservation->historyReadHandle);
                    owner.depthAccesses[reservation->readSlot] =
                        recordedGraph->GetRealizedAccess(reservation->historyDepthReadHandle);
                    owner.normalAccesses[reservation->readSlot] =
                        recordedGraph->GetRealizedAccess(reservation->historyNormalReadHandle);
                }
                if (recordedGraph)
                {
                    owner.maskAccesses[reservation->writeSlot] =
                        recordedGraph->GetRealizedAccess(reservation->shadowMaskHandle);
                    owner.depthAccesses[reservation->writeSlot] =
                        recordedGraph->GetRealizedAccess(reservation->historyDepthWriteHandle);
                    owner.normalAccesses[reservation->writeSlot] =
                        recordedGraph->GetRealizedAccess(reservation->historyNormalWriteHandle);
                }
            }
            if (!reservation->cancelled && reservation->recorded &&
                reservation->allocationGeneration == owner.allocationGeneration &&
                isNewestSubmittedRecording &&
                IsNewerRecording(reservation->identity, owner.lastCommittedIdentity))
            {
                owner.committedSlot = reservation->writeSlot;
                owner.historyValid = true;
                owner.historyViewValid = true;
                owner.lastHistoryConfig = reservation->config;
                owner.lastHistoryConfigValid = true;
                owner.lastHistoryViewProjection = reservation->currentViewProjection;
                owner.lastHistoryRayDirection = reservation->currentRayDirection;
                owner.lastCommittedIdentity = reservation->identity;
            }
            if (!reservation->cancelled && reservation->recorded &&
                isNewestSubmittedRecording &&
                reservation->timingResolveRecorded && reservation->timingQueryPool &&
                reservation->timingReadbackBuffer)
            {
                owner.pendingTimingSamples.push_back({
                    reservation->identity,
                    completion,
                    reservation->timingQueryPool,
                    reservation->timingReadbackBuffer,
                    reservation->timingTimestampFrequency});
            }
            it = owner.reservations.erase(it);
        }
        if (submission != owner.submissionRecords.end())
        {
            owner.submissionRecords.erase(submission);
        }
        PollCompletedTimingSamples();
    }

    void RayTracedShadowPass::NotifySubmission(const GPUCompletionToken& completion)
    {
        if (!m_historyOwner)
        {
            return;
        }
        const auto active = std::find_if(
            m_historyOwner->submissionRecords.begin(),
            m_historyOwner->submissionRecords.end(),
            [](const std::shared_ptr<RayTracedShadowSubmissionRecord>& record)
            {
                return record != nullptr && record->legacyAdapter;
            });
        if (active == m_historyOwner->submissionRecords.end())
        {
            return;
        }
        const bool moreThanOne = std::any_of(
            std::next(active), m_historyOwner->submissionRecords.end(),
            [](const std::shared_ptr<RayTracedShadowSubmissionRecord>& record)
            {
                return record != nullptr && record->legacyAdapter;
            });
        if (!moreThanOne)
        {
            NotifySubmission((*active)->identity, completion);
        }
    }

    void RayTracedShadowPass::ReleaseUnsubmittedFrame(
        const RenderPassRecordIdentity& identity)
    {
        if (!m_historyOwner || !identity.IsValid())
        {
            return;
        }
        std::erase_if(m_historyOwner->reservations,
            [&identity](const std::shared_ptr<RayTracedShadowHistoryReservation>& reservation)
            {
                return !reservation || reservation->identity == identity;
            });
        std::erase_if(m_historyOwner->submissionRecords,
            [&identity](const std::shared_ptr<RayTracedShadowSubmissionRecord>& record)
            {
                return !record || record->identity == identity;
            });
    }

    void RayTracedShadowPass::ReleaseUnsubmittedFrame()
    {
        if (!m_historyOwner)
        {
            return;
        }
        const auto active = std::find_if(
            m_historyOwner->submissionRecords.begin(),
            m_historyOwner->submissionRecords.end(),
            [](const std::shared_ptr<RayTracedShadowSubmissionRecord>& record)
            {
                return record != nullptr && record->legacyAdapter;
            });
        if (active == m_historyOwner->submissionRecords.end())
        {
            return;
        }
        const bool moreThanOne = std::any_of(
            std::next(active), m_historyOwner->submissionRecords.end(),
            [](const std::shared_ptr<RayTracedShadowSubmissionRecord>& record)
            {
                return record != nullptr && record->legacyAdapter;
            });
        if (!moreThanOne)
        {
            ReleaseUnsubmittedFrame((*active)->identity);
        }
    }

    void RayTracedShadowPass::PollCompletedTimingSamples()
    {
        if (!m_historyOwner || !m_submissionTracker)
        {
            return;
        }
        RayTracedShadowHistoryOwner& owner = *m_historyOwner;
        for (auto it = owner.pendingTimingSamples.begin();
             it != owner.pendingTimingSamples.end(); )
        {
            const GPUCompletionStatus status = m_submissionTracker->Query(it->completion);
            if (status == GPUCompletionStatus::Pending)
            {
                ++it;
                continue;
            }
            const bool completionSatisfied =
                status == GPUCompletionStatus::Completed ||
                status == GPUCompletionStatus::CompatibilityWaitIdle;
            if (completionSatisfied &&
                it->identity == owner.lastSubmittedIdentity &&
                it->readbackBuffer && it->timestampFrequency != 0)
            {
                if (void* mapped = it->readbackBuffer->Map())
                {
                    uint64 timestamps[2]{};
                    std::memcpy(timestamps, mapped, sizeof(timestamps));
                    it->readbackBuffer->Unmap();
                    if (timestamps[1] >= timestamps[0])
                    {
                        m_lastSubmittedStats.gpuTimingResultAvailable = true;
                        m_lastSubmittedStats.gpuTimingStartTimestamp = timestamps[0];
                        m_lastSubmittedStats.gpuTimingEndTimestamp = timestamps[1];
                        m_lastSubmittedStats.gpuTimingElapsedTicks =
                            timestamps[1] - timestamps[0];
                        m_lastSubmittedStats.gpuTimingElapsedMs = static_cast<float32>(
                            (static_cast<double>(m_lastSubmittedStats.gpuTimingElapsedTicks) * 1000.0) /
                            static_cast<double>(it->timestampFrequency));
                    }
                }
            }
            // Completed samples from superseded recordings and lost samples are
            // intentionally consumed without touching current-record statistics.
            it = owner.pendingTimingSamples.erase(it);
        }
    }

    void RayTracedShadowPass::RefreshCompletionDiagnostics()
    {
        // Deliberately polls only; scheduling policy must never wait for GPU
        // completion in order to consume the latest available diagnostic data.
        PollCompletedTimingSamples();
    }

    void RayTracedShadowPass::RetireOwnerSnapshots(
        const GPUCompletionToken& completion,
        RenderRetirementQueue& retirement)
    {
        if (m_historyOwner)
        {
            FlushRenderOwnerRetirements(
                m_historyOwner->pendingOwnerRetirements, completion, retirement);
        }
    }

    void RayTracedShadowPass::SetResources(PipelineCache* pipelineCache, ResourceViewCache* viewCache)
    {
        m_pipelineCache = pipelineCache;
        m_viewCache = viewCache;
    }

    bool RayTracedShadowPass::IsSupported() const
    {
        if (!m_enabled)
        {
            m_unsupportedReason = "Ray traced shadows have not been requested";
            return false;
        }
        if (!m_device)
        {
            m_unsupportedReason = "Ray traced shadows require an RHI device";
            return false;
        }
        const RHICapabilities& caps = m_device->GetCapabilities();
        if (!caps.supportsRaytracing || !caps.supportsRaytracingPipeline)
        {
            m_unsupportedReason = "RHI device does not support ray tracing pipelines";
            return false;
        }
        if (!m_sceneManager || !m_sceneManager->GetTopLevelAS() ||
            !m_sceneManager->GetInstanceAlphaMetadataBuffer() ||
            !m_sceneManager->GetInstanceMaterialMetadataBuffer())
        {
            m_unsupportedReason = "Ray tracing scene resources are not available";
            return false;
        }
        if (m_sceneManager->GetInstanceMaterialTextureTable().size() >
            RTShadowBindings::RVX_RT_SHADOW_MAX_MATERIAL_TEXTURES)
        {
            m_unsupportedReason =
                "Ray tracing material texture table exceeds the supported descriptor count";
            return false;
        }
        if (m_sceneManager->GetInstanceAlphaTextureTable().size() >
            RTShadowBindings::RVX_RT_SHADOW_MAX_ALPHA_TEXTURES)
        {
            m_unsupportedReason =
                "Ray tracing alpha texture table exceeds the supported descriptor count";
            return false;
        }
        if (m_sceneManager->GetInstanceAlphaIndexBufferTable().size() >
                RTShadowBindings::RVX_RT_SHADOW_MAX_ALPHA_GEOMETRY_BUFFERS ||
            m_sceneManager->GetInstanceAlphaUVBufferTable().size() >
                RTShadowBindings::RVX_RT_SHADOW_MAX_ALPHA_GEOMETRY_BUFFERS)
        {
            m_unsupportedReason =
                "Ray tracing alpha geometry buffer table exceeds the supported descriptor count";
            return false;
        }
        if (!m_pipelineCache || !m_pipelineCache->IsInitialized() ||
            !m_viewCache || !m_viewCache->IsInitialized() ||
            !m_pipelineCache->GetRayTracedShadowPipeline() ||
            !m_pipelineCache->GetRayTracedShadowShaderTable() ||
            !m_pipelineCache->GetRayTracedShadowSetLayout())
        {
            m_unsupportedReason = "Ray traced shadow pipeline resources are not available";
            return false;
        }
        m_unsupportedReason.clear();
        return true;
    }

    bool RayTracedShadowPass::CreateFrameConstantBuffer(
        RayTracedShadowFrameState& state) const
    {
        if (!m_device)
        {
            return false;
        }
        RHIBufferDesc desc;
        desc.size = AlignRayTracedShadowConstantBufferSize(sizeof(RayTracedShadowGPUConstants));
        desc.usage = RHIBufferUsage::Constant;
        desc.memoryType = RHIMemoryType::Upload;
        desc.debugName = "RayTracedShadowConstants";
        state.constantBuffer = m_device->CreateBuffer(desc);
        return state.constantBuffer != nullptr;
    }

    bool RayTracedShadowPass::CreateFrameTimingResources(
        RayTracedShadowFrameState& state) const
    {
        if (!m_device || !m_device->GetCapabilities().supportsTimestampQueries)
        {
            return true;
        }
        RHIQueryPoolDesc queryDesc;
        queryDesc.type = RHIQueryType::Timestamp;
        queryDesc.count = RVX_RAY_TRACED_SHADOW_TIMING_QUERY_COUNT_PER_FRAME;
        queryDesc.debugName = "RayTracedShadowTimingQueries";
        state.timingQueryPool = m_device->CreateQueryPool(queryDesc);
        if (!state.timingQueryPool)
        {
            return false;
        }
        RHIBufferDesc readbackDesc;
        readbackDesc.size = RVX_RAY_TRACED_SHADOW_TIMING_READBACK_BYTES;
        readbackDesc.usage = RHIBufferUsage::CopyDst;
        readbackDesc.memoryType = RHIMemoryType::Readback;
        readbackDesc.debugName = "RayTracedShadowTimingReadback";
        state.timingReadbackBuffer = m_device->CreateBuffer(readbackDesc);
        return state.timingReadbackBuffer != nullptr;
    }

    bool RayTracedShadowPass::CreateFrameFallbackTextures(
        RayTracedShadowFrameState& state) const
    {
        if (!m_device || !state.reservation)
        {
            return false;
        }

        // These are intentionally recording-owned resources.  A persistent
        // Common-state fallback imported by multiple independently compiled
        // graphs would recreate the same cross-graph initial-state hazard as
        // an in-flight history read.
        if (!state.execution.view.velocityTarget.IsValid())
        {
            RHITextureDesc velocityDesc = RHITextureDesc::Texture2D(
                1, 1, RHIFormat::RG16_FLOAT, RHITextureUsage::ShaderResource);
            velocityDesc.debugName = "RayTracedShadowFallbackVelocity";
            state.fallbackVelocityTexture = m_device->CreateTexture(velocityDesc);
            if (!state.fallbackVelocityTexture)
            {
                return false;
            }
        }
        if (state.reservation->historyAvailable)
        {
            return true;
        }

        RHITextureDesc maskDesc = RHITextureDesc::Texture2D(
            1, 1, RHIFormat::R8_UNORM, RHITextureUsage::ShaderResource);
        maskDesc.debugName = "RayTracedShadowFallbackHistoryMask";
        state.fallbackHistoryMaskTexture = m_device->CreateTexture(maskDesc);
        RHITextureDesc depthDesc = RHITextureDesc::Texture2D(
            1, 1, RHIFormat::R32_FLOAT, RHITextureUsage::ShaderResource);
        depthDesc.debugName = "RayTracedShadowFallbackHistoryDepth";
        state.fallbackHistoryDepthTexture = m_device->CreateTexture(depthDesc);
        RHITextureDesc normalDesc = RHITextureDesc::Texture2D(
            1, 1, RHIFormat::RGBA16_FLOAT, RHITextureUsage::ShaderResource);
        normalDesc.debugName = "RayTracedShadowFallbackHistoryNormal";
        state.fallbackHistoryNormalTexture = m_device->CreateTexture(normalDesc);
        return state.fallbackHistoryMaskTexture && state.fallbackHistoryDepthTexture &&
               state.fallbackHistoryNormalTexture;
    }

    bool RayTracedShadowPass::UpdateConstants(RayTracedShadowFrameState& state) const
    {
        if (!state.constantBuffer || !state.reservation)
        {
            return false;
        }
        const ViewData& view = state.execution.view;
        const float32 width = static_cast<float32>(std::max(1u, view.viewportWidth));
        const float32 height = static_cast<float32>(std::max(1u, view.viewportHeight));
        const bool stableHistoryLight = state.reservation->historyViewValid &&
            NearlyEqual(state.reservation->previousRayDirection,
                        state.reservation->currentRayDirection, 1.0e-4f);
        const bool useHistory = state.reservation->historyAvailable &&
            state.config.rayTracedTemporalAccumulation && stableHistoryLight;
        RayTracedShadowGPUConstants constants;
        constants.inverseViewProjection = view.inverseViewMatrix * view.inverseProjectionMatrix;
        constants.previousViewProjection = state.reservation->historyViewValid
            ? state.reservation->previousViewProjection : view.viewProjectionMatrix;
        constants.lightDirectionAndTMax = Vec4(state.reservation->currentRayDirection,
            std::max(1.0f, ClampFiniteNonNegative(view.farPlane, 1000.0f)));
        constants.viewportSizeAndInvSize = Vec4(width, height, 1.0f / width, 1.0f / height);
        constants.depthAndBiasParams = Vec4(
            state.reverseZ ? 1.0f : 0.0f,
            std::max(0.001f, ClampFiniteNonNegative(state.config.normalBias, 0.02f)),
            useHistory ? 1.0f : 0.0f,
            std::min(ClampFiniteNonNegative(state.config.rayTracedTemporalBlendFactor, 0.75f), 0.95f));
        constants.historyReprojectionParams = Vec4(
            std::min(ClampFiniteNonNegative(state.config.rayTracedHistoryDepthThreshold, 0.01f), 0.1f),
            std::min(ClampFiniteNonNegative(state.config.rayTracedHistoryNormalThreshold, 0.85f), 1.0f),
            state.execution.view.velocityTarget.IsValid() ? 1.0f : 0.0f,
            std::min(ClampFiniteNonNegative(state.config.rayTracedHistoryVelocityRejectionScale, 8.0f), 64.0f));
        constants.softShadowParams = Vec4(
            std::min(ClampFiniteNonNegative(state.config.rayTracedLightAngularRadius, 0.00465f), 0.25f),
            static_cast<float32>(view.frameNumber & 0x00FFFFFFull),
            static_cast<float32>(state.stats.samplesPerPixel), 0.0f);
        constants.rayOptions = Vec4(static_cast<float32>(std::min<uint32>(state.config.rayTracedInstanceMask, 0xFFu)), 0, 0, 0);
        void* mapped = state.constantBuffer->Map();
        if (!mapped)
        {
            return false;
        }
        std::memcpy(mapped, &constants, sizeof(constants));
        if (!state.constantBuffer->CommitMappedWrite())
        {
            return false;
        }
        state.stats.constantsUploaded = true;
        state.stats.temporalAccumulated = useHistory;
        return true;
    }
} // namespace RVX
