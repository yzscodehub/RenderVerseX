/**
 * @file IOcclusionProvider.h
 * @brief Interface for audio occlusion calculation
 */

#pragma once

#include "Audio/AudioTypes.h"

namespace RVX::Audio
{

/**
 * @brief Occlusion result
 */
struct OcclusionResult
{
    float occlusion = 0.0f;     ///< 0 = no occlusion, 1 = fully occluded
    float obstruction = 0.0f;   ///< Direct path blocking
    float transmission = 1.0f;  ///< How much sound passes through
    
    // Material-based filtering
    float lowPassCutoff = 20000.0f;  ///< Suggested low-pass cutoff
    float volumeScale = 1.0f;        ///< Volume multiplier
};

/**
 * @brief Interface for audio occlusion providers
 * 
 * Occlusion providers calculate how much a sound source
 * is blocked by geometry between it and the listener.
 * 
 * Implementations can use:
 * - Raycasting against physics geometry
 * - Portal-based propagation
 * - Pre-computed occlusion data
 * - Simplified bounding volume tests
 */
class IOcclusionProvider
{
public:
    virtual ~IOcclusionProvider() = default;

    /**
     * @brief Calculate occlusion between source and listener
     * @param sourcePosition Sound source position
     * @param listenerPosition Listener position
     * @return Occlusion result
     */
    virtual OcclusionResult CalculateOcclusion(
        const Vec3& sourcePosition,
        const Vec3& listenerPosition) = 0;

    /**
     * @brief Calculate occlusion with multiple sample points
     * 
     * For larger sound sources, sampling multiple points
     * provides smoother occlusion results.
     */
    virtual OcclusionResult CalculateOcclusionMultiSample(
        const Vec3& sourcePosition,
        const Vec3& listenerPosition,
        float sourceRadius,
        int sampleCount = 4) = 0;

    /**
     * @brief Enable/disable the provider
     */
    virtual void SetEnabled(bool enabled) = 0;
    virtual bool IsEnabled() const = 0;

    /**
     * @brief Update the provider (call each frame if needed)
     */
    virtual void Update(float deltaTime) { (void)deltaTime; }
};

/**
 * @brief Simple raycast-based occlusion provider
 * 
 * Uses physics raycasts to determine occlusion.
 * Requires integration with the Physics module.
 */
class RaycastOcclusionProvider : public IOcclusionProvider
{
public:
    RaycastOcclusionProvider() = default;
    ~RaycastOcclusionProvider() override = default;

    // =========================================================================
    // IOcclusionProvider Interface
    // =========================================================================

    OcclusionResult CalculateOcclusion(
        const Vec3& sourcePosition,
        const Vec3& listenerPosition) override;

    OcclusionResult CalculateOcclusionMultiSample(
        const Vec3& sourcePosition,
        const Vec3& listenerPosition,
        float sourceRadius,
        int sampleCount = 4) override;

    void SetEnabled(bool enabled) override { m_enabled = enabled; }
    bool IsEnabled() const override { return m_enabled; }

    // =========================================================================
    // Configuration
    // =========================================================================

    /**
     * @brief Set maximum occlusion distance
     */
    void SetMaxDistance(float distance) { m_maxDistance = distance; }

    /**
     * @brief Set occlusion per hit
     */
    void SetOcclusionPerHit(float occlusion) { m_occlusionPerHit = occlusion; }

    /**
     * @brief Set low-pass cutoff reduction per hit
     */
    void SetLowPassReduction(float reduction) { m_lowPassReduction = reduction; }

private:
    bool m_enabled = true;
    float m_maxDistance = 100.0f;
    float m_occlusionPerHit = 0.5f;
    float m_lowPassReduction = 2000.0f;

    // Raycast interface (to be connected to Physics)
    // For now, this is a stub that will need physics integration
    bool Raycast(const Vec3& start, const Vec3& end, int& hitCount);
};

/**
 * @brief No-op occlusion provider (always returns no occlusion)
 */
class NullOcclusionProvider : public IOcclusionProvider
{
public:
    OcclusionResult CalculateOcclusion(
        const Vec3& sourcePosition,
        const Vec3& listenerPosition) override
    {
        (void)sourcePosition;
        (void)listenerPosition;
        return OcclusionResult{};
    }

    OcclusionResult CalculateOcclusionMultiSample(
        const Vec3& sourcePosition,
        const Vec3& listenerPosition,
        float sourceRadius,
        int sampleCount) override
    {
        (void)sourcePosition;
        (void)listenerPosition;
        (void)sourceRadius;
        (void)sampleCount;
        return OcclusionResult{};
    }

    void SetEnabled(bool enabled) override { m_enabled = enabled; }
    bool IsEnabled() const override { return m_enabled; }

private:
    bool m_enabled = true;
};

} // namespace RVX::Audio
