#include "Networking/Replication/PropertyReplication.h"

#include <algorithm>

namespace RVX::Net
{
    // =========================================================================
    // PropertyTracker Implementation
    // =========================================================================

    PropertyTracker::PropertyTracker(uint32 propertyCount)
    {
        // Mark all properties as dirty initially
        for (uint32 i = 0; i < propertyCount && i < RVX_NET_MAX_REPLICATED_PROPERTIES; ++i)
        {
            m_dirtyBits.set(i);
        }
    }

    void PropertyTracker::MarkDirty(uint32 propertyIndex)
    {
        if (propertyIndex < RVX_NET_MAX_REPLICATED_PROPERTIES)
        {
            m_dirtyBits.set(propertyIndex);
        }
    }

    void PropertyTracker::ClearDirty(uint32 propertyIndex)
    {
        if (propertyIndex < RVX_NET_MAX_REPLICATED_PROPERTIES)
        {
            m_dirtyBits.reset(propertyIndex);
        }
    }

    void PropertyTracker::ClearAll()
    {
        m_dirtyBits.reset();
    }

    bool PropertyTracker::IsDirty(uint32 propertyIndex) const
    {
        if (propertyIndex < RVX_NET_MAX_REPLICATED_PROPERTIES)
        {
            return m_dirtyBits.test(propertyIndex);
        }
        return false;
    }

    bool PropertyTracker::HasDirtyProperties() const
    {
        return m_dirtyBits.any();
    }

    void PropertyTracker::SetDirtyBits(uint64 bits)
    {
        m_dirtyBits = std::bitset<RVX_NET_MAX_REPLICATED_PROPERTIES>(bits);
    }

    // =========================================================================
    // PropertyReplicator Implementation
    // =========================================================================

    uint32 PropertyReplicator::RegisterCustomProperty(
        const std::string& name,
        uint32 size,
        void* ptr,
        std::function<void(const void*, NetworkWriter&)> serialize,
        std::function<void(void*, NetworkReader&)> deserialize,
        ReplicationCondition condition)
    {
        PropertyDescriptor desc;
        desc.name = name;
        desc.type = PropertyType::Custom;
        desc.offset = reinterpret_cast<uintptr_t>(ptr);
        desc.size = size;
        desc.condition = condition;
        desc.serialize = std::move(serialize);
        desc.deserialize = std::move(deserialize);
        
        return AddProperty(desc);
    }

    uint32 PropertyReplicator::AddProperty(const PropertyDescriptor& desc)
    {
        uint32 index = static_cast<uint32>(m_properties.size());
        if (index >= RVX_NET_MAX_REPLICATED_PROPERTIES)
        {
            return RVX_INVALID_INDEX;
        }
        
        m_properties.push_back(desc);
        return index;
    }

    const PropertyDescriptor* PropertyReplicator::GetProperty(uint32 index) const
    {
        if (index < m_properties.size())
        {
            return &m_properties[index];
        }
        return nullptr;
    }

    const PropertyDescriptor* PropertyReplicator::GetPropertyByName(const std::string& name) const
    {
        for (const auto& prop : m_properties)
        {
            if (prop.name == name)
            {
                return &prop;
            }
        }
        return nullptr;
    }

    void PropertyReplicator::StoreBaseline(const void* object)
    {
        // Calculate total size needed
        uint32 totalSize = 0;
        for (const auto& prop : m_properties)
        {
            totalSize += prop.size;
        }

        m_baseline.resize(totalSize);

        // Copy each property
        uint32 offset = 0;
        for (const auto& prop : m_properties)
        {
            const uint8* src = reinterpret_cast<const uint8*>(prop.offset);
            std::memcpy(m_baseline.data() + offset, src, prop.size);
            offset += prop.size;
        }
    }

    void PropertyReplicator::DetectChanges(const void* object, PropertyTracker& tracker)
    {
        (void)object;
        
        if (m_baseline.empty())
        {
            // No baseline - mark all dirty
            for (uint32 i = 0; i < m_properties.size(); ++i)
            {
                tracker.MarkDirty(i);
            }
            return;
        }

        uint32 baselineOffset = 0;
        for (uint32 i = 0; i < m_properties.size(); ++i)
        {
            const auto& prop = m_properties[i];
            
            if (!CompareProperty(object, prop, m_baseline.data() + baselineOffset))
            {
                tracker.MarkDirty(i);
            }
            
            baselineOffset += prop.size;
        }
    }

    bool PropertyReplicator::CompareProperty(const void* object, const PropertyDescriptor& prop,
                                              const uint8* baseline) const
    {
        (void)object;
        
        if (prop.compare)
        {
            return prop.compare(reinterpret_cast<const void*>(prop.offset), baseline);
        }

        // Default: memcmp
        const uint8* current = reinterpret_cast<const uint8*>(prop.offset);
        return std::memcmp(current, baseline, prop.size) == 0;
    }

    void PropertyReplicator::SerializeAll(const void* object, NetworkWriter& writer) const
    {
        (void)object;
        
        for (const auto& prop : m_properties)
        {
            SerializeProperty(object, prop, writer);
        }
    }

    void PropertyReplicator::SerializeDirty(const void* object, const PropertyTracker& tracker,
                                             NetworkWriter& writer) const
    {
        (void)object;
        
        // Write dirty bits
        writer.WriteUInt64(tracker.GetDirtyBits());

        // Write only dirty properties
        for (uint32 i = 0; i < m_properties.size(); ++i)
        {
            if (tracker.IsDirty(i))
            {
                SerializeProperty(object, m_properties[i], writer);
            }
        }
    }

    void PropertyReplicator::DeserializeAll(void* object, NetworkReader& reader) const
    {
        (void)object;
        
        for (const auto& prop : m_properties)
        {
            DeserializeProperty(object, prop, reader);
        }
    }

    void PropertyReplicator::DeserializeDirty(void* object, NetworkReader& reader) const
    {
        (void)object;
        
        // Read dirty bits
        uint64 dirtyBits = reader.ReadUInt64();

        // Read only dirty properties
        for (uint32 i = 0; i < m_properties.size(); ++i)
        {
            if (dirtyBits & (1ull << i))
            {
                DeserializeProperty(object, m_properties[i], reader);
            }
        }
    }

    void PropertyReplicator::SerializeProperty(const void* object, const PropertyDescriptor& prop,
                                                NetworkWriter& writer) const
    {
        (void)object;
        
        if (prop.serialize)
        {
            prop.serialize(reinterpret_cast<const void*>(prop.offset), writer);
            return;
        }

        const void* ptr = reinterpret_cast<const void*>(prop.offset);

        switch (prop.type)
        {
            case PropertyType::Bool:
                writer.WriteBool(*static_cast<const bool*>(ptr));
                break;
            case PropertyType::Int8:
                writer.WriteInt8(*static_cast<const int8*>(ptr));
                break;
            case PropertyType::UInt8:
                writer.WriteUInt8(*static_cast<const uint8*>(ptr));
                break;
            case PropertyType::Int16:
                writer.WriteInt16(*static_cast<const int16*>(ptr));
                break;
            case PropertyType::UInt16:
                writer.WriteUInt16(*static_cast<const uint16*>(ptr));
                break;
            case PropertyType::Int32:
                writer.WriteInt32(*static_cast<const int32*>(ptr));
                break;
            case PropertyType::UInt32:
                writer.WriteUInt32(*static_cast<const uint32*>(ptr));
                break;
            case PropertyType::Int64:
                writer.WriteInt64(*static_cast<const int64*>(ptr));
                break;
            case PropertyType::UInt64:
                writer.WriteUInt64(*static_cast<const uint64*>(ptr));
                break;
            case PropertyType::Float32:
                writer.WriteFloat32(*static_cast<const float32*>(ptr));
                break;
            case PropertyType::Float64:
                writer.WriteFloat64(*static_cast<const float64*>(ptr));
                break;
            case PropertyType::String:
                writer.WriteString(*static_cast<const std::string*>(ptr));
                break;
            case PropertyType::Vec2:
                writer.WriteVec2(*static_cast<const Vec2*>(ptr));
                break;
            case PropertyType::Vec3:
                writer.WriteVec3(*static_cast<const Vec3*>(ptr));
                break;
            case PropertyType::Vec4:
                writer.WriteVec4(*static_cast<const Vec4*>(ptr));
                break;
            case PropertyType::Quat:
                writer.WriteQuat(*static_cast<const Quat*>(ptr));
                break;
            default:
                break;
        }
    }

    void PropertyReplicator::DeserializeProperty(void* object, const PropertyDescriptor& prop,
                                                  NetworkReader& reader) const
    {
        (void)object;
        
        if (prop.deserialize)
        {
            prop.deserialize(reinterpret_cast<void*>(prop.offset), reader);
            return;
        }

        void* ptr = reinterpret_cast<void*>(prop.offset);

        switch (prop.type)
        {
            case PropertyType::Bool:
                *static_cast<bool*>(ptr) = reader.ReadBool();
                break;
            case PropertyType::Int8:
                *static_cast<int8*>(ptr) = reader.ReadInt8();
                break;
            case PropertyType::UInt8:
                *static_cast<uint8*>(ptr) = reader.ReadUInt8();
                break;
            case PropertyType::Int16:
                *static_cast<int16*>(ptr) = reader.ReadInt16();
                break;
            case PropertyType::UInt16:
                *static_cast<uint16*>(ptr) = reader.ReadUInt16();
                break;
            case PropertyType::Int32:
                *static_cast<int32*>(ptr) = reader.ReadInt32();
                break;
            case PropertyType::UInt32:
                *static_cast<uint32*>(ptr) = reader.ReadUInt32();
                break;
            case PropertyType::Int64:
                *static_cast<int64*>(ptr) = reader.ReadInt64();
                break;
            case PropertyType::UInt64:
                *static_cast<uint64*>(ptr) = reader.ReadUInt64();
                break;
            case PropertyType::Float32:
                *static_cast<float32*>(ptr) = reader.ReadFloat32();
                break;
            case PropertyType::Float64:
                *static_cast<float64*>(ptr) = reader.ReadFloat64();
                break;
            case PropertyType::String:
                *static_cast<std::string*>(ptr) = reader.ReadString();
                break;
            case PropertyType::Vec2:
                *static_cast<Vec2*>(ptr) = reader.ReadVec2();
                break;
            case PropertyType::Vec3:
                *static_cast<Vec3*>(ptr) = reader.ReadVec3();
                break;
            case PropertyType::Vec4:
                *static_cast<Vec4*>(ptr) = reader.ReadVec4();
                break;
            case PropertyType::Quat:
                *static_cast<Quat*>(ptr) = reader.ReadQuat();
                break;
            default:
                break;
        }
    }

} // namespace RVX::Net
