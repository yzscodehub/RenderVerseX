#include "Core/Log.h"
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <vector>

namespace RVX
{
    std::shared_ptr<spdlog::logger> Log::s_coreLogger;
    std::shared_ptr<spdlog::logger> Log::s_rhiLogger;
    std::shared_ptr<spdlog::logger> Log::s_sceneLogger;
    std::shared_ptr<spdlog::logger> Log::s_assetLogger;
    std::shared_ptr<spdlog::logger> Log::s_spatialLogger;
    std::shared_ptr<spdlog::logger> Log::s_renderLogger;

    namespace
    {
        std::shared_ptr<spdlog::logger> CreateLogger(
            const std::string& name, 
            const std::vector<spdlog::sink_ptr>& sinks)
        {
            auto logger = std::make_shared<spdlog::logger>(name, sinks.begin(), sinks.end());
            spdlog::register_logger(logger);
            logger->set_level(spdlog::level::trace);
            logger->flush_on(spdlog::level::trace);
            return logger;
        }
    }

    void Log::Initialize()
    {
        // Create sinks
        std::vector<spdlog::sink_ptr> sinks;
        sinks.emplace_back(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());
        sinks.emplace_back(std::make_shared<spdlog::sinks::basic_file_sink_mt>("RenderVerseX.log", true));

        // Set pattern: [timestamp] [logger] [level] message
        sinks[0]->set_pattern("%^[%T] [%n] [%l]%$ %v");
        sinks[1]->set_pattern("[%T] [%n] [%l] %v");

        // Create module loggers
        s_coreLogger    = CreateLogger("CORE",    sinks);
        s_rhiLogger     = CreateLogger("RHI",     sinks);
        s_sceneLogger   = CreateLogger("SCENE",   sinks);
        s_assetLogger   = CreateLogger("ASSET",   sinks);
        s_spatialLogger = CreateLogger("SPATIAL", sinks);
        s_renderLogger  = CreateLogger("RENDER",  sinks);

        RVX_CORE_INFO("RenderVerseX Logger Initialized");
    }

    void Log::Shutdown()
    {
        RVX_CORE_INFO("RenderVerseX Logger Shutdown");
        spdlog::shutdown();
    }

} // namespace RVX
