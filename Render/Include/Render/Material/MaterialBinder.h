#pragma once

/**
 * @file MaterialBinder.h
 * @brief Binds material data (textures and constants) to the GPU pipeline
 */

#include "Render/Material/MaterialGPUData.h"
#include "Render/Material/MaterialSourceData.h"
#include "RenderContracts/RenderIdentity.h"
#include "RHI/RHI.h"
#include <string>
#include <unordered_map>

namespace RVX
{
    // Forward declarations
    class GPUResourceManager;
    class RenderResourceRegistry;

    enum class MaterialBindStatus : uint8
    {
        None,
        BoundDefaultMaterial,
        Unsupported,
        Error
    };

    /**
     * @brief Binds material data to the rendering pipeline
     *
     * MaterialBinder handles:
     * - Converting CPU Material data to GPU constants
     * - Managing material constant buffers
     * - Binding material textures to shader slots
     * - Caching descriptor sets per material
     */
    class MaterialBinder
    {
    public:
        MaterialBinder() = default;
        ~MaterialBinder();

        // Non-copyable
        MaterialBinder(const MaterialBinder&) = delete;
        MaterialBinder& operator=(const MaterialBinder&) = delete;

        // =========================================================================
        // Lifecycle
        // =========================================================================

        /**
         * @brief Initialize the material binder
         * @param device RHI device for resource creation
         * @param gpuResources GPU resource manager for texture access
         */
        void Initialize(IRHIDevice* device,
                        GPUResourceManager* gpuResources,
                        const RenderResourceRegistry* resourceRegistry = nullptr);

        /**
         * @brief Shutdown and release resources
         */
        void Shutdown();

        /**
         * @brief Check if initialized
         */
        bool IsInitialized() const { return m_device != nullptr; }
        MaterialBindStatus GetLastBindStatus() const { return m_lastBindStatus; }
        const std::string& GetLastBindMessage() const { return m_lastBindMessage; }

        // =========================================================================
        // Binding
        // =========================================================================

        /**
         * @brief Bind a material for rendering
         * @param ctx Command context to bind to
         * @param material Source material data to bind
         * @param setIndex Descriptor set index for material constants
         *
         * Updates the material constant buffer and binds textures.
         */
        void Bind(RHICommandContext& ctx, const MaterialSourceData& material, uint32 setIndex = 2);

        /**
         * @brief Bind a material by ID
         * @param ctx Command context
         * @param materialId Material resource ID
         * @param setIndex Descriptor set index
         */
        void Bind(RHICommandContext& ctx, uint64 materialId, uint32 setIndex = 2);
        void Bind(RHICommandContext& ctx,
                  RenderResourceHandle material,
                  uint32 setIndex = 2);

        /**
         * @brief Convert render-facing material data to GPU constants
         * @param material Source material data
         * @return GPU constant buffer data
         */
        static MaterialGPUConstants ConvertToGPU(const MaterialSourceData& material);

        // =========================================================================
        // Default Material
        // =========================================================================

        /**
         * @brief Bind the default material
         * @param ctx Command context
         * @param setIndex Descriptor set index
         */
        void BindDefault(RHICommandContext& ctx, uint32 setIndex = 2);

        /**
         * @brief Get the default material constants
         */
        static MaterialGPUConstants GetDefaultConstants();

    private:
        bool EnsureConstantBuffer();
        bool UpdateConstantBuffer(const MaterialGPUConstants& constants);
        void SetBindResult(MaterialBindStatus status, std::string message);

        IRHIDevice* m_device = nullptr;
        GPUResourceManager* m_gpuResources = nullptr;
        const RenderResourceRegistry* m_resourceRegistry = nullptr;

        // Shared constant buffer for material data
        RHIBufferRef m_constantBuffer;

        // Current material ID (for caching)
        uint64 m_currentMaterialId = 0;
        MaterialBindStatus m_lastBindStatus = MaterialBindStatus::None;
        std::string m_lastBindMessage;

        // Default material constants
        MaterialGPUConstants m_defaultConstants;
    };

} // namespace RVX
