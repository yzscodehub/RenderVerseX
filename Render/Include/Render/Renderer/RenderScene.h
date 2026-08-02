#pragma once

/**
 * @file RenderScene.h
 * @brief Render-owned transactional copy of immutable frame packets.
 */

#include "Core/Math/AABB.h"
#include "Core/MathTypes.h"
#include "Core/Types.h"
#include "RenderContracts/RenderFramePacket.h"
#include "RenderContracts/RenderMaterial.h"
#include "Render/Renderer/MeshBatch.h"

#include <unordered_map>
#include <vector>

namespace RVX
{
    class RenderResourceRegistry;

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

        uint64 entityId = 0;
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
            return !skinningMatrices.empty();
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
        bool castsShadow = false;
    };

    /** @brief Transactional packet consumer that owns every retained value. */
    class RenderScene
    {
    public:
        RenderScene() = default;

        void Clear();
        [[nodiscard]] RenderFrameApplyResult ApplyFramePacket(
            const RenderFramePacket& packet,
            const RenderResourceRegistry& registry);
        void MarkAcceptedFrameRendered();
        void SetSurfaceCompatibilityKey(uint64 key) noexcept;

        void CullAgainstView(
            const RenderViewSnapshot& view,
            std::vector<uint32>& outVisibleIndices) const;
        void SortVisibleObjects(std::vector<uint32>& visibleIndices,
                                const Vec3& cameraPosition) const;

        [[nodiscard]] const std::vector<RenderObject>& GetObjects() const
        {
            return m_objects;
        }
        [[nodiscard]] const std::vector<RenderLight>& GetLights() const
        {
            return m_lights;
        }
        [[nodiscard]] const RenderFrameHeader& GetAcceptedHeader() const
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
            m_objects.push_back(object);
        }
        void AddLight(const RenderLight& light)
        {
            m_lights.push_back(light);
        }

    private:
        std::vector<RenderObject> m_objects;
        std::vector<RenderLight> m_lights;
        RenderFrameHeader m_acceptedHeader{};
        RenderViewSnapshot m_view{};
        RenderSkySnapshot m_sky{};
        RenderEnvironmentSnapshot m_environment{};
        RenderFrameSettings m_settings{};
        RenderFrameCaptureRequest m_captureRequest{};
        RenderFeatureSnapshot m_features{};
        std::vector<RenderResourceHandle> m_referencedResources;
        bool m_hasAcceptedFrame = false;
        bool m_temporalHistoryReset = true;

        RenderFrameHeader m_lastRenderedHeader{};
        RenderViewSnapshot m_lastRenderedView{};
        std::unordered_map<uint64, Mat4> m_lastRenderedObjectTransforms;
        uint64 m_surfaceCompatibilityKey = 0;
        uint64 m_lastRenderedSurfaceCompatibilityKey = 0;

    };

    static_assert(static_cast<uint8>(RenderFrameApplyCode::Applied) == 0);
    static_assert(static_cast<uint8>(RenderFrameExecutionCode::Rendered) == 0);
} // namespace RVX
