#pragma once

#include "ShaderCompiler/ShaderCompiler.h"
#include "ShaderCompiler/ShaderTypes.h"
#include <memory>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>
#include <chrono>

namespace RVX
{
    // =========================================================================
    // Compile Request
    // =========================================================================
    struct CompileRequest
    {
        ShaderCompileOptions options;
        CompileCallback callback;
        CompilePriority priority = CompilePriority::Normal;
        CompileHandle handle = RVX_INVALID_COMPILE_HANDLE;
    };

    // =========================================================================
    // Compile Task State
    // =========================================================================
    struct CompileTask
    {
        CompileHandle handle = RVX_INVALID_COMPILE_HANDLE;
        CompileStatus status = CompileStatus::Pending;
        ShaderCompileResult result;
        CompileCallback callback;
        std::chrono::steady_clock::time_point submitTime;
        std::chrono::steady_clock::time_point completeTime;
    };

    // =========================================================================
    // Shader Compile Service - Async compilation service
    // =========================================================================
    class ShaderCompileService
    {
    public:
        // =====================================================================
        // Configuration
        // =====================================================================
        struct Config
        {
            uint32 maxConcurrentCompiles = 4;
            bool enableStatistics = true;
        };

        // =====================================================================
        // Construction
        // =====================================================================
        explicit ShaderCompileService(const Config& config = {});
        ~ShaderCompileService();

        // Non-copyable
        ShaderCompileService(const ShaderCompileService&) = delete;
        ShaderCompileService& operator=(const ShaderCompileService&) = delete;

        // =====================================================================
        // Compilation Interface
        // =====================================================================

        /** @brief Synchronous compilation (blocks current thread) */
        ShaderCompileResult CompileSync(const ShaderCompileOptions& options);

        /** @brief Asynchronous compilation (returns immediately, executes in background) */
        CompileHandle CompileAsync(
            const ShaderCompileOptions& options,
            CompileCallback onComplete = nullptr,
            CompilePriority priority = CompilePriority::Normal);

        /** @brief Batch asynchronous compilation */
        std::vector<CompileHandle> CompileBatch(
            const std::vector<ShaderCompileOptions>& batch,
            CompilePriority priority = CompilePriority::Normal);

        // =====================================================================
        // Task Management
        // =====================================================================

        /** @brief Wait for compilation to complete and get result */
        ShaderCompileResult Wait(CompileHandle handle);

        /** @brief Wait for multiple compilations to complete */
        std::vector<ShaderCompileResult> WaitAll(const std::vector<CompileHandle>& handles);

        /** @brief Check if compilation is complete */
        bool IsComplete(CompileHandle handle) const;

        /** @brief Get compilation status */
        CompileStatus GetStatus(CompileHandle handle) const;

        /** @brief Cancel a pending compilation task */
        bool Cancel(CompileHandle handle);

        /** @brief Cancel all pending tasks */
        void CancelAll();

        /** @brief Wait for all tasks to complete */
        void Flush();

        // =====================================================================
        // Configuration
        // =====================================================================

        /** @brief Set maximum concurrent compilations */
        void SetMaxConcurrentCompiles(uint32 count);

        /** @brief Get number of pending tasks */
        uint32 GetPendingCount() const;

        /** @brief Get number of currently active compilations */
        uint32 GetActiveCount() const;

        // =====================================================================
        // Statistics
        // =====================================================================
        struct Statistics
        {
            uint64 totalCompiles = 0;
            uint64 totalCompileTimeMs = 0;
            uint64 averageCompileTimeMs = 0;
            uint32 successCount = 0;
            uint32 failureCount = 0;
            uint32 cancelledCount = 0;

            void Reset()
            {
                totalCompiles = 0;
                totalCompileTimeMs = 0;
                averageCompileTimeMs = 0;
                successCount = 0;
                failureCount = 0;
                cancelledCount = 0;
            }
        };

        const Statistics& GetStatistics() const { return m_stats; }
        void ResetStatistics() { m_stats.Reset(); }

    private:
        // =====================================================================
        // Internal Implementation
        // =====================================================================
        CompileHandle GenerateHandle();
        void WorkerThread();
        void UpdateStatistics(const ShaderCompileResult& result, uint64 durationMs);

        Config m_config;
        std::unique_ptr<IShaderCompiler> m_compiler;

        // Task queue
        mutable std::mutex m_queueMutex;
        std::condition_variable m_queueCV;
        std::vector<CompileRequest> m_pendingQueue;

        // Active tasks
        mutable std::mutex m_tasksMutex;
        std::condition_variable m_tasksCV;
        std::unordered_map<CompileHandle, CompileTask> m_tasks;

        // Worker threads
        std::vector<std::thread> m_workers;
        std::atomic<bool> m_shutdown{false};
        std::atomic<uint32> m_activeCount{0};

        // Handle generation
        std::atomic<uint64> m_nextHandle{1};

        // Statistics
        mutable std::mutex m_statsMutex;
        Statistics m_stats;
    };

} // namespace RVX
