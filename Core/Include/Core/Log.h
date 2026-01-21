#pragma once

#include <spdlog/spdlog.h>
#include <spdlog/fmt/fmt.h>
#include <memory>

namespace RVX
{
    // =============================================================================
    // Logging System
    // =============================================================================
    class Log
    {
    public:
        static void Initialize();
        static void Shutdown();

        // Module loggers
        static std::shared_ptr<spdlog::logger>& GetCoreLogger()    { return s_coreLogger; }
        static std::shared_ptr<spdlog::logger>& GetRHILogger()     { return s_rhiLogger; }
        static std::shared_ptr<spdlog::logger>& GetSceneLogger()   { return s_sceneLogger; }
        static std::shared_ptr<spdlog::logger>& GetAssetLogger()   { return s_assetLogger; }
        static std::shared_ptr<spdlog::logger>& GetSpatialLogger() { return s_spatialLogger; }
        static std::shared_ptr<spdlog::logger>& GetRenderLogger()  { return s_renderLogger; }

    private:
        static std::shared_ptr<spdlog::logger> s_coreLogger;
        static std::shared_ptr<spdlog::logger> s_rhiLogger;
        static std::shared_ptr<spdlog::logger> s_sceneLogger;
        static std::shared_ptr<spdlog::logger> s_assetLogger;
        static std::shared_ptr<spdlog::logger> s_spatialLogger;
        static std::shared_ptr<spdlog::logger> s_renderLogger;
    };

} // namespace RVX

// =============================================================================
// Core Module Macros
// =============================================================================
#define RVX_CORE_TRACE(...)    ::RVX::Log::GetCoreLogger()->trace(__VA_ARGS__)
#define RVX_CORE_DEBUG(...)    ::RVX::Log::GetCoreLogger()->debug(__VA_ARGS__)
#define RVX_CORE_INFO(...)     ::RVX::Log::GetCoreLogger()->info(__VA_ARGS__)
#define RVX_CORE_WARN(...)     ::RVX::Log::GetCoreLogger()->warn(__VA_ARGS__)
#define RVX_CORE_ERROR(...)    ::RVX::Log::GetCoreLogger()->error(__VA_ARGS__)
#define RVX_CORE_CRITICAL(...) ::RVX::Log::GetCoreLogger()->critical(__VA_ARGS__)

// =============================================================================
// RHI Module Macros
// =============================================================================
#define RVX_RHI_TRACE(...)     ::RVX::Log::GetRHILogger()->trace(__VA_ARGS__)
#define RVX_RHI_DEBUG(...)     ::RVX::Log::GetRHILogger()->debug(__VA_ARGS__)
#define RVX_RHI_INFO(...)      ::RVX::Log::GetRHILogger()->info(__VA_ARGS__)
#define RVX_RHI_WARN(...)      ::RVX::Log::GetRHILogger()->warn(__VA_ARGS__)
#define RVX_RHI_ERROR(...)     ::RVX::Log::GetRHILogger()->error(__VA_ARGS__)
#define RVX_RHI_CRITICAL(...)  ::RVX::Log::GetRHILogger()->critical(__VA_ARGS__)

// =============================================================================
// Scene Module Macros
// =============================================================================
#define RVX_SCENE_TRACE(...)    ::RVX::Log::GetSceneLogger()->trace(__VA_ARGS__)
#define RVX_SCENE_DEBUG(...)    ::RVX::Log::GetSceneLogger()->debug(__VA_ARGS__)
#define RVX_SCENE_INFO(...)     ::RVX::Log::GetSceneLogger()->info(__VA_ARGS__)
#define RVX_SCENE_WARN(...)     ::RVX::Log::GetSceneLogger()->warn(__VA_ARGS__)
#define RVX_SCENE_ERROR(...)    ::RVX::Log::GetSceneLogger()->error(__VA_ARGS__)
#define RVX_SCENE_CRITICAL(...) ::RVX::Log::GetSceneLogger()->critical(__VA_ARGS__)

// =============================================================================
// Asset Module Macros
// =============================================================================
#define RVX_ASSET_TRACE(...)    ::RVX::Log::GetAssetLogger()->trace(__VA_ARGS__)
#define RVX_ASSET_DEBUG(...)    ::RVX::Log::GetAssetLogger()->debug(__VA_ARGS__)
#define RVX_ASSET_INFO(...)     ::RVX::Log::GetAssetLogger()->info(__VA_ARGS__)
#define RVX_ASSET_WARN(...)     ::RVX::Log::GetAssetLogger()->warn(__VA_ARGS__)
#define RVX_ASSET_ERROR(...)    ::RVX::Log::GetAssetLogger()->error(__VA_ARGS__)
#define RVX_ASSET_CRITICAL(...) ::RVX::Log::GetAssetLogger()->critical(__VA_ARGS__)

// =============================================================================
// Spatial Module Macros
// =============================================================================
#define RVX_SPATIAL_TRACE(...)    ::RVX::Log::GetSpatialLogger()->trace(__VA_ARGS__)
#define RVX_SPATIAL_DEBUG(...)    ::RVX::Log::GetSpatialLogger()->debug(__VA_ARGS__)
#define RVX_SPATIAL_INFO(...)     ::RVX::Log::GetSpatialLogger()->info(__VA_ARGS__)
#define RVX_SPATIAL_WARN(...)     ::RVX::Log::GetSpatialLogger()->warn(__VA_ARGS__)
#define RVX_SPATIAL_ERROR(...)    ::RVX::Log::GetSpatialLogger()->error(__VA_ARGS__)
#define RVX_SPATIAL_CRITICAL(...) ::RVX::Log::GetSpatialLogger()->critical(__VA_ARGS__)

// =============================================================================
// Render Module Macros
// =============================================================================
#define RVX_RENDER_TRACE(...)    ::RVX::Log::GetRenderLogger()->trace(__VA_ARGS__)
#define RVX_RENDER_DEBUG(...)    ::RVX::Log::GetRenderLogger()->debug(__VA_ARGS__)
#define RVX_RENDER_INFO(...)     ::RVX::Log::GetRenderLogger()->info(__VA_ARGS__)
#define RVX_RENDER_WARN(...)     ::RVX::Log::GetRenderLogger()->warn(__VA_ARGS__)
#define RVX_RENDER_ERROR(...)    ::RVX::Log::GetRenderLogger()->error(__VA_ARGS__)
#define RVX_RENDER_CRITICAL(...) ::RVX::Log::GetRenderLogger()->critical(__VA_ARGS__)

// =============================================================================
// Convenience Macros (default to Core logger)
// =============================================================================
#define LOG_TRACE(...) ::RVX::Log::GetCoreLogger()->trace(__VA_ARGS__)
#define LOG_DEBUG(...) ::RVX::Log::GetCoreLogger()->debug(__VA_ARGS__)
#define LOG_INFO(...)  ::RVX::Log::GetCoreLogger()->info(__VA_ARGS__)
#define LOG_WARN(...)  ::RVX::Log::GetCoreLogger()->warn(__VA_ARGS__)
#define LOG_ERROR(...) ::RVX::Log::GetCoreLogger()->error(__VA_ARGS__)
