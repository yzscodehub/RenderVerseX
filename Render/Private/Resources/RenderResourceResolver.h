#pragma once

/** @file RenderResourceResolver.h @brief Exact-handle resource lookup with legacy fallback */

#include "Render/GPUResourceManager.h"
#include "Resources/RenderResourceRegistry.h"

namespace RVX
{
    inline MeshGPUBuffers ResolveRenderMeshBuffers(
        const RenderResourceRegistry* registry,
        GPUResourceManager* legacyResources,
        RenderResourceHandle handle,
        uint64 legacyId)
    {
        if (registry != nullptr && handle.IsValid())
        {
            return registry->ResolveMeshBuffers(handle);
        }
        return legacyResources != nullptr
                   ? legacyResources->GetMeshBuffers(legacyId)
                   : MeshGPUBuffers{};
    }

    inline RHITexture* ResolveRenderTexture(
        const RenderResourceRegistry* registry,
        GPUResourceManager* legacyResources,
        RenderResourceHandle handle,
        uint64 legacyId)
    {
        if (registry != nullptr && handle.IsValid())
        {
            return registry->ResolveTextureObject(handle);
        }
        return legacyResources != nullptr
                   ? legacyResources->GetTexture(legacyId)
                   : nullptr;
    }
} // namespace RVX
