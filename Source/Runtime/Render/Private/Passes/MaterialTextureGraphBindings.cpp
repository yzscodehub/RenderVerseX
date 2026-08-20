/**
 * @file MaterialTextureGraphBindings.cpp
 * @brief RenderGraph declarations for registry-owned material textures.
 */

#include "Passes/MaterialTextureGraphBindings.h"

#include "Render/Graph/RenderGraph.h"
#include "Render/Passes/RenderPassRecordContext.h"
#include "Resources/RenderResourceRegistry.h"

#include <algorithm>

namespace RVX
{
    bool DeclareMaterialTextureGraphReads(
        RenderGraphBuilder& builder,
        const RenderResourceRegistry* registry,
        RenderResourceHandle material,
        RenderPassRecordResults& results)
    {
        if (registry == nullptr || !material.IsValid())
        {
            return true;
        }

        const RenderMaterialResourceData* materialData =
            registry->ResolveMaterial(material);
        if (materialData == nullptr || !materialData->metadataValid)
        {
            return true;
        }

        for (const MaterialUploadTextureBinding& binding :
             materialData->textureBindings)
        {
            if (!binding.texture.IsValid())
            {
                continue;
            }

            const auto existing = std::find_if(
                results.externalTextureAccesses.begin(),
                results.externalTextureAccesses.end(),
                [&binding](const RenderGraphExternalTextureAccess& access)
                {
                    return access.resource == binding.texture;
                });
            RGTextureHandle graphTexture;
            if (existing != results.externalTextureAccesses.end())
            {
                graphTexture = existing->graphTexture;
            }
            else
            {
                const RenderTextureResourceData* texture =
                    registry->ResolveTexture(binding.texture);
                if (texture == nullptr || !texture->texture)
                {
                    // The material binding owns the explicit fallback choice.
                    continue;
                }
                graphTexture = builder.ImportTexture(
                    texture->texture, texture->accessSnapshot);
                if (!graphTexture.IsValid())
                {
                    return false;
                }
                results.externalTextureAccesses.push_back(
                    {binding.texture, graphTexture});
            }

            graphTexture = builder.Read(
                graphTexture,
                MakeRGAccessDesc(
                    RHIResourceState::ShaderResource,
                    RHIShaderStage::Pixel));
            if (!graphTexture.IsValid())
            {
                return false;
            }
            builder.SetExportState(
                graphTexture, RHIResourceState::ShaderResource);
        }
        return true;
    }
} // namespace RVX
