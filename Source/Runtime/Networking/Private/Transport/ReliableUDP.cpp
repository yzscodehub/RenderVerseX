#include "Networking/Transport/ReliableUDP.h"
#include "Networking/Transport/UDPTransport.h"
#include "Core/Log.h"

#include <algorithm>
#include <cmath>

namespace RVX::Net
{
    // =========================================================================
    // ReliableHeader Serialization
    // =========================================================================

    void ReliableHeader::Serialize(BitWriter& writer) const
    {
        writer.WriteUInt8(static_cast<uint8>(type));
        writer.WriteUInt16(sequence);
        writer.WriteUInt16(ack);
        writer.WriteUInt32(ackBits);
        writer.WriteUInt8(channel);
        writer.WriteUInt16(fragmentIndex);
        writer.WriteUInt16(fragmentCount);
    }

    void ReliableHeader::Deserialize(BitReader& reader)
    {
        type = static_cast<PacketType>(reader.ReadUInt8());
        sequence = reader.ReadUInt16();
        ack = reader.ReadUInt16();
        ackBits = reader.ReadUInt32();
        channel = reader.ReadUInt8();
        fragmentIndex = reader.ReadUInt16();
        fragmentCount = reader.ReadUInt16();
    }

    // =========================================================================
    // ReliableUDP Implementation
    // =========================================================================

    ReliableUDP::ReliableUDP() = default;
    ReliableUDP::~ReliableUDP() = default;

    bool ReliableUDP::Initialize(std::shared_ptr<UDPTransport> transport,
                                  const ReliableConfig& config)
    {
        if (!transport || !transport->IsActive())
        {
            RVX_CORE_ERROR("ReliableUDP: Invalid transport");
            return false;
        }

        m_transport = std::move(transport);
        m_config = config;

        RVX_CORE_INFO("ReliableUDP initialized");
        return true;
    }

    void ReliableUDP::Shutdown()
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        m_addressStates.clear();
        m_fragmentAssembly.clear();
        
        std::lock_guard<std::mutex> deliveryLock(m_deliveryMutex);
        while (!m_deliveryQueue.empty())
            m_deliveryQueue.pop();

        m_transport.reset();
    }

    std::string ReliableUDP::AddressToKey(const NetworkAddress& address) const
    {
        return address.host + ":" + std::to_string(address.port);
    }

    ReliableUDP::AddressState& ReliableUDP::GetOrCreateState(const NetworkAddress& address)
    {
        std::string key = AddressToKey(address);
        auto it = m_addressStates.find(key);
        if (it == m_addressStates.end())
        {
            AddressState state;
            state.rto = static_cast<float32>(m_config.initialRTOMs);
            state.lastAckTime = std::chrono::steady_clock::now();
            it = m_addressStates.emplace(key, std::move(state)).first;
        }
        return it->second;
    }

    bool ReliableUDP::Send(const NetworkAddress& address,
                            std::span<const uint8> data,
                            DeliveryMode mode,
                            ChannelId channel)
    {
        if (!m_transport)
            return false;

        std::lock_guard<std::mutex> lock(m_stateMutex);
        AddressState& state = GetOrCreateState(address);
        ChannelState& channelState = state.channels[channel];

        // Check if we need fragmentation
        bool needsFragmentation = data.size() > m_config.fragmentSize;
        
        if (needsFragmentation && mode == DeliveryMode::Unreliable)
        {
            // Large unreliable packets get dropped
            RVX_CORE_WARN("Unreliable packet too large, dropping");
            return false;
        }

        if (needsFragmentation)
        {
            // Fragment the data
            uint16 fragmentCount = static_cast<uint16>(
                (data.size() + m_config.fragmentSize - 1) / m_config.fragmentSize);
            
            for (uint16 i = 0; i < fragmentCount; ++i)
            {
                size_t offset = i * m_config.fragmentSize;
                size_t fragSize = std::min(
                    static_cast<size_t>(m_config.fragmentSize),
                    data.size() - offset);

                BitWriter writer(m_config.fragmentSize + ReliableHeader::kHeaderSize);
                
                ReliableHeader header;
                header.type = PacketType::ReliableFragment;
                header.sequence = channelState.nextOutgoingSequence++;
                header.ack = channelState.lastAckedSequence;
                header.ackBits = channelState.receivedBits;
                header.channel = channel;
                header.fragmentIndex = i;
                header.fragmentCount = fragmentCount;
                
                header.Serialize(writer);
                writer.WriteBytes(data.data() + offset, static_cast<uint32>(fragSize));

                auto packetData = writer.ToVector();
                
                // Queue for reliable delivery
                PendingPacket pending;
                pending.sequence = header.sequence;
                pending.data = packetData;
                pending.sendTime = std::chrono::steady_clock::now();
                pending.lastResendTime = pending.sendTime;
                pending.mode = mode;
                pending.channel = channel;
                state.pendingReliable.push_back(std::move(pending));

                m_transport->SendTo(address, packetData);
                state.stats.packetsSent++;
            }
            
            return true;
        }

        // Non-fragmented packet
        BitWriter writer(data.size() + ReliableHeader::kHeaderSize);
        
        ReliableHeader header;
        header.type = (mode == DeliveryMode::Unreliable || mode == DeliveryMode::UnreliableSequenced)
            ? PacketType::UserData : PacketType::ReliableData;
        header.sequence = channelState.nextOutgoingSequence++;
        header.ack = channelState.lastAckedSequence;
        header.ackBits = channelState.receivedBits;
        header.channel = channel;
        header.fragmentIndex = 0;
        header.fragmentCount = 1;
        
        header.Serialize(writer);
        writer.WriteBytes(data);

        auto packetData = writer.ToVector();

        // For reliable modes, queue the packet
        if (mode == DeliveryMode::Reliable || 
            mode == DeliveryMode::ReliableSequenced ||
            mode == DeliveryMode::ReliableOrdered)
        {
            PendingPacket pending;
            pending.sequence = header.sequence;
            pending.data = packetData;
            pending.sendTime = std::chrono::steady_clock::now();
            pending.lastResendTime = pending.sendTime;
            pending.mode = mode;
            pending.channel = channel;
            state.pendingReliable.push_back(std::move(pending));
        }

        m_transport->SendTo(address, packetData);
        state.stats.packetsSent++;

        return true;
    }

    uint32 ReliableUDP::Update(uint32 timeoutMs)
    {
        if (!m_transport)
            return 0;

        // Poll the transport
        m_transport->Poll(timeoutMs);

        // Process received packets
        ReceivedPacket rawPacket;
        uint32 processed = 0;
        
        while (m_transport->ReceiveFrom(rawPacket) == TransportResult::Success)
        {
            ProcessReceivedPacket(rawPacket);
            processed++;
        }

        // Resend pending reliable packets
        ResendPendingPackets();

        // Cleanup old fragments
        CleanupOldFragments();

        return processed;
    }

    void ReliableUDP::ProcessReceivedPacket(const ReceivedPacket& rawPacket)
    {
        if (rawPacket.data.size() < ReliableHeader::kHeaderSize)
            return;

        BitReader reader(rawPacket.data.data(), static_cast<uint32>(rawPacket.data.size()));
        
        ReliableHeader header;
        header.Deserialize(reader);

        if (reader.HasOverflowed())
            return;

        std::lock_guard<std::mutex> lock(m_stateMutex);
        AddressState& state = GetOrCreateState(rawPacket.source);
        state.stats.packetsReceived++;

        // Process acknowledgment
        ProcessAck(state, header.ack, header.ackBits);

        // Handle based on packet type
        switch (header.type)
        {
            case PacketType::ReliableData:
            case PacketType::UserData:
            {
                auto payload = reader.ReadBytesSpan(reader.GetRemainingBits() / 8);
                ProcessReliablePacket(state, header, payload, rawPacket.source);
                break;
            }
            case PacketType::ReliableFragment:
            {
                auto payload = reader.ReadBytesSpan(reader.GetRemainingBits() / 8);
                
                // Store fragment
                auto key = std::make_pair(AddressToKey(rawPacket.source), header.sequence);
                auto& assembly = m_fragmentAssembly[key];
                
                if (assembly.totalFragments == 0)
                {
                    assembly.totalFragments = header.fragmentCount;
                    assembly.firstFragmentTime = rawPacket.receiveTime;
                }
                
                assembly.fragments[header.fragmentIndex] = 
                    std::vector<uint8>(payload.begin(), payload.end());

                // Check if complete
                if (assembly.fragments.size() == assembly.totalFragments)
                {
                    auto assembled = AssembleFragments(rawPacket.source, header.sequence);
                    if (!assembled.empty())
                    {
                        DeliveredPacket delivered;
                        delivered.packet.source = rawPacket.source;
                        delivered.packet.receiveTime = rawPacket.receiveTime;
                        delivered.packet.data = std::move(assembled);
                        delivered.mode = DeliveryMode::Reliable;
                        delivered.channel = header.channel;
                        
                        std::lock_guard<std::mutex> deliveryLock(m_deliveryMutex);
                        m_deliveryQueue.push(std::move(delivered));
                    }
                }
                
                // Send ACK
                SendAck(rawPacket.source, header.channel);
                break;
            }
            case PacketType::ReliableAck:
            case PacketType::Ack:
                // ACK is processed above
                break;
            default:
                break;
        }
    }

    void ReliableUDP::ProcessAck(AddressState& state, SequenceNumber ack, uint32 ackBits)
    {
        auto now = std::chrono::steady_clock::now();

        // Remove acknowledged packets
        auto it = state.pendingReliable.begin();
        while (it != state.pendingReliable.end())
        {
            bool acked = false;
            
            if (it->sequence == ack)
            {
                acked = true;
            }
            else
            {
                int32 diff = SequenceDiff(ack, it->sequence);
                if (diff > 0 && diff <= 32)
                {
                    if (ackBits & (1u << (diff - 1)))
                    {
                        acked = true;
                    }
                }
            }

            if (acked)
            {
                // Calculate RTT sample
                auto rttSample = std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - it->sendTime).count();
                UpdateRTT(state, static_cast<float32>(rttSample));
                
                state.stats.packetsAcked++;
                it = state.pendingReliable.erase(it);
            }
            else
            {
                ++it;
            }
        }

        state.lastAckTime = now;
    }

    void ReliableUDP::ProcessReliablePacket(AddressState& state, const ReliableHeader& header,
                                             std::span<const uint8> payload,
                                             const NetworkAddress& source)
    {
        ChannelState& channelState = state.channels[header.channel];

        // Update received bits
        int32 diff = SequenceDiff(header.sequence, channelState.lastAckedSequence);
        if (diff > 0)
        {
            // New sequence, shift the bits
            if (diff < 32)
            {
                channelState.receivedBits <<= diff;
                channelState.receivedBits |= 1;
            }
            else
            {
                channelState.receivedBits = 1;
            }
            channelState.lastAckedSequence = header.sequence;
        }
        else if (diff < 0 && diff > -32)
        {
            // Old sequence, set the bit
            channelState.receivedBits |= (1u << (-diff - 1));
        }

        // Send ACK for reliable packets
        if (header.type == PacketType::ReliableData)
        {
            SendAck(source, header.channel);
        }

        // Deliver the packet
        DeliveredPacket delivered;
        delivered.packet.source = source;
        delivered.packet.receiveTime = std::chrono::steady_clock::now();
        delivered.packet.data = std::vector<uint8>(payload.begin(), payload.end());
        delivered.mode = (header.type == PacketType::ReliableData) ? 
            DeliveryMode::Reliable : DeliveryMode::Unreliable;
        delivered.channel = header.channel;

        std::lock_guard<std::mutex> deliveryLock(m_deliveryMutex);
        m_deliveryQueue.push(std::move(delivered));
    }

    void ReliableUDP::ResendPendingPackets()
    {
        auto now = std::chrono::steady_clock::now();

        std::lock_guard<std::mutex> lock(m_stateMutex);

        for (auto& [key, state] : m_addressStates)
        {
            for (auto& pending : state.pendingReliable)
            {
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - pending.lastResendTime).count();

                if (elapsed >= static_cast<int64>(state.rto))
                {
                    if (pending.resendCount >= m_config.maxResendAttempts)
                    {
                        // Give up on this packet
                        state.stats.packetsDropped++;
                        continue;
                    }

                    // Parse address from key
                    size_t colonPos = key.rfind(':');
                    if (colonPos != std::string::npos)
                    {
                        NetworkAddress addr;
                        addr.host = key.substr(0, colonPos);
                        addr.port = static_cast<uint16>(std::stoi(key.substr(colonPos + 1)));
                        
                        m_transport->SendTo(addr, pending.data);
                        pending.lastResendTime = now;
                        pending.resendCount++;
                        state.stats.packetsResent++;
                    }
                }
            }

            // Remove packets that exceeded max attempts
            state.pendingReliable.erase(
                std::remove_if(state.pendingReliable.begin(), state.pendingReliable.end(),
                    [this](const PendingPacket& p) {
                        return p.resendCount >= m_config.maxResendAttempts;
                    }),
                state.pendingReliable.end());
        }
    }

    void ReliableUDP::SendAck(const NetworkAddress& address, ChannelId channel)
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        AddressState& state = GetOrCreateState(address);
        ChannelState& channelState = state.channels[channel];

        BitWriter writer(ReliableHeader::kHeaderSize);
        
        ReliableHeader header;
        header.type = PacketType::Ack;
        header.sequence = 0;
        header.ack = channelState.lastAckedSequence;
        header.ackBits = channelState.receivedBits;
        header.channel = channel;
        header.fragmentIndex = 0;
        header.fragmentCount = 1;
        
        header.Serialize(writer);

        m_transport->SendTo(address, writer.GetSpan());
    }

    std::vector<uint8> ReliableUDP::AssembleFragments(const NetworkAddress& address,
                                                        SequenceNumber sequence)
    {
        auto key = std::make_pair(AddressToKey(address), sequence);
        auto it = m_fragmentAssembly.find(key);
        
        if (it == m_fragmentAssembly.end())
            return {};

        std::vector<uint8> assembled;
        for (uint16 i = 0; i < it->second.totalFragments; ++i)
        {
            auto fragIt = it->second.fragments.find(i);
            if (fragIt == it->second.fragments.end())
                return {}; // Missing fragment
            
            assembled.insert(assembled.end(), 
                             fragIt->second.begin(), 
                             fragIt->second.end());
        }

        m_fragmentAssembly.erase(it);
        return assembled;
    }

    void ReliableUDP::CleanupOldFragments()
    {
        auto now = std::chrono::steady_clock::now();
        constexpr auto maxAge = std::chrono::seconds(30);

        auto it = m_fragmentAssembly.begin();
        while (it != m_fragmentAssembly.end())
        {
            if (now - it->second.firstFragmentTime > maxAge)
            {
                it = m_fragmentAssembly.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }

    void ReliableUDP::UpdateRTT(AddressState& state, float32 sampleRtt)
    {
        // Jacobson's algorithm
        constexpr float32 alpha = 0.125f;
        constexpr float32 beta = 0.25f;

        if (state.stats.rtt == 0.0f)
        {
            state.stats.rtt = sampleRtt;
            state.stats.rttVariance = sampleRtt / 2.0f;
        }
        else
        {
            float32 diff = sampleRtt - state.stats.rtt;
            state.stats.rtt += alpha * diff;
            state.stats.rttVariance += beta * (std::abs(diff) - state.stats.rttVariance);
        }

        // Calculate RTO
        state.rto = state.stats.rtt + 4.0f * state.stats.rttVariance;
        state.rto = std::clamp(state.rto, 
                               static_cast<float32>(m_config.initialRTOMs),
                               static_cast<float32>(m_config.maxRTOMs));
    }

    bool ReliableUDP::Receive(ReceivedPacket& outPacket,
                               DeliveryMode& outMode,
                               ChannelId& outChannel)
    {
        std::lock_guard<std::mutex> lock(m_deliveryMutex);
        
        if (m_deliveryQueue.empty())
            return false;

        auto& delivered = m_deliveryQueue.front();
        outPacket = std::move(delivered.packet);
        outMode = delivered.mode;
        outChannel = delivered.channel;
        m_deliveryQueue.pop();
        
        return true;
    }

    bool ReliableUDP::HasPendingPackets() const
    {
        std::lock_guard<std::mutex> lock(m_deliveryMutex);
        return !m_deliveryQueue.empty();
    }

    float32 ReliableUDP::GetRTT(const NetworkAddress& address) const
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        auto it = m_addressStates.find(AddressToKey(address));
        if (it != m_addressStates.end())
            return it->second.stats.rtt;
        return 0.0f;
    }

    float32 ReliableUDP::GetPacketLoss(const NetworkAddress& address) const
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        auto it = m_addressStates.find(AddressToKey(address));
        if (it != m_addressStates.end())
        {
            const auto& stats = it->second.stats;
            if (stats.packetsSent > 0)
            {
                return static_cast<float32>(stats.packetsResent) / 
                       static_cast<float32>(stats.packetsSent) * 100.0f;
            }
        }
        return 0.0f;
    }

    ReliableStats ReliableUDP::GetStats(const NetworkAddress& address) const
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        auto it = m_addressStates.find(AddressToKey(address));
        if (it != m_addressStates.end())
            return it->second.stats;
        return {};
    }

    void ReliableUDP::ResetAddress(const NetworkAddress& address)
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        m_addressStates.erase(AddressToKey(address));
    }

} // namespace RVX::Net
