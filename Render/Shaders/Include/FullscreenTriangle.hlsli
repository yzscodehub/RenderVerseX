// =============================================================================
// FullscreenTriangle.hlsli - Backend-neutral fullscreen triangle helpers
// =============================================================================

#ifndef RVX_FULLSCREEN_UV_Y_MATCHES_CLIP_Y
#error RVX_FULLSCREEN_UV_Y_MATCHES_CLIP_Y must be defined by the render pipeline
#endif

// Texture coordinates use the engine/glTF upper-left origin contract.
float2 RVX_GetFullscreenTriangleTexCoord(uint vertexID)
{
    return float2((vertexID << 1) & 2, vertexID & 2);
}

float2 RVX_GetFullscreenTriangleSemanticNdc(float2 texCoord)
{
    // Semantic NDC is backend-independent: (-1, +1) is the upper-left corner.
    return texCoord * float2(2.0, -2.0) + float2(-1.0, 1.0);
}

float4 RVX_GetFullscreenTrianglePosition(float2 texCoord)
{
    // Vulkan/OpenGL viewport mapping requires clip Y to follow texture Y;
    // DX11/DX12/Metal consume semantic NDC directly.
#if RVX_FULLSCREEN_UV_Y_MATCHES_CLIP_Y
    return float4(texCoord * 2.0 - 1.0, 0.0, 1.0);
#else
    return float4(RVX_GetFullscreenTriangleSemanticNdc(texCoord), 0.0, 1.0);
#endif
}
