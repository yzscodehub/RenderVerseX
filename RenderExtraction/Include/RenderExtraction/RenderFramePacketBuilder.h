#pragma once

/**
 * @file RenderFramePacketBuilder.h
 * @brief Update-thread builder for immutable complete frame packets.
 */

#include "RenderContracts/RenderFramePacket.h"

#include <memory>
#include <optional>
#include <vector>

namespace RVX
{
    enum class RenderFramePacketBuilderState : uint8
    {
        Building = 0,
        Sealed = 1
    };

    enum class RenderFrameSealCode : uint8
    {
        None = 0,
        Sealed = 1,
        AlreadySealed = 2,
        InvalidSchema = 3,
        InvalidSequence = 4,
        MissingValue = 5,
        InvalidViewport = 6,
        InvalidNumericValue = 7,
        IncompleteExtraction = 8,
        IncompleteFeatureSnapshot = 9,
        CountMismatch = 10,
        InvalidResourceReference = 11,
        InvalidCaptureRequest = 12
    };

    /** @brief Mutable packet accumulator with repairable validation failures. */
    class RenderFramePacketBuilder final
    {
    public:
        RenderFramePacketBuilder() = default;
        RenderFramePacketBuilder(const RenderFramePacketBuilder&) = delete;
        RenderFramePacketBuilder& operator=(const RenderFramePacketBuilder&) = delete;
        RenderFramePacketBuilder(RenderFramePacketBuilder&&) = delete;
        RenderFramePacketBuilder& operator=(RenderFramePacketBuilder&&) = delete;

        bool SetHeader(RenderFrameHeader header);
        bool SetView(RenderViewSnapshot view);
        bool AddPrimitive(RenderPrimitiveSnapshot primitive);
        bool AddLight(RenderLightSnapshot light);
        bool SetSky(RenderSkySnapshot sky);
        bool SetEnvironment(RenderEnvironmentSnapshot environment);
        bool SetSettings(RenderFrameSettings settings);
        bool SetCaptureRequest(RenderFrameCaptureRequest request);
        bool SetFeatures(RenderFeatureSnapshot features);
        bool SetExtractionDiagnostics(RenderExtractionDiagnostics diagnostics);

        [[nodiscard]] std::unique_ptr<const RenderFramePacket> Seal();
        [[nodiscard]] RenderFrameSealCode GetLastSealCode() const noexcept;
        [[nodiscard]] RenderFramePacketBuilderState GetState() const noexcept;

    private:
        RenderFramePacketBuilderState m_state =
            RenderFramePacketBuilderState::Building;
        RenderFrameSealCode m_lastSealCode = RenderFrameSealCode::None;
        std::optional<RenderFrameHeader> m_header{};
        std::optional<RenderViewSnapshot> m_view{};
        std::vector<RenderPrimitiveSnapshot> m_primitives{};
        std::vector<RenderLightSnapshot> m_lights{};
        std::optional<RenderSkySnapshot> m_sky{};
        std::optional<RenderEnvironmentSnapshot> m_environment{};
        std::optional<RenderFrameSettings> m_settings{};
        std::optional<RenderFrameCaptureRequest> m_captureRequest{};
        std::optional<RenderFeatureSnapshot> m_features{};
        std::optional<RenderExtractionDiagnostics> m_extractionDiagnostics{};
    };

    static_assert(static_cast<uint8>(RenderFramePacketBuilderState::Building) == 0);
    static_assert(static_cast<uint8>(RenderFramePacketBuilderState::Sealed) == 1);
    static_assert(static_cast<uint8>(RenderFrameSealCode::None) == 0);
    static_assert(static_cast<uint8>(RenderFrameSealCode::Sealed) == 1);
    static_assert(static_cast<uint8>(RenderFrameSealCode::AlreadySealed) == 2);
    static_assert(static_cast<uint8>(RenderFrameSealCode::InvalidSchema) == 3);
    static_assert(static_cast<uint8>(RenderFrameSealCode::InvalidSequence) == 4);
    static_assert(static_cast<uint8>(RenderFrameSealCode::MissingValue) == 5);
    static_assert(static_cast<uint8>(RenderFrameSealCode::InvalidViewport) == 6);
    static_assert(static_cast<uint8>(RenderFrameSealCode::InvalidNumericValue) == 7);
    static_assert(static_cast<uint8>(RenderFrameSealCode::IncompleteExtraction) == 8);
    static_assert(static_cast<uint8>(RenderFrameSealCode::IncompleteFeatureSnapshot) == 9);
    static_assert(static_cast<uint8>(RenderFrameSealCode::CountMismatch) == 10);
    static_assert(static_cast<uint8>(RenderFrameSealCode::InvalidResourceReference) == 11);
    static_assert(static_cast<uint8>(RenderFrameSealCode::InvalidCaptureRequest) == 12);
} // namespace RVX
