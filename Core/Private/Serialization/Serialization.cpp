/**
 * @file Serialization.cpp
 * @brief Serialization framework implementation
 */

#include "Core/Serialization/Serialization.h"
#include <cstring>

namespace RVX
{

// ============================================================================
// TypeRegistry
// ============================================================================

TypeRegistry& TypeRegistry::Get()
{
    static TypeRegistry instance;
    return instance;
}

const TypeInfo* TypeRegistry::GetTypeInfo(const std::string& name) const
{
    auto it = m_typesByName.find(name);
    return (it != m_typesByName.end()) ? &it->second : nullptr;
}

const TypeInfo* TypeRegistry::GetTypeInfo(std::type_index type) const
{
    auto it = m_namesByType.find(type);
    if (it != m_namesByType.end())
    {
        return GetTypeInfo(it->second);
    }
    return nullptr;
}

// ============================================================================
// Archive
// ============================================================================

void Archive::Serialize(const char* name, Vec2& value)
{
    BeginObject(name);
    Serialize("x", value.x);
    Serialize("y", value.y);
    EndObject();
}

void Archive::Serialize(const char* name, Vec3& value)
{
    BeginObject(name);
    Serialize("x", value.x);
    Serialize("y", value.y);
    Serialize("z", value.z);
    EndObject();
}

void Archive::Serialize(const char* name, Vec4& value)
{
    BeginObject(name);
    Serialize("x", value.x);
    Serialize("y", value.y);
    Serialize("z", value.z);
    Serialize("w", value.w);
    EndObject();
}

void Archive::Serialize(const char* name, Quat& value)
{
    BeginObject(name);
    Serialize("x", value.x);
    Serialize("y", value.y);
    Serialize("z", value.z);
    Serialize("w", value.w);
    EndObject();
}

void Archive::Serialize(const char* name, Mat4& value)
{
    BeginObject(name);
    for (int i = 0; i < 4; ++i)
    {
        std::string rowName = "row" + std::to_string(i);
        BeginObject(rowName.c_str());
        for (int j = 0; j < 4; ++j)
        {
            std::string colName = "c" + std::to_string(j);
            Serialize(colName.c_str(), value[i][j]);
        }
        EndObject();
    }
    EndObject();
}

// ============================================================================
// BinaryArchive
// ============================================================================

BinaryArchive::BinaryArchive(ArchiveMode mode)
    : Archive(mode)
{
    if (mode == ArchiveMode::Write)
    {
        m_data.reserve(1024);
    }
}

void BinaryArchive::SetData(const uint8* data, size_t size)
{
    m_data.assign(data, data + size);
    m_readPos = 0;
}

void BinaryArchive::SetData(std::vector<uint8> data)
{
    m_data = std::move(data);
    m_readPos = 0;
}

template<typename T>
void BinaryArchive::WriteRaw(const T& value)
{
    const uint8* bytes = reinterpret_cast<const uint8*>(&value);
    m_data.insert(m_data.end(), bytes, bytes + sizeof(T));
}

template<typename T>
void BinaryArchive::ReadRaw(T& value)
{
    if (m_readPos + sizeof(T) <= m_data.size())
    {
        std::memcpy(&value, m_data.data() + m_readPos, sizeof(T));
        m_readPos += sizeof(T);
    }
}

void BinaryArchive::Serialize(const char* name, bool& value)
{
    (void)name;
    if (IsWriting()) WriteRaw(value);
    else ReadRaw(value);
}

void BinaryArchive::Serialize(const char* name, int8& value)
{
    (void)name;
    if (IsWriting()) WriteRaw(value);
    else ReadRaw(value);
}

void BinaryArchive::Serialize(const char* name, int16& value)
{
    (void)name;
    if (IsWriting()) WriteRaw(value);
    else ReadRaw(value);
}

void BinaryArchive::Serialize(const char* name, int32& value)
{
    (void)name;
    if (IsWriting()) WriteRaw(value);
    else ReadRaw(value);
}

void BinaryArchive::Serialize(const char* name, int64& value)
{
    (void)name;
    if (IsWriting()) WriteRaw(value);
    else ReadRaw(value);
}

void BinaryArchive::Serialize(const char* name, uint8& value)
{
    (void)name;
    if (IsWriting()) WriteRaw(value);
    else ReadRaw(value);
}

void BinaryArchive::Serialize(const char* name, uint16& value)
{
    (void)name;
    if (IsWriting()) WriteRaw(value);
    else ReadRaw(value);
}

void BinaryArchive::Serialize(const char* name, uint32& value)
{
    (void)name;
    if (IsWriting()) WriteRaw(value);
    else ReadRaw(value);
}

void BinaryArchive::Serialize(const char* name, uint64& value)
{
    (void)name;
    if (IsWriting()) WriteRaw(value);
    else ReadRaw(value);
}

void BinaryArchive::Serialize(const char* name, float& value)
{
    (void)name;
    if (IsWriting()) WriteRaw(value);
    else ReadRaw(value);
}

void BinaryArchive::Serialize(const char* name, double& value)
{
    (void)name;
    if (IsWriting()) WriteRaw(value);
    else ReadRaw(value);
}

void BinaryArchive::Serialize(const char* name, std::string& value)
{
    (void)name;
    if (IsWriting())
    {
        uint32 len = static_cast<uint32>(value.size());
        WriteRaw(len);
        m_data.insert(m_data.end(), value.begin(), value.end());
    }
    else
    {
        uint32 len = 0;
        ReadRaw(len);
        if (m_readPos + len <= m_data.size())
        {
            value.assign(reinterpret_cast<const char*>(m_data.data() + m_readPos), len);
            m_readPos += len;
        }
    }
}

void BinaryArchive::BeginObject(const char* name)
{
    (void)name;
    // Binary format doesn't need object markers
}

void BinaryArchive::EndObject()
{
    // Binary format doesn't need object markers
}

void BinaryArchive::BeginArray(const char* name, size_t& size)
{
    (void)name;
    if (IsWriting())
    {
        uint32 len = static_cast<uint32>(size);
        WriteRaw(len);
    }
    else
    {
        uint32 len = 0;
        ReadRaw(len);
        size = len;
    }
}

void BinaryArchive::EndArray()
{
    // Binary format doesn't need array end markers
}

// ============================================================================
// JsonArchive (stub - would use rapidjson/nlohmann_json)
// ============================================================================

JsonArchive::JsonArchive(ArchiveMode mode)
    : Archive(mode)
{
}

std::string JsonArchive::ToString() const
{
    return m_output;
}

bool JsonArchive::Parse(const std::string& json)
{
    // TODO: Parse JSON using library
    (void)json;
    return true;
}

void JsonArchive::Serialize(const char* name, bool& value)
{
    if (IsWriting())
    {
        m_output += std::string(m_indent * 2, ' ') + "\"" + name + "\": " + (value ? "true" : "false") + ",\n";
    }
    // TODO: Read from parsed JSON
}

void JsonArchive::Serialize(const char* name, int8& value)
{
    if (IsWriting())
    {
        m_output += std::string(m_indent * 2, ' ') + "\"" + name + "\": " + std::to_string(value) + ",\n";
    }
}

void JsonArchive::Serialize(const char* name, int16& value)
{
    if (IsWriting())
    {
        m_output += std::string(m_indent * 2, ' ') + "\"" + name + "\": " + std::to_string(value) + ",\n";
    }
}

void JsonArchive::Serialize(const char* name, int32& value)
{
    if (IsWriting())
    {
        m_output += std::string(m_indent * 2, ' ') + "\"" + name + "\": " + std::to_string(value) + ",\n";
    }
}

void JsonArchive::Serialize(const char* name, int64& value)
{
    if (IsWriting())
    {
        m_output += std::string(m_indent * 2, ' ') + "\"" + name + "\": " + std::to_string(value) + ",\n";
    }
}

void JsonArchive::Serialize(const char* name, uint8& value)
{
    if (IsWriting())
    {
        m_output += std::string(m_indent * 2, ' ') + "\"" + name + "\": " + std::to_string(value) + ",\n";
    }
}

void JsonArchive::Serialize(const char* name, uint16& value)
{
    if (IsWriting())
    {
        m_output += std::string(m_indent * 2, ' ') + "\"" + name + "\": " + std::to_string(value) + ",\n";
    }
}

void JsonArchive::Serialize(const char* name, uint32& value)
{
    if (IsWriting())
    {
        m_output += std::string(m_indent * 2, ' ') + "\"" + name + "\": " + std::to_string(value) + ",\n";
    }
}

void JsonArchive::Serialize(const char* name, uint64& value)
{
    if (IsWriting())
    {
        m_output += std::string(m_indent * 2, ' ') + "\"" + name + "\": " + std::to_string(value) + ",\n";
    }
}

void JsonArchive::Serialize(const char* name, float& value)
{
    if (IsWriting())
    {
        m_output += std::string(m_indent * 2, ' ') + "\"" + name + "\": " + std::to_string(value) + ",\n";
    }
}

void JsonArchive::Serialize(const char* name, double& value)
{
    if (IsWriting())
    {
        m_output += std::string(m_indent * 2, ' ') + "\"" + name + "\": " + std::to_string(value) + ",\n";
    }
}

void JsonArchive::Serialize(const char* name, std::string& value)
{
    if (IsWriting())
    {
        m_output += std::string(m_indent * 2, ' ') + "\"" + name + "\": \"" + value + "\",\n";
    }
}

void JsonArchive::BeginObject(const char* name)
{
    if (IsWriting())
    {
        m_output += std::string(m_indent * 2, ' ') + "\"" + name + "\": {\n";
        m_indent++;
    }
}

void JsonArchive::EndObject()
{
    if (IsWriting())
    {
        m_indent--;
        m_output += std::string(m_indent * 2, ' ') + "},\n";
    }
}

void JsonArchive::BeginArray(const char* name, size_t& size)
{
    if (IsWriting())
    {
        m_output += std::string(m_indent * 2, ' ') + "\"" + name + "\": [\n";
        m_indent++;
    }
}

void JsonArchive::EndArray()
{
    if (IsWriting())
    {
        m_indent--;
        m_output += std::string(m_indent * 2, ' ') + "],\n";
    }
}

} // namespace RVX
