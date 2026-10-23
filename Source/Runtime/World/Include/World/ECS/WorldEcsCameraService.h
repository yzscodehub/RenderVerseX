#pragma once

/**
 * @file WorldEcsCameraService.h
 * @brief Handle-and-value camera authoring facade over one pure Scene ECS runtime.
 */

#include "Scene/ECS/RenderFragments.h"
#include "Scene/ECS/SceneEcsRuntime.h"

#include <optional>
#include <vector>

namespace RVX::WorldECS
{
    /**
     * @brief Generation-qualified camera identity that cannot cross Scene runtime boundaries.
     *
     * This is intentionally a value identity rather than an object facade.  The
     * Scene ECS registry remains the only owner of camera and transform data.
     */
    struct WorldEcsCameraRef
    {
        ECS::SceneRuntimeId sceneRuntimeId;
        ECS::EntityHandle entity = ECS::EntityHandle::Invalid();

        [[nodiscard]] constexpr bool IsValid() const
        {
            return sceneRuntimeId.IsValid() && entity.IsValid();
        }

        constexpr bool operator==(const WorldEcsCameraRef&) const = default;
    };

    /** @brief Value inputs for creating one camera entity. */
    struct WorldEcsCameraCreateDesc
    {
        SceneECS::RuntimeEntityDesc entity;
        SceneECS::Camera camera;
    };

    /** @brief World-space position and orientation applied through the Scene hierarchy. */
    struct WorldEcsCameraPose
    {
        Vec3 position{0.0f};
        Quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    };

    /**
     * @brief Value-only camera authoring API for one World-owned Scene ECS runtime.
     *
     * All mutations route through SceneEcsRuntime's public value operations.
     * The short list of service-created identities is a non-authoritative
     * enumeration cache used only to disable a camera that was activated before
     * the first frozen snapshot; Camera.enabled and Camera.priority remain the
     * sole selection authority.
     */
    class WorldEcsCameraService
    {
    public:
        explicit WorldEcsCameraService(SceneECS::SceneEcsRuntime& runtime);

        WorldEcsCameraService(const WorldEcsCameraService&) = delete;
        WorldEcsCameraService& operator=(const WorldEcsCameraService&) = delete;
        WorldEcsCameraService(WorldEcsCameraService&&) = delete;
        WorldEcsCameraService& operator=(WorldEcsCameraService&&) = delete;

        [[nodiscard]] ECS::SceneRuntimeId GetSceneRuntimeId() const;

        /** @brief Create an initially inactive camera. */
        [[nodiscard]] WorldEcsCameraRef CreateCamera(const WorldEcsCameraCreateDesc& desc = {});
        /** @brief Create and exclusively activate one main camera. */
        [[nodiscard]] WorldEcsCameraRef CreateMainCamera(const WorldEcsCameraCreateDesc& desc = {});

        /** @brief Return the selected camera in the latest immutable frozen Scene snapshot. */
        [[nodiscard]] WorldEcsCameraRef GetActiveCamera() const;
        /** @brief Exclusively select a valid, alive camera for the next frozen Scene snapshot. */
        [[nodiscard]] bool Activate(WorldEcsCameraRef camera);

        /** @brief Copy the current Camera fragment as a value, never as a mutable reference. */
        [[nodiscard]] std::optional<SceneECS::Camera> GetCamera(WorldEcsCameraRef camera) const;

        [[nodiscard]] bool SetPerspective(WorldEcsCameraRef camera,
                                          float32 verticalFieldOfViewRadians,
                                          float32 aspectRatio,
                                          float32 nearPlane,
                                          float32 farPlane);
        [[nodiscard]] bool SetOrthographic(WorldEcsCameraRef camera,
                                           float32 orthographicHalfHeight,
                                           float32 aspectRatio,
                                           float32 nearPlane,
                                           float32 farPlane);
        [[nodiscard]] bool SetAspectRatio(WorldEcsCameraRef camera, float32 aspectRatio);
        [[nodiscard]] bool SetPose(WorldEcsCameraRef camera, const WorldEcsCameraPose& pose);
        [[nodiscard]] bool LookAt(WorldEcsCameraRef camera, const Vec3& target);

        [[nodiscard]] bool SetExposure(WorldEcsCameraRef camera, float32 exposure);
        [[nodiscard]] bool SetViewport(WorldEcsCameraRef camera, const Vec4& normalizedViewport);
        [[nodiscard]] bool SetCullingMask(WorldEcsCameraRef camera, uint32 cullingMask);
        [[nodiscard]] bool SetClearPolicy(WorldEcsCameraRef camera,
                                          SceneECS::CameraClearPolicy clearPolicy,
                                          const Vec4& clearColor);
        [[nodiscard]] bool SetPriority(WorldEcsCameraRef camera, int32 priority);
        /**
         * @brief Advance the temporal-discontinuity revision without allowing a zero wrap.
         * @return False after uint64 exhaustion; callers must recreate the camera instead.
         */
        [[nodiscard]] bool MarkCut(WorldEcsCameraRef camera);

        [[nodiscard]] SceneECS::DestroyRequestResult RequestDestroy(
            WorldEcsCameraRef camera,
            SceneECS::CleanupDomainMask requiredCleanupDomains =
                SceneECS::ToCleanupDomainMask(SceneECS::CleanupDomain::None));

    private:
        [[nodiscard]] WorldEcsCameraRef MakeRef(ECS::EntityHandle entity) const;
        [[nodiscard]] bool IsOwnedAliveCamera(WorldEcsCameraRef camera) const;
        [[nodiscard]] bool IsFinite(const Vec3& value) const;
        [[nodiscard]] bool IsFinite(const Vec4& value) const;
        [[nodiscard]] bool IsFinite(const Quat& value) const;
        [[nodiscard]] bool IsValidProjection(float32 primaryExtent,
                                             float32 aspectRatio,
                                             float32 nearPlane,
                                             float32 farPlane) const;
        [[nodiscard]] bool IsValidViewport(const Vec4& normalizedViewport) const;
        [[nodiscard]] bool WriteCamera(WorldEcsCameraRef camera, const SceneECS::Camera& value);
        [[nodiscard]] bool RegisterKnownCamera(WorldEcsCameraRef camera);
        void PruneKnownCameras();
        [[nodiscard]] std::vector<WorldEcsCameraRef> CollectActivationCandidates(
            WorldEcsCameraRef target) const;

        SceneECS::SceneEcsRuntime& m_runtime;
        /** @brief Non-authoritative enumeration aid; no camera data or selection state is cached here. */
        std::vector<WorldEcsCameraRef> m_knownCameraRefs;
    };
} // namespace RVX::WorldECS
