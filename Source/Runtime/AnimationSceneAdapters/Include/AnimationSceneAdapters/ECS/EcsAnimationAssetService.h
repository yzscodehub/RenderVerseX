#pragma once

/**
 * @file EcsAnimationAssetService.h
 * @brief Engine-owned scene-qualified AnimationResource requests for pure ECS.
 */

#include "Animation/Data/Skeleton.h"
#include "AnimationSceneAdapters/ECS/ResourceAnimationEcsEvaluator.h"
#include "Core/Handle.h"
#include "Resource/ResourceLoadOperation.h"
#include "Scene/ECS/AnimationFragments.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace RVX::Resource
{
    class ResourceSubsystem;
    class AnimationResource;
}

namespace RVX::AnimationSceneAdapters
{
    struct EcsAnimationAssetLoadHandleTag final
    {
    };
    using EcsAnimationAssetLoadHandle = Handle<EcsAnimationAssetLoadHandleTag, uint32>;
    inline constexpr EcsAnimationAssetLoadHandle InvalidEcsAnimationAssetLoadHandle =
        EcsAnimationAssetLoadHandle::Invalid();

    /**
     * @brief A generation-safe handle explicitly qualified by its owning Scene runtime.
     *
     * A shared service must never accept a valid request handle from another World merely
     * because the handle generation happens to match. Every mutation and query requires this
     * complete value.
     */
    struct EcsAnimationAssetLoadRef
    {
        ECS::SceneRuntimeId sceneRuntimeId;
        EcsAnimationAssetLoadHandle handle = InvalidEcsAnimationAssetLoadHandle;

        [[nodiscard]] bool IsValid() const noexcept
        {
            return sceneRuntimeId.IsValid() && handle.IsValid();
        }

        auto operator<=>(const EcsAnimationAssetLoadRef&) const = default;
    };
    using EcsAnimationAssetRequestRef = EcsAnimationAssetLoadRef;

    /** @brief Immutable caller input for one AnimationResource request. */
    struct EcsAnimationAssetLoadDesc
    {
        std::string path;
        Resource::ResourceLoadOptions resourceOptions;
        ECS::SceneRuntimeId expectedSceneRuntimeId;
    };
    using EcsAnimationAssetRequestDesc = EcsAnimationAssetLoadDesc;

    enum class EcsAnimationAssetLoadState : uint8
    {
        Requested = 0,
        Loading,
        Ready,
        Failed,
        Cancelled,
    };

    /** @brief Stable lexical metadata for one immutable clip. */
    struct EcsAnimationClipMetadata
    {
        uint32 ordinal = 0;
        std::string name;
        int64 durationUs = 0;
        bool hasRootMotion = false;
        std::string rootMotionBoneName;
    };

    /**
     * @brief Value-only public state for one asynchronous animation request.
     *
     * ResourceHandle, Resource pointer, and shared ownership are deliberately absent. The
     * resolver bridge retains ownership privately only while an evaluator needs the immutable
     * payload.
     */
    struct EcsAnimationAssetLoadStatus
    {
        EcsAnimationAssetLoadState state = EcsAnimationAssetLoadState::Requested;
        Resource::ResourceLoadState resourceLoadState = Resource::ResourceLoadState::Queued;
        Resource::ResourceLoadSnapshot request;
        Resource::AssetKey assetKey;
        ECS::SceneRuntimeId sceneRuntimeId;
        Resource::ResourceContentVerificationReceipt contentVerification;
        uint64 animationAssetValue = 0;
        uint32 boneCount = 0;
        std::vector<EcsAnimationClipMetadata> clips;
        std::string diagnostic;

        [[nodiscard]] bool IsTerminal() const noexcept
        {
            return state == EcsAnimationAssetLoadState::Ready ||
                   state == EcsAnimationAssetLoadState::Failed ||
                   state == EcsAnimationAssetLoadState::Cancelled;
        }
    };
    using EcsAnimationAssetRequestStatus = EcsAnimationAssetLoadStatus;

    /** @brief Value topology used to prove binding compatibility without exposing a Skeleton. */
    struct EcsAnimationBoneTopology
    {
        std::string name;
        int32 parentIndex = -1;
        /** @brief Exact authored local bind transform; value-only, never a Skeleton pointer. */
        Animation::TransformSample localBindPose = Animation::TransformSample::Identity();
        /** @brief Exact palette inverse-bind matrix for this lexical bone ordinal. */
        Mat4 inverseBindPose{1.0f};
    };

    struct EcsAnimationSkeletonTopology
    {
        std::vector<EcsAnimationBoneTopology> bones;
    };

    enum class EcsAnimationBindingPreparationCode : uint8
    {
        Prepared = 0,
        InvalidRequestRef,
        InvalidTargetEntity,
        TargetBindingUnavailable,
        TargetBindingMutationRejected,
        RequestNotReady,
        InvalidTargetTopology,
        BoneCountMismatch,
        SkeletonTopologyMismatch,
        SkeletonBindPoseMismatch,
        ClipOrdinalOutOfRange,
        ResourceUnavailable,
        TargetResourceUnavailable,
        ServiceShutDown,
    };

    /** @brief Value result from an exact skeleton-compatible binding preparation. */
    struct EcsAnimationBindingPreparationResult
    {
        EcsAnimationBindingPreparationCode code =
            EcsAnimationBindingPreparationCode::InvalidRequestRef;
        SceneECS::AnimationSkeletonBinding binding;
        std::string diagnostic;

        [[nodiscard]] bool IsPrepared() const noexcept
        {
            return code == EcsAnimationBindingPreparationCode::Prepared;
        }
    };

    /** @brief One Scene-only drain observation for the Engine shutdown path. */
    struct EcsAnimationAssetServiceSceneDiagnosticsSnapshot
    {
        ECS::SceneRuntimeId sceneRuntimeId;
        uint32 requestCount = 0;
        uint32 activeRequestCount = 0;
        uint32 readyRequestCount = 0;
        uint32 failedRequestCount = 0;
        uint32 cancelledRequestCount = 0;
        bool shutdown = false;
        std::string lastDiagnostic;
    };

    /** @brief Aggregate service diagnostics retained independently of individual Worlds. */
    struct EcsAnimationAssetServiceDiagnosticsSnapshot
    {
        uint32 requestCount = 0;
        uint32 activeRequestCount = 0;
        uint32 weakResolverCacheEntryCount = 0;
        bool shutdown = false;
    };

    /**
     * @brief Engine-shared owner of async AnimationResource requests and resolver leases.
     *
     * Requests are mutated only from the ResourceSubsystem update-owner thread. Resolver cache
     * misses call TryAcquireLoaded only: evaluation never starts disk I/O or a registry lookup.
     */
    class EcsAnimationAssetService final
    {
    public:
        explicit EcsAnimationAssetService(Resource::ResourceSubsystem& resources);
        ~EcsAnimationAssetService();

        EcsAnimationAssetService(const EcsAnimationAssetService&) = delete;
        EcsAnimationAssetService& operator=(const EcsAnimationAssetService&) = delete;
        EcsAnimationAssetService(EcsAnimationAssetService&&) = delete;
        EcsAnimationAssetService& operator=(EcsAnimationAssetService&&) = delete;

        [[nodiscard]] EcsAnimationAssetLoadRef Request(
            EcsAnimationAssetLoadDesc desc,
            std::string& outError);
        /** @brief Advance only requests owned by exactly one Scene. */
        [[nodiscard]] bool Update(ECS::SceneRuntimeId sceneRuntimeId);
        [[nodiscard]] bool Cancel(EcsAnimationAssetLoadRef request);
        [[nodiscard]] std::optional<EcsAnimationAssetLoadStatus> GetStatus(
            EcsAnimationAssetLoadRef request) const;

        /** @brief Cancel and prove no active request remains for one exact Scene. */
        [[nodiscard]] bool PrepareForSceneShutdown(ECS::SceneRuntimeId sceneRuntimeId);
        [[nodiscard]] EcsAnimationAssetServiceSceneDiagnosticsSnapshot
        GetSceneDiagnosticsSnapshot(ECS::SceneRuntimeId sceneRuntimeId) const;
        [[nodiscard]] EcsAnimationAssetServiceDiagnosticsSnapshot GetDiagnosticsSnapshot() const;

        /**
         * @brief Build the evaluator resolver backed by this service's weak cache.
         *
         * The callback is fail-closed after service shutdown or destruction. Its private
         * intrusive-to-shared bridge is intentional: this is the only ownership conversion
         * boundary, and no ResourceHandle or raw resource pointer reaches the public request API.
         */
        [[nodiscard]] ResourceAnimationEcsResolver CreateResolver() const;

        /**
         * @brief Replace a procedural authored binding after exact value topology matches.
         *
         * The supplied topology is accepted only for procedural bindings with an asset value of
         * zero. Resource-backed target bindings must use the overload below so the target
         * skeleton is independently reacquired by its exact immutable resource identity.
         */
        [[nodiscard]] EcsAnimationBindingPreparationResult
        PrepareCompatibleAnimationBinding(
            EcsAnimationAssetLoadRef request,
            const SceneECS::AnimationSkeletonBinding& targetBinding,
            const EcsAnimationSkeletonTopology& targetTopology,
            uint32 animationClipOrdinal = 0) const;

        /**
         * @brief Replace a resource-backed binding after exact target-resource skeleton proof.
         *
         * Both source and target resources are acquired through TryAcquireLoaded. This performs
         * no I/O and prevents a caller-supplied topology value from forging or stale-proving a
         * resource-backed binding.
         */
        [[nodiscard]] EcsAnimationBindingPreparationResult
        PrepareCompatibleAnimationBinding(
            EcsAnimationAssetLoadRef request,
            const SceneECS::AnimationSkeletonBinding& targetBinding,
            uint32 animationClipOrdinal = 0) const;

        /** @brief Fail closed, cancel subscriptions, and release every weak resolver entry. */
        void Shutdown();
        [[nodiscard]] bool IsShutdown() const;

    private:
        struct State;

        [[nodiscard]] static std::shared_ptr<const Resource::AnimationResource>
        ResolveAnimationAsset(const std::shared_ptr<State>& state, uint64 animationAssetValue);

        std::shared_ptr<State> m_state;
    };
} // namespace RVX::AnimationSceneAdapters
