/** @file EcsSceneAssetLoadCoordinator.cpp @brief Pure ECS Model asset lifecycle. */

#include "ResourceSceneAdapters/ECS/EcsSceneAssetLoadCoordinator.h"

#include "Core/Assert.h"
#include "Resource/Types/MaterialResource.h"
#include "Resource/Types/TextureResource.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace RVX::ResourceSceneAdapters
{
    struct EcsSceneAssetLoadCoordinator::Entry
    {
        EcsSceneAssetLoadHandle handle = InvalidEcsSceneAssetLoadHandle;
        EcsSceneAssetLoadStatus status;
        Resource::ResourceLoadHandle<Resource::ModelResource> request;
        Resource::ResourceHandle<Resource::ModelResource> model;
        Resource::AssetResidencyLease residencyLease;
        PreparedModelBatch batch;
        std::vector<ECS::EntityHandle> meshMembers;
        std::vector<PreparedModelNodeId> preparedMeshNodeIds;
        std::vector<SceneECS::Visibility> preparedMeshVisibility;
        std::vector<SceneECS::Visibility> visibleMeshValues;
        EcsSceneAssetRetirementRequest retirementRequest;
        EcsSceneAssetRetirementBeginReceipt retirementAdmission;
        bool adopted = false;
        bool renderAcknowledged = false;
        bool resourcesAcknowledged = false;
    };

    namespace
    {
        bool MatchesExactly(const EcsSceneAssetRetirementRequest& expected,
                            const EcsSceneAssetRetirementRequest& actual)
        {
            return expected.sceneRuntimeId == actual.sceneRuntimeId &&
                   expected.rootEntity == actual.rootEntity &&
                   expected.members == actual.members;
        }

        bool IsReleasableHandleState(EcsSceneAssetLoadState state)
        {
            return state == EcsSceneAssetLoadState::Cancelled ||
                   state == EcsSceneAssetLoadState::Recycled;
        }

        bool IsStrictlySortedRenderableVisibilityVersions(
            const std::vector<EcsSceneAssetRenderableVisibilityVersion>& versions)
        {
            for (size_t index = 0; index < versions.size(); ++index)
            {
                if (!versions[index].entity.IsValid() ||
                    versions[index].entityVisibilityWriteVersion == 0 ||
                    (index != 0 && !(versions[index - 1].entity < versions[index].entity)))
                {
                    return false;
                }
            }
            return true;
        }

        bool IsFinite(const Vec4& value)
        {
            return std::isfinite(value.x) && std::isfinite(value.y) &&
                   std::isfinite(value.z) && std::isfinite(value.w);
        }
    } // namespace

    EcsSceneAssetLoadCoordinator::EcsSceneAssetLoadCoordinator(
        SceneECS::SceneEcsRuntime& runtime,
        Resource::ResourceSubsystem& resources,
        IEcsSceneAssetRetirementProofGateway* retirementProofGateway) noexcept
        : m_runtime(runtime)
        , m_resources(resources)
        , m_retirementProofGateway(retirementProofGateway)
        , m_ownerThread(std::this_thread::get_id())
    {
    }

    EcsSceneAssetLoadCoordinator::~EcsSceneAssetLoadCoordinator() noexcept
    {
        const bool hasLiveEntries = std::any_of(
            m_entries.begin(), m_entries.end(),
            [](const std::unique_ptr<Entry>& entry) { return entry != nullptr; });
        const bool clean = !hasLiveEntries || PrepareForHostShutdown();
        const bool drained = std::none_of(
            m_entries.begin(), m_entries.end(),
            [](const std::unique_ptr<Entry>& entry) { return entry != nullptr; });
        RVX_ASSERT_MSG(clean && drained,
                       "ECS asset coordinator destruction requires all exact leases and ECS "
                       "retirements to be drained first.");
    }

    bool EcsSceneAssetLoadCoordinator::IsOwnerThread() const noexcept
    {
        return m_ownerThread == std::this_thread::get_id();
    }

    EcsSceneAssetLoadHandle EcsSceneAssetLoadCoordinator::Allocate(std::unique_ptr<Entry> entry)
    {
        if (entry == nullptr)
        {
            return InvalidEcsSceneAssetLoadHandle;
        }

        const EcsSceneAssetLoadHandle handle = m_handles.Allocate();
        try
        {
            if (handle.GetIndex() >= m_entries.size())
            {
                m_entries.resize(static_cast<size_t>(handle.GetIndex()) + 1u);
            }
            entry->handle = handle;
            m_entries[handle.GetIndex()] = std::move(entry);
            return handle;
        }
        catch (...)
        {
            static_cast<void>(m_handles.TryFree(handle));
            return InvalidEcsSceneAssetLoadHandle;
        }
    }

    EcsSceneAssetLoadCoordinator::Entry* EcsSceneAssetLoadCoordinator::Resolve(
        EcsSceneAssetLoadHandle handle)
    {
        return m_handles.IsValid(handle) && handle.GetIndex() < m_entries.size() ?
                   m_entries[handle.GetIndex()].get() :
                   nullptr;
    }

    const EcsSceneAssetLoadCoordinator::Entry* EcsSceneAssetLoadCoordinator::Resolve(
        EcsSceneAssetLoadHandle handle) const
    {
        return m_handles.IsValid(handle) && handle.GetIndex() < m_entries.size() ?
                   m_entries[handle.GetIndex()].get() :
                   nullptr;
    }

    EcsSceneAssetLoadHandle EcsSceneAssetLoadCoordinator::RequestModel(
        EcsModelAssetLoadDesc desc, std::string& outError)
    {
        outError.clear();
        if (!IsOwnerThread() || m_hostShutdown || desc.path.empty())
        {
            outError = !IsOwnerThread() ? "ECS asset requests require the coordinator owner thread." :
                       "ECS asset request was rejected during host shutdown or for an empty path.";
            return InvalidEcsSceneAssetLoadHandle;
        }

        Resource::ResourceLoadHandle<Resource::ModelResource> request =
            m_resources.RequestAsync<Resource::ModelResource>(
                desc.path, std::move(desc.resourceOptions));
        if (!request.IsValid())
        {
            outError = "ResourceSubsystem rejected the prepared Model request.";
            return InvalidEcsSceneAssetLoadHandle;
        }

        auto entry = std::make_unique<Entry>();
        entry->request = std::move(request);
        entry->status.request = entry->request.GetSnapshot();
        entry->status.assetKey = entry->status.request.assetKey;
        entry->status.sceneRuntimeId = desc.expectedSceneRuntimeId.IsValid() ?
                                           desc.expectedSceneRuntimeId :
                                           m_runtime.GetSceneRuntimeId();
        entry->status.state = EcsSceneAssetLoadState::Requested;
        const EcsSceneAssetLoadHandle handle = Allocate(std::move(entry));
        if (!handle.IsValid())
        {
            outError = "ECS asset coordinator could not allocate a generation-safe handle.";
        }
        return handle;
    }

    void EcsSceneAssetLoadCoordinator::Fail(Entry& entry,
                                             EcsSceneAssetLoadState state,
                                             std::string diagnostic)
    {
        entry.status.textureStreamingStartReceipt.reset();
        entry.status.fullyResidentTextureReceipt.reset();
        entry.status.state = state;
        entry.status.diagnostic = std::move(diagnostic);
    }

    bool EcsSceneAssetLoadCoordinator::PopulateModelMetadata(Entry& entry)
    {
        if (!entry.model.IsValid() || !entry.model.IsLoaded())
        {
            return false;
        }

        try
        {
            EcsModelAssetMetadata metadata;
            metadata.sourceModelAssetId = {.value = entry.model.GetId()};
            if (!metadata.sourceModelAssetId.IsValid())
            {
                return false;
            }
            metadata.sourceNodeCount = static_cast<uint64>(entry.model->GetNodeCount());
            metadata.contentVerificationReceipt =
                entry.model->GetContentVerificationReceipt();

            const auto& meshes = entry.model->GetMeshes();
            metadata.meshAssetIds.reserve(meshes.size());
            for (const Resource::ResourceHandle<Resource::MeshResource>& mesh : meshes)
            {
                if (!mesh.IsValid() || !mesh.IsLoaded() || mesh.GetId() == Resource::InvalidResourceId)
                {
                    return false;
                }
                metadata.meshAssetIds.push_back({.value = mesh.GetId()});
            }

            const auto& materials = entry.model->GetMaterials();
            metadata.materialAssetIds.reserve(materials.size());
            metadata.materials.reserve(materials.size());
            const auto appendTexture = [&metadata](
                                           const Resource::ResourceHandle<Resource::TextureResource>& texture)
            {
                if (!texture.IsValid() || !texture.IsLoaded() ||
                    texture.GetId() == Resource::InvalidResourceId ||
                    texture->GetWidth() == 0 || texture->GetHeight() == 0 ||
                    texture->GetMipLevels() == 0 ||
                    texture->GetFormat() == Resource::TextureFormat::Unknown)
                {
                    return false;
                }

                const AssetId textureAssetId{.value = texture.GetId()};
                const auto existing = std::find_if(
                    metadata.textures.begin(), metadata.textures.end(),
                    [textureAssetId](const EcsModelAssetMetadata::Texture& value)
                    {
                        return value.textureAssetId == textureAssetId;
                    });
                if (existing != metadata.textures.end())
                {
                    return true;
                }

                metadata.textures.push_back({
                    .textureAssetId = textureAssetId,
                    .width = texture->GetWidth(),
                    .height = texture->GetHeight(),
                    .mipLevels = texture->GetMipLevels(),
                    .format = texture->GetFormat(),
                    .isDefaultFallback = texture->IsDefaultFallback(),
                    .isStreamingPlaceholder = texture->IsStreamingPlaceholder(),
                    .fallbackReason = texture->GetFallbackReason(),
                });
                return true;
            };
            for (const Resource::ResourceHandle<Resource::MaterialResource>& material : materials)
            {
                if (!material.IsValid() || !material.IsLoaded() ||
                    material.GetId() == Resource::InvalidResourceId ||
                    material->GetMaterial() == nullptr)
                {
                    return false;
                }

                EcsModelAssetMetadata::Material materialMetadata;
                materialMetadata.materialAssetId = {.value = material.GetId()};
                materialMetadata.sourceName = material->GetMaterialName();
                materialMetadata.workflow = material->GetWorkflowMode();
                materialMetadata.alphaMode = material->GetAlphaMode();
                materialMetadata.baseColor = material->GetBaseColor();
                materialMetadata.metallicFactor = material->GetMetallicFactor();
                materialMetadata.roughnessFactor = material->GetRoughnessFactor();
                if (!IsFinite(materialMetadata.baseColor) ||
                    !std::isfinite(materialMetadata.metallicFactor) ||
                    !std::isfinite(materialMetadata.roughnessFactor))
                {
                    return false;
                }

                const auto& materialTextures = material->GetTextures();
                std::vector<std::pair<std::string,
                                      Resource::ResourceHandle<Resource::TextureResource>>>
                    orderedTextures;
                orderedTextures.reserve(materialTextures.size());
                for (const auto& [slot, texture] : materialTextures)
                {
                    if (slot.empty())
                    {
                        return false;
                    }
                    orderedTextures.emplace_back(slot, texture);
                }
                std::sort(orderedTextures.begin(), orderedTextures.end(),
                          [](const auto& lhs, const auto& rhs) { return lhs.first < rhs.first; });
                materialMetadata.textureSlots.reserve(orderedTextures.size());
                for (const auto& [slot, texture] : orderedTextures)
                {
                    if (!appendTexture(texture))
                    {
                        return false;
                    }
                    materialMetadata.textureSlots.push_back(
                        {.slot = slot, .textureAssetId = {.value = texture.GetId()}});
                }

                metadata.materialAssetIds.push_back({.value = material.GetId()});
                metadata.materials.push_back(std::move(materialMetadata));
            }

            const std::vector<Resource::ResourceHandle<Resource::TextureResource>> textures =
                entry.model->GetStreamingTextures();
            metadata.streamingTextureAssetIds.reserve(textures.size());
            for (const Resource::ResourceHandle<Resource::TextureResource>& texture : textures)
            {
                if (!appendTexture(texture))
                {
                    return false;
                }
                metadata.streamingTextureAssetIds.push_back({.value = texture.GetId()});
            }

            const Resource::AnimationHandle animation = entry.model->GetAnimationResource();
            if (animation.IsValid())
            {
                if (!animation.IsLoaded() || animation.GetId() == Resource::InvalidResourceId)
                {
                    return false;
                }
                metadata.animationAssetId = {.value = animation.GetId()};
            }
            entry.status.modelMetadata = std::move(metadata);
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    bool EcsSceneAssetLoadCoordinator::BuildFullyResidentTextureReceipt(Entry& entry)
    {
        entry.status.fullyResidentTextureReceipt.reset();
        if (!entry.model.IsValid() || !entry.model.IsLoaded())
        {
            return false;
        }

        const EcsModelAssetMetadata& source = entry.status.modelMetadata;
        if (!source.HasPublishedSource() ||
            source.sourceModelAssetId.value != entry.model.GetId() ||
            source.materialAssetIds.size() != source.materials.size())
        {
            return false;
        }

        try
        {
            EcsModelFullyResidentTextureReceipt receipt;
            receipt.sourceModelAssetId = source.sourceModelAssetId;

            const auto equalTextureValue = [](
                                               const EcsModelAssetMetadata::Texture& lhs,
                                               const EcsModelAssetMetadata::Texture& rhs)
            {
                return lhs.textureAssetId == rhs.textureAssetId &&
                       lhs.width == rhs.width &&
                       lhs.height == rhs.height &&
                       lhs.mipLevels == rhs.mipLevels &&
                       lhs.format == rhs.format &&
                       lhs.isDefaultFallback == rhs.isDefaultFallback &&
                       lhs.isStreamingPlaceholder == rhs.isStreamingPlaceholder &&
                       lhs.fallbackReason == rhs.fallbackReason;
            };
            const auto appendTexture = [&receipt, &equalTextureValue](
                                           const Resource::ResourceHandle<Resource::TextureResource>& texture)
            {
                if (!texture.IsValid() || !texture.IsLoaded() ||
                    texture.GetId() == Resource::InvalidResourceId ||
                    texture->GetWidth() == 0 || texture->GetHeight() == 0 ||
                    texture->GetMipLevels() == 0 ||
                    texture->GetFormat() == Resource::TextureFormat::Unknown ||
                    texture->IsDefaultFallback() || texture->IsStreamingPlaceholder())
                {
                    return false;
                }

                const EcsModelAssetMetadata::Texture value{
                    .textureAssetId = {.value = texture.GetId()},
                    .width = texture->GetWidth(),
                    .height = texture->GetHeight(),
                    .mipLevels = texture->GetMipLevels(),
                    .format = texture->GetFormat(),
                    .isDefaultFallback = texture->IsDefaultFallback(),
                    .isStreamingPlaceholder = texture->IsStreamingPlaceholder(),
                    .fallbackReason = texture->GetFallbackReason(),
                };
                const auto existing = std::find_if(
                    receipt.textures.begin(), receipt.textures.end(),
                    [&value](const EcsModelAssetMetadata::Texture& candidate)
                    {
                        return candidate.textureAssetId == value.textureAssetId;
                    });
                if (existing != receipt.textures.end())
                {
                    return equalTextureValue(*existing, value);
                }

                receipt.textures.push_back(value);
                return true;
            };

            const auto& materials = entry.model->GetMaterials();
            if (materials.size() != source.materials.size())
            {
                return false;
            }
            for (size_t materialIndex = 0; materialIndex < materials.size(); ++materialIndex)
            {
                const Resource::ResourceHandle<Resource::MaterialResource>& material =
                    materials[materialIndex];
                const EcsModelAssetMetadata::Material& expected =
                    source.materials[materialIndex];
                if (!material.IsValid() || !material.IsLoaded() ||
                    material.GetId() == Resource::InvalidResourceId ||
                    material->GetMaterial() == nullptr ||
                    !expected.materialAssetId.IsValid() ||
                    source.materialAssetIds[materialIndex] != expected.materialAssetId ||
                    material.GetId() != expected.materialAssetId.value)
                {
                    return false;
                }

                const auto& currentTextures = material->GetTextures();
                std::vector<std::pair<std::string,
                                      Resource::ResourceHandle<Resource::TextureResource>>>
                    orderedTextures;
                orderedTextures.reserve(currentTextures.size());
                for (const auto& [slot, texture] : currentTextures)
                {
                    if (slot.empty())
                    {
                        return false;
                    }
                    orderedTextures.emplace_back(slot, texture);
                }
                std::sort(orderedTextures.begin(), orderedTextures.end(),
                          [](const auto& lhs, const auto& rhs)
                          {
                              return lhs.first < rhs.first;
                          });
                if (orderedTextures.size() != expected.textureSlots.size())
                {
                    return false;
                }
                for (size_t textureIndex = 0; textureIndex < orderedTextures.size(); ++textureIndex)
                {
                    const EcsModelAssetMetadata::TextureSlot& expectedSlot =
                        expected.textureSlots[textureIndex];
                    const auto& [slot, texture] = orderedTextures[textureIndex];
                    if (expectedSlot.slot.empty() || !expectedSlot.textureAssetId.IsValid() ||
                        slot != expectedSlot.slot || !texture.IsValid() ||
                        texture.GetId() != expectedSlot.textureAssetId.value ||
                        !appendTexture(texture))
                    {
                        return false;
                    }
                }
            }

            const std::vector<Resource::ResourceHandle<Resource::TextureResource>>
                streamingTextures = entry.model->GetStreamingTextures();
            if (streamingTextures.size() != source.streamingTextureAssetIds.size())
            {
                return false;
            }
            for (size_t textureIndex = 0; textureIndex < streamingTextures.size(); ++textureIndex)
            {
                const Resource::ResourceHandle<Resource::TextureResource>& texture =
                    streamingTextures[textureIndex];
                const AssetId expectedAssetId = source.streamingTextureAssetIds[textureIndex];
                if (!expectedAssetId.IsValid() || !texture.IsValid() ||
                    texture.GetId() != expectedAssetId.value || !appendTexture(texture))
                {
                    return false;
                }
            }

            if (receipt.textures.size() != source.textures.size())
            {
                return false;
            }
            for (size_t textureIndex = 0; textureIndex < receipt.textures.size(); ++textureIndex)
            {
                const EcsModelAssetMetadata::Texture& expected = source.textures[textureIndex];
                const EcsModelAssetMetadata::Texture& actual = receipt.textures[textureIndex];
                if (!expected.textureAssetId.IsValid() ||
                    actual.textureAssetId != expected.textureAssetId ||
                    std::any_of(source.textures.begin(),
                                source.textures.begin() + static_cast<std::ptrdiff_t>(textureIndex),
                                [&expected](const EcsModelAssetMetadata::Texture& prior)
                                {
                                    return prior.textureAssetId == expected.textureAssetId;
                                }))
                {
                    return false;
                }
            }

            entry.status.fullyResidentTextureReceipt.emplace(std::move(receipt));
            return true;
        }
        catch (...)
        {
            entry.status.fullyResidentTextureReceipt.reset();
            return false;
        }
    }

    bool EcsSceneAssetLoadCoordinator::AdoptHidden(Entry& entry)
    {
        entry.residencyLease = m_resources.GetManager().AcquireAssetResidencyLease(
            entry.status.assetKey);
        if (!entry.residencyLease.IsValid())
        {
            Fail(entry, EcsSceneAssetLoadState::Failed,
                 "The exact published AssetKey could not acquire a residency lease.");
            return false;
        }

        PreparedModelAdoptionReceipt adopted = AdoptPreparedModelBatch(
            m_runtime, entry.batch, entry.status.sceneRuntimeId);
        if (!adopted.IsApplied())
        {
            entry.residencyLease.Reset();
            Fail(entry, EcsSceneAssetLoadState::Failed,
                 "Atomic prepared-model adoption rejected the expected ECS runtime.");
            return false;
        }

        entry.adopted = true;
        entry.status.sceneRuntimeId = adopted.sceneRuntimeId;
        entry.status.rootEntity = adopted.rootEntity;
        entry.status.adoptedStructuralRevision = adopted.sceneStructuralRevisionAfter;
        // Retain the complete value-only identity mapping before the prepared
        // Geometry batch is later discarded. The receipt is already sorted by
        // temporary node ID; keep that order as the public status contract.
        entry.status.entityMappings = std::move(adopted.members);
        entry.status.members.clear();
        entry.status.members.reserve(entry.status.entityMappings.size());
        for (const PreparedModelEntityMapping& mapping : entry.status.entityMappings)
        {
            entry.status.members.push_back(mapping.entity);
            const auto source = std::find_if(
                entry.batch.nodes.begin(), entry.batch.nodes.end(),
                [&mapping](const PreparedModelNode& node)
                {
                    return node.temporaryNodeId == mapping.temporaryNodeId;
                });
            if (source != entry.batch.nodes.end() && source->hasMesh)
            {
                const auto sourceIndex = std::find(
                    entry.preparedMeshNodeIds.begin(), entry.preparedMeshNodeIds.end(),
                    mapping.temporaryNodeId);
                if (sourceIndex == entry.preparedMeshNodeIds.end())
                {
                    Fail(entry, EcsSceneAssetLoadState::FailedRetained,
                         "Prepared mesh visibility was absent after atomic ECS adoption.");
                    static_cast<void>(BeginRetirement(entry));
                    return false;
                }
                entry.meshMembers.push_back(mapping.entity);
                entry.visibleMeshValues.push_back(
                    entry.preparedMeshVisibility[static_cast<size_t>(
                        sourceIndex - entry.preparedMeshNodeIds.begin())]);
            }
        }
        entry.status.state = EcsSceneAssetLoadState::CPUReadyHidden;
        return true;
    }

    bool EcsSceneAssetLoadCoordinator::EnableMinimumResident(Entry& entry)
    {
        if (!CheckMinimumDependencies(entry))
        {
            return entry.status.state != EcsSceneAssetLoadState::FailedRetained;
        }

        if (entry.meshMembers.size() != entry.visibleMeshValues.size())
        {
            Fail(entry, EcsSceneAssetLoadState::FailedRetained,
                 "Prepared mesh visibility does not match the adopted renderable member set.");
            static_cast<void>(BeginRetirement(entry));
            return false;
        }
        const ECS::Registry& registry = m_runtime.GetRegistry();
        for (const ECS::EntityHandle member : entry.meshMembers)
        {
            if (registry.TryGet<SceneECS::Visibility>(member) == nullptr)
            {
                Fail(entry, EcsSceneAssetLoadState::FailedRetained,
                     "A renderable ECS member lost Visibility before minimum-resident activation.");
                static_cast<void>(BeginRetirement(entry));
                return false;
            }
        }
        try
        {
            entry.status.renderableVisibilityVersions.clear();
            entry.status.renderableVisibilityVersions.reserve(entry.meshMembers.size());
        }
        catch (...)
        {
            Fail(entry, EcsSceneAssetLoadState::FailedRetained,
                 "Minimum-resident activation could not reserve exact Visibility evidence.");
            static_cast<void>(BeginRetirement(entry));
            return false;
        }
        for (size_t index = 0; index < entry.meshMembers.size(); ++index)
        {
            if (!m_runtime.SetFragment<SceneECS::Visibility>(
                    entry.meshMembers[index], entry.visibleMeshValues[index]))
            {
                Fail(entry, EcsSceneAssetLoadState::FailedRetained,
                     "The safe ECS visibility write for minimum residency failed.");
                static_cast<void>(BeginRetirement(entry));
                return false;
            }
            const uint64 writeVersion =
                registry.GetFragmentWriteVersion<SceneECS::Visibility>(entry.meshMembers[index]);
            if (writeVersion == 0)
            {
                Fail(entry, EcsSceneAssetLoadState::FailedRetained,
                     "Minimum-resident activation did not produce an entity Visibility write version.");
                static_cast<void>(BeginRetirement(entry));
                return false;
            }
            entry.status.renderableVisibilityVersions.push_back(
                {.entity = entry.meshMembers[index], .entityVisibilityWriteVersion = writeVersion});
        }
        std::sort(entry.status.renderableVisibilityVersions.begin(),
                  entry.status.renderableVisibilityVersions.end(),
                  [](const EcsSceneAssetRenderableVisibilityVersion& lhs,
                     const EcsSceneAssetRenderableVisibilityVersion& rhs)
                  {
                      return lhs.entity < rhs.entity;
                  });
        entry.status.state = EcsSceneAssetLoadState::MinimumResidentPendingPresentation;
        return true;
    }

    bool EcsSceneAssetLoadCoordinator::CheckMinimumDependencies(Entry& entry)
    {
        enum class Readiness : uint8
        {
            Pending = 0,
            Ready,
            Failed,
        };

        bool pending = false;
        const auto inspect = [this, &pending](AssetId assetId, RenderResourceKind kind)
        {
            if (!assetId.IsValid())
            {
                return Readiness::Failed;
            }
            const Resource::RenderResourceResolveResult resolved =
                m_resources.ResolveRenderResource(assetId, kind);
            if (resolved.code == Resource::RenderResourceResolveCode::NotFound)
            {
                pending = true;
                return Readiness::Pending;
            }
            if (resolved.code != Resource::RenderResourceResolveCode::Resolved ||
                resolved.status.code == RenderResourceStatusCode::StaleGeneration ||
                resolved.status.code == RenderResourceStatusCode::InvalidHandle ||
                resolved.status.state == RenderResourcePublicState::Failed ||
                resolved.status.state == RenderResourcePublicState::Released ||
                resolved.status.state == RenderResourcePublicState::Evicting)
            {
                return Readiness::Failed;
            }
            if (resolved.status.state != RenderResourcePublicState::GPUReady)
            {
                pending = true;
                return Readiness::Pending;
            }
            return Readiness::Ready;
        };

        for (const Resource::ResourceHandle<Resource::MeshResource>& mesh : entry.model->GetMeshes())
        {
            if (!mesh.IsLoaded())
            {
                if (mesh.IsLoading())
                {
                    pending = true;
                    continue;
                }
                Fail(entry, EcsSceneAssetLoadState::FailedRetained,
                     "A required Model mesh was no longer CPU-resident.");
                static_cast<void>(BeginRetirement(entry));
                return false;
            }
            if (inspect(AssetId{mesh.GetId()}, RenderResourceKind::Mesh) == Readiness::Failed)
            {
                Fail(entry, EcsSceneAssetLoadState::FailedRetained,
                     "A required Model mesh failed before minimum GPU residency.");
                static_cast<void>(BeginRetirement(entry));
                return false;
            }
        }
        for (const Resource::ResourceHandle<Resource::MaterialResource>& material :
             entry.model->GetMaterials())
        {
            if (!material.IsLoaded())
            {
                if (material.IsLoading())
                {
                    pending = true;
                    continue;
                }
                Fail(entry, EcsSceneAssetLoadState::FailedRetained,
                     "A required Model material was no longer CPU-resident.");
                static_cast<void>(BeginRetirement(entry));
                return false;
            }
            if (inspect(AssetId{material.GetId()}, RenderResourceKind::Material) == Readiness::Failed)
            {
                Fail(entry, EcsSceneAssetLoadState::FailedRetained,
                     "A required Model material failed before minimum GPU residency.");
                static_cast<void>(BeginRetirement(entry));
                return false;
            }
        }
        return !pending;
    }

    bool EcsSceneAssetLoadCoordinator::ConfirmMinimumResidentPresentation(
        EcsSceneAssetLoadHandle handle,
        const EcsSceneAssetMinimumResidentPresentationReceipt& receipt)
    {
        if (!IsOwnerThread())
        {
            return false;
        }
        Entry* entry = Resolve(handle);
        if (entry == nullptr ||
            entry->status.state != EcsSceneAssetLoadState::MinimumResidentPendingPresentation ||
            receipt.sceneRuntimeId != entry->status.sceneRuntimeId ||
            receipt.rootEntity != entry->status.rootEntity ||
            receipt.frozenSourceSnapshotRevision == 0 ||
            receipt.renderSceneRevision == 0 ||
            receipt.carryingPresentedFrameSequence == 0 ||
            !IsStrictlySortedRenderableVisibilityVersions(receipt.renderableVisibilityVersions) ||
            receipt.renderableVisibilityVersions != entry->status.renderableVisibilityVersions)
        {
            return false;
        }
        entry->status.minimumResidentFrozenSourceSnapshotRevision =
            receipt.frozenSourceSnapshotRevision;
        entry->status.minimumResidentRenderSceneRevision = receipt.renderSceneRevision;
        entry->status.minimumResidentPresentedFrameSequence =
            receipt.carryingPresentedFrameSequence;
        entry->status.state = EcsSceneAssetLoadState::MinimumResident;
        return true;
    }

    bool EcsSceneAssetLoadCoordinator::BeginRetirement(Entry& entry)
    {
        if (!entry.adopted)
        {
            return false;
        }
        if (entry.status.state == EcsSceneAssetLoadState::AwaitingRenderProof ||
            entry.status.state == EcsSceneAssetLoadState::AwaitingResourceClosure ||
            entry.status.state == EcsSceneAssetLoadState::AwaitingEcsRecycle ||
            entry.status.state == EcsSceneAssetLoadState::Retiring)
        {
            return true;
        }

        // A cancellation/retirement must not advertise a live streaming authorization.
        entry.status.textureStreamingStartReceipt.reset();
        entry.status.fullyResidentTextureReceipt.reset();
        entry.retirementRequest = {
            .sceneRuntimeId = entry.status.sceneRuntimeId,
            .rootEntity = entry.status.rootEntity,
            .members = entry.status.members,
        };
        if (m_retirementProofGateway == nullptr)
        {
            Fail(entry, EcsSceneAssetLoadState::FailedRetained,
                 "No value-only Render retirement proof gateway is installed.");
            return false;
        }
        const bool shouldCancelStreaming =
            entry.model &&
            m_resources.GetManager().IsSoleAssetResidencyConsumer(entry.residencyLease);
        Resource::AssetResidencyLease streamingRetirementPin;
        if (shouldCancelStreaming)
        {
            streamingRetirementPin =
                m_resources.GetManager().AcquireAssetResidencyLease(entry.status.assetKey);
            if (!streamingRetirementPin.IsValid())
            {
                Fail(entry, EcsSceneAssetLoadState::FailedRetained,
                     "Could not acquire an exact streaming-retirement residency pin before Render admission.");
                return false;
            }
        }
        entry.retirementAdmission =
            m_retirementProofGateway->BeginRetirement(entry.retirementRequest);
        if (!entry.retirementAdmission.IsAccepted() ||
            !MatchesExactly(entry.retirementRequest, entry.retirementAdmission.request))
        {
            Fail(entry,
                 entry.retirementAdmission.code == EcsSceneAssetRetirementBeginCode::DeviceLost ?
                     EcsSceneAssetLoadState::DeviceLost :
                     EcsSceneAssetLoadState::FailedRetained,
                 entry.retirementAdmission.diagnostic.empty() ?
                     "Render retirement admission did not capture the exact ECS member set." :
                     entry.retirementAdmission.diagnostic);
            return false;
        }
        if (shouldCancelStreaming)
        {
            const Resource::ModelTextureStreamingCancellationReport cancellation =
                m_resources.CancelModelTextureStreaming(entry.model);
            if (cancellation.renderState ==
                Resource::ModelTextureStreamingRenderCancellationState::AwaitingRenderRetirement)
            {
                m_resources.RetainAssetResidencyUntilModelTextureRetirement(
                    entry.model.GetId(), std::move(streamingRetirementPin));
            }
        }
        entry.status.state = EcsSceneAssetLoadState::Retiring;
        const SceneECS::CleanupDomainMask domains =
            SceneECS::ToCleanupDomainMask(SceneECS::CleanupDomain::Render) |
            SceneECS::ToCleanupDomainMask(SceneECS::CleanupDomain::Resources);
        if (m_runtime.RequestDestroyBatch(entry.status.members, domains) !=
            SceneECS::DestroyRequestResult::Accepted)
        {
            Fail(entry, EcsSceneAssetLoadState::FailedRetained,
                 "ECS instance retirement could not atomically request every member.");
            return false;
        }
        static_cast<void>(m_runtime.PublishPendingDestroyCleanup());
        entry.status.state = EcsSceneAssetLoadState::AwaitingRenderProof;
        return true;
    }

    bool EcsSceneAssetLoadCoordinator::AdvanceRetirement(Entry& entry)
    {
        if (entry.status.state == EcsSceneAssetLoadState::AwaitingRenderProof)
        {
            const EcsSceneAssetRetirementProof proof =
                m_retirementProofGateway->QueryRetirementProof(
                    entry.retirementAdmission.token);
            if (!MatchesExactly(entry.retirementRequest, proof.request))
            {
                Fail(entry, EcsSceneAssetLoadState::FailedRetained,
                     "Render retirement evidence did not match the exact ECS member set.");
                return false;
            }
            if (proof.state == EcsSceneAssetRetirementProofState::DeviceLost)
            {
                Fail(entry, EcsSceneAssetLoadState::DeviceLost,
                     proof.diagnostic.empty() ? "Render device lost before ECS retirement proof." :
                                                proof.diagnostic);
                return false;
            }
            if (proof.state == EcsSceneAssetRetirementProofState::Failed)
            {
                Fail(entry, EcsSceneAssetLoadState::FailedRetained,
                     proof.diagnostic.empty() ? "Render retirement proof failed." : proof.diagnostic);
                return false;
            }
            const bool neverPublished =
                proof.state == EcsSceneAssetRetirementProofState::NeverPublished;
            if (proof.state != EcsSceneAssetRetirementProofState::Presented &&
                !neverPublished)
            {
                return true;
            }
            if ((!neverPublished &&
                 (proof.appliedRenderSceneRevision == 0 ||
                  proof.presentedFrameSequence == 0)) ||
                (neverPublished &&
                 (proof.appliedRenderSceneRevision != 0 ||
                  proof.presentedFrameSequence != 0)))
            {
                Fail(entry, EcsSceneAssetLoadState::FailedRetained,
                     "Render retirement evidence carried an invalid presentation revision/frame pair.");
                return false;
            }

            for (const ECS::EntityHandle member : entry.status.members)
            {
                if (!m_runtime.AcknowledgeCleanup(
                        member, SceneECS::ToCleanupDomainMask(SceneECS::CleanupDomain::Render)))
                {
                    Fail(entry, EcsSceneAssetLoadState::FailedRetained,
                         "Render proof arrived but exact ECS Render cleanup acknowledgement failed.");
                    return false;
                }
            }
            if (!m_retirementProofGateway->AcknowledgeRetirementProof(
                    entry.retirementAdmission.token))
            {
                Fail(entry, EcsSceneAssetLoadState::FailedRetained,
                     "Terminal Render retirement evidence could not be acknowledged.");
                return false;
            }
            entry.renderAcknowledged = true;
            const Resource::ResourceSceneClosureReleaseBeginResult release =
                m_resources.BeginSceneAssetClosureRelease(std::move(entry.residencyLease));
            entry.status.closureRelease = release.receipt;
            if (!release.IsAccepted())
            {
                Fail(entry, EcsSceneAssetLoadState::FailedRetained,
                     "Resource closure release retained the exact residency lease.");
                return false;
            }
            entry.status.state = EcsSceneAssetLoadState::AwaitingResourceClosure;
        }

        if (entry.status.state == EcsSceneAssetLoadState::AwaitingEcsRecycle)
        {
            return AdvanceEcsRecycle(entry);
        }

        if (entry.status.state != EcsSceneAssetLoadState::AwaitingResourceClosure)
        {
            return true;
        }
        const std::optional<Resource::ResourceSceneClosureReleaseReceipt> receipt =
            m_resources.QuerySceneAssetClosureRelease(entry.status.closureRelease.token);
        if (!receipt.has_value())
        {
            Fail(entry, EcsSceneAssetLoadState::FailedRetained,
                 "The exact Resource closure receipt became unavailable.");
            return false;
        }
        entry.status.closureRelease = *receipt;
        if (!receipt->IsTerminal())
        {
            return true;
        }
        if (receipt->state == Resource::ResourceSceneClosureReleaseState::FailedRetained)
        {
            Fail(entry, EcsSceneAssetLoadState::FailedRetained,
                 "Resource closure reported retained failure.");
            return false;
        }
        if (receipt->state == Resource::ResourceSceneClosureReleaseState::DeviceLost)
        {
            Fail(entry, EcsSceneAssetLoadState::DeviceLost,
                 "Resource closure reported Render device loss.");
            return false;
        }
        for (const ECS::EntityHandle member : entry.status.members)
        {
            if (!m_runtime.AcknowledgeCleanup(
                    member, SceneECS::ToCleanupDomainMask(SceneECS::CleanupDomain::Resources)))
            {
                Fail(entry, EcsSceneAssetLoadState::FailedRetained,
                     "Resource closure completed but ECS Resources cleanup acknowledgement failed.");
                return false;
            }
        }
        const Resource::ResourceSceneClosureReleaseAcknowledgeResult acknowledged =
            m_resources.AcknowledgeSceneAssetClosureRelease(receipt->token);
        if (acknowledged.code !=
            Resource::ResourceSceneClosureReleaseAcknowledgeCode::Acknowledged)
        {
            Fail(entry, EcsSceneAssetLoadState::FailedRetained,
                 "Terminal Resource closure could not be acknowledged after ECS cleanup acceptance.");
            return false;
        }
        entry.resourcesAcknowledged = true;
        entry.status.state = EcsSceneAssetLoadState::AwaitingEcsRecycle;
        return AdvanceEcsRecycle(entry);
    }

    bool EcsSceneAssetLoadCoordinator::AdvanceEcsRecycle(Entry& entry)
    {
        if (entry.status.state != EcsSceneAssetLoadState::AwaitingEcsRecycle)
        {
            return false;
        }

        static_cast<void>(m_runtime.AdvanceRetirements());
        static_cast<void>(m_runtime.RecycleRecyclableEntities());
        const bool allMembersRecycled =
            std::all_of(entry.status.members.begin(),
                        entry.status.members.end(),
                        [this](ECS::EntityHandle member)
                        {
                            return !m_runtime.GetRegistry().IsAlive(member);
                        });
        if (!allMembersRecycled)
        {
            return true;
        }
        entry.status.state = EcsSceneAssetLoadState::Recycled;
        return true;
    }

    bool EcsSceneAssetLoadCoordinator::UpdateEntry(Entry& entry)
    {
        entry.status.request = entry.request.GetSnapshot();
        if (entry.status.state == EcsSceneAssetLoadState::Requested ||
            entry.status.state == EcsSceneAssetLoadState::Preparing ||
            entry.status.state == EcsSceneAssetLoadState::PreparedCPU)
        {
            switch (entry.status.request.state)
            {
                case Resource::ResourceLoadState::Queued:
                    entry.status.state = EcsSceneAssetLoadState::Requested;
                    return true;
                case Resource::ResourceLoadState::Loading:
                    entry.status.state = EcsSceneAssetLoadState::Preparing;
                    return true;
                case Resource::ResourceLoadState::AwaitingPublish:
                    entry.status.state = EcsSceneAssetLoadState::PreparedCPU;
                    return true;
                case Resource::ResourceLoadState::Ready:
                {
                    entry.model = entry.request.TryGet();
                    if (!entry.model)
                    {
                        Fail(entry, EcsSceneAssetLoadState::Failed,
                             "The Ready prepared request did not expose a ModelResource.");
                        return false;
                    }
                    const std::optional<Resource::AssetKey> publishedKey =
                        m_resources.GetManager().FindPublishedAssetKey(entry.model.GetId());
                    if (!publishedKey.has_value())
                    {
                        Fail(entry, EcsSceneAssetLoadState::Failed,
                             "The Ready ModelResource has no exact published AssetKey identity.");
                        return false;
                    }
                    if (*publishedKey != entry.status.request.assetKey)
                    {
                        Fail(entry, EcsSceneAssetLoadState::Failed,
                             "The published Model AssetKey differs from the exact request identity.");
                        return false;
                    }
                    entry.status.assetKey = *publishedKey;
                    if (!PopulateModelMetadata(entry))
                    {
                        Fail(entry, EcsSceneAssetLoadState::Failed,
                             "The published ModelResource could not produce a value-only metadata receipt.");
                        return false;
                    }
                    {
                        PreparedModelBatchBuildReceipt built =
                            PreparedModelBatchBuilder::Build(*entry.model);
                        if (!built.IsPrepared())
                        {
                            Fail(entry, EcsSceneAssetLoadState::Failed,
                                 "Published ModelResource did not satisfy ECS batch prerequisites.");
                            return false;
                        }
                        entry.batch = std::move(built.batch);
                        for (PreparedModelNode& node : entry.batch.nodes)
                        {
                            if (node.hasMesh)
                            {
                                entry.preparedMeshNodeIds.push_back(node.temporaryNodeId);
                                entry.preparedMeshVisibility.push_back(node.visibility);
                                node.visibility.visible = false;
                            }
                        }
                    }
                    entry.status.state = EcsSceneAssetLoadState::PendingAdopt;
                    return true;
                }
                case Resource::ResourceLoadState::Failed:
                    Fail(entry, EcsSceneAssetLoadState::Failed, entry.status.request.error.message);
                    return false;
                case Resource::ResourceLoadState::Cancelled:
                    Fail(entry, EcsSceneAssetLoadState::Cancelled,
                         "Prepared Model request was cancelled before ECS adoption.");
                    return true;
            }
        }

        if (entry.status.state == EcsSceneAssetLoadState::PendingAdopt)
        {
            return AdoptHidden(entry);
        }
        if (entry.status.state == EcsSceneAssetLoadState::CPUReadyHidden)
        {
            return EnableMinimumResident(entry);
        }
        if (entry.status.state == EcsSceneAssetLoadState::MinimumResident)
        {
            std::vector<AssetId> streamingTextureAssetIds;
            try
            {
                streamingTextureAssetIds = entry.status.modelMetadata.streamingTextureAssetIds;
            }
            catch (...)
            {
                Fail(entry, EcsSceneAssetLoadState::FailedRetained,
                     "Deferred Model texture streaming could not copy its exact texture authorization set.");
                static_cast<void>(BeginRetirement(entry));
                return false;
            }
            const uint64 startSceneFrameSequence =
                m_runtime.GetDiagnosticsSnapshot().frameSequence;
            if (!m_resources.EnsureModelTextureStreaming(entry.model) ||
                !m_resources.BeginModelTextureStreaming(entry.model))
            {
                Fail(entry, EcsSceneAssetLoadState::FailedRetained,
                     "Deferred Model texture streaming could not start after presentation proof.");
                static_cast<void>(BeginRetirement(entry));
                return false;
            }
            entry.status.textureStreamingStartReceipt = {
                .authorizedFrozenSourceSnapshotRevision =
                    entry.status.minimumResidentFrozenSourceSnapshotRevision,
                .authorizedRenderSceneRevision = entry.status.minimumResidentRenderSceneRevision,
                .authorizedPresentedFrameSequence =
                    entry.status.minimumResidentPresentedFrameSequence,
                .startSceneFrameSequence = startSceneFrameSequence,
                .textureAssetIds = std::move(streamingTextureAssetIds),
            };
            entry.status.state = EcsSceneAssetLoadState::Streaming;
            return true;
        }
        if (entry.status.state == EcsSceneAssetLoadState::Streaming)
        {
            const Resource::ModelTextureStreamingSnapshot stream =
                entry.model->GetTextureStreamingSnapshot();
            if (stream.stage == Resource::ModelTextureStreamingStage::Failed ||
                stream.stage == Resource::ModelTextureStreamingStage::Cancelled)
            {
                Fail(entry, EcsSceneAssetLoadState::FailedRetained,
                     stream.error.empty() ? "Model texture streaming failed or was cancelled." : stream.error);
                static_cast<void>(BeginRetirement(entry));
                return false;
            }
            const bool fullyResident =
                stream.stage == Resource::ModelTextureStreamingStage::FullyResident ||
                stream.stage == Resource::ModelTextureStreamingStage::None;
            if (fullyResident)
            {
                if (!BuildFullyResidentTextureReceipt(entry))
                {
                    Fail(entry, EcsSceneAssetLoadState::FailedRetained,
                         "Fully-resident Model textures could not produce an exact value-only receipt.");
                    static_cast<void>(BeginRetirement(entry));
                    return false;
                }
                entry.status.state = EcsSceneAssetLoadState::FullyResident;
            }
        }
        if (entry.status.state == EcsSceneAssetLoadState::AwaitingRenderProof ||
            entry.status.state == EcsSceneAssetLoadState::AwaitingResourceClosure ||
            entry.status.state == EcsSceneAssetLoadState::AwaitingEcsRecycle)
        {
            return AdvanceRetirement(entry);
        }
        return true;
    }

    void EcsSceneAssetLoadCoordinator::ReleaseEntry(EcsSceneAssetLoadHandle handle) noexcept
    {
        if (!m_handles.IsValid(handle) || handle.GetIndex() >= m_entries.size())
        {
            return;
        }
        m_entries[handle.GetIndex()].reset();
        static_cast<void>(m_handles.TryFree(handle));
    }

    bool EcsSceneAssetLoadCoordinator::Update()
    {
        if (!IsOwnerThread())
        {
            return false;
        }
        bool succeeded = true;
        for (size_t index = 0; index < m_entries.size(); ++index)
        {
            Entry* entry = m_entries[index].get();
            if (entry == nullptr)
            {
                continue;
            }
            const EcsSceneAssetLoadHandle handle = entry->handle;
            succeeded = UpdateEntry(*entry) && succeeded;
            entry = Resolve(handle);
            if (entry != nullptr && IsReleasableHandleState(entry->status.state))
            {
                ReleaseEntry(handle);
            }
        }
        return succeeded;
    }

    bool EcsSceneAssetLoadCoordinator::Cancel(EcsSceneAssetLoadHandle handle)
    {
        if (!IsOwnerThread())
        {
            return false;
        }
        Entry* entry = Resolve(handle);
        if (entry == nullptr)
        {
            return false;
        }
        if (entry->status.state == EcsSceneAssetLoadState::Cancelled ||
            entry->status.state == EcsSceneAssetLoadState::Retiring ||
            entry->status.state == EcsSceneAssetLoadState::AwaitingRenderProof ||
            entry->status.state == EcsSceneAssetLoadState::AwaitingResourceClosure ||
            entry->status.state == EcsSceneAssetLoadState::AwaitingEcsRecycle ||
            entry->status.state == EcsSceneAssetLoadState::Recycled)
        {
            return true;
        }
        if (!entry->adopted)
        {
            static_cast<void>(entry->request.Cancel());
            entry->model = {};
            entry->batch = {};
            entry->status.textureStreamingStartReceipt.reset();
            entry->status.state = EcsSceneAssetLoadState::Cancelled;
            entry->status.diagnostic = "Cancelled before ECS adoption; no ECS mutation occurred.";
            return true;
        }
        return BeginRetirement(*entry);
    }

    bool EcsSceneAssetLoadCoordinator::PrepareForHostShutdown() noexcept
    {
        if (!IsOwnerThread())
        {
            return false;
        }
        m_hostShutdown = true;
        bool clean = true;
        for (const std::unique_ptr<Entry>& owned : m_entries)
        {
            if (owned != nullptr && !owned->adopted &&
                owned->status.state != EcsSceneAssetLoadState::Cancelled)
            {
                clean = Cancel(owned->handle) && clean;
            }
            else if (owned != nullptr && !owned->status.IsTerminal())
            {
                clean = Cancel(owned->handle) && clean;
            }
        }
        for (const std::unique_ptr<Entry>& owned : m_entries)
        {
            if (owned != nullptr &&
                (owned->status.state == EcsSceneAssetLoadState::AwaitingRenderProof ||
                 owned->status.state == EcsSceneAssetLoadState::AwaitingResourceClosure ||
                 owned->status.state == EcsSceneAssetLoadState::AwaitingEcsRecycle))
            {
                clean = AdvanceRetirement(*owned) && clean;
            }
            if (owned != nullptr && !owned->status.IsTerminal())
            {
                clean = false;
            }
        }
        for (size_t index = 0; index < m_entries.size(); ++index)
        {
            if (m_entries[index] != nullptr &&
                IsReleasableHandleState(m_entries[index]->status.state))
            {
                ReleaseEntry(m_entries[index]->handle);
            }
        }
        for (const std::unique_ptr<Entry>& owned : m_entries)
        {
            clean = (owned == nullptr) && clean;
        }
        return clean;
    }

    bool EcsSceneAssetLoadCoordinator::IsValid(EcsSceneAssetLoadHandle handle) const
    {
        return IsOwnerThread() && Resolve(handle) != nullptr;
    }

    std::optional<EcsSceneAssetLoadStatus> EcsSceneAssetLoadCoordinator::GetStatus(
        EcsSceneAssetLoadHandle handle) const
    {
        if (!IsOwnerThread())
        {
            return std::nullopt;
        }
        const Entry* entry = Resolve(handle);
        return entry != nullptr ? std::optional<EcsSceneAssetLoadStatus>(entry->status) :
                                  std::nullopt;
    }

    ECS::EntityHandle EcsSceneAssetLoadStatus::FindEntityByTemporaryNodeId(
        PreparedModelNodeId id) const
    {
        const auto found = std::lower_bound(
            entityMappings.begin(), entityMappings.end(), id,
            [](const PreparedModelEntityMapping& mapping, PreparedModelNodeId targetId)
            {
                return mapping.temporaryNodeId < targetId;
            });
        return found != entityMappings.end() && found->temporaryNodeId == id ?
                   found->entity :
                   ECS::EntityHandle::Invalid();
    }

    std::vector<ECS::EntityHandle> EcsSceneAssetLoadStatus::FindEntitiesByExactSourceName(
        std::string_view sourceName) const
    {
        std::vector<ECS::EntityHandle> result;
        for (const PreparedModelEntityMapping& mapping : entityMappings)
        {
            if (mapping.sourceName == sourceName)
            {
                result.push_back(mapping.entity);
            }
        }
        return result;
    }

    std::optional<ECS::EntityHandle>
    EcsSceneAssetLoadStatus::FindUniqueEntityByExactSourceName(std::string_view sourceName) const
    {
        const std::vector<ECS::EntityHandle> matches =
            FindEntitiesByExactSourceName(sourceName);
        return matches.size() == 1 ? std::optional<ECS::EntityHandle>(matches.front()) :
                                     std::nullopt;
    }

    Resource::ResourceLoadRequestId EcsSceneAssetLoadCoordinator::GetResourceRequestId(
        EcsSceneAssetLoadHandle handle) const
    {
        if (!IsOwnerThread())
        {
            return {};
        }
        const Entry* entry = Resolve(handle);
        return entry != nullptr ? entry->status.request.requestId :
                                  Resource::ResourceLoadRequestId{};
    }
} // namespace RVX::ResourceSceneAdapters
