#pragma once

/**
 * @file RenderDrawPacketCache.h
 * @brief Retained value-only draw packet templates owned by RenderScene.
 */

#include "Core/Types.h"
#include "Render/Renderer/RenderDrawPacket.h"

#include <array>
#include <cstddef>
#include <unordered_map>

namespace RVX
{
    inline constexpr uint32 RVX_LEGACY_MATERIAL_PASS_CONTRACT_VERSION = 1U;
    inline constexpr uint32 RVX_LEGACY_SHADER_LAYOUT_VERSION_NOT_APPLICABLE =
        0U;

    /** @brief Versions that affect the static legacy material draw contract. */
    struct RenderDrawPacketCacheVersions
    {
        uint32 passContractVersion =
            RVX_LEGACY_MATERIAL_PASS_CONTRACT_VERSION;
        uint32 shaderLayoutVersion =
            RVX_LEGACY_SHADER_LAYOUT_VERSION_NOT_APPLICABLE;

        [[nodiscard]] bool operator==(
            const RenderDrawPacketCacheVersions& other) const noexcept = default;
    };

    /** @brief Exact static values that must match before a template can be reused. */
    struct RenderDrawPacketStaticSignature
    {
        RenderResourceHandle mesh;
        RenderResourceHandle material;
        MeshUploadIndexType indexType = MeshUploadIndexType::UInt32;
        uint32 indexOffset = 0;
        uint32 indexCount = 0;
        int32 baseVertex = 0;
        MeshUploadPrimitiveTopology topology =
            MeshUploadPrimitiveTopology::Triangles;
        RenderMaterialMode materialMode = RenderMaterialMode::Opaque;
        RenderBatchFlags flags = RenderBatchFlags::None;
        RenderDrawPacketCacheVersions versions;

        [[nodiscard]] bool operator==(
            const RenderDrawPacketStaticSignature& other) const noexcept = default;
    };

    enum class RenderDrawPacketCacheResolveCode : uint8
    {
        Hit = 0,
        Miss = 1,
        DynamicBypass = 2,
        PublicationInactive = 3
    };

    enum class RenderDrawPacketCacheInvalidationReason : uint8
    {
        None = 0,
        MeshGenerationChanged = 1,
        MaterialGenerationChanged = 2,
        PassContractVersionChanged = 3,
        ShaderLayoutVersionChanged = 4,
        StaticStateChanged = 5,
        ObjectRemoved = 6,
        DynamicBypass = 7,
        Count = 8
    };

    [[nodiscard]] constexpr const char* ToName(
        RenderDrawPacketCacheResolveCode code) noexcept
    {
        switch (code)
        {
            case RenderDrawPacketCacheResolveCode::Hit:
                return "Hit";
            case RenderDrawPacketCacheResolveCode::Miss:
                return "Miss";
            case RenderDrawPacketCacheResolveCode::DynamicBypass:
                return "DynamicBypass";
            case RenderDrawPacketCacheResolveCode::PublicationInactive:
                return "PublicationInactive";
            default:
                return "Unknown";
        }
    }

    [[nodiscard]] constexpr const char* GetName(
        RenderDrawPacketCacheResolveCode code) noexcept
    {
        return ToName(code);
    }

    [[nodiscard]] constexpr const char* ToName(
        RenderDrawPacketCacheInvalidationReason reason) noexcept
    {
        switch (reason)
        {
            case RenderDrawPacketCacheInvalidationReason::None:
                return "None";
            case RenderDrawPacketCacheInvalidationReason::MeshGenerationChanged:
                return "MeshGenerationChanged";
            case RenderDrawPacketCacheInvalidationReason::MaterialGenerationChanged:
                return "MaterialGenerationChanged";
            case RenderDrawPacketCacheInvalidationReason::PassContractVersionChanged:
                return "PassContractVersionChanged";
            case RenderDrawPacketCacheInvalidationReason::ShaderLayoutVersionChanged:
                return "ShaderLayoutVersionChanged";
            case RenderDrawPacketCacheInvalidationReason::StaticStateChanged:
                return "StaticStateChanged";
            case RenderDrawPacketCacheInvalidationReason::ObjectRemoved:
                return "ObjectRemoved";
            case RenderDrawPacketCacheInvalidationReason::DynamicBypass:
                return "DynamicBypass";
            case RenderDrawPacketCacheInvalidationReason::Count:
                return "Count";
            default:
                return "Unknown";
        }
    }

    [[nodiscard]] constexpr const char* GetName(
        RenderDrawPacketCacheInvalidationReason reason) noexcept
    {
        return ToName(reason);
    }

    struct RenderDrawPacketCacheResolveResult
    {
        RenderDrawPacketCacheResolveCode code =
            RenderDrawPacketCacheResolveCode::Miss;
        RenderDrawPacketCacheInvalidationReason invalidationReason =
            RenderDrawPacketCacheInvalidationReason::None;

        [[nodiscard]] bool IsHit() const noexcept
        {
            return code == RenderDrawPacketCacheResolveCode::Hit;
        }
    };

    struct RenderDrawPacketCacheStats
    {
        uint64 resolveCount = 0;
        uint64 hitCount = 0;
        uint64 missCount = 0;
        uint64 dynamicBypassCount = 0;
        uint64 packetBuildCount = 0;
        uint64 entryCreationCount = 0;
        uint64 clearCount = 0;
        size_t entryCount = 0;
        std::array<uint64,
                   static_cast<size_t>(
                       RenderDrawPacketCacheInvalidationReason::Count)>
            invalidationCounts{};

        [[nodiscard]] uint64 GetInvalidationCount(
            RenderDrawPacketCacheInvalidationReason reason) const noexcept;
    };

    /**
     * @brief Stores no GPU state; all resource handles are copied value identities.
     *
     * Publications delimit accepted RenderScene state. Entries not observed inside
     * a completed publication are pruned when that publication ends.
     */
    class RenderDrawPacketCache
    {
    public:
        void BeginAcceptedPublication() noexcept;
        void EndAcceptedPublication();
        void Clear();

        /** @brief Read-only exact lookup used while building material draw lists. */
        [[nodiscard]] RenderDrawPacketCacheResolveResult Find(
            const MeshBatch& batch,
            const RenderDrawPacketCacheVersions& versions,
            RenderDrawPacket& outTemplate) const noexcept;

        /**
         * @brief Atomically find or build a template while publishing accepted state.
         * @param dynamicBypass Prevents reuse and removes an older entry for identity.
         */
        [[nodiscard]] RenderDrawPacketCacheResolveResult Resolve(
            const MeshBatch& batch,
            const RenderDrawPacketCacheVersions& versions,
            bool dynamicBypass,
            RenderDrawPacket& outTemplate);

        [[nodiscard]] RenderDrawPacketCacheStats GetStats() const noexcept;

        [[nodiscard]] static RenderDrawPacketStaticSignature MakeSignature(
            const MeshBatch& batch,
            const RenderDrawPacketCacheVersions& versions) noexcept;

    private:
        struct EntryKey
        {
            RenderObjectId objectId = 0;
            uint32 submeshIndex = 0;

            [[nodiscard]] bool operator==(const EntryKey& other) const noexcept = default;
        };

        struct EntryKeyHasher
        {
            [[nodiscard]] size_t operator()(const EntryKey& key) const noexcept;
        };

        struct Entry
        {
            RenderDrawPacketStaticSignature signature;
            RenderDrawPacket packetTemplate;
            uint64 lastPublishedGeneration = 0;
        };

        [[nodiscard]] RenderDrawPacketCacheResolveResult FindExact(
            const MeshBatch& batch,
            const RenderDrawPacketCacheVersions& versions,
            RenderDrawPacket& outTemplate) const noexcept;
        [[nodiscard]] static RenderDrawPacketCacheInvalidationReason
            ClassifyMismatch(const RenderDrawPacketStaticSignature& existing,
                             const RenderDrawPacketStaticSignature& requested) noexcept;
        void RecordInvalidation(
            RenderDrawPacketCacheInvalidationReason reason) noexcept;

        std::unordered_map<EntryKey, Entry, EntryKeyHasher> m_entries;
        RenderDrawPacketCacheStats m_stats;
        uint64 m_publicationGeneration = 0;
        bool m_publicationActive = false;
    };

    static_assert(static_cast<uint8>(RenderDrawPacketCacheResolveCode::Hit) == 0);
    static_assert(static_cast<uint8>(RenderDrawPacketCacheResolveCode::Miss) == 1);
    static_assert(static_cast<uint8>(
                      RenderDrawPacketCacheResolveCode::DynamicBypass) == 2);
    static_assert(static_cast<uint8>(
                      RenderDrawPacketCacheResolveCode::PublicationInactive) == 3);
    static_assert(static_cast<uint8>(RenderDrawPacketCacheInvalidationReason::None) == 0);
} // namespace RVX
