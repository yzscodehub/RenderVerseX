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

// =============================================================================
// Normal Distribution Function (D)
// GGX/Trowbridge-Reitz distribution
// =============================================================================

float D_GGX(float NdotH, float roughness)
{
    float a = roughness * roughness;
    float a2 = a * a;
    float NdotH2 = NdotH * NdotH;
    
    float denom = NdotH2 * (a2 - 1.0) + 1.0;
    denom = PI * denom * denom;
    
    return a2 / max(denom, EPSILON);
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
float G_SmithGGXCorrelated(float NdotV, float NdotL, float roughness)
{
    float a2 = roughness * roughness;
    float GGXV = NdotL * sqrt(NdotV * NdotV * (1.0 - a2) + a2);
    float GGXL = NdotV * sqrt(NdotL * NdotL * (1.0 - a2) + a2);
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
    float roughness)
{
    float3 H = normalize(V + L);
    
    float NdotV = max(dot(N, V), 0.001);
    float NdotL = max(dot(N, L), 0.0);
    float NdotH = max(dot(N, H), 0.0);
    float VdotH = max(dot(V, H), 0.0);
    
    // Specular BRDF components
    float D = D_GGX(NdotH, roughness);
    float G = G_SmithGGXCorrelated(NdotV, NdotL, roughness);
    float3 F = F_Schlick(VdotH, F0);
    
    // Combine: (D * G * F) / (4 * NdotV * NdotL)
    // Note: G_SmithGGXCorrelated already includes the 1/(4*NdotV*NdotL) factor
    return D * G * F;
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
