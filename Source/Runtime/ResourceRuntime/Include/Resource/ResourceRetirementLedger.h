#pragma once

/**
 * @file ResourceRetirementLedger.h
 * @brief Generation-safe update-side proof for a released resource closure.
 */

#include "RenderContracts/IRenderResourceGateway.h"
#include "Resource/IResource.h"
#include "Resource/ResourceLoadOperation.h"

#include <compare>
#include <cstddef>
#include <optional>
#include <unordered_map>
#include <vector>

namespace RVX::Resource
{
    /** @brief Why ResourceManager removed one exact CPU ownership closure. */
    enum class ResourceClosureRetirementReason : uint8
    {
        Invalid = 0,
        ExplicitUnload,
        CacheEviction,
        SceneTeardown,
    };

    /** @brief ResourceManager-owned closure result fed to ResourceSubsystem later. */
    struct ResourceClosureRetirementOutcome
    {
        AssetId rootAssetId{};
        uint64 closureGeneration = 0;
        ResourceClosureRetirementReason reason =
            ResourceClosureRetirementReason::Invalid;
        std::vector<ResourceId> removedResourceIds{};
        std::vector<ResourceId> sharedRetainedResourceIds{};

        [[nodiscard]] bool IsStructurallyValid() const noexcept;
    };

    /** @brief Captured exact Render generation for one Resource-owned asset. */
    struct ResourceRetirementHandleCapture
    {
        ResourceId resourceId = InvalidResourceId;
        RenderResourceHandle handle{};

        [[nodiscard]] bool IsValid() const noexcept
        {
            return resourceId != InvalidResourceId && handle.IsValid();
        }
    };

    /** @brief Opaque generation-qualified receipt key. */
    struct ResourceRetirementToken
    {
        uint64 value = 0;
        uint64 generation = 0;

        [[nodiscard]] bool IsValid() const noexcept
        {
            return value != 0 && generation != 0;
        }

        auto operator<=>(const ResourceRetirementToken&) const = default;
    };

    /** @brief Update-side retirement lifecycle; no RHI fence or WaitIdle is exposed. */
    enum class ResourceClosureRetirementState : uint8
    {
        Requested = 0,
        ReleaseAccepted,
        AwaitingGpuLastUse,
        Completed,
        Failed,
        DeviceLost,
    };

    /** @brief Copyable evidence for one closure's Render retirement. */
    struct ResourceClosureRetirementReceipt
    {
        ResourceRetirementToken token{};
        AssetId rootAssetId{};
        uint64 closureGeneration = 0;
        ResourceClosureRetirementReason reason =
            ResourceClosureRetirementReason::Invalid;
        ResourceClosureRetirementState state =
            ResourceClosureRetirementState::Failed;
        uint64 removedResourceCount = 0;
        uint64 sharedRetainedResourceCount = 0;
        uint64 releaseAcceptedCount = 0;
        uint64 gatewayRejectedCount = 0;
        uint64 pendingGpuResourceCount = 0;

        [[nodiscard]] bool IsValid() const noexcept
        {
            return token.IsValid() && rootAssetId.IsValid() &&
                   closureGeneration != 0;
        }

        [[nodiscard]] bool IsTerminal() const noexcept
        {
            return state == ResourceClosureRetirementState::Completed ||
                   state == ResourceClosureRetirementState::Failed ||
                   state == ResourceClosureRetirementState::DeviceLost;
        }
    };

    enum class ResourceClosureRetirementSubmitCode : uint8
    {
        Accepted = 0,
        InvalidOutcome,
        DuplicateClosureGeneration,
        GatewayUnavailable,
        StaleBeforeAdmission,
        GatewayRejected,
        DeviceLost,
        TokenExhausted,
        AllocationFailure,
        NotInitialized,
        WrongThread,
        RenderShuttingDown,
    };

    struct ResourceClosureRetirementSubmitResult
    {
        ResourceClosureRetirementSubmitCode code =
            ResourceClosureRetirementSubmitCode::InvalidOutcome;
        ResourceClosureRetirementReceipt receipt{};

        [[nodiscard]] bool IsAccepted() const noexcept
        {
            return code == ResourceClosureRetirementSubmitCode::Accepted;
        }
    };

    enum class ResourceClosureRetirementPollCode : uint8
    {
        Updated = 0,
        StaleToken,
    };

    struct ResourceClosureRetirementPollResult
    {
        ResourceClosureRetirementPollCode code =
            ResourceClosureRetirementPollCode::StaleToken;
        ResourceClosureRetirementReceipt receipt{};
    };

    enum class ResourceClosureRetirementAcknowledgeCode : uint8
    {
        Acknowledged = 0,
        NotTerminal,
        StaleToken,
    };

    struct ResourceClosureRetirementAcknowledgeResult
    {
        ResourceClosureRetirementAcknowledgeCode code =
            ResourceClosureRetirementAcknowledgeCode::StaleToken;
        ResourceClosureRetirementReceipt receipt{};
    };

    /** @brief Opaque Scene-consumer request identity, separate from GPU token. */
    struct ResourceSceneClosureReleaseToken
    {
        uint64 value = 0;
        uint64 generation = 0;

        [[nodiscard]] bool IsValid() const noexcept
        {
            return value != 0 && generation != 0;
        }

        auto operator<=>(const ResourceSceneClosureReleaseToken&) const = default;
    };

    enum class ResourceSceneClosureReleaseState : uint8
    {
        Queued = 0,
        AwaitingGpuLastUse,
        CompletedShared,
        Completed,
        FailedRetained,
        DeviceLost,
    };

    /** @brief Value-only status for a Scene consumer's moved residency lease. */
    struct ResourceSceneClosureReleaseReceipt
    {
        ResourceSceneClosureReleaseToken token{};
        AssetKey assetKey{};
        AssetId rootAssetId{};
        uint64 leaseGeneration = 0;
        uint64 closureGeneration = 0;
        ResourceSceneClosureReleaseState state =
            ResourceSceneClosureReleaseState::FailedRetained;
        ResourceRetirementToken renderRetirementToken{};
        uint64 sharedRetainedResourceCount = 0;

        [[nodiscard]] bool IsTerminal() const noexcept
        {
            return state == ResourceSceneClosureReleaseState::CompletedShared ||
                   state == ResourceSceneClosureReleaseState::Completed ||
                   state == ResourceSceneClosureReleaseState::FailedRetained ||
                   state == ResourceSceneClosureReleaseState::DeviceLost;
        }
    };

    enum class ResourceSceneClosureReleaseBeginCode : uint8
    {
        Accepted = 0,
        InvalidLease,
        NotInitialized,
        WrongThread,
        AllocationFailure,
        RetainedFailure,
    };

    struct ResourceSceneClosureReleaseBeginResult
    {
        ResourceSceneClosureReleaseBeginCode code =
            ResourceSceneClosureReleaseBeginCode::InvalidLease;
        ResourceSceneClosureReleaseReceipt receipt{};

        [[nodiscard]] bool IsAccepted() const noexcept
        {
            return code == ResourceSceneClosureReleaseBeginCode::Accepted;
        }
    };

    enum class ResourceSceneClosureReleaseAcknowledgeCode : uint8
    {
        Acknowledged = 0,
        NotTerminal,
        StaleToken,
    };

    struct ResourceSceneClosureReleaseAcknowledgeResult
    {
        ResourceSceneClosureReleaseAcknowledgeCode code =
            ResourceSceneClosureReleaseAcknowledgeCode::StaleToken;
        ResourceSceneClosureReleaseReceipt receipt{};
    };

    /**
     * @brief Owner-thread ledger for Render last-use polling of CPU closures.
     *
     * ResourceSubsystem captures RenderResourceHandle values from its tracked
     * map before calling Begin.  This ledger intentionally cannot resolve by
     * ResourceId: after a release, a ResourceId may point to a reused Render
     * slot with a different generation.
     */
    class ResourceRetirementLedger final
    {
    public:
        [[nodiscard]] ResourceClosureRetirementSubmitResult Begin(
            const ResourceClosureRetirementOutcome& outcome,
            const std::vector<ResourceRetirementHandleCapture>& captures,
            IRenderResourceGateway* gateway) noexcept;

        /** @brief Compatibility overload for closures with a live Render gateway. */
        [[nodiscard]] ResourceClosureRetirementSubmitResult Begin(
            const ResourceClosureRetirementOutcome& outcome,
            const std::vector<ResourceRetirementHandleCapture>& captures,
            IRenderResourceGateway& gateway) noexcept
        {
            return Begin(outcome, captures, &gateway);
        }

        [[nodiscard]] ResourceClosureRetirementPollResult Poll(
            ResourceRetirementToken token,
            IRenderResourceGateway& gateway);

        void PollAll(IRenderResourceGateway& gateway);

        /** @brief Record terminal device loss without exposing backend/RHI APIs. */
        void NotifyDeviceLost();

        [[nodiscard]] std::optional<ResourceClosureRetirementReceipt> Query(
            ResourceRetirementToken token) const;

        [[nodiscard]] ResourceClosureRetirementAcknowledgeResult Acknowledge(
            ResourceRetirementToken token);

        /** @brief Read stable exact captures until the matching token is acknowledged. */
        [[nodiscard]] const std::vector<ResourceRetirementHandleCapture>*
            GetCaptures(ResourceRetirementToken token) const noexcept;

        [[nodiscard]] size_t GetOutstandingGpuRetirementCount() const noexcept;

    private:
        struct Entry
        {
            ResourceClosureRetirementReceipt receipt{};
            std::vector<ResourceRetirementHandleCapture> captures{};
            std::vector<bool> releaseAdmitted{};
            std::vector<bool> terminalObserved{};
            std::vector<bool> failureObserved{};
        };

        [[nodiscard]] static bool HasStaleStatus(
            const RenderResourceStatus& status) noexcept;
        [[nodiscard]] static bool HasDeviceLost(
            const RenderResourceStatus& status) noexcept;
        [[nodiscard]] static bool IsReleasedOrStale(
            const RenderResourceStatus& status) noexcept;
        [[nodiscard]] static bool Contains(
            const std::vector<ResourceId>& values,
            ResourceId value) noexcept;
        [[nodiscard]] bool IsCaptureSetValid(
            const ResourceClosureRetirementOutcome& outcome,
            const std::vector<ResourceRetirementHandleCapture>& captures) const noexcept;
        [[nodiscard]] ResourceClosureRetirementPollResult PollEntry(
            Entry& entry,
            IRenderResourceGateway& gateway);

        uint64 m_nextTokenValue = 1;
        std::unordered_map<uint64, Entry> m_entries{};
        // Keep only the highest seen generation for each root.  This survives
        // acknowledgement so an old closure notification cannot release a
        // later Render generation after the ResourceId is reused.
        std::unordered_map<AssetId, uint64, AssetIdHash>
            m_highestClosureGeneration{};
    };
} // namespace RVX::Resource

namespace RVX
{
    using Resource::ResourceClosureRetirementAcknowledgeCode;
    using Resource::ResourceClosureRetirementAcknowledgeResult;
    using Resource::ResourceClosureRetirementOutcome;
    using Resource::ResourceClosureRetirementPollCode;
    using Resource::ResourceClosureRetirementPollResult;
    using Resource::ResourceClosureRetirementReason;
    using Resource::ResourceClosureRetirementReceipt;
    using Resource::ResourceClosureRetirementState;
    using Resource::ResourceClosureRetirementSubmitCode;
    using Resource::ResourceClosureRetirementSubmitResult;
    using Resource::ResourceRetirementHandleCapture;
    using Resource::ResourceRetirementLedger;
    using Resource::ResourceRetirementToken;
    using Resource::ResourceSceneClosureReleaseAcknowledgeCode;
    using Resource::ResourceSceneClosureReleaseAcknowledgeResult;
    using Resource::ResourceSceneClosureReleaseBeginCode;
    using Resource::ResourceSceneClosureReleaseBeginResult;
    using Resource::ResourceSceneClosureReleaseReceipt;
    using Resource::ResourceSceneClosureReleaseState;
    using Resource::ResourceSceneClosureReleaseToken;
} // namespace RVX
