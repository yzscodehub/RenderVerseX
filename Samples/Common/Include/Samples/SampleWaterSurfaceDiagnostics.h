#pragma once

/**
 * @file SampleWaterSurfaceDiagnostics.h
 * @brief Narrow, renderer-owned water execution projection for scenarios.
 */

#include "Core/Types.h"

#include <string>

namespace RVX
{
    struct SampleRenderDiagnostics;

    /**
     * @brief Presentation-bound Gerstner water evidence without Render/RHI handles.
     *
     * Sample scenarios may observe this immutable projection, but cannot create
     * or infer it from their authored Water ECS values.  It binds the actual
     * presented renderer receipt to the water grid, snapshot inputs, shading
     * evaluations, reactive-mask write, and frozen frame identity.
     */
    struct SampleWaterSurfaceExecutionDiagnostics
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
        bool opaqueSceneColorPyramidReady = false;
        bool depthSnapshotReady = false;
        bool depthLinearizationReady = false;
        bool layeredNormalsEvaluated = false;
        bool fresnelEvaluated = false;
        bool thicknessEvaluated = false;
        bool beerLambertEvaluated = false;
        bool foamEvaluated = false;
        bool reactiveMaskWriteEnabled = false;
        uint32 itemCount = 0;
        uint32 drawCallCount = 0;
        uint32 waveCount = 0;
        uint32 fallbackCount = 0;
        uint32 waitIdleCallCount = 0;
        uint32 frameSlot = 0;
        uint32 reactiveMaskWriteDrawCount = 0;
        uint32 surfaceReceiptCount = 0;
        bool surfaceReceiptsOverflow = false;
        uint64 uploadBytes = 0;
        uint64 resourceCreationCount = 0;
        uint64 pipelineCreationCount = 0;
        uint64 gridVertexCount = 0;
        uint64 gridIndexCount = 0;
        uint64 componentId = 0;
        uint64 contentHash = 0;
        uint64 waveContentHash = 0;
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

        [[nodiscard]] bool HasWaterExecution() const noexcept
        {
            // Schema 11 remains decodable in historical reports, but only the
            // current renderer receipt can qualify a live Sample execution.
            return available && executionReceiptSchemaVersion == 13 &&
                   executionReceiptStructurallyValid &&
                   executionReceiptPresented && executionReceiptSucceeded &&
                   mainView && requested && supported && enabled &&
                   graphPassScheduled && drawSubmitted && uploadPublished &&
                   opaqueSceneColorPyramidReady && depthSnapshotReady &&
                   depthLinearizationReady && layeredNormalsEvaluated &&
                   fresnelEvaluated && thicknessEvaluated &&
                   beerLambertEvaluated && foamEvaluated &&
                   reactiveMaskWriteEnabled && itemCount == 1u &&
                   drawCallCount == 1u && waveCount != 0 && fallbackCount == 0 &&
                   waitIdleCallCount == 0 && reactiveMaskWriteDrawCount == 1u &&
                   surfaceReceiptCount == 1u && !surfaceReceiptsOverflow &&
                   uploadBytes != 0 && resourceCreationCount != 0 &&
                   pipelineCreationCount != 0 && gridVertexCount != 0 &&
                   gridIndexCount != 0 && componentId != 0 && contentHash != 0 &&
                   waveContentHash != 0 && recordFrameSequence != 0 &&
                   recordSceneRevision != 0 && recordSurfaceGeneration != 0 &&
                   recordConfigurationRevision != 0 && recordGraphIdentity != 0 &&
                   recordGraphGeneration != 0 && recordEpoch != 0 &&
                   recordIdentityMatchesExecutionReceipt &&
                   recordFrameSequence == executionReceiptFrameSequence &&
                   executionReceiptFrameSequence == presentedFrameSequence &&
                   presentedFrameAligned && diagnostic.empty();
        }
    };

    /** @brief Access the one immutable water receipt from Sample diagnostics. */
    [[nodiscard]] const SampleWaterSurfaceExecutionDiagnostics&
        GetSampleWaterSurfaceExecutionDiagnostics(
            const SampleRenderDiagnostics& diagnostics) noexcept;
} // namespace RVX
