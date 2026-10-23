#pragma once

/**
 * @file ITransport.h
 * @brief Transport layer interface for network communication
 * 
 * Defines the abstract interface for network transports.
 * Implementations include UDP, TCP, and platform-specific transports.
 */

#include "Networking/NetworkTypes.h"

#include <functional>
#include <memory>
#include <span>

namespace RVX::Net
{
    /**
     * @brief Result of a transport operation
     */
    enum class TransportResult : uint8
    {
        Success = 0,
        WouldBlock,     ///< Non-blocking operation has no data
        Disconnected,   ///< Connection was closed
        Timeout,        ///< Operation timed out
        Error,          ///< General error
        InvalidAddress, ///< Address could not be resolved
        BindFailed,     ///< Could not bind to address
        SendFailed,     ///< Failed to send data
        ReceiveFailed   ///< Failed to receive data
    };

    /**
     * @brief Received packet information
     */
    struct ReceivedPacket
    {
        NetworkAddress source;
        std::vector<uint8> data;
        NetworkTime receiveTime;
    };

    /**
     * @brief Callback for received packets
     */
    using PacketReceivedCallback = std::function<void(const ReceivedPacket&)>;

    /**
     * @brief Transport configuration
     */
    struct TransportConfig
    {
        /// Local address to bind to (empty = any)
        std::string bindAddress;
        
        /// Local port to bind to (0 = auto)
        uint16 bindPort = 0;
        
        /// Enable non-blocking mode
        bool nonBlocking = true;
        
        /// Socket receive buffer size (0 = system default)
        uint32 receiveBufferSize = 0;
        
        /// Socket send buffer size (0 = system default)
        uint32 sendBufferSize = 0;
        
        /// Enable address reuse
        bool reuseAddress = true;
        
        /// Enable broadcast (UDP only)
        bool enableBroadcast = false;
        
        /// Enable IPv6
        bool enableIPv6 = false;
    };

    /**
     * @brief Abstract transport interface
     * 
     * Provides low-level send/receive functionality.
     * Concrete implementations handle the actual network I/O.
     */
    class ITransport
    {
    public:
        virtual ~ITransport() = default;

        // =====================================================================
        // Lifecycle
        // =====================================================================

        /**
         * @brief Initialize the transport
         * @param config Transport configuration
         * @return Success or error
         */
        virtual TransportResult Initialize(const TransportConfig& config) = 0;

        /**
         * @brief Shutdown the transport
         */
        virtual void Shutdown() = 0;

        /**
         * @brief Check if transport is active
         */
        virtual bool IsActive() const = 0;

        // =====================================================================
        // Send/Receive
        // =====================================================================

        /**
         * @brief Send data to an address
         * @param address Destination address
         * @param data Data to send
         * @return Success or error
         */
        virtual TransportResult SendTo(
            const NetworkAddress& address,
            std::span<const uint8> data) = 0;

        /**
         * @brief Receive data (non-blocking)
         * @param outPacket Received packet if available
         * @return Success, WouldBlock, or error
         */
        virtual TransportResult ReceiveFrom(ReceivedPacket& outPacket) = 0;

        /**
         * @brief Poll for events
         * @param timeoutMs Maximum time to wait (0 = no wait)
         * @return Number of events processed
         */
        virtual uint32 Poll(uint32 timeoutMs = 0) = 0;

        // =====================================================================
        // Callbacks
        // =====================================================================

        /**
         * @brief Set callback for received packets
         */
        virtual void SetPacketCallback(PacketReceivedCallback callback) = 0;

        // =====================================================================
        // Info
        // =====================================================================

        /**
         * @brief Get the local bound address
         */
        virtual NetworkAddress GetLocalAddress() const = 0;

        /**
         * @brief Get transport type name
         */
        virtual const char* GetTypeName() const = 0;

        /**
         * @brief Get MTU for this transport
         */
        virtual uint32 GetMTU() const = 0;
    };

    /**
     * @brief Shared pointer type for transport
     */
    using TransportPtr = std::shared_ptr<ITransport>;

    /**
     * @brief Create a UDP transport
     */
    TransportPtr CreateUDPTransport();

} // namespace RVX::Net
