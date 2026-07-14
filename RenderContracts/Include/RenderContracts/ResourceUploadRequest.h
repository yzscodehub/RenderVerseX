#pragma once

/**
 * @file ResourceUploadRequest.h
 * @brief Immutable owned resource-upload values crossing into Render.
 */

#include "Core/MathTypes.h"
#include "Core/Types.h"
#include "RenderContracts/RenderIdentity.h"
#include "RenderContracts/RenderMaterial.h"

#include <memory>
#include <variant>
#include <vector>

namespace RVX
{
    inline constexpr uint32 RVX_RESOURCE_UPLOAD_REQUEST_SCHEMA_ID = 0x52565855U;
    inline constexpr uint32 RVX_RESOURCE_UPLOAD_REQUEST_SCHEMA_VERSION = 1;

    enum class MeshUploadIndexType : uint8
    {
        UInt8 = 0,
        UInt16 = 1,
        UInt32 = 2
    };

    enum class MeshUploadPrimitiveTopology : uint8
    {
        Triangles = 0,
        TriangleStrip = 1,
        TriangleFan = 2,
        Lines = 3,
        LineStrip = 4,
        LineLoop = 5,
        Points = 6
    };

    enum class TextureUploadFormat : uint8
    {
        Unknown = 0,
        RGBA8 = 1,
        RGBA16F = 2,
        RGBA32F = 3,
        RGB8 = 4,
        RG8 = 5,
        R8 = 6,
        BC1 = 7,
        BC3 = 8,
        BC5 = 9,
        BC7 = 10
    };

    enum class MaterialUploadTextureSlot : uint8
    {
        BaseColor = 0,
        Normal = 1,
        MetallicRoughness = 2,
        Occlusion = 3,
        Emissive = 4
    };

    enum class MaterialUploadWrapMode : uint8
    {
        Repeat = 0,
        MirrorRepeat = 1,
        ClampToEdge = 2,
        ClampToBorder = 3
    };

    enum class MaterialUploadFilterMode : uint8
    {
        Nearest = 0,
        Linear = 1,
        NearestMipmapNearest = 2,
        LinearMipmapNearest = 3,
        NearestMipmapLinear = 4,
        LinearMipmapLinear = 5
    };

    struct UploadByteRange
    {
        uint64 offset = 0;
        uint64 size = 0;
        uint32 stride = 0;
    };

    struct MeshUploadCreateInfo
    {
        uint64 vertexCount = 0;
        uint64 indexCount = 0;
        MeshUploadIndexType indexType = MeshUploadIndexType::UInt32;
        MeshUploadPrimitiveTopology topology =
            MeshUploadPrimitiveTopology::Triangles;
        Vec3 boundsMin{0.0f};
        Vec3 boundsMax{0.0f};
    };

    struct MeshUploadSubmesh
    {
        uint32 indexOffset = 0;
        uint32 indexCount = 0;
        int32 baseVertex = 0;
        MeshUploadPrimitiveTopology topology =
            MeshUploadPrimitiveTopology::Triangles;
    };

    struct MeshUploadPayload
    {
        MeshUploadCreateInfo createInfo;
        std::vector<uint8> bytes;
        UploadByteRange indexRange;
        UploadByteRange positionRange;
        UploadByteRange normalRange;
        UploadByteRange uvRange;
        UploadByteRange tangentRange;
        UploadByteRange boneIndexRange;
        UploadByteRange boneWeightRange;
        std::vector<MeshUploadSubmesh> submeshes;
    };

    struct TextureUploadCreateInfo
    {
        uint32 width = 0;
        uint32 height = 0;
        uint32 depth = 1;
        uint32 mipLevels = 1;
        uint32 arrayLayers = 1;
        TextureUploadFormat format = TextureUploadFormat::Unknown;
        bool isCubemap = false;
        bool isArray = false;
        bool isSRGB = true;
    };

    struct TextureUploadSubresource
    {
        UploadByteRange bytes;
        uint32 mipLevel = 0;
        uint32 arrayLayer = 0;
        uint64 rowPitch = 0;
        uint64 slicePitch = 0;
    };

    struct TextureUploadPayload
    {
        TextureUploadCreateInfo createInfo;
        std::vector<uint8> bytes;
        std::vector<TextureUploadSubresource> subresources;
    };

    struct MaterialUploadTextureBinding
    {
        MaterialUploadTextureSlot slot = MaterialUploadTextureSlot::BaseColor;
        RenderResourceHandle texture;
        int32 uvSet = 0;
        Vec2 offset{0.0f, 0.0f};
        Vec2 scale{1.0f, 1.0f};
        float32 rotation = 0.0f;
        MaterialUploadWrapMode wrapS = MaterialUploadWrapMode::Repeat;
        MaterialUploadWrapMode wrapT = MaterialUploadWrapMode::Repeat;
        MaterialUploadFilterMode minFilter =
            MaterialUploadFilterMode::LinearMipmapLinear;
        MaterialUploadFilterMode magFilter = MaterialUploadFilterMode::Linear;
    };

    struct MaterialUploadPayload
    {
        MaterialSourceData sourceData;
        std::vector<MaterialUploadTextureBinding> textureBindings;
    };

    using ResourceUploadPayload = std::variant<
        MeshUploadPayload,
        TextureUploadPayload,
        MaterialUploadPayload>;

    struct ResourceUploadDiagnosticProvenance
    {
        uint64 sourceRevision = 0;
        uint32 sourceKind = 0;
        uint32 flags = 0;
    };

    struct ResourceUploadRequestCreateInfo
    {
        uint32 schemaId = RVX_RESOURCE_UPLOAD_REQUEST_SCHEMA_ID;
        uint32 schemaVersion = RVX_RESOURCE_UPLOAD_REQUEST_SCHEMA_VERSION;
        uint64 sequence = 0;
        AssetId assetId;
        RenderResourceHandle handle;
        RenderResourceKind kind = RenderResourceKind::Invalid;
        ResourceUploadPayload payload;
        std::vector<RenderResourceHandle> dependencies;
        RenderDependencyReadiness dependencyReadiness =
            RenderDependencyReadiness::RequireAll;
        uint64 declaredPayloadBytes = 0;
        RenderUploadPriority priority = RenderUploadPriority::Normal;
        ResourceUploadDiagnosticProvenance provenance;
    };

    enum class ResourceUploadRequestCreateCode : uint8
    {
        Created = 0,
        InvalidSchema = 1,
        InvalidSequence = 2,
        InvalidAsset = 3,
        InvalidHandle = 4,
        InvalidKind = 5,
        PayloadKindMismatch = 6,
        InvalidPayload = 7,
        PayloadRangeOverflow = 8,
        PayloadByteCountOverflow = 9,
        DiagnosticByteCountMismatch = 10,
        InvalidDependency = 11,
        DuplicateDependency = 12,
        SelfDependency = 13
    };

    class ResourceUploadRequest;
    using ResourceUploadRequestRef = std::shared_ptr<const ResourceUploadRequest>;

    struct ResourceUploadRequestCreateResult
    {
        ResourceUploadRequestCreateCode code =
            ResourceUploadRequestCreateCode::InvalidPayload;
        ResourceUploadRequestRef request;
    };

    /** @brief Immutable, factory-validated upload request with owned storage. */
    class ResourceUploadRequest final
    {
    public:
        /** @brief Validate and create an immutable request. */
        static ResourceUploadRequestCreateResult Create(
            ResourceUploadRequestCreateInfo info);

        ~ResourceUploadRequest() = default;
        ResourceUploadRequest(const ResourceUploadRequest&) = delete;
        ResourceUploadRequest& operator=(const ResourceUploadRequest&) = delete;
        ResourceUploadRequest(ResourceUploadRequest&&) = delete;
        ResourceUploadRequest& operator=(ResourceUploadRequest&&) = delete;

        [[nodiscard]] uint32 GetSchemaId() const noexcept;
        [[nodiscard]] uint32 GetSchemaVersion() const noexcept;
        [[nodiscard]] uint64 GetSequence() const noexcept;
        [[nodiscard]] AssetId GetAssetId() const noexcept;
        [[nodiscard]] RenderResourceHandle GetHandle() const noexcept;
        [[nodiscard]] RenderResourceKind GetKind() const noexcept;
        [[nodiscard]] uint64 GetDerivedPayloadBytes() const noexcept;
        [[nodiscard]] uint64 GetDeclaredPayloadBytes() const noexcept;
        [[nodiscard]] const ResourceUploadPayload& GetPayload() const noexcept;
        [[nodiscard]] const std::vector<RenderResourceHandle>&
            GetDependencies() const noexcept;
        [[nodiscard]] RenderDependencyReadiness
            GetDependencyReadiness() const noexcept;
        [[nodiscard]] RenderUploadPriority GetPriority() const noexcept;
        [[nodiscard]] const ResourceUploadDiagnosticProvenance&
            GetProvenance() const noexcept;

    private:
        explicit ResourceUploadRequest(ResourceUploadRequestCreateInfo&& info,
                                       uint64 derivedPayloadBytes);

        uint32 m_schemaId = RVX_RESOURCE_UPLOAD_REQUEST_SCHEMA_ID;
        uint32 m_schemaVersion = RVX_RESOURCE_UPLOAD_REQUEST_SCHEMA_VERSION;
        uint64 m_sequence = 0;
        AssetId m_assetId{};
        RenderResourceHandle m_handle{};
        RenderResourceKind m_kind = RenderResourceKind::Invalid;
        ResourceUploadPayload m_payload{MeshUploadPayload{}};
        std::vector<RenderResourceHandle> m_dependencies{};
        RenderDependencyReadiness m_dependencyReadiness =
            RenderDependencyReadiness::RequireAll;
        uint64 m_derivedPayloadBytes = 0;
        uint64 m_declaredPayloadBytes = 0;
        RenderUploadPriority m_priority = RenderUploadPriority::Normal;
        ResourceUploadDiagnosticProvenance m_provenance{};
    };

    static_assert(static_cast<uint8>(MeshUploadIndexType::UInt8) == 0);
    static_assert(static_cast<uint8>(MeshUploadIndexType::UInt16) == 1);
    static_assert(static_cast<uint8>(MeshUploadIndexType::UInt32) == 2);
    static_assert(static_cast<uint8>(MeshUploadPrimitiveTopology::Triangles) == 0);
    static_assert(static_cast<uint8>(MeshUploadPrimitiveTopology::TriangleStrip) == 1);
    static_assert(static_cast<uint8>(MeshUploadPrimitiveTopology::TriangleFan) == 2);
    static_assert(static_cast<uint8>(MeshUploadPrimitiveTopology::Lines) == 3);
    static_assert(static_cast<uint8>(MeshUploadPrimitiveTopology::LineStrip) == 4);
    static_assert(static_cast<uint8>(MeshUploadPrimitiveTopology::LineLoop) == 5);
    static_assert(static_cast<uint8>(MeshUploadPrimitiveTopology::Points) == 6);
    static_assert(static_cast<uint8>(TextureUploadFormat::Unknown) == 0);
    static_assert(static_cast<uint8>(TextureUploadFormat::RGBA8) == 1);
    static_assert(static_cast<uint8>(TextureUploadFormat::RGBA16F) == 2);
    static_assert(static_cast<uint8>(TextureUploadFormat::RGBA32F) == 3);
    static_assert(static_cast<uint8>(TextureUploadFormat::RGB8) == 4);
    static_assert(static_cast<uint8>(TextureUploadFormat::RG8) == 5);
    static_assert(static_cast<uint8>(TextureUploadFormat::R8) == 6);
    static_assert(static_cast<uint8>(TextureUploadFormat::BC1) == 7);
    static_assert(static_cast<uint8>(TextureUploadFormat::BC3) == 8);
    static_assert(static_cast<uint8>(TextureUploadFormat::BC5) == 9);
    static_assert(static_cast<uint8>(TextureUploadFormat::BC7) == 10);
    static_assert(static_cast<uint8>(MaterialUploadTextureSlot::BaseColor) == 0);
    static_assert(static_cast<uint8>(MaterialUploadTextureSlot::Normal) == 1);
    static_assert(static_cast<uint8>(MaterialUploadTextureSlot::MetallicRoughness) == 2);
    static_assert(static_cast<uint8>(MaterialUploadTextureSlot::Occlusion) == 3);
    static_assert(static_cast<uint8>(MaterialUploadTextureSlot::Emissive) == 4);
    static_assert(static_cast<uint8>(MaterialUploadWrapMode::Repeat) == 0);
    static_assert(static_cast<uint8>(MaterialUploadWrapMode::MirrorRepeat) == 1);
    static_assert(static_cast<uint8>(MaterialUploadWrapMode::ClampToEdge) == 2);
    static_assert(static_cast<uint8>(MaterialUploadWrapMode::ClampToBorder) == 3);
    static_assert(static_cast<uint8>(MaterialUploadFilterMode::Nearest) == 0);
    static_assert(static_cast<uint8>(MaterialUploadFilterMode::Linear) == 1);
    static_assert(static_cast<uint8>(MaterialUploadFilterMode::NearestMipmapNearest) == 2);
    static_assert(static_cast<uint8>(MaterialUploadFilterMode::LinearMipmapNearest) == 3);
    static_assert(static_cast<uint8>(MaterialUploadFilterMode::NearestMipmapLinear) == 4);
    static_assert(static_cast<uint8>(MaterialUploadFilterMode::LinearMipmapLinear) == 5);
    static_assert(static_cast<uint8>(ResourceUploadRequestCreateCode::Created) == 0);
    static_assert(static_cast<uint8>(ResourceUploadRequestCreateCode::InvalidSchema) == 1);
    static_assert(static_cast<uint8>(ResourceUploadRequestCreateCode::InvalidSequence) == 2);
    static_assert(static_cast<uint8>(ResourceUploadRequestCreateCode::InvalidAsset) == 3);
    static_assert(static_cast<uint8>(ResourceUploadRequestCreateCode::InvalidHandle) == 4);
    static_assert(static_cast<uint8>(ResourceUploadRequestCreateCode::InvalidKind) == 5);
    static_assert(static_cast<uint8>(ResourceUploadRequestCreateCode::PayloadKindMismatch) == 6);
    static_assert(static_cast<uint8>(ResourceUploadRequestCreateCode::InvalidPayload) == 7);
    static_assert(static_cast<uint8>(ResourceUploadRequestCreateCode::PayloadRangeOverflow) == 8);
    static_assert(static_cast<uint8>(ResourceUploadRequestCreateCode::PayloadByteCountOverflow) == 9);
    static_assert(static_cast<uint8>(ResourceUploadRequestCreateCode::DiagnosticByteCountMismatch) == 10);
    static_assert(static_cast<uint8>(ResourceUploadRequestCreateCode::InvalidDependency) == 11);
    static_assert(static_cast<uint8>(ResourceUploadRequestCreateCode::DuplicateDependency) == 12);
    static_assert(static_cast<uint8>(ResourceUploadRequestCreateCode::SelfDependency) == 13);
} // namespace RVX
