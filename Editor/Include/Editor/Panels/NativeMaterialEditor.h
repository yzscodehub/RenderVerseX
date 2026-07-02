/**
 * @file NativeMaterialEditor.h
 * @brief Native UI material editor panel
 */

#pragma once

#include "Editor/EditorContext.h"
#include "Editor/UI/EditorUIPanel.h"
#include "Tools/AssetDatabase.h"

#include <string>

namespace RVX::Editor
{

struct NativeMaterialEditorStats
{
    SelectionType selectionType = SelectionType::None;
    Tools::AssetGUID selectedGuid;
    bool built = false;
    bool hasAssetSelection = false;
    bool hasMaterialAsset = false;
    bool hasNonMaterialAsset = false;
    bool selectedAssetMissing = false;
    bool hasScrollViewport = false;
    float scrollViewportHeight = 0.0f;
    float scrollContentHeight = 0.0f;
    float scrollOffsetY = 0.0f;
    uint32 detailRowCount = 0;
    uint32 actionButtonCount = 0;
};

class NativeMaterialEditorPanel final : public IEditorUIPanel
{
public:
    NativeMaterialEditorPanel();

    static constexpr const char* PanelId() { return "native.materialEditor"; }

    const EditorUIPanelDesc& GetPanelDesc() const override { return m_desc; }
    void BuildUI(EditorUIPanelFrameContext& context) override;

    const NativeMaterialEditorStats& GetLastBuildStats() const { return m_lastBuildStats; }

private:
    float AddEmptyState(EditorUIPanelFrameContext& context,
                        const std::string& text,
                        const std::string& widgetName);
    float AddMissingAssetState(EditorUIPanelFrameContext& context);
    float AddNonMaterialAssetState(EditorUIPanelFrameContext& context,
                                   const Tools::AssetEntry& asset);
    float AddMaterialAssetSummary(EditorUIPanelFrameContext& context,
                                  const Tools::AssetEntry& asset);
    void AddActionButtons(EditorUIPanelFrameContext& context,
                          float y,
                          float width,
                          float rowHeight,
                          float padding);

    EditorUIPanelDesc m_desc;
    NativeMaterialEditorStats m_lastBuildStats;
    float m_scrollOffsetY = 0.0f;
};

} // namespace RVX::Editor
