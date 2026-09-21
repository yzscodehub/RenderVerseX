// T0 input ABI: full-resolution textures in the same view/graph generation.
// Surface.xy = current NDC - previous presented NDC, z = composited color
// contribution, w = positive linear view depth. Flags are prepared only after
// CPU instance/generation/history validation. Opaque velocity is read-only.
Texture2D<float2> OpaqueVelocity : register(t0, space0);
Texture2D<float> OpaqueReactive : register(t1, space0);
Texture2D<float4> Surface : register(t2, space0);
Texture2D<uint> SurfaceFlags : register(t3, space0);
Texture2D<float> OpaqueLinearDepth : register(t4, space0);

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
