#pragma once

/**
 * @file Connection.h
 * @brief Network connection management
 */

#include "Networking/NetworkTypes.h"
#include "Networking/Packet.h"
#include "Networking/Transport/ReliableUDP.h"

#include <functional>
#include <memory>
#include <queue>
#include <mutex>

namespace RVX::Net
{
    // Forward declarations
    class NetworkManager;

    /**
     * @brief Connection event types
     */
    enum class ConnectionEventType : uint8
    {
        Connected,
        Disconnected,
        ConnectionFailed,
        DataReceived
    };

    /**
     * @brief Connection event
     */
    struct ConnectionEvent
    {
        ConnectionEventType type;
        ConnectionId connectionId;
        DisconnectReason disconnectReason = DisconnectReason::None;
        std::vector<uint8> data; // For DataReceived events
    };

    /**
     * @brief Connection event callback
     */
    using ConnectionEventCallback = std::function<void(const ConnectionEvent&)>;

    /**
     * @brief Represents a network connection
     */
    class Connection
    {
    public:
        // =====================================================================
        // Construction
        // =====================================================================

        Connection(ConnectionId id, const NetworkAddress& remoteAddress);
        ~Connection();

        // =====================================================================
        // Identification
        // =====================================================================

        /**
         * @brief Get connection ID
         */
        ConnectionId GetId() const { return m_id; }

        /**
         * @brief Get remote address
         */
        const NetworkAddress& GetRemoteAddress() const { return m_remoteAddress; }

        /**
         * @brief Get connection state
         */
        ConnectionState GetState() const { return m_state; }

        /**
         * @brief Check if connected
         */
        bool IsConnected() const { return m_state == ConnectionState::Connected; }

        // =====================================================================
        // Statistics
        // =====================================================================

        /**
         * @brief Get connection statistics
         */
        const ConnectionStats& GetStats() const { return m_stats; }

        /**
         * @brief Get round-trip time in milliseconds
         */
        float32 GetRTT() const { return m_stats.roundTripTimeMs; }

        /**
         * @brief Get packet loss percentage
         */
        float32 GetPacketLoss() const { return m_stats.packetLossPercent; }

        // =====================================================================
        // Data Transfer
        // =====================================================================

        /**
         * @brief Send data over this connection
         * @param data Data to send
         * @param mode Delivery mode
         * @param channel Channel ID
         */
        void Send(std::span<const uint8> data, 
                  DeliveryMode mode = DeliveryMode::Reliable,
                  ChannelId channel = 0);

        /**
         * @brief Send a packet
         */
        void SendPacket(const Packet& packet,
                        DeliveryMode mode = DeliveryMode::Reliable,
                        ChannelId channel = 0);

        /**
         * @brief Queue data for sending (internal use)
         */
        void QueueOutgoing(std::vector<uint8> data, DeliveryMode mode, ChannelId channel);

        /**
         * @brief Queue received data (internal use)
         */
        void QueueIncoming(std::vector<uint8> data, ChannelId channel);

        /**
         * @brief Get pending outgoing data
         */
        bool PopOutgoing(std::vector<uint8>& data, DeliveryMode& mode, ChannelId& channel);

        /**
         * @brief Get received data
         */
        bool PopIncoming(std::vector<uint8>& data, ChannelId& channel);

        /**
         * @brief Check if there's pending incoming data
         */
        bool HasIncomingData() const;

        // =====================================================================
        // Connection Management (Internal)
        // =====================================================================

        /**
         * @brief Set connection state
         */
        void SetState(ConnectionState state);

        /**
         * @brief Update connection (called each frame)
         */
        void Update(float32 deltaTime);

        /**
         * @brief Handle timeout
         */
        void OnTimeout();

        /**
         * @brief Update statistics from reliable layer
         */
        void UpdateStats(const ReliableStats& reliableStats);

        /**
         * @brief Get last activity time
         */
        NetworkTime GetLastActivityTime() const { return m_lastActivityTime; }

        /**
         * @brief Mark activity
         */
        void MarkActivity();

        // =====================================================================
        // User Data
        // =====================================================================

        /**
         * @brief Set user data pointer
         */
        void SetUserData(void* data) { m_userData = data; }

        /**
         * @brief Get user data pointer
         */
        void* GetUserData() const { return m_userData; }

        /**
         * @brief Set client name (for display)
         */
        void SetClientName(const std::string& name) { m_clientName = name; }

        /**
         * @brief Get client name
         */
        const std::string& GetClientName() const { return m_clientName; }

    private:
        ConnectionId m_id;
        NetworkAddress m_remoteAddress;
        ConnectionState m_state = ConnectionState::Disconnected;
        ConnectionStats m_stats;
        
        // Activity tracking
        NetworkTime m_connectionTime;
        NetworkTime m_lastActivityTime;
        float32 m_timeSinceLastPing = 0.0f;
        uint32 m_pingSequence = 0;
        
        // User data
        void* m_userData = nullptr;
        std::string m_clientName;
        
        // Data queues
        struct OutgoingData
        {
            std::vector<uint8> data;
            DeliveryMode mode;
            ChannelId channel;
        };
        
        struct IncomingData
        {
            std::vector<uint8> data;
            ChannelId channel;
        };
        
        std::queue<OutgoingData> m_outgoingQueue;
        std::queue<IncomingData> m_incomingQueue;
        mutable std::mutex m_outgoingMutex;
        mutable std::mutex m_incomingMutex;
    };

    /**
     * @brief Shared pointer type for connections
     */
    using ConnectionPtr = std::shared_ptr<Connection>;

} // namespace RVX::Net
