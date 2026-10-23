#pragma once

/**
 * @file NetworkSubsystem.h
 * @brief Engine subsystem for network management
 */

#include "Core/Subsystem/EngineSubsystem.h"
#include "Networking/NetworkManager.h"
#include "Networking/Replication/ReplicatedObject.h"

namespace RVX::Net
{
    /**
     * @brief Network subsystem providing engine-level network services
     * 
     * Integrates networking into the engine's subsystem framework.
     * Handles initialization, updates, and shutdown of network systems.
     */
    class NetworkSubsystem : public EngineSubsystem
    {
    public:
        NetworkSubsystem();
        ~NetworkSubsystem() override;

        // =====================================================================
        // ISubsystem Implementation
        // =====================================================================

        const char* GetName() const override { return "NetworkSubsystem"; }
        bool ShouldTick() const override { return true; }
        
        void Initialize() override;
        void Deinitialize() override;
        void Tick(float deltaTime) override;

        // =====================================================================
        // Network Access
        // =====================================================================

        /**
         * @brief Get the network manager
         */
        NetworkManager& GetNetworkManager() { return m_networkManager; }
        const NetworkManager& GetNetworkManager() const { return m_networkManager; }

        /**
         * @brief Get the replication manager
         */
        ReplicationManager& GetReplicationManager() { return m_replicationManager; }
        const ReplicationManager& GetReplicationManager() const { return m_replicationManager; }

        // =====================================================================
        // Convenience Methods
        // =====================================================================

        /**
         * @brief Start a server on specified port
         */
        bool StartServer(uint16 port);

        /**
         * @brief Connect to a server
         */
        bool Connect(const std::string& address, uint16 port);

        /**
         * @brief Start as host (server + client)
         */
        bool StartHost(uint16 port);

        /**
         * @brief Stop networking
         */
        void Stop();

        /**
         * @brief Check if network is active
         */
        bool IsActive() const { return m_networkManager.IsActive(); }

        /**
         * @brief Check if running as server
         */
        bool IsServer() const { return m_networkManager.IsServer(); }

        /**
         * @brief Check if running as client
         */
        bool IsClient() const { return m_networkManager.IsClient(); }

        /**
         * @brief Get current network role
         */
        NetworkRole GetRole() const { return m_networkManager.GetRole(); }

    private:
        NetworkManager m_networkManager;
        ReplicationManager m_replicationManager;
    };

} // namespace RVX::Net
