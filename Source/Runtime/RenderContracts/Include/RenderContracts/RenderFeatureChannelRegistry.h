#pragma once

/**
 * @file RenderFeatureChannelRegistry.h
 * @brief Sealed composition routes for the fixed GFX01 render-feature channels.
 */

#include "RenderContracts/FeatureChannelContracts.h"

#include <array>
#include <cstddef>
#include <optional>

namespace RVX
{
    /** @brief Stable producer endpoint identities used by the runtime composition. */
    enum class RenderFeatureProducerEndpointId : uint32
    {
        Invalid = 0,
        SceneFeatureSnapshotStore = 1,
    };

    /** @brief Stable consumer endpoint identities used by the runtime composition. */
    enum class RenderFeatureConsumerEndpointId : uint32
    {
        Invalid = 0,
        RenderSceneDatabase = 1,
    };

    /** @brief One exact typed route. A channel may be registered only once. */
    struct RenderFeatureChannelRegistration
    {
        RenderFeatureChannelId channelId = RenderFeatureChannelId::Invalid;
        uint32 schemaVersion = 0;
        RenderFeatureProducerEndpointId producerEndpoint =
            RenderFeatureProducerEndpointId::Invalid;
        RenderFeatureConsumerEndpointId consumerEndpoint =
            RenderFeatureConsumerEndpointId::Invalid;

        constexpr bool operator==(
            const RenderFeatureChannelRegistration&) const = default;
    };

    enum class RenderFeatureChannelRegistryCode : uint8
    {
        Success = 0,
        RegistrySealed,
        RegistryNotSealed,
        UnknownChannel,
        SchemaMismatch,
        InvalidProducerEndpoint,
        InvalidConsumerEndpoint,
        DuplicateChannel,
        MissingChannel,
        RouteMismatch,
    };

    /**
     * @brief Fixed GFX01 composition registry for Particle, Water, and Terrain.
     *
     * This deliberately describes three compile-time payload types backed by
     * fixed Scene/Render storage. It is not a dynamic plug-in payload registry.
     * Composition registers one producer and one consumer endpoint per channel,
     * then Seal() makes every subsequent mutation fail closed.
     */
    class RenderFeatureChannelRegistry final
    {
    public:
        [[nodiscard]] RenderFeatureChannelRegistryCode Register(
            const RenderFeatureChannelRegistration& registration) noexcept;

        [[nodiscard]] RenderFeatureChannelRegistryCode Seal() noexcept;

        [[nodiscard]] bool IsSealed() const noexcept
        {
            return m_sealed;
        }

        [[nodiscard]] size_t GetRegistrationCount() const noexcept
        {
            return m_registrationCount;
        }

        [[nodiscard]] const RenderFeatureChannelRegistration* Find(
            RenderFeatureChannelId channelId) const noexcept;

        [[nodiscard]] RenderFeatureChannelRegistryCode ValidateProducer(
            RenderFeatureChannelId channelId,
            uint32 schemaVersion,
            RenderFeatureProducerEndpointId producerEndpoint) const noexcept;

        [[nodiscard]] RenderFeatureChannelRegistryCode ValidateRoute(
            RenderFeatureChannelId channelId,
            uint32 schemaVersion,
            RenderFeatureProducerEndpointId producerEndpoint,
            RenderFeatureConsumerEndpointId consumerEndpoint) const noexcept;

    private:
        std::array<std::optional<RenderFeatureChannelRegistration>,
                   RVX_RENDER_FEATURE_CHANNEL_COUNT>
            m_registrations{};
        size_t m_registrationCount = 0;
        bool m_sealed = false;
    };

    /** @brief Build the required sealed Scene-store to Render-database composition. */
    [[nodiscard]] RenderFeatureChannelRegistry
    CreateBuiltinRenderFeatureChannelRegistry() noexcept;

    static_assert(static_cast<uint32>(
                      RenderFeatureProducerEndpointId::SceneFeatureSnapshotStore) ==
                  1U);
    static_assert(static_cast<uint32>(
                      RenderFeatureConsumerEndpointId::RenderSceneDatabase) ==
                  1U);
} // namespace RVX
