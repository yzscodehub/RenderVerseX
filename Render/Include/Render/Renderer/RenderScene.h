#pragma once

/**
 * @file RenderScene.h
 * @brief Render-owned retained state consumed from v5 frames and RenderSceneDatabase.
 */

#include "Core/Math/AABB.h"
#include "Core/MathTypes.h"
#include "Core/Types.h"
#include "Render/RenderDiagnostics.h"
#include "RenderContracts/RenderFramePacketV5.h"
#include "RenderContracts/RenderMaterial.h"
#include "Render/Renderer/MeshBatch.h"
#include "Render/Renderer/RenderDrawPacketCache.h"

#include <unordered_map>
#include <vector>

namespace RVX
{
    class RenderResourceRegistry;
    class RenderSceneDatabase;
    struct RenderSceneDatabaseChanges;

    enum class RenderFrameApplyCode : uint8
    {
        Applied = 0,
        UnsupportedSchema,
        OutOfOrder,
        StaleRequiredHandle,
        InvalidPacket
    };

    struct RenderFrameApplyResult
    {
        RenderFrameApplyCode code = RenderFrameApplyCode::InvalidPacket;
        uint64 sequence = 0;
        uint64 previousAcceptedSequence = 0;
        uint64 lastRenderedSequence = 0;
        uint64 sequenceGap = 0;
        uint32 pendingFallbackCount = 0;
        uint32 skippedDrawCount = 0;
        bool temporalHistoryReset = false;
        bool sceneMutated = false;

        [[nodiscard]] bool IsApplied() const noexcept
        {
            return code == RenderFrameApplyCode::Applied;
        }
    };

    enum class RenderFrameExecutionCode : uint8
    {
        Rendered = 0,
        NoAcceptedFrame,
        SubmissionFailed,
        PresentationFailed
    };

    struct RenderFrameExecutionResult
    {
        RenderFrameExecutionCode code =
            RenderFrameExecutionCode::NoAcceptedFrame;
        uint64 frameSequence = 0;
        std::vector<RenderResourceHandle> referencedResources;
        bool submitted = false;
        bool presented = false;
    };

    /** @brief Deterministic retained-scene work counters for scale validation. */
    struct RenderSceneRetainedStats
    {
        uint64 fullRebuildCount = 0;
        uint64 incrementalUpdateCount = 0;
        uint64 staticReuseCount = 0;
        uint64 appliedSceneRevision = 0;
        uint32 lastRebuiltObjectCount = 0;
        uint32 lastRemovedObjectCount = 0;
    };

    /** @brief Render-owned primitive values and exact resource generations. */
    struct RenderObject
    {
        Mat4 worldMatrix = Mat4Identity();
        Mat4 previousWorldMatrix = Mat4Identity();
        uint8 previousWorldMatrixValid = 0;
        Mat4 normalMatrix = Mat4Identity();
        AABB bounds;

        RenderResourceHandle mesh;
        RenderResourceHandle material;
        RenderResourceHandle fallbackMesh;
        RenderResourceHandle fallbackMaterial;
        std::vector<Mat4> skinningMatrices;
        bool hasSkinningPaletteProvider = false;
        RenderSkinningPaletteMetadata skinningPalette{};
        std::vector<RenderResourceHandle> referencedResources;

        uint64 entityId = 0;
        uint64 objectRevision = 0;
        uint64 sortKey = 0;
        uint32 layerMask = ~0U;
        uint32 flags = 0;
        bool drawable = false;
        bool visible = true;
        bool castsShadow = true;
        bool receivesShadow = true;

        std::vector<RenderMaterialMode> materialModes;
        std::vector<MeshBatch> meshBatches;
        bool meshBatchesAuthoritative = false;

        [[nodiscard]] bool HasSkinningData() const
        {
            return hasSkinningPaletteProvider || !skinningMatrices.empty();
        }
        [[nodiscard]] bool HasValidSkinningPalette() const noexcept
        {
            return hasSkinningPaletteProvider &&
                   skinningPalette.IsValidFor(skinningMatrices);
        }
    };

    struct RenderLight
    {
        enum class Type : uint8
        {
            Directional = 0,
            Point,
            Spot
        };

        uint64 lightId = 0;
        Type type = Type::Directional;
        Vec3 position{0.0f};
        Vec3 direction{0.0f, 0.0f, -1.0f};
        Vec3 color{1.0f};
        float intensity = 1.0f;
        float range = 10.0f;
        float innerConeAngle = 0.0f;
        float outerConeAngle = 0.7854f;
        RenderResourceHandle shadowResource;
        std::vector<RenderResourceHandle> referencedResources;
        uint32 layerMask = ~0U;
        bool castsShadow = false;
    };

    /** @brief Transactional packet consumer that owns every retained value. */
    class RenderScene
    {
    public:
        RenderScene() = default;

        void Clear();
        /** @brief Apply v5 frame state directly against a persistent scene database. */
        [[nodiscard]] RenderFrameApplyResult ApplyFrameV5(
            const RenderFramePacketV5& frame,
            const RenderSceneDatabase& scene,
            const RenderResourceRegistry& registry);
        void MarkAcceptedFrameRendered();
        void SetSurfaceCompatibilityKey(uint64 key) noexcept;

        void CullAgainstView(
            const RenderViewSnapshot& view,
            std::vector<uint32>& outVisibleIndices) const;
        void SortVisibleObjects(std::vector<uint32>& visibleIndices,
                                const Vec3& cameraPosition) const;

        /** @brief Copy a fully matched retained packet template when available. */
        [[nodiscard]] bool FindCachedDrawPacketTemplate(
            const MeshBatch& batch,
            RenderDrawPacket& outTemplate) const noexcept;
        [[nodiscard]] RenderDrawPacketCacheStats
            GetDrawPacketCacheStats() const noexcept;
        [[nodiscard]] const RenderSceneRetainedStats& GetRetainedStats() const noexcept
        {
            return m_retainedStats;
        }
        [[nodiscard]] const RenderSceneMutationTotals&
            GetMutationTotals() const noexcept
        {
            return m_mutationTotals;
        }
        [[nodiscard]] bool IsMutationTotalsSaturated() const noexcept
        {
            return m_mutationTotalsSaturated;
        }
        [[nodiscard]] const std::vector<uint64>&
            GetGPUSceneChangedObjectIds() const noexcept
        {
            return m_gpuSceneChangedObjectIds;
        }
        [[nodiscard]] const std::vector<uint64>&
            GetGPUSceneRemovedObjectIds() const noexcept
        {
            return m_gpuSceneRemovedObjectIds;
        }
        [[nodiscard]] bool IsFullGPUSceneMutation() const noexcept
        {
            return m_fullGPUSceneMutation;
        }
        [[nodiscard]] const RenderObject* FindObject(
            uint64 objectId) const noexcept;

        [[nodiscard]] const std::vector<RenderObject>& GetObjects() const
        {
            return m_objects;
        }
        [[nodiscard]] const std::vector<RenderLight>& GetLights() const
        {
            return m_lights;
        }
        [[nodiscard]] const RenderFrameHeaderV5& GetAcceptedHeader() const
        {
            return m_acceptedHeader;
        }
        [[nodiscard]] const RenderViewSnapshot& GetView() const
        {
            return m_view;
        }
        [[nodiscard]] const RenderSkySnapshot& GetSky() const
        {
            return m_sky;
        }
        [[nodiscard]] const RenderEnvironmentSnapshot& GetEnvironment() const
        {
            return m_environment;
        }
        [[nodiscard]] const RenderFrameSettings& GetSettings() const
        {
            return m_settings;
        }
        [[nodiscard]] const RenderFrameCaptureRequest& GetCaptureRequest() const
        {
            return m_captureRequest;
        }
        [[nodiscard]] const RenderFeatureSnapshot& GetFeatures() const
        {
            return m_features;
        }
        [[nodiscard]] uint64 GetLastRenderedFrameSequence() const noexcept
        {
            return m_lastRenderedHeader.sequence;
        }
        [[nodiscard]] const RenderViewSnapshot& GetLastRenderedView() const
        {
            return m_lastRenderedView;
        }
        [[nodiscard]] bool HasAcceptedFrame() const noexcept
        {
            return m_hasAcceptedFrame;
        }
        [[nodiscard]] bool IsTemporalHistoryReset() const noexcept
        {
            return m_temporalHistoryReset;
        }
        [[nodiscard]] const std::vector<RenderResourceHandle>&
            GetReferencedResources() const noexcept
        {
            return m_referencedResources;
        }

        [[nodiscard]] size_t GetObjectCount() const
        {
            return m_objects.size();
        }
        [[nodiscard]] size_t GetLightCount() const
        {
            return m_lights.size();
        }
        [[nodiscard]] uint32 GetDrawCount() const noexcept
        {
            return m_drawCount;
        }
        [[nodiscard]] const RenderObject& GetObject(size_t index) const
        {
            return m_objects[index];
        }
        [[nodiscard]] RenderObject& GetMutableObject(size_t index)
        {
            return m_objects[index];
        }
        [[nodiscard]] const RenderLight& GetLight(size_t index) const
        {
            return m_lights[index];
        }

        void AddObject(const RenderObject& object)
        {
            m_objectIndices.insert_or_assign(
                object.entityId, static_cast<uint32>(m_objects.size()));
            m_drawCount += static_cast<uint32>(object.meshBatches.size());
            m_objects.push_back(object);
        }
        void AddLight(const RenderLight& light)
        {
            m_lightIndices.insert_or_assign(
                light.lightId, static_cast<uint32>(m_lights.size()));
            m_lights.push_back(light);
        }

    private:
        [[nodiscard]] RenderFrameApplyResult ApplyFrameState(
            const RenderFrameHeaderV5& header,
            const RenderViewSnapshot& view,
            const std::vector<RenderPrimitiveSnapshot>& primitives,
            const std::vector<RenderLightSnapshot>& lights,
            const RenderSkySnapshot& sky,
            const RenderEnvironmentSnapshot& environment,
            const RenderFrameSettings& settings,
            const RenderFrameCaptureRequest& captureRequest,
            const RenderFeatureSnapshot& features,
            const RenderSceneDatabase& retainedScene,
            const RenderResourceRegistry& registry);
        [[nodiscard]] RenderFrameApplyResult ApplyIncrementalFrameState(
            const RenderFramePacketV5& frame,
            const RenderSceneDatabase& retainedScene,
            const RenderSceneDatabaseChanges& changes,
            const RenderResourceRegistry& registry);
        void RebuildRetainedIndicesAndReferences();
        void AddReferences(const std::vector<RenderResourceHandle>& references);
        void RemoveReferences(const std::vector<RenderResourceHandle>& references);
        void RemoveObjectAt(uint32 index);
        void RemoveLightAt(uint32 index);

        std::vector<RenderObject> m_objects;
        std::vector<RenderLight> m_lights;
        std::unordered_map<uint64, uint32> m_objectIndices;
        std::unordered_map<uint64, uint32> m_lightIndices;
        RenderFrameHeaderV5 m_acceptedHeader{};
        RenderViewSnapshot m_view{};
        RenderSkySnapshot m_sky{};
        RenderEnvironmentSnapshot m_environment{};
        RenderFrameSettings m_settings{};
        RenderFrameCaptureRequest m_captureRequest{};
        RenderFeatureSnapshot m_features{};
        std::vector<RenderResourceHandle> m_referencedResources;
        std::unordered_map<RenderResourceHandle,
                           uint32,
                           RenderResourceHandleHash>
            m_referenceCounts;
        std::unordered_map<RenderResourceHandle,
                           uint32,
                           RenderResourceHandleHash>
            m_referenceIndices;
        std::vector<RenderResourceHandle> m_skyReferences;
        std::vector<RenderResourceHandle> m_environmentReferences;
        std::unordered_map<RenderResourceHandle,
                           uint64,
                           RenderResourceHandleHash>
            m_watchedResourceRevisions;
        RenderDrawPacketCache m_drawPacketCache;
        RenderDrawPacketCacheVersions m_drawPacketCacheVersions{};
        RenderSceneRetainedStats m_retainedStats{};
        RenderSceneMutationTotals m_mutationTotals{};
        bool m_mutationTotalsSaturated = false;
        uint64 m_appliedSceneRevision = 0;
        uint64 m_sourceSceneDatabaseId = 0;
        uint64 m_observedResourceContentRevision = 0;
        uint32 m_drawCount = 0;
        std::vector<uint64> m_gpuSceneChangedObjectIds;
        std::vector<uint64> m_gpuSceneRemovedObjectIds;
        std::vector<uint64> m_pendingRenderedObjectIds;
        std::vector<uint64> m_pendingRemovedObjectIds;
        std::vector<uint64> m_temporalSettleObjectIds;
        bool m_fullGPUSceneMutation = false;
        bool m_fullRenderedObjectRefresh = false;
        bool m_hasAcceptedFrame = false;
        bool m_temporalHistoryReset = true;
        bool m_requiresTemporalSettle = false;
        bool m_acceptedSceneMutated = false;

        RenderFrameHeaderV5 m_lastRenderedHeader{};
        RenderViewSnapshot m_lastRenderedView{};
        std::unordered_map<uint64, Mat4> m_lastRenderedObjectTransforms;
        uint64 m_surfaceCompatibilityKey = 0;
        uint64 m_lastRenderedSurfaceCompatibilityKey = 0;

    };

    static_assert(static_cast<uint8>(RenderFrameApplyCode::Applied) == 0);
    static_assert(static_cast<uint8>(RenderFrameExecutionCode::Rendered) == 0);
} // namespace RVX
