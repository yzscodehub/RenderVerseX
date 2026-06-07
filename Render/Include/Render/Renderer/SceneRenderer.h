#pragma once

/**
 * @file SceneRenderer.h
 * @brief Scene renderer - orchestrates render pass execution
 */

#include "Render/Renderer/ViewData.h"
#include "Render/Renderer/RenderScene.h"
#include "Render/Graph/RenderGraph.h"
#include "Render/Graph/TransientResourcePool.h"
#include "Render/Graph/ResourceViewCache.h"
#include "Render/Context/RenderContext.h"
#include "Render/GPUResourceManager.h"
#include "Render/Material/MaterialSystem.h"
#include "Render/Passes/IRenderPass.h"
#include "Render/PipelineCache.h"
#include "Render/PostProcess/PostProcessStack.h"
#include "Render/Renderer/RenderDrawItem.h"
#include "Render/Renderer/RenderProxy.h"

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace RVX
{
    inline constexpr RHIFormat RVX_SCENE_COLOR_HDR_FORMAT = RHIFormat::RGBA16_FLOAT;

    class World;
    class Camera;
    class BloomPass;
    class IRenderPass;
    class DepthPrepass;
    class OpaquePass;
    class RenderPassRegistry;
    class RenderProxySceneBridge;
    class ShadowPass;
    class SkyboxPass;
    class ToneMappingPass;
    class TransparentPass;

    enum class SceneRenderCollectionPath : uint8
    {
        None = 0,
        Proxy,
        LegacyFallback
    };

    struct SceneRenderCollectionStats
    {
        SceneRenderCollectionPath lastPath = SceneRenderCollectionPath::None;
        uint64 proxyFrameCount = 0;
        uint64 legacyFallbackFrameCount = 0;
        size_t lastProxyPrimitiveCount = 0;
        size_t lastProxyLightCount = 0;
        uint64 lastFallbackOwnerId = 0;
        std::string lastFallbackReason;
    };

    struct SceneRenderPassChainStats
    {
        uint64 frameCount = 0;
        size_t registeredPassCount = 0;
        size_t graphPassCount = 0;
        size_t skippedDisabledPassCount = 0;
        size_t skippedUnsupportedPassCount = 0;
        std::vector<RenderPassStatus> passStatuses;
    };

    struct SceneColorFormatPolicy
    {
        RHIFormat requestedSceneColorFormat = RVX_SCENE_COLOR_HDR_FORMAT;
        RHIFormat actualSceneColorFormat = RHIFormat::Unknown;
        RHIFormat backBufferFormat = RHIFormat::Unknown;
        RHIFormat toneMappingOutputFormat = RHIFormat::Unknown;
        bool hdrSceneColorEnabled = false;
        std::string hdrFallbackReason;
    };

    struct SceneRenderPostProcessStats
    {
        uint64 frameCount = 0;
        bool sceneColorStagingUsed = false;
        bool directToBackBuffer = true;
        uint32 sceneColorWidth = 0;
        uint32 sceneColorHeight = 0;
        RHIFormat sceneColorFormat = RHIFormat::Unknown;
        RHIFormat requestedSceneColorFormat = RVX_SCENE_COLOR_HDR_FORMAT;
        RHIFormat actualSceneColorFormat = RHIFormat::Unknown;
        RHIFormat backBufferFormat = RHIFormat::Unknown;
        RHIFormat toneMappingOutputFormat = RHIFormat::Unknown;
        bool hdrSceneColorEnabled = false;
        std::string hdrFallbackReason;
        PostProcessStackExecuteStats stackStats;
    };

    struct SceneEnvironmentIBLStats
    {
        uint64 frameCount = 0;
        bool skyboxFound = false;
        bool uploadRequested = false;
        bool textureIBLEnabled = false;
        uint32 prefilteredMipLevels = 1;
        float intensity = 1.0f;
        std::string fallbackReason;
    };

    /**
     * @brief Scene renderer - orchestrates rendering of a scene
     * 
     * SceneRenderer is responsible for:
     * - Collecting renderable data from the World
     * - Setting up the RenderGraph with render passes
     * - Executing the render graph
     * 
     * Usage:
     * @code
     * SceneRenderer renderer;
     * renderer.Initialize(renderContext);
     * 
     * // Each frame
     * renderer.SetupView(camera, world);
     * renderer.Render();
     * @endcode
     */
    class SceneRenderer
    {
    public:
        SceneRenderer();
        ~SceneRenderer();

        // Non-copyable
        SceneRenderer(const SceneRenderer&) = delete;
        SceneRenderer& operator=(const SceneRenderer&) = delete;

        // =====================================================================
        // Lifecycle
        // =====================================================================

        /**
         * @brief Initialize the scene renderer
         * @param renderContext The render context to use
         */
        void Initialize(RenderContext* renderContext);

        /**
         * @brief Shutdown and release resources
         */
        void Shutdown();

        /**
         * @brief Check if initialized
         */
        bool IsInitialized() const { return m_initialized; }

        // =====================================================================
        // Frame Setup
        // =====================================================================

        /**
         * @brief Setup view data from camera and collect scene data
         * @param camera The camera to render from
         * @param world The world to render (can be null for just camera setup)
         */
        void SetupView(const Camera& camera, World* world);

        /**
         * @brief Set the color render target
         * @param target The render target texture handle
         */
        void SetColorTarget(RGTextureHandle target) { m_viewData.colorTarget = target; }

        /**
         * @brief Set the depth render target
         * @param target The depth target texture handle
         */
        void SetDepthTarget(RGTextureHandle target) { m_viewData.depthTarget = target; }

        // =====================================================================
        // Rendering
        // =====================================================================

        /**
         * @brief Build and execute the render graph
         * 
         * This builds the render graph by calling Setup on all registered passes,
         * compiles it, and executes it.
         */
        void Render();

        // =====================================================================
        // Pass Management
        // =====================================================================

        /**
         * @brief Add a render pass
         * @param pass The pass to add (takes ownership)
         */
        void AddPass(std::unique_ptr<IRenderPass> pass);

        /**
         * @brief Remove a render pass by name
         * @param name The name of the pass to remove
         * @return true if the pass was found and removed
         */
        bool RemovePass(const char* name);

        /**
         * @brief Clear all render passes
         */
        void ClearPasses();

        /**
         * @brief Get number of registered passes
         */
        size_t GetPassCount() const;

        // =====================================================================
        // Accessors
        // =====================================================================

        /// Get the render graph
        RenderGraph* GetRenderGraph() { return m_renderGraph.get(); }
        const RenderGraph* GetRenderGraph() const { return m_renderGraph.get(); }

        /// Get the current view data
        ViewData& GetViewData() { return m_viewData; }
        const ViewData& GetViewData() const { return m_viewData; }

        /// Get the render scene
        RenderScene& GetRenderScene() { return m_renderScene; }
        const RenderScene& GetRenderScene() const { return m_renderScene; }

        /// Get the render context
        RenderContext* GetRenderContext() { return m_renderContext; }

        /// Get the GPU resource manager
        GPUResourceManager* GetGPUResourceManager() { return m_gpuResourceManager.get(); }

        /// Get the pipeline cache
        PipelineCache* GetPipelineCache() { return m_pipelineCache.get(); }

        /// Get the material system
        MaterialSystem* GetMaterialSystem() { return m_materialSystem.get(); }

        /// Get the transient resource pool
        TransientResourcePool* GetTransientResourcePool() { return m_transientResourcePool.get(); }

        /// Get the resource view cache
        ResourceViewCache* GetResourceViewCache() { return m_resourceViewCache.get(); }

        /// Get visible object indices
        const std::vector<uint32_t>& GetVisibleObjectIndices() const { return m_visibleObjectIndices; }

        /// Get scene collection path statistics.
        const SceneRenderCollectionStats& GetCollectionStats() const { return m_collectionStats; }

        /// Get render pass chain statistics from the last RenderGraph build.
        const SceneRenderPassChainStats& GetPassChainStats() const { return m_passChainStats; }

        /// Get runtime post-process statistics from the last RenderGraph build.
        const SceneRenderPostProcessStats& GetPostProcessStats() const { return m_postProcessStats; }

        /// Get environment IBL binding statistics from the last view setup.
        const SceneEnvironmentIBLStats& GetEnvironmentIBLStats() const { return m_environmentIBLStats; }

        /// Get runtime post-process stack.
        PostProcessStack* GetPostProcessStack() { return m_postProcessStack.get(); }
        const PostProcessStack* GetPostProcessStack() const { return m_postProcessStack.get(); }

        /// Apply runtime post-process settings.
        void ApplyPostProcessSettings(const PostProcessSettings& settings);

        /// Get runtime post-process settings.
        PostProcessSettings& GetPostProcessSettings() { return m_postProcessSettings; }
        const PostProcessSettings& GetPostProcessSettings() const { return m_postProcessSettings; }

        /// Get draw items for material-aware passes
        const std::vector<RenderDrawItem>& GetOpaqueDrawItems() const { return m_opaqueDrawItems; }
        const std::vector<RenderDrawItem>& GetMaskedDrawItems() const { return m_maskedDrawItems; }
        const std::vector<RenderDrawItem>& GetTransparentDrawItems() const { return m_transparentDrawItems; }

        /// Set shader directory (must be set before Initialize)
        void SetShaderDirectory(const std::string& dir) { m_shaderDir = dir; }

    private:
        void BuildRenderGraph();
        void BuildMaterialDrawLists();
        void PreparePassesForFrame();
        void SetupDefaultPostProcess();
        void SetupDefaultPasses();
        void UpdateEnvironmentIBL(World* world);
        SceneColorFormatPolicy ResolveSceneColorFormatPolicy(RHIFormat backBufferFormat,
                                                             bool postProcessActive) const;
        bool SupportsHDRSceneColor() const;
        void UpdatePassResources();
        void ExecutePasses(RHICommandContext& ctx);
        void EnsureDepthBuffer(uint32_t width, uint32_t height);

        RenderContext* m_renderContext = nullptr;
        std::unique_ptr<RenderGraph> m_renderGraph;
        std::unique_ptr<GPUResourceManager> m_gpuResourceManager;
        std::unique_ptr<PipelineCache> m_pipelineCache;
        std::unique_ptr<MaterialSystem> m_materialSystem;
        std::unique_ptr<TransientResourcePool> m_transientResourcePool;
        std::unique_ptr<ResourceViewCache> m_resourceViewCache;
        std::unique_ptr<RenderPassRegistry> m_passRegistry;
        std::unique_ptr<RenderProxySceneBridge> m_proxyBridge;
        std::unique_ptr<PostProcessStack> m_postProcessStack;
        
        ViewData m_viewData;
        RenderScene m_renderScene;
        RenderProxySnapshot m_proxySnapshot;
        SceneRenderCollectionStats m_collectionStats;
        SceneRenderPassChainStats m_passChainStats;
        SceneRenderPostProcessStats m_postProcessStats;
        SceneEnvironmentIBLStats m_environmentIBLStats;
        SceneColorFormatPolicy m_sceneColorFormatPolicy;
        PostProcessSettings m_postProcessSettings;
        std::vector<uint32_t> m_visibleObjectIndices;
        std::vector<RenderDrawItem> m_opaqueDrawItems;
        std::vector<RenderDrawItem> m_maskedDrawItems;
        std::vector<RenderDrawItem> m_transparentDrawItems;
        std::vector<std::string> m_loggedUnsupportedPassNames;
        
        std::string m_shaderDir;
        DepthPrepass* m_depthPrepass = nullptr;  // Cached pointer to optional depth prepass
        OpaquePass* m_opaquePass = nullptr;  // Cached pointer to opaque pass
        ShadowPass* m_shadowPass = nullptr;  // Cached pointer to shadow pass
        TransparentPass* m_transparentPass = nullptr;  // Cached pointer to transparent pass
        SkyboxPass* m_skyboxPass = nullptr;  // Cached pointer to skybox pass
        BloomPass* m_bloomPostProcess = nullptr;
        ToneMappingPass* m_toneMappingPostProcess = nullptr;
        
        // Depth buffer
        RHITextureRef m_depthTexture;
        RHITextureViewRef m_depthTextureView;
        uint32_t m_depthWidth = 0;
        uint32_t m_depthHeight = 0;
        
        // Back buffer state tracking
        std::vector<RHIResourceState> m_backBufferStates;
        RHIResourceState m_depthBufferState = RHIResourceState::Undefined;
        
        // Track swap chain dimensions to detect resize
        uint32_t m_lastSwapChainWidth = 0;
        uint32_t m_lastSwapChainHeight = 0;
        
        bool m_initialized = false;
    };

} // namespace RVX
