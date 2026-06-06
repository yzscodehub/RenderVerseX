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
#include <filesystem>
#include <limits>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

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

    struct PipelineCacheConfig
    {
        RHIFormat renderTargetFormat = RHIFormat::RGBA8_UNORM;
        RHIFormat depthStencilFormat = RHIFormat::D32_FLOAT;
        bool reverseZ = false;
        std::filesystem::path manifestDirectory;
    };

    struct PipelineCacheStats
    {
        uint64 lastPipelineStateHash = 0;
        uint64 opaquePipelineHash = 0;
        uint64 maskedPipelineHash = 0;
        uint64 transparentPipelineHash = 0;
        uint64 toneMappingPipelineHash = 0;
        uint32 pipelineCreateCount = 0;
        uint32 pipelineCacheHitCount = 0;
        uint32 pipelineCacheMissCount = 0;
        bool manifestLoaded = false;
        bool manifestValid = false;
        bool manifestInvalidated = false;
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

        /**
         * @brief Configure pipeline cache inputs. Must be called before Initialize().
         */
        void SetConfig(const PipelineCacheConfig& config);

        const PipelineCacheConfig& GetConfig() const { return m_config; }
        const PipelineCacheStats& GetStats() const { return m_stats; }
        const std::string& GetLastError() const { return m_lastError; }

        uint64 GetPipelineStateHashForVariant(MaterialPipelineVariant variant) const;

        static constexpr const char* GetManifestFileName() { return "PipelineCacheManifest.txt"; }
        static constexpr RHIFormat GetDefaultDepthStencilFormat() { return RHIFormat::D32_FLOAT; }
        static constexpr float GetDepthClearValue(bool reverseZ) { return reverseZ ? 0.0f : 1.0f; }
        static RHIDepthStencilState BuildDepthStencilState(bool reverseZ, bool depthWrite);
        float GetDepthClearValue() const { return GetDepthClearValue(m_config.reverseZ); }

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
         */
        RHIPipeline* GetDepthOnlyPipeline() const { return m_depthOnlyPipeline.Get(); }

        /**
         * @brief Get the fullscreen ToneMapping post-process pipeline
         */
        RHIPipeline* GetToneMappingPipeline() const { return m_toneMappingPipeline.Get(); }

        /**
         * @brief Get the default pipeline layout
         */
        RHIPipelineLayout* GetDefaultLayout() const { return m_pipelineLayout.Get(); }

        /**
         * @brief Get the post-process fullscreen pipeline layout
         */
        RHIPipelineLayout* GetPostProcessLayout() const { return m_postProcessPipelineLayout.Get(); }

        /**
         * @brief Get the descriptor set layout used by fullscreen post-process passes
         */
        RHIDescriptorSetLayout* GetPostProcessSetLayout() const { return m_postProcessSetLayout.Get(); }

        /**
         * @brief Get the owning RHI device for pass-local resources
         */
        IRHIDevice* GetDevice() const { return m_device; }

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
        void SetRenderTargetFormat(RHIFormat format)
        {
            m_renderTargetFormat = format;
            m_config.renderTargetFormat = format;
        }

        void SetDepthStencilFormat(RHIFormat format) { m_config.depthStencilFormat = format; }
        void SetReverseZ(bool enabled) { m_config.reverseZ = enabled; }

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
        bool CreatePostProcessPipelineLayout();
        bool CreatePipeline();
        RHIPipelineRef GetOrCreateDefaultLitPipeline(MaterialPipelineVariant variant,
                                                     const char* debugName,
                                                     const RHIDepthStencilState& depthStencilState,
                                                     const RHIBlendState& blendState);
        RHIPipelineRef GetOrCreateDepthOnlyPipeline();
        RHIPipelineRef GetOrCreateToneMappingPipeline();
        RHIGraphicsPipelineDesc BuildDefaultLitPipelineDesc(const char* debugName,
                                                            const RHIDepthStencilState& depthStencilState,
                                                            const RHIBlendState& blendState) const;
        RHIGraphicsPipelineDesc BuildDepthOnlyPipelineDesc() const;
        RHIGraphicsPipelineDesc BuildToneMappingPipelineDesc() const;
        bool CreateViewConstantBuffer();
        bool CreateObjectConstantBuffer();
        RHIDescriptorSetRef CreateFrameDescriptorSet();
        RHIDescriptorSetRef CreateObjectDescriptorSet();
        uint64 AllocateObjectConstantSlot();
        bool BuildReflectedDefaultLitLayouts(std::vector<RHIDescriptorSetLayoutDesc>& outLayouts);
        bool ValidateDefaultLitLayouts(const std::vector<RHIDescriptorSetLayoutDesc>& layouts);
        void ProcessPipelineManifest();
        void SetLastError(std::string message);
        uint64 ComputePipelineStateHash(const RHIGraphicsPipelineDesc& desc, MaterialPipelineVariant variant) const;
        uint64 ComputeShaderHash(const ShaderCompileResult* result) const;
        uint64 StoreVariantHash(MaterialPipelineVariant variant, uint64 hash);

        IRHIDevice* m_device = nullptr;
        std::string m_shaderDir;
        bool m_initialized = false;
        PipelineCacheConfig m_config;
        PipelineCacheStats m_stats;
        std::string m_lastError;

        // Shader manager
        std::unique_ptr<ShaderManager> m_shaderManager;

        // Shaders
        RHIShaderRef m_vertexShader;
        RHIShaderRef m_pixelShader;
        RHIShaderRef m_depthOnlyVertexShader;
        RHIShaderRef m_toneMappingVertexShader;
        RHIShaderRef m_toneMappingPixelShader;
        std::unique_ptr<ShaderCompileResult> m_vsCompileResult;
        std::unique_ptr<ShaderCompileResult> m_psCompileResult;
        std::unique_ptr<ShaderCompileResult> m_depthOnlyVsCompileResult;
        std::unique_ptr<ShaderCompileResult> m_toneMappingVsCompileResult;
        std::unique_ptr<ShaderCompileResult> m_toneMappingPsCompileResult;

        // Descriptor set layouts and pipeline layout
        std::vector<RHIDescriptorSetLayoutRef> m_setLayouts;
        RHIPipelineLayoutRef m_pipelineLayout;
        RHIDescriptorSetLayoutRef m_postProcessSetLayout;
        RHIPipelineLayoutRef m_postProcessPipelineLayout;

        // Graphics pipelines
        RHIPipelineRef m_opaquePipeline;
        RHIPipelineRef m_maskedPipeline;
        RHIPipelineRef m_transparentPipeline;
        RHIPipelineRef m_depthOnlyPipeline;
        RHIPipelineRef m_toneMappingPipeline;
        std::unordered_map<uint64, RHIPipelineRef> m_pipelineCache;

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
