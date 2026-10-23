#pragma once

/**
 * @file Trace.h
 * @brief Thread-safe, opt-in diagnostic tracing for correlated runtime timelines.
 */

#include "Core/Types.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <initializer_list>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>
#include <variant>

namespace RVX::Diagnostics
{
    using TraceSpanId = uint64;
    using TraceCorrelationId = uint64;

    inline constexpr uint32 RVX_STARTUP_TIMELINE_SCHEMA_VERSION = 1;
    inline constexpr const char* RVX_STARTUP_TIMELINE_SCHEMA_ID =
        "RVX.SampleStartupTimeline";

    using TraceAttributeValue = std::variant<std::string, bool, int64, uint64, float64>;

    struct TraceAttribute
    {
        std::string name;
        TraceAttributeValue value;

        TraceAttribute(std::string_view attributeName, const char* attributeValue)
            : name(attributeName),
              value(std::string(attributeValue != nullptr ? attributeValue : ""))
        {
        }

        TraceAttribute(std::string_view attributeName, std::string attributeValue)
            : name(attributeName),
              value(std::move(attributeValue))
        {
        }

        TraceAttribute(std::string_view attributeName, std::string_view attributeValue)
            : name(attributeName),
              value(std::string(attributeValue))
        {
        }

        TraceAttribute(std::string_view attributeName, bool attributeValue)
            : name(attributeName),
              value(attributeValue)
        {
        }

        template<typename T>
        requires(std::is_integral_v<T> && !std::is_same_v<std::remove_cv_t<T>, bool>)
        TraceAttribute(std::string_view attributeName, T attributeValue)
            : name(attributeName),
              value(MakeIntegralValue(attributeValue))
        {
        }

        TraceAttribute(std::string_view attributeName, float64 attributeValue)
            : name(attributeName),
              value(attributeValue)
        {
        }

    private:
        template<typename T>
        static TraceAttributeValue MakeIntegralValue(T value)
        {
            if constexpr (std::is_signed_v<T>)
            {
                return static_cast<int64>(value);
            }
            else
            {
                return static_cast<uint64>(value);
            }
        }
    };

    enum class TraceEventKind : uint8
    {
        Span = 0,
        Instant = 1
    };

    struct TraceEvent
    {
        TraceEventKind kind = TraceEventKind::Instant;
        TraceSpanId id = 0;
        TraceCorrelationId correlationId = 0;
        TraceSpanId parentId = 0;
        uint64 timestampNs = 0;
        uint64 durationNs = 0;
        uint64 threadId = 0;
        bool completed = true;
        std::string name;
        std::map<std::string, TraceAttributeValue> attributes;
    };

    struct TraceSessionConfig
    {
        /** @brief Disabled by default so production callers pay no trace cost. */
        bool enabled = false;
        std::string name = "startup";
        std::string schemaId = RVX_STARTUP_TIMELINE_SCHEMA_ID;
        uint32 schemaVersion = RVX_STARTUP_TIMELINE_SCHEMA_VERSION;
        std::map<std::string, TraceAttributeValue> metadata;
    };

    class TraceSession;

    struct TraceContext
    {
        std::shared_ptr<TraceSession> session;
        TraceCorrelationId correlationId = 0;
        TraceSpanId parentSpanId = 0;

        [[nodiscard]] bool IsEnabled() const noexcept;
    };

    /** @brief Move-only RAII span that completes at an observed boundary. */
    class TraceSpan final
    {
    public:
        TraceSpan() = default;
        ~TraceSpan();

        TraceSpan(const TraceSpan&) = delete;
        TraceSpan& operator=(const TraceSpan&) = delete;
        TraceSpan(TraceSpan&& other) noexcept;
        TraceSpan& operator=(TraceSpan&& other) noexcept;

        [[nodiscard]] bool IsActive() const noexcept;
        [[nodiscard]] TraceSpanId GetId() const noexcept;
        [[nodiscard]] TraceContext GetChildContext() const;
        void SetAttribute(std::string name, TraceAttributeValue value);
        void End() noexcept;

    private:
        friend class TraceSession;

        TraceSpan(std::shared_ptr<TraceSession> session,
                  TraceSpanId id,
                  TraceCorrelationId correlationId) noexcept;

        std::shared_ptr<TraceSession> m_session;
        TraceSpanId m_id = 0;
        TraceCorrelationId m_correlationId = 0;
    };

    /**
     * @brief Owns a synchronized, monotonic timeline.
     *
     * Construct sessions through Create().  A disabled session is a safe
     * default and all recording methods become no-ops.
     */
    class TraceSession final : public std::enable_shared_from_this<TraceSession>
    {
    public:
        [[nodiscard]] static std::shared_ptr<TraceSession> Create(
            TraceSessionConfig config = {});

        [[nodiscard]] bool IsEnabled() const noexcept;
        [[nodiscard]] TraceContext CreateRootContext();
        [[nodiscard]] TraceSpan BeginSpan(
            std::string_view name,
            const TraceContext& context = {},
            std::initializer_list<TraceAttribute> attributes = {});
        void RecordInstant(
            std::string_view name,
            const TraceContext& context = {},
            std::initializer_list<TraceAttribute> attributes = {});

        [[nodiscard]] std::vector<TraceEvent> GetEvents() const;
        void SetMetadata(std::string name, TraceAttributeValue value);
        [[nodiscard]] std::map<std::string, TraceAttributeValue> GetMetadata() const;
        [[nodiscard]] std::string ExportJson() const;
        [[nodiscard]] bool SaveJson(const std::filesystem::path& path) const;

    private:
        friend class TraceSpan;

        explicit TraceSession(TraceSessionConfig config);

        [[nodiscard]] TraceSpanId AddEvent(
            TraceEventKind kind,
            std::string_view name,
            const TraceContext& context,
            std::initializer_list<TraceAttribute> attributes);
        void EndSpan(TraceSpanId id) noexcept;
        void SetSpanAttribute(TraceSpanId id,
                              std::string name,
                              TraceAttributeValue value);
        [[nodiscard]] uint64 ElapsedNanoseconds() const noexcept;
        [[nodiscard]] uint64 CurrentThreadId() const noexcept;

        TraceSessionConfig m_config;
        const std::chrono::steady_clock::time_point m_startTime;
        std::atomic<bool> m_enabled{false};
        mutable std::mutex m_mutex;
        TraceSpanId m_nextEventId = 1;
        TraceCorrelationId m_nextCorrelationId = 1;
        std::vector<TraceEvent> m_events;
        std::map<std::string, TraceAttributeValue> m_metadata;
    };

    [[nodiscard]] TraceSpan BeginTraceSpan(
        const TraceContext& context,
        std::string_view name,
        std::initializer_list<TraceAttribute> attributes = {});
    void RecordTraceInstant(
        const TraceContext& context,
        std::string_view name,
        std::initializer_list<TraceAttribute> attributes = {});
} // namespace RVX::Diagnostics
