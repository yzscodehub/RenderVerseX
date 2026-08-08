#pragma once

/**
 * @file RenderFrameExtractor.h
 * @brief Aggregate update-side extraction into immutable frame packets.
 */

#include "Core/Types.h"
#include "RenderContracts/RenderFramePacket.h"
#include "RenderContracts/RenderFramePacketV5.h"
#include "RenderContracts/RenderSceneUpdate.h"
#include "RenderExtraction/RenderFramePacketBuilder.h"
#include "RenderExtraction/WorldCameraBridge.h"

#include <memory>
#include <optional>
#include <unordered_map>

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
        /** Compatibility shadow output; v4 remains the actual input in Phase 4. */
        std::unique_ptr<const RenderSceneUpdateBatch> sceneUpdate;
        std::unique_ptr<const RenderFramePacketV5> frameV5;

        [[nodiscard]] bool IsComplete() const noexcept
        {
            return code == RenderFrameExtractionResultCode::Complete &&
                   packet != nullptr;
        }

        [[nodiscard]] bool HasCompleteShadowOutput() const noexcept
        {
            return IsComplete() && frameV5 != nullptr;
        }
    };

    enum class RenderFramePublicationDisposition : uint8
    {
        Accepted = 0,
        NotAccepted,
        SceneUpdateAcceptedWithoutFrame,
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
        /** @brief Resolve transport ownership of the most recent candidate. */
        void ResolveLastPublication(
            RenderFramePublicationDisposition disposition) noexcept;

    private:
        struct RetainedSceneState
        {
            std::unordered_map<uint64, RenderPrimitiveSnapshot> primitives;
            std::unordered_map<uint64, RenderLightSnapshot> lights;
            std::unordered_map<uint64, RenderDecalSnapshot> decals;
            std::unordered_map<uint64, RenderProbeSnapshot> probes;
            std::unordered_map<uint64, ParticleRenderSnapshotItem> particles;
            std::unordered_map<uint64, WaterRenderSnapshotItem> water;
            std::unordered_map<uint64, TerrainRenderSnapshotItem> terrain;
            RenderSkySnapshot sky;
            RenderEnvironmentSnapshot environment;
        };

        std::optional<RetainedSceneState> m_publishedScene;
        std::optional<RetainedSceneState> m_candidateScene;
        uint64 m_lastCompletedSequence = 0;
        uint64 m_publishedSceneRevision = 0;
        uint64 m_publishedWorldRevision = 0;
        uint64 m_candidateSceneRevision = 0;
        uint64 m_candidateWorldRevision = 0;
        bool m_publicationPending = false;
        bool m_forceFullReset = true;
    };

    static_assert(
        static_cast<uint8>(RenderFrameExtractionResultCode::Complete) == 0);
} // namespace RVX
