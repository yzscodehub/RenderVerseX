#include "Render/Passes/CameraVelocityPass.h"

#include "Core/Log.h"
#include "Render/Graph/ResourceViewCache.h"
#include "Render/PipelineCache.h"
#include "Render/Renderer/ViewData.h"
#include "RHI/RHIRenderPass.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace RVX
{
    namespace
    {
        constexpr uint64 RVX_CAMERA_VELOCITY_CONSTANT_BUFFER_ALIGNMENT = 256;

        uint64 AlignCameraVelocityConstantBufferSize(uint64 size)
        {
            return (size + RVX_CAMERA_VELOCITY_CONSTANT_BUFFER_ALIGNMENT - 1) &
                   ~(RVX_CAMERA_VELOCITY_CONSTANT_BUFFER_ALIGNMENT - 1);
        }

        [[nodiscard]] bool HasUnjitteredViewProjectionMotion(
            const Mat4& current,
            const Mat4& previous) noexcept
        {
            constexpr float32 epsilon = 1.0e-6f;
            for (uint32 column = 0; column < 4; ++column)
            {
                for (uint32 row = 0; row < 4; ++row)
                {
                    const float32 currentValue = current[column][row];
                    const float32 previousValue = previous[column][row];
                    if (!std::isfinite(currentValue) ||
                        !std::isfinite(previousValue))
                    {
                        return false;
                    }
                    if (std::abs(currentValue - previousValue) > epsilon)
                    {
                        return true;
                    }
                }
            }
            return false;
        }

        struct CameraVelocityGPUConstants
        {
            Mat4 inverseViewProjection = Mat4Identity();
            Mat4 previousViewProjection = Mat4Identity();
            Vec4 outputSizeAndInvSize{1.0f, 1.0f, 1.0f, 1.0f};
            Vec4 velocityParams{0.0f, 0.0f, 1.0f, 1.0e-6f};
        };

        struct GraphPassData
        {
            RenderPassExecutionData execution{};
            RenderPassRecordIdentity identity{};
            std::shared_ptr<RenderPassRecordResults> results;
            IRHIDevice* device = nullptr;
            RHIPipelineRef pipeline;
            RHIDescriptorSetLayoutRef descriptorSetLayout;
            RHIBufferRef constantBuffer;
            RHISamplerRef sampler;
            float32 depthClearValue = 1.0f;
            RGTextureHandle depthHandle{};
            RGTextureHandle velocityHandle{};
            RGTextureViewHandle depthViewHandle{};
            RGTextureViewHandle velocityViewHandle{};
            bool requestedEnabled = false;
            bool supported = false;
            bool contextValid = false;
        };

        [[nodiscard]] bool IsNewerOrSameRecording(
            const RenderPassRecordIdentity& candidate,
            const RenderPassRecordIdentity& previous)
        {
            if (!candidate.IsValid())
            {
                return false;
            }
            if (!previous.IsValid() || candidate == previous)
            {
                return true;
            }
            if (candidate.frameSequence != previous.frameSequence)
            {
                return candidate.frameSequence > previous.frameSequence;
            }
            if (candidate.viewOrdinal != previous.viewOrdinal)
            {
                return candidate.viewOrdinal > previous.viewOrdinal;
            }
            return candidate.recordEpoch > previous.recordEpoch;
        }

        void ResetStatsForIdentity(GraphPassData& data)
        {
            if (!data.results || data.results->identity != data.identity)
            {
                return;
            }

            CameraVelocityPassStats& stats = data.results->cameraVelocityStats;
            stats = {};
            stats.requested = data.requestedEnabled;
            stats.supported = data.supported;
            stats.outputFormat = RHIFormat::RG16_FLOAT;

            const ViewData& view = data.execution.view;
            stats.depthAvailable = view.depthTarget.IsValid();
            stats.previousViewProjectionAvailable =
                view.previousViewProjectionValid != 0 && !view.resetTemporalHistory;
            stats.viewProjectionMotionObserved =
                stats.previousViewProjectionAvailable &&
                HasUnjitteredViewProjectionMotion(
                    view.viewProjectionMatrix,
                    view.previousViewProjectionMatrix);
            stats.velocityTargetAvailable = view.velocityTarget.IsValid();
            stats.width = view.viewportWidth;
            stats.height = view.viewportHeight;
            if (data.identity.graph == nullptr ||
                !view.velocityTarget.IsValid() ||
                !HasCurrentGraphProvenance(view.velocityTarget, data.identity))
            {
                return;
            }

            stats.velocityTargetResourceIndex = view.velocityTarget.index;
            stats.velocityTargetGraphIdentity = data.identity.graphIdentity;
            stats.velocityTargetRecordingGeneration =
                data.identity.graphRecordingGeneration;
            if (const RHITextureDesc* const desc =
                    data.identity.graph->GetTextureDesc(view.velocityTarget))
            {
                stats.outputFormat = desc->format;
                stats.width = desc->width;
                stats.height = desc->height;
            }
        }

        [[nodiscard]] bool RetainResource(RenderGraphBuilder& builder,
                                          RefCounted* resource)
        {
            return resource == nullptr ||
                builder.RetainSubmissionResource(Ref<RefCounted>(resource));
        }

        [[nodiscard]] bool RetainRecordingResources(
            RenderGraphBuilder& builder,
            const GraphPassData& data)
        {
            return RetainResource(builder, data.pipeline.Get()) &&
                RetainResource(builder, data.descriptorSetLayout.Get()) &&
                RetainResource(builder, data.constantBuffer.Get()) &&
                RetainResource(builder, data.sampler.Get());
        }

        [[nodiscard]] bool CreateAndUploadRecordConstants(GraphPassData& data,
                                                            uint32 width,
                                                            uint32 height)
        {
            if (data.device == nullptr || !data.results ||
                data.results->identity != data.identity)
            {
                return false;
            }

            RHIBufferDesc bufferDesc;
            bufferDesc.size = AlignCameraVelocityConstantBufferSize(
                sizeof(CameraVelocityGPUConstants));
            bufferDesc.usage = RHIBufferUsage::Constant;
            bufferDesc.memoryType = RHIMemoryType::Upload;
            bufferDesc.debugName = "CameraVelocityRecordConstants";
            data.constantBuffer = data.device->CreateBuffer(bufferDesc);
            if (!data.constantBuffer)
            {
                return false;
            }

            const ViewData& view = data.execution.view;
            const float outputWidth = static_cast<float>(std::max(1u, width));
            const float outputHeight = static_cast<float>(std::max(1u, height));
            const bool previousValid =
                view.previousViewProjectionValid != 0 && !view.resetTemporalHistory;
            CameraVelocityGPUConstants constants;
            constants.inverseViewProjection = view.inverseJitteredViewProjectionMatrix;
            constants.previousViewProjection = previousValid
                ? view.previousJitteredViewProjectionMatrix
                : view.jitteredViewProjectionMatrix;
            constants.outputSizeAndInvSize = Vec4(
                outputWidth, outputHeight, 1.0f / outputWidth, 1.0f / outputHeight);
            // A first frame or camera cut still writes the full target, but
            // tells the shader to emit zero motion instead of reading history.
            constants.velocityParams = Vec4(
                previousValid ? 1.0f : 0.0f,
                0.0f,
                data.depthClearValue,
                1.0e-6f);

            void* const mapped = data.constantBuffer->Map();
            if (mapped == nullptr)
            {
                return false;
            }
            std::memcpy(mapped, &constants, sizeof(constants));
            if (!data.constantBuffer->CommitMappedWrite())
            {
                return false;
            }
            data.results->cameraVelocityStats.constantsUploaded = true;
            return true;
        }

        [[nodiscard]] bool PrepareRecording(GraphPassData& data,
                                            RenderGraphBuilder& builder)
        {
            ResetStatsForIdentity(data);
            if (!data.contextValid || !data.results ||
                data.results->identity != data.identity ||
                !data.requestedEnabled || !data.supported || data.device == nullptr)
            {
                return false;
            }

            const ViewData& view = data.execution.view;
            CameraVelocityPassStats& stats = data.results->cameraVelocityStats;
            if (!view.depthTarget.IsValid() || !view.velocityTarget.IsValid() ||
                !HasCurrentGraphProvenance(view.depthTarget, data.identity) ||
                !stats.MatchesVelocityTarget(view.velocityTarget, data.identity))
            {
                return false;
            }

            const RHITextureDesc* const depthDesc =
                builder.GetTextureDesc(view.depthTarget);
            const RHITextureDesc* const velocityDesc =
                builder.GetTextureDesc(view.velocityTarget);
            if (depthDesc == nullptr || velocityDesc == nullptr ||
                velocityDesc->width == 0 || velocityDesc->height == 0 ||
                !data.pipeline || !data.descriptorSetLayout)
            {
                return false;
            }
            stats.outputFormat = velocityDesc->format;
            stats.width = velocityDesc->width;
            stats.height = velocityDesc->height;

            RHISamplerDesc samplerDesc = RHISamplerDesc::PointClamp();
            samplerDesc.debugName = "CameraVelocityRecordPointClampSampler";
            data.sampler = data.device->CreateSampler(samplerDesc);
            if (!data.sampler || !CreateAndUploadRecordConstants(
                                    data, velocityDesc->width, velocityDesc->height) ||
                !RetainRecordingResources(builder, data))
            {
                return false;
            }

            data.depthHandle = view.depthTarget;
            data.depthHandle.hasSubresourceRange = true;
            data.depthHandle.subresourceRange = RHISubresourceRange{
                0, RVX_ALL_MIPS, 0, RVX_ALL_LAYERS, RHITextureAspect::Depth};
            RHITextureViewDesc depthViewDesc;
            depthViewDesc.format = depthDesc->format;
            depthViewDesc.dimension = depthDesc->dimension;
            depthViewDesc.subresourceRange = data.depthHandle.subresourceRange;
            depthViewDesc.type = RHITextureViewType::ShaderResource;
            depthViewDesc.debugName = "CameraVelocityDepthSRV";
            data.depthViewHandle = builder.Read(
                builder.CreateTextureView(data.depthHandle, depthViewDesc),
                MakeRGAccessDesc(
                    RHIResourceState::ShaderResource, RHIShaderStage::Pixel));

            data.velocityHandle = view.velocityTarget;
            RHITextureViewDesc velocityViewDesc;
            velocityViewDesc.format = velocityDesc->format;
            velocityViewDesc.dimension = velocityDesc->dimension;
            velocityViewDesc.subresourceRange = RHISubresourceRange::All();
            velocityViewDesc.type = RHITextureViewType::RenderTarget;
            velocityViewDesc.debugName = "CameraVelocityRTV";
            data.velocityViewHandle = builder.Write(
                builder.CreateTextureView(data.velocityHandle, velocityViewDesc),
                MakeRGAccessDesc(
                    RHIResourceState::RenderTarget,
                    RHIShaderStage::Pixel,
                    RHIDiscardIntent::Discard));
            stats.outputDeclared = data.depthViewHandle.IsValid() &&
                data.velocityViewHandle.IsValid();
            return stats.outputDeclared;
        }

        void ExecuteRecording(const GraphPassData& data,
                              RenderGraphPassContext& context)
        {
            if (!data.results || data.results->identity != data.identity)
            {
                context.MarkExecutionFailed(
                    "CameraVelocityPass has no matching recording-results owner");
                return;
            }

            CameraVelocityPassStats stats = data.results->cameraVelocityStats;
            stats.velocityRecorded = false;
            if (!stats.requested || !stats.supported)
            {
                data.results->cameraVelocityStats = stats;
                return;
            }
            if (!data.contextValid || !data.identity.IsValid() ||
                data.identity.graph == nullptr ||
                !data.identity.Matches(*data.identity.graph) ||
                !stats.outputDeclared || !stats.constantsUploaded ||
                !stats.MatchesVelocityTarget(data.velocityHandle, data.identity) ||
                !data.depthHandle.IsValid() || !data.depthViewHandle.IsValid() ||
                !data.velocityViewHandle.IsValid() || !data.pipeline ||
                !data.descriptorSetLayout || !data.constantBuffer || !data.sampler ||
                !HasCurrentGraphProvenance(data.depthHandle, data.identity) ||
                !HasCurrentGraphProvenance(data.velocityHandle, data.identity) ||
                data.depthViewHandle.graphIdentity != data.identity.graphIdentity ||
                data.depthViewHandle.recordingGeneration !=
                    data.identity.graphRecordingGeneration ||
                data.velocityViewHandle.graphIdentity != data.identity.graphIdentity ||
                data.velocityViewHandle.recordingGeneration !=
                    data.identity.graphRecordingGeneration)
            {
                data.results->cameraVelocityStats = stats;
                context.MarkExecutionFailed(
                    "CameraVelocityPass graph recording prerequisites are unavailable");
                return;
            }

            RHITexture* const depthTexture = context.GetTexture(data.depthHandle);
            RHITexture* const velocityTexture = context.GetTexture(data.velocityHandle);
            RHITextureView* const depthView =
                context.GetTextureView(data.depthViewHandle);
            RHITextureView* const velocityView =
                context.GetTextureView(data.velocityViewHandle);
            if (depthTexture == nullptr || velocityTexture == nullptr ||
                depthView == nullptr || velocityView == nullptr ||
                velocityTexture->GetWidth() != stats.width ||
                velocityTexture->GetHeight() != stats.height ||
                velocityTexture->GetFormat() != stats.outputFormat)
            {
                data.results->cameraVelocityStats = stats;
                context.MarkExecutionFailed(
                    "CameraVelocityPass could not realize its exact recording resources");
                return;
            }

            RHIDescriptorSetDesc descriptorDesc;
            descriptorDesc.layout = data.descriptorSetLayout.Get();
            descriptorDesc.debugName = "CameraVelocityDescriptorSet";
            descriptorDesc.BindBuffer(
                0,
                data.constantBuffer.Get(),
                0,
                AlignCameraVelocityConstantBufferSize(
                    sizeof(CameraVelocityGPUConstants)));
            descriptorDesc.BindTexture(1, depthView);
            descriptorDesc.BindSampler(2, data.sampler.Get());
            descriptorDesc.BindTexture(3, depthView);
            RHIDescriptorSetRef descriptorSet =
                data.device->CreateDescriptorSet(descriptorDesc);
            if (!descriptorSet ||
                !context.RetainSubmissionResource(descriptorSet) ||
                !context.RetainSubmissionResource(
                    Ref<RefCounted>(data.constantBuffer)) ||
                !context.RetainSubmissionResource(
                    Ref<RefCounted>(data.sampler)))
            {
                data.results->cameraVelocityStats = stats;
                context.MarkExecutionFailed(
                    "CameraVelocityPass descriptor creation or retention failed");
                return;
            }

            RHIRenderPassDesc renderPassDesc;
            renderPassDesc.AddColorAttachment(
                velocityView, RHILoadOp::DontCare, RHIStoreOp::Store);
            renderPassDesc.SetRenderArea(
                0, 0, velocityTexture->GetWidth(), velocityTexture->GetHeight());
            RHICommandContext& commands = context.Commands();
            commands.BeginRenderPass(renderPassDesc);
            commands.SetPipeline(data.pipeline.Get());
            commands.SetDescriptorSet(0, descriptorSet.Get());
            RHIViewport viewport;
            viewport.width = static_cast<float>(velocityTexture->GetWidth());
            viewport.height = static_cast<float>(velocityTexture->GetHeight());
            viewport.minDepth = 0.0f;
            viewport.maxDepth = 1.0f;
            commands.SetViewport(viewport);
            RHIRect scissor;
            scissor.width = velocityTexture->GetWidth();
            scissor.height = velocityTexture->GetHeight();
            commands.SetScissor(scissor);
            commands.Draw(3, 1, 0, 0);
            commands.EndRenderPass();

            stats.velocityRecorded = true;
            data.results->cameraVelocityStats = stats;
        }
    } // namespace

    void CameraVelocityPass::OnAdd(IRHIDevice* device)
    {
        m_device = device;
    }

    void CameraVelocityPass::OnRemove()
    {
        m_device = nullptr;
        m_pipelineCache = nullptr;
        m_viewCache = nullptr;
        m_lastPublishedIdentity = {};
        m_lastPublishedStats = {};
        m_enabled = false;
    }

    void CameraVelocityPass::SetResources(PipelineCache* pipelineCache,
                                          ResourceViewCache* viewCache)
    {
        m_pipelineCache = pipelineCache;
        m_viewCache = viewCache;
        m_device = pipelineCache ? pipelineCache->GetDevice() : m_device;
    }

    bool CameraVelocityPass::IsSupported() const
    {
        if (!m_enabled)
        {
            m_unsupportedReason = "Camera velocity pass has not been requested";
            return false;
        }
        if (!m_pipelineCache || !m_pipelineCache->IsInitialized())
        {
            m_unsupportedReason =
                "Camera velocity pass requires an initialized PipelineCache";
            return false;
        }
        if (!m_viewCache || !m_viewCache->IsInitialized())
        {
            m_unsupportedReason =
                "Camera velocity pass requires an initialized ResourceViewCache";
            return false;
        }
        if (!m_pipelineCache->GetPostProcessSetLayout() ||
            !m_pipelineCache->GetCameraVelocityPipeline(RHIFormat::RG16_FLOAT))
        {
            m_unsupportedReason =
                "Camera velocity fullscreen pipeline resources are not available";
            return false;
        }
        m_unsupportedReason.clear();
        return true;
    }

    void CameraVelocityPass::Setup(RenderGraphBuilder& builder, const ViewData& view)
    {
        (void)builder;
        (void)view;
        // Typed AddToGraph owns all per-recording declarations.
    }

    void CameraVelocityPass::Execute(RHICommandContext& ctx, const ViewData& view)
    {
        (void)ctx;
        (void)view;
        // Typed AddToGraph owns execution; no mutable pass mailbox is read.
    }

    void CameraVelocityPass::Execute(RenderGraphPassContext& context,
                                     const ViewData& view)
    {
        (void)context;
        (void)view;
        // The legacy graph adapter is deliberately inert.
    }

    void CameraVelocityPass::PublishRecordResults(
        const std::shared_ptr<RenderPassRecordResults>& results,
        const RenderPassRecordIdentity& expectedIdentity)
    {
        if (results == nullptr || results->identity != expectedIdentity ||
            !IsNewerOrSameRecording(expectedIdentity, m_lastPublishedIdentity))
        {
            return;
        }
        m_lastPublishedIdentity = expectedIdentity;
        m_lastPublishedStats = results->cameraVelocityStats;
    }

    void CameraVelocityPass::AddToGraph(RenderGraph& graph, const ViewData& view)
    {
        AddToGraph(graph, MakeRenderPassRecordContext(graph, view));
    }

    void CameraVelocityPass::AddToGraph(
        RenderGraph& graph,
        const RenderPassRecordContext& context)
    {
        // Prove source ownership before invoking the generic snapshot helper:
        // it initializes results, which must never mutate a foreign recording.
        const bool sourceContextValid = !context.legacyAdapter &&
            context.MatchesTargetGraph(graph) && context.IsFrameIdentityValid() &&
            context.frameSnapshot != nullptr && context.results != nullptr;
        const bool suppliedResultsCompatible = context.results == nullptr ||
            context.results->identity == RenderPassRecordIdentity{} ||
            context.results->identity == context.identity;
        const bool suppliedSnapshotCompatible = context.frameSnapshot == nullptr ||
            context.frameSnapshot->identity == context.identity;
        RenderPassExecutionData execution;
        if (sourceContextValid && suppliedResultsCompatible &&
            suppliedSnapshotCompatible)
        {
            execution = MakeRenderPassExecutionData(context);
        }
        else
        {
            execution.view = context.view;
            execution.identity = context.identity;
        }

        const bool contextValid = !context.legacyAdapter &&
            context.MatchesTargetGraph(graph) && context.IsFrameIdentityValid() &&
            execution.MatchesTargetGraph(graph) && execution.IsFrameIdentityValid() &&
            execution.view.velocityTarget.IsValid() &&
            execution.view.depthTarget.IsValid() &&
            HasCurrentGraphProvenance(execution.view.velocityTarget,
                                      execution.identity) &&
            HasCurrentGraphProvenance(execution.view.depthTarget,
                                      execution.identity) &&
            graph.GetTextureDesc(execution.view.velocityTarget) != nullptr &&
            graph.GetTextureDesc(execution.view.depthTarget) != nullptr;
        const bool resultOwnershipValid = execution.identity.Matches(graph) &&
            execution.results != nullptr &&
            execution.results->identity == execution.identity &&
            execution.frameSnapshot != nullptr &&
            execution.frameSnapshot->identity == execution.identity;
        const bool requestedEnabled = m_enabled;
        const bool supported = IsSupported();
        PipelineCache* const pipelineCache = m_pipelineCache;
        IRHIDevice* const device = pipelineCache != nullptr
            ? pipelineCache->GetDevice() : m_device;
        const std::shared_ptr<RenderPassRecordResults> results = execution.results;
        const RHIPipelineRef pipeline = pipelineCache != nullptr
            ? RHIPipelineRef(pipelineCache->GetCameraVelocityPipeline(
                  RHIFormat::RG16_FLOAT))
            : RHIPipelineRef{};
        const RHIDescriptorSetLayoutRef descriptorSetLayout = pipelineCache != nullptr
            ? RHIDescriptorSetLayoutRef(pipelineCache->GetPostProcessSetLayout())
            : RHIDescriptorSetLayoutRef{};
        const float32 depthClearValue = pipelineCache != nullptr
            ? pipelineCache->GetDepthClearValue()
            : 1.0f;

        graph.AddPass<GraphPassData>(
            GetName(),
            GetPassType(),
            [execution,
             contextValid,
             requestedEnabled,
             supported,
             device,
             pipeline,
             descriptorSetLayout,
             depthClearValue,
             results,
             resultOwnershipValid](RenderGraphBuilder& builder, GraphPassData& data)
            {
                data.execution = execution;
                data.identity = execution.identity;
                data.contextValid = contextValid;
                data.requestedEnabled = requestedEnabled;
                data.supported = supported;
                data.device = device;
                data.pipeline = pipeline;
                data.descriptorSetLayout = descriptorSetLayout;
                data.depthClearValue = depthClearValue;
                data.results = resultOwnershipValid ? results : nullptr;
                ResetStatsForIdentity(data);
                if (!PrepareRecording(data, builder) && data.results &&
                    data.results->identity == data.identity)
                {
                    data.depthHandle = {};
                    data.velocityHandle = {};
                    data.depthViewHandle = {};
                    data.velocityViewHandle = {};
                    data.results->cameraVelocityStats.outputDeclared = false;
                    data.results->cameraVelocityStats.velocityRecorded = false;
                }
            },
            [](const GraphPassData& data, RenderGraphPassContext& context)
            {
                ExecuteRecording(data, context);
            });
    }
} // namespace RVX
