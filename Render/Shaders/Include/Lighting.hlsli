// =============================================================================
// Lighting.hlsli - Light structures and utilities
// =============================================================================
//
// Defines GPU-side light structures and lighting helper functions.
// Matches the CPU-side LightManager structures.
//
// =============================================================================

#ifndef LIGHTING_HLSLI
#define LIGHTING_HLSLI

#include "BRDF.hlsli"

// =============================================================================
// Light Structures
// =============================================================================

struct DirectionalLight
{
    float3 direction;
    float intensity;
    float3 color;
    int shadowMapIndex;     // -1 if no shadow
    float4x4 lightSpaceMatrix;
};

struct PointLight
{
    float3 position;
    float range;
    float3 color;
    float intensity;
};

struct SpotLight
{
    float3 position;
    float range;
    float3 direction;
    float innerConeAngle;   // cos(inner angle)
    float3 color;
    float outerConeAngle;   // cos(outer angle)
    float intensity;
    int shadowMapIndex;
    float2 padding;
};

// =============================================================================
// Attenuation Functions
// =============================================================================

// Inverse square falloff with range clamping
float AttenuatePoint(float distance, float range)
{
    float attenuation = 1.0 / (distance * distance + 1.0);
    
    // Smooth falloff at range edge
    float factor = distance / range;
    float smoothFactor = saturate(1.0 - factor * factor);
    smoothFactor = smoothFactor * smoothFactor;
    
    return attenuation * smoothFactor;
}

// Spotlight cone attenuation
float AttenuateSpot(float3 lightDir, float3 spotDir, float innerCos, float outerCos)
{
    float theta = dot(lightDir, spotDir);
    float epsilon = innerCos - outerCos;
    return saturate((theta - outerCos) / epsilon);
}

// =============================================================================
// Directional Light Evaluation
// =============================================================================

float3 EvaluateDirectionalLight(
    DirectionalLight light,
    float3 N,
    float3 V,
    float3 worldPos,
    float3 albedo,
    float metallic,
    float roughness,
    float shadow)
{
    float3 L = -normalize(light.direction);
    float3 lightColor = light.color * light.intensity;
    
    return EvaluatePBR(N, V, L, albedo, metallic, roughness, lightColor, shadow);
}

// =============================================================================
// Point Light Evaluation
// =============================================================================

float3 EvaluatePointLight(
    PointLight light,
    float3 N,
    float3 V,
    float3 worldPos,
    float3 albedo,
    float metallic,
    float roughness)
{
    float3 lightVec = light.position - worldPos;
    float distance = length(lightVec);
    
    // Early out if beyond range
    if (distance > light.range)
        return float3(0.0, 0.0, 0.0);
    
    float3 L = lightVec / distance;
    float attenuation = AttenuatePoint(distance, light.range);
    float3 lightColor = light.color * light.intensity;
    
    return EvaluatePBR(N, V, L, albedo, metallic, roughness, lightColor, attenuation);
}

// =============================================================================
// Spot Light Evaluation
// =============================================================================

float3 EvaluateSpotLight(
    SpotLight light,
    float3 N,
    float3 V,
    float3 worldPos,
    float3 albedo,
    float metallic,
    float roughness,
    float shadow)
{
    float3 lightVec = light.position - worldPos;
    float distance = length(lightVec);
    
    // Early out if beyond range
    if (distance > light.range)
        return float3(0.0, 0.0, 0.0);
    
    float3 L = lightVec / distance;
    
    // Cone attenuation
    float coneAtten = AttenuateSpot(-L, normalize(light.direction), 
                                    light.innerConeAngle, light.outerConeAngle);
    if (coneAtten <= 0.0)
        return float3(0.0, 0.0, 0.0);
    
    float distAtten = AttenuatePoint(distance, light.range);
    float attenuation = distAtten * coneAtten * shadow;
    float3 lightColor = light.color * light.intensity;
    
    return EvaluatePBR(N, V, L, albedo, metallic, roughness, lightColor, attenuation);
}

// =============================================================================
// Ambient / Environment Lighting
// =============================================================================

// Simple ambient approximation (no IBL)
float3 AmbientLighting(
    float3 N,
    float3 V,
    float3 albedo,
    float metallic,
    float roughness,
    float3 ambientColor,
    float ao)
{
    float3 F0 = ComputeF0(albedo, metallic);
    float NdotV = max(dot(N, V), 0.001);
    
    // Approximate ambient specular using Fresnel
    float3 F = F_SchlickRoughness(NdotV, F0, roughness);
    float3 kS = F;
    float3 kD = (1.0 - kS) * (1.0 - metallic);
    
    // Simple diffuse ambient
    float3 diffuseAmbient = kD * albedo * ambientColor;
    
    // Simple specular ambient (proper IBL would sample a pre-filtered environment map)
    float3 specularAmbient = F * ambientColor * 0.3;
    
    return (diffuseAmbient + specularAmbient) * ao;
}

// =============================================================================
// Shadow Mapping Utilities
// =============================================================================

// Standard PCF shadow sampling
float SampleShadowPCF(
    Texture2DArray shadowMap,
    SamplerComparisonState shadowSampler,
    float3 shadowCoord,
    int cascadeIndex,
    float bias)
{
    float shadow = 0.0;
    float2 texelSize = 1.0 / float2(2048.0, 2048.0);  // Assuming 2048x2048 shadow map
    
    // 3x3 PCF kernel
    for (int x = -1; x <= 1; x++)
    {
        for (int y = -1; y <= 1; y++)
        {
            float2 offset = float2(x, y) * texelSize;
            shadow += shadowMap.SampleCmpLevelZero(
                shadowSampler,
                float3(shadowCoord.xy + offset, cascadeIndex),
                shadowCoord.z - bias
            );
        }
    }
    
    return shadow / 9.0;
}

// Get cascade index based on view depth
int GetCascadeIndex(float viewDepth, float4 cascadeSplits)
{
    int cascade = 0;
    if (viewDepth > cascadeSplits.x) cascade = 1;
    if (viewDepth > cascadeSplits.y) cascade = 2;
    if (viewDepth > cascadeSplits.z) cascade = 3;
    return cascade;
}

#endif // LIGHTING_HLSLI
