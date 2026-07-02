// =============================================================================
// FullscreenQuad.hlsl - Fullscreen triangle shader for displaying textures
// =============================================================================

Texture2D g_Texture : register(t0);
SamplerState g_Sampler : register(s0);

struct PSInput
{
    float4 Position : SV_POSITION;
    float2 TexCoord : TEXCOORD0;
};

// =============================================================================
// Vertex Shader - Generates fullscreen triangle without vertex buffer
// =============================================================================
PSInput VSMain(uint vertexId : SV_VertexID)
{
    PSInput output;
    
    // Generate fullscreen triangle vertices
    // Vertex 0: (-1, -1), Vertex 1: (3, -1), Vertex 2: (-1, 3)
    float2 pos = float2((vertexId << 1) & 2, vertexId & 2);
    output.Position = float4(pos * 2.0 - 1.0, 0.0, 1.0);
    output.TexCoord = float2(pos.x, 1.0 - pos.y);  // Flip Y for correct orientation
    
    return output;
}

// =============================================================================
// Pixel Shader
// =============================================================================
float4 PSMain(PSInput input) : SV_TARGET
{
    return g_Texture.Sample(g_Sampler, input.TexCoord);
}
