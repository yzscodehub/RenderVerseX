// =============================================================================
// Exposure.hlsl - deterministic GPU histogram and pre-exposure adaptation
// =============================================================================

#include "../Include/Exposure.hlsli"

cbuffer ExposureConstants : register(b0, space0)
{
    uint2 RenderExtent;
    float FixedDeltaSeconds;
    uint ExposureMode;

    float ManualEV100;
    float ExposureCompensationEV;
    float HistogramLowPercentile;
    float HistogramHighPercentile;

    float HistogramKeyValue;
    float MinEV100;
    float MaxEV100;
    float BrighteningAdaptationSpeed;

    float DarkeningAdaptationSpeed;
    uint ConfigurationHashLo;
    uint ConfigurationHashHi;
    uint ForceHistoryReset;

    uint CandidateStateContentHashLo;
    uint CandidateStateContentHashHi;
    uint2 ExposurePadding;
};

Texture2D<float4> SceneColor : register(t1, space0);
StructuredBuffer<RVXExposureState> CommittedExposureState : register(t2, space0);
RWStructuredBuffer<uint> Histogram : register(u3, space0);
RWStructuredBuffer<RVXExposureState> CandidateExposureState : register(u4, space0);

static const uint RVX_EXPOSURE_HISTOGRAM_BIN_COUNT = 256u;
// These endpoints mirror the public CPU metering contract. Endpoint bins
// round-trip exactly so a black frame resolves through the same 1e-4 floor.
static const float RVX_EXPOSURE_MIN_METERED_LUMINANCE = 1.0e-4;
static const float RVX_EXPOSURE_MAX_METERED_LUMINANCE = 65504.0;
static const float RVX_EXPOSURE_LOG_LUMINANCE_MIN = -13.2877123795494;
static const float RVX_EXPOSURE_LOG_LUMINANCE_MAX = 15.9992953870234;
static const float RVX_EXPOSURE_LOG_LUMINANCE_RANGE =
    RVX_EXPOSURE_LOG_LUMINANCE_MAX - RVX_EXPOSURE_LOG_LUMINANCE_MIN;
static const uint RVX_EXPOSURE_MODE_MANUAL_EV100 = 0u;
static const uint RVX_EXPOSURE_MODE_HISTOGRAM_AUTO = 1u;

float RVXLuminance(float3 color)
{
    return dot(max(color, 0.0), float3(0.2126, 0.7152, 0.0722));
}

uint RVXLuminanceToHistogramBin(float luminance)
{
    const float logLuminance = log2(clamp(
        luminance,
        RVX_EXPOSURE_MIN_METERED_LUMINANCE,
        RVX_EXPOSURE_MAX_METERED_LUMINANCE));
    const float normalized = saturate(
        (logLuminance - RVX_EXPOSURE_LOG_LUMINANCE_MIN) /
        RVX_EXPOSURE_LOG_LUMINANCE_RANGE);
    return min(
        (uint)(normalized * (float)(RVX_EXPOSURE_HISTOGRAM_BIN_COUNT - 1u) + 0.5),
        RVX_EXPOSURE_HISTOGRAM_BIN_COUNT - 1u);
}

float RVXHistogramBinToLuminance(uint bin)
{
    const float normalized = (float)min(
        bin, RVX_EXPOSURE_HISTOGRAM_BIN_COUNT - 1u) /
        (float)(RVX_EXPOSURE_HISTOGRAM_BIN_COUNT - 1u);
    return exp2(lerp(
        RVX_EXPOSURE_LOG_LUMINANCE_MIN,
        RVX_EXPOSURE_LOG_LUMINANCE_MAX,
        normalized));
}

float RVXAdaptEV100(float previousEV100, float targetEV100)
{
    const float speed = targetEV100 < previousEV100
        ? max(BrighteningAdaptationSpeed, 0.0)
        : max(DarkeningAdaptationSpeed, 0.0);
    const float alpha = 1.0 - exp(-speed * max(FixedDeltaSeconds, 0.0));
    return lerp(previousEV100, targetEV100, saturate(alpha));
}

[numthreads(64, 1, 1)]
void CSClearHistogram(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint index = dispatchThreadId.x;
    if (index < RVX_EXPOSURE_HISTOGRAM_BIN_COUNT)
    {
        Histogram[index] = 0u;
    }
}

[numthreads(16, 16, 1)]
void CSBuildHistogram(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.x >= RenderExtent.x || dispatchThreadId.y >= RenderExtent.y)
    {
        return;
    }

    const RVXExposureState committed = CommittedExposureState[0];
    // HDR producers write with the previously presented scale. Metering must
    // use scene-linear luminance, otherwise the exposure loop measures itself.
    const float preExposure = RVXExposureStateIsValid(committed)
        ? RVXExposureScale(committed)
        : 1.0;
    const float luminance = RVXLuminance(
        SceneColor.Load(int3(dispatchThreadId.xy, 0)).rgb / preExposure);
    uint ignored;
    InterlockedAdd(Histogram[RVXLuminanceToHistogramBin(luminance)], 1u, ignored);
}

[numthreads(1, 1, 1)]
void CSAdaptExposure(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const RVXExposureState committed = CommittedExposureState[0];
    const bool useCommitted = ForceHistoryReset == 0u &&
        RVXExposureStateIsValid(committed);
    const float priorExposureScale = useCommitted
        ? RVXExposureScale(committed)
        : 1.0;
    const float priorPreExposure = useCommitted
        ? RVXExposurePreExposure(committed)
        : priorExposureScale;

    // Manual EV100 is independent authoring authority. Min/Max bound only
    // histogram metering and adaptation; the public CPU contract likewise
    // accepts the full documented manual EV range.
    float targetEV100 = ManualEV100;
    if (ExposureMode == RVX_EXPOSURE_MODE_HISTOGRAM_AUTO)
    {
        uint totalSamples = 0u;
        [unroll]
        for (uint bin = 0u; bin < RVX_EXPOSURE_HISTOGRAM_BIN_COUNT; ++bin)
        {
            totalSamples += Histogram[bin];
        }

        if (totalSamples > 0u)
        {
            const uint lowRank = min(
                (uint)(saturate(HistogramLowPercentile) * (float)(totalSamples - 1u)),
                totalSamples - 1u);
            const uint highRank = max(lowRank, min(
                (uint)(saturate(HistogramHighPercentile) * (float)(totalSamples - 1u)),
                totalSamples - 1u));
            uint cumulative = 0u;
            float weightedLogLuminance = 0.0;
            uint selectedSamples = 0u;
            [unroll]
            for (uint bin = 0u; bin < RVX_EXPOSURE_HISTOGRAM_BIN_COUNT; ++bin)
            {
                const uint count = Histogram[bin];
                const uint startRank = cumulative;
                const uint endRank = cumulative + count;
                const uint selectedStart = max(startRank, lowRank);
                const uint selectedEnd = min(endRank, highRank + 1u);
                if (selectedEnd > selectedStart)
                {
                    const uint selectedCount = selectedEnd - selectedStart;
                    weightedLogLuminance += log2(RVXHistogramBinToLuminance(bin)) *
                        (float)selectedCount;
                    selectedSamples += selectedCount;
                }
                cumulative = endRank;
            }
            if (selectedSamples > 0u)
            {
                const float meteredLuminance = exp2(
                    weightedLogLuminance / (float)selectedSamples);
                targetEV100 = log2(max(
                    meteredLuminance /
                        max(HistogramKeyValue * RVX_EXPOSURE_PHYSICAL_CALIBRATION, 1.0e-6),
                    1.0e-6));
            }
        }
        targetEV100 = clamp(targetEV100, MinEV100, MaxEV100);
    }

    // Manual EV is authoring authority: it emits a candidate every frame but
    // never applies histogram-derived temporal adaptation.
    const float adaptedEV100 = ExposureMode == RVX_EXPOSURE_MODE_HISTOGRAM_AUTO &&
        useCommitted
        ? RVXAdaptEV100(committed.ExposureValues.w, targetEV100)
        : targetEV100;
    const float exposureScale = exp2(
        ExposureCompensationEV - adaptedEV100) /
        RVX_EXPOSURE_PHYSICAL_CALIBRATION;

    RVXExposureState candidate;
    candidate.ExposureValues = float4(
        exposureScale,
        priorExposureScale,
        priorPreExposure,
        adaptedEV100);
    candidate.TargetAndNonce = uint4(
        asuint(targetEV100),
        CandidateStateContentHashLo,
        CandidateStateContentHashHi,
        0u);
    candidate.StateMeta = uint4(
        ConfigurationHashLo,
        ConfigurationHashHi,
        ExposureMode,
        RVX_EXPOSURE_STATE_VALID);
    CandidateExposureState[0] = candidate;
}
