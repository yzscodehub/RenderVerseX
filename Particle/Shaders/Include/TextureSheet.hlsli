/**
 * @file TextureSheet.hlsli
 * @brief Texture sheet animation utilities
 */

#ifndef TEXTURE_SHEET_HLSLI
#define TEXTURE_SHEET_HLSLI

// Texture sheet parameters
cbuffer TextureSheetParams : register(b3)
{
    float2 g_TileSize;      // 1.0 / tiles
    float2 g_TileCount;     // Number of tiles
    float g_FrameCount;
    float g_FrameRate;
    uint g_StartFrame;
    uint g_RandomStartFrame;
};

/**
 * Calculate UV coordinates for a specific frame
 * @param baseUV Original UV coordinates (0-1)
 * @param frameIndex Frame index to display
 * @return Transformed UV coordinates
 */
float2 GetTextureSheetUV(float2 baseUV, uint frameIndex)
{
    uint frame = frameIndex % uint(g_FrameCount);
    
    uint col = frame % uint(g_TileCount.x);
    uint row = frame / uint(g_TileCount.x);
    
    float2 offset = float2(float(col), float(row)) * g_TileSize;
    float2 uv = baseUV * g_TileSize + offset;
    
    return uv;
}

/**
 * Calculate frame index based on normalized age
 * @param normalizedAge Particle age / lifetime (0-1)
 * @param randomOffset Random offset for start frame
 * @return Frame index
 */
uint GetFrameFromAge(float normalizedAge, uint randomOffset)
{
    uint startFrame = g_RandomStartFrame ? randomOffset : g_StartFrame;
    uint frameIndex = startFrame + uint(normalizedAge * g_FrameCount);
    return frameIndex % uint(g_FrameCount);
}

/**
 * Calculate UV coordinates based on particle age
 * @param baseUV Original UV coordinates (0-1)
 * @param age Current particle age
 * @param lifetime Particle lifetime
 * @param randomSeed Random seed for start frame
 * @return Transformed UV coordinates
 */
float2 GetTextureSheetUVFromAge(float2 baseUV, float age, float lifetime, uint randomSeed)
{
    float normalizedAge = saturate(age / lifetime);
    uint randomOffset = randomSeed % uint(g_FrameCount);
    uint frameIndex = GetFrameFromAge(normalizedAge, randomOffset);
    return GetTextureSheetUV(baseUV, frameIndex);
}

#endif // TEXTURE_SHEET_HLSLI
