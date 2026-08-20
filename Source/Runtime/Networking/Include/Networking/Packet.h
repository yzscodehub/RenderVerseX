#pragma once

/**
 * @file Packet.h
 * @brief Network packet structure and utilities
 */

#include "Networking/NetworkTypes.h"
#include "Networking/Serialization/BitStream.h"

#include <memory>
#include <vector>

namespace RVX::Net
{
    /**
     * @brief Packet header structure
     */
    struct PacketHeader
    {
        uint32 magic = RVX_NET_PROTOCOL_MAGIC;
        uint16 protocolVersion = RVX_NET_PROTOCOL_VERSION;
        PacketType type = PacketType::UserData;
        uint16 payloadSize = 0;
        ConnectionId connectionId = RVX_NET_INVALID_CONNECTION_ID;
        
        static constexpr uint32 kSize = 12; // bytes
        
        void Serialize(BitWriter& writer) const
        {
            writer.WriteUInt32(magic);
            writer.WriteUInt16(protocolVersion);
            writer.WriteUInt8(static_cast<uint8>(type));
            writer.WriteUInt16(payloadSize);
            writer.WriteUInt32(connectionId);
        }
        
        bool Deserialize(BitReader& reader)
        {
            magic = reader.ReadUInt32();
            if (magic != RVX_NET_PROTOCOL_MAGIC)
                return false;
            
            protocolVersion = reader.ReadUInt16();
            type = static_cast<PacketType>(reader.ReadUInt8());
            payloadSize = reader.ReadUInt16();
            connectionId = reader.ReadUInt32();
            
            return !reader.HasOverflowed();
        }
    };

    /**
     * @brief Network packet for sending/receiving data
     */
    class Packet
    {
    public:
        // =====================================================================
        // Construction
        // =====================================================================

        Packet() = default;
        
        /**
         * @brief Create packet with reserved capacity
         */
        explicit Packet(uint32 reserveSize)
        {
            m_data.reserve(reserveSize);
        }

        /**
         * @brief Create packet from raw data
         */
        Packet(const uint8* data, uint32 size)
            : m_data(data, data + size)
        {
        }

        /**
         * @brief Create packet from vector
         */
        explicit Packet(std::vector<uint8> data)
            : m_data(std::move(data))
        {
        }

        // =====================================================================
        // Header
        // =====================================================================

        /**
         * @brief Get packet header
         */
        const PacketHeader& GetHeader() const { return m_header; }
        
        /**
         * @brief Set packet header
         */
        void SetHeader(const PacketHeader& header) { m_header = header; }

        /**
         * @brief Get packet type
         */
        PacketType GetType() const { return m_header.type; }
        
        /**
         * @brief Set packet type
         */
        void SetType(PacketType type) { m_header.type = type; }

        /**
         * @brief Get connection ID
         */
        ConnectionId GetConnectionId() const { return m_header.connectionId; }
        
        /**
         * @brief Set connection ID
         */
        void SetConnectionId(ConnectionId id) { m_header.connectionId = id; }

        // =====================================================================
        // Data Access
        // =====================================================================

        /**
         * @brief Get payload data (after header)
         */
        std::span<const uint8> GetPayload() const
        {
            if (m_data.size() <= PacketHeader::kSize)
                return {};
            return { m_data.data() + PacketHeader::kSize, m_data.size() - PacketHeader::kSize };
        }

        /**
         * @brief Get full packet data (header + payload)
         */
        std::span<const uint8> GetData() const
        {
            return m_data;
        }

        /**
         * @brief Get mutable data
         */
        std::vector<uint8>& GetMutableData() { return m_data; }

        /**
         * @brief Get packet size
         */
        uint32 GetSize() const { return static_cast<uint32>(m_data.size()); }

        /**
         * @brief Get payload size
         */
        uint32 GetPayloadSize() const 
        { 
            return m_data.size() > PacketHeader::kSize ? 
                   static_cast<uint32>(m_data.size() - PacketHeader::kSize) : 0;
        }

        /**
         * @brief Check if packet is empty
         */
        bool IsEmpty() const { return m_data.empty(); }

        // =====================================================================
        // Writing
        // =====================================================================

        /**
         * @brief Start writing a new packet
         */
        BitWriter BeginWrite()
        {
            m_data.clear();
            m_data.resize(PacketHeader::kSize); // Reserve space for header
            return BitWriter(m_data.data() + PacketHeader::kSize, 
                            RVX_NET_MAX_PACKET_SIZE - PacketHeader::kSize);
        }

        /**
         * @brief Finalize packet after writing
         */
        void EndWrite(BitWriter& writer)
        {
            uint32 payloadSize = writer.GetBytesWritten();
            m_data.resize(PacketHeader::kSize + payloadSize);
            m_header.payloadSize = static_cast<uint16>(payloadSize);
            
            // Write header
            BitWriter headerWriter(m_data.data(), PacketHeader::kSize);
            m_header.Serialize(headerWriter);
        }

        /**
         * @brief Set payload directly
         */
        void SetPayload(std::span<const uint8> payload)
        {
            m_data.resize(PacketHeader::kSize + payload.size());
            std::memcpy(m_data.data() + PacketHeader::kSize, payload.data(), payload.size());
            m_header.payloadSize = static_cast<uint16>(payload.size());
            
            // Write header
            BitWriter headerWriter(m_data.data(), PacketHeader::kSize);
            m_header.Serialize(headerWriter);
        }

        // =====================================================================
        // Reading
        // =====================================================================

        /**
         * @brief Parse packet from raw data
         */
        bool Parse(std::span<const uint8> rawData)
        {
            if (rawData.size() < PacketHeader::kSize)
                return false;

            m_data.assign(rawData.begin(), rawData.end());
            
            BitReader reader(m_data.data(), PacketHeader::kSize);
            if (!m_header.Deserialize(reader))
                return false;

            return m_data.size() >= PacketHeader::kSize + m_header.payloadSize;
        }

        /**
         * @brief Get reader for payload
         */
        BitReader GetPayloadReader() const
        {
            if (m_data.size() <= PacketHeader::kSize)
                return BitReader(nullptr, 0);
            
            return BitReader(m_data.data() + PacketHeader::kSize, 
                            static_cast<uint32>(m_data.size() - PacketHeader::kSize));
        }

        // =====================================================================
        // Validation
        // =====================================================================

        /**
         * @brief Check if packet has valid header
         */
        bool IsValid() const
        {
            return m_header.magic == RVX_NET_PROTOCOL_MAGIC &&
                   m_data.size() >= PacketHeader::kSize;
        }

        /**
         * @brief Check protocol version compatibility
         */
        bool IsCompatibleVersion() const
        {
            return m_header.protocolVersion == RVX_NET_PROTOCOL_VERSION;
        }

    private:
        PacketHeader m_header;
        std::vector<uint8> m_data;
    };

    /**
     * @brief Shared pointer type for packets
     */
    using PacketPtr = std::shared_ptr<Packet>;

    // =========================================================================
    // Packet Factories
    // =========================================================================

    /**
     * @brief Create a connection request packet
     */
    inline Packet CreateConnectionRequest(std::string_view clientName = "")
    {
        Packet packet(64);
        packet.SetType(PacketType::ConnectionRequest);
        
        auto writer = packet.BeginWrite();
        writer.WriteString(clientName);
        packet.EndWrite(writer);
        
        return packet;
    }

    /**
     * @brief Create a connection accepted packet
     */
    inline Packet CreateConnectionAccepted(ConnectionId connectionId)
    {
        Packet packet(32);
        packet.SetType(PacketType::ConnectionAccepted);
        packet.SetConnectionId(connectionId);
        
        auto writer = packet.BeginWrite();
        packet.EndWrite(writer);
        
        return packet;
    }

    /**
     * @brief Create a connection denied packet
     */
    inline Packet CreateConnectionDenied(DisconnectReason reason, std::string_view message = "")
    {
        Packet packet(64);
        packet.SetType(PacketType::ConnectionDenied);
        
        auto writer = packet.BeginWrite();
        writer.WriteUInt8(static_cast<uint8>(reason));
        writer.WriteString(message);
        packet.EndWrite(writer);
        
        return packet;
    }

    /**
     * @brief Create a disconnect packet
     */
    inline Packet CreateDisconnect(DisconnectReason reason)
    {
        Packet packet(32);
        packet.SetType(PacketType::Disconnect);
        
        auto writer = packet.BeginWrite();
        writer.WriteUInt8(static_cast<uint8>(reason));
        packet.EndWrite(writer);
        
        return packet;
    }

    /**
     * @brief Create a ping packet
     */
    inline Packet CreatePing(uint32 sequence, uint64 timestamp)
    {
        Packet packet(32);
        packet.SetType(PacketType::Ping);
        
        auto writer = packet.BeginWrite();
        writer.WriteUInt32(sequence);
        writer.WriteUInt64(timestamp);
        packet.EndWrite(writer);
        
        return packet;
    }

    /**
     * @brief Create a pong packet
     */
    inline Packet CreatePong(uint32 sequence, uint64 pingTimestamp)
    {
        Packet packet(32);
        packet.SetType(PacketType::Pong);
        
        auto writer = packet.BeginWrite();
        writer.WriteUInt32(sequence);
        writer.WriteUInt64(pingTimestamp);
        packet.EndWrite(writer);
        
        return packet;
    }

} // namespace RVX::Net
