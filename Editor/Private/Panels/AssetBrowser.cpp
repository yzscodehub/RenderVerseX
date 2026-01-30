/**
 * @file AssetBrowser.cpp
 * @brief Asset browser panel implementation
 */

#include "Editor/Panels/AssetBrowser.h"
#include "Editor/EditorContext.h"
#include "Core/Log.h"

#include <imgui.h>
#include <algorithm>

namespace RVX::Editor
{

AssetBrowserPanel::AssetBrowserPanel()
{
    m_searchFilter.resize(256);
}

void AssetBrowserPanel::OnInit()
{
    auto& context = EditorContext::Get();
    if (context.HasProject())
    {
        m_rootPath = std::filesystem::path(context.GetProjectPath()) / "Assets";
        m_currentPath = m_rootPath;
    }
    else
    {
        m_rootPath = std::filesystem::current_path();
        m_currentPath = m_rootPath;
    }
    Refresh();
}

void AssetBrowserPanel::OnGUI()
{
    if (!ImGui::Begin(GetName()))
    {
        ImGui::End();
        return;
    }

    DrawToolbar();
    DrawBreadcrumbs();
    
    ImGui::Separator();

    // Split view: directory tree on left, content on right
    if (m_showDirectoryTree)
    {
        ImGui::Columns(2, "AssetBrowserColumns", true);
        ImGui::SetColumnWidth(0, 200);

        // Left panel: Directory tree
        ImGui::BeginChild("DirectoryTree", ImVec2(0, 0), true);
        DrawDirectoryTree();
        ImGui::EndChild();

        ImGui::NextColumn();
    }

    // Right panel: Asset grid/list
    ImGui::BeginChild("AssetContent", ImVec2(0, 0), true);
    if (m_useListView)
    {
        DrawAssetList();
    }
    else
    {
        DrawAssetGrid();
    }
    ImGui::EndChild();

    if (m_showDirectoryTree)
    {
        ImGui::Columns(1);
    }

    ImGui::End();
}

void AssetBrowserPanel::DrawToolbar()
{
    // Navigation buttons
    bool canGoBack = m_historyIndex > 0;
    bool canGoForward = m_historyIndex < static_cast<int>(m_directoryHistory.size()) - 1;
    bool canGoUp = m_currentPath != m_rootPath && m_currentPath.has_parent_path();

    if (ImGui::Button("<") && canGoBack)
    {
        m_historyIndex--;
        m_currentPath = m_directoryHistory[m_historyIndex];
        m_needsRefresh = true;
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Back");

    ImGui::SameLine();
    if (ImGui::Button(">") && canGoForward)
    {
        m_historyIndex++;
        m_currentPath = m_directoryHistory[m_historyIndex];
        m_needsRefresh = true;
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Forward");

    ImGui::SameLine();
    if (ImGui::Button("^") && canGoUp)
    {
        NavigateUp();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Up");

    ImGui::SameLine();
    if (ImGui::Button("R"))
    {
        Refresh();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Refresh");

    ImGui::SameLine();
    ImGui::Separator();
    ImGui::SameLine();

    // Search box
    ImGui::SetNextItemWidth(200);
    ImGui::InputText("##Search", m_searchFilter.data(), m_searchFilter.size());
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Search assets");

    ImGui::SameLine();
    ImGui::Separator();
    ImGui::SameLine();

    // View options
    if (ImGui::Button(m_useListView ? "Grid" : "List"))
    {
        m_useListView = !m_useListView;
    }

    ImGui::SameLine();
    ImGui::SetNextItemWidth(100);
    ImGui::SliderFloat("##Size", &m_thumbnailSize, 48.0f, 256.0f, "%.0f");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Thumbnail size");

    ImGui::SameLine();
    ImGui::Checkbox("Tree", &m_showDirectoryTree);

    ImGui::SameLine();

    // Create menu
    if (ImGui::BeginCombo("##Create", "+ Create", ImGuiComboFlags_NoPreview))
    {
        if (ImGui::MenuItem("Folder"))
        {
            // Create new folder
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Material"))
        {
            // Create new material
        }
        if (ImGui::MenuItem("Shader"))
        {
            // Create new shader
        }
        if (ImGui::MenuItem("Script"))
        {
            // Create new script
        }
        ImGui::EndCombo();
    }
}

void AssetBrowserPanel::DrawBreadcrumbs()
{
    auto relativePath = std::filesystem::relative(m_currentPath, m_rootPath);
    std::vector<std::filesystem::path> pathParts;

    // Build path parts
    pathParts.push_back(m_rootPath);
    auto tempPath = m_rootPath;
    for (const auto& part : relativePath)
    {
        if (part != ".")
        {
            tempPath /= part;
            pathParts.push_back(tempPath);
        }
    }

    // Draw breadcrumbs
    for (size_t i = 0; i < pathParts.size(); ++i)
    {
        if (i > 0)
        {
            ImGui::SameLine();
            ImGui::TextDisabled(">");
            ImGui::SameLine();
        }

        std::string label = (i == 0) ? "Assets" : pathParts[i].filename().string();
        if (ImGui::SmallButton(label.c_str()))
        {
            NavigateTo(pathParts[i]);
        }
    }
}

void AssetBrowserPanel::DrawDirectoryTree()
{
    if (std::filesystem::exists(m_rootPath))
    {
        DrawDirectoryNode(m_rootPath, true);
    }
}

void AssetBrowserPanel::DrawDirectoryNode(const std::filesystem::path& path, bool isRoot)
{
    std::string name = isRoot ? "Assets" : path.filename().string();

    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
    if (m_currentPath == path)
    {
        flags |= ImGuiTreeNodeFlags_Selected;
    }

    // Check if has subdirectories
    bool hasSubdirs = false;
    if (std::filesystem::exists(path) && std::filesystem::is_directory(path))
    {
        for (const auto& entry : std::filesystem::directory_iterator(path))
        {
            if (entry.is_directory() && !entry.path().filename().string().starts_with("."))
            {
                hasSubdirs = true;
                break;
            }
        }
    }

    if (!hasSubdirs)
    {
        flags |= ImGuiTreeNodeFlags_Leaf;
    }

    bool open = ImGui::TreeNodeEx(name.c_str(), flags);

    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen())
    {
        NavigateTo(path);
    }

    // Context menu
    if (ImGui::BeginPopupContextItem())
    {
        if (ImGui::MenuItem("Show in Explorer"))
        {
            // Open in file explorer
        }
        if (ImGui::MenuItem("New Folder"))
        {
            // Create folder
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Delete"))
        {
            // Delete folder
        }
        ImGui::EndPopup();
    }

    if (open)
    {
        if (std::filesystem::exists(path) && std::filesystem::is_directory(path))
        {
            std::vector<std::filesystem::path> subdirs;
            for (const auto& entry : std::filesystem::directory_iterator(path))
            {
                if (entry.is_directory() && !entry.path().filename().string().starts_with("."))
                {
                    subdirs.push_back(entry.path());
                }
            }

            std::sort(subdirs.begin(), subdirs.end());
            for (const auto& subdir : subdirs)
            {
                DrawDirectoryNode(subdir);
            }
        }
        ImGui::TreePop();
    }
}

void AssetBrowserPanel::DrawAssetGrid()
{
    if (m_needsRefresh)
    {
        Refresh();
    }

    float padding = 8.0f;
    float cellSize = m_thumbnailSize + padding;
    float panelWidth = ImGui::GetContentRegionAvail().x;
    int columnCount = std::max(1, static_cast<int>(panelWidth / cellSize));

    ImGui::Columns(columnCount, nullptr, false);

    std::string searchLower = m_searchFilter.data();
    std::transform(searchLower.begin(), searchLower.end(), searchLower.begin(), ::tolower);

    for (const auto& entry : m_cachedEntries)
    {
        std::string filename = entry.filename().string();
        
        // Apply search filter
        if (!searchLower.empty())
        {
            std::string filenameLower = filename;
            std::transform(filenameLower.begin(), filenameLower.end(), filenameLower.begin(), ::tolower);
            if (filenameLower.find(searchLower) == std::string::npos)
            {
                continue;
            }
        }

        bool isDirectory = std::filesystem::is_directory(entry);

        ImGui::PushID(filename.c_str());
        
        ImGui::BeginGroup();

        // Draw thumbnail/icon
        DrawAssetIcon(entry, isDirectory);

        // Draw filename (truncated)
        std::string displayName = filename;
        if (displayName.length() > 12)
        {
            displayName = displayName.substr(0, 10) + "...";
        }
        
        float textWidth = ImGui::CalcTextSize(displayName.c_str()).x;
        float offset = (m_thumbnailSize - textWidth) * 0.5f;
        if (offset > 0)
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + offset);
        
        ImGui::TextWrapped("%s", displayName.c_str());

        ImGui::EndGroup();

        // Handle interaction
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("%s", filename.c_str());

            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
            {
                if (isDirectory)
                {
                    NavigateTo(entry);
                }
                else
                {
                    // Open asset
                    EditorContext::Get().SelectAsset(Tools::AssetGUID{});  // TODO: Get real GUID
                }
            }
            else if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
            {
                m_selectedPath = entry;
                if (!isDirectory)
                {
                    EditorContext::Get().SelectAsset(Tools::AssetGUID{});  // TODO: Get real GUID
                }
            }
        }

        // Context menu
        DrawAssetContextMenu(entry);

        // Drag source
        HandleDragDrop(entry);

        ImGui::PopID();
        ImGui::NextColumn();
    }

    ImGui::Columns(1);
}

void AssetBrowserPanel::DrawAssetList()
{
    if (m_needsRefresh)
    {
        Refresh();
    }

    ImGui::BeginTable("AssetList", 4, ImGuiTableFlags_Resizable | ImGuiTableFlags_Sortable |
                                       ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY);

    ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_DefaultSort);
    ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 80);
    ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, 80);
    ImGui::TableSetupColumn("Modified", ImGuiTableColumnFlags_WidthFixed, 150);
    ImGui::TableHeadersRow();

    for (const auto& entry : m_cachedEntries)
    {
        std::string filename = entry.filename().string();
        bool isDirectory = std::filesystem::is_directory(entry);

        ImGui::TableNextRow();
        ImGui::TableNextColumn();

        bool selected = (m_selectedPath == entry);
        if (ImGui::Selectable(filename.c_str(), selected, ImGuiSelectableFlags_SpanAllColumns))
        {
            m_selectedPath = entry;
        }

        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
        {
            if (isDirectory)
            {
                NavigateTo(entry);
            }
        }

        ImGui::TableNextColumn();
        ImGui::Text("%s", isDirectory ? "Folder" : entry.extension().string().c_str());

        ImGui::TableNextColumn();
        if (!isDirectory)
        {
            auto size = std::filesystem::file_size(entry);
            if (size < 1024)
                ImGui::Text("%zu B", size);
            else if (size < 1024 * 1024)
                ImGui::Text("%.1f KB", size / 1024.0f);
            else
                ImGui::Text("%.1f MB", size / (1024.0f * 1024.0f));
        }

        ImGui::TableNextColumn();
        // TODO: Show last modified time
        ImGui::TextDisabled("-");
    }

    ImGui::EndTable();
}

void AssetBrowserPanel::DrawAssetIcon(const std::filesystem::path& path, bool isDirectory)
{
    ImVec2 iconSize(m_thumbnailSize, m_thumbnailSize);
    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImDrawList* drawList = ImGui::GetWindowDrawList();

    bool selected = (m_selectedPath == path);
    ImU32 bgColor = selected ? IM_COL32(60, 100, 150, 255) : IM_COL32(50, 50, 55, 255);

    drawList->AddRectFilled(pos, ImVec2(pos.x + iconSize.x, pos.y + iconSize.y), bgColor, 4.0f);

    // Draw icon based on type
    ImVec2 center(pos.x + iconSize.x * 0.5f, pos.y + iconSize.y * 0.5f);
    ImU32 iconColor = isDirectory ? IM_COL32(200, 180, 100, 255) : IM_COL32(150, 150, 150, 255);

    if (isDirectory)
    {
        // Folder icon
        float w = iconSize.x * 0.4f;
        float h = iconSize.y * 0.3f;
        ImVec2 tl(center.x - w, center.y - h);
        ImVec2 br(center.x + w, center.y + h);
        drawList->AddRectFilled(tl, br, iconColor, 2.0f);
        drawList->AddRectFilled(ImVec2(tl.x, tl.y - h * 0.3f), 
                                 ImVec2(center.x - w * 0.2f, tl.y), iconColor, 2.0f);
    }
    else
    {
        // File icon
        float w = iconSize.x * 0.3f;
        float h = iconSize.y * 0.35f;
        ImVec2 tl(center.x - w, center.y - h);
        ImVec2 br(center.x + w, center.y + h);
        drawList->AddRectFilled(tl, br, iconColor, 2.0f);

        // Extension text
        std::string ext = path.extension().string();
        if (ext.length() > 1)
        {
            ext = ext.substr(1, 4);
            std::transform(ext.begin(), ext.end(), ext.begin(), ::toupper);
            ImVec2 textPos(center.x - ImGui::CalcTextSize(ext.c_str()).x * 0.5f, center.y);
            drawList->AddText(textPos, IM_COL32(255, 255, 255, 200), ext.c_str());
        }
    }

    ImGui::InvisibleButton("##Icon", iconSize);
}

void AssetBrowserPanel::DrawAssetContextMenu(const std::filesystem::path& path)
{
    if (ImGui::BeginPopupContextItem())
    {
        bool isDirectory = std::filesystem::is_directory(path);

        if (ImGui::MenuItem("Open"))
        {
            if (isDirectory)
                NavigateTo(path);
        }
        if (ImGui::MenuItem("Show in Explorer"))
        {
            // Platform-specific file explorer
        }

        ImGui::Separator();

        if (ImGui::MenuItem("Copy"))
        {
            // Copy to clipboard
        }
        if (ImGui::MenuItem("Paste", nullptr, false, false))
        {
            // Paste from clipboard
        }
        if (ImGui::MenuItem("Duplicate"))
        {
            // Duplicate asset
        }

        ImGui::Separator();

        if (ImGui::MenuItem("Rename"))
        {
            // Enter rename mode
        }
        if (ImGui::MenuItem("Delete"))
        {
            // Delete with confirmation
        }

        ImGui::Separator();

        if (ImGui::MenuItem("Reimport"))
        {
            // Reimport asset
        }

        ImGui::EndPopup();
    }
}

void AssetBrowserPanel::HandleDragDrop(const std::filesystem::path& path)
{
    (void)path;
    
    if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None))
    {
        std::string pathStr = path.string();
        ImGui::SetDragDropPayload("ASSET_PATH", pathStr.c_str(), pathStr.size() + 1);
        ImGui::Text("%s", path.filename().string().c_str());
        ImGui::EndDragDropSource();
    }
}

void AssetBrowserPanel::NavigateTo(const std::filesystem::path& path)
{
    if (std::filesystem::exists(path) && std::filesystem::is_directory(path))
    {
        // Truncate forward history if navigating while in the middle
        if (m_historyIndex < static_cast<int>(m_directoryHistory.size()) - 1)
        {
            m_directoryHistory.resize(m_historyIndex + 1);
        }

        m_currentPath = path;
        m_directoryHistory.push_back(path);
        m_historyIndex = static_cast<int>(m_directoryHistory.size()) - 1;
        m_needsRefresh = true;
    }
}

void AssetBrowserPanel::NavigateUp()
{
    if (m_currentPath != m_rootPath && m_currentPath.has_parent_path())
    {
        NavigateTo(m_currentPath.parent_path());
    }
}

void AssetBrowserPanel::Refresh()
{
    m_cachedEntries.clear();

    if (std::filesystem::exists(m_currentPath) && std::filesystem::is_directory(m_currentPath))
    {
        for (const auto& entry : std::filesystem::directory_iterator(m_currentPath))
        {
            std::string filename = entry.path().filename().string();
            
            // Skip hidden files
            if (!m_showHiddenFiles && filename.starts_with("."))
                continue;

            m_cachedEntries.push_back(entry.path());
        }

        // Sort: directories first, then alphabetically
        std::sort(m_cachedEntries.begin(), m_cachedEntries.end(),
            [](const std::filesystem::path& a, const std::filesystem::path& b)
            {
                bool aDir = std::filesystem::is_directory(a);
                bool bDir = std::filesystem::is_directory(b);
                if (aDir != bDir)
                    return aDir;
                return a.filename() < b.filename();
            });
    }

    m_needsRefresh = false;
}

bool AssetBrowserPanel::IsAssetFile(const std::filesystem::path& path) const
{
    static const std::vector<std::string> assetExtensions = {
        ".gltf", ".glb", ".fbx", ".obj", ".dae",    // Models
        ".png", ".jpg", ".jpeg", ".tga", ".hdr", ".exr",  // Textures
        ".wav", ".mp3", ".ogg",                      // Audio
        ".hlsl", ".glsl", ".frag", ".vert",          // Shaders
        ".mat", ".material",                          // Materials
        ".scene", ".prefab",                          // Scene/Prefab
        ".particle",                                  // Particle
        ".anim", ".animation"                         // Animation
    };

    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    
    return std::find(assetExtensions.begin(), assetExtensions.end(), ext) != assetExtensions.end();
}

void AssetBrowserPanel::DrawImportDialog()
{
    // TODO: Implement import dialog
}

const char* AssetBrowserPanel::GetAssetTypeIcon(const std::filesystem::path& path) const
{
    (void)path;
    return "file";
}

} // namespace RVX::Editor
