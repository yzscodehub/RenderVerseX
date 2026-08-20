/**
 * @file SoftParticle.hlsli
 * @brief Soft particle depth fade implementation
 */

#ifndef SOFT_PARTICLE_HLSLI
#define SOFT_PARTICLE_HLSLI

// Scene depth texture and sampler (must be bound when sceneDepthTestEnabled != 0)
Texture2D<float> g_SceneDepth : register(t5);
SamplerState g_DepthSampler : register(s6);

/**
 * Convert NDC depth to linear view-space depth
 * Assumes reverse-Z depth buffer (1 near, 0 far)
 */
float NDCToViewDepth(float ndcDepth, float nearPlane, float farPlane, uint reverseZ)
{
    float depth = saturate(ndcDepth);
    float depthRange = max(farPlane - nearPlane, 0.000001);
    float denominator = reverseZ != 0
        ? nearPlane + depth * depthRange
        : farPlane - depth * depthRange;
    return nearPlane * farPlane / max(denominator, 0.000001);
}

/**
 * Compute soft particle fade factor
 * @param clipPos Particle clip-space position (from vertex shader)
 * @param particleViewZ Particle view-space Z depth
 * @param nearPlane Camera near plane
 * @param farPlane Camera far plane
 * @return Fade factor (0 = fully faded, 1 = fully visible)
 */
float2 ComputeParticleScreenUV(float4 clipPos)
{
    float2 screenUV = clipPos.xy / clipPos.w * 0.5 + 0.5;
    screenUV.y = 1.0 - screenUV.y;  // Flip Y if needed
    return screenUV;
}

float SampleSceneViewDepth(float4 clipPos, float nearPlane, float farPlane, uint reverseZ)
{
    float2 screenUV = ComputeParticleScreenUV(clipPos);
    float sceneDepthNDC = g_SceneDepth.Sample(g_DepthSampler, screenUV);
    return NDCToViewDepth(sceneDepthNDC, nearPlane, farPlane, reverseZ);
}

float ComputeSoftParticleFade(float4 clipPos,
                              float particleViewZ,
                              float nearPlane,
                              float farPlane,
                              uint reverseZ,
                              float fadeDistance,
                              float contrastPower)
{
    // Sample scene depth
    float sceneDepthVS = SampleSceneViewDepth(clipPos, nearPlane, farPlane, reverseZ);
    float depthDiff = sceneDepthVS - particleViewZ;
    float fade = saturate(depthDiff / max(fadeDistance, 0.000001));
    fade = pow(fade, max(contrastPower, 0.000001));
    return fade;
}

#endif // SOFT_PARTICLE_HLSLI
