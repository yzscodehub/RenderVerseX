#pragma once

/**
 * @file PipelineCache.h
 * @brief Pipeline cache for managing graphics pipelines
 * 
 * PipelineCache handles:
 * - Shader compilation with ShaderManager
 * - Pipeline layout creation using shader reflection
 * - Graphics pipeline creation and caching
 * - View constants buffer management
 */

#include "Core/MathTypes.h"
#include "RHI/RHI.h"
#include "ShaderCompiler/ShaderManager.h"
#include "ShaderCompiler/ShaderLayout.h"
#include <memory>
#include <string>
#include <unordered_map>

namespace RVX
{
    // Forward declarations
    struct ViewData;

    /**
     * @brief View constants structure (matches HLSL cbuffer)
     */
    struct ViewConstants
    {
        Mat4 viewProjection;
        Vec3 cameraPosition;
        float time;
        Vec3 lightDirection;
        float padding;
    };

    /**
     * @brief Object constants structure (matches HLSL cbuffer)
     */
    struct ObjectConstants
    {
        Mat4 world;
    };

    /**
     * @brief Pipeline cache for graphics pipelines
     * 
     * Uses shader reflection to automatically generate pipeline layouts.
     * Manages default shaders and pipelines for scene rendering.
     */
    class PipelineCache
    {
    public:
        PipelineCache() = default;
        ~PipelineCache();

        // Non-copyable
        PipelineCache(const PipelineCache&) = delete;
        PipelineCache& operator=(const PipelineCache&) = delete;

        // =====================================================================
        // Initialization
        // =====================================================================

        /**
         * @brief Initialize the pipeline cache
         * @param device RHI device
         * @param shaderDir Directory containing shader files
         * @return true if initialization succeeded
         */
        bool Initialize(IRHIDevice* device, const std::string& shaderDir);

        /**
         * @brief Shutdown and release resources
         */
        void Shutdown();

        /**
         * @brief Check if initialized
         */
        bool IsInitialized() const { return m_initialized; }

        // =====================================================================
        // Pipeline Access
        // =====================================================================

        /**
         * @brief Get the default opaque pipeline
         * @return Graphics pipeline or nullptr if not available
         */
        RHIPipeline* GetOpaquePipeline() const { return m_opaquePipeline.Get(); }

        /**
         * @brief Get the depth-only pipeline for depth prepass
         * @return Depth-only pipeline or nullptr if not available
         * @note Currently returns nullptr - depth-only pipeline not yet implemented
         */
        RHIPipeline* GetDepthOnlyPipeline() const { return m_depthOnlyPipeline.Get(); }

        /**
         * @brief Get the default pipeline layout
         */
        RHIPipelineLayout* GetDefaultLayout() const { return m_pipelineLayout.Get(); }

        // =====================================================================
        // Descriptor Sets
        // =====================================================================

        /**
         * @brief Get the view constants descriptor set
         */
        RHIDescriptorSet* GetViewDescriptorSet() const { return m_viewDescriptorSet.Get(); }

        /**
         * @brief Update view constants from ViewData
         * @param view The current view data
         */
        void UpdateViewConstants(const ViewData& view);

        /**
         * @brief Update per-object constants
         * @param worldMatrix The object's world matrix
         */
        void UpdateObjectConstants(const Mat4& worldMatrix);

        // =====================================================================
        // Render Target Format
        // =====================================================================

        /**
         * @brief Set the render target format (call before Initialize or recreate pipeline)
         */
        void SetRenderTargetFormat(RHIFormat format) { m_renderTargetFormat = format; }

    private:
        bool CompileShaders();
        bool CreatePipelineLayout();
        bool CreatePipeline();
        bool CreateViewConstantBuffer();
        bool CreateObjectConstantBuffer();

        IRHIDevice* m_device = nullptr;
        std::string m_shaderDir;
        bool m_initialized = false;

        // Shader manager
        std::unique_ptr<ShaderManager> m_shaderManager;

        // Shaders
        RHIShaderRef m_vertexShader;
        RHIShaderRef m_pixelShader;
        ShaderCompileResult m_vsCompileResult;
        ShaderCompileResult m_psCompileResult;

        // Pipeline layout (from reflection)
        std::vector<RHIDescriptorSetLayoutRef> m_setLayouts;
        RHIPipelineLayoutRef m_pipelineLayout;

        // Graphics pipelines
        RHIPipelineRef m_opaquePipeline;
        RHIPipelineRef m_depthOnlyPipeline;  // For depth prepass

        // View constants
        RHIBufferRef m_viewConstantBuffer;
        RHIBufferRef m_objectConstantBuffer;
        RHIDescriptorSetRef m_viewDescriptorSet;

        // Render target format
        RHIFormat m_renderTargetFormat = RHIFormat::RGBA8_UNORM;
    };

} // namespace RVX
