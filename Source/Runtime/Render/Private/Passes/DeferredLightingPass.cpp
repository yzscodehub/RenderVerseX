#include "Render/Passes/DeferredLightingPass.h"

#include "Render/Passes/RenderPassRecordContext.h"

#include "Render/Lighting/ClusteredLighting.h"
#include "Render/Lighting/LightManager.h"
#include "Render/PipelineCache.h"
#include "Render/Renderer/ViewData.h"

#include <algorithm>
#include <cstring>

namespace RVX
{
    namespace
    {
        constexpr uint64 RVX_DEFERRED_LIGHTING_CONSTANT_BUFFER_ALIGNMENT = 256;
        constexpr uint32 RVX_DEFERRED_LIGHTING_THREAD_GROUP_SIZE = 8;

        [[nodiscard]] bool IsCanonicalDeferredMaterialTableDesc(
            const RHIBufferDesc& desc) noexcept
        {
            return desc.size == sizeof(DeferredMaterialClassification) &&
                   desc.stride == sizeof(DeferredMaterialClassification) &&
                   desc.memoryType == RHIMemoryType::Upload &&
                   HasFlag(desc.usage, RHIBufferUsage::Structured) &&
                   HasFlag(desc.usage, RHIBufferUsage::ShaderResource);
        }

        [[nodiscard]] bool IsCanonicalDeferredMaterialTable(
            const RHIBuffer& buffer) noexcept
        {
            return buffer.GetSize() == sizeof(DeferredMaterialClassification) &&
                   buffer.GetStride() == sizeof(DeferredMaterialClassification) &&
                   buffer.GetMemoryType() == RHIMemoryType::Upload &&
                   HasFlag(buffer.GetUsage(), RHIBufferUsage::Structured) &&
                   HasFlag(buffer.GetUsage(), RHIBufferUsage::ShaderResource);
        }

        class RequiredPassExecutionGuard final
        {
        public:
            RequiredPassExecutionGuard(
                RenderGraphPassContext& context,
                std::string& failureDiagnostic,
                const char* defaultFailureDiagnostic) noexcept
                : m_context(context)
                , m_failureDiagnostic(failureDiagnostic)
                , m_defaultFailureDiagnostic(defaultFailureDiagnostic)
            {
            }

            ~RequiredPassExecutionGuard()
            {
                if (!m_succeeded)
                {
                    m_context.MarkExecutionFailed(
                        m_failureDiagnostic.empty()
                            ? m_defaultFailureDiagnostic
                            : m_failureDiagnostic.c_str());
                }
            }

            void MarkSucceeded() noexcept
            {
                m_succeeded = true;
            }

        private:
            RenderGraphPassContext& m_context;
            std::string& m_failureDiagnostic;
            const char* m_defaultFailureDiagnostic = nullptr;
            bool m_succeeded = false;
        };

        uint64 AlignDeferredLightingConstantBufferSize(uint64 size)
        {
            return (size + RVX_DEFERRED_LIGHTING_CONSTANT_BUFFER_ALIGNMENT - 1) &
                   ~(RVX_DEFERRED_LIGHTING_CONSTANT_BUFFER_ALIGNMENT - 1);
        }

        struct DeferredLightingGPUConstants
        {
            Mat4 inverseViewProjection{1.0f};
            float outputSizeInvSize[4] = {1.0f, 1.0f, 1.0f, 1.0f};
            // x = clear depth, y = comparison epsilon, z = reverse-Z flag.
            float depthBackgroundRule[4] = {1.0f, 0.000001f, 0.0f, 0.0f};
        };

        static_assert(sizeof(DeferredLightingGPUConstants) == 96);

        struct DeferredLightingGraphPassStats
        {
            bool inputsDeclared = false;
            bool resourceViewsDeclared = false;
            bool descriptorSetCreated = false;
            bool constantsUploaded = false;
            bool dispatchRecorded = false;
            uint32 dispatchGroupCountX = 0;
            uint32 dispatchGroupCountY = 0;
            uint64 dispatchPixelCount = 0;
        };

        /**
         * @brief Immutable native recording dependencies sealed before graph registration.
         *
         * The graph callback must not return to DeferredLightingPass once
         * recording starts: another frame may otherwise replace the pass's
         * cache/device pointers or diagnostic while this graph is executing.
         * Pipeline and layout are held strongly here; the engine owns the
         * device and PipelineCache for the complete graph submission lifetime.
         */
        struct DeferredLightingExecutionResources
        {
            PipelineCache* pipelineCache = nullptr;
            IRHIDevice* device = nullptr;
            RHIPipelineRef pipeline;
            RHIDescriptorSetLayoutRef descriptorSetLayout;
            float depthClearValue = 1.0f;
            bool reverseZ = false;
            ViewData view{};

            [[nodiscard]] bool IsComplete() const noexcept
            {
                return pipelineCache != nullptr && device != nullptr &&
                       pipeline && descriptorSetLayout;
            }
        };

        struct DeferredLightingGraphPassData
        {
            RGTextureHandle gBuffer0{};
            RGTextureHandle gBuffer1{};
            RGTextureHandle gBuffer2{};
            RGTextureHandle gBuffer3{};
            RGTextureHandle deviceDepth{};
            RGTextureHandle hdrSceneColor{};
            RGBufferHandle materialTable{};
            DeferredLightingFrameResources lighting{};
            RGTextureViewHandle gBuffer0View{};
            RGTextureViewHandle gBuffer1View{};
            RGTextureViewHandle gBuffer2View{};
            RGTextureViewHandle gBuffer3View{};
            RGTextureViewHandle deviceDepthView{};
            RGTextureViewHandle hdrSceneColorView{};
            RGTextureViewHandle directionalShadowView{};
            RGTextureViewHandle irradianceView{};
            RGTextureViewHandle prefilteredEnvironmentView{};
            RGTextureViewHandle brdfLUTView{};
            DeferredLightingExecutionResources execution{};
            // Execution receives const pass data. Keeping these mutable makes
            // diagnostics and evidence belong to this graph pass instance,
            // never to the feature object that may already be recording a
            // later frame.
            mutable DeferredLightingGraphPassStats stats{};
            mutable std::string executionDiagnostic{};
            bool inputsValid = false;
        };

        bool CreateAndUploadDeferredLightingConstants(
            IRHIDevice& device,
            const ViewData& view,
            float depthClearValue,
            bool reverseZ,
            uint32 width,
            uint32 height,
            RHIBufferRef& outConstantBuffer,
            std::string& outError)
        {
            const float outputWidth = static_cast<float>(std::max(1u, width));
            const float outputHeight = static_cast<float>(std::max(1u, height));
            DeferredLightingGPUConstants constants;
            constants.inverseViewProjection =
                view.inverseJitteredViewProjectionMatrix;
            constants.outputSizeInvSize[0] = outputWidth;
            constants.outputSizeInvSize[1] = outputHeight;
            constants.outputSizeInvSize[2] = 1.0f / outputWidth;
            constants.outputSizeInvSize[3] = 1.0f / outputHeight;
            constants.depthBackgroundRule[0] = depthClearValue;
            constants.depthBackgroundRule[1] = 0.000001f;
            constants.depthBackgroundRule[2] = reverseZ ? 1.0f : 0.0f;

            RHIBufferDesc bufferDesc;
            bufferDesc.size = AlignDeferredLightingConstantBufferSize(
                sizeof(DeferredLightingGPUConstants));
            bufferDesc.usage = RHIBufferUsage::Constant;
            bufferDesc.memoryType = RHIMemoryType::Upload;
            bufferDesc.debugName = "DeferredLightingFrameConstants";
            outConstantBuffer = device.CreateBuffer(bufferDesc);
            if (!outConstantBuffer)
            {
                outError =
                    "Deferred lighting could not allocate its frame constant buffer";
                return false;
            }

            void* mapped = outConstantBuffer->Map();
            if (mapped == nullptr)
            {
                outError =
                    "Deferred lighting could not map its frame constant buffer";
                outConstantBuffer.Reset();
                return false;
            }
            std::memcpy(mapped, &constants, sizeof(constants));
            outConstantBuffer->Unmap();
            return true;
        }

        bool MatchesExtentAndSamples(const RHITextureDesc& candidate,
                                     const RHITextureDesc& reference) noexcept
        {
            return candidate.width == reference.width &&
                   candidate.height == reference.height &&
                   candidate.depth == reference.depth &&
                   candidate.sampleCount == reference.sampleCount;
        }

        RGTextureViewHandle DeclareComputeSRV(RenderGraphBuilder& builder,
                                              RGTextureHandle texture,
                                              RHITextureAspect aspect,
                                              const char* debugName)
        {
            const RHITextureDesc* desc = builder.GetTextureDesc(texture);
            if (desc == nullptr)
            {
                return {};
            }

            RHITextureViewDesc viewDesc;
            viewDesc.format = desc->format;
            viewDesc.dimension = desc->dimension;
            viewDesc.subresourceRange = RHISubresourceRange::All();
            viewDesc.subresourceRange.aspect = aspect;
            viewDesc.type = RHITextureViewType::ShaderResource;
            viewDesc.debugName = debugName;
            return builder.Read(
                builder.CreateTextureView(texture, viewDesc),
                MakeRGAccessDesc(
                    RHIResourceState::ShaderResource,
                    RHIShaderStage::Compute));
        }

        RGTextureViewHandle DeclareExactComputeIBLSRV(
            RenderGraphBuilder& builder,
            RGTextureHandle texture,
            const RHITextureViewRef& retainedView,
            const char* debugName)
        {
            const RHITextureDesc* const textureDesc =
                builder.GetTextureDesc(texture);
            if (!texture.IsValid() || textureDesc == nullptr || !retainedView ||
                retainedView->GetTexture() == nullptr)
            {
                return {};
            }

            RHITextureViewDesc viewDesc;
            viewDesc.format = retainedView->GetFormat();
            viewDesc.dimension = textureDesc->dimension;
            viewDesc.subresourceRange = retainedView->GetSubresourceRange();
            viewDesc.type = RHITextureViewType::ShaderResource;
            viewDesc.debugName = debugName;
            return builder.Read(
                builder.CreateTextureView(texture, viewDesc),
                MakeRGAccessDesc(
                    RHIResourceState::ShaderResource,
                    RHIShaderStage::Compute));
        }

        RGTextureViewHandle DeclareComputeUAV(RenderGraphBuilder& builder,
                                              RGTextureHandle texture,
                                              const char* debugName)
        {
            const RHITextureDesc* desc = builder.GetTextureDesc(texture);
            if (desc == nullptr)
            {
                return {};
            }

            RHITextureViewDesc viewDesc;
            viewDesc.format = desc->format;
            viewDesc.dimension = desc->dimension;
            viewDesc.subresourceRange = RHISubresourceRange::All();
            viewDesc.type = RHITextureViewType::UnorderedAccess;
            viewDesc.debugName = debugName;
            return builder.Write(
                builder.CreateTextureView(texture, viewDesc),
                MakeRGAccessDesc(
                    RHIResourceState::UnorderedAccess,
                    RHIShaderStage::Compute,
                    RHIDiscardIntent::Discard));
        }
    } // namespace

    bool CreateDeferredStandardLitMaterialTable(
        IRHIDevice& device,
        RHIBufferRef& outTable,
        std::string& outError)
    {
        outTable.Reset();
        outError.clear();

        RHIBufferDesc desc;
        desc.size = sizeof(DeferredMaterialClassification);
        desc.stride = sizeof(DeferredMaterialClassification);
        desc.usage = RHIBufferUsage::Structured |
            RHIBufferUsage::ShaderResource;
        desc.memoryType = RHIMemoryType::Upload;
        desc.debugName = "DeferredStandardLitMaterialTable";

        RHIBufferRef table = device.CreateBuffer(desc);
        if (!table || !IsCanonicalDeferredMaterialTable(*table))
        {
            outError =
                "Deferred lighting could not allocate its canonical StandardLit material table";
            return false;
        }

        RHIMappedWriteAccess access = table->MapWriteRange(
            0, sizeof(DeferredMaterialClassification));
        if (!access.IsValid())
        {
            outError =
                "Deferred lighting could not map its canonical StandardLit material table";
            return false;
        }

        const DeferredMaterialClassification standardLit{};
        std::memcpy(access.GetData(), &standardLit, sizeof(standardLit));
        if (!table->CommitMappedWriteRange(std::move(access)).IsPublished())
        {
            outError =
                "Deferred lighting could not publish its canonical StandardLit material table";
            return false;
        }

        outTable = std::move(table);
        return true;
    }

    void DeferredLightingPass::SetPipelineCache(PipelineCache* pipelineCache)
    {
        IRHIDevice* device = pipelineCache ? pipelineCache->GetDevice() : nullptr;
        if (m_device != device)
        {
            m_device = device;
        }
        m_pipelineCache = pipelineCache;
    }

    bool DeferredLightingPass::EnsurePipelineReady()
    {
        if (m_pipelineCache == nullptr || !m_pipelineCache->IsInitialized())
        {
            m_unsupportedReason =
                "Deferred lighting requires an initialized PipelineCache";
            return false;
        }

        m_device = m_pipelineCache->GetDevice();
        if (m_device == nullptr)
        {
            m_unsupportedReason = "Deferred lighting requires an RHI device";
            return false;
        }

        const RHIBackendType backend = m_device->GetBackendType();
        if (backend != RHIBackendType::DX12 && backend != RHIBackendType::Vulkan &&
            backend != RHIBackendType::Metal)
        {
            m_unsupportedReason =
                "Deferred lighting is unsupported on this compatibility backend";
            return false;
        }
        if (!m_device->GetCapabilities().supportsComputePipeline)
        {
            m_unsupportedReason =
                "Deferred lighting requires native compute-pipeline support";
            return false;
        }

        if (m_pipelineCache->GetDeferredLightingPipeline() == nullptr ||
            m_pipelineCache->GetDeferredLightingSetLayout() == nullptr)
        {
            m_unsupportedReason = m_pipelineCache->GetLastError();
            if (m_unsupportedReason.empty())
            {
                m_unsupportedReason =
                    "Deferred-lighting compute pipeline resources are unavailable";
            }
            return false;
        }

        m_unsupportedReason.clear();
        return true;
    }

    bool DeferredLightingPass::IsSupported() const
    {
        return m_pipelineCache != nullptr &&
               m_pipelineCache->IsDeferredLightingPipelineReady() &&
               m_unsupportedReason.empty();
    }

    bool DeferredLightingPass::AddToGraph(
        RenderFeatureGraphBuilder& featureBuilder,
        const ViewData& view,
        const DeferredLightingInputs& inputs)
    {
        if (!featureBuilder.IsValid() || !inputs.IsComplete())
        {
            m_unsupportedReason =
                "Deferred lighting requires a valid graph and complete frame inputs";
            return false;
        }
        if (!EnsurePipelineReady())
        {
            return false;
        }
        if ((view.directionalShadowEnabled != 0) !=
                inputs.lighting.directionalShadowEnabled ||
            (view.textureIBLEnabled != 0) != inputs.lighting.textureIBLEnabled)
        {
            m_unsupportedReason =
                "Deferred lighting frame-resource readiness disagrees with ViewData";
            return false;
        }

        // Seal every mutable pass dependency before graph registration. The
        // execution callback below deliberately captures nothing and only
        // consumes this value-owned record state.
        DeferredLightingExecutionResources execution;
        execution.pipelineCache = m_pipelineCache;
        execution.device = m_device;
        execution.pipeline = RHIPipelineRef(
            m_pipelineCache->GetDeferredLightingPipeline());
        execution.descriptorSetLayout = RHIDescriptorSetLayoutRef(
            m_pipelineCache->GetDeferredLightingSetLayout());
        execution.depthClearValue = m_pipelineCache->GetDepthClearValue();
        execution.reverseZ = m_pipelineCache->IsReverseZ();
        execution.view = view;
        if (!execution.IsComplete())
        {
            m_unsupportedReason =
                "Deferred lighting could not seal its native recording dependencies";
            return false;
        }

        const bool registered = featureBuilder.AddPass<DeferredLightingGraphPassData>(
            "DeferredLightingPass",
            RenderGraphPassType::Compute,
            [inputs, execution](RenderGraphBuilder& builder,
                                DeferredLightingGraphPassData& data)
            {
                data.gBuffer0 = inputs.gBuffer0;
                data.gBuffer1 = inputs.gBuffer1;
                data.gBuffer2 = inputs.gBuffer2;
                data.gBuffer3 = inputs.gBuffer3;
                data.deviceDepth = inputs.deviceDepth;
                data.hdrSceneColor = inputs.hdrSceneColor;
                data.materialTable = inputs.materialTable;
                data.lighting = inputs.lighting;
                data.execution = execution;

                const RHITextureDesc* gBuffer0Desc =
                    builder.GetTextureDesc(data.gBuffer0);
                const RHITextureDesc* gBuffer1Desc =
                    builder.GetTextureDesc(data.gBuffer1);
                const RHITextureDesc* gBuffer2Desc =
                    builder.GetTextureDesc(data.gBuffer2);
                const RHITextureDesc* gBuffer3Desc =
                    builder.GetTextureDesc(data.gBuffer3);
                const RHITextureDesc* depthDesc =
                    builder.GetTextureDesc(data.deviceDepth);
                const RHITextureDesc* hdrDesc =
                    builder.GetTextureDesc(data.hdrSceneColor);
                const RHIBufferDesc* materialTableDesc =
                    builder.GetBufferDesc(data.materialTable);
                if (gBuffer0Desc == nullptr || gBuffer1Desc == nullptr ||
                    gBuffer2Desc == nullptr || gBuffer3Desc == nullptr ||
                    depthDesc == nullptr || hdrDesc == nullptr ||
                    gBuffer0Desc->format != RHIFormat::RGBA8_UNORM_SRGB ||
                    gBuffer1Desc->format != RHIFormat::RGBA8_UNORM ||
                    gBuffer2Desc->format != RHIFormat::RGBA16_FLOAT ||
                    gBuffer3Desc->format != RHIFormat::R32_UINT ||
                    depthDesc->format != RHIFormat::D32_FLOAT ||
                    hdrDesc->format != RHIFormat::RGBA16_FLOAT ||
                    !HasFlag(hdrDesc->usage, RHITextureUsage::UnorderedAccess) ||
                    !MatchesExtentAndSamples(*gBuffer1Desc, *gBuffer0Desc) ||
                    !MatchesExtentAndSamples(*gBuffer2Desc, *gBuffer0Desc) ||
                    !MatchesExtentAndSamples(*gBuffer3Desc, *gBuffer0Desc) ||
                    !MatchesExtentAndSamples(*depthDesc, *gBuffer0Desc) ||
                    !MatchesExtentAndSamples(*hdrDesc, *gBuffer0Desc) ||
                    gBuffer0Desc->sampleCount != RHISampleCount::Count1)
                {
                    return;
                }

                data.gBuffer0View = DeclareComputeSRV(
                    builder, data.gBuffer0, RHITextureAspect::Color,
                    "DeferredLightingGBuffer0SRV");
                data.gBuffer1View = DeclareComputeSRV(
                    builder, data.gBuffer1, RHITextureAspect::Color,
                    "DeferredLightingGBuffer1SRV");
                data.gBuffer2View = DeclareComputeSRV(
                    builder, data.gBuffer2, RHITextureAspect::Color,
                    "DeferredLightingGBuffer2SRV");
                data.gBuffer3View = DeclareComputeSRV(
                    builder, data.gBuffer3, RHITextureAspect::Color,
                    "DeferredLightingGBuffer3SRV");
                data.deviceDepthView = DeclareComputeSRV(
                    builder, data.deviceDepth, RHITextureAspect::Depth,
                    "DeferredLightingDepthSRV");
                data.hdrSceneColorView = DeclareComputeUAV(
                    builder, data.hdrSceneColor,
                    "DeferredLightingHDRSceneColorUAV");
                data.lighting.lightConstants = builder.Read(
                    data.lighting.lightConstants,
                    MakeRGAccessDesc(
                        RHIResourceState::ConstantBuffer,
                        RHIShaderStage::Compute));
                data.lighting.pointLights = builder.Read(
                    data.lighting.pointLights,
                    MakeRGAccessDesc(
                        RHIResourceState::ShaderResource,
                        RHIShaderStage::Compute));
                data.lighting.spotLights = builder.Read(
                    data.lighting.spotLights,
                    MakeRGAccessDesc(
                        RHIResourceState::ShaderResource,
                        RHIShaderStage::Compute));
                data.lighting.clusterConstants = builder.Read(
                    data.lighting.clusterConstants,
                    MakeRGAccessDesc(
                        RHIResourceState::ConstantBuffer,
                        RHIShaderStage::Compute));
                data.lighting.clusterData = builder.Read(
                    data.lighting.clusterData,
                    MakeRGAccessDesc(
                        RHIResourceState::ShaderResource,
                        RHIShaderStage::Compute));
                data.lighting.clusterLightIndices = builder.Read(
                    data.lighting.clusterLightIndices,
                    MakeRGAccessDesc(
                        RHIResourceState::ShaderResource,
                        RHIShaderStage::Compute));
                if (data.lighting.directionalShadowEnabled)
                {
                    data.directionalShadowView = DeclareComputeSRV(
                        builder, data.lighting.directionalShadowMap,
                        RHITextureAspect::Depth,
                        "DeferredLightingDirectionalShadowSRV");
                }
                if (data.lighting.textureIBLEnabled)
                {
                    data.irradianceView = DeclareExactComputeIBLSRV(
                        builder, data.lighting.irradiance,
                        data.lighting.expectedIrradianceView,
                        "DeferredLightingIrradianceSRV");
                    data.prefilteredEnvironmentView = DeclareExactComputeIBLSRV(
                        builder, data.lighting.prefilteredEnvironment,
                        data.lighting.expectedPrefilteredEnvironmentView,
                        "DeferredLightingPrefilteredEnvironmentSRV");
                    data.brdfLUTView = DeclareExactComputeIBLSRV(
                        builder, data.lighting.brdfLUT,
                        data.lighting.expectedBRDFLUTView,
                        "DeferredLightingBRDFLUTSRV");
                }
                if (materialTableDesc != nullptr &&
                    IsCanonicalDeferredMaterialTableDesc(*materialTableDesc))
                {
                    data.materialTable = builder.Read(
                        data.materialTable,
                        MakeRGAccessDesc(
                            RHIResourceState::ShaderResource,
                            RHIShaderStage::Compute));
                }
                data.inputsValid = data.gBuffer0View.IsValid() &&
                    data.gBuffer1View.IsValid() && data.gBuffer2View.IsValid() &&
                    data.gBuffer3View.IsValid() && data.deviceDepthView.IsValid() &&
                    data.hdrSceneColorView.IsValid() &&
                    data.materialTable.IsValid() &&
                    data.lighting.lightConstants.IsValid() &&
                    data.lighting.pointLights.IsValid() &&
                    data.lighting.spotLights.IsValid() &&
                    data.lighting.clusterConstants.IsValid() &&
                    data.lighting.clusterData.IsValid() &&
                    data.lighting.clusterLightIndices.IsValid() &&
                    (!data.lighting.directionalShadowEnabled ||
                     data.directionalShadowView.IsValid()) &&
                    (!data.lighting.textureIBLEnabled ||
                     (data.irradianceView.IsValid() &&
                      data.prefilteredEnvironmentView.IsValid() &&
                      data.brdfLUTView.IsValid())) &&
                    materialTableDesc != nullptr &&
                    IsCanonicalDeferredMaterialTableDesc(*materialTableDesc);
            },
            [](const DeferredLightingGraphPassData& data,
               RenderGraphPassContext& context)
            {
                RequiredPassExecutionGuard executionGuard(
                    context,
                    data.executionDiagnostic,
                    "DeferredLighting did not record its required compute dispatch");
                data.stats.inputsDeclared = data.inputsValid;
                data.stats.resourceViewsDeclared = data.gBuffer0View.IsValid() &&
                    data.gBuffer1View.IsValid() && data.gBuffer2View.IsValid() &&
                    data.gBuffer3View.IsValid() && data.deviceDepthView.IsValid() &&
                    data.hdrSceneColorView.IsValid() &&
                    data.materialTable.IsValid() &&
                    data.lighting.lightConstants.IsValid() &&
                    data.lighting.pointLights.IsValid() &&
                    data.lighting.spotLights.IsValid() &&
                    data.lighting.clusterConstants.IsValid() &&
                    data.lighting.clusterData.IsValid() &&
                    data.lighting.clusterLightIndices.IsValid() &&
                    (!data.lighting.directionalShadowEnabled ||
                     data.directionalShadowView.IsValid()) &&
                    (!data.lighting.textureIBLEnabled ||
                     (data.irradianceView.IsValid() &&
                      data.prefilteredEnvironmentView.IsValid() &&
                      data.brdfLUTView.IsValid()));

                if (!data.inputsValid || !data.execution.IsComplete())
                {
                    data.executionDiagnostic =
                        "Deferred lighting graph inputs or sealed native dependencies are invalid";
                    return;
                }

                RHITexture* gBuffer0 = context.GetTexture(data.gBuffer0);
                RHITexture* gBuffer1 = context.GetTexture(data.gBuffer1);
                RHITexture* gBuffer2 = context.GetTexture(data.gBuffer2);
                RHITexture* gBuffer3 = context.GetTexture(data.gBuffer3);
                RHITexture* depth = context.GetTexture(data.deviceDepth);
                RHITexture* hdrSceneColor = context.GetTexture(data.hdrSceneColor);
                RHIBuffer* materialTable = context.GetBuffer(data.materialTable);
                RHIBuffer* lightConstants =
                    context.GetBuffer(data.lighting.lightConstants);
                RHIBuffer* pointLights = context.GetBuffer(data.lighting.pointLights);
                RHIBuffer* spotLights = context.GetBuffer(data.lighting.spotLights);
                RHIBuffer* clusterConstants =
                    context.GetBuffer(data.lighting.clusterConstants);
                RHIBuffer* clusterData = context.GetBuffer(data.lighting.clusterData);
                RHIBuffer* clusterLightIndices =
                    context.GetBuffer(data.lighting.clusterLightIndices);
                if (gBuffer0 == nullptr || gBuffer1 == nullptr ||
                    gBuffer2 == nullptr || gBuffer3 == nullptr ||
                    depth == nullptr || hdrSceneColor == nullptr ||
                    lightConstants == nullptr || pointLights == nullptr ||
                    spotLights == nullptr || clusterConstants == nullptr ||
                    clusterData == nullptr || clusterLightIndices == nullptr)
                {
                    data.executionDiagnostic =
                        "Deferred lighting could not resolve graph-owned resources";
                    return;
                }
                const FrameLightResources frozenLightResources =
                    data.lighting.frozenLightResources.Borrow();
                if (lightConstants != frozenLightResources.lightConstantsBuffer ||
                    pointLights != frozenLightResources.pointLightsBuffer ||
                    spotLights != frozenLightResources.spotLightsBuffer ||
                    clusterConstants != frozenLightResources.clusterConstantsBuffer ||
                    clusterData != frozenLightResources.clusterBuffer ||
                    clusterLightIndices !=
                        frozenLightResources.clusterLightIndexBuffer)
                {
                    data.executionDiagnostic =
                        "Deferred lighting graph imports do not match its frozen LightingFramePlan buffers";
                    return;
                }
                if (materialTable == nullptr ||
                    !IsCanonicalDeferredMaterialTable(*materialTable))
                {
                    data.executionDiagnostic =
                        "Deferred lighting received an invalid StandardLit material table";
                    return;
                }

                RHITextureView* gBuffer0View =
                    context.GetTextureView(data.gBuffer0View);
                RHITextureView* gBuffer1View =
                    context.GetTextureView(data.gBuffer1View);
                RHITextureView* gBuffer2View =
                    context.GetTextureView(data.gBuffer2View);
                RHITextureView* gBuffer3View =
                    context.GetTextureView(data.gBuffer3View);
                RHITextureView* depthView =
                    context.GetTextureView(data.deviceDepthView);
                RHITextureView* hdrSceneColorView =
                    context.GetTextureView(data.hdrSceneColorView);
                RHITextureView* directionalShadowView =
                    data.lighting.directionalShadowEnabled
                        ? context.GetTextureView(data.directionalShadowView)
                        : nullptr;
                RHITextureView* irradianceView =
                    data.lighting.textureIBLEnabled
                        ? context.GetTextureView(data.irradianceView)
                        : nullptr;
                RHITextureView* prefilteredEnvironmentView =
                    data.lighting.textureIBLEnabled
                        ? context.GetTextureView(data.prefilteredEnvironmentView)
                        : nullptr;
                RHITextureView* brdfLUTView =
                    data.lighting.textureIBLEnabled
                        ? context.GetTextureView(data.brdfLUTView)
                        : nullptr;
                if (gBuffer0View == nullptr || gBuffer1View == nullptr ||
                    gBuffer2View == nullptr || gBuffer3View == nullptr ||
                    depthView == nullptr || hdrSceneColorView == nullptr)
                {
                    data.executionDiagnostic =
                        "Deferred lighting could not resolve graph-owned texture views";
                    return;
                }
                if ((data.lighting.directionalShadowEnabled &&
                     directionalShadowView == nullptr) ||
                    (data.lighting.textureIBLEnabled &&
                     (irradianceView == nullptr ||
                      prefilteredEnvironmentView == nullptr ||
                      brdfLUTView == nullptr)))
                {
                    data.executionDiagnostic =
                        "Deferred lighting could not resolve required LightingFramePlan views";
                    return;
                }

                const auto isExactIBLView = [](
                    RHITextureView* graphView,
                    const RHITextureViewRef& expectedView) noexcept
                {
                    if (graphView == nullptr || !expectedView ||
                        graphView->GetTexture() != expectedView->GetTexture() ||
                        graphView->GetFormat() != expectedView->GetFormat())
                    {
                        return false;
                    }
                    const RHISubresourceRange& graphRange =
                        graphView->GetSubresourceRange();
                    const RHISubresourceRange& expectedRange =
                        expectedView->GetSubresourceRange();
                    return graphRange.baseMipLevel == expectedRange.baseMipLevel &&
                        graphRange.mipLevelCount == expectedRange.mipLevelCount &&
                        graphRange.baseArrayLayer == expectedRange.baseArrayLayer &&
                        graphRange.arrayLayerCount == expectedRange.arrayLayerCount &&
                        graphRange.aspect == expectedRange.aspect;
                };
                const bool exactIBLViews = !data.lighting.textureIBLEnabled ||
                    (isExactIBLView(
                         irradianceView,
                         data.lighting.expectedIrradianceView) &&
                     isExactIBLView(
                         prefilteredEnvironmentView,
                         data.lighting.expectedPrefilteredEnvironmentView) &&
                     isExactIBLView(
                         brdfLUTView,
                         data.lighting.expectedBRDFLUTView));
                if (!exactIBLViews)
                {
                    data.executionDiagnostic =
                        "Deferred lighting graph IBL views do not match its retained LightingFramePlan allocations";
                    return;
                }

                LightingBindingSnapshotRequest snapshotRequest;
                snapshotRequest.lightingPlanIdentity =
                    data.lighting.lightingPlanIdentity;
                snapshotRequest.frozenLightResources =
                    data.lighting.frozenLightResources;
                snapshotRequest.directionalShadow.enabled =
                    data.lighting.directionalShadowEnabled;
                snapshotRequest.directionalShadow.shadowMapView =
                    directionalShadowView;
                // GFX08 deferred lighting does not consume RT shadow masks.
                // It must therefore only be selected for views that do not
                // request that optional path; no fallback can hide a mismatch.
                snapshotRequest.rayTracedShadow.enabled = false;
                snapshotRequest.environmentIBL.enabled =
                    data.lighting.textureIBLEnabled;
                snapshotRequest.environmentIBL.irradianceView = irradianceView;
                snapshotRequest.environmentIBL.prefilteredEnvironmentView =
                    prefilteredEnvironmentView;
                snapshotRequest.environmentIBL.brdfLUTView = brdfLUTView;

                LightingBindingSnapshot lightingSnapshot;
                if (data.execution.view.rayTracedShadowEnabled != 0 ||
                    !data.execution.pipelineCache->CreateLightingBindingSnapshot(
                        data.execution.view, snapshotRequest, lightingSnapshot) ||
                    !lightingSnapshot.IsValid() ||
                    !lightingSnapshot.MatchesLightingPlan(
                        data.lighting.lightingPlanIdentity))
                {
                    data.executionDiagnostic =
                        "Deferred lighting could not seal its record-owned LightingFramePlan snapshot";
                    return;
                }

                if (!data.execution.pipeline || !data.execution.descriptorSetLayout)
                {
                    data.executionDiagnostic =
                        "Deferred lighting lost its sealed native pipeline or descriptor layout";
                    return;
                }

                RHIBufferRef constantBuffer;
                if (!CreateAndUploadDeferredLightingConstants(
                        *data.execution.device, data.execution.view,
                        data.execution.depthClearValue, data.execution.reverseZ,
                        hdrSceneColor->GetWidth(), hdrSceneColor->GetHeight(),
                        constantBuffer, data.executionDiagnostic))
                {
                    return;
                }
                data.stats.constantsUploaded = true;

                RHIDescriptorSetDesc descriptorSetDesc;
                descriptorSetDesc.layout = data.execution.descriptorSetLayout.Get();
                descriptorSetDesc.debugName = "DeferredLightingDescriptorSet";
                descriptorSetDesc.BindBuffer(
                    0, constantBuffer.Get(), 0,
                    AlignDeferredLightingConstantBufferSize(
                        sizeof(DeferredLightingGPUConstants)));
                descriptorSetDesc.BindTexture(1, gBuffer0View);
                descriptorSetDesc.BindTexture(2, gBuffer1View);
                descriptorSetDesc.BindTexture(3, gBuffer2View);
                descriptorSetDesc.BindTexture(4, gBuffer3View);
                descriptorSetDesc.BindTexture(5, depthView);
                descriptorSetDesc.BindTexture(6, hdrSceneColorView);
                descriptorSetDesc.BindBuffer(
                    7, materialTable, 0, sizeof(DeferredMaterialClassification));
                descriptorSetDesc.BindTexture(
                    8, lightingSnapshot.directionalShadowView.Get());
                descriptorSetDesc.BindSampler(
                    9, lightingSnapshot.directionalShadowSampler.Get());
                descriptorSetDesc.BindBuffer(
                    10, lightingSnapshot.lightConstantsBuffer.Get(), 0,
                    AlignDeferredLightingConstantBufferSize(sizeof(LightConstants)));
                descriptorSetDesc.BindBuffer(
                    11, lightingSnapshot.pointLightsBuffer.Get());
                descriptorSetDesc.BindBuffer(
                    12, lightingSnapshot.spotLightsBuffer.Get());
                descriptorSetDesc.BindBuffer(
                    13, lightingSnapshot.clusterConstantsBuffer.Get(), 0,
                    AlignDeferredLightingConstantBufferSize(sizeof(GPUClusterConstants)));
                descriptorSetDesc.BindBuffer(
                    14, lightingSnapshot.clusterBuffer.Get());
                descriptorSetDesc.BindBuffer(
                    15, lightingSnapshot.clusterLightIndexBuffer.Get());
                descriptorSetDesc.BindTexture(
                    16, lightingSnapshot.environmentIBLIrradianceView.Get());
                descriptorSetDesc.BindTexture(
                    17, lightingSnapshot.environmentIBLPrefilteredView.Get());
                descriptorSetDesc.BindTexture(
                    18, lightingSnapshot.environmentIBLBRDFLUTView.Get());
                descriptorSetDesc.BindSampler(
                    19, lightingSnapshot.environmentIBLSampler.Get());
                descriptorSetDesc.BindBuffer(
                    20, lightingSnapshot.viewConstantBuffer.Get(), 0,
                    lightingSnapshot.viewConstantBuffer->GetSize());

                RHIDescriptorSetRef descriptorSet =
                    data.execution.device->CreateDescriptorSet(descriptorSetDesc);
                if (!descriptorSet)
                {
                    data.executionDiagnostic =
                        "Deferred lighting could not create its record-owned descriptor set";
                    return;
                }
                data.stats.descriptorSetCreated = true;

                if (!context.RetainSubmissionResource(descriptorSet) ||
                    !context.RetainSubmissionResource(
                        Ref<RefCounted>(constantBuffer)) ||
                    !context.RetainSubmissionResource(
                        Ref<RefCounted>(data.execution.pipeline)) ||
                    !context.RetainSubmissionResource(
                        Ref<RefCounted>(data.execution.descriptorSetLayout)) ||
                    !context.RetainSubmissionResource(
                        Ref<RefCounted>(lightingSnapshot.frameDescriptorSet)) ||
                    !context.RetainSubmissionResource(
                        Ref<RefCounted>(lightingSnapshot.frameSetLayout)) ||
                    !context.RetainSubmissionResource(
                        Ref<RefCounted>(lightingSnapshot.viewConstantBuffer)) ||
                    !context.RetainSubmissionResource(
                        Ref<RefCounted>(lightingSnapshot.directionalShadowView)) ||
                    !context.RetainSubmissionResource(
                        Ref<RefCounted>(lightingSnapshot.directionalShadowSampler)) ||
                    !context.RetainSubmissionResource(
                        Ref<RefCounted>(lightingSnapshot.lightConstantsBuffer)) ||
                    !context.RetainSubmissionResource(
                        Ref<RefCounted>(lightingSnapshot.pointLightsBuffer)) ||
                    !context.RetainSubmissionResource(
                        Ref<RefCounted>(lightingSnapshot.spotLightsBuffer)) ||
                    !context.RetainSubmissionResource(
                        Ref<RefCounted>(lightingSnapshot.clusterConstantsBuffer)) ||
                    !context.RetainSubmissionResource(
                        Ref<RefCounted>(lightingSnapshot.clusterBuffer)) ||
                    !context.RetainSubmissionResource(
                        Ref<RefCounted>(lightingSnapshot.clusterLightIndexBuffer)) ||
                    !context.RetainSubmissionResource(
                        Ref<RefCounted>(lightingSnapshot.environmentIBLIrradianceView)) ||
                    !context.RetainSubmissionResource(
                        Ref<RefCounted>(lightingSnapshot.environmentIBLPrefilteredView)) ||
                    !context.RetainSubmissionResource(
                        Ref<RefCounted>(lightingSnapshot.environmentIBLBRDFLUTView)) ||
                    !context.RetainSubmissionResource(
                        Ref<RefCounted>(lightingSnapshot.environmentIBLSampler)))
                {
                    data.executionDiagnostic =
                        "Deferred lighting submission ownership rejected record-local resources";
                    return;
                }

                const uint32 width = hdrSceneColor->GetWidth();
                const uint32 height = hdrSceneColor->GetHeight();
                const uint32 groupsX =
                    (width + RVX_DEFERRED_LIGHTING_THREAD_GROUP_SIZE - 1) /
                    RVX_DEFERRED_LIGHTING_THREAD_GROUP_SIZE;
                const uint32 groupsY =
                    (height + RVX_DEFERRED_LIGHTING_THREAD_GROUP_SIZE - 1) /
                    RVX_DEFERRED_LIGHTING_THREAD_GROUP_SIZE;

                RHICommandContext& commands = context.Commands();
                commands.SetPipeline(data.execution.pipeline.Get());
                commands.SetDescriptorSet(0, descriptorSet.Get());
                commands.Dispatch(groupsX, groupsY, 1);

                data.stats.dispatchGroupCountX = groupsX;
                data.stats.dispatchGroupCountY = groupsY;
                data.stats.dispatchPixelCount =
                    static_cast<uint64>(width) * static_cast<uint64>(height);
                data.stats.dispatchRecorded = true;
                if (data.lighting.recordResults != nullptr)
                {
                    RenderLightingSurfaceExecutionEvidence evidence;
                    evidence.lightingPlanIdentity =
                        data.lighting.lightingPlanIdentity;
                    evidence.route =
                        RenderLightingSurfaceRoute::DeferredOpaque;
                    evidence.frozenBufferMask = 0x3fu;
                    evidence.directionalShadowBound =
                        data.lighting.directionalShadowEnabled;
                    evidence.textureIBLBound =
                        data.lighting.textureIBLEnabled;
                    evidence.exactIBLViews = exactIBLViews;
                    evidence.workCount = data.stats.dispatchPixelCount;
                    evidence.executed = true;
                    if (!data.lighting.recordResults
                             ->RecordLightingSurfaceExecution(evidence))
                    {
                        data.executionDiagnostic =
                            "Deferred lighting could not publish its exact execution evidence";
                        return;
                    }
                }
                executionGuard.MarkSucceeded();
            });
        if (!registered)
        {
            m_unsupportedReason =
                "Deferred lighting could not register its feature graph pass";
        }
        return registered;
    }

} // namespace RVX
