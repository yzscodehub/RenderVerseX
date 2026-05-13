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

#include "Core/Assert.h"
#include "Core/MathTypes.h"
#include "RHI/RHI.h"
#include <array>
#include <limits>
#include <memory>
#include <string>
#include <unordered_map>

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
     * @brief Material constants structure (matches HLSL cbuffer)
     */
    struct MaterialConstants
    {
        Vec4 baseColorFactor{1.0f, 1.0f, 1.0f, 1.0f};
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
        PipelineCache();
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
         * @brief Reset transient per-draw constant slots for a new frame
         */
        void BeginFrame();

        /**
         * @brief Get the view constants descriptor set
         */
        RHIDescriptorSet* GetViewDescriptorSet();

        /**
         * @brief Get a descriptor set for view/object constants and a base color texture
         * @param baseColorView Texture view for the material base color, or nullptr for default white
         */
        RHIDescriptorSet* GetMaterialDescriptorSet(RHITextureView* baseColorView);

        /**
         * @brief Get a descriptor set and synchronize cached texture-view generation
         * @param baseColorView Texture view for the material base color, or nullptr for default white
         * @param textureViewGeneration Generation from the ResourceViewCache that produced baseColorView
         */
        RHIDescriptorSet* GetMaterialDescriptorSet(RHITextureView* baseColorView, uint64 textureViewGeneration);

        /**
         * @brief Get dynamic constant-buffer offsets for the current object/material slots
         */
        std::array<uint32, 2> GetCurrentConstantDynamicOffsets() const;

        /**
         * @brief Build dynamic offsets in the pipeline layout order: object constants, then material constants
         */
        static std::array<uint32, 2> BuildConstantDynamicOffsets(uint64 objectOffset, uint64 materialOffset)
        {
            return {
                ToRHIConstantDynamicOffset(objectOffset),
                ToRHIConstantDynamicOffset(materialOffset)
            };
        }

        /**
         * @brief Clear descriptor sets that reference externally cached texture views
         */
        void ClearMaterialDescriptorSets();

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

        /**
         * @brief Update per-material constants
         * @param baseColorFactor Material base color multiplier
         */
        void UpdateMaterialConstants(const Vec4& baseColorFactor);

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
                return std::numeric_limits<uint32>::max();
            }

            return static_cast<uint32>(offset);
        }

        bool CompileShaders();
        bool CreatePipelineLayout();
        bool CreatePipeline();
        bool CreateDefaultMaterialResources();
        bool CreateViewConstantBuffer();
        bool CreateObjectConstantBuffer();
        bool CreateMaterialConstantBuffer();
        RHIDescriptorSetRef CreateMaterialDescriptorSet(RHITextureView* baseColorView);
        RHIDescriptorSet* GetMaterialDescriptorSetInternal(RHITextureView* baseColorView);
        uint64 AllocateObjectConstantSlot();
        uint64 AllocateMaterialConstantSlot();

        struct MaterialDescriptorKey
        {
            RHITextureView* textureView = nullptr;

            bool operator==(const MaterialDescriptorKey& other) const
            {
                return textureView == other.textureView;
            }
        };

        struct MaterialDescriptorKeyHash
        {
            size_t operator()(const MaterialDescriptorKey& key) const
            {
                return std::hash<RHITextureView*>{}(key.textureView);
            }
        };

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

        // Pipeline layout (from reflection)
        std::vector<RHIDescriptorSetLayoutRef> m_setLayouts;
        RHIPipelineLayoutRef m_pipelineLayout;

        // Graphics pipelines
        RHIPipelineRef m_opaquePipeline;
        RHIPipelineRef m_depthOnlyPipeline;  // For depth prepass

        // View constants
        RHIBufferRef m_viewConstantBuffer;
        RHIBufferRef m_objectConstantBuffer;
        RHIBufferRef m_materialConstantBuffer;
        RHIDescriptorSetRef m_viewDescriptorSet;
        RHITextureRef m_defaultWhiteTexture;
        RHITextureViewRef m_defaultWhiteTextureView;
        RHISamplerRef m_defaultSampler;
        std::unordered_map<MaterialDescriptorKey, RHIDescriptorSetRef, MaterialDescriptorKeyHash> m_materialDescriptorSets;
        uint64 m_objectConstantStride = 0;
        uint64 m_materialConstantStride = 0;
        uint64 m_objectConstantCursor = 0;
        uint64 m_materialConstantCursor = 0;
        uint64 m_currentObjectConstantOffset = 0;
        uint64 m_currentMaterialConstantOffset = 0;
        uint64 m_materialDescriptorCacheGeneration = ~uint64{0};

        // Render target format
        RHIFormat m_renderTargetFormat = RHIFormat::RGBA8_UNORM;
    };

} // namespace RVX
