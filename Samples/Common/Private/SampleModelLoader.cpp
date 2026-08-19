/** @file SampleModelLoader.cpp @brief Pure-ECS sample model request adapter. */

#include "Samples/SampleModelLoader.h"

#include "Scene/ECS/Fragments.h"
#include "Scene/ECS/RenderFragments.h"
#include "Scene/ECS/TransformMath.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <system_error>
#include <utility>
#include <vector>

namespace RVX
{
namespace
{
    [[nodiscard]] bool IsFinite(const Vec3& value)
    {
        return std::isfinite(value.x) && std::isfinite(value.y) &&
               std::isfinite(value.z);
    }

    [[nodiscard]] bool IsFinite(const Quat& value)
    {
        return std::isfinite(value.w) && std::isfinite(value.x) &&
               std::isfinite(value.y) && std::isfinite(value.z);
    }

    [[nodiscard]] bool IsFinite(const Mat4& matrix)
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

    [[nodiscard]] bool IsFiniteLocalTransform(const SceneECS::LocalTransform& transform)
    {
        return IsFinite(transform.translation) && IsFinite(transform.rotation) &&
               IsFinite(transform.scale) &&
               glm::dot(transform.rotation, transform.rotation) > 0.000001f;
    }

    enum class LocalMatrixResolutionState : uint8
    {
        Visiting = 0,
        Resolved,
        Failed,
    };

    /**
     * @brief Resolve an up-to-date world matrix directly from local fragments.
     *
     * This intentionally avoids TransformHierarchy's cached resolved values:
     * setup code can call bounds immediately after an ancestor local write.
     */
    [[nodiscard]] bool TryResolveCurrentLocalWorldMatrix(
        const ECS::Registry& registry,
        ECS::EntityHandle entity,
        std::map<ECS::EntityHandle, LocalMatrixResolutionState>& states,
        std::map<ECS::EntityHandle, Mat4>& matrices,
        Mat4& outMatrix)
    {
        const auto state = states.find(entity);
        if (state != states.end())
        {
            if (state->second != LocalMatrixResolutionState::Resolved)
            {
                return false;
            }
            outMatrix = matrices.at(entity);
            return true;
        }

        if (!entity.IsValid() || !registry.IsAlive(entity))
        {
            return false;
        }

        states.emplace(entity, LocalMatrixResolutionState::Visiting);
        const SceneECS::EntityLifecycleState* lifecycle =
            registry.TryGet<SceneECS::EntityLifecycleState>(entity);
        const SceneECS::LocalTransform* local =
            registry.TryGet<SceneECS::LocalTransform>(entity);
        if (lifecycle == nullptr ||
            !registry.IsFragmentEnabled<SceneECS::EntityLifecycleState>(entity) ||
            lifecycle->phase != SceneECS::EntityLifecyclePhase::Alive ||
            local == nullptr ||
            !registry.IsFragmentEnabled<SceneECS::LocalTransform>(entity) ||
            !IsFiniteLocalTransform(*local))
        {
            states[entity] = LocalMatrixResolutionState::Failed;
            return false;
        }

        Mat4 resolved = SceneECS::MakeLocalTransformMatrix(*local);
        if (!IsFinite(resolved))
        {
            states[entity] = LocalMatrixResolutionState::Failed;
            return false;
        }

        const SceneECS::ParentRelation* relation =
            registry.TryGet<SceneECS::ParentRelation>(entity);
        const ECS::EntityHandle parent =
            relation != nullptr && registry.IsFragmentEnabled<SceneECS::ParentRelation>(entity)
                ? relation->parent
                : ECS::EntityHandle::Invalid();
        if (parent.IsValid())
        {
            Mat4 parentMatrix(1.0f);
            if (!TryResolveCurrentLocalWorldMatrix(
                    registry, parent, states, matrices, parentMatrix))
            {
                states[entity] = LocalMatrixResolutionState::Failed;
                return false;
            }
            resolved = parentMatrix * resolved;
            if (!IsFinite(resolved))
            {
                states[entity] = LocalMatrixResolutionState::Failed;
                return false;
            }
        }

        matrices.insert_or_assign(entity, resolved);
        states[entity] = LocalMatrixResolutionState::Resolved;
        outMatrix = resolved;
        return true;
    }

    [[nodiscard]] bool IsCpuReadyState(
        ResourceSceneAdapters::EcsSceneAssetLoadState state) noexcept
    {
        using State = ResourceSceneAdapters::EcsSceneAssetLoadState;
        return state == State::CPUReadyHidden ||
               state == State::MinimumResidentPendingPresentation ||
               state == State::MinimumResident || state == State::Streaming ||
               state == State::FullyResident;
    }
} // namespace

bool LoadedSampleModel::IsCPUReady() const noexcept
{
    return request.IsValid() && status.sceneRuntimeId == request.sceneRuntimeId &&
           IsCpuReadyState(status.state) &&
           status.modelMetadata.HasPublishedSource();
}

bool LoadedSampleModel::IsFullyResident() const noexcept
{
    return IsCPUReady() &&
           status.state == ResourceSceneAdapters::EcsSceneAssetLoadState::FullyResident;
}

bool TryComputeSampleModelRenderableWorldBounds(
    const LoadedSampleModel& model,
    const SceneECS::SceneEcsRuntime& scene,
    AABB& outBounds)
{
    outBounds.Reset();
    if (!model.request.IsValid() ||
        model.request.sceneRuntimeId != scene.GetSceneRuntimeId() ||
        model.status.sceneRuntimeId != model.request.sceneRuntimeId ||
        !model.status.rootEntity.IsValid() || model.status.members.empty() ||
        std::find(model.status.members.begin(), model.status.members.end(),
                  model.status.rootEntity) == model.status.members.end())
    {
        return false;
    }

    const ECS::Registry& registry = scene.GetRegistry();
    std::map<ECS::EntityHandle, LocalMatrixResolutionState> matrixStates;
    std::map<ECS::EntityHandle, Mat4> matrices;
    bool foundRenderable = false;

    for (const ECS::EntityHandle member : model.status.members)
    {
        // A member handle is valid only in its declared Scene. GetEntityRef
        // also validates the exact handle generation before any fragment read.
        if (!member.IsValid() || !registry.IsAlive(member) ||
            !scene.GetEntityRef(member).IsValid())
        {
            outBounds.Reset();
            return false;
        }

        Mat4 worldMatrix(1.0f);
        if (!TryResolveCurrentLocalWorldMatrix(
                registry, member, matrixStates, matrices, worldMatrix))
        {
            outBounds.Reset();
            return false;
        }

        const SceneECS::Mesh* mesh = registry.TryGet<SceneECS::Mesh>(member);
        if (mesh == nullptr)
        {
            continue;
        }
        const SceneECS::Bounds* bounds = registry.TryGet<SceneECS::Bounds>(member);
        const SceneECS::Active* active = registry.TryGet<SceneECS::Active>(member);
        const SceneECS::EntityLifecycleState* lifecycle =
            registry.TryGet<SceneECS::EntityLifecycleState>(member);
        if (bounds == nullptr || active == nullptr || lifecycle == nullptr ||
            !registry.IsFragmentEnabled<SceneECS::Mesh>(member) ||
            !registry.IsFragmentEnabled<SceneECS::Bounds>(member) ||
            !registry.IsFragmentEnabled<SceneECS::Active>(member) ||
            !registry.IsFragmentEnabled<SceneECS::EntityLifecycleState>(member) ||
            !mesh->meshAssetId.IsValid() || mesh->submeshCount == 0 ||
            !IsFinite(bounds->center) || !IsFinite(bounds->extents) ||
            bounds->extents.x < 0.0f || bounds->extents.y < 0.0f ||
            bounds->extents.z < 0.0f)
        {
            outBounds.Reset();
            return false;
        }

        // Visibility is deliberately not consulted: coordinator adoption keeps
        // renderables CPU-hidden until presentation proof, but setup still
        // needs their structural bound. Registry enablement and Active retain
        // their normal gameplay eligibility meaning.
        if (!registry.IsEnabled(member) || !active->value ||
            lifecycle->phase != SceneECS::EntityLifecyclePhase::Alive)
        {
            continue;
        }

        const AABB localBounds(
            bounds->center - bounds->extents,
            bounds->center + bounds->extents);
        const AABB worldBounds = localBounds.Transformed(worldMatrix);
        const Vec3 minimum = worldBounds.GetMin();
        const Vec3 maximum = worldBounds.GetMax();
        if (!localBounds.IsValid() || !worldBounds.IsValid() || !IsFinite(minimum) ||
            !IsFinite(maximum))
        {
            outBounds.Reset();
            return false;
        }
        outBounds.Expand(worldBounds);
        foundRenderable = true;
    }

    return foundRenderable && outBounds.IsValid();
}

SampleModelLoader::SampleModelLoader(IWorldEcsRuntimeServices& runtimeServices,
                                     const SceneECS::SceneEcsRuntime& scene) noexcept
    : SampleModelLoader(runtimeServices,
                        scene,
                        SampleAssetRegistry{},
                        std::vector<SampleModelExpectedContentIdentity>{})
{
}

SampleModelLoader::SampleModelLoader(
    IWorldEcsRuntimeServices& runtimeServices,
    const SceneECS::SceneEcsRuntime& scene,
    std::vector<SampleModelExpectedContentIdentity> expectedContentIdentities)
    : SampleModelLoader(runtimeServices,
                        scene,
                        SampleAssetRegistry{},
                        std::move(expectedContentIdentities))
{
}

SampleModelLoader::SampleModelLoader(IWorldEcsRuntimeServices& runtimeServices,
                                     const SceneECS::SceneEcsRuntime& scene,
                                     SampleAssetRegistry assetRegistry)
    : SampleModelLoader(runtimeServices, scene, std::move(assetRegistry), {})
{
}

SampleModelLoader::SampleModelLoader(
    IWorldEcsRuntimeServices& runtimeServices,
    const SceneECS::SceneEcsRuntime& scene,
    SampleAssetRegistry assetRegistry,
    std::vector<SampleModelExpectedContentIdentity> expectedContentIdentities)
    : m_runtimeServices(runtimeServices)
    , m_scene(scene)
    , m_assetRegistry(std::move(assetRegistry))
{
    for (SampleModelExpectedContentIdentity& expectation : expectedContentIdentities)
    {
        if (expectation.identity.IsValid())
        {
            m_expectedContentIdentities.try_emplace(
                MakeLogicalPathKey(expectation.path), std::move(expectation.identity));
        }
    }
}

bool SampleModelLoader::RequestByAssetId(
    std::string_view assetId,
    LoadedSampleModel& outModel,
    std::string& outError,
    SampleAssetRegistryLookupResult* outLookup) const
{
    if (outModel.request.IsValid())
    {
        outError = "Model request output already owns a live ECS request reference.";
        return false;
    }

    const SampleAssetRegistryLookupResult lookup =
        m_assetRegistry.Lookup(assetId, SampleAssetKind::Model);
    if (outLookup != nullptr)
    {
        *outLookup = lookup;
    }
    if (!lookup.IsFound())
    {
        outError = lookup.code == SampleAssetRegistryLookupCode::KindMismatch
                       ? "SampleAssetRegistry kind mismatch for model id '" +
                             std::string(assetId) + "': actual=" +
                             GetSampleAssetKindName(lookup.actualKind)
                       : "SampleAssetRegistry unknown model id: " + std::string(assetId);
        return false;
    }

    Resource::ResourceLoadOptions options;
    if (lookup.entry->contentIdentity.IsValid())
    {
        options.expectedContentIdentity = lookup.entry->contentIdentity;
    }
    return RequestResolved(lookup.entry->resolvedPath,
                           std::move(options),
                           outModel,
                           outError);
}

bool SampleModelLoader::Request(const std::filesystem::path& path,
                                LoadedSampleModel& outModel,
                                std::string& outError) const
{
    if (outModel.request.IsValid())
    {
        outError = "Model request output already owns a live ECS request reference.";
        return false;
    }

    Resource::ResourceLoadOptions options;
    const auto expected = m_expectedContentIdentities.find(MakeLogicalPathKey(path));
    if (expected != m_expectedContentIdentities.end())
    {
        options.expectedContentIdentity = expected->second;
    }
    return RequestResolved(path, std::move(options), outModel, outError);
}

bool SampleModelLoader::RequestAdditionalInstance(
    const LoadedSampleModel& source,
    LoadedSampleModel& outModel,
    std::string& outError) const
{
    if (outModel.request.IsValid())
    {
        outError = "Additional model instance output already owns a live ECS request reference.";
        return false;
    }
    if (!source.request.IsValid() ||
        source.request.sceneRuntimeId != m_scene.GetSceneRuntimeId())
    {
        outError = "Additional model instances require a live source from this exact ECS Scene.";
        return false;
    }

    const std::optional<ResourceSceneAdapters::EcsSceneAssetLoadStatus> sourceStatus =
        m_runtimeServices.GetModelStatus(source.request);
    if (!sourceStatus.has_value() ||
        sourceStatus->sceneRuntimeId != source.request.sceneRuntimeId ||
        sourceStatus->request.assetKey.resourceType != Resource::ResourceType::Model ||
        sourceStatus->request.assetKey.canonicalPath.empty())
    {
        outError = "Additional model instance source is stale, foreign, or lacks an exact model descriptor.";
        return false;
    }

    return RequestResolved(sourceStatus->request.assetKey.canonicalPath,
                           sourceStatus->request.options,
                           outModel,
                           outError);
}

bool SampleModelLoader::RequestResolved(const std::filesystem::path& path,
                                        Resource::ResourceLoadOptions resourceOptions,
                                        LoadedSampleModel& outModel,
                                        std::string& outError) const
{
    if (path.empty())
    {
        outError = "Model path must not be empty";
        return false;
    }

    ResourceSceneAdapters::EcsModelAssetLoadDesc desc;
    desc.path = path.string();
    desc.resourceOptions = std::move(resourceOptions);
    desc.expectedSceneRuntimeId = m_scene.GetSceneRuntimeId();
    if (!desc.expectedSceneRuntimeId.IsValid())
    {
        outError = "The target ECS Scene has no valid runtime identity.";
        return false;
    }

    LoadedSampleModel requested;
    requested.request = m_runtimeServices.RequestModel(std::move(desc), outError);
    if (!requested.request.IsValid() ||
        requested.request.sceneRuntimeId != m_scene.GetSceneRuntimeId())
    {
        if (outError.empty())
        {
            outError = "Model runtime service returned an invalid or foreign request reference.";
        }
        return false;
    }
    requested.sourcePath = path;
    static_cast<void>(UpdateReadiness(requested));
    outModel = std::move(requested);
    return true;
}

ResourceSceneAdapters::EcsSceneAssetLoadStatus SampleModelLoader::UpdateReadiness(
    LoadedSampleModel& model) const
{
    const ECS::SceneRuntimeId expectedRuntimeId = m_scene.GetSceneRuntimeId();
    const std::optional<ResourceSceneAdapters::EcsSceneAssetLoadStatus> current =
        model.request.IsValid() && model.request.sceneRuntimeId == expectedRuntimeId
            ? m_runtimeServices.GetModelStatus(model.request)
            : std::nullopt;
    if (!current.has_value() || current->sceneRuntimeId != expectedRuntimeId)
    {
        model.status = {};
        model.status.state = ResourceSceneAdapters::EcsSceneAssetLoadState::Failed;
        model.status.sceneRuntimeId = expectedRuntimeId;
        model.status.request.error = {
            Resource::ResourceLoadErrorCode::InvalidRequest,
            "Model load handle is stale or belongs to a foreign ECS Scene.",
        };
        model.status.diagnostic = model.status.request.error.message;
        return model.status;
    }

    model.status = *current;
    model.resourceRequestId = current->request.requestId;
    const Resource::ResourceContentVerificationReceipt& receipt =
        current->modelMetadata.contentVerificationReceipt;
    if (current->modelMetadata.HasPublishedSource())
    {
        m_contentVerificationReceipts.try_emplace(
            MakeLogicalPathKey(model.sourcePath), receipt);
    }
    return model.status;
}

bool SampleModelLoader::Cancel(LoadedSampleModel& model) const
{
    if (!model.request.IsValid() ||
        model.request.sceneRuntimeId != m_scene.GetSceneRuntimeId())
    {
        return false;
    }
    if (!m_runtimeServices.CancelModel(model.request))
    {
        return false;
    }
    model = {};
    return true;
}

bool SampleModelLoader::CancelForRetirement(
    LoadedSampleModel& model,
    SampleModelCancellation& outCancellation) const
{
    if (!model.request.IsValid() ||
        model.request.sceneRuntimeId != m_scene.GetSceneRuntimeId())
    {
        return false;
    }

    const bool cancelled = m_runtimeServices.CancelModel(model.request);
    if (!cancelled)
    {
        return false;
    }

    SampleModelCancellation cancellation{
        .request = model.request,
        .resourceRequestId = model.resourceRequestId,
        .requested = true,
    };
    outCancellation = std::move(cancellation);
    model = {};
    return true;
}

bool SampleModelLoader::IsRetirementComplete(
    const SampleModelCancellation& cancellation) const
{
    return cancellation.IsValid() &&
           cancellation.request.sceneRuntimeId == m_scene.GetSceneRuntimeId() &&
           !m_runtimeServices.GetModelStatus(cancellation.request).has_value();
}

std::optional<ResourceSceneAdapters::EcsSceneAssetLoadStatus>
SampleModelLoader::QueryRetirementStatus(
    const SampleModelCancellation& cancellation) const
{
    if (!cancellation.IsValid() ||
        cancellation.request.sceneRuntimeId != m_scene.GetSceneRuntimeId())
    {
        return std::nullopt;
    }
    return m_runtimeServices.GetModelStatus(cancellation.request);
}

std::optional<Resource::ResourceContentVerificationReceipt>
SampleModelLoader::GetContentVerificationReceipt(const std::filesystem::path& path) const
{
    const auto receipt = m_contentVerificationReceipts.find(MakeLogicalPathKey(path));
    return receipt != m_contentVerificationReceipts.end()
               ? std::optional<Resource::ResourceContentVerificationReceipt>(receipt->second)
               : std::nullopt;
}

std::string SampleModelLoader::MakeLogicalPathKey(const std::filesystem::path& path)
{
    std::error_code error;
    std::filesystem::path absolute = path;
    if (!absolute.is_absolute())
    {
        absolute = std::filesystem::absolute(absolute, error);
    }
    if (!error)
    {
        const std::filesystem::path canonical =
            std::filesystem::weakly_canonical(absolute, error);
        if (!error)
        {
            absolute = canonical;
        }
    }

    std::string key = absolute.lexically_normal().generic_string();
#if defined(_WIN32)
    std::transform(key.begin(), key.end(), key.begin(),
                   [](unsigned char character)
                   {
                       return static_cast<char>(std::tolower(character));
                   });
#endif
    return key;
}
} // namespace RVX
