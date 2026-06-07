// Procedural skybox minimum path.
// Draws a fullscreen triangle and lets read-only depth keep foreground geometry.

cbuffer SkyboxConstants : register(b0, space0)
{
    float4 SkyboxZenithColor;    // rgb: zenith color, a: exposure
    float4 SkyboxHorizonColor;   // rgb: horizon color, a: scattering intensity
    float4 SkyboxGroundColor;    // rgb: ground color, a: far-depth value
    float4 SkyboxSunDirection;   // xyz: sun direction, w: sun intensity
    float4 SkyboxSunColor;       // rgb: sun color, a: reserved
};

struct VSOutput
{
    float4 position : SV_POSITION;
    float2 ndc : TEXCOORD0;
};

VSOutput VSMain(uint vertexId : SV_VertexID)
{
    static const float2 positions[3] =
    {
        float2(-1.0, -1.0),
        float2(-1.0,  3.0),
        float2( 3.0, -1.0)
    };

    VSOutput output;
    output.ndc = positions[vertexId];
    output.position = float4(output.ndc, SkyboxGroundColor.a, 1.0);
    return output;
}

float4 PSMain(VSOutput input) : SV_TARGET
{
    const float height = saturate(input.ndc.y * 0.5 + 0.5);
    const float skyBlend = smoothstep(0.0, 1.0, height);
    const float groundBlend = saturate((0.5 - height) * 2.0);

    float3 color = lerp(SkyboxHorizonColor.rgb, SkyboxZenithColor.rgb, skyBlend);
    color = lerp(color, SkyboxGroundColor.rgb, groundBlend);

    const float3 viewDir = normalize(float3(input.ndc.x, input.ndc.y, 1.0));
    const float3 sunDir = normalize(SkyboxSunDirection.xyz);
    const float sunGlow = pow(saturate(dot(viewDir, sunDir)), 48.0) * SkyboxSunDirection.w;
    color += SkyboxSunColor.rgb * sunGlow * SkyboxHorizonColor.a;

    color *= SkyboxZenithColor.a;
    return float4(color, 1.0);
}
