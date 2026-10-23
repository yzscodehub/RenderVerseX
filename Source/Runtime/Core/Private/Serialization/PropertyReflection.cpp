/**
 * @file PropertyReflection.cpp
 * @brief Property reflection system implementation
 */

#include "Core/Serialization/PropertyReflection.h"

namespace RVX
{

// =============================================================================
// ReflectionRegistry Implementation
// =============================================================================

ReflectionRegistry& ReflectionRegistry::Get()
{
    static ReflectionRegistry s_instance;
    return s_instance;
}

void ReflectionRegistry::RegisterClass(std::unique_ptr<ClassDescriptor> descriptor)
{
    if (!descriptor)
    {
        return;
    }
    
    std::type_index typeIndex = descriptor->GetTypeIndex();
    const std::string& name = descriptor->GetName();
    
    // Store raw pointer before moving
    ClassDescriptor* rawPtr = descriptor.get();
    
    // Insert by name
    m_classesByName[name] = std::move(descriptor);
    
    // Insert by type index
    m_classesByType[typeIndex] = rawPtr;
}

ClassDescriptor* ReflectionRegistry::GetClass(const std::string& name)
{
    auto it = m_classesByName.find(name);
    if (it != m_classesByName.end())
    {
        return it->second.get();
    }
    return nullptr;
}

ClassDescriptor* ReflectionRegistry::GetClass(std::type_index type)
{
    auto it = m_classesByType.find(type);
    if (it != m_classesByType.end())
    {
        return it->second;
    }
    return nullptr;
}

} // namespace RVX
