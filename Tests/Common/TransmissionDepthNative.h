#pragma once

#include "Common/GpuTestUtils.h"
#include "EngineDevelopmentComposition/EngineDevelopmentComposition.h"
#include "RenderContracts/TransmissionProjection.h"
#include "ShaderContracts/ShaderProgramProvider.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <string>

using namespace RVX;

namespace RVX::Test
{
    class ScopedTransmissionDepthShaderFile final
    {
    public:
        explicit ScopedTransmissionDepthShaderFile(
            const std::filesystem::path& productionInclude)
        {
            static std::atomic<uint64> s_nextId{0};
            std::error_code error;
            const auto temporaryDirectory = std::filesystem::temp_directory_path(error);
            if (error)
            {
                m_error = "Unable to locate temporary directory: " + error.message();
                return;
            }

            const auto ticks = std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            m_path = temporaryDirectory / ("rvx_transmission_depth_native_" +
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
#ifndef RVX_TRANSMISSION_DEPTH_REVERSE_Z
#define RVX_TRANSMISSION_DEPTH_REVERSE_Z 0
#endif

Texture2D<float> RVXTransmissionDepthInput : register(t0);

struct RVXTransmissionDepthVSOutput
{
    float4 position : SV_Position;
};

RVXTransmissionDepthVSOutput VSMain(uint vertexId : SV_VertexID)
{
    const float2 position = vertexId == 0u ? float2(-1.0f, -1.0f) :
        (vertexId == 1u ? float2(-1.0f, 3.0f) : float2(3.0f, -1.0f));
    RVXTransmissionDepthVSOutput output;
    output.position = float4(position, 0.0f, 1.0f);
    return output;
}

float4 PSMain(RVXTransmissionDepthVSOutput input) : SV_Target0
{
    const uint2 pixel = uint2(input.position.xy);
    const float sampledDepth = RVXTransmissionDepthInput.Load(int3(pixel, 0));
    const bool accepted = RVXTransmissionDepthBehindExit(
        sampledDepth, 0.5f, RVX_TRANSMISSION_DEPTH_REVERSE_Z != 0);

    float3 displacement;
    float pathLength;
    const bool rayValid = RVXTransmissionRay(
        float3(0.6f, 0.0f, -0.8f), float3(0.0f, 0.0f, 1.0f), 1.5f, 0.1f,
        displacement, pathLength);

    float2 projectedUV;
    const float4x4 identity = float4x4(
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f);
    const bool projectionValid = rayValid && RVXProjectTransmissionCandidate(
        identity, float3(0.0f, 0.0f, 0.5f) + displacement, float2(8.0f, 1.0f),
        projectedUV);
    return float4(accepted ? 1.0f : 0.0f,
                  rayValid ? pathLength : -1.0f,
                  projectionValid ? projectedUV.x : -1.0f,
                  projectionValid ? projectedUV.y : -1.0f);
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

        ~ScopedTransmissionDepthShaderFile()
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

    inline void RunTransmissionDepthReference(RHIBackendType backend, bool reverseZ)
    {
        RHIDeviceDesc deviceDesc;
        auto device = RVX::Test::CreateNativeValidationDevice(backend, deviceDesc);
        RVX_GTEST_REQUIRE_GPU_DEVICE(device, backend);

        const auto shaderRoot = std::filesystem::path(__FILE__).parent_path().parent_path().parent_path() /
            "Source/Runtime/Render/Shaders";
        const auto productionInclude = shaderRoot / "Translucency/TransmissionProjection.hlsli";
        ASSERT_TRUE(std::filesystem::is_regular_file(productionInclude));
        ScopedTransmissionDepthShaderFile source(productionInclude);
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
            request.defines.push_back({"RVX_TRANSMISSION_DEPTH_REVERSE_Z", reverseZ ? "1" : "0"});
            const auto loaded = provider->Load(*device, request);
            EXPECT_TRUE(loaded.Succeeded()) << loaded.diagnostic;
            return loaded.shader;
        };

        const RHIShaderRef vertexShader = loadShader(RHIShaderStage::Vertex, "VSMain");
        const RHIShaderRef pixelShader = loadShader(RHIShaderStage::Pixel, "PSMain");
        ASSERT_NE(vertexShader.Get(), nullptr);
        ASSERT_NE(pixelShader.Get(), nullptr);

        RHIDescriptorSetLayoutDesc setLayoutDesc;
        setLayoutDesc.AddBinding(0, RHIBindingType::SampledTexture, RHIShaderStage::Pixel);
        setLayoutDesc.debugName = "TransmissionDepthNativeSet";
        const RHIDescriptorSetLayoutRef setLayout = device->CreateDescriptorSetLayout(setLayoutDesc);
        ASSERT_NE(setLayout.Get(), nullptr);

        RHIPipelineLayoutDesc pipelineLayoutDesc;
        pipelineLayoutDesc.setLayouts.push_back(setLayout.Get());
        pipelineLayoutDesc.debugName = "TransmissionDepthNativeLayout";
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
        pipelineDesc.debugName = "TransmissionDepthNativePipeline";
        const RHIPipelineRef pipeline = device->CreateGraphicsPipeline(pipelineDesc);
        ASSERT_NE(pipeline.Get(), nullptr);

        constexpr uint32 width = 8;
        constexpr uint32 rowPitch = 256;
        constexpr float32 exitDepth = 0.5f;
        const float32 nan = std::numeric_limits<float32>::quiet_NaN();
        const std::array<float32, width> sampledDepths = reverseZ
            ? std::array<float32, width>{0.75f, 0.40f, 0.50f, 0.0f, nan, 0.499991f, 0.49998f, 1.1f}
            : std::array<float32, width>{0.25f, 0.60f, 0.50f, 1.0f, nan, 0.500009f, 0.50002f, -0.1f};
        const std::array<bool, width> expectedAccepted{false, true, false, true,
                                                        false, false, true, false};
        for (uint32 index = 0; index < width; ++index)
        {
            EXPECT_EQ(IsTransmissionDepthBehindExit(sampledDepths[index], exitDepth, reverseZ),
                      expectedAccepted[index]) << index;
        }

        RHITextureDesc inputDesc = RHITextureDesc::Texture2D(width, 1, RHIFormat::R32_FLOAT,
            RHITextureUsage::ShaderResource | RHITextureUsage::CopyDst);
        inputDesc.debugName = "TransmissionDepthNativeInput";
        const RHITextureRef input = device->CreateTexture(inputDesc);
        ASSERT_NE(input.Get(), nullptr);
        RHITextureViewDesc inputViewDesc;
        inputViewDesc.format = RHIFormat::R32_FLOAT;
        inputViewDesc.type = RHITextureViewType::ShaderResource;
        const RHITextureViewRef inputView = device->CreateTextureView(input.Get(), inputViewDesc);
        ASSERT_NE(inputView.Get(), nullptr);

        RHITextureDesc outputDesc = RHITextureDesc::Texture2D(width, 1, RHIFormat::RGBA32_FLOAT,
            RHITextureUsage::RenderTarget | RHITextureUsage::CopySrc);
        outputDesc.debugName = "TransmissionDepthNativeOutput";
        const RHITextureRef output = device->CreateTexture(outputDesc);
        ASSERT_NE(output.Get(), nullptr);
        RHITextureViewDesc outputViewDesc;
        outputViewDesc.format = RHIFormat::RGBA32_FLOAT;
        outputViewDesc.type = RHITextureViewType::RenderTarget;
        const RHITextureViewRef outputView = device->CreateTextureView(output.Get(), outputViewDesc);
        ASSERT_NE(outputView.Get(), nullptr);

        RHIBufferDesc uploadDesc;
        uploadDesc.size = rowPitch;
        uploadDesc.usage = RHIBufferUsage::CopySrc;
        uploadDesc.memoryType = RHIMemoryType::Upload;
        uploadDesc.debugName = "TransmissionDepthNativeUpload";
        const RHIBufferRef upload = device->CreateBuffer(uploadDesc);
        ASSERT_NE(upload.Get(), nullptr);
        void* uploadData = upload->Map();
        ASSERT_NE(uploadData, nullptr);
        std::memset(uploadData, 0, rowPitch);
        std::memcpy(uploadData, sampledDepths.data(), sizeof(sampledDepths));
        upload->Unmap();

        RHIDescriptorSetDesc descriptorSetDesc;
        descriptorSetDesc.layout = setLayout.Get();
        descriptorSetDesc.debugName = "TransmissionDepthNativeDescriptorSet";
        descriptorSetDesc.BindTexture(0, inputView.Get());
        const RHIDescriptorSetRef descriptorSet = device->CreateDescriptorSet(descriptorSetDesc);
        ASSERT_NE(descriptorSet.Get(), nullptr);

        RHIBufferDesc readbackDesc;
        readbackDesc.size = rowPitch;
        readbackDesc.usage = RHIBufferUsage::CopyDst;
        readbackDesc.memoryType = RHIMemoryType::Readback;
        readbackDesc.debugName = "TransmissionDepthNativeReadback";
        const RHIBufferRef readback = device->CreateBuffer(readbackDesc);
        ASSERT_NE(readback.Get(), nullptr);

        const auto ray = ResolveTransmissionRay({0.6f, 0.0f, -0.8f},
                                                {0.0f, 0.0f, 1.0f}, 1.5f, 0.1f);
        ASSERT_TRUE(ray.valid);
        Vec2 projectedUV;
        ASSERT_TRUE(ProjectTransmissionCandidate(Mat4Identity(),
            Vec3(0.0f, 0.0f, 0.5f) + ray.displacement, {8.0f, 1.0f}, projectedUV));

        const auto commands = device->CreateCommandContext(RHICommandQueueType::Graphics);
        const auto fence = device->CreateFence(0);
        ASSERT_NE(commands.Get(), nullptr);
        ASSERT_NE(fence.Get(), nullptr);
        commands->Begin();
        commands->TextureBarrier(input.Get(), RHIResourceState::Undefined, RHIResourceState::CopyDest);
        RHIBufferTextureCopyDesc uploadCopy;
        uploadCopy.bufferRowPitch = rowPitch;
        uploadCopy.textureRegion = {0, 0, width, 1};
        commands->CopyBufferToTexture(upload.Get(), input.Get(), uploadCopy);
        commands->TextureBarrier(input.Get(), RHIResourceState::CopyDest, RHIResourceState::ShaderResource);
        commands->TextureBarrier(output.Get(), RHIResourceState::Undefined, RHIResourceState::RenderTarget);
        RHIRenderPassDesc renderPass;
        renderPass.AddColorAttachment(outputView.Get(), RHILoadOp::Clear, RHIStoreOp::Store,
                                      {0.0f, 0.0f, 0.0f, 0.0f});
        commands->BeginRenderPass(renderPass);
        commands->SetViewport({0.0f, 0.0f, static_cast<float32>(width), 1.0f, 0.0f, 1.0f});
        commands->SetScissor({0, 0, width, 1});
        commands->SetPipeline(pipeline.Get());
        commands->SetDescriptorSet(0, descriptorSet.Get());
        commands->Draw(3, 1, 0, 0);
        commands->EndRenderPass();
        commands->TextureBarrier(output.Get(), RHIResourceState::RenderTarget, RHIResourceState::CopySource);
        commands->BufferBarrier(readback.Get(), RHIResourceState::Common, RHIResourceState::CopyDest);
        RHIBufferTextureCopyDesc readbackCopy;
        readbackCopy.bufferRowPitch = rowPitch;
        readbackCopy.textureRegion = {0, 0, width, 1};
        commands->CopyTextureToBuffer(output.Get(), readback.Get(), readbackCopy);
        commands->BufferBarrier(readback.Get(), RHIResourceState::CopyDest, RHIResourceState::Common);
        commands->End();

        const uint64 submitted = device->SubmitCommandContext(commands.Get(), fence.Get());
        ASSERT_NE(submitted, 0u);
        device->WaitForFence(fence.Get(), submitted);
        ASSERT_GE(fence->GetCompletedValue(), submitted);

        const auto* pixels = static_cast<const float32*>(readback->Map());
        ASSERT_NE(pixels, nullptr);
        for (uint32 index = 0; index < width; ++index)
        {
            const float32* pixel = pixels + index * 4;
            EXPECT_FLOAT_EQ(pixel[0], expectedAccepted[index] ? 1.0f : 0.0f) << index;
            EXPECT_NEAR(pixel[1], ray.pathLength, 1.0e-5f) << index;
            EXPECT_NEAR(pixel[2], projectedUV.x, 1.0e-5f) << index;
            EXPECT_NEAR(pixel[3], projectedUV.y, 1.0e-5f) << index;
        }
        readback->Unmap();

        const auto validation = device->GetNativeValidationDiagnostics();
        EXPECT_TRUE(validation.available);
        EXPECT_TRUE(validation.readComplete);
        EXPECT_EQ(validation.errorCount, 0u);
        EXPECT_EQ(validation.corruptionCount, 0u);
    }
} // namespace RVX::Test
