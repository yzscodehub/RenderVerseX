#include "GPUSceneCulling.hlsli"

struct IndirectDrawIndexedCommand
{
    uint indexCount;
    uint instanceCount;
    uint firstIndex;
    int vertexOffset;
    uint firstInstance;
};

cbuffer CullingConstants : register(b0, space0)
{
    float4x4 ViewProj;
    float4 FrustumPlanes[6];
    float4 CameraPosition;
    float4 Params;
    uint4 Counts; // x=denseActiveCount, y=drawGroupCount, z=compactDispatchWidth
    uint4 GPUSceneTableCounts0; // primitive, bounds, transform, material
    uint4 GPUSceneTableCounts1; // geometry, draw, reserved, reserved
};

StructuredBuffer<GPUSceneCullingCandidate> gCandidates : register(t1, space0);
StructuredBuffer<GPUCullingActiveRow> gActiveRows : register(t2, space0);
RWStructuredBuffer<uint> gVisibility : register(u3, space0);
RWStructuredBuffer<uint> gVisibleInstanceIndices : register(u4, space0);
RWStructuredBuffer<IndirectDrawIndexedCommand> gIndirectDraws : register(u5, space0);
RWStructuredBuffer<uint> gDrawCount : register(u6, space0);
StructuredBuffer<GPUScenePrimitiveRow> gPrimitives : register(t7, space0);
StructuredBuffer<GPUSceneBoundsRow> gBounds : register(t8, space0);
StructuredBuffer<GPUSceneTransformRow> gTransforms : register(t9, space0);
StructuredBuffer<GPUSceneMaterialRow> gMaterials : register(t10, space0);
StructuredBuffer<GPUSceneGeometryRow> gGeometries : register(t11, space0);
StructuredBuffer<GPUSceneDrawMetadataRow> gDrawMetadata : register(t12, space0);

// Direct rendering owns the canonical frustum decision. Retain candidates
// within the same scale-relative margin as the non-resident GPU culling path
// so backend fused dot products cannot create GPU-only false negatives.
static const float RVX_FRUSTUM_REJECT_RELATIVE_TOLERANCE =
    16.0f * 1.1920928955078125e-7f;

float ConservativeFrustumRejectTolerance(float signedDistance,
                                         float projectedRadius)
{
    return RVX_FRUSTUM_REJECT_RELATIVE_TOLERANCE *
        max(1.0f, abs(signedDistance) + abs(projectedRadius));
}

bool AABBInsideFrustum(float3 center, float3 extent)
{
    [unroll]
    for (uint planeIndex = 0u; planeIndex < 6u; ++planeIndex)
    {
        const float4 plane = FrustumPlanes[planeIndex];
        if (dot(plane.xyz, plane.xyz) <= 1.0e-12f)
        {
            continue;
        }
        const float signedDistance = dot(plane.xyz, center) + plane.w;
        const float projectedRadius = dot(abs(plane.xyz), extent);
        if (signedDistance < -projectedRadius -
            ConservativeFrustumRejectTolerance(
                signedDistance, projectedRadius))
        {
            return false;
        }
    }
    return true;
}

bool LoadValidatedCandidate(
    uint activeIndex,
    out GPUCullingActiveRow active,
    out GPUSceneCullingCandidate candidate,
    out GPUSceneBoundsRow bounds,
    out GPUSceneDrawMetadataRow draw)
{
    active = gActiveRows[activeIndex];
    if (active.residentRow == 0xFFFFFFFFu ||
        active.drawGroupIndex >= Counts.y)
    {
        return false;
    }
    candidate = gCandidates[active.residentRow];
    if (candidate.rasterInstanceIndex != active.residentRow ||
        !GPUSceneCandidateSlotsInRange(
            candidate,
            GPUSceneTableCounts0.x,
            GPUSceneTableCounts0.y,
            GPUSceneTableCounts0.z,
            GPUSceneTableCounts0.w,
            GPUSceneTableCounts1.x,
            GPUSceneTableCounts1.y))
    {
        return false;
    }

    const GPUScenePrimitiveRow primitive = gPrimitives[candidate.primitiveSlot];
    if (!GPUSceneIsValidRef(primitive.bounds) || !GPUSceneIsValidRef(primitive.transform) ||
        !GPUSceneIsValidRef(primitive.firstDraw) ||
        primitive.bounds.x >= GPUSceneTableCounts0.y ||
        primitive.transform.x >= GPUSceneTableCounts0.z ||
        primitive.firstDraw.x >= GPUSceneTableCounts1.y ||
        primitive.drawCount == 0u ||
        primitive.drawCount > GPUSceneTableCounts1.y - primitive.firstDraw.x)
    {
        return false;
    }

    draw = gDrawMetadata[candidate.drawSlot];
    if (!GPUSceneIsValidRef(draw.material) || !GPUSceneIsValidRef(draw.geometry) ||
        draw.material.x >= GPUSceneTableCounts0.w ||
        draw.geometry.x >= GPUSceneTableCounts1.x)
    {
        return false;
    }

    bounds = gBounds[primitive.bounds.x];
    const GPUSceneTransformRow transform = gTransforms[primitive.transform.x];
    const GPUSceneMaterialRow material = gMaterials[draw.material.x];
    const GPUSceneGeometryRow geometry = gGeometries[draw.geometry.x];
    return GPUSceneValidateCandidateRows(
        candidate, primitive, bounds, transform, material, geometry, draw);
}

[numthreads(64, 1, 1)]
void CSGPUSceneFrustumCull(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint activeIndex = dispatchThreadId.x;
    const uint candidateCount = Counts.x;
    const uint drawGroupCount = Counts.y;
    if (activeIndex <= drawGroupCount)
    {
        gDrawCount[activeIndex] = 0u;
    }
    if (activeIndex >= candidateCount)
    {
        return;
    }

    gVisibility[activeIndex] = 0u;
    GPUCullingActiveRow active;
    GPUSceneCullingCandidate candidate;
    GPUSceneBoundsRow bounds;
    GPUSceneDrawMetadataRow draw;
    if (!LoadValidatedCandidate(activeIndex, active, candidate, bounds, draw))
    {
        return;
    }

    bool visible = GPUSceneBoundsForceVisible(bounds) ||
        Params.z <= 0.5f ||
        AABBInsideFrustum(
            (bounds.minimum.xyz + bounds.maximum.xyz) * 0.5f,
            max((bounds.maximum.xyz - bounds.minimum.xyz) * 0.5f, 0.0f));
    if (visible && !GPUSceneBoundsForceVisible(bounds) && Params.w > 0.5f &&
        Params.x > 0.0f)
    {
        visible = (length(bounds.sphere.xyz - CameraPosition.xyz) -
                   max(bounds.sphere.w, 0.0f)) <= Params.x;
    }
    gVisibility[activeIndex] = visible ? 1u : 0u;
}

groupshared uint gGPUSceneCompactScan[64];
groupshared uint gGPUSceneCompactVisibleBase;

[numthreads(64, 1, 1)]
void CSGPUSceneCompactDraws(uint3 groupId : SV_GroupID,
                            uint groupThreadIndex : SV_GroupIndex)
{
    const uint drawGroupIndex = groupId.x + groupId.y * Counts.z;
    if (drawGroupIndex >= Counts.y)
    {
        return;
    }

    // firstInstance and the pre-finalize instanceCount are the current dense
    // dispatch range. The resident candidate lookup remains deliberately
    // sparse through GPUCullingActiveRow.residentRow.
    const IndirectDrawIndexedCommand command =
        gIndirectDraws[drawGroupIndex];
    const uint activeRangeStart = min(command.firstInstance, Counts.x);
    const uint activeRangeCount = min(
        command.instanceCount, Counts.x - activeRangeStart);
    if (groupThreadIndex == 0u)
    {
        gGPUSceneCompactVisibleBase = 0u;
    }
    GroupMemoryBarrierWithGroupSync();

    for (uint blockStart = 0u;
         blockStart < activeRangeCount;
         blockStart += 64u)
    {
        const uint activeIndex =
            activeRangeStart + blockStart + groupThreadIndex;
        uint visible = 0u;
        uint residentRow = 0xFFFFFFFFu;
        if (groupThreadIndex < activeRangeCount - blockStart &&
            gVisibility[activeIndex] != 0u)
        {
            GPUCullingActiveRow active;
            GPUSceneCullingCandidate candidate;
            GPUSceneBoundsRow bounds;
            GPUSceneDrawMetadataRow draw;
            if (LoadValidatedCandidate(
                    activeIndex, active, candidate, bounds, draw) &&
                active.drawGroupIndex == drawGroupIndex)
            {
                visible = 1u;
                residentRow = active.residentRow;
            }
        }

        gGPUSceneCompactScan[groupThreadIndex] = visible;
        GroupMemoryBarrierWithGroupSync();
        [unroll]
        for (uint stride = 1u; stride < 64u; stride <<= 1u)
        {
            const uint prior = groupThreadIndex >= stride
                ? gGPUSceneCompactScan[groupThreadIndex - stride]
                : 0u;
            GroupMemoryBarrierWithGroupSync();
            gGPUSceneCompactScan[groupThreadIndex] += prior;
            GroupMemoryBarrierWithGroupSync();
        }

        if (visible != 0u)
        {
            const uint exclusiveIndex =
                gGPUSceneCompactScan[groupThreadIndex] - 1u;
            gVisibleInstanceIndices[
                command.firstInstance + gGPUSceneCompactVisibleBase +
                    exclusiveIndex] = residentRow;
        }
        GroupMemoryBarrierWithGroupSync();
        if (groupThreadIndex == 0u)
        {
            gGPUSceneCompactVisibleBase += gGPUSceneCompactScan[63];
        }
        GroupMemoryBarrierWithGroupSync();
    }

    if (groupThreadIndex == 0u)
    {
        gDrawCount[drawGroupIndex + 1u] = gGPUSceneCompactVisibleBase;
    }
}

[numthreads(64, 1, 1)]
void CSGPUSceneFinalizeDrawGroups(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint drawGroupIndex = dispatchThreadId.x;
    if (drawGroupIndex >= Counts.y)
    {
        return;
    }

    const uint visibleCount = gDrawCount[drawGroupIndex + 1u];
    IndirectDrawIndexedCommand command = gIndirectDraws[drawGroupIndex];
    command.instanceCount = visibleCount;
    gIndirectDraws[drawGroupIndex] = command;
    gDrawCount[drawGroupIndex + 1u] = visibleCount > 0u ? 1u : 0u;
    if (visibleCount > 0u)
    {
        uint ignored = 0u;
        InterlockedAdd(gDrawCount[0], 1u, ignored);
    }
}
