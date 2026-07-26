#pragma once

/** @file RenderResourceResolver.h @brief Exact-handle resource lookup helpers */

#include "Resources/RenderResourceRegistry.h"

namespace RVX
{
    inline MeshGPUBuffers ResolveRenderMeshBuffers(
        const RenderResourceRegistry* registry,
        RenderResourceHandle handle)
    {
        if (registry != nullptr && handle.IsValid())
        {
            return registry->ResolveMeshBuffers(handle);
        }
        return {};
    }

    inline RHITexture* ResolveRenderTexture(
        const RenderResourceRegistry* registry,
        RenderResourceHandle handle)
    {
        if (registry != nullptr && handle.IsValid())
        {
            return registry->ResolveTextureObject(handle);
        }
        return nullptr;
    }
} // namespace RVX
