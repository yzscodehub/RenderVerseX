#include "Resource/RenderUploadRequestBuilder.h"

#include "Resource/Types/MaterialResource.h"
#include "Resource/Types/MeshResource.h"
#include "Resource/Types/TextureResource.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <utility>

namespace RVX::Resource
{
namespace
{
    struct TextureFormatLayout
    {
        uint32 blockWidth = 1;
        uint32 blockHeight = 1;
        uint32 bytesPerBlock = 0;
    };

    bool CheckedAdd(uint64 left, uint64 right, uint64& result)
    {
        if (left > std::numeric_limits<uint64>::max() - right)
            return false;
        result = left + right;
        return true;
    }

    bool CheckedMultiply(uint64 left, uint64 right, uint64& result)
    {
        if (left != 0 && right > std::numeric_limits<uint64>::max() / left)
            return false;
        result = left * right;
        return true;
    }

    bool AppendRange(std::vector<uint8>& destination,
                     const void* source,
                     size_t size,
                     size_t stride,
                     UploadByteRange& range)
    {
        if (source == nullptr || size == 0)
        {
            range = {};
            return true;
        }
        if (stride == 0 || size > std::numeric_limits<uint64>::max() ||
            destination.size() > std::numeric_limits<uint64>::max() - size)
        {
            return false;
        }

        range.offset = static_cast<uint64>(destination.size());
        range.size = static_cast<uint64>(size);
        if (stride > std::numeric_limits<uint32>::max())
            return false;
        range.stride = static_cast<uint32>(stride);
        const auto* bytes = static_cast<const uint8*>(source);
        destination.insert(destination.end(), bytes, bytes + size);
        return true;
    }

    MeshUploadIndexType ConvertIndexType(IndexType type)
    {
        switch (type)
        {
            case IndexType::UInt8: return MeshUploadIndexType::UInt8;
            case IndexType::UInt16: return MeshUploadIndexType::UInt16;
            case IndexType::UInt32: return MeshUploadIndexType::UInt32;
        }
        return MeshUploadIndexType::UInt32;
    }

    uint32 GetIndexStride(MeshUploadIndexType type)
    {
        switch (type)
        {
            case MeshUploadIndexType::UInt8: return 1;
            case MeshUploadIndexType::UInt16: return 2;
            case MeshUploadIndexType::UInt32: return 4;
        }
        return 0;
    }

    MeshUploadPrimitiveTopology ConvertTopology(PrimitiveType topology)
    {
        switch (topology)
        {
            case PrimitiveType::TriangleStrip:
                return MeshUploadPrimitiveTopology::TriangleStrip;
            case PrimitiveType::TriangleFan:
                return MeshUploadPrimitiveTopology::TriangleFan;
            case PrimitiveType::Lines:
                return MeshUploadPrimitiveTopology::Lines;
            case PrimitiveType::LineStrip:
                return MeshUploadPrimitiveTopology::LineStrip;
            case PrimitiveType::LineLoop:
                return MeshUploadPrimitiveTopology::LineLoop;
            case PrimitiveType::Points:
                return MeshUploadPrimitiveTopology::Points;
            case PrimitiveType::Triangles:
            default:
                return MeshUploadPrimitiveTopology::Triangles;
        }
    }

    TextureUploadFormat ConvertTextureFormat(TextureFormat format)
    {
        switch (format)
        {
            case TextureFormat::RGBA8: return TextureUploadFormat::RGBA8;
            case TextureFormat::RGBA16F: return TextureUploadFormat::RGBA16F;
            case TextureFormat::RGBA32F: return TextureUploadFormat::RGBA32F;
            case TextureFormat::RGB8: return TextureUploadFormat::RGB8;
            case TextureFormat::RG8: return TextureUploadFormat::RG8;
            case TextureFormat::R8: return TextureUploadFormat::R8;
            case TextureFormat::BC1: return TextureUploadFormat::BC1;
            case TextureFormat::BC3: return TextureUploadFormat::BC3;
            case TextureFormat::BC5: return TextureUploadFormat::BC5;
            case TextureFormat::BC7: return TextureUploadFormat::BC7;
            case TextureFormat::Unknown: return TextureUploadFormat::Unknown;
        }
        return TextureUploadFormat::Unknown;
    }

    TextureFormatLayout GetTextureLayout(TextureFormat format)
    {
        switch (format)
        {
            case TextureFormat::RGBA8: return {1, 1, 4};
            case TextureFormat::RGBA16F: return {1, 1, 8};
            case TextureFormat::RGBA32F: return {1, 1, 16};
            case TextureFormat::RGB8: return {1, 1, 3};
            case TextureFormat::RG8: return {1, 1, 2};
            case TextureFormat::R8: return {1, 1, 1};
            case TextureFormat::BC1: return {4, 4, 8};
            case TextureFormat::BC3:
            case TextureFormat::BC5:
            case TextureFormat::BC7: return {4, 4, 16};
            case TextureFormat::Unknown: return {};
        }
        return {};
    }

    MaterialUploadWrapMode ConvertWrap(TextureInfo::WrapMode mode)
    {
        switch (mode)
        {
            case TextureInfo::WrapMode::MirrorRepeat:
                return MaterialUploadWrapMode::MirrorRepeat;
            case TextureInfo::WrapMode::ClampToEdge:
                return MaterialUploadWrapMode::ClampToEdge;
            case TextureInfo::WrapMode::ClampToBorder:
                return MaterialUploadWrapMode::ClampToBorder;
            case TextureInfo::WrapMode::Repeat:
            default:
                return MaterialUploadWrapMode::Repeat;
        }
    }

    MaterialUploadFilterMode ConvertFilter(TextureInfo::FilterMode mode)
    {
        switch (mode)
        {
            case TextureInfo::FilterMode::Nearest:
                return MaterialUploadFilterMode::Nearest;
            case TextureInfo::FilterMode::NearestMipmapNearest:
                return MaterialUploadFilterMode::NearestMipmapNearest;
            case TextureInfo::FilterMode::LinearMipmapNearest:
                return MaterialUploadFilterMode::LinearMipmapNearest;
            case TextureInfo::FilterMode::NearestMipmapLinear:
                return MaterialUploadFilterMode::NearestMipmapLinear;
            case TextureInfo::FilterMode::LinearMipmapLinear:
                return MaterialUploadFilterMode::LinearMipmapLinear;
            case TextureInfo::FilterMode::Linear:
            default:
                return MaterialUploadFilterMode::Linear;
        }
    }

    RenderUploadRequestBuildResult FinishRequest(
        ResourceUploadRequestCreateInfo info)
    {
        ResourceUploadRequestCreateResult created =
            ResourceUploadRequest::Create(std::move(info));
        if (created.code != ResourceUploadRequestCreateCode::Created)
        {
            return {RenderUploadRequestBuildCode::RequestRejected,
                    created.code,
                    {}};
        }
        return {RenderUploadRequestBuildCode::Built,
                created.code,
                std::move(created.request)};
    }

    RenderUploadRequestBuildResult BuildMesh(
        const MeshResource& resource,
        RenderResourceHandle handle,
        uint64 sequence,
        RenderUploadPriority priority,
        uint64 sourceRevision,
        RenderResourceContentOperation operation)
    {
        const std::shared_ptr<Mesh> mesh = resource.GetMesh();
        const AABB bounds = resource.GetBounds();
        const VertexAttribute* position =
            mesh ? mesh->GetAttribute(VertexBufferNames::Position) : nullptr;
        if (!mesh || mesh->GetVertexCount() == 0 || position == nullptr ||
            position->GetData() == nullptr || position->GetTotalSize() == 0 ||
            position->GetStride() == 0 || !bounds.IsValid())
        {
            return {RenderUploadRequestBuildCode::InvalidResource};
        }

        MeshUploadPayload payload;
        payload.createInfo.vertexCount = static_cast<uint64>(mesh->GetVertexCount());
        payload.createInfo.indexCount = static_cast<uint64>(mesh->GetIndexCount());
        payload.createInfo.indexType = ConvertIndexType(mesh->GetIndexType());
        payload.createInfo.topology = ConvertTopology(mesh->GetPrimitiveType());
        payload.createInfo.boundsMin = bounds.GetMin();
        payload.createInfo.boundsMax = bounds.GetMax();

        const auto appendAttribute = [&payload](const VertexAttribute* attribute,
                                                UploadByteRange& range)
        {
            return attribute == nullptr
                       ? AppendRange(payload.bytes, nullptr, 0, 0, range)
                       : AppendRange(payload.bytes,
                                     attribute->GetData(),
                                     attribute->GetTotalSize(),
                                     attribute->GetStride(),
                                     range);
        };

        const auto& indexData = mesh->GetIndexData();
        const char* uvNames[] = {
            VertexBufferNames::UV, "uv", "texcoord0", "texcoord"};
        const VertexAttribute* uv = nullptr;
        for (const char* uvName : uvNames)
        {
            uv = mesh->GetAttribute(uvName);
            if (uv != nullptr && uv->GetData() != nullptr &&
                uv->GetTotalSize() > 0)
            {
                break;
            }
            uv = nullptr;
        }

        const VertexAttribute* tangent =
            mesh->GetAttribute(VertexBufferNames::Tangent);
        const bool hasTangentBasis =
            tangent != nullptr && tangent->GetData() != nullptr &&
            tangent->GetVertexCount() == mesh->GetVertexCount() &&
            tangent->GetComponents() == 4 &&
            tangent->GetType() == AttributeType::Float &&
            tangent->GetStride() == sizeof(Vec4) &&
            tangent->GetTotalSize() != 0;
        payload.createInfo.hasTangentBasis = hasTangentBasis;

        std::vector<Vec4> fallbackTangents;
        if (!hasTangentBasis)
        {
            fallbackTangents.assign(mesh->GetVertexCount(),
                                    Vec4{1.0f, 0.0f, 0.0f, 1.0f});
        }

        if ((mesh->GetIndexCount() != 0 &&
             !AppendRange(payload.bytes,
                          indexData.empty() ? nullptr : indexData.data(),
                          indexData.size(),
                          GetIndexStride(payload.createInfo.indexType),
                          payload.indexRange)) ||
            !appendAttribute(position, payload.positionRange) ||
            !appendAttribute(mesh->GetAttribute(VertexBufferNames::Normal),
                             payload.normalRange) ||
            !appendAttribute(uv, payload.uvRange) ||
            !AppendRange(payload.bytes,
                         hasTangentBasis ? tangent->GetData()
                                             : fallbackTangents.data(),
                         hasTangentBasis ? tangent->GetTotalSize()
                                             : fallbackTangents.size() * sizeof(Vec4),
                         hasTangentBasis ? tangent->GetStride()
                                             : sizeof(Vec4),
                         payload.tangentRange) ||
            !appendAttribute(mesh->GetAttribute(VertexBufferNames::BoneIndices),
                             payload.boneIndexRange) ||
            !appendAttribute(mesh->GetAttribute(VertexBufferNames::BoneWeights),
                             payload.boneWeightRange))
        {
            return {RenderUploadRequestBuildCode::PayloadOverflow};
        }

        if (mesh->GetIndexCount() != 0)
        {
            if (mesh->HasSubMeshes())
            {
                payload.submeshes.reserve(mesh->GetSubMeshes().size());
                for (const auto& submesh : mesh->GetSubMeshes())
                {
                    payload.submeshes.push_back(MeshUploadSubmesh{
                        submesh.indexOffset,
                        submesh.indexCount,
                        submesh.baseVertex,
                        ConvertTopology(submesh.primitive.value_or(
                            mesh->GetPrimitiveType()))});
                }
            }
            else
            {
                payload.submeshes.push_back(MeshUploadSubmesh{
                    0,
                    static_cast<uint32>(mesh->GetIndexCount()),
                    0,
                    payload.createInfo.topology});
            }
        }

        uint64 submeshBytes = 0;
        uint64 declaredBytes = 0;
        if (!CheckedMultiply(static_cast<uint64>(payload.submeshes.size()),
                             sizeof(MeshUploadSubmesh),
                             submeshBytes) ||
            !CheckedAdd(static_cast<uint64>(payload.bytes.size()),
                        submeshBytes,
                        declaredBytes))
        {
            return {RenderUploadRequestBuildCode::PayloadOverflow};
        }
        payload.byteStorage =
            UploadByteStorage::Create(std::move(payload.bytes));
        if (!payload.byteStorage)
            return {RenderUploadRequestBuildCode::InvalidResource};

        ResourceUploadRequestCreateInfo info;
        info.sequence = sequence;
        info.assetId = AssetId{resource.GetId()};
        info.handle = handle;
        info.kind = RenderResourceKind::Mesh;
        info.operation = operation;
        info.sourceRevision = sourceRevision;
        info.payload = std::move(payload);
        info.declaredPayloadBytes = declaredBytes;
        info.priority = priority;
        info.provenance.sourceRevision = sourceRevision;
        info.provenance.sourceKind = static_cast<uint32>(resource.GetType());
        return FinishRequest(std::move(info));
    }

    RenderUploadRequestBuildResult BuildTexture(
        const TextureResource& resource,
        RenderResourceHandle handle,
        uint64 sequence,
        RenderUploadPriority priority,
        uint64 sourceRevision,
        RenderResourceContentOperation operation)
    {
        const TextureMetadata& metadata = resource.GetMetadata();
        const TextureFormatLayout layout = GetTextureLayout(metadata.format);
        if (metadata.width == 0 || metadata.height == 0 ||
            metadata.depth == 0 || metadata.mipLevels == 0 ||
            metadata.arrayLayers == 0 || layout.bytesPerBlock == 0 ||
            resource.GetData().empty())
        {
            return {RenderUploadRequestBuildCode::InvalidResource};
        }

        TextureUploadPayload payload;
        payload.createInfo.width = metadata.width;
        payload.createInfo.height = metadata.height;
        payload.createInfo.depth = metadata.depth;
        payload.createInfo.mipLevels = metadata.mipLevels;
        payload.createInfo.arrayLayers = metadata.arrayLayers;
        payload.createInfo.format = ConvertTextureFormat(metadata.format);
        payload.createInfo.isCubemap = metadata.isCubemap;
        payload.createInfo.isArray = metadata.isArray;
        payload.createInfo.isSRGB = metadata.isSRGB;
        payload.byteStorage =
            UploadByteStorage::CreateShared(resource.GetDataStorage());
        const std::span<const uint8> textureBytes =
            GetUploadPayloadBytes(payload);
        if (!payload.byteStorage || textureBytes.empty())
            return {RenderUploadRequestBuildCode::InvalidResource};

        uint64 offset = 0;
        for (uint32 mip = 0; mip < metadata.mipLevels; ++mip)
        {
            const uint32 width =
                mip >= 32U ? 1U : std::max(1U, metadata.width >> mip);
            const uint32 height =
                mip >= 32U ? 1U : std::max(1U, metadata.height >> mip);
            const uint32 depth =
                mip >= 32U ? 1U : std::max(1U, metadata.depth >> mip);
            const uint64 blocksWide =
                (static_cast<uint64>(width) + layout.blockWidth - 1U) /
                layout.blockWidth;
            const uint64 blocksHigh =
                (static_cast<uint64>(height) + layout.blockHeight - 1U) /
                layout.blockHeight;
            uint64 rowPitch = 0;
            uint64 slicePitch = 0;
            uint64 subresourceBytes = 0;
            if (!CheckedMultiply(blocksWide,
                                 layout.bytesPerBlock,
                                 rowPitch) ||
                !CheckedMultiply(rowPitch, blocksHigh, slicePitch) ||
                !CheckedMultiply(slicePitch, depth, subresourceBytes))
            {
                return {RenderUploadRequestBuildCode::PayloadOverflow};
            }

            for (uint32 layer = 0; layer < metadata.arrayLayers; ++layer)
            {
                uint64 end = 0;
                if (!CheckedAdd(offset, subresourceBytes, end) ||
                    end > textureBytes.size())
                {
                    return {RenderUploadRequestBuildCode::InvalidResource};
                }
                payload.subresources.push_back(TextureUploadSubresource{
                    UploadByteRange{offset, subresourceBytes, 0},
                    mip,
                    layer,
                    rowPitch,
                    slicePitch});
                offset = end;
            }
        }
        if (offset != textureBytes.size())
        {
            return {RenderUploadRequestBuildCode::InvalidResource};
        }

        uint64 subresourceTableBytes = 0;
        uint64 declaredBytes = 0;
        if (!CheckedMultiply(
                static_cast<uint64>(payload.subresources.size()),
                sizeof(TextureUploadSubresource),
                subresourceTableBytes) ||
            !CheckedAdd(static_cast<uint64>(textureBytes.size()),
                        subresourceTableBytes,
                        declaredBytes))
        {
            return {RenderUploadRequestBuildCode::PayloadOverflow};
        }

        ResourceUploadRequestCreateInfo info;
        info.sequence = sequence;
        info.assetId = AssetId{resource.GetId()};
        info.handle = handle;
        info.kind = RenderResourceKind::Texture;
        info.operation = operation;
        info.sourceRevision = sourceRevision;
        info.payload = std::move(payload);
        info.declaredPayloadBytes = declaredBytes;
        info.priority = priority;
        info.provenance.sourceRevision = sourceRevision;
        info.provenance.sourceKind = static_cast<uint32>(resource.GetType());
        return FinishRequest(std::move(info));
    }

    RenderUploadRequestBuildResult BuildMaterial(
        const MaterialResource& resource,
        RenderResourceHandle handle,
        uint64 sequence,
        const RenderResourceDependencyResolver& dependencyResolver,
        RenderUploadPriority priority,
        uint64 sourceRevision,
        RenderResourceContentOperation operation)
    {
        MaterialUploadPayload payload;
        payload.sourceData = resource.GetMaterialSourceData();

        constexpr MaterialUploadTextureSlot slots[] = {
            MaterialUploadTextureSlot::BaseColor,
            MaterialUploadTextureSlot::Normal,
            MaterialUploadTextureSlot::MetallicRoughness,
            MaterialUploadTextureSlot::Occlusion,
            MaterialUploadTextureSlot::Emissive};
        const std::shared_ptr<Material> material = resource.GetMaterial();

        std::vector<RenderResourceHandle> dependencies;
        for (MaterialUploadTextureSlot slot : slots)
        {
            ResourceHandle<TextureResource> sourceTexture;
            const std::optional<TextureInfo>* textureInfo = nullptr;
            switch (slot)
            {
                case MaterialUploadTextureSlot::BaseColor:
                    sourceTexture = resource.GetAlbedoTexture();
                    textureInfo = material ? &material->GetBaseColorTexture()
                                           : nullptr;
                    break;
                case MaterialUploadTextureSlot::Normal:
                    sourceTexture = resource.GetNormalTexture();
                    textureInfo = material ? &material->GetNormalTexture()
                                           : nullptr;
                    break;
                case MaterialUploadTextureSlot::MetallicRoughness:
                    sourceTexture = resource.GetMetallicRoughnessTexture();
                    textureInfo = material
                                      ? &material->GetMetallicRoughnessTexture()
                                      : nullptr;
                    break;
                case MaterialUploadTextureSlot::Occlusion:
                    sourceTexture = resource.GetAOTexture();
                    textureInfo = material ? &material->GetOcclusionTexture()
                                           : nullptr;
                    break;
                case MaterialUploadTextureSlot::Emissive:
                    sourceTexture = resource.GetEmissiveTexture();
                    textureInfo = material ? &material->GetEmissiveTexture()
                                           : nullptr;
                    break;
            }
            if (!sourceTexture)
                continue;
            if (!dependencyResolver)
            {
                return {RenderUploadRequestBuildCode::DependencyUnavailable};
            }
            const RenderResourceHandle texture = dependencyResolver(
                AssetId{sourceTexture.GetId()}, RenderResourceKind::Texture);
            if (!texture.IsValid())
            {
                return {RenderUploadRequestBuildCode::DependencyUnavailable};
            }

            MaterialUploadTextureBinding binding;
            binding.slot = slot;
            binding.texture = texture;
            binding.isDefaultFallback = sourceTexture->IsDefaultFallback();
            if (textureInfo != nullptr && textureInfo->has_value())
            {
                const TextureInfo& source = textureInfo->value();
                binding.uvSet = source.uvSet;
                binding.offset = source.offset;
                binding.scale = source.scale;
                binding.rotation = source.rotation;
                binding.wrapS = ConvertWrap(source.wrapS);
                binding.wrapT = ConvertWrap(source.wrapT);
                binding.minFilter = ConvertFilter(source.minFilter);
                binding.magFilter = ConvertFilter(source.magFilter);
            }
            payload.textureBindings.push_back(binding);
            payload.sourceData.textureFlags |=
                1U << static_cast<uint32>(slot);
            if (std::find(dependencies.begin(), dependencies.end(), texture) ==
                dependencies.end())
            {
                dependencies.push_back(texture);
            }
        }

        uint64 bindingBytes = 0;
        uint64 dependencyBytes = 0;
        uint64 declaredBytes = 0;
        if (!CheckedMultiply(
                static_cast<uint64>(payload.textureBindings.size()),
                sizeof(MaterialUploadTextureBinding),
                bindingBytes) ||
            !CheckedMultiply(static_cast<uint64>(dependencies.size()),
                             sizeof(RenderResourceHandle),
                             dependencyBytes) ||
            !CheckedAdd(bindingBytes, dependencyBytes, declaredBytes))
        {
            return {RenderUploadRequestBuildCode::PayloadOverflow};
        }

        ResourceUploadRequestCreateInfo info;
        info.sequence = sequence;
        info.assetId = AssetId{resource.GetId()};
        info.handle = handle;
        info.kind = RenderResourceKind::Material;
        info.operation = operation;
        info.sourceRevision = sourceRevision;
        info.payload = std::move(payload);
        info.dependencies = std::move(dependencies);
        info.declaredPayloadBytes = declaredBytes;
        info.priority = priority;
        info.provenance.sourceRevision = sourceRevision;
        info.provenance.sourceKind = static_cast<uint32>(resource.GetType());
        return FinishRequest(std::move(info));
    }
} // namespace

    RenderUploadRequestBuildResult RenderUploadRequestBuilder::Build(
        const IResource& resource,
        RenderResourceHandle handle,
        uint64 sequence,
        const RenderResourceDependencyResolver& dependencyResolver,
        RenderUploadPriority priority,
        uint64 sourceRevision,
        RenderResourceContentOperation operation)
    {
        if (resource.GetId() == InvalidResourceId || !handle.IsValid() ||
            sequence == 0)
        {
            return {RenderUploadRequestBuildCode::InvalidResource};
        }

        switch (resource.GetType())
        {
            case ResourceType::Mesh:
            {
                const auto* mesh = dynamic_cast<const MeshResource*>(&resource);
                return mesh != nullptr
                           ? BuildMesh(*mesh,
                                       handle,
                                       sequence,
                                       priority,
                                       sourceRevision,
                                       operation)
                           : RenderUploadRequestBuildResult{
                                 RenderUploadRequestBuildCode::InvalidResource};
            }
            case ResourceType::Texture:
            {
                const auto* texture =
                    dynamic_cast<const TextureResource*>(&resource);
                return texture != nullptr
                           ? BuildTexture(*texture,
                                          handle,
                                          sequence,
                                          priority,
                                          sourceRevision,
                                          operation)
                           : RenderUploadRequestBuildResult{
                                 RenderUploadRequestBuildCode::InvalidResource};
            }
            case ResourceType::Material:
            {
                const auto* material =
                    dynamic_cast<const MaterialResource*>(&resource);
                return material != nullptr
                           ? BuildMaterial(*material,
                                           handle,
                                           sequence,
                                           dependencyResolver,
                                           priority,
                                           sourceRevision,
                                           operation)
                           : RenderUploadRequestBuildResult{
                                 RenderUploadRequestBuildCode::InvalidResource};
            }
            default:
                return {RenderUploadRequestBuildCode::UnsupportedResource};
        }
    }
} // namespace RVX::Resource
