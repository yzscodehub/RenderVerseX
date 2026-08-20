#pragma once

/**
 * @file ReplicatedObject.h
 * @brief Network object replication system
 * 
 * Provides automatic synchronization of object state across the network.
 */

#include "Networking/NetworkTypes.h"
#include "Networking/Serialization/NetworkSerializer.h"

#include <functional>
#include <memory>
#include <vector>
#include <unordered_map>
#include <string>

namespace RVX::Net
{
    // Forward declarations
    class NetworkManager;
    class PropertyReplicator;

    /**
     * @brief Network object identifier
     */
    using NetObjectId = uint32;
    
    constexpr NetObjectId RVX_NET_INVALID_OBJECT_ID = 0;

    /**
     * @brief Replication mode for objects
     */
    enum class ReplicationMode : uint8
    {
        None = 0,           ///< No replication
        ServerOnly,         ///< Only exists on server
        ClientOnly,         ///< Only exists on client that owns it
        ServerToClients,    ///< Server authoritative, replicated to all clients
        ClientToServer,     ///< Client authoritative, sent to server
        PeerToPeer          ///< Replicated to all peers
    };

    /**
     * @brief Network authority (who controls the object)
     */
    enum class NetworkAuthority : uint8
    {
        Server = 0,         ///< Server controls this object
        Client,             ///< A specific client controls this object
        Local               ///< Local authority (for predicted objects)
    };

    /**
     * @brief Replication priority
     */
    enum class ReplicationPriority : uint8
    {
        Low = 0,
        Normal = 1,
        High = 2,
        Critical = 3
    };

    /**
     * @brief Replicated object configuration
     */
    struct ReplicationConfig
    {
        /// Replication mode
        ReplicationMode mode = ReplicationMode::ServerToClients;
        
        /// Replication priority
        ReplicationPriority priority = ReplicationPriority::Normal;
        
        /// Update rate in Hz (0 = as fast as possible)
        float32 updateRate = 20.0f;
        
        /// Enable delta compression
        bool deltaCompression = true;
        
        /// Relevancy distance (0 = always relevant)
        float32 relevancyDistance = 0.0f;
    };

    /**
     * @brief Base class for replicated network objects
     * 
     * Inherit from this class to create objects that can be
     * automatically synchronized across the network.
     */
    class IReplicatedObject
    {
    public:
        virtual ~IReplicatedObject() = default;

        // =====================================================================
        // Identification
        // =====================================================================

        /**
         * @brief Get network object ID
         */
        NetObjectId GetNetId() const { return m_netId; }

        /**
         * @brief Get object type name
         */
        virtual const char* GetTypeName() const = 0;

        /**
         * @brief Get owner connection ID
         */
        ConnectionId GetOwnerId() const { return m_ownerId; }

        /**
         * @brief Check if this object is locally controlled
         */
        bool IsLocallyControlled() const { return m_isLocallyControlled; }

        /**
         * @brief Check if this object has authority
         */
        bool HasAuthority() const { return m_hasAuthority; }

        // =====================================================================
        // Replication
        // =====================================================================

        /**
         * @brief Get replication configuration
         */
        virtual ReplicationConfig GetReplicationConfig() const { return {}; }

        /**
         * @brief Serialize full state
         */
        virtual void SerializeState(NetworkWriter& writer) const = 0;

        /**
         * @brief Deserialize full state
         */
        virtual void DeserializeState(NetworkReader& reader) = 0;

        /**
         * @brief Serialize delta (changes only)
         */
        virtual void SerializeDelta(NetworkWriter& writer) const
        {
            SerializeState(writer);
        }

        /**
         * @brief Deserialize delta
         */
        virtual void DeserializeDelta(NetworkReader& reader)
        {
            DeserializeState(reader);
        }

        /**
         * @brief Called when object is spawned on network
         */
        virtual void OnNetworkSpawn() {}

        /**
         * @brief Called when object is despawned from network
         */
        virtual void OnNetworkDespawn() {}

        /**
         * @brief Called when authority changes
         */
        virtual void OnAuthorityChanged(bool hasAuthority) { (void)hasAuthority; }

        /**
         * @brief Called when ownership changes
         */
        virtual void OnOwnershipChanged(ConnectionId newOwner) { (void)newOwner; }

        // =====================================================================
        // Internal (set by replication system)
        // =====================================================================

        void SetNetId(NetObjectId id) { m_netId = id; }
        void SetOwnerId(ConnectionId id) { m_ownerId = id; }
        void SetLocallyControlled(bool controlled) { m_isLocallyControlled = controlled; }
        void SetAuthority(bool authority) 
        { 
            if (m_hasAuthority != authority)
            {
                m_hasAuthority = authority;
                OnAuthorityChanged(authority);
            }
        }

    protected:
        NetObjectId m_netId = RVX_NET_INVALID_OBJECT_ID;
        ConnectionId m_ownerId = RVX_NET_INVALID_CONNECTION_ID;
        bool m_isLocallyControlled = false;
        bool m_hasAuthority = false;
    };

    /**
     * @brief Shared pointer for replicated objects
     */
    using ReplicatedObjectPtr = std::shared_ptr<IReplicatedObject>;

    /**
     * @brief Factory function for creating replicated objects
     */
    using ReplicatedObjectFactory = std::function<ReplicatedObjectPtr()>;

    /**
     * @brief Manages replicated objects
     */
    class ReplicationManager
    {
    public:
        ReplicationManager();
        ~ReplicationManager();

        // =====================================================================
        // Setup
        // =====================================================================

        /**
         * @brief Initialize with network manager
         */
        void Initialize(NetworkManager* networkManager);

        /**
         * @brief Shutdown
         */
        void Shutdown();

        /**
         * @brief Register a replicated object type
         */
        template<typename T>
        void RegisterType()
        {
            static_assert(std::is_base_of_v<IReplicatedObject, T>,
                          "Type must derive from IReplicatedObject");
            
            T temp;
            RegisterTypeInternal(temp.GetTypeName(), []() -> ReplicatedObjectPtr {
                return std::make_shared<T>();
            });
        }

        /**
         * @brief Register type with factory
         */
        void RegisterTypeInternal(const char* typeName, ReplicatedObjectFactory factory);

        // =====================================================================
        // Object Management
        // =====================================================================

        /**
         * @brief Spawn a replicated object (server)
         * @param obj Object to spawn
         * @param owner Owner connection (or RVX_NET_INVALID_CONNECTION_ID for server)
         * @return Network ID assigned to the object
         */
        NetObjectId Spawn(ReplicatedObjectPtr obj, ConnectionId owner = RVX_NET_INVALID_CONNECTION_ID);

        /**
         * @brief Despawn a replicated object (server)
         */
        void Despawn(NetObjectId netId);

        /**
         * @brief Get object by network ID
         */
        ReplicatedObjectPtr GetObject(NetObjectId netId) const;

        /**
         * @brief Get all replicated objects
         */
        std::vector<ReplicatedObjectPtr> GetAllObjects() const;

        /**
         * @brief Get objects owned by a connection
         */
        std::vector<ReplicatedObjectPtr> GetObjectsByOwner(ConnectionId owner) const;

        // =====================================================================
        // Update
        // =====================================================================

        /**
         * @brief Update replication (call each frame)
         */
        void Update(float32 deltaTime);

        /**
         * @brief Force sync an object immediately
         */
        void ForceSync(NetObjectId netId);

        /**
         * @brief Mark object as dirty (needs sync)
         */
        void MarkDirty(NetObjectId netId);

        // =====================================================================
        // Authority
        // =====================================================================

        /**
         * @brief Transfer authority to a client
         */
        void TransferAuthority(NetObjectId netId, ConnectionId newAuthority);

        /**
         * @brief Request authority (client to server)
         */
        void RequestAuthority(NetObjectId netId);

    private:
        NetworkManager* m_networkManager = nullptr;
        
        // Object registry
        std::unordered_map<NetObjectId, ReplicatedObjectPtr> m_objects;
        std::unordered_map<std::string, ReplicatedObjectFactory> m_factories;
        NetObjectId m_nextNetId = 1;
        
        // Dirty tracking
        std::vector<NetObjectId> m_dirtyObjects;
        std::unordered_map<NetObjectId, float32> m_lastSyncTime;
        
        // Serialization buffer
        std::vector<uint8> m_serializationBuffer;
        
        void BroadcastSpawn(const ReplicatedObjectPtr& obj);
        void BroadcastDespawn(NetObjectId netId);
        void BroadcastState(const ReplicatedObjectPtr& obj);
        void HandleSpawnPacket(ConnectionId source, BitReader& reader);
        void HandleDespawnPacket(ConnectionId source, BitReader& reader);
        void HandleStatePacket(ConnectionId source, BitReader& reader);
    };

    /**
     * @brief Macro for declaring replicated object type
     */
    #define RVX_REPLICATED_OBJECT(ClassName) \
        const char* GetTypeName() const override { return #ClassName; }

} // namespace RVX::Net
