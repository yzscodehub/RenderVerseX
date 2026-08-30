#pragma once

/** @file RenderSubsystem.h @brief Dedicated render-runtime publication gateway */

#include "Core/Subsystem/EngineSubsystem.h"
#include "Render/RenderDiagnostics.h"
#include "Render/RenderRuntimeTypes.h"
#include "RenderContracts/IRenderResourceGateway.h"
#include "RenderContracts/RenderFramePacketV5.h"
#include "RenderContracts/RenderSceneUpdate.h"
#include "HAL/Window/NativeSurface.h"
#include "ShaderContracts/ShaderProgramProvider.h"

#include <memory>

namespace RVX
{
    class RenderThreadRuntime;

    /**
     * @brief Engine-owned front door to the dedicated render runtime.
     *
     * Update-side callers publish immutable values and never receive live RHI
     * or renderer objects. Configure() is required before Initialize().
     */
    class RenderSubsystem final : public EngineSubsystem,
                                  public IRenderResourceGateway
    {
    public:
        RenderSubsystem();
        ~RenderSubsystem() override;

        const char* GetName() const override { return "RenderSubsystem"; }
        bool ShouldTick() const override { return false; }

        void Initialize() override;
        void Deinitialize() override;

        /** @brief Configure the dedicated runtime before Initialize(). */
        void Configure(const RenderRuntimeConfig& config,
                       const HAL::NativeSurfaceDesc& surface,
                       ShaderProgramProviderFactory shaderProgramProviderFactory);
        RenderFramePublishResult TryPublishFrameSet(
            std::unique_ptr<const RenderSceneUpdateBatch> sceneUpdate,
            std::unique_ptr<const RenderFramePacketV5> frameV5);
        RenderResizeResult RequestResize(const HAL::NativeSurfaceDesc& surface);
        [[nodiscard]] RenderDiagnosticsSnapshot
            GetDiagnosticsSnapshot() const;
        [[nodiscard]] RenderRuntimeResult GetLastRuntimeResult() const;
        [[nodiscard]] RenderShutdownResult GetLastShutdownResult() const;
        /** @brief Seal publication and enter the terminal GPU-completion drain. */
        [[nodiscard]] bool RequestCompletionPump() noexcept;
        /** @brief Wake one completion poll without sealing publication. */
        [[nodiscard]] bool RequestCompletionPoll() noexcept;
        /**
         * @brief Arm one post-fence GPUScene culling qualification capture.
         *
         * The request is accepted at most once for this runtime instance and
         * is consumed on the Render owner before its next frame is recorded.
         */
        [[nodiscard]] bool RequestGPUSceneCullingQualificationCapture() noexcept;
        /** @brief Arm one post-fence Direct Opaque raster-input readback. */
        [[nodiscard]] bool RequestDirectOpaqueRasterReadbackQualificationCapture()
            noexcept;
        /** @brief Request one complete same-frame RenderQuality attachment bundle. */
        [[nodiscard]] bool RequestQualificationCaptureBundle() noexcept;
        /** @brief Request the fixed 60-warmup + 300-sample performance run. */
        [[nodiscard]] bool RequestRenderQualityPerformanceSession() noexcept;
        /**
         * @brief Arm one post-fence physical-exposure qualification probe.
         *
         * The request is accepted at most once for this runtime instance and
         * is consumed on the Render owner before its next frame is recorded.
         * Completion remains asynchronous and is published only after the
         * corresponding submission fence has completed.
         */
        [[nodiscard]] bool RequestExposureQualificationProbe() noexcept;
        /** @brief Arm one physical GTAO visibility qualification probe. */
        [[nodiscard]] bool RequestAmbientOcclusionQualificationProbe() noexcept;
        /** @brief Arm one Toon lighting and screen-space outline qualification probe. */
        [[nodiscard]] bool RequestToonOutlineQualificationProbe() noexcept;
        /** @brief Arm one exact Hi-Z reflection route-mask qualification probe. */
        [[nodiscard]] bool RequestHiZReflectionQualificationProbe() noexcept;
        /**
         * @brief Read the one-shot Render-owner admission result for the Toon probe.
         *
         * A pending result has not yet reached the Render owner.  Accepted and
         * rejected are terminal for this runtime instance.
         */
        [[nodiscard]] RenderToonOutlineQualificationRequestResult
        GetToonOutlineQualificationProbeRequestResult() const noexcept;
        /** @brief Read exact-frame Render-owner admission for the Hi-Z probe. */
        [[nodiscard]] RenderHiZReflectionQualificationRequestResult
        GetHiZReflectionQualificationProbeRequestResult() const noexcept;

        RenderResourceReserveResult ReserveResource(
            AssetId assetId,
            RenderResourceKind kind) noexcept override;
        RenderUploadEnqueueResult TryEnqueueUpload(
            const ResourceUploadRequestRef& request) noexcept override;
        RenderReleaseResult RequestRelease(
            RenderResourceHandle handle) noexcept override;
        [[nodiscard]] RenderResourceStatus QueryResourceStatus(
            RenderResourceHandle handle) const noexcept override;

        [[nodiscard]] bool IsReady() const;

    private:
        std::unique_ptr<RenderThreadRuntime> m_runtime;
        RenderRuntimeConfig m_runtimeConfig{};
        HAL::NativeSurfaceDesc m_runtimeSurface{};
        RenderRuntimeResult m_preRuntimeResult{};
        RenderShutdownResult m_preShutdownResult{};
        bool m_runtimeConfigured = false;
        bool m_initializeAttempted = false;
    };
} // namespace RVX
