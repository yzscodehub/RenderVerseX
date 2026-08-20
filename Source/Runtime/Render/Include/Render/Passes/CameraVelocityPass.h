#pragma once

/**
 * @file CameraVelocityPass.h
 * @brief Camera/depth motion-vector pass for temporal reprojection
 */

#include "Render/Passes/IRenderPass.h"


namespace RVX
{
    class PipelineCache;
    class ResourceViewCache;

    struct CameraVelocityPassStats
    {
        bool requested = false;
        bool supported = false;
        bool depthAvailable = false;
        bool previousViewProjectionAvailable = false;
        bool velocityTargetAvailable = false;
        bool outputDeclared = false;
        bool constantsUploaded = false;
        bool velocityRecorded = false;
        uint32 width = 0;
        uint32 height = 0;
        RHIFormat outputFormat = RHIFormat::Unknown;
    };

    class CameraVelocityPass : public IRenderPass
    {
    public:
        CameraVelocityPass() = default;
        ~CameraVelocityPass() override = default;

        const char* GetName() const override { return "CameraVelocityPass"; }
        int32_t GetPriority() const override { return PassPriority::PostProcess - 300; }

        void OnAdd(IRHIDevice* device) override;
        void OnRemove() override;
        void Setup(RenderGraphBuilder& builder, const ViewData& view) override;
        void Execute(RHICommandContext& ctx, const ViewData& view) override;
        void Execute(RenderGraphPassContext& context,
                     const ViewData& view) override;

        void SetResources(PipelineCache* pipelineCache, ResourceViewCache* viewCache);
        void SetEnabled(bool enabled) { m_enabled = enabled; }
        bool IsRequestedEnabled() const override { return m_enabled; }
        bool IsSupported() const override;
        const std::string& GetUnsupportedReason() const override { return m_unsupportedReason; }
        bool IsEnabled() const override { return IsRequestedEnabled() && IsSupported(); }
        const CameraVelocityPassStats& GetStats() const { return m_stats; }

    private:
        IRHIDevice* m_device = nullptr;
        PipelineCache* m_pipelineCache = nullptr;
        ResourceViewCache* m_viewCache = nullptr;
        bool m_enabled = false;
        mutable std::string m_unsupportedReason = "Camera velocity pass has not been requested";

        RGTextureHandle m_depthReadHandle;
        RGTextureHandle m_velocityWriteHandle;
        RGTextureViewHandle m_depthViewHandle;
        RGTextureViewHandle m_velocityViewHandle;
        RHIBufferRef m_constantBuffer;
        RHISamplerRef m_sampler;
        CameraVelocityPassStats m_stats;

        bool EnsureRuntimeResources();
        bool UpdateConstants(const ViewData& view, uint32 width, uint32 height);
    };

} // namespace RVX
