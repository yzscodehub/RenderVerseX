#pragma once

/**
 * @file EcsSceneAssetLoadCoordinator.h
 * @brief Owner-thread asynchronous Model-resource lifecycle for Scene ECS.
 */

#include "Core/Handle.h"
#include "Resource/ResourceSubsystem.h"
#include "Resource/Types/ModelResource.h"
#include "ResourceSceneAdapters/ECS/PreparedModelBatch.h"
#include "Scene/ECS/SceneEcsRuntime.h"

#include <compare>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace RVX::ResourceSceneAdapters
{
    struct EcsSceneAssetLoadHandleTag final
    {
    };
    using EcsSceneAssetLoadHandle = Handle<EcsSceneAssetLoadHandleTag, uint32>;
    inline constexpr EcsSceneAssetLoadHandle InvalidEcsSceneAssetLoadHandle =
        EcsSceneAssetLoadHandle::Invalid();

    /**
     * @brief Generation-safe Model request identity qualified by its owning Scene runtime.
     *
     * The local coordinator handle is only meaningful inside the Scene that created it.
     * Keeping the runtime id in the public value prevents a coincident local handle in
     * another World from being queried or cancelled accidentally.
     */
    struct EcsModelAssetLoadRef
    {
        ECS::SceneRuntimeId sceneRuntimeId;
        EcsSceneAssetLoadHandle handle = InvalidEcsSceneAssetLoadHandle;

        [[nodiscard]] bool IsValid() const noexcept
        {
            return sceneRuntimeId.IsValid() && handle.IsValid();
        }

        auto operator<=>(const EcsModelAssetLoadRef&) const = default;
    };

    enum class EcsSceneAssetLoadState : uint8
    {
        Requested = 0,
        Preparing,
        PreparedCPU,
        PendingAdopt,
        CPUReadyHidden,
        MinimumResidentPendingPresentation,
        MinimumResident,
        Streaming,
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

    /** @brief Value request for one exact ModelResource variant in one ECS runtime. */
    struct EcsModelAssetLoadDesc
    {
        std::string path;
        Resource::ResourceLoadOptions resourceOptions;
        /**
         * @brief Explicit identity expected by adoption; invalid means the target
         * runtime's identity at request time.
         */
        ECS::SceneRuntimeId expectedSceneRuntimeId;
    };

    /** @brief Exact per-renderable Visibility write captured by render extraction. */
    struct EcsSceneAssetRenderableVisibilityVersion
    {
        ECS::EntityHandle entity = ECS::EntityHandle::Invalid();
        uint64 entityVisibilityWriteVersion = 0;

        bool operator==(const EcsSceneAssetRenderableVisibilityVersion&) const = default;
    };

    /** @brief Exact applied-and-presented Render receipt for one activation write set. */
    struct EcsSceneAssetMinimumResidentPresentationReceipt
    {
        ECS::SceneRuntimeId sceneRuntimeId;
        ECS::EntityHandle rootEntity = ECS::EntityHandle::Invalid();
        /** Sorted complete set; each value is the entity-local Visibility version. */
        std::vector<EcsSceneAssetRenderableVisibilityVersion> renderableVisibilityVersions;
        uint64 frozenSourceSnapshotRevision = 0;
        uint64 renderSceneRevision = 0;
        uint64 carryingPresentedFrameSequence = 0;
    };

    /**
     * @brief Immutable value inventory captured from the exact published ModelResource.
     *
     * The coordinator creates this only while it owns the strong ModelResource
     * handle.  Consumers retain a copy in @ref EcsSceneAssetLoadStatus and
     * therefore never need a ResourceHandle or object pointer to inspect a
     * model's source identity, dependencies, or content proof.
     */
    struct EcsModelAssetMetadata
    {
        /** @brief One named texture binding in deterministic lexical slot order. */
        struct TextureSlot
        {
            std::string slot;
            AssetId textureAssetId{};
        };

        /** @brief Immutable value metadata for one loaded texture dependency. */
        struct Texture
        {
            AssetId textureAssetId{};
            uint32 width = 0;
            uint32 height = 0;
            uint32 mipLevels = 0;
            Resource::TextureFormat format = Resource::TextureFormat::Unknown;
            bool isDefaultFallback = false;
            bool isStreamingPlaceholder = false;
            std::string fallbackReason;
        };

        /**
         * @brief Immutable value snapshot for one material entry in source order.
         *
         * Source names deliberately are not keys: duplicate names remain distinct
         * entries, exactly as they appeared in the ModelResource material list.
         */
        struct Material
        {
            AssetId materialAssetId{};
            std::string sourceName;
            Resource::MaterialWorkflowMode workflow =
                Resource::MaterialWorkflowMode::MetallicRoughness;
            Resource::MaterialAlphaMode alphaMode = Resource::MaterialAlphaMode::Opaque;
            Vec4 baseColor{1.0f, 1.0f, 1.0f, 1.0f};
            float metallicFactor = 1.0f;
            float roughnessFactor = 1.0f;
            /** Lexically sorted by @ref TextureSlot::slot. */
            std::vector<TextureSlot> textureSlots;
        };

        AssetId sourceModelAssetId{};
        uint64 sourceNodeCount = 0;
        /** Exact source-model mesh order. Malformed entries fail publication. */
        std::vector<AssetId> meshAssetIds;
        /** Exact source-model material order. Malformed entries fail publication. */
        std::vector<AssetId> materialAssetIds;
        /** Material values in the same source order as @ref materialAssetIds. */
        std::vector<Material> materials;
        /**
         * Stable first-use texture inventory: material order and lexical slot
         * order first, followed by streaming-source order. Every AssetId appears
         * once here; binding and streaming vectors preserve duplicate uses.
         */
        std::vector<Texture> textures;
        /** Exact streaming-source order captured before deferred streaming begins. */
        std::vector<AssetId> streamingTextureAssetIds;
        /** Optional immutable skeletal-animation dependency. */
        AssetId animationAssetId{};
        Resource::ResourceContentVerificationReceipt contentVerificationReceipt;

        [[nodiscard]] bool HasPublishedSource() const noexcept
        {
            return sourceModelAssetId.IsValid();
        }
    };

    /**
     * @brief Value proof that deferred texture streaming started after the exact
     * minimum-resident Scene snapshot was applied and presented.
     *
     * @ref startSceneFrameSequence may be zero when the owner starts streaming
     * before its first SceneEcsRuntime::Tick; zero is still the exact sequence.
     */
    struct EcsModelTextureStreamingStartReceipt
    {
        uint64 authorizedFrozenSourceSnapshotRevision = 0;
        uint64 authorizedRenderSceneRevision = 0;
        uint64 authorizedPresentedFrameSequence = 0;
        uint64 startSceneFrameSequence = 0;
        /** Exact streaming-source order, including duplicate source uses. */
        std::vector<AssetId> textureAssetIds;

        [[nodiscard]] bool IsAuthorized() const noexcept
        {
            return authorizedFrozenSourceSnapshotRevision != 0 &&
                   authorizedRenderSceneRevision != 0 &&
                   authorizedPresentedFrameSequence != 0;
        }
    };

    /**
     * @brief Value-only texture state captured from the exact ModelResource
     * after deferred streaming has completed.
     *
     * @ref EcsModelAssetMetadata remains the immutable source-identity
     * inventory captured at CPU-ready time. This separate receipt is the
     * current fully-resident value proof: it deliberately owns no resource
     * handle or pointer, and keeps the source inventory's stable texture
     * order so consumers never need to inspect mutable Resource objects.
     */
    struct EcsModelFullyResidentTextureReceipt
    {
        AssetId sourceModelAssetId{};
        /** Stable first-use order, exactly matching modelMetadata.textures. */
        std::vector<EcsModelAssetMetadata::Texture> textures;

        [[nodiscard]] bool HasPublishedSource() const noexcept
        {
            return sourceModelAssetId.IsValid();
        }
    };

    /** @brief Exact value request whose removal must be proven by Render. */
    struct EcsSceneAssetRetirementRequest
    {
        ECS::SceneRuntimeId sceneRuntimeId;
        ECS::EntityHandle rootEntity = ECS::EntityHandle::Invalid();
        std::vector<ECS::EntityHandle> members;
    };

    enum class EcsSceneAssetRetirementProofState : uint8
    {
        Pending = 0,
        AppliedNotPresented,
        Presented,
        /**
         * @brief Explicit safe evidence that none of the requested identities
         * ever entered the trusted RenderScene baseline. Both revision fields
         * remain zero; callers must not fabricate a presentation value.
         */
        NeverPublished,
        Failed,
        DeviceLost,
    };

    struct EcsSceneAssetRetirementToken
    {
        uint64 value = 0;
        uint64 generation = 0;

        [[nodiscard]] bool IsValid() const noexcept
        {
            return value != 0 && generation != 0;
        }

        auto operator<=>(const EcsSceneAssetRetirementToken&) const = default;
    };

    enum class EcsSceneAssetRetirementBeginCode : uint8
    {
        Accepted = 0,
        Rejected,
        FailedRetained,
        DeviceLost,
    };

    /** @brief Admission receipt proving Render captured the exact retiring member set. */
    struct EcsSceneAssetRetirementBeginReceipt
    {
        EcsSceneAssetRetirementBeginCode code =
            EcsSceneAssetRetirementBeginCode::Rejected;
        EcsSceneAssetRetirementToken token;
        EcsSceneAssetRetirementRequest request;
        std::string diagnostic;

        [[nodiscard]] bool IsAccepted() const noexcept
        {
            return code == EcsSceneAssetRetirementBeginCode::Accepted && token.IsValid();
        }
    };

    /**
     * @brief Value-only response to one exact ECS retirement request.
     *
     * The gateway must echo the exact runtime, root and complete member list.
     * A mismatched response is ambiguous and therefore retained.
     */
    struct EcsSceneAssetRetirementProof
    {
        EcsSceneAssetRetirementProofState state =
            EcsSceneAssetRetirementProofState::Pending;
        EcsSceneAssetRetirementRequest request;
        uint64 appliedRenderSceneRevision = 0;
        uint64 presentedFrameSequence = 0;
        std::string diagnostic;
    };

    /** @brief Narrow Render/ECS retirement evidence seam; no Actor or RHI access. */
    class IEcsSceneAssetRetirementProofGateway
    {
    public:
        virtual ~IEcsSceneAssetRetirementProofGateway() = default;
        /** Capture exact Render-side removal ownership before Scene destruction is requested. */
        [[nodiscard]] virtual EcsSceneAssetRetirementBeginReceipt BeginRetirement(
            const EcsSceneAssetRetirementRequest& request) = 0;
        [[nodiscard]] virtual EcsSceneAssetRetirementProof QueryRetirementProof(
            EcsSceneAssetRetirementToken token) const = 0;
        /**
         * @brief Release a consumed terminal proof token.
         *
         * Acknowledgement is intentionally separate from Query so a failed
         * Scene cleanup can retain the exact Render evidence for diagnosis.
         */
        [[nodiscard]] virtual bool AcknowledgeRetirementProof(
            EcsSceneAssetRetirementToken token) = 0;
    };

    /** @brief Value-only status retained for generation-safe request handles. */
    struct EcsSceneAssetLoadStatus
    {
        EcsSceneAssetLoadState state = EcsSceneAssetLoadState::Requested;
        Resource::ResourceLoadSnapshot request;
        Resource::AssetKey assetKey;
        /**
         * @brief Value-only source/dependency inventory captured at Ready.
         *
         * This remains unchanged through ECS adoption, streaming, retirement,
         * and retained-failure states. A default value denotes a request that
         * has not yet published a ModelResource.
         */
        EcsModelAssetMetadata modelMetadata;
        ECS::SceneRuntimeId sceneRuntimeId;
        ECS::EntityHandle rootEntity = ECS::EntityHandle::Invalid();
        /**
         * @brief Complete deterministic batch-to-entity mapping.
         *
         * This survives CPU-hidden, residency and streaming states so callers
         * can select duplicated source node names without retaining Geometry
         * Nodes or performing a hash lookup.
         */
        std::vector<PreparedModelEntityMapping> entityMappings;
        std::vector<ECS::EntityHandle> members;
        uint64 adoptedStructuralRevision = 0;
        std::vector<EcsSceneAssetRenderableVisibilityVersion> renderableVisibilityVersions;
        uint64 minimumResidentFrozenSourceSnapshotRevision = 0;
        uint64 minimumResidentRenderSceneRevision = 0;
        uint64 minimumResidentPresentedFrameSequence = 0;
        /** Present only after ResourceSubsystem::BeginModelTextureStreaming succeeds. */
        std::optional<EcsModelTextureStreamingStartReceipt> textureStreamingStartReceipt;
        /**
         * @brief Present only while state is FullyResident and every exact
         * texture value has been re-read from the owning ModelResource.
         */
        std::optional<EcsModelFullyResidentTextureReceipt> fullyResidentTextureReceipt;
        Resource::ResourceSceneClosureReleaseReceipt closureRelease;
        std::string diagnostic;

        [[nodiscard]] bool IsTerminal() const noexcept
        {
            return state == EcsSceneAssetLoadState::Cancelled ||
                   state == EcsSceneAssetLoadState::Failed ||
                   state == EcsSceneAssetLoadState::FailedRetained ||
                   state == EcsSceneAssetLoadState::DeviceLost ||
                   state == EcsSceneAssetLoadState::Recycled;
        }

        [[nodiscard]] ECS::EntityHandle FindEntityByTemporaryNodeId(
            PreparedModelNodeId id) const;
        [[nodiscard]] std::vector<ECS::EntityHandle> FindEntitiesByExactSourceName(
            std::string_view sourceName) const;
        [[nodiscard]] std::optional<ECS::EntityHandle> FindUniqueEntityByExactSourceName(
            std::string_view sourceName) const;
    };

    /**
     * @brief Coordinates prepared Model loading and pure ECS adoption on one owner thread.
     *
     * Worker threads only advance ResourceLoadOperation state. Every Scene ECS
     * mutation is performed during Update, Cancel, or host shutdown on the
     * constructor thread.
     */
    class EcsSceneAssetLoadCoordinator final
    {
    public:
        EcsSceneAssetLoadCoordinator(
            SceneECS::SceneEcsRuntime& runtime,
            Resource::ResourceSubsystem& resources,
            IEcsSceneAssetRetirementProofGateway* retirementProofGateway = nullptr) noexcept;
        ~EcsSceneAssetLoadCoordinator() noexcept;

        EcsSceneAssetLoadCoordinator(const EcsSceneAssetLoadCoordinator&) = delete;
        EcsSceneAssetLoadCoordinator& operator=(const EcsSceneAssetLoadCoordinator&) = delete;

        [[nodiscard]] EcsSceneAssetLoadHandle RequestModel(
            EcsModelAssetLoadDesc desc, std::string& outError);
        [[nodiscard]] bool Update();
        /** @brief Accept only exact per-member applied-and-presented activation evidence. */
        [[nodiscard]] bool ConfirmMinimumResidentPresentation(
            EcsSceneAssetLoadHandle handle,
            const EcsSceneAssetMinimumResidentPresentationReceipt& receipt);
        [[nodiscard]] bool Cancel(EcsSceneAssetLoadHandle handle);
        [[nodiscard]] bool PrepareForHostShutdown() noexcept;

        [[nodiscard]] bool IsValid(EcsSceneAssetLoadHandle handle) const;
        [[nodiscard]] std::optional<EcsSceneAssetLoadStatus> GetStatus(
            EcsSceneAssetLoadHandle handle) const;
        [[nodiscard]] Resource::ResourceLoadRequestId GetResourceRequestId(
            EcsSceneAssetLoadHandle handle) const;

    private:
        struct Entry;

        [[nodiscard]] bool IsOwnerThread() const noexcept;
        [[nodiscard]] Entry* Resolve(EcsSceneAssetLoadHandle handle);
        [[nodiscard]] const Entry* Resolve(EcsSceneAssetLoadHandle handle) const;
        [[nodiscard]] EcsSceneAssetLoadHandle Allocate(std::unique_ptr<Entry> entry);
        [[nodiscard]] bool UpdateEntry(Entry& entry);
        [[nodiscard]] bool AdoptHidden(Entry& entry);
        [[nodiscard]] bool EnableMinimumResident(Entry& entry);
        [[nodiscard]] bool CheckMinimumDependencies(Entry& entry);
        [[nodiscard]] bool BeginRetirement(Entry& entry);
        [[nodiscard]] bool AdvanceRetirement(Entry& entry);
        [[nodiscard]] bool AdvanceEcsRecycle(Entry& entry);
        [[nodiscard]] bool PopulateModelMetadata(Entry& entry);
        [[nodiscard]] bool BuildFullyResidentTextureReceipt(Entry& entry);
        void Fail(Entry& entry, EcsSceneAssetLoadState state, std::string diagnostic);
        void ReleaseEntry(EcsSceneAssetLoadHandle handle) noexcept;

        SceneECS::SceneEcsRuntime& m_runtime;
        Resource::ResourceSubsystem& m_resources;
        IEcsSceneAssetRetirementProofGateway* m_retirementProofGateway = nullptr;
        std::thread::id m_ownerThread;
        HandlePool<EcsSceneAssetLoadHandle> m_handles;
        std::vector<std::unique_ptr<Entry>> m_entries;
        bool m_hostShutdown = false;
    };
} // namespace RVX::ResourceSceneAdapters
