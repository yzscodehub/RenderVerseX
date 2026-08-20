#pragma once

/**
 * @file WaterEcsRuntimeGateway.h
 * @brief Owner-thread water runtime gateway for pure ECS feature bindings.
 */

#include "Water/ECS/WaterEcsBridge.h"

#include <memory>

namespace RVX::Water
{
    /**
     * @brief Owns WaterSurface, WaterSimulation, Caustics, and Underwater state.
     *
     * Water's current runtime objects are CPU/value-only feature objects. The
     * gateway creates and updates those objects on its construction thread and
     * exports only WaterRenderSnapshot values through the ECS bridge.
     */
    class WaterEcsRuntimeGateway final : public IWaterEcsGateway
    {
    public:
        WaterEcsRuntimeGateway();
        ~WaterEcsRuntimeGateway() override;

        WaterEcsRuntimeGateway(const WaterEcsRuntimeGateway&) = delete;
        WaterEcsRuntimeGateway& operator=(const WaterEcsRuntimeGateway&) = delete;
        WaterEcsRuntimeGateway(WaterEcsRuntimeGateway&&) = delete;
        WaterEcsRuntimeGateway& operator=(WaterEcsRuntimeGateway&&) = delete;

        [[nodiscard]] WaterEcsRuntimeHandle Create(const WaterEcsRuntimeRequest& request) override;
        [[nodiscard]] bool Update(WaterEcsRuntimeHandle handle,
                                  const WaterEcsRuntimeRequest& request) override;
        [[nodiscard]] bool CaptureSnapshot(WaterEcsRuntimeHandle handle,
                                           WaterEcsRuntimeSnapshot& outSnapshot) const override;
        [[nodiscard]] WaterEcsReleaseResult Release(WaterEcsRuntimeHandle handle) override;
        [[nodiscard]] bool IsAlive(WaterEcsRuntimeHandle handle) const override;

    private:
        struct State;
        std::unique_ptr<State> m_state;
    };
} // namespace RVX::Water
