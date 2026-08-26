#pragma once

/**
 * @file MaterialGPUEncoding.h
 * @brief Shared CPU-to-GPU encoding for the fixed material constants ABI.
 */

#include "Render/Material/MaterialGPUData.h"
#include "RenderContracts/ResourceUploadRequest.h"

#include <span>

namespace RVX
{
    /**
     * @brief Encode canonical material parameters and texture bindings for HLSL.
     *
     * This is the sole CPU encoder for MaterialGPUConstants.  It deliberately
     * lives in RenderRuntimeCore so resource upload and render binding cannot
     * diverge on the fixed MaterialGPUConstants shader ABI.
     */
    [[nodiscard]] MaterialGPUConstants EncodeMaterialGPUConstants(
        const MaterialSourceData& sourceData,
        std::span<const MaterialUploadTextureBinding> textureBindings = {}) noexcept;

    /**
     * @brief Encode a renderer-selected material program and stable table slot.
     *
     * The immutable descriptor is intentionally separate from legacy source
     * workflow data so ToonLit cannot be approximated as StandardLit.
     */
    [[nodiscard]] MaterialGPUConstants EncodeMaterialGPUConstants(
        const MaterialStaticDescriptor& staticDescriptor,
        const MaterialSourceData& sourceData,
        std::span<const MaterialUploadTextureBinding> textureBindings,
        uint32 materialTableIndex) noexcept;

} // namespace RVX
