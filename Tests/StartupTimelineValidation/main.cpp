#include "Core/Diagnostics/Trace.h"

#include <gtest/gtest.h>

#include <array>
#include <string>
#include <thread>
#include <vector>

namespace
{
    TEST(StartupTimelineValidation, DisabledSessionRecordsNoEvents)
    {
        const std::shared_ptr<RVX::Diagnostics::TraceSession> session =
            RVX::Diagnostics::TraceSession::Create();
        const RVX::Diagnostics::TraceContext context =
            session->CreateRootContext();
        RVX::Diagnostics::TraceSpan span = session->BeginSpan(
            "DisabledSpan", context);
        session->RecordInstant("DisabledInstant", context);

        EXPECT_FALSE(context.IsEnabled());
        EXPECT_FALSE(span.IsActive());
        EXPECT_TRUE(session->GetEvents().empty());
    }

    TEST(StartupTimelineValidation, CorrelatesNestedSpansAndPreservesTypes)
    {
        RVX::Diagnostics::TraceSessionConfig config;
        config.enabled = true;
        config.metadata.emplace("build", std::string("validation"));
        const std::shared_ptr<RVX::Diagnostics::TraceSession> session =
            RVX::Diagnostics::TraceSession::Create(std::move(config));
        const RVX::Diagnostics::TraceContext rootContext =
            session->CreateRootContext();
        RVX::Diagnostics::TraceSpan root = session->BeginSpan(
            "Sample.Startup", rootContext, {{"sample", "unit"}});
        RVX::Diagnostics::TraceSpan child = session->BeginSpan(
            "Engine.Initialize", root.GetChildContext());
        session->RecordInstant(
            "Resource.Load.Ready",
            child.GetChildContext(),
            {{"boolean", true},
             {"signed", static_cast<RVX::int64>(-7)},
             {"unsigned", static_cast<RVX::uint64>(9)},
             {"double", 1.25}});
        child.End();
        root.End();

        const std::vector<RVX::Diagnostics::TraceEvent> events =
            session->GetEvents();
        ASSERT_EQ(events.size(), 3U);
        EXPECT_EQ(events[0].id, 1U);
        EXPECT_EQ(events[1].parentId, events[0].id);
        EXPECT_EQ(events[2].parentId, events[1].id);
        EXPECT_EQ(events[0].correlationId, events[1].correlationId);
        EXPECT_EQ(events[1].correlationId, events[2].correlationId);
        EXPECT_TRUE(events[0].completed);
        EXPECT_TRUE(events[1].completed);
        EXPECT_EQ(std::get<bool>(events[2].attributes.at("boolean")), true);
        EXPECT_EQ(std::get<RVX::int64>(events[2].attributes.at("signed")), -7);
        EXPECT_EQ(std::get<RVX::uint64>(events[2].attributes.at("unsigned")), 9U);
        EXPECT_DOUBLE_EQ(std::get<RVX::float64>(events[2].attributes.at("double")),
                         1.25);

        const std::string json = session->ExportJson();
        EXPECT_NE(json.find("\"schemaId\": \"RVX.SampleStartupTimeline\""),
                  std::string::npos);
        EXPECT_NE(json.find("\"timestampNs\""), std::string::npos);
        EXPECT_NE(json.find("\"durationNs\""), std::string::npos);
        EXPECT_NE(json.find("\"totalDurationNs\""), std::string::npos);
        EXPECT_NE(json.find("\"peakConcurrentSpans\""), std::string::npos);
        EXPECT_NE(json.find("\"boolean\": true"), std::string::npos);
        EXPECT_NE(json.find("\"signed\": -7"), std::string::npos);
        EXPECT_NE(json.find("\"unsigned\": 9"), std::string::npos);
        EXPECT_NE(json.find("\"double\": 1.25"), std::string::npos);
    }

    TEST(StartupTimelineValidation, ConcurrentRecordingRetainsEveryEvent)
    {
        RVX::Diagnostics::TraceSessionConfig config;
        config.enabled = true;
        const std::shared_ptr<RVX::Diagnostics::TraceSession> session =
            RVX::Diagnostics::TraceSession::Create(std::move(config));
        const RVX::Diagnostics::TraceContext context =
            session->CreateRootContext();

        constexpr size_t ThreadCount = 4;
        constexpr size_t EventsPerThread = 32;
        std::array<std::thread, ThreadCount> threads;
        for (std::thread& thread : threads)
        {
            thread = std::thread([session, context]() {
                for (size_t index = 0; index < EventsPerThread; ++index)
                {
                    session->RecordInstant("Concurrent.Instant",
                                           context,
                                           {{"index", static_cast<RVX::uint64>(index)}});
                }
            });
        }
        for (std::thread& thread : threads)
        {
            thread.join();
        }

        const std::vector<RVX::Diagnostics::TraceEvent> events =
            session->GetEvents();
        EXPECT_EQ(events.size(), ThreadCount * EventsPerThread);
        for (const RVX::Diagnostics::TraceEvent& event : events)
        {
            EXPECT_EQ(event.correlationId, context.correlationId);
            EXPECT_EQ(event.parentId, context.parentSpanId);
        }
    }
} // namespace
