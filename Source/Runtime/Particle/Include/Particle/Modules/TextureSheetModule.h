#pragma once

/**
 * @file TextureSheetModule.h
 * @brief Texture sheet animation module - sprite sheet animation
 */

#include "Particle/Modules/IParticleModule.h"
#include "Particle/Curves/AnimationCurve.h"
#include "Particle/ParticleTypes.h"
#include "Core/MathTypes.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace RVX::Particle
{
    /**
     * @brief Animation mode for texture sheet
     */
    enum class TextureSheetAnimationMode : uint8
    {
        WholeSheet,     ///< Animate through entire sheet
        SingleRow       ///< Animate through a single row
    };

    /**
     * @brief Time mode for animation
     */
    enum class TextureSheetTimeMode : uint8
    {
        Lifetime,       ///< Animation tied to particle lifetime
        Speed           ///< Animation at fixed frame rate
    };

    /**
     * @brief Texture sheet animation for sprite-based particles
     */
    class TextureSheetModule : public IParticleModule
    {
    public:
        /// Grid size (columns x rows)
        UVec2 tiles{4, 4};

        /// Total frame count (0 = tiles.x * tiles.y)
        uint32 frameCount = 0;

        /// Animation mode
        TextureSheetAnimationMode mode = TextureSheetAnimationMode::WholeSheet;

        /// Time mode
        TextureSheetTimeMode timeMode = TextureSheetTimeMode::Lifetime;

        /// Frame rate (only used in Speed time mode)
        float frameRate = 30.0f;

        /// Starting frame
        uint32 startFrame = 0;

        /// Randomize starting frame
        bool randomStartFrame = false;

        /// Number of animation cycles over lifetime
        float cycles = 1.0f;

        /// Frame over time curve (for Lifetime time mode)
        AnimationCurve frameOverTime = AnimationCurve::Linear();

        /// Row index (for SingleRow mode)
        uint32 rowIndex = 0;

        /// Use random row
        bool randomRow = false;

        const char* GetTypeName() const override { return "TextureSheetModule"; }
        ModuleStage GetStage() const override { return ModuleStage::Render; }

        size_t GetGPUDataSize() const override 
        { 
            return sizeof(TextureSheetGPUData);
        }

        void GetGPUData(void* outData, size_t maxSize) const override
        {
            if (maxSize < sizeof(TextureSheetGPUData)) return;
            
            TextureSheetGPUData data;
            data.tileSize = Vec2(1.0f / static_cast<float>(tiles.x),
                                 1.0f / static_cast<float>(tiles.y));
            data.tileCount = Vec2(static_cast<float>(tiles.x),
                                  static_cast<float>(tiles.y));
            data.frameCount = static_cast<float>(
                frameCount > 0 ? frameCount : tiles.x * tiles.y);
            data.frameRate = frameRate;
            data.startFrame = startFrame;
            data.randomStartFrame = randomStartFrame ? 1 : 0;
            
            std::memcpy(outData, &data, sizeof(data));
        }

        /// Calculate UV offset and scale for a given frame
        void GetUVTransform(uint32 frame, Vec2& outOffset, Vec2& outScale) const
        {
            uint32 totalFrames = frameCount > 0 ? frameCount : tiles.x * tiles.y;
            frame = frame % totalFrames;
            
            uint32 col = frame % tiles.x;
            uint32 row = frame / tiles.x;
            
            outScale = Vec2(1.0f / static_cast<float>(tiles.x),
                           1.0f / static_cast<float>(tiles.y));
            outOffset = Vec2(static_cast<float>(col) * outScale.x,
                            static_cast<float>(row) * outScale.y);
        }

        /**
         * @brief Resolve an atlas frame using only explicit simulation inputs.
         *
         * The legacy normalized-age overload could not implement Speed mode
         * and silently ignored random row/start settings.  This overload is
         * used when sealing CPU render packets and never touches wall clock or
         * process entropy.
         */
        [[nodiscard]] uint32 GetFrameIndex(float ageSeconds,
                                           float lifetimeSeconds,
                                           uint32 deterministicSeed,
                                           uint64 deterministicInstanceKey,
                                           uint32 stableParticleId,
                                           uint64 simulationTick,
                                           float fixedDeltaSeconds) const noexcept
        {
            static_cast<void>(simulationTick);
            if (tiles.x == 0U || tiles.y == 0U || !std::isfinite(ageSeconds) ||
                !std::isfinite(lifetimeSeconds) || lifetimeSeconds <= 0.0f ||
                !std::isfinite(frameRate) || frameRate < 0.0f ||
                !std::isfinite(cycles) || cycles < 0.0f ||
                !std::isfinite(fixedDeltaSeconds) || fixedDeltaSeconds < 0.0f)
            {
                return 0U;
            }

            const uint64 totalTiles64 = static_cast<uint64>(tiles.x) *
                                         static_cast<uint64>(tiles.y);
            if (totalTiles64 == 0U || totalTiles64 > RVX_INVALID_INDEX)
            {
                return 0U;
            }
            const uint32 totalTiles = static_cast<uint32>(totalTiles64);

            const uint64 identity = MixDeterministicIdentity(
                deterministicSeed,
                deterministicInstanceKey,
                stableParticleId);
            const uint32 selectedRow = mode == TextureSheetAnimationMode::SingleRow
                ? (randomRow ? static_cast<uint32>(identity % tiles.y)
                             : rowIndex % tiles.y)
                : 0U;
            const uint32 rowStart = mode == TextureSheetAnimationMode::SingleRow
                ? selectedRow * tiles.x
                : 0U;
            const uint32 localFirstFrame = mode == TextureSheetAnimationMode::SingleRow
                ? startFrame % tiles.x
                : std::min(startFrame, totalTiles - 1U);
            const uint32 rangeStart = rowStart + localFirstFrame;
            const uint32 availableFrames = mode == TextureSheetAnimationMode::SingleRow
                ? tiles.x - localFirstFrame
                : totalTiles - localFirstFrame;
            const uint32 configuredFrames = frameCount == 0U
                ? availableFrames
                : std::min(frameCount, availableFrames);
            if (configuredFrames == 0U)
            {
                return 0U;
            }

            const uint32 randomizedStart = randomStartFrame
                ? static_cast<uint32>((identity >> 32U) % configuredFrames)
                : 0U;
            uint32 animatedFrame = 0U;
            if (timeMode == TextureSheetTimeMode::Lifetime)
            {
                const float normalizedAge = std::clamp(ageSeconds / lifetimeSeconds,
                                                       0.0f,
                                                       1.0f);
                float progress = frameOverTime.Evaluate(normalizedAge) * cycles;
                progress -= std::floor(progress);
                animatedFrame = std::min(
                    static_cast<uint32>(progress * static_cast<float>(configuredFrames)),
                    configuredFrames - 1U);
            }
            else
            {
                const float elapsed = std::max(0.0f, ageSeconds);
                const uint64 steps = static_cast<uint64>(
                    std::floor(elapsed * frameRate));
                animatedFrame = static_cast<uint32>(steps % configuredFrames);
            }

            return rangeStart +
                ((randomizedStart + animatedFrame) % configuredFrames);
        }

        /** @brief Compatibility helper for non-packet editor tooling. */
        [[nodiscard]] uint32 GetFrameIndex(float normalizedAge) const noexcept
        {
            return GetFrameIndex(normalizedAge,
                                 1.0f,
                                 0U,
                                 1U,
                                 0U,
                                 0U,
                                 0.0f);
        }

    private:
        [[nodiscard]] static uint64 MixDeterministicIdentity(
            uint32 seed,
            uint64 instanceKey,
            uint32 particleId) noexcept
        {
            uint64 value = (static_cast<uint64>(seed) << 32U) ^ instanceKey;
            value ^= static_cast<uint64>(particleId) * 0xbf58476d1ce4e5b9ull;
            value ^= value >> 30U;
            value *= 0xbf58476d1ce4e5b9ull;
            value ^= value >> 27U;
            value *= 0x94d049bb133111ebull;
            return value ^ (value >> 31U);
        }
    };

} // namespace RVX::Particle
