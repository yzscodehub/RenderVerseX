#include "Render/Passes/DeferredLightingPass.h"

#include "Core/Log.h"
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
                const char* failureDiagnostic) noexcept
                : m_context(context)
                , m_failureDiagnostic(failureDiagnostic)
            {
            }

            ~RequiredPassExecutionGuard()
            {
                if (!m_succeeded)
                {
                    m_context.MarkExecutionFailed(m_failureDiagnostic);
                }
            }

            void MarkSucceeded() noexcept
            {
                m_succeeded = true;
            }

        private:
            RenderGraphPassContext& m_context;
            const char* m_failureDiagnostic = nullptr;
            bool m_succeeded = false;
        };

        uint64 AlignDeferredLightingConstantBufferSize(uint64 size)
        {
            return (size + RVX_DEFERRED_LIGHTING_CONSTANT_BUFFER_ALIGNMENT - 1) &
                   ~(RVX_DEFERRED_LIGHTING_CONSTANT_BUFFER_ALIGNMENT - 1);
        }

        struct DeferredLightingGPUConstants
        {
            float directionalLightDirectionIntensity[4] =
                {0.0f, -1.0f, 0.0f, 0.0f};
            float directionalLightColorAmbientIntensity[4] =
                {1.0f, 1.0f, 1.0f, 0.0f};
            float ambientColorPadding[4] = {1.0f, 1.0f, 1.0f, 0.0f};
            float outputSizeInvSize[4] = {1.0f, 1.0f, 1.0f, 1.0f};
            // x = clear depth, y = comparison epsilon, z = reverse-Z flag.
            float depthBackgroundRule[4] = {1.0f, 0.000001f, 0.0f, 0.0f};
        };

        static_assert(sizeof(DeferredLightingGPUConstants) == 80);

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

        struct DeferredLightingGraphPassData
        {
            RGTextureHandle gBuffer0{};
            RGTextureHandle gBuffer1{};
            RGTextureHandle gBuffer2{};
            RGTextureHandle gBuffer3{};
            RGTextureHandle deviceDepth{};
            RGTextureHandle hdrSceneColor{};
            RGBufferHandle materialTable{};
            RGTextureViewHandle gBuffer0View{};
            RGTextureViewHandle gBuffer1View{};
            RGTextureViewHandle gBuffer2View{};
            RGTextureViewHandle gBuffer3View{};
            RGTextureViewHandle deviceDepthView{};
            RGTextureViewHandle hdrSceneColorView{};
            // Execution receives const pass data.  Keeping this mutable makes
            // evidence belong to this graph pass instance, never to the
            // feature object that may already be recording a later frame.
            mutable DeferredLightingGraphPassStats stats{};
            bool inputsValid = false;
        };

        bool CreateAndUploadDeferredLightingConstants(
            IRHIDevice& device,
            const PipelineCache& pipelineCache,
            const ViewData& view,
            uint32 width,
            uint32 height,
            RHIBufferRef& outConstantBuffer,
            std::string& outError)
        {
            const float outputWidth = static_cast<float>(std::max(1u, width));
            const float outputHeight = static_cast<float>(std::max(1u, height));
            DeferredLightingGPUConstants constants;
            constants.directionalLightDirectionIntensity[0] =
                view.directionalLightDirection.x;
            constants.directionalLightDirectionIntensity[1] =
                view.directionalLightDirection.y;
            constants.directionalLightDirectionIntensity[2] =
                view.directionalLightDirection.z;
            constants.directionalLightDirectionIntensity[3] =
                std::max(view.directionalLightIntensity, 0.0f);
            constants.directionalLightColorAmbientIntensity[0] =
                view.directionalLightColor.x;
            constants.directionalLightColorAmbientIntensity[1] =
                view.directionalLightColor.y;
            constants.directionalLightColorAmbientIntensity[2] =
                view.directionalLightColor.z;
            constants.directionalLightColorAmbientIntensity[3] =
                view.iblAmbientEnabled != 0 ?
                    std::max(view.iblDiffuseIntensity, 0.0f) : 0.0f;
            constants.ambientColorPadding[0] = view.iblDiffuseColor.x;
            constants.ambientColorPadding[1] = view.iblDiffuseColor.y;
            constants.ambientColorPadding[2] = view.iblDiffuseColor.z;
            constants.outputSizeInvSize[0] = outputWidth;
            constants.outputSizeInvSize[1] = outputHeight;
            constants.outputSizeInvSize[2] = 1.0f / outputWidth;
            constants.outputSizeInvSize[3] = 1.0f / outputHeight;
            constants.depthBackgroundRule[0] =
                pipelineCache.GetDepthClearValue();
            constants.depthBackgroundRule[1] = 0.000001f;
            constants.depthBackgroundRule[2] =
                pipelineCache.IsReverseZ() ? 1.0f : 0.0f;

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
        if (!featureBuilder.IsValid() || !inputs.IsComplete() ||
            !EnsurePipelineReady())
        {
            return false;
        }

        const ViewData executionView = view;
        const bool registered = featureBuilder.AddPass<DeferredLightingGraphPassData>(
            "DeferredLightingPass",
            RenderGraphPassType::Compute,
            [inputs](RenderGraphBuilder& builder,
                     DeferredLightingGraphPassData& data)
            {
                data.gBuffer0 = inputs.gBuffer0;
                data.gBuffer1 = inputs.gBuffer1;
                data.gBuffer2 = inputs.gBuffer2;
                data.gBuffer3 = inputs.gBuffer3;
                data.deviceDepth = inputs.deviceDepth;
                data.hdrSceneColor = inputs.hdrSceneColor;
                data.materialTable = inputs.materialTable;

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
                    materialTableDesc != nullptr &&
                    IsCanonicalDeferredMaterialTableDesc(*materialTableDesc);
            },
            [this, executionView](const DeferredLightingGraphPassData& data,
                                  RenderGraphPassContext& context)
            {
                RequiredPassExecutionGuard executionGuard(
                    context,
                    "DeferredLighting did not record its required compute dispatch");
                data.stats.inputsDeclared = data.inputsValid;
                data.stats.resourceViewsDeclared = data.gBuffer0View.IsValid() &&
                    data.gBuffer1View.IsValid() && data.gBuffer2View.IsValid() &&
                    data.gBuffer3View.IsValid() && data.deviceDepthView.IsValid() &&
                    data.hdrSceneColorView.IsValid() &&
                    data.materialTable.IsValid();

                if (!data.inputsValid || !EnsurePipelineReady())
                {
                    return;
                }

                RHITexture* gBuffer0 = context.GetTexture(data.gBuffer0);
                RHITexture* gBuffer1 = context.GetTexture(data.gBuffer1);
                RHITexture* gBuffer2 = context.GetTexture(data.gBuffer2);
                RHITexture* gBuffer3 = context.GetTexture(data.gBuffer3);
                RHITexture* depth = context.GetTexture(data.deviceDepth);
                RHITexture* hdrSceneColor = context.GetTexture(data.hdrSceneColor);
                RHIBuffer* materialTable = context.GetBuffer(data.materialTable);
                if (gBuffer0 == nullptr || gBuffer1 == nullptr ||
                    gBuffer2 == nullptr || gBuffer3 == nullptr ||
                    depth == nullptr || hdrSceneColor == nullptr)
                {
                    return;
                }
                if (materialTable == nullptr ||
                    !IsCanonicalDeferredMaterialTable(*materialTable))
                {
                    m_unsupportedReason =
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
                if (gBuffer0View == nullptr || gBuffer1View == nullptr ||
                    gBuffer2View == nullptr || gBuffer3View == nullptr ||
                    depthView == nullptr || hdrSceneColorView == nullptr)
                {
                    return;
                }

                RHIPipeline* pipeline =
                    m_pipelineCache->GetDeferredLightingPipeline();
                RHIDescriptorSetLayout* descriptorSetLayout =
                    m_pipelineCache->GetDeferredLightingSetLayout();
                if (pipeline == nullptr || descriptorSetLayout == nullptr)
                {
                    return;
                }

                RHIBufferRef constantBuffer;
                if (!CreateAndUploadDeferredLightingConstants(
                        *m_device, *m_pipelineCache, executionView,
                        hdrSceneColor->GetWidth(), hdrSceneColor->GetHeight(),
                        constantBuffer, m_unsupportedReason))
                {
                    return;
                }
                data.stats.constantsUploaded = true;

                RHIDescriptorSetDesc descriptorSetDesc;
                descriptorSetDesc.layout = descriptorSetLayout;
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

                RHIDescriptorSetRef descriptorSet =
                    m_device->CreateDescriptorSet(descriptorSetDesc);
                if (!descriptorSet)
                {
                    RVX_CORE_WARN(
                        "DeferredLightingPass: failed to create descriptor set");
                    return;
                }
                data.stats.descriptorSetCreated = true;

                if (!context.RetainSubmissionResource(descriptorSet) ||
                    !context.RetainSubmissionResource(
                        Ref<RefCounted>(constantBuffer)))
                {
                    RVX_CORE_WARN(
                        "DeferredLightingPass: submission ownership rejected resources");
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
                commands.SetPipeline(pipeline);
                commands.SetDescriptorSet(0, descriptorSet.Get());
                commands.Dispatch(groupsX, groupsY, 1);

                data.stats.dispatchGroupCountX = groupsX;
                data.stats.dispatchGroupCountY = groupsY;
                data.stats.dispatchPixelCount =
                    static_cast<uint64>(width) * static_cast<uint64>(height);
                data.stats.dispatchRecorded = true;
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
