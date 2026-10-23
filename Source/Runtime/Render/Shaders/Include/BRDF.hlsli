// =============================================================================
// BRDF.hlsli - Bidirectional Reflectance Distribution Functions
// =============================================================================
//
// Implements the standard Cook-Torrance BRDF with GGX distribution
// for physically based rendering.
//
// References:
// - Epic Games, "Real Shading in Unreal Engine 4"
// - Google Filament documentation
// =============================================================================

#ifndef BRDF_HLSLI
#define BRDF_HLSLI

// =============================================================================
// Constants
// =============================================================================

static const float PI = 3.14159265359;
static const float INV_PI = 0.31830988618;
static const float EPSILON = 1e-6;

// glTF material roughness is perceptual. GGX uses the microfacet alpha value.
float PerceptualRoughnessToAlpha(float perceptualRoughness)
{
    return perceptualRoughness * perceptualRoughness;
}

// =============================================================================
// Normal Distribution Function (D)
// GGX/Trowbridge-Reitz distribution
// =============================================================================

float D_GGX(float NdotH, float alphaRoughness)
{
    float alphaRoughnessSquared = alphaRoughness * alphaRoughness;
    float NdotH2 = NdotH * NdotH;

    float denom = NdotH2 * (alphaRoughnessSquared - 1.0) + 1.0;
    denom = PI * denom * denom;

    return alphaRoughnessSquared / max(denom, EPSILON);
}

// =============================================================================
// Geometry/Visibility Function (G)
// Smith-GGX with Schlick approximation
// =============================================================================

float G1_SchlickGGX(float NdotV, float roughness)
{
    float r = roughness + 1.0;
    float k = (r * r) / 8.0;
    
    return NdotV / (NdotV * (1.0 - k) + k);
}

float G_Smith(float NdotV, float NdotL, float roughness)
{
    float ggx1 = G1_SchlickGGX(NdotV, roughness);
    float ggx2 = G1_SchlickGGX(NdotL, roughness);
    return ggx1 * ggx2;
}

// Height-correlated Smith G term (more accurate)
float G_SmithGGXCorrelated(
    float NdotV,
    float NdotL,
    float alphaRoughness)
{
    float alphaRoughnessSquared = alphaRoughness * alphaRoughness;
    float GGXV = NdotL * sqrt(
        NdotV * NdotV * (1.0 - alphaRoughnessSquared) +
        alphaRoughnessSquared);
    float GGXL = NdotV * sqrt(
        NdotL * NdotL * (1.0 - alphaRoughnessSquared) +
        alphaRoughnessSquared);
    return 0.5 / max(GGXV + GGXL, EPSILON);
}

// =============================================================================
// Fresnel Function (F)
// Schlick's approximation
// =============================================================================

float3 F_Schlick(float VdotH, float3 F0)
{
    return F0 + (1.0 - F0) * pow(1.0 - VdotH, 5.0);
}

float3 F_SchlickWithF90(float VdotH, float3 F0, float3 F90)
{
    return F0 + (F90 - F0) * pow(1.0 - VdotH, 5.0);
}

// Fresnel with roughness for ambient specular
float3 F_SchlickRoughness(float NdotV, float3 F0, float roughness)
{
    float3 oneMinusRoughness = 1.0 - roughness;
    return F0 + (max(oneMinusRoughness, F0) - F0) * pow(1.0 - NdotV, 5.0);
}

// =============================================================================
// Full Cook-Torrance Specular BRDF
// =============================================================================

float3 CookTorranceSpecular(
    float3 N,      // Surface normal
    float3 V,      // View direction
    float3 L,      // Light direction
    float3 F0,     // Base reflectance at normal incidence
    float perceptualRoughness)
{
    float3 H = normalize(V + L);
    
    float NdotV = max(dot(N, V), 0.001);
    float NdotL = max(dot(N, L), 0.0);
    float NdotH = max(dot(N, H), 0.0);
    float VdotH = max(dot(V, H), 0.0);
    
    // Use the same alpha parameterization for the GGX distribution and
    // height-correlated visibility terms.
    float alphaRoughness =
        PerceptualRoughnessToAlpha(perceptualRoughness);
    float D = D_GGX(NdotH, alphaRoughness);
    float G = G_SmithGGXCorrelated(NdotV, NdotL, alphaRoughness);
    float3 F = F_Schlick(VdotH, F0);
    
    // Combine: (D * G * F) / (4 * NdotV * NdotL)
    // Note: G_SmithGGXCorrelated already includes the 1/(4*NdotV*NdotL) factor
    return D * G * F;
}

float3 CookTorranceSpecularWithF90(
    float3 N,
    float3 V,
    float3 L,
    float3 F0,
    float3 F90,
    float perceptualRoughness)
{
    const float3 H = normalize(V + L);
    const float NdotV = max(dot(N, V), 0.001);
    const float NdotL = max(dot(N, L), 0.0);
    const float NdotH = max(dot(N, H), 0.0);
    const float VdotH = max(dot(V, H), 0.0);
    const float alphaRoughness =
        PerceptualRoughnessToAlpha(perceptualRoughness);
    const float D = D_GGX(NdotH, alphaRoughness);
    const float G = G_SmithGGXCorrelated(NdotV, NdotL, alphaRoughness);
    return D * G * F_SchlickWithF90(VdotH, F0, F90);
}

// =============================================================================
// Lambertian Diffuse BRDF
// =============================================================================

float3 LambertianDiffuse(float3 albedo)
{
    return albedo * INV_PI;
}

// =============================================================================
// Disney Diffuse BRDF (more accurate but more expensive)
// =============================================================================

float3 DisneyDiffuse(float3 albedo, float NdotV, float NdotL, float VdotH, float roughness)
{
    float FD90 = 0.5 + 2.0 * VdotH * VdotH * roughness;
    float FdV = 1.0 + (FD90 - 1.0) * pow(1.0 - NdotV, 5.0);
    float FdL = 1.0 + (FD90 - 1.0) * pow(1.0 - NdotL, 5.0);
    return albedo * INV_PI * FdV * FdL;
}

// =============================================================================
// F0 (Base Reflectance) Computation
// =============================================================================

// Compute F0 from IOR (Index of Refraction)
float3 F0FromIOR(float ior)
{
    float f = (ior - 1.0) / (ior + 1.0);
    return float3(f * f, f * f, f * f);
}

// Compute F0 for metallic workflow
// For dielectrics, F0 is typically 0.04 (4% reflectance)
// For metals, F0 equals the albedo color
float3 ComputeF0(float3 albedo, float metallic)
{
    float3 dielectricF0 = float3(0.04, 0.04, 0.04);
    return lerp(dielectricF0, albedo, metallic);
}

float3 F_SchlickRoughnessWithF90(
    float NdotV,
    float3 F0,
    float3 F90,
    float roughness)
{
    const float3 roughF90 = max((1.0 - roughness) * F90, F0);
    return F0 + (roughF90 - F0) * pow(1.0 - NdotV, 5.0);
}

/** KHR_materials_specular dielectric F0, preserving the metallic endpoint. */
float3 ComputeF0WithSpecular(
    float3 albedo,
    float metallic,
    float indexOfRefraction,
    float specularFactor,
    float3 specularColor)
{
    const float3 dielectricF0 = min(
        F0FromIOR(max(indexOfRefraction, 1.0)) *
            max(specularColor, 0.0),
        1.0) * saturate(specularFactor);
    return lerp(dielectricF0, albedo, saturate(metallic));
}

float3 ComputeSpecularF90(float metallic, float specularFactor)
{
    return lerp(
        saturate(specularFactor).xxx,
        1.0.xxx,
        saturate(metallic));
}

/** Fresnel energy taken by KHR_materials_clearcoat's dielectric top layer. */
float3 EvaluateClearcoatFresnel(float3 normal, float3 viewDir, float factor)
{
    const float nDotV = max(dot(normal, viewDir), 0.001);
    const float factorClamped = saturate(factor);
    return F_SchlickWithF90(
        nDotV,
        float3(0.04, 0.04, 0.04) * factorClamped,
        factorClamped.xxx);
}

/**
 * Layered metallic-roughness direct lighting. The clearcoat lobe uses its own
 * normal/roughness and attenuates the underlying diffuse, specular and
 * emissive layers through its view Fresnel, so enabling it cannot create
 * unbounded energy.
 */
float3 EvaluatePBRWithExtensions(
    float3 N,
    float3 V,
    float3 L,
    float3 albedo,
    float metallic,
    float roughness,
    float indexOfRefraction,
    float specularFactor,
    float3 specularColor,
    float clearcoatFactor,
    float clearcoatRoughness,
    float3 clearcoatNormal,
    float3 lightColor,
    float lightAtten)
{
    const float nDotL = max(dot(N, L), 0.0);
    if (nDotL <= 0.0)
    {
        return 0.0;
    }

    const float3 H = normalize(V + L);
    const float vDotH = max(dot(V, H), 0.0);
    const float3 f0 = ComputeF0WithSpecular(
        albedo, metallic, indexOfRefraction, specularFactor, specularColor);
    const float3 fresnel = F_SchlickWithF90(
        vDotH, f0, ComputeSpecularF90(metallic, specularFactor));
    const float diffuseWeight = (1.0 - max(fresnel.x, max(fresnel.y, fresnel.z))) *
        (1.0 - metallic);
    const float3 diffuse = diffuseWeight *
        LambertianDiffuse(albedo);
    const float3 baseSpecular = CookTorranceSpecularWithF90(
        N, V, L, f0, ComputeSpecularF90(metallic, specularFactor), roughness);

    const float3 coatNormal = normalize(clearcoatNormal);
    const float coatNdotL = max(dot(coatNormal, L), 0.0);
    const float3 coatFresnel = EvaluateClearcoatFresnel(
        coatNormal, V, clearcoatFactor);
    const float3 coatSpecular = coatNdotL > 0.0
        ? CookTorranceSpecular(
              coatNormal,
              V,
              L,
              float3(0.04, 0.04, 0.04),
              clamp(clearcoatRoughness, 0.04, 1.0)) *
            saturate(clearcoatFactor)
        : 0.0;

    const float3 baseLayer = (diffuse + baseSpecular) * nDotL *
        (1.0 - coatFresnel);
    return (baseLayer + coatSpecular * coatNdotL) * lightColor * lightAtten;
}

// =============================================================================
// Complete PBR Lighting
// Combines diffuse and specular BRDF with energy conservation
// =============================================================================

float3 EvaluatePBR(
    float3 N,           // Surface normal
    float3 V,           // View direction
    float3 L,           // Light direction  
    float3 albedo,      // Base color
    float metallic,     // Metallic factor [0,1]
    float roughness,    // Roughness factor [0,1]
    float3 lightColor,  // Light color and intensity
    float lightAtten)   // Light attenuation
{
    float NdotL = max(dot(N, L), 0.0);
    
    // Early out if facing away from light
    if (NdotL <= 0.0)
        return float3(0.0, 0.0, 0.0);
    
    float3 H = normalize(V + L);
    float NdotV = max(dot(N, V), 0.001);
    float VdotH = max(dot(V, H), 0.0);
    
    // Compute F0 (base reflectance)
    float3 F0 = ComputeF0(albedo, metallic);
    
    // Fresnel term (used for energy conservation)
    float3 F = F_Schlick(VdotH, F0);
    
    // Specular BRDF
    float3 specular = CookTorranceSpecular(N, V, L, F0, roughness);
    
    // Diffuse BRDF (only non-metals have diffuse)
    float3 kS = F;  // Specular reflection ratio
    float3 kD = (1.0 - kS) * (1.0 - metallic);  // Diffuse ratio
    float3 diffuse = kD * LambertianDiffuse(albedo);
    
    // Combine and apply lighting
    return (diffuse + specular) * lightColor * lightAtten * NdotL;
}

#endif // BRDF_HLSLI
