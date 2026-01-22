#pragma once

/**
 * @file MaterialBinder.h
 * @brief Binds material data (textures and constants) to the GPU pipeline
 */

#include "Render/Material/MaterialGPUData.h"
#include "RHI/RHI.h"
#include <unordered_map>

namespace RVX
{
    // Forward declarations
    class Material;
    class GPUResourceManager;

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
        void Initialize(IRHIDevice* device, GPUResourceManager* gpuResources);

        /**
         * @brief Shutdown and release resources
         */
        void Shutdown();

        /**
         * @brief Check if initialized
         */
        bool IsInitialized() const { return m_device != nullptr; }

        // =========================================================================
        // Binding
        // =========================================================================

        /**
         * @brief Bind a material for rendering
         * @param ctx Command context to bind to
         * @param material The material to bind
         * @param setIndex Descriptor set index for material constants
         * 
         * Updates the material constant buffer and binds textures.
         */
        void Bind(RHICommandContext& ctx, const Material& material, uint32 setIndex = 2);

        /**
         * @brief Bind a material by ID
         * @param ctx Command context
         * @param materialId Material resource ID
         * @param setIndex Descriptor set index
         */
        void Bind(RHICommandContext& ctx, uint64 materialId, uint32 setIndex = 2);

        /**
         * @brief Convert material to GPU constants
         * @param material Source material
         * @return GPU constant buffer data
         */
        static MaterialGPUConstants ConvertToGPU(const Material& material);

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
        void EnsureConstantBuffer();
        void UpdateConstantBuffer(const MaterialGPUConstants& constants);

        IRHIDevice* m_device = nullptr;
        GPUResourceManager* m_gpuResources = nullptr;

        // Shared constant buffer for material data
        RHIBufferRef m_constantBuffer;

        // Current material ID (for caching)
        uint64 m_currentMaterialId = 0;

        // Default material constants
        MaterialGPUConstants m_defaultConstants;
    };

} // namespace RVX
