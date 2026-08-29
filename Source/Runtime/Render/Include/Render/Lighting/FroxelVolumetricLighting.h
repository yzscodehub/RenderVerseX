#pragma once

/**
 * @file FroxelVolumetricLighting.h
 * @brief Renderer-owned four-stage froxel volumetric lighting primitive.
 *
 * This primitive consumes an immutable VolumetricLightingFramePlan and turns
 * it into real GPU work: injection, front-to-back integration, temporal
 * resolve into a persistent candidate, and opaque HDR composition.  It is
 * intentionally a graph primitive rather than a post-process stack entry;
 * special-surface consumers receive the exact integrated Texture3D through
 * the typed blackboard contract instead of performing a second ray march.
 */

#include "Core/Types.h"
#include "Render/Features/RenderFeatureGraphBuilder.h"
#include "Render/Lighting/VolumetricLightingFramePlan.h"
#include "Render/PipelineCache.h"
#include "Render/Renderer/ViewData.h"

#include <array>
#include <memory>
#include <string>

namespace RVX
{
    /**
     * @brief Maps device depth to the froxel field's near=0, far=1 axis.
     *
     * The volume is constructed front-to-back, independently of the active
     * raster depth convention.  Every raster and compute consumer therefore
     * uses this conversion before addressing its Texture3D Z coordinate.
     */
    [[nodiscard]] constexpr float32 CanonicalizeFroxelDeviceDepth(
        float32 deviceDepth,
        bool reverseZ) noexcept
    {
        const float32 saturatedDepth = deviceDepth < 0.0f
            ? 0.0f
            : (deviceDepth > 1.0f ? 1.0f : deviceDepth);
        return reverseZ ? 1.0f - saturatedDepth : saturatedDepth;
    }

    /** Exact graph resources consumed by one sealed froxel-plan execution. */
    struct FroxelVolumetricLightingInputs
    {
        /** HDR opaque input. The non-identity composite always writes another texture. */
        RGTextureHandle sourceSceneColor{};
        /** Same-view device depth used by the opaque composite. */
        RGTextureHandle deviceDepth{};
        /** Must exactly equal the sealed frame plan identity. */
        uint64 lightingPlanIdentity = 0;
        /** Byte-frozen buffers for the exact selected lighting-plan realization. */
        FrozenFrameLightResources frozenLightResources{};
        RGBufferHandle lightConstants{};
        RGBufferHandle pointLights{};
        RGBufferHandle spotLights{};
        RGBufferHandle clusterConstants{};
        RGBufferHandle clusterData{};
        RGBufferHandle clusterLightIndices{};
        bool directionalShadowEnabled = false;
        RGTextureHandle directionalShadowMap{};
        bool localShadowEnabled = false;
        RGTextureHandle localShadowAtlas{};
        /** Candidate key for the frame being recorded, never the prior source key. */
        RenderViewHistoryIdentity historyCandidate{};
        /** True only when composition offered a compatible prior-presented source. */
        bool historyAvailable = false;

        [[nodiscard]] bool HasCompleteLightingResources() const noexcept
        {
            return lightingPlanIdentity != 0 && frozenLightResources.IsComplete() &&
                   lightConstants.IsValid() && pointLights.IsValid() &&
                   spotLights.IsValid() && clusterConstants.IsValid() &&
                   clusterData.IsValid() && clusterLightIndices.IsValid() &&
                   (!directionalShadowEnabled || directionalShadowMap.IsValid()) &&
                   (!localShadowEnabled || localShadowAtlas.IsValid());
        }
    };

    /** Metadata for the graph-local result and persistent history candidate. */
    struct FroxelVolumetricLightingOutput
    {
        RGTextureHandle sourceSceneColor{};
        RGTextureHandle resolvedSceneColor{};
        /** RGBA16F Texture3D: in-scattering in RGB and transmittance in A. */
        RGTextureHandle integratedScatteringTransmittance{};
        FroxelGridMetadata froxelGrid{};
        RenderViewHistoryIdentity historyCandidate{};
        uint64 volumetricPlanIdentity = 0;
        uint64 lightingPlanIdentity = 0;
        bool identityRoute = true;
        bool temporalHistoryUsed = false;

        [[nodiscard]] bool IsIdentityOutput() const noexcept
        {
            return identityRoute && sourceSceneColor.IsValid() &&
                   resolvedSceneColor.IsValid() &&
                   sourceSceneColor.index == resolvedSceneColor.index &&
                   !integratedScatteringTransmittance.IsValid();
        }
    };

    /** Receipt-oriented facts from the latest graph candidate. */
    struct FroxelVolumetricLightingStats
    {
        bool requested = false;
        bool pipelineReady = false;
        bool inputsReady = false;
        bool identityRoute = true;
        bool injectionRecorded = false;
        bool integrationRecorded = false;
        bool temporalResolveRecorded = false;
        bool opaqueCompositeRecorded = false;
        bool temporalHistorySampled = false;
        bool historyCandidatePrepared = false;
        bool historySubmissionAccepted = false;
        bool historyCommitted = false;
        bool historyAbandoned = false;
        uint32 injectionDispatchCount = 0;
        uint32 integrationDispatchCount = 0;
        uint32 temporalDispatchCount = 0;
        uint32 compositeDispatchCount = 0;
        uint64 volumetricPlanIdentity = 0;
        uint64 lightingPlanIdentity = 0;
        RenderViewHistoryIdentity historyCandidate{};
        std::string failureDiagnostic{};
    };

    struct FroxelVolumetricLightingExecutionState;

    /**
     * @brief Real GPU froxel primitive with presentation-gated ping-pong history.
     *
     * The persistent candidate texture is never promoted on callback entry.
     * The feature adapter must call FinalizeGraphExecution(), report the exact
     * GPU submission, and then commit only after the same view has presented.
     */
    class FroxelVolumetricLighting final
    {
    public:
        FroxelVolumetricLighting() = default;
        ~FroxelVolumetricLighting();

        FroxelVolumetricLighting(const FroxelVolumetricLighting&) = delete;
        FroxelVolumetricLighting& operator=(const FroxelVolumetricLighting&) =
            delete;

        void SetPipelineCache(PipelineCache* pipelineCache) noexcept;

        [[nodiscard]] FroxelVolumetricLightingOutput AddToGraph(
            RenderFeatureGraphBuilder& featureBuilder,
            const ViewData& view,
            const VolumetricLightingFramePlan& plan,
            const FroxelVolumetricLightingInputs& inputs);

        /** Accept or discard callback execution evidence for the one candidate. */
        void FinalizeGraphExecution(bool executionAccepted) noexcept;

        [[nodiscard]] bool CanCommitHistoryAfterPresent(
            const RenderViewHistoryIdentity& candidateIdentity,
            uint64 volumetricPlanIdentity) const noexcept;
        [[nodiscard]] bool NotifyHistorySubmission(
            const RenderViewHistoryIdentity& candidateIdentity,
            uint64 volumetricPlanIdentity) noexcept;
        [[nodiscard]] bool CommitHistoryAfterPresent(
            const RenderViewHistoryIdentity& candidateIdentity,
            uint64 volumetricPlanIdentity) noexcept;
        [[nodiscard]] bool AbandonPendingHistory(
            const RenderViewHistoryIdentity& candidateIdentity,
            uint64 volumetricPlanIdentity) noexcept;
        void ResetHistory() noexcept;
        void Shutdown() noexcept;

        [[nodiscard]] bool HasValidHistory() const noexcept
        {
            return m_historyValid;
        }
        /**
         * @brief True only when the requested global source is this
         * primitive's exact presentation-qualified history.
         */
        [[nodiscard]] bool HasCompatibleCommittedHistory(
            const RenderViewHistoryIdentity& identity) const noexcept
        {
            return m_historyValid && identity.IsValid() &&
                   m_committedHistoryIdentity == identity;
        }
        [[nodiscard]] const FroxelVolumetricLightingStats& GetLastStats() const
            noexcept
        {
            return m_lastStats;
        }
        [[nodiscard]] const std::string& GetUnsupportedReason() const noexcept
        {
            return m_unsupportedReason;
        }

    private:
        [[nodiscard]] bool EnsurePipelineResources();
        [[nodiscard]] bool EnsureHistoryResources(const FroxelGridMetadata& grid);
        [[nodiscard]] bool IsCandidateIdentityCompatible(
            const VolumetricLightingFramePlan& plan,
            const RenderViewHistoryIdentity& candidate) const noexcept;
        [[nodiscard]] bool IsCommittedHistoryCompatible(
            const VolumetricLightingFramePlan& plan,
            const FroxelVolumetricLightingInputs& inputs) const noexcept;
        void RecordFailure(const char* diagnostic) noexcept;

        PipelineCache* m_pipelineCache = nullptr;
        IRHIDevice* m_device = nullptr;
        RHITextureRef m_historyTextures[2];
        /** Array-shaped neutral shadow source for disabled shadow branches. */
        RHITextureRef m_neutralShadowTexture;
        RHISamplerRef m_linearClampSampler;
        uint32 m_historyWidth = 0;
        uint32 m_historyHeight = 0;
        uint32 m_historyDepth = 0;
        uint32 m_committedHistoryIndex = 0;
        uint32 m_pendingHistoryIndex = 0;
        bool m_historyValid = false;
        bool m_historyResourceStatesInitialized = false;
        bool m_hasPendingHistory = false;
        bool m_pendingHistorySubmissionAccepted = false;
        RenderViewHistoryIdentity m_committedHistoryIdentity{};
        RenderViewHistoryIdentity m_pendingHistoryIdentity{};
        uint64 m_committedVolumetricPlanIdentity = 0;
        uint64 m_pendingVolumetricPlanIdentity = 0;
        std::shared_ptr<FroxelVolumetricLightingExecutionState>
            m_pendingExecutionState{};
        FroxelVolumetricLightingStats m_lastStats{};
        std::string m_unsupportedReason{
            "Froxel volumetric lighting has not created native compute pipelines"};
    };
} // namespace RVX
