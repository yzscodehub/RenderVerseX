/**
 * @file NativeMaterialEditor.cpp
 * @brief Native UI material editor panel implementation
 */

#include "Editor/Panels/NativeMaterialEditor.h"

#include "Editor/UI/EditorFormLayout.h"
#include "Editor/UI/EditorPanelButtonStyle.h"
#include "Editor/UI/EditorShellMetrics.h"
#include "Editor/UI/EditorTypography.h"
#include "UI/Widgets/Button.h"
#include "UI/Widgets/Label.h"
#include "UI/Widgets/Panel.h"

#include <algorithm>
#include <string>
#include <utility>

namespace RVX::Editor
{
namespace
{
    const char* AssetTypeLabel(Tools::AssetType type)
    {
        switch (type)
        {
            case Tools::AssetType::Texture:
                return "Texture";
            case Tools::AssetType::Mesh:
                return "Mesh";
            case Tools::AssetType::Material:
                return "Material";
            case Tools::AssetType::Shader:
                return "Shader";
            case Tools::AssetType::Animation:
                return "Animation";
            case Tools::AssetType::Audio:
                return "Audio";
            case Tools::AssetType::Font:
                return "Font";
            case Tools::AssetType::Prefab:
                return "Prefab";
            case Tools::AssetType::Scene:
                return "Scene";
            case Tools::AssetType::Script:
                return "Script";
            case Tools::AssetType::Unknown:
                break;
        }
        return "Unknown";
    }

    UI::Label::Ptr CreateMaterialLabel(const std::string& name,
                                       const std::string& text,
                                       const UI::UITheme& theme,
                                       const UI::UIColor& color,
                                       float fontSize = 0.0f)
    {
        UI::Label::Ptr label = UI::Label::Create(text);
        label->SetName(name);
        label->SetFontSize(fontSize > 0.0f
                               ? fontSize
                               : EditorTypography::GetFontSize(
                                     theme,
                                     EditorTypographyRole::PropertyLabel));
        label->SetTextColor(color);
        label->SetVerticalAlign(UI::VerticalAlign::Middle);
        label->SetInteractive(false);
        return label;
    }

    UI::Button::Ptr CreateMaterialButton(const std::string& name,
                                         const std::string& text,
                                         const UI::UITheme& theme)
    {
        UI::Button::Ptr button = UI::Button::Create(text);
        button->SetName(name);
        EditorPanelButtonStyleDesc styleDesc;
        styleDesc.overflowMode = UI::TextOverflowMode::Clip;
        styleDesc.setTooltipFromText = false;
        EditorPanelButtonStyle::Apply(*button, theme, styleDesc);
        return button;
    }

    float AddFieldRow(EditorUIPanelFrameContext& context,
                      const std::string& widgetPrefix,
                      const std::string& labelText,
                      const std::string& valueText,
                      float y,
                      float width,
                      float rowHeight,
                      float padding,
                      const UI::UIColor& valueColor)
    {
        const UI::UITheme& theme = context.ui->GetTheme();
        const float labelWidth = std::min(104.0f, std::max(64.0f, width * 0.34f));
        const float valueWidth = std::max(0.0f, width - labelWidth - padding);

        UI::Label::Ptr label = CreateMaterialLabel(widgetPrefix + ".Label",
                                                   labelText,
                                                   theme,
                                                   theme.colors.textMuted);
        label->SetPosition(padding, y);
        label->SetSize(labelWidth, rowHeight);
        context.contentContainer->AddChild(label);

        UI::Label::Ptr value = CreateMaterialLabel(widgetPrefix + ".Value",
                                                   valueText,
                                                   theme,
                                                   valueColor);
        value->SetPosition(padding + labelWidth + padding, y);
        value->SetSize(valueWidth, rowHeight);
        value->SetWordWrap(true);
        context.contentContainer->AddChild(value);
        return y + rowHeight;
    }
}

NativeMaterialEditorPanel::NativeMaterialEditorPanel()
{
    m_desc.id = PanelId();
    m_desc.title = "Material";
    m_desc.defaultDockArea = EditorUIPanelDockArea::Right;
    m_desc.visibleByDefault = false;
    m_desc.closable = true;
}

void NativeMaterialEditorPanel::BuildUI(EditorUIPanelFrameContext& context)
{
    if (!context.ui || !context.contentContainer)
    {
        return;
    }

    m_lastBuildStats = {};
    m_lastBuildStats.built = true;

    EditorContext& editorContext = EditorContext::Get();
    m_lastBuildStats.selectionType = editorContext.GetSelectionType();

    EditorFormLayoutDesc formDesc;
    formDesc.context = &context;
    formDesc.namePrefix = "NativeMaterialEditor";
    formDesc.scrollOffsetY = m_scrollOffsetY;
    formDesc.onScrollChanged = [this](const Vec2& offset) {
        m_scrollOffsetY = offset.y;
    };

    EditorFormLayoutFrame form =
        EditorFormLayout::BeginScrollableFrame(std::move(formDesc));
    if (!form)
    {
        return;
    }

    m_lastBuildStats.hasScrollViewport = true;
    m_lastBuildStats.scrollViewportHeight = form.metrics.viewportHeight;
    m_lastBuildStats.scrollOffsetY = m_scrollOffsetY;

    EditorUIPanelFrameContext contentContext = form.contentContext;

    float contentHeight = 0.0f;
    if (editorContext.GetSelectionType() == SelectionType::Asset)
    {
        const Tools::AssetGUID guid = editorContext.GetSelectedAsset();
        m_lastBuildStats.hasAssetSelection = guid.IsValid();
        m_lastBuildStats.selectedGuid = guid;

        const Tools::AssetEntry* asset = editorContext.GetAssetDatabase().GetAsset(guid);
        if (!asset)
        {
            contentHeight = AddMissingAssetState(contentContext);
        }
        else if (asset->type == Tools::AssetType::Material)
        {
            contentHeight = AddMaterialAssetSummary(contentContext, *asset);
        }
        else
        {
            contentHeight = AddNonMaterialAssetState(contentContext, *asset);
        }
    }
    else
    {
        contentHeight = AddEmptyState(contentContext,
                                      "Select a material asset.",
                                      "NativeMaterialEditor.Empty");
    }

    m_lastBuildStats.scrollContentHeight =
        std::max(form.metrics.viewportHeight, contentHeight);
    EditorFormLayout::EndScrollableFrame(form, contentHeight);
}

float NativeMaterialEditorPanel::AddEmptyState(EditorUIPanelFrameContext& context,
                                               const std::string& text,
                                               const std::string& widgetName)
{
    const UI::UITheme& theme = context.ui->GetTheme();
    const EditorShellMetrics shellMetrics = EditorShellMetricPolicy::Resolve(theme);
    const float padding = shellMetrics.contentPadding;
    const float rowHeight = shellMetrics.formLineHeight;
    const float width = std::max(0.0f, context.contentContainer->GetWidth() - padding * 2.0f);

    UI::Label::Ptr label = CreateMaterialLabel(widgetName,
                                               text,
                                               theme,
                                               theme.colors.textMuted);
    label->SetPosition(padding, padding);
    label->SetSize(width, rowHeight);
    context.contentContainer->AddChild(label);
    return padding + rowHeight + padding;
}

float NativeMaterialEditorPanel::AddMissingAssetState(EditorUIPanelFrameContext& context)
{
    m_lastBuildStats.selectedAssetMissing = true;
    return AddEmptyState(context,
                         "Selected material asset is no longer indexed.",
                         "NativeMaterialEditor.MissingAsset");
}

float NativeMaterialEditorPanel::AddNonMaterialAssetState(EditorUIPanelFrameContext& context,
                                                          const Tools::AssetEntry& asset)
{
    const UI::UITheme& theme = context.ui->GetTheme();
    const EditorShellMetrics shellMetrics = EditorShellMetricPolicy::Resolve(theme);
    const float padding = shellMetrics.contentPadding;
    const float rowHeight = shellMetrics.formRowHeight;
    const float width = std::max(0.0f, context.contentContainer->GetWidth() - padding * 2.0f);
    float y = padding;

    m_lastBuildStats.hasNonMaterialAsset = true;

    UI::Label::Ptr message =
        CreateMaterialLabel("NativeMaterialEditor.NonMaterial.Message",
                            "Selected asset is not a material.",
                            theme,
                            theme.colors.warning);
    message->SetPosition(padding, y);
    message->SetSize(width, rowHeight);
    context.contentContainer->AddChild(message);
    y += rowHeight + padding;

    y = AddFieldRow(context,
                    "NativeMaterialEditor.NonMaterial.Type",
                    "Type",
                    AssetTypeLabel(asset.type),
                    y,
                    width,
                    rowHeight,
                    padding,
                    theme.colors.text);
    ++m_lastBuildStats.detailRowCount;

    y = AddFieldRow(context,
                    "NativeMaterialEditor.NonMaterial.Path",
                    "Path",
                    asset.path,
                    y,
                    width,
                    rowHeight,
                    padding,
                    theme.colors.text);
    ++m_lastBuildStats.detailRowCount;

    AddActionButtons(context, y + padding, width, rowHeight, padding);
    return y + padding + rowHeight + padding;
}

float NativeMaterialEditorPanel::AddMaterialAssetSummary(EditorUIPanelFrameContext& context,
                                                         const Tools::AssetEntry& asset)
{
    const UI::UITheme& theme = context.ui->GetTheme();
    const EditorShellMetrics shellMetrics = EditorShellMetricPolicy::Resolve(theme);
    const float padding = shellMetrics.contentPadding;
    const float rowHeight = shellMetrics.formRowHeight;
    const float sectionHeight = shellMetrics.formLineHeight;
    const float width = std::max(0.0f, context.contentContainer->GetWidth() - padding * 2.0f);
    float y = padding;

    m_lastBuildStats.hasMaterialAsset = true;

    UI::Label::Ptr title =
        CreateMaterialLabel("NativeMaterialEditor.Header",
                            asset.name.empty() ? asset.path : asset.name,
                            theme,
                            theme.colors.text,
                            EditorTypography::GetFontSize(
                                theme,
                                EditorTypographyRole::Body));
    title->SetPosition(padding, y);
    title->SetSize(width, rowHeight);
    context.contentContainer->AddChild(title);
    y += rowHeight + padding;

    y = AddFieldRow(context,
                    "NativeMaterialEditor.Asset.Type",
                    "Type",
                    AssetTypeLabel(asset.type),
                    y,
                    width,
                    rowHeight,
                    padding,
                    theme.colors.text);
    ++m_lastBuildStats.detailRowCount;

    y = AddFieldRow(context,
                    "NativeMaterialEditor.Asset.Path",
                    "Path",
                    asset.path,
                    y,
                    width,
                    rowHeight,
                    padding,
                    theme.colors.text);
    ++m_lastBuildStats.detailRowCount;

    y = AddFieldRow(context,
                    "NativeMaterialEditor.Asset.Guid",
                    "GUID",
                    asset.guid.ToString(),
                    y,
                    width,
                    rowHeight,
                    padding,
                    theme.colors.textMuted);
    ++m_lastBuildStats.detailRowCount;

    y = AddFieldRow(context,
                    "NativeMaterialEditor.Asset.Status",
                    "Status",
                    asset.isDirty ? "Dirty" : "Ready",
                    y,
                    width,
                    rowHeight,
                    padding,
                    asset.isDirty ? theme.colors.warning : theme.colors.textMuted);
    ++m_lastBuildStats.detailRowCount;
    y += padding;

    UI::Label::Ptr previewHeader =
        CreateMaterialLabel("NativeMaterialEditor.Preview.Header",
                            "Preview Parameters",
                            theme,
                            theme.colors.text);
    previewHeader->SetPosition(padding, y);
    previewHeader->SetSize(width, sectionHeight);
    context.contentContainer->AddChild(previewHeader);
    y += sectionHeight;

    UI::Panel::Ptr baseColor = UI::Panel::Create();
    baseColor->SetName("NativeMaterialEditor.Preview.BaseColorSwatch");
    baseColor->SetPosition(padding, y + 4.0f);
    baseColor->SetSize(34.0f, std::max(12.0f, rowHeight - 8.0f));
    baseColor->SetBackgroundColor(UI::UIColor(0.76f, 0.70f, 0.60f, 1.0f));
    baseColor->SetBorderColor(theme.colors.border);
    baseColor->SetBorderWidth(1.0f);
    baseColor->SetBorderRadius(theme.metrics.cornerRadius);
    context.contentContainer->AddChild(baseColor);

    UI::Label::Ptr baseColorLabel =
        CreateMaterialLabel("NativeMaterialEditor.Preview.BaseColor",
                            "Base Color  0.76, 0.70, 0.60",
                            theme,
                            theme.colors.text);
    baseColorLabel->SetPosition(padding + 42.0f, y);
    baseColorLabel->SetSize(std::max(0.0f, width - 42.0f), rowHeight);
    context.contentContainer->AddChild(baseColorLabel);
    ++m_lastBuildStats.detailRowCount;
    y += rowHeight;

    y = AddFieldRow(context,
                    "NativeMaterialEditor.Preview.Workflow",
                    "Workflow",
                    "Metallic/Roughness",
                    y,
                    width,
                    rowHeight,
                    padding,
                    theme.colors.text);
    ++m_lastBuildStats.detailRowCount;

    y = AddFieldRow(context,
                    "NativeMaterialEditor.Preview.Metallic",
                    "Metallic",
                    "0.00",
                    y,
                    width,
                    rowHeight,
                    padding,
                    theme.colors.text);
    ++m_lastBuildStats.detailRowCount;

    y = AddFieldRow(context,
                    "NativeMaterialEditor.Preview.Roughness",
                    "Roughness",
                    "0.50",
                    y,
                    width,
                    rowHeight,
                    padding,
                    theme.colors.text);
    ++m_lastBuildStats.detailRowCount;

    AddActionButtons(context, y + padding, width, rowHeight, padding);
    return y + padding + rowHeight + padding;
}

void NativeMaterialEditorPanel::AddActionButtons(EditorUIPanelFrameContext& context,
                                                 float y,
                                                 float width,
                                                 float rowHeight,
                                                 float padding)
{
    const UI::UITheme& theme = context.ui->GetTheme();
    const float clearWidth = std::min(132.0f, std::max(70.0f, width * 0.45f));
    const float refreshWidth = std::min(132.0f,
                                        std::max(70.0f, width - clearWidth - padding));

    UI::Button::Ptr clear = CreateMaterialButton("NativeMaterialEditor.Action.ClearSelection",
                                                 "Clear Selection",
                                                 theme);
    clear->SetPosition(padding, y);
    clear->SetSize(clearWidth, rowHeight);
    clear->SetOnClick([](const UI::UIEvent& event) {
        (void)event;
        EditorContext::Get().ClearSelection();
    });
    context.contentContainer->AddChild(clear);

    UI::Button::Ptr refresh = CreateMaterialButton("NativeMaterialEditor.Action.RefreshDatabase",
                                                   "Refresh",
                                                   theme);
    refresh->SetPosition(padding + clearWidth + padding, y);
    refresh->SetSize(refreshWidth, rowHeight);
    refresh->SetOnClick([](const UI::UIEvent& event) {
        (void)event;
        EditorContext::Get().GetAssetDatabase().Refresh();
    });
    context.contentContainer->AddChild(refresh);

    m_lastBuildStats.actionButtonCount += 2u;
}

} // namespace RVX::Editor
