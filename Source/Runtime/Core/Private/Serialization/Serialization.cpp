/**
 * @file Serialization.cpp
 * @brief Serialization framework implementation
 */

#include "Core/Serialization/Serialization.h"
#include <algorithm>
#include <cctype>
#include <cstring>

namespace RVX
{
namespace
{
    class JsonSyntaxValidator
    {
    public:
        explicit JsonSyntaxValidator(const std::string& json)
            : m_json(json)
        {
        }

        bool Validate()
        {
            SkipWhitespace();
            if (!ParseValue())
            {
                return false;
            }
            SkipWhitespace();
            return m_pos == m_json.size();
        }

    private:
        bool ParseValue()
        {
            SkipWhitespace();
            if (m_pos >= m_json.size())
            {
                return false;
            }

            const char ch = m_json[m_pos];
            if (ch == '{')
            {
                return ParseObject();
            }
            if (ch == '[')
            {
                return ParseArray();
            }
            if (ch == '"')
            {
                return ParseString();
            }
            if (ch == '-' || IsDigit(ch))
            {
                return ParseNumber();
            }
            return MatchLiteral("true") || MatchLiteral("false") || MatchLiteral("null");
        }

        bool ParseObject()
        {
            if (!Consume('{'))
            {
                return false;
            }

            SkipWhitespace();
            if (Consume('}'))
            {
                return true;
            }

            while (true)
            {
                SkipWhitespace();
                if (!ParseString())
                {
                    return false;
                }

                SkipWhitespace();
                if (!Consume(':'))
                {
                    return false;
                }

                if (!ParseValue())
                {
                    return false;
                }

                SkipWhitespace();
                if (Consume('}'))
                {
                    return true;
                }
                if (!Consume(','))
                {
                    return false;
                }

                SkipWhitespace();
                if (m_pos >= m_json.size() || m_json[m_pos] == '}')
                {
                    return false;
                }
            }
        }

        bool ParseArray()
        {
            if (!Consume('['))
            {
                return false;
            }

            SkipWhitespace();
            if (Consume(']'))
            {
                return true;
            }

            while (true)
            {
                if (!ParseValue())
                {
                    return false;
                }

                SkipWhitespace();
                if (Consume(']'))
                {
                    return true;
                }
                if (!Consume(','))
                {
                    return false;
                }

                SkipWhitespace();
                if (m_pos >= m_json.size() || m_json[m_pos] == ']')
                {
                    return false;
                }
            }
        }

        bool ParseString()
        {
            if (!Consume('"'))
            {
                return false;
            }

            while (m_pos < m_json.size())
            {
                const unsigned char ch = static_cast<unsigned char>(m_json[m_pos++]);
                if (ch == '"')
                {
                    return true;
                }
                if (ch < 0x20)
                {
                    return false;
                }
                if (ch != '\\')
                {
                    continue;
                }

                if (m_pos >= m_json.size())
                {
                    return false;
                }

                const char escaped = m_json[m_pos++];
                switch (escaped)
                {
                    case '"':
                    case '\\':
                    case '/':
                    case 'b':
                    case 'f':
                    case 'n':
                    case 'r':
                    case 't':
                        break;
                    case 'u':
                        for (int i = 0; i < 4; ++i)
                        {
                            if (m_pos >= m_json.size() || !IsHexDigit(m_json[m_pos]))
                            {
                                return false;
                            }
                            ++m_pos;
                        }
                        break;
                    default:
                        return false;
                }
            }

            return false;
        }

        bool ParseNumber()
        {
            if (Consume('-') && m_pos >= m_json.size())
            {
                return false;
            }

            if (Consume('0'))
            {
                if (m_pos < m_json.size() && IsDigit(m_json[m_pos]))
                {
                    return false;
                }
            }
            else
            {
                if (m_pos >= m_json.size() || !IsDigitOneToNine(m_json[m_pos]))
                {
                    return false;
                }
                while (m_pos < m_json.size() && IsDigit(m_json[m_pos]))
                {
                    ++m_pos;
                }
            }

            if (Consume('.'))
            {
                if (m_pos >= m_json.size() || !IsDigit(m_json[m_pos]))
                {
                    return false;
                }
                while (m_pos < m_json.size() && IsDigit(m_json[m_pos]))
                {
                    ++m_pos;
                }
            }

            if (m_pos < m_json.size() && (m_json[m_pos] == 'e' || m_json[m_pos] == 'E'))
            {
                ++m_pos;
                if (m_pos < m_json.size() && (m_json[m_pos] == '+' || m_json[m_pos] == '-'))
                {
                    ++m_pos;
                }
                if (m_pos >= m_json.size() || !IsDigit(m_json[m_pos]))
                {
                    return false;
                }
                while (m_pos < m_json.size() && IsDigit(m_json[m_pos]))
                {
                    ++m_pos;
                }
            }

            return true;
        }

        bool MatchLiteral(const char* literal)
        {
            const size_t start = m_pos;
            for (const char* cursor = literal; *cursor != '\0'; ++cursor)
            {
                if (m_pos >= m_json.size() || m_json[m_pos] != *cursor)
                {
                    m_pos = start;
                    return false;
                }
                ++m_pos;
            }
            return true;
        }

        bool Consume(char expected)
        {
            if (m_pos < m_json.size() && m_json[m_pos] == expected)
            {
                ++m_pos;
                return true;
            }
            return false;
        }

        void SkipWhitespace()
        {
            while (m_pos < m_json.size() &&
                   std::isspace(static_cast<unsigned char>(m_json[m_pos])) != 0)
            {
                ++m_pos;
            }
        }

        static bool IsDigit(char ch)
        {
            return ch >= '0' && ch <= '9';
        }

        static bool IsDigitOneToNine(char ch)
        {
            return ch >= '1' && ch <= '9';
        }

        static bool IsHexDigit(char ch)
        {
            return (ch >= '0' && ch <= '9') ||
                   (ch >= 'a' && ch <= 'f') ||
                   (ch >= 'A' && ch <= 'F');
        }

        const std::string& m_json;
        size_t m_pos = 0;
    };

    bool IsValidJsonSyntax(const std::string& json)
    {
        JsonSyntaxValidator validator(json);
        return validator.Validate();
    }
}

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
// JsonArchive
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
    m_parseSucceeded = IsValidJsonSyntax(json);
    m_unsupportedReadAttempted = false;
    m_unsupportedReason = m_parseSucceeded
        ? "JsonArchive read deserialization is unsupported; Parse only validates JSON syntax."
        : "JsonArchive parse failed: input is not valid JSON.";
    return m_parseSucceeded;
}

void JsonArchive::MarkReadUnsupported(const char* operation)
{
    if (!IsReading())
    {
        return;
    }

    m_unsupportedReadAttempted = true;
    m_unsupportedReason = "JsonArchive read deserialization is unsupported; ";
    m_unsupportedReason += operation ? operation : "read operation";
    m_unsupportedReason += " was not applied.";
}

void JsonArchive::Serialize(const char* name, bool& value)
{
    if (IsWriting())
    {
        m_output += std::string(m_indent * 2, ' ') + "\"" + name + "\": " + (value ? "true" : "false") + ",\n";
        return;
    }

    MarkReadUnsupported(name);
}

void JsonArchive::Serialize(const char* name, int8& value)
{
    if (IsWriting())
    {
        m_output += std::string(m_indent * 2, ' ') + "\"" + name + "\": " + std::to_string(value) + ",\n";
        return;
    }

    MarkReadUnsupported(name);
}

void JsonArchive::Serialize(const char* name, int16& value)
{
    if (IsWriting())
    {
        m_output += std::string(m_indent * 2, ' ') + "\"" + name + "\": " + std::to_string(value) + ",\n";
        return;
    }

    MarkReadUnsupported(name);
}

void JsonArchive::Serialize(const char* name, int32& value)
{
    if (IsWriting())
    {
        m_output += std::string(m_indent * 2, ' ') + "\"" + name + "\": " + std::to_string(value) + ",\n";
        return;
    }

    MarkReadUnsupported(name);
}

void JsonArchive::Serialize(const char* name, int64& value)
{
    if (IsWriting())
    {
        m_output += std::string(m_indent * 2, ' ') + "\"" + name + "\": " + std::to_string(value) + ",\n";
        return;
    }

    MarkReadUnsupported(name);
}

void JsonArchive::Serialize(const char* name, uint8& value)
{
    if (IsWriting())
    {
        m_output += std::string(m_indent * 2, ' ') + "\"" + name + "\": " + std::to_string(value) + ",\n";
        return;
    }

    MarkReadUnsupported(name);
}

void JsonArchive::Serialize(const char* name, uint16& value)
{
    if (IsWriting())
    {
        m_output += std::string(m_indent * 2, ' ') + "\"" + name + "\": " + std::to_string(value) + ",\n";
        return;
    }

    MarkReadUnsupported(name);
}

void JsonArchive::Serialize(const char* name, uint32& value)
{
    if (IsWriting())
    {
        m_output += std::string(m_indent * 2, ' ') + "\"" + name + "\": " + std::to_string(value) + ",\n";
        return;
    }

    MarkReadUnsupported(name);
}

void JsonArchive::Serialize(const char* name, uint64& value)
{
    if (IsWriting())
    {
        m_output += std::string(m_indent * 2, ' ') + "\"" + name + "\": " + std::to_string(value) + ",\n";
        return;
    }

    MarkReadUnsupported(name);
}

void JsonArchive::Serialize(const char* name, float& value)
{
    if (IsWriting())
    {
        m_output += std::string(m_indent * 2, ' ') + "\"" + name + "\": " + std::to_string(value) + ",\n";
        return;
    }

    MarkReadUnsupported(name);
}

void JsonArchive::Serialize(const char* name, double& value)
{
    if (IsWriting())
    {
        m_output += std::string(m_indent * 2, ' ') + "\"" + name + "\": " + std::to_string(value) + ",\n";
        return;
    }

    MarkReadUnsupported(name);
}

void JsonArchive::Serialize(const char* name, std::string& value)
{
    if (IsWriting())
    {
        m_output += std::string(m_indent * 2, ' ') + "\"" + name + "\": \"" + value + "\",\n";
        return;
    }

    MarkReadUnsupported(name);
}

void JsonArchive::BeginObject(const char* name)
{
    if (IsWriting())
    {
        m_output += std::string(m_indent * 2, ' ') + "\"" + name + "\": {\n";
        m_indent++;
        return;
    }

    MarkReadUnsupported(name);
}

void JsonArchive::EndObject()
{
    if (IsWriting())
    {
        m_indent--;
        m_output += std::string(m_indent * 2, ' ') + "},\n";
        return;
    }

    MarkReadUnsupported("EndObject");
}

void JsonArchive::BeginArray(const char* name, size_t& size)
{
    if (IsWriting())
    {
        m_output += std::string(m_indent * 2, ' ') + "\"" + name + "\": [\n";
        m_indent++;
        return;
    }

    (void)size;
    MarkReadUnsupported(name);
}

void JsonArchive::EndArray()
{
    if (IsWriting())
    {
        m_indent--;
        m_output += std::string(m_indent * 2, ' ') + "],\n";
        return;
    }

    MarkReadUnsupported("EndArray");
}

} // namespace RVX
