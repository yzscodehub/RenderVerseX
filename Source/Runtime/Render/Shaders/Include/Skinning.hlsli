// =============================================================================
// Skinning.hlsli - Structured current/previous skin palette ABI
// =============================================================================
//
// Requires ObjectConstants.SkinningParams to be declared before inclusion:
//   x = skinning enabled, y = matrix count,
//   z = current palette matrix offset, w = previous palette matrix offset.
// The producer validates every range before command recording. Both resources
// are nevertheless always bound (identity fallback for rigid draws) so an
// invalid previous history remains safe to read in ObjectVelocity.

#ifndef RVX_SKINNING_HLSLI
#define RVX_SKINNING_HLSLI

#if !defined(RVX_GPU_SCENE_RASTER)

#if defined(RVX_DX11_FLATTENED_REGISTER_SPACES)
// GetDX11FlattenedRegisterBase('t', 1) == 32. Reflection restores these
// physical SM5 slots to the logical set-1 bindings after compilation.
StructuredBuffer<float4x4> CurrentSkinningPalette : register(t34);
StructuredBuffer<float4x4> PreviousSkinningPalette : register(t35);
#else
StructuredBuffer<float4x4> CurrentSkinningPalette : register(t2, space1);
StructuredBuffer<float4x4> PreviousSkinningPalette : register(t3, space1);
#endif

bool RVXHasSkinningPalette()
{
    return SkinningParams.x > 0.5f && SkinningParams.y > 0.5f;
}

bool RVXResolveSkinningInfluences(
    uint4 boneIndices,
    float4 boneWeights,
    out uint4 safeIndices,
    out float4 normalizedWeights)
{
    safeIndices = uint4(0u, 0u, 0u, 0u);
    normalizedWeights = float4(0.0f, 0.0f, 0.0f, 0.0f);
    if (!RVXHasSkinningPalette())
    {
        return false;
    }

    const float weightSum = boneWeights.x + boneWeights.y +
                            boneWeights.z + boneWeights.w;
    if (weightSum <= 1.0e-5f)
    {
        return false;
    }

    const uint matrixCount = (uint)SkinningParams.y;
    safeIndices = min(boneIndices, matrixCount - 1u);
    normalizedWeights = boneWeights / weightSum;
    return true;
}

float4 RVXBlendSkinnedPosition(
    float3 position,
    float4x4 matrix0,
    float4x4 matrix1,
    float4x4 matrix2,
    float4x4 matrix3,
    float4 weights)
{
    const float4 localPosition = float4(position, 1.0f);
    return mul(matrix0, localPosition) * weights.x +
           mul(matrix1, localPosition) * weights.y +
           mul(matrix2, localPosition) * weights.z +
           mul(matrix3, localPosition) * weights.w;
}

float3 RVXBlendSkinnedVector(
    float3 vectorValue,
    float4x4 matrix0,
    float4x4 matrix1,
    float4x4 matrix2,
    float4x4 matrix3,
    float4 weights)
{
    const float4 localVector = float4(vectorValue, 0.0f);
    return (mul(matrix0, localVector) * weights.x +
            mul(matrix1, localVector) * weights.y +
            mul(matrix2, localVector) * weights.z +
            mul(matrix3, localVector) * weights.w).xyz;
}

float4 RVXResolveCurrentSkinningPosition(
    float3 position,
    uint4 boneIndices,
    float4 boneWeights)
{
    uint4 safeIndices;
    float4 weights;
    if (!RVXResolveSkinningInfluences(boneIndices, boneWeights, safeIndices, weights))
    {
        return float4(position, 1.0f);
    }

    const uint baseOffset = (uint)SkinningParams.z;
    return RVXBlendSkinnedPosition(
        position,
        CurrentSkinningPalette[baseOffset + safeIndices.x],
        CurrentSkinningPalette[baseOffset + safeIndices.y],
        CurrentSkinningPalette[baseOffset + safeIndices.z],
        CurrentSkinningPalette[baseOffset + safeIndices.w],
        weights);
}

float3 RVXResolveCurrentSkinningVector(
    float3 vectorValue,
    uint4 boneIndices,
    float4 boneWeights)
{
    uint4 safeIndices;
    float4 weights;
    if (!RVXResolveSkinningInfluences(boneIndices, boneWeights, safeIndices, weights))
    {
        return vectorValue;
    }

    const uint baseOffset = (uint)SkinningParams.z;
    return RVXBlendSkinnedVector(
        vectorValue,
        CurrentSkinningPalette[baseOffset + safeIndices.x],
        CurrentSkinningPalette[baseOffset + safeIndices.y],
        CurrentSkinningPalette[baseOffset + safeIndices.z],
        CurrentSkinningPalette[baseOffset + safeIndices.w],
        weights);
}

float4 RVXResolvePreviousSkinningPosition(
    float3 position,
    uint4 boneIndices,
    float4 boneWeights)
{
    uint4 safeIndices;
    float4 weights;
    if (!RVXResolveSkinningInfluences(boneIndices, boneWeights, safeIndices, weights))
    {
        return float4(position, 1.0f);
    }

    const uint baseOffset = (uint)SkinningParams.w;
    return RVXBlendSkinnedPosition(
        position,
        PreviousSkinningPalette[baseOffset + safeIndices.x],
        PreviousSkinningPalette[baseOffset + safeIndices.y],
        PreviousSkinningPalette[baseOffset + safeIndices.z],
        PreviousSkinningPalette[baseOffset + safeIndices.w],
        weights);
}

#else

// GPU-scene permutations retain the shared source entry points but bind their
// own set-1 layout. The direct-skinned entry points are unreachable there; the
// stubs keep HLSL's whole-translation-unit validation from requiring t2/t3.
float4 RVXResolveCurrentSkinningPosition(
    float3 position,
    uint4 boneIndices,
    float4 boneWeights)
{
    return float4(position, 1.0f);
}

float3 RVXResolveCurrentSkinningVector(
    float3 vectorValue,
    uint4 boneIndices,
    float4 boneWeights)
{
    return vectorValue;
}

float4 RVXResolvePreviousSkinningPosition(
    float3 position,
    uint4 boneIndices,
    float4 boneWeights)
{
    return float4(position, 1.0f);
}

#endif // !RVX_GPU_SCENE_RASTER

#endif // RVX_SKINNING_HLSLI
