// =============================================================================
// AmbientOcclusion.hlsli - renderer-wide indirect-lighting AO composition
// =============================================================================
//
// Screen-space ambient occlusion is a visibility term for indirect lighting.
// These helpers deliberately accept individual indirect lobes, so a caller
// cannot accidentally darken direct lights, emissive, sky, or translucency by
// multiplying the fully composed HDR scene color.
// =============================================================================

#ifndef RVX_AMBIENT_OCCLUSION_HLSLI
#define RVX_AMBIENT_OCCLUSION_HLSLI

float RVX_SaturateAmbientOcclusion(float ambientOcclusion)
{
    return saturate(ambientOcclusion);
}

float3 RVX_ApplyIndirectDiffuseAmbientOcclusion(
    float3 indirectDiffuse,
    float ambientOcclusion)
{
    return indirectDiffuse * RVX_SaturateAmbientOcclusion(ambientOcclusion);
}

// Lagarde-style specular visibility approximation. Rough lobes preserve more
// distant environment contribution while smooth lobes receive stronger local
// contact occlusion. `ambientOcclusion == 1` is always identity.
float RVX_IndirectSpecularOcclusion(
    float ambientOcclusion,
    float perceptualRoughness,
    float NoV)
{
    const float safeAO = RVX_SaturateAmbientOcclusion(ambientOcclusion);
    const float safeRoughness = saturate(perceptualRoughness);
    const float safeNoV = saturate(NoV);
    const float exponent = exp2(-16.0 * safeRoughness - 1.0);
    return saturate(pow(safeNoV + safeAO, exponent) - 1.0 + safeAO);
}

float3 RVX_ApplyIndirectSpecularAmbientOcclusion(
    float3 indirectSpecular,
    float ambientOcclusion,
    float perceptualRoughness,
    float NoV)
{
    return indirectSpecular * RVX_IndirectSpecularOcclusion(
        ambientOcclusion, perceptualRoughness, NoV);
}

#endif // RVX_AMBIENT_OCCLUSION_HLSLI
