#include "ShaderCompiler/ShaderCompileService.h"
#include "Core/Log.h"
#include <algorithm>

namespace RVX
{
    ShaderCompileService::ShaderCompileService(const Config& config)
        : m_config(config)
        , m_compiler(CreateShaderCompiler())
    {
        // Start worker threads
        uint32 workerCount = std::max(1u, config.maxConcurrentCompiles);
        m_workers.reserve(workerCount);
        for (uint32 i = 0; i < workerCount; ++i)
        {
            m_workers.emplace_back(&ShaderCompileService::WorkerThread, this);
        }

        RVX_CORE_INFO("ShaderCompileService: Started with {} worker threads", workerCount);
    }

    ShaderCompileService::~ShaderCompileService()
    {
        // Signal shutdown
        m_shutdown.store(true);
        m_queueCV.notify_all();

        // Wait for all workers to finish
        for (auto& worker : m_workers)
        {
            if (worker.joinable())
            {
                worker.join();
            }
        }

        RVX_CORE_INFO("ShaderCompileService: Shutdown complete");
    }

    ShaderCompileResult ShaderCompileService::CompileSync(const ShaderCompileOptions& options)
    {
        if (!m_compiler)
        {
            ShaderCompileResult result;
            result.errorMessage = "ShaderCompileService: Compiler not initialized";
            return result;
        }

        // Compile directly on current thread
        auto start = std::chrono::steady_clock::now();
        ShaderCompileResult result = m_compiler->Compile(options);
        auto end = std::chrono::steady_clock::now();

        // Update statistics
        if (m_config.enableStatistics)
        {
            auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
            UpdateStatistics(result, duration.count());
        }

        return result;
    }

    CompileHandle ShaderCompileService::CompileAsync(
        const ShaderCompileOptions& options,
        CompileCallback onComplete,
        CompilePriority priority)
    {
        CompileHandle handle = GenerateHandle();

        // Create task entry
        {
            std::lock_guard<std::mutex> lock(m_tasksMutex);
            CompileTask task;
            task.handle = handle;
            task.status = CompileStatus::Pending;
            task.callback = onComplete;
            task.submitTime = std::chrono::steady_clock::now();
            m_tasks.emplace(handle, std::move(task));
        }

        // Add to queue
        {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            CompileRequest request;
            request.options = options;
            request.callback = onComplete;
            request.priority = priority;
            request.handle = handle;

            // Insert by priority (higher priority first)
            auto it = std::lower_bound(
                m_pendingQueue.begin(),
                m_pendingQueue.end(),
                request,
                [](const CompileRequest& a, const CompileRequest& b)
                {
                    return static_cast<uint8>(a.priority) > static_cast<uint8>(b.priority);
                });
            m_pendingQueue.insert(it, std::move(request));
        }

        m_queueCV.notify_one();
        return handle;
    }

    std::vector<CompileHandle> ShaderCompileService::CompileBatch(
        const std::vector<ShaderCompileOptions>& batch,
        CompilePriority priority)
    {
        std::vector<CompileHandle> handles;
        handles.reserve(batch.size());

        for (const auto& options : batch)
        {
            handles.push_back(CompileAsync(options, nullptr, priority));
        }

        return handles;
    }

    ShaderCompileResult ShaderCompileService::Wait(CompileHandle handle)
    {
        std::unique_lock<std::mutex> lock(m_tasksMutex);

        while (true)
        {
            auto it = m_tasks.find(handle);
            if (it == m_tasks.end())
            {
                // Task doesn't exist
                ShaderCompileResult result;
                result.errorMessage = "Invalid compile handle";
                return result;
            }

            if (it->second.status == CompileStatus::Completed ||
                it->second.status == CompileStatus::Failed ||
                it->second.status == CompileStatus::Cancelled)
            {
                ShaderCompileResult result = std::move(it->second.result);
                // Optionally remove completed task
                // m_tasks.erase(it);
                return result;
            }

            // Wait for task completion notification
            m_tasksCV.wait(lock);
        }
    }

    std::vector<ShaderCompileResult> ShaderCompileService::WaitAll(
        const std::vector<CompileHandle>& handles)
    {
        std::vector<ShaderCompileResult> results;
        results.reserve(handles.size());

        for (const auto& handle : handles)
        {
            results.push_back(Wait(handle));
        }

        return results;
    }

    bool ShaderCompileService::IsComplete(CompileHandle handle) const
    {
        std::lock_guard<std::mutex> lock(m_tasksMutex);
        auto it = m_tasks.find(handle);
        if (it != m_tasks.end())
        {
            return it->second.status == CompileStatus::Completed ||
                   it->second.status == CompileStatus::Failed ||
                   it->second.status == CompileStatus::Cancelled;
        }
        return true; // Non-existent task is considered complete
    }

    CompileStatus ShaderCompileService::GetStatus(CompileHandle handle) const
    {
        std::lock_guard<std::mutex> lock(m_tasksMutex);
        auto it = m_tasks.find(handle);
        if (it != m_tasks.end())
        {
            return it->second.status;
        }
        return CompileStatus::Completed; // Non-existent task
    }

    bool ShaderCompileService::Cancel(CompileHandle handle)
    {
        // Try to remove from pending queue
        {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            auto it = std::find_if(
                m_pendingQueue.begin(),
                m_pendingQueue.end(),
                [handle](const CompileRequest& req) { return req.handle == handle; });

            if (it != m_pendingQueue.end())
            {
                m_pendingQueue.erase(it);

                // Update task status
                std::lock_guard<std::mutex> taskLock(m_tasksMutex);
                auto taskIt = m_tasks.find(handle);
                if (taskIt != m_tasks.end())
                {
                    taskIt->second.status = CompileStatus::Cancelled;
                    taskIt->second.completeTime = std::chrono::steady_clock::now();

                    if (m_config.enableStatistics)
                    {
                        std::lock_guard<std::mutex> statsLock(m_statsMutex);
                        m_stats.cancelledCount++;
                    }
                }

                m_tasksCV.notify_all();
                return true;
            }
        }

        return false; // Already compiling or completed
    }

    void ShaderCompileService::CancelAll()
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);

        for (const auto& req : m_pendingQueue)
        {
            std::lock_guard<std::mutex> taskLock(m_tasksMutex);
            auto it = m_tasks.find(req.handle);
            if (it != m_tasks.end())
            {
                it->second.status = CompileStatus::Cancelled;
                it->second.completeTime = std::chrono::steady_clock::now();
            }
        }

        if (m_config.enableStatistics)
        {
            std::lock_guard<std::mutex> statsLock(m_statsMutex);
            m_stats.cancelledCount += static_cast<uint32>(m_pendingQueue.size());
        }

        m_pendingQueue.clear();
        m_tasksCV.notify_all();
    }

    void ShaderCompileService::Flush()
    {
        std::unique_lock<std::mutex> lock(m_tasksMutex);

        while (true)
        {
            bool allComplete = true;
            for (const auto& [handle, task] : m_tasks)
            {
                if (task.status == CompileStatus::Pending ||
                    task.status == CompileStatus::Compiling)
                {
                    allComplete = false;
                    break;
                }
            }

            if (allComplete)
            {
                break;
            }

            m_tasksCV.wait(lock);
        }
    }

    void ShaderCompileService::SetMaxConcurrentCompiles(uint32 count)
    {
        // Note: Dynamic thread pool resizing is complex
        // For now, just update the config for future reference
        m_config.maxConcurrentCompiles = count;
        RVX_CORE_WARN("ShaderCompileService: SetMaxConcurrentCompiles requires restart to take effect");
    }

    uint32 ShaderCompileService::GetPendingCount() const
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        return static_cast<uint32>(m_pendingQueue.size());
    }

    uint32 ShaderCompileService::GetActiveCount() const
    {
        return m_activeCount.load();
    }

    CompileHandle ShaderCompileService::GenerateHandle()
    {
        return m_nextHandle.fetch_add(1);
    }

    void ShaderCompileService::WorkerThread()
    {
        while (!m_shutdown.load())
        {
            CompileRequest request;

            // Get task from queue
            {
                std::unique_lock<std::mutex> lock(m_queueMutex);
                m_queueCV.wait(lock, [this]
                {
                    return m_shutdown.load() || !m_pendingQueue.empty();
                });

                if (m_shutdown.load() && m_pendingQueue.empty())
                {
                    break;
                }

                if (m_pendingQueue.empty())
                {
                    continue;
                }

                // Get highest priority task (at the front due to sorting)
                request = std::move(m_pendingQueue.front());
                m_pendingQueue.erase(m_pendingQueue.begin());
            }

            m_activeCount++;

            // Update status to compiling
            {
                std::lock_guard<std::mutex> lock(m_tasksMutex);
                auto it = m_tasks.find(request.handle);
                if (it != m_tasks.end())
                {
                    it->second.status = CompileStatus::Compiling;
                }
            }

            // Execute compilation
            auto start = std::chrono::steady_clock::now();
            ShaderCompileResult result;
            
            if (m_compiler)
            {
                result = m_compiler->Compile(request.options);
            }
            else
            {
                result.errorMessage = "Compiler not available";
            }

            auto end = std::chrono::steady_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

            // Update task state
            {
                std::lock_guard<std::mutex> lock(m_tasksMutex);
                auto it = m_tasks.find(request.handle);
                if (it != m_tasks.end())
                {
                    it->second.result = std::move(result);
                    it->second.status = it->second.result.success ?
                        CompileStatus::Completed : CompileStatus::Failed;
                    it->second.completeTime = end;
                }
            }

            // Execute callback
            if (request.callback)
            {
                std::lock_guard<std::mutex> lock(m_tasksMutex);
                auto it = m_tasks.find(request.handle);
                if (it != m_tasks.end())
                {
                    request.callback(it->second.result);
                }
            }

            // Update statistics
            if (m_config.enableStatistics)
            {
                std::lock_guard<std::mutex> lock(m_tasksMutex);
                auto it = m_tasks.find(request.handle);
                if (it != m_tasks.end())
                {
                    UpdateStatistics(it->second.result, duration.count());
                }
            }

            m_activeCount--;
            m_tasksCV.notify_all();
        }
    }

    void ShaderCompileService::UpdateStatistics(const ShaderCompileResult& result, uint64 durationMs)
    {
        std::lock_guard<std::mutex> lock(m_statsMutex);
        m_stats.totalCompiles++;
        m_stats.totalCompileTimeMs += durationMs;
        m_stats.averageCompileTimeMs = m_stats.totalCompileTimeMs / m_stats.totalCompiles;

        if (result.success)
        {
            m_stats.successCount++;
        }
        else
        {
            m_stats.failureCount++;
        }
    }

} // namespace RVX
