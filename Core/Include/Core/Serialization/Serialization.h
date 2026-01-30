/**
 * @file Serialization.h
 * @brief Type-safe serialization framework
 * 
 * Provides binary and text (JSON/YAML) serialization with:
 * - Automatic type registration
 * - Versioning support
 * - Polymorphic type handling
 */

#pragma once

#include "Core/Types.h"
#include "Core/MathTypes.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <functional>
#include <typeindex>

namespace RVX
{

class Archive;
class BinaryArchive;
class JsonArchive;

/**
 * @brief Serialization mode
 */
enum class ArchiveMode : uint8
{
    Read,
    Write
};

/**
 * @brief Serialization format
 */
enum class ArchiveFormat : uint8
{
    Binary,
    Json,
    Yaml
};

/**
 * @brief Type metadata for serialization
 */
struct TypeInfo
{
    std::string name;
    std::type_index typeIndex;
    uint32 version = 1;
    std::function<void*()> createFunc;
    std::function<void(Archive&, void*)> serializeFunc;
};

/**
 * @brief Type registry for serialization
 */
class TypeRegistry
{
public:
    static TypeRegistry& Get();

    /**
     * @brief Register a type for serialization
     */
    template<typename T>
    void Register(const std::string& name, uint32 version = 1);

    /**
     * @brief Get type info by name
     */
    const TypeInfo* GetTypeInfo(const std::string& name) const;

    /**
     * @brief Get type info by type index
     */
    const TypeInfo* GetTypeInfo(std::type_index type) const;

    /**
     * @brief Get type name from type
     */
    template<typename T>
    std::string GetTypeName() const;

private:
    std::unordered_map<std::string, TypeInfo> m_typesByName;
    std::unordered_map<std::type_index, std::string> m_namesByType;
};

/**
 * @brief Base archive class for serialization
 */
class Archive
{
public:
    Archive(ArchiveMode mode) : m_mode(mode) {}
    virtual ~Archive() = default;

    ArchiveMode GetMode() const { return m_mode; }
    bool IsReading() const { return m_mode == ArchiveMode::Read; }
    bool IsWriting() const { return m_mode == ArchiveMode::Write; }

    // =========================================================================
    // Primitive Types
    // =========================================================================

    virtual void Serialize(const char* name, bool& value) = 0;
    virtual void Serialize(const char* name, int8& value) = 0;
    virtual void Serialize(const char* name, int16& value) = 0;
    virtual void Serialize(const char* name, int32& value) = 0;
    virtual void Serialize(const char* name, int64& value) = 0;
    virtual void Serialize(const char* name, uint8& value) = 0;
    virtual void Serialize(const char* name, uint16& value) = 0;
    virtual void Serialize(const char* name, uint32& value) = 0;
    virtual void Serialize(const char* name, uint64& value) = 0;
    virtual void Serialize(const char* name, float& value) = 0;
    virtual void Serialize(const char* name, double& value) = 0;
    virtual void Serialize(const char* name, std::string& value) = 0;

    // =========================================================================
    // Math Types
    // =========================================================================

    virtual void Serialize(const char* name, Vec2& value);
    virtual void Serialize(const char* name, Vec3& value);
    virtual void Serialize(const char* name, Vec4& value);
    virtual void Serialize(const char* name, Quat& value);
    virtual void Serialize(const char* name, Mat4& value);

    // =========================================================================
    // Arrays/Containers
    // =========================================================================

    template<typename T>
    void SerializeArray(const char* name, std::vector<T>& values);

    template<typename K, typename V>
    void SerializeMap(const char* name, std::unordered_map<K, V>& values);

    // =========================================================================
    // Objects
    // =========================================================================

    virtual void BeginObject(const char* name) = 0;
    virtual void EndObject() = 0;

    virtual void BeginArray(const char* name, size_t& size) = 0;
    virtual void EndArray() = 0;

    // =========================================================================
    // Polymorphic Serialization
    // =========================================================================

    template<typename T>
    void SerializePolymorphic(const char* name, std::shared_ptr<T>& ptr);

    template<typename T>
    void SerializePolymorphic(const char* name, std::unique_ptr<T>& ptr);

    // =========================================================================
    // Versioning
    // =========================================================================

    uint32 GetVersion() const { return m_version; }
    void SetVersion(uint32 version) { m_version = version; }

protected:
    ArchiveMode m_mode;
    uint32 m_version = 1;
};

/**
 * @brief Binary archive for efficient serialization
 */
class BinaryArchive : public Archive
{
public:
    BinaryArchive(ArchiveMode mode);

    // For writing
    const std::vector<uint8>& GetData() const { return m_data; }

    // For reading
    void SetData(const uint8* data, size_t size);
    void SetData(std::vector<uint8> data);

    // =========================================================================
    // Implementation
    // =========================================================================

    void Serialize(const char* name, bool& value) override;
    void Serialize(const char* name, int8& value) override;
    void Serialize(const char* name, int16& value) override;
    void Serialize(const char* name, int32& value) override;
    void Serialize(const char* name, int64& value) override;
    void Serialize(const char* name, uint8& value) override;
    void Serialize(const char* name, uint16& value) override;
    void Serialize(const char* name, uint32& value) override;
    void Serialize(const char* name, uint64& value) override;
    void Serialize(const char* name, float& value) override;
    void Serialize(const char* name, double& value) override;
    void Serialize(const char* name, std::string& value) override;

    void BeginObject(const char* name) override;
    void EndObject() override;

    void BeginArray(const char* name, size_t& size) override;
    void EndArray() override;

private:
    template<typename T>
    void WriteRaw(const T& value);

    template<typename T>
    void ReadRaw(T& value);

    std::vector<uint8> m_data;
    size_t m_readPos = 0;
};

/**
 * @brief JSON archive for human-readable serialization
 */
class JsonArchive : public Archive
{
public:
    JsonArchive(ArchiveMode mode);

    // For writing
    std::string ToString() const;

    // For reading
    bool Parse(const std::string& json);

    // =========================================================================
    // Implementation
    // =========================================================================

    void Serialize(const char* name, bool& value) override;
    void Serialize(const char* name, int8& value) override;
    void Serialize(const char* name, int16& value) override;
    void Serialize(const char* name, int32& value) override;
    void Serialize(const char* name, int64& value) override;
    void Serialize(const char* name, uint8& value) override;
    void Serialize(const char* name, uint16& value) override;
    void Serialize(const char* name, uint32& value) override;
    void Serialize(const char* name, uint64& value) override;
    void Serialize(const char* name, float& value) override;
    void Serialize(const char* name, double& value) override;
    void Serialize(const char* name, std::string& value) override;

    void BeginObject(const char* name) override;
    void EndObject() override;

    void BeginArray(const char* name, size_t& size) override;
    void EndArray() override;

private:
    void* m_jsonRoot = nullptr;  // JSON library root node
    std::vector<void*> m_nodeStack;
    int m_indent = 0;
    std::string m_output;
};

/**
 * @brief Serializable interface for custom types
 */
class ISerializable
{
public:
    virtual ~ISerializable() = default;
    virtual void Serialize(Archive& archive) = 0;
    virtual const char* GetSerializableTypeName() const = 0;
};

/**
 * @brief Helper macro for implementing serialization
 */
#define RVX_SERIALIZE_TYPE(Type) \
    static const char* StaticTypeName() { return #Type; } \
    const char* GetSerializableTypeName() const override { return #Type; } \
    void Serialize(Archive& archive) override

/**
 * @brief Auto-registration helper
 */
template<typename T>
struct TypeRegistrar
{
    TypeRegistrar(const std::string& name, uint32 version = 1)
    {
        TypeRegistry::Get().Register<T>(name, version);
    }
};

#define RVX_REGISTER_TYPE(Type) \
    static TypeRegistrar<Type> s_##Type##Registrar(#Type)

#define RVX_REGISTER_TYPE_VERSIONED(Type, Version) \
    static TypeRegistrar<Type> s_##Type##Registrar(#Type, Version)

} // namespace RVX
