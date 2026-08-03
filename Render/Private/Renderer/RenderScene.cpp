/**
 * @file RenderScene.cpp
 * @brief RenderScene transactional packet application.
 */

#include "Render/Renderer/RenderScene.h"

#include "Core/Math/Frustum.h"
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

    bool IsSkinned(const RenderPrimitiveSnapshot& primitive,
                   const RenderMeshResourceData& mesh)
    {
        if (!primitive.skinMatrices.empty())
        {
            return true;
        }
        return std::any_of(
            mesh.buffers.begin(), mesh.buffers.end(),
            [](const RenderOwnedBuffer& buffer)
            {
                return buffer.semantic == RenderMeshBufferSemantic::BoneIndices ||
                       buffer.semantic == RenderMeshBufferSemantic::BoneWeights;
            });
    }
} // namespace

void RenderScene::Clear()
{
    m_objects.clear();
    m_lights.clear();
    m_acceptedHeader = {};
    m_view = {};
    m_sky = {};
    m_environment = {};
    m_settings = {};
    m_captureRequest = {};
    m_features.Clear();
    m_referencedResources.clear();
    m_drawPacketCache.Clear();
    m_hasAcceptedFrame = false;
    m_temporalHistoryReset = true;
    m_lastRenderedHeader = {};
    m_lastRenderedView = {};
    m_lastRenderedObjectTransforms.clear();
    m_lastRenderedSurfaceCompatibilityKey = m_surfaceCompatibilityKey;
}

RenderFrameApplyResult RenderScene::ApplyFramePacket(
    const RenderFramePacket& packet,
    const RenderResourceRegistry& registry)
{
    RenderFrameApplyResult result;
    const RenderFrameHeader& header = packet.GetHeader();
    result.sequence = header.sequence;
    result.previousAcceptedSequence = m_acceptedHeader.sequence;
    result.lastRenderedSequence = m_lastRenderedHeader.sequence;

    if (header.schemaId != RVX_RENDER_FRAME_PACKET_SCHEMA_ID ||
        header.schemaVersion != RVX_RENDER_FRAME_PACKET_SCHEMA_VERSION)
    {
        result.code = RenderFrameApplyCode::UnsupportedSchema;
        return result;
    }
    if (header.sequence == 0 ||
        (m_hasAcceptedFrame && header.sequence <= m_acceptedHeader.sequence))
    {
        result.code = RenderFrameApplyCode::OutOfOrder;
        return result;
    }

    // Object identity is the retained-scene and GPU-scene-shadow key.  Reject
    // a duplicate before constructing candidates or touching the publication
    // cache so the prior accepted scene remains wholly intact.
    std::unordered_set<uint64> packetObjectIds;
    packetObjectIds.reserve(packet.GetPrimitives().size());
    for (const RenderPrimitiveSnapshot& primitive : packet.GetPrimitives())
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
    candidateObjects.reserve(packet.GetPrimitives().size());
    candidateLights.reserve(packet.GetLights().size());

    const bool resetHistory =
        m_lastRenderedHeader.sequence == 0 ||
        header.worldRevision != m_lastRenderedHeader.worldRevision ||
        header.temporalEpoch != m_lastRenderedHeader.temporalEpoch ||
        header.explicitDiscontinuity ||
        m_surfaceCompatibilityKey !=
            m_lastRenderedSurfaceCompatibilityKey;

    for (const RenderPrimitiveSnapshot& primitive : packet.GetPrimitives())
    {
        const RenderResourceStatus meshStatus =
            registry.QueryStatus(primitive.mesh);
        if (primitive.objectId == 0 || !primitive.mesh.IsValid() ||
            meshStatus.code != RenderResourceStatusCode::Current)
        {
            result.code = RenderFrameApplyCode::StaleRequiredHandle;
            return result;
        }

        RenderObject object;
        object.entityId = primitive.objectId;
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
            batchInput.mesh = object.mesh;
            batchInput.primitiveData =
                static_cast<PrimitiveDataIndex>(candidateObjects.size());
            batchInput.indexType = meshData->createInfo.indexType;
            batchInput.boundsMin = primitive.boundsMin;
            batchInput.boundsMax = primitive.boundsMax;
            if (IsSkinned(primitive, *meshData))
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
        AddUniqueHandle(candidateReferences, object.mesh);
        for (const MeshBatch& batch : object.meshBatches)
        {
            AddUniqueHandle(candidateReferences, batch.material);
        }
        AddUniqueHandle(candidateReferences, object.material);
        candidateObjects.push_back(std::move(object));
    }

    for (const RenderLightSnapshot& snapshot : packet.GetLights())
    {
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
        light.castsShadow = snapshot.castsShadows;
        light.shadowResource =
            snapshot.shadowResource.IsValid() &&
                    registry.IsGPUReadyExact(snapshot.shadowResource)
                ? snapshot.shadowResource
                : RenderResourceHandle{};
        AddUniqueHandle(candidateReferences, light.shadowResource);
        candidateLights.push_back(std::move(light));
    }

    RenderSkySnapshot sky = packet.GetSky();
    if (sky.skyTexture.IsValid() &&
        !registry.IsGPUReadyExact(sky.skyTexture))
    {
        sky.skyTexture = {};
    }
    AddUniqueHandle(candidateReferences, sky.skyTexture);

    RenderEnvironmentSnapshot environment = packet.GetEnvironment();
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
    m_acceptedHeader = header;
    m_view = packet.GetView();
    m_sky = std::move(sky);
    m_environment = std::move(environment);
    m_settings = packet.GetSettings();
    m_captureRequest = packet.GetCaptureRequest();
    m_features = packet.GetFeatures();
    m_hasAcceptedFrame = true;
    m_temporalHistoryReset = resetHistory;

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

void RenderScene::MarkAcceptedFrameRendered()
{
    if (!m_hasAcceptedFrame)
    {
        return;
    }
    m_lastRenderedHeader = m_acceptedHeader;
    m_lastRenderedView = m_view;
    m_lastRenderedSurfaceCompatibilityKey = m_surfaceCompatibilityKey;
    m_lastRenderedObjectTransforms.clear();
    m_lastRenderedObjectTransforms.reserve(m_objects.size());
    for (const RenderObject& object : m_objects)
    {
        m_lastRenderedObjectTransforms.insert_or_assign(
            object.entityId, object.worldMatrix);
    }
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
