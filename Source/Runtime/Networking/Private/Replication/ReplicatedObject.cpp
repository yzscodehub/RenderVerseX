#include "Networking/Replication/ReplicatedObject.h"
#include "Networking/NetworkManager.h"
#include "Core/Log.h"

namespace RVX::Net
{
    // =========================================================================
    // ReplicationManager Implementation
    // =========================================================================

    ReplicationManager::ReplicationManager()
    {
        m_serializationBuffer.reserve(RVX_NET_MAX_PACKET_SIZE);
    }

    ReplicationManager::~ReplicationManager()
    {
        Shutdown();
    }

    void ReplicationManager::Initialize(NetworkManager* networkManager)
    {
        m_networkManager = networkManager;
        RVX_CORE_INFO("ReplicationManager initialized");
    }

    void ReplicationManager::Shutdown()
    {
        m_objects.clear();
        m_dirtyObjects.clear();
        m_lastSyncTime.clear();
        m_nextNetId = 1;
        m_networkManager = nullptr;
    }

    void ReplicationManager::RegisterTypeInternal(const char* typeName, ReplicatedObjectFactory factory)
    {
        m_factories[typeName] = std::move(factory);
        RVX_CORE_DEBUG("Registered replicated type: {}", typeName);
    }

    NetObjectId ReplicationManager::Spawn(ReplicatedObjectPtr obj, ConnectionId owner)
    {
        if (!obj)
            return RVX_NET_INVALID_OBJECT_ID;

        NetObjectId netId = m_nextNetId++;
        obj->SetNetId(netId);
        obj->SetOwnerId(owner);

        // Set authority based on network role
        if (m_networkManager && m_networkManager->IsServer())
        {
            obj->SetAuthority(true);
        }

        m_objects[netId] = obj;
        obj->OnNetworkSpawn();

        // Broadcast spawn to all clients
        if (m_networkManager && m_networkManager->IsServer())
        {
            BroadcastSpawn(obj);
        }

        RVX_CORE_DEBUG("Spawned network object {} (type: {}, owner: {})", 
                       netId, obj->GetTypeName(), owner);

        return netId;
    }

    void ReplicationManager::Despawn(NetObjectId netId)
    {
        auto it = m_objects.find(netId);
        if (it == m_objects.end())
            return;

        auto obj = it->second;
        obj->OnNetworkDespawn();

        // Broadcast despawn
        if (m_networkManager && m_networkManager->IsServer())
        {
            BroadcastDespawn(netId);
        }

        m_objects.erase(it);
        m_lastSyncTime.erase(netId);

        // Remove from dirty list
        m_dirtyObjects.erase(
            std::remove(m_dirtyObjects.begin(), m_dirtyObjects.end(), netId),
            m_dirtyObjects.end());

        RVX_CORE_DEBUG("Despawned network object {}", netId);
    }

    ReplicatedObjectPtr ReplicationManager::GetObject(NetObjectId netId) const
    {
        auto it = m_objects.find(netId);
        if (it != m_objects.end())
            return it->second;
        return nullptr;
    }

    std::vector<ReplicatedObjectPtr> ReplicationManager::GetAllObjects() const
    {
        std::vector<ReplicatedObjectPtr> result;
        result.reserve(m_objects.size());
        for (const auto& [id, obj] : m_objects)
        {
            result.push_back(obj);
        }
        return result;
    }

    std::vector<ReplicatedObjectPtr> ReplicationManager::GetObjectsByOwner(ConnectionId owner) const
    {
        std::vector<ReplicatedObjectPtr> result;
        for (const auto& [id, obj] : m_objects)
        {
            if (obj->GetOwnerId() == owner)
            {
                result.push_back(obj);
            }
        }
        return result;
    }

    void ReplicationManager::Update(float32 deltaTime)
    {
        if (!m_networkManager || !m_networkManager->IsActive())
            return;

        // Only server replicates state
        if (!m_networkManager->IsServer())
            return;

        // Process dirty objects
        for (const auto& [netId, obj] : m_objects)
        {
            auto config = obj->GetReplicationConfig();
            
            // Check update rate
            float32 timeSinceLastSync = 0.0f;
            auto it = m_lastSyncTime.find(netId);
            if (it != m_lastSyncTime.end())
            {
                timeSinceLastSync = it->second + deltaTime;
                it->second = timeSinceLastSync;
            }
            else
            {
                m_lastSyncTime[netId] = 0.0f;
            }

            float32 minInterval = config.updateRate > 0.0f ? 1.0f / config.updateRate : 0.0f;
            
            if (timeSinceLastSync >= minInterval)
            {
                BroadcastState(obj);
                m_lastSyncTime[netId] = 0.0f;
            }
        }

        // Clear dirty list
        m_dirtyObjects.clear();
    }

    void ReplicationManager::ForceSync(NetObjectId netId)
    {
        auto obj = GetObject(netId);
        if (obj && m_networkManager && m_networkManager->IsServer())
        {
            BroadcastState(obj);
            m_lastSyncTime[netId] = 0.0f;
        }
    }

    void ReplicationManager::MarkDirty(NetObjectId netId)
    {
        if (std::find(m_dirtyObjects.begin(), m_dirtyObjects.end(), netId) == m_dirtyObjects.end())
        {
            m_dirtyObjects.push_back(netId);
        }
    }

    void ReplicationManager::TransferAuthority(NetObjectId netId, ConnectionId newAuthority)
    {
        auto obj = GetObject(netId);
        if (!obj)
            return;

        ConnectionId oldOwner = obj->GetOwnerId();
        obj->SetOwnerId(newAuthority);
        obj->OnOwnershipChanged(newAuthority);

        // Update authority
        if (m_networkManager)
        {
            bool isLocal = (newAuthority == RVX_NET_INVALID_CONNECTION_ID && m_networkManager->IsServer());
            obj->SetAuthority(isLocal);
        }

        RVX_CORE_DEBUG("Transferred authority of object {} from {} to {}", 
                       netId, oldOwner, newAuthority);

        // Notify clients
        ForceSync(netId);
    }

    void ReplicationManager::RequestAuthority(NetObjectId netId)
    {
        // Client requesting authority - send to server
        if (m_networkManager && m_networkManager->IsClient())
        {
            // TODO: Implement authority request packet
            RVX_CORE_DEBUG("Requesting authority for object {}", netId);
        }
    }

    void ReplicationManager::BroadcastSpawn(const ReplicatedObjectPtr& obj)
    {
        if (!m_networkManager)
            return;

        m_serializationBuffer.clear();
        BitWriter writer(m_serializationBuffer.data(), RVX_NET_MAX_PACKET_SIZE);

        // Write spawn packet header
        writer.WriteUInt8(static_cast<uint8>(PacketType::Replication));
        writer.WriteUInt8(0x01); // Spawn command
        writer.WriteUInt32(obj->GetNetId());
        writer.WriteUInt32(obj->GetOwnerId());
        writer.WriteString(obj->GetTypeName());

        // Write initial state
        NetworkWriter netWriter(writer);
        obj->SerializeState(netWriter);

        auto data = writer.GetSpan();
        m_networkManager->Broadcast(data, DeliveryMode::ReliableOrdered, 
                                    static_cast<ChannelId>(ChannelType::Spawn));
    }

    void ReplicationManager::BroadcastDespawn(NetObjectId netId)
    {
        if (!m_networkManager)
            return;

        m_serializationBuffer.clear();
        BitWriter writer(m_serializationBuffer.data(), RVX_NET_MAX_PACKET_SIZE);

        writer.WriteUInt8(static_cast<uint8>(PacketType::Replication));
        writer.WriteUInt8(0x02); // Despawn command
        writer.WriteUInt32(netId);

        auto data = writer.GetSpan();
        m_networkManager->Broadcast(data, DeliveryMode::ReliableOrdered,
                                    static_cast<ChannelId>(ChannelType::Spawn));
    }

    void ReplicationManager::BroadcastState(const ReplicatedObjectPtr& obj)
    {
        if (!m_networkManager)
            return;

        m_serializationBuffer.clear();
        BitWriter writer(m_serializationBuffer.data(), RVX_NET_MAX_PACKET_SIZE);

        writer.WriteUInt8(static_cast<uint8>(PacketType::Replication));
        writer.WriteUInt8(0x03); // State update command
        writer.WriteUInt32(obj->GetNetId());

        // Write state
        NetworkWriter netWriter(writer);
        obj->SerializeState(netWriter);

        auto data = writer.GetSpan();
        
        // Use appropriate delivery mode based on priority
        auto config = obj->GetReplicationConfig();
        DeliveryMode mode = (config.priority == ReplicationPriority::Critical) ?
            DeliveryMode::ReliableOrdered : DeliveryMode::UnreliableSequenced;

        m_networkManager->Broadcast(data, mode,
                                    static_cast<ChannelId>(ChannelType::Replication));
    }

    void ReplicationManager::HandleSpawnPacket(ConnectionId source, BitReader& reader)
    {
        (void)source;
        
        NetObjectId netId = reader.ReadUInt32();
        ConnectionId ownerId = reader.ReadUInt32();
        std::string typeName = reader.ReadString();

        // Check if we already have this object
        if (GetObject(netId))
        {
            RVX_CORE_WARN("Received duplicate spawn for object {}", netId);
            return;
        }

        // Create object from factory
        auto factoryIt = m_factories.find(typeName);
        if (factoryIt == m_factories.end())
        {
            RVX_CORE_ERROR("Unknown replicated type: {}", typeName);
            return;
        }

        auto obj = factoryIt->second();
        obj->SetNetId(netId);
        obj->SetOwnerId(ownerId);

        // Read initial state
        NetworkReader netReader(reader);
        obj->DeserializeState(netReader);

        // Determine local control
        if (m_networkManager && m_networkManager->IsClient())
        {
            auto serverConn = m_networkManager->GetServerConnection();
            bool isLocallyControlled = serverConn && (ownerId == serverConn->GetId());
            obj->SetLocallyControlled(isLocallyControlled);
            obj->SetAuthority(isLocallyControlled);
        }

        m_objects[netId] = obj;
        if (netId >= m_nextNetId)
        {
            m_nextNetId = netId + 1;
        }

        obj->OnNetworkSpawn();

        RVX_CORE_DEBUG("Remote spawn: object {} (type: {})", netId, typeName);
    }

    void ReplicationManager::HandleDespawnPacket(ConnectionId source, BitReader& reader)
    {
        (void)source;
        
        NetObjectId netId = reader.ReadUInt32();
        
        auto it = m_objects.find(netId);
        if (it != m_objects.end())
        {
            it->second->OnNetworkDespawn();
            m_objects.erase(it);
            RVX_CORE_DEBUG("Remote despawn: object {}", netId);
        }
    }

    void ReplicationManager::HandleStatePacket(ConnectionId source, BitReader& reader)
    {
        (void)source;
        
        NetObjectId netId = reader.ReadUInt32();
        
        auto obj = GetObject(netId);
        if (!obj)
        {
            // Object doesn't exist yet - might receive before spawn
            return;
        }

        // Only apply state if we don't have authority
        if (obj->HasAuthority())
            return;

        NetworkReader netReader(reader);
        obj->DeserializeState(netReader);
    }

} // namespace RVX::Net
