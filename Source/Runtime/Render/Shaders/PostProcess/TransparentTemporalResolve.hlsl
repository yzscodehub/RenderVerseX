// T0 input ABI: full-resolution textures in the same view/graph generation.
// Surface.xy = current NDC - previous presented NDC, z = composited color
// contribution, w = positive linear view depth. Flags are prepared only after
// CPU instance/generation/history validation. Opaque velocity is read-only.
Texture2D<float2> OpaqueVelocity : register(t0, space0);
Texture2D<float> OpaqueReactive : register(t1, space0);
Texture2D<float4> Surface : register(t2, space0);
Texture2D<uint> SurfaceFlags : register(t3, space0);
Texture2D<float> OpaqueLinearDepth : register(t4, space0);
Texture2D<float> RawLayerCount : register(t5, space0);
Texture2D<float> RawMeshReactive : register(t6, space0);
cbuffer RawTemporalParameters : register(b7, space0)
{
    float4x4 RawInverseProjection;
    float4 RawPolicy; // x: view history allowed; remaining fields reserved.
};

static const uint SURFACE_PRESENT = 1u;
static const uint SORTED = 2u;
static const uint HISTORY_QUALIFIED = 4u;
static const uint OVERLAPPING = 8u;
static const uint REFRACTIVE = 16u;

struct TemporalOutputs
{
    float2 Velocity : SV_Target0;
    float Reactive : SV_Target1;
    float LinearDepth : SV_Target2;
};

TemporalOutputs ResolveTemporal(float4 position, bool allowHistory)
{
    const int3 pixel = int3(int2(position.xy), 0);
    TemporalOutputs output;
    const float depth = OpaqueLinearDepth.Load(pixel);
    output.LinearDepth = isfinite(depth) && depth > 0.0f ? depth : 0.0f;
    output.Velocity = OpaqueVelocity.Load(pixel);
    output.Reactive = OpaqueReactive.Load(pixel);
    if (!all(isfinite(output.Velocity)) || !isfinite(output.Reactive))
    {
        output.Velocity = 0.0f.xx;
        output.Reactive = 1.0f;
        return output;
    }
    output.Reactive = saturate(output.Reactive);
    const uint flags = SurfaceFlags.Load(pixel);
    if ((flags & SURFACE_PRESENT) == 0u)
        return output;

    const float4 surface = Surface.Load(pixel);
    if ((flags & ~31u) != 0u || !all(isfinite(surface)) ||
        !isfinite(depth) || depth <= 0.0f || surface.w <= 0.0f ||
        surface.z < 0.0f || surface.z > 1.0f)
    {
        // Malformed GPU inputs never propagate non-finite motion into history.
        output.Reactive = 1.0f;
        return output;
    }
    if (surface.z == 0.0f || surface.w >= depth)
        return output;

    const bool dominantSurface = (flags & SORTED) != 0u &&
        (flags & (OVERLAPPING | REFRACTIVE)) == 0u && surface.z == 1.0f;
    if (dominantSurface)
        output.LinearDepth = surface.w;
    const bool singleQualifiedSurface = allowHistory &&
        (flags & (SORTED | HISTORY_QUALIFIED)) == (SORTED | HISTORY_QUALIFIED) &&
        (flags & (OVERLAPPING | REFRACTIVE)) == 0u && surface.z == 1.0f;
    if (singleQualifiedSurface)
        output.Velocity = surface.xy;
    else
        output.Reactive = max(output.Reactive, surface.z);
    return output;
}

TemporalOutputs PSMain(float4 position : SV_Position)
{
    return ResolveTemporal(position, true);
}

TemporalOutputs PSNoHistory(float4 position : SV_Position)
{
    return ResolveTemporal(position, false);
}

// Production admission: raw geometry history and material history are
// independent proofs. Neither one alone permits transparent history reuse.
TemporalOutputs PSRaw(float4 position : SV_Position)
{
    const int3 pixel = int3(int2(position.xy), 0);
    uint width, height;
    OpaqueLinearDepth.GetDimensions(width, height);
    const float2 uv = position.xy / float2(width, height);
    const float deviceDepth = OpaqueLinearDepth.Load(pixel);
    const float4 viewPoint = mul(RawInverseProjection,
        float4(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f, deviceDepth, 1.0f));
    const float depth = abs(viewPoint.z / viewPoint.w);
    TemporalOutputs output;
    output.Velocity = OpaqueVelocity.Load(pixel);
    output.Reactive = OpaqueReactive.Load(pixel);
    output.LinearDepth = isfinite(depth) && depth > 0.0f ? depth : 0.0f;
    if (!all(isfinite(output.Velocity)) || !isfinite(output.Reactive) ||
        !isfinite(deviceDepth) || !all(isfinite(viewPoint)) || abs(viewPoint.w) < 1.0e-8f ||
        !isfinite(depth) || depth <= 0.0f)
    {
        output.Velocity = 0.0f.xx;
        output.Reactive = 1.0f;
        return output;
    }
    output.Reactive = saturate(output.Reactive);
    const uint flags = SurfaceFlags.Load(pixel);
    const float layers = RawLayerCount.Load(pixel);
    if (flags == 0u && layers == 0.0f)
    {
        const float meshReactive = RawMeshReactive.Load(pixel);
        output.Reactive = isfinite(meshReactive) ? max(output.Reactive, saturate(meshReactive)) : 1.0f;
        return output;
    }
    const float4 surface = Surface.Load(pixel);
    if ((flags & ~255u) != 0u || (flags & 129u) != 129u ||
        !isfinite(layers) || layers < 1.0f || !all(isfinite(surface)) ||
        surface.z <= 0.0f || surface.z > 1.0f || surface.w <= 0.0f)
    {
        output.Reactive = 1.0f;
        return output;
    }
    if (surface.w >= depth)
    {
        // Candidate depth is binary16. Near-coplanar visible geometry can
        // quantize behind the full-precision opaque depth. Never erase its
        // actual color-pass rejection mask based on that rounded comparison.
        const float meshReactive = RawMeshReactive.Load(pixel);
        output.Reactive = isfinite(meshReactive) ? max(output.Reactive, saturate(meshReactive)) : 1.0f;
        return output;
    }
    const bool mixed = layers != 1.0f || (flags & OVERLAPPING) != 0u || output.Reactive > 0.0f;
    const bool dominant = !mixed && (flags & SORTED) != 0u &&
        (flags & REFRACTIVE) == 0u && surface.z == 1.0f;
    if (dominant) output.LinearDepth = surface.w;
    if (dominant && RawPolicy.x > 0.5f && (flags & 96u) == 96u)
        output.Velocity = surface.xy;
    else
        // Surface motion cannot explain refracted background or mixed
        // contributors. Reject that history fully, even at small base alpha.
        output.Reactive = max(output.Reactive, mixed || (flags & REFRACTIVE) != 0u ? 1.0f : surface.z);
    return output;
}
