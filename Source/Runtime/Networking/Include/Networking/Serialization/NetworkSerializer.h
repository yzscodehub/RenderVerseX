#pragma once

/**
 * @file NetworkSerializer.h
 * @brief Type-safe serialization for network objects
 * 
 * Provides a higher-level serialization API built on BitStream,
 * with support for custom types, versioning, and delta compression.
 */

#include "Networking/Serialization/BitStream.h"
#include "Core/MathTypes.h"

#include <type_traits>
#include <unordered_map>
#include <optional>
#include <variant>

namespace RVX::Net
{
    // Forward declarations
    class INetworkSerializable;

    // =========================================================================
    // Serialization Traits
    // =========================================================================

    /**
     * @brief Type trait for checking if a type is network serializable
     */
    template<typename T>
    struct IsNetworkSerializable : std::false_type {};

    // Built-in serializable types
    template<> struct IsNetworkSerializable<bool> : std::true_type {};
    template<> struct IsNetworkSerializable<int8> : std::true_type {};
    template<> struct IsNetworkSerializable<uint8> : std::true_type {};
    template<> struct IsNetworkSerializable<int16> : std::true_type {};
    template<> struct IsNetworkSerializable<uint16> : std::true_type {};
    template<> struct IsNetworkSerializable<int32> : std::true_type {};
    template<> struct IsNetworkSerializable<uint32> : std::true_type {};
    template<> struct IsNetworkSerializable<int64> : std::true_type {};
    template<> struct IsNetworkSerializable<uint64> : std::true_type {};
    template<> struct IsNetworkSerializable<float32> : std::true_type {};
    template<> struct IsNetworkSerializable<float64> : std::true_type {};
    template<> struct IsNetworkSerializable<std::string> : std::true_type {};

    template<typename T>
    inline constexpr bool IsNetworkSerializableV = IsNetworkSerializable<T>::value;

    // =========================================================================
    // Network Serializer
    // =========================================================================

    /**
     * @brief High-level serializer for reading network data
     */
    class NetworkReader
    {
    public:
        explicit NetworkReader(BitReader& reader)
            : m_reader(reader)
        {
        }

        // =====================================================================
        // Basic Types
        // =====================================================================

        bool ReadBool() { return m_reader.ReadBool(); }
        int8 ReadInt8() { return m_reader.ReadInt8(); }
        uint8 ReadUInt8() { return m_reader.ReadUInt8(); }
        int16 ReadInt16() { return m_reader.ReadInt16(); }
        uint16 ReadUInt16() { return m_reader.ReadUInt16(); }
        int32 ReadInt32() { return m_reader.ReadInt32(); }
        uint32 ReadUInt32() { return m_reader.ReadUInt32(); }
        int64 ReadInt64() { return m_reader.ReadInt64(); }
        uint64 ReadUInt64() { return m_reader.ReadUInt64(); }
        float32 ReadFloat32() { return m_reader.ReadFloat32(); }
        float64 ReadFloat64() { return m_reader.ReadFloat64(); }
        std::string ReadString() { return m_reader.ReadString(); }

        // =====================================================================
        // Math Types
        // =====================================================================

        Vec2 ReadVec2()
        {
            Vec2 v;
            v.x = m_reader.ReadFloat32();
            v.y = m_reader.ReadFloat32();
            return v;
        }

        Vec3 ReadVec3()
        {
            Vec3 v;
            v.x = m_reader.ReadFloat32();
            v.y = m_reader.ReadFloat32();
            v.z = m_reader.ReadFloat32();
            return v;
        }

        Vec4 ReadVec4()
        {
            Vec4 v;
            v.x = m_reader.ReadFloat32();
            v.y = m_reader.ReadFloat32();
            v.z = m_reader.ReadFloat32();
            v.w = m_reader.ReadFloat32();
            return v;
        }

        Quat ReadQuat()
        {
            Quat q;
            q.x = m_reader.ReadFloat32();
            q.y = m_reader.ReadFloat32();
            q.z = m_reader.ReadFloat32();
            q.w = m_reader.ReadFloat32();
            return q;
        }

        /**
         * @brief Read a compressed quaternion (smallest-three encoding)
         */
        Quat ReadCompressedQuat()
        {
            // Read which component was largest (2 bits)
            uint32 largestIndex = m_reader.ReadBits(2);
            
            // Read three 10-bit components
            float32 a = m_reader.ReadNormalizedFloat(10) * 2.0f - 1.0f;
            float32 b = m_reader.ReadNormalizedFloat(10) * 2.0f - 1.0f;
            float32 c = m_reader.ReadNormalizedFloat(10) * 2.0f - 1.0f;
            
            // Scale to proper range [-0.707, 0.707]
            constexpr float32 kScale = 0.70710678118f; // 1/sqrt(2)
            a *= kScale;
            b *= kScale;
            c *= kScale;
            
            // Reconstruct the largest component
            float32 d = std::sqrt(std::max(0.0f, 1.0f - a*a - b*b - c*c));
            
            Quat q;
            switch (largestIndex)
            {
                case 0: q = Quat(d, a, b, c); break; // x was largest
                case 1: q = Quat(a, d, b, c); break; // y was largest
                case 2: q = Quat(a, b, d, c); break; // z was largest
                case 3: q = Quat(a, b, c, d); break; // w was largest
                default: q = Quat(0, 0, 0, 1); break;
            }
            
            return glm::normalize(q);
        }

        /**
         * @brief Read a compressed Vec3 position
         */
        Vec3 ReadCompressedPosition(Vec3 min, Vec3 max, uint32 bitsPerComponent)
        {
            Vec3 v;
            v.x = m_reader.ReadRangedFloat(min.x, max.x, bitsPerComponent);
            v.y = m_reader.ReadRangedFloat(min.y, max.y, bitsPerComponent);
            v.z = m_reader.ReadRangedFloat(min.z, max.z, bitsPerComponent);
            return v;
        }

        Mat4 ReadMat4()
        {
            Mat4 m;
            for (int col = 0; col < 4; ++col)
            {
                for (int row = 0; row < 4; ++row)
                {
                    m[col][row] = m_reader.ReadFloat32();
                }
            }
            return m;
        }

        // =====================================================================
        // Containers
        // =====================================================================

        template<typename T>
        std::vector<T> ReadVector()
        {
            uint32 count = m_reader.ReadVarInt();
            std::vector<T> result;
            result.reserve(count);
            
            for (uint32 i = 0; i < count; ++i)
            {
                result.push_back(Read<T>());
            }
            return result;
        }

        template<typename T>
        std::optional<T> ReadOptional()
        {
            if (m_reader.ReadBool())
            {
                return Read<T>();
            }
            return std::nullopt;
        }

        // =====================================================================
        // Generic Read
        // =====================================================================

        template<typename T>
        T Read()
        {
            if constexpr (std::is_same_v<T, bool>) return ReadBool();
            else if constexpr (std::is_same_v<T, int8>) return ReadInt8();
            else if constexpr (std::is_same_v<T, uint8>) return ReadUInt8();
            else if constexpr (std::is_same_v<T, int16>) return ReadInt16();
            else if constexpr (std::is_same_v<T, uint16>) return ReadUInt16();
            else if constexpr (std::is_same_v<T, int32>) return ReadInt32();
            else if constexpr (std::is_same_v<T, uint32>) return ReadUInt32();
            else if constexpr (std::is_same_v<T, int64>) return ReadInt64();
            else if constexpr (std::is_same_v<T, uint64>) return ReadUInt64();
            else if constexpr (std::is_same_v<T, float32>) return ReadFloat32();
            else if constexpr (std::is_same_v<T, float64>) return ReadFloat64();
            else if constexpr (std::is_same_v<T, std::string>) return ReadString();
            else if constexpr (std::is_same_v<T, Vec2>) return ReadVec2();
            else if constexpr (std::is_same_v<T, Vec3>) return ReadVec3();
            else if constexpr (std::is_same_v<T, Vec4>) return ReadVec4();
            else if constexpr (std::is_same_v<T, Quat>) return ReadQuat();
            else if constexpr (std::is_same_v<T, Mat4>) return ReadMat4();
            else if constexpr (std::is_enum_v<T>)
            {
                return static_cast<T>(ReadUInt32());
            }
            else
            {
                static_assert(sizeof(T) == 0, "Type is not network serializable");
            }
        }

        // =====================================================================
        // State
        // =====================================================================

        bool HasOverflowed() const { return m_reader.HasOverflowed(); }
        uint32 GetRemainingBits() const { return m_reader.GetRemainingBits(); }
        BitReader& GetBitReader() { return m_reader; }

    private:
        BitReader& m_reader;
    };

    /**
     * @brief High-level serializer for writing network data
     */
    class NetworkWriter
    {
    public:
        explicit NetworkWriter(BitWriter& writer)
            : m_writer(writer)
        {
        }

        // =====================================================================
        // Basic Types
        // =====================================================================

        void WriteBool(bool value) { m_writer.WriteBool(value); }
        void WriteInt8(int8 value) { m_writer.WriteInt8(value); }
        void WriteUInt8(uint8 value) { m_writer.WriteUInt8(value); }
        void WriteInt16(int16 value) { m_writer.WriteInt16(value); }
        void WriteUInt16(uint16 value) { m_writer.WriteUInt16(value); }
        void WriteInt32(int32 value) { m_writer.WriteInt32(value); }
        void WriteUInt32(uint32 value) { m_writer.WriteUInt32(value); }
        void WriteInt64(int64 value) { m_writer.WriteInt64(value); }
        void WriteUInt64(uint64 value) { m_writer.WriteUInt64(value); }
        void WriteFloat32(float32 value) { m_writer.WriteFloat32(value); }
        void WriteFloat64(float64 value) { m_writer.WriteFloat64(value); }
        void WriteString(std::string_view value) { m_writer.WriteString(value); }

        // =====================================================================
        // Math Types
        // =====================================================================

        void WriteVec2(const Vec2& v)
        {
            m_writer.WriteFloat32(v.x);
            m_writer.WriteFloat32(v.y);
        }

        void WriteVec3(const Vec3& v)
        {
            m_writer.WriteFloat32(v.x);
            m_writer.WriteFloat32(v.y);
            m_writer.WriteFloat32(v.z);
        }

        void WriteVec4(const Vec4& v)
        {
            m_writer.WriteFloat32(v.x);
            m_writer.WriteFloat32(v.y);
            m_writer.WriteFloat32(v.z);
            m_writer.WriteFloat32(v.w);
        }

        void WriteQuat(const Quat& q)
        {
            m_writer.WriteFloat32(q.x);
            m_writer.WriteFloat32(q.y);
            m_writer.WriteFloat32(q.z);
            m_writer.WriteFloat32(q.w);
        }

        /**
         * @brief Write a compressed quaternion (smallest-three encoding)
         * Reduces from 128 bits to 32 bits with minimal precision loss
         */
        void WriteCompressedQuat(const Quat& qIn)
        {
            Quat q = glm::normalize(qIn);
            
            // Find the largest component
            float32 absX = std::abs(q.x);
            float32 absY = std::abs(q.y);
            float32 absZ = std::abs(q.z);
            float32 absW = std::abs(q.w);
            
            uint32 largestIndex = 0;
            float32 largestValue = absX;
            
            if (absY > largestValue) { largestIndex = 1; largestValue = absY; }
            if (absZ > largestValue) { largestIndex = 2; largestValue = absZ; }
            if (absW > largestValue) { largestIndex = 3; largestValue = absW; }
            
            // Ensure the largest component is positive
            float32 sign = 1.0f;
            switch (largestIndex)
            {
                case 0: sign = (q.x < 0) ? -1.0f : 1.0f; break;
                case 1: sign = (q.y < 0) ? -1.0f : 1.0f; break;
                case 2: sign = (q.z < 0) ? -1.0f : 1.0f; break;
                case 3: sign = (q.w < 0) ? -1.0f : 1.0f; break;
            }
            q = q * sign;
            
            // Write which component was largest
            m_writer.WriteBits(largestIndex, 2);
            
            // Write the three smallest components
            constexpr float32 kScale = 1.0f / 0.70710678118f; // sqrt(2)
            float32 a, b, c;
            
            switch (largestIndex)
            {
                case 0: a = q.y; b = q.z; c = q.w; break;
                case 1: a = q.x; b = q.z; c = q.w; break;
                case 2: a = q.x; b = q.y; c = q.w; break;
                case 3: a = q.x; b = q.y; c = q.z; break;
                default: a = b = c = 0; break;
            }
            
            // Normalize to [0, 1] range and write
            m_writer.WriteNormalizedFloat((a * kScale + 1.0f) * 0.5f, 10);
            m_writer.WriteNormalizedFloat((b * kScale + 1.0f) * 0.5f, 10);
            m_writer.WriteNormalizedFloat((c * kScale + 1.0f) * 0.5f, 10);
        }

        /**
         * @brief Write a compressed Vec3 position
         */
        void WriteCompressedPosition(const Vec3& v, Vec3 min, Vec3 max, uint32 bitsPerComponent)
        {
            m_writer.WriteRangedFloat(v.x, min.x, max.x, bitsPerComponent);
            m_writer.WriteRangedFloat(v.y, min.y, max.y, bitsPerComponent);
            m_writer.WriteRangedFloat(v.z, min.z, max.z, bitsPerComponent);
        }

        void WriteMat4(const Mat4& m)
        {
            for (int col = 0; col < 4; ++col)
            {
                for (int row = 0; row < 4; ++row)
                {
                    m_writer.WriteFloat32(m[col][row]);
                }
            }
        }

        // =====================================================================
        // Containers
        // =====================================================================

        template<typename T>
        void WriteVector(const std::vector<T>& vec)
        {
            m_writer.WriteVarInt(static_cast<uint32>(vec.size()));
            for (const auto& item : vec)
            {
                Write<T>(item);
            }
        }

        template<typename T>
        void WriteOptional(const std::optional<T>& opt)
        {
            m_writer.WriteBool(opt.has_value());
            if (opt.has_value())
            {
                Write<T>(*opt);
            }
        }

        // =====================================================================
        // Generic Write
        // =====================================================================

        template<typename T>
        void Write(const T& value)
        {
            if constexpr (std::is_same_v<T, bool>) WriteBool(value);
            else if constexpr (std::is_same_v<T, int8>) WriteInt8(value);
            else if constexpr (std::is_same_v<T, uint8>) WriteUInt8(value);
            else if constexpr (std::is_same_v<T, int16>) WriteInt16(value);
            else if constexpr (std::is_same_v<T, uint16>) WriteUInt16(value);
            else if constexpr (std::is_same_v<T, int32>) WriteInt32(value);
            else if constexpr (std::is_same_v<T, uint32>) WriteUInt32(value);
            else if constexpr (std::is_same_v<T, int64>) WriteInt64(value);
            else if constexpr (std::is_same_v<T, uint64>) WriteUInt64(value);
            else if constexpr (std::is_same_v<T, float32>) WriteFloat32(value);
            else if constexpr (std::is_same_v<T, float64>) WriteFloat64(value);
            else if constexpr (std::is_same_v<T, std::string>) WriteString(value);
            else if constexpr (std::is_same_v<std::decay_t<T>, Vec2>) WriteVec2(value);
            else if constexpr (std::is_same_v<std::decay_t<T>, Vec3>) WriteVec3(value);
            else if constexpr (std::is_same_v<std::decay_t<T>, Vec4>) WriteVec4(value);
            else if constexpr (std::is_same_v<std::decay_t<T>, Quat>) WriteQuat(value);
            else if constexpr (std::is_same_v<std::decay_t<T>, Mat4>) WriteMat4(value);
            else if constexpr (std::is_enum_v<T>)
            {
                WriteUInt32(static_cast<uint32>(value));
            }
            else
            {
                static_assert(sizeof(T) == 0, "Type is not network serializable");
            }
        }

        // =====================================================================
        // State
        // =====================================================================

        bool HasOverflowed() const { return m_writer.HasOverflowed(); }
        uint32 GetBytesWritten() const { return m_writer.GetBytesWritten(); }
        BitWriter& GetBitWriter() { return m_writer; }

    private:
        BitWriter& m_writer;
    };

    // =========================================================================
    // Serializable Interface
    // =========================================================================

    /**
     * @brief Interface for objects that can be serialized over the network
     */
    class INetworkSerializable
    {
    public:
        virtual ~INetworkSerializable() = default;

        /**
         * @brief Serialize object to network stream
         */
        virtual void Serialize(NetworkWriter& writer) const = 0;

        /**
         * @brief Deserialize object from network stream
         */
        virtual void Deserialize(NetworkReader& reader) = 0;

        /**
         * @brief Get serialization version for compatibility
         */
        virtual uint16 GetSerializationVersion() const { return 1; }
    };

    /**
     * @brief Helper macro for simple serializable structs
     * 
     * Usage:
     * @code
     * struct PlayerState : INetworkSerializable
     * {
     *     Vec3 position;
     *     Quat rotation;
     *     float32 health;
     *     
     *     RVX_NET_SERIALIZE(position, rotation, health)
     * };
     * @endcode
     */
    #define RVX_NET_SERIALIZE(...) \
        void Serialize(::RVX::Net::NetworkWriter& writer) const override \
        { \
            RVX_NET_SERIALIZE_IMPL(writer, __VA_ARGS__) \
        } \
        void Deserialize(::RVX::Net::NetworkReader& reader) override \
        { \
            RVX_NET_DESERIALIZE_IMPL(reader, __VA_ARGS__) \
        }

    // Implementation helpers for the macro
    #define RVX_NET_SERIALIZE_EACH(writer, x) writer.Write(x);
    #define RVX_NET_DESERIALIZE_EACH(reader, x) x = reader.Read<decltype(x)>();

    #define RVX_NET_SERIALIZE_IMPL(writer, ...) \
        (void(RVX_NET_SERIALIZE_EACH(writer, __VA_ARGS__)), ...)

    #define RVX_NET_DESERIALIZE_IMPL(reader, ...) \
        (void(RVX_NET_DESERIALIZE_EACH(reader, __VA_ARGS__)), ...)

} // namespace RVX::Net
