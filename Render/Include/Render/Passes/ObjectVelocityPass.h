#pragma once

/**
 * @file ObjectVelocityPass.h
 * @brief Opaque and alpha-masked object motion-vector pass for temporal reprojection
 */

#include "Render/Passes/IRenderPass.h"
#include "Render/Renderer/RenderDrawItem.h"

#include <vector>

namespace RVX
{
    class RenderResourceRegistry;
    class GPUResourceManager;
    class MaterialSystem;
    class PipelineCache;
    class RenderScene;
    class ResourceViewCache;

    struct ObjectVelocityPassStats
    {
        bool requested = false;
        bool supported = false;
        bool velocityTargetAvailable = false;
        bool depthAvailable = false;
        bool previousViewProjectionAvailable = false;
        bool drawItemsAvailable = false;
        bool outputDeclared = false;
        bool velocityRecorded = false;
        uint32 width = 0;
        uint32 height = 0;
        uint32 drawItemCount = 0;
        uint32 opaqueDrawItemCount = 0;
        uint32 maskedDrawItemCount = 0;
        uint32 objectsWithHistory = 0;
        uint32 drawCount = 0;
        uint32 maskedDrawCount = 0;
        uint32 skippedNoHistoryCount = 0;
        uint32 skippedMissingResourceCount = 0;
        uint32 skippedMissingUVCount = 0;
        uint32 skippedMaterialBindingCount = 0;
        RHIFormat outputFormat = RHIFormat::Unknown;
    };

    class ObjectVelocityPass : public IRenderPass
    {
    public:
        ObjectVelocityPass() = default;
        ~ObjectVelocityPass() override = default;

        const char* GetName() const override { return "ObjectVelocityPass"; }
        int32_t GetPriority() const override { return PassPriority::PostProcess - 250; }

        void OnAdd(IRHIDevice* device) override;
        void OnRemove() override;
        void Setup(RenderGraphBuilder& builder, const ViewData& view) override;
        void Execute(RHICommandContext& ctx, const ViewData& view) override;

        void SetResources(GPUResourceManager* gpuResources,
                          PipelineCache* pipelineCache,
                          ResourceViewCache* viewCache,
                          MaterialSystem* materialSystem);
        void SetResourceRegistry(const RenderResourceRegistry* registry)
        {
            m_resourceRegistry = registry;
        }
        void SetRenderScene(const RenderScene* scene,
                            const std::vector<RenderDrawItem>* opaqueDrawItems,
                            const std::vector<RenderDrawItem>* maskedDrawItems);
        void SetEnabled(bool enabled) { m_enabled = enabled; }
        bool IsRequestedEnabled() const override { return m_enabled; }
        bool IsSupported() const override;
        const std::string& GetUnsupportedReason() const override { return m_unsupportedReason; }
        bool IsEnabled() const override { return IsRequestedEnabled() && IsSupported(); }
        const ObjectVelocityPassStats& GetStats() const { return m_stats; }

    private:
        IRHIDevice* m_device = nullptr;
        GPUResourceManager* m_gpuResources = nullptr;
        const RenderResourceRegistry* m_resourceRegistry = nullptr;
        PipelineCache* m_pipelineCache = nullptr;
        ResourceViewCache* m_viewCache = nullptr;
        MaterialSystem* m_materialSystem = nullptr;
        const RenderScene* m_renderScene = nullptr;
        const std::vector<RenderDrawItem>* m_opaqueDrawItems = nullptr;
        const std::vector<RenderDrawItem>* m_maskedDrawItems = nullptr;
        bool m_enabled = false;
        mutable std::string m_unsupportedReason = "Object velocity pass has not been requested";

        RGTextureHandle m_velocityWriteHandle;
        RGTextureHandle m_depthReadHandle;
        ObjectVelocityPassStats m_stats;
    };

} // namespace RVX
