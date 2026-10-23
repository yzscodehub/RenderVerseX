#pragma once

/**
 * @file ReliableUDP.h
 * @brief Reliable delivery layer on top of UDP
 * 
 * Provides:
 * - Reliable delivery with acknowledgments
 * - Ordered delivery
 * - Fragmentation for large packets
 * - Congestion control
 */

#include "Networking/Transport/ITransport.h"
#include "Networking/Serialization/BitStream.h"

#include <map>
#include <queue>
#include <deque>
#include <mutex>
#include <memory>

namespace RVX::Net
{
    // Forward declarations
    class UDPTransport;

    /**
     * @brief Reliable packet header
     */
    struct ReliableHeader
    {
        PacketType type = PacketType::UserData;
        SequenceNumber sequence = 0;
        SequenceNumber ack = 0;
        uint32 ackBits = 0;  ///< Bitfield for 32 previous sequences
        ChannelId channel = 0;
        uint16 fragmentIndex = 0;
        uint16 fragmentCount = 1;
        
        static constexpr uint32 kHeaderSize = 12; // bytes
        
        void Serialize(BitWriter& writer) const;
        void Deserialize(BitReader& reader);
    };

    /**
     * @brief Pending reliable packet
     */
    struct PendingPacket
    {
        SequenceNumber sequence = 0;
        std::vector<uint8> data;
        NetworkTime sendTime;
        NetworkTime lastResendTime;
        uint32 resendCount = 0;
        DeliveryMode mode = DeliveryMode::Reliable;
        ChannelId channel = 0;
    };

    /**
     * @brief Statistics for reliable layer
     */
    struct ReliableStats
    {
        uint64 packetsSent = 0;
        uint64 packetsResent = 0;
        uint64 packetsReceived = 0;
        uint64 packetsAcked = 0;
        uint64 packetsDropped = 0;  ///< Out of order packets dropped
        float32 rtt = 0.0f;
        float32 rttVariance = 0.0f;
    };

    /**
     * @brief Per-channel state for ordered delivery
     */
    struct ChannelState
    {
        SequenceNumber nextOutgoingSequence = 0;
        SequenceNumber nextExpectedSequence = 0;
        SequenceNumber lastAckedSequence = 0;
        uint32 receivedBits = 0;
        
        // For ordered delivery
        std::map<SequenceNumber, std::vector<uint8>> outOfOrderPackets;
    };

    /**
     * @brief Configuration for reliable layer
     */
    struct ReliableConfig
    {
        /// Maximum resend attempts before giving up
        uint32 maxResendAttempts = 10;
        
        /// Initial resend timeout (ms)
        uint32 initialRTOMs = 100;
        
        /// Maximum resend timeout (ms)
        uint32 maxRTOMs = 2000;
        
        /// Window size for flow control
        uint32 windowSize = 64;
        
        /// Enable congestion control
        bool enableCongestionControl = true;
        
        /// Fragment size for large packets
        uint32 fragmentSize = 1200;
    };

    /**
     * @brief Reliable UDP layer
     * 
     * Wraps a UDPTransport to provide reliable delivery.
     */
    class ReliableUDP
    {
    public:
        ReliableUDP();
        ~ReliableUDP();

        // =====================================================================
        // Lifecycle
        // =====================================================================

        /**
         * @brief Initialize with underlying transport
         */
        bool Initialize(std::shared_ptr<UDPTransport> transport,
                       const ReliableConfig& config = {});

        /**
         * @brief Shutdown the reliable layer
         */
        void Shutdown();

        // =====================================================================
        // Send
        // =====================================================================

        /**
         * @brief Send data with specified delivery mode
         * @param address Destination address
         * @param data Data to send
         * @param mode Delivery mode
         * @param channel Channel ID
         * @return true if queued successfully
         */
        bool Send(const NetworkAddress& address,
                  std::span<const uint8> data,
                  DeliveryMode mode,
                  ChannelId channel = 0);

        /**
         * @brief Send reliable data
         */
        bool SendReliable(const NetworkAddress& address,
                          std::span<const uint8> data,
                          ChannelId channel = 0)
        {
            return Send(address, data, DeliveryMode::Reliable, channel);
        }

        /**
         * @brief Send reliable ordered data
         */
        bool SendReliableOrdered(const NetworkAddress& address,
                                  std::span<const uint8> data,
                                  ChannelId channel = 0)
        {
            return Send(address, data, DeliveryMode::ReliableOrdered, channel);
        }

        /**
         * @brief Send unreliable data
         */
        bool SendUnreliable(const NetworkAddress& address,
                            std::span<const uint8> data,
                            ChannelId channel = 0)
        {
            return Send(address, data, DeliveryMode::Unreliable, channel);
        }

        // =====================================================================
        // Receive
        // =====================================================================

        /**
         * @brief Process incoming packets
         * @param timeoutMs Maximum time to wait
         * @return Number of packets processed
         */
        uint32 Update(uint32 timeoutMs = 0);

        /**
         * @brief Get next received packet
         * @param outPacket Received packet
         * @param outMode Delivery mode of the packet
         * @param outChannel Channel the packet was received on
         * @return true if a packet was available
         */
        bool Receive(ReceivedPacket& outPacket,
                     DeliveryMode& outMode,
                     ChannelId& outChannel);

        /**
         * @brief Check if packets are available
         */
        bool HasPendingPackets() const;

        // =====================================================================
        // Per-Address State
        // =====================================================================

        /**
         * @brief Get RTT for an address
         */
        float32 GetRTT(const NetworkAddress& address) const;

        /**
         * @brief Get packet loss for an address
         */
        float32 GetPacketLoss(const NetworkAddress& address) const;

        /**
         * @brief Get statistics for an address
         */
        ReliableStats GetStats(const NetworkAddress& address) const;

        /**
         * @brief Reset state for an address (called on disconnect)
         */
        void ResetAddress(const NetworkAddress& address);

        // =====================================================================
        // Configuration
        // =====================================================================

        const ReliableConfig& GetConfig() const { return m_config; }

    private:
        struct AddressState
        {
            std::array<ChannelState, RVX_NET_MAX_CHANNELS> channels;
            std::deque<PendingPacket> pendingReliable;
            ReliableStats stats;
            float32 rto = 100.0f; // Retransmission timeout
            NetworkTime lastAckTime;
        };

        std::shared_ptr<UDPTransport> m_transport;
        ReliableConfig m_config;

        // Per-address state
        std::map<std::string, AddressState> m_addressStates;
        mutable std::mutex m_stateMutex;

        // Received ordered packets ready for delivery
        struct DeliveredPacket
        {
            ReceivedPacket packet;
            DeliveryMode mode;
            ChannelId channel;
        };
        std::queue<DeliveredPacket> m_deliveryQueue;
        mutable std::mutex m_deliveryMutex;

        // Fragment assembly
        struct FragmentAssembly
        {
            uint16 totalFragments = 0;
            std::map<uint16, std::vector<uint8>> fragments;
            NetworkTime firstFragmentTime;
        };
        std::map<std::pair<std::string, SequenceNumber>, FragmentAssembly> m_fragmentAssembly;

        // Internal methods
        AddressState& GetOrCreateState(const NetworkAddress& address);
        std::string AddressToKey(const NetworkAddress& address) const;
        
        void ProcessReceivedPacket(const ReceivedPacket& rawPacket);
        void ProcessAck(AddressState& state, SequenceNumber ack, uint32 ackBits);
        void ProcessReliablePacket(AddressState& state, const ReliableHeader& header,
                                    std::span<const uint8> payload,
                                    const NetworkAddress& source);
        void ResendPendingPackets();
        void SendAck(const NetworkAddress& address, ChannelId channel);
        
        std::vector<uint8> AssembleFragments(const NetworkAddress& address,
                                              SequenceNumber sequence);
        void CleanupOldFragments();
        
        void UpdateRTT(AddressState& state, float32 sampleRtt);
    };

} // namespace RVX::Net
