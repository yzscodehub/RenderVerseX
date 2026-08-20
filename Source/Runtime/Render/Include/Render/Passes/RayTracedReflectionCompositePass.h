#pragma once

/**
 * @file RayTracedReflectionCompositePass.h
 * @brief Fullscreen composite pass for ray-traced reflections
 */

#include "Render/Passes/IRenderPass.h"


namespace RVX
{
    class PipelineCache;
    class RayTracedReflectionDenoisePass;
    class RayTracedReflectionPass;
    class ResourceViewCache;

    enum class RayTracedReflectionCompositeSource : uint8
    {
        None = 0,
        RawReflection,
        DenoisedReflection
    };

    struct RayTracedReflectionCompositePassStats
    {
        bool requested = false;
        bool supported = false;
        bool sourcePassEnabled = false;
        bool denoiseRequested = false;
        bool denoisedSourceAvailable = false;
        bool denoisedSourceUsed = false;
        bool rawSourceUsed = false;
        bool denoiseFallbackToRaw = false;
        bool reflectionHandleAvailable = false;
        bool outputDeclared = false;
        bool constantsUploaded = false;
        bool compositeRecorded = false;
        RayTracedReflectionCompositeSource source = RayTracedReflectionCompositeSource::None;
        uint32 width = 0;
        uint32 height = 0;
    };

    class RayTracedReflectionCompositePass : public IRenderPass
    {
    public:
        RayTracedReflectionCompositePass() = default;
        ~RayTracedReflectionCompositePass() override = default;

        const char* GetName() const override { return "RayTracedReflectionCompositePass"; }
        int32_t GetPriority() const override { return PassPriority::PostProcess - 100; }

        void OnAdd(IRHIDevice* device) override;
        void OnRemove() override;
        void Setup(RenderGraphBuilder& builder, const ViewData& view) override;
        void Execute(RHICommandContext& ctx, const ViewData& view) override;
        void Execute(RenderGraphPassContext& context,
                     const ViewData& view) override;

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
        void SetDenoisedReflectionSource(const RayTracedReflectionDenoisePass* denoisePass)
        {
            m_denoisePass = denoisePass;
        }

        const RayTracedReflectionCompositePassStats& GetStats() const { return m_stats; }

    private:
        IRHIDevice* m_device = nullptr;
        PipelineCache* m_pipelineCache = nullptr;
        ResourceViewCache* m_viewCache = nullptr;
        const RayTracedReflectionPass* m_reflectionPass = nullptr;
        const RayTracedReflectionDenoisePass* m_denoisePass = nullptr;
        bool m_enabled = false;
        mutable std::string m_unsupportedReason = "Ray traced reflection composite has not been requested";

        RGTextureHandle m_reflectionReadHandle;
        RGTextureHandle m_colorTargetHandle;
        RGTextureViewHandle m_reflectionViewHandle;
        RGTextureViewHandle m_colorTargetViewHandle;
        RHIFormat m_outputFormat = RHIFormat::Unknown;
        RHIBufferRef m_constantBuffer;
        RHISamplerRef m_sampler;
        RayTracedReflectionCompositePassStats m_stats;

        bool EnsureRuntimeResources();
        bool UpdateConstants();
    };

} // namespace RVX
