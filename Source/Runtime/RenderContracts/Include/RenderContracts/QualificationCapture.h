#pragma once

/**
 * @file QualificationCapture.h
 * @brief Immutable, backend-neutral visual-qualification capture contracts.
 *
 * A qualification capture is deliberately a complete bundle rather than a
 * collection of unrelated screenshots.  Every attachment is tied to the same
 * graph recording, frame, view, scene revision, and renderer configuration so
 * visual metrics cannot accidentally compare surfaces from different frames.
 */

#include "Core/Hash/SHA256.h"
#include "RenderContracts/RenderFrameTypes.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace RVX
{
    inline constexpr uint32 RVX_QUALIFICATION_CAPTURE_SCHEMA_VERSION = 1;

    /** @brief The fixed surface set required by a GFX24 visual capture. */
    enum class QualificationCaptureAttachmentSemantic : uint8
    {
        BeautyLDR = 0,
        PreToneHDR,
        LinearDepth,
        Normal,
        Velocity,
        AmbientOcclusion,
        ReactiveMask,
        Count,
    };

    inline constexpr size_t RVX_QUALIFICATION_CAPTURE_ATTACHMENT_COUNT =
        static_cast<size_t>(QualificationCaptureAttachmentSemantic::Count);

    [[nodiscard]] inline bool IsQualificationCaptureReceiptDigestValid(
        const Hash::SHA256Digest& digest) noexcept
    {
        for (const uint8 value : digest)
        {
            if (value != 0)
                return true;
        }
        return false;
    }

    /**
     * @brief Canonical CPU payload layout published to qualification tooling.
     *
     * GPU sources may use backend-native compact formats.  The completion-owned
     * mapper must explicitly convert them to these finite, top-left-origin
     * float32 layouts before a bundle can be published.
     */
    enum class QualificationCapturePixelFormat : uint8
    {
        Unknown = 0,
        R32Float,
        RG32Float,
        RGBA32Float,
    };

    /** @brief Semantic color/value interpretation; it is never inferred from a name. */
    enum class QualificationCaptureColorDomain : uint8
    {
        Unknown = 0,
        DisplayReferredLDR,
        SceneLinearHDR,
        LinearDepth,
        WorldNormal,
        MotionVector,
        AmbientOcclusion,
        ReactiveMask,
    };

    [[nodiscard]] constexpr bool IsQualificationCaptureSemantic(
        QualificationCaptureAttachmentSemantic semantic) noexcept
    {
        return static_cast<uint8>(semantic) <
            static_cast<uint8>(QualificationCaptureAttachmentSemantic::Count);
    }

    [[nodiscard]] constexpr bool IsQualificationCapturePixelFormat(
        QualificationCapturePixelFormat format) noexcept
    {
        return format != QualificationCapturePixelFormat::Unknown;
    }

    [[nodiscard]] constexpr bool IsQualificationCaptureColorDomain(
        QualificationCaptureColorDomain domain) noexcept
    {
        return domain != QualificationCaptureColorDomain::Unknown;
    }

    /** @brief Format and value-domain policy for one required semantic surface. */
    struct QualificationCaptureAttachmentContract
    {
        QualificationCaptureAttachmentSemantic semantic =
            QualificationCaptureAttachmentSemantic::Count;
        QualificationCapturePixelFormat format =
            QualificationCapturePixelFormat::Unknown;
        QualificationCaptureColorDomain colorDomain =
            QualificationCaptureColorDomain::Unknown;

        [[nodiscard]] constexpr bool IsValid() const noexcept
        {
            return IsQualificationCaptureSemantic(semantic) &&
                   IsQualificationCapturePixelFormat(format) &&
                   IsQualificationCaptureColorDomain(colorDomain);
        }

        friend constexpr bool operator==(
            const QualificationCaptureAttachmentContract&,
            const QualificationCaptureAttachmentContract&) = default;
    };

    [[nodiscard]] constexpr QualificationCaptureAttachmentContract
    GetQualificationCaptureAttachmentContract(
        QualificationCaptureAttachmentSemantic semantic) noexcept
    {
        switch (semantic)
        {
            case QualificationCaptureAttachmentSemantic::BeautyLDR:
                return {semantic, QualificationCapturePixelFormat::RGBA32Float,
                        QualificationCaptureColorDomain::DisplayReferredLDR};
            case QualificationCaptureAttachmentSemantic::PreToneHDR:
                return {semantic, QualificationCapturePixelFormat::RGBA32Float,
                        QualificationCaptureColorDomain::SceneLinearHDR};
            case QualificationCaptureAttachmentSemantic::LinearDepth:
                return {semantic, QualificationCapturePixelFormat::R32Float,
                        QualificationCaptureColorDomain::LinearDepth};
            case QualificationCaptureAttachmentSemantic::Normal:
                return {semantic, QualificationCapturePixelFormat::RGBA32Float,
                        QualificationCaptureColorDomain::WorldNormal};
            case QualificationCaptureAttachmentSemantic::Velocity:
                return {semantic, QualificationCapturePixelFormat::RG32Float,
                        QualificationCaptureColorDomain::MotionVector};
            case QualificationCaptureAttachmentSemantic::AmbientOcclusion:
                return {semantic, QualificationCapturePixelFormat::R32Float,
                        QualificationCaptureColorDomain::AmbientOcclusion};
            case QualificationCaptureAttachmentSemantic::ReactiveMask:
                return {semantic, QualificationCapturePixelFormat::R32Float,
                        QualificationCaptureColorDomain::ReactiveMask};
            case QualificationCaptureAttachmentSemantic::Count: break;
        }
        return {};
    }

    /** @brief Immutable graph/execution proof shared by every attachment in one bundle. */
    struct QualificationCaptureGraphProvenance
    {
        uint64 graphIdentity = 0;
        uint64 graphRecordingGeneration = 0;
        uint64 graphRecordEpoch = 0;
        /** SHA-256 of every semantic field in the accepted exact receipt. */
        Hash::SHA256Digest executionReceiptDigest{};

        [[nodiscard]] constexpr bool IsRecordable() const noexcept
        {
            return graphIdentity != 0 && graphRecordingGeneration != 0 &&
                   graphRecordEpoch != 0;
        }

        [[nodiscard]] bool IsValid() const noexcept
        {
            return IsRecordable() &&
                   IsQualificationCaptureReceiptDigestValid(
                       executionReceiptDigest);
        }

        friend constexpr bool operator==(
            const QualificationCaptureGraphProvenance&,
            const QualificationCaptureGraphProvenance&) = default;
    };

    /** @brief Exact logical owner of all surfaces in a qualification bundle. */
    struct QualificationCaptureBundleIdentity
    {
        uint64 bundleId = 0;
        RenderFrameIdentity frameIdentity{};
        RenderViewIdentity viewIdentity{};
        QualificationCaptureGraphProvenance graphProvenance{};

        [[nodiscard]] constexpr bool IsRecordable() const noexcept
        {
            return bundleId != 0 && frameIdentity.IsValid() &&
                   viewIdentity.IsValid() && graphProvenance.IsRecordable();
        }

        [[nodiscard]] bool IsValid() const noexcept
        {
            return IsRecordable() && graphProvenance.IsValid();
        }

        friend constexpr bool operator==(
            const QualificationCaptureBundleIdentity&,
            const QualificationCaptureBundleIdentity&) = default;
    };

    /**
     * @brief Graph-local source proof for one copied surface.
     *
     * The resource and producer identities are opaque to consumers.  They
     * prevent a valid bundle from silently mixing a stale physical resource
     * into a newer graph recording.
     */
    struct QualificationCaptureAttachmentProvenance
    {
        /** Exact graph recording which exposed this logical resource. */
        uint64 graphIdentity = 0;
        uint64 graphRecordingGeneration = 0;
        uint64 graphRecordEpoch = 0;
        uint64 graphResourceIdentity = 0;
        uint64 graphResourceGeneration = 0;
        uint64 producerIdentity = 0;

        [[nodiscard]] constexpr bool IsValid() const noexcept
        {
            return graphIdentity != 0 && graphRecordingGeneration != 0 &&
                   graphRecordEpoch != 0 && graphResourceIdentity != 0 &&
                   graphResourceGeneration != 0 && producerIdentity != 0;
        }

        friend constexpr bool operator==(
            const QualificationCaptureAttachmentProvenance&,
            const QualificationCaptureAttachmentProvenance&) = default;
    };

    /** @brief Metadata frozen when the graph records the copy for one surface. */
    struct QualificationCaptureAttachmentDescriptor
    {
        QualificationCaptureAttachmentContract contract{};
        RenderOutputExtent extent{};
        uint32 rowPitchBytes = 0;
        QualificationCaptureAttachmentProvenance provenance{};

        [[nodiscard]] constexpr bool IsValid() const noexcept
        {
            return contract.IsValid() && extent.IsValid() && rowPitchBytes != 0 &&
                   provenance.IsValid();
        }

        friend constexpr bool operator==(
            const QualificationCaptureAttachmentDescriptor&,
            const QualificationCaptureAttachmentDescriptor&) = default;
    };

    /**
     * @brief CPU-owned result returned only after the source completion closes.
     *
     * The owner recomputes payloadHash from bytes.  A readback implementation
     * supplies its own claimed hash so a corruption or stale-map bug cannot be
     * promoted simply by changing metadata.
     */
    struct QualificationCaptureMappedPayload
    {
        QualificationCaptureBundleIdentity identity{};
        QualificationCaptureAttachmentSemantic semantic =
            QualificationCaptureAttachmentSemantic::Count;
        QualificationCaptureAttachmentProvenance provenance{};
        Hash::SHA256Digest claimedPayloadHash{};
        std::vector<uint8> bytes{};
    };

    /** @brief Immutable completed attachment retained by the capture owner. */
    struct QualificationCaptureAttachment
    {
        QualificationCaptureAttachmentDescriptor descriptor{};
        Hash::SHA256Digest payloadHash{};
        std::vector<uint8> bytes{};
    };

    /** @brief Complete, same-frame visual evidence bundle exposed to validators. */
    struct QualificationCaptureBundle
    {
        uint32 schemaVersion = RVX_QUALIFICATION_CAPTURE_SCHEMA_VERSION;
        QualificationCaptureBundleIdentity identity{};
        std::array<QualificationCaptureAttachment,
                   RVX_QUALIFICATION_CAPTURE_ATTACHMENT_COUNT> attachments{};
    };
} // namespace RVX
