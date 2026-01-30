#include "ShaderCompiler/ShaderPermutation.h"
#include "ShaderCompiler/ShaderCacheManager.h"
#include "Core/Log.h"
#include <fstream>
#include <sstream>

namespace RVX
{
    // =========================================================================
    // Shader Permutation Cache - Disk persistence for permutation metadata
    // =========================================================================
    namespace ShaderPermutationCache
    {
        // =====================================================================
        // Permutation Registry File Format
        // =====================================================================
        // This stores the registered shader permutation spaces for quick reload

        constexpr uint32 PERMUTATION_CACHE_MAGIC = 0x52565850; // "RVXP"
        constexpr uint32 PERMUTATION_CACHE_VERSION = 1;

        #pragma pack(push, 1)
        struct PermutationCacheHeader
        {
            uint32 magic = PERMUTATION_CACHE_MAGIC;
            uint32 version = PERMUTATION_CACHE_VERSION;
            uint32 shaderCount = 0;
            uint32 reserved = 0;
        };
        #pragma pack(pop)

        // =====================================================================
        // Serialization Helpers
        // =====================================================================
        namespace
        {
            void WriteString(std::ostream& out, const std::string& str)
            {
                uint32 len = static_cast<uint32>(str.size());
                out.write(reinterpret_cast<const char*>(&len), sizeof(len));
                if (len > 0)
                {
                    out.write(str.data(), len);
                }
            }

            std::string ReadString(std::istream& in)
            {
                uint32 len = 0;
                in.read(reinterpret_cast<char*>(&len), sizeof(len));
                if (len == 0 || !in.good())
                {
                    return "";
                }

                std::string result(len, '\0');
                in.read(result.data(), len);
                return result;
            }

            void WriteDimension(std::ostream& out, const ShaderPermutationDimension& dim)
            {
                WriteString(out, dim.name);

                uint32 valueCount = static_cast<uint32>(dim.values.size());
                out.write(reinterpret_cast<const char*>(&valueCount), sizeof(valueCount));
                for (const auto& value : dim.values)
                {
                    WriteString(out, value);
                }

                uint8 optional = dim.optional ? 1 : 0;
                out.write(reinterpret_cast<const char*>(&optional), sizeof(optional));
                WriteString(out, dim.defaultValue);
            }

            ShaderPermutationDimension ReadDimension(std::istream& in)
            {
                ShaderPermutationDimension dim;
                dim.name = ReadString(in);

                uint32 valueCount = 0;
                in.read(reinterpret_cast<char*>(&valueCount), sizeof(valueCount));
                dim.values.reserve(valueCount);
                for (uint32 i = 0; i < valueCount; ++i)
                {
                    dim.values.push_back(ReadString(in));
                }

                uint8 optional = 0;
                in.read(reinterpret_cast<char*>(&optional), sizeof(optional));
                dim.optional = optional != 0;
                dim.defaultValue = ReadString(in);

                return dim;
            }

            void WriteLoadDesc(std::ostream& out, const ShaderPermutationLoadDesc& desc)
            {
                WriteString(out, desc.path);
                WriteString(out, desc.entryPoint);

                uint8 stage = static_cast<uint8>(desc.stage);
                uint8 backend = static_cast<uint8>(desc.backend);
                out.write(reinterpret_cast<const char*>(&stage), sizeof(stage));
                out.write(reinterpret_cast<const char*>(&backend), sizeof(backend));

                WriteString(out, desc.targetProfile);

                uint8 flags = 0;
                if (desc.enableDebugInfo) flags |= 0x01;
                if (desc.enableOptimization) flags |= 0x02;
                out.write(reinterpret_cast<const char*>(&flags), sizeof(flags));
            }

            ShaderPermutationLoadDesc ReadLoadDesc(std::istream& in)
            {
                ShaderPermutationLoadDesc desc;
                desc.path = ReadString(in);
                desc.entryPoint = ReadString(in);

                uint8 stage = 0, backend = 0;
                in.read(reinterpret_cast<char*>(&stage), sizeof(stage));
                in.read(reinterpret_cast<char*>(&backend), sizeof(backend));
                desc.stage = static_cast<RHIShaderStage>(stage);
                desc.backend = static_cast<RHIBackendType>(backend);

                desc.targetProfile = ReadString(in);

                uint8 flags = 0;
                in.read(reinterpret_cast<char*>(&flags), sizeof(flags));
                desc.enableDebugInfo = (flags & 0x01) != 0;
                desc.enableOptimization = (flags & 0x02) != 0;

                return desc;
            }
        }

        // =====================================================================
        // Public API
        // =====================================================================

        /** @brief Save permutation registry to file */
        bool SaveRegistry(
            const std::filesystem::path& path,
            const std::unordered_map<std::string, std::pair<ShaderPermutationSpace, ShaderPermutationLoadDesc>>& registry)
        {
            std::ofstream file(path, std::ios::binary | std::ios::trunc);
            if (!file.is_open())
            {
                RVX_CORE_ERROR("ShaderPermutationCache: Failed to open file for writing: {}", path.string());
                return false;
            }

            PermutationCacheHeader header;
            header.shaderCount = static_cast<uint32>(registry.size());
            file.write(reinterpret_cast<const char*>(&header), sizeof(header));

            for (const auto& [shaderPath, entry] : registry)
            {
                const auto& [space, desc] = entry;

                WriteString(file, shaderPath);

                // Write dimensions
                uint32 dimCount = static_cast<uint32>(space.dimensions.size());
                file.write(reinterpret_cast<const char*>(&dimCount), sizeof(dimCount));
                for (const auto& dim : space.dimensions)
                {
                    WriteDimension(file, dim);
                }

                // Write load desc
                WriteLoadDesc(file, desc);
            }

            RVX_CORE_DEBUG("ShaderPermutationCache: Saved {} shader registrations to {}",
                registry.size(), path.string());
            return true;
        }

        /** @brief Load permutation registry from file */
        bool LoadRegistry(
            const std::filesystem::path& path,
            std::unordered_map<std::string, std::pair<ShaderPermutationSpace, ShaderPermutationLoadDesc>>& outRegistry)
        {
            std::ifstream file(path, std::ios::binary);
            if (!file.is_open())
            {
                return false;
            }

            PermutationCacheHeader header;
            file.read(reinterpret_cast<char*>(&header), sizeof(header));

            if (header.magic != PERMUTATION_CACHE_MAGIC)
            {
                RVX_CORE_WARN("ShaderPermutationCache: Invalid magic number");
                return false;
            }

            if (header.version > PERMUTATION_CACHE_VERSION)
            {
                RVX_CORE_WARN("ShaderPermutationCache: Version mismatch");
                return false;
            }

            outRegistry.clear();
            for (uint32 i = 0; i < header.shaderCount; ++i)
            {
                std::string shaderPath = ReadString(file);

                ShaderPermutationSpace space;
                uint32 dimCount = 0;
                file.read(reinterpret_cast<char*>(&dimCount), sizeof(dimCount));
                space.dimensions.reserve(dimCount);
                for (uint32 j = 0; j < dimCount; ++j)
                {
                    space.dimensions.push_back(ReadDimension(file));
                }

                ShaderPermutationLoadDesc desc = ReadLoadDesc(file);

                outRegistry[shaderPath] = {std::move(space), std::move(desc)};
            }

            RVX_CORE_DEBUG("ShaderPermutationCache: Loaded {} shader registrations from {}",
                outRegistry.size(), path.string());
            return true;
        }

        /** @brief Get compiled variant count for persistence */
        struct VariantCacheInfo
        {
            uint64 variantHash;
            uint64 cacheKey;
        };

        /** @brief Save compiled variant list */
        bool SaveCompiledVariants(
            const std::filesystem::path& path,
            const std::string& shaderPath,
            const std::vector<VariantCacheInfo>& variants)
        {
            std::filesystem::path variantPath = path / (std::to_string(std::hash<std::string>{}(shaderPath)) + ".variants");
            std::ofstream file(variantPath, std::ios::binary | std::ios::trunc);
            if (!file.is_open())
            {
                return false;
            }

            uint32 count = static_cast<uint32>(variants.size());
            file.write(reinterpret_cast<const char*>(&count), sizeof(count));
            for (const auto& variant : variants)
            {
                file.write(reinterpret_cast<const char*>(&variant.variantHash), sizeof(variant.variantHash));
                file.write(reinterpret_cast<const char*>(&variant.cacheKey), sizeof(variant.cacheKey));
            }

            return true;
        }

        /** @brief Load compiled variant list */
        bool LoadCompiledVariants(
            const std::filesystem::path& path,
            const std::string& shaderPath,
            std::vector<VariantCacheInfo>& outVariants)
        {
            std::filesystem::path variantPath = path / (std::to_string(std::hash<std::string>{}(shaderPath)) + ".variants");
            std::ifstream file(variantPath, std::ios::binary);
            if (!file.is_open())
            {
                return false;
            }

            uint32 count = 0;
            file.read(reinterpret_cast<char*>(&count), sizeof(count));

            outVariants.clear();
            outVariants.reserve(count);
            for (uint32 i = 0; i < count; ++i)
            {
                VariantCacheInfo info;
                file.read(reinterpret_cast<char*>(&info.variantHash), sizeof(info.variantHash));
                file.read(reinterpret_cast<char*>(&info.cacheKey), sizeof(info.cacheKey));
                outVariants.push_back(info);
            }

            return true;
        }

    } // namespace ShaderPermutationCache

} // namespace RVX
