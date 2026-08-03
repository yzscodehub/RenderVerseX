#include "RenderExtraction/RenderFramePacketBuilder.h"

#include "RenderContracts/RenderFrameValidation.h"

#include <cmath>
#include <limits>
#include <utility>

namespace RVX
{
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

    bool IsFinite(const Mat4& value)
    {
        for (uint32 column = 0; column < 4; ++column)
        {
            if (!IsFinite(value[column]))
            {
                return false;
            }
        }
        return true;
    }

    bool IsOrdered(const Vec3& minimum, const Vec3& maximum)
    {
        return minimum.x <= maximum.x && minimum.y <= maximum.y &&
               minimum.z <= maximum.z;
    }

    bool HasValidViewNumerics(const RenderViewSnapshot& view)
    {
        return IsFinite(view.viewMatrix) && IsFinite(view.projectionMatrix) &&
               IsFinite(view.viewProjectionMatrix) &&
               IsFinite(view.inverseViewProjectionMatrix) &&
               IsFinite(view.cameraPosition) && IsFinite(view.cameraDirection) &&
               IsFinite(view.cameraUp) && IsFinite(view.nearPlane) &&
               IsFinite(view.farPlane) && IsFinite(view.absoluteTime) &&
               IsFinite(view.deltaTime) && IsFinite(view.exposure) &&
               view.nearPlane > 0.0f && view.farPlane > view.nearPlane &&
               view.deltaTime >= 0.0f && view.exposure > 0.0f;
    }

    bool HasValidPrimitiveNumerics(const RenderPrimitiveSnapshot& primitive)
    {
        if (!IsFinite(primitive.worldTransform) ||
            !IsFinite(primitive.previousWorldTransform) ||
            !IsFinite(primitive.boundsMin) || !IsFinite(primitive.boundsMax) ||
            !IsOrdered(primitive.boundsMin, primitive.boundsMax))
        {
            return false;
        }
        for (const Mat4& skinMatrix : primitive.skinMatrices)
        {
            if (!IsFinite(skinMatrix))
            {
                return false;
            }
        }
        return true;
    }

    bool HasValidLightNumerics(const RenderLightSnapshot& light)
    {
        return IsFinite(light.position) && IsFinite(light.direction) &&
               IsFinite(light.color) && IsFinite(light.intensity) &&
               IsFinite(light.range) && IsFinite(light.innerConeRadians) &&
               IsFinite(light.outerConeRadians);
    }

    bool IsDeclared(RenderSkyMode mode)
    {
        return mode == RenderSkyMode::Disabled ||
               mode == RenderSkyMode::Cubemap ||
               mode == RenderSkyMode::Equirectangular ||
               mode == RenderSkyMode::Procedural ||
               mode == RenderSkyMode::SolidColor;
    }

    bool HasValidSkyNumerics(const RenderSkySnapshot& sky)
    {
        return IsDeclared(sky.mode) && IsFinite(sky.tint) &&
               IsFinite(sky.sunDirection) && IsFinite(sky.sunColor) &&
               IsFinite(sky.zenithColor) && IsFinite(sky.horizonColor) &&
               IsFinite(sky.groundColor) && IsFinite(sky.intensity) &&
               IsFinite(sky.rotationRadians) && IsFinite(sky.blurLevel) &&
               IsFinite(sky.scatteringIntensity);
    }

    bool HasValidNumerics(
        const RenderViewSnapshot& view,
        const std::vector<RenderPrimitiveSnapshot>& primitives,
        const std::vector<RenderLightSnapshot>& lights,
        const RenderSkySnapshot& sky,
        const RenderEnvironmentSnapshot& environment,
        const RenderFrameSettings& settings)
    {
        if (!HasValidViewNumerics(view) || !HasValidSkyNumerics(sky) ||
            !IsFinite(environment.intensity) ||
            !IsValidRenderFrameSettings(settings))
        {
            return false;
        }
        for (const RenderPrimitiveSnapshot& primitive : primitives)
        {
            if (!HasValidPrimitiveNumerics(primitive))
            {
                return false;
            }
        }
        for (const RenderLightSnapshot& light : lights)
        {
            if (!HasValidLightNumerics(light))
            {
                return false;
            }
        }
        return true;
    }

    bool IsComplete(const RenderExtractionDiagnostics& diagnostics)
    {
        return diagnostics.code == RenderExtractionCode::Complete &&
               diagnostics.complete && diagnostics.skippedPrimitiveCount == 0 &&
               diagnostics.skippedLightCount == 0 &&
               diagnostics.skippedFeatureProviderCount == 0;
    }

    bool IsComplete(const RenderFeatureSnapshot& features, uint64 sequence)
    {
        return
            features.metadata.schemaVersion ==
                RVX_RENDER_FEATURE_SNAPSHOT_SCHEMA_VERSION &&
            features.metadata.sequence == sequence &&
            features.metadata.status == RenderFeatureSnapshotStatus::Complete &&
            features.metadata.complete &&
            features.particles.metadata.schemaVersion ==
                RVX_PARTICLE_RENDER_SNAPSHOT_SCHEMA_VERSION &&
            features.particles.metadata.sequence == sequence &&
            features.particles.metadata.status ==
                ParticleRenderSnapshotStatus::Complete &&
            features.particles.metadata.complete &&
            features.water.metadata.schemaVersion ==
                RVX_WATER_RENDER_SNAPSHOT_SCHEMA_VERSION &&
            features.water.metadata.sequence == sequence &&
            features.water.metadata.status == WaterRenderSnapshotStatus::Complete &&
            features.water.metadata.complete &&
            features.terrain.metadata.schemaVersion ==
                RVX_TERRAIN_RENDER_SNAPSHOT_SCHEMA_VERSION &&
            features.terrain.metadata.sequence == sequence &&
            features.terrain.metadata.status ==
                TerrainRenderSnapshotStatus::Complete &&
            features.terrain.metadata.complete;
    }

    bool MatchesSize(uint32 expected, size_t actual)
    {
        return actual <= std::numeric_limits<uint32>::max() &&
               expected == static_cast<uint32>(actual);
    }

    bool MatchesUint32(size_t actual, uint32 expected)
    {
        return actual <= std::numeric_limits<uint32>::max() &&
               static_cast<uint32>(actual) == expected;
    }

    bool CountsMatch(
        const RenderFrameHeader& header,
        const std::vector<RenderPrimitiveSnapshot>& primitives,
        const std::vector<RenderLightSnapshot>& lights,
        const RenderFeatureSnapshot& features)
    {
        if (!MatchesSize(header.expectedPrimitiveCount, primitives.size()) ||
            !MatchesSize(header.extractedPrimitiveCount, primitives.size()) ||
            !MatchesSize(header.expectedLightCount, lights.size()) ||
            !MatchesSize(header.extractedLightCount, lights.size()) ||
            !MatchesUint32(features.metadata.providerCount,
                           header.expectedFeatureProviderCount) ||
            !MatchesUint32(features.metadata.providerCount,
                           header.extractedFeatureProviderCount) ||
            features.metadata.skippedProviderCount != 0 ||
            features.particles.metadata.skippedInstanceCount != 0 ||
            !features.particles.skippedReasons.empty())
        {
            return false;
        }

        return
            features.metadata.particleItemCount == features.particles.items.size() &&
            features.metadata.waterItemCount == features.water.items.size() &&
            features.metadata.terrainItemCount == features.terrain.items.size() &&
            features.particles.metadata.itemCount == features.particles.items.size() &&
            features.water.metadata.itemCount == features.water.items.size() &&
            features.terrain.metadata.itemCount == features.terrain.items.size();
    }

    bool IsDeclared(RenderLightType type)
    {
        return type == RenderLightType::Directional ||
               type == RenderLightType::Point || type == RenderLightType::Spot;
    }

    bool HasValidResourceReferences(
        const std::vector<RenderPrimitiveSnapshot>& primitives,
        const std::vector<RenderLightSnapshot>& lights,
        const RenderEnvironmentSnapshot& environment)
    {
        for (const RenderPrimitiveSnapshot& primitive : primitives)
        {
            if (primitive.objectId == 0 || !primitive.mesh.IsValid())
            {
                return false;
            }
            for (size_t index = 0; index < primitive.submeshes.size(); ++index)
            {
                const RenderSubmeshMaterialBinding& binding =
                    primitive.submeshes[index];
                const bool validMode =
                    binding.materialMode == RenderMaterialMode::Opaque ||
                    binding.materialMode == RenderMaterialMode::Masked ||
                    binding.materialMode == RenderMaterialMode::Transparent;
                if (binding.submeshIndex != index || !validMode)
                {
                    return false;
                }
            }
        }
        for (const RenderLightSnapshot& light : lights)
        {
            if (light.lightId == 0 || !IsDeclared(light.type) ||
                (light.shadowResource.IsValid() && !light.castsShadows))
            {
                return false;
            }
        }

        const uint32 validEnvironmentHandleCount =
            static_cast<uint32>(environment.irradianceTexture.IsValid()) +
            static_cast<uint32>(environment.prefilteredTexture.IsValid()) +
            static_cast<uint32>(environment.brdfLutTexture.IsValid());
        return validEnvironmentHandleCount == 0 ||
               validEnvironmentHandleCount == 3;
    }

} // namespace

bool RenderFramePacketBuilder::SetHeader(RenderFrameHeader header)
{
    if (m_state != RenderFramePacketBuilderState::Building)
    {
        return false;
    }
    m_header = std::move(header);
    return true;
}

bool RenderFramePacketBuilder::SetView(RenderViewSnapshot view)
{
    if (m_state != RenderFramePacketBuilderState::Building)
    {
        return false;
    }
    m_view = std::move(view);
    return true;
}

bool RenderFramePacketBuilder::AddPrimitive(RenderPrimitiveSnapshot primitive)
{
    if (m_state != RenderFramePacketBuilderState::Building)
    {
        return false;
    }
    m_primitives.push_back(std::move(primitive));
    return true;
}

bool RenderFramePacketBuilder::AddLight(RenderLightSnapshot light)
{
    if (m_state != RenderFramePacketBuilderState::Building)
    {
        return false;
    }
    m_lights.push_back(std::move(light));
    return true;
}

bool RenderFramePacketBuilder::SetSky(RenderSkySnapshot sky)
{
    if (m_state != RenderFramePacketBuilderState::Building)
    {
        return false;
    }
    m_sky = std::move(sky);
    return true;
}

bool RenderFramePacketBuilder::SetEnvironment(RenderEnvironmentSnapshot environment)
{
    if (m_state != RenderFramePacketBuilderState::Building)
    {
        return false;
    }
    m_environment = std::move(environment);
    return true;
}

bool RenderFramePacketBuilder::SetSettings(RenderFrameSettings settings)
{
    if (m_state != RenderFramePacketBuilderState::Building)
    {
        return false;
    }
    m_settings = std::move(settings);
    return true;
}

bool RenderFramePacketBuilder::SetCaptureRequest(RenderFrameCaptureRequest request)
{
    if (m_state != RenderFramePacketBuilderState::Building)
    {
        return false;
    }
    m_captureRequest = std::move(request);
    return true;
}

bool RenderFramePacketBuilder::SetFeatures(RenderFeatureSnapshot features)
{
    if (m_state != RenderFramePacketBuilderState::Building)
    {
        return false;
    }
    m_features = std::move(features);
    return true;
}

bool RenderFramePacketBuilder::SetExtractionDiagnostics(
    RenderExtractionDiagnostics diagnostics)
{
    if (m_state != RenderFramePacketBuilderState::Building)
    {
        return false;
    }
    m_extractionDiagnostics = std::move(diagnostics);
    return true;
}

std::unique_ptr<const RenderFramePacket> RenderFramePacketBuilder::Seal()
{
    auto fail = [this](RenderFrameSealCode code)
    {
        m_lastSealCode = code;
        return std::unique_ptr<const RenderFramePacket>{};
    };

    if (m_state == RenderFramePacketBuilderState::Sealed)
    {
        return fail(RenderFrameSealCode::AlreadySealed);
    }
    if (!m_header || !m_view || !m_sky || !m_environment || !m_settings ||
        !m_captureRequest || !m_features || !m_extractionDiagnostics)
    {
        return fail(RenderFrameSealCode::MissingValue);
    }
    if (m_header->schemaId != RVX_RENDER_FRAME_PACKET_SCHEMA_ID ||
        m_header->schemaVersion != RVX_RENDER_FRAME_PACKET_SCHEMA_VERSION)
    {
        return fail(RenderFrameSealCode::InvalidSchema);
    }
    if (m_header->sequence == 0)
    {
        return fail(RenderFrameSealCode::InvalidSequence);
    }
    if (m_view->viewportWidth == 0 || m_view->viewportHeight == 0)
    {
        return fail(RenderFrameSealCode::InvalidViewport);
    }
    if (!HasValidNumerics(*m_view, m_primitives, m_lights, *m_sky,
                          *m_environment, *m_settings))
    {
        return fail(RenderFrameSealCode::InvalidNumericValue);
    }
    if (!IsComplete(*m_extractionDiagnostics))
    {
        return fail(RenderFrameSealCode::IncompleteExtraction);
    }
    if (!IsComplete(*m_features, m_header->sequence))
    {
        return fail(RenderFrameSealCode::IncompleteFeatureSnapshot);
    }
    if (!CountsMatch(*m_header, m_primitives, m_lights, *m_features))
    {
        return fail(RenderFrameSealCode::CountMismatch);
    }
    if (!HasValidResourceReferences(m_primitives, m_lights, *m_environment))
    {
        return fail(RenderFrameSealCode::InvalidResourceReference);
    }
    if (!IsValidRenderFrameCaptureRequest(*m_captureRequest))
    {
        return fail(RenderFrameSealCode::InvalidCaptureRequest);
    }

    std::unique_ptr<const RenderFramePacket> packet(
        new RenderFramePacket(
            std::move(*m_header),
            std::move(*m_view),
            std::move(m_primitives),
            std::move(m_lights),
            std::move(*m_sky),
            std::move(*m_environment),
            std::move(*m_settings),
            std::move(*m_captureRequest),
            std::move(*m_features),
            std::move(*m_extractionDiagnostics)));
    m_state = RenderFramePacketBuilderState::Sealed;
    m_lastSealCode = RenderFrameSealCode::Sealed;
    return packet;
}

RenderFrameSealCode RenderFramePacketBuilder::GetLastSealCode() const noexcept
{
    return m_lastSealCode;
}

RenderFramePacketBuilderState RenderFramePacketBuilder::GetState() const noexcept
{
    return m_state;
}
} // namespace RVX
