/**
 * @file GPUSceneUpdate.cpp
 * @brief Incremental publication of accepted RenderScene values into CPU shadow rows.
 */

#include "GPUScene/GPUSceneUpdate.h"

#include "Render/Renderer/RenderDrawPacket.h"
#include "Render/Renderer/RenderScene.h"
#include "Render/Visibility/RenderVisibility.h"
#include "Resources/RenderResourceRegistry.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <unordered_set>
#include <utility>

namespace RVX
{
namespace
{
    constexpr float32 RVX_GPU_SCENE_AFFINE_EPSILON = 0.0001F;

    [[nodiscard]] bool IsLiveSchemaHeader(
        const GPUSceneRowHeader& header,
        uint32 generation,
        uint64 objectId) noexcept
    {
        const uint32 flags = header.flags;
        return header.schemaVersion == RVX_GPU_SCENE_SCHEMA_VERSION &&
               header.generation == generation &&
               UnpackGPUSceneUint64(header.objectId) == objectId &&
               (flags & static_cast<uint32>(GPUSceneRowFlags::Live)) != 0 &&
               (flags & static_cast<uint32>(GPUSceneRowFlags::Tombstone)) == 0;
    }

    [[nodiscard]] uint32 GetGPUScenePassMask(RenderPassKind pass) noexcept
    {
        switch (pass)
        {
            case RenderPassKind::Depth:
                return static_cast<uint32>(GPUScenePassMask::Depth);
            case RenderPassKind::Opaque:
                return static_cast<uint32>(GPUScenePassMask::Opaque);
            case RenderPassKind::Shadow:
                return static_cast<uint32>(GPUScenePassMask::Shadow);
            case RenderPassKind::Transparent:
                return static_cast<uint32>(GPUScenePassMask::Transparent);
            case RenderPassKind::None:
            default:
                return 0;
        }
    }

    struct BuildPublishedObjectResult
    {
        GPUSceneUpdate::PublishedObject object;
        GPUScenePublicationFailureReason failureReason =
            GPUScenePublicationFailureReason::None;

        [[nodiscard]] bool Succeeded() const noexcept
        {
            return failureReason == GPUScenePublicationFailureReason::None;
        }
    };

    [[nodiscard]] bool IsFiniteMatrix(const Mat4& matrix) noexcept
    {
        for (uint32 column = 0; column < 4; ++column)
        {
            for (uint32 row = 0; row < 4; ++row)
            {
                if (!std::isfinite(matrix[column][row]))
                {
                    return false;
                }
            }
        }
        return true;
    }

    [[nodiscard]] bool IsAffineMatrix(const Mat4& matrix) noexcept
    {
        return IsFiniteMatrix(matrix) &&
               std::abs(matrix[0][3]) <= RVX_GPU_SCENE_AFFINE_EPSILON &&
               std::abs(matrix[1][3]) <= RVX_GPU_SCENE_AFFINE_EPSILON &&
               std::abs(matrix[2][3]) <= RVX_GPU_SCENE_AFFINE_EPSILON &&
               std::abs(matrix[3][3] - 1.0F) <= RVX_GPU_SCENE_AFFINE_EPSILON;
    }

    [[nodiscard]] GPUSceneAffineMatrix3x4 PackAffineRowMajor(
        const Mat4& matrix) noexcept
    {
        GPUSceneAffineMatrix3x4 packed;
        for (uint32 row = 0; row < 3; ++row)
        {
            packed.rows[row] = {
                matrix[0][row], matrix[1][row], matrix[2][row], matrix[3][row]};
        }
        return packed;
    }

    [[nodiscard]] bool IsFiniteBounds(const AABB& bounds) noexcept
    {
        const Vec3& minimum = bounds.GetMin();
        const Vec3& maximum = bounds.GetMax();
        return bounds.IsValid() && std::isfinite(minimum.x) &&
               std::isfinite(minimum.y) && std::isfinite(minimum.z) &&
               std::isfinite(maximum.x) && std::isfinite(maximum.y) &&
               std::isfinite(maximum.z);
    }

    void SetFlag(uint32& value, uint32 flag) noexcept
    {
        value |= flag;
    }

    void AddUniqueResource(
        std::vector<RenderResourceHandle>& resources,
        RenderResourceHandle handle)
    {
        if (!handle.IsValid())
        {
            return;
        }
        if (std::find(resources.begin(), resources.end(), handle) == resources.end())
        {
            resources.push_back(handle);
        }
    }

    [[nodiscard]] uint32 MakePassMask(const MeshBatch& batch) noexcept
    {
        uint32 passMask = 0;
        switch (batch.materialMode)
        {
            case RenderMaterialMode::Masked:
                SetFlag(passMask, static_cast<uint32>(GPUScenePassMask::Depth));
                SetFlag(passMask, static_cast<uint32>(GPUScenePassMask::Opaque));
                break;
            case RenderMaterialMode::Transparent:
                SetFlag(passMask, static_cast<uint32>(GPUScenePassMask::Transparent));
                break;
            case RenderMaterialMode::Opaque:
            default:
                SetFlag(passMask, static_cast<uint32>(GPUScenePassMask::Depth));
                SetFlag(passMask, static_cast<uint32>(GPUScenePassMask::Opaque));
                break;
        }
        if (HasRenderBatchFlag(batch.flags, RenderBatchFlags::CastsShadow))
        {
            SetFlag(passMask, static_cast<uint32>(GPUScenePassMask::Shadow));
        }
        return passMask;
    }

    [[nodiscard]] GPUSceneMaterialRow MakeDefaultMaterialRow(
        const MeshBatch& batch,
        GPUSceneMaterialFlags reason) noexcept
    {
        GPUSceneMaterialRow row;
        row.materialFlags = static_cast<uint32>(GPUSceneMaterialFlags::DefaultMaterial) |
                            static_cast<uint32>(reason);
        if (batch.materialMode == RenderMaterialMode::Masked)
        {
            SetFlag(row.materialFlags, static_cast<uint32>(GPUSceneMaterialFlags::Masked));
        }
        else if (batch.materialMode == RenderMaterialMode::Transparent)
        {
            SetFlag(row.materialFlags, static_cast<uint32>(GPUSceneMaterialFlags::Transparent));
        }
        row.baseColor = {0.8F, 0.8F, 0.8F, 1.0F};
        row.metallic = 0.0F;
        row.roughness = 0.5F;
        row.emissiveIntensity = 0.0F;
        row.opacity = 1.0F;
        return row;
    }

    [[nodiscard]] BuildPublishedObjectResult BuildPublishedObject(
        const RenderObject& source,
        const RenderResourceRegistry& registry)
    {
        BuildPublishedObjectResult result;
        if (source.entityId == 0 || !source.drawable || !source.mesh.IsValid() ||
            source.meshBatches.empty() || !IsAffineMatrix(source.worldMatrix) ||
            !IsAffineMatrix(source.previousWorldMatrix) ||
            !IsFiniteMatrix(source.normalMatrix))
        {
            result.failureReason = GPUScenePublicationFailureReason::InvalidObject;
            return result;
        }

        GPUSceneObjectData& object = result.object.data;
        object.objectId = source.entityId;
        object.primitiveFlags = source.flags;
        object.layerMask = source.layerMask;
        object.sortKey = source.sortKey;
        object.transform.worldFromLocal = PackAffineRowMajor(source.worldMatrix);
        object.transform.previousWorldFromLocal =
            PackAffineRowMajor(source.previousWorldMatrix);
        object.transform.normalFromLocal = PackAffineRowMajor(source.normalMatrix);
        if (source.previousWorldMatrixValid != 0)
        {
            SetFlag(
                object.transform.transformFlags,
                static_cast<uint32>(GPUSceneTransformFlags::PreviousWorldFromLocalValid));
        }
        SetFlag(
            object.transform.transformFlags,
            static_cast<uint32>(GPUSceneTransformFlags::NormalFromLocalValid));

        if (IsFiniteBounds(source.bounds))
        {
            const Vec3& minimum = source.bounds.GetMin();
            const Vec3& maximum = source.bounds.GetMax();
            const Vec3 center = (minimum + maximum) * 0.5F;
            const Vec3 extent = (maximum - minimum) * 0.5F;
            object.bounds.minimum = {minimum.x, minimum.y, minimum.z, 0.0F};
            object.bounds.maximum = {maximum.x, maximum.y, maximum.z, 0.0F};
            object.bounds.sphere = {
                center.x, center.y, center.z, glm::length(extent)};
        }
        else
        {
            object.bounds.boundsFlags =
                static_cast<uint32>(GPUSceneBoundsFlags::Invalid) |
                static_cast<uint32>(GPUSceneBoundsFlags::ForceVisible);
        }

        object.draws.reserve(source.meshBatches.size());
        for (const MeshBatch& batch : source.meshBatches)
        {
            if (batch.objectId != source.entityId || batch.mesh != source.mesh ||
                !batch.mesh.IsValid())
            {
                result.failureReason = GPUScenePublicationFailureReason::InvalidObject;
                return result;
            }
            if (!registry.IsGPUReadyExact(batch.mesh))
            {
                result.failureReason =
                    GPUScenePublicationFailureReason::ResourceUnavailable;
                return result;
            }
            if (registry.ResolveMesh(batch.mesh) == nullptr)
            {
                result.failureReason =
                    GPUScenePublicationFailureReason::ResourceResolutionFailed;
                return result;
            }
            AddUniqueResource(result.object.requiredResources, batch.mesh);

            GPUSceneDrawData drawData;
            const RenderDrawPacket packet = BuildLegacyMaterialDrawPacket(batch);
            drawData.geometry.resourceSlot = batch.mesh.slot;
            drawData.geometry.resourceGeneration = batch.mesh.generation;
            drawData.geometry.geometryId =
                PackGPUSceneUint64(GetStableHash(packet.geometryKey));
            drawData.geometry.submeshIndex = batch.submeshIndex;
            drawData.geometry.firstIndex = packet.arguments.firstIndex;
            drawData.geometry.vertexOffset = packet.arguments.vertexOffset;
            drawData.geometry.indexCount = packet.arguments.indexCount;
            drawData.geometry.topology =
                static_cast<uint32>(packet.pipelineKey.topology);
            drawData.geometry.geometryFlags =
                batch.indexType == MeshUploadIndexType::UInt16
                    ? static_cast<uint32>(GPUSceneGeometryFlags::IndexUInt16)
                    : static_cast<uint32>(GPUSceneGeometryFlags::IndexUInt32);
            if (packet.pipelineKey.skinned)
            {
                SetFlag(
                    drawData.geometry.geometryFlags,
                    static_cast<uint32>(GPUSceneGeometryFlags::Skinned));
            }

            if (!batch.material.IsValid())
            {
                drawData.material = MakeDefaultMaterialRow(
                    batch, GPUSceneMaterialFlags::MissingMaterial);
            }
            else if (!registry.IsGPUReadyExact(batch.material))
            {
                result.failureReason =
                    GPUScenePublicationFailureReason::ResourceUnavailable;
                return result;
            }
            else
            {
                const RenderMaterialResourceData* material =
                    registry.ResolveMaterial(batch.material);
                if (material == nullptr)
                {
                    result.failureReason =
                        GPUScenePublicationFailureReason::ResourceResolutionFailed;
                    return result;
                }
                if (!material->metadataValid)
                {
                    drawData.material = MakeDefaultMaterialRow(
                        batch, GPUSceneMaterialFlags::MetadataInvalid);
                }
                else
                {
                    const MaterialSourceData& sourceData = material->sourceData;
                    drawData.material.resourceSlot = batch.material.slot;
                    drawData.material.resourceGeneration = batch.material.generation;
                    drawData.material.materialId =
                        PackGPUSceneUint64(GetStableHash(packet.materialKey));
                    drawData.material.shadingModel =
                        static_cast<uint32>(sourceData.workflow);
                    drawData.material.baseColor = {
                        sourceData.baseColorFactor.x,
                        sourceData.baseColorFactor.y,
                        sourceData.baseColorFactor.z,
                        sourceData.baseColorFactor.w};
                    drawData.material.metallic = sourceData.metallicFactor;
                    drawData.material.roughness = sourceData.roughnessFactor;
                    drawData.material.emissiveIntensity =
                        sourceData.emissiveStrength;
                    drawData.material.opacity = sourceData.baseColorFactor.w;
                    if (batch.materialMode == RenderMaterialMode::Masked)
                    {
                        SetFlag(
                            drawData.material.materialFlags,
                            static_cast<uint32>(GPUSceneMaterialFlags::Masked));
                    }
                    else if (batch.materialMode == RenderMaterialMode::Transparent)
                    {
                        SetFlag(
                            drawData.material.materialFlags,
                            static_cast<uint32>(GPUSceneMaterialFlags::Transparent));
                    }
                    if (sourceData.doubleSided)
                    {
                        SetFlag(
                            drawData.material.materialFlags,
                            static_cast<uint32>(GPUSceneMaterialFlags::DoubleSided));
                    }
                    if (!material->textureBindings.empty())
                    {
                        SetFlag(
                            drawData.material.materialFlags,
                            static_cast<uint32>(GPUSceneMaterialFlags::HasTextureBindings));
                    }
                    AddUniqueResource(
                        result.object.requiredResources, batch.material);
                }
            }

            drawData.draw.indexCount = packet.arguments.indexCount;
            drawData.draw.instanceCount = packet.arguments.instanceCount;
            drawData.draw.firstIndex = packet.arguments.firstIndex;
            drawData.draw.vertexOffset = packet.arguments.vertexOffset;
            drawData.draw.firstInstance = packet.arguments.firstInstance;
            drawData.draw.passMask = MakePassMask(batch);
            drawData.draw.materialVariant =
                static_cast<uint32>(packet.pipelineKey.materialVariant);
            drawData.draw.pipelineKey =
                PackGPUSceneUint64(GetStableHash(packet.pipelineKey));
            object.draws.push_back(std::move(drawData));
        }

        return result;
    }
} // namespace

void GPUSceneUpdate::PopulateCommittedIdentity(
    GPUScenePublicationStats& stats) const noexcept
{
    stats.committedVersion = m_database.GetCommittedVersion();
    stats.committedSourceSequence = m_committedSourceSequence;
    stats.publishedObjectCount = m_database.GetObjectCount();
    stats.publishedDrawCount = GetPublishedDrawCount();
}

uint32 GPUSceneUpdate::GetPublishedDrawCount() const noexcept
{
    uint32 drawCount = 0;
    for (const auto& [objectId, object] : m_publishedObjects)
    {
        static_cast<void>(objectId);
        drawCount += static_cast<uint32>(object.data.draws.size());
    }
    return drawCount;
}

void GPUSceneUpdate::SetDatabasePrepareAllocationFailureCountdownForTesting(
    int32 countdown) noexcept
{
    m_database.SetPrepareAllocationFailureCountdownForTesting(countdown);
}

void GPUSceneUpdate::SetThrowOnPublishForTesting(bool enabled) noexcept
{
    m_throwOnPublishForTesting = enabled;
}

std::optional<GPUSceneAcceptedDrawLookup>
GPUSceneUpdate::ResolveAcceptedDraw(
    const RenderScene& scene,
    const RenderVisibilityCandidate& candidate,
    const RenderDrawPacket& packet) const noexcept
{
    if (candidate.candidateIndex == RVX_INVALID_INDEX ||
        candidate.sourcePacketIndex == RVX_INVALID_INDEX ||
        candidate.pass == RenderPassKind::None ||
        candidate.pass != packet.pass ||
        packet.objectId == 0 ||
        packet.primitiveData != candidate.objectIndex ||
        candidate.objectIndex >= scene.GetObjectCount() ||
        !candidate.objectVisible || !candidate.drawable ||
        packet.arguments.indexCount == 0)
    {
        return std::nullopt;
    }

    const RenderObject& object = scene.GetObject(candidate.objectIndex);
    if (!object.drawable || !object.visible || object.entityId != packet.objectId ||
        object.mesh != packet.geometryKey.mesh)
    {
        return std::nullopt;
    }

    bool matchedAcceptedBatch = false;
    for (const MeshBatch& batch : object.meshBatches)
    {
        const RenderDrawPacket acceptedPacket = BuildLegacyMaterialDrawPacket(batch);
        if (batch.objectId == packet.objectId &&
            acceptedPacket.submeshIndex == packet.submeshIndex &&
            acceptedPacket.geometryKey == packet.geometryKey &&
            acceptedPacket.materialKey == packet.materialKey &&
            acceptedPacket.pipelineKey == packet.pipelineKey &&
            acceptedPacket.arguments == packet.arguments)
        {
            matchedAcceptedBatch = true;
            break;
        }
    }
    if (!matchedAcceptedBatch)
    {
        return std::nullopt;
    }

    const auto publishedObject = m_publishedObjects.find(packet.objectId);
    const std::optional<GPUScenePrimitiveRef> primitive =
        m_database.FindPrimitive(packet.objectId);
    if (publishedObject == m_publishedObjects.end() || !primitive ||
        !m_database.IsLive(*primitive))
    {
        return std::nullopt;
    }

    const GPUScenePrimitiveRow* primitiveRow = m_database.GetRow(*primitive);
    if (primitiveRow == nullptr ||
        !IsLiveSchemaHeader(primitiveRow->header,
                            primitive->generation,
                            packet.objectId) ||
        !m_database.IsLive(primitiveRow->bounds) ||
        !m_database.IsLive(primitiveRow->transform) ||
        primitiveRow->drawCount == 0 || !primitiveRow->firstDraw.IsValid())
    {
        return std::nullopt;
    }

    const GPUSceneBoundsRow* boundsRow = m_database.GetRow(primitiveRow->bounds);
    const GPUSceneTransformRow* transformRow =
        m_database.GetRow(primitiveRow->transform);
    if (boundsRow == nullptr || transformRow == nullptr ||
        !IsLiveSchemaHeader(boundsRow->header,
                            primitiveRow->bounds.generation,
                            packet.objectId) ||
        !IsLiveSchemaHeader(transformRow->header,
                            primitiveRow->transform.generation,
                            packet.objectId))
    {
        return std::nullopt;
    }

    const GPUSceneCommittedMirror& mirror = m_database.GetCommittedMirror();
    const uint64 firstDrawSlot = primitiveRow->firstDraw.slot;
    const uint64 drawEnd = firstDrawSlot + primitiveRow->drawCount;
    if (firstDrawSlot == 0 || drawEnd < firstDrawSlot ||
        drawEnd > mirror.draws.size())
    {
        return std::nullopt;
    }

    const uint32 requiredPassMask = GetGPUScenePassMask(packet.pass);
    if (requiredPassMask == 0)
    {
        return std::nullopt;
    }

    std::optional<GPUSceneAcceptedDrawLookup> result;
    for (uint64 slot = firstDrawSlot; slot < drawEnd; ++slot)
    {
        const GPUSceneDrawRef draw{
            static_cast<uint32>(slot), primitiveRow->firstDraw.generation};
        if (!m_database.IsLive(draw))
        {
            return std::nullopt;
        }
        const GPUSceneDrawMetadataRow* drawRow = m_database.GetRow(draw);
        if (drawRow == nullptr ||
            !IsLiveSchemaHeader(drawRow->header, draw.generation, packet.objectId) ||
            drawRow->primitive != *primitive ||
            !m_database.IsLive(drawRow->material) ||
            !m_database.IsLive(drawRow->geometry))
        {
            return std::nullopt;
        }

        const GPUSceneMaterialRow* materialRow =
            m_database.GetRow(drawRow->material);
        const GPUSceneGeometryRow* geometryRow =
            m_database.GetRow(drawRow->geometry);
        if (materialRow == nullptr || geometryRow == nullptr ||
            !IsLiveSchemaHeader(materialRow->header,
                                drawRow->material.generation,
                                packet.objectId) ||
            !IsLiveSchemaHeader(geometryRow->header,
                                drawRow->geometry.generation,
                                packet.objectId))
        {
            return std::nullopt;
        }

        const bool packetMatchesRow =
            (drawRow->passMask & requiredPassMask) != 0 &&
            geometryRow->resourceSlot == packet.geometryKey.mesh.slot &&
            geometryRow->resourceGeneration == packet.geometryKey.mesh.generation &&
            geometryRow->submeshIndex == packet.submeshIndex &&
            geometryRow->indexCount == packet.arguments.indexCount &&
            geometryRow->firstIndex == packet.arguments.firstIndex &&
            geometryRow->vertexOffset == packet.arguments.vertexOffset &&
            materialRow->resourceSlot == packet.materialKey.material.slot &&
            materialRow->resourceGeneration == packet.materialKey.material.generation &&
            drawRow->indexCount == packet.arguments.indexCount &&
            drawRow->firstIndex == packet.arguments.firstIndex &&
            drawRow->vertexOffset == packet.arguments.vertexOffset;
        if (!packetMatchesRow)
        {
            continue;
        }

        if (result)
        {
            // A packet must map to one exact draw row. Ambiguous duplicate
            // batches are intentionally not guessed by ordinal.
            return std::nullopt;
        }
        result = GPUSceneAcceptedDrawLookup{
            m_database.GetCommittedVersion(), *primitive, draw};
    }
    return result;
}

void GPUSceneUpdate::RecordFailure(
    uint64 sourceSequence,
    GPUScenePublicationFailureReason reason) noexcept
{
    GPUScenePublicationStats stats;
    stats.attempted = true;
    stats.sourceSequence = sourceSequence;
    PopulateCommittedIdentity(stats);
    stats.failureReason = reason;
    stats.complete = false;
    stats.executionEligible = false;
    m_stats = stats;
}

void GPUSceneUpdate::RecordUnexpectedFailure(uint64 sourceSequence) noexcept
{
    RecordFailure(sourceSequence, GPUScenePublicationFailureReason::UnexpectedFailure);
}

GPUScenePublicationStats GPUSceneUpdate::Publish(
    const RenderScene& scene,
    const RenderResourceRegistry& registry)
{
    try
    {
        if (m_throwOnPublishForTesting)
        {
            throw std::bad_alloc();
        }
        return PublishImpl(scene, registry);
    }
    catch (const std::bad_alloc&)
    {
        RecordFailure(
            scene.GetAcceptedHeader().sequence,
            GPUScenePublicationFailureReason::AllocationFailed);
    }
    catch (...)
    {
        RecordUnexpectedFailure(scene.GetAcceptedHeader().sequence);
    }
    return m_stats;
}

bool GPUSceneUpdate::IsEquivalent(
    const PublishedObject& lhs,
    const PublishedObject& rhs) const noexcept
{
    if (lhs.data.objectId != rhs.data.objectId ||
        lhs.data.primitiveFlags != rhs.data.primitiveFlags ||
        lhs.data.layerMask != rhs.data.layerMask ||
        lhs.data.sortKey != rhs.data.sortKey ||
        lhs.requiredResources != rhs.requiredResources ||
        lhs.data.draws.size() != rhs.data.draws.size())
    {
        return false;
    }

    const GPUSceneBoundsRow& leftBounds = lhs.data.bounds;
    const GPUSceneBoundsRow& rightBounds = rhs.data.bounds;
    if (leftBounds.minimum != rightBounds.minimum ||
        leftBounds.maximum != rightBounds.maximum ||
        leftBounds.sphere != rightBounds.sphere ||
        leftBounds.boundsFlags != rightBounds.boundsFlags)
    {
        return false;
    }

    const GPUSceneTransformRow& leftTransform = lhs.data.transform;
    const GPUSceneTransformRow& rightTransform = rhs.data.transform;
    if (leftTransform.worldFromLocal != rightTransform.worldFromLocal ||
        leftTransform.previousWorldFromLocal != rightTransform.previousWorldFromLocal ||
        leftTransform.normalFromLocal != rightTransform.normalFromLocal ||
        leftTransform.transformFlags != rightTransform.transformFlags)
    {
        return false;
    }

    for (size_t index = 0; index < lhs.data.draws.size(); ++index)
    {
        const GPUSceneDrawData& left = lhs.data.draws[index];
        const GPUSceneDrawData& right = rhs.data.draws[index];
        const GPUSceneMaterialRow& leftMaterial = left.material;
        const GPUSceneMaterialRow& rightMaterial = right.material;
        const GPUSceneGeometryRow& leftGeometry = left.geometry;
        const GPUSceneGeometryRow& rightGeometry = right.geometry;
        const GPUSceneDrawMetadataRow& leftDraw = left.draw;
        const GPUSceneDrawMetadataRow& rightDraw = right.draw;
        if (leftMaterial.resourceSlot != rightMaterial.resourceSlot ||
            leftMaterial.resourceGeneration != rightMaterial.resourceGeneration ||
            leftMaterial.materialId != rightMaterial.materialId ||
            leftMaterial.materialFlags != rightMaterial.materialFlags ||
            leftMaterial.shadingModel != rightMaterial.shadingModel ||
            leftMaterial.baseColor != rightMaterial.baseColor ||
            leftMaterial.metallic != rightMaterial.metallic ||
            leftMaterial.roughness != rightMaterial.roughness ||
            leftMaterial.emissiveIntensity != rightMaterial.emissiveIntensity ||
            leftMaterial.opacity != rightMaterial.opacity ||
            leftGeometry.resourceSlot != rightGeometry.resourceSlot ||
            leftGeometry.resourceGeneration != rightGeometry.resourceGeneration ||
            leftGeometry.geometryId != rightGeometry.geometryId ||
            leftGeometry.submeshIndex != rightGeometry.submeshIndex ||
            leftGeometry.firstIndex != rightGeometry.firstIndex ||
            leftGeometry.vertexOffset != rightGeometry.vertexOffset ||
            leftGeometry.indexCount != rightGeometry.indexCount ||
            leftGeometry.topology != rightGeometry.topology ||
            leftGeometry.geometryFlags != rightGeometry.geometryFlags ||
            leftDraw.indexCount != rightDraw.indexCount ||
            leftDraw.instanceCount != rightDraw.instanceCount ||
            leftDraw.firstIndex != rightDraw.firstIndex ||
            leftDraw.vertexOffset != rightDraw.vertexOffset ||
            leftDraw.firstInstance != rightDraw.firstInstance ||
            leftDraw.passMask != rightDraw.passMask ||
            leftDraw.materialVariant != rightDraw.materialVariant ||
            leftDraw.pipelineKey != rightDraw.pipelineKey ||
            leftDraw.sortKey != rightDraw.sortKey)
        {
            return false;
        }
    }
    return true;
}

bool GPUSceneUpdate::AreExactDependenciesReady(
    const std::vector<RenderResourceHandle>& roots,
    const RenderResourceRegistry& registry,
    GPUScenePublicationFailureReason& outReason) const
{
    enum class VisitState : uint8
    {
        Visiting = 0,
        Complete,
    };
    std::unordered_map<RenderResourceHandle, VisitState, RenderResourceHandleHash>
        visits;

    const std::function<bool(RenderResourceHandle, bool)> visit =
        [&](RenderResourceHandle handle, bool dependency)
    {
        if (!handle.IsValid() || !registry.IsGPUReadyExact(handle))
        {
            outReason = dependency
                            ? GPUScenePublicationFailureReason::DependencyUnavailable
                            : GPUScenePublicationFailureReason::ResourceUnavailable;
            return false;
        }
        const auto found = visits.find(handle);
        if (found != visits.end())
        {
            if (found->second == VisitState::Visiting)
            {
                outReason = GPUScenePublicationFailureReason::DependencyCycle;
                return false;
            }
            return true;
        }
        visits.emplace(handle, VisitState::Visiting);
        const std::vector<RenderResourceHandle>* dependencies =
            registry.GetDependencies(handle);
        if (dependencies == nullptr)
        {
            outReason = GPUScenePublicationFailureReason::DependencyUnavailable;
            return false;
        }
        for (RenderResourceHandle child : *dependencies)
        {
            if (!visit(child, true))
            {
                return false;
            }
        }
        visits.find(handle)->second = VisitState::Complete;
        return true;
    };

    for (RenderResourceHandle root : roots)
    {
        if (!visit(root, false))
        {
            return false;
        }
    }
    return true;
}

GPUScenePublicationStats GPUSceneUpdate::PublishImpl(
    const RenderScene& scene,
    const RenderResourceRegistry& registry)
{
    GPUScenePublicationStats stats;
    stats.attempted = true;
    stats.sourceSequence = scene.GetAcceptedHeader().sequence;
    stats.attemptedObjectCount = static_cast<uint32>(scene.GetObjectCount());
    stats.acceptedObjectCount = stats.attemptedObjectCount;
    PopulateCommittedIdentity(stats);

    std::unordered_map<uint64, PublishedObject> candidateObjects;
    candidateObjects.reserve(scene.GetObjectCount());
    for (const RenderObject& source : scene.GetObjects())
    {
        const uint32 sourceDrawCount =
            static_cast<uint32>(source.meshBatches.size());
        stats.attemptedDrawCount += sourceDrawCount;
        stats.acceptedDrawCount = stats.attemptedDrawCount;
        BuildPublishedObjectResult built = BuildPublishedObject(source, registry);
        if (!built.Succeeded())
        {
            ++stats.excludedObjectCount;
            stats.excludedDrawCount += sourceDrawCount;
            if (stats.failureReason == GPUScenePublicationFailureReason::None)
            {
                stats.failureReason = built.failureReason;
            }
            continue;
        }

        GPUScenePublicationFailureReason dependencyFailure =
            GPUScenePublicationFailureReason::None;
        if (!AreExactDependenciesReady(
                built.object.requiredResources, registry, dependencyFailure))
        {
            ++stats.excludedObjectCount;
            stats.excludedDrawCount += sourceDrawCount;
            if (stats.failureReason == GPUScenePublicationFailureReason::None)
            {
                stats.failureReason = dependencyFailure;
            }
            continue;
        }

        const uint32 drawCount =
            static_cast<uint32>(built.object.data.draws.size());
        const auto [inserted, wasInserted] = candidateObjects.emplace(
            source.entityId, std::move(built.object));
        static_cast<void>(inserted);
        if (!wasInserted)
        {
            ++stats.excludedObjectCount;
            stats.excludedDrawCount += drawCount;
            if (stats.failureReason == GPUScenePublicationFailureReason::None)
            {
                stats.failureReason = GPUScenePublicationFailureReason::InvalidObject;
            }
            continue;
        }
        stats.candidateDrawCount += drawCount;
    }
    stats.candidateObjectCount = static_cast<uint32>(candidateObjects.size());

    GPUSceneTransaction transaction;
    for (const auto& [objectId, candidate] : candidateObjects)
    {
        const auto existing = m_publishedObjects.find(objectId);
        if (existing == m_publishedObjects.end())
        {
            transaction.Add(candidate.data);
            ++stats.addCount;
        }
        else if (IsEquivalent(existing->second, candidate))
        {
            ++stats.noOpCount;
        }
        else
        {
            const std::optional<GPUScenePrimitiveRef> primitive =
                m_database.FindPrimitive(objectId);
            if (!primitive)
            {
                stats.failureReason =
                    GPUScenePublicationFailureReason::DatabaseCommitFailed;
                stats.complete = false;
                stats.executionEligible = false;
                m_stats = stats;
                return m_stats;
            }
            transaction.Update(*primitive, candidate.data);
            ++stats.updateCount;
        }
    }
    for (const auto& [objectId, existing] : m_publishedObjects)
    {
        static_cast<void>(existing);
        if (candidateObjects.contains(objectId))
        {
            continue;
        }
        const std::optional<GPUScenePrimitiveRef> primitive =
            m_database.FindPrimitive(objectId);
        if (!primitive)
        {
            stats.failureReason =
                GPUScenePublicationFailureReason::DatabaseCommitFailed;
            stats.complete = false;
            stats.executionEligible = false;
            m_stats = stats;
            return m_stats;
        }
        transaction.Remove(*primitive);
        ++stats.removeCount;
    }

    const GPUSceneCommitResult committed = m_database.Commit(transaction);
    if (!committed.Succeeded())
    {
        stats.failureReason =
            committed.status == GPUSceneCommitStatus::AllocationFailed
                ? GPUScenePublicationFailureReason::AllocationFailed
                : GPUScenePublicationFailureReason::DatabaseCommitFailed;
        PopulateCommittedIdentity(stats);
        stats.complete = false;
        stats.executionEligible = false;
        m_stats = stats;
        return m_stats;
    }

    m_publishedObjects.swap(candidateObjects);
    m_committedSourceSequence = stats.sourceSequence;
    PopulateCommittedIdentity(stats);
    stats.complete = stats.excludedObjectCount == 0 &&
                     stats.excludedDrawCount == 0;
    // Task 11B intentionally has no upload/binding/command-generation path.
    stats.executionEligible = false;
    m_stats = stats;
    return m_stats;
}

GPUScenePublicationStats GPUSceneUpdate::Revalidate(
    const RenderResourceRegistry& registry)
{
    GPUScenePublicationStats stats = m_stats;
    PopulateCommittedIdentity(stats);

    for (const auto& [objectId, object] : m_publishedObjects)
    {
        static_cast<void>(objectId);
        GPUScenePublicationFailureReason failure =
            GPUScenePublicationFailureReason::None;
        if (!AreExactDependenciesReady(object.requiredResources, registry, failure))
        {
            stats.failureReason = failure;
            stats.complete = false;
            stats.executionEligible = false;
            m_stats = stats;
            return m_stats;
        }
    }
    if (stats.attemptedObjectCount == stats.publishedObjectCount &&
        stats.attemptedDrawCount == stats.publishedDrawCount &&
        stats.excludedObjectCount == 0 &&
        stats.excludedDrawCount == 0 &&
        (stats.failureReason == GPUScenePublicationFailureReason::ResourceUnavailable ||
         stats.failureReason == GPUScenePublicationFailureReason::DependencyUnavailable ||
         stats.failureReason == GPUScenePublicationFailureReason::DependencyCycle ||
         stats.failureReason == GPUScenePublicationFailureReason::ResourceResolutionFailed))
    {
        stats.failureReason = GPUScenePublicationFailureReason::None;
        stats.complete = true;
    }
    stats.executionEligible = false;
    m_stats = stats;
    return m_stats;
}

void GPUSceneUpdate::Clear() noexcept
{
    m_database.Clear();
    m_publishedObjects.clear();
    m_stats = {};
    m_committedSourceSequence = 0;
    PopulateCommittedIdentity(m_stats);
}
} // namespace RVX
