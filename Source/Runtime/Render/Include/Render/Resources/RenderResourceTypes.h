#pragma once

/** @file RenderResourceTypes.h @brief Render-owned GPU resource views */

#include "Core/Types.h"

#include <vector>

namespace RVX
{
    class RHIBuffer;

    /** @brief Immutable draw metadata for one uploaded submesh. */
    struct SubmeshGPUInfo
    {
        uint32 indexOffset = 0;
        uint32 indexCount = 0;
        int32 baseVertex = 0;
    };

    /** @brief Non-owning view resolved from an exact registry generation. */
    struct MeshGPUBuffers
    {
        RHIBuffer* positionBuffer = nullptr;
        RHIBuffer* normalBuffer = nullptr;
        RHIBuffer* uvBuffer = nullptr;
        RHIBuffer* uv1Buffer = nullptr;
        RHIBuffer* tangentBuffer = nullptr;
        RHIBuffer* boneIndicesBuffer = nullptr;
        RHIBuffer* boneWeightsBuffer = nullptr;
        RHIBuffer* indexBuffer = nullptr;
        std::vector<SubmeshGPUInfo> submeshes;
        bool isResident = false;
        bool hasNormals = false;
        bool hasUVs = false;
        bool hasUV1 = false;
        bool hasTangents = false;
        bool hasBoneIndices = false;
        bool hasBoneWeights = false;

        [[nodiscard]] bool IsValid() const
        {
            return positionBuffer != nullptr && indexBuffer != nullptr &&
                   isResident;
        }

        [[nodiscard]] bool HasNormalMapTangentBasis() const
        {
            return normalBuffer != nullptr && uvBuffer != nullptr &&
                   tangentBuffer != nullptr && hasNormals && hasUVs &&
                   hasTangents;
        }

        /**
         * @brief Return whether this mesh owns the requested texture-coordinate set.
         *
         * UV0 may be synthesized for legacy meshes. UV1 is optional and never
         * aliases UV0, so callers selecting a material texture binding must
         * reject a missing UV1 instead of silently sampling another set.
         */
        [[nodiscard]] bool HasTextureCoordinateSet(uint32 set) const noexcept
        {
            switch (set)
            {
                case 0: return uvBuffer != nullptr && hasUVs;
                case 1: return uv1Buffer != nullptr && hasUV1;
                default: return false;
            }
        }

        [[nodiscard]] bool HasSkinningVertexData() const
        {
            return boneIndicesBuffer != nullptr && boneWeightsBuffer != nullptr &&
                   hasBoneIndices && hasBoneWeights;
        }
    };

    /** @brief Value diagnostics for exact-generation registry residency. */
    struct RenderResourceRegistryStats
    {
        size_t residentMeshCount = 0;
        size_t residentTextureCount = 0;
        size_t residentMaterialCount = 0;
        size_t pendingUploadCount = 0;
        size_t usedMemory = 0;
    };
} // namespace RVX
