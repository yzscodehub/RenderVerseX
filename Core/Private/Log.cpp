#include "Core/Log.h"
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <vector>

namespace RVX
{
    std::shared_ptr<spdlog::logger> Log::s_coreLogger;
    std::shared_ptr<spdlog::logger> Log::s_rhiLogger;

    void Log::Initialize()
    {
        // Create sinks
        std::vector<spdlog::sink_ptr> sinks;
        sinks.emplace_back(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());
        sinks.emplace_back(std::make_shared<spdlog::sinks::basic_file_sink_mt>("RenderVerseX.log", true));

        // Set pattern: [timestamp] [logger] [level] message
        sinks[0]->set_pattern("%^[%T] [%n] [%l]%$ %v");
        sinks[1]->set_pattern("[%T] [%n] [%l] %v");

        // Create loggers
        s_coreLogger = std::make_shared<spdlog::logger>("CORE", sinks.begin(), sinks.end());
        spdlog::register_logger(s_coreLogger);
        s_coreLogger->set_level(spdlog::level::trace);
        s_coreLogger->flush_on(spdlog::level::trace);

        s_rhiLogger = std::make_shared<spdlog::logger>("RHI", sinks.begin(), sinks.end());
        spdlog::register_logger(s_rhiLogger);
        s_rhiLogger->set_level(spdlog::level::trace);
        s_rhiLogger->flush_on(spdlog::level::trace);

        RVX_CORE_INFO("RenderVerseX Logger Initialized");
    }

    void Log::Shutdown()
    {
        RVX_CORE_INFO("RenderVerseX Logger Shutdown");
        spdlog::shutdown();
    }

} // namespace RVX
