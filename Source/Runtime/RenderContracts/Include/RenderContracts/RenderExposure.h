#pragma once

/**
 * @file RenderExposure.h
 * @brief Physical exposure settings and deterministic CPU reference contracts.
 *
 * The render frame owns authoring settings, while the renderer publishes one
 * immutable RenderExposureState per evaluated view.  Camera and environment
 * snapshots may retain their legacy scalar exposure values for compatibility,
 * but they are not an authority for this contract.
 */

#include "Core/Types.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>

namespace RVX
{
    /**
     * @brief The exposure controller selected for a rendered frame.
     *
     * ManualEV100 applies an author-selected physical EV100 immediately.
     * HistogramAuto meters the renderer-produced histogram result and adapts
     * the EV100 value using the deterministic effect-frame delta.
     */
    enum class RenderExposureMode : uint8
    {
        ManualEV100 = 0,
        HistogramAuto = 1,
    };

    [[nodiscard]] constexpr bool IsValidRenderExposureMode(
        RenderExposureMode mode) noexcept
    {
        switch (mode)
        {
            case RenderExposureMode::ManualEV100:
            case RenderExposureMode::HistogramAuto: return true;
            default: return false;
        }
    }

    /**
     * @brief Authoring limits shared by every renderer implementation.
     *
     * The interval deliberately remains broad enough for moonlit and bright
     * exterior scenes while limiting accidental values that would overflow the
     * HDR intermediate before pre-exposure has converged.
     */
    inline constexpr float32 RVX_RENDER_EXPOSURE_MIN_EV100 = -12.0f;
    inline constexpr float32 RVX_RENDER_EXPOSURE_MAX_EV100 = 24.0f;
    inline constexpr float32 RVX_RENDER_EXPOSURE_MIN_COMPENSATION_EV = -10.0f;
    inline constexpr float32 RVX_RENDER_EXPOSURE_MAX_COMPENSATION_EV = 10.0f;
    inline constexpr float32 RVX_RENDER_EXPOSURE_MAX_ADAPTATION_SPEED = 64.0f;
    inline constexpr float32 RVX_RENDER_EXPOSURE_MAX_DELTA_SECONDS = 1.0f;
    inline constexpr float32 RVX_RENDER_EXPOSURE_MIN_METERED_LUMINANCE =
        1.0e-4f;
    inline constexpr float32 RVX_RENDER_EXPOSURE_MAX_METERED_LUMINANCE =
        65504.0f;
    inline constexpr float32 RVX_RENDER_EXPOSURE_PHYSICAL_CALIBRATION = 1.2f;
    // Outward-rounded bounds of exp2(compensationEV - EV100) / 1.2 for
    // compensation [-10, 10] and EV100 [-12, 24].  The same values are part
    // of the shader ABI's fail-closed state validation.
    inline constexpr float32 RVX_RENDER_EXPOSURE_MIN_SCALE =
        4.8506380e-11f;
    inline constexpr float32 RVX_RENDER_EXPOSURE_MAX_SCALE =
        3495253.5f;
    inline constexpr float32
        RVX_RENDER_EXPOSURE_SCALE_RELATIVE_TOLERANCE = 1.0e-5f;

    /** @brief Shader-ABI range accepted for any exposure/pre-exposure scale. */
    [[nodiscard]] inline bool IsValidRenderExposureScale(
        float32 scale) noexcept
    {
        return std::isfinite(scale) &&
               scale >= RVX_RENDER_EXPOSURE_MIN_SCALE &&
               scale <= RVX_RENDER_EXPOSURE_MAX_SCALE;
    }

    /** @brief Relative comparison for values expressed in the exposure ABI. */
    [[nodiscard]] inline bool AreRenderExposureScalesEquivalent(
        float32 observed,
        float32 expected) noexcept
    {
        if (!IsValidRenderExposureScale(observed) ||
            !IsValidRenderExposureScale(expected))
        {
            return false;
        }
        const float32 referenceScale = std::max(
            std::abs(expected), RVX_RENDER_EXPOSURE_MIN_SCALE);
        return std::abs(observed - expected) <=
               RVX_RENDER_EXPOSURE_SCALE_RELATIVE_TOLERANCE *
                   referenceScale;
    }

    /**
     * @brief Immutable-frame authoring settings for one exposure controller.
     *
     * Histogram percentiles define the inclusive metering window consumed by
     * the GPU histogram reduction.  `histogramKeyValue` is the desired display
     * middle gray in linear scene space.  A histogram implementation supplies
     * one percentile-filtered metered luminance to the reference resolver.
     */
    struct RenderExposureSettings
    {
        RenderExposureMode mode = RenderExposureMode::ManualEV100;
        float32 manualEV100 = 0.0f;
        float32 exposureCompensationEV = 0.0f;
        float32 histogramLowPercentile = 0.50f;
        float32 histogramHighPercentile = 0.95f;
        float32 histogramKeyValue = 0.18f;
        float32 minEV100 = RVX_RENDER_EXPOSURE_MIN_EV100;
        float32 maxEV100 = RVX_RENDER_EXPOSURE_MAX_EV100;
        /** Speed used while exposure increases because the scene became darker. */
        float32 brighteningAdaptationSpeed = 3.0f;
        /** Speed used while exposure decreases because the scene became brighter. */
        float32 darkeningAdaptationSpeed = 1.0f;
    };

    [[nodiscard]] inline bool IsValidRenderExposureSettings(
        const RenderExposureSettings& settings) noexcept
    {
        return IsValidRenderExposureMode(settings.mode) &&
               std::isfinite(settings.manualEV100) &&
               settings.manualEV100 >= RVX_RENDER_EXPOSURE_MIN_EV100 &&
               settings.manualEV100 <= RVX_RENDER_EXPOSURE_MAX_EV100 &&
               std::isfinite(settings.exposureCompensationEV) &&
               settings.exposureCompensationEV >=
                   RVX_RENDER_EXPOSURE_MIN_COMPENSATION_EV &&
               settings.exposureCompensationEV <=
                   RVX_RENDER_EXPOSURE_MAX_COMPENSATION_EV &&
               std::isfinite(settings.histogramLowPercentile) &&
               settings.histogramLowPercentile >= 0.0f &&
               settings.histogramLowPercentile < 1.0f &&
               std::isfinite(settings.histogramHighPercentile) &&
               settings.histogramHighPercentile >
                   settings.histogramLowPercentile &&
               settings.histogramHighPercentile <= 1.0f &&
               std::isfinite(settings.histogramKeyValue) &&
               settings.histogramKeyValue > 0.0f &&
               settings.histogramKeyValue <= 1.0f &&
               std::isfinite(settings.minEV100) &&
               settings.minEV100 >= RVX_RENDER_EXPOSURE_MIN_EV100 &&
               std::isfinite(settings.maxEV100) &&
               settings.maxEV100 <= RVX_RENDER_EXPOSURE_MAX_EV100 &&
               settings.minEV100 < settings.maxEV100 &&
               std::isfinite(settings.brighteningAdaptationSpeed) &&
               settings.brighteningAdaptationSpeed >= 0.0f &&
               settings.brighteningAdaptationSpeed <=
                   RVX_RENDER_EXPOSURE_MAX_ADAPTATION_SPEED &&
               std::isfinite(settings.darkeningAdaptationSpeed) &&
               settings.darkeningAdaptationSpeed >= 0.0f &&
               settings.darkeningAdaptationSpeed <=
                   RVX_RENDER_EXPOSURE_MAX_ADAPTATION_SPEED;
    }

    [[nodiscard]] inline bool IsValidRenderExposureDeltaSeconds(
        float32 fixedDeltaSeconds) noexcept
    {
        return std::isfinite(fixedDeltaSeconds) && fixedDeltaSeconds >= 0.0f &&
               fixedDeltaSeconds <= RVX_RENDER_EXPOSURE_MAX_DELTA_SECONDS;
    }

    /** @brief Stable value fingerprint used to reject incompatible history. */
    [[nodiscard]] inline uint64 CalculateRenderExposureSettingsHash(
        const RenderExposureSettings& settings) noexcept
    {
        if (!IsValidRenderExposureSettings(settings))
            return 0;

        constexpr uint64 offsetBasis = 14695981039346656037ULL;
        constexpr uint64 prime = 1099511628211ULL;
        uint64 hash = offsetBasis;
        const auto append = [&hash](uint32 value) noexcept
        {
            hash ^= static_cast<uint64>(value);
            hash *= prime;
        };
        append(static_cast<uint32>(settings.mode));
        append(std::bit_cast<uint32>(settings.manualEV100));
        append(std::bit_cast<uint32>(settings.exposureCompensationEV));
        append(std::bit_cast<uint32>(settings.histogramLowPercentile));
        append(std::bit_cast<uint32>(settings.histogramHighPercentile));
        append(std::bit_cast<uint32>(settings.histogramKeyValue));
        append(std::bit_cast<uint32>(settings.minEV100));
        append(std::bit_cast<uint32>(settings.maxEV100));
        append(std::bit_cast<uint32>(settings.brighteningAdaptationSpeed));
        append(std::bit_cast<uint32>(settings.darkeningAdaptationSpeed));
        return hash;
    }

    /**
     * @brief Value-owned exposure result published after an evaluated frame.
     *
     * Its fields are private so a renderer cannot mutate a committed state in
     * place.  A new state must be resolved and published as the next frame's
     * immutable value.
     */
    class RenderExposureState final
    {
    public:
        [[nodiscard]] RenderExposureMode GetMode() const noexcept
        {
            return m_mode;
        }

        [[nodiscard]] float32 GetTargetEV100() const noexcept
        {
            return m_targetEV100;
        }

        [[nodiscard]] float32 GetAdaptedEV100() const noexcept
        {
            return m_adaptedEV100;
        }

        /** @brief Physical scene-to-display scale before pre-exposure ratio. */
        [[nodiscard]] float32 GetExposureScale() const noexcept
        {
            return m_exposureScale;
        }

        /** @brief Scale applied while writing this frame's HDR scene color. */
        [[nodiscard]] float32 GetPreExposure() const noexcept
        {
            return m_preExposure;
        }

        [[nodiscard]] uint64 GetConfigurationHash() const noexcept
        {
            return m_configurationHash;
        }

        [[nodiscard]] bool IsValid() const noexcept
        {
            return IsValidRenderExposureMode(m_mode) &&
                   std::isfinite(m_targetEV100) &&
                   m_targetEV100 >= RVX_RENDER_EXPOSURE_MIN_EV100 &&
                   m_targetEV100 <= RVX_RENDER_EXPOSURE_MAX_EV100 &&
                   std::isfinite(m_adaptedEV100) &&
                   m_adaptedEV100 >= RVX_RENDER_EXPOSURE_MIN_EV100 &&
                   m_adaptedEV100 <= RVX_RENDER_EXPOSURE_MAX_EV100 &&
                   IsValidRenderExposureScale(m_exposureScale) &&
                   IsValidRenderExposureScale(m_preExposure) &&
                   m_configurationHash != 0;
        }

        friend bool operator==(const RenderExposureState&,
                               const RenderExposureState&) = default;

    private:
        friend struct RenderExposureResolveResult;
        friend RenderExposureState MakeRenderExposureState(
            RenderExposureMode mode,
            float32 targetEV100,
            float32 adaptedEV100,
            float32 exposureScale,
            float32 preExposure,
            uint64 configurationHash) noexcept;

        RenderExposureMode m_mode = static_cast<RenderExposureMode>(255);
        float32 m_targetEV100 = std::numeric_limits<float32>::quiet_NaN();
        float32 m_adaptedEV100 = std::numeric_limits<float32>::quiet_NaN();
        float32 m_exposureScale = std::numeric_limits<float32>::quiet_NaN();
        float32 m_preExposure = std::numeric_limits<float32>::quiet_NaN();
        uint64 m_configurationHash = 0;
    };

    [[nodiscard]] inline bool IsValidRenderExposureState(
        const RenderExposureState& state) noexcept
    {
        return state.IsValid();
    }

    enum class RenderExposureResolveCode : uint8
    {
        Resolved = 0,
        ResolvedWithoutHistory,
        InvalidSettings,
        InvalidMeteredLuminance,
        InvalidDeltaSeconds,
        InvalidPreviousState,
        InvalidResolvedState,
    };

    struct RenderExposureResolveResult
    {
        RenderExposureResolveCode code =
            RenderExposureResolveCode::InvalidResolvedState;
        RenderExposureState state{};

        [[nodiscard]] bool Succeeded() const noexcept
        {
            return code == RenderExposureResolveCode::Resolved ||
                   code == RenderExposureResolveCode::ResolvedWithoutHistory;
        }

        [[nodiscard]] bool UsedHistory() const noexcept
        {
            return code == RenderExposureResolveCode::Resolved;
        }
    };

    [[nodiscard]] inline RenderExposureState MakeRenderExposureState(
        RenderExposureMode mode,
        float32 targetEV100,
        float32 adaptedEV100,
        float32 exposureScale,
        float32 preExposure,
        uint64 configurationHash) noexcept
    {
        RenderExposureState state;
        state.m_mode = mode;
        state.m_targetEV100 = targetEV100;
        state.m_adaptedEV100 = adaptedEV100;
        state.m_exposureScale = exposureScale;
        state.m_preExposure = preExposure;
        state.m_configurationHash = configurationHash;
        return state;
    }

    /**
     * @brief Convert an EV100 and compensation into physical exposure scale.
     *
     * This is the only calibration used by public renderer contracts:
     * `exp2(compensationEV - EV100) / 1.2`.
     */
    [[nodiscard]] inline bool TryCalculateRenderExposureScale(
        float32 ev100,
        float32 compensationEV,
        float32& outExposureScale) noexcept
    {
        if (!std::isfinite(ev100) ||
            ev100 < RVX_RENDER_EXPOSURE_MIN_EV100 ||
            ev100 > RVX_RENDER_EXPOSURE_MAX_EV100 ||
            !std::isfinite(compensationEV) ||
            compensationEV < RVX_RENDER_EXPOSURE_MIN_COMPENSATION_EV ||
            compensationEV > RVX_RENDER_EXPOSURE_MAX_COMPENSATION_EV)
        {
            return false;
        }

        const float32 scale = std::exp2(compensationEV - ev100) /
                              RVX_RENDER_EXPOSURE_PHYSICAL_CALIBRATION;
        if (!IsValidRenderExposureScale(scale))
            return false;
        outExposureScale = scale;
        return true;
    }

    /**
     * @brief Convert percentile-filtered histogram luminance into EV100.
     *
     * A black histogram is represented by zero and clamps to the documented
     * metering floor.  Negative, non-finite, or out-of-range values are not a
     * valid histogram result and fail closed.
     */
    [[nodiscard]] inline bool TryCalculateHistogramEV100(
        const RenderExposureSettings& settings,
        float32 meteredLuminance,
        float32& outEV100) noexcept
    {
        if (!IsValidRenderExposureSettings(settings) ||
            !std::isfinite(meteredLuminance) || meteredLuminance < 0.0f ||
            meteredLuminance > RVX_RENDER_EXPOSURE_MAX_METERED_LUMINANCE)
        {
            return false;
        }

        const float32 clampedLuminance = std::max(
            meteredLuminance, RVX_RENDER_EXPOSURE_MIN_METERED_LUMINANCE);
        const float32 ev100 = std::log2(
            clampedLuminance /
            (settings.histogramKeyValue *
             RVX_RENDER_EXPOSURE_PHYSICAL_CALIBRATION));
        if (!std::isfinite(ev100))
            return false;

        outEV100 = std::clamp(ev100, settings.minEV100, settings.maxEV100);
        return true;
    }

    /**
     * @brief Exponentially adapt EV100 with independent bright/dark rates.
     *
     * A lower EV increases scene exposure (the scene became darker), so it
     * uses `brighteningAdaptationSpeed`.  A higher EV uses the darkening rate.
     */
    [[nodiscard]] inline bool TryAdaptRenderExposureEV100(
        const RenderExposureSettings& settings,
        float32 currentEV100,
        float32 targetEV100,
        float32 fixedDeltaSeconds,
        float32& outAdaptedEV100) noexcept
    {
        if (!IsValidRenderExposureSettings(settings) ||
            !IsValidRenderExposureDeltaSeconds(fixedDeltaSeconds) ||
            !std::isfinite(currentEV100) ||
            currentEV100 < settings.minEV100 ||
            currentEV100 > settings.maxEV100 ||
            !std::isfinite(targetEV100) || targetEV100 < settings.minEV100 ||
            targetEV100 > settings.maxEV100)
        {
            return false;
        }

        const float32 speed = targetEV100 < currentEV100
                                  ? settings.brighteningAdaptationSpeed
                                  : settings.darkeningAdaptationSpeed;
        const float32 alpha = 1.0f - std::exp(-speed * fixedDeltaSeconds);
        const float32 adapted = currentEV100 +
                                (targetEV100 - currentEV100) * alpha;
        if (!std::isfinite(adapted))
            return false;
        outAdaptedEV100 = std::clamp(adapted, settings.minEV100,
                                     settings.maxEV100);
        return true;
    }

    /**
     * @brief Resolve one immutable exposure state from frozen frame inputs.
     *
     * `previousPresented` must be the last successfully presented state.  A
     * changed settings hash deliberately resets temporal adaptation and uses a
     * pre-exposure of one, preventing one controller's history from being
     * interpreted by another.
     */
    [[nodiscard]] inline RenderExposureResolveResult ResolveRenderExposureState(
        const RenderExposureSettings& settings,
        float32 meteredLuminance,
        float32 fixedDeltaSeconds,
        const RenderExposureState* previousPresented = nullptr) noexcept
    {
        RenderExposureResolveResult result;
        if (!IsValidRenderExposureSettings(settings))
        {
            result.code = RenderExposureResolveCode::InvalidSettings;
            return result;
        }
        if (!IsValidRenderExposureDeltaSeconds(fixedDeltaSeconds))
        {
            result.code = RenderExposureResolveCode::InvalidDeltaSeconds;
            return result;
        }
        if (!std::isfinite(meteredLuminance) || meteredLuminance < 0.0f ||
            meteredLuminance > RVX_RENDER_EXPOSURE_MAX_METERED_LUMINANCE)
        {
            result.code = RenderExposureResolveCode::InvalidMeteredLuminance;
            return result;
        }
        if (previousPresented != nullptr && !previousPresented->IsValid())
        {
            result.code = RenderExposureResolveCode::InvalidPreviousState;
            return result;
        }

        float32 targetEV100 = settings.manualEV100;
        if (settings.mode == RenderExposureMode::HistogramAuto &&
            !TryCalculateHistogramEV100(settings, meteredLuminance,
                                         targetEV100))
        {
            result.code = RenderExposureResolveCode::InvalidMeteredLuminance;
            return result;
        }

        const uint64 settingsHash = CalculateRenderExposureSettingsHash(settings);
        const bool historyCompatible = previousPresented != nullptr &&
                                       previousPresented->GetConfigurationHash() ==
                                           settingsHash;
        float32 adaptedEV100 = targetEV100;
        if (historyCompatible && settings.mode == RenderExposureMode::HistogramAuto &&
            !TryAdaptRenderExposureEV100(settings,
                                          previousPresented->GetAdaptedEV100(),
                                          targetEV100,
                                          fixedDeltaSeconds,
                                          adaptedEV100))
        {
            result.code = RenderExposureResolveCode::InvalidPreviousState;
            return result;
        }

        float32 exposureScale = 0.0f;
        if (!TryCalculateRenderExposureScale(adaptedEV100,
                                             settings.exposureCompensationEV,
                                             exposureScale))
        {
            result.code = RenderExposureResolveCode::InvalidResolvedState;
            return result;
        }

        const float32 preExposure = historyCompatible
                                        ? previousPresented->GetExposureScale()
                                        : 1.0f;
        result.state = MakeRenderExposureState(settings.mode,
                                               targetEV100,
                                               adaptedEV100,
                                               exposureScale,
                                               preExposure,
                                               settingsHash);
        if (!result.state.IsValid())
        {
            result.code = RenderExposureResolveCode::InvalidResolvedState;
            return result;
        }
        result.code = historyCompatible ? RenderExposureResolveCode::Resolved
                                        : RenderExposureResolveCode::ResolvedWithoutHistory;
        return result;
    }
} // namespace RVX
