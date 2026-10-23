#include "Common/GpuTestUtils.h"
#include "Common/RenderGraphValidationAccess.h"
#include "Render/Graph/RenderGraph.h"
#include "Render/Passes/ParticleFeaturePass.h"
#include "Render/Passes/RenderPassRecordContext.h"
#include "RHI/RHI.h"
#include "RHI/RHICommandContext.h"
#include "ShaderContracts/ShaderArtifact.h"
#include "ShaderRuntime/CookedShaderProgramProvider.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <utility>

namespace
{
    namespace fs = std::filesystem;

    [[nodiscard]] float DecodeFloat16(std::uint16_t half) noexcept
    {
        const std::uint32_t sign = static_cast<std::uint32_t>(half & 0x8000u) << 16u;
        const std::uint32_t exponent = (half >> 10u) & 0x1fu;
        const std::uint32_t mantissa = half & 0x03ffu;

        std::uint32_t bits = 0u;
        if (exponent == 0u)
        {
            if (mantissa == 0u)
            {
                bits = sign;
            }
            else
            {
                std::uint32_t normalizedMantissa = mantissa;
                std::int32_t normalizedExponent = -14;
                while ((normalizedMantissa & 0x0400u) == 0u)
                {
                    normalizedMantissa <<= 1u;
                    --normalizedExponent;
                }
                normalizedMantissa &= 0x03ffu;
                bits = sign |
                    (static_cast<std::uint32_t>(normalizedExponent + 127) << 23u) |
                    (normalizedMantissa << 13u);
            }
        }
        else if (exponent == 31u)
        {
            bits = sign | 0x7f800000u | (mantissa << 13u);
        }
        else
        {
            bits = sign | ((exponent - 15u + 127u) << 23u) |
                (mantissa << 13u);
        }

        float value = 0.0f;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }

    [[nodiscard]] RVX::ParticleRenderSnapshot BuildReadyParticleSnapshot()
    {
        RVX::ParticleRenderSnapshot snapshot;
        snapshot.BeginBuild(17u);

        const auto addBatchItem = [&snapshot](
                                      RVX::uint64 instanceId,
                                      RVX::uint64 systemId,
                                      RVX::uint32 entitySlot,
                                      RVX::uint64 deterministicInstanceKey,
                                      RVX::ParticleRenderSnapshotBlendMode blendMode,
                                      const RVX::Vec3& position,
                                      const RVX::Vec4& color,
                                      bool distortion)
        {
            RVX::ParticleCpuPacket packet;
            packet.simulationTick = 17u;
            packet.deterministicSeed = 0xC001D00Du;
            packet.deterministicInstanceKey = deterministicInstanceKey;
            packet.effectiveSimulationSeed = packet.deterministicSeed ^
                static_cast<RVX::uint32>(deterministicInstanceKey);
            packet.particles = {
                {
                    .stableParticleId = 0u,
                    .position = position,
                    .lifetime = 2.0f,
                    .velocity = {0.0f, 0.0f, 0.0f},
                    .age = 0.25f,
                    .color = color,
                    .size = {0.32f, 0.32f},
                    .rotation = 0.0f,
                },
            };
            packet.contentHash = packet.ComputeContentHash();

            RVX::ParticleRenderSnapshotItem item;
            item.instanceId = instanceId;
            item.systemId = systemId;
            item.systemName = "GFX16 native particle billboard";
            item.header = {
                .channelId = RVX::RenderFeatureChannelId::Particle,
                .schemaVersion = RVX::ParticleRenderFeatureChannelDescriptor::SchemaVersion,
                .captureSequence = 17u,
                .configurationRevision = 1u,
                .source = {
                    .sceneRuntimeId = 0xA11CEu,
                    .entitySlot = entitySlot,
                    .entityGeneration = 3u,
                },
            };
            item.renderMode = RVX::ParticleRenderSnapshotMode::Billboard;
            item.blendMode = blendMode;
            item.simulationBackend =
                RVX::ParticleRenderSnapshotSimulationBackend::CPU;
            item.payloadStatus =
                RVX::ParticleRenderSnapshotPayloadStatus::CpuPacketReady;
            item.aliveParticleCount = static_cast<RVX::uint32>(packet.particles.size());
            item.maxParticleCount = 8u;
            item.visible = true;
            item.simulationSupported = true;
            item.sortingSupported = true;
            if (distortion)
            {
                item.effectDescriptor.distortionStrengthPixels = 2.0f;
                item.effectDescriptor.reactiveContribution = 0.75f;
                item.effectDescriptor.contentHash =
                    item.effectDescriptor.ComputeContentHash();
                packet.effectDescriptorHash = item.effectDescriptor.contentHash;
                packet.contentHash = packet.ComputeContentHash();
            }
            item.cpuPacket = std::move(packet);
            snapshot.items.push_back(std::move(item));
        };

        // Distinct world-space depths make the global transparent sort retain
        // one contiguous batch for each supported blend pipeline.
        addBatchItem(101u, 31u, 13u, 0xA11CE013u,
                     RVX::ParticleRenderSnapshotBlendMode::AlphaBlend,
                     {-0.45f, 0.0f, 0.20f}, {1.0f, 0.20f, 0.10f, 0.90f},
                     false);
        addBatchItem(102u, 32u, 14u, 0xA11CE014u,
                     RVX::ParticleRenderSnapshotBlendMode::Premultiplied,
                     {0.00f, 0.0f, 0.40f}, {0.10f, 0.80f, 0.15f, 0.85f},
                     false);
        addBatchItem(103u, 33u, 15u, 0xA11CE015u,
                     RVX::ParticleRenderSnapshotBlendMode::Additive,
                     {0.45f, 0.0f, 0.60f}, {0.15f, 0.25f, 1.00f, 0.80f},
                     true);
        snapshot.MarkComplete();
        return snapshot;
    }

    [[nodiscard]] fs::path GetCookedShaderRoot(RVX::RHIBackendType backend)
    {
        const char* environmentName = nullptr;
        switch (backend)
        {
            case RVX::RHIBackendType::DX12:
                environmentName = "RVX_PARTICLE_NATIVE_SHADER_ROOT_DX12";
                break;
            case RVX::RHIBackendType::Vulkan:
                environmentName = "RVX_PARTICLE_NATIVE_SHADER_ROOT_VULKAN";
                break;
            default: return {};
        }
        const char* const root = std::getenv(environmentName);
        return root != nullptr ? fs::path(root) : fs::path{};
    }

    void RunCookedParticleBillboardNativeCase(RVX::RHIBackendType backend)
    {
        constexpr RVX::uint32 kTargetExtent = 64u;
        constexpr RVX::uint32 kRgba16fBytesPerPixel = 8u;
        constexpr RVX::uint32 kReadbackRowPitch =
            kTargetExtent * kRgba16fBytesPerPixel;
        static_assert(kReadbackRowPitch % 256u == 0u,
                      "The native RGBA16F readback row pitch must satisfy DX12 alignment");

        const fs::path shaderRoot = GetCookedShaderRoot(backend);
        ASSERT_FALSE(shaderRoot.empty())
            << "The backend-specific cooked shader root is required for the native test";
        ASSERT_TRUE(fs::is_directory(shaderRoot))
            << "The cooked shader fixture did not stage its output: "
            << shaderRoot.generic_string();

        RVX::ShaderProgramRequest cookedPathRequest;
        cookedPathRequest.logicalAssetId = "Particle/ParticleBillboard.hlsl";
        cookedPathRequest.stage = RVX::RHIShaderStage::Vertex;
        cookedPathRequest.entryPoint = "VSMain";
        cookedPathRequest.backend = backend;
        cookedPathRequest.defines = {{
            "RVX_FULLSCREEN_UV_Y_MATCHES_CLIP_Y",
            backend == RVX::RHIBackendType::Vulkan ? "1" : "0"}};
        const fs::path artifactRelativePath =
            RVX::BuildShaderArtifactRelativePath(cookedPathRequest);
        const fs::path backendDirectoryName = artifactRelativePath.parent_path();
        ASSERT_FALSE(backendDirectoryName.empty());
        ASSERT_TRUE(shaderRoot.filename() != backendDirectoryName.filename())
            << "The cooked shader root must be the catalog root, not the "
            << "backend-specific artifact directory.";
        const fs::path expectedArtifactPath =
            shaderRoot / artifactRelativePath;
        ASSERT_TRUE(fs::is_regular_file(expectedArtifactPath))
            << "The cooked shader fixture must place a resolved particle "
            << "artifact directly below the catalog root: "
            << expectedArtifactPath.generic_string();
        ASSERT_TRUE(expectedArtifactPath.parent_path().parent_path() == shaderRoot);

        RVX::RHIDeviceDesc deviceDesc;
        deviceDesc.enableDebugLayer = true;
        auto device = RVX::Test::CreateNativeValidationDevice(backend, deviceDesc);
        ASSERT_NE(device.get(), nullptr)
            << RVX::ToString(backend)
            << " is required for suite.gfx16-particles; native device creation failed";
        ASSERT_EQ(device->GetBackendType(), backend);
        ASSERT_TRUE(RVX::Test::NativeAllowsSoftwareAdapter() ||
                    !RVX::Test::IsSoftwareAdapterName(
                        device->GetCapabilities().adapterName))
            << RVX::ToString(backend)
            << " resolved to a software adapter without explicit validation-host opt-in";

        auto shaderProvider = RVX::CreateCookedShaderProgramProviderFactory(
            shaderRoot)();
        ASSERT_NE(shaderProvider.get(), nullptr);

        RVX::ParticleRenderSnapshot snapshot = BuildReadyParticleSnapshot();
        ASSERT_TRUE(snapshot.metadata.complete);
        ASSERT_EQ(snapshot.metadata.status,
                  RVX::ParticleRenderSnapshotStatus::Complete);
        ASSERT_EQ(snapshot.items.size(), 3u);
        for (const RVX::ParticleRenderSnapshotItem& item : snapshot.items)
        {
            EXPECT_EQ(item.header.Validate(
                          RVX::ParticleRenderFeatureChannelDescriptor::ChannelId,
                          RVX::ParticleRenderFeatureChannelDescriptor::SchemaVersion),
                      RVX::RenderFeatureChannelValidationCode::Valid);
            ASSERT_TRUE(item.HasValidCpuPacket());
        }

        RVX::ParticleFeaturePass pass;
        pass.SetShaderProgramProvider(shaderProvider.get());
        pass.SetReverseZ(true);
        pass.OnAdd(device.get());
        pass.SetSnapshot(&snapshot);

        const RVX::RHINativeValidationDiagnostics nativeValidationBefore =
            device->GetNativeValidationDiagnostics();
        ASSERT_TRUE(nativeValidationBefore.available)
            << RVX::ToString(backend)
            << " native validation telemetry is required for the particle proof";
        ASSERT_TRUE(nativeValidationBefore.enabled)
            << RVX::ToString(backend)
            << " native validation must be enabled for the particle proof";
        ASSERT_TRUE(nativeValidationBefore.readComplete)
            << RVX::ToString(backend)
            << " native validation telemetry was incomplete before particle execution";

        RVX::RHITextureDesc colorDesc = RVX::RHITextureDesc::RenderTarget(
            kTargetExtent, kTargetExtent, RVX::RHIFormat::RGBA16_FLOAT);
        colorDesc.usage = colorDesc.usage | RVX::RHITextureUsage::CopySrc;
        colorDesc.SetOptimizedClearColor({0.0f, 0.0f, 0.0f, 0.0f});
        colorDesc.debugName = "GFX16ParticleNativeColor";
        RVX::RHITextureRef colorTarget = device->CreateTexture(colorDesc);
        ASSERT_NE(colorTarget.Get(), nullptr);
        RVX::RHITextureDesc depthDesc = RVX::RHITextureDesc::DepthStencil(
            kTargetExtent, kTargetExtent, RVX::RHIFormat::D32_FLOAT);
        depthDesc.SetOptimizedClearDepthStencil({0.0f, 0u});
        depthDesc.debugName = "GFX16ParticleNativeDepth";
        RVX::RHITextureRef depthTarget = device->CreateTexture(depthDesc);
        ASSERT_NE(depthTarget.Get(), nullptr);
        RVX::RHITextureDesc reactiveMaskDesc = RVX::RHITextureDesc::RenderTarget(
            kTargetExtent, kTargetExtent, RVX::RHIFormat::R8_UNORM);
        reactiveMaskDesc.SetOptimizedClearColor({0.0f, 0.0f, 0.0f, 0.0f});
        reactiveMaskDesc.debugName = "GFX17ParticleNativeReactiveMask";
        RVX::RHITextureRef reactiveMaskTarget = device->CreateTexture(reactiveMaskDesc);
        ASSERT_NE(reactiveMaskTarget.Get(), nullptr);

        RVX::RHITextureViewDesc colorClearViewDesc;
        colorClearViewDesc.type = RVX::RHITextureViewType::RenderTarget;
        colorClearViewDesc.format = RVX::RHIFormat::RGBA16_FLOAT;
        colorClearViewDesc.dimension = RVX::RHITextureDimension::Texture2D;
        colorClearViewDesc.subresourceRange = RVX::RHISubresourceRange::All();
        colorClearViewDesc.debugName = "GFX16ParticleNativeColorClearRTV";
        RVX::RHITextureViewRef colorClearView = device->CreateTextureView(
            colorTarget.Get(), colorClearViewDesc);
        ASSERT_NE(colorClearView.Get(), nullptr);

        RVX::RHITextureViewDesc depthClearViewDesc;
        depthClearViewDesc.type = RVX::RHITextureViewType::DepthStencil;
        depthClearViewDesc.format = RVX::RHIFormat::D32_FLOAT;
        depthClearViewDesc.dimension = RVX::RHITextureDimension::Texture2D;
        depthClearViewDesc.subresourceRange = RVX::RHISubresourceRange::All();
        depthClearViewDesc.subresourceRange.aspect = RVX::RHITextureAspect::Depth;
        depthClearViewDesc.debugName = "GFX16ParticleNativeDepthClearDSV";
        RVX::RHITextureViewRef depthClearView = device->CreateTextureView(
            depthTarget.Get(), depthClearViewDesc);
        ASSERT_NE(depthClearView.Get(), nullptr);

        RVX::RHITextureViewDesc reactiveMaskClearViewDesc;
        reactiveMaskClearViewDesc.type = RVX::RHITextureViewType::RenderTarget;
        reactiveMaskClearViewDesc.format = RVX::RHIFormat::R8_UNORM;
        reactiveMaskClearViewDesc.dimension = RVX::RHITextureDimension::Texture2D;
        reactiveMaskClearViewDesc.subresourceRange = RVX::RHISubresourceRange::All();
        reactiveMaskClearViewDesc.debugName = "GFX17ParticleNativeReactiveMaskClearRTV";
        RVX::RHITextureViewRef reactiveMaskClearView = device->CreateTextureView(
            reactiveMaskTarget.Get(), reactiveMaskClearViewDesc);
        ASSERT_NE(reactiveMaskClearView.Get(), nullptr);

        RVX::RHIBufferDesc readbackDesc;
        readbackDesc.size = static_cast<RVX::uint64>(kReadbackRowPitch) *
            kTargetExtent;
        readbackDesc.usage = RVX::RHIBufferUsage::CopyDst;
        readbackDesc.memoryType = RVX::RHIMemoryType::Readback;
        readbackDesc.debugName = "GFX16ParticleNativeColorReadback";
        RVX::RHIBufferRef readbackBuffer = device->CreateBuffer(readbackDesc);
        ASSERT_NE(readbackBuffer.Get(), nullptr);

        RVX::RenderGraph graph;
        RVX::RenderGraphValidationAccess::SetDevice(graph, device.get());

        RVX::ViewData view;
        view.frameNumber = 17u;
        view.viewportWidth = kTargetExtent;
        view.viewportHeight = kTargetExtent;
        view.aspectRatio = 1.0f;
        view.cameraPosition = {0.0f, 0.0f, 5.0f};
        view.cameraForward = {0.0f, 0.0f, -1.0f};
        view.inverseViewMatrix = RVX::Mat4Identity();
        view.jitteredViewProjectionMatrix = RVX::Mat4Identity();
        view.colorTarget = RVX::RenderGraphValidationAccess::ImportTexture(
            graph, colorTarget.Get(), RVX::RHIResourceState::RenderTarget);
        view.depthTarget = RVX::RenderGraphValidationAccess::ImportTexture(
            graph, depthTarget.Get(), RVX::RHIResourceState::DepthRead);
        view.reactiveMaskTarget = RVX::RenderGraphValidationAccess::ImportTexture(
            graph, reactiveMaskTarget.Get(), RVX::RHIResourceState::RenderTarget);
        ASSERT_TRUE(view.colorTarget.IsValid());
        ASSERT_TRUE(view.depthTarget.IsValid());
        ASSERT_TRUE(view.reactiveMaskTarget.IsValid());
        graph.SetExportState(view.colorTarget, RVX::RHIResourceState::RenderTarget);
        graph.SetExportState(view.depthTarget, RVX::RHIResourceState::DepthRead);
        graph.SetExportState(view.reactiveMaskTarget,
                             RVX::RHIResourceState::RenderTarget);

        RVX::RHITextureDesc distortionDesc = RVX::RHITextureDesc::RenderTarget(
            kTargetExtent, kTargetExtent, RVX::RHIFormat::RG16_FLOAT);
        distortionDesc.debugName = "GFX17ParticleNativeDistortion";
        const RVX::RGTextureHandle distortionVectors = graph.CreateTexture(
            distortionDesc);
        RVX::RHITextureDesc compositeDesc = RVX::RHITextureDesc::RenderTarget(
            kTargetExtent, kTargetExtent, RVX::RHIFormat::RGBA16_FLOAT);
        compositeDesc.usage = compositeDesc.usage | RVX::RHITextureUsage::CopySrc;
        compositeDesc.debugName = "GFX17ParticleNativeDistortionComposite";
        const RVX::RGTextureHandle compositedSceneColor = graph.CreateTexture(
            compositeDesc);
        ASSERT_TRUE(distortionVectors.IsValid());
        ASSERT_TRUE(compositedSceneColor.IsValid());
        ASSERT_NE(distortionVectors.index, view.colorTarget.index);
        ASSERT_NE(compositedSceneColor.index, view.colorTarget.index);
        graph.SetExportState(compositedSceneColor,
                             RVX::RHIResourceState::RenderTarget);

        RVX::RenderPassRecordContext particleContext =
            RVX::MakeRenderPassRecordContext(graph, view);
        particleContext.particleVfx = {
            .identity = particleContext.identity,
            .distortionVectors = distortionVectors,
            .compositedSceneColor = compositedSceneColor,
            .distortionRequested = true,
        };
        pass.AddToGraph(graph, particleContext);
        const RVX::ParticleFeaturePassStats preparedStats = pass.GetStats();
        ASSERT_TRUE(preparedStats.requested) << preparedStats.unsupportedReason;
        ASSERT_TRUE(preparedStats.supported) << preparedStats.unsupportedReason;
        ASSERT_TRUE(preparedStats.enabled) << preparedStats.unsupportedReason;
        ASSERT_TRUE(preparedStats.graphPassScheduled) << preparedStats.unsupportedReason;
        ASSERT_TRUE(pass.HasRegisteredDistortionComposite())
            << preparedStats.unsupportedReason;
        ASSERT_TRUE(preparedStats.distortionPassScheduled)
            << preparedStats.unsupportedReason;
        ASSERT_TRUE(preparedStats.uploadPublished) << preparedStats.unsupportedReason;
        ASSERT_EQ(preparedStats.instanceCount, 3u);
        ASSERT_GT(preparedStats.uploadBytes, 0u);
        ASSERT_EQ(preparedStats.fallbackCount, 0u);

        RVX::RenderGraphValidationAccess::Compile(graph);
        ASSERT_TRUE(graph.GetCompileStats().compileValid)
            << graph.ExportDiagnosticsText();
        const RVX::RenderGraph::Diagnostics compiledDiagnostics =
            graph.GetDiagnostics();
        const auto compiledParticlePass = std::find_if(
            compiledDiagnostics.passes.begin(), compiledDiagnostics.passes.end(),
            [](const RVX::RenderGraph::PassDiagnostic& diagnostic)
            {
                return diagnostic.name == "ParticleFeaturePass" && !diagnostic.culled;
            });
        ASSERT_NE(compiledParticlePass, compiledDiagnostics.passes.end())
            << graph.ExportDiagnosticsText();
        const auto depthReadUsage = std::find_if(
            compiledParticlePass->usages.begin(), compiledParticlePass->usages.end(),
            [&view](const RVX::RenderGraph::ResourceUsageDiagnostic& usage)
            {
                return usage.type == RVX::RenderGraph::DiagnosticResourceType::Texture &&
                       usage.access == RVX::RenderGraph::DiagnosticAccessType::Read &&
                       usage.resourceIndex == view.depthTarget.index &&
                       usage.desiredState == RVX::RHIResourceState::DepthRead;
            });
        ASSERT_NE(depthReadUsage, compiledParticlePass->usages.end())
            << "Particle billboard graph must declare its D32 depth read edge\n"
            << graph.ExportDiagnosticsText();
        for (const char* passName : {"ParticleDistortion",
                                     "ParticleDistortionComposite"})
        {
            const auto distortionPass = std::find_if(
                compiledDiagnostics.passes.begin(), compiledDiagnostics.passes.end(),
                [passName](const RVX::RenderGraph::PassDiagnostic& diagnostic)
                {
                    return diagnostic.name == passName && !diagnostic.culled;
                });
            ASSERT_NE(distortionPass, compiledDiagnostics.passes.end())
                << passName << " was not registered as part of the atomic particle distortion chain\n"
                << graph.ExportDiagnosticsText();
        }

        RVX::RHICommandContextRef commandContext = device->CreateCommandContext(
            RVX::RHICommandQueueType::Graphics);
        RVX::RHIFenceRef fence = device->CreateFence(0u);
        ASSERT_NE(commandContext.Get(), nullptr);
        ASSERT_NE(fence.Get(), nullptr);
        commandContext->Begin();

        // The graph pass deliberately uses Load on both attachments. Establish
        // a known black HDR surface and a reverse-Z far plane before it runs.
        commandContext->TextureBarrier(
            colorTarget.Get(), RVX::RHIResourceState::Undefined,
            RVX::RHIResourceState::RenderTarget);
        commandContext->TextureBarrier(
            depthTarget.Get(), RVX::RHIResourceState::Undefined,
            RVX::RHIResourceState::DepthWrite);
        commandContext->TextureBarrier(
            reactiveMaskTarget.Get(), RVX::RHIResourceState::Undefined,
            RVX::RHIResourceState::RenderTarget);
        RVX::RHIRenderPassDesc clearPass;
        clearPass.AddColorAttachment(colorClearView.Get(), RVX::RHILoadOp::Clear,
                                     RVX::RHIStoreOp::Store,
                                     {0.0f, 0.0f, 0.0f, 0.0f});
        clearPass.AddColorAttachment(reactiveMaskClearView.Get(), RVX::RHILoadOp::Clear,
                                     RVX::RHIStoreOp::Store,
                                     {0.0f, 0.0f, 0.0f, 0.0f});
        clearPass.SetDepthStencil(depthClearView.Get(), RVX::RHILoadOp::Clear,
                                  RVX::RHIStoreOp::Store, 0.0f, 0u);
        clearPass.SetRenderArea(0u, 0u, kTargetExtent, kTargetExtent);
        commandContext->BeginRenderPass(clearPass);
        commandContext->EndRenderPass();
        commandContext->TextureBarrier(
            depthTarget.Get(), RVX::RHIResourceState::DepthWrite,
            RVX::RHIResourceState::DepthRead);

        RVX::RenderGraphValidationAccess::Execute(graph, *commandContext);
        // Graph-owned transient resources become physical only during graph
        // execution. Resolve the actual composite target after that point,
        // while this command context still owns its terminal RenderTarget
        // state, then read back that output rather than the pre-composite HDR.
        RVX::RHITexture* const compositedSceneColorTexture =
            RVX::RenderGraphValidationAccess::GetTexture(
                graph, compositedSceneColor);
        ASSERT_NE(compositedSceneColorTexture, nullptr)
            << "Particle distortion composite must materialize its HDR output";
        ASSERT_EQ(compositedSceneColorTexture->GetFormat(),
                  RVX::RHIFormat::RGBA16_FLOAT);
        commandContext->TextureBarrier(
            compositedSceneColorTexture, RVX::RHIResourceState::RenderTarget,
            RVX::RHIResourceState::CopySource);
        commandContext->BufferBarrier(
            readbackBuffer.Get(), RVX::RHIResourceState::Common,
            RVX::RHIResourceState::CopyDest);
        RVX::RHIBufferTextureCopyDesc readbackCopy;
        readbackCopy.bufferRowPitch = kReadbackRowPitch;
        readbackCopy.bufferImageHeight = kTargetExtent;
        readbackCopy.textureRegion = {0, 0, kTargetExtent, kTargetExtent};
        commandContext->CopyTextureToBuffer(
            compositedSceneColorTexture, readbackBuffer.Get(), readbackCopy);
        commandContext->BufferBarrier(
            readbackBuffer.Get(), RVX::RHIResourceState::CopyDest,
            RVX::RHIResourceState::Common);
        commandContext->End();

        const RVX::uint64 submittedValue = device->SubmitCommandContext(
            commandContext.Get(), fence.Get());
        ASSERT_GT(submittedValue, 0u)
            << RVX::ToString(backend)
            << " did not accept the GFX16 particle graph submission";
        device->WaitForFence(fence.Get(), submittedValue);
        ASSERT_GE(fence->GetCompletedValue(), submittedValue);
        device->WaitIdle();

        const RVX::RHINativeValidationDiagnostics nativeValidationAfter =
            device->GetNativeValidationDiagnostics();
        EXPECT_TRUE(nativeValidationAfter.available);
        EXPECT_TRUE(nativeValidationAfter.enabled);
        EXPECT_TRUE(nativeValidationAfter.readComplete);
        EXPECT_EQ(nativeValidationAfter.errorCount,
                  nativeValidationBefore.errorCount)
            << RVX::ToString(backend)
            << " particle fallback texture or composite pipeline emitted a native error";
        EXPECT_EQ(nativeValidationAfter.corruptionCount,
                  nativeValidationBefore.corruptionCount)
            << RVX::ToString(backend)
            << " particle fallback texture or composite pipeline emitted native corruption";

        const auto* const readbackPixels = static_cast<const std::uint16_t*>(
            readbackBuffer->Map());
        ASSERT_NE(readbackPixels, nullptr)
            << RVX::ToString(backend)
            << " could not map the GFX16 HDR color readback";
        bool allComponentsFinite = true;
        bool foundVisibleParticlePixel = false;
        for (RVX::uint32 y = 0u; y < kTargetExtent; ++y)
        {
            for (RVX::uint32 x = 0u; x < kTargetExtent; ++x)
            {
                const size_t pixelOffset =
                    static_cast<size_t>(y) * kReadbackRowPitch / sizeof(std::uint16_t) +
                    static_cast<size_t>(x) * 4u;
                const float red = DecodeFloat16(readbackPixels[pixelOffset + 0u]);
                const float green = DecodeFloat16(readbackPixels[pixelOffset + 1u]);
                const float blue = DecodeFloat16(readbackPixels[pixelOffset + 2u]);
                const float alpha = DecodeFloat16(readbackPixels[pixelOffset + 3u]);
                allComponentsFinite = allComponentsFinite && std::isfinite(red) &&
                    std::isfinite(green) && std::isfinite(blue) && std::isfinite(alpha);
                foundVisibleParticlePixel = foundVisibleParticlePixel ||
                    (std::max({red, green, blue}) > 0.01f && alpha > 0.01f);
            }
        }
        readbackBuffer->Unmap();
        ASSERT_TRUE(allComponentsFinite)
            << RVX::ToString(backend)
            << " produced non-finite RGBA16F particle color output";
        ASSERT_TRUE(foundVisibleParticlePixel)
            << RVX::ToString(backend)
            << " submitted the particle distortion chain but did not produce a visible composite HDR pixel";

        const RVX::RenderGraph::Diagnostics executedDiagnostics =
            graph.GetDiagnostics();
        const auto executedParticlePass = std::find_if(
            executedDiagnostics.passes.begin(), executedDiagnostics.passes.end(),
            [](const RVX::RenderGraph::PassDiagnostic& diagnostic)
            {
                return diagnostic.name == "ParticleFeaturePass";
            });
        ASSERT_NE(executedParticlePass, executedDiagnostics.passes.end());
        EXPECT_TRUE(executedParticlePass->executedLastRun)
            << graph.ExportDiagnosticsText();
        EXPECT_TRUE(executedParticlePass->semanticExecutionSucceededLastRun)
            << executedParticlePass->semanticExecutionDiagnostic;

        const RVX::ParticleFeaturePassStats executedStats = pass.GetStats();
        EXPECT_TRUE(executedStats.requested);
        EXPECT_TRUE(executedStats.supported) << executedStats.unsupportedReason;
        EXPECT_TRUE(executedStats.enabled) << executedStats.unsupportedReason;
        EXPECT_TRUE(executedStats.graphPassScheduled);
        EXPECT_TRUE(executedStats.uploadPublished);
        EXPECT_TRUE(executedStats.reactiveMaskEnabled);
        EXPECT_TRUE(executedStats.distortionPassScheduled);
        EXPECT_TRUE(executedStats.distortionPassExecuted);
        EXPECT_TRUE(executedStats.distortionCompositeExecuted);
        EXPECT_TRUE(executedStats.drawSubmitted) << executedStats.unsupportedReason;
        EXPECT_EQ(executedStats.drawCallCount, 3u);
        EXPECT_EQ(executedStats.instanceCount, 3u);
        EXPECT_GT(executedStats.uploadBytes, 0u);
        EXPECT_EQ(executedStats.fallbackCount, 0u);
        EXPECT_EQ(executedStats.waitIdleCallCount, 0u);
        EXPECT_NE(executedStats.frameSlot, RVX::RVX_INVALID_INDEX);
        EXPECT_GT(executedStats.resourceCreationCount, 0u);
        EXPECT_GT(executedStats.pipelineCreationCount, 0u);

        pass.OnRemove();
    }
} // namespace

#if defined(RVX_GFX16_PARTICLE_DX12_ENABLED)
TEST(ParticleNativeValidation, DX12CookedBillboardsExecuteWithoutFallback)
{
    RunCookedParticleBillboardNativeCase(RVX::RHIBackendType::DX12);
}
#endif

#if defined(RVX_GFX16_PARTICLE_VULKAN_ENABLED)
TEST(ParticleNativeValidation, VulkanCookedBillboardsExecuteWithoutFallback)
{
    RunCookedParticleBillboardNativeCase(RVX::RHIBackendType::Vulkan);
}
#endif
