#pragma once

/**
 * @file ParticleTypes.h
 * @brief Core type definitions for the particle system
 * 
 * Defines GPU particle data structures, enums for emitter shapes,
 * render modes, blend modes, and utility range types.
 */

#include "Core/Types.h"
#include "Core/MathTypes.h"
#include <cstdint>
#include <cstring>
#include <algorithm>

namespace RVX::Particle
{
    // =========================================================================
    // Constants
    // =========================================================================
    
    /// Particle flag bits
    constexpr uint32 PARTICLE_FLAG_ALIVE = 0x01;
    constexpr uint32 PARTICLE_FLAG_COLLISION = 0x02;
    constexpr uint32 PARTICLE_FLAG_TRAIL = 0x04;
    
    /// Default maximum particles per system
    constexpr uint32 RVX_DEFAULT_MAX_PARTICLES = 10000;
    
    /// Maximum emitters per system
    constexpr uint32 RVX_MAX_EMITTERS = 8;
    
    /// Maximum modules per system
    constexpr uint32 RVX_MAX_MODULES = 16;
    
    /// Maximum collision planes
    constexpr uint32 RVX_MAX_COLLISION_PLANES = 8;

    // =========================================================================
    // GPU Particle Data Structure
    // =========================================================================

    /**
     * @brief GPU-friendly particle data structure (64 bytes, cache-aligned)
     * 
     * This structure is used for GPU simulation and rendering.
     * The layout is optimized for cache coherency and GPU access patterns.
     */
    struct GPUParticle
    {
        Vec3 position;           ///< World position (12 bytes)
        float lifetime;          ///< Total lifetime in seconds (4 bytes)
        Vec3 velocity;           ///< Velocity vector (12 bytes)
        float age;               ///< Current age in seconds (4 bytes)
        Vec4 color;              ///< RGBA color (16 bytes)
        Vec2 size;               ///< Width and height (8 bytes)
        float rotation;          ///< Rotation in radians (4 bytes)
        uint32 flags;            ///< Status flags (alive, emitter id, etc.) (4 bytes)
    };
    static_assert(sizeof(GPUParticle) == 64, "GPUParticle must be 64 bytes");

    /**
     * @brief CPU-side particle for fallback simulation
     */
    struct CPUParticle
    {
        Vec3 position;
        Vec3 velocity;
        Vec4 color;
        Vec4 startColor;
        Vec2 size;
        Vec2 startSize;
        float lifetime;
        float age;
        float rotation;
        float rotationSpeed;
        uint32 flags;
        uint32 emitterIndex;
        uint32 randomSeed;
        uint32 textureFrame;
    };

    // =========================================================================
    // Enumerations
    // =========================================================================

    /**
     * @brief Emitter shape types
     */
    enum class EmitterShape : uint8
    {
        Point,          ///< Single point emission
        Box,            ///< Box volume emission
        Sphere,         ///< Sphere volume/surface emission
        Hemisphere,     ///< Hemisphere volume/surface emission
        Cone,           ///< Cone volume emission
        Circle,         ///< Circle edge/area emission
        Edge,           ///< Line segment emission
        Mesh            ///< Mesh surface emission
    };

    /**
     * @brief Particle rendering mode
     */
    enum class ParticleRenderMode : uint8
    {
        Billboard,              ///< Camera-facing quad
        StretchedBillboard,     ///< Velocity-stretched quad
        HorizontalBillboard,    ///< Horizontal plane quad
        VerticalBillboard,      ///< Vertical plane quad
        Mesh,                   ///< 3D mesh particles
        Trail                   ///< Trail/ribbon rendering
    };

    /**
     * @brief Particle blend mode
     */
    enum class ParticleBlendMode : uint8
    {
        Additive,           ///< Additive blending (fire, sparks)
        AlphaBlend,         ///< Standard alpha blending
        Multiply,           ///< Multiply blending
        Premultiplied       ///< Premultiplied alpha
    };

    /**
     * @brief Particle simulation space
     */
    enum class ParticleSpace : uint8
    {
        World,      ///< Particles move in world space
        Local       ///< Particles move relative to emitter
    };

    /**
     * @brief Particle sorting mode
     */
    enum class ParticleSortMode : uint8
    {
        None,               ///< No sorting
        ByDistance,         ///< Sort by distance to camera (back-to-front)
        ByAge,              ///< Sort by particle age
        ByDepth             ///< Sort by depth (for OIT)
    };

    // =========================================================================
    // Range Types (for random value generation)
    // =========================================================================

    /**
     * @brief Float range for random generation
     */
    struct FloatRange
    {
        float min = 0.0f;
        float max = 1.0f;

        FloatRange() = default;
        FloatRange(float value) : min(value), max(value) {}
        FloatRange(float minVal, float maxVal) : min(minVal), max(maxVal) {}

        /// Get a value at t (0-1) between min and max
        float Lerp(float t) const { return min + (max - min) * t; }
        
        /// Check if this is a constant value
        bool IsConstant() const { return min == max; }
    };

    /**
     * @brief Vec2 range for random generation
     */
    struct Vec2Range
    {
        Vec2 min{0.0f, 0.0f};
        Vec2 max{1.0f, 1.0f};

        Vec2Range() = default;
        Vec2Range(const Vec2& value) : min(value), max(value) {}
        Vec2Range(const Vec2& minVal, const Vec2& maxVal) : min(minVal), max(maxVal) {}

        Vec2 Lerp(float t) const { return min + (max - min) * t; }
    };

    /**
     * @brief Vec3 range for random generation
     */
    struct Vec3Range
    {
        Vec3 min{0.0f, 0.0f, 0.0f};
        Vec3 max{1.0f, 1.0f, 1.0f};

        Vec3Range() = default;
        Vec3Range(const Vec3& value) : min(value), max(value) {}
        Vec3Range(const Vec3& minVal, const Vec3& maxVal) : min(minVal), max(maxVal) {}

        Vec3 Lerp(float t) const { return min + (max - min) * t; }
    };

    /**
     * @brief Vec4/Color range for random generation
     */
    struct Vec4Range
    {
        Vec4 min{1.0f, 1.0f, 1.0f, 1.0f};
        Vec4 max{1.0f, 1.0f, 1.0f, 1.0f};

        Vec4Range() = default;
        Vec4Range(const Vec4& value) : min(value), max(value) {}
        Vec4Range(const Vec4& minVal, const Vec4& maxVal) : min(minVal), max(maxVal) {}

        Vec4 Lerp(float t) const { return min + (max - min) * t; }
    };

    using ColorRange = Vec4Range;

    // =========================================================================
    // GPU Data Structures (for constant buffers)
    // =========================================================================

    /**
     * @brief Emitter GPU data for compute shader
     */
    struct EmitterGPUData
    {
        Mat4 transform;              ///< Emitter world transform
        Vec4 shapeParams;            ///< Shape-specific parameters (radius, angle, etc.)
        Vec4 velocityParams;         ///< Velocity direction and speed range
        Vec4 lifetimeParams;         ///< x=minLife, y=maxLife, z=unused, w=unused
        Vec4 sizeParams;             ///< x=minSize, y=maxSize, z=unused, w=unused
        Vec4 colorStart;             ///< Starting color
        Vec4 rotationParams;         ///< x=minRot, y=maxRot, z=rotSpeed, w=unused
        uint32 emitterShape;         ///< EmitterShape enum value
        uint32 emitCount;            ///< Number of particles to emit this frame
        uint32 randomSeed;           ///< Random seed for this frame
        uint32 flags;                ///< Emitter flags
    };

    /**
     * @brief Simulation constants for compute shader
     */
    struct SimulationGPUData
    {
        Vec4 gravity;                ///< xyz=gravity, w=unused
        Vec4 forceParams;            ///< xyz=constant force, w=drag
        Vec4 noiseParams;            ///< x=strength, y=frequency, z=scrollSpeed, w=octaves
        Vec4 collisionPlanes[RVX_MAX_COLLISION_PLANES];  ///< xyz=normal, w=distance
        float deltaTime;
        float totalTime;
        uint32 aliveCount;
        uint32 maxParticles;
        uint32 collisionPlaneCount;
        float collisionBounce;
        float collisionLifetimeLoss;
        float collisionRadiusScale;
        uint32 noiseEnabled;
        uint32 collisionEnabled;
        uint32 pad[2];
    };

    /**
     * @brief Render constants for vertex/pixel shader
     */
    struct RenderGPUData
    {
        Mat4 viewMatrix;
        Mat4 projMatrix;
        Mat4 viewProjMatrix;
        Vec4 cameraPosition;         ///< xyz=position, w=unused
        Vec4 cameraRight;            ///< xyz=right vector, w=unused
        Vec4 cameraUp;               ///< xyz=up vector, w=unused
        Vec4 cameraForward;          ///< xyz=forward vector, w=unused
        Vec2 screenSize;
        Vec2 invScreenSize;
        float softParticleFadeDistance;
        float softParticleContrast;
        uint32 softParticleEnabled;
        uint32 pad;
    };

    /**
     * @brief Texture sheet animation parameters
     */
    struct TextureSheetGPUData
    {
        Vec2 tileSize;               ///< 1.0 / tiles
        Vec2 tileCount;              ///< Number of tiles
        float frameCount;
        float frameRate;
        uint32 startFrame;
        uint32 randomStartFrame;
    };

    // =========================================================================
    // Indirect Draw Arguments
    // =========================================================================

    /**
     * @brief Indirect draw arguments (matches D3D12/Vulkan DrawIndexedIndirect)
     */
    struct IndirectDrawArgs
    {
        uint32 indexCountPerInstance;
        uint32 instanceCount;
        uint32 startIndexLocation;
        int32 baseVertexLocation;
        uint32 startInstanceLocation;
    };

    /**
     * @brief Indirect dispatch arguments (matches D3D12/Vulkan DispatchIndirect)
     */
    struct IndirectDispatchArgs
    {
        uint32 threadGroupCountX;
        uint32 threadGroupCountY;
        uint32 threadGroupCountZ;
    };

} // namespace RVX::Particle
