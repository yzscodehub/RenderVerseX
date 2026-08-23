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
     * diverge on the fixed 240-byte shader ABI.
     */
    [[nodiscard]] MaterialGPUConstants EncodeMaterialGPUConstants(
        const MaterialSourceData& sourceData,
        std::span<const MaterialUploadTextureBinding> textureBindings = {}) noexcept;

} // namespace RVX
