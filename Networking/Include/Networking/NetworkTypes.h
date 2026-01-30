#pragma once

/**
 * @file NetworkTypes.h
 * @brief Core networking types and constants
 */

#include "Core/Types.h"

#include <string>
#include <chrono>
#include <array>

namespace RVX::Net
{
    // =========================================================================
    // Constants
    // =========================================================================
    
    /// Maximum transmission unit for packets
    constexpr uint32 RVX_NET_MTU = 1400;
    
    /// Maximum packet size including headers
    constexpr uint32 RVX_NET_MAX_PACKET_SIZE = 1500;
    
    /// Maximum number of channels per connection
    constexpr uint32 RVX_NET_MAX_CHANNELS = 32;
    
    /// Maximum connections per server
    constexpr uint32 RVX_NET_MAX_CONNECTIONS = 64;
    
    /// Default connection timeout in milliseconds
    constexpr uint32 RVX_NET_DEFAULT_TIMEOUT_MS = 10000;
    
    /// Default keep-alive interval in milliseconds
    constexpr uint32 RVX_NET_KEEPALIVE_INTERVAL_MS = 1000;
    
    /// Maximum pending reliable packets
    constexpr uint32 RVX_NET_MAX_PENDING_RELIABLE = 256;
    
    /// Invalid connection ID
    constexpr uint32 RVX_NET_INVALID_CONNECTION_ID = 0xFFFFFFFF;
    
    /// Protocol magic number for validation
    constexpr uint32 RVX_NET_PROTOCOL_MAGIC = 0x52565850; // "RVXP"
    
    /// Protocol version
    constexpr uint16 RVX_NET_PROTOCOL_VERSION = 1;

    // =========================================================================
    // Enumerations
    // =========================================================================

    /**
     * @brief Network role (server or client)
     */
    enum class NetworkRole : uint8
    {
        None = 0,
        Server,
        Client,
        Host    ///< Server + local client (listen server)
    };

    /**
     * @brief Connection state
     */
    enum class ConnectionState : uint8
    {
        Disconnected = 0,
        Connecting,
        Connected,
        Disconnecting,
        TimedOut,
        Failed
    };

    /**
     * @brief Packet delivery mode
     */
    enum class DeliveryMode : uint8
    {
        Unreliable = 0,         ///< Fire and forget, may be lost
        UnreliableSequenced,    ///< Unreliable, but only latest packet is processed
        Reliable,               ///< Guaranteed delivery, may arrive out of order
        ReliableSequenced,      ///< Guaranteed delivery, only latest processed
        ReliableOrdered         ///< Guaranteed delivery in order
    };

    /**
     * @brief Network channel types
     */
    enum class ChannelType : uint8
    {
        Default = 0,        ///< General purpose
        Voice,              ///< Low-latency voice chat
        Movement,           ///< Position updates (frequent, lossy OK)
        Events,             ///< Game events (reliable)
        Replication,        ///< Object replication
        Commands,           ///< Client commands to server
        Spawn               ///< Object spawn/despawn
    };

    /**
     * @brief Packet type identifiers
     */
    enum class PacketType : uint8
    {
        // System packets (0x00-0x1F)
        ConnectionRequest = 0x01,
        ConnectionAccepted = 0x02,
        ConnectionDenied = 0x03,
        Disconnect = 0x04,
        Ping = 0x05,
        Pong = 0x06,
        Ack = 0x07,
        Nack = 0x08,
        KeepAlive = 0x09,
        
        // Reliable transport (0x20-0x3F)
        ReliableData = 0x20,
        ReliableFragment = 0x21,
        ReliableAck = 0x22,
        
        // User data (0x40+)
        UserData = 0x40,
        Replication = 0x41,
        RPC = 0x42,
        Broadcast = 0x43,
        
        // Custom user packets start here
        UserPacketStart = 0x80
    };

    /**
     * @brief Disconnect reason codes
     */
    enum class DisconnectReason : uint8
    {
        None = 0,
        UserRequested,
        Timeout,
        Kicked,
        Banned,
        ServerShutdown,
        ConnectionFailed,
        InvalidProtocol,
        ServerFull,
        AuthenticationFailed
    };

    // =========================================================================
    // Basic Structures
    // =========================================================================

    /**
     * @brief Network address (IPv4/IPv6)
     */
    struct NetworkAddress
    {
        std::string host;
        uint16 port = 0;
        bool isIPv6 = false;
        
        NetworkAddress() = default;
        NetworkAddress(const std::string& h, uint16 p) : host(h), port(p) {}
        
        std::string ToString() const
        {
            if (isIPv6)
                return "[" + host + "]:" + std::to_string(port);
            return host + ":" + std::to_string(port);
        }
        
        bool operator==(const NetworkAddress& other) const
        {
            return host == other.host && port == other.port;
        }
        
        bool operator!=(const NetworkAddress& other) const
        {
            return !(*this == other);
        }
    };

    /**
     * @brief Connection ID type
     */
    using ConnectionId = uint32;

    /**
     * @brief Sequence number type (wraps around)
     */
    using SequenceNumber = uint16;

    /**
     * @brief Channel ID type
     */
    using ChannelId = uint8;

    /**
     * @brief Network time point
     */
    using NetworkTime = std::chrono::steady_clock::time_point;

    /**
     * @brief Network duration in milliseconds
     */
    using NetworkDuration = std::chrono::milliseconds;

    // =========================================================================
    // Statistics
    // =========================================================================

    /**
     * @brief Connection statistics
     */
    struct ConnectionStats
    {
        uint64 packetsSent = 0;
        uint64 packetsReceived = 0;
        uint64 packetsLost = 0;
        uint64 bytesSent = 0;
        uint64 bytesReceived = 0;
        
        float32 packetLossPercent = 0.0f;
        float32 roundTripTimeMs = 0.0f;
        float32 jitterMs = 0.0f;
        float32 bandwidthIn = 0.0f;  // bytes/sec
        float32 bandwidthOut = 0.0f; // bytes/sec
        
        NetworkTime lastPacketReceived;
        NetworkTime lastPacketSent;
    };

    /**
     * @brief Network manager statistics
     */
    struct NetworkStats
    {
        uint32 activeConnections = 0;
        uint64 totalPacketsSent = 0;
        uint64 totalPacketsReceived = 0;
        uint64 totalBytesSent = 0;
        uint64 totalBytesReceived = 0;
        float32 averageRTT = 0.0f;
        float32 averagePacketLoss = 0.0f;
    };

    // =========================================================================
    // Configuration
    // =========================================================================

    /**
     * @brief Network configuration
     */
    struct NetworkConfig
    {
        /// Maximum number of connections (server only)
        uint32 maxConnections = RVX_NET_MAX_CONNECTIONS;
        
        /// Connection timeout in milliseconds
        uint32 connectionTimeoutMs = RVX_NET_DEFAULT_TIMEOUT_MS;
        
        /// Keep-alive interval in milliseconds
        uint32 keepAliveIntervalMs = RVX_NET_KEEPALIVE_INTERVAL_MS;
        
        /// Maximum transmission unit
        uint32 mtu = RVX_NET_MTU;
        
        /// Enable packet compression
        bool enableCompression = false;
        
        /// Enable packet encryption
        bool enableEncryption = false;
        
        /// Simulate network conditions (for testing)
        bool simulateLatency = false;
        float32 simulatedLatencyMs = 0.0f;
        float32 simulatedPacketLoss = 0.0f;
        float32 simulatedJitter = 0.0f;
    };

    // =========================================================================
    // Utility Functions
    // =========================================================================

    /**
     * @brief Check if sequence a is newer than b (handles wraparound)
     */
    inline bool SequenceNewerThan(SequenceNumber a, SequenceNumber b)
    {
        return ((a > b) && (a - b <= 32768)) ||
               ((a < b) && (b - a > 32768));
    }

    /**
     * @brief Get the difference between two sequence numbers
     */
    inline int32 SequenceDiff(SequenceNumber a, SequenceNumber b)
    {
        int32 diff = static_cast<int32>(a) - static_cast<int32>(b);
        if (diff > 32768) diff -= 65536;
        if (diff < -32768) diff += 65536;
        return diff;
    }

    /**
     * @brief Convert connection state to string
     */
    inline const char* ConnectionStateToString(ConnectionState state)
    {
        switch (state)
        {
            case ConnectionState::Disconnected:   return "Disconnected";
            case ConnectionState::Connecting:     return "Connecting";
            case ConnectionState::Connected:      return "Connected";
            case ConnectionState::Disconnecting:  return "Disconnecting";
            case ConnectionState::TimedOut:       return "TimedOut";
            case ConnectionState::Failed:         return "Failed";
            default:                              return "Unknown";
        }
    }

    /**
     * @brief Convert disconnect reason to string
     */
    inline const char* DisconnectReasonToString(DisconnectReason reason)
    {
        switch (reason)
        {
            case DisconnectReason::None:                 return "None";
            case DisconnectReason::UserRequested:        return "UserRequested";
            case DisconnectReason::Timeout:              return "Timeout";
            case DisconnectReason::Kicked:               return "Kicked";
            case DisconnectReason::Banned:               return "Banned";
            case DisconnectReason::ServerShutdown:       return "ServerShutdown";
            case DisconnectReason::ConnectionFailed:     return "ConnectionFailed";
            case DisconnectReason::InvalidProtocol:      return "InvalidProtocol";
            case DisconnectReason::ServerFull:           return "ServerFull";
            case DisconnectReason::AuthenticationFailed: return "AuthenticationFailed";
            default:                                     return "Unknown";
        }
    }

} // namespace RVX::Net
