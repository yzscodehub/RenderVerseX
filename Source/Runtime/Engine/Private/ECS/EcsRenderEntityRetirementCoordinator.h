#pragma once

/**
 * @file EcsRenderEntityRetirementCoordinator.h
 * @brief Generic Render proof and cleanup acknowledgement for direct Scene ECS entities.
 */

#include "ResourceSceneAdapters/ECS/EcsSceneAssetLoadCoordinator.h"
#include "Scene/ECS/SceneEcsRuntime.h"

#include <memory>
#include <string>

namespace RVX
{
    /** @brief Fail-closed diagnostics for generic direct-entity Render retirement. */
    struct EcsRenderEntityRetirementDiagnostics
    {
        uint64 observedCleanupRecordCount = 0;
        uint64 authoritativeRebuildCount = 0;
        uint64 cleanupContinuityLossCount = 0;
        uint64 specializedExclusionCount = 0;
        uint64 staleCleanupRecordCount = 0;
        uint64 retirementAdmissionFailureCount = 0;
        uint64 retirementQueryFailureCount = 0;
        uint64 sceneAcknowledgementFailureCount = 0;
        uint64 proofAcknowledgementFailureCount = 0;
        uint64 neverPublishedCount = 0;
        uint64 deviceLostCount = 0;
        uint32 outstandingRetirementCount = 0;
        bool processorRegistered = false;
        bool shutdownRequested = false;
        bool ownerReleasedWithOutstandingProof = false;
        std::string lastDiagnostic;
    };

    /**
     * @brief Engine-private EndFrameCleanup owner for direct ECS Render retirement.
     *
     * Resource-owned instances carry SceneECS::SpecializedRenderRetirement and
     * are intentionally excluded. Every remaining Render cleanup record is
     * admitted as an exact one-entity proof request before this coordinator
     * acknowledges the Scene Render cleanup domain.
     */
    class EcsRenderEntityRetirementCoordinator final
    {
    public:
        EcsRenderEntityRetirementCoordinator(
            SceneECS::SceneEcsRuntime& runtime,
            ResourceSceneAdapters::IEcsSceneAssetRetirementProofGateway& proofGateway);
        ~EcsRenderEntityRetirementCoordinator() noexcept;

        EcsRenderEntityRetirementCoordinator(const EcsRenderEntityRetirementCoordinator&) = delete;
        EcsRenderEntityRetirementCoordinator& operator=(
            const EcsRenderEntityRetirementCoordinator&) = delete;

        /**
         * @brief Start host shutdown validation without releasing any proof token.
         *
         * The host must continue owner-thread Scene ticks until this returns true.
         * A false result deliberately retains all exact proof evidence.
         */
        [[nodiscard]] bool PrepareForShutdown() noexcept;
        [[nodiscard]] bool HasOutstandingRetirements() const noexcept;
        [[nodiscard]] EcsRenderEntityRetirementDiagnostics GetDiagnostics() const;

    private:
        struct State;
        std::shared_ptr<State> m_state;
    };
} // namespace RVX
