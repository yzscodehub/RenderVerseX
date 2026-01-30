#include "ShaderCompiler/ShaderManager.h"
#include "ShaderCompiler/ShaderReflection.h"
#include "Core/Core.h"
#include "RHI/RHIDevice.h"
#include <filesystem>
#include <fstream>
#include <sstream>

namespace RVX
{
    // =========================================================================
    // Construction
    // =========================================================================

    ShaderManager::ShaderManager(const ShaderManagerConfig& config)
    {
        Initialize(config);
    }

    ShaderManager::ShaderManager(std::unique_ptr<IShaderCompiler> /* compiler */)
    {
        // Legacy constructor - create with default config
        ShaderManagerConfig config;
        config.cacheDirectory = std::filesystem::current_path() / "ShaderCache";
        Initialize(config);
    }

    ShaderManager::~ShaderManager()
    {
        // Services are cleaned up automatically via unique_ptr
    }

    void ShaderManager::Initialize(const ShaderManagerConfig& config)
    {
        m_config = config;

        // Initialize cache directory
        if (m_config.cacheDirectory.empty())
        {
            m_config.cacheDirectory = std::filesystem::current_path() / "ShaderCache";
        }

        // Create compile service
        ShaderCompileService::Config compileConfig;
        compileConfig.maxConcurrentCompiles = m_config.maxConcurrentCompiles;
        compileConfig.enableStatistics = m_config.enableStatistics;
        m_compileService = std::make_unique<ShaderCompileService>(compileConfig);

        // Create cache manager
        ShaderCacheManager::Config cacheConfig;
        cacheConfig.cacheDirectory = m_config.cacheDirectory;
        cacheConfig.maxCacheSizeBytes = m_config.maxCacheSizeBytes;
        cacheConfig.enableMemoryCache = m_config.enableMemoryCache;
        cacheConfig.enableDiskCache = m_config.enableDiskCache;
        cacheConfig.validateOnLoad = true;
        m_cacheManager = std::make_unique<ShaderCacheManager>(cacheConfig);

        // Create permutation system
        m_permutationSystem = std::make_unique<ShaderPermutationSystem>(
            m_compileService.get(),
            m_cacheManager.get());

        // Create hot reloader
        ShaderHotReloader::Config hotReloadConfig;
        hotReloadConfig.watchDirectories = m_config.shaderDirectories;
        hotReloadConfig.enabled = false; // Starts disabled
        m_hotReloader = std::make_unique<ShaderHotReloader>(
            m_compileService.get(),
            m_cacheManager.get(),
            hotReloadConfig);

        if (m_config.enableHotReload)
        {
            m_hotReloader->Enable();
        }

        RVX_CORE_INFO("ShaderManager: Initialized with cache at {}", m_config.cacheDirectory.string());
    }

    // =========================================================================
    // Synchronous Loading
    // =========================================================================

    ShaderLoadResult ShaderManager::LoadFromFile(IRHIDevice* device, const ShaderLoadDesc& desc)
    {
        ShaderLoadResult result;
        std::string source;
        if (!LoadFile(desc.path, source))
        {
            result.compileResult.errorMessage = "Failed to load shader file: " + desc.path;
            return result;
        }

        return LoadFromSource(device, desc, source);
    }

    ShaderLoadResult ShaderManager::LoadFromSource(
        IRHIDevice* device,
        const ShaderLoadDesc& desc,
        const std::string& source)
    {
        ShaderLoadResult result;
        if (!device || !m_compileService)
        {
            result.compileResult.errorMessage = "ShaderManager not initialized";
            return result;
        }

        uint64 sourceHash = std::hash<std::string>{}(source);
        uint64 key = BuildCacheKey(desc, sourceHash);

        // Check legacy cache first (for backward compatibility)
        {
            std::lock_guard<std::mutex> lock(m_legacyCacheMutex);
            auto it = m_legacyCache.find(key);
            if (it != m_legacyCache.end())
            {
                result.shader = it->second;
                result.compileResult.success = true;
                return result;
            }
        }

        // Try new cache manager
        if (m_cacheManager)
        {
            auto cached = m_cacheManager->Load(key);
            if (cached)
            {
                RHIShaderDesc shaderDesc;
                shaderDesc.stage = desc.stage;
                shaderDesc.entryPoint = desc.entryPoint.c_str();
                shaderDesc.debugName = desc.path.empty() ? "Shader" : desc.path.c_str();

                // Handle backend-specific bytecode
                if (desc.backend == RHIBackendType::OpenGL && !cached->glslSource.empty())
                {
                    shaderDesc.bytecode = reinterpret_cast<const uint8*>(cached->glslSource.data());
                    shaderDesc.bytecodeSize = cached->glslSource.size();
                }
                else if (desc.backend == RHIBackendType::Metal && !cached->mslSource.empty())
                {
                    shaderDesc.bytecode = reinterpret_cast<const uint8*>(cached->mslSource.data());
                    shaderDesc.bytecodeSize = cached->mslSource.size();
                }
                else
                {
                    shaderDesc.bytecode = cached->bytecode.data();
                    shaderDesc.bytecodeSize = cached->bytecode.size();
                }

                result.shader = device->CreateShader(shaderDesc);
                if (result.shader)
                {
                    result.compileResult.success = true;
                    result.compileResult.bytecode = std::move(cached->bytecode);
                    result.compileResult.reflection = std::move(cached->reflection);
                    result.compileResult.glslSource = std::move(cached->glslSource);
                    result.compileResult.mslSource = std::move(cached->mslSource);

                    std::lock_guard<std::mutex> lock(m_legacyCacheMutex);
                    m_legacyCache.emplace(key, result.shader);

                    // Register for hot reload if enabled
                    if (m_hotReloader && m_hotReloader->IsEnabled() && !desc.path.empty())
                    {
                        m_hotReloader->RegisterShader(
                            device,
                            desc.path,
                            result.shader,
                            ConvertToPermutationDesc(desc));
                    }

                    return result;
                }
            }
        }

        // Compile
        ShaderCompileOptions options;
        options.stage = desc.stage;
        options.entryPoint = desc.entryPoint.c_str();
        options.sourceCode = source.c_str();
        options.sourcePath = desc.path.empty() ? nullptr : desc.path.c_str();
        options.targetProfile = desc.targetProfile.empty() ? nullptr : desc.targetProfile.c_str();
        options.defines = desc.defines;
        options.targetBackend = desc.backend;
        options.enableDebugInfo = desc.enableDebugInfo;
        options.enableOptimization = desc.enableOptimization;

        result.compileResult = m_compileService->CompileSync(options);
        if (!result.compileResult.success)
        {
            return result;
        }

        // Extract reflection if not provided
        if (result.compileResult.reflection.resources.empty() &&
            result.compileResult.reflection.inputs.empty() &&
            result.compileResult.reflection.pushConstants.empty())
        {
            result.compileResult.reflection = ReflectShader(
                desc.backend, desc.stage, result.compileResult.bytecode);
        }

        // Create shader
        RHIShaderDesc shaderDesc;
        shaderDesc.stage = desc.stage;
        shaderDesc.entryPoint = desc.entryPoint.c_str();
        shaderDesc.debugName = desc.path.empty() ? "Shader" : desc.path.c_str();

        if (desc.backend == RHIBackendType::OpenGL)
        {
            if (result.compileResult.glslSource.empty())
            {
                result.compileResult.success = false;
                result.compileResult.errorMessage = "OpenGL shader compilation failed: no GLSL source generated";
                return result;
            }
            shaderDesc.bytecode = reinterpret_cast<const uint8*>(result.compileResult.glslSource.data());
            shaderDesc.bytecodeSize = result.compileResult.glslSource.size();
        }
        else if (desc.backend == RHIBackendType::Metal)
        {
            if (result.compileResult.mslSource.empty())
            {
                result.compileResult.success = false;
                result.compileResult.errorMessage = "Metal shader compilation failed: no MSL source generated";
                return result;
            }
            shaderDesc.bytecode = reinterpret_cast<const uint8*>(result.compileResult.mslSource.data());
            shaderDesc.bytecodeSize = result.compileResult.mslSource.size();
        }
        else
        {
            shaderDesc.bytecode = result.compileResult.bytecode.data();
            shaderDesc.bytecodeSize = result.compileResult.bytecode.size();
        }

        result.shader = device->CreateShader(shaderDesc);
        if (!result.shader)
        {
            result.compileResult.success = false;
            result.compileResult.errorMessage = "Failed to create RHI shader";
            return result;
        }

        // Save to cache
        if (m_cacheManager)
        {
            ShaderCacheEntry cacheEntry;
            cacheEntry.bytecode = result.compileResult.bytecode;
            cacheEntry.reflection = result.compileResult.reflection;
            cacheEntry.backend = desc.backend;
            cacheEntry.stage = desc.stage;
            cacheEntry.debugInfo = desc.enableDebugInfo;
            cacheEntry.optimized = desc.enableOptimization;
            cacheEntry.glslSource = result.compileResult.glslSource;
            cacheEntry.glslVersion = result.compileResult.glslVersion;
            cacheEntry.mslSource = result.compileResult.mslSource;
            cacheEntry.mslEntryPoint = result.compileResult.mslEntryPoint;

            // Set up source info
            if (!desc.path.empty())
            {
                cacheEntry.sourceInfo.mainFile = desc.path;
                cacheEntry.sourceInfo.fileHashes[desc.path] = ShaderSourceInfo::ComputeFileHash(desc.path);
                cacheEntry.sourceInfo.combinedHash = cacheEntry.sourceInfo.ComputeCombinedHash();
            }

            m_cacheManager->Save(key, cacheEntry);
        }

        {
            std::lock_guard<std::mutex> lock(m_legacyCacheMutex);
            m_legacyCache.emplace(key, result.shader);
        }

        // Register for hot reload if enabled
        if (m_hotReloader && m_hotReloader->IsEnabled() && !desc.path.empty())
        {
            m_hotReloader->RegisterShader(
                device,
                desc.path,
                result.shader,
                ConvertToPermutationDesc(desc));
        }

        return result;
    }

    // =========================================================================
    // Asynchronous Loading
    // =========================================================================

    CompileHandle ShaderManager::LoadFromFileAsync(
        IRHIDevice* device,
        const ShaderLoadDesc& desc,
        LoadCallback onComplete)
    {
        std::string source;
        if (!LoadFile(desc.path, source))
        {
            if (onComplete)
            {
                ShaderLoadResult result;
                result.compileResult.errorMessage = "Failed to load shader file: " + desc.path;
                onComplete(result);
            }
            return RVX_INVALID_COMPILE_HANDLE;
        }

        return LoadFromSourceAsync(device, desc, source, onComplete);
    }

    CompileHandle ShaderManager::LoadFromSourceAsync(
        IRHIDevice* device,
        const ShaderLoadDesc& desc,
        const std::string& source,
        LoadCallback onComplete)
    {
        if (!device || !m_compileService)
        {
            if (onComplete)
            {
                ShaderLoadResult result;
                result.compileResult.errorMessage = "ShaderManager not initialized";
                onComplete(result);
            }
            return RVX_INVALID_COMPILE_HANDLE;
        }

        ShaderCompileOptions options;
        options.stage = desc.stage;
        options.entryPoint = desc.entryPoint.c_str();
        options.sourceCode = source.c_str();
        options.sourcePath = desc.path.empty() ? nullptr : desc.path.c_str();
        options.targetProfile = desc.targetProfile.empty() ? nullptr : desc.targetProfile.c_str();
        options.defines = desc.defines;
        options.targetBackend = desc.backend;
        options.enableDebugInfo = desc.enableDebugInfo;
        options.enableOptimization = desc.enableOptimization;

        // Capture for callback
        auto descCopy = desc;
        auto devicePtr = device;
        auto* self = this;

        CompileHandle handle = m_compileService->CompileAsync(options,
            [self, devicePtr, descCopy, onComplete](const ShaderCompileResult& compileResult)
            {
                ShaderLoadResult result;
                result.compileResult = compileResult;

                if (compileResult.success)
                {
                    RHIShaderDesc shaderDesc;
                    shaderDesc.stage = descCopy.stage;
                    shaderDesc.entryPoint = descCopy.entryPoint.c_str();
                    shaderDesc.debugName = descCopy.path.empty() ? "Shader" : descCopy.path.c_str();

                    if (descCopy.backend == RHIBackendType::OpenGL)
                    {
                        shaderDesc.bytecode = reinterpret_cast<const uint8*>(compileResult.glslSource.data());
                        shaderDesc.bytecodeSize = compileResult.glslSource.size();
                    }
                    else if (descCopy.backend == RHIBackendType::Metal)
                    {
                        shaderDesc.bytecode = reinterpret_cast<const uint8*>(compileResult.mslSource.data());
                        shaderDesc.bytecodeSize = compileResult.mslSource.size();
                    }
                    else
                    {
                        shaderDesc.bytecode = compileResult.bytecode.data();
                        shaderDesc.bytecodeSize = compileResult.bytecode.size();
                    }

                    result.shader = devicePtr->CreateShader(shaderDesc);
                    if (!result.shader)
                    {
                        result.compileResult.success = false;
                        result.compileResult.errorMessage = "Failed to create RHI shader";
                    }
                }

                if (onComplete)
                {
                    onComplete(result);
                }
            },
            CompilePriority::Normal);

        {
            std::lock_guard<std::mutex> lock(m_loadTasksMutex);
            m_loadTasks[handle] = desc;
        }

        return handle;
    }

    ShaderLoadResult ShaderManager::WaitForLoad(CompileHandle handle)
    {
        ShaderLoadResult result;
        if (!m_compileService)
        {
            result.compileResult.errorMessage = "ShaderManager not initialized";
            return result;
        }

        result.compileResult = m_compileService->Wait(handle);
        return result;
    }

    bool ShaderManager::IsLoadComplete(CompileHandle handle) const
    {
        if (!m_compileService)
        {
            return true;
        }
        return m_compileService->IsComplete(handle);
    }

    // =========================================================================
    // Variant System
    // =========================================================================

    void ShaderManager::RegisterShaderVariants(
        const std::string& shaderPath,
        const ShaderPermutationSpace& space,
        const ShaderLoadDesc& baseDesc)
    {
        if (m_permutationSystem)
        {
            m_permutationSystem->RegisterShader(
                shaderPath,
                space,
                ConvertToPermutationDesc(baseDesc));
        }
    }

    RHIShaderRef ShaderManager::GetShaderVariant(
        IRHIDevice* device,
        const std::string& shaderPath,
        const std::vector<ShaderMacro>& defines)
    {
        if (m_permutationSystem)
        {
            return m_permutationSystem->GetVariant(device, shaderPath, defines);
        }
        return nullptr;
    }

    void ShaderManager::PrewarmVariants(
        IRHIDevice* device,
        const std::string& shaderPath,
        const std::vector<std::vector<ShaderMacro>>& variants)
    {
        if (m_permutationSystem)
        {
            m_permutationSystem->PrewarmVariants(device, shaderPath, variants);
        }
    }

    // =========================================================================
    // Hot Reload
    // =========================================================================

    void ShaderManager::EnableHotReload()
    {
        if (m_hotReloader)
        {
            m_hotReloader->Enable();
        }
    }

    void ShaderManager::DisableHotReload()
    {
        if (m_hotReloader)
        {
            m_hotReloader->Disable();
        }
    }

    bool ShaderManager::IsHotReloadEnabled() const
    {
        return m_hotReloader && m_hotReloader->IsEnabled();
    }

    void ShaderManager::SetHotReloadCallback(ShaderReloadCallback callback)
    {
        if (m_hotReloader)
        {
            m_hotReloader->SetGlobalReloadCallback(std::move(callback));
        }
    }

    void ShaderManager::AddShaderWatchDirectory(const std::filesystem::path& dir)
    {
        if (m_hotReloader)
        {
            m_hotReloader->AddWatchDirectory(dir);
        }
        m_config.shaderDirectories.push_back(dir);
    }

    // =========================================================================
    // Update
    // =========================================================================

    void ShaderManager::Update()
    {
        if (m_hotReloader)
        {
            m_hotReloader->Update();
        }
    }

    // =========================================================================
    // Cache Management
    // =========================================================================

    void ShaderManager::ClearCache()
    {
        ClearMemoryCache();
    }

    void ShaderManager::ClearMemoryCache()
    {
        {
            std::lock_guard<std::mutex> lock(m_legacyCacheMutex);
            m_legacyCache.clear();
        }

        if (m_cacheManager)
        {
            m_cacheManager->ClearMemoryCache();
        }
    }

    void ShaderManager::ClearDiskCache()
    {
        if (m_cacheManager)
        {
            m_cacheManager->InvalidateAll();
        }
    }

    void ShaderManager::InvalidateShader(const std::string& shaderPath)
    {
        // Invalidate from legacy cache
        {
            std::lock_guard<std::mutex> lock(m_legacyCacheMutex);
            // Would need to track path -> key mapping
            // For now, just clear all
            m_legacyCache.clear();
        }

        // Clear variants
        if (m_permutationSystem)
        {
            m_permutationSystem->ClearVariants(shaderPath);
        }
    }

    // =========================================================================
    // Statistics
    // =========================================================================

    ShaderManagerStats ShaderManager::GetStats() const
    {
        ShaderManagerStats stats;

        if (m_compileService)
        {
            stats.compileStats = m_compileService->GetStatistics();
            stats.pendingCompiles = m_compileService->GetPendingCount();
        }

        if (m_cacheManager)
        {
            stats.cacheStats = m_cacheManager->GetStatistics();
        }

        if (m_permutationSystem)
        {
            stats.pendingCompiles += m_permutationSystem->GetPendingCompileCount();
        }

        if (m_hotReloader)
        {
            auto hotStats = m_hotReloader->GetStatistics();
            stats.reloadCount = hotStats.reloadCount;
            stats.reloadSuccessCount = hotStats.successCount;
            stats.reloadFailureCount = hotStats.failureCount;
        }

        return stats;
    }

    void ShaderManager::ResetStats()
    {
        if (m_compileService)
        {
            m_compileService->ResetStatistics();
        }

        if (m_cacheManager)
        {
            m_cacheManager->ResetStatistics();
        }
    }

    // =========================================================================
    // Internal Implementation
    // =========================================================================

    uint64 ShaderManager::BuildCacheKey(const ShaderLoadDesc& desc, uint64 sourceHash) const
    {
        // Use FNV-1a hash for better distribution
        constexpr uint64 FNV_OFFSET_BASIS = 0xcbf29ce484222325ull;
        constexpr uint64 FNV_PRIME = 0x100000001b3ull;

        auto hashCombine = [](uint64& hash, uint64 value)
        {
            hash ^= value;
            hash *= FNV_PRIME;
        };

        uint64 hash = FNV_OFFSET_BASIS;

        hashCombine(hash, std::hash<std::string>{}(desc.path));
        hashCombine(hash, std::hash<std::string>{}(desc.entryPoint));
        hashCombine(hash, static_cast<uint64>(desc.stage));
        hashCombine(hash, static_cast<uint64>(desc.backend));
        hashCombine(hash, std::hash<std::string>{}(desc.targetProfile));
        hashCombine(hash, sourceHash);
        hashCombine(hash, desc.enableDebugInfo ? 1ull : 0ull);
        hashCombine(hash, desc.enableOptimization ? 1ull : 0ull);

        for (const auto& def : desc.defines)
        {
            hashCombine(hash, std::hash<std::string>{}(def.name));
            hashCombine(hash, std::hash<std::string>{}(def.value));
        }

        return hash;
    }

    bool ShaderManager::LoadFile(const std::string& path, std::string& outSource) const
    {
        std::ifstream file(path);
        if (!file.is_open())
        {
            return false;
        }

        std::stringstream buffer;
        buffer << file.rdbuf();
        outSource = buffer.str();
        return true;
    }

    ShaderPermutationLoadDesc ShaderManager::ConvertToPermutationDesc(const ShaderLoadDesc& desc) const
    {
        ShaderPermutationLoadDesc permDesc;
        permDesc.path = desc.path;
        permDesc.entryPoint = desc.entryPoint;
        permDesc.stage = desc.stage;
        permDesc.backend = desc.backend;
        permDesc.targetProfile = desc.targetProfile;
        permDesc.enableDebugInfo = desc.enableDebugInfo;
        permDesc.enableOptimization = desc.enableOptimization;
        return permDesc;
    }

} // namespace RVX
