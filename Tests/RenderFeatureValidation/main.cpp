#include "Core/Log.h"
#include "Render/Features/IRenderFeature.h"
#include "Render/Features/RenderFeatureRegistry.h"
#include "Render/Features/RenderGraphBlackboard.h"
#include "Render/Graph/RenderGraph.h"
#include "Render/Lighting/LightingFramePlan.h"
#include "Render/Lighting/LocalShadowAtlas.h"
#include "Render/Lighting/LocalShadowFramePlan.h"
#include "Render/Lighting/VolumetricLightingFramePlan.h"
#include "Render/Material/MaterialGPUData.h"
#include "Render/Passes/CameraVelocityPass.h"
#include "Render/Passes/LocalShadowPass.h"
#include "Render/Passes/ObjectVelocityPass.h"
#include "Render/Passes/OpaquePass.h"
#include "Render/Passes/RayTracedReflectionCompositePass.h"
#include "Render/Passes/RayTracedReflectionDenoisePass.h"
#include "Render/Passes/RayTracedReflectionPass.h"
#include "Render/RayTracing/HybridRayTracingFramePlan.h"
#include "Render/Reflection/ReflectionFramePlan.h"
#include "Render/Renderer/SceneRenderer.h"
#include "RHI/RHICommandContext.h"

#include "Features/DefaultRenderComposition.h"
#include "Features/DefaultRenderPassBundle.h"
#include "Features/DeferredRecipeFeatureAdapter.h"
#include "Features/LegacyForwardFeatureAdapter.h"
#include "Features/PostProcessPipelineFeatureAdapter.h"
#include "Features/ReflectionFeatureAdapter.h"
#include "Features/TranslucencyFeatureAdapter.h"
#include "Features/VolumetricLightingFeatureAdapter.h"
#include "../Common/RenderGraphValidationAccess.h"

#include <algorithm>
#include <cstdint>
#include <gtest/gtest.h>

#include <iterator>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace RVX
{
namespace
{
    constexpr RenderFeatureResourceContract SceneTexturesContract{
        RenderBlackboardResourceId::SceneTextures,
        SceneTextures::SchemaVersion};
    constexpr RenderFeatureResourceContract VisibilityOutputsContract{
        RenderBlackboardResourceId::VisibilityOutputs,
        VisibilityOutputs::SchemaVersion};
    constexpr RenderFeatureResourceContract LightingOutputsContract{
        RenderBlackboardResourceId::LightingOutputs,
        LightingOutputs::SchemaVersion};
    constexpr RenderFeatureResourceContract LocalShadowOutputsContract{
        RenderBlackboardResourceId::LocalShadowOutputs,
        LocalShadowOutputs::SchemaVersion};
    constexpr RenderFeatureResourceContract ReflectionOutputsContract{
        RenderBlackboardResourceId::ReflectionOutputs,
        ReflectionOutputs::SchemaVersion};
    constexpr RenderFeatureResourceContract VolumetricLightingOutputsContract{
        RenderBlackboardResourceId::VolumetricLightingOutputs,
        VolumetricLightingOutputs::SchemaVersion};
    constexpr RenderFeatureResourceContract PreTranslucencyOutputsContract{
        RenderBlackboardResourceId::PreTranslucencyOutputs,
        PreTranslucencyOutputs::SchemaVersion};
    constexpr RenderFeatureResourceContract TranslucencyOutputsContract{
        RenderBlackboardResourceId::TranslucencyOutputs,
        TranslucencyOutputs::SchemaVersion};
    constexpr RenderFeatureResourceContract PostProcessOutputsContract{
        RenderBlackboardResourceId::PostProcessOutputs,
        PostProcessOutputs::SchemaVersion};
    constexpr RenderFeatureResourceContract AmbientOcclusionOutputsContract{
        RenderBlackboardResourceId::AmbientOcclusionOutputs,
        AmbientOcclusionOutputs::SchemaVersion};

    class ScopedTestLog final
    {
    public:
        ScopedTestLog() { Log::Initialize(); }
        ~ScopedTestLog() { Log::Shutdown(); }
    };

    class TestBuffer final : public RHIBuffer
    {
    public:
        explicit TestBuffer(uint64 size = 256, uint32 stride = 16)
        {
            m_desc.size = size;
            m_desc.stride = stride;
            m_desc.usage = RHIBufferUsage::ShaderResource |
                RHIBufferUsage::UnorderedAccess;
        }

        uint64 GetSize() const override { return m_desc.size; }
        RHIBufferUsage GetUsage() const override { return m_desc.usage; }
        RHIMemoryType GetMemoryType() const override
        {
            return RHIMemoryType::Default;
        }
        uint32 GetStride() const override { return m_desc.stride; }
        void* Map() override { return nullptr; }
        void Unmap() override {}

    private:
        RHIBufferDesc m_desc{};
    };

    class NoOpCommandContext final : public RHICommandContext
    {
    public:
        void Begin() override {}
        void End() override {}
        void Reset() override {}
        void BeginEvent(const char*, uint32 = 0) override {}
        void EndEvent() override {}
        void SetMarker(const char*, uint32 = 0) override {}
        void BufferBarrier(const RHIBufferBarrier&) override {}
        void TextureBarrier(const RHITextureBarrier&) override {}
        void Barriers(std::span<const RHIBufferBarrier>,
                      std::span<const RHITextureBarrier>) override {}
        void BeginBarrier(const RHIBufferBarrier&) override {}
        void BeginBarrier(const RHITextureBarrier&) override {}
        void EndBarrier(const RHIBufferBarrier&) override {}
        void EndBarrier(const RHITextureBarrier&) override {}
        void BeginRenderPass(const RHIRenderPassDesc&) override {}
        void EndRenderPass() override {}
        void SetPipeline(RHIPipeline*) override {}
        void SetVertexBuffer(uint32, RHIBuffer*, uint64 = 0) override {}
        void SetVertexBuffers(uint32,
                              std::span<RHIBuffer* const>,
                              std::span<const uint64> = {}) override {}
        void SetIndexBuffer(RHIBuffer*, RHIFormat, uint64 = 0) override {}
        void SetDescriptorSet(uint32,
                              RHIDescriptorSet*,
                              std::span<const uint32> = {}) override {}
        void SetPushConstants(const void*, uint32, uint32 = 0) override {}
        void SetViewport(const RHIViewport&) override {}
        void SetViewports(std::span<const RHIViewport>) override {}
        void SetScissor(const RHIRect&) override {}
        void SetScissors(std::span<const RHIRect>) override {}
        void Draw(uint32, uint32 = 1, uint32 = 0, uint32 = 0) override {}
        void DrawIndexed(uint32,
                         uint32 = 1,
                         uint32 = 0,
                         int32 = 0,
                         uint32 = 0) override {}
        void DrawIndirect(RHIBuffer*, uint64, uint32, uint32) override {}
        void DrawIndexedIndirect(RHIBuffer*, uint64, uint32, uint32) override {}
        void Dispatch(uint32, uint32, uint32) override {}
        void DispatchIndirect(RHIBuffer*, uint64) override {}
        void CopyBuffer(RHIBuffer*, RHIBuffer*, uint64, uint64, uint64) override {}
        void CopyTexture(RHITexture*,
                         RHITexture*,
                         const RHITextureCopyDesc& = {}) override {}
        void CopyBufferToTexture(RHIBuffer*,
                                 RHITexture*,
                                 const RHIBufferTextureCopyDesc&) override {}
        void CopyTextureToBuffer(RHITexture*,
                                 RHIBuffer*,
                                 const RHIBufferTextureCopyDesc&) override {}
        void BeginQuery(RHIQueryPool*, uint32) override {}
        void EndQuery(RHIQueryPool*, uint32) override {}
        void WriteTimestamp(RHIQueryPool*, uint32) override {}
        void ResolveQueries(RHIQueryPool*,
                            uint32,
                            uint32,
                            RHIBuffer*,
                            uint64) override {}
        void ResetQueries(RHIQueryPool*, uint32, uint32) override {}
        void SetStencilReference(uint32) override {}
        void SetBlendConstants(const float[4]) override {}
        void SetDepthBias(float, float, float = 0.0f) override {}
        void SetDepthBounds(float, float) override {}
        void SetStencilReferenceSeparate(uint32, uint32) override {}
        void SetLineWidth(float) override {}
        void SignalFence(RHIFence*, uint64) override {}
        void WaitFence(RHIFence*, uint64) override {}
    };

    struct TestLegacyPassLifecycle
    {
        uint32 onAddCount = 0;
        uint32 onRemoveCount = 0;
    };

    class TestLegacyPass final : public IRenderPass
    {
    public:
        TestLegacyPass(std::string name,
                       int32 priority,
                       bool requestedEnabled,
                       bool supported,
                       bool enabled,
                       bool recordsResourceUsage = true,
                       std::shared_ptr<TestLegacyPassLifecycle> lifecycle = {})
            : m_name(std::move(name))
            , m_priority(priority)
            , m_requestedEnabled(requestedEnabled)
            , m_supported(supported)
            , m_enabled(enabled)
            , m_recordsResourceUsage(recordsResourceUsage)
            , m_lifecycle(std::move(lifecycle))
            , m_buffer(RHIBufferRef(new TestBuffer()))
        {
        }

        const char* GetName() const override { return m_name.c_str(); }
        int32 GetPriority() const override { return m_priority; }
        bool IsRequestedEnabled() const override { return m_requestedEnabled; }
        bool IsSupported() const override { return m_supported; }
        bool IsEnabled() const override { return m_enabled; }
        const std::string& GetUnsupportedReason() const override
        {
            return m_unsupportedReason;
        }

        void AddToGraph(RenderGraph& graph,
                        const RenderPassRecordContext& context) override
        {
            m_receivedMatchingContext = context.MatchesTargetGraph(graph);
            if (!m_recordsResourceUsage)
            {
                struct EmptyPassData
                {
                    TestLegacyPass* pass = nullptr;
                };
                graph.AddPass<EmptyPassData>(
                    m_name.c_str(),
                    RenderGraphPassType::Graphics,
                    [this](RenderGraphBuilder&, EmptyPassData& data)
                    {
                        data.pass = this;
                    },
                    [](const EmptyPassData& data, RHICommandContext&)
                    {
                        ++data.pass->m_executeCount;
                    });
                return;
            }

            struct PassData
            {
                RHIBufferRef buffer{};
                RGBufferHandle handle{};
                TestLegacyPass* pass = nullptr;
            };

            graph.AddPass<PassData>(
                m_name.c_str(),
                RenderGraphPassType::Graphics,
                [this](RenderGraphBuilder& builder, PassData& data)
                {
                    data.buffer = m_buffer;
                    data.handle = builder.ImportBuffer(
                        data.buffer, RHIResourceState::Common);
                    data.handle = builder.Write(
                        data.handle, RHIResourceState::UnorderedAccess);
                    builder.SetExportState(
                        data.handle, RHIResourceState::ShaderResource);
                    data.pass = this;
                },
                [](const PassData& data, RHICommandContext&)
                {
                    ++data.pass->m_executeCount;
                });
        }

        void Setup(RenderGraphBuilder&, const ViewData&) override {}
        void Execute(RHICommandContext&, const ViewData&) override {}
        void OnAdd(IRHIDevice*) override
        {
            if (m_lifecycle != nullptr)
            {
                ++m_lifecycle->onAddCount;
            }
        }
        void OnRemove() override
        {
            if (m_lifecycle != nullptr)
            {
                ++m_lifecycle->onRemoveCount;
            }
        }

        [[nodiscard]] uint32 GetExecuteCount() const noexcept
        {
            return m_executeCount;
        }

        [[nodiscard]] bool ReceivedMatchingContext() const noexcept
        {
            return m_receivedMatchingContext;
        }

    private:
        std::string m_name;
        int32 m_priority = 0;
        bool m_requestedEnabled = false;
        bool m_supported = false;
        bool m_enabled = false;
        bool m_recordsResourceUsage = true;
        std::shared_ptr<TestLegacyPassLifecycle> m_lifecycle;
        std::string m_unsupportedReason = "Test unsupported legacy pass";
        RHIBufferRef m_buffer{};
        uint32 m_executeCount = 0;
        bool m_receivedMatchingContext = false;
    };

    void AddTestHybridRayTracingPass(RenderGraph& graph,
                                     const RenderPassRecordContext& context,
                                     const char* name)
    {
        struct PassData
        {
            bool contextMatches = false;
        };
        graph.AddPass<PassData>(
            name, RenderGraphPassType::Graphics,
            [&graph, context](RenderGraphBuilder&, PassData& data)
            {
                data.contextMatches = context.MatchesTargetGraph(graph);
            },
            [](const PassData&, RHICommandContext&) {});
    }

    class TestRayTracedReflectionPass final : public RayTracedReflectionPass
    {
    public:
        const char* GetName() const override
        {
            return "RayTracedReflectionPass";
        }

        bool IsRequestedEnabled() const override { return true; }
        bool IsSupported() const override { return true; }
        bool IsEnabled() const override { return true; }

        void AddToGraph(RenderGraph& graph,
                        const RenderPassRecordContext& context) override
        {
            AddTestHybridRayTracingPass(graph, context, GetName());
        }
    };

    class TestRayTracedReflectionDenoisePass final
        : public RayTracedReflectionDenoisePass
    {
    public:
        const char* GetName() const override
        {
            return "RayTracedReflectionDenoisePass";
        }

        bool IsRequestedEnabled() const override { return true; }
        bool IsSupported() const override { return true; }
        bool IsEnabled() const override { return true; }

        void AddToGraph(RenderGraph& graph,
                        const RenderPassRecordContext& context) override
        {
            AddTestHybridRayTracingPass(graph, context, GetName());
        }
    };

    class TestRayTracedReflectionCompositePass final
        : public RayTracedReflectionCompositePass
    {
    public:
        const char* GetName() const override
        {
            return "RayTracedReflectionCompositePass";
        }

        bool IsRequestedEnabled() const override { return true; }
        bool IsSupported() const override { return true; }
        bool IsEnabled() const override { return true; }

        void AddToGraph(RenderGraph& graph,
                        const RenderPassRecordContext& context) override
        {
            AddTestHybridRayTracingPass(graph, context, GetName());
        }
    };

    class TestCameraVelocityPass final : public CameraVelocityPass
    {
    public:
        bool IsRequestedEnabled() const override { return true; }
        bool IsSupported() const override { return true; }
        bool IsEnabled() const override { return true; }

        void AddToGraph(RenderGraph& graph,
                        const RenderPassRecordContext& context) override
        {
            m_receivedMatchingContext = context.MatchesTargetGraph(graph);
            AddTestHybridRayTracingPass(graph, context, GetName());
        }

        [[nodiscard]] bool ReceivedMatchingContext() const noexcept
        {
            return m_receivedMatchingContext;
        }

    private:
        bool m_receivedMatchingContext = false;
    };

    class TestObjectVelocityPass final : public ObjectVelocityPass
    {
    public:
        explicit TestObjectVelocityPass(bool supported = true) noexcept
            : m_supported(supported)
        {
        }

        bool IsRequestedEnabled() const override { return true; }
        bool IsSupported() const override { return m_supported; }
        bool IsEnabled() const override { return m_supported; }

        void AddToGraph(RenderGraph& graph,
                        const RenderPassRecordContext& context) override
        {
            m_receivedMatchingContext = context.MatchesTargetGraph(graph);
            AddTestHybridRayTracingPass(graph, context, GetName());
        }

        [[nodiscard]] bool ReceivedMatchingContext() const noexcept
        {
            return m_receivedMatchingContext;
        }

    private:
        bool m_supported = true;
        bool m_receivedMatchingContext = false;
    };

    /**
     * @brief Minimal supported ToneMapping implementation for adapter receipt
     *        lifecycle tests.
     *
     * The test only records graph work and feeds registry-equivalent final
     * evidence; it intentionally never realizes or dereferences the stack's
     * device token.
     */
    class TestPostProcessToneMappingPass final : public IPostProcessPass
    {
    public:
        explicit TestPostProcessToneMappingPass(
            PostProcessColorDomain outputDomain) noexcept
            : m_outputDomain(outputDomain)
        {
        }

        [[nodiscard]] PostProcessPassDescriptor Describe() const override
        {
            const bool displayLinear =
                m_outputDomain == PostProcessColorDomain::DisplayLinearLDR;
            return {PostProcessPassId::ToneMapping,
                    "ToneMapping",
                    PostProcessStage::ToneMap,
                    900,
                    PostProcessColorDomain::SceneLinearHDR,
                    m_outputDomain,
                    displayLinear
                        ? PostProcessTransfer::SceneLinearToDisplayLinear
                        : PostProcessTransfer::SceneLinearToDisplayEncoded};
        }

        void Configure(const PostProcessSettings&) override {}

        void AddToGraph(RenderGraph& graph,
                        RGTextureHandle input,
                        RGTextureHandle output) override
        {
            struct PassData
            {
                RGTextureHandle input{};
                RGTextureHandle output{};
            };

            graph.AddPass<PassData>(
                "TestPostProcessToneMapping",
                RenderGraphPassType::Graphics,
                [input, output](RenderGraphBuilder& builder, PassData& data)
                {
                    data.input = builder.Read(
                        input, RHIResourceState::ShaderResource);
                    data.output = builder.Write(
                        output, RHIResourceState::RenderTarget);
                },
                [](const PassData&, RHICommandContext&) {});
        }

    private:
        PostProcessColorDomain m_outputDomain =
            PostProcessColorDomain::DisplayEncodedLDR;
    };

    class FakeRenderFeature final : public IRenderFeature
    {
    public:
        explicit FakeRenderFeature(RenderFeatureDescriptor descriptor,
                                   RenderFeatureReadiness readiness = {})
            : m_descriptor(std::move(descriptor))
            , m_readiness(std::move(readiness))
        {
        }

        [[nodiscard]] RenderFeatureDescriptor Describe() const override
        {
            return m_descriptor;
        }

        void Prepare(const RenderFeatureContext& context) override
        {
            ++m_prepareCount;
            m_lastContext = context;
        }

        [[nodiscard]] RenderFeatureReadiness ResolveReadiness() const override
        {
            return m_readiness;
        }

        void AddPasses(RenderFeatureGraphBuilder& builder,
                       RenderGraphBlackboard& blackboard) override
        {
            ++m_addPassesCount;
            if (m_addCulledPass)
            {
                struct CulledPassData
                {
                    RGTextureHandle output{};
                };
                const RGTextureHandle unusedOutput = builder.CreateTexture(
                    RHITextureDesc::RenderTarget(
                        4, 4, RHIFormat::RGBA8_UNORM));
                static_cast<void>(builder.AddPass<CulledPassData>(
                    "CulledDataOnlyCandidate",
                    RenderGraphPassType::Graphics,
                    [unusedOutput](
                        RenderGraphBuilder& graphBuilder,
                        CulledPassData& data)
                    {
                        data.output = graphBuilder.Write(
                            unusedOutput, RHIResourceState::RenderTarget);
                    },
                    [](const CulledPassData&, RenderGraphPassContext&) {}));
            }
            if (m_addSemanticPass)
            {
                struct SemanticPassData
                {
                    RHIBufferRef buffer{};
                    RGBufferHandle handle{};
                    bool failExecution = false;
                };
                static_cast<void>(builder.AddPass<SemanticPassData>(
                    "SemanticEvidence",
                    RenderGraphPassType::Graphics,
                    [buffer = m_semanticBuffer,
                     failExecution = m_failSemanticPass](
                        RenderGraphBuilder& graphBuilder,
                        SemanticPassData& data)
                    {
                        data.buffer = buffer;
                        data.failExecution = failExecution;
                        data.handle = graphBuilder.ImportBuffer(
                            data.buffer, RHIResourceState::Common);
                        data.handle = graphBuilder.Write(
                            data.handle, RHIResourceState::UnorderedAccess);
                        graphBuilder.SetExportState(
                            data.handle, RHIResourceState::ShaderResource);
                    },
                    [](const SemanticPassData& data,
                       RenderGraphPassContext& context)
                    {
                        if (data.failExecution)
                        {
                            context.MarkExecutionFailed(
                                "required semantic workload was not recorded");
                        }
                    }));
            }
            if (m_publishSceneTextures)
            {
                SceneTextures outputs;
                outputs.provenance = blackboard.GetProvenance();
                m_lastBlackboardCode = blackboard.Publish<SceneTextures>(
                    m_descriptor.featureId, m_descriptor, std::move(outputs));
            }
            if (m_consumeSceneTextures)
            {
                RenderGraphBlackboardCode code =
                    RenderGraphBlackboardCode::Success;
                static_cast<void>(blackboard.Require<SceneTextures>(
                    m_descriptor.featureId, m_descriptor, &code));
                m_lastBlackboardCode = code;
            }
            if (m_replaceReadinessAfterAddPasses)
            {
                m_readiness = m_readinessAfterAddPasses;
            }
        }

        void OnGraphExecutionFinalized(
            const RenderFeatureGraphExecutionEvidence& evidence) override
        {
            ++m_executionEvidenceCount;
            m_lastExecutionEvidence = evidence;
        }

        [[nodiscard]] RenderFeatureExecutionReceipt CollectReceipt() const override
        {
            RenderFeatureExecutionReceipt receipt;
            receipt.readiness = m_readiness;
            receipt.readiness.executed = m_claimExecutionInReceipt;
            receipt.graphPassCount = m_addPassesCount;
            receipt.executedGraphPassCount = 0;
            receipt.diagnostic = m_readiness.diagnostic;
            return receipt;
        }

        [[nodiscard]] uint32 GetPrepareCount() const noexcept
        {
            return m_prepareCount;
        }

        [[nodiscard]] uint32 GetAddPassesCount() const noexcept
        {
            return m_addPassesCount;
        }

        [[nodiscard]] const RenderFeatureContext& GetLastContext() const noexcept
        {
            return m_lastContext;
        }

        [[nodiscard]] uint32 GetExecutionEvidenceCount() const noexcept
        {
            return m_executionEvidenceCount;
        }

        [[nodiscard]] const RenderFeatureGraphExecutionEvidence&
        GetLastExecutionEvidence() const noexcept
        {
            return m_lastExecutionEvidence;
        }

        void SetReadinessAfterAddPasses(RenderFeatureReadiness readiness)
        {
            m_readinessAfterAddPasses = std::move(readiness);
            m_replaceReadinessAfterAddPasses = true;
        }

        void SetPublishSceneTextures(bool enabled = true) noexcept
        {
            m_publishSceneTextures = enabled;
        }

        void SetConsumeSceneTextures(bool enabled = true) noexcept
        {
            m_consumeSceneTextures = enabled;
        }

        void SetClaimExecutionInReceipt(bool enabled = true) noexcept
        {
            m_claimExecutionInReceipt = enabled;
        }

        void SetSemanticPass(bool failExecution) noexcept
        {
            m_addSemanticPass = true;
            m_failSemanticPass = failExecution;
        }

        void SetCulledPass(bool enabled = true) noexcept
        {
            m_addCulledPass = enabled;
        }

    private:
        RenderFeatureDescriptor m_descriptor{};
        RenderFeatureReadiness m_readiness{};
        RenderFeatureReadiness m_readinessAfterAddPasses{};
        RenderFeatureContext m_lastContext{};
        uint32 m_prepareCount = 0;
        uint32 m_addPassesCount = 0;
        RenderGraphBlackboardCode m_lastBlackboardCode =
            RenderGraphBlackboardCode::Success;
        bool m_publishSceneTextures = false;
        bool m_consumeSceneTextures = false;
        bool m_replaceReadinessAfterAddPasses = false;
        bool m_claimExecutionInReceipt = false;
        bool m_addCulledPass = false;
        bool m_addSemanticPass = false;
        bool m_failSemanticPass = false;
        RHIBufferRef m_semanticBuffer{new TestBuffer()};
        uint32 m_executionEvidenceCount = 0;
        RenderFeatureGraphExecutionEvidence m_lastExecutionEvidence{};
    };

    [[nodiscard]] RenderFeatureDescriptor MakeDescriptor(
        uint32 featureId,
        std::string name,
        RenderFeaturePhase phase = RenderFeaturePhase::FrameSetup)
    {
        RenderFeatureDescriptor descriptor;
        descriptor.featureId = RenderFeatureId{featureId};
        descriptor.name = std::move(name);
        descriptor.phase = phase;
        descriptor.requiredRecipes =
            ToMask(RenderPipelineRecipe::ForwardStandard);
        return descriptor;
    }

    [[nodiscard]] RenderFeatureReadiness MakeReadyReadiness()
    {
        RenderFeatureReadiness readiness;
        readiness.compiled = true;
        readiness.hardwareSupported = true;
        readiness.implementationSupported = true;
        readiness.pipelineReady = true;
        readiness.inputsReady = true;
        return readiness;
    }

    [[nodiscard]] RenderViewSnapshot MakeValidViewSnapshot(
        RenderViewIdentity identity,
        float64 absoluteTime,
        float32 deltaTime)
    {
        RenderViewSnapshot view;
        view.identity = identity;
        view.cameraCutRevision = 1;
        view.viewportWidth = 320;
        view.viewportHeight = 180;
        view.absoluteTime = absoluteTime;
        view.deltaTime = deltaTime;
        return view;
    }

    [[nodiscard]] RenderFeatureContext MakeValidContext(
        RenderPipelineRecipe recipe = RenderPipelineRecipe::ForwardStandard)
    {
        RenderFeatureContext context;
        context.frameIdentity = RenderFrameIdentity{1, 2, 3, 4};
        context.frameStamp = RenderFrameStamp{1, 0.0, 1.0f / 60.0f};
        context.effectTime = EffectFrameTime{
            1, 1.0f / 60.0f, 1.0 / 60.0, 1013};
        context.outputExtent = RenderOutputExtent{320, 180};
        context.viewFamilyId = 7;
        context.primaryViewIndex = 0;
        context.viewCount = 1;
        context.viewIdentity = RenderViewIdentity{5, 6, 0, ViewRole::Main};
        context.recordingViewOrdinal = 0;
        context.recordEpoch = 1;
        context.backend = RHIBackendType::DX12;
        context.pipeline.recipe = recipe;
        context.pipeline.quality = RenderQualityTier::Medium;
        context.hardwareCapabilities = ToMask(RenderFeatureCapability::Graphics);
        auto viewFamily = std::make_shared<RenderViewFamilySnapshot>();
        viewFamily->frame = context.frameStamp;
        viewFamily->identity = context.frameIdentity;
        viewFamily->effectTime = context.effectTime;
        viewFamily->extent = context.outputExtent;
        viewFamily->familyId = context.viewFamilyId;
        viewFamily->primaryViewIndex = context.primaryViewIndex;
        viewFamily->views.push_back(MakeValidViewSnapshot(
            context.viewIdentity,
            context.frameStamp.absoluteTimeSeconds,
            context.frameStamp.deltaTimeSeconds));
        context.viewFamily = std::move(viewFamily);
        return context;
    }

    [[nodiscard]] RenderFeatureSnapshot MakeCompleteFeatureSnapshot(
        uint64 sequence = 1)
    {
        RenderFeatureSnapshot snapshot;
        snapshot.BeginBuild(sequence);
        snapshot.MarkComplete();
        return snapshot;
    }

    [[nodiscard]] RenderFeatureSnapshotView MakeSnapshotInputs(
        RenderFeatureSnapshot snapshot,
        RenderFeatureProducerEndpointId producer =
            RenderFeatureProducerEndpointId::SceneFeatureSnapshotStore,
        RenderFeatureConsumerEndpointId consumer =
            RenderFeatureConsumerEndpointId::RenderSceneDatabase)
    {
        return RenderFeatureSnapshotView(
            std::make_shared<const RenderFeatureSnapshot>(std::move(snapshot)),
            std::make_shared<const RenderFeatureChannelRegistry>(
                CreateBuiltinRenderFeatureChannelRegistry()),
            producer,
            consumer);
    }

    [[nodiscard]] std::shared_ptr<const LightingFramePlan>
    MakeValidLightingPlan();

    [[nodiscard]] RenderPassRecordContext MakeValidRecordContext(
        RenderGraph& graph)
    {
        ViewData view;
        view.frameNumber = 1;
        view.viewportWidth = 320;
        view.viewportHeight = 180;
        view.colorTarget.index = 1;
        view.colorTarget.graphIdentity = graph.GetGraphIdentity();
        view.colorTarget.recordingGeneration = graph.GetRecordingGeneration();
        view.depthTarget.index = 4;
        view.depthTarget.graphIdentity = graph.GetGraphIdentity();
        view.depthTarget.recordingGeneration = graph.GetRecordingGeneration();
        view.reactiveMaskTarget.index = 2;
        view.reactiveMaskTarget.graphIdentity = graph.GetGraphIdentity();
        view.reactiveMaskTarget.recordingGeneration =
            graph.GetRecordingGeneration();
        view.exposureState.index = 3;
        view.exposureState.graphIdentity = graph.GetGraphIdentity();
        view.exposureState.recordingGeneration = graph.GetRecordingGeneration();
        view.exposureStateSchemaVersion =
            SceneTextures::ExposureStateSchemaVersion;
        view.exposureStateStride = SceneTextures::ExposureStateStride;
        view.exposureStateMode = RenderExposureMode::ManualEV100;
        view.exposureStateConfigurationHash = 0xE1205EULL;
        RenderPassRecordContext context =
            MakeRenderPassRecordContext(graph, view);
        context.featureFrameIdentity = RenderFrameIdentity{1, 2, 3, 4};
        context.featureViewIdentity =
            RenderViewIdentity{5, 6, 0, ViewRole::Main};
        context.lightingPlan = MakeValidLightingPlan();
        if (context.lightingPlan)
        {
            LocalShadowFramePlanRequest localRequest;
            localRequest.atlas = LocalShadowAtlasConfig{
                64, 8, 1, 2,
                LocalShadowOverflowPolicy::StableUnshadowedFallback};
            localRequest.atlasGeneration = 1;
            localRequest.expectedLightingPlanIdentity =
                context.lightingPlan->GetIdentityHash();
            const LocalShadowFramePlanBuildResult localResult =
                LocalShadowFramePlanBuilder{}.Build(
                    *context.lightingPlan, localRequest);
            if (localResult.Succeeded())
            {
                LocalShadowAtlasPlanRequest atlasRequest;
                atlasRequest.atlasGeneration = 1;
                atlasRequest.expectedLocalShadowPlanIdentity =
                    localResult.plan.GetIdentityHash();
                const LocalShadowAtlasPlanBuildResult atlasResult =
                    LocalShadowAtlasPlanBuilder{}.Build(
                        *context.lightingPlan, localResult.plan,
                        atlasRequest);
                if (atlasResult.Succeeded())
                {
                    context.localShadowAtlasPlan =
                        std::make_shared<const LocalShadowAtlasPlan>(
                            atlasResult.plan);
                    context.localShadowPass =
                        std::make_shared<LocalShadowPass>();
                }
            }
        }
        context.results = std::make_shared<RenderPassRecordResults>();
        context.results->identity = context.identity;
        context.results->localShadowOutput.identity = context.identity;
        context.results->localShadowOutput.frameIdentity =
            context.featureFrameIdentity;
        context.results->localShadowOutput.viewIdentity =
            context.featureViewIdentity;
        context.results->localShadowOutput.lightingPlanIdentity =
            context.lightingPlan
                ? context.lightingPlan->GetIdentityHash() : 0;
        return context;
    }

    [[nodiscard]] RGTextureHandle MakeVelocityTarget(
        RenderGraph& graph,
        RHIFormat format = SceneTextures::VelocityContract.format)
    {
        RHITextureDesc desc = RHITextureDesc::RenderTarget(
            320, 180, format);
        desc.sampleCount = SceneTextures::VelocityContract.sampleCount;
        desc.debugName = "RenderFeatureValidationVelocity";
        return graph.CreateTexture(desc);
    }

    [[nodiscard]] std::shared_ptr<const LightingFramePlan>
    MakeValidLightingPlan()
    {
        LightingFramePlanRequest request;
        request.frameIdentity = RenderFrameIdentity{1, 2, 3, 4};
        request.viewIdentity = RenderViewIdentity{5, 6, 0, ViewRole::Main};
        request.sceneLightStateHash = 7;
        request.lightingConfigurationRevision = 8;
        request.qualityRevision = 9;
        request.quality = RenderQualityTier::Medium;
        request.pointLightCapacity = 1;
        request.spotLightCapacity = 1;

        const LightingFramePlanBuildResult result =
            LightingFramePlanBuilder{}.Build(request);
        return result.Succeeded()
            ? std::make_shared<const LightingFramePlan>(result.plan)
            : nullptr;
    }

    [[nodiscard]] std::shared_ptr<const ReflectionFramePlan>
    MakeValidReflectionPlan(const RenderFeatureContext& context,
                            uint64 lightingPlanIdentity,
                            bool requestSSR = false)
    {
        ReflectionFramePlanRequest request;
        request.frameIdentity = context.frameIdentity;
        request.viewIdentity = context.viewIdentity;
        request.sceneProbeStateHash = 0x5245464C45435431ULL;
        request.reflectionConfigurationRevision = 0x5245464C45435432ULL;
        request.viewPosition = Vec3(0.0f);
        request.viewProjection = Mat4Identity();
        request.viewCullingMask = 1U;
        request.ssr.requested = requestSSR;
        if (requestSSR)
        {
            request.ssr.inputsReady = true;
            request.ssr.configurationIdentityHash =
                0x5245464C45435433ULL;
            request.ssr.pipelineIdentityHash =
                0x5245464C45435434ULL;
            request.ssr.depthPyramidIdentityHash =
                0x5245464C45435435ULL;
        }
        request.ibl.source = ReflectionIBLSource::Analytic;
        request.ibl.lightingPlanIdentityHash = lightingPlanIdentity;
        request.ibl.analyticFallbackIdentityHash =
            0x5245464C45435436ULL;
        request.fallback.configurationIdentityHash =
            0x5245464C45435437ULL;

        ReflectionFramePlanBuildResult result =
            ReflectionFramePlanBuilder{}.Build(request);
        return result.Succeeded() && result.plan.IsValid()
            ? std::make_shared<const ReflectionFramePlan>(std::move(result.plan))
            : nullptr;
    }

    [[nodiscard]] HybridRayTracingFramePlan MakeValidHybridRayTracingPlan(
        const RenderFeatureContext& context)
    {
        HybridRayTracingFramePlanRequest request;
        request.frameIdentity = context.frameIdentity;
        request.viewIdentity = context.viewIdentity;
        request.reflectionHistoryReset = RenderHistoryResetReason::FirstFrame;
        request.outputExtent = context.outputExtent;
        request.recipe = RenderPipelineRecipe::HybridRayTracing;
        request.quality = context.pipeline.quality;
        request.profileHash = ComputeRenderPipelineProfileHash(
            request.recipe, request.quality);
        request.rayTracingScenePreparationGeneration = 1;
        request.rayTracingSceneStructuralHash = 2;
        request.shadowRequirement = HybridRayTracingRequirement::Required;
        request.reflectionRequirement = HybridRayTracingRequirement::Required;
        request.shadow = {
            true, true, true, true, true, true, true, {}};
        request.reflection = {
            true, true, true, true, true, true, true, {}};
        return HybridRayTracingFramePlan::Build(request);
    }

    [[nodiscard]] std::unique_ptr<FakeRenderFeature> MakeFeature(
        RenderFeatureDescriptor descriptor,
        RenderFeatureReadiness readiness = MakeReadyReadiness())
    {
        return std::make_unique<FakeRenderFeature>(
            std::move(descriptor), std::move(readiness));
    }

    [[nodiscard]] RenderGraphBlackboardProvenance MakeProvenance(
        uint64 graphIdentity = 11,
        uint64 recordingGeneration = 12)
    {
        return RenderGraphBlackboardProvenance{
            graphIdentity,
            recordingGeneration,
            13,
            0,
            14};
    }

    [[nodiscard]] RGTextureHandle MakeTextureHandle(
        const RenderGraphBlackboardProvenance& provenance,
        uint32 index)
    {
        RGTextureHandle handle;
        handle.index = index;
        handle.graphIdentity = provenance.graphIdentity;
        handle.recordingGeneration = provenance.recordingGeneration;
        return handle;
    }

    [[nodiscard]] RGBufferHandle MakeBufferHandle(
        const RenderGraphBlackboardProvenance& provenance,
        uint32 index)
    {
        RGBufferHandle handle;
        handle.index = index;
        handle.graphIdentity = provenance.graphIdentity;
        handle.recordingGeneration = provenance.recordingGeneration;
        return handle;
    }

    [[nodiscard]] SceneTextures::Metadata MakeSceneTextureMetadata(
        SceneTextures::FormatContract contract,
        RenderOutputExtent extent = RenderOutputExtent{320, 180})
    {
        return SceneTextures::Metadata{
            extent, contract.format, contract.sampleCount};
    }

    [[nodiscard]] SceneTextures MakeDeferredSceneTextures(
        const RenderGraphBlackboardProvenance& provenance = MakeProvenance())
    {
        SceneTextures textures;
        textures.provenance = provenance;
        textures.hdrSceneColor = MakeTextureHandle(provenance, 1);
        textures.hdrSceneColorMetadata = MakeSceneTextureMetadata(
            SceneTextures::HDRSceneColorContract);
        textures.deviceDepth = MakeTextureHandle(provenance, 2);
        textures.deviceDepthMetadata = MakeSceneTextureMetadata(
            SceneTextures::DeviceDepthContract);
        textures.velocity = MakeTextureHandle(provenance, 3);
        textures.velocityMetadata = MakeSceneTextureMetadata(
            SceneTextures::VelocityContract);
        textures.gBuffer0 = MakeTextureHandle(provenance, 4);
        textures.gBuffer0Metadata = MakeSceneTextureMetadata(
            SceneTextures::GBuffer0Contract);
        textures.gBuffer1 = MakeTextureHandle(provenance, 5);
        textures.gBuffer1Metadata = MakeSceneTextureMetadata(
            SceneTextures::GBuffer1Contract);
        textures.gBuffer2 = MakeTextureHandle(provenance, 6);
        textures.gBuffer2Metadata = MakeSceneTextureMetadata(
            SceneTextures::GBuffer2Contract);
        textures.gBuffer3 = MakeTextureHandle(provenance, 7);
        textures.gBuffer3Metadata = MakeSceneTextureMetadata(
            SceneTextures::GBuffer3Contract);
        textures.materialTable = MakeBufferHandle(provenance, 8);
        textures.materialTableMetadata = {
            PostProcessMaterialTableMetadata::SchemaVersion,
            static_cast<uint32>(sizeof(MaterialGPUConstants)),
            1,
            0,
            0x4D41545441424C45ull,
            textures.gBuffer3Metadata.extent,
            textures.gBuffer3Metadata.sampleCount};
        textures.exposureState = MakeBufferHandle(provenance, 9);
        textures.exposureStateMetadata = {
            SceneTextures::ExposureStateSchemaVersion,
            SceneTextures::ExposureStateStride,
            RenderExposureMode::ManualEV100,
            0xE1205EULL};
        return textures;
    }

    [[nodiscard]] LightingOutputs MakeLightingOutputs(
        const RenderGraphBlackboardProvenance& provenance,
        uint64 lightingPlanIdentity)
    {
        LightingOutputs outputs;
        outputs.provenance = provenance;
        outputs.frameIdentity = RenderFrameIdentity{
            provenance.frameSequence, 2, 3, 4};
        outputs.viewIdentity = RenderViewIdentity{
            5, 6, provenance.viewOrdinal, ViewRole::Main};
        outputs.lightingPlanIdentity = lightingPlanIdentity;
        outputs.hdrSceneColor = MakeTextureHandle(provenance, 1);
        outputs.hdrSceneColorMetadata = MakeSceneTextureMetadata(
            SceneTextures::HDRSceneColorContract);
        outputs.clusteredLights = MakeBufferHandle(provenance, 2);
        outputs.baseIndirectSpecular = MakeTextureHandle(provenance, 3);
        outputs.baseIndirectSpecularMetadata = MakeSceneTextureMetadata(
            SceneTextures::BaseIndirectSpecularContract);
        outputs.specularResponse = MakeTextureHandle(provenance, 4);
        outputs.specularResponseMetadata = MakeSceneTextureMetadata(
            SceneTextures::SpecularResponseContract);
        outputs.exposureState = MakeBufferHandle(provenance, 5);
        outputs.exposureStateMetadata = {
            SceneTextures::ExposureStateSchemaVersion,
            SceneTextures::ExposureStateStride,
            RenderExposureMode::ManualEV100,
            0xE1205EULL};
        outputs.exposureStateContentHash = 0xE1205F00ULL;
        return outputs;
    }

    [[nodiscard]] ReflectionOutputs MakeReflectionOutputs(
        const RenderGraphBlackboardProvenance& provenance,
        uint64 lightingPlanIdentity)
    {
        ReflectionOutputs outputs;
        outputs.provenance = provenance;
        outputs.frameIdentity = RenderFrameIdentity{
            provenance.frameSequence, 2, 3, 4};
        outputs.viewIdentity = RenderViewIdentity{
            5, 6, provenance.viewOrdinal, ViewRole::Main};
        outputs.lightingPlanIdentity = lightingPlanIdentity;
        outputs.resolvedSceneColor = MakeTextureHandle(provenance, 1);
        outputs.resolvedSceneColorMetadata = MakeSceneTextureMetadata(
            SceneTextures::HDRSceneColorContract);
        outputs.imageBasedLightingPreserved = true;
        return outputs;
    }

    [[nodiscard]] FroxelGridMetadata MakeValidFroxelGrid(
        RenderOutputExtent extent = RenderOutputExtent{320, 180})
    {
        const FroxelGridResolution resolution = ResolveFroxelGridMetadata(
            extent, RenderQualityTier::Medium);
        return resolution.metadata;
    }

    [[nodiscard]] RenderViewHistoryIdentity MakeVolumetricHistoryIdentity(
        const RenderGraphBlackboardProvenance& provenance,
        RenderOutputExtent extent = RenderOutputExtent{320, 180})
    {
        return {
            RenderFrameIdentity{provenance.frameSequence, 2, 3, 4},
            RenderViewIdentity{5, 6, provenance.viewOrdinal, ViewRole::Main},
            extent,
            7,
            2,
            1,
            1,
            3,
            1};
    }

    [[nodiscard]] FroxelVolumeMetadata MakeFroxelVolumeMetadata(
        const FroxelGridMetadata& grid)
    {
        return {
            RHITextureDimension::Texture3D,
            RHIFormat::RGBA16_FLOAT,
            RHISampleCount::Count1,
            grid.froxelWidth,
            grid.froxelHeight,
            grid.depthSliceCount};
    }

    [[nodiscard]] VolumetricLightingOutputs MakeVolumetricLightingOutputs(
        const RenderGraphBlackboardProvenance& provenance,
        bool identityRoute = false)
    {
        VolumetricLightingOutputs outputs;
        outputs.provenance = provenance;
        outputs.frameIdentity = RenderFrameIdentity{
            provenance.frameSequence, 2, 3, 4};
        outputs.viewIdentity = RenderViewIdentity{
            5, 6, provenance.viewOrdinal, ViewRole::Main};
        outputs.historyIdentity = MakeVolumetricHistoryIdentity(provenance);
        outputs.historyResetReasons = RenderHistoryResetReason::FirstFrame;
        outputs.lightingPlanIdentity = 0x4C49474854504C4EULL;
        outputs.volumetricPlanIdentity = 0x564F4C554D455452ULL;
        outputs.settingsHash = 0x564F4C5345545449ULL;
        outputs.froxelGrid = MakeValidFroxelGrid();
        outputs.sourceSceneColor = MakeTextureHandle(provenance, 1);
        outputs.sourceSceneColorMetadata = MakeSceneTextureMetadata(
            SceneTextures::HDRSceneColorContract);
        outputs.resolvedSceneColor = outputs.sourceSceneColor;
        outputs.resolvedSceneColorMetadata =
            outputs.sourceSceneColorMetadata;
        outputs.worldNormal = MakeTextureHandle(provenance, 3);
        outputs.worldNormalMetadata = MakeSceneTextureMetadata(
            SceneTextures::FormatContract{
                RHIFormat::RGBA8_UNORM, RHISampleCount::Count1});
        outputs.materialIdentity = MakeTextureHandle(provenance, 4);
        outputs.materialIdentityMetadata = MakeSceneTextureMetadata(
            SceneTextures::GBuffer3Contract);
        outputs.materialParameterTable = MakeBufferHandle(provenance, 5);
        outputs.materialParameterTableMetadata = {
            PostProcessMaterialTableMetadata::SchemaVersion,
            static_cast<uint32>(sizeof(MaterialGPUConstants)),
            1,
            0,
            0x4D41545441424C45ULL,
            outputs.resolvedSceneColorMetadata.extent,
            RHISampleCount::Count1};
        outputs.identityRoute = identityRoute;
        if (!identityRoute)
        {
            outputs.integratedScatteringTransmittance =
                MakeTextureHandle(provenance, 2);
            outputs.integratedScatteringTransmittanceMetadata =
                MakeFroxelVolumeMetadata(outputs.froxelGrid);
        }
        return outputs;
    }

    class DataOnlyLightingOutputsFeature final : public IRenderFeature
    {
    public:
        explicit DataOnlyLightingOutputsFeature(
            uint64 lightingPlanIdentity) noexcept
            : m_lightingPlanIdentity(lightingPlanIdentity)
        {
        }

        [[nodiscard]] RenderFeatureDescriptor Describe() const override
        {
            RenderFeatureDescriptor descriptor;
            descriptor.featureId = BuiltinRenderFeatureIds::ForwardLighting;
            descriptor.name = "DataOnlyLightingOutputs";
            descriptor.phase = RenderFeaturePhase::Lighting;
            descriptor.producedResources = {LightingOutputsContract};
            descriptor.requiredCapabilities =
                ToMask(RenderFeatureCapability::Graphics);
            descriptor.requiredRecipes =
                ToMask(RenderPipelineRecipe::ForwardStandard);
            descriptor.qualityTiers = RVX_ALL_RENDER_QUALITY_TIERS;
            descriptor.fallbackPolicy = RenderFeatureFallbackPolicy::None;
            descriptor.allowsDataOnlyCompletion = true;
            return descriptor;
        }

        void Prepare(const RenderFeatureContext& context) override
        {
            m_context = context;
            m_readiness = MakeReadyReadiness();
        }

        [[nodiscard]] RenderFeatureReadiness ResolveReadiness() const override
        {
            return m_readiness;
        }

        void AddPasses(RenderFeatureGraphBuilder&,
                       RenderGraphBlackboard& blackboard) override
        {
            LightingOutputs outputs = MakeLightingOutputs(
                blackboard.GetProvenance(), m_lightingPlanIdentity);
            outputs.frameIdentity = m_context.frameIdentity;
            outputs.viewIdentity = m_context.viewIdentity;
            m_publishCode = blackboard.Publish(
                BuiltinRenderFeatureIds::ForwardLighting,
                Describe(), std::move(outputs));
        }

        [[nodiscard]] RenderFeatureExecutionReceipt CollectReceipt()
            const override
        {
            RenderFeatureExecutionReceipt receipt;
            receipt.featureId = BuiltinRenderFeatureIds::ForwardLighting;
            receipt.featureSchemaVersion = 1;
            receipt.phase = RenderFeaturePhase::Lighting;
            receipt.requirement = RenderFeatureRequirement::Required;
            receipt.selected = true;
            receipt.readiness = m_readiness;
            receipt.readiness.executed = false;
            return receipt;
        }

        [[nodiscard]] RenderGraphBlackboardCode GetPublishCode() const noexcept
        {
            return m_publishCode;
        }

    private:
        uint64 m_lightingPlanIdentity = 0;
        RenderFeatureContext m_context{};
        RenderFeatureReadiness m_readiness{};
        RenderGraphBlackboardCode m_publishCode =
            RenderGraphBlackboardCode::Success;
    };

    [[nodiscard]] AmbientOcclusionOutputs MakeAmbientOcclusionOutputs(
        const RenderGraphBlackboardProvenance& provenance)
    {
        AmbientOcclusionOutputs outputs;
        outputs.provenance = provenance;
        outputs.frameIdentity = RenderFrameIdentity{
            provenance.frameSequence, 2, 3, 4};
        outputs.viewIdentity = RenderViewIdentity{
            5, 6, provenance.viewOrdinal, ViewRole::Main};
        outputs.historyIdentity.previousFrame = outputs.frameIdentity;
        outputs.historyIdentity.view = outputs.viewIdentity;
        outputs.historyIdentity.extent = RenderOutputExtent{320, 180};
        outputs.historyIdentity.familyId = 7;
        outputs.historyIdentity.worldRevision = 8;
        outputs.historyIdentity.temporalEpoch = 9;
        outputs.historyIdentity.formatRevision = 10;
        outputs.historyIdentity.surfaceGeneration =
            outputs.frameIdentity.surfaceGeneration;
        outputs.historyIdentity.cameraCutRevision = 11;
        outputs.historyResetReasons = RenderHistoryResetReason::FirstFrame;
        outputs.settingsHash = CalculateRenderAmbientOcclusionSettingsHash({});
        outputs.consumerMask =
            RVX_RENDER_AMBIENT_OCCLUSION_INDIRECT_CONSUMER_MASK;
        outputs.ambientOcclusionFieldHash = 0xA013ULL;
        outputs.ambientOcclusion = MakeTextureHandle(provenance, 10);
        outputs.ambientOcclusionMetadata = MakeSceneTextureMetadata(
            AmbientOcclusionOutputs::VisibilityContract);
        return outputs;
    }

    [[nodiscard]] TranslucencyOutputs MakeTranslucencyOutputs(
        const RenderGraphBlackboardProvenance& provenance)
    {
        TranslucencyOutputs outputs;
        outputs.provenance = provenance;
        outputs.frameIdentity = RenderFrameIdentity{
            provenance.frameSequence, 2, 3, 4};
        outputs.viewIdentity = RenderViewIdentity{
            5, 6, provenance.viewOrdinal, ViewRole::Main};
        outputs.hdrSceneColor = MakeTextureHandle(provenance, 1);
        outputs.hdrSceneColorMetadata = MakeSceneTextureMetadata(
            SceneTextures::HDRSceneColorContract);
        outputs.opaqueSceneSnapshot = MakeTextureHandle(provenance, 5);
        outputs.opaqueSceneSnapshotMetadata = MakeSceneTextureMetadata(
            SceneTextures::HDRSceneColorContract);
        outputs.deviceDepth = MakeTextureHandle(provenance, 2);
        outputs.deviceDepthMetadata = MakeSceneTextureMetadata(
            SceneTextures::DeviceDepthContract);
        outputs.velocity = MakeTextureHandle(provenance, 3);
        outputs.velocityMetadata = MakeSceneTextureMetadata(
            SceneTextures::VelocityContract);
        outputs.exposureState = MakeBufferHandle(provenance, 4);
        outputs.exposureStateMetadata = {
            SceneTextures::ExposureStateSchemaVersion,
            SceneTextures::ExposureStateStride,
            RenderExposureMode::ManualEV100,
            0xE1205EULL};
        outputs.colorDomain = PostProcessColorDomain::SceneLinearHDR;
        outputs.transfer = PostProcessTransfer::None;
        return outputs;
    }

    [[nodiscard]] PreTranslucencyOutputs MakePreTranslucencyOutputs(
        const RenderGraphBlackboardProvenance& provenance)
    {
        PreTranslucencyOutputs outputs;
        outputs.provenance = provenance;
        outputs.frameIdentity = RenderFrameIdentity{
            provenance.frameSequence, 2, 3, 4};
        outputs.viewIdentity = RenderViewIdentity{
            5, 6, provenance.viewOrdinal, ViewRole::Main};
        outputs.lightingPlanIdentity = 0x4C49474854504C4EULL;
        outputs.hdrSceneColor = MakeTextureHandle(provenance, 1);
        outputs.hdrSceneColorMetadata = MakeSceneTextureMetadata(
            SceneTextures::HDRSceneColorContract);
        outputs.opaqueSceneSnapshot = MakeTextureHandle(provenance, 5);
        outputs.opaqueSceneSnapshotMetadata = MakeSceneTextureMetadata(
            SceneTextures::HDRSceneColorContract);
        outputs.opaqueSceneSnapshotReceipt =
            std::make_shared<OpaqueSceneSnapshotCopyReceipt>();
        outputs.exposureState = MakeBufferHandle(provenance, 4);
        outputs.exposureStateMetadata = {
            SceneTextures::ExposureStateSchemaVersion,
            SceneTextures::ExposureStateStride,
            RenderExposureMode::ManualEV100,
            0xE1205EULL};
        const VolumetricLightingOutputs volumetric =
            MakeVolumetricLightingOutputs(provenance, true);
        outputs.volumetricPlanIdentity = volumetric.volumetricPlanIdentity;
        outputs.froxelGrid = volumetric.froxelGrid;
        outputs.volumetricHistoryIdentity = volumetric.historyIdentity;
        outputs.volumetricHistoryResetReasons =
            volumetric.historyResetReasons;
        outputs.integratedScatteringTransmittance =
            volumetric.integratedScatteringTransmittance;
        outputs.integratedScatteringTransmittanceMetadata =
            volumetric.integratedScatteringTransmittanceMetadata;
        outputs.volumetricIdentityRoute = volumetric.identityRoute;
        outputs.volumetricTemporalHistoryUsed =
            volumetric.temporalHistoryUsed;
        outputs.colorDomain = PostProcessColorDomain::SceneLinearHDR;
        outputs.transfer = PostProcessTransfer::None;
        return outputs;
    }

    void PopulateValidManualExposureReceipt(
        RenderPostProcessExecutionReceipt& receipt,
        bool committed = false)
    {
        constexpr uint64 kExposureStateHash = 0xE1205EULL;
        receipt.exposureStateContentHash = kExposureStateHash;
        receipt.toneMapExposureStateHash = kExposureStateHash;
        receipt.exposureMode = RenderExposureMode::ManualEV100;
        receipt.exposureRequested = true;
        receipt.exposurePipelineReady = true;
        receipt.exposureInputsReady = true;
        receipt.exposureExecuted = true;
        receipt.exposureAdaptationPassCount = 1;
        receipt.exposureHistoryCandidateProduced = true;
        receipt.exposureHistoryCommitted = committed;
        receipt.exposureHistoryResetReasons =
            RenderHistoryResetReason::FirstFrame;
    }

    [[nodiscard]] PostProcessOutputs MakePostProcessOutputs(
        const RenderGraphBlackboardProvenance& provenance)
    {
        PostProcessOutputs outputs;
        outputs.provenance = provenance;
        outputs.frameIdentity = RenderFrameIdentity{
            provenance.frameSequence, 2, 3, 4};
        outputs.viewIdentity = RenderViewIdentity{
            5, 6, provenance.viewOrdinal, ViewRole::Main};
        outputs.presentableColor = MakeTextureHandle(provenance, 4);
        outputs.presentableColorMetadata = MakeSceneTextureMetadata(
            SceneTextures::FormatContract{
                RHIFormat::RGBA8_UNORM_SRGB, RHISampleCount::Count1});
        outputs.colorDomain = PostProcessColorDomain::DisplayEncodedLDR;
        outputs.transfer = PostProcessTransfer::SceneLinearToDisplayEncoded;
        return outputs;
    }

    class PostProcessAdapterTestFixture final
    {
    public:
        explicit PostProcessAdapterTestFixture(RHIFormat presentableFormat)
            : m_context(MakeValidContext())
            , m_adapter(&m_stack)
        {
            // PostProcessStack currently uses device presence solely as its
            // initialization lifetime token. The test effect has no device
            // dependencies and the token is never dereferenced.
            m_stack.Initialize(reinterpret_cast<IRHIDevice*>(
                static_cast<std::uintptr_t>(1)));
            m_stack.AddEffect<TestPostProcessToneMappingPass>(
                IsSRGBFormat(presentableFormat)
                    ? PostProcessColorDomain::DisplayLinearLDR
                    : PostProcessColorDomain::DisplayEncodedLDR);
            PostProcessSettings noAntiAliasing;
            noAntiAliasing.antiAliasingMode = RenderAntiAliasingMode::None;
            m_stack.ApplySettings(noAntiAliasing);

            const RHITextureDesc sceneColorDesc = RHITextureDesc::RenderTarget(
                m_context.outputExtent.width,
                m_context.outputExtent.height,
                RHIFormat::RGBA16_FLOAT);
            const RHITextureDesc presentableColorDesc =
                RHITextureDesc::RenderTarget(
                    m_context.outputExtent.width,
                    m_context.outputExtent.height,
                    presentableFormat);
            m_sceneColor = m_graph.CreateTexture(sceneColorDesc);
            RHITextureDesc opaqueSnapshotDesc = RHITextureDesc::Texture2D(
                m_context.outputExtent.width,
                m_context.outputExtent.height,
                RHIFormat::RGBA16_FLOAT,
                RHITextureUsage::ShaderResource | RHITextureUsage::CopyDst);
            m_opaqueSceneSnapshot = m_graph.CreateTexture(opaqueSnapshotDesc);
            m_presentableColor = m_graph.CreateTexture(presentableColorDesc);
            m_exposureStateBuffer = RHIBufferRef(new TestBuffer(
                SceneTextures::ExposureStateStride,
                SceneTextures::ExposureStateStride));
            m_exposureState = m_graph.ImportBuffer(
                m_exposureStateBuffer, RHIResourceState::ShaderResource);

            m_provenance = {
                m_graph.GetGraphIdentity(),
                m_graph.GetRecordingGeneration(),
                m_context.frameIdentity.frameSequence,
                m_context.recordingViewOrdinal,
                m_context.recordEpoch};

            RenderPassRecordContext passContext =
                MakeValidRecordContext(m_graph);
            passContext.view.colorTarget = m_presentableColor;

            PostProcessPipelineRecordContext recordContext;
            recordContext.frameIdentity = m_context.frameIdentity;
            recordContext.viewIdentity = m_context.viewIdentity;
            recordContext.presentableColor = m_presentableColor;
            recordContext.presentableColorMetadata = MakeSceneTextureMetadata(
                SceneTextures::FormatContract{
                    presentableFormat, RHISampleCount::Count1},
                m_context.outputExtent);
            recordContext.frameInputs.exposureState = m_exposureState;
            recordContext.frameInputs.exposureStateContentHash = 0xE1205EULL;
            recordContext.frameInputs.outputFormat = presentableFormat;
            recordContext.frameInputs.frameIndex =
                m_context.frameIdentity.frameSequence;
            recordContext.frameInputs.fixedDeltaSeconds =
                m_context.frameStamp.deltaTimeSeconds;
            recordContext.frameInputs.historyIdentity = {
                m_context.frameIdentity,
                m_context.viewIdentity,
                m_context.outputExtent,
                m_context.viewFamilyId,
                m_context.frameIdentity.sceneRevision,
                1,
                1,
                m_context.frameIdentity.surfaceGeneration,
                1};
            recordContext.frameInputs.historyResetReasons =
                RenderHistoryResetReason::FirstFrame;
            recordContext.passRecordContext = std::move(passContext);
            m_adapter.SetRecordContext(std::move(recordContext));
            m_adapter.Prepare(m_context);
        }

        ~PostProcessAdapterTestFixture()
        {
            m_adapter.SetStack(nullptr);
            m_stack.Shutdown();
        }

        [[nodiscard]] bool Compose()
        {
            RenderFeatureGraphBuilder builder(m_graph, m_provenance);
            RenderGraphBlackboard blackboard(m_provenance);
            TranslucencyOutputs translucency;
            translucency.provenance = m_provenance;
            translucency.frameIdentity = m_context.frameIdentity;
            translucency.viewIdentity = m_context.viewIdentity;
            translucency.hdrSceneColor = m_sceneColor;
            translucency.hdrSceneColorMetadata = MakeSceneTextureMetadata(
                SceneTextures::HDRSceneColorContract,
                m_context.outputExtent);
            translucency.opaqueSceneSnapshot = m_opaqueSceneSnapshot;
            translucency.opaqueSceneSnapshotMetadata = MakeSceneTextureMetadata(
                SceneTextures::HDRSceneColorContract,
                m_context.outputExtent);
            translucency.exposureState = m_exposureState;
            translucency.exposureStateMetadata = {
                SceneTextures::ExposureStateSchemaVersion,
                SceneTextures::ExposureStateStride,
                RenderExposureMode::ManualEV100,
                CalculateRenderExposureSettingsHash(
                    m_stack.GetSettings().exposureSettings)};
            translucency.colorDomain = PostProcessColorDomain::SceneLinearHDR;
            translucency.transfer = PostProcessTransfer::None;
            if (blackboard.Publish(RenderFeatureId{71},
                                   MakeTranslucencyProducerDescriptor(),
                                   std::move(translucency)) !=
                RenderGraphBlackboardCode::Success)
            {
                return false;
            }

            m_adapter.AddPasses(builder, blackboard);
            return m_adapter.ResolveReadiness().IsReadyToExecute() &&
                   m_graph.GetRecordedPassCount() != 0;
        }

        [[nodiscard]] RenderFeatureGraphExecutionEvidence
        MakeEvidence(bool semanticExecutionSucceeded = true) const
        {
            return {BuiltinRenderFeatureIds::PostProcessPipeline,
                    m_graph.GetGraphIdentity(),
                    m_graph.GetRecordingGeneration(),
                    m_context.frameIdentity,
                    m_context.viewIdentity,
                    1,
                    1,
                    semanticExecutionSucceeded,
                    semanticExecutionSucceeded ? std::string() :
                        "test semantic failure"};
        }

        [[nodiscard]] PostProcessPipelineFeatureAdapter& Adapter() noexcept
        {
            return m_adapter;
        }

        [[nodiscard]] PostProcessStack& Stack() noexcept
        {
            return m_stack;
        }

    private:
        [[nodiscard]] static RenderFeatureDescriptor
        MakeTranslucencyProducerDescriptor()
        {
            RenderFeatureDescriptor descriptor = MakeDescriptor(
                71, "translucency", RenderFeaturePhase::Translucency);
            descriptor.producedResources.push_back(
                TranslucencyOutputsContract);
            return descriptor;
        }

        RenderGraph m_graph{};
        RenderFeatureContext m_context{};
        RenderGraphBlackboardProvenance m_provenance{};
        RGTextureHandle m_sceneColor{};
        RGTextureHandle m_opaqueSceneSnapshot{};
        RGTextureHandle m_presentableColor{};
        RHIBufferRef m_exposureStateBuffer{};
        RGBufferHandle m_exposureState{};
        PostProcessStack m_stack{};
        PostProcessPipelineFeatureAdapter m_adapter;
    };

    TEST(RenderFeatureValidation,
         DeferredLightingRequiresValidLightingPlanBeforePipelinePreparation)
    {
        RenderGraph graph;
        DeferredLightingFeatureAdapter adapter(nullptr, nullptr);
        RenderPassRecordContext missingPlan = MakeValidRecordContext(graph);
        missingPlan.lightingPlan.reset();
        missingPlan.localShadowAtlasPlan.reset();
        missingPlan.localShadowPass.reset();
        adapter.SetRecordContext(std::move(missingPlan));

        RenderFeatureContext context = MakeValidContext(
            RenderPipelineRecipe::DeferredQuality);
        context.resolvedRecipe = RenderPipelineRecipe::DeferredQuality;
        adapter.Prepare(context);
        EXPECT_FALSE(adapter.ResolveReadiness().inputsReady);
        EXPECT_FALSE(adapter.ResolveReadiness().pipelineReady);
        EXPECT_EQ(adapter.ResolveReadiness().diagnostic,
                  "Deferred lighting requires a valid LightingFramePlan for the current frame and view");

        RenderPassRecordContext planned = MakeValidRecordContext(graph);
        planned.lightingPlan = MakeValidLightingPlan();
        ASSERT_NE(nullptr, planned.lightingPlan);
        adapter.SetRecordContext(std::move(planned));
        adapter.Prepare(context);
        EXPECT_FALSE(adapter.ResolveReadiness().inputsReady);
        EXPECT_EQ(adapter.ResolveReadiness().diagnostic,
                  "Deferred lighting composition dependencies are unavailable");
    }

    TEST(RenderFeatureValidation,
         LightingOutputsV5RejectsPartialReplacementBundleAndPreservesFrameViewPlanIdentity)
    {
        const std::shared_ptr<const LightingFramePlan> plan =
            MakeValidLightingPlan();
        ASSERT_NE(nullptr, plan);
        ASSERT_NE(0U, plan->GetIdentityHash());
        EXPECT_EQ(5U, LightingOutputs::SchemaVersion);

        DeferredLightingFeatureAdapter producer(nullptr, nullptr);
        ReflectionFeatureAdapter consumer;
        const RenderGraphBlackboardProvenance provenance = MakeProvenance();

        RenderGraphBlackboard zeroIdentityBlackboard(provenance);
        EXPECT_EQ(zeroIdentityBlackboard.Publish(
                      BuiltinRenderFeatureIds::DeferredLighting,
                      producer.Describe(),
                      MakeLightingOutputs(provenance, 0)),
                  RenderGraphBlackboardCode::InvalidSemanticContract);

        LightingOutputs partial = MakeLightingOutputs(
            provenance, plan->GetIdentityHash());
        partial.specularResponse = {};
        EXPECT_EQ(zeroIdentityBlackboard.Publish(
                      BuiltinRenderFeatureIds::DeferredLighting,
                      producer.Describe(), std::move(partial)),
                  RenderGraphBlackboardCode::InvalidSemanticContract);

        RenderGraphBlackboard blackboard(provenance);
        ASSERT_EQ(blackboard.Publish(
                      BuiltinRenderFeatureIds::DeferredLighting,
                      producer.Describe(),
                      MakeLightingOutputs(provenance, plan->GetIdentityHash())),
                  RenderGraphBlackboardCode::Success);

        RenderGraphBlackboardCode code = RenderGraphBlackboardCode::Success;
        const LightingOutputs* const outputs = blackboard.Require<LightingOutputs>(
            BuiltinRenderFeatureIds::ReflectionResolve,
            consumer.Describe(),
            &code);
        ASSERT_EQ(RenderGraphBlackboardCode::Success, code);
        ASSERT_NE(nullptr, outputs);
        EXPECT_TRUE(outputs->IsCompatibleWithLightingPlan(plan->GetIdentityHash()));
        EXPECT_FALSE(outputs->IsCompatibleWithLightingPlan(
            plan->GetIdentityHash() + 1U));
        EXPECT_EQ(outputs->frameIdentity.frameSequence,
                  provenance.frameSequence);
        EXPECT_EQ(outputs->viewIdentity.subViewIndex,
                  provenance.viewOrdinal);
        EXPECT_EQ(outputs->hdrSceneColorMetadata,
                  MakeSceneTextureMetadata(
                      SceneTextures::HDRSceneColorContract));
    }

    TEST(RenderFeatureValidation,
         ReflectionIdentityResolvePublishesV3AsDataOnlyCompletion)
    {
        RenderGraph graph;
        const std::shared_ptr<const LightingFramePlan> plan =
            MakeValidLightingPlan();
        ASSERT_NE(nullptr, plan);

        auto lighting = std::make_unique<DataOnlyLightingOutputsFeature>(
            plan->GetIdentityHash());
        DataOnlyLightingOutputsFeature* const lightingRaw = lighting.get();
        auto reflection = std::make_unique<ReflectionFeatureAdapter>();
        RenderPassRecordContext recordContext = MakeValidRecordContext(graph);
        recordContext.lightingPlan = plan;
        reflection->SetRecordContext(std::move(recordContext));

        RenderFeatureRegistry registry;
        ASSERT_EQ(registry.Register(std::move(lighting)),
                  RenderFeatureRegistryCode::Success);
        ASSERT_EQ(registry.Register(std::move(reflection)),
                  RenderFeatureRegistryCode::Success);
        ASSERT_EQ(registry.DeclareImplementedRecipe(
                      RenderPipelineRecipe::ForwardStandard),
                  RenderFeatureRegistryCode::Success);
        ASSERT_EQ(registry.Seal(), RenderFeatureRegistryCode::Success);
        ASSERT_TRUE(registry.PrepareFrame(MakeValidContext()).Succeeded());

        const RenderGraphBlackboardProvenance provenance{
            graph.GetGraphIdentity(), graph.GetRecordingGeneration(), 1, 0, 1};
        RenderFeatureGraphBuilder builder(graph, provenance);
        RenderGraphBlackboard blackboard(provenance);
        ASSERT_EQ(registry.AddPasses(builder, blackboard),
                  RenderFeatureRegistryCode::Success);
        EXPECT_EQ(lightingRaw->GetPublishCode(),
                  RenderGraphBlackboardCode::Success);
        EXPECT_EQ(graph.GetRecordedPassCount(), 0U);

        VolumetricLightingFeatureAdapter volumetricLighting(nullptr);
        RenderGraphBlackboardCode code = RenderGraphBlackboardCode::Success;
        const ReflectionOutputs* const outputs =
            blackboard.Require<ReflectionOutputs>(
                BuiltinRenderFeatureIds::VolumetricLighting,
                volumetricLighting.Describe(), &code);
        ASSERT_EQ(code, RenderGraphBlackboardCode::Success);
        ASSERT_NE(outputs, nullptr);
        EXPECT_EQ(ReflectionOutputs::SchemaVersion, 3U);
        EXPECT_EQ(outputs->frameIdentity, MakeValidContext().frameIdentity);
        EXPECT_EQ(outputs->viewIdentity, MakeValidContext().viewIdentity);
        EXPECT_EQ(outputs->lightingPlanIdentity, plan->GetIdentityHash());
        EXPECT_TRUE(outputs->resolvedSceneColor.IsValid());
        EXPECT_TRUE(outputs->imageBasedLightingPreserved);
        EXPECT_FALSE(outputs->screenSpaceReflectionsApplied);
        EXPECT_FALSE(outputs->reflectionProbesApplied);
        EXPECT_FALSE(outputs->rayTracedReflectionsScheduled);
        EXPECT_FALSE(outputs->rayTracedReflectionsApplied);
        EXPECT_EQ(outputs->hybridRayTracingPlanIdentity, 0U);

        RenderGraphValidationAccess::Compile(graph);
        ASSERT_TRUE(graph.GetCompileStats().compileValid);
        NoOpCommandContext commands;
        RenderGraphValidationAccess::Execute(graph, commands);
        ASSERT_EQ(registry.FinalizeGraphExecution(graph),
                  RenderFeatureRegistryCode::Success);

        const RenderExecutionReceipt receipt =
            registry.CollectExecutionReceipt();
        const auto reflectionReceipt = std::find_if(
            receipt.features.begin(), receipt.features.end(),
            [](const RenderFeatureExecutionReceipt& feature)
            {
                return feature.featureId ==
                    BuiltinRenderFeatureIds::ReflectionResolve;
            });
        ASSERT_NE(reflectionReceipt, receipt.features.end());
        EXPECT_EQ(reflectionReceipt->graphPassCount, 0U);
        EXPECT_EQ(reflectionReceipt->executedGraphPassCount, 0U);
        EXPECT_TRUE(reflectionReceipt->contractCompletedWithoutGraphWork);
        EXPECT_TRUE(reflectionReceipt->readiness.executed);
        EXPECT_TRUE(receipt.executionSucceeded);
    }

    TEST(RenderFeatureValidation,
         ReflectionIdentityResolveRejectsStaleLightingFrameIdentity)
    {
        RenderGraph graph;
        const std::shared_ptr<const LightingFramePlan> plan =
            MakeValidLightingPlan();
        ASSERT_NE(nullptr, plan);

        ReflectionFeatureAdapter adapter;
        RenderPassRecordContext recordContext = MakeValidRecordContext(graph);
        recordContext.lightingPlan = plan;
        adapter.SetRecordContext(std::move(recordContext));
        adapter.Prepare(MakeValidContext());
        ASSERT_TRUE(adapter.ResolveReadiness().IsReadyToExecute());

        const RenderGraphBlackboardProvenance provenance{
            graph.GetGraphIdentity(), graph.GetRecordingGeneration(), 1, 0, 1};
        LightingOutputs stale = MakeLightingOutputs(
            provenance, plan->GetIdentityHash());
        ++stale.frameIdentity.sceneRevision;
        DeferredLightingFeatureAdapter producer(nullptr, nullptr);
        RenderGraphBlackboard blackboard(provenance);
        ASSERT_EQ(blackboard.Publish(
                      BuiltinRenderFeatureIds::DeferredLighting,
                      producer.Describe(), std::move(stale)),
                  RenderGraphBlackboardCode::Success);

        RenderFeatureGraphBuilder builder(graph, provenance);
        adapter.AddPasses(builder, blackboard);
        EXPECT_FALSE(adapter.ResolveReadiness().inputsReady);
        EXPECT_EQ(adapter.ResolveReadiness().diagnostic,
                  "Reflection identity resolve requires the exact typed LightingOutputs v5");
        EXPECT_FALSE(blackboard.WasPublishedBy(
            ReflectionOutputsContract,
            BuiltinRenderFeatureIds::ReflectionResolve));
    }

    TEST(RenderFeatureValidation,
         HybridReflectionRegistersOnlyRayTracingChainBeforeSpecialSurfaces)
    {
        RenderGraph graph;
        RenderFeatureContext context = MakeValidContext(
            RenderPipelineRecipe::HybridRayTracing);
        context.resolvedRecipe = RenderPipelineRecipe::HybridRayTracing;
        context.pipeline.quality = RenderQualityTier::Medium;

        RenderPassRecordContext passContext = MakeValidRecordContext(graph);
        const std::shared_ptr<const LightingFramePlan> lightingPlan =
            MakeValidLightingPlan();
        ASSERT_NE(nullptr, lightingPlan);
        passContext.lightingPlan = lightingPlan;

        const std::shared_ptr<const ReflectionFramePlan> reflectionPlan =
            MakeValidReflectionPlan(context, lightingPlan->GetIdentityHash());
        ASSERT_NE(nullptr, reflectionPlan);
        const HybridRayTracingFramePlan hybridPlan =
            MakeValidHybridRayTracingPlan(context);
        ASSERT_TRUE(hybridPlan.IsValid());
        ASSERT_EQ(hybridPlan.GetReflectionRoute(),
                  HybridRayTracingReflectionRoute::RayTraced);

        DefaultRenderPassBundle passBundle;
        passBundle.AddPass(
            std::make_unique<TestRayTracedReflectionPass>(), nullptr);
        passBundle.AddPass(
            std::make_unique<TestRayTracedReflectionDenoisePass>(), nullptr);
        passBundle.AddPass(
            std::make_unique<TestRayTracedReflectionCompositePass>(), nullptr);
        ReflectionFeatureRecordContext recordContext;
        recordContext.pass = std::move(passContext);
        recordContext.plan = reflectionPlan;
        recordContext.hybridRayTracingPlan = hybridPlan;

        ReflectionFeatureAdapter adapter(&passBundle);
        adapter.SetRecordContext(std::move(recordContext));
        adapter.Prepare(context);
        ASSERT_TRUE(adapter.ResolveReadiness().IsReadyToExecute());

        const RenderGraphBlackboardProvenance provenance{
            graph.GetGraphIdentity(), graph.GetRecordingGeneration(), 1, 0, 1};
        DeferredLightingFeatureAdapter lightingProducer(nullptr, nullptr);
        RenderGraphBlackboard blackboard(provenance);
        ASSERT_EQ(blackboard.Publish(
                      BuiltinRenderFeatureIds::DeferredLighting,
                      lightingProducer.Describe(),
                      MakeLightingOutputs(provenance,
                                          lightingPlan->GetIdentityHash())),
                  RenderGraphBlackboardCode::Success);

        RenderFeatureGraphBuilder builder(graph, provenance);
        adapter.AddPasses(builder, blackboard);
        ASSERT_TRUE(adapter.ResolveReadiness().inputsReady);
        const auto addPostReflectionSentinel =
            [&builder](const char* name)
            {
                static_cast<void>(builder.AddPass<uint8>(
                    name,
                    RenderGraphPassType::Graphics,
                    [](RenderGraphBuilder&, uint8&) {},
                    [](const uint8&, RHICommandContext&) {}));
            };
        addPostReflectionSentinel("OpaqueSceneSnapshotCopy");
        addPostReflectionSentinel("WaterFeaturePass");
        // Translucency is now a later, recipe-neutral feature rather than
        // part of either special-surface adapter.
        addPostReflectionSentinel("TransparentPass");

        const std::vector<std::string>& names = builder.GetRegisteredPassNames();
        const auto findName = [&names](const char* name)
        {
            return std::find(names.begin(), names.end(), name);
        };
        const auto rayTracedReflection =
            findName("RayTracedReflectionPass");
        const auto spatialDenoise =
            findName("RayTracedReflectionDenoisePass");
        const auto composite =
            findName("RayTracedReflectionCompositePass");
        const auto opaqueSnapshot = findName("OpaqueSceneSnapshotCopy");
        const auto water = findName("WaterFeaturePass");
        const auto transparent = findName("TransparentPass");
        ASSERT_NE(rayTracedReflection, names.end());
        ASSERT_NE(spatialDenoise, names.end());
        ASSERT_NE(composite, names.end());
        ASSERT_NE(opaqueSnapshot, names.end());
        ASSERT_NE(water, names.end());
        ASSERT_NE(transparent, names.end());
        EXPECT_EQ(findName("HiZScreenSpaceReflection"), names.end());
        const auto indexOf = [&names](const auto& iterator)
        {
            return std::distance(names.cbegin(), iterator);
        };
        EXPECT_LT(indexOf(rayTracedReflection), indexOf(spatialDenoise));
        EXPECT_LT(indexOf(spatialDenoise), indexOf(composite));
        EXPECT_LT(indexOf(composite), indexOf(opaqueSnapshot));
        EXPECT_LT(indexOf(composite), indexOf(water));
        EXPECT_LT(indexOf(composite), indexOf(transparent));

        VolumetricLightingFeatureAdapter consumer(nullptr);
        RenderGraphBlackboardCode code = RenderGraphBlackboardCode::Success;
        const ReflectionOutputs* const outputs =
            blackboard.Require<ReflectionOutputs>(
                BuiltinRenderFeatureIds::VolumetricLighting,
                consumer.Describe(), &code);
        ASSERT_EQ(code, RenderGraphBlackboardCode::Success);
        ASSERT_NE(outputs, nullptr);
        EXPECT_TRUE(outputs->rayTracedReflectionsScheduled);
        EXPECT_FALSE(outputs->rayTracedReflectionsApplied);
        EXPECT_EQ(outputs->hybridRayTracingPlanIdentity,
                  hybridPlan.GetIdentityHash());
    }

    TEST(RenderFeatureValidation,
         HybridReflectionRejectsMissingPassesAndMismatchedFrozenPlan)
    {
        RenderGraph graph;
        RenderFeatureContext context = MakeValidContext(
            RenderPipelineRecipe::HybridRayTracing);
        context.resolvedRecipe = RenderPipelineRecipe::HybridRayTracing;
        RenderPassRecordContext passContext = MakeValidRecordContext(graph);
        const std::shared_ptr<const LightingFramePlan> lightingPlan =
            MakeValidLightingPlan();
        ASSERT_NE(nullptr, lightingPlan);
        passContext.lightingPlan = lightingPlan;
        const std::shared_ptr<const ReflectionFramePlan> reflectionPlan =
            MakeValidReflectionPlan(context, lightingPlan->GetIdentityHash());
        ASSERT_NE(nullptr, reflectionPlan);

        ReflectionFeatureRecordContext missingPasses;
        missingPasses.pass = passContext;
        missingPasses.plan = reflectionPlan;
        missingPasses.hybridRayTracingPlan =
            MakeValidHybridRayTracingPlan(context);
        ReflectionFeatureAdapter adapter;
        adapter.SetRecordContext(std::move(missingPasses));
        adapter.Prepare(context);
        EXPECT_FALSE(adapter.ResolveReadiness().inputsReady);
        EXPECT_FALSE(adapter.ResolveReadiness().IsReadyToExecute());

        RenderFeatureContext mismatchedContext = context;
        ++mismatchedContext.frameIdentity.sceneRevision;
        ReflectionFeatureRecordContext mismatchedPlan;
        mismatchedPlan.pass = passContext;
        mismatchedPlan.plan = reflectionPlan;
        mismatchedPlan.hybridRayTracingPlan =
            MakeValidHybridRayTracingPlan(mismatchedContext);
        adapter.SetRecordContext(std::move(mismatchedPlan));
        adapter.Prepare(context);
        EXPECT_FALSE(adapter.ResolveReadiness().inputsReady);
        EXPECT_FALSE(adapter.ResolveReadiness().IsReadyToExecute());
    }

    TEST(RenderFeatureValidation,
         NonHybridReflectionNeverRegistersRayTracingReflectionPasses)
    {
        RenderGraph graph;
        RenderFeatureContext context = MakeValidContext(
            RenderPipelineRecipe::DeferredQuality);
        context.resolvedRecipe = RenderPipelineRecipe::DeferredQuality;
        RenderPassRecordContext passContext = MakeValidRecordContext(graph);
        const std::shared_ptr<const LightingFramePlan> lightingPlan =
            MakeValidLightingPlan();
        ASSERT_NE(nullptr, lightingPlan);
        passContext.lightingPlan = lightingPlan;
        const std::shared_ptr<const ReflectionFramePlan> reflectionPlan =
            MakeValidReflectionPlan(context, lightingPlan->GetIdentityHash());
        ASSERT_NE(nullptr, reflectionPlan);

        ReflectionFeatureRecordContext recordContext;
        recordContext.pass = std::move(passContext);
        recordContext.plan = reflectionPlan;
        ReflectionFeatureAdapter adapter;
        adapter.SetRecordContext(std::move(recordContext));
        adapter.Prepare(context);
        ASSERT_TRUE(adapter.ResolveReadiness().IsReadyToExecute());

        const RenderGraphBlackboardProvenance provenance{
            graph.GetGraphIdentity(), graph.GetRecordingGeneration(), 1, 0, 1};
        DeferredLightingFeatureAdapter lightingProducer(nullptr, nullptr);
        RenderGraphBlackboard blackboard(provenance);
        ASSERT_EQ(blackboard.Publish(
                      BuiltinRenderFeatureIds::DeferredLighting,
                      lightingProducer.Describe(),
                      MakeLightingOutputs(provenance,
                                          lightingPlan->GetIdentityHash())),
                  RenderGraphBlackboardCode::Success);

        RenderFeatureGraphBuilder builder(graph, provenance);
        adapter.AddPasses(builder, blackboard);
        ASSERT_TRUE(adapter.ResolveReadiness().inputsReady);
        const std::vector<std::string>& names = builder.GetRegisteredPassNames();
        EXPECT_EQ(std::find(names.begin(), names.end(),
                            "RayTracedReflectionPass"),
                  names.end());
        EXPECT_EQ(std::find(names.begin(), names.end(),
                            "RayTracedReflectionDenoisePass"),
                  names.end());
        EXPECT_EQ(std::find(names.begin(), names.end(),
                            "RayTracedReflectionCompositePass"),
                  names.end());
    }

    TEST(RenderFeatureValidation,
         RegistryRejectsDuplicateIdentityAndRegistrationAfterSeal)
    {
        RenderFeatureRegistry registry;
        EXPECT_EQ(registry.Register(MakeFeature(MakeDescriptor(1, "first"))),
                  RenderFeatureRegistryCode::Success);
        EXPECT_EQ(registry.Register(MakeFeature(MakeDescriptor(1, "second"))),
                  RenderFeatureRegistryCode::DuplicateFeatureId);
        EXPECT_EQ(registry.Register(MakeFeature(MakeDescriptor(2, "first"))),
                  RenderFeatureRegistryCode::DuplicateFeatureName);
        EXPECT_EQ(registry.DeclareImplementedRecipe(
                      RenderPipelineRecipe::ForwardStandard),
                  RenderFeatureRegistryCode::Success);
        EXPECT_EQ(registry.Seal(), RenderFeatureRegistryCode::Success);
        EXPECT_TRUE(registry.IsSealed());
        EXPECT_EQ(registry.Register(MakeFeature(MakeDescriptor(3, "late"))),
                  RenderFeatureRegistryCode::RegistrySealed);
    }

    TEST(RenderFeatureValidation,
         RegistryRequiresTypedSnapshotChannelAndExactSchema)
    {
        RenderFeatureRegistry invalidRegistry;
        RenderFeatureDescriptor invalid = MakeDescriptor(1, "invalid-snapshot");
        invalid.snapshotChannel = RenderFeatureChannelId::Particle;
        invalid.snapshotChannelSchemaVersion =
            RVX_PARTICLE_RENDER_SNAPSHOT_SCHEMA_VERSION + 1;
        EXPECT_EQ(invalidRegistry.Register(MakeFeature(std::move(invalid))),
                  RenderFeatureRegistryCode::InvalidSnapshotChannel);

        RenderFeatureRegistry validRegistry;
        RenderFeatureDescriptor valid = MakeDescriptor(2, "particle-snapshot");
        valid.snapshotChannel = RenderFeatureChannelId::Particle;
        valid.snapshotChannelSchemaVersion =
            RVX_PARTICLE_RENDER_SNAPSHOT_SCHEMA_VERSION;
        EXPECT_EQ(validRegistry.Register(MakeFeature(std::move(valid))),
                  RenderFeatureRegistryCode::Success);
    }

    TEST(RenderFeatureValidation,
         RegistryRejectsUnfrozenOrInconsistentFrameContext)
    {
        RenderFeatureRegistry registry;
        EXPECT_EQ(registry.Register(MakeFeature(MakeDescriptor(1, "feature"))),
                  RenderFeatureRegistryCode::Success);
        EXPECT_EQ(registry.DeclareImplementedRecipe(
                      RenderPipelineRecipe::ForwardStandard),
                  RenderFeatureRegistryCode::Success);
        EXPECT_EQ(registry.Seal(), RenderFeatureRegistryCode::Success);

        RenderFeatureContext mismatchedFrame = MakeValidContext();
        mismatchedFrame.frameStamp.sequence = 2;
        EXPECT_EQ(registry.PrepareFrame(mismatchedFrame).code,
                  RenderFeatureRegistryCode::InvalidFrameContext);

        RenderFeatureContext invalidEffectTime = MakeValidContext();
        invalidEffectTime.effectTime.deterministicSeed = 0;
        EXPECT_EQ(registry.PrepareFrame(invalidEffectTime).code,
                  RenderFeatureRegistryCode::InvalidFrameContext);

        RenderFeatureContext invalidViewFamily = MakeValidContext();
        invalidViewFamily.viewCount = 0;
        EXPECT_EQ(registry.PrepareFrame(invalidViewFamily).code,
                  RenderFeatureRegistryCode::InvalidFrameContext);

        RenderFeatureContext invalidViewOrdinal = MakeValidContext();
        invalidViewOrdinal.recordingViewOrdinal = 1;
        EXPECT_EQ(registry.PrepareFrame(invalidViewOrdinal).code,
                  RenderFeatureRegistryCode::InvalidFrameContext);

        RenderFeatureContext invalidRecordEpoch = MakeValidContext();
        invalidRecordEpoch.recordEpoch = 0;
        EXPECT_EQ(registry.PrepareFrame(invalidRecordEpoch).code,
                  RenderFeatureRegistryCode::InvalidFrameContext);

        RenderFeatureContext missingViewFamily = MakeValidContext();
        missingViewFamily.viewFamily.reset();
        EXPECT_EQ(registry.PrepareFrame(missingViewFamily).code,
                  RenderFeatureRegistryCode::InvalidFrameContext);

        RenderFeatureContext mismatchedViewIdentity = MakeValidContext();
        mismatchedViewIdentity.viewIdentity.cameraEntityPacked = 7;
        EXPECT_EQ(registry.PrepareFrame(mismatchedViewIdentity).code,
                  RenderFeatureRegistryCode::InvalidFrameContext);

        RenderFeatureContext staleViewFamily = MakeValidContext();
        auto staleFamily = std::make_shared<RenderViewFamilySnapshot>(
            *staleViewFamily.viewFamily);
        ++staleFamily->identity.configurationRevision;
        staleViewFamily.viewFamily = std::move(staleFamily);
        EXPECT_EQ(registry.PrepareFrame(staleViewFamily).code,
                  RenderFeatureRegistryCode::InvalidFrameContext);
    }

    TEST(RenderFeatureValidation,
         RegistryPreparesEveryValidatedViewFamilyOrdinal)
    {
        RenderFeatureRegistry registry;
        auto feature = MakeFeature(MakeDescriptor(1, "view-feature"));
        FakeRenderFeature* const rawFeature = feature.get();
        ASSERT_EQ(registry.Register(std::move(feature)),
                  RenderFeatureRegistryCode::Success);
        ASSERT_EQ(registry.DeclareImplementedRecipe(
                      RenderPipelineRecipe::ForwardStandard),
                  RenderFeatureRegistryCode::Success);
        ASSERT_EQ(registry.Seal(), RenderFeatureRegistryCode::Success);

        RenderFeatureContext context = MakeValidContext();
        auto family = std::make_shared<RenderViewFamilySnapshot>(
            *context.viewFamily);
        family->views.push_back(MakeValidViewSnapshot(
            RenderViewIdentity{5, 7, 0, ViewRole::Shadow},
            context.frameStamp.absoluteTimeSeconds,
            context.frameStamp.deltaTimeSeconds));
        family->views.push_back(MakeValidViewSnapshot(
            RenderViewIdentity{5, 8, 0, ViewRole::Reflection},
            context.frameStamp.absoluteTimeSeconds,
            context.frameStamp.deltaTimeSeconds));
        context.viewFamily = family;
        context.viewCount = static_cast<uint32>(family->views.size());

        context.recordingViewOrdinal = 1;
        context.viewIdentity = family->views[1].identity;
        EXPECT_TRUE(registry.PrepareFrame(context).Succeeded());
        EXPECT_EQ(rawFeature->GetPrepareCount(), 1U);
        EXPECT_EQ(rawFeature->GetLastContext().recordingViewOrdinal, 1U);
        EXPECT_EQ(rawFeature->GetLastContext().viewIdentity.role,
                  ViewRole::Shadow);

        context.recordingViewOrdinal = 2;
        context.viewIdentity = family->views[2].identity;
        EXPECT_TRUE(registry.PrepareFrame(context).Succeeded());
        EXPECT_EQ(rawFeature->GetPrepareCount(), 2U);
        EXPECT_EQ(rawFeature->GetLastContext().recordingViewOrdinal, 2U);
        EXPECT_EQ(rawFeature->GetLastContext().viewIdentity.role,
                  ViewRole::Reflection);
    }

    TEST(RenderFeatureValidation,
         RegistryFailsClosedForRequiredSnapshotChannelBindings)
    {
        const auto makeRegistry = []()
        {
            RenderFeatureRegistry registry;
            RenderFeatureDescriptor descriptor =
                MakeDescriptor(1, "particle-feature");
            descriptor.snapshotChannel = RenderFeatureChannelId::Particle;
            descriptor.snapshotChannelSchemaVersion =
                RVX_PARTICLE_RENDER_SNAPSHOT_SCHEMA_VERSION;
            auto feature = MakeFeature(std::move(descriptor));
            FakeRenderFeature* const rawFeature = feature.get();
            EXPECT_EQ(registry.Register(std::move(feature)),
                      RenderFeatureRegistryCode::Success);
            EXPECT_EQ(registry.DeclareImplementedRecipe(
                          RenderPipelineRecipe::ForwardStandard),
                      RenderFeatureRegistryCode::Success);
            EXPECT_EQ(registry.Seal(), RenderFeatureRegistryCode::Success);
            return std::pair{std::move(registry), rawFeature};
        };
        const auto expectRequiredFailure = [&makeRegistry](
                                               RenderFeatureContext context,
                                               const char* diagnostic)
        {
            auto [registry, rawFeature] = makeRegistry();
            const RenderFeatureFramePrepareResult result =
                registry.PrepareFrame(std::move(context));
            EXPECT_EQ(result.code,
                      RenderFeatureRegistryCode::RequiredFeatureUnavailable);
            EXPECT_EQ(result.diagnostic, diagnostic);
            EXPECT_EQ(rawFeature->GetPrepareCount(), 0U);
        };

        expectRequiredFailure(
            MakeValidContext(),
            "Required render feature snapshot input is missing");

        RenderFeatureSnapshot mismatchedChannel = MakeCompleteFeatureSnapshot();
        ParticleRenderSnapshotItem item;
        item.header.channelId = RenderFeatureChannelId::Water;
        item.header.schemaVersion = RVX_PARTICLE_RENDER_SNAPSHOT_SCHEMA_VERSION;
        item.header.captureSequence = 1;
        item.header.configurationRevision = 4;
        item.header.source = RenderFeatureSourceIdentity{5, 0, 1};
        mismatchedChannel.particles.items.push_back(std::move(item));
        RenderFeatureContext mismatchedChannelContext = MakeValidContext();
        mismatchedChannelContext.snapshotInputs =
            MakeSnapshotInputs(std::move(mismatchedChannel));
        expectRequiredFailure(
            std::move(mismatchedChannelContext),
            "Render feature snapshot item channel identity is invalid");

        RenderFeatureSnapshot mismatchedSchema = MakeCompleteFeatureSnapshot();
        ++mismatchedSchema.particles.metadata.schemaVersion;
        RenderFeatureContext mismatchedSchemaContext = MakeValidContext();
        mismatchedSchemaContext.snapshotInputs =
            MakeSnapshotInputs(std::move(mismatchedSchema));
        expectRequiredFailure(
            std::move(mismatchedSchemaContext),
            "Render feature snapshot channel schema mismatches its descriptor");

        RenderFeatureSnapshot duplicateSource = MakeCompleteFeatureSnapshot();
        ParticleRenderSnapshotItem firstProducer;
        firstProducer.header.captureSequence = 7;
        firstProducer.header.configurationRevision = 8;
        firstProducer.header.source = RenderFeatureSourceIdentity{5, 0, 1};
        ParticleRenderSnapshotItem secondProducer = firstProducer;
        secondProducer.header.captureSequence = 9;
        duplicateSource.particles.items.push_back(std::move(firstProducer));
        duplicateSource.particles.items.push_back(std::move(secondProducer));
        RenderFeatureContext duplicateSourceContext = MakeValidContext();
        duplicateSourceContext.snapshotInputs = MakeSnapshotInputs(
            std::move(duplicateSource));
        expectRequiredFailure(
            std::move(duplicateSourceContext),
            "Render feature snapshot channel contains duplicate producer source");

        RenderFeatureContext staleGenerationContext = MakeValidContext();
        staleGenerationContext.snapshotInputs =
            MakeSnapshotInputs(MakeCompleteFeatureSnapshot(2));
        expectRequiredFailure(
            std::move(staleGenerationContext),
            "Render feature snapshot generation is stale");

        RenderFeatureContext mismatchedRouteContext = MakeValidContext();
        mismatchedRouteContext.snapshotInputs = MakeSnapshotInputs(
            MakeCompleteFeatureSnapshot(),
            RenderFeatureProducerEndpointId::SceneFeatureSnapshotStore,
            static_cast<RenderFeatureConsumerEndpointId>(99));
        expectRequiredFailure(
            std::move(mismatchedRouteContext),
            "Render feature snapshot channel route mismatches composition");

        auto [validRegistry, validRawFeature] = makeRegistry();
        RenderFeatureContext validContext = MakeValidContext();
        RenderFeatureSnapshot producerClockSnapshot =
            MakeCompleteFeatureSnapshot();
        ParticleRenderSnapshotItem producerClockItem;
        producerClockItem.header.channelId = RenderFeatureChannelId::Particle;
        producerClockItem.header.schemaVersion =
            RVX_PARTICLE_RENDER_SNAPSHOT_SCHEMA_VERSION;
        // These are deliberately producer clocks, not the Render frame
        // identity (frame sequence 1/configuration revision 4).
        producerClockItem.header.captureSequence = 97;
        producerClockItem.header.configurationRevision = 311;
        producerClockItem.header.source = RenderFeatureSourceIdentity{5, 0, 1};
        producerClockSnapshot.particles.items.push_back(
            std::move(producerClockItem));
        validContext.snapshotInputs = MakeSnapshotInputs(
            std::move(producerClockSnapshot));
        EXPECT_TRUE(validRegistry.PrepareFrame(validContext).Succeeded());
        EXPECT_EQ(validRawFeature->GetPrepareCount(), 1U);
        EXPECT_NE(validRawFeature->GetLastContext().snapshotInputs.GetSnapshot(),
                  nullptr);
        EXPECT_NE(
            validRawFeature->GetLastContext().snapshotInputs.GetChannelRegistry(),
            nullptr);
    }

    TEST(RenderFeatureValidation,
         RegistryRejectsMissingAndDuplicateResourceProducers)
    {
        RenderFeatureRegistry missingProducerRegistry;
        RenderFeatureDescriptor consumer = MakeDescriptor(
            1, "consumer", RenderFeaturePhase::Geometry);
        consumer.consumedResources.push_back(SceneTexturesContract);
        EXPECT_EQ(missingProducerRegistry.Register(MakeFeature(std::move(consumer))),
                  RenderFeatureRegistryCode::Success);
        EXPECT_EQ(missingProducerRegistry.DeclareImplementedRecipe(
                      RenderPipelineRecipe::ForwardStandard),
                  RenderFeatureRegistryCode::Success);
        EXPECT_EQ(missingProducerRegistry.Seal(),
                  RenderFeatureRegistryCode::MissingProducer);

        RenderFeatureRegistry duplicateProducerRegistry;
        RenderFeatureDescriptor firstProducer = MakeDescriptor(2, "first");
        firstProducer.producedResources.push_back(SceneTexturesContract);
        RenderFeatureDescriptor secondProducer = MakeDescriptor(3, "second");
        secondProducer.producedResources.push_back(SceneTexturesContract);
        EXPECT_EQ(duplicateProducerRegistry.Register(
                      MakeFeature(std::move(firstProducer))),
                  RenderFeatureRegistryCode::Success);
        EXPECT_EQ(duplicateProducerRegistry.Register(
                      MakeFeature(std::move(secondProducer))),
                  RenderFeatureRegistryCode::Success);
        EXPECT_EQ(duplicateProducerRegistry.DeclareImplementedRecipe(
                      RenderPipelineRecipe::ForwardStandard),
                  RenderFeatureRegistryCode::Success);
        EXPECT_EQ(duplicateProducerRegistry.Seal(),
                  RenderFeatureRegistryCode::DuplicateProducer);
    }

    TEST(RenderFeatureValidation,
         RegistryValidatesResourceCoverageOnlyForDeclaredRecipeProfiles)
    {
        RenderFeatureRegistry registry;

        RenderFeatureDescriptor forwardProducer = MakeDescriptor(
            61, "forward-producer", RenderFeaturePhase::Geometry);
        forwardProducer.producedResources.push_back(
            TranslucencyOutputsContract);
        RenderFeatureDescriptor futureHybridConsumer = MakeDescriptor(
            62, "future-hybrid-consumer", RenderFeaturePhase::HDRPost);
        futureHybridConsumer.requiredRecipes =
            ToMask(RenderPipelineRecipe::HybridRayTracing);
        futureHybridConsumer.consumedResources.push_back(
            TranslucencyOutputsContract);

        ASSERT_EQ(registry.Register(MakeFeature(std::move(forwardProducer))),
                  RenderFeatureRegistryCode::Success);
        ASSERT_EQ(registry.Register(
                      MakeFeature(std::move(futureHybridConsumer))),
                  RenderFeatureRegistryCode::Success);
        ASSERT_EQ(registry.DeclareImplementedRecipe(
                      RenderPipelineRecipe::ForwardStandard),
                  RenderFeatureRegistryCode::Success);

        // HybridRayTracing is not a composition promise of this registry, so
        // it must not demand a producer while sealing ForwardStandard.
        EXPECT_EQ(registry.Seal(), RenderFeatureRegistryCode::Success);
    }

    TEST(RenderFeatureValidation,
         RegistryRejectsInvalidSchemaAndIllegalPhaseEdges)
    {
        RenderFeatureRegistry invalidSchemaRegistry;
        RenderFeatureDescriptor invalidSchema = MakeDescriptor(1, "invalid");
        invalidSchema.producedResources.push_back(
            RenderFeatureResourceContract{
                RenderBlackboardResourceId::SceneTextures,
                SceneTextures::SchemaVersion + 1});
        EXPECT_EQ(invalidSchemaRegistry.Register(MakeFeature(std::move(invalidSchema))),
                  RenderFeatureRegistryCode::InvalidResourceContract);

        RenderFeatureRegistry illegalPhaseRegistry;
        RenderFeatureDescriptor lateProducer = MakeDescriptor(
            2, "late-producer", RenderFeaturePhase::Lighting);
        lateProducer.producedResources.push_back(SceneTexturesContract);
        RenderFeatureDescriptor earlyConsumer = MakeDescriptor(
            3, "early-consumer", RenderFeaturePhase::Geometry);
        earlyConsumer.consumedResources.push_back(SceneTexturesContract);
        EXPECT_EQ(illegalPhaseRegistry.Register(MakeFeature(std::move(lateProducer))),
                  RenderFeatureRegistryCode::Success);
        EXPECT_EQ(illegalPhaseRegistry.Register(MakeFeature(std::move(earlyConsumer))),
                  RenderFeatureRegistryCode::Success);
        EXPECT_EQ(illegalPhaseRegistry.DeclareImplementedRecipe(
                      RenderPipelineRecipe::ForwardStandard),
                  RenderFeatureRegistryCode::Success);
        EXPECT_EQ(illegalPhaseRegistry.Seal(),
                  RenderFeatureRegistryCode::IllegalPhaseEdge);
    }

    TEST(RenderFeatureValidation,
         RegistryRejectsResourceAndExplicitFallbackCycles)
    {
        RenderFeatureRegistry resourceCycleRegistry;
        RenderFeatureDescriptor first = MakeDescriptor(1, "first");
        first.consumedResources.push_back(VisibilityOutputsContract);
        first.producedResources.push_back(SceneTexturesContract);
        RenderFeatureDescriptor second = MakeDescriptor(2, "second");
        second.consumedResources.push_back(SceneTexturesContract);
        second.producedResources.push_back(VisibilityOutputsContract);
        EXPECT_EQ(resourceCycleRegistry.Register(MakeFeature(std::move(first))),
                  RenderFeatureRegistryCode::Success);
        EXPECT_EQ(resourceCycleRegistry.Register(MakeFeature(std::move(second))),
                  RenderFeatureRegistryCode::Success);
        EXPECT_EQ(resourceCycleRegistry.DeclareImplementedRecipe(
                      RenderPipelineRecipe::ForwardStandard),
                  RenderFeatureRegistryCode::Success);
        EXPECT_EQ(resourceCycleRegistry.Seal(),
                  RenderFeatureRegistryCode::DependencyCycle);

        RenderFeatureRegistry fallbackCycleRegistry;
        RenderFeatureDescriptor fallbackA = MakeDescriptor(3, "fallback-a");
        fallbackA.requiredRecipes = 0;
        fallbackA.optionalRecipes = ToMask(RenderPipelineRecipe::ForwardStandard);
        fallbackA.fallbackPolicy = RenderFeatureFallbackPolicy::ExplicitFeature;
        fallbackA.fallbackFeatureId = RenderFeatureId{4};
        RenderFeatureDescriptor fallbackB = MakeDescriptor(4, "fallback-b");
        fallbackB.requiredRecipes = 0;
        fallbackB.optionalRecipes = ToMask(RenderPipelineRecipe::ForwardStandard);
        fallbackB.fallbackPolicy = RenderFeatureFallbackPolicy::ExplicitFeature;
        fallbackB.fallbackFeatureId = RenderFeatureId{3};
        EXPECT_EQ(fallbackCycleRegistry.Register(MakeFeature(std::move(fallbackA))),
                  RenderFeatureRegistryCode::Success);
        EXPECT_EQ(fallbackCycleRegistry.Register(MakeFeature(std::move(fallbackB))),
                  RenderFeatureRegistryCode::Success);
        EXPECT_EQ(fallbackCycleRegistry.DeclareImplementedRecipe(
                      RenderPipelineRecipe::ForwardStandard),
                  RenderFeatureRegistryCode::Success);
        EXPECT_EQ(fallbackCycleRegistry.Seal(),
                  RenderFeatureRegistryCode::DependencyCycle);
    }

    TEST(RenderFeatureValidation,
         RegistryRejectsUnsupportedRecipeAndRequiredUnavailableFeature)
    {
        RenderFeatureRegistry unsupportedRecipeRegistry;
        EXPECT_EQ(unsupportedRecipeRegistry.Register(
                      MakeFeature(MakeDescriptor(1, "forward-only"))),
                  RenderFeatureRegistryCode::Success);
        EXPECT_EQ(unsupportedRecipeRegistry.DeclareImplementedRecipe(
                      RenderPipelineRecipe::ForwardStandard),
                  RenderFeatureRegistryCode::Success);
        EXPECT_EQ(unsupportedRecipeRegistry.Seal(),
                  RenderFeatureRegistryCode::Success);
        const RenderFeatureFramePrepareResult unsupported =
            unsupportedRecipeRegistry.PrepareFrame(
                MakeValidContext(RenderPipelineRecipe::DeferredQuality));
        EXPECT_EQ(unsupported.code, RenderFeatureRegistryCode::RecipeNotImplemented);
        const RenderExecutionReceipt unsupportedReceipt =
            unsupportedRecipeRegistry.CollectExecutionReceipt();
        EXPECT_TRUE(unsupportedReceipt.available);
        EXPECT_FALSE(unsupportedReceipt.graphComposed);
        EXPECT_FALSE(unsupportedReceipt.executionSucceeded);
        EXPECT_FALSE(unsupportedReceipt.succeeded);
        EXPECT_EQ(unsupportedReceipt.requestedRecipe,
                  RenderPipelineRecipe::DeferredQuality);

        RenderFeatureRegistry requiredRegistry;
        RenderFeatureReadiness unavailable = MakeReadyReadiness();
        unavailable.pipelineReady = false;
        unavailable.diagnostic = "pipeline missing";
        EXPECT_EQ(requiredRegistry.Register(MakeFeature(
                      MakeDescriptor(2, "required"), unavailable)),
                  RenderFeatureRegistryCode::Success);
        EXPECT_EQ(requiredRegistry.DeclareImplementedRecipe(
                      RenderPipelineRecipe::ForwardStandard),
                  RenderFeatureRegistryCode::Success);
        EXPECT_EQ(requiredRegistry.Seal(), RenderFeatureRegistryCode::Success);
        const RenderFeatureFramePrepareResult required =
            requiredRegistry.PrepareFrame(MakeValidContext());
        EXPECT_EQ(required.code,
                  RenderFeatureRegistryCode::RequiredFeatureUnavailable);
        EXPECT_EQ(required.diagnostic, "pipeline missing");
    }

    TEST(RenderFeatureValidation,
         RegistryFailsRequiredDeferredFeatureWithoutMrtOrStorageTexture)
    {
        const auto prepare = [](RenderFeatureCapabilityMask capabilities)
        {
            RenderFeatureRegistry registry;
            RenderFeatureDescriptor descriptor = MakeDescriptor(
                1, "deferred-geometry");
            descriptor.requiredRecipes = ToMask(
                RenderPipelineRecipe::DeferredQuality);
            descriptor.requiredCapabilities =
                ToMask(RenderFeatureCapability::Graphics) |
                ToMask(RenderFeatureCapability::MultipleRenderTargets) |
                ToMask(RenderFeatureCapability::StorageTexture);
            EXPECT_EQ(registry.Register(MakeFeature(std::move(descriptor))),
                      RenderFeatureRegistryCode::Success);
            EXPECT_EQ(registry.DeclareImplementedRecipe(
                          RenderPipelineRecipe::DeferredQuality),
                      RenderFeatureRegistryCode::Success);
            EXPECT_EQ(registry.Seal(), RenderFeatureRegistryCode::Success);

            RenderFeatureContext context = MakeValidContext(
                RenderPipelineRecipe::DeferredQuality);
            context.hardwareCapabilities = capabilities;
            const RenderFeatureFramePrepareResult result =
                registry.PrepareFrame(std::move(context));
            EXPECT_EQ(result.code,
                      RenderFeatureRegistryCode::RequiredFeatureUnavailable);
            EXPECT_EQ(result.diagnostic,
                      "Required RHI capabilities are unavailable");
            const RenderExecutionReceipt receipt =
                registry.CollectExecutionReceipt();
            ASSERT_EQ(receipt.features.size(), 1U);
            EXPECT_FALSE(receipt.features[0].readiness.hardwareSupported);
            EXPECT_FALSE(receipt.features[0].readiness.executed);
        };

        prepare(ToMask(RenderFeatureCapability::Graphics));
        prepare(ToMask(RenderFeatureCapability::Graphics) |
                ToMask(RenderFeatureCapability::MultipleRenderTargets));
        prepare(ToMask(RenderFeatureCapability::Graphics) |
                ToMask(RenderFeatureCapability::StorageTexture));
    }

    TEST(RenderFeatureValidation,
         VolumetricIdentityDefersComputeRequirementUntilGpuWorkIsRequested)
    {
        VolumetricLightingFeatureAdapter adapter(nullptr);
        const RenderFeatureDescriptor descriptor = adapter.Describe();
        EXPECT_EQ(descriptor.requiredCapabilities,
                  ToMask(RenderFeatureCapability::Graphics) |
                      ToMask(RenderFeatureCapability::DescriptorBindings));
        EXPECT_TRUE(descriptor.allowsDataOnlyCompletion);

        RenderFeatureContext context = MakeValidContext();
        context.hardwareCapabilities =
            ToMask(RenderFeatureCapability::Graphics) |
            ToMask(RenderFeatureCapability::DescriptorBindings);

        // Default settings are disabled, so a compatibility backend without
        // compute must still be eligible to complete the typed identity edge.
        adapter.Prepare(context);
        EXPECT_TRUE(adapter.ResolveReadiness().hardwareSupported);
        EXPECT_TRUE(adapter.ResolveReadiness().pipelineReady);

        VolumetricLightingFeatureRecordContext enabled;
        enabled.settings.enabled = true;
        enabled.settings.density = 0.025f;
        adapter.SetRecordContext(std::move(enabled));
        adapter.Prepare(context);
        EXPECT_FALSE(adapter.ResolveReadiness().hardwareSupported);
        EXPECT_FALSE(adapter.ResolveReadiness().pipelineReady);
        EXPECT_EQ(adapter.ResolveReadiness().diagnostic,
                  "Froxel volumetric lighting requires graphics, compute, and descriptor bindings");
    }

    TEST(RenderFeatureValidation,
         RasterRecipeAdaptersDeclareAUniqueRecipeNeutralTranslucencyBoundary)
    {
        LegacyForwardFeatureAdapter forwardGeometry(nullptr);
        ForwardLightingFeatureAdapter forwardLighting(nullptr);
        DeferredGeometryFeatureAdapter geometry(nullptr, nullptr);
        DeferredLightingFeatureAdapter lighting(nullptr, nullptr);
        ReflectionFeatureAdapter reflection;
        VolumetricLightingFeatureAdapter volumetricLighting(nullptr);
        ForwardSpecialFeatureAdapter forwardSpecial(nullptr);
        DeferredForwardSpecialFeatureAdapter deferredSpecial(nullptr);
        TranslucencyFeatureAdapter translucency(nullptr);

        const RenderFeatureDescriptor forwardGeometryDescriptor =
            forwardGeometry.Describe();
        const RenderFeatureDescriptor forwardLightingDescriptor =
            forwardLighting.Describe();
        const RenderFeatureDescriptor geometryDescriptor = geometry.Describe();
        const RenderFeatureDescriptor lightingDescriptor = lighting.Describe();
        const RenderFeatureDescriptor reflectionDescriptor =
            reflection.Describe();
        const RenderFeatureDescriptor volumetricLightingDescriptor =
            volumetricLighting.Describe();
        const RenderFeatureDescriptor forwardSpecialDescriptor =
            forwardSpecial.Describe();
        const RenderFeatureDescriptor deferredSpecialDescriptor =
            deferredSpecial.Describe();
        const RenderFeatureDescriptor translucencyDescriptor =
            translucency.Describe();

        EXPECT_EQ(geometryDescriptor.featureId,
                  BuiltinRenderFeatureIds::DeferredGeometry);
        EXPECT_EQ(lightingDescriptor.featureId,
                  BuiltinRenderFeatureIds::DeferredLighting);
        EXPECT_EQ(reflectionDescriptor.featureId,
                  BuiltinRenderFeatureIds::ReflectionResolve);
        EXPECT_EQ(volumetricLightingDescriptor.featureId,
                  BuiltinRenderFeatureIds::VolumetricLighting);
        EXPECT_EQ(forwardSpecialDescriptor.featureId,
                  BuiltinRenderFeatureIds::ForwardSpecialChain);
        EXPECT_EQ(deferredSpecialDescriptor.featureId,
                  BuiltinRenderFeatureIds::DeferredForwardSpecialChain);
        EXPECT_EQ(translucencyDescriptor.featureId,
                  BuiltinRenderFeatureIds::Translucency);
        EXPECT_EQ(BuiltinRenderFeatureIds::Translucency.value, 11U);
        EXPECT_EQ(BuiltinRenderFeatureIds::VolumetricLighting.value, 12U);
        EXPECT_EQ(forwardSpecialDescriptor.phase, RenderFeaturePhase::Sky);
        EXPECT_EQ(deferredSpecialDescriptor.phase, RenderFeaturePhase::Sky);
        EXPECT_EQ(translucencyDescriptor.phase,
                  RenderFeaturePhase::Translucency);
        EXPECT_EQ(geometryDescriptor.requiredRecipes,
                  ToMask(RenderPipelineRecipe::DeferredQuality) |
                      ToMask(RenderPipelineRecipe::HybridRayTracing));
        EXPECT_EQ(lightingDescriptor.requiredRecipes,
                  ToMask(RenderPipelineRecipe::DeferredQuality) |
                      ToMask(RenderPipelineRecipe::HybridRayTracing));
        EXPECT_EQ(reflectionDescriptor.requiredRecipes,
                  ToMask(RenderPipelineRecipe::ForwardStandard) |
                      ToMask(RenderPipelineRecipe::DeferredQuality) |
                      ToMask(RenderPipelineRecipe::HybridRayTracing));
        EXPECT_EQ(forwardSpecialDescriptor.requiredRecipes,
                  ToMask(RenderPipelineRecipe::ForwardStandard));
        EXPECT_EQ(deferredSpecialDescriptor.requiredRecipes,
                  ToMask(RenderPipelineRecipe::DeferredQuality) |
                      ToMask(RenderPipelineRecipe::HybridRayTracing));
        EXPECT_EQ(translucencyDescriptor.requiredRecipes,
                  ToMask(RenderPipelineRecipe::ForwardStandard) |
                      ToMask(RenderPipelineRecipe::DeferredQuality) |
                      ToMask(RenderPipelineRecipe::HybridRayTracing));
        EXPECT_EQ(geometryDescriptor.optionalRecipes, 0U);
        EXPECT_EQ(lightingDescriptor.optionalRecipes, 0U);
        EXPECT_EQ(reflectionDescriptor.optionalRecipes, 0U);
        EXPECT_EQ(volumetricLightingDescriptor.optionalRecipes, 0U);
        EXPECT_EQ(forwardSpecialDescriptor.optionalRecipes, 0U);
        EXPECT_EQ(deferredSpecialDescriptor.optionalRecipes, 0U);
        EXPECT_EQ(translucencyDescriptor.optionalRecipes, 0U);
        EXPECT_TRUE(std::find(geometryDescriptor.producedResources.begin(),
                              geometryDescriptor.producedResources.end(),
                              SceneTexturesContract) !=
                    geometryDescriptor.producedResources.end());
        EXPECT_TRUE(std::find(
                        forwardGeometryDescriptor.producedResources.begin(),
                        forwardGeometryDescriptor.producedResources.end(),
                        LocalShadowOutputsContract) !=
                    forwardGeometryDescriptor.producedResources.end());
        EXPECT_TRUE(std::find(
                        forwardLightingDescriptor.consumedResources.begin(),
                        forwardLightingDescriptor.consumedResources.end(),
                        LocalShadowOutputsContract) !=
                    forwardLightingDescriptor.consumedResources.end());
        EXPECT_TRUE(std::find(geometryDescriptor.producedResources.begin(),
                              geometryDescriptor.producedResources.end(),
                              LocalShadowOutputsContract) !=
                    geometryDescriptor.producedResources.end());
        EXPECT_TRUE(std::find(lightingDescriptor.consumedResources.begin(),
                              lightingDescriptor.consumedResources.end(),
                              SceneTexturesContract) !=
                    lightingDescriptor.consumedResources.end());
        EXPECT_TRUE(std::find(lightingDescriptor.consumedResources.begin(),
                              lightingDescriptor.consumedResources.end(),
                              LocalShadowOutputsContract) !=
                    lightingDescriptor.consumedResources.end());
        EXPECT_TRUE(std::find(lightingDescriptor.producedResources.begin(),
                              lightingDescriptor.producedResources.end(),
                              LightingOutputsContract) !=
                    lightingDescriptor.producedResources.end());
        EXPECT_TRUE(std::find(reflectionDescriptor.consumedResources.begin(),
                              reflectionDescriptor.consumedResources.end(),
                              LightingOutputsContract) !=
                    reflectionDescriptor.consumedResources.end());
        EXPECT_TRUE(std::find(reflectionDescriptor.producedResources.begin(),
                              reflectionDescriptor.producedResources.end(),
                              ReflectionOutputsContract) !=
                    reflectionDescriptor.producedResources.end());
        EXPECT_TRUE(std::find(
                        volumetricLightingDescriptor.consumedResources.begin(),
                        volumetricLightingDescriptor.consumedResources.end(),
                        ReflectionOutputsContract) !=
                    volumetricLightingDescriptor.consumedResources.end());
        EXPECT_TRUE(std::find(
                        volumetricLightingDescriptor.producedResources.begin(),
                        volumetricLightingDescriptor.producedResources.end(),
                        VolumetricLightingOutputsContract) !=
                    volumetricLightingDescriptor.producedResources.end());
        EXPECT_TRUE(std::find(
                        forwardSpecialDescriptor.consumedResources.begin(),
                        forwardSpecialDescriptor.consumedResources.end(),
                        VolumetricLightingOutputsContract) !=
                    forwardSpecialDescriptor.consumedResources.end());
        EXPECT_TRUE(std::find(
                        deferredSpecialDescriptor.consumedResources.begin(),
                        deferredSpecialDescriptor.consumedResources.end(),
                        VolumetricLightingOutputsContract) !=
                    deferredSpecialDescriptor.consumedResources.end());
        EXPECT_TRUE(std::find(
                        forwardSpecialDescriptor.consumedResources.begin(),
                        forwardSpecialDescriptor.consumedResources.end(),
                        LightingOutputsContract) ==
                    forwardSpecialDescriptor.consumedResources.end());
        EXPECT_TRUE(std::find(
                        deferredSpecialDescriptor.consumedResources.begin(),
                        deferredSpecialDescriptor.consumedResources.end(),
                        LightingOutputsContract) ==
                    deferredSpecialDescriptor.consumedResources.end());
        EXPECT_TRUE(std::find(
                        forwardSpecialDescriptor.producedResources.begin(),
                        forwardSpecialDescriptor.producedResources.end(),
                        PreTranslucencyOutputsContract) !=
                    forwardSpecialDescriptor.producedResources.end());
        EXPECT_TRUE(std::find(
                        deferredSpecialDescriptor.producedResources.begin(),
                        deferredSpecialDescriptor.producedResources.end(),
                        PreTranslucencyOutputsContract) !=
                    deferredSpecialDescriptor.producedResources.end());
        EXPECT_TRUE(std::find(
                        forwardSpecialDescriptor.producedResources.begin(),
                        forwardSpecialDescriptor.producedResources.end(),
                        TranslucencyOutputsContract) ==
                    forwardSpecialDescriptor.producedResources.end());
        EXPECT_TRUE(std::find(
                        deferredSpecialDescriptor.producedResources.begin(),
                        deferredSpecialDescriptor.producedResources.end(),
                        TranslucencyOutputsContract) ==
                    deferredSpecialDescriptor.producedResources.end());
        EXPECT_TRUE(std::find(
                        translucencyDescriptor.consumedResources.begin(),
                        translucencyDescriptor.consumedResources.end(),
                        PreTranslucencyOutputsContract) !=
                    translucencyDescriptor.consumedResources.end());
        EXPECT_TRUE(std::find(
                        translucencyDescriptor.producedResources.begin(),
                        translucencyDescriptor.producedResources.end(),
                        TranslucencyOutputsContract) !=
                    translucencyDescriptor.producedResources.end());

        EXPECT_NE(geometryDescriptor.requiredCapabilities &
                      ToMask(RenderFeatureCapability::MultipleRenderTargets),
                  0U);
        EXPECT_NE(lightingDescriptor.requiredCapabilities &
                      ToMask(RenderFeatureCapability::Compute),
                  0U);
        EXPECT_NE(lightingDescriptor.requiredCapabilities &
                      ToMask(RenderFeatureCapability::StorageTexture),
                  0U);

        const RenderGraphBlackboardProvenance provenance = MakeProvenance();
        RenderGraphBlackboard missingBlackboard(provenance);
        RenderGraphBlackboardCode code = RenderGraphBlackboardCode::Success;
        EXPECT_EQ(missingBlackboard.Require<VolumetricLightingOutputs>(
                      BuiltinRenderFeatureIds::ForwardSpecialChain,
                      forwardSpecialDescriptor, &code),
                  nullptr);
        EXPECT_EQ(code, RenderGraphBlackboardCode::MissingResource);

        RenderGraphBlackboard blackboard(provenance);
        ASSERT_EQ(blackboard.Publish(
                      BuiltinRenderFeatureIds::ReflectionResolve,
                      reflectionDescriptor,
                      MakeReflectionOutputs(provenance, 0x5245464C454354ULL)),
                  RenderGraphBlackboardCode::Success);
        ASSERT_EQ(blackboard.Publish(
                      BuiltinRenderFeatureIds::VolumetricLighting,
                      volumetricLightingDescriptor,
                      MakeVolumetricLightingOutputs(provenance)),
                  RenderGraphBlackboardCode::Success);
        EXPECT_NE(blackboard.Require<VolumetricLightingOutputs>(
                      BuiltinRenderFeatureIds::ForwardSpecialChain,
                      forwardSpecialDescriptor, &code),
                  nullptr);
        EXPECT_EQ(code, RenderGraphBlackboardCode::Success);
        EXPECT_NE(blackboard.Require<VolumetricLightingOutputs>(
                      BuiltinRenderFeatureIds::DeferredForwardSpecialChain,
                      deferredSpecialDescriptor, &code),
                  nullptr);
        EXPECT_EQ(code, RenderGraphBlackboardCode::Success);
    }

    TEST(RenderFeatureValidation,
         PreTranslucencyOutputsEnforcesSchemaAndExactGraphProvenance)
    {
        ForwardSpecialFeatureAdapter producer(nullptr);
        TranslucencyFeatureAdapter consumer(nullptr);
        const RenderFeatureDescriptor producerDescriptor =
            producer.Describe();
        const RenderFeatureDescriptor consumerDescriptor =
            consumer.Describe();
        const RenderGraphBlackboardProvenance provenance = MakeProvenance();

        RenderGraphBlackboard valid(provenance);
        ASSERT_EQ(valid.Publish(
                      BuiltinRenderFeatureIds::ForwardSpecialChain,
                      producerDescriptor,
                      MakePreTranslucencyOutputs(provenance)),
                  RenderGraphBlackboardCode::Success);
        RenderGraphBlackboardCode code = RenderGraphBlackboardCode::Success;
        EXPECT_NE(valid.Require<PreTranslucencyOutputs>(
                      BuiltinRenderFeatureIds::Translucency,
                      consumerDescriptor,
                      &code),
                  nullptr);
        EXPECT_EQ(code, RenderGraphBlackboardCode::Success);

        RenderFeatureDescriptor invalidSchema = producerDescriptor;
        ASSERT_EQ(invalidSchema.producedResources.size(), 1U);
        ++invalidSchema.producedResources[0].schemaVersion;
        RenderFeatureRegistry invalidSchemaRegistry;
        EXPECT_EQ(invalidSchemaRegistry.Register(
                      MakeFeature(std::move(invalidSchema))),
                  RenderFeatureRegistryCode::InvalidResourceContract);

        RenderGraphBlackboard wrongProvenanceBlackboard(provenance);
        PreTranslucencyOutputs wrongProvenance =
            MakePreTranslucencyOutputs(provenance);
        ++wrongProvenance.provenance.recordEpoch;
        EXPECT_EQ(wrongProvenanceBlackboard.Publish(
                      BuiltinRenderFeatureIds::ForwardSpecialChain,
                      producerDescriptor,
                      std::move(wrongProvenance)),
                  RenderGraphBlackboardCode::InvalidProvenance);

        RenderGraphBlackboard staleHandleBlackboard(provenance);
        PreTranslucencyOutputs staleHandle =
            MakePreTranslucencyOutputs(provenance);
        ++staleHandle.opaqueSceneSnapshot.recordingGeneration;
        EXPECT_EQ(staleHandleBlackboard.Publish(
                      BuiltinRenderFeatureIds::ForwardSpecialChain,
                      producerDescriptor,
                      std::move(staleHandle)),
                  RenderGraphBlackboardCode::StaleGraphResource);

        RenderGraphBlackboard staleFrameBlackboard(provenance);
        PreTranslucencyOutputs staleFrame =
            MakePreTranslucencyOutputs(provenance);
        ++staleFrame.frameIdentity.frameSequence;
        EXPECT_EQ(staleFrameBlackboard.Publish(
                      BuiltinRenderFeatureIds::ForwardSpecialChain,
                      producerDescriptor,
                      std::move(staleFrame)),
                  RenderGraphBlackboardCode::InvalidSemanticContract);

        RenderGraphBlackboard missingReceiptBlackboard(provenance);
        PreTranslucencyOutputs missingReceipt =
            MakePreTranslucencyOutputs(provenance);
        missingReceipt.opaqueSceneSnapshotReceipt.reset();
        EXPECT_EQ(missingReceiptBlackboard.Publish(
                      BuiltinRenderFeatureIds::ForwardSpecialChain,
                      producerDescriptor,
                      std::move(missingReceipt)),
                  RenderGraphBlackboardCode::InvalidSemanticContract);
    }

    TEST(RenderFeatureValidation,
         VolumetricOutputsRejectStaleVolumesAndPreserveTheExactIdentitySurface)
    {
        constexpr RenderFeatureId consumerId{91};
        const RenderGraphBlackboardProvenance provenance = MakeProvenance();
        RenderFeatureDescriptor producer = MakeDescriptor(
            BuiltinRenderFeatureIds::VolumetricLighting.value,
            "froxel-volumetric", RenderFeaturePhase::Sky);
        producer.producedResources.push_back(VolumetricLightingOutputsContract);
        RenderFeatureDescriptor consumer = MakeDescriptor(
            consumerId.value, "froxel-consumer", RenderFeaturePhase::Sky);
        consumer.consumedResources.push_back(VolumetricLightingOutputsContract);

        EXPECT_EQ(VolumetricLightingOutputs::SchemaVersion, 1U);
        EXPECT_EQ(BuiltinRenderFeatureIds::VolumetricLighting.value, 12U);

        RenderGraphBlackboard populated(provenance);
        const VolumetricLightingOutputs integrated =
            MakeVolumetricLightingOutputs(provenance);
        ASSERT_EQ(populated.Publish(
                      BuiltinRenderFeatureIds::VolumetricLighting,
                      producer, integrated),
                  RenderGraphBlackboardCode::Success);
        RenderGraphBlackboardCode consumeCode =
            RenderGraphBlackboardCode::Success;
        EXPECT_NE(populated.Require<VolumetricLightingOutputs>(
                      consumerId, consumer, &consumeCode),
                  nullptr);
        EXPECT_EQ(consumeCode, RenderGraphBlackboardCode::Success);
        EXPECT_TRUE(populated.WasConsumedBy(
            VolumetricLightingOutputsContract, consumerId));
        EXPECT_EQ(populated.Publish(
                      BuiltinRenderFeatureIds::VolumetricLighting,
                      producer, integrated),
                  RenderGraphBlackboardCode::DuplicateProducer);

        RenderGraphBlackboard stale(provenance);
        VolumetricLightingOutputs staleVolume =
            MakeVolumetricLightingOutputs(provenance);
        ++staleVolume.integratedScatteringTransmittance.recordingGeneration;
        EXPECT_EQ(stale.Publish(BuiltinRenderFeatureIds::VolumetricLighting,
                                producer, std::move(staleVolume)),
                  RenderGraphBlackboardCode::StaleGraphResource);

        RenderGraphBlackboard invalidVolume(provenance);
        VolumetricLightingOutputs invalidDimension =
            MakeVolumetricLightingOutputs(provenance);
        invalidDimension.integratedScatteringTransmittanceMetadata.dimension =
            RHITextureDimension::Texture2D;
        EXPECT_EQ(invalidVolume.Publish(
                      BuiltinRenderFeatureIds::VolumetricLighting, producer,
                      std::move(invalidDimension)),
                  RenderGraphBlackboardCode::InvalidSemanticContract);

        RenderGraphBlackboard identity(provenance);
        const VolumetricLightingOutputs identityOutputs =
            MakeVolumetricLightingOutputs(provenance, true);
        ASSERT_EQ(identity.Publish(BuiltinRenderFeatureIds::VolumetricLighting,
                                   producer, identityOutputs),
                  RenderGraphBlackboardCode::Success);
        EXPECT_FALSE(identityOutputs.integratedScatteringTransmittance.IsValid());
        EXPECT_EQ(identityOutputs.sourceSceneColor.index,
                  identityOutputs.resolvedSceneColor.index);
        EXPECT_EQ(identityOutputs.sourceSceneColorMetadata,
                  identityOutputs.resolvedSceneColorMetadata);

        RenderGraphBlackboard identityWithoutSurfaceGuides(provenance);
        VolumetricLightingOutputs guideFreeIdentity = identityOutputs;
        guideFreeIdentity.worldNormal = {};
        guideFreeIdentity.worldNormalMetadata = {};
        guideFreeIdentity.materialIdentity = {};
        guideFreeIdentity.materialIdentityMetadata = {};
        guideFreeIdentity.materialParameterTable = {};
        guideFreeIdentity.materialParameterTableMetadata = {};
        EXPECT_EQ(identityWithoutSurfaceGuides.Publish(
                      BuiltinRenderFeatureIds::VolumetricLighting,
                      producer,
                      std::move(guideFreeIdentity)),
                  RenderGraphBlackboardCode::Success);

        RenderGraphBlackboard partialSurfaceGuides(provenance);
        VolumetricLightingOutputs partialGuides = identityOutputs;
        partialGuides.materialIdentity = {};
        partialGuides.materialIdentityMetadata = {};
        partialGuides.materialParameterTable = {};
        partialGuides.materialParameterTableMetadata = {};
        EXPECT_EQ(partialSurfaceGuides.Publish(
                      BuiltinRenderFeatureIds::VolumetricLighting,
                      producer,
                      std::move(partialGuides)),
                  RenderGraphBlackboardCode::InvalidSemanticContract);

        RenderGraphBlackboard forgedIdentity(provenance);
        VolumetricLightingOutputs forged =
            MakeVolumetricLightingOutputs(provenance, true);
        forged.resolvedSceneColor = MakeTextureHandle(provenance, 77);
        EXPECT_EQ(forgedIdentity.Publish(
                      BuiltinRenderFeatureIds::VolumetricLighting, producer,
                      std::move(forged)),
                  RenderGraphBlackboardCode::InvalidSemanticContract);

        RenderFeatureRegistry schemaRegistry;
        RenderFeatureDescriptor invalidSchema = producer;
        ++invalidSchema.producedResources.front().schemaVersion;
        EXPECT_EQ(schemaRegistry.Register(MakeFeature(std::move(invalidSchema))),
                  RenderFeatureRegistryCode::InvalidResourceContract);
    }

    TEST(RenderFeatureValidation,
         PreTranslucencyV2ForwardsOneExactVolumetricRoute)
    {
        ForwardSpecialFeatureAdapter producer(nullptr);
        TranslucencyFeatureAdapter consumer(nullptr);
        const RenderFeatureDescriptor producerDescriptor = producer.Describe();
        const RenderFeatureDescriptor consumerDescriptor = consumer.Describe();
        const RenderGraphBlackboardProvenance provenance = MakeProvenance();

        EXPECT_EQ(PreTranslucencyOutputs::SchemaVersion, 2U);

        RenderGraphBlackboard valid(provenance);
        ASSERT_EQ(valid.Publish(
                      BuiltinRenderFeatureIds::ForwardSpecialChain,
                      producerDescriptor,
                      MakePreTranslucencyOutputs(provenance)),
                  RenderGraphBlackboardCode::Success);
        RenderGraphBlackboardCode code = RenderGraphBlackboardCode::Success;
        EXPECT_NE(valid.Require<PreTranslucencyOutputs>(
                      BuiltinRenderFeatureIds::Translucency,
                      consumerDescriptor, &code),
                  nullptr);
        EXPECT_EQ(code, RenderGraphBlackboardCode::Success);

        RenderGraphBlackboard identityWithVolume(provenance);
        PreTranslucencyOutputs invalidIdentity =
            MakePreTranslucencyOutputs(provenance);
        invalidIdentity.integratedScatteringTransmittance =
            MakeTextureHandle(provenance, 44);
        invalidIdentity.integratedScatteringTransmittanceMetadata =
            MakeFroxelVolumeMetadata(invalidIdentity.froxelGrid);
        EXPECT_EQ(identityWithVolume.Publish(
                      BuiltinRenderFeatureIds::ForwardSpecialChain,
                      producerDescriptor, std::move(invalidIdentity)),
                  RenderGraphBlackboardCode::InvalidSemanticContract);

        RenderGraphBlackboard stale(provenance);
        PreTranslucencyOutputs staleOutputs =
            MakePreTranslucencyOutputs(provenance);
        staleOutputs.volumetricIdentityRoute = false;
        staleOutputs.integratedScatteringTransmittance =
            MakeTextureHandle(provenance, 45);
        staleOutputs.integratedScatteringTransmittanceMetadata =
            MakeFroxelVolumeMetadata(staleOutputs.froxelGrid);
        ++staleOutputs.integratedScatteringTransmittance.graphIdentity;
        EXPECT_EQ(stale.Publish(BuiltinRenderFeatureIds::ForwardSpecialChain,
                                producerDescriptor, std::move(staleOutputs)),
                  RenderGraphBlackboardCode::StaleGraphResource);

        RenderGraphBlackboard wrongGrid(provenance);
        PreTranslucencyOutputs mismatchedGrid =
            MakePreTranslucencyOutputs(provenance);
        mismatchedGrid.froxelGrid.outputExtent.width += 1;
        EXPECT_EQ(wrongGrid.Publish(
                      BuiltinRenderFeatureIds::ForwardSpecialChain,
                      producerDescriptor, std::move(mismatchedGrid)),
                  RenderGraphBlackboardCode::InvalidSemanticContract);
    }

    TEST(RenderFeatureValidation,
         TranslucencyFeatureTransformsTheExactPreBoundaryIntoPostOutputs)
    {
        RenderGraph graph;
        DefaultRenderPassBundle passes;
        RenderPassRecordContext recordContext =
            MakeValidRecordContext(graph);
        auto frozenFrame = std::make_shared<RenderPassFrameSnapshot>();
        frozenFrame->identity = recordContext.identity;
        recordContext.frameSnapshot = std::move(frozenFrame);
        ASSERT_NE(recordContext.lightingPlan, nullptr);
        const uint64 lightingPlanIdentity =
            recordContext.lightingPlan->GetIdentityHash();

        TranslucencyFeatureAdapter adapter(&passes);
        adapter.SetRecordContext(std::move(recordContext));
        RenderFeatureContext context = MakeValidContext();
        context.resolvedRecipe = context.pipeline.recipe;
        context.resolvedQuality = context.pipeline.quality;
        context.profileHash = ComputeRenderPipelineProfileHash(
            context.resolvedRecipe, context.resolvedQuality);
        adapter.Prepare(context);
        ASSERT_TRUE(adapter.ResolveReadiness().IsReadyToExecute())
            << adapter.ResolveReadiness().diagnostic;

        const RenderGraphBlackboardProvenance provenance{
            graph.GetGraphIdentity(),
            graph.GetRecordingGeneration(),
            context.frameIdentity.frameSequence,
            context.recordingViewOrdinal,
            context.recordEpoch};
        RenderGraphBlackboard blackboard(provenance);
        ForwardSpecialFeatureAdapter producer(nullptr);
        PreTranslucencyOutputs inputs =
            MakePreTranslucencyOutputs(provenance);
        inputs.frameIdentity = context.frameIdentity;
        inputs.viewIdentity = context.viewIdentity;
        inputs.lightingPlanIdentity = lightingPlanIdentity;
        ASSERT_EQ(blackboard.Publish(
                      BuiltinRenderFeatureIds::ForwardSpecialChain,
                      producer.Describe(),
                      inputs),
                  RenderGraphBlackboardCode::Success);

        RenderFeatureGraphBuilder builder(graph, provenance);
        adapter.AddPasses(builder, blackboard);
        ASSERT_TRUE(adapter.ResolveReadiness().inputsReady)
            << adapter.ResolveReadiness().diagnostic;
        EXPECT_TRUE(blackboard.WasPublishedBy(
            TranslucencyOutputsContract,
            BuiltinRenderFeatureIds::Translucency));

        const RenderFeatureDescriptor postProcessConsumer =
            PostProcessPipelineFeatureAdapter(nullptr).Describe();
        RenderGraphBlackboardCode code = RenderGraphBlackboardCode::Success;
        const TranslucencyOutputs* const outputs =
            blackboard.Require<TranslucencyOutputs>(
                BuiltinRenderFeatureIds::PostProcessPipeline,
                postProcessConsumer,
                &code);
        ASSERT_EQ(code, RenderGraphBlackboardCode::Success);
        ASSERT_NE(outputs, nullptr);
        EXPECT_EQ(outputs->frameIdentity, inputs.frameIdentity);
        EXPECT_EQ(outputs->viewIdentity, inputs.viewIdentity);
        EXPECT_EQ(outputs->hdrSceneColor.index,
                  inputs.hdrSceneColor.index);
        EXPECT_EQ(outputs->hdrSceneColor.graphIdentity,
                  inputs.hdrSceneColor.graphIdentity);
        EXPECT_EQ(outputs->opaqueSceneSnapshot.index,
                  inputs.opaqueSceneSnapshot.index);
        EXPECT_EQ(outputs->opaqueSceneSnapshot.graphIdentity,
                  inputs.opaqueSceneSnapshot.graphIdentity);
        EXPECT_EQ(outputs->exposureState.index,
                  inputs.exposureState.index);
        EXPECT_EQ(outputs->exposureState.graphIdentity,
                  inputs.exposureState.graphIdentity);
        EXPECT_FALSE(outputs->distortionVectors.IsValid());
    }

    TEST(RenderFeatureValidation,
         OptionalFeatureCanSkipOrSelectItsExplicitFallback)
    {
        RenderFeatureRegistry skipRegistry;
        RenderFeatureDescriptor skipped = MakeDescriptor(1, "skipped");
        skipped.requiredRecipes = 0;
        skipped.optionalRecipes = ToMask(RenderPipelineRecipe::ForwardStandard);
        skipped.fallbackPolicy = RenderFeatureFallbackPolicy::Skip;
        RenderFeatureReadiness unavailable = MakeReadyReadiness();
        unavailable.inputsReady = false;
        unavailable.diagnostic = "source input missing";
        EXPECT_EQ(skipRegistry.Register(
                      MakeFeature(std::move(skipped), unavailable)),
                  RenderFeatureRegistryCode::Success);
        EXPECT_EQ(skipRegistry.DeclareImplementedRecipe(
                      RenderPipelineRecipe::ForwardStandard),
                  RenderFeatureRegistryCode::Success);
        EXPECT_EQ(skipRegistry.Seal(), RenderFeatureRegistryCode::Success);
        EXPECT_TRUE(skipRegistry.PrepareFrame(MakeValidContext()).Succeeded());
        const RenderExecutionReceipt skippedReceipt =
            skipRegistry.CollectExecutionReceipt();
        ASSERT_EQ(skippedReceipt.features.size(), 1U);
        EXPECT_TRUE(skippedReceipt.features[0].selected);
        EXPECT_TRUE(skippedReceipt.features[0].fallbackUsed);
        EXPECT_FALSE(
            skippedReceipt.features[0].selectedFallbackFeatureId.IsValid());
        EXPECT_FALSE(
            skippedReceipt.features[0].fallbackForFeatureId.IsValid());
        EXPECT_EQ(skippedReceipt.features[0].diagnostic, "source input missing");

        RenderFeatureRegistry explicitFallbackRegistry;
        RenderFeatureDescriptor primary = MakeDescriptor(2, "primary");
        primary.requiredRecipes = 0;
        primary.optionalRecipes = ToMask(RenderPipelineRecipe::ForwardStandard);
        primary.fallbackPolicy = RenderFeatureFallbackPolicy::ExplicitFeature;
        primary.fallbackFeatureId = RenderFeatureId{3};
        RenderFeatureDescriptor fallback = MakeDescriptor(3, "fallback");
        fallback.requiredRecipes = 0;
        fallback.optionalRecipes = ToMask(RenderPipelineRecipe::ForwardStandard);
        fallback.fallbackPolicy = RenderFeatureFallbackPolicy::Skip;
        EXPECT_EQ(explicitFallbackRegistry.Register(
                      MakeFeature(std::move(primary), unavailable)),
                  RenderFeatureRegistryCode::Success);
        EXPECT_EQ(explicitFallbackRegistry.Register(MakeFeature(std::move(fallback))),
                  RenderFeatureRegistryCode::Success);
        EXPECT_EQ(explicitFallbackRegistry.DeclareImplementedRecipe(
                      RenderPipelineRecipe::ForwardStandard),
                  RenderFeatureRegistryCode::Success);
        EXPECT_EQ(explicitFallbackRegistry.Seal(),
                  RenderFeatureRegistryCode::Success);
        EXPECT_TRUE(explicitFallbackRegistry.PrepareFrame(MakeValidContext())
                        .Succeeded());
        const RenderExecutionReceipt fallbackReceipt =
            explicitFallbackRegistry.CollectExecutionReceipt();
        ASSERT_EQ(fallbackReceipt.features.size(), 2U);
        EXPECT_EQ(fallbackReceipt.features[0].featureId, RenderFeatureId{2});
        EXPECT_TRUE(fallbackReceipt.features[0].fallbackUsed);
        EXPECT_EQ(fallbackReceipt.features[0].selectedFallbackFeatureId,
                  RenderFeatureId{3});
        EXPECT_FALSE(
            fallbackReceipt.features[0].fallbackForFeatureId.IsValid());
        EXPECT_EQ(fallbackReceipt.features[1].featureId, RenderFeatureId{3});
        EXPECT_TRUE(fallbackReceipt.features[1].fallbackUsed);
        EXPECT_EQ(fallbackReceipt.features[1].fallbackForFeatureId,
                  RenderFeatureId{2});
        EXPECT_FALSE(
            fallbackReceipt.features[1].selectedFallbackFeatureId.IsValid());
    }

    TEST(RenderFeatureValidation,
         OptionalFallbackChainsContinueUntilAReadyFeature)
    {
        RenderFeatureReadiness unavailablePrimary = MakeReadyReadiness();
        unavailablePrimary.pipelineReady = false;
        unavailablePrimary.diagnostic = "primary pipeline missing";
        RenderFeatureReadiness unavailableSecondary = MakeReadyReadiness();
        unavailableSecondary.inputsReady = false;
        unavailableSecondary.diagnostic = "secondary inputs missing";

        RenderFeatureDescriptor primary = MakeDescriptor(30, "primary");
        primary.requiredRecipes = 0;
        primary.optionalRecipes =
            ToMask(RenderPipelineRecipe::ForwardStandard);
        primary.fallbackPolicy = RenderFeatureFallbackPolicy::ExplicitFeature;
        primary.fallbackFeatureId = RenderFeatureId{31};

        RenderFeatureDescriptor secondary = MakeDescriptor(31, "secondary");
        secondary.requiredRecipes = 0;
        secondary.optionalRecipes =
            ToMask(RenderPipelineRecipe::ForwardStandard);
        secondary.fallbackPolicy =
            RenderFeatureFallbackPolicy::ExplicitFeature;
        secondary.fallbackFeatureId = RenderFeatureId{32};

        RenderFeatureDescriptor terminal = MakeDescriptor(32, "terminal");
        terminal.requiredRecipes = 0;
        terminal.optionalRecipes =
            ToMask(RenderPipelineRecipe::ForwardStandard);
        terminal.fallbackPolicy = RenderFeatureFallbackPolicy::Skip;

        RenderFeatureRegistry registry;
        ASSERT_EQ(registry.Register(MakeFeature(
                      std::move(primary), unavailablePrimary)),
                  RenderFeatureRegistryCode::Success);
        ASSERT_EQ(registry.Register(MakeFeature(
                      std::move(secondary), unavailableSecondary)),
                  RenderFeatureRegistryCode::Success);
        ASSERT_EQ(registry.Register(MakeFeature(std::move(terminal))),
                  RenderFeatureRegistryCode::Success);
        ASSERT_EQ(registry.DeclareImplementedRecipe(
                      RenderPipelineRecipe::ForwardStandard),
                  RenderFeatureRegistryCode::Success);
        ASSERT_EQ(registry.Seal(), RenderFeatureRegistryCode::Success);
        ASSERT_TRUE(registry.PrepareFrame(MakeValidContext()).Succeeded());

        RenderGraph graph;
        const RenderGraphBlackboardProvenance provenance{
            graph.GetGraphIdentity(), graph.GetRecordingGeneration(), 1, 0, 1};
        RenderFeatureGraphBuilder builder(graph, provenance);
        RenderGraphBlackboard blackboard(provenance);
        ASSERT_EQ(registry.AddPasses(builder, blackboard),
                  RenderFeatureRegistryCode::Success);

        const RenderExecutionReceipt receipt =
            registry.CollectExecutionReceipt();
        ASSERT_EQ(receipt.features.size(), 3U);
        EXPECT_EQ(receipt.features[0].selectedFallbackFeatureId,
                  RenderFeatureId{31});
        EXPECT_FALSE(receipt.features[0].fallbackForFeatureId.IsValid());
        EXPECT_EQ(receipt.features[1].selectedFallbackFeatureId,
                  RenderFeatureId{32});
        EXPECT_EQ(receipt.features[1].fallbackForFeatureId,
                  RenderFeatureId{30});
        EXPECT_FALSE(
            receipt.features[2].selectedFallbackFeatureId.IsValid());
        EXPECT_EQ(receipt.features[2].fallbackForFeatureId,
                  RenderFeatureId{31});
    }

    TEST(RenderFeatureValidation,
         RegistryRejectsAmbiguousOrContractBreakingFallbacks)
    {
        const auto makeOptionalPrimary = [](uint32 id,
                                            std::string name,
                                            uint32 fallbackId)
        {
            RenderFeatureDescriptor descriptor =
                MakeDescriptor(id, std::move(name));
            descriptor.requiredRecipes = 0;
            descriptor.optionalRecipes =
                ToMask(RenderPipelineRecipe::ForwardStandard);
            descriptor.fallbackPolicy =
                RenderFeatureFallbackPolicy::ExplicitFeature;
            descriptor.fallbackFeatureId = RenderFeatureId{fallbackId};
            descriptor.producedResources.push_back(SceneTexturesContract);
            return descriptor;
        };
        const auto makeFallback = [](uint32 id,
                                     RenderFeaturePhase phase =
                                         RenderFeaturePhase::FrameSetup)
        {
            RenderFeatureDescriptor descriptor =
                MakeDescriptor(id, "fallback", phase);
            descriptor.requiredRecipes = 0;
            descriptor.optionalRecipes =
                ToMask(RenderPipelineRecipe::ForwardStandard);
            descriptor.fallbackPolicy = RenderFeatureFallbackPolicy::Skip;
            descriptor.producedResources.push_back(SceneTexturesContract);
            return descriptor;
        };

        RenderFeatureRegistry converging;
        ASSERT_EQ(converging.Register(MakeFeature(
                      makeOptionalPrimary(40, "first", 42))),
                  RenderFeatureRegistryCode::Success);
        ASSERT_EQ(converging.Register(MakeFeature(
                      makeOptionalPrimary(41, "second", 42))),
                  RenderFeatureRegistryCode::Success);
        ASSERT_EQ(converging.Register(MakeFeature(makeFallback(42))),
                  RenderFeatureRegistryCode::Success);
        ASSERT_EQ(converging.DeclareImplementedRecipe(
                      RenderPipelineRecipe::ForwardStandard),
                  RenderFeatureRegistryCode::Success);
        EXPECT_EQ(converging.Seal(),
                  RenderFeatureRegistryCode::InvalidFallback);

        RenderFeatureRegistry phaseMismatch;
        ASSERT_EQ(phaseMismatch.Register(MakeFeature(
                      makeOptionalPrimary(50, "phase-primary", 51))),
                  RenderFeatureRegistryCode::Success);
        ASSERT_EQ(phaseMismatch.Register(MakeFeature(makeFallback(
                      51, RenderFeaturePhase::Geometry))),
                  RenderFeatureRegistryCode::Success);
        ASSERT_EQ(phaseMismatch.DeclareImplementedRecipe(
                      RenderPipelineRecipe::ForwardStandard),
                  RenderFeatureRegistryCode::Success);
        EXPECT_EQ(phaseMismatch.Seal(),
                  RenderFeatureRegistryCode::InvalidFallback);

        RenderFeatureRegistry outputMismatch;
        ASSERT_EQ(outputMismatch.Register(MakeFeature(
                      makeOptionalPrimary(60, "output-primary", 61))),
                  RenderFeatureRegistryCode::Success);
        RenderFeatureDescriptor wrongOutput = makeFallback(61);
        wrongOutput.producedResources.clear();
        ASSERT_EQ(outputMismatch.Register(MakeFeature(std::move(wrongOutput))),
                  RenderFeatureRegistryCode::Success);
        ASSERT_EQ(outputMismatch.DeclareImplementedRecipe(
                      RenderPipelineRecipe::ForwardStandard),
                  RenderFeatureRegistryCode::Success);
        EXPECT_EQ(outputMismatch.Seal(),
                  RenderFeatureRegistryCode::InvalidFallback);

        RenderFeatureRegistry requiredTarget;
        ASSERT_EQ(requiredTarget.Register(MakeFeature(
                      makeOptionalPrimary(62, "required-target-primary", 63))),
                  RenderFeatureRegistryCode::Success);
        RenderFeatureDescriptor requiredFallback =
            MakeDescriptor(63, "required-target-fallback");
        requiredFallback.producedResources.push_back(SceneTexturesContract);
        ASSERT_EQ(requiredTarget.Register(MakeFeature(
                      std::move(requiredFallback))),
                  RenderFeatureRegistryCode::Success);
        ASSERT_EQ(requiredTarget.DeclareImplementedRecipe(
                      RenderPipelineRecipe::ForwardStandard),
                  RenderFeatureRegistryCode::Success);
        EXPECT_EQ(requiredTarget.Seal(),
                  RenderFeatureRegistryCode::InvalidFallback);
    }

    TEST(RenderFeatureValidation,
         OptionalFeatureCannotSkipDeclaredOutputsWhenUnavailable)
    {
        RenderFeatureDescriptor descriptor = MakeDescriptor(70, "producer");
        descriptor.requiredRecipes = 0;
        descriptor.optionalRecipes =
            ToMask(RenderPipelineRecipe::ForwardStandard);
        descriptor.fallbackPolicy = RenderFeatureFallbackPolicy::Skip;
        descriptor.producedResources.push_back(SceneTexturesContract);
        RenderFeatureReadiness unavailable = MakeReadyReadiness();
        unavailable.inputsReady = false;

        RenderFeatureRegistry registry;
        ASSERT_EQ(registry.Register(MakeFeature(
                      std::move(descriptor), unavailable)),
                  RenderFeatureRegistryCode::Success);
        ASSERT_EQ(registry.DeclareImplementedRecipe(
                      RenderPipelineRecipe::ForwardStandard),
                  RenderFeatureRegistryCode::Success);
        ASSERT_EQ(registry.Seal(), RenderFeatureRegistryCode::Success);
        EXPECT_EQ(registry.PrepareFrame(MakeValidContext()).code,
                  RenderFeatureRegistryCode::OptionalFallbackUnavailable);
    }

    TEST(RenderFeatureValidation,
         DeclaredRecipeMustCoverEveryPublicQualityTier)
    {
        RenderFeatureDescriptor lowOnly = MakeDescriptor(80, "low-only");
        lowOnly.qualityTiers = ToMask(RenderQualityTier::Low);

        RenderFeatureRegistry registry;
        ASSERT_EQ(registry.Register(MakeFeature(std::move(lowOnly))),
                  RenderFeatureRegistryCode::Success);
        ASSERT_EQ(registry.DeclareImplementedRecipe(
                      RenderPipelineRecipe::ForwardStandard),
                  RenderFeatureRegistryCode::Success);
        EXPECT_EQ(registry.Seal(),
                  RenderFeatureRegistryCode::RecipeNotImplemented);

        RenderFeatureDescriptor deferredOnly =
            MakeDescriptor(81, "deferred-only");
        deferredOnly.requiredRecipes =
            ToMask(RenderPipelineRecipe::DeferredQuality);
        RenderFeatureRegistry wrongRecipe;
        ASSERT_EQ(wrongRecipe.Register(MakeFeature(
                      std::move(deferredOnly))),
                  RenderFeatureRegistryCode::Success);
        ASSERT_EQ(wrongRecipe.DeclareImplementedRecipe(
                      RenderPipelineRecipe::ForwardStandard),
                  RenderFeatureRegistryCode::Success);
        EXPECT_EQ(wrongRecipe.Seal(),
                  RenderFeatureRegistryCode::RecipeNotImplemented);
    }

    TEST(RenderFeatureValidation,
         RegistrySelectsFeaturesAndFallbackTargetsPerCompleteProfile)
    {
        RenderFeatureDescriptor lowPrimary =
            MakeDescriptor(90, "low-primary");
        lowPrimary.requiredRecipes = 0;
        lowPrimary.optionalRecipes =
            ToMask(RenderPipelineRecipe::ForwardStandard);
        lowPrimary.qualityTiers = ToMask(RenderQualityTier::Low);
        lowPrimary.fallbackPolicy =
            RenderFeatureFallbackPolicy::ExplicitFeature;
        lowPrimary.fallbackFeatureId = RenderFeatureId{91};

        RenderFeatureDescriptor fallback =
            MakeDescriptor(91, "shared-fallback");
        fallback.requiredRecipes = 0;
        fallback.optionalRecipes =
            ToMask(RenderPipelineRecipe::ForwardStandard);
        fallback.fallbackPolicy = RenderFeatureFallbackPolicy::Skip;

        auto low = MakeFeature(std::move(lowPrimary));
        FakeRenderFeature* const lowRaw = low.get();
        auto sharedFallback = MakeFeature(std::move(fallback));
        FakeRenderFeature* const fallbackRaw = sharedFallback.get();

        RenderFeatureRegistry registry;
        ASSERT_EQ(registry.Register(std::move(low)),
                  RenderFeatureRegistryCode::Success);
        ASSERT_EQ(registry.Register(std::move(sharedFallback)),
                  RenderFeatureRegistryCode::Success);
        ASSERT_EQ(registry.DeclareImplementedRecipe(
                      RenderPipelineRecipe::ForwardStandard),
                  RenderFeatureRegistryCode::Success);
        ASSERT_EQ(registry.Seal(), RenderFeatureRegistryCode::Success);

        RenderFeatureContext lowContext = MakeValidContext();
        lowContext.pipeline.quality = RenderQualityTier::Low;
        ASSERT_TRUE(registry.PrepareFrame(lowContext).Succeeded());
        EXPECT_EQ(lowRaw->GetPrepareCount(), 1U);
        EXPECT_EQ(fallbackRaw->GetPrepareCount(), 0U);

        RenderFeatureContext highContext = MakeValidContext();
        highContext.pipeline.quality = RenderQualityTier::High;
        ASSERT_TRUE(registry.PrepareFrame(highContext).Succeeded());
        EXPECT_EQ(lowRaw->GetPrepareCount(), 1U);
        EXPECT_EQ(fallbackRaw->GetPrepareCount(), 1U);
    }

    TEST(RenderFeatureValidation,
         FallbackProducerSelectionActivatesExactlyOneProducerPerFrame)
    {
        const auto makePrimary = [](RenderFeatureReadiness readiness)
        {
            RenderFeatureDescriptor primary = MakeDescriptor(1, "primary");
            primary.requiredRecipes = 0;
            primary.optionalRecipes = ToMask(
                RenderPipelineRecipe::ForwardStandard);
            primary.producedResources.push_back(SceneTexturesContract);
            primary.fallbackPolicy =
                RenderFeatureFallbackPolicy::ExplicitFeature;
            primary.fallbackFeatureId = RenderFeatureId{2};
            auto feature =
                MakeFeature(std::move(primary), std::move(readiness));
            feature->SetPublishSceneTextures();
            return feature;
        };
        const auto makeFallback = []()
        {
            RenderFeatureDescriptor fallback = MakeDescriptor(2, "fallback");
            fallback.requiredRecipes = 0;
            fallback.optionalRecipes = ToMask(
                RenderPipelineRecipe::ForwardStandard);
            fallback.producedResources.push_back(SceneTexturesContract);
            fallback.fallbackPolicy = RenderFeatureFallbackPolicy::Skip;
            auto feature = MakeFeature(std::move(fallback));
            feature->SetPublishSceneTextures();
            return feature;
        };
        const auto compose = [](RenderFeatureRegistry& registry)
        {
            RenderGraph graph;
            const RenderGraphBlackboardProvenance provenance{
                graph.GetGraphIdentity(), graph.GetRecordingGeneration(), 1, 0, 1};
            RenderFeatureGraphBuilder builder(graph, provenance);
            RenderGraphBlackboard blackboard(provenance);
            EXPECT_EQ(registry.AddPasses(builder, blackboard),
                      RenderFeatureRegistryCode::Success);
        };

        RenderFeatureRegistry primaryReadyRegistry;
        auto primaryReady = makePrimary(MakeReadyReadiness());
        FakeRenderFeature* const primaryReadyRaw = primaryReady.get();
        auto fallbackReady = makeFallback();
        FakeRenderFeature* const fallbackReadyRaw = fallbackReady.get();
        ASSERT_EQ(primaryReadyRegistry.Register(std::move(primaryReady)),
                  RenderFeatureRegistryCode::Success);
        ASSERT_EQ(primaryReadyRegistry.Register(std::move(fallbackReady)),
                  RenderFeatureRegistryCode::Success);
        ASSERT_EQ(primaryReadyRegistry.DeclareImplementedRecipe(
                      RenderPipelineRecipe::ForwardStandard),
                  RenderFeatureRegistryCode::Success);
        ASSERT_EQ(primaryReadyRegistry.Seal(), RenderFeatureRegistryCode::Success);
        ASSERT_TRUE(primaryReadyRegistry.PrepareFrame(MakeValidContext())
                        .Succeeded());
        compose(primaryReadyRegistry);
        EXPECT_EQ(primaryReadyRaw->GetPrepareCount(), 1U);
        EXPECT_EQ(primaryReadyRaw->GetAddPassesCount(), 1U);
        EXPECT_EQ(fallbackReadyRaw->GetPrepareCount(), 0U);
        EXPECT_EQ(fallbackReadyRaw->GetAddPassesCount(), 0U);

        RenderFeatureRegistry fallbackSelectedRegistry;
        RenderFeatureReadiness unavailable = MakeReadyReadiness();
        unavailable.inputsReady = false;
        unavailable.diagnostic = "primary inputs unavailable";
        auto unavailablePrimary = makePrimary(unavailable);
        FakeRenderFeature* const unavailablePrimaryRaw =
            unavailablePrimary.get();
        auto selectedFallback = makeFallback();
        FakeRenderFeature* const selectedFallbackRaw = selectedFallback.get();
        ASSERT_EQ(fallbackSelectedRegistry.Register(std::move(unavailablePrimary)),
                  RenderFeatureRegistryCode::Success);
        ASSERT_EQ(fallbackSelectedRegistry.Register(std::move(selectedFallback)),
                  RenderFeatureRegistryCode::Success);
        ASSERT_EQ(fallbackSelectedRegistry.DeclareImplementedRecipe(
                      RenderPipelineRecipe::ForwardStandard),
                  RenderFeatureRegistryCode::Success);
        ASSERT_EQ(fallbackSelectedRegistry.Seal(),
                  RenderFeatureRegistryCode::Success);
        ASSERT_TRUE(fallbackSelectedRegistry.PrepareFrame(MakeValidContext())
                        .Succeeded());
        compose(fallbackSelectedRegistry);
        EXPECT_EQ(unavailablePrimaryRaw->GetPrepareCount(), 1U);
        EXPECT_EQ(unavailablePrimaryRaw->GetAddPassesCount(), 0U);
        EXPECT_EQ(selectedFallbackRaw->GetPrepareCount(), 1U);
        EXPECT_EQ(selectedFallbackRaw->GetAddPassesCount(), 1U);

        RenderFeatureRegistry disjointRecipeRegistry;
        RenderFeatureDescriptor forwardProducer = MakeDescriptor(
            10, "forward-producer");
        forwardProducer.producedResources.push_back(SceneTexturesContract);
        RenderFeatureDescriptor deferredProducer = MakeDescriptor(
            11, "deferred-producer");
        deferredProducer.requiredRecipes = ToMask(
            RenderPipelineRecipe::DeferredQuality);
        deferredProducer.producedResources.push_back(SceneTexturesContract);
        ASSERT_EQ(disjointRecipeRegistry.Register(
                      MakeFeature(std::move(forwardProducer))),
                  RenderFeatureRegistryCode::Success);
        ASSERT_EQ(disjointRecipeRegistry.Register(
                      MakeFeature(std::move(deferredProducer))),
                  RenderFeatureRegistryCode::Success);
        ASSERT_EQ(disjointRecipeRegistry.DeclareImplementedRecipe(
                      RenderPipelineRecipe::ForwardStandard),
                  RenderFeatureRegistryCode::Success);
        ASSERT_EQ(disjointRecipeRegistry.DeclareImplementedRecipe(
                      RenderPipelineRecipe::DeferredQuality),
                  RenderFeatureRegistryCode::Success);
        EXPECT_EQ(disjointRecipeRegistry.Seal(),
                  RenderFeatureRegistryCode::Success);

        RenderFeatureRegistry invalidFallbackDomainRegistry;
        RenderFeatureDescriptor optionalPrimary = MakeDescriptor(
            20, "optional-primary");
        optionalPrimary.requiredRecipes = 0;
        optionalPrimary.optionalRecipes = ToMask(
            RenderPipelineRecipe::ForwardStandard);
        optionalPrimary.fallbackPolicy =
            RenderFeatureFallbackPolicy::ExplicitFeature;
        optionalPrimary.fallbackFeatureId = RenderFeatureId{21};
        RenderFeatureDescriptor deferredOnlyFallback = MakeDescriptor(
            21, "deferred-only-fallback");
        deferredOnlyFallback.requiredRecipes = ToMask(
            RenderPipelineRecipe::DeferredQuality);
        ASSERT_EQ(invalidFallbackDomainRegistry.Register(
                      MakeFeature(std::move(optionalPrimary))),
                  RenderFeatureRegistryCode::Success);
        ASSERT_EQ(invalidFallbackDomainRegistry.Register(
                      MakeFeature(std::move(deferredOnlyFallback))),
                  RenderFeatureRegistryCode::Success);
        ASSERT_EQ(invalidFallbackDomainRegistry.DeclareImplementedRecipe(
                      RenderPipelineRecipe::ForwardStandard),
                  RenderFeatureRegistryCode::Success);
        ASSERT_EQ(invalidFallbackDomainRegistry.DeclareImplementedRecipe(
                      RenderPipelineRecipe::DeferredQuality),
                  RenderFeatureRegistryCode::Success);
        EXPECT_EQ(invalidFallbackDomainRegistry.Seal(),
                  RenderFeatureRegistryCode::InvalidFallback);
    }

    TEST(RenderFeatureValidation,
         BlackboardRejectsUndeclaredDuplicateAndStalePublications)
    {
        const RenderGraphBlackboardProvenance provenance = MakeProvenance();
        RenderGraphBlackboard blackboard(provenance);

        RenderFeatureDescriptor producer = MakeDescriptor(1, "producer");
        producer.producedResources.push_back(SceneTexturesContract);
        SceneTextures entry;
        entry.provenance = provenance;

        RenderFeatureDescriptor undeclared = MakeDescriptor(2, "undeclared");
        EXPECT_EQ(blackboard.Publish(RenderFeatureId{2}, undeclared, entry),
                  RenderGraphBlackboardCode::UndeclaredProducer);
        EXPECT_EQ(blackboard.Publish(RenderFeatureId{1}, producer, entry),
                  RenderGraphBlackboardCode::Success);
        EXPECT_EQ(blackboard.Publish(RenderFeatureId{1}, producer, entry),
                  RenderGraphBlackboardCode::DuplicateProducer);

        RenderGraphBlackboard staleBlackboard(provenance);
        SceneTextures staleEntry;
        staleEntry.provenance = provenance;
        staleEntry.deviceDepth.index = 0;
        staleEntry.deviceDepth.graphIdentity = provenance.graphIdentity + 1;
        staleEntry.deviceDepth.recordingGeneration =
            provenance.recordingGeneration;
        EXPECT_EQ(staleBlackboard.Publish(RenderFeatureId{1}, producer,
                                          staleEntry),
                  RenderGraphBlackboardCode::StaleGraphResource);

        RenderGraphBlackboard staleGBufferBlackboard(provenance);
        SceneTextures staleGBufferEntry;
        staleGBufferEntry.provenance = provenance;
        staleGBufferEntry.gBuffer3.index = 1;
        staleGBufferEntry.gBuffer3.graphIdentity = provenance.graphIdentity;
        staleGBufferEntry.gBuffer3.recordingGeneration =
            provenance.recordingGeneration + 1;
        EXPECT_EQ(staleGBufferBlackboard.Publish(RenderFeatureId{1},
                                                  producer,
                                                  staleGBufferEntry),
                  RenderGraphBlackboardCode::StaleGraphResource);

        RenderGraphBlackboard wrongProvenanceBlackboard(provenance);
        SceneTextures wrongProvenance = entry;
        ++wrongProvenance.provenance.recordEpoch;
        EXPECT_EQ(wrongProvenanceBlackboard.Publish(RenderFeatureId{1}, producer,
                                                    wrongProvenance),
                  RenderGraphBlackboardCode::InvalidProvenance);
    }

    TEST(RenderFeatureValidation,
         AmbientOcclusionAndTranslucencyRequireTheExactRasterRecipeOrder)
    {
        EXPECT_EQ(BuiltinRenderFeatureIds::AmbientOcclusion.value, 7U);
        EXPECT_EQ(BuiltinRenderFeatureIds::ForwardLighting.value, 8U);
        EXPECT_EQ(AmbientOcclusionOutputs::VisibilityContract.format,
                  RHIFormat::R8_UNORM);
        EXPECT_EQ(AmbientOcclusionOutputs::VisibilityContract.sampleCount,
                  RHISampleCount::Count1);

        RenderFeatureRegistry forwardRegistry;
        RenderFeatureDescriptor forwardGeometry =
            LegacyForwardFeatureAdapter(nullptr).Describe();
        RenderFeatureDescriptor ambientOcclusion = MakeDescriptor(
            BuiltinRenderFeatureIds::AmbientOcclusion.value, "gtao",
            RenderFeaturePhase::Lighting);
        ambientOcclusion.consumedResources.push_back(SceneTexturesContract);
        ambientOcclusion.producedResources.push_back(
            AmbientOcclusionOutputsContract);
        RenderFeatureDescriptor forwardLighting =
            ForwardLightingFeatureAdapter(nullptr).Describe();
        RenderFeatureDescriptor forwardReflection =
            ReflectionFeatureAdapter{}.Describe();
        RenderFeatureDescriptor forwardVolumetricLighting =
            VolumetricLightingFeatureAdapter(nullptr).Describe();
        RenderFeatureDescriptor forwardSpecial =
            ForwardSpecialFeatureAdapter(nullptr).Describe();
        RenderFeatureDescriptor forwardTranslucency =
            TranslucencyFeatureAdapter(nullptr).Describe();

        ASSERT_EQ(forwardRegistry.Register(
                      MakeFeature(std::move(forwardTranslucency))),
                  RenderFeatureRegistryCode::Success);
        ASSERT_EQ(forwardRegistry.Register(MakeFeature(std::move(forwardSpecial))),
                  RenderFeatureRegistryCode::Success);
        ASSERT_EQ(forwardRegistry.Register(
                      MakeFeature(std::move(forwardVolumetricLighting))),
                  RenderFeatureRegistryCode::Success);
        ASSERT_EQ(forwardRegistry.Register(
                      MakeFeature(std::move(forwardReflection))),
                  RenderFeatureRegistryCode::Success);
        ASSERT_EQ(forwardRegistry.Register(
                      MakeFeature(std::move(forwardLighting))),
                  RenderFeatureRegistryCode::Success);
        ASSERT_EQ(forwardRegistry.Register(
                      MakeFeature(std::move(ambientOcclusion))),
                  RenderFeatureRegistryCode::Success);
        ASSERT_EQ(forwardRegistry.Register(
                      MakeFeature(std::move(forwardGeometry))),
                  RenderFeatureRegistryCode::Success);
        ASSERT_EQ(forwardRegistry.DeclareImplementedRecipe(
                      RenderPipelineRecipe::ForwardStandard),
                  RenderFeatureRegistryCode::Success);
        ASSERT_EQ(forwardRegistry.Seal(), RenderFeatureRegistryCode::Success);
        const std::vector<RenderFeatureId>& forwardOrder =
            forwardRegistry.GetExecutionOrder();
        ASSERT_EQ(forwardOrder.size(), 7U);
        EXPECT_EQ(forwardOrder[0], BuiltinRenderFeatureIds::LegacyForwardMainChain);
        EXPECT_EQ(forwardOrder[1], BuiltinRenderFeatureIds::AmbientOcclusion);
        EXPECT_EQ(forwardOrder[2], BuiltinRenderFeatureIds::ForwardLighting);
        EXPECT_EQ(forwardOrder[3], BuiltinRenderFeatureIds::ReflectionResolve);
        EXPECT_EQ(forwardOrder[4], BuiltinRenderFeatureIds::VolumetricLighting);
        EXPECT_EQ(forwardOrder[5], BuiltinRenderFeatureIds::ForwardSpecialChain);
        EXPECT_EQ(forwardOrder[6], BuiltinRenderFeatureIds::Translucency);
        EXPECT_EQ(std::count(forwardOrder.begin(), forwardOrder.end(),
                             BuiltinRenderFeatureIds::Translucency),
                  1);

        RenderFeatureRegistry deferredRegistry;
        RenderFeatureDescriptor deferredGeometry =
            DeferredGeometryFeatureAdapter(nullptr, nullptr).Describe();
        RenderFeatureDescriptor deferredAO = MakeDescriptor(
            BuiltinRenderFeatureIds::AmbientOcclusion.value, "deferred-gtao",
            RenderFeaturePhase::Lighting);
        deferredAO.requiredRecipes =
            ToMask(RenderPipelineRecipe::DeferredQuality);
        deferredAO.consumedResources.push_back(SceneTexturesContract);
        deferredAO.producedResources.push_back(AmbientOcclusionOutputsContract);
        RenderFeatureDescriptor deferredLighting =
            DeferredLightingFeatureAdapter(nullptr, nullptr).Describe();
        RenderFeatureDescriptor deferredReflection =
            ReflectionFeatureAdapter{}.Describe();
        RenderFeatureDescriptor deferredVolumetricLighting =
            VolumetricLightingFeatureAdapter(nullptr).Describe();
        RenderFeatureDescriptor deferredSpecial =
            DeferredForwardSpecialFeatureAdapter(nullptr).Describe();
        RenderFeatureDescriptor deferredTranslucency =
            TranslucencyFeatureAdapter(nullptr).Describe();
        ASSERT_EQ(deferredRegistry.Register(
                      MakeFeature(std::move(deferredTranslucency))),
                  RenderFeatureRegistryCode::Success);
        ASSERT_EQ(deferredRegistry.Register(
                      MakeFeature(std::move(deferredSpecial))),
                  RenderFeatureRegistryCode::Success);
        ASSERT_EQ(deferredRegistry.Register(
                      MakeFeature(std::move(deferredVolumetricLighting))),
                  RenderFeatureRegistryCode::Success);
        ASSERT_EQ(deferredRegistry.Register(
                      MakeFeature(std::move(deferredReflection))),
                  RenderFeatureRegistryCode::Success);
        ASSERT_EQ(deferredRegistry.Register(
                      MakeFeature(std::move(deferredLighting))),
                  RenderFeatureRegistryCode::Success);
        ASSERT_EQ(deferredRegistry.Register(
                      MakeFeature(std::move(deferredAO))),
                  RenderFeatureRegistryCode::Success);
        ASSERT_EQ(deferredRegistry.Register(
                      MakeFeature(std::move(deferredGeometry))),
                  RenderFeatureRegistryCode::Success);
        ASSERT_EQ(deferredRegistry.DeclareImplementedRecipe(
                      RenderPipelineRecipe::DeferredQuality),
                  RenderFeatureRegistryCode::Success);
        ASSERT_EQ(deferredRegistry.Seal(), RenderFeatureRegistryCode::Success);
        const std::vector<RenderFeatureId>& deferredOrder =
            deferredRegistry.GetExecutionOrder();
        ASSERT_EQ(deferredOrder.size(), 7U);
        EXPECT_EQ(deferredOrder[0], BuiltinRenderFeatureIds::DeferredGeometry);
        EXPECT_EQ(deferredOrder[1], BuiltinRenderFeatureIds::AmbientOcclusion);
        EXPECT_EQ(deferredOrder[2], BuiltinRenderFeatureIds::DeferredLighting);
        EXPECT_EQ(deferredOrder[3], BuiltinRenderFeatureIds::ReflectionResolve);
        EXPECT_EQ(deferredOrder[4],
                  BuiltinRenderFeatureIds::VolumetricLighting);
        EXPECT_EQ(deferredOrder[5],
                  BuiltinRenderFeatureIds::DeferredForwardSpecialChain);
        EXPECT_EQ(deferredOrder[6], BuiltinRenderFeatureIds::Translucency);
        EXPECT_EQ(std::count(deferredOrder.begin(), deferredOrder.end(),
                             BuiltinRenderFeatureIds::Translucency),
                  1);

        const RenderGraphBlackboardProvenance provenance = MakeProvenance();
        RenderFeatureDescriptor producer = MakeDescriptor(
            BuiltinRenderFeatureIds::AmbientOcclusion.value, "gtao-producer",
            RenderFeaturePhase::Lighting);
        producer.producedResources.push_back(AmbientOcclusionOutputsContract);
        RenderFeatureDescriptor consumer = MakeDescriptor(
            BuiltinRenderFeatureIds::ForwardLighting.value, "gtao-consumer",
            RenderFeaturePhase::Lighting);
        consumer.consumedResources.push_back(AmbientOcclusionOutputsContract);

        RenderGraphBlackboard missing(provenance);
        RenderGraphBlackboardCode code = RenderGraphBlackboardCode::Success;
        EXPECT_EQ(missing.Require<AmbientOcclusionOutputs>(
                      BuiltinRenderFeatureIds::ForwardLighting, consumer, &code),
                  nullptr);
        EXPECT_EQ(code, RenderGraphBlackboardCode::MissingResource);

        RenderGraphBlackboard stale(provenance);
        AmbientOcclusionOutputs staleOutputs = MakeAmbientOcclusionOutputs(provenance);
        ++staleOutputs.ambientOcclusion.recordingGeneration;
        EXPECT_EQ(stale.Publish(BuiltinRenderFeatureIds::AmbientOcclusion,
                                producer, staleOutputs),
                  RenderGraphBlackboardCode::StaleGraphResource);

        RenderGraphBlackboard invalidSemantic(provenance);
        AmbientOcclusionOutputs invalidOutputs = MakeAmbientOcclusionOutputs(provenance);
        invalidOutputs.ambientOcclusionMetadata.format = RHIFormat::R16_FLOAT;
        EXPECT_EQ(invalidSemantic.Publish(BuiltinRenderFeatureIds::AmbientOcclusion,
                                          producer, invalidOutputs),
                  RenderGraphBlackboardCode::InvalidSemanticContract);

        RenderGraphBlackboard published(provenance);
        const AmbientOcclusionOutputs outputs = MakeAmbientOcclusionOutputs(provenance);
        EXPECT_EQ(published.Publish(BuiltinRenderFeatureIds::AmbientOcclusion,
                                    producer, outputs),
                  RenderGraphBlackboardCode::Success);
        EXPECT_EQ(published.Publish(BuiltinRenderFeatureIds::AmbientOcclusion,
                                    producer, outputs),
                  RenderGraphBlackboardCode::DuplicateProducer);
        EXPECT_NE(published.Require<AmbientOcclusionOutputs>(
                      BuiltinRenderFeatureIds::ForwardLighting, consumer, &code),
                  nullptr);
        EXPECT_EQ(code, RenderGraphBlackboardCode::Success);

        RenderFeatureRegistry schemaRegistry;
        RenderFeatureDescriptor invalidSchema = MakeDescriptor(77, "bad-gtao");
        invalidSchema.producedResources.push_back(
            RenderFeatureResourceContract{
                RenderBlackboardResourceId::AmbientOcclusionOutputs,
                AmbientOcclusionOutputs::SchemaVersion + 1});
        EXPECT_EQ(schemaRegistry.Register(MakeFeature(std::move(invalidSchema))),
                  RenderFeatureRegistryCode::InvalidResourceContract);
    }

    TEST(RenderFeatureValidation,
         BlackboardV2RejectsStaleAndWrongColorDomainPresentationContracts)
    {
        const RenderGraphBlackboardProvenance provenance = MakeProvenance();

        RenderFeatureDescriptor translucencyProducer = MakeDescriptor(
            71, "translucency", RenderFeaturePhase::Translucency);
        translucencyProducer.producedResources.push_back(
            TranslucencyOutputsContract);
        RenderFeatureDescriptor postProcessProducer = MakeDescriptor(
            72, "post-process", RenderFeaturePhase::HDRPost);
        postProcessProducer.producedResources.push_back(
            PostProcessOutputsContract);

        RenderGraphBlackboard validBlackboard(provenance);
        EXPECT_EQ(validBlackboard.Publish(
                      RenderFeatureId{71},
                      translucencyProducer,
                      MakeTranslucencyOutputs(provenance)),
                  RenderGraphBlackboardCode::Success);
        const RenderFeatureDescriptor postProcessConsumer =
            PostProcessPipelineFeatureAdapter(nullptr).Describe();
        RenderGraphBlackboardCode consumedCode =
            RenderGraphBlackboardCode::Success;
        EXPECT_NE(validBlackboard.Require<TranslucencyOutputs>(
                      BuiltinRenderFeatureIds::PostProcessPipeline,
                      postProcessConsumer,
                      &consumedCode),
                  nullptr);
        EXPECT_EQ(consumedCode, RenderGraphBlackboardCode::Success);

        RenderGraphBlackboard wrongDomainBlackboard(provenance);
        TranslucencyOutputs wrongDomain = MakeTranslucencyOutputs(provenance);
        wrongDomain.colorDomain = PostProcessColorDomain::DisplayEncodedLDR;
        EXPECT_EQ(wrongDomainBlackboard.Publish(
                      RenderFeatureId{71}, translucencyProducer, wrongDomain),
                  RenderGraphBlackboardCode::InvalidSemanticContract);

        RenderGraphBlackboard missingSnapshotBlackboard(provenance);
        TranslucencyOutputs missingSnapshot = MakeTranslucencyOutputs(provenance);
        missingSnapshot.opaqueSceneSnapshot = {};
        EXPECT_EQ(missingSnapshotBlackboard.Publish(
                      RenderFeatureId{71}, translucencyProducer, missingSnapshot),
                  RenderGraphBlackboardCode::InvalidSemanticContract);

        RenderGraphBlackboard aliasedSnapshotBlackboard(provenance);
        TranslucencyOutputs aliasedSnapshot = MakeTranslucencyOutputs(provenance);
        aliasedSnapshot.opaqueSceneSnapshot = aliasedSnapshot.hdrSceneColor;
        EXPECT_EQ(aliasedSnapshotBlackboard.Publish(
                      RenderFeatureId{71}, translucencyProducer, aliasedSnapshot),
                  RenderGraphBlackboardCode::InvalidSemanticContract);

        RenderGraphBlackboard staleHandleBlackboard(provenance);
        TranslucencyOutputs staleHandle = MakeTranslucencyOutputs(provenance);
        ++staleHandle.velocity.recordingGeneration;
        EXPECT_EQ(staleHandleBlackboard.Publish(
                      RenderFeatureId{71}, translucencyProducer, staleHandle),
                  RenderGraphBlackboardCode::StaleGraphResource);

        RenderGraphBlackboard staleFrameBlackboard(provenance);
        TranslucencyOutputs staleFrame = MakeTranslucencyOutputs(provenance);
        ++staleFrame.frameIdentity.frameSequence;
        EXPECT_EQ(staleFrameBlackboard.Publish(
                      RenderFeatureId{71}, translucencyProducer, staleFrame),
                  RenderGraphBlackboardCode::InvalidSemanticContract);

        RenderGraphBlackboard encodedBlackboard(provenance);
        EXPECT_EQ(encodedBlackboard.Publish(
                      RenderFeatureId{72},
                      postProcessProducer,
                      MakePostProcessOutputs(provenance)),
                  RenderGraphBlackboardCode::Success);

        RenderGraphBlackboard hardwareSRGBBlackboard(provenance);
        PostProcessOutputs hardwareSRGB = MakePostProcessOutputs(provenance);
        hardwareSRGB.transfer = PostProcessTransfer::DisplayLinearToEncoded;
        EXPECT_EQ(hardwareSRGBBlackboard.Publish(
                      RenderFeatureId{72},
                      postProcessProducer,
                      hardwareSRGB),
                  RenderGraphBlackboardCode::Success);

        RenderGraphBlackboard omittedTransitionBlackboard(provenance);
        PostProcessOutputs omittedTransition = MakePostProcessOutputs(provenance);
        omittedTransition.colorDomain = PostProcessColorDomain::Unknown;
        omittedTransition.transfer = PostProcessTransfer::None;
        EXPECT_EQ(omittedTransitionBlackboard.Publish(
                      RenderFeatureId{72},
                      postProcessProducer,
                      omittedTransition),
                  RenderGraphBlackboardCode::InvalidSemanticContract);

        RenderGraphBlackboard unencodedBlackboard(provenance);
        PostProcessOutputs unencoded = MakePostProcessOutputs(provenance);
        unencoded.colorDomain = PostProcessColorDomain::DisplayLinearLDR;
        EXPECT_EQ(unencodedBlackboard.Publish(
                      RenderFeatureId{72}, postProcessProducer, unencoded),
                  RenderGraphBlackboardCode::InvalidSemanticContract);
    }

    TEST(RenderFeatureValidation,
         PostProcessPipelineUsesStableFeatureContractAndDirectEncodedReceipt)
    {
        PostProcessPipelineFeatureAdapter adapter(nullptr);
        const RenderFeatureDescriptor descriptor = adapter.Describe();

        EXPECT_EQ(descriptor.featureId,
                  BuiltinRenderFeatureIds::PostProcessPipeline);
        EXPECT_EQ(descriptor.featureId.value, 2U);
        EXPECT_EQ(descriptor.phase, RenderFeaturePhase::HDRPost);
        EXPECT_TRUE(std::find(descriptor.consumedResources.begin(),
                              descriptor.consumedResources.end(),
                              TranslucencyOutputsContract) !=
                    descriptor.consumedResources.end());
        EXPECT_TRUE(std::find(descriptor.producedResources.begin(),
                              descriptor.producedResources.end(),
                              PostProcessOutputsContract) !=
                    descriptor.producedResources.end());
        EXPECT_EQ(descriptor.requiredRecipes,
                  ToMask(RenderPipelineRecipe::ForwardStandard) |
                      ToMask(RenderPipelineRecipe::DeferredQuality) |
                      ToMask(RenderPipelineRecipe::HybridRayTracing));

        RenderPostProcessExecutionReceipt directEncoded;
        directEncoded.postProcessContentHash = 0x10U;
        directEncoded.toneMapPassCount = 1;
        directEncoded.inputColorDomain =
            PostProcessColorDomain::SceneLinearHDR;
        directEncoded.plannedOutputColorDomain =
            PostProcessColorDomain::DisplayEncodedLDR;
        directEncoded.storedOutputColorDomain =
            PostProcessColorDomain::DisplayEncodedLDR;
        directEncoded.finalOutputFormat = RHIFormat::RGBA8_UNORM;
        directEncoded.toneMappingTransfer =
            PostProcessTransfer::SceneLinearToDisplayEncoded;
        directEncoded.presentationTransfer =
            PostProcessTransfer::SceneLinearToDisplayEncoded;
        directEncoded.hdrInputReady = true;
        directEncoded.encodedOutputReady = true;
        PopulateValidManualExposureReceipt(directEncoded);
        EXPECT_FALSE(directEncoded.displayLinearOutputReady);
        EXPECT_TRUE(directEncoded.IsStructurallyValid());

        directEncoded.taaPassCount = 1;
        EXPECT_FALSE(directEncoded.IsStructurallyValid());
        directEncoded.taaPassCount = 0;

        RenderPostProcessExecutionReceipt temporal = directEncoded;
        temporal.antiAliasingMode = RenderAntiAliasingMode::TAA;
        temporal.taaRequested = true;
        temporal.taaPipelineReady = true;
        temporal.taaInputsReady = true;
        temporal.taaExecuted = true;
        temporal.historyAvailableBefore = false;
        temporal.historySampled = false;
        temporal.historyCandidateProduced = true;
        temporal.historyResetReasons = RenderHistoryResetReason::FirstFrame;
        temporal.taaPassCount = 1;
        temporal.taaResolveGraphPassCount = 1;
        temporal.taaHistoryStoreGraphPassCount = 1;
        temporal.taaExposureStateHash = temporal.exposureStateContentHash;
        EXPECT_TRUE(temporal.IsStructurallyValid());

        temporal.historyCommitted = true;
        EXPECT_TRUE(temporal.IsStructurallyValid());
        temporal.historyAbandoned = true;
        EXPECT_FALSE(temporal.IsStructurallyValid());
        temporal.historyCommitted = false;
        EXPECT_TRUE(temporal.IsStructurallyValid());
        temporal.historyCandidateProduced = false;
        EXPECT_FALSE(temporal.IsStructurallyValid());
        temporal.historyAbandoned = false;
        EXPECT_FALSE(temporal.IsStructurallyValid());

        temporal = directEncoded;
        temporal.currentJitterPixels.x =
            std::numeric_limits<float>::quiet_NaN();
        EXPECT_FALSE(temporal.IsStructurallyValid());
        temporal = directEncoded;
        temporal.previousJitterPixels.y =
            std::numeric_limits<float>::infinity();
        EXPECT_FALSE(temporal.IsStructurallyValid());

        directEncoded.toneMapPassCount = 2;
        EXPECT_FALSE(directEncoded.IsStructurallyValid());
        directEncoded.toneMapPassCount = 1;
        directEncoded.fallbackUsed = true;
        EXPECT_FALSE(directEncoded.IsStructurallyValid());
        directEncoded.fallbackUsed = false;
        directEncoded.presentationTransfer =
            PostProcessTransfer::DisplayLinearToEncoded;
        EXPECT_FALSE(directEncoded.IsStructurallyValid());
    }

    TEST(RenderFeatureValidation,
         PostProcessPipelineFailsClosedWithoutExposurePipelineResources)
    {
        ScopedTestLog log;
        const auto verifyMissingExposureResources = [](RHIFormat format)
        {
            PostProcessAdapterTestFixture fixture(format);
            EXPECT_FALSE(fixture.Compose());
            EXPECT_FALSE(fixture.Adapter().ResolveReadiness().inputsReady);
            EXPECT_FALSE(fixture.Adapter().ResolveReadiness().executed);
            EXPECT_TRUE(fixture.Adapter().GetLastExecutionReceipt().IsEmpty());
            EXPECT_EQ(
                fixture.Adapter().ResolveReadiness().diagnostic,
                "Post-process typed plan could not be materialized");
            EXPECT_FALSE(fixture.Adapter().CollectReceipt().readiness.executed);
        };

        verifyMissingExposureResources(RHIFormat::RGBA8_UNORM_SRGB);
        verifyMissingExposureResources(RHIFormat::RGBA8_UNORM);
    }

    TEST(RenderFeatureValidation,
         PostProcessPipelineStackRebindingInvalidatesFrozenRecordState)
    {
        RenderGraph graph;
        const RenderPassRecordContext passContext =
            MakeValidRecordContext(graph);
        const RenderFeatureContext featureContext = MakeValidContext();

        PostProcessPipelineRecordContext recordContext;
        recordContext.frameIdentity = featureContext.frameIdentity;
        recordContext.viewIdentity = featureContext.viewIdentity;
        recordContext.presentableColor = passContext.view.colorTarget;
        recordContext.presentableColorMetadata = MakeSceneTextureMetadata(
            SceneTextures::FormatContract{
                RHIFormat::RGBA8_UNORM_SRGB, RHISampleCount::Count1});
        recordContext.passRecordContext = passContext;

        PostProcessPipelineFeatureAdapter adapter(nullptr);
        adapter.SetRecordContext(recordContext);
        adapter.Prepare(featureContext);
        EXPECT_TRUE(adapter.ResolveReadiness().inputsReady);
        EXPECT_FALSE(adapter.ResolveReadiness().pipelineReady);

        PostProcessStack replacementStack;
        adapter.SetStack(&replacementStack);
        EXPECT_FALSE(adapter.ResolveReadiness().inputsReady);
        EXPECT_FALSE(adapter.ResolveReadiness().pipelineReady);

        adapter.SetRecordContext(recordContext);
        adapter.Prepare(featureContext);
        EXPECT_TRUE(adapter.ResolveReadiness().inputsReady);
        EXPECT_FALSE(adapter.ResolveReadiness().pipelineReady);

        // Teardown must sever the borrowed stack and discard the record context
        // that was frozen against its prior lifetime.
        adapter.SetStack(nullptr);
        EXPECT_FALSE(adapter.ResolveReadiness().inputsReady);
        EXPECT_FALSE(adapter.ResolveReadiness().pipelineReady);
    }

    TEST(RenderFeatureValidation,
         DeferredSceneTexturesRejectMissingAndIncompatibleOutputs)
    {
        SceneTextures textures = MakeDeferredSceneTextures();
        EXPECT_EQ(ValidateDeferredSceneTextures(textures),
                  DeferredSceneTexturesValidationCode::Success);

        SceneTextures withoutVelocity = textures;
        withoutVelocity.velocity = {};
        withoutVelocity.velocityMetadata = {};
        EXPECT_EQ(ValidateDeferredSceneTextures(withoutVelocity),
                  DeferredSceneTexturesValidationCode::Success);

        SceneTextures incompatibleVelocity = textures;
        incompatibleVelocity.velocityMetadata.format = RHIFormat::RGBA16_FLOAT;
        EXPECT_EQ(ValidateDeferredSceneTextures(incompatibleVelocity),
                  DeferredSceneTexturesValidationCode::FormatMismatch);

        SceneTextures missingHandle = textures;
        missingHandle.gBuffer2 = {};
        EXPECT_EQ(ValidateDeferredSceneTextures(missingHandle),
                  DeferredSceneTexturesValidationCode::MissingRequiredHandle);

        SceneTextures missingMaterialTable = textures;
        missingMaterialTable.materialTable = {};
        EXPECT_EQ(ValidateDeferredSceneTextures(missingMaterialTable),
                  DeferredSceneTexturesValidationCode::MissingMaterialTable);

        SceneTextures invalidMaterialMetadata = textures;
        invalidMaterialMetadata.materialTableMetadata.contentHash = 0;
        EXPECT_EQ(ValidateDeferredSceneTextures(invalidMaterialMetadata),
                  DeferredSceneTexturesValidationCode::InvalidMaterialTable);

        SceneTextures invalidExtent = textures;
        invalidExtent.gBuffer0Metadata.extent = {};
        EXPECT_EQ(ValidateDeferredSceneTextures(invalidExtent),
                  DeferredSceneTexturesValidationCode::InvalidExtent);

        SceneTextures incompatibleFormat = textures;
        incompatibleFormat.gBuffer0Metadata.format = RHIFormat::RGBA8_UNORM;
        EXPECT_EQ(ValidateDeferredSceneTextures(incompatibleFormat),
                  DeferredSceneTexturesValidationCode::FormatMismatch);

        SceneTextures mismatchedExtent = textures;
        mismatchedExtent.gBuffer1Metadata.extent = RenderOutputExtent{640, 360};
        EXPECT_EQ(ValidateDeferredSceneTextures(mismatchedExtent),
                  DeferredSceneTexturesValidationCode::ExtentMismatch);

        SceneTextures multisampled = textures;
        multisampled.gBuffer2Metadata.sampleCount = RHISampleCount::Count4;
        EXPECT_EQ(ValidateDeferredSceneTextures(multisampled),
                  DeferredSceneTexturesValidationCode::SampleCountMismatch);
    }

    TEST(RenderFeatureValidation,
         RegistryBindsBlackboardToTheFrozenFrameViewAndRecordEpoch)
    {
        RenderFeatureRegistry registry;
        ASSERT_EQ(registry.Register(MakeFeature(MakeDescriptor(1, "feature"))),
                  RenderFeatureRegistryCode::Success);
        ASSERT_EQ(registry.DeclareImplementedRecipe(
                      RenderPipelineRecipe::ForwardStandard),
                  RenderFeatureRegistryCode::Success);
        ASSERT_EQ(registry.Seal(), RenderFeatureRegistryCode::Success);
        ASSERT_TRUE(registry.PrepareFrame(MakeValidContext()).Succeeded());

        const auto compose = [&registry](uint64 frameSequence,
                                         uint32 viewOrdinal,
                                         uint64 recordEpoch)
        {
            RenderGraph graph;
            const RenderGraphBlackboardProvenance provenance{
                graph.GetGraphIdentity(),
                graph.GetRecordingGeneration(),
                frameSequence,
                viewOrdinal,
                recordEpoch};
            RenderFeatureGraphBuilder builder(graph, provenance);
            RenderGraphBlackboard blackboard(provenance);
            return registry.AddPasses(builder, blackboard);
        };

        EXPECT_EQ(compose(2, 0, 1),
                  RenderFeatureRegistryCode::InvalidFrameContext);
        EXPECT_EQ(compose(1, 1, 1),
                  RenderFeatureRegistryCode::InvalidFrameContext);
        EXPECT_EQ(compose(1, 0, 2),
                  RenderFeatureRegistryCode::InvalidFrameContext);
        EXPECT_EQ(compose(1, 0, 1), RenderFeatureRegistryCode::Success);
    }

    TEST(RenderFeatureValidation,
         RegistryRequiresDeclaredTypedOutputsAndInputsAtCompositionTime)
    {
        const auto makeRegistry = [](bool publish, bool consume)
        {
            RenderFeatureRegistry registry;
            RenderFeatureDescriptor producer = MakeDescriptor(
                1, "producer", RenderFeaturePhase::FrameSetup);
            producer.producedResources.push_back(SceneTexturesContract);
            auto producerFeature = MakeFeature(std::move(producer));
            producerFeature->SetPublishSceneTextures(publish);

            RenderFeatureDescriptor consumer = MakeDescriptor(
                2, "consumer", RenderFeaturePhase::Geometry);
            consumer.consumedResources.push_back(SceneTexturesContract);
            auto consumerFeature = MakeFeature(std::move(consumer));
            consumerFeature->SetConsumeSceneTextures(consume);

            EXPECT_EQ(registry.Register(std::move(producerFeature)),
                      RenderFeatureRegistryCode::Success);
            EXPECT_EQ(registry.Register(std::move(consumerFeature)),
                      RenderFeatureRegistryCode::Success);
            EXPECT_EQ(registry.DeclareImplementedRecipe(
                          RenderPipelineRecipe::ForwardStandard),
                      RenderFeatureRegistryCode::Success);
            EXPECT_EQ(registry.Seal(), RenderFeatureRegistryCode::Success);
            EXPECT_TRUE(registry.PrepareFrame(MakeValidContext()).Succeeded());
            return registry;
        };
        const auto compose = [](RenderFeatureRegistry& registry)
        {
            RenderGraph graph;
            const RenderGraphBlackboardProvenance provenance{
                graph.GetGraphIdentity(), graph.GetRecordingGeneration(), 1, 0, 1};
            RenderFeatureGraphBuilder builder(graph, provenance);
            RenderGraphBlackboard blackboard(provenance);
            return registry.AddPasses(builder, blackboard);
        };

        RenderFeatureRegistry missingOutput = makeRegistry(false, true);
        EXPECT_EQ(compose(missingOutput),
                  RenderFeatureRegistryCode::GraphCompositionFailed);
        EXPECT_EQ(missingOutput.CollectExecutionReceipt().diagnostic,
                  "Render feature 'producer' did not publish a declared typed output");

        RenderFeatureRegistry missingInput = makeRegistry(true, false);
        EXPECT_EQ(compose(missingInput),
                  RenderFeatureRegistryCode::GraphCompositionFailed);
        EXPECT_EQ(missingInput.CollectExecutionReceipt().diagnostic,
                  "Render feature 'consumer' did not consume a declared typed input");

        RenderFeatureRegistry complete = makeRegistry(true, true);
        EXPECT_EQ(compose(complete), RenderFeatureRegistryCode::Success);
    }

    TEST(RenderFeatureValidation,
         ReceiptsKeepReadinessDimensionsAndExecutionSeparate)
    {
        RenderFeatureRegistry registry;
        RenderFeatureReadiness readyButNotExecuted = MakeReadyReadiness();
        readyButNotExecuted.executed = false;
        auto feature = MakeFeature(
            MakeDescriptor(1, "ready-not-executed"), readyButNotExecuted);
        FakeRenderFeature* const featureRaw = feature.get();
        EXPECT_EQ(registry.Register(std::move(feature)),
                  RenderFeatureRegistryCode::Success);
        EXPECT_EQ(registry.DeclareImplementedRecipe(
                      RenderPipelineRecipe::ForwardStandard),
                  RenderFeatureRegistryCode::Success);
        EXPECT_EQ(registry.Seal(), RenderFeatureRegistryCode::Success);
        EXPECT_TRUE(registry.PrepareFrame(MakeValidContext()).Succeeded());

        RenderGraph graph;
        const RenderGraphBlackboardProvenance provenance{
            graph.GetGraphIdentity(), graph.GetRecordingGeneration(), 1, 0, 1};
        RenderFeatureGraphBuilder builder(graph, provenance);
        RenderGraphBlackboard blackboard(provenance);
        EXPECT_EQ(registry.AddPasses(builder, blackboard),
                  RenderFeatureRegistryCode::Success);
        EXPECT_EQ(featureRaw->GetPrepareCount(), 1U);
        EXPECT_EQ(featureRaw->GetAddPassesCount(), 1U);

        const RenderExecutionReceipt receipt = registry.CollectExecutionReceipt(
            RenderExecutionBuildIdentity{std::string(40, 'a'), "test_preset"});
        ASSERT_EQ(receipt.features.size(), 1U);
        const RenderFeatureReadiness& readiness = receipt.features[0].readiness;
        EXPECT_TRUE(readiness.compiled);
        EXPECT_TRUE(readiness.hardwareSupported);
        EXPECT_TRUE(readiness.implementationSupported);
        EXPECT_TRUE(readiness.pipelineReady);
        EXPECT_TRUE(readiness.inputsReady);
        EXPECT_FALSE(readiness.executed);
        EXPECT_TRUE(receipt.available);
        EXPECT_TRUE(receipt.graphComposed);
        EXPECT_FALSE(receipt.executionSucceeded);
        EXPECT_FALSE(receipt.succeeded);

        RenderFeatureRegistry optionalRegistry;
        RenderFeatureDescriptor optionalDescriptor =
            MakeDescriptor(2, "optional-ready-not-executed");
        optionalDescriptor.requiredRecipes = 0;
        optionalDescriptor.optionalRecipes =
            ToMask(RenderPipelineRecipe::ForwardStandard);
        optionalDescriptor.fallbackPolicy =
            RenderFeatureFallbackPolicy::Skip;
        ASSERT_EQ(optionalRegistry.Register(MakeFeature(
                      std::move(optionalDescriptor), readyButNotExecuted)),
                  RenderFeatureRegistryCode::Success);
        ASSERT_EQ(optionalRegistry.DeclareImplementedRecipe(
                      RenderPipelineRecipe::ForwardStandard),
                  RenderFeatureRegistryCode::Success);
        ASSERT_EQ(optionalRegistry.Seal(), RenderFeatureRegistryCode::Success);
        ASSERT_TRUE(optionalRegistry.PrepareFrame(MakeValidContext())
                        .Succeeded());

        RenderGraph optionalGraph;
        const RenderGraphBlackboardProvenance optionalProvenance{
            optionalGraph.GetGraphIdentity(),
            optionalGraph.GetRecordingGeneration(),
            1,
            0,
            1};
        RenderFeatureGraphBuilder optionalBuilder(
            optionalGraph, optionalProvenance);
        RenderGraphBlackboard optionalBlackboard(optionalProvenance);
        ASSERT_EQ(optionalRegistry.AddPasses(
                      optionalBuilder, optionalBlackboard),
                  RenderFeatureRegistryCode::Success);
        const RenderExecutionReceipt optionalReceipt =
            optionalRegistry.CollectExecutionReceipt();
        ASSERT_EQ(optionalReceipt.features.size(), 1U);
        EXPECT_EQ(optionalReceipt.features[0].requirement,
                  RenderFeatureRequirement::Optional);
        EXPECT_FALSE(optionalReceipt.features[0].readiness.executed);
        EXPECT_FALSE(optionalReceipt.executionSucceeded);
    }

    TEST(RenderFeatureValidation,
         RegistryRejectsFeatureSelfReportedExecutionWithoutGraphPasses)
    {
        RenderFeatureRegistry registry;
        auto feature = MakeFeature(MakeDescriptor(1, "forged-execution"));
        feature->SetClaimExecutionInReceipt();
        ASSERT_EQ(registry.Register(std::move(feature)),
                  RenderFeatureRegistryCode::Success);
        ASSERT_EQ(registry.DeclareImplementedRecipe(
                      RenderPipelineRecipe::ForwardStandard),
                  RenderFeatureRegistryCode::Success);
        ASSERT_EQ(registry.Seal(), RenderFeatureRegistryCode::Success);
        ASSERT_TRUE(registry.PrepareFrame(MakeValidContext()).Succeeded());

        RenderGraph graph;
        const RenderGraphBlackboardProvenance provenance{
            graph.GetGraphIdentity(), graph.GetRecordingGeneration(), 1, 0, 1};
        RenderFeatureGraphBuilder builder(graph, provenance);
        RenderGraphBlackboard blackboard(provenance);
        ASSERT_EQ(registry.AddPasses(builder, blackboard),
                  RenderFeatureRegistryCode::Success);
        RenderGraphValidationAccess::Compile(graph);
        ASSERT_TRUE(graph.GetCompileStats().compileValid);
        ASSERT_EQ(registry.FinalizeGraphExecution(graph),
                  RenderFeatureRegistryCode::Success);

        const RenderExecutionReceipt receipt = registry.CollectExecutionReceipt();
        ASSERT_EQ(receipt.features.size(), 1U);
        EXPECT_EQ(receipt.features[0].graphPassCount, 0U);
        EXPECT_EQ(receipt.features[0].executedGraphPassCount, 0U);
        EXPECT_FALSE(receipt.features[0].readiness.executed);
        EXPECT_FALSE(receipt.executionSucceeded);
        EXPECT_EQ(receipt.diagnostic,
                  "forged-execution: Render feature self-reported execution; "
                  "graph evidence is registry-owned");
    }

    TEST(RenderFeatureValidation,
         RegistryCompletesOptInTypedOutputWithoutGraphWork)
    {
        RenderFeatureDescriptor descriptor =
            MakeDescriptor(1, "data-only-output");
        descriptor.producedResources.push_back(SceneTexturesContract);
        descriptor.allowsDataOnlyCompletion = true;
        auto feature = MakeFeature(std::move(descriptor), MakeReadyReadiness());
        FakeRenderFeature* const featureRaw = feature.get();
        feature->SetPublishSceneTextures();

        RenderFeatureRegistry registry;
        ASSERT_EQ(registry.Register(std::move(feature)),
                  RenderFeatureRegistryCode::Success);
        ASSERT_EQ(registry.DeclareImplementedRecipe(
                      RenderPipelineRecipe::ForwardStandard),
                  RenderFeatureRegistryCode::Success);
        ASSERT_EQ(registry.Seal(), RenderFeatureRegistryCode::Success);
        ASSERT_TRUE(registry.PrepareFrame(MakeValidContext()).Succeeded());

        RenderGraph graph;
        const RenderGraphBlackboardProvenance provenance{
            graph.GetGraphIdentity(), graph.GetRecordingGeneration(), 1, 0, 1};
        RenderFeatureGraphBuilder builder(graph, provenance);
        RenderGraphBlackboard blackboard(provenance);
        ASSERT_EQ(registry.AddPasses(builder, blackboard),
                  RenderFeatureRegistryCode::Success);
        EXPECT_EQ(graph.GetRecordedPassCount(), 0U);
        RenderGraphValidationAccess::Compile(graph);
        ASSERT_TRUE(graph.GetCompileStats().compileValid);
        NoOpCommandContext commands;
        RenderGraphValidationAccess::Execute(graph, commands);
        ASSERT_EQ(registry.FinalizeGraphExecution(graph),
                  RenderFeatureRegistryCode::Success);

        ASSERT_EQ(featureRaw->GetExecutionEvidenceCount(), 1U);
        const RenderFeatureGraphExecutionEvidence& evidence =
            featureRaw->GetLastExecutionEvidence();
        EXPECT_TRUE(evidence.contractCompletedWithoutGraphWork);
        EXPECT_FALSE(evidence.semanticExecutionSucceeded);
        EXPECT_FALSE(evidence.HasExactExecution());

        const RenderExecutionReceipt receipt = registry.CollectExecutionReceipt();
        ASSERT_EQ(receipt.features.size(), 1U);
        EXPECT_EQ(receipt.features[0].graphPassCount, 0U);
        EXPECT_EQ(receipt.features[0].executedGraphPassCount, 0U);
        EXPECT_TRUE(
            receipt.features[0].contractCompletedWithoutGraphWork);
        EXPECT_TRUE(receipt.features[0].readiness.executed);
        EXPECT_TRUE(receipt.executionSucceeded);
        EXPECT_TRUE(receipt.IsStructurallyValid());
    }

    TEST(RenderFeatureValidation,
         RegistryRejectsDataOnlyCompletionWithoutTypedOutput)
    {
        RenderFeatureDescriptor descriptor =
            MakeDescriptor(1, "unbound-data-only-output");
        descriptor.allowsDataOnlyCompletion = true;

        RenderFeatureRegistry registry;
        EXPECT_EQ(registry.Register(MakeFeature(std::move(descriptor))),
                  RenderFeatureRegistryCode::InvalidResourceContract);
    }

    TEST(RenderFeatureValidation,
         RegistryRejectsDefaultFeatureWithoutGraphWork)
    {
        RenderFeatureDescriptor descriptor =
            MakeDescriptor(1, "default-no-graph-work");
        descriptor.producedResources.push_back(SceneTexturesContract);
        auto feature = MakeFeature(std::move(descriptor), MakeReadyReadiness());
        FakeRenderFeature* const featureRaw = feature.get();
        feature->SetPublishSceneTextures();

        RenderFeatureRegistry registry;
        ASSERT_EQ(registry.Register(std::move(feature)),
                  RenderFeatureRegistryCode::Success);
        ASSERT_EQ(registry.DeclareImplementedRecipe(
                      RenderPipelineRecipe::ForwardStandard),
                  RenderFeatureRegistryCode::Success);
        ASSERT_EQ(registry.Seal(), RenderFeatureRegistryCode::Success);
        ASSERT_TRUE(registry.PrepareFrame(MakeValidContext()).Succeeded());

        RenderGraph graph;
        const RenderGraphBlackboardProvenance provenance{
            graph.GetGraphIdentity(), graph.GetRecordingGeneration(), 1, 0, 1};
        RenderFeatureGraphBuilder builder(graph, provenance);
        RenderGraphBlackboard blackboard(provenance);
        ASSERT_EQ(registry.AddPasses(builder, blackboard),
                  RenderFeatureRegistryCode::Success);
        RenderGraphValidationAccess::Compile(graph);
        ASSERT_TRUE(graph.GetCompileStats().compileValid);
        NoOpCommandContext commands;
        RenderGraphValidationAccess::Execute(graph, commands);
        ASSERT_EQ(registry.FinalizeGraphExecution(graph),
                  RenderFeatureRegistryCode::Success);

        ASSERT_EQ(featureRaw->GetExecutionEvidenceCount(), 1U);
        EXPECT_FALSE(featureRaw->GetLastExecutionEvidence()
                         .contractCompletedWithoutGraphWork);
        const RenderExecutionReceipt receipt = registry.CollectExecutionReceipt();
        ASSERT_EQ(receipt.features.size(), 1U);
        EXPECT_FALSE(
            receipt.features[0].contractCompletedWithoutGraphWork);
        EXPECT_FALSE(receipt.features[0].readiness.executed);
        EXPECT_FALSE(receipt.executionSucceeded);
    }

    TEST(RenderFeatureValidation,
         RegistryRejectsCulledRegisteredPassForDataOnlyCompletion)
    {
        RenderFeatureDescriptor descriptor =
            MakeDescriptor(1, "culled-data-only-work");
        descriptor.producedResources.push_back(SceneTexturesContract);
        descriptor.allowsDataOnlyCompletion = true;
        auto feature = MakeFeature(std::move(descriptor), MakeReadyReadiness());
        FakeRenderFeature* const featureRaw = feature.get();
        feature->SetPublishSceneTextures();
        feature->SetCulledPass();

        RenderFeatureRegistry registry;
        ASSERT_EQ(registry.Register(std::move(feature)),
                  RenderFeatureRegistryCode::Success);
        ASSERT_EQ(registry.DeclareImplementedRecipe(
                      RenderPipelineRecipe::ForwardStandard),
                  RenderFeatureRegistryCode::Success);
        ASSERT_EQ(registry.Seal(), RenderFeatureRegistryCode::Success);
        ASSERT_TRUE(registry.PrepareFrame(MakeValidContext()).Succeeded());

        RenderGraph graph;
        const RenderGraphBlackboardProvenance provenance{
            graph.GetGraphIdentity(), graph.GetRecordingGeneration(), 1, 0, 1};
        RenderFeatureGraphBuilder builder(graph, provenance);
        RenderGraphBlackboard blackboard(provenance);
        ASSERT_EQ(registry.AddPasses(builder, blackboard),
                  RenderFeatureRegistryCode::Success);
        ASSERT_EQ(graph.GetRecordedPassCount(), 1U);
        RenderGraphValidationAccess::Compile(graph);
        ASSERT_TRUE(graph.GetCompileStats().compileValid);
        ASSERT_EQ(graph.GetCompileStats().culledPasses, 1U);
        ASSERT_EQ(registry.FinalizeGraphExecution(graph),
                  RenderFeatureRegistryCode::Success);

        ASSERT_EQ(featureRaw->GetExecutionEvidenceCount(), 1U);
        EXPECT_FALSE(featureRaw->GetLastExecutionEvidence()
                         .contractCompletedWithoutGraphWork);
        const RenderExecutionReceipt receipt = registry.CollectExecutionReceipt();
        ASSERT_EQ(receipt.features.size(), 1U);
        EXPECT_EQ(receipt.features[0].graphPassCount, 0U);
        EXPECT_EQ(receipt.features[0].executedGraphPassCount, 0U);
        EXPECT_FALSE(
            receipt.features[0].contractCompletedWithoutGraphWork);
        EXPECT_FALSE(receipt.features[0].readiness.executed);
        EXPECT_FALSE(receipt.executionSucceeded);
        EXPECT_TRUE(receipt.IsStructurallyValid());
    }

    TEST(RenderFeatureValidation,
         RegistryRequiresFinalizedEvidenceForDataOnlyCompletion)
    {
        RenderFeatureDescriptor descriptor =
            MakeDescriptor(1, "unfinalized-data-only-output");
        descriptor.producedResources.push_back(SceneTexturesContract);
        descriptor.allowsDataOnlyCompletion = true;
        auto feature = MakeFeature(std::move(descriptor), MakeReadyReadiness());
        feature->SetPublishSceneTextures();

        RenderFeatureRegistry registry;
        ASSERT_EQ(registry.Register(std::move(feature)),
                  RenderFeatureRegistryCode::Success);
        ASSERT_EQ(registry.DeclareImplementedRecipe(
                      RenderPipelineRecipe::ForwardStandard),
                  RenderFeatureRegistryCode::Success);
        ASSERT_EQ(registry.Seal(), RenderFeatureRegistryCode::Success);
        ASSERT_TRUE(registry.PrepareFrame(MakeValidContext()).Succeeded());

        RenderGraph graph;
        const RenderGraphBlackboardProvenance provenance{
            graph.GetGraphIdentity(), graph.GetRecordingGeneration(), 1, 0, 1};
        RenderFeatureGraphBuilder builder(graph, provenance);
        RenderGraphBlackboard blackboard(provenance);
        ASSERT_EQ(registry.AddPasses(builder, blackboard),
                  RenderFeatureRegistryCode::Success);
        RenderGraphValidationAccess::Compile(graph);
        ASSERT_TRUE(graph.GetCompileStats().compileValid);

        const RenderExecutionReceipt receipt = registry.CollectExecutionReceipt();
        ASSERT_EQ(receipt.features.size(), 1U);
        EXPECT_FALSE(
            receipt.features[0].contractCompletedWithoutGraphWork);
        EXPECT_FALSE(receipt.features[0].readiness.executed);
        EXPECT_FALSE(receipt.executionSucceeded);
    }

    TEST(RenderFeatureValidation,
         RegistryRejectsExecutedCallbackThatReportsSemanticFailure)
    {
        RenderFeatureRegistry registry;
        RenderFeatureDescriptor descriptor =
            MakeDescriptor(1, "semantic-evidence");
        descriptor.producedResources.push_back(SceneTexturesContract);
        descriptor.allowsDataOnlyCompletion = true;
        auto feature = MakeFeature(std::move(descriptor), MakeReadyReadiness());
        FakeRenderFeature* const featureRaw = feature.get();
        feature->SetSemanticPass(true);
        feature->SetPublishSceneTextures();
        ASSERT_EQ(registry.Register(std::move(feature)),
                  RenderFeatureRegistryCode::Success);
        ASSERT_EQ(registry.DeclareImplementedRecipe(
                      RenderPipelineRecipe::ForwardStandard),
                  RenderFeatureRegistryCode::Success);
        ASSERT_EQ(registry.Seal(), RenderFeatureRegistryCode::Success);
        ASSERT_TRUE(registry.PrepareFrame(MakeValidContext()).Succeeded());

        RenderGraph graph;
        const RenderGraphBlackboardProvenance provenance{
            graph.GetGraphIdentity(), graph.GetRecordingGeneration(), 1, 0, 1};
        RenderFeatureGraphBuilder builder(graph, provenance);
        RenderGraphBlackboard blackboard(provenance);
        ASSERT_EQ(registry.AddPasses(builder, blackboard),
                  RenderFeatureRegistryCode::Success);
        EXPECT_EQ(featureRaw->GetExecutionEvidenceCount(), 0U);
        RenderGraphValidationAccess::Compile(graph);
        ASSERT_TRUE(graph.GetCompileStats().compileValid);
        NoOpCommandContext commands;
        RenderGraphValidationAccess::Execute(graph, commands);
        EXPECT_EQ(featureRaw->GetExecutionEvidenceCount(), 0U);

        const RenderGraph::Diagnostics diagnostics = graph.GetDiagnostics();
        ASSERT_EQ(diagnostics.passes.size(), 1U);
        EXPECT_TRUE(diagnostics.passes[0].executedLastRun);
        EXPECT_FALSE(
            diagnostics.passes[0].semanticExecutionSucceededLastRun);
        EXPECT_EQ(diagnostics.passes[0].semanticExecutionDiagnostic,
                  "required semantic workload was not recorded");
        ASSERT_EQ(registry.FinalizeGraphExecution(graph),
                  RenderFeatureRegistryCode::Success);
        ASSERT_EQ(featureRaw->GetExecutionEvidenceCount(), 1U);
        EXPECT_FALSE(
            featureRaw->GetLastExecutionEvidence().semanticExecutionSucceeded);
        EXPECT_FALSE(featureRaw->GetLastExecutionEvidence()
                         .contractCompletedWithoutGraphWork);
        EXPECT_FALSE(featureRaw->GetLastExecutionEvidence().HasExactExecution());
        EXPECT_EQ(featureRaw->GetLastExecutionEvidence().diagnostic,
                  "required semantic workload was not recorded");

        const RenderExecutionReceipt receipt =
            registry.CollectExecutionReceipt();
        ASSERT_EQ(receipt.features.size(), 1U);
        EXPECT_EQ(receipt.features[0].graphPassCount, 1U);
        EXPECT_EQ(receipt.features[0].executedGraphPassCount, 0U);
        EXPECT_FALSE(receipt.features[0].readiness.executed);
        EXPECT_FALSE(
            receipt.features[0].contractCompletedWithoutGraphWork);
        EXPECT_EQ(receipt.features[0].diagnostic,
                  "required semantic workload was not recorded");
        EXPECT_FALSE(receipt.executionSucceeded);
        EXPECT_EQ(receipt.diagnostic,
                  "semantic-evidence: required semantic workload was not recorded");
    }

    TEST(RenderFeatureValidation,
         RegistryFinalizesDetailedFeatureEvidenceOnlyAfterExactGraphExecution)
    {
        RenderFeatureRegistry registry;
        auto feature = MakeFeature(
            MakeDescriptor(1, "finalized-evidence"), MakeReadyReadiness());
        FakeRenderFeature* const featureRaw = feature.get();
        feature->SetSemanticPass(false);
        ASSERT_EQ(registry.Register(std::move(feature)),
                  RenderFeatureRegistryCode::Success);
        ASSERT_EQ(registry.DeclareImplementedRecipe(
                      RenderPipelineRecipe::ForwardStandard),
                  RenderFeatureRegistryCode::Success);
        ASSERT_EQ(registry.Seal(), RenderFeatureRegistryCode::Success);
        const RenderFeatureContext context = MakeValidContext();
        ASSERT_TRUE(registry.PrepareFrame(context).Succeeded());

        RenderGraph graph;
        const RenderGraphBlackboardProvenance provenance{
            graph.GetGraphIdentity(), graph.GetRecordingGeneration(), 1, 0, 1};
        RenderFeatureGraphBuilder builder(graph, provenance);
        RenderGraphBlackboard blackboard(provenance);
        ASSERT_EQ(registry.AddPasses(builder, blackboard),
                  RenderFeatureRegistryCode::Success);
        EXPECT_EQ(featureRaw->GetExecutionEvidenceCount(), 0U);

        RenderGraphValidationAccess::Compile(graph);
        ASSERT_TRUE(graph.GetCompileStats().compileValid);
        EXPECT_EQ(featureRaw->GetExecutionEvidenceCount(), 0U);

        NoOpCommandContext commands;
        RenderGraphValidationAccess::Execute(graph, commands);
        EXPECT_EQ(featureRaw->GetExecutionEvidenceCount(), 0U);

        ASSERT_EQ(registry.FinalizeGraphExecution(graph),
                  RenderFeatureRegistryCode::Success);
        ASSERT_EQ(featureRaw->GetExecutionEvidenceCount(), 1U);
        const RenderFeatureGraphExecutionEvidence& evidence =
            featureRaw->GetLastExecutionEvidence();
        EXPECT_EQ(evidence.featureId, RenderFeatureId{1});
        EXPECT_EQ(evidence.graphIdentity, graph.GetGraphIdentity());
        EXPECT_EQ(evidence.recordingGeneration,
                  graph.GetRecordingGeneration());
        EXPECT_EQ(evidence.frameIdentity, context.frameIdentity);
        EXPECT_EQ(evidence.viewIdentity, context.viewIdentity);
        EXPECT_EQ(evidence.graphPassCount, 1U);
        EXPECT_EQ(evidence.executedGraphPassCount, 1U);
        EXPECT_TRUE(evidence.semanticExecutionSucceeded);
        EXPECT_TRUE(evidence.HasExactExecution());
    }

    TEST(RenderFeatureValidation,
         RegistryBuildsStableResourceDependencyExecutionOrder)
    {
        RenderFeatureRegistry registry;
        RenderFeatureDescriptor independent = MakeDescriptor(10, "independent");
        RenderFeatureDescriptor producer = MakeDescriptor(30, "producer");
        producer.producedResources.push_back(SceneTexturesContract);
        RenderFeatureDescriptor consumer = MakeDescriptor(
            20, "consumer", RenderFeaturePhase::Geometry);
        consumer.consumedResources.push_back(SceneTexturesContract);
        EXPECT_EQ(registry.Register(MakeFeature(std::move(independent))),
                  RenderFeatureRegistryCode::Success);
        EXPECT_EQ(registry.Register(MakeFeature(std::move(producer))),
                  RenderFeatureRegistryCode::Success);
        EXPECT_EQ(registry.Register(MakeFeature(std::move(consumer))),
                  RenderFeatureRegistryCode::Success);
        EXPECT_EQ(registry.DeclareImplementedRecipe(
                      RenderPipelineRecipe::ForwardStandard),
                  RenderFeatureRegistryCode::Success);
        EXPECT_EQ(registry.Seal(), RenderFeatureRegistryCode::Success);

        const std::vector<RenderFeatureId>& order = registry.GetExecutionOrder();
        ASSERT_EQ(order.size(), 3U);
        EXPECT_EQ(order[0], RenderFeatureId{10});
        EXPECT_EQ(order[1], RenderFeatureId{30});
        EXPECT_EQ(order[2], RenderFeatureId{20});
    }

    TEST(RenderFeatureValidation,
         OptionalSelectedFeatureFailsClosedWhenCompositionLosesReadiness)
    {
        RenderFeatureDescriptor descriptor =
            MakeDescriptor(1, "optional-composition-failure");
        descriptor.requiredRecipes = 0;
        descriptor.optionalRecipes =
            ToMask(RenderPipelineRecipe::ForwardStandard);
        descriptor.fallbackPolicy = RenderFeatureFallbackPolicy::Skip;
        auto feature = MakeFeature(std::move(descriptor));
        RenderFeatureReadiness lostReadiness = MakeReadyReadiness();
        lostReadiness.pipelineReady = false;
        lostReadiness.diagnostic = "pipeline invalidated during composition";
        feature->SetReadinessAfterAddPasses(lostReadiness);

        RenderFeatureRegistry registry;
        ASSERT_EQ(registry.Register(std::move(feature)),
                  RenderFeatureRegistryCode::Success);
        ASSERT_EQ(registry.DeclareImplementedRecipe(
                      RenderPipelineRecipe::ForwardStandard),
                  RenderFeatureRegistryCode::Success);
        ASSERT_EQ(registry.Seal(), RenderFeatureRegistryCode::Success);
        ASSERT_TRUE(registry.PrepareFrame(MakeValidContext()).Succeeded());

        RenderGraph graph;
        const RenderGraphBlackboardProvenance provenance{
            graph.GetGraphIdentity(), graph.GetRecordingGeneration(), 1, 0, 1};
        RenderFeatureGraphBuilder builder(graph, provenance);
        RenderGraphBlackboard blackboard(provenance);
        EXPECT_EQ(registry.AddPasses(builder, blackboard),
                  RenderFeatureRegistryCode::GraphCompositionFailed);
        const RenderExecutionReceipt receipt =
            registry.CollectExecutionReceipt();
        EXPECT_FALSE(receipt.graphComposed);
        EXPECT_FALSE(receipt.executionSucceeded);
        EXPECT_EQ(receipt.diagnostic,
                  "pipeline invalidated during composition");
    }

    TEST(RenderFeatureValidation,
         ExecutionReceiptRequiresCoherentCandidateAndPresentationStates)
    {
        RenderExecutionBuildIdentity emptyBuild;
        EXPECT_TRUE(emptyBuild.IsEmpty());
        EXPECT_FALSE(emptyBuild.IsAvailable());
        EXPECT_FALSE(emptyBuild.IsStructurallyValid());

        RenderExecutionBuildIdentity validBuild{
            std::string(40, 'a'), "win_x64_pr_debug"};
        EXPECT_FALSE(validBuild.IsEmpty());
        EXPECT_TRUE(validBuild.IsAvailable());
        EXPECT_TRUE(validBuild.IsStructurallyValid());

        RenderExecutionBuildIdentity partialBuild{
            std::string(40, 'a'), {}};
        EXPECT_FALSE(partialBuild.IsStructurallyValid());
        RenderExecutionBuildIdentity malformedBuild{
            std::string(40, 'g'), "invalid/preset"};
        EXPECT_FALSE(malformedBuild.IsStructurallyValid());

        const RenderFeatureContext context = MakeValidContext();
        RenderExecutionReceipt candidate;
        candidate.available = true;
        candidate.graphComposed = true;
        candidate.executionSucceeded = true;
        candidate.presented = false;
        candidate.succeeded = false;
        candidate.frameIdentity = context.frameIdentity;
        candidate.viewIdentity = context.viewIdentity;
        candidate.backend = context.backend;
        candidate.requestedRecipe = RenderPipelineRecipe::ForwardStandard;
        candidate.resolvedRecipe = RenderPipelineRecipe::ForwardStandard;
        candidate.quality = RenderQualityTier::Medium;
        candidate.profileHash = ComputeRenderPipelineProfileHash(
            candidate.resolvedRecipe, candidate.quality);
        candidate.lighting.lightingPlanIdentity = 0x105ULL;
        candidate.lighting.sceneLightStateHash = 0x206ULL;
        candidate.lighting.primaryDirectionalLightId = 0x307ULL;
        candidate.lighting.pointLightRequestedCount = 3;
        candidate.lighting.pointLightAdmittedCount = 2;
        candidate.lighting.pointLightCapacity = 2;
        candidate.lighting.pointLightOverflowCount = 1;
        candidate.lighting.spotLightRequestedCount = 2;
        candidate.lighting.spotLightAdmittedCount = 2;
        candidate.lighting.spotLightCapacity = 4;
        candidate.lighting.spotLightOverflowCount = 0;
        candidate.lighting.csmRequested = true;
        candidate.lighting.csmInputsReady = true;
        candidate.lighting.csmExecutionReady = true;
        candidate.lighting.iblRequested = true;
        candidate.lighting.iblInputsReady = true;
        candidate.lighting.iblExecutionReady = true;
        candidate.deformation.deformationContentHash = 0x408ULL;
        candidate.deformation.skinnedObjectCount = 1;
        candidate.deformation.paletteMatrixCount = 64;
        candidate.deformation.previousPaletteObjectCount = 1;
        candidate.deformation.currentPaletteBufferReady = true;
        candidate.deformation.previousPaletteBufferReady = true;
        candidate.deformation.opaqueSkinnedDrawCount = 1;
        candidate.deformation.shadowSkinnedDrawCount = 2;
        candidate.deformation.velocitySkinnedDrawCount = 1;
        candidate.deformation.velocityPreviousPaletteDrawCount = 1;
        candidate.postProcess.postProcessContentHash = 0x509ULL;
        candidate.postProcess.toneMapPassCount = 1;
        candidate.postProcess.inputColorDomain =
            PostProcessColorDomain::SceneLinearHDR;
        candidate.postProcess.plannedOutputColorDomain =
            PostProcessColorDomain::DisplayEncodedLDR;
        candidate.postProcess.storedOutputColorDomain =
            PostProcessColorDomain::DisplayEncodedLDR;
        candidate.postProcess.finalOutputFormat = RHIFormat::RGBA8_UNORM;
        candidate.postProcess.toneMappingTransfer =
            PostProcessTransfer::SceneLinearToDisplayEncoded;
        candidate.postProcess.presentationTransfer =
            PostProcessTransfer::SceneLinearToDisplayEncoded;
        candidate.postProcess.hdrInputReady = true;
        candidate.postProcess.encodedOutputReady = true;
        PopulateValidManualExposureReceipt(candidate.postProcess);
        RenderFeatureExecutionReceipt executedFeature;
        executedFeature.featureId = RenderFeatureId{1};
        executedFeature.featureSchemaVersion = 1;
        executedFeature.selected = true;
        executedFeature.readiness = MakeReadyReadiness();
        executedFeature.readiness.executed = true;
        executedFeature.contractCompletedWithoutGraphWork = true;
        candidate.features.push_back(std::move(executedFeature));

        EXPECT_TRUE(candidate.IsStructurallyValid());
        EXPECT_FALSE(candidate.presented);
        EXPECT_FALSE(candidate.succeeded);

        candidate.ambientOcclusion.requested = true;
        candidate.ambientOcclusion.linearDepthInputReady = true;
        candidate.ambientOcclusion.worldNormalInputReady = true;
        candidate.ambientOcclusion.velocityInputReady = true;
        candidate.ambientOcclusion.velocityProducerExecuted = true;
        candidate.ambientOcclusion.pipelineReady = true;
        candidate.ambientOcclusion.inputsReady = true;
        candidate.ambientOcclusion.outputPublished = true;
        candidate.ambientOcclusion.executed = true;
        candidate.ambientOcclusion.historyCandidateProduced = true;
        candidate.ambientOcclusion.historyResetReasons =
            RenderHistoryResetReason::FirstFrame;
        candidate.ambientOcclusion.consumerMask =
            RVX_RENDER_AMBIENT_OCCLUSION_INDIRECT_CONSUMER_MASK;
        candidate.ambientOcclusion.outputFormat = RHIFormat::R8_UNORM;
        candidate.ambientOcclusion.horizonSearchGraphPassCount = 1;
        candidate.ambientOcclusion.bilateralDenoiseGraphPassCount = 2;
        candidate.ambientOcclusion.temporalResolveGraphPassCount = 1;
        candidate.ambientOcclusion.planContentHash = 0xA013ULL;
        candidate.ambientOcclusion.settingsHash = 0xA014ULL;
        candidate.ambientOcclusion.aoFieldContentHash = 0xA015ULL;
        candidate.ambientOcclusion.lightingConsumerExecuted = true;
        candidate.ambientOcclusion.lightingAOFieldContentHash = 0xA015ULL;
        candidate.ambientOcclusion.velocityFieldIdentity = 0xF013ULL;
        EXPECT_TRUE(candidate.ambientOcclusion.IsStructurallyValid());
        EXPECT_TRUE(candidate.IsStructurallyValid());

        RenderAmbientOcclusionExecutionReceipt noLightingWork =
            candidate.ambientOcclusion;
        noLightingWork.lightingConsumerExecuted = false;
        noLightingWork.lightingAOFieldContentHash = 0;
        EXPECT_TRUE(noLightingWork.IsStructurallyValid());

        RenderAmbientOcclusionExecutionReceipt mismatchedLightingField =
            candidate.ambientOcclusion;
        mismatchedLightingField.lightingAOFieldContentHash = 0xA016ULL;
        EXPECT_FALSE(mismatchedLightingField.IsStructurallyValid());

        RenderAmbientOcclusionExecutionReceipt unclaimedLightingField =
            candidate.ambientOcclusion;
        unclaimedLightingField.lightingConsumerExecuted = false;
        EXPECT_FALSE(unclaimedLightingField.IsStructurallyValid());

        RenderExecutionReceipt oldSchemaWithAO = candidate;
        oldSchemaWithAO.schemaVersion = 8;
        EXPECT_FALSE(oldSchemaWithAO.IsStructurallyValid());
        RenderExecutionReceipt schemaNineWithAO = candidate;
        schemaNineWithAO.schemaVersion = 9;
        EXPECT_FALSE(schemaNineWithAO.IsStructurallyValid());
        RenderExecutionReceipt invalidAOReceipt = candidate;
        invalidAOReceipt.ambientOcclusion.consumerMask =
            ToMask(RenderAmbientOcclusionConsumer::IBL);
        EXPECT_FALSE(invalidAOReceipt.IsStructurallyValid());
        invalidAOReceipt = candidate;
        invalidAOReceipt.ambientOcclusion.outputFormat = RHIFormat::R16_FLOAT;
        EXPECT_FALSE(invalidAOReceipt.IsStructurallyValid());
        invalidAOReceipt = candidate;
        invalidAOReceipt.ambientOcclusion.historySampled = true;
        EXPECT_FALSE(invalidAOReceipt.IsStructurallyValid());
        invalidAOReceipt = candidate;
        invalidAOReceipt.ambientOcclusion.velocityInputReady = false;
        EXPECT_FALSE(invalidAOReceipt.IsStructurallyValid());
        invalidAOReceipt = candidate;
        invalidAOReceipt.ambientOcclusion.velocityProducerExecuted = false;
        EXPECT_FALSE(invalidAOReceipt.IsStructurallyValid());
        invalidAOReceipt = candidate;
        invalidAOReceipt.ambientOcclusion.velocityFieldIdentity = 0;
        EXPECT_FALSE(invalidAOReceipt.IsStructurallyValid());
        invalidAOReceipt = candidate;
        invalidAOReceipt.ambientOcclusion.historyAvailableBefore = true;
        invalidAOReceipt.ambientOcclusion.historySampled = true;
        EXPECT_FALSE(invalidAOReceipt.IsStructurallyValid());
        invalidAOReceipt = candidate;
        invalidAOReceipt.ambientOcclusion.viewProjectionMotionObserved = true;
        EXPECT_FALSE(invalidAOReceipt.IsStructurallyValid());
        invalidAOReceipt = candidate;
        invalidAOReceipt.ambientOcclusion.historyAvailableBefore = true;
        invalidAOReceipt.ambientOcclusion.historySampled = true;
        invalidAOReceipt.ambientOcclusion.motionReprojectionUsed = true;
        invalidAOReceipt.ambientOcclusion.viewProjectionMotionObserved = true;
        invalidAOReceipt.ambientOcclusion.historyResetReasons =
            RenderHistoryResetReason::None;
        EXPECT_TRUE(invalidAOReceipt.IsStructurallyValid());

        RenderExecutionReceipt temporalCandidate = candidate;
        temporalCandidate.postProcess.antiAliasingMode =
            RenderAntiAliasingMode::TAA;
        temporalCandidate.postProcess.taaRequested = true;
        temporalCandidate.postProcess.taaPipelineReady = true;
        temporalCandidate.postProcess.taaInputsReady = true;
        temporalCandidate.postProcess.taaExecuted = true;
        temporalCandidate.postProcess.historyCandidateProduced = true;
        temporalCandidate.postProcess.historyResetReasons =
            RenderHistoryResetReason::FirstFrame;
        temporalCandidate.postProcess.taaPassCount = 1;
        temporalCandidate.postProcess.taaResolveGraphPassCount = 1;
        temporalCandidate.postProcess.taaHistoryStoreGraphPassCount = 1;
        temporalCandidate.postProcess.taaExposureStateHash =
            temporalCandidate.postProcess.exposureStateContentHash;
        EXPECT_TRUE(temporalCandidate.IsStructurallyValid());

        RenderExecutionReceipt legacySchemaWithTemporal = temporalCandidate;
        legacySchemaWithTemporal.schemaVersion = 6;
        EXPECT_FALSE(legacySchemaWithTemporal.IsStructurallyValid());

        temporalCandidate.presented = true;
        temporalCandidate.succeeded = true;
        EXPECT_FALSE(temporalCandidate.IsStructurallyValid());
        temporalCandidate.postProcess.historyCommitted = true;
        temporalCandidate.postProcess.exposureHistoryCommitted = true;
        EXPECT_TRUE(temporalCandidate.IsStructurallyValid());

        RenderExecutionReceipt presented = candidate;
        presented.presented = true;
        presented.succeeded = true;
        presented.postProcess.exposureHistoryCommitted = true;
        EXPECT_TRUE(presented.IsStructurallyValid());
        EXPECT_TRUE(presented.succeeded);

        const auto makeExposureProbeReady = [](RenderExecutionReceipt& receipt)
        {
            receipt.postProcess.exposureProbeRequested = true;
            receipt.postProcess.exposureProbeReady = true;
            receipt.postProcess.targetEV100 = 10.0f;
            receipt.postProcess.adaptedEV100 = 10.0f;
            receipt.postProcess.exposureScale = 0.0008138021f;
            receipt.postProcess.preExposure = 0.0008138021f;
            receipt.postProcess.previousPreExposure = 0.0008138021f;
        };
        RenderExecutionReceipt presentedWithExposureProbe = presented;
        makeExposureProbeReady(presentedWithExposureProbe);
        EXPECT_TRUE(presentedWithExposureProbe.IsStructurallyValid());

        const auto rejectsOutOfRangeExposureScale =
            [&presentedWithExposureProbe](
                float32 RenderPostProcessExecutionReceipt::* field,
                float32 value)
        {
            RenderExecutionReceipt invalid = presentedWithExposureProbe;
            invalid.postProcess.*field = value;
            return !invalid.IsStructurallyValid();
        };
        const float32 belowMinimumExposure = std::nextafter(
            RVX_RENDER_EXPOSURE_MIN_SCALE, 0.0f);
        const float32 aboveMaximumExposure = std::nextafter(
            RVX_RENDER_EXPOSURE_MAX_SCALE,
            std::numeric_limits<float32>::infinity());
        EXPECT_TRUE(rejectsOutOfRangeExposureScale(
            &RenderPostProcessExecutionReceipt::exposureScale,
            belowMinimumExposure));
        EXPECT_TRUE(rejectsOutOfRangeExposureScale(
            &RenderPostProcessExecutionReceipt::preExposure,
            belowMinimumExposure));
        EXPECT_TRUE(rejectsOutOfRangeExposureScale(
            &RenderPostProcessExecutionReceipt::previousPreExposure,
            belowMinimumExposure));
        EXPECT_TRUE(rejectsOutOfRangeExposureScale(
            &RenderPostProcessExecutionReceipt::exposureScale,
            aboveMaximumExposure));
        EXPECT_TRUE(rejectsOutOfRangeExposureScale(
            &RenderPostProcessExecutionReceipt::preExposure,
            aboveMaximumExposure));
        EXPECT_TRUE(rejectsOutOfRangeExposureScale(
            &RenderPostProcessExecutionReceipt::previousPreExposure,
            aboveMaximumExposure));

        RenderExecutionReceipt forgedUnpresentedExposureProbe =
            presentedWithExposureProbe;
        forgedUnpresentedExposureProbe.presented = false;
        forgedUnpresentedExposureProbe.succeeded = false;
        EXPECT_FALSE(forgedUnpresentedExposureProbe.IsStructurallyValid());

        RenderExecutionReceipt forgedUncommittedExposureProbe =
            presentedWithExposureProbe;
        forgedUncommittedExposureProbe.postProcess.exposureHistoryCommitted =
            false;
        EXPECT_FALSE(forgedUncommittedExposureProbe.IsStructurallyValid());

        RenderExecutionReceipt forgedAbandonedExposureProbe =
            presentedWithExposureProbe;
        forgedAbandonedExposureProbe.postProcess.exposureHistoryCommitted =
            false;
        forgedAbandonedExposureProbe.postProcess.exposureHistoryAbandoned =
            true;
        EXPECT_FALSE(forgedAbandonedExposureProbe.IsStructurallyValid());

        RenderExecutionReceipt rejectedPresentation = presented;
        rejectedPresentation.presented = false;
        rejectedPresentation.succeeded = false;
        rejectedPresentation.diagnostic =
            "Presented frame failed temporal history qualification";
        EXPECT_TRUE(rejectedPresentation.IsStructurallyValid());
        EXPECT_FALSE(rejectedPresentation.succeeded);

        RenderExecutionReceipt forgedSuccess = candidate;
        forgedSuccess.succeeded = true;
        EXPECT_FALSE(forgedSuccess.IsStructurallyValid());

        RenderExecutionReceipt unboundBackend = candidate;
        unboundBackend.backend = RHIBackendType::None;
        EXPECT_FALSE(unboundBackend.IsStructurallyValid());

        RenderExecutionReceipt partiallyBound = candidate;
        partiallyBound.build.sourceCommit = std::string(40, 'a');
        EXPECT_FALSE(partiallyBound.IsStructurallyValid());

        RenderExecutionReceipt duplicateFeature = candidate;
        duplicateFeature.features.push_back(duplicateFeature.features[0]);
        EXPECT_FALSE(duplicateFeature.IsStructurallyValid());

        RenderExecutionReceipt invalidPassCounts = candidate;
        invalidPassCounts.features[0].graphPassCount = 1;
        invalidPassCounts.features[0].executedGraphPassCount = 2;
        EXPECT_FALSE(invalidPassCounts.IsStructurallyValid());

        RenderExecutionReceipt legacyV11MissingDataOnlyEvidence = candidate;
        legacyV11MissingDataOnlyEvidence.schemaVersion = 11;
        legacyV11MissingDataOnlyEvidence.features[0]
            .contractCompletedWithoutGraphWork = false;
        EXPECT_FALSE(legacyV11MissingDataOnlyEvidence.IsStructurallyValid());

        RenderExecutionReceipt hybridWithoutGraphEvidence = candidate;
        hybridWithoutGraphEvidence.requestedRecipe =
            RenderPipelineRecipe::HybridRayTracing;
        hybridWithoutGraphEvidence.resolvedRecipe =
            RenderPipelineRecipe::HybridRayTracing;
        hybridWithoutGraphEvidence.profileHash = ComputeRenderPipelineProfileHash(
            hybridWithoutGraphEvidence.resolvedRecipe,
            hybridWithoutGraphEvidence.quality);
        RenderHybridRayTracingExecutionReceipt& hybrid =
            hybridWithoutGraphEvidence.hybridRayTracing;
        hybrid.available = true;
        hybrid.planIdentityHash = 0xB001ULL;
        hybrid.scenePreparationGeneration = 3;
        hybrid.sceneStructuralHash = 0xB002ULL;
        hybrid.frameIdentity = hybridWithoutGraphEvidence.frameIdentity;
        hybrid.viewIdentity = hybridWithoutGraphEvidence.viewIdentity;
        hybrid.fullResolution = {1920, 1080};
        hybrid.halfResolution = {960, 540};
        hybrid.recipe = hybridWithoutGraphEvidence.resolvedRecipe;
        hybrid.quality = hybridWithoutGraphEvidence.quality;
        hybrid.profileHash = hybridWithoutGraphEvidence.profileHash;
        hybrid.shadowRoute = RenderHybridRayTracingShadowRoute::RayTraced;
        hybrid.reflectionRoute =
            RenderHybridRayTracingReflectionRoute::RayTraced;
        EXPECT_FALSE(hybridWithoutGraphEvidence.IsStructurallyValid());

        hybridWithoutGraphEvidence.executionSucceeded = false;
        EXPECT_TRUE(hybridWithoutGraphEvidence.IsStructurallyValid());
        ++hybridWithoutGraphEvidence.hybridRayTracing.frameIdentity.sceneRevision;
        EXPECT_FALSE(hybridWithoutGraphEvidence.IsStructurallyValid());

        RenderExecutionReceipt nestedPresentationWithoutOuter = candidate;
        nestedPresentationWithoutOuter.requestedRecipe =
            RenderPipelineRecipe::HybridRayTracing;
        nestedPresentationWithoutOuter.resolvedRecipe =
            RenderPipelineRecipe::HybridRayTracing;
        nestedPresentationWithoutOuter.profileHash =
            ComputeRenderPipelineProfileHash(
                nestedPresentationWithoutOuter.resolvedRecipe,
                nestedPresentationWithoutOuter.quality);
        RenderHybridRayTracingExecutionReceipt& nestedHybrid =
            nestedPresentationWithoutOuter.hybridRayTracing;
        nestedHybrid = hybrid;
        nestedHybrid.frameIdentity =
            nestedPresentationWithoutOuter.frameIdentity;
        nestedHybrid.viewIdentity =
            nestedPresentationWithoutOuter.viewIdentity;
        nestedHybrid.profileHash = nestedPresentationWithoutOuter.profileHash;
        nestedHybrid.graphExecutionSucceeded = true;
        nestedHybrid.sceneBuildAccepted = true;
        nestedHybrid.shadowDispatchRecorded = true;
        nestedHybrid.reflectionDispatchRecorded = true;
        nestedHybrid.spatialDenoiseRecorded = true;
        nestedHybrid.temporalResolveRecorded = true;
        nestedHybrid.compositeRecorded = true;
        nestedHybrid.compositeUsedDenoisedSource = true;
        nestedHybrid.submissionAccepted = true;
        nestedHybrid.presentationQualified = true;
        nestedHybrid.succeeded = true;
        EXPECT_TRUE(nestedHybrid.IsStructurallyValid());
        EXPECT_FALSE(nestedPresentationWithoutOuter.IsStructurallyValid());

        RenderExecutionReceipt missingLightingIdentity = candidate;
        missingLightingIdentity.lighting.lightingPlanIdentity = 0;
        EXPECT_FALSE(missingLightingIdentity.IsStructurallyValid());

        RenderExecutionReceipt presentedWithoutLighting = candidate;
        presentedWithoutLighting.lighting = {};
        presentedWithoutLighting.presented = true;
        presentedWithoutLighting.succeeded = true;
        EXPECT_FALSE(presentedWithoutLighting.IsStructurallyValid());

        RenderExecutionReceipt executedWithoutLighting = candidate;
        executedWithoutLighting.lighting = {};
        EXPECT_FALSE(executedWithoutLighting.IsStructurallyValid());

        RenderExecutionReceipt forgedLightingOverflow = candidate;
        ++forgedLightingOverflow.lighting.pointLightOverflowCount;
        EXPECT_FALSE(forgedLightingOverflow.IsStructurallyValid());

        RenderExecutionReceipt forgedLightingReadiness = candidate;
        forgedLightingReadiness.lighting.iblRequested = false;
        EXPECT_FALSE(forgedLightingReadiness.IsStructurallyValid());

        RenderExecutionReceipt executedWithoutLightingInputs = candidate;
        executedWithoutLightingInputs.lighting.csmInputsReady = false;
        EXPECT_FALSE(executedWithoutLightingInputs.IsStructurallyValid());

        RenderExecutionReceipt missingDeformationHash = candidate;
        missingDeformationHash.deformation.deformationContentHash = 0;
        EXPECT_FALSE(missingDeformationHash.IsStructurallyValid());

        RenderExecutionReceipt missingCurrentPalette = candidate;
        missingCurrentPalette.deformation.currentPaletteBufferReady = false;
        EXPECT_FALSE(missingCurrentPalette.IsStructurallyValid());

        RenderExecutionReceipt missingPreviousHistory = candidate;
        missingPreviousHistory.deformation.previousPaletteObjectCount = 0;
        EXPECT_FALSE(missingPreviousHistory.IsStructurallyValid());

        RenderExecutionReceipt mismatchedVelocityEvidence = candidate;
        mismatchedVelocityEvidence.deformation
            .velocityPreviousPaletteDrawCount = 0;
        EXPECT_FALSE(mismatchedVelocityEvidence.IsStructurallyValid());

        RenderExecutionReceipt emptyDeformation = candidate;
        emptyDeformation.deformation = {};
        emptyDeformation.deformation.deformationContentHash = 0x409ULL;
        EXPECT_TRUE(emptyDeformation.IsStructurallyValid());

        RenderExecutionReceipt legacyV2 = candidate;
        legacyV2.schemaVersion = 2;
        legacyV2.features[0].contractCompletedWithoutGraphWork = false;
        legacyV2.ambientOcclusion = {};
        legacyV2.lighting = {};
        legacyV2.deformation = {};
        legacyV2.postProcess = {};
        EXPECT_TRUE(legacyV2.IsStructurallyValid());
        legacyV2.lighting = candidate.lighting;
        EXPECT_FALSE(legacyV2.IsStructurallyValid());

        RenderExecutionReceipt unreleasedV3 = candidate;
        unreleasedV3.schemaVersion = 3;
        EXPECT_FALSE(unreleasedV3.IsStructurallyValid());

        RenderExecutionReceipt legacyV4 = candidate;
        legacyV4.schemaVersion = 4;
        legacyV4.features[0].contractCompletedWithoutGraphWork = false;
        legacyV4.ambientOcclusion = {};
        legacyV4.deformation = {};
        legacyV4.postProcess = {};
        EXPECT_TRUE(legacyV4.IsStructurallyValid());
        legacyV4.deformation = candidate.deformation;
        EXPECT_FALSE(legacyV4.IsStructurallyValid());

        RenderExecutionReceipt legacyV1 = candidate;
        legacyV1.schemaVersion = 1;
        legacyV1.features[0].contractCompletedWithoutGraphWork = false;
        legacyV1.ambientOcclusion = {};
        legacyV1.deferredOpaque = {};
        legacyV1.lighting = {};
        legacyV1.deformation = {};
        legacyV1.postProcess = {};
        EXPECT_TRUE(legacyV1.IsStructurallyValid());
        legacyV1.lighting = candidate.lighting;
        EXPECT_FALSE(legacyV1.IsStructurallyValid());

        RenderExecutionReceipt emptySuccessful = candidate;
        emptySuccessful.features.clear();
        EXPECT_FALSE(emptySuccessful.IsStructurallyValid());
    }

    TEST(RenderFeatureValidation,
         LightingSurfaceEvidenceRejectsDuplicateRouteReceipts)
    {
        RenderPassRecordResults results;
        RenderLightingSurfaceExecutionEvidence first;
        first.lightingPlanIdentity = 7;
        first.route = RenderLightingSurfaceRoute::DeferredOpaque;
        first.frozenBufferMask = 0x3fu;
        first.ambientOcclusionFieldContentHash = 19;
        first.workCount = 11;
        first.executed = true;
        ASSERT_TRUE(results.RecordLightingSurfaceExecution(first));

        RenderLightingSurfaceExecutionEvidence replacement = first;
        replacement.workCount = 13;
        EXPECT_FALSE(results.RecordLightingSurfaceExecution(replacement));

        const size_t deferredIndex = static_cast<size_t>(
            RenderLightingSurfaceRoute::DeferredOpaque);
        EXPECT_EQ(11U, results.lightingSurfaceEvidence[deferredIndex].workCount);

        RenderLightingSurfaceExecutionEvidence separateRoute = first;
        separateRoute.route = RenderLightingSurfaceRoute::Transparent;
        separateRoute.ambientOcclusionFieldContentHash = 0;
        EXPECT_TRUE(results.RecordLightingSurfaceExecution(separateRoute));
    }

    TEST(RenderFeatureValidation,
         LegacyAdapterCountsExecutedGraphPassesAndPreservesStablePassOrder)
    {
        RenderGraph graph;
        const RenderPassRecordContext recordContext =
            MakeValidRecordContext(graph);
        DefaultRenderPassBundle passes;

        auto later = std::make_unique<TestLegacyPass>(
            "Later", 200, true, true, true);
        TestLegacyPass* const laterRaw = later.get();
        passes.AddPass(std::move(later), nullptr);

        auto unsupported = std::make_unique<TestLegacyPass>(
            "Unsupported", 75, true, false, false);
        passes.AddPass(std::move(unsupported), nullptr);

        auto disabled = std::make_unique<TestLegacyPass>(
            "Disabled", 50, false, true, false);
        passes.AddPass(std::move(disabled), nullptr);

        auto first = std::make_unique<TestLegacyPass>(
            "First", 100, true, true, true);
        TestLegacyPass* const firstRaw = first.get();
        passes.AddPass(std::move(first), nullptr);

        auto legacy = std::make_unique<LegacyForwardFeatureAdapter>(&passes);
        LegacyForwardFeatureAdapter* const legacyRaw = legacy.get();
        legacy->SetRecordContext(recordContext);

        RenderFeatureRegistry registry;
        ASSERT_EQ(registry.Register(std::move(legacy)),
                  RenderFeatureRegistryCode::Success);
        ASSERT_EQ(registry.DeclareImplementedRecipe(
                      RenderPipelineRecipe::ForwardStandard),
                  RenderFeatureRegistryCode::Success);
        ASSERT_EQ(registry.Seal(), RenderFeatureRegistryCode::Success);
        ASSERT_TRUE(registry.PrepareFrame(MakeValidContext()).Succeeded());

        const RenderGraphBlackboardProvenance provenance{
            recordContext.identity.graphIdentity,
            recordContext.identity.graphRecordingGeneration,
            recordContext.identity.frameSequence,
            recordContext.identity.viewOrdinal,
            recordContext.identity.recordEpoch};
        RenderFeatureGraphBuilder builder(graph, provenance);
        RenderGraphBlackboard blackboard(provenance);
        ASSERT_EQ(registry.AddPasses(builder, blackboard),
                  RenderFeatureRegistryCode::Success);

        RenderGraphBlackboardCode sceneTexturesCode =
            RenderGraphBlackboardCode::Success;
        RenderFeatureDescriptor sceneTexturesConsumer = MakeDescriptor(
            99, "scene-textures-consumer", RenderFeaturePhase::Lighting);
        sceneTexturesConsumer.consumedResources.push_back(
            SceneTexturesContract);
        const SceneTextures* const sceneTextures =
            blackboard.Require<SceneTextures>(
                sceneTexturesConsumer.featureId, sceneTexturesConsumer,
                &sceneTexturesCode);
        ASSERT_EQ(sceneTexturesCode, RenderGraphBlackboardCode::Success);
        ASSERT_NE(sceneTextures, nullptr);
        EXPECT_TRUE(sceneTextures->hdrSceneColor.IsValid());
        EXPECT_EQ(sceneTextures->hdrSceneColorMetadata,
                  MakeSceneTextureMetadata(
                      SceneTextures::HDRSceneColorContract));
        EXPECT_TRUE(sceneTextures->deviceDepth.IsValid());
        EXPECT_EQ(sceneTextures->deviceDepth.index,
                  recordContext.view.depthTarget.index);
        EXPECT_EQ(sceneTextures->deviceDepthMetadata,
                  MakeSceneTextureMetadata(
                      SceneTextures::DeviceDepthContract));

        const LegacyForwardFeaturePassChainStats chain =
            legacyRaw->GetPassChainStatsSnapshot();
        EXPECT_EQ(chain.registeredPassCount, 4U);
        EXPECT_EQ(chain.skippedDisabledPassCount, 1U);
        EXPECT_EQ(chain.skippedUnsupportedPassCount, 1U);
        EXPECT_EQ(chain.graphPassCount, 2U);
        EXPECT_EQ(chain.graphPassNames,
                  (std::vector<std::string>{"First", "Later"}));
        EXPECT_EQ(chain.unsupportedPassNames,
                  (std::vector<std::string>{"Unsupported"}));
        EXPECT_TRUE(firstRaw->ReceivedMatchingContext());
        EXPECT_TRUE(laterRaw->ReceivedMatchingContext());

        const RenderExecutionReceipt beforeExecution =
            registry.CollectExecutionReceipt(
                RenderExecutionBuildIdentity{
                    std::string(40, 'a'), "test_preset"});
        ASSERT_EQ(beforeExecution.features.size(), 1U);
        EXPECT_TRUE(beforeExecution.available);
        EXPECT_TRUE(beforeExecution.graphComposed);
        EXPECT_FALSE(beforeExecution.features[0].readiness.executed);
        EXPECT_EQ(beforeExecution.features[0].graphPassCount, 2U);
        EXPECT_EQ(beforeExecution.features[0].executedGraphPassCount, 0U);
        EXPECT_FALSE(beforeExecution.executionSucceeded);
        EXPECT_FALSE(beforeExecution.presented);
        EXPECT_FALSE(beforeExecution.succeeded);

        RenderGraphValidationAccess::Compile(graph);
        ASSERT_TRUE(graph.GetCompileStats().compileValid);
        NoOpCommandContext commands;
        RenderGraphValidationAccess::Execute(graph, commands);
        EXPECT_EQ(firstRaw->GetExecuteCount(), 1U);
        EXPECT_EQ(laterRaw->GetExecuteCount(), 1U);
        EXPECT_EQ(graph.GetCompileStats().lastExecutedPassCount, 2U);
        ASSERT_EQ(registry.FinalizeGraphExecution(graph),
                  RenderFeatureRegistryCode::Success);

        const RenderExecutionReceipt candidate =
            registry.CollectExecutionReceipt(
                RenderExecutionBuildIdentity{
                    std::string(40, 'a'), "test_preset"});
        ASSERT_EQ(candidate.features.size(), 1U);
        EXPECT_TRUE(candidate.features[0].readiness.executed);
        EXPECT_EQ(candidate.features[0].graphPassCount, 2U);
        EXPECT_EQ(candidate.features[0].executedGraphPassCount, 2U);
        EXPECT_TRUE(candidate.executionSucceeded);
        EXPECT_FALSE(candidate.presented);
        EXPECT_FALSE(candidate.succeeded);
        EXPECT_TRUE(candidate.IsStructurallyValid());
    }

    TEST(RenderFeatureValidation,
         LegacyAdapterRegistersCanonicalVelocityProducersBeforePublishingSceneTextures)
    {
        RenderGraph graph;
        RenderPassRecordContext recordContext = MakeValidRecordContext(graph);
        recordContext.view.velocityTarget = MakeVelocityTarget(graph);
        ASSERT_TRUE(recordContext.view.velocityTarget.IsValid());

        DefaultRenderPassBundle passes;
        auto camera = std::make_unique<TestCameraVelocityPass>();
        TestCameraVelocityPass* const cameraRaw = camera.get();
        passes.AddPass(std::move(camera), nullptr);
        auto object = std::make_unique<TestObjectVelocityPass>();
        TestObjectVelocityPass* const objectRaw = object.get();
        passes.AddPass(std::move(object), nullptr);

        LegacyForwardFeatureAdapter adapter(&passes);
        adapter.SetRecordContext(recordContext);
        adapter.Prepare(MakeValidContext());
        ASSERT_TRUE(adapter.ResolveReadiness().IsReadyToExecute());

        const RenderGraphBlackboardProvenance provenance{
            recordContext.identity.graphIdentity,
            recordContext.identity.graphRecordingGeneration,
            recordContext.identity.frameSequence,
            recordContext.identity.viewOrdinal,
            recordContext.identity.recordEpoch};
        RenderFeatureGraphBuilder builder(graph, provenance);
        RenderGraphBlackboard blackboard(provenance);
        adapter.AddPasses(builder, blackboard);

        ASSERT_TRUE(adapter.ResolveReadiness().IsReadyToExecute());
        const LegacyForwardFeaturePassChainStats chain =
            adapter.GetPassChainStatsSnapshot();
        EXPECT_EQ(chain.registeredPassCount, 2U);
        EXPECT_EQ(chain.graphPassCount, 2U);
        EXPECT_EQ(chain.graphPassNames,
                  (std::vector<std::string>{"CameraVelocityPass",
                                            "ObjectVelocityPass"}));
        EXPECT_EQ(graph.GetRecordedPassCount(), 2U);
        EXPECT_TRUE(cameraRaw->ReceivedMatchingContext());
        EXPECT_TRUE(objectRaw->ReceivedMatchingContext());

        RenderFeatureDescriptor consumer = MakeDescriptor(
            98, "scene-textures-consumer", RenderFeaturePhase::Lighting);
        consumer.consumedResources.push_back(SceneTexturesContract);
        RenderGraphBlackboardCode code = RenderGraphBlackboardCode::Success;
        const SceneTextures* const sceneTextures =
            blackboard.Require<SceneTextures>(
                consumer.featureId, consumer, &code);
        ASSERT_EQ(code, RenderGraphBlackboardCode::Success);
        ASSERT_NE(sceneTextures, nullptr);
        EXPECT_EQ(sceneTextures->velocity.index,
                  recordContext.view.velocityTarget.index);
        EXPECT_EQ(sceneTextures->velocityMetadata,
                  MakeSceneTextureMetadata(
                      SceneTextures::VelocityContract));

        // Concrete CameraVelocityPass/ObjectVelocityPass integration tests
        // own GPU execution evidence.  This adapter-level test intentionally
        // stops at composition because the transient target has no test RHI
        // device; its contract is atomic ordered registration and publication.
    }

    TEST(RenderFeatureValidation,
         LegacyAdapterRejectsNonCanonicalVelocityTargetBeforePublication)
    {
        RenderGraph graph;
        RenderPassRecordContext recordContext = MakeValidRecordContext(graph);
        recordContext.view.velocityTarget = MakeVelocityTarget(
            graph, RHIFormat::RGBA16_FLOAT);
        ASSERT_TRUE(recordContext.view.velocityTarget.IsValid());

        DefaultRenderPassBundle passes;
        auto camera = std::make_unique<TestCameraVelocityPass>();
        passes.AddPass(std::move(camera), nullptr);
        auto object = std::make_unique<TestObjectVelocityPass>();
        passes.AddPass(std::move(object), nullptr);

        LegacyForwardFeatureAdapter adapter(&passes);
        adapter.SetRecordContext(recordContext);
        adapter.Prepare(MakeValidContext());
        ASSERT_TRUE(adapter.ResolveReadiness().IsReadyToExecute());

        const RenderGraphBlackboardProvenance provenance{
            recordContext.identity.graphIdentity,
            recordContext.identity.graphRecordingGeneration,
            recordContext.identity.frameSequence,
            recordContext.identity.viewOrdinal,
            recordContext.identity.recordEpoch};
        RenderFeatureGraphBuilder builder(graph, provenance);
        RenderGraphBlackboard blackboard(provenance);
        adapter.AddPasses(builder, blackboard);

        EXPECT_FALSE(adapter.ResolveReadiness().inputsReady);
        EXPECT_EQ(adapter.ResolveReadiness().diagnostic,
                  "Requested velocity producers require a full-resolution "
                  "RG16_FLOAT target");
        EXPECT_EQ(adapter.GetPassChainStatsSnapshot().graphPassCount, 0U);
        EXPECT_EQ(graph.GetRecordedPassCount(), 0U);

        RenderFeatureDescriptor consumer = MakeDescriptor(
            99, "scene-textures-consumer", RenderFeaturePhase::Lighting);
        consumer.consumedResources.push_back(SceneTexturesContract);
        RenderGraphBlackboardCode code = RenderGraphBlackboardCode::Success;
        EXPECT_EQ(blackboard.Require<SceneTextures>(
                      consumer.featureId, consumer, &code),
                  nullptr);
        EXPECT_EQ(code, RenderGraphBlackboardCode::MissingResource);
    }

    TEST(RenderFeatureValidation,
         LegacyAdapterRejectsIncompleteVelocityPairWithoutPartialGraphMutation)
    {
        RenderGraph graph;
        RenderPassRecordContext recordContext = MakeValidRecordContext(graph);
        recordContext.view.velocityTarget = MakeVelocityTarget(graph);
        ASSERT_TRUE(recordContext.view.velocityTarget.IsValid());

        DefaultRenderPassBundle passes;
        auto camera = std::make_unique<TestCameraVelocityPass>();
        passes.AddPass(std::move(camera), nullptr);
        auto object = std::make_unique<TestObjectVelocityPass>(false);
        passes.AddPass(std::move(object), nullptr);

        LegacyForwardFeatureAdapter adapter(&passes);
        adapter.SetRecordContext(recordContext);
        adapter.Prepare(MakeValidContext());
        ASSERT_TRUE(adapter.ResolveReadiness().IsReadyToExecute());

        const RenderGraphBlackboardProvenance provenance{
            recordContext.identity.graphIdentity,
            recordContext.identity.graphRecordingGeneration,
            recordContext.identity.frameSequence,
            recordContext.identity.viewOrdinal,
            recordContext.identity.recordEpoch};
        RenderFeatureGraphBuilder builder(graph, provenance);
        RenderGraphBlackboard blackboard(provenance);
        adapter.AddPasses(builder, blackboard);

        EXPECT_FALSE(adapter.ResolveReadiness().inputsReady);
        EXPECT_NE(adapter.ResolveReadiness().diagnostic.find(
                      "ObjectVelocityPass"),
                  std::string::npos);
        EXPECT_EQ(adapter.GetPassChainStatsSnapshot().graphPassCount, 0U);
        EXPECT_EQ(graph.GetRecordedPassCount(), 0U);

        RenderFeatureDescriptor consumer = MakeDescriptor(
            100, "scene-textures-consumer", RenderFeaturePhase::Lighting);
        consumer.consumedResources.push_back(SceneTexturesContract);
        RenderGraphBlackboardCode code = RenderGraphBlackboardCode::Success;
        EXPECT_EQ(blackboard.Require<SceneTextures>(
                      consumer.featureId, consumer, &code),
                  nullptr);
        EXPECT_EQ(code, RenderGraphBlackboardCode::MissingResource);
    }

    TEST(RenderFeatureValidation,
         LegacyAdapterExcludesCulledEmptyWorkFromExecutionEvidence)
    {
        ScopedTestLog log;
        RenderGraph graph;
        const RenderPassRecordContext recordContext =
            MakeValidRecordContext(graph);
        DefaultRenderPassBundle passes;

        auto working = std::make_unique<TestLegacyPass>(
            "Working", 10, true, true, true);
        passes.AddPass(std::move(working), nullptr);
        auto empty = std::make_unique<TestLegacyPass>(
            "Empty", 20, true, true, true, false);
        passes.AddPass(std::move(empty), nullptr);

        auto legacy = std::make_unique<LegacyForwardFeatureAdapter>(&passes);
        legacy->SetRecordContext(recordContext);

        RenderFeatureRegistry registry;
        ASSERT_EQ(registry.Register(std::move(legacy)),
                  RenderFeatureRegistryCode::Success);
        ASSERT_EQ(registry.DeclareImplementedRecipe(
                      RenderPipelineRecipe::ForwardStandard),
                  RenderFeatureRegistryCode::Success);
        ASSERT_EQ(registry.Seal(), RenderFeatureRegistryCode::Success);
        ASSERT_TRUE(registry.PrepareFrame(MakeValidContext()).Succeeded());

        const RenderGraphBlackboardProvenance provenance{
            recordContext.identity.graphIdentity,
            recordContext.identity.graphRecordingGeneration,
            recordContext.identity.frameSequence,
            recordContext.identity.viewOrdinal,
            recordContext.identity.recordEpoch};
        RenderFeatureGraphBuilder builder(graph, provenance);
        RenderGraphBlackboard blackboard(provenance);
        ASSERT_EQ(registry.AddPasses(builder, blackboard),
                  RenderFeatureRegistryCode::Success);

        RenderGraphValidationAccess::Compile(graph);
        ASSERT_TRUE(graph.GetCompileStats().compileValid);
        ASSERT_EQ(registry.FinalizeGraphExecution(graph),
                  RenderFeatureRegistryCode::Success);

        const RenderExecutionReceipt receipt =
            registry.CollectExecutionReceipt();
        ASSERT_EQ(receipt.features.size(), 1U);
        EXPECT_EQ(receipt.features[0].graphPassCount, 1U);
        EXPECT_EQ(receipt.features[0].executedGraphPassCount, 0U);
        EXPECT_EQ(receipt.features[0].graphPassNames,
                  (std::vector<std::string>{"Working"}));
        // The adapter explicitly allows data-only completion, but it did
        // register an Empty graph node. A culled registration therefore must
        // remain a fail-closed non-execution outcome.
        EXPECT_FALSE(
            receipt.features[0].contractCompletedWithoutGraphWork);
        EXPECT_FALSE(receipt.features[0].readiness.executed);
        EXPECT_FALSE(receipt.executionSucceeded);
        EXPECT_TRUE(receipt.IsStructurallyValid());
    }

    TEST(RenderFeatureValidation,
         LegacyAdapterRejectsStaleFrameViewAndRecordEpochContexts)
    {
        RenderGraph graph;
        DefaultRenderPassBundle passes;
        LegacyForwardFeatureAdapter adapter(&passes);
        const RenderFeatureContext featureContext = MakeValidContext();

        RenderPassRecordContext wrongFrame = MakeValidRecordContext(graph);
        ++wrongFrame.featureFrameIdentity.sceneRevision;
        adapter.SetRecordContext(std::move(wrongFrame));
        adapter.Prepare(featureContext);
        EXPECT_FALSE(adapter.ResolveReadiness().inputsReady);

        RenderPassRecordContext wrongView = MakeValidRecordContext(graph);
        ++wrongView.featureViewIdentity.cameraEntityPacked;
        adapter.SetRecordContext(std::move(wrongView));
        adapter.Prepare(featureContext);
        EXPECT_FALSE(adapter.ResolveReadiness().inputsReady);

        RenderPassRecordContext wrongEpoch = MakeValidRecordContext(graph);
        ++wrongEpoch.identity.recordEpoch;
        adapter.SetRecordContext(std::move(wrongEpoch));
        adapter.Prepare(featureContext);
        EXPECT_FALSE(adapter.ResolveReadiness().inputsReady);

        adapter.SetRecordContext(MakeValidRecordContext(graph));
        adapter.Prepare(featureContext);
        EXPECT_TRUE(adapter.ResolveReadiness().inputsReady);
    }

    TEST(RenderFeatureValidation,
         LegacyAdapterReportsZeroRegisteredAndExecutedGraphPasses)
    {
        RenderGraph graph;
        const RenderPassRecordContext recordContext =
            MakeValidRecordContext(graph);
        DefaultRenderPassBundle passes;
        passes.AddPass(std::make_unique<TestLegacyPass>(
                          "NotRequested", 10, false, true, false),
                      nullptr);
        passes.AddPass(std::make_unique<TestLegacyPass>(
                          "Unsupported", 20, true, false, false),
                      nullptr);

        auto legacy = std::make_unique<LegacyForwardFeatureAdapter>(&passes);
        LegacyForwardFeatureAdapter* const legacyRaw = legacy.get();
        legacy->SetRecordContext(recordContext);

        RenderFeatureRegistry registry;
        ASSERT_EQ(registry.Register(std::move(legacy)),
                  RenderFeatureRegistryCode::Success);
        ASSERT_EQ(registry.DeclareImplementedRecipe(
                      RenderPipelineRecipe::ForwardStandard),
                  RenderFeatureRegistryCode::Success);
        ASSERT_EQ(registry.Seal(), RenderFeatureRegistryCode::Success);
        ASSERT_TRUE(registry.PrepareFrame(MakeValidContext()).Succeeded());

        const RenderGraphBlackboardProvenance provenance{
            recordContext.identity.graphIdentity,
            recordContext.identity.graphRecordingGeneration,
            recordContext.identity.frameSequence,
            recordContext.identity.viewOrdinal,
            recordContext.identity.recordEpoch};
        RenderFeatureGraphBuilder builder(graph, provenance);
        RenderGraphBlackboard blackboard(provenance);
        ASSERT_EQ(registry.AddPasses(builder, blackboard),
                  RenderFeatureRegistryCode::Success);
        RenderGraphValidationAccess::Compile(graph);
        ASSERT_TRUE(graph.GetCompileStats().compileValid);
        NoOpCommandContext commands;
        RenderGraphValidationAccess::Execute(graph, commands);
        ASSERT_EQ(registry.FinalizeGraphExecution(graph),
                  RenderFeatureRegistryCode::Success);

        const LegacyForwardFeaturePassChainStats chain =
            legacyRaw->GetPassChainStatsSnapshot();
        EXPECT_EQ(chain.graphPassCount, 0U);
        EXPECT_EQ(chain.skippedDisabledPassCount, 1U);
        EXPECT_EQ(chain.skippedUnsupportedPassCount, 1U);
        EXPECT_TRUE(chain.graphPassNames.empty());
        EXPECT_EQ(graph.GetCompileStats().lastExecutedPassCount, 0U);

        const RenderExecutionReceipt receipt = registry.CollectExecutionReceipt();
        ASSERT_EQ(receipt.features.size(), 1U);
        EXPECT_EQ(receipt.features[0].graphPassCount, 0U);
        EXPECT_EQ(receipt.features[0].executedGraphPassCount, 0U);
        EXPECT_TRUE(
            receipt.features[0].contractCompletedWithoutGraphWork);
        EXPECT_TRUE(receipt.features[0].readiness.executed);
        EXPECT_TRUE(receipt.graphComposed);
        EXPECT_TRUE(receipt.executionSucceeded);
        EXPECT_FALSE(receipt.presented);
        EXPECT_FALSE(receipt.succeeded);
    }

    TEST(RenderFeatureValidation,
         LegacyAdapterReceiptRetainsFinalizedEvidenceAfterGraphDestruction)
    {
        DefaultRenderPassBundle passes;
        passes.AddPass(std::make_unique<TestLegacyPass>(
                          "Working", 10, true, true, true),
                      nullptr);

        auto recordedGraph = std::make_unique<RenderGraph>();
        const RenderPassRecordContext recordContext =
            MakeValidRecordContext(*recordedGraph);
        auto legacy = std::make_unique<LegacyForwardFeatureAdapter>(&passes);
        legacy->SetRecordContext(recordContext);
        RenderFeatureRegistry registry;
        ASSERT_EQ(registry.Register(std::move(legacy)),
                  RenderFeatureRegistryCode::Success);
        ASSERT_EQ(registry.DeclareImplementedRecipe(
                      RenderPipelineRecipe::ForwardStandard),
                  RenderFeatureRegistryCode::Success);
        ASSERT_EQ(registry.Seal(), RenderFeatureRegistryCode::Success);
        ASSERT_TRUE(registry.PrepareFrame(MakeValidContext()).Succeeded());

        {
            const RenderGraphBlackboardProvenance provenance{
                recordContext.identity.graphIdentity,
                recordContext.identity.graphRecordingGeneration,
                recordContext.identity.frameSequence,
                recordContext.identity.viewOrdinal,
                recordContext.identity.recordEpoch};
            RenderFeatureGraphBuilder builder(*recordedGraph, provenance);
            RenderGraphBlackboard blackboard(provenance);
            ASSERT_EQ(registry.AddPasses(builder, blackboard),
                      RenderFeatureRegistryCode::Success);
        }

        RenderGraphValidationAccess::Compile(*recordedGraph);
        ASSERT_TRUE(recordedGraph->GetCompileStats().compileValid);
        NoOpCommandContext commands;
        RenderGraphValidationAccess::Execute(*recordedGraph, commands);
        ASSERT_EQ(registry.FinalizeGraphExecution(*recordedGraph),
                  RenderFeatureRegistryCode::Success);
        recordedGraph.reset();

        const RenderExecutionReceipt receipt = registry.CollectExecutionReceipt();
        ASSERT_EQ(receipt.features.size(), 1U);
        EXPECT_EQ(receipt.features[0].graphPassCount, 1U);
        EXPECT_EQ(receipt.features[0].executedGraphPassCount, 1U);
        EXPECT_EQ(receipt.features[0].graphPassNames,
                  (std::vector<std::string>{"Working"}));
        EXPECT_TRUE(receipt.features[0].readiness.executed);
        EXPECT_TRUE(receipt.executionSucceeded);
        EXPECT_TRUE(receipt.diagnostic.empty());
    }

    TEST(RenderFeatureValidation,
         RegistryRejectsReplacementGraphAndLegacyReceiptRemainsLifetimeSafe)
    {
        DefaultRenderPassBundle passes;
        passes.AddPass(std::make_unique<TestLegacyPass>(
                          "Working", 10, true, true, true),
                      nullptr);

        auto recordedGraph = std::make_unique<RenderGraph>();
        const RenderPassRecordContext recordContext =
            MakeValidRecordContext(*recordedGraph);
        auto legacy = std::make_unique<LegacyForwardFeatureAdapter>(&passes);
        legacy->SetRecordContext(recordContext);
        RenderFeatureRegistry registry;
        ASSERT_EQ(registry.Register(std::move(legacy)),
                  RenderFeatureRegistryCode::Success);
        ASSERT_EQ(registry.DeclareImplementedRecipe(
                      RenderPipelineRecipe::ForwardStandard),
                  RenderFeatureRegistryCode::Success);
        ASSERT_EQ(registry.Seal(), RenderFeatureRegistryCode::Success);
        ASSERT_TRUE(registry.PrepareFrame(MakeValidContext()).Succeeded());

        {
            const RenderGraphBlackboardProvenance provenance{
                recordContext.identity.graphIdentity,
                recordContext.identity.graphRecordingGeneration,
                recordContext.identity.frameSequence,
                recordContext.identity.viewOrdinal,
                recordContext.identity.recordEpoch};
            RenderFeatureGraphBuilder builder(*recordedGraph, provenance);
            RenderGraphBlackboard blackboard(provenance);
            ASSERT_EQ(registry.AddPasses(builder, blackboard),
                      RenderFeatureRegistryCode::Success);
        }

        RenderGraph replacementGraph;
        EXPECT_EQ(registry.FinalizeGraphExecution(replacementGraph),
                  RenderFeatureRegistryCode::GraphCompositionFailed);
        recordedGraph.reset();

        const RenderExecutionReceipt receipt = registry.CollectExecutionReceipt();
        ASSERT_EQ(receipt.features.size(), 1U);
        EXPECT_EQ(receipt.features[0].graphPassCount, 1U);
        EXPECT_EQ(receipt.features[0].executedGraphPassCount, 0U);
        EXPECT_FALSE(receipt.features[0].readiness.executed);
        EXPECT_FALSE(receipt.executionSucceeded);
        EXPECT_EQ(receipt.diagnostic,
                  "Render feature graph execution evidence does not match the "
                  "composed graph");
    }

    TEST(RenderFeatureValidation,
        LegacyAdapterDoesNotRetainRemovedOpaquePass)
    {
        ScopedTestLog log;
        DefaultRenderPassBundle passes;
        // This test exercises ownership invalidation only. Avoid invoking the
        // native OpaquePass::OnAdd path with a deliberately absent device.
        passes.GetRegistry().GetPasses().push_back(
            std::make_unique<OpaquePass>());
        LegacyForwardFeatureAdapter adapter(&passes);
        ASSERT_NE(passes.FindPass<OpaquePass>(), nullptr);
        ASSERT_TRUE(passes.RemovePass("OpaquePass"));
        EXPECT_EQ(passes.FindPass<OpaquePass>(), nullptr);

        // The static ownership gate proves that the adapter cannot cache a
        // concrete pass pointer. Recording after invalidation proves that the
        // adapter resolves the live bundle instead of dereferencing stale
        // state retained from construction.
        RenderGraph graph;
        adapter.SetRecordContext(MakeValidRecordContext(graph));
        adapter.Prepare(MakeValidContext());
        const RenderGraphBlackboardProvenance provenance{
            graph.GetGraphIdentity(), graph.GetRecordingGeneration(), 1, 0, 1};
        RenderFeatureGraphBuilder builder(graph, provenance);
        RenderGraphBlackboard blackboard(provenance);
        adapter.AddPasses(builder, blackboard);
        EXPECT_TRUE(adapter.ResolveReadiness().IsReadyToExecute());
        EXPECT_EQ(adapter.CollectReceipt().graphPassCount, 0U);
    }

    TEST(RenderFeatureValidation,
         DefaultCompositionPreservesPreInitializePassesDuringInstallation)
    {
        DefaultRenderComposition composition;
        auto customPass = std::make_unique<TestLegacyPass>(
            "PreInitializeCustom", PassPriority::Opaque, true, true, true);
        IRenderPass* const customIdentity = customPass.get();
        composition.GetPassBundle().AddPass(std::move(customPass), nullptr);

        auto defaultBundle = std::make_unique<DefaultRenderPassBundle>();
        auto defaultPass = std::make_unique<TestLegacyPass>(
            "DefaultOwned", PassPriority::Opaque, true, true, true);
        IRenderPass* const defaultIdentity = defaultPass.get();
        defaultBundle->AddPass(std::move(defaultPass), nullptr);

        composition.InstallDefaultPassBundle(std::move(defaultBundle));

        const auto& installed =
            composition.GetPassBundle().GetRegistry().GetPasses();
        ASSERT_EQ(installed.size(), 2U);
        EXPECT_EQ(installed[0].get(), customIdentity);
        EXPECT_STREQ(installed[0]->GetName(), "PreInitializeCustom");
        EXPECT_EQ(installed[1].get(), defaultIdentity);
        EXPECT_STREQ(installed[1]->GetName(), "DefaultOwned");

        auto duplicateDefaults = std::make_unique<DefaultRenderPassBundle>();
        duplicateDefaults->AddPass(
            std::make_unique<TestLegacyPass>(
                "DuplicateDefault", PassPriority::Opaque,
                true, true, true),
            nullptr);
        composition.ResetFeatures();
        composition.InstallDefaultPassBundle(std::move(duplicateDefaults));
        EXPECT_EQ(composition.GetPassBundle().GetPassCount(), 2U);
        EXPECT_EQ(composition.GetPassBundle().GetRegistry().GetPasses()[0].get(),
                  customIdentity);
    }

    TEST(RenderFeatureValidation,
         DefaultPassBundleMoveAndDestructionBalancePassLifecycle)
    {
        auto originalLifecycle =
            std::make_shared<TestLegacyPassLifecycle>();
        auto replacementLifecycle =
            std::make_shared<TestLegacyPassLifecycle>();

        {
            DefaultRenderPassBundle source;
            source.AddPass(
                std::make_unique<TestLegacyPass>(
                    "Original", PassPriority::Opaque,
                    true, true, true, true, originalLifecycle),
                nullptr);
            EXPECT_EQ(originalLifecycle->onAddCount, 1U);

            DefaultRenderPassBundle destination(std::move(source));
            DefaultRenderPassBundle replacement;
            replacement.AddPass(
                std::make_unique<TestLegacyPass>(
                    "Replacement", PassPriority::Opaque,
                    true, true, true, true, replacementLifecycle),
                nullptr);

            destination = std::move(replacement);
            EXPECT_EQ(originalLifecycle->onRemoveCount, 1U);
            EXPECT_EQ(replacementLifecycle->onAddCount, 1U);
            EXPECT_EQ(replacementLifecycle->onRemoveCount, 0U);
        }

        EXPECT_EQ(originalLifecycle->onRemoveCount, 1U);
        EXPECT_EQ(replacementLifecycle->onRemoveCount, 1U);
    }

    TEST(RenderFeatureValidation,
         SceneRendererBalancesPreInitializePassLifecycleOnShutdown)
    {
        ScopedTestLog log;
        auto lifecycle = std::make_shared<TestLegacyPassLifecycle>();

        {
            SceneRenderer renderer;
            renderer.AddPass(
                std::make_unique<TestLegacyPass>(
                    "PreInitialize", PassPriority::Opaque,
                    true, true, true, true, lifecycle));
            EXPECT_EQ(lifecycle->onAddCount, 1U);
            EXPECT_EQ(lifecycle->onRemoveCount, 0U);

            renderer.Shutdown();
            EXPECT_EQ(lifecycle->onRemoveCount, 1U);
            renderer.Shutdown();
            EXPECT_EQ(lifecycle->onRemoveCount, 1U);
        }

        EXPECT_EQ(lifecycle->onRemoveCount, 1U);
    }

    TEST(RenderFeatureValidation,
         ToonLightingReceiptRequiresExecutedNonFallbackMaterialEvidence)
    {
        RenderToonLightingExecutionReceipt empty;
        EXPECT_TRUE(empty.IsEmpty());
        EXPECT_TRUE(empty.IsStructurallyValid());

        RenderToonLightingExecutionReceipt receipt;
        receipt.requested = true;
        receipt.pipelineReady = true;
        receipt.inputsReady = true;
        receipt.executed = true;
        receipt.programFamilyHash = 0x544F4F4E4C495431ull;
        receipt.materialCount = 1;
        receipt.drawCount = 2;
        receipt.deferredShadedDrawCount = 2;
        receipt.bandsApplied = true;
        receipt.specularThresholdApplied = true;
        receipt.rimApplied = true;
        receipt.shadowTintApplied = true;
        receipt.materialTableSchemaVersion = 2;
        receipt.materialTableContentHash = 0x4D41545441424C45ull;
        EXPECT_TRUE(receipt.IsStructurallyValid());

        RenderToonLightingExecutionReceipt fallback = receipt;
        fallback.standardLitFallbackCount = 1;
        EXPECT_FALSE(fallback.IsStructurallyValid());
        fallback = receipt;
        fallback.materialFallbackCount = 1;
        EXPECT_FALSE(fallback.IsStructurallyValid());
        fallback = receipt;
        fallback.forwardShadedDrawCount = 1;
        EXPECT_FALSE(fallback.IsStructurallyValid());
        fallback = receipt;
        fallback.materialTableContentHash = 0;
        EXPECT_FALSE(fallback.IsStructurallyValid());

        OpaqueMaterialTableExecutionIdentity tableIdentity{
            1, 2, sizeof(MaterialGPUConstants), 1, receipt.materialTableContentHash};
        EXPECT_TRUE(tableIdentity.IsStructurallyValid());
        tableIdentity.contentHash = 0;
        EXPECT_FALSE(tableIdentity.IsStructurallyValid());

        PostProcessMaterialTableMetadata outlineMetadata{
            PostProcessMaterialTableMetadata::SchemaVersion,
            static_cast<uint32>(sizeof(MaterialGPUConstants)),
            2,
            1,
            receipt.materialTableContentHash,
            {1280, 720},
            RHISampleCount::Count1};
        EXPECT_TRUE(outlineMetadata.IsValid());
        outlineMetadata.eligibleToonMaterialCount = 3;
        EXPECT_FALSE(outlineMetadata.IsValid());
    }

    TEST(RenderFeatureValidation,
         ToonOutlineReceiptRejectsPlannedButUnrecordedExecutionEvidence)
    {
        RenderToonOutlineExecutionReceipt receipt;
        receipt.requested = true;
        receipt.pipelineReady = true;
        receipt.inputsReady = true;
        receipt.depthInputReady = true;
        receipt.worldNormalInputReady = true;
        receipt.materialIdentityInputReady = true;
        receipt.materialTableInputReady = true;
        receipt.settingsHash = 0x544F4F4E4F5554ull;
        receipt.fallbackUsed = true;
        receipt.diagnostic =
            "ScreenSpaceOutline was scheduled but its exact visible draw was not recorded";
        EXPECT_TRUE(receipt.IsStructurallyValid());
        EXPECT_FALSE(receipt.IsExecutionReady());

        RenderToonOutlineExecutionReceipt plannedOnly = receipt;
        plannedOnly.outlineSequenceIndex = 2u;
        plannedOnly.toneMapSequenceIndex = 3u;
        plannedOnly.temporalOrderingValidated = true;
        EXPECT_FALSE(plannedOnly.IsStructurallyValid());

        RenderToonOutlineExecutionReceipt executed = receipt;
        executed.executed = true;
        executed.fallbackUsed = false;
        executed.diagnostic.clear();
        executed.taaSequenceIndex = 1u;
        executed.outlineSequenceIndex = 2u;
        executed.toneMapSequenceIndex = 3u;
        executed.temporalOrderingValidated = true;
        executed.eligibleToonMaterialCount = 1u;
        executed.materialTableContentHash = 0x4D41545441424C45ull;
        EXPECT_TRUE(executed.IsStructurallyValid());
        EXPECT_TRUE(executed.IsExecutionReady());
    }
} // namespace
} // namespace RVX
