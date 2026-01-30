/**
 * @file AtmosphericScattering.hlsl
 * @brief Atmospheric scattering shader
 * 
 * Physically-based atmospheric scattering using Rayleigh and Mie models.
 * Based on Bruneton's improved scattering model.
 */

// =============================================================================
// Constants
// =============================================================================

cbuffer AtmosphereConstants : register(b0)
{
    // Planet parameters
    float PlanetRadius;         // Planet radius in meters
    float AtmosphereRadius;     // Atmosphere radius (planet + atmo height)
    float ViewerHeight;         // Viewer height above surface
    float _Padding1;
    
    // Scattering coefficients
    float4 RayleighScattering;  // xyz: coefficients, w: scale height
    float4 MieScattering;       // xyz: coefficients, w: scale height
    float4 MieAbsorption;       // xyz: absorption, w: anisotropy (g)
    float4 OzoneAbsorption;     // xyz: absorption, w: layer height
    float4 OzoneParams;         // x: layer width, yzw: unused
    
    // Sun parameters
    float4 SunDirection;        // xyz: direction, w: intensity
    float4 SunColor;            // rgb: color, a: disk size
    
    // Camera
    float4 CameraPosition;      // xyz: position, w: unused
    float4x4 InvViewProj;
    
    // LUT parameters
    float4 TransmittanceLUTSize;
    float4 SkyViewLUTSize;
    float4 ScreenSize;
}

#define RayleighCoeff       RayleighScattering.xyz
#define RayleighScaleHeight RayleighScattering.w
#define MieCoeff            MieScattering.xyz
#define MieScaleHeight      MieScattering.w
#define MieAbsorbCoeff      MieAbsorption.xyz
#define MieAnisotropy       MieAbsorption.w
#define OzoneCoeff          OzoneAbsorption.xyz
#define OzoneLayerHeight    OzoneAbsorption.w
#define OzoneLayerWidth     OzoneParams.x
#define SunDir              SunDirection.xyz
#define SunIntensity        SunDirection.w
#define SunDiskSize         SunColor.a

static const float PI = 3.14159265359;

// =============================================================================
// LUTs
// =============================================================================

Texture2D<float4> TransmittanceLUT : register(t0);
Texture2D<float4> MultiScatteringLUT : register(t1);
Texture2D<float4> SkyViewLUT : register(t2);
Texture2D<float> DepthTexture : register(t3);
Texture2D<float4> SceneColor : register(t4);

RWTexture2D<float4> TransmittanceOutput : register(u0);
RWTexture2D<float4> MultiScatteringOutput : register(u1);
RWTexture2D<float4> SkyViewOutput : register(u2);
RWTexture2D<float4> OutputTexture : register(u3);

SamplerState LinearClampSampler : register(s0);

// =============================================================================
// Helper functions
// =============================================================================

// Get altitude from radius
float GetAltitude(float r)
{
    return r - PlanetRadius;
}

// Density at altitude for exponential distribution
float GetDensity(float altitude, float scaleHeight)
{
    return exp(-altitude / scaleHeight);
}

// Ozone density (tent-like distribution)
float GetOzoneDensity(float altitude)
{
    float d = abs(altitude - OzoneLayerHeight) / OzoneLayerWidth;
    return max(0.0, 1.0 - d);
}

// Ray-sphere intersection
// Returns distances to intersection points (negative if behind ray origin)
bool RaySphereIntersect(float3 rayOrigin, float3 rayDir, float sphereRadius, 
                        out float t0, out float t1)
{
    float b = dot(rayOrigin, rayDir);
    float c = dot(rayOrigin, rayOrigin) - sphereRadius * sphereRadius;
    float discriminant = b * b - c;
    
    if (discriminant < 0.0)
    {
        t0 = t1 = 0.0;
        return false;
    }
    
    float sqrtD = sqrt(discriminant);
    t0 = -b - sqrtD;
    t1 = -b + sqrtD;
    return true;
}

// Get intersection distance with atmosphere
float GetAtmosphereIntersection(float3 origin, float3 dir)
{
    float t0, t1;
    if (RaySphereIntersect(origin, dir, AtmosphereRadius, t0, t1))
    {
        // Check for planet intersection
        float tp0, tp1;
        if (RaySphereIntersect(origin, dir, PlanetRadius, tp0, tp1))
        {
            if (tp0 > 0.0)
                return tp0;  // Hit planet first
        }
        return max(0.0, t1);
    }
    return 0.0;
}

// Rayleigh phase function
float RayleighPhase(float cosTheta)
{
    return 3.0 / (16.0 * PI) * (1.0 + cosTheta * cosTheta);
}

// Mie phase function (Henyey-Greenstein)
float MiePhase(float cosTheta, float g)
{
    float g2 = g * g;
    float denom = 1.0 + g2 - 2.0 * g * cosTheta;
    return (1.0 - g2) / (4.0 * PI * pow(abs(denom), 1.5));
}

// =============================================================================
// Transmittance LUT computation
// =============================================================================

// Map UV to (cosZenith, altitude)
void TransmittanceLUTParams(float2 uv, out float cosZenith, out float altitude)
{
    // Non-linear mapping for better precision near horizon
    float x = uv.x;
    float y = uv.y;
    
    float H = sqrt(AtmosphereRadius * AtmosphereRadius - PlanetRadius * PlanetRadius);
    float rho = H * y;
    float r = sqrt(rho * rho + PlanetRadius * PlanetRadius);
    altitude = r - PlanetRadius;
    
    float dMin = AtmosphereRadius - r;
    float dMax = rho + H;
    float d = dMin + x * (dMax - dMin);
    cosZenith = d == 0.0 ? 1.0 : (H * H - rho * rho - d * d) / (2.0 * r * d);
}

// Compute transmittance for a ray
float3 ComputeTransmittance(float3 origin, float3 dir, float maxDist)
{
    const int SAMPLE_COUNT = 40;
    float stepSize = maxDist / float(SAMPLE_COUNT);
    
    float3 opticalDepth = 0.0;
    
    for (int i = 0; i < SAMPLE_COUNT; i++)
    {
        float t = (float(i) + 0.5) * stepSize;
        float3 pos = origin + dir * t;
        float altitude = length(pos) - PlanetRadius;
        
        float rayleighDensity = GetDensity(altitude, RayleighScaleHeight);
        float mieDensity = GetDensity(altitude, MieScaleHeight);
        float ozoneDensity = GetOzoneDensity(altitude);
        
        float3 extinction = RayleighCoeff * rayleighDensity +
                           (MieCoeff + MieAbsorbCoeff) * mieDensity +
                           OzoneCoeff * ozoneDensity;
        
        opticalDepth += extinction * stepSize;
    }
    
    return exp(-opticalDepth);
}

[numthreads(8, 8, 1)]
void CS_Transmittance(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint2 size = (uint2)TransmittanceLUTSize.xy;
    if (dispatchThreadId.x >= size.x || dispatchThreadId.y >= size.y)
        return;
    
    float2 uv = (dispatchThreadId.xy + 0.5) / float2(size);
    
    float cosZenith, altitude;
    TransmittanceLUTParams(uv, cosZenith, altitude);
    
    float r = PlanetRadius + altitude;
    float3 origin = float3(0, r, 0);
    float3 dir = float3(sqrt(1.0 - cosZenith * cosZenith), cosZenith, 0);
    
    float maxDist = GetAtmosphereIntersection(origin, dir);
    float3 transmittance = ComputeTransmittance(origin, dir, maxDist);
    
    TransmittanceOutput[dispatchThreadId.xy] = float4(transmittance, 1.0);
}

// =============================================================================
// Multi-scattering LUT computation
// =============================================================================

[numthreads(8, 8, 1)]
void CS_MultiScattering(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    // TODO: Implement multi-scattering LUT
    // Uses hemispherical integration to compute infinite bounces
    MultiScatteringOutput[dispatchThreadId.xy] = float4(0, 0, 0, 1);
}

// =============================================================================
// Sky-view LUT computation
// =============================================================================

float3 ComputeSkyRadiance(float3 origin, float3 dir, float3 sunDir)
{
    const int SAMPLE_COUNT = 32;
    
    float maxDist = GetAtmosphereIntersection(origin, dir);
    if (maxDist <= 0.0)
        return float3(0, 0, 0);
    
    float stepSize = maxDist / float(SAMPLE_COUNT);
    float cosTheta = dot(dir, sunDir);
    
    float3 rayleighPhase3 = float3(RayleighPhase(cosTheta), RayleighPhase(cosTheta), RayleighPhase(cosTheta));
    float miePhaseVal = MiePhase(cosTheta, MieAnisotropy);
    
    float3 luminance = 0.0;
    float3 transmittance = 1.0;
    
    for (int i = 0; i < SAMPLE_COUNT; i++)
    {
        float t = (float(i) + 0.5) * stepSize;
        float3 pos = origin + dir * t;
        float altitude = length(pos) - PlanetRadius;
        
        float rayleighDensity = GetDensity(altitude, RayleighScaleHeight);
        float mieDensity = GetDensity(altitude, MieScaleHeight);
        
        // Sample transmittance to sun
        float3 sunTransmittance = 1.0;  // TODO: Sample TransmittanceLUT
        
        // In-scattering
        float3 rayleighScatter = RayleighCoeff * rayleighDensity * rayleighPhase3;
        float3 mieScatter = MieCoeff * mieDensity * miePhaseVal;
        
        float3 scattering = (rayleighScatter + mieScatter) * sunTransmittance * SunIntensity;
        
        // Extinction
        float3 extinction = RayleighCoeff * rayleighDensity + 
                           (MieCoeff + MieAbsorbCoeff) * mieDensity;
        
        float3 stepTransmittance = exp(-extinction * stepSize);
        
        // Integrate
        float3 integralScatter = scattering * (1.0 - stepTransmittance) / max(extinction, 0.0001);
        luminance += transmittance * integralScatter;
        transmittance *= stepTransmittance;
    }
    
    return luminance * SunColor.rgb;
}

[numthreads(8, 8, 1)]
void CS_SkyView(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint2 size = (uint2)SkyViewLUTSize.xy;
    if (dispatchThreadId.x >= size.x || dispatchThreadId.y >= size.y)
        return;
    
    float2 uv = (dispatchThreadId.xy + 0.5) / float2(size);
    
    // Map UV to direction
    float azimuth = uv.x * 2.0 * PI;
    float zenith = uv.y * PI;
    
    float3 dir;
    dir.x = sin(zenith) * cos(azimuth);
    dir.y = cos(zenith);
    dir.z = sin(zenith) * sin(azimuth);
    
    float3 origin = float3(0, PlanetRadius + ViewerHeight, 0);
    float3 radiance = ComputeSkyRadiance(origin, dir, SunDir);
    
    SkyViewOutput[dispatchThreadId.xy] = float4(radiance, 1.0);
}

// =============================================================================
// Sky rendering
// =============================================================================

struct VSOutput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

VSOutput VS_Fullscreen(uint vertexId : SV_VertexID)
{
    VSOutput output;
    output.uv = float2((vertexId << 1) & 2, vertexId & 2);
    output.position = float4(output.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return output;
}

float4 PS_RenderSky(VSOutput input) : SV_Target
{
    // Reconstruct view direction
    float4 clipPos = float4(input.uv * 2.0 - 1.0, 1.0, 1.0);
    clipPos.y = -clipPos.y;
    float4 worldPos = mul(InvViewProj, clipPos);
    float3 viewDir = normalize(worldPos.xyz / worldPos.w - CameraPosition.xyz);
    
    // Sample sky-view LUT
    float azimuth = atan2(viewDir.z, viewDir.x);
    float zenith = acos(viewDir.y);
    float2 skyUV = float2(azimuth / (2.0 * PI) + 0.5, zenith / PI);
    
    float3 skyColor = SkyViewLUT.SampleLevel(LinearClampSampler, skyUV, 0).rgb;
    
    // Add sun disk
    float sunCosAngle = dot(viewDir, SunDir);
    if (sunCosAngle > cos(SunDiskSize))
    {
        float sunEdge = smoothstep(cos(SunDiskSize), cos(SunDiskSize * 0.9), sunCosAngle);
        float3 sunDisk = SunColor.rgb * SunIntensity * sunEdge;
        
        // Apply transmittance to sun
        // TODO: Sample transmittance LUT
        skyColor += sunDisk;
    }
    
    return float4(skyColor, 1.0);
}

// =============================================================================
// Aerial perspective
// =============================================================================

[numthreads(8, 8, 1)]
void CS_AerialPerspective(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.x >= (uint)ScreenSize.x || 
        dispatchThreadId.y >= (uint)ScreenSize.y)
        return;
    
    float2 uv = (dispatchThreadId.xy + 0.5) * ScreenSize.zw;
    
    // Get scene depth and color
    float depth = DepthTexture.SampleLevel(LinearClampSampler, uv, 0);
    float4 scene = SceneColor[dispatchThreadId.xy];
    
    // Skip sky pixels
    if (depth <= 0.0)
    {
        OutputTexture[dispatchThreadId.xy] = scene;
        return;
    }
    
    // Reconstruct world position
    float4 clipPos = float4(uv * 2.0 - 1.0, depth, 1.0);
    clipPos.y = -clipPos.y;
    float4 worldPos = mul(InvViewProj, clipPos);
    float3 position = worldPos.xyz / worldPos.w;
    
    float3 viewDir = normalize(position - CameraPosition.xyz);
    float distance = length(position - CameraPosition.xyz);
    
    // Compute aerial perspective
    float3 origin = float3(0, PlanetRadius + ViewerHeight, 0);
    float3 inScattering = ComputeSkyRadiance(origin, viewDir, SunDir) * 
                          (1.0 - exp(-distance * 0.00001));
    
    // Transmittance over distance
    float3 transmittance = exp(-distance * (RayleighCoeff + MieCoeff) * 0.00001);
    
    // Apply
    float3 result = scene.rgb * transmittance + inScattering;
    
    OutputTexture[dispatchThreadId.xy] = float4(result, scene.a);
}
