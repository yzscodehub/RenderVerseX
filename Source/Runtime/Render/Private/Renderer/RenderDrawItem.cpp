#include "Render/Renderer/RenderDrawItem.h"
#include "Render/Renderer/RenderScene.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <tuple>
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

        constexpr uint64 kTransparentOrderHashSeed = 1469598103934665603ULL;
        constexpr uint64 kTransparentOrderHashPrime = 1099511628211ULL;

        void AppendHash(uint64& hash, uint64 value) noexcept
        {
            hash ^= value;
            hash *= kTransparentOrderHashPrime;
        }

        [[nodiscard]] std::tuple<uint64,
                                 uint32,
                                 uint32,
                                 uint32,
                                 uint32,
                                 uint32,
                                 uint32,
                                 uint32>
        GetTransparentTieBreak(const RenderDrawItem& item) noexcept
        {
            // RenderObjectId and RenderResourceHandle both carry the complete
            // retained identity.  objectIndex is deliberately last: it only
            // disambiguates malformed duplicate batches without becoming the
            // primary ordering key.
            return {item.packet.objectId,
                    item.submeshIndex,
                    item.material.slot,
                    item.material.generation,
                    item.mesh.slot,
                    item.mesh.generation,
                    item.packet.primitiveData,
                    item.objectIndex};
        }

        [[nodiscard]] bool IsTransparentBefore(const RenderDrawItem& lhs,
                                               const RenderDrawItem& rhs) noexcept
        {
            // The caller rejects non-finite distances before sorting.  Keep
            // the explicit finite check here as well so an externally supplied
            // list can never acquire an unspecified std::sort ordering.
            if (!std::isfinite(lhs.depthFromCamera) ||
                !std::isfinite(rhs.depthFromCamera))
            {
                return false;
            }
            if (lhs.depthFromCamera != rhs.depthFromCamera)
            {
                return lhs.depthFromCamera > rhs.depthFromCamera;
            }
            return GetTransparentTieBreak(lhs) < GetTransparentTieBreak(rhs);
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
                                  std::vector<RenderDrawItem>& transparent,
                                  TransparentDrawListDiagnostics*
                                      transparentDiagnostics)
        {
            if (item.renderMode == MaterialRenderMode::Transparent)
            {
                if (!std::isfinite(item.depthFromCamera))
                {
                    if (transparentDiagnostics != nullptr)
                    {
                        ++transparentDiagnostics->rejectedNonFiniteDepthCount;
                    }
                    // A transparent blend order with NaN/Inf depth is not
                    // meaningful. Reject that packet rather than relying on
                    // implementation-defined comparator behaviour.
                    return;
                }
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
                                std::vector<RenderDrawItem>& outTransparentDrawItems,
                                TransparentDrawListDiagnostics*
                                    outTransparentDiagnostics)
    {
        outOpaqueDrawItems.clear();
        outMaskedDrawItems.clear();
        outTransparentDrawItems.clear();
        if (outTransparentDiagnostics != nullptr)
        {
            *outTransparentDiagnostics = {};
        }

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
                        outTransparentDrawItems,
                        outTransparentDiagnostics);
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
                outTransparentDrawItems,
                outTransparentDiagnostics);
        }

        const auto sortFrontToBack = [](const RenderDrawItem& lhs, const RenderDrawItem& rhs)
        {
            return lhs.sortKey < rhs.sortKey;
        };
        std::sort(outOpaqueDrawItems.begin(), outOpaqueDrawItems.end(), sortFrontToBack);
        std::sort(outMaskedDrawItems.begin(), outMaskedDrawItems.end(), sortFrontToBack);

        std::sort(outTransparentDrawItems.begin(), outTransparentDrawItems.end(),
                  IsTransparentBefore);

        if (outTransparentDiagnostics != nullptr)
        {
            outTransparentDiagnostics->orderValid =
                IsTransparentDrawListStrictlyOrdered(outTransparentDrawItems);
            outTransparentDiagnostics->orderHash =
                BuildTransparentDrawOrderHash(outTransparentDrawItems);
        }
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
        uint64 hash = kTransparentOrderHashSeed;
        AppendHash(hash, std::bit_cast<uint32>(item.depthFromCamera));
        const auto [objectId,
                    submeshIndex,
                    materialSlot,
                    materialGeneration,
                    meshSlot,
                    meshGeneration,
                    primitiveData,
                    objectIndex] = GetTransparentTieBreak(item);
        AppendHash(hash, objectId);
        AppendHash(hash, submeshIndex);
        AppendHash(hash, materialSlot);
        AppendHash(hash, materialGeneration);
        AppendHash(hash, meshSlot);
        AppendHash(hash, meshGeneration);
        AppendHash(hash, primitiveData);
        AppendHash(hash, objectIndex);
        return MixSortBits(hash);
    }

    bool IsTransparentDrawListStrictlyOrdered(
        const std::vector<RenderDrawItem>& drawItems) noexcept
    {
        if (std::any_of(drawItems.begin(), drawItems.end(),
                        [](const RenderDrawItem& item)
                        {
                            return !std::isfinite(item.depthFromCamera);
                        }))
        {
            return false;
        }
        for (size_t index = 1; index < drawItems.size(); ++index)
        {
            if (!IsTransparentBefore(drawItems[index - 1], drawItems[index]))
            {
                return false;
            }
        }
        return true;
    }

    uint64 BuildTransparentDrawOrderHash(
        const std::vector<RenderDrawItem>& drawItems) noexcept
    {
        uint64 hash = kTransparentOrderHashSeed;
        AppendHash(hash, static_cast<uint64>(drawItems.size()));
        for (const RenderDrawItem& item : drawItems)
        {
            AppendHash(hash, BuildTransparentDrawSortKey(item));
        }
        return MixSortBits(hash);
    }

} // namespace RVX
