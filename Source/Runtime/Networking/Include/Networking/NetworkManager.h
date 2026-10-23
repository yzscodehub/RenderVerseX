#pragma once

/**
 * @file NetworkManager.h
 * @brief Central network management class
 * 
 * Manages connections, handles network events, and provides
 * the main API for network communication.
 */

#include "Networking/NetworkTypes.h"
#include "Networking/Connection.h"
#include "Networking/Packet.h"
#include "Networking/Transport/UDPTransport.h"
#include "Networking/Transport/ReliableUDP.h"

#include <unordered_map>
#include <vector>
#include <queue>
#include <mutex>
#include <atomic>

namespace RVX::Net
{
    /**
     * @brief Network manager configuration
     */
    struct NetworkManagerConfig
    {
        /// Network configuration
        NetworkConfig network;
        
        /// Transport configuration
        TransportConfig transport;
        
        /// Reliable layer configuration
        ReliableConfig reliable;
        
        /// Local port to bind (0 for auto)
        uint16 localPort = 0;
        
        /// Server address (for clients)
        std::string serverAddress;
        
        /// Server port (for clients)
        uint16 serverPort = 7777;
    };

    /**
     * @brief Callback for connection events
     */
    using OnConnectionCallback = std::function<void(ConnectionPtr)>;
    
    /**
     * @brief Callback for disconnection events
     */
    using OnDisconnectCallback = std::function<void(ConnectionPtr, DisconnectReason)>;
    
    /**
     * @brief Callback for received data
     */
    using OnDataCallback = std::function<void(ConnectionPtr, std::span<const uint8>, ChannelId)>;

    /**
     * @brief Central network manager
     * 
     * Handles all network operations including:
     * - Server hosting
     * - Client connections
     * - Packet sending/receiving
     * - Connection management
     */
    class NetworkManager
    {
    public:
        NetworkManager();
        ~NetworkManager();

        // =====================================================================
        // Lifecycle
        // =====================================================================

        /**
         * @brief Start as a server
         * @param port Port to listen on
         * @return true if started successfully
         */
        bool StartServer(uint16 port);

        /**
         * @brief Start as a client and connect to server
         * @param address Server address
         * @param port Server port
         * @return true if connection started
         */
        bool Connect(const std::string& address, uint16 port);

        /**
         * @brief Start as a host (server + local client)
         * @param port Port to listen on
         * @return true if started successfully
         */
        bool StartHost(uint16 port);

        /**
         * @brief Stop all networking
         */
        void Stop();

        /**
         * @brief Update network (call every frame)
         * @param deltaTime Time since last update
         */
        void Update(float32 deltaTime);

        // =====================================================================
        // State
        // =====================================================================

        /**
         * @brief Get current network role
         */
        NetworkRole GetRole() const { return m_role; }

        /**
         * @brief Check if running as server
         */
        bool IsServer() const { return m_role == NetworkRole::Server || m_role == NetworkRole::Host; }

        /**
         * @brief Check if running as client
         */
        bool IsClient() const { return m_role == NetworkRole::Client || m_role == NetworkRole::Host; }

        /**
         * @brief Check if network is active
         */
        bool IsActive() const { return m_active; }

        /**
         * @brief Get local address
         */
        NetworkAddress GetLocalAddress() const;

        // =====================================================================
        // Connections
        // =====================================================================

        /**
         * @brief Get connection by ID
         */
        ConnectionPtr GetConnection(ConnectionId id) const;

        /**
         * @brief Get server connection (client only)
         */
        ConnectionPtr GetServerConnection() const;

        /**
         * @brief Get all connections (server only)
         */
        std::vector<ConnectionPtr> GetConnections() const;

        /**
         * @brief Get connection count
         */
        uint32 GetConnectionCount() const;

        /**
         * @brief Disconnect a specific connection
         */
        void Disconnect(ConnectionId id, DisconnectReason reason = DisconnectReason::UserRequested);

        /**
         * @brief Disconnect all connections
         */
        void DisconnectAll(DisconnectReason reason = DisconnectReason::ServerShutdown);

        // =====================================================================
        // Sending
        // =====================================================================

        /**
         * @brief Send data to a connection
         */
        void Send(ConnectionId connectionId,
                  std::span<const uint8> data,
                  DeliveryMode mode = DeliveryMode::Reliable,
                  ChannelId channel = 0);

        /**
         * @brief Send data to all connections
         */
        void Broadcast(std::span<const uint8> data,
                       DeliveryMode mode = DeliveryMode::Reliable,
                       ChannelId channel = 0);

        /**
         * @brief Send packet to a connection
         */
        void SendPacket(ConnectionId connectionId,
                        const Packet& packet,
                        DeliveryMode mode = DeliveryMode::Reliable,
                        ChannelId channel = 0);

        /**
         * @brief Broadcast packet to all connections
         */
        void BroadcastPacket(const Packet& packet,
                              DeliveryMode mode = DeliveryMode::Reliable,
                              ChannelId channel = 0);

        /**
         * @brief Send to server (client only)
         */
        void SendToServer(std::span<const uint8> data,
                          DeliveryMode mode = DeliveryMode::Reliable,
                          ChannelId channel = 0);

        // =====================================================================
        // Callbacks
        // =====================================================================

        /**
         * @brief Set callback for new connections
         */
        void SetOnConnect(OnConnectionCallback callback);

        /**
         * @brief Set callback for disconnections
         */
        void SetOnDisconnect(OnDisconnectCallback callback);

        /**
         * @brief Set callback for received data
         */
        void SetOnData(OnDataCallback callback);

        // =====================================================================
        // Statistics
        // =====================================================================

        /**
         * @brief Get global network statistics
         */
        NetworkStats GetStats() const;

        /**
         * @brief Get configuration
         */
        const NetworkManagerConfig& GetConfig() const { return m_config; }

    private:
        // State
        std::atomic<bool> m_active{false};
        NetworkRole m_role = NetworkRole::None;
        NetworkManagerConfig m_config;
        
        // Transport
        std::shared_ptr<UDPTransport> m_transport;
        std::unique_ptr<ReliableUDP> m_reliable;
        
        // Connections
        std::unordered_map<ConnectionId, ConnectionPtr> m_connections;
        mutable std::mutex m_connectionsMutex;
        ConnectionId m_nextConnectionId = 1;
        ConnectionId m_serverConnectionId = RVX_NET_INVALID_CONNECTION_ID;
        
        // Address to connection mapping
        std::unordered_map<std::string, ConnectionId> m_addressToConnection;
        
        // Callbacks
        OnConnectionCallback m_onConnect;
        OnDisconnectCallback m_onDisconnect;
        OnDataCallback m_onData;
        
        // Pending connection requests (server)
        struct PendingConnection
        {
            NetworkAddress address;
            std::string clientName;
            NetworkTime requestTime;
        };
        std::queue<PendingConnection> m_pendingConnections;
        
        // Internal methods
        void ProcessTransport();
        void ProcessConnections(float32 deltaTime);
        void HandlePacket(const NetworkAddress& source, std::span<const uint8> data);
        void HandleConnectionRequest(const NetworkAddress& source, BitReader& reader);
        void HandleConnectionAccepted(const NetworkAddress& source, BitReader& reader);
        void HandleConnectionDenied(const NetworkAddress& source, BitReader& reader);
        void HandleDisconnect(const NetworkAddress& source, BitReader& reader);
        void HandlePing(const NetworkAddress& source, BitReader& reader);
        void HandlePong(const NetworkAddress& source, BitReader& reader);
        void HandleUserData(const NetworkAddress& source, std::span<const uint8> payload);
        
        ConnectionPtr CreateConnection(const NetworkAddress& address);
        void RemoveConnection(ConnectionId id, DisconnectReason reason);
        std::string AddressToKey(const NetworkAddress& address) const;
        ConnectionPtr FindConnectionByAddress(const NetworkAddress& address) const;
        void SendConnectionAccepted(const NetworkAddress& address, ConnectionId id);
        void SendConnectionDenied(const NetworkAddress& address, DisconnectReason reason);
    };

} // namespace RVX::Net
