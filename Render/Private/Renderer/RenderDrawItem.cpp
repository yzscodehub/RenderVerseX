#include "Render/Renderer/RenderDrawItem.h"
#include "Render/Renderer/RenderScene.h"
#include "Resource/Types/MaterialResource.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace RVX
{
    namespace
    {
        uint64 MixSortBits(uint64 value)
        {
            value ^= value >> 33;
            value *= 0xff51afd7ed558ccdULL;
            value ^= value >> 33;
            value *= 0xc4ceb9fe1a85ec53ULL;
            value ^= value >> 33;
            return value;
        }
    } // namespace

    void BuildMaterialDrawLists(const RenderScene& scene,
                                const std::vector<uint32_t>& visibleObjectIndices,
                                const Vec3& cameraPosition,
                                std::vector<RenderDrawItem>& outOpaqueDrawItems,
                                std::vector<RenderDrawItem>& outMaskedDrawItems,
                                std::vector<RenderDrawItem>& outTransparentDrawItems)
    {
        outOpaqueDrawItems.clear();
        outMaskedDrawItems.clear();
        outTransparentDrawItems.clear();

        for (uint32_t objectIndex : visibleObjectIndices)
        {
            if (objectIndex >= scene.GetObjectCount())
                continue;

            const RenderObject& obj = scene.GetObject(objectIndex);
            const size_t submeshCount = obj.materialResources.empty() ? 1 : obj.materialResources.size();

            for (size_t submeshIndex = 0; submeshIndex < submeshCount; ++submeshIndex)
            {
                Resource::MaterialResource* materialResource =
                    submeshIndex < obj.materialResources.size() ? obj.materialResources[submeshIndex] : nullptr;
                const Material* material = materialResource ? materialResource->GetMaterial().get() : nullptr;
                const MaterialRenderMode mode = ClassifyMaterialRenderMode(material);

                RenderDrawItem item;
                item.objectIndex = objectIndex;
                item.submeshIndex = static_cast<uint32>(submeshIndex);
                item.meshId = obj.meshId;
                item.materialId = submeshIndex < obj.materialIds.size() ? obj.materialIds[submeshIndex] : 0;
                item.materialResource = materialResource;
                item.renderMode = mode;
                item.depthFromCamera = length(Vec3(obj.worldMatrix[3]) - cameraPosition);

                if (mode == MaterialRenderMode::Transparent)
                {
                    item.sortKey = BuildTransparentDrawSortKey(item);
                    outTransparentDrawItems.push_back(item);
                }
                else if (mode == MaterialRenderMode::Masked)
                {
                    item.sortKey = BuildOpaqueDrawSortKey(item);
                    outMaskedDrawItems.push_back(item);
                }
                else
                {
                    item.sortKey = BuildOpaqueDrawSortKey(item);
                    outOpaqueDrawItems.push_back(item);
                }
            }
        }

        const auto sortFrontToBack = [](const RenderDrawItem& lhs, const RenderDrawItem& rhs)
        {
            return lhs.sortKey < rhs.sortKey;
        };
        std::sort(outOpaqueDrawItems.begin(), outOpaqueDrawItems.end(), sortFrontToBack);
        std::sort(outMaskedDrawItems.begin(), outMaskedDrawItems.end(), sortFrontToBack);

        std::sort(outTransparentDrawItems.begin(), outTransparentDrawItems.end(),
                  [](const RenderDrawItem& lhs, const RenderDrawItem& rhs)
                  {
                      return lhs.depthFromCamera > rhs.depthFromCamera;
                  });
    }

    uint64 BuildOpaqueDrawSortKey(const RenderDrawItem& item)
    {
        const uint64 materialBits = MixSortBits(item.materialId) & 0xFFFF'FFFFULL;
        const uint64 meshBits = MixSortBits(item.meshId) & 0xFFFF'0000ULL;
        return (materialBits << 32) | meshBits | static_cast<uint64>(item.submeshIndex);
    }

    uint64 BuildTransparentDrawSortKey(const RenderDrawItem& item)
    {
        const float safeDepth = std::isfinite(item.depthFromCamera) ? item.depthFromCamera : 0.0f;
        const float clampedDepth = std::clamp(safeDepth, 0.0f, 1000000.0f);
        const uint64 depthBits = static_cast<uint64>(clampedDepth * 1000.0f);
        return (depthBits << 24) | (MixSortBits(item.materialId) & 0x00FF'FFFFULL);
    }

} // namespace RVX
