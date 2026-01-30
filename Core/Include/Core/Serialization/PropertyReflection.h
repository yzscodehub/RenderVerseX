/**
 * @file PropertyReflection.h
 * @brief Property reflection system for editor and serialization
 */

#pragma once

#include "Core/Types.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <functional>
#include <typeindex>
#include <any>

namespace RVX
{

/**
 * @brief Property type enumeration
 */
enum class PropertyType : uint8
{
    Unknown,
    Bool,
    Int32,
    Int64,
    Float,
    Double,
    String,
    Vec2,
    Vec3,
    Vec4,
    Quat,
    Mat4,
    Color,
    Enum,
    Object,
    Array,
    AssetRef
};

/**
 * @brief Property metadata for UI and validation
 */
struct PropertyMeta
{
    std::string displayName;
    std::string tooltip;
    std::string category;
    
    float minValue = 0.0f;
    float maxValue = 1.0f;
    float step = 0.1f;
    
    bool readOnly = false;
    bool hidden = false;
    bool transient = false;  // Don't serialize
    
    std::vector<std::string> enumValues;  // For enum properties
};

/**
 * @brief Property descriptor
 */
class Property
{
public:
    using Getter = std::function<std::any(void*)>;
    using Setter = std::function<void(void*, const std::any&)>;

    Property() = default;
    Property(const std::string& name, PropertyType type, Getter getter, Setter setter)
        : m_name(name), m_type(type), m_getter(std::move(getter)), m_setter(std::move(setter))
    {}

    const std::string& GetName() const { return m_name; }
    PropertyType GetType() const { return m_type; }
    const PropertyMeta& GetMeta() const { return m_meta; }
    PropertyMeta& GetMeta() { return m_meta; }

    template<typename T>
    T GetValue(void* instance) const
    {
        return std::any_cast<T>(m_getter(instance));
    }

    template<typename T>
    void SetValue(void* instance, const T& value) const
    {
        if (!m_meta.readOnly)
        {
            m_setter(instance, std::any(value));
        }
    }

    bool IsReadOnly() const { return m_meta.readOnly; }
    bool IsHidden() const { return m_meta.hidden; }

private:
    std::string m_name;
    PropertyType m_type = PropertyType::Unknown;
    PropertyMeta m_meta;
    Getter m_getter;
    Setter m_setter;
};

/**
 * @brief Class descriptor with properties
 */
class ClassDescriptor
{
public:
    ClassDescriptor(const std::string& name, std::type_index type)
        : m_name(name), m_typeIndex(type)
    {}

    const std::string& GetName() const { return m_name; }
    std::type_index GetTypeIndex() const { return m_typeIndex; }

    void AddProperty(Property property)
    {
        m_properties.push_back(std::move(property));
    }

    const std::vector<Property>& GetProperties() const { return m_properties; }

    Property* FindProperty(const std::string& name)
    {
        for (auto& prop : m_properties)
        {
            if (prop.GetName() == name) return &prop;
        }
        return nullptr;
    }

    void SetBaseClass(const std::string& baseName) { m_baseClassName = baseName; }
    const std::string& GetBaseClass() const { return m_baseClassName; }

private:
    std::string m_name;
    std::type_index m_typeIndex;
    std::string m_baseClassName;
    std::vector<Property> m_properties;
};

/**
 * @brief Reflection registry
 */
class ReflectionRegistry
{
public:
    static ReflectionRegistry& Get();

    void RegisterClass(std::unique_ptr<ClassDescriptor> descriptor);

    ClassDescriptor* GetClass(const std::string& name);
    ClassDescriptor* GetClass(std::type_index type);

    const std::unordered_map<std::string, std::unique_ptr<ClassDescriptor>>& GetAllClasses() const
    {
        return m_classesByName;
    }

private:
    std::unordered_map<std::string, std::unique_ptr<ClassDescriptor>> m_classesByName;
    std::unordered_map<std::type_index, ClassDescriptor*> m_classesByType;
};

/**
 * @brief Property builder for fluent API
 */
template<typename T>
class PropertyBuilder
{
public:
    PropertyBuilder(ClassDescriptor& desc, const std::string& name, PropertyType type)
        : m_descriptor(desc), m_property(name, type, nullptr, nullptr)
    {}

    PropertyBuilder& DisplayName(const std::string& name)
    {
        m_property.GetMeta().displayName = name;
        return *this;
    }

    PropertyBuilder& Tooltip(const std::string& tooltip)
    {
        m_property.GetMeta().tooltip = tooltip;
        return *this;
    }

    PropertyBuilder& Category(const std::string& category)
    {
        m_property.GetMeta().category = category;
        return *this;
    }

    PropertyBuilder& Range(float min, float max)
    {
        m_property.GetMeta().minValue = min;
        m_property.GetMeta().maxValue = max;
        return *this;
    }

    PropertyBuilder& ReadOnly()
    {
        m_property.GetMeta().readOnly = true;
        return *this;
    }

    PropertyBuilder& Hidden()
    {
        m_property.GetMeta().hidden = true;
        return *this;
    }

    ~PropertyBuilder()
    {
        m_descriptor.AddProperty(std::move(m_property));
    }

private:
    ClassDescriptor& m_descriptor;
    Property m_property;
};

/**
 * @brief Class registration builder
 */
template<typename T>
class ClassBuilder
{
public:
    ClassBuilder(const std::string& name)
        : m_descriptor(std::make_unique<ClassDescriptor>(name, typeid(T)))
    {}

    template<typename MemberType>
    PropertyBuilder<T>& Property(const std::string& name, MemberType T::* member);

    ClassBuilder& Extends(const std::string& baseName)
    {
        m_descriptor->SetBaseClass(baseName);
        return *this;
    }

    ~ClassBuilder()
    {
        ReflectionRegistry::Get().RegisterClass(std::move(m_descriptor));
    }

private:
    std::unique_ptr<ClassDescriptor> m_descriptor;
};

// Helper macros
#define RVX_REFLECT_CLASS(Type) \
    static void RegisterReflection_##Type(); \
    namespace { struct Type##ReflectionRegistrar { \
        Type##ReflectionRegistrar() { RegisterReflection_##Type(); } \
    } s_##Type##Registrar; } \
    void RegisterReflection_##Type()

#define RVX_REFLECT_BEGIN(Type) \
    ClassBuilder<Type> builder(#Type);

#define RVX_REFLECT_PROPERTY(name) \
    builder.Property(#name, &Type::name)

#define RVX_REFLECT_END() \
    // Builder destructor handles registration

} // namespace RVX
