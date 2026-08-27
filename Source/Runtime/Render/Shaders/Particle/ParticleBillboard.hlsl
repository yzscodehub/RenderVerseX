// Render-owned CPU particle billboard and distortion-composite programs.
// One narrow descriptor set owns the frame/instance data, exact flipbook SRV,
// soft-depth snapshot SRV and sampler.  The snapshot is never the read-only
// DSV used for depth testing, which keeps DSV/SRV hazards out of the graph.

#include "../Include/FullscreenTriangle.hlsli"

cbuffer ParticleBillboardFrameConstants : register(b0, space0)
{
    float4x4 ViewProjection;
    float4x4 InverseProjection;
    float4 CameraRight;
    float4 CameraUp;
    float4 InverseExtentAndReverseZ;
    float4 ViewportOriginAndTargetExtent;
};

struct ParticleBillboardInstance
{
    float4 PositionAndRotation;
    float4 Color;
    float4 SizeAgeAndLifetime;
    float4 AtlasUVOffsetScale;
    float4 SoftDepthDistortionReactive;
    uint BlendMode;
    uint Flags;
    uint FlipbookFrameIndex;
    uint PackedAtlasTiles;
};

StructuredBuffer<ParticleBillboardInstance> gParticles : register(t1, space0);
Texture2D gParticleSprite : register(t2, space0);
// t3 is a D32 snapshot for particle passes and an RG16F distortion field for
// the fullscreen composite. Keep the declaration untyped because both are
// sampled through the same narrow descriptor layout.
Texture2D gAuxiliaryTexture : register(t3, space0);
SamplerState gLinearClamp : register(s4, space0);

struct VSOutput
{
    float4 Position : SV_Position;
    float4 Color : COLOR0;
    float2 UV : TEXCOORD0;
    nointerpolation uint BlendMode : TEXCOORD1;
    nointerpolation uint Flags : TEXCOORD2;
    nointerpolation uint FlipbookFrameIndex : TEXCOORD3;
    nointerpolation uint PackedAtlasTiles : TEXCOORD4;
    nointerpolation float4 AtlasUVOffsetScale : TEXCOORD5;
    nointerpolation float4 SoftDepthDistortionReactive : TEXCOORD6;
};

float2 GetQuadCorner(uint vertexId)
{
    // Two clockwise triangles. The rasterizer is explicitly no-cull.
    static const float2 Corners[6] = {
        float2(-0.5f, -0.5f), float2(0.5f, -0.5f), float2(0.5f, 0.5f),
        float2(-0.5f, -0.5f), float2(0.5f, 0.5f), float2(-0.5f, 0.5f)
    };
    return Corners[vertexId];
}

VSOutput VSMain(uint vertexId : SV_VertexID, uint instanceId : SV_InstanceID)
{
    const ParticleBillboardInstance particle = gParticles[instanceId];
    const float2 corner = GetQuadCorner(vertexId);
    const float angle = particle.PositionAndRotation.w;
    const float sine = sin(angle);
    const float cosine = cos(angle);
    const float2 rotatedCorner = float2(
        corner.x * cosine - corner.y * sine,
        corner.x * sine + corner.y * cosine);
    const float2 size = particle.SizeAgeAndLifetime.xy;
    const float3 worldPosition = particle.PositionAndRotation.xyz +
        CameraRight.xyz * (rotatedCorner.x * size.x) +
        CameraUp.xyz * (rotatedCorner.y * size.y);

    VSOutput output;
    output.Position = mul(ViewProjection, float4(worldPosition, 1.0f));
    output.Color = particle.Color;
    output.UV = corner + 0.5f;
    output.BlendMode = particle.BlendMode;
    output.Flags = particle.Flags;
    output.FlipbookFrameIndex = particle.FlipbookFrameIndex;
    output.PackedAtlasTiles = particle.PackedAtlasTiles;
    output.AtlasUVOffsetScale = particle.AtlasUVOffsetScale;
    output.SoftDepthDistortionReactive = particle.SoftDepthDistortionReactive;
    return output;
}

float ReconstructViewLinearDepth(float2 uv, float deviceDepth)
{
    // Render targets use top-left UVs while clip space is bottom-left NDC.
    // This is the same convention used by the RT reconstruction programs.
    const float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    const float4 viewPosition = mul(
        InverseProjection, float4(ndc, deviceDepth, 1.0f));
    const float reciprocalW = rcp(max(abs(viewPosition.w), 1.0e-6f));
    return abs(viewPosition.z * reciprocalW);
}

float GetParticleSoftFade(VSOutput input)
{
    const bool softDepthEnabled = (input.Flags & 0x2u) != 0u;
    if (!softDepthEnabled)
    {
        return 1.0f;
    }
    // D32 scene depth is an edge/discontinuity signal. Fetch the exact covered
    // integer texel instead of linearly filtering across foreground edges.
    // GFX17 currently rejects offset viewports at record time; retaining the
    // origin in the contract makes that restriction explicit and keeps the
    // shader correct when support is later widened.
    const float2 localPixel = input.Position.xy -
        ViewportOriginAndTargetExtent.xy;
    const float2 uv = localPixel * InverseExtentAndReverseZ.xy;
    const uint2 targetExtent = max(
        uint2(ViewportOriginAndTargetExtent.zw), uint2(1u, 1u));
    const uint2 scenePixel = min(
        uint2(max(localPixel, float2(0.0f, 0.0f))), targetExtent - 1u);
    const float sceneDeviceDepth = gAuxiliaryTexture.Load(
        int3(scenePixel, 0)).r;
    const float sceneLinearDepth = ReconstructViewLinearDepth(uv, sceneDeviceDepth);
    const float particleLinearDepth = ReconstructViewLinearDepth(
        uv, input.Position.z);
    // Both values are positive view-space meters, independent of the device
    // depth encoding (including reverse-Z). Only opaque geometry in front of
    // the billboard attenuates the particle.
    const float fade = saturate((sceneLinearDepth - particleLinearDepth) /
        max(input.SoftDepthDistortionReactive.x, 0.0001f));
    return pow(fade, max(input.SoftDepthDistortionReactive.y, 0.0001f));
}

float2 GetParticleUV(VSOutput input)
{
    const bool hasSprite = (input.Flags & 0x1u) != 0u;
    if (!hasSprite)
    {
        return input.UV;
    }
    const uint tilesX = max(input.PackedAtlasTiles & 0xffffu, 1u);
    const uint tilesY = max((input.PackedAtlasTiles >> 16u) & 0xffffu, 1u);
    const uint tileX = input.FlipbookFrameIndex % tilesX;
    const uint tileY = input.FlipbookFrameIndex / tilesX;
    const float2 atlasUV = (input.UV + float2(tileX, tileY)) /
        float2(tilesX, tilesY);
    return atlasUV * input.AtlasUVOffsetScale.zw +
        input.AtlasUVOffsetScale.xy;
}

float4 EvaluateParticleColor(VSOutput input, out float outRadialFade)
{
    const float2 centered = input.UV * 2.0f - 1.0f;
    outRadialFade = saturate(1.0f - dot(centered, centered));
    const bool hasSprite = (input.Flags & 0x1u) != 0u;
    const float4 sprite = hasSprite
        ? gParticleSprite.Sample(gLinearClamp, GetParticleUV(input))
        : float4(1.0f, 1.0f, 1.0f, 1.0f);
    const float softFade = GetParticleSoftFade(input);
    // Cooked sprite colors are regular (straight-alpha) samples. AlphaBlend
    // lets the blend state apply finalAlpha; premultiplied smoke carries that
    // alpha in RGB; additive fire uses it as HDR energy. Therefore a fully
    // transparent atlas texel never contributes color to any blend mode.
    const float attenuation = outRadialFade * softFade;
    const float finalAlpha = saturate(input.Color.a * sprite.a * attenuation);
    const float3 straightRgb = input.Color.rgb * sprite.rgb;
    const bool premultipliedOrAdditive = input.BlendMode == 1u ||
        input.BlendMode == 2u;
    const float3 color = premultipliedOrAdditive
        ? straightRgb * finalAlpha
        : straightRgb;
    return float4(color, finalAlpha);
}

struct ParticleColorTargets
{
    float4 Color : SV_Target0;
    float Reactive : SV_Target1;
};

ParticleColorTargets PSMain(VSOutput input)
{
    float radialFade = 0.0f;
    ParticleColorTargets output;
    output.Color = EvaluateParticleColor(input, radialFade);
    output.Reactive = saturate(
        input.SoftDepthDistortionReactive.w * output.Color.a);
    return output;
}

float2 PSDistortionMain(VSOutput input) : SV_Target0
{
    float radialFade = 0.0f;
    const float4 color = EvaluateParticleColor(input, radialFade);
    const float2 direction = normalize((input.UV - 0.5f) + float2(0.0001f, 0.0f));
    const float displacementPixels = min(
        input.SoftDepthDistortionReactive.z, 64.0f) * color.a;
    return direction * displacementPixels;
}

struct DistortionCompositeVSOutput
{
    float4 Position : SV_Position;
    float2 UV : TEXCOORD0;
};

DistortionCompositeVSOutput DistortionCompositeVS(uint vertexId : SV_VertexID)
{
    DistortionCompositeVSOutput output;
    output.UV = RVX_GetFullscreenTriangleTexCoord(vertexId);
    output.Position = RVX_GetFullscreenTrianglePosition(output.UV);
    return output;
}

float4 DistortionCompositePS(DistortionCompositeVSOutput input) : SV_Target0
{
    const float2 displacementPixels = gAuxiliaryTexture.Sample(
        gLinearClamp, input.UV).xy;
    const float2 sourceUV = saturate(input.UV +
        displacementPixels * InverseExtentAndReverseZ.xy);
    return gParticleSprite.Sample(gLinearClamp, sourceUV);
}
