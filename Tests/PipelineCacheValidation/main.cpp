#include "Core/Log.h"
#include "Render/PipelineCache.h"
#include "Render/Lighting/ClusteredLighting.h"
#include "Render/Lighting/LightManager.h"
#include "Render/Renderer/RenderScene.h"
#include "Render/Renderer/ViewData.h"
#include "RHI/RHI.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <type_traits>
#include <vector>

namespace
{
    namespace fs = std::filesystem;

    class PipelineCacheValidationFixture : public ::testing::Test
    {
    protected:
        static void SetUpTestSuite()
        {
            RVX::Log::Initialize();
        }

        static void TearDownTestSuite()
        {
            RVX::Log::Shutdown();
        }
    };

    fs::path FindShaderDirectory()
    {
        fs::path cursor = fs::current_path();
        for (uint32_t i = 0; i < 8; ++i)
        {
            fs::path candidate = cursor / "Render" / "Shaders";
            if (fs::exists(candidate / "DefaultLit.hlsl"))
            {
                return candidate;
            }

            if (!cursor.has_parent_path() || cursor == cursor.parent_path())
            {
                break;
            }
            cursor = cursor.parent_path();
        }

        return {};
    }

    class TempDirectory
    {
    public:
        explicit TempDirectory(const char* prefix)
        {
            const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
            m_path = fs::temp_directory_path() / (std::string(prefix) + "_" + std::to_string(ticks));
            fs::create_directories(m_path);
        }

        ~TempDirectory()
        {
            std::error_code ec;
            fs::remove_all(m_path, ec);
        }

        const fs::path& Path() const { return m_path; }

    private:
        fs::path m_path;
    };

    class FakeBuffer final : public RVX::RHIBuffer
    {
    public:
        explicit FakeBuffer(const RVX::RHIBufferDesc& desc)
            : m_desc(desc)
            , m_storage(static_cast<size_t>(desc.size))
        {
        }

        RVX::uint64 GetSize() const override { return m_desc.size; }
        RVX::RHIBufferUsage GetUsage() const override { return m_desc.usage; }
        RVX::RHIMemoryType GetMemoryType() const override { return m_desc.memoryType; }
        RVX::uint32 GetStride() const override { return m_desc.stride; }
        void* Map() override { return m_storage.data(); }
        void Unmap() override {}
        const std::vector<RVX::uint8>& GetStorage() const { return m_storage; }

    private:
        RVX::RHIBufferDesc m_desc;
        std::vector<RVX::uint8> m_storage;
    };

    class FakeTexture final : public RVX::RHITexture
    {
    public:
        explicit FakeTexture(const RVX::RHITextureDesc& desc)
            : m_desc(desc)
        {
        }

        RVX::uint32 GetWidth() const override { return m_desc.width; }
        RVX::uint32 GetHeight() const override { return m_desc.height; }
        RVX::uint32 GetDepth() const override { return m_desc.depth; }
        RVX::uint32 GetMipLevels() const override { return m_desc.mipLevels; }
        RVX::uint32 GetArraySize() const override { return m_desc.arraySize; }
        RVX::RHIFormat GetFormat() const override { return m_desc.format; }
        RVX::RHITextureUsage GetUsage() const override { return m_desc.usage; }
        RVX::RHITextureDimension GetDimension() const override { return m_desc.dimension; }
        RVX::RHISampleCount GetSampleCount() const override { return m_desc.sampleCount; }

    private:
        RVX::RHITextureDesc m_desc;
    };

    class FakeTextureView final : public RVX::RHITextureView
    {
    public:
        FakeTextureView(RVX::RHITexture* texture, const RVX::RHITextureViewDesc& desc)
            : m_texture(texture)
            , m_desc(desc)
        {
            if (m_desc.format == RVX::RHIFormat::Unknown && m_texture)
            {
                m_desc.format = m_texture->GetFormat();
            }
        }

        RVX::RHITexture* GetTexture() const override { return m_texture; }
        RVX::RHIFormat GetFormat() const override { return m_desc.format; }
        const RVX::RHISubresourceRange& GetSubresourceRange() const override { return m_desc.subresourceRange; }

    private:
        RVX::RHITexture* m_texture = nullptr;
        RVX::RHITextureViewDesc m_desc;
    };

    class FakeSampler final : public RVX::RHISampler
    {
    };

    class FakeShader final : public RVX::RHIShader
    {
    public:
        explicit FakeShader(const RVX::RHIShaderDesc& desc)
            : m_stage(desc.stage)
        {
            if (desc.bytecode && desc.bytecodeSize > 0)
            {
                const auto* bytes = static_cast<const RVX::uint8*>(desc.bytecode);
                m_bytecode.assign(bytes, bytes + desc.bytecodeSize);
            }
        }

        RVX::RHIShaderStage GetStage() const override { return m_stage; }
        const std::vector<RVX::uint8>& GetBytecode() const override { return m_bytecode; }

    private:
        RVX::RHIShaderStage m_stage = RVX::RHIShaderStage::None;
        std::vector<RVX::uint8> m_bytecode;
    };

    class FakeDescriptorSetLayout final : public RVX::RHIDescriptorSetLayout
    {
    public:
        explicit FakeDescriptorSetLayout(const RVX::RHIDescriptorSetLayoutDesc& desc)
            : m_entries(desc.entries)
        {
        }

        const std::vector<RVX::RHIBindingLayoutEntry>& GetEntries() const override { return m_entries; }

    private:
        std::vector<RVX::RHIBindingLayoutEntry> m_entries;
    };

    class FakePipelineLayout final : public RVX::RHIPipelineLayout
    {
    };

    class FakePipeline final : public RVX::RHIPipeline
    {
    public:
        bool IsCompute() const override { return false; }
    };

    class FakeDescriptorSet final : public RVX::RHIDescriptorSet
    {
    public:
        explicit FakeDescriptorSet(const RVX::RHIDescriptorSetDesc& desc)
            : m_bindings(desc.bindings)
        {
        }

        bool Update(const std::vector<RVX::RHIDescriptorBinding>& bindings) override
        {
            m_bindings = bindings;
            return true;
        }

        const std::vector<RVX::RHIDescriptorBinding>& GetBindings() const { return m_bindings; }

    private:
        std::vector<RVX::RHIDescriptorBinding> m_bindings;
    };

    class FakeDevice final : public RVX::IRHIDevice
    {
    public:
        explicit FakeDevice(RVX::RHIBackendType backend = RVX::RHIBackendType::DX12)
            : m_backend(backend)
        {
        }

        RVX::RHIBufferRef CreateBuffer(const RVX::RHIBufferDesc& desc) override
        {
            if (failBufferCreation)
                return {};
            auto buffer = RVX::MakeRef<FakeBuffer>(desc);
            capturedBufferDescs.push_back(desc);
            capturedBuffers.push_back(buffer.Get());
            return buffer;
        }

        RVX::RHITextureRef CreateTexture(const RVX::RHITextureDesc& desc) override
        {
            capturedTextureDescs.push_back(desc);
            return RVX::MakeRef<FakeTexture>(desc);
        }

        RVX::RHITextureViewRef CreateTextureView(RVX::RHITexture* texture, const RVX::RHITextureViewDesc& desc) override
        {
            capturedTextureViewDescs.push_back(desc);
            return RVX::MakeRef<FakeTextureView>(texture, desc);
        }

        RVX::RHISamplerRef CreateSampler(const RVX::RHISamplerDesc& desc) override
        {
            capturedSamplerDescs.push_back(desc);
            return RVX::MakeRef<FakeSampler>();
        }

        RVX::RHIShaderRef CreateShader(const RVX::RHIShaderDesc& desc) override
        {
            ++shaderCreateCount;
            if (failShaderCreation || shaderCreateCount == failShaderCreationAtIndex)
                return {};
            return RVX::MakeRef<FakeShader>(desc);
        }

        RVX::RHIHeapRef CreateHeap(const RVX::RHIHeapDesc&) override { return {}; }
        RVX::RHITextureRef CreatePlacedTexture(RVX::RHIHeap*, RVX::uint64, const RVX::RHITextureDesc&) override { return {}; }
        RVX::RHIBufferRef CreatePlacedBuffer(RVX::RHIHeap*, RVX::uint64, const RVX::RHIBufferDesc&) override { return {}; }
        MemoryRequirements GetTextureMemoryRequirements(const RVX::RHITextureDesc&) override { return {}; }
        MemoryRequirements GetBufferMemoryRequirements(const RVX::RHIBufferDesc&) override { return {}; }

        RVX::RHIDescriptorSetLayoutRef CreateDescriptorSetLayout(const RVX::RHIDescriptorSetLayoutDesc& desc) override
        {
            capturedSetLayouts.push_back(desc);
            return RVX::MakeRef<FakeDescriptorSetLayout>(desc);
        }

        RVX::RHIPipelineLayoutRef CreatePipelineLayout(const RVX::RHIPipelineLayoutDesc& desc) override
        {
            capturedPipelineLayoutSetCount = static_cast<RVX::uint32>(desc.setLayouts.size());
            capturedPipelineLayoutSetCounts.push_back(capturedPipelineLayoutSetCount);
            capturedPipelineLayoutDescs.push_back(desc);
            if (failPipelineLayoutCreation)
                return {};
            return RVX::MakeRef<FakePipelineLayout>();
        }

        RVX::RHIPipelineRef CreateGraphicsPipeline(const RVX::RHIGraphicsPipelineDesc& desc) override
        {
            capturedGraphicsPipelines.push_back(desc);
            if (failPipelineCreation ||
                capturedGraphicsPipelines.size() == static_cast<size_t>(failPipelineCreationAtIndex))
                return {};
            return RVX::MakeRef<FakePipeline>();
        }

        RVX::RHIPipelineRef CreateComputePipeline(const RVX::RHIComputePipelineDesc&) override { return {}; }
        RVX::RHIDescriptorSetRef CreateDescriptorSet(const RVX::RHIDescriptorSetDesc& desc) override
        {
            capturedDescriptorSetDescs.push_back(desc);
            auto descriptorSet = RVX::MakeRef<FakeDescriptorSet>(desc);
            capturedDescriptorSets.push_back(descriptorSet.Get());
            return descriptorSet;
        }
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

        bool failBufferCreation = false;
        bool failShaderCreation = false;
        RVX::uint32 failShaderCreationAtIndex = std::numeric_limits<RVX::uint32>::max();
        bool failPipelineLayoutCreation = false;
        bool failPipelineCreation = false;
        RVX::uint32 failPipelineCreationAtIndex = std::numeric_limits<RVX::uint32>::max();
        RVX::uint32 shaderCreateCount = 0;
        RVX::uint32 capturedPipelineLayoutSetCount = 0;
        std::vector<RVX::uint32> capturedPipelineLayoutSetCounts;
        std::vector<RVX::RHIPipelineLayoutDesc> capturedPipelineLayoutDescs;
        std::vector<RVX::RHIBufferDesc> capturedBufferDescs;
        std::vector<FakeBuffer*> capturedBuffers;
        std::vector<RVX::RHITextureDesc> capturedTextureDescs;
        std::vector<RVX::RHITextureViewDesc> capturedTextureViewDescs;
        std::vector<RVX::RHISamplerDesc> capturedSamplerDescs;
        std::vector<RVX::RHIDescriptorSetDesc> capturedDescriptorSetDescs;
        std::vector<FakeDescriptorSet*> capturedDescriptorSets;
        std::vector<RVX::RHIDescriptorSetLayoutDesc> capturedSetLayouts;
        std::vector<RVX::RHIGraphicsPipelineDesc> capturedGraphicsPipelines;

    private:
        RVX::RHIBackendType m_backend = RVX::RHIBackendType::DX12;
        RVX::RHICapabilities m_capabilities;
    };

    const RVX::RHIBindingLayoutEntry* FindBinding(
        const RVX::RHIDescriptorSetLayoutDesc& desc,
        RVX::uint32 binding)
    {
        auto it = std::find_if(desc.entries.begin(), desc.entries.end(),
            [binding](const RVX::RHIBindingLayoutEntry& entry)
            {
                return entry.binding == binding;
            });
        return it == desc.entries.end() ? nullptr : &(*it);
    }

    const RVX::RHIDescriptorBinding* FindDescriptorBinding(
        const RVX::RHIDescriptorSetDesc& desc,
        RVX::uint32 binding)
    {
        auto it = std::find_if(desc.bindings.begin(), desc.bindings.end(),
            [binding](const RVX::RHIDescriptorBinding& entry)
            {
                return entry.binding == binding;
            });
        return it == desc.bindings.end() ? nullptr : &(*it);
    }

    bool HasCompilerAvailable()
    {
        auto shaderDir = FindShaderDirectory();
        return !shaderDir.empty();
    }

    void WriteTextFile(const fs::path& path, const std::string& contents)
    {
        fs::create_directories(path.parent_path());
        std::ofstream file(path, std::ios::trunc);
        file << contents;
    }

    std::string ReadTextFile(const fs::path& path)
    {
        std::ifstream file(path);
        std::ostringstream contents;
        contents << file.rdbuf();
        return contents.str();
    }

    bool TryParseDecimalToken(std::string token, uint32_t& outValue)
    {
        while (!token.empty() && (token.back() == ';' || token.back() == 'u' || token.back() == 'U'))
        {
            token.pop_back();
        }

        if (token.empty())
        {
            return false;
        }

        uint64_t parsedValue = 0;
        for (char ch : token)
        {
            if (ch < '0' || ch > '9')
            {
                return false;
            }

            parsedValue = parsedValue * 10u + static_cast<uint64_t>(ch - '0');
            if (parsedValue > std::numeric_limits<uint32_t>::max())
            {
                return false;
            }
        }

        outValue = static_cast<uint32_t>(parsedValue);
        return true;
    }

    bool TryReadTokenAfterMarker(const std::string& source, const std::string& marker, std::string& outToken)
    {
        const size_t markerOffset = source.find(marker);
        if (markerOffset == std::string::npos)
        {
            return false;
        }

        size_t tokenBegin = markerOffset + marker.size();
        while (tokenBegin < source.size() && (source[tokenBegin] == ' ' || source[tokenBegin] == '\t'))
        {
            ++tokenBegin;
        }

        size_t tokenEnd = tokenBegin;
        while (tokenEnd < source.size() &&
               source[tokenEnd] != ' ' &&
               source[tokenEnd] != '\t' &&
               source[tokenEnd] != '\r' &&
               source[tokenEnd] != '\n')
        {
            ++tokenEnd;
        }

        if (tokenEnd == tokenBegin)
        {
            return false;
        }

        outToken = source.substr(tokenBegin, tokenEnd - tokenBegin);
        return true;
    }

    bool TryReadCppBindingConstant(const std::string& source, const char* name, uint32_t& outValue)
    {
        std::string token;
        if (!TryReadTokenAfterMarker(source, std::string("constexpr uint32 ") + name + " = ", token))
        {
            return false;
        }

        return TryParseDecimalToken(token, outValue);
    }

    bool TryReadHLSLRegisterDefine(const std::string& source, const char* name, uint32_t& outValue)
    {
        std::string token;
        if (!TryReadTokenAfterMarker(source, std::string("#define ") + name + " ", token))
        {
            return false;
        }

        if (token.size() < 2u || (token[0] != 't' && token[0] != 'u' && token[0] != 'b'))
        {
            return false;
        }

        return TryParseDecimalToken(token.substr(1), outValue);
    }

    bool TryReadHLSLUintDefine(const std::string& source, const char* name, uint32_t& outValue)
    {
        std::string token;
        if (!TryReadTokenAfterMarker(source, std::string("#define ") + name + " ", token))
        {
            return false;
        }

        return TryParseDecimalToken(token, outValue);
    }

    std::string TrimExpression(std::string expression)
    {
        size_t begin = 0;
        while (begin < expression.size() && (expression[begin] == ' ' || expression[begin] == '\t'))
        {
            ++begin;
        }

        size_t end = expression.size();
        while (end > begin &&
               (expression[end - 1u] == ' ' ||
                expression[end - 1u] == '\t' ||
                expression[end - 1u] == ',' ||
                expression[end - 1u] == ';'))
        {
            --end;
        }

        return expression.substr(begin, end - begin);
    }

    bool TryReadExpressionAfterMarker(const std::string& source,
                                      const std::string& marker,
                                      std::string& outExpression)
    {
        const size_t markerOffset = source.find(marker);
        if (markerOffset == std::string::npos)
        {
            return false;
        }

        size_t expressionBegin = markerOffset + marker.size();
        while (expressionBegin < source.size() &&
               (source[expressionBegin] == ' ' || source[expressionBegin] == '\t'))
        {
            ++expressionBegin;
        }

        size_t expressionEnd = expressionBegin;
        while (expressionEnd < source.size() &&
               source[expressionEnd] != ',' &&
               source[expressionEnd] != ';' &&
               source[expressionEnd] != '\r' &&
               source[expressionEnd] != '\n')
        {
            ++expressionEnd;
        }

        outExpression = TrimExpression(source.substr(expressionBegin, expressionEnd - expressionBegin));
        return !outExpression.empty();
    }

    bool TryParseUintLiteral(std::string token, uint32_t& outValue)
    {
        token = TrimExpression(token);
        while (!token.empty() && (token.back() == 'u' || token.back() == 'U'))
        {
            token.pop_back();
        }

        if (token.empty())
        {
            return false;
        }

        if (token.size() > 2u && token[0] == '0' && (token[1] == 'x' || token[1] == 'X'))
        {
            uint64_t parsedValue = 0;
            bool hasDigit = false;
            for (size_t index = 2u; index < token.size(); ++index)
            {
                const char ch = token[index];
                uint32_t digit = 0;
                if (ch >= '0' && ch <= '9')
                {
                    digit = static_cast<uint32_t>(ch - '0');
                }
                else if (ch >= 'a' && ch <= 'f')
                {
                    digit = static_cast<uint32_t>(ch - 'a') + 10u;
                }
                else if (ch >= 'A' && ch <= 'F')
                {
                    digit = static_cast<uint32_t>(ch - 'A') + 10u;
                }
                else
                {
                    return false;
                }

                hasDigit = true;
                parsedValue = parsedValue * 16u + digit;
                if (parsedValue > std::numeric_limits<uint32_t>::max())
                {
                    return false;
                }
            }

            if (!hasDigit)
            {
                return false;
            }

            outValue = static_cast<uint32_t>(parsedValue);
            return true;
        }

        return TryParseDecimalToken(token, outValue);
    }

    bool TryEvaluateUintExpression(const std::string& expression, uint32_t& outValue)
    {
        const size_t shiftOffset = expression.find("<<");
        if (shiftOffset == std::string::npos)
        {
            return TryParseUintLiteral(expression, outValue);
        }

        uint32_t left = 0;
        uint32_t shift = 0;
        if (!TryParseUintLiteral(expression.substr(0, shiftOffset), left) ||
            !TryParseUintLiteral(expression.substr(shiftOffset + 2u), shift) ||
            shift >= 32u)
        {
            return false;
        }

        const uint64_t shifted = static_cast<uint64_t>(left) << shift;
        if (shifted > std::numeric_limits<uint32_t>::max())
        {
            return false;
        }

        outValue = static_cast<uint32_t>(shifted);
        return true;
    }

    bool TryReadCppEnumValue(const std::string& source,
                             const char* enumName,
                             const char* valueName,
                             uint32_t& outValue)
    {
        const size_t enumOffset = source.find(std::string("enum class ") + enumName);
        if (enumOffset == std::string::npos)
        {
            return false;
        }

        const size_t bodyBegin = source.find('{', enumOffset);
        if (bodyBegin == std::string::npos)
        {
            return false;
        }

        const size_t bodyEnd = source.find("};", bodyBegin);
        if (bodyEnd == std::string::npos)
        {
            return false;
        }

        const std::string enumBody = source.substr(bodyBegin, bodyEnd - bodyBegin);
        std::string expression;
        if (!TryReadExpressionAfterMarker(enumBody, std::string(valueName) + " = ", expression))
        {
            return false;
        }

        return TryEvaluateUintExpression(expression, outValue);
    }
    bool TryReadCppConstUintExpression(const std::string& source, const char* name, uint32_t& outValue)
    {
        std::string expression;
        if (!TryReadExpressionAfterMarker(source, std::string("constexpr uint32 ") + name + " = ", expression))
        {
            return false;
        }

        return TryEvaluateUintExpression(expression, outValue);
    }

    bool TryReadHLSLStaticUintExpression(const std::string& source, const char* name, uint32_t& outValue)
    {
        std::string expression;
        if (!TryReadExpressionAfterMarker(source, std::string("static const uint ") + name + " = ", expression))
        {
            return false;
        }

        return TryEvaluateUintExpression(expression, outValue);
    }
    fs::path FindModelViewerSourcePath()
    {
        const fs::path shaderDir = FindShaderDirectory();
        if (shaderDir.empty())
        {
            return {};
        }

        const fs::path repoRoot = shaderDir.parent_path().parent_path();
        const fs::path showcasePath = repoRoot / "Samples" / "Showcase" / "ModelViewer" / "main.cpp";
        if (fs::exists(showcasePath))
        {
            return showcasePath;
        }

        const fs::path legacyPath = repoRoot / "Samples" / "ModelViewer" / "main.cpp";
        return fs::exists(legacyPath) ? legacyPath : fs::path{};
    }

    fs::path FindTestsCMakePath()
    {
        const fs::path shaderDir = FindShaderDirectory();
        if (shaderDir.empty())
        {
            return {};
        }

        const fs::path repoRoot = shaderDir.parent_path().parent_path();
        const fs::path testsCMakePath = repoRoot / "Tests" / "CMakeLists.txt";
        return fs::exists(testsCMakePath) ? testsCMakePath : fs::path{};
    }

    fs::path CopyShaderDirectoryToTemp(const fs::path& destinationRoot)
    {
        const fs::path source = FindShaderDirectory();
        EXPECT_FALSE(source.empty());
        const fs::path destination = destinationRoot / "Shaders";
        fs::create_directories(destination);
        fs::copy(source,
                 destination,
                 fs::copy_options::recursive | fs::copy_options::overwrite_existing);
        return destination;
    }

    void ReplaceManifestFieldValue(const fs::path& manifestPath,
                                   const std::string& key,
                                   const std::string& replacementValue)
    {
        std::string manifest = ReadTextFile(manifestPath);
        const std::string prefix = key + "=";
        const size_t fieldOffset = manifest.find(prefix);
        ASSERT_NE(fieldOffset, std::string::npos);

        const size_t valueOffset = fieldOffset + prefix.size();
        const size_t lineEnd = manifest.find('\n', valueOffset);
        ASSERT_NE(lineEnd, std::string::npos);

        manifest.replace(valueOffset, lineEnd - valueOffset, replacementValue);
        WriteTextFile(manifestPath, manifest);
    }

    RVX::PipelineCacheConfig ConfigWithManifest(const fs::path& manifestDirectory)
    {
        RVX::PipelineCacheConfig config;
        config.manifestDirectory = manifestDirectory;
        return config;
    }

    const FakeBuffer* FindCapturedBuffer(const FakeDevice& device, const char* debugName)
    {
        for (size_t i = 0; i < device.capturedBufferDescs.size(); ++i)
        {
            const char* capturedName = device.capturedBufferDescs[i].debugName;
            if (capturedName && std::strcmp(capturedName, debugName) == 0)
            {
                return device.capturedBuffers[i];
            }
        }
        return nullptr;
    }
}

TEST_F(PipelineCacheValidationFixture, NullDeviceFailsWithVisibleError)
{
    RVX::PipelineCache cache;
    EXPECT_FALSE(cache.Initialize(nullptr, ""));
    EXPECT_NE(cache.GetLastError().find("Invalid device"), std::string::npos);
}

TEST_F(PipelineCacheValidationFixture, MissingShaderFileFailsWithVisibleError)
{
    TempDirectory temp("rvx_pipeline_missing_shader");
    FakeDevice device;
    RVX::PipelineCache cache;

    EXPECT_FALSE(cache.Initialize(&device, temp.Path().string()));
    EXPECT_NE(cache.GetLastError().find("Shader file not found"), std::string::npos);
}

TEST_F(PipelineCacheValidationFixture, FailedShaderCreationFailsWithVisibleError)
{
    if (!HasCompilerAvailable())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    FakeDevice device;
    device.failShaderCreation = true;

    RVX::PipelineCache cache;
    EXPECT_FALSE(cache.Initialize(&device, FindShaderDirectory().string()));
    EXPECT_NE(cache.GetLastError().find("vertex shader"), std::string::npos);
}

TEST_F(PipelineCacheValidationFixture, MissingToneMappingShaderFailsWithVisibleError)
{
    if (!HasCompilerAvailable())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    const fs::path sourceDir = FindShaderDirectory();
    TempDirectory temp("rvx_pipeline_missing_tonemapping_shader");
    fs::copy_file(sourceDir / "DefaultLit.hlsl", temp.Path() / "DefaultLit.hlsl");
    fs::copy_file(sourceDir / "DepthOnly.hlsl", temp.Path() / "DepthOnly.hlsl");
    fs::copy(sourceDir / "Include", temp.Path() / "Include", fs::copy_options::recursive);
    fs::create_directories(temp.Path() / "PostProcess");

    FakeDevice device;
    RVX::PipelineCache cache;

    EXPECT_FALSE(cache.Initialize(&device, temp.Path().string()));
    EXPECT_NE(cache.GetLastError().find("ToneMapping shader file not found"), std::string::npos);
}

TEST_F(PipelineCacheValidationFixture, MissingBloomShaderFailsWithVisibleError)
{
    if (!HasCompilerAvailable())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    const fs::path sourceDir = FindShaderDirectory();
    TempDirectory temp("rvx_pipeline_missing_bloom_shader");
    fs::copy_file(sourceDir / "DefaultLit.hlsl", temp.Path() / "DefaultLit.hlsl");
    fs::copy_file(sourceDir / "DepthOnly.hlsl", temp.Path() / "DepthOnly.hlsl");
    fs::copy(sourceDir / "Include", temp.Path() / "Include", fs::copy_options::recursive);
    fs::create_directories(temp.Path() / "PostProcess");
    fs::copy_file(sourceDir / "PostProcess" / "ToneMapping.hlsl",
                  temp.Path() / "PostProcess" / "ToneMapping.hlsl");

    FakeDevice device;
    RVX::PipelineCache cache;

    EXPECT_FALSE(cache.Initialize(&device, temp.Path().string()));
    EXPECT_NE(cache.GetLastError().find("Bloom shader file not found"), std::string::npos);
}

TEST_F(PipelineCacheValidationFixture, MissingCameraVelocityShaderFailsWithVisibleError)
{
    if (!HasCompilerAvailable())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    TempDirectory temp("rvx_pipeline_missing_camera_velocity_shader");
    const fs::path shaderDir = CopyShaderDirectoryToTemp(temp.Path());
    fs::remove(shaderDir / "PostProcess" / "CameraVelocity.hlsl");

    FakeDevice device;
    RVX::PipelineCache cache;

    EXPECT_FALSE(cache.Initialize(&device, shaderDir.string()));
    EXPECT_NE(cache.GetLastError().find("CameraVelocity shader file not found"), std::string::npos);
}

TEST_F(PipelineCacheValidationFixture, MissingObjectVelocityShaderFailsWithVisibleError)
{
    if (!HasCompilerAvailable())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    TempDirectory temp("rvx_pipeline_missing_object_velocity_shader");
    const fs::path shaderDir = CopyShaderDirectoryToTemp(temp.Path());
    fs::remove(shaderDir / "ObjectVelocity.hlsl");

    FakeDevice device;
    RVX::PipelineCache cache;

    EXPECT_FALSE(cache.Initialize(&device, shaderDir.string()));
    EXPECT_NE(cache.GetLastError().find("ObjectVelocity shader file not found"), std::string::npos);
}
TEST_F(PipelineCacheValidationFixture, MissingFXAAShaderFailsWithVisibleError)
{
    if (!HasCompilerAvailable())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    const fs::path sourceDir = FindShaderDirectory();
    TempDirectory temp("rvx_pipeline_missing_fxaa_shader");
    fs::copy_file(sourceDir / "DefaultLit.hlsl", temp.Path() / "DefaultLit.hlsl");
    fs::copy_file(sourceDir / "DepthOnly.hlsl", temp.Path() / "DepthOnly.hlsl");
    fs::copy(sourceDir / "Include", temp.Path() / "Include", fs::copy_options::recursive);
    fs::create_directories(temp.Path() / "PostProcess");
    fs::copy_file(sourceDir / "PostProcess" / "ToneMapping.hlsl",
                  temp.Path() / "PostProcess" / "ToneMapping.hlsl");
    fs::copy_file(sourceDir / "PostProcess" / "Bloom.hlsl",
                  temp.Path() / "PostProcess" / "Bloom.hlsl");
    fs::copy_file(sourceDir / "PostProcess" / "CameraVelocity.hlsl",
                  temp.Path() / "PostProcess" / "CameraVelocity.hlsl");
    fs::copy_file(sourceDir / "ObjectVelocity.hlsl", temp.Path() / "ObjectVelocity.hlsl");
    fs::copy_file(sourceDir / "PostProcess" / "ColorGrading.hlsl",
                  temp.Path() / "PostProcess" / "ColorGrading.hlsl");
    fs::copy_file(sourceDir / "PostProcess" / "ChromaticAberration.hlsl",
                  temp.Path() / "PostProcess" / "ChromaticAberration.hlsl");

    FakeDevice device;
    RVX::PipelineCache cache;

    EXPECT_FALSE(cache.Initialize(&device, temp.Path().string()));
    EXPECT_NE(cache.GetLastError().find("FXAA shader file not found"), std::string::npos);
}

TEST_F(PipelineCacheValidationFixture, MissingChromaticAberrationShaderFailsWithVisibleError)
{
    if (!HasCompilerAvailable())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    const fs::path sourceDir = FindShaderDirectory();
    TempDirectory temp("rvx_pipeline_missing_chromatic_aberration_shader");
    fs::copy_file(sourceDir / "DefaultLit.hlsl", temp.Path() / "DefaultLit.hlsl");
    fs::copy_file(sourceDir / "DepthOnly.hlsl", temp.Path() / "DepthOnly.hlsl");
    fs::copy(sourceDir / "Include", temp.Path() / "Include", fs::copy_options::recursive);
    fs::create_directories(temp.Path() / "PostProcess");
    fs::copy_file(sourceDir / "PostProcess" / "ToneMapping.hlsl",
                  temp.Path() / "PostProcess" / "ToneMapping.hlsl");
    fs::copy_file(sourceDir / "PostProcess" / "Bloom.hlsl",
                  temp.Path() / "PostProcess" / "Bloom.hlsl");
    fs::copy_file(sourceDir / "PostProcess" / "CameraVelocity.hlsl",
                  temp.Path() / "PostProcess" / "CameraVelocity.hlsl");
    fs::copy_file(sourceDir / "ObjectVelocity.hlsl", temp.Path() / "ObjectVelocity.hlsl");
    fs::copy_file(sourceDir / "PostProcess" / "ColorGrading.hlsl",
                  temp.Path() / "PostProcess" / "ColorGrading.hlsl");

    FakeDevice device;
    RVX::PipelineCache cache;

    EXPECT_FALSE(cache.Initialize(&device, temp.Path().string()));
    EXPECT_NE(cache.GetLastError().find("ChromaticAberration shader file not found"), std::string::npos);
}

TEST_F(PipelineCacheValidationFixture, MissingColorGradingShaderFailsWithVisibleError)
{
    if (!HasCompilerAvailable())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    const fs::path sourceDir = FindShaderDirectory();
    TempDirectory temp("rvx_pipeline_missing_colorgrading_shader");
    fs::copy_file(sourceDir / "DefaultLit.hlsl", temp.Path() / "DefaultLit.hlsl");
    fs::copy_file(sourceDir / "DepthOnly.hlsl", temp.Path() / "DepthOnly.hlsl");
    fs::copy(sourceDir / "Include", temp.Path() / "Include", fs::copy_options::recursive);
    fs::create_directories(temp.Path() / "PostProcess");
    fs::copy_file(sourceDir / "PostProcess" / "ToneMapping.hlsl",
                  temp.Path() / "PostProcess" / "ToneMapping.hlsl");
    fs::copy_file(sourceDir / "PostProcess" / "Bloom.hlsl",
                  temp.Path() / "PostProcess" / "Bloom.hlsl");
    fs::copy_file(sourceDir / "PostProcess" / "CameraVelocity.hlsl",
                  temp.Path() / "PostProcess" / "CameraVelocity.hlsl");
    fs::copy_file(sourceDir / "ObjectVelocity.hlsl", temp.Path() / "ObjectVelocity.hlsl");

    FakeDevice device;
    RVX::PipelineCache cache;

    EXPECT_FALSE(cache.Initialize(&device, temp.Path().string()));
    EXPECT_NE(cache.GetLastError().find("ColorGrading shader file not found"), std::string::npos);
}

TEST_F(PipelineCacheValidationFixture, MissingVignetteShaderFailsWithVisibleError)
{
    if (!HasCompilerAvailable())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    const fs::path sourceDir = FindShaderDirectory();
    TempDirectory temp("rvx_pipeline_missing_vignette_shader");
    fs::copy_file(sourceDir / "DefaultLit.hlsl", temp.Path() / "DefaultLit.hlsl");
    fs::copy_file(sourceDir / "DepthOnly.hlsl", temp.Path() / "DepthOnly.hlsl");
    fs::copy(sourceDir / "Include", temp.Path() / "Include", fs::copy_options::recursive);
    fs::create_directories(temp.Path() / "PostProcess");
    fs::copy_file(sourceDir / "PostProcess" / "ToneMapping.hlsl",
                  temp.Path() / "PostProcess" / "ToneMapping.hlsl");
    fs::copy_file(sourceDir / "PostProcess" / "Bloom.hlsl",
                  temp.Path() / "PostProcess" / "Bloom.hlsl");
    fs::copy_file(sourceDir / "PostProcess" / "CameraVelocity.hlsl",
                  temp.Path() / "PostProcess" / "CameraVelocity.hlsl");
    fs::copy_file(sourceDir / "ObjectVelocity.hlsl", temp.Path() / "ObjectVelocity.hlsl");
    fs::copy_file(sourceDir / "PostProcess" / "ColorGrading.hlsl",
                  temp.Path() / "PostProcess" / "ColorGrading.hlsl");
    fs::copy_file(sourceDir / "PostProcess" / "ChromaticAberration.hlsl",
                  temp.Path() / "PostProcess" / "ChromaticAberration.hlsl");
    fs::copy_file(sourceDir / "PostProcess" / "FXAA.hlsl",
                  temp.Path() / "PostProcess" / "FXAA.hlsl");

    FakeDevice device;
    RVX::PipelineCache cache;

    EXPECT_FALSE(cache.Initialize(&device, temp.Path().string()));
    EXPECT_NE(cache.GetLastError().find("Vignette shader file not found"), std::string::npos);
}

TEST_F(PipelineCacheValidationFixture, MissingSkyboxShaderFailsWithVisibleError)
{
    if (!HasCompilerAvailable())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    const fs::path sourceDir = FindShaderDirectory();
    TempDirectory temp("rvx_pipeline_missing_skybox_shader");
    fs::copy_file(sourceDir / "DefaultLit.hlsl", temp.Path() / "DefaultLit.hlsl");
    fs::copy_file(sourceDir / "DepthOnly.hlsl", temp.Path() / "DepthOnly.hlsl");
    fs::copy(sourceDir / "Include", temp.Path() / "Include", fs::copy_options::recursive);
    fs::create_directories(temp.Path() / "PostProcess");
    fs::copy_file(sourceDir / "PostProcess" / "ToneMapping.hlsl",
                  temp.Path() / "PostProcess" / "ToneMapping.hlsl");
    fs::copy_file(sourceDir / "PostProcess" / "Bloom.hlsl",
                  temp.Path() / "PostProcess" / "Bloom.hlsl");
    fs::copy_file(sourceDir / "PostProcess" / "CameraVelocity.hlsl",
                  temp.Path() / "PostProcess" / "CameraVelocity.hlsl");
    fs::copy_file(sourceDir / "ObjectVelocity.hlsl", temp.Path() / "ObjectVelocity.hlsl");
    fs::copy_file(sourceDir / "PostProcess" / "ColorGrading.hlsl",
                  temp.Path() / "PostProcess" / "ColorGrading.hlsl");
    fs::copy_file(sourceDir / "PostProcess" / "ChromaticAberration.hlsl",
                  temp.Path() / "PostProcess" / "ChromaticAberration.hlsl");
    fs::copy_file(sourceDir / "PostProcess" / "FXAA.hlsl",
                  temp.Path() / "PostProcess" / "FXAA.hlsl");
    fs::copy_file(sourceDir / "PostProcess" / "Vignette.hlsl",
                  temp.Path() / "PostProcess" / "Vignette.hlsl");
    fs::copy_file(sourceDir / "UI.hlsl", temp.Path() / "UI.hlsl");

    FakeDevice device;
    RVX::PipelineCache cache;

    EXPECT_FALSE(cache.Initialize(&device, temp.Path().string()));
    EXPECT_NE(cache.GetLastError().find("Skybox shader file not found"), std::string::npos);
}

TEST_F(PipelineCacheValidationFixture, MissingUIShaderFailsWithVisibleError)
{
    if (!HasCompilerAvailable())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    const fs::path sourceDir = FindShaderDirectory();
    TempDirectory temp("rvx_pipeline_missing_ui_shader");
    fs::copy_file(sourceDir / "DefaultLit.hlsl", temp.Path() / "DefaultLit.hlsl");
    fs::copy_file(sourceDir / "DepthOnly.hlsl", temp.Path() / "DepthOnly.hlsl");
    fs::copy(sourceDir / "Include", temp.Path() / "Include", fs::copy_options::recursive);
    fs::create_directories(temp.Path() / "PostProcess");
    fs::copy_file(sourceDir / "PostProcess" / "ToneMapping.hlsl",
                  temp.Path() / "PostProcess" / "ToneMapping.hlsl");
    fs::copy_file(sourceDir / "PostProcess" / "Bloom.hlsl",
                  temp.Path() / "PostProcess" / "Bloom.hlsl");
    fs::copy_file(sourceDir / "PostProcess" / "CameraVelocity.hlsl",
                  temp.Path() / "PostProcess" / "CameraVelocity.hlsl");
    fs::copy_file(sourceDir / "ObjectVelocity.hlsl", temp.Path() / "ObjectVelocity.hlsl");
    fs::copy_file(sourceDir / "PostProcess" / "ColorGrading.hlsl",
                  temp.Path() / "PostProcess" / "ColorGrading.hlsl");
    fs::copy_file(sourceDir / "PostProcess" / "ChromaticAberration.hlsl",
                  temp.Path() / "PostProcess" / "ChromaticAberration.hlsl");
    fs::copy_file(sourceDir / "PostProcess" / "FXAA.hlsl",
                  temp.Path() / "PostProcess" / "FXAA.hlsl");
    fs::copy_file(sourceDir / "PostProcess" / "Vignette.hlsl",
                  temp.Path() / "PostProcess" / "Vignette.hlsl");

    FakeDevice device;
    RVX::PipelineCache cache;

    EXPECT_FALSE(cache.Initialize(&device, temp.Path().string()));
    EXPECT_NE(cache.GetLastError().find("UI shader file not found"), std::string::npos);
}

TEST_F(PipelineCacheValidationFixture, MissingRayTracedReflectionCompositeShaderFailsWithVisibleError)
{
    if (!HasCompilerAvailable())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    TempDirectory temp("rvx_pipeline_missing_rt_reflection_composite_shader");
    const fs::path shaderDir = CopyShaderDirectoryToTemp(temp.Path());
    fs::remove(shaderDir / "PostProcess" / "RayTracedReflectionComposite.hlsl");

    FakeDevice device;
    RVX::PipelineCache cache;

    EXPECT_FALSE(cache.Initialize(&device, shaderDir.string()));
    EXPECT_NE(cache.GetLastError().find("RayTracedReflectionComposite shader file not found"),
              std::string::npos);
}

TEST_F(PipelineCacheValidationFixture, MissingRayTracedReflectionDenoiseShaderFailsWithVisibleError)
{
    if (!HasCompilerAvailable())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    TempDirectory temp("rvx_pipeline_missing_rt_reflection_denoise_shader");
    const fs::path shaderDir = CopyShaderDirectoryToTemp(temp.Path());
    fs::remove(shaderDir / "PostProcess" / "RayTracedReflectionDenoise.hlsl");

    FakeDevice device;
    RVX::PipelineCache cache;

    EXPECT_FALSE(cache.Initialize(&device, shaderDir.string()));
    EXPECT_NE(cache.GetLastError().find("RayTracedReflectionDenoise shader file not found"),
              std::string::npos);
}

TEST_F(PipelineCacheValidationFixture, SkyboxShaderUsesFullscreenTriangleAndProceduralGradient)
{
    if (!HasCompilerAvailable())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    const std::string shaderSource = ReadTextFile(FindShaderDirectory() / "Skybox.hlsl");

    EXPECT_NE(shaderSource.find("SV_VertexID"), std::string::npos);
    EXPECT_NE(shaderSource.find("SkyboxGroundColor.a"), std::string::npos);
    EXPECT_NE(shaderSource.find("SkyboxZenithColor"), std::string::npos);
    EXPECT_NE(shaderSource.find("SkyboxHorizonColor"), std::string::npos);
    EXPECT_NE(shaderSource.find("TextureCube SkyboxCubemap"), std::string::npos);
    EXPECT_NE(shaderSource.find("SamplerState SkyboxSampler"), std::string::npos);
    EXPECT_NE(shaderSource.find("SkyboxTextureParams.x"), std::string::npos);
    EXPECT_NE(shaderSource.find("smoothstep"), std::string::npos);
}

TEST_F(PipelineCacheValidationFixture, ToneMappingShaderUsesSingleDisplayConversion)
{
    if (!HasCompilerAvailable())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    const std::string shaderSource =
        ReadTextFile(FindShaderDirectory() / "PostProcess" / "ToneMapping.hlsl");

    auto countOccurrences = [](const std::string& text, const std::string& needle)
    {
        size_t count = 0;
        size_t offset = 0;
        while ((offset = text.find(needle, offset)) != std::string::npos)
        {
            ++count;
            offset += needle.size();
        }
        return count;
    };

    EXPECT_EQ(shaderSource.find("LinearToSRGB"), std::string::npos);
    EXPECT_NE(shaderSource.find("#define TONE_MAPPING_OUTPUT_LINEAR 0"), std::string::npos);
    EXPECT_NE(shaderSource.find("#define TONE_MAPPING_OUTPUT_SRGB 1"), std::string::npos);
    EXPECT_NE(shaderSource.find("uint OutputColorSpace;"), std::string::npos);
    EXPECT_NE(shaderSource.find("if (outputColorSpace == TONE_MAPPING_OUTPUT_LINEAR)"), std::string::npos);
    EXPECT_NE(shaderSource.find("return linearColor;"), std::string::npos);
    EXPECT_NE(shaderSource.find("ApplyDisplayConversion(ldr, Gamma, OutputColorSpace)"), std::string::npos);
    EXPECT_EQ(countOccurrences(shaderSource, "ApplyDisplayConversion("), static_cast<size_t>(2));
    EXPECT_EQ(countOccurrences(shaderSource, "pow("), static_cast<size_t>(1));
}

TEST_F(PipelineCacheValidationFixture, BloomShaderUsesMipChainCompositeModes)
{
    if (!HasCompilerAvailable())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    const std::string shaderSource =
        ReadTextFile(FindShaderDirectory() / "PostProcess" / "Bloom.hlsl");

    auto countOccurrences = [](const std::string& text, const std::string& needle)
    {
        size_t count = 0;
        size_t offset = 0;
        while ((offset = text.find(needle, offset)) != std::string::npos)
        {
            ++count;
            offset += needle.size();
        }
        return count;
    };

    EXPECT_NE(shaderSource.find("ApplySoftThreshold"), std::string::npos);
    EXPECT_NE(shaderSource.find("SampleBloomThreshold"), std::string::npos);
    EXPECT_NE(shaderSource.find("float4 Mode_Padding;"), std::string::npos);
    EXPECT_NE(shaderSource.find("#define Mode Mode_Padding.x"), std::string::npos);
    EXPECT_NE(shaderSource.find("BLOOM_MODE_COPY_SCENE"), std::string::npos);
    EXPECT_NE(shaderSource.find("BLOOM_MODE_EXTRACT"), std::string::npos);
    EXPECT_NE(shaderSource.find("BLOOM_MODE_DOWNSAMPLE"), std::string::npos);
    EXPECT_NE(shaderSource.find("BLOOM_MODE_COMPOSITE_ADDITIVE"), std::string::npos);
    EXPECT_NE(shaderSource.find("SampleBloomWideKernel"), std::string::npos);
    EXPECT_NE(shaderSource.find("return scene;"), std::string::npos);
    EXPECT_NE(shaderSource.find("ring1"), std::string::npos);
    EXPECT_NE(shaderSource.find("ring2"), std::string::npos);
    EXPECT_NE(shaderSource.find("float2 ring2 = radius * 2.0;"), std::string::npos);
    EXPECT_GE(countOccurrences(shaderSource, "SampleBloomThreshold("),
              static_cast<size_t>(13));
    EXPECT_NE(shaderSource.find("return float4(bloom * max(Intensity, 0.0), 0.0);"), std::string::npos);
    EXPECT_EQ(shaderSource.find("tiny fullscreen-neighborhood"), std::string::npos);
    EXPECT_EQ(shaderSource.find("deliberately small one-pass"), std::string::npos);
    EXPECT_EQ(shaderSource.find("Minimum fullscreen bloom path"), std::string::npos);
    EXPECT_EQ(shaderSource.find("single-pass wide-kernel bloom approximation"), std::string::npos);
}

TEST_F(PipelineCacheValidationFixture, FXAAShaderUsesPostProcessDescriptorLayout)
{
    if (!HasCompilerAvailable())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    const std::string shaderSource =
        ReadTextFile(FindShaderDirectory() / "PostProcess" / "FXAA.hlsl");

    EXPECT_NE(shaderSource.find("cbuffer FXAAConstants : register(b0, space0)"), std::string::npos);
    EXPECT_NE(shaderSource.find("Texture2D<float4> InputTexture : register(t1, space0)"), std::string::npos);
    EXPECT_NE(shaderSource.find("SamplerState LinearSampler : register(s2, space0)"), std::string::npos);
}

TEST_F(PipelineCacheValidationFixture, VignetteShaderUsesPostProcessDescriptorLayout)
{
    if (!HasCompilerAvailable())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    const std::string shaderSource =
        ReadTextFile(FindShaderDirectory() / "PostProcess" / "Vignette.hlsl");

    EXPECT_NE(shaderSource.find("cbuffer VignetteConstants : register(b0, space0)"), std::string::npos);
    EXPECT_NE(shaderSource.find("Texture2D<float4> InputTexture : register(t1, space0)"), std::string::npos);
    EXPECT_NE(shaderSource.find("SamplerState LinearSampler : register(s2, space0)"), std::string::npos);
}

TEST_F(PipelineCacheValidationFixture, ColorGradingShaderUsesPostProcessDescriptorLayout)
{
    if (!HasCompilerAvailable())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    const std::string shaderSource =
        ReadTextFile(FindShaderDirectory() / "PostProcess" / "ColorGrading.hlsl");

    EXPECT_NE(shaderSource.find("cbuffer ColorGradingConstants : register(b0, space0)"), std::string::npos);
    EXPECT_NE(shaderSource.find("Texture2D<float4> InputTexture : register(t1, space0)"), std::string::npos);
    EXPECT_NE(shaderSource.find("SamplerState LinearSampler : register(s2, space0)"), std::string::npos);
    EXPECT_EQ(shaderSource.find("RWTexture2D"), std::string::npos);
}

TEST_F(PipelineCacheValidationFixture, ChromaticAberrationShaderUsesPostProcessDescriptorLayout)
{
    if (!HasCompilerAvailable())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    const std::string shaderSource =
        ReadTextFile(FindShaderDirectory() / "PostProcess" / "ChromaticAberration.hlsl");

    EXPECT_NE(shaderSource.find("cbuffer ChromaticAberrationConstants : register(b0, space0)"),
              std::string::npos);
    EXPECT_NE(shaderSource.find("Texture2D<float4> InputTexture : register(t1, space0)"), std::string::npos);
    EXPECT_NE(shaderSource.find("SamplerState LinearSampler : register(s2, space0)"), std::string::npos);
    EXPECT_NE(shaderSource.find("VSMain"), std::string::npos);
    EXPECT_NE(shaderSource.find("PSMain"), std::string::npos);
    EXPECT_EQ(shaderSource.find("RWTexture2D"), std::string::npos);
}

TEST_F(PipelineCacheValidationFixture, ReflectionBuildsDefaultLitLayouts)
{
    if (!HasCompilerAvailable())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    FakeDevice device;
    RVX::PipelineCache cache;

    ASSERT_TRUE(cache.Initialize(&device, FindShaderDirectory().string())) << cache.GetLastError();
    ASSERT_GE(device.capturedSetLayouts.size(), 6u);
    ASSERT_GE(device.capturedPipelineLayoutSetCounts.size(), 3u);
    EXPECT_EQ(device.capturedPipelineLayoutSetCounts[0], 3u);
    EXPECT_EQ(device.capturedPipelineLayoutSetCounts[1], 1u);
    EXPECT_EQ(device.capturedPipelineLayoutSetCounts[2], 1u);

    const auto& frameLayout = device.capturedSetLayouts[0];
    const auto& objectLayout = device.capturedSetLayouts[1];
    const auto& materialLayout = device.capturedSetLayouts[2];
    const auto& postProcessLayout = device.capturedSetLayouts[3];
    const auto& uiLayout = device.capturedSetLayouts[4];
    const auto& skyboxLayout = device.capturedSetLayouts[5];

    const auto* frame = FindBinding(frameLayout, 0);
    ASSERT_NE(frame, nullptr);
    EXPECT_EQ(frame->type, RVX::RHIBindingType::UniformBuffer);
    EXPECT_TRUE(RVX::HasFlag(frame->visibility, RVX::RHIShaderStage::Vertex));
    EXPECT_TRUE(RVX::HasFlag(frame->visibility, RVX::RHIShaderStage::Pixel));
    const auto* frameShadowTexture = FindBinding(frameLayout, 1);
    ASSERT_NE(frameShadowTexture, nullptr);
    EXPECT_EQ(frameShadowTexture->type, RVX::RHIBindingType::SampledTexture);
    EXPECT_TRUE(RVX::HasFlag(frameShadowTexture->visibility, RVX::RHIShaderStage::Pixel));
    const auto* frameShadowSampler = FindBinding(frameLayout, 2);
    ASSERT_NE(frameShadowSampler, nullptr);
    EXPECT_EQ(frameShadowSampler->type, RVX::RHIBindingType::Sampler);
    EXPECT_TRUE(RVX::HasFlag(frameShadowSampler->visibility, RVX::RHIShaderStage::Pixel));
    const auto* frameLightConstants = FindBinding(frameLayout, 3);
    ASSERT_NE(frameLightConstants, nullptr);
    EXPECT_EQ(frameLightConstants->type, RVX::RHIBindingType::UniformBuffer);
    EXPECT_TRUE(RVX::HasFlag(frameLightConstants->visibility, RVX::RHIShaderStage::Pixel));
    const auto* framePointLights = FindBinding(frameLayout, 4);
    ASSERT_NE(framePointLights, nullptr);
    EXPECT_EQ(framePointLights->type, RVX::RHIBindingType::ShaderResourceBuffer);
    EXPECT_TRUE(RVX::HasFlag(framePointLights->visibility, RVX::RHIShaderStage::Pixel));
    const auto* frameSpotLights = FindBinding(frameLayout, 5);
    ASSERT_NE(frameSpotLights, nullptr);
    EXPECT_EQ(frameSpotLights->type, RVX::RHIBindingType::ShaderResourceBuffer);
    EXPECT_TRUE(RVX::HasFlag(frameSpotLights->visibility, RVX::RHIShaderStage::Pixel));
    const auto* frameRayTracedShadowMask = FindBinding(frameLayout, 6);
    ASSERT_NE(frameRayTracedShadowMask, nullptr);
    EXPECT_EQ(frameRayTracedShadowMask->type, RVX::RHIBindingType::SampledTexture);
    EXPECT_TRUE(RVX::HasFlag(frameRayTracedShadowMask->visibility, RVX::RHIShaderStage::Pixel));
    const auto* frameClusterConstants = FindBinding(frameLayout, 7);
    ASSERT_NE(frameClusterConstants, nullptr);
    EXPECT_EQ(frameClusterConstants->type, RVX::RHIBindingType::UniformBuffer);
    EXPECT_TRUE(RVX::HasFlag(frameClusterConstants->visibility, RVX::RHIShaderStage::Pixel));
    const auto* frameClusterData = FindBinding(frameLayout, 8);
    ASSERT_NE(frameClusterData, nullptr);
    EXPECT_EQ(frameClusterData->type, RVX::RHIBindingType::ShaderResourceBuffer);
    EXPECT_TRUE(RVX::HasFlag(frameClusterData->visibility, RVX::RHIShaderStage::Pixel));
    const auto* frameClusterLightIndices = FindBinding(frameLayout, 9);
    ASSERT_NE(frameClusterLightIndices, nullptr);
    EXPECT_EQ(frameClusterLightIndices->type, RVX::RHIBindingType::ShaderResourceBuffer);
    EXPECT_TRUE(RVX::HasFlag(frameClusterLightIndices->visibility, RVX::RHIShaderStage::Pixel));

    ASSERT_FALSE(device.capturedDescriptorSetDescs.empty());
    const RVX::RHIDescriptorSetDesc& frameSetDesc = device.capturedDescriptorSetDescs.front();
    EXPECT_NE(FindDescriptorBinding(frameSetDesc, 0), nullptr);
    const auto* fallbackShadowTexture = FindDescriptorBinding(frameSetDesc, 1);
    ASSERT_NE(fallbackShadowTexture, nullptr);
    EXPECT_NE(fallbackShadowTexture->textureView, nullptr);
    ASSERT_NE(fallbackShadowTexture->textureView->GetTexture(), nullptr);
    EXPECT_EQ(fallbackShadowTexture->textureView->GetTexture()->GetArraySize(),
              RVX::RVX_MAX_DIRECTIONAL_SHADOW_CASCADES);
    EXPECT_EQ(fallbackShadowTexture->textureView->GetSubresourceRange().arrayLayerCount,
              RVX::RVX_ALL_LAYERS);
    const auto* fallbackShadowSampler = FindDescriptorBinding(frameSetDesc, 2);
    ASSERT_NE(fallbackShadowSampler, nullptr);
    EXPECT_NE(fallbackShadowSampler->sampler, nullptr);
    const auto* fallbackLightConstants = FindDescriptorBinding(frameSetDesc, 3);
    ASSERT_NE(fallbackLightConstants, nullptr);
    ASSERT_NE(fallbackLightConstants->buffer, nullptr);
    EXPECT_TRUE(RVX::HasFlag(fallbackLightConstants->buffer->GetUsage(), RVX::RHIBufferUsage::Constant));
    const auto* fallbackPointLights = FindDescriptorBinding(frameSetDesc, 4);
    ASSERT_NE(fallbackPointLights, nullptr);
    ASSERT_NE(fallbackPointLights->buffer, nullptr);
    EXPECT_TRUE(RVX::HasFlag(fallbackPointLights->buffer->GetUsage(), RVX::RHIBufferUsage::Structured));
    EXPECT_TRUE(RVX::HasFlag(fallbackPointLights->buffer->GetUsage(), RVX::RHIBufferUsage::ShaderResource));
    EXPECT_EQ(fallbackPointLights->buffer->GetStride(), sizeof(RVX::GPUPointLight));
    const auto* fallbackSpotLights = FindDescriptorBinding(frameSetDesc, 5);
    ASSERT_NE(fallbackSpotLights, nullptr);
    ASSERT_NE(fallbackSpotLights->buffer, nullptr);
    EXPECT_TRUE(RVX::HasFlag(fallbackSpotLights->buffer->GetUsage(), RVX::RHIBufferUsage::Structured));
    EXPECT_TRUE(RVX::HasFlag(fallbackSpotLights->buffer->GetUsage(), RVX::RHIBufferUsage::ShaderResource));
    EXPECT_EQ(fallbackSpotLights->buffer->GetStride(), sizeof(RVX::GPUSpotLight));
    const auto* fallbackRayTracedShadowMask = FindDescriptorBinding(frameSetDesc, 6);
    ASSERT_NE(fallbackRayTracedShadowMask, nullptr);
    EXPECT_NE(fallbackRayTracedShadowMask->textureView, nullptr);
    ASSERT_NE(fallbackRayTracedShadowMask->textureView->GetTexture(), nullptr);
    EXPECT_EQ(fallbackRayTracedShadowMask->textureView->GetTexture()->GetFormat(),
              RVX::RHIFormat::R8_UNORM);
    const auto* fallbackClusterConstants = FindDescriptorBinding(frameSetDesc, 7);
    ASSERT_NE(fallbackClusterConstants, nullptr);
    ASSERT_NE(fallbackClusterConstants->buffer, nullptr);
    EXPECT_TRUE(RVX::HasFlag(fallbackClusterConstants->buffer->GetUsage(), RVX::RHIBufferUsage::Constant));
    const auto* fallbackClusterData = FindDescriptorBinding(frameSetDesc, 8);
    ASSERT_NE(fallbackClusterData, nullptr);
    ASSERT_NE(fallbackClusterData->buffer, nullptr);
    EXPECT_TRUE(RVX::HasFlag(fallbackClusterData->buffer->GetUsage(), RVX::RHIBufferUsage::Structured));
    EXPECT_TRUE(RVX::HasFlag(fallbackClusterData->buffer->GetUsage(), RVX::RHIBufferUsage::ShaderResource));
    EXPECT_EQ(fallbackClusterData->buffer->GetStride(), sizeof(RVX::GPUCluster));
    const auto* fallbackClusterLightIndices = FindDescriptorBinding(frameSetDesc, 9);
    ASSERT_NE(fallbackClusterLightIndices, nullptr);
    ASSERT_NE(fallbackClusterLightIndices->buffer, nullptr);
    EXPECT_TRUE(RVX::HasFlag(fallbackClusterLightIndices->buffer->GetUsage(), RVX::RHIBufferUsage::Structured));
    EXPECT_TRUE(RVX::HasFlag(fallbackClusterLightIndices->buffer->GetUsage(), RVX::RHIBufferUsage::ShaderResource));
    EXPECT_EQ(fallbackClusterLightIndices->buffer->GetStride(), sizeof(RVX::LightIndex));

    const auto* object = FindBinding(objectLayout, 0);
    ASSERT_NE(object, nullptr);
    EXPECT_EQ(object->type, RVX::RHIBindingType::DynamicUniformBuffer);
    EXPECT_TRUE(object->isDynamic);

    const auto* materialConstants = FindBinding(materialLayout, 0);
    ASSERT_NE(materialConstants, nullptr);
    EXPECT_EQ(materialConstants->type, RVX::RHIBindingType::DynamicUniformBuffer);
    EXPECT_TRUE(materialConstants->isDynamic);

    for (RVX::uint32 binding = 1; binding <= 5; ++binding)
    {
        const auto* texture = FindBinding(materialLayout, binding);
        ASSERT_NE(texture, nullptr);
        EXPECT_EQ(texture->type, RVX::RHIBindingType::SampledTexture);
    }

    const auto* sampler = FindBinding(materialLayout, 6);
    ASSERT_NE(sampler, nullptr);
    EXPECT_EQ(sampler->type, RVX::RHIBindingType::Sampler);

    for (RVX::uint32 binding = 7; binding <= 9; ++binding)
    {
        const auto* iblTexture = FindBinding(materialLayout, binding);
        ASSERT_NE(iblTexture, nullptr);
        EXPECT_EQ(iblTexture->type, RVX::RHIBindingType::SampledTexture);
    }

    const auto* postProcessConstants = FindBinding(postProcessLayout, 0);
    ASSERT_NE(postProcessConstants, nullptr);
    EXPECT_EQ(postProcessConstants->type, RVX::RHIBindingType::UniformBuffer);
    EXPECT_TRUE(RVX::HasFlag(postProcessConstants->visibility, RVX::RHIShaderStage::Pixel));

    const auto* postProcessTexture = FindBinding(postProcessLayout, 1);
    ASSERT_NE(postProcessTexture, nullptr);
    EXPECT_EQ(postProcessTexture->type, RVX::RHIBindingType::SampledTexture);
    EXPECT_TRUE(RVX::HasFlag(postProcessTexture->visibility, RVX::RHIShaderStage::Pixel));

    const auto* postProcessSampler = FindBinding(postProcessLayout, 2);
    ASSERT_NE(postProcessSampler, nullptr);
    EXPECT_EQ(postProcessSampler->type, RVX::RHIBindingType::Sampler);
    EXPECT_TRUE(RVX::HasFlag(postProcessSampler->visibility, RVX::RHIShaderStage::Pixel));

    EXPECT_NE(cache.GetPostProcessSetLayout(), nullptr);
    EXPECT_NE(cache.GetPostProcessLayout(), nullptr);

    ASSERT_EQ(uiLayout.entries.size(), static_cast<size_t>(1));
    const auto* uiTexture = FindBinding(uiLayout, 0);
    ASSERT_NE(uiTexture, nullptr);
    EXPECT_EQ(uiTexture->type, RVX::RHIBindingType::CombinedTextureSampler);
    EXPECT_TRUE(RVX::HasFlag(uiTexture->visibility, RVX::RHIShaderStage::Pixel));
    EXPECT_NE(cache.GetUITextureSetLayout(), nullptr);
    EXPECT_NE(cache.GetUILayout(), nullptr);

    ASSERT_EQ(skyboxLayout.entries.size(), static_cast<size_t>(3));
    const auto* skyboxConstants = FindBinding(skyboxLayout, 0);
    ASSERT_NE(skyboxConstants, nullptr);
    EXPECT_EQ(skyboxConstants->type, RVX::RHIBindingType::UniformBuffer);
    EXPECT_TRUE(RVX::HasFlag(skyboxConstants->visibility, RVX::RHIShaderStage::Vertex));
    EXPECT_TRUE(RVX::HasFlag(skyboxConstants->visibility, RVX::RHIShaderStage::Pixel));
    const auto* skyboxTexture = FindBinding(skyboxLayout, 1);
    ASSERT_NE(skyboxTexture, nullptr);
    EXPECT_EQ(skyboxTexture->type, RVX::RHIBindingType::SampledTexture);
    EXPECT_TRUE(RVX::HasFlag(skyboxTexture->visibility, RVX::RHIShaderStage::Pixel));
    const auto* skyboxSampler = FindBinding(skyboxLayout, 2);
    ASSERT_NE(skyboxSampler, nullptr);
    EXPECT_EQ(skyboxSampler->type, RVX::RHIBindingType::Sampler);
    EXPECT_TRUE(RVX::HasFlag(skyboxSampler->visibility, RVX::RHIShaderStage::Pixel));
    EXPECT_NE(cache.GetSkyboxSetLayout(), nullptr);
    EXPECT_NE(cache.GetSkyboxLayout(), nullptr);
}

TEST_F(PipelineCacheValidationFixture, OpenGLDescriptorBindingsUseStableGLSLBindingABIAndSingleSamplerPropagation)
{
    if (!HasCompilerAvailable())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    const fs::path repoRoot = FindShaderDirectory().parent_path().parent_path();
    const std::string translatorSource =
        ReadTextFile(repoRoot / "ShaderCompiler" / "Private" / "SPIRVCrossTranslator.cpp");
    const std::string descriptorSource =
        ReadTextFile(repoRoot / "RHI_OpenGL" / "Private" / "OpenGLDescriptor.cpp");

    EXPECT_NE(translatorSource.find("SortResourcesBySetBinding(glslCompiler, resources.uniform_buffers);"),
              std::string::npos);
    EXPECT_NE(translatorSource.find("SortResourcesBySetBinding(glslCompiler, resources.separate_images);"),
              std::string::npos);
    EXPECT_NE(translatorSource.find("SortResourcesBySetBinding(glslCompiler, resources.separate_samplers);"),
              std::string::npos);

    EXPECT_NE(translatorSource.find("GLSLBindingABI::FlattenBinding(RHIBindingType::UniformBuffer, set, binding)"),
              std::string::npos);
    EXPECT_NE(translatorSource.find("GLSLBindingABI::FlattenBinding(RHIBindingType::SampledTexture, set, binding)"),
              std::string::npos);
    EXPECT_NE(translatorSource.find("GLSLBindingABI::RVX_GLSL_PUSH_CONSTANT_UBO_BINDING"),
              std::string::npos);

    EXPECT_NE(descriptorSource.find("glEntry.rhiBinding = binding.binding + binding.arrayElement"),
              std::string::npos);
    EXPECT_NE(descriptorSource.find("GLSLBindingABI::FlattenBinding(entry.type, setIndex, entry.rhiBinding)"),
              std::string::npos);
    EXPECT_NE(descriptorSource.find("sampledTextureSlots.push_back(globalBinding);"),
              std::string::npos);
    EXPECT_NE(descriptorSource.find("separateSamplerBindings.size() == 1"), std::string::npos);
    EXPECT_NE(descriptorSource.find("for (uint32 slot : sampledTextureSlots)"), std::string::npos);
}

TEST_F(PipelineCacheValidationFixture, OpenGLPBRMaterialSmokeHasVisualGoldenCoverage)
{
    if (!HasCompilerAvailable())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    const fs::path testsCMakePath = FindTestsCMakePath();
    ASSERT_FALSE(testsCMakePath.empty());
    const std::string testsCMake = ReadTextFile(testsCMakePath);
    EXPECT_NE(testsCMake.find("if(RVX_ENABLE_OPENGL AND TARGET ModelViewer)"), std::string::npos);
    EXPECT_NE(testsCMake.find("ModelViewerOpenGLPBRMaterialSmoke"), std::string::npos);
    EXPECT_NE(testsCMake.find("OpenGLPBRMaterialVisualGoldenValidation"), std::string::npos);
    EXPECT_NE(testsCMake.find("RQ4_PBRMaterial_OpenGL_320x180.ppm"), std::string::npos);
    EXPECT_NE(testsCMake.find("--model \"${CMAKE_CURRENT_SOURCE_DIR}/Fixtures/ModelViewer/PBRMaterialSwatch.gltf\""),
              std::string::npos);
    EXPECT_NE(testsCMake.find("--material-test-scene"), std::string::npos);
    EXPECT_NE(testsCMake.find("--expect-material-ready"), std::string::npos);
    EXPECT_NE(testsCMake.find("--backend opengl"), std::string::npos);
    EXPECT_NE(testsCMake.find("set_property(TEST OpenGLPBRMaterialVisualGoldenValidation PROPERTY DEPENDS ModelViewerOpenGLPBRMaterialSmoke)"),
              std::string::npos);
    EXPECT_NE(testsCMake.find("ModelViewerCookedBCFixtureWriter"), std::string::npos);
    EXPECT_NE(testsCMake.find("ModelViewerCookedBCMaterialFixture"), std::string::npos);
    EXPECT_NE(testsCMake.find("ModelViewerOpenGLCookedBCMaterialSmoke"), std::string::npos);
    EXPECT_NE(testsCMake.find("OpenGLCookedBCMaterialImageContentValidation"), std::string::npos);
    EXPECT_NE(testsCMake.find("--rvxcook \"$<TARGET_FILE:RVXCook>\""), std::string::npos);
    EXPECT_NE(testsCMake.find("SP14_CookedBCMaterial_OpenGL_320x180.ppm"), std::string::npos);
    EXPECT_NE(testsCMake.find("set_property(TEST ModelViewerOpenGLCookedBCMaterialSmoke PROPERTY DEPENDS ModelViewerCookedBCMaterialFixture)"),
              std::string::npos);
    EXPECT_NE(testsCMake.find("set_property(TEST OpenGLCookedBCMaterialImageContentValidation PROPERTY DEPENDS ModelViewerOpenGLCookedBCMaterialSmoke)"),
              std::string::npos);

    const fs::path fixtureWriterPath =
        testsCMakePath.parent_path() / "ModelViewerCookedBCFixtureWriter" / "main.cpp";
    ASSERT_TRUE(fs::exists(fixtureWriterPath));
    const std::string fixtureWriterSource = ReadTextFile(fixtureWriterPath);
    EXPECT_NE(fixtureWriterSource.find("SP14BaseColorBC7"), std::string::npos);
    EXPECT_NE(fixtureWriterSource.find("texture.compression[BaseColorBC7.tga]=bc7"), std::string::npos);
    EXPECT_NE(fixtureWriterSource.find("SP14MetallicRoughnessBC3"), std::string::npos);
    EXPECT_NE(fixtureWriterSource.find("texture.compression[MetallicRoughness.tga]=bc3"), std::string::npos);
    EXPECT_NE(fixtureWriterSource.find("--rewrite-gltf-texture-uris"), std::string::npos);
    EXPECT_NE(fixtureWriterSource.find("VerifyRewrittenGltf"), std::string::npos);

    const fs::path modelViewerPath = FindModelViewerSourcePath();
    ASSERT_FALSE(modelViewerPath.empty());
    const std::string modelViewerSource = ReadTextFile(modelViewerPath);
    EXPECT_NE(modelViewerSource.find("IsSmokeScreenshotBackendSupported"), std::string::npos);
    EXPECT_NE(modelViewerSource.find("RHIBackendType::OpenGL"), std::string::npos);
    EXPECT_NE(modelViewerSource.find("windowConfig.graphicsApi = options.backend == RHIBackendType::OpenGL"),
              std::string::npos);
    EXPECT_NE(modelViewerSource.find("outScreenshot.originBottomLeft = backendType == RHIBackendType::OpenGL"),
              std::string::npos);

    const fs::path repoRoot = FindShaderDirectory().parent_path().parent_path();
    const std::string renderSubsystem =
        ReadTextFile(repoRoot / "Render" / "Private" / "RenderSubsystem.cpp");
    EXPECT_NE(renderSubsystem.find("backendType == RHIBackendType::OpenGL"), std::string::npos);
    EXPECT_NE(renderSubsystem.find("windowSubsystem->GetInternalHandle()"), std::string::npos);

    const std::string openGLCommandContext =
        ReadTextFile(repoRoot / "RHI_OpenGL" / "Private" / "OpenGLCommandContext.cpp");
    const std::string openGLResources =
        ReadTextFile(repoRoot / "RHI_OpenGL" / "Private" / "OpenGLResources.cpp");
    const std::string openGLResourcesHeader =
        ReadTextFile(repoRoot / "RHI_OpenGL" / "Private" / "OpenGLResources.h");
    const std::string openGLUpload =
        ReadTextFile(repoRoot / "RHI_OpenGL" / "Private" / "OpenGLUpload.cpp");
    const std::string particleRenderer =
        ReadTextFile(repoRoot / "Particle" / "Private" / "Rendering" / "ParticleRenderer.cpp");
    EXPECT_NE(openGLCommandContext.find("srcGL->GetHandle() == 0"), std::string::npos);
    EXPECT_NE(openGLCommandContext.find("glReadPixels"), std::string::npos);
    EXPECT_NE(openGLCommandContext.find("GL_PIXEL_PACK_BUFFER"), std::string::npos);
    EXPECT_NE(openGLCommandContext.find("std::array<uint32, VAOCacheKey::MAX_VERTEX_BUFFERS> currentOffsets"),
              std::string::npos);
    EXPECT_NE(openGLCommandContext.find("!m_vertexBuffers[elem.inputSlot].buffer"), std::string::npos);
    EXPECT_NE(openGLResources.find("m_desc.memoryType == RHIMemoryType::Upload"), std::string::npos);
    EXPECT_NE(openGLResourcesHeader.find("OpenGLBuffer(OpenGLDevice* device, const RHIBufferDesc& desc, GLuint existingBuffer, GLenum target)"),
              std::string::npos);
    EXPECT_NE(openGLUpload.find("MakeRef<OpenGLBuffer>(m_device, desc, m_pbo, GL_PIXEL_UNPACK_BUFFER)"),
              std::string::npos);
    EXPECT_NE(openGLUpload.find("m_wrapperBuffer.Reset();"), std::string::npos);
    EXPECT_EQ(openGLUpload.find("m_wrapperBuffer = m_device->CreateBuffer(desc);"), std::string::npos);
    EXPECT_NE(particleRenderer.find("m_device->GetBackendType() == RHIBackendType::OpenGL"), std::string::npos);
    EXPECT_NE(particleRenderer.find("result.glslSource"), std::string::npos);
}

TEST_F(PipelineCacheValidationFixture, RVXCookWorkflowDocumentationCoversCurrentCliContract)
{
    if (!HasCompilerAvailable())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    const fs::path repoRoot = FindShaderDirectory().parent_path().parent_path();
    const fs::path docPath = repoRoot / "Docs" / "rvxcook-workflow.md";
    ASSERT_TRUE(fs::exists(docPath));

    const std::string doc = ReadTextFile(docPath);
    const std::string cookMain = ReadTextFile(repoRoot / "Tools" / "Private" / "CookMain.cpp");
    const std::string testsCMake = ReadTextFile(repoRoot / "Tests" / "CMakeLists.txt");

    EXPECT_NE(cookMain.find("--texture-compression <auto|none|bc1|bc3|bc5|bc7>"), std::string::npos);
    EXPECT_NE(cookMain.find("mesh.generateLODs"), std::string::npos);
    EXPECT_NE(cookMain.find("mesh.lodCount"), std::string::npos);
    EXPECT_NE(cookMain.find("mesh.lodReductionFactor"), std::string::npos);
    EXPECT_NE(doc.find("--texture-compression <auto|none|bc1|bc3|bc5|bc7>"), std::string::npos);
    EXPECT_NE(doc.find("--rewrite-gltf-texture-uris"), std::string::npos);
    EXPECT_NE(doc.find("--fail-on-errors"), std::string::npos);
    EXPECT_NE(doc.find("--non-recursive"), std::string::npos);
    EXPECT_NE(doc.find("RVX_COOK_PROFILE_V1"), std::string::npos);
    EXPECT_NE(doc.find("texture.compression[pattern]"), std::string::npos);
    EXPECT_NE(doc.find("Later matching rules override earlier matching rules"), std::string::npos);
    EXPECT_NE(doc.find("mesh.generateTangents"), std::string::npos);
    EXPECT_NE(doc.find("mesh.generateLODs"), std::string::npos);
    EXPECT_NE(doc.find("mesh.lodCount"), std::string::npos);
    EXPECT_NE(doc.find("Mesh artifacts can include lower LOD payloads"), std::string::npos);
    EXPECT_NE(doc.find("runtimeGltfTextureUriRewrites=5"), std::string::npos);
    EXPECT_NE(doc.find("BC7 is explicit opt-in"), std::string::npos);
    EXPECT_NE(doc.find("binary `.glb` rewrite is not implemented"), std::string::npos);
    EXPECT_NE(doc.find("not a production-quality geometric simplifier"), std::string::npos);

    EXPECT_NE(testsCMake.find("ModelViewerCookedBCMaterialFixture"), std::string::npos);
    EXPECT_NE(doc.find("ModelViewerCookedBCMaterialFixture"), std::string::npos);
    EXPECT_NE(doc.find("ModelViewerOpenGLCookedBCMaterialSmoke"), std::string::npos);
    EXPECT_NE(doc.find("OpenGLCookedBCMaterialImageContentValidation"), std::string::npos);
    EXPECT_NE(doc.find("RVXCookCliAppliesMeshProfileLODOptions"), std::string::npos);
    EXPECT_NE(doc.find("RVXCookCliRejectsInvalidMeshCookProfile"), std::string::npos);
    EXPECT_NE(doc.find("RVXCookCliRewritesGltfTextureUrisToCookedArtifacts"), std::string::npos);
    EXPECT_NE(doc.find("ResourceManagerLoadsCookedMeshArtifactWithLOD"), std::string::npos);
    EXPECT_NE(doc.find("BC7TextureDataUploadsWithBlockLayout"), std::string::npos);
}

TEST_F(PipelineCacheValidationFixture, OpenGLRenderPassClearsIgnorePreviousWriteMasks)
{
    if (!HasCompilerAvailable())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    const fs::path repoRoot = FindShaderDirectory().parent_path().parent_path();
    const std::string openGLCommandContext =
        ReadTextFile(repoRoot / "RHI_OpenGL" / "Private" / "OpenGLCommandContext.cpp");

    const size_t colorMaskSave =
        openGLCommandContext.find("glGetBooleani_v(GL_COLOR_WRITEMASK, i, previousColorMask)");
    const size_t colorMaskEnable =
        openGLCommandContext.find("glColorMaski(i, GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE)");
    const size_t colorClear =
        openGLCommandContext.find("glClearNamedFramebufferfv(m_currentFBO, GL_COLOR, i, clearColor)");
    const size_t colorMaskRestore =
        openGLCommandContext.find("glColorMaski(i,\n                                      previousColorMask[0]");

    ASSERT_NE(colorMaskSave, std::string::npos);
    ASSERT_NE(colorMaskEnable, std::string::npos);
    ASSERT_NE(colorClear, std::string::npos);
    ASSERT_NE(colorMaskRestore, std::string::npos);
    EXPECT_LT(colorMaskSave, colorMaskEnable);
    EXPECT_LT(colorMaskEnable, colorClear);
    EXPECT_LT(colorClear, colorMaskRestore);

    const size_t depthMaskSave =
        openGLCommandContext.find("glGetBooleanv(GL_DEPTH_WRITEMASK, &previousDepthMask)");
    const size_t depthMaskEnable =
        openGLCommandContext.find("glDepthMask(GL_TRUE)");
    const size_t depthClear =
        openGLCommandContext.find("glClearNamedFramebufferfv(m_currentFBO, GL_DEPTH, 0, &ds.clearValue.depth)");
    const size_t depthMaskRestore =
        openGLCommandContext.find("glDepthMask(previousDepthMask)");

    ASSERT_NE(depthMaskSave, std::string::npos);
    ASSERT_NE(depthMaskEnable, std::string::npos);
    ASSERT_NE(depthClear, std::string::npos);
    ASSERT_NE(depthMaskRestore, std::string::npos);
    EXPECT_LT(depthMaskSave, depthMaskEnable);
    EXPECT_LT(depthMaskEnable, depthClear);
    EXPECT_LT(depthClear, depthMaskRestore);
}

TEST_F(PipelineCacheValidationFixture, ShaderCacheKeyIncludesCompilerAbiVersion)
{
    if (!HasCompilerAvailable())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    const fs::path repoRoot = FindShaderDirectory().parent_path().parent_path();
    const std::string formatHeader =
        ReadTextFile(repoRoot / "ShaderCompiler" / "Include" / "ShaderCompiler" / "ShaderCacheFormat.h");
    const std::string managerSource =
        ReadTextFile(repoRoot / "ShaderCompiler" / "Private" / "ShaderManager.cpp");
    const std::string cacheManagerSource =
        ReadTextFile(repoRoot / "ShaderCompiler" / "Private" / "ShaderCacheManager.cpp");

    EXPECT_NE(formatHeader.find("RVX_SHADER_COMPILER_CACHE_ABI_VERSION"), std::string::npos);
    EXPECT_NE(managerSource.find("hashCombine(hash, RVX_SHADER_COMPILER_CACHE_ABI_VERSION);"),
              std::string::npos);
    EXPECT_NE(cacheManagerSource.find("header.compilerVersion = RVX_SHADER_COMPILER_CACHE_ABI_VERSION;"),
              std::string::npos);
    EXPECT_NE(cacheManagerSource.find("header.compilerVersion != RVX_SHADER_COMPILER_CACHE_ABI_VERSION"),
              std::string::npos);
}

TEST_F(PipelineCacheValidationFixture, DX11DefaultLitContractBuildsLocalLightBindings)
{
    if (!HasCompilerAvailable())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    FakeDevice device(RVX::RHIBackendType::DX11);
    RVX::PipelineCache cache;

    ASSERT_TRUE(cache.Initialize(&device, FindShaderDirectory().string())) << cache.GetLastError();
    ASSERT_FALSE(device.capturedSetLayouts.empty());

    const auto& frameLayout = device.capturedSetLayouts[0];
    const auto* frameLightConstants = FindBinding(frameLayout, 3);
    ASSERT_NE(frameLightConstants, nullptr);
    EXPECT_EQ(frameLightConstants->type, RVX::RHIBindingType::UniformBuffer);
    const auto* framePointLights = FindBinding(frameLayout, 4);
    ASSERT_NE(framePointLights, nullptr);
    EXPECT_EQ(framePointLights->type, RVX::RHIBindingType::ShaderResourceBuffer);
    const auto* frameSpotLights = FindBinding(frameLayout, 5);
    ASSERT_NE(frameSpotLights, nullptr);
    EXPECT_EQ(frameSpotLights->type, RVX::RHIBindingType::ShaderResourceBuffer);
    const auto* frameRayTracedShadowMask = FindBinding(frameLayout, 6);
    ASSERT_NE(frameRayTracedShadowMask, nullptr);
    EXPECT_EQ(frameRayTracedShadowMask->type, RVX::RHIBindingType::SampledTexture);
    const auto* frameClusterConstants = FindBinding(frameLayout, 7);
    ASSERT_NE(frameClusterConstants, nullptr);
    EXPECT_EQ(frameClusterConstants->type, RVX::RHIBindingType::UniformBuffer);
    const auto* frameClusterData = FindBinding(frameLayout, 8);
    ASSERT_NE(frameClusterData, nullptr);
    EXPECT_EQ(frameClusterData->type, RVX::RHIBindingType::ShaderResourceBuffer);
    const auto* frameClusterLightIndices = FindBinding(frameLayout, 9);
    ASSERT_NE(frameClusterLightIndices, nullptr);
    EXPECT_EQ(frameClusterLightIndices->type, RVX::RHIBindingType::ShaderResourceBuffer);
}

TEST_F(PipelineCacheValidationFixture, ViewConstantsLayoutMatchesDefaultLitCBufferPacking)
{
    EXPECT_TRUE(std::is_standard_layout_v<RVX::ViewConstants>);
    EXPECT_EQ(sizeof(RVX::Mat4), 64u);
    EXPECT_EQ(sizeof(RVX::Vec3), 12u);
    EXPECT_EQ(sizeof(RVX::Vec4), 16u);
    EXPECT_EQ(offsetof(RVX::ViewConstants, viewProjection), 0u);
    EXPECT_EQ(offsetof(RVX::ViewConstants, cameraPosition), 64u);
    EXPECT_EQ(offsetof(RVX::ViewConstants, time), 76u);
    EXPECT_EQ(offsetof(RVX::ViewConstants, lightDirection), 80u);
    EXPECT_EQ(offsetof(RVX::ViewConstants, directionalLightIntensity), 92u);
    EXPECT_EQ(offsetof(RVX::ViewConstants, directionalLightColor), 96u);
    EXPECT_EQ(offsetof(RVX::ViewConstants, directionalLightColorPadding), 108u);
    EXPECT_EQ(offsetof(RVX::ViewConstants, iblDiffuseAmbient), 112u);
    EXPECT_EQ(offsetof(RVX::ViewConstants, iblSpecularAmbient), 128u);
    EXPECT_EQ(offsetof(RVX::ViewConstants, iblTextureParams), 144u);
    EXPECT_EQ(offsetof(RVX::ViewConstants, cameraForwardAndShadowCascadeCount), 160u);
    EXPECT_EQ(offsetof(RVX::ViewConstants, directionalShadowViewProjections), 176u);
    EXPECT_EQ(offsetof(RVX::ViewConstants, directionalShadowParams), 432u);
    EXPECT_EQ(offsetof(RVX::ViewConstants, directionalShadowReceiverParams), 448u);
    EXPECT_EQ(offsetof(RVX::ViewConstants, directionalShadowCascadeSplits), 464u);
    EXPECT_EQ(offsetof(RVX::ViewConstants, directionalShadowCascadeFadeDistances), 480u);
    EXPECT_EQ(offsetof(RVX::ViewConstants, rayTracedShadowParams), 496u);
    EXPECT_EQ(sizeof(RVX::ViewConstants), 512u);
}

TEST_F(PipelineCacheValidationFixture, ObjectConstantsLayoutMatchesDefaultLitCBufferPacking)
{
    EXPECT_TRUE(std::is_standard_layout_v<RVX::ObjectConstants>);
    EXPECT_EQ(offsetof(RVX::ObjectConstants, world), 0u);
    EXPECT_EQ(offsetof(RVX::ObjectConstants, normalMatrix), 64u);
    EXPECT_EQ(offsetof(RVX::ObjectConstants, previousWorldViewProjection), 128u);
    EXPECT_EQ(offsetof(RVX::ObjectConstants, objectVelocityParams), 192u);
    EXPECT_EQ(offsetof(RVX::ObjectConstants, skinningParams), 208u);
    EXPECT_EQ(offsetof(RVX::ObjectConstants, skinningMatrices), 224u);
    EXPECT_EQ(sizeof(RVX::ObjectConstants),
              224u + sizeof(RVX::Mat4) * RVX::RVX_MAX_OBJECT_SKINNING_MATRICES);
}

TEST_F(PipelineCacheValidationFixture, ObjectConstantBufferUsesAlignedObjectConstantsStride)
{
    if (!HasCompilerAvailable())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    FakeDevice device;
    RVX::PipelineCache cache;
    ASSERT_TRUE(cache.Initialize(&device, FindShaderDirectory().string())) << cache.GetLastError();

    constexpr RVX::uint64 kExpectedConstantBufferAlignment = 256;
    constexpr RVX::uint64 kExpectedDrawConstantSlots = 8192;
    const RVX::uint64 expectedStride =
        (sizeof(RVX::ObjectConstants) + kExpectedConstantBufferAlignment - 1u) &
        ~(kExpectedConstantBufferAlignment - 1u);

    const FakeBuffer* objectBuffer = FindCapturedBuffer(device, "ObjectConstantBuffer");
    ASSERT_NE(objectBuffer, nullptr);
    EXPECT_EQ(objectBuffer->GetSize(), expectedStride * kExpectedDrawConstantSlots);

    const auto objectDescIt = std::find_if(device.capturedDescriptorSetDescs.begin(),
                                           device.capturedDescriptorSetDescs.end(),
                                           [](const RVX::RHIDescriptorSetDesc& desc)
                                           {
                                               return desc.debugName &&
                                                      std::strcmp(desc.debugName, "DefaultObjectDescriptorSet") == 0;
                                           });
    ASSERT_NE(objectDescIt, device.capturedDescriptorSetDescs.end());
    const RVX::RHIDescriptorBinding* objectBinding = FindDescriptorBinding(*objectDescIt, 0);
    ASSERT_NE(objectBinding, nullptr);
    EXPECT_EQ(objectBinding->range, expectedStride);
}

TEST_F(PipelineCacheValidationFixture, UpdateObjectConstantsUploadsWorldAndNormalMatrices)
{
    if (!HasCompilerAvailable())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    FakeDevice device;
    RVX::PipelineCache cache;
    ASSERT_TRUE(cache.Initialize(&device, FindShaderDirectory().string())) << cache.GetLastError();

    RVX::Mat4 world = RVX::Mat4Identity();
    world[3][0] = 4.0f;
    world[3][1] = 5.0f;
    world[3][2] = 6.0f;

    RVX::Mat4 normalMatrix = RVX::Mat4Identity();
    normalMatrix[0][0] = 0.5f;
    normalMatrix[1][1] = 2.0f;
    normalMatrix[2][2] = 3.0f;

    RVX::Mat4 previousWorld = RVX::Mat4Identity();
    previousWorld[3][0] = 7.0f;
    previousWorld[3][1] = 8.0f;
    previousWorld[3][2] = 9.0f;

    cache.UpdateObjectConstants(world, normalMatrix, previousWorld, RVX::Mat4Identity(), true);

    const FakeBuffer* objectBuffer = FindCapturedBuffer(device, "ObjectConstantBuffer");
    ASSERT_NE(objectBuffer, nullptr);
    ASSERT_GE(objectBuffer->GetStorage().size(), sizeof(RVX::ObjectConstants));

    RVX::ObjectConstants uploaded{};
    std::memcpy(&uploaded, objectBuffer->GetStorage().data(), sizeof(uploaded));
    EXPECT_FLOAT_EQ(uploaded.world[3][0], 4.0f);
    EXPECT_FLOAT_EQ(uploaded.world[3][1], 5.0f);
    EXPECT_FLOAT_EQ(uploaded.world[3][2], 6.0f);
    EXPECT_FLOAT_EQ(uploaded.normalMatrix[0][0], 0.5f);
    EXPECT_FLOAT_EQ(uploaded.normalMatrix[1][1], 2.0f);
    EXPECT_FLOAT_EQ(uploaded.normalMatrix[2][2], 3.0f);
    EXPECT_FLOAT_EQ(uploaded.previousWorldViewProjection[3][0], 7.0f);
    EXPECT_FLOAT_EQ(uploaded.previousWorldViewProjection[3][1], 8.0f);
    EXPECT_FLOAT_EQ(uploaded.previousWorldViewProjection[3][2], 9.0f);
    EXPECT_FLOAT_EQ(uploaded.objectVelocityParams.x, 1.0f);
    EXPECT_FLOAT_EQ(uploaded.objectVelocityParams.y, 1.0f);
    EXPECT_FLOAT_EQ(uploaded.skinningParams.x, 0.0f);
    EXPECT_FLOAT_EQ(uploaded.skinningParams.y, 0.0f);

    cache.UpdateObjectConstants(world, normalMatrix, previousWorld, RVX::Mat4Identity(), true, false);
    const RVX::uint64 objectStride =
        (sizeof(RVX::ObjectConstants) + 255u) & ~static_cast<RVX::uint64>(255u);
    std::memcpy(&uploaded, objectBuffer->GetStorage().data() + objectStride, sizeof(uploaded));
    EXPECT_FLOAT_EQ(uploaded.objectVelocityParams.x, 1.0f);
    EXPECT_FLOAT_EQ(uploaded.objectVelocityParams.y, 0.0f);
}

TEST_F(PipelineCacheValidationFixture, UpdateObjectConstantsUploadsSkinningMatrices)
{
    if (!HasCompilerAvailable())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    FakeDevice device;
    RVX::PipelineCache cache;
    ASSERT_TRUE(cache.Initialize(&device, FindShaderDirectory().string())) << cache.GetLastError();

    std::array<RVX::Mat4, 2> skinningMatrices = {
        RVX::Mat4Identity(),
        RVX::Mat4Identity()
    };
    skinningMatrices[0][3][0] = 2.0f;
    skinningMatrices[1][3][1] = 3.0f;

    cache.UpdateObjectConstants(RVX::Mat4Identity(),
                                RVX::Mat4Identity(),
                                RVX::Mat4Identity(),
                                RVX::Mat4Identity(),
                                false,
                                skinningMatrices);

    const FakeBuffer* objectBuffer = FindCapturedBuffer(device, "ObjectConstantBuffer");
    ASSERT_NE(objectBuffer, nullptr);
    ASSERT_GE(objectBuffer->GetStorage().size(), sizeof(RVX::ObjectConstants));

    RVX::ObjectConstants uploaded{};
    std::memcpy(&uploaded, objectBuffer->GetStorage().data(), sizeof(uploaded));
    EXPECT_FLOAT_EQ(uploaded.skinningParams.x, 1.0f);
    EXPECT_FLOAT_EQ(uploaded.skinningParams.y, 2.0f);
    EXPECT_FLOAT_EQ(uploaded.skinningMatrices[0][3][0], 2.0f);
    EXPECT_FLOAT_EQ(uploaded.skinningMatrices[1][3][1], 3.0f);
}

TEST_F(PipelineCacheValidationFixture, DefaultLitUsesObjectNormalMatrix)
{
    const std::string shader = ReadTextFile(FindShaderDirectory() / "DefaultLit.hlsl");
    EXPECT_NE(shader.find("float4x4 NormalMatrix;"), std::string::npos);
    EXPECT_NE(shader.find("float4x4 PreviousWorldViewProjection;"), std::string::npos);
    EXPECT_NE(shader.find("float4 ObjectVelocityParams;"), std::string::npos);
    EXPECT_NE(shader.find("float4 SkinningParams;"), std::string::npos);
    EXPECT_NE(shader.find("float4x4 SkinningMatrices[RVX_MAX_OBJECT_SKINNING_MATRICES];"), std::string::npos);
    EXPECT_NE(shader.find("uint4 BoneIndices : BLENDINDICES;"), std::string::npos);
    EXPECT_NE(shader.find("float4 BoneWeights : BLENDWEIGHT;"), std::string::npos);
    EXPECT_NE(shader.find("ResolveSkinningPosition(input.Position, input.BoneIndices, input.BoneWeights)"),
              std::string::npos);
    EXPECT_NE(shader.find("mul((float3x3)NormalMatrix, localNormal)"), std::string::npos);
    EXPECT_EQ(shader.find("mul((float3x3)World, input.Normal)"), std::string::npos);
}

TEST_F(PipelineCacheValidationFixture, DrawPassesUploadRenderObjectNormalMatrix)
{
    const fs::path passesDir = FindShaderDirectory().parent_path() / "Private" / "Passes";

    const std::string opaquePass = ReadTextFile(passesDir / "OpaquePass.cpp");
    const std::string transparentPass = ReadTextFile(passesDir / "TransparentPass.cpp");
    const std::string depthPrepass = ReadTextFile(passesDir / "DepthPrepass.cpp");
    const std::string shadowPass = ReadTextFile(passesDir / "ShadowPass.cpp");
    const std::string objectVelocityPass = ReadTextFile(passesDir / "ObjectVelocityPass.cpp");

    EXPECT_NE(opaquePass.find("obj.previousWorldMatrix"), std::string::npos);
    EXPECT_NE(transparentPass.find("obj.previousWorldMatrix"), std::string::npos);
    EXPECT_NE(depthPrepass.find("obj.previousWorldMatrix"), std::string::npos);
    EXPECT_NE(shadowPass.find("obj.previousWorldMatrix"), std::string::npos);
    EXPECT_NE(opaquePass.find("view.previousViewProjectionMatrix"), std::string::npos);
    EXPECT_NE(transparentPass.find("view.previousViewProjectionMatrix"), std::string::npos);
    EXPECT_NE(depthPrepass.find("view.previousViewProjectionMatrix"), std::string::npos);
    EXPECT_NE(shadowPass.find("view.previousViewProjectionMatrix"), std::string::npos);
    EXPECT_NE(opaquePass.find("ResolveSkinningMatrices(obj, buffers)"), std::string::npos);
    EXPECT_NE(transparentPass.find("ResolveSkinningMatrices(obj, buffers)"), std::string::npos);
    EXPECT_NE(depthPrepass.find("ResolveSkinningMatrices(obj, buffers)"), std::string::npos);
    EXPECT_NE(shadowPass.find("ResolveSkinningMatrices(obj, buffers)"), std::string::npos);
    EXPECT_NE(objectVelocityPass.find("ResolveSkinningMatrices(object, buffers)"), std::string::npos);
}

TEST_F(PipelineCacheValidationFixture, UpdateViewConstantsUploadsDefaultIBLAmbientValues)
{
    if (!HasCompilerAvailable())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    FakeDevice device;
    RVX::PipelineCache cache;
    ASSERT_TRUE(cache.Initialize(&device, FindShaderDirectory().string())) << cache.GetLastError();

    RVX::ViewData view;
    view.cameraPosition = RVX::Vec3(1.0f, 2.0f, 3.0f);
    view.time = 4.0f;
    cache.UpdateViewConstants(view);

    const FakeBuffer* viewBuffer = FindCapturedBuffer(device, "ViewConstantBuffer");
    ASSERT_NE(viewBuffer, nullptr);
    ASSERT_GE(viewBuffer->GetStorage().size(), sizeof(RVX::ViewConstants));

    RVX::ViewConstants uploaded{};
    std::memcpy(&uploaded, viewBuffer->GetStorage().data(), sizeof(uploaded));
    EXPECT_NEAR(uploaded.lightDirection.x, 0.505076f, 0.00001f);
    EXPECT_NEAR(uploaded.lightDirection.y, -0.808122f, 0.00001f);
    EXPECT_NEAR(uploaded.lightDirection.z, 0.303046f, 0.00001f);
    EXPECT_FLOAT_EQ(uploaded.directionalLightIntensity, 4.0f);
    EXPECT_FLOAT_EQ(uploaded.iblDiffuseAmbient.x, 1.0f);
    EXPECT_FLOAT_EQ(uploaded.iblDiffuseAmbient.y, 1.0f);
    EXPECT_FLOAT_EQ(uploaded.iblDiffuseAmbient.z, 1.0f);
    EXPECT_FLOAT_EQ(uploaded.iblDiffuseAmbient.w, 0.12f);
    EXPECT_FLOAT_EQ(uploaded.iblSpecularAmbient.x, 1.0f);
    EXPECT_FLOAT_EQ(uploaded.iblSpecularAmbient.y, 1.0f);
    EXPECT_FLOAT_EQ(uploaded.iblSpecularAmbient.z, 1.0f);
    EXPECT_FLOAT_EQ(uploaded.iblSpecularAmbient.w, 0.04f);
    EXPECT_FLOAT_EQ(uploaded.iblTextureParams.x, 0.0f);
    EXPECT_FLOAT_EQ(uploaded.iblTextureParams.y, 1.0f);
    EXPECT_FLOAT_EQ(uploaded.iblTextureParams.z, 1.0f);
    EXPECT_FLOAT_EQ(uploaded.iblTextureParams.w, 0.08f);
    EXPECT_FLOAT_EQ(uploaded.cameraForwardAndShadowCascadeCount.x, 0.0f);
    EXPECT_FLOAT_EQ(uploaded.cameraForwardAndShadowCascadeCount.y, 0.0f);
    EXPECT_FLOAT_EQ(uploaded.cameraForwardAndShadowCascadeCount.z, -1.0f);
    EXPECT_FLOAT_EQ(uploaded.cameraForwardAndShadowCascadeCount.w, 0.0f);
    EXPECT_FLOAT_EQ(uploaded.directionalShadowViewProjections[0][0][0], 1.0f);
    EXPECT_FLOAT_EQ(uploaded.directionalShadowViewProjections[0][1][1], 1.0f);
    EXPECT_FLOAT_EQ(uploaded.directionalShadowViewProjections[0][2][2], 1.0f);
    EXPECT_FLOAT_EQ(uploaded.directionalShadowViewProjections[0][3][3], 1.0f);
    EXPECT_FLOAT_EQ(uploaded.directionalShadowParams.x, 0.0f);
    EXPECT_FLOAT_EQ(uploaded.directionalShadowParams.y, 0.005f);
    EXPECT_FLOAT_EQ(uploaded.directionalShadowParams.z, 1.0f);
    EXPECT_FLOAT_EQ(uploaded.directionalShadowParams.w, 0.0f);
    EXPECT_FLOAT_EQ(uploaded.rayTracedShadowParams.x, 0.0f);
    EXPECT_FLOAT_EQ(uploaded.rayTracedShadowParams.y, 1.0f);
    EXPECT_FLOAT_EQ(uploaded.rayTracedShadowParams.z, 0.0f);
}

TEST_F(PipelineCacheValidationFixture, UpdateViewConstantsUploadsCustomAndDisabledIBLAmbientValues)
{
    if (!HasCompilerAvailable())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    FakeDevice device;
    RVX::PipelineCache cache;
    ASSERT_TRUE(cache.Initialize(&device, FindShaderDirectory().string())) << cache.GetLastError();

    RVX::ViewData view;
    view.directionalLightDirection = RVX::Vec3(0.0f, -2.0f, 0.0f);
    view.directionalLightIntensity = 2.5f;
    view.directionalLightColor = RVX::Vec3(1.0f, 0.72f, 0.45f);
    view.iblDiffuseColor = RVX::Vec3(0.25f, 0.5f, 0.75f);
    view.iblDiffuseIntensity = 0.8f;
    view.iblSpecularColor = RVX::Vec3(0.1f, 0.2f, 0.3f);
    view.iblSpecularIntensity = 0.6f;
    view.iblAmbientEnabled = 2;
    view.textureIBLEnabled = 1;
    view.textureIBLPrefilteredMipLevels = 5;
    view.textureIBLIntensity = 1.7f;
    view.ambientFloorIntensity = 0.03f;
    cache.UpdateViewConstants(view);

    const FakeBuffer* viewBuffer = FindCapturedBuffer(device, "ViewConstantBuffer");
    ASSERT_NE(viewBuffer, nullptr);
    ASSERT_GE(viewBuffer->GetStorage().size(), sizeof(RVX::ViewConstants));

    RVX::ViewConstants uploaded{};
    std::memcpy(&uploaded, viewBuffer->GetStorage().data(), sizeof(uploaded));
    EXPECT_FLOAT_EQ(uploaded.lightDirection.x, 0.0f);
    EXPECT_FLOAT_EQ(uploaded.lightDirection.y, -1.0f);
    EXPECT_FLOAT_EQ(uploaded.lightDirection.z, 0.0f);
    EXPECT_FLOAT_EQ(uploaded.directionalLightIntensity, 2.5f);
    EXPECT_FLOAT_EQ(uploaded.directionalLightColor.x, 1.0f);
    EXPECT_FLOAT_EQ(uploaded.directionalLightColor.y, 0.72f);
    EXPECT_FLOAT_EQ(uploaded.directionalLightColor.z, 0.45f);
    EXPECT_FLOAT_EQ(uploaded.directionalLightColorPadding, 0.0f);
    EXPECT_FLOAT_EQ(uploaded.iblDiffuseAmbient.x, 0.25f);
    EXPECT_FLOAT_EQ(uploaded.iblDiffuseAmbient.y, 0.5f);
    EXPECT_FLOAT_EQ(uploaded.iblDiffuseAmbient.z, 0.75f);
    EXPECT_FLOAT_EQ(uploaded.iblDiffuseAmbient.w, 0.8f);
    EXPECT_FLOAT_EQ(uploaded.iblSpecularAmbient.x, 0.1f);
    EXPECT_FLOAT_EQ(uploaded.iblSpecularAmbient.y, 0.2f);
    EXPECT_FLOAT_EQ(uploaded.iblSpecularAmbient.z, 0.3f);
    EXPECT_FLOAT_EQ(uploaded.iblSpecularAmbient.w, 0.6f);
    EXPECT_FLOAT_EQ(uploaded.iblTextureParams.x, 1.0f);
    EXPECT_FLOAT_EQ(uploaded.iblTextureParams.y, 5.0f);
    EXPECT_FLOAT_EQ(uploaded.iblTextureParams.z, 1.7f);
    EXPECT_FLOAT_EQ(uploaded.iblTextureParams.w, 0.03f);

    view.iblAmbientEnabled = 0;
    view.textureIBLEnabled = 0;
    cache.UpdateViewConstants(view);
    std::memcpy(&uploaded, viewBuffer->GetStorage().data(), sizeof(uploaded));
    EXPECT_FLOAT_EQ(uploaded.iblDiffuseAmbient.x, 0.25f);
    EXPECT_FLOAT_EQ(uploaded.iblDiffuseAmbient.y, 0.5f);
    EXPECT_FLOAT_EQ(uploaded.iblDiffuseAmbient.z, 0.75f);
    EXPECT_FLOAT_EQ(uploaded.iblDiffuseAmbient.w, 0.0f);
    EXPECT_FLOAT_EQ(uploaded.iblSpecularAmbient.x, 0.1f);
    EXPECT_FLOAT_EQ(uploaded.iblSpecularAmbient.y, 0.2f);
    EXPECT_FLOAT_EQ(uploaded.iblSpecularAmbient.z, 0.3f);
    EXPECT_FLOAT_EQ(uploaded.iblSpecularAmbient.w, 0.0f);
    EXPECT_FLOAT_EQ(uploaded.iblTextureParams.x, 0.0f);
    EXPECT_FLOAT_EQ(uploaded.iblTextureParams.y, 5.0f);
    EXPECT_FLOAT_EQ(uploaded.iblTextureParams.z, 1.7f);
    EXPECT_FLOAT_EQ(uploaded.iblTextureParams.w, 0.03f);
}

TEST_F(PipelineCacheValidationFixture, UpdateViewConstantsUploadsZeroAmbientFloorForTextureIBLReadyViews)
{
    if (!HasCompilerAvailable())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    FakeDevice device;
    RVX::PipelineCache cache;
    ASSERT_TRUE(cache.Initialize(&device, FindShaderDirectory().string())) << cache.GetLastError();

    RVX::ViewData view;
    view.textureIBLEnabled = 1;
    view.textureIBLPrefilteredMipLevels = 4;
    view.textureIBLIntensity = 1.0f;
    view.ambientFloorIntensity = 0.0f;
    cache.UpdateViewConstants(view);

    const FakeBuffer* viewBuffer = FindCapturedBuffer(device, "ViewConstantBuffer");
    ASSERT_NE(viewBuffer, nullptr);

    RVX::ViewConstants uploaded{};
    std::memcpy(&uploaded, viewBuffer->GetStorage().data(), sizeof(uploaded));
    EXPECT_FLOAT_EQ(uploaded.iblTextureParams.x, 1.0f);
    EXPECT_FLOAT_EQ(uploaded.iblTextureParams.w, 0.0f);
}

TEST_F(PipelineCacheValidationFixture, UpdateViewConstantsSanitizesTextureIBLIntensity)
{
    if (!HasCompilerAvailable())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    FakeDevice device;
    RVX::PipelineCache cache;
    ASSERT_TRUE(cache.Initialize(&device, FindShaderDirectory().string())) << cache.GetLastError();

    RVX::ViewData view;
    view.textureIBLEnabled = 1;
    view.textureIBLIntensity = std::numeric_limits<float>::quiet_NaN();
    cache.UpdateViewConstants(view);

    const FakeBuffer* viewBuffer = FindCapturedBuffer(device, "ViewConstantBuffer");
    ASSERT_NE(viewBuffer, nullptr);

    RVX::ViewConstants uploaded{};
    std::memcpy(&uploaded, viewBuffer->GetStorage().data(), sizeof(uploaded));
    EXPECT_FLOAT_EQ(uploaded.iblTextureParams.z, 1.0f);

    view.textureIBLIntensity = -4.0f;
    cache.UpdateViewConstants(view);
    std::memcpy(&uploaded, viewBuffer->GetStorage().data(), sizeof(uploaded));
    EXPECT_FLOAT_EQ(uploaded.iblTextureParams.z, 0.0f);
}

TEST_F(PipelineCacheValidationFixture, UpdateViewConstantsUploadsDirectionalShadowParamsAndBackendConvention)
{
    if (!HasCompilerAvailable())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    RVX::ViewData view;
    view.directionalShadowEnabled = 1;
    view.cameraForward = RVX::Vec3(0.0f, 0.0f, -4.0f);
    view.directionalShadowCascadeCount = 3;
    view.directionalShadowCascadeSplits = RVX::Vec4(8.0f, 42.0f, 100.0f, 0.0f);
    view.directionalShadowCascadeFadeDistances = RVX::Vec4(0.5f, 2.5f, 0.0f, 0.0f);
    view.directionalShadowViewProjections[0] = RVX::Mat4Identity();
    view.directionalShadowViewProjections[0][1][1] = 2.0f;
    view.directionalShadowViewProjections[0][1][0] = 0.25f;
    view.directionalShadowViewProjections[0][1][2] = -0.5f;
    view.directionalShadowViewProjections[0][3][2] = 0.75f;
    view.directionalShadowViewProjections[1] = RVX::Mat4Identity();
    view.directionalShadowViewProjections[1][0][0] = 3.0f;
    view.directionalShadowViewProjections[2] = RVX::Mat4Identity();
    view.directionalShadowViewProjections[2][2][2] = 4.0f;
    view.directionalShadowDepthBias = 0.0125f;
    view.directionalShadowStrength = 0.6f;
    view.directionalShadowInvMapSize = 1.0f / 512.0f;
    view.directionalShadowFilterRadiusTexels = 2.0f;
    view.directionalShadowNormalBias = 0.03125f;
    view.rayTracedShadowEnabled = 1;
    view.rayTracedShadowFilterRadiusPixels = 2.5f;
    view.rayTracedShadowMode = RVX::RayTracedShadowMode::ReplaceRaster;

    FakeDevice dxDevice(RVX::RHIBackendType::DX12);
    RVX::PipelineCache dxCache;
    ASSERT_TRUE(dxCache.Initialize(&dxDevice, FindShaderDirectory().string())) << dxCache.GetLastError();
    dxCache.UpdateViewConstants(view);

    const FakeBuffer* dxViewBuffer = FindCapturedBuffer(dxDevice, "ViewConstantBuffer");
    ASSERT_NE(dxViewBuffer, nullptr);

    RVX::ViewConstants uploaded{};
    std::memcpy(&uploaded, dxViewBuffer->GetStorage().data(), sizeof(uploaded));
    EXPECT_FLOAT_EQ(uploaded.cameraForwardAndShadowCascadeCount.x, 0.0f);
    EXPECT_FLOAT_EQ(uploaded.cameraForwardAndShadowCascadeCount.y, 0.0f);
    EXPECT_FLOAT_EQ(uploaded.cameraForwardAndShadowCascadeCount.z, -1.0f);
    EXPECT_FLOAT_EQ(uploaded.cameraForwardAndShadowCascadeCount.w, 3.0f);
    EXPECT_FLOAT_EQ(uploaded.directionalShadowViewProjections[0][1][0], 0.25f);
    EXPECT_FLOAT_EQ(uploaded.directionalShadowViewProjections[0][1][1], 2.0f);
    EXPECT_FLOAT_EQ(uploaded.directionalShadowViewProjections[0][1][2], -0.5f);
    EXPECT_FLOAT_EQ(uploaded.directionalShadowViewProjections[0][3][2], 0.75f);
    EXPECT_FLOAT_EQ(uploaded.directionalShadowViewProjections[1][0][0], 3.0f);
    EXPECT_FLOAT_EQ(uploaded.directionalShadowViewProjections[2][2][2], 4.0f);
    EXPECT_FLOAT_EQ(uploaded.directionalShadowParams.x, 1.0f);
    EXPECT_FLOAT_EQ(uploaded.directionalShadowParams.y, 0.0125f);
    EXPECT_FLOAT_EQ(uploaded.directionalShadowParams.z, 0.6f);
    EXPECT_FLOAT_EQ(uploaded.directionalShadowParams.w, 2.0f / 512.0f);
    EXPECT_FLOAT_EQ(uploaded.directionalShadowReceiverParams.x, 0.03125f);
    EXPECT_FLOAT_EQ(uploaded.directionalShadowReceiverParams.y, 0.0f);
    EXPECT_FLOAT_EQ(uploaded.directionalShadowCascadeSplits.x, 8.0f);
    EXPECT_FLOAT_EQ(uploaded.directionalShadowCascadeSplits.y, 42.0f);
    EXPECT_FLOAT_EQ(uploaded.directionalShadowCascadeSplits.z, 100.0f);
    EXPECT_FLOAT_EQ(uploaded.directionalShadowCascadeFadeDistances.x, 0.5f);
    EXPECT_FLOAT_EQ(uploaded.directionalShadowCascadeFadeDistances.y, 2.5f);
    EXPECT_FLOAT_EQ(uploaded.directionalShadowCascadeFadeDistances.z, 0.0f);
    EXPECT_FLOAT_EQ(uploaded.rayTracedShadowParams.x, 1.0f);
    EXPECT_FLOAT_EQ(uploaded.rayTracedShadowParams.y, 2.5f);
    EXPECT_FLOAT_EQ(uploaded.rayTracedShadowParams.z, 1.0f);

    FakeDevice vkDevice(RVX::RHIBackendType::Vulkan);
    RVX::PipelineCache vkCache;
    ASSERT_TRUE(vkCache.Initialize(&vkDevice, FindShaderDirectory().string())) << vkCache.GetLastError();
    vkCache.UpdateViewConstants(view);

    const FakeBuffer* vkViewBuffer = FindCapturedBuffer(vkDevice, "ViewConstantBuffer");
    ASSERT_NE(vkViewBuffer, nullptr);
    std::memcpy(&uploaded, vkViewBuffer->GetStorage().data(), sizeof(uploaded));
    EXPECT_FLOAT_EQ(uploaded.directionalShadowViewProjections[0][1][0], -0.25f);
    EXPECT_FLOAT_EQ(uploaded.directionalShadowViewProjections[0][1][1], -2.0f);
    EXPECT_FLOAT_EQ(uploaded.directionalShadowViewProjections[0][1][2], 0.5f);
    EXPECT_FLOAT_EQ(uploaded.directionalShadowParams.x, 1.0f);
    EXPECT_FLOAT_EQ(uploaded.directionalShadowParams.w, 2.0f / 512.0f);
    EXPECT_FLOAT_EQ(uploaded.directionalShadowReceiverParams.x, 0.03125f);
    EXPECT_FLOAT_EQ(uploaded.cameraForwardAndShadowCascadeCount.w, 3.0f);
    EXPECT_FLOAT_EQ(uploaded.rayTracedShadowParams.z, 1.0f);

    FakeDevice reverseZDevice(RVX::RHIBackendType::DX12);
    RVX::PipelineCache reverseZCache;
    RVX::PipelineCacheConfig config;
    config.reverseZ = true;
    reverseZCache.SetConfig(config);
    ASSERT_TRUE(reverseZCache.Initialize(&reverseZDevice, FindShaderDirectory().string()))
        << reverseZCache.GetLastError();
    reverseZCache.UpdateViewConstants(view);

    const FakeBuffer* reverseZViewBuffer = FindCapturedBuffer(reverseZDevice, "ViewConstantBuffer");
    ASSERT_NE(reverseZViewBuffer, nullptr);
    std::memcpy(&uploaded, reverseZViewBuffer->GetStorage().data(), sizeof(uploaded));
    EXPECT_FLOAT_EQ(uploaded.directionalShadowParams.x, 0.0f);
}

TEST_F(PipelineCacheValidationFixture, DirectionalShadowFrameResourcesReportFallbackReasons)
{
    if (!HasCompilerAvailable())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    FakeDevice device;
    RVX::PipelineCache cache;
    ASSERT_TRUE(cache.Initialize(&device, FindShaderDirectory().string())) << cache.GetLastError();

    RVX::DirectionalShadowFrameBindingResult result =
        cache.UpdateDirectionalShadowFrameResources({});
    EXPECT_FALSE(result.shadowSamplingEnabled);
    EXPECT_EQ(result.fallbackReason, RVX::DirectionalShadowFallbackReason::DisabledNoDirectionalLight);

    RVX::DirectionalShadowFrameResources resources;
    resources.enabled = true;
    result = cache.UpdateDirectionalShadowFrameResources(resources);
    EXPECT_FALSE(result.shadowSamplingEnabled);
    EXPECT_EQ(result.fallbackReason, RVX::DirectionalShadowFallbackReason::MissingShadowSRV);

    RVX::RHITextureDesc textureDesc = RVX::RHITextureDesc::DepthStencil(32, 32, RVX::RHIFormat::D32_FLOAT);
    textureDesc.arraySize = RVX::RVX_MAX_DIRECTIONAL_SHADOW_CASCADES;
    RVX::RHITextureRef texture = device.CreateTexture(textureDesc);
    ASSERT_NE(texture, nullptr);
    RVX::RHITextureViewDesc viewDesc;
    viewDesc.format = texture->GetFormat();
    viewDesc.dimension = texture->GetDimension();
    viewDesc.subresourceRange = RVX::RHISubresourceRange::All();
    viewDesc.subresourceRange.aspect = RVX::RHITextureAspect::Depth;
    viewDesc.type = RVX::RHITextureViewType::ShaderResource;
    RVX::RHITextureViewRef shadowView = device.CreateTextureView(texture.Get(), viewDesc);
    ASSERT_NE(shadowView, nullptr);

    resources.shadowMapView = shadowView.Get();
    result = cache.UpdateDirectionalShadowFrameResources(resources);
    EXPECT_TRUE(result.shadowSamplingEnabled);
    EXPECT_EQ(result.fallbackReason, RVX::DirectionalShadowFallbackReason::None);

    RVX::PipelineCache reverseZCache;
    RVX::PipelineCacheConfig config;
    config.reverseZ = true;
    reverseZCache.SetConfig(config);
    ASSERT_TRUE(reverseZCache.Initialize(&device, FindShaderDirectory().string()))
        << reverseZCache.GetLastError();
    result = reverseZCache.UpdateDirectionalShadowFrameResources(resources);
    EXPECT_FALSE(result.shadowSamplingEnabled);
    EXPECT_EQ(result.fallbackReason, RVX::DirectionalShadowFallbackReason::ReverseZUnsupported);
}

TEST_F(PipelineCacheValidationFixture, FrameLightResourcesUseFallbacksAndSurviveShadowUpdates)
{
    if (!HasCompilerAvailable())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    FakeDevice device;
    RVX::PipelineCache cache;
    ASSERT_TRUE(cache.Initialize(&device, FindShaderDirectory().string())) << cache.GetLastError();

    ASSERT_FALSE(device.capturedDescriptorSets.empty());
    FakeDescriptorSet* frameSet = device.capturedDescriptorSets.front();
    ASSERT_NE(frameSet, nullptr);

    RVX::FrameLightBindingResult fallbackResult = cache.UpdateFrameLightResources({});
    EXPECT_TRUE(fallbackResult.lightResourcesBound);
    EXPECT_EQ(fallbackResult.fallbackReason, RVX::FrameLightFallbackReason::MissingLightConstants);

    const auto& fallbackBindings = frameSet->GetBindings();
    EXPECT_NE(FindDescriptorBinding({nullptr, fallbackBindings, nullptr}, 0), nullptr);
    EXPECT_NE(FindDescriptorBinding({nullptr, fallbackBindings, nullptr}, 1), nullptr);
    EXPECT_NE(FindDescriptorBinding({nullptr, fallbackBindings, nullptr}, 2), nullptr);
    EXPECT_NE(FindDescriptorBinding({nullptr, fallbackBindings, nullptr}, 3), nullptr);
    EXPECT_NE(FindDescriptorBinding({nullptr, fallbackBindings, nullptr}, 4), nullptr);
    EXPECT_NE(FindDescriptorBinding({nullptr, fallbackBindings, nullptr}, 5), nullptr);
    EXPECT_NE(FindDescriptorBinding({nullptr, fallbackBindings, nullptr}, 7), nullptr);
    EXPECT_NE(FindDescriptorBinding({nullptr, fallbackBindings, nullptr}, 8), nullptr);
    EXPECT_NE(FindDescriptorBinding({nullptr, fallbackBindings, nullptr}, 9), nullptr);

    RVX::RHIBufferDesc lightConstantsDesc;
    lightConstantsDesc.size = 256;
    lightConstantsDesc.usage = RVX::RHIBufferUsage::Constant;
    lightConstantsDesc.memoryType = RVX::RHIMemoryType::Upload;
    lightConstantsDesc.debugName = "TestLightConstantsBuffer";
    RVX::RHIBufferRef lightConstants = device.CreateBuffer(lightConstantsDesc);

    RVX::RHIBufferDesc pointLightsDesc;
    pointLightsDesc.size = sizeof(RVX::GPUPointLight);
    pointLightsDesc.usage = RVX::RHIBufferUsage::Structured | RVX::RHIBufferUsage::ShaderResource;
    pointLightsDesc.memoryType = RVX::RHIMemoryType::Upload;
    pointLightsDesc.stride = sizeof(RVX::GPUPointLight);
    pointLightsDesc.debugName = "TestPointLightsBuffer";
    RVX::RHIBufferRef pointLights = device.CreateBuffer(pointLightsDesc);

    RVX::RHIBufferDesc spotLightsDesc;
    spotLightsDesc.size = sizeof(RVX::GPUSpotLight);
    spotLightsDesc.usage = RVX::RHIBufferUsage::Structured | RVX::RHIBufferUsage::ShaderResource;
    spotLightsDesc.memoryType = RVX::RHIMemoryType::Upload;
    spotLightsDesc.stride = sizeof(RVX::GPUSpotLight);
    spotLightsDesc.debugName = "TestSpotLightsBuffer";
    RVX::RHIBufferRef spotLights = device.CreateBuffer(spotLightsDesc);

    RVX::RHIBufferDesc clusterConstantsDesc;
    clusterConstantsDesc.size = 256;
    clusterConstantsDesc.usage = RVX::RHIBufferUsage::Constant;
    clusterConstantsDesc.memoryType = RVX::RHIMemoryType::Upload;
    clusterConstantsDesc.debugName = "TestClusterConstantsBuffer";
    RVX::RHIBufferRef clusterConstants = device.CreateBuffer(clusterConstantsDesc);

    RVX::RHIBufferDesc clusterDataDesc;
    clusterDataDesc.size = sizeof(RVX::GPUCluster);
    clusterDataDesc.usage = RVX::RHIBufferUsage::Structured | RVX::RHIBufferUsage::ShaderResource;
    clusterDataDesc.memoryType = RVX::RHIMemoryType::Upload;
    clusterDataDesc.stride = sizeof(RVX::GPUCluster);
    clusterDataDesc.debugName = "TestClusterDataBuffer";
    RVX::RHIBufferRef clusterData = device.CreateBuffer(clusterDataDesc);

    RVX::RHIBufferDesc clusterLightIndexDesc;
    clusterLightIndexDesc.size = sizeof(RVX::LightIndex);
    clusterLightIndexDesc.usage = RVX::RHIBufferUsage::Structured | RVX::RHIBufferUsage::ShaderResource;
    clusterLightIndexDesc.memoryType = RVX::RHIMemoryType::Upload;
    clusterLightIndexDesc.stride = sizeof(RVX::LightIndex);
    clusterLightIndexDesc.debugName = "TestClusterLightIndexBuffer";
    RVX::RHIBufferRef clusterLightIndices = device.CreateBuffer(clusterLightIndexDesc);
    ASSERT_TRUE(lightConstants);
    ASSERT_TRUE(pointLights);
    ASSERT_TRUE(spotLights);
    ASSERT_TRUE(clusterConstants);
    ASSERT_TRUE(clusterData);
    ASSERT_TRUE(clusterLightIndices);

    RVX::FrameLightResources resources;
    resources.lightConstantsBuffer = lightConstants.Get();
    resources.pointLightsBuffer = pointLights.Get();
    resources.spotLightsBuffer = spotLights.Get();
    resources.clusterConstantsBuffer = clusterConstants.Get();
    resources.clusterBuffer = clusterData.Get();
    resources.clusterLightIndexBuffer = clusterLightIndices.Get();
    RVX::FrameLightBindingResult realResult = cache.UpdateFrameLightResources(resources);
    EXPECT_TRUE(realResult.lightResourcesBound);
    EXPECT_TRUE(realResult.clusteredLightResourcesBound);
    EXPECT_EQ(realResult.fallbackReason, RVX::FrameLightFallbackReason::None);
    EXPECT_EQ(realResult.clusteredFallbackReason, RVX::FrameClusteredLightFallbackReason::None);

    auto bindingsDesc = RVX::RHIDescriptorSetDesc{nullptr, frameSet->GetBindings(), nullptr};
    ASSERT_EQ(FindDescriptorBinding(bindingsDesc, 3)->buffer, lightConstants.Get());
    ASSERT_EQ(FindDescriptorBinding(bindingsDesc, 4)->buffer, pointLights.Get());
    ASSERT_EQ(FindDescriptorBinding(bindingsDesc, 5)->buffer, spotLights.Get());
    ASSERT_EQ(FindDescriptorBinding(bindingsDesc, 7)->buffer, clusterConstants.Get());
    ASSERT_EQ(FindDescriptorBinding(bindingsDesc, 8)->buffer, clusterData.Get());
    ASSERT_EQ(FindDescriptorBinding(bindingsDesc, 9)->buffer, clusterLightIndices.Get());

    RVX::DirectionalShadowFrameBindingResult shadowFallback = cache.UpdateDirectionalShadowFrameResources({});
    EXPECT_FALSE(shadowFallback.shadowSamplingEnabled);
    EXPECT_EQ(shadowFallback.fallbackReason, RVX::DirectionalShadowFallbackReason::DisabledNoDirectionalLight);

    bindingsDesc = RVX::RHIDescriptorSetDesc{nullptr, frameSet->GetBindings(), nullptr};
    ASSERT_EQ(FindDescriptorBinding(bindingsDesc, 3)->buffer, lightConstants.Get());
    ASSERT_EQ(FindDescriptorBinding(bindingsDesc, 4)->buffer, pointLights.Get());
    ASSERT_EQ(FindDescriptorBinding(bindingsDesc, 5)->buffer, spotLights.Get());
    ASSERT_EQ(FindDescriptorBinding(bindingsDesc, 7)->buffer, clusterConstants.Get());
    ASSERT_EQ(FindDescriptorBinding(bindingsDesc, 8)->buffer, clusterData.Get());
    ASSERT_EQ(FindDescriptorBinding(bindingsDesc, 9)->buffer, clusterLightIndices.Get());
}

TEST_F(PipelineCacheValidationFixture, UpdateViewConstantsSanitizesInvalidLightingControls)
{
    if (!HasCompilerAvailable())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    FakeDevice device;
    RVX::PipelineCache cache;
    ASSERT_TRUE(cache.Initialize(&device, FindShaderDirectory().string())) << cache.GetLastError();

    RVX::ViewData view;
    view.directionalLightDirection = RVX::Vec3(0.0f, 0.0f, 0.0f);
    view.cameraForward = RVX::Vec3(0.0f, 0.0f, 0.0f);
    view.directionalLightIntensity = -3.0f;
    view.directionalLightColor = RVX::Vec3(0.25f, -1.0f, 2.0f);
    view.ambientFloorIntensity = -1.0f;
    cache.UpdateViewConstants(view);

    const FakeBuffer* viewBuffer = FindCapturedBuffer(device, "ViewConstantBuffer");
    ASSERT_NE(viewBuffer, nullptr);

    RVX::ViewConstants uploaded{};
    std::memcpy(&uploaded, viewBuffer->GetStorage().data(), sizeof(uploaded));
    EXPECT_NEAR(uploaded.lightDirection.x, 0.505076f, 0.00001f);
    EXPECT_NEAR(uploaded.lightDirection.y, -0.808122f, 0.00001f);
    EXPECT_NEAR(uploaded.lightDirection.z, 0.303046f, 0.00001f);
    EXPECT_FLOAT_EQ(uploaded.directionalLightIntensity, 0.0f);
    EXPECT_FLOAT_EQ(uploaded.directionalLightColor.x, 0.25f);
    EXPECT_FLOAT_EQ(uploaded.directionalLightColor.y, 0.0f);
    EXPECT_FLOAT_EQ(uploaded.directionalLightColor.z, 2.0f);
    EXPECT_FLOAT_EQ(uploaded.cameraForwardAndShadowCascadeCount.x, 0.0f);
    EXPECT_FLOAT_EQ(uploaded.cameraForwardAndShadowCascadeCount.y, 0.0f);
    EXPECT_FLOAT_EQ(uploaded.cameraForwardAndShadowCascadeCount.z, -1.0f);
    EXPECT_FLOAT_EQ(uploaded.iblTextureParams.w, 0.0f);

    view.directionalShadowEnabled = 1;
    view.directionalShadowDepthBias = -0.25f;
    view.directionalShadowStrength = 5.0f;
    view.directionalShadowInvMapSize = 1.0f / 256.0f;
    view.directionalShadowFilterRadiusTexels = -2.0f;
    view.directionalShadowNormalBias = -3.0f;
    view.rayTracedShadowEnabled = 1;
    view.rayTracedShadowFilterRadiusPixels = 8.0f;
    view.rayTracedShadowMode = RVX::RayTracedShadowMode::ReplaceRaster;
    view.directionalShadowCascadeFadeDistances = RVX::Vec4(-1.0f, -2.0f, -3.0f, -4.0f);
    cache.UpdateViewConstants(view);
    std::memcpy(&uploaded, viewBuffer->GetStorage().data(), sizeof(uploaded));
    EXPECT_FLOAT_EQ(uploaded.directionalShadowParams.x, 1.0f);
    EXPECT_FLOAT_EQ(uploaded.directionalShadowParams.y, 0.0f);
    EXPECT_FLOAT_EQ(uploaded.directionalShadowParams.z, 1.0f);
    EXPECT_FLOAT_EQ(uploaded.directionalShadowParams.w, 0.0f);
    EXPECT_FLOAT_EQ(uploaded.directionalShadowReceiverParams.x, 0.0f);
    EXPECT_FLOAT_EQ(uploaded.directionalShadowCascadeFadeDistances.x, 0.0f);
    EXPECT_FLOAT_EQ(uploaded.directionalShadowCascadeFadeDistances.y, 0.0f);
    EXPECT_FLOAT_EQ(uploaded.rayTracedShadowParams.x, 1.0f);
    EXPECT_FLOAT_EQ(uploaded.rayTracedShadowParams.y, 3.0f);
    EXPECT_FLOAT_EQ(uploaded.rayTracedShadowParams.z, 1.0f);

    view.directionalLightDirection = RVX::Vec3(std::numeric_limits<float>::quiet_NaN(), 0.0f, 0.0f);
    view.cameraForward = RVX::Vec3(std::numeric_limits<float>::quiet_NaN(), 0.0f, 0.0f);
    view.directionalLightIntensity = std::numeric_limits<float>::infinity();
    view.directionalLightColor = RVX::Vec3(0.5f, std::numeric_limits<float>::quiet_NaN(), 0.75f);
    view.ambientFloorIntensity = std::numeric_limits<float>::quiet_NaN();
    view.directionalShadowDepthBias = std::numeric_limits<float>::quiet_NaN();
    view.directionalShadowStrength = std::numeric_limits<float>::quiet_NaN();
    view.directionalShadowInvMapSize = std::numeric_limits<float>::quiet_NaN();
    view.directionalShadowFilterRadiusTexels = std::numeric_limits<float>::infinity();
    view.directionalShadowNormalBias = std::numeric_limits<float>::quiet_NaN();
    view.rayTracedShadowFilterRadiusPixels = std::numeric_limits<float>::quiet_NaN();
    view.rayTracedShadowMode = RVX::RayTracedShadowMode::ComplementRaster;
    view.directionalShadowCascadeFadeDistances = RVX::Vec4(
        std::numeric_limits<float>::quiet_NaN(),
        std::numeric_limits<float>::infinity(),
        -1.0f,
        3.0f);
    cache.UpdateViewConstants(view);
    std::memcpy(&uploaded, viewBuffer->GetStorage().data(), sizeof(uploaded));
    EXPECT_NEAR(uploaded.lightDirection.x, 0.505076f, 0.00001f);
    EXPECT_NEAR(uploaded.lightDirection.y, -0.808122f, 0.00001f);
    EXPECT_NEAR(uploaded.lightDirection.z, 0.303046f, 0.00001f);
    EXPECT_FLOAT_EQ(uploaded.directionalLightIntensity, 4.0f);
    EXPECT_FLOAT_EQ(uploaded.directionalLightColor.x, 1.0f);
    EXPECT_FLOAT_EQ(uploaded.directionalLightColor.y, 1.0f);
    EXPECT_FLOAT_EQ(uploaded.directionalLightColor.z, 1.0f);
    EXPECT_FLOAT_EQ(uploaded.cameraForwardAndShadowCascadeCount.x, 0.0f);
    EXPECT_FLOAT_EQ(uploaded.cameraForwardAndShadowCascadeCount.y, 0.0f);
    EXPECT_FLOAT_EQ(uploaded.cameraForwardAndShadowCascadeCount.z, -1.0f);
    EXPECT_FLOAT_EQ(uploaded.iblTextureParams.w, 0.08f);
    EXPECT_FLOAT_EQ(uploaded.directionalShadowParams.y, 0.005f);
    EXPECT_FLOAT_EQ(uploaded.directionalShadowParams.z, 1.0f);
    EXPECT_FLOAT_EQ(uploaded.directionalShadowParams.w, 0.0f);
    EXPECT_FLOAT_EQ(uploaded.directionalShadowReceiverParams.x, 0.02f);
    EXPECT_FLOAT_EQ(uploaded.directionalShadowCascadeFadeDistances.x, 0.0f);
    EXPECT_FLOAT_EQ(uploaded.directionalShadowCascadeFadeDistances.y, 0.0f);
    EXPECT_FLOAT_EQ(uploaded.directionalShadowCascadeFadeDistances.z, 0.0f);
    EXPECT_FLOAT_EQ(uploaded.directionalShadowCascadeFadeDistances.w, 3.0f);
    EXPECT_FLOAT_EQ(uploaded.rayTracedShadowParams.y, 1.0f);
    EXPECT_FLOAT_EQ(uploaded.rayTracedShadowParams.z, 0.0f);
}

TEST_F(PipelineCacheValidationFixture, DefaultLitUsesIBLAmbientViewConstants)
{
    if (!HasCompilerAvailable())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    const std::string shader = ReadTextFile(FindShaderDirectory() / "DefaultLit.hlsl");
    EXPECT_NE(shader.find("IBLDiffuseAmbient"), std::string::npos);
    EXPECT_NE(shader.find("IBLSpecularAmbient"), std::string::npos);
    EXPECT_NE(shader.find("TextureCube IrradianceTexture : register(t7, space2);"), std::string::npos);
    EXPECT_NE(shader.find("TextureCube PrefilteredEnvironmentTexture : register(t8, space2);"), std::string::npos);
    EXPECT_NE(shader.find("Texture2D BRDFLUTTexture : register(t9, space2);"), std::string::npos);
    EXPECT_NE(shader.find("Texture2DArray<float> DirectionalShadowMapTexture : register(t1, space0);"),
              std::string::npos);
    EXPECT_NE(shader.find("SamplerState DirectionalShadowSampler : register(s2, space0);"), std::string::npos);
    EXPECT_NE(shader.find("float4 CameraForwardAndShadowCascadeCount;"), std::string::npos);
    EXPECT_NE(shader.find("float4x4 DirectionalShadowViewProjections[4];"), std::string::npos);
    EXPECT_NE(shader.find("float4 DirectionalShadowCascadeSplits;"), std::string::npos);
    EXPECT_NE(shader.find("float4 DirectionalShadowCascadeFadeDistances;"), std::string::npos);
    EXPECT_NE(shader.find("int SelectDirectionalShadowCascade(float3 worldPos)"), std::string::npos);
    EXPECT_NE(shader.find("float GetDirectionalShadowViewDepth(float3 worldPos)"), std::string::npos);
    EXPECT_NE(shader.find("float4 DirectionalShadowReceiverParams;"), std::string::npos);
    EXPECT_NE(shader.find("float SampleDirectionalShadowCascade(float3 worldPos, float3 worldNormal, int cascadeIndex)"),
              std::string::npos);
    EXPECT_NE(shader.find("float SampleDirectionalShadow(float3 worldPos, float3 worldNormal)"), std::string::npos);
    EXPECT_NE(shader.find("float CompareDirectionalShadowDepth(float2 uv, float compareDepth, int cascadeIndex)"),
              std::string::npos);
    EXPECT_NE(shader.find("float SampleDirectionalShadowPCF(float2 shadowUV, float compareDepth, float filterStep, int cascadeIndex)"),
              std::string::npos);
    EXPECT_NE(shader.find("float4 LightDirection_Intensity;"), std::string::npos);
    EXPECT_NE(shader.find("#define DirectionalLightIntensity LightDirection_Intensity.w"), std::string::npos);
    EXPECT_NE(shader.find("if (IBLTextureParams.x > 0.5)"), std::string::npos);
    EXPECT_NE(shader.find("IBLTextureParams.w"), std::string::npos);
    EXPECT_NE(shader.find("DirectionalShadowParams.w"), std::string::npos);
    EXPECT_NE(shader.find("float4 DirectionalLightColor_Padding;"), std::string::npos);
    EXPECT_NE(shader.find("#define DirectionalLightColor DirectionalLightColor_Padding.xyz"), std::string::npos);
    EXPECT_NE(shader.find("dot(worldPos - CameraPosition, cameraForward)"), std::string::npos);
    EXPECT_NE(shader.find("viewDepth > DirectionalShadowCascadeSplits[i]"), std::string::npos);
    EXPECT_NE(shader.find("float3(uv, (float)cascadeIndex)"), std::string::npos);
    EXPECT_NE(shader.find("int nextCascadeIndex = cascadeIndex + 1;"), std::string::npos);
    EXPECT_NE(shader.find("nextCascadeIndex < cascadeCount"), std::string::npos);
    EXPECT_NE(shader.find("bool insideFadeBand = fadeDistance > 1.0e-5"), std::string::npos);
    EXPECT_NE(shader.find("float nextShadow = SampleDirectionalShadowCascade(worldPos, worldNormal, nextCascadeIndex);"),
              std::string::npos);
    EXPECT_NE(shader.find("shadow = lerp(shadow, nextShadow, fadeT);"), std::string::npos);
    EXPECT_NE(shader.find("float normalBias = max(DirectionalShadowReceiverParams.x, 0.0);"), std::string::npos);
    EXPECT_NE(shader.find("receiverNormal * normalBias"), std::string::npos);
    EXPECT_NE(shader.find("static const int RVX_DIRECTIONAL_SHADOW_POISSON_TAP_COUNT = 16;"), std::string::npos);
    EXPECT_NE(shader.find("static const float2 RVX_DIRECTIONAL_SHADOW_POISSON_DISK[16]"), std::string::npos);
    EXPECT_NE(shader.find("for (int i = 0; i < RVX_DIRECTIONAL_SHADOW_POISSON_TAP_COUNT; ++i)"),
              std::string::npos);
    EXPECT_NE(shader.find("RVX_DIRECTIONAL_SHADOW_POISSON_DISK[i] * filterStepUv"), std::string::npos);
    EXPECT_NE(shader.find("if (filterStepUv <= 1.0e-7)"), std::string::npos);
    EXPECT_EQ(shader.find("for (int y = -1; y <= 1; ++y)"), std::string::npos);
    EXPECT_EQ(shader.find("for (int x = -1; x <= 1; ++x)"), std::string::npos);
    EXPECT_NE(shader.find("tapCount > 0.5 ? visibility / tapCount : 1.0"), std::string::npos);
    EXPECT_NE(shader.find("SampleDirectionalShadowPCF(shadowUV, compareDepth, DirectionalShadowParams.w, cascadeIndex)"),
              std::string::npos);
    EXPECT_NE(shader.find("#define ReceivesShadow ObjectVelocityParams.y"), std::string::npos);
    EXPECT_NE(shader.find("const bool receivesShadow = ReceivesShadow > 0.5;"), std::string::npos);
    EXPECT_NE(shader.find("receivesShadow ? SampleDirectionalShadow(input.WorldPos, normal) : 1.0"),
              std::string::npos);
    EXPECT_NE(shader.find("receivesShadow ? SampleRayTracedShadowMask(input.Position) : 1.0"),
              std::string::npos);
    EXPECT_NE(shader.find("ComposeDirectionalShadowVisibility(rasterShadowVisibility, rayTracedShadowVisibility)"),
              std::string::npos);
    EXPECT_NE(shader.find("DirectionalLightColor * DirectionalLightIntensity"),
              std::string::npos);
    EXPECT_EQ(shader.find("float3(DirectionalLightIntensity, DirectionalLightIntensity, DirectionalLightIntensity)"),
              std::string::npos);
    EXPECT_EQ(shader.find("float3(DirectionalLightIntensity, DirectionalLightIntensity, DirectionalLightIntensity),\n        1.0"),
              std::string::npos);
    EXPECT_EQ(shader.find("float storedDepth = DirectionalShadowMapTexture.SampleLevel(DirectionalShadowSampler, shadowUV, 0).r"),
              std::string::npos);
    EXPECT_EQ(shader.find("float lit = compareDepth <= storedDepth ? 1.0 : 0.0"), std::string::npos);
    EXPECT_EQ(shader.find("float3(4.0, 4.0, 4.0)"), std::string::npos);
    EXPECT_EQ(shader.find("ambientFloor = baseColor.rgb * 0.08"), std::string::npos);
    EXPECT_EQ(shader.find("ambientDiffuse = baseColor.rgb * (1.0 - fresnel) * (1.0 - metallic) * occlusion * 0.12;"),
              std::string::npos);
    EXPECT_EQ(shader.find("ambientSpecular = f0 * occlusion * (1.0 - clampedRoughness) * 0.04;"),
              std::string::npos);
}

TEST_F(PipelineCacheValidationFixture, DefaultLitConsumesMaterialWorkflowAndDoubleSidedFlags)
{
    if (!HasCompilerAvailable())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    const std::string shader = ReadTextFile(FindShaderDirectory() / "DefaultLit.hlsl");
    EXPECT_NE(shader.find("#define MATERIAL_WORKFLOW_METALLIC_ROUGHNESS 0"), std::string::npos);
    EXPECT_NE(shader.find("#define MATERIAL_WORKFLOW_SPECULAR_GLOSSINESS 1"), std::string::npos);
    EXPECT_NE(shader.find("#define MATERIAL_WORKFLOW_UNLIT 2"), std::string::npos);
    EXPECT_NE(shader.find("if (Workflow == MATERIAL_WORKFLOW_UNLIT)"), std::string::npos);
    EXPECT_NE(shader.find("return float4(baseColor.rgb + emissive, baseColor.a);"), std::string::npos);
    EXPECT_NE(shader.find("if (Workflow == MATERIAL_WORKFLOW_SPECULAR_GLOSSINESS)"), std::string::npos);
    EXPECT_NE(shader.find("metallic = 0.0;"), std::string::npos);
    EXPECT_NE(shader.find("float4 doubleSidedTangent = input.WorldTangent;"), std::string::npos);
    EXPECT_NE(shader.find("if (DoubleSided != 0 && dot(normal, viewDir) < 0.0)"), std::string::npos);
    EXPECT_NE(shader.find("normal = -normal;"), std::string::npos);
    EXPECT_NE(shader.find("doubleSidedTangent.w = -doubleSidedTangent.w;"), std::string::npos);
    EXPECT_NE(shader.find("SampleNormalMap(input.TexCoord, normal, doubleSidedTangent)"), std::string::npos);
}

TEST_F(PipelineCacheValidationFixture, DefaultLitUsesFrameLocalLightResources)
{
    if (!HasCompilerAvailable())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    const std::string shader = ReadTextFile(FindShaderDirectory() / "DefaultLit.hlsl");
    EXPECT_NE(shader.find("#include \"Include/Lighting.hlsli\""), std::string::npos);
    EXPECT_NE(shader.find("cbuffer LightConstants : register(b3, space0)"), std::string::npos);
    EXPECT_NE(shader.find("cbuffer ClusterConstants : register(b7, space0)"), std::string::npos);
    EXPECT_NE(shader.find("StructuredBuffer<PointLight> PointLights : register(t4, space0);"), std::string::npos);
    EXPECT_NE(shader.find("StructuredBuffer<SpotLight> SpotLights : register(t5, space0);"), std::string::npos);
    EXPECT_NE(shader.find("StructuredBuffer<GPUCluster> ClusterData : register(t8, space0);"), std::string::npos);
    EXPECT_NE(shader.find("StructuredBuffer<uint> ClusterLightIndices : register(t9, space0);"), std::string::npos);
    EXPECT_NE(shader.find("float3 EvaluateClusteredLocalLights("), std::string::npos);
    EXPECT_NE(shader.find("return EvaluateLinearLocalLights(normal, viewDir, worldPos, baseColor, metallic, roughness);"),
              std::string::npos);
    EXPECT_NE(shader.find("const uint packedLightIndex = ClusterLightIndices[cluster.offset + clusteredLightIndex];"),
              std::string::npos);
    EXPECT_NE(shader.find("const uint pointLightCount = min(NumPointLights, 256u);"), std::string::npos);
    EXPECT_NE(shader.find("const uint spotLightCount = min(NumSpotLights, 128u);"), std::string::npos);
    EXPECT_NE(shader.find("localLight += EvaluatePointLight("), std::string::npos);
    EXPECT_NE(shader.find("localLight += EvaluateSpotLight("), std::string::npos);
    EXPECT_NE(shader.find("directLight += EvaluateClusteredLocalLights("), std::string::npos);
}

TEST_F(PipelineCacheValidationFixture, SceneRendererWiresLightManagerIntoDefaultLitPasses)
{
    if (!HasCompilerAvailable())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    const fs::path rendererPath = FindShaderDirectory().parent_path() / "Private" / "Renderer" / "SceneRenderer.cpp";
    const std::string source = ReadTextFile(rendererPath);
    EXPECT_NE(source.find("#include \"Render/Lighting/LightManager.h\""), std::string::npos);
    EXPECT_NE(source.find("m_lightManager = std::make_unique<LightManager>();"), std::string::npos);
    EXPECT_NE(source.find("m_lightManager->Initialize(renderContext->GetDevice());"), std::string::npos);
    EXPECT_NE(source.find("m_lightManager->CollectLights(m_renderScene);"), std::string::npos);
    EXPECT_NE(source.find("m_lightManager->UpdateGPUBuffers();"), std::string::npos);
    EXPECT_NE(source.find("m_localLightingStats.pointLightCount = m_lightManager->GetPointLightCount();"),
              std::string::npos);
    EXPECT_NE(source.find("m_localLightingStats.spotLightCount = m_lightManager->GetSpotLightCount();"),
              std::string::npos);
    EXPECT_NE(source.find("m_localLightingStats.pointShadowRequestCount = m_lightManager->GetPointShadowRequestCount();"),
              std::string::npos);
    EXPECT_NE(source.find("m_localLightingStats.spotShadowRequestCount = m_lightManager->GetSpotShadowRequestCount();"),
              std::string::npos);
    EXPECT_NE(source.find("m_localLightingStats.localShadowRequestCount = m_lightManager->GetLocalShadowRequestCount();"),
              std::string::npos);
    EXPECT_NE(source.find("diagnostics.localShadowRequestCount = m_localLightingStats.localShadowRequestCount;"),
              std::string::npos);
    EXPECT_NE(source.find("m_pipelineCache->GetLastFrameLightBindingResult()"), std::string::npos);
    EXPECT_NE(source.find("m_clusteredLighting.get());"), std::string::npos);
}

TEST_F(PipelineCacheValidationFixture, SceneRendererBuildsClusteredLightingFrameData)
{
    if (!HasCompilerAvailable())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    const fs::path renderRoot = FindShaderDirectory().parent_path();
    const fs::path rendererHeaderPath = renderRoot / "Include" / "Render" / "Renderer" / "SceneRenderer.h";
    const fs::path rendererSourcePath = renderRoot / "Private" / "Renderer" / "SceneRenderer.cpp";
    const std::string header = ReadTextFile(rendererHeaderPath);
    const std::string source = ReadTextFile(rendererSourcePath);

    EXPECT_NE(source.find("#include \"Render/Lighting/ClusteredLighting.h\""), std::string::npos);
    EXPECT_NE(header.find("std::unique_ptr<ClusteredLighting> m_clusteredLighting;"), std::string::npos);
    EXPECT_NE(header.find("const SceneClusteredLightingStats& GetClusteredLightingStats() const"),
              std::string::npos);
    EXPECT_NE(source.find("m_clusteredLighting = std::make_unique<ClusteredLighting>();"), std::string::npos);
    EXPECT_NE(source.find("m_clusteredLighting->Initialize(renderContext->GetDevice())"), std::string::npos);
    EXPECT_NE(source.find("m_clusteredLighting->BeginFrame(m_viewData.viewMatrix,"), std::string::npos);
    EXPECT_NE(source.find("m_clusteredLighting->AssignLights(*m_lightManager)"), std::string::npos);
    EXPECT_NE(source.find("m_clusteredLighting->UploadFrameData()"), std::string::npos);
    EXPECT_NE(source.find("diagnostics.clusteredLightingGpuBuffersUploaded = m_clusteredLightingStats.gpuBuffersUploaded;"),
              std::string::npos);
    EXPECT_NE(source.find("diagnostics.clusteredLightingActiveClusters = m_clusteredLightingStats.activeClusters;"),
              std::string::npos);
}

TEST_F(PipelineCacheValidationFixture, LightManagerCreatesStructuredLocalLightBuffers)
{
    if (!HasCompilerAvailable())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    const fs::path lightManagerPath = FindShaderDirectory().parent_path() / "Private" / "Lighting" / "LightManager.cpp";
    const std::string source = ReadTextFile(lightManagerPath);
    EXPECT_NE(source.find("desc.size = AlignLightConstantsSize(sizeof(LightConstants));"), std::string::npos);
    EXPECT_NE(source.find("desc.usage = RHIBufferUsage::Structured | RHIBufferUsage::ShaderResource;"),
              std::string::npos);
    EXPECT_NE(source.find("desc.stride = sizeof(GPUPointLight);"), std::string::npos);
    EXPECT_NE(source.find("desc.stride = sizeof(GPUSpotLight);"), std::string::npos);
}

TEST_F(PipelineCacheValidationFixture, LightManagerTracksRequestedLocalShadowsSeparatelyFromBuffers)
{
    RVX::RenderScene scene;

    RVX::RenderLight point;
    point.type = RVX::RenderLight::Type::Point;
    point.castsShadow = true;
    scene.AddLight(point);

    RVX::RenderLight spot;
    spot.type = RVX::RenderLight::Type::Spot;
    spot.castsShadow = true;
    scene.AddLight(spot);

    RVX::RenderLight unshadowedPoint;
    unshadowedPoint.type = RVX::RenderLight::Type::Point;
    unshadowedPoint.castsShadow = false;
    scene.AddLight(unshadowedPoint);

    RVX::LightManager lights;
    lights.CollectLights(scene);

    EXPECT_EQ(lights.GetPointLightCount(), 2u);
    EXPECT_EQ(lights.GetSpotLightCount(), 1u);
    EXPECT_EQ(lights.GetPointShadowRequestCount(), 1u);
    EXPECT_EQ(lights.GetSpotShadowRequestCount(), 1u);
    EXPECT_EQ(lights.GetLocalShadowRequestCount(), 2u);

    lights.Clear();
    EXPECT_EQ(lights.GetLocalShadowRequestCount(), 0u);
}

TEST_F(PipelineCacheValidationFixture, SceneRendererClearsAmbientFloorWhenTextureIBLIsReady)
{
    if (!HasCompilerAvailable())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    const fs::path sceneRendererPath = FindShaderDirectory().parent_path() /
        "Private" / "Renderer" / "SceneRenderer.cpp";
    const std::string source = ReadTextFile(sceneRendererPath);
    EXPECT_NE(source.find("m_viewData.ambientFloorIntensity = 0.08f;"), std::string::npos);
    EXPECT_NE(source.find("m_viewData.ambientFloorIntensity = 0.0f;"), std::string::npos);
}

TEST_F(PipelineCacheValidationFixture, SceneRendererUsesSamePrimaryDirectionalLightForDefaultLitAndShadowPass)
{
    if (!HasCompilerAvailable())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    const fs::path sceneRendererPath = FindShaderDirectory().parent_path() /
        "Private" / "Renderer" / "SceneRenderer.cpp";
    const std::string source = ReadTextFile(sceneRendererPath);
    EXPECT_NE(source.find("m_viewData.directionalLightDirection = Vec3{0.5f, -0.8f, 0.3f};"),
              std::string::npos);
    EXPECT_NE(source.find("m_viewData.directionalLightColor = Vec3{1.0f, 1.0f, 1.0f};"), std::string::npos);
    EXPECT_NE(source.find("m_viewData.directionalLightDirection = light.direction;"), std::string::npos);
    EXPECT_NE(source.find("m_viewData.directionalLightIntensity = light.intensity;"), std::string::npos);
    EXPECT_NE(source.find("m_viewData.directionalLightColor = light.color;"), std::string::npos);
    EXPECT_NE(source.find("m_shadowPass->SetDirectionalLight(light.direction, light.color, light.intensity);"),
              std::string::npos);
    EXPECT_NE(source.find("m_rayTracedShadowPass->SetDirectionalLight(light.direction, light.color, light.intensity);"),
              std::string::npos);
}

TEST_F(PipelineCacheValidationFixture, SceneRendererAppliesShadowQualityConfigToShadowPass)
{
    if (!HasCompilerAvailable())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    const fs::path renderRoot = FindShaderDirectory().parent_path();
    const std::string header = ReadTextFile(renderRoot / "Include" / "Render" / "Renderer" / "SceneRenderer.h");
    const std::string source = ReadTextFile(renderRoot / "Private" / "Renderer" / "SceneRenderer.cpp");

    EXPECT_NE(header.find("void ApplyShadowPassConfig(const ShadowPassConfig& config);"), std::string::npos);
    EXPECT_NE(header.find("const ShadowPassConfig& GetShadowPassConfig() const"), std::string::npos);
    EXPECT_NE(header.find("ShadowPassConfig m_shadowPassConfig;"), std::string::npos);
    EXPECT_NE(source.find("m_shadowPassConfig = config;"), std::string::npos);
    EXPECT_NE(source.find("if (m_shadowPass)"), std::string::npos);
    EXPECT_NE(source.find("m_shadowPass->SetConfig(m_shadowPassConfig);"), std::string::npos);
    EXPECT_NE(source.find("shadowPass->SetConfig(m_shadowPassConfig);"), std::string::npos);
    EXPECT_NE(source.find("m_rayTracedShadowPass->SetConfig(m_shadowPassConfig);"), std::string::npos);
    EXPECT_NE(source.find("rayTracedShadowPass->SetConfig(m_shadowPassConfig);"), std::string::npos);
}

TEST_F(PipelineCacheValidationFixture, SceneRendererWiresRayTracingSceneBuildBeforeRenderPasses)
{
    if (!HasCompilerAvailable())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    const fs::path renderRoot = FindShaderDirectory().parent_path();
    const std::string header = ReadTextFile(renderRoot / "Include" / "Render" / "Renderer" / "SceneRenderer.h");
    const std::string source = ReadTextFile(renderRoot / "Private" / "Renderer" / "SceneRenderer.cpp");

    EXPECT_NE(header.find("#include \"Render/RayTracing/RayTracingSceneManager.h\""), std::string::npos);
    EXPECT_NE(header.find("const RayTracingSceneManagerStats& GetRayTracingSceneStats() const;"), std::string::npos);
    EXPECT_NE(header.find("RHIAccelerationStructure* GetRayTracingTopLevelAS() const;"), std::string::npos);
    EXPECT_NE(header.find("void PrepareRayTracingScene();"), std::string::npos);
    EXPECT_NE(header.find("void AddRayTracingSceneBuildPass();"), std::string::npos);
    EXPECT_NE(header.find("std::unique_ptr<RayTracingSceneManager> m_rayTracingSceneManager;"), std::string::npos);

    EXPECT_NE(source.find("m_rayTracingSceneManager = std::make_unique<RayTracingSceneManager>();"), std::string::npos);
    EXPECT_NE(source.find("m_rayTracingSceneManager->Initialize(m_renderContext->GetDevice());"), std::string::npos);
    EXPECT_NE(source.find("RayTracingSceneBuildPlan plan = BuildRayTracingSceneBuildPlan("), std::string::npos);
    EXPECT_NE(source.find("m_rayTracingSceneManager->SetBLASCacheEvictionFrameThreshold("),
              std::string::npos);
    EXPECT_NE(source.find("m_rayTracingSceneManager->SetTrackedResourceBudget("), std::string::npos);
    EXPECT_NE(source.find("m_rayTracingSceneManager->Prepare(plan);"), std::string::npos);
    EXPECT_NE(source.find("\"RayTracingSceneBuild\""), std::string::npos);
    EXPECT_NE(source.find("RenderGraphPassType::RayTracing"), std::string::npos);
    EXPECT_NE(source.find("data.manager->RecordBuildCommands(ctx);"), std::string::npos);

    const auto renderStart = source.find("void SceneRenderer::Render()");
    ASSERT_NE(renderStart, std::string::npos);
    const auto prepareCall = source.find("PrepareRayTracingScene();", renderStart);
    const auto clearCall = source.find("m_renderGraph->Clear();", renderStart);
    ASSERT_NE(prepareCall, std::string::npos);
    ASSERT_NE(clearCall, std::string::npos);
    EXPECT_LT(prepareCall, clearCall);

    const auto buildGraphStart = source.find("void SceneRenderer::BuildRenderGraph()");
    ASSERT_NE(buildGraphStart, std::string::npos);
    const auto rtBuildPassCall = source.find("AddRayTracingSceneBuildPass();", buildGraphStart);
    const auto passLoop = source.find("for (auto& pass : m_passRegistry->GetPasses())", buildGraphStart);
    ASSERT_NE(rtBuildPassCall, std::string::npos);
    ASSERT_NE(passLoop, std::string::npos);
    EXPECT_LT(rtBuildPassCall, passLoop);
}

TEST_F(PipelineCacheValidationFixture, RayTracingSceneManagerInvalidatesFrameOutputsOnFallback)
{
    if (!HasCompilerAvailable())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    const fs::path renderRoot = FindShaderDirectory().parent_path();
    const std::string managerHeader =
        ReadTextFile(renderRoot / "Include" / "Render" / "RayTracing" / "RayTracingSceneManager.h");
    const std::string managerSource =
        ReadTextFile(renderRoot / "Private" / "RayTracing" / "RayTracingSceneManager.cpp");

    EXPECT_NE(managerHeader.find("void InvalidateFrameOutputs();"), std::string::npos);
    EXPECT_NE(managerHeader.find("void SetTrackedResourceBudget(uint64 budgetBytes)"), std::string::npos);
    EXPECT_NE(managerHeader.find("void SetBLASScratchReleaseFrameDelay(uint64 frameDelay)"), std::string::npos);
    EXPECT_NE(managerHeader.find("size_t resourceBudgetEvictedBLASCount = 0;"), std::string::npos);
    EXPECT_NE(managerHeader.find("size_t releasedBLASScratchCount = 0;"), std::string::npos);
    EXPECT_NE(managerHeader.find("size_t pendingBLASScratchReleaseCount = 0;"), std::string::npos);
    EXPECT_NE(managerHeader.find("bool resourceBudgetEvictionAttempted = false;"), std::string::npos);
    EXPECT_NE(managerHeader.find("bool resourceBudgetExceeded = false;"), std::string::npos);
    EXPECT_NE(managerHeader.find("bool resourceByteAccountingOverflowed = false;"), std::string::npos);
    EXPECT_NE(managerHeader.find("uint64 trackedResourceBudget = 0;"), std::string::npos);
    EXPECT_NE(managerSource.find("void RayTracingSceneManager::EvictUnusedBLASForResourceBudget"),
              std::string::npos);
    EXPECT_NE(managerSource.find("void RayTracingSceneManager::ReleaseRetiredBLASScratchBuffers()"),
              std::string::npos);
    EXPECT_NE(managerSource.find("ReleaseRetiredBLASScratchBuffers();"), std::string::npos);
    EXPECT_NE(managerSource.find("entry->scratchReleasePending = true;"), std::string::npos);
    EXPECT_NE(managerSource.find("m_stats.releasedBLASScratchBytes = AddSaturatingUint64("),
              std::string::npos);
    EXPECT_EQ(managerSource.find("m_stats.releasedBLASScratchBytes +="), std::string::npos);
    EXPECT_NE(managerSource.find("m_stats.resourceBudgetEvictionAttempted = true;"), std::string::npos);
    EXPECT_NE(managerSource.find("++m_stats.resourceBudgetEvictedBLASCount;"), std::string::npos);
    EXPECT_NE(managerSource.find("isRequestedThisFrame(it->key)"), std::string::npos);
    EXPECT_NE(managerSource.find("m_stats.resourceBudgetExceeded ="), std::string::npos);
    EXPECT_NE(managerSource.find("if (!m_stats.resourceBudgetExceeded)"), std::string::npos);
    EXPECT_NE(managerSource.find("while (m_stats.resourceBudgetExceeded)"), std::string::npos);
    EXPECT_EQ(managerSource.find("while (m_stats.totalTrackedResourceBytes > m_trackedResourceBudget)"),
              std::string::npos);

    const auto prepareStart = managerSource.find("bool RayTracingSceneManager::Prepare");
    ASSERT_NE(prepareStart, std::string::npos);
    const auto firstBudgetEviction =
        managerSource.find("EvictUnusedBLASForResourceBudget(plan.blasBuilds);", prepareStart);
    ASSERT_NE(firstBudgetEviction, std::string::npos);
    const auto blasCreation = managerSource.find("GetOrCreateBLAS(build)", firstBudgetEviction);
    ASSERT_NE(blasCreation, std::string::npos);
    const auto postCreateBudgetEviction =
        managerSource.find("EvictUnusedBLASForResourceBudget(plan.blasBuilds);", blasCreation);
    ASSERT_NE(postCreateBudgetEviction, std::string::npos);
    const auto collectFrameBLASLambda =
        managerSource.find("auto collectFrameBLAS = [this, &plan]", postCreateBudgetEviction);
    ASSERT_NE(collectFrameBLASLambda, std::string::npos);
    const auto frameBLASCollection =
        managerSource.find("m_frameBLAS.reserve(plan.blasBuilds.size());", collectFrameBLASLambda);
    ASSERT_NE(frameBLASCollection, std::string::npos);
    EXPECT_LT(firstBudgetEviction, blasCreation);
    EXPECT_LT(blasCreation, postCreateBudgetEviction);
    EXPECT_LT(postCreateBudgetEviction, collectFrameBLASLambda);
    EXPECT_LT(collectFrameBLASLambda, frameBLASCollection);

    const auto finalResourceStats = managerSource.find("UpdateResourceStats();", frameBLASCollection);
    ASSERT_NE(finalResourceStats, std::string::npos);
    const auto finalBudgetEviction =
        managerSource.find("EvictUnusedBLASForResourceBudget(plan.blasBuilds);", finalResourceStats);
    ASSERT_NE(finalBudgetEviction, std::string::npos);
    const auto finalFrameBLASRefresh = managerSource.find("if (!collectFrameBLAS(nullptr))", finalBudgetEviction);
    ASSERT_NE(finalFrameBLASRefresh, std::string::npos);
    const auto finalPendingBuildCountRefresh =
        managerSource.find("m_stats.pendingBLASBuildCount = m_pendingBLASBuilds.size();", finalFrameBLASRefresh);
    ASSERT_NE(finalPendingBuildCountRefresh, std::string::npos);
    EXPECT_LT(finalResourceStats, finalBudgetEviction);
    EXPECT_LT(finalBudgetEviction, finalFrameBLASRefresh);
    EXPECT_LT(finalFrameBLASRefresh, finalPendingBuildCountRefresh);
    const auto budgetEvictionStart =
        managerSource.find("void RayTracingSceneManager::EvictUnusedBLASForResourceBudget");
    ASSERT_NE(budgetEvictionStart, std::string::npos);
    const auto noEvictionCandidateBreak =
        managerSource.find("if (evictionCandidate == m_blasCache.end())", budgetEvictionStart);
    ASSERT_NE(noEvictionCandidateBreak, std::string::npos);
    const auto evictionAttemptMarker =
        managerSource.find("m_stats.resourceBudgetEvictionAttempted = true;", budgetEvictionStart);
    ASSERT_NE(evictionAttemptMarker, std::string::npos);
    const auto eraseEvictionCandidate =
        managerSource.find("m_blasCache.erase(evictionCandidate);", budgetEvictionStart);
    ASSERT_NE(eraseEvictionCandidate, std::string::npos);
    EXPECT_LT(noEvictionCandidateBreak, evictionAttemptMarker);
    EXPECT_LT(evictionAttemptMarker, eraseEvictionCandidate);
    EXPECT_NE(managerSource.find("#include <limits>"), std::string::npos);
    EXPECT_NE(managerSource.find("bool TryMultiplyUint64"), std::string::npos);
    EXPECT_NE(managerSource.find("bool TryAlignUpUint64"), std::string::npos);
    EXPECT_NE(managerSource.find("uint64 AddSaturatingUint64"), std::string::npos);
    EXPECT_NE(managerSource.find("std::numeric_limits<uint64>::max()"), std::string::npos);
    EXPECT_NE(managerSource.find("TryMultiplyUint64(static_cast<uint64>(records.size()), sizeof(RHIRayTracingInstanceRecord), dataSize)"),
              std::string::npos);
    EXPECT_NE(managerSource.find("TryAlignUpUint64(dataSize, RVX_RAY_TRACING_INSTANCE_BUFFER_ALIGNMENT, requiredSize)"),
              std::string::npos);
    EXPECT_NE(managerSource.find("TryMultiplyUint64(static_cast<uint64>(records.size()), sizeof(RayTracingInstanceMaterialMetadata), dataSize)"),
              std::string::npos);
    EXPECT_NE(managerSource.find("TryMultiplyUint64(static_cast<uint64>(records.size()), sizeof(RayTracingInstanceAlphaMetadata), dataSize)"),
              std::string::npos);
    EXPECT_NE(managerSource.find("bool trackedResourceByteAccountingOverflowed = false;"),
              std::string::npos);
    EXPECT_NE(managerSource.find("m_stats.resourceByteAccountingOverflowed =\n"
                                 "        m_stats.resourceByteAccountingOverflowed || trackedResourceByteAccountingOverflowed;"),
              std::string::npos);
    EXPECT_NE(managerSource.find("trackedResourceByteAccountingOverflowed ||"), std::string::npos);
    EXPECT_EQ(managerSource.find("(m_stats.resourceByteAccountingOverflowed ||\n"
                                 "         m_stats.totalTrackedResourceBytes > m_trackedResourceBudget)"),
              std::string::npos);
    EXPECT_NE(managerSource.find("cachedBLASAccelerationStructureBytes = AddSaturatingUint64"),
              std::string::npos);
    EXPECT_NE(managerSource.find("uint64 totalBLASResourceBytes = 0;"), std::string::npos);
    EXPECT_NE(managerSource.find("totalBLASResourceBytes = AddSaturatingUint64"),
              std::string::npos);
    EXPECT_EQ(managerSource.find("return accelerationStructureBytes + scratchBytes;"),
              std::string::npos);
    EXPECT_NE(managerSource.find("totalTrackedResourceBytes = AddSaturatingUint64"), std::string::npos);
    EXPECT_EQ(managerSource.find("records.size() * sizeof("), std::string::npos);
    EXPECT_EQ(managerSource.find("AlignUp(dataSize"), std::string::npos);
    EXPECT_EQ(managerSource.find("cachedBLASAccelerationStructureBytes +="), std::string::npos);
    EXPECT_EQ(managerSource.find("cachedBLASScratchBytes +="), std::string::npos);
    EXPECT_EQ(managerSource.find("m_stats.cachedBLASAccelerationStructureBytes +"), std::string::npos);

    const auto invalidateStart =
        managerSource.find("void RayTracingSceneManager::InvalidateFrameOutputs()");
    ASSERT_NE(invalidateStart, std::string::npos);
    const auto fallbackStart =
        managerSource.find("void RayTracingSceneManager::SetFallback", invalidateStart);
    ASSERT_NE(fallbackStart, std::string::npos);
    const std::string invalidateBody = managerSource.substr(invalidateStart, fallbackStart - invalidateStart);

    EXPECT_NE(invalidateBody.find("m_frameBLAS.clear();"), std::string::npos);
    EXPECT_NE(invalidateBody.find("m_pendingBLASBuilds.clear();"), std::string::npos);
    EXPECT_NE(invalidateBody.find("m_instanceRecords.clear();"), std::string::npos);
    EXPECT_NE(invalidateBody.find("m_instanceMaterialMetadataRecords.clear();"), std::string::npos);
    EXPECT_NE(invalidateBody.find("m_instanceMaterialTextureIds.clear();"), std::string::npos);
    EXPECT_NE(invalidateBody.find("m_instanceAlphaMetadataRecords.clear();"), std::string::npos);
    EXPECT_NE(invalidateBody.find("m_instanceAlphaTextureIds.clear();"), std::string::npos);
    EXPECT_NE(invalidateBody.find("m_instanceBuffer.Reset();"), std::string::npos);
    EXPECT_NE(invalidateBody.find("m_instanceBufferSize = 0;"), std::string::npos);
    EXPECT_NE(invalidateBody.find("m_instanceMaterialMetadataBuffer.Reset();"), std::string::npos);
    EXPECT_NE(invalidateBody.find("m_instanceMaterialMetadataBufferSize = 0;"), std::string::npos);
    EXPECT_NE(invalidateBody.find("m_instanceAlphaMetadataBuffer.Reset();"), std::string::npos);
    EXPECT_NE(invalidateBody.find("m_instanceAlphaMetadataBufferSize = 0;"), std::string::npos);
    EXPECT_NE(invalidateBody.find("m_topLevelAS.Reset();"), std::string::npos);
    EXPECT_NE(invalidateBody.find("m_topLevelSizes = {};"), std::string::npos);
    EXPECT_NE(invalidateBody.find("m_topLevelScratchBuffer.Reset();"), std::string::npos);
    EXPECT_NE(invalidateBody.find("m_topLevelBuildDesc = {};"), std::string::npos);
    EXPECT_EQ(invalidateBody.find("m_blasCache.clear();"), std::string::npos);

    const auto fallbackInvalidate = managerSource.find("InvalidateFrameOutputs();", fallbackStart);
    const auto fallbackPreparedFalse = managerSource.find("m_stats.prepared = false;", fallbackStart);
    ASSERT_NE(fallbackInvalidate, std::string::npos);
    ASSERT_NE(fallbackPreparedFalse, std::string::npos);
    EXPECT_LT(fallbackInvalidate, fallbackPreparedFalse);
    EXPECT_NE(managerSource.find("m_stats.hasTopLevelAS = false;", fallbackStart), std::string::npos);
    EXPECT_NE(managerSource.find("m_stats.hasInstanceBuffer = false;", fallbackStart), std::string::npos);
    EXPECT_NE(managerSource.find("m_stats.hasMaterialMetadataBuffer = false;", fallbackStart), std::string::npos);
    EXPECT_NE(managerSource.find("m_stats.hasAlphaMetadataBuffer = false;", fallbackStart), std::string::npos);
    EXPECT_NE(managerSource.find("m_stats.pendingBLASBuildCount = 0;", fallbackStart), std::string::npos);
}

TEST_F(PipelineCacheValidationFixture, RayTracingSceneManagerRetriesIncompleteBLASCacheEntries)
{
    if (!HasCompilerAvailable())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    const fs::path renderRoot = FindShaderDirectory().parent_path();
    const std::string managerSource =
        ReadTextFile(renderRoot / "Private" / "RayTracing" / "RayTracingSceneManager.cpp");

    const auto getOrCreateStart =
        managerSource.find("RayTracingSceneManager::BLASCacheEntry* RayTracingSceneManager::GetOrCreateBLAS(");
    ASSERT_NE(getOrCreateStart, std::string::npos);
    const auto scratchStart = managerSource.find("bool RayTracingSceneManager::EnsureScratchBuffer", getOrCreateStart);
    ASSERT_NE(scratchStart, std::string::npos);
    const std::string getOrCreateBody = managerSource.substr(getOrCreateStart, scratchStart - getOrCreateStart);

    EXPECT_NE(getOrCreateBody.find("const bool sizesMissing = !entry->sizes.IsValid();"), std::string::npos);
    EXPECT_NE(getOrCreateBody.find("const bool accelerationStructureMissing ="), std::string::npos);
    EXPECT_NE(getOrCreateBody.find("const bool buildRequired ="), std::string::npos);
    EXPECT_NE(getOrCreateBody.find("entry->needsBuild"), std::string::npos);
    EXPECT_NE(getOrCreateBody.find("if (buildRequired)"), std::string::npos);
    EXPECT_NE(getOrCreateBody.find("EnsureScratchBuffer(entry->scratchBuffer"), std::string::npos);
    EXPECT_NE(getOrCreateBody.find("entry->scratchReleasePending = false;"), std::string::npos);
    EXPECT_EQ(getOrCreateBody.find("!entry->scratchBuffer ||"), std::string::npos);

    const auto buildRequiredCheck = getOrCreateBody.find("const bool buildRequired =");
    const auto createBranch = getOrCreateBody.find("if (buildRequired)");
    const auto dirtyMark = getOrCreateBody.find("entry->needsBuild = true;");
    const auto scratchEnsure = getOrCreateBody.find("EnsureScratchBuffer(entry->scratchBuffer");
    const auto reusedBranch = getOrCreateBody.find("++m_stats.reusedBLASCount;");
    ASSERT_NE(buildRequiredCheck, std::string::npos);
    ASSERT_NE(createBranch, std::string::npos);
    ASSERT_NE(dirtyMark, std::string::npos);
    ASSERT_NE(scratchEnsure, std::string::npos);
    ASSERT_NE(reusedBranch, std::string::npos);
    EXPECT_LT(buildRequiredCheck, createBranch);
    EXPECT_LT(createBranch, dirtyMark);
    EXPECT_LT(dirtyMark, scratchEnsure);
    EXPECT_LT(scratchEnsure, reusedBranch);
}

TEST_F(PipelineCacheValidationFixture, RayTracingValidationRejectsNonFiniteTLASInstanceTransforms)
{
    if (!HasCompilerAvailable())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    const fs::path rhiRoot = FindShaderDirectory().parent_path().parent_path() / "RHI";
    const std::string rhiHeader = ReadTextFile(rhiRoot / "Include" / "RHI" / "RHIRayTracing.h");
    const std::string descriptorHeader = ReadTextFile(rhiRoot / "Include" / "RHI" / "RHIDescriptor.h");

    EXPECT_NE(descriptorHeader.find("#include \"RHI/RHIRayTracing.h\""), std::string::npos);
    EXPECT_NE(descriptorHeader.find("binding.accelerationStructure->GetType() != RHIAccelerationStructureType::TopLevel"),
              std::string::npos);
    EXPECT_NE(descriptorHeader.find("descriptor acceleration structure binding requires a top-level acceleration structure"),
              std::string::npos);
    EXPECT_NE(descriptorHeader.find("binding.accelerationStructure->GetGPUVirtualAddress() == 0"),
              std::string::npos);
    EXPECT_NE(descriptorHeader.find("descriptor acceleration structure binding requires a non-zero GPU address"),
              std::string::npos);

    EXPECT_NE(rhiHeader.find("#include <cmath>"), std::string::npos);
    EXPECT_NE(rhiHeader.find("inline bool IsRHIRayTracingInstanceTransformFinite"), std::string::npos);
    EXPECT_NE(rhiHeader.find("ValidateRHIAccelerationStructureDesc"), std::string::npos);
    EXPECT_NE(rhiHeader.find("AreRHIRayTracingGeometryFlagsValid"), std::string::npos);
    EXPECT_NE(rhiHeader.find("AreRHIAccelerationStructureBuildFlagsValid"), std::string::npos);
    EXPECT_NE(rhiHeader.find("AreRHIRayTracingInstanceFlagsValid"), std::string::npos);
    EXPECT_NE(rhiHeader.find("ray tracing geometry flags contain unknown bits"), std::string::npos);
    EXPECT_NE(rhiHeader.find("ray tracing geometry type is invalid"), std::string::npos);
    EXPECT_NE(rhiHeader.find("BLAS build flags contain unknown bits"), std::string::npos);
    EXPECT_NE(rhiHeader.find("TLAS build flags contain unknown bits"), std::string::npos);
    EXPECT_NE(rhiHeader.find("TLAS instance flags contain unknown bits"), std::string::npos);
    EXPECT_NE(rhiHeader.find("TLAS build desc must use either an instance buffer or CPU instances, not both"),
              std::string::npos);
    EXPECT_NE(rhiHeader.find("desc.instanceBuffer && !desc.instances.empty()"), std::string::npos);
    EXPECT_NE(rhiHeader.find("TryMultiplyRHIRayTracingCount"), std::string::npos);
    EXPECT_NE(rhiHeader.find("TryGetRHIRayTracingDispatchRayCount"), std::string::npos);
    EXPECT_NE(rhiHeader.find("DispatchRays ray count exceeds 64-bit range"), std::string::npos);
    EXPECT_NE(rhiHeader.find("GetRayTracingPipeline() const = 0"), std::string::npos);
    EXPECT_NE(rhiHeader.find("boundRayTracingPipeline = nullptr"), std::string::npos);
    EXPECT_NE(rhiHeader.find("DispatchRays shader table must match the bound ray tracing pipeline"),
              std::string::npos);
    EXPECT_NE(rhiHeader.find("acceleration structure type is invalid"), std::string::npos);
    EXPECT_NE(rhiHeader.find("acceleration structure size must be non-zero"), std::string::npos);
    EXPECT_NE(rhiHeader.find("RVX_RAY_TRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT = 256"),
              std::string::npos);
    EXPECT_NE(rhiHeader.find("IsRHIRayTracingAccelerationStructureSizeAligned"), std::string::npos);
    EXPECT_NE(rhiHeader.find("IsRHIRayTracingAccelerationStructureSizeAligned(accelerationStructureSize)"),
              std::string::npos);
    EXPECT_NE(rhiHeader.find("HasValidUpdateScratchSize()"), std::string::npos);
    EXPECT_NE(rhiHeader.find("IsRHIRayTracingAccelerationStructureSizeAligned(desc.size)"),
              std::string::npos);
    EXPECT_NE(rhiHeader.find("acceleration structure size must be 256-byte aligned"), std::string::npos);
    EXPECT_NE(rhiHeader.find("!std::isfinite(value)"), std::string::npos);
    EXPECT_NE(rhiHeader.find("inline bool HasRHIRayTracingASInputUsage"), std::string::npos);
    EXPECT_NE(rhiHeader.find("inline bool IsRHIRayTracingBufferRangeValid"), std::string::npos);
    EXPECT_NE(rhiHeader.find("inline bool TryGetRHIRayTracingStridedRangeSize"), std::string::npos);
    EXPECT_NE(rhiHeader.find("inline constexpr uint32 RVX_RAY_TRACING_MAX_ATTRIBUTE_SIZE = 32;"),
              std::string::npos);
    EXPECT_NE(rhiHeader.find("ray tracing pipeline max attribute size exceeds the native 32-byte limit"),
              std::string::npos);
    EXPECT_NE(rhiHeader.find("general shader group cannot include hit shaders"), std::string::npos);
    EXPECT_NE(rhiHeader.find("triangle hit group cannot include general or intersection shaders"),
              std::string::npos);
    EXPECT_NE(rhiHeader.find("procedural hit group cannot include a general shader"), std::string::npos);
    EXPECT_NE(rhiHeader.find("procedural hit group any-hit shader has the wrong stage"), std::string::npos);
    EXPECT_NE(rhiHeader.find("ray tracing pipeline shader group has an invalid type"), std::string::npos);
    EXPECT_NE(rhiHeader.find("ray tracing pipeline requires a ray-generation shader group"),
              std::string::npos);
    EXPECT_NE(rhiHeader.find("hasRayGenerationGroup"), std::string::npos);
    EXPECT_NE(rhiHeader.find("group.type == RHIRayTracingShaderGroupType::ProceduralHitGroup"),
              std::string::npos);
    EXPECT_NE(rhiHeader.find("#include <unordered_set>"), std::string::npos);
    EXPECT_NE(rhiHeader.find("MakeRHIRayTracingDefaultGeneralExportName"), std::string::npos);
    EXPECT_NE(rhiHeader.find("MakeRHIRayTracingDefaultHitGroupExportName"), std::string::npos);
    EXPECT_NE(rhiHeader.find("AddRHIRayTracingStateObjectExport"), std::string::npos);
    EXPECT_NE(rhiHeader.find("std::unordered_set<std::string> stateObjectExports"), std::string::npos);
    EXPECT_NE(rhiHeader.find("ray tracing pipeline shader group export name cannot be empty"),
              std::string::npos);
    EXPECT_NE(rhiHeader.find("ray tracing pipeline shader exports must be unique"), std::string::npos);
    EXPECT_NE(rhiHeader.find("groupExportName + \"_ClosestHit\""), std::string::npos);
    EXPECT_NE(rhiHeader.find("groupExportName + \"_AnyHit\""), std::string::npos);
    EXPECT_NE(rhiHeader.find("groupExportName + \"_Intersection\""), std::string::npos);
    EXPECT_NE(rhiHeader.find("HasRHIRayTracingASInputUsage(*desc.instanceBuffer)"), std::string::npos);
    EXPECT_NE(rhiHeader.find("desc.instanceBuffer->GetSize()"), std::string::npos);
    EXPECT_NE(rhiHeader.find("TLAS instance buffer requires AccelerationStructureInput and DeviceAddress usage"),
              std::string::npos);
    EXPECT_NE(rhiHeader.find("TLAS instance buffer range exceeds buffer size"), std::string::npos);
    EXPECT_NE(rhiHeader.find("triangle geometry vertex buffer requires AccelerationStructureInput and DeviceAddress usage"),
              std::string::npos);
    EXPECT_NE(rhiHeader.find("indexed triangle geometry index buffer requires AccelerationStructureInput and DeviceAddress usage"),
              std::string::npos);
    EXPECT_NE(rhiHeader.find("AABB geometry buffer requires AccelerationStructureInput and DeviceAddress usage"),
              std::string::npos);
    EXPECT_NE(rhiHeader.find("triangle geometry vertex range exceeds vertex buffer size"), std::string::npos);
    EXPECT_NE(rhiHeader.find("indexed triangle geometry range exceeds index buffer size"), std::string::npos);
    EXPECT_NE(rhiHeader.find("AABB geometry range exceeds AABB buffer size"), std::string::npos);
    EXPECT_NE(rhiHeader.find("instance.bottomLevel->GetType() != RHIAccelerationStructureType::BottomLevel"),
              std::string::npos);
    EXPECT_NE(rhiHeader.find("instance.bottomLevel->GetGPUVirtualAddress() == 0"),
              std::string::npos);
    EXPECT_NE(rhiHeader.find("TLAS instance requires a bottom-level acceleration structure"),
              std::string::npos);
    EXPECT_NE(rhiHeader.find("TLAS instance requires a non-zero BLAS GPU address"),
              std::string::npos);
    EXPECT_NE(rhiHeader.find("TLAS instance transform must contain only finite values"),
              std::string::npos);
}

TEST_F(PipelineCacheValidationFixture, RayTracingSceneBuildPlanSkipsZeroInstanceMasksBeforeBLASWork)
{
    if (!HasCompilerAvailable())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    const fs::path renderRoot = FindShaderDirectory().parent_path();
    const std::string sceneHeader =
        ReadTextFile(renderRoot / "Include" / "Render" / "RayTracing" / "RayTracingScene.h");
    const std::string sceneSource =
        ReadTextFile(renderRoot / "Private" / "RayTracing" / "RayTracingScene.cpp");

    EXPECT_NE(sceneHeader.find("InstanceMaskZero"), std::string::npos);
    EXPECT_NE(sceneSource.find("const uint32 resolvedInstanceMask = ResolveRayTracingInstanceMask"),
              std::string::npos);
    EXPECT_NE(sceneSource.find("if (resolvedInstanceMask == 0)"), std::string::npos);
    EXPECT_NE(sceneSource.find("RayTracingSceneSkipReason::InstanceMaskZero"), std::string::npos);
    EXPECT_NE(sceneHeader.find("InvalidTransform"), std::string::npos);
    EXPECT_NE(sceneSource.find("bool IsFiniteTransform(const Mat4& matrix)"), std::string::npos);
    EXPECT_NE(sceneSource.find("if (!IsFiniteTransform(object.worldMatrix))"), std::string::npos);
    EXPECT_NE(sceneSource.find("RayTracingSceneSkipReason::InvalidTransform"), std::string::npos);
    EXPECT_NE(sceneSource.find("instance.desc.instanceMask = resolvedInstanceMask;"), std::string::npos);
    EXPECT_EQ(sceneSource.find("instance.desc.instanceMask = ResolveRayTracingInstanceMask"),
              std::string::npos);

    const auto maskFilter = sceneSource.find("if (resolvedInstanceMask == 0)");
    const auto transformFilter = sceneSource.find("if (!IsFiniteTransform(object.worldMatrix))", maskFilter);
    const auto blasKey = sceneSource.find("RayTracingBLASKey key;", maskFilter);
    ASSERT_NE(maskFilter, std::string::npos);
    ASSERT_NE(transformFilter, std::string::npos);
    ASSERT_NE(blasKey, std::string::npos);
    EXPECT_LT(maskFilter, transformFilter);
    EXPECT_LT(transformFilter, blasKey);
}

TEST_F(PipelineCacheValidationFixture, RayTracingSceneBuildPassTransitionsScratchBuffersAsUAV)
{
    if (!HasCompilerAvailable())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    const fs::path renderRoot = FindShaderDirectory().parent_path();
    const std::string managerHeader =
        ReadTextFile(renderRoot / "Include" / "Render" / "RayTracing" / "RayTracingSceneManager.h");
    const std::string managerSource =
        ReadTextFile(renderRoot / "Private" / "RayTracing" / "RayTracingSceneManager.cpp");
    const std::string sceneRendererSource =
        ReadTextFile(renderRoot / "Private" / "Renderer" / "SceneRenderer.cpp");

    EXPECT_NE(managerHeader.find("void GatherPendingBuildScratchBuffers(std::vector<RHIBuffer*>& outScratchBuffers) const;"),
              std::string::npos);
    EXPECT_NE(managerSource.find("void RayTracingSceneManager::GatherPendingBuildScratchBuffers"),
              std::string::npos);
    EXPECT_NE(managerSource.find("outScratchBuffers.push_back(scratchBuffer);"), std::string::npos);
    EXPECT_NE(sceneRendererSource.find("GatherPendingBuildScratchBuffers(blasScratchBuffers);"),
              std::string::npos);
    EXPECT_NE(sceneRendererSource.find("SetExportState(scratchBufferHandle, RHIResourceState::Common)"),
              std::string::npos);
    EXPECT_NE(sceneRendererSource.find("ImportBuffer(instanceBuffer, RHIResourceState::ShaderResource)"),
              std::string::npos);
    EXPECT_NE(sceneRendererSource.find("SetExportState(instanceHandle, RHIResourceState::ShaderResource)"),
              std::string::npos);
    EXPECT_NE(sceneRendererSource.find("SetExportState(scratchHandle, RHIResourceState::Common)"),
              std::string::npos);
    EXPECT_NE(sceneRendererSource.find("builder.Write(handle, RHIResourceState::UnorderedAccess)"),
              std::string::npos);
    EXPECT_NE(sceneRendererSource.find("builder.Write(scratchHandle, RHIResourceState::UnorderedAccess)"),
              std::string::npos);
    EXPECT_EQ(sceneRendererSource.find("builder.Write(scratchHandle, RHIResourceState::AccelerationStructureBuildWrite)"),
              std::string::npos);
}

TEST_F(PipelineCacheValidationFixture, SceneRendererWiresRayTracedShadowPassAfterRasterShadow)
{
    if (!HasCompilerAvailable())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    const fs::path renderRoot = FindShaderDirectory().parent_path();
    const std::string header = ReadTextFile(renderRoot / "Include" / "Render" / "Renderer" / "SceneRenderer.h");
    const std::string source = ReadTextFile(renderRoot / "Private" / "Renderer" / "SceneRenderer.cpp");

    EXPECT_NE(header.find("#include \"Render/Passes/RayTracedShadowPass.h\""), std::string::npos);
    EXPECT_NE(header.find("#include \"Render/Passes/CameraVelocityPass.h\""), std::string::npos);
    EXPECT_NE(header.find("#include \"Render/Passes/RayTracedReflectionCompositePass.h\""), std::string::npos);
    EXPECT_NE(header.find("#include \"Render/Passes/RayTracedReflectionDenoisePass.h\""), std::string::npos);
    EXPECT_NE(header.find("#include \"Render/Passes/RayTracedReflectionPass.h\""), std::string::npos);
    EXPECT_NE(header.find("const RayTracedShadowPassStats& GetRayTracedShadowStats() const;"), std::string::npos);
    EXPECT_NE(header.find("const CameraVelocityPassStats& GetCameraVelocityStats() const;"), std::string::npos);
    EXPECT_NE(header.find("const RayTracedReflectionPassStats& GetRayTracedReflectionStats() const;"),
              std::string::npos);
    EXPECT_NE(header.find("const RayTracedReflectionDenoisePassStats& GetRayTracedReflectionDenoiseStats() const;"),
              std::string::npos);
    EXPECT_NE(header.find("const RayTracedReflectionCompositePassStats& GetRayTracedReflectionCompositeStats() const;"),
              std::string::npos);
    EXPECT_NE(header.find("struct SceneRayTracingBudgetSettings"), std::string::npos);
    EXPECT_NE(header.find("struct SceneRayTracingFrameStats"), std::string::npos);
    EXPECT_NE(header.find("SceneRayTracingFrameStats GetRayTracingFrameStats() const;"), std::string::npos);
    EXPECT_NE(header.find("void ApplyRayTracingBudgetSettings(const SceneRayTracingBudgetSettings& settings);"),
              std::string::npos);
    EXPECT_NE(header.find("const SceneRayTracingBudgetSettings& GetRayTracingBudgetSettings() const"),
              std::string::npos);
    EXPECT_NE(header.find("bool shadowGpuTimingSupported = false;"), std::string::npos);
    EXPECT_NE(header.find("bool shadowGpuTimingQueriesRecorded = false;"), std::string::npos);
    EXPECT_NE(header.find("bool shadowGpuTimingResolveRecorded = false;"), std::string::npos);
    EXPECT_NE(header.find("bool shadowGpuTimingReadbackBufferAvailable = false;"), std::string::npos);
    EXPECT_NE(header.find("bool shadowGpuTimingResultAvailable = false;"), std::string::npos);
    EXPECT_NE(header.find("bool reflectionGpuTimingSupported = false;"), std::string::npos);
    EXPECT_NE(header.find("bool reflectionGpuTimingQueriesRecorded = false;"), std::string::npos);
    EXPECT_NE(header.find("bool reflectionGpuTimingResolveRecorded = false;"), std::string::npos);
    EXPECT_NE(header.find("bool reflectionGpuTimingReadbackBufferAvailable = false;"), std::string::npos);
    EXPECT_NE(header.find("bool reflectionGpuTimingResultAvailable = false;"), std::string::npos);
    EXPECT_NE(header.find("uint64 shadowGpuTimestampFrequency = 0;"), std::string::npos);
    EXPECT_NE(header.find("uint64 reflectionGpuTimestampFrequency = 0;"), std::string::npos);
    EXPECT_NE(header.find("uint64 shadowGpuTimingReadbackBytes = 0;"), std::string::npos);
    EXPECT_NE(header.find("uint64 reflectionGpuTimingReadbackBytes = 0;"), std::string::npos);
    EXPECT_NE(header.find("uint64 shadowGpuTimingElapsedTicks = 0;"), std::string::npos);
    EXPECT_NE(header.find("uint64 reflectionGpuTimingElapsedTicks = 0;"), std::string::npos);
    EXPECT_NE(header.find("float shadowGpuTimingElapsedMs = 0.0f;"), std::string::npos);
    EXPECT_NE(header.find("float reflectionGpuTimingElapsedMs = 0.0f;"), std::string::npos);
    EXPECT_NE(header.find("float totalMeasuredRayTracingGpuMs = 0.0f;"), std::string::npos);
    EXPECT_NE(header.find("float gpuTimeBudget = 0.0f;"), std::string::npos);
    EXPECT_NE(header.find("float shadowGpuTimeBudget = 0.0f;"), std::string::npos);
    EXPECT_NE(header.find("float reflectionGpuTimeBudget = 0.0f;"), std::string::npos);
    EXPECT_NE(header.find("float measuredGpuTimeForBudgetMs = 0.0f;"), std::string::npos);
    EXPECT_NE(header.find("float measuredShadowGpuTimeForBudgetMs = 0.0f;"), std::string::npos);
    EXPECT_NE(header.find("float measuredReflectionGpuTimeForBudgetMs = 0.0f;"), std::string::npos);
    EXPECT_NE(header.find("float gpuTimeBudgetQualityScale = 1.0f;"), std::string::npos);
    EXPECT_NE(header.find("float shadowGpuTimeBudgetQualityScale = 1.0f;"), std::string::npos);
    EXPECT_NE(header.find("float reflectionGpuTimeBudgetQualityScale = 1.0f;"), std::string::npos);
    EXPECT_NE(header.find("float gpuTimingRecoveryRate = 0.05f;"), std::string::npos);
    EXPECT_NE(header.find("uint32 gpuTimeBudgetOverBudgetFrameCount = 0;"), std::string::npos);
    EXPECT_NE(header.find("uint32 gpuTimeBudgetUnderBudgetFrameCount = 0;"), std::string::npos);
    EXPECT_NE(header.find("uint32 shadowGpuTimeBudgetOverBudgetFrameCount = 0;"), std::string::npos);
    EXPECT_NE(header.find("uint32 shadowGpuTimeBudgetUnderBudgetFrameCount = 0;"), std::string::npos);
    EXPECT_NE(header.find("uint32 reflectionGpuTimeBudgetOverBudgetFrameCount = 0;"), std::string::npos);
    EXPECT_NE(header.find("uint32 reflectionGpuTimeBudgetUnderBudgetFrameCount = 0;"), std::string::npos);
    EXPECT_NE(header.find("uint32 gpuTimeBudgetAdjustmentFrameCount = 0;"), std::string::npos);
    EXPECT_NE(header.find("uint32 shadowGpuTimingReadbackBufferCount = 0;"), std::string::npos);
    EXPECT_NE(header.find("uint32 reflectionGpuTimingReadbackFrameIndex = RVX_INVALID_INDEX;"), std::string::npos);
    EXPECT_NE(header.find("bool budgetEnabled = false;"), std::string::npos);
    EXPECT_NE(header.find("bool budgetApplied = false;"), std::string::npos);
    EXPECT_NE(header.find("bool rayBudgetExceeded = false;"), std::string::npos);
    EXPECT_NE(header.find("bool denoiseTapBudgetExceeded = false;"), std::string::npos);
    EXPECT_NE(header.find("bool resourceBudgetExceeded = false;"), std::string::npos);
    EXPECT_NE(header.find("bool resourceBudgetEvictionAttempted = false;"), std::string::npos);
    EXPECT_NE(header.find("bool resourceByteAccountingOverflowed = false;"), std::string::npos);
    EXPECT_NE(header.find("bool gpuTimeBudgetExceeded = false;"), std::string::npos);
    EXPECT_NE(header.find("bool gpuTimeBudgetApplied = false;"), std::string::npos);
    EXPECT_NE(header.find("bool gpuTimeBudgetQualityScaleAdjusted = false;"), std::string::npos);
    EXPECT_NE(header.find("bool shadowGpuTimeBudgetExceeded = false;"), std::string::npos);
    EXPECT_NE(header.find("bool shadowGpuTimeBudgetApplied = false;"), std::string::npos);
    EXPECT_NE(header.find("bool shadowGpuTimeBudgetQualityScaleAdjusted = false;"), std::string::npos);
    EXPECT_NE(header.find("bool reflectionGpuTimeBudgetExceeded = false;"), std::string::npos);
    EXPECT_NE(header.find("bool reflectionGpuTimeBudgetApplied = false;"), std::string::npos);
    EXPECT_NE(header.find("bool reflectionGpuTimeBudgetQualityScaleAdjusted = false;"), std::string::npos);
    EXPECT_NE(header.find("bool shadowHistoryAvailable = false;"), std::string::npos);
    EXPECT_NE(header.find("bool shadowDepthHistoryAvailable = false;"), std::string::npos);
    EXPECT_NE(header.find("bool shadowNormalHistoryAvailable = false;"), std::string::npos);
    EXPECT_NE(header.find("bool shadowHistoryReset = false;"), std::string::npos);
    EXPECT_NE(header.find("bool shadowHistoryRecreated = false;"), std::string::npos);
    EXPECT_NE(header.find("bool shadowHistoryResolutionChanged = false;"), std::string::npos);
    EXPECT_NE(header.find("bool shadowHistoryConfigChanged = false;"), std::string::npos);
    EXPECT_NE(header.find("bool shadowTemporalAccumulated = false;"), std::string::npos);
    EXPECT_NE(header.find("bool reflectionHistoryAvailable = false;"), std::string::npos);
    EXPECT_NE(header.find("bool reflectionDepthHistoryAvailable = false;"), std::string::npos);
    EXPECT_NE(header.find("bool reflectionNormalHistoryAvailable = false;"), std::string::npos);
    EXPECT_NE(header.find("bool reflectionHistoryReset = false;"), std::string::npos);
    EXPECT_NE(header.find("bool reflectionHistoryRecreated = false;"), std::string::npos);
    EXPECT_NE(header.find("bool reflectionHistoryResolutionChanged = false;"), std::string::npos);
    EXPECT_NE(header.find("bool reflectionHistoryConfigChanged = false;"), std::string::npos);
    EXPECT_NE(header.find("bool reflectionTemporalAccumulated = false;"), std::string::npos);
    EXPECT_NE(header.find("bool measuredGpuTimeAvailable = false;"), std::string::npos);
    EXPECT_NE(header.find("uint64 trackedResourceBudget = 0;"), std::string::npos);
    EXPECT_NE(header.find("uint64 estimatedRayCountBeforeBudget = 0;"), std::string::npos);
    EXPECT_NE(header.find("uint64 estimatedDenoiseTapCountBeforeBudget = 0;"), std::string::npos);
    EXPECT_NE(header.find("uint64 maxTrackedResourceBytes = 0;"), std::string::npos);
    EXPECT_NE(header.find("float maxMeasuredGpuMs = 0.0f;"), std::string::npos);
    EXPECT_NE(header.find("float maxShadowMeasuredGpuMs = 0.0f;"), std::string::npos);
    EXPECT_NE(header.find("float maxReflectionMeasuredGpuMs = 0.0f;"), std::string::npos);
    EXPECT_NE(header.find("float gpuTimingHysteresis = 0.15f;"), std::string::npos);
    EXPECT_NE(header.find("float gpuTimingRecoveryRate = 0.05f;"), std::string::npos);
    EXPECT_NE(header.find("uint32 gpuTimingAdjustmentFrameCount = 2;"), std::string::npos);
    EXPECT_NE(header.find("float minReflectionResolutionScale = 0.25f;"), std::string::npos);
    EXPECT_NE(header.find("void ApplyRayTracingBudget(ShadowPassConfig& shadowConfig,"), std::string::npos);
    EXPECT_NE(header.find("SceneRayTracingBudgetSettings m_rayTracingBudgetSettings;"), std::string::npos);
    EXPECT_NE(header.find("SceneRayTracingFrameStats m_rayTracingFrameBudgetStats;"), std::string::npos);
    EXPECT_NE(header.find("float m_rayTracingGpuBudgetQualityScale = 1.0f;"), std::string::npos);
    EXPECT_NE(header.find("float m_rayTracingShadowGpuBudgetQualityScale = 1.0f;"), std::string::npos);
    EXPECT_NE(header.find("float m_rayTracingReflectionGpuBudgetQualityScale = 1.0f;"), std::string::npos);
    EXPECT_NE(header.find("float m_rayTracingLastShadowMeasuredGpuMs = 0.0f;"), std::string::npos);
    EXPECT_NE(header.find("float m_rayTracingLastReflectionMeasuredGpuMs = 0.0f;"), std::string::npos);
    EXPECT_NE(header.find("bool m_rayTracingLastMeasuredGpuMsValid = false;"), std::string::npos);
    EXPECT_NE(header.find("bool m_rayTracingLastShadowMeasuredGpuMsValid = false;"), std::string::npos);
    EXPECT_NE(header.find("bool m_rayTracingLastReflectionMeasuredGpuMsValid = false;"), std::string::npos);
    EXPECT_NE(header.find("uint64 m_rayTracingGpuBudgetLastShadowEndTimestamp = 0;"), std::string::npos);
    EXPECT_NE(header.find("uint32 m_rayTracingGpuBudgetOverBudgetFrameCount = 0;"), std::string::npos);
    EXPECT_NE(header.find("uint32 m_rayTracingGpuBudgetUnderBudgetFrameCount = 0;"), std::string::npos);
    EXPECT_NE(header.find("uint32 m_rayTracingShadowGpuBudgetOverBudgetFrameCount = 0;"), std::string::npos);
    EXPECT_NE(header.find("uint32 m_rayTracingShadowGpuBudgetUnderBudgetFrameCount = 0;"), std::string::npos);
    EXPECT_NE(header.find("uint32 m_rayTracingReflectionGpuBudgetOverBudgetFrameCount = 0;"), std::string::npos);
    EXPECT_NE(header.find("uint32 m_rayTracingReflectionGpuBudgetUnderBudgetFrameCount = 0;"), std::string::npos);
    EXPECT_NE(header.find("bool sceneSupported = false;"), std::string::npos);
    EXPECT_NE(header.find("bool shadowSupported = false;"), std::string::npos);
    EXPECT_NE(header.find("bool reflectionSupported = false;"), std::string::npos);
    EXPECT_NE(header.find("bool reflectionDenoiseSupported = false;"), std::string::npos);
    EXPECT_NE(header.find("bool reflectionCompositeSupported = false;"), std::string::npos);
    EXPECT_NE(header.find("uint64 estimatedShadowRayCount = 0;"), std::string::npos);
    EXPECT_NE(header.find("uint64 estimatedReflectionRayCount = 0;"), std::string::npos);
    EXPECT_NE(header.find("uint64 estimatedTotalRayCount = 0;"), std::string::npos);
    EXPECT_NE(header.find("uint64 estimatedReflectionDenoiseTapCount = 0;"), std::string::npos);
    EXPECT_NE(header.find("size_t cachedBLASCount = 0;"), std::string::npos);
    EXPECT_NE(header.find("size_t evictedBLASCount = 0;"), std::string::npos);
    EXPECT_NE(header.find("size_t resourceBudgetEvictedBLASCount = 0;"), std::string::npos);
    EXPECT_NE(header.find("size_t releasedBLASScratchCount = 0;"), std::string::npos);
    EXPECT_NE(header.find("size_t pendingBLASScratchReleaseCount = 0;"), std::string::npos);
    EXPECT_NE(header.find("uint64 cachedBLASAccelerationStructureBytes = 0;"), std::string::npos);
    EXPECT_NE(header.find("uint64 cachedBLASScratchBytes = 0;"), std::string::npos);
    EXPECT_NE(header.find("uint64 releasedBLASScratchBytes = 0;"), std::string::npos);
    EXPECT_NE(header.find("uint64 topLevelAccelerationStructureBytes = 0;"), std::string::npos);
    EXPECT_NE(header.find("uint64 topLevelScratchBytes = 0;"), std::string::npos);
    EXPECT_NE(header.find("uint64 instanceBufferBytes = 0;"), std::string::npos);
    EXPECT_NE(header.find("uint64 materialMetadataBufferBytes = 0;"), std::string::npos);
    EXPECT_NE(header.find("uint64 alphaMetadataBufferBytes = 0;"), std::string::npos);
    EXPECT_NE(header.find("uint64 totalTrackedResourceBytes = 0;"), std::string::npos);
    EXPECT_NE(header.find("uint32 reflectionDenoiseKernelTapCount = 0;"), std::string::npos);
    EXPECT_NE(header.find("RayTracedShadowPass* m_rayTracedShadowPass = nullptr;"), std::string::npos);
    EXPECT_NE(header.find("CameraVelocityPass* m_cameraVelocityPass = nullptr;"), std::string::npos);
    EXPECT_NE(header.find("Mat4 m_previousViewProjectionMatrix = Mat4Identity();"), std::string::npos);
    EXPECT_NE(header.find("bool m_previousViewProjectionValid = false;"), std::string::npos);
    EXPECT_NE(header.find("RayTracedReflectionPass* m_rayTracedReflectionPass = nullptr;"), std::string::npos);
    EXPECT_NE(header.find("RayTracedReflectionDenoisePass* m_rayTracedReflectionDenoisePass = nullptr;"),
              std::string::npos);
    EXPECT_NE(header.find("RayTracedReflectionCompositePass* m_rayTracedReflectionCompositePass = nullptr;"),
              std::string::npos);

    EXPECT_NE(source.find("#include \"Render/Passes/CameraVelocityPass.h\""), std::string::npos);
    EXPECT_NE(source.find("#include \"Render/Passes/RayTracedShadowPass.h\""), std::string::npos);
    EXPECT_NE(source.find("#include \"Render/Passes/RayTracedReflectionCompositePass.h\""), std::string::npos);
    EXPECT_NE(source.find("#include \"Render/Passes/RayTracedReflectionDenoisePass.h\""), std::string::npos);
    EXPECT_NE(source.find("#include \"Render/Passes/RayTracedReflectionPass.h\""), std::string::npos);
    EXPECT_NE(source.find("SceneRayTracingFrameStats SceneRenderer::GetRayTracingFrameStats() const"),
              std::string::npos);
    EXPECT_NE(source.find("const RayTracedShadowPassStats& shadowStats = GetRayTracedShadowStats();"),
              std::string::npos);
    EXPECT_NE(source.find("const RayTracedReflectionPassStats& reflectionStats = GetRayTracedReflectionStats();"),
              std::string::npos);
    EXPECT_NE(source.find("const RayTracedReflectionDenoisePassStats& denoiseStats = GetRayTracedReflectionDenoiseStats();"),
              std::string::npos);
    EXPECT_NE(source.find("const RayTracedReflectionCompositePassStats& compositeStats = GetRayTracedReflectionCompositeStats();"),
              std::string::npos);
    EXPECT_NE(source.find("SceneRayTracingFrameStats stats = m_rayTracingFrameBudgetStats;"), std::string::npos);
    EXPECT_NE(source.find("void SceneRenderer::ApplyRayTracingBudgetSettings(const SceneRayTracingBudgetSettings& settings)"),
              std::string::npos);
    EXPECT_NE(source.find("void SceneRenderer::ApplyRayTracingBudget(ShadowPassConfig& shadowConfig,"),
              std::string::npos);
    EXPECT_NE(source.find("ApplyRayTracingBudget(effectiveShadowConfig,"), std::string::npos);
    EXPECT_NE(source.find("m_rayTracedShadowPass->SetConfig(effectiveShadowConfig);"), std::string::npos);
    EXPECT_NE(source.find("budgetStats.estimatedRayCountBeforeBudget"), std::string::npos);
    EXPECT_NE(source.find("budgetStats.estimatedDenoiseTapCountBeforeBudget"), std::string::npos);
    EXPECT_NE(source.find("budgetStats.trackedResourceBudget = m_rayTracingBudgetSettings.maxTrackedResourceBytes;"),
              std::string::npos);
    EXPECT_NE(source.find("budgetStats.resourceBudgetExceeded ="), std::string::npos);
    EXPECT_NE(source.find("budgetStats.resourceByteAccountingOverflowed = m_rayTracingSceneStats.resourceByteAccountingOverflowed;"),
              std::string::npos);
    EXPECT_NE(source.find("m_rayTracingSceneStats.resourceBudgetExceeded;"), std::string::npos);
    EXPECT_EQ(source.find("budgetStats.resourceByteAccountingOverflowed ||"), std::string::npos);
    EXPECT_NE(source.find("budgetStats.resourceBudgetEvictedBLASCount = m_rayTracingSceneStats.resourceBudgetEvictedBLASCount;"),
              std::string::npos);
    EXPECT_NE(source.find("budgetStats.releasedBLASScratchCount = m_rayTracingSceneStats.releasedBLASScratchCount;"),
              std::string::npos);
    EXPECT_NE(source.find("budgetStats.pendingBLASScratchReleaseCount = m_rayTracingSceneStats.pendingBLASScratchReleaseCount;"),
              std::string::npos);
    EXPECT_NE(source.find("budgetStats.releasedBLASScratchBytes = m_rayTracingSceneStats.releasedBLASScratchBytes;"),
              std::string::npos);
    EXPECT_NE(source.find("budgetStats.resourceBudgetEvictionAttempted = m_rayTracingSceneStats.resourceBudgetEvictionAttempted;"),
              std::string::npos);
    EXPECT_NE(source.find("uint32 ApplyRayTracingGpuBudgetScale"), std::string::npos);
    EXPECT_NE(source.find("m_rayTracingBudgetSettings.maxMeasuredGpuMs ="), std::string::npos);
    EXPECT_NE(source.find("m_rayTracingBudgetSettings.maxShadowMeasuredGpuMs ="), std::string::npos);
    EXPECT_NE(source.find("m_rayTracingBudgetSettings.maxReflectionMeasuredGpuMs ="), std::string::npos);
    EXPECT_NE(source.find("m_rayTracingBudgetSettings.maxTrackedResourceBytes = settings.maxTrackedResourceBytes;"),
              std::string::npos);
    EXPECT_NE(source.find("m_rayTracingBudgetSettings.gpuTimingRecoveryRate ="), std::string::npos);
    EXPECT_NE(source.find("m_rayTracingBudgetSettings.gpuTimingAdjustmentFrameCount ="), std::string::npos);
    EXPECT_NE(source.find("m_rayTracingBudgetSettings.blasCacheEvictionFrameThreshold ="), std::string::npos);
    EXPECT_NE(source.find("const bool hasNewGpuTimingSample ="), std::string::npos);
    EXPECT_NE(source.find("const bool hasNewShadowGpuTimingSample ="), std::string::npos);
    EXPECT_NE(source.find("const bool hasNewReflectionGpuTimingSample ="), std::string::npos);
    EXPECT_NE(source.find("const auto updateGpuBudgetFeedback ="), std::string::npos);
    EXPECT_NE(source.find("m_rayTracingGpuBudgetLastShadowEndTimestamp"), std::string::npos);
    EXPECT_NE(source.find("overBudgetFrameCount >= adjustmentFrameCount"), std::string::npos);
    EXPECT_NE(source.find("underBudgetFrameCount >= adjustmentFrameCount"), std::string::npos);
    EXPECT_NE(source.find("qualityScale = std::clamp("), std::string::npos);
    EXPECT_NE(source.find("qualityScale * (1.0f + m_rayTracingBudgetSettings.gpuTimingRecoveryRate)"),
              std::string::npos);
    EXPECT_NE(source.find("budgetStats.gpuTimeBudgetQualityScaleAdjusted"), std::string::npos);
    EXPECT_NE(source.find("updateGpuBudgetFeedback(m_rayTracingBudgetSettings.maxShadowMeasuredGpuMs,"),
              std::string::npos);
    EXPECT_NE(source.find("updateGpuBudgetFeedback(m_rayTracingBudgetSettings.maxReflectionMeasuredGpuMs,"),
              std::string::npos);
    EXPECT_NE(source.find("budgetStats.gpuTimeBudgetOverBudgetFrameCount = m_rayTracingGpuBudgetOverBudgetFrameCount;"),
              std::string::npos);
    EXPECT_NE(source.find("budgetStats.shadowGpuTimeBudgetOverBudgetFrameCount = m_rayTracingShadowGpuBudgetOverBudgetFrameCount;"),
              std::string::npos);
    EXPECT_NE(source.find("budgetStats.reflectionGpuTimeBudgetUnderBudgetFrameCount = m_rayTracingReflectionGpuBudgetUnderBudgetFrameCount;"),
              std::string::npos);
    EXPECT_NE(source.find("budgetStats.gpuTimeBudgetApplied ="), std::string::npos);
    EXPECT_NE(source.find("budgetStats.gpuTimeBudgetQualityScale = m_rayTracingGpuBudgetQualityScale;"),
              std::string::npos);
    EXPECT_NE(source.find("budgetStats.shadowGpuTimeBudgetApplied = true;"), std::string::npos);
    EXPECT_NE(source.find("budgetStats.reflectionGpuTimeBudgetApplied = true;"), std::string::npos);
    EXPECT_NE(source.find("std::min(m_rayTracingGpuBudgetQualityScale, m_rayTracingShadowGpuBudgetQualityScale)"),
              std::string::npos);
    EXPECT_NE(source.find("std::min(m_rayTracingGpuBudgetQualityScale, m_rayTracingReflectionGpuBudgetQualityScale)"),
              std::string::npos);
    EXPECT_NE(source.find("budgetStats.shadowGpuTimeBudgetQualityScale = m_rayTracingShadowGpuBudgetQualityScale;"),
              std::string::npos);
    EXPECT_NE(source.find("budgetStats.reflectionGpuTimeBudgetQualityScale = m_rayTracingReflectionGpuBudgetQualityScale;"),
              std::string::npos);
    EXPECT_NE(source.find("budgetStats.budgetApplied = true;"), std::string::npos);
    EXPECT_NE(source.find("float ReduceReflectionResolutionScaleToFitRayBudget(uint32 width,"), std::string::npos);
    EXPECT_NE(source.find("std::nextafter(static_cast<float>(scaledWidth - 1u) / static_cast<float>(width), 0.0f)"),
              std::string::npos);
    EXPECT_NE(source.find("appliedReflectionScale = ReduceReflectionResolutionScaleToFitRayBudget("),
              std::string::npos);
    EXPECT_NE(source.find("budgetStats.rayBudgetExceeded = estimatedRays > m_rayTracingBudgetSettings.maxRayCount;"),
              std::string::npos);
    EXPECT_NE(source.find("budgetStats.denoiseTapBudgetExceeded ="), std::string::npos);
    EXPECT_NE(source.find("budgetStats.gpuTimeBudgetExceeded ="), std::string::npos);
    EXPECT_NE(source.find("budgetStats.shadowGpuTimeBudgetExceeded ="), std::string::npos);
    EXPECT_NE(source.find("budgetStats.reflectionGpuTimeBudgetExceeded ="), std::string::npos);
    EXPECT_NE(source.find("appliedReflectionScale = std::max(m_rayTracingBudgetSettings.minReflectionResolutionScale,"),
              std::string::npos);
    EXPECT_NE(source.find("reflectionConfig.resolutionScale = appliedReflectionScale;"), std::string::npos);
    EXPECT_NE(source.find("denoiseConfig.radius = appliedDenoiseRadius;"), std::string::npos);
    EXPECT_NE(source.find("SceneRayTracingFrameStats stats = m_rayTracingFrameBudgetStats;"), std::string::npos);
    EXPECT_NE(source.find("stats.sceneSupported = m_rayTracingSceneStats.supported;"), std::string::npos);
    EXPECT_NE(source.find("stats.scenePrepared = m_rayTracingSceneStats.prepared;"), std::string::npos);
    EXPECT_NE(source.find("stats.tlasAvailable = m_rayTracingSceneStats.hasTopLevelAS || GetRayTracingTopLevelAS() != nullptr;"),
              std::string::npos);
    EXPECT_NE(source.find("stats.shadowSupported = shadowStats.supported;"), std::string::npos);
    EXPECT_NE(source.find("stats.reflectionSupported = reflectionStats.supported;"), std::string::npos);
    EXPECT_NE(source.find("stats.reflectionDenoiseSupported = denoiseStats.supported;"), std::string::npos);
    EXPECT_NE(source.find("stats.reflectionCompositeSupported = compositeStats.supported;"), std::string::npos);
    EXPECT_NE(source.find("stats.shadowHistoryAvailable = shadowStats.historyAvailable;"), std::string::npos);
    EXPECT_NE(source.find("stats.shadowDepthHistoryAvailable = shadowStats.depthHistoryAvailable;"),
              std::string::npos);
    EXPECT_NE(source.find("stats.shadowHistoryRecreated = shadowStats.historyRecreated;"), std::string::npos);
    EXPECT_NE(source.find("stats.shadowHistoryResolutionChanged = shadowStats.historyResolutionChanged;"),
              std::string::npos);
    EXPECT_NE(source.find("stats.shadowHistoryConfigChanged = shadowStats.historyConfigChanged;"),
              std::string::npos);
    EXPECT_NE(source.find("stats.shadowTemporalAccumulated = shadowStats.temporalAccumulated;"),
              std::string::npos);
    EXPECT_NE(source.find("stats.reflectionHistoryAvailable = reflectionStats.historyAvailable;"),
              std::string::npos);
    EXPECT_NE(source.find("stats.reflectionDepthHistoryAvailable = reflectionStats.depthHistoryAvailable;"),
              std::string::npos);
    EXPECT_NE(source.find("stats.reflectionHistoryRecreated = reflectionStats.historyRecreated;"),
              std::string::npos);
    EXPECT_NE(source.find("stats.reflectionHistoryResolutionChanged = reflectionStats.historyResolutionChanged;"),
              std::string::npos);
    EXPECT_NE(source.find("stats.reflectionHistoryConfigChanged = reflectionStats.historyConfigChanged;"),
              std::string::npos);
    EXPECT_NE(source.find("stats.reflectionTemporalAccumulated = reflectionStats.temporalAccumulated;"),
              std::string::npos);
    EXPECT_NE(source.find("stats.shadowGpuTimingSupported = shadowStats.gpuTimingSupported;"), std::string::npos);
    EXPECT_NE(source.find("stats.shadowGpuTimingQueriesRecorded = shadowStats.gpuTimingQueriesRecorded;"),
              std::string::npos);
    EXPECT_NE(source.find("stats.shadowGpuTimingResolveRecorded = shadowStats.gpuTimingResolveRecorded;"),
              std::string::npos);
    EXPECT_NE(source.find("stats.shadowGpuTimingReadbackBufferAvailable = shadowStats.gpuTimingReadbackBufferAvailable;"),
              std::string::npos);
    EXPECT_NE(source.find("stats.shadowGpuTimingResultAvailable = shadowStats.gpuTimingResultAvailable;"),
              std::string::npos);
    EXPECT_NE(source.find("stats.reflectionGpuTimingSupported = reflectionStats.gpuTimingSupported;"),
              std::string::npos);
    EXPECT_NE(source.find("stats.reflectionGpuTimingQueriesRecorded = reflectionStats.gpuTimingQueriesRecorded;"),
              std::string::npos);
    EXPECT_NE(source.find("stats.reflectionGpuTimingResolveRecorded = reflectionStats.gpuTimingResolveRecorded;"),
              std::string::npos);
    EXPECT_NE(source.find("stats.reflectionGpuTimingReadbackBufferAvailable = reflectionStats.gpuTimingReadbackBufferAvailable;"),
              std::string::npos);
    EXPECT_NE(source.find("stats.reflectionGpuTimingResultAvailable = reflectionStats.gpuTimingResultAvailable;"),
              std::string::npos);
    EXPECT_NE(source.find("stats.shadowGpuTimestampFrequency = shadowStats.gpuTimestampFrequency;"),
              std::string::npos);
    EXPECT_NE(source.find("stats.reflectionGpuTimestampFrequency = reflectionStats.gpuTimestampFrequency;"),
              std::string::npos);
    EXPECT_NE(source.find("stats.shadowGpuTimingReadbackBytes = shadowStats.gpuTimingReadbackBytes;"),
              std::string::npos);
    EXPECT_NE(source.find("stats.reflectionGpuTimingReadbackBytes = reflectionStats.gpuTimingReadbackBytes;"),
              std::string::npos);
    EXPECT_NE(source.find("stats.shadowGpuTimingElapsedMs = shadowStats.gpuTimingElapsedMs;"),
              std::string::npos);
    EXPECT_NE(source.find("stats.reflectionGpuTimingElapsedMs = reflectionStats.gpuTimingElapsedMs;"),
              std::string::npos);
    EXPECT_NE(source.find("stats.totalMeasuredRayTracingGpuMs ="),
              std::string::npos);
    EXPECT_NE(source.find("stats.shadowGpuTimingReadbackBufferCount = shadowStats.gpuTimingReadbackBufferCount;"),
              std::string::npos);
    EXPECT_NE(source.find("stats.reflectionGpuTimingReadbackFrameIndex = reflectionStats.gpuTimingReadbackFrameIndex;"),
              std::string::npos);
    EXPECT_NE(source.find("stats.estimatedTotalRayCount = stats.estimatedShadowRayCount + stats.estimatedReflectionRayCount;"),
              std::string::npos);
    EXPECT_NE(source.find("stats.estimatedReflectionDenoiseTapCount = denoiseStats.estimatedTapCount;"),
              std::string::npos);
    EXPECT_NE(source.find("stats.trackedResourceBudget = m_rayTracingBudgetSettings.maxTrackedResourceBytes;"),
              std::string::npos);
    EXPECT_NE(source.find("stats.blasCacheEvictionFrameThreshold = m_rayTracingBudgetSettings.blasCacheEvictionFrameThreshold;"),
              std::string::npos);
    EXPECT_NE(source.find("stats.cachedBLASCount = m_rayTracingSceneStats.cachedBLASCount;"),
              std::string::npos);
    EXPECT_NE(source.find("stats.evictedBLASCount = m_rayTracingSceneStats.evictedBLASCount;"),
              std::string::npos);
    EXPECT_NE(source.find("stats.resourceBudgetEvictedBLASCount = m_rayTracingSceneStats.resourceBudgetEvictedBLASCount;"),
              std::string::npos);
    EXPECT_NE(source.find("stats.releasedBLASScratchCount = m_rayTracingSceneStats.releasedBLASScratchCount;"),
              std::string::npos);
    EXPECT_NE(source.find("stats.pendingBLASScratchReleaseCount = m_rayTracingSceneStats.pendingBLASScratchReleaseCount;"),
              std::string::npos);
    EXPECT_NE(source.find("stats.cachedBLASAccelerationStructureBytes = m_rayTracingSceneStats.cachedBLASAccelerationStructureBytes;"),
              std::string::npos);
    EXPECT_NE(source.find("stats.cachedBLASScratchBytes = m_rayTracingSceneStats.cachedBLASScratchBytes;"),
              std::string::npos);
    EXPECT_NE(source.find("stats.releasedBLASScratchBytes = m_rayTracingSceneStats.releasedBLASScratchBytes;"),
              std::string::npos);
    EXPECT_NE(source.find("stats.topLevelAccelerationStructureBytes = m_rayTracingSceneStats.topLevelAccelerationStructureBytes;"),
              std::string::npos);
    EXPECT_NE(source.find("stats.topLevelScratchBytes = m_rayTracingSceneStats.topLevelScratchBytes;"),
              std::string::npos);
    EXPECT_NE(source.find("stats.instanceBufferBytes = m_rayTracingSceneStats.instanceBufferBytes;"),
              std::string::npos);
    EXPECT_NE(source.find("stats.materialMetadataBufferBytes = m_rayTracingSceneStats.materialMetadataBufferBytes;"),
              std::string::npos);
    EXPECT_NE(source.find("stats.alphaMetadataBufferBytes = m_rayTracingSceneStats.alphaMetadataBufferBytes;"),
              std::string::npos);
    EXPECT_NE(source.find("stats.totalTrackedResourceBytes = m_rayTracingSceneStats.totalTrackedResourceBytes;"),
              std::string::npos);
    EXPECT_NE(source.find("stats.resourceBudgetExceeded ="), std::string::npos);
    EXPECT_NE(source.find("stats.resourceByteAccountingOverflowed = m_rayTracingSceneStats.resourceByteAccountingOverflowed;"),
              std::string::npos);
    EXPECT_NE(source.find("m_rayTracingSceneStats.resourceBudgetExceeded;"), std::string::npos);
    EXPECT_EQ(source.find("stats.resourceByteAccountingOverflowed ||"), std::string::npos);
    EXPECT_NE(source.find("stats.resourceBudgetEvictionAttempted = m_rayTracingSceneStats.resourceBudgetEvictionAttempted;"),
              std::string::npos);
    EXPECT_NE(source.find("stats.shadowSamplesPerPixel = shadowStats.requested ? shadowStats.samplesPerPixel : stats.shadowSamplesPerPixel;"),
              std::string::npos);
    EXPECT_NE(source.find("stats.reflectionSamplesPerPixel = reflectionStats.requested"),
              std::string::npos);
    EXPECT_NE(source.find("stats.reflectionDenoiseKernelTapCount = denoiseStats.requested"),
              std::string::npos);
    EXPECT_NE(source.find("stats.denoiseFallbackToRaw = compositeStats.denoiseFallbackToRaw;"),
              std::string::npos);
    EXPECT_NE(source.find("std::make_unique<RayTracedShadowPass>()"), std::string::npos);
    EXPECT_NE(source.find("std::make_unique<CameraVelocityPass>()"), std::string::npos);
    EXPECT_NE(source.find("cameraVelocityPass->SetResources(m_pipelineCache.get(), m_resourceViewCache.get());"),
              std::string::npos);
    EXPECT_NE(source.find("std::make_unique<RayTracedReflectionPass>()"), std::string::npos);
    EXPECT_NE(source.find("std::make_unique<RayTracedReflectionDenoisePass>()"), std::string::npos);
    EXPECT_NE(source.find("std::make_unique<RayTracedReflectionCompositePass>()"), std::string::npos);
    EXPECT_NE(source.find("rayTracedShadowPass->SetResources("), std::string::npos);
    EXPECT_NE(source.find("rayTracedReflectionPass->SetResources("), std::string::npos);
    EXPECT_NE(source.find("rayTracedReflectionDenoisePass->SetResources("), std::string::npos);
    EXPECT_NE(source.find("rayTracedReflectionCompositePass->SetResources("), std::string::npos);
    EXPECT_NE(source.find("m_gpuResourceManager.get(),"), std::string::npos);
    EXPECT_NE(source.find("m_pipelineCache.get(),"), std::string::npos);
    EXPECT_NE(source.find("m_resourceViewCache.get());"),
              std::string::npos);
    EXPECT_NE(source.find("rayTracedShadowPass->SetRayTracingScene(m_rayTracingSceneManager.get());"), std::string::npos);
    EXPECT_NE(source.find("rayTracedReflectionPass->SetRayTracingScene(m_rayTracingSceneManager.get());"),
              std::string::npos);
    EXPECT_NE(source.find("m_rayTracedShadowPass->SetRayTracingScene(m_rayTracingSceneManager.get());"),
              std::string::npos);
    EXPECT_NE(source.find("m_rayTracedReflectionPass->SetRayTracingScene(m_rayTracingSceneManager.get());"),
              std::string::npos);
    EXPECT_NE(source.find("rayTracedReflectionCompositePass->SetReflectionSource(m_rayTracedReflectionPass);"),
              std::string::npos);
    EXPECT_NE(source.find("rayTracedReflectionCompositePass->SetDenoisedReflectionSource(m_rayTracedReflectionDenoisePass);"),
              std::string::npos);
    EXPECT_NE(source.find("m_rayTracedReflectionDenoisePass->SetReflectionSource(m_rayTracedReflectionPass);"),
              std::string::npos);
    EXPECT_NE(source.find("m_rayTracedReflectionCompositePass->SetReflectionSource(m_rayTracedReflectionPass);"),
              std::string::npos);
    EXPECT_NE(source.find("m_rayTracedReflectionCompositePass->SetDenoisedReflectionSource(m_rayTracedReflectionDenoisePass);"),
              std::string::npos);
    EXPECT_NE(source.find("m_rayTracedShadowPass->SetEnabled(false);"), std::string::npos);
    EXPECT_NE(source.find("m_rayTracedShadowPass->SetEnabled(true);"), std::string::npos);
    EXPECT_NE(source.find("m_cameraVelocityPass->SetEnabled(false);"), std::string::npos);
    EXPECT_NE(source.find("const bool cameraVelocityRequested = rayTracedReflectionsRequested && m_postProcessSettings.enableTAA;"),
              std::string::npos);
    EXPECT_NE(source.find("m_cameraVelocityPass->SetEnabled(cameraVelocityRequested);"), std::string::npos);
    EXPECT_NE(source.find("velocityDesc.debugName = \"SceneCameraVelocity\";"), std::string::npos);
    EXPECT_NE(source.find("m_viewData.velocityTarget = m_renderGraph->CreateTexture(velocityDesc);"),
              std::string::npos);
    EXPECT_NE(source.find("void SceneRenderer::RequestTemporalHistoryReset()"), std::string::npos);
    EXPECT_NE(source.find("m_pendingTemporalHistoryReset = true;"), std::string::npos);
    EXPECT_NE(source.find("m_viewData.resetTemporalHistory = resetTemporalHistory;"), std::string::npos);
    EXPECT_NE(source.find("m_viewData.previousViewProjectionMatrix = (!resetTemporalHistory && m_previousViewProjectionValid)"),
              std::string::npos);
    EXPECT_NE(source.find("m_previousViewProjectionValid = false;"), std::string::npos);
    EXPECT_NE(source.find("m_previousViewProjectionMatrix = m_viewData.viewProjectionMatrix;"),
              std::string::npos);
    EXPECT_NE(source.find("m_rayTracedReflectionPass->SetEnabled(false);"), std::string::npos);
    EXPECT_NE(source.find("m_rayTracedReflectionDenoisePass->SetEnabled(false);"), std::string::npos);
    EXPECT_NE(source.find("m_rayTracedReflectionDenoisePass->SetEnabled("), std::string::npos);
    EXPECT_NE(source.find("m_postProcessSettings.enableRayTracedReflectionDenoise"), std::string::npos);
    EXPECT_NE(source.find("m_rayTracedReflectionCompositePass->SetEnabled(false);"), std::string::npos);
    EXPECT_NE(source.find("RayTracedReflectionPassConfig reflectionConfig;"), std::string::npos);
    EXPECT_NE(source.find("RayTracedReflectionDenoisePassConfig denoiseConfig;"), std::string::npos);
    EXPECT_NE(source.find("reflectionConfig.intensity ="), std::string::npos);
    EXPECT_NE(source.find("m_postProcessSettings.rayTracedReflectionIntensity"), std::string::npos);
    EXPECT_NE(source.find("reflectionConfig.resolutionScale ="), std::string::npos);
    EXPECT_NE(source.find("m_postProcessSettings.rayTracedReflectionResolutionScale"), std::string::npos);
    EXPECT_NE(source.find("reflectionConfig.maxRoughness ="), std::string::npos);
    EXPECT_NE(source.find("m_postProcessSettings.rayTracedReflectionMaxRoughness"), std::string::npos);
    EXPECT_NE(source.find("reflectionConfig.maxTraceDistance ="),
              std::string::npos);
    EXPECT_NE(source.find("m_postProcessSettings.rayTracedReflectionMaxDistance"),
              std::string::npos);
    EXPECT_NE(source.find("reflectionConfig.distanceFadeStart ="),
              std::string::npos);
    EXPECT_NE(source.find("m_postProcessSettings.rayTracedReflectionDistanceFadeStart"),
              std::string::npos);
    EXPECT_NE(source.find("reflectionConfig.instanceMask ="),
              std::string::npos);
    EXPECT_NE(source.find("m_postProcessSettings.rayTracedReflectionInstanceMask"), std::string::npos);
    EXPECT_NE(source.find("reflectionConfig.samplesPerPixel ="), std::string::npos);
    EXPECT_NE(source.find("m_postProcessSettings.rayTracedReflectionSamplesPerPixel"), std::string::npos);
    EXPECT_NE(source.find("reflectionConfig.roughnessConeSpread ="), std::string::npos);
    EXPECT_NE(source.find("m_postProcessSettings.rayTracedReflectionRoughnessConeSpread"), std::string::npos);
    EXPECT_NE(source.find("reflectionConfig.normalBias ="), std::string::npos);
    EXPECT_NE(source.find("m_postProcessSettings.rayTracedReflectionNormalBias"), std::string::npos);
    EXPECT_NE(source.find("reflectionConfig.rayMinT ="), std::string::npos);
    EXPECT_NE(source.find("m_postProcessSettings.rayTracedReflectionRayMinT"), std::string::npos);
    EXPECT_NE(source.find("reflectionConfig.fireflyClamp ="), std::string::npos);
    EXPECT_NE(source.find("m_postProcessSettings.rayTracedReflectionFireflyClamp"), std::string::npos);
    EXPECT_NE(source.find("reflectionConfig.temporalBlendFactor ="), std::string::npos);
    EXPECT_NE(source.find("m_postProcessSettings.rayTracedReflectionTemporalBlendFactor"), std::string::npos);
    EXPECT_NE(source.find("reflectionConfig.historyDepthThreshold ="), std::string::npos);
    EXPECT_NE(source.find("m_postProcessSettings.rayTracedReflectionHistoryDepthThreshold"), std::string::npos);
    EXPECT_NE(source.find("reflectionConfig.historyNormalThreshold ="), std::string::npos);
    EXPECT_NE(source.find("m_postProcessSettings.rayTracedReflectionHistoryNormalThreshold"), std::string::npos);
    EXPECT_NE(source.find("reflectionConfig.historyLuminanceTolerance ="), std::string::npos);
    EXPECT_NE(source.find("m_postProcessSettings.rayTracedReflectionHistoryLuminanceTolerance"), std::string::npos);
    EXPECT_NE(source.find("reflectionConfig.historyConfidenceThreshold ="), std::string::npos);
    EXPECT_NE(source.find("m_postProcessSettings.rayTracedReflectionHistoryConfidenceThreshold"), std::string::npos);
    EXPECT_NE(source.find("reflectionConfig.historyVelocityRejectionScale ="), std::string::npos);
    EXPECT_NE(source.find("m_postProcessSettings.rayTracedReflectionHistoryVelocityRejectionScale"), std::string::npos);
    EXPECT_NE(source.find("m_postProcessSettings.rayTracedReflectionDenoiseRadius"), std::string::npos);
    EXPECT_NE(source.find("m_postProcessSettings.rayTracedReflectionDenoiseDepthSigma"), std::string::npos);
    EXPECT_NE(source.find("m_postProcessSettings.rayTracedReflectionDenoiseNormalThreshold"), std::string::npos);
    EXPECT_NE(source.find("m_postProcessSettings.rayTracedReflectionDenoiseConfidencePower"), std::string::npos);
    EXPECT_NE(source.find("m_postProcessSettings.rayTracedReflectionDenoiseCenterWeight"), std::string::npos);
    EXPECT_NE(source.find("m_postProcessSettings.rayTracedReflectionDenoiseLowConfidenceDepthScale"), std::string::npos);
    EXPECT_NE(source.find("m_rayTracedReflectionPass->SetConfig(reflectionConfig);"), std::string::npos);
    EXPECT_NE(source.find("m_rayTracedReflectionDenoisePass->SetConfig(denoiseConfig);"), std::string::npos);
    EXPECT_NE(source.find("denoiseConfig.centerWeight ="), std::string::npos);
    EXPECT_NE(source.find("denoiseConfig.lowConfidenceDepthScale ="), std::string::npos);
    EXPECT_NE(source.find("m_rayTracedReflectionPass->SetEnabled(true);"), std::string::npos);
    EXPECT_NE(source.find("reflectionConfig.temporalAccumulation = m_postProcessSettings.enableTAA;"),
              std::string::npos);
    EXPECT_NE(source.find("m_rayTracedReflectionCompositePass->SetEnabled(true);"), std::string::npos);
    EXPECT_NE(source.find("m_postProcessSettings.enableRayTracedReflections"), std::string::npos);

    const auto setupStart = source.find("void SceneRenderer::SetupDefaultPasses()");
    ASSERT_NE(setupStart, std::string::npos);
    const auto shadowCreate = source.find("std::make_unique<ShadowPass>()", setupStart);
    const auto rayShadowCreate = source.find("std::make_unique<RayTracedShadowPass>()", setupStart);
    const auto rayReflectionCreate = source.find("std::make_unique<RayTracedReflectionPass>()", setupStart);
    const auto rayReflectionDenoiseCreate =
        source.find("std::make_unique<RayTracedReflectionDenoisePass>()", setupStart);
    const auto rayReflectionCompositeCreate =
        source.find("std::make_unique<RayTracedReflectionCompositePass>()", setupStart);
    const auto opaqueCreate = source.find("std::make_unique<OpaquePass>()", setupStart);
    ASSERT_NE(shadowCreate, std::string::npos);
    ASSERT_NE(rayShadowCreate, std::string::npos);
    ASSERT_NE(rayReflectionCreate, std::string::npos);
    ASSERT_NE(rayReflectionDenoiseCreate, std::string::npos);
    ASSERT_NE(rayReflectionCompositeCreate, std::string::npos);
    ASSERT_NE(opaqueCreate, std::string::npos);
    EXPECT_LT(shadowCreate, rayShadowCreate);
    EXPECT_LT(rayShadowCreate, rayReflectionCreate);
    EXPECT_LT(rayReflectionCreate, rayReflectionDenoiseCreate);
    EXPECT_LT(rayReflectionDenoiseCreate, rayReflectionCompositeCreate);
    EXPECT_LT(rayReflectionCompositeCreate, opaqueCreate);

    const auto renderStart = source.find("void SceneRenderer::Render()");
    ASSERT_NE(renderStart, std::string::npos);
    const auto prepareRTScene = source.find("PrepareRayTracingScene();", renderStart);
    const auto enableReflection = source.find("m_rayTracedReflectionPass->SetEnabled(true);",
                                              renderStart);
    const auto enableReflectionDenoise =
        source.find("m_rayTracedReflectionDenoisePass->SetEnabled(", renderStart);
    const auto enableReflectionComposite =
        source.find("m_rayTracedReflectionCompositePass->SetEnabled(true);", renderStart);
    const auto buildGraph = source.find("BuildRenderGraph();", renderStart);
    ASSERT_NE(prepareRTScene, std::string::npos);
    ASSERT_NE(enableReflection, std::string::npos);
    ASSERT_NE(enableReflectionDenoise, std::string::npos);
    ASSERT_NE(enableReflectionComposite, std::string::npos);
    ASSERT_NE(buildGraph, std::string::npos);
    EXPECT_LT(prepareRTScene, enableReflection);
    EXPECT_LT(enableReflection, enableReflectionDenoise);
    EXPECT_LT(enableReflectionDenoise, enableReflectionComposite);
    EXPECT_LT(enableReflectionComposite, buildGraph);
}

TEST_F(PipelineCacheValidationFixture, RayTracingResourceBindingsMatchBetweenCppAndHlsl)
{
    const fs::path shaderDir = FindShaderDirectory();
    ASSERT_FALSE(shaderDir.empty()) << "Render/Shaders directory not found";

    const fs::path renderRoot = shaderDir.parent_path();
    const std::string cppBindings = ReadTextFile(renderRoot / "Include" / "Render" / "RayTracing" / "RayTracingResourceBindings.h");
    const std::string hlslBindings = ReadTextFile(shaderDir / "RayTracing" / "RayTracingResourceBindings.hlsli");

    EXPECT_NE(cppBindings.find("namespace RayTracingResourceBindings"), std::string::npos);
    EXPECT_NE(hlslBindings.find("RVX_RAY_TRACING_RESOURCE_BINDINGS_HLSLI"), std::string::npos);

    struct BindingExpectation
    {
        const char* cppName;
        const char* hlslName;
    };

    const BindingExpectation bindingExpectations[] = {
        {"RVX_RT_SHADOW_TLAS_BINDING", "RVX_RT_SHADOW_TLAS_REGISTER"},
        {"RVX_RT_SHADOW_OUTPUT_MASK_BINDING", "RVX_RT_SHADOW_OUTPUT_MASK_REGISTER"},
        {"RVX_RT_SHADOW_SCENE_DEPTH_BINDING", "RVX_RT_SHADOW_SCENE_DEPTH_REGISTER"},
        {"RVX_RT_SHADOW_CONSTANTS_BINDING", "RVX_RT_SHADOW_CONSTANTS_REGISTER"},
        {"RVX_RT_SHADOW_PREVIOUS_MASK_BINDING", "RVX_RT_SHADOW_PREVIOUS_MASK_REGISTER"},
        {"RVX_RT_SHADOW_PREVIOUS_DEPTH_BINDING", "RVX_RT_SHADOW_PREVIOUS_DEPTH_REGISTER"},
        {"RVX_RT_SHADOW_OUTPUT_DEPTH_BINDING", "RVX_RT_SHADOW_OUTPUT_DEPTH_REGISTER"},
        {"RVX_RT_SHADOW_PREVIOUS_NORMAL_BINDING", "RVX_RT_SHADOW_PREVIOUS_NORMAL_REGISTER"},
        {"RVX_RT_SHADOW_OUTPUT_NORMAL_BINDING", "RVX_RT_SHADOW_OUTPUT_NORMAL_REGISTER"},
        {"RVX_RT_SHADOW_ALPHA_METADATA_BINDING", "RVX_RT_SHADOW_ALPHA_METADATA_REGISTER"},
        {"RVX_RT_SHADOW_ALPHA_TEXTURES_BINDING", "RVX_RT_SHADOW_ALPHA_TEXTURES_REGISTER"},
        {"RVX_RT_SHADOW_ALPHA_INDEX_BUFFERS_BINDING", "RVX_RT_SHADOW_ALPHA_INDEX_BUFFERS_REGISTER"},
        {"RVX_RT_SHADOW_ALPHA_UV_BUFFERS_BINDING", "RVX_RT_SHADOW_ALPHA_UV_BUFFERS_REGISTER"},
        {"RVX_RT_SHADOW_MATERIAL_METADATA_BINDING", "RVX_RT_SHADOW_MATERIAL_METADATA_REGISTER"},
        {"RVX_RT_SHADOW_MATERIAL_TEXTURES_BINDING", "RVX_RT_SHADOW_MATERIAL_TEXTURES_REGISTER"},
        {"RVX_RT_SHADOW_SCENE_VELOCITY_BINDING", "RVX_RT_SHADOW_SCENE_VELOCITY_REGISTER"},
        {"RVX_RT_REFLECTION_TLAS_BINDING", "RVX_RT_REFLECTION_TLAS_REGISTER"},
        {"RVX_RT_REFLECTION_OUTPUT_BINDING", "RVX_RT_REFLECTION_OUTPUT_REGISTER"},
        {"RVX_RT_REFLECTION_SCENE_COLOR_BINDING", "RVX_RT_REFLECTION_SCENE_COLOR_REGISTER"},
        {"RVX_RT_REFLECTION_SCENE_DEPTH_BINDING", "RVX_RT_REFLECTION_SCENE_DEPTH_REGISTER"},
        {"RVX_RT_REFLECTION_CONSTANTS_BINDING", "RVX_RT_REFLECTION_CONSTANTS_REGISTER"},
        {"RVX_RT_REFLECTION_MATERIAL_METADATA_BINDING", "RVX_RT_REFLECTION_MATERIAL_METADATA_REGISTER"},
        {"RVX_RT_REFLECTION_MATERIAL_TEXTURES_BINDING", "RVX_RT_REFLECTION_MATERIAL_TEXTURES_REGISTER"},
        {"RVX_RT_REFLECTION_PREVIOUS_HISTORY_BINDING", "RVX_RT_REFLECTION_PREVIOUS_HISTORY_REGISTER"},
        {"RVX_RT_REFLECTION_PREVIOUS_DEPTH_BINDING", "RVX_RT_REFLECTION_PREVIOUS_DEPTH_REGISTER"},
        {"RVX_RT_REFLECTION_OUTPUT_DEPTH_BINDING", "RVX_RT_REFLECTION_OUTPUT_DEPTH_REGISTER"},
        {"RVX_RT_REFLECTION_PREVIOUS_NORMAL_BINDING", "RVX_RT_REFLECTION_PREVIOUS_NORMAL_REGISTER"},
        {"RVX_RT_REFLECTION_OUTPUT_NORMAL_BINDING", "RVX_RT_REFLECTION_OUTPUT_NORMAL_REGISTER"},
        {"RVX_RT_REFLECTION_GEOMETRY_METADATA_BINDING", "RVX_RT_REFLECTION_GEOMETRY_METADATA_REGISTER"},
        {"RVX_RT_REFLECTION_INDEX_BUFFERS_BINDING", "RVX_RT_REFLECTION_INDEX_BUFFERS_REGISTER"},
        {"RVX_RT_REFLECTION_UV_BUFFERS_BINDING", "RVX_RT_REFLECTION_UV_BUFFERS_REGISTER"},
        {"RVX_RT_REFLECTION_NORMAL_BUFFERS_BINDING", "RVX_RT_REFLECTION_NORMAL_BUFFERS_REGISTER"},
        {"RVX_RT_REFLECTION_TANGENT_BUFFERS_BINDING", "RVX_RT_REFLECTION_TANGENT_BUFFERS_REGISTER"},
        {"RVX_RT_REFLECTION_SCENE_VELOCITY_BINDING", "RVX_RT_REFLECTION_SCENE_VELOCITY_REGISTER"},
    };

    for (const BindingExpectation& expectation : bindingExpectations)
    {
        uint32_t cppBinding = 0;
        uint32_t hlslBinding = 0;
        ASSERT_TRUE(TryReadCppBindingConstant(cppBindings, expectation.cppName, cppBinding)) << expectation.cppName;
        ASSERT_TRUE(TryReadHLSLRegisterDefine(hlslBindings, expectation.hlslName, hlslBinding)) << expectation.hlslName;
        EXPECT_EQ(cppBinding, hlslBinding) << expectation.cppName << " vs " << expectation.hlslName;
    }

    const char* capacityExpectations[] = {
        "RVX_RT_SHADOW_MAX_MATERIAL_TEXTURES",
        "RVX_RT_SHADOW_MAX_ALPHA_TEXTURES",
        "RVX_RT_SHADOW_MAX_ALPHA_GEOMETRY_BUFFERS",
        "RVX_RT_REFLECTION_MAX_MATERIAL_TEXTURES",
        "RVX_RT_REFLECTION_MAX_GEOMETRY_BUFFERS",
    };

    for (const char* capacityName : capacityExpectations)
    {
        uint32_t cppCapacity = 0;
        uint32_t hlslCapacity = 0;
        ASSERT_TRUE(TryReadCppBindingConstant(cppBindings, capacityName, cppCapacity)) << capacityName;
        ASSERT_TRUE(TryReadHLSLUintDefine(hlslBindings, capacityName, hlslCapacity)) << capacityName;
        EXPECT_EQ(cppCapacity, hlslCapacity) << capacityName;
    }
}

TEST_F(PipelineCacheValidationFixture, RayTracingSceneMetadataConstantsMatchBetweenCppAndHlsl)
{
    const fs::path shaderDir = FindShaderDirectory();
    ASSERT_FALSE(shaderDir.empty()) << "Render/Shaders directory not found";

    const fs::path renderRoot = shaderDir.parent_path();
    const std::string sceneHeader = ReadTextFile(renderRoot / "Include" / "Render" / "RayTracing" / "RayTracingScene.h");
    const std::string sceneManagerHeader =
        ReadTextFile(renderRoot / "Include" / "Render" / "RayTracing" / "RayTracingSceneManager.h");
    const std::string sceneSource = ReadTextFile(renderRoot / "Private" / "RayTracing" / "RayTracingScene.cpp");
    const std::string hlslMetadata = ReadTextFile(shaderDir / "RayTracing" / "RayTracingSceneMetadata.hlsli");

    enum class CppConstantSource
    {
        SceneHeader,
        SceneManagerHeader,
        SceneSource
    };

    struct MetadataConstantExpectation
    {
        const char* cppName;
        const char* hlslName;
        CppConstantSource cppSource;
        bool cppEnumValue;
    };

    const MetadataConstantExpectation expectations[] = {
        {"AlphaTest", "RT_MATERIAL_ALPHA_TEST", CppConstantSource::SceneHeader, true},
        {"Transparent", "RT_MATERIAL_TRANSPARENT", CppConstantSource::SceneHeader, true},
        {"DoubleSided", "RT_MATERIAL_DOUBLE_SIDED", CppConstantSource::SceneHeader, true},
        {"HasBaseColorTexture", "RT_MATERIAL_HAS_BASE_COLOR_TEXTURE", CppConstantSource::SceneHeader, true},
        {"HasMetallicRoughnessTexture", "RT_MATERIAL_HAS_METALLIC_ROUGHNESS_TEXTURE", CppConstantSource::SceneHeader, true},
        {"HasNormalTexture", "RT_MATERIAL_HAS_NORMAL_TEXTURE", CppConstantSource::SceneHeader, true},
        {"HasEmissiveTexture", "RT_MATERIAL_HAS_EMISSIVE_TEXTURE", CppConstantSource::SceneHeader, true},
        {"Unlit", "RT_MATERIAL_UNLIT", CppConstantSource::SceneHeader, true},
        {"ShadowCaster", "RT_MATERIAL_SHADOW_CASTER", CppConstantSource::SceneHeader, true},
        {"AlphaTestEnabled", "RT_ALPHA_TEST_ENABLED", CppConstantSource::SceneManagerHeader, true},
        {"HasBaseColorTexture", "RT_ALPHA_HAS_BASE_COLOR_TEXTURE", CppConstantSource::SceneManagerHeader, true},
        {"HasResolvedBaseColorTexture", "RT_ALPHA_HAS_RESOLVED_BASE_COLOR_TEXTURE", CppConstantSource::SceneManagerHeader, true},
        {"HasUVBuffer", "RT_ALPHA_HAS_UV_BUFFER", CppConstantSource::SceneManagerHeader, true},
        {"HasIndexBuffer", "RT_ALPHA_HAS_INDEX_BUFFER", CppConstantSource::SceneManagerHeader, true},
        {"IndexFormatUInt32", "RT_ALPHA_INDEX_FORMAT_UINT32", CppConstantSource::SceneManagerHeader, true},
        {"IndexFormatUInt16", "RT_ALPHA_INDEX_FORMAT_UINT16", CppConstantSource::SceneManagerHeader, true},
        {"HasNormalBuffer", "RT_ALPHA_HAS_NORMAL_BUFFER", CppConstantSource::SceneManagerHeader, true},
        {"HasTangentBuffer", "RT_ALPHA_HAS_TANGENT_BUFFER", CppConstantSource::SceneManagerHeader, true},
        {"RVX_RT_ALPHA_WRAP_S_CLAMP", "RT_ALPHA_WRAP_S_CLAMP", CppConstantSource::SceneSource, false},
        {"RVX_RT_ALPHA_WRAP_T_CLAMP", "RT_ALPHA_WRAP_T_CLAMP", CppConstantSource::SceneSource, false},
        {"RVX_RT_ALPHA_MAG_NEAREST", "RT_ALPHA_MAG_NEAREST", CppConstantSource::SceneSource, false},
        {"RVX_RT_ALPHA_WRAP_S_MIRROR", "RT_ALPHA_WRAP_S_MIRROR", CppConstantSource::SceneSource, false},
        {"RVX_RT_ALPHA_WRAP_T_MIRROR", "RT_ALPHA_WRAP_T_MIRROR", CppConstantSource::SceneSource, false},
    };

    for (const MetadataConstantExpectation& expectation : expectations)
    {
        const std::string& cppSource = expectation.cppSource == CppConstantSource::SceneHeader
            ? sceneHeader
            : (expectation.cppSource == CppConstantSource::SceneManagerHeader ? sceneManagerHeader : sceneSource);
        const char* cppEnumName = expectation.cppSource == CppConstantSource::SceneHeader
            ? "RayTracingMaterialMetadataFlags"
            : "RayTracingInstanceAlphaMetadataFlags";
        uint32_t cppValue = 0;
        uint32_t hlslValue = 0;
        ASSERT_TRUE(expectation.cppEnumValue
                        ? TryReadCppEnumValue(cppSource, cppEnumName, expectation.cppName, cppValue)
                        : TryReadCppConstUintExpression(cppSource, expectation.cppName, cppValue))
            << expectation.cppName;
        ASSERT_TRUE(TryReadHLSLStaticUintExpression(hlslMetadata, expectation.hlslName, hlslValue))
            << expectation.hlslName;
        EXPECT_EQ(cppValue, hlslValue) << expectation.cppName << " vs " << expectation.hlslName;
    }

    uint32_t hlslInvalidIndex = 0;
    ASSERT_TRUE(TryReadHLSLStaticUintExpression(hlslMetadata, "RT_INVALID_INDEX", hlslInvalidIndex));
    EXPECT_EQ(hlslInvalidIndex, std::numeric_limits<uint32_t>::max());
}

TEST_F(PipelineCacheValidationFixture, RayTracedShadowPassCreatesDescriptorSetAndDispatchesRays)
{
    if (!HasCompilerAvailable())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    const fs::path renderRoot = FindShaderDirectory().parent_path();
    const std::string passHeader = ReadTextFile(renderRoot / "Include" / "Render" / "Passes" / "RayTracedShadowPass.h");
    const std::string passSource = ReadTextFile(renderRoot / "Private" / "Passes" / "RayTracedShadowPass.cpp");
    const std::string viewDataHeader = ReadTextFile(renderRoot / "Include" / "Render" / "Renderer" / "ViewData.h");
    const std::string pipelineHeader = ReadTextFile(renderRoot / "Include" / "Render" / "PipelineCache.h");
    const std::string pipelineSource = ReadTextFile(renderRoot / "Private" / "PipelineCache.cpp");
    const std::string shaderSource = ReadTextFile(FindShaderDirectory() / "RayTracing" / "RayTracedShadow.hlsl");
    const std::string metadataSource =
        ReadTextFile(FindShaderDirectory() / "RayTracing" / "RayTracingSceneMetadata.hlsli");

    EXPECT_NE(passHeader.find("void SetResources(PipelineCache* pipelineCache, ResourceViewCache* viewCache);"),
              std::string::npos);
    EXPECT_NE(passHeader.find("void SetResources(GPUResourceManager* gpuResources,"),
              std::string::npos);
    EXPECT_NE(passHeader.find("bool EnsureHistoryTextures(uint32 width, uint32 height);"), std::string::npos);
    EXPECT_NE(passHeader.find("void ResetHistoryTextures();"), std::string::npos);
    EXPECT_NE(passHeader.find("ShadowPassConfig m_lastHistoryConfig;"), std::string::npos);
    EXPECT_NE(passHeader.find("bool m_lastHistoryConfigValid = false;"), std::string::npos);
    EXPECT_NE(passHeader.find("bool temporalAccumulated = false;"), std::string::npos);
    EXPECT_NE(passHeader.find("bool resourceViewsAvailable = false;"), std::string::npos);
    EXPECT_NE(passHeader.find("bool descriptorSetAvailable = false;"), std::string::npos);
    EXPECT_NE(passHeader.find("bool depthHistoryAvailable = false;"), std::string::npos);
    EXPECT_NE(passHeader.find("bool normalHistoryAvailable = false;"), std::string::npos);
    EXPECT_NE(passHeader.find("bool velocityAvailable = false;"), std::string::npos);
    EXPECT_NE(passHeader.find("bool historyReset = false;"), std::string::npos);
    EXPECT_NE(passHeader.find("bool historyRecreated = false;"), std::string::npos);
    EXPECT_NE(passHeader.find("bool historyResolutionChanged = false;"), std::string::npos);
    EXPECT_NE(passHeader.find("bool historyConfigChanged = false;"), std::string::npos);
    EXPECT_NE(passHeader.find("bool materialMetadataAvailable = false;"), std::string::npos);
    EXPECT_NE(passHeader.find("bool materialTextureTableAvailable = false;"), std::string::npos);
    EXPECT_NE(passHeader.find("bool alphaMetadataAvailable = false;"), std::string::npos);
    EXPECT_NE(passHeader.find("bool alphaTextureTableAvailable = false;"), std::string::npos);
    EXPECT_NE(passHeader.find("bool alphaGeometryTableAvailable = false;"), std::string::npos);
    EXPECT_NE(passHeader.find("uint32 alphaTexturesBound = 0;"), std::string::npos);
    EXPECT_NE(passHeader.find("uint32 materialTextureCount = 0;"), std::string::npos);
    EXPECT_NE(passHeader.find("uint32 materialTexturesBound = 0;"), std::string::npos);
    EXPECT_NE(passHeader.find("uint32 alphaIndexBufferCount = 0;"), std::string::npos);
    EXPECT_NE(passHeader.find("uint32 alphaUVBufferCount = 0;"), std::string::npos);
    EXPECT_NE(passHeader.find("bool gpuTimingSupported = false;"), std::string::npos);
    EXPECT_NE(passHeader.find("bool gpuTimingQueriesRecorded = false;"), std::string::npos);
    EXPECT_NE(passHeader.find("bool gpuTimingResolveRecorded = false;"), std::string::npos);
    EXPECT_NE(passHeader.find("bool gpuTimingReadbackBufferAvailable = false;"), std::string::npos);
    EXPECT_NE(passHeader.find("bool gpuTimingResultAvailable = false;"), std::string::npos);
    EXPECT_NE(passHeader.find("uint32 gpuTimingStartQueryIndex = 0;"), std::string::npos);
    EXPECT_NE(passHeader.find("uint32 gpuTimingEndQueryIndex = 1;"), std::string::npos);
    EXPECT_NE(passHeader.find("uint64 gpuTimestampFrequency = 0;"), std::string::npos);
    EXPECT_NE(passHeader.find("uint64 gpuTimingReadbackBytes = 0;"), std::string::npos);
    EXPECT_NE(passHeader.find("uint64 gpuTimingElapsedTicks = 0;"), std::string::npos);
    EXPECT_NE(passHeader.find("float gpuTimingElapsedMs = 0.0f;"), std::string::npos);
    EXPECT_NE(passHeader.find("uint32 gpuTimingReadbackBufferCount = 0;"), std::string::npos);
    EXPECT_NE(passHeader.find("uint32 gpuTimingReadbackFrameIndex = RVX_INVALID_INDEX;"), std::string::npos);
    EXPECT_NE(passHeader.find("RHIQueryPoolRef m_timingQueryPool;"), std::string::npos);
    EXPECT_NE(passHeader.find("std::array<RHIBufferRef, RVX_MAX_FRAME_COUNT> m_timingReadbackBuffers;"),
              std::string::npos);
    EXPECT_NE(passHeader.find("std::array<bool, RVX_MAX_FRAME_COUNT> m_timingReadbackValid{};"),
              std::string::npos);
    EXPECT_NE(passHeader.find("uint32 samplesPerPixel = 1;"), std::string::npos);
    EXPECT_NE(passHeader.find("uint64 dispatchPixelCount = 0;"), std::string::npos);
    EXPECT_NE(passHeader.find("uint64 estimatedRayCount = 0;"), std::string::npos);
    EXPECT_NE(passHeader.find("bool ResolveMaterialTextureViews(std::vector<RHITextureView*>& outViews) const;"),
              std::string::npos);
    EXPECT_NE(passHeader.find("m_historyDepthTextures"), std::string::npos);
    EXPECT_NE(passHeader.find("m_historyNormalTextures"), std::string::npos);
    EXPECT_NE(passHeader.find("RGTextureHandle m_velocityReadHandle;"), std::string::npos);
    EXPECT_NE(passHeader.find("RHITextureRef m_fallbackVelocityTexture;"), std::string::npos);
    EXPECT_NE(passHeader.find("bool EnsureFallbackVelocityTexture();"), std::string::npos);
    EXPECT_NE(passSource.find("GetRayTracedShadowPipeline()"), std::string::npos);
    EXPECT_NE(passSource.find("GetRayTracedShadowShaderTable()"), std::string::npos);
    EXPECT_NE(passSource.find("EnsureHistoryTextures(view.viewportWidth, view.viewportHeight)"), std::string::npos);
    EXPECT_NE(passSource.find("m_stats.historyReset = view.resetTemporalHistory;"), std::string::npos);
    EXPECT_NE(passSource.find("m_stats.historyRecreated = true;"), std::string::npos);
    EXPECT_NE(passSource.find("ShadowHistoryConfigChanged(m_lastHistoryConfig, m_config)"),
              std::string::npos);
    EXPECT_NE(passSource.find("m_stats.historyConfigChanged = historyConfigChanged;"), std::string::npos);
    EXPECT_NE(passSource.find("m_lastHistoryConfig = m_config;"), std::string::npos);
    EXPECT_NE(passSource.find("m_lastHistoryConfigValid = true;"), std::string::npos);
    EXPECT_NE(passSource.find("m_stats.historyResolutionChanged = historyResolutionChanged;"),
              std::string::npos);
    EXPECT_NE(passSource.find("m_stats.historyAvailable = false;"), std::string::npos);
    EXPECT_NE(passSource.find("m_viewCache->InvalidateTexture(texture.Get())"), std::string::npos);
    const auto shadowOnAdd = passSource.find("void RayTracedShadowPass::OnAdd(IRHIDevice* device)");
    ASSERT_NE(shadowOnAdd, std::string::npos);
    const auto shadowDeviceChange = passSource.find("if (m_device != device)", shadowOnAdd);
    ASSERT_NE(shadowDeviceChange, std::string::npos);
    const auto shadowResetHistoryOnDeviceChange = passSource.find("ResetHistoryTextures();", shadowDeviceChange);
    const auto shadowMaskClearOnDeviceChange = passSource.find("m_shadowMaskTexture = nullptr;", shadowDeviceChange);
    const auto shadowConstantBufferResetOnDeviceChange = passSource.find("m_constantBuffer.Reset();", shadowDeviceChange);
    const auto shadowRetainedDescriptorsClearOnDeviceChange =
        passSource.find("m_retainedDescriptorSets.clear();", shadowDeviceChange);
    const auto shadowDeviceAssign = passSource.find("m_device = device;", shadowDeviceChange);
    ASSERT_NE(shadowResetHistoryOnDeviceChange, std::string::npos);
    ASSERT_NE(shadowMaskClearOnDeviceChange, std::string::npos);
    ASSERT_NE(shadowConstantBufferResetOnDeviceChange, std::string::npos);
    ASSERT_NE(shadowRetainedDescriptorsClearOnDeviceChange, std::string::npos);
    ASSERT_NE(shadowDeviceAssign, std::string::npos);
    EXPECT_LT(shadowResetHistoryOnDeviceChange, shadowMaskClearOnDeviceChange);
    EXPECT_LT(shadowMaskClearOnDeviceChange, shadowConstantBufferResetOnDeviceChange);
    EXPECT_LT(shadowConstantBufferResetOnDeviceChange, shadowRetainedDescriptorsClearOnDeviceChange);
    EXPECT_LT(shadowRetainedDescriptorsClearOnDeviceChange, shadowDeviceAssign);
    EXPECT_NE(passSource.find("if (view.resetTemporalHistory)"), std::string::npos);
    EXPECT_NE(passSource.find("m_historyValid = false;"), std::string::npos);
    EXPECT_NE(passSource.find("m_historyViewValid = false;"), std::string::npos);
    EXPECT_NE(passSource.find("view.renderGraph->ImportTexture("), std::string::npos);
    EXPECT_NE(passSource.find("SetExportState(m_shadowMaskHandle, RHIResourceState::ShaderResource)"),
              std::string::npos);
    EXPECT_NE(passSource.find("RayTracedShadowDepthHistory0"), std::string::npos);
    EXPECT_NE(passSource.find("RayTracedShadowNormalHistory0"), std::string::npos);
    EXPECT_NE(passSource.find("SetExportState(m_historyDepthWriteHandle, RHIResourceState::ShaderResource)"),
              std::string::npos);
    EXPECT_NE(passSource.find("SetExportState(m_historyNormalWriteHandle, RHIResourceState::ShaderResource)"),
              std::string::npos);
    EXPECT_NE(passSource.find("GetDefaultUAV(shadowMask)"), std::string::npos);
    EXPECT_NE(passSource.find("m_depthReadHandle = builder.Read(depthHandle, RHIShaderStage::AllRayTracing);"),
              std::string::npos);
    EXPECT_NE(passSource.find("m_stats.velocityAvailable = view.velocityTarget.IsValid();"), std::string::npos);
    EXPECT_NE(passSource.find("m_stats.samplesPerPixel = std::min(std::max(m_config.rayTracedSamplesPerPixel, 1u), 8u);"),
              std::string::npos);
    EXPECT_NE(passSource.find("TryGetRHIRayTracingDispatchRayCount(m_stats.width, m_stats.height, 1, m_stats.dispatchPixelCount)"),
              std::string::npos);
    EXPECT_NE(passSource.find("TryMultiplyRHIRayTracingCount(m_stats.dispatchPixelCount"),
              std::string::npos);
    EXPECT_NE(passSource.find("RVX_RAY_TRACED_SHADOW_TIMING_QUERY_COUNT_PER_FRAME = 2"),
              std::string::npos);
    EXPECT_NE(passSource.find("queryDesc.count = RVX_MAX_FRAME_COUNT * RVX_RAY_TRACED_SHADOW_TIMING_QUERY_COUNT_PER_FRAME;"),
              std::string::npos);
    EXPECT_NE(passSource.find("queryDesc.debugName = \"RayTracedShadowTimingQueries\";"), std::string::npos);
    EXPECT_NE(passSource.find("RVX_RAY_TRACED_SHADOW_TIMING_READBACK_BYTES = sizeof(uint64) * 2"),
              std::string::npos);
    EXPECT_NE(passSource.find("readbackDesc.usage = RHIBufferUsage::CopyDst;"), std::string::npos);
    EXPECT_NE(passSource.find("readbackDesc.memoryType = RHIMemoryType::Readback;"), std::string::npos);
    EXPECT_NE(passSource.find("readbackDesc.debugName = \"RayTracedShadowTimingReadback\";"), std::string::npos);
    EXPECT_NE(passSource.find("m_stats.gpuTimingSupported = m_timingQueryPool != nullptr;"), std::string::npos);
    EXPECT_NE(passSource.find("m_stats.gpuTimingReadbackBufferAvailable = timingReadbackBuffer != nullptr;"),
              std::string::npos);
    EXPECT_NE(passSource.find("m_stats.gpuTimingReadbackBytes = static_cast<uint64>(timingReadbackBufferCount) *"),
              std::string::npos);
    EXPECT_NE(passSource.find("m_stats.gpuTimestampFrequency = m_timingQueryPool ? m_timingQueryPool->GetTimestampFrequency() : 0;"),
              std::string::npos);
    EXPECT_NE(passSource.find("ctx.WriteTimestamp(m_timingQueryPool.Get(), timingStartQuery);"),
              std::string::npos);
    EXPECT_NE(passSource.find("ctx.WriteTimestamp(m_timingQueryPool.Get(), timingEndQuery);"),
              std::string::npos);
    EXPECT_NE(passSource.find("m_stats.gpuTimingQueriesRecorded = true;"), std::string::npos);
    EXPECT_NE(passSource.find("ctx.ResolveQueries(m_timingQueryPool.Get(),"), std::string::npos);
    EXPECT_NE(passSource.find("m_timingReadbackValid[timingFrameIndex] = true;"), std::string::npos);
    EXPECT_NE(passSource.find("m_stats.gpuTimingResolveRecorded = true;"), std::string::npos);
    EXPECT_NE(passSource.find("TryReadbackTimingResult(timingFrameIndex);"), std::string::npos);
    EXPECT_NE(passSource.find("m_stats.gpuTimingElapsedMs = static_cast<float>("), std::string::npos);
    EXPECT_NE(passSource.find("m_velocityReadHandle = builder.Read(view.velocityTarget, RHIShaderStage::AllRayTracing);"),
              std::string::npos);
    EXPECT_NE(passSource.find("GetDefaultSRV(depthTexture)"), std::string::npos);
    EXPECT_NE(passSource.find("RHITexture* sceneVelocity = m_velocityReadHandle.IsValid() ?"),
              std::string::npos);
    EXPECT_NE(passSource.find("EnsureFallbackVelocityTexture()"), std::string::npos);
    EXPECT_NE(passSource.find("RHITextureDesc::Texture2D(1, 1, RHIFormat::RG16_FLOAT)"), std::string::npos);
    EXPECT_NE(passSource.find("RayTracedShadowFallbackVelocity"), std::string::npos);
    EXPECT_NE(passSource.find("sceneVelocity = m_fallbackVelocityTexture.Get();"), std::string::npos);
    EXPECT_NE(passSource.find("m_viewCache->GetDefaultSRV(sceneVelocity)"), std::string::npos);
    EXPECT_NE(passSource.find("GetDefaultSRV(previousShadowMask)"), std::string::npos);
    EXPECT_NE(passSource.find("GetDefaultSRV(previousDepthHistory)"), std::string::npos);
    EXPECT_NE(passSource.find("GetDefaultUAV(currentDepthHistory)"), std::string::npos);
    EXPECT_NE(passSource.find("GetDefaultSRV(previousNormalHistory)"), std::string::npos);
    EXPECT_NE(passSource.find("GetDefaultUAV(currentNormalHistory)"), std::string::npos);
    EXPECT_NE(passSource.find("EnsureConstantBuffer()"), std::string::npos);
    EXPECT_NE(passSource.find("UpdateConstants(view)"), std::string::npos);
    EXPECT_NE(passSource.find("descriptorDesc.BindAccelerationStructure(RTShadowBindings::RVX_RT_SHADOW_TLAS_BINDING, tlas);"), std::string::npos);
    EXPECT_NE(passSource.find("descriptorDesc.BindTexture(RTShadowBindings::RVX_RT_SHADOW_OUTPUT_MASK_BINDING, shadowMaskUAV);"), std::string::npos);
    EXPECT_NE(passSource.find("descriptorDesc.BindTexture(RTShadowBindings::RVX_RT_SHADOW_SCENE_DEPTH_BINDING, depthSRV);"), std::string::npos);
    EXPECT_NE(passSource.find("descriptorDesc.BindBuffer(RTShadowBindings::RVX_RT_SHADOW_CONSTANTS_BINDING,"), std::string::npos);
    EXPECT_NE(passSource.find("descriptorDesc.BindTexture(RTShadowBindings::RVX_RT_SHADOW_PREVIOUS_MASK_BINDING, previousShadowMaskSRV);"), std::string::npos);
    EXPECT_NE(passSource.find("descriptorDesc.BindTexture(RTShadowBindings::RVX_RT_SHADOW_PREVIOUS_DEPTH_BINDING, previousDepthHistorySRV);"), std::string::npos);
    EXPECT_NE(passSource.find("descriptorDesc.BindTexture(RTShadowBindings::RVX_RT_SHADOW_OUTPUT_DEPTH_BINDING, currentDepthHistoryUAV);"), std::string::npos);
    EXPECT_NE(passSource.find("descriptorDesc.BindTexture(RTShadowBindings::RVX_RT_SHADOW_PREVIOUS_NORMAL_BINDING, previousNormalHistorySRV);"), std::string::npos);
    EXPECT_NE(passSource.find("descriptorDesc.BindTexture(RTShadowBindings::RVX_RT_SHADOW_OUTPUT_NORMAL_BINDING, currentNormalHistoryUAV);"), std::string::npos);
    EXPECT_NE(passSource.find("RHIBuffer* materialMetadataBuffer = m_sceneManager->GetInstanceMaterialMetadataBuffer();"),
              std::string::npos);
    EXPECT_NE(passSource.find("ResolveMaterialTextureViews(materialTextureViews)"), std::string::npos);
    EXPECT_NE(passSource.find("m_stats.resourceViewsAvailable = true;"), std::string::npos);
    EXPECT_NE(passSource.find("descriptorDesc.BindBuffer(RTShadowBindings::RVX_RT_SHADOW_ALPHA_METADATA_BINDING, alphaMetadataBuffer);"), std::string::npos);
    EXPECT_NE(passSource.find("descriptorDesc.BindTexture(RTShadowBindings::RVX_RT_SHADOW_ALPHA_TEXTURES_BINDING, alphaTextureViews[textureIndex], textureIndex);"),
              std::string::npos);
    EXPECT_NE(passSource.find("descriptorDesc.BindBuffer(RTShadowBindings::RVX_RT_SHADOW_ALPHA_INDEX_BUFFERS_BINDING, alphaIndexBuffers[bufferIndex], 0, RVX_WHOLE_SIZE, bufferIndex);"),
              std::string::npos);
    EXPECT_NE(passSource.find("descriptorDesc.BindBuffer(RTShadowBindings::RVX_RT_SHADOW_ALPHA_UV_BUFFERS_BINDING, alphaUVBuffers[bufferIndex], 0, RVX_WHOLE_SIZE, bufferIndex);"),
              std::string::npos);
    EXPECT_NE(passSource.find("descriptorDesc.BindBuffer(RTShadowBindings::RVX_RT_SHADOW_MATERIAL_METADATA_BINDING, materialMetadataBuffer);"), std::string::npos);
    EXPECT_NE(passSource.find("descriptorDesc.BindTexture(RTShadowBindings::RVX_RT_SHADOW_MATERIAL_TEXTURES_BINDING, materialTextureViews[textureIndex], textureIndex);"),
              std::string::npos);
    EXPECT_NE(passSource.find("descriptorDesc.BindTexture(RTShadowBindings::RVX_RT_SHADOW_SCENE_VELOCITY_BINDING, sceneVelocitySRV);"), std::string::npos);
    EXPECT_NE(passSource.find("m_stats.descriptorSetAvailable = true;"), std::string::npos);
    const auto shadowResourceViewsReady = passSource.find("m_stats.resourceViewsAvailable = true;");
    const auto shadowCreateDescriptorSet = passSource.find("device->CreateDescriptorSet(descriptorDesc);");
    const auto shadowDescriptorSetReady = passSource.find("m_stats.descriptorSetAvailable = true;");
    const auto shadowSetPipeline = passSource.find("ctx.SetPipeline(pipeline);");
    ASSERT_NE(shadowResourceViewsReady, std::string::npos);
    ASSERT_NE(shadowCreateDescriptorSet, std::string::npos);
    ASSERT_NE(shadowDescriptorSetReady, std::string::npos);
    ASSERT_NE(shadowSetPipeline, std::string::npos);
    EXPECT_LT(shadowResourceViewsReady, shadowCreateDescriptorSet);
    EXPECT_LT(shadowCreateDescriptorSet, shadowDescriptorSetReady);
    EXPECT_LT(shadowDescriptorSetReady, shadowSetPipeline);
    EXPECT_NE(passSource.find("m_sceneManager->GetInstanceMaterialTextureTable()"), std::string::npos);
    EXPECT_NE(passSource.find("m_config.rayTracedTemporalAccumulation"), std::string::npos);
    EXPECT_NE(passSource.find("m_config.rayTracedLightAngularRadius"), std::string::npos);
    EXPECT_NE(passSource.find("m_config.rayTracedSamplesPerPixel"), std::string::npos);
    EXPECT_NE(passSource.find("m_config.rayTracedTemporalBlendFactor"), std::string::npos);
    EXPECT_NE(passSource.find("m_config.rayTracedHistoryDepthThreshold"), std::string::npos);
    EXPECT_NE(passSource.find("m_config.rayTracedHistoryNormalThreshold"), std::string::npos);
    EXPECT_NE(passSource.find("m_config.rayTracedHistoryVelocityRejectionScale"), std::string::npos);
    EXPECT_NE(passSource.find("view.velocityTarget.IsValid() ? 1.0f : 0.0f"), std::string::npos);
    EXPECT_NE(passSource.find("velocityRejectionScale"), std::string::npos);
    EXPECT_NE(passSource.find("m_config.rayTracedInstanceMask"), std::string::npos);
    EXPECT_NE(passSource.find("constants.previousViewProjection = m_historyViewValid"), std::string::npos);
    EXPECT_NE(passSource.find("historyReprojectionParams"), std::string::npos);
    EXPECT_NE(passSource.find("softShadowParams"), std::string::npos);
    EXPECT_NE(passSource.find("rayOptions"), std::string::npos);
    EXPECT_NE(passSource.find("std::min<uint32>(m_config.rayTracedInstanceMask, 0xFFu)"),
              std::string::npos);
    EXPECT_NE(passSource.find("std::min(std::max(m_config.rayTracedSamplesPerPixel, 1u), 8u)"),
              std::string::npos);
    EXPECT_NE(passSource.find("view.frameNumber & 0x00FFFFFFull"), std::string::npos);
    EXPECT_NE(passSource.find("ctx.DispatchRays(dispatchDesc);"), std::string::npos);
    EXPECT_NE(viewDataHeader.find("bool resetTemporalHistory = false;"), std::string::npos);

    const std::string shadowPassHeader =
        ReadTextFile(renderRoot / "Include" / "Render" / "Passes" / "ShadowPass.h");
    EXPECT_NE(shadowPassHeader.find("bool rayTracedTemporalAccumulation = true;"), std::string::npos);
    EXPECT_NE(shadowPassHeader.find("float rayTracedLightAngularRadius = 0.00465f;"), std::string::npos);
    EXPECT_NE(shadowPassHeader.find("uint32_t rayTracedSamplesPerPixel = 1;"), std::string::npos);
    EXPECT_NE(shadowPassHeader.find("float rayTracedTemporalBlendFactor = 0.75f;"), std::string::npos);
    EXPECT_NE(shadowPassHeader.find("float rayTracedHistoryDepthThreshold = 0.01f;"), std::string::npos);
    EXPECT_NE(shadowPassHeader.find("float rayTracedHistoryNormalThreshold = 0.85f;"), std::string::npos);
    EXPECT_NE(shadowPassHeader.find("float rayTracedHistoryVelocityRejectionScale = 8.0f;"), std::string::npos);
    EXPECT_NE(shadowPassHeader.find("uint32_t rayTracedInstanceMask = 0xFF;"), std::string::npos);

    EXPECT_NE(pipelineHeader.find("GetRayTracedShadowPipeline()"), std::string::npos);
    EXPECT_NE(pipelineHeader.find("GetRayTracedShadowShaderTable()"), std::string::npos);
    EXPECT_NE(pipelineHeader.find("GetRayTracedShadowSetLayout()"), std::string::npos);
    EXPECT_NE(pipelineHeader.find("m_rayTracedShadowAnyHitShader"), std::string::npos);
    EXPECT_NE(pipelineHeader.find("m_rayTracedShadowAnyHitCompileResult"), std::string::npos);
    EXPECT_NE(pipelineSource.find("RayTracing/RayTracedShadow.hlsl"), std::string::npos);
    EXPECT_NE(pipelineSource.find("CreateRayTracedShadowPipelineLayout()"), std::string::npos);
    EXPECT_NE(pipelineSource.find("anyHitDesc.entryPoint = \"ShadowAnyHit\""), std::string::npos);
    EXPECT_NE(pipelineSource.find("anyHitDesc.stage = RHIShaderStage::AnyHit"), std::string::npos);
    EXPECT_NE(pipelineSource.find("RHIBindingType::AccelerationStructure"), std::string::npos);
    EXPECT_NE(pipelineSource.find("RHIBindingType::StorageTexture"), std::string::npos);
    EXPECT_NE(pipelineSource.find("setLayoutDesc.AddBinding(RTShadowBindings::RVX_RT_SHADOW_SCENE_DEPTH_BINDING, RHIBindingType::SampledTexture"),
              std::string::npos);
    EXPECT_NE(pipelineSource.find("setLayoutDesc.AddBinding(RTShadowBindings::RVX_RT_SHADOW_CONSTANTS_BINDING, RHIBindingType::UniformBuffer"),
              std::string::npos);
    EXPECT_NE(pipelineSource.find("setLayoutDesc.AddBinding(RTShadowBindings::RVX_RT_SHADOW_PREVIOUS_MASK_BINDING, RHIBindingType::SampledTexture"),
              std::string::npos);
    EXPECT_NE(pipelineSource.find("setLayoutDesc.AddBinding(RTShadowBindings::RVX_RT_SHADOW_PREVIOUS_DEPTH_BINDING, RHIBindingType::SampledTexture"),
              std::string::npos);
    EXPECT_NE(pipelineSource.find("setLayoutDesc.AddBinding(RTShadowBindings::RVX_RT_SHADOW_OUTPUT_DEPTH_BINDING, RHIBindingType::StorageTexture"),
              std::string::npos);
    EXPECT_NE(pipelineSource.find("setLayoutDesc.AddBinding(RTShadowBindings::RVX_RT_SHADOW_PREVIOUS_NORMAL_BINDING, RHIBindingType::SampledTexture"),
              std::string::npos);
    EXPECT_NE(pipelineSource.find("setLayoutDesc.AddBinding(RTShadowBindings::RVX_RT_SHADOW_OUTPUT_NORMAL_BINDING, RHIBindingType::StorageTexture"),
              std::string::npos);
    EXPECT_NE(pipelineSource.find("setLayoutDesc.AddBinding(RTShadowBindings::RVX_RT_SHADOW_ALPHA_METADATA_BINDING, RHIBindingType::ShaderResourceBuffer"),
              std::string::npos);
    EXPECT_NE(pipelineSource.find("setLayoutDesc.AddBinding(RTShadowBindings::RVX_RT_SHADOW_ALPHA_TEXTURES_BINDING, RHIBindingType::SampledTexture"),
              std::string::npos);
    EXPECT_NE(pipelineSource.find("setLayoutDesc.AddBinding(RTShadowBindings::RVX_RT_SHADOW_ALPHA_INDEX_BUFFERS_BINDING, RHIBindingType::ShaderResourceBuffer"),
              std::string::npos);
    EXPECT_NE(pipelineSource.find("setLayoutDesc.AddBinding(RTShadowBindings::RVX_RT_SHADOW_ALPHA_UV_BUFFERS_BINDING, RHIBindingType::ShaderResourceBuffer"),
              std::string::npos);
    EXPECT_NE(pipelineSource.find("setLayoutDesc.AddBinding(RTShadowBindings::RVX_RT_SHADOW_MATERIAL_METADATA_BINDING, RHIBindingType::ShaderResourceBuffer"),
              std::string::npos);
    EXPECT_NE(pipelineSource.find("setLayoutDesc.AddBinding(RTShadowBindings::RVX_RT_SHADOW_MATERIAL_TEXTURES_BINDING, RHIBindingType::SampledTexture"),
              std::string::npos);
    EXPECT_NE(pipelineSource.find("setLayoutDesc.AddBinding(RTShadowBindings::RVX_RT_SHADOW_SCENE_VELOCITY_BINDING, RHIBindingType::SampledTexture"),
              std::string::npos);
    EXPECT_NE(pipelineSource.find("CreateRayTracedShadowPipeline()"), std::string::npos);
    EXPECT_NE(pipelineSource.find("shaderTableDesc.rayTracingPipelineOwner = m_rayTracedShadowPipeline;"),
              std::string::npos);
    EXPECT_NE(pipelineSource.find("RHIRayTracingShaderGroupType::TrianglesHitGroup"), std::string::npos);
    EXPECT_NE(pipelineSource.find("hitGroup.anyHitShader = m_rayTracedShadowAnyHitShader.Get();"),
              std::string::npos);
    EXPECT_NE(pipelineSource.find("shaderTableDesc.rayGenerationRecords.push_back({0});"), std::string::npos);
    EXPECT_NE(pipelineSource.find("shaderTableDesc.missRecords.push_back({1});"), std::string::npos);
    EXPECT_NE(pipelineSource.find("shaderTableDesc.hitGroupRecords.push_back({2});"), std::string::npos);

    EXPECT_NE(shaderSource.find("#include \"RayTracingResourceBindings.hlsli\""), std::string::npos);
    EXPECT_NE(shaderSource.find("#include \"RayTracingSceneMetadata.hlsli\""), std::string::npos);
    EXPECT_NE(shaderSource.find("[shader(\"raygeneration\")]"), std::string::npos);
    EXPECT_NE(shaderSource.find("TraceRay(gScene"), std::string::npos);
    EXPECT_NE(shaderSource.find("RWTexture2D<float> gShadowMask"), std::string::npos);
    EXPECT_NE(shaderSource.find("Texture2D<float> gSceneDepth : register(RVX_RT_SHADOW_SCENE_DEPTH_REGISTER, space0);"), std::string::npos);
    EXPECT_NE(shaderSource.find("Texture2D<float> gPreviousShadowMask : register(RVX_RT_SHADOW_PREVIOUS_MASK_REGISTER, space0);"),
              std::string::npos);
    EXPECT_NE(shaderSource.find("Texture2D<float> gPreviousDepthHistory : register(RVX_RT_SHADOW_PREVIOUS_DEPTH_REGISTER, space0);"),
              std::string::npos);
    EXPECT_NE(shaderSource.find("RWTexture2D<float> gCurrentDepthHistory : register(RVX_RT_SHADOW_OUTPUT_DEPTH_REGISTER, space0);"),
              std::string::npos);
    EXPECT_NE(shaderSource.find("Texture2D<float4> gPreviousNormalHistory : register(RVX_RT_SHADOW_PREVIOUS_NORMAL_REGISTER, space0);"),
              std::string::npos);
    EXPECT_NE(shaderSource.find("RWTexture2D<float4> gCurrentNormalHistory : register(RVX_RT_SHADOW_OUTPUT_NORMAL_REGISTER, space0);"),
              std::string::npos);
    EXPECT_NE(shaderSource.find("StructuredBuffer<RayTracingInstanceAlphaMetadata> gInstanceAlphaMetadata : register(RVX_RT_SHADOW_ALPHA_METADATA_REGISTER, space0);"),
              std::string::npos);
    EXPECT_NE(shaderSource.find("Texture2D<float4> gAlphaBaseColorTextures[RT_MAX_ALPHA_TEXTURES] : register(RVX_RT_SHADOW_ALPHA_TEXTURES_REGISTER, space0);"),
              std::string::npos);
    EXPECT_NE(shaderSource.find("ByteAddressBuffer gAlphaIndexBuffers[RT_MAX_ALPHA_GEOMETRY_BUFFERS] : register(RVX_RT_SHADOW_ALPHA_INDEX_BUFFERS_REGISTER, space0);"),
              std::string::npos);
    EXPECT_NE(shaderSource.find("ByteAddressBuffer gAlphaUVBuffers[RT_MAX_ALPHA_GEOMETRY_BUFFERS] : register(RVX_RT_SHADOW_ALPHA_UV_BUFFERS_REGISTER, space0);"),
              std::string::npos);
    EXPECT_NE(metadataSource.find("struct RayTracingInstanceAlphaMetadata"), std::string::npos);
    EXPECT_NE(metadataSource.find("struct RayTracingInstanceMaterialMetadata"), std::string::npos);
    EXPECT_NE(shaderSource.find("StructuredBuffer<RayTracingInstanceMaterialMetadata> gInstanceMaterialMetadata : register(RVX_RT_SHADOW_MATERIAL_METADATA_REGISTER, space0);"),
              std::string::npos);
    EXPECT_NE(shaderSource.find("Texture2D<float4> gMaterialTextures[RT_MAX_MATERIAL_TEXTURES] : register(RVX_RT_SHADOW_MATERIAL_TEXTURES_REGISTER, space0);"),
              std::string::npos);
    EXPECT_NE(shaderSource.find("Texture2D<float2> gSceneVelocity : register(RVX_RT_SHADOW_SCENE_VELOCITY_REGISTER, space0);"),
              std::string::npos);
    EXPECT_NE(metadataSource.find("struct RayTracingMaterialTextureSamplingMetadata"), std::string::npos);
    EXPECT_NE(metadataSource.find("float2 UVOffset;"), std::string::npos);
    EXPECT_NE(metadataSource.find("float2 UVScale;"), std::string::npos);
    EXPECT_NE(metadataSource.find("float UVRotation;"), std::string::npos);
    EXPECT_NE(metadataSource.find("uint SamplerFlags;"), std::string::npos);
    EXPECT_NE(shaderSource.find("static const uint RT_MAX_MATERIAL_TEXTURES = RVX_RT_SHADOW_MAX_MATERIAL_TEXTURES;"), std::string::npos);
    EXPECT_NE(metadataSource.find("static const uint RT_MATERIAL_ALPHA_TEST = 1u << 0;"), std::string::npos);
    EXPECT_NE(metadataSource.find("static const uint RT_MATERIAL_SHADOW_CASTER = 1u << 8;"),
              std::string::npos);
    EXPECT_NE(metadataSource.find("float4 BaseColorFactor;"), std::string::npos);
    EXPECT_NE(metadataSource.find("float4 EmissiveFactor;"), std::string::npos);
    EXPECT_NE(metadataSource.find("float4 MaterialFactors; // x: metallic, y: roughness, z: alpha cutoff, w: normal scale"),
              std::string::npos);
    EXPECT_NE(metadataSource.find("uint BaseColorTextureTableIndex;"), std::string::npos);
    EXPECT_NE(metadataSource.find("uint MetallicRoughnessTextureTableIndex;"), std::string::npos);
    EXPECT_NE(metadataSource.find("uint NormalTextureTableIndex;"), std::string::npos);
    EXPECT_NE(metadataSource.find("uint EmissiveTextureTableIndex;"), std::string::npos);
    EXPECT_NE(metadataSource.find("RayTracingMaterialTextureSamplingMetadata BaseColorTextureSampling;"),
              std::string::npos);
    EXPECT_NE(metadataSource.find("RayTracingMaterialTextureSamplingMetadata MetallicRoughnessTextureSampling;"),
              std::string::npos);
    EXPECT_NE(metadataSource.find("RayTracingMaterialTextureSamplingMetadata NormalTextureSampling;"),
              std::string::npos);
    EXPECT_NE(metadataSource.find("RayTracingMaterialTextureSamplingMetadata EmissiveTextureSampling;"),
              std::string::npos);
    EXPECT_NE(metadataSource.find("uint BaseColorTextureTableIndex;"), std::string::npos);
    EXPECT_NE(metadataSource.find("uint IndexBufferTableIndex;"), std::string::npos);
    EXPECT_NE(metadataSource.find("uint BaseColorSamplerFlags;"), std::string::npos);
    EXPECT_NE(metadataSource.find("uint NormalBufferTableIndex;"), std::string::npos);
    EXPECT_NE(metadataSource.find("uint TangentBufferTableIndex;"), std::string::npos);
    EXPECT_NE(metadataSource.find("float2 BaseColorUVOffset;"), std::string::npos);
    EXPECT_NE(metadataSource.find("float2 BaseColorUVScale;"), std::string::npos);
    EXPECT_NE(metadataSource.find("float BaseColorUVRotation;"), std::string::npos);
    EXPECT_NE(shaderSource.find("uint LoadAlphaIndex"), std::string::npos);
    EXPECT_NE(shaderSource.find("float2 TransformAlphaUV"), std::string::npos);
    EXPECT_NE(shaderSource.find("float SampleAlphaTexture"), std::string::npos);
    EXPECT_NE(shaderSource.find("float SampleMaterialBaseColorAlpha"), std::string::npos);
    EXPECT_NE(shaderSource.find("LoadMaterialBaseColorAlphaTexel"), std::string::npos);
    EXPECT_NE(shaderSource.find("WrapAlphaCoordinate"), std::string::npos);
    EXPECT_NE(metadataSource.find("RT_ALPHA_MAG_NEAREST"), std::string::npos);
    EXPECT_NE(metadataSource.find("RT_ALPHA_WRAP_T_MIRROR"), std::string::npos);
    EXPECT_NE(metadataSource.find("RT_ALPHA_INDEX_FORMAT_UINT16"), std::string::npos);
    EXPECT_NE(metadataSource.find("RT_ALPHA_HAS_NORMAL_BUFFER"), std::string::npos);
    EXPECT_NE(metadataSource.find("RT_ALPHA_HAS_TANGENT_BUFFER"), std::string::npos);
    EXPECT_NE(shaderSource.find("PrimitiveIndex() * 3u"), std::string::npos);
    EXPECT_NE(shaderSource.find("attributes.barycentrics"), std::string::npos);
    EXPECT_NE(shaderSource.find(".Load(int3(texel, 0)).a"), std::string::npos);
    EXPECT_NE(shaderSource.find("const RayTracingInstanceMaterialMetadata material = gInstanceMaterialMetadata[instanceId];"),
              std::string::npos);
    EXPECT_NE(shaderSource.find("material.BaseColorFactor.a"), std::string::npos);
    EXPECT_NE(shaderSource.find("material.MaterialFactors.z"), std::string::npos);
    EXPECT_NE(shaderSource.find("canSampleMaterialBaseColor"), std::string::npos);
    EXPECT_NE(shaderSource.find("SampleMaterialBaseColorAlpha(material.BaseColorTextureTableIndex, alpha, uv)"),
              std::string::npos);
    EXPECT_NE(shaderSource.find("cbuffer RayTracedShadowConstants : register(RVX_RT_SHADOW_CONSTANTS_REGISTER, space0)"),
              std::string::npos);
    EXPECT_NE(shaderSource.find("float4x4 gPreviousViewProjection;"), std::string::npos);
    EXPECT_NE(shaderSource.find("float4 gHistoryReprojectionParams;"), std::string::npos);
    EXPECT_NE(shaderSource.find("z: velocity available, w: velocity rejection scale"), std::string::npos);
    EXPECT_NE(shaderSource.find("float4 gSoftShadowParams;"), std::string::npos);
    EXPECT_NE(shaderSource.find("float4 gRayOptions;"), std::string::npos);
    EXPECT_NE(shaderSource.find("uint HashUInt(uint value)"), std::string::npos);
    EXPECT_NE(shaderSource.find("float RandomFloat01(uint seed)"), std::string::npos);
    EXPECT_NE(shaderSource.find("float3 BuildSoftShadowRayDirection(float3 lightDirection, uint2 pixel, uint sampleIndex)"),
              std::string::npos);
    EXPECT_NE(shaderSource.find("float TraceShadowVisibility"), std::string::npos);
    EXPECT_NE(shaderSource.find("((uint)gRayOptions.x) & 0xFFu"), std::string::npos);
    EXPECT_NE(shaderSource.find("sqrt(u0) * tan(min(angularRadius"), std::string::npos);
    EXPECT_NE(shaderSource.find("sampleIndex * 0x9e3779b9u"), std::string::npos);
    EXPECT_NE(shaderSource.find("TryReconstructWorldNormal"), std::string::npos);
    EXPECT_NE(shaderSource.find("EncodeNormalHistory"), std::string::npos);
    EXPECT_NE(shaderSource.find("DecodeNormalHistory"), std::string::npos);
    EXPECT_NE(shaderSource.find("bool TryLoadReprojectedHistory"), std::string::npos);
    EXPECT_NE(shaderSource.find("float ComputeVelocityHistoryWeight(uint2 pixel)"), std::string::npos);
    EXPECT_NE(shaderSource.find("gSceneVelocity.Load(int3(pixel, 0)).xy"), std::string::npos);
    EXPECT_NE(shaderSource.find("return saturate(1.0f - length(velocityNdc) * velocityRejectionScale);"),
              std::string::npos);
    EXPECT_NE(shaderSource.find("mul(gPreviousViewProjection"), std::string::npos);
    EXPECT_NE(shaderSource.find("gPreviousDepthHistory.Load"), std::string::npos);
    EXPECT_NE(shaderSource.find("gCurrentDepthHistory[pixel] = depth;"), std::string::npos);
    EXPECT_NE(shaderSource.find("gPreviousNormalHistory.Load"), std::string::npos);
    EXPECT_NE(shaderSource.find("gCurrentNormalHistory[pixel] = EncodeNormalHistory"), std::string::npos);
    EXPECT_NE(shaderSource.find("dot(previousNormal, currentNormal)"), std::string::npos);
    EXPECT_NE(shaderSource.find("const uint sampleCount = min(max((uint)gSoftShadowParams.z, 1u), 8u);"),
              std::string::npos);
    EXPECT_NE(shaderSource.find("for (uint sampleIndex = 0u; sampleIndex < sampleCount; ++sampleIndex)"),
              std::string::npos);
    EXPECT_NE(shaderSource.find("const float velocityHistoryWeight = ComputeVelocityHistoryWeight(pixel);"),
              std::string::npos);
    EXPECT_NE(shaderSource.find("const float historyWeight = saturate(gDepthAndBiasParams.w) * velocityHistoryWeight;"),
              std::string::npos);
    EXPECT_NE(shaderSource.find("lerp(visibility, previousVisibility, historyWeight)"), std::string::npos);
    EXPECT_NE(shaderSource.find("BuildSoftShadowRayDirection(baseRayDirection, pixel, sampleIndex)"),
              std::string::npos);
    EXPECT_NE(shaderSource.find("visibility *= 1.0f / (float)sampleCount;"),
              std::string::npos);
    EXPECT_NE(shaderSource.find("gDepthAndBiasParams.z > 0.5f"), std::string::npos);
    EXPECT_NE(shaderSource.find("lerp(visibility, previousVisibility"), std::string::npos);
    EXPECT_NE(shaderSource.find("ReconstructWorldPosition"), std::string::npos);
    EXPECT_NE(shaderSource.find("gSceneDepth.Load(int3(pixel, 0)).r"), std::string::npos);
    EXPECT_NE(shaderSource.find("ray.Origin = worldPosition + rayDirection * originBias;"), std::string::npos);
    EXPECT_NE(shaderSource.find("[shader(\"miss\")]"), std::string::npos);
    EXPECT_NE(shaderSource.find("[shader(\"anyhit\")]"), std::string::npos);
    EXPECT_NE(shaderSource.find("void ShadowAnyHit"), std::string::npos);
    EXPECT_NE(shaderSource.find("const uint instanceId = InstanceID();"), std::string::npos);
    EXPECT_NE(shaderSource.find("(material.Flags & RT_MATERIAL_SHADOW_CASTER) == 0u"), std::string::npos);
    EXPECT_NE(shaderSource.find("RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_FORCE_NON_OPAQUE"),
              std::string::npos);
    EXPECT_NE(shaderSource.find("gInstanceAlphaMetadata[instanceId]"), std::string::npos);
    EXPECT_NE(shaderSource.find("IgnoreHit();"), std::string::npos);
    EXPECT_NE(shaderSource.find("[shader(\"closesthit\")]"), std::string::npos);
}

TEST_F(PipelineCacheValidationFixture, RayTracedReflectionPipelineCacheResources)
{
    if (!HasCompilerAvailable())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    const fs::path renderRoot = FindShaderDirectory().parent_path();
    const std::string pipelineHeader = ReadTextFile(renderRoot / "Include" / "Render" / "PipelineCache.h");
    const std::string pipelineSource = ReadTextFile(renderRoot / "Private" / "PipelineCache.cpp");
    const std::string passHeader =
        ReadTextFile(renderRoot / "Include" / "Render" / "Passes" / "RayTracedReflectionPass.h");
    const std::string passSource =
        ReadTextFile(renderRoot / "Private" / "Passes" / "RayTracedReflectionPass.cpp");
    const std::string cameraVelocityPassHeader =
        ReadTextFile(renderRoot / "Include" / "Render" / "Passes" / "CameraVelocityPass.h");
    const std::string cameraVelocityPassSource =
        ReadTextFile(renderRoot / "Private" / "Passes" / "CameraVelocityPass.cpp");
    const std::string viewDataHeader = ReadTextFile(renderRoot / "Include" / "Render" / "Renderer" / "ViewData.h");
    const std::string compositePassHeader =
        ReadTextFile(renderRoot / "Include" / "Render" / "Passes" / "RayTracedReflectionCompositePass.h");
    const std::string compositePassSource =
        ReadTextFile(renderRoot / "Private" / "Passes" / "RayTracedReflectionCompositePass.cpp");
    const std::string denoisePassHeader =
        ReadTextFile(renderRoot / "Include" / "Render" / "Passes" / "RayTracedReflectionDenoisePass.h");
    const std::string denoisePassSource =
        ReadTextFile(renderRoot / "Private" / "Passes" / "RayTracedReflectionDenoisePass.cpp");
    const std::string shaderSource = ReadTextFile(FindShaderDirectory() / "RayTracing" / "RayTracedReflection.hlsl");
    const std::string metadataSource =
        ReadTextFile(FindShaderDirectory() / "RayTracing" / "RayTracingSceneMetadata.hlsli");
    const std::string compositeShaderSource =
        ReadTextFile(FindShaderDirectory() / "PostProcess" / "RayTracedReflectionComposite.hlsl");
    const std::string denoiseShaderSource =
        ReadTextFile(FindShaderDirectory() / "PostProcess" / "RayTracedReflectionDenoise.hlsl");
    const std::string cameraVelocityShaderSource =
        ReadTextFile(FindShaderDirectory() / "PostProcess" / "CameraVelocity.hlsl");

    EXPECT_NE(pipelineHeader.find("GetRayTracedReflectionPipeline()"), std::string::npos);
    EXPECT_NE(pipelineHeader.find("GetRayTracedReflectionShaderTable()"), std::string::npos);
    EXPECT_NE(pipelineHeader.find("GetRayTracedReflectionLayout()"), std::string::npos);
    EXPECT_NE(pipelineHeader.find("GetRayTracedReflectionSetLayout()"), std::string::npos);
    EXPECT_NE(pipelineHeader.find("GetRayTracedReflectionCompositePipeline()"), std::string::npos);
    EXPECT_NE(pipelineHeader.find("GetRayTracedReflectionCompositePipeline(RHIFormat outputFormat)"),
              std::string::npos);
    EXPECT_NE(pipelineHeader.find("GetRayTracedReflectionDenoisePipeline()"), std::string::npos);
    EXPECT_NE(pipelineHeader.find("GetRayTracedReflectionDenoisePipeline(RHIFormat outputFormat)"),
              std::string::npos);
    EXPECT_NE(pipelineHeader.find("GetCameraVelocityPipeline()"), std::string::npos);
    EXPECT_NE(pipelineHeader.find("GetCameraVelocityPipeline(RHIFormat outputFormat)"), std::string::npos);
    EXPECT_NE(pipelineHeader.find("GetRayTracedReflectionDenoiseSetLayout()"), std::string::npos);
    EXPECT_NE(pipelineHeader.find("CreateRayTracedReflectionPipelineLayout()"), std::string::npos);
    EXPECT_NE(pipelineHeader.find("CreateRayTracedReflectionDenoisePipelineLayout()"), std::string::npos);
    EXPECT_NE(pipelineHeader.find("CreateRayTracedReflectionPipeline()"), std::string::npos);
    EXPECT_NE(pipelineHeader.find("m_rayTracedReflectionRayGenShader"), std::string::npos);
    EXPECT_NE(pipelineHeader.find("m_rayTracedReflectionClosestHitShader"), std::string::npos);
    EXPECT_NE(pipelineHeader.find("m_rayTracedReflectionAnyHitShader"), std::string::npos);
    EXPECT_NE(pipelineHeader.find("m_rayTracedReflectionAnyHitCompileResult"), std::string::npos);
    EXPECT_NE(pipelineHeader.find("m_rayTracedReflectionShaderTable"), std::string::npos);
    EXPECT_NE(pipelineHeader.find("m_rayTracedReflectionCompositeVertexShader"), std::string::npos);
    EXPECT_NE(pipelineHeader.find("m_rayTracedReflectionCompositePixelShader"), std::string::npos);
    EXPECT_NE(pipelineHeader.find("m_rayTracedReflectionCompositePipeline"), std::string::npos);
    EXPECT_NE(pipelineHeader.find("m_rayTracedReflectionDenoiseVertexShader"), std::string::npos);
    EXPECT_NE(pipelineHeader.find("m_rayTracedReflectionDenoisePixelShader"), std::string::npos);
    EXPECT_NE(pipelineHeader.find("m_rayTracedReflectionDenoisePipeline"), std::string::npos);
    EXPECT_NE(pipelineHeader.find("m_cameraVelocityPixelShader"), std::string::npos);
    EXPECT_NE(pipelineHeader.find("m_cameraVelocityPipeline"), std::string::npos);
    EXPECT_NE(pipelineHeader.find("GetOrCreateCameraVelocityPipeline"), std::string::npos);
    EXPECT_NE(pipelineHeader.find("BuildCameraVelocityPipelineDesc"), std::string::npos);

    EXPECT_NE(pipelineSource.find("RayTracing/RayTracedReflection.hlsl"), std::string::npos);
    EXPECT_NE(pipelineSource.find("PostProcess/RayTracedReflectionComposite.hlsl"), std::string::npos);
    EXPECT_NE(pipelineSource.find("PostProcess/RayTracedReflectionDenoise.hlsl"), std::string::npos);
    EXPECT_NE(pipelineSource.find("PostProcess/CameraVelocity.hlsl"), std::string::npos);
    EXPECT_NE(pipelineSource.find("CameraVelocity shader file not found"), std::string::npos);
    EXPECT_NE(pipelineSource.find("Failed to compile CameraVelocity pixel shader"), std::string::npos);
    EXPECT_NE(pipelineSource.find("GetCameraVelocityPipeline(RHIFormat outputFormat)"), std::string::npos);
    EXPECT_NE(pipelineSource.find("GetOrCreateCameraVelocityPipeline"), std::string::npos);
    EXPECT_NE(pipelineSource.find("BuildCameraVelocityPipelineDesc"), std::string::npos);
    EXPECT_NE(pipelineSource.find("pipelineDesc.debugName = \"CameraVelocityPipeline\""), std::string::npos);
    EXPECT_NE(pipelineSource.find("pipelineDesc.pixelShader = m_cameraVelocityPixelShader.Get();"),
              std::string::npos);
    EXPECT_NE(pipelineSource.find("pipelineDesc.renderTargetFormats[0] = outputFormat;"), std::string::npos);
    EXPECT_NE(pipelineSource.find("ReflectionRayGen"), std::string::npos);
    EXPECT_NE(pipelineSource.find("ReflectionMiss"), std::string::npos);
    EXPECT_NE(pipelineSource.find("ReflectionClosestHit"), std::string::npos);
    EXPECT_NE(pipelineSource.find("reflectionAnyHitDesc.entryPoint = \"ReflectionAnyHit\""), std::string::npos);
    EXPECT_NE(pipelineSource.find("reflectionAnyHitDesc.stage = RHIShaderStage::AnyHit"), std::string::npos);
    EXPECT_NE(pipelineSource.find("CreateRayTracedReflectionPipelineLayout()"), std::string::npos);
    EXPECT_NE(pipelineSource.find("shaderTableDesc.rayTracingPipelineOwner = m_rayTracedReflectionPipeline;"),
              std::string::npos);
    EXPECT_NE(pipelineSource.find("setLayoutDesc.debugName = \"RayTracedReflectionSetLayout\""),
              std::string::npos);
    EXPECT_NE(pipelineSource.find("setLayoutDesc.AddBinding(RTReflectionBindings::RVX_RT_REFLECTION_TLAS_BINDING, RHIBindingType::AccelerationStructure"),
              std::string::npos);
    EXPECT_NE(pipelineSource.find("setLayoutDesc.AddBinding(RTReflectionBindings::RVX_RT_REFLECTION_OUTPUT_BINDING, RHIBindingType::StorageTexture"),
              std::string::npos);
    EXPECT_NE(pipelineSource.find("setLayoutDesc.AddBinding(RTReflectionBindings::RVX_RT_REFLECTION_MATERIAL_METADATA_BINDING, RHIBindingType::ShaderResourceBuffer"),
              std::string::npos);
    EXPECT_NE(pipelineSource.find("setLayoutDesc.AddBinding(RTReflectionBindings::RVX_RT_REFLECTION_MATERIAL_TEXTURES_BINDING, RHIBindingType::SampledTexture"),
              std::string::npos);
    EXPECT_NE(pipelineSource.find("setLayoutDesc.AddBinding(RTReflectionBindings::RVX_RT_REFLECTION_PREVIOUS_HISTORY_BINDING, RHIBindingType::SampledTexture"),
              std::string::npos);
    EXPECT_NE(pipelineSource.find("setLayoutDesc.AddBinding(RTReflectionBindings::RVX_RT_REFLECTION_OUTPUT_DEPTH_BINDING, RHIBindingType::StorageTexture"),
              std::string::npos);
    EXPECT_NE(pipelineSource.find("setLayoutDesc.AddBinding(RTReflectionBindings::RVX_RT_REFLECTION_OUTPUT_NORMAL_BINDING, RHIBindingType::StorageTexture"),
              std::string::npos);
    EXPECT_NE(pipelineSource.find("setLayoutDesc.AddBinding(RTReflectionBindings::RVX_RT_REFLECTION_GEOMETRY_METADATA_BINDING, RHIBindingType::ShaderResourceBuffer"),
              std::string::npos);
    EXPECT_NE(pipelineSource.find("setLayoutDesc.AddBinding(RTReflectionBindings::RVX_RT_REFLECTION_INDEX_BUFFERS_BINDING, RHIBindingType::ShaderResourceBuffer, RHIShaderStage::AllRayTracing, RTReflectionBindings::RVX_RT_REFLECTION_MAX_GEOMETRY_BUFFERS)"),
              std::string::npos);
    EXPECT_NE(pipelineSource.find("setLayoutDesc.AddBinding(RTReflectionBindings::RVX_RT_REFLECTION_UV_BUFFERS_BINDING, RHIBindingType::ShaderResourceBuffer, RHIShaderStage::AllRayTracing, RTReflectionBindings::RVX_RT_REFLECTION_MAX_GEOMETRY_BUFFERS)"),
              std::string::npos);
    EXPECT_NE(pipelineSource.find("setLayoutDesc.AddBinding(RTReflectionBindings::RVX_RT_REFLECTION_NORMAL_BUFFERS_BINDING, RHIBindingType::ShaderResourceBuffer, RHIShaderStage::AllRayTracing, RTReflectionBindings::RVX_RT_REFLECTION_MAX_GEOMETRY_BUFFERS)"),
              std::string::npos);
    EXPECT_NE(pipelineSource.find("setLayoutDesc.AddBinding(RTReflectionBindings::RVX_RT_REFLECTION_TANGENT_BUFFERS_BINDING, RHIBindingType::ShaderResourceBuffer, RHIShaderStage::AllRayTracing, RTReflectionBindings::RVX_RT_REFLECTION_MAX_GEOMETRY_BUFFERS)"),
              std::string::npos);
    EXPECT_NE(pipelineSource.find("setLayoutDesc.AddBinding(RTReflectionBindings::RVX_RT_REFLECTION_SCENE_VELOCITY_BINDING, RHIBindingType::SampledTexture"),
              std::string::npos);
    EXPECT_NE(pipelineSource.find("pipelineDesc.debugName = \"RayTracedReflectionPipeline\""),
              std::string::npos);
    EXPECT_NE(pipelineSource.find("pipelineDesc.maxPayloadSize = sizeof(Vec4) + sizeof(float);"),
              std::string::npos);
    EXPECT_NE(pipelineSource.find("hitGroup.exportName = \"ReflectionHitGroup\""), std::string::npos);
    EXPECT_NE(pipelineSource.find("hitGroup.anyHitShader = m_rayTracedReflectionAnyHitShader.Get();"),
              std::string::npos);
    EXPECT_NE(pipelineSource.find("shaderTableDesc.debugName = \"RayTracedReflectionShaderTable\""),
              std::string::npos);
    EXPECT_NE(pipelineSource.find("BuildRayTracedReflectionCompositePipelineDesc"), std::string::npos);
    EXPECT_NE(pipelineSource.find("pipelineDesc.debugName = \"RayTracedReflectionCompositePipeline\""),
              std::string::npos);
    EXPECT_NE(pipelineSource.find("BuildRayTracedReflectionDenoisePipelineDesc"), std::string::npos);
    EXPECT_NE(pipelineSource.find("setLayoutDesc.debugName = \"RayTracedReflectionDenoiseSetLayout\""),
              std::string::npos);
    EXPECT_NE(pipelineSource.find("setLayoutDesc.AddBinding(2, RHIBindingType::SampledTexture, RHIShaderStage::Pixel);"),
              std::string::npos);
    EXPECT_NE(pipelineSource.find("setLayoutDesc.AddBinding(3, RHIBindingType::SampledTexture, RHIShaderStage::Pixel);"),
              std::string::npos);
    EXPECT_NE(pipelineSource.find("pipelineDesc.debugName = \"RayTracedReflectionDenoisePipeline\""),
              std::string::npos);
    EXPECT_NE(pipelineSource.find("pipelineDesc.pipelineLayout = m_rayTracedReflectionDenoisePipelineLayout.Get();"),
              std::string::npos);
    EXPECT_NE(pipelineSource.find("pipelineDesc.blendState.renderTargets[0].srcColorBlend = RHIBlendFactor::SrcAlpha"),
              std::string::npos);
    EXPECT_NE(pipelineSource.find("pipelineDesc.blendState.renderTargets[0].dstColorBlend = RHIBlendFactor::InvSrcAlpha"),
              std::string::npos);

    EXPECT_NE(passHeader.find("class RayTracedReflectionPass : public IRenderPass"), std::string::npos);
    EXPECT_NE(passHeader.find("struct RayTracedReflectionPassConfig"), std::string::npos);
    EXPECT_NE(passHeader.find("void SetConfig(const RayTracedReflectionPassConfig& config)"), std::string::npos);
    EXPECT_NE(passHeader.find("float resolutionScale = 1.0f;"), std::string::npos);
    EXPECT_NE(passHeader.find("uint32 samplesPerPixel = 1;"), std::string::npos);
    EXPECT_NE(passHeader.find("float intensity = 1.0f;"), std::string::npos);
    EXPECT_NE(passHeader.find("float maxTraceDistance = 0.0f;"), std::string::npos);
    EXPECT_NE(passHeader.find("float maxRoughness = 1.0f;"), std::string::npos);
    EXPECT_NE(passHeader.find("float distanceFadeStart = 0.8f;"), std::string::npos);
    EXPECT_NE(passHeader.find("float roughnessConeSpread = 1.0f;"), std::string::npos);
    EXPECT_NE(passHeader.find("float normalBias = 0.02f;"), std::string::npos);
    EXPECT_NE(passHeader.find("float rayMinT = 0.001f;"), std::string::npos);
    EXPECT_NE(passHeader.find("float fireflyClamp = 64.0f;"), std::string::npos);
    EXPECT_NE(passHeader.find("float temporalBlendFactor = 0.0f;"), std::string::npos);
    EXPECT_NE(passHeader.find("float historyDepthThreshold = 0.01f;"), std::string::npos);
    EXPECT_NE(passHeader.find("float historyNormalThreshold = 0.85f;"), std::string::npos);
    EXPECT_NE(passHeader.find("float historyLuminanceTolerance = 4.0f;"), std::string::npos);
    EXPECT_NE(passHeader.find("float historyConfidenceThreshold = 0.05f;"), std::string::npos);
    EXPECT_NE(passHeader.find("float historyVelocityRejectionScale = 0.0f;"), std::string::npos);
    EXPECT_NE(passHeader.find("bool gpuTimingSupported = false;"), std::string::npos);
    EXPECT_NE(passHeader.find("bool gpuTimingQueriesRecorded = false;"), std::string::npos);
    EXPECT_NE(passHeader.find("bool gpuTimingResolveRecorded = false;"), std::string::npos);
    EXPECT_NE(passHeader.find("bool gpuTimingReadbackBufferAvailable = false;"), std::string::npos);
    EXPECT_NE(passHeader.find("bool gpuTimingResultAvailable = false;"), std::string::npos);
    EXPECT_NE(passHeader.find("uint64 gpuTimestampFrequency = 0;"), std::string::npos);
    EXPECT_NE(passHeader.find("uint64 gpuTimingReadbackBytes = 0;"), std::string::npos);
    EXPECT_NE(passHeader.find("uint64 gpuTimingElapsedTicks = 0;"), std::string::npos);
    EXPECT_NE(passHeader.find("float gpuTimingElapsedMs = 0.0f;"), std::string::npos);
    EXPECT_NE(passHeader.find("uint32 gpuTimingReadbackBufferCount = 0;"), std::string::npos);
    EXPECT_NE(passHeader.find("uint32 gpuTimingReadbackFrameIndex = RVX_INVALID_INDEX;"), std::string::npos);
    EXPECT_NE(passHeader.find("RHIQueryPoolRef m_timingQueryPool;"), std::string::npos);
    EXPECT_NE(passHeader.find("std::array<RHIBufferRef, RVX_MAX_FRAME_COUNT> m_timingReadbackBuffers;"),
              std::string::npos);
    EXPECT_NE(passHeader.find("std::array<bool, RVX_MAX_FRAME_COUNT> m_timingReadbackValid{};"),
              std::string::npos);
    EXPECT_NE(passHeader.find("float distanceFadeStart = 0.8f;"), std::string::npos);
    EXPECT_NE(passHeader.find("uint32 samplesPerPixel = 1;"), std::string::npos);
    EXPECT_NE(passHeader.find("float roughnessConeSpread = 1.0f;"), std::string::npos);
    EXPECT_NE(passHeader.find("float normalBias = 0.02f;"), std::string::npos);
    EXPECT_NE(passHeader.find("float rayMinT = 0.001f;"), std::string::npos);
    EXPECT_NE(passHeader.find("float fireflyClamp = 64.0f;"), std::string::npos);
    EXPECT_NE(passHeader.find("GetCurrentNormalHistoryHandle() const"), std::string::npos);
    EXPECT_NE(passHeader.find("bool materialMetadataAvailable = false;"), std::string::npos);
    EXPECT_NE(passHeader.find("bool materialTextureTableAvailable = false;"), std::string::npos);
    EXPECT_NE(passHeader.find("bool velocityAvailable = false;"), std::string::npos);
    EXPECT_NE(passHeader.find("bool geometryMetadataAvailable = false;"), std::string::npos);
    EXPECT_NE(passHeader.find("bool geometryTableAvailable = false;"), std::string::npos);
    EXPECT_NE(passHeader.find("uint32 geometryIndexBufferCount = 0;"), std::string::npos);
    EXPECT_NE(passHeader.find("uint32 geometryUVBufferCount = 0;"), std::string::npos);
    EXPECT_NE(passHeader.find("uint32 geometryNormalBufferCount = 0;"), std::string::npos);
    EXPECT_NE(passHeader.find("uint32 geometryTangentBufferCount = 0;"), std::string::npos);
    EXPECT_NE(passHeader.find("bool historyAvailable = false;"), std::string::npos);
    EXPECT_NE(passHeader.find("bool resourceViewsAvailable = false;"), std::string::npos);
    EXPECT_NE(passHeader.find("bool descriptorSetAvailable = false;"), std::string::npos);
    EXPECT_NE(passHeader.find("bool historyReset = false;"), std::string::npos);
    EXPECT_NE(passHeader.find("bool historyRecreated = false;"), std::string::npos);
    EXPECT_NE(passHeader.find("bool historyResolutionChanged = false;"), std::string::npos);
    EXPECT_NE(passHeader.find("bool historyConfigChanged = false;"), std::string::npos);
    EXPECT_NE(passHeader.find("bool temporalAccumulated = false;"), std::string::npos);
    EXPECT_NE(passHeader.find("bool temporalAccumulation = true;"), std::string::npos);
    EXPECT_NE(passHeader.find("float temporalBlendFactor = 0.85f;"), std::string::npos);
    EXPECT_NE(passHeader.find("float historyLuminanceTolerance = 4.0f;"), std::string::npos);
    EXPECT_NE(passHeader.find("float historyConfidenceThreshold = 0.05f;"), std::string::npos);
    EXPECT_NE(passHeader.find("float historyVelocityRejectionScale = 8.0f;"), std::string::npos);
    EXPECT_NE(passHeader.find("std::array<RHITextureRef, 2> m_historyTextures;"), std::string::npos);
    EXPECT_NE(passHeader.find("RayTracedReflectionPassConfig m_lastHistoryConfig;"), std::string::npos);
    EXPECT_NE(passHeader.find("bool m_lastHistoryConfigValid = false;"), std::string::npos);
    EXPECT_NE(passHeader.find("RGTextureHandle m_velocityReadHandle;"), std::string::npos);
    EXPECT_NE(passHeader.find("RHITextureRef m_fallbackVelocityTexture;"), std::string::npos);
    EXPECT_NE(passHeader.find("bool EnsureFallbackVelocityTexture();"), std::string::npos);
    EXPECT_NE(passHeader.find("bool ResolveMaterialTextureViews(std::vector<RHITextureView*>& outViews) const;"),
              std::string::npos);
    EXPECT_NE(passHeader.find("bool ResolveGeometryBuffers(std::vector<RHIBuffer*>& outIndexBuffers"),
              std::string::npos);
    EXPECT_NE(passSource.find("GetRayTracedReflectionPipeline()"), std::string::npos);
    EXPECT_NE(passSource.find("GetRayTracedReflectionShaderTable()"), std::string::npos);
    EXPECT_NE(passSource.find("descriptorDesc.BindAccelerationStructure(RTReflectionBindings::RVX_RT_REFLECTION_TLAS_BINDING, tlas);"), std::string::npos);
    EXPECT_NE(passSource.find("descriptorDesc.BindTexture(RTReflectionBindings::RVX_RT_REFLECTION_OUTPUT_BINDING, reflectionUAV);"), std::string::npos);
    EXPECT_NE(passSource.find("descriptorDesc.BindTexture(RTReflectionBindings::RVX_RT_REFLECTION_SCENE_COLOR_BINDING, sceneColorSRV);"), std::string::npos);
    EXPECT_NE(passSource.find("descriptorDesc.BindTexture(RTReflectionBindings::RVX_RT_REFLECTION_SCENE_DEPTH_BINDING, sceneDepthSRV);"), std::string::npos);
    EXPECT_NE(passSource.find("descriptorDesc.BindBuffer(RTReflectionBindings::RVX_RT_REFLECTION_CONSTANTS_BINDING,"), std::string::npos);
    EXPECT_NE(passSource.find("descriptorDesc.BindBuffer(RTReflectionBindings::RVX_RT_REFLECTION_MATERIAL_METADATA_BINDING, materialMetadataBuffer);"), std::string::npos);
    EXPECT_NE(passSource.find("descriptorDesc.BindTexture(RTReflectionBindings::RVX_RT_REFLECTION_MATERIAL_TEXTURES_BINDING, materialTextureViews[textureIndex], textureIndex);"),
              std::string::npos);
    EXPECT_NE(passSource.find("descriptorDesc.BindTexture(RTReflectionBindings::RVX_RT_REFLECTION_PREVIOUS_HISTORY_BINDING, previousReflectionSRV);"), std::string::npos);
    EXPECT_NE(passSource.find("descriptorDesc.BindTexture(RTReflectionBindings::RVX_RT_REFLECTION_OUTPUT_DEPTH_BINDING, currentDepthHistoryUAV);"), std::string::npos);
    EXPECT_NE(passSource.find("descriptorDesc.BindTexture(RTReflectionBindings::RVX_RT_REFLECTION_OUTPUT_NORMAL_BINDING, currentNormalHistoryUAV);"), std::string::npos);
    EXPECT_NE(passSource.find("descriptorDesc.BindTexture(RTReflectionBindings::RVX_RT_REFLECTION_SCENE_VELOCITY_BINDING, sceneVelocitySRV);"), std::string::npos);
    EXPECT_NE(passSource.find("m_stats.descriptorSetAvailable = true;"), std::string::npos);
    const auto reflectionResourceViewsReady = passSource.find("m_stats.resourceViewsAvailable = true;");
    const auto reflectionCreateDescriptorSet = passSource.find("device->CreateDescriptorSet(descriptorDesc);");
    const auto reflectionDescriptorSetReady = passSource.find("m_stats.descriptorSetAvailable = true;");
    const auto reflectionSetPipeline = passSource.find("ctx.SetPipeline(pipeline);");
    ASSERT_NE(reflectionResourceViewsReady, std::string::npos);
    ASSERT_NE(reflectionCreateDescriptorSet, std::string::npos);
    ASSERT_NE(reflectionDescriptorSetReady, std::string::npos);
    ASSERT_NE(reflectionSetPipeline, std::string::npos);
    EXPECT_LT(reflectionResourceViewsReady, reflectionCreateDescriptorSet);
    EXPECT_LT(reflectionCreateDescriptorSet, reflectionDescriptorSetReady);
    EXPECT_LT(reflectionDescriptorSetReady, reflectionSetPipeline);
    EXPECT_NE(passSource.find("RHIBuffer* geometryMetadataBuffer = m_sceneManager->GetInstanceAlphaMetadataBuffer();"),
              std::string::npos);
    EXPECT_NE(passSource.find("std::vector<RHIBuffer*> geometryTangentBuffers;"), std::string::npos);
    EXPECT_NE(passSource.find("geometryTangentBuffers))"),
              std::string::npos);
    EXPECT_NE(passSource.find("m_stats.resourceViewsAvailable = true;"), std::string::npos);
    EXPECT_NE(passSource.find("m_sceneManager->GetInstanceAlphaTangentBufferTable()"), std::string::npos);
    EXPECT_NE(passSource.find("descriptorDesc.BindBuffer(RTReflectionBindings::RVX_RT_REFLECTION_GEOMETRY_METADATA_BINDING, geometryMetadataBuffer);"), std::string::npos);
    EXPECT_NE(passSource.find("descriptorDesc.BindBuffer(RTReflectionBindings::RVX_RT_REFLECTION_INDEX_BUFFERS_BINDING, geometryIndexBuffers[bufferIndex], 0, RVX_WHOLE_SIZE, bufferIndex);"),
              std::string::npos);
    EXPECT_NE(passSource.find("descriptorDesc.BindBuffer(RTReflectionBindings::RVX_RT_REFLECTION_UV_BUFFERS_BINDING, geometryUVBuffers[bufferIndex], 0, RVX_WHOLE_SIZE, bufferIndex);"),
              std::string::npos);
    EXPECT_NE(passSource.find("descriptorDesc.BindBuffer(RTReflectionBindings::RVX_RT_REFLECTION_NORMAL_BUFFERS_BINDING, geometryNormalBuffers[bufferIndex], 0, RVX_WHOLE_SIZE, bufferIndex);"),
              std::string::npos);
    EXPECT_NE(passSource.find("descriptorDesc.BindBuffer(RTReflectionBindings::RVX_RT_REFLECTION_TANGENT_BUFFERS_BINDING, geometryTangentBuffers[bufferIndex], 0, RVX_WHOLE_SIZE, bufferIndex);"),
              std::string::npos);
    EXPECT_NE(passSource.find("m_stats.geometryTangentBufferCount"), std::string::npos);
    EXPECT_NE(passSource.find("m_config.maxTraceDistance"), std::string::npos);
    EXPECT_NE(passSource.find("m_config.maxRoughness"), std::string::npos);
    EXPECT_NE(passSource.find("m_config.instanceMask"), std::string::npos);
    EXPECT_NE(passSource.find("ResolveReflectionResolutionScale(m_config.resolutionScale)"), std::string::npos);
    EXPECT_NE(passSource.find("ResolveScaledReflectionDimension(view.viewportWidth, resolutionScale)"),
              std::string::npos);
    EXPECT_NE(passSource.find("EnsureHistoryTextures(outputWidth, outputHeight)"), std::string::npos);
    EXPECT_NE(passSource.find("m_stats.samplesPerPixel = ResolveReflectionSamplesPerPixel(m_config.samplesPerPixel);"),
              std::string::npos);
    EXPECT_NE(passSource.find("m_stats.intensity = ClampFiniteRange(m_config.intensity, 1.0f, 0.0f, 1.0f);"),
              std::string::npos);
    EXPECT_NE(passSource.find("m_stats.maxTraceDistance = ResolveReflectionMaxTraceDistance(m_config.maxTraceDistance, view.farPlane);"),
              std::string::npos);
    EXPECT_NE(passSource.find("m_stats.maxRoughness = ClampFiniteRange(m_config.maxRoughness, 1.0f, 0.01f, 1.0f);"),
              std::string::npos);
    EXPECT_NE(passSource.find("m_stats.fireflyClamp = ClampFiniteNonNegative(m_config.fireflyClamp, 64.0f);"),
              std::string::npos);
    EXPECT_NE(passSource.find("m_stats.historyConfidenceThreshold ="),
              std::string::npos);
    EXPECT_NE(passSource.find("m_stats.temporalBlendFactor = ClampFiniteRange(m_config.temporalBlendFactor, 0.85f, 0.0f, 0.95f);"),
              std::string::npos);
    EXPECT_NE(passSource.find("m_stats.historyVelocityRejectionScale ="),
              std::string::npos);
    EXPECT_NE(passSource.find("TryGetRHIRayTracingDispatchRayCount(outputWidth, outputHeight, 1, m_stats.dispatchPixelCount)"),
              std::string::npos);
    EXPECT_NE(passSource.find("TryMultiplyRHIRayTracingCount(m_stats.dispatchPixelCount"),
              std::string::npos);
    EXPECT_NE(passSource.find("RVX_RAY_TRACED_REFLECTION_TIMING_QUERY_COUNT_PER_FRAME = 2"),
              std::string::npos);
    EXPECT_NE(passSource.find("queryDesc.count = RVX_MAX_FRAME_COUNT * RVX_RAY_TRACED_REFLECTION_TIMING_QUERY_COUNT_PER_FRAME;"),
              std::string::npos);
    EXPECT_NE(passSource.find("queryDesc.debugName = \"RayTracedReflectionTimingQueries\";"), std::string::npos);
    EXPECT_NE(passSource.find("RVX_RAY_TRACED_REFLECTION_TIMING_READBACK_BYTES = sizeof(uint64) * 2"),
              std::string::npos);
    EXPECT_NE(passSource.find("readbackDesc.usage = RHIBufferUsage::CopyDst;"), std::string::npos);
    EXPECT_NE(passSource.find("readbackDesc.memoryType = RHIMemoryType::Readback;"), std::string::npos);
    EXPECT_NE(passSource.find("readbackDesc.debugName = \"RayTracedReflectionTimingReadback\";"), std::string::npos);
    EXPECT_NE(passSource.find("m_stats.gpuTimingSupported = m_timingQueryPool != nullptr;"), std::string::npos);
    EXPECT_NE(passSource.find("m_stats.gpuTimingReadbackBufferAvailable = timingReadbackBuffer != nullptr;"),
              std::string::npos);
    EXPECT_NE(passSource.find("m_stats.gpuTimingReadbackBytes = static_cast<uint64>(timingReadbackBufferCount) *"),
              std::string::npos);
    EXPECT_NE(passSource.find("m_stats.gpuTimestampFrequency = m_timingQueryPool ? m_timingQueryPool->GetTimestampFrequency() : 0;"),
              std::string::npos);
    EXPECT_NE(passSource.find("ctx.WriteTimestamp(m_timingQueryPool.Get(), timingStartQuery);"),
              std::string::npos);
    EXPECT_NE(passSource.find("ctx.WriteTimestamp(m_timingQueryPool.Get(), timingEndQuery);"),
              std::string::npos);
    EXPECT_NE(passSource.find("m_stats.gpuTimingQueriesRecorded = true;"), std::string::npos);
    EXPECT_NE(passSource.find("ctx.ResolveQueries(m_timingQueryPool.Get(),"), std::string::npos);
    EXPECT_NE(passSource.find("m_timingReadbackValid[timingFrameIndex] = true;"), std::string::npos);
    EXPECT_NE(passSource.find("m_stats.gpuTimingResolveRecorded = true;"), std::string::npos);
    EXPECT_NE(passSource.find("TryReadbackTimingResult(timingFrameIndex);"), std::string::npos);
    EXPECT_NE(passSource.find("m_stats.gpuTimingElapsedMs = static_cast<float>("), std::string::npos);
    EXPECT_NE(passSource.find("m_stats.velocityAvailable = view.velocityTarget.IsValid();"), std::string::npos);
    EXPECT_NE(passSource.find("m_velocityReadHandle = builder.Read(view.velocityTarget, RHIShaderStage::AllRayTracing);"),
              std::string::npos);
    EXPECT_NE(passSource.find("RHITexture* sceneVelocity = m_velocityReadHandle.IsValid() ?"),
              std::string::npos);
    EXPECT_NE(passSource.find("EnsureFallbackVelocityTexture()"), std::string::npos);
    EXPECT_NE(passSource.find("RHITextureDesc::Texture2D(1, 1, RHIFormat::RG16_FLOAT)"), std::string::npos);
    EXPECT_NE(passSource.find("RayTracedReflectionFallbackVelocity"), std::string::npos);
    EXPECT_NE(passSource.find("sceneVelocity = m_fallbackVelocityTexture.Get();"), std::string::npos);
    EXPECT_NE(passSource.find("m_viewCache->GetDefaultSRV(sceneVelocity)"), std::string::npos);
    EXPECT_EQ(passSource.find("sceneVelocity ? m_viewCache->GetDefaultSRV(sceneVelocity) : sceneColorSRV"),
              std::string::npos);
    EXPECT_NE(passSource.find("m_stats.historyReset = view.resetTemporalHistory;"), std::string::npos);
    EXPECT_NE(passSource.find("m_stats.historyRecreated = true;"), std::string::npos);
    EXPECT_NE(passSource.find("ReflectionHistoryConfigChanged(m_lastHistoryConfig, m_config)"),
              std::string::npos);
    EXPECT_NE(passSource.find("m_stats.historyConfigChanged = historyConfigChanged;"), std::string::npos);
    EXPECT_NE(passSource.find("m_lastHistoryConfig = m_config;"), std::string::npos);
    EXPECT_NE(passSource.find("m_lastHistoryConfigValid = true;"), std::string::npos);
    EXPECT_NE(passSource.find("m_stats.historyResolutionChanged = historyResolutionChanged;"),
              std::string::npos);
    EXPECT_NE(passSource.find("m_stats.historyAvailable = false;"), std::string::npos);
    EXPECT_NE(passSource.find("m_viewCache->InvalidateTexture(texture.Get())"), std::string::npos);
    const auto reflectionOnAdd = passSource.find("void RayTracedReflectionPass::OnAdd(IRHIDevice* device)");
    ASSERT_NE(reflectionOnAdd, std::string::npos);
    const auto reflectionDeviceChange = passSource.find("if (m_device != device)", reflectionOnAdd);
    ASSERT_NE(reflectionDeviceChange, std::string::npos);
    const auto reflectionResetHistoryOnDeviceChange = passSource.find("ResetHistoryTextures();", reflectionDeviceChange);
    const auto reflectionConstantBufferResetOnDeviceChange = passSource.find("m_constantBuffer.Reset();", reflectionDeviceChange);
    const auto reflectionRetainedDescriptorsClearOnDeviceChange =
        passSource.find("m_retainedDescriptorSets.clear();", reflectionDeviceChange);
    const auto reflectionDeviceAssign = passSource.find("m_device = device;", reflectionDeviceChange);
    ASSERT_NE(reflectionResetHistoryOnDeviceChange, std::string::npos);
    ASSERT_NE(reflectionConstantBufferResetOnDeviceChange, std::string::npos);
    ASSERT_NE(reflectionRetainedDescriptorsClearOnDeviceChange, std::string::npos);
    ASSERT_NE(reflectionDeviceAssign, std::string::npos);
    EXPECT_LT(reflectionResetHistoryOnDeviceChange, reflectionConstantBufferResetOnDeviceChange);
    EXPECT_LT(reflectionConstantBufferResetOnDeviceChange, reflectionRetainedDescriptorsClearOnDeviceChange);
    EXPECT_LT(reflectionRetainedDescriptorsClearOnDeviceChange, reflectionDeviceAssign);

    const auto reflectionOnRemove = passSource.find("void RayTracedReflectionPass::OnRemove()");
    ASSERT_NE(reflectionOnRemove, std::string::npos);
    const auto reflectionResetHistoryOnRemove = passSource.find("ResetHistoryTextures();", reflectionOnRemove);
    const auto reflectionClearViewCacheOnRemove = passSource.find("m_viewCache = nullptr;", reflectionOnRemove);
    ASSERT_NE(reflectionResetHistoryOnRemove, std::string::npos);
    ASSERT_NE(reflectionClearViewCacheOnRemove, std::string::npos);
    EXPECT_LT(reflectionResetHistoryOnRemove, reflectionClearViewCacheOnRemove);
    EXPECT_NE(passSource.find("if (view.resetTemporalHistory)"), std::string::npos);
    EXPECT_NE(passSource.find("m_historyValid = false;"), std::string::npos);
    EXPECT_NE(passSource.find("m_historyViewValid = false;"), std::string::npos);
    EXPECT_NE(passSource.find("m_config.temporalAccumulation"), std::string::npos);
    EXPECT_NE(passSource.find("constants.previousViewProjection"), std::string::npos);
    EXPECT_NE(passSource.find("constants.outputSizeAndInvSize ="), std::string::npos);
    EXPECT_NE(passSource.find("constants.sceneSizeAndInvSize ="), std::string::npos);
    EXPECT_NE(passSource.find("m_config.distanceFadeStart"), std::string::npos);
    EXPECT_NE(passSource.find("constants.reflectionOptions = Vec4(intensity, maxRoughness, static_cast<float>(rayMask), distanceFadeStart);"),
              std::string::npos);
    EXPECT_NE(passSource.find("Vec4 stochasticParams{1.0f, 0.0f, 1.0f, 0.0f};"), std::string::npos);
    EXPECT_NE(passSource.find("Vec4 rayBiasParams{0.02f, 0.001f, 64.0f, 0.0f};"), std::string::npos);
    EXPECT_NE(passSource.find("Vec4 historyClampParams{4.0f, 0.05f, 0.0f, 8.0f};"), std::string::npos);
    EXPECT_NE(passSource.find("ResolveReflectionSamplesPerPixel(m_config.samplesPerPixel)"), std::string::npos);
    EXPECT_NE(passSource.find("ClampFiniteRange(m_config.intensity, 1.0f, 0.0f, 1.0f)"), std::string::npos);
    EXPECT_NE(passSource.find("ResolveReflectionMaxTraceDistance(m_config.maxTraceDistance, view.farPlane)"),
              std::string::npos);
    EXPECT_NE(passSource.find("m_config.roughnessConeSpread"), std::string::npos);
    EXPECT_NE(passSource.find("m_config.normalBias"), std::string::npos);
    EXPECT_NE(passSource.find("m_config.rayMinT"), std::string::npos);
    EXPECT_NE(passSource.find("m_config.fireflyClamp"), std::string::npos);
    EXPECT_NE(passSource.find("m_config.historyLuminanceTolerance"), std::string::npos);
    EXPECT_NE(passSource.find("m_config.historyConfidenceThreshold"), std::string::npos);
    EXPECT_NE(passSource.find("m_config.historyVelocityRejectionScale"), std::string::npos);
    EXPECT_NE(passSource.find("constants.stochasticParams ="), std::string::npos);
    EXPECT_NE(passSource.find("constants.rayBiasParams = Vec4(normalBias, rayMinT, fireflyClamp, 0.0f);"),
              std::string::npos);
    EXPECT_NE(passSource.find("constants.historyClampParams ="), std::string::npos);
    EXPECT_NE(passSource.find("view.velocityTarget.IsValid() ? 1.0f : 0.0f"), std::string::npos);
    EXPECT_NE(passSource.find("historyVelocityRejectionScale);"), std::string::npos);
    EXPECT_NE(passSource.find("constants.historyParams ="), std::string::npos);
    EXPECT_NE(passSource.find("m_stats.temporalAccumulated = m_temporalAccumulatedThisFrame;"),
              std::string::npos);
    EXPECT_NE(passSource.find("ctx.DispatchRays(dispatchDesc);"), std::string::npos);

    EXPECT_NE(cameraVelocityPassHeader.find("struct CameraVelocityPassStats"), std::string::npos);
    EXPECT_NE(cameraVelocityPassHeader.find("bool previousViewProjectionAvailable = false;"), std::string::npos);
    EXPECT_NE(cameraVelocityPassHeader.find("class CameraVelocityPass : public IRenderPass"), std::string::npos);
    EXPECT_NE(cameraVelocityPassHeader.find("PassPriority::PostProcess - 300"), std::string::npos);
    EXPECT_NE(cameraVelocityPassHeader.find("void SetEnabled(bool enabled)"), std::string::npos);
    EXPECT_NE(cameraVelocityPassSource.find("GetCameraVelocityPipeline(RHIFormat::RG16_FLOAT)"),
              std::string::npos);
    EXPECT_NE(cameraVelocityPassSource.find("view.previousViewProjectionValid != 0 && !view.resetTemporalHistory"),
              std::string::npos);
    EXPECT_NE(cameraVelocityPassSource.find("m_depthReadHandle = builder.Read(depthHandle, RHIShaderStage::Pixel);"),
              std::string::npos);
    EXPECT_NE(cameraVelocityPassSource.find("m_velocityWriteHandle = builder.Write(view.velocityTarget, RHIResourceState::RenderTarget);"),
              std::string::npos);
    EXPECT_NE(cameraVelocityPassSource.find("descriptorDesc.debugName = \"CameraVelocityDescriptorSet\""),
              std::string::npos);
    EXPECT_NE(cameraVelocityPassSource.find("descriptorDesc.BindTexture(1, depthView);"), std::string::npos);
    EXPECT_NE(cameraVelocityPassSource.find("RHIFormat::RG16_FLOAT"), std::string::npos);
    EXPECT_NE(cameraVelocityPassSource.find("constants.inverseViewProjection = view.inverseViewMatrix * view.inverseProjectionMatrix;"),
              std::string::npos);
    EXPECT_NE(cameraVelocityPassSource.find("constants.previousViewProjection = previousValid ? view.previousViewProjectionMatrix : view.viewProjectionMatrix;"),
              std::string::npos);
    EXPECT_NE(cameraVelocityPassSource.find("Vec4(previousValid ? 1.0f : 0.0f"), std::string::npos);
    EXPECT_NE(cameraVelocityPassSource.find("ctx.Draw(3, 1, 0, 0);"), std::string::npos);

    EXPECT_NE(cameraVelocityShaderSource.find("cbuffer CameraVelocityConstants : register(b0, space0)"),
              std::string::npos);
    EXPECT_NE(cameraVelocityShaderSource.find("Texture2D<float> DepthTexture : register(t1, space0);"),
              std::string::npos);
    EXPECT_NE(cameraVelocityShaderSource.find("VelocityParams.x <= 0.5f || depth >= VelocityParams.z"),
              std::string::npos);
    EXPECT_NE(cameraVelocityShaderSource.find("const float2 currentNdc"), std::string::npos);
    EXPECT_NE(cameraVelocityShaderSource.find("mul(InverseViewProjection, float4(currentNdc, depth, 1.0f))"),
              std::string::npos);
    EXPECT_NE(cameraVelocityShaderSource.find("mul(PreviousViewProjection, float4(world.xyz, 1.0f))"),
              std::string::npos);
    EXPECT_NE(cameraVelocityShaderSource.find("return currentNdc - previousNdc;"), std::string::npos);

    EXPECT_NE(shaderSource.find("RaytracingAccelerationStructure gScene : register(RVX_RT_REFLECTION_TLAS_REGISTER, space0);"),
              std::string::npos);
    EXPECT_NE(shaderSource.find("RWTexture2D<float4> gReflectionOutput : register(RVX_RT_REFLECTION_OUTPUT_REGISTER, space0);"),
              std::string::npos);
    EXPECT_NE(shaderSource.find("Texture2D<float4> gSceneColor : register(RVX_RT_REFLECTION_SCENE_COLOR_REGISTER, space0);"), std::string::npos);
    EXPECT_NE(shaderSource.find("Texture2D<float> gSceneDepth : register(RVX_RT_REFLECTION_SCENE_DEPTH_REGISTER, space0);"), std::string::npos);
    EXPECT_NE(shaderSource.find("#include \"RayTracingResourceBindings.hlsli\""), std::string::npos);
    EXPECT_NE(shaderSource.find("#include \"RayTracingSceneMetadata.hlsli\""), std::string::npos);
    EXPECT_NE(metadataSource.find("RT_MATERIAL_HAS_METALLIC_ROUGHNESS_TEXTURE"), std::string::npos);
    EXPECT_NE(metadataSource.find("RT_MATERIAL_HAS_NORMAL_TEXTURE"), std::string::npos);
    EXPECT_NE(metadataSource.find("RT_MATERIAL_HAS_EMISSIVE_TEXTURE"), std::string::npos);
    EXPECT_NE(metadataSource.find("RT_MATERIAL_ALPHA_TEST"), std::string::npos);
    EXPECT_NE(metadataSource.find("RT_ALPHA_TEST_ENABLED"), std::string::npos);
    EXPECT_NE(metadataSource.find("RT_ALPHA_HAS_TANGENT_BUFFER"), std::string::npos);
    EXPECT_NE(shaderSource.find("StructuredBuffer<RayTracingInstanceMaterialMetadata> gInstanceMaterialMetadata : register(RVX_RT_REFLECTION_MATERIAL_METADATA_REGISTER, space0);"),
              std::string::npos);
    EXPECT_NE(shaderSource.find("Texture2D<float4> gMaterialTextures[RT_MAX_MATERIAL_TEXTURES] : register(RVX_RT_REFLECTION_MATERIAL_TEXTURES_REGISTER, space0);"),
              std::string::npos);
    EXPECT_NE(metadataSource.find("struct RayTracingMaterialTextureSamplingMetadata"), std::string::npos);
    EXPECT_NE(metadataSource.find("RayTracingMaterialTextureSamplingMetadata BaseColorTextureSampling;"),
              std::string::npos);
    EXPECT_NE(metadataSource.find("RayTracingMaterialTextureSamplingMetadata MetallicRoughnessTextureSampling;"),
              std::string::npos);
    EXPECT_NE(metadataSource.find("RayTracingMaterialTextureSamplingMetadata EmissiveTextureSampling;"),
              std::string::npos);
    EXPECT_NE(metadataSource.find("uint TangentBufferTableIndex;"), std::string::npos);
    EXPECT_NE(shaderSource.find("Texture2D<float4> gPreviousReflectionHistory : register(RVX_RT_REFLECTION_PREVIOUS_HISTORY_REGISTER, space0);"),
              std::string::npos);
    EXPECT_NE(shaderSource.find("RWTexture2D<float> gCurrentDepthHistory : register(RVX_RT_REFLECTION_OUTPUT_DEPTH_REGISTER, space0);"),
              std::string::npos);
    EXPECT_NE(shaderSource.find("RWTexture2D<float4> gCurrentNormalHistory : register(RVX_RT_REFLECTION_OUTPUT_NORMAL_REGISTER, space0);"),
              std::string::npos);
    EXPECT_NE(shaderSource.find("StructuredBuffer<RayTracingInstanceAlphaMetadata> gInstanceGeometryMetadata : register(RVX_RT_REFLECTION_GEOMETRY_METADATA_REGISTER, space0);"),
              std::string::npos);
    EXPECT_NE(shaderSource.find("ByteAddressBuffer gReflectionIndexBuffers[RT_MAX_GEOMETRY_BUFFERS] : register(RVX_RT_REFLECTION_INDEX_BUFFERS_REGISTER, space0);"),
              std::string::npos);
    EXPECT_NE(shaderSource.find("ByteAddressBuffer gReflectionUVBuffers[RT_MAX_GEOMETRY_BUFFERS] : register(RVX_RT_REFLECTION_UV_BUFFERS_REGISTER, space0);"),
              std::string::npos);
    EXPECT_NE(shaderSource.find("ByteAddressBuffer gReflectionNormalBuffers[RT_MAX_GEOMETRY_BUFFERS] : register(RVX_RT_REFLECTION_NORMAL_BUFFERS_REGISTER, space0);"),
              std::string::npos);
    EXPECT_NE(shaderSource.find("ByteAddressBuffer gReflectionTangentBuffers[RT_MAX_GEOMETRY_BUFFERS] : register(RVX_RT_REFLECTION_TANGENT_BUFFERS_REGISTER, space0);"),
              std::string::npos);
    EXPECT_NE(shaderSource.find("Texture2D<float2> gSceneVelocity : register(RVX_RT_REFLECTION_SCENE_VELOCITY_REGISTER, space0);"),
              std::string::npos);
    EXPECT_NE(shaderSource.find("cbuffer RayTracedReflectionConstants : register(RVX_RT_REFLECTION_CONSTANTS_REGISTER, space0)"),
              std::string::npos);
    EXPECT_NE(shaderSource.find("float4x4 gPreviousViewProjection;"), std::string::npos);
    EXPECT_NE(shaderSource.find("float4 gOutputSizeAndInvSize;"), std::string::npos);
    EXPECT_NE(shaderSource.find("float4 gSceneSizeAndInvSize;"), std::string::npos);
    EXPECT_NE(shaderSource.find("float4 gHistoryParams;"), std::string::npos);
    EXPECT_NE(shaderSource.find("w: distance fade start"), std::string::npos);
    EXPECT_NE(shaderSource.find("float4 gStochasticParams;"), std::string::npos);
    EXPECT_NE(shaderSource.find("float4 gRayBiasParams;"), std::string::npos);
    EXPECT_NE(shaderSource.find("z: firefly clamp luminance"), std::string::npos);
    EXPECT_NE(shaderSource.find("float4 gHistoryClampParams;"), std::string::npos);
    EXPECT_NE(shaderSource.find("x: luminance tolerance, y: min current confidence"), std::string::npos);
    EXPECT_NE(shaderSource.find("z: velocity available, w: velocity rejection scale"), std::string::npos);
    EXPECT_NE(shaderSource.find("RVX_RT_REFLECTION_MAX_SAMPLES_PER_PIXEL"), std::string::npos);
    EXPECT_NE(shaderSource.find("float hitT;"), std::string::npos);
    EXPECT_NE(shaderSource.find("ComputeHitDistanceConfidence"), std::string::npos);
    EXPECT_NE(shaderSource.find("ClampReflectionSampleRadiance"), std::string::npos);
    EXPECT_NE(shaderSource.find("dot(radiance, float3(0.2126f, 0.7152f, 0.0722f))"), std::string::npos);
    EXPECT_NE(shaderSource.find("ReflectionLuminance"), std::string::npos);
    EXPECT_NE(shaderSource.find("ClampReflectionHistoryRadiance"), std::string::npos);
    EXPECT_NE(shaderSource.find("ComputeVelocityHistoryWeight"), std::string::npos);
    EXPECT_NE(shaderSource.find("ComputeReflectionHistoryWeight"), std::string::npos);
    EXPECT_NE(shaderSource.find("currentReflection.a <= minConfidence"), std::string::npos);
    EXPECT_NE(shaderSource.find("baseWeight * luminanceWeight * motionWeight"), std::string::npos);
    EXPECT_NE(shaderSource.find("HasMaterialTexture"), std::string::npos);
    EXPECT_NE(shaderSource.find("LoadMaterialTextureRepresentative"), std::string::npos);
    EXPECT_NE(shaderSource.find("TryLoadHitUV"), std::string::npos);
    EXPECT_NE(shaderSource.find("PrimitiveIndex() * 3u"), std::string::npos);
    EXPECT_NE(shaderSource.find("gReflectionUVBuffers[NonUniformResourceIndex(geometry.UVBufferTableIndex)].Load2"),
              std::string::npos);
    EXPECT_NE(shaderSource.find("TryLoadHitNormal"), std::string::npos);
    EXPECT_NE(shaderSource.find("gReflectionNormalBuffers[NonUniformResourceIndex(geometry.NormalBufferTableIndex)].Load3"),
              std::string::npos);
    EXPECT_NE(shaderSource.find("TryLoadHitTangent"), std::string::npos);
    EXPECT_NE(shaderSource.find("gReflectionTangentBuffers[NonUniformResourceIndex(geometry.TangentBufferTableIndex)].Load4"),
              std::string::npos);
    EXPECT_NE(shaderSource.find("TryApplyNormalMap"), std::string::npos);
    EXPECT_NE(shaderSource.find("material.NormalTextureTableIndex"), std::string::npos);
    EXPECT_NE(shaderSource.find("material.NormalTextureSampling"), std::string::npos);
    EXPECT_NE(shaderSource.find("material.MaterialFactors.w"), std::string::npos);
    EXPECT_NE(shaderSource.find("ObjectRayDirection()"), std::string::npos);
    EXPECT_NE(shaderSource.find("SampleMaterialTextureAtUV"), std::string::npos);
    EXPECT_NE(shaderSource.find("SampleMaterialTextureAtUV(textureTableIndex, sampling, hitUV)"),
              std::string::npos);
    EXPECT_NE(shaderSource.find("TransformMaterialUV(sampling, uv)"), std::string::npos);
    EXPECT_NE(shaderSource.find("sampling.SamplerFlags & RT_ALPHA_WRAP_S_CLAMP"), std::string::npos);
    EXPECT_NE(shaderSource.find("material.BaseColorTextureSampling"), std::string::npos);
    EXPECT_NE(shaderSource.find("ReflectionRandom2"), std::string::npos);
    EXPECT_NE(shaderSource.find("BuildStochasticReflectionDirection"), std::string::npos);
    EXPECT_NE(shaderSource.find("uint2 MapOutputPixelToScenePixel(uint2 outputPixel)"), std::string::npos);
    EXPECT_NE(shaderSource.find("const uint2 scenePixel = MapOutputPixelToScenePixel(pixel);"),
              std::string::npos);
    EXPECT_NE(shaderSource.find("gSceneDepth.Load(int3(scenePixel, 0)).r"), std::string::npos);
    EXPECT_NE(shaderSource.find("[shader(\"raygeneration\")]"), std::string::npos);
    EXPECT_NE(shaderSource.find("void ReflectionRayGen()"), std::string::npos);
    EXPECT_NE(shaderSource.find("TraceRay(gScene"), std::string::npos);
    EXPECT_NE(shaderSource.find("ray.Origin = worldPosition + worldNormal * max(gRayBiasParams.x, 0.0f);"),
              std::string::npos);
    EXPECT_NE(shaderSource.find("ray.TMin = max(gRayBiasParams.y, 0.0f);"), std::string::npos);
    EXPECT_NE(shaderSource.find("reflect(viewDirection, worldNormal)"), std::string::npos);
    EXPECT_NE(shaderSource.find("for (uint sampleIndex = 0u; sampleIndex < RVX_RT_REFLECTION_MAX_SAMPLES_PER_PIXEL; ++sampleIndex)"),
              std::string::npos);
    EXPECT_NE(shaderSource.find("accumulatedColor += ClampReflectionSampleRadiance(payload.color, gRayBiasParams.z);"),
              std::string::npos);
    EXPECT_NE(shaderSource.find("accumulatedConfidence += payload.hit != 0u"), std::string::npos);
    EXPECT_NE(shaderSource.find("TryResolvePreviousReflectionHistoryPixel"), std::string::npos);
    EXPECT_NE(shaderSource.find("gHistoryClampParams.z > 0.5f"), std::string::npos);
    EXPECT_NE(shaderSource.find("gSceneVelocity.Load(int3(scenePixel, 0)).xy"), std::string::npos);
    EXPECT_NE(shaderSource.find("return saturate(1.0f - length(velocityNdc) * velocityRejectionScale);"),
              std::string::npos);
    EXPECT_NE(shaderSource.find("previousUv = currentUv - velocityUv;"), std::string::npos);
    EXPECT_NE(shaderSource.find("TryLoadReprojectedReflection"), std::string::npos);
    EXPECT_NE(shaderSource.find("TryLoadReprojectedReflection(worldPosition, worldNormal, scenePixel, previousReflection)"),
              std::string::npos);
    EXPECT_NE(shaderSource.find("const float velocityHistoryWeight = ComputeVelocityHistoryWeight(scenePixel);"),
              std::string::npos);
    EXPECT_NE(shaderSource.find("ComputeReflectionHistoryWeight(reflection, previousReflection, velocityHistoryWeight)"),
              std::string::npos);
    EXPECT_NE(shaderSource.find("ClampReflectionHistoryRadiance(previousReflection.rgb, reflection.rgb, gHistoryClampParams.x);"),
              std::string::npos);
    EXPECT_NE(shaderSource.find("reflection = lerp(reflection, previousReflection, historyWeight);"),
              std::string::npos);
    EXPECT_NE(shaderSource.find("gReflectionOutput[pixel] = reflection;"),
              std::string::npos);
    EXPECT_NE(viewDataHeader.find("RGTextureHandle velocityTarget;"), std::string::npos);
    EXPECT_NE(viewDataHeader.find("Mat4 previousViewProjectionMatrix = Mat4Identity();"), std::string::npos);
    EXPECT_NE(viewDataHeader.find("uint8 previousViewProjectionValid = 0;"), std::string::npos);
    EXPECT_NE(shaderSource.find("[shader(\"miss\")]"), std::string::npos);
    EXPECT_NE(shaderSource.find("[shader(\"anyhit\")]"), std::string::npos);
    EXPECT_NE(shaderSource.find("void ReflectionAnyHit"), std::string::npos);
    EXPECT_NE(shaderSource.find("ShouldIgnoreReflectionHitForAlpha"), std::string::npos);
    EXPECT_NE(shaderSource.find("(material.Flags & RT_MATERIAL_ALPHA_TEST) != 0u"), std::string::npos);
    EXPECT_NE(shaderSource.find("(geometry.Flags & RT_ALPHA_TEST_ENABLED) != 0u"), std::string::npos);
    EXPECT_NE(shaderSource.find("LoadMaterialTexture("), std::string::npos);
    EXPECT_NE(shaderSource.find("IgnoreHit();"), std::string::npos);
    EXPECT_NE(shaderSource.find("[shader(\"closesthit\")]"), std::string::npos);
    EXPECT_NE(shaderSource.find("const uint instanceId = InstanceID();"), std::string::npos);
    EXPECT_NE(shaderSource.find("gInstanceMaterialMetadata[instanceId]"), std::string::npos);
    EXPECT_NE(shaderSource.find("material.MaterialFactors.y"), std::string::npos);
    EXPECT_NE(shaderSource.find("material.MaterialFactors.x"), std::string::npos);
    EXPECT_NE(shaderSource.find("material.MetallicRoughnessTextureTableIndex"), std::string::npos);
    EXPECT_NE(shaderSource.find("metallicRoughness.b"), std::string::npos);
    EXPECT_NE(shaderSource.find("material.MetallicRoughnessTextureSampling"), std::string::npos);
    EXPECT_NE(shaderSource.find("material.EmissiveTextureTableIndex"), std::string::npos);
    EXPECT_NE(shaderSource.find("material.EmissiveTextureSampling"), std::string::npos);
    EXPECT_NE(shaderSource.find("ComputeReflectionF0"), std::string::npos);
    EXPECT_NE(shaderSource.find("FresnelSchlickRoughness"), std::string::npos);
    EXPECT_NE(shaderSource.find("RoughnessSpecularVisibility"), std::string::npos);
    EXPECT_NE(shaderSource.find("EvaluateReflectionHitRadiance"), std::string::npos);
    EXPECT_NE(shaderSource.find("payload.hitT = RayTCurrent();"), std::string::npos);

    EXPECT_NE(compositePassHeader.find("class RayTracedReflectionCompositePass : public IRenderPass"),
              std::string::npos);
    EXPECT_NE(compositePassHeader.find("SetReflectionSource(const RayTracedReflectionPass* reflectionPass)"),
              std::string::npos);
    EXPECT_NE(compositePassHeader.find("SetDenoisedReflectionSource(const RayTracedReflectionDenoisePass* denoisePass)"),
              std::string::npos);
    EXPECT_NE(compositePassHeader.find("enum class RayTracedReflectionCompositeSource"), std::string::npos);
    EXPECT_NE(compositePassHeader.find("DenoisedReflection"), std::string::npos);
    EXPECT_NE(compositePassHeader.find("bool denoiseRequested = false;"), std::string::npos);
    EXPECT_NE(compositePassHeader.find("bool denoisedSourceUsed = false;"), std::string::npos);
    EXPECT_NE(compositePassHeader.find("bool rawSourceUsed = false;"), std::string::npos);
    EXPECT_NE(compositePassHeader.find("bool denoiseFallbackToRaw = false;"), std::string::npos);
    EXPECT_NE(compositePassHeader.find("RayTracedReflectionCompositeSource source = RayTracedReflectionCompositeSource::None;"),
              std::string::npos);
    EXPECT_NE(compositePassSource.find("GetRayTracedReflectionCompositePipeline(outputFormat)"),
              std::string::npos);
    EXPECT_NE(compositePassSource.find("m_denoisePass->GetDenoisedReflectionHandle()"), std::string::npos);
    EXPECT_NE(compositePassSource.find("m_stats.denoisedSourceUsed = true;"), std::string::npos);
    EXPECT_NE(compositePassSource.find("m_stats.source = RayTracedReflectionCompositeSource::DenoisedReflection;"),
              std::string::npos);
    EXPECT_NE(compositePassSource.find("m_stats.rawSourceUsed = reflectionHandle.IsValid();"), std::string::npos);
    EXPECT_NE(compositePassSource.find("m_stats.denoiseFallbackToRaw = m_stats.rawSourceUsed && m_stats.denoiseRequested;"),
              std::string::npos);
    EXPECT_NE(compositePassSource.find("m_stats.source = RayTracedReflectionCompositeSource::RawReflection;"),
              std::string::npos);
    EXPECT_NE(compositePassSource.find("descriptorDesc.BindTexture(1, reflectionView);"), std::string::npos);
    EXPECT_NE(compositePassSource.find("renderPassDesc.AddColorAttachment(colorTargetView, RHILoadOp::Load"),
              std::string::npos);
    EXPECT_NE(compositePassSource.find("ctx.Draw(3, 1, 0, 0);"), std::string::npos);
    EXPECT_NE(compositeShaderSource.find("Texture2D<float4> ReflectionTexture : register(t1, space0);"),
              std::string::npos);
    EXPECT_NE(compositeShaderSource.find("const float alpha = saturate(reflection.a * max(IntensityScale, 0.0));"),
              std::string::npos);

    EXPECT_NE(denoisePassHeader.find("class RayTracedReflectionDenoisePass : public IRenderPass"),
              std::string::npos);
    EXPECT_NE(denoisePassHeader.find("struct RayTracedReflectionDenoisePassConfig"), std::string::npos);
    EXPECT_NE(denoisePassHeader.find("RGTextureHandle GetDenoisedReflectionHandle() const"), std::string::npos);
    EXPECT_NE(denoisePassHeader.find("bool normalGuideAvailable = false;"), std::string::npos);
    EXPECT_NE(denoisePassHeader.find("uint32 radius = 0;"), std::string::npos);
    EXPECT_NE(denoisePassHeader.find("float depthSigma = 0.0f;"), std::string::npos);
    EXPECT_NE(denoisePassHeader.find("float normalThreshold = 0.0f;"), std::string::npos);
    EXPECT_NE(denoisePassHeader.find("float confidencePower = 0.0f;"), std::string::npos);
    EXPECT_NE(denoisePassHeader.find("float centerWeight = 0.0f;"), std::string::npos);
    EXPECT_NE(denoisePassHeader.find("float lowConfidenceDepthScale = 0.0f;"), std::string::npos);
    EXPECT_NE(denoisePassHeader.find("uint32 kernelTapCount = 0;"), std::string::npos);
    EXPECT_NE(denoisePassHeader.find("uint64 dispatchPixelCount = 0;"), std::string::npos);
    EXPECT_NE(denoisePassHeader.find("uint64 estimatedTapCount = 0;"), std::string::npos);
    EXPECT_NE(denoisePassHeader.find("uint32 radius = 1;"), std::string::npos);
    EXPECT_NE(denoisePassHeader.find("float normalThreshold = 0.85f;"), std::string::npos);
    EXPECT_NE(denoisePassHeader.find("float centerWeight = 1.0f;"), std::string::npos);
    EXPECT_NE(denoisePassHeader.find("float lowConfidenceDepthScale = 4.0f;"), std::string::npos);
    EXPECT_NE(denoisePassSource.find("GetRayTracedReflectionDenoisePipeline(outputFormat)"), std::string::npos);
    EXPECT_NE(denoisePassSource.find("GetRayTracedReflectionDenoiseSetLayout()"), std::string::npos);
    EXPECT_NE(denoisePassSource.find("RHITextureDesc::RenderTarget(view.viewportWidth, view.viewportHeight, RHIFormat::RGBA16_FLOAT)"),
              std::string::npos);
    EXPECT_EQ(denoisePassSource.find("outputDesc.width = view.viewportWidth;"), std::string::npos);
    EXPECT_EQ(denoisePassSource.find("outputDesc.height = view.viewportHeight;"), std::string::npos);
    EXPECT_NE(denoisePassSource.find("m_reflectionPass->GetCurrentNormalHistoryHandle()"), std::string::npos);
    EXPECT_NE(denoisePassSource.find("m_stats.radius = std::min<uint32>(m_config.radius, 3u);"),
              std::string::npos);
    EXPECT_NE(denoisePassSource.find("m_stats.depthSigma = std::max(m_config.depthSigma, 1.0e-5f);"),
              std::string::npos);
    EXPECT_NE(denoisePassSource.find("m_stats.normalThreshold = std::clamp(m_config.normalThreshold, 0.0f, 1.0f);"),
              std::string::npos);
    EXPECT_NE(denoisePassSource.find("m_stats.confidencePower = std::max(m_config.confidencePower, 0.01f);"),
              std::string::npos);
    EXPECT_NE(denoisePassSource.find("m_stats.centerWeight = std::max(m_config.centerWeight, 0.0f);"),
              std::string::npos);
    EXPECT_NE(denoisePassSource.find("m_stats.lowConfidenceDepthScale = std::max(m_config.lowConfidenceDepthScale, 1.0f);"),
              std::string::npos);
    EXPECT_NE(denoisePassSource.find("m_normalGuideReadHandle = builder.Read(normalGuideHandle, RHIShaderStage::Pixel);"),
              std::string::npos);
    EXPECT_NE(denoisePassSource.find("descriptorDesc.BindTexture(1, reflectionView);"), std::string::npos);
    EXPECT_NE(denoisePassSource.find("descriptorDesc.BindTexture(2, depthView);"), std::string::npos);
    EXPECT_NE(denoisePassSource.find("descriptorDesc.BindTexture(3, normalGuideView);"), std::string::npos);
    EXPECT_NE(denoisePassSource.find("float denoiseQualityParams[4] = {1.0f, 4.0f, 0.0f, 0.0f};"),
              std::string::npos);
    EXPECT_NE(denoisePassSource.find("constants.denoiseQualityParams[0] = std::max(m_config.centerWeight, 0.0f);"),
              std::string::npos);
    EXPECT_NE(denoisePassSource.find("constants.denoiseQualityParams[1] = std::max(m_config.lowConfidenceDepthScale, 1.0f);"),
              std::string::npos);
    EXPECT_NE(denoisePassSource.find("renderPassDesc.AddColorAttachment(outputView, RHILoadOp::DontCare"),
              std::string::npos);
    EXPECT_NE(denoisePassSource.find("ctx.Draw(3, 1, 0, 0);"), std::string::npos);
    EXPECT_NE(denoiseShaderSource.find("Texture2D<float4> ReflectionTexture : register(t1, space0);"),
              std::string::npos);
    EXPECT_NE(denoiseShaderSource.find("Texture2D<float> SceneDepthTexture : register(t2, space0);"),
              std::string::npos);
    EXPECT_NE(denoiseShaderSource.find("Texture2D<float4> NormalGuideTexture : register(t3, space0);"),
              std::string::npos);
    EXPECT_NE(denoiseShaderSource.find("float4 OutputSizeAndInvSize;"), std::string::npos);
    EXPECT_NE(denoiseShaderSource.find("float4 SceneDepthSizeAndInvSize;"), std::string::npos);
    EXPECT_NE(denoiseShaderSource.find("float4 DenoiseQualityParams;"), std::string::npos);
    EXPECT_NE(denoiseShaderSource.find("MapOutputPixelToSceneDepthPixel"), std::string::npos);
    EXPECT_NE(denoiseShaderSource.find("DecodeNormalGuide"), std::string::npos);
    EXPECT_NE(denoiseShaderSource.find("NormalWeight"), std::string::npos);
    EXPECT_NE(denoiseShaderSource.find("DepthWeight(sampleDepth, centerDepth, adaptiveDepthSigma)"), std::string::npos);
    EXPECT_NE(denoiseShaderSource.find("confidenceWeight"), std::string::npos);
    EXPECT_NE(denoiseShaderSource.find("const float centerPreservation = max(DenoiseQualityParams.x, 0.0);"),
              std::string::npos);
    EXPECT_NE(denoiseShaderSource.find("const float lowConfidenceDepthScale = max(DenoiseQualityParams.y, 1.0);"),
              std::string::npos);
    EXPECT_NE(denoiseShaderSource.find("const float adaptiveDepthSigma = depthSigma * lerp(lowConfidenceDepthScale, 1.0, centerConfidence);"),
              std::string::npos);
    EXPECT_NE(denoiseShaderSource.find("const float centerWeight = centerPreservation * max(centerConfidence, 0.05);"),
              std::string::npos);
    EXPECT_NE(denoiseShaderSource.find("weightedReflection += centerReflection.rgb * centerWeight;"),
              std::string::npos);
    EXPECT_NE(denoiseShaderSource.find("spatialWeight * edgeWeight * normalWeight * confidenceWeight"),
              std::string::npos);
}

TEST_F(PipelineCacheValidationFixture, OpaquePassConsumesRayTracedShadowMask)
{
    if (!HasCompilerAvailable())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    const fs::path renderRoot = FindShaderDirectory().parent_path();
    const std::string opaqueHeader = ReadTextFile(renderRoot / "Include" / "Render" / "Passes" / "OpaquePass.h");
    const std::string opaqueSource = ReadTextFile(renderRoot / "Private" / "Passes" / "OpaquePass.cpp");
    const std::string sceneRendererSource = ReadTextFile(renderRoot / "Private" / "Renderer" / "SceneRenderer.cpp");
    const std::string pipelineHeader = ReadTextFile(renderRoot / "Include" / "Render" / "PipelineCache.h");
    const std::string pipelineSource = ReadTextFile(renderRoot / "Private" / "PipelineCache.cpp");
    const std::string defaultLitSource = ReadTextFile(FindShaderDirectory() / "DefaultLit.hlsl");

    EXPECT_NE(opaqueHeader.find("void SetRayTracedShadowSource(const RayTracedShadowPass* shadowPass);"),
              std::string::npos);
    EXPECT_NE(opaqueSource.find("m_rayTracedShadowMaskReadHandle = builder.Read(shadowMask, RHIShaderStage::Pixel);"),
              std::string::npos);
    EXPECT_NE(opaqueSource.find("RayTracedShadowFrameResources rayTracedShadowResources;"),
              std::string::npos);
    EXPECT_NE(opaqueSource.find("UpdateRayTracedShadowFrameResources(rayTracedShadowResources)"),
              std::string::npos);
    EXPECT_NE(opaqueSource.find("drawView.rayTracedShadowEnabled = rayTracedShadowBinding.shadowMaskSamplingEnabled ? 1 : 0;"),
              std::string::npos);
    EXPECT_NE(opaqueSource.find("rayTracedShadowConfig.filterRadiusTexels"),
              std::string::npos);
    EXPECT_NE(opaqueSource.find("drawView.rayTracedShadowMode = rayTracedShadowConfig.rayTracedShadowMode;"),
              std::string::npos);
    EXPECT_NE(opaqueSource.find("drawView.rayTracedShadowMode == RayTracedShadowMode::ReplaceRaster"),
              std::string::npos);
    EXPECT_NE(opaqueSource.find("ClearDirectionalShadowViewData(drawView);"),
              std::string::npos);
    EXPECT_NE(sceneRendererSource.find("m_opaquePass->SetRayTracedShadowSource(m_rayTracedShadowPass);"),
              std::string::npos);

    EXPECT_NE(pipelineHeader.find("RayTracedShadowFrameResources"), std::string::npos);
    EXPECT_NE(pipelineHeader.find("UpdateRayTracedShadowFrameResources"), std::string::npos);
    EXPECT_NE(pipelineSource.find("frameLayout.AddBinding(6, RHIBindingType::SampledTexture"),
              std::string::npos);
    EXPECT_NE(pipelineSource.find("descSetDesc.BindTexture(6, m_fallbackRayTracedShadowMaskView.Get());"),
              std::string::npos);
    EXPECT_NE(pipelineSource.find("bindings.push_back({6, nullptr, 0, 0, rayTracedShadowMask, nullptr});"),
              std::string::npos);
    EXPECT_NE(pipelineSource.find("view.rayTracedShadowMode == RayTracedShadowMode::ReplaceRaster ? 1.0f : 0.0f"),
              std::string::npos);

    EXPECT_NE(defaultLitSource.find("Texture2D<float> RayTracedShadowMaskTexture : register(t6, space0);"),
              std::string::npos);
    EXPECT_NE(defaultLitSource.find("float SampleRayTracedShadowMask(float4 screenPosition)"),
              std::string::npos);
    EXPECT_NE(defaultLitSource.find("int filterRadius = (int)round(clamp(RayTracedShadowParams.y"),
              std::string::npos);
    EXPECT_NE(defaultLitSource.find("visibility * (1.0 / 16.0)"), std::string::npos);
    EXPECT_NE(defaultLitSource.find("float ComposeDirectionalShadowVisibility(float rasterVisibility, float rayTracedVisibility)"),
              std::string::npos);
    EXPECT_NE(defaultLitSource.find("const float compositionMode = round(RayTracedShadowParams.z);"),
              std::string::npos);
    EXPECT_NE(defaultLitSource.find("return rasterVisibility * rayTracedVisibility;"),
              std::string::npos);
    EXPECT_NE(defaultLitSource.find("ComposeDirectionalShadowVisibility(rasterShadowVisibility, rayTracedShadowVisibility)"),
              std::string::npos);
}

TEST_F(PipelineCacheValidationFixture, DX12BackendImplementsRayTracingPipelineShaderTableAndDispatch)
{
    if (!HasCompilerAvailable())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    const fs::path repoRoot = FindShaderDirectory().parent_path().parent_path();
    const fs::path dx12Dir = repoRoot / "RHI_DX12" / "Private";
    const std::string pipelineHeader = ReadTextFile(dx12Dir / "DX12Pipeline.h");
    const std::string pipelineSource = ReadTextFile(dx12Dir / "DX12Pipeline.cpp");
    const std::string resourcesHeader = ReadTextFile(dx12Dir / "DX12Resources.h");
    const std::string resourcesSource = ReadTextFile(dx12Dir / "DX12Resources.cpp");
    const std::string deviceHeader = ReadTextFile(dx12Dir / "DX12Device.h");
    const std::string deviceSource = ReadTextFile(dx12Dir / "DX12Device.cpp");
    const std::string commandHeader = ReadTextFile(dx12Dir / "DX12CommandContext.h");
    const std::string commandSource = ReadTextFile(dx12Dir / "DX12CommandContext.cpp");

    EXPECT_NE(pipelineHeader.find("DX12Pipeline(DX12Device* device, const RHIRayTracingPipelineDesc& desc);"),
              std::string::npos);
    EXPECT_NE(pipelineHeader.find("class DX12ShaderTable : public RHIShaderTable"), std::string::npos);
    EXPECT_NE(pipelineHeader.find("RHIPipeline* GetRayTracingPipeline() const override"), std::string::npos);
    EXPECT_NE(pipelineHeader.find("DX12Pipeline* GetPipeline() const"), std::string::npos);
    EXPECT_NE(pipelineHeader.find("RHIPipelineRef m_pipelineOwner"), std::string::npos);
    EXPECT_NE(pipelineHeader.find("DX12Pipeline* m_pipeline = nullptr"), std::string::npos);
    EXPECT_NE(pipelineHeader.find("D3D12_DISPATCH_RAYS_DESC BuildDispatchRaysDesc"), std::string::npos);
    EXPECT_NE(pipelineHeader.find("bool IsValid() const;"), std::string::npos);
    EXPECT_EQ(pipelineHeader.find("bool IsValid() const { return m_buffer.Get() != nullptr; }"),
              std::string::npos);
    EXPECT_NE(pipelineHeader.find("GetRayTracingShaderGroupCount() const override"), std::string::npos);
    EXPECT_NE(pipelineHeader.find("RHIShaderStage GetRayTracingShaderGroupStage"), std::string::npos);
    EXPECT_NE(pipelineHeader.find("bool IsRayTracingHitGroup"), std::string::npos);
    EXPECT_NE(pipelineHeader.find("std::vector<RHIShaderStage> m_shaderGroupStages"), std::string::npos);
    EXPECT_NE(pipelineHeader.find("std::vector<uint8> m_shaderGroupIsHitGroup"), std::string::npos);
    EXPECT_NE(pipelineHeader.find("uint32 GetSetLayoutCount() const"), std::string::npos);
    EXPECT_NE(pipelineHeader.find("DX12DescriptorSetLayout* GetSetLayout(uint32 setIndex) const;"),
              std::string::npos);
    EXPECT_NE(pipelineHeader.find("std::vector<DX12DescriptorSetLayout*> m_setLayouts;"),
              std::string::npos);
    EXPECT_NE(commandHeader.find("class DX12DescriptorSetLayout;"), std::string::npos);
    EXPECT_NE(commandHeader.find("std::vector<DX12DescriptorSetLayout*> m_boundRayTracingDescriptorSetLayouts;"),
              std::string::npos);
    EXPECT_NE(pipelineSource.find("D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE"), std::string::npos);
    EXPECT_NE(pipelineSource.find("const RHICapabilities& caps = m_device->GetCapabilities();"),
              std::string::npos);
    EXPECT_NE(pipelineSource.find("caps.maxRayRecursionDepth == 0"), std::string::npos);
    EXPECT_NE(pipelineSource.find("desc.maxRecursionDepth > caps.maxRayRecursionDepth"),
              std::string::npos);
    EXPECT_NE(pipelineSource.find("recursion depth {} exceeds device limit {}"), std::string::npos);
    EXPECT_NE(pipelineSource.find("D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY"), std::string::npos);
    EXPECT_NE(pipelineSource.find("D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP"), std::string::npos);
    EXPECT_NE(pipelineSource.find("D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE"), std::string::npos);
    EXPECT_EQ(pipelineSource.find("D3D12_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE"), std::string::npos);
    EXPECT_NE(pipelineSource.find("hitGroup.anyHitImport = hitGroup.exportName + L\"_AnyHit\""),
              std::string::npos);
    EXPECT_NE(pipelineSource.find("hitGroup.desc.AnyHitShaderImport"), std::string::npos);
    EXPECT_NE(pipelineSource.find("GetShaderIdentifier"), std::string::npos);
    EXPECT_NE(pipelineSource.find("MakeRHIRayTracingDefaultGeneralExportName"), std::string::npos);
    EXPECT_NE(pipelineSource.find("MakeRHIRayTracingDefaultHitGroupExportName"), std::string::npos);
    EXPECT_EQ(pipelineSource.find("MakeDefaultRayTracingExportName"), std::string::npos);
    EXPECT_EQ(pipelineSource.find("MakeDefaultHitGroupExportName"), std::string::npos);
    EXPECT_NE(pipelineSource.find("GetRayTracingShaderGroupStage"), std::string::npos);
    EXPECT_NE(pipelineSource.find("IsRayTracingHitGroup"), std::string::npos);
    EXPECT_NE(pipelineSource.find("shaderGroupStages.push_back(group.generalShader->GetStage())"),
              std::string::npos);
    EXPECT_NE(pipelineSource.find("shaderGroupIsHitGroup.push_back(1)"), std::string::npos);
    EXPECT_NE(pipelineSource.find("m_shaderGroupStages = std::move(shaderGroupStages);"), std::string::npos);
    EXPECT_NE(pipelineSource.find("dx12Shader->GetEntryPoint()"), std::string::npos);
    EXPECT_NE(pipelineSource.find("#include <limits>"), std::string::npos);
    EXPECT_NE(pipelineSource.find("bool TryAddUint64"), std::string::npos);
    EXPECT_NE(pipelineSource.find("bool TryMultiplyUint64"), std::string::npos);
    EXPECT_NE(pipelineSource.find("bool TryAlignUpUint64"), std::string::npos);
    EXPECT_NE(pipelineSource.find("records.size() > std::numeric_limits<uint32>::max()"),
              std::string::npos);
    EXPECT_NE(pipelineSource.find("shader table record count exceeds the RHI limit"),
              std::string::npos);
    EXPECT_NE(pipelineSource.find("TryAddUint64(handleSize, static_cast<uint64>(record.localRootDataSize), recordSize)"),
              std::string::npos);
    EXPECT_NE(pipelineSource.find("TryAlignUpUint64(cursor, tableAlignment, section.offset)"),
              std::string::npos);
    EXPECT_NE(pipelineSource.find("TryAlignUpUint64(maxRecordSize, recordAlignment, section.stride)"),
              std::string::npos);
    EXPECT_NE(pipelineSource.find("TryMultiplyUint64(section.stride, static_cast<uint64>(section.count), section.size)"),
              std::string::npos);
    EXPECT_NE(pipelineSource.find("TryAddUint64(section.offset, alignedSectionSize, cursor)"),
              std::string::npos);
    EXPECT_NE(pipelineSource.find("RVX_DX12_MAX_SHADER_RECORD_STRIDE"),
              std::string::npos);
    EXPECT_NE(pipelineSource.find("shader record stride"), std::string::npos);
    EXPECT_NE(pipelineSource.find("shader table section size overflowed"), std::string::npos);
    EXPECT_NE(pipelineSource.find("shader table size alignment overflowed"), std::string::npos);
    EXPECT_NE(pipelineSource.find("bool DX12ShaderTable::IsValid() const"), std::string::npos);
    EXPECT_NE(pipelineSource.find("auto sectionRangeValid = [&](const Section& section, bool required) -> bool"),
              std::string::npos);
    EXPECT_NE(pipelineSource.find("recordAlignment == 0 || tableAlignment == 0"), std::string::npos);
    EXPECT_NE(pipelineSource.find("section.stride > RVX_DX12_MAX_SHADER_RECORD_STRIDE"), std::string::npos);
    EXPECT_NE(pipelineSource.find("expectedSectionSize != section.size"), std::string::npos);
    EXPECT_NE(pipelineSource.find("sectionEnd > bufferSize"), std::string::npos);
    EXPECT_NE(pipelineSource.find("(sectionAddress % tableAlignment) != 0"), std::string::npos);
    EXPECT_NE(pipelineSource.find("m_rayGenerationSection.count == 1"), std::string::npos);
    EXPECT_NE(pipelineSource.find("shader table buffer must be GPU-addressable"), std::string::npos);
    EXPECT_NE(pipelineSource.find("if (baseAddress == 0)"), std::string::npos);
    EXPECT_NE(pipelineSource.find("internal shader table validation failed"), std::string::npos);
    EXPECT_EQ(pipelineSource.find("section.stride * section.count"), std::string::npos);
    EXPECT_EQ(pipelineSource.find("AlignUp(section.size, tableAlignment)"), std::string::npos);
    EXPECT_NE(pipelineSource.find("desc.RayGenerationShaderRecord.SizeInBytes = m_rayGenerationSection.size;"),
              std::string::npos);
    EXPECT_NE(pipelineSource.find("desc.MissShaderTable.SizeInBytes = m_missSection.size;"),
              std::string::npos);
    EXPECT_NE(pipelineSource.find("desc.HitGroupTable.SizeInBytes = m_hitGroupSection.size;"),
              std::string::npos);
    EXPECT_NE(pipelineSource.find("RHIBufferUsage::ShaderBindingTable"), std::string::npos);
    EXPECT_NE(pipelineSource.find("desc.GetRayTracingPipeline()"), std::string::npos);
    EXPECT_NE(pipelineSource.find("m_pipelineOwner = desc.rayTracingPipelineOwner;"), std::string::npos);
    EXPECT_NE(pipelineSource.find("m_pipelineOwner.Reset();"), std::string::npos);
    EXPECT_NE(pipelineSource.find("m_pipeline = pipeline;"), std::string::npos);
    EXPECT_NE(pipelineSource.find("m_pipeline = nullptr;"), std::string::npos);
    EXPECT_NE(pipelineSource.find("DX12DescriptorSetLayout* DX12PipelineLayout::GetSetLayout"),
              std::string::npos);
    EXPECT_NE(pipelineSource.find("m_setLayouts.reserve(desc.setLayouts.size())"), std::string::npos);
    EXPECT_NE(pipelineSource.find("m_setLayouts.push_back(setLayout);"), std::string::npos);
    EXPECT_NE(resourcesHeader.find("const char* GetEntryPoint() const"), std::string::npos);
    EXPECT_NE(resourcesHeader.find("std::string m_entryPoint"), std::string::npos);
    EXPECT_NE(resourcesSource.find("m_entryPoint(desc.entryPoint ? desc.entryPoint : \"main\")"), std::string::npos);
    EXPECT_NE(resourcesSource.find("ValidateRHIAccelerationStructureDesc(desc)"), std::string::npos);
    EXPECT_NE(resourcesSource.find("Cannot create acceleration structure: {}"), std::string::npos);
    EXPECT_EQ(resourcesSource.find("Cannot create zero-sized acceleration structure"), std::string::npos);

    EXPECT_NE(deviceHeader.find("RHIPipelineRef CreateRayTracingPipeline"), std::string::npos);
    EXPECT_NE(deviceHeader.find("RHIShaderTableRef CreateShaderTable"), std::string::npos);
    EXPECT_NE(deviceSource.find("CreateDX12RayTracingPipeline(this, desc)"), std::string::npos);
    EXPECT_NE(deviceSource.find("CreateDX12ShaderTable(this, desc)"), std::string::npos);
    EXPECT_NE(deviceSource.find("m_capabilities.supportsAccelerationStructureUpdate = m_capabilities.supportsRaytracing;"),
              std::string::npos);
    EXPECT_NE(deviceSource.find("m_capabilities.supportsAccelerationStructureCompaction = false;"),
              std::string::npos);
    EXPECT_EQ(deviceSource.find("m_capabilities.supportsAccelerationStructureCompaction = m_capabilities.supportsRaytracing;"),
              std::string::npos);
    EXPECT_NE(deviceSource.find("#include <limits>"), std::string::npos);
    EXPECT_NE(deviceSource.find("bool TryAddDX12GPUVirtualAddress"), std::string::npos);
    EXPECT_NE(deviceSource.find("offset > std::numeric_limits<uint64>::max() - baseAddress64"),
              std::string::npos);
    EXPECT_NE(deviceSource.find("TryAddDX12GPUVirtualAddress(dx12Buffer->GetGPUVirtualAddress(), offset, address)"),
              std::string::npos);
    EXPECT_EQ(deviceSource.find("dx12Buffer->GetGPUVirtualAddress() + offset"), std::string::npos);
    EXPECT_NE(deviceSource.find("ValidateDX12ASBuildFlags(m_capabilities, desc.buildFlags, \"BLAS size query\")"),
              std::string::npos);
    EXPECT_NE(deviceSource.find("ValidateDX12ASBuildFlags(m_capabilities, desc.buildFlags, \"TLAS size query\")"),
              std::string::npos);

    EXPECT_NE(commandHeader.find("void DispatchRays(const RHIDispatchRaysDesc& desc) override;"),
              std::string::npos);
    EXPECT_NE(resourcesHeader.find("return m_resource ? m_resource->GetGPUVirtualAddress() : 0;"),
              std::string::npos);
    EXPECT_NE(resourcesSource.find("D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE"),
              std::string::npos);
    EXPECT_NE(commandSource.find("#include <limits>"), std::string::npos);
    EXPECT_NE(commandSource.find("ValidateDX12BLASGeometryInputAddresses"), std::string::npos);
    EXPECT_NE(commandSource.find("bool TryAddDX12GPUVirtualAddress"), std::string::npos);
    EXPECT_NE(commandSource.find("offset > std::numeric_limits<uint64>::max() - baseAddress64"),
              std::string::npos);
    EXPECT_NE(commandSource.find("ValidateDX12BufferAddressRange"), std::string::npos);
    EXPECT_NE(commandSource.find("IsRHIRayTracingBufferRangeValid(*buffer, offset, requiredBytes)"),
              std::string::npos);
    EXPECT_NE(commandSource.find("GPU address overflowed"), std::string::npos);
    EXPECT_NE(commandSource.find("TryGetRHIRayTracingStridedRangeSize(triangles.vertexCount"),
              std::string::npos);
    EXPECT_NE(commandSource.find("buildDesc.ScratchAccelerationStructureData = scratchAddress;"),
              std::string::npos);
    EXPECT_NE(commandSource.find("buildDesc.Inputs.InstanceDescs = instanceAddress;"),
              std::string::npos);
    EXPECT_EQ(commandSource.find("scratch->GetGPUVirtualAddress() + scratchOffset"), std::string::npos);
    EXPECT_EQ(commandSource.find("instanceBuffer->GetGPUVirtualAddress() + desc.instanceOffset"),
              std::string::npos);
    EXPECT_NE(commandSource.find("ValidateDX12ASBuildFlags(m_device->GetCapabilities(), desc.buildFlags, \"BLAS build\")"),
              std::string::npos);
    EXPECT_NE(commandSource.find("ValidateDX12ASBuildFlags(m_device->GetCapabilities(), desc.buildFlags, \"TLAS build\")"),
              std::string::npos);
    EXPECT_NE(commandSource.find("AllowCompaction requires acceleration structure compaction support"),
              std::string::npos);
    EXPECT_NE(commandSource.find("ValidateDX12ASBuildResources"), std::string::npos);
    EXPECT_NE(commandSource.find("ValidateDX12RayTracingCommandState"), std::string::npos);
    EXPECT_NE(commandSource.find("requires active command recording"), std::string::npos);
    EXPECT_NE(commandSource.find("cannot run inside a render pass"), std::string::npos);
    EXPECT_NE(commandSource.find("D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT"),
              std::string::npos);
    EXPECT_NE(commandSource.find("update && !sizes.HasValidUpdateScratchSize()"), std::string::npos);
    EXPECT_NE(commandSource.find("update requires valid acceleration structure update scratch size"),
              std::string::npos);
    EXPECT_NE(commandSource.find("scratch buffer range is too small"), std::string::npos);
    EXPECT_NE(commandSource.find("destination AS has the wrong type"), std::string::npos);
    EXPECT_NE(commandSource.find("source AS has the wrong type for update"), std::string::npos);
    EXPECT_NE(commandSource.find("scratch buffer must use default memory"), std::string::npos);
    EXPECT_NE(commandSource.find("scratch buffer requires unordered access and device address usage"),
              std::string::npos);
    EXPECT_NE(commandSource.find("destination AS is too small"), std::string::npos);
    EXPECT_NE(commandSource.find("source AS is too small for update"), std::string::npos);
    EXPECT_NE(commandSource.find("requires a GPU-addressable {}"), std::string::npos);
    EXPECT_NE(commandSource.find("\"vertex buffer\""), std::string::npos);
    EXPECT_NE(commandSource.find("\"index buffer\""), std::string::npos);
    EXPECT_NE(commandSource.find("\"transform buffer\""), std::string::npos);
    EXPECT_NE(commandSource.find("\"AABB buffer\""), std::string::npos);
    EXPECT_NE(commandSource.find("SetPipeline requires a pipeline"), std::string::npos);
    EXPECT_NE(commandSource.find("ray tracing pipeline binding cannot run on the copy queue"),
              std::string::npos);
    EXPECT_NE(commandSource.find("ValidateDX12RayTracingCommandState(m_isRecording, m_inRenderPass, \"ray tracing pipeline binding\")"),
              std::string::npos);
    EXPECT_NE(commandSource.find("ray tracing pipeline binding requires a valid DX12 state object"),
              std::string::npos);
    EXPECT_NE(commandSource.find("ray tracing pipeline binding requires a pipeline layout and global root signature"),
              std::string::npos);
    EXPECT_NE(commandSource.find("bindingRayTracingPipeline"), std::string::npos);
    EXPECT_NE(commandSource.find("ray tracing descriptor set binding"), std::string::npos);
    EXPECT_NE(commandSource.find("ray tracing descriptor set binding requires a valid DX12 descriptor set"),
              std::string::npos);
    EXPECT_NE(commandSource.find("ray tracing descriptor set binding requires a pipeline layout"),
              std::string::npos);
    EXPECT_NE(commandSource.find("pipelineLayout->GetSetLayout(slot)"), std::string::npos);
    EXPECT_NE(commandSource.find("expectedLayout != setLayout"), std::string::npos);
    EXPECT_NE(commandSource.find("ray tracing descriptor set binding layout does not match the pipeline layout slot"),
              std::string::npos);
    EXPECT_NE(commandSource.find("m_boundRayTracingDescriptorSetLayouts.clear();"), std::string::npos);
    EXPECT_NE(commandSource.find("m_boundRayTracingDescriptorSetLayouts.resize(pipelineLayout->GetSetLayoutCount(), nullptr)"),
              std::string::npos);
    EXPECT_NE(commandSource.find("m_boundRayTracingDescriptorSetLayouts[slot] = setLayout;"),
              std::string::npos);
    EXPECT_NE(commandSource.find("DispatchRays requires descriptor set {} to be bound with the ray tracing pipeline layout"),
              std::string::npos);
    EXPECT_NE(commandSource.find("expectedLayout->GetEntries().empty()"), std::string::npos);
    EXPECT_NE(commandSource.find("ray tracing push constants binding"), std::string::npos);
    EXPECT_NE(commandSource.find("SetPipelineState1(dx12Pipeline->GetStateObject())"), std::string::npos);
    EXPECT_NE(commandSource.find("ValidateRHIDispatchRaysDesc(desc, m_currentPipeline)"), std::string::npos);
    EXPECT_NE(commandSource.find("shaderTable->GetPipeline() != m_currentPipeline"), std::string::npos);
    EXPECT_NE(commandSource.find("shader table does not match the bound ray tracing pipeline"), std::string::npos);
    EXPECT_NE(commandSource.find("m_currentPipeline->UsesComputeRootSignature()"), std::string::npos);

    const auto setPipelineStart = commandSource.find("void DX12CommandContext::SetPipeline");
    const auto setVertexBufferStart = commandSource.find("void DX12CommandContext::SetVertexBuffer", setPipelineStart);
    ASSERT_NE(setPipelineStart, std::string::npos);
    ASSERT_NE(setVertexBufferStart, std::string::npos);

    const std::string setPipelineBody = commandSource.substr(setPipelineStart, setVertexBufferStart - setPipelineStart);
    EXPECT_NE(setPipelineBody.find("m_currentPipeline = nullptr;"), std::string::npos);
    EXPECT_NE(setPipelineBody.find("m_boundRayTracingDescriptorSetLayouts.clear();"), std::string::npos);
    EXPECT_NE(setPipelineBody.find("if (!pipeline)"), std::string::npos);
    EXPECT_NE(setPipelineBody.find("if (m_queueType == RHICommandQueueType::Copy)"), std::string::npos);
    EXPECT_NE(setPipelineBody.find("ValidateDX12RayTracingCommandState(m_isRecording, m_inRenderPass, \"ray tracing pipeline binding\")"),
              std::string::npos);
    EXPECT_NE(setPipelineBody.find("!dx12Pipeline->IsValid() || !dx12Pipeline->GetStateObject()"),
              std::string::npos);
    EXPECT_NE(setPipelineBody.find("!dx12Pipeline->GetPipelineLayout() || !dx12Pipeline->GetRootSignature()"),
              std::string::npos);
    const auto rtBindingStateValidation = setPipelineBody.find("ValidateDX12RayTracingCommandState");
    const auto rtStateObjectValidityCheck = setPipelineBody.find("!dx12Pipeline->IsValid() || !dx12Pipeline->GetStateObject()");
    const auto rtRootSignatureValidityCheck = setPipelineBody.find("!dx12Pipeline->GetPipelineLayout() || !dx12Pipeline->GetRootSignature()");
    const auto rtStateObjectBind = setPipelineBody.find("SetPipelineState1(dx12Pipeline->GetStateObject())");
    const auto rtDescriptorSetTrackingResize =
        setPipelineBody.find("m_boundRayTracingDescriptorSetLayouts.resize(pipelineLayout->GetSetLayoutCount(), nullptr)");
    const auto rtCurrentPipelineAssign = setPipelineBody.find("m_currentPipeline = dx12Pipeline;", rtStateObjectBind);
    ASSERT_NE(rtBindingStateValidation, std::string::npos);
    ASSERT_NE(rtStateObjectValidityCheck, std::string::npos);
    ASSERT_NE(rtRootSignatureValidityCheck, std::string::npos);
    ASSERT_NE(rtStateObjectBind, std::string::npos);
    ASSERT_NE(rtDescriptorSetTrackingResize, std::string::npos);
    ASSERT_NE(rtCurrentPipelineAssign, std::string::npos);
    EXPECT_LT(rtBindingStateValidation, rtStateObjectValidityCheck);
    EXPECT_LT(rtStateObjectValidityCheck, rtRootSignatureValidityCheck);
    EXPECT_LT(rtRootSignatureValidityCheck, rtStateObjectBind);
    EXPECT_LT(rtStateObjectBind, rtDescriptorSetTrackingResize);
    EXPECT_LT(rtDescriptorSetTrackingResize, rtCurrentPipelineAssign);

    const auto setDescriptorSetStart = commandSource.find("void DX12CommandContext::SetDescriptorSet", setVertexBufferStart);
    const auto setPushConstantsStart = commandSource.find("void DX12CommandContext::SetPushConstants", setDescriptorSetStart);
    const auto setViewportStart = commandSource.find("void DX12CommandContext::SetViewport", setPushConstantsStart);
    ASSERT_NE(setDescriptorSetStart, std::string::npos);
    ASSERT_NE(setPushConstantsStart, std::string::npos);
    ASSERT_NE(setViewportStart, std::string::npos);

    const std::string setDescriptorSetBody =
        commandSource.substr(setDescriptorSetStart, setPushConstantsStart - setDescriptorSetStart);
    const std::string setPushConstantsBody =
        commandSource.substr(setPushConstantsStart, setViewportStart - setPushConstantsStart);
    EXPECT_NE(setDescriptorSetBody.find("bindingRayTracingPipeline"), std::string::npos);
    EXPECT_NE(setDescriptorSetBody.find("ValidateDX12RayTracingCommandState(m_isRecording, m_inRenderPass, \"ray tracing descriptor set binding\")"),
              std::string::npos);
    EXPECT_NE(setDescriptorSetBody.find("ray tracing descriptor set binding requires a valid DX12 descriptor set"),
              std::string::npos);
    EXPECT_NE(setDescriptorSetBody.find("auto* setLayout = dx12Set->GetLayout();"), std::string::npos);
    EXPECT_NE(setDescriptorSetBody.find("pipelineLayout->GetSetLayout(slot)"), std::string::npos);
    EXPECT_NE(setDescriptorSetBody.find("expectedLayout != setLayout"), std::string::npos);
    EXPECT_NE(setDescriptorSetBody.find("ray tracing descriptor set binding layout does not match the pipeline layout slot"),
              std::string::npos);
    EXPECT_NE(setDescriptorSetBody.find("m_boundRayTracingDescriptorSetLayouts[slot] = setLayout;"),
              std::string::npos);
    const auto rtDescriptorStateValidation = setDescriptorSetBody.find("ValidateDX12RayTracingCommandState");
    const auto rtDescriptorLayoutValidation = setDescriptorSetBody.find("pipelineLayout->GetSetLayout(slot)");
    const auto rtDescriptorLayoutMismatch = setDescriptorSetBody.find("expectedLayout != setLayout");
    const auto rtDescriptorTableBind = setDescriptorSetBody.find("SetComputeRootDescriptorTable");
    const auto rtDescriptorSetTracking = setDescriptorSetBody.find("m_boundRayTracingDescriptorSetLayouts[slot] = setLayout;");
    ASSERT_NE(rtDescriptorStateValidation, std::string::npos);
    ASSERT_NE(rtDescriptorLayoutValidation, std::string::npos);
    ASSERT_NE(rtDescriptorLayoutMismatch, std::string::npos);
    ASSERT_NE(rtDescriptorTableBind, std::string::npos);
    ASSERT_NE(rtDescriptorSetTracking, std::string::npos);
    EXPECT_LT(rtDescriptorStateValidation, rtDescriptorLayoutValidation);
    EXPECT_LT(rtDescriptorLayoutMismatch, rtDescriptorTableBind);
    EXPECT_LT(rtDescriptorTableBind, rtDescriptorSetTracking);

    EXPECT_NE(setPushConstantsBody.find("ValidateDX12RayTracingCommandState(m_isRecording, m_inRenderPass, \"ray tracing push constants binding\")"),
              std::string::npos);
    const auto rtPushConstantsStateValidation = setPushConstantsBody.find("ValidateDX12RayTracingCommandState");
    const auto rtPushConstantsWrite = setPushConstantsBody.find("SetComputeRoot32BitConstants");
    ASSERT_NE(rtPushConstantsStateValidation, std::string::npos);
    ASSERT_NE(rtPushConstantsWrite, std::string::npos);
    EXPECT_LT(rtPushConstantsStateValidation, rtPushConstantsWrite);

    const auto blasBuildStart =
        commandSource.find("void DX12CommandContext::BuildBottomLevelAccelerationStructure");
    const auto tlasBuildStart =
        commandSource.find("void DX12CommandContext::BuildTopLevelAccelerationStructure", blasBuildStart);
    const auto dispatchStart = commandSource.find("void DX12CommandContext::DispatchRays", tlasBuildStart);
    ASSERT_NE(blasBuildStart, std::string::npos);
    ASSERT_NE(tlasBuildStart, std::string::npos);
    ASSERT_NE(dispatchStart, std::string::npos);

    const std::string blasBuildBody = commandSource.substr(blasBuildStart, tlasBuildStart - blasBuildStart);
    const std::string tlasBuildBody = commandSource.substr(tlasBuildStart, dispatchStart - tlasBuildStart);
    const std::string dispatchBody = commandSource.substr(dispatchStart);
    EXPECT_NE(commandSource.find("D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE"),
              std::string::npos);
    EXPECT_NE(commandSource.find("bool performUpdate = false"), std::string::npos);
    EXPECT_NE(blasBuildBody.find("ValidateDX12RayTracingCommandState(m_isRecording, m_inRenderPass, \"BLAS build\")"),
              std::string::npos);
    EXPECT_NE(tlasBuildBody.find("ValidateDX12RayTracingCommandState(m_isRecording, m_inRenderPass, \"TLAS build\")"),
              std::string::npos);
    EXPECT_NE(dispatchBody.find("ValidateDX12RayTracingCommandState(m_isRecording, m_inRenderPass, \"DispatchRays\")"),
              std::string::npos);
    EXPECT_NE(blasBuildBody.find("const bool update = srcAS != nullptr;"), std::string::npos);
    EXPECT_NE(tlasBuildBody.find("const bool update = srcAS != nullptr;"), std::string::npos);
    EXPECT_NE(blasBuildBody.find("BLAS update requires AllowUpdate build flag"), std::string::npos);
    EXPECT_NE(tlasBuildBody.find("TLAS update requires AllowUpdate build flag"), std::string::npos);
    EXPECT_NE(blasBuildBody.find("if (!ValidateDX12BLASGeometryInputAddresses(desc))"), std::string::npos);
    EXPECT_NE(blasBuildBody.find("m_device->GetBottomLevelASBuildSizes(desc)"), std::string::npos);
    EXPECT_NE(tlasBuildBody.find("m_device->GetTopLevelASBuildSizes(desc)"), std::string::npos);
    EXPECT_NE(blasBuildBody.find("ValidateDX12ASBuildResources(\"BLAS build\", RHIAccelerationStructureType::BottomLevel, sizes, update, dstAS, srcAS, scratch, scratchOffset)"),
              std::string::npos);
    EXPECT_NE(tlasBuildBody.find("ValidateDX12ASBuildResources(\"TLAS build\", RHIAccelerationStructureType::TopLevel, sizes, update, dstAS, srcAS, scratch, scratchOffset)"),
              std::string::npos);
    const auto blasStateValidation = blasBuildBody.find("ValidateDX12RayTracingCommandState");
    const auto tlasStateValidation = tlasBuildBody.find("ValidateDX12RayTracingCommandState");
    const auto dispatchStateValidation = dispatchBody.find("ValidateDX12RayTracingCommandState");
    const auto dispatchDescValidation = dispatchBody.find("ValidateRHIDispatchRaysDesc(desc, m_currentPipeline)");
    const auto blasInputValidation = blasBuildBody.find("ValidateDX12BLASGeometryInputAddresses(desc)");
    const auto blasSizeQuery = blasBuildBody.find("m_device->GetBottomLevelASBuildSizes(desc)");
    const auto blasResourceValidation = blasBuildBody.find("ValidateDX12ASBuildResources");
    const auto blasGeometryBuild = blasBuildBody.find("BuildDX12GeometryDescs(desc)");
    const auto tlasSizeQuery = tlasBuildBody.find("m_device->GetTopLevelASBuildSizes(desc)");
    const auto tlasResourceValidation = tlasBuildBody.find("ValidateDX12ASBuildResources");
    ASSERT_NE(blasStateValidation, std::string::npos);
    ASSERT_NE(tlasStateValidation, std::string::npos);
    ASSERT_NE(dispatchStateValidation, std::string::npos);
    ASSERT_NE(dispatchDescValidation, std::string::npos);
    ASSERT_NE(blasInputValidation, std::string::npos);
    ASSERT_NE(blasSizeQuery, std::string::npos);
    ASSERT_NE(blasResourceValidation, std::string::npos);
    ASSERT_NE(blasGeometryBuild, std::string::npos);
    ASSERT_NE(tlasSizeQuery, std::string::npos);
    ASSERT_NE(tlasResourceValidation, std::string::npos);
    EXPECT_LT(blasStateValidation, blasInputValidation);
    EXPECT_LT(tlasStateValidation, tlasSizeQuery);
    EXPECT_LT(dispatchStateValidation, dispatchDescValidation);
    EXPECT_LT(blasInputValidation, blasSizeQuery);
    EXPECT_LT(blasSizeQuery, blasResourceValidation);
    EXPECT_LT(blasResourceValidation, blasGeometryBuild);
    EXPECT_LT(tlasSizeQuery, tlasResourceValidation);
    EXPECT_NE(blasBuildBody.find("buildDesc.Inputs.Flags = ToD3D12ASBuildFlags(desc.buildFlags, update);"),
              std::string::npos);
    EXPECT_NE(tlasBuildBody.find("buildDesc.Inputs.Flags = ToD3D12ASBuildFlags(desc.buildFlags, update);"),
              std::string::npos);
    EXPECT_NE(blasBuildBody.find("buildDesc.SourceAccelerationStructureData = update ? srcAS->GetGPUVirtualAddress() : 0;"),
              std::string::npos);
    EXPECT_NE(tlasBuildBody.find("buildDesc.SourceAccelerationStructureData = update ? srcAS->GetGPUVirtualAddress() : 0;"),
              std::string::npos);
    const auto blasDxrBuild = blasBuildBody.find("BuildRaytracingAccelerationStructure(&buildDesc, 0, nullptr);");
    const auto blasUavBarrier = blasBuildBody.find("InsertAccelerationStructureUAVBarrier(m_commandList.Get(), dstAS);");
    const auto tlasDxrBuild = tlasBuildBody.find("BuildRaytracingAccelerationStructure(&buildDesc, 0, nullptr);");
    const auto tlasUavBarrier = tlasBuildBody.find("InsertAccelerationStructureUAVBarrier(m_commandList.Get(), dstAS);");
    ASSERT_NE(blasDxrBuild, std::string::npos);
    ASSERT_NE(blasUavBarrier, std::string::npos);
    ASSERT_NE(tlasDxrBuild, std::string::npos);
    ASSERT_NE(tlasUavBarrier, std::string::npos);
    EXPECT_LT(blasDxrBuild, blasUavBarrier);
    EXPECT_LT(tlasDxrBuild, tlasUavBarrier);
    const auto shaderTablePipelineCheck = dispatchBody.find("shaderTable->GetPipeline() != m_currentPipeline");
    const auto dispatchDescriptorSetBindingCheck = dispatchBody.find("descriptorSetBound");
    const auto dispatchCall = dispatchBody.find("commandList4->DispatchRays(&d3dDesc);");
    ASSERT_NE(shaderTablePipelineCheck, std::string::npos);
    ASSERT_NE(dispatchDescriptorSetBindingCheck, std::string::npos);
    ASSERT_NE(dispatchCall, std::string::npos);
    EXPECT_LT(shaderTablePipelineCheck, dispatchDescriptorSetBindingCheck);
    EXPECT_LT(dispatchDescriptorSetBindingCheck, dispatchCall);
    EXPECT_NE(commandSource.find("commandList4->DispatchRays(&d3dDesc);"), std::string::npos);
}

TEST_F(PipelineCacheValidationFixture, VulkanBackendDoesNotAdvertiseRayTracingBeforeBackendImplementation)
{
    if (!HasCompilerAvailable())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    const fs::path repoRoot = FindShaderDirectory().parent_path().parent_path();
    const fs::path vulkanDir = repoRoot / "RHI_Vulkan" / "Private";
    const std::string deviceHeader = ReadTextFile(vulkanDir / "VulkanDevice.h");
    const std::string deviceSource = ReadTextFile(vulkanDir / "VulkanDevice.cpp");
    const std::string commandHeader = ReadTextFile(vulkanDir / "VulkanCommandContext.h");
    const std::string commandSource = ReadTextFile(vulkanDir / "VulkanCommandContext.cpp");

    EXPECT_EQ(deviceHeader.find("CreateRayTracingPipeline"), std::string::npos);
    EXPECT_EQ(deviceHeader.find("CreateShaderTable"), std::string::npos);
    EXPECT_EQ(deviceHeader.find("CreateAccelerationStructure"), std::string::npos);
    EXPECT_EQ(deviceHeader.find("GetBottomLevelASBuildSizes"), std::string::npos);
    EXPECT_EQ(commandHeader.find("BuildBottomLevelAccelerationStructure"), std::string::npos);
    EXPECT_EQ(commandHeader.find("BuildTopLevelAccelerationStructure"), std::string::npos);
    EXPECT_EQ(commandHeader.find("DispatchRays"), std::string::npos);
    EXPECT_EQ(commandSource.find("BuildBottomLevelAccelerationStructure"), std::string::npos);
    EXPECT_EQ(commandSource.find("BuildTopLevelAccelerationStructure"), std::string::npos);
    EXPECT_EQ(commandSource.find("DispatchRays"), std::string::npos);

    EXPECT_NE(deviceSource.find("const bool hasRayTracingExtensions ="), std::string::npos);
    EXPECT_NE(deviceSource.find("VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME"), std::string::npos);
    EXPECT_NE(deviceSource.find("m_capabilities.supportsRaytracing = false;"), std::string::npos);
    EXPECT_NE(deviceSource.find("m_capabilities.supportsRaytracingPipeline = false;"), std::string::npos);
    EXPECT_NE(deviceSource.find("m_capabilities.supportsRayQuery = false;"), std::string::npos);
    EXPECT_NE(deviceSource.find("m_capabilities.supportsAccelerationStructureUpdate = false;"),
              std::string::npos);
    EXPECT_NE(deviceSource.find("m_capabilities.supportsAccelerationStructureCompaction = false;"),
              std::string::npos);
    EXPECT_NE(deviceSource.find("m_capabilities.maxRayRecursionDepth = 0;"), std::string::npos);
    EXPECT_NE(deviceSource.find("RaytracingExtensions={}"), std::string::npos);
}

TEST_F(PipelineCacheValidationFixture, ToneMappingOperatorUsesSharedRuntimeSettings)
{
    if (!HasCompilerAvailable())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    const fs::path renderRoot = FindShaderDirectory().parent_path();
    const fs::path postProcessIncludeDir = renderRoot / "Include" / "Render" / "PostProcess";
    const std::string settingsHeader = ReadTextFile(postProcessIncludeDir / "PostProcessStack.h");
    const std::string toneMappingHeader = ReadTextFile(postProcessIncludeDir / "ToneMapping.h");
    const std::string toneMappingTypesHeader = ReadTextFile(postProcessIncludeDir / "ToneMappingTypes.h");
    const std::string toneMappingSource =
        ReadTextFile(renderRoot / "Private" / "PostProcess" / "ToneMapping.cpp");
    const std::string sceneRendererSource =
        ReadTextFile(renderRoot / "Private" / "Renderer" / "SceneRenderer.cpp");

    EXPECT_NE(toneMappingTypesHeader.find("enum class ToneMappingOperator : uint8"), std::string::npos);
    EXPECT_NE(toneMappingTypesHeader.find("enum class ToneMappingExposureMode : uint8"), std::string::npos);
    EXPECT_NE(toneMappingTypesHeader.find("enum class ToneMappingOutputColorSpace : uint8"), std::string::npos);
    EXPECT_NE(toneMappingTypesHeader.find("ManualMultiplier"), std::string::npos);
    EXPECT_NE(toneMappingTypesHeader.find("CameraEV100"), std::string::npos);
    EXPECT_NE(toneMappingTypesHeader.find("Linear"), std::string::npos);
    EXPECT_NE(toneMappingTypesHeader.find("SRGB"), std::string::npos);
    EXPECT_EQ(toneMappingHeader.find("enum class ToneMappingOperator"), std::string::npos);
    EXPECT_NE(settingsHeader.find("#include \"Render/PostProcess/ToneMappingTypes.h\""), std::string::npos);
    EXPECT_NE(toneMappingHeader.find("#include \"Render/PostProcess/ToneMappingTypes.h\""), std::string::npos);
    EXPECT_EQ(settingsHeader.find("#include \"Render/PostProcess/ToneMapping.h\""), std::string::npos);
    EXPECT_NE(settingsHeader.find("ToneMappingExposureMode exposureMode = ToneMappingExposureMode::ManualMultiplier;"),
              std::string::npos);
    EXPECT_NE(settingsHeader.find("float cameraEV100 = 0.0f;"), std::string::npos);
    EXPECT_NE(settingsHeader.find("float exposureCompensationEV = 0.0f;"), std::string::npos);
    EXPECT_NE(settingsHeader.find("ToneMappingOperator toneMappingOperator = ToneMappingOperator::ACES;"),
              std::string::npos);
    EXPECT_NE(settingsHeader.find("ToneMappingOutputColorSpace toneMappingOutputColorSpace = ToneMappingOutputColorSpace::SRGB;"),
              std::string::npos);
    EXPECT_NE(settingsHeader.find("bool enableRayTracedReflections = false;"), std::string::npos);
    EXPECT_NE(settingsHeader.find("bool enableRayTracedReflectionDenoise = true;"), std::string::npos);
    EXPECT_NE(settingsHeader.find("float rayTracedReflectionIntensity = 1.0f;"), std::string::npos);
    EXPECT_NE(settingsHeader.find("float rayTracedReflectionResolutionScale = 1.0f;"), std::string::npos);
    EXPECT_NE(settingsHeader.find("float rayTracedReflectionMaxDistance = 50.0f;"), std::string::npos);
    EXPECT_NE(settingsHeader.find("float rayTracedReflectionMaxRoughness = 1.0f;"), std::string::npos);
    EXPECT_NE(settingsHeader.find("float rayTracedReflectionDistanceFadeStart = 0.8f;"), std::string::npos);
    EXPECT_NE(settingsHeader.find("uint32 rayTracedReflectionInstanceMask = 0xFFu;"), std::string::npos);
    EXPECT_NE(settingsHeader.find("uint32 rayTracedReflectionSamplesPerPixel = 1;"), std::string::npos);
    EXPECT_NE(settingsHeader.find("float rayTracedReflectionRoughnessConeSpread = 1.0f;"), std::string::npos);
    EXPECT_NE(settingsHeader.find("float rayTracedReflectionNormalBias = 0.02f;"), std::string::npos);
    EXPECT_NE(settingsHeader.find("float rayTracedReflectionRayMinT = 0.001f;"), std::string::npos);
    EXPECT_NE(settingsHeader.find("float rayTracedReflectionFireflyClamp = 64.0f;"), std::string::npos);
    EXPECT_NE(settingsHeader.find("float rayTracedReflectionTemporalBlendFactor = 0.85f;"), std::string::npos);
    EXPECT_NE(settingsHeader.find("float rayTracedReflectionHistoryLuminanceTolerance = 4.0f;"), std::string::npos);
    EXPECT_NE(settingsHeader.find("float rayTracedReflectionHistoryConfidenceThreshold = 0.05f;"), std::string::npos);
    EXPECT_NE(settingsHeader.find("float rayTracedReflectionHistoryVelocityRejectionScale = 8.0f;"), std::string::npos);
    EXPECT_NE(settingsHeader.find("uint32 rayTracedReflectionDenoiseRadius = 1;"), std::string::npos);
    EXPECT_NE(settingsHeader.find("float rayTracedReflectionDenoiseNormalThreshold = 0.85f;"),
              std::string::npos);
    EXPECT_NE(settingsHeader.find("float rayTracedReflectionDenoiseCenterWeight = 1.0f;"), std::string::npos);
    EXPECT_NE(settingsHeader.find("float rayTracedReflectionDenoiseLowConfidenceDepthScale = 4.0f;"), std::string::npos);
    EXPECT_NE(toneMappingSource.find("float ResolveToneMappingExposure(const PostProcessSettings& settings)"),
              std::string::npos);
    EXPECT_NE(toneMappingSource.find("settings.exposureMode == ToneMappingExposureMode::CameraEV100"),
              std::string::npos);
    EXPECT_NE(toneMappingSource.find("settings.exposureCompensationEV - settings.cameraEV100"),
              std::string::npos);
    EXPECT_NE(toneMappingSource.find("std::clamp(settings.exposureCompensationEV - settings.cameraEV100"),
              std::string::npos);
    EXPECT_NE(toneMappingSource.find("std::pow(2.0f, evDelta)"), std::string::npos);
    EXPECT_NE(toneMappingSource.find("RVX_TONE_MAPPING_FALLBACK_EXPOSURE"), std::string::npos);
    EXPECT_NE(toneMappingSource.find("return SanitizeManualExposure(settings.exposure);"), std::string::npos);
    EXPECT_NE(toneMappingSource.find("m_exposure = ResolveToneMappingExposure(settings);"), std::string::npos);
    EXPECT_NE(toneMappingSource.find("m_operator = settings.toneMappingOperator;"), std::string::npos);
    EXPECT_NE(toneMappingSource.find("m_outputColorSpace = settings.toneMappingOutputColorSpace;"),
              std::string::npos);
    EXPECT_NE(toneMappingSource.find("constants.outputColorSpace = static_cast<uint32>(outputColorSpace);"),
              std::string::npos);
    EXPECT_NE(sceneRendererSource.find("ToneMappingOutputColorSpace SceneRenderer::ResolveToneMappingOutputColorSpace"),
              std::string::npos);
    EXPECT_NE(sceneRendererSource.find("IsSRGBFormat(outputFormat)"), std::string::npos);
    EXPECT_NE(sceneRendererSource.find("return ToneMappingOutputColorSpace::Linear;"), std::string::npos);
    EXPECT_NE(sceneRendererSource.find("return ToneMappingOutputColorSpace::SRGB;"), std::string::npos);
    EXPECT_NE(sceneRendererSource.find("policy.toneMappingOutputColorSpace = ResolveToneMappingOutputColorSpace(policy.toneMappingOutputFormat);"),
              std::string::npos);
    EXPECT_NE(sceneRendererSource.find("m_postProcessSettings.toneMappingOutputColorSpace = m_sceneColorFormatPolicy.toneMappingOutputColorSpace;"),
              std::string::npos);
    EXPECT_NE(sceneRendererSource.find("m_postProcessStats.toneMappingOutputColorSpace = m_sceneColorFormatPolicy.toneMappingOutputColorSpace;"),
              std::string::npos);
    EXPECT_NE(sceneRendererSource.find("diagnostics.toneMappingOutputColorSpace = m_postProcessStats.toneMappingOutputColorSpace;"),
              std::string::npos);

    const auto defaultsStart = sceneRendererSource.find("PostProcessSettings MakeDefaultRuntimePostProcessSettings()");
    ASSERT_NE(defaultsStart, std::string::npos);
    const auto defaultsEnd = sceneRendererSource.find("SceneRenderer::SceneRenderer()", defaultsStart);
    ASSERT_NE(defaultsEnd, std::string::npos);
    const std::string defaultsBody = sceneRendererSource.substr(defaultsStart, defaultsEnd - defaultsStart);
    EXPECT_NE(defaultsBody.find("settings.toneMappingOperator = ToneMappingOperator::None;"), std::string::npos);
    EXPECT_NE(defaultsBody.find("settings.enableRayTracedReflections = false;"), std::string::npos);
    EXPECT_NE(defaultsBody.find("settings.enableRayTracedReflectionDenoise = true;"), std::string::npos);

    const auto setupStart = sceneRendererSource.find("void SceneRenderer::SetupDefaultPostProcess()");
    ASSERT_NE(setupStart, std::string::npos);
    const auto setupEnd = sceneRendererSource.find("void SceneRenderer::SetupDefaultPasses()", setupStart);
    ASSERT_NE(setupEnd, std::string::npos);
    const std::string setupBody = sceneRendererSource.substr(setupStart, setupEnd - setupStart);
    EXPECT_EQ(setupBody.find("SetOperator(ToneMappingOperator::None)"), std::string::npos);
}

TEST_F(PipelineCacheValidationFixture, ModelViewerExposesShadowQualityPresets)
{
    if (!HasCompilerAvailable())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    const fs::path modelViewerPath = FindModelViewerSourcePath();
    ASSERT_FALSE(modelViewerPath.empty());
    const std::string source = ReadTextFile(modelViewerPath);

    EXPECT_NE(source.find("--shadow-quality <default|low|medium|high|ultra>"), std::string::npos);
    EXPECT_NE(source.find("enum class ShadowQualityPreset"), std::string::npos);
    EXPECT_NE(source.find("bool ParseShadowQualityPreset"), std::string::npos);
    EXPECT_NE(source.find("value == \"default\""), std::string::npos);
    EXPECT_NE(source.find("value == \"low\""), std::string::npos);
    EXPECT_NE(source.find("value == \"medium\""), std::string::npos);
    EXPECT_NE(source.find("value == \"high\""), std::string::npos);
    EXPECT_NE(source.find("value == \"ultra\""), std::string::npos);
    EXPECT_NE(source.find("ShadowPassConfig MakeShadowQualityConfig"), std::string::npos);
    EXPECT_NE(source.find("return ShadowPassConfig{};"), std::string::npos);
    EXPECT_NE(source.find("config.casterDepthBias = 0.0f;"), std::string::npos);
    EXPECT_NE(source.find("config.casterSlopeScaledDepthBias = 0.0f;"), std::string::npos);
    EXPECT_NE(source.find("config.casterDepthBiasClamp = 0.0f;"), std::string::npos);
    EXPECT_NE(source.find("MakeShadowQualityConfig(options.shadowQualityPreset)"), std::string::npos);
    EXPECT_NE(source.find("sceneRenderer->ApplyShadowPassConfig(shadowConfig);"), std::string::npos);
}

TEST_F(PipelineCacheValidationFixture, ModelViewerExposesTonemapOperatorSelection)
{
    if (!HasCompilerAvailable())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    const fs::path modelViewerPath = FindModelViewerSourcePath();
    ASSERT_FALSE(modelViewerPath.empty());
    const std::string source = ReadTextFile(modelViewerPath);

    EXPECT_NE(source.find("#include \"Render/PostProcess/ToneMappingTypes.h\""), std::string::npos);
    EXPECT_NE(source.find("--tonemap <default|none|reinhard|reinhard-extended|aces|uncharted2|neutral>"),
              std::string::npos);
    EXPECT_NE(source.find("--post-exposure <linear>"), std::string::npos);
    EXPECT_NE(source.find("range [0.0, 64.0]"), std::string::npos);
    EXPECT_NE(source.find("--camera-ev100 <value>"), std::string::npos);
    EXPECT_NE(source.find("range [-16.0, 32.0]"), std::string::npos);
    EXPECT_NE(source.find("--exposure-compensation <ev>"), std::string::npos);
    EXPECT_NE(source.find("range [-16.0, 16.0]"), std::string::npos);
    EXPECT_NE(source.find("--display-gamma <value>"), std::string::npos);
    EXPECT_NE(source.find("range [0.1, 10.0]"), std::string::npos);
    EXPECT_NE(source.find("--bloom-intensity <value>"), std::string::npos);
    EXPECT_NE(source.find("range [0.0, 16.0]"), std::string::npos);
    EXPECT_NE(source.find("--bloom-threshold <value>"), std::string::npos);
    EXPECT_NE(source.find("range [0.0, 64.0]"), std::string::npos);
    EXPECT_NE(source.find("--bloom-radius <texels>"), std::string::npos);
    EXPECT_NE(source.find("enum class ToneMapSelection"), std::string::npos);
    EXPECT_NE(source.find("bool postExposureSet = false;"), std::string::npos);
    EXPECT_NE(source.find("bool displayGammaSet = false;"), std::string::npos);
    EXPECT_NE(source.find("bool cameraEV100Set = false;"), std::string::npos);
    EXPECT_NE(source.find("bool exposureCompensationSet = false;"), std::string::npos);
    EXPECT_NE(source.find("bool bloomIntensitySet = false;"), std::string::npos);
    EXPECT_NE(source.find("bool bloomThresholdSet = false;"), std::string::npos);
    EXPECT_NE(source.find("bool bloomRadiusSet = false;"), std::string::npos);
    EXPECT_NE(source.find("float postExposure = 1.0f;"), std::string::npos);
    EXPECT_NE(source.find("float displayGamma = 2.2f;"), std::string::npos);
    EXPECT_NE(source.find("float cameraEV100 = 0.0f;"), std::string::npos);
    EXPECT_NE(source.find("float exposureCompensationEV = 0.0f;"), std::string::npos);
    EXPECT_NE(source.find("float bloomIntensity = 0.0f;"), std::string::npos);
    EXPECT_NE(source.find("float bloomThreshold = 1.0f;"), std::string::npos);
    EXPECT_NE(source.find("float bloomRadius = 0.5f;"), std::string::npos);

    const auto parseStart = source.find("bool ParseToneMapSelection");
    ASSERT_NE(parseStart, std::string::npos);
    const auto parseEnd = source.find("bool TryGetToneMappingOperator", parseStart);
    ASSERT_NE(parseEnd, std::string::npos);
    const std::string parseBody = source.substr(parseStart, parseEnd - parseStart);
    EXPECT_NE(parseBody.find("value == \"default\""), std::string::npos);
    EXPECT_NE(parseBody.find("value == \"none\""), std::string::npos);
    EXPECT_NE(parseBody.find("value == \"reinhard\""), std::string::npos);
    EXPECT_NE(parseBody.find("value == \"reinhard-extended\""), std::string::npos);
    EXPECT_NE(parseBody.find("value == \"aces\""), std::string::npos);
    EXPECT_NE(parseBody.find("value == \"uncharted2\""), std::string::npos);
    EXPECT_NE(parseBody.find("value == \"neutral\""), std::string::npos);

    const auto operatorStart = source.find("bool TryGetToneMappingOperator");
    ASSERT_NE(operatorStart, std::string::npos);
    const auto operatorEnd = source.find("ShadowPassConfig MakeShadowQualityConfig", operatorStart);
    ASSERT_NE(operatorEnd, std::string::npos);
    const std::string operatorBody = source.substr(operatorStart, operatorEnd - operatorStart);
    EXPECT_NE(operatorBody.find("outOperator = ToneMappingOperator::None;"), std::string::npos);
    EXPECT_NE(operatorBody.find("outOperator = ToneMappingOperator::Reinhard;"), std::string::npos);
    EXPECT_NE(operatorBody.find("outOperator = ToneMappingOperator::ReinhardExtended;"), std::string::npos);
    EXPECT_NE(operatorBody.find("outOperator = ToneMappingOperator::ACES;"), std::string::npos);
    EXPECT_NE(operatorBody.find("outOperator = ToneMappingOperator::Uncharted2;"), std::string::npos);
    EXPECT_NE(operatorBody.find("outOperator = ToneMappingOperator::Neutral;"), std::string::npos);

    const auto parseFloatStart = source.find("bool ParseFloat");
    ASSERT_NE(parseFloatStart, std::string::npos);
    const auto parseFloatEnd = source.find("bool ParseOptions", parseFloatStart);
    ASSERT_NE(parseFloatEnd, std::string::npos);
    const std::string parseFloatBody = source.substr(parseFloatStart, parseFloatEnd - parseFloatStart);
    EXPECT_NE(parseFloatBody.find("std::stof(text, &parsedChars)"), std::string::npos);
    EXPECT_NE(parseFloatBody.find("parsedChars != std::strlen(text)"), std::string::npos);
    EXPECT_NE(parseFloatBody.find("!std::isfinite(parsed)"), std::string::npos);

    EXPECT_NE(source.find("arg == \"--post-exposure\""), std::string::npos);
    EXPECT_NE(source.find("parsed < 0.0f || parsed > 64.0f"), std::string::npos);
    EXPECT_NE(source.find("Invalid --post-exposure value"), std::string::npos);
    EXPECT_NE(source.find("arg == \"--camera-ev100\""), std::string::npos);
    EXPECT_NE(source.find("parsed < -16.0f || parsed > 32.0f"), std::string::npos);
    EXPECT_NE(source.find("Invalid --camera-ev100 value"), std::string::npos);
    EXPECT_NE(source.find("arg == \"--exposure-compensation\""), std::string::npos);
    EXPECT_NE(source.find("parsed < -16.0f || parsed > 16.0f"), std::string::npos);
    EXPECT_NE(source.find("Invalid --exposure-compensation value"), std::string::npos);
    EXPECT_NE(source.find("options.cameraEV100Set && options.postExposureSet"), std::string::npos);
    EXPECT_NE(source.find("--camera-ev100 cannot be combined with --post-exposure"), std::string::npos);
    EXPECT_NE(source.find("options.exposureCompensationSet && !options.cameraEV100Set"), std::string::npos);
    EXPECT_NE(source.find("--exposure-compensation requires --camera-ev100"), std::string::npos);
    EXPECT_NE(source.find("arg == \"--display-gamma\""), std::string::npos);
    EXPECT_NE(source.find("parsed < 0.1f || parsed > 10.0f"), std::string::npos);
    EXPECT_NE(source.find("Invalid --display-gamma value"), std::string::npos);
    EXPECT_NE(source.find("arg == \"--bloom-intensity\""), std::string::npos);
    EXPECT_NE(source.find("parsed < 0.0f || parsed > 16.0f"), std::string::npos);
    EXPECT_NE(source.find("Invalid --bloom-intensity value"), std::string::npos);
    EXPECT_NE(source.find("arg == \"--bloom-threshold\""), std::string::npos);
    EXPECT_NE(source.find("Invalid --bloom-threshold value"), std::string::npos);
    EXPECT_NE(source.find("arg == \"--bloom-radius\""), std::string::npos);
    EXPECT_NE(source.find("Invalid --bloom-radius value"), std::string::npos);
    EXPECT_NE(source.find("PostProcessSettings postProcessSettings = sceneRenderer->GetPostProcessSettings();"),
              std::string::npos);
    EXPECT_NE(source.find("postProcessSettings.toneMappingOperator = tonemapOperator;"), std::string::npos);
    EXPECT_NE(source.find("postProcessSettings.exposureMode = ToneMappingExposureMode::CameraEV100;"),
              std::string::npos);
    EXPECT_NE(source.find("postProcessSettings.cameraEV100 = options.cameraEV100;"), std::string::npos);
    EXPECT_NE(source.find("postProcessSettings.exposureCompensationEV ="), std::string::npos);
    EXPECT_NE(source.find("options.exposureCompensationSet ? options.exposureCompensationEV : 0.0f"),
              std::string::npos);
    EXPECT_NE(source.find("postProcessSettings.exposureMode = ToneMappingExposureMode::ManualMultiplier;"),
              std::string::npos);
    EXPECT_NE(source.find("postProcessSettings.exposure = options.postExposure;"), std::string::npos);
    EXPECT_NE(source.find("postProcessSettings.gamma = options.displayGamma;"), std::string::npos);
    EXPECT_NE(source.find("postProcessSettings.enableBloom = true;"), std::string::npos);
    EXPECT_NE(source.find("postProcessSettings.bloomIntensity = options.bloomIntensity;"), std::string::npos);
    EXPECT_NE(source.find("postProcessSettings.bloomThreshold = options.bloomThreshold;"), std::string::npos);
    EXPECT_NE(source.find("postProcessSettings.bloomRadius = options.bloomRadius;"), std::string::npos);
    EXPECT_NE(source.find("bloomIntensity={:.3f}"), std::string::npos);
    EXPECT_NE(source.find("bloomThreshold={:.3f}"), std::string::npos);
    EXPECT_NE(source.find("bloomRadius={:.3f}"), std::string::npos);
    EXPECT_NE(source.find("sceneRenderer->ApplyPostProcessSettings(postProcessSettings);"), std::string::npos);
    EXPECT_NE(source.find("TryGetToneMappingOperator(options.tonemapSelection, tonemapOperator)"),
              std::string::npos);
    EXPECT_NE(source.find("bool applyPostProcessSettings ="), std::string::npos);
    EXPECT_NE(source.find("tonemapSet || options.postExposureSet || options.cameraEV100Set ||"), std::string::npos);
    EXPECT_NE(source.find("options.exposureCompensationSet || options.displayGammaSet ||"), std::string::npos);
    EXPECT_NE(source.find("options.bloomIntensitySet || options.bloomThresholdSet || options.bloomRadiusSet ||"),
              std::string::npos);
    EXPECT_NE(source.find("options.enableRayTracedReflections;"), std::string::npos);
    EXPECT_NE(source.find("Invalid --tonemap value"), std::string::npos);
    EXPECT_EQ(source.find("options.tonemapSelection = ToneMapSelection::ACES"), std::string::npos);

    const auto applyStart = source.find("if (applyPostProcessSettings)");
    ASSERT_NE(applyStart, std::string::npos);
    const auto applyEnd = source.find("const ShadowPassConfig shadowConfig", applyStart);
    ASSERT_NE(applyEnd, std::string::npos);
    const std::string applyBody = source.substr(applyStart, applyEnd - applyStart);
    EXPECT_NE(applyBody.find("sceneRenderer->ApplyPostProcessSettings(postProcessSettings);"), std::string::npos);

    auto countOccurrences = [](const std::string& text, const std::string& needle)
    {
        size_t count = 0;
        size_t pos = text.find(needle);
        while (pos != std::string::npos)
        {
            ++count;
            pos = text.find(needle, pos + needle.size());
        }
        return count;
    };
    EXPECT_EQ(countOccurrences(source, "sceneRenderer->ApplyPostProcessSettings(postProcessSettings);"),
              static_cast<size_t>(1));
}

TEST_F(PipelineCacheValidationFixture, ModelViewerRayTracingSmokeGatesAreObservable)
{
    if (!HasCompilerAvailable())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    const fs::path modelViewerPath = FindModelViewerSourcePath();
    ASSERT_FALSE(modelViewerPath.empty());
    const std::string source = ReadTextFile(modelViewerPath);

    EXPECT_NE(source.find("--ray-traced-reflections"), std::string::npos);
    EXPECT_NE(source.find("--no-rt-reflection-denoise"), std::string::npos);
    EXPECT_NE(source.find("--rt-max-rays"), std::string::npos);
    EXPECT_NE(source.find("--rt-max-denoise-taps"), std::string::npos);
    EXPECT_NE(source.find("--rt-max-resource-bytes"), std::string::npos);
    EXPECT_NE(source.find("--rt-max-gpu-ms"), std::string::npos);
    EXPECT_NE(source.find("--rt-max-shadow-gpu-ms"), std::string::npos);
    EXPECT_NE(source.find("--rt-max-reflection-gpu-ms"), std::string::npos);
    EXPECT_NE(source.find("--rt-recovery-reflection-gpu-ms"), std::string::npos);
    EXPECT_NE(source.find("--rt-gpu-adjust-frames"), std::string::npos);
    EXPECT_NE(source.find("--rt-reset-history-frame"), std::string::npos);
    EXPECT_NE(source.find("--rt-resize-frame"), std::string::npos);
    EXPECT_NE(source.find("--rt-resize-width"), std::string::npos);
    EXPECT_NE(source.find("--rt-resize-height"), std::string::npos);
    EXPECT_NE(source.find("--expect-ray-traced-shadow-ready"), std::string::npos);
    EXPECT_NE(source.find("--expect-ray-traced-shadow-history-ready"), std::string::npos);
    EXPECT_NE(source.find("--expect-ray-traced-shadow-history-reset-ready"), std::string::npos);
    EXPECT_NE(source.find("--expect-ray-traced-shadow-history-resize-ready"), std::string::npos);
    EXPECT_NE(source.find("--expect-ray-traced-reflection-ready"), std::string::npos);
    EXPECT_NE(source.find("--expect-ray-traced-reflection-history-ready"), std::string::npos);
    EXPECT_NE(source.find("--expect-ray-traced-reflection-history-reset-ready"), std::string::npos);
    EXPECT_NE(source.find("--expect-ray-traced-reflection-history-resize-ready"), std::string::npos);
    EXPECT_NE(source.find("--expect-ray-tracing-budget-applied"), std::string::npos);
    EXPECT_NE(source.find("--expect-ray-tracing-ray-budget-respected"), std::string::npos);
    EXPECT_NE(source.find("--expect-ray-tracing-resource-budget-exceeded"), std::string::npos);
    EXPECT_NE(source.find("--expect-ray-tracing-gpu-timing-ready"), std::string::npos);
    EXPECT_NE(source.find("--expect-ray-tracing-gpu-budget-applied"), std::string::npos);
    EXPECT_NE(source.find("--expect-ray-tracing-gpu-budget-recovered"), std::string::npos);
    EXPECT_NE(source.find("bool expectRayTracedShadowReady = false;"), std::string::npos);
    EXPECT_NE(source.find("bool expectRayTracedShadowHistoryReady = false;"), std::string::npos);
    EXPECT_NE(source.find("bool expectRayTracedShadowHistoryResetReady = false;"), std::string::npos);
    EXPECT_NE(source.find("bool expectRayTracedShadowHistoryResizeReady = false;"), std::string::npos);
    EXPECT_NE(source.find("bool expectRayTracedReflectionReady = false;"), std::string::npos);
    EXPECT_NE(source.find("bool expectRayTracedReflectionHistoryReady = false;"), std::string::npos);
    EXPECT_NE(source.find("bool expectRayTracedReflectionHistoryResetReady = false;"), std::string::npos);
    EXPECT_NE(source.find("bool expectRayTracedReflectionHistoryResizeReady = false;"), std::string::npos);
    EXPECT_NE(source.find("bool enableRayTracedReflections = false;"), std::string::npos);
    EXPECT_NE(source.find("bool enableRayTracedReflectionDenoise = true;"), std::string::npos);
    EXPECT_NE(source.find("uint64 rayTracingMaxRayCount = 0;"), std::string::npos);
    EXPECT_NE(source.find("uint64 rayTracingMaxDenoiseTapCount = 0;"), std::string::npos);
    EXPECT_NE(source.find("uint64 rayTracingMaxResourceBytes = 0;"), std::string::npos);
    EXPECT_NE(source.find("float rayTracingMaxGpuMs = 0.0f;"), std::string::npos);
    EXPECT_NE(source.find("float rayTracingMaxShadowGpuMs = 0.0f;"), std::string::npos);
    EXPECT_NE(source.find("float rayTracingMaxReflectionGpuMs = 0.0f;"), std::string::npos);
    EXPECT_NE(source.find("float rayTracingRecoveryMaxReflectionGpuMs = 0.0f;"), std::string::npos);
    EXPECT_NE(source.find("uint32 rayTracingGpuAdjustmentFrameCount = 0;"), std::string::npos);
    EXPECT_NE(source.find("uint32 rayTracingHistoryResetFrame = 0;"), std::string::npos);
    EXPECT_NE(source.find("uint32 rayTracingResizeFrame = 0;"), std::string::npos);
    EXPECT_NE(source.find("uint32 rayTracingResizeWidth = 0;"), std::string::npos);
    EXPECT_NE(source.find("uint32 rayTracingResizeHeight = 0;"), std::string::npos);
    EXPECT_NE(source.find("bool expectRayTracingBudgetApplied = false;"), std::string::npos);
    EXPECT_NE(source.find("bool expectRayTracingRayBudgetRespected = false;"), std::string::npos);
    EXPECT_NE(source.find("bool expectRayTracingResourceBudgetExceeded = false;"), std::string::npos);
    EXPECT_NE(source.find("bool expectRayTracingGpuTimingReady = false;"), std::string::npos);
    EXPECT_NE(source.find("bool expectRayTracingGpuBudgetApplied = false;"), std::string::npos);
    EXPECT_NE(source.find("bool expectRayTracingGpuBudgetRecovered = false;"), std::string::npos);
    EXPECT_NE(source.find("const bool dx12SmokeRequested = rayTracingSmokeRequested ||"),
              std::string::npos);
    EXPECT_NE(source.find("options.backend = dx12SmokeRequested ? RHIBackendType::DX12 : RHIBackendType::DX11;"),
              std::string::npos);
    EXPECT_NE(source.find("postProcessSettings.enableRayTracedReflections = true;"), std::string::npos);
    EXPECT_NE(source.find("postProcessSettings.enableRayTracedReflectionDenoise = options.enableRayTracedReflectionDenoise;"),
              std::string::npos);
    EXPECT_NE(source.find("postProcessSettings.enableTAA = true;"), std::string::npos);
    EXPECT_NE(source.find("bool IsRayTracedShadowReady(SceneRenderer* sceneRenderer"), std::string::npos);
    EXPECT_NE(source.find("bool IsRayTracedShadowHistoryReady(SceneRenderer* sceneRenderer"), std::string::npos);
    EXPECT_NE(source.find("bool IsRayTracedShadowHistoryResetReady(SceneRenderer* sceneRenderer"), std::string::npos);
    EXPECT_NE(source.find("bool IsRayTracedShadowHistoryResizeReady(SceneRenderer* sceneRenderer"), std::string::npos);
    EXPECT_NE(source.find("bool IsRayTracedReflectionReady(SceneRenderer* sceneRenderer,"), std::string::npos);
    EXPECT_NE(source.find("bool IsRayTracedReflectionHistoryReady(SceneRenderer* sceneRenderer"), std::string::npos);
    EXPECT_NE(source.find("bool IsRayTracedReflectionHistoryResetReady(SceneRenderer* sceneRenderer"), std::string::npos);
    EXPECT_NE(source.find("bool IsRayTracedReflectionHistoryResizeReady(SceneRenderer* sceneRenderer"), std::string::npos);
    EXPECT_NE(source.find("const SceneRayTracingFrameStats stats = sceneRenderer->GetRayTracingFrameStats();"),
              std::string::npos);
    EXPECT_NE(source.find("sceneRenderer->ApplyRayTracingBudgetSettings(budgetSettings);"), std::string::npos);
    EXPECT_NE(source.find("budgetSettings.maxTrackedResourceBytes = options.rayTracingMaxResourceBytes;"),
              std::string::npos);
    EXPECT_NE(source.find("budgetSettings.maxMeasuredGpuMs = options.rayTracingMaxGpuMs;"), std::string::npos);
    EXPECT_NE(source.find("budgetSettings.maxReflectionMeasuredGpuMs = options.rayTracingMaxReflectionGpuMs;"), std::string::npos);
    EXPECT_NE(source.find("budgetSettings.gpuTimingAdjustmentFrameCount = options.rayTracingGpuAdjustmentFrameCount;"),
              std::string::npos);
    EXPECT_NE(source.find("DescribeRayTracingShadowHistoryReadiness"), std::string::npos);
    EXPECT_NE(source.find("DescribeRayTracingReflectionHistoryReadiness"), std::string::npos);
    EXPECT_NE(source.find("DescribeRayTracingBudgetReadiness"), std::string::npos);
    EXPECT_NE(source.find("cachedBLASCount="), std::string::npos);
    EXPECT_NE(source.find("evictedBLASCount="), std::string::npos);
    EXPECT_NE(source.find("trackedResourceBudget="), std::string::npos);
    EXPECT_NE(source.find("resourceBudgetExceeded="), std::string::npos);
    EXPECT_NE(source.find("resourceBudgetEvictedBLASCount="), std::string::npos);
    EXPECT_NE(source.find("releasedBLASScratchCount="), std::string::npos);
    EXPECT_NE(source.find("pendingBLASScratchReleaseCount="), std::string::npos);
    EXPECT_NE(source.find("releasedBLASScratchBytes="), std::string::npos);
    EXPECT_NE(source.find("resourceBudgetEvictionAttempted="), std::string::npos);
    EXPECT_NE(source.find("resourceByteAccountingOverflowed="), std::string::npos);
    EXPECT_NE(source.find("rtTrackedResourceBytes="), std::string::npos);
    EXPECT_NE(source.find("cachedBLASAccelerationStructureBytes="), std::string::npos);
    EXPECT_NE(source.find("topLevelAccelerationStructureBytes="), std::string::npos);
    EXPECT_NE(source.find("materialMetadataBufferBytes="), std::string::npos);
    EXPECT_NE(source.find("IsRayTracingResourceBudgetExceeded"), std::string::npos);
    EXPECT_NE(source.find("DescribeRayTracingGpuTimingReadiness"), std::string::npos);
    EXPECT_NE(source.find("IsRayTracingGpuTimingReady"), std::string::npos);
    EXPECT_NE(source.find("DescribeRayTracingGpuBudgetReadiness"), std::string::npos);
    EXPECT_NE(source.find("IsRayTracingGpuBudgetApplied"), std::string::npos);
    EXPECT_NE(source.find("DescribeRayTracingGpuBudgetRecoveryReadiness"), std::string::npos);
    EXPECT_NE(source.find("IsRayTracingGpuBudgetRecovered"), std::string::npos);
    EXPECT_NE(source.find("ApplyRayTracingGpuRecoveryBudgetSettings"), std::string::npos);
    EXPECT_NE(source.find("stats.shadowGpuTimingQueriesRecorded"), std::string::npos);
    EXPECT_NE(source.find("stats.reflectionGpuTimingQueriesRecorded"), std::string::npos);
    EXPECT_NE(source.find("stats.gpuTimeBudgetApplied && pathBudgetApplied"), std::string::npos);
    EXPECT_NE(source.find("stats.reflectionGpuTimeBudgetQualityScale < 0.999f"), std::string::npos);
    EXPECT_NE(source.find("stats.reflectionGpuTimeBudgetQualityScale > baselineReflectionQualityScale"),
              std::string::npos);
    EXPECT_NE(source.find("stats.reflectionResolutionScale > baselineReflectionScale"), std::string::npos);
    EXPECT_NE(source.find("stats.estimatedTotalRayCount > stats.rayBudget"), std::string::npos);
    EXPECT_NE(source.find("stats.shadowRequested && stats.shadowSupported && stats.shadowRecorded"),
              std::string::npos);
    EXPECT_NE(source.find("stats.shadowHistoryAvailable &&"), std::string::npos);
    EXPECT_NE(source.find("stats.shadowTemporalAccumulated"), std::string::npos);
    EXPECT_NE(source.find("stats.shadowHistoryReset &&"), std::string::npos);
    EXPECT_NE(source.find("stats.shadowHistoryRecreated &&"), std::string::npos);
    EXPECT_NE(source.find("stats.shadowHistoryResolutionChanged &&"), std::string::npos);
    EXPECT_NE(source.find("stats.shadowWidth == expectedWidth && stats.shadowHeight == expectedHeight"),
              std::string::npos);
    EXPECT_NE(source.find("!stats.shadowTemporalAccumulated"), std::string::npos);
    EXPECT_NE(source.find("stats.reflectionRequested && stats.reflectionSupported && stats.reflectionRecorded"),
              std::string::npos);
    EXPECT_NE(source.find("stats.reflectionHistoryAvailable &&"), std::string::npos);
    EXPECT_NE(source.find("stats.reflectionTemporalAccumulated"), std::string::npos);
    EXPECT_NE(source.find("stats.reflectionHistoryReset &&"), std::string::npos);
    EXPECT_NE(source.find("stats.reflectionHistoryRecreated &&"), std::string::npos);
    EXPECT_NE(source.find("stats.reflectionHistoryResolutionChanged &&"), std::string::npos);
    EXPECT_NE(source.find("stats.reflectionWidth == expectedWidth && stats.reflectionHeight == expectedHeight"),
              std::string::npos);
    EXPECT_NE(source.find("!stats.reflectionTemporalAccumulated"), std::string::npos);
    EXPECT_NE(source.find("stats.reflectionCompositeRequested &&"), std::string::npos);
    EXPECT_NE(source.find("stats.reflectionCompositeRecorded &&"), std::string::npos);
    EXPECT_NE(source.find("!stats.denoiseFallbackToRaw"), std::string::npos);
    EXPECT_NE(source.find("ModelViewer smoke ray-traced shadow ready"), std::string::npos);
    EXPECT_NE(source.find("ModelViewer smoke ray-traced shadow history ready"), std::string::npos);
    EXPECT_NE(source.find("ModelViewer smoke ray-traced shadow history reset ready"), std::string::npos);
    EXPECT_NE(source.find("ModelViewer smoke ray-traced shadow history resize ready"), std::string::npos);
    EXPECT_NE(source.find("ModelViewer smoke resized RT history viewport"), std::string::npos);
    EXPECT_NE(source.find("ModelViewer smoke ray-traced reflection ready"), std::string::npos);
    EXPECT_NE(source.find("ModelViewer smoke ray-traced reflection history ready"), std::string::npos);
    EXPECT_NE(source.find("ModelViewer smoke ray-traced reflection history reset ready"), std::string::npos);
    EXPECT_NE(source.find("ModelViewer smoke ray-traced reflection history resize ready"), std::string::npos);

    const fs::path testsCMakePath = FindTestsCMakePath();
    ASSERT_FALSE(testsCMakePath.empty());
    const std::string testsCMake = ReadTextFile(testsCMakePath);
    EXPECT_NE(testsCMake.find("ModelViewerRayTracedShadowSmoke"), std::string::npos);
    EXPECT_NE(testsCMake.find("ModelViewerRayTracedShadowVisualGoldenValidation"), std::string::npos);
    EXPECT_NE(testsCMake.find("ModelViewerRayTracedReflectionSmoke"), std::string::npos);
    EXPECT_NE(testsCMake.find("ModelViewerRayTracedReflectionVisualGoldenValidation"), std::string::npos);
    EXPECT_NE(testsCMake.find("ModelViewerRayTracingBudgetSmoke"), std::string::npos);
    EXPECT_NE(testsCMake.find("ModelViewerRayTracingGpuBudgetSmoke"), std::string::npos);
    EXPECT_NE(testsCMake.find("ModelViewerRayTracingGpuBudgetRecoverySmoke"), std::string::npos);
    EXPECT_NE(testsCMake.find("--expect-ray-traced-shadow-ready"), std::string::npos);
    EXPECT_NE(testsCMake.find("--expect-ray-traced-shadow-history-ready"), std::string::npos);
    EXPECT_NE(testsCMake.find("--expect-ray-traced-shadow-history-reset-ready"), std::string::npos);
    EXPECT_NE(testsCMake.find("--expect-ray-traced-shadow-history-resize-ready"), std::string::npos);
    EXPECT_NE(testsCMake.find("--rt-reset-history-frame 4"), std::string::npos);
    EXPECT_NE(testsCMake.find("--rt-resize-frame 5"), std::string::npos);
    EXPECT_NE(testsCMake.find("--rt-resize-width ${RVX_MODELVIEWER_RT_RESIZE_WIDTH}"), std::string::npos);
    EXPECT_NE(testsCMake.find("--rt-resize-height ${RVX_MODELVIEWER_RT_RESIZE_HEIGHT}"), std::string::npos);
    EXPECT_NE(testsCMake.find("RTShadow_DX12_192x108.ppm"), std::string::npos);
    EXPECT_NE(testsCMake.find("--screenshot \"${RVX_MODELVIEWER_RT_SHADOW_ACTUAL}\""), std::string::npos);
    EXPECT_NE(testsCMake.find("--expected \"${RVX_MODELVIEWER_RT_SHADOW_GOLDEN}\""), std::string::npos);
    EXPECT_NE(testsCMake.find("set_property(TEST ModelViewerRayTracedShadowVisualGoldenValidation PROPERTY DEPENDS ModelViewerRayTracedShadowSmoke)"), std::string::npos);
    EXPECT_NE(testsCMake.find("--ray-traced-reflections"), std::string::npos);
    EXPECT_NE(testsCMake.find("--expect-ray-traced-reflection-ready"), std::string::npos);
    EXPECT_NE(testsCMake.find("--expect-ray-traced-reflection-history-ready"), std::string::npos);
    EXPECT_NE(testsCMake.find("--expect-ray-traced-reflection-history-reset-ready"), std::string::npos);
    EXPECT_NE(testsCMake.find("--expect-ray-traced-reflection-history-resize-ready"), std::string::npos);
    EXPECT_NE(testsCMake.find("RTReflection_DX12_192x108.ppm"), std::string::npos);
    EXPECT_NE(testsCMake.find("--screenshot \"${RVX_MODELVIEWER_RT_REFLECTION_ACTUAL}\""), std::string::npos);
    EXPECT_NE(testsCMake.find("--expected \"${RVX_MODELVIEWER_RT_REFLECTION_GOLDEN}\""), std::string::npos);
    EXPECT_NE(testsCMake.find("set_property(TEST ModelViewerRayTracedReflectionVisualGoldenValidation PROPERTY DEPENDS ModelViewerRayTracedReflectionSmoke)"), std::string::npos);
    EXPECT_NE(testsCMake.find("--rt-max-rays 8000"), std::string::npos);
    EXPECT_NE(testsCMake.find("--rt-max-resource-bytes 1"), std::string::npos);
    EXPECT_NE(testsCMake.find("--expect-ray-tracing-budget-applied"), std::string::npos);
    EXPECT_NE(testsCMake.find("--expect-ray-tracing-ray-budget-respected"), std::string::npos);
    EXPECT_NE(testsCMake.find("--expect-ray-tracing-resource-budget-exceeded"), std::string::npos);
    EXPECT_NE(testsCMake.find("--expect-ray-tracing-gpu-timing-ready"), std::string::npos);
    EXPECT_NE(testsCMake.find("--rt-max-reflection-gpu-ms 0.000001"), std::string::npos);
    EXPECT_NE(testsCMake.find("--rt-gpu-adjust-frames 1"), std::string::npos);
    EXPECT_NE(testsCMake.find("--frames 6"), std::string::npos);
    EXPECT_NE(testsCMake.find("--expect-ray-tracing-gpu-budget-applied"), std::string::npos);
    EXPECT_NE(testsCMake.find("--rt-recovery-reflection-gpu-ms 1000.0"), std::string::npos);
    EXPECT_NE(testsCMake.find("--expect-ray-tracing-gpu-budget-recovered"), std::string::npos);
    EXPECT_NE(testsCMake.find("--frames 6"), std::string::npos);
    EXPECT_NE(testsCMake.find("--frames 8"), std::string::npos);
    EXPECT_NE(testsCMake.find("LABELS \"visual;gpu;raytracing\""), std::string::npos);

    const fs::path renderRoot = FindShaderDirectory().parent_path();
    const std::string renderSubsystem = ReadTextFile(renderRoot / "Private" / "RenderSubsystem.cpp");
    EXPECT_NE(renderSubsystem.find("m_renderContext->WaitIdle();\n            if (m_sceneRenderer)\n            {\n                m_sceneRenderer->PrepareForSwapChainResize();"),
              std::string::npos);
    EXPECT_NE(renderSubsystem.find("m_renderContext->WaitIdle();\n        if (m_sceneRenderer)\n        {\n            m_sceneRenderer->PrepareForSwapChainResize();"),
              std::string::npos);
}

TEST_F(PipelineCacheValidationFixture, PipelineStateHashesAreStableAndVariantAware)
{
    if (!HasCompilerAvailable())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    FakeDevice firstDevice;
    RVX::PipelineCache firstCache;
    ASSERT_TRUE(firstCache.Initialize(&firstDevice, FindShaderDirectory().string())) << firstCache.GetLastError();

    FakeDevice secondDevice;
    RVX::PipelineCache secondCache;
    ASSERT_TRUE(secondCache.Initialize(&secondDevice, FindShaderDirectory().string())) << secondCache.GetLastError();

    EXPECT_EQ(firstCache.GetPipelineStateHashForVariant(RVX::MaterialPipelineVariant::Opaque),
              secondCache.GetPipelineStateHashForVariant(RVX::MaterialPipelineVariant::Opaque));
    EXPECT_NE(firstCache.GetPipelineStateHashForVariant(RVX::MaterialPipelineVariant::Opaque), 0u);
    EXPECT_NE(firstCache.GetPipelineStateHashForVariant(RVX::MaterialPipelineVariant::Opaque),
              firstCache.GetPipelineStateHashForVariant(RVX::MaterialPipelineVariant::Transparent));
    EXPECT_NE(firstCache.GetStats().toneMappingPipelineHash, 0u);
    EXPECT_NE(firstCache.GetStats().bloomPipelineHash, 0u);
    EXPECT_NE(firstCache.GetStats().colorGradingPipelineHash, 0u);
    EXPECT_NE(firstCache.GetStats().chromaticAberrationPipelineHash, 0u);
    EXPECT_NE(firstCache.GetStats().vignettePipelineHash, 0u);
    EXPECT_NE(firstCache.GetStats().fxaaPipelineHash, 0u);
    EXPECT_NE(firstCache.GetStats().skyboxPipelineHash, 0u);
    EXPECT_NE(firstCache.GetStats().uiPipelineHash, 0u);
    EXPECT_NE(firstCache.GetStats().toneMappingPipelineHash, firstCache.GetStats().bloomPipelineHash);
    EXPECT_NE(firstCache.GetStats().colorGradingPipelineHash, firstCache.GetStats().bloomPipelineHash);
    EXPECT_NE(firstCache.GetStats().chromaticAberrationPipelineHash, firstCache.GetStats().bloomPipelineHash);
    EXPECT_NE(firstCache.GetStats().vignettePipelineHash, firstCache.GetStats().bloomPipelineHash);
    EXPECT_NE(firstCache.GetStats().fxaaPipelineHash, firstCache.GetStats().bloomPipelineHash);
    EXPECT_NE(firstCache.GetStats().skyboxPipelineHash, firstCache.GetStats().toneMappingPipelineHash);
    EXPECT_NE(firstCache.GetStats().uiPipelineHash, firstCache.GetStats().bloomPipelineHash);
    EXPECT_EQ(firstCache.GetStats().pipelineCreateCount, 14u);
    EXPECT_EQ(firstCache.GetStats().pipelineCacheMissCount, 14u);
}

TEST_F(PipelineCacheValidationFixture, MaskedObjectVelocityAlphaTestContracts)
{
    if (!HasCompilerAvailable())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    const fs::path renderRoot = FindShaderDirectory().parent_path();
    const std::string pipelineHeader = ReadTextFile(renderRoot / "Include" / "Render" / "PipelineCache.h");
    const std::string pipelineSource = ReadTextFile(renderRoot / "Private" / "PipelineCache.cpp");
    const std::string passHeader =
        ReadTextFile(renderRoot / "Include" / "Render" / "Passes" / "ObjectVelocityPass.h");
    const std::string passSource =
        ReadTextFile(renderRoot / "Private" / "Passes" / "ObjectVelocityPass.cpp");
    const std::string rendererSource = ReadTextFile(renderRoot / "Private" / "Renderer" / "SceneRenderer.cpp");
    const std::string shaderSource = ReadTextFile(FindShaderDirectory() / "ObjectVelocity.hlsl");

    EXPECT_NE(pipelineHeader.find("GetMaskedObjectVelocityPipeline()"), std::string::npos);
    EXPECT_NE(pipelineHeader.find("GetMaskedObjectVelocityPipeline(RHIFormat outputFormat)"), std::string::npos);
    EXPECT_NE(pipelineHeader.find("GetOrCreateMaskedObjectVelocityPipeline"), std::string::npos);
    EXPECT_NE(pipelineHeader.find("BuildMaskedObjectVelocityPipelineDesc"), std::string::npos);
    EXPECT_NE(pipelineHeader.find("m_maskedObjectVelocityVertexShader"), std::string::npos);
    EXPECT_NE(pipelineHeader.find("m_maskedObjectVelocityPixelShader"), std::string::npos);
    EXPECT_NE(pipelineHeader.find("m_maskedObjectVelocityPipeline"), std::string::npos);

    EXPECT_NE(pipelineSource.find("maskedObjectVelocityVsDesc.entryPoint = \"VSMainMasked\""), std::string::npos);
    EXPECT_NE(pipelineSource.find("maskedObjectVelocityPsDesc.entryPoint = \"PSMainMasked\""), std::string::npos);
    EXPECT_NE(pipelineSource.find("GetMaskedObjectVelocityPipeline(RHIFormat outputFormat)"), std::string::npos);
    EXPECT_NE(pipelineSource.find("GetOrCreateMaskedObjectVelocityPipeline"), std::string::npos);
    EXPECT_NE(pipelineSource.find("BuildMaskedObjectVelocityPipelineDesc"), std::string::npos);
    EXPECT_NE(pipelineSource.find("pipelineDesc.debugName = \"MaskedObjectVelocityPipeline\""), std::string::npos);
    EXPECT_NE(pipelineSource.find("pipelineDesc.inputLayout.AddElement(\"TEXCOORD\", RHIFormat::RG32_FLOAT, 2)"),
              std::string::npos);

    EXPECT_NE(passHeader.find("MaterialSystem* materialSystem"), std::string::npos);
    EXPECT_NE(passHeader.find("maskedDrawItemCount"), std::string::npos);
    EXPECT_NE(passHeader.find("maskedDrawCount"), std::string::npos);
    EXPECT_NE(passHeader.find("skippedMissingUVCount"), std::string::npos);
    EXPECT_NE(passHeader.find("skippedMaterialBindingCount"), std::string::npos);
    EXPECT_NE(passHeader.find("const std::vector<RenderDrawItem>* maskedDrawItems"), std::string::npos);

    EXPECT_NE(passSource.find("GetMaskedObjectVelocityPipeline(RHIFormat::RG16_FLOAT)"), std::string::npos);
    EXPECT_NE(passSource.find("m_stats.maskedDrawItemCount"), std::string::npos);
    EXPECT_NE(passSource.find("MaterialBindingOptions materialOptions;"), std::string::npos);
    EXPECT_NE(passSource.find("materialOptions.allowNormalMap = false;"), std::string::npos);
    EXPECT_NE(passSource.find("m_materialSystem->PrepareMaterialBinding(materialResource, view.viewCache, materialOptions)"),
              std::string::npos);
    EXPECT_NE(passSource.find("++m_stats.skippedMissingUVCount"), std::string::npos);
    EXPECT_NE(passSource.find("++m_stats.skippedMaterialBindingCount"), std::string::npos);
    EXPECT_NE(passSource.find("ctx.SetDescriptorSet(2, materialBinding.descriptorSet, materialBinding.dynamicOffsets)"),
              std::string::npos);
    EXPECT_NE(passSource.find("ctx.SetVertexBuffer(2, buffers.uvBuffer)"), std::string::npos);
    EXPECT_NE(passSource.find("++m_stats.maskedDrawCount"), std::string::npos);

    EXPECT_NE(rendererSource.find("objectVelocityPass->SetResources("), std::string::npos);
    EXPECT_NE(rendererSource.find("m_materialSystem.get());"), std::string::npos);
    EXPECT_NE(rendererSource.find("m_objectVelocityPass->SetRenderScene(&m_renderScene, &m_opaqueDrawItems, &m_maskedDrawItems);"),
              std::string::npos);

    EXPECT_NE(shaderSource.find("struct VSMaskedInput"), std::string::npos);
    EXPECT_NE(shaderSource.find("VSMaskedOutput VSMainMasked(VSMaskedInput input)"), std::string::npos);
    EXPECT_NE(shaderSource.find("float2 PSMainMasked(VSMaskedOutput input) : SV_TARGET"), std::string::npos);
    EXPECT_NE(shaderSource.find("Texture2D BaseColorTexture : register(t1, space2);"), std::string::npos);
    EXPECT_NE(shaderSource.find("SamplerState MaterialSampler : register(s6, space2);"), std::string::npos);
    EXPECT_NE(shaderSource.find("BaseColorTexture.Sample(MaterialSampler, input.TexCoord)"), std::string::npos);
    EXPECT_NE(shaderSource.find("AlphaMode == MATERIAL_ALPHA_MASK && baseColor.a < AlphaCutoff"), std::string::npos);
    EXPECT_NE(shaderSource.find("discard;"), std::string::npos);
}
TEST_F(PipelineCacheValidationFixture, CameraVelocityPipelineIsLazyAndUsesRG16F)
{
    if (!HasCompilerAvailable())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    FakeDevice device;
    RVX::PipelineCache cache;
    ASSERT_TRUE(cache.Initialize(&device, FindShaderDirectory().string()));

    const size_t initialPipelineCount = device.capturedGraphicsPipelines.size();
    EXPECT_EQ(cache.GetCameraVelocityPipeline(), nullptr);

    RVX::RHIPipeline* velocityPipeline = cache.GetCameraVelocityPipeline(RVX::RHIFormat::RG16_FLOAT);
    ASSERT_NE(velocityPipeline, nullptr);
    ASSERT_EQ(device.capturedGraphicsPipelines.size(), initialPipelineCount + 1u);

    const RVX::RHIGraphicsPipelineDesc& velocityDesc = device.capturedGraphicsPipelines.back();
    EXPECT_STREQ(velocityDesc.debugName, "CameraVelocityPipeline");
    EXPECT_EQ(velocityDesc.numRenderTargets, 1u);
    EXPECT_EQ(velocityDesc.renderTargetFormats[0], RVX::RHIFormat::RG16_FLOAT);
    EXPECT_EQ(velocityDesc.depthStencilFormat, RVX::RHIFormat::Unknown);
    EXPECT_FALSE(velocityDesc.depthStencilState.depthTestEnable);
    EXPECT_FALSE(velocityDesc.depthStencilState.depthWriteEnable);
    EXPECT_NE(velocityDesc.vertexShader, nullptr);
    EXPECT_NE(velocityDesc.pixelShader, nullptr);
    EXPECT_TRUE(velocityDesc.inputLayout.elements.empty());

    EXPECT_EQ(cache.GetCameraVelocityPipeline(RVX::RHIFormat::RG16_FLOAT), velocityPipeline);
    EXPECT_EQ(device.capturedGraphicsPipelines.size(), initialPipelineCount + 1u);
}

TEST_F(PipelineCacheValidationFixture, ObjectVelocityPipelineIsLazyAndUsesRG16FDepthRead)
{
    if (!HasCompilerAvailable())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    FakeDevice device;
    RVX::PipelineCache cache;
    ASSERT_TRUE(cache.Initialize(&device, FindShaderDirectory().string()));

    const size_t initialPipelineCount = device.capturedGraphicsPipelines.size();
    EXPECT_EQ(cache.GetObjectVelocityPipeline(), nullptr);
    EXPECT_EQ(cache.GetMaskedObjectVelocityPipeline(), nullptr);

    RVX::RHIPipeline* velocityPipeline = cache.GetObjectVelocityPipeline(RVX::RHIFormat::RG16_FLOAT);
    ASSERT_NE(velocityPipeline, nullptr);
    ASSERT_EQ(device.capturedGraphicsPipelines.size(), initialPipelineCount + 1u);

    const RVX::RHIGraphicsPipelineDesc& velocityDesc = device.capturedGraphicsPipelines.back();
    EXPECT_STREQ(velocityDesc.debugName, "ObjectVelocityPipeline");
    EXPECT_EQ(velocityDesc.numRenderTargets, 1u);
    EXPECT_EQ(velocityDesc.renderTargetFormats[0], RVX::RHIFormat::RG16_FLOAT);
    EXPECT_EQ(velocityDesc.depthStencilFormat, RVX::PipelineCache::GetDefaultDepthStencilFormat());
    EXPECT_TRUE(velocityDesc.depthStencilState.depthTestEnable);
    EXPECT_FALSE(velocityDesc.depthStencilState.depthWriteEnable);
    EXPECT_EQ(velocityDesc.depthStencilState.depthCompareOp, RVX::RHICompareOp::LessEqual);
    EXPECT_NE(velocityDesc.vertexShader, nullptr);
    EXPECT_NE(velocityDesc.pixelShader, nullptr);
    ASSERT_EQ(velocityDesc.inputLayout.elements.size(), 3u);
    EXPECT_STREQ(velocityDesc.inputLayout.elements[0].semanticName, "POSITION");
    EXPECT_EQ(velocityDesc.inputLayout.elements[0].format, RVX::RHIFormat::RGB32_FLOAT);
    EXPECT_EQ(velocityDesc.inputLayout.elements[0].inputSlot, 0u);
    EXPECT_STREQ(velocityDesc.inputLayout.elements[1].semanticName, "BLENDINDICES");
    EXPECT_EQ(velocityDesc.inputLayout.elements[1].format, RVX::RHIFormat::RGBA32_UINT);
    EXPECT_EQ(velocityDesc.inputLayout.elements[1].inputSlot, 4u);
    EXPECT_STREQ(velocityDesc.inputLayout.elements[2].semanticName, "BLENDWEIGHT");
    EXPECT_EQ(velocityDesc.inputLayout.elements[2].format, RVX::RHIFormat::RGBA32_FLOAT);
    EXPECT_EQ(velocityDesc.inputLayout.elements[2].inputSlot, 5u);

    RVX::RHIPipeline* maskedVelocityPipeline =
        cache.GetMaskedObjectVelocityPipeline(RVX::RHIFormat::RG16_FLOAT);
    ASSERT_NE(maskedVelocityPipeline, nullptr);
    ASSERT_EQ(device.capturedGraphicsPipelines.size(), initialPipelineCount + 2u);

    const RVX::RHIGraphicsPipelineDesc& maskedVelocityDesc = device.capturedGraphicsPipelines.back();
    EXPECT_STREQ(maskedVelocityDesc.debugName, "MaskedObjectVelocityPipeline");
    EXPECT_EQ(maskedVelocityDesc.numRenderTargets, 1u);
    EXPECT_EQ(maskedVelocityDesc.renderTargetFormats[0], RVX::RHIFormat::RG16_FLOAT);
    EXPECT_EQ(maskedVelocityDesc.depthStencilFormat, RVX::PipelineCache::GetDefaultDepthStencilFormat());
    EXPECT_TRUE(maskedVelocityDesc.depthStencilState.depthTestEnable);
    EXPECT_FALSE(maskedVelocityDesc.depthStencilState.depthWriteEnable);
    EXPECT_EQ(maskedVelocityDesc.depthStencilState.depthCompareOp, RVX::RHICompareOp::LessEqual);
    EXPECT_NE(maskedVelocityDesc.vertexShader, nullptr);
    EXPECT_NE(maskedVelocityDesc.pixelShader, nullptr);
    ASSERT_EQ(maskedVelocityDesc.inputLayout.elements.size(), 4u);
    EXPECT_STREQ(maskedVelocityDesc.inputLayout.elements[0].semanticName, "POSITION");
    EXPECT_EQ(maskedVelocityDesc.inputLayout.elements[0].format, RVX::RHIFormat::RGB32_FLOAT);
    EXPECT_EQ(maskedVelocityDesc.inputLayout.elements[0].inputSlot, 0u);
    EXPECT_STREQ(maskedVelocityDesc.inputLayout.elements[1].semanticName, "BLENDINDICES");
    EXPECT_EQ(maskedVelocityDesc.inputLayout.elements[1].format, RVX::RHIFormat::RGBA32_UINT);
    EXPECT_EQ(maskedVelocityDesc.inputLayout.elements[1].inputSlot, 4u);
    EXPECT_STREQ(maskedVelocityDesc.inputLayout.elements[2].semanticName, "BLENDWEIGHT");
    EXPECT_EQ(maskedVelocityDesc.inputLayout.elements[2].format, RVX::RHIFormat::RGBA32_FLOAT);
    EXPECT_EQ(maskedVelocityDesc.inputLayout.elements[2].inputSlot, 5u);
    EXPECT_STREQ(maskedVelocityDesc.inputLayout.elements[3].semanticName, "TEXCOORD");
    EXPECT_EQ(maskedVelocityDesc.inputLayout.elements[3].format, RVX::RHIFormat::RG32_FLOAT);
    EXPECT_EQ(maskedVelocityDesc.inputLayout.elements[3].inputSlot, 2u);

    EXPECT_EQ(cache.GetObjectVelocityPipeline(RVX::RHIFormat::RG16_FLOAT), velocityPipeline);
    EXPECT_EQ(cache.GetMaskedObjectVelocityPipeline(RVX::RHIFormat::RG16_FLOAT), maskedVelocityPipeline);
    EXPECT_EQ(device.capturedGraphicsPipelines.size(), initialPipelineCount + 2u);
}
TEST_F(PipelineCacheValidationFixture, RenderTargetFormatChangesPipelineHash)
{
    if (!HasCompilerAvailable())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    FakeDevice firstDevice;
    RVX::PipelineCache firstCache;
    ASSERT_TRUE(firstCache.Initialize(&firstDevice, FindShaderDirectory().string())) << firstCache.GetLastError();

    FakeDevice secondDevice;
    RVX::PipelineCache secondCache;
    secondCache.SetRenderTargetFormat(RVX::RHIFormat::RGBA16_FLOAT);
    ASSERT_TRUE(secondCache.Initialize(&secondDevice, FindShaderDirectory().string())) << secondCache.GetLastError();

    EXPECT_NE(firstCache.GetPipelineStateHashForVariant(RVX::MaterialPipelineVariant::Opaque),
              secondCache.GetPipelineStateHashForVariant(RVX::MaterialPipelineVariant::Opaque));
    ASSERT_GE(secondDevice.capturedGraphicsPipelines.size(), 14u);
    EXPECT_EQ(secondDevice.capturedGraphicsPipelines.front().renderTargetFormats[0], RVX::RHIFormat::RGBA16_FLOAT);
    EXPECT_EQ(secondDevice.capturedGraphicsPipelines[4].renderTargetFormats[0], RVX::RHIFormat::RGBA16_FLOAT);
    EXPECT_EQ(secondDevice.capturedGraphicsPipelines[5].renderTargetFormats[0], RVX::RHIFormat::RGBA16_FLOAT);
    EXPECT_EQ(secondDevice.capturedGraphicsPipelines[6].renderTargetFormats[0], RVX::RHIFormat::RGBA16_FLOAT);
    EXPECT_EQ(secondDevice.capturedGraphicsPipelines[7].renderTargetFormats[0], RVX::RHIFormat::RGBA16_FLOAT);
    EXPECT_EQ(secondDevice.capturedGraphicsPipelines[8].renderTargetFormats[0], RVX::RHIFormat::RGBA16_FLOAT);
    EXPECT_EQ(secondDevice.capturedGraphicsPipelines[9].renderTargetFormats[0], RVX::RHIFormat::RGBA16_FLOAT);
    EXPECT_EQ(secondDevice.capturedGraphicsPipelines[10].renderTargetFormats[0], RVX::RHIFormat::RGBA16_FLOAT);
    EXPECT_EQ(secondDevice.capturedGraphicsPipelines[11].renderTargetFormats[0], RVX::RHIFormat::RGBA16_FLOAT);
    EXPECT_EQ(secondDevice.capturedGraphicsPipelines[12].renderTargetFormats[0], RVX::RHIFormat::RGBA16_FLOAT);
    EXPECT_EQ(secondDevice.capturedGraphicsPipelines[13].renderTargetFormats[0], RVX::RHIFormat::RGBA16_FLOAT);
}

TEST_F(PipelineCacheValidationFixture, SplitRenderTargetFormatsRouteSceneBloomAndToneMapping)
{
    if (!HasCompilerAvailable())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    FakeDevice device;
    RVX::PipelineCache cache;
    cache.SetRenderTargetFormats(RVX::RHIFormat::RGBA16_FLOAT,
                                 RVX::RHIFormat::RGBA16_FLOAT,
                                 RVX::RHIFormat::BGRA8_UNORM);
    ASSERT_TRUE(cache.Initialize(&device, FindShaderDirectory().string())) << cache.GetLastError();

    EXPECT_EQ(cache.GetConfig().renderTargetFormat, RVX::RHIFormat::RGBA16_FLOAT);
    EXPECT_EQ(cache.GetConfig().postProcessIntermediateFormat, RVX::RHIFormat::RGBA16_FLOAT);
    EXPECT_EQ(cache.GetConfig().toneMappingOutputFormat, RVX::RHIFormat::BGRA8_UNORM);
    EXPECT_EQ(cache.GetSceneRenderTargetFormat(), RVX::RHIFormat::RGBA16_FLOAT);
    EXPECT_EQ(cache.GetPostProcessIntermediateFormat(), RVX::RHIFormat::RGBA16_FLOAT);
    EXPECT_EQ(cache.GetToneMappingOutputFormat(), RVX::RHIFormat::BGRA8_UNORM);

    ASSERT_GE(device.capturedGraphicsPipelines.size(), 14u);
    EXPECT_EQ(device.capturedGraphicsPipelines[0].renderTargetFormats[0], RVX::RHIFormat::RGBA16_FLOAT);
    EXPECT_EQ(device.capturedGraphicsPipelines[1].renderTargetFormats[0], RVX::RHIFormat::RGBA16_FLOAT);
    EXPECT_EQ(device.capturedGraphicsPipelines[2].renderTargetFormats[0], RVX::RHIFormat::RGBA16_FLOAT);
    EXPECT_EQ(device.capturedGraphicsPipelines[4].renderTargetFormats[0], RVX::RHIFormat::RGBA16_FLOAT);
    EXPECT_EQ(device.capturedGraphicsPipelines[5].renderTargetFormats[0], RVX::RHIFormat::BGRA8_UNORM);
    EXPECT_EQ(device.capturedGraphicsPipelines[6].renderTargetFormats[0], RVX::RHIFormat::RGBA16_FLOAT);
    EXPECT_EQ(device.capturedGraphicsPipelines[7].renderTargetFormats[0], RVX::RHIFormat::RGBA16_FLOAT);
    EXPECT_EQ(device.capturedGraphicsPipelines[8].renderTargetFormats[0], RVX::RHIFormat::RGBA16_FLOAT);
    EXPECT_EQ(device.capturedGraphicsPipelines[9].renderTargetFormats[0], RVX::RHIFormat::BGRA8_UNORM);
    EXPECT_EQ(device.capturedGraphicsPipelines[10].renderTargetFormats[0], RVX::RHIFormat::BGRA8_UNORM);
    EXPECT_EQ(device.capturedGraphicsPipelines[11].renderTargetFormats[0], RVX::RHIFormat::BGRA8_UNORM);
    EXPECT_EQ(device.capturedGraphicsPipelines[12].renderTargetFormats[0], RVX::RHIFormat::BGRA8_UNORM);
    EXPECT_EQ(device.capturedGraphicsPipelines[13].renderTargetFormats[0], RVX::RHIFormat::BGRA8_UNORM);

    EXPECT_NE(cache.GetPipelineStateHashForVariant(RVX::MaterialPipelineVariant::Opaque), 0u);
    EXPECT_NE(cache.GetStats().skyboxPipelineHash, 0u);
    EXPECT_NE(cache.GetStats().toneMappingPipelineHash, 0u);
    EXPECT_NE(cache.GetStats().bloomPipelineHash, 0u);
    EXPECT_NE(cache.GetStats().colorGradingPipelineHash, 0u);
    EXPECT_NE(cache.GetStats().chromaticAberrationPipelineHash, 0u);
    EXPECT_NE(cache.GetStats().vignettePipelineHash, 0u);
    EXPECT_NE(cache.GetStats().fxaaPipelineHash, 0u);
    EXPECT_NE(cache.GetStats().uiPipelineHash, 0u);
    EXPECT_NE(cache.GetStats().toneMappingPipelineHash, cache.GetStats().bloomPipelineHash);
    EXPECT_NE(cache.GetStats().colorGradingPipelineHash, cache.GetStats().bloomPipelineHash);
    EXPECT_NE(cache.GetStats().chromaticAberrationPipelineHash, cache.GetStats().bloomPipelineHash);
    EXPECT_NE(cache.GetStats().vignettePipelineHash, cache.GetStats().bloomPipelineHash);
    EXPECT_NE(cache.GetStats().fxaaPipelineHash, cache.GetStats().bloomPipelineHash);
    EXPECT_NE(cache.GetStats().uiPipelineHash, cache.GetStats().bloomPipelineHash);
}

TEST_F(PipelineCacheValidationFixture, BloomAdditivePipelineUsesAdditiveColorAndPreservesAlpha)
{
    if (!HasCompilerAvailable())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    FakeDevice device;
    RVX::PipelineCache cache;
    cache.SetRenderTargetFormats(RVX::RHIFormat::RGBA16_FLOAT,
                                 RVX::RHIFormat::RGBA16_FLOAT,
                                 RVX::RHIFormat::BGRA8_UNORM);
    ASSERT_TRUE(cache.Initialize(&device, FindShaderDirectory().string())) << cache.GetLastError();

    RVX::RHIPipeline* opaqueBloom = cache.GetBloomPipeline(RVX::RHIFormat::RGBA16_FLOAT);
    RVX::RHIPipeline* additiveBloom = cache.GetBloomAdditivePipeline(RVX::RHIFormat::RGBA16_FLOAT);
    ASSERT_NE(opaqueBloom, nullptr);
    ASSERT_NE(additiveBloom, nullptr);
    EXPECT_NE(opaqueBloom, additiveBloom);

    auto it = std::find_if(device.capturedGraphicsPipelines.begin(),
                           device.capturedGraphicsPipelines.end(),
                           [](const RVX::RHIGraphicsPipelineDesc& desc)
                           {
                               return desc.debugName && std::string(desc.debugName) == "BloomAdditivePipeline";
                           });
    ASSERT_NE(it, device.capturedGraphicsPipelines.end());

    const RVX::RHIRenderTargetBlendState& blend = it->blendState.renderTargets[0];
    EXPECT_TRUE(blend.blendEnable);
    EXPECT_EQ(blend.srcColorBlend, RVX::RHIBlendFactor::One);
    EXPECT_EQ(blend.dstColorBlend, RVX::RHIBlendFactor::One);
    EXPECT_EQ(blend.srcAlphaBlend, RVX::RHIBlendFactor::Zero);
    EXPECT_EQ(blend.dstAlphaBlend, RVX::RHIBlendFactor::One);
    EXPECT_EQ(it->renderTargetFormats[0], RVX::RHIFormat::RGBA16_FLOAT);
}

TEST_F(PipelineCacheValidationFixture, RuntimeOutputFormatRequestsCreateMatchingPipelines)
{
    if (!HasCompilerAvailable())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    FakeDevice device;
    RVX::PipelineCache cache;
    cache.SetRenderTargetFormats(RVX::RHIFormat::RGBA16_FLOAT,
                                 RVX::RHIFormat::RGBA16_FLOAT,
                                 RVX::RHIFormat::BGRA8_UNORM);
    ASSERT_TRUE(cache.Initialize(&device, FindShaderDirectory().string())) << cache.GetLastError();

    const size_t initialPipelineCount = device.capturedGraphicsPipelines.size();
    ASSERT_NE(cache.GetPipelineForVariant(RVX::MaterialPipelineVariant::Opaque, RVX::RHIFormat::RGBA8_UNORM), nullptr);
    ASSERT_NE(cache.GetSkyboxPipeline(RVX::RHIFormat::RGBA8_UNORM), nullptr);
    ASSERT_NE(cache.GetBloomPipeline(RVX::RHIFormat::RGBA8_UNORM), nullptr);
    ASSERT_NE(cache.GetVignettePipeline(RVX::RHIFormat::RGBA8_UNORM), nullptr);
    ASSERT_NE(cache.GetFXAAPipeline(RVX::RHIFormat::RGBA8_UNORM), nullptr);
    ASSERT_NE(cache.GetColorGradingPipeline(RVX::RHIFormat::RGBA8_UNORM), nullptr);
    ASSERT_NE(cache.GetChromaticAberrationPipeline(RVX::RHIFormat::RGBA8_UNORM), nullptr);
    ASSERT_NE(cache.GetUIPipeline(RVX::RHIFormat::RGBA8_UNORM), nullptr);

    ASSERT_GE(device.capturedGraphicsPipelines.size(), initialPipelineCount + 8u);
    EXPECT_EQ(device.capturedGraphicsPipelines[initialPipelineCount].renderTargetFormats[0],
              RVX::RHIFormat::RGBA8_UNORM);
    EXPECT_EQ(device.capturedGraphicsPipelines[initialPipelineCount + 1].renderTargetFormats[0],
              RVX::RHIFormat::RGBA8_UNORM);
    EXPECT_EQ(device.capturedGraphicsPipelines[initialPipelineCount + 2].renderTargetFormats[0],
              RVX::RHIFormat::RGBA8_UNORM);
    EXPECT_EQ(device.capturedGraphicsPipelines[initialPipelineCount + 3].renderTargetFormats[0],
              RVX::RHIFormat::RGBA8_UNORM);
    EXPECT_EQ(device.capturedGraphicsPipelines[initialPipelineCount + 4].renderTargetFormats[0],
              RVX::RHIFormat::RGBA8_UNORM);
    EXPECT_EQ(device.capturedGraphicsPipelines[initialPipelineCount + 5].renderTargetFormats[0],
              RVX::RHIFormat::RGBA8_UNORM);
    EXPECT_EQ(device.capturedGraphicsPipelines[initialPipelineCount + 6].renderTargetFormats[0],
              RVX::RHIFormat::RGBA8_UNORM);
    EXPECT_EQ(device.capturedGraphicsPipelines[initialPipelineCount + 7].renderTargetFormats[0],
              RVX::RHIFormat::RGBA8_UNORM);
}

TEST_F(PipelineCacheValidationFixture, RuntimeRenderTargetFormatSwitchRefreshesCurrentFormatPipelines)
{
    if (!HasCompilerAvailable())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    FakeDevice device;
    RVX::PipelineCache cache;
    cache.SetRenderTargetFormats(RVX::RHIFormat::RGBA16_FLOAT,
                                 RVX::RHIFormat::RGBA16_FLOAT,
                                 RVX::RHIFormat::BGRA8_UNORM);
    ASSERT_TRUE(cache.Initialize(&device, FindShaderDirectory().string())) << cache.GetLastError();

    const size_t initialPipelineCount = device.capturedGraphicsPipelines.size();
    cache.SetRenderTargetFormats(RVX::RHIFormat::RGBA8_UNORM,
                                 RVX::RHIFormat::RGBA8_UNORM,
                                 RVX::RHIFormat::RGBA8_UNORM);

    ASSERT_NE(cache.GetPipelineForVariant(RVX::MaterialPipelineVariant::Opaque, RVX::RHIFormat::RGBA8_UNORM), nullptr);
    ASSERT_NE(cache.GetSkyboxPipeline(RVX::RHIFormat::RGBA8_UNORM), nullptr);
    ASSERT_NE(cache.GetBloomPipeline(RVX::RHIFormat::RGBA8_UNORM), nullptr);
    ASSERT_NE(cache.GetToneMappingPipeline(RVX::RHIFormat::RGBA8_UNORM), nullptr);
    ASSERT_NE(cache.GetVignettePipeline(RVX::RHIFormat::RGBA8_UNORM), nullptr);
    ASSERT_NE(cache.GetFXAAPipeline(RVX::RHIFormat::RGBA8_UNORM), nullptr);
    ASSERT_NE(cache.GetColorGradingPipeline(RVX::RHIFormat::RGBA8_UNORM), nullptr);
    ASSERT_NE(cache.GetChromaticAberrationPipeline(RVX::RHIFormat::RGBA8_UNORM), nullptr);
    ASSERT_NE(cache.GetUIPipeline(RVX::RHIFormat::RGBA8_UNORM), nullptr);

    ASSERT_GE(device.capturedGraphicsPipelines.size(), initialPipelineCount + 9u);
    for (size_t index = initialPipelineCount; index < initialPipelineCount + 9u; ++index)
    {
        EXPECT_EQ(device.capturedGraphicsPipelines[index].renderTargetFormats[0],
                  RVX::RHIFormat::RGBA8_UNORM);
    }
}

TEST_F(PipelineCacheValidationFixture, DefaultDepthFormatIsD32AndForwardZ)
{
    if (!HasCompilerAvailable())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    FakeDevice device;
    RVX::PipelineCache cache;
    ASSERT_TRUE(cache.Initialize(&device, FindShaderDirectory().string())) << cache.GetLastError();

    EXPECT_EQ(cache.GetConfig().depthStencilFormat, RVX::RHIFormat::D32_FLOAT);
    EXPECT_EQ(RVX::PipelineCache::GetDefaultDepthStencilFormat(), RVX::RHIFormat::D32_FLOAT);
    EXPECT_EQ(cache.GetDepthClearValue(), 1.0f);

    ASSERT_GE(device.capturedGraphicsPipelines.size(), 14u);
    const auto& opaqueDesc = device.capturedGraphicsPipelines[0];
    const auto& transparentDesc = device.capturedGraphicsPipelines[2];
    const auto& depthOnlyDesc = device.capturedGraphicsPipelines[3];
    const auto& skyboxDesc = device.capturedGraphicsPipelines[4];
    const auto& toneMappingDesc = device.capturedGraphicsPipelines[5];
    const auto& bloomDesc = device.capturedGraphicsPipelines[6];
    const auto& reflectionCompositeDesc = device.capturedGraphicsPipelines[7];
    const auto& reflectionDenoiseDesc = device.capturedGraphicsPipelines[8];
    const auto& vignetteDesc = device.capturedGraphicsPipelines[9];
    const auto& fxaaDesc = device.capturedGraphicsPipelines[10];
    const auto& colorGradingDesc = device.capturedGraphicsPipelines[11];
    const auto& chromaticAberrationDesc = device.capturedGraphicsPipelines[12];
    const auto& uiDesc = device.capturedGraphicsPipelines[13];

    EXPECT_EQ(opaqueDesc.depthStencilFormat, RVX::RHIFormat::D32_FLOAT);
    EXPECT_EQ(opaqueDesc.depthStencilState.depthCompareOp, RVX::RHICompareOp::Less);
    EXPECT_TRUE(opaqueDesc.depthStencilState.depthWriteEnable);

    EXPECT_EQ(transparentDesc.depthStencilFormat, RVX::RHIFormat::D32_FLOAT);
    EXPECT_EQ(transparentDesc.depthStencilState.depthCompareOp, RVX::RHICompareOp::Less);
    EXPECT_FALSE(transparentDesc.depthStencilState.depthWriteEnable);

    ASSERT_NE(cache.GetDepthOnlyPipeline(), nullptr);
    EXPECT_EQ(depthOnlyDesc.numRenderTargets, 0u);
    EXPECT_EQ(depthOnlyDesc.renderTargetFormats[0], RVX::RHIFormat::Unknown);
    EXPECT_EQ(depthOnlyDesc.depthStencilFormat, RVX::RHIFormat::D32_FLOAT);
    EXPECT_EQ(depthOnlyDesc.depthStencilState.depthCompareOp, RVX::RHICompareOp::Less);
    EXPECT_TRUE(depthOnlyDesc.depthStencilState.depthWriteEnable);
    EXPECT_EQ(depthOnlyDesc.pixelShader, nullptr);
    ASSERT_EQ(depthOnlyDesc.inputLayout.elements.size(), static_cast<size_t>(3));
    EXPECT_STREQ(depthOnlyDesc.inputLayout.elements[0].semanticName, "POSITION");
    EXPECT_EQ(depthOnlyDesc.inputLayout.elements[0].format, RVX::RHIFormat::RGB32_FLOAT);
    EXPECT_EQ(depthOnlyDesc.inputLayout.elements[0].inputSlot, 0u);
    EXPECT_STREQ(depthOnlyDesc.inputLayout.elements[1].semanticName, "BLENDINDICES");
    EXPECT_EQ(depthOnlyDesc.inputLayout.elements[1].format, RVX::RHIFormat::RGBA32_UINT);
    EXPECT_EQ(depthOnlyDesc.inputLayout.elements[1].inputSlot, 4u);
    EXPECT_STREQ(depthOnlyDesc.inputLayout.elements[2].semanticName, "BLENDWEIGHT");
    EXPECT_EQ(depthOnlyDesc.inputLayout.elements[2].format, RVX::RHIFormat::RGBA32_FLOAT);
    EXPECT_EQ(depthOnlyDesc.inputLayout.elements[2].inputSlot, 5u);

    ASSERT_NE(cache.GetSkyboxPipeline(), nullptr);
    EXPECT_EQ(skyboxDesc.numRenderTargets, 1u);
    EXPECT_EQ(skyboxDesc.renderTargetFormats[0], RVX::RHIFormat::RGBA8_UNORM);
    EXPECT_EQ(skyboxDesc.depthStencilFormat, RVX::RHIFormat::D32_FLOAT);
    EXPECT_EQ(skyboxDesc.depthStencilState.depthCompareOp, RVX::RHICompareOp::Less);
    EXPECT_TRUE(skyboxDesc.depthStencilState.depthTestEnable);
    EXPECT_FALSE(skyboxDesc.depthStencilState.depthWriteEnable);
    EXPECT_NE(skyboxDesc.vertexShader, nullptr);
    EXPECT_NE(skyboxDesc.pixelShader, nullptr);
    EXPECT_TRUE(skyboxDesc.inputLayout.elements.empty());

    ASSERT_NE(cache.GetToneMappingPipeline(), nullptr);
    EXPECT_EQ(toneMappingDesc.numRenderTargets, 1u);
    EXPECT_EQ(toneMappingDesc.renderTargetFormats[0], RVX::RHIFormat::RGBA8_UNORM);
    EXPECT_EQ(toneMappingDesc.depthStencilFormat, RVX::RHIFormat::Unknown);
    EXPECT_FALSE(toneMappingDesc.depthStencilState.depthTestEnable);
    EXPECT_FALSE(toneMappingDesc.depthStencilState.depthWriteEnable);
    EXPECT_NE(toneMappingDesc.vertexShader, nullptr);
    EXPECT_NE(toneMappingDesc.pixelShader, nullptr);
    EXPECT_TRUE(toneMappingDesc.inputLayout.elements.empty());

    ASSERT_NE(cache.GetBloomPipeline(), nullptr);
    EXPECT_EQ(bloomDesc.numRenderTargets, 1u);
    EXPECT_EQ(bloomDesc.renderTargetFormats[0], RVX::RHIFormat::RGBA8_UNORM);
    EXPECT_EQ(bloomDesc.depthStencilFormat, RVX::RHIFormat::Unknown);
    EXPECT_FALSE(bloomDesc.depthStencilState.depthTestEnable);
    EXPECT_FALSE(bloomDesc.depthStencilState.depthWriteEnable);
    EXPECT_NE(bloomDesc.vertexShader, nullptr);
    EXPECT_NE(bloomDesc.pixelShader, nullptr);
    EXPECT_TRUE(bloomDesc.inputLayout.elements.empty());

    ASSERT_NE(cache.GetRayTracedReflectionCompositePipeline(), nullptr);
    EXPECT_EQ(reflectionCompositeDesc.numRenderTargets, 1u);
    EXPECT_EQ(reflectionCompositeDesc.renderTargetFormats[0], RVX::RHIFormat::RGBA8_UNORM);
    EXPECT_EQ(reflectionCompositeDesc.depthStencilFormat, RVX::RHIFormat::Unknown);
    EXPECT_FALSE(reflectionCompositeDesc.depthStencilState.depthTestEnable);
    EXPECT_FALSE(reflectionCompositeDesc.depthStencilState.depthWriteEnable);
    EXPECT_NE(reflectionCompositeDesc.vertexShader, nullptr);
    EXPECT_NE(reflectionCompositeDesc.pixelShader, nullptr);
    EXPECT_TRUE(reflectionCompositeDesc.inputLayout.elements.empty());

    ASSERT_NE(cache.GetRayTracedReflectionDenoisePipeline(), nullptr);
    EXPECT_EQ(reflectionDenoiseDesc.numRenderTargets, 1u);
    EXPECT_EQ(reflectionDenoiseDesc.renderTargetFormats[0], RVX::RHIFormat::RGBA8_UNORM);
    EXPECT_EQ(reflectionDenoiseDesc.depthStencilFormat, RVX::RHIFormat::Unknown);
    EXPECT_FALSE(reflectionDenoiseDesc.depthStencilState.depthTestEnable);
    EXPECT_FALSE(reflectionDenoiseDesc.depthStencilState.depthWriteEnable);
    EXPECT_NE(reflectionDenoiseDesc.vertexShader, nullptr);
    EXPECT_NE(reflectionDenoiseDesc.pixelShader, nullptr);
    EXPECT_TRUE(reflectionDenoiseDesc.inputLayout.elements.empty());

    ASSERT_NE(cache.GetVignettePipeline(), nullptr);
    EXPECT_EQ(vignetteDesc.numRenderTargets, 1u);
    EXPECT_EQ(vignetteDesc.renderTargetFormats[0], RVX::RHIFormat::RGBA8_UNORM);
    EXPECT_EQ(vignetteDesc.depthStencilFormat, RVX::RHIFormat::Unknown);
    EXPECT_FALSE(vignetteDesc.depthStencilState.depthTestEnable);
    EXPECT_FALSE(vignetteDesc.depthStencilState.depthWriteEnable);
    EXPECT_NE(vignetteDesc.vertexShader, nullptr);
    EXPECT_NE(vignetteDesc.pixelShader, nullptr);
    EXPECT_TRUE(vignetteDesc.inputLayout.elements.empty());

    ASSERT_NE(cache.GetFXAAPipeline(), nullptr);
    EXPECT_EQ(fxaaDesc.numRenderTargets, 1u);
    EXPECT_EQ(fxaaDesc.renderTargetFormats[0], RVX::RHIFormat::RGBA8_UNORM);
    EXPECT_EQ(fxaaDesc.depthStencilFormat, RVX::RHIFormat::Unknown);
    EXPECT_FALSE(fxaaDesc.depthStencilState.depthTestEnable);
    EXPECT_FALSE(fxaaDesc.depthStencilState.depthWriteEnable);
    EXPECT_NE(fxaaDesc.vertexShader, nullptr);
    EXPECT_NE(fxaaDesc.pixelShader, nullptr);
    EXPECT_TRUE(fxaaDesc.inputLayout.elements.empty());

    ASSERT_NE(cache.GetColorGradingPipeline(), nullptr);
    EXPECT_EQ(colorGradingDesc.numRenderTargets, 1u);
    EXPECT_EQ(colorGradingDesc.renderTargetFormats[0], RVX::RHIFormat::RGBA8_UNORM);
    EXPECT_EQ(colorGradingDesc.depthStencilFormat, RVX::RHIFormat::Unknown);
    EXPECT_FALSE(colorGradingDesc.depthStencilState.depthTestEnable);
    EXPECT_FALSE(colorGradingDesc.depthStencilState.depthWriteEnable);
    EXPECT_NE(colorGradingDesc.vertexShader, nullptr);
    EXPECT_NE(colorGradingDesc.pixelShader, nullptr);
    EXPECT_TRUE(colorGradingDesc.inputLayout.elements.empty());

    ASSERT_NE(cache.GetChromaticAberrationPipeline(), nullptr);
    EXPECT_EQ(chromaticAberrationDesc.numRenderTargets, 1u);
    EXPECT_EQ(chromaticAberrationDesc.renderTargetFormats[0], RVX::RHIFormat::RGBA8_UNORM);
    EXPECT_EQ(chromaticAberrationDesc.depthStencilFormat, RVX::RHIFormat::Unknown);
    EXPECT_FALSE(chromaticAberrationDesc.depthStencilState.depthTestEnable);
    EXPECT_FALSE(chromaticAberrationDesc.depthStencilState.depthWriteEnable);
    EXPECT_NE(chromaticAberrationDesc.vertexShader, nullptr);
    EXPECT_NE(chromaticAberrationDesc.pixelShader, nullptr);
    EXPECT_TRUE(chromaticAberrationDesc.inputLayout.elements.empty());

    ASSERT_NE(cache.GetUIPipeline(), nullptr);
    ASSERT_NE(cache.GetUILayout(), nullptr);
    ASSERT_NE(cache.GetUITextureSetLayout(), nullptr);
    EXPECT_STREQ(uiDesc.debugName, "UIPipeline");
    EXPECT_EQ(uiDesc.numRenderTargets, 1u);
    EXPECT_EQ(uiDesc.renderTargetFormats[0], RVX::RHIFormat::RGBA8_UNORM);
    EXPECT_EQ(uiDesc.depthStencilFormat, RVX::RHIFormat::Unknown);
    EXPECT_FALSE(uiDesc.depthStencilState.depthTestEnable);
    EXPECT_FALSE(uiDesc.depthStencilState.depthWriteEnable);
    EXPECT_NE(uiDesc.vertexShader, nullptr);
    EXPECT_NE(uiDesc.pixelShader, nullptr);
    EXPECT_TRUE(uiDesc.blendState.renderTargets[0].blendEnable);
    EXPECT_EQ(uiDesc.blendState.renderTargets[0].srcColorBlend, RVX::RHIBlendFactor::SrcAlpha);
    EXPECT_EQ(uiDesc.blendState.renderTargets[0].dstColorBlend, RVX::RHIBlendFactor::InvSrcAlpha);
    ASSERT_EQ(uiDesc.inputLayout.elements.size(), 3u);
    EXPECT_STREQ(uiDesc.inputLayout.elements[0].semanticName, "POSITION");
    EXPECT_EQ(uiDesc.inputLayout.elements[0].format, RVX::RHIFormat::RG32_FLOAT);
    EXPECT_EQ(uiDesc.inputLayout.elements[0].inputSlot, 0u);
    EXPECT_STREQ(uiDesc.inputLayout.elements[1].semanticName, "TEXCOORD");
    EXPECT_EQ(uiDesc.inputLayout.elements[1].format, RVX::RHIFormat::RG32_FLOAT);
    EXPECT_EQ(uiDesc.inputLayout.elements[1].inputSlot, 0u);
    EXPECT_STREQ(uiDesc.inputLayout.elements[2].semanticName, "COLOR");
    EXPECT_EQ(uiDesc.inputLayout.elements[2].format, RVX::RHIFormat::RGBA32_FLOAT);
    EXPECT_EQ(uiDesc.inputLayout.elements[2].inputSlot, 0u);

    auto uiSetLayoutIt = std::find_if(device.capturedSetLayouts.begin(),
                                      device.capturedSetLayouts.end(),
                                      [](const RVX::RHIDescriptorSetLayoutDesc& desc)
                                      {
                                          return desc.debugName &&
                                                 std::string(desc.debugName) == "UITextureSetLayout";
                                      });
    ASSERT_NE(uiSetLayoutIt, device.capturedSetLayouts.end());
    ASSERT_EQ(uiSetLayoutIt->entries.size(), 1u);
    EXPECT_EQ(uiSetLayoutIt->entries[0].binding, 0u);
    EXPECT_EQ(uiSetLayoutIt->entries[0].type, RVX::RHIBindingType::CombinedTextureSampler);
    EXPECT_TRUE(RVX::HasFlag(uiSetLayoutIt->entries[0].visibility, RVX::RHIShaderStage::Pixel));

    auto uiPipelineLayoutIt = std::find_if(device.capturedPipelineLayoutDescs.begin(),
                                           device.capturedPipelineLayoutDescs.end(),
                                           [](const RVX::RHIPipelineLayoutDesc& desc)
                                           {
                                               return desc.debugName &&
                                                      std::string(desc.debugName) == "UIPipelineLayout";
                                           });
    ASSERT_NE(uiPipelineLayoutIt, device.capturedPipelineLayoutDescs.end());
    ASSERT_EQ(uiPipelineLayoutIt->setLayouts.size(), 1u);
    EXPECT_EQ(uiPipelineLayoutIt->setLayouts[0], cache.GetUITextureSetLayout());
    EXPECT_EQ(uiPipelineLayoutIt->pushConstantSize, 16u);
    EXPECT_TRUE(RVX::HasFlag(uiPipelineLayoutIt->pushConstantStages, RVX::RHIShaderStage::Vertex));
    EXPECT_TRUE(RVX::HasFlag(uiPipelineLayoutIt->pushConstantStages, RVX::RHIShaderStage::Pixel));
}

TEST_F(PipelineCacheValidationFixture, ReverseZOptInChangesDepthCompareAndClearConvention)
{
    if (!HasCompilerAvailable())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    FakeDevice device;
    RVX::PipelineCache cache;
    RVX::PipelineCacheConfig config;
    config.reverseZ = true;
    cache.SetConfig(config);

    ASSERT_TRUE(cache.Initialize(&device, FindShaderDirectory().string())) << cache.GetLastError();
    EXPECT_EQ(cache.GetDepthClearValue(), 0.0f);
    EXPECT_EQ(RVX::PipelineCache::GetDepthClearValue(true), 0.0f);
    EXPECT_EQ(RVX::PipelineCache::GetDepthClearValue(false), 1.0f);

    ASSERT_GE(device.capturedGraphicsPipelines.size(), 11u);
    const auto& opaqueDesc = device.capturedGraphicsPipelines[0];
    const auto& transparentDesc = device.capturedGraphicsPipelines[2];
    const auto& depthOnlyDesc = device.capturedGraphicsPipelines[3];
    const auto& skyboxDesc = device.capturedGraphicsPipelines[4];

    EXPECT_EQ(opaqueDesc.depthStencilState.depthCompareOp, RVX::RHICompareOp::GreaterEqual);
    EXPECT_TRUE(opaqueDesc.depthStencilState.depthWriteEnable);

    EXPECT_EQ(transparentDesc.depthStencilState.depthCompareOp, RVX::RHICompareOp::GreaterEqual);
    EXPECT_FALSE(transparentDesc.depthStencilState.depthWriteEnable);

    EXPECT_EQ(depthOnlyDesc.depthStencilState.depthCompareOp, RVX::RHICompareOp::GreaterEqual);
    EXPECT_TRUE(depthOnlyDesc.depthStencilState.depthWriteEnable);

    EXPECT_EQ(skyboxDesc.depthStencilState.depthCompareOp, RVX::RHICompareOp::GreaterEqual);
    EXPECT_FALSE(skyboxDesc.depthStencilState.depthWriteEnable);
}

TEST_F(PipelineCacheValidationFixture, ShadowDepthBiasStateSanitizesUnsafeValues)
{
    RVX::ShadowDepthBiasState unsafe;
    unsafe.constantBias = -0.0f;
    unsafe.slopeScaledBias = std::numeric_limits<float>::infinity();
    unsafe.biasClamp = std::numeric_limits<float>::quiet_NaN();

    RVX::ShadowDepthBiasState sanitized = RVX::PipelineCache::SanitizeShadowDepthBiasState(unsafe);
    EXPECT_FLOAT_EQ(sanitized.constantBias, 0.0f);
    EXPECT_FLOAT_EQ(sanitized.slopeScaledBias, 0.0f);
    EXPECT_FLOAT_EQ(sanitized.biasClamp, 0.0f);

    unsafe.constantBias = 50000.0f;
    unsafe.slopeScaledBias = 64.0f;
    unsafe.biasClamp = 2.0f;

    sanitized = RVX::PipelineCache::SanitizeShadowDepthBiasState(unsafe);
    EXPECT_FLOAT_EQ(sanitized.constantBias, 10000.0f);
    EXPECT_FLOAT_EQ(sanitized.slopeScaledBias, 16.0f);
    EXPECT_FLOAT_EQ(sanitized.biasClamp, 0.0f);
}

TEST_F(PipelineCacheValidationFixture, ShadowDepthPipelineUsesPurposeKeyAndSanitizedCasterBias)
{
    if (!HasCompilerAvailable())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    FakeDevice device;
    RVX::PipelineCache cache;
    ASSERT_TRUE(cache.Initialize(&device, FindShaderDirectory().string())) << cache.GetLastError();

    const size_t initialPipelineCount = device.capturedGraphicsPipelines.size();
    ASSERT_NE(cache.GetDepthOnlyPipeline(), nullptr);
    ASSERT_GT(initialPipelineCount, 3u);
    const RVX::RHIGraphicsPipelineDesc& genericDepthDesc = device.capturedGraphicsPipelines[3];
    EXPECT_FLOAT_EQ(genericDepthDesc.rasterizerState.depthBias, 0.0f);
    EXPECT_FLOAT_EQ(genericDepthDesc.rasterizerState.slopeScaledDepthBias, 0.0f);
    EXPECT_FLOAT_EQ(genericDepthDesc.rasterizerState.depthBiasClamp, 0.0f);

    RVX::ShadowDepthBiasState zeroBias;
    ASSERT_NE(cache.GetShadowDepthPipeline(zeroBias), nullptr);
    const RVX::uint64 zeroShadowHash = cache.GetStats().lastPipelineStateHash;
    ASSERT_EQ(device.capturedGraphicsPipelines.size(), initialPipelineCount + 1u);

    const RVX::RHIGraphicsPipelineDesc& zeroShadowDesc = device.capturedGraphicsPipelines.back();
    EXPECT_STREQ(zeroShadowDesc.debugName, "ShadowDepthPipeline");
    EXPECT_FLOAT_EQ(zeroShadowDesc.rasterizerState.depthBias, 0.0f);
    EXPECT_FLOAT_EQ(zeroShadowDesc.rasterizerState.slopeScaledDepthBias, 0.0f);
    EXPECT_FLOAT_EQ(zeroShadowDesc.rasterizerState.depthBiasClamp, 0.0f);

    RVX::ShadowDepthBiasState configuredBias;
    configuredBias.constantBias = 50000.0f;
    configuredBias.slopeScaledBias = 2.5f;
    configuredBias.biasClamp = 2.0f;
    ASSERT_NE(cache.GetShadowDepthPipeline(configuredBias), nullptr);
    const RVX::uint64 configuredShadowHash = cache.GetStats().lastPipelineStateHash;
    ASSERT_EQ(device.capturedGraphicsPipelines.size(), initialPipelineCount + 2u);
    EXPECT_NE(configuredShadowHash, zeroShadowHash);

    const RVX::RHIGraphicsPipelineDesc& configuredShadowDesc = device.capturedGraphicsPipelines.back();
    EXPECT_STREQ(configuredShadowDesc.debugName, "ShadowDepthPipeline");
    EXPECT_FLOAT_EQ(configuredShadowDesc.rasterizerState.depthBias, 10000.0f);
    EXPECT_FLOAT_EQ(configuredShadowDesc.rasterizerState.slopeScaledDepthBias, 2.5f);
    EXPECT_FLOAT_EQ(configuredShadowDesc.rasterizerState.depthBiasClamp, 0.0f);

    ASSERT_NE(cache.GetShadowDepthPipeline(configuredBias), nullptr);
    EXPECT_EQ(device.capturedGraphicsPipelines.size(), initialPipelineCount + 2u);
    EXPECT_EQ(cache.GetStats().lastPipelineStateHash, configuredShadowHash);
}

TEST_F(PipelineCacheValidationFixture, VulkanRasterizerEnablesDepthBiasForSlopeAndClamp)
{
    const fs::path shaderDir = FindShaderDirectory();
    if (shaderDir.empty())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    const fs::path vulkanPipelinePath = shaderDir.parent_path().parent_path() /
                                        "RHI_Vulkan" / "Private" / "VulkanPipeline.cpp";
    const std::string source = ReadTextFile(vulkanPipelinePath);
    ASSERT_FALSE(source.empty());
    EXPECT_NE(source.find("desc.rasterizerState.depthBias != 0.0f"), std::string::npos);
    EXPECT_NE(source.find("desc.rasterizerState.slopeScaledDepthBias != 0.0f"), std::string::npos);
    EXPECT_NE(source.find("desc.rasterizerState.depthBiasClamp != 0.0f"), std::string::npos);
}

TEST_F(PipelineCacheValidationFixture, ManifestMissingIsColdInitAndSavesMetadata)
{
    if (!HasCompilerAvailable())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    TempDirectory temp("rvx_pipeline_manifest_cold");
    FakeDevice device;
    RVX::PipelineCache cache;
    cache.SetConfig(ConfigWithManifest(temp.Path()));

    ASSERT_TRUE(cache.Initialize(&device, FindShaderDirectory().string())) << cache.GetLastError();
    EXPECT_FALSE(cache.GetStats().manifestLoaded);
    EXPECT_FALSE(cache.GetStats().manifestValid);
    EXPECT_FALSE(cache.GetStats().manifestInvalidated);
    EXPECT_TRUE(fs::exists(temp.Path() / RVX::PipelineCache::GetManifestFileName()));

    const std::string manifest = ReadTextFile(temp.Path() / RVX::PipelineCache::GetManifestFileName());
    EXPECT_NE(manifest.find("version=11"), std::string::npos);
    EXPECT_NE(manifest.find("skyboxVertexShaderHash="), std::string::npos);
    EXPECT_NE(manifest.find("skyboxPixelShaderHash="), std::string::npos);
    EXPECT_NE(manifest.find("skyboxPipelineHash="), std::string::npos);
    EXPECT_NE(manifest.find("fxaaVertexShaderHash="), std::string::npos);
    EXPECT_NE(manifest.find("fxaaPixelShaderHash="), std::string::npos);
    EXPECT_NE(manifest.find("fxaaPipelineHash="), std::string::npos);
    EXPECT_NE(manifest.find("colorGradingVertexShaderHash="), std::string::npos);
    EXPECT_NE(manifest.find("colorGradingPixelShaderHash="), std::string::npos);
    EXPECT_NE(manifest.find("colorGradingPipelineHash="), std::string::npos);
    EXPECT_NE(manifest.find("chromaticAberrationVertexShaderHash="), std::string::npos);
    EXPECT_NE(manifest.find("chromaticAberrationPixelShaderHash="), std::string::npos);
    EXPECT_NE(manifest.find("chromaticAberrationPipelineHash="), std::string::npos);
    EXPECT_NE(manifest.find("vignetteVertexShaderHash="), std::string::npos);
    EXPECT_NE(manifest.find("vignettePixelShaderHash="), std::string::npos);
    EXPECT_NE(manifest.find("vignettePipelineHash="), std::string::npos);
    EXPECT_NE(manifest.find("uiVertexShaderHash="), std::string::npos);
    EXPECT_NE(manifest.find("uiPixelShaderHash="), std::string::npos);
    EXPECT_NE(manifest.find("uiPipelineHash="), std::string::npos);
}

TEST_F(PipelineCacheValidationFixture, ManifestReloadsAsValidForSameInputs)
{
    if (!HasCompilerAvailable())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    TempDirectory temp("rvx_pipeline_manifest_reload");

    {
        FakeDevice device;
        RVX::PipelineCache cache;
        cache.SetConfig(ConfigWithManifest(temp.Path()));
        ASSERT_TRUE(cache.Initialize(&device, FindShaderDirectory().string())) << cache.GetLastError();
    }

    FakeDevice device;
    RVX::PipelineCache cache;
    cache.SetConfig(ConfigWithManifest(temp.Path()));
    ASSERT_TRUE(cache.Initialize(&device, FindShaderDirectory().string())) << cache.GetLastError();

    EXPECT_TRUE(cache.GetStats().manifestLoaded);
    EXPECT_TRUE(cache.GetStats().manifestValid);
    EXPECT_FALSE(cache.GetStats().manifestInvalidated);
}

TEST_F(PipelineCacheValidationFixture, ManifestInvalidatesWhenConfigChanges)
{
    if (!HasCompilerAvailable())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    TempDirectory temp("rvx_pipeline_manifest_stale");

    {
        FakeDevice device;
        RVX::PipelineCache cache;
        cache.SetConfig(ConfigWithManifest(temp.Path()));
        ASSERT_TRUE(cache.Initialize(&device, FindShaderDirectory().string())) << cache.GetLastError();
    }

    RVX::PipelineCacheConfig changedConfig = ConfigWithManifest(temp.Path());
    changedConfig.renderTargetFormat = RVX::RHIFormat::RGBA16_FLOAT;

    FakeDevice device;
    RVX::PipelineCache cache;
    cache.SetConfig(changedConfig);
    ASSERT_TRUE(cache.Initialize(&device, FindShaderDirectory().string())) << cache.GetLastError();

    EXPECT_TRUE(cache.GetStats().manifestLoaded);
    EXPECT_FALSE(cache.GetStats().manifestValid);
    EXPECT_TRUE(cache.GetStats().manifestInvalidated);
}

TEST_F(PipelineCacheValidationFixture, ManifestInvalidatesWhenToneMappingOutputFormatChanges)
{
    if (!HasCompilerAvailable())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    TempDirectory temp("rvx_pipeline_manifest_tonemapping_format_stale");

    {
        FakeDevice device;
        RVX::PipelineCache cache;
        cache.SetConfig(ConfigWithManifest(temp.Path()));
        ASSERT_TRUE(cache.Initialize(&device, FindShaderDirectory().string())) << cache.GetLastError();
    }

    RVX::PipelineCacheConfig changedConfig = ConfigWithManifest(temp.Path());
    changedConfig.toneMappingOutputFormat = RVX::RHIFormat::BGRA8_UNORM;

    FakeDevice device;
    RVX::PipelineCache cache;
    cache.SetConfig(changedConfig);
    ASSERT_TRUE(cache.Initialize(&device, FindShaderDirectory().string())) << cache.GetLastError();

    EXPECT_TRUE(cache.GetStats().manifestLoaded);
    EXPECT_FALSE(cache.GetStats().manifestValid);
    EXPECT_TRUE(cache.GetStats().manifestInvalidated);
}

TEST_F(PipelineCacheValidationFixture, ManifestInvalidatesWhenToneMappingPipelineHashChanges)
{
    if (!HasCompilerAvailable())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    TempDirectory temp("rvx_pipeline_manifest_tonemapping_stale");
    const fs::path manifestPath = temp.Path() / RVX::PipelineCache::GetManifestFileName();
    RVX::uint64 firstToneMappingHash = 0;

    {
        FakeDevice device;
        RVX::PipelineCache cache;
        cache.SetConfig(ConfigWithManifest(temp.Path()));
        ASSERT_TRUE(cache.Initialize(&device, FindShaderDirectory().string())) << cache.GetLastError();
        firstToneMappingHash = cache.GetStats().toneMappingPipelineHash;
        ASSERT_NE(firstToneMappingHash, 0u);
    }

    const RVX::uint64 staleToneMappingHash = firstToneMappingHash == 1u ? 2u : 1u;
    ReplaceManifestFieldValue(manifestPath, "toneMappingPipelineHash", std::to_string(staleToneMappingHash));

    FakeDevice device;
    RVX::PipelineCache cache;
    cache.SetConfig(ConfigWithManifest(temp.Path()));
    ASSERT_TRUE(cache.Initialize(&device, FindShaderDirectory().string())) << cache.GetLastError();

    EXPECT_TRUE(cache.GetStats().manifestLoaded);
    EXPECT_FALSE(cache.GetStats().manifestValid);
    EXPECT_TRUE(cache.GetStats().manifestInvalidated);
}

TEST_F(PipelineCacheValidationFixture, ManifestInvalidatesWhenBloomPipelineHashChanges)
{
    if (!HasCompilerAvailable())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    TempDirectory temp("rvx_pipeline_manifest_bloom_stale");
    const fs::path manifestPath = temp.Path() / RVX::PipelineCache::GetManifestFileName();
    RVX::uint64 firstBloomHash = 0;

    {
        FakeDevice device;
        RVX::PipelineCache cache;
        cache.SetConfig(ConfigWithManifest(temp.Path()));
        ASSERT_TRUE(cache.Initialize(&device, FindShaderDirectory().string())) << cache.GetLastError();
        firstBloomHash = cache.GetStats().bloomPipelineHash;
        ASSERT_NE(firstBloomHash, 0u);
    }

    const RVX::uint64 staleBloomHash = firstBloomHash == 1u ? 2u : 1u;
    ReplaceManifestFieldValue(manifestPath, "bloomPipelineHash", std::to_string(staleBloomHash));

    FakeDevice device;
    RVX::PipelineCache cache;
    cache.SetConfig(ConfigWithManifest(temp.Path()));
    ASSERT_TRUE(cache.Initialize(&device, FindShaderDirectory().string())) << cache.GetLastError();

    EXPECT_TRUE(cache.GetStats().manifestLoaded);
    EXPECT_FALSE(cache.GetStats().manifestValid);
    EXPECT_TRUE(cache.GetStats().manifestInvalidated);
}

TEST_F(PipelineCacheValidationFixture, ManifestInvalidatesWhenColorGradingPipelineHashChanges)
{
    if (!HasCompilerAvailable())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    TempDirectory temp("rvx_pipeline_manifest_colorgrading_stale");
    const fs::path manifestPath = temp.Path() / RVX::PipelineCache::GetManifestFileName();
    RVX::uint64 firstColorGradingHash = 0;

    {
        FakeDevice device;
        RVX::PipelineCache cache;
        cache.SetConfig(ConfigWithManifest(temp.Path()));
        ASSERT_TRUE(cache.Initialize(&device, FindShaderDirectory().string())) << cache.GetLastError();
        firstColorGradingHash = cache.GetStats().colorGradingPipelineHash;
        ASSERT_NE(firstColorGradingHash, 0u);
    }

    const RVX::uint64 staleColorGradingHash = firstColorGradingHash == 1u ? 2u : 1u;
    ReplaceManifestFieldValue(manifestPath, "colorGradingPipelineHash", std::to_string(staleColorGradingHash));

    FakeDevice device;
    RVX::PipelineCache cache;
    cache.SetConfig(ConfigWithManifest(temp.Path()));
    ASSERT_TRUE(cache.Initialize(&device, FindShaderDirectory().string())) << cache.GetLastError();

    EXPECT_TRUE(cache.GetStats().manifestLoaded);
    EXPECT_FALSE(cache.GetStats().manifestValid);
    EXPECT_TRUE(cache.GetStats().manifestInvalidated);
}

TEST_F(PipelineCacheValidationFixture, ManifestInvalidatesWhenChromaticAberrationPipelineHashChanges)
{
    if (!HasCompilerAvailable())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    TempDirectory temp("rvx_pipeline_manifest_chromatic_aberration_stale");
    const fs::path manifestPath = temp.Path() / RVX::PipelineCache::GetManifestFileName();
    RVX::uint64 firstChromaticAberrationHash = 0;

    {
        FakeDevice device;
        RVX::PipelineCache cache;
        cache.SetConfig(ConfigWithManifest(temp.Path()));
        ASSERT_TRUE(cache.Initialize(&device, FindShaderDirectory().string())) << cache.GetLastError();
        firstChromaticAberrationHash = cache.GetStats().chromaticAberrationPipelineHash;
        ASSERT_NE(firstChromaticAberrationHash, 0u);
    }

    const RVX::uint64 staleChromaticAberrationHash = firstChromaticAberrationHash == 1u ? 2u : 1u;
    ReplaceManifestFieldValue(manifestPath,
                              "chromaticAberrationPipelineHash",
                              std::to_string(staleChromaticAberrationHash));

    FakeDevice device;
    RVX::PipelineCache cache;
    cache.SetConfig(ConfigWithManifest(temp.Path()));
    ASSERT_TRUE(cache.Initialize(&device, FindShaderDirectory().string())) << cache.GetLastError();

    EXPECT_TRUE(cache.GetStats().manifestLoaded);
    EXPECT_FALSE(cache.GetStats().manifestValid);
    EXPECT_TRUE(cache.GetStats().manifestInvalidated);
}

TEST_F(PipelineCacheValidationFixture, ManifestInvalidatesWhenVignettePipelineHashChanges)
{
    if (!HasCompilerAvailable())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    TempDirectory temp("rvx_pipeline_manifest_vignette_stale");
    const fs::path manifestPath = temp.Path() / RVX::PipelineCache::GetManifestFileName();
    RVX::uint64 firstVignetteHash = 0;

    {
        FakeDevice device;
        RVX::PipelineCache cache;
        cache.SetConfig(ConfigWithManifest(temp.Path()));
        ASSERT_TRUE(cache.Initialize(&device, FindShaderDirectory().string())) << cache.GetLastError();
        firstVignetteHash = cache.GetStats().vignettePipelineHash;
        ASSERT_NE(firstVignetteHash, 0u);
    }

    const RVX::uint64 staleVignetteHash = firstVignetteHash == 1u ? 2u : 1u;
    ReplaceManifestFieldValue(manifestPath, "vignettePipelineHash", std::to_string(staleVignetteHash));

    FakeDevice device;
    RVX::PipelineCache cache;
    cache.SetConfig(ConfigWithManifest(temp.Path()));
    ASSERT_TRUE(cache.Initialize(&device, FindShaderDirectory().string())) << cache.GetLastError();

    EXPECT_TRUE(cache.GetStats().manifestLoaded);
    EXPECT_FALSE(cache.GetStats().manifestValid);
    EXPECT_TRUE(cache.GetStats().manifestInvalidated);
}

TEST_F(PipelineCacheValidationFixture, ManifestInvalidatesWhenFXAAPipelineHashChanges)
{
    if (!HasCompilerAvailable())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    TempDirectory temp("rvx_pipeline_manifest_fxaa_stale");
    const fs::path manifestPath = temp.Path() / RVX::PipelineCache::GetManifestFileName();
    RVX::uint64 firstFXAAHash = 0;

    {
        FakeDevice device;
        RVX::PipelineCache cache;
        cache.SetConfig(ConfigWithManifest(temp.Path()));
        ASSERT_TRUE(cache.Initialize(&device, FindShaderDirectory().string())) << cache.GetLastError();
        firstFXAAHash = cache.GetStats().fxaaPipelineHash;
        ASSERT_NE(firstFXAAHash, 0u);
    }

    const RVX::uint64 staleFXAAHash = firstFXAAHash == 1u ? 2u : 1u;
    ReplaceManifestFieldValue(manifestPath, "fxaaPipelineHash", std::to_string(staleFXAAHash));

    FakeDevice device;
    RVX::PipelineCache cache;
    cache.SetConfig(ConfigWithManifest(temp.Path()));
    ASSERT_TRUE(cache.Initialize(&device, FindShaderDirectory().string())) << cache.GetLastError();

    EXPECT_TRUE(cache.GetStats().manifestLoaded);
    EXPECT_FALSE(cache.GetStats().manifestValid);
    EXPECT_TRUE(cache.GetStats().manifestInvalidated);
}

TEST_F(PipelineCacheValidationFixture, ManifestInvalidatesWhenUIPipelineHashChanges)
{
    if (!HasCompilerAvailable())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    TempDirectory temp("rvx_pipeline_manifest_ui_stale");
    const fs::path manifestPath = temp.Path() / RVX::PipelineCache::GetManifestFileName();
    RVX::uint64 firstUIHash = 0;

    {
        FakeDevice device;
        RVX::PipelineCache cache;
        cache.SetConfig(ConfigWithManifest(temp.Path()));
        ASSERT_TRUE(cache.Initialize(&device, FindShaderDirectory().string())) << cache.GetLastError();
        firstUIHash = cache.GetStats().uiPipelineHash;
        ASSERT_NE(firstUIHash, 0u);
    }

    const RVX::uint64 staleUIHash = firstUIHash == 1u ? 2u : 1u;
    ReplaceManifestFieldValue(manifestPath, "uiPipelineHash", std::to_string(staleUIHash));

    FakeDevice device;
    RVX::PipelineCache cache;
    cache.SetConfig(ConfigWithManifest(temp.Path()));
    ASSERT_TRUE(cache.Initialize(&device, FindShaderDirectory().string())) << cache.GetLastError();

    EXPECT_TRUE(cache.GetStats().manifestLoaded);
    EXPECT_FALSE(cache.GetStats().manifestValid);
    EXPECT_TRUE(cache.GetStats().manifestInvalidated);
}

TEST_F(PipelineCacheValidationFixture, ManifestInvalidatesWhenSkyboxPipelineHashChanges)
{
    if (!HasCompilerAvailable())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    TempDirectory temp("rvx_pipeline_manifest_skybox_stale");
    const fs::path manifestPath = temp.Path() / RVX::PipelineCache::GetManifestFileName();
    RVX::uint64 firstSkyboxHash = 0;

    {
        FakeDevice device;
        RVX::PipelineCache cache;
        cache.SetConfig(ConfigWithManifest(temp.Path()));
        ASSERT_TRUE(cache.Initialize(&device, FindShaderDirectory().string())) << cache.GetLastError();
        firstSkyboxHash = cache.GetStats().skyboxPipelineHash;
        ASSERT_NE(firstSkyboxHash, 0u);
    }

    const RVX::uint64 staleSkyboxHash = firstSkyboxHash == 1u ? 2u : 1u;
    ReplaceManifestFieldValue(manifestPath, "skyboxPipelineHash", std::to_string(staleSkyboxHash));

    FakeDevice device;
    RVX::PipelineCache cache;
    cache.SetConfig(ConfigWithManifest(temp.Path()));
    ASSERT_TRUE(cache.Initialize(&device, FindShaderDirectory().string())) << cache.GetLastError();

    EXPECT_TRUE(cache.GetStats().manifestLoaded);
    EXPECT_FALSE(cache.GetStats().manifestValid);
    EXPECT_TRUE(cache.GetStats().manifestInvalidated);
}

TEST_F(PipelineCacheValidationFixture, ManifestInvalidatesWhenSkyboxShaderChanges)
{
    if (!HasCompilerAvailable())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    TempDirectory temp("rvx_pipeline_manifest_skybox_shader");
    const fs::path shaderDir = CopyShaderDirectoryToTemp(temp.Path());
    const fs::path manifestDir = temp.Path() / "Manifest";
    RVX::uint64 firstSkyboxHash = 0;

    {
        FakeDevice device;
        RVX::PipelineCache cache;
        cache.SetConfig(ConfigWithManifest(manifestDir));
        ASSERT_TRUE(cache.Initialize(&device, shaderDir.string()));
        firstSkyboxHash = cache.GetStats().skyboxPipelineHash;
        ASSERT_NE(firstSkyboxHash, 0u);
        EXPECT_FALSE(cache.GetStats().manifestInvalidated);
    }

    const fs::path skyboxShader = shaderDir / "Skybox.hlsl";
    std::string source = ReadTextFile(skyboxShader);
    source += "\n// PipelineCacheValidation skybox hash mutation\n";
    WriteTextFile(skyboxShader, source);

    FakeDevice device;
    RVX::PipelineCache cache;
    cache.SetConfig(ConfigWithManifest(manifestDir));
    ASSERT_TRUE(cache.Initialize(&device, shaderDir.string()));
    EXPECT_TRUE(cache.GetStats().manifestLoaded);
    EXPECT_TRUE(cache.GetStats().manifestInvalidated);
    EXPECT_NE(cache.GetStats().skyboxPipelineHash, 0u);
    EXPECT_NE(cache.GetStats().skyboxPipelineHash, firstSkyboxHash);
}

TEST_F(PipelineCacheValidationFixture, ManifestInvalidatesWhenFXAAShaderChanges)
{
    if (!HasCompilerAvailable())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    TempDirectory temp("rvx_pipeline_manifest_fxaa_shader");
    const fs::path shaderDir = CopyShaderDirectoryToTemp(temp.Path());
    const fs::path manifestDir = temp.Path() / "Manifest";
    RVX::uint64 firstFXAAHash = 0;

    {
        FakeDevice device;
        RVX::PipelineCache cache;
        cache.SetConfig(ConfigWithManifest(manifestDir));
        ASSERT_TRUE(cache.Initialize(&device, shaderDir.string()));
        firstFXAAHash = cache.GetStats().fxaaPipelineHash;
        ASSERT_NE(firstFXAAHash, 0u);
        EXPECT_FALSE(cache.GetStats().manifestInvalidated);
    }

    const fs::path fxaaShader = shaderDir / "PostProcess" / "FXAA.hlsl";
    std::string source = ReadTextFile(fxaaShader);
    source += "\n// PipelineCacheValidation fxaa hash mutation\n";
    WriteTextFile(fxaaShader, source);

    FakeDevice device;
    RVX::PipelineCache cache;
    cache.SetConfig(ConfigWithManifest(manifestDir));
    ASSERT_TRUE(cache.Initialize(&device, shaderDir.string()));
    EXPECT_TRUE(cache.GetStats().manifestLoaded);
    EXPECT_TRUE(cache.GetStats().manifestInvalidated);
    EXPECT_NE(cache.GetStats().fxaaPipelineHash, 0u);
    EXPECT_NE(cache.GetStats().fxaaPipelineHash, firstFXAAHash);
}

TEST_F(PipelineCacheValidationFixture, ManifestInvalidatesWhenColorGradingShaderChanges)
{
    if (!HasCompilerAvailable())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    TempDirectory temp("rvx_pipeline_manifest_colorgrading_shader");
    const fs::path shaderDir = CopyShaderDirectoryToTemp(temp.Path());
    const fs::path manifestDir = temp.Path() / "Manifest";
    RVX::uint64 firstColorGradingHash = 0;

    {
        FakeDevice device;
        RVX::PipelineCache cache;
        cache.SetConfig(ConfigWithManifest(manifestDir));
        ASSERT_TRUE(cache.Initialize(&device, shaderDir.string()));
        firstColorGradingHash = cache.GetStats().colorGradingPipelineHash;
        ASSERT_NE(firstColorGradingHash, 0u);
        EXPECT_FALSE(cache.GetStats().manifestInvalidated);
    }

    const fs::path colorGradingShader = shaderDir / "PostProcess" / "ColorGrading.hlsl";
    std::string source = ReadTextFile(colorGradingShader);
    source += "\n// PipelineCacheValidation color grading hash mutation\n";
    WriteTextFile(colorGradingShader, source);

    FakeDevice device;
    RVX::PipelineCache cache;
    cache.SetConfig(ConfigWithManifest(manifestDir));
    ASSERT_TRUE(cache.Initialize(&device, shaderDir.string()));
    EXPECT_TRUE(cache.GetStats().manifestLoaded);
    EXPECT_TRUE(cache.GetStats().manifestInvalidated);
    EXPECT_NE(cache.GetStats().colorGradingPipelineHash, 0u);
    EXPECT_NE(cache.GetStats().colorGradingPipelineHash, firstColorGradingHash);
}

TEST_F(PipelineCacheValidationFixture, ManifestInvalidatesWhenChromaticAberrationShaderChanges)
{
    if (!HasCompilerAvailable())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    TempDirectory temp("rvx_pipeline_manifest_chromatic_aberration_shader");
    const fs::path shaderDir = CopyShaderDirectoryToTemp(temp.Path());
    const fs::path manifestDir = temp.Path() / "Manifest";
    RVX::uint64 firstChromaticAberrationHash = 0;

    {
        FakeDevice device;
        RVX::PipelineCache cache;
        cache.SetConfig(ConfigWithManifest(manifestDir));
        ASSERT_TRUE(cache.Initialize(&device, shaderDir.string()));
        firstChromaticAberrationHash = cache.GetStats().chromaticAberrationPipelineHash;
        ASSERT_NE(firstChromaticAberrationHash, 0u);
        EXPECT_FALSE(cache.GetStats().manifestInvalidated);
    }

    const fs::path shader = shaderDir / "PostProcess" / "ChromaticAberration.hlsl";
    std::string source = ReadTextFile(shader);
    source += "\n// PipelineCacheValidation chromatic aberration hash mutation\n";
    WriteTextFile(shader, source);

    FakeDevice device;
    RVX::PipelineCache cache;
    cache.SetConfig(ConfigWithManifest(manifestDir));
    ASSERT_TRUE(cache.Initialize(&device, shaderDir.string()));
    EXPECT_TRUE(cache.GetStats().manifestLoaded);
    EXPECT_TRUE(cache.GetStats().manifestInvalidated);
    EXPECT_NE(cache.GetStats().chromaticAberrationPipelineHash, 0u);
    EXPECT_NE(cache.GetStats().chromaticAberrationPipelineHash, firstChromaticAberrationHash);
}

TEST_F(PipelineCacheValidationFixture, ManifestInvalidatesWhenVignetteShaderChanges)
{
    if (!HasCompilerAvailable())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    TempDirectory temp("rvx_pipeline_manifest_vignette_shader");
    const fs::path shaderDir = CopyShaderDirectoryToTemp(temp.Path());
    const fs::path manifestDir = temp.Path() / "Manifest";
    RVX::uint64 firstVignetteHash = 0;

    {
        FakeDevice device;
        RVX::PipelineCache cache;
        cache.SetConfig(ConfigWithManifest(manifestDir));
        ASSERT_TRUE(cache.Initialize(&device, shaderDir.string()));
        firstVignetteHash = cache.GetStats().vignettePipelineHash;
        ASSERT_NE(firstVignetteHash, 0u);
        EXPECT_FALSE(cache.GetStats().manifestInvalidated);
    }

    const fs::path vignetteShader = shaderDir / "PostProcess" / "Vignette.hlsl";
    std::string source = ReadTextFile(vignetteShader);
    source += "\n// PipelineCacheValidation vignette hash mutation\n";
    WriteTextFile(vignetteShader, source);

    FakeDevice device;
    RVX::PipelineCache cache;
    cache.SetConfig(ConfigWithManifest(manifestDir));
    ASSERT_TRUE(cache.Initialize(&device, shaderDir.string()));
    EXPECT_TRUE(cache.GetStats().manifestLoaded);
    EXPECT_TRUE(cache.GetStats().manifestInvalidated);
    EXPECT_NE(cache.GetStats().vignettePipelineHash, 0u);
    EXPECT_NE(cache.GetStats().vignettePipelineHash, firstVignetteHash);
}

TEST_F(PipelineCacheValidationFixture, ManifestInvalidatesWhenUIShaderChanges)
{
    if (!HasCompilerAvailable())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    TempDirectory temp("rvx_pipeline_manifest_ui_shader");
    const fs::path shaderDir = CopyShaderDirectoryToTemp(temp.Path());
    const fs::path manifestDir = temp.Path() / "Manifest";
    RVX::uint64 firstUIHash = 0;

    {
        FakeDevice device;
        RVX::PipelineCache cache;
        cache.SetConfig(ConfigWithManifest(manifestDir));
        ASSERT_TRUE(cache.Initialize(&device, shaderDir.string()));
        firstUIHash = cache.GetStats().uiPipelineHash;
        ASSERT_NE(firstUIHash, 0u);
        EXPECT_FALSE(cache.GetStats().manifestInvalidated);
    }

    const fs::path uiShader = shaderDir / "UI.hlsl";
    std::string source = ReadTextFile(uiShader);
    source += "\n// PipelineCacheValidation ui hash mutation\n";
    WriteTextFile(uiShader, source);

    FakeDevice device;
    RVX::PipelineCache cache;
    cache.SetConfig(ConfigWithManifest(manifestDir));
    ASSERT_TRUE(cache.Initialize(&device, shaderDir.string()));
    EXPECT_TRUE(cache.GetStats().manifestLoaded);
    EXPECT_TRUE(cache.GetStats().manifestInvalidated);
    EXPECT_NE(cache.GetStats().uiPipelineHash, 0u);
    EXPECT_NE(cache.GetStats().uiPipelineHash, firstUIHash);
}

TEST_F(PipelineCacheValidationFixture, CorruptManifestInvalidatesWithoutFailingInitialization)
{
    if (!HasCompilerAvailable())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    TempDirectory temp("rvx_pipeline_manifest_corrupt");
    WriteTextFile(temp.Path() / RVX::PipelineCache::GetManifestFileName(), "not a manifest\n");

    FakeDevice device;
    RVX::PipelineCache cache;
    cache.SetConfig(ConfigWithManifest(temp.Path()));

    ASSERT_TRUE(cache.Initialize(&device, FindShaderDirectory().string())) << cache.GetLastError();
    EXPECT_TRUE(cache.GetStats().manifestLoaded);
    EXPECT_FALSE(cache.GetStats().manifestValid);
    EXPECT_TRUE(cache.GetStats().manifestInvalidated);
}

TEST_F(PipelineCacheValidationFixture, ManifestRejectsInvalidReverseZValue)
{
    if (!HasCompilerAvailable())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    TempDirectory temp("rvx_pipeline_manifest_reversez");
    const fs::path manifestPath = temp.Path() / RVX::PipelineCache::GetManifestFileName();

    {
        FakeDevice device;
        RVX::PipelineCache cache;
        cache.SetConfig(ConfigWithManifest(temp.Path()));
        ASSERT_TRUE(cache.Initialize(&device, FindShaderDirectory().string())) << cache.GetLastError();
    }

    ReplaceManifestFieldValue(manifestPath, "reverseZ", "2");

    FakeDevice device;
    RVX::PipelineCache cache;
    cache.SetConfig(ConfigWithManifest(temp.Path()));

    ASSERT_TRUE(cache.Initialize(&device, FindShaderDirectory().string())) << cache.GetLastError();
    EXPECT_TRUE(cache.GetStats().manifestLoaded);
    EXPECT_FALSE(cache.GetStats().manifestValid);
    EXPECT_TRUE(cache.GetStats().manifestInvalidated);
}

TEST_F(PipelineCacheValidationFixture, ManifestWriteFailureDoesNotFailInitialization)
{
    if (!HasCompilerAvailable())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    TempDirectory temp("rvx_pipeline_manifest_write_failure");
    const fs::path fileInsteadOfDirectory = temp.Path() / "not_a_directory";
    WriteTextFile(fileInsteadOfDirectory, "manifest directory parent is a file");

    RVX::PipelineCacheConfig config;
    config.manifestDirectory = fileInsteadOfDirectory;

    FakeDevice device;
    RVX::PipelineCache cache;
    cache.SetConfig(config);

    EXPECT_TRUE(cache.Initialize(&device, FindShaderDirectory().string())) << cache.GetLastError();
    EXPECT_FALSE(fs::exists(fileInsteadOfDirectory / RVX::PipelineCache::GetManifestFileName()));
}

TEST_F(PipelineCacheValidationFixture, BackendPipelineCreationFailureIsVisible)
{
    if (!HasCompilerAvailable())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    FakeDevice device;
    device.failPipelineCreation = true;
    RVX::PipelineCache cache;

    EXPECT_FALSE(cache.Initialize(&device, FindShaderDirectory().string()));
    EXPECT_NE(cache.GetLastError().find("Backend failed to create pipeline"), std::string::npos);
}

TEST_F(PipelineCacheValidationFixture, ToneMappingPipelineCreationFailureIsVisible)
{
    if (!HasCompilerAvailable())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    FakeDevice device;
    device.failPipelineCreationAtIndex = 6;
    RVX::PipelineCache cache;

    EXPECT_FALSE(cache.Initialize(&device, FindShaderDirectory().string()));
    EXPECT_NE(cache.GetLastError().find("ToneMapping pipeline"), std::string::npos);
}

TEST_F(PipelineCacheValidationFixture, SkyboxPipelineCreationFailureIsVisible)
{
    if (!HasCompilerAvailable())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    FakeDevice device;
    device.failPipelineCreationAtIndex = 5;
    RVX::PipelineCache cache;

    EXPECT_FALSE(cache.Initialize(&device, FindShaderDirectory().string()));
    EXPECT_NE(cache.GetLastError().find("Skybox pipeline"), std::string::npos);
}

TEST_F(PipelineCacheValidationFixture, BloomPipelineCreationFailureIsVisible)
{
    if (!HasCompilerAvailable())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    FakeDevice device;
    device.failPipelineCreationAtIndex = 7;
    RVX::PipelineCache cache;

    EXPECT_FALSE(cache.Initialize(&device, FindShaderDirectory().string()));
    EXPECT_NE(cache.GetLastError().find("Bloom pipeline"), std::string::npos);
}

TEST_F(PipelineCacheValidationFixture, RayTracedReflectionCompositePipelineCreationFailureIsVisible)
{
    if (!HasCompilerAvailable())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    FakeDevice device;
    device.failPipelineCreationAtIndex = 8;
    RVX::PipelineCache cache;

    EXPECT_FALSE(cache.Initialize(&device, FindShaderDirectory().string()));
    EXPECT_NE(cache.GetLastError().find("RayTracedReflectionComposite pipeline"), std::string::npos);
}

TEST_F(PipelineCacheValidationFixture, RayTracedReflectionDenoisePipelineCreationFailureIsVisible)
{
    if (!HasCompilerAvailable())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    FakeDevice device;
    device.failPipelineCreationAtIndex = 9;
    RVX::PipelineCache cache;

    EXPECT_FALSE(cache.Initialize(&device, FindShaderDirectory().string()));
    EXPECT_NE(cache.GetLastError().find("RayTracedReflectionDenoise pipeline"), std::string::npos);
}

TEST_F(PipelineCacheValidationFixture, VignettePipelineCreationFailureIsVisible)
{
    if (!HasCompilerAvailable())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    FakeDevice device;
    device.failPipelineCreationAtIndex = 10;
    RVX::PipelineCache cache;

    EXPECT_FALSE(cache.Initialize(&device, FindShaderDirectory().string()));
    EXPECT_NE(cache.GetLastError().find("Vignette pipeline"), std::string::npos);
}

TEST_F(PipelineCacheValidationFixture, FXAAPipelineCreationFailureIsVisible)
{
    if (!HasCompilerAvailable())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    FakeDevice device;
    device.failPipelineCreationAtIndex = 11;
    RVX::PipelineCache cache;

    EXPECT_FALSE(cache.Initialize(&device, FindShaderDirectory().string()));
    EXPECT_NE(cache.GetLastError().find("FXAA pipeline"), std::string::npos);
}

TEST_F(PipelineCacheValidationFixture, ColorGradingPipelineCreationFailureIsVisible)
{
    if (!HasCompilerAvailable())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    FakeDevice device;
    device.failPipelineCreationAtIndex = 12;
    RVX::PipelineCache cache;

    EXPECT_FALSE(cache.Initialize(&device, FindShaderDirectory().string()));
    EXPECT_NE(cache.GetLastError().find("ColorGrading pipeline"), std::string::npos);
}

TEST_F(PipelineCacheValidationFixture, ChromaticAberrationPipelineCreationFailureIsVisible)
{
    if (!HasCompilerAvailable())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    FakeDevice device;
    device.failPipelineCreationAtIndex = 13;
    RVX::PipelineCache cache;

    EXPECT_FALSE(cache.Initialize(&device, FindShaderDirectory().string()));
    EXPECT_NE(cache.GetLastError().find("ChromaticAberration pipeline"), std::string::npos);
}

TEST_F(PipelineCacheValidationFixture, UIPipelineCreationFailureIsVisible)
{
    if (!HasCompilerAvailable())
    {
        GTEST_SKIP() << "Render/Shaders directory not found";
    }

    FakeDevice device;
    device.failPipelineCreationAtIndex = 14;
    RVX::PipelineCache cache;

    EXPECT_FALSE(cache.Initialize(&device, FindShaderDirectory().string()));
    EXPECT_NE(cache.GetLastError().find("UI pipeline"), std::string::npos);
}
