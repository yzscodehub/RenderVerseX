/**
 * @file ParticleEditor.h
 * @brief Particle system editor panel
 */

#pragma once

#include "Editor/Panels/IEditorPanel.h"
#include "Core/MathTypes.h"
#include <memory>
#include <string>

// Forward declarations
namespace RVX::Particle
{
    class ParticleSystem;
    class ParticleSystemInstance;
    class AnimationCurve;
    class GradientCurve;
    class IEmitter;
    class IParticleModule;
    struct FloatRange;
    struct Vec3Range;
    struct ColorRange;
}

namespace RVX::Editor
{

/**
 * @brief Particle system editor panel
 * 
 * Provides visual editing of particle systems including:
 * - System properties
 * - Emitter configuration
 * - Module parameters
 * - Curve and gradient editors
 * - Real-time preview
 */
class ParticleEditorPanel : public IEditorPanel
{
public:
    ParticleEditorPanel();
    ~ParticleEditorPanel() override;

    // =========================================================================
    // IEditorPanel Interface
    // =========================================================================

    const char* GetName() const override { return "Particle"; }
    const char* GetIcon() const override { return "particle"; }
    void OnInit() override;
    void OnGUI() override;
    void OnUpdate(float deltaTime) override;

    // =========================================================================
    // System Management
    // =========================================================================

    void SetParticleSystem(std::shared_ptr<Particle::ParticleSystem> system);
    std::shared_ptr<Particle::ParticleSystem> GetParticleSystem() const { return m_system; }
    bool HasSystem() const { return m_system != nullptr; }

    // =========================================================================
    // Preview Control
    // =========================================================================

    void SetPreviewEnabled(bool enabled) { m_previewEnabled = enabled; }
    bool IsPreviewEnabled() const { return m_previewEnabled; }

    void PlayPreview();
    void PausePreview();
    void StopPreview();
    void RestartPreview();

private:
    // UI Drawing
    void DrawToolbar();
    void DrawSystemProperties();
    void DrawEmitterList();
    void DrawEmitterProperties(int index);
    void DrawModuleList();
    void DrawModuleProperties(int index);
    void DrawRenderingProperties();
    void DrawLODProperties();
    void DrawPreviewWindow();
    
    // Specialized editors
    void DrawCurveEditor(const char* label, float* curve, int numKeys);
    void DrawGradientEditor(const char* label, Vec4* colors, float* times, int numKeys);
    void DrawFloatRangeEditor(const char* label, float& min, float& max);
    void DrawVec3RangeEditor(const char* label, Vec3& min, Vec3& max);
    void DrawColorRangeEditor(const char* label, Vec4& min, Vec4& max);

    // State
    std::shared_ptr<Particle::ParticleSystem> m_system;
    Particle::ParticleSystemInstance* m_previewInstance = nullptr;

    // UI State
    bool m_previewEnabled = true;
    bool m_previewPaused = false;
    float m_previewTime = 0.0f;
    int m_selectedEmitter = -1;
    int m_selectedModule = -1;

    // Curve editor state
    int m_selectedCurveKey = -1;
    bool m_curveEditorOpen = false;

    // Gradient editor state
    int m_selectedGradientKey = -1;
    bool m_gradientEditorOpen = false;
};

} // namespace RVX::Editor
