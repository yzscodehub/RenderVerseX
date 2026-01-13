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

        static std::shared_ptr<spdlog::logger>& GetCoreLogger() { return s_coreLogger; }
        static std::shared_ptr<spdlog::logger>& GetRHILogger() { return s_rhiLogger; }

    private:
        static std::shared_ptr<spdlog::logger> s_coreLogger;
        static std::shared_ptr<spdlog::logger> s_rhiLogger;
    };

} // namespace RVX

// =============================================================================
// Logging Macros
// =============================================================================
#define RVX_CORE_TRACE(...)    ::RVX::Log::GetCoreLogger()->trace(__VA_ARGS__)
#define RVX_CORE_DEBUG(...)    ::RVX::Log::GetCoreLogger()->debug(__VA_ARGS__)
#define RVX_CORE_INFO(...)     ::RVX::Log::GetCoreLogger()->info(__VA_ARGS__)
#define RVX_CORE_WARN(...)     ::RVX::Log::GetCoreLogger()->warn(__VA_ARGS__)
#define RVX_CORE_ERROR(...)    ::RVX::Log::GetCoreLogger()->error(__VA_ARGS__)
#define RVX_CORE_CRITICAL(...) ::RVX::Log::GetCoreLogger()->critical(__VA_ARGS__)

#define RVX_RHI_TRACE(...)     ::RVX::Log::GetRHILogger()->trace(__VA_ARGS__)
#define RVX_RHI_DEBUG(...)     ::RVX::Log::GetRHILogger()->debug(__VA_ARGS__)
#define RVX_RHI_INFO(...)      ::RVX::Log::GetRHILogger()->info(__VA_ARGS__)
#define RVX_RHI_WARN(...)      ::RVX::Log::GetRHILogger()->warn(__VA_ARGS__)
#define RVX_RHI_ERROR(...)     ::RVX::Log::GetRHILogger()->error(__VA_ARGS__)
#define RVX_RHI_CRITICAL(...)  ::RVX::Log::GetRHILogger()->critical(__VA_ARGS__)
