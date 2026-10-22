// =============================================================================
// TransmissionDepthPyramid.hlsl - conservative refraction guard pyramid
// =============================================================================
//
// This pass never filters depth as color. It writes the nearest canonical
// device depth over every source texel that can contribute to the paired
// opaque-scene color pyramid texel. Mip zero reads the exact D32 snapshot;
// later reductions read canonical R32 output from the prior level.
// =============================================================================

#include "TransmissionFootprint.hlsli"

cbuffer TransmissionDepthPyramidConstants : register(b0, space0)
{
    // x: full-resolution source copy; y: source D32 reverse-Z; z: source
    // already canonical R32; w: reserved. The source/target mip pairing and
    // resource provenance are graph contracts, not shader fallback behavior.
    float4 SourceParameters;
};

Texture2D<float> InputDepth : register(t1, space0);

struct VSOutput
{
    float4 Position : SV_POSITION;
    float2 TexCoord : TEXCOORD0;
};

float PSMain(VSOutput input) : SV_TARGET
{
    if (SourceParameters.x > 0.5f)
    {
        // Full-resolution base has exact 1:1 pixel centers. Avoid interpolated
        // UV roundoff changing the inclusive guard at integer boundaries.
        uint width, height;
        InputDepth.GetDimensions(width, height);
        const int2 pixel = int2(input.Position.xy);
        const int2 first = max(pixel - 1, int2(0, 0));
        const int2 last = min(pixel + 2, int2(width, height) - 1);
        float nearest = 1.0f;
        [loop] for (int y = first.y; y <= last.y; ++y)
        [loop] for (int x = first.x; x <= last.x; ++x)
            nearest = min(nearest, RVXCanonicalTransmissionDepth(
                InputDepth.Load(int3(x, y, 0)), SourceParameters.y > 0.5f));
        return nearest;
    }
    return RVXReduceTransmissionDepth(
        InputDepth,
        input.TexCoord,
        SourceParameters.x > 0.5f,
        SourceParameters.y > 0.5f,
        SourceParameters.z > 0.5f);
}
