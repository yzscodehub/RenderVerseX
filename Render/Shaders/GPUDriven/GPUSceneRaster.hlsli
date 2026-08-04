#ifndef RVX_GPU_SCENE_RASTER_HLSLI
#define RVX_GPU_SCENE_RASTER_HLSLI

// Raster-only GPU-scene bindings.  This include is selected only by the
// VSMainGPUScene permutations, so the normal Direct/Tier1 object set remains
// byte-for-byte independent at b0/t1.
#include "GPUSceneCulling.hlsli"

#if defined(RVX_GPU_SCENE_RASTER)

cbuffer GPUSceneRasterObjectConstants : register(b0, space1)
{
    // Keep the complete ObjectConstants prefix ABI consumed by DefaultLit's
    // pixel shader.  GPU-scene raster owns a separate buffer/descriptor, but
    // its b0 is deliberately compatible with the normal material contract.
    float4x4 World;
    float4x4 NormalMatrix;
    float4x4 PreviousWorldViewProjection;
    float4 ObjectVelocityParams;
    float4 SkinningParams;
    float4x4 SkinningMatrices[RVX_MAX_OBJECT_SKINNING_MATRICES];

    // x: populated candidate rows, y: primitive capacity, z: transform capacity.
    uint4 GPUSceneRasterCounts;
};

StructuredBuffer<GPUSceneCullingCandidate> GPUSceneRasterCandidates : register(t1, space1);
StructuredBuffer<GPUScenePrimitiveRow> GPUSceneRasterPrimitives : register(t2, space1);
StructuredBuffer<GPUSceneTransformRow> GPUSceneRasterTransforms : register(t3, space1);

#define RVX_GPU_SCENE_PRIMITIVE_RECEIVES_SHADOW (1u << 2u)

// Resolves exactly the stable references that raster needs.  Every table load
// follows an explicit range check; a mismatched generation or tombstoned row
// is rejected before its transform payload can affect rasterization.
bool GPUSceneResolveRasterTransform(
    uint rasterInstanceIndex,
    out GPUSceneTransformRow transform,
    out uint primitiveFlags)
{
    primitiveFlags = 0u;
    if (GPUSceneRasterCounts.x == 0u ||
        GPUSceneRasterCounts.y == 0u ||
        GPUSceneRasterCounts.z == 0u ||
        rasterInstanceIndex >= GPUSceneRasterCounts.x)
    {
        return false;
    }

    const GPUSceneCullingCandidate candidate =
        GPUSceneRasterCandidates[rasterInstanceIndex];
    const uint2 objectId = uint2(candidate.objectIdLow, candidate.objectIdHigh);
    const uint2 primitiveRef =
        uint2(candidate.primitiveSlot, candidate.primitiveGeneration);
    if (candidate.rasterInstanceIndex != rasterInstanceIndex ||
        (objectId.x == 0u && objectId.y == 0u) ||
        candidate.requiredPassMask == 0u ||
        (candidate.requiredPassMask & (candidate.requiredPassMask - 1u)) != 0u ||
        !GPUSceneIsValidRef(primitiveRef) ||
        candidate.primitiveSlot >= GPUSceneRasterCounts.y)
    {
        return false;
    }

    const GPUScenePrimitiveRow primitive =
        GPUSceneRasterPrimitives[candidate.primitiveSlot];
    if (!GPUSceneIsLiveHeader(primitive.header, primitiveRef.y, objectId) ||
        !GPUSceneIsValidRef(primitive.transform) ||
        primitive.transform.x >= GPUSceneRasterCounts.z)
    {
        return false;
    }

    primitiveFlags = primitive.primitiveFlags;
    transform = GPUSceneRasterTransforms[primitive.transform.x];
    return GPUSceneIsLiveHeader(transform.header, primitive.transform.y, objectId) &&
        (transform.transformFlags & RVX_GPU_SCENE_TRANSFORM_FLAG_NORMAL_VALID) != 0u;
}

// GPU-scene affine rows are explicitly row-major 3x4.  Use row dot-products
// instead of an HLSL matrix load so the calculation cannot inherit a compiler
// storage convention.
float4 GPUSceneTransformPosition(GPUSceneTransformRow transform, float3 position)
{
    const float4 localPosition = float4(position, 1.0f);
    return float4(
        dot(transform.worldFromLocal[0], localPosition),
        dot(transform.worldFromLocal[1], localPosition),
        dot(transform.worldFromLocal[2], localPosition),
        1.0f);
}

float3 GPUSceneTransformNormal(GPUSceneTransformRow transform, float3 normal)
{
    return float3(
        dot(transform.normalFromLocal[0].xyz, normal),
        dot(transform.normalFromLocal[1].xyz, normal),
        dot(transform.normalFromLocal[2].xyz, normal));
}

float3 GPUSceneTransformTangent(GPUSceneTransformRow transform, float3 tangent)
{
    return float3(
        dot(transform.worldFromLocal[0].xyz, tangent),
        dot(transform.worldFromLocal[1].xyz, tangent),
        dot(transform.worldFromLocal[2].xyz, tangent));
}

float3 GPUSceneSafeNormalize(float3 value, float3 fallback)
{
    const float lengthSquared = dot(value, value);
    return lengthSquared > 1.0e-8f ? value * rsqrt(lengthSquared) : fallback;
}

// All invalid vertices collapse at a single point outside clip space.  This is
// the vertex-stage fail-closed equivalent of rejecting a stale scene row.
float4 GPUSceneInvalidClipPosition()
{
    return float4(2.0f, 2.0f, 2.0f, 1.0f);
}

#endif // defined(RVX_GPU_SCENE_RASTER)

#endif // RVX_GPU_SCENE_RASTER_HLSLI
