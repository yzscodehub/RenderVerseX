RaytracingAccelerationStructure SceneAccelerationStructure : register(t0, space0);
RWStructuredBuffer<uint> DispatchResult : register(u1, space0);

struct RayPayload
{
    uint value;
};

struct CallablePayload
{
    uint value;
};

struct ProceduralAttributes
{
    float2 barycentrics;
};

static const uint RVX_VULKAN_RT_HIT_SENTINEL = 0xC105E57u;
static const uint RVX_VULKAN_RT_MISS_SENTINEL = 0xBADC0DEu;

[shader("raygeneration")]
void RayGenerationMain()
{
    RayDesc ray;
    ray.Origin = float3(0.0f, 0.0f, -1.0f);
    ray.Direction = float3(0.0f, 0.0f, 1.0f);
    ray.TMin = 0.0f;
    ray.TMax = 100.0f;

    RayPayload payload = {0u};
    TraceRay(
        SceneAccelerationStructure,
        RAY_FLAG_NONE,
        0xFFu,
        0u,
        0u,
        0u,
        ray,
        payload);
    DispatchResult[DispatchRaysIndex().x] = payload.value;
}

[shader("miss")]
void MissMain(inout RayPayload payload)
{
    payload.value = RVX_VULKAN_RT_MISS_SENTINEL;
}

[shader("closesthit")]
void ClosestHitMain(
    inout RayPayload payload,
    in BuiltInTriangleIntersectionAttributes attributes)
{
    payload.value = RVX_VULKAN_RT_HIT_SENTINEL +
        (attributes.barycentrics.x < -1.0f ? 1u : 0u);
}

[shader("anyhit")]
void AnyHitMain(
    inout RayPayload payload,
    in BuiltInTriangleIntersectionAttributes attributes)
{
    payload.value += attributes.barycentrics.x < -1.0f ? 1u : 0u;
}

[shader("intersection")]
void IntersectionMain()
{
    ProceduralAttributes attributes = {float2(0.25f, 0.25f)};
    ReportHit(1.0f, 0u, attributes);
}

[shader("callable")]
void CallableMain(inout CallablePayload payload)
{
    payload.value ^= 0x5A5A5A5Au;
}
