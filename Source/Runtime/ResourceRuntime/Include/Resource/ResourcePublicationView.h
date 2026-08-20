#pragma once

/**
 * @file ResourcePublicationView.h
 * @brief Read-only sample-facing view of Resource-to-Render publications.
 */

#include "RenderContracts/IRenderResourceGateway.h"
#include "Resource/IResource.h"

namespace RVX::Resource
{
    /** @brief Structured result of a read-only resource-publication query. */
    enum class ResourcePublicationQueryCode : uint8
    {
        Resolved = 0,
        InvalidResourceId,
        UnsupportedResourceType,
        NotInitialized,
        WrongThread,
        GatewayUnavailable,
        NotFound,
        KindMismatch,
        StaleGeneration
    };

    /**
     * @brief Value snapshot of one Resource publication and its render status.
     *
     * The status is copied as a whole so newly appended public status fields,
     * such as a committed content revision, become visible without widening
     * this sample-facing interface.
     */
    struct ResourcePublicationQueryResult
    {
        ResourcePublicationQueryCode code =
            ResourcePublicationQueryCode::NotFound;
        ResourceId resourceId = InvalidResourceId;
        RenderResourceKind kind = RenderResourceKind::Invalid;
        RenderResourceHandle handle{};
        RenderResourceStatus status{};

        [[nodiscard]] bool IsResolved() const noexcept
        {
            return code == ResourcePublicationQueryCode::Resolved;
        }
    };

    /**
     * @brief Narrow, read-only query interface for Resource publications.
     *
     * Calls are restricted to the ResourceSubsystem update-owner thread: a
     * call from any other thread returns WrongThread without logging, mutation,
     * or synchronization side effects. The returned handle is the exact
     * generation currently mapped to the requested ResourceId.
     */
    class IResourcePublicationView
    {
    public:
        virtual ~IResourcePublicationView() = default;

        [[nodiscard]] virtual ResourcePublicationQueryResult
            QueryResourcePublication(ResourceId resourceId,
                                     ResourceType expectedType) const noexcept = 0;
    };
} // namespace RVX::Resource

namespace RVX
{
    using Resource::IResourcePublicationView;
    using Resource::ResourcePublicationQueryCode;
    using Resource::ResourcePublicationQueryResult;
} // namespace RVX
