#include "RHI/RHI.h"
#include "ShaderContracts/ShaderArtifact.h"
#include "ShaderRuntime/CookedShaderProgramProvider.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <vector>

namespace
{
    namespace fs = std::filesystem;

    class TemporaryDirectory
    {
    public:
        explicit TemporaryDirectory(const char* name)
        {
            const auto stamp = std::chrono::steady_clock::now()
                .time_since_epoch().count();
            m_path = fs::temp_directory_path() /
                (std::string(name) + "_" + std::to_string(stamp));
            std::error_code error;
            fs::create_directories(m_path, error);
            EXPECT_FALSE(error) << error.message();
        }

        ~TemporaryDirectory()
        {
            std::error_code error;
            fs::remove_all(m_path, error);
        }

        const fs::path& Path() const noexcept { return m_path; }

    private:
        fs::path m_path;
    };

    RVX::ShaderArtifact MakeArtifact()
    {
        RVX::ShaderArtifact artifact;
        artifact.request.logicalAssetId = "DefaultLit.hlsl";
        artifact.request.stage = RVX::RHIShaderStage::Vertex;
        artifact.request.entryPoint = "VSMain";
        artifact.request.backend = RVX::RHIBackendType::DX12;
        artifact.request.defines = {
            {"RVX_SKINNED", "1"},
            {"RVX_ALPHA_MODE", "2"},
        };
        artifact.targetProfile = "vs_6_6";
        artifact.runtimeEntryPoint = "VSMain";
        artifact.contentHash = 0x0123456789abcdefull;
        artifact.code = {'D', 'X', 'I', 'L'};
        artifact.reflection.valid = true;
        artifact.reflection.resources = {
            {"Frame", 0, 0, RVX::RHIBindingType::UniformBuffer, 1},
            {"Textures", 1, 4, RVX::RHIBindingType::SampledTexture, 8},
        };
        artifact.reflection.pushConstants = {{0, 32}};
        artifact.reflection.inputs = {
            {"POSITION", 0, 0, RVX::RHIFormat::RGB32_FLOAT, false},
        };
        artifact.reflection.outputs = {
            {"SV_POSITION", 0, RVX::RVX_INVALID_INDEX,
             RVX::RHIFormat::RGBA32_FLOAT, true},
        };
        return artifact;
    }

    class CapturedShader final : public RVX::RHIShader
    {
    public:
        explicit CapturedShader(const RVX::RHIShaderDesc& desc)
            : RHIShader(desc), m_stage(desc.stage)
        {
            if (desc.bytecode != nullptr && desc.bytecodeSize != 0)
            {
                const auto* bytes =
                    static_cast<const RVX::uint8*>(desc.bytecode);
                m_bytecode.assign(bytes, bytes + desc.bytecodeSize);
            }
        }

        RVX::RHIShaderStage GetStage() const override { return m_stage; }
        const std::vector<RVX::uint8>& GetBytecode() const override
        {
            return m_bytecode;
        }

    private:
        RVX::RHIShaderStage m_stage = RVX::RHIShaderStage::None;
        std::vector<RVX::uint8> m_bytecode;
    };

    class CapturingDevice final : public RVX::IRHIDevice
    {
    public:
        explicit CapturingDevice(RVX::RHIBackendType backend)
            : m_backend(backend)
        {
        }

        RVX::RHIShaderRef CreateShader(
            const RVX::RHIShaderDesc& desc) override
        {
            ++createShaderCalls;
            return RVX::MakeRef<CapturedShader>(desc);
        }

        RVX::RHIBufferRef CreateBuffer(const RVX::RHIBufferDesc&) override { return {}; }
        RVX::RHITextureRef CreateTexture(const RVX::RHITextureDesc&) override { return {}; }
        RVX::RHITextureViewRef CreateTextureView(RVX::RHITexture*, const RVX::RHITextureViewDesc&) override { return {}; }
        RVX::RHISamplerRef CreateSampler(const RVX::RHISamplerDesc&) override { return {}; }
        RVX::RHIHeapRef CreateHeap(const RVX::RHIHeapDesc&) override { return {}; }
        RVX::RHITextureRef CreatePlacedTexture(RVX::RHIHeap*, RVX::uint64, const RVX::RHITextureDesc&) override { return {}; }
        RVX::RHIBufferRef CreatePlacedBuffer(RVX::RHIHeap*, RVX::uint64, const RVX::RHIBufferDesc&) override { return {}; }
        MemoryRequirements GetTextureMemoryRequirements(const RVX::RHITextureDesc&) override { return {}; }
        MemoryRequirements GetBufferMemoryRequirements(const RVX::RHIBufferDesc&) override { return {}; }
        RVX::RHIDescriptorSetLayoutRef CreateDescriptorSetLayout(const RVX::RHIDescriptorSetLayoutDesc&) override { return {}; }
        RVX::RHIPipelineLayoutRef CreatePipelineLayout(const RVX::RHIPipelineLayoutDesc&) override { return {}; }
        RVX::RHIPipelineRef CreateGraphicsPipeline(const RVX::RHIGraphicsPipelineDesc&) override { return {}; }
        RVX::RHIPipelineRef CreateComputePipeline(const RVX::RHIComputePipelineDesc&) override { return {}; }
        RVX::RHIDescriptorSetRef CreateDescriptorSet(const RVX::RHIDescriptorSetDesc&) override { return {}; }
        RVX::RHIQueryPoolRef CreateQueryPool(const RVX::RHIQueryPoolDesc&) override { return {}; }
        RVX::RHICommandContextRef CreateCommandContext(RVX::RHICommandQueueType) override { return {}; }
        RVX::uint64 SubmitCommandContext(RVX::RHICommandContext*, RVX::RHIFence*) override { return 0; }
        RVX::uint64 SubmitCommandContexts(std::span<RVX::RHICommandContext* const>, RVX::RHIFence*) override { return 0; }
        RVX::RHISwapChainRef CreateSwapChain(const RVX::RHISwapChainDesc&) override { return {}; }
        RVX::RHIFenceRef CreateFence(RVX::uint64) override { return {}; }
        void WaitForFence(RVX::RHIFence*, RVX::uint64) override {}
        void WaitIdle() override {}
        void BeginFrame() override {}
        void EndFrame() override {}
        RVX::uint32 GetCurrentFrameIndex() const override { return 0; }
        RVX::RHIStagingBufferRef CreateStagingBuffer(const RVX::RHIStagingBufferDesc&) override { return {}; }
        RVX::RHIRingBufferRef CreateRingBuffer(const RVX::RHIRingBufferDesc&) override { return {}; }
        RVX::RHIMemoryStats GetMemoryStats() const override { return {}; }
        void BeginResourceGroup(const char*) override {}
        void EndResourceGroup() override {}
        const RVX::RHICapabilities& GetCapabilities() const override { return m_capabilities; }
        RVX::RHIBackendType GetBackendType() const override { return m_backend; }

        RVX::uint32 createShaderCalls = 0;

    private:
        RVX::RHIBackendType m_backend = RVX::RHIBackendType::None;
        RVX::RHICapabilities m_capabilities;
    };

    fs::path WriteArtifact(
        const fs::path& root,
        const RVX::ShaderArtifact& artifact)
    {
        const fs::path path = root /
            RVX::BuildShaderArtifactRelativePath(artifact.request);
        std::string error;
        EXPECT_TRUE(RVX::WriteShaderArtifactV2(path, artifact, error))
            << error;
        return path;
    }
}

TEST(ShaderArtifactValidation, RoundTripRetainsCompleteProgramContract)
{
    TemporaryDirectory temporary("rvx_shader_artifact_round_trip");
    const RVX::ShaderArtifact expected = MakeArtifact();
    const fs::path path = WriteArtifact(temporary.Path(), expected);

    RVX::ShaderArtifact actual;
    std::string error;
    ASSERT_TRUE(RVX::ReadShaderArtifactV2(path, actual, error)) << error;
    EXPECT_EQ(actual.request.logicalAssetId, expected.request.logicalAssetId);
    EXPECT_EQ(actual.request.entryPoint, expected.request.entryPoint);
    EXPECT_EQ(actual.request.stage, expected.request.stage);
    EXPECT_EQ(actual.request.backend, expected.request.backend);
    ASSERT_EQ(actual.request.defines.size(), 2u);
    EXPECT_EQ(actual.request.defines.front().name, "RVX_ALPHA_MODE");
    EXPECT_EQ(actual.targetProfile, expected.targetProfile);
    EXPECT_EQ(actual.runtimeEntryPoint, expected.runtimeEntryPoint);
    EXPECT_EQ(actual.compilerAbi, RVX::RVX_SHADER_COMPILER_ABI_VERSION);
    EXPECT_NE(actual.permutationHash, 0u);
    EXPECT_EQ(actual.contentHash, expected.contentHash);
    EXPECT_NE(actual.payloadHash, 0u);
    EXPECT_EQ(actual.code, expected.code);
    ASSERT_EQ(actual.reflection.resources.size(), 2u);
    EXPECT_EQ(actual.reflection.resources[1].count, 8u);
    ASSERT_EQ(actual.reflection.pushConstants.size(), 1u);
    EXPECT_EQ(actual.reflection.pushConstants.front().size, 32u);
    ASSERT_EQ(actual.reflection.inputs.size(), 1u);
    EXPECT_EQ(actual.reflection.inputs.front().semantic, "POSITION");
    ASSERT_EQ(actual.reflection.outputs.size(), 1u);
    EXPECT_TRUE(actual.reflection.outputs.front().systemValue);
}

TEST(ShaderArtifactValidation, RejectsPayloadCorruption)
{
    TemporaryDirectory temporary("rvx_shader_artifact_corrupt");
    const fs::path path = WriteArtifact(temporary.Path(), MakeArtifact());
    std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
    ASSERT_TRUE(file.is_open());
    file.seekg(-1, std::ios::end);
    char byte = 0;
    file.read(&byte, 1);
    byte ^= 0x5a;
    file.seekp(-1, std::ios::end);
    file.write(&byte, 1);
    file.close();

    RVX::ShaderArtifact artifact;
    std::string error;
    EXPECT_FALSE(RVX::ReadShaderArtifactV2(path, artifact, error));
    EXPECT_FALSE(error.empty());
}

TEST(ShaderArtifactValidation, RejectsLegacyTextArtifact)
{
    TemporaryDirectory temporary("rvx_shader_artifact_v1");
    const fs::path path = temporary.Path() / "legacy.rva";
    std::ofstream file(path, std::ios::binary);
    file << "RVX_SHADER_PREBAKE_V1\n";
    file.close();

    RVX::ShaderArtifact artifact;
    std::string error;
    EXPECT_FALSE(RVX::ReadShaderArtifactV2(path, artifact, error));
    EXPECT_FALSE(error.empty());
}

TEST(ShaderArtifactValidation, RejectsHeaderIdentityTampering)
{
    TemporaryDirectory temporary("rvx_shader_artifact_header");
    const fs::path path = WriteArtifact(temporary.Path(), MakeArtifact());
    std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
    ASSERT_TRUE(file.is_open());
    RVX::ShaderArtifactHeaderV2 header;
    file.read(reinterpret_cast<char*>(&header), sizeof(header));
    ASSERT_TRUE(file.good());
    header.backend = static_cast<RVX::uint32>(RVX::RHIBackendType::Vulkan);
    file.seekp(0, std::ios::beg);
    file.write(reinterpret_cast<const char*>(&header), sizeof(header));
    file.close();

    RVX::ShaderArtifact artifact;
    std::string error;
    EXPECT_FALSE(RVX::ReadShaderArtifactV2(path, artifact, error));
    EXPECT_FALSE(error.empty());
}

TEST(ShaderArtifactValidation, RejectsHeaderContentHashTampering)
{
    TemporaryDirectory temporary("rvx_shader_artifact_content_hash");
    const fs::path path = WriteArtifact(temporary.Path(), MakeArtifact());
    std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
    ASSERT_TRUE(file.is_open());
    RVX::ShaderArtifactHeaderV2 header;
    file.read(reinterpret_cast<char*>(&header), sizeof(header));
    ASSERT_TRUE(file.good());
    header.contentHash ^= 0x5a5a5a5a5a5a5a5aull;
    file.seekp(0, std::ios::beg);
    file.write(reinterpret_cast<const char*>(&header), sizeof(header));
    file.close();

    RVX::ShaderArtifact artifact;
    std::string error;
    EXPECT_FALSE(RVX::ReadShaderArtifactV2(path, artifact, error));
    EXPECT_FALSE(error.empty());
}

TEST(ShaderArtifactValidation, RejectsUnsupportedHeaderFlags)
{
    TemporaryDirectory temporary("rvx_shader_artifact_flags");
    const fs::path path = WriteArtifact(temporary.Path(), MakeArtifact());
    std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
    ASSERT_TRUE(file.is_open());
    RVX::ShaderArtifactHeaderV2 header;
    file.read(reinterpret_cast<char*>(&header), sizeof(header));
    ASSERT_TRUE(file.good());
    header.flags = 1;
    file.seekp(0, std::ios::beg);
    file.write(reinterpret_cast<const char*>(&header), sizeof(header));
    file.close();

    RVX::ShaderArtifact artifact;
    std::string error;
    EXPECT_FALSE(RVX::ReadShaderArtifactV2(path, artifact, error));
    EXPECT_FALSE(error.empty());
}

TEST(ShaderArtifactValidation, CookedProviderLoadsCodeAndReflection)
{
    TemporaryDirectory temporary("rvx_shader_provider_success");
    const RVX::ShaderArtifact artifact = MakeArtifact();
    WriteArtifact(temporary.Path(), artifact);
    CapturingDevice device(RVX::RHIBackendType::DX12);
    auto provider = RVX::CreateCookedShaderProgramProviderFactory(
        temporary.Path())();

    const RVX::ShaderProgramResult result =
        provider->Load(device, artifact.request);

    ASSERT_TRUE(result.Succeeded()) << result.diagnostic;
    EXPECT_EQ(device.createShaderCalls, 1u);
    EXPECT_EQ(result.contentHash, artifact.contentHash);
    EXPECT_EQ(result.shader->GetBytecode(), artifact.code);
    ASSERT_TRUE(result.shader->HasInterface());
    EXPECT_EQ(result.shader->GetInterface().bindings.size(), 2u);
    EXPECT_EQ(result.shader->GetInterface().pushConstants.size(), 1u);
}

TEST(ShaderArtifactValidation, CookedProviderRejectsBackendMismatchBeforeIo)
{
    TemporaryDirectory temporary("rvx_shader_provider_backend");
    const RVX::ShaderArtifact artifact = MakeArtifact();
    CapturingDevice device(RVX::RHIBackendType::Vulkan);
    auto provider = RVX::CreateCookedShaderProgramProviderFactory(
        temporary.Path())();

    const RVX::ShaderProgramResult result =
        provider->Load(device, artifact.request);

    EXPECT_EQ(result.status, RVX::ShaderProgramStatus::BackendMismatch);
    EXPECT_EQ(device.createShaderCalls, 0u);
}

TEST(ShaderArtifactValidation, CookedProviderRejectsCompilerAbiMismatch)
{
    TemporaryDirectory temporary("rvx_shader_provider_abi");
    RVX::ShaderArtifact artifact = MakeArtifact();
    --artifact.compilerAbi;
    WriteArtifact(temporary.Path(), artifact);
    CapturingDevice device(RVX::RHIBackendType::DX12);
    auto provider = RVX::CreateCookedShaderProgramProviderFactory(
        temporary.Path())();

    const RVX::ShaderProgramResult result =
        provider->Load(device, artifact.request);

    EXPECT_EQ(
        result.status,
        RVX::ShaderProgramStatus::IncompatibleArtifact);
    EXPECT_EQ(device.createShaderCalls, 0u);
}

TEST(ShaderArtifactValidation, CookedProviderDoesNotFallbackToSource)
{
    TemporaryDirectory temporary("rvx_shader_provider_no_fallback");
    const RVX::ShaderArtifact artifact = MakeArtifact();
    std::ofstream source(
        temporary.Path() / artifact.request.logicalAssetId,
        std::ios::binary);
    source << "float4 VSMain() : SV_Position { return 0; }";
    source.close();
    CapturingDevice device(RVX::RHIBackendType::DX12);
    auto provider = RVX::CreateCookedShaderProgramProviderFactory(
        temporary.Path())();

    const RVX::ShaderProgramResult result =
        provider->Load(device, artifact.request);

    EXPECT_EQ(result.status, RVX::ShaderProgramStatus::NotFound);
    EXPECT_EQ(device.createShaderCalls, 0u);
}
