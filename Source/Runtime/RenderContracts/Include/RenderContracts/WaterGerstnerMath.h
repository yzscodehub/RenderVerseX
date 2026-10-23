#pragma once

/**
 * @file WaterGerstnerMath.h
 * @brief Canonical CPU and GPU-boundary Gerstner math shared by Water and Render.
 *
 * Authoring packets contain float32 values.  Canonical CPU evaluation derives
 * wave number and angular frequency in float64 and reduces each wave's frozen
 * frame time by its own period before evaluating its phase.  The renderer then
 * crosses one explicit float32 boundary when it packs `waveNumber` and the
 * already-reduced temporal phase for the GPU.  The GPU mirror intentionally
 * uses those packed values and float32 local coordinates; it is therefore
 * numerically close to, but not bit-identical with, the CPU reference.
 */

#include "Core/MathTypes.h"
#include "Core/Types.h"
#include "RenderContracts/WaterRenderSnapshot.h"

#include <algorithm>
#include <cmath>

namespace RVX
{
    /** Float64 terms before the sole GPU quantization boundary. */
    struct WaterGerstnerWaveFrameTerms
    {
        float64 waveNumber = 0.0;
        float64 angularFrequency = 0.0;
        float64 temporalPhase = 0.0;
    };

    /**
     * Exact scalar payload consumed by the HLSL Gerstner loop.
     *
     * The HLSL ABI is `WaveSteepnessTemporalPhase.y = waveNumber` and
     * `.z = temporalPhase`.  Both are finite float32 values produced here.
     */
    struct WaterGerstnerGpuWaveFrameTerms
    {
        float32 waveNumber = 0.0f;
        float32 temporalPhase = 0.0f;
    };
    static_assert(sizeof(WaterGerstnerGpuWaveFrameTerms) == sizeof(float32) * 2u);

    /** Shared analytical outputs used by gameplay and the renderer reference. */
    struct WaterGerstnerSurfaceMathSample
    {
        Vec3 displacement{0.0f};
        Vec3 tangentX{1.0f, 0.0f, 0.0f};
        Vec3 tangentZ{0.0f, 0.0f, 1.0f};
        Vec3 horizontalVelocity{0.0f};
        float32 crest = 0.0f;
    };

    /**
     * Computes canonical float64 wave terms for an explicit frozen time.
     *
     * `std::remainder` keeps the time and phase bounded before trigonometry;
     * it is deterministic for an identical platform/libm input.  A single
     * common time cannot be reduced safely because every wave has its own
     * angular frequency.
     */
    [[nodiscard]] inline bool EvaluateWaterGerstnerWaveFrameTerms(
        const WaterGerstnerWave& wave,
        float64 simulationSeconds,
        WaterGerstnerWaveFrameTerms& outTerms) noexcept
    {
        outTerms = {};
        if (!std::isfinite(simulationSeconds) ||
            !std::isfinite(wave.wavelength) ||
            !std::isfinite(wave.speed) ||
            !std::isfinite(wave.phaseOffsetRadians) ||
            wave.wavelength <= 0.0f || wave.speed < 0.0f)
        {
            return false;
        }

        const float64 waveNumber = RVX_WATER_GERSTNER_TWO_PI_DOUBLE /
            static_cast<float64>(wave.wavelength);
        const float64 angularFrequency = std::sqrt(
            RVX_WATER_GERSTNER_GRAVITY_METERS_PER_SECOND_SQUARED * waveNumber) *
            static_cast<float64>(wave.speed);
        const float64 wrappedTime = angularFrequency > 0.0
            ? std::remainder(simulationSeconds,
                             RVX_WATER_GERSTNER_TWO_PI_DOUBLE / angularFrequency)
            : 0.0;
        const float64 temporalPhase = std::remainder(
            static_cast<float64>(wave.phaseOffsetRadians) -
                angularFrequency * wrappedTime,
            RVX_WATER_GERSTNER_TWO_PI_DOUBLE);
        if (!std::isfinite(waveNumber) || !std::isfinite(angularFrequency) ||
            !std::isfinite(wrappedTime) || !std::isfinite(temporalPhase))
        {
            return false;
        }
        outTerms.waveNumber = waveNumber;
        outTerms.angularFrequency = angularFrequency;
        outTerms.temporalPhase = temporalPhase;
        return true;
    }

    /** Performs the one documented CPU-to-GPU float32 precision boundary. */
    [[nodiscard]] inline bool PackWaterGerstnerWaveFrameTermsForGpu(
        const WaterGerstnerWaveFrameTerms& terms,
        WaterGerstnerGpuWaveFrameTerms& outTerms) noexcept
    {
        outTerms = {};
        if (!std::isfinite(terms.waveNumber) || !std::isfinite(terms.temporalPhase) ||
            terms.waveNumber <= 0.0)
        {
            return false;
        }
        const float32 waveNumber = static_cast<float32>(terms.waveNumber);
        const float32 temporalPhase = static_cast<float32>(terms.temporalPhase);
        if (!std::isfinite(waveNumber) || !std::isfinite(temporalPhase) ||
            waveNumber <= 0.0f)
        {
            return false;
        }
        outTerms.waveNumber = waveNumber;
        outTerms.temporalPhase = temporalPhase;
        return true;
    }

    /** Canonical float64-phase CPU evaluation used by gameplay and Render reference code. */
    [[nodiscard]] inline bool EvaluateWaterGerstnerSurfaceMath(
        const WaterGerstnerWaveSet& waves,
        const Vec2& localPositionXZ,
        float64 simulationSeconds,
        WaterGerstnerSurfaceMathSample& outSample) noexcept
    {
        outSample = {};
        if (!waves.IsStructurallyValid() || !std::isfinite(localPositionXZ.x) ||
            !std::isfinite(localPositionXZ.y) || !std::isfinite(simulationSeconds))
        {
            return false;
        }

        for (uint32 index = 0; index < waves.waveCount; ++index)
        {
            const WaterGerstnerWave& wave = waves.waves[index];
            WaterGerstnerWaveFrameTerms terms;
            if (!EvaluateWaterGerstnerWaveFrameTerms(wave, simulationSeconds, terms))
            {
                outSample = {};
                return false;
            }
            const float64 spatialPhase = terms.waveNumber *
                (static_cast<float64>(wave.direction.x) * localPositionXZ.x +
                 static_cast<float64>(wave.direction.y) * localPositionXZ.y);
            const float64 phase = std::remainder(
                spatialPhase + terms.temporalPhase,
                RVX_WATER_GERSTNER_TWO_PI_DOUBLE);
            if (!std::isfinite(phase))
            {
                outSample = {};
                return false;
            }
            const float32 sine = static_cast<float32>(std::sin(phase));
            const float32 cosine = static_cast<float32>(std::cos(phase));
            const float32 waveNumber = static_cast<float32>(terms.waveNumber);
            const float32 angularFrequency = static_cast<float32>(terms.angularFrequency);
            if (!std::isfinite(waveNumber) || !std::isfinite(angularFrequency) ||
                waveNumber <= 0.0f)
            {
                outSample = {};
                return false;
            }
            const float32 horizontalAmplitude = wave.steepness / waveNumber;
            const float32 xPhaseDerivative = waveNumber * wave.direction.x;
            const float32 zPhaseDerivative = waveNumber * wave.direction.y;
            outSample.displacement.x += wave.direction.x * horizontalAmplitude * cosine;
            outSample.displacement.y += wave.amplitude * sine;
            outSample.displacement.z += wave.direction.y * horizontalAmplitude * cosine;
            outSample.tangentX += Vec3{
                -wave.direction.x * horizontalAmplitude * sine * xPhaseDerivative,
                wave.amplitude * cosine * xPhaseDerivative,
                -wave.direction.y * horizontalAmplitude * sine * xPhaseDerivative};
            outSample.tangentZ += Vec3{
                -wave.direction.x * horizontalAmplitude * sine * zPhaseDerivative,
                wave.amplitude * cosine * zPhaseDerivative,
                -wave.direction.y * horizontalAmplitude * sine * zPhaseDerivative};
            outSample.horizontalVelocity.x += wave.direction.x * horizontalAmplitude *
                angularFrequency * sine;
            outSample.horizontalVelocity.z += wave.direction.y * horizontalAmplitude *
                angularFrequency * sine;
            outSample.crest += std::max(wave.amplitude * sine, 0.0f);
        }
        return true;
    }

    /**
     * Float32 mirror of the packed HLSL wave loop.
     *
     * The expected approximation error versus `EvaluateWaterGerstnerSurfaceMath`
     * comes only from packed float32 `k`/phase, float32 local-coordinate
     * arithmetic, and platform shader transcendental implementations.  Tests
     * use bounded tolerances rather than impossible cross-API bit parity.
     */
    [[nodiscard]] inline bool EvaluateWaterGerstnerGpuFloatMirror(
        const WaterGerstnerWaveSet& waves,
        const Vec2& localPositionXZ,
        float64 simulationSeconds,
        WaterGerstnerSurfaceMathSample& outSample) noexcept
    {
        outSample = {};
        if (!waves.IsStructurallyValid() || !std::isfinite(localPositionXZ.x) ||
            !std::isfinite(localPositionXZ.y) || !std::isfinite(simulationSeconds))
        {
            return false;
        }

        for (uint32 index = 0; index < waves.waveCount; ++index)
        {
            const WaterGerstnerWave& wave = waves.waves[index];
            WaterGerstnerWaveFrameTerms frameTerms;
            WaterGerstnerGpuWaveFrameTerms gpuTerms;
            if (!EvaluateWaterGerstnerWaveFrameTerms(wave, simulationSeconds, frameTerms) ||
                !PackWaterGerstnerWaveFrameTermsForGpu(frameTerms, gpuTerms))
            {
                outSample = {};
                return false;
            }
            const float32 phase = std::fmod(
                gpuTerms.waveNumber *
                    (wave.direction.x * localPositionXZ.x +
                     wave.direction.y * localPositionXZ.y) +
                    gpuTerms.temporalPhase,
                RVX_WATER_GERSTNER_TWO_PI_FLOAT);
            if (!std::isfinite(phase))
            {
                outSample = {};
                return false;
            }
            const float32 sine = std::sin(phase);
            const float32 cosine = std::cos(phase);
            const float32 horizontalAmplitude = wave.steepness / gpuTerms.waveNumber;
            outSample.displacement.x += wave.direction.x * horizontalAmplitude * cosine;
            outSample.displacement.y += wave.amplitude * sine;
            outSample.displacement.z += wave.direction.y * horizontalAmplitude * cosine;
            outSample.tangentX += Vec3{
                -wave.direction.x * wave.direction.x * wave.steepness * sine,
                wave.direction.x * wave.amplitude * gpuTerms.waveNumber * cosine,
                -wave.direction.x * wave.direction.y * wave.steepness * sine};
            outSample.tangentZ += Vec3{
                -wave.direction.x * wave.direction.y * wave.steepness * sine,
                wave.direction.y * wave.amplitude * gpuTerms.waveNumber * cosine,
                -wave.direction.y * wave.direction.y * wave.steepness * sine};
            outSample.crest += std::max(wave.amplitude * sine, 0.0f);
        }
        return true;
    }
} // namespace RVX
