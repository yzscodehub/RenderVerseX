/**
 * @file Console.h
 * @brief Console/log output panel
 */

#pragma once

#include "Editor/Panels/IEditorPanel.h"
#include <vector>
#include <string>
#include <mutex>

namespace RVX::Editor
{

/**
 * @brief Log level for console messages
 */
enum class ConsoleLogLevel : uint8
{
    Trace,
    Debug,
    Info,
    Warning,
    Error,
    Critical
};

/**
 * @brief Console message entry
 */
struct ConsoleMessage
{
    ConsoleLogLevel level;
    std::string message;
    std::string source;
    std::string file;
    int line = 0;
    uint64 timestamp;
    uint32 count = 1;  // For collapsing duplicate messages
};

/**
 * @brief Console panel for log output
 */
class ConsolePanel : public IEditorPanel
{
public:
    ConsolePanel();
    ~ConsolePanel() override = default;

    // =========================================================================
    // IEditorPanel Interface
    // =========================================================================

    const char* GetName() const override { return "Console"; }
    const char* GetIcon() const override { return "console"; }
    void OnGUI() override;

    // =========================================================================
    // Logging API (Static - thread-safe)
    // =========================================================================

    static void Log(const std::string& message, ConsoleLogLevel level = ConsoleLogLevel::Info);
    static void LogTrace(const std::string& message);
    static void LogDebug(const std::string& message);
    static void LogInfo(const std::string& message);
    static void LogWarning(const std::string& message);
    static void LogError(const std::string& message);
    static void LogCritical(const std::string& message);

    static void Log(const std::string& message, const std::string& source, 
                    const std::string& file, int line, ConsoleLogLevel level);

    static void Clear();
    static uint32 GetMessageCount(ConsoleLogLevel level);

private:
    void DrawToolbar();
    void DrawFilterButtons();
    void DrawMessages();
    void DrawMessage(const ConsoleMessage& message, int index);
    void CopyToClipboard(const ConsoleMessage& message);

    const char* GetLevelIcon(ConsoleLogLevel level) const;
    uint32 GetLevelColor(ConsoleLogLevel level) const;

    static std::vector<ConsoleMessage> s_messages;
    static std::mutex s_mutex;
    static uint32 s_maxMessages;
    static uint32 s_infoCount;
    static uint32 s_warningCount;
    static uint32 s_errorCount;

    bool m_autoScroll = true;
    bool m_showTrace = false;
    bool m_showDebug = false;
    bool m_showInfo = true;
    bool m_showWarnings = true;
    bool m_showErrors = true;
    bool m_collapse = true;
    bool m_showTimestamps = true;
    bool m_showSource = true;
    std::string m_filter;
    int m_selectedMessage = -1;
};

} // namespace RVX::Editor
