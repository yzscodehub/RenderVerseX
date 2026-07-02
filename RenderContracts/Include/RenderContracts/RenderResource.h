#pragma once

/**
 * @file RenderResource.h
 * @brief Render-facing resource upload and material source contracts.
 */

#include "Core/Math/AABB.h"
#include "Core/RefCounted.h"
#include "Core/Types.h"
#include "RenderContracts/RenderMaterial.h"

#include <string_view>
#include <vector>

namespace RVX
{
    class IRenderTextureUploadSource;

    enum class RenderMaterialTextureSlot : uint8
    {
        BaseColor = 0,
        Normal,
        MetallicRoughness,
        Occlusion,
        Emissive
    };

    struct RenderMeshAttributeUploadView
    {
        const void* data = nullptr;
        size_t size = 0;
        size_t stride = 0;

        bool IsValid() const { return data != nullptr && size > 0 && stride > 0; }
    };

    enum class RenderMeshIndexType : uint8
    {
        UInt8 = 0,
        UInt16,
        UInt32
    };

    enum class RenderPrimitiveTopology : uint8
    {
        Triangles = 0,
        TriangleStrip,
        TriangleFan,
        Lines,
        LineStrip,
        LineLoop,
        Points
    };

    struct RenderMeshSubmeshUploadInfo
    {
        uint32 indexOffset = 0;
        uint32 indexCount = 0;
        int32 baseVertex = 0;
        RenderPrimitiveTopology primitive = RenderPrimitiveTopology::Triangles;
    };

    struct RenderMeshUploadData
    {
        size_t vertexCount = 0;
        size_t indexCount = 0;
        RenderMeshIndexType indexType = RenderMeshIndexType::UInt32;
        RenderPrimitiveTopology primitive = RenderPrimitiveTopology::Triangles;
        const void* indexData = nullptr;
        size_t indexDataSize = 0;
        RenderMeshAttributeUploadView position;
        RenderMeshAttributeUploadView normal;
        RenderMeshAttributeUploadView uv;
        RenderMeshAttributeUploadView tangent;
        RenderMeshAttributeUploadView boneIndices;
        RenderMeshAttributeUploadView boneWeights;
        std::vector<RenderMeshSubmeshUploadInfo> submeshes;

        bool HasIndexData() const { return indexData != nullptr && indexDataSize > 0; }
    };

    enum class RenderTextureUploadFormat : uint8
    {
        Unknown = 0,
        RGBA8,
        RGBA16F,
        RGBA32F,
        RGB8,
        RG8,
        R8,
        BC1,
        BC3,
        BC5,
        BC7
    };

    struct RenderTextureUploadMetadata
    {
        uint32 width = 0;
        uint32 height = 0;
        uint32 depth = 1;
        uint32 mipLevels = 1;
        uint32 arrayLayers = 1;
        RenderTextureUploadFormat format = RenderTextureUploadFormat::RGBA8;
        bool isCubemap = false;
        bool isArray = false;
        bool isSRGB = true;
    };

    struct RenderTextureUploadData
    {
        RenderTextureUploadMetadata metadata;
        const uint8* data = nullptr;
        size_t dataSize = 0;

        bool HasData() const { return data != nullptr && dataSize > 0; }
    };

    enum class RenderTextureWrapMode : uint8
    {
        Repeat = 0,
        MirrorRepeat,
        ClampToEdge,
        ClampToBorder
    };

    enum class RenderTextureFilterMode : uint8
    {
        Nearest = 0,
        Linear,
        NearestMipmapNearest,
        LinearMipmapNearest,
        NearestMipmapLinear,
        LinearMipmapLinear
    };

    struct RenderMaterialTextureBinding
    {
        IRenderTextureUploadSource* texture = nullptr;
        uint64 textureId = 0;
        int32 uvSet = 0;
        Vec2 offset{0.0f, 0.0f};
        Vec2 scale{1.0f, 1.0f};
        float rotation = 0.0f;
        RenderTextureWrapMode wrapS = RenderTextureWrapMode::Repeat;
        RenderTextureWrapMode wrapT = RenderTextureWrapMode::Repeat;
        RenderTextureFilterMode minFilter = RenderTextureFilterMode::LinearMipmapLinear;
        RenderTextureFilterMode magFilter = RenderTextureFilterMode::Linear;
        bool hasTextureInfo = false;

        bool IsValid() const { return texture != nullptr || textureId != 0; }
    };

    class IRenderResourceSource
    {
    public:
        virtual ~IRenderResourceSource() = default;

        virtual uint64 GetRenderResourceId() const = 0;
        virtual std::string_view GetRenderResourceName() const = 0;
        virtual uint32 GetRenderResourceRefCount() const = 0;
        virtual RefCounted* GetRenderResourceRefCounted() = 0;
    };

    class IRenderMeshUploadSource : public IRenderResourceSource
    {
    public:
        ~IRenderMeshUploadSource() override = default;

        virtual RenderMeshUploadData GetRenderMeshUploadData() const = 0;
        virtual AABB GetRenderMeshBounds() const = 0;
        virtual size_t GetRenderMeshSubmeshCount() const = 0;
    };

    class IRenderTextureUploadSource : public IRenderResourceSource
    {
    public:
        ~IRenderTextureUploadSource() override = default;

        virtual RenderTextureUploadData GetRenderTextureUploadData() const = 0;
        virtual bool IsRenderDefaultFallbackTexture() const = 0;
        virtual uint32 GetRenderTextureMipLevels() const = 0;
    };

    class IRenderMaterialSource : public IRenderResourceSource
    {
    public:
        ~IRenderMaterialSource() override = default;

        virtual MaterialSourceData GetRenderMaterialSourceData() const = 0;
        virtual IRenderTextureUploadSource* GetRenderMaterialTexture(RenderMaterialTextureSlot slot) const = 0;
        virtual RenderMaterialTextureBinding GetRenderMaterialTextureBinding(RenderMaterialTextureSlot slot) const = 0;
    };

} // namespace RVX
