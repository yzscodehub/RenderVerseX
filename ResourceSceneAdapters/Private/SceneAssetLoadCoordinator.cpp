/** @file SceneAssetLoadCoordinator.cpp @brief Async resource-to-Scene coordinator. */

#include "ResourceSceneAdapters/SceneAssetLoadCoordinator.h"

#include "Core/Diagnostics/Trace.h"
#include "Core/Log.h"
#include "RenderContracts/IRenderResourceGateway.h"
#include "Scene/Components/SkyboxComponent.h"
#include "Scene/SceneRuntime.h"

#include <algorithm>
#include <utility>

namespace RVX
{
namespace
{
    enum class DependencyState : uint8
    {
        Ready = 0,
        Pending,
        Failed
    };

    struct SkyboxBindingSnapshot
    {
        bool captured = false;
        SkyboxType type = SkyboxType::Color;
        SceneTextureHandle cubemap;
        SceneTextureHandle equirectangular;
        SceneTextureHandle irradiance;
        SceneTextureHandle prefiltered;
        SceneTextureHandle brdfLUT;
        float32 exposure = 1.0f;
        bool contributesToLighting = false;
    };

    DependencyState InspectTexture(
        Resource::ResourceSubsystem& resources,
        const Resource::TextureHandle& texture,
        std::string& outDiagnostic)
    {
        if (!texture.IsValid() || !texture.IsLoaded())
        {
            outDiagnostic = "Environment contains an unavailable texture dependency";
            return DependencyState::Failed;
        }

        const Resource::RenderResourceResolveResult resolved =
            resources.ResolveRenderResource(
                AssetId{texture.GetId()},
                RenderResourceKind::Texture);
        if (resolved.code == Resource::RenderResourceResolveCode::NotFound)
            return DependencyState::Pending;
        if (resolved.code != Resource::RenderResourceResolveCode::Resolved ||
            resolved.status.code == RenderResourceStatusCode::StaleGeneration ||
            resolved.status.code == RenderResourceStatusCode::InvalidHandle ||
            resolved.status.state == RenderResourcePublicState::Failed ||
            resolved.status.state == RenderResourcePublicState::Released ||
            resolved.status.state == RenderResourcePublicState::Evicting)
        {
            outDiagnostic =
                "Environment texture render resource became invalid";
            return DependencyState::Failed;
        }
        return resolved.status.state == RenderResourcePublicState::GPUReady ||
                       resolved.status.state ==
                           RenderResourcePublicState::ReplacementQueued ||
                       resolved.status.state ==
                           RenderResourcePublicState::Replacing
                   ? DependencyState::Ready
                   : DependencyState::Pending;
    }

    void SetStatus(SceneAssetStatus& status,
                   SceneAssetLifecycle lifecycle,
                   SceneAssetResidency residency,
                   float32 progress)
    {
        if (status.lifecycle != lifecycle || status.residency != residency)
            ++status.revision;
        status.lifecycle = lifecycle;
        status.residency = residency;
        status.progress = std::clamp(progress, 0.0f, 1.0f);
        if (lifecycle != SceneAssetLifecycle::Failed)
        {
            status.error = {};
            status.diagnostic.clear();
        }
    }
} // namespace

struct SceneAssetLoadCoordinator::Entry
{
    SceneAssetLoadHandle handle = InvalidSceneAssetLoadHandle;
    SceneAssetKind kind = SceneAssetKind::Model;
    SceneAssetStatus status;
    Resource::ResourceLoadHandle<Resource::ModelResource> modelRequest;
    Resource::ResourceLoadHandle<Resource::EnvironmentResource>
        environmentRequest;
    Resource::ResourceHandle<Resource::ModelResource> model;
    Resource::EnvironmentHandle environment;
    SceneAssetInstantiationOptions instantiationOptions;
    SceneAssetInstance instance;
    ComponentHandle targetSkybox = InvalidComponentHandle;
    SkyboxBindingSnapshot skyboxSnapshot;
    bool activateWhenResident = true;
    bool renderablesActivated = false;
    bool environmentBound = false;
    bool cpuReadyMilestone = false;
    bool minimumResidentMilestone = false;
    bool fullyResidentMilestone = false;
    Diagnostics::TraceContext traceContext;
};

SceneAssetLoadCoordinator::SceneAssetLoadCoordinator(
    Scene& scene,
    Resource::ResourceSubsystem& resources) noexcept
    : m_scene(scene), m_resources(resources)
{
}

SceneAssetLoadCoordinator::~SceneAssetLoadCoordinator() = default;

SceneAssetLoadHandle SceneAssetLoadCoordinator::AllocateEntry(
    std::unique_ptr<Entry> entry)
{
    const SceneAssetLoadHandle handle = m_handles.Allocate();
    const size_t index = handle.GetIndex();
    if (index >= m_entries.size())
        m_entries.resize(index + 1);
    entry->handle = handle;
    m_entries[index] = std::move(entry);
    return handle;
}

SceneAssetLoadCoordinator::Entry* SceneAssetLoadCoordinator::Resolve(
    SceneAssetLoadHandle handle)
{
    if (!m_handles.IsValid(handle) || handle.GetIndex() >= m_entries.size())
        return nullptr;
    return m_entries[handle.GetIndex()].get();
}

const SceneAssetLoadCoordinator::Entry* SceneAssetLoadCoordinator::Resolve(
    SceneAssetLoadHandle handle) const
{
    if (!m_handles.IsValid(handle) || handle.GetIndex() >= m_entries.size())
        return nullptr;
    return m_entries[handle.GetIndex()].get();
}

SceneAssetLoadHandle SceneAssetLoadCoordinator::RequestModel(
    SceneModelLoadDesc desc,
    std::string& outError)
{
    outError.clear();
    if (!m_scene.IsInitialized() || !m_scene.IsUpdateThread() ||
        m_scene.IsUpdating() || desc.path.empty())
    {
        outError =
            "Model request requires an initialized idle Scene update thread and a non-empty path";
        return InvalidSceneAssetLoadHandle;
    }

    auto entry = std::make_unique<Entry>();
    entry->kind = SceneAssetKind::Model;
    entry->instantiationOptions = std::move(desc.instantiationOptions);
    entry->activateWhenResident = desc.activateWhenResident;
    entry->modelRequest = m_resources.RequestAsync<Resource::ModelResource>(
        desc.path,
        std::move(desc.resourceOptions));
    if (!entry->modelRequest)
    {
        outError = "ResourceSubsystem rejected the asynchronous model request";
        return InvalidSceneAssetLoadHandle;
    }
    entry->traceContext =
        entry->modelRequest.GetSnapshot().options.traceContext;
    return AllocateEntry(std::move(entry));
}

SceneAssetLoadHandle SceneAssetLoadCoordinator::RequestEnvironment(
    SceneEnvironmentLoadDesc desc,
    std::string& outError)
{
    outError.clear();
    if (!m_scene.IsInitialized() || !m_scene.IsUpdateThread() ||
        m_scene.IsUpdating() || desc.path.empty())
    {
        outError =
            "Environment request requires an initialized idle Scene update thread and a non-empty path";
        return InvalidSceneAssetLoadHandle;
    }

    auto* skybox = dynamic_cast<SkyboxComponent*>(
        m_scene.ResolveComponent(desc.targetSkybox));
    if (!skybox)
    {
        outError = "Environment request target is not a live SkyboxComponent";
        return InvalidSceneAssetLoadHandle;
    }
    for (const auto& current : m_entries)
    {
        if (current && current->kind == SceneAssetKind::Environment &&
            current->targetSkybox == desc.targetSkybox &&
            current->status.lifecycle != SceneAssetLifecycle::Failed &&
            current->status.lifecycle != SceneAssetLifecycle::Cancelled)
        {
            outError = "Skybox already has an active environment request";
            return InvalidSceneAssetLoadHandle;
        }
    }

    uint64 canonicalHash = 0;
    Resource::ResourceLoadError preparationError;
    Resource::ResourceLoadPreparationStateRef state =
        Resource::EnvironmentLoader::CreatePreparationState(
            desc.environmentOptions,
            canonicalHash,
            preparationError);
    if (!state)
    {
        outError = preparationError.message;
        return InvalidSceneAssetLoadHandle;
    }
    if (desc.resourceOptions.importOptionsHash != 0 &&
        desc.resourceOptions.importOptionsHash != canonicalHash)
    {
        outError =
            "Environment ResourceLoadOptions hash conflicts with its immutable options";
        return InvalidSceneAssetLoadHandle;
    }
    desc.resourceOptions.importOptionsHash = canonicalHash;

    auto entry = std::make_unique<Entry>();
    entry->kind = SceneAssetKind::Environment;
    entry->targetSkybox = desc.targetSkybox;
    entry->skyboxSnapshot.captured = true;
    entry->skyboxSnapshot.type = skybox->GetSkyboxType();
    entry->skyboxSnapshot.cubemap = skybox->GetCubemap();
    entry->skyboxSnapshot.equirectangular = skybox->GetEquirectangular();
    entry->skyboxSnapshot.irradiance = skybox->GetIrradianceMap();
    entry->skyboxSnapshot.prefiltered = skybox->GetPrefilteredMap();
    entry->skyboxSnapshot.brdfLUT = skybox->GetBRDFLUT();
    entry->skyboxSnapshot.exposure = skybox->GetExposure();
    entry->skyboxSnapshot.contributesToLighting =
        skybox->ContributesToLighting();
    entry->environmentRequest =
        m_resources.RequestAsync<Resource::EnvironmentResource>(
            desc.path,
            std::move(desc.resourceOptions),
            std::move(state));
    if (!entry->environmentRequest)
    {
        outError = "ResourceSubsystem rejected the asynchronous environment request";
        return InvalidSceneAssetLoadHandle;
    }
    entry->traceContext =
        entry->environmentRequest.GetSnapshot().options.traceContext;
    return AllocateEntry(std::move(entry));
}

SceneAssetLoadHandle SceneAssetLoadCoordinator::InstantiateModel(
    Resource::ResourceHandle<Resource::ModelResource> model,
    SceneAssetInstantiationOptions options,
    bool activateWhenResident,
    std::string& outError)
{
    outError.clear();
    if (!m_scene.IsInitialized() || !m_scene.IsUpdateThread() ||
        m_scene.IsUpdating() || !model.IsLoaded())
    {
        outError = "Model instance requires a CPU-ready model on an idle Scene update thread";
        return InvalidSceneAssetLoadHandle;
    }

    auto entry = std::make_unique<Entry>();
    entry->kind = SceneAssetKind::Model;
    entry->model = std::move(model);
    entry->instantiationOptions = std::move(options);
    entry->activateWhenResident = activateWhenResident;
    entry->instance = SceneAssetInstantiator::InstantiateModel(
        m_scene,
        *entry->model,
        entry->instantiationOptions);
    if (!entry->instance.IsValid())
    {
        outError = entry->instance.status.diagnostic;
        return InvalidSceneAssetLoadHandle;
    }
    static_cast<void>(SceneAssetInstantiator::SetRenderablesEnabled(
        m_scene,
        entry->instance,
        false));
    entry->status = entry->instance.status;
    return AllocateEntry(std::move(entry));
}

void SceneAssetLoadCoordinator::Fail(
    Entry& entry,
    Resource::ResourceLoadError error,
    std::string diagnostic)
{
    if (entry.kind == SceneAssetKind::Model && entry.instance.rootActor.IsValid())
    {
        static_cast<void>(SceneAssetInstantiator::Cancel(
            m_scene,
            entry.instance));
    }
    if (entry.kind == SceneAssetKind::Environment && entry.environmentBound)
        RestoreEnvironment(entry);
    entry.status.lifecycle = SceneAssetLifecycle::Failed;
    entry.status.residency = SceneAssetResidency::None;
    entry.status.progress = 0.0f;
    entry.status.error = std::move(error);
    entry.status.diagnostic = std::move(diagnostic);
    ++entry.status.revision;
}

bool SceneAssetLoadCoordinator::UpdateModel(Entry& entry)
{
    if (!entry.model.IsValid())
    {
        const Resource::ResourceLoadSnapshot snapshot =
            entry.modelRequest.GetSnapshot();
        entry.status.progress = snapshot.progress.fraction;
        if (snapshot.state == Resource::ResourceLoadState::Failed)
        {
            Fail(entry, snapshot.error, snapshot.error.message);
            return false;
        }
        if (snapshot.state == Resource::ResourceLoadState::Cancelled)
        {
            entry.status.lifecycle = SceneAssetLifecycle::Cancelled;
            entry.status.residency = SceneAssetResidency::None;
            entry.status.error = snapshot.error;
            entry.status.diagnostic = snapshot.error.message;
            ++entry.status.revision;
            return false;
        }
        if (snapshot.state != Resource::ResourceLoadState::Ready)
            return true;

        entry.model = entry.modelRequest.TryGet();
        if (!entry.model.IsLoaded())
        {
            Fail(entry,
                 {Resource::ResourceLoadErrorCode::TypeMismatch,
                  "Published model request did not contain a loaded ModelResource"},
                 "Published model request did not contain a loaded ModelResource");
            return false;
        }
        entry.instance = SceneAssetInstantiator::InstantiateModel(
            m_scene,
            *entry.model,
            entry.instantiationOptions);
        if (!entry.instance.IsValid())
        {
            Fail(entry,
                 entry.instance.status.error,
                 entry.instance.status.diagnostic);
            return false;
        }
        static_cast<void>(SceneAssetInstantiator::SetRenderablesEnabled(
            m_scene,
            entry.instance,
            false));
        entry.status = entry.instance.status;
    }

    if (!entry.cpuReadyMilestone)
    {
        Diagnostics::RecordTraceInstant(entry.traceContext,
                                        "SceneInstantiate");
        Diagnostics::RecordTraceInstant(entry.traceContext,
                                        "CPUReady");
        entry.cpuReadyMilestone = true;
    }

    entry.status = SceneAssetInstantiator::UpdateResidency(
        m_scene,
        *entry.model,
        m_resources,
        entry.instance);
    const bool minimumResident =
        entry.status.residency >= SceneAssetResidency::MinimumResident;
    if (minimumResident)
    {
        if (entry.activateWhenResident && !entry.renderablesActivated)
        {
            static_cast<void>(SceneAssetInstantiator::SetRenderablesEnabled(
                m_scene,
                entry.instance,
                true));
            entry.renderablesActivated = true;
        }
        if (!entry.minimumResidentMilestone)
        {
            Diagnostics::RecordTraceInstant(entry.traceContext,
                                            "MinimumResident");
            entry.minimumResidentMilestone = true;
        }
    }
    if (entry.status.IsFailed())
        return false;
    if (entry.status.IsFullyResident())
    {
        if (!entry.fullyResidentMilestone)
        {
            Diagnostics::RecordTraceInstant(entry.traceContext,
                                            "FullyResident");
            entry.fullyResidentMilestone = true;
        }
    }
    return true;
}

bool SceneAssetLoadCoordinator::UpdateEnvironment(Entry& entry)
{
    if (!entry.environment.IsValid())
    {
        const Resource::ResourceLoadSnapshot snapshot =
            entry.environmentRequest.GetSnapshot();
        entry.status.progress = snapshot.progress.fraction;
        if (snapshot.state == Resource::ResourceLoadState::Failed)
        {
            Fail(entry, snapshot.error, snapshot.error.message);
            return false;
        }
        if (snapshot.state == Resource::ResourceLoadState::Cancelled)
        {
            entry.status.lifecycle = SceneAssetLifecycle::Cancelled;
            entry.status.residency = SceneAssetResidency::None;
            entry.status.error = snapshot.error;
            entry.status.diagnostic = snapshot.error.message;
            ++entry.status.revision;
            return false;
        }
        if (snapshot.state != Resource::ResourceLoadState::Ready)
            return true;

        entry.environment = entry.environmentRequest.TryGet();
        if (!entry.environment.IsLoaded() ||
            !entry.environment->GetData().IsValid())
        {
            Fail(entry,
                 {Resource::ResourceLoadErrorCode::TypeMismatch,
                  "Published environment is incomplete"},
                 "Published environment is incomplete");
            return false;
        }
        SetStatus(entry.status,
                  SceneAssetLifecycle::Active,
                  SceneAssetResidency::CPUReady,
                  0.5f);
        if (!entry.cpuReadyMilestone)
        {
            Diagnostics::RecordTraceInstant(entry.traceContext, "CPUReady");
            entry.cpuReadyMilestone = true;
        }
    }

    auto* skybox = dynamic_cast<SkyboxComponent*>(
        m_scene.ResolveComponent(entry.targetSkybox));
    if (!skybox)
    {
        Fail(entry,
             {Resource::ResourceLoadErrorCode::PublishFailure,
              "Environment target SkyboxComponent no longer exists"},
             "Environment target SkyboxComponent no longer exists");
        return false;
    }

    const Resource::EnvironmentResourceData& data =
        entry.environment->GetData();
    const Resource::TextureHandle textures[] = {
        data.environment,
        data.irradiance,
        data.prefiltered,
        data.brdfLUT};
    bool pending = false;
    for (const Resource::TextureHandle& texture : textures)
    {
        std::string diagnostic;
        const DependencyState dependency =
            InspectTexture(m_resources, texture, diagnostic);
        if (dependency == DependencyState::Failed)
        {
            Fail(entry,
                 {Resource::ResourceLoadErrorCode::PublishFailure,
                  diagnostic},
                 std::move(diagnostic));
            return false;
        }
        pending |= dependency == DependencyState::Pending;
    }
    if (pending)
    {
        SetStatus(entry.status,
                  SceneAssetLifecycle::Active,
                  SceneAssetResidency::CPUReady,
                  0.75f);
        return true;
    }

    if (!entry.environmentBound)
    {
        skybox->SetSkyboxType(SkyboxType::Cubemap);
        skybox->SetCubemap(data.environment);
        skybox->SetIrradianceMap(data.irradiance);
        skybox->SetPrefilteredMap(data.prefiltered);
        skybox->SetBRDFLUT(data.brdfLUT);
        skybox->SetExposure(data.intensity);
        skybox->SetContributesToLighting(true);
        entry.environmentBound = true;
    }
    SetStatus(entry.status,
              SceneAssetLifecycle::Active,
              SceneAssetResidency::FullyResident,
              1.0f);
    if (!entry.minimumResidentMilestone)
    {
        Diagnostics::RecordTraceInstant(entry.traceContext,
                                        "MinimumResident");
        entry.minimumResidentMilestone = true;
    }
    if (!entry.fullyResidentMilestone)
    {
        Diagnostics::RecordTraceInstant(entry.traceContext,
                                        "FullyResident");
        entry.fullyResidentMilestone = true;
    }
    return true;
}

bool SceneAssetLoadCoordinator::Update()
{
    if (!m_scene.IsInitialized() || !m_scene.IsUpdateThread() ||
        m_scene.IsUpdating())
    {
        RVX_CORE_ERROR(
            "SceneAssetLoadCoordinator::Update requires an idle Scene update thread");
        return false;
    }

    bool succeeded = true;
    for (auto& entry : m_entries)
    {
        if (!entry || entry->status.lifecycle == SceneAssetLifecycle::Failed ||
            entry->status.lifecycle == SceneAssetLifecycle::Cancelled)
        {
            continue;
        }
        succeeded &= entry->kind == SceneAssetKind::Model
                         ? UpdateModel(*entry)
                         : UpdateEnvironment(*entry);
    }
    return succeeded;
}

void SceneAssetLoadCoordinator::RestoreEnvironment(Entry& entry)
{
    auto* skybox = dynamic_cast<SkyboxComponent*>(
        m_scene.ResolveComponent(entry.targetSkybox));
    if (!skybox || !entry.skyboxSnapshot.captured)
    {
        entry.environmentBound = false;
        return;
    }
    skybox->SetSkyboxType(entry.skyboxSnapshot.type);
    skybox->SetCubemap(entry.skyboxSnapshot.cubemap);
    skybox->SetEquirectangular(entry.skyboxSnapshot.equirectangular);
    skybox->SetIrradianceMap(entry.skyboxSnapshot.irradiance);
    skybox->SetPrefilteredMap(entry.skyboxSnapshot.prefiltered);
    skybox->SetBRDFLUT(entry.skyboxSnapshot.brdfLUT);
    skybox->SetExposure(entry.skyboxSnapshot.exposure);
    skybox->SetContributesToLighting(
        entry.skyboxSnapshot.contributesToLighting);
    entry.environmentBound = false;
}

bool SceneAssetLoadCoordinator::Cancel(SceneAssetLoadHandle handle)
{
    Entry* entry = Resolve(handle);
    if (!entry || !m_scene.IsInitialized() || !m_scene.IsUpdateThread() ||
        m_scene.IsUpdating())
    {
        return false;
    }
    if (entry->status.lifecycle == SceneAssetLifecycle::Cancelled)
        return false;

    if (entry->kind == SceneAssetKind::Model)
    {
        static_cast<void>(entry->modelRequest.Cancel());
        if (entry->instance.rootActor.IsValid())
        {
            static_cast<void>(SceneAssetInstantiator::Cancel(
                m_scene,
                entry->instance));
        }
    }
    else
    {
        static_cast<void>(entry->environmentRequest.Cancel());
        if (entry->environmentBound)
            RestoreEnvironment(*entry);
    }
    entry->status.lifecycle = SceneAssetLifecycle::Cancelled;
    entry->status.residency = SceneAssetResidency::None;
    entry->status.progress = 0.0f;
    entry->status.error = {
        Resource::ResourceLoadErrorCode::Cancelled,
        "Scene asset request was cancelled"};
    entry->status.diagnostic = entry->status.error.message;
    ++entry->status.revision;
    return true;
}

void SceneAssetLoadCoordinator::CancelAll()
{
    if (m_scene.IsInitialized() && m_scene.IsUpdateThread() &&
        !m_scene.IsUpdating())
    {
        for (uint32 index = 0; index < m_entries.size(); ++index)
        {
            if (!m_entries[index])
                continue;
            Entry& entry = *m_entries[index];
            if (entry.status.lifecycle != SceneAssetLifecycle::Cancelled)
            {
                if (entry.kind == SceneAssetKind::Model)
                {
                    static_cast<void>(entry.modelRequest.Cancel());
                    if (entry.instance.rootActor.IsValid())
                    {
                        static_cast<void>(SceneAssetInstantiator::Cancel(
                            m_scene,
                            entry.instance));
                    }
                }
                else
                {
                    static_cast<void>(entry.environmentRequest.Cancel());
                    if (entry.environmentBound)
                        RestoreEnvironment(entry);
                }
            }
            m_handles.Free(entry.handle);
            m_entries[index].reset();
        }
    }
}

bool SceneAssetLoadCoordinator::IsValid(SceneAssetLoadHandle handle) const
{
    return Resolve(handle) != nullptr;
}

SceneAssetKind SceneAssetLoadCoordinator::GetKind(
    SceneAssetLoadHandle handle) const
{
    const Entry* entry = Resolve(handle);
    return entry ? entry->kind : SceneAssetKind::Model;
}

const SceneAssetStatus* SceneAssetLoadCoordinator::GetStatus(
    SceneAssetLoadHandle handle) const
{
    const Entry* entry = Resolve(handle);
    return entry ? &entry->status : nullptr;
}

Resource::ResourceLoadRequestId
SceneAssetLoadCoordinator::GetResourceRequestId(
    SceneAssetLoadHandle handle) const
{
    const Entry* entry = Resolve(handle);
    if (!entry)
        return {};
    return entry->kind == SceneAssetKind::Model
               ? entry->modelRequest.GetRequestId()
               : entry->environmentRequest.GetRequestId();
}

Resource::ResourceHandle<Resource::ModelResource>
SceneAssetLoadCoordinator::GetModel(SceneAssetLoadHandle handle) const
{
    const Entry* entry = Resolve(handle);
    return entry && entry->kind == SceneAssetKind::Model
               ? entry->model
               : Resource::ResourceHandle<Resource::ModelResource>{};
}

const SceneAssetInstance* SceneAssetLoadCoordinator::GetModelInstance(
    SceneAssetLoadHandle handle) const
{
    const Entry* entry = Resolve(handle);
    return entry && entry->kind == SceneAssetKind::Model &&
                   entry->instance.rootActor.IsValid()
               ? &entry->instance
               : nullptr;
}

Resource::EnvironmentHandle SceneAssetLoadCoordinator::GetEnvironment(
    SceneAssetLoadHandle handle) const
{
    const Entry* entry = Resolve(handle);
    return entry && entry->kind == SceneAssetKind::Environment
               ? entry->environment
               : Resource::EnvironmentHandle{};
}
} // namespace RVX
