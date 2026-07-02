/**
 * @file EditorPropertyDrawerRegistry.h
 * @brief Native editor reflected-property drawer registry
 */

#pragma once

#include "Core/Serialization/PropertyReflection.h"
#include "Editor/UI/EditorUIPanel.h"

#include <functional>
#include <vector>

namespace RVX::UI
{
    class Panel;
}

namespace RVX::Editor
{

class EditorUIHost;

struct EditorPropertyDrawerContext
{
    EditorUIHost* host = nullptr;
    UI::UIContext* ui = nullptr;
    UI::UICanvas* canvas = nullptr;
    UI::Panel* parent = nullptr;
    const Property* property = nullptr;
    void* instance = nullptr;
    std::string widgetPrefix;
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float rowHeight = 0.0f;
    float labelWidth = 0.0f;
    float consumedHeight = 0.0f;
};

using EditorPropertyDrawerCallback = std::function<bool(EditorPropertyDrawerContext&)>;

struct EditorPropertyDrawerDesc
{
    PropertyType propertyType = PropertyType::Unknown;
    std::string debugName;
    EditorPropertyDrawerCallback callback;
};

class EditorPropertyDrawerRegistry
{
public:
    bool RegisterDrawer(EditorPropertyDrawerDesc desc);
    bool UnregisterDrawer(PropertyType type);

    bool DrawProperty(EditorPropertyDrawerContext& context) const;
    bool HasDrawer(PropertyType type) const;
    size_t GetDrawerCount() const { return m_drawers.size(); }

private:
    const EditorPropertyDrawerDesc* FindDrawer(PropertyType type) const;
    EditorPropertyDrawerDesc* FindDrawer(PropertyType type);

    std::vector<EditorPropertyDrawerDesc> m_drawers;
};

bool RegisterDefaultEditorPropertyDrawers(EditorPropertyDrawerRegistry& registry);

} // namespace RVX::Editor
