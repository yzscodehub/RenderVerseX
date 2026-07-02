/**
 * @file EditorLayoutSerializer.cpp
 * @brief Persistent native editor dock layout serialization implementation
 */

#include "Editor/UI/EditorLayoutSerializer.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <system_error>
#include <utility>

namespace RVX::Editor
{
namespace
{
    constexpr const char* RVX_EDITOR_LAYOUT_MAGIC = "RenderVerseXEditorLayout";
    constexpr uint32 RVX_EDITOR_LAYOUT_VERSION = 6;

    void SetError(std::string* error, const std::string& message)
    {
        if (error)
        {
            *error = message;
        }
    }

    const char* ToString(EditorUIPanelDockArea area)
    {
        switch (area)
        {
            case EditorUIPanelDockArea::Center:
                return "Center";
            case EditorUIPanelDockArea::Left:
                return "Left";
            case EditorUIPanelDockArea::Right:
                return "Right";
            case EditorUIPanelDockArea::Bottom:
                return "Bottom";
            case EditorUIPanelDockArea::Floating:
                return "Floating";
        }

        return "Floating";
    }

    bool TryParseDockArea(const std::string& token, EditorUIPanelDockArea& area)
    {
        if (token == "Center")
        {
            area = EditorUIPanelDockArea::Center;
            return true;
        }
        if (token == "Left")
        {
            area = EditorUIPanelDockArea::Left;
            return true;
        }
        if (token == "Right")
        {
            area = EditorUIPanelDockArea::Right;
            return true;
        }
        if (token == "Bottom")
        {
            area = EditorUIPanelDockArea::Bottom;
            return true;
        }
        if (token == "Floating")
        {
            area = EditorUIPanelDockArea::Floating;
            return true;
        }

        return false;
    }

    const char* ToCollapsedToken(bool collapsed)
    {
        return collapsed ? "1" : "0";
    }

    bool TryParseCollapsedToken(const std::string& token, bool& collapsed)
    {
        if (token == "1" || token == "collapsed")
        {
            collapsed = true;
            return true;
        }
        if (token == "0" || token == "expanded")
        {
            collapsed = false;
            return true;
        }

        return false;
    }

    char ToHex(uint8 value)
    {
        return static_cast<char>(value < 10 ? ('0' + value) : ('A' + (value - 10)));
    }

    int32 FromHex(char value)
    {
        if (value >= '0' && value <= '9')
        {
            return value - '0';
        }
        if (value >= 'a' && value <= 'f')
        {
            return value - 'a' + 10;
        }
        if (value >= 'A' && value <= 'F')
        {
            return value - 'A' + 10;
        }

        return -1;
    }

    std::string EscapeToken(const std::string& value)
    {
        std::string result;
        result.reserve(value.size());
        for (const unsigned char character : value)
        {
            if (std::isalnum(character) || character == '_' || character == '-' || character == '.')
            {
                result.push_back(static_cast<char>(character));
                continue;
            }

            result.push_back('%');
            result.push_back(ToHex(static_cast<uint8>((character >> 4u) & 0x0Fu)));
            result.push_back(ToHex(static_cast<uint8>(character & 0x0Fu)));
        }

        return result;
    }

    bool UnescapeToken(const std::string& token, std::string& value)
    {
        value.clear();
        value.reserve(token.size());
        for (size_t index = 0; index < token.size(); ++index)
        {
            const char character = token[index];
            if (character != '%')
            {
                value.push_back(character);
                continue;
            }

            if (index + 2 >= token.size())
            {
                return false;
            }

            const int32 high = FromHex(token[index + 1]);
            const int32 low = FromHex(token[index + 2]);
            if (high < 0 || low < 0)
            {
                return false;
            }

            value.push_back(static_cast<char>((high << 4) | low));
            index += 2;
        }

        return true;
    }

    bool IsBlankOrComment(const std::string& line)
    {
        for (const char character : line)
        {
            if (std::isspace(static_cast<unsigned char>(character)))
            {
                continue;
            }

            return character == '#';
        }

        return true;
    }

    bool TryParseUint32Token(const std::string& token, uint32& value)
    {
        if (token.empty() || token[0] == '-')
        {
            return false;
        }

        uint64 parsedValue = 0;
        std::istringstream tokenStream(token);
        std::string invalidToken;
        if (!(tokenStream >> parsedValue) || (tokenStream >> invalidToken) ||
            parsedValue > std::numeric_limits<uint32>::max())
        {
            return false;
        }

        value = static_cast<uint32>(parsedValue);
        return true;
    }
}

std::string EditorLayoutSerializer::Serialize(const EditorDockingModel& model)
{
    std::vector<EditorDockPanelPlacement> placements = model.GetPlacements();
    std::stable_sort(placements.begin(),
                     placements.end(),
                     [](const EditorDockPanelPlacement& lhs,
                        const EditorDockPanelPlacement& rhs) {
                         if (lhs.order != rhs.order)
                         {
                             return lhs.order < rhs.order;
                         }

                         return lhs.panelId < rhs.panelId;
                     });

    std::ostringstream stream;
    stream << RVX_EDITOR_LAYOUT_MAGIC << " " << RVX_EDITOR_LAYOUT_VERSION << "\n";
    stream << std::fixed << std::setprecision(6);
    const EditorDockAreaSizing& sizing = model.GetAreaSizing();
    stream << "areas "
           << sizing.leftWidthRatio << " "
           << sizing.rightWidthRatio << " "
           << sizing.bottomHeightRatio << "\n";
    stream << "drawer Bottom "
           << ToCollapsedToken(
                  model.IsDockAreaCollapsed(EditorUIPanelDockArea::Bottom))
           << "\n";
    for (const EditorDockPanelPlacement& placement : placements)
    {
        stream << "panel "
               << EscapeToken(placement.panelId) << " "
               << ToString(placement.area) << " "
               << placement.normalizedSize << " "
               << placement.order << " "
               << (placement.visible ? 1 : 0) << " "
               << (placement.hasFloatingBounds ? 1 : 0) << " "
               << placement.floatingBounds.x << " "
               << placement.floatingBounds.y << " "
               << placement.floatingBounds.width << " "
               << placement.floatingBounds.height << " "
               << placement.floatingZOrder;
        if (!placement.tabStackId.empty())
        {
            stream << " " << EscapeToken(placement.tabStackId);
        }
        stream << "\n";
    }
    for (const EditorDockTabStackState& state : model.GetTabStackStates())
    {
        if (state.stackId.empty() || state.activePanelId.empty() ||
            model.GetTabStackPanelCount(state.stackId) == 0)
        {
            continue;
        }

        stream << "tabstack "
               << EscapeToken(state.stackId) << " "
               << EscapeToken(state.activePanelId) << "\n";
    }

    return stream.str();
}

bool EditorLayoutSerializer::Deserialize(std::string_view text,
                                         EditorDockingModel& model,
                                         std::string* error)
{
    EditorDockingModel loadedModel;
    std::istringstream stream{std::string(text)};
    std::string line;
    bool foundHeader = false;
    bool foundAreas = false;
    uint32 layoutVersion = 0;
    uint32 lineNumber = 0;
    std::vector<std::pair<std::string, std::string>> pendingActiveTabs;

    while (std::getline(stream, line))
    {
        ++lineNumber;
        if (IsBlankOrComment(line))
        {
            continue;
        }

        std::istringstream lineStream(line);
        if (!foundHeader)
        {
            std::string magic;
            uint32 version = 0;
            if (!(lineStream >> magic >> version) || magic != RVX_EDITOR_LAYOUT_MAGIC)
            {
                SetError(error, "Invalid editor layout header");
                return false;
            }
            if (version == 0 || version > RVX_EDITOR_LAYOUT_VERSION)
            {
                SetError(error, "Unsupported editor layout version");
                return false;
            }

            layoutVersion = version;
            foundHeader = true;
            continue;
        }

        std::string recordType;
        lineStream >> recordType;
        if (recordType == "areas")
        {
            float leftWidthRatio = 0.0f;
            float rightWidthRatio = 0.0f;
            float bottomHeightRatio = 0.0f;
            if (foundAreas ||
                !(lineStream >> leftWidthRatio >> rightWidthRatio >> bottomHeightRatio))
            {
                SetError(error, "Invalid editor layout area record at line " +
                                    std::to_string(lineNumber));
                return false;
            }

            std::string extraToken;
            if (lineStream >> extraToken)
            {
                SetError(error, "Unexpected token in editor layout at line " +
                                    std::to_string(lineNumber));
                return false;
            }
            if (!std::isfinite(leftWidthRatio) || !std::isfinite(rightWidthRatio) ||
                !std::isfinite(bottomHeightRatio))
            {
                SetError(error, "Invalid editor layout area size at line " +
                                    std::to_string(lineNumber));
                return false;
            }

            EditorDockAreaSizing sizing;
            sizing.leftWidthRatio = leftWidthRatio;
            sizing.rightWidthRatio = rightWidthRatio;
            sizing.bottomHeightRatio = bottomHeightRatio;
            loadedModel.SetAreaSizing(sizing);
            foundAreas = true;
            continue;
        }

        if (recordType == "tabstack")
        {
            if (layoutVersion < 5)
            {
                SetError(error, "Unexpected tab stack record at line " +
                                    std::to_string(lineNumber));
                return false;
            }

            std::string stackToken;
            std::string activePanelToken;
            std::string extraToken;
            if (!(lineStream >> stackToken >> activePanelToken) ||
                (lineStream >> extraToken))
            {
                SetError(error, "Invalid editor layout tab stack record at line " +
                                    std::to_string(lineNumber));
                return false;
            }

            std::string stackId;
            std::string activePanelId;
            if (!UnescapeToken(stackToken, stackId) || stackId.empty() ||
                !UnescapeToken(activePanelToken, activePanelId) ||
                activePanelId.empty())
            {
                SetError(error, "Invalid editor layout tab stack id at line " +
                                    std::to_string(lineNumber));
                return false;
            }

            pendingActiveTabs.emplace_back(std::move(stackId),
                                           std::move(activePanelId));
            continue;
        }

        if (recordType == "drawer")
        {
            if (layoutVersion < 6)
            {
                SetError(error, "Unexpected drawer record at line " +
                                    std::to_string(lineNumber));
                return false;
            }

            std::string areaToken;
            std::string collapsedToken;
            std::string extraToken;
            if (!(lineStream >> areaToken >> collapsedToken) ||
                (lineStream >> extraToken))
            {
                SetError(error, "Invalid editor layout drawer record at line " +
                                    std::to_string(lineNumber));
                return false;
            }

            EditorUIPanelDockArea area = EditorUIPanelDockArea::Floating;
            bool collapsed = false;
            if (!TryParseDockArea(areaToken, area) ||
                !TryParseCollapsedToken(collapsedToken, collapsed) ||
                !loadedModel.SetDockAreaCollapsed(area, collapsed))
            {
                SetError(error, "Invalid editor layout drawer area at line " +
                                    std::to_string(lineNumber));
                return false;
            }
            continue;
        }

        if (recordType != "panel")
        {
            SetError(error, "Invalid editor layout record at line " + std::to_string(lineNumber));
            return false;
        }

        std::string panelToken;
        std::string areaToken;
        float normalizedSize = 0.0f;
        uint32 order = 0;
        int32 visibleValue = 0;
        if (!(lineStream >> panelToken >> areaToken >> normalizedSize >> order >> visibleValue))
        {
            SetError(error, "Invalid editor layout panel record at line " + std::to_string(lineNumber));
            return false;
        }

        EditorDockPanelPlacement placement;
        std::string floatingBoundsToken;
        if (lineStream >> floatingBoundsToken)
        {
            int32 hasFloatingBoundsValue = 0;
            std::istringstream floatingBoundsTokenStream(floatingBoundsToken);
            std::string invalidFlagToken;
            float floatingX = 0.0f;
            float floatingY = 0.0f;
            float floatingWidth = 0.0f;
            float floatingHeight = 0.0f;
            if (!(floatingBoundsTokenStream >> hasFloatingBoundsValue) ||
                (floatingBoundsTokenStream >> invalidFlagToken) ||
                (hasFloatingBoundsValue != 0 && hasFloatingBoundsValue != 1) ||
                !(lineStream >> floatingX >> floatingY >> floatingWidth >> floatingHeight))
            {
                SetError(error, "Invalid editor layout floating bounds at line " +
                                    std::to_string(lineNumber));
                return false;
            }

            if (!std::isfinite(floatingX) || !std::isfinite(floatingY) ||
                !std::isfinite(floatingWidth) || !std::isfinite(floatingHeight))
            {
                SetError(error, "Invalid editor layout floating bounds at line " +
                                    std::to_string(lineNumber));
                return false;
            }

            placement.hasFloatingBounds = hasFloatingBoundsValue != 0;
            placement.floatingBounds.x = floatingX;
            placement.floatingBounds.y = floatingY;
            placement.floatingBounds.width = floatingWidth;
            placement.floatingBounds.height = floatingHeight;

            std::string floatingZOrderToken;
            if (lineStream >> floatingZOrderToken)
            {
                uint32 floatingZOrder = 0;
                if (!TryParseUint32Token(floatingZOrderToken, floatingZOrder))
                {
                    SetError(error, "Invalid editor layout floating z-order at line " +
                                        std::to_string(lineNumber));
                    return false;
                }

                placement.floatingZOrder = floatingZOrder;
            }

            if (layoutVersion >= 5)
            {
                std::string tabStackToken;
                if (lineStream >> tabStackToken)
                {
                    if (!UnescapeToken(tabStackToken, placement.tabStackId))
                    {
                        SetError(error, "Invalid editor layout tab stack at line " +
                                            std::to_string(lineNumber));
                        return false;
                    }
                }
            }

            std::string extraToken;
            if (lineStream >> extraToken)
            {
                SetError(error, "Unexpected token in editor layout at line " +
                                    std::to_string(lineNumber));
                return false;
            }
        }

        if (!UnescapeToken(panelToken, placement.panelId) || placement.panelId.empty())
        {
            SetError(error, "Invalid panel id in editor layout at line " + std::to_string(lineNumber));
            return false;
        }
        if (loadedModel.FindPlacement(placement.panelId))
        {
            SetError(error, "Duplicate panel id in editor layout at line " + std::to_string(lineNumber));
            return false;
        }
        if (!TryParseDockArea(areaToken, placement.area))
        {
            SetError(error, "Invalid dock area in editor layout at line " + std::to_string(lineNumber));
            return false;
        }
        if (!std::isfinite(normalizedSize) || normalizedSize <= 0.0f || normalizedSize > 1.0f)
        {
            SetError(error, "Invalid panel size in editor layout at line " + std::to_string(lineNumber));
            return false;
        }
        if (visibleValue != 0 && visibleValue != 1)
        {
            SetError(error, "Invalid panel visibility in editor layout at line " + std::to_string(lineNumber));
            return false;
        }

        placement.normalizedSize = normalizedSize;
        placement.order = order;
        placement.visible = visibleValue != 0;
        loadedModel.DockPanel(std::move(placement));
    }

    if (!foundHeader)
    {
        SetError(error, "Missing editor layout header");
        return false;
    }

    for (const auto& [stackId, activePanelId] : pendingActiveTabs)
    {
        const EditorDockPanelPlacement* placement =
            loadedModel.FindPlacement(activePanelId);
        if (!placement || placement->tabStackId != stackId ||
            !loadedModel.SetActiveDockTab(activePanelId))
        {
            SetError(error, "Invalid editor layout active tab stack state");
            return false;
        }
    }

    model = std::move(loadedModel);
    if (error)
    {
        error->clear();
    }
    return true;
}

bool EditorLayoutSerializer::SaveToFile(const EditorDockingModel& model,
                                        const std::filesystem::path& path,
                                        std::string* error)
{
    if (path.empty())
    {
        SetError(error, "Editor layout path is empty");
        return false;
    }

    if (path.has_parent_path())
    {
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        if (ec)
        {
            SetError(error, "Could not create editor layout directory: " + ec.message());
            return false;
        }
    }

    std::ofstream file(path, std::ios::binary);
    if (!file)
    {
        SetError(error, "Could not open editor layout file for writing");
        return false;
    }

    file << Serialize(model);
    if (!file)
    {
        SetError(error, "Could not write editor layout file");
        return false;
    }

    if (error)
    {
        error->clear();
    }
    return true;
}

bool EditorLayoutSerializer::LoadFromFile(const std::filesystem::path& path,
                                          EditorDockingModel& model,
                                          std::string* error)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
    {
        SetError(error, "Could not open editor layout file for reading");
        return false;
    }

    std::ostringstream contents;
    contents << file.rdbuf();
    if (!file.good() && !file.eof())
    {
        SetError(error, "Could not read editor layout file");
        return false;
    }

    return Deserialize(contents.str(), model, error);
}

} // namespace RVX::Editor
