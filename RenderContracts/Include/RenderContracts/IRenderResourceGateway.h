#pragma once

/**
 * @file IRenderResourceGateway.h
 * @brief Public update-side gateway and render-resource status contract.
 */

#include "RenderContracts/RenderIdentity.h"
#include "RenderContracts/ResourceUploadRequest.h"

namespace RVX
{
    enum class RenderResourcePublicState : uint8
    {
        Released = 0,
        Reserved = 1,
        UploadQueued = 2,
        Uploading = 3,
        GPUReady = 4,
        Failed = 5,
        Evicting = 6,
        /** Internal status-table state; gateway exposes GPUReady + Queued. */
        ReplacementQueued = 7,
        /** Internal status-table state; gateway exposes GPUReady + Uploading. */
        Replacing = 8
    };

    /**
     * @brief Orthogonal progress for a replacement whose prior committed
     * content remains publicly GPU-ready.
     */
    enum class RenderResourceReplacementState : uint8
    {
        None = 0,
        Queued = 1,
        Uploading = 2
    };

    enum class RenderResourceFailureCode : uint16
    {
        None = 0,
        InvalidPayload = 1,
        DependencyUnavailable = 2,
        ResourceCreationFailed = 3,
        UploadSubmissionFailed = 4,
        DeviceLost = 5,
        Cancelled = 6,
        RuntimeFailure = 7
    };

    enum class RenderResourceReserveCode : uint8
    {
        Reserved = 0,
        Existing = 1,
        InvalidAsset = 2,
        KindMismatch = 3,
        CapacityExceeded = 4,
        ShuttingDown = 5
    };

    enum class RenderUploadEnqueueCode : uint8
    {
        Accepted = 0,
        QueueFullByCount = 1,
        QueueFullByBytes = 2,
        InvalidRequest = 3,
        StaleGeneration = 4,
        Cancelled = 5,
        ShuttingDown = 6
    };

    enum class RenderReleaseCode : uint8
    {
        Accepted = 0,
        StaleGeneration = 1,
        AlreadyPending = 2,
        ShuttingDown = 3
    };

    enum class RenderResourceStatusCode : uint8
    {
        Current = 0,
        InvalidHandle = 1,
        StaleGeneration = 2
    };

    struct RenderResourceStatus
    {
        RenderResourceStatusCode code = RenderResourceStatusCode::InvalidHandle;
        RenderResourcePublicState state = RenderResourcePublicState::Released;
        /**
         * A GPUReady state may retain a replacement failure while its prior
         * committed content remains usable. Failed means no usable content.
         */
        RenderResourceFailureCode failure = RenderResourceFailureCode::None;
        /**
         * Replacement activity is orthogonal to public availability. Queued
         * and Uploading both retain the prior committed GPU-ready content.
         */
        RenderResourceReplacementState replacementState =
            RenderResourceReplacementState::None;
        /** Zero when no replacement is currently accepted for this handle. */
        uint64 pendingSourceRevision = 0;
    };

    struct RenderResourceReserveResult
    {
        RenderResourceReserveCode code = RenderResourceReserveCode::InvalidAsset;
        RenderResourceHandle handle;
        RenderResourceStatus status;
    };

    struct RenderUploadEnqueueResult
    {
        RenderUploadEnqueueCode code = RenderUploadEnqueueCode::InvalidRequest;
    };

    struct RenderReleaseResult
    {
        RenderReleaseCode code = RenderReleaseCode::StaleGeneration;
    };

    class IRenderResourceGateway
    {
    public:
        virtual ~IRenderResourceGateway() = default;
        virtual RenderResourceReserveResult ReserveResource(
            AssetId assetId,
            RenderResourceKind kind) noexcept = 0;
        virtual RenderUploadEnqueueResult TryEnqueueUpload(
            const ResourceUploadRequestRef& request) noexcept = 0;
        virtual RenderReleaseResult RequestRelease(
            RenderResourceHandle handle) noexcept = 0;
        /**
         * @brief Query update-side availability. Internal replacement progress
         * is reported through RenderResourceStatus::replacementState while the
         * last committed content remains GPUReady.
         */
        [[nodiscard]] virtual RenderResourceStatus QueryResourceStatus(
            RenderResourceHandle handle) const noexcept = 0;
    };
} // namespace RVX
