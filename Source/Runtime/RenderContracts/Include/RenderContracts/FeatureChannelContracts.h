#pragma once

/**
 * @file FeatureChannelContracts.h
 * @brief Typed, versioned producer-to-render feature channel contracts.
 */

#include "Core/Types.h"

#include <cstddef>
#include <limits>
#include <tuple>

namespace RVX
{
    /** @brief Stable IDs for render feature channels. Values are serialized policy. */
    enum class RenderFeatureChannelId : uint32
    {
        Invalid = 0,
        Particle = 1,
        Water = 2,
        Terrain = 3,
    };

    // GFX17 adds an immutable effect descriptor and packet-level frame
    // cadence/resource identity. Consumers must reject the former GFX16
    // payload shape rather than interpreting absent effect fields as defaults.
    inline constexpr uint32 RVX_PARTICLE_RENDER_SNAPSHOT_SCHEMA_VERSION = 3;
    // GFX18 replaces the placeholder Water payload with a bounded, canonical
    // Gerstner descriptor. Consumers must reject the former CPU-simulation and
    // fake-FFT fields rather than inventing a compatibility fallback.
    inline constexpr uint32 RVX_WATER_RENDER_SNAPSHOT_SCHEMA_VERSION = 3;
    inline constexpr uint32 RVX_TERRAIN_RENDER_SNAPSHOT_SCHEMA_VERSION = 2;

    /** @brief Explicit generation-qualified Scene source; opaque render IDs are not decoded. */
    struct RenderFeatureSourceIdentity
    {
        uint64 sceneRuntimeId = 0;
        uint32 entitySlot = std::numeric_limits<uint32>::max();
        uint32 entityGeneration = 0;

        [[nodiscard]] constexpr bool IsValid() const noexcept
        {
            return sceneRuntimeId != 0 &&
                   entitySlot != std::numeric_limits<uint32>::max();
        }

        friend constexpr bool operator==(const RenderFeatureSourceIdentity&,
                                         const RenderFeatureSourceIdentity&) = default;
    };

    enum class RenderFeatureChannelValidationCode : uint8
    {
        Valid = 0,
        UnknownChannel,
        ChannelMismatch,
        SchemaMismatch,
        ZeroCaptureSequence,
        ZeroConfigurationRevision,
        StaleCaptureSequence,
        InvalidSource,
        SourceMismatch,
    };

    [[nodiscard]] constexpr bool IsKnownRenderFeatureChannel(
        RenderFeatureChannelId channelId) noexcept
    {
        switch (channelId)
        {
            case RenderFeatureChannelId::Particle:
            case RenderFeatureChannelId::Water:
            case RenderFeatureChannelId::Terrain: return true;
            case RenderFeatureChannelId::Invalid:
            default: return false;
        }
    }

    [[nodiscard]] constexpr uint32 GetRenderFeatureChannelSchemaVersion(
        RenderFeatureChannelId channelId) noexcept
    {
        switch (channelId)
        {
            case RenderFeatureChannelId::Particle:
                return RVX_PARTICLE_RENDER_SNAPSHOT_SCHEMA_VERSION;
            case RenderFeatureChannelId::Water:
                return RVX_WATER_RENDER_SNAPSHOT_SCHEMA_VERSION;
            case RenderFeatureChannelId::Terrain:
                return RVX_TERRAIN_RENDER_SNAPSHOT_SCHEMA_VERSION;
            case RenderFeatureChannelId::Invalid:
            default: return 0;
        }
    }

    /** @brief Header retained unchanged from the Scene producer to the Render consumer. */
    struct RenderFeatureChannelHeader
    {
        RenderFeatureChannelId channelId = RenderFeatureChannelId::Invalid;
        uint32 schemaVersion = 0;
        uint64 captureSequence = 0;
        uint64 configurationRevision = 0;
        RenderFeatureSourceIdentity source;

        friend constexpr bool operator==(const RenderFeatureChannelHeader&,
                                         const RenderFeatureChannelHeader&) = default;

        [[nodiscard]] constexpr RenderFeatureChannelValidationCode Validate(
            RenderFeatureChannelId expectedChannel,
            uint32 expectedSchemaVersion,
            uint64 lastAcceptedCaptureSequence = 0,
            const RenderFeatureSourceIdentity* expectedSource = nullptr) const noexcept
        {
            if (!IsKnownRenderFeatureChannel(channelId))
                return RenderFeatureChannelValidationCode::UnknownChannel;
            if (channelId != expectedChannel)
                return RenderFeatureChannelValidationCode::ChannelMismatch;
            if (schemaVersion != expectedSchemaVersion)
                return RenderFeatureChannelValidationCode::SchemaMismatch;
            if (captureSequence == 0)
                return RenderFeatureChannelValidationCode::ZeroCaptureSequence;
            if (configurationRevision == 0)
                return RenderFeatureChannelValidationCode::ZeroConfigurationRevision;
            if (captureSequence <= lastAcceptedCaptureSequence)
                return RenderFeatureChannelValidationCode::StaleCaptureSequence;
            if (!source.IsValid())
                return RenderFeatureChannelValidationCode::InvalidSource;
            if (expectedSource != nullptr && source != *expectedSource)
                return RenderFeatureChannelValidationCode::SourceMismatch;
            return RenderFeatureChannelValidationCode::Valid;
        }
    };

    struct ParticleRenderSnapshotItem;
    struct WaterRenderSnapshotItem;
    struct TerrainRenderSnapshotItem;

    struct ParticleRenderFeatureChannelDescriptor
    {
        static constexpr RenderFeatureChannelId ChannelId =
            RenderFeatureChannelId::Particle;
        static constexpr uint32 SchemaVersion =
            RVX_PARTICLE_RENDER_SNAPSHOT_SCHEMA_VERSION;
    };

    struct WaterRenderFeatureChannelDescriptor
    {
        static constexpr RenderFeatureChannelId ChannelId =
            RenderFeatureChannelId::Water;
        static constexpr uint32 SchemaVersion =
            RVX_WATER_RENDER_SNAPSHOT_SCHEMA_VERSION;
    };

    struct TerrainRenderFeatureChannelDescriptor
    {
        static constexpr RenderFeatureChannelId ChannelId =
            RenderFeatureChannelId::Terrain;
        static constexpr uint32 SchemaVersion =
            RVX_TERRAIN_RENDER_SNAPSHOT_SCHEMA_VERSION;
    };

    /** @brief Single compile-time catalog used by every typed feature-channel layer. */
    using RenderFeatureChannelCatalog = std::tuple<
        ParticleRenderFeatureChannelDescriptor,
        WaterRenderFeatureChannelDescriptor,
        TerrainRenderFeatureChannelDescriptor>;

    inline constexpr size_t RVX_RENDER_FEATURE_CHANNEL_COUNT =
        std::tuple_size_v<RenderFeatureChannelCatalog>;

    /**
     * @brief Adds a typed channel header without changing legacy payload field access.
     *
     * Payload fields remain public base members, so existing `item.componentId`
     * style code continues to compile while `item.header` carries provenance.
     */
    template<typename Descriptor, typename Payload>
    struct RenderFeatureChannelEnvelope : Payload
    {
        using ChannelDescriptor = Descriptor;
        using PayloadType = Payload;

        static constexpr RenderFeatureChannelId ChannelId = Descriptor::ChannelId;
        static constexpr uint32 SchemaVersion = Descriptor::SchemaVersion;

        RenderFeatureChannelHeader header{
            .channelId = ChannelId,
            .schemaVersion = SchemaVersion,
        };

        [[nodiscard]] constexpr RenderFeatureChannelValidationCode ValidateChannel(
            uint64 lastAcceptedCaptureSequence = 0) const noexcept
        {
            return header.Validate(
                ChannelId, SchemaVersion, lastAcceptedCaptureSequence);
        }

        [[nodiscard]] constexpr RenderFeatureChannelValidationCode ValidateChannel(
            const RenderFeatureSourceIdentity& expectedSource,
            uint64 lastAcceptedCaptureSequence = 0) const noexcept
        {
            return header.Validate(ChannelId,
                                   SchemaVersion,
                                   lastAcceptedCaptureSequence,
                                   &expectedSource);
        }
    };

    static_assert(static_cast<uint32>(RenderFeatureChannelId::Particle) == 1);
    static_assert(static_cast<uint32>(RenderFeatureChannelId::Water) == 2);
    static_assert(static_cast<uint32>(RenderFeatureChannelId::Terrain) == 3);
    static_assert(RVX_RENDER_FEATURE_CHANNEL_COUNT == 3);
} // namespace RVX
