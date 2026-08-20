#include "Networking/NetworkManager.h"
#include "Core/Log.h"

namespace RVX::Net
{
    NetworkManager::NetworkManager() = default;

    NetworkManager::~NetworkManager()
    {
        Stop();
    }

    bool NetworkManager::StartServer(uint16 port)
    {
        if (m_active)
        {
            RVX_CORE_WARN("NetworkManager already active");
            return false;
        }

        // Create transport
        m_transport = std::make_shared<UDPTransport>();
        
        TransportConfig transportConfig;
        transportConfig.bindPort = port;
        transportConfig.reuseAddress = true;
        
        if (m_transport->Initialize(transportConfig) != TransportResult::Success)
        {
            RVX_CORE_ERROR("Failed to initialize transport on port {}", port);
            m_transport.reset();
            return false;
        }

        // Create reliable layer
        m_reliable = std::make_unique<ReliableUDP>();
        if (!m_reliable->Initialize(m_transport, m_config.reliable))
        {
            RVX_CORE_ERROR("Failed to initialize reliable layer");
            m_transport->Shutdown();
            m_transport.reset();
            m_reliable.reset();
            return false;
        }

        m_role = NetworkRole::Server;
        m_active = true;
        
        RVX_CORE_INFO("Server started on port {}", port);
        return true;
    }

    bool NetworkManager::Connect(const std::string& address, uint16 port)
    {
        if (m_active)
        {
            RVX_CORE_WARN("NetworkManager already active");
            return false;
        }

        // Create transport (bind to any available port)
        m_transport = std::make_shared<UDPTransport>();
        
        TransportConfig transportConfig;
        transportConfig.bindPort = 0; // Auto-assign port
        
        if (m_transport->Initialize(transportConfig) != TransportResult::Success)
        {
            RVX_CORE_ERROR("Failed to initialize client transport");
            m_transport.reset();
            return false;
        }

        // Create reliable layer
        m_reliable = std::make_unique<ReliableUDP>();
        if (!m_reliable->Initialize(m_transport, m_config.reliable))
        {
            RVX_CORE_ERROR("Failed to initialize reliable layer");
            m_transport->Shutdown();
            m_transport.reset();
            m_reliable.reset();
            return false;
        }

        m_role = NetworkRole::Client;
        m_active = true;
        m_config.serverAddress = address;
        m_config.serverPort = port;

        // Create server connection
        NetworkAddress serverAddr(address, port);
        auto serverConn = CreateConnection(serverAddr);
        serverConn->SetState(ConnectionState::Connecting);
        m_serverConnectionId = serverConn->GetId();

        // Send connection request
        auto requestPacket = CreateConnectionRequest("RVX_Client");
        m_reliable->SendReliable(serverAddr, requestPacket.GetData(), 0);

        RVX_CORE_INFO("Connecting to {}:{}", address, port);
        return true;
    }

    bool NetworkManager::StartHost(uint16 port)
    {
        if (!StartServer(port))
            return false;
        
        m_role = NetworkRole::Host;
        
        // Create local connection
        NetworkAddress localAddr("127.0.0.1", port);
        auto localConn = CreateConnection(localAddr);
        localConn->SetState(ConnectionState::Connected);
        m_serverConnectionId = localConn->GetId();
        
        RVX_CORE_INFO("Host started on port {}", port);
        return true;
    }

    void NetworkManager::Stop()
    {
        if (!m_active)
            return;

        // Notify all connections
        DisconnectAll(m_role == NetworkRole::Server ? 
                      DisconnectReason::ServerShutdown : 
                      DisconnectReason::UserRequested);

        // Shutdown layers
        if (m_reliable)
        {
            m_reliable->Shutdown();
            m_reliable.reset();
        }

        if (m_transport)
        {
            m_transport->Shutdown();
            m_transport.reset();
        }

        // Clear state
        {
            std::lock_guard<std::mutex> lock(m_connectionsMutex);
            m_connections.clear();
            m_addressToConnection.clear();
        }

        m_active = false;
        m_role = NetworkRole::None;
        m_serverConnectionId = RVX_NET_INVALID_CONNECTION_ID;
        m_nextConnectionId = 1;

        RVX_CORE_INFO("NetworkManager stopped");
    }

    void NetworkManager::Update(float32 deltaTime)
    {
        if (!m_active)
            return;

        // Process transport and reliable layer
        ProcessTransport();

        // Update connections
        ProcessConnections(deltaTime);
    }

    void NetworkManager::ProcessTransport()
    {
        if (!m_reliable)
            return;

        // Update reliable layer (polls transport, handles resends)
        m_reliable->Update(0);

        // Process received packets
        ReceivedPacket rawPacket;
        DeliveryMode mode;
        ChannelId channel;

        while (m_reliable->Receive(rawPacket, mode, channel))
        {
            HandlePacket(rawPacket.source, rawPacket.data);
        }
    }

    void NetworkManager::ProcessConnections(float32 deltaTime)
    {
        std::vector<ConnectionId> timedOut;
        
        {
            std::lock_guard<std::mutex> lock(m_connectionsMutex);
            
            for (auto& [id, conn] : m_connections)
            {
                conn->Update(deltaTime);
                
                // Process outgoing data
                std::vector<uint8> data;
                DeliveryMode mode;
                ChannelId channel;
                
                while (conn->PopOutgoing(data, mode, channel))
                {
                    m_reliable->Send(conn->GetRemoteAddress(), data, mode, channel);
                }
                
                // Update stats
                conn->UpdateStats(m_reliable->GetStats(conn->GetRemoteAddress()));
                
                // Check for timeout
                if (conn->GetState() == ConnectionState::TimedOut)
                {
                    timedOut.push_back(id);
                }
            }
        }

        // Remove timed out connections
        for (auto id : timedOut)
        {
            RemoveConnection(id, DisconnectReason::Timeout);
        }
    }

    void NetworkManager::HandlePacket(const NetworkAddress& source, std::span<const uint8> data)
    {
        if (data.size() < PacketHeader::kSize)
            return;

        Packet packet;
        if (!packet.Parse(data))
        {
            RVX_CORE_WARN("Invalid packet from {}", source.ToString());
            return;
        }

        if (!packet.IsValid() || !packet.IsCompatibleVersion())
        {
            RVX_CORE_WARN("Incompatible packet from {}", source.ToString());
            return;
        }

        auto reader = packet.GetPayloadReader();

        switch (packet.GetType())
        {
            case PacketType::ConnectionRequest:
                HandleConnectionRequest(source, reader);
                break;
            case PacketType::ConnectionAccepted:
                HandleConnectionAccepted(source, reader);
                break;
            case PacketType::ConnectionDenied:
                HandleConnectionDenied(source, reader);
                break;
            case PacketType::Disconnect:
                HandleDisconnect(source, reader);
                break;
            case PacketType::Ping:
                HandlePing(source, reader);
                break;
            case PacketType::Pong:
                HandlePong(source, reader);
                break;
            case PacketType::UserData:
            case PacketType::Replication:
            case PacketType::RPC:
            case PacketType::Broadcast:
                HandleUserData(source, packet.GetPayload());
                break;
            default:
                if (static_cast<uint8>(packet.GetType()) >= static_cast<uint8>(PacketType::UserPacketStart))
                {
                    HandleUserData(source, packet.GetPayload());
                }
                break;
        }
    }

    void NetworkManager::HandleConnectionRequest(const NetworkAddress& source, BitReader& reader)
    {
        if (!IsServer())
            return;

        std::string clientName = reader.ReadString();

        // Check connection limit
        if (GetConnectionCount() >= m_config.network.maxConnections)
        {
            RVX_CORE_WARN("Connection denied: server full");
            SendConnectionDenied(source, DisconnectReason::ServerFull);
            return;
        }

        // Check for existing connection
        if (FindConnectionByAddress(source))
        {
            RVX_CORE_WARN("Duplicate connection request from {}", source.ToString());
            return;
        }

        // Accept connection
        auto conn = CreateConnection(source);
        conn->SetClientName(clientName);
        conn->SetState(ConnectionState::Connected);

        SendConnectionAccepted(source, conn->GetId());

        RVX_CORE_INFO("Client connected: {} from {}", clientName, source.ToString());

        if (m_onConnect)
        {
            m_onConnect(conn);
        }
    }

    void NetworkManager::HandleConnectionAccepted(const NetworkAddress& source, BitReader& reader)
    {
        (void)reader; // Unused for now
        
        if (!IsClient())
            return;

        auto conn = GetServerConnection();
        if (!conn)
            return;

        if (conn->GetState() == ConnectionState::Connecting)
        {
            conn->SetState(ConnectionState::Connected);
            RVX_CORE_INFO("Connected to server {}", source.ToString());

            if (m_onConnect)
            {
                m_onConnect(conn);
            }
        }
    }

    void NetworkManager::HandleConnectionDenied(const NetworkAddress& source, BitReader& reader)
    {
        auto reason = static_cast<DisconnectReason>(reader.ReadUInt8());
        std::string message = reader.ReadString();

        RVX_CORE_WARN("Connection denied by {}: {} - {}", 
                      source.ToString(),
                      DisconnectReasonToString(reason),
                      message);

        if (IsClient())
        {
            auto conn = GetServerConnection();
            if (conn)
            {
                conn->SetState(ConnectionState::Failed);
                if (m_onDisconnect)
                {
                    m_onDisconnect(conn, reason);
                }
            }
        }
    }

    void NetworkManager::HandleDisconnect(const NetworkAddress& source, BitReader& reader)
    {
        auto reason = static_cast<DisconnectReason>(reader.ReadUInt8());
        
        auto conn = FindConnectionByAddress(source);
        if (conn)
        {
            RemoveConnection(conn->GetId(), reason);
        }
    }

    void NetworkManager::HandlePing(const NetworkAddress& source, BitReader& reader)
    {
        uint32 sequence = reader.ReadUInt32();
        uint64 timestamp = reader.ReadUInt64();

        auto pongPacket = CreatePong(sequence, timestamp);
        m_reliable->SendUnreliable(source, pongPacket.GetData(), 0);
    }

    void NetworkManager::HandlePong(const NetworkAddress& source, BitReader& reader)
    {
        uint32 sequence = reader.ReadUInt32();
        uint64 pingTimestamp = reader.ReadUInt64();
        (void)sequence;
        (void)pingTimestamp;
        
        auto conn = FindConnectionByAddress(source);
        if (conn)
        {
            conn->MarkActivity();
        }
    }

    void NetworkManager::HandleUserData(const NetworkAddress& source, std::span<const uint8> payload)
    {
        auto conn = FindConnectionByAddress(source);
        if (!conn)
            return;

        conn->MarkActivity();

        if (m_onData)
        {
            m_onData(conn, payload, 0);
        }
    }

    NetworkAddress NetworkManager::GetLocalAddress() const
    {
        if (m_transport)
            return m_transport->GetLocalAddress();
        return {};
    }

    ConnectionPtr NetworkManager::GetConnection(ConnectionId id) const
    {
        std::lock_guard<std::mutex> lock(m_connectionsMutex);
        auto it = m_connections.find(id);
        if (it != m_connections.end())
            return it->second;
        return nullptr;
    }

    ConnectionPtr NetworkManager::GetServerConnection() const
    {
        if (m_serverConnectionId == RVX_NET_INVALID_CONNECTION_ID)
            return nullptr;
        return GetConnection(m_serverConnectionId);
    }

    std::vector<ConnectionPtr> NetworkManager::GetConnections() const
    {
        std::lock_guard<std::mutex> lock(m_connectionsMutex);
        std::vector<ConnectionPtr> result;
        result.reserve(m_connections.size());
        for (const auto& [id, conn] : m_connections)
        {
            result.push_back(conn);
        }
        return result;
    }

    uint32 NetworkManager::GetConnectionCount() const
    {
        std::lock_guard<std::mutex> lock(m_connectionsMutex);
        return static_cast<uint32>(m_connections.size());
    }

    void NetworkManager::Disconnect(ConnectionId id, DisconnectReason reason)
    {
        auto conn = GetConnection(id);
        if (!conn)
            return;

        // Send disconnect packet
        auto disconnectPacket = CreateDisconnect(reason);
        m_reliable->SendReliable(conn->GetRemoteAddress(), disconnectPacket.GetData(), 0);

        RemoveConnection(id, reason);
    }

    void NetworkManager::DisconnectAll(DisconnectReason reason)
    {
        auto connections = GetConnections();
        for (const auto& conn : connections)
        {
            Disconnect(conn->GetId(), reason);
        }
    }

    void NetworkManager::Send(ConnectionId connectionId,
                              std::span<const uint8> data,
                              DeliveryMode mode,
                              ChannelId channel)
    {
        auto conn = GetConnection(connectionId);
        if (!conn || !conn->IsConnected())
            return;

        // Wrap in packet
        Packet packet;
        packet.SetType(PacketType::UserData);
        packet.SetConnectionId(connectionId);
        packet.SetPayload(data);

        m_reliable->Send(conn->GetRemoteAddress(), packet.GetData(), mode, channel);
    }

    void NetworkManager::Broadcast(std::span<const uint8> data,
                                   DeliveryMode mode,
                                   ChannelId channel)
    {
        auto connections = GetConnections();
        for (const auto& conn : connections)
        {
            if (conn->IsConnected())
            {
                Send(conn->GetId(), data, mode, channel);
            }
        }
    }

    void NetworkManager::SendPacket(ConnectionId connectionId,
                                    const Packet& packet,
                                    DeliveryMode mode,
                                    ChannelId channel)
    {
        auto conn = GetConnection(connectionId);
        if (!conn || !conn->IsConnected())
            return;

        m_reliable->Send(conn->GetRemoteAddress(), packet.GetData(), mode, channel);
    }

    void NetworkManager::BroadcastPacket(const Packet& packet,
                                          DeliveryMode mode,
                                          ChannelId channel)
    {
        auto connections = GetConnections();
        for (const auto& conn : connections)
        {
            if (conn->IsConnected())
            {
                SendPacket(conn->GetId(), packet, mode, channel);
            }
        }
    }

    void NetworkManager::SendToServer(std::span<const uint8> data,
                                       DeliveryMode mode,
                                       ChannelId channel)
    {
        if (m_serverConnectionId != RVX_NET_INVALID_CONNECTION_ID)
        {
            Send(m_serverConnectionId, data, mode, channel);
        }
    }

    void NetworkManager::SetOnConnect(OnConnectionCallback callback)
    {
        m_onConnect = std::move(callback);
    }

    void NetworkManager::SetOnDisconnect(OnDisconnectCallback callback)
    {
        m_onDisconnect = std::move(callback);
    }

    void NetworkManager::SetOnData(OnDataCallback callback)
    {
        m_onData = std::move(callback);
    }

    NetworkStats NetworkManager::GetStats() const
    {
        NetworkStats stats;
        stats.activeConnections = GetConnectionCount();

        auto connections = GetConnections();
        for (const auto& conn : connections)
        {
            const auto& connStats = conn->GetStats();
            stats.totalPacketsSent += connStats.packetsSent;
            stats.totalPacketsReceived += connStats.packetsReceived;
            stats.totalBytesSent += connStats.bytesSent;
            stats.totalBytesReceived += connStats.bytesReceived;
            stats.averageRTT += connStats.roundTripTimeMs;
            stats.averagePacketLoss += connStats.packetLossPercent;
        }

        if (!connections.empty())
        {
            stats.averageRTT /= static_cast<float32>(connections.size());
            stats.averagePacketLoss /= static_cast<float32>(connections.size());
        }

        return stats;
    }

    ConnectionPtr NetworkManager::CreateConnection(const NetworkAddress& address)
    {
        std::lock_guard<std::mutex> lock(m_connectionsMutex);
        
        ConnectionId id = m_nextConnectionId++;
        auto conn = std::make_shared<Connection>(id, address);
        
        m_connections[id] = conn;
        m_addressToConnection[AddressToKey(address)] = id;
        
        return conn;
    }

    void NetworkManager::RemoveConnection(ConnectionId id, DisconnectReason reason)
    {
        ConnectionPtr conn;
        
        {
            std::lock_guard<std::mutex> lock(m_connectionsMutex);
            auto it = m_connections.find(id);
            if (it == m_connections.end())
                return;
            
            conn = it->second;
            m_addressToConnection.erase(AddressToKey(conn->GetRemoteAddress()));
            m_connections.erase(it);
        }

        conn->SetState(ConnectionState::Disconnected);
        m_reliable->ResetAddress(conn->GetRemoteAddress());

        RVX_CORE_INFO("Connection {} disconnected: {}", id, DisconnectReasonToString(reason));

        if (m_onDisconnect)
        {
            m_onDisconnect(conn, reason);
        }

        if (id == m_serverConnectionId)
        {
            m_serverConnectionId = RVX_NET_INVALID_CONNECTION_ID;
        }
    }

    std::string NetworkManager::AddressToKey(const NetworkAddress& address) const
    {
        return address.host + ":" + std::to_string(address.port);
    }

    ConnectionPtr NetworkManager::FindConnectionByAddress(const NetworkAddress& address) const
    {
        std::lock_guard<std::mutex> lock(m_connectionsMutex);
        auto it = m_addressToConnection.find(AddressToKey(address));
        if (it != m_addressToConnection.end())
        {
            auto connIt = m_connections.find(it->second);
            if (connIt != m_connections.end())
                return connIt->second;
        }
        return nullptr;
    }

    void NetworkManager::SendConnectionAccepted(const NetworkAddress& address, ConnectionId id)
    {
        auto packet = CreateConnectionAccepted(id);
        m_reliable->SendReliable(address, packet.GetData(), 0);
    }

    void NetworkManager::SendConnectionDenied(const NetworkAddress& address, DisconnectReason reason)
    {
        auto packet = CreateConnectionDenied(reason);
        m_reliable->SendReliable(address, packet.GetData(), 0);
    }

} // namespace RVX::Net
