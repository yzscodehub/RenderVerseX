#pragma once

/** @file RenderSubsystem.h @brief Dedicated render-runtime publication gateway */

#include "Core/Subsystem/EngineSubsystem.h"
#include "Render/RenderDiagnostics.h"
#include "Render/RenderRuntimeTypes.h"
#include "RenderContracts/IRenderResourceGateway.h"
#include "RenderContracts/RenderFramePacketV5.h"
#include "RenderContracts/RenderSceneUpdate.h"
#include "RHI/RHINativeSurface.h"

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
                       const NativeSurfaceDesc& surface);
        RenderFramePublishResult TryPublishFrameSet(
            std::unique_ptr<const RenderSceneUpdateBatch> sceneUpdate,
            std::unique_ptr<const RenderFramePacketV5> frameV5);
        RenderResizeResult RequestResize(const NativeSurfaceDesc& surface);
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
        NativeSurfaceDesc m_runtimeSurface{};
        RenderRuntimeResult m_preRuntimeResult{};
        RenderShutdownResult m_preShutdownResult{};
        bool m_runtimeConfigured = false;
        bool m_initializeAttempted = false;
    };
} // namespace RVX
