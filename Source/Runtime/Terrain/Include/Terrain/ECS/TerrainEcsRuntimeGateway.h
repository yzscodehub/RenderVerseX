#pragma once

/**
 * @file TerrainEcsRuntimeGateway.h
 * @brief Fail-closed owner-thread terrain gateway for pure ECS bindings.
 */

#include "Terrain/ECS/TerrainEcsBridge.h"

#include <memory>
#include <string>

namespace RVX::Terrain
{
    /**
     * @brief Reports the precise unsupported terrain Resource seam.
     *
     * Terrain currently has CPU Heightmap and TerrainMaterial classes, but
     * neither is a Resource type and no owner-thread AssetId resolver exists.
     * This gateway therefore rejects creation rather than inventing heightfield
     * or material data from AssetId values. All calls must use the construction
     * thread.
     */
    class TerrainEcsRuntimeGateway final : public ITerrainEcsGateway
    {
    public:
        TerrainEcsRuntimeGateway();
        ~TerrainEcsRuntimeGateway() override;

        TerrainEcsRuntimeGateway(const TerrainEcsRuntimeGateway&) = delete;
        TerrainEcsRuntimeGateway& operator=(const TerrainEcsRuntimeGateway&) = delete;
        TerrainEcsRuntimeGateway(TerrainEcsRuntimeGateway&&) = delete;
        TerrainEcsRuntimeGateway& operator=(TerrainEcsRuntimeGateway&&) = delete;

        [[nodiscard]] TerrainEcsRuntimeHandle Create(const TerrainEcsRuntimeRequest& request) override;
        [[nodiscard]] bool Update(TerrainEcsRuntimeHandle handle,
                                  const TerrainEcsRuntimeRequest& request) override;
        [[nodiscard]] bool CaptureSnapshot(TerrainEcsRuntimeHandle handle,
                                           TerrainEcsRuntimeSnapshot& outSnapshot) const override;
        [[nodiscard]] TerrainEcsReleaseResult Release(TerrainEcsRuntimeHandle handle) override;
        [[nodiscard]] bool IsAlive(TerrainEcsRuntimeHandle handle) const override;

        /** @brief Last owner-thread rejection diagnostic, empty before the first failure. */
        [[nodiscard]] const std::string& GetLastFailureReason() const;

    private:
        struct State;
        std::unique_ptr<State> m_state;
    };
} // namespace RVX::Terrain
