#pragma once

/**
 * @file UDPTransport.h
 * @brief UDP-based transport implementation using ASIO
 */

#include "Networking/Transport/ITransport.h"

#include <queue>
#include <mutex>
#include <atomic>

// Forward declare ASIO types to avoid including in header
namespace asio
{
    class io_context;

    namespace ip
    {
        class udp;
        template<typename Protocol> class basic_endpoint;
    }
}

namespace RVX::Net
{
    /**
     * @brief UDP transport implementation
     * 
     * Uses ASIO for cross-platform UDP socket operations.
     * Supports both IPv4 and IPv6.
     */
    class UDPTransport : public ITransport
    {
    public:
        UDPTransport();
        ~UDPTransport() override;

        // =====================================================================
        // ITransport Implementation
        // =====================================================================

        TransportResult Initialize(const TransportConfig& config) override;
        void Shutdown() override;
        bool IsActive() const override;

        TransportResult SendTo(
            const NetworkAddress& address,
            std::span<const uint8> data) override;

        TransportResult ReceiveFrom(ReceivedPacket& outPacket) override;

        uint32 Poll(uint32 timeoutMs = 0) override;

        void SetPacketCallback(PacketReceivedCallback callback) override;

        NetworkAddress GetLocalAddress() const override;
        const char* GetTypeName() const override { return "UDPTransport"; }
        uint32 GetMTU() const override { return m_mtu; }

        // =====================================================================
        // UDP-specific
        // =====================================================================

        /**
         * @brief Send broadcast packet (requires enableBroadcast in config)
         */
        TransportResult Broadcast(uint16 port, std::span<const uint8> data);

        /**
         * @brief Get number of pending outgoing packets
         */
        uint32 GetPendingSendCount() const;

        /**
         * @brief Get number of received packets waiting
         */
        uint32 GetPendingReceiveCount() const;

    private:
        struct Impl;
        std::unique_ptr<Impl> m_impl;

        std::atomic<bool> m_active{false};
        uint32 m_mtu = RVX_NET_MTU;
        
        PacketReceivedCallback m_packetCallback;
        
        // Receive queue (thread-safe)
        std::queue<ReceivedPacket> m_receiveQueue;
        mutable std::mutex m_receiveMutex;
        
        void StartReceive();
        void HandleReceive(const std::error_code& error, std::size_t bytesReceived);
    };

} // namespace RVX::Net
