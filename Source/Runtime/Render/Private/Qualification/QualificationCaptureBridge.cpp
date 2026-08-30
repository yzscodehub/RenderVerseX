#include "Render/Qualification/QualificationCaptureBridge.h"

#include "Core/Diagnostics/ContentHash.h"
#include "Core/Hash/SHA256.h"
#include "Render/Qualification/RenderExecutionReceiptDigest.h"
#include "Resources/RenderSubmissionTracker.h"
#include "RenderContracts/RenderAmbientOcclusion.h"
#include "RHI/RHIBuffer.h"
#include "RHI/RHICommandContext.h"
#include "RHI/RHIDevice.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>
#include <vector>

namespace RVX
{
namespace
{
    [[nodiscard]] constexpr size_t ToIndex(
        QualificationCaptureAttachmentSemantic semantic) noexcept
    {
        return static_cast<size_t>(semantic);
    }

    [[nodiscard]] constexpr uint32 CanonicalBytesPerPixel(
        QualificationCapturePixelFormat format) noexcept
    {
        switch (format)
        {
            case QualificationCapturePixelFormat::R32Float: return 4U;
            case QualificationCapturePixelFormat::RG32Float: return 8U;
            case QualificationCapturePixelFormat::RGBA32Float: return 16U;
            case QualificationCapturePixelFormat::Unknown: break;
        }
        return 0U;
    }

    [[nodiscard]] constexpr uint32 PhysicalBytesPerPixel(
        RHIFormat format) noexcept
    {
        switch (format)
        {
            case RHIFormat::R8_UNORM: return 1U;
            case RHIFormat::RG16_FLOAT: return 4U;
            case RHIFormat::R32_FLOAT:
            case RHIFormat::D32_FLOAT:
            case RHIFormat::RGBA8_UNORM:
            case RHIFormat::RGBA8_UNORM_SRGB:
            case RHIFormat::BGRA8_UNORM:
            case RHIFormat::BGRA8_UNORM_SRGB: return 4U;
            case RHIFormat::RGBA16_FLOAT: return 8U;
            default: break;
        }
        return 0U;
    }

    [[nodiscard]] bool IsExpectedPhysicalFormat(
        QualificationCaptureAttachmentSemantic semantic,
        RHIFormat format) noexcept
    {
        switch (semantic)
        {
            case QualificationCaptureAttachmentSemantic::BeautyLDR:
                return format == RHIFormat::RGBA8_UNORM ||
                       format == RHIFormat::RGBA8_UNORM_SRGB ||
                       format == RHIFormat::BGRA8_UNORM ||
                       format == RHIFormat::BGRA8_UNORM_SRGB;
            case QualificationCaptureAttachmentSemantic::PreToneHDR:
                return format == RHIFormat::RGBA16_FLOAT;
            case QualificationCaptureAttachmentSemantic::LinearDepth:
                return format == RHIFormat::D32_FLOAT;
            case QualificationCaptureAttachmentSemantic::Normal:
                return format == RHIFormat::RGBA8_UNORM;
            case QualificationCaptureAttachmentSemantic::Velocity:
                return format == RHIFormat::RG16_FLOAT;
            case QualificationCaptureAttachmentSemantic::AmbientOcclusion:
            case QualificationCaptureAttachmentSemantic::ReactiveMask:
                return format == RHIFormat::R8_UNORM;
            case QualificationCaptureAttachmentSemantic::Count: break;
        }
        return false;
    }

    [[nodiscard]] bool ComputeRowPitch(
        RHIBackendType backend,
        uint32 width,
        uint32 bytesPerPixel,
        uint32& outRowPitch) noexcept
    {
        outRowPitch = 0;
        if (width == 0 || bytesPerPixel == 0 ||
            width > std::numeric_limits<uint32>::max() / bytesPerPixel)
        {
            return false;
        }
        const uint64 tight = static_cast<uint64>(width) * bytesPerPixel;
        const uint64 alignment = backend == RHIBackendType::DX12 ||
                                         backend == RHIBackendType::Metal
                                     ? 256U
                                     : 1U;
        const uint64 aligned = (tight + alignment - 1U) & ~(alignment - 1U);
        if (aligned == 0 || aligned > std::numeric_limits<uint32>::max())
        {
            return false;
        }
        outRowPitch = static_cast<uint32>(aligned);
        return true;
    }

    [[nodiscard]] bool ComputeByteSize(
        uint32 rowPitch,
        uint32 height,
        uint64& outSize) noexcept
    {
        outSize = 0;
        if (rowPitch == 0 || height == 0 ||
            height > std::numeric_limits<uint64>::max() / rowPitch)
        {
            return false;
        }
        outSize = static_cast<uint64>(rowPitch) * height;
        return outSize != 0;
    }

    [[nodiscard]] uint64 MakeResourceIdentity(
        const RGTextureHandle& texture) noexcept
    {
        uint64 hash = Diagnostics::RVX_DIAGNOSTICS_FNV1A64_OFFSET_BASIS;
        Diagnostics::MixContentHashValue(hash, texture.graphIdentity);
        Diagnostics::MixContentHashValue(hash, texture.recordingGeneration);
        Diagnostics::MixContentHashValue(hash, texture.index);
        return hash;
    }

    [[nodiscard]] bool ExtractExactGraphicsSubmission(
        const GPUCompletionToken& completion,
        const RenderSubmissionTracker& tracker,
        GPUCompletionPoint& outGraphicsPoint) noexcept
    {
        outGraphicsPoint = {};
        GPUCompletionToken normalized;
        if (!MergeGPUCompletionToken(normalized, completion) ||
            normalized.count == 0 ||
            tracker.Query(normalized) == GPUCompletionStatus::Lost)
        {
            return false;
        }

        for (uint8 index = 0; index < normalized.count; ++index)
        {
            const GPUCompletionPoint point = normalized.points[index];
            if (point.value == 0 ||
                point.value != tracker.GetLastSubmittedValue(point.domain))
            {
                return false;
            }
            if (point.domain == GPUQueueDomain::Graphics)
            {
                outGraphicsPoint = point;
            }
        }
        return outGraphicsPoint.value != 0;
    }

    [[nodiscard]] float32 ConvertHalfToFloat(uint16 value) noexcept
    {
        const uint32 sign = static_cast<uint32>(value & 0x8000U) << 16U;
        int32 exponent = static_cast<int32>((value >> 10U) & 0x1FU);
        uint32 mantissa = value & 0x03FFU;
        uint32 bits = 0;
        if (exponent == 0)
        {
            if (mantissa == 0U)
            {
                bits = sign;
            }
            else
            {
                // Binary16 subnormals have an exponent of -14 before their
                // leading bit is restored. Keep this signed: uint32
                // decrement underflows for the smallest subnormal (0x0001).
                exponent = -14;
                while ((mantissa & 0x0400U) == 0U)
                {
                    mantissa <<= 1U;
                    --exponent;
                }
                mantissa &= 0x03FFU;
                bits = sign | (static_cast<uint32>(exponent + 127) << 23U) |
                    (mantissa << 13U);
            }
        }
        else if (exponent == 31)
        {
            bits = sign | 0x7F800000U | (mantissa << 13U);
        }
        else
        {
            bits = sign | (static_cast<uint32>(exponent - 15 + 127) << 23U) |
                (mantissa << 13U);
        }
        float32 result = 0.0f;
        std::memcpy(&result, &bits, sizeof(result));
        return result;
    }

    [[nodiscard]] float32 ReadHalf(const uint8* source) noexcept
    {
        uint16 value = 0;
        std::memcpy(&value, source, sizeof(value));
        return ConvertHalfToFloat(value);
    }

    void StoreFloat(uint8* destination, float32 value) noexcept
    {
        std::memcpy(destination, &value, sizeof(value));
    }

    [[nodiscard]] bool ConvertPhysicalPixel(
        QualificationCaptureAttachmentSemantic semantic,
        RHIFormat format,
        const uint8* source,
        float32 nearPlane,
        float32 farPlane,
        bool reverseZ,
        uint8* destination) noexcept
    {
        constexpr float32 inverseUNorm8 = 1.0f / 255.0f;
        switch (semantic)
        {
            case QualificationCaptureAttachmentSemantic::BeautyLDR:
            {
                const bool bgra = format == RHIFormat::BGRA8_UNORM ||
                    format == RHIFormat::BGRA8_UNORM_SRGB;
                const float32 r = static_cast<float32>(source[bgra ? 2 : 0]) *
                    inverseUNorm8;
                const float32 g = static_cast<float32>(source[1]) * inverseUNorm8;
                const float32 b = static_cast<float32>(source[bgra ? 0 : 2]) *
                    inverseUNorm8;
                const float32 a = static_cast<float32>(source[3]) * inverseUNorm8;
                StoreFloat(destination + 0U, r);
                StoreFloat(destination + 4U, g);
                StoreFloat(destination + 8U, b);
                StoreFloat(destination + 12U, a);
                return true;
            }
            case QualificationCaptureAttachmentSemantic::PreToneHDR:
            {
                for (uint32 channel = 0; channel < 4U; ++channel)
                {
                    StoreFloat(destination + channel * sizeof(float32),
                               ReadHalf(source + channel * sizeof(uint16)));
                }
                return true;
            }
            case QualificationCaptureAttachmentSemantic::LinearDepth:
            {
                float32 deviceDepth = 0.0f;
                std::memcpy(&deviceDepth, source, sizeof(deviceDepth));
                if (!std::isfinite(deviceDepth))
                {
                    return false;
                }
                if (reverseZ)
                {
                    deviceDepth = 1.0f - deviceDepth;
                }
                float32 linearDepth = 0.0f;
                if (!TryLinearizeRenderAmbientOcclusionDepth(
                        deviceDepth, nearPlane, farPlane, linearDepth))
                {
                    return false;
                }
                StoreFloat(destination, linearDepth);
                return true;
            }
            case QualificationCaptureAttachmentSemantic::Normal:
            {
                // The renderer's preserved normal field uses octahedral XY
                // packing in RGBA8. Decode it before publishing the canonical
                // world-normal payload; alpha is a fixed semantic sentinel.
                float32 x = static_cast<float32>(source[0]) * inverseUNorm8 *
                    2.0f - 1.0f;
                float32 y = static_cast<float32>(source[1]) * inverseUNorm8 *
                    2.0f - 1.0f;
                float32 z = 1.0f - std::abs(x) - std::abs(y);
                if (z < 0.0f)
                {
                    const float32 oldX = x;
                    x = (1.0f - std::abs(y)) * (oldX >= 0.0f ? 1.0f : -1.0f);
                    y = (1.0f - std::abs(oldX)) * (y >= 0.0f ? 1.0f : -1.0f);
                }
                const float32 lengthSquared = x * x + y * y + z * z;
                if (!std::isfinite(lengthSquared) || lengthSquared <= 0.0f)
                {
                    return false;
                }
                const float32 inverseLength = 1.0f / std::sqrt(lengthSquared);
                StoreFloat(destination + 0U, x * inverseLength);
                StoreFloat(destination + 4U, y * inverseLength);
                StoreFloat(destination + 8U, z * inverseLength);
                StoreFloat(destination + 12U, 1.0f);
                return true;
            }
            case QualificationCaptureAttachmentSemantic::Velocity:
                StoreFloat(destination + 0U, ReadHalf(source + 0U));
                StoreFloat(destination + 4U, ReadHalf(source + sizeof(uint16)));
                return true;
            case QualificationCaptureAttachmentSemantic::AmbientOcclusion:
            case QualificationCaptureAttachmentSemantic::ReactiveMask:
                StoreFloat(destination,
                           static_cast<float32>(source[0]) * inverseUNorm8);
                return true;
            case QualificationCaptureAttachmentSemantic::Count: break;
        }
        return false;
    }

    [[nodiscard]] const char* GetCapturePassName(
        QualificationCaptureAttachmentSemantic semantic) noexcept
    {
        switch (semantic)
        {
            case QualificationCaptureAttachmentSemantic::BeautyLDR:
                return "RenderQualityCapture.BeautyLDR";
            case QualificationCaptureAttachmentSemantic::PreToneHDR:
                return "RenderQualityCapture.PreToneHDR";
            case QualificationCaptureAttachmentSemantic::LinearDepth:
                return "RenderQualityCapture.LinearDepth";
            case QualificationCaptureAttachmentSemantic::Normal:
                return "RenderQualityCapture.WorldNormal";
            case QualificationCaptureAttachmentSemantic::Velocity:
                return "RenderQualityCapture.Velocity";
            case QualificationCaptureAttachmentSemantic::AmbientOcclusion:
                return "RenderQualityCapture.AmbientOcclusion";
            case QualificationCaptureAttachmentSemantic::ReactiveMask:
                return "RenderQualityCapture.ReactiveMask";
            case QualificationCaptureAttachmentSemantic::Count: break;
        }
        return "RenderQualityCapture.Invalid";
    }

    class CompletionTrackerAdapter final
        : public IQualificationCaptureCompletionTracker
    {
    public:
        CompletionTrackerAdapter(const RenderSubmissionTracker& tracker,
                                 const GPUCompletionToken& completion,
                                 QualificationCaptureCompletionToken expected) noexcept
            : m_tracker(tracker)
            , m_completion(completion)
            , m_expected(expected)
        {
        }

        [[nodiscard]] bool IsExactCurrentSubmission(
            const QualificationCaptureCompletionToken& token) const noexcept override
        {
            GPUCompletionPoint graphics;
            return token == m_expected &&
                   ExtractExactGraphicsSubmission(m_completion, m_tracker, graphics) &&
                   graphics.value == m_expected.completionValue;
        }

        [[nodiscard]] QualificationCaptureCompletionStatus Query(
            const QualificationCaptureCompletionToken& token) const noexcept override
        {
            if (token != m_expected)
            {
                return QualificationCaptureCompletionStatus::Rejected;
            }
            switch (m_tracker.Query(m_completion))
            {
                case GPUCompletionStatus::Completed:
                    return QualificationCaptureCompletionStatus::Completed;
                case GPUCompletionStatus::Pending:
                    return QualificationCaptureCompletionStatus::Pending;
                case GPUCompletionStatus::Lost:
                    return QualificationCaptureCompletionStatus::Lost;
                case GPUCompletionStatus::CompatibilityWaitIdle:
                    // A qualification capture must be fence-completion-owned;
                    // it may not acquire validity from a compatibility wait.
                    return QualificationCaptureCompletionStatus::Rejected;
            }
            return QualificationCaptureCompletionStatus::Rejected;
        }

    private:
        const RenderSubmissionTracker& m_tracker;
        const GPUCompletionToken& m_completion;
        QualificationCaptureCompletionToken m_expected{};
    };
} // namespace

struct QualificationCaptureBridge::PhysicalReadback
{
    QualificationCaptureAttachmentDescriptor descriptor{};
    RGTextureHandle sourceTexture{};
    RHIFormat sourceFormat = RHIFormat::Unknown;
    uint32 sourceRowPitch = 0;
    // The bridge owns this until its exact Graphics token completes; the
    // per-pass CopyData alias only keeps it alive through submission recording.
    RHIBufferRef readback{};
    bool copyRecorded = false;
};

QualificationCaptureBridge::QualificationCaptureBridge() = default;
QualificationCaptureBridge::~QualificationCaptureBridge() = default;

float32 QualificationCaptureBridgeTestAccess::HalfToFloat(uint16 value) noexcept
{
    return ConvertHalfToFloat(value);
}

bool QualificationCaptureBridgeTestAccess::ConvertPixel(
    QualificationCaptureAttachmentSemantic semantic,
    RHIFormat format,
    const uint8* source,
    float32 nearPlane,
    float32 farPlane,
    bool reverseZ,
    uint8* destination) noexcept
{
    return ConvertPhysicalPixel(
        semantic, format, source, nearPlane, farPlane, reverseZ, destination);
}

uint32 QualificationCaptureBridgeTestAccess::SourceRowForCanonicalTopLeft(
    uint32 canonicalTopLeftRow,
    uint32 height,
    bool originBottomLeft) noexcept
{
    if (height == 0 || canonicalTopLeftRow >= height)
    {
        return RVX_INVALID_INDEX;
    }
    return originBottomLeft ? height - 1U - canonicalTopLeftRow
                            : canonicalTopLeftRow;
}

bool QualificationCaptureBridge::Arm() noexcept
{
    if (m_armRequested || m_owner.HasPendingCompletion() ||
        m_owner.GetState() == QualificationCaptureState::Armed ||
        m_owner.GetState() == QualificationCaptureState::Recorded)
    {
        return false;
    }
    if (m_owner.GetState() == QualificationCaptureState::Completed)
    {
        m_owner.ResetCompletedBundle();
    }
    m_readbacks = {};
    m_recordingIdentity = {};
    m_submissionToken.reset();
    m_recordingNearPlane = 0.0f;
    m_recordingFarPlane = 0.0f;
    m_recordingOriginBottomLeft = false;
    m_recordingReverseZ = false;
    m_copyExecutionFailed = false;
    m_armRequested = true;
    return true;
}

bool QualificationCaptureBridge::Record(
    RenderGraph& graph,
    IRHIDevice& device,
    const QualificationCaptureGraphInputs& inputs)
{
    if (!m_armRequested)
    {
        return true;
    }
    m_armRequested = false;
    if (!inputs.provenance.IsValid() || !inputs.frameIdentity.IsValid() ||
        !inputs.viewIdentity.IsValid() || !inputs.perspectiveProjection ||
        !std::isfinite(inputs.nearPlane) ||
        !std::isfinite(inputs.farPlane) || inputs.nearPlane <= 0.0f ||
        inputs.farPlane <= inputs.nearPlane || graph.GetGraphIdentity() == 0 ||
        graph.GetRecordingGeneration() == 0 ||
        inputs.provenance.graphIdentity != graph.GetGraphIdentity() ||
        inputs.provenance.recordingGeneration !=
            graph.GetRecordingGeneration() ||
        inputs.provenance.frameSequence != inputs.frameIdentity.frameSequence)
    {
        return RejectRecording();
    }

    QualificationCaptureBundleIdentity identity;
    ++m_nextBundleId;
    if (m_nextBundleId == 0)
    {
        ++m_nextBundleId;
    }
    identity.bundleId = m_nextBundleId;
    identity.frameIdentity = inputs.frameIdentity;
    identity.viewIdentity = inputs.viewIdentity;
    identity.graphProvenance = {
        graph.GetGraphIdentity(), graph.GetRecordingGeneration(),
        inputs.provenance.recordEpoch, {}};
    if (!m_owner.Arm(identity))
    {
        return false;
    }
    m_recordingIdentity = identity;
    m_recordingNearPlane = inputs.nearPlane;
    m_recordingFarPlane = inputs.farPlane;
    m_recordingOriginBottomLeft = inputs.originBottomLeft;
    m_recordingReverseZ = inputs.reverseZ;
    m_readbacks = {};
    m_copyExecutionFailed = false;

    std::array<QualificationCaptureAttachmentDescriptor,
               RVX_QUALIFICATION_CAPTURE_ATTACHMENT_COUNT> descriptors{};
    for (const QualificationCaptureGraphSource& source : inputs.sources)
    {
        const size_t index = ToIndex(source.semantic);
        const QualificationCaptureAttachmentContract contract =
            GetQualificationCaptureAttachmentContract(source.semantic);
        const RHITextureDesc* const textureDesc =
            source.texture.IsValid() ? graph.GetTextureDesc(source.texture) : nullptr;
        if (index >= m_readbacks.size() || !contract.IsValid() ||
            !source.producer.IsValid() || textureDesc == nullptr ||
            !source.metadata.extent.IsValid() || source.metadata.format !=
                textureDesc->format || source.metadata.extent.width !=
                textureDesc->width || source.metadata.extent.height !=
                textureDesc->height ||
            source.metadata.sampleCount != RHISampleCount::Count1 ||
            textureDesc->sampleCount != RHISampleCount::Count1 ||
            textureDesc->dimension != RHITextureDimension::Texture2D ||
            textureDesc->depth != 1U || textureDesc->arraySize != 1U ||
            textureDesc->mipLevels != 1U ||
            !IsExpectedPhysicalFormat(source.semantic, textureDesc->format) ||
            m_readbacks[index] != nullptr)
        {
            return RejectRecording();
        }

        const uint32 sourceBytesPerPixel =
            PhysicalBytesPerPixel(textureDesc->format);
        const uint32 canonicalBytesPerPixel =
            CanonicalBytesPerPixel(contract.format);
        uint32 sourceRowPitch = 0;
        uint32 canonicalRowPitch = 0;
        uint64 sourceByteSize = 0;
        if (sourceBytesPerPixel == 0 || canonicalBytesPerPixel == 0 ||
            !ComputeRowPitch(device.GetBackendType(), textureDesc->width,
                             sourceBytesPerPixel, sourceRowPitch) ||
            !ComputeRowPitch(RHIBackendType::None, textureDesc->width,
                             canonicalBytesPerPixel, canonicalRowPitch) ||
            !ComputeByteSize(sourceRowPitch, textureDesc->height, sourceByteSize))
        {
            return RejectRecording();
        }

        RHIBufferDesc readbackDesc;
        readbackDesc.size = sourceByteSize;
        readbackDesc.usage = RHIBufferUsage::CopyDst;
        readbackDesc.memoryType = RHIMemoryType::Readback;
        readbackDesc.debugName = "RenderQualityQualificationReadback";
        RHIBufferRef readback;
        try
        {
            readback = device.CreateBuffer(readbackDesc);
        }
        catch (...)
        {
            return RejectRecording();
        }
        if (!readback || readback->GetSize() != sourceByteSize)
        {
            return RejectRecording();
        }

        auto physical = std::make_unique<PhysicalReadback>();
        physical->sourceFormat = textureDesc->format;
        physical->sourceTexture = source.texture;
        physical->sourceRowPitch = sourceRowPitch;
        physical->readback = std::move(readback);
        physical->descriptor.contract = contract;
        physical->descriptor.extent = source.metadata.extent;
        physical->descriptor.rowPitchBytes = canonicalRowPitch;
        physical->descriptor.provenance = {
            identity.graphProvenance.graphIdentity,
            identity.graphProvenance.graphRecordingGeneration,
            identity.graphProvenance.graphRecordEpoch,
            MakeResourceIdentity(source.texture),
            source.texture.recordingGeneration,
            source.producer.value};
        if (!physical->descriptor.IsValid())
        {
            return RejectRecording();
        }
        descriptors[index] = physical->descriptor;
        m_readbacks[index] = std::move(physical);
    }

    for (size_t index = 0; index < m_readbacks.size(); ++index)
    {
        PhysicalReadback* const physical = m_readbacks[index].get();
        if (physical == nullptr)
        {
            return RejectRecording();
        }
        const RGBufferHandle readback = graph.ImportBuffer(
            RHIBufferRef(physical->readback), RHIResourceState::CopyDest);
        if (!readback.IsValid())
        {
            return RejectRecording();
        }

        struct CopyData
        {
            RGTextureHandle source{};
            RGBufferHandle readback{};
            // The submitted graph retains this alias in its exact batch.
            RHIBufferRef readbackOwner{};
            QualificationCaptureBridge* bridge = nullptr;
            uint32 sourceIndex = RVX_INVALID_INDEX;
            uint32 width = 0;
            uint32 height = 0;
            uint32 rowPitch = 0;
        };
        graph.AddPass<CopyData>(
            GetCapturePassName(physical->descriptor.contract.semantic),
            RenderGraphPassType::Copy,
            RenderGraphQueueAffinity::Graphics,
            [sourceTexture = physical->sourceTexture, readback,
             owner = RHIBufferRef(physical->readback), this,
             sourceIndex = static_cast<uint32>(index),
             width = physical->descriptor.extent.width,
             height = physical->descriptor.extent.height,
             rowPitch = physical->sourceRowPitch](
                RenderGraphBuilder& builder, CopyData& data)
            {
                data.source = builder.Read(
                    sourceTexture,
                    MakeRGAccessDesc(RHIResourceState::CopySource,
                                     RHIShaderStage::All));
                data.readback = builder.Write(
                    readback,
                    MakeRGAccessDesc(RHIResourceState::CopyDest,
                                     RHIShaderStage::All));
                data.readbackOwner = owner;
                data.bridge = this;
                data.sourceIndex = sourceIndex;
                data.width = width;
                data.height = height;
                data.rowPitch = rowPitch;
            },
            [](const CopyData& data, RenderGraphPassContext& context)
            {
                RHITexture* const sourceTexture = context.GetTexture(data.source);
                RHIBuffer* const readback = context.GetBuffer(data.readback);
                if (data.bridge == nullptr || sourceTexture == nullptr ||
                    readback == nullptr || !data.readbackOwner ||
                    readback != data.readbackOwner.Get() ||
                    !context.RetainSubmissionResource(
                        Ref<RefCounted>(data.readbackOwner)))
                {
                    if (data.bridge != nullptr)
                    {
                        data.bridge->m_copyExecutionFailed = true;
                    }
                    context.MarkExecutionFailed(
                        "RenderQuality qualification readback resource is unavailable");
                    return;
                }
                RHIBufferTextureCopyDesc copyDesc;
                copyDesc.bufferRowPitch = data.rowPitch;
                copyDesc.bufferImageHeight = data.height;
                copyDesc.textureRegion = {0, 0, data.width, data.height};
                context.Commands().CopyTextureToBuffer(
                    sourceTexture, readback, copyDesc);
                if (data.sourceIndex >= data.bridge->m_readbacks.size() ||
                    data.bridge->m_readbacks[data.sourceIndex] == nullptr)
                {
                    data.bridge->m_copyExecutionFailed = true;
                    context.MarkExecutionFailed(
                        "RenderQuality qualification capture source identity is unavailable");
                    return;
                }
                data.bridge->m_readbacks[data.sourceIndex]->copyRecorded = true;
            });
    }

    if (!m_owner.BindRecordedAttachments(identity, descriptors))
    {
        return false;
    }
    return true;
}

bool QualificationCaptureBridge::FinalizeExecutionReceipt(
    const RenderExecutionReceipt& receipt) noexcept
{
    if (m_owner.GetState() != QualificationCaptureState::Recorded)
    {
        return false;
    }
    const bool copiesRecorded = std::all_of(
        m_readbacks.begin(), m_readbacks.end(),
        [](const std::unique_ptr<PhysicalReadback>& readback)
        {
            return readback != nullptr && readback->copyRecorded;
        });
    if (m_copyExecutionFailed || !copiesRecorded || !receipt.executionSucceeded ||
        !receipt.IsStructurallyValid() ||
        receipt.frameIdentity != m_recordingIdentity.frameIdentity ||
        receipt.viewIdentity != m_recordingIdentity.viewIdentity)
    {
        return RejectRecording();
    }
    if (!m_owner.ConfirmExecutionReceipt())
    {
        return RejectRecording();
    }

    return true;
}

bool QualificationCaptureBridge::FinalizePresentedReceipt(
    const RenderExecutionReceipt& receipt) noexcept
{
    if (m_owner.GetState() != QualificationCaptureState::PendingCompletion ||
        !receipt.presented || !receipt.succeeded ||
        !receipt.IsStructurallyValid() ||
        receipt.frameIdentity != m_recordingIdentity.frameIdentity ||
        receipt.viewIdentity != m_recordingIdentity.viewIdentity)
    {
        return false;
    }
    const Hash::SHA256Digest receiptDigest =
        ComputeRenderExecutionReceiptDigest(receipt);
    if (!IsRenderExecutionReceiptDigestValid(receiptDigest) ||
        !m_owner.FinalizePresentedReceipt(receiptDigest))
    {
        return false;
    }

    m_recordingIdentity.graphProvenance.executionReceiptDigest = receiptDigest;
    return true;
}

bool QualificationCaptureBridge::NotifySubmission(
    const GPUCompletionToken& completion,
    const RenderSubmissionTracker& tracker) noexcept
{
    if (m_owner.GetState() != QualificationCaptureState::Recorded ||
        !m_recordingIdentity.IsRecordable())
    {
        return false;
    }
    GPUCompletionPoint graphics;
    if (!ExtractExactGraphicsSubmission(completion, tracker, graphics))
    {
        return RejectRecording();
    }
    try
    {
        m_submissionToken = std::make_unique<GPUCompletionToken>(completion);
    }
    catch (...)
    {
        return RejectRecording();
    }
    const QualificationCaptureCompletionToken ownerToken{
        m_recordingIdentity.bundleId, graphics.value};
    const CompletionTrackerAdapter adapter(tracker, *m_submissionToken,
                                           ownerToken);
    return m_owner.NotifySubmission(
        m_recordingIdentity, ownerToken, adapter);
}

bool QualificationCaptureBridge::PollCompletion(
    const RenderSubmissionTracker& tracker)
{
    if (!m_owner.HasPendingCompletion())
    {
        return m_owner.GetState() == QualificationCaptureState::Completed;
    }
    if (!m_submissionToken || !m_recordingIdentity.IsRecordable())
    {
        return RejectRecording();
    }
    const QualificationCaptureCompletionToken ownerToken{
        m_recordingIdentity.bundleId,
        m_owner.GetDiagnostics().completion.completionValue};
    const CompletionTrackerAdapter adapter(tracker, *m_submissionToken,
                                           ownerToken);

    class Mapper final : public IQualificationCaptureReadbackMapper
    {
    public:
        explicit Mapper(QualificationCaptureBridge& bridge) noexcept
            : m_bridge(bridge)
        {
        }

        [[nodiscard]] bool Map(
            const QualificationCaptureBundleIdentity& identity,
            const QualificationCaptureAttachmentDescriptor& descriptor,
            QualificationCaptureMappedPayload& outPayload) override
        {
            return m_bridge.Map(identity, descriptor, outPayload);
        }

    private:
        QualificationCaptureBridge& m_bridge;
    } mapper(*this);

    const bool completed = m_owner.PollCompletion(adapter, mapper);
    if (!m_owner.HasPendingCompletion())
    {
        m_submissionToken.reset();
        m_readbacks = {};
    }
    return completed;
}

void QualificationCaptureBridge::ReleaseUnsubmitted() noexcept
{
    if (m_recordingIdentity.IsRecordable())
    {
        m_owner.ReleaseUnsubmitted(m_recordingIdentity);
    }
    m_armRequested = false;
    m_submissionToken.reset();
    m_recordingNearPlane = 0.0f;
    m_recordingFarPlane = 0.0f;
    m_recordingOriginBottomLeft = false;
    m_recordingReverseZ = false;
    m_readbacks = {};
}

void QualificationCaptureBridge::RejectPending() noexcept
{
    if (m_owner.GetState() == QualificationCaptureState::PendingCompletion)
    {
        m_owner.RejectPendingCompletion();
        return;
    }
    if (m_armRequested && m_owner.GetState() == QualificationCaptureState::Idle)
    {
        // Route a missing graph source through the owner so public diagnostics
        // remain fail-closed instead of silently returning to Idle.
        static_cast<void>(m_owner.Arm({}));
    }
    ReleaseUnsubmitted();
}

bool QualificationCaptureBridge::HasPendingCompletion() const noexcept
{
    return m_owner.HasPendingCompletion();
}

bool QualificationCaptureBridge::IsAwaitingGraphRecord() const noexcept
{
    return m_armRequested;
}

const QualificationCaptureDiagnostics&
QualificationCaptureBridge::GetDiagnostics() const noexcept
{
    return m_owner.GetDiagnostics();
}

std::shared_ptr<const QualificationCaptureBundle>
QualificationCaptureBridge::GetCompletedBundle() const noexcept
{
    return m_owner.GetCompletedBundle();
}

bool QualificationCaptureBridge::Map(
    const QualificationCaptureBundleIdentity& identity,
    const QualificationCaptureAttachmentDescriptor& descriptor,
    QualificationCaptureMappedPayload& outPayload)
{
    const size_t index = ToIndex(descriptor.contract.semantic);
    if (index >= m_readbacks.size() || m_readbacks[index] == nullptr ||
        m_readbacks[index]->descriptor != descriptor || !descriptor.IsValid())
    {
        return false;
    }
    const PhysicalReadback& source = *m_readbacks[index];
    const uint32 sourceBytesPerPixel = PhysicalBytesPerPixel(source.sourceFormat);
    const uint32 canonicalBytesPerPixel =
        CanonicalBytesPerPixel(descriptor.contract.format);
    uint64 canonicalByteSize = 0;
    if (!source.readback || sourceBytesPerPixel == 0 ||
        canonicalBytesPerPixel == 0 || !source.copyRecorded ||
        !ComputeByteSize(descriptor.rowPitchBytes, descriptor.extent.height,
                         canonicalByteSize) ||
        canonicalByteSize > std::numeric_limits<size_t>::max() ||
        source.readback->GetSize() <
            static_cast<uint64>(source.sourceRowPitch) * descriptor.extent.height)
    {
        return false;
    }

    void* const mapped = source.readback->Map();
    if (mapped == nullptr)
    {
        return false;
    }
    std::vector<uint8> bytes;
    bool converted = false;
    try
    {
        bytes.resize(static_cast<size_t>(canonicalByteSize));
        const uint8* const sourceBytes = static_cast<const uint8*>(mapped);
        for (uint32 y = 0; y < descriptor.extent.height; ++y)
        {
            const uint32 sourceY =
                QualificationCaptureBridgeTestAccess::SourceRowForCanonicalTopLeft(
                    y, descriptor.extent.height, m_recordingOriginBottomLeft);
            const uint8* const sourceRow = sourceBytes +
                static_cast<uint64>(sourceY) * source.sourceRowPitch;
            uint8* const destinationRow = bytes.data() +
                static_cast<uint64>(y) * descriptor.rowPitchBytes;
            for (uint32 x = 0; x < descriptor.extent.width; ++x)
            {
                if (!ConvertPhysicalPixel(
                        descriptor.contract.semantic, source.sourceFormat,
                        sourceRow + static_cast<uint64>(x) * sourceBytesPerPixel,
                        m_recordingNearPlane,
                        m_recordingFarPlane,
                        m_recordingReverseZ,
                        destinationRow +
                            static_cast<uint64>(x) * canonicalBytesPerPixel))
                {
                    source.readback->Unmap();
                    return false;
                }
            }
        }
        converted = true;
    }
    catch (...)
    {
        converted = false;
    }
    source.readback->Unmap();
    if (!converted)
    {
        return false;
    }

    outPayload.identity = identity;
    outPayload.semantic = descriptor.contract.semantic;
    outPayload.provenance = descriptor.provenance;
    outPayload.claimedPayloadHash = Hash::ComputeSHA256(bytes.data(), bytes.size());
    outPayload.bytes = std::move(bytes);
    return true;
}

bool QualificationCaptureBridge::RejectRecording() noexcept
{
    if (m_owner.GetState() == QualificationCaptureState::Idle)
    {
        // Record can fail before a valid graph identity exists. Route that
        // failure through the owner with its intentionally invalid identity
        // so host diagnostics observe a terminal fail-closed state.
        static_cast<void>(m_owner.Arm({}));
    }
    else if (m_owner.GetState() == QualificationCaptureState::Armed)
    {
        const std::array<QualificationCaptureAttachmentDescriptor, 0> empty{};
        static_cast<void>(m_owner.BindRecordedAttachments(
            m_recordingIdentity, empty));
    }
    else if (m_owner.GetState() == QualificationCaptureState::Recorded)
    {
        m_owner.ReleaseUnsubmitted(m_recordingIdentity);
    }
    m_armRequested = false;
    m_submissionToken.reset();
    m_recordingNearPlane = 0.0f;
    m_recordingFarPlane = 0.0f;
    m_recordingOriginBottomLeft = false;
    m_recordingReverseZ = false;
    m_readbacks = {};
    return false;
}
} // namespace RVX
