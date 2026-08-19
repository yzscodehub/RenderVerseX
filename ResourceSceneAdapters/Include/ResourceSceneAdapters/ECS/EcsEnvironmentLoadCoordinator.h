#pragma once

/**
 * @file EcsEnvironmentLoadCoordinator.h
 * @brief Owner-thread Environment-resource lifecycle for one dedicated ECS Skybox.
 */

#include "Core/Handle.h"
#include "Resource/Loader/EnvironmentLoader.h"
#include "Resource/ResourceSubsystem.h"
#include "Resource/Types/EnvironmentResource.h"
#include "ResourceSceneAdapters/ECS/EcsEnvironmentPresentationReceipt.h"
#include "ResourceSceneAdapters/ECS/EcsSceneAssetLoadCoordinator.h"
#include "Scene/ECS/RenderFragments.h"
#include "Scene/ECS/SceneEcsRuntime.h"

#include <compare>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace RVX::ResourceSceneAdapters
{
    struct EcsEnvironmentLoadHandleTag final
    {
    };
    using EcsEnvironmentLoadHandle = Handle<EcsEnvironmentLoadHandleTag, uint32>;
    inline constexpr EcsEnvironmentLoadHandle InvalidEcsEnvironmentLoadHandle =
        EcsEnvironmentLoadHandle::Invalid();

    /** @brief Generation-safe Environment request identity qualified by its owning Scene. */
    struct EcsEnvironmentLoadRef
    {
        ECS::SceneRuntimeId sceneRuntimeId;
        EcsEnvironmentLoadHandle handle = InvalidEcsEnvironmentLoadHandle;

        [[nodiscard]] bool IsValid() const noexcept
        {
            return sceneRuntimeId.IsValid() && handle.IsValid();
        }

        auto operator<=>(const EcsEnvironmentLoadRef&) const = default;
    };

    enum class EcsEnvironmentLoadState : uint8
    {
        Requested = 0,
        Preparing,
        PreparedCPU,
        PendingGpuReadiness,
        PendingAdopt,
        PendingPresentation,
        FullyResident,
        Cancelled,
        Retiring,
        AwaitingRenderProof,
        AwaitingResourceClosure,
        AwaitingEcsRecycle,
        Failed,
        FailedRetained,
        DeviceLost,
        Recycled,
    };

    /** @brief Value request for one exact EnvironmentResource variant in one ECS runtime. */
    struct EcsEnvironmentLoadDesc
    {
        std::string path;
        /** Immutable bake profile captured before asynchronous preparation. */
        Resource::EnvironmentLoadOptions environmentOptions;
        Resource::ResourceLoadOptions resourceOptions;
        /** Must name this coordinator's one exact Scene runtime. */
        ECS::SceneRuntimeId expectedSceneRuntimeId;
    };

    /** @brief Root-owned value identity of one ECS-owned complete environment IBL set. */
    struct EnvironmentInstanceFragment
    {
        AssetId sourceEnvironmentAssetId{};
        AssetId environmentAssetId{};
        AssetId irradianceAssetId{};
        AssetId prefilteredEnvironmentAssetId{};
        AssetId brdfLutAssetId{};
    };

    static_assert(ECS::Fragment<EnvironmentInstanceFragment>);

    /** @brief Value-only input to the friend-only atomic ECS environment adoption. */
    struct EcsEnvironmentAdoptionBatch
    {
        AssetId sourceEnvironmentAssetId{};
        SceneECS::Skybox skybox;
    };

    enum class EcsEnvironmentAdoptionError : uint8
    {
        None = 0,
        InvalidExpectedSceneRuntime,
        SceneRuntimeMismatch,
        InvalidEnvironmentAssetPrerequisite,
        InvalidSkyboxAssetPrerequisite,
        ExistingLiveSkybox,
        RecordingRejected,
        TransactionRejected,
    };

    /** @brief Explicit test-only all-or-nothing adoption fault injection. */
    enum class EcsEnvironmentAdoptionFault : uint8
    {
        None = 0,
        RejectAfterRecording,
    };

    struct EcsEnvironmentAdoptionOptions
    {
        EcsEnvironmentAdoptionFault fault = EcsEnvironmentAdoptionFault::None;
    };

    /** @brief Exact value receipt from one dedicated Skybox adoption transaction. */
    struct EcsEnvironmentAdoptionReceipt
    {
        EcsEnvironmentAdoptionError error = EcsEnvironmentAdoptionError::None;
        ECS::CommandStatus transactionStatus = ECS::CommandStatus::Rejected;
        ECS::CommandError transactionError = ECS::CommandError::None;
        ECS::SceneRuntimeId sceneRuntimeId;
        uint64 sceneStructuralRevisionBefore = 0;
        uint64 sceneStructuralRevisionAfter = 0;
        ECS::EntityHandle skyboxEntity = ECS::EntityHandle::Invalid();

        [[nodiscard]] bool IsApplied() const noexcept
        {
            return error == EcsEnvironmentAdoptionError::None &&
                   transactionStatus == ECS::CommandStatus::Applied &&
                   skyboxEntity.IsValid();
        }
    };

    /** @brief Atomically create one dedicated, specialized-retirement ECS Skybox entity. */
    [[nodiscard]] EcsEnvironmentAdoptionReceipt AdoptEcsEnvironmentBatch(
        SceneECS::SceneEcsRuntime& runtime,
        const EcsEnvironmentAdoptionBatch& batch,
        ECS::SceneRuntimeId expectedSceneRuntimeId,
        const EcsEnvironmentAdoptionOptions& options = {});

    /** @brief Value-only lifecycle state for one generation-safe environment request. */
    struct EcsEnvironmentLoadStatus
    {
        EcsEnvironmentLoadState state = EcsEnvironmentLoadState::Requested;
        Resource::ResourceLoadSnapshot request;
        Resource::AssetKey assetKey;
        /** Immutable profile and canonical identity admitted for this request. */
        Resource::EnvironmentLoadOptions environmentOptions;
        uint64 canonicalImportOptionsHash = 0;
        /** Published root proof copied while the strong resource lease is held. */
        std::optional<Resource::ResourceContentVerificationReceipt>
            contentVerificationReceipt;
        /** Value-only complete IBL metadata. */
        uint32 environmentResolution = 0;
        uint32 irradianceResolution = 0;
        uint32 prefilteredResolution = 0;
        uint32 prefilteredMipLevels = 0;
        uint32 brdfLUTResolution = 0;
        float32 exposure = 1.0f;
        ECS::SceneRuntimeId sceneRuntimeId;
        ECS::EntityHandle skyboxEntity = ECS::EntityHandle::Invalid();
        uint64 adoptedStructuralRevision = 0;
        uint64 skyboxWriteVersion = 0;
        uint64 presentedFrozenSourceSnapshotRevision = 0;
        uint64 presentedRenderSceneRevision = 0;
        uint64 presentedFrameSequence = 0;
        Resource::ResourceSceneClosureReleaseReceipt closureRelease;
        std::string diagnostic;

        [[nodiscard]] bool IsTerminal() const noexcept
        {
            return state == EcsEnvironmentLoadState::Cancelled ||
                   state == EcsEnvironmentLoadState::Failed ||
                   state == EcsEnvironmentLoadState::FailedRetained ||
                   state == EcsEnvironmentLoadState::DeviceLost ||
                   state == EcsEnvironmentLoadState::Recycled;
        }
    };

    /**
     * @brief Coordinates exact Environment residency and ECS Skybox ownership on one owner thread.
     *
     * Resource references enter ECS only as AssetId values. The exact residency
     * lease remains held until Render retirement, Resource closure, ECS Resources
     * cleanup acknowledgement, and entity recycle have all completed.
     */
    class EcsEnvironmentLoadCoordinator final
    {
    public:
        EcsEnvironmentLoadCoordinator(
            SceneECS::SceneEcsRuntime& runtime,
            Resource::ResourceSubsystem& resources,
            IEcsSceneAssetRetirementProofGateway* retirementProofGateway = nullptr) noexcept;
        ~EcsEnvironmentLoadCoordinator() noexcept;

        EcsEnvironmentLoadCoordinator(const EcsEnvironmentLoadCoordinator&) = delete;
        EcsEnvironmentLoadCoordinator& operator=(const EcsEnvironmentLoadCoordinator&) = delete;

        [[nodiscard]] EcsEnvironmentLoadHandle RequestEnvironment(
            EcsEnvironmentLoadDesc desc, std::string& outError);
        [[nodiscard]] bool Update();
        /** @brief Accept only exact accepted-and-presented evidence for this Skybox write. */
        [[nodiscard]] bool ConfirmPresentation(
            EcsEnvironmentLoadHandle handle,
            const EcsEnvironmentPresentationReceipt& receipt);
        [[nodiscard]] bool Cancel(EcsEnvironmentLoadHandle handle);
        [[nodiscard]] bool PrepareForHostShutdown() noexcept;

        [[nodiscard]] bool IsValid(EcsEnvironmentLoadHandle handle) const;
        [[nodiscard]] std::optional<EcsEnvironmentLoadStatus> GetStatus(
            EcsEnvironmentLoadHandle handle) const;
        [[nodiscard]] Resource::ResourceLoadRequestId GetResourceRequestId(
            EcsEnvironmentLoadHandle handle) const;

    private:
        struct Entry;

        [[nodiscard]] bool IsOwnerThread() const noexcept;
        [[nodiscard]] Entry* Resolve(EcsEnvironmentLoadHandle handle);
        [[nodiscard]] const Entry* Resolve(EcsEnvironmentLoadHandle handle) const;
        [[nodiscard]] EcsEnvironmentLoadHandle Allocate(std::unique_ptr<Entry> entry);
        [[nodiscard]] bool UpdateEntry(Entry& entry);
        [[nodiscard]] bool CheckGpuReadiness(Entry& entry);
        [[nodiscard]] bool Adopt(Entry& entry);
        [[nodiscard]] bool BeginRetirement(Entry& entry);
        [[nodiscard]] bool AdvanceRetirement(Entry& entry);
        [[nodiscard]] bool AdvanceEcsRecycle(Entry& entry);
        void Fail(Entry& entry, EcsEnvironmentLoadState state, std::string diagnostic);
        void ReleaseEntry(EcsEnvironmentLoadHandle handle) noexcept;

        SceneECS::SceneEcsRuntime& m_runtime;
        Resource::ResourceSubsystem& m_resources;
        IEcsSceneAssetRetirementProofGateway* m_retirementProofGateway = nullptr;
        std::thread::id m_ownerThread;
        HandlePool<EcsEnvironmentLoadHandle> m_handles;
        std::vector<std::unique_ptr<Entry>> m_entries;
        bool m_hostShutdown = false;
    };
} // namespace RVX::ResourceSceneAdapters
