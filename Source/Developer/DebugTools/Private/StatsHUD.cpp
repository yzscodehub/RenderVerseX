/**
 * @file StatsHUD.cpp
 * @brief Statistics HUD implementation
 */

#include "Debug/StatsHUD.h"
#include "Debug/CPUProfiler.h"
#include "Debug/MemoryTracker.h"
#include "Core/Log.h"
#include <algorithm>
#include <numeric>
#include <sstream>
#include <iomanip>

namespace RVX
{
    // =========================================================================
    // Singleton
    // =========================================================================

    StatsHUD& StatsHUD::Get()
    {
        static StatsHUD instance;
        return instance;
    }

    // =========================================================================
    // Lifecycle
    // =========================================================================

    void StatsHUD::Initialize()
    {
        if (m_initialized)
        {
            return;
        }

        m_frameTimeHistory.reserve(m_historySize);
        m_recentFrameTimes.reserve(120);

        m_initialized = true;
        RVX_CORE_INFO("StatsHUD initialized");
    }

    void StatsHUD::Shutdown()
    {
        if (!m_initialized)
        {
            return;
        }

        m_frameTimeHistory.clear();
        m_recentFrameTimes.clear();
        m_customStats.clear();

        m_initialized = false;
        RVX_CORE_INFO("StatsHUD shutdown");
    }

    // =========================================================================
    // Frame Update
    // =========================================================================

    void StatsHUD::Update(float deltaTime)
    {
        if (!m_initialized || !m_visible)
        {
            return;
        }

        m_frameCount++;
        m_timeSinceUpdate += deltaTime;

        // Accumulate for FPS calculation
        m_fpsAccumulator += deltaTime;
        m_fpsFrameCount++;

        // Update FPS every 0.5 seconds
        if (m_fpsAccumulator >= 0.5f)
        {
            m_fps = static_cast<float>(m_fpsFrameCount) / m_fpsAccumulator;
            m_fpsAccumulator = 0.0f;
            m_fpsFrameCount = 0;
        }

        // Update averaged stats periodically
        if (m_timeSinceUpdate >= m_updateInterval)
        {
            UpdateBuiltinStats();
            UpdateCustomStats();
            m_timeSinceUpdate = 0.0f;
        }
    }

    void StatsHUD::RecordFrameTime(float frameTimeMs)
    {
        m_frameTimeMs = frameTimeMs;

        // Track min/max
        m_minFrameTimeMs = std::min(m_minFrameTimeMs, frameTimeMs);
        m_maxFrameTimeMs = std::max(m_maxFrameTimeMs, frameTimeMs);

        // Add to recent times for averaging
        m_recentFrameTimes.push_back(frameTimeMs);
        if (m_recentFrameTimes.size() > 60)
        {
            m_recentFrameTimes.erase(m_recentFrameTimes.begin());
        }

        // Update history for graphing
        UpdateHistory(m_frameTimeHistory, frameTimeMs);
    }

    void StatsHUD::RecordDrawCalls(uint32 count)
    {
        m_drawCalls = count;
    }

    void StatsHUD::RecordTriangles(uint64 count)
    {
        m_triangles = count;
    }

    void StatsHUD::RecordGPUTime(float gpuTimeMs)
    {
        m_gpuTimeMs = gpuTimeMs;
    }

    // =========================================================================
    // Custom Stats
    // =========================================================================

    void StatsHUD::RegisterStat(
        const std::string& category,
        const std::string& name,
        const std::string& unit,
        StatProvider provider,
        bool showGraph)
    {
        std::string key = category + "." + name;

        CustomStat stat;
        stat.value.name = name;
        stat.value.category = category;
        stat.value.unit = unit;
        stat.value.showGraph = showGraph;
        stat.provider = std::move(provider);

        if (showGraph)
        {
            stat.value.history.reserve(m_historySize);
        }

        m_customStats[key] = std::move(stat);

        // Ensure category visibility entry exists
        if (m_categoryVisibility.find(category) == m_categoryVisibility.end())
        {
            m_categoryVisibility[category] = true;
        }
    }

    void StatsHUD::UnregisterStat(const std::string& category, const std::string& name)
    {
        std::string key = category + "." + name;
        m_customStats.erase(key);
    }

    void StatsHUD::SetStatValue(const std::string& category, const std::string& name, float value)
    {
        std::string key = category + "." + name;

        auto it = m_customStats.find(key);
        if (it != m_customStats.end())
        {
            it->second.value.value = value;

            if (it->second.value.showGraph)
            {
                UpdateHistory(it->second.value.history, value);
            }

            // Update min/max
            it->second.value.minValue = std::min(it->second.value.minValue, value);
            it->second.value.maxValue = std::max(it->second.value.maxValue, value);
        }
    }

    // =========================================================================
    // Display
    // =========================================================================

    std::vector<StatLine> StatsHUD::GetDisplayLines() const
    {
        std::vector<StatLine> lines;

        if (!m_visible || m_displayMode == StatsDisplayMode::Hidden)
        {
            return lines;
        }

        // FPS and frame time (always shown except Hidden)
        if (m_displayMode >= StatsDisplayMode::Minimal)
        {
            StatLine fpsLine;
            fpsLine.label = "FPS";
            fpsLine.value = std::to_string(static_cast<int>(m_fps));
            fpsLine.color = GetColorForFrameTime(m_avgFrameTimeMs);
            lines.push_back(fpsLine);
        }

        if (m_displayMode >= StatsDisplayMode::Basic)
        {
            // Frame time
            StatLine frameLine;
            frameLine.label = "Frame";
            frameLine.value = FormatValue(m_avgFrameTimeMs, "ms");
            frameLine.color = GetColorForFrameTime(m_avgFrameTimeMs);
            lines.push_back(frameLine);

            // Draw calls
            StatLine drawLine;
            drawLine.label = "Draw Calls";
            drawLine.value = std::to_string(m_drawCalls);
            lines.push_back(drawLine);
        }

        if (m_displayMode >= StatsDisplayMode::Extended)
        {
            // Triangles
            StatLine triLine;
            triLine.label = "Triangles";
            if (m_triangles > 1000000)
            {
                triLine.value = FormatValue(static_cast<float>(m_triangles) / 1000000.0f, "M");
            }
            else if (m_triangles > 1000)
            {
                triLine.value = FormatValue(static_cast<float>(m_triangles) / 1000.0f, "K");
            }
            else
            {
                triLine.value = std::to_string(m_triangles);
            }
            lines.push_back(triLine);

            // GPU time
            StatLine gpuLine;
            gpuLine.label = "GPU";
            gpuLine.value = FormatValue(m_gpuTimeMs, "ms");
            lines.push_back(gpuLine);

            // Memory
            if (MemoryTracker::Get().IsInitialized())
            {
                size_t memBytes = MemoryTracker::Get().GetTotalAllocatedBytes();
                StatLine memLine;
                memLine.label = "Memory";
                memLine.value = FormatValue(static_cast<float>(memBytes) / (1024.0f * 1024.0f), "MB");
                lines.push_back(memLine);
            }
        }

        if (m_displayMode >= StatsDisplayMode::Full || m_displayMode == StatsDisplayMode::Custom)
        {
            // Custom stats
            for (const auto& [key, stat] : m_customStats)
            {
                // Check category visibility
                auto catIt = m_categoryVisibility.find(stat.value.category);
                if (catIt != m_categoryVisibility.end() && !catIt->second)
                {
                    continue;
                }

                StatLine line;
                line.label = stat.value.name;
                line.value = FormatValue(stat.value.value, stat.value.unit);
                lines.push_back(line);
            }
        }

        return lines;
    }

    const StatValue* StatsHUD::GetStat(const std::string& category, const std::string& name) const
    {
        std::string key = category + "." + name;

        auto it = m_customStats.find(key);
        if (it != m_customStats.end())
        {
            return &it->second.value;
        }

        return nullptr;
    }

    std::vector<StatCategory> StatsHUD::GetCategories() const
    {
        std::unordered_map<std::string, StatCategory> categoryMap;

        for (const auto& [key, stat] : m_customStats)
        {
            auto& cat = categoryMap[stat.value.category];
            cat.name = stat.value.category;
            cat.statNames.push_back(stat.value.name);

            auto visIt = m_categoryVisibility.find(stat.value.category);
            cat.expanded = (visIt == m_categoryVisibility.end()) || visIt->second;
        }

        std::vector<StatCategory> result;
        result.reserve(categoryMap.size());

        for (auto& [name, cat] : categoryMap)
        {
            result.push_back(std::move(cat));
        }

        // Sort by name
        std::sort(result.begin(), result.end(),
            [](const StatCategory& a, const StatCategory& b) {
                return a.name < b.name;
            });

        return result;
    }

    // =========================================================================
    // Configuration
    // =========================================================================

    void StatsHUD::SetCategoryVisible(const std::string& category, bool visible)
    {
        m_categoryVisibility[category] = visible;
    }

    bool StatsHUD::IsCategoryVisible(const std::string& category) const
    {
        auto it = m_categoryVisibility.find(category);
        return (it == m_categoryVisibility.end()) || it->second;
    }

    // =========================================================================
    // Internal Methods
    // =========================================================================

    void StatsHUD::UpdateBuiltinStats()
    {
        // Calculate average frame time
        if (!m_recentFrameTimes.empty())
        {
            m_avgFrameTimeMs = std::accumulate(m_recentFrameTimes.begin(), m_recentFrameTimes.end(), 0.0f)
                / static_cast<float>(m_recentFrameTimes.size());
        }

        // Reset min/max periodically
        static uint64 lastResetFrame = 0;
        if (m_frameCount - lastResetFrame > 300) // Reset every 5 seconds at 60fps
        {
            m_minFrameTimeMs = m_frameTimeMs;
            m_maxFrameTimeMs = m_frameTimeMs;
            lastResetFrame = m_frameCount;
        }
    }

    void StatsHUD::UpdateCustomStats()
    {
        for (auto& [key, stat] : m_customStats)
        {
            if (stat.provider)
            {
                float value = stat.provider();
                stat.value.value = value;

                // Update history if graphing is enabled
                if (stat.value.showGraph)
                {
                    UpdateHistory(stat.value.history, value);
                }

                // Update min/max/avg
                stat.value.minValue = std::min(stat.value.minValue, value);
                stat.value.maxValue = std::max(stat.value.maxValue, value);

                if (!stat.value.history.empty())
                {
                    stat.value.avgValue = std::accumulate(
                        stat.value.history.begin(),
                        stat.value.history.end(),
                        0.0f
                    ) / static_cast<float>(stat.value.history.size());
                }
            }
        }
    }

    void StatsHUD::UpdateHistory(std::vector<float>& history, float value)
    {
        history.push_back(value);

        if (history.size() > m_historySize)
        {
            history.erase(history.begin());
        }
    }

    std::string StatsHUD::FormatValue(float value, const std::string& unit) const
    {
        std::ostringstream ss;

        if (std::abs(value) < 10.0f)
        {
            ss << std::fixed << std::setprecision(2) << value;
        }
        else if (std::abs(value) < 100.0f)
        {
            ss << std::fixed << std::setprecision(1) << value;
        }
        else
        {
            ss << std::fixed << std::setprecision(0) << value;
        }

        if (!unit.empty())
        {
            ss << " " << unit;
        }

        return ss.str();
    }

    uint32 StatsHUD::GetColorForFrameTime(float timeMs) const
    {
        // RGBA format
        // Green: good (< 16.67ms = 60fps)
        // Yellow: warning (16.67 - 33.33ms = 30-60fps)
        // Red: bad (> 33.33ms = < 30fps)

        if (timeMs < 16.67f)
        {
            return 0x00FF00FF;  // Green
        }
        else if (timeMs < 33.33f)
        {
            return 0xFFFF00FF;  // Yellow
        }
        else
        {
            return 0xFF0000FF;  // Red
        }
    }

} // namespace RVX
