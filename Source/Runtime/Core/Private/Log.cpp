#include "Core/Log.h"
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/rotating_file_sink.h>
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
            // Flush on warning and above for performance
            logger->flush_on(spdlog::level::warn);
            return logger;
        }
    }

    void Log::Initialize()
    {
        Initialize(Config{});
    }

    void Log::Initialize(const Config& config)
    {
        // Create sinks
        std::vector<spdlog::sink_ptr> sinks;
        sinks.emplace_back(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());

        // File output - rotating or basic based on config
        if (config.enableRotation)
        {
            sinks.emplace_back(std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
                config.logFileName,
                config.maxFileSizeMB * 1024 * 1024,  // MB to bytes
                config.maxFiles));
        }
        else
        {
            sinks.emplace_back(std::make_shared<spdlog::sinks::basic_file_sink_mt>(
                config.logFileName, true));
        }

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

        RVX_CORE_INFO("RenderVerseX Logger Initialized (rotation: {}, max size: {}MB, max files: {})",
                      config.enableRotation ? "enabled" : "disabled",
                      config.maxFileSizeMB,
                      config.maxFiles);
    }

    void Log::Shutdown()
    {
        RVX_CORE_INFO("RenderVerseX Logger Shutdown");
        spdlog::shutdown();
    }

    void Log::SetLevel(spdlog::level::level_enum level)
    {
        std::array<std::shared_ptr<spdlog::logger>, 6> loggers = {
            s_coreLogger, s_rhiLogger, s_sceneLogger,
            s_assetLogger, s_spatialLogger, s_renderLogger
        };

        for (auto& logger : loggers)
        {
            if (logger)
            {
                logger->set_level(level);
            }
        }
    }

    void Log::SetModuleLevel(const std::string& module, spdlog::level::level_enum level)
    {
        std::shared_ptr<spdlog::logger> logger = nullptr;

        if (module == "CORE")    logger = s_coreLogger;
        else if (module == "RHI")     logger = s_rhiLogger;
        else if (module == "SCENE")   logger = s_sceneLogger;
        else if (module == "ASSET")   logger = s_assetLogger;
        else if (module == "SPATIAL") logger = s_spatialLogger;
        else if (module == "RENDER")  logger = s_renderLogger;

        if (logger)
        {
            logger->set_level(level);
        }
    }

    spdlog::level::level_enum Log::GetModuleLevel(const std::string& module)
    {
        std::shared_ptr<spdlog::logger> logger = nullptr;

        if (module == "CORE")    logger = s_coreLogger;
        else if (module == "RHI")     logger = s_rhiLogger;
        else if (module == "SCENE")   logger = s_sceneLogger;
        else if (module == "ASSET")   logger = s_assetLogger;
        else if (module == "SPATIAL") logger = s_spatialLogger;
        else if (module == "RENDER")  logger = s_renderLogger;

        return logger ? logger->level() : spdlog::level::info;
    }

} // namespace RVX
