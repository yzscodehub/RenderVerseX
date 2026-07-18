#pragma once

/**
 * @file RenderUploadRequestBuilder.h
 * @brief Copies concrete CPU resources into immutable owned render uploads.
 */

#include "RenderContracts/ResourceUploadRequest.h"
#include "Resource/IResource.h"

#include <functional>

namespace RVX::Resource
{
    /** @brief Stable result for update-side upload request construction. */
    enum class RenderUploadRequestBuildCode : uint8
    {
        Built = 0,
        UnsupportedResource = 1,
        InvalidResource = 2,
        DependencyUnavailable = 3,
        PayloadOverflow = 4,
        RequestRejected = 5
    };

    using RenderResourceDependencyResolver =
        std::function<RenderResourceHandle(AssetId, RenderResourceKind)>;

    struct RenderUploadRequestBuildResult
    {
        RenderUploadRequestBuildCode code =
            RenderUploadRequestBuildCode::InvalidResource;
        ResourceUploadRequestCreateCode requestCode =
            ResourceUploadRequestCreateCode::InvalidPayload;
        ResourceUploadRequestRef request;
    };

    /**
     * @brief Builds pointer-free requests whose vectors own every payload byte.
     */
    class RenderUploadRequestBuilder final
    {
    public:
        // =====================================================================
        // Request Construction
        // =====================================================================
        static RenderUploadRequestBuildResult Build(
            const IResource& resource,
            RenderResourceHandle handle,
            uint64 sequence,
            const RenderResourceDependencyResolver& dependencyResolver,
            RenderUploadPriority priority = RenderUploadPriority::Normal,
            uint64 sourceRevision = 0);
    };
} // namespace RVX::Resource

namespace RVX
{
    using Resource::RenderResourceDependencyResolver;
    using Resource::RenderUploadRequestBuildCode;
    using Resource::RenderUploadRequestBuildResult;
    using Resource::RenderUploadRequestBuilder;
} // namespace RVX
