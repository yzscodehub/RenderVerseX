#include "Resource/Loader/ShaderLoader.h"

#include "Core/Log.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <sstream>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace RVX::Resource
{
namespace
{
    using FieldMap = std::unordered_map<std::string, std::string>;

    std::vector<std::uint8_t> ReadFileBytes(const std::string& path, std::string& outError)
    {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file.is_open())
        {
            outError = "Cannot open shader artifact: " + path;
            return {};
        }

        const std::streamsize size = file.tellg();
        if (size <= 0)
        {
            outError = "Shader artifact is empty: " + path;
            return {};
        }

        file.seekg(0, std::ios::beg);
        std::vector<std::uint8_t> bytes(static_cast<size_t>(size));
        if (!file.read(reinterpret_cast<char*>(bytes.data()), size))
        {
            outError = "Failed to read shader artifact: " + path;
            return {};
        }

        return bytes;
    }

    std::string ToLower(std::string value)
    {
        std::transform(value.begin(),
                       value.end(),
                       value.begin(),
                       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        return value;
    }

    FieldMap ParseFields(std::string_view text)
    {
        FieldMap fields;
        std::istringstream stream{std::string(text)};
        std::string line;
        while (std::getline(stream, line))
        {
            const size_t separator = line.find('=');
            if (separator == std::string::npos)
            {
                continue;
            }
            fields[line.substr(0, separator)] = line.substr(separator + 1);
        }
        return fields;
    }

    std::optional<size_t> ParseSize(const FieldMap& fields, const std::string& key)
    {
        auto it = fields.find(key);
        if (it == fields.end())
        {
            return std::nullopt;
        }

        try
        {
            return static_cast<size_t>(std::stoull(it->second));
        }
        catch (...)
        {
            return std::nullopt;
        }
    }

    std::optional<std::uint64_t> ParseUint64(const FieldMap& fields, const std::string& key)
    {
        auto it = fields.find(key);
        if (it == fields.end())
        {
            return std::nullopt;
        }

        try
        {
            return static_cast<std::uint64_t>(std::stoull(it->second));
        }
        catch (...)
        {
            return std::nullopt;
        }
    }

    std::optional<std::uint32_t> ParseUint32(const FieldMap& fields, const std::string& key)
    {
        auto parsed = ParseUint64(fields, key);
        if (!parsed || *parsed > static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()))
        {
            return std::nullopt;
        }
        return static_cast<std::uint32_t>(*parsed);
    }

    std::optional<std::string> ParseString(const FieldMap& fields, const std::string& key)
    {
        auto it = fields.find(key);
        if (it == fields.end())
        {
            return std::nullopt;
        }
        return it->second;
    }

    std::optional<ShaderBackendType> ParseBackend(const std::string& value)
    {
        const std::string backend = ToLower(value);
        if (backend == "auto")
        {
            return ShaderBackendType::Auto;
        }
        if (backend == "directx 11" || backend == "dx11")
        {
            return ShaderBackendType::DX11;
        }
        if (backend == "directx 12" || backend == "dx12")
        {
            return ShaderBackendType::DX12;
        }
        if (backend == "vulkan")
        {
            return ShaderBackendType::Vulkan;
        }
        if (backend == "metal")
        {
            return ShaderBackendType::Metal;
        }
        if (backend == "opengl")
        {
            return ShaderBackendType::OpenGL;
        }
        return std::nullopt;
    }

    std::optional<ShaderStage> ParseShaderStage(const std::string& value)
    {
        const std::string stage = ToLower(value);
        if (stage == "vertex")
        {
            return ShaderStage::Vertex;
        }
        if (stage == "hull")
        {
            return ShaderStage::Hull;
        }
        if (stage == "domain")
        {
            return ShaderStage::Domain;
        }
        if (stage == "geometry")
        {
            return ShaderStage::Geometry;
        }
        if (stage == "pixel")
        {
            return ShaderStage::Pixel;
        }
        if (stage == "compute")
        {
            return ShaderStage::Compute;
        }
        if (stage == "mesh")
        {
            return ShaderStage::Mesh;
        }
        if (stage == "amplification")
        {
            return ShaderStage::Amplification;
        }
        if (stage == "raygeneration" || stage == "ray generation")
        {
            return ShaderStage::RayGeneration;
        }
        if (stage == "anyhit" || stage == "any hit")
        {
            return ShaderStage::AnyHit;
        }
        if (stage == "closesthit" || stage == "closest hit")
        {
            return ShaderStage::ClosestHit;
        }
        if (stage == "miss")
        {
            return ShaderStage::Miss;
        }
        if (stage == "intersection")
        {
            return ShaderStage::Intersection;
        }
        if (stage == "callable")
        {
            return ShaderStage::Callable;
        }
        return std::nullopt;
    }

    bool HasMarkerAt(std::string_view text, size_t offset, std::string_view marker)
    {
        return offset <= text.size() &&
               marker.size() <= text.size() - offset &&
               text.substr(offset, marker.size()) == marker;
    }

    bool ParseShaderArtifact(const std::vector<std::uint8_t>& bytes,
                             ShaderMetadata& outMetadata,
                             std::vector<std::uint8_t>& outBytecode,
                             std::string& outGLSL,
                             std::string& outMSL,
                             std::string& outError)
    {
        constexpr std::string_view magic = "RVX_SHADER_PREBAKE_V1\n";
        constexpr std::string_view bytecodeMarker = "RVX_SHADER_BYTECODE_BEGIN\n";
        constexpr std::string_view glslMarker = "\nRVX_SHADER_GLSL_BEGIN\n";
        constexpr std::string_view mslMarker = "\nRVX_SHADER_MSL_BEGIN\n";
        constexpr std::string_view endMarker = "\nRVX_SHADER_PREBAKE_END\n";

        const std::string_view text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        if (!text.starts_with(magic))
        {
            outError = "Shader artifact missing RVX_SHADER_PREBAKE_V1 magic";
            return false;
        }

        const size_t bytecodeMarkerOffset = text.find(bytecodeMarker);
        if (bytecodeMarkerOffset == std::string_view::npos)
        {
            outError = "Shader artifact missing bytecode marker";
            return false;
        }

        const FieldMap fields = ParseFields(text.substr(0, bytecodeMarkerOffset));
        const auto bytecodeSize = ParseSize(fields, "bytecodeSize");
        const auto glslSize = ParseSize(fields, "glslSize");
        const auto mslSize = ParseSize(fields, "mslSize");
        const auto source = ParseString(fields, "source");
        const auto backendText = ParseString(fields, "backend");
        const auto stageText = ParseString(fields, "stage");
        const auto entry = ParseString(fields, "entry");
        const auto targetProfile = ParseString(fields, "targetProfile");
        const auto reflectionResources = ParseUint32(fields, "reflectionResources");
        const auto sourceHash = ParseUint64(fields, "sourceHash");

        if (!bytecodeSize || !glslSize || !mslSize || !source || !backendText ||
            !stageText || !entry || !targetProfile || !reflectionResources || !sourceHash)
        {
            outError = "Shader artifact metadata is incomplete";
            return false;
        }

        const auto backend = ParseBackend(*backendText);
        const auto stage = ParseShaderStage(*stageText);
        if (!backend)
        {
            outError = "Shader artifact has unsupported backend: " + *backendText;
            return false;
        }
        if (!stage)
        {
            outError = "Shader artifact has unsupported shader stage: " + *stageText;
            return false;
        }

        const size_t bytecodeOffset = bytecodeMarkerOffset + bytecodeMarker.size();
        if (*bytecodeSize > text.size() - bytecodeOffset)
        {
            outError = "Shader bytecode payload is truncated";
            return false;
        }

        const size_t glslMarkerOffset = bytecodeOffset + *bytecodeSize;
        if (!HasMarkerAt(text, glslMarkerOffset, glslMarker))
        {
            outError = "Shader artifact missing GLSL marker at expected bytecode boundary";
            return false;
        }

        const size_t glslOffset = glslMarkerOffset + glslMarker.size();
        if (*glslSize > text.size() - glslOffset)
        {
            outError = "Shader GLSL payload is truncated";
            return false;
        }

        const size_t mslMarkerOffset = glslOffset + *glslSize;
        if (!HasMarkerAt(text, mslMarkerOffset, mslMarker))
        {
            outError = "Shader artifact missing MSL marker at expected GLSL boundary";
            return false;
        }

        const size_t mslOffset = mslMarkerOffset + mslMarker.size();
        if (*mslSize > text.size() - mslOffset)
        {
            outError = "Shader MSL payload is truncated";
            return false;
        }

        const size_t endMarkerOffset = mslOffset + *mslSize;
        if (!HasMarkerAt(text, endMarkerOffset, endMarker))
        {
            outError = "Shader artifact missing end marker at expected MSL boundary";
            return false;
        }

        if (*bytecodeSize == 0 && *glslSize == 0 && *mslSize == 0)
        {
            outError = "Shader artifact contains no runtime payload";
            return false;
        }

        outMetadata = {};
        outMetadata.sourcePath = *source;
        outMetadata.backend = *backend;
        outMetadata.stage = *stage;
        outMetadata.entryPoint = *entry;
        outMetadata.targetProfile = *targetProfile;
        outMetadata.reflectionResourceCount = *reflectionResources;
        outMetadata.sourceHash = *sourceHash;

        outBytecode.assign(bytes.begin() + static_cast<std::ptrdiff_t>(bytecodeOffset),
                           bytes.begin() + static_cast<std::ptrdiff_t>(bytecodeOffset + *bytecodeSize));
        outGLSL.assign(text.substr(glslOffset, *glslSize));
        outMSL.assign(text.substr(mslOffset, *mslSize));
        return true;
    }
} // namespace

ShaderLoader::ShaderLoader(ResourceManager* manager)
    : m_manager(manager)
{
}

std::vector<std::string> ShaderLoader::GetSupportedExtensions() const
{
    return { ".rva" };
}

bool ShaderLoader::CanLoad(const std::string& path) const
{
    std::filesystem::path filePath(path);
    std::string ext = filePath.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

    auto extensions = GetSupportedExtensions();
    return std::find(extensions.begin(), extensions.end(), ext) != extensions.end();
}

IResource* ShaderLoader::Load(const std::string& path)
{
    (void)m_manager;

    std::filesystem::path absPath = std::filesystem::absolute(path);
    m_lastLoadError.clear();

    std::vector<std::uint8_t> bytes = ReadFileBytes(absPath.string(), m_lastLoadError);
    if (bytes.empty())
    {
        RVX_CORE_ERROR("ShaderLoader: {}", m_lastLoadError);
        return nullptr;
    }

    ShaderMetadata metadata;
    std::vector<std::uint8_t> bytecode;
    std::string glslSource;
    std::string mslSource;
    if (!ParseShaderArtifact(bytes, metadata, bytecode, glslSource, mslSource, m_lastLoadError))
    {
        RVX_CORE_ERROR("ShaderLoader: {}", m_lastLoadError);
        return nullptr;
    }

    auto* resource = new ShaderResource();
    resource->SetData(std::move(bytecode), std::move(glslSource), std::move(mslSource), metadata);
    resource->SetId(GenerateResourceId(path));
    resource->SetPath(path);
    resource->SetName(absPath.stem().string());
    resource->NotifyLoaded();

    return resource;
}

} // namespace RVX::Resource
