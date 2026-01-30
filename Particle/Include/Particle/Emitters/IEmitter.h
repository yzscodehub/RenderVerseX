#pragma once

/**
 * @file IEmitter.h
 * @brief Base interface for particle emitters
 */

#include "Particle/ParticleTypes.h"
#include "Core/MathTypes.h"
#include <string>

namespace RVX::Particle
{
    /**
     * @brief Base interface for all particle emitters
     * 
     * Emitters define where and how particles are spawned.
     * Each emitter type provides different spatial distribution patterns.
     */
    class IEmitter
    {
    public:
        virtual ~IEmitter() = default;

        // =====================================================================
        // Type Information
        // =====================================================================

        /// Get the emitter type name (for serialization/debugging)
        virtual const char* GetTypeName() const = 0;

        /// Get the emitter shape type
        virtual EmitterShape GetShape() const = 0;

        // =====================================================================
        // GPU Data Generation
        // =====================================================================

        /// Generate GPU-ready emitter parameters
        virtual void GetEmitParams(EmitterGPUData& outData) const = 0;

        // =====================================================================
        // Emission Settings
        // =====================================================================

        /// Continuous emission rate (particles per second)
        float emissionRate = 10.0f;

        /// Burst emission count (0 = disabled)
        uint32 burstCount = 0;

        /// Interval between bursts (seconds)
        float burstInterval = 0.0f;

        /// Time until first burst (seconds)
        float burstDelay = 0.0f;

        /// Number of burst cycles (0 = infinite)
        uint32 burstCycles = 0;

        // =====================================================================
        // Initial Particle Properties
        // =====================================================================

        /// Particle lifetime range (seconds)
        FloatRange initialLifetime{1.0f, 2.0f};

        /// Initial speed range
        FloatRange initialSpeed{1.0f, 5.0f};

        /// Initial velocity direction (normalized, then multiplied by speed)
        Vec3Range initialVelocityDirection{{0.0f, 1.0f, 0.0f}, {0.0f, 1.0f, 0.0f}};

        /// Whether to use shape-based velocity (outward from shape)
        bool useShapeVelocity = true;

        /// Initial color range
        ColorRange initialColor{Vec4(1.0f), Vec4(1.0f)};

        /// Initial size range
        FloatRange initialSize{0.1f, 0.2f};

        /// Initial rotation range (degrees)
        FloatRange initialRotation{0.0f, 360.0f};

        /// Rotation speed range (degrees per second)
        FloatRange rotationSpeed{0.0f, 0.0f};

        // =====================================================================
        // Transform
        // =====================================================================

        /// Local position offset
        Vec3 position{0.0f, 0.0f, 0.0f};

        /// Local rotation (Euler angles in degrees)
        Vec3 rotation{0.0f, 0.0f, 0.0f};

        /// Local scale
        Vec3 scale{1.0f, 1.0f, 1.0f};

        /// Get local transform matrix
        Mat4 GetLocalTransform() const
        {
            Mat4 result = Mat4Identity();
            result = translate(result, position);
            result = result * QuatToMat4(QuatFromEuler(radians(rotation)));
            result = RVX::scale(result, scale);
            return result;
        }

        // =====================================================================
        // Enabled State
        // =====================================================================

        bool enabled = true;

    protected:
        /// Helper to fill common emission parameters
        void FillCommonParams(EmitterGPUData& data) const
        {
            data.transform = GetLocalTransform();
            data.lifetimeParams = Vec4(initialLifetime.min, initialLifetime.max, 0.0f, 0.0f);
            data.sizeParams = Vec4(initialSize.min, initialSize.max, 0.0f, 0.0f);
            data.colorStart = initialColor.min;  // Start color (gradient handles over lifetime)
            data.rotationParams = Vec4(
                radians(initialRotation.min),
                radians(initialRotation.max),
                radians(rotationSpeed.min),
                radians(rotationSpeed.max)
            );
            data.velocityParams = Vec4(
                initialSpeed.min,
                initialSpeed.max,
                useShapeVelocity ? 1.0f : 0.0f,
                0.0f
            );
        }
    };

} // namespace RVX::Particle
