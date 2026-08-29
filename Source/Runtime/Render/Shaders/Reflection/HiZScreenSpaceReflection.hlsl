// =============================================================================
// HiZScreenSpaceReflection.hlsl
// Renderer-owned depth pyramid, screen-space trace, and materialized fallback resolve.
// =============================================================================

#include "../Include/Exposure.hlsli"

cbuffer SSRConstants : register(b0, space0)
{
    float4 RenderSize;              // xy extent, zw inverse extent
    float4 SourceSize;              // xy source mip extent, zw inverse extent
    float4 TraceParameters;         // max distance, thickness, roughness cutoff, edge fade
    // x = max hierarchy probes, y = trace refinement steps, z = mip count,
    // w = flags. Reduce stages receive their exact source extent separately.
    uint4 TraceControl;
    float4 CameraPosition;
    float4 CameraForward;
    float4 ProbeControl;            // x reserved, y ready count, z SSR requested, w local-probe mips
    float4 ProbeWeights[2];         // eight fixed-capacity influence weights
    int4 ProbePriorities[2];
    float4 ProbePositions[8];
    float4 ProbeExtents[8];
    float4x4 ProbeWorldTransforms[8];
    float4x4 ProbeInverseWorldTransforms[8];
    float4 ProbeBoxProjectionSizes[8];
    float4 ProbeBoxProjectionOffsets[8];
    // x normalized blend distance, y sphere flag, z box projection flag.
    float4 ProbeInfluenceModes[8];
    float4x4 InverseViewProjection;
    float4x4 ViewProjection;
};

Texture2D<float4> SceneColor : register(t1, space0);
Texture2D<float> StageSource : register(t2, space0);
Texture2D<float4> NormalRoughness : register(t3, space0);
Texture2D<float> HiZPyramid : register(t4, space0);
Texture2D<float4> RawReflection : register(t5, space0);
Texture2D<float> ReflectionConfidence : register(t6, space0);
TextureCube<float4> LocalProbes[8] : register(t7, space0);
Texture2D<float4> BaseIndirectSpecular : register(t15, space0);
StructuredBuffer<RVXExposureState> ExposureState : register(t16, space0);
Texture2D<float4> SpecularResponse : register(t17, space0);
SamplerState LinearClampSampler : register(s19, space0);

static const uint RVX_SSR_FLAG_REVERSE_Z = 1u << 0u;
static const uint RVX_SSR_FLAG_NORMAL_SOURCE = 1u << 1u;
static const uint RVX_SSR_FLAG_FIRST_PROBE_READY = 1u << 2u;
static const uint RVX_SSR_FLAG_SCREEN_SPACE_REQUESTED = 1u << 10u;
// Set only for the one armed qualification readback. Normal frames retain
// their existing route-mask semantics and never fail a diagnostic route for
// valid-but-black lighting.
static const uint RVX_SSR_FLAG_QUALIFICATION_RADIANCE_ROUTE = 1u << 11u;
static const float RVX_SSR_EPSILON = 1.0e-5;

struct FullscreenInput
{
    float4 Position : SV_POSITION;
    float2 TexCoord : TEXCOORD0;
};

struct SSRTraceOutput
{
    float4 radiance : SV_Target0;
    float confidence : SV_Target1;
};

struct SSRResolveOutput
{
    float4 sceneColor : SV_Target0;
    float route : SV_Target1;
};

bool RVX_IsBackgroundDepth(float depth)
{
    return (TraceControl.w & RVX_SSR_FLAG_REVERSE_Z) != 0u
        ? depth <= 1.0e-6
        : depth >= 1.0 - 1.0e-6;
}

float3 RVX_SafeNormalize(float3 value, float3 fallback)
{
    const float lengthSquared = dot(value, value);
    return lengthSquared > RVX_SSR_EPSILON
        ? value * rsqrt(lengthSquared)
        : fallback;
}

bool RVX_IsFiniteNonZeroRadiance(float3 radiance)
{
    return all(isfinite(radiance)) &&
        max(radiance.x, max(radiance.y, radiance.z)) > RVX_SSR_EPSILON;
}

float2 RVX_OctWrap(float2 value)
{
    return (1.0 - abs(value.yx)) *
        float2(value.x >= 0.0 ? 1.0 : -1.0,
               value.y >= 0.0 ? 1.0 : -1.0);
}

float3 RVX_DecodeOctNormal(float2 encoded)
{
    float2 folded = encoded * 2.0 - 1.0;
    float3 normal = float3(folded, 1.0 - abs(folded.x) - abs(folded.y));
    if (normal.z < 0.0)
    {
        normal.xy = RVX_OctWrap(normal.xy);
    }
    return RVX_SafeNormalize(normal, float3(0.0, 1.0, 0.0));
}

float3 RVX_ReconstructWorld(float2 uv, float depth)
{
    const float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    float4 world = mul(InverseViewProjection, float4(ndc, depth, 1.0));
    world.xyz /= abs(world.w) > RVX_SSR_EPSILON ? world.w : 1.0;
    return world.xyz;
}

uint2 RVX_TexelFromUV(float2 uv, uint2 extent)
{
    const uint2 safeExtent = max(extent, uint2(1u, 1u));
    return min(uint2(saturate(uv) * float2(safeExtent)), safeExtent - 1u);
}

float2 RVX_TexelCenterUV(uint2 texel, uint2 extent)
{
    const uint2 safeExtent = max(extent, uint2(1u, 1u));
    return (float2(min(texel, safeExtent - 1u)) + 0.5) /
        float2(safeExtent);
}

float RVX_ViewDepth(float3 world)
{
    return max(0.0, dot(world - CameraPosition.xyz,
                        RVX_SafeNormalize(CameraForward.xyz,
                                          float3(0.0, 0.0, -1.0))));
}

float2 RVX_ProjectUV(float3 world, out bool valid)
{
    const float4 clip = mul(ViewProjection, float4(world, 1.0));
    valid = clip.w > RVX_SSR_EPSILON;
    const float2 ndc = clip.xy / max(clip.w, RVX_SSR_EPSILON);
    const float2 uv = float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
    valid = valid && all(uv >= 0.0) && all(uv <= 1.0);
    return uv;
}

float3 RVX_ReconstructNormal(float2 uv)
{
    const uint2 extent = max(uint2(RenderSize.xy), uint2(1u, 1u));
    const uint2 centerPixel = RVX_TexelFromUV(uv, extent);
    if ((TraceControl.w & RVX_SSR_FLAG_NORMAL_SOURCE) != 0u)
    {
        return RVX_DecodeOctNormal(
            NormalRoughness.Load(int3(centerPixel, 0)).xy);
    }

    const uint2 rightPixel = min(centerPixel + uint2(1u, 0u), extent - 1u);
    const uint2 downPixel = min(centerPixel + uint2(0u, 1u), extent - 1u);
    const float centerDepth = StageSource.Load(int3(centerPixel, 0));
    const float rightDepth = StageSource.Load(int3(rightPixel, 0));
    const float downDepth = StageSource.Load(int3(downPixel, 0));
    if (RVX_IsBackgroundDepth(centerDepth) ||
        RVX_IsBackgroundDepth(rightDepth) || RVX_IsBackgroundDepth(downDepth))
    {
        return -RVX_SafeNormalize(CameraForward.xyz,
                                  float3(0.0, 0.0, -1.0));
    }
    const float2 centerUV = RVX_TexelCenterUV(centerPixel, extent);
    const float2 rightUV = RVX_TexelCenterUV(rightPixel, extent);
    const float2 downUV = RVX_TexelCenterUV(downPixel, extent);
    const float3 center = RVX_ReconstructWorld(centerUV, centerDepth);
    const float3 right = RVX_ReconstructWorld(rightUV, rightDepth);
    const float3 down = RVX_ReconstructWorld(downUV, downDepth);
    float3 normal = RVX_SafeNormalize(
        cross(right - center, down - center),
        -RVX_SafeNormalize(CameraForward.xyz, float3(0.0, 0.0, -1.0)));
    const float3 toCamera = RVX_SafeNormalize(
        CameraPosition.xyz - center, -CameraForward.xyz);
    return dot(normal, toCamera) >= 0.0 ? normal : -normal;
}

float RVX_EdgeConfidence(float2 uv)
{
    const float distanceToEdge = min(
        min(uv.x, 1.0 - uv.x), min(uv.y, 1.0 - uv.y));
    return saturate(distanceToEdge / max(TraceParameters.w, 1.0e-4));
}

float PSHiZBase(FullscreenInput input) : SV_Target0
{
    const uint2 extent = max(uint2(RenderSize.xy), uint2(1u, 1u));
    const uint2 pixel = min(uint2(input.Position.xy), extent - 1u);
    const float2 uv = RVX_TexelCenterUV(pixel, extent);
    const float depth = StageSource.Load(int3(pixel, 0));
    if (RVX_IsBackgroundDepth(depth))
    {
        return TraceParameters.x;
    }
    return min(TraceParameters.x,
               RVX_ViewDepth(RVX_ReconstructWorld(uv, depth)));
}

float PSHiZReduce(FullscreenInput input) : SV_Target0
{
    const uint2 sourceExtent = max(uint2(SourceSize.xy), uint2(1u, 1u));
    const uint2 destinationExtent = max(sourceExtent / 2u, uint2(1u, 1u));
    const uint2 destinationPixel = min(
        uint2(input.Position.xy), destinationExtent - 1u);

    // Partition the complete source extent instead of assuming every mip is
    // exactly 2x2. A 5x3 source therefore contributes its final column/row to
    // the 2x1 destination and the pyramid remains conservative for NPOT views.
    const uint2 sourceBegin =
        (destinationPixel * sourceExtent) / destinationExtent;
    const uint2 sourceEnd = max(
        sourceBegin + 1u,
        ((destinationPixel + 1u) * sourceExtent) / destinationExtent);
    float minimumDepth = TraceParameters.x;
    [loop]
    for (uint sourceY = sourceBegin.y;
         sourceY < min(sourceEnd.y, sourceExtent.y);
         ++sourceY)
    {
        [loop]
        for (uint sourceX = sourceBegin.x;
             sourceX < min(sourceEnd.x, sourceExtent.x);
             ++sourceX)
        {
            minimumDepth = min(
                minimumDepth,
                StageSource.Load(int3(uint2(sourceX, sourceY), 0)));
        }
    }
    return minimumDepth;
}

float RVX_SolveBoundaryTravel(float4 clipOrigin,
                              float4 clipDirection,
                              float targetNDC,
                              bool xAxis)
{
    const float originComponent =
        xAxis ? clipOrigin.x : clipOrigin.y;
    const float directionComponent =
        xAxis ? clipDirection.x : clipDirection.y;
    const float denominator =
        directionComponent - targetNDC * clipDirection.w;
    if (abs(denominator) <= RVX_SSR_EPSILON)
    {
        return TraceParameters.x + 1.0;
    }
    return (targetNDC * clipOrigin.w - originComponent) / denominator;
}

SSRTraceOutput PSTrace(FullscreenInput input)
{
    SSRTraceOutput output;
    output.radiance = 0.0;
    output.confidence = 0.0;
    if ((TraceControl.w & RVX_SSR_FLAG_SCREEN_SPACE_REQUESTED) == 0u)
    {
        return output;
    }

    const uint2 renderExtent = max(uint2(RenderSize.xy), uint2(1u, 1u));
    const uint2 sourcePixel = min(uint2(input.Position.xy), renderExtent - 1u);
    const float2 uv = RVX_TexelCenterUV(sourcePixel, renderExtent);
    const float deviceDepth = StageSource.Load(int3(sourcePixel, 0));
    if (RVX_IsBackgroundDepth(deviceDepth))
    {
        return output;
    }

    const float4 normalSample = NormalRoughness.Load(int3(sourcePixel, 0));
    const float roughness = (TraceControl.w & RVX_SSR_FLAG_NORMAL_SOURCE) != 0u
        ? saturate(normalSample.z)
        : 0.2;
    if (roughness >= TraceParameters.z)
    {
        return output;
    }

    const float3 world = RVX_ReconstructWorld(uv, deviceDepth);
    const float3 normal = RVX_ReconstructNormal(uv);
    const float3 incident = RVX_SafeNormalize(
        world - CameraPosition.xyz, CameraForward.xyz);
    const float3 rayDirection = RVX_SafeNormalize(
        reflect(incident, normal), normal);
    if (dot(rayDirection, normal) <= 0.0)
    {
        return output;
    }

    const uint maxSteps = clamp(TraceControl.x, 1u, 256u);
    const uint refinementSteps = min(TraceControl.y, 16u);
    const uint mipCount = max(TraceControl.z, 1u);
    const uint maximumTraceMip = min(mipCount - 1u, 5u);
    const float fineStep = max(
        TraceParameters.y * 0.5,
        TraceParameters.x /
            (float(maxSteps) * exp2(float(maximumTraceMip))));
    const float3 rayOrigin =
        world + normal * (TraceParameters.y * 2.0);
    const float4 rayClipOrigin =
        mul(ViewProjection, float4(rayOrigin, 1.0));
    const float4 rayClipDirection =
        mul(ViewProjection, float4(rayDirection, 0.0));
    const float2 uvDirection = float2(
        rayClipDirection.x * rayClipOrigin.w -
            rayClipOrigin.x * rayClipDirection.w,
        -(rayClipDirection.y * rayClipOrigin.w -
            rayClipOrigin.y * rayClipDirection.w));
    uint mip = 0u;
    float frontTravel = 0.0;
    float travel = fineStep;
    bool hasVerifiedFront = false;
    float2 hitUV = 0.0;
    float hitDistance = 0.0;
    bool hit = false;

    [loop]
    for (uint stepIndex = 0u; stepIndex < maxSteps; ++stepIndex)
    {
        const float3 sampleWorld = rayOrigin + rayDirection * travel;
        bool projected = false;
        const float2 sampleUV = RVX_ProjectUV(sampleWorld, projected);
        if (!projected)
        {
            break;
        }

        const uint2 mipExtent = max(renderExtent >> mip, uint2(1u, 1u));
        const float2 directionSign = float2(
            uvDirection.x > RVX_SSR_EPSILON
                ? 1.0
                : (uvDirection.x < -RVX_SSR_EPSILON ? -1.0 : 0.0),
            uvDirection.y > RVX_SSR_EPSILON
                ? 1.0
                : (uvDirection.y < -RVX_SSR_EPSILON ? -1.0 : 0.0));
        const float2 cellSelectionUV = saturate(
            sampleUV + directionSign *
                (1.0e-4 / float2(renderExtent)));
        const uint2 basePixel = RVX_TexelFromUV(
            cellSelectionUV, renderExtent);
        const uint2 mipPixel = min(basePixel >> mip, mipExtent - 1u);
        const uint2 cellBaseBegin = mipPixel << mip;
        const uint2 cellBaseEnd = uint2(
            mipPixel.x + 1u < mipExtent.x
                ? (mipPixel.x + 1u) << mip
                : renderExtent.x,
            mipPixel.y + 1u < mipExtent.y
                ? (mipPixel.y + 1u) << mip
                : renderExtent.y);

        float xBoundaryTravel = TraceParameters.x + 1.0;
        if (directionSign.x != 0.0)
        {
            const float boundaryUV =
                float(directionSign.x > 0.0
                          ? cellBaseEnd.x : cellBaseBegin.x) /
                float(renderExtent.x);
            xBoundaryTravel = RVX_SolveBoundaryTravel(
                rayClipOrigin, rayClipDirection,
                boundaryUV * 2.0 - 1.0, true);
        }
        float yBoundaryTravel = TraceParameters.x + 1.0;
        if (directionSign.y != 0.0)
        {
            const float boundaryUV =
                float(directionSign.y > 0.0
                          ? cellBaseEnd.y : cellBaseBegin.y) /
                float(renderExtent.y);
            yBoundaryTravel = RVX_SolveBoundaryTravel(
                rayClipOrigin, rayClipDirection,
                1.0 - boundaryUV * 2.0, false);
        }
        if (xBoundaryTravel <= travel + RVX_SSR_EPSILON)
        {
            xBoundaryTravel = TraceParameters.x + 1.0;
        }
        if (yBoundaryTravel <= travel + RVX_SSR_EPSILON)
        {
            yBoundaryTravel = TraceParameters.x + 1.0;
        }
        const float cellExitTravel = min(
            TraceParameters.x,
            min(xBoundaryTravel, yBoundaryTravel));
        if (cellExitTravel <= travel + RVX_SSR_EPSILON)
        {
            break;
        }

        const float sceneDepth = HiZPyramid.Load(int3(mipPixel, mip));
        const float3 cellExitWorld =
            rayOrigin + rayDirection * cellExitTravel;
        const float maximumRayDepth = max(
            RVX_ViewDepth(sampleWorld), RVX_ViewDepth(cellExitWorld));
        const bool wholeCellIntervalIsFront =
            maximumRayDepth < sceneDepth - TraceParameters.y;
        if (!wholeCellIntervalIsFront)
        {
            if (!hasVerifiedFront)
            {
                const float entryRayDepth = RVX_ViewDepth(sampleWorld);
                if (mip == 0u &&
                    entryRayDepth < sceneDepth - TraceParameters.y)
                {
                    // The first mip0 cell may itself contain a crossing. Its
                    // entry point is a valid front endpoint even though the
                    // complete interval cannot be skipped.
                    frontTravel = travel;
                    hasVerifiedFront = true;
                }
                else
                {
                    // Never manufacture a bracket from the shaded surface or
                    // an unclassified interval. Stay at mip0 until a point or
                    // complete interval is proven to be in front.
                    mip = 0u;
                    travel = cellExitTravel;
                    continue;
                }
            }
            if (mip != 0u)
            {
                // The complete coarse interval could contain a crossing.
                // Revisit the same interval at the next finer mip; no screen
                // cell between the verified front and candidate is skipped.
                --mip;
                continue;
            }

            float front = frontTravel;
            float back = cellExitTravel;
            [loop]
            for (uint refinement = 0u;
                 refinement < refinementSteps;
                 ++refinement)
            {
                const float candidate = 0.5 * (front + back);
                const float3 candidateWorld =
                    rayOrigin + rayDirection * candidate;
                bool candidateProjected = false;
                const float2 candidateUV =
                    RVX_ProjectUV(candidateWorld, candidateProjected);
                if (!candidateProjected)
                {
                    back = candidate;
                    continue;
                }
                const uint2 candidatePixel =
                    RVX_TexelFromUV(candidateUV, renderExtent);
                const float candidateSceneDepth =
                    HiZPyramid.Load(int3(candidatePixel, 0));
                const float candidateRayDepth = RVX_ViewDepth(candidateWorld);
                if (candidateRayDepth >=
                    candidateSceneDepth - TraceParameters.y)
                {
                    back = candidate;
                }
                else
                {
                    front = candidate;
                }
            }

            const float3 refinedWorld = rayOrigin + rayDirection * back;
            bool refinedProjected = false;
            const float2 refinedUV =
                RVX_ProjectUV(refinedWorld, refinedProjected);
            if (refinedProjected)
            {
                const uint2 refinedPixel =
                    RVX_TexelFromUV(refinedUV, renderExtent);
                const float refinedSceneDepth =
                    HiZPyramid.Load(int3(refinedPixel, 0));
                const float refinedDelta =
                    RVX_ViewDepth(refinedWorld) - refinedSceneDepth;
                if (isfinite(refinedDelta) &&
                    abs(refinedDelta) <= TraceParameters.y)
                {
                    hit = true;
                    hitUV = refinedUV;
                    hitDistance = back;
                    break;
                }
            }

            // The candidate was not within the accepted surface thickness.
            // It is not a valid front point, so discard the bracket and seek
            // a new mip0 interval that is explicitly proven to be in front.
            hasVerifiedFront = false;
            mip = 0u;
            travel = cellExitTravel;
        }
        else
        {
            // Only a complete cell interval whose maximum ray depth precedes
            // that cell's conservative minimum scene depth may advance the
            // front end of a later binary-search bracket.
            frontTravel = cellExitTravel;
            hasVerifiedFront = true;
            mip = min(mip + 1u, maximumTraceMip);
            travel = cellExitTravel;
        }
        if (travel > TraceParameters.x)
        {
            break;
        }
    }

    if (!hit)
    {
        return output;
    }

    const float edge = RVX_EdgeConfidence(hitUV);
    const float distanceConfidence =
        saturate(1.0 - hitDistance / max(TraceParameters.x, RVX_SSR_EPSILON));
    const float roughnessConfidence =
        saturate(1.0 - roughness / max(TraceParameters.z, RVX_SSR_EPSILON));
    const float confidence = edge * distanceConfidence * roughnessConfidence;
    output.radiance = float4(
        max(SceneColor.SampleLevel(LinearClampSampler, hitUV, 0.0).rgb, 0.0),
        confidence);
    output.confidence = confidence;
    return output;
}

float RVX_ProbeInfluence(float3 world, uint probeIndex)
{
    const float4 localHomogeneous = mul(
        ProbeInverseWorldTransforms[probeIndex], float4(world, 1.0));
    if (!all(isfinite(localHomogeneous)) ||
        abs(localHomogeneous.w) <= RVX_SSR_EPSILON)
    {
        return 0.0;
    }
    const float3 localPosition =
        localHomogeneous.xyz / localHomogeneous.w;
    const float3 safeExtent = max(
        abs(ProbeExtents[probeIndex].xyz), 1.0e-3);
    const float3 normalizedPosition = localPosition / safeExtent;
    const float normalizedDistance =
        ProbeInfluenceModes[probeIndex].y >= 0.5
            ? length(normalizedPosition)
            : max(abs(normalizedPosition.x),
                  max(abs(normalizedPosition.y),
                      abs(normalizedPosition.z)));
    if (!isfinite(normalizedDistance) || normalizedDistance > 1.0)
    {
        return 0.0;
    }
    const float normalizedBlendDistance =
        ProbeInfluenceModes[probeIndex].x;
    return normalizedBlendDistance <= RVX_SSR_EPSILON
        ? 1.0
        : saturate((1.0 - normalizedDistance) /
                   normalizedBlendDistance);
}

float3 RVX_LocalProbeDirection(float3 world,
                               float3 reflectionDirection,
                               uint probeIndex)
{
    if (ProbeInfluenceModes[probeIndex].z < 0.5)
    {
        return reflectionDirection;
    }

    const float4 localHomogeneous = mul(
        ProbeInverseWorldTransforms[probeIndex], float4(world, 1.0));
    const float3 localPosition =
        localHomogeneous.xyz /
        (abs(localHomogeneous.w) > RVX_SSR_EPSILON
             ? localHomogeneous.w : 1.0);
    const float3 localDirection = RVX_SafeNormalize(
        mul(ProbeInverseWorldTransforms[probeIndex],
            float4(reflectionDirection, 0.0)).xyz,
        reflectionDirection);
    const float3 boxCenter = ProbeBoxProjectionOffsets[probeIndex].xyz;
    const float3 halfSize = max(
        abs(ProbeBoxProjectionSizes[probeIndex].xyz) * 0.5,
        RVX_SSR_EPSILON);
    const float3 relativePosition = localPosition - boxCenter;
    const float3 safeDirection = float3(
        abs(localDirection.x) > RVX_SSR_EPSILON
            ? localDirection.x :
              (localDirection.x >= 0.0 ? RVX_SSR_EPSILON
                                       : -RVX_SSR_EPSILON),
        abs(localDirection.y) > RVX_SSR_EPSILON
            ? localDirection.y :
              (localDirection.y >= 0.0 ? RVX_SSR_EPSILON
                                       : -RVX_SSR_EPSILON),
        abs(localDirection.z) > RVX_SSR_EPSILON
            ? localDirection.z :
              (localDirection.z >= 0.0 ? RVX_SSR_EPSILON
                                       : -RVX_SSR_EPSILON));
    const float3 firstIntersection =
        (-halfSize - relativePosition) / safeDirection;
    const float3 secondIntersection =
        (halfSize - relativePosition) / safeDirection;
    const float3 farIntersection = max(
        firstIntersection, secondIntersection);
    const float exitDistance = min(
        farIntersection.x,
        min(farIntersection.y, farIntersection.z));
    if (!isfinite(exitDistance) || exitDistance <= 0.0)
    {
        return reflectionDirection;
    }

    const float3 localHit =
        localPosition + localDirection * exitDistance;
    const float3 worldHit = mul(
        ProbeWorldTransforms[probeIndex], float4(localHit, 1.0)).xyz;
    return RVX_SafeNormalize(
        worldHit - ProbePositions[probeIndex].xyz,
        reflectionDirection);
}

SSRResolveOutput PSResolve(FullscreenInput input)
{
    SSRResolveOutput output;
    const uint2 renderExtent = max(uint2(RenderSize.xy), uint2(1u, 1u));
    const uint2 pixel = min(uint2(input.Position.xy), renderExtent - 1u);
    const float2 uv = RVX_TexelCenterUV(pixel, renderExtent);
    const float4 scene = SceneColor.SampleLevel(LinearClampSampler, uv, 0.0);
    const float depth = StageSource.Load(int3(pixel, 0));
    if (RVX_IsBackgroundDepth(depth))
    {
        output.sceneColor = scene;
        // Background has no receiver material. It is deliberately distinct
        // from the materialized global-IBL baseline route in qualification.
        output.route = 0.0;
        return output;
    }

    const RVXExposureState exposureState = ExposureState[0];
    if (!RVXExposureStateIsValid(exposureState))
    {
        output.sceneColor = scene;
        // Deliberately encode a value outside the four valid qualification
        // classes. A receiver must never masquerade as background when the
        // producer exposure state cannot be consumed.
        output.route = 0.125;
        return output;
    }
    const float producerPreExposure = RVXExposureProducerScale(exposureState);
    const float3 rawBaseIndirectSpecular =
        BaseIndirectSpecular.Load(int3(pixel, 0)).rgb;
    const float3 baseIndirectSpecular = max(rawBaseIndirectSpecular, 0.0);
    const bool qualificationRadianceRoute =
        (TraceControl.w & RVX_SSR_FLAG_QUALIFICATION_RADIANCE_ROUTE) != 0u;
    const float3 specularResponse = max(
        SpecularResponse.Load(int3(pixel, 0)).rgb, 0.0);
    const float maximumBase = max(
        baseIndirectSpecular.x,
        max(baseIndirectSpecular.y, baseIndirectSpecular.z));
    const float maximumResponse = max(
        specularResponse.x,
        max(specularResponse.y, specularResponse.z));
    if (maximumResponse <= RVX_SSR_EPSILON)
    {
        output.sceneColor = scene;
        // A zero-response material is not a reflection receiver. An existing
        // nonzero baseline paired with zero response is instead a broken
        // producer contract and deliberately maps to an invalid class.
        output.route = maximumBase <= RVX_SSR_EPSILON
            ? 0.06666667
            : 0.125;
        return output;
    }

    const float3 world = RVX_ReconstructWorld(uv, depth);
    const float3 normal = RVX_ReconstructNormal(uv);
    const float3 viewDirection = RVX_SafeNormalize(
        CameraPosition.xyz - world, -CameraForward.xyz);
    const float3 reflectionDirection = reflect(-viewDirection, normal);
    const float roughness = (TraceControl.w & RVX_SSR_FLAG_NORMAL_SOURCE) != 0u
        ? saturate(NormalRoughness.Load(int3(pixel, 0)).z)
        : 0.2;
    const float lod = roughness * max(ProbeControl.w - 1.0, 0.0);
    int highestInfluencingPriority = -2147483647;
    [unroll]
    for (uint probeIndex = 0u; probeIndex < 8u; ++probeIndex)
    {
        const uint readyFlag =
            RVX_SSR_FLAG_FIRST_PROBE_READY << probeIndex;
        if ((TraceControl.w & readyFlag) == 0u)
        {
            continue;
        }
        const float influence = RVX_ProbeInfluence(world, probeIndex);
        if (influence > RVX_SSR_EPSILON)
        {
            highestInfluencingPriority = max(
                highestInfluencingPriority,
                ProbePriorities[probeIndex / 4u][probeIndex % 4u]);
        }
    }

    float probeWeightSum = 0.0;
    float3 weightedLocalSpecular = 0.0;
    bool localProbeSamplesFinite = true;
    [unroll]
    for (uint probeIndex = 0u; probeIndex < 8u; ++probeIndex)
    {
        const uint readyFlag =
            RVX_SSR_FLAG_FIRST_PROBE_READY << probeIndex;
        if ((TraceControl.w & readyFlag) == 0u)
        {
            continue;
        }
        const int priority = ProbePriorities[probeIndex / 4u][
            probeIndex % 4u];
        if (priority != highestInfluencingPriority)
        {
            continue;
        }
        const float influence = RVX_ProbeInfluence(world, probeIndex);
        // View-level influence is used only for deterministic admission and
        // ordering. Per-pixel energy is weighted exactly once by the probe
        // volume, within the highest-priority overlapping group.
        const float localWeight = influence;
        probeWeightSum += localWeight;
        const float3 probeDirection = RVX_LocalProbeDirection(
            world, reflectionDirection, probeIndex);
        // A zero influence has no physical contribution and cannot affect the
        // qualification route. Avoid sampling it too: multiplying a malformed
        // sample by zero can still propagate NaN on some shader backends.
        if (localWeight > RVX_SSR_EPSILON)
        {
            const float3 rawLocalProbeRadiance =
                LocalProbes[probeIndex].SampleLevel(
                    LinearClampSampler, probeDirection, lod).rgb;
            // Every contributing cubemap sample must remain finite before
            // clamping, otherwise it is invalid qualification evidence.
            if (qualificationRadianceRoute &&
                !all(isfinite(rawLocalProbeRadiance)))
            {
                localProbeSamplesFinite = false;
            }
            weightedLocalSpecular += max(rawLocalProbeRadiance, 0.0) *
                localWeight;
        }
    }

    float3 probeSpecular = baseIndirectSpecular;
    float3 materializedLocal = 0.0;
    const float probeTotal = saturate(probeWeightSum);
    if (probeTotal > RVX_SSR_EPSILON)
    {
        const float3 local = weightedLocalSpecular /
            max(probeWeightSum, RVX_SSR_EPSILON);
        materializedLocal =
            local * specularResponse * producerPreExposure;
        probeSpecular = lerp(
            baseIndirectSpecular, materializedLocal, probeTotal);
    }

    const float confidence = saturate(
        ReflectionConfidence.Load(int3(pixel, 0)));
    // Screen-space source radiance was sampled from the producer's already
    // pre-exposed HDR scene, so it receives only the destination material
    // response. Local cubemap radiance above receives both response and the
    // producer pre-exposure. The existing exact baseline is subtracted once.
    const float3 ssr = max(
        RawReflection.Load(int3(pixel, 0)).rgb, 0.0) * specularResponse;
    const float3 routed = lerp(probeSpecular, ssr, confidence);
    const float3 resolved = max(
        scene.rgb - baseIndirectSpecular + routed,
        0.0);
    output.sceneColor = float4(resolved, scene.a);
    const bool screenSpaceRoute = confidence > 0.001;
    if (!screenSpaceRoute && qualificationRadianceRoute)
    {
        const bool localProbeRoute = probeTotal > RVX_SSR_EPSILON;
        const bool routeRadianceQualified = localProbeRoute
            ? (localProbeSamplesFinite &&
               RVX_IsFiniteNonZeroRadiance(
                   materializedLocal * probeTotal) &&
               RVX_IsFiniteNonZeroRadiance(routed))
            : (all(isfinite(rawBaseIndirectSpecular)) &&
               RVX_IsFiniteNonZeroRadiance(baseIndirectSpecular) &&
               RVX_IsFiniteNonZeroRadiance(routed));
        if (!routeRadianceQualified)
        {
            // 0.125 deliberately quantizes outside all valid route classes.
            // The qualification readback treats it as invalid evidence; the
            // already-written scene color remains unchanged.
            output.route = 0.125;
            return output;
        }
    }
    output.route = screenSpaceRoute
        ? 0.33333334
        : (probeTotal > RVX_SSR_EPSILON ? 0.66666667 : 1.0);
    return output;
}
