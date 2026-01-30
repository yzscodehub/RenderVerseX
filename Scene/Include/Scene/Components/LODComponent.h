#pragma once

/**
 * @file LODComponent.h
 * @brief Level of Detail component for mesh switching
 * 
 * LODComponent automatically switches between mesh LODs
 * based on screen size or distance from camera.
 */

#include "Scene/Component.h"
#include "Core/MathTypes.h"
#include "Resource/ResourceHandle.h"
#include "Resource/Types/MeshResource.h"
#include "Resource/Types/MaterialResource.h"
#include <vector>

namespace RVX
{

/**
 * @brief LOD fade mode
 */
enum class LODFadeMode : uint8_t
{
    None = 0,           // Instant switch
    CrossFade,          // Blend between LODs
    SpeedTree           // SpeedTree-style dithering
};

/**
 * @brief Single LOD level definition
 */
struct LODLevel
{
    /// Screen size threshold (0.0 - 1.0, relative to screen height)
    /// LOD activates when object is smaller than this
    float screenSizeThreshold = 0.5f;

    /// Optional: fixed distance threshold (used if > 0)
    float distanceThreshold = 0.0f;

    /// Mesh for this LOD level
    Resource::ResourceHandle<Resource::MeshResource> mesh;

    /// Optional: Material override for this LOD (can be simpler)
    Resource::ResourceHandle<Resource::MaterialResource> material;

    /// Fade transition width (for cross-fade mode)
    float fadeWidth = 0.1f;

    /// Shadow casting (can disable shadows for distant LODs)
    bool castShadows = true;

    /// Receive shadows
    bool receiveShadows = true;
};

/**
 * @brief LOD component for automatic level of detail switching
 * 
 * Features:
 * - Screen-size based LOD selection
 * - Distance-based LOD selection
 * - Cross-fade transitions
 * - Per-LOD material overrides
 * - LOD bias control
 * - Culling at lowest LOD
 * 
 * Usage:
 * @code
 * auto* entity = scene->CreateEntity("Tree");
 * auto* lod = entity->AddComponent<LODComponent>();
 * 
 * // Add LOD levels (high to low detail)
 * LODLevel level0;
 * level0.screenSizeThreshold = 0.6f;
 * level0.mesh = highDetailMesh;
 * lod->AddLODLevel(level0);
 * 
 * LODLevel level1;
 * level1.screenSizeThreshold = 0.3f;
 * level1.mesh = mediumDetailMesh;
 * lod->AddLODLevel(level1);
 * 
 * LODLevel level2;
 * level2.screenSizeThreshold = 0.1f;
 * level2.mesh = lowDetailMesh;
 * lod->AddLODLevel(level2);
 * 
 * lod->SetFadeMode(LODFadeMode::CrossFade);
 * @endcode
 */
class LODComponent : public Component
{
public:
    LODComponent() = default;
    ~LODComponent() override = default;

    // =========================================================================
    // Component Interface
    // =========================================================================

    const char* GetTypeName() const override { return "LOD"; }

    void OnAttach() override;
    void OnDetach() override;
    void Tick(float deltaTime) override;

    // =========================================================================
    // Spatial Bounds
    // =========================================================================

    bool ProvidesBounds() const override { return true; }
    AABB GetLocalBounds() const override;

    // =========================================================================
    // LOD Levels
    // =========================================================================

    /// Add a LOD level (should be added in order: high detail first)
    void AddLODLevel(const LODLevel& level);

    /// Insert LOD level at specific index
    void InsertLODLevel(size_t index, const LODLevel& level);

    /// Remove LOD level at index
    void RemoveLODLevel(size_t index);

    /// Clear all LOD levels
    void ClearLODLevels();

    /// Get LOD level count
    size_t GetLODLevelCount() const { return m_levels.size(); }

    /// Get LOD level by index
    const LODLevel* GetLODLevel(size_t index) const;
    LODLevel* GetLODLevel(size_t index);

    /// Get all LOD levels
    const std::vector<LODLevel>& GetLODLevels() const { return m_levels; }

    // =========================================================================
    // Current LOD State
    // =========================================================================

    /// Get currently active LOD index
    int GetCurrentLOD() const { return m_currentLOD; }

    /// Force a specific LOD (disable automatic selection)
    void ForceLOD(int lodIndex);

    /// Resume automatic LOD selection
    void ResumeAutoLOD() { m_forcedLOD = -1; }

    /// Is LOD forced?
    bool IsLODForced() const { return m_forcedLOD >= 0; }

    /// Get fade transition progress (0-1, for cross-fade)
    float GetFadeProgress() const { return m_fadeProgress; }

    /// Is currently transitioning between LODs?
    bool IsTransitioning() const { return m_isTransitioning; }

    // =========================================================================
    // LOD Settings
    // =========================================================================

    /// LOD bias (negative = higher quality, positive = lower quality)
    float GetLODBias() const { return m_lodBias; }
    void SetLODBias(float bias) { m_lodBias = bias; }

    /// Fade mode for transitions
    LODFadeMode GetFadeMode() const { return m_fadeMode; }
    void SetFadeMode(LODFadeMode mode) { m_fadeMode = mode; }

    /// Cross-fade duration in seconds
    float GetCrossFadeDuration() const { return m_crossFadeDuration; }
    void SetCrossFadeDuration(float duration) { m_crossFadeDuration = duration; }

    /// Use distance-based LOD instead of screen size
    bool UseDistanceBasedLOD() const { return m_useDistanceLOD; }
    void SetUseDistanceBasedLOD(bool use) { m_useDistanceLOD = use; }

    // =========================================================================
    // Culling
    // =========================================================================

    /// Cull the object when below minimum screen size
    bool AutoCullEnabled() const { return m_autoCull; }
    void SetAutoCull(bool enable) { m_autoCull = enable; }

    /// Minimum screen size before culling (if autoCull enabled)
    float GetCullScreenSize() const { return m_cullScreenSize; }
    void SetCullScreenSize(float size) { m_cullScreenSize = size; }

    /// Is currently culled?
    bool IsCulled() const { return m_isCulled; }

    // =========================================================================
    // LOD Calculation
    // =========================================================================

    /// Calculate screen size for a given camera
    float CalculateScreenSize(const Vec3& cameraPosition, float fov, float screenHeight) const;

    /// Calculate appropriate LOD for given screen size
    int CalculateLODForScreenSize(float screenSize) const;

    /// Calculate appropriate LOD for given distance
    int CalculateLODForDistance(float distance) const;

    /// Update LOD based on camera (called by render system)
    void UpdateLOD(const Vec3& cameraPosition, float fov, float screenHeight);

private:
    void UpdateTransition(float deltaTime);
    AABB GetLODBounds(size_t lodIndex) const;

    std::vector<LODLevel> m_levels;

    // Current state
    int m_currentLOD = 0;
    int m_previousLOD = 0;
    int m_forcedLOD = -1;

    // Transition state
    bool m_isTransitioning = false;
    float m_fadeProgress = 1.0f;

    // Settings
    float m_lodBias = 0.0f;
    LODFadeMode m_fadeMode = LODFadeMode::None;
    float m_crossFadeDuration = 0.3f;
    bool m_useDistanceLOD = false;

    // Culling
    bool m_autoCull = true;
    float m_cullScreenSize = 0.01f;
    bool m_isCulled = false;

    // Cached values
    float m_lastScreenSize = 1.0f;
};

} // namespace RVX
