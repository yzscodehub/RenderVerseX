#pragma once

/**
 * @file RasterInstanceStream.h
 * @brief Shared raster-instance ABI and frame-local buffer materialization.
 */

#include "Core/MathTypes.h"
#include "Core/Types.h"
#include "RenderContracts/RenderIdentity.h"
#include "RHI/RHI.h"

#include <cstddef>
#include <span>
#include <vector>

namespace RVX
{
    struct DirectDrawPacketBatch;
    struct RenderInstanceBatchPlan;
    class RenderScene;
    /** @brief Raster transform/identity data shared by Direct and GPU-driven paths. */
    struct alignas(16) GPUInstanceData
    {
        Mat4 worldMatrix;
        Mat4 normalMatrix;
        Vec4 boundingSphere;
        Vec4 aabbMin;
        Vec4 aabbMax;
        uint32 meshId;
        uint32 materialId;
        uint32 indexCount;
        uint32 firstIndex;
        int32 vertexOffset;
        uint32 sourceIndex = RVX_INVALID_INDEX;
        uint32 drawGroupIndex;
        uint32 drawGroupVisibleOffset;
        uint32 candidateIndex = RVX_INVALID_INDEX;
        uint32 forceVisible = 0;
        uint32 padding[2] = {};
    };

    static_assert(sizeof(GPUInstanceData) == 224,
                  "GPUInstanceData must match GPUInstanceData.hlsli");
    static_assert(alignof(GPUInstanceData) == 16);
    static_assert(offsetof(GPUInstanceData, worldMatrix) == 0);
    static_assert(offsetof(GPUInstanceData, normalMatrix) == 64);
    static_assert(offsetof(GPUInstanceData, boundingSphere) == 128);
    static_assert(offsetof(GPUInstanceData, aabbMin) == 144);
    static_assert(offsetof(GPUInstanceData, aabbMax) == 160);
    static_assert(offsetof(GPUInstanceData, meshId) == 176);
    static_assert(offsetof(GPUInstanceData, materialId) == 180);
    static_assert(offsetof(GPUInstanceData, indexCount) == 184);
    static_assert(offsetof(GPUInstanceData, firstIndex) == 188);
    static_assert(offsetof(GPUInstanceData, vertexOffset) == 192);
    static_assert(offsetof(GPUInstanceData, sourceIndex) == 196);
    static_assert(offsetof(GPUInstanceData, drawGroupIndex) == 200);
    static_assert(offsetof(GPUInstanceData, drawGroupVisibleOffset) == 204);
    static_assert(offsetof(GPUInstanceData, candidateIndex) == 208);
    static_assert(offsetof(GPUInstanceData, forceVisible) == 212);
    static_assert(offsetof(GPUInstanceData, padding) == 216);

    /** @brief RHI-backed immutable stream retained for one graph recording. */
    struct RasterInstanceStream
    {
        RHIBufferRef instances;
        RHIBufferRef instanceIndices;
        uint32 instanceCount = 0;

        [[nodiscard]] bool IsValid() const noexcept
        {
            return instances && instanceIndices && instanceCount != 0;
        }
    };

    /** @brief Upload a complete identity-indexed raster stream. */
    [[nodiscard]] bool CreateRasterInstanceStream(
        IRHIDevice& device,
        std::span<const GPUInstanceData> instances,
        const char* debugName,
        RasterInstanceStream& outStream);

    /** @brief Resolve one immutable CPU instance array from a sealed batch plan. */
    [[nodiscard]] bool BuildRasterInstanceData(
        const RenderInstanceBatchPlan& plan,
        const DirectDrawPacketBatch& directBatch,
        const RenderScene& scene,
        std::vector<GPUInstanceData>& outInstances);

    /** @brief Shared identity stream creator used by GPUCulling frame owners. */
    [[nodiscard]] RHIBufferRef CreateRasterInstanceIndexBuffer(
        IRHIDevice& device,
        uint32 instanceCapacity,
        const char* debugName);
} // namespace RVX
