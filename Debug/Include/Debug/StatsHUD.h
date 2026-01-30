#pragma once

/**
 * @file StatsHUD.h
 * @brief Runtime statistics HUD display
 * 
 * Features:
 * - FPS and frame time display
 * - Draw call and triangle counts
 * - Memory usage statistics
 * - GPU timing information
 * - Customizable stat groups
 */

#include "Core/Types.h"
#include <string>
#include <vector>
#include <functional>
#include <unordered_map>

namespace RVX
{
    /**
     * @brief Stat display modes
     */
    enum class StatsDisplayMode : uint8
    {
        Hidden,         // Not displayed
        Minimal,        // FPS only
        Basic,          // FPS + frame time + draw calls
        Extended,       // All common stats
        Full,           // All stats including custom
        Custom          // User-selected stats
    };

    /**
     * @brief Individual stat value
     */
    struct StatValue
    {
        std::string name;
        std::string category;
        std::string unit;           // e.g., "ms", "MB", ""
        float value = 0.0f;
        float minValue = 0.0f;
        float maxValue = 0.0f;
        float avgValue = 0.0f;
        bool showGraph = false;     // Show history graph
        std::vector<float> history; // For graphing
    };

    /**
     * @brief Stat category for grouping
     */
    struct StatCategory
    {
        std::string name;
        bool expanded = true;
        std::vector<std::string> statNames;
    };

    /**
     * @brief Provider function for custom stats
     */
    using StatProvider = std::function<float()>;

    /**
     * @brief Formatted stat line for display
     */
    struct StatLine
    {
        std::string label;
        std::string value;
        uint32 color = 0xFFFFFFFF;  // RGBA
    };

    /**
     * @brief Runtime statistics HUD
     * 
     * Usage:
     * @code
     * // Initialize
     * StatsHUD::Get().Initialize();
     * 
     * // Register custom stats
     * StatsHUD::Get().RegisterStat("Physics", "RigidBodies", "count", 
     *     []() { return PhysicsWorld::Get().GetBodyCount(); });
     * 
     * // Each frame
     * StatsHUD::Get().Update(deltaTime);
     * 
     * // Get display lines
     * auto lines = StatsHUD::Get().GetDisplayLines();
     * @endcode
     */
    class StatsHUD
    {
    public:
        // =========================================================================
        // Singleton Access
        // =========================================================================

        /**
         * @brief Get the global HUD instance
         */
        static StatsHUD& Get();

        // =========================================================================
        // Lifecycle
        // =========================================================================

        /**
         * @brief Initialize the stats HUD
         */
        void Initialize();

        /**
         * @brief Shutdown
         */
        void Shutdown();

        /**
         * @brief Check if initialized
         */
        bool IsInitialized() const { return m_initialized; }

        // =========================================================================
        // Frame Update
        // =========================================================================

        /**
         * @brief Update stats for the current frame
         * @param deltaTime Frame delta time in seconds
         */
        void Update(float deltaTime);

        /**
         * @brief Record frame timing
         */
        void RecordFrameTime(float frameTimeMs);

        /**
         * @brief Record draw call count
         */
        void RecordDrawCalls(uint32 count);

        /**
         * @brief Record triangle count
         */
        void RecordTriangles(uint64 count);

        /**
         * @brief Record GPU time
         */
        void RecordGPUTime(float gpuTimeMs);

        // =========================================================================
        // Custom Stats
        // =========================================================================

        /**
         * @brief Register a custom stat with a provider function
         * @param category Category name
         * @param name Stat name
         * @param unit Unit string (e.g., "ms", "MB")
         * @param provider Function that returns the stat value
         * @param showGraph Whether to show a history graph
         */
        void RegisterStat(
            const std::string& category,
            const std::string& name,
            const std::string& unit,
            StatProvider provider,
            bool showGraph = false
        );

        /**
         * @brief Unregister a custom stat
         */
        void UnregisterStat(const std::string& category, const std::string& name);

        /**
         * @brief Set a stat value directly (for non-provider stats)
         */
        void SetStatValue(const std::string& category, const std::string& name, float value);

        // =========================================================================
        // Display
        // =========================================================================

        /**
         * @brief Get formatted display lines for rendering
         */
        std::vector<StatLine> GetDisplayLines() const;

        /**
         * @brief Get a specific stat value
         */
        const StatValue* GetStat(const std::string& category, const std::string& name) const;

        /**
         * @brief Get all categories
         */
        std::vector<StatCategory> GetCategories() const;

        // =========================================================================
        // Configuration
        // =========================================================================

        /**
         * @brief Set display mode
         */
        void SetDisplayMode(StatsDisplayMode mode) { m_displayMode = mode; }
        StatsDisplayMode GetDisplayMode() const { return m_displayMode; }

        /**
         * @brief Toggle visibility
         */
        void SetVisible(bool visible) { m_visible = visible; }
        bool IsVisible() const { return m_visible; }

        /**
         * @brief Toggle a stat category visibility
         */
        void SetCategoryVisible(const std::string& category, bool visible);
        bool IsCategoryVisible(const std::string& category) const;

        /**
         * @brief Set history size for graphs
         */
        void SetHistorySize(size_t size) { m_historySize = size; }

        /**
         * @brief Set update interval (stats are averaged over this period)
         */
        void SetUpdateInterval(float seconds) { m_updateInterval = seconds; }

        // =========================================================================
        // Built-in Stats Access
        // =========================================================================

        /**
         * @brief Get current FPS
         */
        float GetFPS() const { return m_fps; }

        /**
         * @brief Get frame time in milliseconds
         */
        float GetFrameTimeMs() const { return m_frameTimeMs; }

        /**
         * @brief Get averaged frame time
         */
        float GetAvgFrameTimeMs() const { return m_avgFrameTimeMs; }

        /**
         * @brief Get draw call count
         */
        uint32 GetDrawCalls() const { return m_drawCalls; }

        /**
         * @brief Get triangle count
         */
        uint64 GetTriangles() const { return m_triangles; }

        /**
         * @brief Get GPU time in milliseconds
         */
        float GetGPUTimeMs() const { return m_gpuTimeMs; }

        /**
         * @brief Get frame time history for graphing
         */
        const std::vector<float>& GetFrameTimeHistory() const { return m_frameTimeHistory; }

    private:
        StatsHUD() = default;
        ~StatsHUD() { Shutdown(); }

        // Non-copyable
        StatsHUD(const StatsHUD&) = delete;
        StatsHUD& operator=(const StatsHUD&) = delete;

        void UpdateBuiltinStats();
        void UpdateCustomStats();
        void UpdateHistory(std::vector<float>& history, float value);
        std::string FormatValue(float value, const std::string& unit) const;
        uint32 GetColorForFrameTime(float timeMs) const;

        // =========================================================================
        // Internal Types
        // =========================================================================

        struct CustomStat
        {
            StatValue value;
            StatProvider provider;
        };

        // =========================================================================
        // Internal State
        // =========================================================================

        bool m_initialized = false;
        bool m_visible = true;
        StatsDisplayMode m_displayMode = StatsDisplayMode::Basic;

        // Configuration
        size_t m_historySize = 120;      // 2 seconds at 60fps
        float m_updateInterval = 0.5f;   // Update averaged stats every 0.5s

        // Built-in stats
        float m_fps = 0.0f;
        float m_frameTimeMs = 0.0f;
        float m_avgFrameTimeMs = 0.0f;
        float m_minFrameTimeMs = FLT_MAX;
        float m_maxFrameTimeMs = 0.0f;
        uint32 m_drawCalls = 0;
        uint64 m_triangles = 0;
        float m_gpuTimeMs = 0.0f;

        // Frame time tracking
        std::vector<float> m_frameTimeHistory;
        std::vector<float> m_recentFrameTimes;
        float m_timeSinceUpdate = 0.0f;
        uint64 m_frameCount = 0;

        // Custom stats
        std::unordered_map<std::string, CustomStat> m_customStats;  // key = "category.name"
        std::unordered_map<std::string, bool> m_categoryVisibility;

        // FPS calculation
        float m_fpsAccumulator = 0.0f;
        uint32 m_fpsFrameCount = 0;
    };

} // namespace RVX

// =============================================================================
// Stats Macros
// =============================================================================

/**
 * @brief Record a scoped stat (automatically measures time)
 */
#define RVX_STAT_SCOPE(category, name) \
    struct _StatScope_##__LINE__ { \
        float startTime; \
        _StatScope_##__LINE__() : startTime(/* get time */) {} \
        ~_StatScope_##__LINE__() { \
            ::RVX::StatsHUD::Get().SetStatValue(category, name, /* elapsed */); \
        } \
    } _stat_scope_##__LINE__
