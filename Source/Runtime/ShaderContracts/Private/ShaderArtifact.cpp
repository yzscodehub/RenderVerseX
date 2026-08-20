/** @file ShaderArtifact.cpp @brief Strict cooked shader artifact v2 codec */

#include "ShaderContracts/ShaderArtifact.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <set>
#include <span>
#include <sstream>
#include <system_error>
#include <utility>

namespace RVX
{
namespace
{
    constexpr uint64 FNV_OFFSET = 0xcbf29ce484222325ull;
    constexpr uint64 FNV_PRIME = 0x100000001b3ull;
    constexpr uint32 MAX_COLLECTION_COUNT = 1u << 20u;
    constexpr uint32 MAX_STRING_SIZE = 1u << 24u;
    constexpr uint64 MAX_ARTIFACT_SIZE = 1ull << 34u;

    void HashBytes(uint64& hash, const void* data, size_t size) noexcept
    {
        const auto* bytes = static_cast<const uint8*>(data);
        for (size_t index = 0; index < size; ++index)
        {
            hash ^= bytes[index];
            hash *= FNV_PRIME;
        }
    }

    void HashUint32(uint64& hash, uint32 value) noexcept
    {
        HashBytes(hash, &value, sizeof(value));
    }

    void HashString(uint64& hash, const std::string& value) noexcept
    {
        HashUint32(hash, static_cast<uint32>(value.size()));
        HashBytes(hash, value.data(), value.size());
    }

    bool IsSingleShaderStage(RHIShaderStage stage) noexcept
    {
        const uint32 value = static_cast<uint32>(stage);
        const uint32 all = static_cast<uint32>(RHIShaderStage::All);
        return value != 0 && (value & (value - 1u)) == 0 &&
            (value & ~all) == 0;
    }

    bool IsConcreteBackend(RHIBackendType backend) noexcept
    {
        return backend >= RHIBackendType::DX11 &&
            backend <= RHIBackendType::OpenGL;
    }

    const char* BackendIdentifier(RHIBackendType backend) noexcept
    {
        switch (backend)
        {
            case RHIBackendType::DX11: return "DX11";
            case RHIBackendType::DX12: return "DX12";
            case RHIBackendType::Vulkan: return "Vulkan";
            case RHIBackendType::Metal: return "Metal";
            case RHIBackendType::OpenGL: return "OpenGL";
            default: return "Unknown";
        }
    }

    bool IsCanonicalLogicalAssetId(
        const std::string& logicalAssetId) noexcept
    {
        if (logicalAssetId.empty() ||
            logicalAssetId.find('\\') != std::string::npos)
        {
            return false;
        }
        const std::filesystem::path path(logicalAssetId);
        if (path.is_absolute() || path.has_root_path())
        {
            return false;
        }
        for (const auto& component : path)
        {
            if (component == "." || component == ".." ||
                component.empty())
            {
                return false;
            }
        }
        return path.generic_string() == logicalAssetId;
    }

    class ByteWriter
    {
    public:
        void WriteU32(uint32 value)
        {
            WriteBytes(&value, sizeof(value));
        }

        void WriteString(const std::string& value)
        {
            WriteU32(static_cast<uint32>(value.size()));
            WriteBytes(value.data(), value.size());
        }

        void WriteBytes(const void* data, size_t size)
        {
            const size_t offset = bytes.size();
            bytes.resize(offset + size);
            if (size != 0)
            {
                std::memcpy(bytes.data() + offset, data, size);
            }
        }

        std::vector<uint8> bytes;
    };

    class ByteReader
    {
    public:
        explicit ByteReader(std::span<const uint8> bytes)
            : m_bytes(bytes)
        {
        }

        bool ReadU32(uint32& value)
        {
            return ReadBytes(&value, sizeof(value));
        }

        bool ReadString(std::string& value)
        {
            uint32 size = 0;
            if (!ReadU32(size) || size > MAX_STRING_SIZE ||
                size > Remaining())
            {
                return false;
            }
            value.assign(
                reinterpret_cast<const char*>(m_bytes.data() + m_offset),
                size);
            m_offset += size;
            return true;
        }

        bool ReadBytes(void* destination, size_t size)
        {
            if (size > Remaining())
            {
                return false;
            }
            if (size != 0)
            {
                std::memcpy(
                    destination, m_bytes.data() + m_offset, size);
            }
            m_offset += size;
            return true;
        }

        [[nodiscard]] size_t Remaining() const noexcept
        {
            return m_bytes.size() - m_offset;
        }

        [[nodiscard]] bool Finished() const noexcept
        {
            return m_offset == m_bytes.size();
        }

    private:
        std::span<const uint8> m_bytes;
        size_t m_offset = 0;
    };

    std::vector<uint8> SerializeIdentity(const ShaderArtifact& artifact)
    {
        ByteWriter writer;
        writer.WriteString(artifact.request.logicalAssetId);
        writer.WriteString(artifact.request.entryPoint);
        writer.WriteString(artifact.targetProfile);
        writer.WriteString(artifact.runtimeEntryPoint);
        return std::move(writer.bytes);
    }

    std::vector<uint8> SerializePermutation(const ShaderArtifact& artifact)
    {
        ByteWriter writer;
        writer.WriteU32(
            static_cast<uint32>(artifact.request.defines.size()));
        for (const ShaderMacro& define : artifact.request.defines)
        {
            writer.WriteString(define.name);
            writer.WriteString(define.value);
        }
        return std::move(writer.bytes);
    }

    void SerializeAttributes(
        ByteWriter& writer,
        const std::vector<ShaderReflection::InputAttribute>& attributes)
    {
        writer.WriteU32(static_cast<uint32>(attributes.size()));
        for (const ShaderReflection::InputAttribute& attribute : attributes)
        {
            writer.WriteString(attribute.semantic);
            writer.WriteU32(attribute.semanticIndex);
            writer.WriteU32(attribute.location);
            writer.WriteU32(static_cast<uint32>(attribute.format));
            writer.WriteU32(attribute.systemValue ? 1u : 0u);
        }
    }

    std::vector<uint8> SerializeReflection(
        const ShaderReflection& reflection)
    {
        ByteWriter writer;
        writer.WriteU32(reflection.valid ? 1u : 0u);
        writer.WriteU32(static_cast<uint32>(reflection.resources.size()));
        for (const ShaderReflection::ResourceBinding& resource :
             reflection.resources)
        {
            writer.WriteString(resource.name);
            writer.WriteU32(resource.set);
            writer.WriteU32(resource.binding);
            writer.WriteU32(static_cast<uint32>(resource.type));
            writer.WriteU32(resource.count);
        }
        writer.WriteU32(
            static_cast<uint32>(reflection.pushConstants.size()));
        for (const ShaderReflection::PushConstantRange& range :
             reflection.pushConstants)
        {
            writer.WriteU32(range.offset);
            writer.WriteU32(range.size);
        }
        SerializeAttributes(writer, reflection.inputs);
        SerializeAttributes(writer, reflection.outputs);
        return std::move(writer.bytes);
    }

    bool ReadCollectionCount(ByteReader& reader, uint32& count)
    {
        return reader.ReadU32(count) && count <= MAX_COLLECTION_COUNT;
    }

    bool DeserializeAttributes(
        ByteReader& reader,
        std::vector<ShaderReflection::InputAttribute>& attributes)
    {
        uint32 count = 0;
        if (!ReadCollectionCount(reader, count))
        {
            return false;
        }
        attributes.reserve(count);
        for (uint32 index = 0; index < count; ++index)
        {
            ShaderReflection::InputAttribute attribute;
            uint32 format = 0;
            uint32 systemValue = 0;
            if (!reader.ReadString(attribute.semantic) ||
                !reader.ReadU32(attribute.semanticIndex) ||
                !reader.ReadU32(attribute.location) ||
                !reader.ReadU32(format) ||
                !reader.ReadU32(systemValue) ||
                format >= static_cast<uint32>(RHIFormat::Count) ||
                systemValue > 1u)
            {
                return false;
            }
            attribute.format = static_cast<RHIFormat>(format);
            attribute.systemValue = systemValue != 0;
            attributes.push_back(std::move(attribute));
        }
        return true;
    }

    bool DeserializeReflection(
        std::span<const uint8> bytes,
        ShaderReflection& reflection)
    {
        ByteReader reader(bytes);
        uint32 valid = 0;
        uint32 resourceCount = 0;
        if (!reader.ReadU32(valid) || valid > 1u ||
            !ReadCollectionCount(reader, resourceCount))
        {
            return false;
        }
        reflection.valid = valid != 0;
        reflection.resources.reserve(resourceCount);
        for (uint32 index = 0; index < resourceCount; ++index)
        {
            ShaderReflection::ResourceBinding resource;
            uint32 type = 0;
            if (!reader.ReadString(resource.name) ||
                !reader.ReadU32(resource.set) ||
                !reader.ReadU32(resource.binding) ||
                !reader.ReadU32(type) ||
                !reader.ReadU32(resource.count) ||
                type > static_cast<uint32>(
                    RHIBindingType::AccelerationStructure) ||
                resource.count == 0)
            {
                return false;
            }
            resource.type = static_cast<RHIBindingType>(type);
            reflection.resources.push_back(std::move(resource));
        }

        uint32 pushConstantCount = 0;
        if (!ReadCollectionCount(reader, pushConstantCount))
        {
            return false;
        }
        reflection.pushConstants.reserve(pushConstantCount);
        for (uint32 index = 0; index < pushConstantCount; ++index)
        {
            ShaderReflection::PushConstantRange range;
            if (!reader.ReadU32(range.offset) ||
                !reader.ReadU32(range.size) || range.size == 0)
            {
                return false;
            }
            reflection.pushConstants.push_back(range);
        }
        return DeserializeAttributes(reader, reflection.inputs) &&
            DeserializeAttributes(reader, reflection.outputs) &&
            reader.Finished();
    }

    bool DeserializeIdentity(
        std::span<const uint8> bytes,
        ShaderArtifact& artifact)
    {
        ByteReader reader(bytes);
        return reader.ReadString(artifact.request.logicalAssetId) &&
            reader.ReadString(artifact.request.entryPoint) &&
            reader.ReadString(artifact.targetProfile) &&
            reader.ReadString(artifact.runtimeEntryPoint) &&
            reader.Finished();
    }

    bool DeserializePermutation(
        std::span<const uint8> bytes,
        std::vector<ShaderMacro>& defines)
    {
        ByteReader reader(bytes);
        uint32 count = 0;
        if (!ReadCollectionCount(reader, count))
        {
            return false;
        }
        defines.reserve(count);
        for (uint32 index = 0; index < count; ++index)
        {
            ShaderMacro define;
            if (!reader.ReadString(define.name) ||
                !reader.ReadString(define.value))
            {
                return false;
            }
            defines.push_back(std::move(define));
        }
        return reader.Finished();
    }

    bool ValidateArtifactIdentity(
        ShaderArtifact& artifact,
        std::string& outError)
    {
        const auto stringFits = [](const std::string& value)
        {
            return value.size() <= MAX_STRING_SIZE;
        };
        const auto attributesFit = [&stringFits](
            const std::vector<ShaderReflection::InputAttribute>& attributes)
        {
            return attributes.size() <= MAX_COLLECTION_COUNT &&
                std::all_of(
                    attributes.begin(), attributes.end(),
                    [&stringFits](
                        const ShaderReflection::InputAttribute& attribute)
                    {
                        return stringFits(attribute.semantic);
                    });
        };
        if (!IsConcreteBackend(artifact.request.backend) ||
            !IsSingleShaderStage(artifact.request.stage))
        {
            outError = "Shader artifact requires one concrete backend and stage";
            return false;
        }
        if (!IsCanonicalLogicalAssetId(artifact.request.logicalAssetId))
        {
            outError = "Shader artifact logical asset ID is not canonical";
            return false;
        }
        if (artifact.request.entryPoint.empty() ||
            artifact.targetProfile.empty() ||
            artifact.runtimeEntryPoint.empty())
        {
            outError =
                "Shader artifact entry point, runtime entry point, or target profile is empty";
            return false;
        }
        if (!stringFits(artifact.request.logicalAssetId) ||
            !stringFits(artifact.request.entryPoint) ||
            !stringFits(artifact.targetProfile) ||
            !stringFits(artifact.runtimeEntryPoint) ||
            artifact.request.defines.size() > MAX_COLLECTION_COUNT ||
            artifact.reflection.resources.size() > MAX_COLLECTION_COUNT ||
            artifact.reflection.pushConstants.size() > MAX_COLLECTION_COUNT ||
            !attributesFit(artifact.reflection.inputs) ||
            !attributesFit(artifact.reflection.outputs) ||
            !std::all_of(
                artifact.request.defines.begin(),
                artifact.request.defines.end(),
                [&stringFits](const ShaderMacro& define)
                {
                    return stringFits(define.name) &&
                        stringFits(define.value);
                }) ||
            !std::all_of(
                artifact.reflection.resources.begin(),
                artifact.reflection.resources.end(),
                [&stringFits](
                    const ShaderReflection::ResourceBinding& resource)
                {
                    return stringFits(resource.name);
                }))
        {
            outError =
                "Shader artifact metadata exceeds serialization limits";
            return false;
        }
        if (!CanonicalizeShaderMacros(
                artifact.request.defines, outError))
        {
            return false;
        }
        if (!artifact.reflection.valid || artifact.code.empty() ||
            artifact.compilerAbi == 0 || artifact.contentHash == 0)
        {
            outError =
                "Shader artifact lacks code, reflection, ABI, or content hash";
            return false;
        }
        const uint64 expectedPermutation =
            ComputeShaderPermutationHash(artifact.request);
        if (artifact.permutationHash == 0)
        {
            artifact.permutationHash = expectedPermutation;
        }
        if (artifact.permutationHash != expectedPermutation)
        {
            outError = "Shader artifact permutation hash is inconsistent";
            return false;
        }
        return true;
    }

    bool ValidateSection(
        const ShaderArtifactSection& section,
        uint64 expectedOffset,
        uint64 totalSize,
        std::string_view name,
        std::string& outError)
    {
        if (section.offset != expectedOffset || section.size == 0 ||
            section.offset > totalSize ||
            section.size > totalSize - section.offset)
        {
            outError = "Shader artifact has invalid " +
                std::string(name) + " section bounds";
            return false;
        }
        return true;
    }

    std::span<const uint8> SectionBytes(
        const std::vector<uint8>& bytes,
        const ShaderArtifactSection& section)
    {
        return std::span<const uint8>(
            bytes.data() + static_cast<size_t>(section.offset),
            static_cast<size_t>(section.size));
    }

    bool ReadAllBytes(
        const std::filesystem::path& path,
        std::vector<uint8>& bytes,
        std::string& outError)
    {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file.is_open())
        {
            outError = "Cannot open shader artifact: " + path.string();
            return false;
        }
        const std::streamsize size = file.tellg();
        if (size <= 0 || static_cast<uint64>(size) > MAX_ARTIFACT_SIZE)
        {
            outError = "Shader artifact size is invalid";
            return false;
        }
        bytes.resize(static_cast<size_t>(size));
        file.seekg(0, std::ios::beg);
        if (!file.read(
                reinterpret_cast<char*>(bytes.data()), size))
        {
            outError = "Cannot read complete shader artifact";
            return false;
        }
        return true;
    }
} // namespace

bool CanonicalizeShaderMacros(
    std::vector<ShaderMacro>& defines,
    std::string& outError)
{
    std::sort(
        defines.begin(), defines.end(),
        [](const ShaderMacro& left, const ShaderMacro& right)
        {
            return left.name < right.name ||
                (left.name == right.name && left.value < right.value);
        });
    std::set<std::string> names;
    for (const ShaderMacro& define : defines)
    {
        if (define.name.empty() ||
            !names.insert(define.name).second)
        {
            outError =
                "Shader permutation has an empty or duplicate macro name";
            return false;
        }
    }
    outError.clear();
    return true;
}

uint64 ComputeShaderPermutationHash(
    const ShaderProgramRequest& request)
{
    std::vector<ShaderMacro> defines = request.defines;
    std::string error;
    if (!CanonicalizeShaderMacros(defines, error))
    {
        return 0;
    }
    uint64 hash = FNV_OFFSET;
    HashString(hash, request.logicalAssetId);
    HashString(hash, request.entryPoint);
    HashUint32(hash, static_cast<uint32>(request.backend));
    HashUint32(hash, static_cast<uint32>(request.stage));
    HashUint32(hash, static_cast<uint32>(defines.size()));
    for (const ShaderMacro& define : defines)
    {
        HashString(hash, define.name);
        HashString(hash, define.value);
    }
    return hash;
}

std::filesystem::path BuildShaderArtifactRelativePath(
    const ShaderProgramRequest& request)
{
    const uint64 hash = ComputeShaderPermutationHash(request);
    std::ostringstream fileName;
    fileName << std::hex << std::setfill('0') << std::setw(16)
             << hash << ".rvxshader";
    return std::filesystem::path(BackendIdentifier(request.backend)) /
        fileName.str();
}

bool WriteShaderArtifactV2(
    const std::filesystem::path& path,
    ShaderArtifact artifact,
    std::string& outError)
{
    if (!ValidateArtifactIdentity(artifact, outError))
    {
        return false;
    }

    const std::vector<uint8> identity = SerializeIdentity(artifact);
    const std::vector<uint8> permutation =
        SerializePermutation(artifact);
    const std::vector<uint8> reflection =
        SerializeReflection(artifact.reflection);

    ShaderArtifactHeaderV2 header;
    header.headerSize = sizeof(header);
    header.backend = static_cast<uint32>(artifact.request.backend);
    header.stage = static_cast<uint32>(artifact.request.stage);
    header.compilerAbi = artifact.compilerAbi;
    header.permutationHash = artifact.permutationHash;
    header.contentHash = artifact.contentHash;

    uint64 offset = sizeof(header);
    const auto assignSection = [&offset](
        ShaderArtifactSection& section,
        size_t size)
    {
        section.offset = offset;
        section.size = static_cast<uint64>(size);
        offset += section.size;
    };
    assignSection(header.identity, identity.size());
    assignSection(header.permutation, permutation.size());
    assignSection(header.code, artifact.code.size());
    assignSection(header.reflection, reflection.size());
    header.totalSize = offset;
    if (header.totalSize > MAX_ARTIFACT_SIZE)
    {
        outError = "Shader artifact exceeds the maximum supported size";
        return false;
    }

    uint64 payloadHash = FNV_OFFSET;
    HashBytes(payloadHash, &header, sizeof(header));
    HashBytes(payloadHash, identity.data(), identity.size());
    HashBytes(payloadHash, permutation.data(), permutation.size());
    HashBytes(payloadHash, artifact.code.data(), artifact.code.size());
    HashBytes(payloadHash, reflection.data(), reflection.size());
    header.payloadHash = payloadHash;

    std::error_code directoryError;
    if (!path.parent_path().empty())
    {
        std::filesystem::create_directories(
            path.parent_path(), directoryError);
    }
    if (directoryError)
    {
        outError = "Cannot create shader artifact directory: " +
            directoryError.message();
        return false;
    }
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file.is_open())
    {
        outError = "Cannot open shader artifact for writing: " +
            path.string();
        return false;
    }
    const auto write = [&file](const void* data, size_t size)
    {
        if (size != 0)
        {
            file.write(
                static_cast<const char*>(data),
                static_cast<std::streamsize>(size));
        }
    };
    write(&header, sizeof(header));
    write(identity.data(), identity.size());
    write(permutation.data(), permutation.size());
    write(artifact.code.data(), artifact.code.size());
    write(reflection.data(), reflection.size());
    if (!file.good())
    {
        outError = "Failed while writing shader artifact: " +
            path.string();
        return false;
    }
    outError.clear();
    return true;
}

bool ReadShaderArtifactV2(
    const std::filesystem::path& path,
    ShaderArtifact& outArtifact,
    std::string& outError)
{
    std::vector<uint8> bytes;
    if (!ReadAllBytes(path, bytes, outError))
    {
        return false;
    }
    if (bytes.size() < sizeof(ShaderArtifactHeaderV2))
    {
        outError = "Shader artifact is smaller than the v2 header";
        return false;
    }
    ShaderArtifactHeaderV2 header;
    std::memcpy(&header, bytes.data(), sizeof(header));
    if (header.magic != RVX_SHADER_ARTIFACT_MAGIC_V2 ||
        header.version != RVX_SHADER_ARTIFACT_VERSION ||
        header.endianMarker != RVX_SHADER_ARTIFACT_ENDIAN_MARKER ||
        header.headerSize != sizeof(ShaderArtifactHeaderV2))
    {
        outError = "Shader artifact is not strict RVX shader v2";
        return false;
    }
    if (header.flags != 0 ||
        std::any_of(
            header.reserved.begin(), header.reserved.end(),
            [](uint64 value) { return value != 0; }))
    {
        outError = "Shader artifact uses unsupported flags or reserved data";
        return false;
    }
    if (header.totalSize != bytes.size() ||
        header.totalSize > MAX_ARTIFACT_SIZE)
    {
        outError = "Shader artifact total size is inconsistent";
        return false;
    }
    uint64 expectedOffset = sizeof(header);
    if (!ValidateSection(
            header.identity, expectedOffset, header.totalSize,
            "identity", outError))
    {
        return false;
    }
    expectedOffset += header.identity.size;
    if (!ValidateSection(
            header.permutation, expectedOffset, header.totalSize,
            "permutation", outError))
    {
        return false;
    }
    expectedOffset += header.permutation.size;
    if (!ValidateSection(
            header.code, expectedOffset, header.totalSize,
            "code", outError))
    {
        return false;
    }
    expectedOffset += header.code.size;
    if (!ValidateSection(
            header.reflection, expectedOffset, header.totalSize,
            "reflection", outError) ||
        header.reflection.offset + header.reflection.size !=
            header.totalSize)
    {
        return false;
    }

    ShaderArtifactHeaderV2 hashHeader = header;
    hashHeader.payloadHash = 0;
    uint64 payloadHash = FNV_OFFSET;
    HashBytes(payloadHash, &hashHeader, sizeof(hashHeader));
    HashBytes(
        payloadHash,
        bytes.data() + sizeof(header),
        bytes.size() - sizeof(header));
    if (payloadHash != header.payloadHash)
    {
        outError = "Shader artifact payload hash mismatch";
        return false;
    }

    ShaderArtifact artifact;
    artifact.request.backend =
        static_cast<RHIBackendType>(header.backend);
    artifact.request.stage =
        static_cast<RHIShaderStage>(header.stage);
    artifact.compilerAbi = header.compilerAbi;
    artifact.permutationHash = header.permutationHash;
    artifact.contentHash = header.contentHash;
    artifact.payloadHash = header.payloadHash;
    if (!DeserializeIdentity(
            SectionBytes(bytes, header.identity), artifact) ||
        !DeserializePermutation(
            SectionBytes(bytes, header.permutation),
            artifact.request.defines) ||
        !DeserializeReflection(
            SectionBytes(bytes, header.reflection),
            artifact.reflection))
    {
        outError = "Shader artifact section payload is malformed";
        return false;
    }
    const std::span<const uint8> code =
        SectionBytes(bytes, header.code);
    artifact.code.assign(code.begin(), code.end());
    if (!ValidateArtifactIdentity(artifact, outError))
    {
        return false;
    }
    outArtifact = std::move(artifact);
    outError.clear();
    return true;
}
} // namespace RVX
