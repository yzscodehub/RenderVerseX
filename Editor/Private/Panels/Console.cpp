/**
 * @file Console.cpp
 * @brief Console panel implementation
 */

#include "Editor/Panels/Console.h"
#include <imgui.h>
#include <chrono>
#include <ctime>
#include <algorithm>

namespace RVX::Editor
{

std::vector<ConsoleMessage> ConsolePanel::s_messages;
std::mutex ConsolePanel::s_mutex;
uint32 ConsolePanel::s_maxMessages = 2000;
uint32 ConsolePanel::s_infoCount = 0;
uint32 ConsolePanel::s_warningCount = 0;
uint32 ConsolePanel::s_errorCount = 0;

ConsolePanel::ConsolePanel()
{
    m_filter.resize(256);
}

void ConsolePanel::OnGUI()
{
    if (!ImGui::Begin(GetName()))
    {
        ImGui::End();
        return;
    }

    DrawToolbar();
    ImGui::Separator();
    DrawMessages();

    ImGui::End();
}

void ConsolePanel::DrawToolbar()
{
    // Clear button
    if (ImGui::Button("Clear"))
    {
        Clear();
    }

    ImGui::SameLine();
    ImGui::Checkbox("Auto-scroll", &m_autoScroll);

    ImGui::SameLine();
    ImGui::Checkbox("Collapse", &m_collapse);

    ImGui::SameLine();
    ImGui::Separator();
    ImGui::SameLine();

    DrawFilterButtons();

    ImGui::SameLine();
    ImGui::Separator();
    ImGui::SameLine();

    // Search filter
    ImGui::SetNextItemWidth(200);
    ImGui::InputTextWithHint("##Filter", "Filter...", m_filter.data(), m_filter.size());

    ImGui::SameLine();

    // Options
    if (ImGui::Button("..."))
    {
        ImGui::OpenPopup("ConsoleOptions");
    }

    if (ImGui::BeginPopup("ConsoleOptions"))
    {
        ImGui::Checkbox("Show Timestamps", &m_showTimestamps);
        ImGui::Checkbox("Show Source", &m_showSource);
        ImGui::Checkbox("Show Trace", &m_showTrace);
        ImGui::Checkbox("Show Debug", &m_showDebug);
        ImGui::Separator();
        int maxMsgs = static_cast<int>(s_maxMessages);
        if (ImGui::SliderInt("Max Messages", &maxMsgs, 100, 10000))
        {
            s_maxMessages = static_cast<uint32>(maxMsgs);
        }
        ImGui::EndPopup();
    }
}

void ConsolePanel::DrawFilterButtons()
{
    // Info toggle
    {
        ImVec4 color = m_showInfo ? ImVec4(0.2f, 0.6f, 0.9f, 1.0f) : ImVec4(0.4f, 0.4f, 0.4f, 1.0f);
        ImGui::PushStyleColor(ImGuiCol_Button, color);
        char label[32];
        snprintf(label, sizeof(label), "Info (%u)", s_infoCount);
        if (ImGui::Button(label))
        {
            m_showInfo = !m_showInfo;
        }
        ImGui::PopStyleColor();
    }

    ImGui::SameLine();

    // Warning toggle
    {
        ImVec4 color = m_showWarnings ? ImVec4(0.9f, 0.7f, 0.1f, 1.0f) : ImVec4(0.4f, 0.4f, 0.4f, 1.0f);
        ImGui::PushStyleColor(ImGuiCol_Button, color);
        char label[32];
        snprintf(label, sizeof(label), "Warn (%u)", s_warningCount);
        if (ImGui::Button(label))
        {
            m_showWarnings = !m_showWarnings;
        }
        ImGui::PopStyleColor();
    }

    ImGui::SameLine();

    // Error toggle
    {
        ImVec4 color = m_showErrors ? ImVec4(0.9f, 0.2f, 0.2f, 1.0f) : ImVec4(0.4f, 0.4f, 0.4f, 1.0f);
        ImGui::PushStyleColor(ImGuiCol_Button, color);
        char label[32];
        snprintf(label, sizeof(label), "Error (%u)", s_errorCount);
        if (ImGui::Button(label))
        {
            m_showErrors = !m_showErrors;
        }
        ImGui::PopStyleColor();
    }
}

void ConsolePanel::DrawMessages()
{
    ImGui::BeginChild("ConsoleScrollRegion", ImVec2(0, 0), false, ImGuiWindowFlags_HorizontalScrollbar);

    std::lock_guard<std::mutex> lock(s_mutex);

    std::string filterLower = m_filter.data();
    std::transform(filterLower.begin(), filterLower.end(), filterLower.begin(), ::tolower);

    int displayIndex = 0;
    for (size_t i = 0; i < s_messages.size(); ++i)
    {
        const auto& msg = s_messages[i];

        // Filter by level
        bool show = false;
        switch (msg.level)
        {
            case ConsoleLogLevel::Trace: show = m_showTrace; break;
            case ConsoleLogLevel::Debug: show = m_showDebug; break;
            case ConsoleLogLevel::Info: show = m_showInfo; break;
            case ConsoleLogLevel::Warning: show = m_showWarnings; break;
            case ConsoleLogLevel::Error:
            case ConsoleLogLevel::Critical: show = m_showErrors; break;
        }

        if (!show)
            continue;

        // Filter by text
        if (!filterLower.empty())
        {
            std::string msgLower = msg.message;
            std::transform(msgLower.begin(), msgLower.end(), msgLower.begin(), ::tolower);
            if (msgLower.find(filterLower) == std::string::npos)
                continue;
        }

        DrawMessage(msg, displayIndex++);
    }

    // Auto-scroll
    if (m_autoScroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
    {
        ImGui::SetScrollHereY(1.0f);
    }

    ImGui::EndChild();
}

void ConsolePanel::DrawMessage(const ConsoleMessage& message, int index)
{
    ImGui::PushID(index);

    // Background color based on level and selection
    ImVec4 bgColor;
    bool isSelected = (m_selectedMessage == index);
    
    if (isSelected)
    {
        bgColor = ImVec4(0.2f, 0.4f, 0.6f, 0.5f);
    }
    else if (index % 2 == 0)
    {
        bgColor = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    }
    else
    {
        bgColor = ImVec4(0.05f, 0.05f, 0.05f, 0.5f);
    }

    // Get text color
    ImU32 textColor = GetLevelColor(message.level);

    // Draw row
    ImGui::PushStyleColor(ImGuiCol_Header, bgColor);
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.2f, 0.3f, 0.5f, 0.5f));

    // Selectable row
    if (ImGui::Selectable("##MessageRow", isSelected, 
                          ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick))
    {
        m_selectedMessage = index;
    }

    // Context menu
    if (ImGui::BeginPopupContextItem())
    {
        if (ImGui::MenuItem("Copy"))
        {
            CopyToClipboard(message);
        }
        if (ImGui::MenuItem("Copy All"))
        {
            std::string allMessages;
            for (const auto& msg : s_messages)
            {
                allMessages += msg.message + "\n";
            }
            ImGui::SetClipboardText(allMessages.c_str());
        }
        ImGui::EndPopup();
    }

    ImGui::PopStyleColor(2);

    // Draw message content on same line
    ImGui::SameLine();

    // Icon
    const char* icon = GetLevelIcon(message.level);
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(textColor), "%s", icon);
    ImGui::SameLine();

    // Timestamp
    if (m_showTimestamps)
    {
        time_t time = static_cast<time_t>(message.timestamp);
        struct tm* tm = localtime(&time);
        char timeStr[16];
        strftime(timeStr, sizeof(timeStr), "%H:%M:%S", tm);
        ImGui::TextDisabled("[%s]", timeStr);
        ImGui::SameLine();
    }

    // Source
    if (m_showSource && !message.source.empty())
    {
        ImGui::TextDisabled("[%s]", message.source.c_str());
        ImGui::SameLine();
    }

    // Count badge (for collapsed duplicates)
    if (m_collapse && message.count > 1)
    {
        ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "(%u)", message.count);
        ImGui::SameLine();
    }

    // Message text
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(textColor), "%s", message.message.c_str());

    ImGui::PopID();
}

void ConsolePanel::CopyToClipboard(const ConsoleMessage& message)
{
    std::string text;
    
    if (m_showTimestamps)
    {
        time_t time = static_cast<time_t>(message.timestamp);
        struct tm* tm = localtime(&time);
        char timeStr[32];
        strftime(timeStr, sizeof(timeStr), "[%H:%M:%S] ", tm);
        text += timeStr;
    }

    if (!message.source.empty())
    {
        text += "[" + message.source + "] ";
    }

    text += message.message;

    if (!message.file.empty())
    {
        text += " (" + message.file + ":" + std::to_string(message.line) + ")";
    }

    ImGui::SetClipboardText(text.c_str());
}

const char* ConsolePanel::GetLevelIcon(ConsoleLogLevel level) const
{
    switch (level)
    {
        case ConsoleLogLevel::Trace:    return "[T]";
        case ConsoleLogLevel::Debug:    return "[D]";
        case ConsoleLogLevel::Info:     return "[I]";
        case ConsoleLogLevel::Warning:  return "[W]";
        case ConsoleLogLevel::Error:    return "[E]";
        case ConsoleLogLevel::Critical: return "[!]";
        default: return "[ ]";
    }
}

uint32 ConsolePanel::GetLevelColor(ConsoleLogLevel level) const
{
    switch (level)
    {
        case ConsoleLogLevel::Trace:    return IM_COL32(150, 150, 150, 255);
        case ConsoleLogLevel::Debug:    return IM_COL32(100, 180, 200, 255);
        case ConsoleLogLevel::Info:     return IM_COL32(200, 200, 200, 255);
        case ConsoleLogLevel::Warning:  return IM_COL32(230, 180, 50, 255);
        case ConsoleLogLevel::Error:    return IM_COL32(230, 80, 80, 255);
        case ConsoleLogLevel::Critical: return IM_COL32(255, 50, 50, 255);
        default: return IM_COL32(200, 200, 200, 255);
    }
}

// Static methods
void ConsolePanel::Log(const std::string& message, ConsoleLogLevel level)
{
    Log(message, "", "", 0, level);
}

void ConsolePanel::Log(const std::string& message, const std::string& source,
                       const std::string& file, int line, ConsoleLogLevel level)
{
    std::lock_guard<std::mutex> lock(s_mutex);

    // Check for duplicate (collapse mode)
    if (!s_messages.empty())
    {
        auto& last = s_messages.back();
        if (last.message == message && last.level == level)
        {
            last.count++;
            return;
        }
    }

    // Create new message
    ConsoleMessage msg;
    msg.level = level;
    msg.message = message;
    msg.source = source;
    msg.file = file;
    msg.line = line;
    msg.timestamp = static_cast<uint64>(std::chrono::system_clock::to_time_t(
        std::chrono::system_clock::now()));
    msg.count = 1;

    // Update counters
    switch (level)
    {
        case ConsoleLogLevel::Info: s_infoCount++; break;
        case ConsoleLogLevel::Warning: s_warningCount++; break;
        case ConsoleLogLevel::Error:
        case ConsoleLogLevel::Critical: s_errorCount++; break;
        default: break;
    }

    // Add message
    if (s_messages.size() >= s_maxMessages)
    {
        // Remove oldest and update counters
        auto& oldest = s_messages.front();
        switch (oldest.level)
        {
            case ConsoleLogLevel::Info: if (s_infoCount > 0) s_infoCount--; break;
            case ConsoleLogLevel::Warning: if (s_warningCount > 0) s_warningCount--; break;
            case ConsoleLogLevel::Error:
            case ConsoleLogLevel::Critical: if (s_errorCount > 0) s_errorCount--; break;
            default: break;
        }
        s_messages.erase(s_messages.begin());
    }

    s_messages.push_back(std::move(msg));
}

void ConsolePanel::LogTrace(const std::string& message)
{
    Log(message, ConsoleLogLevel::Trace);
}

void ConsolePanel::LogDebug(const std::string& message)
{
    Log(message, ConsoleLogLevel::Debug);
}

void ConsolePanel::LogInfo(const std::string& message)
{
    Log(message, ConsoleLogLevel::Info);
}

void ConsolePanel::LogWarning(const std::string& message)
{
    Log(message, ConsoleLogLevel::Warning);
}

void ConsolePanel::LogError(const std::string& message)
{
    Log(message, ConsoleLogLevel::Error);
}

void ConsolePanel::LogCritical(const std::string& message)
{
    Log(message, ConsoleLogLevel::Critical);
}

void ConsolePanel::Clear()
{
    std::lock_guard<std::mutex> lock(s_mutex);
    s_messages.clear();
    s_infoCount = 0;
    s_warningCount = 0;
    s_errorCount = 0;
}

uint32 ConsolePanel::GetMessageCount(ConsoleLogLevel level)
{
    switch (level)
    {
        case ConsoleLogLevel::Info: return s_infoCount;
        case ConsoleLogLevel::Warning: return s_warningCount;
        case ConsoleLogLevel::Error:
        case ConsoleLogLevel::Critical: return s_errorCount;
        default: return 0;
    }
}

} // namespace RVX::Editor
