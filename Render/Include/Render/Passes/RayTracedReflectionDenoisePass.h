#pragma once

/**
 * @file RayTracedReflectionDenoisePass.h
 * @brief Spatial denoise pass for ray-traced reflections
 */

#include "Render/Passes/IRenderPass.h"

#include <deque>

namespace RVX
{
    class PipelineCache;
    class RayTracedReflectionPass;
    class ResourceViewCache;

    struct RayTracedReflectionDenoisePassStats
    {
        bool requested = false;
        bool supported = false;
        bool sourcePassEnabled = false;
        bool reflectionHandleAvailable = false;
        bool depthAvailable = false;
        bool normalGuideAvailable = false;
        bool outputDeclared = false;
        bool constantsUploaded = false;
        bool denoiseRecorded = false;
        uint32 radius = 0;
        float depthSigma = 0.0f;
        float normalThreshold = 0.0f;
        float confidencePower = 0.0f;
        float centerWeight = 0.0f;
        float lowConfidenceDepthScale = 0.0f;
        uint32 kernelTapCount = 0;
        uint64 dispatchPixelCount = 0;
        uint64 estimatedTapCount = 0;
        uint32 width = 0;
        uint32 height = 0;
    };

    struct RayTracedReflectionDenoisePassConfig
    {
        uint32 radius = 1;
        float depthSigma = 0.01f;
        float normalThreshold = 0.85f;
        float confidencePower = 1.0f;
        float centerWeight = 1.0f;
        float lowConfidenceDepthScale = 4.0f;
    };

    class RayTracedReflectionDenoisePass : public IRenderPass
    {
    public:
        RayTracedReflectionDenoisePass() = default;
        ~RayTracedReflectionDenoisePass() override = default;

        const char* GetName() const override { return "RayTracedReflectionDenoisePass"; }
        int32_t GetPriority() const override { return PassPriority::PostProcess - 125; }

        void OnAdd(IRHIDevice* device) override;
        void OnRemove() override;
        void Setup(RenderGraphBuilder& builder, const ViewData& view) override;
        void Execute(RHICommandContext& ctx, const ViewData& view) override;

        void SetEnabled(bool enabled) { m_enabled = enabled; }
        bool IsRequestedEnabled() const override { return m_enabled; }
        bool IsSupported() const override;
        const std::string& GetUnsupportedReason() const override { return m_unsupportedReason; }
        bool IsEnabled() const override { return IsRequestedEnabled() && IsSupported(); }

        void SetResources(PipelineCache* pipelineCache, ResourceViewCache* viewCache);
        void SetReflectionSource(const RayTracedReflectionPass* reflectionPass)
        {
            m_reflectionPass = reflectionPass;
        }
        void SetConfig(const RayTracedReflectionDenoisePassConfig& config) { m_config = config; }

        RGTextureHandle GetDenoisedReflectionHandle() const { return m_denoisedReflectionHandle; }
        const RayTracedReflectionDenoisePassStats& GetStats() const { return m_stats; }

    private:
        IRHIDevice* m_device = nullptr;
        PipelineCache* m_pipelineCache = nullptr;
        ResourceViewCache* m_viewCache = nullptr;
        const RayTracedReflectionPass* m_reflectionPass = nullptr;
        RayTracedReflectionDenoisePassConfig m_config;
        bool m_enabled = false;
        mutable std::string m_unsupportedReason = "Ray traced reflection denoise has not been requested";

        RGTextureHandle m_reflectionReadHandle;
        RGTextureHandle m_depthReadHandle;
        RGTextureHandle m_normalGuideReadHandle;
        RGTextureHandle m_denoisedReflectionHandle;
        RHIBufferRef m_constantBuffer;
        std::deque<RHIDescriptorSetRef> m_retainedDescriptorSets;
        RayTracedReflectionDenoisePassStats m_stats;

        bool EnsureRuntimeResources();
        bool UpdateConstants(const ViewData& view, uint32 outputWidth, uint32 outputHeight);
    };

} // namespace RVX
