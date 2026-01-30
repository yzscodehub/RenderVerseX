#include "ShaderCompiler/ShaderPermutation.h"
#include "ShaderCompiler/ShaderCompileService.h"
#include "ShaderCompiler/ShaderCacheManager.h"
#include "RHI/RHIDevice.h"
#include "Core/Log.h"
#include <fstream>
#include <sstream>
#include <algorithm>

namespace RVX
{
    namespace
    {
        // FNV-1a hash
        constexpr uint64 FNV_OFFSET_BASIS = 0xcbf29ce484222325ull;
        constexpr uint64 FNV_PRIME = 0x100000001b3ull;

        uint64 HashString(const std::string& str)
        {
            uint64 hash = FNV_OFFSET_BASIS;
            for (char c : str)
            {
                hash ^= static_cast<uint8>(c);
                hash *= FNV_PRIME;
            }
            return hash;
        }

        uint64 HashCombine(uint64 a, uint64 b)
        {
            return a ^ (b + 0x9e3779b9ull + (a << 6) + (a >> 2));
        }
    }

    // =========================================================================
    // ShaderPermutationSpace Implementation
    // =========================================================================

    uint64 ShaderPermutationSpace::GetTotalVariantCount() const
    {
        if (dimensions.empty())
        {
            return 1;
        }

        uint64 count = 1;
        for (const auto& dim : dimensions)
        {
            uint64 dimSize = dim.values.size();
            if (dim.optional)
            {
                dimSize++; // +1 for undefined case
            }
            count *= dimSize;
        }
        return count;
    }

    std::vector<std::vector<ShaderMacro>> ShaderPermutationSpace::EnumerateAll() const
    {
        std::vector<std::vector<ShaderMacro>> result;

        if (dimensions.empty())
        {
            result.push_back({});
            return result;
        }

        // Build all combinations
        std::vector<size_t> indices(dimensions.size(), 0);
        std::vector<size_t> maxIndices;
        for (const auto& dim : dimensions)
        {
            size_t max = dim.values.size();
            if (dim.optional)
            {
                max++; // +1 for undefined
            }
            maxIndices.push_back(max);
        }

        while (true)
        {
            // Build current combination
            std::vector<ShaderMacro> variant;
            for (size_t i = 0; i < dimensions.size(); ++i)
            {
                const auto& dim = dimensions[i];
                size_t idx = indices[i];

                if (dim.optional && idx == dim.values.size())
                {
                    // Skip - macro is undefined
                    continue;
                }

                if (idx < dim.values.size())
                {
                    ShaderMacro macro;
                    macro.name = dim.name;
                    macro.value = dim.values[idx];
                    variant.push_back(macro);
                }
            }

            result.push_back(std::move(variant));

            // Increment indices
            bool done = true;
            for (size_t i = 0; i < indices.size(); ++i)
            {
                indices[i]++;
                if (indices[i] < maxIndices[i])
                {
                    done = false;
                    break;
                }
                indices[i] = 0;
            }

            if (done)
            {
                break;
            }
        }

        return result;
    }

    uint64 ShaderPermutationSpace::ComputePermutationHash(const std::vector<ShaderMacro>& defines) const
    {
        // Normalize first
        auto normalized = Normalize(defines);

        // Hash the normalized defines
        uint64 hash = FNV_OFFSET_BASIS;
        for (const auto& macro : normalized)
        {
            hash = HashCombine(hash, HashString(macro.name));
            hash = HashCombine(hash, HashString(macro.value));
        }
        return hash;
    }

    std::vector<ShaderMacro> ShaderPermutationSpace::Normalize(const std::vector<ShaderMacro>& defines) const
    {
        std::vector<ShaderMacro> result;

        // Build a map of provided defines
        std::unordered_map<std::string, std::string> defineMap;
        for (const auto& def : defines)
        {
            defineMap[def.name] = def.value;
        }

        // Process each dimension
        for (const auto& dim : dimensions)
        {
            auto it = defineMap.find(dim.name);
            if (it != defineMap.end())
            {
                // Use provided value
                ShaderMacro macro;
                macro.name = dim.name;
                macro.value = it->second;
                result.push_back(macro);
            }
            else if (!dim.optional && !dim.defaultValue.empty())
            {
                // Use default value
                ShaderMacro macro;
                macro.name = dim.name;
                macro.value = dim.defaultValue;
                result.push_back(macro);
            }
            // If optional and not provided, skip
        }

        // Sort by name for consistent hashing
        std::sort(result.begin(), result.end(),
            [](const ShaderMacro& a, const ShaderMacro& b)
            {
                return a.name < b.name;
            });

        return result;
    }

    bool ShaderPermutationSpace::IsValid(const std::vector<ShaderMacro>& defines) const
    {
        for (const auto& def : defines)
        {
            bool found = false;
            for (const auto& dim : dimensions)
            {
                if (dim.name == def.name)
                {
                    // Check if value is valid
                    auto it = std::find(dim.values.begin(), dim.values.end(), def.value);
                    if (it == dim.values.end())
                    {
                        return false; // Invalid value
                    }
                    found = true;
                    break;
                }
            }
            // Unknown macro is allowed (might be a user-defined one)
        }
        return true;
    }

    // =========================================================================
    // ShaderPermutationSystem Implementation
    // =========================================================================

    ShaderPermutationSystem::ShaderPermutationSystem(
        ShaderCompileService* compileService,
        ShaderCacheManager* cacheManager)
        : m_compileService(compileService)
        , m_cacheManager(cacheManager)
    {
    }

    void ShaderPermutationSystem::RegisterShader(
        const std::string& shaderPath,
        const ShaderPermutationSpace& space,
        const ShaderPermutationLoadDesc& baseDesc)
    {
        std::unique_lock<std::shared_mutex> lock(m_shadersMutex);

        auto entry = std::make_unique<ShaderEntry>();
        entry->space = space;
        entry->baseDesc = baseDesc;

        // Load source
        if (!LoadShaderSource(shaderPath, entry->source))
        {
            RVX_CORE_ERROR("ShaderPermutationSystem: Failed to load shader source: {}", shaderPath);
            return;
        }

        m_shaders[shaderPath] = std::move(entry);
        RVX_CORE_INFO("ShaderPermutationSystem: Registered shader with {} variants: {}",
            space.GetTotalVariantCount(), shaderPath);
    }

    void ShaderPermutationSystem::UnregisterShader(const std::string& shaderPath)
    {
        std::unique_lock<std::shared_mutex> lock(m_shadersMutex);
        m_shaders.erase(shaderPath);
    }

    bool ShaderPermutationSystem::IsRegistered(const std::string& shaderPath) const
    {
        std::shared_lock<std::shared_mutex> lock(m_shadersMutex);
        return m_shaders.find(shaderPath) != m_shaders.end();
    }

    RHIShaderRef ShaderPermutationSystem::GetVariant(
        IRHIDevice* device,
        const std::string& shaderPath,
        const std::vector<ShaderMacro>& defines)
    {
        if (!device || !m_compileService)
        {
            return nullptr;
        }

        ShaderEntry* entry = nullptr;
        {
            std::shared_lock<std::shared_mutex> lock(m_shadersMutex);
            auto it = m_shaders.find(shaderPath);
            if (it == m_shaders.end())
            {
                RVX_CORE_ERROR("ShaderPermutationSystem: Shader not registered: {}", shaderPath);
                return nullptr;
            }
            entry = it->second.get();
        }

        uint64 variantKey = ComputeVariantKey(shaderPath, defines);

        // Check if already compiled
        {
            std::lock_guard<std::mutex> lock(entry->mutex);
            auto it = entry->variants.find(variantKey);
            if (it != entry->variants.end())
            {
                return it->second;
            }

            // Check if pending
            auto pendingIt = entry->pendingCompiles.find(variantKey);
            if (pendingIt != entry->pendingCompiles.end())
            {
                // Wait for pending compilation
                ShaderCompileResult result = m_compileService->Wait(pendingIt->second);
                entry->pendingCompiles.erase(pendingIt);

                if (result.success)
                {
                    RHIShaderDesc shaderDesc;
                    shaderDesc.stage = entry->baseDesc.stage;
                    shaderDesc.entryPoint = entry->baseDesc.entryPoint.c_str();
                    shaderDesc.bytecode = result.bytecode.data();
                    shaderDesc.bytecodeSize = result.bytecode.size();
                    shaderDesc.debugName = shaderPath.c_str();

                    RHIShaderRef shader = device->CreateShader(shaderDesc);
                    if (shader)
                    {
                        entry->variants[variantKey] = shader;
                        return shader;
                    }
                }
                return nullptr;
            }
        }

        // Compile now
        ShaderCompileOptions options;
        options.stage = entry->baseDesc.stage;
        options.entryPoint = entry->baseDesc.entryPoint.c_str();
        options.sourceCode = entry->source.c_str();
        options.sourcePath = shaderPath.c_str();
        options.targetProfile = entry->baseDesc.targetProfile.empty() ? nullptr : entry->baseDesc.targetProfile.c_str();
        options.targetBackend = entry->baseDesc.backend;
        options.enableDebugInfo = entry->baseDesc.enableDebugInfo;
        options.enableOptimization = entry->baseDesc.enableOptimization;

        // Merge defines
        auto normalizedDefines = entry->space.Normalize(defines);
        options.defines = normalizedDefines;

        ShaderCompileResult result = m_compileService->CompileSync(options);
        if (!result.success)
        {
            RVX_CORE_ERROR("ShaderPermutationSystem: Failed to compile variant: {}", result.errorMessage);
            return nullptr;
        }

        // Create shader
        RHIShaderDesc shaderDesc;
        shaderDesc.stage = entry->baseDesc.stage;
        shaderDesc.entryPoint = entry->baseDesc.entryPoint.c_str();
        shaderDesc.bytecode = result.bytecode.data();
        shaderDesc.bytecodeSize = result.bytecode.size();
        shaderDesc.debugName = shaderPath.c_str();

        RHIShaderRef shader = device->CreateShader(shaderDesc);
        if (shader)
        {
            std::lock_guard<std::mutex> lock(entry->mutex);
            entry->variants[variantKey] = shader;
        }

        return shader;
    }

    CompileHandle ShaderPermutationSystem::GetVariantAsync(
        IRHIDevice* device,
        const std::string& shaderPath,
        const std::vector<ShaderMacro>& defines,
        std::function<void(RHIShaderRef)> callback)
    {
        if (!device || !m_compileService)
        {
            return RVX_INVALID_COMPILE_HANDLE;
        }

        ShaderEntry* entry = nullptr;
        {
            std::shared_lock<std::shared_mutex> lock(m_shadersMutex);
            auto it = m_shaders.find(shaderPath);
            if (it == m_shaders.end())
            {
                return RVX_INVALID_COMPILE_HANDLE;
            }
            entry = it->second.get();
        }

        uint64 variantKey = ComputeVariantKey(shaderPath, defines);

        // Check if already compiled
        {
            std::lock_guard<std::mutex> lock(entry->mutex);
            auto it = entry->variants.find(variantKey);
            if (it != entry->variants.end())
            {
                if (callback)
                {
                    callback(it->second);
                }
                return RVX_INVALID_COMPILE_HANDLE; // Already done
            }

            // Check if pending
            auto pendingIt = entry->pendingCompiles.find(variantKey);
            if (pendingIt != entry->pendingCompiles.end())
            {
                return pendingIt->second;
            }
        }

        // Start async compilation
        ShaderCompileOptions options;
        options.stage = entry->baseDesc.stage;
        options.entryPoint = entry->baseDesc.entryPoint.c_str();
        options.sourceCode = entry->source.c_str();
        options.sourcePath = shaderPath.c_str();
        options.targetProfile = entry->baseDesc.targetProfile.empty() ? nullptr : entry->baseDesc.targetProfile.c_str();
        options.targetBackend = entry->baseDesc.backend;
        options.enableDebugInfo = entry->baseDesc.enableDebugInfo;
        options.enableOptimization = entry->baseDesc.enableOptimization;

        auto normalizedDefines = entry->space.Normalize(defines);
        options.defines = normalizedDefines;

        // Capture for callback
        auto* entryPtr = entry;
        auto devicePtr = device;
        auto shaderPathCopy = shaderPath;

        CompileHandle handle = m_compileService->CompileAsync(options,
            [entryPtr, variantKey, devicePtr, shaderPathCopy, callback](const ShaderCompileResult& result)
            {
                if (result.success)
                {
                    RHIShaderDesc shaderDesc;
                    shaderDesc.stage = entryPtr->baseDesc.stage;
                    shaderDesc.entryPoint = entryPtr->baseDesc.entryPoint.c_str();
                    shaderDesc.bytecode = result.bytecode.data();
                    shaderDesc.bytecodeSize = result.bytecode.size();
                    shaderDesc.debugName = shaderPathCopy.c_str();

                    RHIShaderRef shader = devicePtr->CreateShader(shaderDesc);
                    if (shader)
                    {
                        std::lock_guard<std::mutex> lock(entryPtr->mutex);
                        entryPtr->variants[variantKey] = shader;
                        entryPtr->pendingCompiles.erase(variantKey);

                        if (callback)
                        {
                            callback(shader);
                        }
                    }
                }
                else
                {
                    std::lock_guard<std::mutex> lock(entryPtr->mutex);
                    entryPtr->pendingCompiles.erase(variantKey);

                    if (callback)
                    {
                        callback(nullptr);
                    }
                }
            },
            CompilePriority::Normal);

        {
            std::lock_guard<std::mutex> lock(entry->mutex);
            entry->pendingCompiles[variantKey] = handle;
        }

        return handle;
    }

    bool ShaderPermutationSystem::HasVariant(
        const std::string& shaderPath,
        const std::vector<ShaderMacro>& defines) const
    {
        std::shared_lock<std::shared_mutex> lock(m_shadersMutex);
        auto it = m_shaders.find(shaderPath);
        if (it == m_shaders.end())
        {
            return false;
        }

        uint64 variantKey = ComputeVariantKey(shaderPath, defines);

        std::lock_guard<std::mutex> entryLock(it->second->mutex);
        return it->second->variants.find(variantKey) != it->second->variants.end();
    }

    void ShaderPermutationSystem::PrewarmVariants(
        IRHIDevice* device,
        const std::string& shaderPath,
        const std::vector<std::vector<ShaderMacro>>& variants,
        VariantPriority priority)
    {
        CompilePriority compilePriority;
        switch (priority)
        {
            case VariantPriority::Critical: compilePriority = CompilePriority::High; break;
            case VariantPriority::High: compilePriority = CompilePriority::Normal; break;
            case VariantPriority::Medium: compilePriority = CompilePriority::Normal; break;
            case VariantPriority::Low: compilePriority = CompilePriority::Low; break;
            default: compilePriority = CompilePriority::Low; break;
        }

        for (const auto& defines : variants)
        {
            if (!HasVariant(shaderPath, defines))
            {
                GetVariantAsync(device, shaderPath, defines, nullptr);
            }
        }
    }

    void ShaderPermutationSystem::PrewarmAllVariants(
        IRHIDevice* device,
        const std::string& shaderPath,
        VariantPriority priority)
    {
        std::shared_lock<std::shared_mutex> lock(m_shadersMutex);
        auto it = m_shaders.find(shaderPath);
        if (it == m_shaders.end())
        {
            return;
        }

        auto allVariants = it->second->space.EnumerateAll();
        lock.unlock();

        PrewarmVariants(device, shaderPath, allVariants, priority);
    }

    float ShaderPermutationSystem::GetPrewarmProgress(const std::string& shaderPath) const
    {
        std::shared_lock<std::shared_mutex> lock(m_shadersMutex);
        auto it = m_shaders.find(shaderPath);
        if (it == m_shaders.end())
        {
            return 1.0f;
        }

        uint64 total = it->second->space.GetTotalVariantCount();
        if (total == 0)
        {
            return 1.0f;
        }

        std::lock_guard<std::mutex> entryLock(it->second->mutex);
        uint64 compiled = it->second->variants.size();

        return static_cast<float>(compiled) / static_cast<float>(total);
    }

    uint32 ShaderPermutationSystem::GetCompiledVariantCount(const std::string& shaderPath) const
    {
        std::shared_lock<std::shared_mutex> lock(m_shadersMutex);
        auto it = m_shaders.find(shaderPath);
        if (it == m_shaders.end())
        {
            return 0;
        }

        std::lock_guard<std::mutex> entryLock(it->second->mutex);
        return static_cast<uint32>(it->second->variants.size());
    }

    uint32 ShaderPermutationSystem::GetTotalVariantCount(const std::string& shaderPath) const
    {
        std::shared_lock<std::shared_mutex> lock(m_shadersMutex);
        auto it = m_shaders.find(shaderPath);
        if (it == m_shaders.end())
        {
            return 0;
        }

        return static_cast<uint32>(it->second->space.GetTotalVariantCount());
    }

    uint32 ShaderPermutationSystem::GetPendingCompileCount() const
    {
        uint32 count = 0;
        std::shared_lock<std::shared_mutex> lock(m_shadersMutex);
        for (const auto& [path, entry] : m_shaders)
        {
            std::lock_guard<std::mutex> entryLock(entry->mutex);
            count += static_cast<uint32>(entry->pendingCompiles.size());
        }
        return count;
    }

    void ShaderPermutationSystem::ClearVariants(const std::string& shaderPath)
    {
        std::shared_lock<std::shared_mutex> lock(m_shadersMutex);
        auto it = m_shaders.find(shaderPath);
        if (it != m_shaders.end())
        {
            std::lock_guard<std::mutex> entryLock(it->second->mutex);
            it->second->variants.clear();
            it->second->pendingCompiles.clear();
        }
    }

    void ShaderPermutationSystem::ClearAllVariants()
    {
        std::shared_lock<std::shared_mutex> lock(m_shadersMutex);
        for (auto& [path, entry] : m_shaders)
        {
            std::lock_guard<std::mutex> entryLock(entry->mutex);
            entry->variants.clear();
            entry->pendingCompiles.clear();
        }
    }

    uint64 ShaderPermutationSystem::ComputeVariantKey(
        const std::string& shaderPath,
        const std::vector<ShaderMacro>& defines) const
    {
        std::shared_lock<std::shared_mutex> lock(m_shadersMutex);
        auto it = m_shaders.find(shaderPath);
        if (it == m_shaders.end())
        {
            return 0;
        }

        uint64 pathHash = HashString(shaderPath);
        uint64 permHash = it->second->space.ComputePermutationHash(defines);
        return HashCombine(pathHash, permHash);
    }

    bool ShaderPermutationSystem::LoadShaderSource(const std::string& path, std::string& outSource) const
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

} // namespace RVX
