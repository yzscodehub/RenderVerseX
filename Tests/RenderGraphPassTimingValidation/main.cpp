#include "Render/Graph/RenderGraph.h"

#include <gtest/gtest.h>

#include <span>
#include <string>
#include <vector>

namespace RVX
{
    struct RenderGraphExecutionTestAccess
    {
        static void Prepare(RenderGraphExecution& execution)
        {
            execution.Prepare();
        }

        static void RetainObserver(
            RenderGraphExecution& execution,
            std::shared_ptr<IRenderGraphExecutionTerminalObserver> observer)
        {
            execution.RetainTerminalObserver(std::move(observer));
        }

    };
} // namespace RVX

namespace
{
    class TerminalProbe final
        : public RVX::IRenderGraphExecutionTerminalObserver
    {
    public:
        void OnRenderGraphSubmitted(
            const RVX::GPUCompletionToken&) noexcept override
        {
            ++submitted;
        }
        void OnRenderGraphAborted() noexcept override { ++aborted; }
        void OnRenderGraphDeviceLost() noexcept override { ++deviceLost; }
        void OnRenderGraphRetired() noexcept override { ++retired; }

        RVX::uint32 submitted = 0;
        RVX::uint32 aborted = 0;
        RVX::uint32 deviceLost = 0;
        RVX::uint32 retired = 0;
    };

    class RecordingProbe final : public RVX::IRenderGraphPassTimestampRecorder
    {
    public:
        bool BeginExactGraphicsExecution(
            RVX::RHICommandContext&,
            RVX::uint32 expectedPassCount) noexcept override
        {
            events.emplace_back("begin:" + std::to_string(expectedPassCount));
            return beginSucceeds;
        }

        bool RecordPassTimestamp(
            RVX::RHICommandContext&,
            RVX::uint32,
            std::string_view passName,
            RVX::RenderGraphPassTimestampBoundary boundary) noexcept override
        {
            events.emplace_back(
                std::string(boundary == RVX::RenderGraphPassTimestampBoundary::Begin
                    ? "start:"
                    : "end:") + std::string(passName));
            return boundarySucceeds;
        }

        bool FinalizeExactGraphicsExecution(
            RVX::RHICommandContext&) noexcept override
        {
            events.emplace_back("finalize");
            return finalizeSucceeds;
        }

        std::shared_ptr<RVX::IRenderGraphExecutionTerminalObserver>
        SealExecutionTerminalObserver() noexcept override
        {
            return std::make_shared<TerminalProbe>();
        }

        void AbortExactGraphicsExecution(const char*) noexcept override
        {
            events.emplace_back("abort");
        }

        bool beginSucceeds = true;
        bool boundarySucceeds = true;
        bool finalizeSucceeds = true;
        std::vector<std::string> events;
    };

    // This command context is intentionally command-free. Native timestamp
    // query behavior is qualified by the separate producer; these tests lock
    // only the narrow RenderGraph recorder ABI.
    class ContractCommandContext final : public RVX::RHICommandContext
    {
    public:
        RVX::RHICommandQueueType GetQueueType() const override
        {
            return RVX::RHICommandQueueType::Graphics;
        }
        void Begin() override {}
        void End() override {}
        void Reset() override {}
        void BeginEvent(const char*, RVX::uint32 = 0) override {}
        void EndEvent() override {}
        void SetMarker(const char*, RVX::uint32 = 0) override {}
        void BufferBarrier(const RVX::RHIBufferBarrier&) override {}
        void TextureBarrier(const RVX::RHITextureBarrier&) override {}
        void Barriers(std::span<const RVX::RHIBufferBarrier>, std::span<const RVX::RHITextureBarrier>) override {}
        void BeginBarrier(const RVX::RHIBufferBarrier&) override {}
        void BeginBarrier(const RVX::RHITextureBarrier&) override {}
        void EndBarrier(const RVX::RHIBufferBarrier&) override {}
        void EndBarrier(const RVX::RHITextureBarrier&) override {}
        void BeginRenderPass(const RVX::RHIRenderPassDesc&) override {}
        void EndRenderPass() override {}
        void SetPipeline(RVX::RHIPipeline*) override {}
        void SetVertexBuffer(RVX::uint32, RVX::RHIBuffer*, RVX::uint64 = 0) override {}
        void SetVertexBuffers(RVX::uint32, std::span<RVX::RHIBuffer* const>, std::span<const RVX::uint64> = {}) override {}
        void SetIndexBuffer(RVX::RHIBuffer*, RVX::RHIFormat, RVX::uint64 = 0) override {}
        void SetDescriptorSet(RVX::uint32, RVX::RHIDescriptorSet*, std::span<const RVX::uint32> = {}) override {}
        void SetPushConstants(const void*, RVX::uint32, RVX::uint32 = 0) override {}
        void SetViewport(const RVX::RHIViewport&) override {}
        void SetViewports(std::span<const RVX::RHIViewport>) override {}
        void SetScissor(const RVX::RHIRect&) override {}
        void SetScissors(std::span<const RVX::RHIRect>) override {}
        void Draw(RVX::uint32, RVX::uint32 = 1, RVX::uint32 = 0, RVX::uint32 = 0) override {}
        void DrawIndexed(RVX::uint32, RVX::uint32 = 1, RVX::uint32 = 0, RVX::int32 = 0, RVX::uint32 = 0) override {}
        void DrawIndirect(RVX::RHIBuffer*, RVX::uint64, RVX::uint32, RVX::uint32) override {}
        void DrawIndexedIndirect(RVX::RHIBuffer*, RVX::uint64, RVX::uint32, RVX::uint32) override {}
        void Dispatch(RVX::uint32, RVX::uint32, RVX::uint32) override {}
        void DispatchIndirect(RVX::RHIBuffer*, RVX::uint64) override {}
        void CopyBuffer(RVX::RHIBuffer*, RVX::RHIBuffer*, RVX::uint64, RVX::uint64, RVX::uint64) override {}
        void CopyTexture(RVX::RHITexture*, RVX::RHITexture*, const RVX::RHITextureCopyDesc& = {}) override {}
        void CopyBufferToTexture(RVX::RHIBuffer*, RVX::RHITexture*, const RVX::RHIBufferTextureCopyDesc&) override {}
        void CopyTextureToBuffer(RVX::RHITexture*, RVX::RHIBuffer*, const RVX::RHIBufferTextureCopyDesc&) override {}
        void BeginQuery(RVX::RHIQueryPool*, RVX::uint32) override {}
        void EndQuery(RVX::RHIQueryPool*, RVX::uint32) override {}
        void WriteTimestamp(RVX::RHIQueryPool*, RVX::uint32) override {}
        void ResolveQueries(RVX::RHIQueryPool*, RVX::uint32, RVX::uint32, RVX::RHIBuffer*, RVX::uint64) override {}
        void ResetQueries(RVX::RHIQueryPool*, RVX::uint32, RVX::uint32) override {}
        void SetStencilReference(RVX::uint32) override {}
        void SetBlendConstants(const float[4]) override {}
        void SetDepthBias(float, float, float = 0.0f) override {}
        void SetDepthBounds(float, float) override {}
        void SetStencilReferenceSeparate(RVX::uint32, RVX::uint32) override {}
        void SetLineWidth(float) override {}
        void SignalFence(RVX::RHIFence*, RVX::uint64) override {}
        void WaitFence(RVX::RHIFence*, RVX::uint64) override {}
    };
} // namespace

TEST(RenderGraphPassTimestampRecorderContract, OrderedCallbacksRetainStablePassNames)
{
    RecordingProbe probe;
    ContractCommandContext context;

    ASSERT_TRUE(probe.BeginExactGraphicsExecution(context, 2));
    ASSERT_TRUE(probe.RecordPassTimestamp(
        context, 0, "Depth", RVX::RenderGraphPassTimestampBoundary::Begin));
    ASSERT_TRUE(probe.RecordPassTimestamp(
        context, 0, "Depth", RVX::RenderGraphPassTimestampBoundary::End));
    ASSERT_TRUE(probe.RecordPassTimestamp(
        context, 1, "Lighting", RVX::RenderGraphPassTimestampBoundary::Begin));
    ASSERT_TRUE(probe.RecordPassTimestamp(
        context, 1, "Lighting", RVX::RenderGraphPassTimestampBoundary::End));
    ASSERT_TRUE(probe.FinalizeExactGraphicsExecution(context));

    EXPECT_EQ(probe.events, (std::vector<std::string>{
        "begin:2", "start:Depth", "end:Depth", "start:Lighting",
        "end:Lighting", "finalize" }));
}

TEST(RenderGraphPassTimestampRecorderContract, NullInjectionHasNoCallbackBehavior)
{
    RVX::IRenderGraphPassTimestampRecorder* recorder = nullptr;
    RecordingProbe probe;

    if (recorder != nullptr)
        recorder->AbortExactGraphicsExecution("unexpected");

    EXPECT_TRUE(probe.events.empty());
}

TEST(RenderGraphPassTimestampRecorderContract, BoundaryFailurePropagatesAndCanBeAborted)
{
    RecordingProbe probe;
    probe.boundarySucceeds = false;
    ContractCommandContext context;

    ASSERT_TRUE(probe.BeginExactGraphicsExecution(context, 1));
    EXPECT_FALSE(probe.RecordPassTimestamp(
        context, 0, "Opaque", RVX::RenderGraphPassTimestampBoundary::Begin));
    probe.AbortExactGraphicsExecution("boundary failed");

    EXPECT_EQ(probe.events, (std::vector<std::string>{
        "begin:1", "start:Opaque", "abort" }));
}

TEST(RenderGraphPassTimestampRecorderContract,
     ExecutionObserverReceivesUnsubmittedAbort)
{
    auto observer = std::make_shared<TerminalProbe>();
    RVX::RenderGraphExecution execution;
    RVX::RenderGraphExecutionTestAccess::Prepare(execution);
    RVX::RenderGraphExecutionTestAccess::RetainObserver(execution, observer);

    EXPECT_TRUE(execution.AbortUnsubmitted());
    EXPECT_EQ(observer->aborted, 1u);
    EXPECT_EQ(observer->submitted, 0u);
    EXPECT_EQ(observer->retired, 0u);
}
