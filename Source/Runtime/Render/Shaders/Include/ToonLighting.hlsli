#ifndef TOON_LIGHTING_HLSLI
#define TOON_LIGHTING_HLSLI

// Canonical GFX14 ToonLit reference.  Both Forward+ and Deferred call these
// functions so banding, thresholded specular, rim and shadow tint remain one
// renderer contract rather than recipe-local approximations.

float RVX_ToonQuantizeDiffuse(float nDotL, float authoredBandCount)
{
    const float bandCount = clamp(round(authoredBandCount), 1.0f, 64.0f);
    const float lit = saturate(nDotL);
    if (bandCount <= 1.0f)
    {
        return lit;
    }
    const float bandIndex = min(
        floor(lit * bandCount), bandCount - 1.0f);
    return bandIndex / (bandCount - 1.0f);
}

float3 RVX_EvaluateToonDirectLight(
    float3 normal,
    float3 viewDirection,
    float3 lightDirection,
    float3 baseColor,
    float3 lightRadiance,
    float visibility,
    float toonBands,
    float toonSpecularThreshold,
    float toonRimStrength,
    float3 toonShadowTint)
{
    const float3 N = normalize(normal);
    const float3 V = normalize(viewDirection);
    const float3 L = normalize(lightDirection);
    const float3 H = normalize(V + L);
    const float diffuseBand = RVX_ToonQuantizeDiffuse(dot(N, L), toonBands);
    const float specular = step(
        saturate(toonSpecularThreshold), saturate(dot(N, H)));
    const float rim = pow(saturate(1.0f - dot(N, V)), 2.0f) *
        max(toonRimStrength, 0.0f);
    const float3 shadedDirect = baseColor * diffuseBand + specular.xxx +
        baseColor * rim;
    const float3 tint = lerp(
        saturate(toonShadowTint), float3(1.0f, 1.0f, 1.0f),
        saturate(visibility));
    return shadedDirect * max(lightRadiance, 0.0f) * tint;
}

#endif // TOON_LIGHTING_HLSLI
