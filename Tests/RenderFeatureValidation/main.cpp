#include "Core/Log.h"
#include "Render/Features/IRenderFeature.h"
#include "Render/Features/RenderFeatureRegistry.h"
#include "Render/Features/RenderGraphBlackboard.h"
#include "Render/Graph/RenderGraph.h"
#include "Render/Passes/OpaquePass.h"
#include "RHI/RHICommandContext.h"

#include "Features/DeferredRecipeFeatureAdapter.h"
#include "Features/LegacyForwardFeatureAdapter.h"
#include "Renderer/RenderPassRegistry.h"
#include "../Common/RenderGraphValidationAccess.h"

#include <algorithm>
#include <gtest/gtest.h>

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

    class ScopedTestLog final
    {
    public:
        ScopedTestLog() { Log::Initialize(); }
        ~ScopedTestLog() { Log::Shutdown(); }
    };

    class TestBuffer final : public RHIBuffer
    {
    public:
        TestBuffer()
        {
            m_desc.size = 256;
            m_desc.stride = 16;
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

    class TestLegacyPass final : public IRenderPass
    {
    public:
        TestLegacyPass(std::string name,
                       int32 priority,
                       bool requestedEnabled,
                       bool supported,
                       bool enabled,
                       bool recordsResourceUsage = true)
            : m_name(std::move(name))
            , m_priority(priority)
            , m_requestedEnabled(requestedEnabled)
            , m_supported(supported)
            , m_enabled(enabled)
            , m_recordsResourceUsage(recordsResourceUsage)
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
        std::string m_unsupportedReason = "Test unsupported legacy pass";
        RHIBufferRef m_buffer{};
        uint32 m_executeCount = 0;
        bool m_receivedMatchingContext = false;
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
        bool m_addSemanticPass = false;
        bool m_failSemanticPass = false;
        RHIBufferRef m_semanticBuffer{new TestBuffer()};
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

    [[nodiscard]] RenderPassRecordContext MakeValidRecordContext(
        RenderGraph& graph)
    {
        ViewData view;
        view.frameNumber = 1;
        RenderPassRecordContext context =
            MakeRenderPassRecordContext(graph, view);
        context.featureFrameIdentity = RenderFrameIdentity{1, 2, 3, 4};
        context.featureViewIdentity =
            RenderViewIdentity{5, 6, 0, ViewRole::Main};
        return context;
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
        return textures;
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
        validContext.snapshotInputs =
            MakeSnapshotInputs(MakeCompleteFeatureSnapshot());
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
         DeferredRecipeAdaptersDeclareOneWayGBufferLightingAndSpecialSurfaceFlow)
    {
        DeferredGeometryFeatureAdapter geometry(nullptr, nullptr, nullptr);
        DeferredLightingFeatureAdapter lighting(nullptr, nullptr);
        DeferredForwardSpecialFeatureAdapter special(nullptr, nullptr);

        const RenderFeatureDescriptor geometryDescriptor = geometry.Describe();
        const RenderFeatureDescriptor lightingDescriptor = lighting.Describe();
        const RenderFeatureDescriptor specialDescriptor = special.Describe();

        EXPECT_EQ(geometryDescriptor.featureId,
                  BuiltinRenderFeatureIds::DeferredGeometry);
        EXPECT_EQ(lightingDescriptor.featureId,
                  BuiltinRenderFeatureIds::DeferredLighting);
        EXPECT_EQ(specialDescriptor.featureId,
                  BuiltinRenderFeatureIds::DeferredForwardSpecialChain);
        EXPECT_EQ(geometryDescriptor.requiredRecipes,
                  ToMask(RenderPipelineRecipe::DeferredQuality));
        EXPECT_EQ(lightingDescriptor.requiredRecipes,
                  ToMask(RenderPipelineRecipe::DeferredQuality));
        EXPECT_EQ(specialDescriptor.requiredRecipes,
                  ToMask(RenderPipelineRecipe::DeferredQuality));
        EXPECT_EQ(geometryDescriptor.optionalRecipes, 0U);
        EXPECT_EQ(lightingDescriptor.optionalRecipes, 0U);
        EXPECT_EQ(specialDescriptor.optionalRecipes, 0U);
        EXPECT_TRUE(std::find(geometryDescriptor.producedResources.begin(),
                              geometryDescriptor.producedResources.end(),
                              SceneTexturesContract) !=
                    geometryDescriptor.producedResources.end());
        EXPECT_TRUE(std::find(lightingDescriptor.consumedResources.begin(),
                              lightingDescriptor.consumedResources.end(),
                              SceneTexturesContract) !=
                    lightingDescriptor.consumedResources.end());
        EXPECT_TRUE(std::find(lightingDescriptor.producedResources.begin(),
                              lightingDescriptor.producedResources.end(),
                              LightingOutputsContract) !=
                    lightingDescriptor.producedResources.end());
        EXPECT_TRUE(std::find(specialDescriptor.consumedResources.begin(),
                              specialDescriptor.consumedResources.end(),
                              LightingOutputsContract) !=
                    specialDescriptor.consumedResources.end());

        EXPECT_NE(geometryDescriptor.requiredCapabilities &
                      ToMask(RenderFeatureCapability::MultipleRenderTargets),
                  0U);
        EXPECT_NE(lightingDescriptor.requiredCapabilities &
                      ToMask(RenderFeatureCapability::Compute),
                  0U);
        EXPECT_NE(lightingDescriptor.requiredCapabilities &
                      ToMask(RenderFeatureCapability::StorageTexture),
                  0U);
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
        multisampled.gBuffer3Metadata.sampleCount = RHISampleCount::Count4;
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
                  "Render feature self-reported execution; graph evidence is "
                  "registry-owned");
    }

    TEST(RenderFeatureValidation,
         RegistryRejectsExecutedCallbackThatReportsSemanticFailure)
    {
        RenderFeatureRegistry registry;
        auto feature = MakeFeature(
            MakeDescriptor(1, "semantic-evidence"), MakeReadyReadiness());
        feature->SetSemanticPass(true);
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

        const RenderGraph::Diagnostics diagnostics = graph.GetDiagnostics();
        ASSERT_EQ(diagnostics.passes.size(), 1U);
        EXPECT_TRUE(diagnostics.passes[0].executedLastRun);
        EXPECT_FALSE(
            diagnostics.passes[0].semanticExecutionSucceededLastRun);
        EXPECT_EQ(diagnostics.passes[0].semanticExecutionDiagnostic,
                  "required semantic workload was not recorded");
        ASSERT_EQ(registry.FinalizeGraphExecution(graph),
                  RenderFeatureRegistryCode::Success);

        const RenderExecutionReceipt receipt =
            registry.CollectExecutionReceipt();
        ASSERT_EQ(receipt.features.size(), 1U);
        EXPECT_EQ(receipt.features[0].graphPassCount, 1U);
        EXPECT_EQ(receipt.features[0].executedGraphPassCount, 0U);
        EXPECT_FALSE(receipt.features[0].readiness.executed);
        EXPECT_EQ(receipt.features[0].diagnostic,
                  "required semantic workload was not recorded");
        EXPECT_FALSE(receipt.executionSucceeded);
        EXPECT_EQ(receipt.diagnostic,
                  "required semantic workload was not recorded");
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
        RenderFeatureExecutionReceipt executedFeature;
        executedFeature.featureId = RenderFeatureId{1};
        executedFeature.featureSchemaVersion = 1;
        executedFeature.selected = true;
        executedFeature.readiness = MakeReadyReadiness();
        executedFeature.readiness.executed = true;
        candidate.features.push_back(std::move(executedFeature));

        EXPECT_TRUE(candidate.IsStructurallyValid());
        EXPECT_FALSE(candidate.presented);
        EXPECT_FALSE(candidate.succeeded);

        RenderExecutionReceipt presented = candidate;
        presented.presented = true;
        presented.succeeded = true;
        EXPECT_TRUE(presented.IsStructurallyValid());
        EXPECT_TRUE(presented.succeeded);

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

        RenderExecutionReceipt emptySuccessful = candidate;
        emptySuccessful.features.clear();
        EXPECT_FALSE(emptySuccessful.IsStructurallyValid());
    }

    TEST(RenderFeatureValidation,
         LegacyAdapterCountsExecutedGraphPassesAndPreservesStablePassOrder)
    {
        RenderGraph graph;
        const RenderPassRecordContext recordContext =
            MakeValidRecordContext(graph);
        RenderPassRegistry passes;

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

        auto legacy = std::make_unique<LegacyForwardFeatureAdapter>(
            &passes, nullptr);
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
         LegacyAdapterExcludesCulledEmptyWorkFromExecutionEvidence)
    {
        ScopedTestLog log;
        RenderGraph graph;
        const RenderPassRecordContext recordContext =
            MakeValidRecordContext(graph);
        RenderPassRegistry passes;

        auto working = std::make_unique<TestLegacyPass>(
            "Working", 10, true, true, true);
        passes.AddPass(std::move(working), nullptr);
        auto empty = std::make_unique<TestLegacyPass>(
            "Empty", 20, true, true, true, false);
        passes.AddPass(std::move(empty), nullptr);

        auto legacy = std::make_unique<LegacyForwardFeatureAdapter>(
            &passes, nullptr);
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
        EXPECT_FALSE(receipt.features[0].readiness.executed);
        EXPECT_FALSE(receipt.executionSucceeded);
        EXPECT_TRUE(receipt.IsStructurallyValid());
    }

    TEST(RenderFeatureValidation,
         LegacyAdapterRejectsStaleFrameViewAndRecordEpochContexts)
    {
        RenderGraph graph;
        RenderPassRegistry passes;
        LegacyForwardFeatureAdapter adapter(&passes, nullptr);
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
        RenderPassRegistry passes;
        passes.AddPass(std::make_unique<TestLegacyPass>(
                          "NotRequested", 10, false, true, false),
                      nullptr);
        passes.AddPass(std::make_unique<TestLegacyPass>(
                          "Unsupported", 20, true, false, false),
                      nullptr);

        auto legacy = std::make_unique<LegacyForwardFeatureAdapter>(
            &passes, nullptr);
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
        EXPECT_FALSE(receipt.features[0].readiness.executed);
        EXPECT_TRUE(receipt.graphComposed);
        EXPECT_FALSE(receipt.executionSucceeded);
        EXPECT_FALSE(receipt.presented);
        EXPECT_FALSE(receipt.succeeded);
    }

    TEST(RenderFeatureValidation,
         LegacyAdapterReceiptRetainsFinalizedEvidenceAfterGraphDestruction)
    {
        RenderPassRegistry passes;
        passes.AddPass(std::make_unique<TestLegacyPass>(
                          "Working", 10, true, true, true),
                      nullptr);

        auto recordedGraph = std::make_unique<RenderGraph>();
        const RenderPassRecordContext recordContext =
            MakeValidRecordContext(*recordedGraph);
        auto legacy = std::make_unique<LegacyForwardFeatureAdapter>(
            &passes, nullptr);
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
        RenderPassRegistry passes;
        passes.AddPass(std::make_unique<TestLegacyPass>(
                          "Working", 10, true, true, true),
                      nullptr);

        auto recordedGraph = std::make_unique<RenderGraph>();
        const RenderPassRecordContext recordContext =
            MakeValidRecordContext(*recordedGraph);
        auto legacy = std::make_unique<LegacyForwardFeatureAdapter>(
            &passes, nullptr);
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
         LegacyAdapterAcceptsClearedOpaquePointerBeforeOwnerTeardown)
    {
        RenderPassRegistry passes;
        auto opaque = std::make_unique<OpaquePass>();
        OpaquePass* const opaqueRaw = opaque.get();

        LegacyForwardFeatureAdapter adapter(&passes, opaqueRaw);
        adapter.SetOpaquePass(nullptr);
        opaque.reset();

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
} // namespace
} // namespace RVX
