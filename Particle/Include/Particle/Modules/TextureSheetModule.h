#pragma once

/**
 * @file TextureSheetModule.h
 * @brief Texture sheet animation module - sprite sheet animation
 */

#include "Particle/Modules/IParticleModule.h"
#include "Particle/Curves/AnimationCurve.h"
#include "Particle/ParticleTypes.h"
#include "Core/MathTypes.h"
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

        /// Calculate frame index for a given normalized age
        uint32 GetFrameIndex(float normalizedAge) const
        {
            uint32 totalFrames = frameCount > 0 ? frameCount : tiles.x * tiles.y;
            
            if (timeMode == TextureSheetTimeMode::Lifetime)
            {
                float t = frameOverTime.Evaluate(normalizedAge);
                t = t * cycles;
                t = t - std::floor(t);  // Wrap to 0-1
                return static_cast<uint32>(t * static_cast<float>(totalFrames)) % totalFrames;
            }
            else
            {
                // Speed mode - would need actual time, not age
                return startFrame;
            }
        }
    };

} // namespace RVX::Particle
