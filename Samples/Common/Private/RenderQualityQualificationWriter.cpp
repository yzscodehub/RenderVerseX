#include "Samples/RenderQualityQualificationWriter.h"

#include "Samples/SampleCLI.h"

#include "Core/Diagnostics/JsonWriter.h"
#include "Core/Hash/SHA256.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <locale>
#include <sstream>
#include <system_error>
#include <utility>
#include <vector>

namespace RVX
{
namespace
{
    inline constexpr uint32 RVX_RENDER_QUALITY_MAX_ATTACHMENT_WIDTH = 8192U;
    inline constexpr uint32 RVX_RENDER_QUALITY_MAX_ATTACHMENT_HEIGHT = 8192U;
    inline constexpr uint64 RVX_RENDER_QUALITY_MAX_ATTACHMENT_BYTES =
        256ULL * 1024ULL * 1024ULL;
    inline constexpr uint64 RVX_RENDER_QUALITY_MAX_BUNDLE_BYTES =
        512ULL * 1024ULL * 1024ULL;

    struct AttachmentSourceDescription
    {
        const char* rawFileName = nullptr;
        const char* resourceName = nullptr;
        const char* graphPassName = nullptr;
    };

    [[nodiscard]] const AttachmentSourceDescription& GetSourceDescription(
        QualificationCaptureAttachmentSemantic semantic) noexcept
    {
        static constexpr std::array<AttachmentSourceDescription,
                                    RVX_QUALIFICATION_CAPTURE_ATTACHMENT_COUNT>
            Descriptions{{
                {"BeautyLDR.raw", "PostProcessOutputs.presentableColor",
                 "RenderQualityCapture.BeautyLDR"},
                {"PreToneHDR.raw", "TranslucencyOutputs.hdrSceneColor",
                 "RenderQualityCapture.PreToneHDR"},
                {"LinearDepth.raw", "TranslucencyOutputs.deviceDepth",
                 "RenderQualityCapture.LinearDepth"},
                {"Normal.raw", "TranslucencyOutputs.worldNormal",
                 "RenderQualityCapture.WorldNormal"},
                {"Velocity.raw", "TranslucencyOutputs.velocity",
                 "RenderQualityCapture.Velocity"},
                {"AmbientOcclusion.raw",
                 "AmbientOcclusionOutputs.ambientOcclusion",
                 "RenderQualityCapture.AmbientOcclusion"},
                {"ReactiveMask.raw", "TranslucencyOutputs.reactiveMask",
                 "RenderQualityCapture.ReactiveMask"},
            }};
        const size_t index = static_cast<size_t>(semantic);
        static constexpr AttachmentSourceDescription Invalid{};
        return index < Descriptions.size() ? Descriptions[index] : Invalid;
    }

    [[nodiscard]] const char* GetPixelFormatName(
        QualificationCapturePixelFormat format) noexcept
    {
        switch (format)
        {
            case QualificationCapturePixelFormat::R32Float:
                return "R32_FLOAT";
            case QualificationCapturePixelFormat::RG32Float:
                return "RG32_FLOAT";
            case QualificationCapturePixelFormat::RGBA32Float:
                return "RGBA32_FLOAT";
            case QualificationCapturePixelFormat::Unknown: break;
        }
        return "invalid";
    }

    [[nodiscard]] const char* GetColorDomainName(
        QualificationCaptureColorDomain domain) noexcept
    {
        switch (domain)
        {
            case QualificationCaptureColorDomain::DisplayReferredLDR:
                return "LDR";
            case QualificationCaptureColorDomain::SceneLinearHDR:
                return "HDR";
            case QualificationCaptureColorDomain::LinearDepth:
                return "LinearDepth";
            case QualificationCaptureColorDomain::WorldNormal:
                return "WorldNormal";
            case QualificationCaptureColorDomain::MotionVector:
                return "Motion";
            case QualificationCaptureColorDomain::AmbientOcclusion:
            case QualificationCaptureColorDomain::ReactiveMask:
                return "LinearMask";
            case QualificationCaptureColorDomain::Unknown: break;
        }
        return "invalid";
    }

    [[nodiscard]] const char* GetViewRoleProvenanceName(
        ViewRole role) noexcept
    {
        switch (role)
        {
            case ViewRole::Main: return "Main";
            case ViewRole::Shadow: return "Shadow";
            case ViewRole::Reflection: return "Reflection";
            case ViewRole::Editor: return "Editor";
            default: return "invalid";
        }
    }

    [[nodiscard]] constexpr uint32 GetChannelCount(
        QualificationCapturePixelFormat format) noexcept
    {
        switch (format)
        {
            case QualificationCapturePixelFormat::R32Float: return 1U;
            case QualificationCapturePixelFormat::RG32Float: return 2U;
            case QualificationCapturePixelFormat::RGBA32Float: return 4U;
            case QualificationCapturePixelFormat::Unknown: break;
        }
        return 0U;
    }

    [[nodiscard]] bool IsNonEmptyText(const std::string& value) noexcept
    {
        return !value.empty() && value.size() <= 1024U;
    }

    [[nodiscard]] bool IsDeclaredRenderPathName(
        const std::string& value) noexcept
    {
        return value == GetSampleRenderPathName(SampleRenderPath::Auto) ||
               value == GetSampleRenderPathName(SampleRenderPath::Direct) ||
               value == GetSampleRenderPathName(SampleRenderPath::GPUDriven);
    }

    [[nodiscard]] bool IsDeclaredInstancingModeName(
        const std::string& value) noexcept
    {
        return value ==
                   GetRenderInstancingModeName(RenderInstancingMode::Disabled) ||
               value == GetRenderInstancingModeName(RenderInstancingMode::Auto);
    }

    void SetError(std::string* outError, std::string value)
    {
        if (outError != nullptr)
        {
            *outError = std::move(value);
        }
    }

    [[nodiscard]] bool IsContained(
        const std::filesystem::path& root,
        const std::filesystem::path& candidate) noexcept
    {
        const std::filesystem::path relative = candidate.lexically_relative(root);
        if (relative.empty() || relative.is_absolute())
        {
            return false;
        }
        for (const std::filesystem::path& component : relative)
        {
            if (component == "..")
            {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] bool PrepareOutputRoot(
        const std::filesystem::path& requestedRoot,
        std::filesystem::path& outRoot,
        std::string* outError)
    {
        if (requestedRoot.empty())
        {
            SetError(outError, "RenderQuality output root is empty");
            return false;
        }

        std::error_code error;
        std::filesystem::path absolute =
            std::filesystem::absolute(requestedRoot, error);
        if (error)
        {
            SetError(outError,
                     "Cannot resolve RenderQuality output root: " +
                         error.message());
            return false;
        }
        std::filesystem::create_directories(absolute, error);
        if (error || !std::filesystem::is_directory(absolute, error) || error)
        {
            SetError(outError,
                     "Cannot create RenderQuality output root: " +
                         (error ? error.message() : "not a directory"));
            return false;
        }

        outRoot = std::filesystem::weakly_canonical(absolute, error);
        if (error || outRoot.empty())
        {
            SetError(outError,
                     "Cannot canonicalize RenderQuality output root: " +
                         error.message());
            return false;
        }
        return true;
    }

    [[nodiscard]] bool WriteBytesAtomically(
        const std::filesystem::path& root,
        const std::filesystem::path& destination,
        const std::vector<uint8>& bytes,
        std::vector<std::filesystem::path>& inOutPublished,
        std::string* outError)
    {
        std::error_code existsError;
        if (!IsContained(root, destination) ||
            std::filesystem::exists(destination, existsError) || existsError)
        {
            SetError(outError,
                     "RenderQuality output already exists or escapes root: " +
                         destination.string());
            return false;
        }
        const std::filesystem::path temporary =
            destination.parent_path() /
            ("." + destination.filename().string() + ".rvx-tmp");
        existsError.clear();
        if (!IsContained(root, temporary) ||
            std::filesystem::exists(temporary, existsError) || existsError)
        {
            SetError(outError,
                     "RenderQuality temporary output already exists or escapes root: " +
                         temporary.string());
            return false;
        }

        {
            std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
            if (!stream)
            {
                SetError(outError,
                         "Cannot open RenderQuality temporary output: " +
                             temporary.string());
                return false;
            }
            if (!bytes.empty())
            {
                stream.write(reinterpret_cast<const char*>(bytes.data()),
                             static_cast<std::streamsize>(bytes.size()));
            }
            stream.flush();
            if (!stream)
            {
                std::error_code removeError;
                std::filesystem::remove(temporary, removeError);
                SetError(outError,
                         "Cannot write RenderQuality temporary output: " +
                             temporary.string());
                return false;
            }
        }

        std::error_code renameError;
        std::filesystem::rename(temporary, destination, renameError);
        if (renameError)
        {
            std::error_code removeError;
            std::filesystem::remove(temporary, removeError);
            SetError(outError,
                     "Cannot atomically publish RenderQuality output: " +
                         renameError.message());
            return false;
        }
        inOutPublished.push_back(destination);
        return true;
    }

    void RemovePublishedArtifacts(
        const std::vector<std::filesystem::path>& published) noexcept
    {
        for (auto iterator = published.rbegin(); iterator != published.rend();
             ++iterator)
        {
            std::error_code error;
            std::filesystem::remove(*iterator, error);
        }
    }

    [[nodiscard]] bool ValidateAttachment(
        const QualificationCaptureAttachment& attachment,
        QualificationCaptureAttachmentSemantic semantic,
        const QualificationCaptureGraphProvenance& bundleProvenance,
        uint64& inOutAggregateBytes,
        std::string* outError)
    {
        const QualificationCaptureAttachmentContract expected =
            GetQualificationCaptureAttachmentContract(semantic);
        const QualificationCaptureAttachmentDescriptor& descriptor =
            attachment.descriptor;
        const uint32 channelCount = GetChannelCount(expected.format);
        if (expected.format == QualificationCapturePixelFormat::Unknown ||
            !descriptor.IsValid() || descriptor.contract != expected ||
            channelCount == 0U || !descriptor.extent.IsValid() ||
            descriptor.extent.width > RVX_RENDER_QUALITY_MAX_ATTACHMENT_WIDTH ||
            descriptor.extent.height > RVX_RENDER_QUALITY_MAX_ATTACHMENT_HEIGHT ||
            descriptor.extent.width >
                std::numeric_limits<uint32>::max() / (channelCount * 4U))
        {
            SetError(outError, "RenderQuality attachment descriptor is invalid");
            return false;
        }

        const uint32 requiredRowPitch = descriptor.extent.width * channelCount * 4U;
        const uint64 requiredBytes =
            static_cast<uint64>(requiredRowPitch) * descriptor.extent.height;
        if (descriptor.rowPitchBytes != requiredRowPitch ||
            requiredBytes == 0U ||
            requiredBytes > RVX_RENDER_QUALITY_MAX_ATTACHMENT_BYTES ||
            requiredBytes != attachment.bytes.size() ||
            Hash::ComputeSHA256(attachment.bytes.data(), attachment.bytes.size()) !=
                attachment.payloadHash)
        {
            SetError(outError,
                     "RenderQuality attachment payload metadata or hash is invalid");
            return false;
        }
        if (inOutAggregateBytes >
                RVX_RENDER_QUALITY_MAX_BUNDLE_BYTES - requiredBytes ||
            descriptor.provenance.graphIdentity !=
                bundleProvenance.graphIdentity ||
            descriptor.provenance.graphRecordingGeneration !=
                bundleProvenance.graphRecordingGeneration ||
            descriptor.provenance.graphRecordEpoch !=
                bundleProvenance.graphRecordEpoch)
        {
            SetError(outError,
                     "RenderQuality attachment provenance or aggregate payload is invalid");
            return false;
        }
        inOutAggregateBytes += requiredBytes;

        for (size_t offset = 0; offset < attachment.bytes.size();
             offset += sizeof(float32))
        {
            float32 value = 0.0f;
            std::memcpy(&value, attachment.bytes.data() + offset, sizeof(value));
            if (!std::isfinite(value))
            {
                SetError(outError,
                         "RenderQuality attachment payload contains non-finite values");
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] bool ValidateInputs(
        const RenderQualityQualificationWriteRequest& request,
        const QualificationCaptureBundle& bundle,
        const RenderExecutionReceipt& receipt,
        std::string* outError)
    {
        if constexpr (std::endian::native != std::endian::little)
        {
            SetError(outError,
                     "RenderQuality raw float32 artifacts require a little-endian host");
            return false;
        }
        if (!IsNonEmptyText(request.sampleId) ||
            !IsNonEmptyText(request.cameraBookmarkId) ||
            !IsNonEmptyText(request.runId) ||
            !IsDeclaredRenderPathName(request.renderPath) ||
            !IsDeclaredInstancingModeName(request.instancingMode) ||
            request.visualContractVersion == 0U ||
            request.submittedFrame == 0U || request.completedFrame == 0U ||
            request.completedFrame < request.submittedFrame)
        {
            SetError(outError,
                     "RenderQuality writer request lacks required scenario, runtime configuration, or completion provenance");
            return false;
        }
        if (bundle.schemaVersion !=
                RVX_QUALIFICATION_CAPTURE_SCHEMA_VERSION ||
            !bundle.identity.IsValid() || !receipt.IsStructurallyValid() ||
            !receipt.build.IsStructurallyValid() || !receipt.available ||
            !receipt.graphComposed || !receipt.executionSucceeded ||
            !receipt.presented || !receipt.succeeded ||
            receipt.frameIdentity != bundle.identity.frameIdentity ||
            receipt.viewIdentity != bundle.identity.viewIdentity)
        {
            SetError(outError,
                     "RenderQuality bundle and presentation-qualified receipt do not share exact identity");
            return false;
        }
        const std::string receiptDigest =
            ComputeRenderExecutionReceiptSha256(receipt);
        if (receiptDigest.size() != 64U ||
            Hash::FormatSHA256Digest(
                bundle.identity.graphProvenance.executionReceiptDigest) !=
                receiptDigest)
        {
            SetError(outError,
                     "RenderQuality bundle digest does not bind the exact presentation receipt");
            return false;
        }
        uint64 aggregateBytes = 0;
        for (size_t index = 0; index < bundle.attachments.size(); ++index)
        {
            if (!ValidateAttachment(
                    bundle.attachments[index],
                    static_cast<QualificationCaptureAttachmentSemantic>(index),
                    bundle.identity.graphProvenance,
                    aggregateBytes,
                    outError))
            {
                return false;
            }
        }
        for (size_t left = 0; left < bundle.attachments.size(); ++left)
        {
            const QualificationCaptureAttachmentProvenance& provenance =
                bundle.attachments[left].descriptor.provenance;
            for (size_t right = left + 1U; right < bundle.attachments.size();
                 ++right)
            {
                const QualificationCaptureAttachmentProvenance& candidate =
                    bundle.attachments[right].descriptor.provenance;
                if (provenance.graphResourceIdentity ==
                        candidate.graphResourceIdentity &&
                    provenance.graphResourceGeneration ==
                        candidate.graphResourceGeneration)
                {
                    SetError(outError,
                             "RenderQuality bundle reuses graph resource provenance");
                    return false;
                }
            }
        }
        return true;
    }

    void WriteAttachmentJson(
        std::ostream& stream,
        const QualificationCaptureBundle& bundle,
        const QualificationCaptureAttachment& attachment,
        QualificationCaptureAttachmentSemantic semantic,
        const RenderExecutionReceipt& receipt,
        const RenderQualityQualificationWriteRequest& request,
        const std::string& receiptSha256)
    {
        const QualificationCaptureAttachmentDescriptor& descriptor =
            attachment.descriptor;
        const QualificationCaptureAttachmentProvenance& provenance =
            descriptor.provenance;
        const AttachmentSourceDescription& source = GetSourceDescription(semantic);
        stream << "    {\n";
        stream << "      \"semantic\": "
               << Diagnostics::JsonString(
                      semantic == QualificationCaptureAttachmentSemantic::BeautyLDR
                          ? "BeautyLDR"
                          : semantic == QualificationCaptureAttachmentSemantic::PreToneHDR
                              ? "PreToneHDR"
                              : semantic == QualificationCaptureAttachmentSemantic::LinearDepth
                                  ? "LinearDepth"
                                  : semantic == QualificationCaptureAttachmentSemantic::Normal
                                      ? "Normal"
                                      : semantic == QualificationCaptureAttachmentSemantic::Velocity
                                          ? "Velocity"
                                          : semantic == QualificationCaptureAttachmentSemantic::AmbientOcclusion
                                              ? "AmbientOcclusion"
                                              : "ReactiveMask")
               << ",\n";
        stream << "      \"dataPath\": "
               << Diagnostics::JsonString(source.rawFileName) << ",\n";
        stream << "      \"format\": "
               << Diagnostics::JsonString(GetPixelFormatName(
                      descriptor.contract.format)) << ",\n";
        stream << "      \"colorDomain\": "
               << Diagnostics::JsonString(GetColorDomainName(
                      descriptor.contract.colorDomain)) << ",\n";
        stream << "      \"width\": " << descriptor.extent.width << ",\n";
        stream << "      \"height\": " << descriptor.extent.height << ",\n";
        stream << "      \"channels\": "
               << GetChannelCount(descriptor.contract.format) << ",\n";
        stream << "      \"scalarType\": \"float32le\",\n";
        stream << "      \"rowPitchBytes\": " << descriptor.rowPitchBytes
               << ",\n";
        stream << "      \"payloadBytes\": " << attachment.bytes.size()
               << ",\n";
        stream << "      \"sha256\": "
               << Diagnostics::JsonString(
                      Hash::FormatSHA256Digest(attachment.payloadHash)) << ",\n";
        stream << "      \"provenance\": {\n";
        stream << "        \"sourceCommit\": "
               << Diagnostics::JsonString(receipt.build.sourceCommit) << ",\n";
        stream << "        \"runId\": "
               << Diagnostics::JsonString(request.runId) << ",\n";
        stream << "        \"preset\": "
               << Diagnostics::JsonString(receipt.build.preset) << ",\n";
        stream << "        \"renderExecutionReceiptSha256\": "
               << Diagnostics::JsonString(receiptSha256) << ",\n";
        stream << "        \"frameSequence\": "
               << receipt.frameIdentity.frameSequence << ",\n";
        stream << "        \"submittedFrame\": " << request.submittedFrame
               << ",\n";
        stream << "        \"completedFrame\": " << request.completedFrame
               << ",\n";
        stream << "        \"sceneRevision\": "
               << receipt.frameIdentity.sceneRevision << ",\n";
        stream << "        \"surfaceGeneration\": "
               << receipt.frameIdentity.surfaceGeneration << ",\n";
        stream << "        \"configurationRevision\": "
               << receipt.frameIdentity.configurationRevision << ",\n";
        stream << "        \"viewId\": "
               << Diagnostics::JsonString(request.cameraBookmarkId) << ",\n";
        stream << "        \"viewSceneRuntimeId\": "
               << receipt.viewIdentity.sceneRuntimeId << ",\n";
        stream << "        \"viewCameraEntityPacked\": "
               << receipt.viewIdentity.cameraEntityPacked << ",\n";
        stream << "        \"viewSubViewIndex\": "
               << receipt.viewIdentity.subViewIndex << ",\n";
        stream << "        \"viewRole\": "
               << Diagnostics::JsonString(
                      GetViewRoleProvenanceName(receipt.viewIdentity.role))
               << ",\n";
        stream << "        \"graphIdentity\": "
               << bundle.identity.graphProvenance.graphIdentity << ",\n";
        stream << "        \"graphRecordingGeneration\": "
               << bundle.identity.graphProvenance.graphRecordingGeneration
               << ",\n";
        stream << "        \"graphRecordEpoch\": "
               << bundle.identity.graphProvenance.graphRecordEpoch << ",\n";
        stream << "        \"graphResourceIdentity\": "
               << provenance.graphResourceIdentity << ",\n";
        stream << "        \"graphResourceGeneration\": "
               << provenance.graphResourceGeneration << ",\n";
        stream << "        \"producerIdentity\": "
               << provenance.producerIdentity << ",\n";
        stream << "        \"resourceName\": "
               << Diagnostics::JsonString(source.resourceName) << ",\n";
        stream << "        \"graphPasses\": ["
               << Diagnostics::JsonString(source.graphPassName) << "]\n";
        stream << "      }\n";
        stream << "    }";
    }

    [[nodiscard]] std::vector<uint8> ToBytes(const std::string& value)
    {
        const auto* begin = reinterpret_cast<const uint8*>(value.data());
        return std::vector<uint8>(begin, begin + value.size());
    }

    [[nodiscard]] bool IsLowercaseHex(
        const std::string& value,
        const size_t expectedLength) noexcept
    {
        return value.size() == expectedLength &&
               std::all_of(value.begin(), value.end(), [](const char value)
               {
                   return (value >= '0' && value <= '9') ||
                          (value >= 'a' && value <= 'f');
               });
    }

    [[nodiscard]] bool IsCommitDigest(const std::string& value) noexcept
    {
        return IsLowercaseHex(value, 40U) || IsLowercaseHex(value, 64U);
    }

    [[nodiscard]] bool IsPosixRelativeBundlePath(
        const std::string& value) noexcept
    {
        if (value.empty() || value.find('\\') != std::string::npos)
        {
            return false;
        }
        const std::filesystem::path path(value);
        if (path.is_absolute() || path.filename() != "bundle.json" ||
            path.generic_string() != value)
        {
            return false;
        }
        for (const std::filesystem::path& component : path)
        {
            if (component.empty() || component == "." || component == "..")
            {
                return false;
            }
        }
        return true;
    }

    void AppendSeriesBinding(
        std::string& outBinding,
        const char* name,
        const std::string& value)
    {
        outBinding += name;
        outBinding += ':';
        outBinding += std::to_string(value.size());
        outBinding += ':';
        outBinding += value;
        outBinding += '\n';
    }

    void AppendSeriesBinding(
        std::string& outBinding,
        const char* name,
        const uint64 value)
    {
        AppendSeriesBinding(outBinding, name, std::to_string(value));
    }

    [[nodiscard]] std::string ComputeSeriesSha256(
        const RenderQualityAttachmentSeriesWriteRequest& request)
    {
        std::string binding;
        binding.reserve(1024U + request.ticks.size() * 512U);
        AppendSeriesBinding(binding, "schema",
                            "RVX.RenderQualityAttachmentSeries");
        AppendSeriesBinding(binding, "schemaVersion",
                            static_cast<uint64>(
                                RVX_RENDER_QUALITY_ATTACHMENT_SERIES_SCHEMA_VERSION));
        AppendSeriesBinding(binding, "sourceCommit", request.sourceCommit);
        AppendSeriesBinding(binding, "runId", request.runId);
        AppendSeriesBinding(binding, "preset", request.preset);
        AppendSeriesBinding(binding, "identity.sampleId",
                            request.qualificationIdentity.sampleId);
        AppendSeriesBinding(binding, "identity.pipeline",
                            request.qualificationIdentity.pipeline);
        AppendSeriesBinding(binding, "identity.quality",
                            request.qualificationIdentity.quality);
        AppendSeriesBinding(binding, "identity.backend",
                            request.qualificationIdentity.backend);
        AppendSeriesBinding(binding, "identity.view",
                            request.qualificationIdentity.view);
        AppendSeriesBinding(binding, "identity.visualContractVersion",
                            static_cast<uint64>(
                                request.qualificationIdentity.visualContractVersion));
        AppendSeriesBinding(binding, "configuration.renderPath",
                            request.renderConfigurationIdentity.renderPath);
        AppendSeriesBinding(binding, "configuration.instancingMode",
                            request.renderConfigurationIdentity.instancingMode);
        AppendSeriesBinding(binding, "configuration.requestedRecipe",
                            request.renderConfigurationIdentity.requestedRecipe);
        AppendSeriesBinding(binding, "configuration.resolvedRecipe",
                            request.renderConfigurationIdentity.resolvedRecipe);
        AppendSeriesBinding(binding, "configuration.profileHash",
                            request.renderConfigurationIdentity.profileHash);
        AppendSeriesBinding(binding, "configuration.renderConfigurationHash",
                            request.renderConfigurationIdentity
                                .renderConfigurationHash);
        AppendSeriesBinding(binding, "hardware.backend",
                            request.hardwareIdentity.backend);
        AppendSeriesBinding(binding, "hardware.adapterName",
                            request.hardwareIdentity.adapterName);
        AppendSeriesBinding(binding, "hardware.driverVersion",
                            request.hardwareIdentity.driverVersion);
        AppendSeriesBinding(binding, "hardware.osBuild",
                            request.hardwareIdentity.osBuild);
        AppendSeriesBinding(binding, "hardware.capabilityHash",
                            request.hardwareIdentity.capabilityHash);
        AppendSeriesBinding(binding, "manifestSha256", request.manifestSha256);
        for (const RenderQualityAttachmentSeriesTick& tick : request.ticks)
        {
            AppendSeriesBinding(binding, "tick.captureTick", tick.captureTick);
            AppendSeriesBinding(binding, "tick.bundleId", tick.bundleId);
            AppendSeriesBinding(binding, "tick.bundlePath", tick.bundlePath);
            AppendSeriesBinding(binding, "tick.bundleSha256", tick.bundleSha256);
            AppendSeriesBinding(binding, "tick.receiptSha256",
                                tick.receiptSha256);
            AppendSeriesBinding(binding, "tick.frameSequence", tick.frameSequence);
            AppendSeriesBinding(binding, "tick.submittedFrame", tick.submittedFrame);
            AppendSeriesBinding(binding, "tick.completedFrame", tick.completedFrame);
        }
        return Hash::FormatSHA256Digest(Hash::ComputeSHA256(binding));
    }

    [[nodiscard]] std::string ComputeRenderConfigurationHash(
        const RenderQualityQualificationWriteRequest& request,
        const RenderExecutionReceipt& receipt)
    {
        std::string binding;
        binding.reserve(512U);
        AppendSeriesBinding(binding, "schema",
                            "RVX.RenderQualityConfigurationIdentity");
        AppendSeriesBinding(binding, "schemaVersion", 1U);
        AppendSeriesBinding(binding, "renderPath", request.renderPath);
        AppendSeriesBinding(binding, "instancingMode", request.instancingMode);
        AppendSeriesBinding(binding, "requestedRecipe",
                            GetRenderPipelineRecipeName(receipt.requestedRecipe));
        AppendSeriesBinding(binding, "resolvedRecipe",
                            GetRenderPipelineRecipeName(receipt.resolvedRecipe));
        AppendSeriesBinding(binding, "quality",
                            GetRenderQualityTierName(receipt.quality));
        AppendSeriesBinding(binding, "backend",
                            GetSampleBackendName(receipt.backend));
        AppendSeriesBinding(binding, "profileHash", receipt.profileHash);
        return Hash::FormatSHA256Digest(Hash::ComputeSHA256(binding));
    }

    [[nodiscard]] std::string ComputeRenderConfigurationHash(
        const RenderQualityAttachmentSeriesConfigurationIdentity& configuration,
        const RenderQualityAttachmentSeriesIdentity& identity)
    {
        std::string binding;
        binding.reserve(512U);
        AppendSeriesBinding(binding, "schema",
                            "RVX.RenderQualityConfigurationIdentity");
        AppendSeriesBinding(binding, "schemaVersion", 1U);
        AppendSeriesBinding(binding, "renderPath", configuration.renderPath);
        AppendSeriesBinding(
            binding, "instancingMode", configuration.instancingMode);
        AppendSeriesBinding(
            binding, "requestedRecipe", configuration.requestedRecipe);
        AppendSeriesBinding(
            binding, "resolvedRecipe", configuration.resolvedRecipe);
        AppendSeriesBinding(binding, "quality", identity.quality);
        AppendSeriesBinding(binding, "backend", identity.backend);
        AppendSeriesBinding(binding, "profileHash", configuration.profileHash);
        return Hash::FormatSHA256Digest(Hash::ComputeSHA256(binding));
    }

    [[nodiscard]] bool ReadAndValidateBundleSha256(
        const std::filesystem::path& root,
        const RenderQualityAttachmentSeriesWriteRequest& request,
        const RenderQualityAttachmentSeriesTick& tick,
        std::string* outError)
    {
        if (!IsPosixRelativeBundlePath(tick.bundlePath))
        {
            SetError(outError,
                     "RenderQuality attachment series bundle path is not POSIX-relative");
            return false;
        }
        std::error_code error;
        const std::filesystem::path candidate =
            std::filesystem::weakly_canonical(
                root / std::filesystem::path(tick.bundlePath), error);
        if (error || !IsContained(root, candidate) ||
            !std::filesystem::is_regular_file(candidate, error) || error)
        {
            SetError(outError,
                     "RenderQuality attachment series bundle is missing or escapes its root");
            return false;
        }
        const uintmax_t size = std::filesystem::file_size(candidate, error);
        if (error || size == 0U || size > 16ULL * 1024ULL * 1024ULL)
        {
            SetError(outError,
                     "RenderQuality attachment series bundle has an invalid size");
            return false;
        }
        std::ifstream stream(candidate, std::ios::binary);
        if (!stream)
        {
            SetError(outError,
                     "Cannot read RenderQuality attachment series bundle");
            return false;
        }
        std::vector<uint8> bytes(static_cast<size_t>(size));
        stream.read(reinterpret_cast<char*>(bytes.data()),
                    static_cast<std::streamsize>(bytes.size()));
        if (!stream || stream.gcount() != static_cast<std::streamsize>(bytes.size()) ||
            Hash::FormatSHA256Digest(
                Hash::ComputeSHA256(bytes.data(), bytes.size())) != tick.bundleSha256)
        {
            SetError(outError,
                     "RenderQuality attachment series bundle hash does not match");
            return false;
        }
        const std::string bundleJson(
            reinterpret_cast<const char*>(bytes.data()), bytes.size());
        const auto contains = [&bundleJson](const std::string& text)
        {
            return bundleJson.find(text) != std::string::npos;
        };
        if (!contains("\"schema\": \"RVX.RenderQualityAttachmentBundle\"") ||
            !contains("\"schemaVersion\": 2") ||
            !contains("\"bundleId\": \"" +
                      std::to_string(tick.bundleId) + "\"") ||
            !contains("\"sourceCommit\": \"" + request.sourceCommit +
                      "\"") ||
            !contains("\"sampleId\": \"" +
                      request.qualificationIdentity.sampleId + "\"") ||
            !contains("\"pipeline\": \"" +
                      request.qualificationIdentity.pipeline + "\"") ||
            !contains("\"quality\": \"" +
                      request.qualificationIdentity.quality + "\"") ||
            !contains("\"backend\": \"" +
                      request.qualificationIdentity.backend + "\"") ||
            !contains("\"view\": \"" +
                      request.qualificationIdentity.view + "\"") ||
            !contains("\"captureTick\": " +
                      std::to_string(tick.captureTick)) ||
            !contains("\"visualContractVersion\": " + std::to_string(
                      request.qualificationIdentity.visualContractVersion)) ||
            !contains("\"renderPath\": \"" +
                      request.renderConfigurationIdentity.renderPath + "\"") ||
            !contains("\"instancingMode\": \"" +
                      request.renderConfigurationIdentity.instancingMode + "\"") ||
            !contains("\"requestedRecipe\": \"" +
                      request.renderConfigurationIdentity.requestedRecipe + "\"") ||
            !contains("\"resolvedRecipe\": \"" +
                      request.renderConfigurationIdentity.resolvedRecipe + "\"") ||
            !contains("\"profileHash\": " + std::to_string(
                      request.renderConfigurationIdentity.profileHash)) ||
            !contains("\"renderConfigurationHash\": \"" +
                      request.renderConfigurationIdentity
                          .renderConfigurationHash + "\"") ||
            !contains("\"runId\": \"" + request.runId + "\"") ||
            !contains("\"preset\": \"" + request.preset + "\"") ||
            !contains("\"renderExecutionReceiptSha256\": \"" +
                      tick.receiptSha256 + "\"") ||
            !contains("\"frameSequence\": " +
                      std::to_string(tick.frameSequence)) ||
            !contains("\"submittedFrame\": " +
                      std::to_string(tick.submittedFrame)) ||
            !contains("\"completedFrame\": " +
                      std::to_string(tick.completedFrame)))
        {
            SetError(outError,
                     "RenderQuality attachment series bundle does not match the required Bundle V2 schema and identity");
            return false;
        }
        return true;
    }

    [[nodiscard]] bool ValidateSeriesRequest(
        const RenderQualityAttachmentSeriesWriteRequest& request,
        std::string* outError)
    {
        const RenderQualityAttachmentSeriesIdentity& identity =
            request.qualificationIdentity;
        const RenderQualityAttachmentSeriesHardwareIdentity& hardware =
            request.hardwareIdentity;
        const RenderQualityAttachmentSeriesConfigurationIdentity& configuration =
            request.renderConfigurationIdentity;
        RenderPipelineRecipe identityRecipe = RenderPipelineRecipe::Count;
        RenderPipelineRecipe requestedRecipe = RenderPipelineRecipe::Count;
        RenderPipelineRecipe resolvedRecipe = RenderPipelineRecipe::Count;
        RenderQualityTier quality = RenderQualityTier::Count;
        RHIBackendType backend = RHIBackendType::None;
        const bool declaredConfiguration =
            IsDeclaredRenderPathName(configuration.renderPath) &&
            IsDeclaredInstancingModeName(configuration.instancingMode) &&
            ParseRenderPipelineRecipe(identity.pipeline, identityRecipe) &&
            ParseRenderPipelineRecipe(
                configuration.requestedRecipe, requestedRecipe) &&
            ParseRenderPipelineRecipe(
                configuration.resolvedRecipe, resolvedRecipe) &&
            ParseRenderQualityTier(identity.quality, quality) &&
            ParseSampleBackend(identity.backend, backend) &&
            identity.backend == GetSampleBackendName(backend) &&
            backend != RHIBackendType::Auto &&
            backend != RHIBackendType::None &&
            IsPresentableRasterRecipe(identityRecipe) &&
            IsPresentableRasterRecipe(requestedRecipe) &&
            IsPresentableRasterRecipe(resolvedRecipe) &&
            identityRecipe == resolvedRecipe &&
            configuration.profileHash ==
                ComputeRenderPipelineProfileHash(resolvedRecipe, quality) &&
            configuration.renderConfigurationHash ==
                ComputeRenderConfigurationHash(configuration, identity);
        if (request.outputPath.filename() != "series.json" ||
            !IsCommitDigest(request.sourceCommit) || !IsNonEmptyText(request.runId) ||
            !IsNonEmptyText(request.preset) || !IsNonEmptyText(identity.sampleId) ||
            !IsNonEmptyText(identity.pipeline) || !IsNonEmptyText(identity.quality) ||
            !IsNonEmptyText(identity.backend) || !IsNonEmptyText(identity.view) ||
            identity.visualContractVersion == 0U ||
            !declaredConfiguration ||
            !IsNonEmptyText(hardware.backend) ||
            !IsNonEmptyText(hardware.adapterName) ||
            !IsNonEmptyText(hardware.driverVersion) ||
            !IsNonEmptyText(hardware.osBuild) ||
            !IsLowercaseHex(hardware.capabilityHash, 64U) ||
            hardware.backend != identity.backend ||
            !IsLowercaseHex(request.manifestSha256, 64U) || request.ticks.empty())
        {
            SetError(outError,
                     "RenderQuality attachment series request lacks strict identity, runtime configuration, or hardware provenance");
            return false;
        }

        uint64 previousTick = 0;
        uint64 previousBundleId = 0;
        uint64 previousFrame = 0;
        uint64 previousSubmitted = 0;
        uint64 previousCompleted = 0;
        for (const RenderQualityAttachmentSeriesTick& tick : request.ticks)
        {
            if (tick.captureTick == 0U || tick.bundleId == 0U ||
                tick.frameSequence == 0U || tick.submittedFrame == 0U ||
                tick.completedFrame < tick.submittedFrame ||
                !IsPosixRelativeBundlePath(tick.bundlePath) ||
                !IsLowercaseHex(tick.bundleSha256, 64U) ||
                !IsLowercaseHex(tick.receiptSha256, 64U) ||
                (previousTick != 0U && tick.captureTick <= previousTick) ||
                (previousBundleId != 0U && tick.bundleId <= previousBundleId) ||
                (previousFrame != 0U && tick.frameSequence <= previousFrame) ||
                (previousSubmitted != 0U &&
                 tick.submittedFrame <= previousSubmitted) ||
                (previousCompleted != 0U &&
                 tick.completedFrame <= previousCompleted))
            {
                SetError(outError,
                         "RenderQuality attachment series ticks are unordered or invalid");
                return false;
            }
            previousTick = tick.captureTick;
            previousBundleId = tick.bundleId;
            previousFrame = tick.frameSequence;
            previousSubmitted = tick.submittedFrame;
            previousCompleted = tick.completedFrame;
        }
        return true;
    }
} // namespace

bool WriteRenderQualityQualificationBundle(
    const RenderQualityQualificationWriteRequest& request,
    const QualificationCaptureBundle& bundle,
    const RenderExecutionReceipt& receipt,
    RenderQualityQualificationWriteResult& outResult,
    std::string* outError)
{
    outResult = {};
    if (!ValidateInputs(request, bundle, receipt, outError))
    {
        return false;
    }

    std::filesystem::path root;
    if (!PrepareOutputRoot(request.outputRoot, root, outError))
    {
        return false;
    }

    const std::filesystem::path manifestPath = root / "bundle.json";
    std::error_code manifestExistsError;
    if (!IsContained(root, manifestPath) ||
        std::filesystem::exists(manifestPath, manifestExistsError) ||
        manifestExistsError)
    {
        SetError(outError,
                 "RenderQuality bundle manifest already exists or escapes output root");
        return false;
    }

    const std::string receiptSha256 =
        ComputeRenderExecutionReceiptSha256(receipt);
    if (receiptSha256.size() != 64U)
    {
        SetError(outError,
                 "RenderQuality receipt serializer did not produce a SHA-256 digest");
        return false;
    }
    const RenderQualityAttachmentSeriesConfigurationIdentity
        renderConfiguration{
            .renderPath = request.renderPath,
            .instancingMode = request.instancingMode,
            .requestedRecipe = GetRenderPipelineRecipeName(
                receipt.requestedRecipe),
            .resolvedRecipe = GetRenderPipelineRecipeName(
                receipt.resolvedRecipe),
            .profileHash = receipt.profileHash,
            .renderConfigurationHash = ComputeRenderConfigurationHash(
                request, receipt)};
    if (!IsLowercaseHex(renderConfiguration.renderConfigurationHash, 64U))
    {
        SetError(outError,
                 "RenderQuality runtime configuration hash could not be computed");
        return false;
    }

    std::vector<std::filesystem::path> published;
    for (size_t index = 0; index < bundle.attachments.size(); ++index)
    {
        const QualificationCaptureAttachmentSemantic semantic =
            static_cast<QualificationCaptureAttachmentSemantic>(index);
        const AttachmentSourceDescription& source = GetSourceDescription(semantic);
        if (source.rawFileName == nullptr || source.resourceName == nullptr ||
            source.graphPassName == nullptr ||
            !WriteBytesAtomically(root,
                                  root / source.rawFileName,
                                  bundle.attachments[index].bytes,
                                  published,
                                  outError))
        {
            RemovePublishedArtifacts(published);
            return false;
        }
    }

    std::ostringstream manifest;
    manifest.imbue(std::locale::classic());
    manifest << "{\n";
    manifest << "  \"schema\": \"RVX.RenderQualityAttachmentBundle\",\n";
    manifest << "  \"schemaVersion\": "
             << RVX_RENDER_QUALITY_ATTACHMENT_BUNDLE_SCHEMA_VERSION << ",\n";
    manifest << "  \"bundleId\": "
             << Diagnostics::JsonString(std::to_string(bundle.identity.bundleId))
             << ",\n";
    manifest << "  \"sourceCommit\": "
             << Diagnostics::JsonString(receipt.build.sourceCommit) << ",\n";
    manifest << "  \"qualificationIdentity\": {\n";
    manifest << "    \"sampleId\": "
             << Diagnostics::JsonString(request.sampleId) << ",\n";
    manifest << "    \"pipeline\": "
             << Diagnostics::JsonString(
                    GetRenderPipelineRecipeName(receipt.resolvedRecipe)) << ",\n";
    manifest << "    \"quality\": "
             << Diagnostics::JsonString(GetRenderQualityTierName(receipt.quality))
             << ",\n";
    manifest << "    \"backend\": "
             << Diagnostics::JsonString(GetSampleBackendName(receipt.backend))
             << ",\n";
    manifest << "    \"view\": "
             << Diagnostics::JsonString(request.cameraBookmarkId) << ",\n";
    manifest << "    \"captureTick\": " << request.captureTick << ",\n";
    manifest << "    \"visualContractVersion\": "
             << request.visualContractVersion << "\n";
    manifest << "  },\n";
    manifest << "  \"renderConfigurationIdentity\": {\n";
    manifest << "    \"renderPath\": "
             << Diagnostics::JsonString(renderConfiguration.renderPath) << ",\n";
    manifest << "    \"instancingMode\": "
             << Diagnostics::JsonString(renderConfiguration.instancingMode) << ",\n";
    manifest << "    \"requestedRecipe\": "
             << Diagnostics::JsonString(renderConfiguration.requestedRecipe) << ",\n";
    manifest << "    \"resolvedRecipe\": "
             << Diagnostics::JsonString(renderConfiguration.resolvedRecipe) << ",\n";
    manifest << "    \"profileHash\": " << renderConfiguration.profileHash << ",\n";
    manifest << "    \"renderConfigurationHash\": "
             << Diagnostics::JsonString(
                    renderConfiguration.renderConfigurationHash) << "\n";
    manifest << "  },\n";
    manifest << "  \"execution\": {\n";
    manifest << "    \"runId\": " << Diagnostics::JsonString(request.runId)
             << ",\n";
    manifest << "    \"preset\": "
             << Diagnostics::JsonString(receipt.build.preset) << ",\n";
    manifest << "    \"renderExecutionReceiptSha256\": "
             << Diagnostics::JsonString(receiptSha256) << ",\n";
    manifest << "    \"completionOwned\": true,\n";
    manifest << "    \"frameSequence\": "
             << receipt.frameIdentity.frameSequence << ",\n";
    manifest << "    \"submittedFrame\": " << request.submittedFrame
             << ",\n";
    manifest << "    \"completedFrame\": " << request.completedFrame
             << ",\n";
    manifest << "    \"sceneRevision\": "
             << receipt.frameIdentity.sceneRevision << ",\n";
    manifest << "    \"surfaceGeneration\": "
             << receipt.frameIdentity.surfaceGeneration << ",\n";
    manifest << "    \"configurationRevision\": "
             << receipt.frameIdentity.configurationRevision << ",\n";
    manifest << "    \"viewId\": "
             << Diagnostics::JsonString(request.cameraBookmarkId) << ",\n";
    manifest << "    \"viewSceneRuntimeId\": "
             << receipt.viewIdentity.sceneRuntimeId << ",\n";
    manifest << "    \"viewCameraEntityPacked\": "
             << receipt.viewIdentity.cameraEntityPacked << ",\n";
    manifest << "    \"viewSubViewIndex\": "
             << receipt.viewIdentity.subViewIndex << ",\n";
    manifest << "    \"viewRole\": "
             << Diagnostics::JsonString(
                    GetViewRoleProvenanceName(receipt.viewIdentity.role))
             << ",\n";
    manifest << "    \"graphIdentity\": "
             << bundle.identity.graphProvenance.graphIdentity << ",\n";
    manifest << "    \"graphRecordingGeneration\": "
             << bundle.identity.graphProvenance.graphRecordingGeneration
             << ",\n";
    manifest << "    \"graphRecordEpoch\": "
             << bundle.identity.graphProvenance.graphRecordEpoch << "\n";
    manifest << "  },\n";
    manifest << "  \"attachments\": [\n";
    for (size_t index = 0; index < bundle.attachments.size(); ++index)
    {
        WriteAttachmentJson(
            manifest, bundle, bundle.attachments[index],
            static_cast<QualificationCaptureAttachmentSemantic>(index), receipt,
            request, receiptSha256);
        manifest << (index + 1U < bundle.attachments.size() ? ",\n" : "\n");
    }
    manifest << "  ]\n";
    manifest << "}\n";

    const std::vector<uint8> manifestBytes = ToBytes(manifest.str());
    const std::string bundleSha256 = Hash::FormatSHA256Digest(
        Hash::ComputeSHA256(manifestBytes.data(), manifestBytes.size()));
    if (!WriteBytesAtomically(
            root, manifestPath, manifestBytes, published, outError))
    {
        RemovePublishedArtifacts(published);
        return false;
    }

    outResult.bundlePath = manifestPath;
    outResult.bundleSha256 = bundleSha256;
    outResult.renderExecutionReceiptSha256 = receiptSha256;
    outResult.renderConfigurationIdentity = renderConfiguration;
    return true;
}

bool WriteRenderQualityAttachmentSeries(
    const RenderQualityAttachmentSeriesWriteRequest& request,
    RenderQualityAttachmentSeriesWriteResult& outResult,
    std::string* outError)
{
    outResult = {};
    if (!ValidateSeriesRequest(request, outError))
    {
        return false;
    }

    std::filesystem::path root;
    const std::filesystem::path requestedRoot =
        request.outputPath.has_parent_path()
            ? request.outputPath.parent_path()
            : std::filesystem::path(".");
    if (!PrepareOutputRoot(requestedRoot, root, outError))
    {
        return false;
    }
    const std::filesystem::path seriesPath = root / "series.json";
    std::error_code existsError;
    if (!IsContained(root, seriesPath) ||
        std::filesystem::exists(seriesPath, existsError) || existsError)
    {
        SetError(outError,
                 "RenderQuality attachment series manifest already exists or escapes root");
        return false;
    }
    for (const RenderQualityAttachmentSeriesTick& tick : request.ticks)
    {
        if (!ReadAndValidateBundleSha256(root, request, tick, outError))
        {
            return false;
        }
    }

    const std::string seriesSha256 = ComputeSeriesSha256(request);
    if (!IsLowercaseHex(seriesSha256, 64U))
    {
        SetError(outError,
                 "RenderQuality attachment series digest could not be computed");
        return false;
    }

    std::ostringstream manifest;
    manifest.imbue(std::locale::classic());
    manifest << "{\n";
    manifest << "  \"schema\": \"RVX.RenderQualityAttachmentSeries\",\n";
    manifest << "  \"schemaVersion\": "
             << RVX_RENDER_QUALITY_ATTACHMENT_SERIES_SCHEMA_VERSION << ",\n";
    manifest << "  \"sourceCommit\": "
             << Diagnostics::JsonString(request.sourceCommit) << ",\n";
    manifest << "  \"runId\": " << Diagnostics::JsonString(request.runId)
             << ",\n";
    manifest << "  \"preset\": " << Diagnostics::JsonString(request.preset)
             << ",\n";
    manifest << "  \"qualificationIdentity\": {\n";
    manifest << "    \"sampleId\": "
             << Diagnostics::JsonString(request.qualificationIdentity.sampleId)
             << ",\n";
    manifest << "    \"pipeline\": "
             << Diagnostics::JsonString(request.qualificationIdentity.pipeline)
             << ",\n";
    manifest << "    \"quality\": "
             << Diagnostics::JsonString(request.qualificationIdentity.quality)
             << ",\n";
    manifest << "    \"backend\": "
             << Diagnostics::JsonString(request.qualificationIdentity.backend)
             << ",\n";
    manifest << "    \"view\": "
             << Diagnostics::JsonString(request.qualificationIdentity.view)
             << ",\n";
    manifest << "    \"visualContractVersion\": "
             << request.qualificationIdentity.visualContractVersion << "\n";
    manifest << "  },\n";
    manifest << "  \"renderConfigurationIdentity\": {\n";
    manifest << "    \"renderPath\": "
             << Diagnostics::JsonString(
                    request.renderConfigurationIdentity.renderPath) << ",\n";
    manifest << "    \"instancingMode\": "
             << Diagnostics::JsonString(
                    request.renderConfigurationIdentity.instancingMode) << ",\n";
    manifest << "    \"requestedRecipe\": "
             << Diagnostics::JsonString(
                    request.renderConfigurationIdentity.requestedRecipe) << ",\n";
    manifest << "    \"resolvedRecipe\": "
             << Diagnostics::JsonString(
                    request.renderConfigurationIdentity.resolvedRecipe) << ",\n";
    manifest << "    \"profileHash\": "
             << request.renderConfigurationIdentity.profileHash << ",\n";
    manifest << "    \"renderConfigurationHash\": "
             << Diagnostics::JsonString(
                    request.renderConfigurationIdentity.renderConfigurationHash)
             << "\n";
    manifest << "  },\n";
    manifest << "  \"hardwareIdentity\": {\n";
    manifest << "    \"backend\": "
             << Diagnostics::JsonString(request.hardwareIdentity.backend)
             << ",\n";
    manifest << "    \"adapterName\": "
             << Diagnostics::JsonString(request.hardwareIdentity.adapterName)
             << ",\n";
    manifest << "    \"driverVersion\": "
             << Diagnostics::JsonString(request.hardwareIdentity.driverVersion)
             << ",\n";
    manifest << "    \"osBuild\": "
             << Diagnostics::JsonString(request.hardwareIdentity.osBuild)
             << ",\n";
    manifest << "    \"capabilityHash\": "
             << Diagnostics::JsonString(request.hardwareIdentity.capabilityHash)
             << "\n";
    manifest << "  },\n";
    manifest << "  \"manifestSha256\": "
             << Diagnostics::JsonString(request.manifestSha256) << ",\n";
    manifest << "  \"ticks\": [\n";
    for (size_t index = 0; index < request.ticks.size(); ++index)
    {
        const RenderQualityAttachmentSeriesTick& tick = request.ticks[index];
        manifest << "    {\n";
        manifest << "      \"captureTick\": " << tick.captureTick << ",\n";
        manifest << "      \"bundleId\": "
                 << Diagnostics::JsonString(std::to_string(tick.bundleId))
                 << ",\n";
        manifest << "      \"bundlePath\": "
                 << Diagnostics::JsonString(tick.bundlePath) << ",\n";
        manifest << "      \"bundleSha256\": "
                 << Diagnostics::JsonString(tick.bundleSha256) << ",\n";
        manifest << "      \"receiptSha256\": "
                 << Diagnostics::JsonString(tick.receiptSha256)
                 << ",\n";
        manifest << "      \"frameSequence\": " << tick.frameSequence
                 << ",\n";
        manifest << "      \"submittedFrame\": " << tick.submittedFrame
                 << ",\n";
        manifest << "      \"completedFrame\": " << tick.completedFrame
                 << "\n";
        manifest << "    }"
                 << (index + 1U < request.ticks.size() ? ",\n" : "\n");
    }
    manifest << "  ],\n";
    manifest << "  \"seriesSha256\": "
             << Diagnostics::JsonString(seriesSha256) << "\n";
    manifest << "}\n";

    std::vector<std::filesystem::path> published;
    if (!WriteBytesAtomically(
            root, seriesPath, ToBytes(manifest.str()), published, outError))
    {
        return false;
    }
    outResult.seriesPath = seriesPath;
    outResult.seriesSha256 = seriesSha256;
    return true;
}
} // namespace RVX
