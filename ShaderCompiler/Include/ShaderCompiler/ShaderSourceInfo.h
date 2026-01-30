#pragma once

#include "Core/Types.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <filesystem>

namespace RVX
{
    // =========================================================================
    // Shader Source File Dependency Information
    // =========================================================================
    struct ShaderSourceInfo
    {
        std::string mainFile;                               // Main file path
        std::vector<std::string> includeFiles;              // Included file list
        std::unordered_map<std::string, uint64> fileHashes; // File hash mapping
        uint64 combinedHash = 0;                            // Combined hash

        // =====================================================================
        // Methods
        // =====================================================================

        /** @brief Compute combined hash (includes all files) */
        uint64 ComputeCombinedHash() const;

        /** @brief Check if any file has changed */
        bool HasChanged(const std::filesystem::path& baseDir = {}) const;

        /** @brief Add an include file */
        void AddInclude(const std::string& path, uint64 hash);

        /** @brief Clear all data */
        void Clear();

        /** @brief Check if empty */
        bool IsEmpty() const { return mainFile.empty(); }

        /** @brief Serialize to byte array */
        void Serialize(std::vector<uint8>& out) const;

        /** @brief Deserialize from byte array */
        static ShaderSourceInfo Deserialize(const uint8* data, size_t size);

        /** @brief Compute file hash */
        static uint64 ComputeFileHash(const std::filesystem::path& path);

        /** @brief Compute string hash */
        static uint64 ComputeStringHash(const std::string& str);
    };

} // namespace RVX
