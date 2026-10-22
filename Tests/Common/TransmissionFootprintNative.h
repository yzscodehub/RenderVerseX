#pragma once

#include "Common/GpuTestUtils.h"
#include "EngineDevelopmentComposition/EngineDevelopmentComposition.h"
#include "RenderContracts/TransmissionFootprint.h"
#include "ShaderContracts/ShaderProgramProvider.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

using namespace RVX;

namespace RVX::Test
{
    class ScopedTransmissionFootprintShaderFile final
    {
    public:
        explicit ScopedTransmissionFootprintShaderFile(
            const std::filesystem::path& productionInclude)
        {
            static std::atomic<uint64> s_nextId{0};
            std::error_code error;
            const std::filesystem::path temporaryDirectory =
                std::filesystem::temp_directory_path(error);
            if (error)
            {
                m_error = "Unable to locate temporary directory: " + error.message();
                return;
            }

            const auto ticks = std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            m_path = temporaryDirectory / ("rvx_transmission_footprint_native_" +
                std::to_string(ticks) + "_" +
                std::to_string(s_nextId.fetch_add(1, std::memory_order_relaxed)) + ".hlsl");

            std::ofstream stream(m_path, std::ios::binary | std::ios::trunc);
            if (!stream)
            {
                m_error = "Unable to create temporary shader source: " + m_path.string();
                return;
            }

            stream << "#include \"" << productionInclude.generic_string() << "\"\n"
                   << R"(
#ifndef RVX_TRANSMISSION_FOOTPRINT_REVERSE_Z
#define RVX_TRANSMISSION_FOOTPRINT_REVERSE_Z 0
#endif

Texture2D<float> RVXReductionInput : register(t0);
Texture2D<float> RVXPyramidInput : register(t1);

struct RVXTransmissionFootprintVSOutput
{
    float4 position : SV_Position;
};

RVXTransmissionFootprintVSOutput VSMain(uint vertexId : SV_VertexID)
{
    const float2 position = vertexId == 0u ? float2(-1.0f, -1.0f) :
        (vertexId == 1u ? float2(-1.0f, 3.0f) : float2(3.0f, -1.0f));
    RVXTransmissionFootprintVSOutput output;
    output.position = float4(position, 0.0f, 1.0f);
    return output;
}

float4 PSMain(RVXTransmissionFootprintVSOutput input) : SV_Target0
{
    const uint2 pixel = uint2(input.position.xy);
    if (pixel.x >= 2u)
    {
        const float2 clearUV = float2(0.8f, 0.2f);
        const float2 blockedUV = float2(0.2f, 0.2f);
        float2 candidate = clearUV;
        float2 base = blockedUV;
        bool projected = true;
        float lod = 0.0f;
        float exitDepth = 0.5f;
        if (pixel.x == 3u) { candidate = blockedUV; base = clearUV; }
        if (pixel.x == 4u) { candidate = blockedUV; base = blockedUV; }
        if (pixel.x == 5u) { projected = false; base = clearUV; }
        if (pixel.x == 6u) { base = clearUV; lod = 0.5f; }
        if (pixel.x == 7u) { candidate = float2(2.0f, 0.2f); base = clearUV; }
        if (pixel.x == 8u) { base = clearUV; exitDepth = -1.0f; }
        if (pixel.x == 9u) { base = float2(0.8f, 0.8f); }
        float2 selected;
        const bool accepted = RVXResolveTransmissionSampleUV(
            RVXPyramidInput, candidate, projected, base, lod, exitDepth, selected);
        return float4(accepted ? 1.0f : 0.0f, selected, 1.0f);
    }
    if (pixel.x == 0u)
    {
        const float2 reductionUV = float2(0.5f, 0.625f);
        return float4(
            RVXReduceTransmissionDepth(RVXReductionInput, reductionUV, false,
                RVX_TRANSMISSION_FOOTPRINT_REVERSE_Z != 0, false),
            RVXReduceTransmissionDepth(RVXReductionInput, reductionUV, true,
                RVX_TRANSMISSION_FOOTPRINT_REVERSE_Z != 0, false),
            RVXSampleTransmissionDepthPyramid(RVXPyramidInput, float2(0.5f, 0.5f), 2.0f),
            RVXSampleTransmissionDepthPyramid(RVXPyramidInput, float2(0.5f, 0.5f), 2.5f));
    }

    return float4(
        RVXSampleTransmissionDepthPyramid(RVXPyramidInput, float2(0.5f, 0.5f), 4.0f),
        RVXSampleTransmissionDepthPyramid(RVXPyramidInput, float2(0.5f, 0.5f), 4.5f),
        0.0f, 0.0f);
}
)";
            stream.close();
            if (!stream)
            {
                m_error = "Unable to write temporary shader source: " + m_path.string();
                std::filesystem::remove(m_path, error);
                m_path.clear();
            }
        }

        ~ScopedTransmissionFootprintShaderFile()
        {
            std::error_code error;
            std::filesystem::remove(m_path, error);
        }

        [[nodiscard]] bool IsValid() const noexcept { return m_error.empty(); }
        [[nodiscard]] const std::filesystem::path& GetPath() const noexcept { return m_path; }
        [[nodiscard]] const std::string& GetError() const noexcept { return m_error; }

    private:
        std::filesystem::path m_path;
        std::string m_error;
    };

    struct TransmissionFootprintUploadLayout
    {
        uint64 offset = 0;
        uint32 rowPitch = 0;
        uint32 width = 0;
        uint32 height = 0;
    };

    [[nodiscard]] inline uint64 AlignTransmissionFootprintUpload(
        uint64 value, uint64 alignment) noexcept
    {
        return (value + alignment - 1u) / alignment * alignment;
    }

    [[nodiscard]] inline float32 EncodeTransmissionFootprintDepth(
        float32 canonical, bool reverseZ) noexcept
    {
        return reverseZ ? 1.0f - canonical : canonical;
    }

    [[nodiscard]] inline float32 MinimumTransmissionFootprintDepth(
        const std::vector<float32>& values, UVec2 extent,
        const TransmissionTexelBounds& bounds) noexcept
    {
        float32 nearest = 1.0f;
        if (!bounds.valid || values.size() !=
            static_cast<size_t>(extent.x) * extent.y) return 0.0f;
        for (uint32 y = bounds.first.y; y <= bounds.last.y; ++y)
        {
            for (uint32 x = bounds.first.x; x <= bounds.last.x; ++x)
            {
                nearest = std::min(nearest, values[static_cast<size_t>(y) * extent.x + x]);
            }
        }
        return nearest;
    }

    [[nodiscard]] inline float32 MinimumTransmissionMipFootprint(
        const std::vector<std::vector<float32>>& values,
        UVec2 baseExtent, const TransmissionMipFootprint& footprint) noexcept
    {
        if (!footprint.valid || footprint.lowerMip >= values.size() ||
            footprint.upperMip >= values.size()) return 0.0f;
        const float32 lower = MinimumTransmissionFootprintDepth(
            values[footprint.lowerMip],
            GetTransmissionMipExtent(baseExtent, footprint.lowerMip),
            footprint.lower);
        const float32 upper = MinimumTransmissionFootprintDepth(
            values[footprint.upperMip],
            GetTransmissionMipExtent(baseExtent, footprint.upperMip),
            footprint.upper);
        return std::min(lower, upper);
    }

    inline void RunTransmissionFootprintReference(
        RHIBackendType backend, bool reverseZ, bool guardBandProbe)
    {
        constexpr UVec2 reductionExtent{13u, 7u};
        constexpr UVec2 highBaseExtent{7u, 4u};
        const UVec2 sentinel = guardBandProbe ? UVec2(4u, 2u) : UVec2(5u, 3u);
        constexpr float32 backgroundDepth = 0.85f;
        constexpr float32 reductionSentinelDepth = 0.125f;
        constexpr float32 lowerMipSentinelDepth = 0.47f;
        constexpr float32 upperMipSentinelDepth = 0.13f;
        constexpr float32 skinnyMipSentinelDepth = 0.31f;
        constexpr float32 tailMipSentinelDepth = 0.07f;
        constexpr uint32 rowAlignment = 256u;
        constexpr uint64 placementAlignment = 512u;
        constexpr uint32 outputWidth = 10u;
        constexpr uint32 outputHeight = 1u;
        constexpr uint32 outputRowPitch = 256u;
        constexpr RHIClearColor outputClear{0.0f, 0.0f, 0.0f, 0.0f};
        constexpr UVec2 pyramidExtent{33u, 17u};
        constexpr Vec2 reductionUV{0.5f, 0.625f};
        constexpr Vec2 pyramidUV{0.5f, 0.5f};

        RHIDeviceDesc deviceDesc;
        auto device = RVX::Test::CreateNativeValidationDevice(backend, deviceDesc);
        RVX_GTEST_REQUIRE_GPU_DEVICE(device, backend);

        const auto shaderRoot = std::filesystem::path(__FILE__).parent_path().parent_path().parent_path() /
            "Source/Runtime/Render/Shaders";
        const auto productionInclude = shaderRoot / "Translucency/TransmissionFootprint.hlsli";
        ASSERT_TRUE(std::filesystem::is_regular_file(productionInclude));
        ScopedTransmissionFootprintShaderFile source(productionInclude);
        ASSERT_TRUE(source.IsValid()) << source.GetError();

        auto provider = CreateDevelopmentShaderProgramProviderFactory(shaderRoot)();
        ASSERT_NE(provider, nullptr);
        const auto loadShader = [&](RHIShaderStage stage, const char* entryPoint)
        {
            ShaderProgramRequest request;
            request.logicalAssetId = source.GetPath().string();
            request.stage = stage;
            request.entryPoint = entryPoint;
            request.backend = backend;
            request.defines.push_back({"RVX_TRANSMISSION_FOOTPRINT_REVERSE_Z",
                reverseZ ? "1" : "0"});
            const ShaderProgramResult loaded = provider->Load(*device, request);
            EXPECT_TRUE(loaded.Succeeded()) << loaded.diagnostic;
            return loaded.shader;
        };

        const RHIShaderRef vertexShader = loadShader(RHIShaderStage::Vertex, "VSMain");
        const RHIShaderRef pixelShader = loadShader(RHIShaderStage::Pixel, "PSMain");
        ASSERT_NE(vertexShader.Get(), nullptr);
        ASSERT_NE(pixelShader.Get(), nullptr);

        RHIDescriptorSetLayoutDesc setLayoutDesc;
        setLayoutDesc.AddBinding(0, RHIBindingType::SampledTexture, RHIShaderStage::Pixel);
        setLayoutDesc.AddBinding(1, RHIBindingType::SampledTexture, RHIShaderStage::Pixel);
        setLayoutDesc.debugName = "TransmissionFootprintNativeSet";
        const RHIDescriptorSetLayoutRef setLayout =
            device->CreateDescriptorSetLayout(setLayoutDesc);
        ASSERT_NE(setLayout.Get(), nullptr);

        RHIPipelineLayoutDesc pipelineLayoutDesc;
        pipelineLayoutDesc.setLayouts.push_back(setLayout.Get());
        pipelineLayoutDesc.debugName = "TransmissionFootprintNativeLayout";
        const RHIPipelineLayoutRef pipelineLayout = device->CreatePipelineLayout(pipelineLayoutDesc);
        ASSERT_NE(pipelineLayout.Get(), nullptr);

        RHIGraphicsPipelineDesc pipelineDesc;
        pipelineDesc.vertexShader = vertexShader.Get();
        pipelineDesc.pixelShader = pixelShader.Get();
        pipelineDesc.pipelineLayout = pipelineLayout.Get();
        pipelineDesc.rasterizerState = RHIRasterizerState::NoCull();
        pipelineDesc.depthStencilState = RHIDepthStencilState::Disabled();
        pipelineDesc.numRenderTargets = 1;
        pipelineDesc.renderTargetFormats[0] = RHIFormat::RGBA32_FLOAT;
        pipelineDesc.depthStencilFormat = RHIFormat::Unknown;
        pipelineDesc.primitiveTopology = RHIPrimitiveTopology::TriangleList;
        pipelineDesc.debugName = "TransmissionFootprintNativePipeline";
        const RHIPipelineRef pipeline = device->CreateGraphicsPipeline(pipelineDesc);
        ASSERT_NE(pipeline.Get(), nullptr);

        EXPECT_EQ(highBaseExtent, UVec2((reductionExtent.x + 1u) / 2u,
                                        (reductionExtent.y + 1u) / 2u));
        const TransmissionTexelBounds reductionBounds =
            ResolveTransmissionReductionFootprint(reductionUV, reductionExtent, false);
        const TransmissionTexelBounds fullCopyBounds =
            ResolveTransmissionReductionFootprint(reductionUV, reductionExtent, true);
        ASSERT_TRUE(reductionBounds.valid);
        ASSERT_TRUE(fullCopyBounds.valid);
        ASSERT_GE(sentinel.x, reductionBounds.first.x);
        ASSERT_LE(sentinel.x, reductionBounds.last.x);
        ASSERT_GE(sentinel.y, reductionBounds.first.y);
        ASSERT_LE(sentinel.y, reductionBounds.last.y);
        const TransmissionTexelBounds legacyLocalBounds{
            UVec2(6u, 3u), UVec2(7u, 4u), true};
        ASSERT_TRUE(sentinel.x < legacyLocalBounds.first.x ||
                    sentinel.x > legacyLocalBounds.last.x ||
                    sentinel.y < legacyLocalBounds.first.y ||
                    sentinel.y > legacyLocalBounds.last.y);
        if (guardBandProbe)
        {
            ASSERT_TRUE(sentinel.x < fullCopyBounds.first.x ||
                        sentinel.x > fullCopyBounds.last.x ||
                        sentinel.y < fullCopyBounds.first.y ||
                        sentinel.y > fullCopyBounds.last.y);
        }
        else
        {
            ASSERT_GE(sentinel.x, fullCopyBounds.first.x);
            ASSERT_LE(sentinel.x, fullCopyBounds.last.x);
            ASSERT_GE(sentinel.y, fullCopyBounds.first.y);
            ASSERT_LE(sentinel.y, fullCopyBounds.last.y);
        }

        std::vector<float32> reductionCanonical(
            static_cast<size_t>(reductionExtent.x) * reductionExtent.y, backgroundDepth);
        reductionCanonical[static_cast<size_t>(sentinel.y) * reductionExtent.x + sentinel.x] =
            reductionSentinelDepth;
        const float32 expectedReduction = MinimumTransmissionFootprintDepth(
            reductionCanonical, reductionExtent, reductionBounds);
        const float32 expectedFullCopy = MinimumTransmissionFootprintDepth(
            reductionCanonical, reductionExtent, fullCopyBounds);
        const float32 expectedLegacyLocal = MinimumTransmissionFootprintDepth(
            reductionCanonical, reductionExtent, legacyLocalBounds);
        EXPECT_FLOAT_EQ(expectedReduction, reductionSentinelDepth);
        EXPECT_FLOAT_EQ(expectedFullCopy,
                        guardBandProbe ? backgroundDepth : reductionSentinelDepth);
        EXPECT_FLOAT_EQ(expectedLegacyLocal, backgroundDepth);

        const uint32 mipCount = GetTransmissionMipCount(pyramidExtent);
        ASSERT_EQ(mipCount, 6u);
        std::vector<std::vector<float32>> pyramidCanonical(mipCount);
        for (uint32 mip = 0; mip < mipCount; ++mip)
        {
            const UVec2 extent = GetTransmissionMipExtent(pyramidExtent, mip);
            pyramidCanonical[mip].assign(static_cast<size_t>(extent.x) * extent.y,
                                         backgroundDepth);
        }

        // Decision matrix: mip zero has one blocked footprint; mip one blocks
        // every footprint, proving fractional LOD cannot ignore its upper mip.
        pyramidCanonical[0][3u * pyramidExtent.x + 6u] = 0.1f;
        std::fill(pyramidCanonical[1].begin(), pyramidCanonical[1].end(), 0.1f);
        const TransmissionMipFootprint lowerOnly =
            ResolveTransmissionMipFootprint(pyramidUV, pyramidExtent, mipCount, 2.0f);
        const TransmissionMipFootprint trilinear =
            ResolveTransmissionMipFootprint(pyramidUV, pyramidExtent, mipCount, 2.5f);
        const TransmissionMipFootprint skinnyOnly =
            ResolveTransmissionMipFootprint(pyramidUV, pyramidExtent, mipCount, 4.0f);
        const TransmissionMipFootprint skinnyTail =
            ResolveTransmissionMipFootprint(pyramidUV, pyramidExtent, mipCount, 4.5f);
        ASSERT_TRUE(lowerOnly.valid);
        ASSERT_TRUE(trilinear.valid);
        ASSERT_TRUE(skinnyOnly.valid);
        ASSERT_TRUE(skinnyTail.valid);
        ASSERT_EQ(lowerOnly.lowerMip, 2u);
        ASSERT_EQ(trilinear.upperMip, 3u);
        ASSERT_EQ(skinnyOnly.lowerMip, 4u);
        ASSERT_EQ(skinnyTail.upperMip, 5u);
        EXPECT_EQ(GetTransmissionMipExtent(pyramidExtent, 4u), UVec2(2u, 1u));
        EXPECT_EQ(GetTransmissionMipExtent(pyramidExtent, 5u), UVec2(1u, 1u));

        const auto putMinimum = [&](uint32 mip, const TransmissionTexelBounds& bounds,
                                    float32 value)
        {
            const UVec2 extent = GetTransmissionMipExtent(pyramidExtent, mip);
            ASSERT_TRUE(bounds.valid);
            pyramidCanonical[mip][static_cast<size_t>(bounds.first.y) * extent.x +
                                  bounds.first.x] = value;
        };
        putMinimum(2u, lowerOnly.lower, lowerMipSentinelDepth);
        putMinimum(3u, trilinear.upper, upperMipSentinelDepth);
        putMinimum(4u, skinnyOnly.lower, skinnyMipSentinelDepth);
        putMinimum(5u, skinnyTail.upper, tailMipSentinelDepth);

        const float32 expectedLowerOnly = MinimumTransmissionMipFootprint(
            pyramidCanonical, pyramidExtent, lowerOnly);
        const float32 expectedTrilinear = MinimumTransmissionMipFootprint(
            pyramidCanonical, pyramidExtent, trilinear);
        const float32 expectedSkinnyOnly = MinimumTransmissionMipFootprint(
            pyramidCanonical, pyramidExtent, skinnyOnly);
        const float32 expectedSkinnyTail = MinimumTransmissionMipFootprint(
            pyramidCanonical, pyramidExtent, skinnyTail);
        EXPECT_FLOAT_EQ(expectedLowerOnly, lowerMipSentinelDepth);
        EXPECT_FLOAT_EQ(expectedTrilinear, upperMipSentinelDepth);
        EXPECT_FLOAT_EQ(expectedSkinnyOnly, skinnyMipSentinelDepth);
        EXPECT_FLOAT_EQ(expectedSkinnyTail, tailMipSentinelDepth);

        std::vector<float32> reductionEncoded(reductionCanonical.size());
        std::transform(reductionCanonical.begin(), reductionCanonical.end(),
            reductionEncoded.begin(), [reverseZ](float32 value)
            {
                return EncodeTransmissionFootprintDepth(value, reverseZ);
            });

        RHITextureDesc reductionDesc = RHITextureDesc::Texture2D(
            reductionExtent.x, reductionExtent.y, RHIFormat::R32_FLOAT,
            RHITextureUsage::ShaderResource | RHITextureUsage::CopyDst);
        reductionDesc.debugName = "TransmissionFootprintNativeReduction";
        const RHITextureRef reductionTexture = device->CreateTexture(reductionDesc);
        ASSERT_NE(reductionTexture.Get(), nullptr);

        RHITextureDesc pyramidDesc = RHITextureDesc::Texture2D(
            pyramidExtent.x, pyramidExtent.y, RHIFormat::R32_FLOAT,
            RHITextureUsage::ShaderResource | RHITextureUsage::CopyDst);
        pyramidDesc.mipLevels = mipCount;
        pyramidDesc.debugName = "TransmissionFootprintNativePyramid";
        const RHITextureRef pyramidTexture = device->CreateTexture(pyramidDesc);
        ASSERT_NE(pyramidTexture.Get(), nullptr);

        RHITextureViewDesc reductionViewDesc;
        reductionViewDesc.format = RHIFormat::R32_FLOAT;
        reductionViewDesc.type = RHITextureViewType::ShaderResource;
        const RHITextureViewRef reductionView =
            device->CreateTextureView(reductionTexture.Get(), reductionViewDesc);
        ASSERT_NE(reductionView.Get(), nullptr);

        RHITextureViewDesc pyramidViewDesc;
        pyramidViewDesc.format = RHIFormat::R32_FLOAT;
        pyramidViewDesc.subresourceRange = RHISubresourceRange::All();
        pyramidViewDesc.type = RHITextureViewType::ShaderResource;
        const RHITextureViewRef pyramidView =
            device->CreateTextureView(pyramidTexture.Get(), pyramidViewDesc);
        ASSERT_NE(pyramidView.Get(), nullptr);

        RHITextureDesc outputDesc = RHITextureDesc::Texture2D(
            outputWidth, outputHeight, RHIFormat::RGBA32_FLOAT,
            RHITextureUsage::RenderTarget | RHITextureUsage::CopySrc);
        outputDesc.optimizedClearValue = RHIOptimizedClearValue::Color(outputClear);
        outputDesc.debugName = "TransmissionFootprintNativeOutput";
        const RHITextureRef output = device->CreateTexture(outputDesc);
        ASSERT_NE(output.Get(), nullptr);

        RHITextureViewDesc outputViewDesc;
        outputViewDesc.format = RHIFormat::RGBA32_FLOAT;
        outputViewDesc.type = RHITextureViewType::RenderTarget;
        const RHITextureViewRef outputView =
            device->CreateTextureView(output.Get(), outputViewDesc);
        ASSERT_NE(outputView.Get(), nullptr);

        TransmissionFootprintUploadLayout reductionUpload;
        reductionUpload.offset = 0u;
        reductionUpload.rowPitch = static_cast<uint32>(AlignTransmissionFootprintUpload(
            static_cast<uint64>(reductionExtent.x) * sizeof(float32), rowAlignment));
        reductionUpload.width = reductionExtent.x;
        reductionUpload.height = reductionExtent.y;

        std::vector<TransmissionFootprintUploadLayout> pyramidUploads(mipCount);
        uint64 uploadSize = AlignTransmissionFootprintUpload(
            static_cast<uint64>(reductionUpload.rowPitch) * reductionUpload.height,
            placementAlignment);
        for (uint32 mip = 0; mip < mipCount; ++mip)
        {
            const UVec2 extent = GetTransmissionMipExtent(pyramidExtent, mip);
            TransmissionFootprintUploadLayout& layout = pyramidUploads[mip];
            layout.offset = uploadSize;
            layout.rowPitch = static_cast<uint32>(AlignTransmissionFootprintUpload(
                static_cast<uint64>(extent.x) * sizeof(float32), rowAlignment));
            layout.width = extent.x;
            layout.height = extent.y;
            uploadSize = AlignTransmissionFootprintUpload(
                layout.offset + static_cast<uint64>(layout.rowPitch) * layout.height,
                placementAlignment);
        }

        RHIBufferDesc uploadDesc;
        uploadDesc.size = uploadSize;
        uploadDesc.usage = RHIBufferUsage::CopySrc;
        uploadDesc.memoryType = RHIMemoryType::Upload;
        uploadDesc.debugName = "TransmissionFootprintNativeUpload";
        const RHIBufferRef upload = device->CreateBuffer(uploadDesc);
        ASSERT_NE(upload.Get(), nullptr);
        auto* uploadBytes = static_cast<uint8*>(upload->Map());
        ASSERT_NE(uploadBytes, nullptr);
        std::memset(uploadBytes, 0, static_cast<size_t>(uploadSize));
        for (uint32 y = 0; y < reductionUpload.height; ++y)
        {
            std::memcpy(uploadBytes + reductionUpload.offset +
                            static_cast<size_t>(y) * reductionUpload.rowPitch,
                        reductionEncoded.data() + static_cast<size_t>(y) * reductionExtent.x,
                        static_cast<size_t>(reductionExtent.x) * sizeof(float32));
        }
        for (uint32 mip = 0; mip < mipCount; ++mip)
        {
            const TransmissionFootprintUploadLayout& layout = pyramidUploads[mip];
            for (uint32 y = 0; y < layout.height; ++y)
            {
                std::memcpy(uploadBytes + layout.offset + static_cast<size_t>(y) * layout.rowPitch,
                            pyramidCanonical[mip].data() + static_cast<size_t>(y) * layout.width,
                            static_cast<size_t>(layout.width) * sizeof(float32));
            }
        }
        upload->Unmap();

        RHIDescriptorSetDesc descriptorSetDesc;
        descriptorSetDesc.layout = setLayout.Get();
        descriptorSetDesc.debugName = "TransmissionFootprintNativeDescriptorSet";
        descriptorSetDesc.BindTexture(0, reductionView.Get());
        descriptorSetDesc.BindTexture(1, pyramidView.Get());
        const RHIDescriptorSetRef descriptorSet = device->CreateDescriptorSet(descriptorSetDesc);
        ASSERT_NE(descriptorSet.Get(), nullptr);

        RHIBufferDesc readbackDesc;
        readbackDesc.size = outputRowPitch * outputHeight;
        readbackDesc.usage = RHIBufferUsage::CopyDst;
        readbackDesc.memoryType = RHIMemoryType::Readback;
        readbackDesc.debugName = "TransmissionFootprintNativeReadback";
        const RHIBufferRef readback = device->CreateBuffer(readbackDesc);
        ASSERT_NE(readback.Get(), nullptr);

        const RHICommandContextRef commands =
            device->CreateCommandContext(RHICommandQueueType::Graphics);
        const RHIFenceRef fence = device->CreateFence(0);
        ASSERT_NE(commands.Get(), nullptr);
        ASSERT_NE(fence.Get(), nullptr);
        commands->Begin();
        commands->TextureBarrier(reductionTexture.Get(), RHIResourceState::Undefined,
                                 RHIResourceState::CopyDest);
        RHIBufferTextureCopyDesc reductionCopy;
        reductionCopy.bufferOffset = reductionUpload.offset;
        reductionCopy.bufferRowPitch = reductionUpload.rowPitch;
        reductionCopy.bufferImageHeight = reductionUpload.height;
        reductionCopy.textureRegion = {0, 0, reductionUpload.width, reductionUpload.height};
        commands->CopyBufferToTexture(upload.Get(), reductionTexture.Get(), reductionCopy);
        commands->TextureBarrier(reductionTexture.Get(), RHIResourceState::CopyDest,
                                 RHIResourceState::ShaderResource);

        commands->TextureBarrier(pyramidTexture.Get(), RHIResourceState::Undefined,
                                 RHIResourceState::CopyDest);
        for (uint32 mip = 0; mip < mipCount; ++mip)
        {
            const TransmissionFootprintUploadLayout& layout = pyramidUploads[mip];
            RHIBufferTextureCopyDesc pyramidCopy;
            pyramidCopy.bufferOffset = layout.offset;
            pyramidCopy.bufferRowPitch = layout.rowPitch;
            pyramidCopy.bufferImageHeight = layout.height;
            pyramidCopy.textureSubresource = mip;
            pyramidCopy.textureRegion = {0, 0, layout.width, layout.height};
            commands->CopyBufferToTexture(upload.Get(), pyramidTexture.Get(), pyramidCopy);
        }
        commands->TextureBarrier(pyramidTexture.Get(), RHIResourceState::CopyDest,
                                 RHIResourceState::ShaderResource);

        commands->TextureBarrier(output.Get(), RHIResourceState::Undefined,
                                 RHIResourceState::RenderTarget);
        RHIRenderPassDesc renderPass;
        renderPass.AddColorAttachment(outputView.Get(), RHILoadOp::Clear, RHIStoreOp::Store,
                                      outputClear);
        commands->BeginRenderPass(renderPass);
        commands->SetViewport({0.0f, 0.0f, static_cast<float32>(outputWidth),
                               static_cast<float32>(outputHeight), 0.0f, 1.0f});
        commands->SetScissor({0, 0, outputWidth, outputHeight});
        commands->SetPipeline(pipeline.Get());
        commands->SetDescriptorSet(0, descriptorSet.Get());
        commands->Draw(3, 1, 0, 0);
        commands->EndRenderPass();
        commands->TextureBarrier(output.Get(), RHIResourceState::RenderTarget,
                                 RHIResourceState::CopySource);
        commands->BufferBarrier(readback.Get(), RHIResourceState::Common,
                                RHIResourceState::CopyDest);
        RHIBufferTextureCopyDesc readbackCopy;
        readbackCopy.bufferRowPitch = outputRowPitch;
        readbackCopy.bufferImageHeight = outputHeight;
        readbackCopy.textureRegion = {0, 0, outputWidth, outputHeight};
        commands->CopyTextureToBuffer(output.Get(), readback.Get(), readbackCopy);
        commands->BufferBarrier(readback.Get(), RHIResourceState::CopyDest,
                                RHIResourceState::Common);
        commands->End();

        const uint64 submitted = device->SubmitCommandContext(commands.Get(), fence.Get());
        ASSERT_NE(submitted, 0u);
        device->WaitForFence(fence.Get(), submitted);
        ASSERT_GE(fence->GetCompletedValue(), submitted);

        const auto* pixels = static_cast<const float32*>(readback->Map());
        ASSERT_NE(pixels, nullptr);
        const float32* primary = pixels;
        const float32* skinny = pixels + 4;
        EXPECT_NEAR(primary[0], expectedReduction, 1.0e-6f);
        EXPECT_NEAR(primary[1], expectedFullCopy, 1.0e-6f);
        EXPECT_NEAR(primary[2], expectedLowerOnly, 1.0e-6f);
        EXPECT_NEAR(primary[3], expectedTrilinear, 1.0e-6f);
        EXPECT_NEAR(skinny[0], expectedSkinnyOnly, 1.0e-6f);
        EXPECT_NEAR(skinny[1], expectedSkinnyTail, 1.0e-6f);
        for (uint32 pixel = 2; pixel < outputWidth; ++pixel)
        {
            SCOPED_TRACE("guarded pixel case " + std::to_string(pixel));
            const bool accepted = pixel != 4 && pixel != 6 && pixel != 8;
            EXPECT_FLOAT_EQ(pixels[pixel * 4], accepted ? 1.0f : 0.0f);
            EXPECT_NEAR(pixels[pixel * 4 + 1], accepted ? 0.8f : 0.0f, 1.0e-6f);
            EXPECT_NEAR(pixels[pixel * 4 + 2], accepted ? 0.2f : 0.0f, 1.0e-6f);
            EXPECT_FLOAT_EQ(pixels[pixel * 4 + 3], 1.0f);
        }
        readback->Unmap();

        const RHINativeValidationDiagnostics validation = device->GetNativeValidationDiagnostics();
        EXPECT_TRUE(validation.available);
        EXPECT_TRUE(validation.readComplete);
        EXPECT_EQ(validation.warningCount, 0u);
        EXPECT_EQ(validation.errorCount, 0u);
        EXPECT_EQ(validation.corruptionCount, 0u);
    }
} // namespace RVX::Test
