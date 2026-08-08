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
    uint4 Counts; // x=candidateCount, y=drawGroupCount
    uint4 GPUSceneTableCounts0; // primitive, bounds, transform, material
    uint4 GPUSceneTableCounts1; // geometry, draw, reserved, reserved
};

StructuredBuffer<GPUSceneCullingCandidate> gCandidates : register(t1, space0);
RWStructuredBuffer<uint> gVisibility : register(u2, space0);
RWStructuredBuffer<uint> gVisibleInstanceIndices : register(u3, space0);
RWStructuredBuffer<IndirectDrawIndexedCommand> gIndirectDraws : register(u4, space0);
RWStructuredBuffer<uint> gDrawCount : register(u5, space0);
StructuredBuffer<GPUScenePrimitiveRow> gPrimitives : register(t6, space0);
StructuredBuffer<GPUSceneBoundsRow> gBounds : register(t7, space0);
StructuredBuffer<GPUSceneTransformRow> gTransforms : register(t8, space0);
StructuredBuffer<GPUSceneMaterialRow> gMaterials : register(t9, space0);
StructuredBuffer<GPUSceneGeometryRow> gGeometries : register(t10, space0);
StructuredBuffer<GPUSceneDrawMetadataRow> gDrawMetadata : register(t11, space0);

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
        if (dot(plane.xyz, center) + plane.w <
            -dot(abs(plane.xyz), extent))
        {
            return false;
        }
    }
    return true;
}

bool LoadValidatedCandidate(
    uint candidateIndex,
    out GPUSceneCullingCandidate candidate,
    out GPUSceneBoundsRow bounds,
    out GPUSceneDrawMetadataRow draw)
{
    candidate = gCandidates[candidateIndex];
    if (candidate.rasterInstanceIndex != candidateIndex ||
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
    const uint candidateIndex = dispatchThreadId.x;
    const uint candidateCount = Counts.x;
    const uint drawGroupCount = Counts.y;
    if (candidateIndex <= drawGroupCount)
    {
        gDrawCount[candidateIndex] = 0u;
    }
    if (candidateIndex >= candidateCount)
    {
        return;
    }

    gVisibility[candidateIndex] = 0u;
    GPUSceneCullingCandidate candidate;
    GPUSceneBoundsRow bounds;
    GPUSceneDrawMetadataRow draw;
    if (!LoadValidatedCandidate(candidateIndex, candidate, bounds, draw) ||
        candidate.drawGroupIndex >= drawGroupCount)
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
    gVisibility[candidateIndex] = visible ? 1u : 0u;
}

[numthreads(64, 1, 1)]
void CSGPUSceneCompactDraws(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint candidateIndex = dispatchThreadId.x;
    if (candidateIndex >= Counts.x || gVisibility[candidateIndex] == 0u)
    {
        return;
    }

    GPUSceneCullingCandidate candidate;
    GPUSceneBoundsRow bounds;
    GPUSceneDrawMetadataRow draw;
    if (!LoadValidatedCandidate(candidateIndex, candidate, bounds, draw) ||
        candidate.drawGroupIndex >= Counts.y)
    {
        return;
    }

    uint groupVisibleIndex = 0u;
    InterlockedAdd(
        gDrawCount[candidate.drawGroupIndex + 1u], 1u, groupVisibleIndex);
    gVisibleInstanceIndices[
        candidate.drawGroupVisibleOffset + groupVisibleIndex] =
            candidate.rasterInstanceIndex;
    if (groupVisibleIndex == 0u)
    {
        IndirectDrawIndexedCommand command;
        command.indexCount = draw.indexCount;
        command.instanceCount = 0u;
        command.firstIndex = draw.firstIndex;
        command.vertexOffset = draw.vertexOffset;
        command.firstInstance = candidate.drawGroupVisibleOffset;
        gIndirectDraws[candidate.drawGroupIndex] = command;
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
