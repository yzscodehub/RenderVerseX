// Deterministic Gerstner water. All time comes from the frame-owned constant
// buffer: source assets never carry a mutable clock and the shader never reads
// wall time. The two sampled inputs are immutable graph copies, never active
// color/depth attachments.

cbuffer WaterSurfaceConstants : register(b0, space0)
{
    float4x4 ViewProjection;
    float4x4 InverseProjection;
    float4 CameraPositionAndPadding;
    float4 InverseExtentAndReverseZ;
    float4 SurfacePositionAndDepth;
    float4 SurfaceSizeAndTransparency;
    float4 ShallowColorAndRefraction;
    float4 DeepColorAndReflection;
    float4 FoamColorAndParameters;
    float4 AbsorptionAndNormalScale;
    float4 FoamAndFeatureParameters;
    // CPU-reduced independently for rates 0.07, -0.05, -0.11, 0.09.
    float4 DetailTemporalPhases;
    float4 WaveDirectionAmplitude[8];
    // x = authored steepness, y = CPU-quantized wave number,
    // z = CPU-reduced temporal phase, w = enabled.
    float4 WaveSteepnessTemporalPhase[8];
};

Texture2D gOpaqueSceneSnapshot : register(t1, space0);
Texture2D gWaterDepthSnapshot : register(t2, space0);
SamplerState gLinearClamp : register(s3, space0);

struct VSInput
{
    float2 LocalPosition : POSITION;
};

struct VSOutput
{
    float4 Position : SV_Position;
    float3 WorldPosition : TEXCOORD0;
    float3 WorldNormal : TEXCOORD1;
    float Crest : TEXCOORD2;
};

static const float RVX_WATER_TWO_PI = 6.28318530717958647692f;

void EvaluateGerstner(
    float2 baseXZ,
    out float3 outDisplacement,
    out float3 outTangentX,
    out float3 outTangentZ,
    out float outCrest)
{
    outDisplacement = float3(0.0f, 0.0f, 0.0f);
    outTangentX = float3(1.0f, 0.0f, 0.0f);
    outTangentZ = float3(0.0f, 0.0f, 1.0f);
    outCrest = 0.0f;
    [unroll]
    for (uint waveIndex = 0; waveIndex < 8u; ++waveIndex)
    {
        const float4 directionAmplitude = WaveDirectionAmplitude[waveIndex];
        const float4 steepnessTemporalPhase = WaveSteepnessTemporalPhase[waveIndex];
        const float enabled = steepnessTemporalPhase.w;
        // C++ owns both float64 derivation and this one float32 ABI boundary.
        // Recomputing k from wavelength here would make shader and CPU
        // references diverge by an implementation-dependent float division.
        const float k = max(steepnessTemporalPhase.y, 1.0e-6f);
        // C++ reduces phaseOffset - omega * frozenFrameTime per wave. The
        // shader receives no long-running raw float clock.
        const float phase = fmod(k * dot(directionAmplitude.xy, baseXZ) +
            steepnessTemporalPhase.z, RVX_WATER_TWO_PI);
        const float sine = sin(phase);
        const float cosine = cos(phase);
        // Packet validation bounds aggregate steepness at one, keeping
        // horizontal displacement bounded without a second wave-count scale.
        const float horizontalAmplitude = enabled * steepnessTemporalPhase.x / k;
        const float amplitude = enabled * directionAmplitude.z;
        const float2 horizontal = directionAmplitude.xy * horizontalAmplitude * cosine;
        outDisplacement.xz += horizontal;
        outDisplacement.y += amplitude * sine;
        outTangentX += float3(
            -directionAmplitude.x * directionAmplitude.x * steepnessTemporalPhase.x * sine,
            directionAmplitude.x * amplitude * k * cosine,
            -directionAmplitude.x * directionAmplitude.y * steepnessTemporalPhase.x * sine);
        outTangentZ += float3(
            -directionAmplitude.x * directionAmplitude.y * steepnessTemporalPhase.x * sine,
            directionAmplitude.y * amplitude * k * cosine,
            -directionAmplitude.y * directionAmplitude.y * steepnessTemporalPhase.x * sine);
        outCrest += max(sine * amplitude, 0.0f);
    }
}

float3 EvaluateDetailNormal(float3 worldPosition)
{
    // Each rate is reduced independently by C++, so the shader never derives
    // non-integral scroll rates from one periodic or unbounded time value.
    const float2 p0 = worldPosition.xz * 0.55f + DetailTemporalPhases.xy;
    const float2 p1 = worldPosition.xz * 1.37f + DetailTemporalPhases.zw;
    const float nx = sin(p0.x) * 0.55f + cos(p0.y) * 0.35f +
        sin(p1.x + p1.y) * 0.25f;
    const float nz = cos(p0.x) * 0.45f + sin(p0.y) * 0.45f +
        cos(p1.x - p1.y) * 0.20f;
    return normalize(float3(-nx * AbsorptionAndNormalScale.w, 1.0f,
                            -nz * AbsorptionAndNormalScale.w));
}

VSOutput VSMain(VSInput input)
{
    // Gerstner phase is surface local. Translating a water actor moves the
    // surface in world space without changing its animated wave pattern.
    const float2 localXZ = input.LocalPosition * SurfaceSizeAndTransparency.xy;
    const float2 worldXZ = localXZ + SurfacePositionAndDepth.xz;
    float3 displacement;
    float3 tangentX;
    float3 tangentZ;
    float crest;
    EvaluateGerstner(localXZ, displacement, tangentX, tangentZ, crest);
    VSOutput output;
    output.WorldPosition = float3(worldXZ.x, SurfacePositionAndDepth.y, worldXZ.y) +
        displacement;
    const float3 analyticNormal = normalize(cross(tangentZ, tangentX));
    output.WorldNormal = normalize(lerp(analyticNormal,
        EvaluateDetailNormal(output.WorldPosition), 0.35f));
    output.Crest = crest;
    output.Position = mul(ViewProjection, float4(output.WorldPosition, 1.0f));
    return output;
}

float ReconstructViewLinearDepth(float2 uv, float deviceDepth)
{
    const float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    const float4 viewPosition = mul(InverseProjection, float4(ndc, deviceDepth, 1.0f));
    return abs(viewPosition.z / max(abs(viewPosition.w), 1.0e-6f));
}

struct WaterTargets
{
    float4 Color : SV_Target0;
    float Reactive : SV_Target1;
};

WaterTargets PSMain(VSOutput input)
{
    const float2 uv = input.Position.xy * InverseExtentAndReverseZ.xy;
    const uint2 pixel = uint2(max(input.Position.xy, float2(0.0f, 0.0f)));
    const float sceneDeviceDepth = gWaterDepthSnapshot.Load(int3(pixel, 0)).r;
    const float sceneDepth = ReconstructViewLinearDepth(uv, sceneDeviceDepth);
    const float waterDepth = ReconstructViewLinearDepth(uv, input.Position.z);
    const float thickness = max(sceneDepth - waterDepth, 0.0f);
    const float3 transmission = exp(-AbsorptionAndNormalScale.xyz * thickness);
    const float2 refractOffset = input.WorldNormal.xz *
        ShallowColorAndRefraction.w * FoamAndFeatureParameters.w *
        18.0f * InverseExtentAndReverseZ.xy;
    const float3 refracted = gOpaqueSceneSnapshot.Sample(
        gLinearClamp, saturate(uv + refractOffset)).rgb;
    const float3 absorbedWater = lerp(DeepColorAndReflection.xyz,
        ShallowColorAndRefraction.xyz, saturate(thickness / max(
            SurfacePositionAndDepth.w, 0.001f)));
    const float3 throughWater = refracted * transmission +
        absorbedWater * (1.0f - transmission);
    const float3 viewDirection = normalize(
        CameraPositionAndPadding.xyz - input.WorldPosition);
    const float roughness = saturate(SurfaceSizeAndTransparency.w);
    const float schlickFresnel = 0.02f + 0.98f * pow(
        1.0f - saturate(dot(viewDirection, input.WorldNormal)), 5.0f);
    // Rough surfaces keep the same reflected environment identity but attenuate
    // its coherent lobe. This makes the published material roughness affect
    // the rendered response without exposing arbitrary RHI sampler state.
    const float reflectionLobe = (1.0f - roughness * 0.65f) *
        DeepColorAndReflection.w * FoamAndFeatureParameters.z;
    const float3 reflected = lerp(absorbedWater, ShallowColorAndRefraction.xyz,
        0.5f + 0.5f * input.WorldNormal.y);
    const float3 surfaceColor = lerp(throughWater, reflected,
        saturate(schlickFresnel * reflectionLobe));
    const float foam = saturate((input.Crest - FoamAndFeatureParameters.y) *
        4.0f) * FoamAndFeatureParameters.x;
    const float opticalOpacity = saturate((1.0f -
        (transmission.x + transmission.y + transmission.z) / 3.0f) +
        (1.0f - SurfaceSizeAndTransparency.z) * 0.35f + foam * 0.5f);
    // RT0 is an opaque write of an explicitly composed pixel: blending it
    // again against the active HDR color would double-apply transparency and
    // darken water with zero absorption. The authored transparency controls
    // how much optical/reflection response replaces the refracted snapshot.
    const float waterResponse = saturate(max(opticalOpacity,
        SurfaceSizeAndTransparency.z * 0.15f));
    const float3 composed = lerp(refracted,
        lerp(surfaceColor, FoamColorAndParameters.xyz, foam), waterResponse);
    WaterTargets output;
    output.Color = float4(composed, 1.0f);
    output.Reactive = saturate(waterResponse + foam);
    return output;
}
