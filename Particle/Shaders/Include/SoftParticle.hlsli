/**
 * @file SoftParticle.hlsli
 * @brief Soft particle depth fade implementation
 */

#ifndef SOFT_PARTICLE_HLSLI
#define SOFT_PARTICLE_HLSLI

// Scene depth texture and sampler (must be bound)
Texture2D<float> g_SceneDepth : register(t1);
SamplerState g_DepthSampler : register(s1);

// Soft particle parameters (from RenderData)
cbuffer SoftParticleParams : register(b2)
{
    float g_FadeDistance;
    float g_ContrastPower;
    float2 g_InvScreenSize;
};

/**
 * Convert NDC depth to linear view-space depth
 * Assumes reverse-Z depth buffer (1 near, 0 far)
 */
float NDCToViewDepth(float ndcDepth, float nearPlane, float farPlane)
{
    // Reverse-Z projection
    return nearPlane * farPlane / (farPlane - ndcDepth * (farPlane - nearPlane));
}

/**
 * Compute soft particle fade factor
 * @param clipPos Particle clip-space position (from vertex shader)
 * @param particleViewZ Particle view-space Z depth
 * @param nearPlane Camera near plane
 * @param farPlane Camera far plane
 * @return Fade factor (0 = fully faded, 1 = fully visible)
 */
float ComputeSoftParticleFade(float4 clipPos, float particleViewZ, float nearPlane, float farPlane)
{
    // Calculate screen UV
    float2 screenUV = clipPos.xy / clipPos.w * 0.5 + 0.5;
    screenUV.y = 1.0 - screenUV.y;  // Flip Y if needed
    
    // Sample scene depth
    float sceneDepthNDC = g_SceneDepth.Sample(g_DepthSampler, screenUV);
    
    // Convert to view-space depth
    float sceneDepthVS = NDCToViewDepth(sceneDepthNDC, nearPlane, farPlane);
    
    // Compute depth difference
    float depthDiff = sceneDepthVS - particleViewZ;
    
    // Apply fade
    float fade = saturate(depthDiff / g_FadeDistance);
    
    // Apply contrast
    fade = pow(fade, g_ContrastPower);
    
    return fade;
}

/**
 * Simplified soft particle fade (assumes depth is already linear)
 */
float ComputeSoftParticleFadeSimple(float2 screenUV, float particleDepth, float fadeDistance)
{
    float sceneDepth = g_SceneDepth.Sample(g_DepthSampler, screenUV);
    float depthDiff = sceneDepth - particleDepth;
    return saturate(depthDiff / fadeDistance);
}

#endif // SOFT_PARTICLE_HLSLI
