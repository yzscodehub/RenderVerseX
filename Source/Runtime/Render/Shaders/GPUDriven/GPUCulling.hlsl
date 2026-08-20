#include "../Include/GPUInstanceData.hlsli"

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
    float4 Params; // x=maxDistance, y=reserved, z=frustumEnabled, w=distanceEnabled
    uint4 Counts; // x=denseActiveCount, y=drawGroupCount, z=compactDispatchWidth
    uint4 GPUSceneTableCounts0; // primitive, bounds, transform, material
    uint4 GPUSceneTableCounts1; // geometry, draw, reserved, reserved
};

StructuredBuffer<GPUInstanceData> gInstances : register(t1, space0);
struct GPUCullingActiveRow
{
    uint residentRow;
    uint drawGroupIndex;
    uint drawGroupVisibleOffset;
    uint padding0;
};

StructuredBuffer<GPUCullingActiveRow> gActiveRows : register(t2, space0);
RWStructuredBuffer<uint> gVisibility : register(u3, space0);
RWStructuredBuffer<uint> gVisibleInstanceIndices : register(u4, space0);
RWStructuredBuffer<IndirectDrawIndexedCommand> gIndirectDraws : register(u5, space0);
RWStructuredBuffer<uint> gDrawCount : register(u6, space0);

// Direct rendering owns the canonical frustum decision. Retain candidates
// within a tiny scale-relative margin so a backend-specific fused dot product
// cannot turn a Direct-visible boundary AABB into a GPU false negative.
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
    for (uint planeIndex = 0; planeIndex < 6; ++planeIndex)
    {
        float4 plane = FrustumPlanes[planeIndex];
        if (dot(plane.xyz, plane.xyz) <= 1.0e-12f)
        {
            continue;
        }
        float distanceToPlane = dot(plane.xyz, center) + plane.w;
        float projectedRadius = dot(abs(plane.xyz), extent);
        if (distanceToPlane < -projectedRadius -
            ConservativeFrustumRejectTolerance(
                distanceToPlane, projectedRadius))
        {
            return false;
        }
    }

    return true;
}

[numthreads(64, 1, 1)]
void CSFrustumCull(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint activeIndex = dispatchThreadId.x;
    uint instanceCount = Counts.x;
    uint drawGroupCount = Counts.y;
    if (activeIndex <= drawGroupCount)
    {
        gDrawCount[activeIndex] = 0;
    }

    if (activeIndex >= instanceCount)
    {
        return;
    }

    GPUCullingActiveRow active = gActiveRows[activeIndex];
    if (active.residentRow == 0xFFFFFFFFu ||
        active.drawGroupIndex >= drawGroupCount)
    {
        gVisibility[activeIndex] = 0u;
        return;
    }
    GPUInstanceData instance = gInstances[active.residentRow];
    float3 center = instance.boundingSphere.xyz;
    float radius = max(instance.boundingSphere.w, 0.0f);
    float3 extent = max((instance.aabbMax.xyz - instance.aabbMin.xyz) * 0.5f, 0.0f);

    gVisibility[activeIndex] = 0;

    if (instance.indexCount == 0)
    {
        return;
    }

    bool visible = true;
    if (instance.forceVisible == 0 && Params.z > 0.5f)
    {
        visible = AABBInsideFrustum(center, extent);
    }

    if (visible && instance.forceVisible == 0 && Params.w > 0.5f && Params.x > 0.0f)
    {
        float distanceToCamera = length(center - CameraPosition.xyz);
        visible = (distanceToCamera - radius) <= Params.x;
    }

    gVisibility[activeIndex] = visible ? 1u : 0u;
}

groupshared uint gCompactScan[64];
groupshared uint gCompactVisibleBase;

[numthreads(64, 1, 1)]
void CSCompactDraws(uint3 groupId : SV_GroupID,
                    uint groupThreadIndex : SV_GroupIndex)
{
    // One workgroup owns one deterministic packet/draw-group range. Counts.z
    // linearizes the 2D dispatch so 65,536+ groups do not depend on a native
    // one-dimensional dispatch limit.
    const uint drawGroupIndex = groupId.x + groupId.y * Counts.z;
    if (drawGroupIndex >= Counts.y)
    {
        return;
    }

    // CPU prefill stores immutable draw metadata and the dense active-range
    // start in firstInstance. CSFinalizeDrawGroups overwrites instanceCount
    // only after this shader has consumed it as the range length.
    const IndirectDrawIndexedCommand command =
        gIndirectDraws[drawGroupIndex];
    const uint activeRangeStart = min(command.firstInstance, Counts.x);
    const uint activeRangeCount = min(
        command.instanceCount, Counts.x - activeRangeStart);

    if (groupThreadIndex == 0u)
    {
        gCompactVisibleBase = 0u;
    }
    GroupMemoryBarrierWithGroupSync();

    // A large group is processed by deterministic 64-entry blocks in one
    // workgroup. This intentionally trades a single group's throughput for a
    // stable visible-row order without global compact atomics.
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
            const GPUCullingActiveRow active = gActiveRows[activeIndex];
            if (active.residentRow != 0xFFFFFFFFu &&
                active.drawGroupIndex == drawGroupIndex)
            {
                visible = 1u;
                residentRow = active.residentRow;
            }
        }

        gCompactScan[groupThreadIndex] = visible;
        GroupMemoryBarrierWithGroupSync();
        [unroll]
        for (uint stride = 1u; stride < 64u; stride <<= 1u)
        {
            const uint prior = groupThreadIndex >= stride
                ? gCompactScan[groupThreadIndex - stride]
                : 0u;
            GroupMemoryBarrierWithGroupSync();
            gCompactScan[groupThreadIndex] += prior;
            GroupMemoryBarrierWithGroupSync();
        }

        if (visible != 0u)
        {
            const uint exclusiveIndex =
                gCompactScan[groupThreadIndex] - 1u;
            gVisibleInstanceIndices[
                command.firstInstance + gCompactVisibleBase + exclusiveIndex] =
                residentRow;
        }
        GroupMemoryBarrierWithGroupSync();
        if (groupThreadIndex == 0u)
        {
            gCompactVisibleBase += gCompactScan[63];
        }
        GroupMemoryBarrierWithGroupSync();
    }

    if (groupThreadIndex == 0u)
    {
        gDrawCount[drawGroupIndex + 1u] = gCompactVisibleBase;
    }
}

[numthreads(64, 1, 1)]
void CSFinalizeDrawGroups(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint drawGroupIndex = dispatchThreadId.x;
    if (drawGroupIndex >= Counts.y)
    {
        return;
    }

    const uint visibleCount = gDrawCount[drawGroupIndex + 1];
    IndirectDrawIndexedCommand command = gIndirectDraws[drawGroupIndex];
    command.instanceCount = visibleCount;
    gIndirectDraws[drawGroupIndex] = command;
    gDrawCount[drawGroupIndex + 1] = visibleCount > 0 ? 1u : 0u;
    if (visibleCount > 0)
    {
        uint ignored = 0;
        InterlockedAdd(gDrawCount[0], 1, ignored);
    }
}
