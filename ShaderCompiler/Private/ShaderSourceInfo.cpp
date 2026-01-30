#include "ShaderCompiler/ShaderSourceInfo.h"
#include "Core/Log.h"
#include <fstream>
#include <cstring>

namespace RVX
{
    namespace
    {
        // FNV-1a hash constants
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

        void WriteString(std::vector<uint8>& out, const std::string& str)
        {
            uint32 len = static_cast<uint32>(str.size());
            size_t offset = out.size();
            out.resize(offset + sizeof(uint32) + len);
            std::memcpy(out.data() + offset, &len, sizeof(uint32));
            if (len > 0)
            {
                std::memcpy(out.data() + offset + sizeof(uint32), str.data(), len);
            }
        }

        std::string ReadString(const uint8*& ptr, const uint8* end)
        {
            if (ptr + sizeof(uint32) > end)
            {
                return "";
            }

            uint32 len = 0;
            std::memcpy(&len, ptr, sizeof(uint32));
            ptr += sizeof(uint32);

            if (ptr + len > end)
            {
                return "";
            }

            std::string result(reinterpret_cast<const char*>(ptr), len);
            ptr += len;
            return result;
        }
    }

    uint64 ShaderSourceInfo::ComputeCombinedHash() const
    {
        uint64 hash = FNV_OFFSET_BASIS;

        // Hash main file path
        hash ^= ComputeStringHash(mainFile);
        hash *= FNV_PRIME;

        // Hash all include file hashes
        for (const auto& [path, fileHash] : fileHashes)
        {
            hash ^= ComputeStringHash(path);
            hash *= FNV_PRIME;
            hash ^= fileHash;
            hash *= FNV_PRIME;
        }

        return hash;
    }

    bool ShaderSourceInfo::HasChanged(const std::filesystem::path& baseDir) const
    {
        // Check main file
        std::filesystem::path mainPath = baseDir.empty() ? 
            std::filesystem::path(mainFile) : baseDir / mainFile;

        auto mainIt = fileHashes.find(mainFile);
        if (mainIt != fileHashes.end())
        {
            uint64 currentHash = ComputeFileHash(mainPath);
            if (currentHash != mainIt->second)
            {
                return true;
            }
        }

        // Check all include files
        for (const auto& include : includeFiles)
        {
            std::filesystem::path includePath = baseDir.empty() ?
                std::filesystem::path(include) : baseDir / include;

            auto it = fileHashes.find(include);
            if (it != fileHashes.end())
            {
                uint64 currentHash = ComputeFileHash(includePath);
                if (currentHash != it->second)
                {
                    return true;
                }
            }
            else
            {
                // File not in hash map but in include list - something changed
                return true;
            }
        }

        return false;
    }

    void ShaderSourceInfo::AddInclude(const std::string& path, uint64 hash)
    {
        // Avoid duplicates
        if (std::find(includeFiles.begin(), includeFiles.end(), path) == includeFiles.end())
        {
            includeFiles.push_back(path);
        }
        fileHashes[path] = hash;
    }

    void ShaderSourceInfo::Clear()
    {
        mainFile.clear();
        includeFiles.clear();
        fileHashes.clear();
        combinedHash = 0;
    }

    void ShaderSourceInfo::Serialize(std::vector<uint8>& out) const
    {
        // Format:
        // [mainFile string]
        // [includeCount: uint32]
        // [includeFiles...]
        // [hashCount: uint32]
        // [path, hash pairs...]
        // [combinedHash: uint64]

        WriteString(out, mainFile);

        // Include files
        uint32 includeCount = static_cast<uint32>(includeFiles.size());
        size_t offset = out.size();
        out.resize(offset + sizeof(uint32));
        std::memcpy(out.data() + offset, &includeCount, sizeof(uint32));

        for (const auto& include : includeFiles)
        {
            WriteString(out, include);
        }

        // File hashes
        uint32 hashCount = static_cast<uint32>(fileHashes.size());
        offset = out.size();
        out.resize(offset + sizeof(uint32));
        std::memcpy(out.data() + offset, &hashCount, sizeof(uint32));

        for (const auto& [path, hash] : fileHashes)
        {
            WriteString(out, path);
            offset = out.size();
            out.resize(offset + sizeof(uint64));
            std::memcpy(out.data() + offset, &hash, sizeof(uint64));
        }

        // Combined hash
        offset = out.size();
        out.resize(offset + sizeof(uint64));
        std::memcpy(out.data() + offset, &combinedHash, sizeof(uint64));
    }

    ShaderSourceInfo ShaderSourceInfo::Deserialize(const uint8* data, size_t size)
    {
        ShaderSourceInfo info;
        if (!data || size == 0)
        {
            return info;
        }

        const uint8* ptr = data;
        const uint8* end = data + size;

        // Main file
        info.mainFile = ReadString(ptr, end);

        // Include files
        if (ptr + sizeof(uint32) > end)
        {
            return info;
        }

        uint32 includeCount = 0;
        std::memcpy(&includeCount, ptr, sizeof(uint32));
        ptr += sizeof(uint32);

        info.includeFiles.reserve(includeCount);
        for (uint32 i = 0; i < includeCount; ++i)
        {
            info.includeFiles.push_back(ReadString(ptr, end));
        }

        // File hashes
        if (ptr + sizeof(uint32) > end)
        {
            return info;
        }

        uint32 hashCount = 0;
        std::memcpy(&hashCount, ptr, sizeof(uint32));
        ptr += sizeof(uint32);

        for (uint32 i = 0; i < hashCount; ++i)
        {
            std::string path = ReadString(ptr, end);
            if (ptr + sizeof(uint64) > end)
            {
                break;
            }

            uint64 hash = 0;
            std::memcpy(&hash, ptr, sizeof(uint64));
            ptr += sizeof(uint64);

            info.fileHashes[path] = hash;
        }

        // Combined hash
        if (ptr + sizeof(uint64) <= end)
        {
            std::memcpy(&info.combinedHash, ptr, sizeof(uint64));
        }

        return info;
    }

    uint64 ShaderSourceInfo::ComputeFileHash(const std::filesystem::path& path)
    {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file.is_open())
        {
            return 0;
        }

        std::streamsize size = file.tellg();
        if (size <= 0)
        {
            return 0;
        }

        file.seekg(0, std::ios::beg);

        std::vector<char> buffer(static_cast<size_t>(size));
        if (!file.read(buffer.data(), size))
        {
            return 0;
        }

        return FNV1aHash(buffer.data(), buffer.size());
    }

    uint64 ShaderSourceInfo::ComputeStringHash(const std::string& str)
    {
        return FNV1aHash(str.data(), str.size());
    }

} // namespace RVX
