// =============================================================================
// ProceduralTexture.hlsl - Compute shader for procedural texture generation
// =============================================================================

cbuffer ComputeParams : register(b0)
{
    float g_Time;
    float g_Scale;
    uint g_Width;
    uint g_Height;
};

RWTexture2D<float4> g_Output : register(u1);

// Simple noise function
float hash(float2 p)
{
    return frac(sin(dot(p, float2(127.1, 311.7))) * 43758.5453123);
}

float noise(float2 p)
{
    float2 i = floor(p);
    float2 f = frac(p);
    
    float a = hash(i);
    float b = hash(i + float2(1.0, 0.0));
    float c = hash(i + float2(0.0, 1.0));
    float d = hash(i + float2(1.0, 1.0));
    
    float2 u = f * f * (3.0 - 2.0 * f);
    
    return lerp(a, b, u.x) + (c - a) * u.y * (1.0 - u.x) + (d - b) * u.x * u.y;
}

float fbm(float2 p)
{
    float value = 0.0;
    float amplitude = 0.5;
    float frequency = 1.0;
    
    for (int i = 0; i < 4; i++)
    {
        value += amplitude * noise(p * frequency);
        amplitude *= 0.5;
        frequency *= 2.0;
    }
    
    return value;
}

[numthreads(8, 8, 1)]
void CSMain(uint3 DTid : SV_DispatchThreadID)
{
    if (DTid.x >= g_Width || DTid.y >= g_Height)
        return;
    
    float2 uv = float2(DTid.xy) / float2(g_Width, g_Height);
    float2 p = uv * g_Scale;
    
    // Animated noise pattern
    float n1 = fbm(p + float2(g_Time * 0.3, g_Time * 0.2));
    float n2 = fbm(p * 2.0 - float2(g_Time * 0.2, g_Time * 0.1));
    
    // Create colorful pattern
    float3 col1 = float3(0.2, 0.4, 0.8);  // Blue
    float3 col2 = float3(0.8, 0.3, 0.5);  // Pink
    float3 col3 = float3(0.3, 0.8, 0.5);  // Green
    
    float3 color = lerp(col1, col2, n1);
    color = lerp(color, col3, n2 * 0.5);
    
    // Add some glow effect
    float glow = sin(g_Time * 2.0 + n1 * 10.0) * 0.5 + 0.5;
    color += float3(0.1, 0.1, 0.2) * glow;
    
    g_Output[DTid.xy] = float4(color, 1.0);
}
