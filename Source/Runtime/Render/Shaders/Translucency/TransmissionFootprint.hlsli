#ifndef RVX_TRANSMISSION_FOOTPRINT
#define RVX_TRANSMISSION_FOOTPRINT


// CPU reference: RenderContracts/TransmissionFootprint.h. This defines a
// conservative support, not a new color filter. Guard texels account for
// coordinate/filter rounding. Stored pyramid values are increasing device
// depth, so MIN is conservative for both standard and reversed Z.
bool RVXTransmissionFootprintBounds(float2 uv, uint2 extent, bool fullResolution,
                                    out int2 first, out int2 last)
{
    first = last = 0;
    if (any(extent == 0u) || any(extent > 65536u) ||
        !all(isfinite(uv)) || any(uv < 0.0f) || any(uv > 1.0f)) return false;
    const float2 coordinate = uv * float2(extent) - 0.5f;
    const float radius = fullResolution ? 0.0f : 0.5f;
    first = clamp(int2(floor(coordinate - radius)) - 1, int2(0, 0), int2(extent) - 1);
    last = clamp(int2(floor(coordinate + radius)) + 2, int2(0, 0), int2(extent) - 1);
    return true;
}

float RVXCanonicalTransmissionDepth(float depth, bool reverseZ)
{
    if (!isfinite(depth) || depth < 0.0f || depth > 1.0f) return 0.0f;
    return reverseZ ? 1.0f - depth : depth;
}

// Bind a single source mip view. Base reads the exact D32 snapshot, later
// levels read canonical R32 values. At most 5x5 source texels are visited.
float RVXReduceTransmissionDepth(Texture2D<float> source, float2 uv,
                                 bool fullResolution, bool reverseZ,
                                 bool sourceCanonical)
{
    uint width, height;
    source.GetDimensions(width, height);
    int2 first, last;
    if (!RVXTransmissionFootprintBounds(uv, uint2(width, height),
                                        fullResolution, first, last)) return 0.0f;
    float nearest = 1.0f;
    [loop] for (int y = first.y; y <= last.y; ++y)
    [loop] for (int x = first.x; x <= last.x; ++x)
        nearest = min(nearest, RVXCanonicalTransmissionDepth(
            source.Load(int3(x, y, 0)), sourceCanonical ? false : reverseZ));
    return nearest;
}

// Include both trilinear levels and every bilinear contributor at each level.
// Missing producers/provenance are a graph-level error, not handled here.
float RVXSampleTransmissionDepthPyramid(Texture2D<float> source, float2 uv, float lod)
{
    uint width, height, mipCount;
    source.GetDimensions(0u, width, height, mipCount);
    if (mipCount == 0u || mipCount > 17u || !isfinite(lod)) return 0.0f;
    int2 first, last;
    if (!RVXTransmissionFootprintBounds(uv, uint2(width, height), true, first, last))
        return 0.0f;
    const float bounded = clamp(lod, 0.0f, float(mipCount - 1u));
    const uint lower = uint(floor(bounded));
    const uint upper = uint(ceil(bounded));
    float nearest = 1.0f;
    [loop] for (uint mip = lower; mip <= upper; ++mip)
    {
        uint levelWidth, levelHeight, levels;
        source.GetDimensions(mip, levelWidth, levelHeight, levels);
        if (!RVXTransmissionFootprintBounds(uv, uint2(levelWidth, levelHeight),
                                            true, first, last)) return 0.0f;
        [loop] for (int y = first.y; y <= last.y; ++y)
        [loop] for (int x = first.x; x <= last.x; ++x)
            nearest = min(nearest, RVXCanonicalTransmissionDepth(
                source.Load(int3(x, y, mip)), false));
    }
    return nearest;
}

// One shared decision for production refraction and native pixel tests. A
// rejected candidate never authorizes sampling the unshifted color implicitly.
bool RVXResolveTransmissionSampleUV(Texture2D<float> depthPyramid,
    float2 candidateUV, bool candidateProjected, float2 baseUV,
    float lod, float canonicalExit, out float2 sampleUV)
{
    sampleUV = 0.0f;
    if (!isfinite(canonicalExit) || canonicalExit < 0.0f || canonicalExit > 1.0f ||
        !isfinite(lod)) return false;
    // The guard stores canonical [0,1] depth and maps invalid texels to zero.
    // A clear-depth value of one denotes sky, including when exit depth is one.
    const float candidateDepth = candidateProjected
        ? RVXSampleTransmissionDepthPyramid(depthPyramid, candidateUV, lod) : 0.0f;
    if (candidateProjected &&
        (candidateDepth == 1.0f || candidateDepth > canonicalExit + 1.0e-5f))
    {
        sampleUV = candidateUV;
        return true;
    }
    const float baseDepth = RVXSampleTransmissionDepthPyramid(depthPyramid, baseUV, lod);
    if (baseDepth == 1.0f || baseDepth > canonicalExit + 1.0e-5f)
    {
        sampleUV = baseUV;
        return true;
    }
    return false;
}

#endif
