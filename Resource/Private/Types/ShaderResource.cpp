#include "Resource/Types/ShaderResource.h"
#include "Core/Diagnostics/JsonWriter.h"

#include <fstream>
#include <sstream>
#include <string_view>
#include <utility>

namespace RVX::Resource
{
namespace
{
    using Diagnostics::JsonBool;
    using Diagnostics::JsonString;

    constexpr std::uint64_t RVX_SHADER_CONTRACT_FNV_OFFSET = 14695981039346656037ull;
    constexpr std::uint64_t RVX_SHADER_CONTRACT_FNV_PRIME = 1099511628211ull;

    void HashByte(std::uint64_t& hash, std::uint8_t value)
    {
        hash ^= value;
        hash *= RVX_SHADER_CONTRACT_FNV_PRIME;
    }

    void HashBytes(std::uint64_t& hash, const std::uint8_t* data, size_t size)
    {
        if (!data || size == 0)
        {
            return;
        }

        for (size_t i = 0; i < size; ++i)
        {
            HashByte(hash, data[i]);
        }
    }

    void HashString(std::uint64_t& hash, std::string_view value)
    {
        HashBytes(hash,
                  reinterpret_cast<const std::uint8_t*>(value.data()),
                  value.size());
    }

    std::uint64_t ComputePayloadHash(const std::vector<std::uint8_t>& bytecode,
                                     const std::string& glslSource,
                                     const std::string& mslSource)
    {
        std::uint64_t hash = RVX_SHADER_CONTRACT_FNV_OFFSET;
        HashString(hash, "bytecode:");
        HashBytes(hash, bytecode.data(), bytecode.size());
        HashString(hash, "|glsl:");
        HashString(hash, glslSource);
        HashString(hash, "|msl:");
        HashString(hash, mslSource);
        return hash;
    }

    std::uint64_t ComputeStringHash(std::string_view value)
    {
        std::uint64_t hash = RVX_SHADER_CONTRACT_FNV_OFFSET;
        HashString(hash, value);
        return hash;
    }

    const char* GetShaderBackendContractName(ShaderBackendType backend)
    {
        switch (backend)
        {
            case ShaderBackendType::Auto: return "Auto";
            case ShaderBackendType::DX11: return "DX11";
            case ShaderBackendType::DX12: return "DX12";
            case ShaderBackendType::Vulkan: return "Vulkan";
            case ShaderBackendType::Metal: return "Metal";
            case ShaderBackendType::OpenGL: return "OpenGL";
            case ShaderBackendType::None:
            default: return "None";
        }
    }

    const char* GetShaderStageContractName(ShaderStage stage)
    {
        switch (stage)
        {
            case ShaderStage::Vertex: return "Vertex";
            case ShaderStage::Hull: return "Hull";
            case ShaderStage::Domain: return "Domain";
            case ShaderStage::Geometry: return "Geometry";
            case ShaderStage::Pixel: return "Pixel";
            case ShaderStage::Compute: return "Compute";
            case ShaderStage::Mesh: return "Mesh";
            case ShaderStage::Amplification: return "Amplification";
            case ShaderStage::RayGeneration: return "RayGeneration";
            case ShaderStage::AnyHit: return "AnyHit";
            case ShaderStage::ClosestHit: return "ClosestHit";
            case ShaderStage::Miss: return "Miss";
            case ShaderStage::Intersection: return "Intersection";
            case ShaderStage::Callable: return "Callable";
            case ShaderStage::None:
            default: return "None";
        }
    }

    const char* GetShaderRuntimeContractStatusName(ShaderRuntimeContractStatus status)
    {
        switch (status)
        {
            case ShaderRuntimeContractStatus::Valid: return "Valid";
            case ShaderRuntimeContractStatus::MissingRuntimePayload: return "MissingRuntimePayload";
            case ShaderRuntimeContractStatus::MissingSourcePath: return "MissingSourcePath";
            case ShaderRuntimeContractStatus::MissingEntryPoint: return "MissingEntryPoint";
            case ShaderRuntimeContractStatus::MissingTargetProfile: return "MissingTargetProfile";
            case ShaderRuntimeContractStatus::MissingBackend: return "MissingBackend";
            case ShaderRuntimeContractStatus::MissingStage: return "MissingStage";
            default: return "Unknown";
        }
    }


    void MarkContractInvalid(ShaderRuntimeContract& contract,
                             ShaderRuntimeContractStatus status,
                             const std::string& message)
    {
        contract.valid = false;
        contract.status = status;
        contract.diagnosticMessage = message;
    }
} // namespace

ShaderResource::ShaderResource() = default;
ShaderResource::~ShaderResource() = default;

void ShaderResource::SetData(std::vector<std::uint8_t> bytecode,
                             std::string glslSource,
                             std::string mslSource,
                             const ShaderMetadata& metadata)
{
    m_bytecode = std::move(bytecode);
    m_glslSource = std::move(glslSource);
    m_mslSource = std::move(mslSource);
    m_metadata = metadata;
    RebuildRuntimeContract();
}

size_t ShaderResource::GetMemoryUsage() const
{
    return sizeof(*this) +
           m_metadata.sourcePath.size() +
           m_metadata.entryPoint.size() +
           m_metadata.targetProfile.size() +
           m_runtimeContract.diagnosticMessage.size() +
           m_runtimeContract.cacheKey.size() +
           m_bytecode.size() +
           m_glslSource.size() +
           m_mslSource.size();
}

size_t ShaderResource::GetGPUMemoryUsage() const
{
    return 0;
}

void ShaderResource::RebuildRuntimeContract()
{
    ShaderRuntimeContract contract;
    contract.schemaVersion = RVX_SHADER_RUNTIME_CONTRACT_SCHEMA_VERSION;
    contract.sourceHash = m_metadata.sourceHash;
    contract.reflectionResourceCount = m_metadata.reflectionResourceCount;
    contract.bytecodeSize = static_cast<std::uint64_t>(m_bytecode.size());
    contract.glslSourceSize = static_cast<std::uint64_t>(m_glslSource.size());
    contract.mslSourceSize = static_cast<std::uint64_t>(m_mslSource.size());
    contract.payloadHash = ComputePayloadHash(m_bytecode, m_glslSource, m_mslSource);

    if (contract.bytecodeSize == 0 && contract.glslSourceSize == 0 && contract.mslSourceSize == 0)
    {
        MarkContractInvalid(contract,
                            ShaderRuntimeContractStatus::MissingRuntimePayload,
                            "Shader runtime contract has no bytecode or translated source payload.");
        m_runtimeContract = std::move(contract);
        return;
    }

    if (m_metadata.sourcePath.empty())
    {
        MarkContractInvalid(contract,
                            ShaderRuntimeContractStatus::MissingSourcePath,
                            "Shader runtime contract is missing source path metadata.");
        m_runtimeContract = std::move(contract);
        return;
    }

    if (m_metadata.entryPoint.empty())
    {
        MarkContractInvalid(contract,
                            ShaderRuntimeContractStatus::MissingEntryPoint,
                            "Shader runtime contract is missing entry point metadata.");
        m_runtimeContract = std::move(contract);
        return;
    }

    if (m_metadata.targetProfile.empty())
    {
        MarkContractInvalid(contract,
                            ShaderRuntimeContractStatus::MissingTargetProfile,
                            "Shader runtime contract is missing target profile metadata.");
        m_runtimeContract = std::move(contract);
        return;
    }

    if (m_metadata.backend == ShaderBackendType::None)
    {
        MarkContractInvalid(contract,
                            ShaderRuntimeContractStatus::MissingBackend,
                            "Shader runtime contract is missing backend metadata.");
        m_runtimeContract = std::move(contract);
        return;
    }

    if (m_metadata.stage == ShaderStage::None)
    {
        MarkContractInvalid(contract,
                            ShaderRuntimeContractStatus::MissingStage,
                            "Shader runtime contract is missing shader stage metadata.");
        m_runtimeContract = std::move(contract);
        return;
    }

    std::ostringstream key;
    key << "schema=" << contract.schemaVersion
        << "|metadataSchema=" << m_metadata.schemaVersion
        << "|source=" << m_metadata.sourcePath
        << "|backend=" << GetShaderBackendContractName(m_metadata.backend)
        << "|stage=" << GetShaderStageContractName(m_metadata.stage)
        << "|entry=" << m_metadata.entryPoint
        << "|targetProfile=" << m_metadata.targetProfile
        << "|sourceHash=" << contract.sourceHash
        << "|reflectionResources=" << contract.reflectionResourceCount
        << "|bytecodeSize=" << contract.bytecodeSize
        << "|glslSize=" << contract.glslSourceSize
        << "|mslSize=" << contract.mslSourceSize
        << "|payloadHash=" << contract.payloadHash;

    contract.cacheKey = key.str();
    contract.contractHash = ComputeStringHash(contract.cacheKey);
    contract.valid = true;
    contract.status = ShaderRuntimeContractStatus::Valid;
    contract.diagnosticMessage = "Shader runtime contract is valid.";
    m_runtimeContract = std::move(contract);
}

std::string ShaderResource::ExportRuntimeContractJson() const
{
    const ShaderRuntimeContract& contract = m_runtimeContract;

    std::ostringstream ss;
    ss << "{\n";
    ss << "  \"schemaVersion\": " << contract.schemaVersion << ",\n";
    ss << "  \"schemaId\": " << JsonString(RVX_SHADER_RUNTIME_CONTRACT_SCHEMA_ID) << ",\n";
    ss << "  \"id\": \"shaderRuntimeContractJson\",\n";
    ss << "  \"kind\": \"ShaderRuntimeContractJson\",\n";
    ss << "  \"contentType\": \"application/json\",\n";
    ss << "  \"resource\": {\n";
    ss << "    \"type\": \"Shader\",\n";
    ss << "    \"name\": " << JsonString(GetName()) << ",\n";
    ss << "    \"path\": " << JsonString(GetPath()) << "\n";
    ss << "  },\n";
    ss << "  \"metadata\": {\n";
    ss << "    \"schemaVersion\": " << m_metadata.schemaVersion << ",\n";
    ss << "    \"sourcePath\": " << JsonString(m_metadata.sourcePath) << ",\n";
    ss << "    \"backend\": " << JsonString(GetShaderBackendContractName(m_metadata.backend)) << ",\n";
    ss << "    \"stage\": " << JsonString(GetShaderStageContractName(m_metadata.stage)) << ",\n";
    ss << "    \"entryPoint\": " << JsonString(m_metadata.entryPoint) << ",\n";
    ss << "    \"targetProfile\": " << JsonString(m_metadata.targetProfile) << ",\n";
    ss << "    \"sourceHash\": " << m_metadata.sourceHash << ",\n";
    ss << "    \"reflectionResourceCount\": " << m_metadata.reflectionResourceCount << "\n";
    ss << "  },\n";
    ss << "  \"contract\": {\n";
    ss << "    \"valid\": " << JsonBool(contract.valid) << ",\n";
    ss << "    \"status\": " << JsonString(GetShaderRuntimeContractStatusName(contract.status)) << ",\n";
    ss << "    \"diagnosticMessage\": " << JsonString(contract.diagnosticMessage) << ",\n";
    ss << "    \"cacheKey\": " << JsonString(contract.cacheKey) << ",\n";
    ss << "    \"contractHash\": " << contract.contractHash << ",\n";
    ss << "    \"payloadHash\": " << contract.payloadHash << ",\n";
    ss << "    \"sourceHash\": " << contract.sourceHash << ",\n";
    ss << "    \"reflectionResourceCount\": " << contract.reflectionResourceCount << ",\n";
    ss << "    \"bytecodeSize\": " << contract.bytecodeSize << ",\n";
    ss << "    \"glslSourceSize\": " << contract.glslSourceSize << ",\n";
    ss << "    \"mslSourceSize\": " << contract.mslSourceSize << "\n";
    ss << "  }\n";
    ss << "}\n";
    return ss.str();
}

bool ShaderResource::SaveRuntimeContractJson(const char* filename) const
{
    if (!filename || filename[0] == '\0')
    {
        return false;
    }

    std::ofstream file(filename, std::ios::binary);
    if (!file.is_open())
    {
        return false;
    }

    file << ExportRuntimeContractJson();
    return file.good();
}

} // namespace RVX::Resource
