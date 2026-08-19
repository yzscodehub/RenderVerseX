#include "RHI/RHIDescriptor.h"
#include "RHI/RHIPipelineValidation.h"
#include "RHI/RHIShader.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace RVX::Tests
{
    static_assert(
        static_cast<uint8>(
            RHIPipelineValidationCode::InvalidVertexInputLayout) == 25);

    namespace
    {
        class TestShader final : public RHIShader
        {
        public:
            TestShader(RHIShaderStage stage,
                       RHIShaderInterface shaderInterface,
                       const char* name)
                : RHIShader(MakeDesc(
                      stage,
                      shaderInterface,
                      name))
                , m_stage(stage)
            {
            }

            RHIShaderStage GetStage() const override
            {
                return m_stage;
            }

            const std::vector<uint8>& GetBytecode() const override
            {
                return m_bytecode;
            }

        private:
            static RHIShaderDesc MakeDesc(
                RHIShaderStage stage,
                const RHIShaderInterface& shaderInterface,
                const char* name)
            {
                RHIShaderDesc desc;
                desc.stage = stage;
                desc.entryPoint = "Main";
                desc.debugName = name;
                desc.shaderInterface = &shaderInterface;
                return desc;
            }

            RHIShaderStage m_stage = RHIShaderStage::None;
            std::vector<uint8> m_bytecode = {1};
        };

        class TestDescriptorSetLayout final
            : public RHIDescriptorSetLayout
        {
        public:
            explicit TestDescriptorSetLayout(
                std::vector<RHIBindingLayoutEntry> entries)
                : m_entries(std::move(entries))
            {
            }

            const std::vector<RHIBindingLayoutEntry>& GetEntries()
                const override
            {
                return m_entries;
            }

        private:
            std::vector<RHIBindingLayoutEntry> m_entries;
        };

        class TestPipelineLayout final : public RHIPipelineLayout
        {
        public:
            explicit TestPipelineLayout(
                const RHIPipelineLayoutDesc& desc)
                : RHIPipelineLayout(desc)
            {
            }
        };

        RHIShaderInterface MakeInterface(RHIShaderStage stage)
        {
            RHIShaderInterface shaderInterface;
            shaderInterface.available = true;
            shaderInterface.stage = stage;
            return shaderInterface;
        }

        RHIShaderInterfaceVariable MakeVariable(
            uint32 location,
            RHIFormat format,
            const char* semantic,
            uint32 semanticIndex = 0,
            bool systemValue = false)
        {
            RHIShaderInterfaceVariable variable;
            variable.location = location;
            variable.format = format;
            variable.systemValue = systemValue;
            variable.semanticName = semantic;
            variable.semanticIndex = semanticIndex;
            return variable;
        }

        RHIShaderInterface MakeValidVertexInterface()
        {
            RHIShaderInterface shaderInterface =
                MakeInterface(RHIShaderStage::Vertex);
            shaderInterface.inputs.push_back(
                MakeVariable(
                    0,
                    RHIFormat::RGB32_FLOAT,
                    "POSITION"));
            return shaderInterface;
        }

        RHIShaderInterface MakeValidPixelInterface()
        {
            RHIShaderInterface shaderInterface =
                MakeInterface(RHIShaderStage::Pixel);
            shaderInterface.outputs.push_back(
                MakeVariable(
                    0,
                    RHIFormat::RGBA32_FLOAT,
                    "SV_TARGET",
                    0,
                    true));
            return shaderInterface;
        }

        std::string ReadTextFile(
            const std::filesystem::path& path)
        {
            std::ifstream file(path);
            std::ostringstream stream;
            stream << file.rdbuf();
            return stream.str();
        }

        struct ValidPipelineFixture
        {
            RHIPipelineLayoutDesc layoutDesc;
            TestPipelineLayout pipelineLayout;
            TestShader vertexShader;
            TestShader pixelShader;
            RHIGraphicsPipelineDesc pipelineDesc;

            ValidPipelineFixture()
                : pipelineLayout(layoutDesc)
                , vertexShader(
                      RHIShaderStage::Vertex,
                      MakeValidVertexInterface(),
                      "ValidVS")
                , pixelShader(
                      RHIShaderStage::Pixel,
                      MakeValidPixelInterface(),
                      "ValidPS")
            {
                pipelineDesc.vertexShader = &vertexShader;
                pipelineDesc.pixelShader = &pixelShader;
                pipelineDesc.pipelineLayout = &pipelineLayout;
                pipelineDesc.debugName = "ValidPipeline";
                pipelineDesc.inputLayout.AddElement(
                    "POSITION",
                    RHIFormat::RGB32_FLOAT);
                pipelineDesc.numRenderTargets = 1;
                pipelineDesc.renderTargetFormats[0] =
                    RHIFormat::RGBA8_UNORM;
            }
        };
    } // namespace

    TEST(RHIPipelineValidation,
         ShaderInterfaceHashIsCanonicalAndOwned)
    {
        RHIShaderInterface ordered =
            MakeInterface(RHIShaderStage::Vertex);
        ordered.inputs = {
            MakeVariable(
                0,
                RHIFormat::RGB32_FLOAT,
                "POSITION"),
            MakeVariable(
                1,
                RHIFormat::RG32_FLOAT,
                "TEXCOORD"),
        };
        ordered.bindings = {
            {0, 0, RHIBindingType::UniformBuffer, 1},
            {1, 2, RHIBindingType::SampledTexture, 1},
        };

        RHIShaderInterface reversed = ordered;
        std::reverse(reversed.inputs.begin(),
                     reversed.inputs.end());
        std::reverse(reversed.bindings.begin(),
                     reversed.bindings.end());

        ordered = FinalizeRHIShaderInterface(
            std::move(ordered));
        reversed = FinalizeRHIShaderInterface(
            std::move(reversed));
        EXPECT_EQ(ordered.hash, reversed.hash);
        EXPECT_EQ(ordered.hash, 0x4163b319fb5c088eull);

        TestShader shader(
            RHIShaderStage::Vertex,
            ordered,
            "OwnedInterfaceVS");
        ordered.inputs.front().semanticName = "MUTATED";
        EXPECT_EQ(
            shader.GetInterface().inputs.front().semanticName,
            "POSITION");
        EXPECT_EQ(shader.GetEntryPoint(), "Main");
    }

    TEST(RHIPipelineValidation,
         ValidPipelinePassesPortablePreflight)
    {
        ValidPipelineFixture fixture;

        const RHIPipelineValidationResult result =
            ValidateRHIGraphicsPipelineDesc(
                fixture.pipelineDesc);

        EXPECT_TRUE(result);
        EXPECT_EQ(result.code,
                  RHIPipelineValidationCode::Passed);
    }

    TEST(RHIPipelineValidation,
         VertexTranslationIsDeterministicAcrossBackendConsumers)
    {
        RHIInputLayoutDesc inputLayout;
        inputLayout.AddElementAtLocation(
            3,
            "POSITION",
            0,
            RHIFormat::RGB32_FLOAT,
            2);
        inputLayout.AddElement(
            "TEXCOORD",
            RHIFormat::RG32_FLOAT,
            0);
        inputLayout.AddElementAtLocation(
            7,
            "NORMAL",
            0,
            RHIFormat::RGB32_FLOAT,
            2);

        const RHIVertexInputTranslation translation =
            BuildRHIVertexInputTranslation(inputLayout);
        ASSERT_TRUE(translation);
        ASSERT_EQ(translation.attributes.size(), 3u);
        EXPECT_EQ(translation.attributes[0].location, 3u);
        EXPECT_EQ(translation.attributes[0].inputSlot, 2u);
        EXPECT_EQ(translation.attributes[0].alignedByteOffset, 0u);
        EXPECT_EQ(translation.attributes[1].location, 1u);
        EXPECT_EQ(translation.attributes[1].inputSlot, 0u);
        EXPECT_EQ(translation.attributes[1].alignedByteOffset, 0u);
        EXPECT_EQ(translation.attributes[2].location, 7u);
        EXPECT_EQ(translation.attributes[2].inputSlot, 2u);
        EXPECT_EQ(translation.attributes[2].alignedByteOffset, 12u);

        ASSERT_EQ(translation.bindings.size(), 2u);
        EXPECT_EQ(translation.bindings[0].inputSlot, 0u);
        EXPECT_EQ(translation.bindings[0].stride, 8u);
        EXPECT_EQ(translation.bindings[1].inputSlot, 2u);
        EXPECT_EQ(translation.bindings[1].stride, 24u);

        inputLayout.elements[2].perInstance = true;
        inputLayout.elements[2].instanceDataStepRate = 1;
        const RHIVertexInputTranslation invalidTranslation =
            BuildRHIVertexInputTranslation(inputLayout);
        EXPECT_FALSE(invalidTranslation);
        EXPECT_EQ(invalidTranslation.elementIndex, 2u);
        EXPECT_EQ(invalidTranslation.inputSlot, 2u);

        RHIInputLayoutDesc duplicateLocationLayout;
        duplicateLocationLayout.AddElementAtLocation(
            1,
            "POSITION",
            0,
            RHIFormat::RGB32_FLOAT);
        duplicateLocationLayout.AddElement(
            "TEXCOORD",
            RHIFormat::RG32_FLOAT);
        EXPECT_FALSE(BuildRHIVertexInputTranslation(
            duplicateLocationLayout));

        RHIInputLayoutDesc unsupportedStepRateLayout;
        unsupportedStepRateLayout.AddElement(
            "INSTANCE_DATA",
            RHIFormat::RGBA32_FLOAT);
        unsupportedStepRateLayout.elements.front().perInstance = true;
        unsupportedStepRateLayout.elements.front().
            instanceDataStepRate = 2;
        EXPECT_FALSE(BuildRHIVertexInputTranslation(
            unsupportedStepRateLayout));
    }

    TEST(RHIPipelineValidation,
         PrimaryBackendsConsumeSharedVertexTranslation)
    {
        const std::filesystem::path sourceRoot =
            RVX_SOURCE_DIR;
        const std::array<std::filesystem::path, 3> backendSources = {
            sourceRoot / "RHI_DX12" / "Private" /
                "DX12Pipeline.cpp",
            sourceRoot / "RHI_Vulkan" / "Private" /
                "VulkanPipeline.cpp",
            sourceRoot / "RHI_Metal" / "Private" /
                "MetalPipeline.mm",
        };

        for (const std::filesystem::path& path : backendSources)
        {
            const std::string source = ReadTextFile(path);
            ASSERT_FALSE(source.empty()) << path.string();
            EXPECT_NE(
                source.find(
                    "BuildRHIVertexInputTranslation"),
                std::string::npos)
                << path.string();
        }
    }

    TEST(RHIPipelineValidation,
         MetalVertexStreamsUseLegalDescendingBufferIndices)
    {
        const std::filesystem::path sourceRoot = RVX_SOURCE_DIR;
        const std::string commonSource = ReadTextFile(
            sourceRoot / "RHI_Metal" / "Private" / "MetalCommon.h");
        const std::string pipelineSource = ReadTextFile(
            sourceRoot / "RHI_Metal" / "Private" / "MetalPipeline.mm");
        const std::string commandSource = ReadTextFile(
            sourceRoot / "RHI_Metal" / "Private" / "MetalCommandContext.mm");

        ASSERT_FALSE(commonSource.empty());
        ASSERT_FALSE(pipelineSource.empty());
        ASSERT_FALSE(commandSource.empty());
        EXPECT_NE(commonSource.find("kMetalHighestBufferIndex = 30"),
                  std::string::npos);
        EXPECT_NE(commonSource.find(
                      "kMetalHighestBufferIndex - inputSlot"),
                  std::string::npos);
        EXPECT_NE(pipelineSource.find(
                      "MetalVertexBufferIndex(attribute.inputSlot)"),
                  std::string::npos);
        EXPECT_NE(pipelineSource.find(
                      "MetalVertexBufferIndex(binding.inputSlot)"),
                  std::string::npos);
        EXPECT_NE(commandSource.find("MetalVertexBufferIndex(slot)"),
                  std::string::npos);
        EXPECT_EQ(pipelineSource.find("30 + attribute.inputSlot"),
                  std::string::npos);
        EXPECT_EQ(commandSource.find("30 + slot"), std::string::npos);
    }

    TEST(RHIPipelineValidation,
         MissingSkinningSemanticFailsBeforeNativeCreation)
    {
        ValidPipelineFixture fixture;
        RHIShaderInterface vertexInterface =
            MakeInterface(RHIShaderStage::Vertex);
        vertexInterface.inputs = {
            MakeVariable(
                0,
                RHIFormat::RGB32_FLOAT,
                "POSITION"),
            MakeVariable(
                1,
                RHIFormat::RGBA32_UINT,
                "BLENDINDICES"),
            MakeVariable(
                2,
                RHIFormat::RGBA32_FLOAT,
                "BLENDWEIGHT"),
        };
        TestShader vertexShader(
            RHIShaderStage::Vertex,
            vertexInterface,
            "SkinnedVS");
        fixture.pipelineDesc.vertexShader = &vertexShader;

        const RHIPipelineValidationResult result =
            ValidateRHIGraphicsPipelineDesc(
                fixture.pipelineDesc);

        EXPECT_FALSE(result);
        EXPECT_EQ(result.code,
                  RHIPipelineValidationCode::MissingVertexInput);
        EXPECT_EQ(result.semanticName, "BLENDINDICES");
        EXPECT_EQ(result.shaderName, "SkinnedVS");
        EXPECT_EQ(result.shaderEntryPoint, "Main");
        const std::string diagnostic =
            FormatRHIPipelineValidationResult(result);
        EXPECT_NE(
            diagnostic.find("MissingVertexInput"),
            std::string::npos);
        EXPECT_NE(
            diagnostic.find("semantic='BLENDINDICES0'"),
            std::string::npos);
    }

    TEST(RHIPipelineValidation,
         SemanticIndexAndFormatMismatchesAreStructured)
    {
        ValidPipelineFixture fixture;
        RHIShaderInterface vertexInterface =
            MakeInterface(RHIShaderStage::Vertex);
        vertexInterface.inputs.push_back(
            MakeVariable(
                0,
                RHIFormat::RG32_FLOAT,
                "TEXCOORD",
                1));
        TestShader vertexShader(
            RHIShaderStage::Vertex,
            vertexInterface,
            "IndexedVS");
        fixture.pipelineDesc.vertexShader = &vertexShader;
        fixture.pipelineDesc.inputLayout.elements.clear();
        fixture.pipelineDesc.inputLayout.AddElement(
            "TEXCOORD",
            RHIFormat::RG32_FLOAT);

        RHIPipelineValidationResult result =
            ValidateRHIGraphicsPipelineDesc(
                fixture.pipelineDesc);
        EXPECT_FALSE(result);
        EXPECT_EQ(result.code,
                  RHIPipelineValidationCode::MissingVertexInput);
        EXPECT_EQ(result.semanticIndex, 1u);

        fixture.pipelineDesc.inputLayout.elements.front().
            semanticIndex = 1;
        fixture.pipelineDesc.inputLayout.elements.front().format =
            RHIFormat::RG32_UINT;
        result = ValidateRHIGraphicsPipelineDesc(
            fixture.pipelineDesc);
        EXPECT_FALSE(result);
        EXPECT_EQ(
            result.code,
            RHIPipelineValidationCode::VertexInputFormatMismatch);
        EXPECT_EQ(result.expectedFormat,
                  RHIFormat::RG32_FLOAT);
        EXPECT_EQ(result.actualFormat,
                  RHIFormat::RG32_UINT);
    }

    TEST(RHIPipelineValidation,
         DuplicateAndUnexpectedInputsFailClosed)
    {
        ValidPipelineFixture fixture;
        fixture.pipelineDesc.inputLayout.AddElement(
            "POSITION",
            RHIFormat::RGB32_FLOAT);

        RHIPipelineValidationResult result =
            ValidateRHIGraphicsPipelineDesc(
                fixture.pipelineDesc);
        EXPECT_FALSE(result);
        EXPECT_EQ(
            result.code,
            RHIPipelineValidationCode::DuplicateInputSemantic);

        fixture.pipelineDesc.inputLayout.elements.back().
            semanticName = "NORMAL";
        result = ValidateRHIGraphicsPipelineDesc(
            fixture.pipelineDesc);
        EXPECT_FALSE(result);
        EXPECT_EQ(
            result.code,
            RHIPipelineValidationCode::UnexpectedVertexInput);
        EXPECT_EQ(result.semanticName, "NORMAL");
    }

    TEST(RHIPipelineValidation,
         SystemValuesAndValidNoInputShadersNeedNoVertexLayout)
    {
        ValidPipelineFixture fixture;
        RHIShaderInterface vertexInterface =
            MakeInterface(RHIShaderStage::Vertex);
        vertexInterface.inputs.push_back(
            MakeVariable(
                RVX_INVALID_INDEX,
                RHIFormat::R32_UINT,
                "SV_VERTEXID",
                0,
                true));
        TestShader vertexShader(
            RHIShaderStage::Vertex,
            vertexInterface,
            "FullscreenVS");
        fixture.pipelineDesc.vertexShader = &vertexShader;
        fixture.pipelineDesc.inputLayout.elements.clear();

        RHIPipelineValidationResult result =
            ValidateRHIGraphicsPipelineDesc(
                fixture.pipelineDesc);
        EXPECT_TRUE(result);

        TestShader noInputShader(
            RHIShaderStage::Vertex,
            MakeInterface(RHIShaderStage::Vertex),
            "NoInputVS");
        fixture.pipelineDesc.vertexShader = &noInputShader;
        result = ValidateRHIGraphicsPipelineDesc(
            fixture.pipelineDesc);
        EXPECT_TRUE(result);
    }

    TEST(RHIPipelineValidation,
         FragmentAttachmentCountAndFormatMustMatch)
    {
        ValidPipelineFixture fixture;
        RHIShaderInterface pixelInterface =
            MakeInterface(RHIShaderStage::Pixel);
        pixelInterface.outputs = {
            MakeVariable(
                0,
                RHIFormat::RGBA32_FLOAT,
                "SV_TARGET",
                0,
                true),
            MakeVariable(
                1,
                RHIFormat::RGBA32_UINT,
                "SV_TARGET",
                1,
                true),
        };
        TestShader pixelShader(
            RHIShaderStage::Pixel,
            pixelInterface,
            "GBufferPS");
        fixture.pipelineDesc.pixelShader = &pixelShader;

        RHIPipelineValidationResult result =
            ValidateRHIGraphicsPipelineDesc(
                fixture.pipelineDesc);
        EXPECT_FALSE(result);
        EXPECT_EQ(
            result.code,
            RHIPipelineValidationCode::UnexpectedFragmentOutput);
        EXPECT_EQ(result.location, 1u);

        fixture.pipelineDesc.numRenderTargets = 2;
        fixture.pipelineDesc.renderTargetFormats[1] =
            RHIFormat::RGBA8_UNORM;
        result = ValidateRHIGraphicsPipelineDesc(
            fixture.pipelineDesc);
        EXPECT_FALSE(result);
        EXPECT_EQ(
            result.code,
            RHIPipelineValidationCode::
                FragmentOutputFormatMismatch);
        EXPECT_EQ(result.location, 1u);
    }

    TEST(RHIPipelineValidation,
         PipelineLayoutMustCoverBindingsAndPushConstants)
    {
        RHIShaderInterface vertexInterface =
            MakeInterface(RHIShaderStage::Vertex);
        vertexInterface.inputs.push_back(
            MakeVariable(
                0,
                RHIFormat::RGB32_FLOAT,
                "POSITION"));
        vertexInterface.bindings.push_back(
            {0, 2, RHIBindingType::UniformBuffer, 1});
        vertexInterface.pushConstants.push_back({0, 32});
        TestShader vertexShader(
            RHIShaderStage::Vertex,
            vertexInterface,
            "LayoutVS");

        RHIShaderInterface pixelInterface =
            MakeInterface(RHIShaderStage::Pixel);
        pixelInterface.outputs.push_back(
            MakeVariable(
                0,
                RHIFormat::RGBA32_FLOAT,
                "SV_TARGET",
                0,
                true));
        TestShader pixelShader(
            RHIShaderStage::Pixel,
            pixelInterface,
            "LayoutPS");

        TestDescriptorSetLayout setLayout(
            {{2,
              RHIBindingType::UniformBuffer,
              RHIShaderStage::Vertex,
              1,
              false}});
        RHIPipelineLayoutDesc layoutDesc;
        layoutDesc.setLayouts.push_back(&setLayout);
        layoutDesc.pushConstantSize = 16;
        layoutDesc.pushConstantStages =
            RHIShaderStage::Vertex;
        TestPipelineLayout layout(layoutDesc);

        RHIGraphicsPipelineDesc desc;
        desc.vertexShader = &vertexShader;
        desc.pixelShader = &pixelShader;
        desc.pipelineLayout = &layout;
        desc.inputLayout.AddElement(
            "POSITION",
            RHIFormat::RGB32_FLOAT);

        RHIPipelineValidationResult result =
            ValidateRHIGraphicsPipelineDesc(desc);
        EXPECT_FALSE(result);
        EXPECT_EQ(
            result.code,
            RHIPipelineValidationCode::
                PushConstantRangeMismatch);
        EXPECT_EQ(result.expectedCount, 32u);
        EXPECT_EQ(result.actualCount, 16u);

        layoutDesc.pushConstantSize = 32;
        TestPipelineLayout completeLayout(layoutDesc);
        desc.pipelineLayout = &completeLayout;
        result = ValidateRHIGraphicsPipelineDesc(desc);
        EXPECT_TRUE(result);
    }
} // namespace RVX::Tests
