/**
 * @file Inspector.h
 * @brief Property inspector panel
 */

#pragma once

#include "Editor/Panels/IEditorPanel.h"
#include "Core/Serialization/PropertyReflection.h"
#include <string>

namespace RVX
{
    class Entity;
    class Component;
}

namespace RVX::Editor
{

/**
 * @brief Inspector panel for editing entity/component properties
 */
class InspectorPanel : public IEditorPanel
{
public:
    InspectorPanel();
    ~InspectorPanel() override = default;

    // =========================================================================
    // IEditorPanel Interface
    // =========================================================================

    const char* GetName() const override { return "Inspector"; }
    const char* GetIcon() const override { return "inspector"; }
    void OnGUI() override;

private:
    // Entity inspection
    void DrawEntityHeader(Entity* entity);
    void DrawEntityInspector(Entity* entity);
    void DrawTransformComponent(Entity* entity);
    void DrawComponentInspector(Component* component);
    void DrawComponentHeader(Component* component, bool& removeRequested);
    void DrawAddComponentMenu(Entity* entity);

    // Property drawing
    void DrawProperty(const Property& prop, void* instance);
    void DrawBoolProperty(const Property& prop, void* instance);
    void DrawIntProperty(const Property& prop, void* instance);
    void DrawFloatProperty(const Property& prop, void* instance);
    void DrawDoubleProperty(const Property& prop, void* instance);
    void DrawVec2Property(const Property& prop, void* instance);
    void DrawVec3Property(const Property& prop, void* instance);
    void DrawVec4Property(const Property& prop, void* instance);
    void DrawColorProperty(const Property& prop, void* instance);
    void DrawStringProperty(const Property& prop, void* instance);
    void DrawEnumProperty(const Property& prop, void* instance);
    void DrawAssetRefProperty(const Property& prop, void* instance);

    // Asset inspection
    void DrawAssetInspector();

    // Utilities
    void DrawPropertyLabel(const Property& prop);
    bool BeginPropertyRow(const Property& prop);
    void EndPropertyRow();

    // State
    bool m_showDebugInfo = false;
    bool m_lockSelection = false;
    std::string m_entityNameBuffer;
};

} // namespace RVX::Editor
