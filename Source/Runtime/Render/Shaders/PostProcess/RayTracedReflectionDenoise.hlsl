// =============================================================================
// RayTracedReflectionDenoise.hlsl
// =============================================================================
//
// Edge-aware spatial denoise for ray-traced reflection results.
// Input RGB is the reflection signal; input A is confidence/strength.
//
// Descriptor layout: b0 constants, t1 reflection texture, t2 scene depth, t3 encoded normal guide.
//
// =============================================================================

#include "../Include/FullscreenTriangle.hlsli"

cbuffer RayTracedReflectionDenoiseConstants : register(b0, space0)
{
    float4 OutputSizeAndInvSize;
    float4 SceneDepthSizeAndInvSize;
    float4 DenoiseParams;
    float4 DenoiseQualityParams;
};

Texture2D<float4> ReflectionTexture : register(t1, space0);
Texture2D<float> SceneDepthTexture : register(t2, space0);
Texture2D<float4> NormalGuideTexture : register(t3, space0);

struct VSOutput
{
    float4 Position : SV_POSITION;
    float2 TexCoord : TEXCOORD0;
};

VSOutput VSMain(uint vertexID : SV_VertexID)
{
    VSOutput output;
    output.TexCoord = RVX_GetFullscreenTriangleTexCoord(vertexID);
    output.Position = RVX_GetFullscreenTrianglePosition(output.TexCoord);
    return output;
}

float DepthWeight(float sampleDepth, float centerDepth, float depthSigma)
{
    const float depthDelta = abs(sampleDepth - centerDepth);
    return exp(-depthDelta / max(depthSigma, 1.0e-5));
}

bool DecodeNormalGuide(float4 encoded, out float3 normal)
{
    normal = encoded.xyz * 2.0 - 1.0;
    return encoded.w > 0.5 && all(isfinite(normal)) && dot(normal, normal) > 0.5;
}

float NormalWeight(float4 encodedSampleNormal, float3 centerNormal, bool centerValid, float normalThreshold)
{
    float3 sampleNormal;
    if (!centerValid || !DecodeNormalGuide(encodedSampleNormal, sampleNormal))
    {
        return 1.0;
    }

    const float normalDot = saturate(dot(normalize(sampleNormal), normalize(centerNormal)));
    const float threshold = saturate(normalThreshold);
    return saturate((normalDot - threshold) / max(1.0 - threshold, 1.0e-3));
}

int2 MapOutputPixelToSceneDepthPixel(int2 outputPixel)
{
    const int2 sceneSize = max(int2(SceneDepthSizeAndInvSize.xy), int2(1, 1));
    const float2 uv = (float2(outputPixel) + 0.5) * OutputSizeAndInvSize.zw;
    return clamp(int2(uv * SceneDepthSizeAndInvSize.xy), int2(0, 0), sceneSize - 1);
}

float4 PSMain(VSOutput input) : SV_TARGET
{
    const int2 outputSize = max(int2(OutputSizeAndInvSize.xy), int2(1, 1));
    const int2 pixel = clamp(int2(input.Position.xy), int2(0, 0), outputSize - 1);
    const float4 centerReflection = ReflectionTexture.Load(int3(pixel, 0));
    const float centerDepth = SceneDepthTexture.Load(int3(MapOutputPixelToSceneDepthPixel(pixel), 0));
    float3 centerNormal = 0.0;
    const bool centerNormalValid = DecodeNormalGuide(NormalGuideTexture.Load(int3(pixel, 0)), centerNormal);

    const int radius = clamp((int)round(DenoiseParams.x), 0, 3);
    const float depthSigma = max(DenoiseParams.y, 1.0e-5);
    const float normalThreshold = saturate(DenoiseParams.z);
    const float confidencePower = max(DenoiseParams.w, 0.01);
    const float centerPreservation = max(DenoiseQualityParams.x, 0.0);
    const float lowConfidenceDepthScale = max(DenoiseQualityParams.y, 1.0);
    const float centerConfidence = pow(saturate(centerReflection.a), confidencePower);
    const float adaptiveDepthSigma = depthSigma * lerp(lowConfidenceDepthScale, 1.0, centerConfidence);

    if (radius == 0 || centerReflection.a <= 0.0)
    {
        return float4(max(centerReflection.rgb, 0.0), saturate(centerReflection.a));
    }

    float3 weightedReflection = 0.0;
    float weightedAlpha = 0.0;
    float weightSum = 0.0;

    [unroll]
    for (int y = -3; y <= 3; ++y)
    {
        [unroll]
        for (int x = -3; x <= 3; ++x)
        {
            const int2 offset = int2(x, y);
            if (abs(x) > radius || abs(y) > radius)
                continue;

            const int2 samplePixel = clamp(pixel + offset, int2(0, 0), outputSize - 1);
            const float4 sampleReflection = ReflectionTexture.Load(int3(samplePixel, 0));
            const float sampleDepth =
                SceneDepthTexture.Load(int3(MapOutputPixelToSceneDepthPixel(samplePixel), 0));
            const float4 sampleNormal = NormalGuideTexture.Load(int3(samplePixel, 0));

            const float spatialWeight = 1.0 / (1.0 + dot(float2(offset), float2(offset)));
            const float edgeWeight = DepthWeight(sampleDepth, centerDepth, adaptiveDepthSigma);
            const float normalWeight = NormalWeight(sampleNormal, centerNormal, centerNormalValid, normalThreshold);
            const float confidenceWeight = max(pow(saturate(sampleReflection.a), confidencePower), 0.05);
            const float weight = spatialWeight * edgeWeight * normalWeight * confidenceWeight;

            weightedReflection += sampleReflection.rgb * weight;
            weightedAlpha += sampleReflection.a * weight;
            weightSum += weight;
        }
    }

    const float centerWeight = centerPreservation * max(centerConfidence, 0.05);
    weightedReflection += centerReflection.rgb * centerWeight;
    weightedAlpha += centerReflection.a * centerWeight;
    weightSum += centerWeight;

    if (weightSum <= 0.0)
    {
        return float4(max(centerReflection.rgb, 0.0), saturate(centerReflection.a));
    }

    return float4(max(weightedReflection / weightSum, 0.0), saturate(weightedAlpha / weightSum));
}
