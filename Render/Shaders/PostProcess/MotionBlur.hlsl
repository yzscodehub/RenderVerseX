/**
 * @file MotionBlur.hlsl
 * @brief Motion blur shader
 * 
 * Multi-pass motion blur implementation:
 * - Tile max velocity pass
 * - Neighbor max expansion pass
 * - Motion blur gather pass
 */

// =============================================================================
// Constants and resources
// =============================================================================

cbuffer MotionBlurConstants : register(b0)
{
    float4 MotionParams;        // x: intensity, y: maxVelocity, z: minVelocity, w: shutterAngle
    float4 MotionParams2;       // x: softZDistance, y: jitterStrength, z: sampleCount, w: depthAware
    float4 ScreenSize;          // x: width, y: height, z: 1/width, w: 1/height
    float4 TileParams;          // x: tileSize, y: tileCountX, z: tileCountY, w: unused
    float FrameTime;
    float3 _Padding;
}

#define Intensity       MotionParams.x
#define MaxVelocity     MotionParams.y
#define MinVelocity     MotionParams.z
#define ShutterAngle    MotionParams.w
#define SoftZDistance   MotionParams2.x
#define JitterStrength  MotionParams2.y
#define SampleCount     (uint)MotionParams2.z
#define DepthAware      (MotionParams2.w > 0.5)
#define TileSize        (uint)TileParams.x

Texture2D<float4> InputColor : register(t0);
Texture2D<float2> VelocityTexture : register(t1);
Texture2D<float> DepthTexture : register(t2);
Texture2D<float2> TileMaxTexture : register(t3);
Texture2D<float2> NeighborMaxTexture : register(t4);

RWTexture2D<float2> TileMaxOutput : register(u0);
RWTexture2D<float2> NeighborMaxOutput : register(u1);
RWTexture2D<float4> BlurOutput : register(u2);

SamplerState LinearClampSampler : register(s0);
SamplerState PointClampSampler : register(s1);

// =============================================================================
// Helper functions
// =============================================================================

// Interleaved gradient noise for jittering
float InterleavedGradientNoise(float2 uv, float frame)
{
    uv += frame * (float2(47.0, 17.0) * 0.695);
    const float3 magic = float3(0.06711056, 0.00583715, 52.9829189);
    return frac(magic.z * frac(dot(uv, magic.xy)));
}

// Soft depth comparison
float SoftDepthCompare(float depthA, float depthB, float softness)
{
    return saturate(1.0 - (depthA - depthB) / softness);
}

// Cone weight for velocity blur
float ConeWeight(float dist, float len)
{
    return saturate(1.0 - dist / len);
}

// Convert velocity from NDC to pixels
float2 VelocityToPixels(float2 velocity)
{
    return velocity * ScreenSize.xy * 0.5;
}

// =============================================================================
// Pass 1: Tile max velocity
// =============================================================================

groupshared float2 s_maxVelocity[64];

[numthreads(8, 8, 1)]
void CS_TileMax(uint3 groupId : SV_GroupID, 
                uint3 groupThreadId : SV_GroupThreadID,
                uint groupIndex : SV_GroupIndex)
{
    // Each thread processes multiple pixels in the tile
    uint2 tileOrigin = groupId.xy * TileSize;
    uint2 localPos = groupThreadId.xy;
    
    float2 maxVel = float2(0, 0);
    float maxVelMag = 0;
    
    // Each thread samples a portion of the tile
    for (uint y = localPos.y; y < TileSize; y += 8)
    {
        for (uint x = localPos.x; x < TileSize; x += 8)
        {
            uint2 pixelPos = tileOrigin + uint2(x, y);
            if (pixelPos.x < (uint)ScreenSize.x && pixelPos.y < (uint)ScreenSize.y)
            {
                float2 vel = VelocityTexture[pixelPos];
                float2 velPixels = VelocityToPixels(vel);
                float velMag = length(velPixels);
                
                if (velMag > maxVelMag)
                {
                    maxVelMag = velMag;
                    maxVel = velPixels;
                }
            }
        }
    }
    
    s_maxVelocity[groupIndex] = maxVel;
    GroupMemoryBarrierWithGroupSync();
    
    // Parallel reduction
    for (uint stride = 32; stride > 0; stride >>= 1)
    {
        if (groupIndex < stride)
        {
            float2 other = s_maxVelocity[groupIndex + stride];
            if (length(other) > length(s_maxVelocity[groupIndex]))
            {
                s_maxVelocity[groupIndex] = other;
            }
        }
        GroupMemoryBarrierWithGroupSync();
    }
    
    if (groupIndex == 0)
    {
        TileMaxOutput[groupId.xy] = s_maxVelocity[0];
    }
}

// =============================================================================
// Pass 2: Neighbor max expansion
// =============================================================================

[numthreads(8, 8, 1)]
void CS_NeighborMax(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    int2 tileCoord = (int2)dispatchThreadId.xy;
    
    float2 maxVel = float2(0, 0);
    float maxVelMag = 0;
    
    // 3x3 neighborhood
    for (int dy = -1; dy <= 1; dy++)
    {
        for (int dx = -1; dx <= 1; dx++)
        {
            int2 neighborCoord = tileCoord + int2(dx, dy);
            neighborCoord = clamp(neighborCoord, int2(0, 0), int2(TileParams.yz) - 1);
            
            float2 vel = TileMaxTexture[neighborCoord];
            float velMag = length(vel);
            
            if (velMag > maxVelMag)
            {
                maxVelMag = velMag;
                maxVel = vel;
            }
        }
    }
    
    NeighborMaxOutput[tileCoord] = maxVel;
}

// =============================================================================
// Pass 3: Motion blur gather
// =============================================================================

[numthreads(8, 8, 1)]
void CS_MotionBlurGather(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.x >= (uint)ScreenSize.x || 
        dispatchThreadId.y >= (uint)ScreenSize.y)
        return;
    
    float2 uv = (dispatchThreadId.xy + 0.5) * ScreenSize.zw;
    
    // Get velocity at this pixel
    float2 velocity = VelocityTexture.SampleLevel(PointClampSampler, uv, 0);
    float2 velocityPixels = VelocityToPixels(velocity) * ShutterAngle;
    
    float velocityLength = length(velocityPixels);
    
    // Early out for stationary pixels
    if (velocityLength < MinVelocity)
    {
        BlurOutput[dispatchThreadId.xy] = InputColor.SampleLevel(LinearClampSampler, uv, 0);
        return;
    }
    
    // Clamp velocity
    velocityLength = min(velocityLength, MaxVelocity);
    float2 velocityDir = normalize(velocityPixels);
    velocityPixels = velocityDir * velocityLength;
    
    // Get center depth
    float centerDepth = DepthTexture.SampleLevel(PointClampSampler, uv, 0);
    
    // Jitter for noise reduction
    float jitter = InterleavedGradientNoise(dispatchThreadId.xy, FrameTime) * JitterStrength;
    
    // Accumulate samples
    float4 colorSum = float4(0, 0, 0, 0);
    float weightSum = 0;
    
    for (uint i = 0; i < SampleCount; i++)
    {
        // Sample position along velocity direction
        float t = (float(i) + jitter) / float(SampleCount) - 0.5;
        float2 sampleOffset = velocityPixels * t * ScreenSize.zw;
        float2 sampleUV = uv + sampleOffset;
        
        // Clamp to screen bounds
        sampleUV = clamp(sampleUV, 0.0, 1.0);
        
        // Sample color and depth
        float4 sampleColor = InputColor.SampleLevel(LinearClampSampler, sampleUV, 0);
        float sampleDepth = DepthTexture.SampleLevel(PointClampSampler, sampleUV, 0);
        
        // Get velocity at sample position
        float2 sampleVel = VelocityTexture.SampleLevel(PointClampSampler, sampleUV, 0);
        float2 sampleVelPixels = VelocityToPixels(sampleVel) * ShutterAngle;
        
        // Weight based on velocity coverage
        float dist = abs(t) * velocityLength;
        float velWeight = ConeWeight(dist, length(sampleVelPixels));
        
        // Depth weight (prevents background bleeding into foreground)
        float depthWeight = 1.0;
        if (DepthAware)
        {
            depthWeight = SoftDepthCompare(centerDepth, sampleDepth, SoftZDistance);
            // Also allow samples that are in front
            depthWeight = max(depthWeight, SoftDepthCompare(sampleDepth, centerDepth, SoftZDistance));
        }
        
        float weight = velWeight * depthWeight;
        
        colorSum += sampleColor * weight;
        weightSum += weight;
    }
    
    // Add center sample
    float4 centerColor = InputColor.SampleLevel(LinearClampSampler, uv, 0);
    float centerWeight = 1.0 / (velocityLength / MaxVelocity + 1.0);
    colorSum += centerColor * centerWeight;
    weightSum += centerWeight;
    
    // Normalize
    float4 result = colorSum / max(weightSum, 0.0001);
    
    // Blend with original based on intensity
    result = lerp(centerColor, result, Intensity);
    
    BlurOutput[dispatchThreadId.xy] = result;
}

// =============================================================================
// Fullscreen pass for final output (if needed)
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

float4 PS_MotionBlur(VSOutput input) : SV_Target
{
    // This would be a graphics pass version of the blur
    // The compute version is preferred for performance
    return InputColor.Sample(LinearClampSampler, input.uv);
}
