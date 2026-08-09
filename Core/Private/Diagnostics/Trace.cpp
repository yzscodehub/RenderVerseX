/**
 * @file Trace.cpp
 * @brief Thread-safe, opt-in diagnostic tracing for correlated runtime timelines.
 */

#include "Core/Diagnostics/Trace.h"

#include "Core/Diagnostics/JsonWriter.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <functional>
#include <iomanip>
#include <limits>
#include <sstream>
#include <thread>
#include <utility>

namespace RVX::Diagnostics
{
    namespace
    {
        const char* GetTraceEventKindName(TraceEventKind kind) noexcept
        {
            return kind == TraceEventKind::Span ? "span" : "instant";
        }

        void WriteValue(std::ostringstream& json,
                        const TraceAttributeValue& value)
        {
            std::visit(
                [&json](const auto& typedValue)
                {
                    using T = std::decay_t<decltype(typedValue)>;
                    if constexpr (std::is_same_v<T, std::string>)
                    {
                        json << JsonString(typedValue);
                    }
                    else if constexpr (std::is_same_v<T, bool>)
                    {
                        json << JsonBool(typedValue);
                    }
                    else if constexpr (std::is_same_v<T, float64>)
                    {
                        json << std::setprecision(
                            std::numeric_limits<float64>::max_digits10)
                             << typedValue;
                    }
                    else
                    {
                        json << typedValue;
                    }
                },
                value);
        }

        void WriteAttributes(
            std::ostringstream& json,
            const std::map<std::string, TraceAttributeValue>& attributes)
        {
            json << "{";
            bool first = true;
            for (const auto& [name, value] : attributes)
            {
                if (!first)
                {
                    json << ", ";
                }
                json << JsonString(name) << ": ";
                WriteValue(json, value);
                first = false;
            }
            json << "}";
        }
    } // namespace

    bool TraceContext::IsEnabled() const noexcept
    {
        return session != nullptr && session->IsEnabled();
    }

    TraceSpan::TraceSpan(std::shared_ptr<TraceSession> session,
                         TraceSpanId id,
                         TraceCorrelationId correlationId) noexcept
        : m_session(std::move(session)),
          m_id(id),
          m_correlationId(correlationId)
    {
    }

    TraceSpan::~TraceSpan()
    {
        End();
    }

    TraceSpan::TraceSpan(TraceSpan&& other) noexcept
        : m_session(std::move(other.m_session)),
          m_id(std::exchange(other.m_id, 0)),
          m_correlationId(std::exchange(other.m_correlationId, 0))
    {
    }

    TraceSpan& TraceSpan::operator=(TraceSpan&& other) noexcept
    {
        if (this != &other)
        {
            End();
            m_session = std::move(other.m_session);
            m_id = std::exchange(other.m_id, 0);
            m_correlationId = std::exchange(other.m_correlationId, 0);
        }
        return *this;
    }

    bool TraceSpan::IsActive() const noexcept
    {
        return m_session != nullptr && m_id != 0;
    }

    TraceSpanId TraceSpan::GetId() const noexcept
    {
        return m_id;
    }

    TraceContext TraceSpan::GetChildContext() const
    {
        if (!IsActive())
        {
            return {};
        }
        return TraceContext{m_session, m_correlationId, m_id};
    }

    void TraceSpan::SetAttribute(std::string name, TraceAttributeValue value)
    {
        if (IsActive())
        {
            m_session->SetSpanAttribute(m_id, std::move(name), std::move(value));
        }
    }

    void TraceSpan::End() noexcept
    {
        if (!IsActive())
        {
            return;
        }
        m_session->EndSpan(m_id);
        m_id = 0;
        m_correlationId = 0;
        m_session.reset();
    }

    std::shared_ptr<TraceSession> TraceSession::Create(TraceSessionConfig config)
    {
        return std::shared_ptr<TraceSession>(new TraceSession(std::move(config)));
    }

    TraceSession::TraceSession(TraceSessionConfig config)
        : m_config(std::move(config)),
          m_startTime(std::chrono::steady_clock::now()),
          m_enabled(m_config.enabled),
          m_metadata(m_config.metadata)
    {
        if (m_config.schemaId.empty())
        {
            m_config.schemaId = RVX_STARTUP_TIMELINE_SCHEMA_ID;
        }
        if (m_config.schemaVersion == 0U)
        {
            m_config.schemaVersion = RVX_STARTUP_TIMELINE_SCHEMA_VERSION;
        }
    }

    bool TraceSession::IsEnabled() const noexcept
    {
        return m_enabled.load(std::memory_order_acquire);
    }

    TraceContext TraceSession::CreateRootContext()
    {
        if (!IsEnabled())
        {
            return {};
        }

        std::lock_guard lock(m_mutex);
        return TraceContext{shared_from_this(), m_nextCorrelationId++, 0};
    }

    TraceSpan TraceSession::BeginSpan(
        std::string_view name,
        const TraceContext& context,
        std::initializer_list<TraceAttribute> attributes)
    {
        const TraceSpanId id = AddEvent(
            TraceEventKind::Span, name, context, attributes);
        if (id == 0)
        {
            return {};
        }

        TraceCorrelationId correlationId = context.correlationId;
        if (correlationId == 0)
        {
            std::lock_guard lock(m_mutex);
            const auto event = std::find_if(
                m_events.begin(),
                m_events.end(),
                [id](const TraceEvent& event) { return event.id == id; });
            correlationId = event != m_events.end() ? event->correlationId : 0;
        }
        return TraceSpan(shared_from_this(), id, correlationId);
    }

    void TraceSession::RecordInstant(
        std::string_view name,
        const TraceContext& context,
        std::initializer_list<TraceAttribute> attributes)
    {
        static_cast<void>(AddEvent(
            TraceEventKind::Instant, name, context, attributes));
    }

    std::vector<TraceEvent> TraceSession::GetEvents() const
    {
        std::lock_guard lock(m_mutex);
        return m_events;
    }

    void TraceSession::SetMetadata(std::string name, TraceAttributeValue value)
    {
        std::lock_guard lock(m_mutex);
        m_metadata[std::move(name)] = std::move(value);
    }

    std::map<std::string, TraceAttributeValue> TraceSession::GetMetadata() const
    {
        std::lock_guard lock(m_mutex);
        return m_metadata;
    }

    std::string TraceSession::ExportJson() const
    {
        std::vector<TraceEvent> events;
        std::map<std::string, TraceAttributeValue> metadata;
        {
            std::lock_guard lock(m_mutex);
            events = m_events;
            metadata = m_metadata;
        }
        std::sort(events.begin(),
                  events.end(),
                  [](const TraceEvent& left, const TraceEvent& right)
                  {
                      return left.id < right.id;
                  });

        std::ostringstream json;
        json << "{\n";
        json << "  \"schemaId\": " << JsonString(m_config.schemaId) << ",\n";
        json << "  \"schemaVersion\": " << m_config.schemaVersion << ",\n";
        json << "  \"name\": " << JsonString(m_config.name) << ",\n";
        json << "  \"metadata\": ";
        WriteAttributes(json, metadata);
        json << ",\n";
        const uint64 totalDurationNs = ElapsedNanoseconds();
        std::vector<std::pair<uint64, int32>> concurrencyPoints;
        concurrencyPoints.reserve(events.size() * 2U);
        for (const TraceEvent& event : events)
        {
            if (event.kind != TraceEventKind::Span)
            {
                continue;
            }
            concurrencyPoints.emplace_back(event.timestampNs, 1);
            const uint64 endNs = event.completed
                                     ? event.timestampNs + event.durationNs
                                     : totalDurationNs;
            concurrencyPoints.emplace_back(endNs, -1);
        }
        std::sort(concurrencyPoints.begin(),
                  concurrencyPoints.end(),
                  [](const auto& left, const auto& right)
                  {
                      if (left.first != right.first)
                      {
                          return left.first < right.first;
                      }
                      return left.second < right.second;
                  });
        int32 concurrentSpans = 0;
        uint32 peakConcurrentSpans = 0;
        for (const auto& [timestampNs, delta] : concurrencyPoints)
        {
            (void)timestampNs;
            concurrentSpans += delta;
            peakConcurrentSpans = std::max(
                peakConcurrentSpans,
                static_cast<uint32>(std::max(concurrentSpans, 0)));
        }
        json << "  \"summary\": {\"totalDurationNs\": "
             << totalDurationNs
             << ", \"peakConcurrentSpans\": " << peakConcurrentSpans
             << "},\n";
        json << "  \"events\": [\n";
        for (size_t index = 0; index < events.size(); ++index)
        {
            const TraceEvent& event = events[index];
            json << "    {\"id\": " << event.id
                 << ", \"correlationId\": " << event.correlationId
                 << ", \"parentId\": ";
            if (event.parentId == 0)
            {
                json << "null";
            }
            else
            {
                json << event.parentId;
            }
            json << ", \"kind\": " << JsonString(GetTraceEventKindName(event.kind))
                 << ", \"name\": " << JsonString(event.name)
                 << ", \"timestampNs\": " << event.timestampNs
                 << ", \"durationNs\": " << event.durationNs
                 << ", \"threadId\": " << event.threadId
                 << ", \"completed\": " << JsonBool(event.completed)
                 << ", \"attributes\": ";
            WriteAttributes(json, event.attributes);
            json << "}";
            json << (index + 1U == events.size() ? "\n" : ",\n");
        }
        json << "  ]\n";
        json << "}\n";
        return json.str();
    }

    bool TraceSession::SaveJson(const std::filesystem::path& path) const
    {
        if (path.empty())
        {
            return false;
        }

        const std::filesystem::path parent = path.parent_path();
        if (!parent.empty())
        {
            std::error_code error;
            std::filesystem::create_directories(parent, error);
            if (error)
            {
                return false;
            }
        }

        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        if (!file.is_open())
        {
            return false;
        }
        file << ExportJson();
        return file.good();
    }

    TraceSpanId TraceSession::AddEvent(
        TraceEventKind kind,
        std::string_view name,
        const TraceContext& context,
        std::initializer_list<TraceAttribute> attributes)
    {
        if (!IsEnabled())
        {
            return 0;
        }

        std::lock_guard lock(m_mutex);
        TraceEvent event;
        event.kind = kind;
        event.id = m_nextEventId++;
        event.correlationId = context.session.get() == this &&
                                      context.correlationId != 0
                                  ? context.correlationId
                                  : m_nextCorrelationId++;
        event.parentId = context.session.get() == this
                             ? context.parentSpanId
                             : 0;
        event.timestampNs = ElapsedNanoseconds();
        event.threadId = CurrentThreadId();
        event.completed = kind == TraceEventKind::Instant;
        event.name = name;
        for (const TraceAttribute& attribute : attributes)
        {
            event.attributes[attribute.name] = attribute.value;
        }
        m_events.push_back(std::move(event));
        return m_events.back().id;
    }

    void TraceSession::EndSpan(TraceSpanId id) noexcept
    {
        if (!IsEnabled() || id == 0)
        {
            return;
        }

        std::lock_guard lock(m_mutex);
        const auto event = std::find_if(
            m_events.begin(),
            m_events.end(),
            [id](const TraceEvent& candidate) { return candidate.id == id; });
        if (event == m_events.end() || event->kind != TraceEventKind::Span ||
            event->completed)
        {
            return;
        }

        const uint64 nowNs = ElapsedNanoseconds();
        event->durationNs = nowNs >= event->timestampNs
                                ? nowNs - event->timestampNs
                                : 0;
        event->completed = true;
    }

    void TraceSession::SetSpanAttribute(TraceSpanId id,
                                        std::string name,
                                        TraceAttributeValue value)
    {
        if (!IsEnabled() || id == 0)
        {
            return;
        }

        std::lock_guard lock(m_mutex);
        const auto event = std::find_if(
            m_events.begin(),
            m_events.end(),
            [id](const TraceEvent& candidate) { return candidate.id == id; });
        if (event != m_events.end())
        {
            event->attributes[std::move(name)] = std::move(value);
        }
    }

    uint64 TraceSession::ElapsedNanoseconds() const noexcept
    {
        const auto elapsed = std::chrono::steady_clock::now() - m_startTime;
        return static_cast<uint64>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count());
    }

    uint64 TraceSession::CurrentThreadId() const noexcept
    {
        return static_cast<uint64>(
            std::hash<std::thread::id>{}(std::this_thread::get_id()));
    }

    TraceSpan BeginTraceSpan(
        const TraceContext& context,
        std::string_view name,
        std::initializer_list<TraceAttribute> attributes)
    {
        return context.IsEnabled()
                   ? context.session->BeginSpan(name, context, attributes)
                   : TraceSpan{};
    }

    void RecordTraceInstant(
        const TraceContext& context,
        std::string_view name,
        std::initializer_list<TraceAttribute> attributes)
    {
        if (context.IsEnabled())
        {
            context.session->RecordInstant(name, context, attributes);
        }
    }
} // namespace RVX::Diagnostics
