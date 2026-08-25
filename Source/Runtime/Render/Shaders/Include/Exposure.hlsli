// =============================================================================
// Exposure.hlsli - shared pre-exposure state ABI
// =============================================================================
//
// This ABI is deliberately shared by the exposure compute controller and every
// HDR producer/consumer.  Keep every member 16-byte aligned: the state is a
// StructuredBuffer element on D3D, Vulkan, Metal, and the compatibility
// backends and must not depend on cbuffer packing rules.
// =============================================================================

#ifndef RVX_EXPOSURE_HLSLI
#define RVX_EXPOSURE_HLSLI

#define RVX_EXPOSURE_STATE_VALID 1u
#define RVX_EXPOSURE_PHYSICAL_CALIBRATION 1.2
#define RVX_EXPOSURE_MIN_EV100 (-12.0)
#define RVX_EXPOSURE_MAX_EV100 24.0
#define RVX_EXPOSURE_MIN_SCALE 4.8506380e-11
#define RVX_EXPOSURE_MAX_SCALE 3495253.5
#define RVX_EXPOSURE_STATE_MODE_MANUAL 0u
#define RVX_EXPOSURE_STATE_MODE_HISTOGRAM_AUTO 1u

struct RVXExposureState
{
    // x = adapted candidate scale (and the producer scale once committed),
    // y = scale that authored the HDR frame paired with this candidate,
    // z = scale that authored the immediately preceding temporal history,
    // w = adapted EV100.
    float4 ExposureValues;
    // x = asuint(target EV100), y/z = exact 64-bit candidate identity. Keep
    // these as integers so a nonce containing a NaN bit pattern cannot be
    // canonicalized by a floating-point store.
    uint4 TargetAndNonce;
    // x/y = stable 64-bit configuration/state hash, z = RenderExposureMode,
    // w = RVX_EXPOSURE_STATE_VALID only after a candidate was fully written.
    uint4 StateMeta;
};

float RVXExposureScale(const RVXExposureState state)
{
    return state.ExposureValues.x;
}

// HDR producers use the prior presented physical exposure scale.  The y/z
// values instead describe the candidate's current/previous pre-exposed HDR
// spaces so temporal consumers can rescale history without a one-frame lag.
float RVXExposureProducerScale(const RVXExposureState state)
{
    return RVXExposureScale(state);
}

float RVXExposurePreExposure(const RVXExposureState state)
{
    return state.ExposureValues.y;
}

float RVXExposurePreviousPreExposure(const RVXExposureState state)
{
    return state.ExposureValues.z;
}

bool RVXExposureStateIsValid(const RVXExposureState state)
{
    return state.StateMeta.w == RVX_EXPOSURE_STATE_VALID &&
        state.StateMeta.z <= RVX_EXPOSURE_STATE_MODE_HISTOGRAM_AUTO &&
        state.ExposureValues.x == state.ExposureValues.x &&
        state.ExposureValues.x >= RVX_EXPOSURE_MIN_SCALE &&
        state.ExposureValues.x <= RVX_EXPOSURE_MAX_SCALE &&
        state.ExposureValues.y == state.ExposureValues.y &&
        state.ExposureValues.y >= RVX_EXPOSURE_MIN_SCALE &&
        state.ExposureValues.y <= RVX_EXPOSURE_MAX_SCALE &&
        state.ExposureValues.z == state.ExposureValues.z &&
        state.ExposureValues.z >= RVX_EXPOSURE_MIN_SCALE &&
        state.ExposureValues.z <= RVX_EXPOSURE_MAX_SCALE &&
        state.ExposureValues.w == state.ExposureValues.w &&
        state.ExposureValues.w >= RVX_EXPOSURE_MIN_EV100 &&
        state.ExposureValues.w <= RVX_EXPOSURE_MAX_EV100 &&
        asfloat(state.TargetAndNonce.x) == asfloat(state.TargetAndNonce.x) &&
        asfloat(state.TargetAndNonce.x) >= RVX_EXPOSURE_MIN_EV100 &&
        asfloat(state.TargetAndNonce.x) <= RVX_EXPOSURE_MAX_EV100;
}

#endif // RVX_EXPOSURE_HLSLI
