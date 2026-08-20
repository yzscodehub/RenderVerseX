#include "World/ECS/WorldEcsCameraService.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <numbers>
#include <utility>

namespace RVX::WorldECS
{
namespace
{
    constexpr float32 kMinimumOrientationLengthSquared = 0.000001f;
    constexpr float32 kLookAtTargetEpsilonSquared = 0.000000000001f;

    [[nodiscard]] bool ContainsCameraRef(const std::vector<WorldEcsCameraRef>& values,
                                         WorldEcsCameraRef candidate)
    {
        return std::find(values.begin(), values.end(), candidate) != values.end();
    }
} // namespace

WorldEcsCameraService::WorldEcsCameraService(SceneECS::SceneEcsRuntime& runtime)
    : m_runtime(runtime)
{
}

ECS::SceneRuntimeId WorldEcsCameraService::GetSceneRuntimeId() const
{
    return m_runtime.GetSceneRuntimeId();
}

WorldEcsCameraRef WorldEcsCameraService::CreateCamera(const WorldEcsCameraCreateDesc& desc)
{
    const SceneECS::Camera& requested = desc.camera;
    const bool knownProjection = requested.projection == SceneECS::CameraProjection::Perspective ||
                                 requested.projection == SceneECS::CameraProjection::Orthographic;
    const bool validProjection = requested.projection == SceneECS::CameraProjection::Perspective ?
                                     IsValidProjection(requested.verticalFieldOfViewRadians,
                                                       requested.aspectRatio,
                                                       requested.nearPlane,
                                                       requested.farPlane) &&
                                         requested.verticalFieldOfViewRadians <
                                             std::numbers::pi_v<float32> :
                                     IsValidProjection(requested.orthographicHalfHeight,
                                                       requested.aspectRatio,
                                                       requested.nearPlane,
                                                       requested.farPlane);
    const uint8 clearPolicy = static_cast<uint8>(requested.clearPolicy);
    if (!knownProjection || !validProjection || !std::isfinite(requested.exposure) ||
        requested.exposure < 0.0f ||
        !IsFinite(requested.clearColor) || !IsValidViewport(requested.normalizedViewport) ||
        clearPolicy > static_cast<uint8>(SceneECS::CameraClearPolicy::Nothing))
    {
        return {};
    }

    // A camera's render participation is controlled only by Camera.enabled.
    // Keep its ECS entity enabled so transform resolution and value authoring
    // remain available before it is selected as the active view.
    SceneECS::RuntimeEntityDesc entityDesc = desc.entity;
    entityDesc.active.value = true;
    const ECS::EntityHandle entity = m_runtime.CreateEntity(entityDesc);
    if (!entity.IsValid())
    {
        return {};
    }

    SceneECS::Camera camera = requested;
    camera.enabled = false;
    if (camera.cutRevision == 0)
    {
        camera.cutRevision = 1;
    }

    const WorldEcsCameraRef reference = MakeRef(entity);
    if (!m_runtime.AddFragment<SceneECS::Camera>(entity, camera) ||
        !RegisterKnownCamera(reference))
    {
        // A failed setup must never leave a selectable camera behind.  The
        // Scene owns the deferred recycle boundary, so this intentionally
        // records a non-render cleanup request rather than mutating Registry
        // state behind SceneEcsRuntime's public authority.
        static_cast<void>(m_runtime.RequestDestroy(
            entity,
            SceneECS::ToCleanupDomainMask(SceneECS::CleanupDomain::None)));
        return {};
    }

    return reference;
}

WorldEcsCameraRef WorldEcsCameraService::CreateMainCamera(const WorldEcsCameraCreateDesc& desc)
{
    const WorldEcsCameraRef camera = CreateCamera(desc);
    if (!camera.IsValid())
    {
        return {};
    }

    if (!Activate(camera))
    {
        static_cast<void>(m_runtime.RequestDestroy(
            camera.entity,
            SceneECS::ToCleanupDomainMask(SceneECS::CleanupDomain::None)));
        return {};
    }
    return camera;
}

WorldEcsCameraRef WorldEcsCameraService::GetActiveCamera() const
{
    const std::shared_ptr<const SceneECS::FrozenSceneSnapshot> snapshot =
        m_runtime.GetLatestFrozenSnapshot();
    if (snapshot == nullptr || !snapshot->selectedCamera.has_value() ||
        snapshot->selectedCamera->type != SceneECS::FrozenSceneObjectType::Camera)
    {
        return {};
    }

    const WorldEcsCameraRef camera{
        .sceneRuntimeId = snapshot->selectedCamera->sceneRuntimeId,
        .entity = snapshot->selectedCamera->entity,
    };
    return IsOwnedAliveCamera(camera) ? camera : WorldEcsCameraRef{};
}

bool WorldEcsCameraService::Activate(WorldEcsCameraRef camera)
{
    if (!IsOwnedAliveCamera(camera))
    {
        return false;
    }

    const std::optional<SceneECS::Camera> requested = GetCamera(camera);
    if (!requested.has_value())
    {
        return false;
    }

    struct CameraBackup
    {
        WorldEcsCameraRef reference;
        SceneECS::Camera value;
    };

    std::vector<CameraBackup> backups;
    const std::vector<WorldEcsCameraRef> candidates = CollectActivationCandidates(camera);
    try
    {
        backups.reserve(candidates.size());
        for (const WorldEcsCameraRef candidate : candidates)
        {
            const std::optional<SceneECS::Camera> value = GetCamera(candidate);
            if (value.has_value())
            {
                backups.push_back({.reference = candidate, .value = *value});
            }
        }
    }
    catch (...)
    {
        return false;
    }

    if (backups.empty())
    {
        return false;
    }

    const SceneECS::Active* currentActive =
        m_runtime.GetRegistry().TryGet<SceneECS::Active>(camera.entity);
    if (currentActive == nullptr)
    {
        return false;
    }
    const bool previousActive = currentActive->value;
    if (!m_runtime.SetActive(camera.entity, true))
    {
        return false;
    }

    for (const CameraBackup& backup : backups)
    {
        if (backup.reference == camera)
        {
            continue;
        }

        SceneECS::Camera disabled = backup.value;
        disabled.enabled = false;
        if (!WriteCamera(backup.reference, disabled))
        {
            for (const CameraBackup& restore : backups)
            {
                static_cast<void>(WriteCamera(restore.reference, restore.value));
            }
            static_cast<void>(m_runtime.SetActive(camera.entity, previousActive));
            return false;
        }
    }

    SceneECS::Camera active = *requested;
    active.enabled = true;
    active.priority = std::numeric_limits<int32>::max();
    if (!WriteCamera(camera, active))
    {
        for (const CameraBackup& restore : backups)
        {
            static_cast<void>(WriteCamera(restore.reference, restore.value));
        }
        static_cast<void>(m_runtime.SetActive(camera.entity, previousActive));
        return false;
    }

    return true;
}

std::optional<SceneECS::Camera> WorldEcsCameraService::GetCamera(WorldEcsCameraRef camera) const
{
    if (!IsOwnedAliveCamera(camera))
    {
        return std::nullopt;
    }

    const SceneECS::Camera* value = m_runtime.GetRegistry().TryGet<SceneECS::Camera>(camera.entity);
    return value != nullptr ? std::optional<SceneECS::Camera>(*value) : std::nullopt;
}

bool WorldEcsCameraService::SetPerspective(WorldEcsCameraRef camera,
                                            float32 verticalFieldOfViewRadians,
                                            float32 aspectRatio,
                                            float32 nearPlane,
                                            float32 farPlane)
{
    if (!IsValidProjection(verticalFieldOfViewRadians, aspectRatio, nearPlane, farPlane))
    {
        return false;
    }

    if (verticalFieldOfViewRadians >= std::numbers::pi_v<float32>)
    {
        return false;
    }

    std::optional<SceneECS::Camera> value = GetCamera(camera);
    if (!value.has_value())
    {
        return false;
    }
    value->projection = SceneECS::CameraProjection::Perspective;
    value->verticalFieldOfViewRadians = verticalFieldOfViewRadians;
    value->aspectRatio = aspectRatio;
    value->nearPlane = nearPlane;
    value->farPlane = farPlane;
    return WriteCamera(camera, *value);
}

bool WorldEcsCameraService::SetOrthographic(WorldEcsCameraRef camera,
                                             float32 orthographicHalfHeight,
                                             float32 aspectRatio,
                                             float32 nearPlane,
                                             float32 farPlane)
{
    if (!IsValidProjection(orthographicHalfHeight, aspectRatio, nearPlane, farPlane))
    {
        return false;
    }

    std::optional<SceneECS::Camera> value = GetCamera(camera);
    if (!value.has_value())
    {
        return false;
    }
    value->projection = SceneECS::CameraProjection::Orthographic;
    value->orthographicHalfHeight = orthographicHalfHeight;
    value->aspectRatio = aspectRatio;
    value->nearPlane = nearPlane;
    value->farPlane = farPlane;
    return WriteCamera(camera, *value);
}

bool WorldEcsCameraService::SetAspectRatio(WorldEcsCameraRef camera, float32 aspectRatio)
{
    if (!std::isfinite(aspectRatio) || aspectRatio <= 0.0f)
    {
        return false;
    }

    std::optional<SceneECS::Camera> value = GetCamera(camera);
    if (!value.has_value())
    {
        return false;
    }
    value->aspectRatio = aspectRatio;
    return WriteCamera(camera, *value);
}

bool WorldEcsCameraService::SetPose(WorldEcsCameraRef camera, const WorldEcsCameraPose& pose)
{
    if (!IsOwnedAliveCamera(camera) || !IsFinite(pose.position) || !IsFinite(pose.rotation) ||
        glm::dot(pose.rotation, pose.rotation) <= kMinimumOrientationLengthSquared)
    {
        return false;
    }

    return m_runtime.SetSimulationWorldPose(camera.entity, pose.position, pose.rotation) ==
           SceneECS::SetWorldPoseResult::Applied;
}

bool WorldEcsCameraService::LookAt(WorldEcsCameraRef camera, const Vec3& target)
{
    if (!IsOwnedAliveCamera(camera) || !IsFinite(target))
    {
        return false;
    }

    // Resolve through the Scene authority first so a LookAt issued in the
    // same frame as SetPose observes the camera's actual world-space origin.
    m_runtime.ResolveSimulationTransforms();
    const SceneECS::SimulationWorldTransform* transform =
        m_runtime.GetRegistry().TryGet<SceneECS::SimulationWorldTransform>(camera.entity);
    if (transform == nullptr)
    {
        return false;
    }

    const Vec3 position = Vec3(transform->matrix[3]);
    const Vec3 direction = target - position;
    if (!IsFinite(position) || glm::dot(direction, direction) <= kLookAtTargetEpsilonSquared)
    {
        return false;
    }

    const Vec3 forward = glm::normalize(direction);
    Vec3 up{0.0f, 1.0f, 0.0f};
    if (std::abs(glm::dot(forward, up)) > 0.999f)
    {
        up = {0.0f, 0.0f, 1.0f};
    }

    const Quat rotation = glm::normalize(glm::quat_cast(glm::inverse(glm::lookAt(position, target, up))));
    return IsFinite(rotation) && SetPose(camera, {.position = position, .rotation = rotation});
}

bool WorldEcsCameraService::SetExposure(WorldEcsCameraRef camera, float32 exposure)
{
    if (!std::isfinite(exposure) || exposure < 0.0f)
    {
        return false;
    }

    std::optional<SceneECS::Camera> value = GetCamera(camera);
    if (!value.has_value())
    {
        return false;
    }
    value->exposure = exposure;
    return WriteCamera(camera, *value);
}

bool WorldEcsCameraService::SetViewport(WorldEcsCameraRef camera, const Vec4& normalizedViewport)
{
    if (!IsValidViewport(normalizedViewport))
    {
        return false;
    }

    std::optional<SceneECS::Camera> value = GetCamera(camera);
    if (!value.has_value())
    {
        return false;
    }
    value->normalizedViewport = normalizedViewport;
    return WriteCamera(camera, *value);
}

bool WorldEcsCameraService::SetCullingMask(WorldEcsCameraRef camera, uint32 cullingMask)
{
    std::optional<SceneECS::Camera> value = GetCamera(camera);
    if (!value.has_value())
    {
        return false;
    }
    value->cullingMask = cullingMask;
    return WriteCamera(camera, *value);
}

bool WorldEcsCameraService::SetClearPolicy(WorldEcsCameraRef camera,
                                            SceneECS::CameraClearPolicy clearPolicy,
                                            const Vec4& clearColor)
{
    if (static_cast<uint8>(clearPolicy) > static_cast<uint8>(SceneECS::CameraClearPolicy::Nothing) ||
        !IsFinite(clearColor))
    {
        return false;
    }

    std::optional<SceneECS::Camera> value = GetCamera(camera);
    if (!value.has_value())
    {
        return false;
    }
    value->clearPolicy = clearPolicy;
    value->clearColor = clearColor;
    return WriteCamera(camera, *value);
}

bool WorldEcsCameraService::SetPriority(WorldEcsCameraRef camera, int32 priority)
{
    std::optional<SceneECS::Camera> value = GetCamera(camera);
    if (!value.has_value())
    {
        return false;
    }
    value->priority = priority;
    return WriteCamera(camera, *value);
}

bool WorldEcsCameraService::MarkCut(WorldEcsCameraRef camera)
{
    std::optional<SceneECS::Camera> value = GetCamera(camera);
    if (!value.has_value() || value->cutRevision == std::numeric_limits<uint64>::max())
    {
        return false;
    }

    ++value->cutRevision;
    return WriteCamera(camera, *value);
}

SceneECS::DestroyRequestResult WorldEcsCameraService::RequestDestroy(
    WorldEcsCameraRef camera,
    SceneECS::CleanupDomainMask requiredCleanupDomains)
{
    if (!IsOwnedAliveCamera(camera))
    {
        return SceneECS::DestroyRequestResult::InvalidEntity;
    }

    const SceneECS::DestroyRequestResult result =
        m_runtime.RequestDestroy(camera.entity, requiredCleanupDomains);
    if (result == SceneECS::DestroyRequestResult::Accepted)
    {
        PruneKnownCameras();
    }
    return result;
}

WorldEcsCameraRef WorldEcsCameraService::MakeRef(ECS::EntityHandle entity) const
{
    return {
        .sceneRuntimeId = GetSceneRuntimeId(),
        .entity = entity,
    };
}

bool WorldEcsCameraService::IsOwnedAliveCamera(WorldEcsCameraRef camera) const
{
    if (!camera.IsValid() || camera.sceneRuntimeId != GetSceneRuntimeId())
    {
        return false;
    }

    const ECS::Registry& registry = m_runtime.GetRegistry();
    const SceneECS::EntityLifecycleState* lifecycle =
        registry.TryGet<SceneECS::EntityLifecycleState>(camera.entity);
    return lifecycle != nullptr && lifecycle->phase == SceneECS::EntityLifecyclePhase::Alive &&
           registry.TryGet<SceneECS::Camera>(camera.entity) != nullptr;
}

bool WorldEcsCameraService::IsFinite(const Vec3& value) const
{
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

bool WorldEcsCameraService::IsFinite(const Vec4& value) const
{
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z) &&
           std::isfinite(value.w);
}

bool WorldEcsCameraService::IsFinite(const Quat& value) const
{
    return std::isfinite(value.w) && std::isfinite(value.x) && std::isfinite(value.y) &&
           std::isfinite(value.z);
}

bool WorldEcsCameraService::IsValidProjection(float32 primaryExtent,
                                               float32 aspectRatio,
                                               float32 nearPlane,
                                               float32 farPlane) const
{
    return std::isfinite(primaryExtent) && primaryExtent > 0.0f &&
           std::isfinite(aspectRatio) && aspectRatio > 0.0f &&
           std::isfinite(nearPlane) && nearPlane > 0.0f &&
           std::isfinite(farPlane) && farPlane > nearPlane;
}

bool WorldEcsCameraService::IsValidViewport(const Vec4& normalizedViewport) const
{
    return IsFinite(normalizedViewport) && normalizedViewport.x >= 0.0f &&
           normalizedViewport.y >= 0.0f && normalizedViewport.z > 0.0f &&
           normalizedViewport.w > 0.0f && normalizedViewport.x + normalizedViewport.z <= 1.0f &&
           normalizedViewport.y + normalizedViewport.w <= 1.0f;
}

bool WorldEcsCameraService::WriteCamera(WorldEcsCameraRef camera, const SceneECS::Camera& value)
{
    return IsOwnedAliveCamera(camera) && m_runtime.SetFragment<SceneECS::Camera>(camera.entity, value);
}

bool WorldEcsCameraService::RegisterKnownCamera(WorldEcsCameraRef camera)
{
    try
    {
        if (!ContainsCameraRef(m_knownCameraRefs, camera))
        {
            m_knownCameraRefs.push_back(camera);
        }
        return true;
    }
    catch (...)
    {
        return false;
    }
}

void WorldEcsCameraService::PruneKnownCameras()
{
    std::erase_if(m_knownCameraRefs,
                  [this](WorldEcsCameraRef camera) { return !IsOwnedAliveCamera(camera); });
}

std::vector<WorldEcsCameraRef> WorldEcsCameraService::CollectActivationCandidates(
    WorldEcsCameraRef target) const
{
    std::vector<WorldEcsCameraRef> candidates;
    try
    {
        candidates.reserve(m_knownCameraRefs.size() + 1u);
        candidates.push_back(target);
        for (const WorldEcsCameraRef known : m_knownCameraRefs)
        {
            if (!ContainsCameraRef(candidates, known))
            {
                candidates.push_back(known);
            }
        }

        const std::shared_ptr<const SceneECS::FrozenSceneSnapshot> snapshot =
            m_runtime.GetLatestFrozenSnapshot();
        if (snapshot != nullptr && snapshot->sceneRuntimeId == GetSceneRuntimeId())
        {
            candidates.reserve(candidates.size() + snapshot->cameras.size());
            for (const SceneECS::FrozenSceneCamera& frozen : snapshot->cameras)
            {
                const WorldEcsCameraRef frozenRef{
                    .sceneRuntimeId = frozen.id.sceneRuntimeId,
                    .entity = frozen.id.entity,
                };
                if (!ContainsCameraRef(candidates, frozenRef))
                {
                    candidates.push_back(frozenRef);
                }
            }
        }
    }
    catch (...)
    {
        return {};
    }
    return candidates;
}
} // namespace RVX::WorldECS
