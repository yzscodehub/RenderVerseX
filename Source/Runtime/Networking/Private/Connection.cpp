#include "Networking/Connection.h"
#include "Core/Log.h"

namespace RVX::Net
{
    Connection::Connection(ConnectionId id, const NetworkAddress& remoteAddress)
        : m_id(id)
        , m_remoteAddress(remoteAddress)
        , m_connectionTime(std::chrono::steady_clock::now())
        , m_lastActivityTime(m_connectionTime)
    {
    }

    Connection::~Connection() = default;

    void Connection::Send(std::span<const uint8> data, DeliveryMode mode, ChannelId channel)
    {
        QueueOutgoing(std::vector<uint8>(data.begin(), data.end()), mode, channel);
    }

    void Connection::SendPacket(const Packet& packet, DeliveryMode mode, ChannelId channel)
    {
        auto data = packet.GetData();
        Send(data, mode, channel);
    }

    void Connection::QueueOutgoing(std::vector<uint8> data, DeliveryMode mode, ChannelId channel)
    {
        std::lock_guard<std::mutex> lock(m_outgoingMutex);
        m_outgoingQueue.push({ std::move(data), mode, channel });
    }

    void Connection::QueueIncoming(std::vector<uint8> data, ChannelId channel)
    {
        std::lock_guard<std::mutex> lock(m_incomingMutex);
        m_incomingQueue.push({ std::move(data), channel });
        MarkActivity();
    }

    bool Connection::PopOutgoing(std::vector<uint8>& data, DeliveryMode& mode, ChannelId& channel)
    {
        std::lock_guard<std::mutex> lock(m_outgoingMutex);
        if (m_outgoingQueue.empty())
            return false;
        
        auto& front = m_outgoingQueue.front();
        data = std::move(front.data);
        mode = front.mode;
        channel = front.channel;
        m_outgoingQueue.pop();
        return true;
    }

    bool Connection::PopIncoming(std::vector<uint8>& data, ChannelId& channel)
    {
        std::lock_guard<std::mutex> lock(m_incomingMutex);
        if (m_incomingQueue.empty())
            return false;
        
        auto& front = m_incomingQueue.front();
        data = std::move(front.data);
        channel = front.channel;
        m_incomingQueue.pop();
        return true;
    }

    bool Connection::HasIncomingData() const
    {
        std::lock_guard<std::mutex> lock(m_incomingMutex);
        return !m_incomingQueue.empty();
    }

    void Connection::SetState(ConnectionState state)
    {
        if (m_state != state)
        {
            RVX_CORE_DEBUG("Connection {} state: {} -> {}", 
                          m_id, 
                          ConnectionStateToString(m_state),
                          ConnectionStateToString(state));
            m_state = state;
            
            if (state == ConnectionState::Connected)
            {
                m_connectionTime = std::chrono::steady_clock::now();
            }
        }
    }

    void Connection::Update(float32 deltaTime)
    {
        if (m_state != ConnectionState::Connected)
            return;

        // Update ping timing
        m_timeSinceLastPing += deltaTime;
        
        // Check for timeout
        auto now = std::chrono::steady_clock::now();
        auto timeSinceActivity = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - m_lastActivityTime).count();
        
        if (timeSinceActivity > RVX_NET_DEFAULT_TIMEOUT_MS)
        {
            OnTimeout();
        }
    }

    void Connection::OnTimeout()
    {
        RVX_CORE_WARN("Connection {} timed out", m_id);
        SetState(ConnectionState::TimedOut);
    }

    void Connection::UpdateStats(const ReliableStats& reliableStats)
    {
        m_stats.packetsSent = reliableStats.packetsSent;
        m_stats.packetsReceived = reliableStats.packetsReceived;
        m_stats.packetsLost = reliableStats.packetsDropped;
        m_stats.roundTripTimeMs = reliableStats.rtt;
        
        if (reliableStats.packetsSent > 0)
        {
            m_stats.packetLossPercent = 
                static_cast<float32>(reliableStats.packetsResent) / 
                static_cast<float32>(reliableStats.packetsSent) * 100.0f;
        }
    }

    void Connection::MarkActivity()
    {
        m_lastActivityTime = std::chrono::steady_clock::now();
        m_stats.lastPacketReceived = m_lastActivityTime;
    }

} // namespace RVX::Net
