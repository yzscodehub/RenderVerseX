#include "RenderContracts/RenderFeatureChannelRegistry.h"

#include <type_traits>
#include <utility>

namespace RVX
{
namespace
{
    [[nodiscard]] constexpr std::optional<size_t> ToRegistrationIndex(
        RenderFeatureChannelId channelId) noexcept
    {
        if (!IsKnownRenderFeatureChannel(channelId))
        {
            return std::nullopt;
        }

        const uint32 value = static_cast<uint32>(channelId);
        if (value == 0 || value > RVX_RENDER_FEATURE_CHANNEL_COUNT)
        {
            return std::nullopt;
        }
        return static_cast<size_t>(value - 1U);
    }

    [[nodiscard]] constexpr bool IsKnownProducerEndpoint(
        RenderFeatureProducerEndpointId endpoint) noexcept
    {
        return endpoint ==
               RenderFeatureProducerEndpointId::SceneFeatureSnapshotStore;
    }

    [[nodiscard]] constexpr bool IsKnownConsumerEndpoint(
        RenderFeatureConsumerEndpointId endpoint) noexcept
    {
        return endpoint == RenderFeatureConsumerEndpointId::RenderSceneDatabase;
    }
} // namespace

RenderFeatureChannelRegistryCode RenderFeatureChannelRegistry::Register(
    const RenderFeatureChannelRegistration& registration) noexcept
{
    if (m_sealed)
    {
        return RenderFeatureChannelRegistryCode::RegistrySealed;
    }

    const std::optional<size_t> index =
        ToRegistrationIndex(registration.channelId);
    if (!index.has_value())
    {
        return RenderFeatureChannelRegistryCode::UnknownChannel;
    }
    if (registration.schemaVersion !=
        GetRenderFeatureChannelSchemaVersion(registration.channelId))
    {
        return RenderFeatureChannelRegistryCode::SchemaMismatch;
    }
    if (!IsKnownProducerEndpoint(registration.producerEndpoint))
    {
        return RenderFeatureChannelRegistryCode::InvalidProducerEndpoint;
    }
    if (!IsKnownConsumerEndpoint(registration.consumerEndpoint))
    {
        return RenderFeatureChannelRegistryCode::InvalidConsumerEndpoint;
    }
    if (m_registrations[*index].has_value())
    {
        return RenderFeatureChannelRegistryCode::DuplicateChannel;
    }

    m_registrations[*index] = registration;
    ++m_registrationCount;
    return RenderFeatureChannelRegistryCode::Success;
}

RenderFeatureChannelRegistryCode RenderFeatureChannelRegistry::Seal() noexcept
{
    if (m_sealed)
    {
        return RenderFeatureChannelRegistryCode::RegistrySealed;
    }
    if (m_registrationCount != RVX_RENDER_FEATURE_CHANNEL_COUNT)
    {
        return RenderFeatureChannelRegistryCode::MissingChannel;
    }
    for (const auto& registration : m_registrations)
    {
        if (!registration.has_value())
        {
            return RenderFeatureChannelRegistryCode::MissingChannel;
        }
    }

    m_sealed = true;
    return RenderFeatureChannelRegistryCode::Success;
}

const RenderFeatureChannelRegistration* RenderFeatureChannelRegistry::Find(
    RenderFeatureChannelId channelId) const noexcept
{
    if (!m_sealed)
    {
        return nullptr;
    }
    const std::optional<size_t> index = ToRegistrationIndex(channelId);
    if (!index.has_value() || !m_registrations[*index].has_value())
    {
        return nullptr;
    }
    return &*m_registrations[*index];
}

RenderFeatureChannelRegistryCode RenderFeatureChannelRegistry::ValidateProducer(
    RenderFeatureChannelId channelId,
    uint32 schemaVersion,
    RenderFeatureProducerEndpointId producerEndpoint) const noexcept
{
    if (!m_sealed)
    {
        return RenderFeatureChannelRegistryCode::RegistryNotSealed;
    }
    if (!IsKnownRenderFeatureChannel(channelId))
    {
        return RenderFeatureChannelRegistryCode::UnknownChannel;
    }
    if (schemaVersion != GetRenderFeatureChannelSchemaVersion(channelId))
    {
        return RenderFeatureChannelRegistryCode::SchemaMismatch;
    }
    if (!IsKnownProducerEndpoint(producerEndpoint))
    {
        return RenderFeatureChannelRegistryCode::InvalidProducerEndpoint;
    }

    const RenderFeatureChannelRegistration* registration = Find(channelId);
    if (registration == nullptr ||
        registration->producerEndpoint != producerEndpoint)
    {
        return RenderFeatureChannelRegistryCode::RouteMismatch;
    }
    return RenderFeatureChannelRegistryCode::Success;
}

RenderFeatureChannelRegistryCode RenderFeatureChannelRegistry::ValidateRoute(
    RenderFeatureChannelId channelId,
    uint32 schemaVersion,
    RenderFeatureProducerEndpointId producerEndpoint,
    RenderFeatureConsumerEndpointId consumerEndpoint) const noexcept
{
    const RenderFeatureChannelRegistryCode producerResult =
        ValidateProducer(channelId, schemaVersion, producerEndpoint);
    if (producerResult != RenderFeatureChannelRegistryCode::Success)
    {
        return producerResult;
    }
    if (!IsKnownConsumerEndpoint(consumerEndpoint))
    {
        return RenderFeatureChannelRegistryCode::InvalidConsumerEndpoint;
    }

    const RenderFeatureChannelRegistration* registration = Find(channelId);
    if (registration == nullptr ||
        registration->consumerEndpoint != consumerEndpoint)
    {
        return RenderFeatureChannelRegistryCode::RouteMismatch;
    }
    return RenderFeatureChannelRegistryCode::Success;
}

RenderFeatureChannelRegistry CreateBuiltinRenderFeatureChannelRegistry() noexcept
{
    RenderFeatureChannelRegistry registry;
    bool registeredAll = true;
    std::apply(
        [&](auto... descriptor)
        {
            ((registeredAll =
                  registeredAll &&
                  registry.Register({
                      .channelId = std::remove_cvref_t<
                          decltype(descriptor)>::ChannelId,
                      .schemaVersion = std::remove_cvref_t<
                          decltype(descriptor)>::SchemaVersion,
                      .producerEndpoint =
                          RenderFeatureProducerEndpointId::SceneFeatureSnapshotStore,
                      .consumerEndpoint =
                          RenderFeatureConsumerEndpointId::RenderSceneDatabase,
                  }) == RenderFeatureChannelRegistryCode::Success),
             ...);
        },
        RenderFeatureChannelCatalog{});
    if (!registeredAll ||
        registry.Seal() != RenderFeatureChannelRegistryCode::Success)
    {
        return {};
    }
    return registry;
}
} // namespace RVX
