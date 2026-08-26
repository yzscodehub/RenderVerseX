// Render-owned CPU particle billboard raster program.
// Set 0 is deliberately limited to b0 + t1 so its ABI remains portable across
// DX11, DX12, Vulkan, Metal, and OpenGL cooked program providers.

cbuffer ParticleBillboardFrameConstants : register(b0, space0)
{
    float4x4 ViewProjection;
    float4 CameraRight;
    float4 CameraUp;
};

struct ParticleBillboardInstance
{
    float4 PositionAndRotation;
    float4 Color;
    float4 SizeAgeAndLifetime;
    uint BlendMode;
    uint Flags;
    uint StableParticleId;
    uint Padding;
};

StructuredBuffer<ParticleBillboardInstance> gParticles : register(t1, space0);

struct VSOutput
{
    float4 Position : SV_Position;
    float4 Color : COLOR0;
    float2 UV : TEXCOORD0;
    nointerpolation uint BlendMode : TEXCOORD1;
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
    return output;
}

float4 PSMain(VSOutput input) : SV_Target0
{
    // GFX16 intentionally has no sprite texture, soft depth, distortion, or
    // flipbook sampling. Those are introduced by the GFX17 fire/smoke feature.
    const float2 centered = input.UV * 2.0f - 1.0f;
    const float radialFade = saturate(1.0f - dot(centered, centered));
    // AlphaBlend uses source alpha as its RGB blend factor. Premultiplied and
    // additive batches use a unit RGB factor, so their source RGB must carry
    // the radial attenuation explicitly.
    const bool premultipliedOrAdditive = input.BlendMode == 1u ||
        input.BlendMode == 2u;
    const float3 color = premultipliedOrAdditive
        ? input.Color.rgb * radialFade : input.Color.rgb;
    return float4(color, input.Color.a * radialFade);
}
