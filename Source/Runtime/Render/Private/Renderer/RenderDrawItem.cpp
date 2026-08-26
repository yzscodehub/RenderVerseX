#include "Render/Renderer/RenderDrawItem.h"
#include "Render/Renderer/RenderDrawPlan.h"
#include "Render/Renderer/RenderScene.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <set>
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

        void AppendFloatHash(uint64& hash, float32 value) noexcept
        {
            AppendHash(hash, std::bit_cast<uint32>(value));
        }

        [[nodiscard]] bool HasDrawFlag(RenderDrawFlags flags,
                                       RenderDrawFlags flag) noexcept
        {
            return (static_cast<uint32>(flags) &
                    static_cast<uint32>(flag)) != 0;
        }

        [[nodiscard]] uint64 ComputeClassificationSemanticHash(
            const RenderSceneDrawClassification& classification) noexcept
        {
            uint64 hash = kTransparentOrderHashSeed;
            for (const RenderSceneDrawClassificationEntry& entry :
                 classification.entries)
            {
                const RenderDrawPacket& packet = entry.draw.packet;
                AppendHash(hash, entry.classificationIndex);
                AppendHash(hash, entry.draw.objectIndex);
                AppendHash(hash, entry.draw.submeshIndex);
                AppendHash(hash, static_cast<uint8>(entry.draw.renderMode));
                AppendHash(hash, packet.objectId);
                AppendHash(hash, packet.primitiveData);
                AppendHash(hash, packet.submeshIndex);
                AppendHash(hash, GetStableHash(packet.pipelineKey));
                AppendHash(hash, GetStableHash(packet.geometryKey));
                AppendHash(hash, GetStableHash(packet.materialKey));
                AppendHash(hash,
                           packet.materialInstanceKey.textureBindingHash);
                AppendHash(hash,
                           static_cast<uint8>(packet.materialInstanceKey
                                                  .parameterTableCompatible));
                AppendHash(hash, packet.arguments.indexCount);
                AppendHash(hash, packet.arguments.instanceCount);
                AppendHash(hash, packet.arguments.firstIndex);
                AppendHash(hash,
                           static_cast<uint32>(packet.arguments.vertexOffset));
                AppendHash(hash, packet.arguments.firstInstance);
                AppendHash(hash, static_cast<uint32>(packet.flags));
                AppendHash(hash, entry.layerMask);
                AppendHash(hash, static_cast<uint8>(entry.sceneVisible));
                AppendHash(hash, static_cast<uint8>(entry.drawable));
                AppendHash(hash, static_cast<uint8>(entry.castsShadow));
                AppendHash(hash, static_cast<uint8>(entry.skinned));
                AppendFloatHash(hash, entry.worldPosition.x);
                AppendFloatHash(hash, entry.worldPosition.y);
                AppendFloatHash(hash, entry.worldPosition.z);
                const Vec3 boundsMin = entry.worldBounds.GetMin();
                const Vec3 boundsMax = entry.worldBounds.GetMax();
                AppendFloatHash(hash, boundsMin.x);
                AppendFloatHash(hash, boundsMin.y);
                AppendFloatHash(hash, boundsMin.z);
                AppendFloatHash(hash, boundsMax.x);
                AppendFloatHash(hash, boundsMax.y);
                AppendFloatHash(hash, boundsMax.z);
            }
            AppendHash(hash, classification.stats.sceneObjectCount);
            AppendHash(hash, classification.stats.classifiedPacketCount);
            AppendHash(hash, classification.stats.classifiedPrimitiveCount);
            AppendHash(hash, classification.stats.materialBindingCount);
            AppendHash(hash, classification.stats.uniqueMaterialCount);
            hash = MixSortBits(hash);
            return hash == 0 ? kTransparentOrderHashSeed : hash;
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
            if (!std::isfinite(lhs.viewSpaceDepth) ||
                !std::isfinite(rhs.viewSpaceDepth))
            {
                return false;
            }
            if (lhs.translucencySortPriority != rhs.translucencySortPriority)
            {
                return lhs.translucencySortPriority < rhs.translucencySortPriority;
            }
            if (lhs.viewSpaceDepth != rhs.viewSpaceDepth)
            {
                return lhs.viewSpaceDepth > rhs.viewSpaceDepth;
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

        [[nodiscard]] bool HasMirroredWinding(const Mat4& worldMatrix) noexcept
        {
            const float32 determinant = glm::determinant(Mat3(worldMatrix));
            return std::isfinite(determinant) && determinant < 0.0f;
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
            item.viewSpaceDepth = 0.0f;
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
            // RenderScene owns the retained primitive identity. A stale batch
            // ordinal must not create a second authority for object lookup.
            item.packet.objectId = object.entityId;
            item.packet.primitiveData = objectIndex;
            item.packet.submeshIndex = batch.submeshIndex;
            // A negative affine determinant reverses triangle winding.  Keep
            // that fact in the packet identity so all raster lanes choose a
            // matching front-face pipeline rather than silently dropping the
            // object or relaxing it to no-cull.
            item.packet.pipelineKey.reverseWinding =
                HasMirroredWinding(object.worldMatrix);
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
                if (!std::isfinite(item.viewSpaceDepth))
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

void RenderSceneDrawClassification::Clear()
{
    entries.clear();
    stats = {};
    structurallyValid = false;
}

bool RenderSceneDrawClassification::IsValid() const noexcept
{
    if (!structurallyValid || entries.size() != stats.classifiedPacketCount ||
        stats.classifiedPrimitiveCount > stats.sceneObjectCount ||
        stats.materialBindingCount != stats.classifiedPacketCount ||
        stats.uniqueMaterialCount > stats.materialBindingCount ||
        stats.semanticHash == 0 ||
        stats.semanticHash != ComputeClassificationSemanticHash(*this))
    {
        return false;
    }
    for (uint32 index = 0; index < static_cast<uint32>(entries.size()); ++index)
    {
        const RenderSceneDrawClassificationEntry& entry = entries[index];
        if (entry.classificationIndex != index ||
            entry.draw.classificationIndex != index ||
            entry.draw.objectIndex >= stats.sceneObjectCount ||
            entry.draw.packet.primitiveData != entry.draw.objectIndex ||
            entry.draw.packet.submeshIndex != entry.draw.submeshIndex ||
            entry.draw.packet.geometryKey.mesh != entry.draw.mesh ||
            entry.draw.packet.materialKey.material != entry.draw.material)
        {
            return false;
        }
    }
    return true;
}

bool BuildRenderSceneDrawClassification(
    const RenderScene& scene,
    RenderSceneDrawClassificationSources sources,
    RenderSceneDrawClassification& outClassification)
{
    outClassification.Clear();
    if (scene.GetObjectCount() > std::numeric_limits<uint32>::max())
    {
        return false;
    }
    outClassification.stats.sceneObjectCount =
        static_cast<uint32>(scene.GetObjectCount());
    std::set<uint32> primitives;
    std::set<RenderResourceHandle> materials;

    const auto appendEntry = [&](uint32 objectIndex,
                                 const RenderObject& object,
                                 const MeshBatch& batch) -> bool
    {
        if (outClassification.entries.size() >=
            static_cast<size_t>(RVX_INVALID_INDEX))
        {
            return false;
        }
        RenderSceneDrawClassificationEntry entry;
        entry.classificationIndex =
            static_cast<uint32>(outClassification.entries.size());
        entry.draw = MakeDrawItem(
            scene,
            objectIndex,
            object,
            batch,
            Vec3(object.worldMatrix[3]));
        if (sources.bindingResolver != nullptr)
        {
            sources.bindingResolver->Resolve(entry.draw.packet);
        }
        entry.draw.classificationIndex = entry.classificationIndex;
        entry.draw.viewSpaceDepth = 0.0f;
        entry.draw.translucencySortPriority =
            entry.draw.packet.translucencySortPriority;
        entry.draw.sortKey = entry.draw.renderMode ==
                                     MaterialRenderMode::Transparent
                                 ? BuildTransparentDrawSortKey(entry.draw)
                                 : BuildOpaqueDrawSortKey(entry.draw);
        entry.layerMask = object.layerMask;
        entry.worldBounds = object.bounds;
        entry.worldPosition = Vec3(object.worldMatrix[3]);
        entry.sceneVisible = object.visible;
        entry.drawable = object.drawable;
        entry.castsShadow = HasDrawFlag(
            entry.draw.packet.flags, RenderDrawFlags::CastsShadow);
        entry.skinned = object.HasSkinningData();
        primitives.insert(objectIndex);
        materials.insert(entry.draw.material);
        outClassification.entries.push_back(std::move(entry));
        return true;
    };

    for (uint32 objectIndex = 0;
         objectIndex < outClassification.stats.sceneObjectCount;
         ++objectIndex)
    {
        const RenderObject& object = scene.GetObject(objectIndex);
        if (object.meshBatchesAuthoritative)
        {
            for (const MeshBatch& batch : object.meshBatches)
            {
                if (!appendEntry(objectIndex, object, batch))
                {
                    outClassification.Clear();
                    return false;
                }
            }
            continue;
        }
        const MeshBatch batch = MakeLegacyBatch(
            object, objectIndex, ResolveMaterialRenderMode(object, 0));
        if (!appendEntry(objectIndex, object, batch))
        {
            outClassification.Clear();
            return false;
        }
    }

    outClassification.stats.classifiedPacketCount =
        static_cast<uint32>(outClassification.entries.size());
    outClassification.stats.classifiedPrimitiveCount =
        static_cast<uint32>(primitives.size());
    outClassification.stats.materialBindingCount =
        outClassification.stats.classifiedPacketCount;
    outClassification.stats.uniqueMaterialCount =
        static_cast<uint32>(materials.size());
    outClassification.stats.semanticHash =
        ComputeClassificationSemanticHash(outClassification);
    outClassification.structurallyValid = true;
    if (!outClassification.IsValid())
    {
        outClassification.Clear();
        return false;
    }
    return true;
}

bool BuildRenderSceneDrawClassification(
    const RenderScene& scene,
    RenderSceneDrawClassification& outClassification)
{
    return BuildRenderSceneDrawClassification(
        scene, RenderSceneDrawClassificationSources{}, outClassification);
}

bool ProjectRenderSceneDrawClassification(
    const RenderSceneDrawClassification& classification,
    std::span<const uint8> visibleObjectMask,
    uint32 viewCullingMask,
    const Vec3& cameraPosition,
    const Vec3& cameraForward,
    std::vector<RenderDrawItem>& outOpaqueDrawItems,
    std::vector<RenderDrawItem>& outMaskedDrawItems,
    std::vector<RenderDrawItem>& outTransparentDrawItems,
    TransparentDrawListDiagnostics* outTransparentDiagnostics)
{
    outOpaqueDrawItems.clear();
    outMaskedDrawItems.clear();
    outTransparentDrawItems.clear();
    if (outTransparentDiagnostics != nullptr)
    {
        *outTransparentDiagnostics = {};
    }
    if (!classification.IsValid())
    {
        return false;
    }
    const float32 forwardLengthSquared = dot(cameraForward, cameraForward);
    if (!std::isfinite(forwardLengthSquared) || forwardLengthSquared <= 0.0f)
    {
        return false;
    }
    const Vec3 normalizedCameraForward = cameraForward / std::sqrt(forwardLengthSquared);

    for (const RenderSceneDrawClassificationEntry& entry :
         classification.entries)
    {
        if (!entry.sceneVisible || !entry.drawable ||
            entry.draw.objectIndex >= visibleObjectMask.size() ||
            visibleObjectMask[entry.draw.objectIndex] == 0 ||
            !IsRenderLayerVisible(entry.layerMask, viewCullingMask))
        {
            continue;
        }
        RenderDrawItem item = entry.draw;
        item.viewSpaceDepth = dot(entry.worldPosition - cameraPosition,
                                  normalizedCameraForward);
        item.translucencySortPriority = item.packet.translucencySortPriority;
        AppendToMaterialList(std::move(item),
                             outOpaqueDrawItems,
                             outMaskedDrawItems,
                             outTransparentDrawItems,
                             outTransparentDiagnostics);
    }

    const auto sortFrontToBack = [](const RenderDrawItem& lhs,
                                    const RenderDrawItem& rhs)
    {
        return lhs.sortKey < rhs.sortKey;
    };
    std::sort(outOpaqueDrawItems.begin(),
              outOpaqueDrawItems.end(),
              sortFrontToBack);
    std::sort(outMaskedDrawItems.begin(),
              outMaskedDrawItems.end(),
              sortFrontToBack);
    std::sort(outTransparentDrawItems.begin(),
              outTransparentDrawItems.end(),
              IsTransparentBefore);
    if (outTransparentDiagnostics != nullptr)
    {
        outTransparentDiagnostics->orderValid =
            IsTransparentDrawListStrictlyOrdered(outTransparentDrawItems);
        outTransparentDiagnostics->orderHash =
            BuildTransparentDrawOrderHash(outTransparentDrawItems);
    }
    return true;
}

void BuildMaterialDrawLists(const RenderScene& scene,
                                const std::vector<uint32_t>& visibleObjectIndices,
                                const Vec3& cameraPosition,
                                std::vector<RenderDrawItem>& outOpaqueDrawItems,
                                std::vector<RenderDrawItem>& outMaskedDrawItems,
                                std::vector<RenderDrawItem>& outTransparentDrawItems,
                                TransparentDrawListDiagnostics*
                                    outTransparentDiagnostics)
    {
        RenderSceneDrawClassification classification;
        if (!BuildRenderSceneDrawClassification(scene, classification))
        {
            outOpaqueDrawItems.clear();
            outMaskedDrawItems.clear();
            outTransparentDrawItems.clear();
            if (outTransparentDiagnostics != nullptr)
            {
                *outTransparentDiagnostics = {};
                outTransparentDiagnostics->orderValid = false;
            }
            return;
        }
        std::vector<uint8> visibleMask(scene.GetObjectCount(), 0);
        for (uint32 objectIndex : visibleObjectIndices)
        {
            if (objectIndex < visibleMask.size())
            {
                visibleMask[objectIndex] = 1;
            }
        }
        (void)ProjectRenderSceneDrawClassification(
            classification,
            visibleMask,
            ~0U,
            cameraPosition,
            Vec3(0.0f, 0.0f, -1.0f),
            outOpaqueDrawItems,
            outMaskedDrawItems,
            outTransparentDrawItems,
            outTransparentDiagnostics);
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
        AppendHash(hash, static_cast<uint32>(item.translucencySortPriority));
        AppendHash(hash, std::bit_cast<uint32>(item.viewSpaceDepth));
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
                            return !std::isfinite(item.viewSpaceDepth);
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
