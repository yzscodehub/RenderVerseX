#include "Render/PostProcess/TransparentTemporalResolve.h"

#include "Render/Features/RenderFeatureGraphBuilder.h"
#include "Render/PipelineCache.h"
#include "RHI/RHIRenderPass.h"

#include <array>

namespace RVX
{
    namespace
    {
        struct ResolveData
        {
            std::array<RGTextureViewHandle, 5> inputViews{};
            std::array<RGTextureViewHandle, 3> outputViews{};
            RenderOutputExtent extent{};
            IRHIDevice* device = nullptr;
            RHIPipelineRef pipeline;
            RHIDescriptorSetLayoutRef layout;
            std::shared_ptr<TransparentTemporalResolveReceipt> receipt;
        };

        bool ValidTexture(RenderFeatureGraphBuilder& builder, RGTextureHandle handle,
                          RHIFormat format, RenderOutputExtent extent)
        {
            if (!handle.IsValid() || handle.graphIdentity != builder.GetGraphIdentity() ||
                handle.recordingGeneration != builder.GetRecordingGeneration())
                return false;
            const auto* desc = builder.GetGraph().GetTextureDesc(handle);
            return desc && desc->format == format && desc->width == extent.width &&
                desc->height == extent.height && desc->sampleCount == RHISampleCount::Count1 &&
                desc->dimension == RHITextureDimension::Texture2D && desc->arraySize == 1 &&
                desc->depth == 1 &&
                desc->mipLevels == 1 && HasFlag(desc->usage, RHITextureUsage::ShaderResource);
        }
    }

    bool BuildTransparentTemporalResolve(
        RenderFeatureGraphBuilder& builder, PipelineCache& pipelines,
        const RenderFrameIdentity& frame, const RenderViewIdentity& view,
        RGTextureHandle opaqueVelocity, RGTextureHandle reactive,
        const TransparentTemporalSurfaceInputs& surface,
        TransparentTemporalResolveOutputs& output, std::string& failure)
    {
        output = {};
        failure.clear();
        const std::array<RGTextureHandle, 5> inputs{
            opaqueVelocity, reactive, surface.motionCoverageDepth, surface.flags,
            surface.opaqueLinearDepth};
        constexpr std::array<RHIFormat, 5> formats{
            RHIFormat::RG16_FLOAT, RHIFormat::R8_UNORM, RHIFormat::RGBA16_FLOAT,
            RHIFormat::R32_UINT, RHIFormat::R32_FLOAT};
        if (!builder.IsValid() || !frame.IsValid() || !view.IsValid() ||
            !surface.extent.IsValid() || surface.frameIdentity != frame ||
            surface.viewIdentity != view ||
            builder.GetProvenance().frameSequence != frame.frameSequence)
        {
            failure = "Transparent temporal inputs have mismatched frame/view provenance";
            return false;
        }
        for (size_t i = 0; i < inputs.size(); ++i)
        {
            if (!ValidTexture(builder, inputs[i], formats[i], surface.extent))
            {
                failure = "Transparent temporal resolve requires current single-sample fixed-format inputs";
                return false;
            }
        }
        RHIPipelineRef pipeline(pipelines.GetTransparentTemporalResolvePipeline(surface.historyAvailable));
        RHIDescriptorSetLayoutRef layout(pipelines.GetTransparentTemporalResolveSetLayout());
        if (!pipeline || !layout || !pipelines.GetDevice())
        {
            failure = "Transparent temporal pipeline unavailable: " + pipelines.GetLastError();
            return false;
        }
        std::array<RGTextureHandle, 3> targets;
        constexpr std::array<RHIFormat, 3> outputFormats{
            RHIFormat::RG16_FLOAT, RHIFormat::R8_UNORM, RHIFormat::R32_FLOAT};
        for (size_t i = 0; i < targets.size(); ++i)
        {
            auto desc = RHITextureDesc::RenderTarget(surface.extent.width, surface.extent.height,
                outputFormats[i]);
            desc.usage = desc.usage | RHITextureUsage::ShaderResource | RHITextureUsage::CopySrc;
            desc.debugName = i == 0 ? "ResolvedTemporalVelocity" :
                (i == 1 ? "ResolvedTemporalReactive" : "ResolvedTemporalLinearDepth");
            targets[i] = builder.CreateTexture(desc);
            if (!targets[i].IsValid())
            {
                failure = "Transparent temporal output allocation failed";
                return false;
            }
        }
        const auto receipt = std::make_shared<TransparentTemporalResolveReceipt>();
        const auto extent = surface.extent;
        auto* device = pipelines.GetDevice();
        const bool added = builder.AddPass<ResolveData>("TransparentTemporalResolve",
            RenderGraphPassType::Graphics,
            [=](RenderGraphBuilder& graph, ResolveData& data)
            {
                data.extent = extent;
                data.device = device;
                data.pipeline = pipeline;
                data.layout = layout;
                data.receipt = receipt;
                for (size_t i = 0; i < inputs.size(); ++i)
                {
                    RHITextureViewDesc desc;
                    desc.format = formats[i];
                    desc.type = RHITextureViewType::ShaderResource;
                    data.inputViews[i] = graph.Read(graph.CreateTextureView(inputs[i], desc),
                        MakeRGAccessDesc(RHIResourceState::ShaderResource, RHIShaderStage::Pixel));
                }
                for (size_t i = 0; i < targets.size(); ++i)
                {
                    RHITextureViewDesc desc;
                    desc.format = outputFormats[i];
                    desc.type = RHITextureViewType::RenderTarget;
                    data.outputViews[i] = graph.Write(graph.CreateTextureView(targets[i], desc),
                        MakeRGAccessDesc(RHIResourceState::RenderTarget, RHIShaderStage::Pixel,
                                         RHIDiscardIntent::Discard));
                    graph.SetExportState(targets[i], RHIResourceState::ShaderResource);
                }
            },
            [](const ResolveData& data, RenderGraphPassContext& context)
            {
                const auto fail = [&](const char* reason)
                {
                    data.receipt->failed.store(true, std::memory_order_release);
                    context.MarkExecutionFailed(reason);
                };
                RHIDescriptorSetDesc desc;
                desc.layout = data.layout.Get();
                for (uint32 i = 0; i < data.inputViews.size(); ++i)
                {
                    auto* input = context.GetTextureView(data.inputViews[i]);
                    if (!input) { fail("Transparent temporal input view missing"); return; }
                    desc.BindTexture(i, input);
                }
                auto* velocity = context.GetTextureView(data.outputViews[0]);
                auto* reactiveView = context.GetTextureView(data.outputViews[1]);
                auto* depthView = context.GetTextureView(data.outputViews[2]);
                auto descriptors = data.device->CreateDescriptorSet(desc);
                if (!velocity || !reactiveView || !depthView || !descriptors ||
                    !context.RetainSubmissionResource(Ref<RefCounted>(descriptors)) ||
                    !context.RetainSubmissionResource(Ref<RefCounted>(data.pipeline)) ||
                    !context.RetainSubmissionResource(Ref<RefCounted>(data.layout)))
                { fail("Transparent temporal output/binding retention failed"); return; }
                RHIRenderPassDesc pass;
                pass.AddColorAttachment(velocity, RHILoadOp::DontCare, RHIStoreOp::Store);
                pass.AddColorAttachment(reactiveView, RHILoadOp::DontCare, RHIStoreOp::Store);
                pass.AddColorAttachment(depthView, RHILoadOp::DontCare, RHIStoreOp::Store);
                pass.SetRenderArea(0, 0, data.extent.width, data.extent.height);
                auto& commands = context.Commands();
                commands.BeginRenderPass(pass);
                commands.SetPipeline(data.pipeline.Get());
                commands.SetDescriptorSet(0, descriptors.Get());
                RHIViewport viewport;
                viewport.width = static_cast<float32>(data.extent.width);
                viewport.height = static_cast<float32>(data.extent.height);
                viewport.minDepth = 0.0f;
                viewport.maxDepth = 1.0f;
                commands.SetViewport(viewport);
                RHIRect scissor;
                scissor.width = data.extent.width;
                scissor.height = data.extent.height;
                commands.SetScissor(scissor);
                commands.Draw(3, 1, 0, 0);
                commands.EndRenderPass();
                data.receipt->recorded.store(true, std::memory_order_release);
            });
        if (!added) { failure = "Transparent temporal graph registration failed"; return false; }
        output = {targets[0], targets[1], targets[2], receipt};
        return true;
    }
}
