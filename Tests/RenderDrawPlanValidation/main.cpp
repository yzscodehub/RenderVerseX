#include "Render/Passes/MeshPassProcessor.h"
#include "Render/Policy/RenderFramePlanCompiler.h"
#include "Render/Policy/RenderPolicyResolver.h"
#include "Render/Renderer/RenderDrawPlan.h"
#include "Render/Renderer/RenderScene.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <set>
#include <utility>
#include <vector>

using namespace RVX;

namespace
{
    constexpr uint64 kFrameSequence = 701;
    constexpr uint32 kViewOrdinal = 3;

    class TestClassificationBindingResolver final
        : public IRenderDrawClassificationBindingResolver
    {
    public:
        void Resolve(RenderDrawPacket& packet) const override
        {
            packet.arguments.indexCount += 17;
            packet.arguments.firstIndex += 5;
            packet.arguments.vertexOffset += 3;
            packet.materialInstanceKey.textureBindingHash = 0x9a71U;
            packet.materialInstanceKey.parameterTableCompatible = true;
        }
    };

    class SurfacePathBindingResolver final
        : public IRenderDrawClassificationBindingResolver
    {
    public:
        void Resolve(RenderDrawPacket& packet) const override
        {
            switch (packet.objectId)
            {
                case 101:
                case 102:
                    packet.pipelineKey.surfacePathEligibility =
                        SurfacePathEligibility::DeferredOpaque;
                    break;
                case 103:
                case 104:
                    packet.pipelineKey.surfacePathEligibility =
                        SurfacePathEligibility::ForwardTranslucency;
                    break;
                default:
                    packet.pipelineKey.surfacePathEligibility =
                        SurfacePathEligibility::Unsupported;
                    break;
            }
        }
    };

    MeshBatch MakeBatch(uint64 objectId,
                        uint32 objectIndex,
                        uint32 submeshIndex,
                        RenderMaterialMode mode,
                        RenderResourceHandle mesh,
                        RenderResourceHandle material)
    {
        MeshBatch batch;
        batch.objectId = objectId;
        batch.mesh = mesh;
        batch.material = material;
        batch.submeshIndex = submeshIndex;
        batch.primitiveData = objectIndex;
        batch.indexType = MeshUploadIndexType::UInt32;
        batch.geometry = {
            submeshIndex * 3U,
            3U,
            0,
            MeshUploadPrimitiveTopology::Triangles};
        batch.materialMode = mode;
        batch.flags = RenderBatchFlags::CastsShadow |
                      RenderBatchFlags::ReceivesShadow;
        if (mode == RenderMaterialMode::Masked)
        {
            batch.flags |= RenderBatchFlags::Masked;
        }
        if (mode == RenderMaterialMode::Transparent)
        {
            batch.flags |= RenderBatchFlags::Transparent;
        }
        return batch;
    }

    RenderObject MakeObject(uint64 objectId,
                            uint32 objectIndex,
                            uint32 layerMask,
                            const Vec3& position,
                            RenderMaterialMode mode,
                            uint32 meshSlot,
                            uint32 materialSlot)
    {
        RenderObject object;
        object.entityId = objectId;
        object.worldMatrix = MakeTranslation(position);
        object.bounds = AABB(position - Vec3{0.5f}, position + Vec3{0.5f});
        object.mesh = {meshSlot, 1};
        object.material = {materialSlot, 1};
        object.layerMask = layerMask;
        object.drawable = true;
        object.visible = true;
        object.castsShadow = true;
        object.receivesShadow = true;
        object.meshBatchesAuthoritative = true;
        object.meshBatches.push_back(MakeBatch(objectId,
                                                objectIndex,
                                                0,
                                                mode,
                                                object.mesh,
                                                object.material));
        return object;
    }

    RenderScene MakeScene()
    {
        RenderScene scene;
        scene.AddObject(MakeObject(101,
                                   0,
                                   0x1U,
                                   Vec3{0.0f, 0.0f, 1.0f},
                                   RenderMaterialMode::Opaque,
                                   11,
                                   21));
        scene.AddObject(MakeObject(102,
                                   1,
                                   0x2U,
                                   Vec3{0.0f, 0.0f, 2.0f},
                                   RenderMaterialMode::Masked,
                                   12,
                                   22));
        scene.AddObject(MakeObject(103,
                                   2,
                                   0x1U,
                                   Vec3{0.0f, 0.0f, 30.0f},
                                   RenderMaterialMode::Transparent,
                                   13,
                                   23));
        scene.AddObject(MakeObject(104,
                                   3,
                                   0x1U,
                                   Vec3{0.0f, 0.0f, 10.0f},
                                   RenderMaterialMode::Transparent,
                                   14,
                                   24));
        return scene;
    }

    MeshPassProcessorInput MakeInput(
        const RenderSceneDrawClassificationEntry& entry,
        uint32 sourceOrdinal,
        float32 viewDepth = 0.0f)
    {
        MeshPassProcessorInput input;
        input.packet = entry.draw.packet;
        input.sourceOrdinal = sourceOrdinal;
        input.viewDepth = viewDepth;
        input.classificationIndex = entry.classificationIndex;
        return input;
    }

    SceneMeshPassPreparation MakePreparation(
        const RenderSceneDrawClassification& classification)
    {
        SceneMeshPassPreparation preparation;
        DepthMeshPassProcessor depth;
        OpaqueMeshPassProcessor opaque;
        TransparentMeshPassProcessor transparent;
        ShadowMeshPassProcessor shadow;

        uint32 depthOpaqueOrdinal = 0;
        uint32 transparentOrdinal = 0;
        uint32 shadowOrdinal = 0;
        for (const RenderSceneDrawClassificationEntry& entry :
             classification.entries)
        {
            const MaterialRenderMode mode = entry.draw.renderMode;
            if (mode != RenderMaterialMode::Transparent)
            {
                const MeshPassProcessorInput input = MakeInput(
                    entry, depthOpaqueOrdinal++, entry.draw.depthFromCamera);
                preparation.depth.Record(depth.Process(input));
                preparation.opaque.Record(opaque.Process(input));
            }
            else
            {
                const MeshPassProcessorInput input = MakeInput(
                    entry,
                    transparentOrdinal++,
                    entry.draw.depthFromCamera);
                preparation.transparent.Record(transparent.Process(input));
            }

            const MeshPassProcessorInput shadowInput = MakeInput(
                entry, shadowOrdinal++, entry.draw.depthFromCamera);
            preparation.shadow.Record(shadow.Process(shadowInput));
        }

        preparation.depth.FinalizeGroups();
        preparation.opaque.FinalizeGroups();
        preparation.shadow.FinalizeGroups();
        preparation.transparent.FinalizeGroups();
        return preparation;
    }

    void SetVisibility(RenderVisibilityResult& visibility,
                       RenderPassKind pass,
                       std::span<const uint8> visible)
    {
        RenderVisibilityPassResult& passVisibility =
            visibility.passes[static_cast<size_t>(pass)];
        passVisibility.pass = pass;
        passVisibility.sourcePacketKnown.assign(visible.size(), 1);
        passVisibility.cpuVisibleBySourcePacket.assign(
            visible.begin(), visible.end());
    }

    RenderVisibilityResult MakeVisibility()
    {
        RenderVisibilityResult visibility;
        visibility.structurallyValid = true;
        visibility.evaluatedCullingMask = ~0U;
        visibility.coarseVisibleObjectMask = {1, 1, 1, 1};
        visibility.cpuVisibleObjectMask = {1, 1, 1, 1};
        // The masked packet is deliberately outside the camera frustum. Shadow
        // planning remains conservative and never consults this camera mask.
        constexpr std::array<uint8, 2> opaqueVisibility = {1, 0};
        constexpr std::array<uint8, 2> transparentVisibility = {1, 1};
        SetVisibility(visibility, RenderPassKind::Depth, opaqueVisibility);
        SetVisibility(visibility, RenderPassKind::Opaque, opaqueVisibility);
        SetVisibility(
            visibility, RenderPassKind::Transparent, transparentVisibility);
        return visibility;
    }

    const MeshPassPacketStream& GetStream(
        const SceneMeshPassPreparation& preparation,
        RenderPassKind pass)
    {
        switch (pass)
        {
            case RenderPassKind::Depth: return preparation.depth;
            case RenderPassKind::Opaque: return preparation.opaque;
            case RenderPassKind::Shadow: return preparation.shadow;
            case RenderPassKind::Transparent: return preparation.transparent;
            default: break;
        }
        ADD_FAILURE() << "Unexpected render pass";
        return preparation.depth;
    }

    RenderPassPolicyFacts MakePolicyFacts(
        RenderPassKind pass,
        const MeshPassPacketStream& stream)
    {
        RenderPassPolicyFacts facts;
        facts.pass = pass;
        facts.requested = true;
        facts.supported = true;
        facts.directAllowed = true;
        facts.gpuDrivenAllowed = pass == RenderPassKind::Depth ||
                                 pass == RenderPassKind::Opaque;
        facts.fixedCountIndirectAllowed = true;
        facts.directShaderReadiness = RenderPolicyReadiness::Ready;
        facts.directPipelineReadiness = RenderPolicyReadiness::Ready;
        facts.directResourceReadiness = RenderPolicyReadiness::Ready;
        facts.gpuDrivenShaderReadiness = RenderPolicyReadiness::Ready;
        facts.gpuDrivenPipelineReadiness = RenderPolicyReadiness::Ready;
        facts.gpuDrivenResourceReadiness = RenderPolicyReadiness::Ready;
        facts.inputPacketCount = stream.stats.inputPacketCount;
        facts.relevantPacketCount = stream.stats.relevantPacketCount;
        facts.candidatePacketCount = stream.stats.gpuCandidatePacketCount;
        facts.directPacketCount = stream.stats.directPacketCount;
        facts.skippedPacketCount = stream.stats.skippedPacketCount;
        facts.drawGroupCount = static_cast<uint32>(stream.groups.size());
        facts.workloadBeneficial = true;
        return facts;
    }

    RenderPolicyResolution ResolvePolicy(
        const SceneMeshPassPreparation& preparation,
        RenderGPUDrivenMode mode)
    {
        RenderPolicyResolverInput input;
        input.request.frameSequence = kFrameSequence;
        input.request.gpuDrivenMode = mode;
        input.viewOrdinal = kViewOrdinal;
        input.view.rendererAllowsGPUDriven = true;
        input.view.viewAllowsGPUDriven = true;
        input.view.implementationAvailable = true;
        input.view.requestedVisibility = RenderVisibilityMode::GpuFrustum;
        input.view.visibilityShaderReadiness = RenderPolicyReadiness::Ready;
        input.view.visibilityPipelineReadiness = RenderPolicyReadiness::Ready;
        input.view.sharedResourceReadiness = RenderPolicyReadiness::Ready;
        input.view.requiredBindingReadiness = RenderPolicyReadiness::Ready;
        input.capabilities.backend = RHIBackendType::DX12;
        input.capabilities.supportsComputeVisibility = true;
        input.capabilities.supportsDescriptorResourceBindings = true;
        input.capabilities.indexedIndirectExecution.supportsFixedCount = true;
        input.capabilities.indexedIndirectExecution.supportsCountBuffer = true;
        input.qualification.backend = RHIBackendType::DX12;
        input.qualification.revision = 1;
        input.qualification.passedGateMask =
            input.qualification.requiredGateMask;
        input.passes = {
            MakePolicyFacts(RenderPassKind::Depth,
                            GetStream(preparation, RenderPassKind::Depth)),
            MakePolicyFacts(RenderPassKind::Opaque,
                            GetStream(preparation, RenderPassKind::Opaque)),
            MakePolicyFacts(RenderPassKind::Shadow,
                            GetStream(preparation, RenderPassKind::Shadow)),
            MakePolicyFacts(
                RenderPassKind::Transparent,
                GetStream(preparation, RenderPassKind::Transparent)),
        };

        EXPECT_TRUE(ValidateRenderPolicyResolverInput(input));
        const RenderPolicyResolution resolution = ResolveRenderPolicy(input);
        EXPECT_TRUE(ValidateRenderPolicyResolution(resolution));
        return resolution;
    }

    RenderViewDrawPlan MakeDrawPlan(
        const RenderSceneDrawClassification& classification,
        const SceneMeshPassPreparation& preparation,
        const RenderVisibilityResult& visibility)
    {
        const RenderViewDrawPlanBuildResult result = BuildRenderViewDrawPlan(
            kFrameSequence, kViewOrdinal, classification, preparation, visibility);
        EXPECT_TRUE(result.IsSuccess());
        return result.plan;
    }

    const RenderPassExecutionPlan& FindPass(
        const RenderFrameExecutionPlan& plan,
        RenderPassKind pass)
    {
        for (const RenderPassExecutionPlan& candidate : plan.passes)
        {
            if (candidate.pass == pass)
            {
                return candidate;
            }
        }
        ADD_FAILURE() << "Missing compiled pass";
        return plan.passes.front();
    }
} // namespace

TEST(RenderDrawPlanValidation,
     ClassificationIsViewIndependentAndProjectionOwnsLayerAndCameraSelection)
{
    const RenderScene scene = MakeScene();
    RenderSceneDrawClassification first;
    RenderSceneDrawClassification repeated;
    ASSERT_TRUE(BuildRenderSceneDrawClassification(scene, first));
    ASSERT_TRUE(BuildRenderSceneDrawClassification(scene, repeated));

    EXPECT_TRUE(first.IsValid());
    EXPECT_EQ(first.stats, repeated.stats);
    ASSERT_EQ(4U, first.entries.size());
    EXPECT_EQ(4U, first.stats.sceneObjectCount);
    EXPECT_EQ(4U, first.stats.classifiedPacketCount);
    EXPECT_EQ(4U, first.stats.classifiedPrimitiveCount);
    EXPECT_EQ(4U, first.stats.materialBindingCount);
    EXPECT_EQ(4U, first.stats.uniqueMaterialCount);

    const std::array<uint8, 4> allVisible = {1, 1, 1, 1};
    std::vector<RenderDrawItem> opaque;
    std::vector<RenderDrawItem> masked;
    std::vector<RenderDrawItem> transparent;
    TransparentDrawListDiagnostics diagnostics;
    ASSERT_TRUE(ProjectRenderSceneDrawClassification(first,
                                                     allVisible,
                                                     0x1U,
                                                     Vec3{0.0f},
                                                     opaque,
                                                     masked,
                                                     transparent,
                                                     &diagnostics));
    EXPECT_EQ(1U, opaque.size());
    EXPECT_TRUE(masked.empty());
    ASSERT_EQ(2U, transparent.size());
    EXPECT_EQ(103U, transparent[0].packet.objectId);
    EXPECT_EQ(104U, transparent[1].packet.objectId);
    EXPECT_TRUE(diagnostics.orderValid);
    EXPECT_TRUE(IsTransparentDrawListStrictlyOrdered(transparent));

    const std::array<uint8, 4> frustumFiltered = {1, 1, 0, 1};
    ASSERT_TRUE(ProjectRenderSceneDrawClassification(first,
                                                     frustumFiltered,
                                                     0x1U,
                                                     Vec3{0.0f},
                                                     opaque,
                                                     masked,
                                                     transparent));
    EXPECT_EQ(1U, opaque.size());
    EXPECT_TRUE(masked.empty());
    ASSERT_EQ(1U, transparent.size());
    EXPECT_EQ(104U, transparent.front().packet.objectId);

    // Projection cannot mutate the frozen scene classification.
    EXPECT_EQ(first.stats, repeated.stats);
    for (size_t index = 0; index < first.entries.size(); ++index)
    {
        EXPECT_EQ(first.entries[index].classificationIndex,
                  repeated.entries[index].classificationIndex);
        EXPECT_EQ(first.entries[index].draw.packet,
                  repeated.entries[index].draw.packet);
        EXPECT_EQ(first.entries[index].layerMask,
                  repeated.entries[index].layerMask);
    }

    RenderSceneDrawClassification drifted = first;
    drifted.entries.front().worldPosition.x += 1.0F;
    EXPECT_FALSE(drifted.IsValid());

    RenderScene hiddenScene;
    RenderObject hidden = MakeObject(201,
                                     0,
                                     0x1U,
                                     Vec3{0.0F, 0.0F, 1.0F},
                                     RenderMaterialMode::Opaque,
                                     31,
                                     41);
    hidden.visible = false;
    hiddenScene.AddObject(std::move(hidden));
    RenderSceneDrawClassification hiddenClassification;
    ASSERT_TRUE(BuildRenderSceneDrawClassification(
        hiddenScene, hiddenClassification));
    const std::array<uint8, 1> malformedVisibleMask = {1};
    ASSERT_TRUE(ProjectRenderSceneDrawClassification(hiddenClassification,
                                                     malformedVisibleMask,
                                                     ~0U,
                                                     Vec3{0.0F},
                                                     opaque,
                                                     masked,
                                                     transparent));
    EXPECT_TRUE(opaque.empty());
    EXPECT_TRUE(masked.empty());
    EXPECT_TRUE(transparent.empty());
}

TEST(RenderDrawPlanValidation,
     PerPassPlanCountsAndHashesAreDeterministicAndStrategyNeutral)
{
    const RenderScene scene = MakeScene();
    RenderSceneDrawClassification classification;
    ASSERT_TRUE(BuildRenderSceneDrawClassification(scene, classification));
    const SceneMeshPassPreparation preparation = MakePreparation(classification);
    const RenderVisibilityResult visibility = MakeVisibility();

    const RenderViewDrawPlan first =
        MakeDrawPlan(classification, preparation, visibility);
    const RenderViewDrawPlan repeated =
        MakeDrawPlan(classification, preparation, visibility);
    ASSERT_TRUE(first.IsValid());
    EXPECT_EQ(first.semanticHash, repeated.semanticHash);
    EXPECT_EQ(first.sceneClassificationHash, classification.stats.semanticHash);

    const RenderPassDrawPlan* depth = first.FindPass(RenderPassKind::Depth);
    const RenderPassDrawPlan* opaque = first.FindPass(RenderPassKind::Opaque);
    const RenderPassDrawPlan* shadow = first.FindPass(RenderPassKind::Shadow);
    const RenderPassDrawPlan* transparent =
        first.FindPass(RenderPassKind::Transparent);
    ASSERT_NE(nullptr, depth);
    ASSERT_NE(nullptr, opaque);
    ASSERT_NE(nullptr, shadow);
    ASSERT_NE(nullptr, transparent);

    const auto expectStats = [](const RenderPassDrawPlanStats& stats,
                                uint32 inputs,
                                uint32 visible,
                                uint32 groups)
    {
        EXPECT_EQ(inputs, stats.inputPacketCount);
        EXPECT_EQ(inputs, stats.semanticPacketCount);
        EXPECT_EQ(inputs, stats.viewCandidatePacketCount);
        EXPECT_EQ(visible, stats.visiblePacketCount);
        EXPECT_EQ(visible, stats.materialBindingCount);
        EXPECT_EQ(visible, stats.visiblePrimitiveCount);
        EXPECT_EQ(groups, stats.semanticDrawGroupCount);
        EXPECT_NE(0U, stats.candidateIdentityHash);
        EXPECT_NE(0U, stats.visibleIdentityHash);
        EXPECT_NE(0U, stats.materialIdentityHash);
        EXPECT_NE(0U, stats.semanticGroupHash);
    };
    expectStats(depth->stats, 2, 1, 1);
    expectStats(opaque->stats, 2, 1, 1);
    expectStats(shadow->stats, 4, 4, 4);
    expectStats(transparent->stats, 2, 2, 2);
    EXPECT_EQ(2U, depth->stats.candidatePrimitiveCount);
    EXPECT_EQ(2U, opaque->stats.candidatePrimitiveCount);
    EXPECT_EQ(4U, shadow->stats.candidatePrimitiveCount);
    EXPECT_EQ(2U, transparent->stats.candidatePrimitiveCount);

    const RenderPolicyResolution directResolution =
        ResolvePolicy(preparation, RenderGPUDrivenMode::ForceDisabled);
    const RenderPolicyResolution gpuResolution =
        ResolvePolicy(preparation, RenderGPUDrivenMode::ForceEnabled);
    const RenderFramePlanCompileResult direct = CompileRenderFrameExecutionPlan(
        directResolution, preparation, first);
    const RenderFramePlanCompileResult gpu = CompileRenderFrameExecutionPlan(
        gpuResolution, preparation, first);
    ASSERT_TRUE(direct.succeeded);
    ASSERT_TRUE(gpu.succeeded);
    EXPECT_TRUE(ValidateRenderFrameExecutionPlanDrawParity(direct.plan, first));
    EXPECT_TRUE(ValidateRenderFrameExecutionPlanDrawParity(gpu.plan, first));

    const RenderPassExecutionPlan& directOpaque =
        FindPass(direct.plan, RenderPassKind::Opaque);
    const RenderPassExecutionPlan& gpuOpaque =
        FindPass(gpu.plan, RenderPassKind::Opaque);
    EXPECT_EQ(0U, directOpaque.gpuEligiblePackets.count);
    EXPECT_EQ(2U, directOpaque.directPackets.count);
    EXPECT_EQ(2U, gpuOpaque.gpuEligiblePackets.count);
    EXPECT_EQ(0U, gpuOpaque.directPackets.count);
    EXPECT_EQ(first.semanticHash,
              MakeDrawPlan(classification, preparation, visibility).semanticHash);

    RenderViewDrawPlan tamperedStats = first;
    ++tamperedStats.passes[0].stats.visiblePacketCount;
    EXPECT_FALSE(tamperedStats.IsValid());
    RenderViewDrawPlan tamperedPacket = first;
    ++tamperedPacket.passes[0].entries.front().packet.objectId;
    EXPECT_FALSE(tamperedPacket.IsValid());
    RenderViewDrawPlan tamperedInstanceArguments = first;
    ++tamperedInstanceArguments.passes[0]
          .entries.front().packet.arguments.instanceCount;
    EXPECT_FALSE(tamperedInstanceArguments.IsValid());

    SceneMeshPassPreparation directPreparation = preparation;
    for (MeshPassPacketStream* stream : {
             &directPreparation.depth,
             &directPreparation.opaque})
    {
        for (MeshPassProcessorResult& packet : stream->packets)
        {
            packet.disposition = MeshPassDisposition::Direct;
            packet.reason = MeshPassEligibilityReason::ResourceUnavailable;
            packet.groupKey.layout.primitiveDataBinding =
                PrimitiveDataBinding::PerDrawConstants;
            packet.groupKey.usesMaterialParameterTable = false;
        }
        stream->stats = {};
        for (const MeshPassProcessorResult& packet : stream->packets)
        {
            stream->stats.Record(packet);
        }
        stream->FinalizeGroups();
    }
    const RenderViewDrawPlan directOnly =
        MakeDrawPlan(classification, directPreparation, visibility);
    EXPECT_EQ(first.semanticHash, directOnly.semanticHash);
}

TEST(RenderDrawPlanValidation,
     ForwardAndDeferredSurfacePathsAreExclusiveAndPreserveVisibleDrawCounts)
{
    const RenderScene scene = MakeScene();
    const SurfacePathBindingResolver bindingResolver;
    RenderSceneDrawClassification classification;
    ASSERT_TRUE(BuildRenderSceneDrawClassification(
        scene,
        RenderSceneDrawClassificationSources{&bindingResolver},
        classification));
    ASSERT_TRUE(classification.IsValid());

    const SceneMeshPassPreparation preparation = MakePreparation(classification);
    RenderVisibilityResult visibility = MakeVisibility();
    constexpr std::array<uint8, 2> allOpaqueVisible = {1, 1};
    SetVisibility(visibility, RenderPassKind::Depth, allOpaqueVisible);
    SetVisibility(visibility, RenderPassKind::Opaque, allOpaqueVisible);
    const RenderViewDrawPlanBuildResult buildResult = BuildRenderViewDrawPlan(
        kFrameSequence, kViewOrdinal, classification, preparation, visibility);
    ASSERT_TRUE(buildResult.IsSuccess());
    const RenderViewDrawPlan& drawPlan = buildResult.plan;

    const RenderPassDrawPlan* opaque = drawPlan.FindPass(RenderPassKind::Opaque);
    const RenderPassDrawPlan* transparent =
        drawPlan.FindPass(RenderPassKind::Transparent);
    ASSERT_NE(nullptr, opaque);
    ASSERT_NE(nullptr, transparent);

    struct RouteCounts
    {
        uint32 drawCount = 0;
        std::set<PrimitiveDataIndex> primitives;
        std::set<RenderResourceHandle> materials;
    };
    RouteCounts deferred;
    RouteCounts forward;
    std::set<PrimitiveDataIndex> sourcePrimitives;
    std::set<RenderResourceHandle> sourceMaterials;
    uint32 sourceDrawCount = 0;

    const auto classify = [&](const RenderPassDrawPlan& pass)
    {
        for (const RenderPassDrawPlanEntry& entry : pass.entries)
        {
            if (!entry.passRelevant || !entry.cpuVisible)
            {
                continue;
            }

            ++sourceDrawCount;
            sourcePrimitives.insert(entry.packet.primitiveData);
            sourceMaterials.insert(entry.packet.materialKey.material);
            RouteCounts* destination = nullptr;
            switch (entry.packet.pipelineKey.surfacePathEligibility)
            {
                case SurfacePathEligibility::DeferredOpaque:
                    destination = &deferred;
                    break;
                case SurfacePathEligibility::ForwardSpecialOpaque:
                case SurfacePathEligibility::ForwardTranslucency:
                    destination = &forward;
                    break;
                case SurfacePathEligibility::Unsupported:
                    ADD_FAILURE() << "A visible draw has no legal surface path";
                    continue;
            }
            ASSERT_NE(nullptr, destination);
            ++destination->drawCount;
            destination->primitives.insert(entry.packet.primitiveData);
            destination->materials.insert(entry.packet.materialKey.material);
        }
    };
    classify(*opaque);
    classify(*transparent);

    EXPECT_EQ(2U, deferred.drawCount);
    EXPECT_EQ(2U, forward.drawCount);
    EXPECT_EQ(sourceDrawCount, deferred.drawCount + forward.drawCount);
    EXPECT_EQ(sourcePrimitives.size(),
              deferred.primitives.size() + forward.primitives.size());
    EXPECT_EQ(sourceMaterials.size(),
              deferred.materials.size() + forward.materials.size());

    std::set<PrimitiveDataIndex> overlappingPrimitives = deferred.primitives;
    for (PrimitiveDataIndex primitive : forward.primitives)
    {
        EXPECT_EQ(0U, overlappingPrimitives.count(primitive));
    }
    std::set<RenderResourceHandle> overlappingMaterials = deferred.materials;
    for (RenderResourceHandle material : forward.materials)
    {
        EXPECT_EQ(0U, overlappingMaterials.count(material));
    }
}

TEST(RenderDrawPlanValidation,
     ClassificationFreezesResourceAndMaterialBindingsBeforePlanning)
{
    const RenderScene scene = MakeScene();
    RenderSceneDrawClassification neutral;
    RenderSceneDrawClassification resolved;
    ASSERT_TRUE(BuildRenderSceneDrawClassification(scene, neutral));
    const TestClassificationBindingResolver bindingResolver;
    ASSERT_TRUE(BuildRenderSceneDrawClassification(
        scene,
        RenderSceneDrawClassificationSources{&bindingResolver},
        resolved));
    ASSERT_TRUE(resolved.IsValid());
    ASSERT_EQ(neutral.entries.size(), resolved.entries.size());
    EXPECT_NE(neutral.stats.semanticHash, resolved.stats.semanticHash);
    for (size_t index = 0; index < resolved.entries.size(); ++index)
    {
        EXPECT_EQ(neutral.entries[index].draw.packet.arguments.indexCount + 17,
                  resolved.entries[index].draw.packet.arguments.indexCount);
        EXPECT_EQ(0x9a71U,
                  resolved.entries[index]
                      .draw.packet.materialInstanceKey.textureBindingHash);
        EXPECT_TRUE(resolved.entries[index]
                        .draw.packet.materialInstanceKey
                        .parameterTableCompatible);
    }

    const SceneMeshPassPreparation resolvedPreparation =
        MakePreparation(resolved);
    const RenderVisibilityResult visibility = MakeVisibility();
    EXPECT_TRUE(BuildRenderViewDrawPlan(kFrameSequence,
                                       kViewOrdinal,
                                       resolved,
                                       resolvedPreparation,
                                       visibility)
                    .IsSuccess());

    const SceneMeshPassPreparation stalePreparation = MakePreparation(neutral);
    EXPECT_EQ(RenderDrawPlanBuildCode::ClassificationMismatch,
              BuildRenderViewDrawPlan(kFrameSequence,
                                      kViewOrdinal,
                                      resolved,
                                      stalePreparation,
                                      visibility)
                  .code);
}

TEST(RenderDrawPlanValidation,
     MissingVisibilityClassificationMismatchAndExecutionReferenceDriftFailClosed)
{
    const RenderScene scene = MakeScene();
    RenderSceneDrawClassification classification;
    ASSERT_TRUE(BuildRenderSceneDrawClassification(scene, classification));
    const SceneMeshPassPreparation preparation = MakePreparation(classification);
    const RenderVisibilityResult visibility = MakeVisibility();
    const RenderViewDrawPlan drawPlan =
        MakeDrawPlan(classification, preparation, visibility);

    RenderVisibilityResult missingVisibility = visibility;
    missingVisibility.passes[static_cast<size_t>(RenderPassKind::Opaque)]
        .sourcePacketKnown[0] = 0;
    EXPECT_EQ(RenderDrawPlanBuildCode::MissingVisibility,
              BuildRenderViewDrawPlan(kFrameSequence,
                                      kViewOrdinal,
                                      classification,
                                      preparation,
                                      missingVisibility)
                  .code);

    SceneMeshPassPreparation mismatchedClassification = preparation;
    ASSERT_FALSE(mismatchedClassification.transparent.packets.empty());
    ++mismatchedClassification.transparent.packets.front().packet.objectId;
    EXPECT_EQ(RenderDrawPlanBuildCode::ClassificationMismatch,
              BuildRenderViewDrawPlan(kFrameSequence,
                                      kViewOrdinal,
                                      classification,
                                      mismatchedClassification,
                                      visibility)
                  .code);

    SceneMeshPassPreparation unclassifiedShadow = preparation;
    ASSERT_FALSE(unclassifiedShadow.shadow.packets.empty());
    unclassifiedShadow.shadow.packets.front().classificationIndex =
        RVX_INVALID_INDEX;
    EXPECT_EQ(RenderDrawPlanBuildCode::ClassificationMismatch,
              BuildRenderViewDrawPlan(kFrameSequence,
                                      kViewOrdinal,
                                      classification,
                                      unclassifiedShadow,
                                      visibility)
                  .code);

    SceneMeshPassPreparation missingCoverage;
    EXPECT_EQ(RenderDrawPlanBuildCode::ClassificationCoverageMismatch,
              BuildRenderViewDrawPlan(kFrameSequence,
                                      kViewOrdinal,
                                      classification,
                                      missingCoverage,
                                      visibility)
                  .code);

    SceneMeshPassPreparation duplicateCoverage = preparation;
    MeshPassPacketStream duplicatedDepth;
    for (const MeshPassProcessorResult& packet :
         duplicateCoverage.depth.packets)
    {
        duplicatedDepth.Record(packet);
    }
    duplicatedDepth.Record(duplicateCoverage.depth.packets.front());
    duplicatedDepth.FinalizeGroups();
    duplicateCoverage.depth = std::move(duplicatedDepth);
    EXPECT_EQ(RenderDrawPlanBuildCode::ClassificationCoverageMismatch,
              BuildRenderViewDrawPlan(kFrameSequence,
                                      kViewOrdinal,
                                      classification,
                                      duplicateCoverage,
                                      visibility)
                  .code);

    const RenderFramePlanCompileResult direct = CompileRenderFrameExecutionPlan(
        ResolvePolicy(preparation, RenderGPUDrivenMode::ForceDisabled),
        preparation,
        drawPlan);
    ASSERT_TRUE(direct.succeeded);
    ASSERT_TRUE(ValidateRenderFrameExecutionPlanDrawParity(direct.plan, drawPlan));

    RenderFrameExecutionPlan duplicateReference = direct.plan;
    const RenderPassExecutionPlan& opaque =
        FindPass(duplicateReference, RenderPassKind::Opaque);
    ASSERT_GE(opaque.directPackets.count, 2U);
    const uint32 firstReferenceIndex = opaque.directPackets.first;
    duplicateReference.packetReferences[firstReferenceIndex + 1U] =
        duplicateReference.packetReferences[firstReferenceIndex];
    EXPECT_FALSE(ValidateRenderFrameExecutionPlanDrawParity(duplicateReference,
                                                             drawPlan));

    RenderFrameExecutionPlan driftedReference = direct.plan;
    const RenderPassExecutionPlan& transparent =
        FindPass(driftedReference, RenderPassKind::Transparent);
    ASSERT_GT(transparent.directPackets.count, 0U);
    ++driftedReference
          .packetReferences[transparent.directPackets.first]
          .sourceSignature.classificationIndex;
    EXPECT_FALSE(ValidateRenderFrameExecutionPlanDrawParity(driftedReference,
                                                             drawPlan));

    RenderFrameExecutionPlan orphanReference = direct.plan;
    orphanReference.packetReferences.push_back(
        orphanReference.packetReferences.front());
    EXPECT_FALSE(ValidateRenderFrameExecutionPlanDrawParity(orphanReference,
                                                             drawPlan));
}

TEST(RenderDrawPlanValidation, EmptySceneBuildsAValidZeroWorkPlan)
{
    const RenderScene emptyScene;
    RenderSceneDrawClassification classification;
    ASSERT_TRUE(BuildRenderSceneDrawClassification(emptyScene, classification));
    ASSERT_TRUE(classification.IsValid());
    EXPECT_TRUE(classification.entries.empty());

    SceneMeshPassPreparation preparation;
    RenderVisibilityResult visibility;
    visibility.structurallyValid = true;
    const RenderViewDrawPlanBuildResult result = BuildRenderViewDrawPlan(
        kFrameSequence,
        kViewOrdinal,
        classification,
        preparation,
        visibility);
    ASSERT_TRUE(result.IsSuccess());
    for (const RenderPassDrawPlan& pass : result.plan.passes)
    {
        EXPECT_TRUE(pass.entries.empty());
        EXPECT_EQ(0U, pass.stats.inputPacketCount);
        EXPECT_EQ(0U, pass.stats.visiblePacketCount);
    }
}
