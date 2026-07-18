#pragma once

/**
 * @file RenderFrameExtractor.h
 * @brief Aggregate update-side extraction into immutable frame packets.
 */

#include "Core/Types.h"
#include "RenderContracts/RenderFramePacket.h"
#include "RenderExtraction/RenderFramePacketBuilder.h"
#include "RenderExtraction/WorldCameraBridge.h"

#include <memory>

namespace RVX
{
    class World;

    namespace Resource
    {
        class ResourceSubsystem;
    }

    enum class RenderFrameExtractionResultCode : uint8
    {
        Complete = 0,
        NullWorld,
        MissingResourceSubsystem,
        MissingCamera,
        NonMonotonicSequence,
        InvalidSettings,
        InvalidCaptureRequest,
        ProxyExtractionFailed,
        FeatureExtractionFailed,
        RequiredResourceUnresolved,
        SealFailed
    };

    struct RenderFrameExtractionInput
    {
        uint64 sequence = 0;
        uint64 worldRevision = 0;
        uint64 temporalEpoch = 0;
        bool explicitDiscontinuity = false;
        RenderViewExtractionParameters view;
        RenderFrameSettings settings;
        RenderFrameCaptureRequest captureRequest;
        World* world = nullptr;
        Resource::ResourceSubsystem* resources = nullptr;
    };

    struct RenderFrameExtractionResult
    {
        RenderFrameExtractionResultCode code =
            RenderFrameExtractionResultCode::SealFailed;
        RenderFrameSealCode sealCode = RenderFrameSealCode::None;
        RenderExtractionDiagnostics diagnostics;
        std::unique_ptr<const RenderFramePacket> packet;

        [[nodiscard]] bool IsComplete() const noexcept
        {
            return code == RenderFrameExtractionResultCode::Complete &&
                   packet != nullptr;
        }
    };

    /** @brief Produces complete packets and never publishes partial output. */
    class RenderFrameExtractor final
    {
    public:
        RenderFrameExtractor() = default;
        RenderFrameExtractor(const RenderFrameExtractor&) = delete;
        RenderFrameExtractor& operator=(const RenderFrameExtractor&) = delete;

        [[nodiscard]] RenderFrameExtractionResult Extract(
            const RenderFrameExtractionInput& input);
        [[nodiscard]] uint64 GetLastCompletedSequence() const noexcept
        {
            return m_lastCompletedSequence;
        }

    private:
        uint64 m_lastCompletedSequence = 0;
    };

    static_assert(
        static_cast<uint8>(RenderFrameExtractionResultCode::Complete) == 0);
} // namespace RVX
