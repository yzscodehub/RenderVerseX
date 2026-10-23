/** @file CookedShaderProgramProvider.cpp @brief Strict v2 cooked shader loader */

#include "ShaderRuntime/CookedShaderProgramProvider.h"

#include "RHI/RHIDevice.h"

#include <system_error>
#include <utility>

namespace RVX
{
namespace
{
    bool IsSingleStage(RHIShaderStage stage) noexcept
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

    bool RequestsMatch(
        const ShaderProgramRequest& expected,
        const ShaderProgramRequest& actual) noexcept
    {
        return expected.logicalAssetId == actual.logicalAssetId &&
            expected.stage == actual.stage &&
            expected.entryPoint == actual.entryPoint &&
            expected.backend == actual.backend &&
            expected.defines == actual.defines;
    }

    class CookedShaderProgramProvider final
        : public IShaderProgramProvider
    {
    public:
        CookedShaderProgramProvider(
            std::filesystem::path artifactRoot,
            uint64 requiredCompilerAbi)
            : m_artifactRoot(std::move(artifactRoot)),
              m_requiredCompilerAbi(requiredCompilerAbi)
        {
        }

        ShaderProgramResult Load(
            IRHIDevice& device,
            const ShaderProgramRequest& request) override
        {
            ShaderProgramResult result;
            ShaderProgramRequest canonicalRequest = request;
            std::string requestError;
            if (request.logicalAssetId.empty() || request.entryPoint.empty() ||
                !IsSingleStage(request.stage) ||
                !IsConcreteBackend(request.backend) ||
                !CanonicalizeShaderMacros(
                    canonicalRequest.defines, requestError))
            {
                result.status = ShaderProgramStatus::InvalidRequest;
                result.diagnostic = requestError.empty()
                    ? "Cooked shader request is incomplete"
                    : std::move(requestError);
                return result;
            }

            if (device.GetBackendType() != canonicalRequest.backend)
            {
                result.status = ShaderProgramStatus::BackendMismatch;
                result.diagnostic =
                    "Cooked shader request backend does not match the device";
                return result;
            }

            const std::filesystem::path artifactPath =
                m_artifactRoot /
                BuildShaderArtifactRelativePath(canonicalRequest);
            std::error_code pathError;
            if (!std::filesystem::is_regular_file(
                    artifactPath, pathError))
            {
                result.status = ShaderProgramStatus::NotFound;
                result.diagnostic =
                    "Cooked shader artifact was not found: " +
                    artifactPath.generic_string();
                return result;
            }

            ShaderArtifact artifact;
            if (!ReadShaderArtifactV2(
                    artifactPath, artifact, result.diagnostic))
            {
                result.status = ShaderProgramStatus::ArtifactInvalid;
                return result;
            }
            if (!RequestsMatch(canonicalRequest, artifact.request))
            {
                result.status = ShaderProgramStatus::ArtifactInvalid;
                result.diagnostic =
                    "Cooked shader artifact identity does not match its path";
                return result;
            }
            if (artifact.compilerAbi != m_requiredCompilerAbi)
            {
                result.status = ShaderProgramStatus::IncompatibleArtifact;
                result.diagnostic =
                    "Cooked shader artifact compiler ABI is incompatible";
                return result;
            }

            RHIShaderInterface shaderInterface =
                BuildRHIShaderInterface(
                    artifact.request.stage, artifact.reflection);
            RHIShaderDesc desc;
            desc.stage = artifact.request.stage;
            desc.bytecode = artifact.code.data();
            desc.bytecodeSize = artifact.code.size();
            desc.entryPoint = artifact.runtimeEntryPoint.c_str();
            desc.debugName = artifact.request.logicalAssetId.c_str();
            desc.shaderInterface = &shaderInterface;
            result.shader = device.CreateShader(desc);
            if (!result.shader)
            {
                result.status = ShaderProgramStatus::DeviceCreationFailed;
                result.diagnostic =
                    "RHI rejected the cooked shader program";
                return result;
            }

            result.reflection = std::move(artifact.reflection);
            result.contentHash = artifact.contentHash;
            result.status = ShaderProgramStatus::Success;
            result.diagnostic.clear();
            return result;
        }

    private:
        std::filesystem::path m_artifactRoot;
        uint64 m_requiredCompilerAbi = 0;
    };
} // namespace

ShaderProgramProviderFactory CreateCookedShaderProgramProviderFactory(
    std::filesystem::path artifactRoot,
    uint64 requiredCompilerAbi)
{
    return [root = std::move(artifactRoot), requiredCompilerAbi]()
    {
        return std::make_unique<CookedShaderProgramProvider>(
            root, requiredCompilerAbi);
    };
}
} // namespace RVX
