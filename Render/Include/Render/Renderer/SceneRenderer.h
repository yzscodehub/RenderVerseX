#pragma once

/**
 * @file SceneRenderer.h
 * @brief Scene renderer - orchestrates render pass execution
 */

#include "Render/Renderer/ViewData.h"
#include "Render/Renderer/RenderScene.h"
#include "Render/Graph/RenderGraph.h"
#include "Render/Context/RenderContext.h"
#include "Render/GPUResourceManager.h"
#include "Render/PipelineCache.h"
#include <memory>
#include <vector>

namespace RVX
{
    class World;
    class Camera;
    class IRenderPass;
    class OpaquePass;

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
        SceneRenderer() = default;
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
        size_t GetPassCount() const { return m_passes.size(); }

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

        /// Get visible object indices
        const std::vector<uint32_t>& GetVisibleObjectIndices() const { return m_visibleObjectIndices; }

        /// Set shader directory (must be set before Initialize)
        void SetShaderDirectory(const std::string& dir) { m_shaderDir = dir; }

    private:
        void BuildRenderGraph();
        void SetupDefaultPasses();
        void UpdatePassResources();
        void ExecutePasses(RHICommandContext& ctx);
        void EnsureDepthBuffer(uint32_t width, uint32_t height);

        RenderContext* m_renderContext = nullptr;
        std::unique_ptr<RenderGraph> m_renderGraph;
        std::unique_ptr<GPUResourceManager> m_gpuResourceManager;
        std::unique_ptr<PipelineCache> m_pipelineCache;
        std::vector<std::unique_ptr<IRenderPass>> m_passes;
        
        ViewData m_viewData;
        RenderScene m_renderScene;
        std::vector<uint32_t> m_visibleObjectIndices;
        
        std::string m_shaderDir;
        OpaquePass* m_opaquePass = nullptr;  // Cached pointer to opaque pass
        
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
