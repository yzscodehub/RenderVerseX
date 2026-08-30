#pragma once

/**
 * @file SampleParticleVfxDiagnostics.h
 * @brief Narrow, renderer-owned particle receipt projection for scenarios.
 */

#include "Core/Types.h"

#include <string>

namespace RVX
{
    struct SampleRenderDiagnostics;

    /**
     * @brief Presentation-bound CPU billboard evidence with no Render/RHI handles.
     *
     * This is the only particle rendering detail that a GFX17 scenario may
     * inspect. Its producer stays in SampleCLI, where the full renderer
     * snapshot is projected after a successful Present.
     */
    struct SampleParticleVfxExecutionDiagnostics
    {
        bool available = false;
        uint32 executionReceiptSchemaVersion = 0;
        bool executionReceiptStructurallyValid = false;
        bool executionReceiptPresented = false;
        bool executionReceiptSucceeded = false;
        bool mainView = false;
        bool requested = false;
        bool supported = false;
        bool enabled = false;
        bool graphPassScheduled = false;
        bool drawSubmitted = false;
        bool uploadPublished = false;
        uint32 itemCount = 0;
        uint32 cpuPacketReadyItemCount = 0;
        uint32 totalAliveParticles = 0;
        uint32 drawCallCount = 0;
        uint32 instanceCount = 0;
        uint32 fallbackCount = 0;
        uint64 uploadBytes = 0;
        uint64 resourceCreationCount = 0;
        uint64 pipelineCreationCount = 0;
        bool flipbookRequested = false;
        bool flipbookValidated = false;
        uint32 additiveBatchCount = 0;
        uint32 premultipliedBatchCount = 0;
        uint32 textureResolvedBatchCount = 0;
        uint32 qualifiedBatchReceiptCount = 0;
        uint32 distinctEffectDescriptorCount = 0;
        uint32 distinctTextureBindingCount = 0;
        uint64 effectDescriptorHash = 0;
        uint64 textureAssetId = 0;
        uint32 textureGeneration = 0;
        uint64 textureContentRevision = 0;
        uint32 batchReceiptCount = 0;
        bool batchReceiptsOverflow = false;
        bool softDepthRequested = false;
        bool softDepthRequired = false;
        bool softDepthSnapshotReady = false;
        bool softDepthLinearizationReady = false;
        bool softDepthEnabled = false;
        bool softDepthOptionalFallback = false;
        bool reactiveMaskRequested = false;
        bool reactiveMaskWriteEnabled = false;
        uint32 reactiveMaskWriteDrawCount = 0;
        bool distortionRequested = false;
        bool distortionPassScheduled = false;
        bool distortionPassExecuted = false;
        bool distortionCompositeExecuted = false;
        uint32 lightHintRequestedCount = 0;
        uint32 lightHintAdmittedCount = 0;
        uint32 lightHintDroppedCount = 0;
        uint64 recordFrameSequence = 0;
        uint64 recordSceneRevision = 0;
        uint64 recordSurfaceGeneration = 0;
        uint64 recordConfigurationRevision = 0;
        uint64 recordGraphIdentity = 0;
        uint64 recordGraphGeneration = 0;
        uint64 recordEpoch = 0;
        uint64 executionReceiptFrameSequence = 0;
        uint64 presentedFrameSequence = 0;
        bool recordIdentityMatchesExecutionReceipt = false;
        bool presentedFrameAligned = false;
        std::string diagnostic;

        [[nodiscard]] bool HasPresentationBoundExecution() const noexcept
        {
            // Schema 11 remains decodable in historical reports, but only the
            // current renderer receipt can qualify a live Sample execution.
            return available && executionReceiptSchemaVersion == 12 &&
                   executionReceiptStructurallyValid &&
                   executionReceiptPresented && executionReceiptSucceeded &&
                   mainView && requested && supported && enabled &&
                   graphPassScheduled && drawSubmitted && uploadPublished &&
                   itemCount != 0 && cpuPacketReadyItemCount != 0 &&
                   totalAliveParticles != 0 && drawCallCount != 0 &&
                   instanceCount != 0 && uploadBytes != 0 &&
                   fallbackCount == 0 && flipbookRequested && flipbookValidated &&
                   recordFrameSequence != 0 && recordSceneRevision != 0 &&
                   recordSurfaceGeneration != 0 &&
                   recordConfigurationRevision != 0 &&
                   recordGraphIdentity != 0 && recordGraphGeneration != 0 &&
                   recordEpoch != 0 &&
                   recordIdentityMatchesExecutionReceipt &&
                   recordFrameSequence == executionReceiptFrameSequence &&
                   executionReceiptFrameSequence == presentedFrameSequence &&
                   presentedFrameAligned && diagnostic.empty();
        }

        [[nodiscard]] bool HasCommonExecution() const noexcept
        {
            return HasPresentationBoundExecution() && itemCount == 1u &&
                   cpuPacketReadyItemCount == 1u && batchReceiptCount == 1u &&
                   !batchReceiptsOverflow &&
                   textureResolvedBatchCount == batchReceiptCount &&
                   qualifiedBatchReceiptCount == batchReceiptCount &&
                   distinctEffectDescriptorCount == 1u &&
                   distinctTextureBindingCount == 1u &&
                   effectDescriptorHash != 0 && textureAssetId != 0 &&
                   textureGeneration != 0 && textureContentRevision != 0;
        }

        [[nodiscard]] bool HasFireExecution() const noexcept
        {
            return HasCommonExecution() && additiveBatchCount != 0 &&
                   premultipliedBatchCount == 0 && distortionRequested &&
                   distortionPassScheduled && distortionPassExecuted &&
                   distortionCompositeExecuted && reactiveMaskRequested &&
                   reactiveMaskWriteEnabled && reactiveMaskWriteDrawCount != 0 &&
                   lightHintRequestedCount == 1u && lightHintAdmittedCount == 1u &&
                   lightHintDroppedCount == 0;
        }

        [[nodiscard]] bool HasSmokeExecution() const noexcept
        {
            return HasCommonExecution() && premultipliedBatchCount != 0 &&
                   additiveBatchCount == 0 && softDepthRequested &&
                   softDepthRequired && softDepthSnapshotReady &&
                   softDepthLinearizationReady && softDepthEnabled &&
                   !softDepthOptionalFallback && reactiveMaskRequested &&
                   reactiveMaskWriteEnabled && reactiveMaskWriteDrawCount != 0 &&
                   !distortionRequested && !distortionPassScheduled &&
                   !distortionPassExecuted && !distortionCompositeExecuted;
        }

        /** @brief Exact two-emitter fire+smoke composition used by GFX24. */
        [[nodiscard]] bool HasFireAndSmokeExecution() const noexcept
        {
            return HasPresentationBoundExecution() && itemCount == 2u &&
                   cpuPacketReadyItemCount == itemCount &&
                   instanceCount == totalAliveParticles &&
                   batchReceiptCount >= 2u && !batchReceiptsOverflow &&
                   drawCallCount == batchReceiptCount &&
                   textureResolvedBatchCount == batchReceiptCount &&
                   qualifiedBatchReceiptCount == batchReceiptCount &&
                   distinctEffectDescriptorCount == 2u &&
                   distinctTextureBindingCount == 2u &&
                   additiveBatchCount != 0 && premultipliedBatchCount != 0 &&
                   additiveBatchCount + premultipliedBatchCount ==
                       batchReceiptCount &&
                   softDepthRequested && softDepthRequired &&
                   softDepthSnapshotReady && softDepthLinearizationReady &&
                   softDepthEnabled && !softDepthOptionalFallback &&
                   reactiveMaskRequested && reactiveMaskWriteEnabled &&
                   reactiveMaskWriteDrawCount != 0 && distortionRequested &&
                   distortionPassScheduled && distortionPassExecuted &&
                   distortionCompositeExecuted &&
                   lightHintRequestedCount == 1u &&
                   lightHintAdmittedCount == 1u &&
                   lightHintDroppedCount == 0;
        }
    };

    /** @brief Access the one immutable particle receipt from Sample diagnostics. */
    [[nodiscard]] const SampleParticleVfxExecutionDiagnostics&
        GetSampleParticleVfxExecutionDiagnostics(
            const SampleRenderDiagnostics& diagnostics) noexcept;
} // namespace RVX
