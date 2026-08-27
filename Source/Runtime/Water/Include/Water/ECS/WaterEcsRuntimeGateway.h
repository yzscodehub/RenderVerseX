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
     * @brief Owns immutable Gerstner surface descriptors for ECS bindings.
     *
     * The gateway deliberately owns no mesh, GPU object, renderer pass,
     * auxiliary water-effect object, or accumulated simulation time. It
     * validates and publishes a complete value packet; both gameplay and
     * Render evaluate that packet against the same explicit frame time.
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
