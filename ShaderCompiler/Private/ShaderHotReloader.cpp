#include "ShaderCompiler/ShaderHotReloader.h"
#include "ShaderCompiler/ShaderCompileService.h"
#include "ShaderCompiler/ShaderCacheManager.h"
#include "RHI/RHIDevice.h"
#include "Core/Log.h"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <chrono>

namespace RVX
{
    namespace
    {
        uint64 GetCurrentTimeMs()
        {
            return static_cast<uint64>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()
                ).count()
            );
        }
    }

    ShaderHotReloader::ShaderHotReloader(
        ShaderCompileService* compileService,
        ShaderCacheManager* cacheManager,
        const Config& config)
        : m_config(config)
        , m_compileService(compileService)
        , m_cacheManager(cacheManager)
    {
        if (m_config.enabled)
        {
            Enable();
        }
    }

    ShaderHotReloader::~ShaderHotReloader()
    {
        Disable();
    }

    void ShaderHotReloader::Enable()
    {
        if (m_enabled.exchange(true))
        {
            return; // Already enabled
        }

        m_shutdown.store(false);

        // Scan initial file states
        for (const auto& dir : m_config.watchDirectories)
        {
            ScanDirectory(dir);
        }

        // Start watcher thread
        m_watcherThread = std::thread(&ShaderHotReloader::WatcherThread, this);

        RVX_CORE_INFO("ShaderHotReloader: Enabled, watching {} directories",
            m_config.watchDirectories.size());
    }

    void ShaderHotReloader::Disable()
    {
        if (!m_enabled.exchange(false))
        {
            return; // Already disabled
        }

        m_shutdown.store(true);

        if (m_watcherThread.joinable())
        {
            m_watcherThread.join();
        }

        RVX_CORE_INFO("ShaderHotReloader: Disabled");
    }

    void ShaderHotReloader::RegisterShader(
        IRHIDevice* device,
        const std::string& shaderPath,
        RHIShaderRef shader,
        const ShaderPermutationLoadDesc& loadDesc,
        ShaderReloadCallback callback)
    {
        std::lock_guard<std::mutex> lock(m_shadersMutex);

        auto it = m_watchedShaders.find(shaderPath);
        if (it != m_watchedShaders.end())
        {
            // Add to existing entry
            it->second.instances.push_back(shader);
            if (callback)
            {
                it->second.callbacks.push_back(callback);
            }
        }
        else
        {
            // Create new entry
            WatchedShader ws;
            ws.path = shaderPath;
            ws.loadDesc = loadDesc;
            ws.instances.push_back(shader);
            if (callback)
            {
                ws.callbacks.push_back(callback);
            }
            ws.device = device;
            ws.lastModifiedTime = GetFileModificationTime(shaderPath);

            m_watchedShaders[shaderPath] = std::move(ws);
        }

        m_stats.watchedShaderCount = static_cast<uint32>(m_watchedShaders.size());
    }

    void ShaderHotReloader::UnregisterShader(const std::string& shaderPath)
    {
        std::lock_guard<std::mutex> lock(m_shadersMutex);
        m_watchedShaders.erase(shaderPath);
        m_stats.watchedShaderCount = static_cast<uint32>(m_watchedShaders.size());
    }

    void ShaderHotReloader::UnregisterShaderInstance(RHIShaderRef shader)
    {
        std::lock_guard<std::mutex> lock(m_shadersMutex);

        for (auto& [path, ws] : m_watchedShaders)
        {
            auto it = std::find(ws.instances.begin(), ws.instances.end(), shader);
            if (it != ws.instances.end())
            {
                ws.instances.erase(it);
                break;
            }
        }
    }

    void ShaderHotReloader::Update()
    {
        if (!m_enabled.load())
        {
            return;
        }

        ProcessPendingChanges();
    }

    void ShaderHotReloader::ForceReload(const std::string& shaderPath)
    {
        std::lock_guard<std::mutex> lock(m_shadersMutex);

        auto it = m_watchedShaders.find(shaderPath);
        if (it != m_watchedShaders.end())
        {
            ReloadShader(it->second);
        }
    }

    void ShaderHotReloader::ForceReloadAll()
    {
        std::lock_guard<std::mutex> lock(m_shadersMutex);

        for (auto& [path, ws] : m_watchedShaders)
        {
            ReloadShader(ws);
        }
    }

    void ShaderHotReloader::AddWatchDirectory(const std::filesystem::path& dir)
    {
        m_config.watchDirectories.push_back(dir);
        ScanDirectory(dir);
    }

    void ShaderHotReloader::RemoveWatchDirectory(const std::filesystem::path& dir)
    {
        auto it = std::find(
            m_config.watchDirectories.begin(),
            m_config.watchDirectories.end(),
            dir);

        if (it != m_config.watchDirectories.end())
        {
            m_config.watchDirectories.erase(it);
        }
    }

    void ShaderHotReloader::ClearWatchDirectories()
    {
        m_config.watchDirectories.clear();
        std::lock_guard<std::mutex> lock(m_filesMutex);
        m_trackedFiles.clear();
    }

    void ShaderHotReloader::SetGlobalReloadCallback(ShaderReloadCallback callback)
    {
        m_globalCallback = std::move(callback);
    }

    void ShaderHotReloader::WatcherThread()
    {
        while (!m_shutdown.load())
        {
            // Sleep for poll interval
            std::this_thread::sleep_for(std::chrono::milliseconds(m_config.pollIntervalMs));

            if (m_shutdown.load())
            {
                break;
            }

            // Check for file changes
            std::vector<std::string> changedFiles;

            {
                std::lock_guard<std::mutex> lock(m_filesMutex);

                for (const auto& dir : m_config.watchDirectories)
                {
                    std::error_code ec;
                    if (!std::filesystem::exists(dir, ec))
                    {
                        continue;
                    }

                    for (const auto& entry : std::filesystem::recursive_directory_iterator(dir, ec))
                    {
                        if (!entry.is_regular_file())
                        {
                            continue;
                        }

                        if (!ShouldWatch(entry.path()))
                        {
                            continue;
                        }

                        std::string pathStr = entry.path().string();
                        uint64 writeTime = GetFileModificationTime(entry.path());
                        uint64 size = entry.file_size(ec);

                        auto it = m_trackedFiles.find(pathStr);
                        if (it != m_trackedFiles.end())
                        {
                            if (it->second.lastWriteTime != writeTime || it->second.size != size)
                            {
                                changedFiles.push_back(pathStr);
                                it->second.lastWriteTime = writeTime;
                                it->second.size = size;
                            }
                        }
                        else
                        {
                            // New file
                            m_trackedFiles[pathStr] = {writeTime, size};
                        }
                    }
                }
            }

            // Queue changed files for processing (with debounce)
            if (!changedFiles.empty())
            {
                uint64 now = GetCurrentTimeMs();
                std::lock_guard<std::mutex> lock(m_pendingMutex);

                for (const auto& file : changedFiles)
                {
                    m_pendingChanges[file] = {file, now};
                }
            }
        }
    }

    void ShaderHotReloader::ProcessPendingChanges()
    {
        std::vector<std::string> filesToProcess;
        uint64 now = GetCurrentTimeMs();

        {
            std::lock_guard<std::mutex> lock(m_pendingMutex);

            for (auto it = m_pendingChanges.begin(); it != m_pendingChanges.end();)
            {
                if (now - it->second.timestamp >= m_config.debounceMs)
                {
                    filesToProcess.push_back(it->second.path);
                    it = m_pendingChanges.erase(it);
                }
                else
                {
                    ++it;
                }
            }
        }

        if (filesToProcess.empty())
        {
            return;
        }

        // Find affected shaders
        std::unordered_set<std::string> affectedShaders;
        for (const auto& file : filesToProcess)
        {
            auto affected = GetAffectedShaders(file);
            for (const auto& shader : affected)
            {
                affectedShaders.insert(shader);
            }
        }

        // Reload affected shaders
        std::lock_guard<std::mutex> lock(m_shadersMutex);
        for (const auto& shaderPath : affectedShaders)
        {
            auto it = m_watchedShaders.find(shaderPath);
            if (it != m_watchedShaders.end())
            {
                RVX_CORE_INFO("ShaderHotReloader: Reloading shader: {}", shaderPath);
                ReloadShader(it->second);
            }
        }
    }

    void ShaderHotReloader::ReloadShader(WatchedShader& shader)
    {
        if (!m_compileService || !shader.device)
        {
            return;
        }

        // Load source
        std::ifstream file(shader.path);
        if (!file.is_open())
        {
            RVX_CORE_ERROR("ShaderHotReloader: Failed to open shader file: {}", shader.path);
            return;
        }

        std::stringstream buffer;
        buffer << file.rdbuf();
        std::string source = buffer.str();

        // Compile
        ShaderCompileOptions options;
        options.stage = shader.loadDesc.stage;
        options.entryPoint = shader.loadDesc.entryPoint.c_str();
        options.sourceCode = source.c_str();
        options.sourcePath = shader.path.c_str();
        options.targetProfile = shader.loadDesc.targetProfile.empty() ? nullptr : shader.loadDesc.targetProfile.c_str();
        options.targetBackend = shader.loadDesc.backend;
        options.enableDebugInfo = shader.loadDesc.enableDebugInfo;
        options.enableOptimization = shader.loadDesc.enableOptimization;

        ShaderCompileResult result = m_compileService->CompileSync(options);

        m_stats.reloadCount++;

        ShaderReloadInfo reloadInfo;
        reloadInfo.shaderPath = shader.path;
        reloadInfo.success = result.success;
        reloadInfo.errorMessage = result.errorMessage;

        if (result.success)
        {
            // Create new shader
            RHIShaderDesc shaderDesc;
            shaderDesc.stage = shader.loadDesc.stage;
            shaderDesc.entryPoint = shader.loadDesc.entryPoint.c_str();
            shaderDesc.bytecode = result.bytecode.data();
            shaderDesc.bytecodeSize = result.bytecode.size();
            shaderDesc.debugName = shader.path.c_str();

            RHIShaderRef newShader = shader.device->CreateShader(shaderDesc);
            if (newShader)
            {
                reloadInfo.newShader = newShader;

                // Update all instances
                for (auto& instance : shader.instances)
                {
                    reloadInfo.oldShader = instance;
                    instance = newShader;
                }

                shader.lastModifiedTime = GetFileModificationTime(shader.path);
                m_stats.successCount++;

                RVX_CORE_INFO("ShaderHotReloader: Successfully reloaded: {}", shader.path);
            }
            else
            {
                reloadInfo.success = false;
                reloadInfo.errorMessage = "Failed to create shader from compiled bytecode";
                m_stats.failureCount++;

                RVX_CORE_ERROR("ShaderHotReloader: Failed to create shader: {}", shader.path);
            }
        }
        else
        {
            m_stats.failureCount++;
            RVX_CORE_ERROR("ShaderHotReloader: Compilation failed for {}: {}",
                shader.path, result.errorMessage);
        }

        // Invalidate cache
        if (m_cacheManager)
        {
            // Would need the cache key here - simplified for now
        }

        // Call callbacks
        for (const auto& callback : shader.callbacks)
        {
            if (callback)
            {
                callback(reloadInfo);
            }
        }

        if (m_globalCallback)
        {
            m_globalCallback(reloadInfo);
        }
    }

    std::vector<std::string> ShaderHotReloader::GetAffectedShaders(const std::string& changedFile)
    {
        std::vector<std::string> affected;

        // Check if the changed file is a watched shader itself
        auto it = m_watchedShaders.find(changedFile);
        if (it != m_watchedShaders.end())
        {
            affected.push_back(changedFile);
        }

        // Check if it's a dependency of any watched shader
        for (const auto& [path, ws] : m_watchedShaders)
        {
            if (ws.dependencies.find(changedFile) != ws.dependencies.end())
            {
                affected.push_back(path);
            }
        }

        // Also check by filename match for includes
        std::filesystem::path changedPath(changedFile);
        std::string changedFilename = changedPath.filename().string();

        for (const auto& [path, ws] : m_watchedShaders)
        {
            for (const auto& dep : ws.dependencies)
            {
                std::filesystem::path depPath(dep);
                if (depPath.filename().string() == changedFilename)
                {
                    if (std::find(affected.begin(), affected.end(), path) == affected.end())
                    {
                        affected.push_back(path);
                    }
                }
            }
        }

        return affected;
    }

    uint64 ShaderHotReloader::GetFileModificationTime(const std::filesystem::path& path) const
    {
        std::error_code ec;
        auto ftime = std::filesystem::last_write_time(path, ec);
        if (ec)
        {
            return 0;
        }

        // Convert to milliseconds since epoch
        auto duration = ftime.time_since_epoch();
        return static_cast<uint64>(
            std::chrono::duration_cast<std::chrono::milliseconds>(duration).count()
        );
    }

    void ShaderHotReloader::ScanDirectory(const std::filesystem::path& dir)
    {
        std::error_code ec;
        if (!std::filesystem::exists(dir, ec))
        {
            return;
        }

        std::lock_guard<std::mutex> lock(m_filesMutex);

        for (const auto& entry : std::filesystem::recursive_directory_iterator(dir, ec))
        {
            if (!entry.is_regular_file())
            {
                continue;
            }

            if (!ShouldWatch(entry.path()))
            {
                continue;
            }

            std::string pathStr = entry.path().string();
            uint64 writeTime = GetFileModificationTime(entry.path());
            uint64 size = entry.file_size(ec);

            m_trackedFiles[pathStr] = {writeTime, size};
        }
    }

    bool ShaderHotReloader::ShouldWatch(const std::filesystem::path& path) const
    {
        std::string ext = path.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

        for (const auto& watchExt : m_config.watchExtensions)
        {
            if (ext == watchExt)
            {
                return true;
            }
        }

        return false;
    }

} // namespace RVX
