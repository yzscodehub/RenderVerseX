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
    uint4 Counts; // x=instanceCount, y=drawGroupCount
    uint4 GPUSceneTableCounts0; // primitive, bounds, transform, material
    uint4 GPUSceneTableCounts1; // geometry, draw, reserved, reserved
};

StructuredBuffer<GPUInstanceData> gInstances : register(t1, space0);
RWStructuredBuffer<uint> gVisibility : register(u2, space0);
RWStructuredBuffer<uint> gVisibleInstanceIndices : register(u3, space0);
RWStructuredBuffer<IndirectDrawIndexedCommand> gIndirectDraws : register(u4, space0);
RWStructuredBuffer<uint> gDrawCount : register(u5, space0);

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
        if (distanceToPlane < -projectedRadius)
        {
            return false;
        }
    }

    return true;
}

IndirectDrawIndexedCommand EmptyCommand()
{
    IndirectDrawIndexedCommand command;
    command.indexCount = 0;
    command.instanceCount = 0;
    command.firstIndex = 0;
    command.vertexOffset = 0;
    command.firstInstance = 0;
    return command;
}

[numthreads(64, 1, 1)]
void CSFrustumCull(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint instanceIndex = dispatchThreadId.x;
    uint instanceCount = Counts.x;
    uint drawGroupCount = Counts.y;
    if (instanceIndex <= drawGroupCount)
    {
        gDrawCount[instanceIndex] = 0;
    }

    if (instanceIndex >= instanceCount)
    {
        return;
    }

    GPUInstanceData instance = gInstances[instanceIndex];
    float3 center = instance.boundingSphere.xyz;
    float radius = max(instance.boundingSphere.w, 0.0f);
    float3 extent = max((instance.aabbMax.xyz - instance.aabbMin.xyz) * 0.5f, 0.0f);

    gVisibility[instanceIndex] = 0;
    gIndirectDraws[instanceIndex] = EmptyCommand();

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

    gVisibility[instanceIndex] = visible ? 1u : 0u;
}

[numthreads(64, 1, 1)]
void CSCompactDraws(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint instanceIndex = dispatchThreadId.x;
    uint instanceCount = Counts.x;
    if (instanceIndex >= instanceCount || gVisibility[instanceIndex] == 0)
    {
        return;
    }

    GPUInstanceData instance = gInstances[instanceIndex];
    uint totalDrawIndex = 0;
    InterlockedAdd(gDrawCount[0], 1, totalDrawIndex);

    uint groupDrawIndex = 0;
    InterlockedAdd(gDrawCount[instance.drawGroupIndex + 1], 1, groupDrawIndex);

    uint commandIndex = instance.drawGroupCommandOffset + groupDrawIndex;
    gVisibleInstanceIndices[totalDrawIndex] = instanceIndex;

    IndirectDrawIndexedCommand command;
    command.indexCount = instance.indexCount;
    command.instanceCount = 1;
    command.firstIndex = instance.firstIndex;
    command.vertexOffset = instance.vertexOffset;
    command.firstInstance = instanceIndex;
    gIndirectDraws[commandIndex] = command;
}
