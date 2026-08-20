/**
 * @file RenderScene.cpp
 * @brief RenderScene transactional packet application.
 */

#include "Render/Renderer/RenderScene.h"

#include "Core/Math/Frustum.h"
#include "Render/Renderer/RenderSceneDatabase.h"
#include "Resources/RenderResourceRegistry.h"

#include <algorithm>
#include <limits>
#include <unordered_set>
#include <utility>

namespace RVX
{
namespace
{
    constexpr uint32 PRIMITIVE_VISIBLE = 1U << 0U;
    constexpr uint32 PRIMITIVE_CASTS_SHADOW = 1U << 1U;
    constexpr uint32 PRIMITIVE_RECEIVES_SHADOW = 1U << 2U;
    constexpr uint32 PRIMITIVE_MATERIAL_MODE_SHIFT = 8U;

    void SaturatingAdd(uint64& total, uint64 value, bool& saturated) noexcept
    {
        const uint64 maximum = std::numeric_limits<uint64>::max();
        if (saturated || value > maximum - total)
        {
            total = maximum;
            saturated = true;
            return;
        }
        total += value;
    }

    RenderLight::Type ToRenderLightType(RenderLightType type)
    {
        switch (type)
        {
            case RenderLightType::Point:
                return RenderLight::Type::Point;
            case RenderLightType::Spot:
                return RenderLight::Type::Spot;
            case RenderLightType::Directional:
            default:
                return RenderLight::Type::Directional;
        }
    }

    void AddUniqueHandle(std::vector<RenderResourceHandle>& handles,
                         RenderResourceHandle handle)
    {
        if (!handle.IsValid() ||
            std::find(handles.begin(), handles.end(), handle) != handles.end())
        {
            return;
        }
        handles.push_back(handle);
    }

    void AddWatchedResourceClosure(
        std::unordered_map<RenderResourceHandle,
                           uint64,
                           RenderResourceHandleHash>& watched,
        RenderResourceHandle handle,
        const RenderResourceRegistry& registry)
    {
        if (!handle.IsValid() || watched.contains(handle))
            return;

        watched.emplace(handle, registry.GetContentRevision(handle));
        const std::vector<RenderResourceHandle>* dependencies =
            registry.GetDependencies(handle);
        if (dependencies == nullptr)
            return;
        for (RenderResourceHandle dependency : *dependencies)
            AddWatchedResourceClosure(watched, dependency, registry);
    }

    bool AreWatchedResourcesCurrent(
        const std::unordered_map<RenderResourceHandle,
                                 uint64,
                                 RenderResourceHandleHash>& watched,
        const RenderResourceRegistry& registry,
        std::vector<std::pair<RenderResourceHandle, uint64>>&
            contentOnlyTextureRefreshes)
    {
        for (const auto& [handle, watchedRevision] : watched)
        {
            const uint64 currentRevision = registry.GetContentRevision(handle);
            if (currentRevision == watchedRevision)
                continue;

            // Texture content is resolved by the material/sky binding at draw
            // time. An exact-generation replacement therefore preserves the
            // extracted scene and its packet identity, but only if it cannot
            // have changed a resource topology below the texture itself.
            // Every other watched change remains a fail-closed rebuild.
            const std::vector<RenderResourceHandle>* dependencies =
                registry.GetDependencies(handle);
            if (registry.GetExactKind(handle) != RenderResourceKind::Texture ||
                !registry.IsGPUReadyExact(handle) || dependencies == nullptr ||
                !dependencies->empty())
            {
                return false;
            }
            contentOnlyTextureRefreshes.emplace_back(handle, currentRevision);
        }
        return true;
    }

    void ApplyContentOnlyTextureRefreshes(
        std::unordered_map<RenderResourceHandle,
                           uint64,
                           RenderResourceHandleHash>& watched,
        const std::vector<std::pair<RenderResourceHandle, uint64>>& refreshes)
    {
        for (const auto& [handle, revision] : refreshes)
            watched.insert_or_assign(handle, revision);
    }

    RenderResourceHandle SelectOptional(
        RenderResourceHandle preferred,
        RenderResourceHandle fallback,
        const RenderResourceRegistry& registry,
        uint32& fallbackCount)
    {
        if (preferred.IsValid() && registry.IsGPUReadyExact(preferred))
        {
            return preferred;
        }
        if (fallback.IsValid() && registry.IsGPUReadyExact(fallback))
        {
            ++fallbackCount;
            return fallback;
        }
        return {};
    }

    bool IsValidMaterialMode(RenderMaterialMode mode)
    {
        return mode == RenderMaterialMode::Opaque ||
               mode == RenderMaterialMode::Masked ||
               mode == RenderMaterialMode::Transparent;
    }

    RenderMaterialMode GetLegacyMaterialMode(
        const RenderPrimitiveSnapshot& primitive)
    {
        const uint32 materialMode =
            (primitive.flags >> PRIMITIVE_MATERIAL_MODE_SHIFT) & 0xFFU;
        return materialMode <= static_cast<uint32>(RenderMaterialMode::Transparent)
                   ? static_cast<RenderMaterialMode>(materialMode)
                   : RenderMaterialMode::Opaque;
    }

    bool IsSkinned(const RenderPrimitiveSnapshot& primitive) noexcept
    {
        // Vertex bone attributes describe what a mesh can support, not what
        // this object is actually executing.  The skinned pipeline requires
        // a per-object palette provider or matrices for this publication.
        return primitive.hasSkinningPaletteProvider ||
               !primitive.skinMatrices.empty();
    }

    bool MatricesEqual(const Mat4& left, const Mat4& right) noexcept
    {
        for (uint32 column = 0; column < 4; ++column)
        {
            for (uint32 row = 0; row < 4; ++row)
            {
                if (left[column][row] != right[column][row])
                    return false;
            }
        }
        return true;
    }

    template<typename Map>
    std::vector<typename Map::mapped_type> CopySortedSceneValues(
        const Map& values)
    {
        using Key = typename Map::key_type;
        using State = typename Map::mapped_type;
        std::vector<std::pair<Key, const State*>> ordered;
        ordered.reserve(values.size());
        for (const auto& [id, state] : values)
        {
            ordered.emplace_back(id, &state);
        }
        std::sort(ordered.begin(), ordered.end(),
                  [](const auto& left, const auto& right)
                  {
                      return left.first < right.first;
                  });
        std::vector<State> result;
        result.reserve(ordered.size());
        for (const auto& [id, state] : ordered)
        {
            static_cast<void>(id);
            result.push_back(*state);
        }
        return result;
    }

    bool BuildRetainedObject(
        const RenderPrimitiveSnapshot& primitive,
        PrimitiveDataIndex primitiveData,
        uint64 objectRevision,
        bool resetHistory,
        const std::unordered_map<uint64, Mat4>& lastRenderedTransforms,
        const RenderResourceRegistry& registry,
        RenderFrameApplyResult& result,
        RenderObject& object,
        std::unordered_map<RenderResourceHandle,
                           uint64,
                           RenderResourceHandleHash>& watchedResources)
    {
        AddWatchedResourceClosure(watchedResources, primitive.mesh, registry);
        AddWatchedResourceClosure(watchedResources,
                                  primitive.fallbackMesh,
                                  registry);
        AddWatchedResourceClosure(watchedResources,
                                  primitive.material,
                                  registry);
        AddWatchedResourceClosure(watchedResources,
                                  primitive.fallbackMaterial,
                                  registry);
        for (const RenderSubmeshMaterialBinding& binding : primitive.submeshes)
            AddWatchedResourceClosure(watchedResources, binding.material, registry);

        const RenderResourceStatus meshStatus = registry.QueryStatus(primitive.mesh);
        if (primitive.objectId == 0 || !primitive.mesh.IsValid() ||
            meshStatus.code != RenderResourceStatusCode::Current)
        {
            result.code = RenderFrameApplyCode::StaleRequiredHandle;
            return false;
        }
        if (primitive.hasSkinningPaletteProvider &&
            !primitive.HasValidSkinningPalette())
        {
            result.code = RenderFrameApplyCode::InvalidPacket;
            return false;
        }

        object = {};
        object.entityId = primitive.objectId;
        object.objectRevision = objectRevision;
        object.worldMatrix = primitive.worldTransform;
        object.previousWorldMatrix = primitive.worldTransform;
        if (!resetHistory)
        {
            const auto previous = lastRenderedTransforms.find(primitive.objectId);
            if (previous != lastRenderedTransforms.end())
            {
                object.previousWorldMatrix = previous->second;
                object.previousWorldMatrixValid = 1;
            }
        }
        object.normalMatrix = glm::inverseTranspose(Mat4(Mat3(object.worldMatrix)));
        object.bounds = AABB(primitive.boundsMin, primitive.boundsMax);
        object.fallbackMesh = primitive.fallbackMesh;
        object.fallbackMaterial = primitive.fallbackMaterial;
        object.mesh = SelectOptional(primitive.mesh,
                                     primitive.fallbackMesh,
                                     registry,
                                     result.pendingFallbackCount);
        object.skinningMatrices = primitive.skinMatrices;
        object.hasSkinningPaletteProvider =
            primitive.hasSkinningPaletteProvider;
        object.skinningPalette = primitive.skinningPalette;
        object.sortKey = primitive.sortKey;
        object.layerMask = primitive.layerMask;
        object.flags = primitive.flags;
        object.visible = (primitive.flags & PRIMITIVE_VISIBLE) != 0;
        object.castsShadow = (primitive.flags & PRIMITIVE_CASTS_SHADOW) != 0;
        object.receivesShadow =
            (primitive.flags & PRIMITIVE_RECEIVES_SHADOW) != 0;
        object.drawable = object.mesh.IsValid();

        const bool usingPreferredMesh = object.mesh == primitive.mesh;
        const RenderMeshResourceData* meshData = object.mesh.IsValid()
            ? registry.ResolveMesh(object.mesh)
            : nullptr;
        object.meshBatchesAuthoritative = meshData != nullptr;
        const bool hasExplicitBindings = !primitive.submeshes.empty();
        if (usingPreferredMesh && hasExplicitBindings && meshData == nullptr)
        {
            result.code = RenderFrameApplyCode::InvalidPacket;
            return false;
        }

        std::vector<MeshUploadSubmesh> actualSubmeshes;
        if (meshData != nullptr)
        {
            actualSubmeshes = meshData->submeshes;
            if (actualSubmeshes.empty() && meshData->createInfo.indexCount != 0)
            {
                if (meshData->createInfo.indexCount >
                    std::numeric_limits<uint32>::max())
                {
                    result.code = RenderFrameApplyCode::InvalidPacket;
                    return false;
                }
                actualSubmeshes.push_back(MeshUploadSubmesh{
                    0,
                    static_cast<uint32>(meshData->createInfo.indexCount),
                    0,
                    meshData->createInfo.topology});
            }
        }

        if (usingPreferredMesh && hasExplicitBindings)
        {
            if (primitive.submeshes.size() != actualSubmeshes.size())
            {
                result.code = RenderFrameApplyCode::InvalidPacket;
                return false;
            }
            for (size_t index = 0; index < primitive.submeshes.size(); ++index)
            {
                const RenderSubmeshMaterialBinding& binding =
                    primitive.submeshes[index];
                if (binding.submeshIndex != index ||
                    !IsValidMaterialMode(binding.materialMode))
                {
                    result.code = RenderFrameApplyCode::InvalidPacket;
                    return false;
                }
            }
        }

        const RenderMaterialMode legacyMode = GetLegacyMaterialMode(primitive);
        std::vector<RenderResourceHandle> selectedMaterials;
        std::vector<RenderMaterialMode> selectedModes;
        selectedMaterials.reserve(actualSubmeshes.size());
        selectedModes.reserve(actualSubmeshes.size());
        if (meshData != nullptr && usingPreferredMesh && hasExplicitBindings)
        {
            for (const RenderSubmeshMaterialBinding& binding : primitive.submeshes)
            {
                selectedMaterials.push_back(SelectOptional(
                    binding.material,
                    primitive.fallbackMaterial,
                    registry,
                    result.pendingFallbackCount));
                selectedModes.push_back(binding.materialMode);
            }
        }
        else
        {
            const RenderResourceHandle legacyMaterial = SelectOptional(
                primitive.material,
                primitive.fallbackMaterial,
                registry,
                result.pendingFallbackCount);
            if (actualSubmeshes.empty())
            {
                object.material = legacyMaterial;
            }
            else
            {
                selectedMaterials.assign(actualSubmeshes.size(), legacyMaterial);
                selectedModes.assign(actualSubmeshes.size(), legacyMode);
            }
        }

        if (!selectedMaterials.empty())
        {
            object.material = selectedMaterials.front();
            object.materialModes = selectedModes;
        }

        if (meshData != nullptr && !actualSubmeshes.empty())
        {
            MeshBatchBuildInput batchInput;
            batchInput.objectId = object.entityId;
            batchInput.objectRevision = object.objectRevision;
            batchInput.mesh = object.mesh;
            batchInput.primitiveData = primitiveData;
            batchInput.indexType = meshData->createInfo.indexType;
            batchInput.boundsMin = primitive.boundsMin;
            batchInput.boundsMax = primitive.boundsMax;
            if (IsSkinned(primitive))
                batchInput.flags |= RenderBatchFlags::Skinned;
            if (object.castsShadow)
                batchInput.flags |= RenderBatchFlags::CastsShadow;
            if (object.receivesShadow)
                batchInput.flags |= RenderBatchFlags::ReceivesShadow;
            batchInput.submeshes.reserve(actualSubmeshes.size());
            for (size_t index = 0; index < actualSubmeshes.size(); ++index)
            {
                batchInput.submeshes.push_back(MeshBatchSourceSubmesh{
                    static_cast<uint32>(index),
                    actualSubmeshes[index],
                    selectedMaterials[index],
                    selectedModes[index]});
            }
            MeshBatchBuildResult batchResult = BuildMeshBatches(batchInput);
            if (!batchResult.IsSuccess())
            {
                result.code = RenderFrameApplyCode::InvalidPacket;
                return false;
            }
            object.meshBatches = std::move(batchResult.batches);
        }
        if (!object.drawable)
            ++result.skippedDrawCount;
        AddUniqueHandle(object.referencedResources, object.mesh);
        for (const MeshBatch& batch : object.meshBatches)
            AddUniqueHandle(object.referencedResources, batch.material);
        AddUniqueHandle(object.referencedResources, object.material);
        return true;
    }

    bool BuildRetainedLight(
        const RenderLightSnapshot& snapshot,
        const RenderResourceRegistry& registry,
        RenderFrameApplyResult& result,
        RenderLight& light,
        std::unordered_map<RenderResourceHandle,
                           uint64,
                           RenderResourceHandleHash>& watchedResources)
    {
        AddWatchedResourceClosure(watchedResources,
                                  snapshot.shadowResource,
                                  registry);
        if (snapshot.lightId == 0 ||
            (snapshot.shadowResource.IsValid() &&
             !registry.HasExactEntry(snapshot.shadowResource)))
        {
            result.code = RenderFrameApplyCode::InvalidPacket;
            return false;
        }
        light = {};
        light.lightId = snapshot.lightId;
        light.type = ToRenderLightType(snapshot.type);
        light.position = snapshot.position;
        light.direction = snapshot.direction;
        light.color = snapshot.color;
        light.intensity = snapshot.intensity;
        light.range = snapshot.range;
        light.innerConeAngle = snapshot.innerConeRadians;
        light.outerConeAngle = snapshot.outerConeRadians;
        light.layerMask = snapshot.layerMask;
        light.castsShadow = snapshot.castsShadows;
        light.shadowResource = snapshot.shadowResource.IsValid() &&
                                       registry.IsGPUReadyExact(
                                           snapshot.shadowResource)
            ? snapshot.shadowResource
            : RenderResourceHandle{};
        AddUniqueHandle(light.referencedResources, light.shadowResource);
        return true;
    }
} // namespace

void RenderScene::Clear()
{
    m_objects.clear();
    m_lights.clear();
    m_objectIndices.clear();
    m_lightIndices.clear();
    m_acceptedHeader = {};
    m_view = {};
    m_sky = {};
    m_environment = {};
    m_settings = {};
    m_captureRequest = {};
    m_features.Clear();
    m_referencedResources.clear();
    m_referenceCounts.clear();
    m_referenceIndices.clear();
    m_skyReferences.clear();
    m_environmentReferences.clear();
    m_watchedResourceRevisions.clear();
    m_drawPacketCache.Clear();
    m_retainedStats = {};
    m_appliedSceneRevision = 0;
    m_sourceSceneDatabaseId = 0;
    m_observedResourceContentRevision = 0;
    m_drawCount = 0;
    m_gpuSceneChangedObjectIds.clear();
    m_gpuSceneRemovedObjectIds.clear();
    m_pendingRenderedObjectIds.clear();
    m_pendingRemovedObjectIds.clear();
    m_temporalSettleObjectIds.clear();
    m_fullGPUSceneMutation = false;
    m_fullRenderedObjectRefresh = false;
    m_hasAcceptedFrame = false;
    m_temporalHistoryReset = true;
    m_requiresTemporalSettle = false;
    m_acceptedSceneMutated = false;
    m_lastRenderedHeader = {};
    m_lastRenderedView = {};
    m_lastRenderedObjectTransforms.clear();
    m_lastRenderedSurfaceCompatibilityKey = m_surfaceCompatibilityKey;
}

RenderFrameApplyResult RenderScene::ApplyFrameV5(
    const RenderFramePacketV5& frame,
    const RenderSceneDatabase& scene,
    const RenderResourceRegistry& registry)
{
    RenderFrameApplyResult result;
    const RenderFrameHeaderV5& frameHeader = frame.GetHeader();
    result.sequence = frameHeader.sequence;
    result.previousAcceptedSequence = m_acceptedHeader.sequence;
    result.lastRenderedSequence = m_lastRenderedHeader.sequence;
    if (frameHeader.schemaId != RVX_RENDER_FRAME_PACKET_V5_SCHEMA_ID ||
        frameHeader.schemaVersion != RVX_RENDER_FRAME_PACKET_V5_SCHEMA_VERSION)
    {
        result.code = RenderFrameApplyCode::UnsupportedSchema;
        return result;
    }
    if (scene.GetRevision() < frameHeader.requiredSceneRevision)
    {
        result.code = RenderFrameApplyCode::InvalidPacket;
        return result;
    }
    if (frameHeader.sequence == 0 ||
        (m_hasAcceptedFrame &&
         frameHeader.sequence <= m_acceptedHeader.sequence))
    {
        result.code = RenderFrameApplyCode::OutOfOrder;
        return result;
    }

    const bool resetHistory =
        m_lastRenderedHeader.sequence == 0 ||
        frameHeader.worldRevision != m_lastRenderedHeader.worldRevision ||
        frameHeader.temporalEpoch != m_lastRenderedHeader.temporalEpoch ||
        frameHeader.explicitDiscontinuity ||
        m_surfaceCompatibilityKey != m_lastRenderedSurfaceCompatibilityKey;
    const uint64 resourceContentRevision = registry.GetContentRevision();
    bool watchedResourcesCurrent = true;
    std::vector<std::pair<RenderResourceHandle, uint64>>
        contentOnlyTextureRefreshes;
    if (resourceContentRevision != m_observedResourceContentRevision)
    {
        watchedResourcesCurrent = AreWatchedResourcesCurrent(
            m_watchedResourceRevisions,
            registry,
            contentOnlyTextureRefreshes);
    }
    const bool sameSceneDatabase =
        scene.GetInstanceId() == m_sourceSceneDatabaseId;
    if (m_hasAcceptedFrame && sameSceneDatabase && !resetHistory &&
        m_requiresTemporalSettle &&
        !m_temporalSettleObjectIds.empty() &&
        scene.GetRevision() == m_appliedSceneRevision &&
        watchedResourcesCurrent)
    {
        m_gpuSceneChangedObjectIds.clear();
        m_gpuSceneRemovedObjectIds.clear();
        m_fullGPUSceneMutation = false;
        for (uint64 objectId : m_temporalSettleObjectIds)
        {
            const auto found = m_objectIndices.find(objectId);
            if (found == m_objectIndices.end())
                continue;
            RenderObject& object = m_objects[found->second];
            object.previousWorldMatrix = object.worldMatrix;
            object.previousWorldMatrixValid = 1;
            m_gpuSceneChangedObjectIds.push_back(objectId);
        }
        m_temporalSettleObjectIds.clear();
        m_requiresTemporalSettle = false;
        m_acceptedHeader = frameHeader;
        m_view = frame.GetView();
        m_settings = frame.GetSettings();
        m_captureRequest = frame.GetCaptureRequest();
        m_temporalHistoryReset = false;
        m_acceptedSceneMutated = !m_gpuSceneChangedObjectIds.empty();
        ApplyContentOnlyTextureRefreshes(m_watchedResourceRevisions,
                                         contentOnlyTextureRefreshes);
        m_observedResourceContentRevision = resourceContentRevision;
        m_retainedStats.lastRebuiltObjectCount = 0;
        m_retainedStats.lastRemovedObjectCount = 0;

        result.code = RenderFrameApplyCode::Applied;
        result.temporalHistoryReset = false;
        result.sceneMutated = m_acceptedSceneMutated;
        if (m_lastRenderedHeader.sequence != 0 &&
            frameHeader.sequence > m_lastRenderedHeader.sequence + 1)
        {
            result.sequenceGap =
                frameHeader.sequence - m_lastRenderedHeader.sequence - 1;
        }
        return result;
    }
    if (m_hasAcceptedFrame && sameSceneDatabase && !resetHistory &&
        !m_requiresTemporalSettle &&
        scene.GetRevision() == m_appliedSceneRevision &&
        watchedResourcesCurrent)
    {
        m_acceptedHeader = frameHeader;
        m_view = frame.GetView();
        m_settings = frame.GetSettings();
        m_captureRequest = frame.GetCaptureRequest();
        m_temporalHistoryReset = false;
        m_acceptedSceneMutated = false;
        m_fullGPUSceneMutation = false;
        m_gpuSceneChangedObjectIds.clear();
        m_gpuSceneRemovedObjectIds.clear();
        ApplyContentOnlyTextureRefreshes(m_watchedResourceRevisions,
                                         contentOnlyTextureRefreshes);
        m_observedResourceContentRevision = resourceContentRevision;
        ++m_retainedStats.staticReuseCount;
        m_retainedStats.lastRebuiltObjectCount = 0;
        m_retainedStats.lastRemovedObjectCount = 0;

        result.code = RenderFrameApplyCode::Applied;
        result.temporalHistoryReset = false;
        result.sceneMutated = false;
        if (m_lastRenderedHeader.sequence != 0 &&
            frameHeader.sequence > m_lastRenderedHeader.sequence + 1)
        {
            result.sequenceGap =
                frameHeader.sequence - m_lastRenderedHeader.sequence - 1;
        }
        return result;
    }
    if (m_hasAcceptedFrame && sameSceneDatabase && !resetHistory &&
        !m_requiresTemporalSettle &&
        scene.GetRevision() != m_appliedSceneRevision &&
        watchedResourcesCurrent)
    {
        const RenderSceneDatabaseChanges changes =
            scene.CollectChangesSince(m_appliedSceneRevision);
        if (changes.available && !changes.fullReset)
        {
            RenderFrameApplyResult incremental = ApplyIncrementalFrameState(
                frame, scene, changes, registry);
            if (incremental.IsApplied())
            {
                ApplyContentOnlyTextureRefreshes(m_watchedResourceRevisions,
                                                 contentOnlyTextureRefreshes);
            }
            return incremental;
        }
    }

    std::vector<RenderPrimitiveSnapshot> primitives =
        CopySortedSceneValues(scene.GetPrimitives());
    std::vector<RenderLightSnapshot> lights =
        CopySortedSceneValues(scene.GetLights());
    RenderFeatureSnapshot features;
    features.BeginBuild(frameHeader.sequence);
    features.particles.items = CopySortedSceneValues(scene.GetParticles());
    features.water.items = CopySortedSceneValues(scene.GetWater());
    features.terrain.items = CopySortedSceneValues(scene.GetTerrain());
    features.metadata.providerCount = features.particles.items.size() +
                                      features.water.items.size() +
                                      features.terrain.items.size();
    features.MarkComplete();

    return ApplyFrameState(
        frameHeader,
        frame.GetView(),
        primitives,
        lights,
        scene.GetSky().value_or(RenderSkySnapshot{}),
        scene.GetEnvironment().value_or(RenderEnvironmentSnapshot{}),
        frame.GetSettings(),
        frame.GetCaptureRequest(),
        features,
        scene,
        registry);
}

RenderFrameApplyResult RenderScene::ApplyFrameState(
    const RenderFrameHeaderV5& header,
    const RenderViewSnapshot& view,
    const std::vector<RenderPrimitiveSnapshot>& primitives,
    const std::vector<RenderLightSnapshot>& lights,
    const RenderSkySnapshot& inputSky,
    const RenderEnvironmentSnapshot& inputEnvironment,
    const RenderFrameSettings& settings,
    const RenderFrameCaptureRequest& captureRequest,
    const RenderFeatureSnapshot& features,
    const RenderSceneDatabase& retainedScene,
    const RenderResourceRegistry& registry)
{
    RenderFrameApplyResult result;
    result.sequence = header.sequence;
    result.previousAcceptedSequence = m_acceptedHeader.sequence;
    result.lastRenderedSequence = m_lastRenderedHeader.sequence;

    if (header.sequence == 0 ||
        (m_hasAcceptedFrame && header.sequence <= m_acceptedHeader.sequence))
    {
        result.code = RenderFrameApplyCode::OutOfOrder;
        return result;
    }

    // Object identity is the retained-scene and GPUScene publication key. Reject
    // a duplicate before constructing candidates or touching the publication
    // cache so the prior accepted scene remains wholly intact.
    std::unordered_set<uint64> packetObjectIds;
    packetObjectIds.reserve(primitives.size());
    for (const RenderPrimitiveSnapshot& primitive : primitives)
    {
        if (primitive.objectId != 0 &&
            !packetObjectIds.insert(primitive.objectId).second)
        {
            result.code = RenderFrameApplyCode::InvalidPacket;
            return result;
        }
    }

    std::vector<RenderObject> candidateObjects;
    std::vector<RenderLight> candidateLights;
    std::vector<RenderResourceHandle> candidateReferences;
    std::vector<RenderResourceHandle> candidateSkyReferences;
    std::vector<RenderResourceHandle> candidateEnvironmentReferences;
    std::unordered_map<RenderResourceHandle,
                       uint64,
                       RenderResourceHandleHash>
        candidateWatchedResources;
    candidateObjects.reserve(primitives.size());
    candidateLights.reserve(lights.size());

    const bool resetHistory =
        m_lastRenderedHeader.sequence == 0 ||
        header.worldRevision != m_lastRenderedHeader.worldRevision ||
        header.temporalEpoch != m_lastRenderedHeader.temporalEpoch ||
        header.explicitDiscontinuity ||
        m_surfaceCompatibilityKey !=
            m_lastRenderedSurfaceCompatibilityKey;

    for (const RenderPrimitiveSnapshot& primitive : primitives)
    {
        AddWatchedResourceClosure(candidateWatchedResources,
                                  primitive.mesh,
                                  registry);
        AddWatchedResourceClosure(candidateWatchedResources,
                                  primitive.fallbackMesh,
                                  registry);
        AddWatchedResourceClosure(candidateWatchedResources,
                                  primitive.material,
                                  registry);
        AddWatchedResourceClosure(candidateWatchedResources,
                                  primitive.fallbackMaterial,
                                  registry);
        for (const RenderSubmeshMaterialBinding& binding : primitive.submeshes)
        {
            AddWatchedResourceClosure(candidateWatchedResources,
                                      binding.material,
                                      registry);
        }
        const RenderResourceStatus meshStatus =
            registry.QueryStatus(primitive.mesh);
        if (primitive.objectId == 0 || !primitive.mesh.IsValid() ||
            meshStatus.code != RenderResourceStatusCode::Current)
        {
            result.code = RenderFrameApplyCode::StaleRequiredHandle;
            return result;
        }
        if (primitive.hasSkinningPaletteProvider &&
            !primitive.HasValidSkinningPalette())
        {
            result.code = RenderFrameApplyCode::InvalidPacket;
            return result;
        }

        RenderObject object;
        object.entityId = primitive.objectId;
        object.objectRevision = retainedScene.GetPrimitiveRevision(
            primitive.objectId);
        object.worldMatrix = primitive.worldTransform;
        object.previousWorldMatrix = primitive.worldTransform;
        if (!resetHistory)
        {
            const auto previous = m_lastRenderedObjectTransforms.find(
                primitive.objectId);
            if (previous != m_lastRenderedObjectTransforms.end())
            {
                object.previousWorldMatrix = previous->second;
                object.previousWorldMatrixValid = 1;
            }
        }
        object.normalMatrix = glm::inverseTranspose(
            Mat4(Mat3(object.worldMatrix)));
        object.bounds = AABB(primitive.boundsMin, primitive.boundsMax);
        object.fallbackMesh = primitive.fallbackMesh;
        object.fallbackMaterial = primitive.fallbackMaterial;
        object.mesh = SelectOptional(primitive.mesh,
                                     primitive.fallbackMesh,
                                     registry,
                                     result.pendingFallbackCount);
        object.skinningMatrices = primitive.skinMatrices;
        object.hasSkinningPaletteProvider =
            primitive.hasSkinningPaletteProvider;
        object.skinningPalette = primitive.skinningPalette;
        object.sortKey = primitive.sortKey;
        object.layerMask = primitive.layerMask;
        object.flags = primitive.flags;
        object.visible = (primitive.flags & PRIMITIVE_VISIBLE) != 0;
        object.castsShadow =
            (primitive.flags & PRIMITIVE_CASTS_SHADOW) != 0;
        object.receivesShadow =
            (primitive.flags & PRIMITIVE_RECEIVES_SHADOW) != 0;
        object.drawable = object.mesh.IsValid();

        const bool usingPreferredMesh = object.mesh == primitive.mesh;
        const RenderMeshResourceData* meshData = object.mesh.IsValid()
                                                     ? registry.ResolveMesh(object.mesh)
                                                     : nullptr;
        object.meshBatchesAuthoritative = meshData != nullptr;
        const bool hasExplicitBindings = !primitive.submeshes.empty();
        if (usingPreferredMesh && hasExplicitBindings)
        {
            if (meshData == nullptr)
            {
                result.code = RenderFrameApplyCode::InvalidPacket;
                return result;
            }
        }

        std::vector<MeshUploadSubmesh> actualSubmeshes;
        if (meshData != nullptr)
        {
            actualSubmeshes = meshData->submeshes;
            if (actualSubmeshes.empty() && meshData->createInfo.indexCount != 0)
            {
                if (meshData->createInfo.indexCount >
                    std::numeric_limits<uint32>::max())
                {
                    result.code = RenderFrameApplyCode::InvalidPacket;
                    return result;
                }
                actualSubmeshes.push_back(MeshUploadSubmesh{
                    0,
                    static_cast<uint32>(meshData->createInfo.indexCount),
                    0,
                    meshData->createInfo.topology});
            }
        }

        if (usingPreferredMesh && hasExplicitBindings)
        {
            if (primitive.submeshes.size() != actualSubmeshes.size())
            {
                result.code = RenderFrameApplyCode::InvalidPacket;
                return result;
            }
            for (size_t index = 0; index < primitive.submeshes.size(); ++index)
            {
                const RenderSubmeshMaterialBinding& binding =
                    primitive.submeshes[index];
                if (binding.submeshIndex != index ||
                    !IsValidMaterialMode(binding.materialMode))
                {
                    result.code = RenderFrameApplyCode::InvalidPacket;
                    return result;
                }
            }
        }

        const RenderMaterialMode legacyMode = GetLegacyMaterialMode(primitive);
        std::vector<RenderResourceHandle> selectedMaterials;
        std::vector<RenderMaterialMode> selectedModes;
        selectedMaterials.reserve(actualSubmeshes.size());
        selectedModes.reserve(actualSubmeshes.size());
        if (meshData != nullptr && usingPreferredMesh && hasExplicitBindings)
        {
            for (const RenderSubmeshMaterialBinding& binding : primitive.submeshes)
            {
                selectedMaterials.push_back(SelectOptional(
                    binding.material,
                    primitive.fallbackMaterial,
                    registry,
                    result.pendingFallbackCount));
                selectedModes.push_back(binding.materialMode);
            }
        }
        else
        {
            const RenderResourceHandle legacyMaterial = SelectOptional(
                primitive.material,
                primitive.fallbackMaterial,
                registry,
                result.pendingFallbackCount);
            if (actualSubmeshes.empty())
            {
                object.material = legacyMaterial;
            }
            else
            {
                selectedMaterials.assign(actualSubmeshes.size(), legacyMaterial);
                selectedModes.assign(actualSubmeshes.size(), legacyMode);
            }
        }

        if (!selectedMaterials.empty())
        {
            object.material = selectedMaterials.front();
            object.materialModes = selectedModes;
        }

        if (meshData != nullptr && !actualSubmeshes.empty())
        {
            MeshBatchBuildInput batchInput;
            batchInput.objectId = object.entityId;
            batchInput.objectRevision = object.objectRevision;
            batchInput.mesh = object.mesh;
            batchInput.primitiveData =
                static_cast<PrimitiveDataIndex>(candidateObjects.size());
            batchInput.indexType = meshData->createInfo.indexType;
            batchInput.boundsMin = primitive.boundsMin;
            batchInput.boundsMax = primitive.boundsMax;
            if (IsSkinned(primitive))
            {
                batchInput.flags |= RenderBatchFlags::Skinned;
            }
            if (object.castsShadow)
            {
                batchInput.flags |= RenderBatchFlags::CastsShadow;
            }
            if (object.receivesShadow)
            {
                batchInput.flags |= RenderBatchFlags::ReceivesShadow;
            }
            batchInput.submeshes.reserve(actualSubmeshes.size());
            for (size_t index = 0; index < actualSubmeshes.size(); ++index)
            {
                batchInput.submeshes.push_back(MeshBatchSourceSubmesh{
                    static_cast<uint32>(index),
                    actualSubmeshes[index],
                    selectedMaterials[index],
                    selectedModes[index]});
            }
            MeshBatchBuildResult batchResult = BuildMeshBatches(batchInput);
            if (!batchResult.IsSuccess())
            {
                result.code = RenderFrameApplyCode::InvalidPacket;
                return result;
            }
            object.meshBatches = std::move(batchResult.batches);
        }
        if (!object.drawable)
        {
            ++result.skippedDrawCount;
        }
        AddUniqueHandle(object.referencedResources, object.mesh);
        for (const MeshBatch& batch : object.meshBatches)
        {
            AddUniqueHandle(object.referencedResources, batch.material);
        }
        AddUniqueHandle(object.referencedResources, object.material);
        for (RenderResourceHandle reference : object.referencedResources)
            AddUniqueHandle(candidateReferences, reference);
        candidateObjects.push_back(std::move(object));
    }

    for (const RenderLightSnapshot& snapshot : lights)
    {
        AddWatchedResourceClosure(candidateWatchedResources,
                                  snapshot.shadowResource,
                                  registry);
        if (snapshot.lightId == 0 ||
            (snapshot.shadowResource.IsValid() &&
             !registry.HasExactEntry(snapshot.shadowResource)))
        {
            result.code = RenderFrameApplyCode::InvalidPacket;
            return result;
        }
        RenderLight light;
        light.lightId = snapshot.lightId;
        light.type = ToRenderLightType(snapshot.type);
        light.position = snapshot.position;
        light.direction = snapshot.direction;
        light.color = snapshot.color;
        light.intensity = snapshot.intensity;
        light.range = snapshot.range;
        light.innerConeAngle = snapshot.innerConeRadians;
        light.outerConeAngle = snapshot.outerConeRadians;
        light.layerMask = snapshot.layerMask;
        light.castsShadow = snapshot.castsShadows;
        light.shadowResource =
            snapshot.shadowResource.IsValid() &&
                    registry.IsGPUReadyExact(snapshot.shadowResource)
                ? snapshot.shadowResource
                : RenderResourceHandle{};
        AddUniqueHandle(candidateReferences, light.shadowResource);
        AddUniqueHandle(light.referencedResources, light.shadowResource);
        candidateLights.push_back(std::move(light));
    }

    RenderSkySnapshot sky = inputSky;
    AddWatchedResourceClosure(candidateWatchedResources,
                              sky.skyTexture,
                              registry);
    if (sky.skyTexture.IsValid() &&
        !registry.IsGPUReadyExact(sky.skyTexture))
    {
        sky.skyTexture = {};
    }
    AddUniqueHandle(candidateReferences, sky.skyTexture);
    AddUniqueHandle(candidateSkyReferences, sky.skyTexture);

    RenderEnvironmentSnapshot environment = inputEnvironment;
    AddWatchedResourceClosure(candidateWatchedResources,
                              environment.irradianceTexture,
                              registry);
    AddWatchedResourceClosure(candidateWatchedResources,
                              environment.prefilteredTexture,
                              registry);
    AddWatchedResourceClosure(candidateWatchedResources,
                              environment.brdfLutTexture,
                              registry);
    const bool environmentReady =
        environment.irradianceTexture.IsValid() &&
        environment.prefilteredTexture.IsValid() &&
        environment.brdfLutTexture.IsValid() &&
        registry.IsGPUReadyExact(environment.irradianceTexture) &&
        registry.IsGPUReadyExact(environment.prefilteredTexture) &&
        registry.IsGPUReadyExact(environment.brdfLutTexture);
    if (!environmentReady)
    {
        environment.irradianceTexture = {};
        environment.prefilteredTexture = {};
        environment.brdfLutTexture = {};
    }
    AddUniqueHandle(candidateReferences, environment.irradianceTexture);
    AddUniqueHandle(candidateReferences, environment.prefilteredTexture);
    AddUniqueHandle(candidateReferences, environment.brdfLutTexture);
    AddUniqueHandle(candidateEnvironmentReferences,
                    environment.irradianceTexture);
    AddUniqueHandle(candidateEnvironmentReferences,
                    environment.prefilteredTexture);
    AddUniqueHandle(candidateEnvironmentReferences,
                    environment.brdfLutTexture);

    // Every validation path above must succeed before the retained cache changes.
    // The cache owns only static packet templates; the draw-list path rebinds
    // frame-local primitive indices and object identity for every use.
    m_drawPacketCache.BeginAcceptedPublication();
    for (const RenderObject& object : candidateObjects)
    {
        for (const MeshBatch& batch : object.meshBatches)
        {
            RenderDrawPacket packetTemplate;
            const RenderDrawPacketCacheResolveResult cacheResult =
                m_drawPacketCache.Resolve(batch,
                                          m_drawPacketCacheVersions,
                                          false,
                                          packetTemplate);
            static_cast<void>(cacheResult);
        }
    }
    m_drawPacketCache.EndAcceptedPublication();

    m_objects.swap(candidateObjects);
    m_lights.swap(candidateLights);
    m_referencedResources.swap(candidateReferences);
    m_skyReferences.swap(candidateSkyReferences);
    m_environmentReferences.swap(candidateEnvironmentReferences);
    m_watchedResourceRevisions.swap(candidateWatchedResources);
    RebuildRetainedIndicesAndReferences();
    m_acceptedHeader = header;
    m_view = view;
    m_sky = std::move(sky);
    m_environment = std::move(environment);
    m_settings = settings;
    m_captureRequest = captureRequest;
    m_features = features;
    m_hasAcceptedFrame = true;
    m_temporalHistoryReset = resetHistory;
    m_requiresTemporalSettle = std::any_of(
        m_objects.begin(),
        m_objects.end(),
        [](const RenderObject& object)
        {
            return object.previousWorldMatrixValid == 0 ||
                   !MatricesEqual(object.previousWorldMatrix,
                                  object.worldMatrix);
        });
    m_acceptedSceneMutated = true;
    m_fullGPUSceneMutation = true;
    m_gpuSceneChangedObjectIds.clear();
    m_gpuSceneRemovedObjectIds.clear();
    m_temporalSettleObjectIds.clear();
    m_fullRenderedObjectRefresh = true;
    m_pendingRenderedObjectIds.clear();
    m_pendingRemovedObjectIds.clear();
    m_appliedSceneRevision = retainedScene.GetRevision();
    m_sourceSceneDatabaseId = retainedScene.GetInstanceId();
    m_observedResourceContentRevision = registry.GetContentRevision();
    ++m_retainedStats.fullRebuildCount;
    SaturatingAdd(m_mutationTotals.fullRebuildCount, 1, m_mutationTotalsSaturated);
    SaturatingAdd(m_mutationTotals.rebuiltObjectCount,
                  static_cast<uint64>(m_objects.size()),
                  m_mutationTotalsSaturated);
    m_retainedStats.appliedSceneRevision = m_appliedSceneRevision;
    m_retainedStats.lastRebuiltObjectCount =
        static_cast<uint32>(m_objects.size());
    m_retainedStats.lastRemovedObjectCount = 0;

    result.code = RenderFrameApplyCode::Applied;
    result.temporalHistoryReset = resetHistory;
    result.sceneMutated = true;
    if (m_lastRenderedHeader.sequence != 0 &&
        header.sequence > m_lastRenderedHeader.sequence + 1)
    {
        result.sequenceGap =
            header.sequence - m_lastRenderedHeader.sequence - 1;
    }
    return result;
}

RenderFrameApplyResult RenderScene::ApplyIncrementalFrameState(
    const RenderFramePacketV5& frame,
    const RenderSceneDatabase& retainedScene,
    const RenderSceneDatabaseChanges& changes,
    const RenderResourceRegistry& registry)
{
    RenderFrameApplyResult result;
    const RenderFrameHeaderV5& header = frame.GetHeader();
    result.sequence = header.sequence;
    result.previousAcceptedSequence = m_acceptedHeader.sequence;
    result.lastRenderedSequence = m_lastRenderedHeader.sequence;

    struct ObjectCandidate
    {
        uint64 objectId = 0;
        RenderObject object;
    };
    struct LightCandidate
    {
        uint64 lightId = 0;
        RenderLight light;
    };

    std::vector<ObjectCandidate> objectCandidates;
    std::vector<uint64> removedObjectIds;
    std::vector<LightCandidate> lightCandidates;
    std::vector<uint64> removedLightIds;
    std::unordered_map<RenderResourceHandle,
                       uint64,
                       RenderResourceHandleHash>
        changedWatchedResources;
    objectCandidates.reserve(changes.primitives.size());
    removedObjectIds.reserve(changes.primitives.size());
    lightCandidates.reserve(changes.lights.size());
    removedLightIds.reserve(changes.lights.size());

    for (uint64 objectId : changes.primitives)
    {
        const RenderPrimitiveSnapshot* primitive =
            retainedScene.FindPrimitive(objectId);
        if (primitive == nullptr)
        {
            removedObjectIds.push_back(objectId);
            continue;
        }
        PrimitiveDataIndex primitiveData = 0;
        const auto existing = m_objectIndices.find(objectId);
        if (existing != m_objectIndices.end())
            primitiveData = existing->second;

        ObjectCandidate candidate;
        candidate.objectId = objectId;
        if (!BuildRetainedObject(
                *primitive,
                primitiveData,
                retainedScene.GetPrimitiveRevision(objectId),
                false,
                m_lastRenderedObjectTransforms,
                registry,
                result,
                candidate.object,
                changedWatchedResources))
        {
            return result;
        }
        objectCandidates.push_back(std::move(candidate));
    }

    for (uint64 lightId : changes.lights)
    {
        const RenderLightSnapshot* light = retainedScene.FindLight(lightId);
        if (light == nullptr)
        {
            removedLightIds.push_back(lightId);
            continue;
        }
        LightCandidate candidate;
        candidate.lightId = lightId;
        if (!BuildRetainedLight(*light,
                                registry,
                                result,
                                candidate.light,
                                changedWatchedResources))
        {
            return result;
        }
        lightCandidates.push_back(std::move(candidate));
    }

    std::optional<RenderFeatureSnapshot> changedFeatures;
    if (!changes.particles.empty() || !changes.water.empty() ||
        !changes.terrain.empty())
    {
        RenderFeatureSnapshot features;
        features.BeginBuild(header.sequence);
        features.particles.items =
            CopySortedSceneValues(retainedScene.GetParticles());
        features.water.items = CopySortedSceneValues(retainedScene.GetWater());
        features.terrain.items =
            CopySortedSceneValues(retainedScene.GetTerrain());
        features.metadata.providerCount = features.particles.items.size() +
                                          features.water.items.size() +
                                          features.terrain.items.size();
        features.MarkComplete();
        changedFeatures = std::move(features);
    }

    std::optional<RenderSkySnapshot> changedSky;
    std::vector<RenderResourceHandle> changedSkyReferences;
    if (changes.skyChanged)
    {
        RenderSkySnapshot sky =
            retainedScene.GetSky().value_or(RenderSkySnapshot{});
        AddWatchedResourceClosure(changedWatchedResources,
                                  sky.skyTexture,
                                  registry);
        if (sky.skyTexture.IsValid() &&
            !registry.IsGPUReadyExact(sky.skyTexture))
        {
            sky.skyTexture = {};
        }
        AddUniqueHandle(changedSkyReferences, sky.skyTexture);
        changedSky = std::move(sky);
    }

    std::optional<RenderEnvironmentSnapshot> changedEnvironment;
    std::vector<RenderResourceHandle> changedEnvironmentReferences;
    if (changes.environmentChanged)
    {
        RenderEnvironmentSnapshot environment = retainedScene.GetEnvironment()
            .value_or(RenderEnvironmentSnapshot{});
        AddWatchedResourceClosure(changedWatchedResources,
                                  environment.irradianceTexture,
                                  registry);
        AddWatchedResourceClosure(changedWatchedResources,
                                  environment.prefilteredTexture,
                                  registry);
        AddWatchedResourceClosure(changedWatchedResources,
                                  environment.brdfLutTexture,
                                  registry);
        const bool environmentReady =
            environment.irradianceTexture.IsValid() &&
            environment.prefilteredTexture.IsValid() &&
            environment.brdfLutTexture.IsValid() &&
            registry.IsGPUReadyExact(environment.irradianceTexture) &&
            registry.IsGPUReadyExact(environment.prefilteredTexture) &&
            registry.IsGPUReadyExact(environment.brdfLutTexture);
        if (!environmentReady)
        {
            environment.irradianceTexture = {};
            environment.prefilteredTexture = {};
            environment.brdfLutTexture = {};
        }
        AddUniqueHandle(changedEnvironmentReferences,
                        environment.irradianceTexture);
        AddUniqueHandle(changedEnvironmentReferences,
                        environment.prefilteredTexture);
        AddUniqueHandle(changedEnvironmentReferences,
                        environment.brdfLutTexture);
        changedEnvironment = std::move(environment);
    }

    m_gpuSceneChangedObjectIds.clear();
    m_gpuSceneRemovedObjectIds.clear();
    m_pendingRenderedObjectIds.clear();
    m_pendingRemovedObjectIds.clear();
    m_fullGPUSceneMutation = false;
    m_fullRenderedObjectRefresh = false;
    m_drawPacketCache.BeginAcceptedPublication(false);

    for (uint64 objectId : removedObjectIds)
    {
        const auto existing = m_objectIndices.find(objectId);
        if (existing == m_objectIndices.end())
            continue;
        RemoveObjectAt(existing->second);
        m_lastRenderedObjectTransforms.erase(objectId);
        m_gpuSceneRemovedObjectIds.push_back(objectId);
        m_pendingRemovedObjectIds.push_back(objectId);
    }

    for (ObjectCandidate& candidate : objectCandidates)
    {
        const auto existing = m_objectIndices.find(candidate.objectId);
        uint32 index = 0;
        uint32 previousBatchCount = 0;
        if (existing != m_objectIndices.end())
        {
            index = existing->second;
            previousBatchCount = static_cast<uint32>(
                m_objects[index].meshBatches.size());
            m_drawCount -= previousBatchCount;
            RemoveReferences(m_objects[index].referencedResources);
            m_objects[index] = std::move(candidate.object);
        }
        else
        {
            index = static_cast<uint32>(m_objects.size());
            m_objects.push_back(std::move(candidate.object));
            m_objectIndices.insert_or_assign(candidate.objectId, index);
        }
        RenderObject& object = m_objects[index];
        m_drawCount += static_cast<uint32>(object.meshBatches.size());
        for (MeshBatch& batch : object.meshBatches)
            batch.primitiveData = index;
        AddReferences(object.referencedResources);

        for (const MeshBatch& batch : object.meshBatches)
        {
            RenderDrawPacket packetTemplate;
            static_cast<void>(m_drawPacketCache.Resolve(
                batch,
                m_drawPacketCacheVersions,
                false,
                packetTemplate));
        }
        for (uint32 submeshIndex = static_cast<uint32>(object.meshBatches.size());
             submeshIndex < previousBatchCount;
             ++submeshIndex)
        {
            static_cast<void>(m_drawPacketCache.Remove(candidate.objectId,
                                                       submeshIndex));
        }
        m_gpuSceneChangedObjectIds.push_back(candidate.objectId);
        m_pendingRenderedObjectIds.push_back(candidate.objectId);
        if (object.previousWorldMatrixValid == 0 ||
            !MatricesEqual(object.previousWorldMatrix, object.worldMatrix))
        {
            m_requiresTemporalSettle = true;
        }
    }
    m_drawPacketCache.EndAcceptedPublication();

    for (uint64 lightId : removedLightIds)
    {
        const auto existing = m_lightIndices.find(lightId);
        if (existing != m_lightIndices.end())
            RemoveLightAt(existing->second);
    }
    for (LightCandidate& candidate : lightCandidates)
    {
        const auto existing = m_lightIndices.find(candidate.lightId);
        if (existing != m_lightIndices.end())
        {
            RemoveReferences(m_lights[existing->second].referencedResources);
            m_lights[existing->second] = std::move(candidate.light);
            AddReferences(m_lights[existing->second].referencedResources);
        }
        else
        {
            const uint32 index = static_cast<uint32>(m_lights.size());
            m_lights.push_back(std::move(candidate.light));
            m_lightIndices.insert_or_assign(candidate.lightId, index);
            AddReferences(m_lights.back().referencedResources);
        }
    }

    if (changedFeatures.has_value())
        m_features = std::move(*changedFeatures);
    if (changedSky.has_value())
    {
        RemoveReferences(m_skyReferences);
        m_sky = std::move(*changedSky);
        m_skyReferences = std::move(changedSkyReferences);
        AddReferences(m_skyReferences);
    }
    if (changedEnvironment.has_value())
    {
        RemoveReferences(m_environmentReferences);
        m_environment = std::move(*changedEnvironment);
        m_environmentReferences = std::move(changedEnvironmentReferences);
        AddReferences(m_environmentReferences);
    }
    for (const auto& [handle, revision] : changedWatchedResources)
        m_watchedResourceRevisions.insert_or_assign(handle, revision);

    m_acceptedHeader = header;
    m_view = frame.GetView();
    m_settings = frame.GetSettings();
    m_captureRequest = frame.GetCaptureRequest();
    m_hasAcceptedFrame = true;
    m_temporalHistoryReset = false;
    m_acceptedSceneMutated = !changes.Empty();
    m_appliedSceneRevision = retainedScene.GetRevision();
    m_sourceSceneDatabaseId = retainedScene.GetInstanceId();
    m_observedResourceContentRevision = registry.GetContentRevision();
    ++m_retainedStats.incrementalUpdateCount;
    SaturatingAdd(m_mutationTotals.incrementalCommitCount,
                  1,
                  m_mutationTotalsSaturated);
    SaturatingAdd(m_mutationTotals.rebuiltObjectCount,
                  static_cast<uint64>(objectCandidates.size()),
                  m_mutationTotalsSaturated);
    SaturatingAdd(m_mutationTotals.removedObjectCount,
                  static_cast<uint64>(m_gpuSceneRemovedObjectIds.size()),
                  m_mutationTotalsSaturated);
    m_retainedStats.appliedSceneRevision = m_appliedSceneRevision;
    m_retainedStats.lastRebuiltObjectCount =
        static_cast<uint32>(objectCandidates.size());
    m_retainedStats.lastRemovedObjectCount =
        static_cast<uint32>(m_gpuSceneRemovedObjectIds.size());

    result.code = RenderFrameApplyCode::Applied;
    result.temporalHistoryReset = false;
    result.sceneMutated = !changes.Empty();
    if (m_lastRenderedHeader.sequence != 0 &&
        header.sequence > m_lastRenderedHeader.sequence + 1)
    {
        result.sequenceGap = header.sequence - m_lastRenderedHeader.sequence - 1;
    }
    return result;
}

void RenderScene::RebuildRetainedIndicesAndReferences()
{
    m_objectIndices.clear();
    m_lightIndices.clear();
    m_referencedResources.clear();
    m_referenceCounts.clear();
    m_referenceIndices.clear();
    m_drawCount = 0;

    m_objectIndices.reserve(m_objects.size());
    for (uint32 index = 0; index < static_cast<uint32>(m_objects.size()); ++index)
    {
        RenderObject& object = m_objects[index];
        m_objectIndices.insert_or_assign(object.entityId, index);
        for (MeshBatch& batch : object.meshBatches)
            batch.primitiveData = index;
        m_drawCount += static_cast<uint32>(object.meshBatches.size());
        AddReferences(object.referencedResources);
    }

    m_lightIndices.reserve(m_lights.size());
    for (uint32 index = 0; index < static_cast<uint32>(m_lights.size()); ++index)
    {
        m_lightIndices.insert_or_assign(m_lights[index].lightId, index);
        AddReferences(m_lights[index].referencedResources);
    }
    AddReferences(m_skyReferences);
    AddReferences(m_environmentReferences);
}

void RenderScene::AddReferences(
    const std::vector<RenderResourceHandle>& references)
{
    for (RenderResourceHandle handle : references)
    {
        if (!handle.IsValid())
            continue;
        auto [count, inserted] = m_referenceCounts.emplace(handle, 1U);
        if (!inserted)
        {
            ++count->second;
            continue;
        }
        const uint32 index = static_cast<uint32>(m_referencedResources.size());
        m_referenceIndices.insert_or_assign(handle, index);
        m_referencedResources.push_back(handle);
    }
}

void RenderScene::RemoveReferences(
    const std::vector<RenderResourceHandle>& references)
{
    for (RenderResourceHandle handle : references)
    {
        const auto count = m_referenceCounts.find(handle);
        if (count == m_referenceCounts.end())
            continue;
        if (count->second > 1U)
        {
            --count->second;
            continue;
        }

        const auto indexIt = m_referenceIndices.find(handle);
        if (indexIt != m_referenceIndices.end())
        {
            const uint32 index = indexIt->second;
            const uint32 last = static_cast<uint32>(
                m_referencedResources.size() - 1U);
            if (index != last)
            {
                const RenderResourceHandle moved = m_referencedResources[last];
                m_referencedResources[index] = moved;
                m_referenceIndices.insert_or_assign(moved, index);
            }
            m_referencedResources.pop_back();
            m_referenceIndices.erase(indexIt);
        }
        m_referenceCounts.erase(count);
    }
}

void RenderScene::RemoveObjectAt(uint32 index)
{
    if (index >= m_objects.size())
        return;
    RenderObject& removed = m_objects[index];
    const uint64 removedId = removed.entityId;
    RemoveReferences(removed.referencedResources);
    m_drawCount -= static_cast<uint32>(removed.meshBatches.size());
    for (const MeshBatch& batch : removed.meshBatches)
        static_cast<void>(m_drawPacketCache.Remove(batch.objectId,
                                                   batch.submeshIndex));
    m_objectIndices.erase(removedId);

    const uint32 last = static_cast<uint32>(m_objects.size() - 1U);
    if (index != last)
    {
        m_objects[index] = std::move(m_objects[last]);
        m_objectIndices.insert_or_assign(m_objects[index].entityId, index);
        for (MeshBatch& batch : m_objects[index].meshBatches)
            batch.primitiveData = index;
    }
    m_objects.pop_back();
}

void RenderScene::RemoveLightAt(uint32 index)
{
    if (index >= m_lights.size())
        return;
    const uint64 removedId = m_lights[index].lightId;
    RemoveReferences(m_lights[index].referencedResources);
    m_lightIndices.erase(removedId);
    const uint32 last = static_cast<uint32>(m_lights.size() - 1U);
    if (index != last)
    {
        m_lights[index] = std::move(m_lights[last]);
        m_lightIndices.insert_or_assign(m_lights[index].lightId, index);
    }
    m_lights.pop_back();
}

const RenderObject* RenderScene::FindObject(uint64 objectId) const noexcept
{
    const auto found = m_objectIndices.find(objectId);
    return found != m_objectIndices.end() && found->second < m_objects.size()
        ? &m_objects[found->second]
        : nullptr;
}

void RenderScene::MarkAcceptedFrameRendered()
{
    if (!m_hasAcceptedFrame)
    {
        return;
    }
    m_lastRenderedHeader = m_acceptedHeader;
    m_lastRenderedView = m_view;
    m_lastRenderedSurfaceCompatibilityKey = m_surfaceCompatibilityKey;
    if (!m_acceptedSceneMutated)
        return;

    m_temporalSettleObjectIds.clear();
    if (m_fullRenderedObjectRefresh)
    {
        m_lastRenderedObjectTransforms.clear();
        m_lastRenderedObjectTransforms.reserve(m_objects.size());
        for (const RenderObject& object : m_objects)
        {
            m_lastRenderedObjectTransforms.insert_or_assign(
                object.entityId, object.worldMatrix);
            if (object.previousWorldMatrixValid == 0 ||
                !MatricesEqual(object.previousWorldMatrix, object.worldMatrix))
            {
                m_temporalSettleObjectIds.push_back(object.entityId);
            }
        }
    }
    else
    {
        for (uint64 objectId : m_pendingRemovedObjectIds)
            m_lastRenderedObjectTransforms.erase(objectId);
        for (uint64 objectId : m_pendingRenderedObjectIds)
        {
            const RenderObject* object = FindObject(objectId);
            if (object == nullptr)
                continue;
            m_lastRenderedObjectTransforms.insert_or_assign(
                objectId, object->worldMatrix);
            if (object->previousWorldMatrixValid == 0 ||
                !MatricesEqual(object->previousWorldMatrix,
                               object->worldMatrix))
            {
                m_temporalSettleObjectIds.push_back(objectId);
            }
        }
    }
    m_requiresTemporalSettle = !m_temporalSettleObjectIds.empty();
    m_fullRenderedObjectRefresh = false;
    m_pendingRenderedObjectIds.clear();
    m_pendingRemovedObjectIds.clear();
    m_acceptedSceneMutated = false;
}

void RenderScene::SetSurfaceCompatibilityKey(uint64 key) noexcept
{
    m_surfaceCompatibilityKey = key;
}

bool RenderScene::FindCachedDrawPacketTemplate(
    const MeshBatch& batch,
    RenderDrawPacket& outTemplate) const noexcept
{
    return m_drawPacketCache.Find(batch, m_drawPacketCacheVersions, outTemplate)
        .IsHit();
}

RenderDrawPacketCacheStats RenderScene::GetDrawPacketCacheStats() const
    noexcept
{
    return m_drawPacketCache.GetStats();
}

void RenderScene::CullAgainstView(
    const RenderViewSnapshot& view,
    std::vector<uint32>& outVisibleIndices) const
{
    outVisibleIndices.clear();
    outVisibleIndices.reserve(m_objects.size());
    Frustum frustum;
    frustum.ExtractFromMatrix(view.viewProjectionMatrix);
    for (uint32 index = 0; index < static_cast<uint32>(m_objects.size());
         ++index)
    {
        const RenderObject& object = m_objects[index];
        if (object.visible && object.drawable &&
            IsRenderLayerVisible(object.layerMask, view.cullingMask) &&
            frustum.IsVisible(object.bounds))
        {
            outVisibleIndices.push_back(index);
        }
    }
}

void RenderScene::SortVisibleObjects(
    std::vector<uint32>& visibleIndices,
    const Vec3& cameraPosition) const
{
    std::sort(
        visibleIndices.begin(),
        visibleIndices.end(),
        [this, &cameraPosition](uint32 lhs, uint32 rhs)
        {
            const RenderObject& left = m_objects[lhs];
            const RenderObject& right = m_objects[rhs];
            if (left.sortKey != right.sortKey)
            {
                return left.sortKey < right.sortKey;
            }
            const Vec3 leftOffset = left.bounds.GetCenter() - cameraPosition;
            const Vec3 rightOffset = right.bounds.GetCenter() - cameraPosition;
            return dot(leftOffset, leftOffset) < dot(rightOffset, rightOffset);
        });
}

} // namespace RVX
