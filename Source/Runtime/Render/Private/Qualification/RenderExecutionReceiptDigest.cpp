#include "Render/Qualification/RenderExecutionReceiptDigest.h"

#include <cmath>
#include <iomanip>
#include <limits>
#include <locale>
#include <ostream>
#include <sstream>
#include <string_view>
#include <type_traits>

namespace RVX
{
namespace
{
    /** @brief Delimited, length-prefixed canonical scalar writer. */
    class CanonicalReceiptWriter final
    {
    public:
        explicit CanonicalReceiptWriter(std::ostream& stream) noexcept
            : m_stream(stream)
        {
            m_stream.imbue(std::locale::classic());
            m_stream << std::setprecision(std::numeric_limits<float64>::max_digits10);
        }

        void Begin(std::string_view name)
        {
            m_stream << name.size() << ':' << name << '{';
        }

        void End() { m_stream << '}'; }

        void Bool(std::string_view name, bool value)
        {
            FieldPrefix(name);
            m_stream << (value ? '1' : '0') << ';';
        }

        template <typename TValue>
        void Integral(std::string_view name, TValue value)
        {
            FieldPrefix(name);
            if constexpr (std::is_signed_v<TValue>)
            {
                m_stream << static_cast<int64>(value);
            }
            else
            {
                m_stream << static_cast<uint64>(value);
            }
            m_stream << ';';
        }

        template <typename TEnum>
        void Enum(std::string_view name, TEnum value)
        {
            using Underlying = std::underlying_type_t<TEnum>;
            Integral(name, static_cast<Underlying>(value));
        }

        void Float(std::string_view name, float32 value)
        {
            FieldPrefix(name);
            if (std::isnan(value))
            {
                m_stream << "nan";
            }
            else if (std::isinf(value))
            {
                m_stream << (value < 0.0f ? "-inf" : "+inf");
            }
            else
            {
                m_stream << std::setprecision(
                    std::numeric_limits<float32>::max_digits10)
                         << value << std::setprecision(
                                std::numeric_limits<float64>::max_digits10);
            }
            m_stream << ';';
        }

        void Text(std::string_view name, std::string_view value)
        {
            FieldPrefix(name);
            m_stream << value.size() << ':' << value << ';';
        }

    private:
        void FieldPrefix(std::string_view name)
        {
            m_stream << name.size() << ':' << name << '=';
        }

        std::ostream& m_stream;
    };

    void WriteFrameIdentity(
        CanonicalReceiptWriter& writer,
        std::string_view name,
        const RenderFrameIdentity& value)
    {
        writer.Begin(name);
        writer.Integral("frameSequence", value.frameSequence);
        writer.Integral("sceneRevision", value.sceneRevision);
        writer.Integral("surfaceGeneration", value.surfaceGeneration);
        writer.Integral("configurationRevision", value.configurationRevision);
        writer.End();
    }

    void WriteViewIdentity(
        CanonicalReceiptWriter& writer,
        std::string_view name,
        const RenderViewIdentity& value)
    {
        writer.Begin(name);
        writer.Integral("sceneRuntimeId", value.sceneRuntimeId);
        writer.Integral("cameraEntityPacked", value.cameraEntityPacked);
        writer.Integral("subViewIndex", value.subViewIndex);
        writer.Enum("role", value.role);
        writer.End();
    }

    void WriteExtent(
        CanonicalReceiptWriter& writer,
        std::string_view name,
        const RenderOutputExtent& value)
    {
        writer.Begin(name);
        writer.Integral("width", value.width);
        writer.Integral("height", value.height);
        writer.End();
    }

    void WriteFeature(
        CanonicalReceiptWriter& writer,
        const RenderFeatureExecutionReceipt& value)
    {
        writer.Begin("feature");
        writer.Integral("featureId", value.featureId.value);
        writer.Integral("featureSchemaVersion", value.featureSchemaVersion);
        writer.Enum("phase", value.phase);
        writer.Enum("requirement", value.requirement);
        writer.Begin("readiness");
        writer.Bool("compiled", value.readiness.compiled);
        writer.Bool("hardwareSupported", value.readiness.hardwareSupported);
        writer.Bool("implementationSupported", value.readiness.implementationSupported);
        writer.Bool("pipelineReady", value.readiness.pipelineReady);
        writer.Bool("inputsReady", value.readiness.inputsReady);
        writer.Bool("executed", value.readiness.executed);
        writer.Text("diagnostic", value.readiness.diagnostic);
        writer.End();
        writer.Bool("selected", value.selected);
        writer.Bool("fallbackUsed", value.fallbackUsed);
        writer.Integral("selectedFallbackFeatureId", value.selectedFallbackFeatureId.value);
        writer.Integral("fallbackForFeatureId", value.fallbackForFeatureId.value);
        writer.Integral("graphPassCount", value.graphPassCount);
        writer.Integral("executedGraphPassCount", value.executedGraphPassCount);
        writer.Integral("graphPassNameCount", value.graphPassNames.size());
        for (const std::string& passName : value.graphPassNames)
            writer.Text("graphPassName", passName);
        writer.Text("diagnostic", value.diagnostic);
        writer.Bool("contractCompletedWithoutGraphWork", value.contractCompletedWithoutGraphWork);
        writer.End();
    }

    void WriteDeferred(
        CanonicalReceiptWriter& writer,
        const RenderDeferredOpaqueExecutionReceipt& value)
    {
        writer.Begin("deferredOpaque");
        writer.Bool("available", value.available);
        writer.Enum("status", value.status);
        writer.Integral("plannedPacketCount", value.plannedPacketCount);
        writer.Integral("executedPacketCount", value.executedPacketCount);
        writer.End();
    }

    void WriteLighting(
        CanonicalReceiptWriter& writer,
        const RenderLightingExecutionReceipt& value)
    {
        writer.Begin("lighting");
        writer.Integral("lightingPlanIdentity", value.lightingPlanIdentity);
        writer.Integral("sceneLightStateHash", value.sceneLightStateHash);
        writer.Integral("primaryDirectionalLightId", value.primaryDirectionalLightId);
        writer.Integral("pointLightRequestedCount", value.pointLightRequestedCount);
        writer.Integral("pointLightAdmittedCount", value.pointLightAdmittedCount);
        writer.Integral("pointLightCapacity", value.pointLightCapacity);
        writer.Integral("pointLightOverflowCount", value.pointLightOverflowCount);
        writer.Integral("spotLightRequestedCount", value.spotLightRequestedCount);
        writer.Integral("spotLightAdmittedCount", value.spotLightAdmittedCount);
        writer.Integral("spotLightCapacity", value.spotLightCapacity);
        writer.Integral("spotLightOverflowCount", value.spotLightOverflowCount);
        writer.Bool("csmRequested", value.csmRequested);
        writer.Bool("csmInputsReady", value.csmInputsReady);
        writer.Bool("csmExecutionReady", value.csmExecutionReady);
        writer.Bool("iblRequested", value.iblRequested);
        writer.Bool("iblInputsReady", value.iblInputsReady);
        writer.Bool("iblExecutionReady", value.iblExecutionReady);
        writer.End();
    }

    void WriteDeformation(
        CanonicalReceiptWriter& writer,
        const RenderDeformationExecutionReceipt& value)
    {
        writer.Begin("deformation");
        writer.Integral("deformationContentHash", value.deformationContentHash);
        writer.Integral("skinnedObjectCount", value.skinnedObjectCount);
        writer.Integral("paletteMatrixCount", value.paletteMatrixCount);
        writer.Integral("previousPaletteObjectCount", value.previousPaletteObjectCount);
        writer.Bool("currentPaletteBufferReady", value.currentPaletteBufferReady);
        writer.Bool("previousPaletteBufferReady", value.previousPaletteBufferReady);
        writer.Integral("opaqueSkinnedDrawCount", value.opaqueSkinnedDrawCount);
        writer.Integral("shadowSkinnedDrawCount", value.shadowSkinnedDrawCount);
        writer.Integral("velocitySkinnedDrawCount", value.velocitySkinnedDrawCount);
        writer.Integral("velocityPreviousPaletteDrawCount", value.velocityPreviousPaletteDrawCount);
        writer.End();
    }

    void WritePostProcess(
        CanonicalReceiptWriter& writer,
        const RenderPostProcessExecutionReceipt& value)
    {
        writer.Begin("postProcess");
        writer.Integral("postProcessContentHash", value.postProcessContentHash);
        writer.Integral("exposureStateContentHash", value.exposureStateContentHash);
        writer.Integral("toneMapExposureStateHash", value.toneMapExposureStateHash);
        writer.Integral("bloomExposureStateHash", value.bloomExposureStateHash);
        writer.Integral("taaExposureStateHash", value.taaExposureStateHash);
        writer.Enum("exposureMode", value.exposureMode);
        writer.Bool("exposureRequested", value.exposureRequested);
        writer.Bool("exposurePipelineReady", value.exposurePipelineReady);
        writer.Bool("exposureInputsReady", value.exposureInputsReady);
        writer.Bool("exposureExecuted", value.exposureExecuted);
        writer.Integral("exposureHistogramPassCount", value.exposureHistogramPassCount);
        writer.Integral("exposureAdaptationPassCount", value.exposureAdaptationPassCount);
        writer.Bool("exposureHistoryAvailableBefore", value.exposureHistoryAvailableBefore);
        writer.Bool("exposureHistorySampled", value.exposureHistorySampled);
        writer.Bool("exposureHistoryCandidateProduced", value.exposureHistoryCandidateProduced);
        writer.Bool("exposureHistoryCommitted", value.exposureHistoryCommitted);
        writer.Bool("exposureHistoryAbandoned", value.exposureHistoryAbandoned);
        writer.Enum("exposureHistoryResetReasons", value.exposureHistoryResetReasons);
        writer.Bool("exposureProbeRequested", value.exposureProbeRequested);
        writer.Bool("exposureProbeReady", value.exposureProbeReady);
        writer.Float("targetEV100", value.targetEV100);
        writer.Float("adaptedEV100", value.adaptedEV100);
        writer.Float("exposureScale", value.exposureScale);
        writer.Float("preExposure", value.preExposure);
        writer.Float("previousPreExposure", value.previousPreExposure);
        writer.Integral("toneMapPassCount", value.toneMapPassCount);
        writer.Integral("bloomPassCount", value.bloomPassCount);
        writer.Integral("colorGradingPassCount", value.colorGradingPassCount);
        writer.Integral("fxaaPassCount", value.fxaaPassCount);
        writer.Integral("taaPassCount", value.taaPassCount);
        writer.Enum("antiAliasingMode", value.antiAliasingMode);
        writer.Bool("taaRequested", value.taaRequested);
        writer.Bool("taaPipelineReady", value.taaPipelineReady);
        writer.Bool("taaInputsReady", value.taaInputsReady);
        writer.Bool("taaExecuted", value.taaExecuted);
        writer.Bool("historyAvailableBefore", value.historyAvailableBefore);
        writer.Bool("historySampled", value.historySampled);
        writer.Bool("historyCandidateProduced", value.historyCandidateProduced);
        writer.Bool("historyCommitted", value.historyCommitted);
        writer.Bool("historyAbandoned", value.historyAbandoned);
        writer.Enum("historyResetReasons", value.historyResetReasons);
        writer.Float("currentJitterPixelsX", value.currentJitterPixels.x);
        writer.Float("currentJitterPixelsY", value.currentJitterPixels.y);
        writer.Float("previousJitterPixelsX", value.previousJitterPixels.x);
        writer.Float("previousJitterPixelsY", value.previousJitterPixels.y);
        writer.Integral("taaResolveGraphPassCount", value.taaResolveGraphPassCount);
        writer.Integral("taaHistoryStoreGraphPassCount", value.taaHistoryStoreGraphPassCount);
        writer.Enum("inputColorDomain", value.inputColorDomain);
        writer.Enum("plannedOutputColorDomain", value.plannedOutputColorDomain);
        writer.Enum("storedOutputColorDomain", value.storedOutputColorDomain);
        writer.Enum("finalOutputFormat", value.finalOutputFormat);
        writer.Enum("toneMappingTransfer", value.toneMappingTransfer);
        writer.Enum("presentationTransfer", value.presentationTransfer);
        writer.Bool("hdrInputReady", value.hdrInputReady);
        writer.Bool("displayLinearOutputReady", value.displayLinearOutputReady);
        writer.Bool("encodedOutputReady", value.encodedOutputReady);
        writer.Bool("fallbackUsed", value.fallbackUsed);
        writer.End();
    }

    void WriteAmbientOcclusion(
        CanonicalReceiptWriter& writer,
        const RenderAmbientOcclusionExecutionReceipt& value)
    {
        writer.Begin("ambientOcclusion");
        writer.Bool("requested", value.requested);
        writer.Bool("linearDepthInputReady", value.linearDepthInputReady);
        writer.Bool("worldNormalInputReady", value.worldNormalInputReady);
        writer.Bool("velocityInputReady", value.velocityInputReady);
        writer.Bool("velocityProducerExecuted", value.velocityProducerExecuted);
        writer.Bool("pipelineReady", value.pipelineReady);
        writer.Bool("inputsReady", value.inputsReady);
        writer.Bool("outputPublished", value.outputPublished);
        writer.Bool("executed", value.executed);
        writer.Bool("lightingConsumerExecuted", value.lightingConsumerExecuted);
        writer.Bool("historyAvailableBefore", value.historyAvailableBefore);
        writer.Bool("historySampled", value.historySampled);
        writer.Bool("motionReprojectionUsed", value.motionReprojectionUsed);
        writer.Bool("viewProjectionMotionObserved", value.viewProjectionMotionObserved);
        writer.Bool("historyCandidateProduced", value.historyCandidateProduced);
        writer.Bool("historyCommitted", value.historyCommitted);
        writer.Bool("historyAbandoned", value.historyAbandoned);
        writer.Bool("fallbackUsed", value.fallbackUsed);
        writer.Bool("probeRequested", value.probeRequested);
        writer.Bool("probeReady", value.probeReady);
        writer.Enum("historyResetReasons", value.historyResetReasons);
        writer.Integral("consumerMask", value.consumerMask);
        writer.Enum("outputFormat", value.outputFormat);
        writer.Integral("horizonSearchGraphPassCount", value.horizonSearchGraphPassCount);
        writer.Integral("bilateralDenoiseGraphPassCount", value.bilateralDenoiseGraphPassCount);
        writer.Integral("temporalResolveGraphPassCount", value.temporalResolveGraphPassCount);
        writer.Integral("probeSampleCount", value.probeSampleCount);
        writer.Integral("probeOccludedSampleCount", value.probeOccludedSampleCount);
        writer.Integral("planContentHash", value.planContentHash);
        writer.Integral("settingsHash", value.settingsHash);
        writer.Integral("aoFieldContentHash", value.aoFieldContentHash);
        writer.Integral("lightingAOFieldContentHash", value.lightingAOFieldContentHash);
        writer.Integral("velocityFieldIdentity", value.velocityFieldIdentity);
        writer.Float("probeMinVisibility", value.probeMinVisibility);
        writer.Float("probeMeanVisibility", value.probeMeanVisibility);
        writer.Float("probeMaxVisibility", value.probeMaxVisibility);
        writer.Text("diagnostic", value.diagnostic);
        writer.End();
    }

    void WriteToonLighting(
        CanonicalReceiptWriter& writer,
        const RenderToonLightingExecutionReceipt& value)
    {
        writer.Begin("toonLighting");
        writer.Bool("requested", value.requested);
        writer.Bool("pipelineReady", value.pipelineReady);
        writer.Bool("inputsReady", value.inputsReady);
        writer.Bool("executed", value.executed);
        writer.Integral("programFamilyHash", value.programFamilyHash);
        writer.Integral("materialCount", value.materialCount);
        writer.Integral("drawCount", value.drawCount);
        writer.Integral("forwardShadedDrawCount", value.forwardShadedDrawCount);
        writer.Integral("deferredShadedDrawCount", value.deferredShadedDrawCount);
        writer.Integral("standardLitFallbackCount", value.standardLitFallbackCount);
        writer.Integral("materialFallbackCount", value.materialFallbackCount);
        writer.Bool("bandsApplied", value.bandsApplied);
        writer.Bool("specularThresholdApplied", value.specularThresholdApplied);
        writer.Bool("rimApplied", value.rimApplied);
        writer.Bool("shadowTintApplied", value.shadowTintApplied);
        writer.Integral("materialTableSchemaVersion", value.materialTableSchemaVersion);
        writer.Integral("materialTableContentHash", value.materialTableContentHash);
        writer.End();
    }

    void WriteToonOutline(
        CanonicalReceiptWriter& writer,
        const RenderToonOutlineExecutionReceipt& value)
    {
        writer.Begin("toonOutline");
        writer.Bool("requested", value.requested);
        writer.Bool("pipelineReady", value.pipelineReady);
        writer.Bool("inputsReady", value.inputsReady);
        writer.Bool("executed", value.executed);
        writer.Bool("depthInputReady", value.depthInputReady);
        writer.Bool("worldNormalInputReady", value.worldNormalInputReady);
        writer.Bool("materialIdentityInputReady", value.materialIdentityInputReady);
        writer.Bool("materialTableInputReady", value.materialTableInputReady);
        writer.Bool("temporalOrderingValidated", value.temporalOrderingValidated);
        writer.Integral("taaSequenceIndex", value.taaSequenceIndex);
        writer.Integral("outlineSequenceIndex", value.outlineSequenceIndex);
        writer.Integral("toneMapSequenceIndex", value.toneMapSequenceIndex);
        writer.Integral("eligibleToonMaterialCount", value.eligibleToonMaterialCount);
        writer.Bool("qualificationRequested", value.qualificationRequested);
        writer.Bool("qualificationReady", value.qualificationReady);
        writer.Integral("outlinedPixelCount", value.outlinedPixelCount);
        writer.Integral("nonToonAffectedPixelCount", value.nonToonAffectedPixelCount);
        writer.Bool("resolutionAware", value.resolutionAware);
        writer.Integral("renderWidth", value.renderWidth);
        writer.Integral("renderHeight", value.renderHeight);
        writer.Integral("settingsHash", value.settingsHash);
        writer.Integral("materialTableContentHash", value.materialTableContentHash);
        writer.Bool("fallbackUsed", value.fallbackUsed);
        writer.Text("diagnostic", value.diagnostic);
        writer.End();
    }

    void WriteHybridRayTracing(
        CanonicalReceiptWriter& writer,
        const RenderHybridRayTracingExecutionReceipt& value)
    {
        writer.Begin("hybridRayTracing");
        writer.Bool("available", value.available);
        writer.Integral("planIdentityHash", value.planIdentityHash);
        writer.Integral("scenePreparationGeneration", value.scenePreparationGeneration);
        writer.Integral("sceneStructuralHash", value.sceneStructuralHash);
        WriteFrameIdentity(writer, "frameIdentity", value.frameIdentity);
        WriteViewIdentity(writer, "viewIdentity", value.viewIdentity);
        WriteExtent(writer, "fullResolution", value.fullResolution);
        WriteExtent(writer, "halfResolution", value.halfResolution);
        writer.Enum("historyResetReasons", value.historyResetReasons);
        writer.Enum("recipe", value.recipe);
        writer.Enum("quality", value.quality);
        writer.Integral("profileHash", value.profileHash);
        writer.Enum("shadowRoute", value.shadowRoute);
        writer.Enum("reflectionRoute", value.reflectionRoute);
        writer.Bool("graphExecutionSucceeded", value.graphExecutionSucceeded);
        writer.Bool("sceneBuildAccepted", value.sceneBuildAccepted);
        writer.Bool("shadowDispatchRecorded", value.shadowDispatchRecorded);
        writer.Bool("reflectionDispatchRecorded", value.reflectionDispatchRecorded);
        writer.Bool("spatialDenoiseRecorded", value.spatialDenoiseRecorded);
        writer.Bool("temporalResolveRecorded", value.temporalResolveRecorded);
        writer.Bool("compositeRecorded", value.compositeRecorded);
        writer.Bool("compositeUsedDenoisedSource", value.compositeUsedDenoisedSource);
        writer.Bool("submissionAccepted", value.submissionAccepted);
        writer.Bool("presentationQualified", value.presentationQualified);
        writer.Bool("succeeded", value.succeeded);
        writer.End();
    }
} // namespace

void WriteRenderExecutionReceiptCanonical(
    std::ostream& stream,
    const RenderExecutionReceipt& receipt)
{
    CanonicalReceiptWriter writer(stream);
    writer.Begin("RenderExecutionReceipt");
    writer.Integral("schemaVersion", receipt.schemaVersion);
    writer.Bool("available", receipt.available);
    writer.Bool("graphComposed", receipt.graphComposed);
    writer.Bool("executionSucceeded", receipt.executionSucceeded);
    writer.Bool("presented", receipt.presented);
    writer.Bool("succeeded", receipt.succeeded);
    writer.Begin("build");
    writer.Text("sourceCommit", receipt.build.sourceCommit);
    writer.Text("preset", receipt.build.preset);
    writer.End();
    WriteFrameIdentity(writer, "frameIdentity", receipt.frameIdentity);
    WriteViewIdentity(writer, "viewIdentity", receipt.viewIdentity);
    writer.Enum("backend", receipt.backend);
    writer.Enum("requestedRecipe", receipt.requestedRecipe);
    writer.Enum("resolvedRecipe", receipt.resolvedRecipe);
    writer.Enum("quality", receipt.quality);
    writer.Integral("profileHash", receipt.profileHash);
    WriteDeferred(writer, receipt.deferredOpaque);
    WriteLighting(writer, receipt.lighting);
    WriteDeformation(writer, receipt.deformation);
    WritePostProcess(writer, receipt.postProcess);
    WriteAmbientOcclusion(writer, receipt.ambientOcclusion);
    WriteToonLighting(writer, receipt.toonLighting);
    WriteToonOutline(writer, receipt.toonOutline);
    WriteHybridRayTracing(writer, receipt.hybridRayTracing);
    writer.Integral("featureCount", receipt.features.size());
    for (const RenderFeatureExecutionReceipt& feature : receipt.features)
        WriteFeature(writer, feature);
    writer.Text("diagnostic", receipt.diagnostic);
    writer.End();
}

std::string SerializeRenderExecutionReceiptCanonical(
    const RenderExecutionReceipt& receipt)
{
    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    stream << std::setprecision(std::numeric_limits<float64>::max_digits10);
    WriteRenderExecutionReceiptCanonical(stream, receipt);
    return stream.str();
}

Hash::SHA256Digest ComputeRenderExecutionReceiptDigest(
    const RenderExecutionReceipt& receipt) noexcept
{
    try
    {
        return Hash::ComputeSHA256(SerializeRenderExecutionReceiptCanonical(receipt));
    }
    catch (...)
    {
        return {};
    }
}

bool IsRenderExecutionReceiptDigestValid(
    const Hash::SHA256Digest& digest) noexcept
{
    for (const uint8 value : digest)
    {
        if (value != 0)
            return true;
    }
    return false;
}
} // namespace RVX
