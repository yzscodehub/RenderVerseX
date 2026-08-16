#include "RenderContracts/ResourceUploadRequest.h"

#include "ResourceUploadRequestInternal.h"

#include <cmath>
#include <limits>
#include <type_traits>
#include <utility>

namespace RVX
{
    std::span<const uint8> GetUploadPayloadBytes(
        const MeshUploadPayload& payload) noexcept
    {
        return std::span<const uint8>(payload.bytes);
    }

    std::span<const uint8> GetUploadPayloadBytes(
        const TextureUploadPayload& payload) noexcept
    {
        return std::span<const uint8>(payload.bytes);
    }

namespace
{
    bool IsFinite(float32 value)
    {
        return std::isfinite(value);
    }

    bool IsFinite(const Vec2& value)
    {
        return IsFinite(value.x) && IsFinite(value.y);
    }

    bool IsFinite(const Vec3& value)
    {
        return IsFinite(value.x) && IsFinite(value.y) && IsFinite(value.z);
    }

    bool IsFinite(const Vec4& value)
    {
        return IsFinite(value.x) && IsFinite(value.y) &&
               IsFinite(value.z) && IsFinite(value.w);
    }

    bool IsUnitRange(float32 value)
    {
        return IsFinite(value) && value >= 0.0f && value <= 1.0f;
    }

    bool IsUnitRange(const Vec3& value)
    {
        return IsUnitRange(value.x) && IsUnitRange(value.y) &&
               IsUnitRange(value.z);
    }

    bool IsUnitRange(const Vec4& value)
    {
        return IsUnitRange(value.x) && IsUnitRange(value.y) &&
               IsUnitRange(value.z) && IsUnitRange(value.w);
    }

    bool IsDeclared(RenderResourceKind value)
    {
        switch (value)
        {
            case RenderResourceKind::Mesh:
            case RenderResourceKind::Texture:
            case RenderResourceKind::Material: return true;
            case RenderResourceKind::Invalid: return false;
        }
        return false;
    }

    bool IsDeclared(RenderDependencyReadiness value)
    {
        return value == RenderDependencyReadiness::RequireAll ||
               value == RenderDependencyReadiness::AllowFallback;
    }

    bool IsDeclared(RenderUploadPriority value)
    {
        return value == RenderUploadPriority::Low ||
               value == RenderUploadPriority::Normal ||
               value == RenderUploadPriority::High;
    }

    bool IsDeclared(MeshUploadIndexType value)
    {
        return value == MeshUploadIndexType::UInt8 ||
               value == MeshUploadIndexType::UInt16 ||
               value == MeshUploadIndexType::UInt32;
    }

    bool IsDeclared(MeshUploadPrimitiveTopology value)
    {
        switch (value)
        {
            case MeshUploadPrimitiveTopology::Triangles:
            case MeshUploadPrimitiveTopology::TriangleStrip:
            case MeshUploadPrimitiveTopology::TriangleFan:
            case MeshUploadPrimitiveTopology::Lines:
            case MeshUploadPrimitiveTopology::LineStrip:
            case MeshUploadPrimitiveTopology::LineLoop:
            case MeshUploadPrimitiveTopology::Points: return true;
        }
        return false;
    }

    bool IsKnown(TextureUploadFormat value)
    {
        switch (value)
        {
            case TextureUploadFormat::RGBA8:
            case TextureUploadFormat::RGBA16F:
            case TextureUploadFormat::RGBA32F:
            case TextureUploadFormat::RGB8:
            case TextureUploadFormat::RG8:
            case TextureUploadFormat::R8:
            case TextureUploadFormat::BC1:
            case TextureUploadFormat::BC3:
            case TextureUploadFormat::BC5:
            case TextureUploadFormat::BC7: return true;
            case TextureUploadFormat::Unknown: return false;
        }
        return false;
    }

    bool IsDeclared(MaterialUploadTextureSlot value)
    {
        switch (value)
        {
            case MaterialUploadTextureSlot::BaseColor:
            case MaterialUploadTextureSlot::Normal:
            case MaterialUploadTextureSlot::MetallicRoughness:
            case MaterialUploadTextureSlot::Occlusion:
            case MaterialUploadTextureSlot::Emissive: return true;
        }
        return false;
    }

    bool IsDeclared(MaterialUploadWrapMode value)
    {
        switch (value)
        {
            case MaterialUploadWrapMode::Repeat:
            case MaterialUploadWrapMode::MirrorRepeat:
            case MaterialUploadWrapMode::ClampToEdge:
            case MaterialUploadWrapMode::ClampToBorder: return true;
        }
        return false;
    }

    bool IsDeclared(MaterialUploadFilterMode value)
    {
        switch (value)
        {
            case MaterialUploadFilterMode::Nearest:
            case MaterialUploadFilterMode::Linear:
            case MaterialUploadFilterMode::NearestMipmapNearest:
            case MaterialUploadFilterMode::LinearMipmapNearest:
            case MaterialUploadFilterMode::NearestMipmapLinear:
            case MaterialUploadFilterMode::LinearMipmapLinear: return true;
        }
        return false;
    }

    bool IsDeclared(MaterialSourceAlphaMode value)
    {
        return value == MaterialSourceAlphaMode::Opaque ||
               value == MaterialSourceAlphaMode::Mask ||
               value == MaterialSourceAlphaMode::Blend;
    }

    bool IsDeclared(MaterialSourceWorkflow value)
    {
        return value == MaterialSourceWorkflow::MetallicRoughness ||
               value == MaterialSourceWorkflow::SpecularGlossiness ||
               value == MaterialSourceWorkflow::Unlit;
    }

    bool IsCanonicalEmpty(const UploadByteRange& range)
    {
        return range.offset == 0 && range.size == 0 && range.stride == 0;
    }

    bool IsNonempty(const UploadByteRange& range)
    {
        return range.size != 0;
    }

    template<typename TPayload>
    ResourceUploadRequestCreateCode ValidateBytes(
        const TPayload& payload)
    {
        if (GetUploadPayloadBytes(payload).empty())
        {
            return ResourceUploadRequestCreateCode::InvalidPayload;
        }
        return ResourceUploadRequestCreateCode::Created;
    }

    ResourceUploadRequestCreateCode ValidateRange(
        const UploadByteRange& range,
        uint64 ownerSize,
        bool requireStride,
        uint64 elementCount = 0)
    {
        if (!IsNonempty(range))
        {
            return IsCanonicalEmpty(range)
                ? ResourceUploadRequestCreateCode::Created
                : ResourceUploadRequestCreateCode::InvalidPayload;
        }
        if (requireStride && range.stride == 0)
        {
            return ResourceUploadRequestCreateCode::InvalidPayload;
        }
        if (range.offset > std::numeric_limits<uint64>::max() - range.size)
        {
            return ResourceUploadRequestCreateCode::PayloadRangeOverflow;
        }
        if (range.offset + range.size > ownerSize)
        {
            return ResourceUploadRequestCreateCode::InvalidPayload;
        }
        if (elementCount != 0)
        {
            if (range.stride != 0 &&
                elementCount > std::numeric_limits<uint64>::max() / range.stride)
            {
                return ResourceUploadRequestCreateCode::PayloadRangeOverflow;
            }
            if (elementCount * range.stride > range.size)
            {
                return ResourceUploadRequestCreateCode::InvalidPayload;
            }
        }
        return ResourceUploadRequestCreateCode::Created;
    }

    ResourceUploadRequestCreateCode ValidateMeshShape(
        const MeshUploadPayload& payload)
    {
        const MeshUploadCreateInfo& createInfo = payload.createInfo;
        const ResourceUploadRequestCreateCode bytesCode =
            ValidateBytes(payload);
        if (bytesCode != ResourceUploadRequestCreateCode::Created)
        {
            return bytesCode;
        }
        if (!IsDeclared(createInfo.indexType) ||
            !IsDeclared(createInfo.topology) ||
            createInfo.vertexCount == 0 || !IsFinite(createInfo.boundsMin) ||
            !IsFinite(createInfo.boundsMax) ||
            createInfo.boundsMin.x > createInfo.boundsMax.x ||
            createInfo.boundsMin.y > createInfo.boundsMax.y ||
            createInfo.boundsMin.z > createInfo.boundsMax.z ||
            !IsNonempty(payload.positionRange) ||
            payload.positionRange.stride == 0)
        {
            return ResourceUploadRequestCreateCode::InvalidPayload;
        }

        const UploadByteRange* optionalRanges[] = {
            &payload.normalRange,
            &payload.uvRange,
            &payload.tangentRange,
            &payload.boneIndexRange,
            &payload.boneWeightRange};
        for (const UploadByteRange* range : optionalRanges)
        {
            if ((!IsNonempty(*range) && !IsCanonicalEmpty(*range)) ||
                (IsNonempty(*range) && range->stride == 0))
            {
                return ResourceUploadRequestCreateCode::InvalidPayload;
            }
        }

        if (IsNonempty(payload.boneIndexRange) !=
            IsNonempty(payload.boneWeightRange))
        {
            return ResourceUploadRequestCreateCode::InvalidPayload;
        }

        if (createInfo.indexCount == 0)
        {
            if (!IsCanonicalEmpty(payload.indexRange) || !payload.submeshes.empty())
            {
                return ResourceUploadRequestCreateCode::InvalidPayload;
            }
        }
        else
        {
            const uint32 expectedStride =
                createInfo.indexType == MeshUploadIndexType::UInt8 ? 1U :
                createInfo.indexType == MeshUploadIndexType::UInt16 ? 2U : 4U;
            if (!IsNonempty(payload.indexRange) ||
                payload.indexRange.stride != expectedStride)
            {
                return ResourceUploadRequestCreateCode::InvalidPayload;
            }
            for (const MeshUploadSubmesh& submesh : payload.submeshes)
            {
                if (submesh.indexCount == 0 || !IsDeclared(submesh.topology))
                {
                    return ResourceUploadRequestCreateCode::InvalidPayload;
                }
            }
        }
        return ResourceUploadRequestCreateCode::Created;
    }

    ResourceUploadRequestCreateCode ValidateMeshRanges(
        const MeshUploadPayload& payload)
    {
        const uint64 byteCount = static_cast<uint64>(
            GetUploadPayloadBytes(payload).size());
        ResourceUploadRequestCreateCode code = ValidateRange(
            payload.positionRange, byteCount, true,
            payload.createInfo.vertexCount);
        if (code != ResourceUploadRequestCreateCode::Created)
        {
            return code;
        }

        const UploadByteRange* optionalRanges[] = {
            &payload.normalRange,
            &payload.uvRange,
            &payload.tangentRange,
            &payload.boneIndexRange,
            &payload.boneWeightRange};
        for (const UploadByteRange* range : optionalRanges)
        {
            code = ValidateRange(*range, byteCount, true,
                                 payload.createInfo.vertexCount);
            if (code != ResourceUploadRequestCreateCode::Created)
            {
                return code;
            }
        }

        if (payload.createInfo.indexCount != 0)
        {
            code = ValidateRange(payload.indexRange, byteCount, true,
                                 payload.createInfo.indexCount);
            if (code != ResourceUploadRequestCreateCode::Created)
            {
                return code;
            }
            for (const MeshUploadSubmesh& submesh : payload.submeshes)
            {
                if (submesh.indexOffset >
                    std::numeric_limits<uint32>::max() - submesh.indexCount)
                {
                    return ResourceUploadRequestCreateCode::PayloadRangeOverflow;
                }
                if (static_cast<uint64>(submesh.indexOffset) +
                        static_cast<uint64>(submesh.indexCount) >
                    payload.createInfo.indexCount)
                {
                    return ResourceUploadRequestCreateCode::InvalidPayload;
                }
            }
        }
        return ResourceUploadRequestCreateCode::Created;
    }

    ResourceUploadRequestCreateCode ValidateTextureShape(
        const TextureUploadPayload& payload)
    {
        const TextureUploadCreateInfo& createInfo = payload.createInfo;
        const ResourceUploadRequestCreateCode bytesCode =
            ValidateBytes(payload);
        if (bytesCode != ResourceUploadRequestCreateCode::Created)
        {
            return bytesCode;
        }
        if (createInfo.width == 0 || createInfo.height == 0 ||
            createInfo.depth == 0 || createInfo.mipLevels == 0 ||
            createInfo.arrayLayers == 0 || !IsKnown(createInfo.format) ||
            payload.subresources.empty())
        {
            return ResourceUploadRequestCreateCode::InvalidPayload;
        }
        if (createInfo.isCubemap &&
            (createInfo.depth != 1 || createInfo.arrayLayers < 6 ||
             createInfo.arrayLayers % 6 != 0))
        {
            return ResourceUploadRequestCreateCode::InvalidPayload;
        }
        if (createInfo.isArray && createInfo.arrayLayers <= 1)
        {
            return ResourceUploadRequestCreateCode::InvalidPayload;
        }
        if (!createInfo.isCubemap && !createInfo.isArray &&
            createInfo.arrayLayers != 1)
        {
            return ResourceUploadRequestCreateCode::InvalidPayload;
        }
        return ResourceUploadRequestCreateCode::Created;
    }

    ResourceUploadRequestCreateCode ValidateTextureRanges(
        const TextureUploadPayload& payload)
    {
        const TextureUploadCreateInfo& createInfo = payload.createInfo;
        if (static_cast<uint64>(createInfo.mipLevels) >
            std::numeric_limits<uint32>::max() /
                static_cast<uint64>(createInfo.arrayLayers))
        {
            return ResourceUploadRequestCreateCode::PayloadRangeOverflow;
        }
        const uint64 expectedCount =
            static_cast<uint64>(createInfo.mipLevels) * createInfo.arrayLayers;
        if (expectedCount != static_cast<uint64>(payload.subresources.size()))
        {
            return ResourceUploadRequestCreateCode::InvalidPayload;
        }

        const uint64 byteCount = static_cast<uint64>(
            GetUploadPayloadBytes(payload).size());
        for (size_t index = 0; index < payload.subresources.size(); ++index)
        {
            const TextureUploadSubresource& subresource =
                payload.subresources[index];
            if (subresource.mipLevel >= createInfo.mipLevels ||
                subresource.arrayLayer >= createInfo.arrayLayers ||
                !IsNonempty(subresource.bytes) || subresource.bytes.stride != 0 ||
                subresource.rowPitch == 0 ||
                subresource.slicePitch < subresource.rowPitch)
            {
                return ResourceUploadRequestCreateCode::InvalidPayload;
            }
            const ResourceUploadRequestCreateCode rangeCode =
                ValidateRange(subresource.bytes, byteCount, false);
            if (rangeCode != ResourceUploadRequestCreateCode::Created)
            {
                return rangeCode;
            }
            if (subresource.bytes.size < subresource.slicePitch)
            {
                return ResourceUploadRequestCreateCode::InvalidPayload;
            }
            for (size_t previous = 0; previous < index; ++previous)
            {
                const TextureUploadSubresource& other =
                    payload.subresources[previous];
                if (other.mipLevel == subresource.mipLevel &&
                    other.arrayLayer == subresource.arrayLayer)
                {
                    return ResourceUploadRequestCreateCode::InvalidPayload;
                }
            }
        }
        return ResourceUploadRequestCreateCode::Created;
    }

    ResourceUploadRequestCreateCode ValidateMaterialShape(
        const MaterialUploadPayload& payload)
    {
        const MaterialSourceData& source = payload.sourceData;
        if (!IsUnitRange(source.baseColorFactor) ||
            !IsUnitRange(source.metallicFactor) ||
            !IsUnitRange(source.roughnessFactor) ||
            !IsFinite(source.normalScale) || source.normalScale < 0.0f ||
            !IsUnitRange(source.occlusionStrength) ||
            !IsUnitRange(source.emissiveColor) ||
            !IsFinite(source.emissiveStrength) ||
            source.emissiveStrength < 0.0f ||
            !IsDeclared(source.alphaMode) ||
            !IsUnitRange(source.alphaCutoff) ||
            !IsDeclared(source.workflow))
        {
            return ResourceUploadRequestCreateCode::InvalidPayload;
        }

        for (size_t index = 0; index < payload.textureBindings.size(); ++index)
        {
            const MaterialUploadTextureBinding& binding =
                payload.textureBindings[index];
            if (!IsDeclared(binding.slot) || !binding.texture.IsValid() ||
                binding.uvSet < 0 || !IsFinite(binding.offset) ||
                !IsFinite(binding.scale) || binding.scale.x == 0.0f ||
                binding.scale.y == 0.0f || !IsFinite(binding.rotation) ||
                !IsDeclared(binding.wrapS) || !IsDeclared(binding.wrapT) ||
                !IsDeclared(binding.minFilter) || !IsDeclared(binding.magFilter) ||
                (binding.magFilter != MaterialUploadFilterMode::Nearest &&
                 binding.magFilter != MaterialUploadFilterMode::Linear))
            {
                return ResourceUploadRequestCreateCode::InvalidPayload;
            }
            for (size_t previous = 0; previous < index; ++previous)
            {
                if (payload.textureBindings[previous].slot == binding.slot)
                {
                    return ResourceUploadRequestCreateCode::InvalidPayload;
                }
            }
        }
        return ResourceUploadRequestCreateCode::Created;
    }

    ResourceUploadRequestCreateCode ValidatePayloadShape(
        const ResourceUploadRequestCreateInfo& info)
    {
        if (!IsDeclared(info.dependencyReadiness) || !IsDeclared(info.priority))
        {
            return ResourceUploadRequestCreateCode::InvalidPayload;
        }
        return std::visit(
            [](const auto& payload)
            {
                using PayloadType = std::decay_t<decltype(payload)>;
                if constexpr (std::is_same_v<PayloadType, MeshUploadPayload>)
                {
                    return ValidateMeshShape(payload);
                }
                else if constexpr (std::is_same_v<PayloadType, TextureUploadPayload>)
                {
                    return ValidateTextureShape(payload);
                }
                else
                {
                    return ValidateMaterialShape(payload);
                }
            },
            info.payload);
    }

    ResourceUploadRequestCreateCode ValidatePayloadRanges(
        const ResourceUploadPayload& payload)
    {
        return std::visit(
            [](const auto& value)
            {
                using PayloadType = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<PayloadType, MeshUploadPayload>)
                {
                    return ValidateMeshRanges(value);
                }
                else if constexpr (std::is_same_v<PayloadType, TextureUploadPayload>)
                {
                    return ValidateTextureRanges(value);
                }
                else
                {
                    return ResourceUploadRequestCreateCode::Created;
                }
            },
            payload);
    }

    bool PayloadMatchesKind(const ResourceUploadRequestCreateInfo& info)
    {
        return (info.kind == RenderResourceKind::Mesh &&
                std::holds_alternative<MeshUploadPayload>(info.payload)) ||
               (info.kind == RenderResourceKind::Texture &&
                std::holds_alternative<TextureUploadPayload>(info.payload)) ||
               (info.kind == RenderResourceKind::Material &&
                std::holds_alternative<MaterialUploadPayload>(info.payload));
    }

    ResourceUploadRequestCreateCode AccumulatePayloadBytes(
        const ResourceUploadRequestCreateInfo& info,
        uint64& total)
    {
        ResourceUploadRequestCreateCode code = std::visit(
            [&total](const auto& payload)
            {
                using PayloadType = std::decay_t<decltype(payload)>;
                ResourceUploadRequestCreateCode result =
                    ResourceUploadRequestCreateCode::Created;
                if constexpr (std::is_same_v<PayloadType, MeshUploadPayload>)
                {
                    const std::span<const uint8> bytes =
                        GetUploadPayloadBytes(payload);
                    result = Detail::AccumulateOwnedBytes(
                        static_cast<uint64>(bytes.size()), sizeof(uint8), total);
                    if (result == ResourceUploadRequestCreateCode::Created)
                    {
                        result = Detail::AccumulateOwnedBytes(
                            static_cast<uint64>(payload.submeshes.size()),
                            sizeof(MeshUploadSubmesh), total);
                    }
                }
                else if constexpr (std::is_same_v<PayloadType, TextureUploadPayload>)
                {
                    const std::span<const uint8> bytes =
                        GetUploadPayloadBytes(payload);
                    result = Detail::AccumulateOwnedBytes(
                        static_cast<uint64>(bytes.size()), sizeof(uint8), total);
                    if (result == ResourceUploadRequestCreateCode::Created)
                    {
                        result = Detail::AccumulateOwnedBytes(
                            static_cast<uint64>(payload.subresources.size()),
                            sizeof(TextureUploadSubresource), total);
                    }
                }
                else
                {
                    result = Detail::AccumulateOwnedBytes(
                        static_cast<uint64>(payload.textureBindings.size()),
                        sizeof(MaterialUploadTextureBinding), total);
                }
                return result;
            },
            info.payload);
        if (code != ResourceUploadRequestCreateCode::Created)
        {
            return code;
        }
        return Detail::AccumulateOwnedBytes(
            static_cast<uint64>(info.dependencies.size()),
            sizeof(RenderResourceHandle), total);
    }
} // namespace

namespace Detail
{
    ResourceUploadRequestCreateCode AccumulateOwnedBytes(
        uint64 elementCount,
        uint64 elementSize,
        uint64& total) noexcept
    {
        if (elementSize != 0 &&
            elementCount > std::numeric_limits<uint64>::max() / elementSize)
        {
            return ResourceUploadRequestCreateCode::PayloadByteCountOverflow;
        }
        const uint64 addition = elementCount * elementSize;
        if (total > std::numeric_limits<uint64>::max() - addition)
        {
            return ResourceUploadRequestCreateCode::PayloadByteCountOverflow;
        }
        total += addition;
        return ResourceUploadRequestCreateCode::Created;
    }
} // namespace Detail

ResourceUploadRequestCreateResult ResourceUploadRequest::Create(
    ResourceUploadRequestCreateInfo info)
{
    auto fail = [](ResourceUploadRequestCreateCode code)
    {
        return ResourceUploadRequestCreateResult{code, {}};
    };

    if (info.schemaId != RVX_RESOURCE_UPLOAD_REQUEST_SCHEMA_ID ||
        info.schemaVersion != RVX_RESOURCE_UPLOAD_REQUEST_SCHEMA_VERSION)
    {
        return fail(ResourceUploadRequestCreateCode::InvalidSchema);
    }
    if (info.sequence == 0)
    {
        return fail(ResourceUploadRequestCreateCode::InvalidSequence);
    }
    if (!info.assetId.IsValid())
    {
        return fail(ResourceUploadRequestCreateCode::InvalidAsset);
    }
    if (!info.handle.IsValid())
    {
        return fail(ResourceUploadRequestCreateCode::InvalidHandle);
    }
    if (!IsDeclared(info.kind))
    {
        return fail(ResourceUploadRequestCreateCode::InvalidKind);
    }
    if (info.operation != RenderResourceContentOperation::Create &&
        info.operation != RenderResourceContentOperation::Replace)
    {
        return fail(ResourceUploadRequestCreateCode::InvalidContentOperation);
    }
    if (info.sourceRevision != 0 &&
        info.provenance.sourceRevision != 0 &&
        info.sourceRevision != info.provenance.sourceRevision)
    {
        return fail(ResourceUploadRequestCreateCode::ConflictingSourceRevision);
    }
    // Compatibility Create builders may still provide the revision only in
    // provenance. Replace always requires the explicit v2 authority field.
    if (info.operation == RenderResourceContentOperation::Create &&
        info.sourceRevision == 0)
    {
        info.sourceRevision = info.provenance.sourceRevision;
    }
    if (info.operation == RenderResourceContentOperation::Replace &&
        info.sourceRevision == 0)
    {
        return fail(ResourceUploadRequestCreateCode::InvalidSourceRevision);
    }
    info.provenance.sourceRevision = info.sourceRevision;
    if (!PayloadMatchesKind(info))
    {
        return fail(ResourceUploadRequestCreateCode::PayloadKindMismatch);
    }

    ResourceUploadRequestCreateCode code = ValidatePayloadShape(info);
    if (code != ResourceUploadRequestCreateCode::Created)
    {
        return fail(code);
    }
    code = ValidatePayloadRanges(info.payload);
    if (code != ResourceUploadRequestCreateCode::Created)
    {
        return fail(code);
    }

    uint64 derivedPayloadBytes = 0;
    code = AccumulatePayloadBytes(info, derivedPayloadBytes);
    if (code != ResourceUploadRequestCreateCode::Created)
    {
        return fail(code);
    }
    if (derivedPayloadBytes != info.declaredPayloadBytes)
    {
        return fail(ResourceUploadRequestCreateCode::DiagnosticByteCountMismatch);
    }

    for (size_t index = 0; index < info.dependencies.size(); ++index)
    {
        const RenderResourceHandle dependency = info.dependencies[index];
        if (!dependency.IsValid())
        {
            return fail(ResourceUploadRequestCreateCode::InvalidDependency);
        }
        if (dependency == info.handle)
        {
            return fail(ResourceUploadRequestCreateCode::SelfDependency);
        }
        for (size_t previous = 0; previous < index; ++previous)
        {
            if (info.dependencies[previous] == dependency)
            {
                return fail(ResourceUploadRequestCreateCode::DuplicateDependency);
            }
        }
    }

    ResourceUploadRequestRef request(
        new ResourceUploadRequest(std::move(info), derivedPayloadBytes));
    return ResourceUploadRequestCreateResult{
        ResourceUploadRequestCreateCode::Created, std::move(request)};
}

ResourceUploadRequest::ResourceUploadRequest(ResourceUploadRequestCreateInfo&& info,
                                             uint64 derivedPayloadBytes)
    : m_schemaId(info.schemaId)
    , m_schemaVersion(info.schemaVersion)
    , m_sequence(info.sequence)
    , m_assetId(info.assetId)
    , m_handle(info.handle)
    , m_kind(info.kind)
    , m_operation(info.operation)
    , m_sourceRevision(info.sourceRevision)
    , m_payload(std::move(info.payload))
    , m_dependencies(std::move(info.dependencies))
    , m_dependencyReadiness(info.dependencyReadiness)
    , m_derivedPayloadBytes(derivedPayloadBytes)
    , m_declaredPayloadBytes(info.declaredPayloadBytes)
    , m_priority(info.priority)
    , m_provenance(info.provenance)
{
}

uint32 ResourceUploadRequest::GetSchemaId() const noexcept { return m_schemaId; }
uint32 ResourceUploadRequest::GetSchemaVersion() const noexcept { return m_schemaVersion; }
uint64 ResourceUploadRequest::GetSequence() const noexcept { return m_sequence; }
AssetId ResourceUploadRequest::GetAssetId() const noexcept { return m_assetId; }
RenderResourceHandle ResourceUploadRequest::GetHandle() const noexcept { return m_handle; }
RenderResourceKind ResourceUploadRequest::GetKind() const noexcept { return m_kind; }
RenderResourceContentOperation ResourceUploadRequest::GetOperation() const noexcept
{
    return m_operation;
}
uint64 ResourceUploadRequest::GetSourceRevision() const noexcept
{
    return m_sourceRevision;
}
uint64 ResourceUploadRequest::GetDerivedPayloadBytes() const noexcept
{
    return m_derivedPayloadBytes;
}
uint64 ResourceUploadRequest::GetDeclaredPayloadBytes() const noexcept
{
    return m_declaredPayloadBytes;
}
const ResourceUploadPayload& ResourceUploadRequest::GetPayload() const noexcept
{
    return m_payload;
}
const std::vector<RenderResourceHandle>&
ResourceUploadRequest::GetDependencies() const noexcept
{
    return m_dependencies;
}
RenderDependencyReadiness
ResourceUploadRequest::GetDependencyReadiness() const noexcept
{
    return m_dependencyReadiness;
}
RenderUploadPriority ResourceUploadRequest::GetPriority() const noexcept
{
    return m_priority;
}
const ResourceUploadDiagnosticProvenance&
ResourceUploadRequest::GetProvenance() const noexcept
{
    return m_provenance;
}
} // namespace RVX
