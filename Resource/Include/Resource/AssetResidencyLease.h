#pragma once

/**
 * @file AssetResidencyLease.h
 * @brief Exact-variant CPU residency pinning for cached resource closures.
 */

#include "Resource/ResourceLoadOperation.h"

#include <cstddef>
#include <memory>
#include <vector>

namespace RVX::Resource
{
    class AssetResidencyLeaseControl;
    class ResourceManager;

    /** @brief Result of a lease-aware explicit unload or eviction request. */
    enum class AssetResidencyReleaseResult : uint8
    {
        NotFound = 0,
        Unloaded,
        Queued,
        BlockedByLease,
        Rejected,
        // CPU ownership intentionally remains resident because a precise
        // Render retirement was admitted only partially or failed closed.
        RetainedFailure
    };

    /**
     * @brief Move-only pin for one exact AssetKey and its published dependency closure.
     *
     * A lease protects CPU cache ownership only. It deliberately does not own
     * RHI objects or authorize off-thread render retirement. The generation is
     * checked on release so a stale moved-from or post-unload lease can never
     * decrement a later residency epoch for the same AssetKey.
     */
    class AssetResidencyLease final
    {
    public:
        AssetResidencyLease() = default;
        ~AssetResidencyLease();

        AssetResidencyLease(const AssetResidencyLease&) = delete;
        AssetResidencyLease& operator=(const AssetResidencyLease&) = delete;

        AssetResidencyLease(AssetResidencyLease&& other) noexcept;
        AssetResidencyLease& operator=(AssetResidencyLease&& other) noexcept;

        [[nodiscard]] bool IsValid() const noexcept;
        [[nodiscard]] const AssetKey& GetAssetKey() const noexcept;
        [[nodiscard]] uint64 GetGeneration() const noexcept;
        [[nodiscard]] const std::vector<ResourceId>& GetDependencyClosure() const noexcept;

        /** @brief Release this pin. A deferred explicit unload is owner-thread queued. */
        void Reset() noexcept;

    private:
        AssetResidencyLease(std::weak_ptr<AssetResidencyLeaseControl> control,
                            AssetKey assetKey,
                            uint64 generation,
                            std::vector<ResourceId> dependencyClosure) noexcept;

        std::weak_ptr<AssetResidencyLeaseControl> m_control;
        AssetKey m_assetKey;
        uint64 m_generation = 0;
        std::vector<ResourceId> m_dependencyClosure;

        friend class ResourceManager;
    };
} // namespace RVX::Resource

namespace RVX
{
    using Resource::AssetResidencyLease;
    using Resource::AssetResidencyReleaseResult;
} // namespace RVX
