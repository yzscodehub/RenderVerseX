#include "Render/Renderer/RenderDrawItem.h"
#include "Render/Renderer/RenderScene.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

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

        MaterialRenderMode ResolveMaterialRenderMode(const RenderObject& object,
                                                     size_t submeshIndex)
        {
            if (submeshIndex < object.materialModes.size())
            {
                return object.materialModes[submeshIndex];
            }

            return MaterialRenderMode::Opaque;
        }

        MeshBatch MakeLegacyBatch(const RenderObject& object,
                                  uint32 objectIndex,
                                  MaterialRenderMode mode)
        {
            RenderBatchFlags flags = RenderBatchFlags::None;
            if (object.HasSkinningData())
                flags |= RenderBatchFlags::Skinned;
            if (object.castsShadow)
                flags |= RenderBatchFlags::CastsShadow;
            if (object.receivesShadow)
                flags |= RenderBatchFlags::ReceivesShadow;
            if (mode == MaterialRenderMode::Masked)
                flags |= RenderBatchFlags::Masked;
            if (mode == MaterialRenderMode::Transparent)
                flags |= RenderBatchFlags::Transparent;
            if (!object.material.IsValid())
                flags |= RenderBatchFlags::MissingMaterial;

            MeshBatch batch;
            batch.objectId = object.entityId;
            batch.mesh = object.mesh;
            batch.material = object.material;
            batch.submeshIndex = 0;
            batch.primitiveData = objectIndex;
            batch.materialMode = mode;
            batch.flags = flags;
            return batch;
        }

        RenderDrawItem MakeDrawItem(const RenderScene& scene,
                                    uint32 objectIndex,
                                    const RenderObject& object,
                                    const MeshBatch& batch,
                                    const Vec3& cameraPosition)
        {
            RenderDrawItem item;
            item.objectIndex = objectIndex;
            item.submeshIndex = batch.submeshIndex;
            item.mesh = batch.mesh;
            item.material = batch.material;
            item.renderMode = batch.materialMode;
            item.depthFromCamera = length(Vec3(object.worldMatrix[3]) - cameraPosition);
            if (scene.FindCachedDrawPacketTemplate(batch, item.packet))
            {
                item.packet.objectId = batch.objectId;
                item.packet.primitiveData = batch.primitiveData;
                item.packet.submeshIndex = batch.submeshIndex;
            }
            else
            {
                item.packet = BuildLegacyMaterialDrawPacket(batch);
            }
            return item;
        }

        void AppendToMaterialList(RenderDrawItem item,
                                  std::vector<RenderDrawItem>& opaque,
                                  std::vector<RenderDrawItem>& masked,
                                  std::vector<RenderDrawItem>& transparent)
        {
            if (item.renderMode == MaterialRenderMode::Transparent)
            {
                item.sortKey = BuildTransparentDrawSortKey(item);
                transparent.push_back(std::move(item));
            }
            else if (item.renderMode == MaterialRenderMode::Masked)
            {
                item.sortKey = BuildOpaqueDrawSortKey(item);
                masked.push_back(std::move(item));
            }
            else
            {
                item.sortKey = BuildOpaqueDrawSortKey(item);
                opaque.push_back(std::move(item));
            }
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
            if (obj.meshBatchesAuthoritative)
            {
                for (const MeshBatch& batch : obj.meshBatches)
                {
                    AppendToMaterialList(
                        MakeDrawItem(scene,
                                     objectIndex,
                                     obj,
                                     batch,
                                     cameraPosition),
                        outOpaqueDrawItems,
                        outMaskedDrawItems,
                        outTransparentDrawItems);
                }
                continue;
            }

            const MeshBatch batch = MakeLegacyBatch(
                obj, objectIndex, ResolveMaterialRenderMode(obj, 0));
            AppendToMaterialList(
                MakeDrawItem(scene,
                             objectIndex,
                             obj,
                             batch,
                             cameraPosition),
                outOpaqueDrawItems,
                outMaskedDrawItems,
                outTransparentDrawItems);
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
        const uint64 materialIdentity = item.material.IsValid()
                                            ? (static_cast<uint64>(item.material.slot) << 32U) |
                                                  item.material.generation
                                            : 0;
        const uint64 meshIdentity = item.mesh.IsValid()
                                        ? (static_cast<uint64>(item.mesh.slot) << 32U) |
                                              item.mesh.generation
                                        : 0;
        const uint64 materialBits =
            MixSortBits(materialIdentity) & 0xFFFF'FFFFULL;
        const uint64 meshBits = MixSortBits(meshIdentity) & 0xFFFF'0000ULL;
        return (materialBits << 32) | meshBits | static_cast<uint64>(item.submeshIndex);
    }

    uint64 BuildTransparentDrawSortKey(const RenderDrawItem& item)
    {
        const float safeDepth = std::isfinite(item.depthFromCamera) ? item.depthFromCamera : 0.0f;
        const float clampedDepth = std::clamp(safeDepth, 0.0f, 1000000.0f);
        const uint64 depthBits = static_cast<uint64>(clampedDepth * 1000.0f);
        const uint64 materialIdentity = item.material.IsValid()
                                            ? (static_cast<uint64>(item.material.slot) << 32U) |
                                                  item.material.generation
                                            : 0;
        return (depthBits << 24) |
               (MixSortBits(materialIdentity) & 0x00FF'FFFFULL);
    }

} // namespace RVX
