#ifndef RVX_WEIGHTED_BLENDED_OIT_HLSLI
#define RVX_WEIGHTED_BLENDED_OIT_HLSLI

// Bounded weighting mirrors the CPU qualification reference. The caller
// supplies the fragment's normalized [0,1] raster depth; alpha and depth terms
// remain finite before the fixed clamp is applied.
static const float RVX_WEIGHTED_OIT_MIN_WEIGHT = 0.01f;
// RGBA16F safety contract: at most 512 overlapping contributors, each with a
// straight HDR channel <= 16 and weight <= 4, yields <= 32768 per channel.
// Additive fire remains on the direct path and is not constrained by this
// accumulator contract.
static const float RVX_WEIGHTED_OIT_MAX_WEIGHT = 4.0f;
static const float RVX_WEIGHTED_OIT_ACCUMULATION_EPSILON = 1.0e-5f;
static const float RVX_WEIGHTED_OIT_MAX_HDR_CHANNEL = 16.0f;

struct RVXWeightedBlendedOITTargets
{
    float4 Accumulation : SV_Target0;
    float Revealage : SV_Target1;
};

float RVXCanonicalizeWeightedBlendedOITDepth(
    float deviceDepth,
    bool reverseZ)
{
    const float saturatedDepth = saturate(deviceDepth);
    return reverseZ ? 1.0f - saturatedDepth : saturatedDepth;
}

float RVXWeightedBlendedOITWeight(float alpha, float depth)
{
    const float alphaTerm = 0.1f + saturate(alpha) * 1.5f;
    const float depthTerm = 1.0f - saturate(depth) * 0.9f;
    return clamp(
        alphaTerm * alphaTerm * alphaTerm *
            depthTerm * depthTerm * depthTerm,
        RVX_WEIGHTED_OIT_MIN_WEIGHT,
        RVX_WEIGHTED_OIT_MAX_WEIGHT);
}

RVXWeightedBlendedOITTargets RVXEncodeWeightedBlendedOIT(
    float3 straightColor,
    float alpha,
    float depth)
{
    RVXWeightedBlendedOITTargets output;
    const float coverage = saturate(alpha);
    const float weight = RVXWeightedBlendedOITWeight(coverage, depth);
    const float weightedCoverage = coverage * weight;
    const float3 boundedColor = min(
        max(straightColor, 0.0f.xxx),
        RVX_WEIGHTED_OIT_MAX_HDR_CHANNEL.xxx);
    output.Accumulation = float4(
        boundedColor * weightedCoverage,
        weightedCoverage);
    // RT1 uses Zero / InvSrcColor. Returning coverage therefore multiplies
    // the cleared revealage value by (1 - coverage) for every contributor.
    output.Revealage = coverage;
    return output;
}

#endif // RVX_WEIGHTED_BLENDED_OIT_HLSLI
