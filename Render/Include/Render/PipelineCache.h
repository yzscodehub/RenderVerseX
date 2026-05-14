#pragma once

/**
 * @file PipelineCache.h
 * @brief Pipeline cache for managing graphics pipelines and frame/object sets
 *
 * PipelineCache handles:
 * - Shader compilation with ShaderManager
 * - Stable frame/object/material descriptor set layouts
 * - Graphics pipeline creation and caching
 * - Frame and object constant buffer management
 */

#include "Core/Assert.h"
#include "Core/MathTypes.h"
#include "Render/Material/MaterialClassification.h"
#include "RHI/RHI.h"

#include <array>
#include <limits>
#include <memory>
#include <string>

namespace RVX
{
    // Forward declarations
    class ShaderManager;
    struct ShaderCompileResult;
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
     * The default material pipeline uses three descriptor sets:
     * set 0 = frame, set 1 = object, set 2 = material.
     */
    class PipelineCache
    {
    public:
        PipelineCache();
        ~PipelineCache();

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
         * @brief Get the alpha-masked pipeline
         * @return Graphics pipeline or nullptr if not available
         */
        RHIPipeline* GetMaskedPipeline() const { return m_maskedPipeline.Get(); }

        /**
         * @brief Get the alpha-blended transparent pipeline
         * @return Graphics pipeline or nullptr if not available
         */
        RHIPipeline* GetTransparentPipeline() const { return m_transparentPipeline.Get(); }

        /**
         * @brief Get a pipeline for a material variant
         */
        RHIPipeline* GetPipelineForVariant(MaterialPipelineVariant variant) const;

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
         * @brief Reset transient per-draw constant slots for a new frame
         */
        void BeginFrame();

        /**
         * @brief Get the frame constants descriptor set (set 0)
         */
        RHIDescriptorSet* GetFrameDescriptorSet();

        /**
         * @brief Backward-compatible alias for frame constants
         */
        RHIDescriptorSet* GetViewDescriptorSet() { return GetFrameDescriptorSet(); }

        /**
         * @brief Get the object constants descriptor set (set 1)
         */
        RHIDescriptorSet* GetObjectDescriptorSet();

        /**
         * @brief Get the material descriptor set layout (set 2)
         */
        RHIDescriptorSetLayout* GetMaterialSetLayout() const;

        /**
         * @brief Get dynamic constant-buffer offset for the current object slot
         */
        std::array<uint32, 1> GetCurrentObjectDynamicOffset() const;

        /**
         * @brief Build a single dynamic offset span for descriptor sets with one dynamic binding
         */
        static std::array<uint32, 1> BuildSingleDynamicOffset(uint64 offset)
        {
            return {ToRHIConstantDynamicOffset(offset)};
        }

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
        static uint32 ToRHIConstantDynamicOffset(uint64 offset)
        {
            constexpr uint64 maxDynamicOffset = static_cast<uint64>(std::numeric_limits<uint32>::max());
            if (offset > maxDynamicOffset)
            {
                RVX_VERIFY(false, "PipelineCache: dynamic constant offset {} exceeds the RHI uint32 offset limit",
                           offset);
                return 0;
            }

            return static_cast<uint32>(offset);
        }

        bool CompileShaders();
        bool CreatePipelineLayout();
        bool CreatePipeline();
        RHIPipelineRef CreateDefaultLitPipeline(const char* debugName,
                                                const RHIDepthStencilState& depthStencilState,
                                                const RHIBlendState& blendState);
        bool CreateViewConstantBuffer();
        bool CreateObjectConstantBuffer();
        RHIDescriptorSetRef CreateFrameDescriptorSet();
        RHIDescriptorSetRef CreateObjectDescriptorSet();
        uint64 AllocateObjectConstantSlot();

        IRHIDevice* m_device = nullptr;
        std::string m_shaderDir;
        bool m_initialized = false;

        // Shader manager
        std::unique_ptr<ShaderManager> m_shaderManager;

        // Shaders
        RHIShaderRef m_vertexShader;
        RHIShaderRef m_pixelShader;
        std::unique_ptr<ShaderCompileResult> m_vsCompileResult;
        std::unique_ptr<ShaderCompileResult> m_psCompileResult;

        // Descriptor set layouts and pipeline layout
        std::vector<RHIDescriptorSetLayoutRef> m_setLayouts;
        RHIPipelineLayoutRef m_pipelineLayout;

        // Graphics pipelines
        RHIPipelineRef m_opaquePipeline;
        RHIPipelineRef m_maskedPipeline;
        RHIPipelineRef m_transparentPipeline;
        RHIPipelineRef m_depthOnlyPipeline;

        // Frame and object constants
        RHIBufferRef m_viewConstantBuffer;
        RHIBufferRef m_objectConstantBuffer;
        RHIDescriptorSetRef m_frameDescriptorSet;
        RHIDescriptorSetRef m_objectDescriptorSet;
        uint64 m_objectConstantStride = 0;
        uint64 m_objectConstantCursor = 0;
        uint64 m_currentObjectConstantOffset = 0;

        // Render target format
        RHIFormat m_renderTargetFormat = RHIFormat::RGBA8_UNORM;
    };

} // namespace RVX
