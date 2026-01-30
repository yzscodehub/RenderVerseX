#pragma once

/**
 * @file PropertyReplication.h
 * @brief Property-level replication with dirty tracking
 * 
 * Provides automatic tracking of property changes and
 * delta serialization for bandwidth optimization.
 */

#include "Networking/NetworkTypes.h"
#include "Networking/Serialization/NetworkSerializer.h"
#include "Core/MathTypes.h"

#include <functional>
#include <vector>
#include <string>
#include <any>
#include <bitset>
#include <cstring>

namespace RVX::Net
{
    /// Maximum properties per object
    constexpr uint32 RVX_NET_MAX_REPLICATED_PROPERTIES = 64;

    /**
     * @brief Property replication condition
     */
    enum class ReplicationCondition : uint8
    {
        Always = 0,         ///< Always replicate
        OnChange,           ///< Only when changed
        InitialOnly,        ///< Only on spawn
        OwnerOnly,          ///< Only to owner
        SkipOwner,          ///< To everyone except owner
        Custom              ///< Custom condition function
    };

    /**
     * @brief Property type identifiers
     */
    enum class PropertyType : uint8
    {
        Unknown = 0,
        Bool,
        Int8,
        UInt8,
        Int16,
        UInt16,
        Int32,
        UInt32,
        Int64,
        UInt64,
        Float32,
        Float64,
        String,
        Vec2,
        Vec3,
        Vec4,
        Quat,
        Custom
    };

    /**
     * @brief Property descriptor
     */
    struct PropertyDescriptor
    {
        std::string name;
        PropertyType type = PropertyType::Unknown;
        uint32 offset = 0;  // Offset in object
        uint32 size = 0;    // Size in bytes
        ReplicationCondition condition = ReplicationCondition::OnChange;
        
        /// Custom serialize function (for Custom type)
        std::function<void(const void*, NetworkWriter&)> serialize;
        
        /// Custom deserialize function (for Custom type)
        std::function<void(void*, NetworkReader&)> deserialize;
        
        /// Custom comparison function (for change detection)
        std::function<bool(const void*, const void*)> compare;
    };

    /**
     * @brief Tracks property changes for delta replication
     */
    class PropertyTracker
    {
    public:
        PropertyTracker() = default;
        explicit PropertyTracker(uint32 propertyCount);

        /**
         * @brief Mark a property as dirty
         */
        void MarkDirty(uint32 propertyIndex);

        /**
         * @brief Clear dirty flag for a property
         */
        void ClearDirty(uint32 propertyIndex);

        /**
         * @brief Clear all dirty flags
         */
        void ClearAll();

        /**
         * @brief Check if a property is dirty
         */
        bool IsDirty(uint32 propertyIndex) const;

        /**
         * @brief Check if any property is dirty
         */
        bool HasDirtyProperties() const;

        /**
         * @brief Get dirty flags as bitfield
         */
        uint64 GetDirtyBits() const { return m_dirtyBits.to_ullong(); }

        /**
         * @brief Set dirty flags from bitfield
         */
        void SetDirtyBits(uint64 bits);

        /**
         * @brief Get count of dirty properties
         */
        uint32 GetDirtyCount() const { return static_cast<uint32>(m_dirtyBits.count()); }

    private:
        std::bitset<RVX_NET_MAX_REPLICATED_PROPERTIES> m_dirtyBits;
    };

    /**
     * @brief Helper class for property-based replication
     */
    class PropertyReplicator
    {
    public:
        PropertyReplicator() = default;

        // =====================================================================
        // Property Registration
        // =====================================================================

        /**
         * @brief Register a property for replication
         */
        template<typename T>
        uint32 RegisterProperty(const std::string& name, T* ptr, 
                                 ReplicationCondition condition = ReplicationCondition::OnChange)
        {
            PropertyDescriptor desc;
            desc.name = name;
            desc.type = GetPropertyType<T>();
            desc.offset = reinterpret_cast<uintptr_t>(ptr);
            desc.size = sizeof(T);
            desc.condition = condition;
            
            return AddProperty(desc);
        }

        /**
         * @brief Register a custom property
         */
        uint32 RegisterCustomProperty(
            const std::string& name,
            uint32 size,
            void* ptr,
            std::function<void(const void*, NetworkWriter&)> serialize,
            std::function<void(void*, NetworkReader&)> deserialize,
            ReplicationCondition condition = ReplicationCondition::OnChange);

        /**
         * @brief Get property count
         */
        uint32 GetPropertyCount() const { return static_cast<uint32>(m_properties.size()); }

        /**
         * @brief Get property by index
         */
        const PropertyDescriptor* GetProperty(uint32 index) const;

        /**
         * @brief Get property by name
         */
        const PropertyDescriptor* GetPropertyByName(const std::string& name) const;

        // =====================================================================
        // Change Detection
        // =====================================================================

        /**
         * @brief Store current values as baseline
         */
        void StoreBaseline(const void* object);

        /**
         * @brief Compare current values to baseline and update dirty flags
         */
        void DetectChanges(const void* object, PropertyTracker& tracker);

        /**
         * @brief Get baseline data
         */
        const std::vector<uint8>& GetBaseline() const { return m_baseline; }

        // =====================================================================
        // Serialization
        // =====================================================================

        /**
         * @brief Serialize all properties
         */
        void SerializeAll(const void* object, NetworkWriter& writer) const;

        /**
         * @brief Serialize only dirty properties
         */
        void SerializeDirty(const void* object, const PropertyTracker& tracker, 
                            NetworkWriter& writer) const;

        /**
         * @brief Deserialize all properties
         */
        void DeserializeAll(void* object, NetworkReader& reader) const;

        /**
         * @brief Deserialize dirty properties (reads dirty bits first)
         */
        void DeserializeDirty(void* object, NetworkReader& reader) const;

    private:
        std::vector<PropertyDescriptor> m_properties;
        std::vector<uint8> m_baseline;

        uint32 AddProperty(const PropertyDescriptor& desc);
        
        void SerializeProperty(const void* object, const PropertyDescriptor& prop,
                               NetworkWriter& writer) const;
        void DeserializeProperty(void* object, const PropertyDescriptor& prop,
                                  NetworkReader& reader) const;
        bool CompareProperty(const void* object, const PropertyDescriptor& prop,
                             const uint8* baseline) const;

        template<typename T>
        static constexpr PropertyType GetPropertyType()
        {
            if constexpr (std::is_same_v<T, bool>) return PropertyType::Bool;
            else if constexpr (std::is_same_v<T, int8>) return PropertyType::Int8;
            else if constexpr (std::is_same_v<T, uint8>) return PropertyType::UInt8;
            else if constexpr (std::is_same_v<T, int16>) return PropertyType::Int16;
            else if constexpr (std::is_same_v<T, uint16>) return PropertyType::UInt16;
            else if constexpr (std::is_same_v<T, int32>) return PropertyType::Int32;
            else if constexpr (std::is_same_v<T, uint32>) return PropertyType::UInt32;
            else if constexpr (std::is_same_v<T, int64>) return PropertyType::Int64;
            else if constexpr (std::is_same_v<T, uint64>) return PropertyType::UInt64;
            else if constexpr (std::is_same_v<T, float32>) return PropertyType::Float32;
            else if constexpr (std::is_same_v<T, float64>) return PropertyType::Float64;
            else if constexpr (std::is_same_v<T, std::string>) return PropertyType::String;
            else if constexpr (std::is_same_v<T, Vec2>) return PropertyType::Vec2;
            else if constexpr (std::is_same_v<T, Vec3>) return PropertyType::Vec3;
            else if constexpr (std::is_same_v<T, Vec4>) return PropertyType::Vec4;
            else if constexpr (std::is_same_v<T, Quat>) return PropertyType::Quat;
            else return PropertyType::Custom;
        }
    };

    /**
     * @brief Macro for registering replicated properties
     * 
     * Usage in constructor or setup method:
     * @code
     * class MyObject : public IReplicatedObject
     * {
     *     Vec3 m_position;
     *     float32 m_health;
     *     
     *     void SetupReplication()
     *     {
     *         RVX_REPLICATE(m_position);
     *         RVX_REPLICATE_CONDITION(m_health, ReplicationCondition::OnChange);
     *     }
     * };
     * @endcode
     */
    #define RVX_REPLICATE(member) \
        m_replicator.RegisterProperty(#member, &member)

    #define RVX_REPLICATE_CONDITION(member, condition) \
        m_replicator.RegisterProperty(#member, &member, condition)

} // namespace RVX::Net
