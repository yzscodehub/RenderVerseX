#pragma once

/**
 * @file RenderRuntimeDiagnostics.h
 * @brief Update-owned, value-only render-runtime observation.
 */

#include "Core/Types.h"
#include "RenderContracts/RenderFrameTypes.h"

namespace RVX
{
    /**
     * @brief Copyable observation of Engine's update-owned render publication state.
     *
     * Values are available only while Engine has a live, configured render
     * composition. Required scene values advance only after Render accepts or
     * replaces a frame publication. Accepted extraction diagnostics retain
     * accepted reliable-update evidence independently of frame mailbox lifetime.
     */
    struct EngineRenderRuntimeDiagnostics
    {
        bool available = false;
        uint64 requiredSceneFrameSequence = 0;
        uint64 requiredSceneRevision = 0;
        uint64 temporalEpoch = 0;
        uint64 cameraIdentity = 0;
        uint64 cameraCutRevision = 0;
        uint64 temporalResetCount = 0;
        AcceptedExtractionDiagnosticsSnapshot acceptedExtractionDiagnostics{};
    };
} // namespace RVX
