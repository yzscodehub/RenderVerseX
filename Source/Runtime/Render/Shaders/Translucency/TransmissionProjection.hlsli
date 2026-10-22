#ifndef RVX_TRANSMISSION_PROJECTION
#define RVX_TRANSMISSION_PROJECTION

// Mirrors RenderContracts/TransmissionProjection.h. Normal thickness and
// optical path length are distinct; roughness only selects the color mip.
bool RVXTransmissionRay(float3 incident, float3 normal, float ior,
                        float thickness, out float3 displacement,
                        out float pathLength)
{
    displacement = 0.0f;
    pathLength = 0.0f;
    if (!all(isfinite(incident)) || !all(isfinite(normal)) ||
        !isfinite(ior) || !isfinite(thickness) || ior < 1.0f || thickness < 0.0f ||
        dot(incident, incident) < 1.0e-8f || dot(normal, normal) < 1.0e-8f)
        return false;
    incident = normalize(incident);
    normal = normalize(normal);
    if (dot(incident, normal) > 0.0f) normal = -normal;
    const float3 ray = refract(incident, normal, 1.0f / ior);
    const float cosine = abs(dot(ray, normal));
    if (!all(isfinite(ray)) || cosine <= 1.0e-4f) return false;
    pathLength = thickness / cosine;
    displacement = ray * pathLength;
    return all(isfinite(displacement)) && isfinite(pathLength);
}

bool RVXProjectTransmissionCandidate(float4x4 viewProjection,
                                     float3 exitPosition, float2 pyramidExtent,
                                     out float2 uv)
{
    uv = 0.0f;
    if (!all(isfinite(pyramidExtent)) || any(pyramidExtent < 1.0f)) return false;
    const float4 clip = mul(viewProjection, float4(exitPosition, 1.0f));
    if (!all(isfinite(clip)) || clip.w <= 1.0e-4f) return false;
    uv = clip.xy / clip.w * float2(0.5f, -0.5f) + 0.5f;
    const float2 margin = 0.5f / pyramidExtent;
    return all(uv >= margin) && all(uv <= 1.0f - margin);
}

bool RVXTransmissionDepthBehindExit(float sampledDepth, float exitDepth, bool reverseZ)
{
    if (!isfinite(sampledDepth) || !isfinite(exitDepth) ||
        sampledDepth < 0.0f || sampledDepth > 1.0f || exitDepth < 0.0f || exitDepth > 1.0f)
        return false;
    if (sampledDepth == (reverseZ ? 0.0f : 1.0f)) return true;
    return reverseZ ? sampledDepth < exitDepth - 1.0e-5f
                    : sampledDepth > exitDepth + 1.0e-5f;
}

#endif
