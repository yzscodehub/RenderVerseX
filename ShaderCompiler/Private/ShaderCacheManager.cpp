#include "ShaderCompiler/ShaderCacheManager.h"
#include "Core/Log.h"
#include <fstream>
#include <cstring>
#include <algorithm>
#include <chrono>

namespace RVX
{
    namespace
    {
        // FNV-1a hash
        constexpr uint64 FNV_OFFSET_BASIS = 0xcbf29ce484222325ull;
        constexpr uint64 FNV_PRIME = 0x100000001b3ull;

        uint64 FNV1aHash(const void* data, size_t size)
        {
            uint64 hash = FNV_OFFSET_BASIS;
            const uint8* bytes = static_cast<const uint8*>(data);
            for (size_t i = 0; i < size; ++i)
            {
                hash ^= bytes[i];
                hash *= FNV_PRIME;
            }
            return hash;
        }

        uint64 GetCurrentTimestamp()
        {
            return static_cast<uint64>(
                std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::system_clock::now().time_since_epoch()
                ).count()
            );
        }
    }

    ShaderCacheManager::ShaderCacheManager(const Config& config)
        : m_config(config)
    {
        if (!m_config.cacheDirectory.empty())
        {
            std::error_code ec;
            std::filesystem::create_directories(m_config.cacheDirectory, ec);
            if (ec)
            {
                RVX_CORE_WARN("ShaderCacheManager: Failed to create cache directory: {}",
                    m_config.cacheDirectory.string());
            }
            else
            {
                RVX_CORE_INFO("ShaderCacheManager: Cache directory: {}",
                    m_config.cacheDirectory.string());
            }
        }
    }

    std::optional<ShaderCacheEntry> ShaderCacheManager::Load(uint64 key)
    {
        // Try memory cache first
        if (m_config.enableMemoryCache)
        {
            std::shared_lock<std::shared_mutex> lock(m_cacheMutex);
            auto it = m_memoryCache.find(key);
            if (it != m_memoryCache.end())
            {
                std::lock_guard<std::mutex> statsLock(m_statsMutex);
                m_stats.memoryHits++;
                return it->second;
            }
        }

        // Try disk cache
        if (m_config.enableDiskCache)
        {
            ShaderCacheEntry entry;
            if (LoadFromDisk(key, entry))
            {
                // Validate dependencies if requested
                if (m_config.validateOnLoad && !entry.sourceInfo.IsEmpty())
                {
                    if (entry.sourceInfo.HasChanged())
                    {
                        RVX_CORE_DEBUG("ShaderCacheManager: Cache invalidated due to source changes: {:016X}", key);
                        Invalidate(key);
                        std::lock_guard<std::mutex> statsLock(m_statsMutex);
                        m_stats.misses++;
                        return std::nullopt;
                    }
                }

                // Add to memory cache
                if (m_config.enableMemoryCache)
                {
                    std::unique_lock<std::shared_mutex> lock(m_cacheMutex);
                    m_memoryCache[key] = entry;
                }

                std::lock_guard<std::mutex> statsLock(m_statsMutex);
                m_stats.diskHits++;
                return entry;
            }
        }

        std::lock_guard<std::mutex> statsLock(m_statsMutex);
        m_stats.misses++;
        return std::nullopt;
    }

    void ShaderCacheManager::Save(uint64 key, const ShaderCacheEntry& entry)
    {
        // Save to memory cache
        if (m_config.enableMemoryCache)
        {
            std::unique_lock<std::shared_mutex> lock(m_cacheMutex);
            m_memoryCache[key] = entry;
        }

        // Save to disk cache
        if (m_config.enableDiskCache)
        {
            SaveToDisk(key, entry);
        }
    }

    bool ShaderCacheManager::IsValid(uint64 key, const ShaderSourceInfo& currentInfo)
    {
        auto cached = Load(key);
        if (!cached)
        {
            return false;
        }

        // Compare combined hashes
        return cached->sourceInfo.combinedHash == currentInfo.combinedHash;
    }

    void ShaderCacheManager::Invalidate(uint64 key)
    {
        // Remove from memory cache
        {
            std::unique_lock<std::shared_mutex> lock(m_cacheMutex);
            m_memoryCache.erase(key);
        }

        // Remove from disk cache
        if (m_config.enableDiskCache)
        {
            std::filesystem::path cachePath = GetCachePath(key);
            std::error_code ec;
            std::filesystem::remove(cachePath, ec);
        }

        std::lock_guard<std::mutex> statsLock(m_statsMutex);
        m_stats.invalidations++;
    }

    void ShaderCacheManager::InvalidateAll()
    {
        // Clear memory cache
        {
            std::unique_lock<std::shared_mutex> lock(m_cacheMutex);
            m_memoryCache.clear();
        }

        // Clear disk cache
        if (m_config.enableDiskCache && !m_config.cacheDirectory.empty())
        {
            std::error_code ec;
            for (const auto& entry : std::filesystem::directory_iterator(m_config.cacheDirectory, ec))
            {
                if (entry.path().extension() == ".rvxs")
                {
                    std::filesystem::remove(entry.path(), ec);
                }
            }
        }

        RVX_CORE_INFO("ShaderCacheManager: All caches invalidated");
    }

    void ShaderCacheManager::ClearMemoryCache()
    {
        std::unique_lock<std::shared_mutex> lock(m_cacheMutex);
        m_memoryCache.clear();
        RVX_CORE_DEBUG("ShaderCacheManager: Memory cache cleared");
    }

    void ShaderCacheManager::SetCacheDirectory(const std::filesystem::path& dir)
    {
        m_config.cacheDirectory = dir;
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        if (ec)
        {
            RVX_CORE_WARN("ShaderCacheManager: Failed to create cache directory: {}", dir.string());
        }
    }

    void ShaderCacheManager::PruneCache(uint64 maxAgeSeconds)
    {
        if (!m_config.enableDiskCache || m_config.cacheDirectory.empty())
        {
            return;
        }

        uint64 currentTime = GetCurrentTimestamp();
        std::error_code ec;

        for (const auto& entry : std::filesystem::directory_iterator(m_config.cacheDirectory, ec))
        {
            if (entry.path().extension() != ".rvxs")
            {
                continue;
            }

            auto lastWrite = std::filesystem::last_write_time(entry.path(), ec);
            if (ec)
            {
                continue;
            }

            auto duration = std::filesystem::file_time_type::clock::now() - lastWrite;
            auto ageSeconds = std::chrono::duration_cast<std::chrono::seconds>(duration).count();

            if (static_cast<uint64>(ageSeconds) > maxAgeSeconds)
            {
                std::filesystem::remove(entry.path(), ec);
                RVX_CORE_DEBUG("ShaderCacheManager: Pruned old cache file: {}", entry.path().string());
            }
        }
    }

    void ShaderCacheManager::EnforceSizeLimit()
    {
        if (!m_config.enableDiskCache || m_config.cacheDirectory.empty())
        {
            return;
        }

        struct CacheFile
        {
            std::filesystem::path path;
            uint64 size;
            std::filesystem::file_time_type lastWrite;
        };

        std::vector<CacheFile> cacheFiles;
        uint64 totalSize = 0;
        std::error_code ec;

        for (const auto& entry : std::filesystem::directory_iterator(m_config.cacheDirectory, ec))
        {
            if (entry.path().extension() != ".rvxs")
            {
                continue;
            }

            CacheFile cf;
            cf.path = entry.path();
            cf.size = entry.file_size(ec);
            cf.lastWrite = entry.last_write_time(ec);

            cacheFiles.push_back(cf);
            totalSize += cf.size;
        }

        if (totalSize <= m_config.maxCacheSizeBytes)
        {
            return;
        }

        // Sort by last write time (oldest first)
        std::sort(cacheFiles.begin(), cacheFiles.end(),
            [](const CacheFile& a, const CacheFile& b)
            {
                return a.lastWrite < b.lastWrite;
            });

        // Remove oldest files until under limit
        for (const auto& cf : cacheFiles)
        {
            if (totalSize <= m_config.maxCacheSizeBytes)
            {
                break;
            }

            std::filesystem::remove(cf.path, ec);
            totalSize -= cf.size;
            RVX_CORE_DEBUG("ShaderCacheManager: Removed cache file to enforce size limit: {}", cf.path.string());
        }
    }

    uint64 ShaderCacheManager::GetDiskCacheSize() const
    {
        if (!m_config.enableDiskCache || m_config.cacheDirectory.empty())
        {
            return 0;
        }

        uint64 totalSize = 0;
        std::error_code ec;

        for (const auto& entry : std::filesystem::directory_iterator(m_config.cacheDirectory, ec))
        {
            if (entry.path().extension() == ".rvxs")
            {
                totalSize += entry.file_size(ec);
            }
        }

        return totalSize;
    }

    std::filesystem::path ShaderCacheManager::GetCachePath(uint64 key) const
    {
        std::ostringstream oss;
        oss << std::hex << std::setfill('0') << std::setw(16) << key << ".rvxs";
        return m_config.cacheDirectory / oss.str();
    }

    bool ShaderCacheManager::LoadFromDisk(uint64 key, ShaderCacheEntry& entry)
    {
        std::filesystem::path cachePath = GetCachePath(key);
        std::ifstream file(cachePath, std::ios::binary);
        if (!file.is_open())
        {
            return false;
        }

        // Read header
        ShaderCacheHeader header;
        file.read(reinterpret_cast<char*>(&header), sizeof(header));
        if (!file || !ValidateHeader(header))
        {
            return false;
        }

        // Read bytecode
        if (header.bytecodeSize > 0)
        {
            entry.bytecode.resize(header.bytecodeSize);
            file.seekg(header.bytecodeOffset);
            file.read(reinterpret_cast<char*>(entry.bytecode.data()), header.bytecodeSize);
        }

        // Read reflection
        if (header.reflectionSize > 0 && HasFlag(header.flags, ShaderCacheFlags::HasReflection))
        {
            std::vector<uint8> reflectionData(header.reflectionSize);
            file.seekg(header.reflectionOffset);
            file.read(reinterpret_cast<char*>(reflectionData.data()), header.reflectionSize);
            entry.reflection = DeserializeReflection(reflectionData.data(), reflectionData.size());
        }

        // Read source info
        if (header.sourceInfoSize > 0 && HasFlag(header.flags, ShaderCacheFlags::HasSourceInfo))
        {
            std::vector<uint8> sourceInfoData(header.sourceInfoSize);
            file.seekg(header.sourceInfoOffset);
            file.read(reinterpret_cast<char*>(sourceInfoData.data()), header.sourceInfoSize);
            entry.sourceInfo = ShaderSourceInfo::Deserialize(sourceInfoData.data(), sourceInfoData.size());
        }

        // Read MSL source
        if (header.mslSourceSize > 0 && HasFlag(header.flags, ShaderCacheFlags::HasMSLSource))
        {
            entry.mslSource.resize(header.mslSourceSize);
            file.seekg(header.mslSourceOffset);
            file.read(entry.mslSource.data(), header.mslSourceSize);
        }

        // Read GLSL source
        if (header.glslSourceSize > 0 && HasFlag(header.flags, ShaderCacheFlags::HasGLSLSource))
        {
            entry.glslSource.resize(header.glslSourceSize);
            file.seekg(header.glslSourceOffset);
            file.read(entry.glslSource.data(), header.glslSourceSize);
        }

        entry.backend = header.backend;
        entry.stage = header.stage;
        entry.timestamp = header.timestamp;
        entry.debugInfo = HasFlag(header.flags, ShaderCacheFlags::DebugInfo);
        entry.optimized = HasFlag(header.flags, ShaderCacheFlags::Optimized);

        return true;
    }

    void ShaderCacheManager::SaveToDisk(uint64 key, const ShaderCacheEntry& entry)
    {
        std::filesystem::path cachePath = GetCachePath(key);

        // Serialize reflection
        std::vector<uint8> reflectionData;
        SerializeReflection(entry.reflection, reflectionData);

        // Serialize source info
        std::vector<uint8> sourceInfoData;
        entry.sourceInfo.Serialize(sourceInfoData);

        // Build header
        ShaderCacheHeader header;
        header.timestamp = GetCurrentTimestamp();
        header.backend = entry.backend;
        header.stage = entry.stage;

        // Set flags
        if (entry.debugInfo) header.flags |= ShaderCacheFlags::DebugInfo;
        if (entry.optimized) header.flags |= ShaderCacheFlags::Optimized;
        if (!reflectionData.empty()) header.flags |= ShaderCacheFlags::HasReflection;
        if (!sourceInfoData.empty()) header.flags |= ShaderCacheFlags::HasSourceInfo;
        if (!entry.mslSource.empty()) header.flags |= ShaderCacheFlags::HasMSLSource;
        if (!entry.glslSource.empty()) header.flags |= ShaderCacheFlags::HasGLSLSource;

        // Calculate offsets
        uint32 offset = sizeof(ShaderCacheHeader);

        header.bytecodeOffset = offset;
        header.bytecodeSize = static_cast<uint32>(entry.bytecode.size());
        offset += header.bytecodeSize;

        header.reflectionOffset = offset;
        header.reflectionSize = static_cast<uint32>(reflectionData.size());
        offset += header.reflectionSize;

        header.sourceInfoOffset = offset;
        header.sourceInfoSize = static_cast<uint32>(sourceInfoData.size());
        offset += header.sourceInfoSize;

        header.mslSourceOffset = offset;
        header.mslSourceSize = static_cast<uint32>(entry.mslSource.size());
        offset += header.mslSourceSize;

        header.glslSourceOffset = offset;
        header.glslSourceSize = static_cast<uint32>(entry.glslSource.size());

        // Compute content hash
        header.contentHash = ComputeContentHash(entry);

        // Write file
        std::ofstream file(cachePath, std::ios::binary | std::ios::trunc);
        if (!file.is_open())
        {
            RVX_CORE_WARN("ShaderCacheManager: Failed to open cache file for writing: {}", cachePath.string());
            return;
        }

        file.write(reinterpret_cast<const char*>(&header), sizeof(header));
        file.write(reinterpret_cast<const char*>(entry.bytecode.data()), entry.bytecode.size());
        file.write(reinterpret_cast<const char*>(reflectionData.data()), reflectionData.size());
        file.write(reinterpret_cast<const char*>(sourceInfoData.data()), sourceInfoData.size());
        file.write(entry.mslSource.data(), entry.mslSource.size());
        file.write(entry.glslSource.data(), entry.glslSource.size());

        RVX_CORE_DEBUG("ShaderCacheManager: Saved cache file: {}", cachePath.string());
    }

    bool ShaderCacheManager::ValidateHeader(const ShaderCacheHeader& header) const
    {
        if (header.magic != RVX_SHADER_CACHE_MAGIC)
        {
            return false;
        }

        if (header.version > RVX_SHADER_CACHE_VERSION)
        {
            return false;
        }

        return true;
    }

    void ShaderCacheManager::SerializeReflection(const ShaderReflection& reflection, std::vector<uint8>& out) const
    {
        // Simple serialization format:
        // [resourceCount: uint32]
        // [resources...]
        // [pushConstantCount: uint32]
        // [pushConstants...]
        // [inputCount: uint32]
        // [inputs...]

        auto writeU32 = [&out](uint32 value)
        {
            size_t offset = out.size();
            out.resize(offset + sizeof(uint32));
            std::memcpy(out.data() + offset, &value, sizeof(uint32));
        };

        auto writeString = [&out](const std::string& str)
        {
            uint32 len = static_cast<uint32>(str.size());
            size_t offset = out.size();
            out.resize(offset + sizeof(uint32) + len);
            std::memcpy(out.data() + offset, &len, sizeof(uint32));
            if (len > 0)
            {
                std::memcpy(out.data() + offset + sizeof(uint32), str.data(), len);
            }
        };

        // Resources
        writeU32(static_cast<uint32>(reflection.resources.size()));
        for (const auto& res : reflection.resources)
        {
            writeString(res.name);
            writeU32(res.set);
            writeU32(res.binding);
            writeU32(static_cast<uint32>(res.type));
            writeU32(res.count);
        }

        // Push constants
        writeU32(static_cast<uint32>(reflection.pushConstants.size()));
        for (const auto& pc : reflection.pushConstants)
        {
            writeU32(pc.offset);
            writeU32(pc.size);
        }

        // Inputs
        writeU32(static_cast<uint32>(reflection.inputs.size()));
        for (const auto& input : reflection.inputs)
        {
            writeString(input.semantic);
            writeU32(input.location);
            writeU32(static_cast<uint32>(input.format));
        }
    }

    ShaderReflection ShaderCacheManager::DeserializeReflection(const uint8* data, size_t size) const
    {
        ShaderReflection reflection;
        if (!data || size == 0)
        {
            return reflection;
        }

        const uint8* ptr = data;
        const uint8* end = data + size;

        auto readU32 = [&ptr, end]() -> uint32
        {
            if (ptr + sizeof(uint32) > end) return 0;
            uint32 value = 0;
            std::memcpy(&value, ptr, sizeof(uint32));
            ptr += sizeof(uint32);
            return value;
        };

        auto readString = [&ptr, end]() -> std::string
        {
            if (ptr + sizeof(uint32) > end) return "";
            uint32 len = 0;
            std::memcpy(&len, ptr, sizeof(uint32));
            ptr += sizeof(uint32);
            if (ptr + len > end) return "";
            std::string result(reinterpret_cast<const char*>(ptr), len);
            ptr += len;
            return result;
        };

        // Resources
        uint32 resourceCount = readU32();
        reflection.resources.reserve(resourceCount);
        for (uint32 i = 0; i < resourceCount; ++i)
        {
            ShaderReflection::ResourceBinding res;
            res.name = readString();
            res.set = readU32();
            res.binding = readU32();
            res.type = static_cast<RHIBindingType>(readU32());
            res.count = readU32();
            reflection.resources.push_back(res);
        }

        // Push constants
        uint32 pcCount = readU32();
        reflection.pushConstants.reserve(pcCount);
        for (uint32 i = 0; i < pcCount; ++i)
        {
            ShaderReflection::PushConstantRange pc;
            pc.offset = readU32();
            pc.size = readU32();
            reflection.pushConstants.push_back(pc);
        }

        // Inputs
        uint32 inputCount = readU32();
        reflection.inputs.reserve(inputCount);
        for (uint32 i = 0; i < inputCount; ++i)
        {
            ShaderReflection::InputAttribute input;
            input.semantic = readString();
            input.location = readU32();
            input.format = static_cast<RHIFormat>(readU32());
            reflection.inputs.push_back(input);
        }

        return reflection;
    }

    uint64 ShaderCacheManager::ComputeContentHash(const ShaderCacheEntry& entry) const
    {
        uint64 hash = FNV_OFFSET_BASIS;

        // Hash bytecode
        if (!entry.bytecode.empty())
        {
            hash ^= FNV1aHash(entry.bytecode.data(), entry.bytecode.size());
            hash *= FNV_PRIME;
        }

        // Hash source info combined hash
        hash ^= entry.sourceInfo.combinedHash;
        hash *= FNV_PRIME;

        return hash;
    }

} // namespace RVX
